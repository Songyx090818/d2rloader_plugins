// Staffmods: the class skills an item rolls because of its item type
// (itemtypes.txt StaffMods), shown as "+N to <Skill> (<Class> Only)".
//
// See celestialrayone.staffmods.toml for the behaviour write-up.
//
// THE ROUTINE. 0x58AF50 (game, item, itemLevel, firstClassSkill,
// classSkillCount, dropBonus) rolls the staffmods of every non-Classic item.
// Its only caller, 0x58A050, takes the class from the item type's StaffMods,
// the class skill count from 0x33CB30 and the first class skill from 0x33DDE0,
// and sends Classic-format items (item format 0) to the separate classic
// routine 0x58B7C0 instead. Staffmods are rolled after the affixes are applied
// (the rare roll 0x58A120 calls 0x58A050 at 0x58A536, after its prefixes and
// suffixes are assigned and their properties written).
//
// THE CHANGE. The routine is replaced by the same steps, rolled in the same
// order on the item seed, plus a level cap:
//   cap = the item's Required Level before staffmods, at least 1, from the
//         game's own Required Level function 0x376DE0 (a loader thunk into
//         D2RLoader's ReadWideItemLevelRequirement, which handles a null unit
//         on its own path) called with no unit, so no class adjustment.
//   A staffmod whose tier is above the cap (tier reqlevel > cap) drops to the
//   highest tier at or below the cap that has skills for the class. A skill
//   whose skills.txt reqlevel is above the cap is never given (a failed try).
// When the cap allows every tier, every roll and result is the vanilla one.
//
// VANILLA STEPS, KEPT:
//   count: dropBonus + rand(100): >90 three, >70 two, >30 (or any dropBonus)
//     one, otherwise none
//   base tier from the item level: <=11 1, <=18 2, <=24 3, <=36 4, above 36
//     5 (item format 100+) or 4
//   per staffmod: rand(100): >80 +1, >30 +0, >10 -1, otherwise -2; at least
//     1; low quality (quality 1) at most 4
//   Reign of the Warlock games (data context 3): tier t is the class skills
//     whose reqlevel is exactly 1/6/12/18/24/30; an empty tier gives that
//     staffmod nothing. Other games: firstClassSkill + 5*(t-1) + rand(5).
//   6 tries per staffmod; a try fails on a skills.txt itypea1 the item is not
//     or on a skill the item already rolled. (Vanilla accepted a try past the
//     end of skills.txt; it is a failed try here, since its reqlevel cannot
//     be checked. Unreachable with real data.)
//   value: 1 on low quality items of format 100+, otherwise
//     dropBonus/2 + rand(100): >=90 3, >=60 2, otherwise 1. Written as
//     item_singleskill (107), layer = skill id, into the item's stat list with
//     flag 40h, allocated and posted when missing.
//
// THE SKILL ID ARGUMENT. The stat write 0x2F7C00 is a loader thunk into
// D2RCore's SetWideListStat, which reads its fifth argument (the layer, here
// the skill id) as a full 32-bit value and keeps all 32 bits in the stat's key.
// The game's own code only writes the low 16 bits of that argument, so
// D2RLoader patches the original routine (0x58B683 -> 0x143E2D7A0) to zero the
// upper 16 bits before the call. This plugin replaces that routine, so it must
// pass the skill id as a whole 32-bit value itself: with only 16 bits written,
// the upper half is left over from the stack, the item keeps a skill id past
// the end of skills.txt (the extended save format writes all 32 bits of a stat
// parameter, so the bad id reaches the client and the save file), and the
// staffmod shows as an empty tooltip line.
//
// Built against D2R 3.3 in the D2RLoader 1.3.1 process image (base
// 0x140000000), PluginSDK 0.3.0. The hook site and every helper entry are
// checked byte for byte before anything is installed.

#include <D2RLPlugin/api.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace {

// ---------------------------------------------------------------------------
// Hook site
// ---------------------------------------------------------------------------

// push rbp / push rbx / push rdi / push r12 / push r15 / lea rbp,[rsp-130h]:
// six whole instructions, sixteen bytes, no relative operands.
constexpr std::uint64_t StaffmodsRva = 0x0058AF50;
constexpr std::uint8_t  StaffmodsPrologue[] {
	0x40, 0x55, 0x53, 0x57, 0x41, 0x54, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0xD0, 0xFE, 0xFF, 0xFF };

// ---------------------------------------------------------------------------
// Game functions
// ---------------------------------------------------------------------------

constexpr std::uint64_t ItemSeedRva      = 0x0036CC80;  // (item) -> seed pair at ItemData+4
constexpr std::uint64_t RollRva          = 0x00153B00;  // (seed, n) -> [0, n); 0 for n <= 0
constexpr std::uint64_t ItemFormatRva    = 0x003730D0;  // (item) -> ItemData+40h, uint16
constexpr std::uint64_t ItemQualityRva   = 0x0036CF60;  // (item)
constexpr std::uint64_t CheckItemTypeRva = 0x00373890;  // (item, itemtypes.txt row)
constexpr std::uint64_t DataTablesRva    = 0x00300A90;  // (dataContext)
constexpr std::uint64_t RequiredLevelRva = 0x00376DE0;  // loader thunk, (item, unit)
constexpr std::uint64_t GetStatListRva   = 0x002F5A00;  // (unit, state, flag)
constexpr std::uint64_t AllocStatListRva = 0x002F7300;  // (flags, expireFrame, sourceType, sourceGuid)
constexpr std::uint64_t PostStatListRva  = 0x002F2CB0;  // loader thunk, (unit, list, update)
constexpr std::uint64_t SetListStatRva   = 0x002F7C00;  // loader thunk, (dataContext, list, stat, value, layer: 32 bits)
constexpr std::uint64_t UnitIdRva        = 0x0034A330;  // (unit, file, line)

// Entry witnesses, whole instructions only except where a call's opcode and
// first displacement byte tell two otherwise identical entries apart.
constexpr std::uint8_t LoaderThunkBytes[] { 0xFF, 0x25 };
constexpr std::uint8_t ItemSeedBytes[] {
	0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9, 0x74, 0x0A, 0xE8, 0x3D };
constexpr std::uint8_t ItemQualityBytes[] {
	0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9, 0x74, 0x0A, 0xE8, 0x5D };
constexpr std::uint8_t RollBytes[] {
	0x44, 0x8B, 0xCA, 0x48, 0x8B, 0xD1, 0x45, 0x85, 0xC9, 0x7F, 0x03, 0x33, 0xC0, 0xC3, 0x8B, 0x01, 0x8B, 0x49, 0x04 };
constexpr std::uint8_t ItemFormatBytes[] {
	0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9, 0x75, 0x13 };
constexpr std::uint8_t CheckItemTypeBytes[] {
	0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57 };
constexpr std::uint8_t DataTablesBytes[] {
	0x48, 0x83, 0xEC, 0x28, 0x0F, 0xB6, 0xC1, 0x48, 0x89, 0x44, 0x24, 0x38, 0x48, 0x83, 0xF8, 0x04 };
constexpr std::uint8_t GetStatListBytes[] {
	0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20 };
constexpr std::uint8_t AllocStatListBytes[] {
	0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20 };
constexpr std::uint8_t UnitIdBytes[] {
	0x48, 0x83, 0xEC, 0x28, 0x48, 0x85, 0xC9, 0x75, 0x1D, 0x88, 0x4C, 0x24, 0x30, 0x48, 0x8D, 0x4C, 0x24, 0x30 };

using ItemSeedFn      = std::uint32_t*(__fastcall*)(void* item) noexcept;
using RollFn          = std::uint32_t(__fastcall*)(std::uint32_t* seed, std::int32_t upperBound) noexcept;
using ItemFormatFn    = std::uint16_t(__fastcall*)(void* item) noexcept;
using ItemQualityFn   = std::int32_t(__fastcall*)(void* item) noexcept;
using CheckItemTypeFn = std::int32_t(__fastcall*)(void* item, std::int32_t itemType) noexcept;
using DataTablesFn    = const std::uint8_t*(__fastcall*)(std::uint8_t context) noexcept;
using RequiredLevelFn = std::int32_t(__fastcall*)(void* item, void* unit) noexcept;
using GetStatListFn   = void*(__fastcall*)(void* unit, std::int32_t state, std::uint32_t flag) noexcept;
using AllocStatListFn = void*(__fastcall*)(std::uint32_t flags, std::int32_t expireFrame, std::int32_t sourceType,
                                           std::uint32_t sourceGuid) noexcept;
using PostStatListFn  = void(__fastcall*)(void* unit, void* list, std::int32_t update) noexcept;
using SetListStatFn   = void(__fastcall*)(std::uint8_t context, void* list, std::int32_t stat, std::int32_t value,
                                          std::uint32_t layer) noexcept;
using UnitIdFn        = std::uint32_t(__fastcall*)(void* unit, const char* file, std::int32_t line) noexcept;

ItemSeedFn      ItemSeed      = nullptr;
RollFn          Roll          = nullptr;
ItemFormatFn    ItemFormat    = nullptr;
ItemQualityFn   ItemQuality   = nullptr;
CheckItemTypeFn CheckItemType = nullptr;
DataTablesFn    DataTables    = nullptr;
RequiredLevelFn RequiredLevel = nullptr;
GetStatListFn   GetStatList   = nullptr;
AllocStatListFn AllocStatList = nullptr;
PostStatListFn  PostStatList  = nullptr;
SetListStatFn   SetListStat   = nullptr;
UnitIdFn        UnitId        = nullptr;

// ---------------------------------------------------------------------------
// Layouts and ids
// ---------------------------------------------------------------------------

constexpr std::size_t GameDataContextOffset = 0x106;   // Game+106h, uint8
constexpr std::size_t SkillRecordsOffset    = 0x11B0;  // DataTables+11B0h, skills.txt rows
constexpr std::size_t SkillCountOffset      = 0x11B8;  // DataTables+11B8h, row count, uint64
constexpr std::size_t SkillRecordSize       = 0x2EC;
constexpr std::size_t SkillIdOffset         = 0x000;   // int16, the row's skill id
constexpr std::size_t SkillItypeA1Offset    = 0x038;   // int16, skills.txt itypea1
constexpr std::size_t SkillReqLevelOffset   = 0x214;   // word, skills.txt reqlevel

constexpr std::uint8_t  MaxDataContext      = 3;
constexpr std::uint8_t  RotwDataContext     = 3;       // Reign of the Warlock
constexpr std::uint16_t ExpansionItemFormat = 100;
constexpr std::int32_t  QualityLow          = 1;
constexpr std::int32_t  LowQualityTierCap   = 4;
constexpr std::int32_t  StatSingleSkill     = 107;     // item_singleskill
constexpr std::uint32_t StaffmodListFlag    = 0x40;
constexpr std::int32_t  UnitTypeItem        = 4;
constexpr std::int32_t  MaxStaffmods        = 3;
constexpr std::int32_t  TriesPerStaffmod    = 6;
constexpr std::int32_t  SkillsPerTier       = 5;       // Lord of Destruction row layout
constexpr std::int32_t  MinimumCap          = 1;
constexpr std::int32_t  NoSkill             = -1;

constexpr std::int32_t                     TierCount = 6;
constexpr std::array<std::int32_t, TierCount> TierLevels { 1, 6, 12, 18, 24, 30 };

constexpr std::uint32_t BucketCapacity = 256;

struct Buckets {
	std::array<std::array<std::int32_t, BucketCapacity>, TierCount> ids {};
	std::array<std::uint32_t, TierCount>                           size {};
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

const D2RL::PluginContext* Context = nullptr;
std::uintptr_t             Base    = 0;
bool                       Hooked  = false;

std::atomic<std::uint64_t> ItemsRolled { 0 };
std::atomic<std::uint64_t> ItemsWithNone { 0 };
std::atomic<std::uint64_t> StaffmodsGiven { 0 };
std::atomic<std::uint64_t> TiersLowered { 0 };
std::atomic<std::uint64_t> NothingUnderCap { 0 };
std::atomic<std::uint64_t> EmptyStaffmods { 0 };
std::atomic<std::uint64_t> TriesOverCap { 0 };
std::atomic<bool>          BucketOverflow { false };

struct LastRoll {
	bool                                    valid         = false;
	std::int32_t                            context       = 0;
	std::int32_t                            itemLevel     = 0;
	std::int32_t                            requiredLevel = 0;
	std::int32_t                            cap           = 0;
	std::int32_t                            baseTier      = 0;
	std::int32_t                            capTier       = 0;
	std::int32_t                            count         = 0;
	std::array<std::int32_t, MaxStaffmods> skills { NoSkill, NoSkill, NoSkill };
	std::array<std::int32_t, MaxStaffmods> values {};
};

std::mutex LastMutex;
LastRoll   Last;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

template <typename T>
auto At(std::uint64_t rva) noexcept -> T {
	return reinterpret_cast<T>(Base + rva);
}

template <std::size_t Size>
constexpr auto ByteCount(const std::uint8_t (&)[Size]) noexcept -> std::uint32_t {
	return static_cast<std::uint32_t>(Size);
}

template <typename T>
auto ReadField(const void* base, std::size_t offset) noexcept -> T {
	T value {};
	std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(T));
	return value;
}

void LogInfo(const char* message) noexcept {
	if (Context != nullptr) Context->LogInfo(message);
}

void LogError(const char* message) noexcept {
	if (Context != nullptr) Context->LogError(message);
}

struct SkillTable {
	const std::uint8_t* records = nullptr;
	std::uint64_t       count   = 0;

	auto Record(std::int32_t id) const noexcept -> const std::uint8_t* {
		if (records == nullptr || id < 0 || static_cast<std::uint64_t>(id) >= count) return nullptr;
		return records + static_cast<std::size_t>(id) * SkillRecordSize;
	}
};

auto Skills(std::uint8_t context) noexcept -> SkillTable {
	const std::uint8_t* tables = DataTables(context);
	if (tables == nullptr) return {};
	return { ReadField<const std::uint8_t*>(tables, SkillRecordsOffset), ReadField<std::uint64_t>(tables, SkillCountOffset) };
}

auto ReqLevel(const std::uint8_t* record) noexcept -> std::int32_t {
	return ReadField<std::uint16_t>(record, SkillReqLevelOffset);
}

// ---------------------------------------------------------------------------
// The roll
// ---------------------------------------------------------------------------

// Reign of the Warlock tiers: the class skills by exact reqlevel, in row order.
// A class skill id without a row ends the routine, as in vanilla.
auto FillBuckets(const SkillTable& skills, std::int32_t firstSkill, std::int32_t skillCount, Buckets& buckets) noexcept
	-> bool {
	for (std::int32_t id = firstSkill; id < firstSkill + skillCount; ++id) {
		const std::uint8_t* record = skills.Record(id);
		if (record == nullptr) return false;
		const std::int32_t level = ReadField<std::int16_t>(record, SkillReqLevelOffset);
		for (std::int32_t tier = 0; tier < TierCount; ++tier) {
			if (level != TierLevels[static_cast<std::size_t>(tier)]) continue;
			std::uint32_t& size = buckets.size[static_cast<std::size_t>(tier)];
			if (size < BucketCapacity) {
				buckets.ids[static_cast<std::size_t>(tier)][size++] = ReadField<std::int16_t>(record, SkillIdOffset);
			} else {
				BucketOverflow.store(true, std::memory_order_relaxed);
			}
			break;
		}
	}
	return true;
}

// Lord of Destruction tiers: five rows per tier from the first class skill.
auto RowBlockHasSkillAtOrBelow(const SkillTable& skills, std::int32_t firstSkill, std::int32_t tier, std::int32_t cap) noexcept
	-> bool {
	const std::int32_t start = firstSkill + SkillsPerTier * (tier - 1);
	for (std::int32_t i = 0; i < SkillsPerTier; ++i) {
		const std::uint8_t* record = skills.Record(start + i);
		if (record != nullptr && ReqLevel(record) <= cap) return true;
	}
	return false;
}

struct RollInput {
	void*                                   item;
	std::uint32_t*                          seed;
	const SkillTable*                       skills;
	const Buckets*                          buckets;
	bool                                    rotw;
	std::int32_t                            firstSkill;
	std::int32_t                            cap;
	const std::array<std::int32_t, MaxStaffmods>* chosen;
};

// Up to six tries in the tier, vanilla order: candidate, then its row checks.
auto PickSkill(const RollInput& in, std::int32_t tier) noexcept -> std::int32_t {
	for (std::int32_t attempt = 0; attempt < TriesPerStaffmod; ++attempt) {
		std::int32_t candidate = NoSkill;
		if (in.rotw) {
			const std::size_t   index = static_cast<std::size_t>(tier - 1);
			const std::uint32_t size  = in.buckets->size[index];
			if (size == 0) return NoSkill;
			const std::uint32_t pick = Roll(in.seed, static_cast<std::int32_t>(size));
			if (pick >= size) continue;
			candidate = in.buckets->ids[index][pick];
		} else {
			candidate = in.firstSkill + SkillsPerTier * (tier - 1)
			          + static_cast<std::int32_t>(Roll(in.seed, SkillsPerTier));
		}

		const std::uint8_t* record = in.skills->Record(candidate);
		if (record == nullptr) continue;
		if (ReqLevel(record) > in.cap) {
			TriesOverCap.fetch_add(1, std::memory_order_relaxed);
			continue;
		}
		const std::int16_t itemType = ReadField<std::int16_t>(record, SkillItypeA1Offset);
		if (itemType > 0 && CheckItemType(in.item, itemType) == 0) continue;
		const auto& chosen = *in.chosen;
		if (candidate == chosen[0] || candidate == chosen[1] || candidate == chosen[2]) continue;
		return candidate;
	}
	return NoSkill;
}

void AddStaffmod(std::uint8_t context, void* item, std::int32_t skill, std::int32_t value) noexcept {
	void* list = GetStatList(item, 0, StaffmodListFlag);
	if (list == nullptr) {
		list = AllocStatList(StaffmodListFlag, 0, UnitTypeItem, UnitId(item, __FILE__, __LINE__));
		if (list == nullptr) return;
		PostStatList(item, list, 1);
	}
	SetListStat(context, list, StatSingleSkill, value, static_cast<std::uint32_t>(skill));
}

void RollStaffmods(void* game, void* item, std::int32_t itemLevel, std::int32_t firstSkill, std::int32_t skillCount,
                   std::int32_t dropBonus) noexcept {
	if (game == nullptr || item == nullptr || skillCount <= 0) return;
	const std::uint8_t context = ReadField<std::uint8_t>(game, GameDataContextOffset);
	if (context > MaxDataContext) return;
	const SkillTable skills = Skills(context);
	std::uint32_t*   seed   = ItemSeed(item);
	if (skills.records == nullptr || seed == nullptr) return;

	ItemsRolled.fetch_add(1, std::memory_order_relaxed);

	// How many.
	const std::int32_t countRoll = dropBonus + static_cast<std::int32_t>(Roll(seed, 100));
	std::int32_t       count     = 0;
	if (countRoll > 90) {
		count = 3;
	} else if (countRoll > 70) {
		count = 2;
	} else if (countRoll > 30 || dropBonus != 0) {
		count = 1;
	}
	if (count == 0) {
		ItemsWithNone.fetch_add(1, std::memory_order_relaxed);
		return;
	}

	// Base tier from the item level.
	std::int32_t baseTier = 1;
	if (itemLevel > 36) {
		baseTier = ItemFormat(item) < ExpansionItemFormat ? 4 : 5;
	} else if (itemLevel > 24) {
		baseTier = 4;
	} else if (itemLevel > 18) {
		baseTier = 3;
	} else if (itemLevel > 11) {
		baseTier = 2;
	}

	// The level cap: Required Level before staffmods, at least 1.
	const std::int32_t requiredLevel = RequiredLevel(item, nullptr);
	const std::int32_t cap           = requiredLevel > MinimumCap ? requiredLevel : MinimumCap;

	const bool rotw = context == RotwDataContext;
	Buckets    buckets {};
	if (rotw && !FillBuckets(skills, firstSkill, skillCount, buckets)) return;

	// Highest tier at or below the cap that has skills for the class; 0 = none.
	std::int32_t capTier = 0;
	for (std::int32_t tier = TierCount; tier >= 1; --tier) {
		if (TierLevels[static_cast<std::size_t>(tier - 1)] > cap) continue;
		const bool hasSkills = rotw ? buckets.size[static_cast<std::size_t>(tier - 1)] > 0
		                            : RowBlockHasSkillAtOrBelow(skills, firstSkill, tier, cap);
		if (hasSkills) {
			capTier = tier;
			break;
		}
	}

	const std::int32_t quality    = ItemQuality(item);
	const bool         fixedValue = ItemFormat(item) >= ExpansionItemFormat && quality == QualityLow;

	std::array<std::int32_t, MaxStaffmods> chosen { NoSkill, NoSkill, NoSkill };
	std::array<std::int32_t, MaxStaffmods> values {};
	const RollInput input { item, seed, &skills, &buckets, rotw, firstSkill, cap, &chosen };

	for (std::int32_t slot = 0; slot < count; ++slot) {
		const std::int32_t tierRoll = static_cast<std::int32_t>(Roll(seed, 100));
		std::int32_t       tier     = baseTier - 2;
		if (tierRoll > 80) {
			tier = baseTier + 1;
		} else if (tierRoll > 30) {
			tier = baseTier;
		} else if (tierRoll > 10) {
			tier = baseTier - 1;
		}
		if (tier < 1) tier = 1;
		if (quality == QualityLow && tier > LowQualityTierCap) tier = LowQualityTierCap;
		if (tier > TierCount) tier = TierCount;

		if (TierLevels[static_cast<std::size_t>(tier - 1)] > cap) {
			if (capTier == 0) {
				NothingUnderCap.fetch_add(1, std::memory_order_relaxed);
				continue;
			}
			tier = capTier;
			TiersLowered.fetch_add(1, std::memory_order_relaxed);
		}

		const std::int32_t skill = PickSkill(input, tier);
		if (skill == NoSkill) {
			EmptyStaffmods.fetch_add(1, std::memory_order_relaxed);
			continue;
		}
		chosen[static_cast<std::size_t>(slot)] = skill;

		std::int32_t value = 1;
		if (!fixedValue) {
			const std::int32_t valueRoll = dropBonus / 2 + static_cast<std::int32_t>(Roll(seed, 100));
			if (valueRoll >= 90) {
				value = 3;
			} else if (valueRoll >= 60) {
				value = 2;
			}
		}
		values[static_cast<std::size_t>(slot)] = value;
		AddStaffmod(context, item, skill, value);
		StaffmodsGiven.fetch_add(1, std::memory_order_relaxed);
	}

	const std::lock_guard<std::mutex> guard(LastMutex);
	Last = LastRoll { true, context, itemLevel, requiredLevel, cap, baseTier, capTier, count, chosen, values };
}

// ---------------------------------------------------------------------------
// Hook
// ---------------------------------------------------------------------------

// Replaces 0x58AF50. Its caller passes the return value on; vanilla leaves
// whatever the last call returned, so 0 is as good as any.
std::uint64_t __fastcall HookStaffmods(void* game, void* item, std::int32_t itemLevel, std::int32_t firstSkill,
                                       std::int32_t skillCount, std::int32_t dropBonus) noexcept {
	RollStaffmods(game, item, itemLevel, firstSkill, skillCount, dropBonus);
	return 0;
}

// ---------------------------------------------------------------------------
// Config (documentation only)
// ---------------------------------------------------------------------------

constexpr char DefaultConfigToml[] = R"toml(# celestialrayone.staffmods
#
# Staffmods: the class skills an item rolls because of its item type
# (itemtypes.txt StaffMods), shown as "+N to <Skill> (<Class> Only)".
# Rolled once, on the server, when the item is created.
# Built for D2RLoader 1.3.1 (Diablo II: Resurrected 3.3).
# This plugin has no settings; this file documents what it does.
# Console command: staffmods (counters and the last item rolled).
#
# THE LEVEL CAP (what this plugin adds)
#   A staffmod can only be a skill whose skills.txt reqlevel is at or below
#   the item's Required Level as the item stands before its staffmods: base
#   item levelreq, magic, rare and crafted affixes, socketed items and the
#   item_levelreq stat. It is the value the item shows, without any class
#   adjustment. Below 1 counts as 1, so an item with no Required Level (most
#   normal tier items) rolls reqlevel 1 skills only.
#   Example: Required Level 8 allows reqlevel 1 and 6 skills, nothing higher.
#   So staffmods never raise an item's Required Level.
#
# HOW A STAFFMOD IS PICKED (as before, except where marked NEW)
#   Count: 0 to 3 per item, same roll as before.
#   Tier: from the item level (the level of the monster or area it dropped
#   from): 11 or lower tier 1, 12 to 18 tier 2, 19 to 24 tier 3, 25 to 36
#   tier 4, above 36 tier 5. Each staffmod then shifts it by -2 to +1
#   (lowest tier 1). Low quality items stay at tier 4 or below.
#   Tiers 1 to 6 are skills.txt reqlevel 1, 6, 12, 18, 24 and 30.
#   NEW: a tier above the item's Required Level drops to the highest tier at
#   or below it that has skills for the class.
#   Skill: one at random from the tier. A skill whose skills.txt itypea1 the
#   item is not, or one the item already rolled, is tried again, up to 6
#   tries. A tier with no skills for the class gives that staffmod nothing
#   (for example the Sorceress has no reqlevel 6 skill).
#   Value: +1 to +3, same roll as before.
#
# GAME MODES
#   Reign of the Warlock games: a tier is the class skills with exactly that
#   reqlevel; a class skill with any other reqlevel never rolls.
#   Lord of Destruction games: a tier is 5 class skills in row order, as
#   before; a skill above the Required Level counts as a failed try.
#   Items of Classic characters use a separate routine, left untouched.
)toml";

// ---------------------------------------------------------------------------
// Installation
// ---------------------------------------------------------------------------

auto CheckWitnesses() noexcept -> bool {
	struct Witness {
		std::uint64_t       rva;
		const std::uint8_t* bytes;
		std::uint32_t       size;
		const char*         name;
	};
	const Witness witnesses[] {
		{ ItemSeedRva, ItemSeedBytes, ByteCount(ItemSeedBytes), "item seed" },
		{ RollRva, RollBytes, ByteCount(RollBytes), "seed roll" },
		{ ItemFormatRva, ItemFormatBytes, ByteCount(ItemFormatBytes), "item format" },
		{ ItemQualityRva, ItemQualityBytes, ByteCount(ItemQualityBytes), "item quality" },
		{ CheckItemTypeRva, CheckItemTypeBytes, ByteCount(CheckItemTypeBytes), "item type check" },
		{ DataTablesRva, DataTablesBytes, ByteCount(DataTablesBytes), "data tables" },
		{ RequiredLevelRva, LoaderThunkBytes, ByteCount(LoaderThunkBytes), "Required Level" },
		{ GetStatListRva, GetStatListBytes, ByteCount(GetStatListBytes), "stat list lookup" },
		{ AllocStatListRva, AllocStatListBytes, ByteCount(AllocStatListBytes), "stat list allocation" },
		{ PostStatListRva, LoaderThunkBytes, ByteCount(LoaderThunkBytes), "stat list post" },
		{ SetListStatRva, LoaderThunkBytes, ByteCount(LoaderThunkBytes), "stat list write" },
		{ UnitIdRva, UnitIdBytes, ByteCount(UnitIdBytes), "unit id" },
	};

	bool ok = true;
	for (const Witness& witness : witnesses) {
		if (!Context->CheckExpectedBytes(witness.rva, witness.bytes, witness.size)) {
			char message[192] {};
			std::snprintf(message, sizeof(message), "Entry of the %s helper at 0x%llX does not match; wrong game build.",
			              witness.name, static_cast<unsigned long long>(witness.rva));
			LogError(message);
			ok = false;
		}
	}
	return ok;
}

void ResolveFunctions() noexcept {
	ItemSeed      = At<ItemSeedFn>(ItemSeedRva);
	Roll          = At<RollFn>(RollRva);
	ItemFormat    = At<ItemFormatFn>(ItemFormatRva);
	ItemQuality   = At<ItemQualityFn>(ItemQualityRva);
	CheckItemType = At<CheckItemTypeFn>(CheckItemTypeRva);
	DataTables    = At<DataTablesFn>(DataTablesRva);
	RequiredLevel = At<RequiredLevelFn>(RequiredLevelRva);
	GetStatList   = At<GetStatListFn>(GetStatListRva);
	AllocStatList = At<AllocStatListFn>(AllocStatListRva);
	PostStatList  = At<PostStatListFn>(PostStatListRva);
	SetListStat   = At<SetListStatFn>(SetListStatRva);
	UnitId        = At<UnitIdFn>(UnitIdRva);
}

// ---------------------------------------------------------------------------
// Console
// ---------------------------------------------------------------------------

auto __cdecl StatusCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept
	-> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr) return D2RL::ConsoleCommandResult::Failed;
	const D2RL::PluginContext* plugin = command->plugin;

	char line[512] {};
	std::snprintf(line, sizeof(line),
	              "Staffmods 1.0.1: hook %s | items rolled %llu (no staffmod rolled %llu) | staffmods given %llu | "
	              "tiers lowered to the cap %llu | nothing at or below the cap %llu | empty tier or 6 failed tries %llu "
	              "| tries above the cap %llu%s",
	              Hooked ? "on" : "OFF", static_cast<unsigned long long>(ItemsRolled.load(std::memory_order_relaxed)),
	              static_cast<unsigned long long>(ItemsWithNone.load(std::memory_order_relaxed)),
	              static_cast<unsigned long long>(StaffmodsGiven.load(std::memory_order_relaxed)),
	              static_cast<unsigned long long>(TiersLowered.load(std::memory_order_relaxed)),
	              static_cast<unsigned long long>(NothingUnderCap.load(std::memory_order_relaxed)),
	              static_cast<unsigned long long>(EmptyStaffmods.load(std::memory_order_relaxed)),
	              static_cast<unsigned long long>(TriesOverCap.load(std::memory_order_relaxed)),
	              BucketOverflow.load(std::memory_order_relaxed) ? " | A TIER HELD OVER 256 SKILLS; THE REST WERE SKIPPED"
	                                                             : "");
	plugin->WriteConsoleMessage(line);

	LastRoll last {};
	{
		const std::lock_guard<std::mutex> guard(LastMutex);
		last = Last;
	}
	if (!last.valid) {
		plugin->WriteConsoleMessage("last item: none with staffmods rolled yet");
		return D2RL::ConsoleCommandResult::Handled;
	}
	std::snprintf(line, sizeof(line),
	              "last item: data context %d, item level %d, Required Level %d (cap %d), base tier %d, highest tier "
	              "under the cap %d, count %d | skills %d (+%d), %d (+%d), %d (+%d)",
	              last.context, last.itemLevel, last.requiredLevel, last.cap, last.baseTier, last.capTier, last.count,
	              last.skills[0], last.values[0], last.skills[1], last.values[1], last.skills[2], last.values[2]);
	plugin->WriteConsoleMessage(line);
	return D2RL::ConsoleCommandResult::Handled;
}

constexpr D2RL::PluginInfo StaffmodsInfo {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = "celestialrayone.staffmods",
	.name        = "Staffmods",
	.version     = "1.0.1",
	.author      = "CelestialRayOne",
	.description = "Staffmods (itemtypes.txt StaffMods class skills) only roll skills whose skills.txt reqlevel is at or "
	               "below the item's Required Level (at least 1), so they never raise it.",
	.flags       = D2RL::PluginFlags::Server | D2RL::PluginFlags::NativeHooks,
};

}  // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
	return &StaffmodsInfo;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr) return false;
	Context = context;
	Base    = context->exeBase;
	if (Base == 0) {
		LogError("exeBase is zero; cannot resolve game functions.");
		return false;
	}

	if (!Context->EnsureConfig(DefaultConfigToml)) {
		LogError("Could not create celestialrayone.staffmods.toml (documentation only; nothing depends on it).");
	}

	if (!CheckWitnesses()) {
		LogError("Helper functions do not match this game build; staffmods stay vanilla.");
		return false;
	}
	ResolveFunctions();

	Hooked = Context->InstallInlineHook(StaffmodsRva, StaffmodsPrologue, ByteCount(StaffmodsPrologue),
	                                    reinterpret_cast<void*>(&HookStaffmods), nullptr);
	if (!Hooked) {
		LogError("Inline hook on the staffmods routine at 0x58AF50 failed: wrong game build, or its first bytes are not "
		         "the expected ones (another plugin may own it). Staffmods stay vanilla.");
		return false;
	}
	LogInfo("Hooked the staffmods routine at 0x58AF50: staffmods are capped at the item's Required Level (at least 1).");

	if (!Context->RegisterConsoleCommand("staffmods", &StatusCommand,
	                                     "Staffmods status: hook, counters and the last item rolled.")) {
		LogError("Could not register the staffmods console command.");
	}
	return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
	Hooked  = false;
	Context = nullptr;
}
