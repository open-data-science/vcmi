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
#include "Actors.h"
#include "../Goals/VisitHero.h"
#include "../VCAI.h"
#include "../AIhelper.h"
#include "../../../CCallback.h"
#include "../../../lib/mapping/CMap.h"
#include "../../../lib/mapObjects/MapObjects.h"

class ExchangeAction : public ISpecialAction
{
private:
	const CGHeroInstance * target;
	const CGHeroInstance * source;

public:
	ExchangeAction(const CGHeroInstance * target, const CGHeroInstance * source)
		:target(target), source(source)
	{ }

	virtual Goals::TSubgoal whatToDo(const HeroPtr & hero) const override
	{
		return Goals::sptr(Goals::VisitHero(target->id.getNum()).sethero(hero));
	}
};

ChainActor::ChainActor(const CGHeroInstance * hero, uint64_t chainMask)
	:hero(hero), isMovable(true), chainMask(chainMask), creatureSet(hero),
	baseActor(this), carrierParent(nullptr), otherParent(nullptr)
{
	initialPosition = hero->visitablePos();
	layer = hero->boat ? EPathfindingLayer::SAIL : EPathfindingLayer::LAND;
	initialMovement = hero->movement;
	initialTurn = 0;
	armyValue = hero->getArmyStrength();
}

ChainActor::ChainActor(const ChainActor * carrier, const ChainActor * other, const CCreatureSet * heroArmy)
	:hero(carrier->hero), isMovable(true), creatureSet(heroArmy), chainMask(carrier->chainMask | other->chainMask),
	baseActor(this), carrierParent(carrier), otherParent(other)
{
	armyValue = heroArmy->getArmyStrength();
}

ChainActor::ChainActor(const CGObjectInstance * obj, const CCreatureSet * creatureSet, uint64_t chainMask, int initialTurn)
	:hero(nullptr), isMovable(false), creatureSet(creatureSet), chainMask(chainMask),
	baseActor(this), carrierParent(nullptr), otherParent(nullptr), initialTurn(initialTurn), initialMovement(0)
{
	initialPosition = obj->visitablePos();
	layer = EPathfindingLayer::LAND;
	armyValue = creatureSet->getArmyStrength();
}

HeroActor::HeroActor(const CGHeroInstance * hero, uint64_t chainMask, const VCAI * ai)
	:ChainActor(hero, chainMask)
{
	exchangeMap = new HeroExchangeMap(this, ai);
	setupSpecialActors();
}

HeroActor::HeroActor(
	const ChainActor * carrier, 
	const ChainActor * other, 
	const CCreatureSet * army, 
	const VCAI * ai)
	:ChainActor(carrier, other,	army)
{
	exchangeMap = new HeroExchangeMap(this, ai);
	setupSpecialActors();
}

void ChainActor::setBaseActor(HeroActor * base)
{
	baseActor = base;
	hero = base->hero;
	layer = base->layer;
	initialMovement = base->initialMovement;
	initialTurn = base->initialTurn;
	armyValue = base->armyValue;
	chainMask = base->chainMask;
	creatureSet = base->creatureSet;
	isMovable = base->isMovable;
}

void HeroActor::setupSpecialActors()
{
	auto allActors = std::vector<ChainActor *>{ this };

	for(ChainActor & specialActor : specialActors)
	{
		specialActor.setBaseActor(this);
		allActors.push_back(&specialActor);
	}

	for(int i = 0; i <= SPECIAL_ACTORS_COUNT; i++)
	{
		ChainActor * actor = allActors[i];

		actor->allowBattle = (i & 1) > 0;
		actor->allowSpellCast = (i & 2) > 0;
		actor->allowUseResources = (i & 4) > 0;
		actor->battleActor = allActors[i | 1];
		actor->castActor = allActors[i | 2];
		actor->resourceActor = allActors[i | 4];
	}
}

ChainActor * ChainActor::exchange(const ChainActor * specialActor, const ChainActor * other) const
{
	return baseActor->exchange(specialActor, other);
}

bool ChainActor::canExchange(const ChainActor * other) const
{
	return isMovable && baseActor->canExchange(other);
}

namespace vstd
{
	template <class M, class Key, class F>
	typename M::mapped_type & getOrCompute(M &m, Key const& k, F f)
	{
		typedef typename M::mapped_type V;

		std::pair<typename M::iterator, bool> r = m.insert(typename M::value_type(k, V()));
		V &v = r.first->second;

		if(r.second)
			f(v);

		return v;
	}
}

bool HeroActor::canExchange(const ChainActor * other) const
{
	return exchangeMap->canExchange(other);
}

bool HeroExchangeMap::canExchange(const ChainActor * other)
{
	return vstd::getOrCompute(canExchangeCache, other, [&](bool & result) {
		result = (actor->chainMask & other->chainMask) == 0;

		if(result)
		{
			uint64_t reinforcment = ai->ah->howManyReinforcementsCanGet(actor->creatureSet, other->creatureSet);

			result = reinforcment > actor->armyValue / 10 || reinforcment > 1000;
		}
	});
}

ChainActor * HeroActor::exchange(const ChainActor * specialActor, const ChainActor * other) const
{
	const ChainActor * otherBase = other->baseActor;
	HeroActor * result = exchangeMap->exchange(otherBase);

	if(specialActor == this)
		return result;

	int index = vstd::find_pos_if(specialActors, [specialActor](const ChainActor & actor) -> bool
	{
		return &actor == specialActor;
	});

	return &result->specialActors[index];
}

HeroActor * HeroExchangeMap::exchange(const ChainActor * other)
{
	HeroActor * result;

	if(vstd::contains(exchangeMap, other))
		result = exchangeMap.at(other);
	else 
	{
		// TODO: decide where to release this CCreatureSet and HeroActor. Probably custom ~ctor?
		CCreatureSet * newArmy = pickBestCreatures(actor->creatureSet, other->creatureSet);
		result = new HeroActor(actor, other, newArmy, ai);
		exchangeMap[other] = result;
	}

	return result;
}

CCreatureSet * HeroExchangeMap::pickBestCreatures(const CCreatureSet * army1, const CCreatureSet * army2) const
{
	CCreatureSet * target = new CCreatureSet();
	auto bestArmy = ai->ah->getBestArmy(army1, army2);

	for(auto & slotInfo : bestArmy)
	{
		auto targetSlot = target->getFreeSlot();

		target->addToSlot(targetSlot, slotInfo.creature->idNumber, TQuantity(slotInfo.count));
	}

	return target;
}

DwellingActor::DwellingActor(const CGDwelling * dwelling, uint64_t chainMask, bool waitForGrowth, int dayOfWeek)
	:ChainActor(
		dwelling, 
		getDwellingCreatures(dwelling, waitForGrowth), 
		chainMask, 
		getInitialTurn(waitForGrowth, dayOfWeek))
{
}

DwellingActor::~DwellingActor()
{
	delete creatureSet;
}

int DwellingActor::getInitialTurn(bool waitForGrowth, int dayOfWeek)
{
	if(!waitForGrowth)
		return 0;

	return 8 - dayOfWeek;
}

CCreatureSet * DwellingActor::getDwellingCreatures(const CGDwelling * dwelling, bool waitForGrowth)
{
	CCreatureSet * dwellingCreatures = new CCreatureSet();

	for(auto & creatureInfo : dwelling->creatures)
	{
		if(!creatureInfo.second.size())
			continue;

		auto creature = creatureInfo.second.back().toCreature();
		auto count = creatureInfo.first;
			
		if(waitForGrowth)
		{
			const CGTownInstance * town = dynamic_cast<const CGTownInstance *>(dwelling);

			count += town ? town->creatureGrowth(creature->level) : creature->growth;
		}

		dwellingCreatures->addToSlot(
			dwellingCreatures->getSlotFor(creature),
			creature->idNumber,
			TQuantity(creatureInfo.first));
	}

	return dwellingCreatures;
}

TownGarrisonActor::TownGarrisonActor(const CGTownInstance * town, uint64_t chainMask)
	:ChainActor(town, town->getUpperArmy(), chainMask, 0)
{
}