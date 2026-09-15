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
        std::vector<RequestEventUnitMovementCountRecord> requestEventUnitMovementCounts;
        std::vector<RequestEventUnitSparseFieldRecord> requestEventUnitSparseFields;
        std::vector<RequestEventUnitModifierRecord> requestEventUnitModifiers;
        std::vector<RequestEventUnitPlagueRecord> requestEventUnitPlagues;
        std::vector<RequestEventUnitBlockedPromotionRecord> requestEventUnitBlockedPromotions;
        std::vector<RequestEventUnitAttackCountRecord> requestEventUnitAttackCounts;

        // Takes freshly collected buffers without copying capability payloads.
        void Take(VoxRlRequestData& data)
        {
            requestEventUnits.swap(data.requestEventUnits);
            requestEventUnitMovementCounts.swap(data.requestEventUnitMovementCounts);
            requestEventUnitSparseFields.swap(data.requestEventUnitSparseFields);
            requestEventUnitModifiers.swap(data.requestEventUnitModifiers);
            requestEventUnitPlagues.swap(data.requestEventUnitPlagues);
            requestEventUnitBlockedPromotions.swap(data.requestEventUnitBlockedPromotions);
            requestEventUnitAttackCounts.swap(data.requestEventUnitAttackCounts);
        }
    };
    // One immutable event-time snapshot, independent of later live unit changes.
    struct MilitaryEvent
    {
        int kind;
        int receiver;
        RequestMilitaryUpgradeRecord row;
        EventSnapshot snapshot;
        // Initializes optional references explicitly; zero is a valid native identity.
        MilitaryEvent() : kind(0), receiver(-1), row()
        {
            row.sourceOwner = row.lineageOwner = row.sourceCityOwner = row.donorPlayer = -1;
            row.sourceUnitId = row.lineageUnitId = row.sourceCityId = row.plotIndex = row.eventUnitIndex = -1;
            row.replacementOwner = -1;
            row.replacementUnitId = row.replacementUnitType = -1;
            row.sourceUnitType = -1;
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
    std::map<UnitKey, UnitKey> lineage;
    std::vector<MilitaryEvent> events;
    std::vector<RequestMilitaryGoldTransactionRecord> goldTransactions;
    std::vector<RequestEconomicBatchRecord> economicBatches;
    std::map<int, EconomicInterval> economicIntervals;
    std::map<int, MaintenanceBaseline> maintenanceBaselines;
    std::map<UnitKey, CampaignPendingTransferRecord> pendingTransfers;
    size_t collectedEvents = 0;
    size_t collectedGold = 0;
    size_t collectedEconomics = 0;
    int eventActor = -1;
    int eventPhase = 0;
    unsigned int eventOrder = 0;
    unsigned int nextTransfer = 1;
    unsigned int goldDepth = 0;
    int goldCause = 0;
    bool captureFailed = false;

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

    // Purchase and upgrade records own their charges; retain only other action effects.
    void ConsumeRecordedCharge(int owner, int costTimes100)
    {
        if (costTimes100 == 0) return;
        for (size_t i = goldTransactions.size(); i > 0; --i)
        {
            const RequestMilitaryGoldTransactionRecord& row = goldTransactions[i - 1];
            if (row.player == owner && row.amountTimes100 == -costTimes100)
            {
                goldTransactions.erase(goldTransactions.begin() + i - 1);
                return;
            }
        }
    }

    // Fills the occurrence identity before any later observer receives the event.
    template <typename Row>
    void SetOccurrence(Row& row)
    {
        SetTurn(row.occurrenceTurn, GC.getGame().getGameTurn(), "MilitaryEvent", "occurrenceTurn");
        row.occurrenceActor = static_cast<i8>(eventActor);
        row.occurrencePhase = static_cast<u8>(eventPhase);
        row.occurrenceOrder = eventOrder;
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
        if (!VoxRlAppendEventUnitSnapshot(unit, unit.getTeam(), snapshot, &event.row.eventUnitIndex)) captureFailed = true;
        event.snapshot.Take(snapshot);
        event.row.plotIndex = static_cast<i16>(unit.plot()->GetPlotIndex());
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
    Row EventRow(const RequestMilitaryUpgradeRecord& source)
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
        row.amountTimes100 = source.amountTimes100;
        row.currency = source.currency;
        row.initializationComplete = source.initializationComplete;
        return row;
    }

    // Appends snapshot children using the generated range ownership helpers.
    bool AppendSnapshot(const MilitaryEvent& event, VoxRlRequestData& data, i32& unitIndex)
    {
        if (event.snapshot.requestEventUnits.empty()) { unitIndex = -1; return true; }
        RequestEventUnitRecord row = event.snapshot.requestEventUnits.front();
        if (!AppendRequestEventUnitRecordMovementCountRange(&row, &data, event.snapshot.requestEventUnitMovementCounts) ||
            !AppendRequestEventUnitRecordSparseFieldRange(&row, &data, event.snapshot.requestEventUnitSparseFields)) return false;
        unitIndex = static_cast<i32>(data.requestEventUnits.size());
        data.requestEventUnits.push_back(row);
        return true;
    }
}

// Clears all evidence when the native attachment changes.
void VoxRlResetMilitaryEvents()
{
    lineage.clear(); events.clear(); goldTransactions.clear(); economicBatches.clear(); economicIntervals.clear(); maintenanceBaselines.clear(); pendingTransfers.clear();
    collectedEvents = collectedGold = collectedEconomics = 0;
    eventActor = -1; eventPhase = 0; eventOrder = 0; nextTransfer = 1; goldDepth = 0; goldCause = 0; captureFailed = false;
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
void VoxRlSetMilitaryEventContext(PlayerTypes actor, int phase, unsigned int order)
{
    eventActor = actor; eventPhase = phase; eventOrder = order;
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
    // Minor and barbarian arrivals have no receiving capture segment.
    if (!VoxRlCapture::GetInstance().AdmitsMilitaryEvents(unit.getOwner())) return;
    const int cause = reason == REASON_TRAIN ? VOX_RL_EVENT_PRODUCTION : reason == REASON_BUY || reason == REASON_FAITH_BUY ? VOX_RL_EVENT_PURCHASE :
        reason == REASON_GIFT ? VOX_RL_EVENT_GIFT : reason == REASON_CONVERT ? VOX_RL_EVENT_CONVERSION : reason == REASON_UPGRADE ? VOX_RL_EVENT_UPGRADE : VOX_RL_EVENT_UNKNOWN;
    events.push_back(MakeEvent(unit, 0, cause));
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
            event.row.donorPlayer = static_cast<i8>(source->getOwner());
            if (source->getOwner() != unit.getOwner() && event.row.cause == VOX_RL_EVENT_UNKNOWN)
                event.row.cause = VOX_RL_EVENT_GIFT;
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
    event->row.cause = VOX_RL_EVENT_CONVERSION; event->row.donorPlayer = static_cast<i8>(sourceOwner);
}

// Replaces the provisional upgrade arrival with a single replacement event.
void VoxRlNoteMilitaryUpgrade(CvUnit& oldUnit, CvUnit& newUnit, int costTimes100)
{
    VoxRlCompleteMilitaryUnit(newUnit, &oldUnit);
    MilitaryEvent* pending = PendingArrival(newUnit);
    if (pending != NULL)
    {
        MilitaryEvent& event = *pending;
        event.kind = 2; event.row.cause = VOX_RL_EVENT_UPGRADE;
        event.row.sourceOwner = static_cast<i8>(oldUnit.getOwner()); event.row.sourceUnitId = oldUnit.GetID();
        event.row.sourceUnitType = oldUnit.getUnitType();
        event.row.replacementOwner = static_cast<i8>(newUnit.getOwner()); event.row.replacementUnitId = newUnit.GetID();
        event.row.replacementUnitType = newUnit.getUnitType(); event.row.amountTimes100 = costTimes100; event.row.currency = VOX_RL_CURRENCY_GOLD;
        ConsumeRecordedCharge(oldUnit.getOwner(), costTimes100);
    }
}

// Completes the pending purchase with its source and actual currency charge.
void VoxRlNoteMilitaryPurchase(CvUnit& unit, int cityOwner, int cityId, int currency, int costTimes100)
{
    VoxRlCompleteMilitaryUnit(unit);
    MilitaryEvent* pending = PendingArrival(unit);
    if (pending != NULL)
    {
        MilitaryEvent& event = *pending;
        event.row.cause = VOX_RL_EVENT_PURCHASE; event.row.sourceCityOwner = static_cast<i8>(cityOwner); event.row.sourceCityId = cityId;
        event.row.currency = static_cast<u8>(currency); event.row.amountTimes100 = costTimes100;
        if (currency == VOX_RL_CURRENCY_GOLD) ConsumeRecordedCharge(unit.getOwner(), costTimes100);
    }
}

// Captures a departure before the native unit is removed from its original owner.
void VoxRlNoteMilitaryDeparture(CvUnit& unit, int receiver, bool gift)
{
    if (!VoxRlCapture::GetInstance().AdmitsMilitaryEvents(unit.getOwner())) return;
    MilitaryEvent event = MakeEvent(unit, 1, gift ? VOX_RL_EVENT_GIFT : VOX_RL_EVENT_DISBAND);
    event.row.initializationComplete = 1;
    if (gift) event.row.transferId = nextTransfer++;
    event.row.donorPlayer = static_cast<i8>(gift ? receiver : -1);
    events.push_back(event);
    // Immediate replacements share the transfer identity with their source departure.
    for (size_t i = 0; i + 1 < events.size(); ++i)
        if (events[i].kind == 0 && events[i].receiver == receiver &&
            events[i].row.lineageOwner == event.row.lineageOwner && events[i].row.lineageUnitId == event.row.lineageUnitId)
            events[i].row.transferId = event.row.transferId;
}

// Records the source before native distance-gift storage removes the unit.
void VoxRlNoteMilitaryTransferStarted(CvUnit& unit, PlayerTypes receiver, int arrivalTurn)
{
    const size_t before = events.size();
    VoxRlNoteMilitaryDeparture(unit, receiver, true);
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
        row.occurrencePhase = static_cast<u8>(eventPhase); row.occurrenceOrder = eventOrder;
        if (interval.external < (-2147483647 - 1) || interval.external > 2147483647) captureFailed = true;
        else row.externalGoldTimes100 = static_cast<i32>(interval.external);
        economicBatches.push_back(row);
    }
    interval.turn = turn; interval.external = 0;
}

// Stages event evidence separately from live upserts, retaining receiver buffers.
bool VoxRlCollectMilitaryEvents(PlayerTypes observer, VoxRlRequestData& data)
{
    if (captureFailed) return false;
    // Native actions can refresh danger before their final cost and replacement state.
    // Their evidence becomes publishable only after the enclosing action completes.
    if (goldDepth != 0)
    {
        collectedEvents = collectedGold = collectedEconomics = 0;
        return true;
    }
    collectedEvents = events.size(); collectedGold = goldTransactions.size(); collectedEconomics = economicBatches.size();
    for (size_t i = 0; i < collectedEvents; ++i)
    {
        const MilitaryEvent& event = events[i];
        if (event.receiver != observer) continue;
        RequestMilitaryUpgradeRecord row = event.row;
        if (!AppendSnapshot(event, data, row.eventUnitIndex)) return false;
        if (event.kind == 0)
        {
            RequestMilitaryArrivalRecord arrival = EventRow<RequestMilitaryArrivalRecord>(row);
            if (!AppendRequestMilitaryArrivalRecordModifierRange(&arrival, &data, event.snapshot.requestEventUnitModifiers) ||
                !AppendRequestMilitaryArrivalRecordPlagueRange(&arrival, &data, event.snapshot.requestEventUnitPlagues) ||
                !AppendRequestMilitaryArrivalRecordBlockedPromotionRange(&arrival, &data, event.snapshot.requestEventUnitBlockedPromotions) ||
                !AppendRequestMilitaryArrivalRecordAttackCountRange(&arrival, &data, event.snapshot.requestEventUnitAttackCounts)) return false;
            data.requestMilitaryArrivals.push_back(arrival);
        }
        else if (event.kind == 1)
        {
            RequestMilitaryDepartureRecord departure = EventRow<RequestMilitaryDepartureRecord>(row);
            if (!AppendRequestMilitaryDepartureRecordModifierRange(&departure, &data, event.snapshot.requestEventUnitModifiers) ||
                !AppendRequestMilitaryDepartureRecordPlagueRange(&departure, &data, event.snapshot.requestEventUnitPlagues) ||
                !AppendRequestMilitaryDepartureRecordBlockedPromotionRange(&departure, &data, event.snapshot.requestEventUnitBlockedPromotions) ||
                !AppendRequestMilitaryDepartureRecordAttackCountRange(&departure, &data, event.snapshot.requestEventUnitAttackCounts)) return false;
            data.requestMilitaryDepartures.push_back(departure);
        }
        else
        {
            if (!AppendRequestMilitaryUpgradeRecordModifierRange(&row, &data, event.snapshot.requestEventUnitModifiers) ||
                !AppendRequestMilitaryUpgradeRecordPlagueRange(&row, &data, event.snapshot.requestEventUnitPlagues) ||
                !AppendRequestMilitaryUpgradeRecordBlockedPromotionRange(&row, &data, event.snapshot.requestEventUnitBlockedPromotions) ||
                !AppendRequestMilitaryUpgradeRecordAttackCountRange(&row, &data, event.snapshot.requestEventUnitAttackCounts)) return false;
            data.requestMilitaryUpgrades.push_back(row);
        }
    }
    // Economic evidence belongs to the shared game timeline. The next admitted
    // segment carries every player's rows; row.player identifies the affected treasury.
    // Unit arrivals and departures instead wait for their receiving observer above.
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
        if (i < collectedEvents && events[i].receiver == observer) continue;
        if (retained != i) events[retained] = events[i];
        ++retained;
    }
    events.resize(retained);
    // This segment carried all players' economic prefixes, so consume each once
    // even when its treasury belongs to a different player from the segment.
    goldTransactions.erase(goldTransactions.begin(), goldTransactions.begin() + collectedGold);
    economicBatches.erase(economicBatches.begin(), economicBatches.begin() + collectedEconomics);
    collectedEvents = collectedGold = collectedEconomics = 0;
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
