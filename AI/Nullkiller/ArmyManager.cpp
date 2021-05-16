/*
* BuildingManager.cpp, part of VCMI engine
*
* Authors: listed in file AUTHORS in main folder
*
* License: GNU General Public License v2.0 or later
* Full text of license available in license.txt file, in main folder
*
*/

#include "StdInc.h"
#include "ArmyManager.h"

#include "../../CCallback.h"
#include "../../lib/mapObjects/MapObjects.h"

void ArmyManager::init(CPlayerSpecificInfoCallback * CB)
{
	cb = CB;
}

void ArmyManager::setAI(VCAI * AI)
{
	ai = AI;
}

std::vector<SlotInfo> ArmyManager::getSortedSlots(const CCreatureSet * target, const CCreatureSet * source) const
{
	const CCreatureSet * armies[] = { target, source };

	//we calculate total strength for each creature type available in armies
	std::map<const CCreature *, SlotInfo> creToPower;
	std::vector<SlotInfo> resultingArmy;

	for(auto armyPtr : armies)
	{
		for(auto & i : armyPtr->Slots())
		{
			auto & slotInfp = creToPower[i.second->type];

			slotInfp.creature = i.second->type;
			slotInfp.power += i.second->getPower();
			slotInfp.count += i.second->count;
		}
	}

	for(auto pair : creToPower)
		resultingArmy.push_back(pair.second);

	boost::sort(resultingArmy, [](const SlotInfo & left, const SlotInfo & right) -> bool
	{
		return left.power > right.power;
	});

	return resultingArmy;
}

std::vector<SlotInfo>::iterator ArmyManager::getWeakestCreature(std::vector<SlotInfo> & army) const
{
	auto weakest = boost::min_element(army, [](const SlotInfo & left, const SlotInfo & right) -> bool
	{
		if(left.creature->level != right.creature->level)
			return left.creature->level < right.creature->level;
		
		return left.creature->Speed() > right.creature->Speed();
	});

	return weakest;
}

std::vector<SlotInfo> ArmyManager::getBestArmy(const CCreatureSet * target, const CCreatureSet * source) const
{
	auto resultingArmy = getSortedSlots(target, source);

	if(resultingArmy.size() > GameConstants::ARMY_SIZE)
	{
		resultingArmy.resize(GameConstants::ARMY_SIZE);
	}
	else if(source->needsLastStack())
	{
		auto weakest = getWeakestCreature(resultingArmy);

		if(weakest->count == 1)
		{
			resultingArmy.erase(weakest);
		}
		else
		{
			weakest->power -= weakest->power / weakest->count;
			weakest->count--;
		}
	}

	return resultingArmy;
}

bool ArmyManager::canGetArmy(const CArmedInstance * target, const CArmedInstance * source) const
{
	//TODO: merge with pickBestCreatures
	//if (ai->primaryHero().h == source)
	if(target->tempOwner != source->tempOwner)
	{
		logAi->error("Why are we even considering exchange between heroes from different players?");
		return false;
	}

	return 0 < howManyReinforcementsCanGet(target, source);
}

ui64 ArmyManager::howManyReinforcementsCanBuy(const CCreatureSet * h, const CGDwelling * t) const
{
	ui64 aivalue = 0;
	auto army = getArmyAvailableToBuy(h, t);

	for(const creInfo & ci : army)
	{
		aivalue += ci.count * ci.cre->AIValue;
	}

	return aivalue;
}

std::vector<creInfo> ArmyManager::getArmyAvailableToBuy(const CCreatureSet * hero, const CGDwelling * dwelling) const
{
	auto availableRes = cb->getResourceAmount();
	std::vector<creInfo> creaturesInDwellings;
	int freeHeroSlots = GameConstants::ARMY_SIZE - hero->stacksCount();

	for(int i = dwelling->creatures.size() - 1; i >= 0; i--)
	{
		auto ci = infoFromDC(dwelling->creatures[i]);

		if(!ci.count || ci.creID == -1)
			continue;

		SlotID dst = hero->getSlotFor(ci.creID);
		if(!hero->hasStackAtSlot(dst)) //need another new slot for this stack
		{
			if(!freeHeroSlots) //no more place for stacks
				continue;
			else
				freeHeroSlots--; //new slot will be occupied
		}

		vstd::amin(ci.count, availableRes / ci.cre->cost); //max count we can afford

		if(!ci.count)
			continue;

		ci.level = i; //this is important for Dungeon Summoning Portal
		creaturesInDwellings.push_back(ci);
		availableRes -= ci.cre->cost * ci.count;
	}

	return creaturesInDwellings;
}

ui64 ArmyManager::howManyReinforcementsCanGet(const CCreatureSet * target, const CCreatureSet * source) const
{
	auto bestArmy = getBestArmy(target, source);
	uint64_t newArmy = 0;
	uint64_t oldArmy = target->getArmyStrength();

	for(auto & slot : bestArmy)
	{
		newArmy += slot.power;
	}

	return newArmy > oldArmy ? newArmy - oldArmy : 0;
}

uint64_t ArmyManager::evaluateStackPower(const CCreature * creature, int count) const
{
	return creature->AIValue * count;
}

SlotInfo ArmyManager::getTotalCreaturesAvailable(CreatureID creatureID) const
{
	auto creatureInfo = totalArmy.find(creatureID);

	return creatureInfo == totalArmy.end() ? SlotInfo() : creatureInfo->second;
}

void ArmyManager::update()
{
	logAi->trace("Start analysing army");

	std::vector<const CCreatureSet *> total;
	auto heroes = cb->getHeroesInfo();
	auto towns = cb->getTownsInfo();

	std::copy(heroes.begin(), heroes.end(), std::back_inserter(total));
	std::copy(towns.begin(), towns.end(), std::back_inserter(total));

	totalArmy.clear();

	for(auto army : total)
	{
		for(auto slot : army->Slots())
		{
			totalArmy[slot.second->getCreatureID()].count += slot.second->count;
		}
	}

	for(auto army : totalArmy)
	{
		army.second.creature = army.first.toCreature();
		army.second.power = evaluateStackPower(army.second.creature, army.second.count);
	}
}