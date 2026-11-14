/*	-------------------------------------------------------------------------------------------------------
	Vox Deorum: the operational loop (VOX_TACTICAL_OPERATIONAL_LOOP). See CvTacticalOperationalLoop.h. Each copy of
	a native function names its original in its comment, so a Vox Populi update can be diffed against it.
	------------------------------------------------------------------------------------------------------- */
#include "CvGameCoreDLLPCH.h"
#include "CvTacticalAI.h"
#include "CvTacticalSearchShared.h"
#include "CvTacticalAirSearch.h"
#include "CvTacticalAnalysisMap.h"
#include "CvTacticalOperationalLoop.h"
#include "CvTacticalOperationalValue.h"
#include "CvDangerPlots.h"
#include "CvMilitaryAI.h"
#include "CvGrandStrategyAI.h"
#include "CvGameCoreUtils.h"
#include "CvTypes.h"
#include "VoxDeorumRL/VoxRlCapture.h"
#include <math.h>
#include "LintFree.h"

// Per-posture offsets, indexed by eTacticalPosture. A unit fight inside one zone gets native's posture row, except
// Hedgehog's RISK (native -10).
const signed char OPERATIONAL_POSTURE_FLAVOR_ROWS[][STacticalFlavors::NUM_SLOTS] =
{
	//  RISK  OCCUPATION  ATTRITION  HOLD_CITY  HOLD_GROUND
	{    0,     0,          0,         0,         0 },  // None
	{  -30,   -20,          0,        10,       -10 },  // Withdraw
	{  -20,     0,         20,         0,         0 },  // Attrition
	{  -10,     0,         10,         0,         0 },  // Exploit flanks
	{   10,     5,         15,       -10,       -10 },  // Steamroll
	{    0,    15,         -5,       -10,       -10 },  // Surgical city strike
	{  -15,   -15,         10,        20,         0 },  // Hedgehog
	{  -10,   -10,         15,         0,        20 },  // Counterattack
};
typedef char OperationalPostureRowsMatchPostures[(sizeof(OPERATIONAL_POSTURE_FLAVOR_ROWS) / sizeof(OPERATIONAL_POSTURE_FLAVOR_ROWS[0]) == NUM_OPERATIONAL_POSTURES) ? 1 : -1];

// Per-kind offsets, added after the blend. City fight and Reinforce are native's CityAssault and Reinforce rows minus
// the mean of the postures they run from, rounded to 5, so they match native on average and still vary with posture.
// Own-city fight takes the zone's posture row alone.
const signed char OPERATIONAL_KIND_FLAVOR_ROWS[][STacticalFlavors::NUM_SLOTS] =
{
	//  RISK  OCCUPATION  ATTRITION  HOLD_CITY  HOLD_GROUND
	{    0,     0,          0,         0,         0 },  // Unit fight
	{   10,    15,        -25,        -5,        -5 },  // City fight
	{  -15,   -10,        -10,        10,        20 },  // Reinforce
	{    0,     0,          0,         0,         0 },  // Own-city fight
};
typedef char OperationalKindRowsMatchKinds[(sizeof(OPERATIONAL_KIND_FLAVOR_ROWS) / sizeof(OPERATIONAL_KIND_FLAVOR_ROWS[0]) == NUM_OPERATIONAL_KINDS) ? 1 : -1];

const char* const OPERATIONAL_POSTURE_NAMES[] =
{
	"NONE", "WITHDRAW", "ATTRITION", "EXPLOIT_FLANKS", "STEAMROLL", "SURGICAL_CITY_STRIKE", "HEDGEHOG", "COUNTERATTACK",
};
typedef char OperationalPostureNamesMatchPostures[(sizeof(OPERATIONAL_POSTURE_NAMES) / sizeof(OPERATIONAL_POSTURE_NAMES[0]) == NUM_OPERATIONAL_POSTURES) ? 1 : -1];

const char* const OPERATIONAL_KIND_NAMES[] =
{
	"UNIT_FIGHT", "CITY_FIGHT", "REINFORCE", "OWN_CITY_FIGHT",
};
typedef char OperationalKindNamesMatchKinds[(sizeof(OPERATIONAL_KIND_NAMES) / sizeof(OPERATIONAL_KIND_NAMES[0]) == NUM_OPERATIONAL_KINDS) ? 1 : -1];

// Vox Deorum: default search purposes in command kind order. Unit battles refine BattleNeutral by target territory.
// Operational reinforcement shares native's Reinforce purpose.
static const SearchIntent OPERATIONAL_KIND_SEARCH_INTENTS[] =
{
	kSearchIntentBattleNeutral, kSearchIntentCityAttack, kSearchIntentReinforce,
	kSearchIntentCityDefense,
};
typedef char OperationalKindIntentsMatchKinds[(sizeof(OPERATIONAL_KIND_SEARCH_INTENTS) / sizeof(OPERATIONAL_KIND_SEARCH_INTENTS[0]) == NUM_OPERATIONAL_KINDS) ? 1 : -1];

// The shipped mapping from VP flavors to the military flavors, one row per term.
const SOperationalVpTerm OPERATIONAL_VP_TERMS[] =
{
	//  slot                            flavor                   layer                         slot term  weight
	{ STacticalFlavors::RISK,         "FLAVOR_RISK",          VP_FLAVOR_LAYER_PERSONALITY,  -1,   1.0 },
	{ STacticalFlavors::OCCUPATION,   "FLAVOR_OFFENSE",       VP_FLAVOR_LAYER_INTENT,       -1,   0.1 },
	{ STacticalFlavors::OCCUPATION,   "FLAVOR_OFFENSE",       VP_FLAVOR_LAYER_STRATEGY,     -1,   0.9 },
	{ STacticalFlavors::OCCUPATION,   "FLAVOR_DEFENSE",       VP_FLAVOR_LAYER_STRATEGY,     -1,  -0.8 },
	{ STacticalFlavors::OCCUPATION,   "FLAVOR_CITY_DEFENSE",  VP_FLAVOR_LAYER_ACTIVE,       -1,  -0.2 },
	{ STacticalFlavors::ATTRITION,    "FLAVOR_RANGED",        VP_FLAVOR_LAYER_ACTIVE,       -1,   0.3 },
	{ STacticalFlavors::ATTRITION,    "FLAVOR_MILITARY_TRAINING", VP_FLAVOR_LAYER_ACTIVE,   -1,   0.1 },
	{ STacticalFlavors::ATTRITION,    "FLAVOR_CITY_DEFENSE",  VP_FLAVOR_LAYER_ACTIVE,       -1,   0.2 },
	{ STacticalFlavors::ATTRITION,    "FLAVOR_DEFENSE",       VP_FLAVOR_LAYER_STRATEGY,     -1,   0.4 },
	{ STacticalFlavors::ATTRITION,    "FLAVOR_DEFENSE",       VP_FLAVOR_LAYER_INTENT,       -1,   0.1 },
	{ STacticalFlavors::HOLD_CITY,    "FLAVOR_CITY_DEFENSE",  VP_FLAVOR_LAYER_ACTIVE,       -1,   1.0 },
	{ STacticalFlavors::HOLD_GROUND,  "FLAVOR_DEFENSE",       VP_FLAVOR_LAYER_INTENT,       -1,   0.1 },
	{ STacticalFlavors::HOLD_GROUND,  "FLAVOR_DEFENSE",       VP_FLAVOR_LAYER_STRATEGY,     -1,   1.0 },
	{ STacticalFlavors::HOLD_GROUND,  "FLAVOR_OFFENSE",       VP_FLAVOR_LAYER_STRATEGY,     -1,  -0.8 },
};
const int NUM_OPERATIONAL_VP_TERMS = sizeof(OPERATIONAL_VP_TERMS) / sizeof(OPERATIONAL_VP_TERMS[0]);

// the damage native's danger adds per fogged plot nearby (FOG_DEFAULT_DANGER in CvDangerPlots.cpp)
static const int OPERATIONAL_FOG_DANGER = 1;

//------------------------------------------------------------------------------
// settings
//------------------------------------------------------------------------------

// Copies the defines and the row and weight tables into the settings.
void GetDefaultOperationalSettings(OperationalSettings* pSettings)
{
	pSettings->iMaxCommands = GD_INT_GET(VOX_OPERATIONAL_MAX_COMMANDS);
	pSettings->iAggressionMediumRisk = GD_INT_GET(VOX_OPERATIONAL_AGGRESSION_MEDIUM_RISK);
	pSettings->iAggressionHighRisk = GD_INT_GET(VOX_OPERATIONAL_AGGRESSION_HIGH_RISK);
	pSettings->iAggressionBraveheartRisk = GD_INT_GET(VOX_OPERATIONAL_AGGRESSION_BRAVEHEART_RISK);
	for (int iSlot = 0; iSlot < STacticalFlavors::NUM_SLOTS; iSlot++)
	{
		for (int iPosture = 0; iPosture < NUM_OPERATIONAL_POSTURES; iPosture++)
			pSettings->aiPostureRows[iPosture][iSlot] = OPERATIONAL_POSTURE_FLAVOR_ROWS[iPosture][iSlot];
		for (int iKind = 0; iKind < NUM_OPERATIONAL_KINDS; iKind++)
			pSettings->aiKindRows[iKind][iSlot] = OPERATIONAL_KIND_FLAVOR_ROWS[iKind][iSlot];
		pSettings->afFlavorSlope[iSlot] = OPERATIONAL_FLAVOR_SLOPE[iSlot];
	}
	for (int i = 0; i < NUM_OPERATIONAL_WEIGHTS; i++)
		pSettings->afWeightBase[i] = OPERATIONAL_WEIGHT[i].fBase;
	for (int i = 0; i < NUM_OPERATIONAL_CONSTANTS; i++)
		pSettings->afConstant[i] = OPERATIONAL_CONSTANT[i].fValue;
}

// The game runs the defaults for every player. The simulator adds its override for the player's role after them.
void GetOperationalSettings(PlayerTypes ePlayer, OperationalSettings* pSettings)
{
	GetDefaultOperationalSettings(pSettings);
}

// Checks the thresholds from the top down.
eAggressionLevel AggressionFromRisk(int iRisk, const OperationalSettings& settings)
{
	if (iRisk >= settings.iAggressionBraveheartRisk)
		return AL_BRAVEHEART;
	if (iRisk >= settings.iAggressionHighRisk)
		return AL_HIGH;
	if (iRisk >= settings.iAggressionMediumRisk)
		return AL_MEDIUM;
	return AL_LOW;
}

//------------------------------------------------------------------------------
// mapped flavors
//------------------------------------------------------------------------------

// Searches every custom value for the one whose personality and game-scale change match the active value best.
int MapVpFlavor(int iActive, int iPersonalityMax)
{
	const int iMaximum = std::min(100, std::max(1, iPersonalityMax));
	int iBest = 50;
	int iBestDistance = INT_MAX;
	for (int iValue = 0; iValue <= 100; iValue++)
	{
		// the rounding CvFlavorManager::GetPersonalityIndividualFlavor applies to a custom value
		const int iPersonality = std::min(iMaximum, std::max(0, (iValue * iMaximum + 50) / 100));
		const int iDistance = abs(iPersonality + CvFlavorManager::FlavorToGameScale(iValue) - iActive);
		if (iDistance < iBestDistance || (iDistance == iBestDistance && abs(iValue - 50) < abs(iBest - 50)))
		{
			iBest = iValue;
			iBestDistance = iDistance;
		}
	}
	return iBest;
}

// Shifts the custom value by the strategy rows that survived, or converts the active value back.
int MapVpFlavorRow(const SVpFlavorLayers& flavor, int iPersonalityMax)
{
	if (flavor.iCustom < 0)
		return MapVpFlavor(flavor.iActive, iPersonalityMax);
	return CvFlavorManager::ShiftFlavor(flavor.iCustom, flavor.iActive - flavor.iBase - CvFlavorManager::FlavorToGameScale(flavor.iCustom));
}

// Reads the layer: a custom value stands in for both the personality and the grand strategy.
int ReadVpFlavorLayer(const SVpFlavorLayers& flavor, eVpFlavorLayer eLayer, int iPersonalityMax)
{
	const int iMaximum = std::min(100, std::max(1, iPersonalityMax));
	const bool bCustom = flavor.iCustom >= 0;
	switch (eLayer)
	{
	case VP_FLAVOR_LAYER_PERSONALITY:
		if (bCustom)
			return flavor.iCustom;
		return std::min(100, std::max(0, (flavor.iBase * 100 + iMaximum / 2) / iMaximum));
	case VP_FLAVOR_LAYER_INTENT:
	{
		if (bCustom)
			return flavor.iCustom;
		// round the scaled mod half away from zero, so a mod of -2 reads 30 just as +2 reads 70
		const int iScaled = (flavor.iGrand - flavor.iBase) * 100;
		const int iMod = iScaled >= 0 ? (iScaled + iMaximum / 2) / iMaximum : -((-iScaled + iMaximum / 2) / iMaximum);
		return std::min(100, std::max(0, 50 + iMod));
	}
	case VP_FLAVOR_LAYER_STRATEGY:
		return CvFlavorManager::FlavorFromGameScale(flavor.iActive - flavor.iBase - (bCustom ? CvFlavorManager::FlavorToGameScale(flavor.iCustom) : 0));
	default:
		return MapVpFlavorRow(flavor, iPersonalityMax);
	}
}

// Two passes: slots whose terms are all native flavors, then slots that read another slot.
bool ComputeMilitaryFlavors(const SOperationalVpTerm* aTerms, int nTerms, const SVpFlavorLayers* aTermFlavors,
	const SVpFlavorLayers& offense, int iPersonalityMax, int (&aiFlavors)[STacticalFlavors::NUM_SLOTS],
	int* piMissingTerm, int* aiTermInputs)
{
	double afValues[STacticalFlavors::NUM_SLOTS];
	bool abReadsSlot[STacticalFlavors::NUM_SLOTS];
	bool abMapped[STacticalFlavors::NUM_SLOTS];
	for (int iSlot = 0; iSlot < STacticalFlavors::NUM_SLOTS; iSlot++)
	{
		aiFlavors[iSlot] = -1;
		afValues[iSlot] = 50.0;
		abReadsSlot[iSlot] = false;
		abMapped[iSlot] = false;
	}
	for (int i = 0; i < nTerms; i++)
	{
		abMapped[aTerms[i].iSlot] = true;
		if (aTerms[i].iSlotTerm >= 0)
			abReadsSlot[aTerms[i].iSlot] = true;
	}
	for (int iPass = 0; iPass < 2; iPass++)
	{
		for (int iSlot = 0; iSlot < STacticalFlavors::NUM_SLOTS; iSlot++)
		{
			if (!abMapped[iSlot] || abReadsSlot[iSlot] != (iPass == 1))
				continue;
			double fValue = 50.0;
			for (int i = 0; i < nTerms; i++)
			{
				const SOperationalVpTerm& term = aTerms[i];
				if (term.iSlot != iSlot)
					continue;
				double fInput = 0.0;
				if (term.iSlotTerm >= 0)
					fInput = afValues[term.iSlotTerm];
				else
				{
					const SVpFlavorLayers* pFlavor = &aTermFlavors[i];
					// a leader without FLAVOR_RISK of its own reads RISK from offense, as the general RISK does; the game never
					// randomizes a zero flavor. A custom RISK is the strategist's own number, so it is read as set.
					if (strcmp(term.szFlavor, "FLAVOR_RISK") == 0 && (!pFlavor->bFound || (pFlavor->iBase == 0 && pFlavor->iCustom < 0)))
						pFlavor = &offense;
					if (!pFlavor->bFound)
					{
						if (piMissingTerm)
							*piMissingTerm = i;
						return false;
					}
					const int iInput = ReadVpFlavorLayer(*pFlavor, term.eLayer, iPersonalityMax);
					if (aiTermInputs)
						aiTermInputs[i] = iInput;
					fInput = iInput;
				}
				fValue += term.fWeight * (fInput - 50.0);
			}
			afValues[iSlot] = fValue;
			aiFlavors[iSlot] = static_cast<int>(std::min(100.0, std::max(0.0, floor(fValue + 0.5))));
		}
	}
	return true;
}

// Reads the active, base, grand strategy and custom values of one flavor.
SVpFlavorLayers ReadVpFlavorLayers(CvPlayer& kPlayer, FlavorTypes eFlavor)
{
	SVpFlavorLayers flavor;
	if (eFlavor == NO_FLAVOR)
		return flavor;
	CvFlavorManager* pFlavorManager = kPlayer.GetFlavorManager();
	flavor.bFound = true;
	flavor.iActive = pFlavorManager->GetActiveFlavor(eFlavor);
	flavor.iBase = pFlavorManager->GetBasePersonalityFlavor(eFlavor);
	flavor.iGrand = kPlayer.GetGrandStrategyAI()->GetPersonalityAndGrandStrategy(eFlavor);
	flavor.iCustom = pFlavorManager->HasCustomFlavors() ? pFlavorManager->GetCustomFlavor(eFlavor) : -1;
	return flavor;
}

// Reads each term's flavor from the player and maps them; slots that cannot be read stay neutral.
void GetMappedMilitaryFlavors(PlayerTypes ePlayer, int (&aiFlavors)[STacticalFlavors::NUM_SLOTS])
{
	CvPlayer& kPlayer = GET_PLAYER(ePlayer);
	vector<SVpFlavorLayers> termFlavors(NUM_OPERATIONAL_VP_TERMS);
	for (int i = 0; i < NUM_OPERATIONAL_VP_TERMS; i++)
		if (OPERATIONAL_VP_TERMS[i].szFlavor)
			termFlavors[i] = ReadVpFlavorLayers(kPlayer, (FlavorTypes)GC.getInfoTypeForString(OPERATIONAL_VP_TERMS[i].szFlavor, true));
	const SVpFlavorLayers offense = ReadVpFlavorLayers(kPlayer, (FlavorTypes)GC.getInfoTypeForString("FLAVOR_OFFENSE", true));
	if (!ComputeMilitaryFlavors(OPERATIONAL_VP_TERMS, NUM_OPERATIONAL_VP_TERMS, &termFlavors[0], offense,
		GD_INT_GET(PERSONALITY_FLAVOR_MAX_VALUE), aiFlavors))
	{
		for (int iSlot = 0; iSlot < STacticalFlavors::NUM_SLOTS; iSlot++)
			aiFlavors[iSlot] = 50;
		return;
	}
	for (int iSlot = 0; iSlot < STacticalFlavors::NUM_SLOTS; iSlot++)
		if (aiFlavors[iSlot] < 0)
			aiFlavors[iSlot] = 50;
}

//------------------------------------------------------------------------------
// geographic blend
//------------------------------------------------------------------------------

// Averages the zone vectors by plot share in doubles, then adds the kind row and rounds once.
void BlendZoneFlavors(const vector<SOperationalZoneFlavors>& zones, const int (&aiMapped)[STacticalFlavors::NUM_SLOTS],
	const int (&aiKindRow)[STacticalFlavors::NUM_SLOTS], int (&aiFlavors)[STacticalFlavors::NUM_SLOTS], int& iRepresentativeZone)
{
	iRepresentativeZone = -1;
	int iBestPlots = 0;
	int iTotalPlots = 0;
	for (size_t i = 0; i < zones.size(); i++)
	{
		iTotalPlots += zones[i].iPlots;
		if (zones[i].iPlots > iBestPlots || (zones[i].iPlots == iBestPlots && iRepresentativeZone != -1 && zones[i].iZoneID < iRepresentativeZone))
		{
			iBestPlots = zones[i].iPlots;
			iRepresentativeZone = zones[i].iZoneID;
		}
	}
	for (int iSlot = 0; iSlot < STacticalFlavors::NUM_SLOTS; iSlot++)
	{
		double fValue = aiMapped[iSlot];
		if (iTotalPlots > 0)
		{
			fValue = 0.0;
			for (size_t i = 0; i < zones.size(); i++)
				fValue += static_cast<double>(zones[i].iPlots) * zones[i].aiFlavor[iSlot] / iTotalPlots;
		}
		fValue += aiKindRow[iSlot];
		aiFlavors[iSlot] = static_cast<int>(floor(std::min(100.0, std::max(0.0, fValue)) + 0.5));
	}
}

// Blends the search flavors from the mapped values and the aggression risk from neutral 50 through the same zones,
// tactical modifiers, posture rows and kind row.
STacticalFlavors CvTacticalOperationalLoop::BlendEngagementFlavors(PlayerTypes ePlayer, const map<int, int>& zonePlots, eOperationalKind eKind,
	PlayerTypes eCounterpart, int iOperationID, const int (&aiMapped)[STacticalFlavors::NUM_SLOTS],
	const OperationalSettings& settings, int& iRepresentativeZone, int& iAggressionRisk)
{
	CvPlayer& kPlayer = GET_PLAYER(ePlayer);
	CvTacticalAnalysisMap* pZoneMap = kPlayer.GetTacticalAI()->GetTacticalAnalysisMap();

	const short* pCivModifier = NULL;
	if (eCounterpart != NO_PLAYER && kPlayer.GetMilitaryAI()->HasTacticalFlavorModifiers())
		pCivModifier = kPlayer.GetMilitaryAI()->GetTacticalFlavorModifiers(eCounterpart);
	const short* pOperationModifier = NULL;
	if (iOperationID >= 0)
	{
		CvAIOperation* pOperation = kPlayer.getAIOperation(iOperationID);
		if (pOperation)
			pOperationModifier = pOperation->GetTacticalFlavorModifiers();
	}

	int aiNeutral[STacticalFlavors::NUM_SLOTS];
	for (int iSlot = 0; iSlot < STacticalFlavors::NUM_SLOTS; iSlot++)
		aiNeutral[iSlot] = 50;
	vector<SOperationalZoneFlavors> zones;
	vector<SOperationalZoneFlavors> neutralZones;
	for (map<int, int>::const_iterator it = zonePlots.begin(); it != zonePlots.end(); ++it)
	{
		const CvTacticalDominanceZone* pZone = pZoneMap->GetZoneByIDWithoutRefresh(it->first);
		if (!pZone || it->second <= 0)
			continue;
		SOperationalZoneFlavors zone;
		zone.iZoneID = it->first;
		zone.iPlots = it->second;
		SOperationalZoneFlavors neutralZone = zone;
		int aiZone[STacticalFlavors::NUM_SLOTS];
		int aiNeutralZone[STacticalFlavors::NUM_SLOTS];
		for (int iSlot = 0; iSlot < STacticalFlavors::NUM_SLOTS; iSlot++)
		{
			aiZone[iSlot] = aiMapped[iSlot];
			aiNeutralZone[iSlot] = aiNeutral[iSlot];
		}
		const short* pZoneModifier = pZoneMap->HasZoneTacticalFlavorModifiers() ? pZoneMap->GetZoneTacticalFlavorModifiers(it->first) : NULL;
		if (pCivModifier || pOperationModifier || pZoneModifier)
		{
			TacticalAIHelpers::ApplyFlavorModifiers(aiZone, pCivModifier, pOperationModifier, pZoneModifier, kSearchIntentUnknown, false);
			TacticalAIHelpers::ApplyFlavorModifiers(aiNeutralZone, pCivModifier, pOperationModifier, pZoneModifier, kSearchIntentUnknown, false);
		}
		const int iPosture = range((int)pZone->GetPosture(), 0, NUM_OPERATIONAL_POSTURES - 1);
		for (int iSlot = 0; iSlot < STacticalFlavors::NUM_SLOTS; iSlot++)
		{
			zone.aiFlavor[iSlot] = aiZone[iSlot] + settings.aiPostureRows[iPosture][iSlot];
			neutralZone.aiFlavor[iSlot] = aiNeutralZone[iSlot] + settings.aiPostureRows[iPosture][iSlot];
		}
		zones.push_back(zone);
		neutralZones.push_back(neutralZone);
	}

	int aiAggression[STacticalFlavors::NUM_SLOTS];
	int iNeutralRepresentative = -1;
	BlendZoneFlavors(neutralZones, aiNeutral, settings.aiKindRows[eKind], aiAggression, iNeutralRepresentative);
	iAggressionRisk = aiAggression[STacticalFlavors::RISK];
	STacticalFlavors flavors;
	BlendZoneFlavors(zones, aiMapped, settings.aiKindRows[eKind], flavors.iFlavor, iRepresentativeZone);
	flavors.bRiskSet = true;
	flavors.iCounterpart = eCounterpart;
	return flavors;
}

//------------------------------------------------------------------------------
// Reinforce selection
//------------------------------------------------------------------------------

// Rounds the plain distance up to whole turns.
int TravelTurns(int iDistance, int iMoves)
{
	const int iPerTurn = std::max(1, iMoves);
	return std::max(1, (iDistance + iPerTurn - 1) / iPerTurn);
}

// Orders Reinforce candidates nearest first
struct PrOperationalCandidateNearest
{
	// Ranks by join turns, then travel turns, then unit ID.
	bool operator()(const SOperationalCandidate& lhs, const SOperationalCandidate& rhs) const
	{
		if (lhs.iJoinTurns != rhs.iJoinTurns)
			return lhs.iJoinTurns < rhs.iJoinTurns;
		if (lhs.iTravelTurns != rhs.iTravelTurns)
			return lhs.iTravelTurns < rhs.iTravelTurns;
		return lhs.iUnitID < rhs.iUnitID;
	}
};

// Full strength sets the stop; discounted help sets the score.
void SelectReinforcement(const vector<SOperationalCandidate>& candidates, double fLocal, double fEnemy, double fRatio, bool bNeedCapturer,
	SOperationalReinforce& out)
{
	vector<SOperationalCandidate> sorted = candidates;
	std::sort(sorted.begin(), sorted.end(), PrOperationalCandidateNearest());
	out.fLocal = fLocal;
	out.fEnemy = fEnemy;
	out.fNeed = std::max(0.0, fRatio * fEnemy - fLocal);
	out.vSelected.clear();
	out.fSelectedStrength = 0.0;
	out.fHelp = 0.0;
	out.fRoleHelp = 0.0;
	out.iRoleUnit = -1;
	out.bMissingRole = false;
	out.iExtended = 0;
	out.fBaseFall = 0.0;
	out.fBaseGarrison = 0.0;
	out.fProjectedFall = 0.0;
	out.fProjectedGarrison = 0.0;

	bool bHasCapturer = !bNeedCapturer;
	size_t i = 0;
	for (; i < sorted.size() && out.fSelectedStrength < out.fNeed; i++)
	{
		out.vSelected.push_back(sorted[i].iUnitID);
		out.fSelectedStrength += sorted[i].fStrength;
		out.fHelp += sorted[i].fStrength / std::max(1, sorted[i].iJoinTurns);
		bHasCapturer = bHasCapturer || sorted[i].bCapturer;
	}

	//the nearest capturer left completes the capture role, even when no help is needed
	for (; i < sorted.size() && !bHasCapturer; i++)
	{
		if (!sorted[i].bCapturer)
			continue;
		out.iRoleUnit = sorted[i].iUnitID;
		out.fRoleHelp = sorted[i].fStrength / std::max(1, sorted[i].iJoinTurns);
		out.vSelected.push_back(sorted[i].iUnitID);
		out.fSelectedStrength += sorted[i].fStrength;
		out.fHelp += out.fRoleHelp;
		bHasCapturer = true;
	}
	out.bMissingRole = !bHasCapturer;
	ScoreReinforcement(fRatio, out);
}

// Help past need plus role help adds nothing to gain; strength past parity adds nothing to odds.
void ScoreReinforcement(double fRatio, SOperationalReinforce& out)
{
	out.fGain = 0.0;
	out.fArrivalRatio = 0.0;
	out.fOdds = 0.0;
	if (out.fEnemy <= 0.0)
		return;
	out.fGain = std::min(fRatio / (1.0 + fRatio), std::min(out.fHelp, out.fNeed + out.fRoleHelp) / ((1.0 + fRatio) * out.fEnemy));
	out.fArrivalRatio = (out.fLocal + out.fSelectedStrength) / out.fEnemy;
	out.fOdds = fRatio > 0.0 ? std::min(1.0, out.fArrivalRatio / fRatio) : 1.0;
}

// Reinforce ranks below every fight; among fights, cities come before units and our cities first.
int OfferRank(eOperationalCenter eCenter, bool bReinforce)
{
	if (bReinforce)
		return 3;
	return eCenter == OPERATIONAL_CENTER_OUR_CITY ? 0 : eCenter == OPERATIONAL_CENTER_ENEMY_CITY ? 1 : 2;
}

// Compares the estimate first and falls back to rank and plot only on an exact tie, so build order breaks full ties.
bool OfferOutranks(double fEstimate, int iRank, int iPlotIndex, double fBest, int iBestRank, int iBestPlotIndex)
{
	if (fEstimate != fBest)
		return fEstimate > fBest;
	if (iRank != iBestRank)
		return iRank < iBestRank;
	return iPlotIndex < iBestPlotIndex;
}

// Bombardment alone leaves a city at 1 HP, where it can still heal, so damage past R - 1 needs a capturer.
double AttackFallShare(double fCityDamage, int iHeal, int iRemaining, bool bCapture)
{
	const double fRemaining = static_cast<double>(std::max(1, iRemaining));
	if (bCapture && fCityDamage >= fRemaining)
		return 1.0;
	return std::max(0.0, std::min(fCityDamage, fRemaining - 1.0) - iHeal) / fRemaining;
}

// Capturing the city kills its garrison, so a falling city pays the whole garrison prize.
double GarrisonShare(double fGarrisonDamage, int iGarrisonHealth, bool bCityFalls)
{
	if (iGarrisonHealth <= 0)
		return 0.0;
	if (bCityFalls)
		return 1.0;
	return std::min(1.0, std::max(0.0, fGarrisonDamage) / iGarrisonHealth);
}

//------------------------------------------------------------------------------
// turn and command loop
//------------------------------------------------------------------------------

// Copy of CvTacticalAI::ProcessDominanceZones (non-barbarian branch). Changes: the zone loop runs PlotEmergencyPurchases
// only, without ExtractTargetsForZone or a posture routine; the command loop runs after it; PlotDeferredGarrisonMoves
// follows the loop, and PlotReinforcementMoves does not run.
void CvTacticalOperationalLoop::ProcessDominanceZones(CvTacticalAI& kTactical)
{
	CvPlayer* pPlayer = kTactical.m_pPlayer;

	//high prio goes first
	kTactical.AssignGlobalHighPrioMoves();

	// Vox Deorum: the first zone dispatch flushes high-priority changes and publishes stance labels once.
	if (MOD_IPC_CHANNEL && gVoxRlCaptureEnabled)
	{
		VoxRlCapture::GetInstance().SetMilitaryPhase(pPlayer->GetID(), VOX_RL_MILITARY_PHASE_ZONE_WORK);
		VoxRlCapture::GetInstance().OnFirstZoneDispatch(pPlayer->GetID());
	}

	//every zone's purchases come before the command loop
	for (int iI = 0; iI < kTactical.GetTacticalAnalysisMap()->GetNumZones(); iI++)
		kTactical.PlotEmergencyPurchases(kTactical.GetTacticalAnalysisMap()->GetZoneByIndex(iI));

	RunCommandLoop(kTactical);

	// Vox Deorum: label the deferred garrisons as the second zone loop, as native does.
	if (MOD_IPC_CHANNEL && gVoxRlCaptureEnabled)
		VoxRlCapture::GetInstance().SetMilitaryPhase(pPlayer->GetID(), VOX_RL_MILITARY_PHASE_ZONE_REINFORCEMENT);
	kTactical.PlotDeferredGarrisonMoves();
	if (MOD_IPC_CHANNEL && gVoxRlCaptureEnabled)
		VoxRlCapture::GetInstance().OnZoneReinforcementComplete(pPlayer->GetID());

	//now mid prio moves like capturing barb camps, pillaging
	if (MOD_IPC_CHANNEL && gVoxRlCaptureEnabled)
		VoxRlCapture::GetInstance().SetMilitaryPhase(pPlayer->GetID(), VOX_RL_MILITARY_PHASE_GLOBAL_MID);
	kTactical.AssignGlobalMidPrioMoves();

	//finally arrange our remaining idle units for defense
	if (MOD_IPC_CHANNEL && gVoxRlCaptureEnabled)
		VoxRlCapture::GetInstance().SetMilitaryPhase(pPlayer->GetID(), VOX_RL_MILITARY_PHASE_GLOBAL_LOW);
	kTactical.AssignGlobalLowPrioMoves();

	//failsafe
	if (MOD_IPC_CHANNEL && gVoxRlCaptureEnabled)
		VoxRlCapture::GetInstance().SetMilitaryPhase(pPlayer->GetID(), VOX_RL_MILITARY_PHASE_UNASSIGNED_REVIEW);
	kTactical.ReviewUnassignedUnits();
}

// Picks the top ready offer, checks it, executes it on a pass, retires the offers it covered, then estimates every
// open offer again and wakes waiting fights on helpful changes.
void CvTacticalOperationalLoop::RunCommandLoop(CvTacticalAI& kTactical, SOperationalTurnRecord* pRecord)
{
	SOperationalTurn turn;
	BuildTurn(kTactical, turn);
	BuildBattlefields(kTactical, turn);
	BuildThreats(turn);
	for (size_t i = 0; i < turn.battlefields.size(); i++)
	{
		SOperationalBattlefield& field = turn.battlefields[i];
		if (!EstimateBattlefield(kTactical, turn, field, true))
		{
			field.eFight = OPERATIONAL_OFFER_DROPPED;
			field.eReinforce = OPERATIONAL_OFFER_DROPPED;
			continue;
		}
		field.eFight = field.fEstimate > 0.0 ? OPERATIONAL_OFFER_READY : OPERATIONAL_OFFER_INACTIVE;
		field.eReinforce = field.fReinforceEstimate > 0.0 ? OPERATIONAL_OFFER_READY : OPERATIONAL_OFFER_INACTIVE;
	}

	SOperationalTurnRecord localRecord;
	SOperationalTurnRecord& record = pRecord ? *pRecord : localRecord;
	const bool bLog = !pRecord && GC.getLogging() && GC.getAILogging();
	if (bLog)
		kTactical.LogTacticalMessage(CvString::format("Operational loop scales: army %.0f (typical unit %.0f, city unit %.4f), cities %.1f, ground base %.1f",
			turn.fArmyScale, turn.fTypicalUnitWorth, turn.fCityUnit, turn.fCityScale, turn.fGroundBase));
	const int iMaxFailures = std::max(1, static_cast<int>(turn.weights.afConstant[OPERATIONAL_CONSTANT_READINESS_MAX_FAILURES]));
	int iCommands = 0;
	for (;;)
	{
		//the ready offer with the highest estimate; ties go to our cities' fights, then enemy cities' fights, then enemy
		//units' fights, then Reinforce, then the lower plot, then build order
		int iBest = -1;
		bool bBestReinforce = false;
		double fBest = 0.0;
		int iBestRank = 0;
		for (size_t i = 0; i < turn.battlefields.size(); i++)
		{
			const SOperationalBattlefield& field = turn.battlefields[i];
			for (int iOffer = 0; iOffer < 2; iOffer++)
			{
				const bool bReinforce = iOffer == 1;
				if ((bReinforce ? field.eReinforce : field.eFight) != OPERATIONAL_OFFER_READY)
					continue;
				const double fEstimate = bReinforce ? field.fReinforceEstimate : field.fEstimate;
				const int iRank = OfferRank(field.eCenter, bReinforce);
				if (iBest >= 0 && !OfferOutranks(fEstimate, iRank, field.iPlotIndex, fBest, iBestRank, turn.battlefields[iBest].iPlotIndex))
					continue;
				iBest = static_cast<int>(i);
				bBestReinforce = bReinforce;
				fBest = fEstimate;
				iBestRank = iRank;
			}
		}
		if (iBest < 0)
			break;
		if (iCommands >= turn.settings.iMaxCommands)
		{
			record.bCommandCap = true;
			if (bLog)
				kTactical.LogTacticalMessage(CvString::format("Operational loop stopped at the command cap of %d", turn.settings.iMaxCommands));
			break;
		}

		SOperationalBattlefield& field = turn.battlefields[iBest];
		const eOperationalCheckResult eResult = bBestReinforce ? CheckReinforce(kTactical, turn, field) : CheckFight(kTactical, turn, field);
		SOperationalCheckRecord check = { iBest, eResult, bBestReinforce };
		record.checks.push_back(check);
		if (bLog)
		{
			static const char* const aszResults[] = { "passed", "failed: target gone", "failed: no recruits", "failed: no damage",
				"failed: no capturer", "failed: no defenders", "failed: no nearby enemy", "failed: no reachable unit",
				"reduced" };
			kTactical.LogTacticalMessage(CvString::format("Operational check of %s %d at plot %d, estimate %.6f: %s",
				bBestReinforce ? "Reinforce" : "fight", iBest, field.iPlotIndex, fBest, aszResults[eResult]));
		}
		if (bBestReinforce && eResult != OPERATIONAL_CHECK_PASSED)
		{
			//a reduced selection goes back into the queue at its checked estimate; Reinforce never waits
			field.eReinforce = eResult == OPERATIONAL_CHECK_REDUCED && field.fReinforceEstimate > 0.0 ? OPERATIONAL_OFFER_READY : OPERATIONAL_OFFER_INACTIVE;
			continue;
		}
		if (eResult != OPERATIONAL_CHECK_PASSED)
		{
			field.iFailures++;
			if (field.iFailures >= iMaxFailures)
			{
				field.eFight = OPERATIONAL_OFFER_SKIPPED;
				record.vSkipped.push_back(iBest);
			}
			else
				field.eFight = OPERATIONAL_OFFER_WAITING;
			continue;
		}

		//the record is kept before the offer runs, so a command that ends the run is still recorded
		record.commands.push_back(SOperationalCommandRecord());
		SOperationalCommandRecord& command = record.commands.back();
		command.iCommand = iCommands;
		command.iBattlefield = iBest;
		command.iPlotIndex = field.iPlotIndex;
		command.eKind = bBestReinforce ? OPERATIONAL_KIND_REINFORCE : field.eCenter == OPERATIONAL_CENTER_OUR_CITY ? OPERATIONAL_KIND_OWN_CITY_FIGHT :
			field.eCenter == OPERATIONAL_CENTER_ENEMY_CITY ? OPERATIONAL_KIND_CITY_FIGHT : OPERATIONAL_KIND_UNIT_FIGHT;
		command.eOwner = field.eOwner;
		//a Reinforce runs at its checked estimate, which a replaced selection may have raised
		command.fEstimate = bBestReinforce ? field.fReinforceEstimate : fBest;
		command.iFailuresBefore = bBestReinforce ? 0 : field.iFailures;

		SOperationalBoard before;
		SnapshotBoard(turn, before);
		BeginCommand(kTactical, iCommands, field.iZoneID);
		if (bBestReinforce)
			ExecuteReinforce(kTactical, turn, field, command);
		else
			ExecuteFight(kTactical, turn, field, command);
		EndCommand(kTactical);
		iCommands++;
		if (bBestReinforce)
			field.eReinforce = OPERATIONAL_OFFER_EXECUTED;
		else
			field.eFight = OPERATIONAL_OFFER_EXECUTED;

		//the exemptions read the threats after the command, since its search may have moved units into a target's reach
		BuildThreats(turn);

		for (size_t i = 0; i < turn.battlefields.size(); i++)
		{
			SOperationalBattlefield& other = turn.battlefields[i];
			//an enemy-unit fight that ran its search covered the unit fights around its target
			if (!bBestReinforce && command.bSearchRan && RetiresFight(field, other))
			{
				other.eFight = OPERATIONAL_OFFER_RETIRED;
				command.vRetired.push_back(other.iID);
			}
			//a Reinforce that moved a unit covered the enemy groups it counted
			if (bBestReinforce && command.bMoved && RetiresReinforce(field, other))
			{
				other.eReinforce = OPERATIONAL_OFFER_RETIRED;
				command.vRetiredReinforce.push_back(other.iID);
			}
			//reach checks hold only until a command changes the board
			other.reinforce.bChecked = false;
		}

		SOperationalBoard after;
		SnapshotBoard(turn, after);
		SOperationalChanges changes;
		DiffBoard(turn.ePlayer, before, after, changes);

		//a city this command captured becomes one of our cities, with its own offers from the next estimate on
		for (size_t i = 0; i < changes.vCapturedPlots.size(); i++)
		{
			CvPlot* pPlot = GC.getMap().plotByIndex(changes.vCapturedPlots[i]);
			CvCity* pCity = pPlot ? pPlot->getPlotCity() : NULL;
			if (!pCity || pCity->getOwner() != turn.ePlayer)
				continue;
			bool bKnown = false;
			for (size_t j = 0; j < turn.battlefields.size() && !bKnown; j++)
				bKnown = turn.battlefields[j].eCenter == OPERATIONAL_CENTER_OUR_CITY && turn.battlefields[j].iTargetID == pCity->GetID();
			if (!bKnown)
				AddOwnCityBattlefield(kTactical, turn, *pCity);
		}
		if (bLog)
			kTactical.LogTacticalMessage(CvString::format("Operational command %d: %s %d at plot %d, search %s, %d fights and %d Reinforce retired",
				command.iCommand, bBestReinforce ? "Reinforce" : "fight", iBest, command.iPlotIndex, command.bSearchRan ? "ran" : "did not run",
				(int)command.vRetired.size(), (int)command.vRetiredReinforce.size()));

		//estimate every open offer on the new board; an executed fight still sets the stake its Reinforce reads
		for (size_t i = 0; i < turn.battlefields.size(); i++)
		{
			SOperationalBattlefield& other = turn.battlefields[i];
			const bool bFightOpen = other.eFight == OPERATIONAL_OFFER_READY || other.eFight == OPERATIONAL_OFFER_INACTIVE ||
				other.eFight == OPERATIONAL_OFFER_WAITING;
			const bool bReinforceOpen = other.eReinforce == OPERATIONAL_OFFER_READY || other.eReinforce == OPERATIONAL_OFFER_INACTIVE;
			if (!bFightOpen && !bReinforceOpen)
				continue;
			if (!EstimateBattlefield(kTactical, turn, other, bReinforceOpen))
			{
				if (bFightOpen)
					other.eFight = OPERATIONAL_OFFER_DROPPED;
				if (bReinforceOpen)
					other.eReinforce = OPERATIONAL_OFFER_DROPPED;
				continue;
			}
			if (other.eFight == OPERATIONAL_OFFER_WAITING)
			{
				if (other.fEstimate > 0.0 && WakesFight(turn.ePlayer, turn, other, changes))
					other.eFight = OPERATIONAL_OFFER_READY;
			}
			else if (bFightOpen)
				other.eFight = other.fEstimate > 0.0 ? OPERATIONAL_OFFER_READY : OPERATIONAL_OFFER_INACTIVE;
			if (bReinforceOpen)
				other.eReinforce = other.fReinforceEstimate > 0.0 ? OPERATIONAL_OFFER_READY : OPERATIONAL_OFFER_INACTIVE;
		}
	}
}

//------------------------------------------------------------------------------
// turn, threats and battlefields
//------------------------------------------------------------------------------

// Resolves the weights from the mapped flavors and sums our units, cities and land into the turn's fixed scales.
void CvTacticalOperationalLoop::BuildTurn(CvTacticalAI& kTactical, SOperationalTurn& turn)
{
	CvPlayer& kPlayer = *kTactical.m_pPlayer;
	const PlayerTypes ePlayer = kPlayer.GetID();
	turn.ePlayer = ePlayer;
	GetOperationalSettings(ePlayer, &turn.settings);
	GetMappedMilitaryFlavors(ePlayer, turn.aiMapped);
	ResolveOperationalWeights(turn.settings, turn.aiMapped, turn.weights);

	//Z_U: every unit of ours at its value, as the grade sums it; B counts the military ones
	double fArmy = 0.0;
	unsigned int iMilitary = 0;
	turn.iStrikeScan = 0;
	int iLoop = 0;
	for (CvUnit* pUnit = kPlayer.firstUnit(&iLoop); pUnit != NULL; pUnit = kPlayer.nextUnit(&iLoop))
	{
		if (pUnit->isDelayedDeath())
			continue;
		fArmy += ValueAtDamage(OurUnitWorth(*pUnit), pUnit->getDamage(), pUnit->GetMaxHitPoints());
		if (pUnit->IsCombatUnit() || pUnit->IsCanAttackRanged())
			iMilitary++;
		if (pUnit->getDomainType() == DOMAIN_LAND || pUnit->getDomainType() == DOMAIN_SEA)
			turn.iStrikeScan = std::max(turn.iStrikeScan, pUnit->baseMoves(false) * 4);
	}
	double fGoldPerWorth = 0.0;
	ReferenceUnitScales(kPlayer, turn.fTypicalUnitWorth, fGoldPerWorth);
	turn.fArmyScale = ReferenceScale(fArmy, turn.fTypicalUnitWorth, gradeUnitScaleFloorUnits);
	turn.fCityUnit = turn.fArmyScale > 0.0 ?
		turn.weights.afConstant[OPERATIONAL_CONSTANT_CITY_UNITS] * turn.fTypicalUnitWorth / turn.fArmyScale : 0.0;

	//Z_C: our cities, or without one the mean city of the players at war with us
	double fOwnCities = 0.0;
	int iOwnCities = 0;
	for (CvCity* pCity = kPlayer.firstCity(&iLoop); pCity != NULL; pCity = kPlayer.nextCity(&iLoop))
	{
		fOwnCities += CityValue(*pCity, ePlayer);
		iOwnCities++;
	}
	double fTypicalCity = iOwnCities > 0 ? fOwnCities / iOwnCities : 0.0;
	if (iOwnCities == 0)
	{
		double fEnemyCities = 0.0;
		int iEnemyCities = 0;
		for (int iOther = 0; iOther < MAX_PLAYERS; iOther++)
		{
			CvPlayer& kOther = GET_PLAYER(static_cast<PlayerTypes>(iOther));
			if (!kOther.isAlive() || kOther.isBarbarian() || iOther == ePlayer || !GET_TEAM(kPlayer.getTeam()).isAtWar(kOther.getTeam()))
				continue;
			for (CvCity* pCity = kOther.firstCity(&iLoop); pCity != NULL; pCity = kOther.nextCity(&iLoop))
			{
				fEnemyCities += CityValue(*pCity, ePlayer);
				iEnemyCities++;
			}
		}
		fTypicalCity = iEnemyCities > 0 ? fEnemyCities / iEnemyCities : 0.0;
	}
	turn.fCityScale = ReferenceScale(fOwnCities, fTypicalCity, gradeCityScaleFloorCities);

	//B: the military units plus the weight of our land
	double fLand = 0.0;
	for (int iPlot = 0; iPlot < GC.getMap().numPlots(); iPlot++)
	{
		CvPlot* pPlot = GC.getMap().plotByIndex(iPlot);
		if (pPlot && pPlot->getOwner() == ePlayer)
			fLand += GroundPlotWeight(ePlayer, *pPlot);
	}
	turn.fGroundBase = GroundBase(iMilitary, fLand);
}

// Splits each exposed unit's exposure over its threats by their share of the unit's whole danger. Fog and terrain damage
// raise the exposure but belong to no threat, so they keep their share and a unit's shares add up to its exposure only
// when no such damage applies. Spent units count, since a threat stays on them after they act; ExecuteFight recruits
// only the endangered units that can still act. Each visible enemy combat unit then joins the approach list of our
// city nearest by path steps when that path is at most twice its moves plus its range: native's cached city distance,
// which goes around impassable plots, may embark and ignores move costs and units.
void CvTacticalOperationalLoop::BuildThreats(SOperationalTurn& turn)
{
	turn.unitThreats.clear();
	turn.cityThreats.clear();
	turn.cityApproach.clear();
	turn.endangeredUnits.clear();
	CvPlayer& kPlayer = GET_PLAYER(turn.ePlayer);
	vector<SOperationalThreatDamage> threats;
	int iLoop = 0;
	for (CvUnit* pUnit = kPlayer.firstUnit(&iLoop); pUnit != NULL; pUnit = kPlayer.nextUnit(&iLoop))
	{
		if (pUnit->isDelayedDeath() || pUnit->IsCivilianUnit() || pUnit->getDomainType() == DOMAIN_AIR || pUnit->getArmyID() != -1)
			continue;
		CvPlot* pPlot = pUnit->plot();
		if (!pPlot || pPlot->isFriendlyCity(*pUnit))
			continue;
		const int iDanger = GetDangerByThreat(DangerContents(turn.ePlayer, *pPlot), pUnit, SUnitIDValueContainer(), 0,
			AIR_ACTION_ATTACK, threats);
		int iThreatTotal = 0;
		for (size_t i = 0; i < threats.size(); i++)
			iThreatTotal += std::max(0, threats[i].iDamage);
		if (iThreatTotal <= 0)
			continue;
		turn.endangeredUnits.insert(pUnit->GetID());
		const double fExposure = UnitExposure(OurUnitWorth(*pUnit), pUnit->getDamage(), pUnit->GetMaxHitPoints(), iDanger);
		const int iShareTotal = std::max(iDanger, iThreatTotal);
		for (size_t i = 0; i < threats.size(); i++)
		{
			if (threats[i].iDamage <= 0)
				continue;
			SOperationalThreatExposure& threat = threats[i].bCity
				? turn.cityThreats[make_pair(static_cast<int>(threats[i].eOwner), threats[i].iID)]
				: turn.unitThreats[make_pair(static_cast<int>(threats[i].eOwner), threats[i].iID)];
			threat.fExposure += fExposure * threats[i].iDamage / iShareTotal;
			threat.vUnitIDs.push_back(pUnit->GetID());
		}
	}

	const TeamTypes eTeam = kPlayer.getTeam();
	for (int iOther = 0; iOther < MAX_PLAYERS; iOther++)
	{
		CvPlayer& kOther = GET_PLAYER(static_cast<PlayerTypes>(iOther));
		if (iOther == turn.ePlayer || !kOther.isAlive() || GroundSide(turn.ePlayer, static_cast<PlayerTypes>(iOther)) != groundHoldsHostile)
			continue;
		for (CvUnit* pUnit = kOther.firstUnit(&iLoop); pUnit != NULL; pUnit = kOther.nextUnit(&iLoop))
		{
			CvPlot* pPlot = pUnit->plot();
			if (pUnit->isDelayedDeath() || !pPlot || !pPlot->isVisible(eTeam) || pUnit->isInvisible(eTeam, false) ||
				pUnit->getDomainType() == DOMAIN_AIR || !(pUnit->IsCombatUnit() || pUnit->IsCanAttackRanged()))
				continue;
			//native keeps path lengths only for major civs and fakes minors as their capital at half the plot distance,
			//so a minor takes its nearest city by plot distance
			CvCity* pCity = NULL;
			int iDistance = INT_MAX;
			if (kPlayer.isMajorCiv())
			{
				pCity = kPlayer.GetClosestCityByPathLength(pPlot);
				iDistance = kPlayer.GetCityDistancePathLength(pPlot);
			}
			else
			{
				int iCityLoop = 0;
				for (CvCity* pLoopCity = kPlayer.firstCity(&iCityLoop); pLoopCity != NULL; pLoopCity = kPlayer.nextCity(&iCityLoop))
				{
					const int iPlotDistance = plotDistance(*pPlot, *pLoopCity->plot());
					if (iPlotDistance < iDistance)
					{
						pCity = pLoopCity;
						iDistance = iPlotDistance;
					}
				}
			}
			if (!pCity)
				continue;
			const int iReach = 2 * pUnit->baseMoves(false) + (pUnit->IsCanAttackRanged() ? pUnit->GetRange() : 1);
			if (iDistance <= iReach)
				turn.cityApproach[pCity->GetID()].push_back(make_pair(iOther, pUnit->GetID()));
		}
	}

	for (size_t i = 0; i < turn.battlefields.size(); i++)
	{
		SOperationalBattlefield& field = turn.battlefields[i];
		const pair<int, int> key(static_cast<int>(field.eOwner), field.iTargetID);
		if (field.eCenter == OPERATIONAL_CENTER_OUR_CITY)
			continue;
		if (field.eCenter == OPERATIONAL_CENTER_ENEMY_UNIT)
		{
			field.bCanAttackOurUnits = turn.unitThreats.find(key) != turn.unitThreats.end();
			continue;
		}
		field.bCanAttackOurUnits = turn.cityThreats.find(key) != turn.cityThreats.end();
		CvCity* pCity = field.eOwner != NO_PLAYER ? GET_PLAYER(field.eOwner).getCity(field.iTargetID) : NULL;
		CvUnit* pGarrison = pCity ? pCity->GetGarrisonedUnit() : NULL;
		if (pGarrison && turn.unitThreats.find(make_pair(static_cast<int>(pGarrison->getOwner()), pGarrison->GetID())) != turn.unitThreats.end())
			field.bCanAttackOurUnits = true;
	}
}

// Reads the snapshot native took before it dropped targets near better ones, so every enemy unit and city keeps a
// battlefield. Targets that are no longer hostile, such as a city captured earlier in the turn, get none. Zone ownership
// and posture are read once for the turn. Every city of ours then gets a battlefield, safe ones included.
void CvTacticalOperationalLoop::BuildBattlefields(CvTacticalAI& kTactical, SOperationalTurn& turn)
{
	CvPlayer& kPlayer = *kTactical.m_pPlayer;
	const PlayerTypes ePlayer = turn.ePlayer;
	const double fTheaterFloor = turn.weights.afConstant[OPERATIONAL_CONSTANT_THEATER_FLOOR];
	set<pair<int, int> > seenUnits;
	set<int> seenCities;
	for (size_t i = 0; i < kTactical.m_OperationalTargets.size(); i++)
	{
		const CvTacticalTarget& target = kTactical.m_OperationalTargets[i];
		CvPlot* pPlot = GC.getMap().plot(target.GetTargetX(), target.GetTargetY());
		if (!pPlot)
			continue;
		SOperationalBattlefield field;
		const CvUnit* pDefender = NULL;
		if (target.GetTargetType() == AI_TACTICAL_TARGET_ENEMY_COMBAT_UNIT)
		{
			pDefender = pPlot->getBestDefender(NO_PLAYER, ePlayer, NULL, true, true);
			if (!pDefender || pDefender->isDelayedDeath() || GroundSide(ePlayer, pDefender->getOwner()) != groundHoldsHostile ||
				!seenUnits.insert(make_pair(static_cast<int>(pDefender->getOwner()), pDefender->GetID())).second)
				continue;
			field.eCenter = OPERATIONAL_CENTER_ENEMY_UNIT;
			field.eOwner = pDefender->getOwner();
			field.iTargetID = pDefender->GetID();
		}
		else if (target.GetTargetType() == AI_TACTICAL_TARGET_ENEMY_CITY)
		{
			//a city captured earlier this turn is still in the snapshot
			CvCity* pCity = pPlot->getPlotCity();
			if (!pCity || GroundSide(ePlayer, pCity->getOwner()) != groundHoldsHostile || !seenCities.insert(pPlot->GetPlotIndex()).second)
				continue;
			field.eCenter = OPERATIONAL_CENTER_ENEMY_CITY;
			field.eOwner = pCity->getOwner();
			field.iTargetID = pCity->GetID();
		}
		else
			continue;

		field.iID = static_cast<int>(turn.battlefields.size());
		field.iPlotIndex = pPlot->GetPlotIndex();
		field.iZoneID = target.GetDominanceZone();
		const CvTacticalDominanceZone* pZone = kTactical.GetTacticalAnalysisMap()->GetZoneByIDWithoutRefresh(field.iZoneID);
		CvCity* pZoneCity = pZone ? pZone->GetZoneCity() : NULL;
		field.fTheater = fTheaterFloor + (pZoneCity && turn.fCityScale > 0.0 ? CityValue(*pZoneCity, ePlayer) / turn.fCityScale : 0.0);
		field.fGround = pDefender ? TargetGround(ePlayer, *pPlot, pDefender, turn.fGroundBase) : 0.0;
		field.bNearOurCity = kPlayer.GetCityDistanceInPlots(pPlot) < 4;
		//Reinforce marches only into a zone of ours or one neighboring ours, and never into a Withdraw zone
		field.bBorder = pZone && IsBorderZone(kTactical, *pZone);
		field.bWithdraw = pZone && pZone->GetPosture() == TACTICAL_POSTURE_WITHDRAW;
		turn.battlefields.push_back(field);
	}

	int iLoop = 0;
	for (CvCity* pCity = kPlayer.firstCity(&iLoop); pCity != NULL; pCity = kPlayer.nextCity(&iLoop))
		AddOwnCityBattlefield(kTactical, turn, *pCity);
}

// Neighbor IDs resolve through our own map without a refresh. The zone's own HasNeighborZone reads the zone owner's
// map, which may not know our zones and does not exist for unowned zones.
bool CvTacticalOperationalLoop::IsBorderZone(CvTacticalAI& kTactical, const CvTacticalDominanceZone& kZone)
{
	const PlayerTypes ePlayer = kTactical.m_pPlayer->GetID();
	if (kZone.GetOwner() == ePlayer)
		return true;
	const vector<int>& vNeighbors = kZone.GetNeighboringZones();
	for (size_t i = 0; i < vNeighbors.size(); i++)
	{
		const CvTacticalDominanceZone* pNeighbor = kTactical.GetTacticalAnalysisMap()->GetZoneByIDWithoutRefresh(vNeighbors[i]);
		if (pNeighbor && pNeighbor->GetOwner() == ePlayer)
			return true;
	}
	return false;
}

// The city plot lies in the city's land zone; a city without one takes its water zone, which has the city's negative
// ID. Zones are read without a refresh, since the map cannot be rebuilt in the middle of the turn. The theater is our
// city's value over Z_C above the floor.
void CvTacticalOperationalLoop::AddOwnCityBattlefield(CvTacticalAI& kTactical, SOperationalTurn& turn, CvCity& kCity)
{
	const CvTacticalAnalysisMap* pZoneMap = kTactical.GetTacticalAnalysisMap();
	SOperationalBattlefield field;
	field.iID = static_cast<int>(turn.battlefields.size());
	field.eCenter = OPERATIONAL_CENTER_OUR_CITY;
	field.iPlotIndex = kCity.plot()->GetPlotIndex();
	field.eOwner = turn.ePlayer;
	field.iTargetID = kCity.GetID();
	field.iZoneID = pZoneMap->GetDominanceZoneIDWithoutRefresh(field.iPlotIndex);
	if (!pZoneMap->GetZoneByIDWithoutRefresh(field.iZoneID) && pZoneMap->GetZoneByIDWithoutRefresh(-kCity.GetID()))
		field.iZoneID = -kCity.GetID();
	field.fTheater = turn.weights.afConstant[OPERATIONAL_CONSTANT_THEATER_FLOOR] +
		(turn.fCityScale > 0.0 ? CityValue(kCity, turn.ePlayer) / turn.fCityScale : 0.0);
	field.bNearOurCity = true;
	turn.battlefields.push_back(field);
}

// The domains follow the center: a city takes land units, and sea units too when it is coastal; a unit target takes its
// plot's domain. Both offers read the same scan.
bool CvTacticalOperationalLoop::EstimateBattlefield(CvTacticalAI& kTactical, const SOperationalTurn& turn, SOperationalBattlefield& field,
	bool bReinforce)
{
	field.fEstimate = 0.0;
	CvPlot* pPlot = BattlefieldPlot(field);
	if (!pPlot)
		return false;
	const CvCity* pCity = field.eCenter != OPERATIONAL_CENTER_ENEMY_UNIT ? pPlot->getPlotCity() : NULL;
	const bool bLand = pCity != NULL || !pPlot->isWater();
	const bool bSea = pCity ? pCity->isCoastal() : pPlot->isWater();
	SOperationalLocalSet local;
	GatherLocalSet(kTactical, turn.ePlayer, *pPlot, bLand, bSea, local);
	SOperationalCityDamage damage;
	if (!EstimateFight(turn, local, field, damage))
		return false;
	if (bReinforce)
		EstimateReinforce(kTactical, turn, local, damage, field);
	return true;
}

// A unit target is followed to its current plot; a city stays on its plot.
CvPlot* CvTacticalOperationalLoop::BattlefieldPlot(SOperationalBattlefield& field)
{
	if (field.eOwner == NO_PLAYER)
		return NULL;
	if (field.eCenter == OPERATIONAL_CENTER_ENEMY_UNIT)
	{
		CvUnit* pUnit = GET_PLAYER(field.eOwner).getUnit(field.iTargetID);
		if (!pUnit || pUnit->isDelayedDeath() || !pUnit->plot())
			return NULL;
		field.iPlotIndex = pUnit->plot()->GetPlotIndex();
		return pUnit->plot();
	}
	CvCity* pCity = GET_PLAYER(field.eOwner).getCity(field.iTargetID);
	return pCity ? pCity->plot() : NULL;
}

// Unit fight: weighted prize, ground and exposure share. Enemy-city fight: the weighted city unit times the attack fall
// share from the local damage simulation, the weighted garrison prize times the garrison damage share, and the
// exposure share of the city and its garrison. Own-city fight: the weighted city unit times the defense fall share from
// native city danger, plus the weighted exposure of its endangered recruits. Terms over a zero scale count 0.
bool CvTacticalOperationalLoop::EstimateFight(const SOperationalTurn& turn, const SOperationalLocalSet& local, SOperationalBattlefield& field,
	SOperationalCityDamage& damage)
{
	const double* afWeight = turn.weights.afWeight;
	const double fPrizeFloor = turn.weights.afConstant[OPERATIONAL_CONSTANT_PRIZE_FLOOR];
	const double fArmyScale = turn.fArmyScale;
	SOperationalStake stake;
	damage = SOperationalCityDamage();
	field.fEstimate = 0.0;
	if (field.eOwner == NO_PLAYER)
		return false;
	if (field.eCenter == OPERATIONAL_CENTER_ENEMY_UNIT)
	{
		CvUnit* pUnit = GET_PLAYER(field.eOwner).getUnit(field.iTargetID);
		if (!pUnit || pUnit->isDelayedDeath() || !pUnit->plot())
			return false;
		field.iPlotIndex = pUnit->plot()->GetPlotIndex();
		map<pair<int, int>, SOperationalThreatExposure>::const_iterator threat = turn.unitThreats.find(make_pair(static_cast<int>(field.eOwner), field.iTargetID));
		const double fExposure = threat != turn.unitThreats.end() ? threat->second.fExposure : 0.0;
		if (fArmyScale > 0.0)
		{
			stake.fPrize = afWeight[OPERATIONAL_WEIGHT_ATTRITION] * UnitPrize(*pUnit, fPrizeFloor) / fArmyScale;
			stake.fExposure = afWeight[OPERATIONAL_WEIGHT_RISK] * fExposure / fArmyScale;
		}
		stake.fGround = afWeight[OPERATIONAL_WEIGHT_HOLD_GROUND] * field.fGround;
		stake.fStake = stake.fPrize + stake.fGround + stake.fExposure;
	}
	else if (field.eCenter == OPERATIONAL_CENTER_OUR_CITY)
	{
		CvCity* pCity = GET_PLAYER(field.eOwner).getCity(field.iTargetID);
		if (!pCity || pCity->getOwner() != turn.ePlayer)
			return false;
		field.fLocalStrength = LocalCityStrength(*pCity, local);
		field.fEnemyStrength = EnemyCityStrength(turn, *pCity, &field.reinforce.countedUnits);

		//native danger is the worst case the visible enemies can deal before our next turn, so no heal or capturer applies
		const int iRemaining = std::max(1, pCity->GetMaxHitPoints() - pCity->getDamage());
		stake.fFallShare = std::min(1.0, static_cast<double>(GET_PLAYER(turn.ePlayer).GetPlotDanger(pCity)) / iRemaining);

		//a garrison whose city can fall has danger past its HP, so it counts at its whole value
		vector<CvUnit*> vEndangered;
		GatherCityEndangered(*GET_PLAYER(turn.ePlayer).GetTacticalAI(), turn, *pCity, vEndangered);
		double fExposure = 0.0;
		for (size_t i = 0; i < vEndangered.size(); i++)
			fExposure += UnitExposure(OurUnitWorth(*vEndangered[i]), vEndangered[i]->getDamage(), vEndangered[i]->GetMaxHitPoints(),
				vEndangered[i]->GetDanger());
		field.bHasEndangered = !vEndangered.empty();

		stake.fCityProgress = afWeight[OPERATIONAL_WEIGHT_HOLD_CITY] * turn.fCityUnit * stake.fFallShare;
		if (fArmyScale > 0.0)
			stake.fExposure = afWeight[OPERATIONAL_WEIGHT_RISK] * fExposure / fArmyScale;
		stake.fStake = stake.fCityProgress + stake.fExposure;
	}
	else
	{
		CvCity* pCity = GET_PLAYER(field.eOwner).getCity(field.iTargetID);
		if (!pCity || pCity->getOwner() != field.eOwner)
			return false;

		//the local attackers in ascending ID; the city term takes the damage left after absorption and healing
		StartCityDamage(*pCity, damage);
		for (size_t i = 0; i < local.vDamage.size(); i++)
		{
			int iCityDamage = 0;
			int iGarrisonDamage = 0;
			SimulateCityAttack(*pCity, *local.vDamage[i], damage, iCityDamage, iGarrisonDamage);
		}
		stake.fFallShare = AttackFallShare(damage.iCityDamage, damage.iHeal, damage.iRemaining, local.bCanCapture);
		stake.fGarrisonShare = GarrisonShare(damage.iGarrisonDamage, damage.iGarrisonHealth, stake.fFallShare >= 1.0);
		stake.fCityProgress = afWeight[OPERATIONAL_WEIGHT_OCCUPATION] * turn.fCityUnit * stake.fFallShare;

		CvUnit* pGarrison = pCity->GetGarrisonedUnit();
		double fExposure = 0.0;
		map<pair<int, int>, SOperationalThreatExposure>::const_iterator threat = turn.cityThreats.find(make_pair(static_cast<int>(field.eOwner), field.iTargetID));
		if (threat != turn.cityThreats.end())
			fExposure += threat->second.fExposure;
		if (pGarrison)
		{
			threat = turn.unitThreats.find(make_pair(static_cast<int>(pGarrison->getOwner()), pGarrison->GetID()));
			if (threat != turn.unitThreats.end())
				fExposure += threat->second.fExposure;
		}
		if (fArmyScale > 0.0)
		{
			if (damage.pGarrison)
				stake.fGarrison = afWeight[OPERATIONAL_WEIGHT_ATTRITION] * UnitPrize(*damage.pGarrison, fPrizeFloor) / fArmyScale *
					stake.fGarrisonShare;
			stake.fExposure = afWeight[OPERATIONAL_WEIGHT_RISK] * fExposure / fArmyScale;
		}
		stake.fStake = stake.fCityProgress + stake.fGarrison + stake.fExposure;
	}
	field.stake = stake;
	field.fEstimate = field.fTheater * stake.fStake;
	return true;
}

// Enemy centers count their strength first, so a gated offer still records L and T, and skip the roster when gated. At
// an enemy city the share difference takes gain's place, so a march never outranks the fight it sets up.
void CvTacticalOperationalLoop::EstimateReinforce(CvTacticalAI& kTactical, const SOperationalTurn& turn, const SOperationalLocalSet& local,
	const SOperationalCityDamage& damage, SOperationalBattlefield& field)
{
	SOperationalReinforce& reinforce = field.reinforce;
	field.fReinforceEstimate = 0.0;
	//start from an empty selection
	vector<SOperationalCandidate> vCandidates;
	SelectReinforcement(vCandidates, 0.0, 0.0, 0.0, false, reinforce);
	CvPlot* pTarget = GC.getMap().plotByIndex(field.iPlotIndex);
	if (!pTarget)
		return;
	const double* afWeight = turn.weights.afWeight;
	const double fRatio = turn.weights.afConstant[OPERATIONAL_CONSTANT_STRENGTH_RATIO];
	const bool bCityTarget = field.eCenter != OPERATIONAL_CENTER_ENEMY_UNIT;
	CvCity* pCity = bCityTarget ? pTarget->getPlotCity() : NULL;
	if (bCityTarget && !pCity)
		return;

	if (field.eCenter == OPERATIONAL_CENTER_OUR_CITY)
	{
		//EstimateFight just counted L and T, and our city's enemies
		GatherReinforceCandidates(kTactical, field, *pTarget, local.bLand, local.bSea, vCandidates);
		SelectReinforcement(vCandidates, field.fLocalStrength, field.fEnemyStrength, fRatio, false, reinforce);
		field.fReinforceEstimate = field.fTheater * afWeight[OPERATIONAL_WEIGHT_HOLD_CITY] * turn.fCityUnit * reinforce.fGain;
		return;
	}

	const double fLocal = LocalStrength(local);
	reinforce.countedUnits.clear();
	reinforce.countedCities.clear();
	const double fEnemy = EnemyCenterStrength(turn.ePlayer, *pTarget, local.bLand, local.bSea, reinforce.countedUnits, reinforce.countedCities);
	if (!field.bBorder || field.bWithdraw)
	{
		SelectReinforcement(vCandidates, fLocal, fEnemy, fRatio, false, reinforce);
		return;
	}
	GatherReinforceCandidates(kTactical, field, *pTarget, local.bLand, local.bSea, vCandidates);
	SelectReinforcement(vCandidates, fLocal, fEnemy, fRatio, pCity != NULL && !local.bCapturer, reinforce);
	if (pCity)
		ProjectCityShares(kTactical, *pCity, local, damage, vCandidates, fRatio, reinforce);
	//parity trim: no march that would arrive below even strength
	if (reinforce.fArrivalRatio < 1.0)
		return;
	if (!pCity)
	{
		field.fReinforceEstimate = field.fTheater * field.stake.fStake * reinforce.fGain * reinforce.fOdds;
		return;
	}
	if (reinforce.vSelected.empty())
		return;
	const double fGarrisonPrize = damage.pGarrison && turn.fArmyScale > 0.0 ? afWeight[OPERATIONAL_WEIGHT_ATTRITION] *
		UnitPrize(*damage.pGarrison, turn.weights.afConstant[OPERATIONAL_CONSTANT_PRIZE_FLOOR]) / turn.fArmyScale : 0.0;
	const double fCityRise = std::max(0.0, reinforce.fProjectedFall - reinforce.fBaseFall);
	const double fGarrisonRise = std::max(0.0, reinforce.fProjectedGarrison - reinforce.fBaseGarrison);
	field.fReinforceEstimate = field.fTheater * (afWeight[OPERATIONAL_WEIGHT_OCCUPATION] * turn.fCityUnit * fCityRise +
		fGarrisonPrize * fGarrisonRise) * reinforce.fOdds;
}

// Each incoming hit is divided by its join turns, the discount help uses. The capture requirement is met by a local
// capturer, spent or not, or by a selected one. The base shares apply the same rule to the local damage alone, so the
// march is credited only with what its damage or its capture role adds. A unit that recruitment would not take against
// the city adds no damage. The extension trial copies the simulation, so a unit it passes over leaves no trace.
void CvTacticalOperationalLoop::ProjectCityShares(CvTacticalAI& kTactical, const CvCity& kCity, const SOperationalLocalSet& local,
	const SOperationalCityDamage& damage, const vector<SOperationalCandidate>& vCandidates, double fRatio,
	SOperationalReinforce& reinforce)
{
	CvPlayer& kPlayer = *kTactical.m_pPlayer;
	map<int, const SOperationalCandidate*> candidateByID;
	for (size_t i = 0; i < vCandidates.size(); i++)
		candidateByID[vCandidates[i].iUnitID] = &vCandidates[i];

	SOperationalCityDamage simulation = damage;
	double fCityDamage = damage.iCityDamage;
	double fGarrisonDamage = damage.iGarrisonDamage;
	bool bCapture = local.bCanCapture || local.bCapturer;
	reinforce.fBaseFall = AttackFallShare(fCityDamage, damage.iHeal, damage.iRemaining, bCapture);
	reinforce.fBaseGarrison = GarrisonShare(fGarrisonDamage, damage.iGarrisonHealth, reinforce.fBaseFall >= 1.0);
	for (size_t i = 0; i < reinforce.vSelected.size(); i++)
	{
		map<int, const SOperationalCandidate*>::const_iterator it = candidateByID.find(reinforce.vSelected[i]);
		CvUnit* pUnit = kPlayer.getUnit(reinforce.vSelected[i]);
		if (it == candidateByID.end() || !pUnit)
			continue;
		const SOperationalCandidate& candidate = *it->second;
		bCapture = bCapture || candidate.bCapturer;
		int iCityDamage = 0;
		int iGarrisonDamage = 0;
		if (CanStrikeCity(kTactical, *pUnit, kCity) && SimulateCityAttack(kCity, *pUnit, simulation, iCityDamage, iGarrisonDamage))
		{
			fCityDamage += static_cast<double>(iCityDamage) / std::max(1, candidate.iJoinTurns);
			fGarrisonDamage += static_cast<double>(iGarrisonDamage) / std::max(1, candidate.iJoinTurns);
		}
	}
	reinforce.fProjectedFall = AttackFallShare(fCityDamage, damage.iHeal, damage.iRemaining, bCapture);
	reinforce.fProjectedGarrison = GarrisonShare(fGarrisonDamage, damage.iGarrisonHealth, reinforce.fProjectedFall >= 1.0);
	if (reinforce.fBaseFall >= 1.0 || reinforce.fProjectedFall > reinforce.fBaseFall || reinforce.fProjectedGarrison > reinforce.fBaseGarrison)
		return;

	//damage extension: strength alone may have picked units that add nothing to the siege
	vector<SOperationalCandidate> sorted = vCandidates;
	std::sort(sorted.begin(), sorted.end(), PrOperationalCandidateNearest());
	vector<int> vAdded;
	double fAddedStrength = 0.0;
	double fAddedHelp = 0.0;
	bool bAddedCapturer = false;
	for (size_t i = 0; i < sorted.size(); i++)
	{
		const SOperationalCandidate& candidate = sorted[i];
		if (std::find(reinforce.vSelected.begin(), reinforce.vSelected.end(), candidate.iUnitID) != reinforce.vSelected.end())
			continue;
		CvUnit* pUnit = kPlayer.getUnit(candidate.iUnitID);
		if (!pUnit || !CanStrikeCity(kTactical, *pUnit, kCity))
			continue;
		SOperationalCityDamage trial = simulation;
		int iCityDamage = 0;
		int iGarrisonDamage = 0;
		if (!SimulateCityAttack(kCity, *pUnit, trial, iCityDamage, iGarrisonDamage) || (iCityDamage <= 0 && iGarrisonDamage <= 0))
			continue;
		simulation = trial;
		bCapture = bCapture || candidate.bCapturer;
		fCityDamage += static_cast<double>(iCityDamage) / std::max(1, candidate.iJoinTurns);
		fGarrisonDamage += static_cast<double>(iGarrisonDamage) / std::max(1, candidate.iJoinTurns);
		vAdded.push_back(candidate.iUnitID);
		bAddedCapturer = bAddedCapturer || candidate.bCapturer;
		fAddedStrength += candidate.fStrength;
		fAddedHelp += candidate.fStrength / std::max(1, candidate.iJoinTurns);
		const double fFall = AttackFallShare(fCityDamage, damage.iHeal, damage.iRemaining, bCapture);
		const double fGarrison = GarrisonShare(fGarrisonDamage, damage.iGarrisonHealth, fFall >= 1.0);
		if (fFall > reinforce.fBaseFall || fGarrison > reinforce.fBaseGarrison)
		{
			reinforce.vSelected.insert(reinforce.vSelected.end(), vAdded.begin(), vAdded.end());
			reinforce.iExtended = static_cast<int>(vAdded.size());
			reinforce.fSelectedStrength += fAddedStrength;
			reinforce.fHelp += fAddedHelp;
			reinforce.fProjectedFall = fFall;
			reinforce.fProjectedGarrison = fGarrison;
			reinforce.bMissingRole = reinforce.bMissingRole && !bAddedCapturer;
			ScoreReinforcement(fRatio, reinforce);
			return;
		}
	}
}

// Reads the tactical roster, as native recruitment does. The filters are those of CvTacticalAI::PlotReinforcementMoves
// that need no pathfinding: the alternative-target filter waits for the readiness check. Changes: one-turn and recruit
// range bounds by plain distance; destination dominance gates apply to enemy centers only; units covering a civilian
// are left out without native's skip and processing; generals, admirals and civilian sappers fall to the combat test.
void CvTacticalOperationalLoop::GatherReinforceCandidates(CvTacticalAI& kTactical, const SOperationalBattlefield& field, const CvPlot& kTarget,
	bool bLand, bool bSea, vector<SOperationalCandidate>& vCandidates)
{
	vCandidates.clear();
	const CvTacticalAnalysisMap* pZoneMap = kTactical.GetTacticalAnalysisMap();
	const CvTacticalDominanceZone* pTargetZone = pZoneMap->GetZoneByIDWithoutRefresh(field.iZoneID);
	const CvCity* pTargetCity = pTargetZone ? pTargetZone->GetZoneCity() : NULL;
	const bool bOurCity = field.eCenter == OPERATIONAL_CENTER_OUR_CITY;
	const bool bFriendlyTerritory = pTargetZone && pTargetZone->GetTerritoryType() == TACTICAL_TERRITORY_FRIENDLY;
	const bool bEnemyDominated = !bOurCity && pTargetZone && pTargetZone->GetOverallDominanceFlag() == TACTICAL_DOMINANCE_ENEMY;
	const int iRecruitRange = kTactical.GetRecruitRange();
	for (list<int>::const_iterator it = kTactical.m_CurrentTurnUnits.begin(); it != kTactical.m_CurrentTurnUnits.end(); ++it)
	{
		CvUnit* pUnit = CityRecruit(kTactical, *it);
		if (!pUnit || !pUnit->canUseNow() || !pUnit->IsCombatUnit() || field.reinforce.rejected.count(pUnit->GetID()) > 0)
			continue;
		if (!((bLand && pUnit->getDomainType() == DOMAIN_LAND) || (bSea && pUnit->getDomainType() == DOMAIN_SEA)))
			continue;

		//local units count in L; a held garrison stays search-only, so it joins only within the search's reach
		const int iDistance = plotDistance(*pUnit->plot(), kTarget);
		const bool bHeld = IsHeldGarrison(pUnit);
		if (iDistance <= StrikeReach(*pUnit) ||
			iDistance > (bHeld ? std::min(iRecruitRange, static_cast<int>(TACTICAL_COMBAT_MAX_TARGET_DISTANCE)) : iRecruitRange))
			continue;

		// Carriers have special moves
		if (pUnit->AI_getUnitAIType() == UNITAI_CARRIER_SEA)
			continue;

		// Do not move siege units into enemy dominated zones ... wait until we have some cover!
		if (bEnemyDominated && pUnit->AI_getUnitAIType() == UNITAI_CITY_BOMBARD)
			continue;

		//conversely, don't leave generals out in the cold
		if (pUnit->IsCoveringFriendlyCivilian())
			continue;

		// don't use near-dead units to attack ... misuse the flag here to be more careful when attacking
		if (pUnit->shouldHeal(bFriendlyTerritory))
			continue;

		//we should not pull units from zones which need them
		const CvTacticalDominanceZone* pUnitZone = pZoneMap->GetZoneByIDWithoutRefresh(pZoneMap->GetDominanceZoneIDWithoutRefresh(pUnit->plot()->GetPlotIndex()));
		const bool bTargetZone = pUnitZone && (pUnitZone->GetZoneID() == field.iZoneID || (pTargetCity && pUnitZone->GetZoneCity() == pTargetCity));
		if (pUnitZone && !bTargetZone)
		{
			if (pUnitZone->GetOverallDominanceFlag() != TACTICAL_DOMINANCE_FRIENDLY && pUnitZone->GetPosture() != TACTICAL_POSTURE_WITHDRAW)
				if (!pUnit->plot()->isCity() || pUnit->plot()->getPlotCity()->isInDangerOfFalling() || pUnit->getDomainType() == DOMAIN_SEA)
					if (!pUnit->isEmbarked()) //cannot fight if embarked, so we can take it!
						continue;
		}

		SOperationalCandidate candidate;
		candidate.iUnitID = pUnit->GetID();
		candidate.fStrength = UnitStrength(*pUnit);
		candidate.iTravelTurns = TravelTurns(iDistance, pUnit->baseMoves(false));
		candidate.iJoinTurns = std::max(1, candidate.iTravelTurns - 1);
		candidate.bCapturer = IsCapturer(*pUnit);
		candidate.bHeldGarrison = bHeld;
		vCandidates.push_back(candidate);
	}
}

//------------------------------------------------------------------------------
// strength and our cities' recruits
//------------------------------------------------------------------------------

// Native strengths are scaled by 100 and already include the damage modifier; the HP share applies on top.
double CvTacticalOperationalLoop::UnitStrength(const CvUnit& kUnit)
{
	if (kUnit.GetMaxHitPoints() <= 0)
		return 0.0;
	const int iStrength = kUnit.IsCanAttackRanged() ? kUnit.GetMaxRangedCombatStrength(NULL, NULL, true, NULL, NULL, true, true)
		: kUnit.GetMaxAttackStrength(NULL, NULL, NULL, true, true);
	return static_cast<double>(iStrength) * kUnit.GetCurrHitPoints() / kUnit.GetMaxHitPoints();
}

// The quick estimates keep the city and terrain modifiers of the city plot and skip adjacency.
double CvTacticalOperationalLoop::UnitStrengthAgainstCity(const CvUnit& kUnit, const CvCity& kCity)
{
	if (kUnit.GetMaxHitPoints() <= 0)
		return 0.0;
	const int iStrength = kUnit.IsCanAttackRanged() ? kUnit.GetMaxRangedCombatStrength(NULL, &kCity, true, NULL, kCity.plot(), true, true)
		: kUnit.GetMaxAttackStrength(NULL, kCity.plot(), NULL, true, true);
	return static_cast<double>(iStrength) * kUnit.GetCurrHitPoints() / kUnit.GetMaxHitPoints();
}

// The city's strength value, which counts its garrison, times its remaining HP share.
double CvTacticalOperationalLoop::CityStrength(const CvCity& kCity)
{
	if (kCity.GetMaxHitPoints() <= 0)
		return 0.0;
	return static_cast<double>(kCity.getStrengthValue()) * (kCity.GetMaxHitPoints() - kCity.getDamage()) / kCity.GetMaxHitPoints();
}

// The strength filter's units at their current strength.
double CvTacticalOperationalLoop::LocalStrength(const SOperationalLocalSet& local)
{
	double fStrength = 0.0;
	for (size_t i = 0; i < local.vStrength.size(); i++)
		fStrength += UnitStrength(*local.vStrength[i]);
	return fStrength;
}

// The city's own strength plus the units around it.
double CvTacticalOperationalLoop::LocalCityStrength(const CvCity& kCity, const SOperationalLocalSet& local)
{
	return CityStrength(kCity) + LocalStrength(local);
}

// A ranged unit can fire from its range beyond where it moves.
int CvTacticalOperationalLoop::StrikeReach(const CvUnit& kUnit)
{
	return std::max(1, kUnit.baseMoves(false)) + (kUnit.IsCanAttackRanged() ? kUnit.GetRange() : 0);
}

// Orders units by ascending ID, the order local damage is simulated in
struct PrOperationalUnitID
{
	// Ranks by unit ID.
	bool operator()(const CvUnit* lhs, const CvUnit* rhs) const
	{
		return lhs->GetID() < rhs->GetID();
	}
};

// Plain distance, no pathfinding. Land and sea units in the domains join within their strike reach; aircraft join the
// damage filter when their range reaches the center from their base. A unit enters the damage filter when it is
// on the tactical roster and has an attack left, and passes the recruitment filters of
// CvTacticalAI::FindUnitsWithinStrikingDistance that need no pathfinding, so readiness can recruit every unit the
// estimate counts.
void CvTacticalOperationalLoop::GatherLocalSet(CvTacticalAI& kTactical, PlayerTypes ePlayer, const CvPlot& kPlot, bool bLand, bool bSea,
	SOperationalLocalSet& local)
{
	local = SOperationalLocalSet();
	local.bLand = bLand;
	local.bSea = bSea;
	CvPlayer& kPlayer = GET_PLAYER(ePlayer);
	const set<int> roster(kTactical.m_CurrentTurnUnits.begin(), kTactical.m_CurrentTurnUnits.end());
	const CvCity* pCity = kPlot.getPlotCity();
	int iLoop = 0;
	for (CvUnit* pUnit = kPlayer.firstUnit(&iLoop); pUnit != NULL; pUnit = kPlayer.nextUnit(&iLoop))
	{
		if (pUnit->isDelayedDeath() || !pUnit->plot() || !(pUnit->IsCombatUnit() || pUnit->IsCanAttackRanged()))
			continue;
		const int iDistance = plotDistance(*pUnit->plot(), kPlot);
		if (pUnit->getDomainType() == DOMAIN_AIR)
		{
			if (iDistance > pUnit->GetRange())
				continue;
		}
		else
		{
			if (!((bLand && pUnit->getDomainType() == DOMAIN_LAND) || (bSea && pUnit->getDomainType() == DOMAIN_SEA)))
				continue;
			if (iDistance > StrikeReach(*pUnit))
				continue;
			local.vStrength.push_back(pUnit);
			local.bCapturer = local.bCapturer || IsCapturer(*pUnit);
		}
		if (roster.count(pUnit->GetID()) > 0 && pUnit->canUseForTacticalAI() && !pUnit->isOutOfAttacks() && !pUnit->IsCoveringFriendlyCivilian() &&
			pUnit->getUnitInfo().GetDefaultUnitAIType() != UNITAI_DEFENSE_AIR && (!pCity || CanStrikeCity(kTactical, *pUnit, *pCity)))
		{
			local.vDamage.push_back(pUnit);
			local.bCanCapture = local.bCanCapture || CanCapture(*pUnit);
		}
	}
	std::sort(local.vDamage.begin(), local.vDamage.end(), PrOperationalUnitID());
}

// The unit-specific city filters in CvTacticalAI::FindUnitsWithinStrikingDistance: no capture prohibition, and a ranged
// attack from the unit's plot that meets native's minimum damage of min(garrison HP / 20, 3).
bool CvTacticalOperationalLoop::CanStrikeCity(CvTacticalAI& kTactical, CvUnit& kUnit, const CvCity& kCity)
{
	if (kUnit.isNoCapture())
		return false;
	if (!kUnit.IsCanAttackRanged())
		return true;
	CvPlot* pPlot = kCity.plot();
	const CvUnit* pDefender = pPlot->getBestDefender(NO_PLAYER, kUnit.getOwner(), NULL, false, true);
	const int iTargetHitpoints = pDefender ? pDefender->GetCurrHitPoints() : 0;
	return kTactical.IsExpectedToDamageWithRangedAttack(&kUnit, pPlot, std::min(iTargetHitpoints / 20, 3));
}

// Copy of the city heal in CvCity::doTurn. Changes: returns the heal without applying it; leaves out the tripling for a
// city that took no damage last turn, since our damage would land this turn; ignores whether the city is damaged now.
int CvTacticalOperationalLoop::ExpectedCityHeal(const CvCity& kCity)
{
	if (kCity.IsBlockadedWaterAndLand())
		return 0;
	int iHitsHealed = /*20 in CP, 8 in VP*/ GD_INT_GET(CITY_HIT_POINTS_HEALED_PER_TURN);

	int iBuildingDefense = kCity.GetCityBuildings()->GetBuildingDefense();
	iBuildingDefense *= (100 + kCity.GetCityBuildings()->GetBuildingDefenseMod());
	iBuildingDefense /= 100;

	iHitsHealed += iBuildingDefense / 1000;

	if (MOD_BALANCE_VP)
		iHitsHealed += kCity.getPopulation();

	if (kCity.getProductionProcess() != NO_PROCESS)
	{
		CvProcessInfo* pkProcessInfo = GC.getProcessInfo(kCity.getProductionProcess());
		if (pkProcessInfo && pkProcessInfo->getDefenseValue() != 0)
		{
			int iPile = kCity.getYieldRateTimes100(YIELD_PRODUCTION) * pkProcessInfo->getDefenseValue();
			iHitsHealed += iPile / 10000;
		}
	}
	return iHitsHealed;
}

// The garrison is read once here, so the garrison term and D_g refer to the garrison present at the start.
void CvTacticalOperationalLoop::StartCityDamage(const CvCity& kCity, SOperationalCityDamage& damage)
{
	damage = SOperationalCityDamage();
	damage.pCurrent = kCity.GetGarrisonedUnit();
	damage.pGarrison = damage.pCurrent;
	damage.iGarrisonHealth = damage.pCurrent ? damage.pCurrent->GetCurrHitPoints() : 0;
	damage.iCurrentHealth = damage.iGarrisonHealth;
	damage.iRemaining = std::max(1, kCity.GetMaxHitPoints() - kCity.getDamage());
	damage.iHeal = ExpectedCityHeal(kCity);
}

// Copy of the city case in CvTacticalAI::ComputeTotalExpectedDamage for one attacker. Changes: the quick estimate; the
// counted totals are kept here; hits on a replacement garrison add city damage only, so D_g stays with the garrison the
// estimate started with; the expected-damage bookkeeping on the move list is left out; a suicidal attack changes no
// garrison state, and a garrison predicted to die is never met again as a replacement.
bool CvTacticalOperationalLoop::SimulateCityAttack(const CvCity& kCity, const CvUnit& kAttacker, SOperationalCityDamage& damage,
	int& iCityDamage, int& iGarrisonDamage)
{
	int iSelfDamage = 0;
	iGarrisonDamage = 0;
	const bool bStartingGarrison = damage.pCurrent != NULL && damage.pCurrent == damage.pGarrison;
	//attacker plot will likely change but this is just an estimation anyway
	iCityDamage = TacticalAIHelpers::GetSimulatedDamageFromAttackOnCity(&kCity, &kAttacker, kAttacker.plot(), iSelfDamage, iGarrisonDamage, true, 0,
		damage.iCityDamage, damage.iTotalGarrisonDamage, true, true, damage.pCurrent);

	//exclude suicidal melee attacks
	if (!(iCityDamage + iGarrisonDamage > iSelfDamage ||
		((iCityDamage + iGarrisonDamage) * 2 > iSelfDamage && kAttacker.GetCurrHitPoints() - iSelfDamage > kAttacker.GetMaxHitPoints() / 2)))
	{
		iCityDamage = 0;
		iGarrisonDamage = 0;
		return false;
	}

	damage.iTotalGarrisonDamage += iGarrisonDamage;
	if (damage.pCurrent)
	{
		damage.iCurrentHealth -= iGarrisonDamage;
		if (damage.iCurrentHealth <= 0)
		{
			//getBestDefender skips only the unit just killed, so an earlier predicted death is skipped here
			damage.vKilled.push_back(damage.pCurrent->GetID());
			damage.pCurrent = kCity.plot()->getBestDefender(kCity.getOwner(), kAttacker.getOwner(), NULL, true, true, false, false, damage.pCurrent);
			if (damage.pCurrent && std::find(damage.vKilled.begin(), damage.vKilled.end(), damage.pCurrent->GetID()) != damage.vKilled.end())
				damage.pCurrent = NULL;
			damage.iCurrentHealth = damage.pCurrent ? damage.pCurrent->GetCurrHitPoints() : 0;
			damage.iTotalGarrisonDamage = 0;
		}
	}
	if (!bStartingGarrison)
		iGarrisonDamage = 0;
	damage.iCityDamage += iCityDamage;
	damage.iGarrisonDamage += iGarrisonDamage;
	return true;
}

// Native danger lists the visible enemy units that can attack the city plot; a unit on both lists counts once.
double CvTacticalOperationalLoop::EnemyCityStrength(const SOperationalTurn& turn, const CvCity& kCity, set<pair<int, int> >* pCounted)
{
	set<pair<int, int> > counted;
	double fStrength = 0.0;
	map<int, vector<pair<int, int> > >::const_iterator approach = turn.cityApproach.find(kCity.GetID());
	if (approach != turn.cityApproach.end())
	{
		for (size_t i = 0; i < approach->second.size(); i++)
		{
			const CvUnit* pUnit = GET_PLAYER(static_cast<PlayerTypes>(approach->second[i].first)).getUnit(approach->second[i].second);
			if (pUnit && !pUnit->isDelayedDeath() && counted.insert(approach->second[i]).second)
				fStrength += UnitStrengthAgainstCity(*pUnit, kCity);
		}
	}
	CvPlayer& kPlayer = GET_PLAYER(turn.ePlayer);
	const vector<CvUnit*> vAttackers = kPlayer.GetPossibleAttackers(*kCity.plot(), kPlayer.getTeam());
	for (size_t i = 0; i < vAttackers.size(); i++)
	{
		const CvUnit* pUnit = vAttackers[i];
		if (!pUnit || pUnit->getDomainType() == DOMAIN_AIR || !(pUnit->IsCombatUnit() || pUnit->IsCanAttackRanged()) ||
			GroundSide(turn.ePlayer, pUnit->getOwner()) != groundHoldsHostile)
			continue;
		if (counted.insert(make_pair(static_cast<int>(pUnit->getOwner()), pUnit->GetID())).second)
			fStrength += UnitStrengthAgainstCity(*pUnit, kCity);
	}
	if (pCounted)
		pCounted->swap(counted);
	return fStrength;
}

// Each plot within 4 is read once: a hostile city adds its strength with at least 1 HP, which already counts its
// garrison; other visible hostile units add their own strength.
double CvTacticalOperationalLoop::EnemyCenterStrength(PlayerTypes ePlayer, const CvPlot& kTarget, bool bLand, bool bSea,
	set<pair<int, int> >& countedUnits, set<pair<int, int> >& countedCities)
{
	countedUnits.clear();
	countedCities.clear();
	const TeamTypes eTeam = GET_PLAYER(ePlayer).getTeam();
	double fStrength = 0.0;
	for (int i = 0; i < RING_PLOTS[4]; i++)
	{
		const CvPlot* pPlot = iterateRingPlots(&kTarget, i);
		if (!pPlot || !pPlot->isVisible(eTeam))
			continue;
		const CvCity* pCity = pPlot->getPlotCity();
		const CvUnit* pGarrison = NULL;
		if (pCity && GroundSide(ePlayer, pCity->getOwner()) == groundHoldsHostile && pCity->GetMaxHitPoints() > 0)
		{
			countedCities.insert(make_pair(static_cast<int>(pCity->getOwner()), pCity->GetID()));
			fStrength += static_cast<double>(pCity->getStrengthValue()) * std::max(1, pCity->GetMaxHitPoints() - pCity->getDamage()) /
				pCity->GetMaxHitPoints();
			pGarrison = pCity->GetGarrisonedUnit();
		}
		for (int j = 0; j < pPlot->getNumUnits(); j++)
		{
			const CvUnit* pUnit = pPlot->getUnitByIndex(j);
			if (!pUnit || pUnit == pGarrison || pUnit->isDelayedDeath() || pUnit->isInvisible(eTeam, false) ||
				!(pUnit->IsCombatUnit() || pUnit->IsCanAttackRanged()) || GroundSide(ePlayer, pUnit->getOwner()) != groundHoldsHostile)
				continue;
			if (!((bLand && pUnit->getDomainType() == DOMAIN_LAND) || (bSea && pUnit->getDomainType() == DOMAIN_SEA)))
				continue;
			if (countedUnits.insert(make_pair(static_cast<int>(pUnit->getOwner()), pUnit->GetID())).second)
				fStrength += UnitStrength(*pUnit);
		}
	}
	return fStrength;
}

// The roster unit a city fight can recruit, as native recruitment reads it, so units of operations and armies stay
// out; NULL for any other unit.
CvUnit* CvTacticalOperationalLoop::CityRecruit(CvTacticalAI& kTactical, int iUnitID)
{
	CvUnit* pUnit = kTactical.m_pPlayer->getUnit(iUnitID);
	if (!pUnit || !pUnit->canUseForTacticalAI() || pUnit->getArmyID() != -1 || !pUnit->plot())
		return NULL;
	return pUnit;
}

// A non-air unit near the city that a threat can damage next turn, or that stands in a friendly city in danger, is
// endangered. Units that cover a friendly civilian can join only this way.
void CvTacticalOperationalLoop::GatherCityEndangered(CvTacticalAI& kTactical, const SOperationalTurn& turn, const CvCity& kCity,
	vector<CvUnit*>& vEndangered)
{
	vEndangered.clear();
	const CvPlot* pCityPlot = kCity.plot();
	for (list<int>::const_iterator it = kTactical.m_CurrentTurnUnits.begin(); it != kTactical.m_CurrentTurnUnits.end(); ++it)
	{
		CvUnit* pUnit = CityRecruit(kTactical, *it);
		if (!pUnit || pUnit->getDomainType() == DOMAIN_AIR || plotDistance(*pUnit->plot(), *pCityPlot) > TACTICAL_COMBAT_MAX_TARGET_DISTANCE)
			continue;
		if (turn.endangeredUnits.find(pUnit->GetID()) != turn.endangeredUnits.end() ||
			(pUnit->plot()->isFriendlyCity(*pUnit) && pUnit->GetDanger() > 0))
			vEndangered.push_back(pUnit);
	}
}

// A defender is a land combat unit, or a sea one at a coastal city, within its strike reach; the held garrison stands on the
// city. Under the air search an aircraft joins when its range reaches the city from its base or a plot its carrier can
// reach, and the carrier joins with it, as ExecuteFight adds carriers. Endangered recruits are never defenders.
void CvTacticalOperationalLoop::GatherCityDefenders(CvTacticalAI& kTactical, const CvCity& kCity, const vector<CvUnit*>& vEndangered,
	vector<CvUnit*>& vDefenders)
{
	vDefenders.clear();
	// carrier reach is rebuilt for every recruitment, as FindUnitsWithinStrikingDistance does, because earlier
	// commands may have opened or closed sea routes
	if (MOD_VOX_TACTICAL_AIR_SEARCH)
		TacticalAIHelpers::ResetAirSearchCaches();
	const CvPlot* pCityPlot = kCity.plot();
	const bool bCoastal = kCity.isCoastal();
	for (list<int>::const_iterator it = kTactical.m_CurrentTurnUnits.begin(); it != kTactical.m_CurrentTurnUnits.end(); ++it)
	{
		CvUnit* pUnit = CityRecruit(kTactical, *it);
		if (!pUnit || pUnit->IsCoveringFriendlyCivilian() || std::find(vEndangered.begin(), vEndangered.end(), pUnit) != vEndangered.end())
			continue;
		if (pUnit->getDomainType() == DOMAIN_AIR)
		{
			if (!MOD_VOX_TACTICAL_AIR_SEARCH)
				continue;
			const vector<int> vLaunchPlots = TacticalAIHelpers::GetCargoLaunchPlots(pUnit, pCityPlot);
			bool bReach = false;
			for (size_t i = 0; i < vLaunchPlots.size() && !bReach; i++)
				bReach = plotDistance(*GC.getMap().plotByIndexUnchecked(vLaunchPlots[i]), *pCityPlot) <= pUnit->GetRange();
			if (bReach)
				vDefenders.push_back(pUnit);
			continue;
		}
		if (!(pUnit->IsCombatUnit() || pUnit->IsCanAttackRanged()) ||
			(pUnit->getDomainType() != DOMAIN_LAND && !(bCoastal && pUnit->getDomainType() == DOMAIN_SEA)))
			continue;
		if (plotDistance(*pUnit->plot(), *pCityPlot) <= StrikeReach(*pUnit))
			vDefenders.push_back(pUnit);
	}

	//carriers belong to operations and are never tactical units on their own, so only the ship's state is checked
	const size_t nDefenders = vDefenders.size();
	for (size_t i = 0; i < nDefenders; i++)
	{
		CvUnit* pShip = vDefenders[i]->getDomainType() == DOMAIN_AIR ? vDefenders[i]->getTransportUnit() : NULL;
		if (pShip && pShip->canUseNow() && std::find(vDefenders.begin(), vDefenders.end(), pShip) == vDefenders.end() &&
			std::find(vEndangered.begin(), vEndangered.end(), pShip) == vEndangered.end())
			vDefenders.push_back(pShip);
	}
}

//------------------------------------------------------------------------------
// readiness and execution
//------------------------------------------------------------------------------

// A melee combat unit with moves and an attack left that may enter cities.
bool CvTacticalOperationalLoop::CanCapture(const CvUnit& kUnit)
{
	return kUnit.IsCombatUnit() && kUnit.canMove() && !kUnit.isOutOfAttacks() && !kUnit.IsCanAttackRanged() && !kUnit.isNoCapture();
}

// CanCapture without this turn's moves and attacks, for a living land unit outside armies.
bool CvTacticalOperationalLoop::IsCapturer(const CvUnit& kUnit)
{
	return !kUnit.isDelayedDeath() && kUnit.getDomainType() == DOMAIN_LAND && kUnit.getArmyID() == -1 && kUnit.IsCombatUnit() &&
		!kUnit.IsCanAttackRanged() && !kUnit.isNoCapture();
}

// Fills a native target with the battlefield's type, plot and zone.
CvTacticalTarget CvTacticalOperationalLoop::FightTarget(const SOperationalBattlefield& field, const CvPlot& kPlot)
{
	CvTacticalTarget target;
	target.SetTargetType(field.eCenter == OPERATIONAL_CENTER_ENEMY_CITY ? AI_TACTICAL_TARGET_ENEMY_CITY : AI_TACTICAL_TARGET_ENEMY_COMBAT_UNIT);
	target.SetTargetX(kPlot.getX());
	target.SetTargetY(kPlot.getY());
	target.SetDominanceZone(field.iZoneID);
	return target;
}

// A unit fight passes when a city can strike its target, and any enemy fight passes when native recruitment finds units
// with positive expected damage, which for a city fight is damage to the city or to its garrison; a city at 1 HP or
// less also needs a recruit that can capture it or positive garrison damage. Our city's fight passes with an
// endangered recruit, or with a defender and a visible enemy combat unit within the search's 5 plots.
eOperationalCheckResult CvTacticalOperationalLoop::CheckFight(CvTacticalAI& kTactical, const SOperationalTurn& turn,
	const SOperationalBattlefield& field)
{
	CvPlot* pPlot = GC.getMap().plotByIndex(field.iPlotIndex);
	if (!pPlot)
		return OPERATIONAL_CHECK_TARGET_GONE;
	const PlayerTypes ePlayer = kTactical.m_pPlayer->GetID();
	if (field.eCenter == OPERATIONAL_CENTER_OUR_CITY)
	{
		CvCity* pOurCity = pPlot->getPlotCity();
		if (!pOurCity || pOurCity->getOwner() != ePlayer)
			return OPERATIONAL_CHECK_TARGET_GONE;
		vector<CvUnit*> vEndangered;
		GatherCityEndangered(kTactical, turn, *pOurCity, vEndangered);
		if (!vEndangered.empty())
			return OPERATIONAL_CHECK_PASSED;
		vector<CvUnit*> vDefenders;
		GatherCityDefenders(kTactical, *pOurCity, vEndangered, vDefenders);
		if (vDefenders.empty())
			return OPERATIONAL_CHECK_NO_DEFENDERS;
		for (int i = 0; i < RING_PLOTS[TACTICAL_COMBAT_MAX_TARGET_DISTANCE + 1]; i++)
		{
			const CvPlot* pLoopPlot = iterateRingPlots(pPlot, i);
			if (pLoopPlot && pLoopPlot->isEnemyUnit(ePlayer, true, true))
				return OPERATIONAL_CHECK_PASSED;
		}
		return OPERATIONAL_CHECK_NO_NEARBY_ENEMY;
	}
	const CvTacticalTarget target = FightTarget(field, *pPlot);
	if (field.eCenter == OPERATIONAL_CENTER_ENEMY_UNIT)
	{
		CvUnit* pDefender = pPlot->getBestDefender(NO_PLAYER, ePlayer, NULL, true, true);
		if (!pDefender || pDefender->isDelayedDeath())
			return OPERATIONAL_CHECK_TARGET_GONE;
		if (kTactical.FindCitiesWithinStrikingDistance(pPlot))
			return OPERATIONAL_CHECK_PASSED;
	}
	if (!kTactical.FindUnitsWithinStrikingDistance(pPlot))
		return OPERATIONAL_CHECK_NO_RECRUITS;
	int iGarrisonDamage = 0;
	if (kTactical.ComputeTotalExpectedDamage(target, &iGarrisonDamage) <= 0 && iGarrisonDamage <= 0)
		return OPERATIONAL_CHECK_NO_DAMAGE;
	CvCity* pCity = field.eCenter == OPERATIONAL_CENTER_ENEMY_CITY ? pPlot->getPlotCity() : NULL;
	if (!pCity || pCity->GetMaxHitPoints() - pCity->getDamage() > 1 || iGarrisonDamage > 0)
		return OPERATIONAL_CHECK_PASSED;
	for (size_t i = 0; i < kTactical.m_CurrentMoveUnits.size(); i++)
	{
		const CvUnit* pUnit = kTactical.m_pPlayer->getUnit(kTactical.m_CurrentMoveUnits[i].GetID());
		if (pUnit && CanCapture(*pUnit))
			return OPERATIONAL_CHECK_PASSED;
	}
	return OPERATIONAL_CHECK_NO_CAPTURER;
}

// Copy of CvTacticalAI::ExecuteDestroyEnemyUnits followed by CvTacticalAI::ExecuteAttackWithUnits, and for a city the
// follow-up of CvTacticalAI::ExecuteCaptureCityMoves. Changes: the target's endangered units of ours join the recruits;
// the spotter move skips held garrisons; the search runs through ExecuteEngagement with the kind row, the geographic
// blend and aggression from RISK; and the executed rows mark each endangered unit placed or free. Our city's fight
// sets its label here and continues in ExecuteCityDefense.
void CvTacticalOperationalLoop::ExecuteFight(CvTacticalAI& kTactical, const SOperationalTurn& turn, const SOperationalBattlefield& field,
	SOperationalCommandRecord& record)
{
	CvPlayer& kPlayer = *kTactical.m_pPlayer;
	const PlayerTypes ePlayer = kPlayer.GetID();
	record.bSearchRan = false;
	CvPlot* pPlot = GC.getMap().plotByIndex(field.iPlotIndex);
	if (!pPlot)
		return;
	const CvTacticalTarget target = FightTarget(field, *pPlot);
	const bool bCity = field.eCenter == OPERATIONAL_CENTER_ENEMY_CITY;

	//UnitProcessed labels the units this fight finishes with the current move, which the garrison pass before the loop
	//left set. The fight takes the label native's routine for its zone's posture sets; without a posture, a city fight
	//takes the city strike label, our city's fight the hedgehog label and a unit fight the attrition label.
	static const AITacticalMove aePostureMoves[NUM_OPERATIONAL_POSTURES] =
	{
		AI_TACTICAL_MOVE_NONE, AI_TACTICAL_WITHDRAW, AI_TACTICAL_ATTRITION, AI_TACTICAL_FLANKATTACK, AI_TACTICAL_STEAMROLL,
		AI_TACTICAL_SURGICAL_STRIKE, AI_TACTICAL_HEDGEHOG, AI_TACTICAL_COUNTERATTACK,
	};
	const CvTacticalDominanceZone* pZone = kTactical.GetTacticalAnalysisMap()->GetZoneByIDWithoutRefresh(field.iZoneID);
	const int iPosture = pZone ? range((int)pZone->GetPosture(), 0, NUM_OPERATIONAL_POSTURES - 1) : (int)TACTICAL_POSTURE_NONE;
	AITacticalMove eMove = aePostureMoves[iPosture];
	if (eMove == AI_TACTICAL_MOVE_NONE)
		eMove = field.eCenter == OPERATIONAL_CENTER_OUR_CITY ? AI_TACTICAL_HEDGEHOG : bCity ? AI_TACTICAL_SURGICAL_STRIKE : AI_TACTICAL_ATTRITION;
	kTactical.m_CurrentMoveUnits.setCurrentTacticalMove(eMove);
	if (field.eCenter == OPERATIONAL_CENTER_OUR_CITY)
	{
		ExecuteCityDefense(kTactical, turn, field, record);
		return;
	}

	// Might be able to hit/kill with a city
	if (!bCity)
	{
		CvUnit* pDefender = pPlot->getBestDefender(NO_PLAYER, ePlayer, NULL, true, true);
		if (!pDefender || pDefender->isDelayedDeath())
			return;
		if (kTactical.FindCitiesWithinStrikingDistance(pPlot) && kTactical.ExecuteAttackWithCities(pDefender))
			return;
	}

	// Now the real deal; without recruits or damage to the target or its garrison the command ends after city fire
	if (!kTactical.FindUnitsWithinStrikingDistance(pPlot))
		return;
	int iGarrisonDamage = 0;
	if (kTactical.ComputeTotalExpectedDamage(target, &iGarrisonDamage) <= 0 && iGarrisonDamage <= 0)
		return;

	vector<CvUnit*> vUnits;
	for (size_t i = 0; i < kTactical.m_CurrentMoveUnits.size(); i++)
	{
		CvUnit* pUnit = kPlayer.getUnit(kTactical.m_CurrentMoveUnits[i].GetID());
		if (pUnit && kTactical.m_CurrentMoveUnits[i].GetAttackStrength() >= 0) //sometimes we mark units as unnecessary
			vUnits.push_back(pUnit);
	}

	//the units of ours the target can attack next turn join the search, outside armies as native recruitment has it
	vector<int> vEndangered;
	const map<pair<int, int>, SOperationalThreatExposure>& threats = bCity ? turn.cityThreats : turn.unitThreats;
	map<pair<int, int>, SOperationalThreatExposure>::const_iterator threat = threats.find(make_pair(static_cast<int>(field.eOwner), field.iTargetID));
	if (threat != threats.end())
		vEndangered = threat->second.vUnitIDs;
	CvCity* pTargetCity = bCity ? pPlot->getPlotCity() : NULL;
	CvUnit* pGarrison = pTargetCity ? pTargetCity->GetGarrisonedUnit() : NULL;
	if (pGarrison)
	{
		threat = turn.unitThreats.find(make_pair(static_cast<int>(pGarrison->getOwner()), pGarrison->GetID()));
		if (threat != turn.unitThreats.end())
			vEndangered.insert(vEndangered.end(), threat->second.vUnitIDs.begin(), threat->second.vUnitIDs.end());
	}
	for (size_t i = 0; i < vEndangered.size(); i++)
	{
		CvUnit* pUnit = kPlayer.getUnit(vEndangered[i]);
		if (!pUnit || !pUnit->canUseNow() || pUnit->getArmyID() != -1)
			continue;
		bool bListed = false;
		for (size_t j = 0; j < record.vEndangered.size() && !bListed; j++)
			bListed = record.vEndangered[j].first == pUnit->GetID();
		if (bListed)
			continue;
		record.vEndangered.push_back(make_pair(pUnit->GetID(), false));
		if (std::find(vUnits.begin(), vUnits.end(), pUnit) == vUnits.end())
			vUnits.push_back(pUnit);
	}

	//try to improve visibility
	vector<CvUnit*> vSpotters;
	for (size_t i = 0; i < vUnits.size(); i++)
		if (!IsHeldGarrison(vUnits[i]))
			vSpotters.push_back(vUnits[i]);
	bool bSearch = kTactical.ExecuteSpotterMove(vSpotters, pPlot);

	if (bSearch && MOD_VOX_TACTICAL_AIR_SEARCH)
	{
		// aircraft and missiles are planned together with the ground and naval units: fighters recruited for sweeps join
		// the search, and so do the ships carrying aircraft, because a carrier may move before it launches
		for (size_t i = 0; i < kTactical.m_CurrentAirSweepUnits.size(); i++)
		{
			CvUnit* pSweeper = kPlayer.getUnit(kTactical.m_CurrentAirSweepUnits[i].GetID());
			if (pSweeper && std::find(vUnits.begin(), vUnits.end(), pSweeper) == vUnits.end())
				vUnits.push_back(pSweeper);
		}
		size_t nUnitsBeforeCarriers = vUnits.size();
		for (size_t i = 0; i < nUnitsBeforeCarriers; i++)
		{
			// carriers belong to operations and are never tactical units on their own, so only the unit's state is checked
			CvUnit* pShip = (vUnits[i] && vUnits[i]->getDomainType() == DOMAIN_AIR) ? vUnits[i]->getTransportUnit() : NULL;
			if (pShip && pShip->canUseNow() && std::find(vUnits.begin(), vUnits.end(), pShip) == vUnits.end())
				vUnits.push_back(pShip);
		}
	}
	else if (bSearch)
	{
		//first handle air units (including missiles)
		kTactical.ExecuteAirSweep(pPlot);
		kTactical.ExecuteAirAttack(pPlot);

		//did the air attack already kill the enemy?
		if (pPlot->getBestDefender(NO_PLAYER, ePlayer, NULL, true, true) == NULL && !pPlot->isCity())
			bSearch = false;
	}

	//the search takes the units still usable after the spotter and air actions, aircraft only with the air search; an
	//empty group is a refusal and retires nothing
	SOperationalEngagementReport report;
	if (bSearch)
	{
		vector<CvUnit*> vSearch;
		for (size_t i = 0; i < vUnits.size(); i++)
			if (vUnits[i] && vUnits[i]->canUseNow() && (MOD_VOX_TACTICAL_AIR_SEARCH || vUnits[i]->getDomainType() != DOMAIN_AIR))
				vSearch.push_back(vUnits[i]);
		vUnits.swap(vSearch);
		record.bSearchRan = !vUnits.empty();
		if (record.bSearchRan)
			ExecuteEngagement(ePlayer, vUnits, pPlot, bCity ? OPERATIONAL_KIND_CITY_FIGHT : OPERATIONAL_KIND_UNIT_FIGHT, field.eOwner,
				turn.aiMapped, turn.settings, true, report);
	}

	//the city follow-up runs after any attack attempt, as in native ExecuteCaptureCityMoves
	if (bCity)
	{
		// Did it work?  If so, don't need a temporary dominance zone if had one here
		if (pPlot->getOwner() == ePlayer)
			kTactical.DeleteFocusArea(pPlot);

		//do we have embarked units we need to put ashore
		if (kTactical.FindEmbarkedUnitsAroundTarget(pPlot, 4))
			kTactical.ExecuteLandingOperation(pPlot);
	}

	MarkDispositions(report, record);
}

// Gathers the city's recruits, records its endangered ones, and runs one engagement over them with no counterpart,
// since native gives no civ modifier in our own zone.
void CvTacticalOperationalLoop::ExecuteCityDefense(CvTacticalAI& kTactical, const SOperationalTurn& turn, const SOperationalBattlefield& field,
	SOperationalCommandRecord& record)
{
	CvPlayer& kPlayer = *kTactical.m_pPlayer;
	record.bSearchRan = false;
	CvPlot* pPlot = GC.getMap().plotByIndex(field.iPlotIndex);
	CvCity* pCity = pPlot ? pPlot->getPlotCity() : NULL;
	if (!pCity || pCity->getOwner() != kPlayer.GetID())
		return;

	vector<CvUnit*> vUnits;
	vector<CvUnit*> vEndangered;
	GatherCityEndangered(kTactical, turn, *pCity, vEndangered);
	GatherCityDefenders(kTactical, *pCity, vEndangered, vUnits);
	for (size_t i = 0; i < vEndangered.size(); i++)
	{
		record.vEndangered.push_back(make_pair(vEndangered[i]->GetID(), false));
		vUnits.push_back(vEndangered[i]);
	}

	SOperationalEngagementReport report;
	record.bSearchRan = !vUnits.empty();
	if (record.bSearchRan)
		ExecuteEngagement(kPlayer.GetID(), vUnits, pPlot, OPERATIONAL_KIND_OWN_CITY_FIGHT, NO_PLAYER, turn.aiMapped, turn.settings,
			false, report);
	MarkDispositions(report, record);
}

// A move or attack that ran places a unit; a finish or hold places it only in an attempt that completed.
void CvTacticalOperationalLoop::MarkDispositions(const SOperationalEngagementReport& report, SOperationalCommandRecord& record)
{
	for (size_t i = 0; i < record.vEndangered.size(); i++)
	{
		const int iUnitID = record.vEndangered[i].first;
		for (size_t a = 0; a < report.attempts.size() && !record.vEndangered[i].second; a++)
		{
			const SOperationalAttempt& attempt = report.attempts[a];
			for (size_t r = 0; r < attempt.nRowsRun && r < attempt.vRows.size(); r++)
			{
				const STacticalAssignment& row = attempt.vRows[r];
				//a blocked unit, such as one the participant cap left out, is not placed
				if (row.iUnitID != iUnitID || row.eAssignmentType == A_INITIAL || row.eAssignmentType == A_RESTART ||
					row.eAssignmentType == A_BLOCKED)
					continue;
				const bool bHold = row.eAssignmentType == A_FINISH || row.eAssignmentType == A_FINISH_TEMP ||
					row.eAssignmentType == A_HEAL || row.eAssignmentType == A_WAIT;
				if (!bHold || attempt.bCompleted)
				{
					record.vEndangered[i].second = true;
					break;
				}
			}
		}
	}
}

// The flood is native's reinforcement reach from PlotReinforcementMoves, run once per check. The alternative-target
// filter is native's too, moved here because it floods each unit's own reach. Held garrisons need no march, but the
// search must count them as near the target, or nothing would move them.
eOperationalCheckResult CvTacticalOperationalLoop::CheckReinforce(CvTacticalAI& kTactical, const SOperationalTurn& turn,
	SOperationalBattlefield& field)
{
	if (field.reinforce.bChecked)
		return OPERATIONAL_CHECK_PASSED;
	CvPlot* pTarget = GC.getMap().plotByIndex(field.iPlotIndex);
	if (!pTarget)
		return OPERATIONAL_CHECK_TARGET_GONE;

	// do not set a player - that way we can traverse unrevealed plots and foreign territory
	SPathFinderUserData data(NO_PLAYER, PT_ARMY_MIXED, NO_PLAYER, kTactical.GetRecruitRange());
	const ReachablePlots relevantPlots = GC.GetStepFinder().GetPlotsInReach(pTarget, data);

	//held garrisons join only through the search, so they need its own domain distance, flooded once per domain
	map<int, ReachablePlots> searchPlots;
	const double fBefore = field.fReinforceEstimate;
	set<int> passed;
	for (;;)
	{
		bool bRejected = false;
		for (size_t i = 0; i < field.reinforce.vSelected.size(); i++)
		{
			const int iUnitID = field.reinforce.vSelected[i];
			if (passed.count(iUnitID) > 0)
				continue;
			CvUnit* pUnit = kTactical.m_pPlayer->getUnit(iUnitID);
			bool bReach = pUnit && pUnit->plot();
			if (bReach && IsHeldGarrison(pUnit))
			{
				const int iDomain = pUnit->getDomainType();
				if (searchPlots.find(iDomain) == searchPlots.end())
					searchPlots[iDomain] = SearchReachPlots(kTactical.m_pPlayer->GetID(), pTarget, pUnit->getDomainType());
				bReach = searchPlots[iDomain].find(pUnit->plot()->GetPlotIndex()) != searchPlots[iDomain].end();
			}
			else if (bReach)
				bReach = relevantPlots.find(pUnit->plot()->GetPlotIndex()) != relevantPlots.end();
			if (bReach && !IsHeldGarrison(pUnit))
			{
				//don't run away if there's other work to do (will eventually be handled by ExecuteAttackWithUnits)
				vector<pair<CvPlot*, bool> > altTargets = TacticalAIHelpers::GetTargetsInRange(pUnit, false, false);
				for (size_t j = 0; j < altTargets.size() && bReach; j++)
					bReach = plotDistance(*altTargets[j].first, *pTarget) <= TACTICAL_COMBAT_MAX_TARGET_DISTANCE;
				bReach = bReach && CanMarch(kTactical, pUnit, pTarget);
			}
			if (bReach)
				passed.insert(iUnitID);
			else
			{
				field.reinforce.rejected.insert(iUnitID);
				bRejected = true;
			}
		}
		if (!bRejected)
			break;
		//the full estimate runs again, so the projected shares and the damage extension follow the smaller roster
		if (!EstimateBattlefield(kTactical, turn, field, true))
			field.fReinforceEstimate = 0.0;
	}
	field.reinforce.bChecked = true;
	if (field.reinforce.vSelected.empty())
		return OPERATIONAL_CHECK_NO_REACHABLE;
	if (field.fReinforceEstimate < fBefore)
		return OPERATIONAL_CHECK_REDUCED;
	return OPERATIONAL_CHECK_PASSED;
}

// The group is the checked selection still usable now. The move label is native's reinforcement label; at an enemy
// center the target's owner is the counterpart, while our city has none, as in its fight.
void CvTacticalOperationalLoop::ExecuteReinforce(CvTacticalAI& kTactical, const SOperationalTurn& turn, const SOperationalBattlefield& field,
	SOperationalCommandRecord& record)
{
	CvPlayer& kPlayer = *kTactical.m_pPlayer;
	record.bSearchRan = false;
	record.bMoved = false;
	record.vSelected = field.reinforce.vSelected;
	CvPlot* pTarget = GC.getMap().plotByIndex(field.iPlotIndex);
	if (!pTarget)
		return;
	kTactical.m_CurrentMoveUnits.setCurrentTacticalMove(AI_TACTICAL_REINFORCE);

	vector<CvUnit*> vUnits;
	map<int, int> startPlots;
	for (size_t i = 0; i < record.vSelected.size(); i++)
	{
		CvUnit* pUnit = kPlayer.getUnit(record.vSelected[i]);
		if (!pUnit || !pUnit->plot())
			continue;
		startPlots[pUnit->GetID()] = pUnit->plot()->GetPlotIndex();
		if (pUnit->canUseNow())
			vUnits.push_back(pUnit);
	}
	if (vUnits.empty())
		return;

	SOperationalEngagementReport report;
	record.bSearchRan = true;
	PositionUnitsAroundTarget(kTactical, vUnits, pTarget, OPERATIONAL_KIND_REINFORCE,
		field.eCenter == OPERATIONAL_CENTER_OUR_CITY ? NO_PLAYER : field.eOwner, turn.aiMapped, turn.settings, report);

	for (map<int, int>::const_iterator it = startPlots.begin(); it != startPlots.end() && !record.bMoved; ++it)
	{
		const CvUnit* pUnit = kPlayer.getUnit(it->first);
		record.bMoved = !pUnit || pUnit->isDelayedDeath() || !pUnit->plot() || pUnit->plot()->GetPlotIndex() != it->second;
	}
}

//------------------------------------------------------------------------------
// overlap and wake
//------------------------------------------------------------------------------

// Native's suppression geometry, applied around an executed enemy-unit fight: a compatible zone and 2 plots, with
// targets near our city or able to attack our units exempt. The caller checks that the executed fight ran its search.
// Enemy and own city fights are neither retired nor retire others.
bool CvTacticalOperationalLoop::RetiresFight(const SOperationalBattlefield& executed, const SOperationalBattlefield& candidate)
{
	if (executed.eCenter != OPERATIONAL_CENTER_ENEMY_UNIT || candidate.eCenter != OPERATIONAL_CENTER_ENEMY_UNIT || candidate.iID == executed.iID)
		return false;
	if (candidate.eFight == OPERATIONAL_OFFER_EXECUTED || candidate.eFight == OPERATIONAL_OFFER_RETIRED ||
		candidate.eFight == OPERATIONAL_OFFER_SKIPPED || candidate.eFight == OPERATIONAL_OFFER_DROPPED)
		return false;
	if (candidate.bCanAttackOurUnits || candidate.bNearOurCity)
		return false;

	//land zones have id > 0, water zones id < 0. opposite signs means domain mismatch
	if ((executed.iZoneID > 0 && candidate.iZoneID < 0) || (executed.iZoneID < 0 && candidate.iZoneID > 0))
		return false;
	CvPlot* pExecuted = GC.getMap().plotByIndex(executed.iPlotIndex);
	CvPlot* pCandidate = GC.getMap().plotByIndex(candidate.iPlotIndex);
	return pExecuted && pCandidate && plotDistance(*pExecuted, *pCandidate) <= 2;
}

// The executed Reinforce's counted sets are from its last estimate, before it ran.
bool CvTacticalOperationalLoop::RetiresReinforce(const SOperationalBattlefield& executed, const SOperationalBattlefield& candidate)
{
	if (candidate.iID == executed.iID || candidate.eCenter == OPERATIONAL_CENTER_OUR_CITY)
		return false;
	if (candidate.eReinforce != OPERATIONAL_OFFER_READY && candidate.eReinforce != OPERATIONAL_OFFER_INACTIVE)
		return false;
	const pair<int, int> key(static_cast<int>(candidate.eOwner), candidate.iTargetID);
	const set<pair<int, int> >& counted = candidate.eCenter == OPERATIONAL_CENTER_ENEMY_CITY ? executed.reinforce.countedCities :
		executed.reinforce.countedUnits;
	return counted.find(key) != counted.end();
}

// Lists the visible living hostile units, the owner of each city battlefield's plot, ours included, and our living
// units' plots.
void CvTacticalOperationalLoop::SnapshotBoard(const SOperationalTurn& turn, SOperationalBoard& board)
{
	board.enemyUnitPlots.clear();
	board.cityOwners.clear();
	board.ourUnitPlots.clear();
	const TeamTypes eTeam = GET_PLAYER(turn.ePlayer).getTeam();
	int iLoop = 0;
	for (int iOther = 0; iOther < MAX_PLAYERS; iOther++)
	{
		CvPlayer& kOther = GET_PLAYER(static_cast<PlayerTypes>(iOther));
		if (iOther == turn.ePlayer || !kOther.isAlive() || GroundSide(turn.ePlayer, static_cast<PlayerTypes>(iOther)) != groundHoldsHostile)
			continue;
		for (CvUnit* pUnit = kOther.firstUnit(&iLoop); pUnit != NULL; pUnit = kOther.nextUnit(&iLoop))
			if (!pUnit->isDelayedDeath() && pUnit->plot() && pUnit->plot()->isVisible(eTeam))
				board.enemyUnitPlots[make_pair(iOther, pUnit->GetID())] = pUnit->plot()->GetPlotIndex();
	}
	for (size_t i = 0; i < turn.battlefields.size(); i++)
	{
		if (turn.battlefields[i].eCenter == OPERATIONAL_CENTER_ENEMY_UNIT)
			continue;
		CvPlot* pPlot = GC.getMap().plotByIndex(turn.battlefields[i].iPlotIndex);
		board.cityOwners[turn.battlefields[i].iPlotIndex] = pPlot && pPlot->getPlotCity() ? pPlot->getPlotCity()->getOwner() : NO_PLAYER;
	}
	CvPlayer& kPlayer = GET_PLAYER(turn.ePlayer);
	for (CvUnit* pUnit = kPlayer.firstUnit(&iLoop); pUnit != NULL; pUnit = kPlayer.nextUnit(&iLoop))
		if (!pUnit->isDelayedDeath() && pUnit->plot())
			board.ourUnitPlots[pUnit->GetID()] = pUnit->plot()->GetPlotIndex();
}

// Enemy units that died, left, arrived or were revealed report their plots; city battlefields report a new owner; our
// units that moved report themselves while they can still be used.
void CvTacticalOperationalLoop::DiffBoard(PlayerTypes ePlayer, const SOperationalBoard& before, const SOperationalBoard& after,
	SOperationalChanges& changes)
{
	changes.vEnemyPlots.clear();
	changes.vCapturedPlots.clear();
	changes.vAvailableUnitIDs.clear();
	for (map<pair<int, int>, int>::const_iterator it = before.enemyUnitPlots.begin(); it != before.enemyUnitPlots.end(); ++it)
	{
		map<pair<int, int>, int>::const_iterator now = after.enemyUnitPlots.find(it->first);
		if (now == after.enemyUnitPlots.end() || now->second != it->second)
			changes.vEnemyPlots.push_back(it->second);
	}
	for (map<pair<int, int>, int>::const_iterator it = after.enemyUnitPlots.begin(); it != after.enemyUnitPlots.end(); ++it)
	{
		map<pair<int, int>, int>::const_iterator then = before.enemyUnitPlots.find(it->first);
		if (then == before.enemyUnitPlots.end() || then->second != it->second)
			changes.vEnemyPlots.push_back(it->second);
	}
	for (map<int, int>::const_iterator it = after.cityOwners.begin(); it != after.cityOwners.end(); ++it)
	{
		map<int, int>::const_iterator then = before.cityOwners.find(it->first);
		if (then != before.cityOwners.end() && then->second != it->second)
			changes.vCapturedPlots.push_back(it->first);
	}
	CvPlayer& kPlayer = GET_PLAYER(ePlayer);
	for (map<int, int>::const_iterator it = after.ourUnitPlots.begin(); it != after.ourUnitPlots.end(); ++it)
	{
		map<int, int>::const_iterator then = before.ourUnitPlots.find(it->first);
		if (then != before.ourUnitPlots.end() && then->second == it->second)
			continue;
		const CvUnit* pUnit = kPlayer.getUnit(it->first);
		if (pUnit && pUnit->canUseNow())
			changes.vAvailableUnitIDs.push_back(it->first);
	}
}

// Enemy changes count within the turn's striking-distance scan of the target; a unit of ours counts within its own
// striking distance, as native's distance reject reads it, and aircraft and port-changing ships always count. Our
// city's fight reads its own reach: the search's 5 plots for enemy changes and captures, strike reach for a unit of ours,
// and the endangered flag its estimate just set.
bool CvTacticalOperationalLoop::WakesFight(PlayerTypes ePlayer, const SOperationalTurn& turn, const SOperationalBattlefield& field,
	const SOperationalChanges& changes)
{
	CvPlot* pTarget = GC.getMap().plotByIndex(field.iPlotIndex);
	if (!pTarget)
		return false;
	const bool bOurCity = field.eCenter == OPERATIONAL_CENTER_OUR_CITY;
	if (bOurCity && field.bHasEndangered)
		return true;
	const int iEnemyReach = bOurCity ? TACTICAL_COMBAT_MAX_TARGET_DISTANCE + 1 : turn.iStrikeScan;
	for (size_t i = 0; i < changes.vEnemyPlots.size(); i++)
	{
		CvPlot* pPlot = GC.getMap().plotByIndex(changes.vEnemyPlots[i]);
		if (pPlot && plotDistance(*pPlot, *pTarget) <= iEnemyReach)
			return true;
	}
	for (size_t i = 0; i < changes.vCapturedPlots.size(); i++)
	{
		CvPlot* pPlot = GC.getMap().plotByIndex(changes.vCapturedPlots[i]);
		if (pPlot && plotDistance(*pPlot, *pTarget) <= iEnemyReach)
			return true;
	}
	CvPlayer& kPlayer = GET_PLAYER(ePlayer);
	for (size_t i = 0; i < changes.vAvailableUnitIDs.size(); i++)
	{
		const CvUnit* pUnit = kPlayer.getUnit(changes.vAvailableUnitIDs[i]);
		if (!pUnit || !pUnit->canUseNow() || !pUnit->plot())
			continue;
		if (pUnit->getDomainType() == DOMAIN_AIR || pUnit->getUnitInfo().IsCanChangePort())
			return true;
		const int iReach = bOurCity ? StrikeReach(*pUnit) : pUnit->baseMoves(false) * 4;
		if (plotDistance(*pUnit->plot(), *pTarget) <= iReach)
			return true;
	}
	return false;
}

//------------------------------------------------------------------------------
// live value adapters
//------------------------------------------------------------------------------

// Base worth with the level premium, plus the discounted gain of its upgrade, as the grade's MakeUnitReference values
// it. The upgrade counts when the unit is ready for it and the tech and project prerequisites are met; gold, territory,
// embarkation and unit class limits are not checked.
double CvTacticalOperationalLoop::OurUnitWorth(const CvUnit& kUnit)
{
	const CvUnitEntry& kInfo = kUnit.getUnitInfo();
	const double fBase = UnitBaseWorth(kInfo.GetPower(), kInfo.GetProductionCost());
	double fUpgrade = 0.0;
	const UnitTypes eUpgrade = kUnit.GetUpgradeUnitType();
	if (eUpgrade != NO_UNIT && kUnit.CanUpgradeRightNow(true))
	{
		const CvUnitEntry* pUpgrade = GC.getUnitInfo(eUpgrade);
		if (pUpgrade)
			fUpgrade = UpgradePremium(fBase, UnitBaseWorth(pUpgrade->GetPower(), pUpgrade->GetProductionCost()), gradeUpgradeDiscount);
	}
	return UnitWorth(fBase, kUnit.getLevel(), gradeSeniorityPerLevel) + fUpgrade;
}

// Worth with the level premium, scaled from the prize floor up to the whole worth by the damage share.
double CvTacticalOperationalLoop::UnitPrize(const CvUnit& kUnit, double fPrizeFloor)
{
	const CvUnitEntry& kInfo = kUnit.getUnitInfo();
	const double fWorth = UnitWorth(UnitBaseWorth(kInfo.GetPower(), kInfo.GetProductionCost()), kUnit.getLevel(), gradeSeniorityPerLevel);
	return fWorth * (fPrizeFloor + (1.0 - fPrizeFloor) * DamageShare(kUnit.getDamage(), kUnit.GetMaxHitPoints()));
}

// Mirrors the grade's SideIn and FriendPlayers on live relations: hostility wins; friends are teammates, master and
// vassal teams, and minors allied with us or one of those.
unsigned int CvTacticalOperationalLoop::GroundSide(PlayerTypes eUs, PlayerTypes eOther)
{
	if (eOther == NO_PLAYER || eOther < 0 || eOther >= MAX_PLAYERS)
		return 0;
	if (eOther == eUs)
		return groundHoldsOurs;
	CvPlayer& kOther = GET_PLAYER(eOther);
	if (kOther.isBarbarian())
		return groundHoldsHostile;
	const TeamTypes eOurTeam = GET_PLAYER(eUs).getTeam();
	const TeamTypes eTeam = kOther.getTeam();
	if (GET_TEAM(eOurTeam).isAtWar(eTeam))
		return groundHoldsHostile;
	if (eTeam == eOurTeam || GET_TEAM(eOurTeam).IsVassal(eTeam) || GET_TEAM(eTeam).IsVassal(eOurTeam))
		return groundHoldsOurs;
	if (kOther.isMinorCiv())
	{
		const PlayerTypes eAlly = kOther.GetMinorCivAI()->GetAlly();
		if (eAlly == eUs)
			return groundHoldsOurs;
		if (eAlly != NO_PLAYER)
		{
			const TeamTypes eAllyTeam = GET_PLAYER(eAlly).getTeam();
			if (eAllyTeam == eOurTeam || GET_TEAM(eOurTeam).IsVassal(eAllyTeam) || GET_TEAM(eAllyTeam).IsVassal(eOurTeam))
				return groundHoldsOurs;
		}
	}
	return groundHoldsNeutral;
}

// Mirrors the grade's MarkPresence for one plot: a city marks its owner's side; a living military unit that is neither
// embarked nor an aircraft marks its side, and blockades when it is of our side or the hostile side, stands off a city
// plot and is in its native domain.
void CvTacticalOperationalLoop::PlotPresence(const CvPlot& kPlot, PlayerTypes eUs, const CvUnit* pSkip, unsigned int& iStand,
	unsigned int& iBlock)
{
	iStand = 0;
	iBlock = 0;
	const bool bCity = kPlot.isCity();
	if (bCity && kPlot.getPlotCity())
		iStand |= GroundSide(eUs, kPlot.getPlotCity()->getOwner());
	for (int i = 0; i < kPlot.getNumUnits(); i++)
	{
		const CvUnit* pUnit = kPlot.getUnitByIndex(i);
		if (!pUnit || pUnit == pSkip || pUnit->isDelayedDeath() || pUnit->isEmbarked() || pUnit->getDomainType() == DOMAIN_AIR ||
			!(pUnit->IsCombatUnit() || pUnit->IsCanAttackRanged()))
			continue;
		const unsigned int iSide = GroundSide(eUs, pUnit->getOwner());
		iStand |= iSide;
		if ((iSide & (groundHoldsOurs | groundHoldsHostile)) != 0 && !bCity && pUnit->isNativeDomain(&kPlot))
			iBlock |= iSide;
	}
}

// Scores the owned plots among the target plot and its neighbors on its landmass, as the grade's GroundScore does,
// without dividing by the base.
double CvTacticalOperationalLoop::GroundAround(PlayerTypes eUs, const CvPlot& kTarget, const CvUnit* pSkip)
{
	double fLost = 0.0;
	double fCredit = 0.0;
	for (int i = 0; i < RING1_PLOTS; i++)
	{
		const CvPlot* pPlot = iterateRingPlots(&kTarget, i);
		if (!pPlot || pPlot->getOwner() == NO_PLAYER || (i > 0 && pPlot->getLandmass() != kTarget.getLandmass()))
			continue;
		unsigned int iStand = 0;
		unsigned int iBlock = 0;
		PlotPresence(*pPlot, eUs, pSkip, iStand, iBlock);
		unsigned int iNearby = 0;
		for (int j = 1; j < RING1_PLOTS; j++)
		{
			const CvPlot* pNeighbor = iterateRingPlots(pPlot, j);
			if (!pNeighbor || pNeighbor->getLandmass() != pPlot->getLandmass())
				continue;
			unsigned int iNeighborStand = 0;
			unsigned int iNeighborBlock = 0;
			PlotPresence(*pNeighbor, eUs, pSkip, iNeighborStand, iNeighborBlock);
			iNearby |= iNeighborBlock;
		}
		const bool bOurs = pPlot->getOwner() == eUs;
		const bool bHostile = !bOurs && GroundSide(eUs, pPlot->getOwner()) == groundHoldsHostile;
		AddGroundPlot(bOurs, bHostile, pPlot->isCity(), GroundPlotWeight(eUs, *pPlot), iStand, iNearby, fLost, fCredit);
	}
	return fLost - fCredit;
}

// The ground the target holds: the score with it less the score without it, over the ground base.
double CvTacticalOperationalLoop::TargetGround(PlayerTypes eUs, const CvPlot& kTarget, const CvUnit* pTarget, double fGroundBase)
{
	if (fGroundBase <= 0.0)
		return 0.0;
	return (GroundAround(eUs, kTarget, NULL) - GroundAround(eUs, kTarget, pTarget)) / fGroundBase;
}

// Rebuilds a dirty cache first, then hands out the plot's contents.
CvDangerPlotContents& CvTacticalOperationalLoop::DangerContents(PlayerTypes ePlayer, const CvPlot& kPlot)
{
	CvDangerPlots* pDanger = GET_PLAYER(ePlayer).GetDangerPlots();
	if (pDanger->IsDirty())
		pDanger->UpdateDanger();
	return pDanger->m_DangerPlots[kPlot.GetPlotIndex()];
}

//------------------------------------------------------------------------------
// host seams: the simulator defines its own
//------------------------------------------------------------------------------

// Reads the revealed resource's usage for GroundWeight's resource and bonus flags.
double CvTacticalOperationalLoop::GroundPlotWeight(PlayerTypes ePlayer, const CvPlot& kPlot)
{
	const ResourceTypes eResource = kPlot.getResourceType(GET_PLAYER(ePlayer).getTeam());
	int iUsage = -1;
	if (eResource != NO_RESOURCE)
	{
		const CvResourceInfo* pInfo = GC.getResourceInfo(eResource);
		if (pInfo)
			iUsage = pInfo->getResourceUsage();
	}
	return GroundWeight(kPlot, kPlot.getImprovementType(), kPlot.IsImprovementPillaged(), kPlot.isCity(),
		iUsage == RESOURCEUSAGE_STRATEGIC || iUsage == RESOURCEUSAGE_LUXURY, iUsage == RESOURCEUSAGE_BONUS);
}

// The game records no per-command work.
void CvTacticalOperationalLoop::BeginCommand(CvTacticalAI& /*kTactical*/, int /*iCommand*/, int /*iZoneID*/)
{
}

// The game records no per-command work.
void CvTacticalOperationalLoop::EndCommand(CvTacticalAI& /*kTactical*/)
{
}

//------------------------------------------------------------------------------
// engagements
//------------------------------------------------------------------------------

// The geometry comes first so the flavors can follow the zones the search covers.
bool CvTacticalOperationalLoop::ExecuteEngagement(PlayerTypes ePlayer, vector<CvUnit*>& vUnits, CvPlot* pTarget, eOperationalKind eKind,
	PlayerTypes eCounterpart, const int (&aiMapped)[STacticalFlavors::NUM_SLOTS], const OperationalSettings& settings,
	bool bTargetDistanceRelevant, SOperationalEngagementReport& report)
{
	if (vUnits.empty() || !pTarget)
		return false;
	GatherSearchPlotZones(vUnits, pTarget, bTargetDistanceRelevant, report.zonePlots);
	report.flavors = BlendEngagementFlavors(ePlayer, report.zonePlots, eKind, eCounterpart, VoxRlCurrentOperationId(ePlayer),
		aiMapped, settings, report.iRepresentativeZone, report.iAggressionRisk);
	report.eAggLvl = AggressionFromRisk(report.iAggressionRisk, settings);
	// Vox Deorum: classify unit battles by the target zone, including friendly team territory.
	SearchIntent eSearchIntent = OPERATIONAL_KIND_SEARCH_INTENTS[eKind];
	if (eKind == OPERATIONAL_KIND_UNIT_FIGHT)
	{
		const CvTacticalAnalysisMap* pZoneMap = GET_PLAYER(ePlayer).GetTacticalAI()->GetTacticalAnalysisMap();
		const CvTacticalDominanceZone* pZone = pZoneMap->GetZoneByIDWithoutRefresh(
			pZoneMap->GetDominanceZoneIDWithoutRefresh(pTarget->GetPlotIndex()));
		if (pZone && pZone->GetTerritoryType() == TACTICAL_TERRITORY_FRIENDLY)
			eSearchIntent = kSearchIntentBattleFriendly;
		else if (pZone && pZone->GetTerritoryType() == TACTICAL_TERRITORY_ENEMY)
			eSearchIntent = kSearchIntentBattleEnemy;
	}
	return SearchAndExecute(ePlayer, vUnits, pTarget, report.eAggLvl, eSearchIntent, report.flavors,
		bTargetDistanceRelevant, report);
}

// Copy of the retry loop of TacticalAIHelpers::FindAndExecuteBestUnitAssignments. Changes: the flavors, aggression and
// target distance flag are given, and each attempt is recorded with the rows that ran.
// Vox Deorum: capture retains the operational search purpose on every retry.
bool CvTacticalOperationalLoop::SearchAndExecute(PlayerTypes ePlayer, vector<CvUnit*>& vUnits, CvPlot* pTarget, eAggressionLevel eAggLvl,
	SearchIntent eSearchIntent, const STacticalFlavors& flavors, bool bTargetDistanceRelevant, SOperationalEngagementReport& report)
{
	int iCount = 0;
	bool bSuccess = false;
	set<int> unuseableUnits;
	vector<CvUnit*> currentUnits = vUnits;
	TacticalAIHelpers::UpdatePlotDistanceToTarget(ePlayer, pTarget);
	if (MOD_IPC_CHANNEL && gVoxRlCaptureEnabled)
		VoxRlCapture::GetInstance().BeginEngagement(VOX_RL_CALLER_FULL, ePlayer);
	do
	{
		iCount++;

		vector<STacticalAssignment> vAssignments;
		if (MOD_IPC_CHANNEL && gVoxRlCaptureEnabled)
		{
			vAssignments = VoxRlCapture::GetInstance().SearchAssignments(VOX_RL_CALLER_FULL, eSearchIntent, ePlayer,
				currentUnits, pTarget, static_cast<int>(eAggLvl), unuseableUnits, bTargetDistanceRelevant, false, 0, flavors);
		}
		else
		{
			vAssignments = TacticalAIHelpers::FindBestUnitAssignments(currentUnits, pTarget, eAggLvl, unuseableUnits, bTargetDistanceRelevant,
				false, 0, flavors);
		}
		if (vAssignments.empty())
		{
			if (unuseableUnits.size()>0 && currentUnits.size()>unuseableUnits.size())
			{
				//drop the offending units and try again
				vector<CvUnit*> remainingUnits;
				for (vector<CvUnit*>::const_iterator it = currentUnits.begin(); it != currentUnits.end(); ++it)
				{
					if (unuseableUnits.find((*it)->GetID())==unuseableUnits.end())
						remainingUnits.push_back(*it);
				}
				currentUnits = remainingUnits;
			}
			else
				break; //give up
		}
		else
		{
			//restarts might happen when new enemies become visible
			SOperationalAttempt attempt;
			attempt.vRows = vAssignments;
			attempt.nRowsRun = 0;
			bSuccess = TacticalAIHelpers::ExecuteUnitAssignments(vUnits.front()->getOwner(), vAssignments, &attempt.nRowsRun);
			attempt.bCompleted = bSuccess;
			report.attempts.push_back(attempt);
			if (MOD_IPC_CHANNEL && gVoxRlCaptureEnabled)
				VoxRlCapture::GetInstance().NoteExecutionResult(bSuccess);
		}
	}
	while (!bSuccess && iCount < 4);

	gDistanceToTargetPlots.clear();
	if (MOD_IPC_CHANNEL && gVoxRlCaptureEnabled)
		VoxRlCapture::GetInstance().EndEngagement();

	report.bSuccess = bSuccess;
	return bSuccess;
}

// Copy of the plot gathering at the start of TacticalAIHelpers::FindBestUnitAssignments: the same resets, unit filter,
// target rings, rings around enemies and rings around each unit the root accepts. Changes: aircraft add no base plot,
// the pool keeps its usable size since the root is the only position, no scoring setup runs, and the plots are
// counted by zone instead of searched.
void CvTacticalOperationalLoop::GatherSearchPlotZones(const vector<CvUnit*>& vUnits, CvPlot* pTarget, bool bTargetDistanceRelevant,
	map<int, int>& zonePlots)
{
	zonePlots.clear();
	if (vUnits.empty() || vUnits.front()==NULL || pTarget==NULL)
		return;

	PlayerTypes ePlayer = vUnits.front()->getOwner();
	TeamTypes ourTeam = GET_PLAYER(ePlayer).getTeam();

	//clean up from the last run, more if the player changed to limit memory usage
	gTactPosStorage.reset(eLastTactSimPlayer!=ePlayer);
	gSupportPosStorage.reset(eLastTactSimPlayer!=ePlayer);
	gAssignmentStorage.reset(eLastTactSimPlayer != ePlayer);
	eLastTactSimPlayer = ePlayer;

	gReachablePlotsLookup.clear();
	gRangeAttackPlotsLookup.clear();
	gSafePlotCount.clear();
	gOpenPlotDanger.clear();
	TacticalAIHelpers::ResetAirSearchCaches();
	gBadUnitsCount.clear();

	CvTacticalPosition* initialPosition = gTactPosStorage.peekNext(); gTactPosStorage.consumeOne();
	if (!initialPosition)
		return;

	//the plots do not depend on flavors or aggression
	STacticalFlavors neutralFlavors;
	neutralFlavors.iFlavor[STacticalFlavors::RISK] = 50;
	initialPosition->initFromScratch(ePlayer, AL_LOW, pTarget, bTargetDistanceRelevant, false, 0, neutralFlavors);
	initialPosition->reservePlotStorage();

	//first pass: make sure there are no duplicates and other invalid inputs
	vector<const CvUnit*> ourUnits;
	ourUnits.reserve(vUnits.size());
	for (size_t i = 0; i < vUnits.size(); i++)
	{
		CvUnit* pUnit = vUnits[i];
		if (std::find(ourUnits.begin(), ourUnits.end(), pUnit) != ourUnits.end())
			continue;
		if (pUnit && pUnit->canUseNow())
		{
			if (pUnit->isNativeDomain(pUnit->plot()) || pUnit->plot()->getNumUnits() == 1 || pUnit->plot()->isCity())
				ourUnits.push_back(vUnits[i]);
		}
	}
	if (ourUnits.empty())
		return;

	//create the tactical plots around the target (up to distance 5)
	vector<CvPlot*> enemyPlots;
	for (int i = 0; i < RING_PLOTS[TACTICAL_COMBAT_MAX_TARGET_DISTANCE + 1]; i++)
	{
		CvPlot* pPlot = iterateRingPlots(pTarget, i);
		if (pPlot && pPlot->isVisible(ourTeam))
		{
			initialPosition->addTacticalPlot(pPlot, ourUnits);
			if (pPlot->isEnemyUnit(initialPosition->getPlayer(), true, false))
				enemyPlots.push_back(pPlot);
		}
	}

	//second pass, ensure we have space to maneuver around our enemies
	for (size_t j = 0; j < enemyPlots.size(); j++)
	{
		for (int i = RING0_PLOTS; i < RING_PLOTS[2]; i++)
		{
			CvPlot* pPlot = iterateRingPlots(enemyPlots[j], i);
			if (pPlot && pPlot->isVisible(ourTeam))
				initialPosition->addTacticalPlot(pPlot, ourUnits);
		}
	}

	initialPosition->countEnemiesAndCheckVisibility();

	//third pass: the immediate surroundings of every unit the root accepts; an aircraft's base is not geometry
	for (vector<const CvUnit*>::const_iterator it=ourUnits.begin(); it!=ourUnits.end(); ++it)
	{
		const CvUnit* pUnit = *it;
		if (pUnit->getDomainType() == DOMAIN_AIR)
			continue;
		if (initialPosition->addAvailableUnit(pUnit))
		{
			for (int j = 0; j < RING1_PLOTS; j++)
			{
				CvPlot* pPlot = iterateRingPlots(pUnit->plot(), j);
				if (pPlot)
					initialPosition->addTacticalPlot(pPlot, ourUnits);
			}
		}
	}

	CvTacticalAnalysisMap* pZoneMap = GET_PLAYER(ePlayer).GetTacticalAI()->GetTacticalAnalysisMap();
	const vector<CvTacticalPlot>& plots = initialPosition->getTactPlots();
	for (size_t i = 0; i < plots.size(); i++)
	{
		int iZoneID = pZoneMap->GetDominanceZoneIDWithoutRefresh(plots[i].getPlot()->GetPlotIndex());
		if (iZoneID != 0 && iZoneID != -1)
			zonePlots[iZoneID]++;
	}
}

//------------------------------------------------------------------------------
// marching
//------------------------------------------------------------------------------

// A garrison the garrison pass left unprocessed stays in its city unless a search moves it.
bool CvTacticalOperationalLoop::IsHeldGarrison(const CvUnit* pUnit)
{
	return pUnit && pUnit->IsGarrisoned() && !pUnit->TurnProcessed();
}

// Orders civilians last so they have a better chance of getting cover (native PrSortCombatFirst)
struct PrOperationalSortCombatFirst
{
	// Ranks combat units first, then siege, then civilians.
	bool operator()(const CvUnit* lhs, const CvUnit* rhs) const
	{
		return (lhs->IsCivilianUnit() ? 2 : lhs->AI_getUnitAIType()==UNITAI_CITY_BOMBARD ? 1 : 0) < (rhs->IsCivilianUnit() ? 2 : rhs->AI_getUnitAIType() == UNITAI_CITY_BOMBARD ? 1 : 0);
	}
};

// The path flags of native's far-out moves, with safe embark for land units.
int CvTacticalOperationalLoop::MarchFlags(const CvUnit& kUnit, const CvPlot& kTarget)
{
	int	iFlags = CvUnit::MOVEFLAG_NO_STOPNODES | CvUnit::MOVEFLAG_APPROX_TARGET_RING2;
	if (kUnit.isNativeDomain(&kTarget)) //don't embark if we don't have to
		iFlags |= CvUnit::MOVEFLAG_APPROX_TARGET_NATIVE_DOMAIN;
	if (kUnit.IsCivilianUnit())
		iFlags |= (CvUnit::MOVEFLAG_DONT_STACK_WITH_NEUTRAL | CvUnit::MOVEFLAG_AI_ABORT_IN_DANGER);
	//embark only where no turn ends at sea in danger
	if (kUnit.getDomainType() == DOMAIN_LAND)
		iFlags |= CvUnit::MOVEFLAG_SAFE_EMBARK_ONLY;
	return iFlags;
}

// Copy of one domain of TacticalAIHelpers::UpdatePlotDistanceToTarget, the distance the search and the positioning
// copy use to tell near units from far ones. Change: the flood stops at TACTICAL_COMBAT_MAX_TARGET_DISTANCE.
ReachablePlots CvTacticalOperationalLoop::SearchReachPlots(PlayerTypes ePlayer, CvPlot* pTarget, DomainTypes eDomain)
{
	const bool bSea = eDomain == DOMAIN_SEA;
	SPathFinderUserData data(ePlayer, bSea ? PT_NAVAL_UNIT_SIMPLE : PT_LAND_UNIT_SIMPLE);
	if (!bSea && !GET_PLAYER(ePlayer).CanEmbark())
		data.iFlags |= CvUnit::MOVEFLAG_NO_EMBARK;
	if (bSea && !GET_TEAM(GET_PLAYER(ePlayer).getTeam()).CanBuildOceanCrossingUnit())
		data.iFlags |= CvUnit::MOVEFLAG_NO_OCEAN;
	data.iMaxTurns = TACTICAL_COMBAT_MAX_TARGET_DISTANCE;
	return GC.GetStepFinder().GetPlotsInReach(pTarget, data);
}

// Native's danger limit at the first stop: half the HP for attackers that are not siege units, otherwise no danger.
bool CvTacticalOperationalLoop::CanMarch(CvTacticalAI& kTactical, CvUnit* pUnit, CvPlot* pTarget)
{
	if (!pUnit->GeneratePath(pTarget, MarchFlags(*pUnit, *pTarget), kTactical.GetRecruitRange()))
		return false;

	//we are not here to fight or flee, let other moves take over
	int iDanger = pUnit->GetDanger(pUnit->GetPathEndFirstTurnPlot());
	int iDangerLimit = (pUnit->IsCanAttack() && pUnit->AI_getUnitAIType()!=UNITAI_CITY_BOMBARD) ? pUnit->GetCurrHitPoints() / 2 : 0;
	//generals and siege should not even be in fog danger
	return iDanger <= iDangerLimit;
}

// Copy of CvTacticalAI::PositionUnitsAroundTarget. Changes: the search runs through ExecuteEngagement over the units
// still usable after the spotter move, with the kind row, the geographic blend and aggression from RISK; the spotter
// and path moves skip held garrisons; path moves go through CanMarch and MarchFlags, whose land units use
// MOVEFLAG_SAFE_EMBARK_ONLY in place of native's two MOVEFLAG_NO_EMBARK rules (no naval escort in the list, or a first
// turn ending outside a zone we dominate).
bool CvTacticalOperationalLoop::PositionUnitsAroundTarget(CvTacticalAI& kTactical, const vector<CvUnit*>& vUnits, CvPlot* pTarget,
	eOperationalKind eKind, PlayerTypes eCounterpart, const int (&aiMapped)[STacticalFlavors::NUM_SLOTS], const OperationalSettings& settings,
	SOperationalEngagementReport& report)
{
	//try to improve visibility. however, if the target is too far away this may fail ... in that case we chance it
	vector<CvUnit*> vSpotters;
	for (vector<CvUnit*>::const_iterator it = vUnits.begin(); it != vUnits.end(); ++it)
		if (!IsHeldGarrison(*it))
			vSpotters.push_back(*it);
	kTactical.ExecuteSpotterMove(vSpotters, pTarget);

	//first round: in case there are enemies around, do a combat simulation
	vector<CvUnit*> vSimUnits; //make a copy we can modify!
	for (vector<CvUnit*>::const_iterator it = vUnits.begin(); it != vUnits.end(); ++it)
		if ((*it)->canUseNow())
			vSimUnits.push_back(*it);
	bool bTactSimSuccess = vSimUnits.empty() ? false : ExecuteEngagement(kTactical.m_pPlayer->GetID(), vSimUnits, pTarget, eKind, eCounterpart,
		aiMapped, settings, true, report);

	//sometimes tactsim cannot use all units, eg if they are too far out
	vector<CvUnit*> farout;
	for (vector<CvUnit*>::const_iterator it = vUnits.begin(); it != vUnits.end(); ++it)
	{
		CvUnit* pUnit = *it;
		if (pUnit->TurnProcessed() || IsHeldGarrison(pUnit))
			continue;

		// This plot distance function call is valid since the update function was called in SearchAndExecute
		if (bTactSimSuccess && TacticalAIHelpers::GetPlotDistanceToTarget(pUnit->plot()->GetPlotIndex(), pUnit->getDomainType()) <= TACTICAL_COMBAT_MAX_TARGET_DISTANCE)
			continue; //do not end the turn ... we may want to shuffle them around later

		farout.push_back(pUnit);
	}

	//we want to move the civilians last so they have a better chance of getting cover
	std::stable_sort(farout.begin(), farout.end(), PrOperationalSortCombatFirst());

	//second round: move in as long as there is no danger and we're still far away
	for (vector<CvUnit*>::const_iterator it = farout.begin(); it != farout.end(); ++it)
	{
		CvUnit* pUnit = *it;
		//since we know the unit was far out originally, this is guaranteed to be actual movement
		if (!CanMarch(kTactical, pUnit, pTarget))
			continue;
		kTactical.ExecuteMoveToPlot(pUnit, pTarget, true, MarchFlags(*pUnit, *pTarget));
	}

	//third round: if the unit is in an army (no tactical moves) and did not move yet, move it to safety now
	for (vector<CvUnit*>::const_iterator it = vUnits.begin(); it != vUnits.end(); ++it)
	{
		CvUnit* pUnit = *it;
		//don't move in further if we're already close
		if (pUnit->TurnProcessed() || pUnit->getArmyID() == -1)
			continue;

		//only flee if we're in danger, and not too far ideally
		if (pUnit->GetDanger() > pUnit->ActualHealRate(pUnit->plot()) || (!pUnit->IsCombatUnit() && pUnit->plot()->getNumDefenders(pUnit->getOwner()) == 0))
		{
			CvPlot* pPlot = TacticalAIHelpers::FindClosestSafePlotForHealing(pUnit, false).first;
			//second chance for emergencies
			if (!pPlot)
				pPlot = TacticalAIHelpers::FindSafestPlotInReach(pUnit, false, false).first;
			if (pPlot)
				pUnit->PushMission(CvTypes::getMISSION_MOVE_TO(), pPlot->getX(), pPlot->getY(), 0, false, false, MISSIONAI_TACTMOVE);
		}

		if (pUnit->canMove())
			pUnit->PushMission(CvTypes::getMISSION_SKIP());
		pUnit->SetTurnProcessed(true);
	}

	return bTactSimSuccess;
}

//------------------------------------------------------------------------------
// danger by threat
//------------------------------------------------------------------------------

// Copy of CvDangerPlotContents::GetDanger(const CvUnit*, const SUnitIDValueContainer&, int, AirActionType). Change: each
// enemy unit's and city's damage is listed as it is added up.
int CvTacticalOperationalLoop::GetDangerByThreat(CvDangerPlotContents& kContents, const CvUnit* pUnit, const SUnitIDValueContainer& extraUnitDamage,
	int iExtraDamage, AirActionType eAirAction, vector<SOperationalThreatDamage>& threats)
{
	threats.clear();
	CvPlot* pPlot = kContents.m_pPlot;
	if (!pPlot || !pUnit)
		return 0;

	// Air units only take damage from interceptions
	if (pUnit->getDomainType() == DOMAIN_AIR)
		return kContents.GetAirUnitDamage(pUnit, eAirAction);

	//otherwise calculate from scratch
	int iPlotDamage = 0;

	CvCity* pFriendlyCity = pPlot->isFriendlyCity(*pUnit) ? pPlot->getPlotCity() : NULL;

	// Civilians can be captured
	if (pUnit->IsCivilianUnit())
	{
		// If plot contains an enemy unit, mark it as max danger
		if (pPlot->isEnemyUnit(pUnit->getOwner(),true,true))
			return MAX_INT;

		if (pFriendlyCity)
		{
			// Can't hide in a city forever
			if (pFriendlyCity->isInDangerOfFalling())
				return MAX_INT;
			else
				return 0;
		}

		//need to differentiate between plots that the enemy can move into and those merely under ranged attack
		for (DangerUnitVector::iterator it = kContents.m_apCaptureUnits.begin(); it < kContents.m_apCaptureUnits.end(); ++it)
		{
			CvUnit* pAttacker = GET_PLAYER(it->first).getUnit(it->second);
			if (pAttacker && !pAttacker->isDelayedDeath() && !pAttacker->IsDead())
				return MAX_INT;
		}

		//attacks are less important for civilians ...
		for (DangerUnitVector::iterator it = kContents.m_apUnits.begin(); it < kContents.m_apUnits.end(); ++it)
		{
			CvUnit* pAttacker = GET_PLAYER(it->first).getUnit(it->second);
			if ( pAttacker && !pAttacker->isDelayedDeath() && !pAttacker->IsDead() )
			{
				int iDummy = 0;
				int iDamage = TacticalAIHelpers::GetSimulatedDamageFromAttackOnUnit(pUnit, pAttacker, pPlot, pAttacker->plot(), iDummy, false, 0, 0, true);
				if (!pPlot->IsKnownVisibleToTeam(pAttacker->getTeam()))
					iDamage = (iDamage * 80) / 100; //there's a chance they won't spot us
				iPlotDamage += iDamage;
				SOperationalThreatDamage threat = { it->first, it->second, false, iDamage };
				threats.push_back(threat);
			}
		}

		// Static damage
		iPlotDamage += kContents.m_iImprovementDamage;
		iPlotDamage += kContents.m_bFlatPlotDamage ? pPlot->getTurnDamage(pUnit->ignoreTerrainDamage(), pUnit->ignoreFeatureDamage(), pUnit->extraTerrainDamage(), pUnit->extraFeatureDamage()) : 0;

		// Potential damage from enemy units in fog
		iPlotDamage += kContents.m_iFogCount*OPERATIONAL_FOG_DANGER;

		// Damage from cities
		for (DangerCityVector::iterator it = kContents.m_apCities.begin(); it < kContents.m_apCities.end(); ++it)
		{
			CvCity* pCity = GET_PLAYER(it->first).getCity(it->second);
			if (!pCity || pCity->getTeam() == pUnit->getTeam())
				continue;

			int iCityDamage = pCity->rangeCombatDamage(pUnit, false, pPlot);
			iPlotDamage += iCityDamage;
			SOperationalThreatDamage threat = { it->first, it->second, true, iCityDamage };
			threats.push_back(threat);
		}

		return iPlotDamage;
	}

	// Capturing a city with a garrisoned unit destroys the garrisoned unit
	if (pFriendlyCity)
	{
		int iCityDanger = kContents.GetDanger(pFriendlyCity, pUnit, extraUnitDamage);
		if (iCityDanger + pFriendlyCity->getDamage() < pFriendlyCity->GetMaxHitPoints() + 50) //add a margin for error
		{
			if (pUnit->CanGarrison())
			{
				// Reconstruct the amount of damage the garrison would absorb for the city
				int iUnitShare = (iCityDanger*2*pUnit->GetMaxHitPoints()) / pFriendlyCity->GetMaxHitPoints();

				// Damage from features
				return iUnitShare + kContents.m_iImprovementDamage;
			}
			else
				return 0;
		}
		else
		{
			return MAX_INT;
		}
	}

	// Damage from units
	for (DangerUnitVector::iterator it = kContents.m_apUnits.begin(); it < kContents.m_apUnits.end(); ++it)
	{
		CvUnit* pAttacker = GET_PLAYER(it->first).getUnit(it->second);
		if (!pAttacker || pAttacker->isDelayedDeath() || pAttacker->IsDead())
			continue;

		//there should be only very few of these, if any
		int iExtraAttackerDamage = extraUnitDamage.GetValue(it->second);
		if (iExtraAttackerDamage >= pAttacker->GetCurrHitPoints())
			continue;

		if (pAttacker->plot() != pPlot)
		{
			int iAttackerDamage = 0;
			int iEnemyRange = pAttacker->IsCanAttackRanged() ? pAttacker->GetRange() : 1;
			bool bOutOfRange = plotDistance(*pPlot, *pAttacker->plot()) > iEnemyRange;

			//if the attacker is not out of range, assume they need to move for the attack, so we don't know their plot
			int iDamage = TacticalAIHelpers::GetSimulatedDamageFromAttackOnUnit(pUnit, pAttacker, pPlot, bOutOfRange ? NULL : pAttacker->plot(), iAttackerDamage, false, iExtraAttackerDamage, iExtraDamage, true);

			//assume units will do AoE damage on move once per turn
			int iAoEDamage = pAttacker->getAoEDamageOnMove();
			if (iAoEDamage > 0)
			{
				iDamage += iAoEDamage;
			}

			if (!pPlot->IsKnownVisibleToTeam(pAttacker->getTeam()))
				iDamage = (iDamage * 80) / 100; //there's a chance they won't spot us

			if (iAttackerDamage >= pAttacker->GetCurrHitPoints() && !pAttacker->isSuicide())
				iDamage /= 2; //suicide attack with non suicide unit is unlikely

			iPlotDamage += iDamage;
			SOperationalThreatDamage threat = { it->first, it->second, false, iDamage };
			threats.push_back(threat);
		}
	}

	// Damage from cities
	for (DangerCityVector::iterator it = kContents.m_apCities.begin(); it < kContents.m_apCities.end(); ++it)
	{
		CvCity* pCity = GET_PLAYER(it->first).getCity(it->second);
		if (!pCity || pCity->getTeam() == pUnit->getTeam())
			continue;

		//there should be only very few of these, if any
		int iExtraAttackerDamage = extraUnitDamage.GetValue(-it->second);
		if (iExtraAttackerDamage >= pCity->GetMaxHitPoints() - pCity->getDamage())
			continue;

		int iCityDamage = pCity->rangeCombatDamage(pUnit, false, pPlot, true);
		//if we cannot see a potential garrison, assume there is one ...
		if (!pCity->isVisible(pUnit->getTeam(), false))
			iCityDamage *= 2;

		iPlotDamage += iCityDamage;
		SOperationalThreatDamage threat = { it->first, it->second, true, iCityDamage };
		threats.push_back(threat);
	}

	// Potential damage from enemy units in fog
	iPlotDamage += kContents.m_iFogCount * OPERATIONAL_FOG_DANGER;

	// Damage from surrounding improvements (citadel) and the plot itself
	iPlotDamage += kContents.m_iImprovementDamage;
	iPlotDamage += kContents.m_bFlatPlotDamage ? pPlot->getTurnDamage(pUnit->ignoreTerrainDamage(), pUnit->ignoreFeatureDamage(), pUnit->extraTerrainDamage(), pUnit->extraFeatureDamage()) : 0;

	//done
	return iPlotDamage;
}
