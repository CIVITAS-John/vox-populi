// Installed from simulator/capture/VoxRlCaptureBuilders.h. Edit that source, then reinstall.

// Vox Deorum: recording capture block builders. Builders stage rows from the
// generated collectors and hand-written loops, then write complete framed
// blocks through the shared writer in canonical section order. They perform
// no file I/O and never mutate game state except through documented lazy
// native caches (zone and citadel preparation).
#ifndef VOX_RL_CAPTURE_BUILDERS_H
#define VOX_RL_CAPTURE_BUILDERS_H

#include "VoxDeorumRL/schema/VoxRlBuilders.generated.h"
#include <vector>

// Reserves storage and builds one complete STATIC block from the global
// defines, info tables, and map topology.
bool VoxRlBuildStaticBlock(const VoxRlBlockIdentity& identity,
	VoxRlOwnedBlockStorage& storage, unsigned int& length);

// Builds one complete WORLD block at the pre-refresh checkpoint for the
// capturing player.
bool VoxRlBuildWorldBlock(const VoxRlBlockIdentity& identity, PlayerTypes capturingPlayer,
	VoxRlOwnedBlockStorage& storage, unsigned int& length);

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

// Collects the generated unit fields and their builder-owned reduced values
// shared by WORLD builds and sparse unit replacements.
bool VoxRlCollectUnitRecord(class CvUnit& unit, TeamTypes capturingTeam, UnitRecord& row);

// Collects generated dynamic plot fields and the current tactical zone id
// shared by WORLD builds and sparse plot replacements.
bool VoxRlCollectPlotDynamicRecord(class CvPlot& plot, class CvTacticalAnalysisMap* zoneMap,
	PlotDynamicRecord& row);

// Collects a complete sparse family across all live units or cities, used by
// WORLD builds and complete-family replacement requests.
bool VoxRlCollectAllUnitModifierRows(std::vector<UnitModifierRecord>& rows);
void VoxRlCollectAllUnitPlagueRows(std::vector<UnitPlagueRecord>& plagues,
	std::vector<UnitBlockedPromotionRecord>& blockedPromotions);
void VoxRlCollectAllUnitAttackCountRows(std::vector<UnitAttackCountRecord>& rows);
void VoxRlCollectAllCityAttackCountRows(std::vector<CityAttackCountRecord>& rows);
void VoxRlCollectAllPlayerResistanceRows(std::vector<PlayerResistanceRecord>& rows);

struct STacticalAssignment;

// Copies collected WORLD-shaped sparse rows into their mirrored request
// record shapes. The pairs are layout-identical by schema construction.
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
