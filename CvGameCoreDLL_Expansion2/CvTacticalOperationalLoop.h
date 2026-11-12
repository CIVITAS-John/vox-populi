/*	-------------------------------------------------------------------------------------------------------
	Vox Deorum: the operational loop (VOX_TACTICAL_OPERATIONAL_LOOP). With the flag on, a non-barbarian player's
	ProcessDominanceZones hands its turn to this file's copy, which runs a global loop of battlefields in place of
	the zone posture routines and the reinforcement sweep. The loop runs fights on enemy units, enemy cities and our own
	cities. This file holds the settings and their accessor, the posture and kind flavor rows, the mapped military flavors, the
	geographic blend, aggression from RISK, the loop's battlefields and offers, and the copies of the native
	functions the loop changes. Each copy names its native original. Include after CvTacticalAI.h.
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
	OPERATIONAL_KIND_OWN_CITY_FIGHT,
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

// What a battlefield is built around
enum eOperationalCenter
{
	OPERATIONAL_CENTER_ENEMY_UNIT,
	OPERATIONAL_CENTER_ENEMY_CITY,
	OPERATIONAL_CENTER_OUR_CITY
};

// Where an offer stands in the turn's queue
enum eOperationalOfferState
{
	OPERATIONAL_OFFER_READY,     // positive estimate, waiting for its turn at the top
	OPERATIONAL_OFFER_INACTIVE,  // estimate 0; a later positive estimate makes it ready
	OPERATIONAL_OFFER_WAITING,   // failed its readiness check; wakes on a helpful change
	OPERATIONAL_OFFER_SKIPPED,   // failed readiness.maxFailures checks; out for the turn
	OPERATIONAL_OFFER_RETIRED,   // an overlapping executed fight covered it; out for the turn
	OPERATIONAL_OFFER_EXECUTED,  // ran as a command
	OPERATIONAL_OFFER_DROPPED    // its target died or its city changed owner
};

// One enemy threat's share of our units' exposure, and the units of ours it can attack next turn
struct SOperationalThreatExposure
{
	double fExposure;
	vector<int> vUnitIDs;

	// Creates a threat that exposes nothing.
	SOperationalThreatExposure() : fExposure(0.0) {}
};

// The terms of one fight's stake before theater, each already weighted and scaled: a unit fight's prize, ground and
// exposure share; a city fight's siege, garrison prize and exposure share; or an own-city fight's city pressure and
// defensive exposure; and their sum
struct SOperationalStake
{
	double fPrize;
	double fGround;
	double fExposure;      // for an own-city fight, the endangered recruits' exposure
	double fSiege;
	double fGarrisonPrize;
	double fCityPressure;  // the city's stake times the enemy's strength share
	double fStake;

	// Creates an empty stake.
	SOperationalStake() : fPrize(0.0), fGround(0.0), fExposure(0.0), fSiege(0.0), fGarrisonPrize(0.0), fCityPressure(0.0),
		fStake(0.0) {}
};

// One battlefield of the turn and its fight offer
struct SOperationalBattlefield
{
	int iID;                      // index in the turn's list
	eOperationalCenter eCenter;
	int iPlotIndex;
	int iZoneID;
	PlayerTypes eOwner;           // the target's owner
	int iTargetID;                // enemy unit ID or city ID, or our city's ID: the identity for the turn
	double fTheater;              // fixed for the turn
	double fGround;               // fixed for the turn; unit targets only
	bool bNearOurCity;            // fixed: within 3 plots of one of our cities
	bool bCanAttackOurUnits;      // rebuilt with the threat maps after each command
	eOperationalOfferState eFight;
	double fEstimate;             // theater times stake
	int iFailures;
	SOperationalStake stake;
	double fLocalStrength;        // own city: our strength L around it, from the last estimate
	double fEnemyStrength;        // own city: the enemy strength T against it, from the last estimate
	bool bHasEndangered;          // own city: it had an endangered recruit at the last estimate

	// Creates a ready battlefield with no target.
	SOperationalBattlefield() : iID(-1), eCenter(OPERATIONAL_CENTER_ENEMY_UNIT), iPlotIndex(-1), iZoneID(0), eOwner(NO_PLAYER),
		iTargetID(-1), fTheater(0.0), fGround(0.0), bNearOurCity(false), bCanAttackOurUnits(false),
		eFight(OPERATIONAL_OFFER_READY), fEstimate(0.0), iFailures(0), fLocalStrength(0.0), fEnemyStrength(0.0),
		bHasEndangered(false) {}
};

// One player-turn of the loop: settings, weights, the fixed scales, the threat maps and the battlefields
struct SOperationalTurn
{
	PlayerTypes ePlayer;
	OperationalSettings settings;
	OperationalWeights weights;
	int aiMapped[STacticalFlavors::NUM_SLOTS];
	double fArmyScale;   // Z_U, fixed for the turn
	double fCityScale;   // Z_C, fixed for the turn
	double fGroundBase;  // B, fixed for the turn
	// each enemy unit's and city's share of our units' exposure, keyed by (owner, ID): rebuilt after each command
	map<pair<int, int>, SOperationalThreatExposure> unitThreats;
	map<pair<int, int>, SOperationalThreatExposure> cityThreats;
	int iStrikeScan;     // the largest baseMoves(false) x 4 among our land and sea units: native's striking distance
	// each city of ours by ID, and the visible enemy combat units by (owner, ID) whose path to it, as the nearest of our
	// cities, is at most twice their moves plus their range: rebuilt after each command
	map<int, vector<pair<int, int> > > cityApproach;
	// our units outside friendly cities that some enemy unit or city can damage next turn: rebuilt after each command
	set<int> endangeredUnits;
	vector<SOperationalBattlefield> battlefields;

	// Creates an empty turn.
	SOperationalTurn() : ePlayer(NO_PLAYER), fArmyScale(0.0), fCityScale(0.0), fGroundBase(0.0), iStrikeScan(0) {}
};

// The board facts a command can change that the wake rule reads
struct SOperationalBoard
{
	map<pair<int, int>, int> enemyUnitPlots;  // visible living enemy units by (owner, ID), and their plots
	map<int, int> cityOwners;                 // each city battlefield's plot and its owner, ours included
	map<int, int> ourUnitPlots;               // our units by ID, and their plots
};

// What one command changed, from the board diff
struct SOperationalChanges
{
	vector<int> vEnemyPlots;        // plots where a visible enemy unit died, left, arrived or was revealed
	vector<int> vCapturedPlots;     // city battlefields whose owner changed
	vector<int> vAvailableUnitIDs;  // our units that moved and can still be used
};

// The result of one readiness check: a pass, or the first condition that failed
enum eOperationalCheckResult
{
	OPERATIONAL_CHECK_PASSED,
	OPERATIONAL_CHECK_TARGET_GONE,  // no living enemy unit stands on a unit target's plot
	OPERATIONAL_CHECK_NO_RECRUITS,  // no unit of ours that can still act is within striking distance
	OPERATIONAL_CHECK_NO_DAMAGE,    // the recruits would deal no expected damage
	OPERATIONAL_CHECK_NO_CAPTURER,  // the city is at 1 HP or less and no recruit can capture it
	OPERATIONAL_CHECK_NO_DEFENDERS, // own city: no local defender and no endangered recruit can still act
	OPERATIONAL_CHECK_NO_NEARBY_ENEMY  // own city: no visible enemy within 5 plots and no endangered recruit
};

// One readiness check and its result
struct SOperationalCheckRecord
{
	int iBattlefield;
	eOperationalCheckResult eResult;
};

// One executed command, for the records
struct SOperationalCommandRecord
{
	int iCommand;
	int iBattlefield;
	int iPlotIndex;
	eOperationalKind eKind;
	PlayerTypes eOwner;
	double fEstimate;           // the estimated priority the fight ran at; its readiness check passes or fails only
	int iFailuresBefore;
	bool bSearchRan;            // the search ran with at least one unit, so overlapping fights retire
	vector<int> vRetired;       // battlefields this command retired
	vector<pair<int, bool> > vEndangered;  // endangered recruits by unit ID, and whether the search placed them

	// Creates an empty record.
	SOperationalCommandRecord() : iCommand(-1), iBattlefield(-1), iPlotIndex(-1), eKind(OPERATIONAL_KIND_UNIT_FIGHT),
		eOwner(NO_PLAYER), fEstimate(0.0), iFailuresBefore(0), bSearchRan(false) {}
};

// One player-turn's minimal records: commands, readiness checks, skipped offers and whether the command cap stopped it
struct SOperationalTurnRecord
{
	vector<SOperationalCommandRecord> commands;
	vector<SOperationalCheckRecord> checks;
	vector<int> vSkipped;
	bool bCommandCap;

	// Creates an empty record.
	SOperationalTurnRecord() : bCommandCap(false) {}
};

// The loop's copies of native tactical functions, and the loop itself. CvTacticalAI declares it a friend.
class CvTacticalOperationalLoop
{
public:
	// Copy of CvTacticalAI::ProcessDominanceZones for a non-barbarian player: global high priority; the zone loop runs
	// emergency purchases only; the command loop; deferred garrisons; global mid and low priority; the review.
	static void ProcessDominanceZones(CvTacticalAI& kTactical);
	// Runs the turn's commands: builds the battlefields from the target snapshot, estimates every fight, then checks the
	// top ready fight and executes it until none is ready or maxCommands commands ran. pRecord, when given, receives the
	// minimal records; otherwise they go to the tactical log when AI logging is on.
	static void RunCommandLoop(CvTacticalAI& kTactical, SOperationalTurnRecord* pRecord = NULL);

	// Resolves the turn's weights and computes our army's value Z_U, our cities' value Z_C, the ground base B and the
	// striking-distance scan.
	static void BuildTurn(CvTacticalAI& kTactical, SOperationalTurn& turn);
	// Rebuilds each enemy unit's and city's share of our units' exposure, our endangered units, each city's approach
	// list and each battlefield's bCanAttackOurUnits.
	static void BuildThreats(SOperationalTurn& turn);
	// Builds one battlefield per hostile combat unit and hostile city target in the snapshot, with its theater, ground and
	// near-city flag, then one per city of ours.
	static void BuildBattlefields(CvTacticalAI& kTactical, SOperationalTurn& turn);
	// Appends our city's battlefield: its land zone, or its water zone without one, and its theater.
	static void AddOwnCityBattlefield(CvTacticalAI& kTactical, SOperationalTurn& turn, CvCity& kCity);
	// Estimates one fight as theater times stake on the current board, or drops it when its target died or its city
	// changed owner.
	static void EstimateFight(const SOperationalTurn& turn, SOperationalBattlefield& field);
	// Our unit's strength: native maximum attack strength, or ranged strength for a ranged unit, times its HP share.
	static double UnitStrength(const CvUnit& kUnit);
	// An enemy unit's strength against our city: its quick attack or ranged strength against the city plot, times its HP
	// share.
	static double UnitStrengthAgainstCity(const CvUnit& kUnit, const CvCity& kCity);
	// A city's strength: its strength value times its HP share.
	static double CityStrength(const CvCity& kCity);
	// Our local strength L around our city: the city plus our land combat units within one turn by plain distance, and
	// sea units at a coastal city. Spent and army units count; aircraft do not.
	static double LocalCityStrength(PlayerTypes ePlayer, const CvCity& kCity);
	// The enemy strength T against our city: its approach list together with the visible units native danger says can
	// attack it next turn, each once.
	static double EnemyCityStrength(const SOperationalTurn& turn, const CvCity& kCity);
	// Our city fight's recruits from the tactical roster: local defenders within one turn, the held garrison, aircraft in
	// reach with their carriers under the air search, and the endangered units within 4 plots. Army units are left out.
	static void GatherCityRecruits(CvTacticalAI& kTactical, const SOperationalTurn& turn, const CvCity& kCity,
		vector<CvUnit*>& vDefenders, vector<CvUnit*>& vEndangered);
	// Fills a native target with the battlefield's type, plot and zone, for native recruitment and damage estimates.
	static CvTacticalTarget FightTarget(const SOperationalBattlefield& field, const CvPlot& kPlot);
	// The fight's readiness check: native city strike and striking-distance recruitment, or for our city its defenders,
	// endangered recruits and nearby enemies, with no orders issued. Returns a pass or the first condition that failed.
	static eOperationalCheckResult CheckFight(CvTacticalAI& kTactical, const SOperationalTurn& turn, const SOperationalBattlefield& field);
	// Copy of CvTacticalAI::ExecuteDestroyEnemyUnits and ExecuteAttackWithUnits for one fight, with endangered recruits
	// and the loop's engagement. Fills the record's search flag and dispositions. Our cities go to ExecuteCityDefense.
	static void ExecuteFight(CvTacticalAI& kTactical, const SOperationalTurn& turn, const SOperationalBattlefield& field,
		SOperationalCommandRecord& record);
	// Defends our city: one search over its recruits with the own-city kind row and the target distance ignored, so
	// units may retreat. No city fire, spotter move, separate air attack or capture follow-up.
	static void ExecuteCityDefense(CvTacticalAI& kTactical, const SOperationalTurn& turn, const SOperationalBattlefield& field,
		SOperationalCommandRecord& record);
	// Marks each endangered recruit in the record placed or free from the engagement's executed rows.
	static void MarkDispositions(const SOperationalEngagementReport& report, SOperationalCommandRecord& record);

	// The fight row of the overlap rule: an enemy-unit fight that ran its search retires an unexecuted enemy-unit fight
	// within 2 plots of its target in a compatible zone, unless that fight's target can attack our units or stands near
	// our city. City fights, ours included, neither retire nor get retired.
	static bool RetiresFight(const SOperationalBattlefield& executed, const SOperationalBattlefield& candidate);
	// Records the board facts the wake rule compares before and after a command.
	static void SnapshotBoard(const SOperationalTurn& turn, SOperationalBoard& board);
	// Compares two snapshots into the changes a command made.
	static void DiffBoard(PlayerTypes ePlayer, const SOperationalBoard& before, const SOperationalBoard& after,
		SOperationalChanges& changes);
	// True when a change can turn the waiting fight's failed check into a pass: an enemy change or capture within
	// striking distance of its target, or a unit of ours that moved within its own striking distance and is available.
	// An own-city fight wakes on an enemy change within 5 plots, an available unit of ours now within one turn, or an
	// endangered recruit.
	static bool WakesFight(PlayerTypes ePlayer, const SOperationalTurn& turn, const SOperationalBattlefield& field,
		const SOperationalChanges& changes);

	// Our unit's worth as the grade counts it: base worth with the level premium, plus a ready upgrade's premium.
	static double OurUnitWorth(const CvUnit& kUnit);
	// An enemy unit's prize: its worth times the prize floor plus the rest in proportion to its damage share.
	static double UnitPrize(const CvUnit& kUnit, double fPrizeFloor);
	// The ground side bit of another player as the grade's sides read it: ours or friendly, hostile, neutral, or 0 for no
	// player.
	static unsigned int GroundSide(PlayerTypes eUs, PlayerTypes eOther);
	// The side bits standing on a plot and blockading from it, as the grade marks them, leaving out pSkip.
	static void PlotPresence(const CvPlot& kPlot, PlayerTypes eUs, const CvUnit* pSkip, unsigned int& iStand,
		unsigned int& iBlock);
	// The ground score of the owned plots among a target plot and its neighbors on its landmass, leaving out pSkip, before
	// dividing by the base.
	static double GroundAround(PlayerTypes eUs, const CvPlot& kTarget, const CvUnit* pSkip);
	// A unit target's ground term: the ground score with the target less the score without it, over the ground base.
	static double TargetGround(PlayerTypes eUs, const CvPlot& kTarget, const CvUnit* pTarget, double fGroundBase);
	// The player's danger contents for one plot, after rebuilding a dirty cache as CvPlayer::GetPlotDanger does.
	static CvDangerPlotContents& DangerContents(PlayerTypes ePlayer, const CvPlot& kPlot);
	// True for a unit that can capture a city: a melee combat unit with moves and an attack left that may enter cities.
	static bool CanCapture(const CvUnit& kUnit);

	// Host seams. The DLL defines them in this file; the simulator defines its own and does not extract these.
	// The mean base worth of the military unit types the player can buy with gold now, or 0 when there are none.
	static double TypicalUnitBaseWorth(PlayerTypes ePlayer);
	// A plot's ground weight with the resources the player's team has revealed.
	static double GroundPlotWeight(PlayerTypes ePlayer, const CvPlot& kPlot);
	// Opens and closes one command's work: the simulator records the command's effects in the battlefield's zone.
	static void BeginCommand(CvTacticalAI& kTactical, int iCommand, int iZoneID);
	static void EndCommand(CvTacticalAI& kTactical);

	// Copy of TacticalAIHelpers::FindAndExecuteBestUnitAssignments for one engagement: gathers the geometry first,
	// blends the flavors over it with the kind row, derives aggression from RISK, then searches and executes.
	// bTargetDistanceRelevant is native's search flag: false lets units end farther from the target than they started.
	static bool ExecuteEngagement(PlayerTypes ePlayer, vector<CvUnit*>& vUnits, CvPlot* pTarget, eOperationalKind eKind,
		PlayerTypes eCounterpart, const int (&aiMapped)[STacticalFlavors::NUM_SLOTS], const OperationalSettings& settings,
		bool bTargetDistanceRelevant, SOperationalEngagementReport& report);
	// The retry loop of TacticalAIHelpers::FindAndExecuteBestUnitAssignments with given flavors, aggression and target
	// distance flag: native FindBestUnitAssignments unchanged, up to four attempts, each recorded with the rows that ran.
	static bool SearchAndExecute(PlayerTypes ePlayer, vector<CvUnit*>& vUnits, CvPlot* pTarget, eAggressionLevel eAggLvl,
		const STacticalFlavors& flavors, bool bTargetDistanceRelevant, SOperationalEngagementReport& report);
	// Copy of the plot gathering in TacticalAIHelpers::FindBestUnitAssignments: the search's plots for all units before
	// the participant cap, without remote air bases, counted by zone. Zone 0, the unknown zone, is left out.
	static void GatherSearchPlotZones(const vector<CvUnit*>& vUnits, CvPlot* pTarget, bool bTargetDistanceRelevant,
		map<int, int>& zonePlots);
	// The geographic blend over a search's zones: per zone the mapped flavors moved by the civ, operation and zone
	// modifiers plus its posture row, blended by plot share, plus the kind row. RISK always counts as set.
	static STacticalFlavors BlendEngagementFlavors(PlayerTypes ePlayer, const map<int, int>& zonePlots, eOperationalKind eKind,
		PlayerTypes eCounterpart, int iOperationID, const int (&aiMapped)[STacticalFlavors::NUM_SLOTS],
		const OperationalSettings& settings, int& iRepresentativeZone);

	// Copy of CvTacticalAI::PositionUnitsAroundTarget with given flavors and aggression in place of AL_LOW and the search
	// intent. The spotter and path moves skip held garrisons, and land units take path moves with the safe-embark flag.
	static bool PositionUnitsAroundTarget(CvTacticalAI& kTactical, const vector<CvUnit*>& vUnits, CvPlot* pTarget,
		const STacticalFlavors& flavors, eAggressionLevel eAggLvl, SOperationalEngagementReport& report);
	// Copy of the unit overload of CvDangerPlotContents::GetDanger: the same total, and each enemy unit's and city's damage
	// as it adds them up. A total that is not a sum (a civilian that can be captured, a unit in our city) lists no threats.
	static int GetDangerByThreat(CvDangerPlotContents& kContents, const CvUnit* pUnit, const SUnitIDValueContainer& extraUnitDamage,
		int iExtraDamage, AirActionType eAirAction, vector<SOperationalThreatDamage>& threats);

	// True for a city's garrison the garrison pass left to zone work: only a search may move it.
	static bool IsHeldGarrison(const CvUnit* pUnit);
};

#endif //CIV5_TACTICAL_OPERATIONAL_LOOP_H
