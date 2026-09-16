// Installed from simulator/capture/VoxRlCaptureMilitaryEvents.h. Edit that source, then reinstall.

// Vox Deorum: military event evidence retained across capture segment boundaries.
#ifndef VOX_RL_CAPTURE_MILITARY_EVENTS_H
#define VOX_RL_CAPTURE_MILITARY_EVENTS_H

#include "VoxDeorumRL/schema/VoxRlSchema.generated.h"

class CvUnit;
class CvPlot;
struct VoxRlRequestData;
struct VoxRlCampaignData;

// Clears attachment-scoped lineage, buffered events, and economic intervals.
void VoxRlResetMilitaryEvents();
// Resolves the first observed identity, retained through replacement and transfer.
void VoxRlGetUnitLineage(const CvUnit& unit, int& owner, int& unitId);
// Associates subsequent events with the native actor, turn, and military boundary.
void VoxRlSetMilitaryEventContext(PlayerTypes actor, int phase, int turn);
// Allocates the next occurrence order shared by requests and native events.
unsigned int VoxRlTakeMilitaryEventOrder();
// Reads an actor's phase for the given turn without changing active event ownership.
int VoxRlGetMilitaryEventPhase(PlayerTypes actor, int turn);
// Allocates an actor's request order without changing active event ownership.
unsigned int VoxRlTakeMilitaryEventOrderFor(PlayerTypes actor, int turn);
// Retains initialized creation capabilities and optional replacement lineage.
void VoxRlNoteMilitaryUnitCreated(CvUnit& unit, int reason, const CvUnit* source);
// Buffers one successful camp placement before its defenders are created.
void VoxRlNoteBarbarianCampCreated(CvPlot& plot, int improvementType);
// Buffers one initialized barbarian spawn and its optional source camp.
void VoxRlNoteBarbarianUnitCreated(CvUnit& unit, CvPlot& source, bool fromCamp);
// Refreshes creation evidence after caller placement, conversion, and transport setup.
void VoxRlCompleteMilitaryUnit(CvUnit& unit, const CvUnit* source = NULL);
// Associates a completed city-created unit with its native source city.
void VoxRlNoteMilitaryCityArrival(CvUnit& unit, int cityOwner, int cityId);
// Finalizes an outside gift after its placement and granted capabilities are known.
void VoxRlNoteMilitaryGiftArrival(CvUnit& unit, PlayerTypes donor);
// Restores capture lineage when the original unit was removed before replacement.
void VoxRlCompleteMilitaryCapture(CvUnit& unit, PlayerTypes sourceOwner, int sourceUnitId);
// Retains the exiting unit's capabilities before an accepted gift or disband.
void VoxRlNoteMilitaryDeparture(CvUnit& unit, int receiver, int cause);
// Retains an accepted distance gift's source identity through its travel interval.
void VoxRlNoteMilitaryTransferStarted(CvUnit& unit, PlayerTypes receiver, int arrivalTurn);
// Connects a delivered or returned gift to its pending source identity.
void VoxRlCompleteMilitaryTransfer(CvUnit& unit, PlayerTypes donor, PlayerTypes receiver);
// Seeds outstanding gifts, leaving unavailable pre-attachment lineage explicitly absent.
void VoxRlCollectPendingMilitaryTransfers(PlayerTypes observer, VoxRlCampaignData& data);
// Records a native treasury change in the currently labelled military context.
void VoxRlNoteGoldChanged(PlayerTypes player, int amountTimes100);
// Separates the native maintenance charge from the external gold interval.
void VoxRlNoteMaintenanceCharged(PlayerTypes player, int amountTimes100);
// Reads the baseline retained at the native charge, before purchases alter the army.
bool VoxRlGetMilitaryMaintenanceBaseline(PlayerTypes player, int& chargeTimes100, int& baseRateTimes100, int& weightTimes100);
// Reads accrued external gold so the first future batch can exclude seeded effects.
bool VoxRlGetMilitaryEconomicInterval(PlayerTypes player, int& turn, int& externalGoldTimes100);
// Closes one actor's external economic interval at its next turn entry.
void VoxRlBeginMilitaryEconomicTurn(PlayerTypes player);
// Appends buffered evidence without changing the live WORLD replica.
bool VoxRlCollectMilitaryEvents(PlayerTypes observer, VoxRlRequestData& data);
// Consumes only the prefix accepted by the last successful collection.
void VoxRlCommitMilitaryEvents(PlayerTypes observer);

// Labels gold changes made by a supported military action, including nested yields.
class VoxRlMilitaryGoldScope
{
public:
    // Disabled scopes do no capture work.
    explicit VoxRlMilitaryGoldScope(bool enabled, int cause = 0);
    // Restores the enclosing classification.
    ~VoxRlMilitaryGoldScope();
private:
    bool m_enabled;
    int m_previousCause;
    VoxRlMilitaryGoldScope(const VoxRlMilitaryGoldScope&);
    VoxRlMilitaryGoldScope& operator=(const VoxRlMilitaryGoldScope&);
};

#endif
