/*	-------------------------------------------------------------------------------------------------------
	The operational loop's own constants (VOX_TACTICAL_OPERATIONAL_LOOP): the base weight and flavor
	of each stake weight, a flavor slope per military flavor, and the floors, point constants and readiness
	failure limit. One table row per constant, so tuning edits one column. A row's name is its name in template
	overrides, records and the weight sweep. ResolveOperationalWeights turns the tables, a player's mapped flavors
	and any overrides into the turn's weights. Include after CvTacticalAI.h.
	------------------------------------------------------------------------------------------------------- */
#pragma once

#ifndef CIV5_TACTICAL_OPERATIONAL_WEIGHTS_H
#define CIV5_TACTICAL_OPERATIONAL_WEIGHTS_H

struct OperationalSettings;

// The stake weights, in OPERATIONAL_WEIGHT row order
enum eOperationalWeight
{
	OPERATIONAL_WEIGHT_ATTRITION,
	OPERATIONAL_WEIGHT_OCCUPATION,
	OPERATIONAL_WEIGHT_HOLD_CITY,
	OPERATIONAL_WEIGHT_HOLD_GROUND,
	OPERATIONAL_WEIGHT_RISK,
	NUM_OPERATIONAL_WEIGHTS
};

// The theater floor and exponent, city allowance, Reinforce strength ratio, readiness failure limit, Withdraw switch,
// exposure split switch, camp distance, situation offsets and Reinforce potential settings, in row order
enum eOperationalConstant
{
	OPERATIONAL_CONSTANT_THEATER_FLOOR,
	OPERATIONAL_CONSTANT_THEATER_STEEPNESS,
	OPERATIONAL_CONSTANT_CITY_UNITS,
	OPERATIONAL_CONSTANT_STRENGTH_RATIO,
	OPERATIONAL_CONSTANT_READINESS_MAX_FAILURES,
	// 0 turns the Withdraw offer off.
	OPERATIONAL_CONSTANT_WITHDRAW_ENABLED,
	// Army enrollment and target discovery switch, followed by the readiness allowance.
	OPERATIONAL_CONSTANT_MARCH_ENABLED,
	OPERATIONAL_CONSTANT_MARCH_READY_UNITS,
	// 0 turns the fight exposure split off.
	OPERATIONAL_CONSTANT_EXPOSURE_SPLIT,
	// Camps within this many plots of our nearest city count at full value.
	OPERATIONAL_CONSTANT_CAMP_NEAR_PLOTS,
	// The situation offsets: how far focus can raise the objective flavors and RISK can move, and the softener that keeps
	// them near 0 when little is at stake, in typical units.
	OPERATIONAL_CONSTANT_FLAVOR_FOCUS_SPAN,
	OPERATIONAL_CONSTANT_FLAVOR_RISK_SPAN,
	OPERATIONAL_CONSTANT_FLAVOR_SOFTENER,
	// Reinforce counts a share of potential stake, discounted for each further turn to contact.
	OPERATIONAL_CONSTANT_POTENTIAL_WEIGHT,
	OPERATIONAL_CONSTANT_POTENTIAL_TURN_DISCOUNT,
	// Reinforce multiplies its swing by this, so that a rise from no strength to parity counts as 1.
	OPERATIONAL_CONSTANT_SWING_SCALE,
	// At our cities, Reinforce raises its strength ratio by this times the defense fall share.
	OPERATIONAL_CONSTANT_FALL_URGENCY,
	NUM_OPERATIONAL_CONSTANTS
};

// Which way a weight follows its flavor: up with the flavor, or down, as the risk weight falls with RISK
enum eOperationalFlavorDirection
{
	FLAVOR_SCALE_UP,
	FLAVOR_SCALE_DOWN
};

// One stake weight: its name, base value, the military flavor slot that scales it, and the direction
struct OperationalWeight
{
	const char* szName;
	double fBase;
	int iFlavorSlot;
	eOperationalFlavorDirection eDirection;
};

// One named constant and its value
struct OperationalConstant
{
	const char* szName;
	double fValue;
};

// The turn's resolved weights and constants, which priorities read
struct OperationalWeights
{
	double afWeight[NUM_OPERATIONAL_WEIGHTS];
	double afConstant[NUM_OPERATIONAL_CONSTANTS];
};

extern const OperationalWeight OPERATIONAL_WEIGHT[];
// the flavor slope per military flavor slot, and its name (slope.<FLAVOR>)
extern const double OPERATIONAL_FLAVOR_SLOPE[];
extern const char* const OPERATIONAL_FLAVOR_SLOPE_NAMES[];
extern const OperationalConstant OPERATIONAL_CONSTANT[];

// Turns the settings' base weights, slopes and constants, which start as the tables and may carry overrides, and
// the player's mapped flavors into the turn's weights through the flavor weight curve.
void ResolveOperationalWeights(const OperationalSettings& settings, const int (&aiMappedFlavors)[STacticalFlavors::NUM_SLOTS],
	OperationalWeights& out);

#endif //CIV5_TACTICAL_OPERATIONAL_WEIGHTS_H
