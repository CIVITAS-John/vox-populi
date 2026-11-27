/*	-------------------------------------------------------------------------------------------------------
	The operational loop (VOX_TACTICAL_OPERATIONAL_LOOP). See CvTacticalOperationalLoop.h. Each copy of
	a native function names its original in its comment, so a Vox Populi update can be diffed against it.
	------------------------------------------------------------------------------------------------------- */
#include "CvGameCoreDLLPCH.h"
// Arrival readiness distinguishes a missing route from any finite travel time.
#include <limits>
#include "CvTacticalAI.h"
#include "CvTacticalSearchShared.h"
#include "CvTacticalAirSearch.h"
#include "CvTacticalAnalysisMap.h"
#include "CvTacticalOperationalLoop.h"
#include "CvTacticalOperationalValue.h"
// Declares the scoped expected damage roll.
#include "CvUnitCombat.h"
#include "CvDangerPlots.h"
#include "CvMilitaryAI.h"
#include "CvGrandStrategyAI.h"
#include "CvGameCoreUtils.h"
#include "CvTypes.h"
#include "VoxDeorumRL/VoxRlCapture.h"
#include <math.h>
// The host clock uses the same performance counter as cvStopWatch.
#include "cvStopWatch.h"
#include "LintFree.h"

namespace
{
	// Temporarily changes projected damage without changing random combat or native cached estimates.
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

// Per-kind base rows, added after the blend under the situation offsets. Fights and Reinforce leave everything to their
// situation. Withdraw lowers the objectives so moving away beats fighting; Army March stays cautious and is not drawn
// into cities on the way. Both run on the base row alone.
const signed char OPERATIONAL_KIND_FLAVOR_ROWS[][STacticalFlavors::NUM_SLOTS] =
{
	//  RISK  OCCUPATION  ATTRITION  HOLD_CITY  HOLD_GROUND
	{    0,     0,          0,         0,         0 },  // Unit fight
	{    0,     0,          0,         0,         0 },  // City fight
	{    0,     0,          0,         0,         0 },  // Reinforce
	{    0,     0,          0,         0,         0 },  // Own-city fight
	{  -20,   -30,        -10,       -20,       -10 },  // Withdraw
	{  -30,   -20,          0,         0,         0 },  // ArmyMarch
};
typedef char OperationalKindRowsMatchKinds[(sizeof(OPERATIONAL_KIND_FLAVOR_ROWS) / sizeof(OPERATIONAL_KIND_FLAVOR_ROWS[0]) == NUM_OPERATIONAL_KINDS) ? 1 : -1];

const char* const OPERATIONAL_POSTURE_NAMES[] =
{
	"NONE", "WITHDRAW", "ATTRITION", "EXPLOIT_FLANKS", "STEAMROLL", "SURGICAL_CITY_STRIKE", "HEDGEHOG", "COUNTERATTACK",
};
typedef char OperationalPostureNamesMatchPostures[(sizeof(OPERATIONAL_POSTURE_NAMES) / sizeof(OPERATIONAL_POSTURE_NAMES[0]) == NUM_OPERATIONAL_POSTURES) ? 1 : -1];

const char* const OPERATIONAL_KIND_NAMES[] =
{
	"UNIT_FIGHT", "CITY_FIGHT", "REINFORCE", "OWN_CITY_FIGHT", "WITHDRAW",
	"ARMY_MARCH",
};
typedef char OperationalKindNamesMatchKinds[(sizeof(OPERATIONAL_KIND_NAMES) / sizeof(OPERATIONAL_KIND_NAMES[0]) == NUM_OPERATIONAL_KINDS) ? 1 : -1];

// Default search purposes in command kind order. Unit battles refine BattleNeutral by target territory.
// Operational reinforcement shares native's Reinforce purpose; Withdraw has its own purpose and retreat scoring.
static const SearchIntent OPERATIONAL_KIND_SEARCH_INTENTS[] =
{
	kSearchIntentBattleNeutral, kSearchIntentCityAttack, kSearchIntentReinforce,
	kSearchIntentCityDefense, kSearchIntentWithdraw,
	kSearchIntentGather, // ArmyMarch keeps the native formation search purpose.
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

// The most distinct enemy targets sampled in one command loop.
static const size_t OPERATIONAL_FORECAST_BUDGET = 24;
// Each successive victim contributes this fraction of the previous exposure weight.
static const double OPERATIONAL_DEFENSE_EXPOSURE_DECAY = 0.8;
// Native caps melee hits by attackers that cannot move after attacking.
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

// Averages the zone vectors by plot share in doubles, then adds the command offset and rounds once.
void BlendZoneFlavors(const vector<SOperationalZoneFlavors>& zones, const int (&aiMapped)[STacticalFlavors::NUM_SLOTS],
	const double (&afCommandOffset)[STacticalFlavors::NUM_SLOTS], int (&aiFlavors)[STacticalFlavors::NUM_SLOTS], int& iRepresentativeZone)
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
		fValue += afCommandOffset[iSlot];
		aiFlavors[iSlot] = static_cast<int>(floor(std::min(100.0, std::max(0.0, fValue)) + 0.5));
	}
}

// Focus splits the span by each objective's share of the gain and never lowers a flavor. RISK compares the gain with the
// uncovered exposure, from -span to +span. The softener keeps all offsets near 0 when little is at stake.
void SituationFlavorOffsets(SOperationalSituation& situation, double fFocusSpan, double fRiskSpan, double fSoftener)
{
	const double fGain = situation.fCapture + situation.fKill + situation.fSiege;
	const double fFocusRoom = fGain + fSoftener;
	const double fRiskRoom = fGain + situation.fUncovered + fSoftener;
	const double fFocus = fFocusRoom > 0.0 ? fFocusSpan / fFocusRoom : 0.0;
	const double fRisk = fRiskRoom > 0.0 ? fRiskSpan * (fGain - situation.fUncovered) / fRiskRoom : 0.0;
	situation.afOffset[STacticalFlavors::RISK] = fRisk;
	situation.afOffset[STacticalFlavors::OCCUPATION] = fFocus * situation.fCapture;
	situation.afOffset[STacticalFlavors::ATTRITION] = fFocus * situation.fKill;
	situation.afOffset[STacticalFlavors::HOLD_CITY] = fFocus * situation.fSiege;
	// Danger beyond the gain holds a line on our land; elsewhere caution or Withdraw answers it.
	situation.afOffset[STacticalFlavors::HOLD_GROUND] = situation.bOurLand ? std::max(0.0, -fRisk) : 0.0;
}

// Blends the search flavors from the mapped values and the aggression risk from neutral 50 through the same zones,
// tactical modifiers, posture rows and command offset.
STacticalFlavors CvTacticalOperationalLoop::BlendEngagementFlavors(PlayerTypes ePlayer, const map<int, int>& zonePlots,
	const double (&afCommandOffset)[STacticalFlavors::NUM_SLOTS],
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
	BlendZoneFlavors(neutralZones, aiNeutral, afCommandOffset, aiAggression, iNeutralRepresentative);
	iAggressionRisk = aiAggression[STacticalFlavors::RISK];
	STacticalFlavors flavors;
	BlendZoneFlavors(zones, aiMapped, afCommandOffset, flavors.iFlavor, iRepresentativeZone);
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

// Uses the shared L and T for the full-strength stop; discounted help sets the score.
void SelectReinforcement(const vector<SOperationalCandidate>& candidates, double fLocal, double fEnemy, double fRatio, bool bNeedCapturer,
	SOperationalReinforce& out)
{
	vector<SOperationalCandidate> sorted = candidates;
	std::sort(sorted.begin(), sorted.end(), PrOperationalCandidateNearest());
	out.fLocal = fLocal;
	out.fEnemy = fEnemy;
	out.fRatio = fRatio;
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

// One offer identity per battlefield and slot.
int OperationalOfferIdentity(int iBattlefield, eOperationalSlot eSlot)
{
	return iBattlefield * NUM_OPERATIONAL_SLOTS + static_cast<int>(eSlot);
}

// The battlefield index of an offer identity.
int OperationalOfferBattlefield(int iOffer)
{
	return iOffer / NUM_OPERATIONAL_SLOTS;
}

// The slot of an offer identity.
eOperationalSlot OperationalOfferSlot(int iOffer)
{
	return static_cast<eOperationalSlot>(iOffer % NUM_OPERATIONAL_SLOTS);
}

// The command kind a slot runs at a center.
// Camps use the same Fight and Reinforce kinds as units.
eOperationalKind OperationalSlotKind(eOperationalCenter eCenter, eOperationalSlot eSlot)
{
	if (eSlot == OPERATIONAL_SLOT_REINFORCE)
		return eCenter == OPERATIONAL_CENTER_ARMY ? OPERATIONAL_KIND_ARMY_MARCH : OPERATIONAL_KIND_REINFORCE;
	if (eSlot == OPERATIONAL_SLOT_WITHDRAW)
		return OPERATIONAL_KIND_WITHDRAW;
	return eCenter == OPERATIONAL_CENTER_OUR_CITY ? OPERATIONAL_KIND_OWN_CITY_FIGHT
		: eCenter == OPERATIONAL_CENTER_ENEMY_CITY ? OPERATIONAL_KIND_CITY_FIGHT : OPERATIONAL_KIND_UNIT_FIGHT;
}

// Keep the stable three-slot address space even where a row cannot run a slot.
bool OperationalSupportsSlot(eOperationalCenter eCenter, eOperationalSlot eSlot)
{
	if (eCenter == OPERATIONAL_CENTER_ARMY)
		return eSlot == OPERATIONAL_SLOT_REINFORCE;
	return eSlot != OPERATIONAL_SLOT_WITHDRAW ||
		(eCenter != OPERATIONAL_CENTER_OUR_CITY && eCenter != OPERATIONAL_CENTER_CAMP);
}

// The slot a command kind belongs to.
eOperationalSlot OperationalKindSlot(eOperationalKind eKind)
{
	if (eKind == OPERATIONAL_KIND_REINFORCE || eKind == OPERATIONAL_KIND_ARMY_MARCH)
		return OPERATIONAL_SLOT_REINFORCE;
	if (eKind == OPERATIONAL_KIND_WITHDRAW)
		return OPERATIONAL_SLOT_WITHDRAW;
	return OPERATIONAL_SLOT_FIGHT;
}

// One battlefield offer's state by slot.
eOperationalOfferState& OperationalOfferState(SOperationalBattlefield& field, eOperationalSlot eSlot)
{
	if (eSlot == OPERATIONAL_SLOT_REINFORCE)
		return field.eReinforce;
	if (eSlot == OPERATIONAL_SLOT_WITHDRAW)
		return field.eWithdraw;
	return field.eFight;
}

// One battlefield offer's state by slot, read-only.
const eOperationalOfferState& OperationalOfferState(const SOperationalBattlefield& field, eOperationalSlot eSlot)
{
	if (eSlot == OPERATIONAL_SLOT_REINFORCE)
		return field.eReinforce;
	if (eSlot == OPERATIONAL_SLOT_WITHDRAW)
		return field.eWithdraw;
	return field.eFight;
}

// One battlefield offer's estimate by slot.
double OperationalOfferEstimate(const SOperationalBattlefield& field, eOperationalSlot eSlot)
{
	if (eSlot == OPERATIONAL_SLOT_REINFORCE)
		return field.fReinforceEstimate;
	if (eSlot == OPERATIONAL_SLOT_WITHDRAW)
		return field.fWithdrawEstimate;
	return field.fEstimate;
}

// Withdraw ranks below Reinforce. Fight ties favor our cities, enemy cities, then units and camps.
int OfferRank(eOperationalCenter eCenter, eOperationalSlot eSlot)
{
	// Existing offers win an exact tie with an army.
	if (eCenter == OPERATIONAL_CENTER_ARMY)
		return 5;
	if (eSlot == OPERATIONAL_SLOT_REINFORCE)
		return 3;
	if (eSlot == OPERATIONAL_SLOT_WITHDRAW)
		return 4;
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

// Orders offers identically in selection and diagnostic ready ranks.
bool OperationalOfferBefore(const SOperationalBattlefield& field, eOperationalSlot eSlot, int iOffer,
	const SOperationalBattlefield& best, eOperationalSlot eBestSlot, int iBest)
{
	const double estimate = OperationalOfferEstimate(field, eSlot);
	const double bestEstimate = OperationalOfferEstimate(best, eBestSlot);
	const int rank = OfferRank(field.eCenter, eSlot), bestRank = OfferRank(best.eCenter, eBestSlot);
	// The waypoint can change, so ties between armies use their stable row order.
	if (estimate == bestEstimate && field.eCenter == OPERATIONAL_CENTER_ARMY && best.eCenter == OPERATIONAL_CENTER_ARMY)
		return iOffer < iBest;
	if (estimate == bestEstimate && rank == bestRank && field.iPlotIndex == best.iPlotIndex)
		return iOffer < iBest;
	return OfferOutranks(estimate, rank, field.iPlotIndex, bestEstimate, bestRank, best.iPlotIndex);
}

// Sorts only ready offer identities without copying battlefields.
struct PrOperationalOfferBefore
{
	const vector<SOperationalBattlefield>& fields;
	// Borrows the current turn's queue for this sort.
	PrOperationalOfferBefore(const vector<SOperationalBattlefield>& value) : fields(value) {}
	// Applies the selection order, including full ties.
	bool operator()(int i, int j) const
	{
		return OperationalOfferBefore(fields[OperationalOfferBattlefield(i)], OperationalOfferSlot(i), i,
			fields[OperationalOfferBattlefield(j)], OperationalOfferSlot(j), j);
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

	// The first zone dispatch flushes high-priority changes and publishes stance labels once.
	if (MOD_IPC_CHANNEL && gVoxRlCaptureEnabled)
	{
		VoxRlCapture::GetInstance().SetMilitaryPhase(pPlayer->GetID(), VOX_RL_MILITARY_PHASE_ZONE_WORK);
		VoxRlCapture::GetInstance().OnFirstZoneDispatch(pPlayer->GetID());
	}

	//every zone's purchases come before the command loop
	for (int iI = 0; iI < kTactical.GetTacticalAnalysisMap()->GetNumZones(); iI++)
		kTactical.PlotEmergencyPurchases(kTactical.GetTacticalAnalysisMap()->GetZoneByIndex(iI));

	RunCommandLoop(kTactical);

	// Label the deferred garrisons as the second zone loop, as native does.
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

// March and live discovery require the operational loop for this player.
bool CvTacticalOperationalLoop::IsMarchEnabled(const CvPlayer& kPlayer, const OperationalSettings* pSettings)
{
	if (!MOD_VOX_TACTICAL_OPERATIONAL_LOOP || kPlayer.isBarbarian())
		return false;
	OperationalSettings settings;
	if (!pSettings)
	{
		GetOperationalSettings(kPlayer.GetID(), &settings);
		pSettings = &settings;
	}
	return pSettings->afConstant[OPERATIONAL_CONSTANT_MARCH_ENABLED] != 0.0;
}

// Identifies the stages that score arrival at muster.
static bool ArmyMarchAtMuster(AIOperationState eStage)
{
	return eStage == AI_OPERATION_STATE_RECRUITING_UNITS || eStage == AI_OPERATION_STATE_GATHERING_FORCES;
}

// Records the native waypoint and corridor and applies the combat move's path and safety aborts.
static bool UpdatePreparedWaypoint(CvAIOperation& kOperation, CvArmyAI& kArmy, SOperationalPreparedArmy& prepared)
{
	const CvPlot* pOldGoal = prepared.pWaypoint ?
		(ArmyMarchAtMuster(prepared.eStage) ? prepared.pMuster : prepared.pGoal) : NULL;
	prepared.eStage = kOperation.GetOperationState();
	prepared.pObjective = kOperation.GetTargetPlot();
	prepared.pMuster = kOperation.GetMusterPlot();
	prepared.pGoal = kArmy.GetGoalPlot();
	if (!prepared.pMuster)
	{
		prepared.pWaypoint = NULL;
		prepared.vMarchCorridor.clear();
		return true;
	}

	vector<CvPlot*> corridor;
	CvPlot* pWaypoint = kOperation.ComputeTargetPlotForThisTurn(&kArmy, &corridor);
	if (!pWaypoint)
	{
		kOperation.SetToAbort(AI_ABORT_LOST_PATH);
		return false;
	}
	if (kArmy.GetArmyAIState() == ARMYAISTATE_MOVING_TO_DESTINATION && kOperation.IsWaypointTooDangerous(pWaypoint))
	{
		kOperation.SetToAbort(AI_ABORT_TOO_DANGEROUS);
		return false;
	}
	const CvPlot* pGoal = ArmyMarchAtMuster(prepared.eStage) ? prepared.pMuster : prepared.pGoal;
	if (pWaypoint != prepared.pWaypoint || pGoal != pOldGoal)
		prepared.vMarchCorridor.swap(corridor);
	prepared.pWaypoint = pWaypoint;
	return true;
}

// Checks whether the operation still has the order that produced the prepared waypoint.
static bool MatchesPreparedArmyOrder(CvAIOperation& kOperation, const CvArmyAI& kArmy, const SOperationalPreparedArmy& order)
{
	return kOperation.GetOperationState() == order.eStage && kOperation.GetTargetPlot() == order.pObjective &&
		kOperation.GetMusterPlot() == order.pMuster && kArmy.GetGoalPlot() == order.pGoal;
}

// Prepares a combat army in the native pass and keeps its movement pending for the loop.
bool CvTacticalOperationalLoop::PrepareArmy(CvTacticalAI& kTactical, CvAIOperation& kOperation, SOperationalPreparedArmy& prepared)
{
	kOperation.LogOperationStatus(true);
	CvArmyAI* pArmy = kOperation.PrepareMove(prepared.start);
	if (!pArmy)
		return false;

	prepared.eOwner = kOperation.GetOwner();
	prepared.iOperation = kOperation.GetID();
	prepared.iArmy = pArmy->GetID();
	prepared.pWaypoint = NULL;
	if (!UpdatePreparedWaypoint(kOperation, *pArmy, prepared))
	{
		pArmy->ReleaseAllUnits();
		return false;
	}

	// Newly recruited reserves can still be on the independent roster.
	for (CvUnit* pUnit = pArmy->GetFirstUnit(); pUnit; pUnit = pArmy->GetNextUnit(pUnit))
		kTactical.m_CurrentTurnUnits.remove(pUnit->GetID());
	return true;
}

// Releases a terminal operation's army, reports the result, and removes the operation and its prepared entry.
vector<SOperationalPreparedArmy>::iterator CvTacticalOperationalLoop::RemovePreparedOperation(CvTacticalAI& kTactical,
	vector<SOperationalPreparedArmy>::iterator order, CvAIOperation& kOperation, CvArmyAI* pArmy)
{
	if (pArmy)
	{
		VoxRlOperationCaptureScope captureScope(MOD_IPC_CHANNEL && gVoxRlCaptureEnabled,
			order->eOwner, order->eOwner, VOX_RL_OPERATION_CHANGE_EXECUTION, order->iOperation);
		pArmy->ReleaseAllUnits();
	}
	kTactical.ReportOperationResult(kOperation, false, false);
	kOperation.Kill();
	return kTactical.m_PreparedArmies.erase(order);
}

// Cleans up missing or already aborted operations and updates waypoints for changed orders.
void CvTacticalOperationalLoop::RefreshPreparedArmies(CvTacticalAI& kTactical)
{
	vector<SOperationalPreparedArmy>& armies = kTactical.m_PreparedArmies;
	for (vector<SOperationalPreparedArmy>::iterator it = armies.begin(); it != armies.end(); )
	{
		CvAIOperation* pOperation = GET_PLAYER(it->eOwner).getAIOperation(it->iOperation);
		if (!pOperation)
		{
			it = armies.erase(it);
			continue;
		}

		CvArmyAI* pArmy = GET_PLAYER(it->eOwner).getArmyAI(it->iArmy);
		{
			VoxRlOperationCaptureScope captureScope(MOD_IPC_CHANNEL && gVoxRlCaptureEnabled,
				it->eOwner, it->eOwner, VOX_RL_OPERATION_CHANGE_EXECUTION, it->iOperation);
			// A casualty has already gone through RemoveUnit and UnitWasRemoved by this point.
			if (!pArmy || pArmy->GetOperationID() != it->iOperation)
			{
				pArmy = NULL;
				pOperation->SetToAbort(AI_ABORT_NO_UNITS);
			}
			else if (pOperation->GetOperationState() != AI_OPERATION_STATE_ABORTED)
			{
				const bool bSameOrder = MatchesPreparedArmyOrder(*pOperation, *pArmy, *it);
				if (bSameOrder || UpdatePreparedWaypoint(*pOperation, *pArmy, *it))
				{
					++it;
					continue;
				}
			}
		}
		it = RemovePreparedOperation(kTactical, it, *pOperation, pArmy);
	}
}

// Runs safety and native finishes for pending operations, then clears the prepared list.
void CvTacticalOperationalLoop::FinishPreparedArmies(CvTacticalAI& kTactical)
{
	vector<SOperationalPreparedArmy>& armies = kTactical.m_PreparedArmies;
	for (size_t i = 0; i < armies.size() && !IsHostStopped(); i++)
	{
		const SOperationalPreparedArmy& prepared = armies[i];
		CvAIOperation* pOperation = GET_PLAYER(prepared.eOwner).getAIOperation(prepared.iOperation);
		if (!pOperation)
			continue;
		CvArmyAI* pArmy = GET_PLAYER(prepared.eOwner).getArmyAI(prepared.iArmy);
		bool bProgressed = false;
		{
			VoxRlOperationCaptureScope captureScope(MOD_IPC_CHANNEL && gVoxRlCaptureEnabled,
				prepared.eOwner, prepared.eOwner, VOX_RL_OPERATION_CHANGE_EXECUTION, prepared.iOperation);
			if (pArmy)
			{
				if (pOperation->GetOperationState() != AI_OPERATION_STATE_ABORTED)
				{
					vector<CvUnit*> units;
					for (CvUnit* pUnit = pArmy->GetFirstUnit(); pUnit; pUnit = pArmy->GetNextUnit(pUnit))
						if (pUnit->canUseNow())
							units.push_back(pUnit);
					MoveArmyUnitsToSafety(units);
					if (IsHostStopped())
						break;
					bProgressed = pOperation->FinishMove(pArmy, prepared.start);
					if (bProgressed)
						pOperation->FinishTurn();
				}
				else
					pArmy->ReleaseAllUnits();
			}
			else
				pOperation->SetToAbort(AI_ABORT_NO_UNITS);
		}
		kTactical.ReportOperationResult(*pOperation, false, bProgressed);
		if (!bProgressed)
			pOperation->Kill();
	}
	armies.clear();
}

// Finalizes the whole call after turn scratch storage has been reclaimed.
struct SOperationalLoopScope
{
	SOperationalPerformance& timing;
	SOperationalTurnRecord* pRecord;
	SOperationalPerformance* pPerformance;
	CvTacticalAI& tactical;
	unsigned long long startNs;
	bool bTiming;
	// Retains the counters that outlive the working turn.
	SOperationalLoopScope(SOperationalPerformance& t, SOperationalTurnRecord* r, SOperationalPerformance* p, CvTacticalAI& ai, unsigned long long start, bool enabled) :
		timing(t), pRecord(r), pPerformance(p), tactical(ai), startNs(start), bTiming(enabled) {}
	// Includes teardown in whole-call time and emits the no-capture timing line once per pass.
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

// Closes host command work on both normal return and unwinding.
struct SOperationalCommandScope
{
	CvTacticalAI& tactical;
	// Opens one command's host work entry.
	SOperationalCommandScope(CvTacticalAI& ai, const SOperationalCommandRecord& command, int zone) : tactical(ai)
	{ CvTacticalOperationalLoop::BeginCommand(tactical, command, zone); }
	// Retains committed effects when command execution stops.
	~SOperationalCommandScope() { CvTacticalOperationalLoop::EndCommand(tactical); }
};

// Picks the top ready fight, Reinforce or Withdraw offer, checks it, executes it on a pass, retires the offers it
// covered, then estimates every open offer again and wakes waiting fights on helpful changes.
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
			if (turn.bMarchEnabled)
				SyncArmyBattlefields(kTactical, turn);
			BuildThreats(kTactical, turn);
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
		// Native hosts rank offers directly without copying battlefield observations.
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
					for (int iSlot = 0; iSlot < NUM_OPERATIONAL_SLOTS; iSlot++)
					{
						const eOperationalSlot eSlot = static_cast<eOperationalSlot>(iSlot);
						if (!OperationalSupportsSlot(field.eCenter, eSlot) || OperationalOfferState(field, eSlot) != OPERATIONAL_OFFER_READY)
							continue;
						const int iOffer = OperationalOfferIdentity(static_cast<int>(i), eSlot);
						if (iNative < 0 ||
							OperationalOfferBefore(field, eSlot, iOffer,
								turn.battlefields[OperationalOfferBattlefield(iNative)], OperationalOfferSlot(iNative), iNative))
							iNative = iOffer;
						if (bProvider)
						{
							SOperationalSelectableOffer offer;
							offer.iOffer = iOffer;
							offer.field = field;
							offer.eSlot = eSlot;
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
			// A permitted objective can close without a native battlefield, or end its menu.
			if (bPermittedMenu && choice.iEffective == -2) continue;
			if (choice.iEffective < 0)
			{
				if (pRecord) record.steps.back().szEnd = "noReadyOffer";
				break;
			}
			const int iBest = OperationalOfferBattlefield(choice.iEffective);
			const eOperationalSlot eBestSlot = OperationalOfferSlot(choice.iEffective);
			const double fBest = OperationalOfferEstimate(turn.battlefields[iBest], eBestSlot);
			SOperationalBattlefield& field = turn.battlefields[iBest];
			const bool bArmyMarch = field.eCenter == OPERATIONAL_CENTER_ARMY;
			const bool bCamp = field.eCenter == OPERATIONAL_CENTER_CAMP;
			const bool bCampClaim = bCamp && !field.bCampFogged;
			if ((bArmyMarch && (!OperationalSupportsSlot(field.eCenter, eBestSlot) || field.eReinforce != OPERATIONAL_OFFER_READY)) ||
				(bCamp && (!OperationalSupportsSlot(field.eCenter, eBestSlot) ||
					(field.bCampFogged && eBestSlot != OPERATIONAL_SLOT_REINFORCE) ||
					OperationalOfferIsClosed(OperationalOfferState(field, eBestSlot)))))
			{
				if (bProvider)
					ReportExecution(choice.iEffective, OPERATIONAL_CHECK_NO_REACHABLE, NULL);
				continue;
			}
			SOperationalCheckRecord check;
			check.iBattlefield = iBest;
			check.iStep = iCommands;
			check.eSlot = eBestSlot;
			check.eBefore = OperationalOfferState(field, eBestSlot);
			check.fBefore = fBest;
			// A check can also change the battlefield's other offers, which the trace records separately.
			eOperationalOfferState aeStateBefore[NUM_OPERATIONAL_SLOTS];
			double afEstimateBefore[NUM_OPERATIONAL_SLOTS];
			for (int iSlot = 0; iSlot < NUM_OPERATIONAL_SLOTS; iSlot++)
			{
				const eOperationalSlot eSlot = static_cast<eOperationalSlot>(iSlot);
				aeStateBefore[iSlot] = OperationalOfferState(field, eSlot);
				afEstimateBefore[iSlot] = OperationalOfferEstimate(field, eSlot);
			}
			const double fRealizedReinforceBefore = field.reinforce.fRealizedEstimate;
			const double fShareWithHelpBefore = field.reinforce.fShareWithHelp;
			const unsigned long long readinessBefore = turn.performance.readinessNs;
			vector<SOperationalMarch> withdraw;
			bool bFormationWithdraw = false;
			eOperationalCheckResult eResult;
			{
				SOperationalTimer timer(turn.bTiming ? &turn.performance.readinessNs : NULL);
				// Only native fight selection initiates forecast path checks.
				eResult = !bCamp && !bProvider && eBestSlot == OPERATIONAL_SLOT_FIGHT
					? CheckForecastReadiness(kTactical, turn, field) : OPERATIONAL_CHECK_PASSED;
				if (bCampClaim)
				{
					CvPlot* pCamp = BattlefieldPlot(field);
					if (!EstimateCampClaim(kTactical, *pCamp, field, true))
						eResult = OPERATIONAL_CHECK_TARGET_GONE;
					else
					{
						const eOperationalSlot eOther = eBestSlot == OPERATIONAL_SLOT_FIGHT ? OPERATIONAL_SLOT_REINFORCE : OPERATIONAL_SLOT_FIGHT;
						eOperationalOfferState& eOtherState = OperationalOfferState(field, eOther);
						if (eOtherState == OPERATIONAL_OFFER_READY || eOtherState == OPERATIONAL_OFFER_INACTIVE)
							eOtherState = OperationalOfferEstimate(field, eOther) > 0.0 ? OPERATIONAL_OFFER_READY : OPERATIONAL_OFFER_INACTIVE;
						const double fChecked = OperationalOfferEstimate(field, eBestSlot);
						if (field.iCampClaimant < 0)
							eResult = OPERATIONAL_CHECK_NO_REACHABLE;
						else if (fChecked <= 0.0 || fChecked < fBest)
							eResult = OPERATIONAL_CHECK_REDUCED;
					}
				}
				else if (eResult == OPERATIONAL_CHECK_PASSED)
				{
					switch (eBestSlot)
					{
					case OPERATIONAL_SLOT_REINFORCE:
						eResult = bArmyMarch ? CheckArmyMarch(kTactical, turn, field) : CheckReinforce(kTactical, turn, field);
						break;
					case OPERATIONAL_SLOT_WITHDRAW:
						eResult = CheckWithdraw(kTactical, turn, field, withdraw, bFormationWithdraw);
						break;
					default:
						eResult = CheckFight(kTactical, turn, field);
						break;
					}
				}
				turn.performance.checks++;
			}
			if (bProvider) ReportExecution(choice.iEffective, eResult, NULL);
			check.eResult = eResult;
			// A reduced offer competes again without consuming a failed readiness attempt.
			if (eResult == OPERATIONAL_CHECK_REDUCED)
				OperationalOfferState(field, eBestSlot) =
					OperationalOfferEstimate(field, eBestSlot) > 0.0 ? OPERATIONAL_OFFER_READY : OPERATIONAL_OFFER_INACTIVE;
			else if (eResult != OPERATIONAL_CHECK_PASSED)
			{
				if (bCampClaim && eResult == OPERATIONAL_CHECK_TARGET_GONE)
				{
					if (!OperationalOfferIsClosed(field.eFight))
						field.eFight = OPERATIONAL_OFFER_DROPPED;
					if (!OperationalOfferIsClosed(field.eReinforce))
						field.eReinforce = OPERATIONAL_OFFER_DROPPED;
				}
				else if (bArmyMarch && eResult == OPERATIONAL_CHECK_TARGET_GONE)
					field.eReinforce = OPERATIONAL_OFFER_DROPPED;
				else if (eBestSlot == OPERATIONAL_SLOT_REINFORCE || bCampClaim)
					OperationalOfferState(field, eBestSlot) = OPERATIONAL_OFFER_INACTIVE;
				else if (eBestSlot == OPERATIONAL_SLOT_WITHDRAW)
				{
					// A failed Withdraw rests inactive until the failure limit, then leaves the turn.
					field.iWithdrawFailures++;
					field.fWithdrawEstimate = WithdrawPriority(turn, field, field.fWithdrawExposure, field.fWithdrawShare);
					field.eWithdraw = field.iWithdrawFailures >= iMaxFailures ? OPERATIONAL_OFFER_SKIPPED : OPERATIONAL_OFFER_INACTIVE;
				}
				else if (eBestSlot == OPERATIONAL_SLOT_FIGHT)
				{
					field.iFailures++;
					OperationalOfferState(field, eBestSlot) =
						field.iFailures >= iMaxFailures ? OPERATIONAL_OFFER_SKIPPED : OPERATIONAL_OFFER_WAITING;
				}
			}
			// Withdraw reads the share with help from a Reinforce that scores on realized stake, so refresh it when either changes.
			if ((eBestSlot == OPERATIONAL_SLOT_FIGHT || eBestSlot == OPERATIONAL_SLOT_REINFORCE) &&
				OperationalSupportsSlot(field.eCenter, OPERATIONAL_SLOT_WITHDRAW) &&
				(field.eWithdraw == OPERATIONAL_OFFER_READY || field.eWithdraw == OPERATIONAL_OFFER_INACTIVE) &&
				(field.eReinforce != aeStateBefore[OPERATIONAL_SLOT_REINFORCE] ||
					field.reinforce.fRealizedEstimate != fRealizedReinforceBefore ||
					field.reinforce.fShareWithHelp != fShareWithHelpBefore))
			{
				CvPlot* pWithdrawTarget = GC.getMap().plotByIndex(field.iPlotIndex);
				if (pWithdrawTarget)
				{
					SOperationalLocalSet local;
					GatherTargetLocalSet(kTactical, turn.ePlayer, *pWithdrawTarget, local, false,
						field.eCenter == OPERATIONAL_CENTER_ENEMY_UNIT);
					EstimateWithdraw(kTactical, turn, local, field);
					field.eWithdraw = field.fWithdrawEstimate > 0.0 ? OPERATIONAL_OFFER_READY : OPERATIONAL_OFFER_INACTIVE;
				}
			}
			// The failure branch above already recorded a failed Withdraw in its own count.
			check.eAfter = OperationalOfferState(field, eBestSlot);
			check.fAfter = OperationalOfferEstimate(field, eBestSlot);
			if (pRecord)
			{
				SOperationalTimer timer(turn.bTiming ? &turn.performance.recordNs : NULL);
				record.steps.back().readinessNs += turn.performance.readinessNs - readinessBefore;
				SOperationalEvent event = {"check", iBest, static_cast<int>(record.checks.size()), eBestSlot,
										   check.eBefore, check.eAfter, check.fBefore, check.fAfter};
				record.steps.back().events.push_back(event);
				// A check can change the battlefield's other offers, each as its own update event.
				for (int iSlot = 0; iSlot < NUM_OPERATIONAL_SLOTS; iSlot++)
				{
					const eOperationalSlot eSlot = static_cast<eOperationalSlot>(iSlot);
					if (eSlot == eBestSlot)
						continue;
					const eOperationalOfferState eState = OperationalOfferState(field, eSlot);
					const double fEstimate = OperationalOfferEstimate(field, eSlot);
					if (eState != aeStateBefore[iSlot] || fEstimate != afEstimateBefore[iSlot])
					{
						SOperationalEvent update = {"update", iBest, event.iRecord, eSlot, aeStateBefore[iSlot], eState,
													afEstimateBefore[iSlot], fEstimate};
						record.steps.back().events.push_back(update);
					}
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
				if (bArmyMarch)
					kTactical.LogTacticalMessage(CvString::format(
						"Operational step %d check of ArmyMarch %d, operation %d, army %d, waypoint %d: %.6f to %.6f, %s; "
						"theater %.6f to %.6f, goal weight %.6f, readiness gain %.6f",
						iCommands, iBest, field.iOperation, field.iTargetID, field.iPlotIndex, fBest, field.fReinforceEstimate, aszResults[eResult],
						field.armyMarch.fUnassistedTheater, field.fTheater, field.armyMarch.fGoalWeight, field.armyMarch.fReadinessGain));
				else if (eBestSlot == OPERATIONAL_SLOT_REINFORCE)
				{
					const SOperationalReinforce& reinforce = field.reinforce;
					kTactical.LogTacticalMessage(CvString::format(
						"Operational step %d check of Reinforce %d at plot %d, estimate %.6f: %s; theater %.6f, realized stake %.6f, L %.0f, T %.0f, "
						"ratio %.6f, need %.0f, selected strength %.0f, help %.0f, share %.6f, share with help %.6f, swing %.6f, role unit %d, role help "
						"%.0f, missing role %d; total stake %.6f, potential stake %.6f, turns to contact %.6f, potential weight %.6f",
						iCommands, iBest, field.iPlotIndex, field.fReinforceEstimate, aszResults[eResult], field.fTheater, field.stake.fRealized,
						reinforce.fLocal, reinforce.fEnemy, reinforce.fRatio, reinforce.fNeed, reinforce.fSelectedStrength, reinforce.fHelp, reinforce.fShare,
						reinforce.fShareWithHelp, reinforce.fSwing, reinforce.iRoleUnit, reinforce.fRoleHelp, reinforce.bMissingRole ? 1 : 0,
						field.stake.fStake, field.stake.fPotential, reinforce.fTurnsToContact, reinforce.fPotentialWeight));
				}
				else if (eBestSlot == OPERATIONAL_SLOT_WITHDRAW)
					kTactical.LogTacticalMessage(CvString::format(
						"Operational step %d check of Withdraw %d at plot %d, estimate %.6f: %s; exposure %.6f, share %.6f, share with help "
						"%.6f, destinations %d and %d",
						iCommands, iBest, field.iPlotIndex, field.fWithdrawEstimate, aszResults[eResult], field.fWithdrawExposure,
						field.fExposureShare, field.fWithdrawShare, field.aiWithdrawDestination[0], field.aiWithdrawDestination[1]));
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
			command.eKind = OperationalSlotKind(field.eCenter, eBestSlot);
			command.eOwner = field.eOwner;
			if (bArmyMarch)
			{
				command.iOperation = field.iOperation;
				command.iArmy = field.iTargetID;
				command.armyMarch = field.armyMarch;
			}
			// an offer other than a fight runs at its checked estimate, which a replaced selection may have raised
			command.fEstimate = eBestSlot == OPERATIONAL_SLOT_FIGHT ? fBest : OperationalOfferEstimate(field, eBestSlot);
			command.iFailuresBefore = eBestSlot == OPERATIONAL_SLOT_WITHDRAW ? field.iWithdrawFailures
				: eBestSlot == OPERATIONAL_SLOT_REINFORCE ? 0 : field.iFailures;
			command.fTheater = field.fTheater;
			if (eBestSlot == OPERATIONAL_SLOT_REINFORCE && !bArmyMarch && !bCampClaim)
			{
				command.fSwing = field.reinforce.fSwing;
				command.fTurnsToContact = field.reinforce.fTurnsToContact;
				command.fPotentialWeight = field.reinforce.fPotentialWeight;
				command.fRatio = field.reinforce.fRatio;
				command.fNeed = field.reinforce.fNeed;
				command.fSelectedStrength = field.reinforce.fSelectedStrength;
			}
			command.stake = field.stake;
			// The field keeps the checked exposure, so a Withdraw records the inputs its reduced estimate used.
			if (eBestSlot == OPERATIONAL_SLOT_WITHDRAW)
			{
				command.fWithdrawExposure = field.fWithdrawExposure;
				command.fWithdrawShare = field.fWithdrawShare;
			}

			if (pRecord)
			{
				SOperationalTimer timer(turn.bTiming ? &turn.performance.recordNs : NULL);
				SOperationalEvent event = {
					"command", iBest, iCommands, eBestSlot, check.eAfter, OPERATIONAL_OFFER_EXECUTED, check.fAfter, command.fEstimate};
				record.steps.back().events.push_back(event);
				record.steps.back().iCommand = iCommands;
				record.steps.back().szEnd = "command";
			}
			SOperationalBoard before;
			SnapshotBoard(turn, before);
			bool bArmyMarchOrderChanged = false;
			const unsigned long long executeBefore = turn.performance.executeNs;
			{
				SOperationalTimer timer(turn.bTiming ? &turn.performance.executeNs : NULL, &turn.performance.recordNs);
				turn.performance.commands++;
				SOperationalCommandScope commandScope(kTactical, command, field.iZoneID);
				if (bCampClaim)
					ExecuteCampClaim(kTactical, turn, field, command);
				else switch (eBestSlot)
				{
				case OPERATIONAL_SLOT_REINFORCE:
				case OPERATIONAL_SLOT_WITHDRAW:
					if (bArmyMarch)
					{
						// Gather and path orders retain their operation alongside the command.
						VoxRlOperationCaptureScope operationScope(MOD_IPC_CHANNEL && gVoxRlCaptureEnabled,
							turn.ePlayer, turn.ePlayer, VOX_RL_OPERATION_CHANGE_EXECUTION, command.iOperation);
						ExecuteMarch(kTactical, turn, field, withdraw, command, bFormationWithdraw, bArmyMarchOrderChanged);
					}
					else
						ExecuteMarch(kTactical, turn, field, withdraw, command, bFormationWithdraw, bArmyMarchOrderChanged);
					break;
				default:
					ExecuteFight(kTactical, turn, field, command);
					break;
				}
			}
			if (pRecord)
			{
				SOperationalTimer timer(turn.bTiming ? &turn.performance.recordNs : NULL);
				record.steps.back().executeNs = turn.performance.executeNs - executeBefore;
				record.steps.back().events.back().fAfter = command.fEstimate;
			}

			// Yield spends the command slot but leaves the March open for the next estimate.
			const bool bYielded = command.iYieldPlot >= 0;
			if (pRecord && bYielded)
			{
				record.steps.back().events.back().szType = "yield";
				record.steps.back().events.back().eAfter = OPERATIONAL_OFFER_READY;
			}
			if (bProvider) ReportExecution(choice.iEffective, OPERATIONAL_CHECK_PASSED, &command);
			iCommands++;
			const bool bRefused = command.admission.eResult == OPERATIONAL_ADMISSION_REFUSED;
			if (eBestSlot == OPERATIONAL_SLOT_REINFORCE)
			{
				field.eReinforce = bYielded ? OPERATIONAL_OFFER_READY :
					bRefused ? OPERATIONAL_OFFER_REFUSED : OPERATIONAL_OFFER_EXECUTED;
				// A Reinforce that moved a unit excludes this battlefield's open Withdraw for the turn. A
				// Reinforce that moved nobody sent no units that a Withdraw could pull back, so the Withdraw stays open.
				// A Withdraw that can never open, at our own city or with the switch off, stays inactive.
				if (!bRefused && command.bMoved && OperationalSupportsSlot(field.eCenter, OPERATIONAL_SLOT_WITHDRAW) &&
					turn.weights.afConstant[OPERATIONAL_CONSTANT_WITHDRAW_ENABLED] != 0.0 &&
					(field.eWithdraw == OPERATIONAL_OFFER_READY || field.eWithdraw == OPERATIONAL_OFFER_INACTIVE))
				{
					field.eWithdraw = OPERATIONAL_OFFER_RETIRED;
					command.vRetiredWithdraw.push_back(field.iID);
				}
			}
			else if (eBestSlot == OPERATIONAL_SLOT_WITHDRAW)
			{
				field.eWithdraw = bRefused ? OPERATIONAL_OFFER_REFUSED : OPERATIONAL_OFFER_EXECUTED;
				if (!bRefused)
				{
					// Recruits that stayed put can still join a later Withdraw. A surviving formation
					// recruit that retreated keeps its stop for the rest of the turn.
					for (size_t i = 0; i < command.vMoved.size(); i++)
					{
						turn.withdrawnUnits.insert(command.vMoved[i]);
						if (bFormationWithdraw)
						{
							CvUnit* pUnit = kTactical.m_pPlayer->getUnit(command.vMoved[i]);
							if (pUnit)
								pUnit->SetTurnProcessed(true);
						}
					}
					// A Withdraw that moved anyone closes this battlefield's own fight and Reinforce only.
					if (command.bMoved)
					{
						if (field.eFight == OPERATIONAL_OFFER_READY || field.eFight == OPERATIONAL_OFFER_INACTIVE ||
							field.eFight == OPERATIONAL_OFFER_WAITING)
						{
							field.eFight = OPERATIONAL_OFFER_RETIRED;
							command.vRetired.push_back(field.iID);
						}
						if (field.eReinforce == OPERATIONAL_OFFER_READY || field.eReinforce == OPERATIONAL_OFFER_INACTIVE)
						{
							field.eReinforce = OPERATIONAL_OFFER_RETIRED;
							command.vRetiredReinforce.push_back(field.iID);
						}
					}
				}
			}
			else
			{
				OperationalOfferState(field, eBestSlot) = bRefused ? OPERATIONAL_OFFER_REFUSED : OPERATIONAL_OFFER_EXECUTED;
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
			RefreshAfterCommand(kTactical, turn, field, command, bArmyMarchOrderChanged, before, changes);
			if (bLog)
			{
				const char* const szKind = bArmyMarch ? "ArmyMarch" : eBestSlot == OPERATIONAL_SLOT_REINFORCE ? "Reinforce"
					: eBestSlot == OPERATIONAL_SLOT_WITHDRAW ? "Withdraw" : "fight";
				if (bYielded)
					kTactical.LogTacticalMessage(CvString::format("Operational ArmyMarch %d yielded at contact plot %d", iBest, command.iYieldPlot));
				kTactical.LogTacticalMessage(CvString::format(
					"Operational step %d command %d: %s %d at plot %d, search %s, %d fights, %d Reinforce and %d Withdraw retired", command.iStep,
					command.iCommand, szKind, iBest, command.iPlotIndex,
					command.bSearchRan ? "ran" : "did not run", (int)command.vRetired.size(), (int)command.vRetiredReinforce.size(),
					(int)command.vRetiredWithdraw.size()));
			}

			{
				SOperationalTimer timer(turn.bTiming ? &turn.performance.estimateNs : NULL);
				EstimateOffers(kTactical, turn, &changes);
			}
			if (pRecord)
				RecordStep(turn, *pRecord);
		}
		// A stopped pass keeps committed effects and skips the remaining operation finishes.
		FinishPreparedArmies(kTactical);
	}
	catch (...)
	{
		// Discard the work left unfinished by the stopped pass.
		kTactical.m_PreparedArmies.clear();
		if (pRecord && !pRecord->steps.empty()) pRecord->steps.back().szEnd = "stopped";
		CloseRecord(turn, pRecord);
		timing = turn.performance;
		throw;
	}
	CloseRecord(turn, pRecord);
	timing = turn.performance;
}

// Refreshes the board, retires covered offers, and adds targets exposed by the command.
void CvTacticalOperationalLoop::RefreshAfterCommand(CvTacticalAI& kTactical, SOperationalTurn& turn, SOperationalBattlefield& field,
	SOperationalCommandRecord& command, bool bArmyMarchOrderChanged, const SOperationalBoard& before, SOperationalChanges& changes)
{
	SOperationalTimer retireTimer(turn.bTiming ? &turn.performance.retireNs : NULL);

	// Settle already aborted operations and release survivors before reading the new board.
	RefreshPreparedArmies(kTactical);

	// the exemptions read the threats after the command, since its search may have moved units into a target's reach
	BuildThreats(kTactical, turn);

	for (size_t i = 0; i < turn.battlefields.size(); i++)
	{
		SOperationalBattlefield& other = turn.battlefields[i];
		// an enemy-unit fight that ran its search covered the unit fights around its target
		if (OperationalKindSlot(command.eKind) == OPERATIONAL_SLOT_FIGHT && command.bSearchRan && RetiresFight(field, other))
		{
			other.eFight = OPERATIONAL_OFFER_RETIRED;
			command.vRetired.push_back(other.iID);
		}
		// a Reinforce that moved a unit covered the enemy groups it counted
		if (command.eKind == OPERATIONAL_KIND_REINFORCE && command.bMoved && RetiresReinforce(field, other))
		{
			other.eReinforce = OPERATIONAL_OFFER_RETIRED;
			command.vRetiredReinforce.push_back(other.iID);
		}
		// Discard reach and March checks before offers read the new board and move budgets.
		other.ResetReinforceCheck();
	}
	// The old order was refused. Sync and estimate the new order with the checks cleared above.
	if (bArmyMarchOrderChanged)
		field.eReinforce = OPERATIONAL_OFFER_INACTIVE;
	if (turn.bMarchEnabled)
		SyncArmyBattlefields(kTactical, turn);

	SOperationalBoard after;
	SnapshotBoard(turn, after);
	DiffBoard(before, after, changes);
	// Movable survivors released by native operation cleanup can join later offers.
	for (size_t i = 0; i < changes.vReleasedUnits.size(); i++)
	{
		const CvUnit* pUnit = kTactical.m_pPlayer->getUnit(changes.vReleasedUnits[i]);
		if (pUnit && pUnit->canMove())
			turn.startRoster.insert(pUnit->GetID());
	}

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

	// Discover after retirement, and only when the enemy observations changed.
	if (turn.bMarchEnabled && (!changes.vEnemyPlots.empty() || !changes.vEnemyCityPlots.empty()))
		DiscoverEnemyBattlefields(kTactical, turn, after, &changes);
}

//------------------------------------------------------------------------------
// turn, threats and battlefields
//------------------------------------------------------------------------------

// Resolves mapped weights, typical unit worth, the best city value and the raw city allowance for this turn.
void CvTacticalOperationalLoop::BuildTurn(CvTacticalAI& kTactical, SOperationalTurn& turn)
{
	CvPlayer& kPlayer = *kTactical.m_pPlayer;
	const PlayerTypes ePlayer = kPlayer.GetID();
	turn.ePlayer = ePlayer;
	GetOperationalSettings(ePlayer, &turn.settings);
	// Resolve the discovery gate from the same settings used for the turn's weights.
	turn.bMarchEnabled = IsMarchEnabled(kPlayer, &turn.settings);
	GetMappedMilitaryFlavors(ePlayer, turn.aiMapped);
	if (turn.bMarchEnabled)
	{
		const SVpFlavorLayers gold = ReadVpFlavorLayers(kPlayer, (FlavorTypes)GC.getInfoTypeForString("FLAVOR_GOLD", true));
		if (gold.bFound)
			turn.iMarchGold = MapVpFlavorRow(gold, GD_INT_GET(PERSONALITY_FLAVOR_MAX_VALUE));
	}
	ResolveOperationalWeights(turn.settings, turn.aiMapped, turn.weights);
	// The roster when the loop starts, kept because commands process units out of the live list.
	turn.startRoster.clear();
	turn.startRoster.insert(kTactical.m_CurrentTurnUnits.begin(), kTactical.m_CurrentTurnUnits.end());

	turn.iStrikeScan = 0;
	int iLoop = 0;
	for (CvUnit* pUnit = kPlayer.firstUnit(&iLoop); pUnit != NULL; pUnit = kPlayer.nextUnit(&iLoop))
	{
		if (!pUnit->isDelayedDeath() && (pUnit->getDomainType() == DOMAIN_LAND || pUnit->getDomainType() == DOMAIN_SEA))
			turn.iStrikeScan = std::max(turn.iStrikeScan, pUnit->baseMoves(false) * 4);
	}
	double fGoldPerWorth = 0.0;
	ReferenceUnitScales(kPlayer, turn.fTypicalUnitWorth, fGoldPerWorth);
	const double fTypicalUnitGold = turn.fTypicalUnitWorth * fGoldPerWorth;
	turn.fCampBounty = fTypicalUnitGold > 0.0 ? 0.5 * std::max(0, CampBounty(kPlayer)) / fTypicalUnitGold : 0.0;
	turn.fTypicalUnitStrength = TypicalUnitStrength(kPlayer);
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

// Sums a threat's largest allocations up to its attacks per turn, skipping the victims that pExclude names.
// With no exclusions this is the threat's whole exposure.
double CvTacticalOperationalLoop::CappedExposure(const SOperationalThreatExposure& threat, const set<int>* pExclude)
{
	vector<double> values;
	values.reserve(threat.vAllocations.size());
	for (size_t i = 0; i < threat.vAllocations.size() && i < threat.vUnitIDs.size(); i++)
		if (!pExclude || pExclude->count(threat.vUnitIDs[i]) == 0)
			values.push_back(threat.vAllocations[i]);
	std::sort(values.begin(), values.end());
	double fExposure = 0.0;
	const size_t nAttacks = static_cast<size_t>(std::max(0, threat.iAttacks));
	for (size_t i = 0; i < values.size() && i < nAttacks; i++)
		fExposure += values[values.size() - 1 - i];
	return fExposure;
}

// Keeps each attacker's direct hit on its selected defender, including damage from earlier hits.
static void AdjustStackExposure(const CvPlot& kPlot, const CvDangerPlotContents& contents, vector<SPlacedUnit>& occupants,
	vector<vector<SOperationalThreatDamage> >& damage, vector<int>& danger)
{
	// A source on the danger list has a route to one hit. A later source can hit the next defender if that hit kills it.
	for (int iKind = 0; iKind < 2; iKind++)
	{
		const DangerUnitVector& sources = iKind == 0 ? contents.m_apUnits : contents.m_apCities;
		for (DangerUnitVector::const_iterator it = sources.begin(); it != sources.end(); ++it)
		{
			const CvUnit* pAttacker = iKind == 0 ? GET_PLAYER(it->first).getUnit(it->second) : NULL;
			if (iKind == 0 && (!pAttacker || pAttacker->isDelayedDeath()))
				continue;
			// Keep the danger forecast's allowance for enemies that do not see the tile yet.
			const bool bIgnoreVisibility = !kPlot.isVisible(GET_PLAYER(it->first).getTeam());
			const CvUnit* pDefender = kPlot.getBestDefender(NO_PLAYER, it->first, pAttacker, true, bIgnoreVisibility,
				false, !pAttacker || pAttacker->IsCanAttackRanged(), NULL, &occupants);
			for (size_t i = 0; i < occupants.size(); i++)
				for (size_t j = 0; j < damage[i].size(); j++)
				{
					SOperationalThreatDamage& hit = damage[i][j];
					if (hit.bCity != (iKind == 1) || hit.eOwner != it->first || hit.iID != it->second)
						continue;
					if (occupants[i].pUnit != pDefender)
					{
						danger[i] -= hit.iDirectDamage;
						hit.iDirectDamage = 0;
					}
					occupants[i].iExtraDamage += hit.TotalDamage();
					break;
				}
		}
	}
}

// Splits each victim's exposure among its threats and records its danger for recruitment.
static void AttributePlotExposure(PlayerTypes ePlayer, const vector<SPlacedUnit>& occupants,
	const vector<vector<SOperationalThreatDamage> >& damage, const vector<int>& danger, OperationalThreatMap& units,
	OperationalThreatMap& cities, map<int, int>* pEndangered, bool bAllVictims, const set<int>* pEnrolled)
{
	for (size_t i = 0; i < occupants.size(); i++)
	{
		const CvUnit& kUnit = *occupants[i].pUnit;
		if (kUnit.getOwner() != ePlayer || (!bAllVictims && (kUnit.IsCivilianUnit() || kUnit.getArmyID() != -1) &&
			(!pEnrolled || pEnrolled->count(kUnit.GetID()) == 0)))
			continue;
		const vector<SOperationalThreatDamage>& threats = damage[i];
		int iThreatTotal = 0;
		for (size_t j = 0; j < threats.size(); j++)
			iThreatTotal += std::max(0, threats[j].TotalDamage());
		// The formation's fallback ranking also needs the damage when a defender absorbs every hit.
		if (pEndangered && (bAllVictims || iThreatTotal > 0))
			(*pEndangered)[kUnit.GetID()] = danger[i];
		if (iThreatTotal <= 0)
			continue;

		const double fExposure = UnitExposure(CvTacticalOperationalLoop::OurUnitWorth(kUnit),
			kUnit.getDamage(), kUnit.GetMaxHitPoints(), danger[i]);
		// Fog and terrain keep their share of the danger without belonging to a threat.
		const int iShareTotal = std::max(danger[i], iThreatTotal);
		for (size_t j = 0; j < threats.size(); j++)
		{
			const SOperationalThreatDamage& hit = threats[j];
			if (hit.TotalDamage() <= 0)
				continue;
			const pair<int, int> key(static_cast<int>(hit.eOwner), hit.iID);
			SOperationalThreatExposure& threat = hit.bCity ? cities[key] : units[key];
			if (threat.vUnitIDs.empty())
			{
				const CvUnit* pAttacker = hit.bCity ? NULL : GET_PLAYER(hit.eOwner).getUnit(hit.iID);
				threat.iAttacks = hit.bCity ? 1 : (pAttacker ? pAttacker->getNumAttacks() : 0);
			}
			threat.vAllocations.push_back(fExposure * hit.TotalDamage() / iShareTotal);
			threat.vUnitIDs.push_back(kUnit.GetID());
		}
	}
}

// Projects each occupant's damage, then attributes exposure with the stack rules for the requested layout.
void CvTacticalOperationalLoop::AllocatePlotExposure(PlayerTypes ePlayer, const CvPlot& kPlot, vector<SPlacedUnit>& occupants, int iRisk,
	OperationalThreatMap& units, OperationalThreatMap& cities, map<int, int>* pEndangered, bool bAllVictims,
	const set<int>* pEnrolled, SOperationalExposure* pCityFight)
{
	CvDangerPlotContents& contents = DangerContents(ePlayer, kPlot);
	if (contents.m_apUnits.empty() && contents.m_apCities.empty())
		return;
	for (vector<SPlacedUnit>::iterator it = occupants.begin(); it != occupants.end(); )
		if (it->pUnit->isDelayedDeath() || it->pUnit->IsDead())
			it = occupants.erase(it);
		else
			++it;
	vector<vector<SOperationalThreatDamage> > damage(occupants.size());
	vector<int> danger(occupants.size(), 0);
	for (size_t i = 0; i < occupants.size(); i++)
	{
		const CvUnit* pUnit = occupants[i].pUnit;
		if (pUnit->getTeam() != GET_PLAYER(ePlayer).getTeam() || pUnit->getDomainType() == DOMAIN_AIR)
			continue;
		if (kPlot.isFriendlyCity(*pUnit))
		{
			// Attribute the risk to an enrolled member sheltering in a city to the city's attackers.
			if (pUnit->getOwner() != ePlayer || !pEnrolled || pEnrolled->count(pUnit->GetID()) == 0)
				continue;
			const CvCity* pCity = kPlot.getPlotCity();
			danger[i] = GetDangerByThreat(contents, pUnit, SUnitIDValueContainer(), 0, AIR_ACTION_ATTACK, damage[i], iRisk);
			if (danger[i] > contents.m_iImprovementDamage)
				GetCityDangerByThreat(contents, pCity, pUnit, SUnitIDValueContainer(), 0, &damage[i], iRisk);
		}
		else
			danger[i] = GetDangerByThreat(contents, pUnit, SUnitIDValueContainer(), 0, AIR_ACTION_ATTACK, damage[i], iRisk);
	}

	if (pCityFight)
	{
		// Fight skips stack adjustment on cities, even if no member is enrolled on this plot.
		map<int, int> fightEndangered;
		AttributePlotExposure(ePlayer, occupants, damage, danger, pCityFight->units, pCityFight->cities,
			pEndangered ? &fightEndangered : NULL, bAllVictims, NULL);
		if (occupants.size() > 1)
			AdjustStackExposure(kPlot, contents, occupants, damage, danger);
		AttributePlotExposure(ePlayer, occupants, damage, danger, units, cities, pEndangered, bAllVictims, NULL);
		if (pEndangered)
			for (map<int, int>::const_iterator it = fightEndangered.begin(); it != fightEndangered.end(); ++it)
				(*pEndangered)[it->first] = it->second;
		return;
	}

	// City shares already account for the projected garrison.
	if (occupants.size() > 1 && !(pEnrolled && kPlot.isCity()))
		AdjustStackExposure(kPlot, contents, occupants, damage, danger);
	AttributePlotExposure(ePlayer, occupants, damage, danger, units, cities, pEndangered, bAllVictims, pEnrolled);
}

// Appends one plot's allocations before applying the attacker's budget across the whole board.
static void AppendPlotExposure(const OperationalThreatMap& plot, OperationalThreatMap& board)
{
	for (OperationalThreatMap::const_iterator it = plot.begin(); it != plot.end(); ++it)
	{
		const SOperationalThreatExposure& shares = it->second;
		SOperationalThreatExposure& threat = board[it->first];
		if (threat.vUnitIDs.empty())
			threat.iAttacks = shares.iAttacks;
		threat.vUnitIDs.insert(threat.vUnitIDs.end(), shares.vUnitIDs.begin(), shares.vUnitIDs.end());
		threat.vAllocations.insert(threat.vAllocations.end(), shares.vAllocations.begin(), shares.vAllocations.end());
	}
}

// Caches plot exposure for performance; both passes produce the same results without the cache.
struct SOperationalSharedExposurePlots
{
	const set<int>& enrolled;
	map<int, SOperationalExposure> plots;

	// Borrows the enrolled IDs for both passes.
	explicit SOperationalSharedExposurePlots(const set<int>& members) : enrolled(members) {}

	// True if a living enrolled member occupies this plot.
	bool HasEnrolled(PlayerTypes ePlayer, const vector<SPlacedUnit>& occupants) const
	{
		for (size_t i = 0; i < occupants.size(); i++)
			if (occupants[i].pUnit->getOwner() == ePlayer && enrolled.count(occupants[i].pUnit->GetID()) > 0)
				return true;
		return false;
	}
};

// Allocates occupied plots in the original order for this layout, then caps each attacker across the board.
static void BuildOccupiedExposure(const SOperationalTurn& turn, OperationalThreatMap& units, OperationalThreatMap& cities,
	map<int, int>* pEndangered, bool bAllVictims, const set<int>* pEnrolled = NULL, SOperationalSharedExposurePlots* pShared = NULL)
{
	CvPlayer& kPlayer = GET_PLAYER(turn.ePlayer);
	CvBoardPlacement board;
	vector<SPlacedUnit> occupants;
	set<int> plots;
	int iLoop = 0;
	for (CvUnit* pUnit = kPlayer.firstUnit(&iLoop); pUnit != NULL; pUnit = kPlayer.nextUnit(&iLoop))
	{
		CvPlot* pPlot = pUnit->plot();
		const bool bEnrolled = pEnrolled && pEnrolled->count(pUnit->GetID()) > 0;
		if (pUnit->isDelayedDeath() || pUnit->getDomainType() == DOMAIN_AIR || !pPlot ||
			(!bEnrolled && (pPlot->isFriendlyCity(*pUnit) || (!bAllVictims && (pUnit->IsCivilianUnit() || pUnit->getArmyID() != -1)))) ||
			!plots.insert(pPlot->GetPlotIndex()).second)
			continue;
		const int iPlot = pPlot->GetPlotIndex();
		if (pShared && pEnrolled)
		{
			map<int, SOperationalExposure>::const_iterator common = pShared->plots.find(iPlot);
			if (common != pShared->plots.end())
			{
				AppendPlotExposure(common->second.units, units);
				AppendPlotExposure(common->second.cities, cities);
				continue;
			}
		}
		board.getUnitsAt(pPlot, occupants);
		const int iRisk = turn.aiMapped[STacticalFlavors::RISK];
		if (pShared && !pEnrolled && !pShared->HasEnrolled(turn.ePlayer, occupants))
		{
			SOperationalExposure& common = pShared->plots[iPlot];
			if (pPlot->isCity())
				CvTacticalOperationalLoop::AllocatePlotExposure(turn.ePlayer, *pPlot, occupants, iRisk,
					units, cities, pEndangered, bAllVictims, NULL, &common);
			else
			{
				CvTacticalOperationalLoop::AllocatePlotExposure(turn.ePlayer, *pPlot, occupants, iRisk,
					common.units, common.cities, pEndangered, bAllVictims);
				AppendPlotExposure(common.units, units);
				AppendPlotExposure(common.cities, cities);
			}
		}
		else
			CvTacticalOperationalLoop::AllocatePlotExposure(turn.ePlayer, *pPlot, occupants, iRisk,
				units, cities, pEndangered, bAllVictims, pEnrolled);
	}

	// Keep all victim IDs for recruitment and overlap; each attacker can act only so many times.
	for (int iKind = 0; iKind < 2; iKind++)
	{
		OperationalThreatMap& threats = iKind == 0 ? units : cities;
		for (OperationalThreatMap::iterator it = threats.begin(); it != threats.end(); ++it)
			it->second.fExposure = CvTacticalOperationalLoop::CappedExposure(it->second, NULL);
	}
}

// Builds live exposure from occupied plots, then refreshes city approaches and battlefield exemptions.
void CvTacticalOperationalLoop::BuildThreats(CvTacticalAI& kTactical, SOperationalTurn& turn)
{
	// Setup and every completed command rebuild threats before any more exposure or city danger reads.
	turn.bSharedExposureBuilt = false;
	turn.sharedExposure.units.clear();
	turn.sharedExposure.cities.clear();
	turn.cityDanger.clear();
	turn.unitThreats.clear();
	turn.cityThreats.clear();
	turn.withdrawUnitThreats.clear();
	turn.withdrawCityThreats.clear();
	turn.cityApproach.clear();
	turn.endangeredUnits.clear();
	CvPlayer& kPlayer = *kTactical.m_pPlayer;
	// Fight includes spent members, while Withdraw still uses the independent allocation.
	turn.preparedArmies.clear();
	set<int> enrolled;
	for (size_t i = 0; i < kTactical.m_PreparedArmies.size(); i++)
	{
		const SOperationalPreparedArmy& order = kTactical.m_PreparedArmies[i];
		CvArmyAI* pArmy = kPlayer.getArmyAI(order.iArmy);
		CvAIOperation* pOperation = kPlayer.getAIOperation(order.iOperation);
		if (!pArmy || !pOperation)
			continue;
		turn.preparedArmies.insert(order.iArmy);
		for (CvUnit* pUnit = pArmy->GetFirstUnit(); pUnit; pUnit = pArmy->GetNextUnit(pUnit))
			if (pUnit->getDomainType() != DOMAIN_AIR && !pUnit->isDelayedDeath() && !pUnit->IsDead())
				enrolled.insert(pUnit->GetID());
	}
	if (turn.preparedArmies.empty())
		BuildOccupiedExposure(turn, turn.unitThreats, turn.cityThreats, &turn.endangeredUnits, false);
	else
	{
		SOperationalSharedExposurePlots shared(enrolled);
		BuildOccupiedExposure(turn, turn.withdrawUnitThreats, turn.withdrawCityThreats, &turn.endangeredUnits, false, NULL, &shared);
		BuildOccupiedExposure(turn, turn.unitThreats, turn.cityThreats, &turn.endangeredUnits, false, &enrolled, &shared);
	}
	int iLoop = 0;
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
		UpdateBattlefieldThreat(turn, turn.battlefields[i]);
		UpdateFightArmies(kTactical, turn, turn.battlefields[i]);
	}
}

// Applies the current threat allocations to one battlefield, including its city garrison.
void CvTacticalOperationalLoop::UpdateBattlefieldThreat(const SOperationalTurn& turn, SOperationalBattlefield& field)
{
	if (field.eCenter != OPERATIONAL_CENTER_ENEMY_UNIT && field.eCenter != OPERATIONAL_CENTER_ENEMY_CITY)
		return;

	const pair<int, int> key(static_cast<int>(field.eOwner), field.iTargetID);
	if (field.eCenter == OPERATIONAL_CENTER_ENEMY_UNIT)
	{
		field.bCanAttackOurUnits = turn.unitThreats.find(key) != turn.unitThreats.end();
		return;
	}
	field.bCanAttackOurUnits = turn.cityThreats.find(key) != turn.cityThreats.end();
	CvCity* pCity = field.eOwner != NO_PLAYER ? GET_PLAYER(field.eOwner).getCity(field.iTargetID) : NULL;
	CvUnit* pGarrison = pCity ? pCity->GetGarrisonedUnit() : NULL;
	if (pGarrison && turn.unitThreats.find(make_pair(static_cast<int>(pGarrison->getOwner()), pGarrison->GetID())) != turn.unitThreats.end())
		field.bCanAttackOurUnits = true;
}

// Checks whether this attack has attributed damage against any formation member.
static bool ThreatensArmy(const OperationalThreatMap& threats, const pair<int, int>& key,
	const CvArmyAI& army)
{
	const OperationalThreatMap::const_iterator it = threats.find(key);
	if (it == threats.end())
		return false;
	for (size_t i = 0; i < it->second.vUnitIDs.size(); i++)
	{
		const CvUnit* pUnit = GET_PLAYER(army.GetOwner()).getUnit(it->second.vUnitIDs[i]);
		if (pUnit && pUnit->getArmyID() == army.GetID())
			return true;
	}
	return false;
}

// Gets the city of the target plot's dominance zone from the current tactical map.
static CvCity* OperationTargetZoneCity(CvTacticalAI& kTactical, const CvAIOperation& operation)
{
	const CvPlot* pTarget = operation.GetTargetPlot();
	if (!pTarget)
		return NULL;

	const CvTacticalAnalysisMap* pMap = kTactical.GetTacticalAnalysisMap();
	const CvTacticalDominanceZone* pZone = pMap->GetZoneByIDWithoutRefresh(
		pMap->GetDominanceZoneIDWithoutRefresh(pTarget->GetPlotIndex()));
	return pZone ? pZone->GetZoneCity() : NULL;
}

// Gives each formation one eligibility decision per Fight and board.
void CvTacticalOperationalLoop::UpdateFightArmies(CvTacticalAI& kTactical, const SOperationalTurn& turn, SOperationalBattlefield& field)
{
	field.fightArmies.clear();
	if (field.eCenter == OPERATIONAL_CENTER_ARMY || field.eCenter == OPERATIONAL_CENTER_CAMP || turn.preparedArmies.empty())
		return;
	const pair<int, int> key(field.eOwner, field.iTargetID);
	CvPlot* pPlot = GC.getMap().plotByIndex(field.iPlotIndex);
	if (!pPlot)
		return;
	const CvCity* pCity = field.eCenter == OPERATIONAL_CENTER_ENEMY_UNIT ? NULL : pPlot->getPlotCity();
	const CvUnit* pGarrison = pCity && pCity->getOwner() == field.eOwner &&
		field.eCenter == OPERATIONAL_CENTER_ENEMY_CITY ? pCity->GetGarrisonedUnit() : NULL;
	const CvUnit* pEnemy = field.eCenter == OPERATIONAL_CENTER_ENEMY_UNIT ? GET_PLAYER(field.eOwner).getUnit(field.iTargetID) : NULL;
	const TeamTypes eTeam = kTactical.m_pPlayer->getTeam();
	const bool bVisibleEnemyAtPlot = pEnemy && pEnemy->plot() == pPlot && !pEnemy->isDelayedDeath() &&
		pPlot->isVisible(eTeam) && !pEnemy->isInvisible(eTeam, false);
	const vector<SOperationalThreatDamage>* pCityAttackers = pCity && field.eCenter == OPERATIONAL_CENTER_OUR_CITY ?
		&CachedCityDanger(turn, *pCity).threats : NULL;

	for (size_t i = 0; i < kTactical.m_PreparedArmies.size(); i++)
	{
		const SOperationalPreparedArmy& order = kTactical.m_PreparedArmies[i];
		CvArmyAI* pArmy = kTactical.m_pPlayer->getArmyAI(order.iArmy);
		CvAIOperation* pOperation = kTactical.m_pPlayer->getAIOperation(order.iOperation);
		if (!pArmy || !pOperation)
			continue;
		bool bFight = false;
		if (field.eCenter == OPERATIONAL_CENTER_OUR_CITY)
		{
			bFight = pCity && order.pObjective == pPlot;
			for (size_t j = 0; pCityAttackers && j < pCityAttackers->size() && !bFight; j++)
			{
				const SOperationalThreatDamage& hit = (*pCityAttackers)[j];
				bFight = hit.TotalDamage() > 0 && ThreatensArmy(hit.bCity ? turn.cityThreats : turn.unitThreats,
					make_pair(hit.eOwner, hit.iID), *pArmy);
			}
		}
		else if (atWar(eTeam, GET_PLAYER(field.eOwner).getTeam()))
		{
			if (field.eCenter == OPERATIONAL_CENTER_ENEMY_UNIT)
			{
				// Rapid response targets a visible enemy cluster by plot.
				bFight = (bVisibleEnemyAtPlot && order.pObjective == pPlot && field.eOwner == pOperation->GetEnemy() &&
					pOperation->GetOperationType() == AI_OPERATION_RAPID_RESPONSE) || ThreatensArmy(turn.unitThreats, key, *pArmy);
			}
			else
			{
				const AIOperationTypes eType = pOperation->GetOperationType();
				const bool bCityAttack = eType == AI_OPERATION_CITY_ATTACK_LAND ||
					eType == AI_OPERATION_CITY_ATTACK_NAVAL || eType == AI_OPERATION_CITY_ATTACK_COMBINED;
				bFight = (bCityAttack && pCity && pCity->getOwner() == pOperation->GetEnemy() &&
					pCity == OperationTargetZoneCity(kTactical, *pOperation)) ||
					ThreatensArmy(turn.cityThreats, key, *pArmy) ||
					(pGarrison && ThreatensArmy(turn.unitThreats, make_pair(pGarrison->getOwner(), pGarrison->GetID()), *pArmy));
			}
		}
		if (bFight)
			field.fightArmies.insert(order.iArmy);
	}
}

// Uses the same formation eligibility for each recruit path.
static bool IsFightArmyMember(const CvUnit& unit, const SOperationalBattlefield& field)
{
	return unit.getArmyID() != -1 && unit.getDomainType() != DOMAIN_AIR && field.fightArmies.count(unit.getArmyID()) > 0;
}

// Supplies the checked formation candidates alongside the ordinary tactical roster.
static void FightArmyRecruits(CvPlayer& player, const SOperationalBattlefield& field, vector<int>& recruits)
{
	for (set<int>::const_iterator it = field.fightArmies.begin(); it != field.fightArmies.end(); ++it)
	{
		CvArmyAI* pArmy = player.getArmyAI(*it);
		if (!pArmy)
			continue;
		for (CvUnit* pUnit = pArmy->GetFirstUnit(); pUnit; pUnit = pArmy->GetNextUnit(pUnit))
			if (IsFightArmyMember(*pUnit, field) && pUnit->plot() && pUnit->canUseForTacticalAI() &&
				std::find(recruits.begin(), recruits.end(), pUnit->GetID()) == recruits.end())
				recruits.push_back(pUnit->GetID());
	}
}

// Uses the same army eligibility for the live strike check and execution.
bool CvTacticalOperationalLoop::FindFightRecruits(CvTacticalAI& kTactical, const SOperationalTurn& turn,
	const SOperationalBattlefield& field, CvPlot* pPlot, vector<int>& armyRecruits)
{
	FightArmyRecruits(*kTactical.m_pPlayer, field, armyRecruits);
	return kTactical.FindUnitsWithinStrikingDistance(pPlot, turn.bMarchEnabled ? &armyRecruits : NULL);
}

// Records visible hostile units and, for March, cities we can identify.
static void SnapshotEnemies(const SOperationalTurn& turn, SOperationalBoard& board)
{
	board.enemyUnitPlots.clear();
	board.visibleEnemyCityOwners.clear();
	const TeamTypes eTeam = GET_PLAYER(turn.ePlayer).getTeam();
	int iLoop = 0;
	for (int iOther = 0; iOther < MAX_PLAYERS; iOther++)
	{
		CvPlayer& kOther = GET_PLAYER(static_cast<PlayerTypes>(iOther));
		if (iOther == turn.ePlayer || !kOther.isAlive() ||
			CvTacticalOperationalLoop::GroundSide(turn.ePlayer, static_cast<PlayerTypes>(iOther)) != groundHoldsHostile)
			continue;
		for (CvUnit* pUnit = kOther.firstUnit(&iLoop); pUnit != NULL; pUnit = kOther.nextUnit(&iLoop))
			if (!pUnit->isDelayedDeath() && pUnit->plot() && pUnit->plot()->isVisible(eTeam) &&
				(!turn.bMarchEnabled || !pUnit->isInvisible(eTeam, false)))
				board.enemyUnitPlots[make_pair(iOther, pUnit->GetID())] = pUnit->plot()->GetPlotIndex();
		if (!turn.bMarchEnabled || !atWar(eTeam, kOther.getTeam()))
			continue;
		for (const CvCity* pCity = kOther.firstCity(&iLoop); pCity; pCity = kOther.nextCity(&iLoop))
		{
			const CvPlot* pPlot = pCity->plot();
			if (pPlot->isVisible(eTeam) && pPlot->getRevealedOwner(eTeam) == pCity->getOwner())
				board.visibleEnemyCityOwners[pPlot->GetPlotIndex()] = pCity->getOwner();
		}
	}
}

// Checks whether the team knows of a camp on an unowned plot.
static bool IsKnownUnownedCamp(const CvPlot& kPlot, TeamTypes eTeam)
{
	return !kPlot.isOwned() && kPlot.isRevealed(eTeam) &&
		kPlot.getRevealedImprovementType(eTeam) == GD_INT_GET(BARBARIAN_CAMP_IMPROVEMENT);
}

// Builds the opening rows in their original order, then reconciles any newly observed targets.
void CvTacticalOperationalLoop::BuildBattlefields(CvTacticalAI& kTactical, SOperationalTurn& turn)
{
	CvPlayer& kPlayer = *kTactical.m_pPlayer;
	set<pair<int, int> > seenUnits;
	set<pair<int, int> > seenCities;
	for (size_t i = 0; i < kTactical.m_OperationalTargets.size(); i++)
	{
		const pair<CvTacticalTarget, IDInfo>& opening = kTactical.m_OperationalTargets[i];
		const CvTacticalTarget& target = opening.first;
		CvPlot* pPlot = GC.getMap().plot(target.GetTargetX(), target.GetTargetY());
		if (!pPlot)
			continue;

		if (target.GetTargetType() == AI_TACTICAL_TARGET_BARBARIAN_CAMP)
		{
			AddCampBattlefield(kTactical, turn, target, *pPlot);
			continue;
		}
		const CvUnit* pDefender = NULL;
		if (target.GetTargetType() == AI_TACTICAL_TARGET_ENEMY_COMBAT_UNIT)
		{
			// Keep native defender lookup with March off. With discovery on, retain
			// the original defender on this plot and require sight of any replacement.
			if (turn.bMarchEnabled)
			{
				pDefender = GET_PLAYER(opening.second.eOwner).getUnit(opening.second.iID);
				if (!pDefender || pDefender->plot() != pPlot || !pDefender->IsCanDefend())
					continue;
			}
			else
				pDefender = pPlot->getBestDefender(NO_PLAYER, turn.ePlayer, NULL, true, true);
		}
		else if (turn.bMarchEnabled)
		{
			const CvCity* pCity = pPlot->getPlotCity();
			if (!pCity || pCity->getOwner() != opening.second.eOwner)
				continue;
		}
		if (AddEnemyBattlefield(kTactical, turn, target, *pPlot, pDefender, seenUnits, seenCities) &&
			pDefender && pPlot->isVisible(kPlayer.getTeam()) && !pDefender->isInvisible(kPlayer.getTeam(), false) &&
			IsKnownUnownedCamp(*pPlot, kPlayer.getTeam()))
		{
			const double fCampValue = CampValue(turn, *pPlot);
			if (fCampValue > 0.0)
				turn.battlefields.back().fCampValue = fCampValue;
		}
	}

	int iLoop = 0;
	for (CvCity* pCity = kPlayer.firstCity(&iLoop); pCity != NULL; pCity = kPlayer.nextCity(&iLoop))
		AddOwnCityBattlefield(kTactical, turn, *pCity);

	if (turn.bMarchEnabled)
	{
		SOperationalBoard board;
		SnapshotEnemies(turn, board);
		DiscoverEnemyBattlefields(kTactical, turn, board, NULL);
	}
}

// Resolves the stable army identity, even when two armies share a waypoint.
vector<SOperationalPreparedArmy>::iterator CvTacticalOperationalLoop::FindPreparedArmy(CvTacticalAI& kTactical,
	const SOperationalBattlefield& field)
{
	vector<SOperationalPreparedArmy>& armies = kTactical.m_PreparedArmies;
	for (vector<SOperationalPreparedArmy>::iterator it = armies.begin(); it != armies.end(); ++it)
		if (it->eOwner == field.eOwner && it->iOperation == field.iOperation && it->iArmy == field.iTargetID)
			return it;
	return armies.end();
}

// Sets an army row's plot and zone from its prepared waypoint.
static void SetArmyRowWaypoint(CvTacticalAI& kTactical, SOperationalBattlefield& field, const SOperationalPreparedArmy& order)
{
	field.iPlotIndex = order.pWaypoint ? order.pWaypoint->GetPlotIndex() : -1;
	field.iZoneID = field.iPlotIndex < 0 ? 0 :
		kTactical.GetTacticalAnalysisMap()->GetDominanceZoneIDWithoutRefresh(field.iPlotIndex);
}

// Appends new army rows and updates or drops open offers without reusing any row identity.
void CvTacticalOperationalLoop::SyncArmyBattlefields(CvTacticalAI& kTactical, SOperationalTurn& turn)
{
	bool bHasArmyRows = false;
	for (size_t i = 0; i < turn.battlefields.size(); i++)
	{
		SOperationalBattlefield& field = turn.battlefields[i];
		if (field.eCenter != OPERATIONAL_CENTER_ARMY)
			continue;
		bHasArmyRows = true;
		if (OperationalOfferIsClosed(field.eReinforce))
			continue;
		const vector<SOperationalPreparedArmy>::const_iterator order = FindPreparedArmy(kTactical, field);
		if (order == kTactical.m_PreparedArmies.end())
			field.eReinforce = OPERATIONAL_OFFER_DROPPED;
		else
			SetArmyRowWaypoint(kTactical, field, *order);
	}

	// Enrollment is complete before the loop starts.
	if (bHasArmyRows)
		return;
	for (size_t i = 0; i < kTactical.m_PreparedArmies.size(); i++)
	{
		const SOperationalPreparedArmy& army = kTactical.m_PreparedArmies[i];
		SOperationalBattlefield field;
		field.iID = static_cast<int>(turn.battlefields.size());
		field.eCenter = OPERATIONAL_CENTER_ARMY;
		field.eOwner = army.eOwner;
		field.iOperation = army.iOperation;
		field.iTargetID = army.iArmy;
		field.eFight = field.eReinforce = OPERATIONAL_OFFER_INACTIVE;
		SetArmyRowWaypoint(kTactical, field, army);
		turn.battlefields.push_back(field);
	}
}

// Discounts a known unowned camp's bounty by its distance from our nearest city.
double CvTacticalOperationalLoop::CampValue(const SOperationalTurn& turn, const CvPlot& kPlot)
{
	const CvPlayer& kPlayer = GET_PLAYER(turn.ePlayer);
	const int iDistance = std::max(1, kPlayer.GetCityDistanceInPlots(&kPlot));
	return turn.fCampBounty * std::min(1.0, turn.weights.afConstant[OPERATIONAL_CONSTANT_CAMP_NEAR_PLOTS] / iDistance);
}

// Finds the best free roster recruit for a camp that can leave its post and preserves the tactical recruit list and move type.
CvUnit* CvTacticalOperationalLoop::FindCampClaimant(CvTacticalAI& kTactical, CvPlot* pPlot, int& iTurns)
{
	CvPlayer& kPlayer = *kTactical.m_pPlayer;
	const int iRange = kPlayer.IsAtWarAnyMajor() ? 6 : 11;
	const list<int>& roster = kTactical.m_CurrentTurnUnits;

	set<int> allowed;
	for (list<int>::const_iterator it = roster.begin(); it != roster.end(); ++it)
	{
		CvUnit* pCandidate = kPlayer.getUnit(*it);
		if (!pCandidate || pCandidate->getArmyID() != -1 || IsHeldGarrison(pCandidate))
			continue;
		allowed.insert(*it);
	}
	if (allowed.empty())
		return NULL;

	const CTacticalUnitArray previous = kTactical.m_CurrentMoveUnits;
	CvUnit* pUnit = NULL;
	kTactical.FindUnitsForHarassing(pPlot, iRange, -1, -1, DOMAIN_LAND, false, true, 1, false, &allowed);
	if (kTactical.m_CurrentMoveUnits.size() > 0)
	{
		pUnit = kPlayer.getUnit(kTactical.m_CurrentMoveUnits[0].GetID());
		iTurns = kTactical.m_CurrentMoveUnits[0].GetMovesToTarget();
	}
	kTactical.m_CurrentMoveUnits = previous;
	return pUnit;
}

// Adds one plot row and keeps the opening fog or empty classification for the turn.
void CvTacticalOperationalLoop::AddCampBattlefield(CvTacticalAI& kTactical, SOperationalTurn& turn,
	const CvTacticalTarget& target, CvPlot& kPlot)
{
	const TeamTypes eTeam = kTactical.m_pPlayer->getTeam();
	if (!IsKnownUnownedCamp(kPlot, eTeam) || kPlot.getVisibleEnemyDefender(turn.ePlayer))
		return;
	const double fCampValue = CampValue(turn, kPlot);
	if (fCampValue <= 0.0)
		return;
	SOperationalBattlefield field;
	field.iID = static_cast<int>(turn.battlefields.size());
	field.eCenter = OPERATIONAL_CENTER_CAMP;
	field.iPlotIndex = field.iTargetID = kPlot.GetPlotIndex();
	field.iZoneID = target.GetDominanceZone();
	const CvTacticalDominanceZone* pZone = kTactical.GetTacticalAnalysisMap()->GetZoneByIDWithoutRefresh(field.iZoneID);
	const CvCity* pZoneCity = pZone ? pZone->GetZoneCity() : NULL;
	field.iZoneCityPlot = pZoneCity ? pZoneCity->plot()->GetPlotIndex() : -1;
	field.bWithdrawPosture = pZone && pZone->GetPosture() == TACTICAL_POSTURE_WITHDRAW;
	field.fCampValue = fCampValue;
	field.bCampFogged = !kPlot.isVisible(eTeam);
	if (!field.bCampFogged)
	{
		const CvUnit* pUnit = FindCampClaimant(kTactical, &kPlot, field.iCampTurns);
		if (pUnit)
			field.iCampClaimant = pUnit->GetID();
	}
	turn.battlefields.push_back(field);
}

// Appends a classified hostile target once, using the same fields for opening and later rows.
bool CvTacticalOperationalLoop::AddEnemyBattlefield(CvTacticalAI& kTactical, SOperationalTurn& turn, const CvTacticalTarget& target,
	const CvPlot& kPlot, const CvUnit* pDefender, set<pair<int, int> >& seenUnits, set<pair<int, int> >& seenCities)
{
	const PlayerTypes ePlayer = turn.ePlayer;
	SOperationalBattlefield field;
	if (target.GetTargetType() == AI_TACTICAL_TARGET_ENEMY_COMBAT_UNIT)
	{
		if (!pDefender || pDefender->isDelayedDeath() || GroundSide(ePlayer, pDefender->getOwner()) != groundHoldsHostile ||
			!seenUnits.insert(make_pair(static_cast<int>(pDefender->getOwner()), pDefender->GetID())).second)
			return false;
		field.eCenter = OPERATIONAL_CENTER_ENEMY_UNIT;
		field.eOwner = pDefender->getOwner();
		field.iTargetID = pDefender->GetID();
	}
	else
	{
		CvCity* pCity = kPlot.getPlotCity();
		if (!pCity || GroundSide(ePlayer, pCity->getOwner()) != groundHoldsHostile ||
			!seenCities.insert(make_pair(kPlot.GetPlotIndex(), static_cast<int>(pCity->getOwner()))).second)
			return false;
		field.eCenter = OPERATIONAL_CENTER_ENEMY_CITY;
		field.eOwner = pCity->getOwner();
		field.iTargetID = pCity->GetID();
		field.fImportance = turn.fCityScale > 0.0 ? std::min(1.0, CityValue(*pCity, ePlayer) / turn.fCityScale) : 0.0;
	}

	field.iID = static_cast<int>(turn.battlefields.size());
	field.iPlotIndex = kPlot.GetPlotIndex();
	field.iZoneID = target.GetDominanceZone();
	const CvTacticalDominanceZone* pZone = kTactical.GetTacticalAnalysisMap()->GetZoneByIDWithoutRefresh(field.iZoneID);
	// Keep the zone city by plot so unit fights follow it after a capture.
	if (field.eCenter == OPERATIONAL_CENTER_ENEMY_UNIT)
	{
		const CvCity* pZoneCity = pZone ? pZone->GetZoneCity() : NULL;
		field.iZoneCityPlot = pZoneCity ? pZoneCity->plot()->GetPlotIndex() : -1;
	}
	field.fTheater = turn.weights.afConstant[OPERATIONAL_CONSTANT_THEATER_FLOOR];
	field.fGround = pDefender ? TargetGround(ePlayer, kPlot, pDefender) : 0.0;
	field.bNearOurCity = kTactical.m_pPlayer->GetCityDistanceInPlots(&kPlot) < 4;
	// Reinforce marches only into a zone of ours or one neighboring ours, and never into a Withdraw zone.
	field.bBorder = pZone && IsBorderZone(kTactical, *pZone);
	field.bWithdrawPosture = pZone && pZone->GetPosture() == TACTICAL_POSTURE_WITHDRAW;
	if (turn.bMarchEnabled)
	{
		UpdateBattlefieldThreat(turn, field);
		UpdateFightArmies(kTactical, turn, field);
	}
	turn.battlefields.push_back(field);
	return true;
}

// Classifies the observed enemy plots in map order and leaves existing offer identities intact.
void CvTacticalOperationalLoop::DiscoverEnemyBattlefields(CvTacticalAI& kTactical, SOperationalTurn& turn, const SOperationalBoard& board,
	SOperationalChanges* pChanges)
{
	set<pair<int, int> > seenUnits;
	set<pair<int, int> > seenCities;
	for (size_t i = 0; i < turn.battlefields.size(); i++)
	{
		const SOperationalBattlefield& field = turn.battlefields[i];
		if (field.eCenter == OPERATIONAL_CENTER_ENEMY_UNIT)
			seenUnits.insert(make_pair(static_cast<int>(field.eOwner), field.iTargetID));
		else if (field.eCenter == OPERATIONAL_CENTER_ENEMY_CITY)
			seenCities.insert(make_pair(field.iPlotIndex, static_cast<int>(field.eOwner)));
	}

	set<int> candidates;
	for (map<pair<int, int>, int>::const_iterator it = board.enemyUnitPlots.begin(); it != board.enemyUnitPlots.end(); ++it)
		candidates.insert(it->second);
	for (map<int, int>::const_iterator it = board.visibleEnemyCityOwners.begin(); it != board.visibleEnemyCityOwners.end(); ++it)
		candidates.insert(it->first);

	const CvTacticalAnalysisMap* pZoneMap = kTactical.GetTacticalAnalysisMap();
	for (set<int>::const_iterator it = candidates.begin(); it != candidates.end(); ++it)
	{
		const int i = *it;
		const CvPlot* pPlot = GC.getMap().plotByIndexUnchecked(i);
		CvTacticalTarget target;
		if (!kTactical.ClassifyEnemyTarget(*pPlot, target, true))
			continue;
		target.SetDominanceZone(pZoneMap->GetDominanceZoneIDWithoutRefresh(i));
		if (AddEnemyBattlefield(kTactical, turn, target, *pPlot, target.GetUnitPtr(), seenUnits, seenCities) && pChanges)
			pChanges->vEnemyPlots.push_back(i);
	}
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
	UpdateFightArmies(kTactical, turn, field);
	turn.battlefields.push_back(field);
}

// One cache serves the city-first estimate pass and expires when that pass ends.
void CvTacticalOperationalLoop::EstimateOffers(CvTacticalAI& kTactical, SOperationalTurn& turn, const SOperationalChanges* pChanges)
{
	turn.bEstimating = true;
	SOperationalEstimateCache cache;
	for (int iPass = 0; iPass < 2; iPass++)
	{
		for (size_t i = 0; i < turn.battlefields.size(); i++)
		{
			SOperationalBattlefield& field = turn.battlefields[i];
			if (field.eCenter == OPERATIONAL_CENTER_ARMY)
				continue;
			const bool bCity = field.eCenter == OPERATIONAL_CENTER_ENEMY_CITY || field.eCenter == OPERATIONAL_CENTER_OUR_CITY;
			if (bCity != (iPass == 0))
				continue;
			const bool bFightOpen = field.eFight == OPERATIONAL_OFFER_READY || field.eFight == OPERATIONAL_OFFER_INACTIVE ||
				field.eFight == OPERATIONAL_OFFER_WAITING;
			const bool bReinforceOpen = field.eReinforce == OPERATIONAL_OFFER_READY || field.eReinforce == OPERATIONAL_OFFER_INACTIVE;
			// Our own city and a disabled Withdraw carry no offer, so their slot stays inactive.
			const bool bWithdrawOpen = turn.weights.afConstant[OPERATIONAL_CONSTANT_WITHDRAW_ENABLED] != 0.0 &&
				OperationalSupportsSlot(field.eCenter, OPERATIONAL_SLOT_WITHDRAW) &&
				(field.eWithdraw == OPERATIONAL_OFFER_READY || field.eWithdraw == OPERATIONAL_OFFER_INACTIVE);
			if (!bFightOpen && !bReinforceOpen && !bWithdrawOpen && !bCity)
				continue;
			turn.performance.estimates += (bFightOpen ? 1 : 0) + (bReinforceOpen ? 1 : 0) + (bWithdrawOpen ? 1 : 0);
			if (!EstimateBattlefield(kTactical, turn, cache, field, bFightOpen || bReinforceOpen || bWithdrawOpen, bReinforceOpen,
				bWithdrawOpen, pChanges))
			{
				if (bFightOpen)
					field.eFight = OPERATIONAL_OFFER_DROPPED;
				if (bReinforceOpen)
					field.eReinforce = OPERATIONAL_OFFER_DROPPED;
				if (bWithdrawOpen)
					field.eWithdraw = OPERATIONAL_OFFER_DROPPED;
				continue;
			}
			if (bCity)
			{
				cache.cityTheaters[field.iPlotIndex] = field.fTheater;
				cache.theaters[field.iPlotIndex] = field.theater;
			}
			if (bFightOpen && field.eFight != OPERATIONAL_OFFER_WAITING)
				field.eFight = field.fEstimate > 0.0 ? OPERATIONAL_OFFER_READY : OPERATIONAL_OFFER_INACTIVE;
			if (bReinforceOpen)
				field.eReinforce = field.fReinforceEstimate > 0.0 ? OPERATIONAL_OFFER_READY : OPERATIONAL_OFFER_INACTIVE;
			if (bWithdrawOpen)
				field.eWithdraw = field.fWithdrawEstimate > 0.0 ? OPERATIONAL_OFFER_READY : OPERATIONAL_OFFER_INACTIVE;
		}
	}
	// Finish both battlefield passes before estimating ArmyMarch against their live theaters.
	for (size_t i = 0; i < turn.battlefields.size(); i++)
	{
		SOperationalBattlefield& field = turn.battlefields[i];
		if (field.eCenter == OPERATIONAL_CENTER_ARMY && !OperationalOfferIsClosed(field.eReinforce))
			EstimateArmyMarch(kTactical, turn, field);
	}
	turn.bEstimating = false;
}

// Prices progress according to the operation's mission.
static double ArmyMarchGoalWeight(const SOperationalTurn& turn, AIOperationTypes eType)
{
	switch (eType)
	{
	case AI_OPERATION_CITY_ATTACK_LAND:
	case AI_OPERATION_CITY_ATTACK_NAVAL:
	case AI_OPERATION_CITY_ATTACK_COMBINED:
		return turn.weights.afWeight[OPERATIONAL_WEIGHT_OCCUPATION];
	case AI_OPERATION_CITY_DEFENSE:
	case AI_OPERATION_RAPID_RESPONSE:
		return turn.weights.afWeight[OPERATIONAL_WEIGHT_HOLD_CITY];
	case AI_OPERATION_NAVAL_SUPERIORITY:
		return turn.weights.afWeight[OPERATIONAL_WEIGHT_ATTRITION];
	case AI_OPERATION_PILLAGE_ENEMY:
		return FlavorWeight(1.0, 1.0, turn.iMarchGold, false);
	case AI_OPERATION_CARRIER_GROUP:
		return 2.0;
	default:
		return 0.0;
	}
}

// Reprices the objective's theater with the full formation and the intended enemy's defenses.
static void EstimateArmyMarchTheater(CvTacticalAI& kTactical, const SOperationalTurn& turn, CvAIOperation& kOperation,
	const vector<CvUnit*>& members, SOperationalBattlefield& field)
{
	const double fFloor = turn.weights.afConstant[OPERATIONAL_CONSTANT_THEATER_FLOOR];
	CvCity* pCity = OperationTargetZoneCity(kTactical, kOperation);
	field.fTheater = field.armyMarch.fUnassistedTheater = fFloor;
	field.theater = SOperationalTheater();
	if (!pCity)
		return;

	SOperationalLocalSet local;
	CvTacticalOperationalLoop::GatherTargetLocalSet(kTactical, turn.ePlayer, *pCity->plot(), local, false);
	const bool bOurCity = pCity->getOwner() == turn.ePlayer;
	const double fLocal = bOurCity ? CvTacticalOperationalLoop::LocalCityStrength(*pCity, local) : CvTacticalOperationalLoop::LocalStrength(local);
	set<pair<int, int> > countedUnits, countedCities;
	const PlayerTypes eEnemy = kOperation.GetEnemy();
	const PlayerTypes eExtraHostile = !bOurCity && pCity->getOwner() == eEnemy && !GET_PLAYER(turn.ePlayer).IsAtWarWith(eEnemy) ?
		eEnemy : NO_PLAYER;
	const double fEnemy = bOurCity ? CvTacticalOperationalLoop::EnemyCityStrength(turn, *pCity, &countedUnits) :
		CvTacticalOperationalLoop::EnemyCenterStrength(turn.ePlayer, *pCity->plot(), local.bLand, local.bSea,
			countedUnits, countedCities, eExtraHostile);
	double fWithArmy = fLocal;
	for (size_t i = 0; i < members.size(); i++)
		if (std::find(local.vStrength.begin(), local.vStrength.end(), members[i]) == local.vStrength.end())
			fWithArmy += CvTacticalOperationalLoop::UnitStrength(*members[i]);

	field.theater.fImportance = turn.fCityScale > 0.0 ?
		std::min(1.0, CityValue(*pCity, turn.ePlayer) / turn.fCityScale) : 0.0;
	field.theater.fDamageLevel = DamageShare(pCity->getDamage(), pCity->GetMaxHitPoints());
	const double fSteepness = turn.weights.afConstant[OPERATIONAL_CONSTANT_THEATER_STEEPNESS];
	for (int i = 0; i < 2; i++)
	{
		const double fStrength = i == 0 ? fLocal : fWithArmy;
		const double fShare = StrengthShare(fStrength, fEnemy, fSteepness);
		field.theater.fPressure = (bOurCity ? fEnemy : fStrength) <= 0.0 ? 0.0 : bOurCity ? 1.0 - fShare : fShare;
		field.theater.fContest = TheaterContest(field.theater.fPressure, field.theater.fDamageLevel);
		field.fTheater = TheaterMultiplier(fFloor, field.theater.fImportance, field.theater.fContest);
		if (i == 0)
			field.armyMarch.fUnassistedTheater = field.fTheater;
	}
}

// Collects living formation members, including spent units, in native slot order.
static void ArmyMarchMembers(CvArmyAI& kArmy, vector<CvUnit*>& units)
{
	for (CvUnit* pUnit = kArmy.GetFirstUnit(); pUnit; pUnit = kArmy.GetNextUnit(pUnit))
		if (pUnit->getArmyID() == kArmy.GetID() && pUnit->plot() && !pUnit->isDelayedDeath() && !pUnit->IsDead() &&
			pUnit->getDomainType() != DOMAIN_AIR)
			units.push_back(pUnit);
}

// Resolves the live operation and the members needed to score its prepared order.
eOperationalCheckResult CvTacticalOperationalLoop::ResolveArmyMarch(CvTacticalAI& kTactical, const SOperationalBattlefield& field,
	vector<SOperationalPreparedArmy>::iterator& order, CvAIOperation*& pOperation, CvArmyAI*& pArmy, vector<CvUnit*>* pMembers)
{
	order = FindPreparedArmy(kTactical, field);
	if (order == kTactical.m_PreparedArmies.end())
		return OPERATIONAL_CHECK_TARGET_GONE;

	CvPlayer& kPlayer = GET_PLAYER(order->eOwner);
	pOperation = kPlayer.getAIOperation(order->iOperation);
	pArmy = kPlayer.getArmyAI(order->iArmy);
	if (!pOperation || !pArmy || pArmy->GetOperationID() != order->iOperation ||
		!MatchesPreparedArmyOrder(*pOperation, *pArmy, *order) || !order->pWaypoint || order->pWaypoint->GetPlotIndex() != field.iPlotIndex)
		return OPERATIONAL_CHECK_NO_REACHABLE;
	if (pMembers)
		ArmyMarchMembers(*pArmy, *pMembers);
	return OPERATIONAL_CHECK_PASSED;
}

// Keeps naval and combined formations out of ocean if any member cannot cross it.
static int ArmyMarchRouteFlags(CvArmyAI& kArmy)
{
	return (kArmy.GetType() == ARMY_TYPE_NAVAL || kArmy.GetType() == ARMY_TYPE_COMBINED) && !kArmy.IsAllOceanGoing()
		? CvUnit::MOVEFLAG_NO_OCEAN : 0;
}

// Sets native safety and domain flags for an exact March stop.
static int ExactMarchFlags(const CvUnit& kUnit, const CvPlot& kTarget)
{
	int iFlags = CvUnit::MOVEFLAG_NO_STOPNODES;
	if (kUnit.isNativeDomain(&kTarget)) //don't embark if we don't have to
		iFlags |= CvUnit::MOVEFLAG_APPROX_TARGET_NATIVE_DOMAIN;
	if (kUnit.IsCivilianUnit())
		iFlags |= (CvUnit::MOVEFLAG_DONT_STACK_WITH_NEUTRAL | CvUnit::MOVEFLAG_AI_ABORT_IN_DANGER);
	//embark only where no turn ends at sea in danger
	if (kUnit.getDomainType() == DOMAIN_LAND)
		iFlags |= CvUnit::MOVEFLAG_SAFE_EMBARK_ONLY;
	return iFlags;
}

// Returns the index of the member nearest the native rounded center.
static size_t ArmyMarchCenter(const vector<const CvPlot*>& positions, const CvPlot* pArmyGoal)
{
	CvMap& kMap = GC.getMap();
	const int iX = positions.front()->getX(), iY = positions.front()->getY();
	int iSumX = 0, iSumY = 0;
	for (size_t i = 1; i < positions.size(); i++)
	{
		int iDX = positions[i]->getX() - iX, iDY = positions[i]->getY() - iY;
		if (kMap.isWrapX() && abs(iDX) > kMap.getGridWidth() / 2)
			iDX += iDX > 0 ? -kMap.getGridWidth() : kMap.getGridWidth();
		if (kMap.isWrapY() && abs(iDY) > kMap.getGridHeight() / 2)
			iDY += iDY > 0 ? -kMap.getGridHeight() : kMap.getGridHeight();
		iSumX += iDX;
		iSumY += iDY;
	}
	const float fX = iX + iSumX / float(positions.size()), fY = iY + iSumY / float(positions.size());
	const CvPlot* pMean = kMap.plot(static_cast<int>(fX + (fX > 0 ? 0.5f : -0.5f)), static_cast<int>(fY + (fY > 0 ? 0.5f : -0.5f)));
	size_t iCenter = 0;
	int iBest = INT_MAX;
	for (size_t i = 0; pMean && i < positions.size(); i++)
	{
		const int iScore = plotDistance(*positions[i], *pMean) * 100 + (pArmyGoal ? plotDistance(*positions[i], *pArmyGoal) : 0);
		if (iScore < iBest)
		{
			iBest = iScore;
			iCenter = i;
		}
	}
	return iCenter;
}

// A projected stop cannot split the formation or make an existing outlier worse.
struct SOperationalFormation
{
	vector<CvUnit*> members;
	vector<const CvPlot*> plots;
	const CvPlot* pGoal;
	size_t iCenterMember;
	int iTolerance;
	bool bNeedsRefresh;

	// Includes members who cannot move or were omitted from admission.
	SOperationalFormation(CvArmyAI& army, const CvAIOperation& operation, const map<int, const CvPlot*>* pStops = NULL)
		: pGoal(NULL), iCenterMember(0), iTolerance(0), bNeedsRefresh(false)
	{
		Refresh(army, operation, pStops);
	}

	// Reuses the center for unchanged positions. A committed action can still change nearby occupancy.
	void Refresh(CvArmyAI& army, const CvAIOperation& operation, const map<int, const CvPlot*>* pStops = NULL)
	{
		vector<CvUnit*> liveMembers;
		ArmyMarchMembers(army, liveMembers);
		const CvPlot* pLiveGoal = army.GetGoalPlot();
		bool bPositionsChanged = liveMembers != members || pLiveGoal != pGoal;
		members.swap(liveMembers);
		pGoal = pLiveGoal;
		plots.resize(members.size(), NULL);
		for (size_t i = 0; i < members.size(); i++)
		{
			const CvPlot* pPlot = members[i]->plot();
			if (pStops)
			{
				const map<int, const CvPlot*>::const_iterator it = pStops->find(members[i]->GetID());
				if (it != pStops->end())
					pPlot = it->second;
			}
			if (plots[i] != pPlot)
			{
				plots[i] = pPlot;
				bPositionsChanged = true;
			}
		}
		if (bPositionsChanged && !plots.empty())
			iCenterMember = ArmyMarchCenter(plots, pGoal);
		iTolerance = plots.empty() ? 0 : operation.GetGatherTolerance(&army, GC.getMap().plotByIndex(plots[iCenterMember]->GetPlotIndex()));
		bNeedsRefresh = false;
	}

	// Uses the same center member before and after the stop so a center change cannot create an outlier.
	bool Allows(const CvUnit& unit, const CvPlot& stop) const
	{
		const vector<CvUnit*>::const_iterator it = std::find(members.begin(), members.end(), &unit);
		if (it == members.end())
			return false;
		const size_t iUnit = it - members.begin();
		if (plots[iUnit] == &stop)
			return true;
		const CvPlot& center = *plots[iCenterMember];
		const CvPlot& nextCenter = iUnit == iCenterMember ? stop : center;
		for (size_t i = 0; i < plots.size(); i++)
			if (plotDistance(i == iUnit ? stop : *plots[i], nextCenter) > std::max(iTolerance, plotDistance(*plots[i], center)))
				return false;
		return true;
	}
};

// Keeps an embarked member of a combined army on water at a March stop, or lets it hold.
static bool AllowsArmyMarchStop(const CvUnit& unit, const CvPlot& stop, const CvArmyAI& army)
{
	return unit.plot() == &stop || army.GetType() != ARMY_TYPE_COMBINED || !unit.isEmbarked() || stop.isWater();
}

// Finds the last stop before the formation would split.
static CvPlot* FormationPathStop(const CvUnit& unit, const CvPathNodeArray& path, CvPlot* pStop,
	const SOperationalFormation& formation)
{
	if (!pStop || pStop == unit.plot())
		return pStop;
	CvPlot* pBest = NULL;
	for (size_t i = 0; i < path.size(); i++)
	{
		CvPlot* pPlot = path.GetPlotByIndex(static_cast<int>(i));
		if (!formation.Allows(unit, *pPlot))
			break;
		if (pPlot != unit.plot() && (pPlot == pStop || unit.canEndTurnAtPlot(pPlot)))
			pBest = pPlot;
		if (pPlot == pStop)
			break;
	}
	return pBest;
}

// Returns the prepared corridor tile nearest to the unit's plot.
static size_t ArmyMarchNearestCorridor(const CvPlot& start, const vector<CvPlot*>& corridor)
{
	int iNearest = INT_MAX;
	size_t iNearestIndex = 0;
	for (size_t i = 0; i < corridor.size(); i++)
	{
		const int iDistance = plotDistance(start, *corridor[i]);
		if (iDistance < iNearest)
		{
			if (iDistance == 0)
				return i;
			iNearest = iDistance;
			iNearestIndex = i;
		}
	}
	return iNearestIndex;
}

// Remembers visible enemy identities without relying on a danger estimate or predicted restart.
static void ObserveMarchContacts(const SOperationalTurn& turn, map<OperationalExposureKey, int>* pNewContacts)
{
	const CvPlayer& player = GET_PLAYER(turn.ePlayer);
	const TeamTypes eTeam = player.getTeam();
	for (int i = 0; i < MAX_PLAYERS; i++)
	{
		CvPlayer& enemy = GET_PLAYER(static_cast<PlayerTypes>(i));
		if (!enemy.isAlive() || !atWar(eTeam, enemy.getTeam()))
			continue;
		int iLoop = 0;
		for (const CvUnit* pUnit = enemy.firstUnit(&iLoop); pUnit; pUnit = enemy.nextUnit(&iLoop))
		{
			const CvPlot* pPlot = pUnit->plot();
			const OperationalExposureKey key(false, make_pair(i, pUnit->GetID()));
			if (pUnit->isDelayedDeath() || pUnit->IsDead() || !pPlot)
				continue;
			if (!pNewContacts && player.IsVanishedUnit(pUnit->GetIDInfo()))
				turn.marchContacts.insert(key);
			if (pPlot->isVisible(eTeam) && !pUnit->isInvisible(eTeam, false) &&
				turn.marchContacts.insert(key).second && pNewContacts)
				(*pNewContacts)[key] = pPlot->GetPlotIndex();
		}
		for (const CvCity* pCity = enemy.firstCity(&iLoop); pCity; pCity = enemy.nextCity(&iLoop))
		{
			const OperationalExposureKey key(true, make_pair(i, pCity->GetID()));
			if (pCity->plot()->isVisible(eTeam) && turn.marchContacts.insert(key).second && pNewContacts)
				(*pNewContacts)[key] = pCity->plot()->GetPlotIndex();
		}
	}
}

// Finds a unit's order in the checked Withdraw roster.
static const SOperationalMarch* FindWithdrawOrder(const vector<SOperationalMarch>& marches, int iUnit)
{
	const SOperationalMarch key = {iUnit, -1, -1, 0};
	const vector<SOperationalMarch>::const_iterator it = std::lower_bound(marches.begin(), marches.end(), key);
	return it != marches.end() && it->iUnitID == iUnit ? &*it : NULL;
}

// Gets the exact stop for a unit in a checked Withdraw.
static CvPlot* FindWithdrawStop(const vector<SOperationalMarch>& marches, int iUnit)
{
	const SOperationalMarch* pOrder = FindWithdrawOrder(marches, iUnit);
	return pOrder ? GC.getMap().plotByIndex(pOrder->iStop) : NULL;
}

// Locates a participant's enrolled army.
static CvArmyAI* PreparedArmyForUnit(const SOperationalTurn& turn, const CvUnit& unit)
{
	return turn.preparedArmies.count(unit.getArmyID()) > 0 ? GET_PLAYER(turn.ePlayer).getArmyAI(unit.getArmyID()) : NULL;
}

// Keeps units on a usable sea route when the formation cannot cross ocean.
static bool AllowsArmyOcean(const CvPlot& plot, CvArmyAI& army)
{
	return !plot.isDeepWater() || !(ArmyMarchRouteFlags(army) & CvUnit::MOVEFLAG_NO_OCEAN);
}

// Replays a checked Withdraw while the live formation still permits its path.
struct SOperationalWithdrawGate : STacticalMovementGate
{
	const SOperationalTurn& turn;
	const vector<SOperationalMarch>& withdrawStops;
	const set<int>& admitted;
	typedef map<int, SOperationalFormation> ArmyFormations;
	mutable ArmyFormations formations;

	// Borrows the checked orders and the admitted roster.
	SOperationalWithdrawGate(const SOperationalTurn& kTurn, const vector<SOperationalMarch>& stops, const set<int>& units)
		: STacticalMovementGate(true), turn(kTurn), withdrawStops(stops), admitted(units) {}

	// Replays the checked path flags.
	virtual int MoveFlags(const CvUnit& unit) const
	{
		const SOperationalMarch* pOrder = FindWithdrawOrder(withdrawStops, unit.GetID());
		return pOrder ? pOrder->iMoveFlags : 0;
	}

private:
	// Shares the live formation across route checks until another action commits.
	const SOperationalFormation* FindFormation(CvArmyAI& army) const
	{
		const CvAIOperation* pOperation = GET_PLAYER(turn.ePlayer).getAIOperation(army.GetOperationID());
		if (!pOperation)
			return NULL;
		ArmyFormations::iterator it = formations.find(army.GetID());
		if (it == formations.end())
			it = formations.insert(make_pair(army.GetID(), SOperationalFormation(army, *pOperation))).first;
		else if (it->second.bNeedsRefresh)
			it->second.Refresh(army, *pOperation);
		return &it->second;
	}

public:
	// Gives each search attempt the checked stops that are still allowed.
	virtual void BuildStops(const vector<CvUnit*>& units, TacticalUnitStops& stops) const
	{
		for (size_t i = 0; i < units.size(); i++)
		{
			const CvUnit& unit = *units[i];
			set<int>& permitted = stops[unit.GetID()];
			permitted.insert(unit.plot()->GetPlotIndex());
			const CvPlot* pStop = FindWithdrawStop(withdrawStops, unit.GetID());
			if (pStop && Allows(unit, *pStop, true))
				permitted.insert(pStop->GetPlotIndex());
		}
	}

	// Validates an order after previous actions have committed.
	virtual bool Allows(const CvUnit& unit, const CvPlot& stop, bool bEndpoint) const
	{
		if (admitted.count(unit.GetID()) == 0)
			return false;
		if (unit.plot() == &stop)
			return true;
		if (bEndpoint && FindWithdrawStop(withdrawStops, unit.GetID()) != &stop)
			return false;
		CvArmyAI* pArmy = PreparedArmyForUnit(turn, unit);
		if (!pArmy)
			return true;
		if (!AllowsArmyOcean(stop, *pArmy))
			return false;
		const SOperationalFormation* pFormation = FindFormation(*pArmy);
		return pFormation && pFormation->Allows(unit, stop);
	}

	// Rechecks the formation after an action changes the board.
	virtual bool AfterAction()
	{
		for (ArmyFormations::iterator it = formations.begin(); it != formations.end(); ++it)
			it->second.bNeedsRefresh = true;
		return false;
	}
};

// Applies the army's route limits and yields Gather on a newly revealed threat.
struct SOperationalMarchGate : STacticalMovementGate
{
	const SOperationalTurn& turn;
	SOperationalCommandRecord& command;
	const CvPlot& waypoint;
	const vector<CvPlot*>* pMarchCorridor;
	const set<int>& admitted;
	vector<int> selectedRoute;
	// A reveal may become relevant when a later member takes a route this command.
	map<OperationalExposureKey, int> newContacts;

	// Borrows the order and remembers contacts already known when March starts.
	SOperationalMarchGate(const SOperationalTurn& kTurn, SOperationalCommandRecord& record, const CvPlot& target,
		const vector<CvPlot*>* pCorridor, const set<int>& units)
		: STacticalMovementGate(false), turn(kTurn), command(record), waypoint(target), pMarchCorridor(pCorridor), admitted(units)
	{
		for (size_t i = 0; i < turn.battlefields.size(); i++)
		{
			const SOperationalBattlefield& field = turn.battlefields[i];
			if (field.eCenter == OPERATIONAL_CENTER_ENEMY_CITY || field.eCenter == OPERATIONAL_CENTER_ENEMY_UNIT)
				turn.marchContacts.insert(OperationalExposureKey(field.eCenter == OPERATIONAL_CENTER_ENEMY_CITY,
					make_pair(field.eOwner, field.iTargetID)));
		}
		ObserveMarchContacts(turn, NULL);
	}

	// Applies the army's ocean limit to the entire route.
	virtual int MoveFlags(const CvUnit& unit) const
	{
		CvArmyAI* pArmy = PreparedArmyForUnit(turn, unit);
		return pArmy ? ArmyMarchRouteFlags(*pArmy) : 0;
	}

	// Uses the same ocean limit when Gather computes reachable plots.
	virtual int SearchMoveFlags(const CvUnit& unit) const
	{
		return MoveFlags(unit);
	}

	// Checks an endpoint chosen by Gather against the live movement rules.
	virtual bool AllowsSearchStop(int iUnit, int iPlot) const
	{
		const CvUnit* pUnit = GET_PLAYER(turn.ePlayer).getUnit(iUnit);
		const CvPlot* pPlot = GC.getMap().plotByIndex(iPlot);
		return pUnit && pPlot && Allows(*pUnit, *pPlot);
	}

	// Keeps recently embarked members of a combined army on water at the stop.
	virtual bool Allows(const CvUnit& unit, const CvPlot& stop, bool bEndpoint = true) const
	{
		if (admitted.count(unit.GetID()) == 0)
			return false;
		if (unit.plot() == &stop)
			return true;
		CvArmyAI* pArmy = PreparedArmyForUnit(turn, unit);
		return !pArmy || (AllowsArmyOcean(stop, *pArmy) && (!bEndpoint || AllowsArmyMarchStop(unit, stop, *pArmy)));
	}

	// Reports the contact that already returned control to the queue.
	virtual bool HasYielded() const
	{
		return command.iYieldPlot >= 0;
	}

	// Checks for newly seen attackers or blockers after the action.
	virtual bool AfterAction()
	{
		if (HasYielded())
			return true;
		ObserveMarchContacts(turn, &newContacts);
		if (newContacts.empty())
		{
			selectedRoute.clear();
			return false;
		}

		// Consume the action's route once. Later reveals cannot block a path we already took.
		set<int> steps(selectedRoute.begin(), selectedRoute.end());
		selectedRoute.clear();

		CvPlayer& player = GET_PLAYER(turn.ePlayer);
		CvArmyAI* pArmy = player.getArmyAI(command.iArmy);
		if (!pArmy)
			return false;
		const TeamTypes eTeam = player.getTeam();
		// Check the action's route and the next corridor step toward this turn's waypoint.
		if (pMarchCorridor)
		{
			const vector<CvPlot*>& corridor = *pMarchCorridor;
			const vector<CvPlot*>::const_iterator itWaypoint = std::find(corridor.begin(), corridor.end(), &waypoint);
			if (itWaypoint != corridor.end())
			{
				const size_t iWaypoint = itWaypoint - corridor.begin();
				for (CvUnit* pUnit = pArmy->GetFirstUnit(); pUnit; pUnit = pArmy->GetNextUnit(pUnit))
					if (pUnit->plot() && pUnit->canUseNow())
					{
						const size_t iStart = ArmyMarchNearestCorridor(*pUnit->plot(), corridor);
						if (iStart != iWaypoint)
							steps.insert(corridor[iStart < iWaypoint ? iStart + 1 : iStart - 1]->GetPlotIndex());
					}
			}
		}

		set<const CvPlot*> plots;
		set<const CvUnit*> attackers;
		for (CvUnit* pUnit = pArmy->GetFirstUnit(); pUnit; pUnit = pArmy->GetNextUnit(pUnit))
		{
			const CvPlot* pPlot = pUnit->plot();
			if (pUnit->getArmyID() != pArmy->GetID() || !pPlot || pUnit->isDelayedDeath() || pUnit->IsDead() || !plots.insert(pPlot).second)
				continue;
			const vector<CvUnit*> possible = player.GetPossibleAttackers(*pPlot, player.getTeam());
			attackers.insert(possible.begin(), possible.end());
		}
		for (map<OperationalExposureKey, int>::const_iterator it = newContacts.begin(); it != newContacts.end(); ++it)
		{
			const OperationalExposureKey& key = it->first;
			CvPlayer& enemy = GET_PLAYER(static_cast<PlayerTypes>(key.second.first));
			const CvUnit* pEnemy = key.first ? NULL : enemy.getUnit(key.second.second);
			const CvCity* pCity = key.first ? enemy.getCity(key.second.second) : NULL;
			const CvPlot* pContact = pCity ? pCity->plot() :
				pEnemy && !pEnemy->IsDead() && !pEnemy->isDelayedDeath() ? pEnemy->plot() : NULL;
			if (!pContact || !pContact->isVisible(eTeam) || (pEnemy && pEnemy->isInvisible(eTeam, false)))
				continue;
			const int iContact = pContact->GetPlotIndex();
			bool bStop = (pEnemy && attackers.count(pEnemy) > 0) ||
				(steps.count(iContact) > 0 && (pCity || (pEnemy && pEnemy->IsCanDefend() && pEnemy->getDomainType() != DOMAIN_AIR)));
			for (set<const CvPlot*>::const_iterator plot = plots.begin(); pCity && plot != plots.end() && !bStop; ++plot)
				bStop = pCity->canRangeStrikeAt((*plot)->getX(), (*plot)->getY());
			if (bStop)
			{
				command.iYieldPlot = iContact;
				return true;
			}
		}
		return false;
	}

private:
	// Gives the path validator storage for the current member's first turn.
	virtual vector<int>* RouteForContactCheck()
	{
		return &selectedRoute;
	}
};

// Checks that multiple incoming units fit the same stop under the native stacking rules.
static bool FitsMarchStop(const CvUnit& unit, const CvPlot* pStop, const vector<SPlacedUnit>& incoming)
{
	if (unit.IsStackingUnit())
		return true;
	int iUsedSlots = unit.CountStackingUnitsAtPlot(pStop);
	for (size_t i = 0; i < incoming.size(); i++)
		if (unit.IsStackingConflict(incoming[i].pUnit, pStop))
			++iUsedSlots;
	return iUsedSlots < pStop->getUnitLimit();
}

// Route combat units first, then siege, then civilians, preserving order within each group.
struct PrOperationalSortCombatFirst
{
	// Compares a pair in the positioning routine's movement order.
	bool operator()(const CvUnit* lhs, const CvUnit* rhs) const
	{
		return (lhs->IsCivilianUnit() ? 2 : lhs->AI_getUnitAIType() == UNITAI_CITY_BOMBARD ? 1 : 0) <
			(rhs->IsCivilianUnit() ? 2 : rhs->AI_getUnitAIType() == UNITAI_CITY_BOMBARD ? 1 : 0);
	}
};

// Estimates progress via the waypoint and collects the members that can still act.
static double ArmyMarchReadinessGain(const SOperationalPreparedArmy& order, const CvAIOperation& operation, CvArmyAI& army,
	const vector<CvUnit*>& members, vector<int>& selected)
{
	const vector<CvPlot*>& corridor = order.vMarchCorridor;
	const vector<CvPlot*>::const_iterator waypoint = std::find(corridor.begin(), corridor.end(), order.pWaypoint);
	if (waypoint == corridor.end() || members.empty())
		return 0.0;
	const size_t iWaypoint = waypoint - corridor.begin();
	const int iTail = static_cast<int>(corridor.size() - 1 - iWaypoint);
	int iRadius = 2; // Gather's approximate path target (MOVEFLAG_APPROX_TARGET_RING2).
	if (iTail == 0)
	{
		if (ArmyMarchAtMuster(order.eStage))
			iRadius = operation.GetGatherTolerance(&army, operation.GetMusterPlot());
		else if (!operation.IsNeverEnding())
			iRadius = operation.GetDeployRange();
	}
	double fGain = 0.0;
	for (size_t i = 0; i < members.size(); i++)
	{
		const CvUnit& unit = *members[i];
		const CvPlot* pStart = unit.plot();
		if (!pStart || !unit.canUseNow())
			continue;
		selected.push_back(unit.GetID());
		// Do not charge a member beyond the waypoint for turning back to it.
		int iToWaypoint = std::max(0, plotDistance(*pStart, *order.pWaypoint) - iRadius);
		if (iToWaypoint > 0 && ArmyMarchNearestCorridor(*pStart, corridor) > iWaypoint)
			iToWaypoint = 0;
		const int iSteps = iToWaypoint + iTail;
		const double fBaseMoves = std::max(1, unit.baseMoves(unit.isEmbarked()));
		const double fTurns = iSteps / fBaseMoves;
		const double fAvailable = std::min(1.0, unit.getMoves() / (fBaseMoves * GD_INT_GET(MOVE_DENOMINATOR)));
		fGain += 1.0 / (1.0 + std::max(0.0, fTurns - fAvailable)) - 1.0 / (1.0 + fTurns);
	}
	return fGain / members.size();
}

// Converts estimated progress into the queue's common priority units.
static double ArmyMarchPriority(const SOperationalTurn& turn, const SOperationalBattlefield& field)
{
	const SOperationalArmyMarchScore& score = field.armyMarch;
	return field.fTheater * turn.weights.afConstant[OPERATIONAL_CONSTANT_MARCH_READY_UNITS] *
		score.fGoalWeight * score.fReadinessGain;
}

// Estimates the formation's one-turn progress along the shared corridor, without an exposure deduction.
void CvTacticalOperationalLoop::EstimateArmyMarch(CvTacticalAI& kTactical, SOperationalTurn& turn, SOperationalBattlefield& field)
{
	turn.performance.estimates++;
	field.fReinforceEstimate = 0.0;
	field.fTheater = turn.weights.afConstant[OPERATIONAL_CONSTANT_THEATER_FLOOR];
	field.theater = SOperationalTheater();
	field.eReinforce = OPERATIONAL_OFFER_INACTIVE;
	field.armyMarch = SOperationalArmyMarchScore();
	field.reinforce.vSelected.clear();
	vector<SOperationalPreparedArmy>::iterator order;
	CvAIOperation* pOperation;
	CvArmyAI* pArmy;
	vector<CvUnit*> members;
	if (ResolveArmyMarch(kTactical, field, order, pOperation, pArmy, &members) != OPERATIONAL_CHECK_PASSED)
		return;
	field.armyMarch.fGoalWeight = ArmyMarchGoalWeight(turn, pOperation->GetOperationType());
	if (field.armyMarch.fGoalWeight <= 0.0)
		return;
	field.armyMarch.fReadinessGain = ArmyMarchReadinessGain(*order, *pOperation, *pArmy, members, field.reinforce.vSelected);
	if (field.armyMarch.fReadinessGain <= 0.0)
		return;
	EstimateArmyMarchTheater(kTactical, turn, *pOperation, members, field);
	field.fReinforceEstimate = ArmyMarchPriority(turn, field);
	if (field.fReinforceEstimate > 0.0)
		field.eReinforce = OPERATIONAL_OFFER_READY;
}

// One local set feeds theater, fight stake, Reinforce and Withdraw; closed cities need only theater strength.
bool CvTacticalOperationalLoop::EstimateBattlefield(CvTacticalAI& kTactical, const SOperationalTurn& turn, SOperationalEstimateCache& cache,
	SOperationalBattlefield& field, bool bStake, bool bReinforce, bool bWithdraw, const SOperationalChanges* pChanges)
{
	field.fEstimate = 0.0;
	CvPlot* pPlot = BattlefieldPlot(field);
	if (!pPlot)
		return false;
	const bool bCamp = field.eCenter == OPERATIONAL_CENTER_CAMP;
	SOperationalLocalSet local;
	GatherTargetLocalSet(kTactical, turn.ePlayer, *pPlot, local, bStake && !bCamp, field.eCenter == OPERATIONAL_CENTER_ENEMY_UNIT, turn.bMarchEnabled ? &field : NULL);
	EstimateTheater(turn, cache, *pPlot, local, field);
	if (!bStake)
		return true;

	// The live plot city may have a different owner and city ID after a capture.
	const CvPlot* pZonePlot = field.iZoneCityPlot >= 0 ? GC.getMap().plotByIndex(field.iZoneCityPlot) : NULL;
	const CvCity* pZoneCity = pZonePlot ? pZonePlot->getPlotCity() : NULL;
	if (bCamp)
	{
		if (field.bCampFogged)
		{
			if (!IsKnownUnownedCamp(*pPlot, GET_PLAYER(turn.ePlayer).getTeam()))
				return false;
			field.stake.fStake = field.stake.fRealized = field.fCampValue;
		}
		else
		{
			bool bClaimantMoved = false;
			if (pChanges && field.iCampClaimant >= 0)
				for (size_t i = 0; i < pChanges->vFriendlyMoves.size() && !bClaimantMoved; i++)
					bClaimantMoved = pChanges->vFriendlyMoves[i].iUnitID == field.iCampClaimant;
			if (!EstimateCampClaim(kTactical, *pPlot, field, bClaimantMoved))
				return false;
		}
	}
	else if (!EstimateFight(kTactical, turn, cache, local, pZoneCity, field))
		return false;
	// A waking fight returns to ready before Reinforce selects candidates.
	if (field.eFight == OPERATIONAL_OFFER_WAITING && pChanges && field.fEstimate > 0.0 &&
		WakesFight(turn.ePlayer, turn, field, *pChanges))
		field.eFight = OPERATIONAL_OFFER_READY;
	if (bReinforce && (!bCamp || field.bCampFogged))
	{
		if (pChanges && !field.reinforce.rejected.empty())
			RetryReinforce(*pPlot, kTactical.GetRecruitRange(), *pChanges, field);
		EstimateReinforce(kTactical, turn, local, field);
	}
	if (bWithdraw)
		EstimateWithdraw(kTactical, turn, local, field);
	return true;
}

// Prices the stored claimant, replacing it when spent or when the offer reaches readiness.
bool CvTacticalOperationalLoop::EstimateCampClaim(CvTacticalAI& kTactical, CvPlot& kPlot,
	SOperationalBattlefield& field, bool bCheck)
{
	const CvPlayer& kPlayer = *kTactical.m_pPlayer;
	field.fEstimate = field.fReinforceEstimate = 0.0;
	if (!IsKnownUnownedCamp(kPlot, kPlayer.getTeam()) || kPlot.getVisibleEnemyDefender(kPlayer.GetID()))
		return false;
	if (field.eFight == OPERATIONAL_OFFER_EXECUTED || field.eReinforce == OPERATIONAL_OFFER_EXECUTED)
		return true;

	const CvUnit* pUnit = field.iCampClaimant >= 0 ? kPlayer.getUnit(field.iCampClaimant) : NULL;
	if (bCheck || (field.iCampClaimant >= 0 && (!pUnit || !pUnit->canUseNow())))
	{
		pUnit = FindCampClaimant(kTactical, &kPlot, field.iCampTurns);
		field.iCampClaimant = pUnit ? pUnit->GetID() : -1;
	}
	const double fHaste = kPlayer.IsAtWarAnyMajor() ? 1.0 : 0.5;
	field.stake.fStake = field.fCampValue;
	field.stake.fRealized = pUnit ? field.fCampValue / (1.0 + fHaste * field.iCampTurns) : 0.0;
	field.stake.fPotential = field.stake.fStake - field.stake.fRealized;
	if (field.iCampTurns == 0)
		field.fEstimate = field.fTheater * field.stake.fRealized;
	else
		field.fReinforceEstimate = field.fTheater * field.stake.fRealized;
	return true;
}

// Tallies L, T and the strength share for every battlefield, saving the share for the fight and Withdraw.
// City tallies set theater; units and camps inherit it from their zone city.
void CvTacticalOperationalLoop::EstimateTheater(const SOperationalTurn& turn, const SOperationalEstimateCache& cache, const CvPlot& kPlot,
	const SOperationalLocalSet& local, SOperationalBattlefield& field)
{
	const bool bCamp = field.eCenter == OPERATIONAL_CENTER_CAMP;
	const bool bInheritsTheater = field.eCenter == OPERATIONAL_CENTER_ENEMY_UNIT || bCamp;
	const bool bOurCity = field.eCenter == OPERATIONAL_CENTER_OUR_CITY;
	SOperationalReinforce& reinforce = field.reinforce;
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
		if (bCamp && field.bCampFogged)
			reinforce.fEnemy = std::max(reinforce.fEnemy, turn.fTypicalUnitStrength);
	}
	reinforce.fShare = StrengthShare(reinforce.fLocal, reinforce.fEnemy,
		turn.weights.afConstant[OPERATIONAL_CONSTANT_THEATER_STEEPNESS]);
	// No counted enemy strength means the local force cannot answer, so an enemy center's fight protects
	// nothing. Our own city is the exception: with no enemy strength its exposure is its whole stake, and it has no
	// Withdraw that could take that exposure.
	field.fExposureShare = reinforce.fEnemy > 0.0 ? reinforce.fShare : (bOurCity ? 1.0 : 0.0);

	if (bInheritsTheater)
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

// A unit target is followed to its current plot; cities and camps stay on their plots.
CvPlot* CvTacticalOperationalLoop::BattlefieldPlot(SOperationalBattlefield& field)
{
	if (field.eCenter == OPERATIONAL_CENTER_CAMP)
		return GC.getMap().plotByIndex(field.iPlotIndex);
	if (field.eOwner == NO_PLAYER)
		return NULL;
	if (field.eCenter == OPERATIONAL_CENTER_ENEMY_UNIT)
	{
		CvUnit* pUnit = GET_PLAYER(field.eOwner).getUnit(field.iTargetID);
		if (!pUnit || pUnit->isDelayedDeath() || !pUnit->plot())
			return NULL;
		// The camp bonus belongs to the opening guard while it stays on the camp.
		if (field.iPlotIndex != pUnit->plot()->GetPlotIndex())
			field.fCampValue = -1.0;
		field.iPlotIndex = pUnit->plot()->GetPlotIndex();
		return pUnit->plot();
	}
	CvCity* pCity = GET_PLAYER(field.eOwner).getCity(field.iTargetID);
	return pCity ? pCity->plot() : NULL;
}

// Identifies a forecast by center, owner and target ID.
static OperationalForecastKey ForecastKey(const SOperationalBattlefield& field)
{
	return OperationalForecastKey(static_cast<int>(field.eCenter), make_pair(static_cast<int>(field.eOwner), field.iTargetID));
}

// Returns the chance in 1/10000 of getting past the planned interceptor and updates expected self damage.
// Non-air attacks bypass this adjustment.
static int AirStrikeChance(const CvUnit& kAttacker, const CvPlot& kTarget, const CvUnit* pInterceptor, int iRisk, int& iSelfDamage)
{
	if (kAttacker.getDomainType() != DOMAIN_AIR)
		return 10000;
	OperationalDamageRollScope roll(EnemyRollPer200(iRisk));
	return TacticalAIHelpers::GetAirStrikeChance(&kAttacker, pInterceptor, &kTarget, 0, 0, iSelfDamage);
}

// Stores damage dealt and taken before interception.
struct SOperationalAttackDamage
{
	int iDamage;
	int iGarrisonDamage;
	int iSelfDamage;
};

// Projects damage to the city and garrison and retaliation before interception.
static SOperationalAttackDamage ProjectCityAttackDamage(const CvCity& kCity, const CvUnit& kAttacker,
	const SOperationalCityDamage& damage, int iRisk)
{
	SOperationalAttackDamage hit = { 0, 0, 0 };
	// A replacement still supplies native city strength, but absorbs none of the later hits.
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

// Projects damage to the target unit and retaliation before interception.
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

// Applies the planned interceptor before testing whether the attack is worth making.
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

// Counts visible interceptors only when the damage filter has an air striker.
static int CountVisibleAirInterceptors(PlayerTypes ePlayer, const CvPlot& kTarget, const SOperationalLocalSet& local)
{
	for (size_t i = 0; i < local.vDamage.size(); i++)
		if (local.vDamage[i]->getDomainType() == DOMAIN_AIR)
			return kTarget.GetInterceptorCount(ePlayer, NULL, false, true);
	return 0;
}

// Holds the planned air exchange for one entry in the damage filter.
struct SOperationalAirExchange
{
	const CvUnit* pInterceptor;
	bool bSweeps; // the fighter sweeps instead of striking
};

// Plans surviving sweeps first, then assigns remaining visible interceptions using the starting damage estimates.
static vector<SOperationalAirExchange> PlanAirExchanges(PlayerTypes ePlayer, const CvPlot& kTarget, const SOperationalLocalSet& local,
	int iVisibleInterceptors, const vector<SOperationalAttackDamage>& vAirHits, int iRisk, bool bCityTarget)
{
	const SOperationalAirExchange noExchange = { NULL, false };
	vector<SOperationalAirExchange> vExchanges(local.vDamage.size(), noExchange);
	// The count is zero without air strikers or visible interceptors.
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
		// The conservative sweep cap is the smaller of the non-fighter striker and visible interceptor
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
			// Assume a lethal sweep leaves the interception for a later strike. Native spends it either way.
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

		// A hit absorbed by the garrison still counts toward the size of the strike.
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

		// A strike that fails against the starting target keeps its interceptor but leaves the use for the next strike.
		vExchanges[iAttacker].pInterceptor = pInterceptor;
		SOperationalAttackDamage hit = vAirHits[iAttacker];
		if (ApplyInterceptionAndTest(kTarget, *pAttacker, pInterceptor, iRisk, bCityTarget, hit))
			++interceptionsUsed[pInterceptor->GetID()];
	}
	return vExchanges;
}

// Values the whole battlefield and what local attackers can realize, in typical units. The realized exposure
// counts the part our strength can protect, s, unless the exposure split is off.
bool CvTacticalOperationalLoop::EstimateFight(CvTacticalAI& kTactical, const SOperationalTurn& turn, SOperationalEstimateCache& cache, const SOperationalLocalSet& local,
	const CvCity* pZoneCity, SOperationalBattlefield& field, bool bSample)
{
	const double* afWeight = turn.weights.afWeight;
	const double fUnitScale = turn.fTypicalUnitWorth;
	const int iRisk = turn.aiMapped[STacticalFlavors::RISK];
	SOperationalStake stake;
	// The fight keeps the exposure our strength can protect; with the split off it counts all of it.
	const double fSplit = turn.weights.afConstant[OPERATIONAL_CONSTANT_EXPOSURE_SPLIT] != 0.0 ? field.fExposureShare : 1.0;
	stake.fExposureShare = fSplit;
	// Only previously selected targets have a cached speculative rejection fraction.
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
		int iRejected = 0;
		const int iDamage = LocalUnitDamage(turn.ePlayer, *pUnit, local, iRisk, iRejected, pForecast, bSample);
		stake.fDamageShare = DamageShare(ScoredDamage(iDamage, iRejected), pUnit->GetCurrHitPoints());
		OperationalThreatMap::const_iterator threat = turn.unitThreats.find(make_pair(static_cast<int>(field.eOwner), field.iTargetID));
		const double fExposure = threat != turn.unitThreats.end() ? threat->second.fExposure : 0.0;
		double fFullDestruction = 0.0, fFullExposure = 0.0, fFullRelief = 0.0;
		if (fUnitScale > 0.0)
		{
			const double fDestroyed = DestroyedUnitValue(*pUnit, iDamage, iRejected);
			const double fLiving = DestroyedUnitValue(*pUnit, pUnit->GetCurrHitPoints(), 0);
			stake.afValue[0] = fDestroyed / fUnitScale;
			stake.afFull[0] = fLiving / fUnitScale;
			stake.fDestruction = afWeight[OPERATIONAL_WEIGHT_ATTRITION] * fDestroyed / fUnitScale;
			fFullDestruction = afWeight[OPERATIONAL_WEIGHT_ATTRITION] * fLiving / fUnitScale;
			stake.afFull[1] = stake.afValue[1] = fExposure / fUnitScale;
			stake.fExposure = afWeight[OPERATIONAL_WEIGHT_RISK] * fExposure / fUnitScale * fSplit;
			fFullExposure = afWeight[OPERATIONAL_WEIGHT_RISK] * fExposure / fUnitScale;
		}
		stake.afValue[2] = stake.fDamageShare * field.fGround;
		stake.afFull[2] = field.fGround;
		stake.fGround = stake.fDamageShare * afWeight[OPERATIONAL_WEIGHT_HOLD_GROUND] * field.fGround;
		const double fFullGround = afWeight[OPERATIONAL_WEIGHT_HOLD_GROUND] * field.fGround;
		if (pZoneCity && pZoneCity->getOwner() == turn.ePlayer)
		{
			const SOperationalCityDanger& danger = CachedCityDanger(turn, *pZoneCity);
			const vector<SOperationalThreatDamage>& threats = danger.threats;
			const int iDanger = danger.iTotal;
			int iTargetDamage = 0;
			for (size_t i = 0; i < threats.size(); i++)
			{
				if (!threats[i].bCity && threats[i].eOwner == field.eOwner && threats[i].iID == field.iTargetID)
					iTargetDamage += std::max(0, threats[i].TotalDamage());
			}
			stake.fFallShare = DamageShare(iDanger, std::max(1, pZoneCity->GetMaxHitPoints() - pZoneCity->getDamage()));
			// City danger is the sum of these attack contributions.
			stake.fReliefShare = iDanger > 0 ? static_cast<double>(iTargetDamage) / iDanger : 0.0;
			stake.afValue[3] = stake.fDamageShare * turn.fCityUnit * stake.fFallShare * stake.fReliefShare;
			stake.afFull[3] = turn.fCityUnit * stake.fFallShare * stake.fReliefShare;
			stake.fRelief = stake.fDamageShare * afWeight[OPERATIONAL_WEIGHT_HOLD_CITY] * turn.fCityUnit *
				stake.fFallShare * stake.fReliefShare;
			fFullRelief = afWeight[OPERATIONAL_WEIGHT_HOLD_CITY] * turn.fCityUnit * stake.fFallShare * stake.fReliefShare;
		}
		stake.fRealized = stake.fDestruction + stake.fExposure + stake.fGround + stake.fRelief;
		stake.fStake = fFullDestruction + fFullExposure + fFullGround + fFullRelief;
		if (field.fCampValue >= 0.0)
		{
			stake.fRealized += stake.fDamageShare * field.fCampValue;
			stake.fStake += field.fCampValue;
		}
	}
	else if (field.eCenter == OPERATIONAL_CENTER_OUR_CITY)
	{
		CvCity* pCity = GET_PLAYER(field.eOwner).getCity(field.iTargetID);
		if (!pCity || pCity->getOwner() != turn.ePlayer)
			return false;
		const int iDanger = CachedCityDanger(turn, *pCity).iTotal;
		stake.fFallShare = DamageShare(iDanger, std::max(1, pCity->GetMaxHitPoints() - pCity->getDamage()));

		vector<CvUnit*> vEndangered;
		map<int, int> dangers;
		GatherCityEndangered(kTactical, turn, *pCity, vEndangered, field, &dangers);
		const double fExposure = CityDefenseExposure(turn, cache, vEndangered, dangers);
		field.bHasEndangered = !vEndangered.empty();

		stake.afValue[4] = turn.fCityUnit * stake.fFallShare;
		stake.afFull[4] = turn.fCityUnit;
		stake.fCityProgress = afWeight[OPERATIONAL_WEIGHT_HOLD_CITY] * turn.fCityUnit * stake.fFallShare;
		const double fFullProgress = afWeight[OPERATIONAL_WEIGHT_HOLD_CITY] * turn.fCityUnit;
		double fFullExposure = 0.0;
		if (fUnitScale > 0.0)
		{
			stake.afFull[1] = stake.afValue[1] = fExposure / fUnitScale;
			stake.fExposure = afWeight[OPERATIONAL_WEIGHT_RISK] * fExposure / fUnitScale * fSplit;
			fFullExposure = afWeight[OPERATIONAL_WEIGHT_RISK] * fExposure / fUnitScale;
		}
		stake.fRealized = stake.fCityProgress + stake.fExposure;
		stake.fStake = fFullProgress + fFullExposure;
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
		// Project the starting air hits before city and garrison damage accumulate.
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
					// A sampled capturer must still belong to the current local capture roster.
					if (eReadiness == OPERATIONAL_STRIKE_IMMEDIATE || pForecast->reachableCapturers.count(kAttacker.GetID()) > 0)
						bCanCapture = true;
				}
			}
			// Reuse the starting hit until the simulated city or garrison changes.
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
		stake.afFull[4] = turn.fCityUnit;
		stake.fCityProgress = afWeight[OPERATIONAL_WEIGHT_OCCUPATION] * turn.fCityUnit * stake.fFallShare;
		const double fFullProgress = afWeight[OPERATIONAL_WEIGHT_OCCUPATION] * turn.fCityUnit;

		CvUnit* pGarrison = pCity->GetGarrisonedUnit();
		double fExposure = 0.0;
		OperationalThreatMap::const_iterator threat = turn.cityThreats.find(make_pair(static_cast<int>(field.eOwner), field.iTargetID));
		if (threat != turn.cityThreats.end())
			fExposure += threat->second.fExposure;
		if (pGarrison)
		{
			threat = turn.unitThreats.find(make_pair(static_cast<int>(pGarrison->getOwner()), pGarrison->GetID()));
			if (threat != turn.unitThreats.end())
				fExposure += threat->second.fExposure;
		}
		double fFullGarrison = 0.0, fFullExposure = 0.0;
		if (fUnitScale > 0.0)
		{
			if (damage.pGarrison && damage.iGarrisonHealth > 0)
			{
				const double fDestroyed = DestroyedUnitValue(*damage.pGarrison,
					stake.fFallShare >= 1.0 ? damage.iGarrisonHealth : damage.iGarrisonDamage, damage.iRemovedGarrison);
				const double fLiving = stake.fFallShare >= 1.0 ? fDestroyed : DestroyedUnitValue(*damage.pGarrison, damage.iGarrisonHealth, 0);
				stake.afValue[5] = fDestroyed / fUnitScale;
				stake.afFull[5] = fLiving / fUnitScale;
				stake.fGarrison = afWeight[OPERATIONAL_WEIGHT_ATTRITION] * fDestroyed / fUnitScale;
				fFullGarrison = afWeight[OPERATIONAL_WEIGHT_ATTRITION] * fLiving / fUnitScale;
			}
			stake.afFull[1] = stake.afValue[1] = fExposure / fUnitScale;
			stake.fExposure = afWeight[OPERATIONAL_WEIGHT_RISK] * fExposure / fUnitScale * fSplit;
			fFullExposure = afWeight[OPERATIONAL_WEIGHT_RISK] * fExposure / fUnitScale;
		}
		stake.fRealized = stake.fCityProgress + stake.fGarrison + stake.fExposure;
		stake.fStake = fFullProgress + fFullGarrison + fFullExposure;
	}
	stake.fPotential = stake.fStake - stake.fRealized;
	field.stake = stake;
	field.fEstimate = field.fTheater * stake.fRealized;
	return true;
}

// Uses the incoming force's weighted join time at enemy targets and one turn at our city. Fogged camps have no potential.
static double ReinforceTurnsToContact(eOperationalCenter eCenter, const SOperationalReinforce& reinforce)
{
	if (eCenter == OPERATIONAL_CENTER_CAMP)
		return 0.0;
	return eCenter == OPERATIONAL_CENTER_OUR_CITY || reinforce.fHelp <= 0.0 ? 1.0 :
		std::max(1.0, reinforce.fSelectedStrength / reinforce.fHelp);
}

// Prices the scaled swing from incoming strength on realized and discounted potential stake, with camp value as a floor.
static double ScoreReinforce(const SOperationalTurn& turn, SOperationalBattlefield& field)
{
	SOperationalReinforce& reinforce = field.reinforce;
	// Selection stops at parity, so the raw swing tops out near one half; the scale puts it on the fight's range.
	const double fScaledSwing = std::min(1.0, turn.weights.afConstant[OPERATIONAL_CONSTANT_SWING_SCALE] * reinforce.fSwing);
	const double fCampBase = field.fCampValue >= 0.0 ? field.fTheater * field.fCampValue : 0.0;
	// Keep the original score for Withdraw and for offers that get no credit for potential stake.
	const double fRealizedBase = field.fCampValue >= 0.0 ? std::max(field.fEstimate, fCampBase) : field.fEstimate;
	reinforce.fRealizedEstimate = fRealizedBase * fScaledSwing;
	reinforce.fTurnsToContact = ReinforceTurnsToContact(field.eCenter, reinforce);
	reinforce.fPotentialWeight = reinforce.fTurnsToContact > 0.0 ?
		PotentialWeight(turn.weights.afConstant[OPERATIONAL_CONSTANT_POTENTIAL_WEIGHT],
			turn.weights.afConstant[OPERATIONAL_CONSTANT_POTENTIAL_TURN_DISCOUNT], reinforce.fTurnsToContact) : 0.0;
	if (reinforce.fPotentialWeight == 0.0 || field.stake.fPotential == 0.0)
		return reinforce.fRealizedEstimate;
	const double fBase = field.fTheater * (field.stake.fRealized + reinforce.fPotentialWeight * field.stake.fPotential);
	return (field.fCampValue >= 0.0 ? std::max(fBase, fCampBase) : fBase) * fScaledSwing;
}

// Estimates how much realized and potential stake the selected help can swing.
void CvTacticalOperationalLoop::EstimateReinforce(CvTacticalAI& kTactical, const SOperationalTurn& turn, const SOperationalLocalSet& local,
	SOperationalBattlefield& field)
{
	SOperationalReinforce& reinforce = field.reinforce;
	field.fReinforceEstimate = 0.0;
	reinforce.fShareWithHelp = reinforce.fShare;
	reinforce.fSwing = 0.0;
	reinforce.fRealizedEstimate = 0.0;
	reinforce.fTurnsToContact = 0.0;
	reinforce.fPotentialWeight = 0.0;

	// Enemy-only gates close recruitment; selection still clears the old offer and records need.
	CvPlot* pTarget = GC.getMap().plotByIndex(field.iPlotIndex);
	const bool bOurCity = field.eCenter == OPERATIONAL_CENTER_OUR_CITY;
	const bool bOpen = pTarget && (bOurCity || ((field.bBorder || field.fCampValue >= 0.0) && !field.bWithdrawPosture));
	// A city closer to falling asks for more than parity; the Fight set its fall share earlier in the pass.
	double fRatio = turn.weights.afConstant[OPERATIONAL_CONSTANT_STRENGTH_RATIO];
	if (bOurCity)
		fRatio *= 1.0 + turn.weights.afConstant[OPERATIONAL_CONSTANT_FALL_URGENCY] * field.stake.fFallShare;
	vector<SOperationalCandidate> vCandidates;
	if (bOpen)
		GatherReinforceCandidates(kTactical, field, *pTarget, local.bLand, local.bSea, vCandidates);
	SelectReinforcement(vCandidates, reinforce.fLocal, reinforce.fEnemy, fRatio,
		bOpen && field.eCenter == OPERATIONAL_CENTER_ENEMY_CITY && !local.bCapturer, reinforce);
	if (reinforce.vSelected.empty())
		return;

	reinforce.fShareWithHelp = StrengthShare(reinforce.fLocal + reinforce.fHelp, reinforce.fEnemy,
		turn.weights.afConstant[OPERATIONAL_CONSTANT_THEATER_STEEPNESS]);
	reinforce.fSwing = StrengthSwing(reinforce.fShare, reinforce.fShareWithHelp);

	// Partial defense still counts; an enemy-center march must reach the desired full-strength ratio.
	if (!bOurCity && reinforce.fSelectedStrength < reinforce.fNeed)
		return;

	field.fReinforceEstimate = ScoreReinforce(turn, field);
}

// Selects the exposure layout before omitted recruits or projected stops change it.
static const OperationalThreatMap& ExposureLayout(const SOperationalTurn& turn, bool bCity,
	bool bIndependentWithdraw, bool bSharedLayout = false)
{
	if (bSharedLayout)
		return bCity ? turn.sharedExposure.cities : turn.sharedExposure.units;
	if (bIndependentWithdraw && !turn.preparedArmies.empty())
		return bCity ? turn.withdrawCityThreats : turn.withdrawUnitThreats;
	return bCity ? turn.cityThreats : turn.unitThreats;
}

// Collects the local combat units and enrolled formations threatened at this battlefield.
// Recruits are sorted by ID; spent units, friendly city holders and civilian operations cannot leave.
void CvTacticalOperationalLoop::GatherWithdrawRecruits(CvTacticalAI& kTactical, const SOperationalTurn& turn,
	const SOperationalBattlefield& field, const SOperationalLocalSet& local, vector<CvUnit*>& vRecruits)
{
	vRecruits.clear();
	if (field.eCenter == OPERATIONAL_CENTER_OUR_CITY)
		return;

	// Shared Fight eligibility does not change independent Withdraw recruitment.
	const bool bArmyThreats = !turn.preparedArmies.empty();
	set<int> candidates;
	set<int> threatenedArmies;
	const bool bCity = field.eCenter == OPERATIONAL_CENTER_ENEMY_CITY;
	const CvCity* pCity = bCity && field.eOwner != NO_PLAYER ? GET_PLAYER(field.eOwner).getCity(field.iTargetID) : NULL;
	const CvUnit* pGarrison = pCity ? pCity->GetGarrisonedUnit() : NULL;
	for (int i = 0; i < (pGarrison ? 2 : 1); i++)
	{
		const bool bCityThreat = i == 0 && bCity;
		const pair<int, int> key = i == 0 ? make_pair(static_cast<int>(field.eOwner), field.iTargetID) :
			make_pair(static_cast<int>(pGarrison->getOwner()), pGarrison->GetID());
		const OperationalThreatMap& independent = ExposureLayout(turn, bCityThreat, true);
		const OperationalThreatMap::const_iterator it = independent.find(key);
		if (it != independent.end())
			candidates.insert(it->second.vUnitIDs.begin(), it->second.vUnitIDs.end());
		if (!bArmyThreats)
			continue;
		const OperationalThreatMap& shared = bCityThreat ? turn.cityThreats : turn.unitThreats;
		const OperationalThreatMap::const_iterator threat = shared.find(key);
		if (threat != shared.end())
			for (size_t j = 0; j < threat->second.vUnitIDs.size(); j++)
			{
				const CvUnit* pUnit = kTactical.m_pPlayer->getUnit(threat->second.vUnitIDs[j]);
				if (pUnit && turn.preparedArmies.count(pUnit->getArmyID()) > 0)
					threatenedArmies.insert(pUnit->getArmyID());
			}
	}
	for (set<int>::const_iterator it = threatenedArmies.begin(); it != threatenedArmies.end(); ++it)
	{
		CvArmyAI* pArmy = kTactical.m_pPlayer->getArmyAI(*it);
		vector<CvUnit*> members;
		if (pArmy)
			ArmyMarchMembers(*pArmy, members);
		for (size_t i = 0; i < members.size(); i++)
			candidates.insert(members[i]->GetID());
	}
	for (size_t i = 0; i < local.vStrength.size(); i++)
		candidates.insert(local.vStrength[i]->GetID());

	// A unit can cover a victim, or be exposed by its departure, even outside the local set's domain.
	set<int> plots;
	for (set<int>::const_iterator it = candidates.begin(); it != candidates.end(); ++it)
	{
		const CvUnit* pUnit = kTactical.m_pPlayer->getUnit(*it);
		if (pUnit && pUnit->plot())
			plots.insert(pUnit->plot()->GetPlotIndex());
	}
	for (set<int>::const_iterator it = plots.begin(); it != plots.end(); ++it)
	{
		const CvPlot* pPlot = GC.getMap().plotByIndex(*it);
		for (const IDInfo* pNode = pPlot->headUnitNode(); pNode; pNode = pPlot->nextUnitNode(pNode))
			if (pNode->eOwner == turn.ePlayer)
				candidates.insert(pNode->iID);
	}

	for (set<int>::const_iterator it = candidates.begin(); it != candidates.end(); ++it)
	{
		CvUnit* pUnit = kTactical.m_pPlayer->getUnit(*it);
		if (!pUnit || pUnit->isDelayedDeath() || !pUnit->IsCombatUnit() ||
			(pUnit->getDomainType() != DOMAIN_LAND && pUnit->getDomainType() != DOMAIN_SEA) ||
			(pUnit->getArmyID() == -1 ? turn.startRoster.count(*it) == 0 : threatenedArmies.count(pUnit->getArmyID()) == 0) ||
			!pUnit->canMove() ||
			!pUnit->plot() || pUnit->plot()->isFriendlyCity(*pUnit) || IsHeldGarrison(pUnit) ||
			turn.withdrawnUnits.count(*it) > 0)
			continue;
		vRecruits.push_back(pUnit);
	}
}

// Compares signed capped exposure after omitting recruits or projecting stops.
double CvTacticalOperationalLoop::ExposureSaved(const SOperationalTurn& turn, const vector<SOperationalMarch>& marches,
	bool bProjectStops, bool bFormationWithdraw)
{
	if (marches.empty())
		return 0.0;
	set<int> replaced;
	OperationalThreatMap unitChanges, cityChanges;
	// Compare the whole occupied board when the formation can pass attacks to other units.
	const bool bSharedLayout = bFormationWithdraw && bProjectStops;
	if (bSharedLayout && !turn.bSharedExposureBuilt)
	{
		BuildOccupiedExposure(turn, turn.sharedExposure.units, turn.sharedExposure.cities, NULL, true);
		turn.bSharedExposureBuilt = true;
	}
	if (bProjectStops)
	{
		CvPlayer& kPlayer = GET_PLAYER(turn.ePlayer);
		set<int> changedPlots;
		set<int> moving;
		map<int, vector<SPlacedUnit> > arrivals;
		for (size_t i = 0; i < marches.size(); i++)
		{
			const CvUnit* pUnit = kPlayer.getUnit(marches[i].iUnitID);
			const CvPlot* pStop = GC.getMap().plotByIndex(marches[i].iStop);
			if (!pUnit || !pUnit->plot() || !pStop)
				continue;
			vector<SPlacedUnit>& incoming = arrivals[marches[i].iStop];
			// Withdraw commits to exact stops that must fit together.
			if (!FitsMarchStop(*pUnit, pStop, incoming))
				continue;
			moving.insert(marches[i].iUnitID);
			changedPlots.insert(pUnit->plot()->GetPlotIndex());
			changedPlots.insert(marches[i].iStop);
			const bool bEmbarked = pStop->needsEmbarkation(pUnit) && pUnit->CanEverEmbark();
			incoming.push_back(SPlacedUnit(pUnit, pStop, 0, bEmbarked));
		}

		CvBoardPlacement board;
		vector<SPlacedUnit> live, projected;
		for (set<int>::const_iterator it = changedPlots.begin(); it != changedPlots.end(); ++it)
		{
			const CvPlot* pPlot = GC.getMap().plotByIndex(*it);
			board.getUnitsAt(pPlot, live);
			projected.clear();
			for (size_t i = 0; i < live.size(); i++)
			{
				const CvUnit& kUnit = *live[i].pUnit;
				if (kUnit.getOwner() == turn.ePlayer)
				{
					replaced.insert(kUnit.GetID());
					if (moving.count(kUnit.GetID()) > 0)
						continue;
				}
				projected.push_back(live[i]);
			}
			const vector<SPlacedUnit>& incoming = arrivals[*it];
			projected.insert(projected.end(), incoming.begin(), incoming.end());
			if (!projected.empty())
				AllocatePlotExposure(turn.ePlayer, *pPlot, projected, turn.aiMapped[STacticalFlavors::RISK], unitChanges, cityChanges,
					NULL, bSharedLayout);
		}
	}
	else
		for (size_t i = 0; i < marches.size(); i++)
			replaced.insert(marches[i].iUnitID);

	double fSaved = 0.0;
	for (int iKind = 0; iKind < 2; iKind++)
	{
		const OperationalThreatMap& before = ExposureLayout(turn, iKind == 1, !bFormationWithdraw, bSharedLayout);
		OperationalThreatMap& after = iKind == 0 ? unitChanges : cityChanges;
		for (OperationalThreatMap::const_iterator it = before.begin(); it != before.end(); ++it)
		{
			const SOperationalThreatExposure& threat = it->second;
			OperationalThreatMap::iterator update = after.find(it->first);
			if (update == after.end())
			{
				for (size_t i = 0; i < threat.vUnitIDs.size(); i++)
					if (replaced.count(threat.vUnitIDs[i]) > 0)
					{
						fSaved += threat.fExposure - CappedExposure(threat, &replaced);
						break;
					}
				continue;
			}
			// Unchanged targets still compete for this attacker's limited attacks.
			fSaved += threat.fExposure;
			for (size_t i = 0; i < threat.vUnitIDs.size(); i++)
				if (replaced.count(threat.vUnitIDs[i]) == 0)
				{
					update->second.vUnitIDs.push_back(threat.vUnitIDs[i]);
					update->second.vAllocations.push_back(threat.vAllocations[i]);
				}
		}
		for (OperationalThreatMap::const_iterator it = after.begin(); it != after.end(); ++it)
			fSaved -= CappedExposure(it->second, NULL);
	}
	return fSaved;
}

// Reads the fight share and any larger share from a Reinforce that scores on realized stake.
static double ExposureShareWithHelp(const SOperationalBattlefield& field)
{
	const bool bReinforceReady = (field.eReinforce == OPERATIONAL_OFFER_READY || field.eReinforce == OPERATIONAL_OFFER_INACTIVE) &&
		field.reinforce.fRealizedEstimate > 0.0 && field.reinforce.fEnemy > 0.0;
	return bReinforceReady ? std::max(field.fExposureShare, field.reinforce.fShareWithHelp) : field.fExposureShare;
}

// Scores the exposure that help does not cover, with a discount for each failed readiness check.
double CvTacticalOperationalLoop::WithdrawPriority(const SOperationalTurn& turn, const SOperationalBattlefield& field,
	double fExposure, double fShare)
{
	if (turn.fTypicalUnitWorth <= 0.0)
		return 0.0;
	return std::max(0.0, field.fTheater * turn.weights.afWeight[OPERATIONAL_WEIGHT_RISK] *
		fExposure / turn.fTypicalUnitWorth * (1.0 - fShare) *
		pow(0.7, field.iWithdrawFailures));
}

// The Withdraw priority: the removable exposure times the part of the strength share that help does not
// cover. It stays inactive until a recruit is endangered.
void CvTacticalOperationalLoop::EstimateWithdraw(CvTacticalAI& kTactical, const SOperationalTurn& turn,
	const SOperationalLocalSet& local, SOperationalBattlefield& field)
{
	field.fWithdrawEstimate = 0.0;
	field.fWithdrawExposure = 0.0;
	field.fWithdrawShare = 0.0;
	// Our own city keeps no Withdraw, and the switch or a zero typical worth turns the offer off.
	if (field.eCenter == OPERATIONAL_CENTER_OUR_CITY ||
		turn.weights.afConstant[OPERATIONAL_CONSTANT_WITHDRAW_ENABLED] == 0.0 || turn.fTypicalUnitWorth <= 0.0)
		return;

	vector<CvUnit*> vRecruits;
	GatherWithdrawRecruits(kTactical, turn, field, local, vRecruits);
	// A recruit in danger sets the priority; healthy units go back with it but do not start the offer.
	bool bEndangered = false;
	for (size_t i = 0; i < vRecruits.size() && !bEndangered; i++)
		bEndangered = turn.endangeredUnits.count(vRecruits[i]->GetID()) > 0;
	if (!bEndangered)
		return;

	vector<SOperationalMarch> recruits;
	bool bFormationWithdraw = false;
	for (size_t i = 0; i < vRecruits.size(); i++)
	{
		SOperationalMarch recruit = {vRecruits[i]->GetID(), -1, -1, 0};
		recruits.push_back(recruit);
		bFormationWithdraw |= vRecruits[i]->getArmyID() != -1;
	}
	field.fWithdrawExposure = std::max(0.0, ExposureSaved(turn, recruits, false, bFormationWithdraw));
	// A Reinforce that scores on realized stake supplies s+ from the larger of the fight's share and the projected
	// share, so a lower projection cannot raise the Withdraw priority.
	field.fWithdrawShare = ExposureShareWithHelp(field);
	field.fWithdrawEstimate = WithdrawPriority(turn, field, field.fWithdrawExposure, field.fWithdrawShare);
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

		// Local units count as strength already at the battlefield, so they are never help.
		const int iDistance = plotDistance(*pUnit->plot(), kTarget);
		const bool bHeld = IsHeldGarrison(pUnit);
		if ((bHeld && field.eCenter == OPERATIONAL_CENTER_CAMP) ||
			iDistance <= StrikeReach(*pUnit) ||
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

		// Enemy-city help must pass the same strike filter as local attackers.
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

// Plain distance, no pathfinding. In-domain land and sea units add strength within strike reach; ranged
// units of either domain and aircraft in range can join the damage filter. Local attackers need a roster spot, an attack
// left and the checks from CvTacticalAI::FindUnitsWithinStrikingDistance that need no pathfinding. Fighters can strike
// with air search on if they can also sweep from their current plot.
void CvTacticalOperationalLoop::GatherLocalSet(CvTacticalAI& kTactical, PlayerTypes ePlayer, const CvPlot& kPlot, bool bLand, bool bSea,
	SOperationalLocalSet& local, bool bDamage, const SOperationalBattlefield* pFight)
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
		const bool bRecruit = pFight && pUnit->getArmyID() != -1 ? IsFightArmyMember(*pUnit, *pFight) : roster.count(pUnit->GetID()) > 0;
		if (!bDamage || (pUnit->IsCanAttackRanged() && pUnit->isEmbarked()) ||
			!bRecruit || !pUnit->canUseForTacticalAI() || pUnit->isOutOfAttacks() || pUnit->IsCoveringFriendlyCivilian())
			continue;

		// A sweep needs a valid target plot but does not need to pass the city strike test.
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

// Gathers local units, keeping unit fight estimates in the plot's terrain domain even at a city.
void CvTacticalOperationalLoop::GatherTargetLocalSet(CvTacticalAI& kTactical, PlayerTypes ePlayer, const CvPlot& kTarget,
	SOperationalLocalSet& local, bool bDamage, bool bUnitTarget, const SOperationalBattlefield* pFight)
{
	const CvCity* pCity = bUnitTarget ? NULL : kTarget.getPlotCity();
	GatherLocalSet(kTactical, ePlayer, kTarget, pCity != NULL || !kTarget.isWater(),
		pCity ? pCity->isCoastal() : kTarget.isWater(), local, bDamage, pFight);
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

// Copies the city case of CvTacticalAI::ComputeTotalExpectedDamage for one attacker. Applies the planned
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
	// Reject unreachable hits before they alter hypothetical city or garrison health.
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

// Counts city fire without changing the move list, then tallies local hits from largest to smallest.
int CvTacticalOperationalLoop::LocalUnitDamage(PlayerTypes ePlayer, const CvUnit& kTarget, const SOperationalLocalSet& local, int iRisk, int& iRejected,
	SOperationalForecast* pForecast, bool bSample)
{
	CvPlayer& kPlayer = GET_PLAYER(ePlayer);
	CvPlot* pTargetPlot = kTarget.plot();
	// Share the starting air hits between interception planning and the damage tally.
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
	// The melee cap keeps the largest hits, so attackers are taken by damage, then by unit ID.
	vector<pair<int, size_t> > vHits;
	for (size_t i = 0; i < local.vDamage.size(); i++)
	{
		if (vExchanges[i].bSweeps)
			continue;
		const CvUnit& kAttacker = *local.vDamage[i];
		SOperationalAttackDamage hit = !vAirHits.empty() && kAttacker.getDomainType() == DOMAIN_AIR
			? vAirHits[i] : RawUnitAttackDamage(kTarget, kAttacker, iRisk);
		// The unsampled tally counts every hit that passes the native test; forecasts count positive hits.
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

// Preserves admitted air estimates and legal immediate attacks from the actual origin.
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

// Copies source-independent target checks from CvUnit::canRangeStrikeAt.
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

// Tests legal attack bases independently without reserving firing tiles for other attackers.
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

// Rounds a cached fraction against the current speculative amount only.
int CvTacticalOperationalLoop::RejectedDamage(int iCurrent, int iRejected, int iSampled)
{
	return iSampled > 0 ? static_cast<int>((static_cast<long long>(iCurrent) * iRejected + iSampled / 2) / iSampled) : 0;
}

// Samples a target on first selection while the command loop has a forecast budget left.
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
	GatherTargetLocalSet(kTactical, turn.ePlayer, *pTarget, local, true, field.eCenter == OPERATIONAL_CENTER_ENEMY_UNIT, turn.bMarchEnabled ? &field : NULL);
	CvPlot* pZone = field.iZoneCityPlot >= 0 ? GC.getMap().plotByIndex(field.iZoneCityPlot) : NULL;
	SOperationalEstimateCache cache;
	const double fBefore = field.fEstimate;
	EstimateFight(kTactical, turn, cache, local, pZone ? pZone->getPlotCity() : NULL, field, true);
	forecast.bChecked = true;
	if (field.fEstimate < fBefore)
	{
		// Keep the selection gates while correcting both Reinforce scores after the fight estimate changes.
		if (field.fReinforceEstimate > 0.0 || field.reinforce.fRealizedEstimate > 0.0)
		{
			field.fReinforceEstimate = ScoreReinforce(turn, field);
			if (field.fReinforceEstimate <= 0.0 && field.eReinforce == OPERATIONAL_OFFER_READY)
				field.eReinforce = OPERATIONAL_OFFER_INACTIVE;
		}
		return OPERATIONAL_CHECK_REDUCED;
	}
	return OPERATIONAL_CHECK_PASSED;
}

// Normalizes threat shares only when identified damage exceeds whole danger.
SOperationalVictimExposure CvTacticalOperationalLoop::AttributeExposure(double fExposure, int iDanger,
	const vector<SOperationalThreatDamage>& threats)
{
	SOperationalVictimExposure result;
	int iTotal = 0;
	for (size_t i = 0; i < threats.size(); i++)
		iTotal += std::max(0, threats[i].TotalDamage());
	const int iDenominator = std::max(iDanger, iTotal);
	if (iDenominator > 0)
		for (size_t i = 0; i < threats.size(); i++)
			if (threats[i].TotalDamage() > 0)
				result.shares[make_pair(threats[i].bCity, make_pair(static_cast<int>(threats[i].eOwner), threats[i].iID))] +=
					fExposure * threats[i].TotalDamage() / iDenominator;
	result.fResidual = iDenominator > 0 ? fExposure * (iDenominator - iTotal) / iDenominator : fExposure;
	return result;
}

// Retains each source's largest allowed shares, then applies geometric decay to the sorted victim exposures.
double CvTacticalOperationalLoop::CityDefenseExposure(const SOperationalTurn& turn, SOperationalEstimateCache& cache,
	const vector<CvUnit*>& vEndangered, const map<int, int>& dangers)
{
	vector<double> exposures(vEndangered.size(), 0.0);
	map<OperationalExposureKey, vector<pair<double, int> > > allocations;
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
				// Use native's pretend-garrison projection and retain environmental danger with the victim.
				const CvCity* pCity = kVictim.plot()->getPlotCity();
				const int iCityDanger = GetCityDangerByThreat(contents, pCity, &kVictim, SUnitIDValueContainer(), 0, &threats, iRisk);
				const bool bFalls = iCityDanger + pCity->getDamage() >= pCity->GetMaxHitPoints();
				const int iHostile = bFalls ? iDanger : (kVictim.CanGarrison() ? std::max(0, iDanger - contents.m_iImprovementDamage) : 0);
				shares = AttributeExposure(fExposure, 0, threats);
				if (iDanger > 0 && !shares.shares.empty())
				{
					const double fFraction = static_cast<double>(iHostile) / iDanger;
					for (map<OperationalExposureKey, double>::iterator it = shares.shares.begin(); it != shares.shares.end(); ++it)
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
		for (map<OperationalExposureKey, double>::const_iterator it = stored->second.shares.begin(); it != stored->second.shares.end(); ++it)
			if (it->second > 0.0)
				// Equal shares keep the earlier native roster victim.
				allocations[it->first].push_back(make_pair(it->second, -static_cast<int>(i)));
	}
	for (map<OperationalExposureKey, vector<pair<double, int> > >::iterator it = allocations.begin(); it != allocations.end(); ++it)
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

// Reads danger contents before installing any roll, so cache refreshes retain native expected damage.
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

// Reads each plot within 4 once. Visible hostile cities and a revealed center city add strength with at
// least 1 HP, including their garrison. Other hostile units add strength only while their plots are visible.
// The optional extra player counts as hostile under the same rules.
double CvTacticalOperationalLoop::EnemyCenterStrength(PlayerTypes ePlayer, const CvPlot& kTarget, bool bLand, bool bSea,
	set<pair<int, int> >& countedUnits, set<pair<int, int> >& countedCities, PlayerTypes eExtraHostile)
{
	countedUnits.clear();
	countedCities.clear();
	const TeamTypes eTeam = GET_PLAYER(ePlayer).getTeam();
	const bool bExtraHostile = eExtraHostile != NO_PLAYER;
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
		if (pCity && pCity->GetMaxHitPoints() > 0 &&
			((bExtraHostile && pCity->getOwner() == eExtraHostile) || GroundSide(ePlayer, pCity->getOwner()) == groundHoldsHostile))
		{
			countedCities.insert(make_pair(static_cast<int>(pCity->getOwner()), pCity->GetID()));
			fStrength += static_cast<double>(pCity->getStrengthValue()) * std::max(1, pCity->GetMaxHitPoints() - pCity->getDamage()) /
				pCity->GetMaxHitPoints();
			pGarrison = pCity->GetGarrisonedUnit();
		}
		// Knowing the center city never reveals units on its fogged plot.
		if (!bVisible)
			continue;
		for (int j = 0; j < pPlot->getNumUnits(); j++)
		{
			const CvUnit* pUnit = pPlot->getUnitByIndex(j);
			if (!pUnit || pUnit == pGarrison || pUnit->isDelayedDeath() || pUnit->isInvisible(eTeam, false) ||
				!(pUnit->IsCombatUnit() || pUnit->IsCanAttackRanged()) ||
				((!bExtraHostile || pUnit->getOwner() != eExtraHostile) && GroundSide(ePlayer, pUnit->getOwner()) != groundHoldsHostile))
				continue;
			if (!((bLand && pUnit->getDomainType() == DOMAIN_LAND) || (bSea && pUnit->getDomainType() == DOMAIN_SEA)))
				continue;
			if (countedUnits.insert(make_pair(static_cast<int>(pUnit->getOwner()), pUnit->GetID())).second)
				fStrength += UnitStrength(*pUnit);
		}
	}
	return fStrength;
}

// Resolves a usable city recruit, allowing members of eligible Fight armies.
CvUnit* CvTacticalOperationalLoop::CityRecruit(CvTacticalAI& kTactical, int iUnitID, const SOperationalBattlefield* pFight)
{
	CvUnit* pUnit = kTactical.m_pPlayer->getUnit(iUnitID);
	if (!pUnit || !pUnit->canUseForTacticalAI() || !pUnit->plot() ||
		(pUnit->getArmyID() != -1 && (!pFight || !IsFightArmyMember(*pUnit, *pFight))))
		return NULL;
	return pUnit;
}

// Lists candidates in roster order, then appends eligible army members once.
static vector<int> CityFightCandidates(CvPlayer& player, const std::list<int>& roster, const SOperationalBattlefield& field)
{
	vector<int> recruits(roster.begin(), roster.end());
	FightArmyRecruits(player, field, recruits);
	return recruits;
}

// Records a nearby non-air recruit if threatened in the open or in a friendly city.
static void AddCityEndangered(CvUnit* pUnit, const CvPlot& cityPlot, const SOperationalTurn& turn,
	vector<CvUnit*>& vEndangered, map<int, int>* pDangers)
{
	if (!pUnit || pUnit->getDomainType() == DOMAIN_AIR || plotDistance(*pUnit->plot(), cityPlot) > TACTICAL_COMBAT_MAX_TARGET_DISTANCE)
		return;
	map<int, int>::const_iterator danger = turn.endangeredUnits.find(pUnit->GetID());
	const int iDanger = danger != turn.endangeredUnits.end() ? danger->second :
		(pUnit->plot()->isFriendlyCity(*pUnit) ? CvTacticalOperationalLoop::UnitDanger(turn, *pUnit) : 0);
	if (danger != turn.endangeredUnits.end() || iDanger > 0)
	{
		vEndangered.push_back(pUnit);
		if (pDangers)
			(*pDangers)[pUnit->GetID()] = iDanger;
	}
}

// Finds endangered recruits in roster order, followed by additional eligible army members.
void CvTacticalOperationalLoop::GatherCityEndangered(CvTacticalAI& kTactical, const SOperationalTurn& turn, const CvCity& kCity,
	vector<CvUnit*>& vEndangered, const SOperationalBattlefield& field, map<int, int>* pDangers)
{
	vEndangered.clear();
	if (pDangers)
		pDangers->clear();
	const CvPlot& cityPlot = *kCity.plot();
	const vector<int> recruits = CityFightCandidates(*kTactical.m_pPlayer, kTactical.m_CurrentTurnUnits, field);
	for (size_t i = 0; i < recruits.size(); i++)
		AddCityEndangered(CityRecruit(kTactical, recruits[i], &field), cityPlot, turn, vEndangered, pDangers);
}

// Adds an unthreatened defender whose strike or carrier launch range reaches the city.
static void AddCityDefender(CvUnit* pUnit, const CvPlot& cityPlot, bool bCoastal, const vector<CvUnit*>& vEndangered,
	vector<CvUnit*>& vDefenders)
{
	if (!pUnit || pUnit->IsCoveringFriendlyCivilian() || std::find(vEndangered.begin(), vEndangered.end(), pUnit) != vEndangered.end())
		return;
	if (pUnit->getDomainType() == DOMAIN_AIR)
	{
		if (!MOD_VOX_TACTICAL_AIR_SEARCH)
			return;
		const vector<int> vLaunchPlots = TacticalAIHelpers::GetCargoLaunchPlots(pUnit, &cityPlot);
		for (size_t i = 0; i < vLaunchPlots.size(); i++)
			if (plotDistance(*GC.getMap().plotByIndexUnchecked(vLaunchPlots[i]), cityPlot) <= pUnit->GetRange())
			{
				vDefenders.push_back(pUnit);
				break;
			}
		return;
	}
	if (!(pUnit->IsCombatUnit() || pUnit->IsCanAttackRanged()) ||
		(pUnit->getDomainType() != DOMAIN_LAND && !(bCoastal && pUnit->getDomainType() == DOMAIN_SEA)))
		return;
	if (plotDistance(*pUnit->plot(), cityPlot) <= CvTacticalOperationalLoop::StrikeReach(*pUnit))
		vDefenders.push_back(pUnit);
}

// Finds defenders in roster order, followed by additional eligible army members and the aircraft's carriers.
void CvTacticalOperationalLoop::GatherCityDefenders(CvTacticalAI& kTactical, const CvCity& kCity, const vector<CvUnit*>& vEndangered,
	vector<CvUnit*>& vDefenders, const SOperationalTurn& turn, const SOperationalBattlefield& field)
{
	vDefenders.clear();
	// Commands can open or close sea routes, so rebuild carrier reach for every recruitment.
	if (MOD_VOX_TACTICAL_AIR_SEARCH)
		TacticalAIHelpers::ResetAirSearchCaches();
	const CvPlot& cityPlot = *kCity.plot();
	const bool bCoastal = kCity.isCoastal();
	const vector<int> recruits = CityFightCandidates(*kTactical.m_pPlayer, kTactical.m_CurrentTurnUnits, field);
	for (size_t i = 0; i < recruits.size(); i++)
		AddCityDefender(CityRecruit(kTactical, recruits[i], &field), cityPlot, bCoastal, vEndangered, vDefenders);

	//carriers belong to operations and are never tactical units on their own, so only the ship's state is checked
	const size_t nDefenders = vDefenders.size();
	for (size_t i = 0; i < nDefenders; i++)
	{
		CvUnit* pShip = vDefenders[i]->getDomainType() == DOMAIN_AIR ? vDefenders[i]->getTransportUnit() : NULL;
		if (pShip && pShip->canUseNow() && (turn.preparedArmies.count(pShip->getArmyID()) == 0 ||
			IsFightArmyMember(*pShip, field)) &&
			std::find(vDefenders.begin(), vDefenders.end(), pShip) == vDefenders.end() &&
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
		GatherCityEndangered(kTactical, turn, *pOurCity, vEndangered, field);
		if (!vEndangered.empty())
			return OPERATIONAL_CHECK_PASSED;
		vector<CvUnit*> vDefenders;
		GatherCityDefenders(kTactical, *pOurCity, vEndangered, vDefenders, turn, field);
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
	vector<int> armyRecruits;
	if (!FindFightRecruits(kTactical, turn, field, pPlot, armyRecruits))
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
	vector<int> armyRecruits;
	if (!FindFightRecruits(kTactical, turn, field, pPlot, armyRecruits))
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
	const OperationalThreatMap& threats = bCity ? turn.cityThreats : turn.unitThreats;
	OperationalThreatMap::const_iterator threat = threats.find(make_pair(static_cast<int>(field.eOwner), field.iTargetID));
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
		if (!pUnit || !pUnit->canUseNow() || (pUnit->getArmyID() != -1 && !IsFightArmyMember(*pUnit, field)))
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
	// Sweep aircraft are candidates even when air search is off and they act greedily.
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
			if (pShip && pShip->canUseNow() &&
				(turn.preparedArmies.count(pShip->getArmyID()) == 0 || IsFightArmyMember(*pShip, field)) &&
				std::find(vUnits.begin(), vUnits.end(), pShip) == vUnits.end())
				vUnits.push_back(pShip);
		}
	}
	// Include the landers in admission even if they cannot strike from the water.
	if (bCity && turn.bMarchEnabled)
	{
		const CTacticalUnitArray strikes = kTactical.m_CurrentMoveUnits;
		kTactical.FindEmbarkedUnitsAroundTarget(pPlot, 4, &armyRecruits);
		for (size_t i = 0; i < kTactical.m_CurrentMoveUnits.size(); i++)
		{
			CvUnit* pLander = kPlayer.getUnit(kTactical.m_CurrentMoveUnits[i].GetID());
			if (pLander && std::find(vUnits.begin(), vUnits.end(), pLander) == vUnits.end())
				vUnits.push_back(pLander);
		}
		kTactical.m_CurrentMoveUnits = strikes;
	}
	if (!AdmitParticipants(kTactical, turn, field, vUnits, record))
		return;
	if (record.admission.eResult == OPERATIONAL_ADMISSION_SUBSET)
		FilterRecruits(kTactical, vUnits);
	vector<CvUnit*> vSpotters;
	for (size_t i = 0; i < vNativeSpotters.size(); i++)
		if (!IsHeldGarrison(vNativeSpotters[i]) && std::find(vUnits.begin(), vUnits.end(), vNativeSpotters[i]) != vUnits.end())
			vSpotters.push_back(vNativeSpotters[i]);
	vector<int> vUnitIDs = GetUnitIDs(vUnits);
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
		GetUnitsByID(ePlayer, vUnitIDs, vUnits);
		vector<CvUnit*> vSearch;
		for (size_t i = 0; i < vUnits.size(); i++)
			if (vUnits[i] && vUnits[i]->canUseNow() && (MOD_VOX_TACTICAL_AIR_SEARCH || vUnits[i]->getDomainType() != DOMAIN_AIR))
				vSearch.push_back(vUnits[i]);
		vUnits.swap(vSearch);
		vUnitIDs = GetUnitIDs(vUnits);
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
		if (kTactical.FindEmbarkedUnitsAroundTarget(pPlot, 4, turn.bMarchEnabled ? &armyRecruits : NULL))
		{
			// A lander need not have taken part in the search, but must have passed admission.
			if (turn.bMarchEnabled || record.admission.eResult == OPERATIONAL_ADMISSION_SUBSET)
			{
				GetUnitsByID(ePlayer, turn.bMarchEnabled ? record.admission.vEffective : vUnitIDs, vUnits);
				FilterRecruits(kTactical, vUnits);
			}
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
	GatherCityEndangered(kTactical, turn, *pCity, vEndangered, field);
	GatherCityDefenders(kTactical, *pCity, vEndangered, vUnits, turn, field);
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
			const SOperationalReinforceReach reach = ReinforceReach(kTactical, pUnit, pTarget, relevantPlots, searchPlots);
			if (reach.eResult == SOperationalReinforceReach::PASSED)
				passed.insert(iUnitID);
			else
			{
				field.reinforce.rejected[iUnitID] = reach;
				bRejected = true;
			}
		}
		if (!bRejected)
			break;
		// Only the rejected selection changed; reuse the current theater, stake and strength tallies.
		SOperationalLocalSet local;
		GatherTargetLocalSet(kTactical, turn.ePlayer, *pTarget, local, false);
		EstimateReinforce(kTactical, turn, local, field);
	}
	// A provider can select an inactive offer. Only our cities can use a group below the required strength.
	if (field.reinforce.vSelected.empty() ||
		(field.eCenter != OPERATIONAL_CENTER_OUR_CITY && field.reinforce.fSelectedStrength < field.reinforce.fNeed))
		return OPERATIONAL_CHECK_NO_REACHABLE;
	field.reinforce.bChecked = true;
	if (field.fReinforceEstimate < fBefore)
		return OPERATIONAL_CHECK_REDUCED;
	return OPERATIONAL_CHECK_PASSED;
}

// Confirms the order that produced the current March offer.
eOperationalCheckResult CvTacticalOperationalLoop::CheckArmyMarch(CvTacticalAI& kTactical, SOperationalTurn& turn,
	SOperationalBattlefield& field)
{
	SOperationalTimer timer(turn.bTiming ? &turn.performance.armyMarchCheckNs : NULL);
	vector<SOperationalPreparedArmy>::iterator order;
	CvAIOperation* pOperation;
	CvArmyAI* pArmy;
	const eOperationalCheckResult eResult = ResolveArmyMarch(kTactical, field, order, pOperation, pArmy);
	if (eResult == OPERATIONAL_CHECK_NO_REACHABLE)
		field.fReinforceEstimate = 0.0;
	return eResult;
}

// Reads a plot's danger with the defenders that would actually be there. Both plots use the same
// allocation, so the held plot and the route stop compare on one measure whether or not they are shared.
static int WithdrawStopDanger(const SOperationalTurn& turn, const CvUnit& unit, const CvPlot& stop,
	const map<int, const CvPlot*>& planned, const vector<SPlacedUnit>& incoming)
{
	CvBoardPlacement board;
	vector<SPlacedUnit> occupants;
	board.getUnitsAt(&stop, occupants);
	for (vector<SPlacedUnit>::iterator it = occupants.begin(); it != occupants.end(); )
		if (it->pUnit == &unit || (it->pUnit->getOwner() == turn.ePlayer && planned.count(it->pUnit->GetID()) > 0))
			it = occupants.erase(it);
		else
			++it;
	occupants.insert(occupants.end(), incoming.begin(), incoming.end());
	occupants.push_back(SPlacedUnit(&unit, &stop, 0, stop.needsEmbarkation(&unit) && unit.CanEverEmbark()));
	OperationalThreatMap units, cities;
	map<int, int> danger;
	CvTacticalOperationalLoop::AllocatePlotExposure(turn.ePlayer, stop, occupants, turn.aiMapped[STacticalFlavors::RISK],
		units, cities, &danger, true);
	// No entry means the plot has no danger sources.
	const map<int, int>::const_iterator it = danger.find(unit.GetID());
	return it != danger.end() ? it->second : 0;
}

// A Withdraw moves its most damaged units first, leaving civilians last so they keep their cover longest.
struct PrOperationalSortWithdraw
{
	// Ranks civilians last; otherwise the lower health share goes first.
	bool operator()(const CvUnit* lhs, const CvUnit* rhs) const
	{
		const int iCivilianLhs = lhs->IsCivilianUnit() ? 1 : 0;
		const int iCivilianRhs = rhs->IsCivilianUnit() ? 1 : 0;
		if (iCivilianLhs != iCivilianRhs)
			return iCivilianLhs < iCivilianRhs;
		return lhs->GetCurrHitPoints() * 1.0 / lhs->GetMaxHitPoints() <
			rhs->GetCurrHitPoints() * 1.0 / rhs->GetMaxHitPoints();
	}
};

// Picks legal first-turn stops in movement order, counting the other members even when they must hold.
static void GetWithdrawStops(CvTacticalAI& tactical, const SOperationalTurn& turn, const SOperationalBattlefield& field,
	const vector<CvUnit*>& recruits, bool bFormationWithdraw, vector<SOperationalMarch>& marches)
{
	marches.clear();
	vector<CvUnit*> ordered(recruits);
	if (bFormationWithdraw)
		std::stable_sort(ordered.begin(), ordered.end(), PrOperationalSortWithdraw());
	map<int, const CvPlot*> planned;
	map<int, vector<SPlacedUnit> > arrivals;
	CvPlayer& player = GET_PLAYER(turn.ePlayer);
	for (size_t i = 0; i < ordered.size(); i++)
	{
		CvUnit& unit = *ordered[i];
		const int iDestination = field.aiWithdrawDestination[unit.getDomainType() == DOMAIN_SEA ? 1 : 0];
		CvPlot* pDestination = iDestination < 0 ? NULL : GC.getMap().plotByIndex(iDestination);
		CvArmyAI* pArmy = bFormationWithdraw ? player.getArmyAI(unit.getArmyID()) : NULL;
		const CvAIOperation* pOperation = pArmy ? player.getAIOperation(pArmy->GetOperationID()) : NULL;
		const bool bArmyRoute = pArmy && pOperation;
		CvPlot* pStop = CvTacticalOperationalLoop::CanMarch(tactical, &unit, pDestination, true, bArmyRoute);
		// Replay the checked stop exactly, with ocean limits only for a live army route.
		const int iMoveFlags = bFormationWithdraw && pStop ?
			(ExactMarchFlags(unit, *pStop) & ~CvUnit::MOVEFLAG_APPROX_TARGET_NATIVE_DOMAIN) |
				(bArmyRoute ? ArmyMarchRouteFlags(*pArmy) : 0) : 0;
		if (bArmyRoute)
		{
			const SOperationalFormation formation(*pArmy, *pOperation, &planned);
			pStop = FormationPathStop(unit, unit.GetLastPath(), pStop, formation);
			const int iHoldDanger = WithdrawStopDanger(turn, unit, *unit.plot(), planned, arrivals[unit.plot()->GetPlotIndex()]);
			const int iDangerLimit = unit.IsCanAttack() && unit.AI_getUnitAIType() != UNITAI_CITY_BOMBARD ? unit.GetCurrHitPoints() / 2 : 0;
			if (pStop)
			{
				const vector<SPlacedUnit>& incoming = arrivals[pStop->GetPlotIndex()];
				const int iDanger = WithdrawStopDanger(turn, unit, *pStop, planned, incoming);
				if (iDanger > iDangerLimit && (iDanger >= iHoldDanger || iDanger >= unit.GetCurrHitPoints()))
					pStop = NULL;
			}
		}

		if (!pStop || pStop == unit.plot() || (bFormationWithdraw && !FitsMarchStop(unit, pStop, arrivals[pStop->GetPlotIndex()])))
			continue;
		if (bFormationWithdraw)
		{
			arrivals[pStop->GetPlotIndex()].push_back(SPlacedUnit(&unit, pStop, 0, pStop->needsEmbarkation(&unit) && unit.CanEverEmbark()));
			planned[unit.GetID()] = pStop;
		}
		const SOperationalMarch march = {unit.GetID(), pDestination ? iDestination : pStop->GetPlotIndex(), pStop->GetPlotIndex(), iMoveFlags};
		marches.push_back(march);
	}
	if (bFormationWithdraw)
		std::sort(marches.begin(), marches.end());
}

// Finds each domain's destination and scores the legal first-turn stops, keeping recruits in ID order.
eOperationalCheckResult CvTacticalOperationalLoop::CheckWithdraw(CvTacticalAI& kTactical, const SOperationalTurn& turn,
	SOperationalBattlefield& field, vector<SOperationalMarch>& marches, bool& bFormationWithdraw)
{
	marches.clear();
	bFormationWithdraw = false;
	// A permitted-menu host can request any offer, so reject a disabled switch or our own city before gathering.
	if (turn.weights.afConstant[OPERATIONAL_CONSTANT_WITHDRAW_ENABLED] == 0.0 ||
		field.eCenter == OPERATIONAL_CENTER_OUR_CITY)
		return OPERATIONAL_CHECK_NO_RECRUITS;

	CvPlot* pTarget = GC.getMap().plotByIndex(field.iPlotIndex);
	if (!pTarget)
		return OPERATIONAL_CHECK_TARGET_GONE;

	SOperationalLocalSet local;
	GatherTargetLocalSet(kTactical, turn.ePlayer, *pTarget, local, false,
		field.eCenter == OPERATIONAL_CENTER_ENEMY_UNIT);
	vector<CvUnit*> vRecruits;
	GatherWithdrawRecruits(kTactical, turn, field, local, vRecruits);

	// One destination per domain that has recruits; domains without recruits stay unset.
	field.aiWithdrawDestination[0] = -1;
	field.aiWithdrawDestination[1] = -1;
	if (vRecruits.empty())
		return OPERATIONAL_CHECK_NO_RECRUITS;
	bool abDomain[2] = {false, false};
	for (size_t i = 0; i < vRecruits.size(); i++)
	{
		abDomain[vRecruits[i]->getDomainType() == DOMAIN_SEA ? 1 : 0] = true;
		bFormationWithdraw |= vRecruits[i]->getArmyID() != -1;
	}
	// A flood leaves only a plot its domain can stand on, so a land flood starts at a water battlefield only
	// from a recruit's plot, and a sea flood at an inland battlefield likewise.
	const bool abSuitable[2] = {!pTarget->isWater(),
		pTarget->isWater() || (pTarget->isCity() && pTarget->getPlotCity() && pTarget->getPlotCity()->isCoastal())};
	for (int iDomain = 0; iDomain < 2; iDomain++)
	{
		if (!abDomain[iDomain])
			continue;
		CvPlot* pStart = abSuitable[iDomain] ? pTarget : NULL;
		if (!pStart)
		{
			// Otherwise flood from the domain's first endangered recruit, or its first recruit when none is
			// endangered. Recruits come sorted by ascending ID.
			CvUnit* pEndangered = NULL;
			CvUnit* pFirst = NULL;
			for (size_t i = 0; i < vRecruits.size(); i++)
			{
				if ((vRecruits[i]->getDomainType() == DOMAIN_SEA ? 1 : 0) != iDomain || !vRecruits[i]->plot())
					continue;
				if (!pFirst)
					pFirst = vRecruits[i];
				if (turn.endangeredUnits.count(vRecruits[i]->GetID()) > 0)
				{
					pEndangered = vRecruits[i];
					break;
				}
			}
			CvUnit* pFallback = pEndangered ? pEndangered : pFirst;
			if (pFallback)
				pStart = pFallback->plot();
		}
		if (pStart)
			field.aiWithdrawDestination[iDomain] = WithdrawDestination(kTactical, turn.ePlayer, *pStart,
				iDomain == 0 ? DOMAIN_LAND : DOMAIN_SEA);
	}

	GetWithdrawStops(kTactical, turn, field, vRecruits, bFormationWithdraw, marches);
	const double fBefore = field.fWithdrawEstimate;
	field.fWithdrawExposure = std::max(0.0, ExposureSaved(turn, marches, true, bFormationWithdraw));
	field.fWithdrawEstimate = WithdrawPriority(turn, field, field.fWithdrawExposure, field.fWithdrawShare);
	if (field.fWithdrawEstimate <= 0.0)
		return OPERATIONAL_CHECK_NO_REACHABLE;
	if (field.fWithdrawEstimate < fBefore)
		return OPERATIONAL_CHECK_REDUCED;
	return OPERATIONAL_CHECK_PASSED;
}

// One reach contract for the checked native selection and any provider-added recruits.
SOperationalReinforceReach CvTacticalOperationalLoop::ReinforceReach(CvTacticalAI& kTactical, CvUnit* pUnit, CvPlot* pTarget,
	const ReachablePlots& relevantPlots, map<int, ReachablePlots>& searchPlots)
{
	if (!pUnit || !pUnit->plot())
		return SOperationalReinforceReach::INITIAL_REACH;
	const int iUnitPlot = pUnit->plot()->GetPlotIndex();
	if (IsHeldGarrison(pUnit))
	{
		const int iDomain = pUnit->getDomainType();
		if (searchPlots.find(iDomain) == searchPlots.end())
			searchPlots[iDomain] = SearchReachPlots(kTactical.m_pPlayer->GetID(), pTarget, pUnit->getDomainType());
		const ReachablePlots& reachable = searchPlots[iDomain];
		return reachable.find(iUnitPlot) != reachable.end() ? SOperationalReinforceReach::PASSED : SOperationalReinforceReach::INITIAL_REACH;
	}
	if (relevantPlots.find(iUnitPlot) == relevantPlots.end())
		return SOperationalReinforceReach::INITIAL_REACH;

	// Don't run away if there's other work to do (eventually handled by ExecuteAttackWithUnits).
	const vector<pair<CvPlot*, bool> > altTargets = TacticalAIHelpers::GetTargetsInRange(pUnit, false, false);
	for (size_t i = 0; i < altTargets.size(); i++)
		if (plotDistance(*altTargets[i].first, *pTarget) > TACTICAL_COMBAT_MAX_TARGET_DISTANCE)
			return SOperationalReinforceReach(SOperationalReinforceReach::COMPETING_TARGET, altTargets[i].first->GetPlotIndex());
	return CanMarch(kTactical, pUnit, pTarget) ? SOperationalReinforceReach::PASSED : SOperationalReinforceReach::MARCH;
}

// Compares each unit's state with its starting plot and fills the command's moved list. A unit counts as
// moved when it is gone, is dying, has no plot, or left the plot it started on.
static void RecordMovedUnits(const CvPlayer& kPlayer, const vector<int>& vUnitIDs, const map<int, int>& startPlots,
	SOperationalCommandRecord& record)
{
	record.vMoved.clear();
	for (size_t i = 0; i < vUnitIDs.size(); i++)
	{
		const int iUnitID = vUnitIDs[i];
		const CvUnit* pUnit = kPlayer.getUnit(iUnitID);
		map<int, int>::const_iterator start = startPlots.find(iUnitID);
		if (!pUnit || pUnit->isDelayedDeath() || !pUnit->plot() ||
			(start != startPlots.end() && pUnit->plot()->GetPlotIndex() != start->second))
			record.vMoved.push_back(iUnitID);
	}
	record.bMoved = !record.vMoved.empty();
}

// Reports a command's moved units on normal return and while a stopped search unwinds. It reserves the
// moved list's space up front, so the destructor never allocates during unwinding.
struct SOperationalMovedUnitsScan
{
	// The player that owns the scanned units.
	const CvPlayer& kPlayer;
	// The copy of the unit IDs the command admitted.
	vector<int> vUnitIDs;
	// Each scanned unit's starting plot, keyed by unit ID.
	const map<int, int>& startPlots;
	// The command record that receives the moved units.
	SOperationalCommandRecord& record;
	// Copies the admitted IDs, borrows the start plots and the record, which must outlive the guard, and
	// reserves room for that many moved units. The scan clears the list without releasing its capacity, so it cannot
	// allocate.
	SOperationalMovedUnitsScan(const CvPlayer& kOwner, const vector<int>& vIDs, const map<int, int>& start,
		SOperationalCommandRecord& commandRecord)
		: kPlayer(kOwner), vUnitIDs(vIDs), startPlots(start), record(commandRecord)
	{
		record.vMoved.reserve(vUnitIDs.size());
	}
	// Reports the moved units on every exit, including a thrown search.
	~SOperationalMovedUnitsScan()
	{
		RecordMovedUnits(kPlayer, vUnitIDs, startPlots, record);
	}
};

// Restores the processed flags of a Withdraw's recruits on every exit, including a refused admission and
// an unwinding search.
struct SOperationalWithdrawFlags
{
	// The player that owns the saved units.
	CvPlayer& kPlayer;
	// The IDs of recruits that were processed before the command.
	const vector<int>& vFlagged;
	// Borrows the saved unit IDs and the player that owns them.
	SOperationalWithdrawFlags(CvPlayer& kOwner, const vector<int>& vIDs) : kPlayer(kOwner), vFlagged(vIDs) {}
	// Processes each saved unit again when it still exists and is not dying.
	~SOperationalWithdrawFlags()
	{
		for (size_t i = 0; i < vFlagged.size(); i++)
		{
			CvUnit* pUnit = kPlayer.getUnit(vFlagged[i]);
			if (pUnit && !pUnit->isDelayedDeath())
				pUnit->SetTurnProcessed(true);
		}
	}
};

// Adds another destination's attempts; its geometry and request replace the first only on invalid flavors.
struct SOperationalMarchReportMerge
{
	SOperationalEngagementReport& target;
	SOperationalEngagementReport& source;
	// Borrows the command report and the report to add.
	SOperationalMarchReportMerge(SOperationalEngagementReport& targetReport, SOperationalEngagementReport& sourceReport)
		: target(targetReport), source(sourceReport) {}
	// Adds any completed work even if the search was stopped.
	~SOperationalMarchReportMerge()
	{
		target.attempts.insert(target.attempts.end(), source.attempts.begin(), source.attempts.end());
		target.iAttempts += source.iAttempts;
		target.searchNs += source.searchNs;
		target.bSuccess = target.bSuccess || source.bSuccess;
		if (source.bInvalidFlavors && !target.bInvalidFlavors)
		{
			target.zonePlots = source.zonePlots;
			target.iRepresentativeZone = source.iRepresentativeZone;
			target.flavors = source.flavors;
			target.iAggressionRisk = source.iAggressionRisk;
			target.situation = source.situation;
			target.eAggLvl = source.eAggLvl;
			target.flavorRequest = source.flavorRequest;
			target.requestedFlavors = source.requestedFlavors;
			target.bInvalidFlavors = true;
		}
	}
};

// Validates the selected army's order, then admits and positions units by destination.
void CvTacticalOperationalLoop::ExecuteMarch(CvTacticalAI& kTactical, const SOperationalTurn& turn,
	const SOperationalBattlefield& field, const vector<SOperationalMarch>& withdraw, SOperationalCommandRecord& record,
	bool bFormationWithdraw, bool& bArmyMarchOrderChanged)
{
	const bool bWithdraw = record.eKind == OPERATIONAL_KIND_WITHDRAW;
	const bool bArmyMarch = record.eKind == OPERATIONAL_KIND_ARMY_MARCH;
	PlayerTypes eCounterpart = field.eCenter == OPERATIONAL_CENTER_OUR_CITY ? NO_PLAYER : field.eOwner;
	CvPlayer& kPlayer = *kTactical.m_pPlayer;
	vector<SOperationalPreparedArmy>::iterator order;
	if (bArmyMarch)
	{
		CvAIOperation* pOperation;
		CvArmyAI* pArmy;
		if (ResolveArmyMarch(kTactical, field, order, pOperation, pArmy) != OPERATIONAL_CHECK_PASSED)
		{
			record.admission.eResult = OPERATIONAL_ADMISSION_REFUSED;
			return;
		}
		const bool bAbort = pOperation->GetOperationState() == AI_OPERATION_STATE_ABORTED || pOperation->ShouldAbort();
		if (bAbort || !MatchesPreparedArmyOrder(*pOperation, *pArmy, *order))
		{
			bArmyMarchOrderChanged = !bAbort && UpdatePreparedWaypoint(*pOperation, *pArmy, *order);
			if (!bArmyMarchOrderChanged)
				RemovePreparedOperation(kTactical, order, *pOperation, pArmy);
			record.admission.eResult = OPERATIONAL_ADMISSION_REFUSED;
			return;
		}
		eCounterpart = pOperation->GetEnemy();
	}
	kTactical.m_CurrentMoveUnits.setCurrentTacticalMove(bArmyMarch ? AI_TACTICAL_OPERATION :
		bWithdraw ? AI_TACTICAL_WITHDRAW : AI_TACTICAL_REINFORCE);

	vector<CvUnit*> vUnits;
	vector<int> vFlagged;
	SOperationalWithdrawFlags restoreFlags(kPlayer, vFlagged);
	if (bWithdraw)
	{
		for (size_t i = 0; i < withdraw.size(); i++)
		{
			CvUnit* pUnit = kPlayer.getUnit(withdraw[i].iUnitID);
			if (!pUnit || !pUnit->plot() || pUnit->isDelayedDeath())
				continue;
			vUnits.push_back(pUnit);
			if (pUnit->TurnProcessed())
			{
				vFlagged.push_back(pUnit->GetID());
				pUnit->SetTurnProcessed(false);
			}
		}
	}
	else
		for (size_t i = 0; i < field.reinforce.vSelected.size(); i++)
		{
			CvUnit* pUnit = kPlayer.getUnit(field.reinforce.vSelected[i]);
			if (pUnit && pUnit->plot() && pUnit->canUseNow() && (!bArmyMarch || pUnit->getArmyID() == field.iTargetID))
				vUnits.push_back(pUnit);
		}

	const bool bAdmitted = AdmitParticipants(kTactical, turn, field, vUnits, record);
	record.vSelected = record.admission.vEffective;
	map<int, int> startPlots;
	map<int, vector<int> > groups;
	for (size_t i = 0; i < vUnits.size(); i++)
	{
		const int iUnitID = vUnits[i]->GetID();
		startPlots[iUnitID] = vUnits[i]->plot()->GetPlotIndex();
		if (!bWithdraw)
			groups[field.iPlotIndex].push_back(iUnitID);
	}
	vector<SOperationalMarch> checkedSubset;
	const vector<SOperationalMarch>* pStops = bFormationWithdraw ? &withdraw : NULL;
	if (bFormationWithdraw && vUnits.size() != withdraw.size())
	{
		// Omitted members hold when the remaining detachment checks its formation again.
		GetWithdrawStops(kTactical, turn, field, vUnits, true, checkedSubset);
		pStops = &checkedSubset;
	}
	if (bWithdraw)
	{
		const vector<SOperationalMarch>& legal = pStops ? *pStops : withdraw;
		vector<SOperationalMarch> admitted;
		record.vSelected.clear();
		for (size_t i = 0; i < vUnits.size(); i++)
		{
			const int iUnitID = vUnits[i]->GetID();
			const SOperationalMarch* march = FindWithdrawOrder(legal, iUnitID);
			if (!march)
				continue;
			admitted.push_back(*march);
			groups[march->iDestination].push_back(iUnitID);
			record.vSelected.push_back(iUnitID);
			record.vEndangered.push_back(make_pair(iUnitID, false));
		}

		// Omitted recruits remain on their original plots. Compare the remaining moves before the first order runs.
		record.fWithdrawShare = field.fWithdrawShare;
		record.fWithdrawExposure = field.fWithdrawExposure;
		if (admitted.size() != withdraw.size() || admitted.empty())
		{
			// Admission can reorder the search roster. Keep the same ID tie break when projecting its stops.
			std::sort(admitted.begin(), admitted.end());
			record.fWithdrawExposure = std::max(0.0, ExposureSaved(turn, admitted, true, bFormationWithdraw));
		}
		record.fEstimate = WithdrawPriority(turn, field, record.fWithdrawExposure, record.fWithdrawShare);
	}
	if (!bAdmitted)
		return;

	SOperationalMovedUnitsScan movedUnits(kPlayer, record.vSelected, startPlots, record);
	if (field.eCenter == OPERATIONAL_CENTER_CAMP)
	{
		// A fogged camp uses the normal march stop without a spotting or attack search.
		CvPlot* pCamp = GC.getMap().plotByIndex(field.iPlotIndex);
		for (size_t i = 0; i < vUnits.size(); i++)
		{
			CvUnit* pUnit = vUnits[i];
			if (IsHeldGarrison(pUnit))
				continue;
			CvPlot* pStop = CanMarch(kTactical, pUnit, pCamp);
			if (pStop && pStop != pUnit->plot())
				kTactical.ExecuteMoveToPlot(pUnit, pStop, true, ExactMarchFlags(*pUnit, *pStop));
		}
		return;
	}
	// An explicit subset also limits the auxiliary recon pool used by Reinforce.
	if (bArmyMarch || record.admission.eResult == OPERATIONAL_ADMISSION_SUBSET)
		FilterRecruits(kTactical, vUnits);

	for (map<int, vector<int> >::const_iterator it = groups.begin(); it != groups.end(); ++it)
	{
		CvPlot* pDestination = GC.getMap().plotByIndex(it->first);
		GetUnitsByID(kPlayer.GetID(), it->second, vUnits);
		if (!pDestination || vUnits.empty())
			continue;

		if (!record.bSearchRan)
		{
			record.bSearchRan = true;
			const eOperationalPositionResult result = PositionUnitsAroundTarget(kTactical, vUnits, pDestination, eCounterpart,
				record.engagement, turn, record, pStops, bArmyMarch ? &order->vMarchCorridor : NULL);
			if (bArmyMarch)
				record.bSearchRan = record.engagement.iAttempts > 0;
			if (result == OPERATIONAL_POSITION_YIELDED)
				return;
		}
		else
		{
			SOperationalEngagementReport search;
			SOperationalMarchReportMerge merge(record.engagement, search);
			PositionUnitsAroundTarget(kTactical, vUnits, pDestination, eCounterpart, search, turn, record,
				pStops, bArmyMarch ? &order->vMarchCorridor : NULL);
		}
	}
	if (bWithdraw)
		MarkDispositions(record.engagement, record);
}

// Admits the one claimant and issues a single move mission. Native entry pays the bounty.
void CvTacticalOperationalLoop::ExecuteCampClaim(CvTacticalAI& kTactical, const SOperationalTurn& turn,
	const SOperationalBattlefield& field, SOperationalCommandRecord& record)
{
	CvPlayer& kPlayer = *kTactical.m_pPlayer;
	vector<CvUnit*> vUnits;
	CvUnit* pUnit = kPlayer.getUnit(field.iCampClaimant);
	if (pUnit && pUnit->canUseNow())
		vUnits.push_back(pUnit);
	if (!AdmitParticipants(kTactical, turn, field, vUnits, record))
		return;
	pUnit = vUnits.front();
	const int iUnit = pUnit->GetID();
	record.vSelected.push_back(iUnit);
	CvPlot* pCamp = GC.getMap().plotByIndex(field.iPlotIndex);
	CvPlot* pStop = record.eKind == OPERATIONAL_KIND_REINFORCE
		? CanMarch(kTactical, pUnit, pCamp, false, false, field.iCampTurns) : pCamp;
	if (!pStop || pStop == pUnit->plot())
		return;
	map<int, int> startPlots;
	startPlots[iUnit] = pUnit->plot()->GetPlotIndex();
	SOperationalMovedUnitsScan movedUnits(kPlayer, record.vSelected, startPlots, record);
	pUnit->PushMission(CvTypes::getMISSION_MOVE_TO(), pStop->getX(), pStop->getY(),
		CvUnit::MOVEFLAG_NO_EMBARK | CvUnit::MOVEFLAG_AI_ABORT_IN_DANGER, false, false, MISSIONAI_TACTMOVE, pStop);
	if (pUnit->plot()->GetPlotIndex() != startPlots[iUnit])
	{
		kTactical.m_CurrentMoveUnits.setCurrentTacticalMove(AI_TACTICAL_BARBARIAN_CAMP);
		kTactical.UnitProcessed(iUnit);
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
	if (executed.eCenter != OPERATIONAL_CENTER_ENEMY_UNIT)
		return false;
	if (candidate.bCanAttackOurUnits || candidate.bNearOurCity || !candidate.fightArmies.empty())
		return false;
	return CoversFight(executed, candidate);
}

// The search's reach around any executed center, without the exemptions that keep a fight open for its own command.
bool CvTacticalOperationalLoop::CoversFight(const SOperationalBattlefield& executed, const SOperationalBattlefield& candidate)
{
	if (candidate.eCenter != OPERATIONAL_CENTER_ENEMY_UNIT || candidate.iID == executed.iID)
		return false;
	if (OperationalOfferIsClosed(candidate.eFight))
		return false;

	//land zones have id > 0, water zones id < 0. opposite signs means domain mismatch
	if ((executed.iZoneID > 0 && candidate.iZoneID < 0) || (executed.iZoneID < 0 && candidate.iZoneID > 0))
		return false;
	CvPlot* pExecuted = GC.getMap().plotByIndex(executed.iPlotIndex);
	CvPlot* pCandidate = GC.getMap().plotByIndex(candidate.iPlotIndex);
	return pExecuted && pCandidate && plotDistance(*pExecuted, *pCandidate) <= 2;
}

// V_O counts city progress only when the city falls this turn. V_C is city danger at our city and the target's relief
// at a unit fight. Covered fights add only their largest destruction, and their exposure stays with their own fights.
bool CvTacticalOperationalLoop::GatherSituation(const SOperationalTurn& turn, const SOperationalCommandRecord& command,
	SOperationalSituation& situation)
{
	if (command.iBattlefield < 0 || command.iBattlefield >= static_cast<int>(turn.battlefields.size()))
		return false;
	const SOperationalBattlefield& field = turn.battlefields[command.iBattlefield];
	const SOperationalStake& stake = command.stake;
	const bool bOurCity = field.eCenter == OPERATIONAL_CENTER_OUR_CITY;
	situation.fCapture = field.eCenter == OPERATIONAL_CENTER_ENEMY_CITY && stake.fFallShare >= 1.0 ? stake.afValue[4] : 0.0;
	situation.fSiege = bOurCity ? stake.afValue[4] : stake.afValue[3];
	double fCovered = 0.0;
	for (size_t i = 0; i < turn.battlefields.size(); i++)
		if (CoversFight(field, turn.battlefields[i]))
			fCovered = std::max(fCovered, turn.battlefields[i].stake.afValue[0]);
	situation.fKill = stake.afValue[0] + stake.afValue[5] + fCovered;
	// The battlefield's strength share, which the exposure.split switch does not touch.
	situation.fUncovered = stake.afValue[1] * (1.0 - std::min(1.0, std::max(0.0, field.fExposureShare)));
	const CvPlot* pPlot = GC.getMap().plotByIndex(field.iPlotIndex);
	situation.bOurLand = bOurCity || (pPlot && GroundSide(turn.ePlayer, pPlot->getOwner()) == groundHoldsOurs);
	return true;
}

// The executed Reinforce's counted sets are from its last estimate, before it ran.
bool CvTacticalOperationalLoop::RetiresReinforce(const SOperationalBattlefield& executed, const SOperationalBattlefield& candidate)
{
	if (executed.eCenter == OPERATIONAL_CENTER_CAMP && !executed.bCampFogged)
		return false;
	if (candidate.iID == executed.iID || candidate.eCenter == OPERATIONAL_CENTER_OUR_CITY ||
		candidate.eCenter == OPERATIONAL_CENTER_ARMY)
		return false;
	if (candidate.eReinforce != OPERATIONAL_OFFER_READY && candidate.eReinforce != OPERATIONAL_OFFER_INACTIVE)
		return false;
	const pair<int, int> key(static_cast<int>(candidate.eOwner), candidate.iTargetID);
	const set<pair<int, int> >& counted = candidate.eCenter == OPERATIONAL_CENTER_ENEMY_CITY ? executed.reinforce.countedCities :
		executed.reinforce.countedUnits;
	return counted.find(key) != counted.end();
}

// Records enemy observations, city battlefield owners and friendly positions for the command diff.
void CvTacticalOperationalLoop::SnapshotBoard(const SOperationalTurn& turn, SOperationalBoard& board)
{
	SnapshotEnemies(turn, board);
	board.cityOwners.clear();
	board.ourUnitPlots.clear();
	board.ourArmyUnits.clear();
	int iLoop = 0;
	for (size_t i = 0; i < turn.battlefields.size(); i++)
	{
		const eOperationalCenter eCenter = turn.battlefields[i].eCenter;
		if (eCenter != OPERATIONAL_CENTER_ENEMY_CITY && eCenter != OPERATIONAL_CENTER_OUR_CITY)
			continue;
		CvPlot* pPlot = GC.getMap().plotByIndex(turn.battlefields[i].iPlotIndex);
		board.cityOwners[turn.battlefields[i].iPlotIndex] = pPlot && pPlot->getPlotCity() ? pPlot->getPlotCity()->getOwner() : NO_PLAYER;
	}
	CvPlayer& kPlayer = GET_PLAYER(turn.ePlayer);
	for (CvUnit* pUnit = kPlayer.firstUnit(&iLoop); pUnit != NULL; pUnit = kPlayer.nextUnit(&iLoop))
		if (!pUnit->isDelayedDeath() && pUnit->plot())
		{
			board.ourUnitPlots[pUnit->GetID()] = pUnit->plot()->GetPlotIndex();
			// A survivor can become available without changing plots.
			if (turn.bMarchEnabled && pUnit->getArmyID() != -1)
				board.ourArmyUnits.insert(pUnit->GetID());
		}
}

// Compares positions and city ownership, including friendly moves that used the unit's last move.
void CvTacticalOperationalLoop::DiffBoard(const SOperationalBoard& before, const SOperationalBoard& after, SOperationalChanges& changes)
{
	changes.vEnemyPlots.clear();
	changes.vEnemyCityPlots.clear();
	changes.vCapturedPlots.clear();
	changes.vFriendlyMoves.clear();
	changes.vReleasedUnits.clear();
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
	for (map<int, int>::const_iterator it = before.visibleEnemyCityOwners.begin(); it != before.visibleEnemyCityOwners.end(); ++it)
		if (after.visibleEnemyCityOwners.count(it->first) == 0)
			changes.vEnemyCityPlots.push_back(it->first);
	for (map<int, int>::const_iterator it = after.visibleEnemyCityOwners.begin(); it != after.visibleEnemyCityOwners.end(); ++it)
	{
		const map<int, int>::const_iterator then = before.visibleEnemyCityOwners.find(it->first);
		if (then == before.visibleEnemyCityOwners.end() || then->second != it->second)
			changes.vEnemyCityPlots.push_back(it->first);
	}
	for (map<int, int>::const_iterator it = after.cityOwners.begin(); it != after.cityOwners.end(); ++it)
	{
		map<int, int>::const_iterator then = before.cityOwners.find(it->first);
		if (then != before.cityOwners.end() && then->second != it->second)
			changes.vCapturedPlots.push_back(it->first);
	}
	for (map<int, int>::const_iterator it = before.ourUnitPlots.begin(); it != before.ourUnitPlots.end(); ++it)
		if (after.ourUnitPlots.count(it->first) == 0)
		{
			SOperationalFriendlyMove move = {it->first, it->second, -1};
			changes.vFriendlyMoves.push_back(move);
		}
	for (map<int, int>::const_iterator it = after.ourUnitPlots.begin(); it != after.ourUnitPlots.end(); ++it)
	{
		map<int, int>::const_iterator then = before.ourUnitPlots.find(it->first);
		// A release can wake the same offers as a friendly arrival.
		const bool bReleased = before.ourArmyUnits.count(it->first) != 0 && after.ourArmyUnits.count(it->first) == 0;
		if (bReleased)
			changes.vReleasedUnits.push_back(it->first);
		if (!bReleased && then != before.ourUnitPlots.end() && then->second == it->second)
			continue;
		SOperationalFriendlyMove move = {it->first, then == before.ourUnitPlots.end() ? -1 : then->second, it->second};
		changes.vFriendlyMoves.push_back(move);
	}
}

// Tests a changed plot against the same distance bound as the Reinforce roster scan.
static bool WithinRecruitRange(int iPlot, const CvPlot& kTarget, int iRange)
{
	const CvPlot* pPlot = iPlot >= 0 ? GC.getMap().plotByIndex(iPlot) : NULL;
	return pPlot && plotDistance(*pPlot, kTarget) <= iRange;
}

// Clears temporary failures when the march area or a watched competing target changes.
void CvTacticalOperationalLoop::RetryReinforce(const CvPlot& kTarget, int iRange, const SOperationalChanges& changes,
	SOperationalBattlefield& field)
{
	SOperationalReinforce& reinforce = field.reinforce;
	bool bAreaChanged = false;
	for (size_t i = 0; i < changes.vEnemyPlots.size() && !bAreaChanged; i++)
		bAreaChanged = WithinRecruitRange(changes.vEnemyPlots[i], kTarget, iRange);
	for (size_t i = 0; i < changes.vFriendlyMoves.size() && !bAreaChanged; i++)
		bAreaChanged = WithinRecruitRange(changes.vFriendlyMoves[i].iFrom, kTarget, iRange) ||
			WithinRecruitRange(changes.vFriendlyMoves[i].iTo, kTarget, iRange);
	for (map<int, SOperationalReinforceReach>::iterator it = reinforce.rejected.begin(); it != reinforce.rejected.end(); )
	{
		const SOperationalReinforceReach& reach = it->second;
		const bool bCompetingPlotChanged = reach.eResult == SOperationalReinforceReach::COMPETING_TARGET &&
			std::find(changes.vEnemyPlots.begin(), changes.vEnemyPlots.end(), reach.iCompetingPlot) != changes.vEnemyPlots.end();
		if (reach.eResult != SOperationalReinforceReach::INITIAL_REACH && (bAreaChanged || bCompetingPlotChanged))
		{
			reinforce.rejected.erase(it++);
			field.ResetReinforceCheck();
		}
		else
			++it;
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
	for (size_t i = 0; i < changes.vFriendlyMoves.size(); i++)
	{
		if (changes.vFriendlyMoves[i].iTo < 0)
			continue;
		const CvUnit* pUnit = kPlayer.getUnit(changes.vFriendlyMoves[i].iUnitID);
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

// Values the enemy's lost health and retained share without an upgrade premium.
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

// The ground the target holds, in plot weight: the score with it less the score without it.
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
void CvTacticalOperationalLoop::BeginCommand(CvTacticalAI& /*kTactical*/, const SOperationalCommandRecord& /*command*/, int /*iZoneID*/)
{
}

// The game records no per-command work.
void CvTacticalOperationalLoop::EndCommand(CvTacticalAI& /*kTactical*/)
{
}

// Gives each callback an identity before the host stamps its branch revision.
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
		identity.iOperation = pCommand->iOperation;
		identity.iArmy = pCommand->iArmy;
	}
	StampDecision(identity);
	return identity;
}

// The same stable scan as the original queue, including every OfferOutranks tie break.
int CvTacticalOperationalLoop::NativeCommandChoice(const SOperationalChoiceRequest& request)
{
	int iBest = -1;
	for (size_t i = 0; i < request.offers.size(); i++)
	{
		const SOperationalSelectableOffer& offer = request.offers[i];
		const SOperationalBattlefield& field = offer.field;
		if (!OperationalSupportsSlot(field.eCenter, offer.eSlot))
			continue;
		if (iBest >= 0)
		{
			const SOperationalSelectableOffer& best = request.offers[iBest];
			if (!OperationalOfferBefore(field, offer.eSlot, offer.iOffer, best.field, best.eSlot, best.iOffer))
				continue;
		}
		iBest = static_cast<int>(i);
	}
	return iBest < 0 ? -1 : request.offers[iBest].iOffer;
}

// Recomputes only the chosen group's contribution; the checked offer keeps its execution priority.
void CvTacticalOperationalLoop::AdmittedReinforcement(const vector<SOperationalCandidate>& candidates, eOperationalCenter eCenter, bool bNeedCapturer,
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
	out.fTurnsToContact = ReinforceTurnsToContact(eCenter, out);
}

// Admission is resolved once, before spotting, air, search or marching can spend a recruit.
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
	// Retain computed native admission facts without repeating strength, reach or search setup.
	decision.vEffective = request.vNative;
	const bool bReinforce = record.eKind == OPERATIONAL_KIND_REINFORCE &&
		(field.eCenter != OPERATIONAL_CENTER_CAMP || field.bCampFogged);
	if (bReinforce)
		decision.effectiveReinforce = field.reinforce;
	// Query capability once for this command and keep it through admission dispatch.
	const bool bHasAdmission = HasAdmission();
	if (!bHasAdmission)
	{
		decision.eResult = vUnits.empty() ? OPERATIONAL_ADMISSION_NO_ELIGIBLE : OPERATIONAL_ADMISSION_ABSENT;
		return !vUnits.empty();
	}
	CvPlot* pTarget = GC.getMap().plotByIndex(field.iPlotIndex);
	// Supplied adapters rebuild eligibility without the hidden-state native filters.
	const bool bPermittedMenu = UsesPermittedMenu();
	if (bPermittedMenu)
		PrepareAdmission(request);
	else if (bReinforce)
	{
		const CvCity* pCity = pTarget->getPlotCity();
		GatherReinforceCandidates(kTactical, field, *pTarget, pCity != NULL || !pTarget->isWater(),
			pCity ? pCity->isCoastal() : pTarget->isWater(), request.candidates);
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
				ReinforceReach(kTactical, pUnit, pTarget, relevantPlots, searchPlots).eResult == SOperationalReinforceReach::PASSED)
				reached.push_back(iUnitID);
			else
				decision.vFailedReach.push_back(iUnitID);
		}
		decision.vEffective.swap(reached);
	}
	// Added fight candidates must belong to native's validated reach set.
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
			AdmittedReinforcement(request.candidates, field.eCenter, field.eCenter == OPERATIONAL_CENTER_ENEMY_CITY && !local.bCapturer,
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

// Prevents native auxiliary actions from recruiting outside a provider's admitted group.
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

// DLL decisions already carry the shared loop's synchronous revision.
void CvTacticalOperationalLoop::StampDecision(SOperationalDecisionIdentity& /*identity*/) {}

// The DLL has no supplied operational provider.
bool CvTacticalOperationalLoop::HasOperationalProvider() { return false; }

// The DLL always uses its native queue.
bool CvTacticalOperationalLoop::UsesPermittedMenu() { return false; }

// Confirmed execution feedback is a simulator-only provider seam.
void CvTacticalOperationalLoop::ReportExecution(int /*iOffer*/, int /*iResult*/, const SOperationalCommandRecord* /*pCommand*/) {}

// The DLL retains its native admission candidates.
void CvTacticalOperationalLoop::PrepareAdmission(SOperationalAdmissionRequest& /*request*/) {}

// The DLL keeps native admission without building an unused observation.
bool CvTacticalOperationalLoop::HasAdmission() { return false; }

// The DLL always uses the shared native command decoder.
int CvTacticalOperationalLoop::ChooseCommand(const SOperationalChoiceRequest& request)
{
	return request.iNative;
}

// The DLL has no optional admission override.
bool CvTacticalOperationalLoop::AdmitCommand(const SOperationalAdmissionRequest& /*request*/, SOperationalAdmissionReply& /*reply*/)
{
	return false;
}

// The DLL always keeps the geographic blend and command-kind row.
STacticalFlavors CvTacticalOperationalLoop::ChooseSearchFlavors(const SOperationalFlavorRequest& request)
{
	return request.native;
}

//------------------------------------------------------------------------------
// engagements
//------------------------------------------------------------------------------

// Retains confirmed engagement counters on every exit from command search.
struct SOperationalEngagementScope
{
	const SOperationalTurn* pTurn;
	const SOperationalEngagementReport& report;
	// Borrows the command's accumulated search report.
	SOperationalEngagementScope(const SOperationalTurn* turn, const SOperationalEngagementReport& value) : pTurn(turn), report(value) {}
	// Counts provider/search attempts even if execution stopped the host.
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
	SOperationalCommandRecord* pCommand, STacticalMovementGate* pMovement)
{
	if (vUnits.empty() || !pTarget)
		return false;
	report.bTiming = pTurn ? pTurn->bTiming : (GC.getLogging() && GC.getAILogging());
	// Map the same embarked participants that the search will consider.
	GatherSearchPlotZones(vUnits, pTarget, bTargetDistanceRelevant, report.zonePlots, pMovement != NULL);
	const int iOperation = eKind == OPERATIONAL_KIND_ARMY_MARCH && pCommand ? pCommand->iOperation : VoxRlCurrentOperationId(ePlayer);
	// The command offset is the kind's base row plus, for a fight or Reinforce, the offsets from its situation.
	SOperationalSituation& situation = report.situation;
	situation = SOperationalSituation();
	if (pTurn && pCommand && eKind != OPERATIONAL_KIND_WITHDRAW && eKind != OPERATIONAL_KIND_ARMY_MARCH &&
		GatherSituation(*pTurn, *pCommand, situation))
	{
		situation.bApplied = true;
		SituationFlavorOffsets(situation, settings.afConstant[OPERATIONAL_CONSTANT_FLAVOR_FOCUS_SPAN],
			settings.afConstant[OPERATIONAL_CONSTANT_FLAVOR_RISK_SPAN], settings.afConstant[OPERATIONAL_CONSTANT_FLAVOR_SOFTENER]);
	}
	// The blend clamps and rounds the sum once.
	double afCommandOffset[STacticalFlavors::NUM_SLOTS];
	for (int iSlot = 0; iSlot < STacticalFlavors::NUM_SLOTS; iSlot++)
		afCommandOffset[iSlot] = settings.aiKindRows[eKind][iSlot] + situation.afOffset[iSlot];
	report.flavors = BlendEngagementFlavors(ePlayer, report.zonePlots, afCommandOffset, eCounterpart, iOperation,
		aiMapped, settings, report.iRepresentativeZone, report.iAggressionRisk);
	report.eAggLvl = AggressionFromRisk(report.iAggressionRisk, settings);
	// A Withdraw never plans a risky melee trade
	if ((eKind == OPERATIONAL_KIND_WITHDRAW || eKind == OPERATIONAL_KIND_ARMY_MARCH) && report.eAggLvl > AL_LOW)
		report.eAggLvl = AL_LOW;
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
	// Classify unit battles by the target zone, including friendly team territory.
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
		bTargetDistanceRelevant, report, pMovement);
}

// Runs one search attempt, keeping the same purpose through capture and fallback.
static vector<STacticalAssignment> SearchEngagementAttempt(PlayerTypes ePlayer, vector<CvUnit*>& units, CvPlot* pTarget,
	eAggressionLevel eAggLvl, SearchIntent eSearchIntent, const STacticalFlavors& flavors, bool bTargetDistanceRelevant,
	set<int>& unusableUnits, SOperationalEngagementReport& report)
{
	SOperationalTimer searchTimer(report.bTiming ? &report.searchNs : NULL);
	if (MOD_IPC_CHANNEL && gVoxRlCaptureEnabled)
		return VoxRlCapture::GetInstance().SearchAssignments(VOX_RL_CALLER_FULL, eSearchIntent, ePlayer,
			units, pTarget, static_cast<int>(eAggLvl), unusableUnits, bTargetDistanceRelevant, false, 0, flavors);
	return TacticalAIHelpers::FindBestUnitAssignments(units, pTarget, eAggLvl, unusableUnits, bTargetDistanceRelevant,
		false, 0, flavors, eSearchIntent == kSearchIntentWithdraw);
}

// Copy of the retry loop of TacticalAIHelpers::FindAndExecuteBestUnitAssignments. Changes: the flavors, aggression and
// target distance flag are given, and each attempt is recorded with the rows that ran.
bool CvTacticalOperationalLoop::SearchAndExecute(PlayerTypes ePlayer, vector<CvUnit*>& vUnits, CvPlot* pTarget, eAggressionLevel eAggLvl,
	SearchIntent eSearchIntent, const STacticalFlavors& flavors, bool bTargetDistanceRelevant, SOperationalEngagementReport& report,
	STacticalMovementGate* pMovement)
{
	int iCount = 0;
	bool bSuccess = false;
	set<int> unuseableUnits;
	vector<int> vCurrentIDs = GetUnitIDs(vUnits);
	vector<CvUnit*> currentUnits = vUnits;
	TacticalAIHelpers::UpdatePlotDistanceToTarget(ePlayer, pTarget);
	if (MOD_IPC_CHANNEL && gVoxRlCaptureEnabled)
		VoxRlCapture::GetInstance().BeginEngagement(VOX_RL_CALLER_FULL, ePlayer);
	do
	{
		iCount++;

		report.iAttempts++;
		vector<STacticalAssignment> vAssignments;
		if (pMovement)
		{
			// Rebuild stops before a retry so the search uses live plot permissions.
			TacticalUnitStops stops;
			pMovement->BuildStops(currentUnits, stops);
			STacticalStopScope stopScope(stops, pMovement);
			vAssignments = SearchEngagementAttempt(ePlayer, currentUnits, pTarget, eAggLvl, eSearchIntent, flavors,
				bTargetDistanceRelevant, unuseableUnits, report);
		}
		else
			vAssignments = SearchEngagementAttempt(ePlayer, currentUnits, pTarget, eAggLvl, eSearchIntent, flavors,
				bTargetDistanceRelevant, unuseableUnits, report);
		if (vAssignments.empty())
		{
			if (!unuseableUnits.empty() && currentUnits.size()>unuseableUnits.size())
			{
				//drop the offending units and try again
				vector<int> vRemainingIDs;
				for (vector<int>::const_iterator it = vCurrentIDs.begin(); it != vCurrentIDs.end(); ++it)
				{
					if (unuseableUnits.find(*it)==unuseableUnits.end())
						vRemainingIDs.push_back(*it);
				}
				vCurrentIDs = vRemainingIDs;
				GetUnitsByID(ePlayer, vCurrentIDs, currentUnits);
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
			bSuccess = TacticalAIHelpers::ExecuteUnitAssignments(ePlayer, vAssignments, &attempt.nRowsRun,
				pMovement);
			GetUnitsByID(ePlayer, vCurrentIDs, currentUnits);
			attempt.bCompleted = bSuccess;
			// A yielded attempt reports its committed prefix only.
			if (pMovement && pMovement->HasYielded())
				attempt.vRows.resize(attempt.nRowsRun);
			report.attempts.push_back(attempt);
			if (MOD_IPC_CHANNEL && gVoxRlCaptureEnabled)
				VoxRlCapture::GetInstance().NoteExecutionResult(bSuccess);
		}
	}
	while (!bSuccess && iCount < 4 && (!pMovement || !pMovement->HasYielded()));

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
	map<int, int>& zonePlots, bool bUseEmbarked)
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
			if (pUnit->isNativeDomain(pUnit->plot()) || pUnit->plot()->getNumUnits() == 1 || pUnit->plot()->isCity() ||
				(bUseEmbarked && pUnit->isEmbarked()))
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

// Shares native's march flags and asks a nearby Withdraw recruit to advance inside its current ring.
int CvTacticalOperationalLoop::MarchFlags(const CvUnit& kUnit, const CvPlot& kTarget, bool bWithdraw, bool bArmyRoute)
{
	int iFlags = ExactMarchFlags(kUnit, kTarget);
	// A formation Withdraw keeps its army's water limits and still needs a legal stop.
	if (bArmyRoute)
	{
		CvArmyAI* pArmy = GET_PLAYER(kUnit.getOwner()).getArmyAI(kUnit.getArmyID());
		return iFlags | (pArmy ? ArmyMarchRouteFlags(*pArmy) : 0);
	}
	const int iDistance = bWithdraw ? plotDistance(*kUnit.plot(), kTarget) : 3;
	if (iDistance > 2)
		iFlags |= CvUnit::MOVEFLAG_APPROX_TARGET_RING2;
	else if (iDistance == 2)
		iFlags |= CvUnit::MOVEFLAG_APPROX_TARGET_RING1;
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

// Ranks reachable cities by path length, preferring city shelter within one step of access by friendship.
// The flood runs in one domain and stays within recruit range. A naval destination must be coastal.
int CvTacticalOperationalLoop::WithdrawDestination(CvTacticalAI& kTactical, PlayerTypes ePlayer, const CvPlot& kStart,
	DomainTypes eDomain)
{
	const bool bSea = eDomain == DOMAIN_SEA;
	SPathFinderUserData data(ePlayer, bSea ? PT_NAVAL_UNIT_SIMPLE : PT_LAND_UNIT_SIMPLE);
	if (!bSea && !GET_PLAYER(ePlayer).CanEmbark())
		data.iFlags |= CvUnit::MOVEFLAG_NO_EMBARK;
	if (bSea && !GET_TEAM(GET_PLAYER(ePlayer).getTeam()).CanBuildOceanCrossingUnit())
		data.iFlags |= CvUnit::MOVEFLAG_NO_OCEAN;
	data.iMaxTurns = kTactical.GetRecruitRange();
	const ReachablePlots reachable = GC.GetStepFinder().GetPlotsInReach(&kStart, data);
	const TeamTypes eTeam = GET_PLAYER(ePlayer).getTeam();
	int iBestPlot = -1;
	pair<int, bool> best(INT_MAX, true);
	for (ReachablePlots::const_iterator it = reachable.begin(); it != reachable.end(); ++it)
	{
		if (it->iPathLength > best.first)
			continue;
		CvPlot* pPlot = GC.getMap().plotByIndex(it->iPlotIndex);
		CvCity* pCity = pPlot ? pPlot->getPlotCity() : NULL;
		if (!pCity || (bSea && !pCity->isCoastal()))
			continue;
		const bool bShelter = pPlot->isFriendlyCityForTeam(eTeam);
		if (!bShelter && !pPlot->IsFriendlyTerritory(ePlayer))
			continue;
		const pair<int, bool> rank(it->iPathLength + (bShelter ? 0 : 1), !bShelter);
		if (rank < best || (rank == best && (iBestPlot < 0 || it->iPlotIndex < iBestPlot)))
		{
			best = rank;
			iBestPlot = it->iPlotIndex;
		}
	}
	return iBestPlot;
}

// Returns the first stop under native's danger limit, or a safer, nonlethal Withdraw stop.
// An empty camp claim follows the exact route and uses the move mission's danger check.
CvPlot* CvTacticalOperationalLoop::CanMarch(CvTacticalAI& kTactical, CvUnit* pUnit, CvPlot* pTarget, bool bWithdraw,
	bool bArmyRoute, int iClaimTurns)
{
	if (!pTarget)
		return NULL;
	const int iFlags = iClaimTurns > 0 ? CvUnit::MOVEFLAG_NO_EMBARK | CvUnit::MOVEFLAG_AI_ABORT_IN_DANGER
		: MarchFlags(*pUnit, *pTarget, bWithdraw, bArmyRoute);
	const int iRange = iClaimTurns > 0 ? iClaimTurns : kTactical.GetRecruitRange();
	if (!pUnit->GeneratePath(pTarget, iFlags, iRange))
	{
		// A blocked destination still permits a legal approach to its ring.
		if (!bArmyRoute || !pUnit->GeneratePath(pTarget, iFlags | CvUnit::MOVEFLAG_APPROX_TARGET_RING1, iRange))
			return NULL;
	}
	CvPlot* pStop = pUnit->GetPathEndFirstTurnPlot();
	// A recruit already in the approximate target ring can still take part in a Reinforce search.
	if (!pStop)
		pStop = pUnit->plot();
	if (!pStop || (bWithdraw && pStop == pUnit->plot()))
		return NULL;

	// A formation Withdraw prices danger for the group; camp claims leave it to the move mission.
	if (bArmyRoute || iClaimTurns > 0)
		return pStop;

	//we are not here to fight or flee, let other moves take over
	int iDanger = pUnit->GetDanger(pStop);
	int iDangerLimit = (pUnit->IsCanAttack() && pUnit->AI_getUnitAIType()!=UNITAI_CITY_BOMBARD) ? pUnit->GetCurrHitPoints() / 2 : 0;
	//generals and siege should not even be in fog danger
	if (iDanger <= iDangerLimit)
		return pStop;
	// A Withdraw may take a first stop safer than staying, but never a lethal one.
	return bWithdraw && iDanger < pUnit->GetDanger() && iDanger < pUnit->GetCurrHitPoints() ? pStop : NULL;
}

// Finds a distant Gather member's first stop, including fleet navigation and native danger limits.
static CvPlot* ArmyGatherApproach(CvTacticalAI& kTactical, CvUnit& unit, CvPlot& waypoint, bool bNavalEscort,
	const STacticalMovementGate& gate, int iRange, int& iMoveFlags)
{
	iMoveFlags = CvUnit::MOVEFLAG_NO_STOPNODES | CvUnit::MOVEFLAG_ABORT_IF_NEW_ENEMY_REVEALED | gate.MoveFlags(unit);
	if (unit.isNativeDomain(&waypoint))
		iMoveFlags |= CvUnit::MOVEFLAG_APPROX_TARGET_NATIVE_DOMAIN;
	if (unit.IsCivilianUnit())
		iMoveFlags |= CvUnit::MOVEFLAG_DONT_STACK_WITH_NEUTRAL | CvUnit::MOVEFLAG_AI_ABORT_IN_DANGER;
	if (!bNavalEscort && unit.getDomainType() == DOMAIN_LAND)
		iMoveFlags |= CvUnit::MOVEFLAG_NO_EMBARK;

	if (!unit.GeneratePath(&waypoint, iMoveFlags | CvUnit::MOVEFLAG_APPROX_TARGET_RING2, iRange))
		return NULL;
	CvPlot* pStop = unit.GetPathEndFirstTurnPlot();
	if (!pStop || pStop == unit.plot())
		return NULL;
	if (!gate.Allows(unit, *pStop))
	{
		// An embarked member can advance along the coast even when the path ends on land.
		const CvPathNodeArray& path = unit.GetLastPath();
		pStop = NULL;
		for (size_t i = path.size(); i > 0; --i)
		{
			if (path[i - 1].m_iTurns != 0)
				continue;
			CvPlot* pCandidate = path.GetPlotByIndex(static_cast<int>(i - 1));
			if (pCandidate != unit.plot() && gate.Allows(unit, *pCandidate) && unit.canEndTurnAtPlot(pCandidate))
			{
				pStop = pCandidate;
				break;
			}
		}
		if (!pStop)
			return NULL;
	}
	const int iDangerLimit = unit.IsCanAttack() && unit.AI_getUnitAIType() != UNITAI_CITY_BOMBARD
		? unit.GetCurrHitPoints() / 2 : 0;
	if (unit.GetDanger(pStop) > iDangerLimit)
		return NULL;
	const CvTacticalDominanceZone* pZone = kTactical.GetTacticalAnalysisMap()->GetZoneByPlot(pStop);
	if (pZone && pZone->GetOverallDominanceFlag() != TACTICAL_DOMINANCE_FRIENDLY && !unit.isEmbarked())
		iMoveFlags |= CvUnit::MOVEFLAG_NO_EMBARK;
	return pStop;
}

// Installs the movement gate only for a command that needs it.
eOperationalPositionResult CvTacticalOperationalLoop::PositionUnitsAroundTarget(CvTacticalAI& kTactical, const vector<CvUnit*>& vUnitsIn, CvPlot* pTarget,
	PlayerTypes eCounterpart, SOperationalEngagementReport& report, const SOperationalTurn& turn, SOperationalCommandRecord& command,
	const vector<SOperationalMarch>* pWithdrawStops, const vector<CvPlot*>* pMarchCorridor)
{
	const vector<int> vUnitIDs = GetUnitIDs(vUnitsIn);
	const bool bArmyMarch = command.eKind == OPERATIONAL_KIND_ARMY_MARCH;
	if (!bArmyMarch && !pWithdrawStops)
		return PositionUnitsAroundTargetWithGate(kTactical, vUnitsIn, vUnitIDs, pTarget, eCounterpart, report, turn, command, NULL, NULL);

	const set<int> admitted(vUnitIDs.begin(), vUnitIDs.end());
	if (bArmyMarch)
	{
		SOperationalMarchGate gate(turn, command, *pTarget, pMarchCorridor, admitted);
		return PositionUnitsAroundTargetWithGate(kTactical, vUnitsIn, vUnitIDs, pTarget, eCounterpart, report, turn, command, &gate, NULL);
	}
	SOperationalWithdrawGate gate(turn, *pWithdrawStops, admitted);
	return PositionUnitsAroundTargetWithGate(kTactical, vUnitsIn, vUnitIDs, pTarget, eCounterpart, report, turn, command, &gate, pWithdrawStops);
}

// Copy of CvTacticalAI::PositionUnitsAroundTarget. Changes: the search runs through ExecuteEngagement over the units
// still usable after the spotter move, with the kind row, the geographic blend and aggression from RISK; the spotter
// and path moves skip held garrisons. A shared Withdraw reuses its checked stops. Army March keeps native's two
// MOVEFLAG_NO_EMBARK rules for land units (no naval escort in the list, or a first turn ending outside a zone we
// dominate), checks danger at its approach stop and records the route it takes. Other path moves go through CanMarch
// and MarchFlags, whose land units use MOVEFLAG_SAFE_EMBARK_ONLY in place of those two rules. A Withdraw skips the
// spotter, records each unit's starting plot, sends every recruit that can still move and orders damaged units first.
// After a successful Withdraw search a recruit it used on a plot with no danger stays.
eOperationalPositionResult CvTacticalOperationalLoop::PositionUnitsAroundTargetWithGate(CvTacticalAI& kTactical, const vector<CvUnit*>& vUnitsIn,
	const vector<int>& vUnitIDs, CvPlot* pTarget, PlayerTypes eCounterpart, SOperationalEngagementReport& report, const SOperationalTurn& turn,
	SOperationalCommandRecord& command, STacticalMovementGate* pGate, const vector<SOperationalMarch>* pWithdrawStops)
{
	const eOperationalKind eKind = command.eKind;
	const PlayerTypes ePlayer = kTactical.m_pPlayer->GetID();

	// A Withdraw compares a unit with where it started after the search, so record the starting plots first.
	const bool bWithdraw = eKind == OPERATIONAL_KIND_WITHDRAW;
	const bool bArmyMarch = eKind == OPERATIONAL_KIND_ARMY_MARCH;
	map<int, int> startPlots;
	if (bWithdraw)
		for (vector<CvUnit*>::const_iterator it = vUnitsIn.begin(); it != vUnitsIn.end(); ++it)
			if ((*it)->plot())
				startPlots[(*it)->GetID()] = (*it)->plot()->GetPlotIndex();

	//try to improve visibility. however, if the target is too far away this may fail ... in that case we chance it
	if (!bWithdraw)
	{
		vector<CvUnit*> vSpotters;
		for (vector<CvUnit*>::const_iterator it = vUnitsIn.begin(); it != vUnitsIn.end(); ++it)
			if (!IsHeldGarrison(*it))
				vSpotters.push_back(*it);
		{
			SOperationalTimer timer(turn.bTiming ? &turn.performance.spottingNs : NULL);
			kTactical.ExecuteSpotterMove(vSpotters, pTarget, pGate);
		}
		// Spotting can disclose contact before Gather has any assignments.
		if (pGate && pGate->AfterAction())
			return OPERATIONAL_POSITION_YIELDED;
	}

	vector<CvUnit*> vUnits;
	GetUnitsByID(ePlayer, vUnitIDs, vUnits);

	//first round: in case there are enemies around, do a combat simulation
	vector<CvUnit*> vSimUnits; //make a copy we can modify!
	for (vector<CvUnit*>::const_iterator it = vUnits.begin(); it != vUnits.end(); ++it)
		if ((*it)->canUseNow())
			vSimUnits.push_back(*it);
	bool bTactSimSuccess = vSimUnits.empty() ? false : ExecuteEngagement(ePlayer, vSimUnits, pTarget, eKind, eCounterpart,
		turn.aiMapped, turn.settings, !bWithdraw, report, &turn, &command, pGate);
	if (command.iYieldPlot >= 0)
		return OPERATIONAL_POSITION_YIELDED;
	GetUnitsByID(ePlayer, vUnitIDs, vUnits);

	SOperationalTimer marchingTimer(turn.bTiming ?
		(bArmyMarch ? &turn.performance.armyMarchMoveNs : &turn.performance.marchingNs) : NULL);
	//sometimes tactsim cannot use all units, eg if they are too far out
	vector<CvUnit*> farout;
	bool bHaveNavalEscort = false;
	//the search clears its distance map when it finishes, so rebuild it for the distance check below.
	// The Withdraw branch reads only danger, never this map.
	if (bTactSimSuccess && !bWithdraw)
		TacticalAIHelpers::UpdatePlotDistanceToTarget(ePlayer, pTarget);
	for (vector<CvUnit*>::const_iterator it = vUnits.begin(); it != vUnits.end(); ++it)
	{
		CvUnit* pUnit = *it;
		if (bArmyMarch && pUnit->IsCombatUnit() && pUnit->getDomainType() == DOMAIN_SEA)
			bHaveNavalEscort = true;
		if (IsHeldGarrison(pUnit))
			continue;

		if (bWithdraw)
		{
			// Every recruit goes to the path step unless the search used it; a used unit still on its
			// starting plot with movement left goes too.
			bool bMarch = !pUnit->TurnProcessed();
			if (!bMarch && pUnit->plot() && pUnit->canMove())
			{
				map<int, int>::const_iterator start = startPlots.find(pUnit->GetID());
				bMarch = start != startPlots.end() && start->second == pUnit->plot()->GetPlotIndex();
			}
			// After a successful search a recruit it used stays when its plot has no danger, so a chosen
			// safe heal stands. A recruit the search did not use still marches.
			if (bTactSimSuccess && pUnit->TurnProcessed() && (!pUnit->plot() || pUnit->GetDanger() <= 0))
				bMarch = false;
			if (bMarch)
				farout.push_back(pUnit);
			continue;
		}

		if (pUnit->TurnProcessed())
			continue;

		if (bTactSimSuccess && TacticalAIHelpers::GetPlotDistanceToTarget(pUnit->plot()->GetPlotIndex(), pUnit->getDomainType()) <= TACTICAL_COMBAT_MAX_TARGET_DISTANCE)
			continue; //do not end the turn ... we may want to shuffle them around later

		farout.push_back(pUnit);
	}
	gDistanceToTargetPlots.clear();

	//we want to move the civilians last so they have a better chance of getting cover
	if (bWithdraw)
		std::stable_sort(farout.begin(), farout.end(), PrOperationalSortWithdraw());
	else
		std::stable_sort(farout.begin(), farout.end(), PrOperationalSortCombatFirst());
	const vector<int> vFaroutIDs = GetUnitIDs(farout);

	//second round: move in as long as there is no danger and we're still far away
	for (vector<int>::const_iterator it = vFaroutIDs.begin(); it != vFaroutIDs.end(); ++it)
	{
		CvUnit* pUnit = kTactical.m_pPlayer->getUnit(*it);
		if (!pUnit || (bArmyMarch && !pUnit->canUseNow()))
			continue;
		// Shared Withdraw uses the exact first stop that was scored for this layout.
		if (pWithdrawStops)
		{
			CvPlot* pStop = FindWithdrawStop(*pWithdrawStops, *it);
			if (pStop && pStop != pUnit->plot() && pUnit->canMove())
				kTactical.ExecuteMoveToPlot(pUnit, pStop, pUnit->getArmyID() == -1, 0, pGate);
			continue;
		}
		if (bArmyMarch)
		{
			int iFlags = 0;
			CvPlot* pStop = ArmyGatherApproach(kTactical, *pUnit, *pTarget, bHaveNavalEscort, *pGate,
				kTactical.GetRecruitRange(), iFlags);
			if (pStop)
				kTactical.ExecuteMoveToPlot(pUnit, pStop, true, iFlags, pGate);
		}
		else if (CanMarch(kTactical, pUnit, pTarget, bWithdraw))
			kTactical.ExecuteMoveToPlot(pUnit, pTarget, true, MarchFlags(*pUnit, *pTarget, bWithdraw), pGate);
		if (command.iYieldPlot >= 0)
			return OPERATIONAL_POSITION_YIELDED;
	}

	// Run the original safety pass for any army members in this positioning group.
	if (!bArmyMarch && !pWithdrawStops)
	{
		GetUnitsByID(ePlayer, vUnitIDs, vUnits);
		MoveArmyUnitsToSafety(vUnits);
	}
	return OPERATIONAL_POSITION_COMPLETE;
}

// Moves unused army members away from danger and ends their turn.
void CvTacticalOperationalLoop::MoveArmyUnitsToSafety(const vector<CvUnit*>& vUnits)
{
	for (vector<CvUnit*>::const_iterator it = vUnits.begin(); it != vUnits.end(); ++it)
	{
		CvUnit* pUnit = *it;
		// Only army members still waiting for orders need this pass.
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
}

//------------------------------------------------------------------------------
// danger by threat
//------------------------------------------------------------------------------

// Copy of CvDangerPlotContents::GetDanger(const CvUnit*, ...), listing each enemy contribution at its roll.
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
				SOperationalThreatDamage threat = { it->first, it->second, false, iDamage, 0 };
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
			SOperationalThreatDamage threat = { it->first, it->second, true, iCityDamage, 0 };
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

			// Project the enemy hit at its roll, then our retaliation at our roll for the suicide check.
			int iDamage = 0;
			int iAreaDamage = 0;
			{
				OperationalDamageRollScope roll(EnemyRollPer200(iRisk));
				iDamage = TacticalAIHelpers::GetSimulatedDamageFromAttackOnUnit(pUnit, pAttacker, pPlot, bOutOfRange ? NULL : pAttacker->plot(), iAttackerDamage, false, iExtraAttackerDamage, iExtraDamage, true, NULL, &iAreaDamage);
			}
			// Native non-air ranged attacks have no retaliation.
			if (!pAttacker->IsCanAttackRanged() || pAttacker->getDomainType() == DOMAIN_AIR)
			{
				OperationalDamageRollScope roll(OurRollPer200(iRisk));
				TacticalAIHelpers::GetSimulatedDamageFromAttackOnUnit(pUnit, pAttacker, pPlot, bOutOfRange ? NULL : pAttacker->plot(), iAttackerDamage, false, iExtraAttackerDamage, iExtraDamage, true);
			}

			// Include area damage on move once per turn. The attack forecast already counted stationary tile damage.
			const int iMoveDamage = std::max(0, pAttacker->getAoEDamageOnMove());
			iDamage += iMoveDamage;
			iAreaDamage += iMoveDamage;

			if (!pPlot->IsKnownVisibleToTeam(pAttacker->getTeam()))
			{
				iDamage = (iDamage * 80) / 100; //there's a chance they won't spot us
				iAreaDamage = (iAreaDamage * 80) / 100;
			}

			if (iAttackerDamage >= pAttacker->GetCurrHitPoints() && !pAttacker->isSuicide())
			{
				iDamage /= 2; //suicide attack with non suicide unit is unlikely
				iAreaDamage /= 2;
			}

			iPlotDamage += iDamage;
			SOperationalThreatDamage threat = { it->first, it->second, false, iDamage - iAreaDamage, iAreaDamage };
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
		SOperationalThreatDamage threat = { it->first, it->second, true, iCityDamage, 0 };
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

// Copy of CvDangerPlotContents::GetDanger(const CvCity*, ...), preserving native hit order and absorption.
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
					// Always assume interception succeeds, using our roll for our interceptor's hit.
					OperationalDamageRollScope roll(OurRollPer200(iRisk));
					iInterceptDamage = pInterceptor->GetInterceptionDamage(pUnit, false, kContents.m_pPlot);
				}
				OperationalDamageRollScope roll(EnemyRollPer200(iRisk));
				int iDamage = pUnit->GetAirCombatDamage(NULL, pCity, (iGarrisonHPLeft <= 0 ? 0 : iGarrisonMaxHP), iGarrisonDamage, false, iInterceptDamage + iExtraAttackerDamage, iExtraSelfDamage, kContents.m_pPlot);
				iPlotDamage += iDamage;
				if (pThreats)
				{
					SOperationalThreatDamage threat = { it->first, it->second, false, iDamage, 0 };
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
					SOperationalThreatDamage threat = { it->first, it->second, false, iDamage, 0 };
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
				SOperationalThreatDamage threat = { it->first, it->second, false, iDamage, 0 };
				pThreats->push_back(threat);
			}
			iExtraSelfDamage += iDamage;
			iGarrisonHPLeft -= iGarrisonDamage;
		}
	}

	return iPlotDamage;
}

// Retains the standard city's total and attacker list until threats are rebuilt.
const SOperationalCityDanger& CvTacticalOperationalLoop::CachedCityDanger(const SOperationalTurn& turn, const CvCity& kCity)
{
	const int iPlotIndex = kCity.plot()->GetPlotIndex();
	map<int, SOperationalCityDanger>::iterator it = turn.cityDanger.find(iPlotIndex);
	if (it == turn.cityDanger.end())
	{
		SOperationalTimer dangerTimer(turn.bTiming && turn.bEstimating ? &turn.performance.dangerReadNs : NULL);
		if (turn.bEstimating) turn.performance.dangerReads++;
		CvDangerPlotContents& contents = DangerContents(turn.ePlayer, *kCity.plot());
		SOperationalCityDanger danger;
		danger.iTotal = GetCityDangerByThreat(contents, &kCity, NULL, SUnitIDValueContainer(), 0, &danger.threats,
			turn.aiMapped[STacticalFlavors::RISK]);
		it = turn.cityDanger.insert(make_pair(iPlotIndex, danger)).first;
	}
	return it->second;
}

// Reads the monotonic performance counter used by cvStopWatch, avoiding absolute-tick overflow.
unsigned long long CvTacticalOperationalLoop::ClockNs()
{
	static LARGE_INTEGER frequency = {0};
	if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
	LARGE_INTEGER ticks;
	QueryPerformanceCounter(&ticks);
	return (ticks.QuadPart / frequency.QuadPart) * 1000000000ULL +
		(ticks.QuadPart % frequency.QuadPart) * 1000000000ULL / frequency.QuadPart;
}

// Reports the game's retained terminal state.
bool CvTacticalOperationalLoop::IsHostStopped()
{
	return GC.getGame().getGameState() == GAMESTATE_OVER;
}

// Closed offers remain consumed for the rest of this pass.
bool OperationalOfferIsClosed(eOperationalOfferState state)
{
	return state >= OPERATIONAL_OFFER_SKIPPED;
}

// Names closure using retained state and immutable offer metadata.
const char* OperationalOfferClosedReason(eOperationalOfferState state, eOperationalKind kind)
{
	if (state == OPERATIONAL_OFFER_SKIPPED) return "failureLimit";
	if (state == OPERATIONAL_OFFER_RETIRED) return "retired";
	if (state == OPERATIONAL_OFFER_EXECUTED) return "executed";
	if (state == OPERATIONAL_OFFER_REFUSED) return "providerRefused";
	if (state == OPERATIONAL_OFFER_DROPPED)
		return kind == OPERATIONAL_KIND_ARMY_MARCH ? "operationGone" : kind == OPERATIONAL_KIND_UNIT_FIGHT ? "targetGone" : "cityLost";
	return "";
}

// City progress at our city uses hold-city; other progress uses occupation.
int OperationalStakeWeight(int iTerm, eOperationalKind kind)
{
	const int weights[NUM_OPERATIONAL_STAKE_TERMS] = { OPERATIONAL_WEIGHT_ATTRITION, OPERATIONAL_WEIGHT_RISK,
		OPERATIONAL_WEIGHT_HOLD_GROUND, OPERATIONAL_WEIGHT_HOLD_CITY,
		kind == OPERATIONAL_KIND_OWN_CITY_FIGHT ? OPERATIONAL_WEIGHT_HOLD_CITY : OPERATIONAL_WEIGHT_OCCUPATION,
		OPERATIONAL_WEIGHT_ATTRITION };
	return weights[iTerm];
}

// Compares the offer's scalar inputs before emitting a replacement snapshot row.
bool OperationalSnapshotEqual(const SOperationalSnapshot& a, const SOperationalSnapshot& b)
{
	if (a.iPlotIndex != b.iPlotIndex || a.iRank != b.iRank || a.eState != b.eState || a.fEstimate != b.fEstimate ||
		a.armyMarch.fUnassistedTheater != b.armyMarch.fUnassistedTheater || a.armyMarch.fGoalWeight != b.armyMarch.fGoalWeight ||
		a.armyMarch.fReadinessGain != b.armyMarch.fReadinessGain ||
		a.fTheater != b.fTheater || a.fSwing != b.fSwing || a.fSelectedSwing != b.fSelectedSwing ||
		a.fExposureShare != b.fExposureShare || a.fWithdrawExposure != b.fWithdrawExposure ||
		a.fWithdrawShare != b.fWithdrawShare || a.aiWithdrawDestination[0] != b.aiWithdrawDestination[0] ||
		a.aiWithdrawDestination[1] != b.aiWithdrawDestination[1] || a.stake.fExposureShare != b.stake.fExposureShare ||
		a.theater.fImportance != b.theater.fImportance || a.theater.fPressure != b.theater.fPressure ||
		a.theater.fContest != b.theater.fContest || a.theater.fDamageLevel != b.theater.fDamageLevel) return false;
	// Only Reinforce uses the added stake inputs, so their changes do not emit Fight or Withdraw rows.
	const bool bReinforce = a.eSlot == OPERATIONAL_SLOT_REINFORCE;
	if (bReinforce && (a.fTurnsToContact != b.fTurnsToContact || a.fPotentialWeight != b.fPotentialWeight ||
		a.fRatio != b.fRatio || a.fNeed != b.fNeed || a.fSelectedStrength != b.fSelectedStrength ||
		a.stake.fStake != b.stake.fStake || a.stake.fRealized != b.stake.fRealized || a.stake.fPotential != b.stake.fPotential)) return false;
	for (int i = 0; i < NUM_OPERATIONAL_STAKE_TERMS; ++i)
		if (a.stake.afValue[i] != b.stake.afValue[i]) return false;
	return a.stake.fDestruction == b.stake.fDestruction && a.stake.fExposure == b.stake.fExposure &&
		a.stake.fGround == b.stake.fGround && a.stake.fRelief == b.stake.fRelief &&
		a.stake.fCityProgress == b.stake.fCityProgress && a.stake.fGarrison == b.stake.fGarrison;
}

// Writes only scalar snapshots from the live queue, with stable ranks and explicit closure reasons.
void CvTacticalOperationalLoop::RecordStep(const SOperationalTurn& turn, SOperationalTurnRecord& record)
{
	SOperationalTimer timer(turn.bTiming ? &turn.performance.recordNs : NULL);
	SOperationalStep step;
	step.iStep = static_cast<int>(record.steps.size());
	step.estimateNs = turn.performance.estimateNs - record.recordedEstimateNs;
	record.recordedEstimateNs = turn.performance.estimateNs;
	// Every battlefield writes one row per offer slot, in slot order.
	vector<int> ready, ranks(turn.battlefields.size() * NUM_OPERATIONAL_SLOTS, -1);
	for (size_t i = 0; i < turn.battlefields.size(); ++i)
		for (int k = 0; k < NUM_OPERATIONAL_SLOTS; ++k)
			if (OperationalSupportsSlot(turn.battlefields[i].eCenter, static_cast<eOperationalSlot>(k)) &&
				OperationalOfferState(turn.battlefields[i], static_cast<eOperationalSlot>(k)) == OPERATIONAL_OFFER_READY)
				ready.push_back(OperationalOfferIdentity(static_cast<int>(i), static_cast<eOperationalSlot>(k)));
	std::sort(ready.begin(), ready.end(), PrOperationalOfferBefore(turn.battlefields));
	for (size_t i = 0; i < ready.size(); ++i) ranks[ready[i]] = static_cast<int>(i);
	for (size_t i = 0; i < turn.battlefields.size(); i++)
	{
		const SOperationalBattlefield& field = turn.battlefields[i];
		for (int k = 0; k < NUM_OPERATIONAL_SLOTS; k++)
		{
			const eOperationalSlot eSlot = static_cast<eOperationalSlot>(k);
			const int iOffer = OperationalOfferIdentity(static_cast<int>(i), eSlot);
			if (record.offers.size() <= static_cast<size_t>(iOffer))
			{
				SOperationalOfferRecord offer = { field.iID, field.iTargetID, field.iPlotIndex, field.iZoneID,
					OperationalSlotKind(field.eCenter, eSlot), field.eOwner, field.iOperation };
				record.offers.push_back(offer);
			}
			SOperationalSnapshot row;
			row.iBattlefield = field.iID;
			row.iPlotIndex = field.iPlotIndex;
			row.eSlot = eSlot;
			row.eState = OperationalOfferState(field, eSlot);
			row.fEstimate = OperationalOfferEstimate(field, eSlot);
			row.fTheater = field.fTheater;
			row.theater = field.theater;
			// A gated Reinforce contributes no swing; fights and Withdraw keep a full swing.
			const bool bReinforce = eSlot == OPERATIONAL_SLOT_REINFORCE && field.eCenter != OPERATIONAL_CENTER_ARMY &&
				(field.eCenter != OPERATIONAL_CENTER_CAMP || field.bCampFogged);
			row.fSwing = bReinforce ? (field.fReinforceEstimate > 0.0 ? field.reinforce.fSwing : 0.0) : 1.0;
			row.fSelectedSwing = bReinforce ? field.reinforce.fSwing : 1.0;
			row.fTurnsToContact = bReinforce ? field.reinforce.fTurnsToContact : 0.0;
			row.fPotentialWeight = bReinforce ? field.reinforce.fPotentialWeight : 0.0;
			row.fRatio = bReinforce ? field.reinforce.fRatio : 0.0;
			row.fNeed = bReinforce ? field.reinforce.fNeed : 0.0;
			row.fSelectedStrength = bReinforce ? field.reinforce.fSelectedStrength : 0.0;
			if (OperationalSlotKind(field.eCenter, eSlot) == OPERATIONAL_KIND_ARMY_MARCH)
				row.armyMarch = field.armyMarch;
			row.stake = field.stake;
			row.fExposureShare = field.fExposureShare;
			row.fWithdrawExposure = field.fWithdrawExposure;
			row.fWithdrawShare = field.fWithdrawShare;
			row.aiWithdrawDestination[0] = field.aiWithdrawDestination[0];
			row.aiWithdrawDestination[1] = field.aiWithdrawDestination[1];

			if (OperationalOfferIsClosed(row.eState) && static_cast<size_t>(iOffer) < record.latestSnapshot.size())
			{
				const eOperationalOfferState state = row.eState;
				const SOperationalSnapshot& previous = record.latestSnapshot[iOffer];
				// Freeze a skipped Withdraw at its failed check, which may have lowered its estimate.
				if (eSlot != OPERATIONAL_SLOT_WITHDRAW || state != OPERATIONAL_OFFER_SKIPPED || previous.eState == state)
					row = previous;
				row.eState = state;
				// A checked offer can execute at a repaired estimate within this step.
				if (state == OPERATIONAL_OFFER_EXECUTED || state == OPERATIONAL_OFFER_REFUSED)
					for (size_t c = 0; c < record.commands.size(); c++)
						if (record.commands[c].iBattlefield == field.iID && record.commands[c].iYieldPlot < 0 &&
							OperationalKindSlot(record.commands[c].eKind) == eSlot)
						{
							row.fEstimate = record.commands[c].fEstimate;
							row.fTheater = record.commands[c].fTheater;
							row.fSwing = row.fSelectedSwing = record.commands[c].fSwing;
							row.fTurnsToContact = record.commands[c].fTurnsToContact;
							row.fPotentialWeight = record.commands[c].fPotentialWeight;
							row.fRatio = record.commands[c].fRatio;
							row.fNeed = record.commands[c].fNeed;
							row.fSelectedStrength = record.commands[c].fSelectedStrength;
							row.iPlotIndex = record.commands[c].iPlotIndex;
							row.stake = record.commands[c].stake;
							row.armyMarch = record.commands[c].armyMarch;
							// The Withdraw check filled the destinations and the reduced exposure just
							// before execution, so the closed row shows where the retreat went and reproduces its
							// estimate.
							if (eSlot == OPERATIONAL_SLOT_WITHDRAW)
							{
								row.aiWithdrawDestination[0] = field.aiWithdrawDestination[0];
								row.aiWithdrawDestination[1] = field.aiWithdrawDestination[1];
								row.fWithdrawExposure = field.fWithdrawExposure;
								row.fWithdrawShare = field.fWithdrawShare;
							}
						}
			}
			row.szReason = OperationalOfferClosedReason(row.eState,
				field.eCenter == OPERATIONAL_CENTER_ARMY ? OperationalSlotKind(field.eCenter, eSlot) :
				record.offers[i * NUM_OPERATIONAL_SLOTS].eKind);
			row.iRank = ranks[iOffer];
			if (static_cast<size_t>(iOffer) >= record.latestSnapshot.size())
			{
				step.snapshot.push_back(row);
				record.latestSnapshot.push_back(row);
			}
			else if (!OperationalSnapshotEqual(row, record.latestSnapshot[iOffer]))
			{
				step.snapshot.push_back(row);
				record.latestSnapshot[iOffer] = row;
			}
		}
	}
	record.steps.push_back(step);
}

// Closes the last trace step before scratch destruction, after nested stage timers have unwound.
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
		// Retain partial stages if an estimate or readiness check unwound before its normal record boundary.
		step.estimateNs = turn.performance.estimateNs - estimated;
		step.readinessNs = turn.performance.readinessNs - checked;
		step.executeNs = turn.performance.executeNs - executed;
	}
}
