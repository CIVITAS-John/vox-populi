/*	-------------------------------------------------------------------------------------------------------
	Vox Deorum: tactical search internals that CvTacticalAI.cpp shares with the air search scorers in
	CvTacticalAirSearch.cpp (VOX_TACTICAL_AIR_SEARCH), the trade search in CvTacticalTradeSearch.cpp
	(VOX_TACTICAL_TRADE_SEARCH) and the operational loop's search copies in CvTacticalOperationalLoop.cpp
	(VOX_TACTICAL_OPERATIONAL_LOOP). Native keeps these file-local in CvTacticalAI.cpp. Only those four files
	include this header, after CvTacticalAI.h.
	------------------------------------------------------------------------------------------------------- */
#pragma once

#ifndef CIV5_TACTICAL_SEARCH_SHARED_H
#define CIV5_TACTICAL_SEARCH_SHARED_H

// the search's position and assignment pools, and the options scored for the current unit
extern CvTactPosStorage gTactPosStorage;
extern CvTactAssignmentStorage gAssignmentStorage;
extern vector<OptionWithScore<STacticalAssignment*> > gPossibleMoves;

// the state the operational loop's copies of the search setup reset or read: the support pool, the reach and attack
// caches, the flooded distances to the search target, the safe plot counts, open-plot danger, the units the search could
// not use, the RISK thresholds and the player the pools were last sized for
extern CvSupportPosStorage gSupportPosStorage;
extern TCachedMovePlots gReachablePlotsLookup;
extern TCachedRangeAttackPlots gRangeAttackPlotsLookup;
extern TCachedDistanceToTargetPlots gDistanceToTargetPlots;
extern CvPlot* gTargetPlot;
extern TUnitFlagLookup gSafePlotCount;
extern tr1::unordered_map<int, int> gOpenPlotDanger;
extern map<int, int> gBadUnitsCount;
extern int gDefaultUnitLossThreshold;
extern int gMinHpForTactsim;
extern PlayerTypes eLastTactSimPlayer;

// true if this attack ends the unit's turn
bool AttackEndsTurn(const CvUnit* pUnit, int iNumAttacksLeft);
// the rough state of the position after the assignment
void GetNextPosition(const CvTacticalPosition& previousPosition, const STacticalAssignment* assignment, CvTacticalPosition& newPosition);
// scores the damage of an attack on the plot into the result, with the search's own damage adjustments
bool ScoreAttackDamage(const CvTacticalPlot* tactPlot, const CvUnit* pUnit, const CvTacticalPlot* assumedPlot, const CvTacticalPosition& assumedPosition, CAttackCache& cache, STacticalAssignment* result, int iSelfDamage);
// the plot score of the unit's latest assignment in the position, or 0
int GetPrevPlotScore(int iUnitID, const CvBasePosition& position);

// an aircraft staying on its base: the stand-down, initial or final row, scored into the next free row of the pool
STacticalAssignment* ScorePlotForAirUnitMove(const SUnitStats& unit, const CvTacticalPlot* testPlot, const CvTacticalPosition& assumedPosition, eUnitMoveEvalMode evalMode);

// the start value and game-scale strategy change of one general flavor (see GetGeneralTacticalFlavors)
void GetGeneralFlavorTerms(const CvFlavorManager* pFlavorManager, FlavorTypes eFlavor, int iDefault, int& iBase, int& iDelta);

// the trade search state CvTacticalAI.cpp reads and resets: the next steps of every route, and this search's routes, the other
// players' units whose death can change its trade score, and the threats to our steps (CvTacticalTradeSearch.cpp)
extern STradeSteps gTradeSteps;
extern vector<int> gSearchTradeRoutes;
extern vector<int> gTradeScoreUnits;
extern tr1::unordered_map<int, vector<const CvUnit*> > gTradeStepThreats;
// the rank bonus for a unit's trade gain from one plot to another, in damage points
int GetTradeRankBonus(const CvUnit* pUnit, const CvTacticalPlot* pToPlot, const CvTacticalPosition& toPosition,
	const CvTacticalPlot* pFromPlot, const CvTacticalPosition& fromPosition);
// whether an assignment damages a unit whose death can change the search's trade score
bool DamagesTradeScoreUnit(const SUnitIDValueContainer& unitDamage);

#endif //CIV5_TACTICAL_SEARCH_SHARED_H
