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
