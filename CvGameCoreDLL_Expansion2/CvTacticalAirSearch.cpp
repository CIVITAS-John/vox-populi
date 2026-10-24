/*	-------------------------------------------------------------------------------------------------------
	Vox Deorum: the air search (VOX_TACTICAL_AIR_SEARCH). This file holds:
	- the helpers for launch plots, air targets, interceptors, the chance a strike lands and sweep damage;
	- the caches that keep those answers for one search, one search tree or one expansion;
	- the scorers for strikes, sweeps and standing down, and the cut that keeps an aircraft's best options;
	- the aircraft, interceptor and carrier state a CvTacticalPosition keeps, and the idle groups of a search.
	CvTacticalSearchShared.h declares the search internals from CvTacticalAI.cpp that the scorers use.
	------------------------------------------------------------------------------------------------------- */
#include "CvGameCoreDLLPCH.h"
#include "CvTacticalAI.h"
#include "CvTacticalSearchShared.h"
#include "CvGameCoreUtils.h"
#include "LintFree.h"

// Vox Deorum: a visible enemy interceptor near a target plot, with its plot distance to that target
struct SInterceptorCandidate
{
	CvUnit* pUnit;
	int iDistance;
	SInterceptorCandidate(CvUnit* pUnit_, int iDistance_) : pUnit(pUnit_), iDistance(iDistance_) {}
};
// Vox Deorum: the interceptor candidates per target plot, for this search (see GetInterceptorCandidates)
map<int, vector<SInterceptorCandidate> > gInterceptorCandidates;
// Vox Deorum: the air targets per (unit ID, base plot), for this search (see GetAirTargetPlots)
map<pair<int, int>, vector<int> > gAirTargetPlots;
// Vox Deorum: ship ID to the number of aircraft aboard that the search does not track (see recordCarrierCargo)
map<int, int> gUntrackedCargo;
// Vox Deorum: the plots a carrying ship can reach this turn, per (owner, ship). Kept for one recruitment or one search only,
// since moves executed in between change both the ship and the routes.
map<pair<int, int>, vector<int> > gCargoLaunchPlots;
// Vox Deorum: the group ID of each kind of interchangeable aircraft in this search (see initAircraftStats for the key)
map<vector<int>, int> gAirGroupKeys;
// Vox Deorum: scratch rows where ScoreAirOptions scores one aircraft's options before getPreferredAssignmentsForAirUnit
// keeps the best. Every call reuses the rows, so only the ones the last call counted are current.
vector<STacticalAssignment> gAirOptions;
// Vox Deorum: the inputs of one interception's damage to the attacker: both units, the target plot and the damage the position
// assumes for each. Real units and the map stay fixed while a search runs, so nothing else changes the result.
struct SInterceptionKey
{
	int iInterceptorID;
	int iAttackerID;
	int iPlotIndex;
	int iInterceptorDamage;
	int iAttackerDamage;

	bool operator==(const SInterceptionKey& rhs) const
	{
		return iInterceptorID == rhs.iInterceptorID && iAttackerID == rhs.iAttackerID && iPlotIndex == rhs.iPlotIndex &&
			iInterceptorDamage == rhs.iInterceptorDamage && iAttackerDamage == rhs.iAttackerDamage;
	}
};

// Vox Deorum: hashes an SInterceptionKey, mixing its fields like AttackKeyHash
struct SInterceptionKeyHash
{
	size_t operator()(const SInterceptionKey& k) const
	{
		size_t h = (size_t)k.iInterceptorID;
		h ^= (size_t)k.iAttackerID + 0x9e3779b9 + (h << 6) + (h >> 2);
		h ^= (size_t)k.iPlotIndex + 0x9e3779b9 + (h << 6) + (h >> 2);
		h ^= (size_t)k.iInterceptorDamage + 0x9e3779b9 + (h << 6) + (h >> 2);
		h ^= (size_t)k.iAttackerDamage + 0x9e3779b9 + (h << 6) + (h >> 2);
		return h;
	}
};

// Vox Deorum: the inputs of an interceptor's value over a plot (CvPlot::GetInterceptorValue) that change during a search:
// the interceptor, the plot, and the damage and interceptions the position assumes for the interceptor
struct SInterceptorValueKey
{
	int iInterceptorID;
	int iPlotIndex;
	int iDamage;
	int iInterceptionsUsed;

	bool operator==(const SInterceptorValueKey& rhs) const
	{
		return iInterceptorID == rhs.iInterceptorID && iPlotIndex == rhs.iPlotIndex && iDamage == rhs.iDamage &&
			iInterceptionsUsed == rhs.iInterceptionsUsed;
	}
};

// Vox Deorum: hashes an SInterceptorValueKey, mixing its fields like AttackKeyHash
struct SInterceptorValueKeyHash
{
	size_t operator()(const SInterceptorValueKey& k) const
	{
		size_t h = (size_t)k.iInterceptorID;
		h ^= (size_t)k.iPlotIndex + 0x9e3779b9 + (h << 6) + (h >> 2);
		h ^= (size_t)k.iDamage + 0x9e3779b9 + (h << 6) + (h >> 2);
		h ^= (size_t)k.iInterceptionsUsed + 0x9e3779b9 + (h << 6) + (h >> 2);
		return h;
	}
};

// Vox Deorum: what ranking sweeps in the position being expanded has learned about one aircraft and one swept interceptor.
// A certain sweep child differs from that position only in the interceptor's damage and interceptions, so for an aircraft
// other than the sweeper the gain depends on the sweep's damage alone.
struct SGuardedAirStrikes
{
	// true once targets is filled
	bool bTargetsKnown;
	// the target plots the interceptor guards against the aircraft, each with the aircraft's strike value there
	vector<pair<const CvTacticalPlot*, int> > targets;
	// (damage a sweep deals the interceptor, the aircraft's best strike gain after it)
	vector<pair<int, int> > gainsByDamage;

	SGuardedAirStrikes() : bTargetsKnown(false) {}
};

// Vox Deorum: the position a search is expanding (see BeginAirExpansion), or NULL. The caches below are used only
// while it is set, since outside a search executed moves change the real units.
const CvTacticalPosition* gAirExpansion = NULL;
// Vox Deorum: interception damage to the attacker, for one search tree
tr1::unordered_map<SInterceptionKey, int, SInterceptionKeyHash> gInterceptionDamage;
// Vox Deorum: interceptor values over a plot, for one search tree
tr1::unordered_map<SInterceptorValueKey, int, SInterceptorValueKeyHash> gInterceptorValues;
// Vox Deorum: per (aircraft, target plot) in the position being expanded, the strike value (see GetAirStrikeValueInExpansion).
// Every sweep the position ranks compares against these same strikes.
tr1::unordered_map<pair<int, int>, int, SIntPairHash> gExpansionStrikeValues;
// Vox Deorum: per (aircraft, target plot) in the position being expanded, the interceptor the strike meets
tr1::unordered_map<pair<int, int>, const CvUnit*, SIntPairHash> gExpansionInterceptors;
// Vox Deorum: per (aircraft, swept interceptor) in the position being expanded, the aircraft's guarded strikes and the gains
// found so far (see GetAirSweepRankBonus)
tr1::unordered_map<pair<int, int>, SGuardedAirStrikes, SIntPairHash> gExpansionGuardedStrikes;
// Vox Deorum: a missile on a base about to be lost fires before an equal missile on a safe base. The bonus is the value of a
// unit that would otherwise be lost, on the scale of the bonus for sinking a ship with aircraft on board.
const int TACTSIM_ENDANGERED_MISSILE_BONUS = 15;
// Vox Deorum: an aircraft group with no positive option, whose best strike ranks below this when a search starts, only
// stands down until the interceptors over its targets change. RankScore counts 10 per damage point, so the margin is 10
// damage points, which covers targets that other attacks soften first.
const int TACTSIM_AIR_HOPELESS_RANK = -100;
// Vox Deorum: group ID of each idle aircraft group of the search to the visible interceptors near its targets
// (see getUnitsToDropWithAircraft and IsAirGroupIdle)
map<int, vector<int> > gIdleAirGroups;

// Vox Deorum: forgets the aircraft groups, the idle groups, and the cached interceptors, air targets, untracked cargo,
// carrier reach, interception damage and interceptor values. Every entry is built again on first use, so a recruitment or
// a search starts from the current units and routes.
void TacticalAIHelpers::ResetAirSearchCaches()
{
	gAirGroupKeys.clear();
	gInterceptorCandidates.clear();
	gAirTargetPlots.clear();
	gUntrackedCargo.clear();
	gCargoLaunchPlots.clear();
	gInterceptionDamage.clear();
	gInterceptorValues.clear();
	gIdleAirGroups.clear();
}

// Vox Deorum: marks the position a search is about to expand, so the air scorers can remember what stays fixed until
// EndAirExpansion. The expansion caches start empty. A position without a parent starts a new search tree, so the
// interception damage and interceptor values of the last tree are forgotten too.
void TacticalAIHelpers::BeginAirExpansion(const CvTacticalPosition& position)
{
	if (!position.getParent())
	{
		gInterceptionDamage.clear();
		gInterceptorValues.clear();
	}
	gExpansionStrikeValues.clear();
	gExpansionInterceptors.clear();
	gExpansionGuardedStrikes.clear();
	gAirExpansion = &position;
}

// Vox Deorum: ends the expansion BeginAirExpansion marked, so later calls compute everything directly
void TacticalAIHelpers::EndAirExpansion()
{
	gAirExpansion = NULL;
}

// Vox Deorum: CvUnit::GetInterceptionDamage without randomness, remembered for the search tree while a search expands a position
static int GetInterceptionDamageInSearch(const CvUnit* pInterceptor, const CvUnit* pAttacker, const CvPlot* pTargetPlot,
	int iInterceptorDamage, int iAttackerDamage)
{
	if (!gAirExpansion)
		return pInterceptor->GetInterceptionDamage(pAttacker, false, pTargetPlot, iInterceptorDamage, iAttackerDamage);

	SInterceptionKey key;
	key.iInterceptorID = pInterceptor->GetID();
	key.iAttackerID = pAttacker->GetID();
	key.iPlotIndex = pTargetPlot->GetPlotIndex();
	key.iInterceptorDamage = iInterceptorDamage;
	key.iAttackerDamage = iAttackerDamage;

	tr1::unordered_map<SInterceptionKey, int, SInterceptionKeyHash>::const_iterator it = gInterceptionDamage.find(key);
	if (it != gInterceptionDamage.end())
		return it->second;

	int iDamage = pInterceptor->GetInterceptionDamage(pAttacker, false, pTargetPlot, iInterceptorDamage, iAttackerDamage);
	gInterceptionDamage[key] = iDamage;
	return iDamage;
}

// Vox Deorum: CvPlot::GetInterceptorValue for the interceptor's distance to the plot, remembered for the search tree while a
// search expands a position
static int GetInterceptorValueInSearch(const CvPlot* pTargetPlot, const CvUnit* pInterceptor, int iDistance, int iDamage, int iInterceptionsUsed)
{
	if (!gAirExpansion)
		return pTargetPlot->GetInterceptorValue(pInterceptor, iDistance, false, iDamage, iInterceptionsUsed);

	SInterceptorValueKey key;
	key.iInterceptorID = pInterceptor->GetID();
	key.iPlotIndex = pTargetPlot->GetPlotIndex();
	key.iDamage = iDamage;
	key.iInterceptionsUsed = iInterceptionsUsed;

	tr1::unordered_map<SInterceptorValueKey, int, SInterceptorValueKeyHash>::const_iterator it = gInterceptorValues.find(key);
	if (it != gInterceptorValues.end())
		return it->second;

	int iValue = pTargetPlot->GetInterceptorValue(pInterceptor, iDistance, false, iDamage, iInterceptionsUsed);
	gInterceptorValues[key] = iValue;
	return iValue;
}

// Vox Deorum: the chance in 1/10000 that this interceptor stops this attacker: its interception probability from the HP it
// has left after iInterceptorExtraDamage, times the chance the attacker does not evade. With no extra damage this is
// interceptionProbability(), as the live game uses it.
static int GetAirInterceptionChance(const CvUnit* pInterceptor, const CvUnit* pAttacker, int iInterceptorExtraDamage)
{
	int iHitPoints = max(0, pInterceptor->GetCurrHitPoints() - iInterceptorExtraDamage);
	int iProbability = (pInterceptor->getInterceptChance() * iHitPoints) / max(1, pInterceptor->GetMaxHitPoints());
	return range(iProbability, 0, 100) * (100 - range(pAttacker->evasionProbability(), 0, 100));
}

// Vox Deorum: the chance in 1/10000 that an air strike lands past the given interceptor (or none), with the attacker's
// expected losses: its losses are capped by the HP it has left, and its retaliation is weighted against the interception.
// The damage the strike deals is left alone, so each caller weights it after its own caps. Shared by the live estimate and
// the tactical simulation, which differ only in how they pick the interceptor and how much damage it has already taken.
int TacticalAIHelpers::GetAirStrikeChance(const CvUnit* pAttacker, const CvUnit* pInterceptor, const CvPlot* pTargetPlot, int iInterceptorExtraDamage,
	int iExtraSelfDamage, int& iAttackerDamage)
{
	// Cap each possible loss before averaging, including damage assumed by the tactical simulation.
	int iRemainingHitPoints = max(0, pAttacker->GetCurrHitPoints() - iExtraSelfDamage);
	iAttackerDamage = min(iAttackerDamage, iRemainingHitPoints);
	if (!pInterceptor)
		return 10000;

	// both sides fight at their assumed HP; without the air search the live estimate keeps the attacker at full strength
	int iAssumedAttackerDamage = MOD_VOX_TACTICAL_AIR_SEARCH ? iExtraSelfDamage : 0;
	int iInterceptionDamage = min(GetInterceptionDamageInSearch(pInterceptor, pAttacker, pTargetPlot, iInterceptorExtraDamage, iAssumedAttackerDamage), iRemainingHitPoints);
	// Execution continues the strike when an interception does no damage.
	if (iInterceptionDamage <= 0)
		return 10000;

	int iInterceptionChance = GetAirInterceptionChance(pInterceptor, pAttacker, iInterceptorExtraDamage);
	if (iInterceptionChance <= 0)
		return 10000;

	int iStrikeChance = 10000 - iInterceptionChance;
	iAttackerDamage = (iInterceptionDamage * iInterceptionChance + iAttackerDamage * iStrikeChance) / 10000;
	return iStrikeChance;
}

// Vox Deorum: the plots an aircraft can launch from this turn: its own plot, plus the plots its carrying ship can reach that
// a search around pSearchTarget can also move it to. The search only holds plots within TACTICAL_COMBAT_MAX_TARGET_DISTANCE + 1
// of its target, and never moves a ship farther from the target (with distances clamped at TACTICAL_COMBAT_MAX_TARGET_DISTANCE,
// see getPreferredAssignmentsForUnit). Plain plot distance stands in for the search's sea path distance.
vector<int> TacticalAIHelpers::GetCargoLaunchPlots(const CvUnit* pUnit, const CvPlot* pSearchTarget)
{
	vector<int> result;
	if (!pUnit || !pUnit->plot())
		return result;

	int iBasePlot = pUnit->plot()->GetPlotIndex();
	result.push_back(iBasePlot);

	const CvUnit* pShip = pUnit->getTransportUnit();
	// carriers belong to operations and are never tactical units on their own, so only the ship's state is checked
	if (!pShip || !pShip->canUseNow())
		return result;

	// every aircraft on the ship shares one pathfinder run within a recruitment or a search
	pair<int, int> shipKey((int)pShip->getOwner(), pShip->GetID());
	map<pair<int, int>, vector<int> >::iterator cached = gCargoLaunchPlots.find(shipKey);
	if (cached == gCargoLaunchPlots.end())
	{
		cached = gCargoLaunchPlots.insert(make_pair(shipKey, vector<int>())).first;
		ReachablePlots reachable = GetAllPlotsInReachThisTurn(pShip, pShip->plot(), CvUnit::MOVEFLAG_IGNORE_STACKING_SELF | CvUnit::MOVEFLAG_IGNORE_DANGER, 0);
		for (ReachablePlots::const_iterator it = reachable.begin(); it != reachable.end(); ++it)
			if (it->iPlotIndex != iBasePlot)
				cached->second.push_back(it->iPlotIndex);
	}

	if (!pSearchTarget)
		return result;

	const vector<int>& vShipPlots = cached->second;
	int iShipDistance = max(plotDistance(*pShip->plot(), *pSearchTarget), (int)TACTICAL_COMBAT_MAX_TARGET_DISTANCE);
	for (size_t i = 0; i < vShipPlots.size(); i++)
	{
		int iDistance = plotDistance(*GC.getMap().plotByIndexUnchecked(vShipPlots[i]), *pSearchTarget);
		if (iDistance > TACTICAL_COMBAT_MAX_TARGET_DISTANCE + 1 || max(iDistance, (int)TACTICAL_COMBAT_MAX_TARGET_DISTANCE) > iShipDistance)
			continue;

		result.push_back(vShipPlots[i]);
	}

	return result;
}

// Vox Deorum: the first plot the aircraft can launch from this turn (its base first) that lets it strike or sweep the target,
// or NULL if none does. Used at recruitment, where the target is also the search target that limits the launch plots.
const CvPlot* TacticalAIHelpers::GetFirstAirLaunchPlot(const CvUnit* pUnit, const CvPlot* pTarget, bool bStrike, bool bSweep)
{
	if (!pUnit || !pTarget)
		return NULL;

	vector<int> vLaunchPlots = GetCargoLaunchPlots(pUnit, pTarget);
	for (size_t i = 0; i < vLaunchPlots.size(); i++)
	{
		const CvPlot* pLaunchPlot = GC.getMap().plotByIndexUnchecked(vLaunchPlots[i]);
		if (bStrike && CanAirUnitStrikeFrom(pUnit, pLaunchPlot, pTarget))
			return pLaunchPlot;
		if (bSweep && pUnit->canAirSweepAt(pTarget->getX(), pTarget->getY(), pLaunchPlot))
			return pLaunchPlot;
	}
	return NULL;
}

// Vox Deorum: true if the aircraft's base is about to be lost, by the rule CvTacticalAI::ShouldRebase uses: a city in
// danger of falling, or a carrying ship projected to die next turn.
bool TacticalAIHelpers::IsAirBaseEndangered(const CvUnit* pUnit)
{
	if (!pUnit || !pUnit->plot())
		return false;

	if (pUnit->plot()->isCity())
		return pUnit->plot()->getPlotCity()->isInDangerOfFalling(true);

	const CvUnit* pCarrier = pUnit->getTransportUnit();
	return pCarrier && pCarrier->isProjectedToDieNextTurn();
}

// Vox Deorum: true if the aircraft can strike the target plot from the assumed base plot
bool TacticalAIHelpers::CanAirUnitStrikeFrom(const CvUnit* pUnit, const CvPlot* pBasePlot, const CvPlot* pTarget)
{
	// the game's own target rules, which execution checks too, from the assumed base
	return pUnit && pBasePlot && pTarget && pUnit->canRangeStrikeAt(pTarget->getX(), pTarget->getY(), true, true, pBasePlot);
}

// Vox Deorum: true if the aircraft can strike or sweep the target plot from the assumed base plot
bool TacticalAIHelpers::CanAirUnitActAgainstPlot(const CvUnit* pUnit, const CvPlot* pBasePlot, const CvPlot* pTarget)
{
	if (!pUnit || !pBasePlot || !pTarget)
		return false;

	return CanAirUnitStrikeFrom(pUnit, pBasePlot, pTarget) || pUnit->canAirSweepAt(pTarget->getX(), pTarget->getY(), pBasePlot);
}

// Vox Deorum: the enemy plots of the search that the aircraft can strike from the assumed base plot. The enemy plots
// are checked by plot distance, so air ranges beyond the ring tables work. The list is built once per unit and base for
// the search, from the root position. Later positions can only lose enemy plots, so callers check isEnemy() in their own
// position.
const vector<int>& TacticalAIHelpers::GetAirTargetPlots(const CvUnit* pUnit, const CvPlot* pBasePlot, const CvTacticalPosition& position)
{
	static const vector<int> noTargets;
	if (!pUnit || !pBasePlot)
		return noTargets;

	pair<int, int> key(pUnit->GetID(), pBasePlot->GetPlotIndex());
	map<pair<int, int>, vector<int> >::const_iterator itCached = gAirTargetPlots.find(key);
	if (itCached != gAirTargetPlots.end())
		return itCached->second;

	const CvTacticalPosition* pRoot = &position;
	while (pRoot->getParent())
		pRoot = pRoot->getParent();

	vector<int>& result = gAirTargetPlots[key];
	const PlotIndexContainer& enemyPlots = pRoot->getEnemyPlots();
	for (PlotIndexContainer::const_iterator it = enemyPlots.begin(); it != enemyPlots.end(); ++it)
	{
		const CvPlot* pPlot = GC.getMap().plotByIndexUnchecked(*it);
		if (!pPlot || plotDistance(*pBasePlot, *pPlot) > pUnit->GetRange())
			continue;

		if (CanAirUnitStrikeFrom(pUnit, pBasePlot, pPlot))
			result.push_back(*it);
	}

	return result;
}

// Vox Deorum: the visible enemy interceptors within 11 plots of a target plot, a rough bound on interception range. Cached
// per plot for the search, since only one player searches at a time.
static const vector<SInterceptorCandidate>& GetInterceptorCandidates(const CvPlot* pTargetPlot, PlayerTypes eAttackingPlayer)
{
	map<int, vector<SInterceptorCandidate> >::const_iterator itCached = gInterceptorCandidates.find(pTargetPlot->GetPlotIndex());
	if (itCached != gInterceptorCandidates.end())
		return itCached->second;

	vector<SInterceptorCandidate>& candidates = gInterceptorCandidates[pTargetPlot->GetPlotIndex()];
	TeamTypes eAttackingTeam = GET_PLAYER(eAttackingPlayer).getTeam();
	const std::vector<PlayerTypes>& vEnemies = GET_PLAYER(eAttackingPlayer).GetPlayersAtWarWith();
	for (size_t iI = 0; iI < vEnemies.size(); iI++)
	{
		CvPlayerAI& kLoopPlayer = GET_PLAYER(vEnemies[iI]);
		const std::vector<std::pair<int, int>>& possibleUnits = kLoopPlayer.GetPossibleInterceptors();
		for (std::vector<std::pair<int, int>>::const_iterator it = possibleUnits.begin(); it != possibleUnits.end(); ++it)
		{
			//we only know about visible interceptors
			CvPlot* pInterceptorPlot = GC.getMap().plotByIndexUnchecked(it->second);
			if (!pInterceptorPlot->isVisible(eAttackingTeam))
				continue;

			//first a very rough distance check to avoid expensive unit lookup
			int iDistance = plotDistance(*pInterceptorPlot, *pTargetPlot);
			if (iDistance > 11)
				continue;

			CvUnit* pInterceptorUnit = kLoopPlayer.getUnit(it->first);
			if (!pInterceptorUnit || pInterceptorUnit->isDelayedDeath())
				continue;

			candidates.push_back(SInterceptorCandidate(pInterceptorUnit, iDistance));
		}
	}

	return candidates;
}

// Vox Deorum: the interceptor that would engage the attacker over the plot, given what the plan has done so far, or NULL.
// Uses the visible interceptors only, skips any whose base the position has taken, and ranks with the game's own rule
// (CvPlot::GetInterceptorValue) against the HP and interceptions the position has left them. The nearer one wins a tie.
CvUnit* TacticalAIHelpers::GetBestInterceptorInPosition(const CvPlot* pTargetPlot, const CvUnit* pAttacker, const CvTacticalPosition& position)
{
	if (!pTargetPlot || !pAttacker)
		return NULL;

	CvUnit* pBestUnit = NULL;
	int iBestValue = 0;
	int iBestDistance = INT_MAX;

	const vector<SInterceptorCandidate>& candidates = GetInterceptorCandidates(pTargetPlot, pAttacker->getOwner());
	for (size_t i = 0; i < candidates.size(); i++)
	{
		CvUnit* pInterceptorUnit = candidates[i].pUnit;
		int iUnitID = pInterceptorUnit->GetID();

		//stealth unit? no intercept
		if (pAttacker->isInvisible(GET_PLAYER(pInterceptorUnit->getOwner()).getTeam(), false, false))
			continue;

		//what the plan has done to this one
		if (position.isInterceptorBaseLost(iUnitID))
			continue;

		int iValue = GetInterceptorValueInSearch(pTargetPlot, pInterceptorUnit, candidates[i].iDistance, position.GetUnitDamage(iUnitID), position.getInterceptionsUsed(iUnitID));
		if (iValue < 0)
			continue;

		if (iValue > iBestValue || (iValue == iBestValue && candidates[i].iDistance < iBestDistance))
		{
			iBestDistance = candidates[i].iDistance;
			iBestValue = iValue;
			pBestUnit = pInterceptorUnit;
		}
	}

	return pBestUnit;
}

// Vox Deorum: one sweep for each distinct interceptor that covers an enemy plot the aircraft can sweep from the assumed base,
// over the covered plot nearest the real target, the lower plot index breaking ties. The pairs are (plot, interceptor),
// ordered by interceptor ID. The simulator's command checks and tokens use the same list.
vector<pair<int, const CvUnit*> > TacticalAIHelpers::GetAirSweepPlots(const CvUnit* pUnit, const CvPlot* pBasePlot, const CvTacticalPosition& position)
{
	//interceptor ID -> ((distance to target, plot), interceptor)
	map<int, pair<pair<int, int>, const CvUnit*> > sweepPlots;
	const PlotIndexContainer& enemies = position.getEnemyPlots();
	for (PlotIndexContainer::const_iterator it = enemies.begin(); it != enemies.end(); ++it)
	{
		const CvTacticalPlot* enemyPlot = position.getTactPlot(*it);
		if (!enemyPlot || !enemyPlot->isEnemy())
			continue;

		const CvPlot* pEnemyPlot = enemyPlot->getPlot();
		if (!pUnit->canAirSweepAt(pEnemyPlot->getX(), pEnemyPlot->getY(), pBasePlot))
			continue;

		const CvUnit* pInterceptor = GetBestInterceptorInPosition(pEnemyPlot, pUnit, position);
		if (!pInterceptor)
			continue;

		int key = pInterceptor->GetID();
		pair<int, int> value(plotDistance(*pEnemyPlot, *position.getTarget()), *it);
		map<int, pair<pair<int, int>, const CvUnit*> >::iterator found = sweepPlots.find(key);
		if (found == sweepPlots.end() || value < found->second.first)
			sweepPlots[key] = make_pair(value, pInterceptor);
	}

	vector<pair<int, const CvUnit*> > result;
	for (map<int, pair<pair<int, int>, const CvUnit*> >::const_iterator it = sweepPlots.begin(); it != sweepPlots.end(); ++it)
		result.push_back(make_pair(it->second.first.second, it->second.second));
	return result;
}

// Vox Deorum: the chance in 1/10000 that an air strike lands, against the interceptor state of the position, using the same
// rule as AdjustAirAttackDamageForInterception (GetAirStrikeChance). The strike is uncertain when the chance is below 10000.
// ScoreAttackDamage weights the strike's damage by it after capping that damage at the HP the target has left.
int TacticalAIHelpers::GetAirStrikeChanceInPosition(const CvUnit* pAttacker, const CvPlot* pTargetPlot, const CvTacticalPosition& position,
	int& iAttackerDamage, int iExtraSelfDamage)
{
	CvUnit* pInterceptor = GetBestInterceptorInPosition(pTargetPlot, pAttacker, position);
	int iInterceptorDamage = pInterceptor ? position.GetUnitDamage(pInterceptor->GetID()) : 0;
	return GetAirStrikeChance(pAttacker, pInterceptor, pTargetPlot, iInterceptorDamage, iExtraSelfDamage, iAttackerDamage);
}

// Vox Deorum: the damage an air sweep trades with the interceptor it engages, without randomness. Ground AA takes no damage
// and hits the sweeper with reduced interception damage; an air interceptor trades melee-style damage. The sweeper fights
// from the assumed launch plot, which differs from its real plot when a carrier is planned to move.
void TacticalAIHelpers::GetSimulatedAirSweepDamage(const CvUnit* pAttacker, const CvUnit* pInterceptor, const CvPlot* pAttackerPlot, const CvPlot* pTargetPlot,
	int iAttackerExtraDamage, int iInterceptorExtraDamage, int& iDamageToInterceptor, int& iDamageToAttacker)
{
	iDamageToInterceptor = 0;
	iDamageToAttacker = 0;
	if (!pAttacker || !pInterceptor)
		return;

	int iAttackerHP = max(0, pAttacker->GetCurrHitPoints() - iAttackerExtraDamage);
	int iInterceptorHP = max(0, pInterceptor->GetCurrHitPoints() - iInterceptorExtraDamage);

	// Ground AA interceptor
	if (pInterceptor->getDomainType() != DOMAIN_AIR)
	{
		int iInterceptionDamage = GetInterceptionDamageInSearch(pInterceptor, pAttacker, pTargetPlot, iInterceptorExtraDamage, iAttackerExtraDamage);

		// Reduce damage for performing a sweep
		iInterceptionDamage *= 100 + /*-50*/ GD_INT_GET(AIR_SWEEP_INTERCEPTION_DAMAGE_MOD);
		iInterceptionDamage /= 100;

		iDamageToAttacker = min(max(0, iInterceptionDamage), iAttackerHP);
		return;
	}

	// Air interceptor
	int iAttackerStrength = pAttacker->GetMaxRangedCombatStrength(pInterceptor, NULL, true, pAttackerPlot, NULL, false, false, iAttackerExtraDamage, iInterceptorExtraDamage);

	// Mod to air sweep strength
	iAttackerStrength *= (100 + pAttacker->GetAirSweepCombatModifier());
	iAttackerStrength /= 100;

	int iDefenderStrength = pInterceptor->GetMaxRangedCombatStrength(pAttacker, NULL, false, NULL, pAttackerPlot, false, false, iInterceptorExtraDamage, iAttackerExtraDamage);

	int iDefenderDamageInflicted = 0;
	int iAttackerDamageInflicted = pAttacker->getMeleeCombatDamage(iAttackerStrength, iDefenderStrength, iDefenderDamageInflicted, false, pInterceptor, iAttackerExtraDamage, iInterceptorExtraDamage);

	iDamageToInterceptor = min(max(0, iAttackerDamageInflicted), iInterceptorHP);
	iDamageToAttacker = min(max(0, iDefenderDamageInflicted), iAttackerHP);
}

// Vox Deorum: aircraft and missiles carried by a ship that the player can see, for the bonus when sinking it
int TacticalAIHelpers::CountVisibleAircraftCargo(const CvUnit* pShip, TeamTypes eViewerTeam)
{
	int iCount = 0;
	if (!pShip || !pShip->hasCargo() || !pShip->plot())
		return iCount;

	const CvPlot* pShipPlot = pShip->plot();
	for (int i = 0; i < pShipPlot->getNumUnits(); i++)
	{
		const CvUnit* pCargo = pShipPlot->getUnitByIndex(i);
		if (pCargo && pCargo->getDomainType() == DOMAIN_AIR && pCargo->getTransportUnit() == pShip && !pCargo->isDelayedDeath() && !pCargo->isInvisible(eViewerTeam, false, false))
			iCount++;
	}
	return iCount;
}

// applies the damage of a strike to the certain record, or to the uncertain record when an interceptor may stop it.
// An uncertain strike that kills if it lands also adds to the chance that its target is already dead.
void CvTacticalPosition::applyStrikeDamage(const STacticalAssignment& strike)
{
	if (strike.bUncertain && strike.iUncertainKillChance > 0)
	{
		const CvTacticalPlot* pTargetPlot = getTactPlot(strike.iToPlotIndex);
		const CvUnit* pTarget = pTargetPlot ? pTargetPlot->getEnemyUnit() : NULL;
		if (pTarget)
			uncertainKillChance.write().ChangeValue(pTarget->GetID(), strike.iUncertainKillChance);
	}

	for (SUnitIDValueContainer::const_iterator it = strike.unitDamage.begin(); it != strike.unitDamage.end(); ++it)
	{
		if (strike.bUncertain)
			ChangeUncertainUnitDamage((*it).first, (*it).second);
		else
			ChangeUnitDamage((*it).first, (*it).second);
	}
	if (strike.iDamagedCityId != -1)
	{
		if (strike.bUncertain)
			ChangeUncertainCityDamage(strike.iDamagedCityId, strike.iCityDamage);
		else
			ChangeCityDamage(strike.iDamagedCityId, strike.iCityDamage);
	}
}

// the interceptions the plan has used up of this interceptor
int CvTacticalPosition::getInterceptionsUsed(int iUnitID) const
{
	const vector<SInterceptorState>& states = interceptorStates.read();
	vector<SInterceptorState>::const_iterator it = std::lower_bound(states.begin(), states.end(), SInterceptorState(iUnitID));
	return (it != states.end() && it->iUnitID == iUnitID) ? it->iInterceptionsUsed : 0;
}

// true if the plan has taken this interceptor's base
bool CvTacticalPosition::isInterceptorBaseLost(int iUnitID) const
{
	const vector<SInterceptorState>& states = interceptorStates.read();
	vector<SInterceptorState>::const_iterator it = std::lower_bound(states.begin(), states.end(), SInterceptorState(iUnitID));
	return it != states.end() && it->iUnitID == iUnitID && it->bBaseLost;
}

// finds the entry for an interceptor in the sorted list, adding a fresh one if the plan has not touched it yet
static SInterceptorState& GetOrAddInterceptorState(vector<SInterceptorState>& states, int iUnitID)
{
	SInterceptorState key(iUnitID);
	vector<SInterceptorState>::iterator it = std::lower_bound(states.begin(), states.end(), key);
	if (it == states.end() || it->iUnitID != iUnitID)
		it = states.insert(it, key);
	return *it;
}

// adds to the interceptions the plan has used up of this interceptor
void CvTacticalPosition::changeInterceptionsUsed(int iUnitID, int iChange)
{
	GetOrAddInterceptorState(interceptorStates.write(), iUnitID).iInterceptionsUsed += (short)iChange;
}

// records that the plan has taken this interceptor's base
void CvTacticalPosition::markInterceptorBaseLost(int iUnitID)
{
	GetOrAddInterceptorState(interceptorStates.write(), iUnitID).bBaseLost = true;
}

// an air sweep or an air strike uses up one interception of the interceptor the game would pick, whether or not the
// interception succeeds (CvUnitCombat spends it either way). Any damage goes through the assignment's damage record.
// An assignment does not carry its interceptor, so it is picked again here. The pick matches the scored one, because the
// state is the one the row was scored against: a combo move only pairs such a row with a later stand-down, finish or
// blocked row (makeNextAssignments), and none of those touch the interceptor state. Call it before the row's damage is
// applied, since the game picks the interceptor before the attack resolves.
void CvTacticalPosition::applyAirInterception(const STacticalAssignment& row)
{
	bool bAirStrike = row.eMoveType == MS_AIR && (row.eAssignmentType == A_RANGEATTACK || row.eAssignmentType == A_RANGEKILL);
	if (row.eAssignmentType != A_AIRSWEEP && !bAirStrike)
		return;

	CvUnit* pAttacker = GET_PLAYER(ePlayer).getUnit(row.iUnitID);
	const CvPlot* pTarget = GC.getMap().plotByIndexUnchecked(row.iToPlotIndex);
	if (!pAttacker || !pTarget)
		return;

	CvUnit* pInterceptor = TacticalAIHelpers::GetBestInterceptorInPosition(pTarget, pAttacker, *this);
	if (pInterceptor)
		changeInterceptionsUsed(pInterceptor->GetID(), 1);
}

// true for a unit without an aircraft group or missing from the available units, and for the lowest-ID member of its
// group that is still available
bool CvTacticalPosition::isAirGroupLeader(int iUnitID) const
{
	const vector<SUnitStats>& units = availableUnits.read();
	const SUnitStats* pStats = NULL;
	for (size_t i = 0; i < units.size(); i++)
		if (units[i].iUnitID == iUnitID)
		{
			pStats = &units[i];
			break;
		}

	if (!pStats || pStats->iAirGroup < 0)
		return true;

	for (size_t i = 0; i < units.size(); i++)
		if (units[i].iAirGroup == pStats->iAirGroup && units[i].iUnitID < iUnitID)
			return false;

	return true;
}

// removes a fired missile or a predicted-dead aircraft, which get no later strike, sweep or finish row
void CvTacticalPosition::removeSpentAircraft(int iUnitID)
{
	vector<SUnitStats>& availableUnits_w = availableUnits.write();
	vector<SUnitStats>::iterator it = find_if(availableUnits_w.begin(), availableUnits_w.end(), PrMatchingUnit(iUnitID));
	if (it != availableUnits_w.end())
		availableUnits_w.erase(it);
}

// a ship that is predicted to die takes the aircraft it carries with it
void CvTacticalPosition::removeCargoOfCarrier(int iCarrierID)
{
	vector<SUnitStats>& availableUnits_w = availableUnits.write();
	for (vector<SUnitStats>::iterator it = availableUnits_w.begin(); it != availableUnits_w.end(); )
	{
		if (it->eMoveStrategy == MS_AIR && it->iCarrierID == iCarrierID)
			it = availableUnits_w.erase(it);
		else
			++it;
	}
}

// the aircraft aboard a ship in this position: the ones the search tracks, which drop out when the plan spends them. Unless
// bTrackedOnly, also the ones it never tracked (already acted, dropped by the cap, or not recruited), which
// recordCarrierCargo counts once at the root.
int CvTacticalPosition::countCargoAircraft(int iCarrierID, bool bTrackedOnly) const
{
	int iCount = 0;
	const vector<SUnitStats>* lists[3] = { &availableUnits.read(), &notQuiteFinishedUnits.read(), &finishedUnits.read() };
	for (int iList = 0; iList < 3; iList++)
		for (size_t i = 0; i < lists[iList]->size(); i++)
			if ((*lists[iList])[i].eMoveStrategy == MS_AIR && (*lists[iList])[i].iCarrierID == iCarrierID)
				iCount++;

	if (!bTrackedOnly)
	{
		map<int, int>::const_iterator itUntracked = gUntrackedCargo.find(iCarrierID);
		if (itUntracked != gUntrackedCargo.end())
			iCount += itUntracked->second;
	}

	return iCount;
}

// called on the root once the cap has run: for every ship in the search, the aircraft aboard that the search does not track
void CvTacticalPosition::recordCarrierCargo() const
{
	gUntrackedCargo.clear();
	TeamTypes eTeam = GET_PLAYER(ePlayer).getTeam();
	const vector<SUnitStats>* lists[3] = { &availableUnits.read(), &notQuiteFinishedUnits.read(), &finishedUnits.read() };
	for (int iList = 0; iList < 3; iList++)
	{
		for (size_t i = 0; i < lists[iList]->size(); i++)
		{
			const SUnitStats& unit = (*lists[iList])[i];
			if (!unit.pUnit || unit.pUnit->getDomainType() != DOMAIN_SEA)
				continue;

			int iUntracked = TacticalAIHelpers::CountVisibleAircraftCargo(unit.pUnit, eTeam) - countCargoAircraft(unit.iUnitID, true);
			if (iUntracked > 0)
				gUntrackedCargo[unit.iUnitID] = iUntracked;
		}
	}
}

// the aircraft carried by a ship sail along when the ship is assigned to a new plot
void CvTacticalPosition::moveCargoWithCarrier(int iCarrierID, int iToPlotIndex)
{
	vector<SUnitStats>& availableUnits_w = availableUnits.write();
	for (size_t i = 0; i < availableUnits_w.size(); i++)
		if (availableUnits_w[i].eMoveStrategy == MS_AIR && availableUnits_w[i].iCarrierID == iCarrierID)
			availableUnits_w[i].iPlotIndex = iToPlotIndex;
}

// the visible enemy aircraft in a captured city, or carried by a ship the plan sank, have lost their base and no longer intercept
void CvTacticalPosition::markAircraftBasesLost(const CvPlot* pPlot, const CvUnit* pKilledShip)
{
	const CvPlot* pBasePlot = pKilledShip ? pKilledShip->plot() : pPlot;
	if (!pBasePlot)
		return;

	TeamTypes eTeam = GET_PLAYER(ePlayer).getTeam();
	for (int i = 0; i < pBasePlot->getNumUnits(); i++)
	{
		const CvUnit* pAircraft = pBasePlot->getUnitByIndex(i);
		if (!pAircraft || pAircraft->getDomainType() != DOMAIN_AIR || pAircraft->isDelayedDeath() || pAircraft->isInvisible(eTeam, false, false))
			continue;
		if (!GET_PLAYER(ePlayer).IsAtWarWith(pAircraft->getOwner()))
			continue;
		if (pKilledShip && pAircraft->getTransportUnit() != pKilledShip)
			continue;

		markInterceptorBaseLost(pAircraft->GetID());
	}
}

// a kill on the plot, ranged or melee, called before the enemy leaves the plot: a captured city loses its aircraft,
// and a sunk ship takes its cargo down with it, also in a city that holds
void CvTacticalPosition::markBasesLostForKill(int iPlotIndex, bool bCityCaptured)
{
	const CvPlot* pPlot = GC.getMap().plotByIndexUnchecked(iPlotIndex);
	if (bCityCaptured && pPlot->isCity())
	{
		markAircraftBasesLost(pPlot, NULL);
		return;
	}

	const CvTacticalPlot* pTactPlot = getTactPlot(iPlotIndex);
	const CvUnit* pKilledUnit = pTactPlot ? pTactPlot->getEnemyUnit() : NULL;
	if (pKilledUnit && pKilledUnit->getDomainType() == DOMAIN_SEA)
		markAircraftBasesLost(NULL, pKilledUnit);
}

// two damage records are the same if they hold the same value for every unit
static bool DamageRecordsEqual(const SUnitIDValueContainer& lhs, const SUnitIDValueContainer& rhs)
{
	vector<pair<int, int> > lhsValues;
	vector<pair<int, int> > rhsValues;
	for (SUnitIDValueContainer::const_iterator it = lhs.begin(); it != lhs.end(); ++it)
		if ((*it).second != 0)
			lhsValues.push_back(*it);
	for (SUnitIDValueContainer::const_iterator it = rhs.begin(); it != rhs.end(); ++it)
		if ((*it).second != 0)
			rhsValues.push_back(*it);

	std::sort(lhsValues.begin(), lhsValues.end());
	std::sort(rhsValues.begin(), rhsValues.end());
	return lhsValues == rhsValues;
}

// two positions with matching assignments are only equivalent if the sweeps left the same interceptors, and the same damage, behind
bool CvTacticalPosition::airStateEquals(const CvTacticalPosition& other) const
{
	if (!(interceptorStates.read() == other.interceptorStates.read()))
		return false;

	return DamageRecordsEqual(unitDamageDealt.read(), other.unitDamageDealt.read())
		&& DamageRecordsEqual(uncertainDamage.read(), other.uncertainDamage.read())
		&& DamageRecordsEqual(uncertainKillChance.read(), other.uncertainKillChance.read());
}

// Vox Deorum: what an aircraft has left after a strike or sweep from its base. Attacks end the turn for aircraft that cannot
// move after attacking, unless they have another attack, and each attack costs a move.
static int GetAirRemainingMovesAfterAttack(const SUnitStats& unit)
{
	if (AttackEndsTurn(unit.pUnit, unit.iAttacksLeft))
		return 0;

	if (unit.pUnit->IsFreeAttackMoves())
		return unit.iMovesLeft;

	return unit.iMovesLeft - min(unit.iMovesLeft, (int)GD_INT_GET(MOVE_DENOMINATOR));
}

// Vox Deorum: an aircraft strike from its base, scored into the given row. ScoreAttackDamage weights the damage once by the
// chance the strike lands past the interceptors the position has left. A strike an interceptor may stop stays A_RANGEATTACK,
// even when it would kill, and scores its expected damage only.
static void ScorePlotForAirStrike(const SUnitStats& unit, const CvTacticalPlot* assumedUnitPlot, const CvTacticalPlot* enemyPlot, const CvTacticalPosition& assumedPosition, STacticalAssignment& result)
{
	result.init(unit.iPlotIndex, enemyPlot->getPlotIndex(), unit.iUnitID, unit.iMovesLeft, unit.eMoveStrategy, A_RANGEATTACK, GetPrevPlotScore(unit.iUnitID, assumedPosition));

	// missiles need a unit to hit, a city without a garrison is not a target
	if (unit.pUnit->AI_getUnitAIType() == UNITAI_MISSILE_AIR && !enemyPlot->getEnemyUnit())
		return;

	ScoreAttackDamage(enemyPlot, unit.pUnit, assumedUnitPlot, assumedPosition, gTactPosStorage.getAttackCache(), &result, unit.iSelfDamage);
	if (!result.IsAcceptable())
		return;

	result.iRemainingMoves = GetAirRemainingMovesAfterAttack(unit);

	//a slight boost for attacking the "real" target
	if (enemyPlot->getPlotIndex() == assumedPosition.getTarget()->GetPlotIndex())
		result.AddScore(0, 2, 0);

	//a missile whose base is about to be lost would be wasted, so firing it is a gain of its own
	if (unit.bBaseEndangered)
		result.AddScore(0, TACTSIM_ENDANGERED_MISSILE_BONUS, 0);
}

// Vox Deorum: the score of an air strike from the assumed plot in the given position, or 0 if it is impossible or a loss.
// It is the strike scorer's own result, missile rule included, in a row of its own.
static int GetAirStrikeValue(const SUnitStats& unit, const CvTacticalPlot* unitPlot, const CvTacticalPlot* targetPlot, const CvTacticalPosition& position)
{
	STacticalAssignment strike;
	ScorePlotForAirStrike(unit, unitPlot, targetPlot, position, strike);
	return strike.IsAcceptable() ? max(0, strike.Score()) : 0;
}

// Vox Deorum: GetAirStrikeValue, remembered for the position a search is expanding. ScoreAirOptions also fills the memo with
// the strikes it scores there. Other positions are scored directly.
static int GetAirStrikeValueInExpansion(const SUnitStats& unit, const CvTacticalPlot* unitPlot, const CvTacticalPlot* targetPlot, const CvTacticalPosition& position)
{
	if (&position != gAirExpansion)
		return GetAirStrikeValue(unit, unitPlot, targetPlot, position);

	pair<int, int> key(unit.iUnitID, targetPlot->getPlotIndex());
	tr1::unordered_map<pair<int, int>, int, SIntPairHash>::const_iterator it = gExpansionStrikeValues.find(key);
	if (it != gExpansionStrikeValues.end())
		return it->second;

	int iValue = GetAirStrikeValue(unit, unitPlot, targetPlot, position);
	gExpansionStrikeValues[key] = iValue;
	return iValue;
}

// Vox Deorum: GetBestInterceptorInPosition for an aircraft, remembered for the position a search is expanding
static const CvUnit* GetBestInterceptorInExpansion(const SUnitStats& unit, const CvTacticalPlot* targetPlot, const CvTacticalPosition& position)
{
	if (&position != gAirExpansion)
		return TacticalAIHelpers::GetBestInterceptorInPosition(targetPlot->getPlot(), unit.pUnit, position);

	pair<int, int> key(unit.iUnitID, targetPlot->getPlotIndex());
	tr1::unordered_map<pair<int, int>, const CvUnit*, SIntPairHash>::const_iterator it = gExpansionInterceptors.find(key);
	if (it != gExpansionInterceptors.end())
		return it->second;

	const CvUnit* pInterceptor = TacticalAIHelpers::GetBestInterceptorInPosition(targetPlot->getPlot(), unit.pUnit, position);
	gExpansionInterceptors[key] = pInterceptor;
	return pInterceptor;
}

// Vox Deorum: the target plots where the swept interceptor is the one an aircraft's strike meets in the parent position,
// each with the aircraft's strike value there. Only these strikes can change when the interceptor is swept.
static void GetGuardedAirStrikes(const SUnitStats& before, const CvTacticalPlot* plotBefore, const CvUnit* pInterceptor,
	const CvTacticalPosition& parent, vector<pair<const CvTacticalPlot*, int> >& result)
{
	const vector<int>& targets = TacticalAIHelpers::GetAirTargetPlots(before.pUnit, plotBefore->getPlot(), parent);
	for (size_t j = 0; j < targets.size(); j++)
	{
		const CvTacticalPlot* targetPlot = parent.getTactPlot(targets[j]);
		if (!targetPlot || !targetPlot->isEnemy())
			continue;

		const CvUnit* pParentBest = GetBestInterceptorInExpansion(before, targetPlot, parent);
		if (!pParentBest || pParentBest != pInterceptor)
			continue;

		result.push_back(make_pair(targetPlot, GetAirStrikeValueInExpansion(before, plotBefore, targetPlot, parent)));
	}
}

// Vox Deorum: the best gain, in damage points, of an aircraft's strike once the swept interceptor is out of the way, over
// the guarded strikes. The strike is scored with the aircraft's state in the child position, which differs from the parent
// state only for the sweeper, whose attack, moves and damage change with the sweep. A child shares the parent's plots, so
// the plots work for both.
static int GetBestAirStrikeGain(const vector<pair<const CvTacticalPlot*, int> >& guarded, const SUnitStats& after, const CvTacticalPlot* plotAfter,
	const CvTacticalPosition& child)
{
	int iBestGain = 0;
	for (size_t j = 0; j < guarded.size(); j++)
		iBestGain = max(iBestGain, GetAirStrikeValue(after, plotAfter, guarded[j].first, child) - guarded[j].second);
	return iBestGain;
}

// Vox Deorum: the sweep's ranking bonus: how much the best strike of each remaining aircraft gains from it, summed. Gains are
// in damage points (the unit of iDamageDelta; RankScore scales it by 10), and cover both the higher landing chance and the
// lower interception damage. Only the strikes the swept interceptor guards can change (GetGuardedAirStrikes).
// - A group of identical aircraft counts once per available member, through its lowest-ID member other than the sweeper,
//   so the sweeper's own group mates count too.
// - The sweeper counts through its remaining attack, from its state after the sweep.
// - In the position a search is expanding, the guarded strikes are remembered per aircraft and interceptor. For a certain
//   sweep that damages only the interceptor, the gain of each aircraft other than the sweeper is remembered per damage
//   too, since nothing else in the child changes for that aircraft.
static int GetAirSweepRankBonus(const SUnitStats& sweeper, const CvUnit* pInterceptor, const STacticalAssignment* sweep, const CvTacticalPosition& parent)
{
	//the child is built only when a gain is not remembered
	CvTacticalPosition child;
	bool bChildBuilt = false;

	//a certain sweep that damages only the interceptor gives a child that its damage fixes for every other aircraft
	bool bRemember = &parent == gAirExpansion && !sweep->bUncertain && sweep->iDamagedCityId == -1;
	int iSweepDamage = 0;
	for (SUnitIDValueContainer::const_iterator it = sweep->unitDamage.begin(); it != sweep->unitDamage.end(); ++it)
	{
		if ((*it).first == pInterceptor->GetID())
			iSweepDamage += (*it).second;
		else
			bRemember = false;
	}

	int iBonus = 0;
	const vector<SUnitStats>& units = parent.getAvailableUnits();
	for (size_t i = 0; i < units.size(); i++)
	{
		const SUnitStats& other = units[i];
		if (other.eMoveStrategy != MS_AIR || other.iUnitID == sweeper.iUnitID || other.iAttacksLeft <= 0 || other.iMovesLeft <= 0)
			continue;
		if (!other.pUnit->IsCanAttackRanged())
			continue;

		//one representative per group, the lowest ID once the sweeper is left out; it counts for the whole group
		bool bRepresentative = true;
		int iGroupSize = 1;
		if (other.iAirGroup >= 0)
		{
			iGroupSize = 0;
			for (size_t k = 0; k < units.size(); k++)
			{
				if (units[k].iAirGroup != other.iAirGroup || units[k].iUnitID == sweeper.iUnitID)
					continue;
				iGroupSize++;
				if (units[k].iUnitID < other.iUnitID)
					bRepresentative = false;
			}
		}
		if (!bRepresentative)
			continue;

		const CvTacticalPlot* otherPlot = parent.getTactPlot(other.iPlotIndex);
		if (!otherPlot)
			continue;

		//the remembered gain for this damage if there is one, and the guarded strikes to score it from otherwise
		int iBestGain = -1;
		vector<pair<const CvTacticalPlot*, int> > localGuarded;
		SGuardedAirStrikes* pRemembered = NULL;
		if (bRemember)
		{
			pRemembered = &gExpansionGuardedStrikes[make_pair(other.iUnitID, pInterceptor->GetID())];
			for (size_t k = 0; k < pRemembered->gainsByDamage.size(); k++)
				if (pRemembered->gainsByDamage[k].first == iSweepDamage)
					iBestGain = pRemembered->gainsByDamage[k].second;
			if (!pRemembered->bTargetsKnown)
			{
				GetGuardedAirStrikes(other, otherPlot, pInterceptor, parent, pRemembered->targets);
				pRemembered->bTargetsKnown = true;
			}
		}
		else
			GetGuardedAirStrikes(other, otherPlot, pInterceptor, parent, localGuarded);

		if (iBestGain < 0)
		{
			const vector<pair<const CvTacticalPlot*, int> >& guarded = pRemembered ? pRemembered->targets : localGuarded;
			iBestGain = 0;
			if (!guarded.empty())
			{
				if (!bChildBuilt)
				{
					GetNextPosition(parent, sweep, child);
					bChildBuilt = true;
				}
				iBestGain = GetBestAirStrikeGain(guarded, other, otherPlot, child);
			}
			if (pRemembered)
				pRemembered->gainsByDamage.push_back(make_pair(iSweepDamage, iBestGain));
		}
		iBonus += (iBestGain * iGroupSize) / 10;
	}

	//the sweeper's own next attack, with the attack, moves and damage the sweep leaves it, if it survives the sweep
	SUnitStats sweeperAfter = sweeper;
	sweeperAfter.iAttacksLeft--;
	sweeperAfter.iMovesLeft = sweep->iRemainingMoves;
	sweeperAfter.iSelfDamage += sweep->iSelfDamage;
	if (sweeperAfter.iAttacksLeft > 0 && sweeperAfter.iMovesLeft > 0 && sweeper.pUnit->IsCanAttackRanged() &&
		sweeper.pUnit->GetCurrHitPoints() > sweeperAfter.iSelfDamage)
	{
		const CvTacticalPlot* sweeperPlot = parent.getTactPlot(sweeper.iPlotIndex);
		vector<pair<const CvTacticalPlot*, int> > guarded;
		if (sweeperPlot)
			GetGuardedAirStrikes(sweeper, sweeperPlot, pInterceptor, parent, guarded);
		if (!guarded.empty())
		{
			if (!bChildBuilt)
			{
				GetNextPosition(parent, sweep, child);
				bChildBuilt = true;
			}
			iBonus += GetBestAirStrikeGain(guarded, sweeperAfter, sweeperPlot, child) / 10;
		}
	}

	return iBonus;
}

// Vox Deorum: an air sweep against the best interceptor over the target plot, scored into the given row. The score is the
// damage the sweep trades with that interceptor and nothing else, so a sweep followed by no strike earns only its fight.
// Its ranking bonus is how much the remaining aircraft's strikes gain afterwards (GetAirSweepRankBonus). The bonus steers
// exploration but never enters a plan score.
static void ScorePlotForAirSweep(const SUnitStats& unit, const CvTacticalPlot* assumedUnitPlot, const CvTacticalPlot* targetPlot, const CvTacticalPosition& assumedPosition, STacticalAssignment& result)
{
	result.init(unit.iPlotIndex, targetPlot->getPlotIndex(), unit.iUnitID, unit.iMovesLeft, unit.eMoveStrategy, A_AIRSWEEP, GetPrevPlotScore(unit.iUnitID, assumedPosition));

	const CvUnit* pUnit = unit.pUnit;
	const CvPlot* pTarget = targetPlot->getPlot();
	if (!pUnit->canAirSweepAt(pTarget->getX(), pTarget->getY(), assumedUnitPlot->getPlot()))
		return;

	CvUnit* pInterceptor = TacticalAIHelpers::GetBestInterceptorInPosition(pTarget, pUnit, assumedPosition);
	if (!pInterceptor)
		return;

	int iDamageToInterceptor = 0;
	int iDamageToAttacker = 0;
	TacticalAIHelpers::GetSimulatedAirSweepDamage(pUnit, pInterceptor, assumedUnitPlot->getPlot(), pTarget, unit.iSelfDamage, assumedPosition.GetUnitDamage(pInterceptor->GetID()), iDamageToInterceptor, iDamageToAttacker);

	result.iSelfDamage = iDamageToAttacker;
	if (iDamageToInterceptor > 0)
		result.unitDamage.ChangeValue(pInterceptor->GetID(), iDamageToInterceptor);

	result.iRemainingMoves = GetAirRemainingMovesAfterAttack(unit);
	result.SetScore(0, 0, iDamageToInterceptor - iDamageToAttacker);

	int iRankBonus = GetAirSweepRankBonus(unit, pInterceptor, &result, assumedPosition);
	if (iRankBonus > 0)
		result.SetRankBonus(iRankBonus);
}

// Vox Deorum: aircraft act from their base and never occupy a plot, so every plot is neutral for them. Staying
// is the stand-down option (A_FINISH_TEMP), with no plot score to gain or lose.
STacticalAssignment* ScorePlotForAirUnitMove(const SUnitStats& unit, const CvTacticalPlot* testPlot, const CvTacticalPosition& assumedPosition, eUnitMoveEvalMode evalMode)
{
	STacticalAssignment* result = gAssignmentStorage.peekNext();
	result->init(unit.iPlotIndex, testPlot->getPlotIndex(), unit.iUnitID, unit.iMovesLeft, unit.eMoveStrategy, A_FINISH_TEMP, GetPrevPlotScore(unit.iUnitID, assumedPosition));
	if (evalMode == EM_INITIAL)
		result->eAssignmentType = A_INITIAL;
	else if (evalMode == EM_FINAL)
		result->eAssignmentType = A_FINISH;

	result->SetScore(0, 0, 0);
	return result;
}

// Vox Deorum: orders air options by their ranking value, best first
static bool RankScoreDescending(const STacticalAssignment& lhs, const STacticalAssignment& rhs)
{
	return lhs.RankScore() > rhs.RankScore();
}

// the damage to a unit from strikes an interceptor may stop
int CvTacticalPosition::GetUncertainUnitDamage(int iUnitID) const
{
	return uncertainDamage.read().GetValue(iUnitID);
}

// the damage to a city from strikes an interceptor may stop
int CvTacticalPosition::GetUncertainCityDamage(int iCityID) const
{
	return uncertainDamage.read().GetValue(-iCityID);
}

// adds damage to a unit from a strike an interceptor may stop
void CvTacticalPosition::ChangeUncertainUnitDamage(int iUnitID, int iChange)
{
	uncertainDamage.write().ChangeValue(iUnitID, iChange);
}

// adds damage to a city from a strike an interceptor may stop
void CvTacticalPosition::ChangeUncertainCityDamage(int iCityID, int iChange)
{
	uncertainDamage.write().ChangeValue(-iCityID, iChange);
}

// the chance in 1/10000 that strikes an interceptor may stop have already killed the unit
int CvTacticalPosition::GetUncertainKillChance(int iUnitID) const
{
	return uncertainKillChance.read().GetValue(iUnitID);
}

// adds a stand-down row for every remaining member of an aircraft group, in unit ID order. Each row is valid by itself
// under the leader rule, because the member before it has left the available units.
void CvTacticalPosition::standDownAirGroup(int iGroup)
{
	if (iGroup < 0)
		return;

	vector<int> memberIds;
	const vector<SUnitStats>& units = availableUnits.read();
	for (size_t i = 0; i < units.size(); i++)
		if (units[i].eMoveStrategy == MS_AIR && units[i].iAirGroup == iGroup)
			memberIds.push_back(units[i].iUnitID);
	std::sort(memberIds.begin(), memberIds.end());

	for (size_t i = 0; i < memberIds.size(); i++)
	{
		const SUnitStats* pStats = getAvailableUnitStats(memberIds[i]);
		if (!pStats)
			continue;

		STacticalAssignment standDown;
		standDown.init(pStats->iPlotIndex, pStats->iPlotIndex, pStats->iUnitID, pStats->iMovesLeft, MS_AIR, A_FINISH_TEMP, GetPrevPlotScore(pStats->iUnitID, *this));
		standDown.SetScore(0, 0, 0);
		addAssignment(standDown);
	}
}

// the aircraft group a row stands down, read before the unit leaves the available units, or -1
static int GetStandDownAirGroup(const CvTacticalPosition& position, const STacticalAssignment* pRow)
{
	if (!MOD_VOX_TACTICAL_AIR_SEARCH || !pRow || pRow->eMoveType != MS_AIR || pRow->eAssignmentType != A_FINISH_TEMP)
		return -1;

	const SUnitStats* pStats = position.getAvailableUnitStats(pRow->iUnitID);
	return pStats ? pStats->iAirGroup : -1;
}

// adds the assignment of a combo move and its optional second one, as addAssignment does. When both are added and either
// stands down the leader of a group of interchangeable aircraft, the rest of that group stands down after them in unit ID
// order, so search never tries their permutations. Without the air search this is addAssignment alone.
void CvTacticalPosition::addAssignmentsWithAirGroups(const STacticalAssignment& first, const STacticalAssignment* pSecond, AddAssignmentResult& resultFirst, AddAssignmentResult& resultSecond)
{
	int iGroupFirst = GetStandDownAirGroup(*this, &first);
	int iGroupSecond = GetStandDownAirGroup(*this, pSecond);

	resultFirst = addAssignment(first);
	resultSecond = pSecond ? addAssignment(*pSecond) : RESULT_NOOP;

	if (resultFirst == RESULT_NOT_ADDED || resultSecond == RESULT_NOT_ADDED)
		return;

	standDownAirGroup(iGroupFirst);
	if (iGroupSecond != iGroupFirst)
		standDownAirGroup(iGroupSecond);
}

// Vox Deorum: scores an aircraft's strikes and sweeps from the assumed base into gAirOptions. The acceptable options that
// rank above zero fill the first rows, and the return value counts them. When asked, also gives the rank of the best
// acceptable strike at any rank, INT_MIN for none.
static size_t ScoreAirOptions(const SUnitStats& unit, const CvUnit* pUnit, const CvTacticalPlot* assumedUnitPlot, const CvTacticalPosition& position, int* piBestStrikeRank)
{
	if (piBestStrikeRank)
		*piBestStrikeRank = INT_MIN;

	if (unit.iAttacksLeft <= 0 || unit.iMovesLeft <= 0)
		return 0;

	const CvPlot* pBasePlot = assumedUnitPlot->getPlot();
	size_t nOptions = 0;

	//strikes
	if (pUnit->IsCanAttackRanged())
	{
		const vector<int>& targets = TacticalAIHelpers::GetAirTargetPlots(pUnit, pBasePlot, position);
		for (size_t i = 0; i < targets.size(); i++)
		{
			const CvTacticalPlot* enemyPlot = position.getTactPlot(targets[i]);
			if (!enemyPlot || !enemyPlot->isEnemy())
				continue;

			if (gAirOptions.size() <= nOptions)
				gAirOptions.resize(nOptions + 1);
			STacticalAssignment& strike = gAirOptions[nOptions];
			ScorePlotForAirStrike(unit, assumedUnitPlot, enemyPlot, position, strike);
			//the sweeps ranked in this expansion compare against this same value (GetAirStrikeValueInExpansion)
			if (&position == gAirExpansion)
				gExpansionStrikeValues[make_pair(unit.iUnitID, targets[i])] = strike.IsAcceptable() ? max(0, strike.Score()) : 0;
			if (piBestStrikeRank && strike.IsAcceptable())
				*piBestStrikeRank = max(*piBestStrikeRank, strike.RankScore());
			if (strike.RankScore() > 0 && strike.IsAcceptable())
				nOptions++;
		}
	}

	//sweeps, one for each distinct interceptor that covers an enemy plot in range, over the plot nearest the real target
	if (pUnit->canAirSweep())
	{
		vector<pair<int, const CvUnit*> > sweepPlots = TacticalAIHelpers::GetAirSweepPlots(pUnit, pBasePlot, position);
		for (size_t i = 0; i < sweepPlots.size(); i++)
		{
			const CvTacticalPlot* sweepPlot = position.getTactPlot(sweepPlots[i].first);
			if (!sweepPlot)
				continue;

			if (gAirOptions.size() <= nOptions)
				gAirOptions.resize(nOptions + 1);
			STacticalAssignment& sweep = gAirOptions[nOptions];
			ScorePlotForAirSweep(unit, assumedUnitPlot, sweepPlot, position, sweep);
			if (sweep.RankScore() > 0 && sweep.IsAcceptable())
				nOptions++;
		}
	}

	return nOptions;
}

// Vox Deorum: the IDs of the visible interceptors near any target the aircraft can strike or sweep from its base, sorted
// and without repeats
static vector<int> GetAirGroupInterceptors(const SUnitStats& unit, const CvTacticalPosition& position)
{
	vector<int> result;
	const CvTacticalPlot* basePlot = position.getTactPlot(unit.iPlotIndex);
	if (!unit.pUnit || !basePlot)
		return result;

	const PlotIndexContainer& enemies = position.getEnemyPlots();
	for (PlotIndexContainer::const_iterator it = enemies.begin(); it != enemies.end(); ++it)
	{
		const CvPlot* pTarget = GC.getMap().plotByIndexUnchecked(*it);
		if (!TacticalAIHelpers::CanAirUnitActAgainstPlot(unit.pUnit, basePlot->getPlot(), pTarget))
			continue;

		const vector<SInterceptorCandidate>& candidates = GetInterceptorCandidates(pTarget, unit.pUnit->getOwner());
		for (size_t i = 0; i < candidates.size(); i++)
			result.push_back(candidates[i].pUnit->GetID());
	}

	std::sort(result.begin(), result.end());
	result.erase(std::unique(result.begin(), result.end()), result.end());
	return result;
}

// Vox Deorum: true while an idle group's interceptors are as the search found them: none has spent an interception, lost
// its base or taken damage. Its strikes and sweeps then score as they did when the search started, so it only stands down.
static bool IsAirGroupIdle(const SUnitStats& unit, const CvTacticalPosition& position)
{
	if (unit.iAirGroup < 0)
		return false;

	map<int, vector<int> >::const_iterator itIdle = gIdleAirGroups.find(unit.iAirGroup);
	if (itIdle == gIdleAirGroups.end())
		return false;

	const vector<int>& interceptors = itIdle->second;
	for (size_t i = 0; i < interceptors.size(); i++)
		if (position.getInterceptionsUsed(interceptors[i]) > 0 || position.isInterceptorBaseLost(interceptors[i]) || position.GetUnitDamage(interceptors[i]) > 0)
			return false;

	return true;
}

// Strikes, sweeps and standing down for an aircraft, into gPossibleMoves. Aircraft act from their base at full air range; a
// group of interchangeable aircraft acts through its leader only. Every ranking here uses RankScore, so a sweep that
// protects strikes survives the cuts. piBestStrikeRank is passed to ScoreAirOptions, and stays as the caller set it when
// no option is scored: a missing unit or plot, a group member that is not the leader, a full pool, or an idle group.
void CvTacticalPosition::getPreferredAssignmentsForAirUnit(const SUnitStats& unit, int nMaxCount, int* piBestStrikeRank) const
{
	gPossibleMoves.clear();

	const CvTacticalPlot* assumedUnitPlot = getTactPlot(unit.iPlotIndex);
	CvUnit* pUnit = GET_PLAYER(getPlayer()).getUnit(unit.iUnitID);
	if (!pUnit || !assumedUnitPlot)
		return;

	//the other members of a group wait without any rows while the leader acts or stands down
	if (!isAirGroupLeader(unit.iUnitID))
		return;

	//options are scored into rows of their own and only the kept ones are copied into the shared pool, since an
	//aircraft can reach more targets than the pool holds; standing down always takes a row there
	if (!gAssignmentStorage.peekNext())
		return;

	//an idle group has nothing but standing down until the interceptors over its targets change
	size_t nOptions = IsAirGroupIdle(unit, *this) ? 0 : ScoreAirOptions(unit, pUnit, assumedUnitPlot, *this, piBestStrikeRank);

	//keep the best attacks, ties cut in option order like ranged attacks, so a group takes a bounded share of the
	//pool the later units are scored in; a row always stays free for standing down
	std::stable_sort(gAirOptions.begin(), gAirOptions.begin() + nOptions, RankScoreDescending);
	size_t nKeep = min(nOptions, (size_t)nMaxCount);
	nKeep = min(nKeep, (size_t)(gAssignmentStorage.getSizeLimit() - gAssignmentStorage.getSize() - 1));

	for (size_t i = 0; i < nKeep; i++)
	{
		STacticalAssignment* option = gAssignmentStorage.peekNext();
		*option = gAirOptions[i];
		gAssignmentStorage.consumeOne();
		gPossibleMoves.push_back(OptionWithScore<STacticalAssignment*>(option, option->RankScore()));
	}

	STacticalAssignment* standDown = ScorePlotForAirUnitMove(unit, assumedUnitPlot, *this, EM_INTERMEDIATE);
	gAssignmentStorage.consumeOne();
	gPossibleMoves.push_back(OptionWithScore<STacticalAssignment*>(standDown, standDown->RankScore()));
}

// With the air search the cap counts land and sea units and aircraft groups together, each group once, as the group
// acts through its leader. A group that can only act after its ship moves keeps that ship, and the ship's cargo
// goes if the ship cannot be kept. Reads the units in order of importance and returns the ones over the cap.
// Also records the idle groups of the search. A group is idle when its leader has no positive strike or sweep (its
// importance from dropSuperfluousUnits is not positive) and bestStrikeRanks, filled by that same pass, holds a rank below
// TACTSIM_AIR_HOPELESS_RANK for it. Sweeps need no check of their own, since a leader without a positive option has no
// positive sweep. A leader missing from bestStrikeRanks is never idle, and neither are aircraft aboard a ship, since the
// ship's moves can bring new targets in range.
vector<SUnitStats> CvTacticalPosition::getUnitsToDropWithAircraft(int iMaxUnitsToKeep, const map<int, int>& bestStrikeRanks) const
{
	const vector<SUnitStats>& availableUnits_r = availableUnits.read();

	set<int> presentIds;
	gIdleAirGroups.clear();
	for (size_t i = 0; i < availableUnits_r.size(); i++)
	{
		const SUnitStats& unit = availableUnits_r[i];
		presentIds.insert(unit.iUnitID);
		if (unit.eMoveStrategy != MS_AIR || unit.iImportanceScore > 0 || unit.iCarrierID >= 0 || !isAirGroupLeader(unit.iUnitID))
			continue;

		map<int, int>::const_iterator itRank = bestStrikeRanks.find(unit.iUnitID);
		if (itRank != bestStrikeRanks.end() && itRank->second < TACTSIM_AIR_HOPELESS_RANK)
			gIdleAirGroups[unit.iAirGroup] = GetAirGroupInterceptors(unit, *this);
	}

	//keep units in order of importance until the cap is reached
	set<int> keptIds;
	map<int, bool> groupKept;
	int iKept = 0;
	vector<SUnitStats> unitsToDrop;
	for (size_t i = 0; i < availableUnits_r.size(); i++)
	{
		const SUnitStats& unit = availableUnits_r[i];
		bool bKeep = false;

		if (unit.eMoveStrategy == MS_AIR)
		{
			map<int, bool>::const_iterator itGroup = groupKept.find(unit.iAirGroup);
			if (itGroup != groupKept.end())
				bKeep = itGroup->second;
			else
			{
				//a new group; if it depends on its ship moving, the ship has to be kept as well
				int iSlotsNeeded = 1;
				bool bShipUsable = true;
				if (unit.bNeedsCarrierMove && unit.iCarrierID >= 0)
				{
					bShipUsable = presentIds.count(unit.iCarrierID) > 0;
					if (bShipUsable && keptIds.count(unit.iCarrierID) == 0)
						iSlotsNeeded = 2;
				}

				if (bShipUsable && iKept + iSlotsNeeded <= iMaxUnitsToKeep)
				{
					bKeep = true;
					iKept += iSlotsNeeded;
					if (unit.bNeedsCarrierMove && unit.iCarrierID >= 0)
						keptIds.insert(unit.iCarrierID);
				}
				groupKept[unit.iAirGroup] = bKeep;
			}
		}
		else if (keptIds.count(unit.iUnitID) > 0)
			//a ship that was kept for its cargo
			bKeep = true;
		else if (iKept < iMaxUnitsToKeep)
		{
			bKeep = true;
			iKept++;
		}

		if (bKeep)
			keptIds.insert(unit.iUnitID);
		else
			unitsToDrop.push_back(unit);
	}

	return unitsToDrop;
}

// Vox Deorum: the air search fields of an aircraft joining the position: the ship carrying it, whether it can only act
// after that ship moves, whether it is a missile on an endangered base, and its group of interchangeable aircraft
void CvTacticalPosition::initAircraftStats(SUnitStats& unit) const
{
	const CvUnit* pShip = unit.pUnit->getTransportUnit();
	unit.iCarrierID = pShip ? pShip->GetID() : -1;

	if (pShip)
	{
		bool bActsFromBase = false;
		const PlotIndexContainer& enemies = enemyPlots.read();
		for (PlotIndexContainer::const_iterator it = enemies.begin(); it != enemies.end() && !bActsFromBase; ++it)
			bActsFromBase = TacticalAIHelpers::CanAirUnitActAgainstPlot(unit.pUnit, unit.pUnit->plot(), GC.getMap().plotByIndexUnchecked(*it));

		if (!bActsFromBase)
		{
			vector<int> vLaunchPlots = TacticalAIHelpers::GetCargoLaunchPlots(unit.pUnit, getTarget());
			for (size_t iPlot = 1; iPlot < vLaunchPlots.size() && !unit.bNeedsCarrierMove; iPlot++)
				for (PlotIndexContainer::const_iterator it = enemies.begin(); it != enemies.end() && !unit.bNeedsCarrierMove; ++it)
					unit.bNeedsCarrierMove = TacticalAIHelpers::CanAirUnitActAgainstPlot(unit.pUnit, GC.getMap().plotByIndexUnchecked(vLaunchPlots[iPlot]), GC.getMap().plotByIndexUnchecked(*it));
		}
	}

	// a missile on a base about to be lost fires first; planes can still rebase, so they get no such priority
	unit.bBaseEndangered = unit.pUnit->isSuicide() && TacticalAIHelpers::IsAirBaseEndangered(unit.pUnit);

	// same owner, type, base, promotions, hit points, moves and attacks made: the aircraft are interchangeable.
	// The base also fixes bBaseEndangered, so every member of a group shares it.
	vector<int> groupKey;
	groupKey.push_back(unit.pUnit->getOwner());
	groupKey.push_back(unit.pUnit->getUnitType());
	groupKey.push_back(unit.iCarrierID >= 0 ? -1 - unit.iCarrierID : unit.pUnit->plot()->GetPlotIndex());
	groupKey.push_back(unit.bNeedsCarrierMove ? 1 : 0);
	groupKey.push_back(unit.pUnit->GetCurrHitPoints());
	groupKey.push_back(unit.pUnit->getMoves());
	groupKey.push_back(unit.pUnit->getNumAttacksMadeThisTurn());
	for (int iPromotion = 0; iPromotion < GC.getNumPromotionInfos(); iPromotion++)
		if (unit.pUnit->isHasPromotion((PromotionTypes)iPromotion))
			groupKey.push_back(iPromotion);

	map<vector<int>, int>::const_iterator itGroup = gAirGroupKeys.find(groupKey);
	if (itGroup == gAirGroupKeys.end())
	{
		int iGroup = (int)gAirGroupKeys.size();
		gAirGroupKeys[groupKey] = iGroup;
		unit.iAirGroup = iGroup;
	}
	else
		unit.iAirGroup = itGroup->second;
}
