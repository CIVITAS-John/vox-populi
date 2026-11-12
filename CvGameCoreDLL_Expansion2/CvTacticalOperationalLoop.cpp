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
};
typedef char OperationalKindRowsMatchKinds[(sizeof(OPERATIONAL_KIND_FLAVOR_ROWS) / sizeof(OPERATIONAL_KIND_FLAVOR_ROWS[0]) == NUM_OPERATIONAL_KINDS) ? 1 : -1];

const char* const OPERATIONAL_POSTURE_NAMES[] =
{
	"NONE", "WITHDRAW", "ATTRITION", "EXPLOIT_FLANKS", "STEAMROLL", "SURGICAL_CITY_STRIKE", "HEDGEHOG", "COUNTERATTACK",
};
typedef char OperationalPostureNamesMatchPostures[(sizeof(OPERATIONAL_POSTURE_NAMES) / sizeof(OPERATIONAL_POSTURE_NAMES[0]) == NUM_OPERATIONAL_POSTURES) ? 1 : -1];

const char* const OPERATIONAL_KIND_NAMES[] =
{
	"UNIT_FIGHT", "CITY_FIGHT", "REINFORCE",
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

// Picks the top ready fight, checks it, executes it on a pass, retires the fights it covered, then estimates every
// open fight again and wakes waiting fights on helpful changes.
void CvTacticalOperationalLoop::RunCommandLoop(CvTacticalAI& kTactical, SOperationalTurnRecord* pRecord)
{
	SOperationalTurn turn;
	BuildTurn(kTactical, turn);
	BuildBattlefields(kTactical, turn);
	BuildThreats(turn);
	for (size_t i = 0; i < turn.battlefields.size(); i++)
	{
		SOperationalBattlefield& field = turn.battlefields[i];
		EstimateFight(turn, field);
		if (field.eFight != OPERATIONAL_OFFER_DROPPED)
			field.eFight = field.fEstimate > 0.0 ? OPERATIONAL_OFFER_READY : OPERATIONAL_OFFER_INACTIVE;
	}

	SOperationalTurnRecord localRecord;
	SOperationalTurnRecord& record = pRecord ? *pRecord : localRecord;
	const bool bLog = !pRecord && GC.getLogging() && GC.getAILogging();
	const int iMaxFailures = std::max(1, static_cast<int>(turn.weights.afConstant[OPERATIONAL_CONSTANT_READINESS_MAX_FAILURES]));
	int iCommands = 0;
	for (;;)
	{
		//the ready fight with the highest estimate; ties go to cities, then the lower plot, then build order
		int iBest = -1;
		for (size_t i = 0; i < turn.battlefields.size(); i++)
		{
			const SOperationalBattlefield& field = turn.battlefields[i];
			if (field.eFight != OPERATIONAL_OFFER_READY)
				continue;
			if (iBest >= 0)
			{
				const SOperationalBattlefield& best = turn.battlefields[iBest];
				if (field.fEstimate < best.fEstimate)
					continue;
				if (field.fEstimate == best.fEstimate)
				{
					if (field.eCenter != best.eCenter && field.eCenter != OPERATIONAL_CENTER_ENEMY_CITY)
						continue;
					if (field.eCenter == best.eCenter && field.iPlotIndex >= best.iPlotIndex)
						continue;
				}
			}
			iBest = static_cast<int>(i);
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
		const eOperationalCheckResult eResult = CheckFight(kTactical, field);
		SOperationalCheckRecord check = { iBest, eResult };
		record.checks.push_back(check);
		if (bLog)
		{
			static const char* const aszResults[] = { "passed", "failed: target gone", "failed: no recruits", "failed: no damage",
				"failed: no capturer" };
			kTactical.LogTacticalMessage(CvString::format("Operational check of fight %d at plot %d, estimate %.6f: %s",
				iBest, field.iPlotIndex, field.fEstimate, aszResults[eResult]));
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

		//the record is kept before the fight runs, so a command that ends the run is still recorded
		record.commands.push_back(SOperationalCommandRecord());
		SOperationalCommandRecord& command = record.commands.back();
		command.iCommand = iCommands;
		command.iBattlefield = iBest;
		command.iPlotIndex = field.iPlotIndex;
		command.eKind = field.eCenter == OPERATIONAL_CENTER_ENEMY_CITY ? OPERATIONAL_KIND_CITY_FIGHT : OPERATIONAL_KIND_UNIT_FIGHT;
		command.eOwner = field.eOwner;
		command.fEstimate = field.fEstimate;
		command.iFailuresBefore = field.iFailures;

		SOperationalBoard before;
		SnapshotBoard(turn, before);
		BeginCommand(kTactical, iCommands, field.iZoneID);
		ExecuteFight(kTactical, turn, field, command);
		EndCommand(kTactical);
		iCommands++;
		field.eFight = OPERATIONAL_OFFER_EXECUTED;

		//the exemptions read the threats after the command, since its search may have moved units into a target's reach
		BuildThreats(turn);

		//an enemy-unit fight that ran its search covered the unit fights around its target
		if (command.bSearchRan)
		{
			for (size_t i = 0; i < turn.battlefields.size(); i++)
			{
				SOperationalBattlefield& other = turn.battlefields[i];
				if (RetiresFight(field, other))
				{
					other.eFight = OPERATIONAL_OFFER_RETIRED;
					command.vRetired.push_back(other.iID);
				}
			}
		}

		SOperationalBoard after;
		SnapshotBoard(turn, after);
		SOperationalChanges changes;
		DiffBoard(turn.ePlayer, before, after, changes);
		if (bLog)
			kTactical.LogTacticalMessage(CvString::format("Operational command %d: fight %d at plot %d, search %s, %d retired",
				command.iCommand, iBest, command.iPlotIndex, command.bSearchRan ? "ran" : "did not run", (int)command.vRetired.size()));

		//estimate every open fight on the new board
		for (size_t i = 0; i < turn.battlefields.size(); i++)
		{
			SOperationalBattlefield& other = turn.battlefields[i];
			if (other.eFight == OPERATIONAL_OFFER_EXECUTED || other.eFight == OPERATIONAL_OFFER_RETIRED ||
				other.eFight == OPERATIONAL_OFFER_SKIPPED || other.eFight == OPERATIONAL_OFFER_DROPPED)
				continue;
			EstimateFight(turn, other);
			if (other.eFight == OPERATIONAL_OFFER_DROPPED)
				continue;
			if (other.eFight == OPERATIONAL_OFFER_WAITING)
			{
				if (other.fEstimate > 0.0 && WakesFight(turn.ePlayer, turn, other, changes))
					other.eFight = OPERATIONAL_OFFER_READY;
			}
			else
				other.eFight = other.fEstimate > 0.0 ? OPERATIONAL_OFFER_READY : OPERATIONAL_OFFER_INACTIVE;
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
	turn.fArmyScale = ReferenceScale(fArmy, TypicalUnitBaseWorth(ePlayer), gradeUnitScaleFloorUnits);

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
// only the endangered units that can still act.
void CvTacticalOperationalLoop::BuildThreats(SOperationalTurn& turn)
{
	turn.unitThreats.clear();
	turn.cityThreats.clear();
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

	for (size_t i = 0; i < turn.battlefields.size(); i++)
	{
		SOperationalBattlefield& field = turn.battlefields[i];
		const pair<int, int> key(static_cast<int>(field.eOwner), field.iTargetID);
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
// battlefield. Targets that are no longer hostile, such as a city captured earlier in the turn, get none.
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
		turn.battlefields.push_back(field);
	}
}

// Unit fight: weighted prize, ground and exposure share. City fight: weighted siege, garrison prize and the exposure
// share of the city and its garrison. Terms over a zero scale count 0.
void CvTacticalOperationalLoop::EstimateFight(const SOperationalTurn& turn, SOperationalBattlefield& field)
{
	const double* afWeight = turn.weights.afWeight;
	const double fPrizeFloor = turn.weights.afConstant[OPERATIONAL_CONSTANT_PRIZE_FLOOR];
	const double fArmyScale = turn.fArmyScale;
	SOperationalStake stake;
	field.fEstimate = 0.0;
	if (field.eOwner == NO_PLAYER)
	{
		field.eFight = OPERATIONAL_OFFER_DROPPED;
		return;
	}
	if (field.eCenter == OPERATIONAL_CENTER_ENEMY_UNIT)
	{
		CvUnit* pUnit = GET_PLAYER(field.eOwner).getUnit(field.iTargetID);
		if (!pUnit || pUnit->isDelayedDeath() || !pUnit->plot())
		{
			field.eFight = OPERATIONAL_OFFER_DROPPED;
			return;
		}
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
	else
	{
		CvCity* pCity = GET_PLAYER(field.eOwner).getCity(field.iTargetID);
		if (!pCity || pCity->getOwner() != field.eOwner)
		{
			field.eFight = OPERATIONAL_OFFER_DROPPED;
			return;
		}
		const double fCityStake = turn.fCityScale > 0.0 ? CityValue(*pCity, turn.ePlayer) / turn.fCityScale : 0.0;
		stake.fSiege = afWeight[OPERATIONAL_WEIGHT_OCCUPATION] * fCityStake *
			(turn.weights.afConstant[OPERATIONAL_CONSTANT_SIEGE_FLOOR] + DamageShare(pCity->getDamage(), pCity->GetMaxHitPoints()));
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
			stake.fGarrisonPrize = pGarrison ? afWeight[OPERATIONAL_WEIGHT_ATTRITION] * UnitPrize(*pGarrison, fPrizeFloor) / fArmyScale : 0.0;
			stake.fExposure = afWeight[OPERATIONAL_WEIGHT_RISK] * fExposure / fArmyScale;
		}
		stake.fStake = stake.fSiege + stake.fGarrisonPrize + stake.fExposure;
	}
	field.stake = stake;
	field.fEstimate = field.fTheater * stake.fStake;
}

//------------------------------------------------------------------------------
// readiness and execution
//------------------------------------------------------------------------------

// A melee combat unit with moves and an attack left that may enter cities.
bool CvTacticalOperationalLoop::CanCapture(const CvUnit& kUnit)
{
	return kUnit.IsCombatUnit() && kUnit.canMove() && !kUnit.isOutOfAttacks() && !kUnit.IsCanAttackRanged() && !kUnit.isNoCapture();
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

// A unit fight passes when a city can strike its target, and any fight passes when native recruitment finds units with
// positive expected damage; a city at 1 HP or less also needs a recruit that can capture it.
eOperationalCheckResult CvTacticalOperationalLoop::CheckFight(CvTacticalAI& kTactical, const SOperationalBattlefield& field)
{
	CvPlot* pPlot = GC.getMap().plotByIndex(field.iPlotIndex);
	if (!pPlot)
		return OPERATIONAL_CHECK_TARGET_GONE;
	const PlayerTypes ePlayer = kTactical.m_pPlayer->GetID();
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
	if (kTactical.ComputeTotalExpectedDamage(target) <= 0)
		return OPERATIONAL_CHECK_NO_DAMAGE;
	CvCity* pCity = field.eCenter == OPERATIONAL_CENTER_ENEMY_CITY ? pPlot->getPlotCity() : NULL;
	if (!pCity || pCity->GetMaxHitPoints() - pCity->getDamage() > 1)
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
// blend and aggression from RISK; and the executed rows mark each endangered unit placed or free.
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
	//takes the city strike label and a unit fight the attrition label.
	static const AITacticalMove aePostureMoves[NUM_OPERATIONAL_POSTURES] =
	{
		AI_TACTICAL_MOVE_NONE, AI_TACTICAL_WITHDRAW, AI_TACTICAL_ATTRITION, AI_TACTICAL_FLANKATTACK, AI_TACTICAL_STEAMROLL,
		AI_TACTICAL_SURGICAL_STRIKE, AI_TACTICAL_HEDGEHOG, AI_TACTICAL_COUNTERATTACK,
	};
	const CvTacticalDominanceZone* pZone = kTactical.GetTacticalAnalysisMap()->GetZoneByIDWithoutRefresh(field.iZoneID);
	const int iPosture = pZone ? range((int)pZone->GetPosture(), 0, NUM_OPERATIONAL_POSTURES - 1) : (int)TACTICAL_POSTURE_NONE;
	AITacticalMove eMove = aePostureMoves[iPosture];
	if (eMove == AI_TACTICAL_MOVE_NONE)
		eMove = bCity ? AI_TACTICAL_SURGICAL_STRIKE : AI_TACTICAL_ATTRITION;
	kTactical.m_CurrentMoveUnits.setCurrentTacticalMove(eMove);

	// Might be able to hit/kill with a city
	if (!bCity)
	{
		CvUnit* pDefender = pPlot->getBestDefender(NO_PLAYER, ePlayer, NULL, true, true);
		if (!pDefender || pDefender->isDelayedDeath())
			return;
		if (kTactical.FindCitiesWithinStrikingDistance(pPlot) && kTactical.ExecuteAttackWithCities(pDefender))
			return;
	}

	// Now the real deal; without recruits or damage the command ends after city fire
	if (!kTactical.FindUnitsWithinStrikingDistance(pPlot) || kTactical.ComputeTotalExpectedDamage(target) <= 0)
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
				turn.aiMapped, turn.settings, report);
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

	//a move or attack that ran places a unit; a finish or hold places it only in an attempt that completed
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

//------------------------------------------------------------------------------
// overlap and wake
//------------------------------------------------------------------------------

// Native's suppression geometry, applied around an executed enemy-unit fight: a compatible zone and 2 plots, with
// targets near our city or able to attack our units exempt. The caller checks that the executed fight ran its search.
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

// Lists the visible living hostile units, the owner of each city battlefield's plot, and our living units' plots.
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
		if (turn.battlefields[i].eCenter != OPERATIONAL_CENTER_ENEMY_CITY)
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
// striking distance, as native's distance reject reads it, and aircraft and port-changing ships always count.
bool CvTacticalOperationalLoop::WakesFight(PlayerTypes ePlayer, const SOperationalTurn& turn, const SOperationalBattlefield& field,
	const SOperationalChanges& changes)
{
	CvPlot* pTarget = GC.getMap().plotByIndex(field.iPlotIndex);
	if (!pTarget)
		return false;
	for (size_t i = 0; i < changes.vEnemyPlots.size(); i++)
	{
		CvPlot* pPlot = GC.getMap().plotByIndex(changes.vEnemyPlots[i]);
		if (pPlot && plotDistance(*pPlot, *pTarget) <= turn.iStrikeScan)
			return true;
	}
	for (size_t i = 0; i < changes.vCapturedPlots.size(); i++)
	{
		CvPlot* pPlot = GC.getMap().plotByIndex(changes.vCapturedPlots[i]);
		if (pPlot && plotDistance(*pPlot, *pTarget) <= turn.iStrikeScan)
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
		if (plotDistance(*pUnit->plot(), *pTarget) <= pUnit->baseMoves(false) * 4)
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
		const bool bHostileNoCity = !bOurs && !pPlot->isCity() && GroundSide(eUs, pPlot->getOwner()) == groundHoldsHostile;
		AddGroundPlot(bOurs, bHostileNoCity, GroundPlotWeight(eUs, *pPlot), iStand, iNearby, fLost, fCredit);
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

// Averages the base worth of the military unit types any of our cities could buy with gold. A type counts when a
// non-puppet city can train it and holds no investment in it; the check skips the gold on hand, the purchase cooldown,
// resistance, placement and building purchase requirements, so it reads which types are available, not affordable.
// The building counts are built once, as the capture's purchase rows build them.
double CvTacticalOperationalLoop::TypicalUnitBaseWorth(PlayerTypes ePlayer)
{
	CvPlayer& kPlayer = GET_PLAYER(ePlayer);
	std::vector<int> buildingCounts(GC.getNumBuildingInfos(), 0);
	int iLoop = 0;
	for (const CvCity* pCity = kPlayer.firstCity(&iLoop); pCity != NULL; pCity = kPlayer.nextCity(&iLoop))
	{
		const std::vector<BuildingTypes>& buildings = pCity->GetCityBuildings()->GetAllBuildingsHere();
		for (size_t i = 0; i < buildings.size(); i++)
			buildingCounts[buildings[i]]++;
	}
	double fTotal = 0.0;
	int iTypes = 0;
	for (int iUnit = 0; iUnit < GC.getNumUnitInfos(); iUnit++)
	{
		const UnitTypes eUnit = static_cast<UnitTypes>(iUnit);
		CvUnitEntry* pInfo = GC.getUnitInfo(eUnit);
		if (!pInfo || (pInfo->GetCombat() <= 0 && pInfo->GetRangedCombat() <= 0))
			continue;
		bool bBuyable = false;
		for (CvCity* pCity = kPlayer.firstCity(&iLoop); pCity != NULL && !bBuyable; pCity = kPlayer.nextCity(&iLoop))
			bBuyable = pCity->IsCanPurchase(buildingCounts, false, true, eUnit, NO_BUILDING, NO_PROJECT, YIELD_GOLD);
		if (!bBuyable)
			continue;
		fTotal += UnitBaseWorth(pInfo->GetPower(), pInfo->GetProductionCost());
		iTypes++;
	}
	return iTypes > 0 ? fTotal / iTypes : 0.0;
}

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
