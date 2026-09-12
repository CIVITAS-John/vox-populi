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
    kMilitaryFlavorHold = 3,
    kMilitaryFlavorFlexibility = 4,
    kMilitaryFlavorCount = 5
};

enum
{
    kMilitaryFlavorNeutral = 50,
    kMilitaryFlavorMaximum = 100,
    kMilitaryFlavorPresenceMask = (1 << kMilitaryFlavorCount) - 1
};

// Supplies editable display names without coupling schema fields to flavor semantics.
static const char* const kMilitaryFlavorNames[kMilitaryFlavorCount] =
{
    "risk",
    "occupation",
    "attrition",
    "hold",
    "flexibility"
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

// Converts risk back to the nearest legacy 0..10 offense value.
inline int MilitaryRiskToLegacyOffense(u8 risk)
{
    const int boundedRisk = risk > kMilitaryFlavorMaximum ? kMilitaryFlavorMaximum : risk;
    return (kMilitaryFlavorMaximum - boundedRisk + 5) / 10;
}

// Checks that every flavor value lies on the portable 0..100 scale.
inline bool IsValidMilitaryFlavors(const u8 (&flavors)[kMilitaryFlavorCount])
{
    for (int slot = 0; slot < kMilitaryFlavorCount; ++slot)
    {
        if (flavors[slot] > kMilitaryFlavorMaximum) return false;
    }
    return true;
}

// Checks that an override mask names only defined military flavor slots.
inline bool IsValidMilitaryFlavorPresenceMask(u8 presenceMask)
{
    return (presenceMask & ~static_cast<u8>(kMilitaryFlavorPresenceMask)) == 0;
}

#endif
