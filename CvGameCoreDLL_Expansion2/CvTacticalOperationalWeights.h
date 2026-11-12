/*	-------------------------------------------------------------------------------------------------------
	Vox Deorum: the operational loop's own constants (VOX_TACTICAL_OPERATIONAL_LOOP): the base weight and flavor
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

// The floors, point constants and readiness failure limit, in OPERATIONAL_CONSTANT row order
enum eOperationalConstant
{
	OPERATIONAL_CONSTANT_THEATER_FLOOR,
	OPERATIONAL_CONSTANT_SIEGE_FLOOR,
	OPERATIONAL_CONSTANT_PRIZE_FLOOR,
	OPERATIONAL_CONSTANT_SAFE_BAR,
	OPERATIONAL_CONSTANT_CAPITAL_PREMIUM,
	OPERATIONAL_CONSTANT_READINESS_MAX_FAILURES,
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
