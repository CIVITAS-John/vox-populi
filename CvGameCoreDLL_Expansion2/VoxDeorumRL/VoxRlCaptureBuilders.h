// Installed from simulator/capture/VoxRlCaptureBuilders.h. Edit that source, then reinstall.

// Vox Deorum: recording capture block builders. Builders stage rows from the
// generated collectors and hand-written loops, then write complete framed
// blocks through the shared writer in canonical section order. They perform
// no file I/O. Zone tables are always read without triggering a refresh.
#ifndef VOX_RL_CAPTURE_BUILDERS_H
#define VOX_RL_CAPTURE_BUILDERS_H

#include "VoxDeorumRL/schema/VoxRlBuilders.generated.h"
#include <cstring>
#include <vector>

// Retains a no-refresh tactical zone table and its per-plot assignments.
struct VoxRlZoneSnapshot
{
	std::vector<ZoneRecord> zones;
	std::vector<ZoneNeighborRecord> neighbors;
	std::vector<i32> plotZones;
};

// Optional WORLD construction timings and row volumes. A null pointer at the
// builder boundary skips every clock read and counter update.
struct VoxRlWorldBuildTimings
{
	unsigned __int64 ownerIterationNs;
	unsigned __int64 dangerSparseRelationsNs;
	unsigned __int64 zoneNs;
	unsigned __int64 plotUnitNs;
	unsigned __int64 visibilityNs;
	unsigned __int64 entityRelationNs;
	unsigned __int64 serializeNs;
	unsigned int plotCount;
	unsigned int unitCount;
	unsigned int cityCount;
	unsigned int alivePlayerCount;
	unsigned int aliveTeamCount;
	// Initializes every optional phase and volume to zero.
	VoxRlWorldBuildTimings()
		: ownerIterationNs(0), dangerSparseRelationsNs(0), zoneNs(0),
		plotUnitNs(0), visibilityNs(0), entityRelationNs(0), serializeNs(0),
		plotCount(0), unitCount(0), cityCount(0), alivePlayerCount(0), aliveTeamCount(0)
	{
	}
};

// Collects current zone records without triggering native map preparation.
bool VoxRlCollectZones(class CvTacticalAnalysisMap* zoneMap, VoxRlZoneSnapshot& snapshot);

// Reserves storage and builds one complete STATIC block from the global
// defines, info tables, and map topology.
bool VoxRlBuildStaticBlock(const VoxRlBlockIdentity& identity,
	VoxRlOwnedBlockStorage& storage, unsigned int& length);

// Builds one complete WORLD block at the pre-refresh checkpoint for the
// capturing player.
bool VoxRlBuildWorldBlock(const VoxRlBlockIdentity& identity, PlayerTypes capturingPlayer,
	VoxRlOwnedBlockStorage& storage, unsigned int& length, VoxRlZoneSnapshot& zones,
	std::vector<TeamPassabilityRecord>& teamPassabilitySnapshot,
	VoxRlWorldBuildTimings* timings = NULL);

// Builds one complete CAMPAIGN block at the UpdateOperations entry for the
// capturing player. Zone data is read without triggering a refresh.
bool VoxRlBuildCampaignBlock(const VoxRlBlockIdentity& identity, PlayerTypes capturingPlayer,
	VoxRlOwnedBlockStorage& storage, unsigned int& length);

// Builds one complete REQUEST block from staged rows.
bool VoxRlBuildRequestBlock(const VoxRlBlockIdentity& identity, VoxRlRequestData& data,
	VoxRlOwnedBlockStorage& storage, unsigned int& length);

// Collects one unit's sparse indexed combat modifiers into the family
// records. Shared by the WORLD builder and sparse replacement flushes.
bool VoxRlCollectUnitModifierRows(PlayerTypes eOwner, int iUnitId, class CvUnit* pUnit,
	std::vector<UnitModifierRecord>& rows);

// Collects one unit's plagues and blocked promotions into the sparse family
// records.
void VoxRlCollectUnitPlagueRows(PlayerTypes eOwner, int iUnitId, class CvUnit* pUnit,
	std::vector<UnitPlagueRecord>& plagues, std::vector<UnitBlockedPromotionRecord>& blockedPromotions);
// Collects one unit's per-attacking-player counts into the sparse family
// records.
void VoxRlCollectUnitAttackCountRows(PlayerTypes eOwner, int iUnitId, class CvUnit* pUnit,
	std::vector<UnitAttackCountRecord>& rows);
void VoxRlCollectCityAttackCountRows(class CvCity* pCity, std::vector<CityAttackCountRecord>& rows);
void VoxRlCollectPlayerResistanceRows(class CvPlayer* pPlayer, std::vector<PlayerResistanceRecord>& rows);

// Collects the generated city fields and the builder-owned snapshots shared
// by WORLD builds and sparse city replacements.
bool VoxRlCollectCityRecord(class CvCity& city, PlayerTypes capturingPlayer, CityRecord& row);

// Collects the generated unit fields and their builder-owned reduced values shared by
// WORLD builds and sparse unit replacements. The movement-count vector receives the
// unit's nonzero terrain and feature extra-move counts in table order; the caller binds
// them into the owning section through the generated range helper.
bool VoxRlCollectUnitRecord(class CvUnit& unit, TeamTypes capturingTeam, UnitRecord& row,
	std::vector<UnitMovementCountRecord>& movementCounts);

// Collects terrain and feature impassability for every team, including the
// barbarian team, from the info tables and each team's researched technology.
bool VoxRlCollectTeamPassabilityRows(std::vector<TeamPassabilityRecord>& rows);

// Collects a complete sparse family across all live units or cities, used by
// WORLD builds and complete-family replacement requests.
bool VoxRlCollectAllUnitModifierRows(std::vector<UnitModifierRecord>& rows);
void VoxRlCollectAllUnitPlagueRows(std::vector<UnitPlagueRecord>& plagues,
	std::vector<UnitBlockedPromotionRecord>& blockedPromotions);
void VoxRlCollectAllUnitAttackCountRows(std::vector<UnitAttackCountRecord>& rows);
void VoxRlCollectAllCityAttackCountRows(std::vector<CityAttackCountRecord>& rows);
void VoxRlCollectAllPlayerResistanceRows(std::vector<PlayerResistanceRecord>& rows);

struct STacticalAssignment;

// Copies collected WORLD-shaped rows into their mirrored request record
// shapes. The zone tables and six sparse replacement families (unit modifiers, plagues,
// blocked promotions, unit attack counts, player resistances, city attack
// counts) mirror their WORLD counterparts field for field; keyed delta
// mirrors such as the plot delta are not layout-identical and must not use
// this helper. The built-in size check rejects any other pairing at compile
// time.
template <typename To, typename From>
void VoxRlMirrorSparseRows(const std::vector<From>& from, std::vector<To>& to)
{
	typedef char VoxRlAssertMirrorSize[(sizeof(To) == sizeof(From)) ? 1 : -1];
	(void)sizeof(VoxRlAssertMirrorSize);
	to.resize(from.size());
	for (size_t index = 0; index < from.size(); ++index)
	{
		std::memcpy(&to[index], &from[index], sizeof(To));
	}
}

// Resolves the owner of a bare damage-ledger unit id following the item 7
// replay disposition: the smallest non-acting owner that has the unit, and
// the acting player when nobody else does.
int VoxRlResolveDamagedUnitOwner(PlayerTypes actingPlayer, int unitId);
// Resolves the owner of a damaged city through the assignment's target plot
// first, then by the smallest owner that has the city id.
int VoxRlResolveDamagedCityOwner(PlayerTypes actingPlayer, const STacticalAssignment& assignment);

// Collects native assignment results and appends their child damage and
// healing rows through the generated range helpers.
bool VoxRlCollectAssignmentRows(PlayerTypes actingPlayer,
	const std::vector<STacticalAssignment>& assignments, VoxRlResultData& data);

#endif
