/*	-------------------------------------------------------------------------------------------------------
	Vox Deorum: declarations for the air search (VOX_TACTICAL_AIR_SEARCH): the interceptor state a
	CvTacticalPosition keeps, and the TacticalAIHelpers functions that find launch plots, air targets and
	interceptors and estimate strikes and sweeps. CvTacticalAI.h includes this file, so its users see both.
	------------------------------------------------------------------------------------------------------- */
#pragma once

#ifndef CIV5_TACTICAL_AIR_SEARCH_H
#define CIV5_TACTICAL_AIR_SEARCH_H

class CvUnit;
class CvPlot;
class CvTacticalPosition;

// Vox Deorum: what a plan has done to one enemy interceptor, on top of its live state. A unit without an
// entry has used no interceptions and still has its base.
struct SInterceptorState
{
	// unit IDs are unique across players
	int iUnitID;
	short iInterceptionsUsed;
	bool bBaseLost;

	SInterceptorState(int iUnitID_ = 0) : iUnitID(iUnitID_), iInterceptionsUsed(0), bBaseLost(false) {}
	bool operator<(const SInterceptorState& rhs) const { return iUnitID < rhs.iUnitID; }
	bool operator==(const SInterceptorState& rhs) const
	{
		return iUnitID == rhs.iUnitID && iInterceptionsUsed == rhs.iInterceptionsUsed && bBaseLost == rhs.bBaseLost;
	}
};

namespace TacticalAIHelpers
{
	// Vox Deorum: air search (VOX_TACTICAL_AIR_SEARCH)
	// the number of participants a search keeps: land, sea and aircraft groups together
	int GetTactSimMaxUnits();
	// the plots an aircraft can launch from this turn: its base, plus every plot its carrying ship can reach
	// Only plots a search around pSearchTarget can move the ship to are included.
	vector<int> GetCargoLaunchPlots(const CvUnit* pUnit, const CvPlot* pSearchTarget);
	// the first launch plot, base first, from which the aircraft can strike and/or sweep the target, or NULL
	const CvPlot* GetFirstAirLaunchPlot(const CvUnit* pUnit, const CvPlot* pTarget, bool bStrike, bool bSweep);
	// true if the aircraft's city is in danger of falling or its carrying ship is projected to die
	bool IsAirBaseEndangered(const CvUnit* pUnit);
	// true if the aircraft could strike the target plot from the assumed base
	bool CanAirUnitStrikeFrom(const CvUnit* pUnit, const CvPlot* pBasePlot, const CvPlot* pTarget);
	// true if the aircraft could strike or sweep the target plot from the assumed base
	bool CanAirUnitActAgainstPlot(const CvUnit* pUnit, const CvPlot* pBasePlot, const CvPlot* pTarget);
	// the enemy plots of the position within air range of the assumed base that the aircraft can strike
	vector<int> GetAirTargetPlots(const CvUnit* pUnit, const CvPlot* pBasePlot, const CvTacticalPosition& position);
	// the interceptor that would engage the attacker over the plot given what the position has done so far
	CvUnit* GetBestInterceptorInPosition(const CvPlot* pTargetPlot, const CvUnit* pAttacker, const CvTacticalPosition& position);
	// the air sweeps the search considers from the assumed base: for each interceptor that covers an enemy plot the aircraft
	// can sweep, the covered plot nearest the target, the lower plot index breaking ties. Pairs are (plot, interceptor),
	// ordered by interceptor ID.
	vector<pair<int, const CvUnit*> > GetAirSweepPlots(const CvUnit* pUnit, const CvPlot* pBasePlot, const CvTacticalPosition& position);
	// the chance in 1/10000 that an air strike lands past the interceptors the position has left. It also caps and
	// weights the attacker's losses; the damage the strike deals is left for the caller to cap and weight.
	int GetAirStrikeChanceInPosition(const CvUnit* pAttacker, const CvPlot* pTargetPlot, const CvTacticalPosition& position,
		int& iAttackerDamage, int iExtraSelfDamage);
	// the damage an air sweep from the assumed launch plot trades with the interceptor, without randomness
	void GetSimulatedAirSweepDamage(const CvUnit* pAttacker, const CvUnit* pInterceptor, const CvPlot* pAttackerPlot, const CvPlot* pTargetPlot,
		int iAttackerExtraDamage, int iInterceptorExtraDamage, int& iDamageToInterceptor, int& iDamageToAttacker);
	// the chance in 1/10000 that an air strike lands past the given interceptor (or none), capping and weighting the attacker's losses
	int GetAirStrikeChance(const CvUnit* pAttacker, const CvUnit* pInterceptor, const CvPlot* pTargetPlot, int iInterceptorExtraDamage,
		int iExtraSelfDamage, int& iAttackerDamage);
	// the aircraft and missiles aboard a ship that the viewing team can see
	int CountVisibleAircraftCargo(const CvUnit* pShip, TeamTypes eViewerTeam);
	// clears the per-search air state (aircraft groups, interceptors, air targets, untracked cargo, carrier reach, interception damage)
	void ResetAirSearchCaches();
	// marks the position a search expands until EndAirExpansion, so the air scorers can remember what stays fixed meanwhile
	void BeginAirExpansion(const CvTacticalPosition& position);
	// ends the expansion BeginAirExpansion marked
	void EndAirExpansion();
}

#endif //CIV5_TACTICAL_AIR_SEARCH_H
