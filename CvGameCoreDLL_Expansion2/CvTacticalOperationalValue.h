/*	-------------------------------------------------------------------------------------------------------
	Vox Deorum: the value pieces the operational loop (VOX_TACTICAL_OPERATIONAL_LOOP) shares with the
	simulator's outcome grade: unit worth and value at damage, exposure, city value, plot defense and ground
	weight, and the flavor weight curve. The grade calls the extracted copy of these functions, so the loop's
	priorities and the grade measure units, cities and ground the same way. The pure functions take numbers;
	the adapters read them from native types.
	------------------------------------------------------------------------------------------------------- */
#pragma once

#ifndef CIV5_TACTICAL_OPERATIONAL_VALUE_H
#define CIV5_TACTICAL_OPERATIONAL_VALUE_H

class CvUnitEntry;
class CvCity;
class CvPlot;

// The value floor k: the share of its worth a unit keeps at 0 HP, so it is lost only when the unit dies or is captured.
// Damage loses the rest in proportion to the health lost. Changing it changes the grade and bumps its version.
const double unitKillShare = 0.6;
// The level premium per level above the first, as a share of the unit's base worth.
const double gradeSeniorityPerLevel = 0.1;

// Returns a unit type's base worth B: its power times its production cost, each floored at 0.
double UnitBaseWorth(int iPower, int iProductionCost);
// Returns the base worth of a unit type from its entry.
double UnitBaseWorth(const CvUnitEntry& kUnitEntry);
// Returns a unit's worth F = B + S, where the level premium S adds fSeniorityPerLevel x B per level above the first.
double UnitWorth(double fBaseWorth, int iLevel, double fSeniorityPerLevel);
// Returns a living unit's value: its worth times k + (1 - k) x its health share, with health clamped to 0..1.
double ValueAtDamage(double fWorth, int iDamage, int iMaxHitPoints);
// Returns damage over maximum hit points, clamped to 0..1, or 0 without hit points.
double DamageShare(int iDamage, int iMaxHitPoints);
// Returns what a unit loses to iIncomingDamage more damage: its whole value when that reaches its HP left,
// otherwise worth x (1 - k) x the incoming damage over maximum HP.
double UnitExposure(double fWorth, int iDamage, int iMaxHitPoints, int iIncomingDamage);
// Returns a city's value: its economic value, or its population when that is not positive.
double CityValue(int iEconomicValue, int iPopulation);
// Returns a city's value as ePlayer would see it on capture.
double CityValue(CvCity& kCity, PlayerTypes ePlayer);
// Returns a plot's defense in percent for whoever stands on it: 100 on a city plot, otherwise the terrain and
// feature modifier plus the defense of an improvement that is not pillaged. The value is not clamped.
int PlotDefense(const CvPlot& kPlot, int iImprovement, bool bPillaged, bool bCity);
// Returns a plot's ground weight 1 + d + s: d is the defense in percent clamped to 0..100 over 100, and s is 1 for
// a revealed strategic or luxury resource.
double GroundWeight(int iDefensePercent, bool bResource);
// Returns the flavor weight curve base x 2^(slope x (flavor - 50) / 50). Reversed, it uses 50 - flavor, so a bold
// player weighs its own losses less. Flavor 50 returns the base exactly.
double FlavorWeight(double fBase, double fSlope, int iFlavor, bool bReversed);

#endif //CIV5_TACTICAL_OPERATIONAL_VALUE_H
