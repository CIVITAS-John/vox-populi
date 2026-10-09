// Installed from simulator/capture/VoxRlCaptureMilitaryEvents.cpp. Edit that source, then reinstall.

// Vox Deorum: event evidence and gold accounting live beyond any one segment.
#include "CvGameCoreDLLPCH.h"
#include "VoxDeorumRL/VoxRlCaptureMilitaryEvents.h"
#include "VoxDeorumRL/VoxRlCaptureBuilders.h"
#include "VoxDeorumRL/VoxRlCapture.h"
#include "CvMinorCivAI.h"
#include <map>
#include <vector>

namespace
{
    typedef std::pair<int, int> UnitKey;
    // Retains only event capabilities, without the unrelated REQUEST containers.
    struct EventSnapshot
    {
        std::vector<RequestEventUnitRecord> requestEventUnits;
        std::vector<RequestEventUnitSparseFieldRecord> requestEventUnitSparseFields;
        std::vector<RequestEventUnitPlagueRecord> requestEventUnitPlagues;
        std::vector<RequestEventUnitBlockedPromotionRecord> requestEventUnitBlockedPromotions;
        std::vector<RequestEventUnitAttackCountRecord> requestEventUnitAttackCounts;
        std::vector<RequestEventUnitMissionRecord> requestEventUnitMissions;
        std::vector<RequestEventUnitPromotionTurnRecord> requestEventUnitPromotionTurns;

        // Takes freshly collected buffers without copying capability payloads.
        void Take(VoxRlRequestData& data)
        {
            requestEventUnits.swap(data.requestEventUnits);
            requestEventUnitSparseFields.swap(data.requestEventUnitSparseFields);
            requestEventUnitPlagues.swap(data.requestEventUnitPlagues);
            requestEventUnitBlockedPromotions.swap(data.requestEventUnitBlockedPromotions);
            requestEventUnitAttackCounts.swap(data.requestEventUnitAttackCounts);
            requestEventUnitMissions.swap(data.requestEventUnitMissions);
            requestEventUnitPromotionTurns.swap(data.requestEventUnitPromotionTurns);
        }
    };
    // One immutable event-time snapshot, independent of later live unit changes.
    struct MilitaryEvent
    {
        int kind;
        int receiver;
        RequestMilitaryArrivalRecord row;
        EventSnapshot snapshot;
        // Initializes optional references explicitly; zero is a valid native identity.
        MilitaryEvent() : kind(0), receiver(-1), row()
        {
            row.sourceOwner = row.lineageOwner = row.sourceCityOwner = row.donorPlayer = -1;
            row.sourceUnitId = row.lineageUnitId = row.sourceCityId = row.plotIndex = row.eventUnitIndex = -1;
        }
    };
    // Totals over one observed economic interval, in hundredths of gold.
    struct EconomicInterval
    {
        int turn;
        __int64 external;
        // The first interval has no earlier observed boundary.
        EconomicInterval() : turn(-1), external(0) {}
    };
    // The charge and weighting refer to the army present when native maintenance ran.
    struct MaintenanceBaseline
    {
        int charge;
        int rate;
        int weight;
        // Initializes a free maintenance baseline.
        MaintenanceBaseline() : charge(0), rate(0), weight(0) {}
    };
    // Retains one actor's order within its current game turn.
    struct ActorTurnSchedule
    {
        int turn;
        int phase;
        unsigned int nextOrder;
        bool initialized;
        // Starts without a captured actor-turn context.
        ActorTurnSchedule() : turn(-1), phase(0), nextOrder(0), initialized(false) {}
    };
    // One unit or city an open damage scope changed: its native identity (unit ID, or plot index
    // for a city, so a capture that replaces the city object stays one subject) and the row its
    // first change began.
    struct DamageSubject
    {
        int owner;
        int id;
        RequestDamageRecord row;
    };
    // One open damage scope: its cause, its two sides, and the subjects it changed, in
    // first-change order.
    struct OpenDamageScope
    {
        int cause;
        int attacker;
        int defender;
        std::vector<DamageSubject> subjects;
    };
    // One finished damage row and the admitted major whose segment carries it.
    struct DamageRow
    {
        int receiver;
        RequestDamageRecord row;
    };
    std::map<UnitKey, UnitKey> lineage;
    std::vector<MilitaryEvent> events;
    std::vector<OpenDamageScope> openDamageScopes;
    std::vector<DamageRow> damageRows;
    std::vector<RequestBarbarianCampCreationRecord> campCreations;
    std::map<int, unsigned int> campIds;
    std::vector<RequestMilitaryGoldTransactionRecord> goldTransactions;
    std::vector<RequestEconomicBatchRecord> economicBatches;
    std::map<int, EconomicInterval> economicIntervals;
    std::map<int, MaintenanceBaseline> maintenanceBaselines;
    std::map<UnitKey, CampaignPendingTransferRecord> pendingTransfers;
    size_t collectedEvents = 0;
    size_t collectedCamps = 0;
    size_t collectedGold = 0;
    size_t collectedEconomics = 0;
    size_t collectedDamage = 0;
    int eventActor = -1;
    int eventPhase = 0;
    int eventTurn = -1;
    ActorTurnSchedule actorTurnSchedules[MAX_PLAYERS];
    unsigned int nextTransfer = 1;
    unsigned int nextCreatedCamp = 0x80000001u;
    unsigned int goldDepth = 0;
    int goldCause = 0;
    unsigned int upgradeDepth = 0;
    bool captureFailed = false;

    // Starts a new bounded schedule only when this actor enters a different turn.
    ActorTurnSchedule* GetActorTurnSchedule(int actor, int turn)
    {
        if (actor < 0 || actor >= MAX_PLAYERS) return NULL;
        ActorTurnSchedule& schedule = actorTurnSchedules[actor];
        if (!schedule.initialized || schedule.turn != turn)
        {
            schedule.turn = turn;
            schedule.phase = 0;
            schedule.nextOrder = 0;
            schedule.initialized = true;
        }
        return &schedule;
    }

    // Reads an actor-turn schedule without changing the active event context.
    const ActorTurnSchedule* FindActorTurnSchedule(int actor, int turn)
    {
        if (actor < 0 || actor >= MAX_PLAYERS) return NULL;
        const ActorTurnSchedule& schedule = actorTurnSchedules[actor];
        return schedule.initialized && schedule.turn == turn ? &schedule : NULL;
    }

    // Checks whether any configured major can carry shared barbarian evidence.
    bool HasMajorCarrier()
    {
        for (int player = 0; player < MAX_MAJOR_CIVS; ++player)
            if (VoxRlCapture::GetInstance().AdmitsMilitaryEvents(static_cast<PlayerTypes>(player))) return true;
        return false;
    }

    // Resolves the segment that carries a unit's event: its admitted major owner, or
    // the first committing admitted major for a city-state, like barbarian spawns.
    bool ResolveEventReceiver(const CvUnit& unit, int& receiver)
    {
        const PlayerTypes owner = unit.getOwner();
        if (VoxRlCapture::GetInstance().AdmitsMilitaryEvents(owner)) { receiver = owner; return true; }
        if (owner < MAX_MAJOR_CIVS || owner >= MAX_CIV_PLAYERS || owner == BARBARIAN_PLAYER) return false;
        if (!GET_PLAYER(owner).isMinorCiv() || !HasMajorCarrier()) return false;
        receiver = -1;
        return true;
    }

    // Finds the latest provisional arrival that caller completion may still update.
    MilitaryEvent* PendingArrival(const CvUnit& unit)
    {
        for (size_t i = events.size(); i > 0; --i)
            if (events[i - 1].kind == 0 && events[i - 1].row.sourceOwner == unit.getOwner() &&
                events[i - 1].row.sourceUnitId == unit.GetID()) return &events[i - 1];
        return NULL;
    }

    // Checks turn storage before conversion, including buffered events outside a WORLD.
    template <typename Target>
    void SetTurn(Target& target, int value, const char* record, const char* field)
    {
        if (value < -1 || value > 32767)
        {
            VoxRlNoteCaptureRangeFailure("captureRange", record, field, value, -1, 32767);
            captureFailed = true;
            return;
        }
        target = static_cast<Target>(value);
    }

    // Checks plot identity storage before narrowing to the wire type.
    template <typename Target>
    void SetPlot(Target& target, int value, const char* record)
    {
        if (value < -1 || value > 32767)
        {
            VoxRlNoteCaptureRangeFailure("captureRange", record, "plotIndex", value, -1, 32767);
            captureFailed = true;
            return;
        }
        target = static_cast<Target>(value);
    }

    // Fills the occurrence identity before any later observer receives the event.
    template <typename Row>
    void SetOccurrence(Row& row)
    {
        SetTurn(row.occurrenceTurn, GC.getGame().getGameTurn(), "MilitaryEvent", "occurrenceTurn");
        row.occurrenceActor = static_cast<i8>(eventActor);
        row.occurrencePhase = static_cast<u8>(eventPhase);
        row.occurrenceOrder = VoxRlTakeMilitaryEventOrder();
    }

    // Collects initialized capabilities now, even when the receiving segment is closed.
    void Snapshot(MilitaryEvent& event, CvUnit& unit)
    {
        VoxRlRequestData snapshot;
        event.row.eventUnitIndex = -1;
        if (unit.plot() == NULL)
        {
            VoxRlNoteCaptureRangeFailure("missingEventPlot", "MilitaryEvent", "plotIndex", -1, 0, 32767);
            captureFailed = true;
            return;
        }
        int snapshotIndex = -1;
        if (!VoxRlAppendEventUnitSnapshot(unit, unit.getTeam(), snapshot, &snapshotIndex) ||
            !VoxRlAssignChecked(event.row.eventUnitIndex, snapshotIndex,
                "RequestMilitaryArrivalRecord", "eventUnitIndex", -1)) captureFailed = true;
        event.snapshot.Take(snapshot);
        SetPlot(event.row.plotIndex, unit.plot()->GetPlotIndex(), "MilitaryEvent");
    }

    // Starts an event with owner-qualified source and persistent lineage.
    MilitaryEvent MakeEvent(CvUnit& unit, int kind, int cause)
    {
        MilitaryEvent event;
        event.kind = kind;
        event.receiver = unit.getOwner();
        event.row.sourceOwner = static_cast<i8>(unit.getOwner());
        event.row.sourceUnitId = unit.GetID();
        int owner, id;
        VoxRlGetUnitLineage(unit, owner, id);
        event.row.lineageOwner = static_cast<i8>(owner);
        event.row.lineageUnitId = id;
        event.row.cause = static_cast<u8>(cause);
        SetOccurrence(event.row);
        Snapshot(event, unit);
        return event;
    }

    // Copies the common event contract without relying on different record layouts.
    template <typename Row>
    Row EventRow(const RequestMilitaryArrivalRecord& source)
    {
        Row row = Row();
        row.occurrenceTurn = source.occurrenceTurn;
        row.occurrenceActor = source.occurrenceActor;
        row.occurrencePhase = source.occurrencePhase;
        row.occurrenceOrder = source.occurrenceOrder;
        row.cause = source.cause;
        row.sourceOwner = source.sourceOwner;
        row.sourceUnitId = source.sourceUnitId;
        row.lineageOwner = source.lineageOwner;
        row.lineageUnitId = source.lineageUnitId;
        row.transferId = source.transferId;
        row.sourceCityOwner = source.sourceCityOwner;
        row.sourceCityId = source.sourceCityId;
        row.donorPlayer = source.donorPlayer;
        row.plotIndex = source.plotIndex;
        row.eventUnitIndex = source.eventUnitIndex;
        row.initializationComplete = source.initializationComplete;
        return row;
    }

    // Resolves a camp that existed when recording began from its initial plot.
    unsigned int ExistingCampId(CvPlot& plot)
    {
        const int plotIndex = plot.GetPlotIndex();
        std::map<int, unsigned int>::iterator found = campIds.find(plotIndex);
        if (found != campIds.end()) return found->second;
        const unsigned int id = static_cast<unsigned int>(plotIndex) + 1u;
        campIds[plotIndex] = id;
        return id;
    }

    // Appends snapshot children using the generated range ownership helpers.
    bool AppendSnapshot(const MilitaryEvent& event, VoxRlRequestData& data, i16& unitIndex)
    {
        if (event.snapshot.requestEventUnits.empty()) { unitIndex = -1; return true; }
        RequestEventUnitRecord row = event.snapshot.requestEventUnits.front();
        if (!AppendRequestEventUnitRecordSparseFieldRange(&row, &data, event.snapshot.requestEventUnitSparseFields)) return false;
        if (!VoxRlAssignChecked(unitIndex, static_cast<i32>(data.requestEventUnits.size()),
            "RequestMilitaryArrivalRecord", "eventUnitIndex", -1)) return false;
        data.requestEventUnits.push_back(row);
        return true;
    }

    // Queues one finished damage row for every admitted major, as the grade of any of them can
    // count it whoever took part. A human major never runs UpdateOperations, so its segment never
    // commits and would only accumulate copies.
    void QueueDamageRow(const RequestDamageRecord& row)
    {
        DamageRow queued;
        queued.row = row;
        for (int player = 0; player < MAX_MAJOR_CIVS; ++player)
        {
            if (!VoxRlCapture::GetInstance().AdmitsMilitaryEvents(static_cast<PlayerTypes>(player)) ||
                GET_PLAYER(static_cast<PlayerTypes>(player)).isHuman(ISHUMAN_AI_UNITS)) continue;
            queued.receiver = player;
            damageRows.push_back(queued);
        }
    }

    // Returns the open scope's entry for one unit (by owner and ID) or city (by plot), or NULL.
    DamageSubject* FindDamageSubject(OpenDamageScope& scope, int subject, int owner, int id)
    {
        for (size_t i = 0; i < scope.subjects.size(); ++i)
        {
            DamageSubject& entry = scope.subjects[i];
            if (entry.row.subject != subject || entry.id != id) continue;
            if (subject == VOX_RL_DAMAGE_CITY || entry.owner == owner) return &entry;
        }
        return NULL;
    }
}

// Clears all evidence when the native attachment changes.
void VoxRlResetMilitaryEvents()
{
    lineage.clear(); events.clear(); openDamageScopes.clear(); damageRows.clear(); campCreations.clear(); campIds.clear(); goldTransactions.clear(); economicBatches.clear(); economicIntervals.clear(); maintenanceBaselines.clear(); pendingTransfers.clear();
    collectedEvents = collectedCamps = collectedGold = collectedEconomics = collectedDamage = 0;
    for (int actor = 0; actor < MAX_PLAYERS; ++actor) actorTurnSchedules[actor] = ActorTurnSchedule();
    eventActor = -1; eventPhase = 0; eventTurn = -1; nextTransfer = 1; nextCreatedCamp = 0x80000001u; goldDepth = 0; goldCause = 0; upgradeDepth = 0; captureFailed = false;
}

// Uses the first observed owner-qualified unit identity within the source game.
void VoxRlGetUnitLineage(const CvUnit& unit, int& owner, int& unitId)
{
    const UnitKey key(unit.getOwner(), unit.GetID());
    std::map<UnitKey, UnitKey>::iterator found = lineage.find(key);
    if (found == lineage.end()) found = lineage.insert(std::make_pair(key, key)).first;
    owner = found->second.first; unitId = found->second.second;
}

// Retains native scheduling context even for actors without capture segments.
void VoxRlSetMilitaryEventContext(PlayerTypes actor, int phase, int turn)
{
    ActorTurnSchedule* schedule = GetActorTurnSchedule(static_cast<int>(actor), turn);
    if (schedule != NULL) schedule->phase = phase;
    eventActor = actor; eventPhase = phase; eventTurn = turn;
}

// Returns the stored phase for an actor-turn without switching event ownership.
int VoxRlGetMilitaryEventPhase(PlayerTypes actor, int turn)
{
    const ActorTurnSchedule* schedule = FindActorTurnSchedule(static_cast<int>(actor), turn);
    return schedule == NULL ? 0 : schedule->phase;
}

// Allocates an order for a request without changing active event ownership.
unsigned int VoxRlTakeMilitaryEventOrderFor(PlayerTypes actor, int turn)
{
    ActorTurnSchedule* schedule = GetActorTurnSchedule(static_cast<int>(actor), turn);
    return schedule == NULL ? 0 : schedule->nextOrder++;
}

// Returns a unique occurrence position within the active actor and turn.
unsigned int VoxRlTakeMilitaryEventOrder()
{
    ActorTurnSchedule* schedule = GetActorTurnSchedule(eventActor, eventTurn);
    return schedule == NULL ? 0 : schedule->nextOrder++;
}

// Buffers creation evidence and propagates the original identity before collection.
void VoxRlNoteMilitaryUnitCreated(CvUnit& unit, int reason, const CvUnit* source)
{
    if (source != NULL)
    {
        int owner, id; VoxRlGetUnitLineage(*source, owner, id);
        lineage[UnitKey(unit.getOwner(), unit.GetID())] = UnitKey(owner, id);
    }
    else lineage[UnitKey(unit.getOwner(), unit.GetID())] = UnitKey(unit.getOwner(), unit.GetID());
    if (reason == REASON_UPGRADE || upgradeDepth != 0) return;
    if (unit.getOwner() == BARBARIAN_PLAYER) return;
    // City-state arrivals are shared evidence for the next admitted major segment.
    int receiver;
    if (!ResolveEventReceiver(unit, receiver)) return;
    // A purchase is an arrival like production, so a replay can deliver one native made outside the
    // branch's own selectors, such as the economic AI's purchases before the military checkpoint.
    const int cause = reason == REASON_TRAIN ? VOX_RL_EVENT_PRODUCTION : reason == REASON_GIFT ? VOX_RL_EVENT_GIFT :
        reason == REASON_BUY || reason == REASON_FAITH_BUY ? VOX_RL_EVENT_PURCHASE :
        reason == REASON_CONVERT ? VOX_RL_EVENT_CONVERSION : VOX_RL_EVENT_UNKNOWN;
    MilitaryEvent event = MakeEvent(unit, 0, cause);
    event.receiver = receiver;
    events.push_back(event);
}

// Records a new camp with an identity distinct from every earlier camp at the plot.
void VoxRlNoteBarbarianCampCreated(CvPlot& plot, int improvementType)
{
    if (!HasMajorCarrier()) return;
    RequestBarbarianCampCreationRecord row = RequestBarbarianCampCreationRecord();
    SetOccurrence(row);
    row.campId = nextCreatedCamp++;
    if (!VoxRlAssignChecked(row.improvementType, improvementType,
        "RequestBarbarianCampCreationRecord", "improvementType", 0)) captureFailed = true;
    SetPlot(row.plotIndex, plot.GetPlotIndex(), "BarbarianCampCreation");
    campIds[plot.GetPlotIndex()] = row.campId;
    campCreations.push_back(row);
}

// Records a completed barbarian spawn for delivery by the next admitted major segment.
void VoxRlNoteBarbarianUnitCreated(CvUnit& unit, CvPlot& source, bool fromCamp)
{
    if (!HasMajorCarrier()) return;
    MilitaryEvent event = MakeEvent(unit, 0, VOX_RL_EVENT_BARBARIAN);
    event.receiver = -1;
    event.row.initializationComplete = 1;
    event.row.sourceCampId = fromCamp ? ExistingCampId(source) : 0;
    events.push_back(event);
}

// Updates only the pending initialization for this unit, preserving its occurrence.
void VoxRlCompleteMilitaryUnit(CvUnit& unit, const CvUnit* source)
{
    int owner, id;
    if (source != NULL)
    {
        VoxRlGetUnitLineage(*source, owner, id);
        lineage[UnitKey(unit.getOwner(), unit.GetID())] = UnitKey(owner, id);
    }
    VoxRlGetUnitLineage(unit, owner, id);
    MilitaryEvent* pending = PendingArrival(unit);
    if (pending != NULL)
    {
        MilitaryEvent& event = *pending;
        event.row.initializationComplete = 1;
        event.row.lineageOwner = static_cast<i8>(owner); event.row.lineageUnitId = id;
        if (source != NULL)
        {
            if (event.row.cause == VOX_RL_EVENT_GIFT)
                event.row.donorPlayer = static_cast<i8>(source->getOwner());
        }
        Snapshot(event, unit);
    }
}

// Retains the source city after native placement and completion yields.
void VoxRlNoteMilitaryCityArrival(CvUnit& unit, int cityOwner, int cityId)
{
    VoxRlCompleteMilitaryUnit(unit);
    MilitaryEvent* event = PendingArrival(unit);
    if (event == NULL) return;
    event->row.sourceCityOwner = static_cast<i8>(cityOwner); event->row.sourceCityId = cityId;
}

// Retains an outside gift's donor after its native initialization is complete.
void VoxRlNoteMilitaryGiftArrival(CvUnit& unit, PlayerTypes donor)
{
    VoxRlCompleteMilitaryUnit(unit);
    MilitaryEvent* event = PendingArrival(unit);
    if (event == NULL) return;
    event->row.cause = VOX_RL_EVENT_GIFT; event->row.donorPlayer = static_cast<i8>(donor);
    // A city-state buyout records the donor's departure before the replacement exists.
    if (event->row.transferId != 0) return;
    for (size_t i = events.size(); i > 0; --i)
    {
        const MilitaryEvent& departure = events[i - 1];
        if (departure.kind == 1 && departure.row.transferId != 0 && departure.row.sourceOwner == donor &&
            departure.row.donorPlayer == unit.getOwner() && departure.row.lineageOwner == event->row.lineageOwner &&
            departure.row.lineageUnitId == event->row.lineageUnitId)
        {
            event->row.transferId = departure.row.transferId;
            return;
        }
    }
}

// Resolves a captured replacement against the removed source's recorded identity.
void VoxRlCompleteMilitaryCapture(CvUnit& unit, PlayerTypes sourceOwner, int sourceUnitId)
{
    const UnitKey source(sourceOwner, sourceUnitId);
    std::map<UnitKey, UnitKey>::const_iterator found = lineage.find(source);
    lineage[UnitKey(unit.getOwner(), unit.GetID())] = found == lineage.end() ? source : found->second;
    VoxRlCompleteMilitaryUnit(unit);
    MilitaryEvent* event = PendingArrival(unit);
    if (event == NULL) return;
    event->row.cause = VOX_RL_EVENT_CONVERSION; event->row.donorPlayer = -1;
}

// Captures a departure before the native unit is removed from its original owner.
void VoxRlNoteMilitaryDeparture(CvUnit& unit, int receiver, int cause)
{
    int eventReceiver;
    if (!ResolveEventReceiver(unit, eventReceiver)) return;
    MilitaryEvent event = MakeEvent(unit, 1, cause);
    event.receiver = eventReceiver;
    event.row.initializationComplete = 1;
    if (cause == VOX_RL_EVENT_GIFT) event.row.transferId = nextTransfer++;
    event.row.donorPlayer = static_cast<i8>(cause == VOX_RL_EVENT_GIFT ? receiver : -1);
    events.push_back(event);
    // Immediate gifts share the transfer identity with their source departure. The
    // arrival is matched by its owner, since a city-state arrival has a shared receiver.
    if (event.row.transferId != 0)
        for (size_t i = 0; i + 1 < events.size(); ++i)
            if (events[i].kind == 0 && events[i].row.sourceOwner == receiver &&
                events[i].row.lineageOwner == event.row.lineageOwner && events[i].row.lineageUnitId == event.row.lineageUnitId)
                events[i].row.transferId = event.row.transferId;
}

// Records the source before native distance-gift storage removes the unit.
void VoxRlNoteMilitaryTransferStarted(CvUnit& unit, PlayerTypes receiver, int arrivalTurn)
{
    const size_t before = events.size();
    VoxRlNoteMilitaryDeparture(unit, receiver, VOX_RL_EVENT_GIFT);
    CampaignPendingTransferRecord row = CampaignPendingTransferRecord();
    row.transferId = events.size() == before ? nextTransfer++ : events.back().row.transferId;
    row.sourceOwner = static_cast<i8>(unit.getOwner()); row.sourceUnitId = unit.GetID();
    int owner, id; VoxRlGetUnitLineage(unit, owner, id);
    row.lineageOwner = static_cast<i8>(owner); row.lineageUnitId = id;
    row.receiver = static_cast<i8>(receiver);
    SetTurn(row.arrivalTurn, arrivalTurn, "CampaignPendingTransferRecord", "arrivalTurn");
    pendingTransfers[UnitKey(receiver, unit.getOwner())] = row;
}

// Carries a pending gift's lineage into its new native unit identity.
void VoxRlCompleteMilitaryTransfer(CvUnit& unit, PlayerTypes donor, PlayerTypes receiver)
{
    const UnitKey key(receiver, donor);
    std::map<UnitKey, CampaignPendingTransferRecord>::iterator found = pendingTransfers.find(key);
    if (found == pendingTransfers.end()) return;
    const CampaignPendingTransferRecord& transfer = found->second;
    if (transfer.lineageOwner >= 0 && transfer.lineageUnitId >= 0)
        lineage[UnitKey(unit.getOwner(), unit.GetID())] = UnitKey(transfer.lineageOwner, transfer.lineageUnitId);
    VoxRlCompleteMilitaryUnit(unit);
    MilitaryEvent* event = PendingArrival(unit);
    if (event != NULL)
    {
        event->row.transferId = transfer.transferId; event->row.cause = VOX_RL_EVENT_GIFT; event->row.donorPlayer = static_cast<i8>(donor);
    }
    pendingTransfers.erase(found);
}

// Reads native outstanding gifts without inventing unavailable source identities.
void VoxRlCollectPendingMilitaryTransfers(PlayerTypes observer, VoxRlCampaignData& data)
{
    for (int receiver = MAX_MAJOR_CIVS; receiver < MAX_CIV_PLAYERS; ++receiver)
    {
        CvPlayer& owner = GET_PLAYER(static_cast<PlayerTypes>(receiver));
        if (!owner.isAlive() || !owner.isMinorCiv()) continue;
        for (int donor = 0; donor < MAX_MAJOR_CIVS; ++donor)
        {
            if (observer != donor && observer != receiver) continue;
            const CvMinorCivIncomingUnitGift& gift = owner.GetMinorCivAI()->getIncomingUnitGift(static_cast<PlayerTypes>(donor));
            if (!gift.hasIncomingUnit()) continue;
            const UnitKey key(receiver, donor);
            std::map<UnitKey, CampaignPendingTransferRecord>::iterator found = pendingTransfers.find(key);
            if (found == pendingTransfers.end())
            {
                CampaignPendingTransferRecord row = CampaignPendingTransferRecord();
                row.transferId = nextTransfer++; row.sourceOwner = static_cast<i8>(donor); row.sourceUnitId = -1;
                row.lineageOwner = -1; row.lineageUnitId = -1; row.receiver = static_cast<i8>(receiver);
                found = pendingTransfers.insert(std::make_pair(key, row)).first;
            }
            SetTurn(found->second.arrivalTurn, GC.getGame().getGameTurn() + gift.getArrivalCountdown(), "CampaignPendingTransferRecord", "arrivalTurn");
            data.campaignPendingTransfers.push_back(found->second);
        }
    }
}

// Records actual military changes individually and bundles all remaining gold.
void VoxRlNoteGoldChanged(PlayerTypes player, int amountTimes100)
{
    if (amountTimes100 == 0) return;
    if (goldDepth != 0)
    {
        RequestMilitaryGoldTransactionRecord row = RequestMilitaryGoldTransactionRecord();
        SetOccurrence(row); row.player = static_cast<i8>(player); row.amountTimes100 = amountTimes100;
        row.cause = static_cast<u8>(goldCause);
        goldTransactions.push_back(row);
    }
    else economicIntervals[player].external += amountTimes100;
}

// Adds back maintenance already included in the native net treasury change.
void VoxRlNoteMaintenanceCharged(PlayerTypes player, int amountTimes100)
{
    economicIntervals[player].external += amountTimes100;
    MaintenanceBaseline& baseline = maintenanceBaselines[player];
    CvPlayer& owner = GET_PLAYER(player);
    baseline.charge = amountTimes100;
    baseline.rate = owner.getGoldPerUnitTimes100();
    __int64 weight = 0;
    int loop = 0;
    for (CvUnit* unit = owner.firstUnit(&loop); unit != NULL; unit = owner.nextUnit(&loop))
        weight += baseline.rate + unit->getUnitInfo().GetExtraMaintenanceCost() * 100;
    if (weight < (-2147483647 - 1) || weight > 2147483647) captureFailed = true;
    else baseline.weight = static_cast<int>(weight);
}

// Keeps the captured charge denominator stable when the current army changes.
bool VoxRlGetMilitaryMaintenanceBaseline(PlayerTypes player, int& chargeTimes100, int& baseRateTimes100, int& weightTimes100)
{
    std::map<int, MaintenanceBaseline>::const_iterator found = maintenanceBaselines.find(player);
    if (found == maintenanceBaselines.end()) return false;
    chargeTimes100 = found->second.charge; baseRateTimes100 = found->second.rate; weightTimes100 = found->second.weight;
    return true;
}

// Supplies the already-applied part of a still-open external economic interval.
bool VoxRlGetMilitaryEconomicInterval(PlayerTypes player, int& turn, int& externalGoldTimes100)
{
    std::map<int, EconomicInterval>::const_iterator found = economicIntervals.find(player);
    if (found == economicIntervals.end()) return false;
    turn = found->second.turn;
    if (found->second.external < (-2147483647 - 1) || found->second.external > 2147483647) return false;
    externalGoldTimes100 = static_cast<int>(found->second.external);
    return true;
}

// Publishes one completed external interval, including zero-valued intervals.
void VoxRlBeginMilitaryEconomicTurn(PlayerTypes player)
{
    EconomicInterval& interval = economicIntervals[player];
    const int turn = GC.getGame().getGameTurn();
    if (interval.turn == turn) return;
    if (interval.turn >= 0)
    {
        RequestEconomicBatchRecord row = RequestEconomicBatchRecord();
        row.player = static_cast<i8>(player);
        SetTurn(row.fromTurn, interval.turn, "RequestEconomicBatchRecord", "fromTurn");
        SetTurn(row.toTurn, turn, "RequestEconomicBatchRecord", "toTurn");
        row.occurrencePhase = static_cast<u8>(eventPhase); row.occurrenceOrder = VoxRlTakeMilitaryEventOrder();
        if (interval.external < (-2147483647 - 1) || interval.external > 2147483647) captureFailed = true;
        else row.externalGoldTimes100 = static_cast<i32>(interval.external);
        economicBatches.push_back(row);
    }
    interval.turn = turn; interval.external = 0;
}

// Reads the unit in a combat role first, then the city, as the attacker or defender may be either.
PlayerTypes VoxRlCombatOwner(const CvCombatInfo& info, BattleUnitTypes role)
{
    const CvUnit* unit = info.getUnit(role);
    if (unit != NULL) return unit->getOwner();
    const CvCity* city = info.getCity(role);
    return city != NULL ? city->getOwner() : NO_PLAYER;
}

// Keeps the unit's identity, plot, and damage before its first change in the innermost open scope.
void VoxRlNoteUnitDamage(const CvUnit& unit, int oldDamage)
{
    if (openDamageScopes.empty()) return;
    OpenDamageScope& scope = openDamageScopes.back();
    if (FindDamageSubject(scope, VOX_RL_DAMAGE_UNIT, unit.getOwner(), unit.GetID()) != NULL) return;
    DamageSubject entry;
    entry.owner = unit.getOwner();
    entry.id = unit.GetID();
    entry.row = RequestDamageRecord();
    entry.row.subject = VOX_RL_DAMAGE_UNIT;
    entry.row.owner = static_cast<i8>(unit.getOwner());
    // The grade counts third-party damage by where the unit stood; -1 when it has no plot.
    entry.row.plotIndex = -1;
    if (unit.plot() != NULL) SetPlot(entry.row.plotIndex, unit.plot()->GetPlotIndex(), "RequestDamageRecord");
    int owner, id;
    VoxRlGetUnitLineage(unit, owner, id);
    entry.row.lineageOwner = static_cast<i8>(owner);
    entry.row.lineageUnitId = id;
    if (!VoxRlAssignChecked(entry.row.unitType, static_cast<int>(unit.getUnitType()), "RequestDamageRecord", "unitType", 0) ||
        !VoxRlAssignChecked(entry.row.level, unit.getLevel(), "RequestDamageRecord", "level", 0) ||
        !VoxRlAssignChecked(entry.row.maxHitPoints, unit.GetMaxHitPoints(), "RequestDamageRecord", "maxHitPoints", 1) ||
        !VoxRlAssignChecked(entry.row.damageBefore, oldDamage, "RequestDamageRecord", "damageBefore", 0))
        captureFailed = true;
    scope.subjects.push_back(entry);
}

// Keeps the city's plot, owner, and damage before its first change in the innermost open scope.
void VoxRlNoteCityDamage(const CvCity& city, int oldDamage)
{
    if (openDamageScopes.empty() || city.plot() == NULL) return;
    OpenDamageScope& scope = openDamageScopes.back();
    const int plotIndex = city.plot()->GetPlotIndex();
    if (FindDamageSubject(scope, VOX_RL_DAMAGE_CITY, city.getOwner(), plotIndex) != NULL) return;
    DamageSubject entry;
    entry.owner = city.getOwner();
    entry.id = plotIndex;
    entry.row = RequestDamageRecord();
    entry.row.subject = VOX_RL_DAMAGE_CITY;
    entry.row.owner = static_cast<i8>(city.getOwner());
    entry.row.lineageOwner = static_cast<i8>(NO_PLAYER);
    entry.row.lineageUnitId = -1;
    entry.row.unitType = -1;
    entry.row.level = 0;
    SetPlot(entry.row.plotIndex, plotIndex, "RequestDamageRecord");
    if (!VoxRlAssignChecked(entry.row.maxHitPoints, city.GetMaxHitPoints(), "RequestDamageRecord", "maxHitPoints", 1) ||
        !VoxRlAssignChecked(entry.row.damageBefore, oldDamage, "RequestDamageRecord", "damageBefore", 0))
        captureFailed = true;
    scope.subjects.push_back(entry);
}

// Stages event evidence separately from live upserts, retaining receiver buffers.
bool VoxRlCollectMilitaryEvents(PlayerTypes observer, VoxRlRequestData& data)
{
    if (captureFailed) return false;
    // Native actions can refresh danger before their final cost and replacement state.
    // Their evidence becomes publishable only after the enclosing action completes.
    if (goldDepth != 0)
    {
        collectedEvents = collectedCamps = collectedGold = collectedEconomics = collectedDamage = 0;
        return true;
    }
    collectedEvents = events.size(); collectedCamps = campCreations.size(); collectedGold = goldTransactions.size(); collectedEconomics = economicBatches.size();
    collectedDamage = damageRows.size();
    // Each damage row is queued once per admitted major, so every segment carries its own copy.
    for (size_t i = 0; i < collectedDamage; ++i)
        if (damageRows[i].receiver == observer) data.requestDamage.push_back(damageRows[i].row);
    for (size_t i = 0; i < collectedEvents; ++i)
    {
        const MilitaryEvent& event = events[i];
        if (event.receiver >= 0 && event.receiver != observer) continue;
        RequestMilitaryArrivalRecord row = event.row;
        if (!AppendSnapshot(event, data, row.eventUnitIndex)) return false;
        if (event.kind == 0)
        {
            RequestMilitaryArrivalRecord arrival = row;
            if (!AppendRequestMilitaryArrivalRecordPlagueRange(&arrival, &data, event.snapshot.requestEventUnitPlagues) ||
                !AppendRequestMilitaryArrivalRecordBlockedPromotionRange(&arrival, &data, event.snapshot.requestEventUnitBlockedPromotions) ||
                !AppendRequestMilitaryArrivalRecordAttackCountRange(&arrival, &data, event.snapshot.requestEventUnitAttackCounts) ||
                !AppendRequestMilitaryArrivalRecordMissionRange(&arrival, &data, event.snapshot.requestEventUnitMissions) ||
                !AppendRequestMilitaryArrivalRecordPromotionTurnRange(&arrival, &data, event.snapshot.requestEventUnitPromotionTurns)) return false;
            data.requestMilitaryArrivals.push_back(arrival);
        }
        else if (event.kind == 1)
        {
            RequestMilitaryDepartureRecord departure = EventRow<RequestMilitaryDepartureRecord>(row);
            if (!AppendRequestMilitaryDepartureRecordPlagueRange(&departure, &data, event.snapshot.requestEventUnitPlagues) ||
                !AppendRequestMilitaryDepartureRecordBlockedPromotionRange(&departure, &data, event.snapshot.requestEventUnitBlockedPromotions) ||
                !AppendRequestMilitaryDepartureRecordAttackCountRange(&departure, &data, event.snapshot.requestEventUnitAttackCounts) ||
                !AppendRequestMilitaryDepartureRecordMissionRange(&departure, &data, event.snapshot.requestEventUnitMissions) ||
                !AppendRequestMilitaryDepartureRecordPromotionTurnRange(&departure, &data, event.snapshot.requestEventUnitPromotionTurns)) return false;
            data.requestMilitaryDepartures.push_back(departure);
        }
    }
    data.requestBarbarianCampCreations.insert(data.requestBarbarianCampCreations.end(), campCreations.begin(), campCreations.begin() + collectedCamps);
    // Economic evidence belongs to the shared game timeline. The next admitted
    // segment carries every player's rows; row.player identifies the affected treasury.
    // Unit arrivals and departures instead wait for their receiving observer above,
    // except city-state and barbarian events, which the first committing segment carries.
    data.requestMilitaryGoldTransactions.insert(data.requestMilitaryGoldTransactions.end(), goldTransactions.begin(), goldTransactions.end());
    data.requestEconomicBatches.insert(data.requestEconomicBatches.end(), economicBatches.begin(), economicBatches.end());
    return true;
}

// Consumes only staged rows after their containing request has been accepted.
void VoxRlCommitMilitaryEvents(PlayerTypes observer)
{
    size_t retained = 0;
    for (size_t i = 0; i < events.size(); ++i)
    {
        if (i < collectedEvents && (events[i].receiver < 0 || events[i].receiver == observer)) continue;
        if (retained != i) events[retained] = events[i];
        ++retained;
    }
    events.resize(retained);
    retained = 0;
    // Copies for a major no longer admitted, such as one that died, are dropped as no segment of
    // that major will carry them.
    for (size_t i = 0; i < damageRows.size(); ++i)
    {
        if (i < collectedDamage && damageRows[i].receiver == observer) continue;
        if (!VoxRlCapture::GetInstance().AdmitsMilitaryEvents(static_cast<PlayerTypes>(damageRows[i].receiver))) continue;
        if (retained != i) damageRows[retained] = damageRows[i];
        ++retained;
    }
    damageRows.resize(retained);
    campCreations.erase(campCreations.begin(), campCreations.begin() + collectedCamps);
    // This segment carried all players' economic prefixes, so consume each once
    // even when its treasury belongs to a different player from the segment.
    goldTransactions.erase(goldTransactions.begin(), goldTransactions.begin() + collectedGold);
    economicBatches.erase(economicBatches.begin(), economicBatches.begin() + collectedEconomics);
    collectedEvents = collectedCamps = collectedGold = collectedEconomics = collectedDamage = 0;
}

// Skips scopes no admitted segment would carry, so they cost nothing.
VoxRlDamageScope::VoxRlDamageScope(bool enabled, int cause, PlayerTypes attacker, PlayerTypes defender) : m_open(false)
{
    m_open = enabled && HasMajorCarrier();
    if (!m_open) return;
    OpenDamageScope scope;
    scope.cause = cause;
    scope.attacker = attacker;
    scope.defender = defender;
    openDamageScopes.push_back(scope);
}

// Reads each subject's final damage and queues one row per subject for every admitted major.
// A unit that is gone or dying counts as removed, and so does a city whose plot no longer holds
// a city of its owner. Every row of the scope shares one occurrence. In a nuke, each row the
// attacker does not own names its own owner as the defender, so the struck side is a side.
VoxRlDamageScope::~VoxRlDamageScope()
{
    if (!m_open || openDamageScopes.empty()) return;
    OpenDamageScope scope;
    scope.cause = openDamageScopes.back().cause;
    scope.attacker = openDamageScopes.back().attacker;
    scope.defender = openDamageScopes.back().defender;
    scope.subjects.swap(openDamageScopes.back().subjects);
    openDamageScopes.pop_back();
    if (scope.subjects.empty()) return;
    RequestDamageRecord shared = RequestDamageRecord();
    SetOccurrence(shared);
    for (size_t i = 0; i < scope.subjects.size(); ++i)
    {
        RequestDamageRecord row = scope.subjects[i].row;
        row.occurrenceTurn = shared.occurrenceTurn;
        row.occurrenceActor = shared.occurrenceActor;
        row.occurrencePhase = shared.occurrencePhase;
        row.occurrenceOrder = shared.occurrenceOrder;
        row.cause = static_cast<u8>(scope.cause);
        row.attackerOwner = static_cast<i8>(scope.attacker);
        row.defenderOwner = static_cast<i8>(scope.cause == VOX_RL_DAMAGE_NUKE && row.owner != scope.attacker
            ? row.owner : scope.defender);
        const int owner = scope.subjects[i].owner;
        int damage = 0;
        if (row.subject == VOX_RL_DAMAGE_CITY)
        {
            const CvPlot* plot = GC.getMap().plotByIndex(scope.subjects[i].id);
            const CvCity* city = plot != NULL ? plot->getPlotCity() : NULL;
            row.removed = city == NULL || city->getOwner() != owner ? 1 : 0;
            if (!row.removed) damage = city->getDamage() < row.maxHitPoints ? city->getDamage() : row.maxHitPoints;
        }
        else
        {
            const CvUnit* unit = owner >= 0 && owner < MAX_PLAYERS
                ? GET_PLAYER(static_cast<PlayerTypes>(owner)).getUnit(scope.subjects[i].id) : NULL;
            row.removed = unit == NULL || unit->isDelayedDeath() || unit->IsDead() ? 1 : 0;
            if (!row.removed) damage = unit->getDamage() < row.maxHitPoints ? unit->getDamage() : row.maxHitPoints;
        }
        if (!VoxRlAssignChecked(row.damageAfter, row.removed ? row.maxHitPoints : damage,
            "RequestDamageRecord", "damageAfter", 0)) captureFailed = true;
        QueueDamageRow(row);
    }
}

// Counts nested supported actions without duplicating their treasury changes.
VoxRlMilitaryGoldScope::VoxRlMilitaryGoldScope(bool enabled, int cause) : m_enabled(enabled), m_previousCause(goldCause)
{
    if (m_enabled) { ++goldDepth; if (cause != 0) goldCause = cause; }
}

// Restores the enclosing military transaction classification.
VoxRlMilitaryGoldScope::~VoxRlMilitaryGoldScope()
{
    if (m_enabled) { --goldDepth; goldCause = m_previousCause; }
}

// Labels creations in the scope as upgrades, which keep lineage but record no arrival.
VoxRlMilitaryUpgradeScope::VoxRlMilitaryUpgradeScope(bool enabled) : m_enabled(enabled)
{
    if (m_enabled) ++upgradeDepth;
}

// Restores the enclosing creation classification.
VoxRlMilitaryUpgradeScope::~VoxRlMilitaryUpgradeScope()
{
    if (m_enabled) --upgradeDepth;
}
