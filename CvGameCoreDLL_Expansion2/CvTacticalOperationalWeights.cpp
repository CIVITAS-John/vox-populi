/*	-------------------------------------------------------------------------------------------------------
	The operational loop's own constants. See CvTacticalOperationalWeights.h. Edit a column and
	rebuild to change what the game runs; the simulator can override any row by name.
	------------------------------------------------------------------------------------------------------- */
#include "CvGameCoreDLLPCH.h"
#include "CvTacticalAI.h"
#include "CvTacticalOperationalWeights.h"
#include "CvTacticalOperationalLoop.h"
#include "CvTacticalOperationalValue.h"
#include "LintFree.h"

// One row per weight: base value, the flavor that scales it, and its direction. A common factor on all five does not
// change the order, so the attrition weight stays at 1 as the reference.
const OperationalWeight OPERATIONAL_WEIGHT[] =
{
	//  name                   base   flavor                           direction
	{ "weight.attrition",      1.0,   STacticalFlavors::ATTRITION,     FLAVOR_SCALE_UP },
	{ "weight.occupation",     1.0,   STacticalFlavors::OCCUPATION,    FLAVOR_SCALE_UP },
	{ "weight.holdCity",       1.0,   STacticalFlavors::HOLD_CITY,     FLAVOR_SCALE_UP },
	{ "weight.holdGround",     0.05,  STacticalFlavors::HOLD_GROUND,   FLAVOR_SCALE_UP },
	{ "weight.risk",           1.0,   STacticalFlavors::RISK,          FLAVOR_SCALE_DOWN },
};
typedef char OperationalWeightRowsMatchWeights[(sizeof(OPERATIONAL_WEIGHT) / sizeof(OPERATIONAL_WEIGHT[0]) == NUM_OPERATIONAL_WEIGHTS) ? 1 : -1];

// Flavor slope per military flavor slot, in slot order. 1 matches the grade.
const double OPERATIONAL_FLAVOR_SLOPE[] = { 1.0, 1.0, 1.0, 1.0, 1.0 };
typedef char OperationalSlopesMatchSlots[(sizeof(OPERATIONAL_FLAVOR_SLOPE) / sizeof(OPERATIONAL_FLAVOR_SLOPE[0]) == STacticalFlavors::NUM_SLOTS) ? 1 : -1];

// The slope names, in slot order.
const char* const OPERATIONAL_FLAVOR_SLOPE_NAMES[] =
{
	"slope.RISK", "slope.OCCUPATION", "slope.ATTRITION", "slope.HOLD_CITY", "slope.HOLD_GROUND",
};
typedef char OperationalSlopeNamesMatchSlots[(sizeof(OPERATIONAL_FLAVOR_SLOPE_NAMES) / sizeof(OPERATIONAL_FLAVOR_SLOPE_NAMES[0]) == STacticalFlavors::NUM_SLOTS) ? 1 : -1];

// The theater floor and exponent, city allowance in typical units, Reinforce strength ratio, readiness failure limit,
// the Withdraw switch, army march settings, the exposure split switch, camp.nearPlots for full camp value, and the
// situation offset spans and softener, followed by the Reinforce potential weight, per-turn discount and swing scale.
const OperationalConstant OPERATIONAL_CONSTANT[] =
{
	{ "floor.theater",          0.2 },
	{ "theater.steepness",       2.0 },
	{ "city.units",             3.0 },
	{ "reinforce.strengthRatio", 1.0 },
	{ "readiness.maxFailures",  3.0 },
	{ "withdraw.enabled",       1.0 },
	{ "march.enabled",          1.0 },
	{ "march.readyUnits",       2.0 },
	{ "exposure.split",         1.0 },
	{ "camp.nearPlots",         5.0 },
	{ "flavor.focusSpan",      40.0 },
	{ "flavor.riskSpan",       30.0 },
	{ "flavor.softener",        1.0 },
	{ "potential.weight",       0.1 },
	{ "potential.turnDiscount", 0.5 },
	{ "reinforce.swingScale",   2.0 },
};
typedef char OperationalConstantRowsMatchConstants[(sizeof(OPERATIONAL_CONSTANT) / sizeof(OPERATIONAL_CONSTANT[0]) == NUM_OPERATIONAL_CONSTANTS) ? 1 : -1];

// Scales each base weight by its flavor through the weight curve and copies the constants.
void ResolveOperationalWeights(const OperationalSettings& settings, const int (&aiMappedFlavors)[STacticalFlavors::NUM_SLOTS],
	OperationalWeights& out)
{
	for (int i = 0; i < NUM_OPERATIONAL_WEIGHTS; i++)
	{
		const int iSlot = OPERATIONAL_WEIGHT[i].iFlavorSlot;
		out.afWeight[i] = FlavorWeight(settings.afWeightBase[i], settings.afFlavorSlope[iSlot], aiMappedFlavors[iSlot],
			OPERATIONAL_WEIGHT[i].eDirection == FLAVOR_SCALE_DOWN);
	}
	for (int i = 0; i < NUM_OPERATIONAL_CONSTANTS; i++)
		out.afConstant[i] = settings.afConstant[i];
}
