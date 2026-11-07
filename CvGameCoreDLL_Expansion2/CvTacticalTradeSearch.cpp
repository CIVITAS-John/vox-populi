/*	-------------------------------------------------------------------------------------------------------
	Vox Deorum: the trade search (VOX_TACTICAL_TRADE_SEARCH). This file holds:
	- the next steps of every trade route, rebuilt only when a route changes (TacticalAIHelpers::UpdateTradeSteps);
	- the routes, threats and gold flavor weight of one search, set at its root (TacticalAIHelpers::SetSearchTradeRoutes);
	- the plunder, defender, guard and health rules that decide who takes or holds a route's step;
	- the position's trade score (CvTacticalPosition::computeTradeScore) and the rank bonus a move's trade gain gives.
	CvTacticalSearchShared.h declares what CvTacticalAI.cpp uses from here and what this file uses from there.
	------------------------------------------------------------------------------------------------------- */
#include "CvGameCoreDLLPCH.h"
#include "CvTacticalAI.h"
#include "CvTacticalSearchShared.h"
#include "CvDangerPlots.h"
#include "CvGameCoreUtils.h"
#include "LintFree.h"

// Vox Deorum: a plan's trade score, in plot score units, before the gold flavor and the unit's health scale it (see
// CvTacticalPosition::computeTradeScore). Intercept: an enemy route one of our units takes when its trade unit steps onto that
// unit's plot in the owner's next trade movement. Guard: an own route whose every endangered step one of our units holds.
// Both sit above the small end plot preferences (stay together, defense, city distance) and below one tier of the
// enemy distance table, so they decide between nearby end plots without pulling units out of their line.
const int TACTICAL_TRADE_INTERCEPT_BONUS = 15;
const int TACTICAL_TRADE_GUARD_BONUS = 10;

// Vox Deorum: the next steps of the trade routes the trade score reads, rebuilt when a route changed
STradeSteps gTradeSteps;
// Vox Deorum: the gTradeSteps slots of the routes this search can intercept or guard, and the gold flavor weight (1000 is
// neutral) that scales its trade score, both set at the root (TacticalAIHelpers::SetSearchTradeRoutes)
vector<int> gSearchTradeRoutes;
int gTradeScoreWeight = 1000;
// Vox Deorum: the IDs, sorted, of other players' units whose death can change this search's trade score: those that can move
// onto a step of our routes, and those standing on a step of an enemy route
vector<int> gTradeScoreUnits;
// Vox Deorum: for each next step of our routes in this search, the known enemy units that can move onto it
// (CvDangerPlots::GetPossibleCapturers) and would plunder there (CanPlunderTradeStep), read once at the root
tr1::unordered_map<int, vector<const CvUnit*> > gTradeStepThreats;

// Vox Deorum: whether the unit would be invisible to the team on the plot. CvUnit::isInvisible judges the unit's current plot;
// this reads the detection of the given plot instead, as getVisibleEnemyDefender sees the unit once it stands there.
static bool IsInvisibleAtPlot(const CvUnit* pUnit, const CvPlot* pPlot, TeamTypes eTeam)
{
	if (pUnit->getTeam() == eTeam)
		return false;
	if (pUnit->IsHiddenByNearbyUnit(pPlot) && !pPlot->isInvisibleVisibleUnit(eTeam))
		return true;
	InvisibleTypes eInvisible = pUnit->getInvisibleType();
	return eInvisible != NO_INVISIBLE && !pPlot->isInvisibleVisible(eTeam, eInvisible);
}

// Vox Deorum: whether no unit may plunder a trade route on the plot (canPlunderTradeRoute's ocean rule)
static bool IsPlunderForbiddenAt(const CvPlot* pPlot)
{
	return MOD_GLOBAL_NO_OCEAN_PLUNDERING && pPlot->isWater() && !pPlot->isShallowWater();
}

// Vox Deorum: whether the unit, ending its turn on the plot, plunders a trade route of the given team when the route's trade unit
// steps there. CvGameTrade::StepUnit plunders through getVisibleEnemyDefender and canPlunderTradeRoute; the latter also needs the
// trade unit on the plot already, so only its rules for the plunderer apply here.
static bool CanPlunderTradeStep(const CvUnit* pUnit, const CvPlot* pPlot, TeamTypes eRouteTeam)
{
	if (!pUnit->IsCombatUnit() || !pUnit->IsCanDefend() || IsInvisibleAtPlot(pUnit, pPlot, eRouteTeam))
		return false;

	bool bEmbarked = (pPlot == pUnit->plot()) ? pUnit->isEmbarked() : pPlot->needsEmbarkation(pUnit);
	if (bEmbarked)
		return false;

	if (IsPlunderForbiddenAt(pPlot))
		return false;

	return GET_TEAM(pUnit->getTeam()).isAtWar(eRouteTeam);
}

// Vox Deorum: whether trade movement can meet the unit on the plot as the route's defender (CvPlot::getVisibleEnemyDefender):
// a unit the route's team sees there that can defend and whose combat owner is at war with that team
static bool IsTradeStepDefender(const CvUnit* pUnit, const CvPlot* pPlot, TeamTypes eRouteTeam)
{
	if (pUnit->isDelayedDeath() || !pUnit->IsCanDefend() || IsInvisibleAtPlot(pUnit, pPlot, eRouteTeam))
		return false;
	return GET_TEAM(GET_PLAYER(pUnit->getCombatOwner(eRouteTeam, *pPlot)).getTeam()).isAtWar(eRouteTeam);
}

// Vox Deorum: keeps pCandidate as the step's defender when it is one and comes before the current one in the plot's unit order
static void KeepFirstTradeStepDefender(const CvUnit*& pDefender, const CvUnit* pCandidate, const CvPlot* pPlot, TeamTypes eRouteTeam)
{
	if (pCandidate && IsTradeStepDefender(pCandidate, pPlot, eRouteTeam) &&(!pDefender || isBeforeUnitCycle(pCandidate, pDefender)))
		pDefender = pCandidate;
}

// Vox Deorum: whether the tactical plot lists the unit. A unit the unit cap dropped stays listed where it started, so loops over
// the real plot's units skip listed units to count each unit once.
static bool IsListedOnTactPlot(const CvTacticalPlot* pTactPlot, int iUnitID)
{
	if (!pTactPlot)
		return false;
	const vector<STacticalUnit>& vUnits = pTactPlot->getUnitsAtPlot();
	for (size_t i = 0; i < vUnits.size(); i++)
		if (vUnits[i].iUnitID == iUnitID)
			return true;
	return false;
}

// Vox Deorum: the unit that takes a route of the given team when its trade unit steps onto the plot, or NULL. Trade movement
// meets only the plot's first defender in unit order (isBeforeUnitCycle), and the route survives when that one cannot plunder.
// Units of the search stand where the position puts them, pUnit (which may be NULL) on the plot only with bUnitHere; every
// other unit, of any player, stays where it is unless the position kills it. Other players' units count only where we see them.
static const CvUnit* GetTradeStepTaker(int iPlotIndex, TeamTypes eRouteTeam, const CvUnit* pUnit, bool bUnitHere, const CvTacticalPosition& assumedPosition)
{
	const CvPlot* pPlot = GC.getMap().plotByIndexUnchecked(iPlotIndex);
	PlayerTypes ePlayer = assumedPosition.getPlayer();
	const CvPlayer& kPlayer = GET_PLAYER(ePlayer);
	const CvUnit* pDefender = NULL;

	if (bUnitHere)
		KeepFirstTradeStepDefender(pDefender, pUnit, pPlot, eRouteTeam);

	const CvTacticalPlot* pTactPlot = assumedPosition.getTactPlot(iPlotIndex);
	if (pTactPlot)
	{
		const vector<STacticalUnit>& vUnits = pTactPlot->getUnitsAtPlot();
		for (size_t i = 0; i < vUnits.size(); i++)
			if (!pUnit || vUnits[i].iUnitID != pUnit->GetID())
				KeepFirstTradeStepDefender(pDefender, kPlayer.getUnit(vUnits[i].iUnitID), pPlot, eRouteTeam);
	}

	for (int i = 0; i < pPlot->getNumUnits(); i++)
	{
		const CvUnit* pOtherUnit = pPlot->getUnitByIndex(i);
		if (!pOtherUnit || pOtherUnit == pUnit)
			continue;
		if (pOtherUnit->getOwner() == ePlayer && (assumedPosition.GetUnitStats(pOtherUnit->GetID()) || IsListedOnTactPlot(pTactPlot, pOtherUnit->GetID())))
			continue;
		//other players' units count only where we can see them
		if (pOtherUnit->getOwner() != ePlayer && (!pPlot->isVisible(kPlayer.getTeam()) || pOtherUnit->isInvisible(kPlayer.getTeam(), false)))
			continue;
		if (GET_TEAM(kPlayer.getTeam()).isAtWar(pOtherUnit->getTeam()) &&
			assumedPosition.GetUnitDamageDealt().GetValue(pOtherUnit->GetID()) >= pOtherUnit->GetCurrHitPoints())
			continue;
		KeepFirstTradeStepDefender(pDefender, pOtherUnit, pPlot, eRouteTeam);
	}

	return (pDefender && CanPlunderTradeStep(pDefender, pPlot, eRouteTeam)) ? pDefender : NULL;
}

// Vox Deorum: whether a known enemy combat unit, melee or ranged, could end its turn on the plot of our route's step and plunder
// it there, and the position does not kill it. The units that can move onto each step come from the danger map at the root
// (gTradeStepThreats, see TacticalAIHelpers::SetSearchTradeRoutes).
static bool IsTradeStepThreatened(const CvPlot* pPlot, const CvTacticalPosition& assumedPosition)
{
	tr1::unordered_map<int, vector<const CvUnit*> >::const_iterator itThreats = gTradeStepThreats.find(pPlot->GetPlotIndex());
	if (itThreats == gTradeStepThreats.end())
		return false;
	const vector<const CvUnit*>& vThreats = itThreats->second;
	for (size_t i = 0; i < vThreats.size(); i++)
		if (assumedPosition.GetUnitDamageDealt().GetValue(vThreats[i]->GetID()) < vThreats[i]->GetCurrHitPoints())
			return true;
	return false;
}

// Vox Deorum: whether the position captures a city whose capture cancels the route (CvGameTrade::ClearAllCityTradeRoutes with
// transits, called from CvCity::PreKill). A melee kill on a city plot is its capture.
static bool IsTradeRouteCut(const STradeStepRoute& route, const CvTacticalPosition& assumedPosition)
{
	const vector<STacticalAssignment>& vAssignments = assumedPosition.getAssignments();
	for (size_t i = 0; i < vAssignments.size(); i++)
	{
		if (vAssignments[i].eAssignmentType != A_MELEEKILL)
			continue;
		if (std::find(route.vCutPlots.begin(), route.vCutPlots.end(), vAssignments[i].iToPlotIndex) != route.vCutPlots.end())
			return true;
	}
	return false;
}

// Vox Deorum: a unit's share of its full trade score, in thousandths: the square of its share of hit points after the damage the
// position predicts for it, so a hurt unit, for which holding a trade step is riskier, gets much less
static int GetTradeHealthPermille(const CvUnit* pUnit, const CvTacticalPosition& assumedPosition)
{
	int iMaxHP = max(1, pUnit->GetMaxHitPoints());
	const SUnitStats* pStats = assumedPosition.GetUnitStats(pUnit->GetID());
	int iHP = range(pUnit->GetCurrHitPoints() - (pStats ? pStats->iSelfDamage : 0), 0, iMaxHP);
	return (1000 * iHP / iMaxHP) * iHP / iMaxHP;
}

// Vox Deorum: whether an enemy that ends its turn on this step of our route would plunder it: a known enemy combat unit can
// move onto the plot (IsTradeStepThreatened) and plunder is allowed there. Moves that would capture a city are left out
// (SetSearchTradeRoutes), since the capture ends the route anyway, so a city step is endangered only by units that can enter
// it without a fight.
static bool IsOwnTradeStepEndangered(const CvPlot* pPlot, const CvTacticalPosition& assumedPosition)
{
	return !IsPlunderForbiddenAt(pPlot) && IsTradeStepThreatened(pPlot, assumedPosition);
}

// Vox Deorum: whether the unit holds the step against the threats there: a combat unit that is not embarked on the plot, which
// every threat the position leaves alive sees there. A threat that cannot see it may share its plot
// (CvUnit::canCoexistWithEnemyUnit) and plunder the route anyway, and an embarked unit cannot defend.
static bool IsTradeStepGuard(const CvUnit* pGuard, const CvPlot* pPlot, const CvTacticalPosition& assumedPosition)
{
	if (!pGuard->IsCombatUnit() || pGuard->getDomainType() == DOMAIN_AIR)
		return false;
	bool bEmbarked = (pPlot == pGuard->plot()) ? pGuard->isEmbarked() : pPlot->needsEmbarkation(pGuard);
	if (bEmbarked)
		return false;

	tr1::unordered_map<int, vector<const CvUnit*> >::const_iterator itThreats = gTradeStepThreats.find(pPlot->GetPlotIndex());
	if (itThreats == gTradeStepThreats.end())
		return true;
	const vector<const CvUnit*>& vThreats = itThreats->second;
	for (size_t i = 0; i < vThreats.size(); i++)
		if (assumedPosition.GetUnitDamageDealt().GetValue(vThreats[i]->GetID()) < vThreats[i]->GetCurrHitPoints() &&
			IsInvisibleAtPlot(pGuard, pPlot, vThreats[i]->getTeam()))
			return false;
	return true;
}

// Vox Deorum: how well our units hold a step against an enemy that would end its turn there, in thousandths: the health share
// (GetTradeHealthPermille) of our healthiest unit on the plot that holds it (IsTradeStepGuard), or 0 without one. Units of the search stand where the position puts them; our other units stay where they are.
static int GetTradeStepGuardPermille(int iPlotIndex, const CvTacticalPosition& assumedPosition)
{
	const CvPlot* pPlot = GC.getMap().plotByIndexUnchecked(iPlotIndex);
	PlayerTypes ePlayer = assumedPosition.getPlayer();
	const CvPlayer& kPlayer = GET_PLAYER(ePlayer);
	int iBest = 0;

	const CvTacticalPlot* pTactPlot = assumedPosition.getTactPlot(iPlotIndex);
	if (pTactPlot)
	{
		const vector<STacticalUnit>& vUnits = pTactPlot->getUnitsAtPlot();
		for (size_t i = 0; i < vUnits.size(); i++)
		{
			const CvUnit* pUnit = kPlayer.getUnit(vUnits[i].iUnitID);
			if (pUnit && vUnits[i].eMoveType != MS_AIR && IsTradeStepGuard(pUnit, pPlot, assumedPosition))
				iBest = max(iBest, GetTradeHealthPermille(pUnit, assumedPosition));
		}
	}

	for (int i = 0; i < pPlot->getNumUnits(); i++)
	{
		const CvUnit* pOtherUnit = pPlot->getUnitByIndex(i);
		if (pOtherUnit && pOtherUnit->getOwner() == ePlayer && !assumedPosition.GetUnitStats(pOtherUnit->GetID()) &&
			!IsListedOnTactPlot(pTactPlot, pOtherUnit->GetID()) && IsTradeStepGuard(pOtherUnit, pPlot, assumedPosition))
			iBest = max(iBest, GetTradeHealthPermille(pOtherUnit, assumedPosition));
	}
	return iBest;
}

// Vox Deorum: one route's trade score for a position, before the gold flavor weight (see CvTacticalPosition::computeTradeScore).
// A route the position cuts by a capture scores nothing.
// - Our own route: an enemy that ends its turn on any endangered step plunders it, so the route is as safe as its least held
//   endangered step (GetTradeStepGuardPermille). It scores minus the guard bonus when that step is not held at all, and nothing
//   when every endangered step is held at full health or none is endangered, for example after the threat is killed. A step
//   outside the search keeps its root state in every plan, so a route that an unheld step there dooms stays doomed, and the
//   threats are read at the root, so plots the search reveals change nothing.
// - An enemy route: the unit native trade movement meets first on the earliest step that has one takes it (GetTradeStepTaker).
//   It scores the intercept bonus, scaled by the taker's health, when the taker is ours.
static int GetTradeRouteScore(const STradeStepRoute& route, const CvTacticalPosition& assumedPosition)
{
	if (IsTradeRouteCut(route, assumedPosition))
		return 0;

	if (route.eOwner == assumedPosition.getPlayer())
	{
		int iHeld = 1000;
		for (size_t i = 0; i < route.vStepPlots.size() && iHeld > 0; i++)
			if (IsOwnTradeStepEndangered(GC.getMap().plotByIndexUnchecked(route.vStepPlots[i]), assumedPosition))
				iHeld = min(iHeld, GetTradeStepGuardPermille(route.vStepPlots[i], assumedPosition));
		return -(TACTICAL_TRADE_GUARD_BONUS * (1000 - iHeld)) / 1000;
	}

	for (size_t i = 0; i < route.vStepPlots.size(); i++)
	{
		const CvUnit* pTaker = GetTradeStepTaker(route.vStepPlots[i], route.eTeam, NULL, false, assumedPosition);
		if (pTaker)
			return pTaker->getOwner() == assumedPosition.getPlayer() ? (TACTICAL_TRADE_INTERCEPT_BONUS * GetTradeHealthPermille(pTaker, assumedPosition)) / 1000 : 0;
	}
	return 0;
}

// Vox Deorum: the trade score one unit earns alone on the plot, for ordering its moves; plans keep the position's trade score
// (CvTacticalPosition::computeTradeScore). It counts the intercept bonus for each route of the search whose trade unit steps here
// and that this unit takes, unless a unit takes it on an earlier step, and the guard bonus once when the plot is an endangered
// step of our own route that this unit would hold (IsTradeStepGuard), all scaled by the unit's health. It leaves out what the unit takes from others and whether a route's
// other endangered steps are held.
static int GetTradeStepScore(const CvUnit* pUnit, const CvTacticalPlot* testPlot, const CvTacticalPosition& assumedPosition)
{
	if (gSearchTradeRoutes.empty())
		return 0;
	tr1::unordered_map<int, vector<pair<int, int> > >::const_iterator itSteps = gTradeSteps.stepsByPlot.find(testPlot->getPlotIndex());
	if (itSteps == gTradeSteps.stepsByPlot.end())
		return 0;

	const CvPlot* pTestPlot = testPlot->getPlot();
	int iScore = 0;
	bool bGuard = false;
	for (size_t i = 0; i < itSteps->second.size(); i++)
	{
		int iRouteSlot = itSteps->second[i].first;
		int iStep = itSteps->second[i].second;
		if (std::find(gSearchTradeRoutes.begin(), gSearchTradeRoutes.end(), iRouteSlot) == gSearchTradeRoutes.end())
			continue;
		const STradeStepRoute& route = gTradeSteps.vRoutes[iRouteSlot];
		if (IsTradeRouteCut(route, assumedPosition))
			continue;

		if (route.eOwner == assumedPosition.getPlayer())
		{
			if (!bGuard && IsOwnTradeStepEndangered(pTestPlot, assumedPosition) && IsTradeStepGuard(pUnit, pTestPlot, assumedPosition))
			{
				bGuard = true;
				iScore += TACTICAL_TRADE_GUARD_BONUS;
			}
			continue;
		}

		if (!CanPlunderTradeStep(pUnit, pTestPlot, route.eTeam))
			continue;
		bool bTaken = false;
		for (int iEarlierStep = 0; iEarlierStep < iStep && !bTaken; iEarlierStep++)
			bTaken = GetTradeStepTaker(route.vStepPlots[iEarlierStep], route.eTeam, pUnit, false, assumedPosition) != NULL;
		if (!bTaken)
			bTaken = GetTradeStepTaker(testPlot->getPlotIndex(), route.eTeam, pUnit, true, assumedPosition) != pUnit;
		if (!bTaken)
			iScore += TACTICAL_TRADE_INTERCEPT_BONUS;
	}

	return iScore > 0 ? (iScore * GetTradeHealthPermille(pUnit, assumedPosition)) / 1000 : 0;
}

// Vox Deorum: the rank bonus for a unit's trade gain from one plot to another, in damage points (a tenth of plot score units):
// its trade score (GetTradeStepScore) where it ends, in the position after the move, less its score where it starts
int GetTradeRankBonus(const CvUnit* pUnit, const CvTacticalPlot* pToPlot, const CvTacticalPosition& toPosition,
	const CvTacticalPlot* pFromPlot, const CvTacticalPosition& fromPosition)
{
	if (gSearchTradeRoutes.empty())
		return 0;
	int iGain = GetTradeStepScore(pUnit, pToPlot, toPosition);
	if (pFromPlot)
		iGain -= GetTradeStepScore(pUnit, pFromPlot, fromPosition);
	iGain = CvTacticalPosition::scaleByWeight(gTradeScoreWeight, iGain);
	return (iGain >= 0 ? iGain + 5 : iGain - 5) / 10;
}

// Vox Deorum: whether an assignment damages a unit whose death can change the search's trade score (gTradeScoreUnits)
bool DamagesTradeScoreUnit(const SUnitIDValueContainer& unitDamage)
{
	for (SUnitIDValueContainer::const_iterator it = unitDamage.begin(); it != unitDamage.end(); ++it)
		if (std::binary_search(gTradeScoreUnits.begin(), gTradeScoreUnits.end(), (*it).first))
			return true;
	return false;
}

// Vox Deorum: the position's trade score, in plot score units: the sum of GetTradeRouteScore over the search's routes
// (gSearchTradeRoutes), scaled by the gold flavor weight (gTradeScoreWeight)
int CvTacticalPosition::computeTradeScore() const
{
	int iScore = 0;
	for (size_t i = 0; i < gSearchTradeRoutes.size(); i++)
		iScore += GetTradeRouteScore(gTradeSteps.vRoutes[gSearchTradeRoutes[i]], *this);
	return scaleByWeight(gTradeScoreWeight, iScore);
}

// Vox Deorum: sets the root's trade score, which every plan's trade change is measured from
void CvTacticalPosition::initTradeScore()
{
	iTradeScore = computeTradeScore();
}

// Vox Deorum: refreshes gTradeSteps with the plots each trade unit steps onto in its owner's next trade movement, as
// CvGameTrade::MoveUnit moves it when nothing plunders it, for every route a corporation does not protect. The steps do not
// depend on the searching player (SetSearchTradeRoutes checks ownership, war and visibility), so they are rebuilt only when the
// trade connections changed since the last update: a route started, ended, moved, was recalled or lost its protection. With
// VOX_TACTICAL_TRADE_SEARCH off it stays empty, so searches have no trade score.
void TacticalAIHelpers::UpdateTradeSteps()
{
	if (!MOD_VOX_TACTICAL_TRADE_SEARCH)
	{
		gTradeSteps.clear();
		return;
	}

	CvGameTrade* pTrade = GC.getGame().GetGameTrade();
	vector<int> vSignature;
	vector<size_t> vConnections;
	for (size_t iConnection = 0; iConnection < pTrade->GetNumTradeConnections(); iConnection++)
	{
		const TradeConnection& kConnection = pTrade->GetTradeConnection(iConnection);
		if (!kConnection.isValid() || kConnection.m_aPlotList.empty())
			continue;

		bool bCorporationInvulnerable = false;
		CorporationTypes eCorporation = GET_PLAYER(kConnection.m_eOriginOwner).GetCorporations()->GetFoundedCorporation();
		if (eCorporation != NO_CORPORATION)
		{
			CvCorporationEntry* pkCorporation = GC.getCorporationInfo(eCorporation);
			if (pkCorporation && pkCorporation->IsTradeRoutesInvulnerable())
				bCorporationInvulnerable = true;
		}
		if (bCorporationInvulnerable)
			continue;

		const CvUnit* pTradeUnit = GET_PLAYER(kConnection.m_eOriginOwner).getUnit(kConnection.m_unitID);
		if (!pTradeUnit || !pTradeUnit->plot())
			continue;

		vConnections.push_back(iConnection);
		vSignature.push_back(kConnection.m_iID);
		vSignature.push_back(kConnection.m_unitID);
		vSignature.push_back(GC.getMap().plotNum(kConnection.m_iOriginX, kConnection.m_iOriginY));
		vSignature.push_back(GC.getMap().plotNum(kConnection.m_iDestX, kConnection.m_iDestY));
		vSignature.push_back((int)kConnection.m_aPlotList.size());
		vSignature.push_back(kConnection.m_iTradeUnitLocationIndex * 2 + (kConnection.m_bTradeUnitMovingForward ? 1 : 0));
		vSignature.push_back(kConnection.m_iCircuitsCompleted);
		vSignature.push_back(kConnection.m_iCircuitsToComplete);
		vSignature.push_back(kConnection.GetMovementSpeed());
	}
	//the end marker gives a game without routes a signature too, so it is not rebuilt for every search
	vSignature.push_back(-1);
	if (vSignature == gTradeSteps.vSignature)
		return;

	gTradeSteps.clear();
	gTradeSteps.vSignature.swap(vSignature);
	for (size_t i = 0; i < vConnections.size(); i++)
	{
		const TradeConnection& kConnection = pTrade->GetTradeConnection(vConnections[i]);

		STradeStepRoute route;
		route.eOwner = kConnection.m_eOriginOwner;
		route.eTeam = GET_PLAYER(kConnection.m_eOriginOwner).getTeam();
		route.eDomain = kConnection.m_eDomain;
		route.iTradeUnitID = kConnection.m_unitID;
		route.vCutPlots.push_back(GC.getMap().plotNum(kConnection.m_iOriginX, kConnection.m_iOriginY));
		route.vCutPlots.push_back(GC.getMap().plotNum(kConnection.m_iDestX, kConnection.m_iDestY));
		//capturing a city a sea route passes through ends the route too
		if (kConnection.m_eDomain == DOMAIN_SEA)
		{
			for (size_t iPathPlot = 0; iPathPlot < kConnection.m_aPlotList.size(); iPathPlot++)
			{
				const CvPlot* pPathPlot = GC.getMap().plot(kConnection.m_aPlotList[iPathPlot].m_iX, kConnection.m_aPlotList[iPathPlot].m_iY);
				if (pPathPlot && pPathPlot->isCity())
					route.vCutPlots.push_back(pPathPlot->GetPlotIndex());
			}
		}

		int iLocationIndex = kConnection.m_iTradeUnitLocationIndex;
		bool bMovingForward = kConnection.m_bTradeUnitMovingForward;
		int iCircuitsCompleted = kConnection.m_iCircuitsCompleted;
		int iMoves = kConnection.GetMovementSpeed();
		for (int iMove = 0; iMove < iMoves && iCircuitsCompleted < kConnection.m_iCircuitsToComplete; iMove++)
		{
			if (CvGameTrade::StepTradeUnitLocation(iLocationIndex, bMovingForward, (int)kConnection.m_aPlotList.size()))
				iCircuitsCompleted++;

			const CvPlot* pStepPlot = GC.getMap().plot(kConnection.m_aPlotList[iLocationIndex].m_iX, kConnection.m_aPlotList[iLocationIndex].m_iY);
			if (!pStepPlot)
				break;
			route.vStepPlots.push_back(pStepPlot->GetPlotIndex());
		}
		if (route.vStepPlots.empty())
			continue;

		int iRouteSlot = (int)gTradeSteps.vRoutes.size();
		gTradeSteps.vRoutes.push_back(STradeStepRoute());
		std::swap(gTradeSteps.vRoutes.back(), route);
		const vector<int>& vStepPlots = gTradeSteps.vRoutes.back().vStepPlots;
		for (size_t iStep = 0; iStep < vStepPlots.size(); iStep++)
		{
			int iPlotIndex = vStepPlots[iStep];
			//a turnaround can bring the trade unit back to a plot, where the first step plunders it
			if (std::find(vStepPlots.begin(), vStepPlots.begin() + iStep, iPlotIndex) == vStepPlots.begin() + iStep)
				gTradeSteps.stepsByPlot[iPlotIndex].push_back(make_pair(iRouteSlot, (int)iStep));
		}
	}
}

// Vox Deorum: sets the routes the search's trade score covers (gSearchTradeRoutes): our own routes, and the routes of teams at
// war with ours whose trade unit we see, that have a next step on one of the root's tactical plots, the enemy units that can move
// onto our steps (gTradeStepThreats), and the other players' units whose death can change the score (gTradeScoreUnits). The
// danger map must be current. With any route, it also sets the
// weight the player's FLAVOR_GOLD gives the trade score (gTradeScoreWeight): half at 0, neutral at 50, double at 100. Then it
// sets the root's trade score.
void TacticalAIHelpers::SetSearchTradeRoutes(CvTacticalPosition* pRoot, PlayerTypes ePlayer)
{
	gSearchTradeRoutes.clear();
	gTradeScoreWeight = 1000;
	TeamTypes eTeam = GET_PLAYER(ePlayer).getTeam();
	for (size_t iRoute = 0; iRoute < gTradeSteps.vRoutes.size(); iRoute++)
	{
		const STradeStepRoute& route = gTradeSteps.vRoutes[iRoute];
		if (route.eOwner != ePlayer)
		{
			if (!GET_TEAM(eTeam).isAtWar(route.eTeam))
				continue;
			const CvUnit* pTradeUnit = GET_PLAYER(route.eOwner).getUnit(route.iTradeUnitID);
			if (!pTradeUnit || !pTradeUnit->plot() || !pTradeUnit->plot()->isVisible(eTeam) || pTradeUnit->isInvisible(eTeam, false))
				continue;
		}
		for (size_t iStep = 0; iStep < route.vStepPlots.size(); iStep++)
		{
			if (pRoot->getTactPlot(route.vStepPlots[iStep]))
			{
				gSearchTradeRoutes.push_back((int)iRoute);
				break;
			}
		}
	}

	//the threats to our steps, and the other players' units whose death can change the trade score
	gTradeScoreUnits.clear();
	gTradeStepThreats.clear();
	const CvDangerPlots* pDangerPlots = GET_PLAYER(ePlayer).GetDangerPlots();
	for (size_t i = 0; i < gSearchTradeRoutes.size(); i++)
	{
		const STradeStepRoute& route = gTradeSteps.vRoutes[gSearchTradeRoutes[i]];
		for (size_t iStep = 0; iStep < route.vStepPlots.size(); iStep++)
		{
			const CvPlot* pStepPlot = GC.getMap().plotByIndexUnchecked(route.vStepPlots[iStep]);
			if (route.eOwner == ePlayer)
			{
				if (!pDangerPlots || gTradeStepThreats.count(pStepPlot->GetPlotIndex()))
					continue;
				vector<const CvUnit*>& vThreats = gTradeStepThreats[pStepPlot->GetPlotIndex()];
				vector<CvUnit*> vMovers = pDangerPlots->GetPossibleCapturers(*pStepPlot);
				for (size_t j = 0; j < vMovers.size(); j++)
				{
					//entering a city the unit is at war with captures it, which ends the route anyway
					if (pStepPlot->isEnemyCity(*vMovers[j]) || !CanPlunderTradeStep(vMovers[j], pStepPlot, eTeam))
						continue;
					vThreats.push_back(vMovers[j]);
					gTradeScoreUnits.push_back(vMovers[j]->GetID());
				}
			}
			else
			{
				for (int j = 0; j < pStepPlot->getNumUnits(); j++)
				{
					const CvUnit* pStepUnit = pStepPlot->getUnitByIndex(j);
					if (pStepUnit && pStepUnit->getOwner() != ePlayer)
						gTradeScoreUnits.push_back(pStepUnit->GetID());
				}
			}
		}
	}
	std::sort(gTradeScoreUnits.begin(), gTradeScoreUnits.end());
	gTradeScoreUnits.erase(std::unique(gTradeScoreUnits.begin(), gTradeScoreUnits.end()), gTradeScoreUnits.end());

	if (!gSearchTradeRoutes.empty())
	{
		FlavorTypes eGold = (FlavorTypes)GC.getInfoTypeForString("FLAVOR_GOLD", true);
		if (eGold != NO_FLAVOR)
		{
			int iBase = 50, iDelta = 0;
			GetGeneralFlavorTerms(GET_PLAYER(ePlayer).GetFlavorManager(), eGold, 50, iBase, iDelta);
			int iGold = CvFlavorManager::ShiftFlavor(iBase, iDelta);
			if (iGold != 50)
				gTradeScoreWeight = range((int)(1000.0 * pow(2.0, (iGold - 50) / 50.0) + 0.5), 1, 65535);
		}
	}
	pRoot->initTradeScore();
}
