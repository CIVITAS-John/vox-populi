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
// Vox Deorum: declares the scoped expected damage roll.
#include "CvUnitCombat.h"
#include "CvDangerPlots.h"
#include "CvMilitaryAI.h"
#include "CvGrandStrategyAI.h"
#include "CvGameCoreUtils.h"
#include "CvTypes.h"
#include "VoxDeorumRL/VoxRlCapture.h"
#include <math.h>
// Vox Deorum: the host clock uses the same performance counter as cvStopWatch.
#include "cvStopWatch.h"
#include "LintFree.h"

namespace
{
	// Vox Deorum: temporarily changes projected damage without changing random combat or native cached estimates.
	class OperationalDamageRollScope
	{
	public:
		// Saves the caller's roll and installs this estimate's roll.
		explicit OperationalDamageRollScope(int iRollPer200) : m_iPrevious(operationalExpectedRollPer200)
		{
			operationalExpectedRollPer200 = iRollPer200;
		}
		// Restores the caller's roll, including when the estimate returns early.
		~OperationalDamageRollScope()
		{
			operationalExpectedRollPer200 = m_iPrevious;
		}
	private:
		// Prevents a copied scope from restoring the roll twice.
		OperationalDamageRollScope(const OperationalDamageRollScope&);
		// Prevents assignment from losing the saved roll.
		OperationalDamageRollScope& operator=(const OperationalDamageRollScope&);
		int m_iPrevious;
	};
}

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

// Vox Deorum: the most distinct enemy targets sampled in one command loop.
static const size_t OPERATIONAL_FORECAST_BUDGET = 24;
// Vox Deorum: each successive victim contributes this fraction of the previous exposure weight.
static const double OPERATIONAL_DEFENSE_EXPOSURE_DECAY = 0.8;
// Vox Deorum: native caps melee hits by attackers that cannot move after attacking.
static const int OPERATIONAL_UNIT_MELEE_CAP = 3;

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

// Vox Deorum: uses the shared L and T for the full-strength stop; discounted help sets the score.
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

// Vox Deorum: orders two offers identically in selection and diagnostic ready ranks.
bool OperationalOfferBefore(const SOperationalBattlefield& field, bool bReinforce, int iOffer,
	const SOperationalBattlefield& best, bool bBestReinforce, int iBest)
{
	const double estimate = bReinforce ? field.fReinforceEstimate : field.fEstimate;
	const double bestEstimate = bBestReinforce ? best.fReinforceEstimate : best.fEstimate;
	const int rank = OfferRank(field.eCenter, bReinforce), bestRank = OfferRank(best.eCenter, bBestReinforce);
	if (estimate == bestEstimate && rank == bestRank && field.iPlotIndex == best.iPlotIndex)
		return iOffer < iBest;
	return OfferOutranks(estimate, rank, field.iPlotIndex, bestEstimate, bestRank, best.iPlotIndex);
}

// Vox Deorum: sorts only ready offer identities without copying battlefields.
struct PrOperationalOfferBefore
{
	const vector<SOperationalBattlefield>& fields;
	// Vox Deorum: borrows the current turn's queue for this sort.
	PrOperationalOfferBefore(const vector<SOperationalBattlefield>& value) : fields(value) {}
	// Vox Deorum: applies the selection order, including full ties.
	bool operator()(int i, int j) const
	{
		return OperationalOfferBefore(fields[i / 2], i % 2 != 0, i, fields[j / 2], j % 2 != 0, j);
	}
};

// Bombardment alone leaves a city at 1 HP, where it can still heal, so damage past R - 1 needs a capturer.
double AttackFallShare(double fRealized, int iHeal, int iRemaining, bool bCapture, int iRejected)
{
	const double fRemaining = static_cast<double>(std::max(1, iRemaining));
	if (bCapture && fRealized >= fRemaining)
		return 1.0;
	return std::max(0.0, std::min(ScoredDamage(fRealized, iRejected), fRemaining - 1.0) - iHeal) / fRemaining;
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

// Vox Deorum: finalizes the whole call after turn scratch storage has been reclaimed.
struct SOperationalLoopScope
{
	SOperationalPerformance& timing;
	SOperationalTurnRecord* pRecord;
	SOperationalPerformance* pPerformance;
	CvTacticalAI& tactical;
	unsigned long long startNs;
	bool bTiming;
	// Vox Deorum: retains the counters that outlive the working turn.
	SOperationalLoopScope(SOperationalPerformance& t, SOperationalTurnRecord* r, SOperationalPerformance* p, CvTacticalAI& ai, unsigned long long start, bool enabled) :
		timing(t), pRecord(r), pPerformance(p), tactical(ai), startNs(start), bTiming(enabled) {}
	// Vox Deorum: includes teardown in whole-call time and emits the no-capture timing line once per pass.
	~SOperationalLoopScope()
	{
		timing.totalNs = bTiming ? CvTacticalOperationalLoop::ClockNs() - startNs : 0;
		const long long remainder = static_cast<long long>(timing.totalNs) - static_cast<long long>(timing.setupNs +
			timing.estimateNs + timing.selectNs + timing.readinessNs + timing.executeNs + timing.retireNs + timing.recordNs);
		ASSERT(remainder >= 0, "Operational stages exceed whole-loop time");
		timing.otherNs = remainder > 0 ? static_cast<unsigned long long>(remainder) : 0;
		if (pRecord) pRecord->performance = timing;
		if (pPerformance) *pPerformance = timing;
		if (!pRecord && GC.getLogging() && GC.getAILogging())
			tactical.LogTacticalMessage(CvString::format("command loop finished in %.0f ms: %d commands, %d checks, %d estimates (%.0f ms), %d searches (%.0f ms)",
				timing.totalNs / 1000000.0, (int)timing.commands, (int)timing.checks,
				(int)timing.estimates, timing.estimateNs / 1000000.0,
				(int)timing.engagements, timing.searchNs / 1000000.0));
	}
};

// Vox Deorum: closes host command work on both normal return and unwinding.
struct SOperationalCommandScope
{
	CvTacticalAI& tactical;
	// Vox Deorum: opens one command's host work entry.
	SOperationalCommandScope(CvTacticalAI& ai, int command, int zone) : tactical(ai)
	{ CvTacticalOperationalLoop::BeginCommand(tactical, command, zone); }
	// Vox Deorum: retains committed effects when command execution stops.
	~SOperationalCommandScope() { CvTacticalOperationalLoop::EndCommand(tactical); }
};

// Picks the top ready offer, checks it, executes it on a pass, retires the offers it covered, then estimates every
// open offer again and wakes waiting fights on helpful changes.
void CvTacticalOperationalLoop::RunCommandLoop(CvTacticalAI& kTactical, SOperationalTurnRecord* pRecord, SOperationalPerformance* pPerformance, bool bTotalOnly)
{
	const bool bTiming = pRecord || pPerformance || (GC.getLogging() && GC.getAILogging());
	const unsigned long long startNs = bTiming ? ClockNs() : 0;
	SOperationalPerformance timing;
	SOperationalLoopScope closure(timing, pRecord, pPerformance, kTactical, startNs, bTiming);
	SOperationalTurn turn;
	turn.bTiming = bTiming && !bTotalOnly;
	try
	{
		{
			SOperationalTimer timer(turn.bTiming ? &turn.performance.setupNs : NULL);
			BuildTurn(kTactical, turn);
			BuildBattlefields(kTactical, turn);
			BuildThreats(turn);
		}
		if (pRecord)
		{
			SOperationalTimer timer(turn.bTiming ? &turn.performance.recordNs : NULL);
			*pRecord = SOperationalTurnRecord();
			pRecord->bRan = true;
			pRecord->settings = turn.settings;
			GetDefaultOperationalSettings(&pRecord->defaults);
			pRecord->weights = turn.weights;
			for (int i = 0; i < STacticalFlavors::NUM_SLOTS; i++)
				pRecord->aiMapped[i] = turn.aiMapped[i];
			pRecord->fTypicalUnitWorth = turn.fTypicalUnitWorth;
			pRecord->fCityScale = turn.fCityScale;
			pRecord->fCityUnit = turn.fCityUnit;
		}
		{
			SOperationalTimer timer(turn.bTiming ? &turn.performance.estimateNs : NULL);
			EstimateOffers(kTactical, turn, NULL);
		}
		if (pRecord)
			RecordStep(turn, *pRecord);

		SOperationalTurnRecord localRecord;
		SOperationalTurnRecord& record = pRecord ? *pRecord : localRecord;
		const bool bLog = !pRecord && GC.getLogging() && GC.getAILogging();
		if (bLog)
			kTactical.LogTacticalMessage(CvString::format("Operational step 0 scales: Z_T %.0f, Z_C %.1f, C %.4f", turn.fTypicalUnitWorth,
														  turn.fCityScale, turn.fCityUnit));
		const int iMaxFailures = std::max(1, static_cast<int>(turn.weights.afConstant[OPERATIONAL_CONSTANT_READINESS_MAX_FAILURES]));
		// Vox Deorum: native hosts rank offers directly without copying battlefield observations.
		const bool bProvider = HasOperationalProvider();
		const bool bPermittedMenu = bProvider && UsesPermittedMenu();
		int iCommands = 0;
		for (;;)
		{
			if (IsHostStopped())
			{
				if (pRecord) record.steps.back().szEnd = "stopped";
				break;
			}
			SOperationalChoiceDecision choice;
			{
				SOperationalTimer timer(turn.bTiming ? &turn.performance.selectNs : NULL, &turn.performance.recordNs);
				choice.request.identity = DecisionIdentity(turn, NULL);
				choice.request.identity.iCommand = iCommands;
				choice.request.identity.iStep = iCommands;
				for (int iSlot = 0; iSlot < STacticalFlavors::NUM_SLOTS; iSlot++)
					choice.request.aiMapped[iSlot] = turn.aiMapped[iSlot];
				if (bProvider)
				{
					for (std::list<int>::const_iterator it = kTactical.m_CurrentTurnUnits.begin(); it != kTactical.m_CurrentTurnUnits.end();
						 ++it)
					{
						CvUnit* pUnit = kTactical.m_pPlayer->getUnit(*it);
						if (pUnit && pUnit->canUseNow())
							choice.request.vAvailable.push_back(pUnit->GetID());
					}
				}
				int iNative = -1;
				for (size_t i = 0; i < turn.battlefields.size(); i++)
				{
					const SOperationalBattlefield& field = turn.battlefields[i];
					for (int iOffer = 0; iOffer < 2; iOffer++)
					{
						if ((iOffer ? field.eReinforce : field.eFight) != OPERATIONAL_OFFER_READY)
							continue;
						if (iNative < 0 ||
							OperationalOfferBefore(field, iOffer == 1, static_cast<int>(i) * 2 + iOffer,
								turn.battlefields[iNative / 2], iNative % 2 != 0, iNative))
							iNative = static_cast<int>(i) * 2 + iOffer;
						if (bProvider)
						{
							SOperationalSelectableOffer offer;
							offer.iOffer = static_cast<int>(i) * 2 + iOffer;
							offer.field = field;
							offer.bReinforce = iOffer == 1;
							choice.request.offers.push_back(offer);
						}
					}
				}
				if (bPermittedMenu) choice.request.battlefields = turn.battlefields;
				if (iNative < 0 && !bPermittedMenu)
				{
					if (pRecord)
						pRecord->steps.back().szEnd = "noReadyOffer";
					break;
				}
				if (iCommands >= turn.settings.iMaxCommands)
				{
					record.bCommandCap = true;
					if (pRecord)
						pRecord->steps.back().szEnd = "commandCap";
					if (bLog)
						kTactical.LogTacticalMessage(
							CvString::format("Operational step %d stopped at the command cap of %d", iCommands, turn.settings.iMaxCommands));
					break;
				}
				choice.request.iNative = iNative;
				{
					SOperationalTimer providerTimer(turn.bTiming ? &turn.performance.choiceNs : NULL);
					choice.iRequested = bProvider ? ChooseCommand(choice.request) : iNative;
				}
				choice.bInvalid = bProvider && !bPermittedMenu;
				for (size_t i = 0; i < choice.request.offers.size(); i++)
					if (choice.request.offers[i].iOffer == choice.iRequested)
						choice.bInvalid = false;
				choice.iEffective = choice.bInvalid ? choice.request.iNative : choice.iRequested;
				if (pRecord)
				{
					SOperationalTimer recordTimer(turn.bTiming ? &turn.performance.recordNs : NULL);
					record.choices.push_back(choice);
				}
			}
			// Vox Deorum: a permitted objective can close without a native battlefield, or end its menu.
			if (bPermittedMenu && choice.iEffective == -2) continue;
			if (choice.iEffective < 0)
			{
				if (pRecord) record.steps.back().szEnd = "noReadyOffer";
				break;
			}
			const int iBest = choice.iEffective / 2;
			const bool bBestReinforce = choice.iEffective % 2 != 0;
			const double fBest = bBestReinforce ? turn.battlefields[iBest].fReinforceEstimate : turn.battlefields[iBest].fEstimate;
			SOperationalBattlefield& field = turn.battlefields[iBest];
			SOperationalCheckRecord check;
			check.iBattlefield = iBest;
			check.iStep = iCommands;
			check.bReinforce = bBestReinforce;
			check.eBefore = bBestReinforce ? field.eReinforce : field.eFight;
			check.fBefore = fBest;
			// Vox Deorum: a fight check can also change its paired Reinforce, which the trace records separately.
			const eOperationalOfferState ePairedBefore = field.eReinforce;
			const double fPairedBefore = field.fReinforceEstimate;
			const unsigned long long readinessBefore = turn.performance.readinessNs;
			eOperationalCheckResult eResult;
			{
				SOperationalTimer timer(turn.bTiming ? &turn.performance.readinessNs : NULL);
				// Vox Deorum: only native fight selection initiates forecast path checks.
				eResult = !bProvider && !bBestReinforce
					? CheckForecastReadiness(kTactical, turn, field) : OPERATIONAL_CHECK_PASSED;
				if (eResult == OPERATIONAL_CHECK_PASSED)
					eResult = bBestReinforce ? CheckReinforce(kTactical, turn, field) : CheckFight(kTactical, turn, field);
				turn.performance.checks++;
			}
			if (bProvider) ReportExecution(choice.iEffective, eResult, NULL);
			check.eResult = eResult;
			if (bBestReinforce && eResult != OPERATIONAL_CHECK_PASSED)
				field.eReinforce = eResult == OPERATIONAL_CHECK_REDUCED && field.fReinforceEstimate > 0.0 ? OPERATIONAL_OFFER_READY
																										  : OPERATIONAL_OFFER_INACTIVE;
			// Vox Deorum: a reduced forecast competes again without consuming a failed readiness attempt.
			else if (eResult == OPERATIONAL_CHECK_REDUCED)
				field.eFight = field.fEstimate > 0.0 ? OPERATIONAL_OFFER_READY : OPERATIONAL_OFFER_INACTIVE;
			else if (eResult != OPERATIONAL_CHECK_PASSED)
			{
				field.iFailures++;
				field.eFight = field.iFailures >= iMaxFailures ? OPERATIONAL_OFFER_SKIPPED : OPERATIONAL_OFFER_WAITING;
				if (eResult == OPERATIONAL_CHECK_NO_RECRUITS || eResult == OPERATIONAL_CHECK_NO_DEFENDERS)
				{
					field.bReleaseLocal = true;
					CvPlot* pTarget = GC.getMap().plotByIndex(field.iPlotIndex);
					if (pTarget && (field.eReinforce == OPERATIONAL_OFFER_READY || field.eReinforce == OPERATIONAL_OFFER_INACTIVE))
					{
						SOperationalLocalSet local;
						GatherTargetLocalSet(kTactical, turn.ePlayer, *pTarget, local, false);
						field.reinforce.bChecked = false;
						EstimateReinforce(kTactical, turn, local, field);
						field.eReinforce = field.fReinforceEstimate > 0.0 ? OPERATIONAL_OFFER_READY : OPERATIONAL_OFFER_INACTIVE;
					}
				}
			}
			check.eAfter = bBestReinforce ? field.eReinforce : field.eFight;
			check.fAfter = bBestReinforce ? field.fReinforceEstimate : field.fEstimate;
			if (pRecord)
			{
				SOperationalTimer timer(turn.bTiming ? &turn.performance.recordNs : NULL);
				record.steps.back().readinessNs += turn.performance.readinessNs - readinessBefore;
				SOperationalEvent event = {"check",		   iBest,		  static_cast<int>(record.checks.size()),
										   bBestReinforce, check.eBefore, check.eAfter,
										   check.fBefore,  check.fAfter};
				record.steps.back().events.push_back(event);
				if (!bBestReinforce && (field.eReinforce != ePairedBefore || field.fReinforceEstimate != fPairedBefore))
				{
					SOperationalEvent paired = {"update", iBest, event.iRecord, true, ePairedBefore, field.eReinforce,
												fPairedBefore, field.fReinforceEstimate};
					record.steps.back().events.push_back(paired);
				}
				record.checks.push_back(check);
				if (check.eAfter == OPERATIONAL_OFFER_SKIPPED)
				{
					record.vSkipped.push_back(iBest);
				}
			}
			if (bLog)
			{
				static const char* const aszResults[] = {"passed",
														 "failed: target gone",
														 "failed: no recruits",
														 "failed: no damage",
														 "failed: no capturer",
														 "failed: no defenders",
														 "failed: no nearby enemy",
														 "failed: no reachable unit",
														 "reduced"};
				if (bBestReinforce)
				{
					const SOperationalReinforce& reinforce = field.reinforce;
					kTactical.LogTacticalMessage(CvString::format(
						"Operational step %d check of Reinforce %d at plot %d, estimate %.6f: %s; theater %.6f, stake %.6f, L %.0f, T %.0f, "
						"need %.0f, selected strength %.0f, help %.0f, share %.6f, share with help %.6f, swing %.6f, role unit %d, role help "
						"%.0f, missing role %d",
						iCommands, iBest, field.iPlotIndex, field.fReinforceEstimate, aszResults[eResult], field.fTheater, field.stake.fStake,
						reinforce.fLocal, reinforce.fEnemy, reinforce.fNeed, reinforce.fSelectedStrength, reinforce.fHelp, reinforce.fShare,
						reinforce.fShareWithHelp, reinforce.fSwing, reinforce.iRoleUnit, reinforce.fRoleHelp, reinforce.bMissingRole ? 1 : 0));
				}
				else
					kTactical.LogTacticalMessage(CvString::format(
						"Operational step %d check of fight %d at plot %d, estimate %.6f: %s; theater %.6f, destruction %.6f, exposure %.6f, "
						"ground %.6f, relief %.6f, city progress %.6f, garrison %.6f",
						iCommands, iBest, field.iPlotIndex, fBest, aszResults[eResult], field.fTheater, field.stake.fDestruction,
						field.stake.fExposure, field.stake.fGround, field.stake.fRelief, field.stake.fCityProgress, field.stake.fGarrison));
			}
			if (eResult != OPERATIONAL_CHECK_PASSED)
				continue;

			// the record is kept before the offer runs, so a command that ends the run is still recorded
			SOperationalCommandRecord localCommand;
			if (pRecord)
				record.commands.push_back(SOperationalCommandRecord());
			SOperationalCommandRecord& command = pRecord ? record.commands.back() : localCommand;
			command.iStep = iCommands;
			command.iCommand = iCommands;
			command.iBattlefield = iBest;
			command.iPlotIndex = field.iPlotIndex;
			command.eKind = bBestReinforce									 ? OPERATIONAL_KIND_REINFORCE
							: field.eCenter == OPERATIONAL_CENTER_OUR_CITY	 ? OPERATIONAL_KIND_OWN_CITY_FIGHT
							: field.eCenter == OPERATIONAL_CENTER_ENEMY_CITY ? OPERATIONAL_KIND_CITY_FIGHT
																			 : OPERATIONAL_KIND_UNIT_FIGHT;
			command.eOwner = field.eOwner;
			// a Reinforce runs at its checked estimate, which a replaced selection may have raised
			command.fEstimate = bBestReinforce ? field.fReinforceEstimate : fBest;
			command.iFailuresBefore = bBestReinforce ? 0 : field.iFailures;
			command.fTheater = field.fTheater;
			command.fSwing = bBestReinforce ? field.reinforce.fSwing : 1.0;
			command.stake = field.stake;

			if (pRecord)
			{
				SOperationalTimer timer(turn.bTiming ? &turn.performance.recordNs : NULL);
				SOperationalEvent event = {
					"command", iBest, iCommands, bBestReinforce, check.eAfter, OPERATIONAL_OFFER_EXECUTED, check.fAfter, command.fEstimate};
				record.steps.back().events.push_back(event);
				record.steps.back().iCommand = iCommands;
				record.steps.back().szEnd = "command";
			}
			SOperationalBoard before;
			SnapshotBoard(turn, before);
			const unsigned long long executeBefore = turn.performance.executeNs;
			{
				SOperationalTimer timer(turn.bTiming ? &turn.performance.executeNs : NULL, &turn.performance.recordNs);
				turn.performance.commands++;
				SOperationalCommandScope commandScope(kTactical, iCommands, field.iZoneID);
				if (bBestReinforce)
					ExecuteReinforce(kTactical, turn, field, command);
				else
					ExecuteFight(kTactical, turn, field, command);
			}
			if (pRecord)
			{
				SOperationalTimer timer(turn.bTiming ? &turn.performance.recordNs : NULL);
				record.steps.back().executeNs = turn.performance.executeNs - executeBefore;
			}

			if (bProvider) ReportExecution(choice.iEffective, OPERATIONAL_CHECK_PASSED, &command);
			iCommands++;
			const bool bRefused = command.admission.eResult == OPERATIONAL_ADMISSION_REFUSED;
			if (bBestReinforce)
				field.eReinforce = bRefused ? OPERATIONAL_OFFER_REFUSED : OPERATIONAL_OFFER_EXECUTED;
			else
			{
				field.eFight = bRefused ? OPERATIONAL_OFFER_REFUSED : OPERATIONAL_OFFER_EXECUTED;
				field.bReleaseLocal = command.bSearchRan && !bRefused;
			}
			turn.iStateRevision++;
			if (pRecord && bRefused)
			{
				SOperationalTimer timer(turn.bTiming ? &turn.performance.recordNs : NULL);
				SOperationalEvent& commandEvent = record.steps.back().events.back();
				commandEvent.eAfter = OPERATIONAL_OFFER_REFUSED;
				SOperationalEvent refusal = commandEvent;
				refusal.szType = "refusal";
				refusal.eBefore = refusal.eAfter;
				refusal.fBefore = refusal.fAfter;
				record.steps.back().events.push_back(refusal);
			}
			if (IsHostStopped())
			{
				if (pRecord)
					pRecord->steps.back().szEnd = "stopped";
				break;
			}
			SOperationalChanges changes;
			{
				SOperationalTimer retireTimer(turn.bTiming ? &turn.performance.retireNs : NULL);

				// the exemptions read the threats after the command, since its search may have moved units into a target's reach
				BuildThreats(turn);

				for (size_t i = 0; i < turn.battlefields.size(); i++)
				{
					SOperationalBattlefield& other = turn.battlefields[i];
					// an enemy-unit fight that ran its search covered the unit fights around its target
					if (!bBestReinforce && command.bSearchRan && RetiresFight(field, other))
					{
						other.eFight = OPERATIONAL_OFFER_RETIRED;
						command.vRetired.push_back(other.iID);
					}
					// a Reinforce that moved a unit covered the enemy groups it counted
					if (bBestReinforce && command.bMoved && RetiresReinforce(field, other))
					{
						other.eReinforce = OPERATIONAL_OFFER_RETIRED;
						command.vRetiredReinforce.push_back(other.iID);
					}
					// reach checks hold only until a command changes the board
					other.reinforce.bChecked = false;
				}

				SOperationalBoard after;
				SnapshotBoard(turn, after);
				DiffBoard(turn.ePlayer, before, after, changes);

				// a city this command captured becomes one of our cities, with its own offers from the next estimate on
				for (size_t i = 0; i < changes.vCapturedPlots.size(); i++)
				{
					CvPlot* pPlot = GC.getMap().plotByIndex(changes.vCapturedPlots[i]);
					CvCity* pCity = pPlot ? pPlot->getPlotCity() : NULL;
					if (!pCity || pCity->getOwner() != turn.ePlayer)
						continue;
					bool bKnown = false;
					for (size_t j = 0; j < turn.battlefields.size() && !bKnown; j++)
						bKnown =
							turn.battlefields[j].eCenter == OPERATIONAL_CENTER_OUR_CITY && turn.battlefields[j].iTargetID == pCity->GetID();
					if (!bKnown)
						AddOwnCityBattlefield(kTactical, turn, *pCity);
				}
			}
			if (bLog)
				kTactical.LogTacticalMessage(CvString::format(
					"Operational step %d command %d: %s %d at plot %d, search %s, %d fights and %d Reinforce retired", command.iStep,
					command.iCommand, bBestReinforce ? "Reinforce" : "fight", iBest, command.iPlotIndex,
					command.bSearchRan ? "ran" : "did not run", (int)command.vRetired.size(), (int)command.vRetiredReinforce.size()));

			{
				SOperationalTimer timer(turn.bTiming ? &turn.performance.estimateNs : NULL);
				EstimateOffers(kTactical, turn, &changes);
			}
			if (pRecord)
				RecordStep(turn, *pRecord);
		}
	}
	catch (...)
	{
		if (pRecord && !pRecord->steps.empty()) pRecord->steps.back().szEnd = "stopped";
		CloseRecord(turn, pRecord);
		timing = turn.performance;
		throw;
	}
	CloseRecord(turn, pRecord);
	timing = turn.performance;
}

//------------------------------------------------------------------------------
// turn, threats and battlefields
//------------------------------------------------------------------------------

// Vox Deorum: resolves mapped weights, typical unit worth, the best city value and the raw city allowance for this turn.
void CvTacticalOperationalLoop::BuildTurn(CvTacticalAI& kTactical, SOperationalTurn& turn)
{
	CvPlayer& kPlayer = *kTactical.m_pPlayer;
	const PlayerTypes ePlayer = kPlayer.GetID();
	turn.ePlayer = ePlayer;
	GetOperationalSettings(ePlayer, &turn.settings);
	GetMappedMilitaryFlavors(ePlayer, turn.aiMapped);
	ResolveOperationalWeights(turn.settings, turn.aiMapped, turn.weights);

	turn.iStrikeScan = 0;
	int iLoop = 0;
	for (CvUnit* pUnit = kPlayer.firstUnit(&iLoop); pUnit != NULL; pUnit = kPlayer.nextUnit(&iLoop))
	{
		if (!pUnit->isDelayedDeath() && (pUnit->getDomainType() == DOMAIN_LAND || pUnit->getDomainType() == DOMAIN_SEA))
			turn.iStrikeScan = std::max(turn.iStrikeScan, pUnit->baseMoves(false) * 4);
	}
	double fGoldPerWorth = 0.0;
	ReferenceUnitScales(kPlayer, turn.fTypicalUnitWorth, fGoldPerWorth);
	turn.fCityUnit = turn.weights.afConstant[OPERATIONAL_CONSTANT_CITY_UNITS];

	turn.fCityScale = 0.0;
	bool bOwnCity = false;
	for (CvCity* pCity = kPlayer.firstCity(&iLoop); pCity != NULL; pCity = kPlayer.nextCity(&iLoop))
	{
		bOwnCity = true;
		turn.fCityScale = std::max(turn.fCityScale, CityValue(*pCity, ePlayer));
	}
	if (!bOwnCity)
	{
		for (int iOther = 0; iOther < MAX_PLAYERS; iOther++)
		{
			CvPlayer& kOther = GET_PLAYER(static_cast<PlayerTypes>(iOther));
			if (!kOther.isAlive() || kOther.isBarbarian() || iOther == ePlayer || !GET_TEAM(kPlayer.getTeam()).isAtWar(kOther.getTeam()))
				continue;
			for (CvCity* pCity = kOther.firstCity(&iLoop); pCity != NULL; pCity = kOther.nextCity(&iLoop))
				turn.fCityScale = std::max(turn.fCityScale, CityValue(*pCity, ePlayer));
		}
	}
}

// Splits each exposed unit's exposure over its threats by their share of the unit's whole danger. Fog and terrain damage
// raise the exposure but belong to no threat, so they keep their share and a unit's shares add up to its exposure only
// when no such damage applies. Each threat keeps its largest allocations up to its attacks per turn.
// Spent units count, since a threat stays on them after they act; ExecuteFight recruits
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
	// Vox Deorum: allocations are scratch data, separate from the persistent victim IDs.
	map<pair<int, int>, vector<double> > allocations[2];
	int iLoop = 0;
	for (CvUnit* pUnit = kPlayer.firstUnit(&iLoop); pUnit != NULL; pUnit = kPlayer.nextUnit(&iLoop))
	{
		if (pUnit->isDelayedDeath() || pUnit->IsCivilianUnit() || pUnit->getDomainType() == DOMAIN_AIR || pUnit->getArmyID() != -1)
			continue;
		CvPlot* pPlot = pUnit->plot();
		if (!pPlot || pPlot->isFriendlyCity(*pUnit))
			continue;
		CvDangerPlotContents& contents = DangerContents(turn.ePlayer, *pPlot);
		const int iDanger = GetDangerByThreat(contents, pUnit, SUnitIDValueContainer(), 0,
			AIR_ACTION_ATTACK, threats, turn.aiMapped[STacticalFlavors::RISK]);
		int iThreatTotal = 0;
		for (size_t i = 0; i < threats.size(); i++)
			iThreatTotal += std::max(0, threats[i].iDamage);
		if (iThreatTotal <= 0)
			continue;
		turn.endangeredUnits[pUnit->GetID()] = iDanger;
		const double fExposure = UnitExposure(OurUnitWorth(*pUnit), pUnit->getDamage(), pUnit->GetMaxHitPoints(), iDanger);
		const int iShareTotal = std::max(iDanger, iThreatTotal);
		for (size_t i = 0; i < threats.size(); i++)
		{
			if (threats[i].iDamage <= 0)
				continue;
			SOperationalThreatExposure& threat = threats[i].bCity
				? turn.cityThreats[make_pair(static_cast<int>(threats[i].eOwner), threats[i].iID)]
				: turn.unitThreats[make_pair(static_cast<int>(threats[i].eOwner), threats[i].iID)];
			allocations[threats[i].bCity ? 1 : 0][make_pair(static_cast<int>(threats[i].eOwner), threats[i].iID)].push_back(
				fExposure * threats[i].iDamage / iShareTotal);
			threat.vUnitIDs.push_back(pUnit->GetID());
		}
	}

	// One attack can expose one victim. Keep all IDs for recruitment and overlap exemptions.
	for (int iKind = 0; iKind < 2; iKind++)
	{
		map<pair<int, int>, SOperationalThreatExposure>& threatMap = iKind == 0 ? turn.unitThreats : turn.cityThreats;
		for (map<pair<int, int>, SOperationalThreatExposure>::iterator it = threatMap.begin(); it != threatMap.end(); ++it)
		{
			SOperationalThreatExposure& threat = it->second;
			const CvUnit* pAttacker = iKind == 0 ? GET_PLAYER(static_cast<PlayerTypes>(it->first.first)).getUnit(it->first.second) : NULL;
			const int iAttacks = iKind == 1 ? 1 : (pAttacker ? std::max(0, pAttacker->getNumAttacks()) : 0);
			vector<double>& values = allocations[iKind][it->first];
			std::sort(values.begin(), values.end());
			for (size_t i = 0; i < values.size() && i < static_cast<size_t>(iAttacks); i++)
				threat.fExposure += values[values.size() - 1 - i];
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
			field.fImportance = turn.fCityScale > 0.0 ? std::min(1.0, CityValue(*pCity, ePlayer) / turn.fCityScale) : 0.0;
		}
		else
			continue;

		field.iID = static_cast<int>(turn.battlefields.size());
		field.iPlotIndex = pPlot->GetPlotIndex();
		field.iZoneID = target.GetDominanceZone();
		const CvTacticalDominanceZone* pZone = kTactical.GetTacticalAnalysisMap()->GetZoneByIDWithoutRefresh(field.iZoneID);
		// Vox Deorum: keep the zone city by plot so unit fights follow it after a capture.
		if (field.eCenter == OPERATIONAL_CENTER_ENEMY_UNIT)
		{
			const CvCity* pZoneCity = pZone ? pZone->GetZoneCity() : NULL;
			field.iZoneCityPlot = pZoneCity ? pZoneCity->plot()->GetPlotIndex() : -1;
		}
		field.fTheater = fTheaterFloor;
		field.fGround = pDefender ? TargetGround(ePlayer, *pPlot, pDefender) : 0.0;
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
// ID. Zones are read without a refresh, since the map cannot be rebuilt in the middle of the turn. Our city's
// importance is its value over Z_C, capped at 1.
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
	field.fImportance = turn.fCityScale > 0.0 ? std::min(1.0, CityValue(kCity, turn.ePlayer) / turn.fCityScale) : 0.0;
	field.fTheater = turn.weights.afConstant[OPERATIONAL_CONSTANT_THEATER_FLOOR];
	field.bNearOurCity = true;
	turn.battlefields.push_back(field);
}

// Vox Deorum: one cache serves the city-first estimate pass and expires when that pass ends.
void CvTacticalOperationalLoop::EstimateOffers(CvTacticalAI& kTactical, SOperationalTurn& turn, const SOperationalChanges* pChanges)
{
	turn.bEstimating = true;
	SOperationalEstimateCache cache;
	for (int iPass = 0; iPass < 2; iPass++)
	{
		for (size_t i = 0; i < turn.battlefields.size(); i++)
		{
			SOperationalBattlefield& field = turn.battlefields[i];
			if ((field.eCenter == OPERATIONAL_CENTER_ENEMY_UNIT) != (iPass == 1))
				continue;
			const bool bFightOpen = field.eFight == OPERATIONAL_OFFER_READY || field.eFight == OPERATIONAL_OFFER_INACTIVE ||
				field.eFight == OPERATIONAL_OFFER_WAITING;
			const bool bReinforceOpen = field.eReinforce == OPERATIONAL_OFFER_READY || field.eReinforce == OPERATIONAL_OFFER_INACTIVE;
			if (!bFightOpen && !bReinforceOpen && field.eCenter == OPERATIONAL_CENTER_ENEMY_UNIT)
				continue;
			turn.performance.estimates += (bFightOpen ? 1 : 0) + (bReinforceOpen ? 1 : 0);
			if (!EstimateBattlefield(kTactical, turn, cache, field, bFightOpen || bReinforceOpen, bReinforceOpen, pChanges))
			{
				if (bFightOpen)
					field.eFight = OPERATIONAL_OFFER_DROPPED;
				if (bReinforceOpen)
					field.eReinforce = OPERATIONAL_OFFER_DROPPED;
				continue;
			}
			if (field.eCenter != OPERATIONAL_CENTER_ENEMY_UNIT)
			{
				cache.cityTheaters[field.iPlotIndex] = field.fTheater;
				cache.theaters[field.iPlotIndex] = field.theater;
			}
			if (bFightOpen && field.eFight != OPERATIONAL_OFFER_WAITING)
				field.eFight = field.fEstimate > 0.0 ? OPERATIONAL_OFFER_READY : OPERATIONAL_OFFER_INACTIVE;
			if (bReinforceOpen)
				field.eReinforce = field.fReinforceEstimate > 0.0 ? OPERATIONAL_OFFER_READY : OPERATIONAL_OFFER_INACTIVE;
		}
	}
	turn.bEstimating = false;
}

// Vox Deorum: one local set feeds theater, fight stake and Reinforce; closed cities need only theater strength.
bool CvTacticalOperationalLoop::EstimateBattlefield(CvTacticalAI& kTactical, const SOperationalTurn& turn, SOperationalEstimateCache& cache,
	SOperationalBattlefield& field, bool bStake, bool bReinforce, const SOperationalChanges* pChanges)
{
	field.fEstimate = 0.0;
	CvPlot* pPlot = BattlefieldPlot(field);
	if (!pPlot)
		return false;
	SOperationalLocalSet local;
	GatherTargetLocalSet(kTactical, turn.ePlayer, *pPlot, local, bStake, field.eCenter == OPERATIONAL_CENTER_ENEMY_UNIT);
	EstimateTheater(turn, cache, *pPlot, local, field, bReinforce);
	if (!bStake)
		return true;

	// Vox Deorum: the live plot city may have a different owner and city ID after a capture.
	const CvPlot* pZonePlot = field.iZoneCityPlot >= 0 ? GC.getMap().plotByIndex(field.iZoneCityPlot) : NULL;
	const CvCity* pZoneCity = pZonePlot ? pZonePlot->getPlotCity() : NULL;
	if (!EstimateFight(kTactical, turn, cache, local, pZoneCity, field))
		return false;
	// Vox Deorum: a waking fight takes back its local units before Reinforce selects candidates.
	if (field.eFight == OPERATIONAL_OFFER_WAITING && pChanges && field.fEstimate > 0.0 &&
		WakesFight(turn.ePlayer, turn, field, *pChanges))
	{
		field.eFight = OPERATIONAL_OFFER_READY;
		field.bReleaseLocal = false;
	}
	if (bReinforce)
		EstimateReinforce(kTactical, turn, local, field);
	return true;
}

// Vox Deorum: city tallies set theater; unit tallies are needed only by an open Reinforce offer.
void CvTacticalOperationalLoop::EstimateTheater(const SOperationalTurn& turn, const SOperationalEstimateCache& cache, const CvPlot& kPlot,
	const SOperationalLocalSet& local, SOperationalBattlefield& field, bool bReinforce)
{
	const bool bUnit = field.eCenter == OPERATIONAL_CENTER_ENEMY_UNIT;
	const bool bOurCity = field.eCenter == OPERATIONAL_CENTER_OUR_CITY;
	SOperationalReinforce& reinforce = field.reinforce;
	if (!bUnit || bReinforce)
	{
		reinforce.countedUnits.clear();
		reinforce.countedCities.clear();
		SOperationalTimer timer(turn.bTiming ? &turn.performance.forceTallyNs : NULL);
		turn.performance.forceTallies++;
		if (bOurCity)
		{
			reinforce.fLocal = LocalCityStrength(*kPlot.getPlotCity(), local);
			reinforce.fEnemy = EnemyCityStrength(turn, *kPlot.getPlotCity(), &reinforce.countedUnits);
		}
		else
		{
			reinforce.fLocal = LocalStrength(local);
			reinforce.fEnemy = EnemyCenterStrength(turn.ePlayer, kPlot, local.bLand, local.bSea,
				reinforce.countedUnits, reinforce.countedCities);
		}
		reinforce.fShare = StrengthShare(reinforce.fLocal, reinforce.fEnemy,
			turn.weights.afConstant[OPERATIONAL_CONSTANT_THEATER_STEEPNESS]);
	}

	if (bUnit)
	{
		map<int, double>::const_iterator theater = cache.cityTheaters.find(field.iZoneCityPlot);
		field.fTheater = theater != cache.cityTheaters.end() ? theater->second : turn.weights.afConstant[OPERATIONAL_CONSTANT_THEATER_FLOOR];
		map<int, SOperationalTheater>::const_iterator components = cache.theaters.find(field.iZoneCityPlot);
		field.theater = components != cache.theaters.end() ? components->second : SOperationalTheater();
	}
	else
	{
		const CvCity* pCity = kPlot.getPlotCity();
		const double fAttackers = bOurCity ? reinforce.fEnemy : reinforce.fLocal;
		const double fPressure = fAttackers <= 0.0 ? 0.0 : (bOurCity ? 1.0 - reinforce.fShare : reinforce.fShare);
		field.theater.fImportance = field.fImportance;
		field.theater.fPressure = fPressure;
		field.theater.fDamageLevel = DamageShare(pCity->getDamage(), pCity->GetMaxHitPoints());
		field.theater.fContest = TheaterContest(fPressure, field.theater.fDamageLevel);
		field.fTheater = TheaterMultiplier(turn.weights.afConstant[OPERATIONAL_CONSTANT_THEATER_FLOOR], field.fImportance,
			field.theater.fContest);
	}
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

// Vox Deorum: identifies a forecast by center, owner and target ID.
static OperationalForecastKey ForecastKey(const SOperationalBattlefield& field)
{
	return OperationalForecastKey(static_cast<int>(field.eCenter), make_pair(static_cast<int>(field.eOwner), field.iTargetID));
}

// Vox Deorum: returns the chance in 1/10000 of getting past the planned interceptor and updates expected self damage.
// Non-air attacks bypass this adjustment.
static int AirStrikeChance(const CvUnit& kAttacker, const CvPlot& kTarget, const CvUnit* pInterceptor, int iRisk, int& iSelfDamage)
{
	if (kAttacker.getDomainType() != DOMAIN_AIR)
		return 10000;
	OperationalDamageRollScope roll(EnemyRollPer200(iRisk));
	return TacticalAIHelpers::GetAirStrikeChance(&kAttacker, pInterceptor, &kTarget, 0, 0, iSelfDamage);
}

// Vox Deorum: stores damage dealt and taken before interception.
struct SOperationalAttackDamage
{
	int iDamage;
	int iGarrisonDamage;
	int iSelfDamage;
};

// Vox Deorum: projects damage to the city and garrison and retaliation before interception.
static SOperationalAttackDamage ProjectCityAttackDamage(const CvCity& kCity, const CvUnit& kAttacker,
	const SOperationalCityDamage& damage, int iRisk)
{
	SOperationalAttackDamage hit = { 0, 0, 0 };
	// Vox Deorum: a replacement still supplies native city strength, but absorbs none of the later hits.
	const int iAssumedGarrisonDamage = damage.pCurrent && damage.pCurrent != damage.pGarrison
		? damage.pCurrent->GetCurrHitPoints() : damage.iTotalGarrisonDamage;
	// The first projection supplies our hit; the second supplies only the enemy-roll retaliation for the suicide test.
	{
		OperationalDamageRollScope roll(OurRollPer200(iRisk));
		hit.iDamage = TacticalAIHelpers::GetSimulatedDamageFromAttackOnCity(&kCity, &kAttacker, kAttacker.plot(), hit.iSelfDamage, hit.iGarrisonDamage,
			true, 0, damage.iCityDamage, iAssumedGarrisonDamage, true, true, damage.pCurrent);
	}
	// Native non-air ranged attacks have no self damage.
	if (!kAttacker.IsCanAttackRanged() || kAttacker.getDomainType() == DOMAIN_AIR)
	{
		OperationalDamageRollScope roll(EnemyRollPer200(iRisk));
		int iIgnoredGarrisonDamage = 0;
		TacticalAIHelpers::GetSimulatedDamageFromAttackOnCity(&kCity, &kAttacker, kAttacker.plot(), hit.iSelfDamage, iIgnoredGarrisonDamage, true, 0,
			damage.iCityDamage, iAssumedGarrisonDamage, true, true, damage.pCurrent);
	}
	return hit;
}

// Vox Deorum: projects damage to the target unit and retaliation before interception.
static SOperationalAttackDamage RawUnitAttackDamage(const CvUnit& kTarget, const CvUnit& kAttacker, int iRisk)
{
	SOperationalAttackDamage hit = { 0, 0, 0 };
	{
		OperationalDamageRollScope roll(OurRollPer200(iRisk));
		hit.iDamage = TacticalAIHelpers::GetSimulatedDamageFromAttackOnUnit(&kTarget, &kAttacker, kTarget.plot(), kAttacker.plot(),
			hit.iSelfDamage, true, 0, 0, true);
	}
	// Native non-air ranged attacks have no self damage.
	if (!kAttacker.IsCanAttackRanged() || kAttacker.getDomainType() == DOMAIN_AIR)
	{
		OperationalDamageRollScope roll(EnemyRollPer200(iRisk));
		TacticalAIHelpers::GetSimulatedDamageFromAttackOnUnit(&kTarget, &kAttacker, kTarget.plot(), kAttacker.plot(),
			hit.iSelfDamage, true, 0, 0, true);
	}
	return hit;
}

// Vox Deorum: applies the planned interceptor before testing whether the attack is worth making.
static bool ApplyInterceptionAndTest(const CvPlot& kTarget, const CvUnit& kAttacker, const CvUnit* pInterceptor,
	int iRisk, bool bCityTarget, SOperationalAttackDamage& hit)
{
	const int iStrikeChance = AirStrikeChance(kAttacker, kTarget, pInterceptor, iRisk, hit.iSelfDamage);
	hit.iDamage = hit.iDamage * iStrikeChance / 10000;
	hit.iGarrisonDamage = hit.iGarrisonDamage * iStrikeChance / 10000;
	if (!bCityTarget)
		return hit.iDamage > hit.iSelfDamage / 5 && kAttacker.GetCurrHitPoints() > hit.iSelfDamage / 2;

	const int iDamage = hit.iDamage + hit.iGarrisonDamage;
	return iDamage > hit.iSelfDamage ||
		(iDamage * 2 > hit.iSelfDamage && kAttacker.GetCurrHitPoints() - hit.iSelfDamage > kAttacker.GetMaxHitPoints() / 2);
}

// Vox Deorum: counts visible interceptors only when the damage filter has an air striker.
static int CountVisibleAirInterceptors(PlayerTypes ePlayer, const CvPlot& kTarget, const SOperationalLocalSet& local)
{
	for (size_t i = 0; i < local.vDamage.size(); i++)
		if (local.vDamage[i]->getDomainType() == DOMAIN_AIR)
			return kTarget.GetInterceptorCount(ePlayer, NULL, false, true);
	return 0;
}

// Vox Deorum: holds the planned air exchange for one entry in the damage filter.
struct SOperationalAirExchange
{
	const CvUnit* pInterceptor;
	bool bSweeps; // the fighter sweeps instead of striking
};

// Vox Deorum: plans surviving sweeps first, then assigns remaining visible interceptions using the starting damage estimates.
static vector<SOperationalAirExchange> PlanAirExchanges(PlayerTypes ePlayer, const CvPlot& kTarget, const SOperationalLocalSet& local,
	int iVisibleInterceptors, const vector<SOperationalAttackDamage>& vAirHits, int iRisk, bool bCityTarget)
{
	const SOperationalAirExchange noExchange = { NULL, false };
	vector<SOperationalAirExchange> vExchanges(local.vDamage.size(), noExchange);
	// Vox Deorum: the count is zero without air strikers or visible interceptors.
	if (iVisibleInterceptors == 0)
		return vExchanges;

	map<int, int> interceptionsUsed;
	if (!local.vSweepers.empty())
	{
		size_t iNonFighterStrikers = 0;
		for (size_t i = 0; i < local.vDamage.size(); i++)
		{
			const CvUnit* pAttacker = local.vDamage[i];
			if (pAttacker->getDomainType() == DOMAIN_AIR && pAttacker->getUnitInfo().GetDefaultUnitAIType() != UNITAI_DEFENSE_AIR)
				++iNonFighterStrikers;
		}
		// Vox Deorum: the conservative sweep cap is the smaller of the non-fighter striker and visible interceptor
		// counts. Native uses the visible count with a minimum of one, or no cap with air search on.
		const size_t iSweepLimit = std::min(iNonFighterStrikers, static_cast<size_t>(iVisibleInterceptors));
		size_t iSweeps = 0;
		for (size_t i = 0; i < local.vSweepers.size() && iSweeps < iSweepLimit; i++)
		{
			const CvUnit* pSweeper = local.vSweepers[i];
			// Select before changing the roll. A later aircraft may be visible to more interceptors.
			const CvUnit* pInterceptor = kTarget.GetBestInterceptor(ePlayer, pSweeper, false, true, NULL, &interceptionsUsed);
			if (!pInterceptor)
				continue;

			int iIgnoredInterceptorDamage = 0;
			int iDamageToSweeper = 0;
			{
				OperationalDamageRollScope roll(EnemyRollPer200(iRisk));
				TacticalAIHelpers::GetSimulatedAirSweepDamage(pSweeper, pInterceptor, pSweeper->plot(), &kTarget,
					0, 0, iIgnoredInterceptorDamage, iDamageToSweeper);
			}
			// Vox Deorum: assume a lethal sweep leaves the interception for a later strike. Native spends it either way.
			if (iDamageToSweeper >= pSweeper->GetCurrHitPoints())
				continue;

			// Ignore damage to the interceptor, so it retains its strength for any remaining interceptions.
			++iSweeps;
			++interceptionsUsed[pInterceptor->GetID()];
			// A fighter can sweep even when it is outside the damage filter.
			const vector<CvUnit*>::const_iterator attacker = std::find(local.vDamage.begin(), local.vDamage.end(), pSweeper);
			if (attacker != local.vDamage.end())
				vExchanges[attacker - local.vDamage.begin()].bSweeps = true;
		}
	}

	// The damage filter is sorted by unit ID, so the index breaks ties in that order.
	vector<pair<int, size_t> > vStrikes;
	for (size_t i = 0; i < local.vDamage.size(); i++)
	{
		if (local.vDamage[i]->getDomainType() != DOMAIN_AIR || vExchanges[i].bSweeps)
			continue;

		// Vox Deorum: a hit absorbed by the garrison still counts toward the size of the strike.
		const SOperationalAttackDamage& hit = vAirHits[i];
		vStrikes.push_back(make_pair(-(hit.iDamage + hit.iGarrisonDamage), i));
	}
	std::sort(vStrikes.begin(), vStrikes.end());
	for (size_t i = 0; i < vStrikes.size(); i++)
	{
		const size_t iAttacker = vStrikes[i].second;
		const CvUnit* pAttacker = local.vDamage[iAttacker];
		const CvUnit* pInterceptor = kTarget.GetBestInterceptor(ePlayer, pAttacker, false, true, NULL, &interceptionsUsed);
		if (!pInterceptor)
			continue;

		// Vox Deorum: a strike that fails against the starting target keeps its interceptor but leaves the use for the next strike.
		vExchanges[iAttacker].pInterceptor = pInterceptor;
		SOperationalAttackDamage hit = vAirHits[iAttacker];
		if (ApplyInterceptionAndTest(kTarget, *pAttacker, pInterceptor, iRisk, bCityTarget, hit))
			++interceptionsUsed[pInterceptor->GetID()];
	}
	return vExchanges;
}

// Vox Deorum: values what local attackers can destroy and what the fight can protect, in typical units.
bool CvTacticalOperationalLoop::EstimateFight(CvTacticalAI& kTactical, const SOperationalTurn& turn, SOperationalEstimateCache& cache, const SOperationalLocalSet& local,
	const CvCity* pZoneCity, SOperationalBattlefield& field, bool bSample)
{
	const double* afWeight = turn.weights.afWeight;
	const double fUnitScale = turn.fTypicalUnitWorth;
	const int iRisk = turn.aiMapped[STacticalFlavors::RISK];
	SOperationalStake stake;
	// Vox Deorum: only previously selected targets have a cached speculative rejection fraction.
	OperationalForecastMap::iterator it = turn.forecasts.find(ForecastKey(field));
	SOperationalForecast* pForecast = it != turn.forecasts.end() && (bSample || it->second.bChecked) ? &it->second : NULL;
	field.fEstimate = 0.0;
	if (field.eOwner == NO_PLAYER)
		return false;
	if (field.eCenter == OPERATIONAL_CENTER_ENEMY_UNIT)
	{
		CvUnit* pUnit = GET_PLAYER(field.eOwner).getUnit(field.iTargetID);
		if (!pUnit || pUnit->isDelayedDeath() || !pUnit->plot())
			return false;
		field.iPlotIndex = pUnit->plot()->GetPlotIndex();
		int iRejected = 0;
		const int iDamage = LocalUnitDamage(turn.ePlayer, *pUnit, local, iRisk, iRejected, pForecast, bSample);
		stake.fDamageShare = DamageShare(ScoredDamage(iDamage, iRejected), pUnit->GetCurrHitPoints());
		map<pair<int, int>, SOperationalThreatExposure>::const_iterator threat = turn.unitThreats.find(make_pair(static_cast<int>(field.eOwner), field.iTargetID));
		const double fExposure = threat != turn.unitThreats.end() ? threat->second.fExposure : 0.0;
		if (fUnitScale > 0.0)
		{
			const double fDestroyed = DestroyedUnitValue(*pUnit, iDamage, iRejected);
			stake.afValue[0] = fDestroyed / fUnitScale;
			stake.fDestruction = afWeight[OPERATIONAL_WEIGHT_ATTRITION] * fDestroyed / fUnitScale;
			stake.afValue[1] = fExposure / fUnitScale;
			stake.fExposure = afWeight[OPERATIONAL_WEIGHT_RISK] * fExposure / fUnitScale;
		}
		stake.afValue[2] = stake.fDamageShare * field.fGround;
		stake.fGround = stake.fDamageShare * afWeight[OPERATIONAL_WEIGHT_HOLD_GROUND] * field.fGround;
		if (pZoneCity && pZoneCity->getOwner() == turn.ePlayer)
		{
			const SOperationalCityDanger& danger = CachedCityDanger(turn, cache, *pZoneCity);
			const vector<SOperationalThreatDamage>& threats = danger.threats;
			const int iDanger = danger.iTotal;
			int iTargetDamage = 0;
			for (size_t i = 0; i < threats.size(); i++)
			{
				if (!threats[i].bCity && threats[i].eOwner == field.eOwner && threats[i].iID == field.iTargetID)
					iTargetDamage += std::max(0, threats[i].iDamage);
			}
			stake.fFallShare = DamageShare(iDanger, std::max(1, pZoneCity->GetMaxHitPoints() - pZoneCity->getDamage()));
			// City danger is the sum of these attack contributions.
			stake.fReliefShare = iDanger > 0 ? static_cast<double>(iTargetDamage) / iDanger : 0.0;
			stake.afValue[3] = stake.fDamageShare * turn.fCityUnit * stake.fFallShare * stake.fReliefShare;
			stake.fRelief = stake.fDamageShare * afWeight[OPERATIONAL_WEIGHT_HOLD_CITY] * turn.fCityUnit *
				stake.fFallShare * stake.fReliefShare;
		}
		stake.fStake = stake.fDestruction + stake.fExposure + stake.fGround + stake.fRelief;
	}
	else if (field.eCenter == OPERATIONAL_CENTER_OUR_CITY)
	{
		CvCity* pCity = GET_PLAYER(field.eOwner).getCity(field.iTargetID);
		if (!pCity || pCity->getOwner() != turn.ePlayer)
			return false;
		const int iDanger = CachedCityDanger(turn, cache, *pCity).iTotal;
		stake.fFallShare = DamageShare(iDanger, std::max(1, pCity->GetMaxHitPoints() - pCity->getDamage()));

		vector<CvUnit*> vEndangered;
		map<int, int> dangers;
		GatherCityEndangered(kTactical, turn, *pCity, vEndangered, &dangers);
		const double fExposure = CityDefenseExposure(turn, cache, vEndangered, dangers);
		field.bHasEndangered = !vEndangered.empty();

		stake.afValue[4] = turn.fCityUnit * stake.fFallShare;
		stake.fCityProgress = afWeight[OPERATIONAL_WEIGHT_HOLD_CITY] * turn.fCityUnit * stake.fFallShare;
		if (fUnitScale > 0.0)
		{
			stake.afValue[1] = fExposure / fUnitScale;
			stake.fExposure = afWeight[OPERATIONAL_WEIGHT_RISK] * fExposure / fUnitScale;
		}
		stake.fStake = stake.fCityProgress + stake.fExposure;
	}
	else
	{
		CvCity* pCity = GET_PLAYER(field.eOwner).getCity(field.iTargetID);
		if (!pCity || pCity->getOwner() != field.eOwner)
			return false;
		SOperationalCityDamage damage;
		StartCityDamage(*pCity, damage);
		bool bCanCapture = !pForecast && local.bCanCapture;
		const CvPlot& kTarget = *pCity->plot();
		// Vox Deorum: project the starting air hits before city and garrison damage accumulate.
		const int iVisibleInterceptors = CountVisibleAirInterceptors(turn.ePlayer, kTarget, local);
		vector<SOperationalAttackDamage> vAirHits;
		if (iVisibleInterceptors > 0)
		{
			vAirHits.resize(local.vDamage.size());
			for (size_t i = 0; i < local.vDamage.size(); i++)
				if (local.vDamage[i]->getDomainType() == DOMAIN_AIR)
					vAirHits[i] = ProjectCityAttackDamage(*pCity, *local.vDamage[i], damage, iRisk);
		}
		const vector<SOperationalAirExchange> vExchanges = PlanAirExchanges(turn.ePlayer, kTarget, local, iVisibleInterceptors, vAirHits, iRisk, true);
		for (size_t i = 0; i < local.vDamage.size(); i++)
		{
			if (vExchanges[i].bSweeps)
				continue;
			const CvUnit& kAttacker = *local.vDamage[i];
			eOperationalStrikeReadiness eReadiness = OPERATIONAL_STRIKE_UNCONFIRMED;
			if (pForecast)
			{
				if (StrikeNow(kAttacker, kTarget))
					eReadiness = OPERATIONAL_STRIKE_IMMEDIATE;
				else if (bSample && StrikeReachable(kAttacker, kTarget))
					eReadiness = OPERATIONAL_STRIKE_REACHABLE;

				if (CanCapture(kAttacker))
				{
					if (eReadiness == OPERATIONAL_STRIKE_REACHABLE)
						pForecast->reachableCapturers.insert(kAttacker.GetID());
					// Vox Deorum: a sampled capturer must still belong to the current local capture roster.
					if (eReadiness == OPERATIONAL_STRIKE_IMMEDIATE || pForecast->reachableCapturers.count(kAttacker.GetID()) > 0)
						bCanCapture = true;
				}
			}
			// Vox Deorum: reuse the starting hit until the simulated city or garrison changes.
			const bool bReuseHit = !vAirHits.empty() && kAttacker.getDomainType() == DOMAIN_AIR &&
				damage.iCityDamage == 0 && damage.iTotalGarrisonDamage == 0 && damage.pCurrent == damage.pGarrison;
			const SOperationalAttackDamage hit = bReuseHit ? vAirHits[i] : ProjectCityAttackDamage(*pCity, kAttacker, damage, iRisk);
			SimulateCityAttack(*pCity, kAttacker, vExchanges[i].pInterceptor, hit, damage, iRisk, pForecast, bSample, eReadiness);
		}
		if (pForecast && bSample)
		{
			pForecast->iSpeculative = damage.iSpeculative;
			pForecast->iSpeculativeGarrison = damage.iSpeculativeGarrison;
			pForecast->iRejected = damage.iRemoved;
			pForecast->iRejectedGarrison = damage.iRemovedGarrison;
		}
		stake.fFallShare = AttackFallShare(damage.iCityDamage, damage.iHeal, damage.iRemaining, bCanCapture, damage.iRemoved);
		stake.afValue[4] = turn.fCityUnit * stake.fFallShare;
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
		if (fUnitScale > 0.0)
		{
			if (damage.pGarrison && damage.iGarrisonHealth > 0)
			{
				const double fDestroyed = DestroyedUnitValue(*damage.pGarrison,
					stake.fFallShare >= 1.0 ? damage.iGarrisonHealth : damage.iGarrisonDamage, damage.iRemovedGarrison);
				stake.afValue[5] = fDestroyed / fUnitScale;
				stake.fGarrison = afWeight[OPERATIONAL_WEIGHT_ATTRITION] * fDestroyed / fUnitScale;
			}
			stake.afValue[1] = fExposure / fUnitScale;
			stake.fExposure = afWeight[OPERATIONAL_WEIGHT_RISK] * fExposure / fUnitScale;
		}
		stake.fStake = stake.fCityProgress + stake.fGarrison + stake.fExposure;
	}
	field.stake = stake;
	field.fEstimate = field.fTheater * stake.fStake;
	return true;
}

// Vox Deorum: finds a local capturer and optionally adds local strength outside the candidate pool.
static bool TallyLocalOutsideCandidates(const SOperationalLocalSet& local, const vector<SOperationalCandidate>& vCandidates,
	double* pfStrength)
{
	set<int> candidates;
	for (size_t i = 0; i < vCandidates.size(); i++)
		candidates.insert(vCandidates[i].iUnitID);
	bool bCapturer = false;
	for (size_t i = 0; i < local.vStrength.size(); i++)
	{
		const CvUnit& kUnit = *local.vStrength[i];
		if (candidates.count(kUnit.GetID()) > 0)
			continue;
		if (pfStrength)
			*pfStrength += CvTacticalOperationalLoop::UnitStrength(kUnit);
		bCapturer = bCapturer || CvTacticalOperationalLoop::IsCapturer(kUnit);
	}
	return bCapturer;
}

// Vox Deorum: the fight supplies theater and stake; incoming strength earns only the share of its outcome it swings.
void CvTacticalOperationalLoop::EstimateReinforce(CvTacticalAI& kTactical, const SOperationalTurn& turn, const SOperationalLocalSet& local,
	SOperationalBattlefield& field)
{
	SOperationalReinforce& reinforce = field.reinforce;
	field.fReinforceEstimate = 0.0;
	reinforce.fShareWithHelp = reinforce.fShare;
	reinforce.fSwing = 0.0;

	// Vox Deorum: enemy-only gates close recruitment; selection still clears the old offer and records need.
	CvPlot* pTarget = GC.getMap().plotByIndex(field.iPlotIndex);
	const bool bOurCity = field.eCenter == OPERATIONAL_CENTER_OUR_CITY;
	const bool bOpen = pTarget && (bOurCity || (field.bBorder && !field.bWithdraw));
	const double fRatio = turn.weights.afConstant[OPERATIONAL_CONSTANT_STRENGTH_RATIO];
	vector<SOperationalCandidate> vCandidates;
	if (bOpen)
		GatherReinforceCandidates(kTactical, field, *pTarget, local.bLand, local.bSea, vCandidates, field.bReleaseLocal);
	bool bLocalCapturer = local.bCapturer;
	if (field.bReleaseLocal && bOpen)
	{
		// Vox Deorum: a leftover enters the selection pool instead of counting as current strength or an existing capturer.
		// A rejected recruit no longer enters the pool, so start each selection from the live local set again.
		reinforce.fLocal = bOurCity ? CityStrength(*pTarget->getPlotCity()) : 0.0;
		bLocalCapturer = TallyLocalOutsideCandidates(local, vCandidates, &reinforce.fLocal);
		reinforce.fShare = StrengthShare(reinforce.fLocal, reinforce.fEnemy,
			turn.weights.afConstant[OPERATIONAL_CONSTANT_THEATER_STEEPNESS]);
		reinforce.fShareWithHelp = reinforce.fShare;
	}
	SelectReinforcement(vCandidates, reinforce.fLocal, reinforce.fEnemy, fRatio,
		bOpen && field.eCenter == OPERATIONAL_CENTER_ENEMY_CITY && !bLocalCapturer, reinforce);
	if (reinforce.vSelected.empty())
		return;

	reinforce.fShareWithHelp = StrengthShare(reinforce.fLocal + reinforce.fHelp, reinforce.fEnemy,
		turn.weights.afConstant[OPERATIONAL_CONSTANT_THEATER_STEEPNESS]);
	reinforce.fSwing = StrengthSwing(reinforce.fShare, reinforce.fShareWithHelp);

	// Vox Deorum: partial defense still counts; an enemy-center march must reach the desired full-strength ratio.
	if (!bOurCity && reinforce.fSelectedStrength < reinforce.fNeed)
		return;

	field.fReinforceEstimate = field.fEstimate * reinforce.fSwing;
}

// Reads the tactical roster, as native recruitment does. The filters are those of CvTacticalAI::PlotReinforcementMoves
// that need no pathfinding: the alternative-target filter waits for the readiness check. Changes: one-turn and recruit
// range bounds by plain distance; destination dominance gates apply to enemy centers only; units covering a civilian
// are left out without native's skip and processing; generals, admirals and civilian sappers fall to the combat test.
void CvTacticalOperationalLoop::GatherReinforceCandidates(CvTacticalAI& kTactical, const SOperationalBattlefield& field, const CvPlot& kTarget,
	bool bLand, bool bSea, vector<SOperationalCandidate>& vCandidates, bool bAllowLocal)
{
	vCandidates.clear();
	const CvTacticalAnalysisMap* pZoneMap = kTactical.GetTacticalAnalysisMap();
	const CvTacticalDominanceZone* pTargetZone = pZoneMap->GetZoneByIDWithoutRefresh(field.iZoneID);
	const CvCity* pTargetCity = pTargetZone ? pTargetZone->GetZoneCity() : NULL;
	const CvCity* pEnemyCity = field.eCenter == OPERATIONAL_CENTER_ENEMY_CITY ? kTarget.getPlotCity() : NULL;
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

		// Vox Deorum: keep local units for the same-field fight until it has used or released them.
		const int iDistance = plotDistance(*pUnit->plot(), kTarget);
		const bool bHeld = IsHeldGarrison(pUnit);
		if ((!bAllowLocal && iDistance <= StrikeReach(*pUnit)) ||
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

		// Vox Deorum: enemy-city help must pass the same strike filter as local attackers.
		if (pEnemyCity && !CanStrikeCity(kTactical, *pUnit, *pEnemyCity))
			continue;
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

// Vox Deorum: plain distance, no pathfinding. In-domain land and sea units add strength within strike reach; ranged
// units of either domain and aircraft in range can join the damage filter. Local attackers need a roster spot, an attack
// left and the checks from CvTacticalAI::FindUnitsWithinStrikingDistance that need no pathfinding. Fighters can strike
// with air search on if they can also sweep from their current plot.
void CvTacticalOperationalLoop::GatherLocalSet(CvTacticalAI& kTactical, PlayerTypes ePlayer, const CvPlot& kPlot, bool bLand, bool bSea,
	SOperationalLocalSet& local, bool bDamage)
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
			const bool bInDomain = (bLand && pUnit->getDomainType() == DOMAIN_LAND) || (bSea && pUnit->getDomainType() == DOMAIN_SEA);
			if ((!bInDomain && !pUnit->IsCanAttackRanged()) || iDistance > StrikeReach(*pUnit))
				continue;
			if (bInDomain)
			{
				local.vStrength.push_back(pUnit);
				local.bCapturer = local.bCapturer || IsCapturer(*pUnit);
			}
		}
		if (!bDamage || (pUnit->IsCanAttackRanged() && pUnit->isEmbarked()) ||
			roster.count(pUnit->GetID()) == 0 || !pUnit->canUseForTacticalAI() || pUnit->isOutOfAttacks() || pUnit->IsCoveringFriendlyCivilian())
			continue;

		// Vox Deorum: a sweep needs a valid target plot but does not need to pass the city strike test.
		if (pUnit->getUnitInfo().GetDefaultUnitAIType() == UNITAI_DEFENSE_AIR)
		{
			if (pUnit->getDomainType() != DOMAIN_AIR || !pUnit->canAirSweepAt(kPlot.getX(), kPlot.getY()))
				continue;
			local.vSweepers.push_back(pUnit);
			if (!MOD_VOX_TACTICAL_AIR_SEARCH)
				continue;
		}
		if (!pCity || CanStrikeCity(kTactical, *pUnit, *pCity))
		{
			local.vDamage.push_back(pUnit);
			local.bCanCapture = local.bCanCapture || CanCapture(*pUnit);
		}
	}
	std::sort(local.vDamage.begin(), local.vDamage.end(), PrOperationalUnitID());
	std::sort(local.vSweepers.begin(), local.vSweepers.end(), PrOperationalUnitID());
}

// Vox Deorum: gathers local units, keeping unit fight estimates in the plot's terrain domain even at a city.
void CvTacticalOperationalLoop::GatherTargetLocalSet(CvTacticalAI& kTactical, PlayerTypes ePlayer, const CvPlot& kTarget,
	SOperationalLocalSet& local, bool bDamage, bool bUnitTarget)
{
	const CvCity* pCity = bUnitTarget ? NULL : kTarget.getPlotCity();
	GatherLocalSet(kTactical, ePlayer, kTarget, pCity != NULL || !kTarget.isWater(),
		pCity ? pCity->isCoastal() : kTarget.isWater(), local, bDamage);
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
	if (damage.pCurrent && (damage.pCurrent->isDelayedDeath() || damage.pCurrent->IsDead()))
		damage.pCurrent = NULL;
	damage.pGarrison = damage.pCurrent;
	damage.iGarrisonHealth = damage.pCurrent ? damage.pCurrent->GetCurrHitPoints() : 0;
	damage.iCurrentHealth = damage.iGarrisonHealth;
	damage.iRemaining = std::max(1, kCity.GetMaxHitPoints() - kCity.getDamage());
	damage.iHeal = ExpectedCityHeal(kCity);
}

// Vox Deorum: copies the city case of CvTacticalAI::ComputeTotalExpectedDamage for one attacker. Applies the planned
// interceptor to the quick estimate and tests it against the accumulated city and garrison state. Applies the readiness
// discount without changing the move list. A suicidal attack leaves the garrison unchanged. Replacement hits count
// toward city damage only, and a garrison predicted to die cannot return.
void CvTacticalOperationalLoop::SimulateCityAttack(const CvCity& kCity, const CvUnit& kAttacker, const CvUnit* pInterceptor,
	SOperationalAttackDamage hit, SOperationalCityDamage& damage, int iRisk, SOperationalForecast* pForecast, bool bSample,
	eOperationalStrikeReadiness eReadiness)
{
	if (!ApplyInterceptionAndTest(*kCity.plot(), kAttacker, pInterceptor, iRisk, true, hit))
		return;

	int iCityDamage = hit.iDamage;
	int iGarrisonDamage = hit.iGarrisonDamage;
	const bool bStartingGarrison = damage.pCurrent != NULL && damage.pCurrent == damage.pGarrison;
	// Vox Deorum: reject unreachable hits before they alter hypothetical city or garrison health.
	if (pForecast && eReadiness != OPERATIONAL_STRIKE_IMMEDIATE)
	{
		damage.iSpeculative += iCityDamage;
		if (bStartingGarrison)
			damage.iSpeculativeGarrison += iGarrisonDamage;
		if (bSample)
		{
			if (eReadiness == OPERATIONAL_STRIKE_UNCONFIRMED)
			{
				damage.iRemoved += iCityDamage;
				if (bStartingGarrison)
					damage.iRemovedGarrison += iGarrisonDamage;
				return;
			}
		}
		else
		{
			const int iRemoved = RejectedDamage(damage.iSpeculative, pForecast->iRejected, pForecast->iSpeculative);
			const int iRemovedGarrison = RejectedDamage(damage.iSpeculativeGarrison,
				pForecast->iRejectedGarrison, pForecast->iSpeculativeGarrison);
			iCityDamage -= iRemoved - damage.iRemoved;
			if (bStartingGarrison)
				iGarrisonDamage -= iRemovedGarrison - damage.iRemovedGarrison;
			damage.iRemoved = iRemoved;
			damage.iRemovedGarrison = iRemovedGarrison;
		}
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
}

// Vox Deorum: counts city fire without changing the move list, then tallies local hits from largest to smallest.
int CvTacticalOperationalLoop::LocalUnitDamage(PlayerTypes ePlayer, const CvUnit& kTarget, const SOperationalLocalSet& local, int iRisk, int& iRejected,
	SOperationalForecast* pForecast, bool bSample)
{
	CvPlayer& kPlayer = GET_PLAYER(ePlayer);
	CvPlot* pTargetPlot = kTarget.plot();
	// Vox Deorum: share the starting air hits between interception planning and the damage tally.
	const int iVisibleInterceptors = CountVisibleAirInterceptors(ePlayer, *pTargetPlot, local);
	vector<SOperationalAttackDamage> vAirHits;
	if (iVisibleInterceptors > 0)
	{
		vAirHits.resize(local.vDamage.size());
		for (size_t i = 0; i < local.vDamage.size(); i++)
			if (local.vDamage[i]->getDomainType() == DOMAIN_AIR)
				vAirHits[i] = RawUnitAttackDamage(kTarget, *local.vDamage[i], iRisk);
	}
	const vector<SOperationalAirExchange> vExchanges = PlanAirExchanges(ePlayer, *pTargetPlot, local, iVisibleInterceptors, vAirHits, iRisk, false);
	int iDamage = 0;
	int iLoop = 0;
	for (CvCity* pCity = kPlayer.firstCity(&iLoop); pCity != NULL; pCity = kPlayer.nextCity(&iLoop))
	{
		if (!pCity->canRangeStrikeAt(pTargetPlot->getX(), pTargetPlot->getY()) || pCity->isMadeAttack() || pCity->getStrengthValue(true) <= 0)
			continue;
		OperationalDamageRollScope roll(OurRollPer200(iRisk));
		iDamage += pCity->rangeCombatDamage(&kTarget, false, pTargetPlot);
	}
	int iMeleeCount = 0;
	iRejected = 0;
	int iSpeculative = 0;
	int iRealizableDamage = iDamage;
	int iRealizableMelee = 0;
	// Vox Deorum: the melee cap keeps the largest hits, so attackers are taken by damage, then by unit ID.
	vector<pair<int, size_t> > vHits;
	for (size_t i = 0; i < local.vDamage.size(); i++)
	{
		if (vExchanges[i].bSweeps)
			continue;
		const CvUnit& kAttacker = *local.vDamage[i];
		SOperationalAttackDamage hit = !vAirHits.empty() && kAttacker.getDomainType() == DOMAIN_AIR
			? vAirHits[i] : RawUnitAttackDamage(kTarget, kAttacker, iRisk);
		// Vox Deorum: the unsampled tally counts every hit that passes the native test; forecasts count positive hits.
		if (ApplyInterceptionAndTest(*pTargetPlot, kAttacker, vExchanges[i].pInterceptor, iRisk, false, hit) && (!pForecast || hit.iDamage > 0))
			vHits.push_back(make_pair(-hit.iDamage, i));
	}
	std::sort(vHits.begin(), vHits.end());
	for (size_t h = 0; h < vHits.size(); h++)
	{
		const CvUnit& kAttacker = *local.vDamage[vHits[h].second];
		const int iRawDamage = -vHits[h].first;
		const bool bCapped = !kAttacker.IsCanAttackRanged() && !kAttacker.canMoveAfterAttacking();
		const bool bStockAdmitted = !bCapped || iMeleeCount < OPERATIONAL_UNIT_MELEE_CAP;
		if (bCapped)
			iMeleeCount++;
		const bool bImmediate = pForecast && StrikeNow(kAttacker, *pTargetPlot);
		if (bStockAdmitted)
		{
			iDamage += iRawDamage;
			if (pForecast && !bImmediate)
				iSpeculative += iRawDamage;
		}
		if (pForecast && bSample && (!bCapped || iRealizableMelee < OPERATIONAL_UNIT_MELEE_CAP) &&
			(bImmediate || StrikeReachable(kAttacker, *pTargetPlot)))
		{
			iRealizableDamage += iRawDamage;
			if (bCapped)
				iRealizableMelee++;
		}
	}
	if (pForecast)
	{
		if (bSample)
		{
			pForecast->iSpeculative = iSpeculative;
			pForecast->iRejected = std::max(0, std::min(iSpeculative, iDamage - iRealizableDamage));
		}
		iRejected = RejectedDamage(iSpeculative, pForecast->iRejected, pForecast->iSpeculative);
	}
	return iDamage - iRejected;
}

// Vox Deorum: preserves admitted air estimates and legal immediate attacks from the actual origin.
bool CvTacticalOperationalLoop::StrikeNow(const CvUnit& kUnit, const CvPlot& kTarget)
{
	if (kUnit.getDomainType() == DOMAIN_AIR)
		return true;
	if (!kUnit.canMove() || kUnit.isOutOfAttacks())
		return false;
	if (kUnit.IsCanAttackRanged())
		return kUnit.canRangeStrikeAt(kTarget.getX(), kTarget.getY());
	int iFlags = CvUnit::MOVEFLAG_ATTACK | CvUnit::MOVEFLAG_DESTINATION;
	if (kTarget.isCity())
	{
		iFlags |= CvUnit::MOVEFLAG_NO_EMBARK;
		if (!kTarget.isEnemyCity(kUnit) || !kUnit.IsCanAttackWithMove() ||
			(kUnit.plot()->needsEmbarkation(&kUnit) && !kUnit.canDisembarkOnto(*kUnit.plot(), kTarget, true, iFlags)))
			return false;
	}
	return kUnit.plot()->isAdjacent(&kTarget) && kUnit.canMoveInto(kTarget, iFlags);
}

// Vox Deorum: copies source-independent target checks from CvUnit::canRangeStrikeAt.
bool CvTacticalOperationalLoop::CanRangeStrikeTarget(const CvUnit& kUnit, const CvPlot& kTarget)
{
	if (kUnit.isEmbarked() || kUnit.GetRange() <= 0 || kUnit.GetBaseRangedCombatStrength() <= 0 || kUnit.isOutOfAttacks())
		return false;
	if (kTarget.isCity())
		return kTarget.isEnemyCity(kUnit) && (kUnit.AI_getUnitAIType() != UNITAI_MISSILE_AIR || kUnit.rangeStrikeTarget(kTarget, true) != NULL);
	const CvUnit* pDefender = kUnit.rangeStrikeTarget(kTarget, true);
	if (!pDefender)
		return false;
	if (MOD_GLOBAL_SUBS_UNDER_ICE_IMMUNITY && pDefender->getInvisibleType() == 0)
	{
		if (kTarget.getFeatureType() == FEATURE_ICE && kUnit.getInvisibleType() != 0)
			return false;
		if (kUnit.getUnitCombatType() == GC.getInfoTypeForString("UNITCOMBAT_ARCHER", true))
			return false;
	}
	return true;
}

// Vox Deorum: tests legal attack bases independently without reserving firing tiles for other attackers.
bool CvTacticalOperationalLoop::StrikeReachable(const CvUnit& kUnit, const CvPlot& kTarget)
{
	if (!kUnit.canMove() || kUnit.isOutOfAttacks() || kUnit.getDomainType() == DOMAIN_AIR)
		return false;
	const bool bRanged = kUnit.IsCanAttackRanged();
	if (bRanged && !CanRangeStrikeTarget(kUnit, kTarget))
		return false;
	int iFlags = CvUnit::MOVEFLAG_ATTACK | CvUnit::MOVEFLAG_DESTINATION;
	if (kTarget.isCity())
	{
		iFlags |= CvUnit::MOVEFLAG_NO_EMBARK;
		if (!kTarget.isEnemyCity(kUnit) || (!bRanged && !kUnit.IsCanAttackWithMove()))
			return false;
	}
	if (!bRanged && !kUnit.canMoveInto(kTarget, iFlags))
		return false;
	const ReachablePlots reach = TacticalAIHelpers::GetAllPlotsInReachThisTurn(&kUnit, kUnit.plot(),
		CvUnit::MOVEFLAG_IGNORE_DANGER | CvUnit::MOVEFLAG_NO_EMBARK, 1);
	for (ReachablePlots::const_iterator it = reach.begin(); it != reach.end(); ++it)
	{
		if (it->iMovesLeft <= 0 || it->iPlotIndex == kTarget.GetPlotIndex())
			continue;
		const CvPlot* pBase = GC.getMap().plotByIndexUnchecked(it->iPlotIndex);
		if (!kUnit.canEndTurnAtPlot(pBase))
			continue;
		if (bRanged)
		{
			if (kUnit.canEverRangeStrikeAt(kTarget.getX(), kTarget.getY(), pBase, false))
				return true;
		}
		else if (pBase->isAdjacent(&kTarget) && (!kTarget.isCity() || !pBase->needsEmbarkation(&kUnit) ||
			kUnit.canDisembarkOnto(*pBase, kTarget, true, iFlags)))
			return true;
	}
	return false;
}

// Vox Deorum: rounds a cached fraction against the current speculative amount only.
int CvTacticalOperationalLoop::RejectedDamage(int iCurrent, int iRejected, int iSampled)
{
	return iSampled > 0 ? static_cast<int>((static_cast<long long>(iCurrent) * iRejected + iSampled / 2) / iSampled) : 0;
}

// Vox Deorum: samples a target on first selection while the command loop has a forecast budget left.
eOperationalCheckResult CvTacticalOperationalLoop::CheckForecastReadiness(CvTacticalAI& kTactical, const SOperationalTurn& turn,
	SOperationalBattlefield& field)
{
	if (field.eCenter == OPERATIONAL_CENTER_OUR_CITY)
		return OPERATIONAL_CHECK_PASSED;
	const OperationalForecastKey key = ForecastKey(field);
	if (turn.forecasts.find(key) != turn.forecasts.end() || turn.forecasts.size() >= OPERATIONAL_FORECAST_BUDGET)
		return OPERATIONAL_CHECK_PASSED;
	CvPlot* pTarget = BattlefieldPlot(field);
	if (!pTarget)
		return OPERATIONAL_CHECK_PASSED;
	SOperationalForecast& forecast = turn.forecasts[key];
	SOperationalLocalSet local;
	GatherTargetLocalSet(kTactical, turn.ePlayer, *pTarget, local, true, field.eCenter == OPERATIONAL_CENTER_ENEMY_UNIT);
	CvPlot* pZone = field.iZoneCityPlot >= 0 ? GC.getMap().plotByIndex(field.iZoneCityPlot) : NULL;
	SOperationalEstimateCache cache;
	const double fBefore = field.fEstimate;
	EstimateFight(kTactical, turn, cache, local, pZone ? pZone->getPlotCity() : NULL, field, true);
	forecast.bChecked = true;
	if (field.fEstimate < fBefore)
	{
		// Vox Deorum: retain reinforcement admission gates while immediately correcting its inherited fight score.
		if (field.fReinforceEstimate > 0.0)
		{
			field.fReinforceEstimate = field.fEstimate * field.reinforce.fSwing;
			if (field.fReinforceEstimate <= 0.0 && field.eReinforce == OPERATIONAL_OFFER_READY)
				field.eReinforce = OPERATIONAL_OFFER_INACTIVE;
		}
		return OPERATIONAL_CHECK_REDUCED;
	}
	return OPERATIONAL_CHECK_PASSED;
}

// Vox Deorum: normalizes threat shares only when identified damage exceeds whole danger.
SOperationalVictimExposure CvTacticalOperationalLoop::AttributeExposure(double fExposure, int iDanger,
	const vector<SOperationalThreatDamage>& threats)
{
	SOperationalVictimExposure result;
	int iTotal = 0;
	for (size_t i = 0; i < threats.size(); i++)
		iTotal += std::max(0, threats[i].iDamage);
	const int iDenominator = std::max(iDanger, iTotal);
	if (iDenominator > 0)
		for (size_t i = 0; i < threats.size(); i++)
			if (threats[i].iDamage > 0)
				result.shares[make_pair(threats[i].bCity, make_pair(static_cast<int>(threats[i].eOwner), threats[i].iID))] +=
					fExposure * threats[i].iDamage / iDenominator;
	result.fResidual = iDenominator > 0 ? fExposure * (iDenominator - iTotal) / iDenominator : fExposure;
	return result;
}

// Vox Deorum: retains each source's largest allowed shares, then applies geometric decay to the sorted victim exposures.
double CvTacticalOperationalLoop::CityDefenseExposure(const SOperationalTurn& turn, SOperationalEstimateCache& cache,
	const vector<CvUnit*>& vEndangered, const map<int, int>& dangers)
{
	vector<double> exposures(vEndangered.size(), 0.0);
	map<pair<bool, pair<int, int> >, vector<pair<double, int> > > allocations;
	const int iRisk = turn.aiMapped[STacticalFlavors::RISK];
	for (size_t i = 0; i < vEndangered.size(); i++)
	{
		const CvUnit& kVictim = *vEndangered[i];
		map<int, SOperationalVictimExposure>::iterator stored = cache.victimExposure.find(kVictim.GetID());
		if (stored == cache.victimExposure.end())
		{
			const int iDanger = dangers.find(kVictim.GetID())->second;
			const double fExposure = UnitExposure(OurUnitWorth(kVictim), kVictim.getDamage(), kVictim.GetMaxHitPoints(), iDanger);
			CvDangerPlotContents& contents = DangerContents(turn.ePlayer, *kVictim.plot());
			vector<SOperationalThreatDamage> threats;
			SOperationalVictimExposure shares;
			if (kVictim.plot()->isFriendlyCity(kVictim))
			{
				// Vox Deorum: use native's pretend-garrison projection and retain environmental danger with the victim.
				const CvCity* pCity = kVictim.plot()->getPlotCity();
				const int iCityDanger = GetCityDangerByThreat(contents, pCity, &kVictim, SUnitIDValueContainer(), 0, &threats, iRisk);
				const bool bFalls = iCityDanger + pCity->getDamage() >= pCity->GetMaxHitPoints();
				const int iHostile = bFalls ? iDanger : (kVictim.CanGarrison() ? std::max(0, iDanger - contents.m_iImprovementDamage) : 0);
				shares = AttributeExposure(fExposure, 0, threats);
				if (iDanger > 0 && !shares.shares.empty())
				{
					const double fFraction = static_cast<double>(iHostile) / iDanger;
					for (map<pair<bool, pair<int, int> >, double>::iterator it = shares.shares.begin(); it != shares.shares.end(); ++it)
						it->second *= fFraction;
					shares.fResidual = fExposure * (1.0 - fFraction);
				}
			}
			else
			{
				GetDangerByThreat(contents, &kVictim, SUnitIDValueContainer(), 0, AIR_ACTION_ATTACK, threats, iRisk);
				shares = AttributeExposure(fExposure, iDanger, threats);
			}
			stored = cache.victimExposure.insert(make_pair(kVictim.GetID(), shares)).first;
		}
		exposures[i] = stored->second.fResidual;
		for (map<pair<bool, pair<int, int> >, double>::const_iterator it = stored->second.shares.begin(); it != stored->second.shares.end(); ++it)
			if (it->second > 0.0)
				// Vox Deorum: equal shares keep the earlier native roster victim.
				allocations[it->first].push_back(make_pair(it->second, -static_cast<int>(i)));
	}
	for (map<pair<bool, pair<int, int> >, vector<pair<double, int> > >::iterator it = allocations.begin(); it != allocations.end(); ++it)
	{
		const CvUnit* pAttacker = it->first.first ? NULL :
			GET_PLAYER(static_cast<PlayerTypes>(it->first.second.first)).getUnit(it->first.second.second);
		const int iAttacks = it->first.first ? 1 : (pAttacker ? std::max(0, pAttacker->getNumAttacks()) : 0);
		vector<pair<double, int> >& shares = it->second;
		std::sort(shares.begin(), shares.end(), std::greater<pair<double, int> >());
		for (size_t i = 0; i < shares.size() && i < static_cast<size_t>(iAttacks); i++)
			exposures[-shares[i].second] += shares[i].first;
	}
	std::sort(exposures.begin(), exposures.end(), std::greater<double>());
	double fTotal = 0.0;
	double fWeight = 1.0;
	for (size_t i = 0; i < exposures.size(); i++)
	{
		fTotal += fWeight * exposures[i];
		fWeight *= OPERATIONAL_DEFENSE_EXPOSURE_DECAY;
	}
	return fTotal;
}

// Vox Deorum: reads danger contents before installing any roll, so cache refreshes retain native expected damage.
int CvTacticalOperationalLoop::UnitDanger(const SOperationalTurn& turn, const CvUnit& kUnit)
{
	SOperationalTimer dangerTimer(turn.bTiming && turn.bEstimating ? &turn.performance.dangerReadNs : NULL);
	if (turn.bEstimating) turn.performance.dangerReads++;
	CvDangerPlotContents& contents = DangerContents(turn.ePlayer, *kUnit.plot());
	vector<SOperationalThreatDamage> threats;
	return GetDangerByThreat(contents, &kUnit, SUnitIDValueContainer(), 0, AIR_ACTION_ATTACK, threats,
		turn.aiMapped[STacticalFlavors::RISK]);
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

// Vox Deorum: reads each plot within 4 once. Visible hostile cities and a revealed center city add strength with at
// least 1 HP, including their garrison. Other hostile units add strength only while their plots are visible.
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
		if (!pPlot)
			continue;
		const bool bVisible = pPlot->isVisible(eTeam);
		if (!bVisible && !(pPlot == &kTarget && pPlot->isRevealed(eTeam)))
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
		// Vox Deorum: knowing the center city never reveals units on its fogged plot.
		if (!bVisible)
			continue;
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
	vector<CvUnit*>& vEndangered, map<int, int>* pDangers)
{
	vEndangered.clear();
	if (pDangers)
		pDangers->clear();
	const CvPlot* pCityPlot = kCity.plot();
	for (list<int>::const_iterator it = kTactical.m_CurrentTurnUnits.begin(); it != kTactical.m_CurrentTurnUnits.end(); ++it)
	{
		CvUnit* pUnit = CityRecruit(kTactical, *it);
		if (!pUnit || pUnit->getDomainType() == DOMAIN_AIR || plotDistance(*pUnit->plot(), *pCityPlot) > TACTICAL_COMBAT_MAX_TARGET_DISTANCE)
			continue;
		map<int, int>::const_iterator danger = turn.endangeredUnits.find(pUnit->GetID());
		const int iDanger = danger != turn.endangeredUnits.end() ? danger->second :
			(pUnit->plot()->isFriendlyCity(*pUnit) ? UnitDanger(turn, *pUnit) : 0);
		if (danger != turn.endangeredUnits.end() || iDanger > 0)
		{
			vEndangered.push_back(pUnit);
			if (pDangers)
				(*pDangers)[pUnit->GetID()] = iDanger;
		}
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
		SOperationalTimer timer(turn.bTiming ? &turn.performance.cityFireNs : NULL);
		CvUnit* pDefender = pPlot->getBestDefender(NO_PLAYER, ePlayer, NULL, true, true);
		if (!pDefender || pDefender->isDelayedDeath())
			return;
		record.bCityFireAttempted = kTactical.FindCitiesWithinStrikingDistance(pPlot);
		if (record.bCityFireAttempted && kTactical.ExecuteAttackWithCities(pDefender))
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

	vector<CvUnit*> vNativeSpotters = vUnits;
	// Vox Deorum: sweep aircraft are candidates even when air search is off and they act greedily.
	for (size_t i = 0; i < kTactical.m_CurrentAirSweepUnits.size(); i++)
	{
		CvUnit* pSweeper = kPlayer.getUnit(kTactical.m_CurrentAirSweepUnits[i].GetID());
		if (pSweeper && std::find(vUnits.begin(), vUnits.end(), pSweeper) == vUnits.end())
			vUnits.push_back(pSweeper);
	}
	if (MOD_VOX_TACTICAL_AIR_SEARCH)
	{
		size_t nUnitsBeforeCarriers = vUnits.size();
		for (size_t i = 0; i < nUnitsBeforeCarriers; i++)
		{
			// carriers belong to operations and are never tactical units on their own, so only the unit's state is checked
			CvUnit* pShip = (vUnits[i] && vUnits[i]->getDomainType() == DOMAIN_AIR) ? vUnits[i]->getTransportUnit() : NULL;
			if (pShip && pShip->canUseNow() && std::find(vUnits.begin(), vUnits.end(), pShip) == vUnits.end())
				vUnits.push_back(pShip);
		}
	}
	if (!AdmitParticipants(kTactical, turn, field, vUnits, record))
		return;
	if (record.admission.eResult == OPERATIONAL_ADMISSION_SUBSET)
		FilterRecruits(kTactical, vUnits);
	vector<CvUnit*> vSpotters;
	for (size_t i = 0; i < vNativeSpotters.size(); i++)
		if (!IsHeldGarrison(vNativeSpotters[i]) && std::find(vUnits.begin(), vUnits.end(), vNativeSpotters[i]) != vUnits.end())
			vSpotters.push_back(vNativeSpotters[i]);
	bool bSearch;
	{
		SOperationalTimer timer(turn.bTiming ? &turn.performance.spottingNs : NULL);
		bSearch = kTactical.ExecuteSpotterMove(vSpotters, pPlot);
	}
	if (bSearch && !MOD_VOX_TACTICAL_AIR_SEARCH)
	{
		SOperationalTimer timer(turn.bTiming ? &turn.performance.greedyAirNs : NULL);
		//first handle air units (including missiles)
		kTactical.ExecuteAirSweep(pPlot);
		kTactical.ExecuteAirAttack(pPlot);

		//did the air attack already kill the enemy?
		if (pPlot->getBestDefender(NO_PLAYER, ePlayer, NULL, true, true) == NULL && !pPlot->isCity())
			bSearch = false;
	}

	//the search takes the units still usable after the spotter and air actions, aircraft only with the air search; an
	//empty group has no search and retires nothing
	SOperationalEngagementReport& report = record.engagement;
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
				turn.aiMapped, turn.settings, true, report, &turn, &record);
	}

	//the city follow-up runs after any attack attempt, as in native ExecuteCaptureCityMoves
	if (bCity)
	{
		// Did it work?  If so, don't need a temporary dominance zone if had one here
		if (pPlot->getOwner() == ePlayer)
			kTactical.DeleteFocusArea(pPlot);

		//do we have embarked units we need to put ashore
		if (kTactical.FindEmbarkedUnitsAroundTarget(pPlot, 4))
		{
			if (record.admission.eResult == OPERATIONAL_ADMISSION_SUBSET)
				FilterRecruits(kTactical, vUnits);
			kTactical.ExecuteLandingOperation(pPlot);
		}
	}

	MarkDispositions(report, record);
}

// Gathers the city's recruits, records its endangered ones, and runs one engagement over them with no counterpart,
// since native gives no civ modifier in our own zone.
void CvTacticalOperationalLoop::ExecuteCityDefense(CvTacticalAI& kTactical, const SOperationalTurn& turn, const SOperationalBattlefield& field,
	SOperationalCommandRecord& record)
{
	record.eKind = OPERATIONAL_KIND_OWN_CITY_FIGHT;
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

	if (!AdmitParticipants(kTactical, turn, field, vUnits, record))
		return;
	SOperationalEngagementReport& report = record.engagement;
	record.bSearchRan = !vUnits.empty();
	if (record.bSearchRan)
		ExecuteEngagement(kPlayer.GetID(), vUnits, pPlot, OPERATIONAL_KIND_OWN_CITY_FIGHT, NO_PLAYER, turn.aiMapped, turn.settings,
			false, report, &turn, &record);
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

	SOperationalTimer reachTimer(turn.bTiming ? &turn.performance.reinforceReachNs : NULL);
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
			turn.performance.reinforceReachChecks++;
			const bool bReach = ReinforceReach(kTactical, pUnit, pTarget, relevantPlots, searchPlots);
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
		// Vox Deorum: only the rejected selection changed; reuse the current theater, stake and strength tallies.
		SOperationalLocalSet local;
		GatherTargetLocalSet(kTactical, turn.ePlayer, *pTarget, local, false);
		EstimateReinforce(kTactical, turn, local, field);
	}
	field.reinforce.bChecked = true;
	if (field.reinforce.vSelected.empty())
		return OPERATIONAL_CHECK_NO_REACHABLE;
	if (field.fReinforceEstimate < fBefore)
		return OPERATIONAL_CHECK_REDUCED;
	return OPERATIONAL_CHECK_PASSED;
}

// Vox Deorum: one reach contract for the checked native selection and any provider-added recruits.
bool CvTacticalOperationalLoop::ReinforceReach(CvTacticalAI& kTactical, CvUnit* pUnit, CvPlot* pTarget,
	const ReachablePlots& relevantPlots, map<int, ReachablePlots>& searchPlots)
{
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
	return bReach;
}

// The group is the checked selection still usable now. The move label is native's reinforcement label; at an enemy
// center the target's owner is the counterpart, while our city has none, as in its fight.
void CvTacticalOperationalLoop::ExecuteReinforce(CvTacticalAI& kTactical, const SOperationalTurn& turn, const SOperationalBattlefield& field,
	SOperationalCommandRecord& record)
{
	record.eKind = OPERATIONAL_KIND_REINFORCE;
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
	const bool bAdmitted = AdmitParticipants(kTactical, turn, field, vUnits, record);
	record.vSelected = record.admission.vEffective;
	if (!bAdmitted)
		return;
	startPlots.clear();
	for (size_t i = 0; i < vUnits.size(); i++)
		startPlots[vUnits[i]->GetID()] = vUnits[i]->plot()->GetPlotIndex();

	// Vox Deorum: restrict air recon only for an explicit subset; native admission keeps gathered sweep units.
	if (record.admission.eResult == OPERATIONAL_ADMISSION_SUBSET)
		FilterRecruits(kTactical, vUnits);
	SOperationalEngagementReport& report = record.engagement;
	record.bSearchRan = true;
	PositionUnitsAroundTarget(kTactical, vUnits, pTarget, OPERATIONAL_KIND_REINFORCE,
		field.eCenter == OPERATIONAL_CENTER_OUR_CITY ? NO_PLAYER : field.eOwner, turn.aiMapped, turn.settings, report, &turn, &record);

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
		candidate.eFight == OPERATIONAL_OFFER_SKIPPED || candidate.eFight == OPERATIONAL_OFFER_DROPPED ||
		candidate.eFight == OPERATIONAL_OFFER_REFUSED)
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
	const double fBase = UnitBaseWorth(kInfo);
	double fUpgrade = 0.0;
	const UnitTypes eUpgrade = kUnit.GetUpgradeUnitType();
	if (eUpgrade != NO_UNIT && kUnit.CanUpgradeRightNow(true))
	{
		const CvUnitEntry* pUpgrade = GC.getUnitInfo(eUpgrade);
		if (pUpgrade)
			fUpgrade = UpgradePremium(fBase, UnitBaseWorth(*pUpgrade), gradeUpgradeDiscount);
	}
	return UnitWorth(fBase, kUnit.getLevel(), gradeSeniorityPerLevel) + fUpgrade;
}

// Vox Deorum: values the enemy's lost health and retained share without an upgrade premium.
double CvTacticalOperationalLoop::DestroyedUnitValue(const CvUnit& kUnit, double fRealized, int iRejected)
{
	const double fWorth = UnitWorth(UnitBaseWorth(kUnit.getUnitInfo()), kUnit.getLevel(), gradeSeniorityPerLevel);
	return DestroyedValue(fWorth, kUnit.GetCurrHitPoints(), kUnit.GetMaxHitPoints(), fRealized, iRejected);
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
// in plot weight.
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

// Vox Deorum: the ground the target holds, in plot weight: the score with it less the score without it.
double CvTacticalOperationalLoop::TargetGround(PlayerTypes eUs, const CvPlot& kTarget, const CvUnit* pTarget)
{
	return GroundAround(eUs, kTarget, NULL) - GroundAround(eUs, kTarget, pTarget);
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

// Vox Deorum: gives each callback an identity before the host stamps its branch revision.
SOperationalDecisionIdentity CvTacticalOperationalLoop::DecisionIdentity(const SOperationalTurn& turn,
	const SOperationalCommandRecord* pCommand)
{
	SOperationalDecisionIdentity identity;
	identity.ePlayer = turn.ePlayer;
	identity.iDecision = turn.iNextDecision++;
	identity.iRevision = turn.iStateRevision;
	if (pCommand)
	{
		identity.iCommand = pCommand->iCommand;
		identity.iStep = pCommand->iStep;
		identity.iBattlefield = pCommand->iBattlefield;
		identity.eKind = pCommand->eKind;
		identity.iTargetPlot = pCommand->iPlotIndex;
	}
	StampDecision(identity);
	return identity;
}

// Vox Deorum: the same stable scan as the original queue, including every OfferOutranks tie break.
int CvTacticalOperationalLoop::NativeCommandChoice(const SOperationalChoiceRequest& request)
{
	int iBest = -1;
	for (size_t i = 0; i < request.offers.size(); i++)
	{
		const SOperationalSelectableOffer& offer = request.offers[i];
		const SOperationalBattlefield& field = offer.field;
		if (iBest >= 0)
		{
			const SOperationalSelectableOffer& best = request.offers[iBest];
			if (!OperationalOfferBefore(field, offer.bReinforce, offer.iOffer,
				best.field, best.bReinforce, best.iOffer))
				continue;
		}
		iBest = static_cast<int>(i);
	}
	return iBest < 0 ? -1 : request.offers[iBest].iOffer;
}

// Vox Deorum: recomputes only the chosen group's contribution; the checked offer keeps its execution priority.
void CvTacticalOperationalLoop::AdmittedReinforcement(const vector<SOperationalCandidate>& candidates, bool bNeedCapturer,
	const vector<int>& vAdmitted, double fSteepness, SOperationalReinforce& out)
{
	out.vSelected = vAdmitted;
	out.fSelectedStrength = 0.0;
	out.fHelp = 0.0;
	out.fRoleHelp = 0.0;
	out.iRoleUnit = -1;
	bool bHasCapturer = !bNeedCapturer;
	for (size_t i = 0; i < vAdmitted.size(); i++)
		for (size_t j = 0; j < candidates.size(); j++)
			if (candidates[j].iUnitID == vAdmitted[i])
			{
				const SOperationalCandidate& candidate = candidates[j];
				const double fHelp = candidate.fStrength / std::max(1, candidate.iJoinTurns);
				out.fSelectedStrength += candidate.fStrength;
				out.fHelp += fHelp;
				if (!bHasCapturer && candidate.bCapturer && out.fSelectedStrength - candidate.fStrength >= out.fNeed)
				{
					out.iRoleUnit = candidate.iUnitID;
					out.fRoleHelp = fHelp;
				}
				bHasCapturer = bHasCapturer || candidate.bCapturer;
				break;
			}
	out.bMissingRole = !bHasCapturer;
	out.fShareWithHelp = StrengthShare(out.fLocal + out.fHelp, out.fEnemy, fSteepness);
	out.fSwing = StrengthSwing(out.fShare, out.fShareWithHelp);
}

// Vox Deorum: admission is resolved once, before spotting, air, search or marching can spend a recruit.
bool CvTacticalOperationalLoop::AdmitParticipants(CvTacticalAI& kTactical, const SOperationalTurn& turn,
	const SOperationalBattlefield& field, vector<CvUnit*>& vUnits, SOperationalCommandRecord& record)
{
	if (record.iBattlefield < 0)
	{
		record.iBattlefield = field.iID;
		record.iPlotIndex = field.iPlotIndex;
	}
	SOperationalAdmissionDecision& decision = record.admission;
	SOperationalAdmissionRequest& request = decision.request;
	turn.iStateRevision++;
	request.identity = DecisionIdentity(turn, &record);
	request.reinforce = field.reinforce;
	for (size_t i = 0; i < vUnits.size(); i++)
		request.vNative.push_back(vUnits[i]->GetID());
	// Vox Deorum: retain computed native admission facts without repeating strength, reach or search setup.
	decision.vEffective = request.vNative;
	const bool bReinforce = record.eKind == OPERATIONAL_KIND_REINFORCE;
	if (bReinforce)
		decision.effectiveReinforce = field.reinforce;
	// Vox Deorum: query capability once for this command and keep it through admission dispatch.
	const bool bHasAdmission = HasAdmission();
	if (!bHasAdmission)
	{
		decision.eResult = vUnits.empty() ? OPERATIONAL_ADMISSION_NO_ELIGIBLE : OPERATIONAL_ADMISSION_ABSENT;
		return !vUnits.empty();
	}
	CvPlot* pTarget = GC.getMap().plotByIndex(field.iPlotIndex);
	// Vox Deorum: supplied adapters rebuild eligibility without the hidden-state native filters.
	const bool bPermittedMenu = UsesPermittedMenu();
	if (bPermittedMenu)
		PrepareAdmission(request);
	else if (bReinforce)
	{
		const CvCity* pCity = pTarget->getPlotCity();
		GatherReinforceCandidates(kTactical, field, *pTarget, pCity != NULL || !pTarget->isWater(),
			pCity ? pCity->isCoastal() : pTarget->isWater(), request.candidates, field.bReleaseLocal);
	}
	else
		for (size_t i = 0; i < vUnits.size(); i++)
		{
			SOperationalCandidate candidate;
			candidate.iUnitID = vUnits[i]->GetID();
			candidate.fStrength = UnitStrength(*vUnits[i]);
			candidate.bCapturer = IsCapturer(*vUnits[i]);
			candidate.bHeldGarrison = IsHeldGarrison(vUnits[i]);
			request.candidates.push_back(candidate);
		}
	if (!bPermittedMenu)
		PrepareAdmission(request);
	GatherSearchPlotZones(vUnits, pTarget, record.eKind != OPERATIONAL_KIND_OWN_CITY_FIGHT, request.preliminaryZones);
	bool bCallback;
	{
		SOperationalTimer timer(turn.bTiming ? &turn.performance.admissionNs : NULL);
		bCallback = AdmitCommand(request, decision.requested);
	}
	decision.eResult = bCallback ? OPERATIONAL_ADMISSION_NATIVE : OPERATIONAL_ADMISSION_ABSENT;
	if (bCallback && !decision.requested.bUseNative)
	{
		set<int> unique;
		bool bValid = true;
		for (size_t i = 0; i < decision.requested.vUnits.size(); i++)
		{
			const int iUnitID = decision.requested.vUnits[i];
			bool bFound = false;
			for (size_t j = 0; j < request.candidates.size(); j++)
				bFound = bFound || request.candidates[j].iUnitID == iUnitID;
			if (!bFound || !unique.insert(iUnitID).second)
				bValid = false;
		}
		if (bValid)
		{
			decision.eResult = decision.requested.vUnits.empty() ? OPERATIONAL_ADMISSION_REFUSED : OPERATIONAL_ADMISSION_SUBSET;
			decision.vEffective = decision.requested.vUnits;
		}
		else
			decision.eResult = OPERATIONAL_ADMISSION_INVALID;
	}
	if (request.candidates.empty() && decision.eResult != OPERATIONAL_ADMISSION_INVALID)
		decision.eResult = OPERATIONAL_ADMISSION_NO_ELIGIBLE;
	if (bReinforce && decision.eResult == OPERATIONAL_ADMISSION_SUBSET)
	{
		SPathFinderUserData data(NO_PLAYER, PT_ARMY_MIXED, NO_PLAYER, kTactical.GetRecruitRange());
		const ReachablePlots relevantPlots = GC.GetStepFinder().GetPlotsInReach(pTarget, data);
		map<int, ReachablePlots> searchPlots;
		vector<int> reached;
		for (size_t i = 0; i < decision.vEffective.size(); i++)
		{
			const int iUnitID = decision.vEffective[i];
			CvUnit* pUnit = kTactical.m_pPlayer->getUnit(iUnitID);
			if (std::find(request.vNative.begin(), request.vNative.end(), iUnitID) != request.vNative.end() ||
				ReinforceReach(kTactical, pUnit, pTarget, relevantPlots, searchPlots))
				reached.push_back(iUnitID);
			else
				decision.vFailedReach.push_back(iUnitID);
		}
		decision.vEffective.swap(reached);
	}
	// Vox Deorum: added fight candidates must belong to native's validated reach set.
	if (!bReinforce && decision.eResult == OPERATIONAL_ADMISSION_SUBSET)
	{
		vector<int> reached;
		for (size_t i = 0; i < decision.vEffective.size(); ++i)
			if (std::find(request.vNative.begin(), request.vNative.end(), decision.vEffective[i]) != request.vNative.end())
				reached.push_back(decision.vEffective[i]);
			else
				decision.vFailedReach.push_back(decision.vEffective[i]);
		decision.vEffective.swap(reached);
	}
	if (bReinforce)
	{
		if (decision.eResult == OPERATIONAL_ADMISSION_SUBSET || decision.eResult == OPERATIONAL_ADMISSION_REFUSED)
		{
			SOperationalLocalSet local;
			GatherTargetLocalSet(kTactical, turn.ePlayer, *pTarget, local, false);
			const bool bLocalCapturer = field.bReleaseLocal && !bPermittedMenu
				? TallyLocalOutsideCandidates(local, request.candidates, NULL) : local.bCapturer;
			AdmittedReinforcement(request.candidates, field.eCenter == OPERATIONAL_CENTER_ENEMY_CITY && !bLocalCapturer,
				decision.vEffective, turn.weights.afConstant[OPERATIONAL_CONSTANT_THEATER_STEEPNESS], decision.effectiveReinforce);
		}
	}
	vUnits.clear();
	for (size_t i = 0; i < decision.vEffective.size(); i++)
	{
		CvUnit* pUnit = kTactical.m_pPlayer->getUnit(decision.vEffective[i]);
		if (pUnit && pUnit->canUseNow())
			vUnits.push_back(pUnit);
	}
	return !vUnits.empty();
}

// Vox Deorum: prevents native auxiliary actions from recruiting outside a provider's admitted group.
void CvTacticalOperationalLoop::FilterRecruits(CvTacticalAI& kTactical, const vector<CvUnit*>& vUnits)
{
	set<int> admitted;
	for (size_t i = 0; i < vUnits.size(); i++)
		admitted.insert(vUnits[i]->GetID());
	for (vector<CvTacticalUnit>::iterator it = kTactical.m_CurrentMoveUnits.begin(); it != kTactical.m_CurrentMoveUnits.end(); )
		if (admitted.count(it->GetID()) == 0)
			it = kTactical.m_CurrentMoveUnits.erase(it);
		else
			++it;
	for (vector<CvTacticalUnit>::iterator it = kTactical.m_CurrentAirSweepUnits.begin(); it != kTactical.m_CurrentAirSweepUnits.end(); )
		if (admitted.count(it->GetID()) == 0)
			it = kTactical.m_CurrentAirSweepUnits.erase(it);
		else
			++it;
}

// Vox Deorum: DLL decisions already carry the shared loop's synchronous revision.
void CvTacticalOperationalLoop::StampDecision(SOperationalDecisionIdentity& /*identity*/) {}

// Vox Deorum: the DLL has no supplied operational provider.
bool CvTacticalOperationalLoop::HasOperationalProvider() { return false; }

// Vox Deorum: the DLL always uses its native queue.
bool CvTacticalOperationalLoop::UsesPermittedMenu() { return false; }

// Vox Deorum: confirmed execution feedback is a simulator-only provider seam.
void CvTacticalOperationalLoop::ReportExecution(int /*iOffer*/, int /*iResult*/, const SOperationalCommandRecord* /*pCommand*/) {}

// Vox Deorum: the DLL retains its native admission candidates.
void CvTacticalOperationalLoop::PrepareAdmission(SOperationalAdmissionRequest& /*request*/) {}

// Vox Deorum: the DLL keeps native admission without building an unused observation.
bool CvTacticalOperationalLoop::HasAdmission() { return false; }

// Vox Deorum: the DLL always uses the shared native command decoder.
int CvTacticalOperationalLoop::ChooseCommand(const SOperationalChoiceRequest& request)
{
	return request.iNative;
}

// Vox Deorum: the DLL has no optional admission override.
bool CvTacticalOperationalLoop::AdmitCommand(const SOperationalAdmissionRequest& /*request*/, SOperationalAdmissionReply& /*reply*/)
{
	return false;
}

// Vox Deorum: the DLL always keeps the geographic blend and command-kind row.
STacticalFlavors CvTacticalOperationalLoop::ChooseSearchFlavors(const SOperationalFlavorRequest& request)
{
	return request.native;
}

//------------------------------------------------------------------------------
// engagements
//------------------------------------------------------------------------------

// Vox Deorum: retains confirmed engagement counters on every exit from command search.
struct SOperationalEngagementScope
{
	const SOperationalTurn* pTurn;
	const SOperationalEngagementReport& report;
	// Vox Deorum: borrows the command's accumulated search report.
	SOperationalEngagementScope(const SOperationalTurn* turn, const SOperationalEngagementReport& value) : pTurn(turn), report(value) {}
	// Vox Deorum: counts provider/search attempts even if execution stopped the host.
	~SOperationalEngagementScope()
	{
		if (!pTurn) return;
		if (report.iAttempts > 0) pTurn->performance.engagements++;
		if (pTurn->bTiming) pTurn->performance.searchNs += report.searchNs;
		pTurn->performance.attempts += report.iAttempts;
	}
};

// The geometry comes first so the flavors can follow the zones the search covers.
bool CvTacticalOperationalLoop::ExecuteEngagement(PlayerTypes ePlayer, vector<CvUnit*>& vUnits, CvPlot* pTarget, eOperationalKind eKind,
	PlayerTypes eCounterpart, const int (&aiMapped)[STacticalFlavors::NUM_SLOTS], const OperationalSettings& settings,
	bool bTargetDistanceRelevant, SOperationalEngagementReport& report, const SOperationalTurn* pTurn,
	SOperationalCommandRecord* pCommand)
{
	if (vUnits.empty() || !pTarget)
		return false;
	report.bTiming = pTurn ? pTurn->bTiming : (GC.getLogging() && GC.getAILogging());
	GatherSearchPlotZones(vUnits, pTarget, bTargetDistanceRelevant, report.zonePlots);
	report.flavors = BlendEngagementFlavors(ePlayer, report.zonePlots, eKind, eCounterpart, VoxRlCurrentOperationId(ePlayer),
		aiMapped, settings, report.iRepresentativeZone, report.iAggressionRisk);
	report.eAggLvl = AggressionFromRisk(report.iAggressionRisk, settings);
	SOperationalFlavorRequest& request = report.flavorRequest;
	if (pTurn)
	{
		pTurn->iStateRevision++;
		request.identity = DecisionIdentity(*pTurn, pCommand);
	}
	else
	{
		request.identity.ePlayer = ePlayer;
		StampDecision(request.identity);
	}
	request.identity.eKind = eKind;
	request.identity.iTargetPlot = pTarget->GetPlotIndex();
	for (size_t i = 0; i < vUnits.size(); i++)
		request.vUnits.push_back(vUnits[i]->GetID());
	request.finalZones = report.zonePlots;
	request.iRepresentativeZone = report.iRepresentativeZone;
	request.native = report.flavors;
	request.iAggressionRisk = report.iAggressionRisk;
	{
		SOperationalTimer timer(pTurn && pTurn->bTiming ? &pTurn->performance.flavorsNs : NULL);
		report.requestedFlavors = ChooseSearchFlavors(request);
	}
	for (int iSlot = 0; iSlot < STacticalFlavors::NUM_SLOTS; iSlot++)
		if (report.requestedFlavors.iFlavor[iSlot] < 0 || report.requestedFlavors.iFlavor[iSlot] > 100)
			report.bInvalidFlavors = true;
	if (!report.bInvalidFlavors)
		for (int iSlot = 0; iSlot < STacticalFlavors::NUM_SLOTS; iSlot++)
			report.flavors.iFlavor[iSlot] = report.requestedFlavors.iFlavor[iSlot];
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
	SOperationalEngagementScope closure(pTurn, report);
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

		report.iAttempts++;
		vector<STacticalAssignment> vAssignments;
		{
			SOperationalTimer searchTimer(report.bTiming ? &report.searchNs : NULL);
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
	SOperationalEngagementReport& report, const SOperationalTurn* pTurn, SOperationalCommandRecord* pCommand)
{
	//try to improve visibility. however, if the target is too far away this may fail ... in that case we chance it
	vector<CvUnit*> vSpotters;
	for (vector<CvUnit*>::const_iterator it = vUnits.begin(); it != vUnits.end(); ++it)
		if (!IsHeldGarrison(*it))
			vSpotters.push_back(*it);
	{
		SOperationalTimer timer(pTurn && pTurn->bTiming ? &pTurn->performance.spottingNs : NULL);
		kTactical.ExecuteSpotterMove(vSpotters, pTarget);
	}

	//first round: in case there are enemies around, do a combat simulation
	vector<CvUnit*> vSimUnits; //make a copy we can modify!
	for (vector<CvUnit*>::const_iterator it = vUnits.begin(); it != vUnits.end(); ++it)
		if ((*it)->canUseNow())
			vSimUnits.push_back(*it);
	bool bTactSimSuccess = vSimUnits.empty() ? false : ExecuteEngagement(kTactical.m_pPlayer->GetID(), vSimUnits, pTarget, eKind, eCounterpart,
		aiMapped, settings, true, report, pTurn, pCommand);

	SOperationalTimer marchingTimer(pTurn && pTurn->bTiming ? &pTurn->performance.marchingNs : NULL);
	//sometimes tactsim cannot use all units, eg if they are too far out
	vector<CvUnit*> farout;
	//the search clears its distance map when it finishes, so rebuild it for the distance check below
	if (bTactSimSuccess)
		TacticalAIHelpers::UpdatePlotDistanceToTarget(kTactical.m_pPlayer->GetID(), pTarget);
	for (vector<CvUnit*>::const_iterator it = vUnits.begin(); it != vUnits.end(); ++it)
	{
		CvUnit* pUnit = *it;
		if (pUnit->TurnProcessed() || IsHeldGarrison(pUnit))
			continue;

		if (bTactSimSuccess && TacticalAIHelpers::GetPlotDistanceToTarget(pUnit->plot()->GetPlotIndex(), pUnit->getDomainType()) <= TACTICAL_COMBAT_MAX_TARGET_DISTANCE)
			continue; //do not end the turn ... we may want to shuffle them around later

		farout.push_back(pUnit);
	}
	gDistanceToTargetPlots.clear();

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

// Vox Deorum: copy of CvDangerPlotContents::GetDanger(const CvUnit*, ...), listing each enemy contribution at its roll.
// City danger uses our uncached copy, and a garrison is lost at the city-fall threshold without native's 50-HP margin.
int CvTacticalOperationalLoop::GetDangerByThreat(CvDangerPlotContents& kContents, const CvUnit* pUnit, const SUnitIDValueContainer& extraUnitDamage,
	int iExtraDamage, AirActionType eAirAction, vector<SOperationalThreatDamage>& threats, int iRisk)
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
				OperationalDamageRollScope roll(EnemyRollPer200(iRisk));
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

			OperationalDamageRollScope roll(EnemyRollPer200(iRisk));
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
		int iCityDanger = GetCityDangerByThreat(kContents, pFriendlyCity, pUnit, extraUnitDamage, 0, NULL, iRisk);
		// A falling city destroys its garrison at the same threshold used by f_def.
		if (iCityDanger + pFriendlyCity->getDamage() < pFriendlyCity->GetMaxHitPoints())
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

			// Vox Deorum: project the enemy hit at its roll, then our retaliation at our roll for the suicide check.
			int iDamage = 0;
			{
				OperationalDamageRollScope roll(EnemyRollPer200(iRisk));
				iDamage = TacticalAIHelpers::GetSimulatedDamageFromAttackOnUnit(pUnit, pAttacker, pPlot, bOutOfRange ? NULL : pAttacker->plot(), iAttackerDamage, false, iExtraAttackerDamage, iExtraDamage, true);
			}
			// Native non-air ranged attacks have no retaliation.
			if (!pAttacker->IsCanAttackRanged() || pAttacker->getDomainType() == DOMAIN_AIR)
			{
				OperationalDamageRollScope roll(OurRollPer200(iRisk));
				TacticalAIHelpers::GetSimulatedDamageFromAttackOnUnit(pUnit, pAttacker, pPlot, bOutOfRange ? NULL : pAttacker->plot(), iAttackerDamage, false, iExtraAttackerDamage, iExtraDamage, true);
			}

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

		OperationalDamageRollScope roll(EnemyRollPer200(iRisk));
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

// Vox Deorum: copy of CvDangerPlotContents::GetDanger(const CvCity*, ...), preserving native hit order and absorption.
// Optionally lists attacker contributions and scopes enemy rolls around damage only, after cache-filling lookups.
int CvTacticalOperationalLoop::GetCityDangerByThreat(CvDangerPlotContents& kContents, const CvCity* pCity, const CvUnit* pPretendGarrison,
	const SUnitIDValueContainer& extraUnitDamage, int iExtraSelfDamage, vector<SOperationalThreatDamage>* pThreats, int iRisk)
{
	if (pThreats)
		pThreats->clear();
	if (!kContents.m_pPlot || !pCity)
		return 0;

	int iPlotDamage = 0;
	CvPlot* pCityPlot = pCity->plot();

	CvCityGarrisonOverride guard(pCity,pPretendGarrison);

	//if we have a garrison, split the damage
	int iGarrisonMaxHP = pCity->GetGarrisonedUnit() ? pCity->GetGarrisonedUnit()->GetMaxHitPoints() : 0;
	int iGarrisonHPLeft = pCity->GetGarrisonedUnit() ? pCity->GetGarrisonedUnit()->GetCurrHitPoints() : 0;
	int iGarrisonDamage = 0; // called by reference

	for (DangerUnitVector::iterator it = kContents.m_apUnits.begin(); it < kContents.m_apUnits.end(); ++it)
	{
		CvUnit* pUnit = GET_PLAYER(it->first).getUnit(it->second);
		if (!pUnit || pUnit->isDelayedDeath() || pUnit->IsDead())
		{
			continue;
		}

		//there should be only very few of these, if any
		int iExtraAttackerDamage = extraUnitDamage.GetValue(it->second);
		if (iExtraAttackerDamage >= pUnit->GetCurrHitPoints())
			continue;

		CvPlot* pAttackerPlot = NULL;
		if (pUnit->IsCanAttackRanged())
		{
			if (pUnit->getDomainType() == DOMAIN_AIR)
			{
				// Interceptor selection can fill native caches, so it precedes the damage-roll scope.
				CvUnit* pInterceptor = kContents.m_pPlot->GetBestInterceptor(pUnit->getOwner(), pUnit);
				int iInterceptDamage = 0;
				if (pInterceptor)
				{
					// Vox Deorum: always assume interception succeeds, using our roll for our interceptor's hit.
					OperationalDamageRollScope roll(OurRollPer200(iRisk));
					iInterceptDamage = pInterceptor->GetInterceptionDamage(pUnit, false, kContents.m_pPlot);
				}
				OperationalDamageRollScope roll(EnemyRollPer200(iRisk));
				int iDamage = pUnit->GetAirCombatDamage(NULL, pCity, (iGarrisonHPLeft <= 0 ? 0 : iGarrisonMaxHP), iGarrisonDamage, false, iInterceptDamage + iExtraAttackerDamage, iExtraSelfDamage, kContents.m_pPlot);
				iPlotDamage += iDamage;
				if (pThreats)
				{
					SOperationalThreatDamage threat = { it->first, it->second, false, iDamage };
					pThreats->push_back(threat);
				}
				iExtraSelfDamage += iDamage;
				iGarrisonHPLeft -= iGarrisonDamage;
			}
			else
			{
				OperationalDamageRollScope roll(EnemyRollPer200(iRisk));
				int iDamage = pUnit->GetRangeCombatDamage(NULL, pCity, (iGarrisonHPLeft <= 0 ? 0 : iGarrisonMaxHP), iGarrisonDamage, false, iExtraAttackerDamage, iExtraSelfDamage, kContents.m_pPlot);
				iPlotDamage += iDamage;
				if (pThreats)
				{
					SOperationalThreatDamage threat = { it->first, it->second, false, iDamage };
					pThreats->push_back(threat);
				}
				iExtraSelfDamage += iDamage;
				iGarrisonHPLeft -= iGarrisonDamage;
			}
		}
		else
		{
			if (pCityPlot->isAdjacent(pUnit->plot()))
			{
				pAttackerPlot = pUnit->plot();
			}
			OperationalDamageRollScope roll(EnemyRollPer200(iRisk));
			int iSelfDamageAttacker = 0;
			// check if the garrison is dead
			int iDamage = pUnit->getMeleeCombatDamageCity(pUnit->GetMaxAttackStrength(pAttackerPlot, pCityPlot, NULL, !pUnit->isNoCapture(), !pUnit->isNoCapture()),
				pCity, iSelfDamageAttacker, (iGarrisonHPLeft <= 0 ? 0 : iGarrisonMaxHP), iGarrisonDamage, false, iExtraAttackerDamage, iExtraSelfDamage);
			iPlotDamage += iDamage;
			if (pThreats)
			{
				SOperationalThreatDamage threat = { it->first, it->second, false, iDamage };
				pThreats->push_back(threat);
			}
			iExtraSelfDamage += iDamage;
			iGarrisonHPLeft -= iGarrisonDamage;
		}
	}

	return iPlotDamage;
}

// Vox Deorum: retains the standard city's total and threat list for the lifetime of this estimate pass.
const SOperationalCityDanger& CvTacticalOperationalLoop::CachedCityDanger(const SOperationalTurn& turn, SOperationalEstimateCache& cache, const CvCity& kCity)
{
	const int iPlotIndex = kCity.plot()->GetPlotIndex();
	map<int, SOperationalCityDanger>::iterator it = cache.cityDanger.find(iPlotIndex);
	if (it == cache.cityDanger.end())
	{
		SOperationalTimer dangerTimer(turn.bTiming && turn.bEstimating ? &turn.performance.dangerReadNs : NULL);
		if (turn.bEstimating) turn.performance.dangerReads++;
		CvDangerPlotContents& contents = DangerContents(turn.ePlayer, *kCity.plot());
		SOperationalCityDanger danger;
		danger.iTotal = GetCityDangerByThreat(contents, &kCity, NULL, SUnitIDValueContainer(), 0, &danger.threats,
			turn.aiMapped[STacticalFlavors::RISK]);
		it = cache.cityDanger.insert(make_pair(iPlotIndex, danger)).first;
	}
	return it->second;
}

// Vox Deorum: reads the monotonic performance counter used by cvStopWatch, avoiding absolute-tick overflow.
unsigned long long CvTacticalOperationalLoop::ClockNs()
{
	static LARGE_INTEGER frequency = {0};
	if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
	LARGE_INTEGER ticks;
	QueryPerformanceCounter(&ticks);
	return (ticks.QuadPart / frequency.QuadPart) * 1000000000ULL +
		(ticks.QuadPart % frequency.QuadPart) * 1000000000ULL / frequency.QuadPart;
}

// Vox Deorum: reports the game's retained terminal state.
bool CvTacticalOperationalLoop::IsHostStopped()
{
	return GC.getGame().getGameState() == GAMESTATE_OVER;
}

// Vox Deorum: closed offers remain consumed for the rest of this pass.
bool OperationalOfferIsClosed(eOperationalOfferState state)
{
	return state >= OPERATIONAL_OFFER_SKIPPED;
}

// Vox Deorum: names closure using retained state and immutable offer metadata.
const char* OperationalOfferClosedReason(eOperationalOfferState state, eOperationalKind kind)
{
	if (state == OPERATIONAL_OFFER_SKIPPED) return "failureLimit";
	if (state == OPERATIONAL_OFFER_RETIRED) return "retired";
	if (state == OPERATIONAL_OFFER_EXECUTED) return "executed";
	if (state == OPERATIONAL_OFFER_REFUSED) return "providerRefused";
	if (state == OPERATIONAL_OFFER_DROPPED) return kind == OPERATIONAL_KIND_UNIT_FIGHT ? "targetGone" : "cityLost";
	return "";
}

// Vox Deorum: city progress at our city uses hold-city; other progress uses occupation.
int OperationalStakeWeight(int iTerm, eOperationalKind kind)
{
	const int weights[NUM_OPERATIONAL_STAKE_TERMS] = { OPERATIONAL_WEIGHT_ATTRITION, OPERATIONAL_WEIGHT_RISK,
		OPERATIONAL_WEIGHT_HOLD_GROUND, OPERATIONAL_WEIGHT_HOLD_CITY,
		kind == OPERATIONAL_KIND_OWN_CITY_FIGHT ? OPERATIONAL_WEIGHT_HOLD_CITY : OPERATIONAL_WEIGHT_OCCUPATION,
		OPERATIONAL_WEIGHT_ATTRITION };
	return weights[iTerm];
}

// Vox Deorum: compares every serialized scalar before emitting a replacement snapshot row.
bool OperationalSnapshotEqual(const SOperationalSnapshot& a, const SOperationalSnapshot& b)
{
	if (a.iPlotIndex != b.iPlotIndex || a.iRank != b.iRank || a.eState != b.eState || a.fEstimate != b.fEstimate ||
		a.fTheater != b.fTheater || a.fSwing != b.fSwing || a.fSelectedSwing != b.fSelectedSwing ||
		a.theater.fImportance != b.theater.fImportance || a.theater.fPressure != b.theater.fPressure ||
		a.theater.fContest != b.theater.fContest || a.theater.fDamageLevel != b.theater.fDamageLevel) return false;
	for (int i = 0; i < NUM_OPERATIONAL_STAKE_TERMS; ++i)
		if (a.stake.afValue[i] != b.stake.afValue[i]) return false;
	return a.stake.fDestruction == b.stake.fDestruction && a.stake.fExposure == b.stake.fExposure &&
		a.stake.fGround == b.stake.fGround && a.stake.fRelief == b.stake.fRelief &&
		a.stake.fCityProgress == b.stake.fCityProgress && a.stake.fGarrison == b.stake.fGarrison;
}

// Vox Deorum: writes only scalar snapshots from the live queue, with stable ranks and explicit closure reasons.
void CvTacticalOperationalLoop::RecordStep(const SOperationalTurn& turn, SOperationalTurnRecord& record)
{
	SOperationalTimer timer(turn.bTiming ? &turn.performance.recordNs : NULL);
	SOperationalStep step;
	step.iStep = static_cast<int>(record.steps.size());
	step.estimateNs = turn.performance.estimateNs - record.recordedEstimateNs;
	record.recordedEstimateNs = turn.performance.estimateNs;
	vector<int> ready, ranks(turn.battlefields.size() * 2, -1);
	for (size_t i = 0; i < turn.battlefields.size(); ++i)
		for (int k = 0; k < 2; ++k)
			if ((k ? turn.battlefields[i].eReinforce : turn.battlefields[i].eFight) == OPERATIONAL_OFFER_READY)
				ready.push_back(static_cast<int>(i) * 2 + k);
	std::sort(ready.begin(), ready.end(), PrOperationalOfferBefore(turn.battlefields));
	for (size_t i = 0; i < ready.size(); ++i) ranks[ready[i]] = static_cast<int>(i);
	for (size_t i = 0; i < turn.battlefields.size(); i++)
	{
		const SOperationalBattlefield& field = turn.battlefields[i];
		for (int k = 0; k < 2; k++)
		{
			if (record.offers.size() <= i * 2 + k)
			{
				SOperationalOfferRecord offer = { field.iID, field.iTargetID, field.iPlotIndex, field.iZoneID,
					k ? OPERATIONAL_KIND_REINFORCE : field.eCenter == OPERATIONAL_CENTER_OUR_CITY ? OPERATIONAL_KIND_OWN_CITY_FIGHT :
					field.eCenter == OPERATIONAL_CENTER_ENEMY_CITY ? OPERATIONAL_KIND_CITY_FIGHT : OPERATIONAL_KIND_UNIT_FIGHT, field.eOwner };
				record.offers.push_back(offer);
			}
			SOperationalSnapshot row;
			row.iBattlefield = field.iID;
			row.iPlotIndex = field.iPlotIndex;
			row.bReinforce = k != 0;
			row.eState = k ? field.eReinforce : field.eFight;
			row.fEstimate = k ? field.fReinforceEstimate : field.fEstimate;
			row.fTheater = field.fTheater;
			row.theater = field.theater;
			// Vox Deorum: a gated offer contributes no swing; keep the native selection swing alongside it.
			row.fSwing = k ? (field.fReinforceEstimate > 0.0 ? field.reinforce.fSwing : 0.0) : 1.0;
			row.fSelectedSwing = k ? field.reinforce.fSwing : 1.0;
			row.stake = field.stake;

			if (OperationalOfferIsClosed(row.eState) && i * 2 + k < record.latestSnapshot.size())
			{
				const eOperationalOfferState state = row.eState;
				row = record.latestSnapshot[i * 2 + k];
				row.eState = state;
				// Vox Deorum: a checked Reinforce can execute at a repaired estimate within this step.
				if (state == OPERATIONAL_OFFER_EXECUTED || state == OPERATIONAL_OFFER_REFUSED)
					for (size_t c = 0; c < record.commands.size(); c++)
						if (record.commands[c].iBattlefield == field.iID &&
							(record.commands[c].eKind == OPERATIONAL_KIND_REINFORCE) == (k != 0))
						{
							row.fEstimate = record.commands[c].fEstimate;
							row.fTheater = record.commands[c].fTheater;
							row.fSwing = row.fSelectedSwing = record.commands[c].fSwing;
							row.iPlotIndex = record.commands[c].iPlotIndex;
							row.stake = record.commands[c].stake;
						}
			}
			row.szReason = OperationalOfferClosedReason(row.eState, record.offers[i * 2].eKind);
			row.iRank = ranks[i * 2 + k];
			if (i * 2 + k >= record.latestSnapshot.size())
			{
				step.snapshot.push_back(row);
				record.latestSnapshot.push_back(row);
			}
			else if (!OperationalSnapshotEqual(row, record.latestSnapshot[i * 2 + k]))
			{
				step.snapshot.push_back(row);
				record.latestSnapshot[i * 2 + k] = row;
			}
		}
	}
	record.steps.push_back(step);
}

// Vox Deorum: closes the last trace step before scratch destruction, after nested stage timers have unwound.
void CvTacticalOperationalLoop::CloseRecord(const SOperationalTurn& turn, SOperationalTurnRecord* pRecord)
{
	if (pRecord && !pRecord->steps.empty())
	{
		SOperationalTimer timer(turn.bTiming ? &turn.performance.recordNs : NULL);
		SOperationalStep& step = pRecord->steps.back();
		if (IsHostStopped() || !*step.szEnd) step.szEnd = "stopped";
		unsigned long long estimated = 0, checked = 0, executed = 0;
		for (size_t i = 0; i + 1 < pRecord->steps.size(); i++)
		{
			estimated += pRecord->steps[i].estimateNs;
			checked += pRecord->steps[i].readinessNs;
			executed += pRecord->steps[i].executeNs;
		}
		// Vox Deorum: retain partial stages if an estimate or readiness check unwound before its normal record boundary.
		step.estimateNs = turn.performance.estimateNs - estimated;
		step.readinessNs = turn.performance.readinessNs - checked;
		step.executeNs = turn.performance.executeNs - executed;
	}
}
