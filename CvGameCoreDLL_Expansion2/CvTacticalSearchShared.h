/*	-------------------------------------------------------------------------------------------------------
	Vox Deorum: tactical search internals that CvTacticalAI.cpp shares with the air search scorers in
	CvTacticalAirSearch.cpp (VOX_TACTICAL_AIR_SEARCH). Native keeps these file-local in CvTacticalAI.cpp. Only
	those two files include this header, after CvTacticalAI.h.
	------------------------------------------------------------------------------------------------------- */
#pragma once

#ifndef CIV5_TACTICAL_SEARCH_SHARED_H
#define CIV5_TACTICAL_SEARCH_SHARED_H

// the search's position and assignment pools, and the options scored for the current unit
extern CvTactPosStorage gTactPosStorage;
extern CvTactAssignmentStorage gAssignmentStorage;
extern vector<OptionWithScore<STacticalAssignment*> > gPossibleMoves;

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

#endif //CIV5_TACTICAL_SEARCH_SHARED_H
