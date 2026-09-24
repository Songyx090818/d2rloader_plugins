// =============================================================================
//  Aura Reactivation  -  celestialrayone.aura-reactivation
//
//  Item auras (itemstatcost 151, item_aura) for D2R 3.3 under D2RLoader 1.3.0:
//
//    1. MULTIPLE AURAS PER ITEM. An item or a charm can carry several item_aura
//       stats and every one of them stays active.
//    2. AURAS COME BACK AFTER A RESURRECT for items the player still carries
//       (charms, the charm inventory page, anything else still merged).
//
//  Every address below was read out of the dumped D2RLoader 1.3.0 image and
//  disassembled before being relied on. Re-verified against 1.3.1: the aura
//  start (0x581970) and aura tick (0x437230) bodies differ only in two calls
//  each into D2RLoader's own thunk table, which moved from 0x3E2B57E to
//  0x3E2B652. Full-body byte witnesses guard the
//  three functions the plugin changes, so a different build refuses cleanly.
//
// -----------------------------------------------------------------------------
//  HOW AN ITEM AURA RUNS
// -----------------------------------------------------------------------------
//  One periodic event per aura, event type 9. EVENT_SetEvent (0x48B720 ->
//  0x48BE80) stores it on the unit's event list:
//
//      node + 0x00  type (byte)          node + 0x18  customId  = item unit id
//      node + 0x02  flags (word)         node + 0x1C  customParam = skill id
//      node + 0x38  next event of the same unit, list head at unit + 0x148
//
//  0x581970  ActivateItemAura(game, unit, itemId, skillId, level, castId)
//            delete (9, itemId) at 0x581A20, then SetEvent(9, .., itemId, skillId)
//  0x437230  the type 9 event handler (game, unit, itemId, skillId, castId)
//            re-arm: delete (9, itemId) at 0x4373EE, then SetEvent again
//            removal: delete (9, itemId) at 0x437444 (stat gone, unit dead...)
//  0x581AE0  DeactivateItemAura(game, unit, itemId, skillId)
//            clears the aura state, frees its stat list, delete (9, itemId)
//
//  D2RCore reimplements the stat-change callback (stat_change_runtime.cpp) and
//  calls the two functions above. During an item refresh transaction it also
//  restores an aura's timing itself: ActivateItemAura, then its own
//  delete (9, itemId) at D2RCore.dll+0x3CCDD0 (1.3.1), then SetEvent. The gear refresh
//  (0x470CA0) runs that transaction on every weapon swap, equip change and
//  zone change that refreshes items.
//
// -----------------------------------------------------------------------------
//  THE MULTI AURA DEFECT
// -----------------------------------------------------------------------------
//  EVENT_DeleteByTypeId (0x48B890) matches type and customId only, and a
//  customId of 0 matches every event of that type. Every delete above passes
//  the item id, so removing or refreshing one aura removes the events of all
//  other auras on the same item. Monprop auras have no item, D2RCore passes id
//  0, and one monprop aura wipes every item aura on the unit.
//
//  THE FIX: every one of those deletes removes only the event whose item id AND
//  skill id match. The engine's own delete still does the work: for the length
//  of the call, every other type 9 event it would have matched is given a type
//  it cannot match (0xFE, event types are 0..14), then restored.
//
//      0x581A20, 0x4373EE, 0x437444  call retargeted to a stub in a page the
//          plugin allocates; the stub hands the skill id to the plugin
//          (ebp at 0x581A20, r14d at both handler sites; each register is
//          written exactly once, at 0x58197A / 0x437250, and only restored
//          in the epilogue afterwards)
//      0x581AE0  inline hook, a line-for-line copy of the 240-byte body with
//          the item-and-skill delete. Its skill id does not survive to the
//          stock delete call, so the body cannot simply be patched.
//      0x48B890  inline hook. Only the call returning to D2RCore+0x3CCDD6 is
//          converted; the skill id is the flush record's skill D2RCore itself
//          loaded into [rsp+0x58] at +0x3CCD92 (1.3.1 offsets; the block is
//          byte-identical to 1.3.0 apart from its three slot displacements). The 147-byte flush block and
//          its function-pointer slots are verified before this is enabled.
//          Every other caller runs the stock delete untouched.
//
// -----------------------------------------------------------------------------
//  THE RESURRECT DEFECT
// -----------------------------------------------------------------------------
//  Player death (0x42E7C0) deletes every type 8 and type 9 event (0x42E820,
//  0x42E832). Equipped gear goes to the corpse and its auras start again when
//  the corpse is picked up, because re-equipping changes the stats. Charms stay
//  merged, nothing changes their stats, and nothing starts their auras again.
//
//  THE FIX: after the resurrect handler (0x4B6160) succeeds, every item whose
//  stat list is still attached to the player gets ActivateItemAura called once
//  per item_aura layer that has no event, with the same arguments D2RCore uses
//  (item unit id, skill, the player's total item_aura level, a new cast id).
//
//  The layers are read from the item's own stat list, in D2RCore's stat store
//  format (D2RCore replaced the game's stat storage; the game's 8-byte
//  entries are no longer used):
//
//      statList = [item+88h]; array = statList + ([statList+1Ch] < 0 ? A8h : 30h)
//      array = {entries, count}; entry = 16 bytes:
//          +00  uint64 key = statId << 32 | layer     (sorted by key)
//          +08  int32  value
//
//  Proven by D2RCore!ReadWideUnitStat (1.3.1: export +0x831DE0 -> core
//  +0x3D8BD0, key built at +0x3D8C28, 16-byte binary search at +0x3D8C7F,
//  value read at +0x3D8CB0), checked byte for byte at load. D2RCore's attach
//  sets the child list's parent at [list+78h] (+0x3DA5A8), so
//  STATLIST_GetOwner (0x2F8120) still names the player, and
//  RemoveWideDeathStats skips item lists (cmp [list+8],4 at +0x3DADAC), so
//  charms stay attached through death.
//
//  Zone changes delete no item aura events of their own. The only loss there
//  was the refresh transaction above, which the multi aura fix covers.
//
//  Left stock on purpose: 0x46F858, inside the item stat removal (0x46F710),
//  deletes every aura event of an item whose stats are leaving the unit. All
//  of that item's auras are going away there, so deleting by item is right.
//  In a refresh it first saves the item's aura timing (itemData+0x3C, the cast
//  id in item+0x12C), which D2RCore restores per aura through the flush.
//
//  Console command "aurareactivation" shows what is installed and counters.
//  Settings: d2rloader/config/celestialrayone.aura-reactivation.toml
// =============================================================================

#include <D2RLPlugin/api.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace CelestialRayOne::AuraReactivation {
namespace {

constexpr char PluginVersion[] = "1.0.4";

// ---------------------------------------------------------------------------
//  Default configuration. EnsureConfig writes this text on first load, and it
//  is the plugin's documentation as shipped.
// ---------------------------------------------------------------------------
constexpr const char DefaultConfigToml[] = R"TOML(# celestialrayone.aura-reactivation
#
# Aura Reactivation
#   1. An item or a charm can carry more than one item_aura stat, and every
#      one of those auras stays active.
#   2. Auras from items you still carry come back after you resurrect.
#
# Changes are read when the plugin loads. Restart the game after editing.
# Built for D2RLoader 1.3.0 (Diablo II: Resurrected 3.3). On any other build
# the byte checks fail and the plugin loads without changing the game.

# Master switch. false loads the plugin without touching the game.
enabled = true

# -----------------------------------------------------------------------------
# multi_aura_per_item
#
# The problem
#   Every item aura runs on its own periodic event, and that event remembers
#   which item granted the aura and which skill it casts. When the game starts,
#   refreshes or stops one of those events it deletes by item only, so the
#   events of every other aura on the same item go with it. Equipping the item,
#   a weapon swap, a gear refresh or the aura's own tick was enough to leave
#   the item with a single working aura.
#
#   Monster property auras (monprop, hireling auras) have no item at all. The
#   game gives them item 0, and it treats 0 as "every item aura on this unit",
#   so one monprop aura tick removed every item aura on a hireling.
#
# What changes
#   Those deletes now remove only the event of the aura they are about: the
#   same item and the same skill. When an aura starts, how often it ticks, what
#   it applies and when it stops are all unchanged.
#
# Where
#   aura start     call at 0x581A20 inside the aura activation (0x581970)
#   aura tick      calls at 0x4373EE and 0x437444 in the item aura event
#   aura stop      0x581AE0, replaced by an exact copy with the fixed delete
#   gear refresh   the D2RCore call that keeps an aura's timing through an
#                  item refresh (D2RCore.dll 1.3.0, checked byte for byte)
multi_aura_per_item = true

# -----------------------------------------------------------------------------
# restore_after_resurrect
#
# The problem
#   Dying deletes every aura event on the player. Equipped gear goes to the
#   corpse and starts its auras again when the corpse is picked up. Charms stay
#   in the inventory, their stats never change, and nothing starts their auras
#   again.
#
# What changes
#   Right after a successful resurrect, every item still active on the player
#   (charms, the charm inventory page, any other item that stayed on the
#   character) starts its item_aura events again through the game's own aura
#   activation, one per aura. Auras that already have an event are left alone.
#
#   This is the whole fix for charm auras: zone changes do not delete aura
#   events, the only loss there was the gear refresh fixed above.
restore_after_resurrect = true

# Writes a line to the plugin log for the first 64 auras kept or restored.
# Keep false for normal play. The console command "aurareactivation" always
# shows what is installed and the counters.
diagnostics = false
)TOML";

// ---------------------------------------------------------------------------
//  Native layout (D2R 3.3, D2RLoader 1.3.0 process image)
// ---------------------------------------------------------------------------
constexpr std::size_t   GameDataContextOffset      = 0x106;  // byte, movzx ecx,[rsi+106h]
constexpr std::size_t   UnitTypeOffset             = 0x00;   // dword
constexpr std::size_t   UnitIdOffset               = 0x08;   // dword, the event custom id
constexpr std::size_t   SkillRecordFlagsByteOffset = 0x24;   // tested against the aura mask
constexpr std::size_t   SkillRecordAuraStateOffset = 0xA0;   // int16 aurastate
constexpr std::size_t   StatListFlagsOffset        = 0x1C;   // bit 31: extended list
constexpr std::size_t   StatListBaseStatsOffset    = 0x30;   // {entries, count}, D2RCore stat store
constexpr std::size_t   StatListExtendedDelta      = 0x78;   // extended lists use +0xA8
constexpr std::uint32_t ItemAuraStatId             = 151;
constexpr std::int32_t  UnitTypePlayer             = 0;
constexpr std::int32_t  UnitTypeItem               = 4;
constexpr std::size_t   MaximumInventoryItems      = 4096;
constexpr std::uint64_t MaximumStatsPerList        = 4096;

// ---------------------------------------------------------------------------
//  Native RVAs
// ---------------------------------------------------------------------------
constexpr std::uint64_t DeleteEventsByTypeRva     = 0x48B890;  // EVENT_DeleteByTypeId
constexpr std::uint64_t DeleteEventsWalkRva       = 0x48B8F0;  // its node walk
constexpr std::uint64_t GetUnitEventListRva       = 0x48FE50;  // returns [unit+148h]
constexpr std::uint64_t SetEventStoreRva          = 0x48BF55;  // node field stores
constexpr std::uint64_t ActivateItemAuraRva       = 0x581970;
constexpr std::uint64_t ActivateDeleteCallRva     = 0x581A20;
constexpr std::uint64_t ItemAuraEventRva          = 0x437230;
constexpr std::uint64_t EventRearmDeleteCallRva   = 0x4373EE;
constexpr std::uint64_t EventRemoveDeleteCallRva  = 0x437444;
constexpr std::uint64_t DeactivateItemAuraRva     = 0x581AE0;
constexpr std::uint64_t ResurrectHandlerRva       = 0x4B6160;
constexpr std::uint64_t ResurrectSuccessTailRva   = 0x4B6685;
constexpr std::uint64_t AuraFlagMaskRva           = 0x1D996E4; // test byte [rip+..],cl

constexpr std::uint64_t GetSkillsTxtRecordRva     = 0x097790;
constexpr std::uint64_t GetStateCountRva          = 0x2141A0;
constexpr std::uint64_t ToggleStateRva            = 0x3354C0;
constexpr std::uint64_t GetStateStatListRva       = 0x2F5940;
constexpr std::uint64_t UnlinkStatListRva         = 0x2F7920;
constexpr std::uint64_t FreeStatListRva           = 0x2F4180;
constexpr std::uint64_t GetStatListOwnerRva       = 0x2F8120;
constexpr std::uint64_t GetUnitStatRva            = 0x2F5020;  // loader thunk to ReadWideUnitStat
constexpr std::uint64_t GetUnitTypeRva            = 0x34B9D0;
constexpr std::uint64_t IsUnitDeadRva             = 0x34C2C0;
constexpr std::uint64_t GetStatListExRva          = 0x34B870;
constexpr std::uint64_t GetInventoryRva           = 0x34A360;
constexpr std::uint64_t GetFirstItemRva           = 0x388C10;
constexpr std::uint64_t GetNextItemRva            = 0x38ABA0;
constexpr std::uint64_t NextGameCounterRva        = 0x404270;

// D2RCore.dll (D2RLoader 1.3.0)
constexpr wchar_t       CoreModuleName[]            = L"D2RCore.dll";
constexpr std::uint64_t CoreFlushWindowRva          = 0x3CCD6A;
constexpr std::uint64_t CoreFlushActivateCallRva    = 0x3CCDB7;  // call [slot] -> 0x581970
constexpr std::uint64_t CoreFlushDeleteCallRva      = 0x3CCDD0;  // call [slot] -> 0x48B890
constexpr std::uint64_t CoreFlushSetEventCallRva    = 0x3CCDF7;  // call [slot] -> 0x48B720
constexpr std::uint64_t CoreFlushDeleteReturnRva    = 0x3CCDD6;
constexpr std::size_t   CoreFlushSkillSlot          = 0x58;      // [rsp+58h] at the call
constexpr std::uint64_t CoreReadWideUnitStatRva     = 0x831DE0;  // export, calls the core below
constexpr std::uint64_t CoreWideStatLayoutRva       = 0x3D8BE6;  // ReadWideUnitStat core body
constexpr std::uint64_t EventSetEventRva            = 0x48B720;

// ---------------------------------------------------------------------------
//  Byte witnesses, read from the image
// ---------------------------------------------------------------------------
constexpr auto ActivateItemAuraBody = std::to_array<std::uint8_t>({
    0x40, 0x55, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x48, 0x41, 0x8B,
    0xE9, 0x45, 0x8B, 0xF0, 0x48, 0x8B, 0xFA, 0x48, 0x8B, 0xF1, 0x48, 0x85,
    0xD2, 0x75, 0x21, 0x48, 0x8D, 0x4C, 0x24, 0x78, 0x88, 0x54, 0x24, 0x78,
    0xE8, 0xC7, 0xFB, 0xFF, 0xFF, 0x84, 0xC0, 0x0F, 0x84, 0x32, 0x01, 0x00,
    0x00, 0xCC, 0x48, 0x83, 0xC4, 0x48, 0x41, 0x5E, 0x5F, 0x5E, 0x5D, 0xC3,
    0x0F, 0xB6, 0x89, 0x06, 0x01, 0x00, 0x00, 0x8B, 0xD5, 0x48, 0x89, 0x5C,
    0x24, 0x70, 0xE8, 0xD1, 0x5D, 0xB1, 0xFF, 0x48, 0x8B, 0xD8, 0x48, 0x85,
    0xC0, 0x0F, 0x84, 0x03, 0x01, 0x00, 0x00, 0x0F, 0xB6, 0x48, 0x24, 0x84,
    0x0D, 0x0F, 0x7D, 0x81, 0x01, 0x0F, 0x84, 0xF3, 0x00, 0x00, 0x00, 0x66,
    0x83, 0xB8, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x8C, 0xE5, 0x00, 0x00,
    0x00, 0x0F, 0xB6, 0x8E, 0x06, 0x01, 0x00, 0x00, 0xE8, 0xAB, 0x27, 0xC9,
    0xFF, 0x0F, 0xBF, 0x8B, 0xA0, 0x00, 0x00, 0x00, 0x3B, 0xC8, 0x0F, 0x8D,
    0xCA, 0x00, 0x00, 0x00, 0x4C, 0x89, 0xA4, 0x24, 0x80, 0x00, 0x00, 0x00,
    0x45, 0x8B, 0xCE, 0x41, 0xB8, 0x09, 0x00, 0x00, 0x00, 0x4C, 0x89, 0x7C,
    0x24, 0x40, 0x48, 0x8B, 0xD7, 0x48, 0x8B, 0xCE, 0xE8, 0x6B, 0x9E, 0xF0,
    0xFF, 0x44, 0x8B, 0xBC, 0x24, 0x90, 0x00, 0x00, 0x00, 0x44, 0x8B, 0xCD,
    0x4C, 0x8B, 0xC3, 0x44, 0x89, 0x7C, 0x24, 0x20, 0x48, 0x8B, 0xD7, 0x48,
    0x8B, 0xCE, 0xE8, 0x7D, 0x32, 0xEB, 0xFF, 0x44, 0x8B, 0xA4, 0x24, 0x98,
    0x00, 0x00, 0x00, 0x44, 0x8B, 0xC8, 0x44, 0x89, 0x64, 0x24, 0x30, 0x41,
    0xB8, 0x09, 0x00, 0x00, 0x00, 0x89, 0x6C, 0x24, 0x28, 0x48, 0x8B, 0xD7,
    0x48, 0x8B, 0xCE, 0x44, 0x89, 0x74, 0x24, 0x20, 0xE8, 0xB3, 0x9C, 0xF0,
    0xFF, 0x0F, 0xB6, 0x43, 0x25, 0x84, 0x05, 0x75, 0x7C, 0x81, 0x01, 0x74,
    0x48, 0x48, 0x8B, 0xCF, 0xE8, 0x3F, 0x9C, 0xDC, 0xFF, 0x41, 0x8B, 0xD4,
    0x48, 0x8B, 0xCF, 0x8B, 0xD8, 0xE8, 0xC4, 0x9B, 0x8A, 0x03, 0xC7, 0x44,
    0x24, 0x30, 0x00, 0x00, 0x00, 0x00, 0x45, 0x8B, 0xCF, 0xC7, 0x44, 0x24,
    0x28, 0x01, 0x00, 0x00, 0x00, 0x44, 0x8B, 0xC5, 0x48, 0x8B, 0xD7, 0xC7,
    0x44, 0x24, 0x20, 0x01, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xCE, 0xE8, 0xF9,
    0x91, 0xEB, 0xFF, 0x8B, 0xD3, 0x48, 0x8B, 0xCF, 0xE8, 0x91, 0x9B, 0x8A,
    0x03, 0x4C, 0x8B, 0xA4, 0x24, 0x80, 0x00, 0x00, 0x00, 0x4C, 0x8B, 0x7C,
    0x24, 0x40, 0x48, 0x8B, 0x5C, 0x24, 0x70, 0x48, 0x83, 0xC4, 0x48, 0x41,
    0x5E, 0x5F, 0x5E, 0x5D, 0xC3,
});

constexpr auto ItemAuraEventBody = std::to_array<std::uint8_t>({
    0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56,
    0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B, 0xF2, 0x48, 0x8B, 0xE9, 0x0F, 0xB6,
    0x89, 0x06, 0x01, 0x00, 0x00, 0x41, 0x8B, 0xD1, 0x45, 0x8B, 0xF1, 0x45,
    0x8B, 0xE0, 0xE8, 0x35, 0x05, 0xC6, 0xFF, 0x48, 0x8B, 0xF8, 0x48, 0x85,
    0xC0, 0x0F, 0x84, 0xCE, 0x01, 0x00, 0x00, 0x66, 0x83, 0xB8, 0xA0, 0x00,
    0x00, 0x00, 0x00, 0x0F, 0x8C, 0xC0, 0x01, 0x00, 0x00, 0x0F, 0xB6, 0x8D,
    0x06, 0x01, 0x00, 0x00, 0xE8, 0x0F, 0x98, 0xEC, 0xFF, 0x48, 0x8B, 0x98,
    0x98, 0x02, 0x00, 0x00, 0x48, 0x63, 0xCB, 0x48, 0x3B, 0xCB, 0x75, 0x04,
    0x85, 0xDB, 0x79, 0x1A, 0x48, 0x8D, 0x8C, 0x24, 0x80, 0x00, 0x00, 0x00,
    0xC6, 0x84, 0x24, 0x80, 0x00, 0x00, 0x00, 0x00, 0xE8, 0xB7, 0xF6, 0xC4,
    0xFF, 0x84, 0xC0, 0x74, 0x01, 0xCC, 0x0F, 0xBF, 0x87, 0xA0, 0x00, 0x00,
    0x00, 0x3B, 0xC3, 0x0F, 0x8D, 0x78, 0x01, 0x00, 0x00, 0x48, 0x8B, 0xCE,
    0xE8, 0xFB, 0x4F, 0xF1, 0xFF, 0x85, 0xC0, 0x0F, 0x85, 0x68, 0x01, 0x00,
    0x00, 0x45, 0x0F, 0xB7, 0xC6, 0xBA, 0x97, 0x00, 0x00, 0x00, 0x48, 0x8B,
    0xCE, 0xE8, 0x42, 0xDD, 0xEB, 0xFF, 0x8B, 0xD8, 0x85, 0xC0, 0x0F, 0x8E,
    0x4D, 0x01, 0x00, 0x00, 0x4C, 0x89, 0x6C, 0x24, 0x70, 0x4C, 0x89, 0x7C,
    0x24, 0x78, 0x44, 0x8B, 0xBC, 0x24, 0x90, 0x00, 0x00, 0x00, 0x41, 0x8D,
    0x4F, 0x01, 0xF7, 0xC1, 0xFE, 0xFF, 0xFF, 0xFF, 0x75, 0x1A, 0x48, 0x8D,
    0x8C, 0x24, 0x90, 0x00, 0x00, 0x00, 0xC6, 0x84, 0x24, 0x90, 0x00, 0x00,
    0x00, 0x00, 0xE8, 0xD5, 0xA9, 0xFF, 0xFF, 0x84, 0xC0, 0x74, 0x01, 0xCC,
    0x48, 0x8B, 0xCE, 0xE8, 0x98, 0x43, 0xF1, 0xFF, 0x41, 0x8B, 0xD7, 0x48,
    0x8B, 0xCE, 0x44, 0x8B, 0xE8, 0xE8, 0x1C, 0x43, 0x9F, 0x03, 0xBA, 0x00,
    0x00, 0x00, 0x20, 0x41, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xCE,
    0xE8, 0xF7, 0x6D, 0xF1, 0xFF, 0xC7, 0x44, 0x24, 0x30, 0x00, 0x00, 0x00,
    0x00, 0x44, 0x8B, 0xCB, 0xC7, 0x44, 0x24, 0x28, 0x01, 0x00, 0x00, 0x00,
    0x45, 0x8B, 0xC6, 0x48, 0x8B, 0xD6, 0xC7, 0x44, 0x24, 0x20, 0x01, 0x00,
    0x00, 0x00, 0x48, 0x8B, 0xCD, 0xE8, 0x3E, 0x39, 0x00, 0x00, 0x45, 0x33,
    0xC0, 0xBA, 0x00, 0x00, 0x00, 0x20, 0x48, 0x8B, 0xCE, 0xE8, 0xBE, 0x6D,
    0xF1, 0xFF, 0x0F, 0xB6, 0x8D, 0x06, 0x01, 0x00, 0x00, 0x41, 0x8B, 0xD6,
    0xE8, 0xFF, 0x03, 0xC6, 0xFF, 0x48, 0x85, 0xC0, 0x0F, 0x84, 0x84, 0x00,
    0x00, 0x00, 0x0F, 0xB6, 0x48, 0x24, 0x85, 0x0D, 0x44, 0x23, 0x96, 0x01,
    0x75, 0x08, 0x85, 0x0D, 0x38, 0x23, 0x96, 0x01, 0x74, 0x70, 0x44, 0x8B,
    0x80, 0x80, 0x01, 0x00, 0x00, 0x45, 0x8B, 0xCE, 0x0F, 0xB6, 0x8D, 0x06,
    0x01, 0x00, 0x00, 0x48, 0x8B, 0xD6, 0x89, 0x5C, 0x24, 0x20, 0xE8, 0x95,
    0xDD, 0xF7, 0xFF, 0x8B, 0xBD, 0x70, 0x01, 0x00, 0x00, 0xBB, 0x05, 0x00,
    0x00, 0x00, 0x3B, 0xC3, 0x45, 0x8B, 0xCC, 0x41, 0xB8, 0x09, 0x00, 0x00,
    0x00, 0x48, 0x8B, 0xD6, 0x0F, 0x4F, 0xD8, 0x48, 0x8B, 0xCD, 0xFF, 0xCF,
    0x03, 0xFB, 0xE8, 0x9D, 0x44, 0x05, 0x00, 0x8B, 0xC7, 0x44, 0x89, 0x7C,
    0x24, 0x30, 0x99, 0x44, 0x89, 0x74, 0x24, 0x28, 0xF7, 0xFB, 0x41, 0xB8,
    0x09, 0x00, 0x00, 0x00, 0x44, 0x89, 0x64, 0x24, 0x20, 0x2B, 0xFA, 0x48,
    0x8B, 0xCD, 0x48, 0x8B, 0xD6, 0x44, 0x8D, 0x4F, 0x01, 0xE8, 0x02, 0x43,
    0x05, 0x00, 0x41, 0x8B, 0xD5, 0x48, 0x8B, 0xCE, 0xE8, 0x29, 0x42, 0x9F,
    0x03, 0x4C, 0x8B, 0x7C, 0x24, 0x78, 0x4C, 0x8B, 0x6C, 0x24, 0x70, 0xEB,
    0x14, 0x45, 0x8B, 0xCC, 0x41, 0xB8, 0x09, 0x00, 0x00, 0x00, 0x48, 0x8B,
    0xD6, 0x48, 0x8B, 0xCD, 0xE8, 0x47, 0x44, 0x05, 0x00, 0x48, 0x8B, 0x9C,
    0x24, 0x88, 0x00, 0x00, 0x00, 0x48, 0x83, 0xC4, 0x40, 0x41, 0x5E, 0x41,
    0x5C, 0x5F, 0x5E, 0x5D, 0xC3, 0xCC, 0xCC, 0xCC,
});

constexpr auto DeactivateItemAuraBody = std::to_array<std::uint8_t>({
    0x40, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x45, 0x8B, 0xF0,
    0x48, 0x8B, 0xFA, 0x48, 0x8B, 0xF1, 0x48, 0x85, 0xD2, 0x75, 0x20, 0x48,
    0x8D, 0x4C, 0x24, 0x48, 0x88, 0x54, 0x24, 0x48, 0xE8, 0x9B, 0xF7, 0xFF,
    0xFF, 0x84, 0xC0, 0x0F, 0x84, 0xB8, 0x00, 0x00, 0x00, 0xCC, 0x48, 0x83,
    0xC4, 0x20, 0x41, 0x5E, 0x5F, 0x5E, 0xC3, 0x0F, 0xB6, 0x89, 0x06, 0x01,
    0x00, 0x00, 0x41, 0x8B, 0xD1, 0x48, 0x89, 0x5C, 0x24, 0x40, 0xE8, 0x65,
    0x5C, 0xB1, 0xFF, 0x48, 0x8B, 0xD8, 0x48, 0x85, 0xC0, 0x0F, 0x84, 0x89,
    0x00, 0x00, 0x00, 0x0F, 0xB6, 0x48, 0x24, 0x84, 0x0D, 0xA3, 0x7B, 0x81,
    0x01, 0x74, 0x7D, 0x66, 0x83, 0xB8, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x7C,
    0x73, 0x0F, 0xB6, 0x8E, 0x06, 0x01, 0x00, 0x00, 0x48, 0x89, 0x6C, 0x24,
    0x50, 0x0F, 0xBF, 0xA8, 0xA0, 0x00, 0x00, 0x00, 0xE8, 0x3B, 0x26, 0xC9,
    0xFF, 0x3B, 0xE8, 0x7D, 0x52, 0x45, 0x33, 0xC0, 0x8B, 0xD5, 0x48, 0x8B,
    0xCF, 0xE8, 0x4A, 0x39, 0xDB, 0xFF, 0x0F, 0xBF, 0x93, 0xA0, 0x00, 0x00,
    0x00, 0x48, 0x8B, 0xCF, 0xE8, 0xBB, 0x3D, 0xD7, 0xFF, 0x48, 0x8B, 0xD8,
    0x48, 0x85, 0xC0, 0x74, 0x1A, 0x48, 0x8B, 0xD0, 0x48, 0x8B, 0xCF, 0xE8,
    0x88, 0x5D, 0xD7, 0xFF, 0x0F, 0xB6, 0x8E, 0x06, 0x01, 0x00, 0x00, 0x48,
    0x8B, 0xD3, 0xE8, 0xD9, 0x25, 0xD7, 0xFF, 0x45, 0x8B, 0xCE, 0x41, 0xB8,
    0x09, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xD7, 0x48, 0x8B, 0xCE, 0xE8, 0xD5,
    0x9C, 0xF0, 0xFF, 0x48, 0x8B, 0x6C, 0x24, 0x50, 0x48, 0x8B, 0x5C, 0x24,
    0x40, 0x48, 0x83, 0xC4, 0x20, 0x41, 0x5E, 0x5F, 0x5E, 0xC3, 0xCC, 0xCC,
});

constexpr auto DeleteEventsWalkWitness = std::to_array<std::uint8_t>({
    0x48, 0x8B, 0x6B, 0x38, 0x4C, 0x39, 0x7B, 0x08, 0x74, 0x0F, 0x48, 0x8D,
    0x4C, 0x24, 0x60, 0xE8, 0x2C, 0xF3, 0xFF, 0xFF, 0x84, 0xC0, 0x74, 0x01,
    0xCC, 0x0F, 0xB6, 0x03, 0x41, 0x3B, 0xC4, 0x0F, 0x85, 0x44, 0x01, 0x00,
    0x00, 0x45, 0x85, 0xF6, 0x74, 0x0A, 0x44, 0x39, 0x73, 0x18, 0x0F, 0x85,
    0x35, 0x01, 0x00, 0x00,
});

constexpr auto ResurrectSuccessTailWitness = std::to_array<std::uint8_t>({
    0xBA, 0x01, 0x00, 0x00, 0x00, 0x49, 0x8B, 0xCD, 0xE8, 0xDE, 0x23, 0xF8,
    0xFF, 0x49, 0x8B, 0xCD, 0xE8, 0x66, 0x4D, 0xE9, 0xFF, 0x48, 0x8B, 0xF8,
    0x48, 0x85, 0xC0, 0x74, 0x2F, 0x48, 0x8B, 0xC8, 0xE8, 0x66, 0x65, 0xE8,
    0xFF, 0x41, 0xB8, 0x6E, 0x1A, 0x00, 0x00, 0x48, 0x8D, 0x15, 0x09, 0xD4,
    0x87, 0x01, 0x48, 0x8B, 0xCF, 0x8B, 0xD8, 0xE8, 0xBF, 0x79, 0xE8, 0xFF,
    0x44, 0x8B, 0xCB, 0x44, 0x8B, 0xC0, 0x33, 0xD2, 0x49, 0x8B, 0xCD, 0xE8,
    0x9F, 0x23, 0xF8, 0xFF, 0x33, 0xC0,
});

constexpr auto CoreFlushWindow = std::to_array<std::uint8_t>({
    0x48, 0x8B, 0x81, 0xF0, 0xA8, 0x01, 0x00, 0x4E, 0x8D, 0x24, 0xAD, 0x00,
    0x00, 0x00, 0x00, 0x4D, 0x01, 0xEC, 0x42, 0x8B, 0x14, 0xA0, 0x89, 0x54,
    0x24, 0x54, 0x42, 0x8B, 0x54, 0xA0, 0x08, 0x89, 0x54, 0x24, 0x5C, 0x42,
    0x8B, 0x54, 0xA0, 0x0C, 0x89, 0x54, 0x24, 0x58, 0x42, 0x8B, 0x5C, 0xA0,
    0x10, 0x89, 0x5C, 0x24, 0x28, 0x44, 0x89, 0x7C, 0x24, 0x20, 0x48, 0x89,
    0x4C, 0x24, 0x48, 0x48, 0x8B, 0x4C, 0x24, 0x48, 0x48, 0x89, 0xF2, 0x41,
    0x89, 0xE8, 0x41, 0x89, 0xF9, 0xFF, 0x15, 0xFB, 0x1F, 0x34, 0x00, 0x49,
    0x8B, 0x0E, 0x48, 0x89, 0xF2, 0x41, 0xB8, 0x09, 0x00, 0x00, 0x00, 0x8B,
    0x7C, 0x24, 0x54, 0x41, 0x89, 0xF9, 0xFF, 0x15, 0xCA, 0x37, 0x34, 0x00,
    0x49, 0x8B, 0x0E, 0x89, 0x5C, 0x24, 0x30, 0x8B, 0x44, 0x24, 0x58, 0x89,
    0x44, 0x24, 0x28, 0x89, 0x7C, 0x24, 0x20, 0x48, 0x89, 0xF2, 0x41, 0xB8,
    0x09, 0x00, 0x00, 0x00, 0x44, 0x8B, 0x4C, 0x24, 0x5C, 0xFF, 0x15, 0x8B,
    0x37, 0x34, 0x00,
});

constexpr auto CoreReadWideUnitStatExport = std::to_array<std::uint8_t>({
    0x48, 0x83, 0xEC, 0x28, 0xE8, 0xE7, 0x6D, 0xBA, 0xFF, 0x89, 0xC0, 0x48,
    0x83, 0xC4, 0x28, 0xC3,
});

constexpr auto CoreWideStatLayoutWindow = std::to_array<std::uint8_t>({
    0x48, 0x8B, 0xB1, 0x88, 0x00, 0x00, 0x00, 0x48, 0x85, 0xF6, 0x41, 0x0F,
    0x94, 0xC1, 0x81, 0xFA, 0x00, 0x80, 0x00, 0x00, 0x41, 0x0F, 0x93, 0xC2,
    0x45, 0x08, 0xCA, 0x0F, 0x85, 0xDB, 0x00, 0x00, 0x00, 0x44, 0x89, 0xC7,
    0x0F, 0xB6, 0x89, 0xBD, 0x01, 0x00, 0x00, 0x48, 0x89, 0xD3, 0xFF, 0x15,
    0xD6, 0x65, 0x32, 0x00, 0x48, 0x89, 0xC1, 0x31, 0xC0, 0x48, 0x85, 0xC9,
    0x0F, 0x84, 0xBA, 0x00, 0x00, 0x00, 0x48, 0xC1, 0xE3, 0x20, 0x41, 0x89,
    0xF8, 0x49, 0x09, 0xD8, 0x8B, 0x56, 0x1C, 0x85, 0xD2, 0xB8, 0xA8, 0x00,
    0x00, 0x00, 0x41, 0xB9, 0x30, 0x00, 0x00, 0x00, 0x4C, 0x0F, 0x48, 0xC8,
    0x4E, 0x8B, 0x5C, 0x0E, 0x08, 0x31, 0xC0, 0x4D, 0x85, 0xDB, 0x74, 0x45,
    0x4A, 0x8B, 0x3C, 0x0E, 0x45, 0x31, 0xD2, 0x4D, 0x89, 0xDE, 0x4C, 0x89,
    0xDB, 0xEB, 0x17, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x2E, 0x0F, 0x1F,
    0x84, 0x00, 0x00, 0x00, 0x00, 0x00, 0x49, 0x89, 0xDE, 0x48, 0x85, 0xDB,
    0x74, 0x22, 0x48, 0xD1, 0xEB, 0x4E, 0x8D, 0x3C, 0x13, 0x49, 0xC1, 0xE7,
    0x04, 0x4E, 0x39, 0x04, 0x3F, 0x73, 0xE7, 0x49, 0x01, 0xDA, 0x49, 0xFF,
    0xC2, 0x48, 0xF7, 0xD3, 0x4C, 0x01, 0xF3, 0xEB, 0xD9, 0x45, 0x31, 0xD2,
    0x4D, 0x39, 0xDA, 0x74, 0x43, 0x4E, 0x8B, 0x0C, 0x0E, 0x49, 0xC1, 0xE2,
    0x04, 0x4F, 0x39, 0x04, 0x11, 0x75, 0x35, 0x4D, 0x01, 0xD1, 0x41, 0x8B,
    0x41, 0x08,
});

constexpr auto DeleteEventsByTypeEntry = std::to_array<std::uint8_t>({
    0x40, 0x53, 0x56, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC,
    0x20, 0x45, 0x8B, 0xF1, 0x45, 0x8B, 0xE0, 0x4C, 0x8B, 0xFA, 0x48, 0x8B,
    0xF1, 0x41, 0x83, 0xF8, 0x0E,
});

constexpr auto GetUnitEventListBody = std::to_array<std::uint8_t>({
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x85, 0xC9, 0x75, 0x1A, 0x88, 0x4C, 0x24,
    0x30, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8, 0xB9, 0xDD, 0xFF, 0xFF, 0x84,
    0xC0, 0x74, 0x01, 0xCC, 0x33, 0xC0, 0x48, 0x83, 0xC4, 0x28, 0xC3, 0x48,
    0x8B, 0x81, 0x48, 0x01, 0x00, 0x00, 0x48, 0x83, 0xC4, 0x28, 0xC3,
});

constexpr auto SetEventStoreWitness = std::to_array<std::uint8_t>({
    0x8B, 0x4C, 0x24, 0x78, 0x44, 0x8B, 0xC5, 0x48, 0x8B, 0xD8, 0x89, 0x68,
    0x04, 0x89, 0x48, 0x18, 0x8B, 0x8C, 0x24, 0x80, 0x00, 0x00, 0x00, 0x89,
    0x48, 0x1C, 0x8B, 0x8C, 0x24, 0x88, 0x00, 0x00, 0x00, 0x89, 0x48, 0x20,
    0x48, 0x8B, 0x4C, 0x24, 0x70, 0x8B, 0x50, 0x14, 0x48, 0x89, 0x48, 0x48,
    0x48, 0x8B, 0xCE, 0x40, 0x88, 0x38,
});

constexpr auto ResurrectHandlerEntry = std::to_array<std::uint8_t>({
    0x40, 0x55, 0x41, 0x55, 0x41, 0x56, 0x48, 0x8D, 0x6C, 0x24, 0xB9, 0x48,
    0x81, 0xEC, 0xB0, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x05, 0x4F, 0x51, 0x51,
    0x02, 0x48, 0x33, 0xC4, 0x48, 0x89, 0x45, 0x17, 0x4C, 0x8B, 0xEA, 0x4C,
    0x8B, 0xF1, 0x41, 0x83, 0xF9, 0x01,
});

// The three call sites, stock: call 0x48B890.
constexpr auto ActivateDeleteCall      = std::to_array<std::uint8_t>({0xE8, 0x6B, 0x9E, 0xF0, 0xFF});
constexpr auto EventRearmDeleteCall    = std::to_array<std::uint8_t>({0xE8, 0x9D, 0x44, 0x05, 0x00});
constexpr auto EventRemoveDeleteCall   = std::to_array<std::uint8_t>({0xE8, 0x47, 0x44, 0x05, 0x00});

constexpr auto GetSkillsTxtRecordEntry = std::to_array<std::uint8_t>({
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48,
    0x83, 0xEC, 0x30, 0x48, 0x63, 0xF2, 0xE8, 0xE9, 0x92, 0x26, 0x00, 0x48,
});
constexpr auto GetStateCountEntry = std::to_array<std::uint8_t>({
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0xE8, 0xE5, 0xC8, 0x0E, 0x00, 0x48,
    0x8B, 0x98, 0x98, 0x02, 0x00, 0x00, 0x48, 0x63, 0xCB, 0x48, 0x3B, 0xCB,
});
constexpr auto ToggleStateEntry = std::to_array<std::uint8_t>({
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89,
    0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x41, 0x8B, 0xE8, 0x8B,
    0xDA, 0x48, 0x8B, 0xF1,
});
constexpr auto GetStateStatListEntry = std::to_array<std::uint8_t>({
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0xDA,
    0x48, 0x8B, 0xF9, 0x48, 0x85, 0xC9, 0x75, 0x13,
});
constexpr auto UnlinkStatListEntry = std::to_array<std::uint8_t>({
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xDA, 0xE8, 0xB2, 0x27,
    0x05, 0x00, 0x0F, 0xB6, 0xC8, 0x48, 0x8B, 0xD3, 0x48, 0x83, 0xC4, 0x20,
    0x5B, 0xE9,
});
constexpr auto FreeStatListEntry = std::to_array<std::uint8_t>({
    0x48, 0x85, 0xD2, 0x74, 0x0A, 0x83, 0x7A, 0x1C, 0x00, 0x0F, 0x8D, 0x21,
    0x5D, 0x00, 0x00, 0xC3,
});
constexpr auto GetStatListOwnerEntry = std::to_array<std::uint8_t>({
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xDA, 0x48, 0x85, 0xC9,
    0x75, 0x15,
});
constexpr auto GetUnitStatThunk = std::to_array<std::uint8_t>({
    0xFF, 0x25, 0xF2, 0x51, 0xB3, 0x03, 0x90, 0x90, 0x90, 0x90,
});
constexpr auto GetUnitTypeEntry = std::to_array<std::uint8_t>({
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x85, 0xC9, 0x75, 0x1D, 0x88, 0x4C, 0x24,
    0x30, 0x48, 0x8D, 0x4C,
});
constexpr auto IsUnitDeadEntry = std::to_array<std::uint8_t>({
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x85, 0xC9, 0x75, 0x1D, 0x88, 0x4C, 0x24,
    0x30, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8, 0x59, 0x94, 0xFF, 0xFF,
});
constexpr auto GetStatListExEntry = std::to_array<std::uint8_t>({
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9,
    0x75, 0x20,
});
constexpr auto GetInventoryEntry = std::to_array<std::uint8_t>({
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
    0xF1, 0x48, 0x85, 0xC9,
});
constexpr auto GetFirstItemEntry = std::to_array<std::uint8_t>({
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9,
    0x74, 0x2E,
});
constexpr auto GetNextItemEntry = std::to_array<std::uint8_t>({
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9,
    0x75, 0x10,
});
constexpr auto NextGameCounterEntry = std::to_array<std::uint8_t>({
    0x4C, 0x8B, 0xC1, 0x48, 0x85, 0xC9, 0x75, 0x06, 0xB8, 0xFF, 0xFF, 0xFF,
    0xFF, 0xC3, 0x8B, 0x91, 0x6C, 0x01, 0x00, 0x00,
});

// RELAY-EMITTER-BEGIN
// ---------------------------------------------------------------------------
//  Relay page layout. One page the plugin allocates within rel32 reach of the
//  image; nothing inside D2R's own code is used as a cave.
//
//      +0x000  UNWIND_INFO for the common frame (sub rsp,38h)
//      +0x008  UNWIND_INFO for the stubs (no frame)
//      +0x010  RUNTIME_FUNCTION table, 4 entries
//      +0x100  common:  sub rsp,38h
//                       mov [rsp+20h],r10d      ; skill id  -> 5th argument
//                       mov [rsp+28h],r11d      ; site      -> 6th argument
//                       mov rax,<bridge>
//                       call rax
//                       add rsp,38h
//                       ret                      ; back to the stock call site
//      +0x140  start stub:   mov r10d,ebp  / mov r11d,0 / jmp common
//      +0x160  tick stub:    mov r10d,r14d / mov r11d,1 / jmp common
//      +0x180  remove stub:  mov r10d,r14d / mov r11d,2 / jmp common
//
//  The stock call sites reach a stub with the stock arguments already in
//  rcx, rdx, r8d and r9d. r10 and r11 are volatile and dead at a call. The
//  site has rsp aligned before its call, so the common frame's 38h keeps the
//  bridge call aligned too.
// ---------------------------------------------------------------------------
constexpr std::size_t RelayPageBytes           = 0x1000;
constexpr std::size_t RelayCommonUnwindOffset  = 0x000;
constexpr std::size_t RelayStubUnwindOffset    = 0x008;
constexpr std::size_t RelayFunctionTableOffset = 0x010;
constexpr std::size_t RelayFunctionCount       = 4;
constexpr std::size_t RelayCommonOffset        = 0x100;
constexpr std::size_t RelayCommonSize          = 31;
constexpr std::size_t RelayStubSize            = 14;
constexpr std::array<std::size_t, 3> RelayStubOffsets{0x140, 0x160, 0x180};

inline void RelayPutU32(std::uint8_t* at, std::uint32_t value) noexcept {
    std::memcpy(at, &value, sizeof(value));
}

inline void RelayPutU64(std::uint8_t* at, std::uint64_t value) noexcept {
    std::memcpy(at, &value, sizeof(value));
}

// Writes the whole relay into a zeroed, writable page.
inline void EmitRelay(std::uint8_t* page, std::uint64_t bridge) noexcept {
    // UNWIND_INFO v1: prolog 4 bytes, one code UWOP_ALLOC_SMALL at +4,
    // (0x38 - 8) / 8 = 6.
    const std::uint8_t commonUnwind[8]{0x01, 0x04, 0x01, 0x00, 0x04, 0x62, 0x00, 0x00};
    const std::uint8_t stubUnwind[4]{0x01, 0x00, 0x00, 0x00};
    std::memcpy(page + RelayCommonUnwindOffset, commonUnwind, sizeof(commonUnwind));
    std::memcpy(page + RelayStubUnwindOffset, stubUnwind, sizeof(stubUnwind));

    const auto putFunction = [page](std::size_t index, std::size_t begin, std::size_t size, std::size_t unwind) noexcept {
        auto* at = page + RelayFunctionTableOffset + index * 12;
        RelayPutU32(at, static_cast<std::uint32_t>(begin));
        RelayPutU32(at + 4, static_cast<std::uint32_t>(begin + size));
        RelayPutU32(at + 8, static_cast<std::uint32_t>(unwind));
    };
    putFunction(0, RelayCommonOffset, RelayCommonSize, RelayCommonUnwindOffset);

    auto* common = page + RelayCommonOffset;
    const std::uint8_t commonHead[]{
        0x48, 0x83, 0xEC, 0x38,        // sub  rsp,38h
        0x44, 0x89, 0x54, 0x24, 0x20,  // mov  [rsp+20h],r10d
        0x44, 0x89, 0x5C, 0x24, 0x28,  // mov  [rsp+28h],r11d
        0x48, 0xB8,                    // mov  rax,imm64
    };
    const std::uint8_t commonTail[]{
        0xFF, 0xD0,                    // call rax
        0x48, 0x83, 0xC4, 0x38,        // add  rsp,38h
        0xC3,                          // ret
    };
    std::memcpy(common, commonHead, sizeof(commonHead));
    RelayPutU64(common + sizeof(commonHead), bridge);
    std::memcpy(common + sizeof(commonHead) + 8, commonTail, sizeof(commonTail));

    const std::uint8_t skillLoads[3][3]{
        {0x41, 0x89, 0xEA},            // mov r10d,ebp
        {0x45, 0x89, 0xF2},            // mov r10d,r14d
        {0x45, 0x89, 0xF2},            // mov r10d,r14d
    };
    for (std::size_t index = 0; index < RelayStubOffsets.size(); ++index) {
        auto* stub = page + RelayStubOffsets[index];
        std::memcpy(stub, skillLoads[index], 3);
        stub[3] = 0x41;                // mov r11d,imm32
        stub[4] = 0xBB;
        RelayPutU32(stub + 5, static_cast<std::uint32_t>(index));
        stub[9] = 0xE9;                // jmp common
        const auto rel = static_cast<std::int64_t>(RelayCommonOffset)
                       - static_cast<std::int64_t>(RelayStubOffsets[index] + RelayStubSize);
        RelayPutU32(stub + 10, static_cast<std::uint32_t>(static_cast<std::int32_t>(rel)));
        putFunction(index + 1, RelayStubOffsets[index], RelayStubSize, RelayStubUnwindOffset);
    }
}
// RELAY-EMITTER-END

// EVENT-LOGIC-BEGIN
// ---------------------------------------------------------------------------
//  Item and skill delete on top of the engine's own delete.
// ---------------------------------------------------------------------------
constexpr std::size_t  UnitEventListOffset      = 0x148;
constexpr std::size_t  EventTypeOffset          = 0x00;
constexpr std::size_t  EventCustomIdOffset      = 0x18;
constexpr std::size_t  EventCustomParamOffset   = 0x1C;
constexpr std::size_t  EventUnitNextOffset      = 0x38;
constexpr std::size_t  EventFlagsOffset         = 0x02;
constexpr std::uint16_t EventDeleteDeferredFlag = 0x0008;
constexpr std::uint8_t ItemAuraEventType        = 9;
constexpr std::uint8_t MaskedEventType          = 0xFE;
constexpr std::size_t  MaximumUnitEvents        = 65536;

inline auto EventReadPointer(void* base, std::size_t offset) noexcept -> void* {
    void* value = nullptr;
    std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(value));
    return value;
}

inline auto EventReadU32(void* base, std::size_t offset) noexcept -> std::uint32_t {
    std::uint32_t value = 0;
    std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(value));
    return value;
}

inline auto EventReadU16(void* base, std::size_t offset) noexcept -> std::uint16_t {
    std::uint16_t value = 0;
    std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(value));
    return value;
}

// Runs the stock delete for (9, customId) with every type 9 event it would
// match, other than the (customId, skillId) event itself, hidden from it.
// Returns the stock result; *kept receives how many events were protected.
template <typename StockDelete>
inline auto DeleteItemAuraEventOnly(StockDelete&& stockDelete, void* game, void* unit,
                                    std::uint32_t customId, std::uint32_t skillId,
                                    std::uint32_t* kept) noexcept -> std::uint64_t {
    std::uint32_t masked = 0;
    if (unit != nullptr) {
        void* node = EventReadPointer(unit, UnitEventListOffset);
        for (std::size_t walked = 0; node != nullptr && walked < MaximumUnitEvents; ++walked) {
            auto* bytes = static_cast<std::uint8_t*>(node);
            if (bytes[EventTypeOffset] == ItemAuraEventType) {
                const auto id    = EventReadU32(node, EventCustomIdOffset);
                const auto param = EventReadU32(node, EventCustomParamOffset);
                const bool isTarget        = id == customId && param == skillId;
                const bool stockWouldMatch = customId == 0 || id == customId;
                if (!isTarget && stockWouldMatch) {
                    bytes[EventTypeOffset] = MaskedEventType;
                    ++masked;
                }
            }
            node = EventReadPointer(node, EventUnitNextOffset);
        }
    }

    const std::uint64_t result = stockDelete(game, unit, static_cast<std::int32_t>(ItemAuraEventType), customId);

    if (masked != 0) {
        void* node = EventReadPointer(unit, UnitEventListOffset);
        for (std::size_t walked = 0; node != nullptr && walked < MaximumUnitEvents; ++walked) {
            auto* bytes = static_cast<std::uint8_t*>(node);
            if (bytes[EventTypeOffset] == MaskedEventType) {
                bytes[EventTypeOffset] = ItemAuraEventType;
            }
            node = EventReadPointer(node, EventUnitNextOffset);
        }
    }

    if (kept != nullptr) {
        *kept = masked;
    }
    return result;
}

// True when the unit already has a live type 9 event for (customId, skillId).
inline auto HasItemAuraEvent(void* unit, std::uint32_t customId, std::uint32_t skillId) noexcept -> bool {
    if (unit == nullptr) {
        return false;
    }
    void* node = EventReadPointer(unit, UnitEventListOffset);
    for (std::size_t walked = 0; node != nullptr && walked < MaximumUnitEvents; ++walked) {
        const auto* bytes = static_cast<const std::uint8_t*>(node);
        if (bytes[EventTypeOffset] == ItemAuraEventType
            && EventReadU32(node, EventCustomIdOffset) == customId
            && EventReadU32(node, EventCustomParamOffset) == skillId
            && (EventReadU16(node, EventFlagsOffset) & EventDeleteDeferredFlag) == 0) {
            return true;
        }
        node = EventReadPointer(node, EventUnitNextOffset);
    }
    return false;
}
// EVENT-LOGIC-END

// STAT-SCAN-BEGIN
// D2RCore stat store entry: 16 bytes, uint64 key = statId << 32 | layer at +0,
// int32 value at +8. Calls visit(skillId, value) for every item_aura (151)
// layer with a positive value.
constexpr std::size_t WideStatEntrySize   = 16;
constexpr std::size_t WideStatValueOffset = 8;

template <typename Visit>
inline void ForEachItemAuraLayer(const std::uint8_t* entries, std::uint64_t count, Visit&& visit) noexcept {
    for (std::uint64_t index = 0; index < count; ++index) {
        const auto* entry = entries + index * WideStatEntrySize;
        std::uint64_t key   = 0;
        std::int32_t  value = 0;
        std::memcpy(&key, entry, sizeof(key));
        std::memcpy(&value, entry + WideStatValueOffset, sizeof(value));
        if ((key >> 32) != 151U || value <= 0) {
            continue;
        }
        visit(static_cast<std::uint32_t>(key), value);
    }
}
// STAT-SCAN-END

// ---------------------------------------------------------------------------
//  Native signatures
// ---------------------------------------------------------------------------
using DeleteEventsByTypeFn = std::uint64_t(__fastcall*)(void* game, void* unit, std::int32_t type, std::uint32_t customId) noexcept;
using DeactivateItemAuraFn = std::uint64_t(__fastcall*)(void* game, void* unit, std::uint32_t customId, std::uint32_t skillId) noexcept;
using ActivateItemAuraFn   = std::uint64_t(__fastcall*)(void* game, void* unit, std::uint32_t customId, std::uint32_t skillId, std::int32_t level, std::int32_t castId) noexcept;
using ResurrectHandlerFn   = std::uint64_t(__fastcall*)(void* game, void* player, void* packet, std::uint32_t packetSize) noexcept;
using GetSkillsTxtRecordFn = const std::uint8_t*(__fastcall*)(std::uint32_t dataContext, std::int32_t skillId) noexcept;
using GetStateCountFn      = std::int32_t(__fastcall*)(std::uint32_t dataContext) noexcept;
using ToggleStateFn        = void(__fastcall*)(void* unit, std::int32_t state, std::int32_t enable) noexcept;
using GetStateStatListFn   = void*(__fastcall*)(void* unit, std::int32_t state) noexcept;
using UnlinkStatListFn     = void(__fastcall*)(void* unit, void* statList) noexcept;
using FreeStatListFn       = void(__fastcall*)(std::uint32_t dataContext, void* statList) noexcept;
using GetStatListOwnerFn   = void*(__fastcall*)(void* item, std::int32_t* active) noexcept;
using GetUnitStatFn        = std::int32_t(__fastcall*)(void* unit, std::int32_t statId, std::uint32_t layer) noexcept;
using GetUnitTypeFn        = std::int32_t(__fastcall*)(void* unit) noexcept;
using IsUnitDeadFn         = bool(__fastcall*)(void* unit) noexcept;
using GetStatListExFn      = void*(__fastcall*)(void* unit) noexcept;
using GetInventoryFn       = void*(__fastcall*)(void* unit, const char* file, std::int32_t line) noexcept;
using GetFirstItemFn       = void*(__fastcall*)(void* inventory) noexcept;
using GetNextItemFn        = void*(__fastcall*)(void* item) noexcept;
using NextGameCounterFn    = std::int32_t(__fastcall*)(void* game) noexcept;

// ---------------------------------------------------------------------------
//  State
// ---------------------------------------------------------------------------
enum class DeleteSite : std::uint32_t {
    AuraStart   = 0,
    AuraTick    = 1,
    AuraRemoval = 2,
    AuraStop    = 3,
    GearRefresh = 4,
};
constexpr std::size_t DeleteSiteCount = 5;
constexpr const char* DeleteSiteNames[DeleteSiteCount]{
    "aura start", "aura tick", "aura removal", "aura stop", "gear refresh",
};

struct Settings {
    bool enabled{true};
    bool multiAuraPerItem{true};
    bool restoreAfterResurrect{true};
    bool diagnostics{false};
};

struct InstallState {
    bool deleteHook{false};
    bool callSites{false};
    bool stopHook{false};
    bool gearRefresh{false};
    bool resurrectHook{false};
};

const D2RL::PluginContext* Context{};
std::uintptr_t             Base{};
Settings                   Config{};
InstallState               Installed{};
char                       ConfigSource[48]{"built-in defaults"};

DeleteEventsByTypeFn OriginalDeleteEventsByType{};
DeactivateItemAuraFn OriginalDeactivateItemAura{};
ResurrectHandlerFn   OriginalResurrectHandler{};
ActivateItemAuraFn   ActivateItemAura{};
GetSkillsTxtRecordFn GetSkillsTxtRecord{};
GetStateCountFn      GetStateCount{};
ToggleStateFn        ToggleState{};
GetStateStatListFn   GetStateStatList{};
UnlinkStatListFn     UnlinkStatList{};
FreeStatListFn       FreeStatList{};
GetStatListOwnerFn   GetStatListOwner{};
GetUnitStatFn        GetUnitStat{};
GetUnitTypeFn        GetUnitType{};
IsUnitDeadFn         IsUnitDead{};
GetStatListExFn      GetStatListEx{};
GetInventoryFn       GetInventory{};
GetFirstItemFn       GetFirstItem{};
GetNextItemFn        GetNextItem{};
NextGameCounterFn    NextGameCounter{};

std::uintptr_t CoreFlushDeleteReturn{};
std::uint8_t*  RelayPage{};
bool           RelayUnwindRegistered{};

struct CallSitePatch {
    std::uint64_t                rva;
    const std::uint8_t*          stock;
    std::size_t                  stubOffset;
    std::array<std::uint8_t, 5>  written;
    bool                         applied;
};
std::array<CallSitePatch, 3> CallSites{{
    {ActivateDeleteCallRva, ActivateDeleteCall.data(), RelayStubOffsets[0], {}, false},
    {EventRearmDeleteCallRva, EventRearmDeleteCall.data(), RelayStubOffsets[1], {}, false},
    {EventRemoveDeleteCallRva, EventRemoveDeleteCall.data(), RelayStubOffsets[2], {}, false},
}};

std::array<std::atomic<std::uint64_t>, DeleteSiteCount> SiteDeletes{};
std::atomic<std::uint64_t> EventsKept{};
std::atomic<std::uint64_t> Resurrects{};
std::atomic<std::uint64_t> ItemsOnPlayer{};
std::atomic<std::uint64_t> AuraLayersFound{};
std::atomic<std::uint64_t> AurasRestored{};
std::atomic<std::uint64_t> AurasAlreadyActive{};
std::atomic<std::uint32_t> DiagnosticsBudget{};

template <typename T>
auto At(std::uint64_t rva) noexcept -> T {
    return reinterpret_cast<T>(Base + static_cast<std::uintptr_t>(rva));
}

auto ReadU8(const void* base, std::size_t offset) noexcept -> std::uint8_t {
    return static_cast<const std::uint8_t*>(base)[offset];
}

auto ReadI16(const void* base, std::size_t offset) noexcept -> std::int16_t {
    std::int16_t value = 0;
    std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(value));
    return value;
}

auto ReadI32(const void* base, std::size_t offset) noexcept -> std::int32_t {
    std::int32_t value = 0;
    std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(value));
    return value;
}

auto ReadU32(const void* base, std::size_t offset) noexcept -> std::uint32_t {
    std::uint32_t value = 0;
    std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(value));
    return value;
}

auto ReadU64(const void* base, std::size_t offset) noexcept -> std::uint64_t {
    std::uint64_t value = 0;
    std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(value));
    return value;
}

// ---------------------------------------------------------------------------
//  Logging
// ---------------------------------------------------------------------------
void LogInfo(const char* format, ...) noexcept {
    char message[512]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    Context->LogInfo(message);
}

void LogWarn(const char* format, ...) noexcept {
    char message[512]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    Context->LogWarn(message);
}

void LogError(const char* format, ...) noexcept {
    char message[512]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    Context->LogError(message);
}

auto TakeDiagnosticsLine() noexcept -> bool {
    if (!Config.diagnostics) {
        return false;
    }
    auto budget = DiagnosticsBudget.load(std::memory_order_relaxed);
    while (budget != 0) {
        if (DiagnosticsBudget.compare_exchange_weak(budget, budget - 1, std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
//  Configuration
// ---------------------------------------------------------------------------
auto Trim(std::string_view value) noexcept -> std::string_view {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
        value.remove_suffix(1);
    }
    return value;
}

void ParseConfig(std::string_view text) noexcept {
    std::size_t lineNumber = 0;
    for (std::size_t start = 0; start <= text.size();) {
        ++lineNumber;
        const auto end = text.find('\n', start);
        auto line = text.substr(start, end == std::string_view::npos ? text.size() - start : end - start);
        start = end == std::string_view::npos ? text.size() + 1 : end + 1;

        const auto comment = line.find('#');
        if (comment != std::string_view::npos) {
            line = line.substr(0, comment);
        }
        line = Trim(line);
        if (line.empty()) {
            continue;
        }

        const auto equals = line.find('=');
        if (equals == std::string_view::npos) {
            LogWarn("AuraReactivation: config line %zu ignored, expected key = value.", lineNumber);
            continue;
        }
        const auto key   = Trim(line.substr(0, equals));
        const auto value = Trim(line.substr(equals + 1));

        bool* target = nullptr;
        if (key == "enabled") {
            target = &Config.enabled;
        } else if (key == "multi_aura_per_item") {
            target = &Config.multiAuraPerItem;
        } else if (key == "restore_after_resurrect") {
            target = &Config.restoreAfterResurrect;
        } else if (key == "diagnostics") {
            target = &Config.diagnostics;
        } else {
            LogWarn("AuraReactivation: config line %zu has an unknown key.", lineNumber);
            continue;
        }

        if (value == "true") {
            *target = true;
        } else if (value == "false") {
            *target = false;
        } else {
            LogWarn("AuraReactivation: config line %zu ignored, the value must be true or false.", lineNumber);
        }
    }
}

void LoadConfig() noexcept {
    Config = Settings{};
    std::snprintf(ConfigSource, sizeof(ConfigSource), "%s", "built-in defaults");
    if (!Context->EnsureConfig(DefaultConfigToml)) {
        LogWarn("AuraReactivation: could not create the default config, using built-in defaults.");
    }

    std::string buffer(16384, '\0');
    std::uint32_t required = 0;
    if (!Context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size()), &required)) {
        if (required <= buffer.size()) {
            LogWarn("AuraReactivation: could not read the config, using built-in defaults.");
            return;
        }
        buffer.assign(required, '\0');
        if (!Context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size()), &required)) {
            LogWarn("AuraReactivation: could not read the config, using built-in defaults.");
            return;
        }
    }
    ParseConfig(std::string_view(buffer.c_str()));
    std::snprintf(ConfigSource, sizeof(ConfigSource), "%s", "config file");
}

// ---------------------------------------------------------------------------
//  Image checks
// ---------------------------------------------------------------------------
auto CheckExact(std::uint64_t rva, const std::uint8_t* bytes, std::size_t size, const char* name) noexcept -> bool {
    if (Context->CheckExpectedBytes(rva, bytes, static_cast<std::uint32_t>(size))) {
        return true;
    }
    LogError("AuraReactivation: %s at RVA 0x%llX does not match D2RLoader 1.3.1.", name,
             static_cast<unsigned long long>(rva));
    return false;
}

// A function the plugin only calls may already carry another plugin's inline
// hook at its entry. The loader tracks those, and calling through one is fine.
auto CheckCallable(std::uint64_t rva, const std::uint8_t* bytes, std::size_t size, const char* name) noexcept -> bool {
    if (Context->CheckExpectedBytes(rva, bytes, static_cast<std::uint32_t>(size))) {
        return true;
    }

    const D2RL::DiagnosticsService* diagnostics = nullptr;
    if (Context->QueryService(&diagnostics)
            == D2RL::ServiceQueryResult::Success
        && D2RL::HasDiagnosticsServiceField(diagnostics, D2RL::DiagnosticsServiceRequiredSize)
        && diagnostics->queryHookStatus != nullptr) {
        D2RL::Diagnostics::HookQuery query{
            .structSize   = D2RL::Diagnostics::HookQuerySize,
            .flags        = 0,
            .rva          = rva,
            .expected     = bytes,
            .expectedSize = static_cast<std::uint32_t>(size),
            .reserved     = 0,
        };
        D2RL::Diagnostics::HookStatus status{
            .structSize = D2RL::Diagnostics::HookStatusSize,
        };
        if (diagnostics->queryHookStatus(Context, &query, &status) == D2RL::Diagnostics::Result::Success
            && status.state == D2RL::Diagnostics::ModificationState::Tracked
            && status.kind == D2RL::Diagnostics::ModificationKind::InlineHook) {
            LogInfo("AuraReactivation: %s at RVA 0x%llX is hooked by another plugin, calling through it.", name,
                    static_cast<unsigned long long>(rva));
            return true;
        }
    }

    LogError("AuraReactivation: %s at RVA 0x%llX does not match D2RLoader 1.3.1.", name,
             static_cast<unsigned long long>(rva));
    return false;
}

auto ValidateCoreStatStore() noexcept -> bool;

auto ValidateEventLayout() noexcept -> bool {
    return CheckCallable(DeleteEventsByTypeRva, DeleteEventsByTypeEntry.data(), DeleteEventsByTypeEntry.size(),
                         "EVENT_DeleteByTypeId entry")
        && CheckExact(DeleteEventsWalkRva, DeleteEventsWalkWitness.data(), DeleteEventsWalkWitness.size(),
                      "EVENT_DeleteByTypeId node walk")
        && CheckCallable(GetUnitEventListRva, GetUnitEventListBody.data(), GetUnitEventListBody.size(),
                         "unit event list getter")
        && CheckExact(SetEventStoreRva, SetEventStoreWitness.data(), SetEventStoreWitness.size(),
                      "EVENT_SetEvent node stores");
}

auto ValidateMultiAura() noexcept -> bool {
    return CheckExact(ActivateItemAuraRva, ActivateItemAuraBody.data(), ActivateItemAuraBody.size(),
                      "item aura activation")
        && CheckExact(ItemAuraEventRva, ItemAuraEventBody.data(), ItemAuraEventBody.size(), "item aura event")
        && CheckExact(DeactivateItemAuraRva, DeactivateItemAuraBody.data(), DeactivateItemAuraBody.size(),
                      "item aura deactivation")
        && CheckCallable(GetSkillsTxtRecordRva, GetSkillsTxtRecordEntry.data(), GetSkillsTxtRecordEntry.size(),
                         "skills.txt record getter")
        && CheckCallable(GetStateCountRva, GetStateCountEntry.data(), GetStateCountEntry.size(), "state count")
        && CheckCallable(ToggleStateRva, ToggleStateEntry.data(), ToggleStateEntry.size(), "STATES_ToggleState")
        && CheckCallable(GetStateStatListRva, GetStateStatListEntry.data(), GetStateStatListEntry.size(),
                         "state stat list getter")
        && CheckCallable(UnlinkStatListRva, UnlinkStatListEntry.data(), UnlinkStatListEntry.size(),
                         "STATLIST_UnlinkStatList")
        && CheckCallable(FreeStatListRva, FreeStatListEntry.data(), FreeStatListEntry.size(),
                         "STATLIST_FreeStatList");
}

auto ValidateResurrect() noexcept -> bool {
    return CheckExact(ResurrectHandlerRva, ResurrectHandlerEntry.data(), ResurrectHandlerEntry.size(),
                      "resurrect handler")
        && CheckExact(ResurrectSuccessTailRva, ResurrectSuccessTailWitness.data(),
                      ResurrectSuccessTailWitness.size(), "resurrect handler success tail")
        && CheckCallable(ActivateItemAuraRva, ActivateItemAuraBody.data(), 26, "item aura activation")
        && CheckCallable(GetStatListOwnerRva, GetStatListOwnerEntry.data(), GetStatListOwnerEntry.size(),
                         "STATLIST_GetOwner")
        && CheckCallable(GetUnitStatRva, GetUnitStatThunk.data(), GetUnitStatThunk.size(), "STATLIST_GetUnitStat")
        && CheckCallable(GetUnitTypeRva, GetUnitTypeEntry.data(), GetUnitTypeEntry.size(), "UNITS_GetUnitType")
        && CheckCallable(IsUnitDeadRva, IsUnitDeadEntry.data(), IsUnitDeadEntry.size(), "UNITS_IsDead")
        && CheckCallable(GetStatListExRva, GetStatListExEntry.data(), GetStatListExEntry.size(),
                         "UNITS_GetStatListEx")
        && CheckCallable(GetInventoryRva, GetInventoryEntry.data(), GetInventoryEntry.size(), "UNITS_GetInventory")
        && CheckCallable(GetFirstItemRva, GetFirstItemEntry.data(), GetFirstItemEntry.size(),
                         "INVENTORY_GetFirstItem")
        && CheckCallable(GetNextItemRva, GetNextItemEntry.data(), GetNextItemEntry.size(), "INVENTORY_GetNextItem")
        && CheckCallable(NextGameCounterRva, NextGameCounterEntry.data(), NextGameCounterEntry.size(),
                         "GAME_NextGameCounter")
        && ValidateCoreStatStore();
}

// D2RCore.dll's image, or null when it is not loaded.
auto CoreImage(std::uint32_t* imageSize) noexcept -> const std::uint8_t* {
    const HMODULE module = GetModuleHandleW(CoreModuleName);
    if (module == nullptr) {
        return nullptr;
    }
    const auto* image    = reinterpret_cast<const std::uint8_t*>(module);
    const auto  ntOffset = static_cast<std::size_t>(ReadU32(image, 0x3C));
    *imageSize = ReadU32(image, ntOffset + 0x50);
    return image;
}

auto CoreWindowMatches(const std::uint8_t* image, std::uint32_t imageSize, std::uint64_t rva,
                       const std::uint8_t* bytes, std::size_t size) noexcept -> bool {
    return rva + size <= imageSize && std::memcmp(image + rva, bytes, size) == 0;
}

// The stat store layout the resurrect restore parses.
auto ValidateCoreStatStore() noexcept -> bool {
    std::uint32_t imageSize = 0;
    const auto* image = CoreImage(&imageSize);
    if (image == nullptr) {
        LogError("AuraReactivation: D2RCore.dll is not loaded; restore_after_resurrect is off.");
        return false;
    }
    if (!CoreWindowMatches(image, imageSize, CoreReadWideUnitStatRva, CoreReadWideUnitStatExport.data(),
                           CoreReadWideUnitStatExport.size())
        || !CoreWindowMatches(image, imageSize, CoreWideStatLayoutRva, CoreWideStatLayoutWindow.data(),
                              CoreWideStatLayoutWindow.size())) {
        LogError("AuraReactivation: D2RCore.dll's stat store is not the 1.3.1 layout this plugin reads; "
                 "restore_after_resurrect is off.");
        return false;
    }
    return true;
}

// D2RCore's refresh flush: the exact block, and the three function-pointer
// slots it calls through must name the activation, the delete and SetEvent.
auto ResolveCoreFlushReturn() noexcept -> std::uintptr_t {
    std::uint32_t imageSize = 0;
    const auto* image = CoreImage(&imageSize);
    if (image == nullptr) {
        LogWarn("AuraReactivation: D2RCore.dll is not loaded; the gear refresh fix is off.");
        return 0;
    }
    const auto coreBase = reinterpret_cast<std::uintptr_t>(image);
    if (!CoreWindowMatches(image, imageSize, CoreFlushWindowRva, CoreFlushWindow.data(), CoreFlushWindow.size())) {
        LogWarn("AuraReactivation: D2RCore.dll is not the 1.3.1 build this plugin was verified against; "
                "the gear refresh fix is off and items with several auras can lose all but one on a refresh.");
        return 0;
    }

    const auto slotNames = [&](std::uint64_t callRva, std::uint64_t expectedRva) noexcept -> bool {
        const auto disp = ReadI32(image, static_cast<std::size_t>(callRva + 2));
        const auto slot = static_cast<std::int64_t>(callRva + 6) + disp;
        if (slot <= 0 || static_cast<std::uint64_t>(slot) + 16 > imageSize) {
            return false;
        }
        const auto resolved = ReadU64(image, static_cast<std::size_t>(slot));
        const auto rva      = ReadU64(image, static_cast<std::size_t>(slot) + 8);
        return rva == expectedRva && (resolved == 0 || resolved == Base + expectedRva);
    };
    if (!slotNames(CoreFlushActivateCallRva, ActivateItemAuraRva)
        || !slotNames(CoreFlushDeleteCallRva, DeleteEventsByTypeRva)
        || !slotNames(CoreFlushSetEventCallRva, EventSetEventRva)) {
        LogWarn("AuraReactivation: D2RCore.dll's refresh flush does not call the expected game functions; "
                "the gear refresh fix is off.");
        return 0;
    }
    return coreBase + CoreFlushDeleteReturnRva;
}

// ---------------------------------------------------------------------------
//  The fixed delete
// ---------------------------------------------------------------------------
auto StockDeleteEventsByType(void* game, void* unit, std::int32_t type, std::uint32_t customId) noexcept -> std::uint64_t {
    if (OriginalDeleteEventsByType != nullptr) {
        return OriginalDeleteEventsByType(game, unit, type, customId);
    }
    return At<DeleteEventsByTypeFn>(DeleteEventsByTypeRva)(game, unit, type, customId);
}

auto DeleteItemAuraEvent(void* game, void* unit, std::uint32_t customId, std::uint32_t skillId, DeleteSite site) noexcept -> std::uint64_t {
    std::uint32_t kept = 0;
    const auto result = DeleteItemAuraEventOnly(&StockDeleteEventsByType, game, unit, customId, skillId, &kept);

    SiteDeletes[static_cast<std::size_t>(site)].fetch_add(1, std::memory_order_relaxed);
    if (kept != 0) {
        EventsKept.fetch_add(kept, std::memory_order_relaxed);
        if (TakeDiagnosticsLine()) {
            LogInfo("AuraReactivation: %s of skill %u from item %u kept %u other aura event(s) on unit type %d.",
                    DeleteSiteNames[static_cast<std::size_t>(site)], skillId, customId, kept,
                    unit != nullptr ? ReadI32(unit, UnitTypeOffset) : -1);
        }
    }
    return result;
}

// Reached from the relay stubs at the three stock call sites.
std::uint64_t __fastcall RelayDeleteBridge(void* game, void* unit, std::int32_t type, std::uint32_t customId,
                                           std::uint32_t skillId, std::uint32_t site) noexcept {
    if (type != static_cast<std::int32_t>(ItemAuraEventType) || site > static_cast<std::uint32_t>(DeleteSite::AuraRemoval)) {
        return StockDeleteEventsByType(game, unit, type, customId);
    }
    return DeleteItemAuraEvent(game, unit, customId, skillId, static_cast<DeleteSite>(site));
}

// Every EVENT_DeleteByTypeId call. Only D2RCore's refresh flush is converted.
std::uint64_t __fastcall HookDeleteEventsByType(void* game, void* unit, std::int32_t type, std::uint32_t customId) noexcept {
    if (type == static_cast<std::int32_t>(ItemAuraEventType) && CoreFlushDeleteReturn != 0
        && reinterpret_cast<std::uintptr_t>(_ReturnAddress()) == CoreFlushDeleteReturn) {
        // The caller's rsp at its call is one slot above our return address.
        const auto* callerFrame = static_cast<const std::uint8_t*>(_AddressOfReturnAddress()) + sizeof(std::uintptr_t);
        std::uint32_t skillId = 0;
        std::memcpy(&skillId, callerFrame + CoreFlushSkillSlot, sizeof(skillId));
        return DeleteItemAuraEvent(game, unit, customId, skillId, DeleteSite::GearRefresh);
    }
    return OriginalDeleteEventsByType(game, unit, type, customId);
}

// DEACTIVATE-BEGIN
// 0x581AE0, instruction for instruction:
//   581B17  record = GetSkillsTxtRecord(game[106h], skillId); null -> return
//   581B37  record[24h] & aura mask == 0 -> return
//   581B43  record[A0h] < 0 -> return
//   581B60  state >= GetStateCount(game[106h]) -> return (no delete either)
//   581B71  ToggleState(unit, state, 0)
//   581B80  statList = GetStateStatList(unit, record[A0h])
//   581B93  statList: UnlinkStatList(unit, statList), FreeStatList(game[106h], statList)
//   581BB6  delete (9, itemId)                  <- item and skill here
std::uint64_t __fastcall HookDeactivateItemAura(void* game, void* unit, std::uint32_t customId, std::uint32_t skillId) noexcept {
    if (game == nullptr || unit == nullptr) {
        return OriginalDeactivateItemAura(game, unit, customId, skillId);
    }

    const auto* record = GetSkillsTxtRecord(ReadU8(game, GameDataContextOffset), static_cast<std::int32_t>(skillId));
    if (record == nullptr) {
        return 0;
    }
    const auto auraMask = ReadU8(reinterpret_cast<const void*>(Base), static_cast<std::size_t>(AuraFlagMaskRva));
    if ((record[SkillRecordFlagsByteOffset] & auraMask) == 0) {
        return 0;
    }
    const auto auraState = ReadI16(record, SkillRecordAuraStateOffset);
    if (auraState < 0) {
        return 0;
    }
    if (static_cast<std::int32_t>(auraState) >= GetStateCount(ReadU8(game, GameDataContextOffset))) {
        return 0;
    }

    ToggleState(unit, auraState, 0);
    if (auto* statList = GetStateStatList(unit, ReadI16(record, SkillRecordAuraStateOffset)); statList != nullptr) {
        UnlinkStatList(unit, statList);
        FreeStatList(ReadU8(game, GameDataContextOffset), statList);
    }
    return DeleteItemAuraEvent(game, unit, customId, skillId, DeleteSite::AuraStop);
}

// DEACTIVATE-END

// ---------------------------------------------------------------------------
//  Resurrect
// ---------------------------------------------------------------------------
// RESTORE-BEGIN
void RestoreItemAuraEvents(void* game, void* player) noexcept {
    if (GetUnitType(player) != UnitTypePlayer || IsUnitDead(player)) {
        return;
    }
    auto* inventory = GetInventory(player, "aura-reactivation", 0);
    if (inventory == nullptr) {
        return;
    }

    std::uint32_t itemsOnPlayer = 0;
    std::uint32_t layersFound = 0;
    std::uint32_t noLevel = 0;
    std::uint32_t restored = 0;
    std::uint32_t alreadyActive = 0;
    std::size_t   visited = 0;
    for (auto* item = GetFirstItem(inventory); item != nullptr && visited < MaximumInventoryItems;
         item = GetNextItem(item), ++visited) {
        if (GetUnitType(item) != UnitTypeItem) {
            continue;
        }
        std::int32_t active = 0;
        if (GetStatListOwner(item, &active) != player || active == 0) {
            continue;
        }
        auto* statList = GetStatListEx(item);
        if (statList == nullptr) {
            continue;
        }
        ++itemsOnPlayer;

        // Same selection as D2RCore!ReadWideUnitStat (+0x3E2EA2).
        const auto arrayOffset = StatListBaseStatsOffset
                               + (ReadI32(statList, StatListFlagsOffset) < 0 ? StatListExtendedDelta : 0);
        void* records = nullptr;
        std::memcpy(&records, static_cast<const std::uint8_t*>(statList) + arrayOffset, sizeof(records));
        const auto count = ReadU64(statList, arrayOffset + sizeof(void*));
        if (records == nullptr || count == 0 || count > MaximumStatsPerList) {
            continue;
        }

        const auto itemId = ReadU32(item, UnitIdOffset);
        ForEachItemAuraLayer(static_cast<const std::uint8_t*>(records), count,
                             [&](std::uint32_t skillId, std::int32_t) noexcept {
            ++layersFound;
            const auto level = GetUnitStat(player, static_cast<std::int32_t>(ItemAuraStatId), skillId);
            if (level <= 0) {
                ++noLevel;
                return;
            }
            if (HasItemAuraEvent(player, itemId, skillId)) {
                ++alreadyActive;
                return;
            }
            ActivateItemAura(game, player, itemId, skillId, level, NextGameCounter(game));
            ++restored;
            if (TakeDiagnosticsLine()) {
                LogInfo("AuraReactivation: resurrect restored skill %u (level %d) from item %u.", skillId, level, itemId);
            }
        });
    }

    Resurrects.fetch_add(1, std::memory_order_relaxed);
    ItemsOnPlayer.fetch_add(itemsOnPlayer, std::memory_order_relaxed);
    AuraLayersFound.fetch_add(layersFound, std::memory_order_relaxed);
    AurasRestored.fetch_add(restored, std::memory_order_relaxed);
    AurasAlreadyActive.fetch_add(alreadyActive, std::memory_order_relaxed);
    if (TakeDiagnosticsLine()) {
        LogInfo("AuraReactivation: resurrect checked %u item(s) on the player: %u aura layer(s), %u restored, "
                "%u already active, %u with no item_aura level on the player.",
                itemsOnPlayer, layersFound, restored, alreadyActive, noLevel);
    }
}

// RESTORE-END

std::uint64_t __fastcall HookResurrectHandler(void* game, void* player, void* packet, std::uint32_t packetSize) noexcept {
    const auto result = OriginalResurrectHandler(game, player, packet, packetSize);
    // 4B66D1 xor eax,eax is the only way out after the player is revived.
    if (static_cast<std::uint32_t>(result) == 0 && game != nullptr && player != nullptr) {
        RestoreItemAuraEvents(game, player);
    }
    return result;
}

// ---------------------------------------------------------------------------
//  Installation
// ---------------------------------------------------------------------------
void ResolveNativeFunctions() noexcept {
    ActivateItemAura   = At<ActivateItemAuraFn>(ActivateItemAuraRva);
    GetSkillsTxtRecord = At<GetSkillsTxtRecordFn>(GetSkillsTxtRecordRva);
    GetStateCount      = At<GetStateCountFn>(GetStateCountRva);
    ToggleState        = At<ToggleStateFn>(ToggleStateRva);
    GetStateStatList   = At<GetStateStatListFn>(GetStateStatListRva);
    UnlinkStatList     = At<UnlinkStatListFn>(UnlinkStatListRva);
    FreeStatList       = At<FreeStatListFn>(FreeStatListRva);
    GetStatListOwner   = At<GetStatListOwnerFn>(GetStatListOwnerRva);
    GetUnitStat        = At<GetUnitStatFn>(GetUnitStatRva);
    GetUnitType        = At<GetUnitTypeFn>(GetUnitTypeRva);
    IsUnitDead         = At<IsUnitDeadFn>(IsUnitDeadRva);
    GetStatListEx      = At<GetStatListExFn>(GetStatListExRva);
    GetInventory       = At<GetInventoryFn>(GetInventoryRva);
    GetFirstItem       = At<GetFirstItemFn>(GetFirstItemRva);
    GetNextItem        = At<GetNextItemFn>(GetNextItemRva);
    NextGameCounter    = At<NextGameCounterFn>(NextGameCounterRva);
}

auto AllocateRelayPage() noexcept -> std::uint8_t* {
    const auto* image = reinterpret_cast<const std::uint8_t*>(Base);
    const auto  ntOffset = static_cast<std::size_t>(ReadU32(image, 0x3C));
    const auto  imageSize = ReadU32(image, ntOffset + 0x50);

    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    const std::uintptr_t granularity = systemInfo.dwAllocationGranularity != 0 ? systemInfo.dwAllocationGranularity : 0x10000;
    const auto alignUp = [granularity](std::uintptr_t value) noexcept {
        return (value + granularity - 1) & ~(granularity - 1);
    };

    // The page sits above the image, so the lowest call site is the farthest.
    const std::uintptr_t limit = Base + EventRearmDeleteCallRva + 0x7FF00000ULL;
    std::uintptr_t address = alignUp(Base + imageSize);
    while (address + RelayPageBytes < limit) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) == 0) {
            break;
        }
        const auto regionEnd = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (info.State == MEM_FREE && address + RelayPageBytes <= regionEnd) {
            if (auto* page = VirtualAlloc(reinterpret_cast<LPVOID>(address), RelayPageBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
                return static_cast<std::uint8_t*>(page);
            }
        }
        const auto next = alignUp(regionEnd);
        if (next <= address) {
            break;
        }
        address = next;
    }
    return nullptr;
}

auto BuildRelay() noexcept -> bool {
    RelayPage = AllocateRelayPage();
    if (RelayPage == nullptr) {
        LogError("AuraReactivation: no free page within jump range of the game image.");
        return false;
    }
    std::memset(RelayPage, 0xCC, RelayPageBytes);
    std::memset(RelayPage, 0, RelayCommonOffset);
    EmitRelay(RelayPage, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&RelayDeleteBridge)));

    DWORD previous = 0;
    if (!VirtualProtect(RelayPage, RelayPageBytes, PAGE_EXECUTE_READ, &previous)) {
        LogError("AuraReactivation: could not make the relay page executable.");
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), RelayPage, RelayPageBytes);

    RelayUnwindRegistered = RtlAddFunctionTable(
        reinterpret_cast<PRUNTIME_FUNCTION>(RelayPage + RelayFunctionTableOffset),
        static_cast<DWORD>(RelayFunctionCount),
        static_cast<DWORD64>(reinterpret_cast<std::uintptr_t>(RelayPage))) != FALSE;
    if (!RelayUnwindRegistered) {
        LogWarn("AuraReactivation: unwind data for the relay page was not registered; stack walks stop there.");
    }
    return true;
}

auto RestoreCallSites() noexcept -> bool {
    bool allRestored = true;
    for (auto& site : CallSites) {
        if (!site.applied) {
            continue;
        }
        if (Context->PatchBytes(site.rva, site.written.data(), 5, site.stock, 5)) {
            site.applied = false;
        } else {
            allRestored = false;
        }
    }
    return allRestored;
}

auto PatchCallSites() noexcept -> bool {
    for (auto& site : CallSites) {
        const auto from = Base + static_cast<std::uintptr_t>(site.rva) + 5;
        const auto to   = reinterpret_cast<std::uintptr_t>(RelayPage) + site.stubOffset;
        const auto rel  = static_cast<std::int64_t>(to) - static_cast<std::int64_t>(from);
        if (rel < INT32_MIN || rel > INT32_MAX) {
            LogError("AuraReactivation: the relay page is out of reach of RVA 0x%llX.", static_cast<unsigned long long>(site.rva));
            RestoreCallSites();
            return false;
        }
        site.written[0] = 0xE8;
        const auto rel32 = static_cast<std::int32_t>(rel);
        std::memcpy(site.written.data() + 1, &rel32, sizeof(rel32));

        if (!Context->PatchBytes(site.rva, site.stock, 5, site.written.data(), 5)) {
            LogError("AuraReactivation: the loader refused the call at RVA 0x%llX.", static_cast<unsigned long long>(site.rva));
            RestoreCallSites();
            return false;
        }
        site.applied = true;
        if (std::memcmp(reinterpret_cast<const std::uint8_t*>(Base + static_cast<std::uintptr_t>(site.rva)), site.written.data(), 5) != 0) {
            LogError("AuraReactivation: RVA 0x%llX does not hold the bytes that were written.", static_cast<unsigned long long>(site.rva));
            RestoreCallSites();
            return false;
        }
    }
    return true;
}

void InstallMultiAura() noexcept {
    if (!ValidateMultiAura()) {
        LogError("AuraReactivation: multi_aura_per_item is off, the game image does not match.");
        return;
    }

    CoreFlushDeleteReturn = 0;
    if (Context->InstallInlineHook(DeleteEventsByTypeRva, DeleteEventsByTypeEntry.data(),
                                   static_cast<std::uint32_t>(DeleteEventsByTypeEntry.size()),
                                   &HookDeleteEventsByType, &OriginalDeleteEventsByType)
        && OriginalDeleteEventsByType != nullptr) {
        Installed.deleteHook = true;
        CoreFlushDeleteReturn = ResolveCoreFlushReturn();
        Installed.gearRefresh = CoreFlushDeleteReturn != 0;
    } else {
        OriginalDeleteEventsByType = nullptr;
        LogWarn("AuraReactivation: EVENT_DeleteByTypeId is already hooked by another plugin; "
                "the gear refresh fix is off.");
    }

    if (Context->InstallInlineHook(DeactivateItemAuraRva, DeactivateItemAuraBody.data(), 18,
                                   &HookDeactivateItemAura, &OriginalDeactivateItemAura)
        && OriginalDeactivateItemAura != nullptr) {
        Installed.stopHook = true;
    } else {
        OriginalDeactivateItemAura = nullptr;
        LogError("AuraReactivation: the item aura deactivation could not be hooked; stopping one aura still "
                 "removes the other auras of its item.");
    }

    if (BuildRelay() && PatchCallSites()) {
        Installed.callSites = true;
    } else {
        LogError("AuraReactivation: the aura start and aura tick deletes were not changed.");
    }
}

void InstallResurrect() noexcept {
    if (!ValidateResurrect()) {
        LogError("AuraReactivation: restore_after_resurrect is off, the game image does not match.");
        return;
    }
    if (Context->InstallInlineHook(ResurrectHandlerRva, ResurrectHandlerEntry.data(), 18, &HookResurrectHandler,
                                   &OriginalResurrectHandler)
        && OriginalResurrectHandler != nullptr) {
        Installed.resurrectHook = true;
    } else {
        OriginalResurrectHandler = nullptr;
        LogError("AuraReactivation: the resurrect handler is already hooked by another plugin; "
                 "restore_after_resurrect is off.");
    }
}

// ---------------------------------------------------------------------------
//  Console
// ---------------------------------------------------------------------------
auto OnOff(bool value) noexcept -> const char* {
    return value ? "on" : "off";
}

auto StatusCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
    if (command == nullptr || command->plugin == nullptr) {
        return D2RL::ConsoleCommandResult::Failed;
    }

    char line[512]{};
    std::snprintf(line, sizeof(line),
                  "Aura Reactivation %s (%s): multi aura %s [start and tick %s, stop %s, gear refresh %s]; "
                  "restore after resurrect %s; diagnostics %s.",
                  PluginVersion, ConfigSource, OnOff(Config.enabled && Config.multiAuraPerItem),
                  OnOff(Installed.callSites), OnOff(Installed.stopHook), OnOff(Installed.gearRefresh),
                  OnOff(Installed.resurrectHook), OnOff(Config.diagnostics));
    command->plugin->WriteConsoleMessage(line);

    std::snprintf(line, sizeof(line),
                  "Item aura deletes: start %llu, tick %llu, removal %llu, stop %llu, gear refresh %llu. "
                  "Other auras kept alive: %llu.",
                  static_cast<unsigned long long>(SiteDeletes[0].load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(SiteDeletes[1].load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(SiteDeletes[2].load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(SiteDeletes[3].load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(SiteDeletes[4].load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(EventsKept.load(std::memory_order_relaxed)));
    command->plugin->WriteConsoleMessage(line);

    std::snprintf(line, sizeof(line),
                  "Resurrects: %llu, items on the player %llu, aura layers found %llu, restored %llu, already active %llu.",
                  static_cast<unsigned long long>(Resurrects.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(ItemsOnPlayer.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(AuraLayersFound.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(AurasRestored.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(AurasAlreadyActive.load(std::memory_order_relaxed)));
    command->plugin->WriteConsoleMessage(line);
    return D2RL::ConsoleCommandResult::Handled;
}

void ResetState() noexcept {
    Config    = Settings{};
    Installed = InstallState{};
    OriginalDeleteEventsByType = nullptr;
    OriginalDeactivateItemAura = nullptr;
    OriginalResurrectHandler   = nullptr;
    CoreFlushDeleteReturn      = 0;
    for (auto& counter : SiteDeletes) {
        counter.store(0, std::memory_order_relaxed);
    }
    EventsKept.store(0, std::memory_order_relaxed);
    Resurrects.store(0, std::memory_order_relaxed);
    ItemsOnPlayer.store(0, std::memory_order_relaxed);
    AuraLayersFound.store(0, std::memory_order_relaxed);
    AurasRestored.store(0, std::memory_order_relaxed);
    AurasAlreadyActive.store(0, std::memory_order_relaxed);
    DiagnosticsBudget.store(64, std::memory_order_relaxed);
}

}  // namespace
}  // namespace CelestialRayOne::AuraReactivation

namespace {

constexpr D2RL::PluginInfo Info{
    .infoSize    = D2RL::PluginInfoSize,
    .abiVersion  = D2RL_PLUGIN_ABI_VERSION,
    .id          = "celestialrayone.aura-reactivation",
    .name        = "Aura Reactivation",
    .version     = "1.0.4",
    .author      = "CelestialRayOne",
    .description = "Every item_aura on an item stays active, and carried item auras return after a resurrect.",
    .flags       = D2RL::PluginFlags::Server | D2RL::PluginFlags::NativeHooks,
};

}  // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
    return &Info;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
    using namespace CelestialRayOne::AuraReactivation;

    if (!D2RL::HasContext(context) || context->exeBase == 0) {
        return false;
    }
    Context = context;
    Base    = context->exeBase;
    ResetState();
    LoadConfig();

    if (!Config.enabled) {
        LogInfo("Aura Reactivation %s loaded disabled by config; the game runs stock.", PluginVersion);
        return true;
    }

    if (!ValidateEventLayout()) {
        LogError("AuraReactivation: the event layout does not match D2RLoader 1.3.1; nothing was changed.");
        return true;
    }
    ResolveNativeFunctions();

    if (Config.multiAuraPerItem) {
        InstallMultiAura();
    }
    if (Config.restoreAfterResurrect) {
        InstallResurrect();
    }

    if (!context->RegisterConsoleCommand("aurareactivation", StatusCommand, "Show Aura Reactivation status and counters.")) {
        LogWarn("AuraReactivation: the aurareactivation console command could not be registered.");
    }

    LogInfo("Aura Reactivation %s by CelestialRayOne: multi aura [start and tick %s, stop %s, gear refresh %s], "
            "restore after resurrect %s, build %s.",
            PluginVersion, OnOff(Installed.callSites), OnOff(Installed.stopHook), OnOff(Installed.gearRefresh),
            OnOff(Installed.resurrectHook), context->buildName != nullptr ? context->buildName : "unknown");
    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    using namespace CelestialRayOne::AuraReactivation;
    if (Context == nullptr) {
        return;
    }
    const bool callSitesRestored = RestoreCallSites();
    if (callSitesRestored && RelayUnwindRegistered && RelayPage != nullptr) {
        RtlDeleteFunctionTable(reinterpret_cast<PRUNTIME_FUNCTION>(RelayPage + RelayFunctionTableOffset));
        RelayUnwindRegistered = false;
    }
    CoreFlushDeleteReturn = 0;
}
