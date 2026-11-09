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
const signed char OPERATIONAL_KIND_FLAVOR_ROWS[][STacticalFlavors::NUM_SLOTS] =
{
	//  RISK  OCCUPATION  ATTRITION  HOLD_CITY  HOLD_GROUND
	{    0,     0,          0,         0,         0 },  // Unit fight
	{   10,    15,        -25,        -5,        -5 },  // City fight
	{  -15,   -10,        -10,        10,        20 },  // Reinforce
	{  -10,   -10,          0,         0,       -10 },  // Withdraw
};
typedef char OperationalKindRowsMatchKinds[(sizeof(OPERATIONAL_KIND_FLAVOR_ROWS) / sizeof(OPERATIONAL_KIND_FLAVOR_ROWS[0]) == NUM_OPERATIONAL_KINDS) ? 1 : -1];

const char* const OPERATIONAL_POSTURE_NAMES[] =
{
	"NONE", "WITHDRAW", "ATTRITION", "EXPLOIT_FLANKS", "STEAMROLL", "SURGICAL_CITY_STRIKE", "HEDGEHOG", "COUNTERATTACK",
};
typedef char OperationalPostureNamesMatchPostures[(sizeof(OPERATIONAL_POSTURE_NAMES) / sizeof(OPERATIONAL_POSTURE_NAMES[0]) == NUM_OPERATIONAL_POSTURES) ? 1 : -1];

const char* const OPERATIONAL_KIND_NAMES[] =
{
	"UNIT_FIGHT", "CITY_FIGHT", "REINFORCE", "WITHDRAW",
};
typedef char OperationalKindNamesMatchKinds[(sizeof(OPERATIONAL_KIND_NAMES) / sizeof(OPERATIONAL_KIND_NAMES[0]) == NUM_OPERATIONAL_KINDS) ? 1 : -1];

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

// Builds each known zone's vector from the native modifiers and the zone's posture row, then blends.
STacticalFlavors CvTacticalOperationalLoop::BlendEngagementFlavors(PlayerTypes ePlayer, const map<int, int>& zonePlots, eOperationalKind eKind,
	PlayerTypes eCounterpart, int iOperationID, const int (&aiMapped)[STacticalFlavors::NUM_SLOTS],
	const OperationalSettings& settings, int& iRepresentativeZone)
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

	vector<SOperationalZoneFlavors> zones;
	for (map<int, int>::const_iterator it = zonePlots.begin(); it != zonePlots.end(); ++it)
	{
		const CvTacticalDominanceZone* pZone = pZoneMap->GetZoneByIDWithoutRefresh(it->first);
		if (!pZone || it->second <= 0)
			continue;
		SOperationalZoneFlavors zone;
		zone.iZoneID = it->first;
		zone.iPlots = it->second;
		int aiZone[STacticalFlavors::NUM_SLOTS];
		for (int iSlot = 0; iSlot < STacticalFlavors::NUM_SLOTS; iSlot++)
			aiZone[iSlot] = aiMapped[iSlot];
		const short* pZoneModifier = pZoneMap->HasZoneTacticalFlavorModifiers() ? pZoneMap->GetZoneTacticalFlavorModifiers(it->first) : NULL;
		if (pCivModifier || pOperationModifier || pZoneModifier)
			TacticalAIHelpers::ApplyFlavorModifiers(aiZone, pCivModifier, pOperationModifier, pZoneModifier, kSearchIntentUnknown, false);
		const int iPosture = range((int)pZone->GetPosture(), 0, NUM_OPERATIONAL_POSTURES - 1);
		for (int iSlot = 0; iSlot < STacticalFlavors::NUM_SLOTS; iSlot++)
			zone.aiFlavor[iSlot] = aiZone[iSlot] + settings.aiPostureRows[iPosture][iSlot];
		zones.push_back(zone);
	}

	STacticalFlavors flavors;
	BlendZoneFlavors(zones, aiMapped, settings.aiKindRows[eKind], flavors.iFlavor, iRepresentativeZone);
	flavors.bRiskSet = true;
	flavors.iCounterpart = eCounterpart;
	return flavors;
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

// No battlefield exists until the fight loop is built, so the loop ends at once.
void CvTacticalOperationalLoop::RunCommandLoop(CvTacticalAI& /*kTactical*/)
{
}

//------------------------------------------------------------------------------
// engagements
//------------------------------------------------------------------------------

// The geometry comes first so the flavors can follow the zones the search covers.
bool CvTacticalOperationalLoop::ExecuteEngagement(PlayerTypes ePlayer, vector<CvUnit*>& vUnits, CvPlot* pTarget, eOperationalKind eKind,
	PlayerTypes eCounterpart, const int (&aiMapped)[STacticalFlavors::NUM_SLOTS], const OperationalSettings& settings,
	SOperationalEngagementReport& report)
{
	if (vUnits.empty() || !pTarget)
		return false;
	GatherSearchPlotZones(vUnits, pTarget, report.zonePlots);
	report.flavors = BlendEngagementFlavors(ePlayer, report.zonePlots, eKind, eCounterpart, VoxRlCurrentOperationId(ePlayer),
		aiMapped, settings, report.iRepresentativeZone);
	report.eAggLvl = AggressionFromRisk(report.flavors.iFlavor[STacticalFlavors::RISK], settings);
	return SearchAndExecute(ePlayer, vUnits, pTarget, report.eAggLvl, report.flavors, report);
}

// Copy of the retry loop of TacticalAIHelpers::FindAndExecuteBestUnitAssignments. Changes: the flavors and aggression
// are given, and each attempt is recorded with the rows that ran. Capture records the search purpose as unknown.
bool CvTacticalOperationalLoop::SearchAndExecute(PlayerTypes ePlayer, vector<CvUnit*>& vUnits, CvPlot* pTarget, eAggressionLevel eAggLvl,
	const STacticalFlavors& flavors, SOperationalEngagementReport& report)
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
			vAssignments = VoxRlCapture::GetInstance().SearchAssignments(VOX_RL_CALLER_FULL, kSearchIntentUnknown, ePlayer,
				currentUnits, pTarget, static_cast<int>(eAggLvl), unuseableUnits, true, false, 0, flavors);
		}
		else
		{
			vAssignments = TacticalAIHelpers::FindBestUnitAssignments(currentUnits, pTarget, eAggLvl, unuseableUnits, true, false, 0, flavors);
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
void CvTacticalOperationalLoop::GatherSearchPlotZones(const vector<CvUnit*>& vUnits, CvPlot* pTarget, map<int, int>& zonePlots)
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
	initialPosition->initFromScratch(ePlayer, AL_LOW, pTarget, true, false, 0, neutralFlavors);
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
// marching and withdrawal
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

// Copy of CvTacticalAI::PositionUnitsAroundTarget. Changes: the search takes the given flavors and aggression; the
// spotter and path moves skip held garrisons; a land unit's path move uses MOVEFLAG_SAFE_EMBARK_ONLY in place of
// native's two MOVEFLAG_NO_EMBARK rules (no naval escort in the list, or a first turn ending outside a zone we dominate).
bool CvTacticalOperationalLoop::PositionUnitsAroundTarget(CvTacticalAI& kTactical, const vector<CvUnit*>& vUnits, CvPlot* pTarget,
	const STacticalFlavors& flavors, eAggressionLevel eAggLvl, SOperationalEngagementReport& report)
{
	//try to improve visibility. however, if the target is too far away this may fail ... in that case we chance it
	vector<CvUnit*> vSpotters;
	for (vector<CvUnit*>::const_iterator it = vUnits.begin(); it != vUnits.end(); ++it)
		if (!IsHeldGarrison(*it))
			vSpotters.push_back(*it);
	kTactical.ExecuteSpotterMove(vSpotters, pTarget);

	//first round: in case there are enemies around, do a combat simulation
	report.flavors = flavors;
	report.eAggLvl = eAggLvl;
	vector<CvUnit*> vSimUnits = vUnits; //make a copy we can modify!
	bool bTactSimSuccess = vSimUnits.empty() ? false : SearchAndExecute(kTactical.m_pPlayer->GetID(), vSimUnits, pTarget, eAggLvl, flavors, report);

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
		int	iFlags = CvUnit::MOVEFLAG_NO_STOPNODES | CvUnit::MOVEFLAG_APPROX_TARGET_RING2;
		if (pUnit->isNativeDomain(pTarget)) //don't embark if we don't have to
			iFlags |= CvUnit::MOVEFLAG_APPROX_TARGET_NATIVE_DOMAIN;
		if (pUnit->IsCivilianUnit())
			iFlags |= (CvUnit::MOVEFLAG_DONT_STACK_WITH_NEUTRAL | CvUnit::MOVEFLAG_AI_ABORT_IN_DANGER);
		//embark only where no turn ends at sea in danger
		if (pUnit->getDomainType() == DOMAIN_LAND)
			iFlags |= CvUnit::MOVEFLAG_SAFE_EMBARK_ONLY;

		//since we know the unit was far out originally, this is guaranteed to be actual movement
		if (!pUnit->GeneratePath(pTarget, iFlags, kTactical.GetRecruitRange()))
			continue;

		//we are not here to fight or flee, let other moves take over
		int iDanger = pUnit->GetDanger(pUnit->GetPathEndFirstTurnPlot());
		int iDangerLimit = (pUnit->IsCanAttack() && pUnit->AI_getUnitAIType()!=UNITAI_CITY_BOMBARD) ? pUnit->GetCurrHitPoints() / 2 : 0;
		//generals and siege should not even be in fog danger
		if (iDanger > iDangerLimit)
			continue;

		kTactical.ExecuteMoveToPlot(pUnit, pTarget, true, iFlags);
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

// Copy of the per-unit body of CvTacticalAI::ExecuteWithdrawMoves. Change: the destination is given, in place of the
// best neighboring zone city or the nearest city.
bool CvTacticalOperationalLoop::WithdrawUnitToward(CvTacticalAI& kTactical, CvUnit* pUnit, CvPlot* pDestination)
{
	if (!pUnit)
		return false;

	if (pDestination && kTactical.MoveToEmptySpaceNearTarget(pUnit, pDestination, pUnit->getDomainType(), 12, true))
	{
		TacticalAIHelpers::PerformRangedOpportunityAttack(pUnit, false);
		kTactical.UnitProcessed(pUnit->GetID());

		if(GC.getLogging() && GC.getAILogging())
		{
			CvString strLogString;
			strLogString.Format("%s %d withdrew from (%d,%d) towards (%d,%d)",
				pUnit->getName().GetCString(),pUnit->GetID(),pUnit->getX(),pUnit->getY(),pDestination->getX(),pDestination->getY());
			kTactical.LogTacticalMessage(strLogString);
		}
		return true;
	}

	if (pUnit->shouldPillage(pUnit->plot(), true))
		pUnit->PushMission(CvTypes::getMISSION_PILLAGE());

	//now move all units which didn't find a path to a city
	kTactical.ExecuteMovesToSafestPlot(pUnit);
	return false;
}

// Orders fight targets by the loop's priority, highest first
struct PrOperationalTargetPriorityDesc
{
	// Ranks the higher priority first.
	bool operator()(const SOperationalFightTarget& lhs, const SOperationalFightTarget& rhs) const
	{
		return lhs.fPriority > rhs.fPriority;
	}
};

// Copy of CvTacticalAI::SortTargetListAndDropUselessTargets. Changes: targets sort and compare by the loop's priority
// instead of their aux data, and a target that can attack our units is never dropped.
void CvTacticalOperationalLoop::SortTargetsAndDropUseless(PlayerTypes ePlayer, vector<SOperationalFightTarget>& targets)
{
	std::stable_sort(targets.begin(), targets.end(), PrOperationalTargetPriorityDesc());

	vector<SOperationalFightTarget> reducedTargetList;
	CvPlayer& kPlayer = GET_PLAYER(ePlayer);

	//now in the sorted list we can suppress adjacent non-maximum targets
	int iSuppressionRange = 2;
	for (vector<SOperationalFightTarget>::const_iterator it = targets.begin(); it != targets.end(); ++it)
	{
		bool bBetterTargetAdjacent = false;

		//do this only for enemy units in the same domain
		if (it->target.GetTargetType() == AI_TACTICAL_TARGET_ENEMY_COMBAT_UNIT && !it->bCanAttackOurUnits)
		{
			for (vector<SOperationalFightTarget>::const_iterator it2 = reducedTargetList.begin(); it2 != reducedTargetList.end(); ++it2)
			{
				//land zones have id > 0, water zones id < 0. opposite signs means domain mismatch
				int iZone1 = it->target.GetDominanceZone();
				int iZone2 = it2->target.GetDominanceZone();
				if ((iZone1 > 0 && iZone2 < 0) || (iZone1 < 0 && iZone2 > 0))
					continue;

				//if close to one of our cities, make sure we're not dropping it
				CvPlot* pPlot = GC.getMap().plot(it->target.GetTargetX(), it->target.GetTargetY());
				if (kPlayer.GetCityDistanceInPlots(pPlot) < 4)
					continue;

				if (it2->target.GetTargetType() == AI_TACTICAL_TARGET_ENEMY_COMBAT_UNIT)
				{
					if (it2->fPriority >= it->fPriority && plotDistance(it2->target.GetTargetX(), it2->target.GetTargetY(), it->target.GetTargetX(), it->target.GetTargetY()) <= iSuppressionRange)
					{
						bBetterTargetAdjacent = true;
						break;
					}
				}
			}
		}

		if (!bBetterTargetAdjacent)
			reducedTargetList.push_back(*it);
	}

	targets = reducedTargetList;
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
