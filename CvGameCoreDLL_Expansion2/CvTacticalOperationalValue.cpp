/*	-------------------------------------------------------------------------------------------------------
	Vox Deorum: the value pieces the operational loop shares with the simulator's outcome grade. See
	CvTacticalOperationalValue.h.
	------------------------------------------------------------------------------------------------------- */
#include "CvGameCoreDLLPCH.h"
#include "CvTacticalOperationalValue.h"
#include "CvUnitClasses.h"
#include "LintFree.h"

// Returns power times production cost, each floored at 0.
double UnitBaseWorth(int iPower, int iProductionCost)
{
	return static_cast<double>(std::max(0, iPower)) * std::max(0, iProductionCost);
}

// Reads power and production cost from the unit entry.
double UnitBaseWorth(const CvUnitEntry& kUnitEntry)
{
	return UnitBaseWorth(kUnitEntry.GetPower(), kUnitEntry.GetProductionCost());
}

// Adds the level premium to the base worth.
double UnitWorth(double fBaseWorth, int iLevel, double fSeniorityPerLevel)
{
	return fBaseWorth + fSeniorityPerLevel * fBaseWorth * std::max(0, iLevel - 1);
}

// Scales the worth by the value floor plus the rest in proportion to the health share.
double ValueAtDamage(double fWorth, int iDamage, int iMaxHitPoints)
{
	const double fHealth = iMaxHitPoints <= 0 ? 0.0 :
		std::min(1.0, std::max(0.0, static_cast<double>(iMaxHitPoints - iDamage) / iMaxHitPoints));
	return fWorth * (unitKillShare + (1.0 - unitKillShare) * fHealth);
}

// Divides damage by maximum hit points, clamped to 0..1.
double DamageShare(int iDamage, int iMaxHitPoints)
{
	if (iMaxHitPoints <= 0)
		return 0.0;
	return std::min(1.0, std::max(0.0, static_cast<double>(iDamage) / iMaxHitPoints));
}

// Loses the whole value on a kill, otherwise the damaged share of the part above the value floor.
double UnitExposure(double fWorth, int iDamage, int iMaxHitPoints, int iIncomingDamage)
{
	if (iMaxHitPoints <= 0 || iIncomingDamage <= 0)
		return 0.0;
	if (iIncomingDamage >= iMaxHitPoints - iDamage)
		return ValueAtDamage(fWorth, iDamage, iMaxHitPoints);
	return fWorth * (1.0 - unitKillShare) * iIncomingDamage / iMaxHitPoints;
}

// Uses population when the economic value is not positive.
double CityValue(int iEconomicValue, int iPopulation)
{
	return iEconomicValue > 0 ? iEconomicValue : iPopulation;
}

// Reads the economic value the city would have for ePlayer, and its population.
double CityValue(CvCity& kCity, PlayerTypes ePlayer)
{
	return CityValue(kCity.getEconomicValue(ePlayer), kCity.getPopulation());
}

// Adds an unpillaged improvement's defense to the terrain and feature modifier; a city plot counts 100.
int PlotDefense(const CvPlot& kPlot, int iImprovement, bool bPillaged, bool bCity)
{
	if (bCity)
		return 100;
	int iModifier = kPlot.defenseModifier(NO_TEAM, true, false);
	if (iImprovement != NO_IMPROVEMENT && !bPillaged)
	{
		const CvImprovementEntry* pInfo = GC.getImprovementInfo(static_cast<ImprovementTypes>(iImprovement));
		if (pInfo)
			iModifier += std::max(0, pInfo->GetDefenseModifier());
	}
	return iModifier;
}

// Adds the defense share, the resource bonus and the water penalty to 1. A workable plot also adds its improvement,
// pillaged or not, and its bonus resource. Workable means a city plot, or a plot within the work range of its
// effective owning city.
double GroundWeight(const CvPlot& kPlot, int iImprovement, bool bPillaged, bool bCity, bool bResource, bool bBonus)
{
	double fWeight = 1.0 + PlotDefense(kPlot, iImprovement, bPillaged, bCity) / 100.0;
	if (bResource)
		fWeight += groundResourceWeight;
	if (kPlot.isWater())
		fWeight += kPlot.isShallowWater() ? groundShallowWaterWeight : groundOceanWeight;
	bool bWorkable = bCity;
	if (!bWorkable)
	{
		const CvCity* pCity = kPlot.getEffectiveOwningCity();
		bWorkable = pCity && plotDistance(kPlot.getX(), kPlot.getY(), pCity->getX(), pCity->getY()) <= pCity->getWorkPlotDistance();
	}
	if (!bWorkable)
		return fWeight;
	if (iImprovement != NO_IMPROVEMENT)
	{
		const CvImprovementEntry* pInfo = GC.getImprovementInfo(static_cast<ImprovementTypes>(iImprovement));
		fWeight += pInfo && pInfo->IsCreatedByGreatPerson() ? groundGreatPersonImprovementWeight : groundImprovementWeight;
	}
	if (bBonus)
		fWeight += groundBonusResourceWeight;
	return fWeight;
}

// Applies the exponential curve around neutral 50.
double FlavorWeight(double fBase, double fSlope, int iFlavor, bool bReversed)
{
	const double fOffset = bReversed ? 50.0 - iFlavor : iFlavor - 50.0;
	return fBase * pow(2.0, fSlope * fOffset / 50.0);
}

// Floors the total at fFloorCount typical items.
double ReferenceScale(double fTotal, double fTypical, double fFloorCount)
{
	return std::max(fTotal, fFloorCount * fTypical);
}

// Adds the per-era cost for each era after the first and scales by the game speed.
double UnitRuleCost(const CvUnitEntry& kUnitEntry, int iEra)
{
	if (kUnitEntry.GetProductionCost() <= 0)
		return 0.0;
	double fCost = kUnitEntry.GetProductionCost();
	if (kUnitEntry.GetProductionCostPerEra() != 0 && iEra - 1 > 0)
		fCost += kUnitEntry.GetProductionCostPerEra() * (iEra - 1);
	return fCost * GC.getGame().getGameSpeedInfo().getTrainPercent() / 100.0;
}

// Follows GetPurchaseCostFromProduction and the unit steps of GetPurchaseCost, without rounding.
double UnitRulePrice(const CvUnitEntry& kUnitEntry, int iEra)
{
	const double fCost = UnitRuleCost(kUnitEntry, iEra);
	const int iModifier = kUnitEntry.GetHurryCostModifier();
	if (fCost <= 0.0 || iModifier == -1)
		return 0.0;
	double fPrice = pow(fCost * /*30*/ GD_INT_GET(GOLD_PURCHASE_GOLD_PER_PRODUCTION),
		static_cast<double>(/*0.75 in CP, 0.68 in VP*/ GD_FLOAT_GET(HURRY_GOLD_PRODUCTION_EXPONENT)));
	fPrice *= GC.getGame().getGameSpeedInfo().getHurryPercent() / 100.0;
	fPrice *= std::max(0, 100 + iModifier) / 100.0;
	if (MOD_BALANCE_PURCHASE_COST_ADJUSTMENTS)
		fPrice *= 0.8;
	return fPrice;
}

// Walks the unit classes in order, keeps the player's unlocked military types, then sorts and deduplicates them.
// Each class maps through the civilization's unit table as GetSpecificUnitType does, but without its policy class
// replacement, so adopting a policy cannot swap a class's type for a policy unit that the filter below drops.
void ReferenceUnitTypes(const CvPlayer& kPlayer, std::vector<UnitTypes>& aeTypes)
{
	aeTypes.clear();
	const CvTeam& kTeam = GET_TEAM(kPlayer.getTeam());
	const int iEra = kPlayer.GetCurrentEra();
	const CvCivilizationInfo* pCivilization = GC.getCivilizationInfo(kPlayer.getCivilizationType());
	for (int iClass = 0; iClass < GC.getNumUnitClassInfos(); iClass++)
	{
		const CvUnitClassInfo* pClass = GC.getUnitClassInfo(static_cast<UnitClassTypes>(iClass));
		if (!pClass)
			continue;
		const UnitTypes eUnit = static_cast<UnitTypes>(pCivilization ? pCivilization->getCivilizationUnits(iClass) : pClass->getDefaultUnitIndex());
		const CvUnitEntry* pInfo = eUnit == NO_UNIT ? NULL : GC.getUnitInfo(eUnit);
		if (!pInfo || (pInfo->GetCombat() <= 0 && pInfo->GetRangedCombat() <= 0))
			continue;
		if (pInfo->IsMinorCivGift() || pInfo->GetPolicyType() != NO_POLICY)
			continue;
		const int iPrereq = pInfo->GetPrereqAndTech();
		if (iPrereq != NO_TECH && !kTeam.GetTeamTechs()->HasTech(static_cast<TechTypes>(iPrereq)))
			continue;
		const int iObsolete = pInfo->GetObsoleteTech();
		if (iObsolete != NO_TECH && kTeam.GetTeamTechs()->HasTech(static_cast<TechTypes>(iObsolete)))
			continue;
		if (UnitRuleCost(*pInfo, iEra) > 0.0)
			aeTypes.push_back(eUnit);
	}
	std::sort(aeTypes.begin(), aeTypes.end());
	aeTypes.erase(std::unique(aeTypes.begin(), aeTypes.end()), aeTypes.end());
}

// Sums base worth over all reference types for the mean, and price and base worth over the priced ones for the rate.
void ReferenceUnitScales(const CvPlayer& kPlayer, double& fTypicalWorth, double& fGoldPerWorth)
{
	fTypicalWorth = 0.0;
	fGoldPerWorth = 0.0;
	std::vector<UnitTypes> aeTypes;
	ReferenceUnitTypes(kPlayer, aeTypes);
	const int iEra = kPlayer.GetCurrentEra();
	double fWorthTotal = 0.0;
	double fPricedWorth = 0.0;
	double fPriceTotal = 0.0;
	for (size_t i = 0; i < aeTypes.size(); i++)
	{
		const CvUnitEntry* pInfo = GC.getUnitInfo(aeTypes[i]);
		if (!pInfo)
			continue;
		const double fWorth = UnitBaseWorth(pInfo->GetPower(), pInfo->GetProductionCost());
		fWorthTotal += fWorth;
		const double fPrice = UnitRulePrice(*pInfo, iEra);
		if (fPrice > 0.0)
		{
			fPriceTotal += fPrice;
			fPricedWorth += fWorth;
		}
	}
	if (!aeTypes.empty())
		fTypicalWorth = fWorthTotal / aeTypes.size();
	if (fPricedWorth > 0.0)
		fGoldPerWorth = fPriceTotal / fPricedWorth;
}

// Credits the discounted gain of the upgrade, never a loss.
double UpgradePremium(double fBaseNow, double fBaseTarget, double fDiscount)
{
	return fDiscount * std::max(0.0, fBaseTarget - fBaseNow);
}

// Weighs each unit at groundUnitWeight and adds the land.
double GroundBase(unsigned int iUnits, double fLand)
{
	return groundUnitWeight * iUnits + fLand;
}

// Gives our plot and a hostile city their whole weight, other hostile land the share of its weight above the floor
// (never below 0), and every other plot 0.
double GroundStake(bool bOurs, bool bHostile, bool bCity, double fWeight)
{
	if (bOurs || (bHostile && bCity))
		return fWeight;
	return bHostile ? groundCreditShare * std::max(0.0, fWeight - groundCreditFloor) : 0.0;
}

// Adds the plot's loss to fLost or its credit to fCredit. The two sums stay apart so callers keep one summing order.
void AddGroundPlot(bool bOurs, bool bHostile, bool bCity, double fWeight, unsigned int iStand, unsigned int iNearby,
	double& fLost, double& fCredit)
{
	const double fStake = GroundStake(bOurs, bHostile, bCity, fWeight);
	if (bOurs)
	{
		if ((iStand & groundHoldsHostile) != 0)
			fLost += fStake;
		else if ((iNearby & groundHoldsHostile) != 0 && (iStand & (groundHoldsOurs | groundHoldsNeutral)) == 0)
			fLost += groundBlockadeShare * fStake;
	}
	else if (bHostile)
	{
		if ((iStand & groundHoldsOurs) != 0)
			fCredit += fStake;
		else if ((iNearby & groundHoldsOurs) != 0 && (iStand & (groundHoldsHostile | groundHoldsNeutral)) == 0)
			fCredit += groundBlockadeShare * fStake;
	}
}
