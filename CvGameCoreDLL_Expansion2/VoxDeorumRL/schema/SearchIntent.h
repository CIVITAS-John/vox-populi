// Installed from simulator/schema/shared/SearchIntent.h. Edit that source, then reinstall.

#ifndef VOX_RL_SEARCH_INTENT_H
#define VOX_RL_SEARCH_INTENT_H

#include "VoxRlTypes.h"

// Identifies why the native tactical caller requested one search. Unknown is reserved
// for synchronization requests and synthetic fixtures, and is not a live search intent.
// BattleFriendly, BattleNeutral, and BattleEnemy split a unit fight by the territory of the
// target tactical zone: our team's territory, neutral territory, or enemy territory.
// City attacks and city defense have intents of their own.
enum SearchIntent
{
    kSearchIntentUnknown = 0,
    kSearchIntentCityAssault = 1,
    kSearchIntentAttrition = 2,
    kSearchIntentExploitFlanks = 3,
    kSearchIntentSteamroll = 4,
    kSearchIntentSurgicalStrikeSupport = 5,
    kSearchIntentHedgehog = 6,
    kSearchIntentCounterattack = 7,
    kSearchIntentReinforce = 8,
    kSearchIntentGather = 9,
    kSearchIntentClearCamp = 10,
    kSearchIntentArmyContact = 11,
    kSearchIntentRangedOpportunity = 12,
    kSearchIntentBarbarianAttack = 13,
    kSearchIntentBattleFriendly = 14,
    kSearchIntentCityAttack = 15,
    kSearchIntentCityDefense = 16,
    kSearchIntentBattleNeutral = 17,
    kSearchIntentBattleEnemy = 18,
    kSearchIntentCount = 19
};

static const char* const kSearchIntentNames[kSearchIntentCount] =
{
    "Unknown",
    "CityAssault",
    "Attrition",
    "ExploitFlanks",
    "Steamroll",
    "SurgicalStrikeSupport",
    "Hedgehog",
    "Counterattack",
    "Reinforce",
    "Gather",
    "ClearCamp",
    "ArmyContact",
    "RangedOpportunity",
    "BarbarianAttack",
    "BattleFriendly",
    "CityAttack",
    "CityDefense",
    "BattleNeutral",
    "BattleEnemy"
};

// Returns a stable display name, treating unvalidated values as unknown.
inline const char* SearchIntentName(u8 intent)
{
    return intent < static_cast<u8>(kSearchIntentCount)
        ? kSearchIntentNames[intent]
        : kSearchIntentNames[kSearchIntentUnknown];
}

#endif
