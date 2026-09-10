// Installed from simulator/capture/VoxRlCaptureBuilders.cpp. Edit that source, then reinstall.

// Vox Deorum: recording capture block builders. See VoxRlCaptureBuilders.h.
#include "CvGameCoreDLLPCH.h"
#include "VoxDeorumRL/VoxRlCaptureBuilders.h"
#include "schema/VoxRlCollectors.generated.h"

#include "VoxDeorumRL/VoxRlCapture.h"
#include "CvDangerPlots.h"
#include "CvDiplomacyAI.h"
#include "CvMilitaryAI.h"
#include "CvTacticalAnalysisMap.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{
	// Zero-initializes one packed record so padding bytes and unset fields
	// serialize deterministically.
	template <typename Record>
	void ZeroRecord(Record& record)
	{
		std::memset(&record, 0, sizeof(Record));
	}

	// Collects the alive team ids in ascending order.
	void CollectAliveTeams(std::vector<TeamTypes>& teams)
	{
		for (int team = 0; team < MAX_TEAMS; ++team)
		{
			if (GET_TEAM(static_cast<TeamTypes>(team)).isAlive())
			{
				teams.push_back(static_cast<TeamTypes>(team));
			}
		}
	}

	// Collects the alive player ids in ascending order.
	void CollectAlivePlayers(std::vector<PlayerTypes>& players)
	{
		for (int player = 0; player < MAX_PLAYERS; ++player)
		{
			if (GET_PLAYER(static_cast<PlayerTypes>(player)).isAlive())
			{
				players.push_back(static_cast<PlayerTypes>(player));
			}
		}
	}

	// Returns the largest value in a unit's yield-indexed kill bonus array,
	// matching the reduced representation consumed by the replay facade.
	int MaxYieldFromKills(const CvUnitEntry& source, bool barbarian)
	{
		int maximum = 0;
		for (int yield = 0; yield < NUM_YIELD_TYPES; ++yield)
		{
			const int value = barbarian
				? source.GetYieldFromBarbarianKills(static_cast<YieldTypes>(yield))
				: source.GetYieldFromKills(static_cast<YieldTypes>(yield));
			if (value > maximum) maximum = value;
		}
		return maximum;
	}

	// Returns the largest value in a live unit's yield-indexed kill bonus
	// array, using the lowercase native unit accessors.
	int MaxYieldFromKills(const CvUnit& source, bool barbarian)
	{
		int maximum = 0;
		for (int yield = 0; yield < NUM_YIELD_TYPES; ++yield)
		{
			const int value = barbarian
				? source.getYieldFromBarbarianKills(static_cast<YieldTypes>(yield))
				: source.getYieldFromKills(static_cast<YieldTypes>(yield));
			if (value > maximum) maximum = value;
		}
		return maximum;
	}

}

// Resolves the owner of a bare damage-ledger unit id following the item 7
// replay disposition: the smallest non-acting owner that has the unit, and
// the acting player when nobody else does.
int VoxRlResolveDamagedUnitOwner(PlayerTypes actingPlayer, int unitId)
{
	if (unitId < 0)
	{
		return static_cast<int>(actingPlayer);
	}
	int owner = static_cast<int>(actingPlayer);
	for (int candidate = 0; candidate < MAX_PLAYERS; ++candidate)
	{
		if (candidate == static_cast<int>(actingPlayer)) continue;
		CvUnit* unit = GET_PLAYER(static_cast<PlayerTypes>(candidate)).getUnit(unitId);
		if (unit == NULL) continue;
		if (owner == static_cast<int>(actingPlayer) || candidate < owner)
		{
			owner = candidate;
		}
	}
	return owner;
}

// Resolves the owner of a damaged city through the assignment's target plot
// first, then by the smallest owner that has the city id.
int VoxRlResolveDamagedCityOwner(PlayerTypes actingPlayer, const STacticalAssignment& assignment)
{
	if (assignment.iDamagedCityId < 0)
	{
		return -1;
	}
	CvPlot* targetPlot = GC.getMap().plotByIndex(assignment.iToPlotIndex);
	if (targetPlot != NULL)
	{
		CvCity* plotCity = targetPlot->getPlotCity();
		if (plotCity != NULL && plotCity->GetID() == assignment.iDamagedCityId)
		{
			return static_cast<int>(plotCity->getOwner());
		}
	}
	int owner = -1;
	for (int candidate = 0; candidate < MAX_PLAYERS; ++candidate)
	{
		if (GET_PLAYER(static_cast<PlayerTypes>(candidate)).getCity(assignment.iDamagedCityId) != NULL)
		{
			owner = candidate;
			break;
		}
	}
	return owner;
}

// Collects result rows in native order and lets generated helpers own their
// owner-relative child ranges.
bool VoxRlCollectAssignmentRows(PlayerTypes actingPlayer,
	const std::vector<STacticalAssignment>& assignments, VoxRlResultData& data)
{
	data.resultAssignments.clear();
	data.resultAssignmentDamage.clear();
	data.resultAssignmentHealing.clear();
	for (size_t index = 0; index < assignments.size(); ++index)
	{
		const STacticalAssignment& assignment = assignments[index];
		ResultAssignmentRecord row;
		ZeroRecord(row);
		row.unitId = static_cast<i32>(assignment.iUnitID);
		row.unitOwner = static_cast<i8>(actingPlayer);
		row.assignmentType = static_cast<u8>(assignment.eAssignmentType);
		row.moveType = static_cast<i8>(assignment.eMoveType);
		row.totalScore = static_cast<i16>(assignment.iTotalScore);
		row.fromPlotIndex = static_cast<i16>(assignment.iFromPlotIndex);
		row.toPlotIndex = static_cast<i16>(assignment.iToPlotIndex);
		row.remainingMoves = static_cast<i16>(assignment.iRemainingMoves);
		row.cityDamage = static_cast<i32>(assignment.iCityDamage);
		row.selfDamage = static_cast<i32>(assignment.iSelfDamage);
		row.damagedCityId = static_cast<i32>(assignment.iDamagedCityId);
		row.damagedCityOwner = static_cast<i8>(VoxRlResolveDamagedCityOwner(actingPlayer, assignment));
		std::vector<ResultAssignmentDamageRecord> damageRows;
		for (SUnitIDValueContainer::const_iterator entry = assignment.unitDamage.begin();
			entry != assignment.unitDamage.end(); ++entry)
		{
			ResultAssignmentDamageRecord damage;
			ZeroRecord(damage);
			damage.owner = static_cast<i8>(VoxRlResolveDamagedUnitOwner(actingPlayer, (*entry).first));
			damage.unitId = static_cast<i32>((*entry).first);
			damage.value = static_cast<i32>((*entry).second);
			damageRows.push_back(damage);
		}
		std::vector<ResultAssignmentHealingRecord> healingRows;
		for (SUnitIDValueContainer::const_iterator entry = assignment.unitHealing.begin();
			entry != assignment.unitHealing.end(); ++entry)
		{
			ResultAssignmentHealingRecord healing;
			ZeroRecord(healing);
			healing.owner = static_cast<i8>(actingPlayer);
			healing.unitId = static_cast<i32>((*entry).first);
			healing.value = static_cast<i32>((*entry).second);
			healingRows.push_back(healing);
		}
		if (!AppendResultAssignmentRecordUnitDamageRange(&row, &data, damageRows) ||
			!AppendResultAssignmentRecordUnitHealingRange(&row, &data, healingRows)) return false;
		data.resultAssignments.push_back(row);
	}
	return true;
}

// Collects one unit's modifiers and rejects schema capacities that cannot fit.
bool VoxRlCollectUnitModifierRows(PlayerTypes eOwner, int iUnitId, CvUnit* pUnit,
	std::vector<UnitModifierRecord>& rows)
{
	if (pUnit == NULL) return true;
	// Every family indexes a fixed-capacity record array on the reader side,
	// so a live table larger than its capacity fails the build instead of
	// emitting rows the loader would reject.
	if (GC.getNumTerrainInfos() > VoxRlTerrainCapacity ||
		GC.getNumFeatureInfos() > VoxRlFeatureCapacity ||
		GC.getNumUnitClassInfos() > VoxRlUnitclassCapacity ||
		GC.getNumUnitCombatClassInfos() > VoxRlUnitcombatCapacity ||
		NUM_DOMAIN_TYPES > VoxRlDomainCapacity) return false;
	UnitModifierRecord row;
	ZeroRecord(row);
	row.owner = static_cast<i8>(eOwner);
	row.unitId = static_cast<i32>(iUnitId);

	// Rows follow the manifest family order: terrain attack and defense,
	// the VP terrain attack and defense, feature attack and defense.
	const int terrainCount = GC.getNumTerrainInfos();
	for (int index = 0; index < terrainCount; ++index)
	{
		const TerrainTypes terrain = static_cast<TerrainTypes>(index);
		const int attack = pUnit->getExtraTerrainAttackPercent(terrain);
		const int defense = pUnit->getExtraTerrainDefensePercent(terrain);
		const int vpAttack = pUnit->GetTerrainModifierAttack(terrain);
		const int vpDefense = pUnit->GetTerrainModifierDefense(terrain);
		if (attack != 0) { row.family = 0; row.index = index; row.value = attack; rows.push_back(row); }
		if (defense != 0) { row.family = 1; row.index = index; row.value = defense; rows.push_back(row); }
		if (vpAttack != 0) { row.family = 2; row.index = index; row.value = vpAttack; rows.push_back(row); }
		if (vpDefense != 0) { row.family = 3; row.index = index; row.value = vpDefense; rows.push_back(row); }
	}
	const int featureCount = GC.getNumFeatureInfos();
	for (int index = 0; index < featureCount; ++index)
	{
		const FeatureTypes feature = static_cast<FeatureTypes>(index);
		const int attack = pUnit->getExtraFeatureAttackPercent(feature);
		const int defense = pUnit->getExtraFeatureDefensePercent(feature);
		if (attack != 0) { row.family = 4; row.index = index; row.value = attack; rows.push_back(row); }
		if (defense != 0) { row.family = 5; row.index = index; row.value = defense; rows.push_back(row); }
	}
	// Unit class, unit class attack and defense.
	const int unitClassCount = GC.getNumUnitClassInfos();
	for (int index = 0; index < unitClassCount; ++index)
	{
		const UnitClassTypes unitClass = static_cast<UnitClassTypes>(index);
		const int generic = pUnit->getUnitClassModifier(unitClass);
		const int attack = pUnit->getUnitClassAttackMod(unitClass);
		const int defense = pUnit->getUnitClassDefenseMod(unitClass);
		if (generic != 0) { row.family = 6; row.index = index; row.value = generic; rows.push_back(row); }
		if (attack != 0) { row.family = 7; row.index = index; row.value = attack; rows.push_back(row); }
		if (defense != 0) { row.family = 8; row.index = index; row.value = defense; rows.push_back(row); }
	}
	// Unit combat, unit combat attack and defense.
	const int combatCount = GC.getNumUnitCombatClassInfos();
	for (int index = 0; index < combatCount; ++index)
	{
		const UnitCombatTypes combat = static_cast<UnitCombatTypes>(index);
		const int generic = pUnit->getExtraUnitCombatModifier(combat);
		const int attack = pUnit->getExtraUnitCombatModifierAttack(combat);
		const int defense = pUnit->getExtraUnitCombatModifierDefense(combat);
		if (generic != 0) { row.family = 9; row.index = index; row.value = generic; rows.push_back(row); }
		if (attack != 0) { row.family = 10; row.index = index; row.value = attack; rows.push_back(row); }
		if (defense != 0) { row.family = 11; row.index = index; row.value = defense; rows.push_back(row); }
	}
	// Domain, domain attack and defense.
	const int domainCount = NUM_DOMAIN_TYPES;
	for (int index = 0; index < domainCount; ++index)
	{
		const DomainTypes domain = static_cast<DomainTypes>(index);
		const int generic = pUnit->getExtraDomainModifier(domain);
		const int attack = pUnit->getExtraDomainAttack(domain);
		const int defense = pUnit->getExtraDomainDefense(domain);
		if (generic != 0) { row.family = 12; row.index = index; row.value = generic; rows.push_back(row); }
		if (attack != 0) { row.family = 13; row.index = index; row.value = attack; rows.push_back(row); }
		if (defense != 0) { row.family = 14; row.index = index; row.value = defense; rows.push_back(row); }
	}
	// The three per-adjacent-combat-class families close the manifest order.
	for (int index = 0; index < combatCount; ++index)
	{
		const UnitCombatTypes combat = static_cast<UnitCombatTypes>(index);
		const int adjacent = pUnit->getCombatModPerAdjacentUnitCombatModifier(combat);
		const int adjacentAttack = pUnit->getCombatModPerAdjacentUnitCombatAttackMod(combat);
		const int adjacentDefense = pUnit->getCombatModPerAdjacentUnitCombatDefenseMod(combat);
		if (adjacent != 0) { row.family = 15; row.index = index; row.value = adjacent; rows.push_back(row); }
		if (adjacentAttack != 0) { row.family = 16; row.index = index; row.value = adjacentAttack; rows.push_back(row); }
		if (adjacentDefense != 0) { row.family = 17; row.index = index; row.value = adjacentDefense; rows.push_back(row); }
	}
	return true;
}

// Collects a unit's plague and blocked-promotion rows for sparse capture.
void VoxRlCollectUnitPlagueRows(PlayerTypes eOwner, int iUnitId, CvUnit* pUnit,
	std::vector<UnitPlagueRecord>& plagues, std::vector<UnitBlockedPromotionRecord>& blockedPromotions)
{
	if (pUnit == NULL) return;
	const std::vector<PlagueInfo>& toInflict = pUnit->GetPlaguesToInflict();
	for (size_t index = 0; index < toInflict.size(); ++index)
	{
		UnitPlagueRecord row;
		ZeroRecord(row);
		row.owner = static_cast<i8>(eOwner);
		row.unitId = static_cast<i32>(iUnitId);
		row.plague = static_cast<i32>(toInflict[index].ePlague);
		row.domain = static_cast<i8>(toInflict[index].eDomain);
		row.applyOnAttack = toInflict[index].bApplyOnAttack ? 1 : 0;
		row.applyOnDefense = toInflict[index].bApplyOnDefense ? 1 : 0;
		row.applyChance = static_cast<i32>(toInflict[index].iApplyChance);
		plagues.push_back(row);
	}
	const int promotionCount = GC.getNumPromotionInfos();
	for (int promotion = 0; promotion < promotionCount; ++promotion)
	{
		if (!pUnit->IsPromotionBlocked(static_cast<PromotionTypes>(promotion))) continue;
		UnitBlockedPromotionRecord row;
		ZeroRecord(row);
		row.owner = static_cast<i8>(eOwner);
		row.unitId = static_cast<i32>(iUnitId);
		row.promotion = promotion;
		blockedPromotions.push_back(row);
	}
}

// Collects a unit's nonzero per-attacking-player counts for sparse capture rows.
void VoxRlCollectUnitAttackCountRows(PlayerTypes eOwner, int iUnitId, CvUnit* pUnit,
	std::vector<UnitAttackCountRecord>& rows)
{
	if (pUnit == NULL) return;
	for (int player = 0; player < MAX_PLAYERS; ++player)
	{
		const int count = pUnit->GetNumTimesAttackedThisTurn(static_cast<PlayerTypes>(player));
		if (count == 0) continue;
		UnitAttackCountRecord row;
		ZeroRecord(row);
		row.owner = static_cast<i8>(eOwner);
		row.unitId = static_cast<i32>(iUnitId);
		row.attackingPlayer = static_cast<i8>(player);
		row.count = static_cast<i32>(count);
		rows.push_back(row);
	}
}

void VoxRlCollectCityAttackCountRows(CvCity* pCity, std::vector<CityAttackCountRecord>& rows)
{
	if (pCity == NULL) return;
	for (int player = 0; player < MAX_PLAYERS; ++player)
	{
		const int count = pCity->GetNumTimesAttackedThisTurn(static_cast<PlayerTypes>(player));
		if (count == 0) continue;
		CityAttackCountRecord row;
		ZeroRecord(row);
		row.cityOwner = static_cast<i8>(pCity->getOwner());
		row.cityId = static_cast<i32>(pCity->GetID());
		row.attackingPlayer = static_cast<i8>(player);
		row.count = static_cast<i32>(count);
		rows.push_back(row);
	}
}

void VoxRlCollectPlayerResistanceRows(CvPlayer* pPlayer, std::vector<PlayerResistanceRecord>& rows)
{
	if (pPlayer == NULL) return;
	for (int opponent = 0; opponent < MAX_PLAYERS; ++opponent)
	{
		if (opponent == static_cast<int>(pPlayer->GetID())) continue;
		const int resistance = pPlayer->GetDominationResistance(static_cast<PlayerTypes>(opponent));
		if (resistance == 0) continue;
		PlayerResistanceRecord row;
		ZeroRecord(row);
		row.player = static_cast<i8>(pPlayer->GetID());
		row.opponent = static_cast<i8>(opponent);
		row.dominationResistance = static_cast<i32>(resistance);
		rows.push_back(row);
	}
}

bool VoxRlCollectCityRecord(CvCity& city, PlayerTypes capturingPlayer, CityRecord& row)
{
	if (!CollectCityRecord(city, capturingPlayer, row)) return false;
	row.strengthValueRanged = static_cast<i32>(city.getStrengthValueRanged());
	row.cityBeliefRangeStrikeModifier = static_cast<i32>(city.GetCityBeliefRangeStrikeModifier());
	return true;
}

bool VoxRlCollectUnitRecord(CvUnit& unit, TeamTypes capturingTeam, UnitRecord& row,
	std::vector<UnitMovementCountRecord>& movementCounts)
{
	if (!CollectUnitRecord(unit, capturingTeam, row)) return false;
	// The passability arrays are builder-owned fixed-capacity fields, so the
	// builder guards the live table sizes itself instead of relying on the
	// generated collector's guards for other unit fields.
	if (GC.getNumTerrainInfos() > VoxRlTerrainCapacity ||
		GC.getNumFeatureInfos() > VoxRlFeatureCapacity) return false;
	row.yieldFromKills = static_cast<i32>(MaxYieldFromKills(unit, false));
	row.yieldFromBarbarianKills = static_cast<i32>(MaxYieldFromKills(unit, true));
	// Sparse movement counts carry only the nonzero per-terrain and per-feature values.
	movementCounts.clear();
	for (int terrain = 0; terrain < GC.getNumTerrainInfos(); ++terrain)
	{
		const int count = unit.getTerrainExtraMoveCount(static_cast<TerrainTypes>(terrain));
		if (count == 0) continue;
		UnitMovementCountRecord entry;
		ZeroRecord(entry);
		entry.kind = 0;
		entry.typeIndex = static_cast<u8>(terrain);
		entry.count = static_cast<i32>(count);
		movementCounts.push_back(entry);
	}
	for (int feature = 0; feature < GC.getNumFeatureInfos(); ++feature)
	{
		const int count = unit.getFeatureExtraMoveCount(static_cast<FeatureTypes>(feature));
		if (count == 0) continue;
		UnitMovementCountRecord entry;
		ZeroRecord(entry);
		entry.kind = 1;
		entry.typeIndex = static_cast<u8>(feature);
		entry.count = static_cast<i32>(count);
		movementCounts.push_back(entry);
	}
	// The promotion passability tables pack one bit per terrain or feature index.
	row.setHasAllowTerrainPassable(unit.GetPromotions().HasAllowTerrainPassable());
	if (row.hasAllowTerrainPassable())
	{
		for (int terrain = 0; terrain < GC.getNumTerrainInfos(); ++terrain)
		{
			if (unit.GetPromotions().GetAllowTerrainPassable(
				static_cast<TerrainTypes>(terrain), unit.getTeam()))
				row.allowTerrainPassable[terrain / 8] = static_cast<u8>(row.allowTerrainPassable[terrain / 8] | (1 << (terrain % 8)));
		}
	}
	row.setHasAllowFeaturePassable(unit.GetPromotions().HasAllowFeaturePassable());
	if (row.hasAllowFeaturePassable())
	{
		for (int feature = 0; feature < GC.getNumFeatureInfos(); ++feature)
		{
			if (unit.GetPromotions().GetAllowFeaturePassable(
				static_cast<FeatureTypes>(feature), unit.getTeam()))
				row.allowFeaturePassable[feature / 8] = static_cast<u8>(row.allowFeaturePassable[feature / 8] | (1 << (feature % 8)));
		}
	}
	return true;
}

// Collects the compact team-specific type tables that CvPlot::updateImpassable
// applies when a terrain or feature defines a prerequisite-passable technology.
bool VoxRlCollectTeamPassabilityRows(std::vector<TeamPassabilityRecord>& rows)
{
	if (GC.getNumTerrainInfos() > VoxRlTerrainCapacity ||
		GC.getNumFeatureInfos() > VoxRlFeatureCapacity) return false;
	rows.clear();
	rows.reserve(MAX_TEAMS);
	for (int teamIndex = 0; teamIndex < MAX_TEAMS; ++teamIndex)
	{
		const TeamTypes teamType = static_cast<TeamTypes>(teamIndex);
		CvTeam& team = GET_TEAM(teamType);
		TeamPassabilityRecord row;
		ZeroRecord(row);
		row.team = static_cast<i8>(teamType);
		for (int terrainIndex = 0; terrainIndex < GC.getNumTerrainInfos(); ++terrainIndex)
		{
			CvTerrainInfo* info = GC.getTerrainInfo(static_cast<TerrainTypes>(terrainIndex));
			if (info == NULL) return false;
			if (!info->isImpassable()) continue;
			const TechTypes prerequisite = static_cast<TechTypes>(info->GetPrereqPassable());
			row.terrainImpassable[terrainIndex] = prerequisite == NO_TECH ||
				!team.GetTeamTechs()->HasTech(prerequisite) ? 1 : 0;
		}
		for (int featureIndex = 0; featureIndex < GC.getNumFeatureInfos(); ++featureIndex)
		{
			CvFeatureInfo* info = GC.getFeatureInfo(static_cast<FeatureTypes>(featureIndex));
			if (info == NULL) return false;
			if (!info->isImpassable()) continue;
			const TechTypes prerequisite = static_cast<TechTypes>(info->GetPrereqPassable());
			row.featureImpassable[featureIndex] = prerequisite == NO_TECH ||
				!team.GetTeamTechs()->HasTech(prerequisite) ? 1 : 0;
		}
		rows.push_back(row);
	}
	return true;
}

// Collects every live unit's modifiers for a complete sparse replacement.
bool VoxRlCollectAllUnitModifierRows(std::vector<UnitModifierRecord>& rows)
{
	int loop = 0;
	for (int player = 0; player < MAX_PLAYERS; ++player)
	{
		CvPlayerAI& owner = GET_PLAYER(static_cast<PlayerTypes>(player));
		loop = 0;
		for (CvUnit* pUnit = owner.firstUnit(&loop); pUnit != NULL; pUnit = owner.nextUnit(&loop))
		{
			if (pUnit->isDelayedDeath()) continue;
			if (!VoxRlCollectUnitModifierRows(owner.GetID(), pUnit->GetID(), pUnit, rows)) return false;
		}
	}
	return true;
}

// Collects all live units' plague and blocked-promotion rows for sparse capture.
void VoxRlCollectAllUnitPlagueRows(std::vector<UnitPlagueRecord>& plagues,
	std::vector<UnitBlockedPromotionRecord>& blockedPromotions)
{
	int loop = 0;
	for (int player = 0; player < MAX_PLAYERS; ++player)
	{
		CvPlayerAI& owner = GET_PLAYER(static_cast<PlayerTypes>(player));
		loop = 0;
		for (CvUnit* pUnit = owner.firstUnit(&loop); pUnit != NULL; pUnit = owner.nextUnit(&loop))
		{
			if (pUnit->isDelayedDeath()) continue;
			VoxRlCollectUnitPlagueRows(owner.GetID(), pUnit->GetID(), pUnit, plagues, blockedPromotions);
		}
	}
}

// Collects all live units' per-attacking-player counts for sparse capture rows.
void VoxRlCollectAllUnitAttackCountRows(std::vector<UnitAttackCountRecord>& rows)
{
	int loop = 0;
	for (int player = 0; player < MAX_PLAYERS; ++player)
	{
		CvPlayerAI& owner = GET_PLAYER(static_cast<PlayerTypes>(player));
		loop = 0;
		for (CvUnit* pUnit = owner.firstUnit(&loop); pUnit != NULL; pUnit = owner.nextUnit(&loop))
		{
			if (pUnit->isDelayedDeath()) continue;
			VoxRlCollectUnitAttackCountRows(owner.GetID(), pUnit->GetID(), pUnit, rows);
		}
	}
}

void VoxRlCollectAllCityAttackCountRows(std::vector<CityAttackCountRecord>& rows)
{
	int loop = 0;
	for (int player = 0; player < MAX_PLAYERS; ++player)
	{
		CvPlayerAI& owner = GET_PLAYER(static_cast<PlayerTypes>(player));
		loop = 0;
		for (CvCity* pCity = owner.firstCity(&loop); pCity != NULL; pCity = owner.nextCity(&loop))
		{
			VoxRlCollectCityAttackCountRows(pCity, rows);
		}
	}
}

void VoxRlCollectAllPlayerResistanceRows(std::vector<PlayerResistanceRecord>& rows)
{
	for (int player = 0; player < MAX_PLAYERS; ++player)
	{
		CvPlayerAI& owner = GET_PLAYER(static_cast<PlayerTypes>(player));
		if (!owner.isAlive()) continue;
		VoxRlCollectPlayerResistanceRows(&owner, rows);
	}
}

// Collects immutable native tables and topology for the generated STATIC builder.
bool VoxRlBuildStaticBlock(const VoxRlBlockIdentity& identity,
	VoxRlOwnedBlockStorage& storage, unsigned int& length)
{
	VoxRlStaticData data;
	ZeroRecord(data.staticRules);
	if (!CollectStaticRulesRecord(data.staticRules)) return false;
	data.staticRules.modAiUnitProduction = MOD_AI_UNIT_PRODUCTION ? 1 : 0;
	ZeroRecord(data.staticHandicapLimits);
	if (!CollectStaticHandicapLimitsRecord(data.staticHandicapLimits)) return false;
	// A mod can delete info-table rows, so the accessors return NULL for
	// those indexes. The vectors stay index-aligned with the native tables:
	// a missing row becomes a zeroed placeholder instead of a skip.
	for (int index = 0; index < GC.getNumTerrainInfos(); ++index)
	{
		TerrainInfoRecord row;
		ZeroRecord(row);
		CvTerrainInfo* info = GC.getTerrainInfo(static_cast<TerrainTypes>(index));
		if (info != NULL && !CollectTerrainInfoRecord(*info, row)) return false;
		data.staticTerrainInfos.push_back(row);
	}
	for (int index = 0; index < GC.getNumFeatureInfos(); ++index)
	{
		FeatureInfoRecord row;
		ZeroRecord(row);
		CvFeatureInfo* info = GC.getFeatureInfo(static_cast<FeatureTypes>(index));
		if (info != NULL && !CollectFeatureInfoRecord(*info, row)) return false;
		data.staticFeatureInfos.push_back(row);
	}
	for (int index = 0; index < GC.getNumRouteInfos(); ++index)
	{
		RouteInfoRecord row;
		ZeroRecord(row);
		CvRouteInfo* info = GC.getRouteInfo(static_cast<RouteTypes>(index));
		if (info != NULL && !CollectRouteInfoRecord(*info, row)) return false;
		data.staticRouteInfos.push_back(row);
	}
	for (int index = 0; index < GC.getNumImprovementInfos(); ++index)
	{
		ImprovementInfoRecord row;
		ZeroRecord(row);
		CvImprovementEntry* info = GC.getImprovementInfo(static_cast<ImprovementTypes>(index));
		if (info != NULL && !CollectImprovementInfoRecord(*info, row)) return false;
		data.staticImprovementInfos.push_back(row);
	}
	for (int index = 0; index < GC.getNumResourceInfos(); ++index)
	{
		ResourceInfoRecord row;
		ZeroRecord(row);
		CvResourceInfo* info = GC.getResourceInfo(static_cast<ResourceTypes>(index));
		if (info != NULL && !CollectResourceInfoRecord(*info, row)) return false;
		data.staticResourceInfos.push_back(row);
	}
	for (int index = 0; index < GC.getNumUnitInfos(); ++index)
	{
		UnitEntryInfoRecord row;
		ZeroRecord(row);
		CvUnitEntry* entry = GC.getUnitInfo(static_cast<UnitTypes>(index));
		if (entry != NULL)
		{
			if (!CollectUnitEntryInfoRecord(*entry, row)) return false;
			row.yieldFromKills = static_cast<i32>(MaxYieldFromKills(*entry, false));
			row.yieldFromBarbarianKills = static_cast<i32>(MaxYieldFromKills(*entry, true));
		}
		data.staticUnitEntryInfos.push_back(row);
	}
	ZeroRecord(data.staticBuildIds);
	if (!CollectStaticBuildIdsRecord(data.staticBuildIds)) return false;
	ZeroRecord(data.staticMapTopology);
	if (!CollectMapTopologyRecord(GC.getMap(), data.staticMapTopology)) return false;
	for (int index = 0; index < GC.getMap().numPlots(); ++index)
	{
		CvPlot* plot = GC.getMap().plotByIndex(index);
		if (plot == NULL) return false;
		PlotTopologyRecord row;
		ZeroRecord(row);
		if (!CollectPlotTopologyRecord(*plot, row)) return false;
		data.staticPlotTopology.push_back(row);
	}
	for (int index = 0; index < GC.getNumPromotionInfos(); ++index)
	{
		PromotionInfoRecord row;
		ZeroRecord(row);
		CvPromotionEntry* info = GC.getPromotionInfo(static_cast<PromotionTypes>(index));
		if (info != NULL && !CollectPromotionInfoRecord(*info, row)) return false;
		data.staticPromotionInfos.push_back(row);
	}
	for (int index = 0; index < GC.getNumProcessInfos(); ++index)
	{
		ProcessInfoRecord row;
		ZeroRecord(row);
		CvProcessInfo* info = GC.getProcessInfo(static_cast<ProcessTypes>(index));
		if (info != NULL && !CollectProcessInfoRecord(*info, row)) return false;
		data.staticProcessInfos.push_back(row);
	}
	for (int index = 0; index < GC.getNumCityEventChoiceInfos(); ++index)
	{
		CityEventChoiceInfoRecord row;
		ZeroRecord(row);
		CvModEventCityChoiceInfo* info = GC.getCityEventChoiceInfo(static_cast<CityEventChoiceTypes>(index));
		if (info != NULL && !CollectCityEventChoiceInfoRecord(*info, row)) return false;
		data.staticCityEventChoiceInfos.push_back(row);
	}
	for (int index = 0; index < GC.getNumUnitClassInfos(); ++index)
	{
		StaticUnitClassInfoRecord row;
		ZeroRecord(row);
		if (!CollectStaticUnitClassInfoRecord(index, row)) return false;
		data.staticUnitClassInfos.push_back(row);
	}
	return data.Write(identity, storage, length);
}

// Collects the native zone table and assignments without advancing its lifecycle.
bool VoxRlCollectZones(CvTacticalAnalysisMap* zoneMap, VoxRlZoneSnapshot& snapshot)
{
	snapshot.zones.clear();
	snapshot.neighbors.clear();
	const int zoneCount = zoneMap != NULL ? zoneMap->GetNumZonesWithoutRefresh() : 0;
	for (int zoneIndex = 0; zoneIndex < zoneCount; ++zoneIndex)
	{
		const CvTacticalDominanceZone* zone = zoneMap->GetZoneByIndexWithoutRefresh(zoneIndex);
		if (zone == NULL) return false;
		ZoneRecord row;
		ZeroRecord(row);
		if (!CollectZoneRecord(*zone, row)) return false;
		row.avgX = static_cast<i32>(zone->GetAverageX());
		row.avgY = static_cast<i32>(zone->GetAverageY());
		CvCity* city = zone->GetZoneCity();
		row.cityOwner = city != NULL ? static_cast<i8>(city->getOwner()) : static_cast<i8>(-1);
		row.cityId = city != NULL ? static_cast<i32>(city->GetID()) : -1;
		const std::vector<int>& nativeNeighbors = zone->GetNeighboringZones();
		std::vector<ZoneNeighborRecord> neighbors;
		for (size_t index = 0; index < nativeNeighbors.size(); ++index)
		{
			ZoneNeighborRecord entry;
			ZeroRecord(entry);
			entry.zoneId = static_cast<i32>(nativeNeighbors[index]);
			neighbors.push_back(entry);
		}
		row.neighborRangeFirst = static_cast<u32>(snapshot.neighbors.size());
		row.neighborRangeCount = static_cast<u32>(neighbors.size());
		snapshot.neighbors.insert(snapshot.neighbors.end(), neighbors.begin(), neighbors.end());
		snapshot.zones.push_back(row);
	}
	const int plotCount = GC.getMap().numPlots();
	snapshot.plotZones.resize(plotCount);
	for (int plot = 0; plot < plotCount; ++plot)
		snapshot.plotZones[plot] = zoneMap != NULL ? zoneMap->GetDominanceZoneIDWithoutRefresh(plot) : -1;
	return true;
}

// Collects native WORLD state in stable owner and plot order for the generated writer.
bool VoxRlBuildWorldBlock(const VoxRlBlockIdentity& identity, PlayerTypes capturingPlayer,
	VoxRlOwnedBlockStorage& storage, unsigned int& length, VoxRlZoneSnapshot& zones,
	std::vector<TeamPassabilityRecord>& teamPassabilitySnapshot)
{
	CvMap& map = GC.getMap();
	const int plotCount = map.numPlots();
	if (plotCount <= 0) return false;
	CvPlayerAI& capturing = GET_PLAYER(capturingPlayer);
	const TeamTypes capturingTeam = capturing.getTeam();
	VoxRlWorldData data;
	std::vector<TeamTypes> aliveTeams;
	CollectAliveTeams(aliveTeams);
	std::vector<PlayerTypes> alivePlayers;
	CollectAlivePlayers(alivePlayers);

	// Unit rows follow plot stacks, while these indices preserve owner iteration order.
	std::map<VoxRlEntityKey, unsigned int> iterationByUnit;
	for (int player = 0; player < MAX_PLAYERS; ++player)
	{
		CvPlayerAI& owner = GET_PLAYER(static_cast<PlayerTypes>(player));
		unsigned int iteration = 0;
		int loop = 0;
		for (CvUnit* unit = owner.firstUnit(&loop); unit != NULL; unit = owner.nextUnit(&loop))
		{
			if (unit->isDelayedDeath() || unit->plot() == NULL) continue;
			iterationByUnit[VoxRlEntityKey(player, unit->GetID())] = iteration++;
		}
	}

	// Prepare the native citadel cache before zone preparation, as at the capture checkpoint.
	for (int player = 0; player < MAX_PLAYERS; ++player)
	{
		CvPlayerAI& owner = GET_PLAYER(static_cast<PlayerTypes>(player));
		PlayerRecord row;
		ZeroRecord(row);
		std::vector<CitadelPlotRecord> citadels;
		if (owner.isAlive())
		{
			const PlotIndexContainer& plots = owner.GetPlots();
			for (size_t index = 0; index < plots.size(); ++index)
			{
				CvPlot* plot = map.plotByIndex(plots[index]);
				if (plot == NULL || !owner.IsNicePlotForCitadel(plot)) continue;
				CitadelPlotRecord entry;
				ZeroRecord(entry);
				entry.plotIndex = static_cast<i32>(plots[index]);
				citadels.push_back(entry);
			}
		}
		if (!AppendPlayerRecordCitadelPlotRange(&row, &data, citadels)) return false;
		data.worldPlayers.push_back(row);
	}

	const CvDangerPlots* danger = capturing.GetDangerPlots();
	if (danger == NULL) return false;
	DangerPlayerRecord dangerRow;
	ZeroRecord(dangerRow);
	if (!CollectDangerPlayerRecord(capturingPlayer, dangerRow)) return false;
	dangerRow.turnBuilt = static_cast<i32>(danger->GetTurnBuilt());
	dangerRow.dirty = danger->IsDirty() ? 1 : 0;
	std::vector<KnownAttackerRecord> known;
	for (UnitSet::const_iterator entry = danger->GetKnownUnits().begin(); entry != danger->GetKnownUnits().end(); ++entry)
	{
		KnownAttackerRecord row;
		ZeroRecord(row);
		row.owner = static_cast<i8>(entry->first);
		row.unitId = static_cast<i32>(entry->second);
		known.push_back(row);
	}
	std::vector<VanishedAttackerRecord> vanished;
	for (UnitSet::const_iterator entry = danger->GetVanishedUnits().begin(); entry != danger->GetVanishedUnits().end(); ++entry)
	{
		VanishedAttackerRecord row;
		ZeroRecord(row);
		row.owner = static_cast<i8>(entry->first);
		row.unitId = static_cast<i32>(entry->second);
		vanished.push_back(row);
	}
	if (!AppendDangerPlayerRecordKnownUnitRange(&dangerRow, &data, known) ||
		!AppendDangerPlayerRecordVanishedUnitRange(&dangerRow, &data, vanished)) return false;
	data.worldDangerPlayers.push_back(dangerRow);

	if (!VoxRlCollectAllUnitModifierRows(data.worldUnitModifiers)) return false;
	VoxRlCollectAllUnitPlagueRows(data.worldUnitPlagues, data.worldUnitBlockedPromotions);
	VoxRlCollectAllUnitAttackCountRows(data.worldUnitAttackCounts);
	VoxRlCollectAllCityAttackCountRows(data.worldCityAttackCounts);
	VoxRlCollectAllPlayerResistanceRows(data.worldPlayerResistances);
	for (size_t minorIndex = 0; minorIndex < alivePlayers.size(); ++minorIndex)
	{
		CvPlayerAI& minor = GET_PLAYER(alivePlayers[minorIndex]);
		if (!minor.isMinorCiv() || minor.isBarbarian()) continue;
		for (size_t majorIndex = 0; majorIndex < alivePlayers.size(); ++majorIndex)
		{
			CvPlayerAI& major = GET_PLAYER(alivePlayers[majorIndex]);
			if (major.isMinorCiv() || major.isBarbarian()) continue;
			MinorRelationRecord row;
			ZeroRecord(row);
			if (!CollectMinorRelationRecord(minor, *minor.GetMinorCivAI(), minor.GetID(), major.GetID(), row)) return false;
			data.worldMinorRelations.push_back(row);
		}
	}

	CvTacticalAnalysisMap* zoneMap = capturing.GetTacticalAI()->GetTacticalAnalysisMap();
	// Zones are the checkpoint table. The no-refresh accessors never trigger
	// the native rebuild, so capture cannot fire the tactical-time zone
	// recompute ahead of its normal schedule.
	if (!VoxRlCollectZones(zoneMap, zones)) return false;
	data.worldZones = zones.zones;
	data.worldZoneNeighbors = zones.neighbors;
	// Each plot's zone membership is encoded as a one-based index into the ordered table.
	// The 16-bit form covers tables up to 65,535 rows; larger tables take the wide form.
	std::map<int, unsigned int> zoneRowByZoneId;
	for (size_t zoneIndex = 0; zoneIndex < zones.zones.size(); ++zoneIndex)
		zoneRowByZoneId[zones.zones[zoneIndex].zoneId] = static_cast<unsigned int>(zoneIndex + 1);
	const bool wideZoneIndices = zones.zones.size() > 65535U;
	for (int plotIndex = 0; plotIndex < plotCount; ++plotIndex)
	{
		CvPlot* plot = map.plotByIndex(plotIndex);
		if (plot == NULL) return false;
		PlotCoreRecord row;
		ZeroRecord(row);
		if (!CollectPlotCoreRecord(*plot, row)) return false;
		data.worldPlotCore.push_back(row);
		const i32 plotZone = zones.plotZones[plotIndex];
		unsigned int zoneTableIndex = 0;
		if (plotZone != -1)
		{
			std::map<int, unsigned int>::const_iterator zoneRow = zoneRowByZoneId.find(plotZone);
			if (zoneRow == zoneRowByZoneId.end()) return false;
			zoneTableIndex = zoneRow->second;
		}
		if (wideZoneIndices)
		{
			PlotZoneIndexWideRecord indexRow;
			ZeroRecord(indexRow);
			indexRow.zoneTableIndex = zoneTableIndex;
			data.worldPlotZonesWide.push_back(indexRow);
		}
		else
		{
			PlotZoneIndexRecord indexRow;
			ZeroRecord(indexRow);
			indexRow.zoneTableIndex = static_cast<u16>(zoneTableIndex);
			data.worldPlotZones.push_back(indexRow);
		}
		std::vector<UnitRecord> units;
		// The default plot node chain omits trade and other managed layers.
		// WORLD ranges retain every physical unit so owner iteration and later
		// deltas remain complete while the simulator filters tactical occupancy.
		const int unitCount = plot->getNumLayerUnits();
		for (int unitIndex = 0; unitIndex < unitCount; ++unitIndex)
		{
			CvUnit* unit = plot->getLayerUnit(unitIndex);
			if (unit == NULL || unit->isDelayedDeath()) continue;
			UnitRecord unitRow;
			ZeroRecord(unitRow);
			std::vector<UnitMovementCountRecord> movementCounts;
			if (!VoxRlCollectUnitRecord(*unit, capturingTeam, unitRow, movementCounts)) return false;
			// The range helper appends the entries to the section in captured unit order and
			// assigns this row's first and count.
			if (!AppendUnitRecordMovementCountRange(&unitRow, &data, movementCounts)) return false;
			std::map<VoxRlEntityKey, unsigned int>::const_iterator iteration =
				iterationByUnit.find(VoxRlEntityKey(static_cast<int>(unit->getOwner()), unit->GetID()));
			if (iteration == iterationByUnit.end()) return false;
			unitRow.iterationIndex = iteration->second;
			units.push_back(unitRow);
		}
		// Unit rows append in captured plot order; the loader reconstructs each
		// plot's packed range from this sequence.
		data.worldUnits.insert(data.worldUnits.end(), units.begin(), units.end());
	}
	if (!VoxRlCollectTeamPassabilityRows(data.worldTeamPassability)) return false;
	teamPassabilitySnapshot = data.worldTeamPassability;
	if (data.worldUnits.size() != iterationByUnit.size()) return false;

	// Each alive team contributes one plot bitset; all-zero optional detection is omitted.
	const size_t bitBytes = (static_cast<size_t>(plotCount) + 7U) / 8U;
	std::vector<u8>* bitsets[] = { &data.worldRevealedBits, &data.worldVisibleBits,
		&data.worldKnownVisibleBits, &data.worldInvisibleVisibleBits };
	for (int kind = 0; kind < 4; ++kind) bitsets[kind]->resize(bitBytes * aliveTeams.size(), 0);
	data.worldRevealedNoneOverrideBits.assign(bitBytes * aliveTeams.size(), 0);
	bool hasInvisibleVisibility = false;
	bool hasRevealedNoneOverrides = false;
	for (size_t teamIndex = 0; teamIndex < aliveTeams.size(); ++teamIndex)
	{
		const TeamTypes team = aliveTeams[teamIndex];
		PlotTeamRecord row;
		ZeroRecord(row);
		row.team = static_cast<i8>(team);
		std::vector<RevealedOverrideRecord> overrides;
		for (int plotIndex = 0; plotIndex < plotCount; ++plotIndex)
		{
			CvPlot* plot = map.plotByIndex(plotIndex);
			const ImprovementTypes revealedImprovement = plot->getRevealedImprovementType(team);
			const RouteTypes revealedRoute = plot->getRevealedRouteType(team);
			const PlayerTypes revealedOwner = plot->getRevealedOwner(team);
			if (revealedImprovement != plot->getImprovementType() || revealedOwner != plot->getOwner() || revealedRoute != plot->getRouteType())
			{
				if (revealedImprovement == NO_IMPROVEMENT && revealedRoute == NO_ROUTE && revealedOwner == NO_PLAYER)
				{
					data.worldRevealedNoneOverrideBits[teamIndex * bitBytes + (plotIndex >> 3)] |= static_cast<u8>(1U << (plotIndex & 7));
					hasRevealedNoneOverrides = true;
				}
				else
				{
					RevealedOverrideRecord entry;
					ZeroRecord(entry);
					entry.team = static_cast<i8>(team);
					entry.plotIndex = plotIndex;
					entry.revealedImprovementType = revealedImprovement;
					entry.revealedRouteType = static_cast<i8>(revealedRoute);
					entry.revealedOwner = revealedOwner;
					overrides.push_back(entry);
				}
			}
			const bool bits[] = { plot->isRevealed(team), plot->isVisible(team),
				plot->GetKnownVisibilityCount(team) > 0, plot->isInvisibleVisibleUnit(team) };
			hasInvisibleVisibility = hasInvisibleVisibility || bits[3];
			for (int kind = 0; kind < 4; ++kind)
				if (bits[kind]) (*bitsets[kind])[teamIndex * bitBytes + (plotIndex >> 3)] |= static_cast<u8>(1U << (plotIndex & 7));
		}
		if (!AppendPlotTeamRecordRevealedOverrideRange(&row, &data, overrides)) return false;
		data.worldPlotTeams.push_back(row);
	}
	if (!hasInvisibleVisibility) data.worldInvisibleVisibleBits.clear();
	if (!hasRevealedNoneOverrides) data.worldRevealedNoneOverrideBits.clear();

	for (int player = 0; player < MAX_PLAYERS; ++player)
	{
		CvPlayerAI& owner = GET_PLAYER(static_cast<PlayerTypes>(player));
		unsigned int iteration = 0;
		int loop = 0;
		for (CvCity* city = owner.firstCity(&loop); city != NULL; city = owner.nextCity(&loop))
		{
			CityRecord row;
			ZeroRecord(row);
			if (!VoxRlCollectCityRecord(*city, capturingPlayer, row)) return false;
			row.iterationIndex = iteration++;
			data.worldCities.push_back(row);
		}
		const u32 citadelRangeFirst = data.worldPlayers[player].citadelPlotRangeFirst;
		const u32 citadelRangeCount = data.worldPlayers[player].citadelPlotRangeCount;
		PlayerRecord row;
		ZeroRecord(row);
		if (!CollectPlayerRecord(owner, capturingPlayer, row)) return false;
		row.id = static_cast<i8>(player);
		row.citadelPlotRangeFirst = citadelRangeFirst;
		row.citadelPlotRangeCount = citadelRangeCount;
		data.worldPlayers[player] = row;
	}
	for (int team = 0; team < MAX_TEAMS; ++team)
	{
		TeamRecord row;
		ZeroRecord(row);
		if (!CollectTeamRecord(GET_TEAM(static_cast<TeamTypes>(team)), row)) return false;
		data.worldTeams.push_back(row);
	}
	for (size_t team = 0; team < aliveTeams.size(); ++team)
		for (size_t other = 0; other < aliveTeams.size(); ++other)
		{
			if (team == other) continue;
			TeamRelationRecord row;
			ZeroRecord(row);
			if (!CollectTeamRelationRecord(GET_TEAM(aliveTeams[team]), aliveTeams[other], capturingPlayer, row)) return false;
			data.worldTeamRelations.push_back(row);
		}
	for (size_t player = 0; player < alivePlayers.size(); ++player)
	{
		CvPlayerAI& owner = GET_PLAYER(alivePlayers[player]);
		InterceptorCacheRecord row;
		ZeroRecord(row);
		row.player = static_cast<i8>(owner.GetID());
		const std::vector<std::pair<int, int> >& nativeEntries = owner.GetPossibleInterceptors();
		std::vector<InterceptorCacheEntryRecord> entries;
		for (size_t index = 0; index < nativeEntries.size(); ++index)
		{
			InterceptorCacheEntryRecord entry;
			ZeroRecord(entry);
			entry.owner = static_cast<i8>(owner.GetID());
			entry.unitId = static_cast<i32>(nativeEntries[index].first);
			entry.plotIndex = static_cast<i32>(nativeEntries[index].second);
			entries.push_back(entry);
		}
		if (!AppendInterceptorCacheRecordEntryRange(&row, &data, entries)) return false;
		data.worldInterceptorCaches.push_back(row);
	}
	return data.Write(identity, storage, length);
}

// Collects the operational entry snapshot without refreshing native zones.
bool VoxRlBuildCampaignBlock(const VoxRlBlockIdentity& identity, PlayerTypes capturingPlayer,
	VoxRlOwnedBlockStorage& storage, unsigned int& length)
{
	CvPlayerAI& capturing = GET_PLAYER(capturingPlayer);
	CvMilitaryAI* military = capturing.GetMilitaryAI();
	if (military == NULL) return false;
	VoxRlCampaignData data;
	CampaignHeaderRecord& header = data.campaignHeader;
	ZeroRecord(header);
	if (!CollectCampaignHeaderRecord(*military, capturing, header)) return false;
	header.numLandUnits = static_cast<i32>(military->GetNumLandUnits());
	header.numNavalUnits = static_cast<i32>(military->GetNumNavalUnits());
	header.numLandUnitsInArmies = static_cast<i32>(military->GetNumLandUnitsInArmies());
	header.numNavalUnitsInArmies = static_cast<i32>(military->GetNumNavalUnitsInArmies());
	header.recommendedLandUnits = static_cast<i32>(military->GetRecommendedLandUnits());
	header.recommendedNavalUnits = static_cast<i32>(military->GetRecommendedNavalUnits());
	header.recommendedExplorerUnits = static_cast<i32>(military->GetRecommendedExplorerUnits());
	header.treasury = static_cast<i32>(capturing.GetTreasury()->GetGold());
	for (int player = 0; player < MAX_PLAYERS; ++player)
	{
		if (player == static_cast<int>(capturingPlayer)) continue;
		CvPlayerAI& other = GET_PLAYER(static_cast<PlayerTypes>(player));
		if (!other.isAlive()) continue;
		CampaignEnemyRecord row;
		ZeroRecord(row);
		row.player = static_cast<i8>(player);
		// Barbarians have no entry in the diplomacy AI's civilization war-state array.
		row.warState = static_cast<i8>(player < MAX_CIV_PLAYERS
			? capturing.GetDiplomacyAI()->GetWarState(static_cast<PlayerTypes>(player)) : NO_WAR_STATE_TYPE);
		row.warScore = static_cast<i8>(capturing.GetDiplomacyAI()->GetWarScore(static_cast<PlayerTypes>(player)));
		data.campaignEnemies.push_back(row);
	}
	const std::vector<CvAttackTarget>& attackTargets = military->GetPotentialAttackTargets();
	for (size_t index = 0; index < attackTargets.size(); ++index)
	{
		CampaignAttackTargetRecord row;
		ZeroRecord(row);
		row.armyType = static_cast<i8>(attackTargets[index].m_armyType);
		row.musterPlotIndex = static_cast<i32>(attackTargets[index].m_iMusterPlotIndex);
		row.stagingPlotIndex = static_cast<i32>(attackTargets[index].m_iStagingPlotIndex);
		row.targetPlotIndex = static_cast<i32>(attackTargets[index].m_iTargetPlotIndex);
		row.pathLength = static_cast<i32>(attackTargets[index].m_iPathLength);
		row.approachScore = static_cast<i32>(attackTargets[index].m_iApproachScore);
		row.preferred = attackTargets[index].m_bPreferred ? 1 : 0;
		data.campaignAttackTargets.push_back(row);
	}
	const std::vector<CvAttackTarget>& exposedCities = military->GetExposedCities();
	std::vector<CampaignExposedCityRecord> exposedRows;
	for (size_t index = 0; index < exposedCities.size(); ++index)
	{
		CampaignExposedCityRecord row;
		ZeroRecord(row);
		CvPlot* plot = GC.getMap().plotByIndex(exposedCities[index].m_iTargetPlotIndex);
		CvCity* city = plot != NULL ? plot->getPlotCity() : NULL;
		row.cityOwner = city != NULL ? static_cast<i8>(city->getOwner()) : static_cast<i8>(-1);
		row.cityId = city != NULL ? static_cast<i32>(city->GetID()) : static_cast<i32>(-1);
		exposedRows.push_back(row);
	}
	if (!AppendCampaignHeaderRecordExposedCityRange(&data, exposedRows)) return false;
	std::vector<CvCity*> threatened = capturing.GetThreatenedCities(false);
	std::vector<CampaignThreatenedCityRecord> threatenedRows;
	for (size_t index = 0; index < threatened.size(); ++index)
	{
		CampaignThreatenedCityRecord row;
		ZeroRecord(row);
		row.cityOwner = threatened[index] != NULL ? static_cast<i8>(threatened[index]->getOwner()) : static_cast<i8>(-1);
		row.cityId = threatened[index] != NULL ? static_cast<i32>(threatened[index]->GetID()) : -1;
		threatenedRows.push_back(row);
	}
	if (!AppendCampaignHeaderRecordThreatenedCityRange(&data, threatenedRows)) return false;
	std::vector<CvCity*> coastal = capturing.GetThreatenedCities(true);
	std::vector<CampaignCoastalThreatenedCityRecord> coastalRows;
	for (size_t index = 0; index < coastal.size(); ++index)
	{
		CampaignCoastalThreatenedCityRecord row;
		ZeroRecord(row);
		row.cityOwner = coastal[index] != NULL ? static_cast<i8>(coastal[index]->getOwner()) : static_cast<i8>(-1);
		row.cityId = coastal[index] != NULL ? static_cast<i32>(coastal[index]->GetID()) : -1;
		coastalRows.push_back(row);
	}
	if (!AppendCampaignHeaderRecordCoastalThreatenedCityRange(&data, coastalRows)) return false;
	for (size_t index = 0; index < capturing.getNumAIOperations(); ++index)
	{
		CvAIOperation* operation = capturing.getAIOperationByIndex(index);
		if (operation == NULL) continue;
		CampaignOperationRecord row;
		ZeroRecord(row);
		row.id = static_cast<i32>(operation->GetID());
		row.type = static_cast<i8>(operation->GetOperationType());
		row.armyType = static_cast<i8>(operation->GetArmyType());
		row.currentState = static_cast<i8>(operation->GetOperationState());
		row.enemy = static_cast<i8>(operation->GetEnemy());
		row.musterX = static_cast<i32>(operation->GetMusterX());
		row.musterY = static_cast<i32>(operation->GetMusterY());
		row.targetX = static_cast<i32>(operation->GetTargetX());
		row.targetY = static_cast<i32>(operation->GetTargetY());
		row.turnStarted = static_cast<i32>(operation->GetTurnStarted());
		row.distanceMusterToTarget = static_cast<i32>(operation->GetDistanceMusterToTarget());
		std::vector<CampaignOperationArmyIdRecord> armyIds;
		const std::vector<int>& ids = operation->GetArmyIDs();
		for (size_t army = 0; army < ids.size(); ++army)
		{
			CampaignOperationArmyIdRecord id;
			ZeroRecord(id);
			id.armyId = static_cast<i32>(ids[army]);
			armyIds.push_back(id);
		}
		if (!AppendCampaignOperationRecordArmyIdRange(&row, &data, armyIds)) return false;
		data.campaignOperations.push_back(row);
		for (size_t army = 0; army < ids.size(); ++army)
		{
			CvArmyAI* nativeArmy = capturing.getArmyAI(ids[army]);
			if (nativeArmy == NULL) continue;
			CampaignArmyRecord armyRow;
			ZeroRecord(armyRow);
			armyRow.id = static_cast<i32>(nativeArmy->GetID());
			armyRow.formation = static_cast<i32>(nativeArmy->GetFormationType());
			armyRow.aiState = static_cast<i8>(nativeArmy->GetArmyAIState());
			armyRow.goalX = static_cast<i32>(nativeArmy->GetGoalX());
			armyRow.goalY = static_cast<i32>(nativeArmy->GetGoalY());
			armyRow.slotsFilled = static_cast<i32>(nativeArmy->GetNumSlotsFilled());
			std::vector<CampaignFormationEntryRecord> slots;
			const std::vector<CvArmyFormationSlot>& nativeSlots = nativeArmy->GetSlotStatus();
			for (size_t slot = 0; slot < nativeSlots.size(); ++slot)
			{
				CampaignFormationEntryRecord entry;
				ZeroRecord(entry);
				entry.unitId = static_cast<i32>(nativeSlots[slot].GetUnitID());
				entry.unitOwner = entry.unitId >= 0 ? static_cast<i8>(capturingPlayer) : static_cast<i8>(-1);
				entry.required = nativeSlots[slot].IsRequired() ? 1 : 0;
				slots.push_back(entry);
			}
			if (!AppendCampaignArmyRecordFormationRange(&armyRow, &data, slots)) return false;
			data.campaignArmies.push_back(armyRow);
		}
	}
	CvTacticalAnalysisMap* zoneMap = capturing.GetTacticalAI()->GetTacticalAnalysisMap();
	const int zoneCount = zoneMap != NULL ? zoneMap->GetNumZonesWithoutRefresh() : 0;
	for (int index = 0; index < zoneCount; ++index)
	{
		const CvTacticalDominanceZone* zone = zoneMap->GetZoneByIndexWithoutRefresh(index);
		if (zone == NULL) return false;
		CampaignZoneRecord row;
		ZeroRecord(row);
		CollectCampaignZoneRecord(*zone, row);
		row.avgX = static_cast<i32>(zone->GetAverageX());
		row.avgY = static_cast<i32>(zone->GetAverageY());
		CvCity* city = zone->GetZoneCity();
		row.cityOwner = city != NULL ? static_cast<i8>(city->getOwner()) : static_cast<i8>(-1);
		row.cityId = city != NULL ? static_cast<i32>(city->GetID()) : static_cast<i32>(-1);
		std::vector<CampaignZoneNeighborRecord> neighbors;
		const std::vector<int>& nativeNeighbors = zone->GetNeighboringZones();
		for (size_t neighbor = 0; neighbor < nativeNeighbors.size(); ++neighbor)
		{
			CampaignZoneNeighborRecord entry;
			ZeroRecord(entry);
			entry.zoneId = static_cast<i32>(nativeNeighbors[neighbor]);
			neighbors.push_back(entry);
		}
		if (!AppendCampaignZoneRecordNeighborRange(&row, &data, neighbors)) return false;
		data.campaignZones.push_back(row);
	}
	return data.Write(identity, storage, length);
}

// Binds staged request children through their declared generated range helpers.
bool VoxRlBuildRequestBlock(const VoxRlBlockIdentity& identity, VoxRlRequestData& data,
	VoxRlOwnedBlockStorage& storage, unsigned int& length)
{
	VoxRlRequestData output;
	RequestHeaderRecord& header = output.requestHeader;
	header = data.requestHeader;
	header.decisionId = static_cast<u32>(identity.decisionId);
	if (!AppendRequestHeaderRecordDangerEventRange(&output, data.requestDangerEvents) ||
		!AppendRequestHeaderRecordTeamRelationRange(&output, data.requestTeamRelations) ||
		!AppendRequestHeaderRecordDeltaUnitRange(&output, data.requestDeltaUnits) ||
		!AppendRequestHeaderRecordDeltaPlotCoreRange(&output, data.requestDeltaPlots) ||
		!AppendRequestHeaderRecordDeltaPlotZoneRange(&output, data.requestDeltaPlotZones) ||
		!AppendRequestHeaderRecordDeltaPlotZoneWideRange(&output, data.requestDeltaPlotZonesWide) ||
		!AppendRequestHeaderRecordZoneReplacementRange(&output, data.requestZoneReplacements) ||
		!AppendRequestHeaderRecordDeltaCityRange(&output, data.requestDeltaCities) ||
		!AppendRequestHeaderRecordRemovedUnitRange(&output, data.requestRemovedUnits) ||
		!AppendRequestHeaderRecordRemovedCityRange(&output, data.requestRemovedCities) ||
		!AppendRequestHeaderRecordTeamPassabilityRange(&output, data.requestTeamPassability) ||
		!AppendRequestHeaderRecordVisibilityResetRange(&output, data.requestVisibilityResets) ||
		!AppendRequestHeaderRecordVisibilityFlipRange(&output, data.requestVisibilityFlips) ||
		!AppendRequestHeaderRecordRevealedOverrideUpsertRange(&output, data.requestRevealedOverrideUpserts) ||
		!AppendRequestHeaderRecordRemovedRevealedOverrideRange(&output, data.requestRemovedRevealedOverrides) ||
		!AppendRequestHeaderRecordKnownAttackerRange(&output, data.requestKnownAttackers) ||
		!AppendRequestHeaderRecordInterceptorReplacementRange(&output, data.requestInterceptorReplacements) ||
		!AppendRequestHeaderRecordParticipantRange(&output, data.requestParticipants) ||
		!AppendRequestHeaderRecordDroppedUnitRange(&output, data.requestDroppedUnits) ||
		!AppendRequestHeaderRecordUnitModifierReplacementRange(&output, data.requestUnitModifierReplacements) ||
		!AppendRequestHeaderRecordUnitPlagueReplacementRange(&output, data.requestUnitPlagueReplacements) ||
		!AppendRequestHeaderRecordUnitBlockedPromotionReplacementRange(&output, data.requestUnitBlockedPromotionReplacements) ||
		!AppendRequestHeaderRecordUnitAttackCountReplacementRange(&output, data.requestUnitAttackCountReplacements) ||
		!AppendRequestHeaderRecordPlayerResistanceReplacementRange(&output, data.requestPlayerResistanceReplacements) ||
		!AppendRequestHeaderRecordCityAttackCountReplacementRange(&output, data.requestCityAttackCountReplacements)) return false;
	output.requestZoneNeighbors = data.requestZoneNeighbors;
	// The sparse replacement parents keep the row ranges they were bound to at
	// collection time, and the child row sections copy in the same order, so
	// those ranges stay exact; the generated request validator re-checks the
	// tiling inside Write.
	output.requestUnitModifierRows = data.requestUnitModifierRows;
	output.requestUnitPlagueRows = data.requestUnitPlagueRows;
	output.requestUnitBlockedPromotionRows = data.requestUnitBlockedPromotionRows;
	output.requestUnitAttackCountRows = data.requestUnitAttackCountRows;
	output.requestPlayerResistanceRows = data.requestPlayerResistanceRows;
	output.requestCityAttackCountRows = data.requestCityAttackCountRows;
	size_t entryCursor = 0;
	for (size_t index = 0; index < data.requestInterceptorReplacements.size(); ++index)
	{
		RequestInterceptorReplacementRecord& replacement = output.requestInterceptorReplacements[index];
		std::vector<RequestInterceptorEntryRecord> entries;
		while (entryCursor < data.requestInterceptorEntries.size() &&
			static_cast<int>(data.requestInterceptorEntries[entryCursor].owner) == static_cast<int>(replacement.player))
		{
			entries.push_back(data.requestInterceptorEntries[entryCursor]);
			++entryCursor;
		}
		if (!AppendRequestInterceptorReplacementRecordEntryRange(&replacement, &output, entries)) return false;
	}
	if (entryCursor != data.requestInterceptorEntries.size()) return false;
	// The sparse replacement tiling and identity consistency are enforced by the
	// generated request validator, which runs inside Write.
	return output.Write(identity, storage, length);
}
