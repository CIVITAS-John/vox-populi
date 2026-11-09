/*	-------------------------------------------------------------------------------------------------------
	Vox Deorum: the operational loop (VOX_TACTICAL_OPERATIONAL_LOOP). With the flag on, a non-barbarian player's
	ProcessDominanceZones hands its turn to this file's copy, which runs a global loop of battlefields in place of
	the zone posture routines and the reinforcement sweep. This file holds the settings and their accessor, the
	posture and kind flavor rows, the mapped military flavors, the geographic blend, aggression from RISK, and the
	copies of the native functions the loop changes. Each copy names its native original. Include after
	CvTacticalAI.h.
	------------------------------------------------------------------------------------------------------- */
#pragma once

#ifndef CIV5_TACTICAL_OPERATIONAL_LOOP_H
#define CIV5_TACTICAL_OPERATIONAL_LOOP_H

#include "CvTacticalOperationalWeights.h"

struct CvDangerPlotContents;

// one posture row per eTacticalPosture value
const int NUM_OPERATIONAL_POSTURES = TACTICAL_POSTURE_COUNTERATTACK + 1;

// The command kinds that carry a kind flavor row
enum eOperationalKind
{
	OPERATIONAL_KIND_UNIT_FIGHT,
	OPERATIONAL_KIND_CITY_FIGHT,
	OPERATIONAL_KIND_REINFORCE,
	OPERATIONAL_KIND_WITHDRAW,
	NUM_OPERATIONAL_KINDS
};

// Additive flavor offsets in 0..100 points per posture and per command kind, in military flavor slot order
extern const signed char OPERATIONAL_POSTURE_FLAVOR_ROWS[][STacticalFlavors::NUM_SLOTS];
extern const signed char OPERATIONAL_KIND_FLAVOR_ROWS[][STacticalFlavors::NUM_SLOTS];
// The row names templates override them by: posture.<NAME> and kind.<NAME>
extern const char* const OPERATIONAL_POSTURE_NAMES[];
extern const char* const OPERATIONAL_KIND_NAMES[];

// One player's loop settings. The game runs the defines and tables; the simulator can override any of them.
struct OperationalSettings
{
	// most commands per player-turn
	int iMaxCommands;
	// lowest blended RISK for Medium, High and Braveheart aggression
	int iAggressionMediumRisk;
	int iAggressionHighRisk;
	int iAggressionBraveheartRisk;
	int aiPostureRows[NUM_OPERATIONAL_POSTURES][STacticalFlavors::NUM_SLOTS];
	int aiKindRows[NUM_OPERATIONAL_KINDS][STacticalFlavors::NUM_SLOTS];
	// the weights file's base weights, flavor slopes and constants
	double afWeightBase[NUM_OPERATIONAL_WEIGHTS];
	double afFlavorSlope[STacticalFlavors::NUM_SLOTS];
	double afConstant[NUM_OPERATIONAL_CONSTANTS];
};

// Fills the settings from the game defines and the tables.
void GetDefaultOperationalSettings(OperationalSettings* pSettings);
// Fills one player's settings: the defaults, which the simulator overrides by the player's role.
void GetOperationalSettings(PlayerTypes ePlayer, OperationalSettings* pSettings);

// Maps blended RISK to an aggression level by the settings' thresholds; below the Medium threshold is Low.
eAggressionLevel AggressionFromRisk(int iRisk, const OperationalSettings& settings);

// The layer of a VP flavor a mapping term reads
enum eVpFlavorLayer
{
	VP_FLAVOR_LAYER_ACTIVE,      // the whole flavor
	VP_FLAVOR_LAYER_PERSONALITY, // the leader's own personality, or the custom value
	VP_FLAVOR_LAYER_INTENT,      // 50 plus the grand strategy's mod, or the custom value
	VP_FLAVOR_LAYER_STRATEGY,    // the strategy rows' change
	NUM_VP_FLAVOR_LAYERS
};

// One VP flavor as the flavor manager holds it
struct SVpFlavorLayers
{
	bool bFound;  // the player has the flavor
	int iActive;  // personality plus strategy rows plus any custom value's game-scale change
	int iBase;    // the leader's own personality
	int iGrand;   // the personality plus the grand strategy's mod, on the personality scale
	int iCustom;  // the Vox Deorum custom value on the 0 to 100 scale, or -1 when none is active

	// Creates a flavor the player does not have.
	SVpFlavorLayers() : bFound(false), iActive(0), iBase(0), iGrand(0), iCustom(-1) {}
};

// One term of a military flavor's mapping: one layer of a native VP flavor, or another military slot (iSlotTerm >= 0,
// szFlavor NULL), with the weight its deviation from 50 carries
struct SOperationalVpTerm
{
	int iSlot;
	const char* szFlavor;
	eVpFlavorLayer eLayer;
	int iSlotTerm;
	double fWeight;
};

// The shipped mapping from VP flavors to the military flavors
extern const SOperationalVpTerm OPERATIONAL_VP_TERMS[];
extern const int NUM_OPERATIONAL_VP_TERMS;

// Converts an active flavor back to the 0 to 100 value a custom flavor would set: the value whose scaled personality
// plus game-scale change comes closest, ties toward 50.
int MapVpFlavor(int iActive, int iPersonalityMax);
// Reads one VP flavor on the 0 to 100 scale: from the custom value while one is active, otherwise MapVpFlavor.
int MapVpFlavorRow(const SVpFlavorLayers& flavor, int iPersonalityMax);
// Reads one layer of a VP flavor on the 0 to 100 scale with 50 neutral.
int ReadVpFlavorLayer(const SVpFlavorLayers& flavor, eVpFlavorLayer eLayer, int iPersonalityMax);
// Computes the military flavors from the terms: 50 plus each term's weight times its input's deviation from 50,
// rounded and clamped to 0..100; slots without terms are -1. aTermFlavors holds each native term's flavor in term
// order. A FLAVOR_RISK term reads offense instead when the player lacks it or its own personality has it at zero
// without a custom value. A slot term reads its slot's unrounded value, so slots whose terms are all native go first.
// Returns false with piMissingTerm set when a term's flavor is missing; aiTermInputs, when set, receives each native
// term's input.
bool ComputeMilitaryFlavors(const SOperationalVpTerm* aTerms, int nTerms, const SVpFlavorLayers* aTermFlavors,
	const SVpFlavorLayers& offense, int iPersonalityMax, int (&aiFlavors)[STacticalFlavors::NUM_SLOTS],
	int* piMissingTerm = NULL, int* aiTermInputs = NULL);
// Reads one VP flavor of a player from its flavor manager.
SVpFlavorLayers ReadVpFlavorLayers(CvPlayer& kPlayer, FlavorTypes eFlavor);
// Computes a player's mapped military flavors with the shipped mapping; a slot that cannot be read is 50.
void GetMappedMilitaryFlavors(PlayerTypes ePlayer, int (&aiFlavors)[STacticalFlavors::NUM_SLOTS]);

// One zone's part of a search's geometry: its plot count and its flavors after modifiers and posture row
struct SOperationalZoneFlavors
{
	int iZoneID;
	int iPlots;
	int aiFlavor[STacticalFlavors::NUM_SLOTS];
};

// Blends zone flavors by plot share, adds the kind row, clamps to 0..100 and rounds once. With no zones the mapped
// flavors stand alone. The representative zone has the most plots, ties to the lower ID, or -1 without zones.
void BlendZoneFlavors(const vector<SOperationalZoneFlavors>& zones, const int (&aiMapped)[STacticalFlavors::NUM_SLOTS],
	const int (&aiKindRow)[STacticalFlavors::NUM_SLOTS], int (&aiFlavors)[STacticalFlavors::NUM_SLOTS], int& iRepresentativeZone);

// One threat's part of a unit's danger: an enemy unit or city and the damage it adds
struct SOperationalThreatDamage
{
	PlayerTypes eOwner;
	int iID;
	bool bCity;
	int iDamage;
};

// One search attempt of an engagement: its plan, how many rows ran, and whether it ran to the end
struct SOperationalAttempt
{
	vector<STacticalAssignment> vRows;
	size_t nRowsRun;
	bool bCompleted;
};

// What one engagement did: its geometry, flavors, aggression and attempts
struct SOperationalEngagementReport
{
	map<int, int> zonePlots;
	int iRepresentativeZone;
	STacticalFlavors flavors;
	eAggressionLevel eAggLvl;
	vector<SOperationalAttempt> attempts;
	bool bSuccess;

	// Creates an empty report.
	SOperationalEngagementReport() : iRepresentativeZone(-1), eAggLvl(AL_LOW), bSuccess(false) {}
};

// One fight target with the loop's priority, for target suppression
struct SOperationalFightTarget
{
	CvTacticalTarget target;
	double fPriority;
	// the target can attack one of our units next turn, so its fight carries their exposure
	bool bCanAttackOurUnits;
};

// The loop's copies of native tactical functions, and the loop itself. CvTacticalAI declares it a friend.
class CvTacticalOperationalLoop
{
public:
	// Copy of CvTacticalAI::ProcessDominanceZones for a non-barbarian player: global high priority; the zone loop runs
	// emergency purchases only; the command loop; deferred garrisons; global mid and low priority; the review.
	static void ProcessDominanceZones(CvTacticalAI& kTactical);
	// Runs commands until none is ready. Battlefields arrive with the fight loop, so no command runs yet.
	static void RunCommandLoop(CvTacticalAI& kTactical);

	// Copy of TacticalAIHelpers::FindAndExecuteBestUnitAssignments for one engagement: gathers the geometry first,
	// blends the flavors over it with the kind row, derives aggression from RISK, then searches and executes.
	static bool ExecuteEngagement(PlayerTypes ePlayer, vector<CvUnit*>& vUnits, CvPlot* pTarget, eOperationalKind eKind,
		PlayerTypes eCounterpart, const int (&aiMapped)[STacticalFlavors::NUM_SLOTS], const OperationalSettings& settings,
		SOperationalEngagementReport& report);
	// The retry loop of TacticalAIHelpers::FindAndExecuteBestUnitAssignments with given flavors and aggression: native
	// FindBestUnitAssignments unchanged, up to four attempts, each recorded with the rows that ran.
	static bool SearchAndExecute(PlayerTypes ePlayer, vector<CvUnit*>& vUnits, CvPlot* pTarget, eAggressionLevel eAggLvl,
		const STacticalFlavors& flavors, SOperationalEngagementReport& report);
	// Copy of the plot gathering in TacticalAIHelpers::FindBestUnitAssignments: the search's plots for all units before
	// the participant cap, without remote air bases, counted by zone. Zone 0, the unknown zone, is left out.
	static void GatherSearchPlotZones(const vector<CvUnit*>& vUnits, CvPlot* pTarget, map<int, int>& zonePlots);
	// The geographic blend over a search's zones: per zone the mapped flavors moved by the civ, operation and zone
	// modifiers plus its posture row, blended by plot share, plus the kind row. RISK always counts as set.
	static STacticalFlavors BlendEngagementFlavors(PlayerTypes ePlayer, const map<int, int>& zonePlots, eOperationalKind eKind,
		PlayerTypes eCounterpart, int iOperationID, const int (&aiMapped)[STacticalFlavors::NUM_SLOTS],
		const OperationalSettings& settings, int& iRepresentativeZone);

	// Copy of CvTacticalAI::PositionUnitsAroundTarget with given flavors and aggression in place of AL_LOW and the search
	// intent. The spotter and path moves skip held garrisons, and land units take path moves with the safe-embark flag.
	static bool PositionUnitsAroundTarget(CvTacticalAI& kTactical, const vector<CvUnit*>& vUnits, CvPlot* pTarget,
		const STacticalFlavors& flavors, eAggressionLevel eAggLvl, SOperationalEngagementReport& report);
	// The per-unit move of CvTacticalAI::ExecuteWithdrawMoves toward a given destination: the withdraw move and the
	// ranged opportunity shot, or without a path a pillage and the safest plot. True when the withdraw move was made.
	static bool WithdrawUnitToward(CvTacticalAI& kTactical, CvUnit* pUnit, CvPlot* pDestination);
	// Copy of CvTacticalAI::SortTargetListAndDropUselessTargets on the loop's priorities: highest first, dropping an enemy
	// unit within 2 plots of one at least as high, unless it is near our city or can attack our units.
	static void SortTargetsAndDropUseless(PlayerTypes ePlayer, vector<SOperationalFightTarget>& targets);
	// Copy of the unit overload of CvDangerPlotContents::GetDanger: the same total, and each enemy unit's and city's damage
	// as it adds them up. A total that is not a sum (a civilian that can be captured, a unit in our city) lists no threats.
	static int GetDangerByThreat(CvDangerPlotContents& kContents, const CvUnit* pUnit, const SUnitIDValueContainer& extraUnitDamage,
		int iExtraDamage, AirActionType eAirAction, vector<SOperationalThreatDamage>& threats);

	// True for a city's garrison the garrison pass left to zone work: only a search may move it.
	static bool IsHeldGarrison(const CvUnit* pUnit);
};

#endif //CIV5_TACTICAL_OPERATIONAL_LOOP_H
