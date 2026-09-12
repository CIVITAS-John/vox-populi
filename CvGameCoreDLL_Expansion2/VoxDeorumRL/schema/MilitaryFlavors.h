// Installed from simulator/schema/shared/MilitaryFlavors.h. Edit that source, then reinstall.

#ifndef VOX_RL_MILITARY_FLAVORS_H
#define VOX_RL_MILITARY_FLAVORS_H

#include "VoxRlTypes.h"

// Identifies each stable military flavor slot on the portable wire format.
enum MilitaryFlavorSlot
{
    kMilitaryFlavorRisk = 0,
    kMilitaryFlavorOccupation = 1,
    kMilitaryFlavorAttrition = 2,
    kMilitaryFlavorHoldCity = 3,
    kMilitaryFlavorHoldGround = 4,
    kMilitaryFlavorCount = 5
};

enum
{
    kMilitaryFlavorNeutral = 50,
    kMilitaryFlavorMaximum = 100
};

// Supplies display names in the Vox Populi flavor naming pattern without coupling
// schema fields to flavor semantics. Value bounds live in the manifest, which the
// generator turns into per-record validators shared by builders and readers.
static const char* const kMilitaryFlavorNames[kMilitaryFlavorCount] =
{
    "RISK",
    "OCCUPATION",
    "ATTRITION",
    "HOLD_CITY",
    "HOLD_GROUND"
};

// Initializes every military flavor to its neutral value.
inline void InitializeNeutralMilitaryFlavors(u8 (&flavors)[kMilitaryFlavorCount])
{
    for (int slot = 0; slot < kMilitaryFlavorCount; ++slot)
    {
        flavors[slot] = static_cast<u8>(kMilitaryFlavorNeutral);
    }
}

// Clamps the stock offense flavor and converts it to inverse risk on the 0..100 scale.
inline u8 MilitaryRiskFromOffense(int offense)
{
    if (offense < 0) offense = 0;
    if (offense > 10) offense = 10;
    return static_cast<u8>(kMilitaryFlavorMaximum - 10 * offense);
}

// Initializes the baseline vector from the stock offense flavor.
inline void InitializeBaselineMilitaryFlavors(int offense, u8 (&flavors)[kMilitaryFlavorCount])
{
    InitializeNeutralMilitaryFlavors(flavors);
    flavors[kMilitaryFlavorRisk] = MilitaryRiskFromOffense(offense);
}

// Converts a validated risk value back to the nearest legacy 0..10 offense value.
inline int MilitaryRiskToLegacyOffense(u8 risk)
{
    return (kMilitaryFlavorMaximum - risk + 5) / 10;
}

#endif
