/*
* AINodeStorage.cpp, part of VCMI engine
*
* Authors: listed in file AUTHORS in main folder
*
* License: GNU General Public License v2.0 or later
* Full text of license available in license.txt file, in main folder
*
*/
#include "StdInc.h"
#include "AINodeStorage.h"
#include "Actions/TownPortalAction.h"
#include "../Goals/Goals.h"
#include "../VCAI.h"
#include "../Engine/Nullkiller.h"
#include "../../../CCallback.h"
#include "../../../lib/mapping/CMap.h"
#include "../../../lib/mapObjects/MapObjects.h"
#include "../../../lib/PathfinderUtil.h"
#include "../../../lib/CPlayerState.h"

std::shared_ptr<boost::multi_array<AIPathNode, 5>> AISharedStorage::shared;

AISharedStorage::AISharedStorage(int3 sizes)
{
	if(!shared){
		shared.reset(new boost::multi_array<AIPathNode, 5>(
			boost::extents[sizes.x][sizes.y][sizes.z][EPathfindingLayer::NUM_LAYERS][AINodeStorage::NUM_CHAINS]));
	}

	nodes = shared;
}

AISharedStorage::~AISharedStorage()
{
	nodes.reset();
	if(shared && shared.use_count() == 1)
	{
		shared.reset();
	}
}

AINodeStorage::AINodeStorage(const Nullkiller * ai, const int3 & Sizes)
	: sizes(Sizes), ai(ai), cb(ai->cb.get()), nodes(Sizes)
{
	dangerEvaluator.reset(new FuzzyHelper(ai));
}

AINodeStorage::~AINodeStorage() = default;

void AINodeStorage::initialize(const PathfinderOptions & options, const CGameState * gs)
{
	if(heroChainPass)
		return;

	//TODO: fix this code duplication with NodeStorage::initialize, problem is to keep `resetTile` inline
	int3 pos;
	const PlayerColor player = playerID;
	const PlayerColor fowPlayer = ai->playerID;
	const int3 sizes = gs->getMapSize();
	const auto & fow = static_cast<const CGameInfoCallback *>(gs)->getPlayerTeam(fowPlayer)->fogOfWarMap;

	//make 200% sure that these are loop invariants (also a bit shorter code), let compiler do the rest(loop unswitching)
	const bool useFlying = options.useFlying;
	const bool useWaterWalking = options.useWaterWalking;

	for(pos.x=0; pos.x < sizes.x; ++pos.x)
	{
		for(pos.y=0; pos.y < sizes.y; ++pos.y)
		{
			for(pos.z=0; pos.z < sizes.z; ++pos.z)
			{
				const TerrainTile * tile = &gs->map->getTile(pos);
				switch(tile->terType)
				{
				case ETerrainType::ROCK:
					break;

				case ETerrainType::WATER:
					resetTile(pos, ELayer::SAIL, PathfinderUtil::evaluateAccessibility<ELayer::SAIL>(pos, tile, fow, player, gs));
					if(useFlying)
						resetTile(pos, ELayer::AIR, PathfinderUtil::evaluateAccessibility<ELayer::AIR>(pos, tile, fow, player, gs));
					if(useWaterWalking)
						resetTile(pos, ELayer::WATER, PathfinderUtil::evaluateAccessibility<ELayer::WATER>(pos, tile, fow, player, gs));
					break;

				default:
					resetTile(pos, ELayer::LAND, PathfinderUtil::evaluateAccessibility<ELayer::LAND>(pos, tile, fow, player, gs));
					if(useFlying)
						resetTile(pos, ELayer::AIR, PathfinderUtil::evaluateAccessibility<ELayer::AIR>(pos, tile, fow, player, gs));
					break;
				}
			}
		}
	}
}

void AINodeStorage::clear()
{
	actors.clear();
	heroChainPass = EHeroChainPass::INITIAL;
	heroChainTurn = 0;
	heroChainMaxTurns = 1;
	scoutTurnDistanceLimit = 255;
}

const AIPathNode * AINodeStorage::getAINode(const CGPathNode * node) const
{
	return static_cast<const AIPathNode *>(node);
}

void AINodeStorage::updateAINode(CGPathNode * node, std::function<void(AIPathNode *)> updater)
{
	auto aiNode = static_cast<AIPathNode *>(node);

	updater(aiNode);
}

boost::optional<AIPathNode *> AINodeStorage::getOrCreateNode(
	const int3 & pos, 
	const EPathfindingLayer layer, 
	const ChainActor * actor)
{
	for(AIPathNode & node : nodes.get(pos, layer))
	{
		if(node.actor == actor)
		{
			return &node;
		}

		if(!node.actor)
		{
			node.actor = actor;

			return &node;
		}
	}

	return boost::none;
}

std::vector<CGPathNode *> AINodeStorage::getInitialNodes()
{
	if(heroChainPass)
	{
		calculateTownPortalTeleportations(heroChain);

		return heroChain;
	}

	std::vector<CGPathNode *> initialNodes;

	for(auto actorPtr : actors)
	{
		ChainActor * actor = actorPtr.get();
		AIPathNode * initialNode =
			getOrCreateNode(actor->initialPosition, actor->layer, actor)
			.get();

		initialNode->turns = actor->initialTurn;
		initialNode->moveRemains = actor->initialMovement;
		initialNode->danger = 0;
		initialNode->cost = actor->initialTurn;
		initialNode->action = CGPathNode::ENodeAction::NORMAL;

		if(actor->isMovable)
		{
			initialNodes.push_back(initialNode);
		}
		else
		{
			initialNode->locked = true;
		}
	}

	calculateTownPortalTeleportations(initialNodes);

	return initialNodes;
}

void AINodeStorage::resetTile(const int3 & coord, EPathfindingLayer layer, CGPathNode::EAccessibility accessibility)
{
	for(AIPathNode & heroNode : nodes.get(coord, layer))
	{
		heroNode.actor = nullptr;
		heroNode.danger = 0;
		heroNode.manaCost = 0;
		heroNode.specialAction.reset();
		heroNode.armyLoss = 0;
		heroNode.chainOther = nullptr;
		heroNode.update(coord, layer, accessibility);
	}
}

void AINodeStorage::commit(CDestinationNodeInfo & destination, const PathNodeInfo & source)
{
	const AIPathNode * srcNode = getAINode(source.node);

	updateAINode(destination.node, [&](AIPathNode * dstNode)
	{
		commit(dstNode, srcNode, destination.action, destination.turn, destination.movementLeft, destination.cost);

		if(srcNode->specialAction || srcNode->chainOther)
		{
			// there is some action on source tile which should be performed before we can bypass it
			destination.node->theNodeBefore = source.node;
		}

		if(dstNode->specialAction && dstNode->actor)
		{
			dstNode->specialAction->applyOnDestination(dstNode->actor->hero, destination, source, dstNode, srcNode);
		}
	});
}

void AINodeStorage::commit(
	AIPathNode * destination, 
	const AIPathNode * source, 
	CGPathNode::ENodeAction action, 
	int turn, 
	int movementLeft, 
	float cost) const
{
	destination->action = action;
	destination->cost = cost;
	destination->moveRemains = movementLeft;
	destination->turns = turn;
	destination->armyLoss = source->armyLoss;
	destination->manaCost = source->manaCost;
	destination->danger = source->danger;
	destination->theNodeBefore = source->theNodeBefore;
	destination->chainOther = nullptr;

#if PATHFINDER_TRACE_LEVEL >= 2
	logAi->trace(
		"Commited %s -> %s, cost: %f, turn: %s, mp: %d, hero: %s, mask: %x, army: %lld",
		source->coord.toString(),
		destination->coord.toString(),
		destination->cost,
		std::to_string(destination->turns),
		destination->moveRemains,
		destination->actor->toString(),
		destination->actor->chainMask,
		destination->actor->armyValue);
#endif
}

std::vector<CGPathNode *> AINodeStorage::calculateNeighbours(
	const PathNodeInfo & source,
	const PathfinderConfig * pathfinderConfig,
	const CPathfinderHelper * pathfinderHelper)
{
	std::vector<CGPathNode *> neighbours;
	neighbours.reserve(16);
	const AIPathNode * srcNode = getAINode(source.node);
	auto accessibleNeighbourTiles = pathfinderHelper->getNeighbourTiles(source);

	for(auto & neighbour : accessibleNeighbourTiles)
	{
		for(EPathfindingLayer i = EPathfindingLayer::LAND; i <= EPathfindingLayer::AIR; i.advance(1))
		{
			auto nextNode = getOrCreateNode(neighbour, i, srcNode->actor);

			if(!nextNode || nextNode.get()->accessible == CGPathNode::NOT_SET)
				continue;

			neighbours.push_back(nextNode.get());
		}
	}
	
	return neighbours;
}

bool AINodeStorage::increaseHeroChainTurnLimit()
{
	if(heroChainTurn >= heroChainMaxTurns)
		return false;

	heroChainTurn++;

	return true;
}

EPathfindingLayer phisycalLayers[2] = {EPathfindingLayer::LAND, EPathfindingLayer::SAIL};

bool AINodeStorage::calculateHeroChainFinal()
{
	heroChainPass = EHeroChainPass::FINAL;
	heroChain.resize(0);

	for(auto layer : phisycalLayers)
	{
		foreach_tile_pos([&](const int3 & pos)
		{
			auto chains = nodes.get(pos, layer);

			for(AIPathNode & node : chains)
			{
				if(node.turns > heroChainTurn
					&& !node.locked
					&& node.action != CGPathNode::ENodeAction::UNKNOWN
					&& node.actor->actorExchangeCount > 1
					&& !hasBetterChain(&node, &node, chains))
				{
					heroChain.push_back(&node);
				}
			}
		});
	}
	
	return heroChain.size();
}

bool AINodeStorage::calculateHeroChain()
{
	heroChainPass = EHeroChainPass::CHAIN;
	heroChain.resize(0);

	std::vector<AIPathNode *> existingChains;
	std::vector<ExchangeCandidate> newChains;

	existingChains.reserve(NUM_CHAINS);
	newChains.reserve(NUM_CHAINS);

	for(auto layer : phisycalLayers)
	{
		foreach_tile_pos([&](const int3 & pos)
		{
			auto chains = nodes.get(pos, layer);

			existingChains.resize(0);
			newChains.resize(0);

			for(AIPathNode & node : chains)
			{
				if(node.turns <= heroChainTurn && node.action != CGPathNode::ENodeAction::UNKNOWN)
					existingChains.push_back(&node);
			}

			for(AIPathNode * node : existingChains)
			{
				if(node->actor->isMovable)
				{
					calculateHeroChain(node, existingChains, newChains);
				}
			}

			cleanupInefectiveChains(newChains);
			addHeroChain(newChains);
		});
	}

	return heroChain.size();
}

bool AINodeStorage::selectFirstActor()
{
	if(!actors.size())
		return false;

	auto strongest = *vstd::maxElementByFun(actors, [](std::shared_ptr<ChainActor> actor) -> uint64_t
	{
		return actor->armyValue;
	});

	chainMask = strongest->chainMask;

	return true;
}

bool AINodeStorage::selectNextActor()
{
	auto currentActor = std::find_if(actors.begin(), actors.end(), [&](std::shared_ptr<ChainActor> actor)-> bool
	{
		return actor->chainMask == chainMask;
	});

	auto nextActor = actors.end();

	for(auto actor = actors.begin(); actor != actors.end(); actor++)
	{
		if(actor->get()->armyValue > currentActor->get()->armyValue
			|| actor->get()->armyValue == currentActor->get()->armyValue && actor <= currentActor)
		{
			continue;
		}

		if(nextActor == actors.end()
			|| actor->get()->armyValue > nextActor->get()->armyValue)
		{
			nextActor = actor;
		}
	}

	if(nextActor != actors.end())
	{
		chainMask = nextActor->get()->chainMask;

		return true;
	}

	return false;
}

void AINodeStorage::cleanupInefectiveChains(std::vector<ExchangeCandidate> & result) const
{
	vstd::erase_if(result, [&](const ExchangeCandidate & chainInfo) -> bool
	{
		auto pos = chainInfo.coord;
		auto chains = nodes.get(pos, EPathfindingLayer::LAND);

		return hasBetterChain(chainInfo.carrierParent, &chainInfo, chains)
			|| hasBetterChain(chainInfo.carrierParent, &chainInfo, result);
	});
}

void AINodeStorage::calculateHeroChain(
	AIPathNode * srcNode, 
	const std::vector<AIPathNode *> & variants, 
	std::vector<ExchangeCandidate> & result) const
{
	for(AIPathNode * node : variants)
	{
		if(node == srcNode || !node->actor)
			continue;

		if((node->actor->chainMask & chainMask) == 0 && (srcNode->actor->chainMask & chainMask) == 0)
			continue;

		if(node->action == CGPathNode::ENodeAction::BATTLE
			|| node->action == CGPathNode::ENodeAction::TELEPORT_BATTLE
			|| node->action == CGPathNode::ENodeAction::TELEPORT_NORMAL
			|| node->action == CGPathNode::ENodeAction::TELEPORT_BLOCKING_VISIT)
		{
			continue;
		}

		if(node->turns > heroChainTurn 
			|| (node->action == CGPathNode::ENodeAction::UNKNOWN && node->actor->hero)
			|| (node->actor->chainMask & srcNode->actor->chainMask) != 0)
		{
#if PATHFINDER_TRACE_LEVEL >= 2
			logAi->trace(
				"Skip exchange %s[%x] -> %s[%x] at %s because of %s",
				node->actor->toString(),
				node->actor->chainMask,
				srcNode->actor->toString(),
				srcNode->actor->chainMask,
				srcNode->coord.toString(),
				(node->turns > heroChainTurn 
					? "turn limit" 
					: (node->action == CGPathNode::ENodeAction::UNKNOWN && node->actor->hero)
						? "action unknown"
						: "chain mask"));
#endif
			continue;
		}

#if PATHFINDER_TRACE_LEVEL >= 2
		logAi->trace(
			"Thy exchange %s[%x] -> %s[%x] at %s",
			node->actor->toString(),
			node->actor->chainMask,
			srcNode->actor->toString(),
			srcNode->actor->chainMask,
			srcNode->coord.toString());
#endif

		calculateHeroChain(srcNode, node, result);
	}
}

void AINodeStorage::calculateHeroChain(
	AIPathNode * carrier, 
	AIPathNode * other, 
	std::vector<ExchangeCandidate> & result) const
{	
	if(carrier->armyLoss < carrier->actor->armyValue
		&& (carrier->action != CGPathNode::BATTLE || (carrier->actor->allowBattle && carrier->specialAction))
		&& carrier->action != CGPathNode::BLOCKING_VISIT
		&& (other->armyLoss == 0 || other->armyLoss < other->actor->armyValue)
		&& carrier->actor->canExchange(other->actor))
	{
#if PATHFINDER_TRACE_LEVEL >= 2
		logAi->trace(
			"Exchange allowed %s[%x] -> %s[%x] at %s",
			other->actor->toString(),
			other->actor->chainMask,
			carrier->actor->toString(),
			carrier->actor->chainMask,
			carrier->coord.toString());
#endif

		if(other->actor->isMovable)
		{
			bool hasLessMp = carrier->turns > other->turns || (carrier->turns == other->turns && carrier->moveRemains < other->moveRemains);
			bool hasLessExperience = carrier->actor->hero->exp < other->actor->hero->exp;

			if(hasLessMp && hasLessExperience)
			{
#if PATHFINDER_TRACE_LEVEL >= 2
				logAi->trace("Exchange at %s is ineficient. Blocked.", carrier->coord.toString());
#endif
				return;
			}
		}

		auto newActor = carrier->actor->exchange(other->actor);
		
		result.push_back(calculateExchange(newActor, carrier, other));
	}
}

void AINodeStorage::addHeroChain(const std::vector<ExchangeCandidate> & result)
{
	for(const ExchangeCandidate & chainInfo : result)
	{
		auto carrier = chainInfo.carrierParent;
		auto newActor = chainInfo.actor;
		auto other = chainInfo.otherParent;
		auto chainNodeOptional = getOrCreateNode(carrier->coord, carrier->layer, newActor);

		if(!chainNodeOptional)
		{
#if PATHFINDER_TRACE_LEVEL >= 2
			logAi->trace("Exchange at %s can not allocate node. Blocked.", carrier->coord.toString());
#endif
			continue;
		}

		auto exchangeNode = chainNodeOptional.get();

		if(exchangeNode->action != CGPathNode::ENodeAction::UNKNOWN)
		{
#if PATHFINDER_TRACE_LEVEL >= 2
			logAi->trace("Exchange at %s node is already in use. Blocked.", carrier->coord.toString());
#endif
			continue;
		}
		
		if(exchangeNode->turns != 0xFF && exchangeNode->cost < chainInfo.cost)
		{
#if PATHFINDER_TRACE_LEVEL >= 2
			logAi->trace(
				"Exchange at %s is is not effective enough. %f < %f", 
				exchangeNode->coord.toString(), 
				exchangeNode->cost, 
				chainInfo.cost);
#endif
			continue;
		}

		commit(exchangeNode, carrier, carrier->action, chainInfo.turns, chainInfo.moveRemains, chainInfo.cost);

		if(carrier->specialAction || carrier->chainOther)
		{
			// there is some action on source tile which should be performed before we can bypass it
			exchangeNode->theNodeBefore = carrier;
		}

		exchangeNode->chainOther = other;
		exchangeNode->armyLoss = chainInfo.armyLoss;

#if PATHFINDER_TRACE_LEVEL >= 2
		logAi->trace(
			"Chain accepted at %s %s -> %s, mask %x, cost %f, turn: %s, mp: %d, army %i", 
			exchangeNode->coord.toString(), 
			other->actor->toString(), 
			exchangeNode->actor->toString(),
			exchangeNode->actor->chainMask,
			exchangeNode->cost,
			std::to_string(exchangeNode->turns),
			exchangeNode->moveRemains,
			exchangeNode->actor->armyValue);
#endif
		heroChain.push_back(exchangeNode);
	}
}

ExchangeCandidate AINodeStorage::calculateExchange(
	ChainActor * exchangeActor, 
	AIPathNode * carrierParentNode, 
	AIPathNode * otherParentNode) const
{
	ExchangeCandidate candidate;
	
	candidate.layer = carrierParentNode->layer;
	candidate.coord = carrierParentNode->coord;
	candidate.carrierParent = carrierParentNode;
	candidate.otherParent = otherParentNode;
	candidate.actor = exchangeActor;
	candidate.armyLoss = carrierParentNode->armyLoss + otherParentNode->armyLoss;
	candidate.turns = carrierParentNode->turns;
	candidate.cost = carrierParentNode->cost + otherParentNode->cost / 1000.0;
	candidate.moveRemains = carrierParentNode->moveRemains;

	if(carrierParentNode->turns < otherParentNode->turns)
	{
		int moveRemains = exchangeActor->hero->maxMovePoints(carrierParentNode->layer);
		float waitingCost = otherParentNode->turns - carrierParentNode->turns - 1
			+ carrierParentNode->moveRemains / (float)moveRemains;

		candidate.turns = otherParentNode->turns;
		candidate.cost += waitingCost;
		candidate.moveRemains = moveRemains;
	}

	return candidate;
}

const CGHeroInstance * AINodeStorage::getHero(const CGPathNode * node) const
{
	auto aiNode = getAINode(node);

	return aiNode->actor->hero;
}

const std::set<const CGHeroInstance *> AINodeStorage::getAllHeroes() const
{
	std::set<const CGHeroInstance *> heroes;

	for(auto actor : actors)
	{
		if(actor->hero)
			heroes.insert(actor->hero);
	}

	return heroes;
}

bool AINodeStorage::isDistanceLimitReached(const PathNodeInfo & source, CDestinationNodeInfo & destination) const
{
	if(heroChainPass == EHeroChainPass::CHAIN && destination.node->turns > heroChainTurn)
	{
		return true;
	}
	
	auto aiNode = getAINode(destination.node);
	
	if(heroChainPass == EHeroChainPass::FINAL)
	{
		if(aiNode->actor->heroRole == HeroRole::SCOUT && destination.node->turns > scoutTurnDistanceLimit)
			return true;
	}
	else if(heroChainPass == EHeroChainPass::INITIAL)
	{
		if(aiNode->actor->heroRole == HeroRole::SCOUT && destination.node->turns > scoutTurnDistanceLimit)
			return true;
	}

	return false;
}

void AINodeStorage::setHeroes(std::map<const CGHeroInstance *, HeroRole> heroes)
{
	playerID = ai->playerID;

	for(auto & hero : heroes)
	{
		uint64_t mask = 1 << actors.size();
		auto actor = std::make_shared<HeroActor>(hero.first, hero.second, mask, ai);

		if(actor->hero->tempOwner != ai->playerID)
		{
			bool onLand = !actor->hero->boat;
			actor->initialMovement = actor->hero->maxMovePoints(onLand);
		}

		playerID = actor->hero->tempOwner;

		actors.push_back(actor);
	}
}

void AINodeStorage::setTownsAndDwellings(
	const std::vector<const CGTownInstance *> & towns,
	const std::set<const CGObjectInstance *> & visitableObjs)
{
	for(auto town : towns)
	{
		uint64_t mask = 1 << actors.size();

		// TODO: investigate logix of second condition || ai->nullkiller->getHeroLockedReason(town->garrisonHero) != HeroLockedReason::DEFENCE
		// check defence imrove
		if(!town->garrisonHero)
		{
			actors.push_back(std::make_shared<TownGarrisonActor>(town, mask));
		}
	}

	/*auto dayOfWeek = cb->getDate(Date::DAY_OF_WEEK);
	auto waitForGrowth = dayOfWeek > 4;*/

	for(auto obj: visitableObjs)
	{
		if(obj->ID == Obj::HILL_FORT)
		{
			uint64_t mask = 1 << actors.size();

			actors.push_back(std::make_shared<HillFortActor>(obj, mask));
		}
		/*const CGDwelling * dwelling = dynamic_cast<const CGDwelling *>(obj);

		if(dwelling)
		{
			uint64_t mask = 1 << actors.size();
			auto dwellingActor = std::make_shared<DwellingActor>(dwelling, mask, false, dayOfWeek);

			if(dwellingActor->creatureSet->getArmyStrength())
			{
				actors.push_back(dwellingActor);
			}

			if(waitForGrowth)
			{
				mask = 1 << actors.size();
				dwellingActor = std::make_shared<DwellingActor>(dwelling, mask, waitForGrowth, dayOfWeek);

				if(dwellingActor->creatureSet->getArmyStrength())
				{
					actors.push_back(dwellingActor);
				}
			}
		}*/
	}
}

std::vector<CGPathNode *> AINodeStorage::calculateTeleportations(
	const PathNodeInfo & source,
	const PathfinderConfig * pathfinderConfig,
	const CPathfinderHelper * pathfinderHelper)
{
	std::vector<CGPathNode *> neighbours;

	if(source.isNodeObjectVisitable())
	{
		auto accessibleExits = pathfinderHelper->getTeleportExits(source);
		auto srcNode = getAINode(source.node);

		for(auto & neighbour : accessibleExits)
		{
			auto node = getOrCreateNode(neighbour, source.node->layer, srcNode->actor);

			if(!node)
				continue;

			neighbours.push_back(node.get());
		}
	}

	return neighbours;
}

struct TowmPortalFinder
{
	const std::vector<CGPathNode *> & initialNodes;
	SecSkillLevel::SecSkillLevel townPortalSkillLevel;
	uint64_t movementNeeded;
	const ChainActor * actor;
	const CGHeroInstance * hero;
	std::vector<const CGTownInstance *> targetTowns;
	AINodeStorage * nodeStorage;

	SpellID spellID;
	const CSpell * townPortal;

	TowmPortalFinder(
		const ChainActor * actor,
		const std::vector<CGPathNode *> & initialNodes,
		std::vector<const CGTownInstance *> targetTowns,
		AINodeStorage * nodeStorage)
		:actor(actor), initialNodes(initialNodes), hero(actor->hero),
		targetTowns(targetTowns), nodeStorage(nodeStorage)
	{
		spellID = SpellID::TOWN_PORTAL;
		townPortal = spellID.toSpell();

		// TODO: Copy/Paste from TownPortalMechanics
		townPortalSkillLevel = SecSkillLevel::SecSkillLevel(hero->getSpellSchoolLevel(townPortal));
		movementNeeded = GameConstants::BASE_MOVEMENT_COST * (townPortalSkillLevel >= SecSkillLevel::EXPERT ? 2 : 3);
	}

	bool actorCanCastTownPortal()
	{
		return hero->canCastThisSpell(townPortal) && hero->mana >= hero->getSpellCost(townPortal);
	}

	CGPathNode * getBestInitialNodeForTownPortal(const CGTownInstance * targetTown)
	{
		CGPathNode * bestNode = nullptr;

		for(CGPathNode * node : initialNodes)
		{
			auto aiNode = nodeStorage->getAINode(node);

			if(aiNode->actor->baseActor != actor
				|| node->layer != EPathfindingLayer::LAND
				|| node->moveRemains < movementNeeded)
			{
				continue;
			}

			if(townPortalSkillLevel < SecSkillLevel::ADVANCED)
			{
				const CGTownInstance * nearestTown = *vstd::minElementByFun(targetTowns, [&](const CGTownInstance * t) -> int
				{
					return node->coord.dist2dSQ(t->visitablePos());
				});

				if(targetTown != nearestTown)
					continue;
			}

			if(!bestNode || bestNode->cost > node->cost)
				bestNode = node;
		}

		return bestNode;
	}

	boost::optional<AIPathNode *> createTownPortalNode(const CGTownInstance * targetTown)
	{
		auto bestNode = getBestInitialNodeForTownPortal(targetTown);

		if(!bestNode)
			return boost::none;

		auto nodeOptional = nodeStorage->getOrCreateNode(targetTown->visitablePos(), EPathfindingLayer::LAND, actor->castActor);

		if(!nodeOptional)
			return boost::none;

		AIPathNode * node = nodeOptional.get();
		float movementCost = (float)movementNeeded / (float)hero->maxMovePoints(EPathfindingLayer::LAND);

		movementCost += bestNode->cost;

		if(node->action == CGPathNode::UNKNOWN || node->cost > movementCost)
		{
			nodeStorage->commit(
				node,
				nodeStorage->getAINode(bestNode),
				CGPathNode::TELEPORT_NORMAL,
				bestNode->turns,
				bestNode->moveRemains - movementNeeded,
				movementCost);

			node->theNodeBefore = bestNode;
			node->specialAction.reset(new AIPathfinding::TownPortalAction(targetTown));
		}

		return nodeOptional;
	}
};

void AINodeStorage::calculateTownPortalTeleportations(std::vector<CGPathNode *> & initialNodes)
{
	std::set<const ChainActor *> actorsOfInitial;

	for(const CGPathNode * node : initialNodes)
	{
		auto aiNode = getAINode(node);

		actorsOfInitial.insert(aiNode->actor->baseActor);
	}

	std::map<const CGHeroInstance *, int> maskMap;

	for(std::shared_ptr<ChainActor> basicActor : actors)
	{
		if(basicActor->hero)
			maskMap[basicActor->hero] = basicActor->chainMask;
	}

	for(const ChainActor * actor : actorsOfInitial)
	{
		if(!actor->hero)
			continue;

		auto towns = cb->getTownsInfo(false);

		vstd::erase_if(towns, [&](const CGTownInstance * t) -> bool
		{
			return cb->getPlayerRelations(actor->hero->tempOwner, t->tempOwner) == PlayerRelations::ENEMIES;
		});

		if(!towns.size())
		{
			return; // no towns no need to run loop further
		}

		TowmPortalFinder townPortalFinder(actor, initialNodes, towns, this);

		if(townPortalFinder.actorCanCastTownPortal())
		{
			for(const CGTownInstance * targetTown : towns)
			{
				// TODO: allow to hide visiting hero in garrison
				if(targetTown->visitingHero)
				{
					auto basicMask = maskMap[targetTown->visitingHero.get()];
					bool heroIsInChain = (actor->chainMask & basicMask) != 0;
					bool sameActorInTown = actor->chainMask == basicMask;

					if(sameActorInTown || !heroIsInChain)
						continue;
				}

				auto nodeOptional = townPortalFinder.createTownPortalNode(targetTown);

				if(nodeOptional)
				{
#if PATHFINDER_TRACE_LEVEL >= 1
					logAi->trace("Adding town portal node at %s", targetTown->name);
#endif
					initialNodes.push_back(nodeOptional.get());
				}
			}
		}
	}
}

bool AINodeStorage::hasBetterChain(const PathNodeInfo & source, CDestinationNodeInfo & destination) const
{
	auto pos = destination.coord;
	auto chains = nodes.get(pos, EPathfindingLayer::LAND);

	return hasBetterChain(source.node, getAINode(destination.node), chains);
}

template<class NodeRange>
bool AINodeStorage::hasBetterChain(
	const CGPathNode * source, 
	const AIPathNode * candidateNode,
	const NodeRange & chains) const
{
	auto candidateActor = candidateNode->actor;

	for(const AIPathNode & node : chains)
	{
		auto sameNode = node.actor == candidateNode->actor;

		if(sameNode	|| node.action == CGPathNode::ENodeAction::UNKNOWN || !node.actor->hero)
		{
			continue;
		}

		if(node.danger <= candidateNode->danger && candidateNode->actor == node.actor->battleActor)
		{
			if(node.cost < candidateNode->cost)
			{
#if PATHFINDER_TRACE_LEVEL >= 2
				logAi->trace(
					"Block ineficient battle move %s->%s, hero: %s[%X], army %lld, mp diff: %i",
					source->coord.toString(),
					candidateNode->coord.toString(),
					candidateNode->actor->hero->name,
					candidateNode->actor->chainMask,
					candidateNode->actor->armyValue,
					node.moveRemains - candidateNode->moveRemains);
#endif
				return true;
			}
		}

		if(candidateActor->chainMask != node.actor->chainMask && heroChainPass != EHeroChainPass::FINAL)
			continue;

		auto nodeActor = node.actor;
		auto nodeArmyValue = nodeActor->armyValue - node.armyLoss;
		auto candidateArmyValue = candidateActor->armyValue - candidateNode->armyLoss;

		if(nodeArmyValue > candidateArmyValue
			&& node.cost <= candidateNode->cost)
		{
#if PATHFINDER_TRACE_LEVEL >= 2
			logAi->trace(
				"Block ineficient move because of stronger army %s->%s, hero: %s[%X], army %lld, mp diff: %i",
				source->coord.toString(),
				candidateNode->coord.toString(),
				candidateNode->actor->hero->name,
				candidateNode->actor->chainMask,
				candidateNode->actor->armyValue,
				node.moveRemains - candidateNode->moveRemains);
#endif
			return true;
		}

		if(heroChainPass == EHeroChainPass::FINAL)
		{
			if(nodeArmyValue == candidateArmyValue
				&& nodeActor->heroFightingStrength >= candidateActor->heroFightingStrength
				&& node.cost <= candidateNode->cost)
			{
				if(nodeActor->heroFightingStrength == candidateActor->heroFightingStrength
					&& node.cost == candidateNode->cost
					&& &node < candidateNode)
				{
					continue;
				}

#if AI_TRACE_LEVEL >= 2
				logAi->trace(
					"Block ineficient move because of stronger hero %s->%s, hero: %s[%X], army %lld, mp diff: %i",
					source->coord.toString(),
					candidateNode->coord.toString(),
					candidateNode->actor->hero->name,
					candidateNode->actor->chainMask,
					candidateNode->actor->armyValue,
					node.moveRemains - candidateNode->moveRemains);
#endif
				return true;
			}
		}
	}

	return false;
}

bool AINodeStorage::isTileAccessible(const HeroPtr & hero, const int3 & pos, const EPathfindingLayer layer) const
{
	auto chains = nodes.get(pos, layer);

	for(const AIPathNode & node : chains)
	{
		if(node.action != CGPathNode::ENodeAction::UNKNOWN 
			&& node.actor && node.actor->hero == hero.h)
		{
			return true;
		}
	}

	return false;
}

std::vector<AIPath> AINodeStorage::getChainInfo(const int3 & pos, bool isOnLand) const
{
	std::vector<AIPath> paths;

	paths.reserve(NUM_CHAINS / 4);

	auto chains = nodes.get(pos, isOnLand ? EPathfindingLayer::LAND : EPathfindingLayer::SAIL);

	for(const AIPathNode & node : chains)
	{
		if(node.action == CGPathNode::ENodeAction::UNKNOWN || !node.actor || !node.actor->hero)
		{
			continue;
		}

		AIPath path;

		path.targetHero = node.actor->hero;
		path.heroArmy = node.actor->creatureSet;
		path.armyLoss = node.armyLoss;
		path.targetObjectDanger = evaluateDanger(pos, path.targetHero, false);
		path.targetObjectArmyLoss = evaluateArmyLoss(path.targetHero, path.heroArmy->getArmyStrength(), path.targetObjectDanger);
		path.chainMask = node.actor->chainMask;
		path.exchangeCount = node.actor->actorExchangeCount;
		
		fillChainInfo(&node, path, -1);

		paths.push_back(path);
	}

	return paths;
}

void AINodeStorage::fillChainInfo(const AIPathNode * node, AIPath & path, int parentIndex) const
{
	while(node != nullptr)
	{
		if(!node->actor->hero)
			return;

		if(node->chainOther)
			fillChainInfo(node->chainOther, path, parentIndex);

		//if(node->actor->hero->visitablePos() != node->coord)
		{
			AIPathNodeInfo pathNode;
			pathNode.cost = node->cost;
			pathNode.targetHero = node->actor->hero;
			pathNode.chainMask = node->actor->chainMask;
			pathNode.specialAction = node->specialAction;
			pathNode.turns = node->turns;
			pathNode.danger = node->danger;
			pathNode.coord = node->coord;
			pathNode.parentIndex = parentIndex;
			pathNode.actionIsBlocked = false;

			if(pathNode.specialAction)
			{
				auto targetNode =node->theNodeBefore ?  getAINode(node->theNodeBefore) : node;

				pathNode.actionIsBlocked = !pathNode.specialAction->canAct(targetNode);
			}

			parentIndex = path.nodes.size();

			path.nodes.push_back(pathNode);
		}
		
		node = getAINode(node->theNodeBefore);
	}
}

AIPath::AIPath()
	: nodes({})
{
}

std::shared_ptr<const SpecialAction> AIPath::getFirstBlockedAction() const
{
	for(auto node = nodes.rbegin(); node != nodes.rend(); node++)
	{
		if(node->specialAction && node->actionIsBlocked)
			return node->specialAction;
	}

	return std::shared_ptr<const SpecialAction>();
}

int3 AIPath::firstTileToGet() const
{
	if(nodes.size())
	{
		return nodes.back().coord;
	}

	return int3(-1, -1, -1);
}

int3 AIPath::targetTile() const
{
	if(nodes.size())
	{
		return targetNode().coord;
	}

	return int3(-1, -1, -1);
}

const AIPathNodeInfo & AIPath::firstNode() const
{
	return nodes.back();
}

const AIPathNodeInfo & AIPath::targetNode() const
{
	auto & node = nodes.front();

	return targetHero == node.targetHero ? node : nodes.at(1);
}

uint64_t AIPath::getPathDanger() const
{
	if(nodes.empty())
		return 0;

	return targetNode().danger;
}

float AIPath::movementCost() const
{
	if(nodes.empty())
		return 0.0f;

	return targetNode().cost;
}

uint8_t AIPath::turn() const
{
	if(nodes.empty())
		return 0;

	return targetNode().turns;
}

uint64_t AIPath::getHeroStrength() const
{
	return targetHero->getFightingStrength() * heroArmy->getArmyStrength();
}

uint64_t AIPath::getTotalDanger() const
{
	uint64_t pathDanger = getPathDanger();
	uint64_t danger = pathDanger > targetObjectDanger ? pathDanger : targetObjectDanger;

	return danger;
}

bool AIPath::containsHero(const CGHeroInstance * hero) const
{
	if(targetHero == hero)
		return true;

	for(auto node : nodes)
	{
		if(node.targetHero == hero)
			return true;
	}

	return false;
}

uint64_t AIPath::getTotalArmyLoss() const
{
	return armyLoss + targetObjectArmyLoss;
}

std::string AIPath::toString() const
{
	std::stringstream str;

	str << targetHero->name << "[" << std::hex << chainMask << std::dec << "]" << ", turn " << (int)(turn()) << ": ";

	for(auto node : nodes)
		str << node.targetHero->name << "[" << std::hex << node.chainMask << std::dec << "]" << "->" << node.coord.toString() << "; ";

	return str.str();
}