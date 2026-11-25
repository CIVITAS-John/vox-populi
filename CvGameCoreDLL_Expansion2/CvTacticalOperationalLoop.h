/*	-------------------------------------------------------------------------------------------------------
	The operational loop (VOX_TACTICAL_OPERATIONAL_LOOP). With the flag on, a non-barbarian player's
	ProcessDominanceZones hands its turn to this file's copy, which runs a global loop of battlefields in place of
	the zone posture routines and the reinforcement sweep. The loop runs fights on enemy units, enemy cities and our own
	cities, a Reinforce at each of them that marches distant units in, and a Withdraw at enemy centers that pulls the
	battlefield's units back to the nearest friendly city. This file holds the settings and their accessor, the posture and kind flavor rows, the mapped military flavors, the
	geographic blend, aggression from RISK, the loop's battlefields and offers, and the copies of the native
	functions the loop changes. Each copy names its native original. Include after CvTacticalAI.h.
	------------------------------------------------------------------------------------------------------- */
#pragma once

#ifndef CIV5_TACTICAL_OPERATIONAL_LOOP_H
#define CIV5_TACTICAL_OPERATIONAL_LOOP_H

#include "CvTacticalOperationalWeights.h"
// The purpose carried by each operational search and capture.
#include "VoxDeorumRL/schema/SearchIntent.h"

struct CvDangerPlotContents;
// Damage dealt and taken before interception.
struct SOperationalAttackDamage;

// one posture row per eTacticalPosture value
const int NUM_OPERATIONAL_POSTURES = TACTICAL_POSTURE_COUNTERATTACK + 1;

// The command kinds that carry a kind flavor row
enum eOperationalKind
{
	OPERATIONAL_KIND_UNIT_FIGHT,
	OPERATIONAL_KIND_CITY_FIGHT,
	OPERATIONAL_KIND_REINFORCE,
	OPERATIONAL_KIND_OWN_CITY_FIGHT,
	OPERATIONAL_KIND_WITHDRAW,
	// Formation movement in the command queue.
	OPERATIONAL_KIND_ARMY_MARCH,
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
	// lowest neutral-based aggression risk for Medium, High and Braveheart aggression
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

// Maps aggression risk to an aggression level by the settings' thresholds; below the Medium threshold is Low.
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

// One threat's damage, split between the selected defender and the tile's other occupants.
struct SOperationalThreatDamage
{
	PlayerTypes eOwner;
	int iID;
	bool bCity;
	int iDirectDamage;
	int iAreaDamage;

	// Returns this victim's damage from the threat after applying cover.
	int TotalDamage() const { return iDirectDamage + iAreaDamage; }
};

// One city's standard danger result and its per-threat contributions.
struct SOperationalCityDanger
{
	int iTotal;
	vector<SOperationalThreatDamage> threats;

	// Creates an empty city danger result.
	SOperationalCityDanger() : iTotal(0) {}
};

// One search attempt of an engagement: its plan, how many rows ran, and whether it ran to the end
struct SOperationalAttempt
{
	vector<STacticalAssignment> vRows;
	size_t nRowsRun;
	bool bCompleted;
};

// What a battlefield is built around
enum eOperationalCenter
{
	OPERATIONAL_CENTER_ENEMY_UNIT,
	OPERATIONAL_CENTER_ENEMY_CITY,
	OPERATIONAL_CENTER_OUR_CITY,
	// An enrolled army, keyed by owner, operation and army.
	OPERATIONAL_CENTER_ARMY,
	// A known camp, keyed by plot index.
	OPERATIONAL_CENTER_CAMP
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
	OPERATIONAL_OFFER_DROPPED,   // its target died or its city changed owner
	OPERATIONAL_OFFER_REFUSED    // an explicit empty admission consumed only this offer
};

// One enemy threat's share of our units' exposure, and the units of ours it can attack next turn
struct SOperationalThreatExposure
{
	double fExposure;
	vector<int> vUnitIDs;
	// Each victim's exposure allocation, parallel to vUnitIDs.
	vector<double> vAllocations;
	// The attacks per turn this threat spends on its victims.
	int iAttacks;

	// Creates a threat that exposes nothing.
	SOperationalThreatExposure() : fExposure(0.0), iAttacks(0) {}
};

typedef map<pair<int, int>, SOperationalThreatExposure> OperationalThreatMap;
// A city flag followed by the attacker's owner and ID.
typedef pair<bool, pair<int, int> > OperationalExposureKey;

// Allocations grouped by enemy unit or city.
struct SOperationalExposure
{
	OperationalThreatMap units, cities;
};

// The six scalar terms retained by stake records.
const int NUM_OPERATIONAL_STAKE_TERMS = 6;

// Weighted fight terms in typical units, their sum, and the damage, relief and fall shares behind them.
struct SOperationalStake
{
	double fDestruction;
	double fExposure;
	double fGround;
	double fRelief;
	double fCityProgress;  // city progress at an enemy city, city danger at ours
	double fGarrison;
	double fDamageShare;   // k_t at a unit target
	double fReliefShare;   // a_t at a unit target
	double fFallShare;     // f_att at an enemy city, f_def at ours or the unit target's zone city
	double fStake;
	// The multiplier applied to the weighted exposure term: s, or 1 with the split off.
	double fExposureShare;
	// Unweighted terms retained even when their weight is zero.
	double afValue[NUM_OPERATIONAL_STAKE_TERMS];

	// Creates an empty stake.
	SOperationalStake() : fDestruction(0.0), fExposure(0.0), fGround(0.0), fRelief(0.0), fCityProgress(0.0),
		fGarrison(0.0), fDamageShare(0.0), fReliefShare(0.0), fFallShare(0.0), fStake(0.0), fExposureShare(1.0)
	{
		for (int i = 0; i < NUM_OPERATIONAL_STAKE_TERMS; i++) afValue[i] = 0.0;
	}
};

// Our units that can take part at a battlefield's center this turn, by plain distance: combat units in the battlefield's
// domains within their strike reach, ranged units across domains, and aircraft whose range reaches the center from their base
struct SOperationalLocalSet
{
	bool bLand;                 // the battlefield takes land units
	bool bSea;                  // the battlefield takes sea units
	vector<CvUnit*> vStrength;  // strength filter: in-domain land and sea units, spent units and army members included
	vector<CvUnit*> vDamage;    // damage filter: recruitable units outside armies that can still attack, aircraft included, by ascending ID
	// Fighters that can sweep this target from their current plot, by ascending ID.
	vector<CvUnit*> vSweepers;
	bool bCapturer;             // a strength-filter unit passes IsCapturer
	bool bCanCapture;           // a damage-filter unit passes CanCapture

	// Creates an empty local set.
	SOperationalLocalSet() : bLand(false), bSea(false), bCapturer(false), bCanCapture(false) {}
};

// A deliberate approximation. Sample each target at most once per command loop and keep the sample
// as the board changes. Targets beyond the budget keep unsampled estimates; readiness checks still use live state.
struct SOperationalForecast
{
	bool bChecked;
	int iSpeculative;
	int iRejected;
	int iSpeculativeGarrison;
	int iRejectedGarrison;
	set<int> reachableCapturers;

	// Creates an unsampled forecast.
	SOperationalForecast() : bChecked(false), iSpeculative(0), iRejected(0), iSpeculativeGarrison(0),
		iRejectedGarrison(0) {}
};

typedef pair<int, pair<int, int> > OperationalForecastKey;
typedef map<OperationalForecastKey, SOperationalForecast> OperationalForecastMap;

// One friendly victim's exposure, separated into environmental residual and identified sources.
struct SOperationalVictimExposure
{
	double fResidual;
	map<OperationalExposureKey, double> shares;

	// Creates an empty exposure allocation.
	SOperationalVictimExposure() : fResidual(0.0) {}
};

// Whether an attacker can strike now or can reach a legal base when sampled.
enum eOperationalStrikeReadiness
{
	OPERATIONAL_STRIKE_IMMEDIATE,
	OPERATIONAL_STRIKE_REACHABLE,
	OPERATIONAL_STRIKE_UNCONFIRMED  // unreachable when sampled, or speculative when not path-checked
};

// A local damage simulation against one enemy city, carried from attack to attack as ComputeTotalExpectedDamage carries
// it, with its counted totals
struct SOperationalCityDamage
{
	const CvUnit* pGarrison;   // the garrison at the start, or NULL
	int iGarrisonHealth;       // G: its remaining HP at the start
	CvUnit* pCurrent;          // the garrison the next hit meets, after any predicted deaths
	int iCurrentHealth;        // its remaining HP in the simulation
	int iTotalGarrisonDamage;  // native's running garrison damage, reset when a garrison dies
	int iCityDamage;           // D_c: counted city damage after absorption
	int iGarrisonDamage;       // D_g: counted damage to the starting garrison
	int iRemaining;            // R: the city's remaining HP, at least 1
	int iHeal;                 // H: the city's expected heal
	vector<int> vKilled;       // garrisons predicted to die, which no later hit may meet again
	// Current speculative totals and cumulative rounded discounts for this estimate only.
	int iSpeculative, iSpeculativeGarrison, iRemoved, iRemovedGarrison;

	// Creates an empty simulation.
	SOperationalCityDamage() : pGarrison(NULL), iGarrisonHealth(0), pCurrent(NULL), iCurrentHealth(0), iTotalGarrisonDamage(0),
		iCityDamage(0), iGarrisonDamage(0), iRemaining(1), iHeal(0),
		iSpeculative(0), iSpeculativeGarrison(0), iRemoved(0), iRemovedGarrison(0) {}
};

// Realized lethal damage with a capturer earns full credit; other progress uses scored damage after healing.
double AttackFallShare(double fRealized, int iHeal, int iRemaining, bool bCapture, int iRejected = 0);

// The three offers every battlefield carries, in queue tie-break order.
enum eOperationalSlot
{
	OPERATIONAL_SLOT_FIGHT,
	OPERATIONAL_SLOT_REINFORCE,
	OPERATIONAL_SLOT_WITHDRAW,
	NUM_OPERATIONAL_SLOTS
};

struct SOperationalBattlefield;
// The offer identity of one battlefield's slot.
int OperationalOfferIdentity(int iBattlefield, eOperationalSlot eSlot);
// Decodes an offer identity's battlefield index.
int OperationalOfferBattlefield(int iOffer);
// Decodes an offer identity's slot.
eOperationalSlot OperationalOfferSlot(int iOffer);
// The command kind a slot runs at a center.
eOperationalKind OperationalSlotKind(eOperationalCenter eCenter, eOperationalSlot eSlot);
// Identifies the slots supported by a row, independently of their current state.
bool OperationalSupportsSlot(eOperationalCenter eCenter, eOperationalSlot eSlot);
// The slot a command kind belongs to.
eOperationalSlot OperationalKindSlot(eOperationalKind eKind);
// One battlefield offer's state by slot.
eOperationalOfferState& OperationalOfferState(SOperationalBattlefield& field, eOperationalSlot eSlot);
// One battlefield offer's state by slot, read-only.
const eOperationalOfferState& OperationalOfferState(const SOperationalBattlefield& field, eOperationalSlot eSlot);
// One battlefield offer's estimate by slot.
double OperationalOfferEstimate(const SOperationalBattlefield& field, eOperationalSlot eSlot);

// The queue rank breaks ties: own-city Fight, enemy-city Fight, unit or camp Fight, Reinforce, Withdraw, Army March.
int OfferRank(eOperationalCenter eCenter, eOperationalSlot eSlot);
// Whether an offer goes ahead of the best one so far: a higher estimate, then a lower rank, then a lower plot index.
bool OfferOutranks(double fEstimate, int iRank, int iPlotIndex, double fBest, int iBestRank, int iBestPlotIndex);

// A Reinforce candidate within recruit range, including units the same-field fight released.
struct SOperationalCandidate
{
	int iUnitID;
	double fStrength;    // full strength str(u)
	int iTravelTurns;    // tau: plain distance over moves, at least 1
	int iJoinTurns;      // j: tau - 1, at least 1
	bool bCapturer;      // a land melee combat unit outside armies that may capture cities, whether or not it can act now
	bool bHeldGarrison;  // joins the destination's search only; it takes no path move

	// Creates an empty candidate.
	SOperationalCandidate() : iUnitID(-1), fStrength(0.0), iTravelTurns(1), iJoinTurns(1), bCapturer(false), bHeldGarrison(false) {}
};

// A Reinforce verdict; a competing target also records the plot that blocked the march.
struct SOperationalReinforceReach
{
	enum Result { PASSED, INITIAL_REACH, MARCH, COMPETING_TARGET };
	Result eResult;
	int iCompetingPlot;
	// Stores the failed gate and, when applicable, the competing target.
	SOperationalReinforceReach(Result eResult_ = PASSED, int iCompetingPlot_ = -1)
		: eResult(eResult_), iCompetingPlot(iCompetingPlot_) {}
};

// One battlefield's Reinforce: strength tallies, the nearest-first selection, its score and the turn's rejections
struct SOperationalReinforce
{
	double fLocal;               // L
	double fEnemy;               // T
	double fNeed;                // H_need = max(0, rT - L)
	vector<int> vSelected;       // selected unit IDs, nearest first, any capture-role unit last
	double fSelectedStrength;    // full strength of the selection
	double fHelp;                // H: selected strength over join turns
	double fRoleHelp;            // H_role: the appended capturer's discounted strength, or 0
	int iRoleUnit;               // the capturer appended for the capture role, or -1
	bool bMissingRole;           // an enemy city needs a capturer and no local unit or candidate is one
	double fShare;               // s: our current strength share
	double fShareWithHelp;       // s+: our strength share with discounted help
	double fSwing;               // s+ - s
	set<pair<int, int> > countedUnits;   // the enemy units counted in T, by (owner, ID)
	set<pair<int, int> > countedCities;  // the enemy cities counted in T, by (owner, ID)
	map<int, SOperationalReinforceReach> rejected;
	bool bChecked;               // the selection passed its reach checks since the last command

	// Creates an empty Reinforce.
	SOperationalReinforce() : fLocal(0.0), fEnemy(0.0), fNeed(0.0), fSelectedStrength(0.0), fHelp(0.0), fRoleHelp(0.0), iRoleUnit(-1),
		bMissingRole(false), fShare(0.0), fShareWithHelp(0.0), fSwing(0.0), bChecked(false) {}
};

// One checked Withdraw mover, with its destination, first-turn stop and move flags.
struct SOperationalMarch
{
	int iUnitID;
	int iDestination;
	int iStop;
	int iMoveFlags; // Path flags for a checked formation Withdraw stop.

	// Orders Withdraw movers by recruit ID so admission uses the same tie break.
	bool operator<(const SOperationalMarch& other) const { return iUnitID < other.iUnitID; }
};

// The turns a unit needs for a plain distance at its moves per turn: max(1, ceil(distance / moves)).
int TravelTurns(int iDistance, int iMoves);
// Selects Reinforce candidates nearest first (join turns, travel turns, unit ID) until their full strength
// meets max(0, rT - L), storing the supplied L and T in out. When bNeedCapturer is set and no selected unit can capture, appends the
// nearest capturer as the role unit, or marks the role missing. Keeps strength shares, counted sets, rejections and check flag.
void SelectReinforcement(const vector<SOperationalCandidate>& candidates, double fLocal, double fEnemy, double fRatio, bool bNeedCapturer,
	SOperationalReinforce& out);

// Scalar theater components shared with unit targets in the same city zone.
struct SOperationalTheater
{
	double fImportance, fPressure, fContest, fDamageLevel;
	// A field without a city uses the theater floor.
	SOperationalTheater() : fImportance(0.0), fPressure(0.0), fContest(0.0), fDamageLevel(0.0) {}
};

// The ArmyMarch inputs that can change between estimation and execution.
struct SOperationalArmyMarchScore
{
	double fUnassistedTheater, fGoalWeight, fReadinessGain;

	// Starts with no progress and no priced exposure.
	SOperationalArmyMarchScore() : fUnassistedTheater(0.0), fGoalWeight(0.0), fReadinessGain(0.0) {}
};

// One battlefield of the turn, its fight offer, its Reinforce offer and its Withdraw offer
struct SOperationalBattlefield
{
	int iID;                      // index in the turn's list
	eOperationalCenter eCenter;
	int iPlotIndex;
	int iZoneID;
	int iZoneCityPlot;             // zone city plot for unit and camp rows, fixed for the turn, or -1
	PlayerTypes eOwner;           // the target's owner
	int iTargetID;                // unit, city or army ID; camp rows use the plot index
	// Army rows use the target ID for the army and also carry the operation ID.
	int iOperation;
	double fImportance;           // city centers only: city value / Z_C, capped at 1, fixed for the turn
	SOperationalTheater theater;
	double fTheater;              // refreshed from the zone city at each estimate pass
	double fGround;               // fixed for the turn; unit targets only
	double fCampValue;            // opening camp value, or -1 for a row without a camp
	bool bCampFogged;             // a camp row whose plot was in fog when built
	int iCampClaimant;            // claimant's unit ID, or -1 if none
	int iCampTurns;               // turns for the claimant to reach the camp
	bool bNearOurCity;            // fixed: within 3 plots of one of our cities
	// Enrolled formations with a reason to fight this target.
	set<int> fightArmies;
	bool bCanAttackOurUnits;      // rebuilt with the threat maps after each command
	eOperationalOfferState eFight;
	double fEstimate;             // theater times stake
	int iFailures;
	bool bReleaseLocal;           // the same-field fight searched or failed to recruit
	SOperationalStake stake;
	bool bHasEndangered;          // own city: it had an endangered recruit at the last estimate
	bool bBorder;                 // fixed: the zone is ours or neighbors one of ours
	bool bWithdrawPosture;        // fixed: the zone's posture is Withdraw
	eOperationalOfferState eReinforce;
	double fReinforceEstimate;    // fight priority times strength swing
	SOperationalReinforce reinforce;
	SOperationalArmyMarchScore armyMarch;
	// The Withdraw offer's state.
	eOperationalOfferState eWithdraw;
	// The Withdraw priority, 0 while it has no endangered recruit.
	double fWithdrawEstimate;
	// X, the removable exposure in worth units before dividing by Z_T.
	double fWithdrawExposure;
	// S, the strength share saved at the tally.
	double fExposureShare;
	// S+, the strength share the Withdraw estimate used.
	double fWithdrawShare;
	// Failed Withdraw readiness checks this turn.
	int iWithdrawFailures;
	// The Withdraw city plot per domain: index 0 land, 1 sea, -1 unset.
	int aiWithdrawDestination[2];

	// Creates a ready battlefield with no target.
	SOperationalBattlefield() : iID(-1), eCenter(OPERATIONAL_CENTER_ENEMY_UNIT), iPlotIndex(-1), iZoneID(0), iZoneCityPlot(-1), eOwner(NO_PLAYER),
		iTargetID(-1), iOperation(-1), fImportance(0.0), fTheater(0.0), fGround(0.0),
		fCampValue(-1.0), bCampFogged(false), iCampClaimant(-1), iCampTurns(0), bNearOurCity(false), bCanAttackOurUnits(false),
		eFight(OPERATIONAL_OFFER_READY), fEstimate(0.0), iFailures(0), bReleaseLocal(false),
		bHasEndangered(false), bBorder(false), bWithdrawPosture(false), eReinforce(OPERATIONAL_OFFER_READY), fReinforceEstimate(0.0),
		eWithdraw(OPERATIONAL_OFFER_INACTIVE), fWithdrawEstimate(0.0), fWithdrawExposure(0.0), fExposureShare(0.0),
		fWithdrawShare(0.0), iWithdrawFailures(0)
	{
		aiWithdrawDestination[0] = -1;
		aiWithdrawDestination[1] = -1;
	}

	// Discards the cached Reinforce reach check.
	void ResetReinforceCheck()
	{
		reinforce.bChecked = false;
	}
};

// City results shared during one estimate pass, indexed by city plot.
struct SOperationalEstimateCache
{
	map<int, double> cityTheaters;
	map<int, SOperationalTheater> theaters;
	// Victim shares expire with this estimate pass when the board changes.
	map<int, SOperationalVictimExposure> victimExposure;
};

// Exclusive stages and nested work counters for one complete command-loop call.
struct SOperationalPerformance
{
	unsigned long long totalNs;
	unsigned long long setupNs;
	unsigned long long estimateNs;
	unsigned long long selectNs;
	unsigned long long readinessNs;
	unsigned long long executeNs;
	unsigned long long retireNs;
	unsigned long long recordNs;
	unsigned long long otherNs;
	unsigned long long forceTallyNs;
	unsigned long long dangerReadNs;
	unsigned long long choiceNs;
	unsigned long long reinforceReachNs;
	unsigned long long admissionNs;
	unsigned long long flavorsNs;
	unsigned long long cityFireNs;
	unsigned long long spottingNs;
	unsigned long long greedyAirNs;
	unsigned long long marchingNs;
	// The queued army command's check and positioning path work.
	unsigned long long armyMarchCheckNs, armyMarchMoveNs;
	unsigned long long searchNs;
	unsigned long long estimates;
	unsigned long long forceTallies;
	unsigned long long dangerReads;
	unsigned long long checks;
	unsigned long long reinforceReachChecks;
	unsigned long long commands;
	unsigned long long engagements;
	unsigned long long attempts;

	// Starts all stage times and work counts at zero.
	SOperationalPerformance() : totalNs(0), setupNs(0), estimateNs(0), selectNs(0), readinessNs(0),
		executeNs(0), retireNs(0), recordNs(0), otherNs(0), forceTallyNs(0),
		dangerReadNs(0), choiceNs(0), reinforceReachNs(0), admissionNs(0), flavorsNs(0),
		cityFireNs(0), spottingNs(0), greedyAirNs(0), marchingNs(0), armyMarchCheckNs(0), armyMarchMoveNs(0), searchNs(0),
		estimates(0), forceTallies(0), dangerReads(0), checks(0), reinforceReachChecks(0),
		commands(0), engagements(0), attempts(0) {}
};

// Distinguishes normal positioning from an interrupted March.
enum eOperationalPositionResult
{
	OPERATIONAL_POSITION_COMPLETE,
	OPERATIONAL_POSITION_YIELDED
};


// One player-turn of the loop: settings, weights, the fixed scales, the threat maps and the battlefields
struct SOperationalTurn
{
	mutable SOperationalPerformance performance;
	bool bTiming;
	// Enables live discovery and combat army enrollment for this pass.
	bool bMarchEnabled;
	mutable bool bEstimating;
	// Sampled rejection shares keyed by target.
	mutable OperationalForecastMap forecasts;
	PlayerTypes ePlayer;
	OperationalSettings settings;
	OperationalWeights weights;
	int aiMapped[STacticalFlavors::NUM_SLOTS];
	double fTypicalUnitWorth;  // Z_T: mean base worth of our reference military types
	double fCampBounty;        // B: ordinary camp gold weighted and converted to typical units
	double fTypicalUnitStrength; // assumed strength of a fogged camp guard
	double fCityUnit;    // C = city.units, fixed for the turn
	double fCityScale;   // Z_C, fixed for the turn
	// Each attacker's share of our units' exposure in the Fight layout.
	OperationalThreatMap unitThreats, cityThreats;
	// Independent Withdraw uses its own layout when Fight includes formations.
	OperationalThreatMap withdrawUnitThreats, withdrawCityThreats;
	// Army IDs enrolled in the current board.
	set<int> preparedArmies;
	// Contact already observed this turn cannot yield another segment.
	mutable set<OperationalExposureKey> marchContacts;
	// The live board used for March and formation Withdraw projections.
	mutable SOperationalExposure sharedExposure;
	mutable bool bSharedExposureBuilt;
	// Standard city danger serves both Fight enrollment and estimates until the board is rebuilt.
	mutable map<int, SOperationalCityDanger> cityDanger;
	int iStrikeScan;     // the largest baseMoves(false) x 4 among our land and sea units: native's striking distance
	// each city of ours by ID, and the visible enemy combat units by (owner, ID) whose path to it, as the nearest of our
	// cities, is at most twice their moves plus their range: rebuilt after each command
	map<int, vector<pair<int, int> > > cityApproach;
	// our units outside friendly cities with a positive threat entry, by ID and whole danger: rebuilt after each command
	map<int, int> endangeredUnits;
	// The tactical roster unit IDs when the loop started.
	set<int> startRoster;
	// Units that went through a Withdraw this turn.
	set<int> withdrawnUnits;
	vector<SOperationalBattlefield> battlefields;
	// Mapped FLAVOR_GOLD for pillage missions.
	int iMarchGold;
	mutable unsigned int iNextDecision;
	mutable unsigned int iStateRevision;

	// Creates an empty turn.
	SOperationalTurn() : bTiming(true), bMarchEnabled(false), bEstimating(false), ePlayer(NO_PLAYER), fTypicalUnitWorth(0.0),
		fCampBounty(0.0), fTypicalUnitStrength(0.0), fCityUnit(0.0), fCityScale(0.0),
		bSharedExposureBuilt(false), iStrikeScan(0), iMarchGold(50), iNextDecision(0), iStateRevision(0) {}
};

// Identifies a synchronous callback and the state it reads. Hosts may supply branch identities.
struct SOperationalDecisionIdentity
{
	unsigned int iDecision;
	unsigned int iRevision;
	PlayerTypes ePlayer;
	int iCommand;
	int iStep;
	int iBattlefield;
	eOperationalKind eKind;
	int iTargetPlot;
	// Own operation and army for ArmyMarch, unset for other decisions.
	int iOperation, iArmy;
	// Creates an unbound decision.
	SOperationalDecisionIdentity() : iDecision(0), iRevision(0), ePlayer(NO_PLAYER), iCommand(-1), iStep(-1), iBattlefield(-1),
		eKind(OPERATIONAL_KIND_UNIT_FIGHT), iTargetPlot(-1), iOperation(-1), iArmy(-1) {}
};

// One supplied adapter response or recovery transition, linked to its shared callback. The DLL never fills these responses.
struct SOperationalProviderResponse
{
	SOperationalDecisionIdentity identity;
	unsigned int iPass, iAttempt, iReplyDecision, iReplyRevision;
	std::string stage, status, reason;
	bool bUseNative;
	int iOffer, iMappedOffer;
	// Requested rank order and optional probabilities keyed by entry position.
	vector<int> vOffers;
	vector<pair<int, double> > vProbabilities;
	int iEntry;
	vector<pair<int, int> > vUnits;
	int aiFlavors[STacticalFlavors::NUM_SLOTS];
	// Starts an empty host response record.
	SOperationalProviderResponse() : iPass(0), iAttempt(0), iReplyDecision(0), iReplyRevision(0), bUseNative(true), iOffer(-1), iMappedOffer(-1), iEntry(-1)
	{ for (int i = 0; i < STacticalFlavors::NUM_SLOTS; ++i) aiFlavors[i] = 0; }
};

// One selectable identity, with the native estimate and permitted command forces.
struct SOperationalSelectableOffer
{
	int iOffer; // the offer identity: OperationalOfferIdentity
	SOperationalBattlefield field;
	eOperationalSlot eSlot;
	// Creates an invalid offer identity.
	SOperationalSelectableOffer() : iOffer(-1), eSlot(OPERATIONAL_SLOT_FIGHT) {}
};

// Command choice over the current queue, in build order, with the exact native proposal.
struct SOperationalChoiceRequest
{
	SOperationalDecisionIdentity identity;
	vector<SOperationalSelectableOffer> offers;
	// Host-only mapping inputs, independent of native readiness.
	vector<SOperationalBattlefield> battlefields;
	vector<int> vAvailable;
	int aiMapped[STacticalFlavors::NUM_SLOTS];
	int iNative;
	// Creates an empty queue with neutral mapped flavors.
	SOperationalChoiceRequest() : iNative(-1)
	{
		for (int iSlot = 0; iSlot < STacticalFlavors::NUM_SLOTS; iSlot++)
			aiMapped[iSlot] = 50;
	}
};

// Distinguishes native admission, refusal, empty eligibility and invalid replies.
enum eOperationalAdmissionResult
{
	OPERATIONAL_ADMISSION_ABSENT,
	OPERATIONAL_ADMISSION_NATIVE,
	OPERATIONAL_ADMISSION_SUBSET,
	OPERATIONAL_ADMISSION_REFUSED,
	OPERATIONAL_ADMISSION_NO_ELIGIBLE,
	OPERATIONAL_ADMISSION_INVALID
};

// Admission before any participant acts; geometry describes the native proposal.
struct SOperationalAdmissionRequest
{
	SOperationalDecisionIdentity identity;
	vector<SOperationalCandidate> candidates;
	vector<int> vNative;
	map<int, int> preliminaryZones;
	SOperationalReinforce reinforce;
};

// Use native admission or an explicit subset, including an explicit refusal.
struct SOperationalAdmissionReply
{
	bool bUseNative;
	vector<int> vUnits;
	// Defaults to the native proposal.
	SOperationalAdmissionReply() : bUseNative(true) {}
};

// Retains decision inputs and outputs without repeating reach or strength queries for records.
struct SOperationalAdmissionDecision
{
	SOperationalAdmissionRequest request;
	SOperationalAdmissionReply requested;
	eOperationalAdmissionResult eResult;
	vector<int> vEffective;
	vector<int> vFailedReach;
	SOperationalReinforce effectiveReinforce;
	// Starts with no optional callback.
	SOperationalAdmissionDecision() : eResult(OPERATIONAL_ADMISSION_ABSENT) {}
};

// Final geometry and native search vector after spotting and greedy air, with separate aggression.
struct SOperationalFlavorRequest
{
	SOperationalDecisionIdentity identity;
	vector<int> vUnits;
	map<int, int> finalZones;
	int iRepresentativeZone;
	STacticalFlavors native;
	int iAggressionRisk;
	// No-search commands carry an empty geometry and neutral aggression.
	SOperationalFlavorRequest() : iRepresentativeZone(-1), iAggressionRisk(50) {}
};

// Keeps choice values separate so records can copy them without running the decoder again.
struct SOperationalChoiceDecision
{
	SOperationalChoiceRequest request;
	int iRequested;
	int iEffective;
	bool bInvalid;
	// Creates a decision with no selected offer.
	SOperationalChoiceDecision() : iRequested(-1), iEffective(-1), bInvalid(false) {}
};

// What one engagement did: its geometry, flavors, aggression and attempts
struct SOperationalEngagementReport
{
	map<int, int> zonePlots;
	int iRepresentativeZone;
	STacticalFlavors flavors;
	// the neutral-based risk that eAggLvl was resolved from
	int iAggressionRisk;
	eAggressionLevel eAggLvl;
	vector<SOperationalAttempt> attempts;
	bool bSuccess;
	unsigned long long searchNs;
	int iAttempts;
	SOperationalFlavorRequest flavorRequest;
	STacticalFlavors requestedFlavors;
	bool bInvalidFlavors;
	bool bTiming;

	// Creates an empty report.
	SOperationalEngagementReport() : iRepresentativeZone(-1), iAggressionRisk(50), eAggLvl(AL_LOW), bSuccess(false), searchNs(0), iAttempts(0), bInvalidFlavors(false), bTiming(true) {}
};

// The board facts a command can change that discovery and the wake rule read.
struct SOperationalBoard
{
	map<pair<int, int>, int> enemyUnitPlots;  // visible living enemy units by (owner, ID), and their plots
	map<int, int> visibleEnemyCityOwners;    // visible enemy city owners by plot, recorded for March
	map<int, int> cityOwners;                 // each city battlefield's plot and its owner, ours included
	map<int, int> ourUnitPlots;               // our units by ID, and their plots
	// Army membership is recorded when March is enabled to detect release.
	set<int> ourArmyUnits;
};

// Both ends of a friendly plot change; -1 means the unit was absent on that board.
struct SOperationalFriendlyMove
{
	int iUnitID;
	int iFrom;
	int iTo;
};

// What one command changed, from the board diff
struct SOperationalChanges
{
	vector<int> vEnemyPlots;        // plots where a visible enemy unit died, left, arrived or was revealed
	vector<int> vEnemyCityPlots;    // changed visible enemy cities, used for discovery only
	vector<int> vCapturedPlots;     // city battlefields whose owner changed
	vector<SOperationalFriendlyMove> vFriendlyMoves;
	// Surviving former army members, whether or not their plot changed.
	vector<int> vReleasedUnits;
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
	OPERATIONAL_CHECK_NO_NEARBY_ENEMY, // own city: no visible enemy within 5 plots and no endangered recruit
	OPERATIONAL_CHECK_NO_REACHABLE,    // Reinforce has no reachable recruit, or Withdraw has no march worth taking
	OPERATIONAL_CHECK_REDUCED         // Readiness lowered a viable offer's estimate
};

// One offer identity, written once when its battlefield enters the pass.
struct SOperationalOfferRecord
{
	int iBattlefield, iTargetID, iPlotIndex, iZoneID;
	eOperationalKind eKind;
	PlayerTypes eOwner;
	// ArmyMarch uses iTargetID as its army ID and carries the operation separately.
	int iOperation;
};

// Identifies offers that cannot return to the queue during this pass.
bool OperationalOfferIsClosed(eOperationalOfferState state);
// Names closure from retained state and kind, without reading the board.
const char* OperationalOfferClosedReason(eOperationalOfferState state, eOperationalKind kind);
// Maps the scalar stake terms to this offer's resolved pass weights.
int OperationalStakeWeight(int iTerm, eOperationalKind kind);

// One scalar queue row, with no per-unit lists or cached game objects.
struct SOperationalSnapshot
{
	// Score details apply only to an ArmyMarch offer.
	SOperationalArmyMarchScore armyMarch;
	int iBattlefield, iRank, iPlotIndex;
	eOperationalSlot eSlot;
	eOperationalOfferState eState;
	// Names why a closed offer left the queue.
	const char* szReason;
	double fEstimate, fTheater, fSwing, fSelectedSwing;
	// The battlefield's strength share and the Withdraw estimate's inputs.
	double fExposureShare, fWithdrawExposure, fWithdrawShare;
	// The Withdraw city plot per domain: index 0 land, 1 sea, -1 unset.
	int aiWithdrawDestination[2];
	SOperationalTheater theater;
	SOperationalStake stake;
};

// An ordered queue transition; checks and commands index the built record vectors.
struct SOperationalEvent
{
	const char* szType;
	int iBattlefield, iRecord;
	eOperationalSlot eSlot;
	eOperationalOfferState eBefore, eAfter;
	double fBefore, fAfter;
};

// One estimate pass through its terminating command or loop end.
struct SOperationalStep
{
	int iStep, iCommand;
	const char* szEnd;
	unsigned long long estimateNs, readinessNs, executeNs;
	vector<SOperationalSnapshot> snapshot;
	vector<SOperationalEvent> events;
	// Starts an open step with no terminating command.
	SOperationalStep() : iStep(0), iCommand(-1), szEnd(""), estimateNs(0), readinessNs(0), executeNs(0) {}
};

// One readiness check and its result
struct SOperationalCheckRecord
{
	int iBattlefield;
	int iStep;
	eOperationalOfferState eBefore, eAfter;
	double fBefore, fAfter;
	eOperationalCheckResult eResult;
	eOperationalSlot eSlot;  // the offer slot this check tested
};

// One executed command, for the records
struct SOperationalCommandRecord
{
	// The chosen ArmyMarch score, after checking its live order and usable members.
	SOperationalArmyMarchScore armyMarch;
	int iCommand;
	int iStep;
	int iBattlefield;
	int iPlotIndex;
	eOperationalKind eKind;
	PlayerTypes eOwner;
	// ArmyMarch mission identity; iPlotIndex is the waypoint that ran.
	int iOperation, iArmy;
	// Observed plot that interrupted this segment, or -1 when it did not yield.
	int iYieldPlot;
	// Retains the chosen estimate components for later scalar closure snapshots.
	double fTheater, fSwing;
	SOperationalStake stake;
	bool bCityFireAttempted;
	double fEstimate;           // the estimated priority the offer ran at, after any reach checks
	int iFailuresBefore;
	SOperationalAdmissionDecision admission;
	SOperationalEngagementReport engagement;
	bool bSearchRan;            // the search ran with at least one unit, so overlapping fights retire
	vector<int> vRetired;       // battlefields whose fight this command retired
	vector<pair<int, bool> > vEndangered;  // endangered recruits by unit ID, and whether the search placed them
	vector<int> vSelected;      // Reinforce: the checked selection; Withdraw: the admitted recruits, by unit ID
	bool bMoved;                // Reinforce or Withdraw: an admitted unit changed plot or died
	vector<int> vMoved;         // Admitted Reinforce or Withdraw units whose plot changed or who died
	vector<int> vRetiredReinforce;  // battlefields whose Reinforce this command retired
	vector<int> vRetiredWithdraw;  // battlefields whose Withdraw this command retired
	// The removable exposure and the strength share a Withdraw ran at, after its check; 0 for other commands.
	double fWithdrawExposure, fWithdrawShare;

	// Creates an empty record.
	SOperationalCommandRecord() : iCommand(-1), iStep(-1), iBattlefield(-1), iPlotIndex(-1), eKind(OPERATIONAL_KIND_UNIT_FIGHT),
		eOwner(NO_PLAYER), iOperation(-1), iArmy(-1), iYieldPlot(-1), fTheater(0.0), fSwing(1.0), bCityFireAttempted(false), fEstimate(0.0), iFailuresBefore(0), bSearchRan(false), bMoved(false),
		fWithdrawExposure(0.0), fWithdrawShare(0.0) {}
};

// One pass's queue trace, built commands and checks, turn inputs and complete performance counters.
struct SOperationalTurnRecord
{
	bool bRan;
	OperationalSettings settings, defaults;
	OperationalWeights weights;
	int aiMapped[STacticalFlavors::NUM_SLOTS];
	double fTypicalUnitWorth, fCityScale, fCityUnit;
	SOperationalPerformance performance;
	vector<SOperationalOfferRecord> offers;
	vector<SOperationalStep> steps;
	// The latest full snapshot is the baseline for replacement rows.
	vector<SOperationalSnapshot> latestSnapshot;
	// Estimate time already assigned to steps.
	unsigned long long recordedEstimateNs;
	vector<SOperationalCommandRecord> commands;
	vector<SOperationalChoiceDecision> choices;
	// Supplied replies and bounded recovery, without exposing the private record to providers.
	vector<SOperationalProviderResponse> responses;
	vector<SOperationalCheckRecord> checks;
	vector<int> vSkipped;
	bool bCommandCap;

	// Creates an empty record.
	SOperationalTurnRecord() : bRan(false), fTypicalUnitWorth(0), fCityScale(0), fCityUnit(0), recordedEstimateNs(0), bCommandCap(false) {}
};

// The loop's copies of native tactical functions, and the loop itself. CvTacticalAI declares it a friend.
class CvTacticalOperationalLoop
{
public:
	// Copy of CvTacticalAI::ProcessDominanceZones for a non-barbarian player: global high priority; the zone loop runs
	// emergency purchases only; the command loop; deferred garrisons; global mid and low priority; the review.
	static void ProcessDominanceZones(CvTacticalAI& kTactical);
	// Checks both switches for the player, using already resolved settings when available.
	static bool IsMarchEnabled(const CvPlayer& kPlayer, const OperationalSettings* pSettings = NULL);
	// Prepares a combat operation at the native army pass and records its waypoint.
	static bool PrepareArmy(CvTacticalAI& kTactical, CvAIOperation& kOperation, SOperationalPreparedArmy& prepared);
	// Releases a terminal operation's army, reports the result, and removes its prepared entry.
	static vector<SOperationalPreparedArmy>::iterator RemovePreparedOperation(CvTacticalAI& kTactical,
		vector<SOperationalPreparedArmy>::iterator order, CvAIOperation& kOperation, CvArmyAI* pArmy);
	// Cleans up missing or already aborted operations and updates waypoints for changed orders.
	static void RefreshPreparedArmies(CvTacticalAI& kTactical);
	// Runs safety, finishes pending operations, and clears the prepared list on loop exit.
	static void FinishPreparedArmies(CvTacticalAI& kTactical);
	// Runs the existing safety moves for unused army members.
	static void MoveArmyUnitsToSafety(const vector<CvUnit*>& vUnits);
	// Monotonic host clock in nanoseconds; its value never participates in decisions.
	static unsigned long long ClockNs();
	// Reads whether the host has stopped without evaluating game rules again.
	static bool IsHostStopped();
	// Copies the queue after an estimate pass, assigning stable ready ranks.
	static void RecordStep(const SOperationalTurn& turn, SOperationalTurnRecord& record);
	// Closes the last trace step before working-turn teardown, including exception unwinding.
	static void CloseRecord(const SOperationalTurn& turn, SOperationalTurnRecord* pRecord);
	// Runs one pass, selecting, checking and executing fights, Reinforce and Withdraw offers, and optionally
	// retains its trace, settings and complete counters.
	static void RunCommandLoop(CvTacticalAI& kTactical, SOperationalTurnRecord* pRecord = NULL, SOperationalPerformance* pPerformance = NULL, bool bTotalOnly = false);

	// Refreshes the board and retires covered offers after a command commits.
	static void RefreshAfterCommand(CvTacticalAI& kTactical, SOperationalTurn& turn, SOperationalBattlefield& field,
		SOperationalCommandRecord& command, bool bArmyMarchOrderChanged, const SOperationalBoard& before, SOperationalChanges& changes);

	// Resolves the turn's weights, typical unit worth Z_T, best city value Z_C, city allowance C and striking-distance scan.
	static void BuildTurn(CvTacticalAI& kTactical, SOperationalTurn& turn);
	// Rebuilds each enemy unit's and city's share of our units' exposure, our endangered units, each city's approach
	// list and each battlefield's bCanAttackOurUnits.
	static void BuildThreats(CvTacticalAI& kTactical, SOperationalTurn& turn);
	// Allocates threats against a plot's occupants. A shared city can also return Fight's unadjusted shares.
	static void AllocatePlotExposure(PlayerTypes ePlayer, const CvPlot& kPlot, vector<SPlacedUnit>& occupants, int iRisk,
		OperationalThreatMap& units, OperationalThreatMap& cities, map<int, int>* pEndangered, bool bAllVictims = false,
		const set<int>* pEnrolled = NULL, SOperationalExposure* pCityFight = NULL);
	// A threat's largest allocations up to its attacks per turn, leaving out the victims in pExclude when set.
	static double CappedExposure(const SOperationalThreatExposure& threat, const set<int>* pExclude);
	// Builds the opening enemy, camp and own-city rows.
	static void BuildBattlefields(CvTacticalAI& kTactical, SOperationalTurn& turn);
	// Returns the bounty scaled by the distance to our nearest city.
	static double CampValue(const SOperationalTurn& turn, const CvPlot& kPlot);
	// Returns the best camp recruit that can leave its post, with its travel turns.
	static CvUnit* FindCampClaimant(CvTacticalAI& kTactical, CvPlot* pPlot, int& iTurns);
	// Adds an opening camp row when there is no visible guard.
	static void AddCampBattlefield(CvTacticalAI& kTactical, SOperationalTurn& turn, const CvTacticalTarget& target, CvPlot& kPlot);
	// Appends enrolled rows, retaining their indices when an operation retargets or drops.
	static void SyncArmyBattlefields(CvTacticalAI& kTactical, SOperationalTurn& turn);
	// Finds the enrolled order for an army row.
	static vector<SOperationalPreparedArmy>::iterator FindPreparedArmy(CvTacticalAI& kTactical, const SOperationalBattlefield& field);
	// Resolves a live March order and optionally gathers its members in formation order.
	static eOperationalCheckResult ResolveArmyMarch(CvTacticalAI& kTactical, const SOperationalBattlefield& field,
		vector<SOperationalPreparedArmy>::iterator& order, CvAIOperation*& pOperation, CvArmyAI*& pArmy,
		vector<CvUnit*>* pMembers = NULL);
	// Estimates a formation's progress from its ready members and the shared corridor, and its projected theater.
	static void EstimateArmyMarch(CvTacticalAI& kTactical, SOperationalTurn& turn, SOperationalBattlefield& field);
	// Revalidates the live order behind a March offer.
	static eOperationalCheckResult CheckArmyMarch(CvTacticalAI& kTactical, SOperationalTurn& turn, SOperationalBattlefield& field);
	// Appends a classified enemy target and initializes its battlefield.
	static bool AddEnemyBattlefield(CvTacticalAI& kTactical, SOperationalTurn& turn, const CvTacticalTarget& target,
		const CvPlot& kPlot, const CvUnit* pDefender, set<pair<int, int> >& seenUnits, set<pair<int, int> >& seenCities);
	// Appends unseen battlefield identities in plot order from the current observations.
	static void DiscoverEnemyBattlefields(CvTacticalAI& kTactical, SOperationalTurn& turn, const SOperationalBoard& board,
		SOperationalChanges* pChanges);
	// Sets a battlefield's threat flag from the current allocations.
	static void UpdateBattlefieldThreat(const SOperationalTurn& turn, SOperationalBattlefield& field);
	// Computes the Fight eligibility shared by estimates, checks and execution.
	static void UpdateFightArmies(CvTacticalAI& kTactical, const SOperationalTurn& turn, SOperationalBattlefield& field);
	// True when the zone is ours or neighbors one of ours, read from our own analysis map.
	static bool IsBorderZone(CvTacticalAI& kTactical, const CvTacticalDominanceZone& kZone);
	// Appends our city's battlefield: its land zone, or its water zone without one, and its theater.
	static void AddOwnCityBattlefield(CvTacticalAI& kTactical, SOperationalTurn& turn, CvCity& kCity);
	// Estimates cities before units and camps with one fresh cache, then updates open offers.
	static void EstimateOffers(CvTacticalAI& kTactical, SOperationalTurn& turn, const SOperationalChanges* pChanges);
	// Gathers one local set, computes theater, and can wake a waiting fight before estimating Reinforce and
	// the Withdraw. Returns false when the target died or its city changed owner. Closed cities need only their theater.
	static bool EstimateBattlefield(CvTacticalAI& kTactical, const SOperationalTurn& turn, SOperationalEstimateCache& cache,
		SOperationalBattlefield& field, bool bStake, bool bReinforce, bool bWithdraw, const SOperationalChanges* pChanges = NULL);
	// Tallies L, T and the strength share for every battlefield, then computes the city's theater or inherits
	// it. The share is saved before Reinforce bookkeeping can change it.
	static void EstimateTheater(const SOperationalTurn& turn, const SOperationalEstimateCache& cache, const CvPlot& kPlot,
		const SOperationalLocalSet& local, SOperationalBattlefield& field);
	// The current unit, city or camp plot. Returns NULL when the target unit or city is gone.
	static CvPlot* BattlefieldPlot(SOperationalBattlefield& field);
	// Prices the claim at a camp seen empty and returns false if the camp is gone or guarded.
	static bool EstimateCampClaim(CvTacticalAI& kTactical, CvPlot& kPlot, SOperationalBattlefield& field, bool bCheck = false);
	// Values local destruction, exposure, ground and city relief at a unit, city progress and garrison destruction at
	// an enemy city, or city danger and endangered recruits at ours. Uses the battlefield's shared theater.
	static bool EstimateFight(CvTacticalAI& kTactical, const SOperationalTurn& turn, SOperationalEstimateCache& cache, const SOperationalLocalSet& local,
		const CvCity* pZoneCity, SOperationalBattlefield& field, bool bSample = false);
	// Selects incoming help and prices the rise in strength share against the fight or camp stake.
	// Enemy centers require full strength covering need and a posture other than Withdraw. Camps waive the border gate.
	static void EstimateReinforce(CvTacticalAI& kTactical, const SOperationalTurn& turn, const SOperationalLocalSet& local,
		SOperationalBattlefield& field);
	// Local combat units and enrolled formations threatened here that still have movement.
	// Sorted by ascending ID. Our own cities have no recruits.
	static void GatherWithdrawRecruits(CvTacticalAI& kTactical, const SOperationalTurn& turn, const SOperationalBattlefield& field,
		const SOperationalLocalSet& local, vector<CvUnit*>& vRecruits);
	// The change in capped exposure; the opening estimate omits the recruits without projecting stops.
	// A formation Withdraw compares the shared layout when it projects its legal stops.
	static double ExposureSaved(const SOperationalTurn& turn, const vector<SOperationalMarch>& marches, bool bProjectStops,
		bool bFormationWithdraw = false);
	// The Withdraw priority from the removable exposure, the risk weight and the share help does not cover.
	static void EstimateWithdraw(CvTacticalAI& kTactical, const SOperationalTurn& turn, const SOperationalLocalSet& local,
		SOperationalBattlefield& field);
	// The Withdraw priority from an exposure list, the theater and the share help does not cover. The check
	// reuses it when it drops recruits that cannot leave.
	static double WithdrawPriority(const SOperationalTurn& turn, const SOperationalBattlefield& field, double fExposure,
		double fShare);
	// Gathers available units outside armies in the destination's domains and recruit range.
	// Local units qualify once the same-field fight releases them. Uses native's carrier, healing, siege, donor-zone
	// and civilian-cover filters and excludes the battlefield's rejections. Enemy-city candidates must also pass
	// CanStrikeCity. A held garrison qualifies within the search's reach.
	static void GatherReinforceCandidates(CvTacticalAI& kTactical, const SOperationalBattlefield& field, const CvPlot& kTarget,
		bool bLand, bool bSea, vector<SOperationalCandidate>& vCandidates, bool bAllowLocal = false);
	// A unit's strike reach in plots: its base moves, at least 1, plus its range when it attacks at range.
	static int StrikeReach(const CvUnit& kUnit);
	// Gathers our local set around a center in the given domains; bDamage also gathers the damage filter.
	static void GatherLocalSet(CvTacticalAI& kTactical, PlayerTypes ePlayer, const CvPlot& kPlot, bool bLand, bool bSea,
		SOperationalLocalSet& local, bool bDamage = true, const SOperationalBattlefield* pFight = NULL);
	// Gathers a local set in the target plot's domains, using terrain alone for unit target estimates.
	static void GatherTargetLocalSet(CvTacticalAI& kTactical, PlayerTypes ePlayer, const CvPlot& kTarget,
		SOperationalLocalSet& local, bool bDamage, bool bUnitTarget = false, const SOperationalBattlefield* pFight = NULL);
	// Whether native recruitment would take this unit against the city: no capture prohibition, and for a ranged unit an
	// attack from its plot that meets native's minimum expected damage.
	static bool CanStrikeCity(CvTacticalAI& kTactical, CvUnit& kUnit, const CvCity& kCity);
	// Copy of the city heal in CvCity::doTurn, as the heal the city would get on its owner's next turn if our damage lands.
	static int ExpectedCityHeal(const CvCity& kCity);
	// Starts a damage simulation against the city: its garrison and health, remaining HP and expected heal.
	static void StartCityDamage(const CvCity& kCity, SOperationalCityDamage& damage);
	// Tests a projected city hit with its planned interceptor and applies the readiness discount.
	// Counts city and starting garrison damage and replaces a garrison predicted to die.
	static void SimulateCityAttack(const CvCity& kCity, const CvUnit& kAttacker, const CvUnit* pInterceptor,
		SOperationalAttackDamage hit, SOperationalCityDamage& damage, int iRisk, SOperationalForecast* pForecast, bool bSample,
		eOperationalStrikeReadiness eReadiness);
	// Estimates realizable damage from city fire and local units; reports rejected damage separately for scoring.
	static int LocalUnitDamage(PlayerTypes ePlayer, const CvUnit& kTarget, const SOperationalLocalSet& local, int iRisk, int& iRejected,
		SOperationalForecast* pForecast = NULL, bool bSample = false);
	// Keeps admitted air attacks and legal immediate strikes at their full estimate.
	static bool StrikeNow(const CvUnit& kUnit, const CvPlot& kTarget);
	// Checks native ranged target legality independently of the current firing origin.
	static bool CanRangeStrikeTarget(const CvUnit& kUnit, const CvPlot& kTarget);
	// Checks each attacker for a legal firing or melee base reached with movement left.
	static bool StrikeReachable(const CvUnit& kUnit, const CvPlot& kTarget);
	// Reapplies a sampled rejection fraction to current speculative damage, rounded once.
	static int RejectedDamage(int iCurrent, int iRejected, int iSampled);
	// Samples a first native-selected target within the shared per-loop budget and reranks a reduced offer.
	static eOperationalCheckResult CheckForecastReadiness(CvTacticalAI& kTactical, const SOperationalTurn& turn,
		SOperationalBattlefield& field);
	// Divides exposure over identified threats while preserving the victim's residual danger.
	static SOperationalVictimExposure AttributeExposure(double fExposure, int iDanger, const vector<SOperationalThreatDamage>& threats);
	// Caps each source's attacks, attributes retained shares to victims, then applies geometric decay.
	static double CityDefenseExposure(const SOperationalTurn& turn, SOperationalEstimateCache& cache,
		const vector<CvUnit*>& vEndangered, const map<int, int>& dangers);
	// Enemy unit damage valued without a ready-upgrade premium.
	static double DestroyedUnitValue(const CvUnit& kUnit, double fRealized, int iRejected);
	// Uncached unit danger at the enemy roll, including city-fall danger to a garrison.
	static int UnitDanger(const SOperationalTurn& turn, const CvUnit& kUnit);
	// Our unit's strength: native maximum attack strength, or ranged strength for a ranged unit, times its HP share.
	static double UnitStrength(const CvUnit& kUnit);
	// An enemy unit's strength against our city: its quick attack or ranged strength against the city plot, times its HP
	// share.
	static double UnitStrengthAgainstCity(const CvUnit& kUnit, const CvCity& kCity);
	// A city's strength: its strength value times its HP share.
	static double CityStrength(const CvCity& kCity);
	// Our local strength L: the strength filter of the local set. Spent and army units count; aircraft do not.
	static double LocalStrength(const SOperationalLocalSet& local);
	// Our local strength L around our city: the city's strength plus LocalStrength.
	static double LocalCityStrength(const CvCity& kCity, const SOperationalLocalSet& local);
	// The enemy strength T against our city: its approach list together with the visible units native danger says can
	// attack it next turn, each once. pCounted, when given, receives the counted units.
	static double EnemyCityStrength(const SOperationalTurn& turn, const CvCity& kCity, set<pair<int, int> >* pCounted = NULL);
	// Enemy strength T includes visible hostile non-air combat units within 4 plots in the given domains,
	// visible hostile cities within 4 plots at no less than 1 HP, and a revealed center city even in fog.
	// A counted city's garrison is part of its strength; units in fog stay excluded. The extra player counts as hostile.
	static double EnemyCenterStrength(PlayerTypes ePlayer, const CvPlot& kTarget, bool bLand, bool bSea,
		set<pair<int, int> >& countedUnits, set<pair<int, int> >& countedCities, PlayerTypes eExtraHostile = NO_PLAYER);
	// True for a unit that can capture a city on a later turn: a land melee combat unit outside armies that may enter
	// cities, whether or not it has moves or attacks left.
	static bool IsCapturer(const CvUnit& kUnit);
	// Resolves a usable city recruit on the map, allowing members of a Fight's eligible armies when supplied.
	static CvUnit* CityRecruit(CvTacticalAI& kTactical, int iUnitID, const SOperationalBattlefield* pFight = NULL);
	// Recruits for the live strike using the same army eligibility in readiness and execution.
	static bool FindFightRecruits(CvTacticalAI& kTactical, const SOperationalTurn& turn,
		const SOperationalBattlefield& field, CvPlot* pPlot, vector<int>& armyRecruits);
	// Our city fight's endangered recruits from the tactical roster: non-air units within 4 plots on the turn's
	// endangered list or in a friendly city in danger. Returns their computed dangers when requested.
	static void GatherCityEndangered(CvTacticalAI& kTactical, const SOperationalTurn& turn, const CvCity& kCity,
		vector<CvUnit*>& vEndangered, const SOperationalBattlefield& field, map<int, int>* pDangers = NULL);
	// Our city fight's defenders from the tactical roster, leaving out the given endangered recruits: local units within
	// strike reach, the held garrison, and aircraft in reach with their carriers under the air search. Resets the air caches
	// and paths carrier reach, so only readiness and execution call it.
	static void GatherCityDefenders(CvTacticalAI& kTactical, const CvCity& kCity, const vector<CvUnit*>& vEndangered,
		vector<CvUnit*>& vDefenders, const SOperationalTurn& turn, const SOperationalBattlefield& field);
	// Fills a native target with the battlefield's type, plot and zone, for native recruitment and damage estimates.
	static CvTacticalTarget FightTarget(const SOperationalBattlefield& field, const CvPlot& kPlot);
	// The fight's readiness check: native city strike and striking-distance recruitment with the damage test, or for our
	// city its defenders, endangered recruits and nearby enemies, with no orders issued. Returns a pass or the first
	// condition that failed.
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
	// The Reinforce's readiness check, with no orders issued: one step-finder flood from the destination, then for each
	// selected unit that is not a held garrison the flood, native's alternative-target filter and a march path with a
	// safe enough first stop. Failures join the turn's rejections and the selection is made again until every selected
	// unit passed. A selection checked since the last command passes at once.
	static eOperationalCheckResult CheckReinforce(CvTacticalAI& kTactical, const SOperationalTurn& turn, SOperationalBattlefield& field);
	// Finds destinations by domain, then scores the available first-turn stops together.
	static eOperationalCheckResult CheckWithdraw(CvTacticalAI& kTactical, const SOperationalTurn& turn, SOperationalBattlefield& field,
		vector<SOperationalMarch>& marches, bool& bFormationWithdraw);
	// Returns the first march stop. A formation Withdraw on an army route leaves danger to the formation check, and
	// camp claims follow the exact route and defer the danger check to the move.
	static CvPlot* CanMarch(CvTacticalAI& kTactical, CvUnit* pUnit, CvPlot* pTarget, bool bWithdraw = false,
		bool bArmyRoute = false, int iClaimTurns = 0);
	// The positioning copy's path flags for a unit marching toward the target.
	static int MarchFlags(const CvUnit& kUnit, const CvPlot& kTarget, bool bWithdraw = false, bool bArmyRoute = false);
	// Plots within the search's near distance of the target for one domain, measured as the search measures it.
	static ReachablePlots SearchReachPlots(PlayerTypes ePlayer, CvPlot* pTarget, DomainTypes eDomain);
	// Picks a reachable friendly city for one domain, preferring shelter on routes of similar length.
	static int WithdrawDestination(CvTacticalAI& kTactical, PlayerTypes ePlayer, const CvPlot& kStart, DomainTypes eDomain);
	// Validates the selected army's order, then admits and positions units by destination.
	static void ExecuteMarch(CvTacticalAI& kTactical, const SOperationalTurn& turn, const SOperationalBattlefield& field,
		const vector<SOperationalMarch>& withdraw, SOperationalCommandRecord& record, bool bFormationWithdraw, bool& bArmyMarchOrderChanged);
	// Admits the checked claimant and attempts one move toward the camp.
	static void ExecuteCampClaim(CvTacticalAI& kTactical, const SOperationalTurn& turn, const SOperationalBattlefield& field,
		SOperationalCommandRecord& record);

	// The fight row of the overlap rule: an enemy-unit fight that ran its search retires an unexecuted enemy-unit fight
	// within 2 plots of its target in a compatible zone, unless that fight's target can attack our units or stands near
	// our city. City fights, ours included, neither retire nor get retired.
	static bool RetiresFight(const SOperationalBattlefield& executed, const SOperationalBattlefield& candidate);
	// The Reinforce row of the overlap rule: a Reinforce that moved a unit retires the open Reinforce of every other enemy
	// battlefield whose target it counted in T. Our city's Reinforce is never retired. The caller checks the move.
	static bool RetiresReinforce(const SOperationalBattlefield& executed, const SOperationalBattlefield& candidate);
	// Records the board facts the wake rule compares before and after a command.
	static void SnapshotBoard(const SOperationalTurn& turn, SOperationalBoard& board);
	// Compares two snapshots into the changes a command made.
	static void DiffBoard(const SOperationalBoard& before, const SOperationalBoard& after, SOperationalChanges& changes);
	// Drops temporary march and competing-target failures affected by the last command.
	static void RetryReinforce(const CvPlot& kTarget, int iRange, const SOperationalChanges& changes, SOperationalBattlefield& field);
	// True when a change can turn the waiting fight's failed check into a pass: an enemy change or capture within
	// striking distance of its target, or a unit of ours that moved within its own striking distance and is available.
	// An own-city fight wakes on an enemy change within 5 plots, an available unit of ours now within its strike reach, or
	// an endangered recruit.
	static bool WakesFight(PlayerTypes ePlayer, const SOperationalTurn& turn, const SOperationalBattlefield& field,
		const SOperationalChanges& changes);

	// Our unit's worth as the grade counts it: base worth with the level premium, plus a ready upgrade's premium.
	static double OurUnitWorth(const CvUnit& kUnit);
	// The ground side bit of another player as the grade's sides read it: ours or friendly, hostile, neutral, or 0 for no
	// player.
	static unsigned int GroundSide(PlayerTypes eUs, PlayerTypes eOther);
	// The side bits standing on a plot and blockading from it, as the grade marks them, leaving out pSkip.
	static void PlotPresence(const CvPlot& kPlot, PlayerTypes eUs, const CvUnit* pSkip, unsigned int& iStand,
		unsigned int& iBlock);
	// The ground score of the owned plots among a target plot and its neighbors on its landmass, leaving out pSkip, in plot weight.
	static double GroundAround(PlayerTypes eUs, const CvPlot& kTarget, const CvUnit* pSkip);
	// A unit target's ground term: the ground score with the target less the score without it, in plot weight.
	static double TargetGround(PlayerTypes eUs, const CvPlot& kTarget, const CvUnit* pTarget);
	// The player's danger contents for one plot, after rebuilding a dirty cache as CvPlayer::GetPlotDanger does.
	static CvDangerPlotContents& DangerContents(PlayerTypes ePlayer, const CvPlot& kPlot);
	// True for a unit that can capture a city: a melee combat unit with moves and an attack left that may enter cities.
	static bool CanCapture(const CvUnit& kUnit);

	// Constructs and stamps a callback identity on the current synchronous state.
	static SOperationalDecisionIdentity DecisionIdentity(const SOperationalTurn& turn, const SOperationalCommandRecord* pCommand);
	// Exactly reproduces the original OfferOutranks scan, including stable build-order ties.
	static int NativeCommandChoice(const SOperationalChoiceRequest& request);
	// Applies optional admission once, validating membership and extra Reinforce reach before any action.
	static bool AdmitParticipants(CvTacticalAI& kTactical, const SOperationalTurn& turn, const SOperationalBattlefield& field,
		vector<CvUnit*>& vUnits, SOperationalCommandRecord& record);
	// The common Reinforce reach test for checked proposals and provider additions.
	static SOperationalReinforceReach ReinforceReach(CvTacticalAI& kTactical, CvUnit* pUnit, CvPlot* pTarget,
		const ReachablePlots& relevantPlots, map<int, ReachablePlots>& searchPlots);
	// Recomputes admitted strength, help, swing and capture role without changing command priority.
	static void AdmittedReinforcement(const vector<SOperationalCandidate>& candidates, bool bNeedCapturer,
		const vector<int>& vAdmitted, double fSteepness, SOperationalReinforce& out);
	// Restricts native air or landing recruitment to the already admitted group.
	static void FilterRecruits(CvTacticalAI& kTactical, const vector<CvUnit*>& vUnits);

	// Host seams. The DLL defines them in this file; the simulator defines its own and does not extract these.
	// Host identity, choice, optional admission and final flavor seams; DLL replies are always native.
	static void StampDecision(SOperationalDecisionIdentity& identity);
	// Reports whether the host needs operational observations.
	static bool HasOperationalProvider();
	// Enables a supplied menu even when native has no ready offer.
	static bool UsesPermittedMenu();
	// Reports confirmed checks or completed commands to the host.
	static void ReportExecution(int iOffer, int iResult, const SOperationalCommandRecord* pCommand);
	// Lets the host rebuild candidates from permitted own-unit inputs.
	static void PrepareAdmission(SOperationalAdmissionRequest& request);
	// Reports admission capability before building candidates or geometry.
	static bool HasAdmission();
	// Returns a current offer identity.
	static int ChooseCommand(const SOperationalChoiceRequest& request);
	// Returns false when the provider has no admission capability.
	static bool AdmitCommand(const SOperationalAdmissionRequest& request, SOperationalAdmissionReply& reply);
	// Returns the final search vector, already resolved by native when unchanged.
	static STacticalFlavors ChooseSearchFlavors(const SOperationalFlavorRequest& request);
	// A plot's ground weight with the resources the player's team has revealed.
	static double GroundPlotWeight(PlayerTypes ePlayer, const CvPlot& kPlot);
	// Opens the host work entry with the command's identity and battlefield zone.
	static void BeginCommand(CvTacticalAI& kTactical, const SOperationalCommandRecord& command, int iZoneID);
	// Closes the host work entry, retaining revisions even when execution stops the pass.
	static void EndCommand(CvTacticalAI& kTactical);

	// Copy of TacticalAIHelpers::FindAndExecuteBestUnitAssignments for one engagement: gathers the geometry first,
	// blends the flavors over it with the kind row, derives aggression from RISK, then searches and executes.
	// bTargetDistanceRelevant is native's search flag: false lets units end farther from the target than they started.
	static bool ExecuteEngagement(PlayerTypes ePlayer, vector<CvUnit*>& vUnits, CvPlot* pTarget, eOperationalKind eKind,
		PlayerTypes eCounterpart, const int (&aiMapped)[STacticalFlavors::NUM_SLOTS], const OperationalSettings& settings,
		bool bTargetDistanceRelevant, SOperationalEngagementReport& report, const SOperationalTurn* pTurn = NULL,
		SOperationalCommandRecord* pCommand = NULL, STacticalMovementGate* pMovement = NULL);
	// The retry loop of TacticalAIHelpers::FindAndExecuteBestUnitAssignments with given flavors, aggression and target
	// distance flag: native FindBestUnitAssignments unchanged, up to four attempts, each recorded with the rows that ran.
	static bool SearchAndExecute(PlayerTypes ePlayer, vector<CvUnit*>& vUnits, CvPlot* pTarget, eAggressionLevel eAggLvl,
		SearchIntent eSearchIntent, const STacticalFlavors& flavors, bool bTargetDistanceRelevant, SOperationalEngagementReport& report,
		STacticalMovementGate* pMovement = NULL);
	// Copy of the plot gathering in TacticalAIHelpers::FindBestUnitAssignments: the search's plots for all units before
	// the participant cap, without remote air bases, counted by zone. Zone 0, the unknown zone, is left out.
	static void GatherSearchPlotZones(const vector<CvUnit*>& vUnits, CvPlot* pTarget, bool bTargetDistanceRelevant,
		map<int, int>& zonePlots, bool bUseEmbarked = false);
	// Blends the mapped search flavors and the neutral-based aggression risk over the same zones, with the civ,
	// operation and zone modifiers, posture rows and the kind row. Search RISK always counts as set.
	static STacticalFlavors BlendEngagementFlavors(PlayerTypes ePlayer, const map<int, int>& zonePlots, eOperationalKind eKind,
		PlayerTypes eCounterpart, int iOperationID, const int (&aiMapped)[STacticalFlavors::NUM_SLOTS],
		const OperationalSettings& settings, int& iRepresentativeZone, int& iAggressionRisk);

	// Creates the gate used by Army March or formation Withdraw before positioning the group.
	static eOperationalPositionResult PositionUnitsAroundTarget(CvTacticalAI& kTactical, const vector<CvUnit*>& vUnits, CvPlot* pTarget,
		PlayerTypes eCounterpart, SOperationalEngagementReport& report, const SOperationalTurn& turn, SOperationalCommandRecord& command,
		const vector<SOperationalMarch>* pWithdrawStops, const vector<CvPlot*>* pMarchCorridor);
	// Copy of CvTacticalAI::PositionUnitsAroundTarget with the loop's engagement in place of AL_LOW and the search intent:
	// the geometry, blend and aggression follow the spotter move. The spotter and path moves skip held garrisons, and
	// Army March's gate checks the selected route or the prepared corridor for contact. Other far movers go through
	// CanMarch. A Withdraw skips the spotter move, sends every recruit that can still move and orders damaged units first.
	static eOperationalPositionResult PositionUnitsAroundTargetWithGate(CvTacticalAI& kTactical, const vector<CvUnit*>& vUnits,
		const vector<int>& vUnitIDs, CvPlot* pTarget, PlayerTypes eCounterpart, SOperationalEngagementReport& report, const SOperationalTurn& turn,
		SOperationalCommandRecord& command, STacticalMovementGate* pGate, const vector<SOperationalMarch>* pWithdrawStops);
	// Copy of the unit overload of CvDangerPlotContents::GetDanger, with enemy-roll damage and per-threat contributions.
	// A falling city destroys its garrison at f_def's threshold. Capturable civilians and units in cities list no threats.
	static int GetDangerByThreat(CvDangerPlotContents& kContents, const CvUnit* pUnit, const SUnitIDValueContainer& extraUnitDamage,
		int iExtraDamage, AirActionType eAirAction, vector<SOperationalThreatDamage>& threats, int iRisk);

	// Copy of the city overload of CvDangerPlotContents::GetDanger with optional per-attacker damage and scoped enemy rolls.
	static int GetCityDangerByThreat(CvDangerPlotContents& kContents, const CvCity* pCity, const CvUnit* pPretendGarrison,
		const SUnitIDValueContainer& extraUnitDamage, int iExtraSelfDamage, vector<SOperationalThreatDamage>* pThreats, int iRisk);
	// Computes standard city danger and its attacker list once per city plot on the current board.
	static const SOperationalCityDanger& CachedCityDanger(const SOperationalTurn& turn, const CvCity& kCity);

	// True for a city's garrison the garrison pass left to zone work: only a search may move it.
	static bool IsHeldGarrison(const CvUnit* pUnit);
};

// Charges one scope, optionally excluding trace construction from an enclosing stage.
struct SOperationalTimer
{
	unsigned long long* pTotal;
	const unsigned long long* pExclude;
	unsigned long long startNs, excludeNs;
	// Starts timing only when an accumulator is supplied.
	SOperationalTimer(unsigned long long* pValue, const unsigned long long* pExcluded = NULL) :
		pTotal(pValue), pExclude(pExcluded), startNs(pValue ? CvTacticalOperationalLoop::ClockNs() : 0),
		excludeNs(pExcluded ? *pExcluded : 0) {}
	// Keeps an enclosing stage exclusive of trace work, including on unwinding.
	~SOperationalTimer()
	{
		if (pTotal) *pTotal += CvTacticalOperationalLoop::ClockNs() - startNs - (pExclude ? *pExclude - excludeNs : 0);
	}
};

#endif //CIV5_TACTICAL_OPERATIONAL_LOOP_H
