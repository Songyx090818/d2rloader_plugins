#pragma once
//
// Enhanced Damage Fix: the commit decision, as a pure function.
//
// The stat engine runs this decision once per target stat while it re-evaluates
// a percent stat whose itemstatcost `op` is 13 (item_maxdamage_percent and
// item_mindamage_percent; item_armor_percent and item_maxdurability_percent use
// op 13 as well). The answer says whether the percent stat may be committed to
// its own statlist, which is what carries it up to the socket host, then to the
// character.
//
// Native rule:  withhold on every statlist owned by an item.
// Fixed rule:   for the physical damage targets, withhold only when op 13 really
//               multiplied something on this item, i.e. the owning item has a
//               base value of that target stat. Only weapons have one.
//
// Everything that is not a physical damage target keeps the native answer.
//

#include <cstdint>

namespace CelestialRayOne::EnhancedDamageFix {

inline constexpr std::uint32_t UnitTypeItem = 4;

// itemstatcost.txt ids of the six stats the two Enhanced Damage stats write into
// through op 13 (op stat1..3 of item_mindamage_percent / item_maxdamage_percent).
inline constexpr std::uint32_t StatMinDamage          = 21;   // mindamage
inline constexpr std::uint32_t StatMaxDamage          = 22;   // maxdamage
inline constexpr std::uint32_t StatSecondaryMinDamage = 23;   // secondary_mindamage
inline constexpr std::uint32_t StatSecondaryMaxDamage = 24;   // secondary_maxdamage
inline constexpr std::uint32_t StatThrowMinDamage     = 159;  // item_throw_mindamage
inline constexpr std::uint32_t StatThrowMaxDamage     = 160;  // item_throw_maxdamage

enum class Verdict : std::uint8_t {
    NotItemList,   // the statlist is not owned by an item: native answer, commit
    OtherStat,     // not a physical damage target (Enhanced Defense, durability): native answer, withhold
    OnItemBase,    // the item has base damage (a weapon): op 13 applied it there, withhold as native
    Repaired,      // item without base damage: native withholds and loses the stat, the fix commits it
};

constexpr auto IsPhysicalDamageStat(std::uint32_t stat) noexcept -> bool {
    return stat == StatMinDamage || stat == StatMaxDamage
        || stat == StatSecondaryMinDamage || stat == StatSecondaryMaxDamage
        || stat == StatThrowMinDamage || stat == StatThrowMaxDamage;
}

constexpr auto IsMinimumDamageStat(std::uint32_t stat) noexcept -> bool {
    return stat == StatMinDamage || stat == StatSecondaryMinDamage || stat == StatThrowMinDamage;
}

// listOwnerType  statlist +08h, the unit type the list belongs to
// targetStat     the op 13 target being decided (op stat1..3 of the percent stat)
// ownerIsItem    the list's owner unit (statlist +A0h) exists and is an item
// ownerBase      that item's own base value of targetStat at layer 0, the value
//                op 13 multiplies; ignored when ownerIsItem is false
constexpr auto Decide(std::uint32_t listOwnerType, std::uint32_t targetStat, bool ownerIsItem,
        std::int32_t ownerBase) noexcept -> Verdict {
    if (listOwnerType != UnitTypeItem) return Verdict::NotItemList;
    if (!IsPhysicalDamageStat(targetStat)) return Verdict::OtherStat;
    if (ownerIsItem && ownerBase != 0) return Verdict::OnItemBase;
    return Verdict::Repaired;
}

constexpr auto AllowsCommit(Verdict verdict) noexcept -> bool {
    return verdict == Verdict::NotItemList || verdict == Verdict::Repaired;
}

// The native answer at the same switch case: `cmp dword [list+8], 4 / setne al`.
constexpr auto NativeAllowsCommit(std::uint32_t listOwnerType) noexcept -> bool {
    return listOwnerType != UnitTypeItem;
}

// ---------------------------------------------------------------------------
// Compile-time checks: where the fix agrees with the native rule and where not
// ---------------------------------------------------------------------------

// Player, monster and every other non-item list: identical to native.
static_assert(AllowsCommit(Decide(0, StatMaxDamage, false, 0)) == NativeAllowsCommit(0));
static_assert(AllowsCommit(Decide(1, StatMinDamage, true, 50)) == NativeAllowsCommit(1));

// A weapon: the item has base damage, op 13 multiplied it, withheld exactly as native.
static_assert(Decide(UnitTypeItem, StatMaxDamage, true, 71) == Verdict::OnItemBase);
static_assert(AllowsCommit(Decide(UnitTypeItem, StatMaxDamage, true, 71)) == NativeAllowsCommit(UnitTypeItem));
static_assert(AllowsCommit(Decide(UnitTypeItem, StatSecondaryMaxDamage, true, 120)) == NativeAllowsCommit(UnitTypeItem));
static_assert(AllowsCommit(Decide(UnitTypeItem, StatThrowMinDamage, true, 6)) == NativeAllowsCommit(UnitTypeItem));

// Armor, jewel, charm, ring, amulet carrying flat damage: no base damage, the fix commits.
static_assert(Decide(UnitTypeItem, StatMaxDamage, true, 0) == Verdict::Repaired);
static_assert(Decide(UnitTypeItem, StatMinDamage, true, 0) == Verdict::Repaired);
static_assert(AllowsCommit(Decide(UnitTypeItem, StatMaxDamage, true, 0)));
static_assert(!NativeAllowsCommit(UnitTypeItem));

// No owner unit: op 13 cannot have applied anything, commit.
static_assert(Decide(UnitTypeItem, StatMaxDamage, false, 0) == Verdict::Repaired);

// Enhanced Defense (armorclass, 31) and max durability % (maxdurability, 73): native answer.
static_assert(Decide(UnitTypeItem, 31, true, 0) == Verdict::OtherStat);
static_assert(AllowsCommit(Decide(UnitTypeItem, 73, true, 0)) == NativeAllowsCommit(UnitTypeItem));

static_assert(IsMinimumDamageStat(StatMinDamage) && IsMinimumDamageStat(StatThrowMinDamage));
static_assert(!IsMinimumDamageStat(StatMaxDamage) && !IsMinimumDamageStat(StatSecondaryMaxDamage));

}  // namespace CelestialRayOne::EnhancedDamageFix
