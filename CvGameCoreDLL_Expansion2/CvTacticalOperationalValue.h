/*	-------------------------------------------------------------------------------------------------------
	Vox Deorum: the value pieces the operational loop (VOX_TACTICAL_OPERATIONAL_LOOP) shares with the
	simulator's outcome grade: unit worth and value at damage, exposure, city value, plot defense and ground
	weight, the flavor weight curve, the typical unit scale Z_T, the reference unit types with their rule cost
	and rule price, the upgrade premium, and the per-plot ground score. The grade calls the extracted
	copy of these functions, so the loop's priorities and the grade measure units, cities and ground the same way.
	The pure functions take numbers; the adapters read them from native types.
	------------------------------------------------------------------------------------------------------- */
#pragma once

#ifndef CIV5_TACTICAL_OPERATIONAL_VALUE_H
#define CIV5_TACTICAL_OPERATIONAL_VALUE_H

class CvUnitEntry;
class CvCity;
class CvPlot;
class CvPlayer;

// Vox Deorum: returns our expected damage roll in units of 1/200 at the mapped risk.
int OurRollPer200(int iRisk);
// Vox Deorum: returns the enemy's expected damage roll in units of 1/200 at the mapped risk.
int EnemyRollPer200(int iRisk);

// The value floor k: the share of its worth a unit keeps at 0 HP, so it is lost only when the unit dies or is captured.
// Damage loses the rest in proportion to the health lost. Changing it changes the grade and bumps its version.
const double unitKillShare = 0.6;
// The level premium per level above the first, as a share of the unit's base worth.
const double gradeSeniorityPerLevel = 0.1;
// The ground weight parts GroundWeight adds to 1 and the plot defense share. Changing one changes the grade and the
// local token's ground_weight.
const double groundResourceWeight = 1.0;
const double groundOceanWeight = -0.25;
const double groundShallowWaterWeight = -0.2;
const double groundImprovementWeight = 0.1;
const double groundGreatPersonImprovementWeight = 0.25;
const double groundBonusResourceWeight = 0.15;
// The share of a plot's weight lost, or credited, when it is merely blockaded instead of occupied.
const double groundBlockadeShare = 0.5;
// A hostile plot without a city credits groundCreditShare times its weight above groundCreditFloor, so plain hostile
// land credits nothing. A hostile city credits its whole weight.
const double groundCreditShare = 0.5;
const double groundCreditFloor = 1.0;
// The share of a ready upgrade's value gain a unit is credited with before it upgrades.
const double gradeUpgradeDiscount = 0.5;
// The side bits of who holds a plot: our side (ours or friendly), the hostile side, or a neutral.
const unsigned int groundHoldsOurs = 1U;
const unsigned int groundHoldsHostile = 2U;
const unsigned int groundHoldsNeutral = 4U;

// Returns base worth B: the geometric mean of power and production cost, each floored at 0.
double UnitBaseWorth(int iPower, int iProductionCost);
// Returns base worth for the unit entry.
double UnitBaseWorth(const CvUnitEntry& kUnitEntry);
// Returns a unit's worth F = B + S, where the level premium S adds fSeniorityPerLevel x B per level above the first.
double UnitWorth(double fBaseWorth, int iLevel, double fSeniorityPerLevel);
// Returns a living unit's value: its worth times k + (1 - k) x its health share, with health clamped to 0..1.
double ValueAtDamage(double fWorth, int iDamage, int iMaxHitPoints);
// Vox Deorum: readiness gives partial damage credit for the total rejected by a target's reach check.
double ScoredDamage(double fRealized, int iRejected);
// Returns damage over maximum hit points, clamped to 0..1, or 0 without hit points.
double DamageShare(double fDamage, int iMaxHitPoints);
// Returns what a unit loses to iIncomingDamage more damage: its whole value when that reaches its HP left,
// otherwise worth x (1 - k) x the incoming damage over maximum HP.
double UnitExposure(double fWorth, int iDamage, int iMaxHitPoints, int iIncomingDamage);
// Vox Deorum: scores health damage up to the target's remaining HP; only realized damage can earn the kill share.
double DestroyedValue(double fWorth, double fRemaining, double fMaximum, double fRealized, int iRejected = 0);
// Vox Deorum: returns L^k / (L^k + T^k), or 1 when enemy strength T is 0.
double StrengthShare(double fLocal, double fEnemy, double fSteepness);
// Vox Deorum: returns city contest: 1 - (1 - pressure) * (1 - damage level).
double TheaterContest(double fPressure, double fDamageLevel);
// Vox Deorum: returns the theater floor plus the contested share of the room above it.
double TheaterMultiplier(double fFloor, double fImportance, double fContest);
// Vox Deorum: returns the rise in strength share after help arrives.
double StrengthSwing(double fShare, double fProjectedShare);
// Returns a city's value: its economic value, or its population when that is not positive.
double CityValue(int iEconomicValue, int iPopulation);
// Returns a city's value as ePlayer would see it on capture.
double CityValue(CvCity& kCity, PlayerTypes ePlayer);
// Returns a plot's defense in percent for whoever stands on it: 100 on a city plot, otherwise the terrain and
// feature modifier plus the defense of an improvement that is not pillaged. The value is not clamped.
int PlotDefense(const CvPlot& kPlot, int iImprovement, bool bPillaged, bool bCity);
// Returns a plot's ground weight 1 + d + r + water, plus its improvement and bonus resource when the plot is
// workable. d is PlotDefense over 100, not clamped. r is groundResourceWeight for a revealed strategic or luxury
// resource (bResource). water is groundOceanWeight on ocean and groundShallowWaterWeight on coast and lakes. A
// workable plot is a city plot or one within its effective owning city's work range; it adds
// groundImprovementWeight for any improvement, pillaged or not (groundGreatPersonImprovementWeight instead for a
// great person improvement), and groundBonusResourceWeight for a revealed bonus resource (bBonus).
double GroundWeight(const CvPlot& kPlot, int iImprovement, bool bPillaged, bool bCity, bool bResource, bool bBonus);
// Returns the flavor weight curve base x 2^(slope x (flavor - 50) / 50). Reversed, it uses 50 - flavor, so a bold
// player weighs its own losses less. Flavor 50 returns the base exactly.
double FlavorWeight(double fBase, double fSlope, int iFlavor, bool bReversed);
// Returns a unit type's rule production cost at an era: the static cost, plus the cost per era for each era after
// the first, times the game speed train percent. Instance, built-count, trait and difficulty terms are left out.
// Returns 0 when the static cost is 0 or less.
double UnitRuleCost(const CvUnitEntry& kUnitEntry, int iEra);
// Returns a unit type's rule price in gold at an era: the rule cost through the native gold purchase curve
// (CvCity::GetPurchaseCostFromProduction and CvCity::GetPurchaseCost) with the game speed hurry percent, the type's
// hurry cost modifier and the flat purchase cost cut, but no player, city or policy modifier. Returns 0 for a type
// without a rule cost or one that cannot be bought (hurry cost modifier -1).
double UnitRulePrice(const CvUnitEntry& kUnitEntry, int iEra);
// Fills the player's reference military types: for every unit class, the civilization's own unit type (before policy
// class replacements) when it has combat
// or ranged combat, its prerequisite tech is known, its obsolete tech is not, and it has a rule cost. City-state gift
// units and policy-unlocked units are left out, so policies do not move the list. Supply, gold, cities and resources
// do not count. The list is sorted and holds each type once.
void ReferenceUnitTypes(const CvPlayer& kPlayer, std::vector<UnitTypes>& aeTypes);
// Sets the typical unit worth (the mean base worth of the reference types) and the gold per unit of worth (the sum of
// their rule prices over the sum of their base worths, among the types with a price). Each is 0 without a basis.
void ReferenceUnitScales(const CvPlayer& kPlayer, double& fTypicalWorth, double& fGoldPerWorth);
// Returns the share fDiscount of a ready upgrade's base worth gain, or 0 when the upgrade is worth less.
double UpgradePremium(double fBaseNow, double fBaseTarget, double fDiscount);
// Returns the most one plot can move the ground score, by its owner when watching started: its whole weight for our
// plot (bOurs) and for a hostile plot with a city (bHostile, bCity), groundCreditShare times the weight above
// groundCreditFloor for other hostile land, never below 0, and 0 for every other plot. Water lowers the weight below
// the floor, so hostile water and plain hostile land have no stake.
double GroundStake(bool bOurs, bool bHostile, bool bCity, double fWeight);
// Adds one plot to the ground score's two sums. Our plot (bOurs) loses its stake when a hostile holder stands on it,
// and groundBlockadeShare of it when a hostile unit next to it blockades it and no holder of our side or a neutral
// stands on it. A hostile plot (bHostile) credits its stake when our side stands on it, and groundBlockadeShare of it
// when our side blockades it and no hostile or neutral holder stands on it. A city holds its own plot, so a hostile
// city is credited only once our side takes it. iStand holds the side bits standing on the plot, iNearby the side bits
// blockading from its neighbors.
void AddGroundPlot(bool bOurs, bool bHostile, bool bCity, double fWeight, unsigned int iStand, unsigned int iNearby,
	double& fLost, double& fCredit);

#endif //CIV5_TACTICAL_OPERATIONAL_VALUE_H
