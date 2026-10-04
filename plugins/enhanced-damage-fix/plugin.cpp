// Enhanced Damage Fix (celestialrayone.enhanced-damage-fix)
//
// Fixes the classic "ED/dmg bug": Enhanced Damage on a helm, armor, shield,
// belt, boots, gloves, jewel, charm, ring or amulet is lost, fully or on one
// side, whenever that item also carries flat minimum or maximum damage at the
// moment the Enhanced Damage is applied. Weapons were never affected.
//
// Every address and byte below was read out of the dumped D2RLoader 1.3.1 image
// (md5 8af7a47c...) and D2RCore.dll 1.3.1 (sha256 2a868d01...) and disassembled
// before being relied on.
//
// ---------------------------------------------------------------------------
// 1. The bug as players see it
// ---------------------------------------------------------------------------
//   - The item already has +max damage, an ED jewel goes in: the ED only raises
//     the character's minimum damage.
//   - The item already has +min damage: the ED only raises the maximum damage.
//   - A jewel with ED and +max (or +min) damage: same, the matching side is lost.
//   - A jewel with ED and both +min and +max (Protector's Stone, ED plus adds
//     10-30): the ED does nothing at all.
//   - ED socketed first, flat damage socketed later: works. Hence the famous
//     dependence on socketing order.
//   - Inside a weapon everything works.
//
// ---------------------------------------------------------------------------
// 2. The engine rule that loses it
// ---------------------------------------------------------------------------
//   Enhanced Damage is two stats, item_maxdamage_percent (17) and
//   item_mindamage_percent (18). In itemstatcost.txt both use op 13 with op
//   stat1..3 = maxdamage, secondary_maxdamage, item_throw_maxdamage (and the min
//   trio). op 13 adds the percent of the ITEM'S OWN BASE value of the target
//   stat: on a weapon that is the weapon damage set from weapons.txt. Base damage
//   is set for weapons only (D2MOO Items.cpp, the ITEMTYPE_WEAPON branch of the
//   item stat initializer); flat +min/+max from affixes and socketed jewels live
//   in child statlists, never in the item's base Stats.
//
//   Once op 13 has applied the percent on the item, the engine must keep the
//   percent stat from also reaching the character, or weapon ED would count
//   twice. That is decided in the stat evaluate-and-update routine (D2MOO
//   sub_6FDB64A0): for each op 13 target it switches on the percent stat's `op`
//   and, for op 13, withholds the commit on every statlist owned by an item.
//   The case is only reached when the target stat's evaluated TOTAL on that list
//   is not 0, and that total includes the flat bonuses. So the test the engine
//   really makes is "does this item have any min/max damage at all", while op 13
//   only multiplied the item's BASE damage. A helm or jewel with flat damage has
//   a non-zero total and a zero base: the percent is withheld from the character
//   and op 13 multiplied nothing. The Enhanced Damage is simply gone.
//
//   max ED (17) is decided against the max targets and min ED (18) against the
//   min targets, which is why +max kills only the max side and so on. The
//   decision runs when the percent stat is committed (added to an item, or a
//   statlist carrying it is attached to its host); flat damage that arrives
//   later never re-runs it for the percent, which is the order dependence.
//
// ---------------------------------------------------------------------------
// 3. Where the rule runs under D2RLoader 1.3.1: D2RCore.dll, not D2R.exe
// ---------------------------------------------------------------------------
//   The game image still contains its own copy, STATLIST_EvaluateAndUpdateStat
//   0x2FA430 with the op 13 case at 0x2FA8C7, but it is dead code. D2RLoader
//   1.3.x moved the whole statlist engine into D2RCore.dll for its wide stats:
//
//     - the statlist API entries are jumps into D2RCore: FF 25 at 0x2F5C60
//       (UnitGetStatValue), 0x2F7C00 (SetStatListStat), 0x2F81A0
//       (MergeStatLists), 0x2F2CB0 (PostStatList), 0x2F6E50 (detach); IDA
//       finds the calls to the game's delta propagator 0x2FA200 inside the
//       bodies behind the AddWideListStat, SetWideListStat,
//       ClearWideListStats, SetWideListStatFromSource and DetachWideStatList
//       jumps
//     - D2RCore stores statlist entries as 16 bytes (u64 key stat<<32|layer,
//       i32 value) in the same Stats (+30h) and FullStats (+A8h) arrays where
//       the game code walks 8-byte entries, so the game copy could not even read
//       a live statlist
//
//   This is also why a hook on the game's 0x2FA430 never fires on 1.3.1.
//
//   The live copy:
//
//     D2RCore 3DBEB0  evaluate and update (ctx, statList, key, record, unit)
//       3DBF0E  4C 89 7C 24 40     mov  [rsp+40h], r15           ; percent stat key
//       3DBF13  B0 01 / 89 44 24 34                               ; commit = 1
//       3DBF25  22 44 24 34        and  al, [rsp+34h]             ; <- resume
//       3DBF29  89 44 24 34        mov  [rsp+34h], eax            ; commit &= al
//       3DBF30  49 FF C7 ...       next target, at most 3
//       3DBF3D  46 0F B7 64 7E 54  movzx r12d, word [rsi+r15*2+54h] ; op stat N
//       3DBF6B  call [GAME 2D9730] ; target record -> r13
//       3DBF8E  call 3DBEB0        ; re-evaluate the target -> ebp
//       3DBFAF  call 3DCE90        ; commit the target
//       3DBFB4  test ebp,ebp / jz next                         ; total 0: no switch
//       3DBFBC  0F B6 46 50        movzx eax, byte [rsi+50h]   ; the stat's `op`
//       3DBFE7  jump table 63042C, entry 13 -> 3DC117
//       3DC117  83 7F 08 04        cmp  dword [rdi+8], 4       ; list owned by an item?
//       3DC11B  0F 95 C0           setne al                    ; item -> withhold
//       3DC11E  E9 02 FE FF FF     jmp  3DBF25
//
//   op 13 itself is applied by D2RCore's evaluator 3DC2D0: the list's owner
//   unit (statList+A0h) must exist and be an item (unit type at +0, 3DC710 and
//   3DC84A), and the percent multiplies that item's own base value of the target
//   at layer 0 (unit+88h -> base Stats +30h/+38h, 3DC90F). D2RCore exports the
//   same read as ReadWideUnitBaseStat (ordinal 1242, 0x831DD0 -> 0x3D8AD0).
//
// ---------------------------------------------------------------------------
// 4. The fix
// ---------------------------------------------------------------------------
//   The 12 bytes at D2RCore 3DC117 become a jump to a 67-byte stub in a page
//   this plugin allocates next to D2RCore. The stub asks AllowCommit for the
//   value of al and resumes at 3DBF25, so the engine's own and/mov records the
//   answer exactly where the native setne result went:
//
//     native  al = (list owner type != item)
//     fixed   for the six physical damage targets (21, 22, 23, 24, 159, 160):
//               withhold only when the owning item has a base value of that
//               target, the exact condition under which op 13 applied the
//               percent on the item
//             every other target (Enhanced Defense's armorclass, max
//               durability's maxdurability): the native answer, untouched
//
//   So a weapon behaves exactly as before, bit for bit: it has base damage, op
//   13 multiplies it, the percent stays on the weapon. Every other item commits
//   its Enhanced Damage, which then reaches the character like any off-weapon
//   ED, whatever else is on the item and in whatever order it was socketed.
//   Nothing is counted twice: on a non-weapon op 13 multiplies nothing.
//
//   Registers at 3DC117: rdi = statList, rsi = the percent stat's record,
//   r15 = target index 0..2, ebp = the target's non-zero total, and the commit
//   flag lives at [rsp+34h]. rsp is 16-aligned there (8 pushes + sub rsp,48h at
//   3DBEB0), so the stub takes 20h of shadow space, calls, and gives it back.
//   Nothing volatile is live at 3DBF25: the next iteration and the exit path
//   reload rcx, rdx and r8..r11 before use.
//
//   Existing characters are fixed when they load: every item's statlists are
//   rebuilt from the save and go through this decision again.
//
// ---------------------------------------------------------------------------
// 5. Verified against
// ---------------------------------------------------------------------------
//   D2MOO D2StatList.cpp (sub_6FDB5830 op 13, sub_6FDB64A0 commit switch,
//   sub_6FDB6C10 propagation, PostStatToStatList), Items.cpp base damage for
//   ITEMTYPE_WEAPON only; Ruff's RVA corpus for the game copy 0x2FA430 /
//   0x2F8AD0; the Blizzard forum case list for the "ed/dmg bug". The stub was
//   assembled with keystone, decoded with capstone and run under unicorn:
//   argument passing, stack alignment, preserved registers and the native tail.
//
//   No other plugin in this repo touches D2RCore 3DBEB0..3DC2CC.

#include <D2RLPlugin/api.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "decision.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace CelestialRayOne::EnhancedDamageFix {
namespace {

// ---------------------------------------------------------------------------
// D2RCore.dll 1.3.1 anchors (RVAs inside D2RCore, fixed image base C0DE5000000)
// ---------------------------------------------------------------------------

constexpr wchar_t CoreModuleName[]     = L"D2RCore.dll";
constexpr char    ReadBaseStatExport[] = "ReadWideUnitBaseStat";

constexpr std::uintptr_t EvaluateUpdateRva = 0x3DBEB0;  // evaluate and update
constexpr std::uintptr_t CommitLoopRva     = 0x3DBF0E;  // per-target commit loop
constexpr std::uintptr_t CommitResumeRva   = 0x3DBF25;  // and al,[rsp+34h]
constexpr std::uintptr_t ItemPercentArmRva = 0x3DC117;  // op 13 arm, the patch site
constexpr std::uintptr_t OpJumpEntry13Rva  = 0x630460;  // table 63042C + 13 * 4
constexpr std::uintptr_t OwnerLoadRva      = 0x3DC710;  // evaluator: owner unit, its type
constexpr std::uintptr_t OwnerGatesRva     = 0x3DC834;  // evaluator: op 11 / op 13 owner gates
constexpr std::uintptr_t OwnerBaseRva      = 0x3DC90F;  // evaluator: owner base Stats lookup
constexpr std::uintptr_t ReadBaseExportRva = 0x831DD0;  // ReadWideUnitBaseStat
constexpr std::uintptr_t ReadBaseImplRva   = 0x3D8AD0;  // its body

// Statlist and unit layout, as both D2RCore functions above read it.
constexpr std::size_t StatListOwnerTypeOffset = 0x08;
constexpr std::size_t StatListOwnerOffset     = 0xA0;
constexpr std::size_t UnitTypeOffset          = 0x00;
constexpr std::size_t UnitClassIdOffset       = 0x04;

constexpr std::uint32_t SiteSize = 12;
constexpr std::uint32_t JumpSize = 5;

// ---------------------------------------------------------------------------
// D2RCore byte witnesses
// ---------------------------------------------------------------------------

// 3DBEB0: push r15..rbx (8) / sub rsp,48h. The frame the stub runs in.
constexpr std::uint8_t EvaluatePrologueWitness[]{
    0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56, 0x57, 0x55, 0x53, 0x48, 0x83, 0xEC, 0x48,
};

// 3DBF0E..3DBFF0: commit flag at [rsp+34h], the resume block at +17h, the loop
// index in r15, the target id read at +2Fh, the recursive re-evaluation, the
// target commit, the zero-total skip, the switch on byte [rsi+50h] and its
// jump through the table at 63042C.
constexpr std::uint8_t CommitLoopWitness[]{
    0x4C, 0x89, 0x7C, 0x24, 0x40, 0xB0, 0x01, 0x89, 0x44, 0x24, 0x34, 0x45, 0x31, 0xFF, 0xEB, 0x1F,
    0x83, 0x7F, 0x08, 0x02, 0x0F, 0x93, 0xC0, 0x22, 0x44, 0x24, 0x34, 0x89, 0x44, 0x24, 0x34, 0x0F,
    0x1F, 0x00, 0x49, 0xFF, 0xC7, 0x49, 0x83, 0xFF, 0x03, 0x0F, 0x84, 0x91, 0x02, 0x00, 0x00, 0x46,
    0x0F, 0xB7, 0x64, 0x7E, 0x54, 0x41, 0x81, 0xFC, 0xFF, 0xFF, 0x00, 0x00, 0x0F, 0x84, 0x7E, 0x02,
    0x00, 0x00, 0x48, 0x8B, 0x87, 0xA0, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x88, 0xBD, 0x01, 0x00, 0x00,
    0x48, 0x85, 0xC0, 0x49, 0x0F, 0x44, 0xCE, 0x0F, 0xB6, 0x09, 0x44, 0x89, 0xE2, 0xFF, 0x15, 0x7F,
    0x32, 0x32, 0x00, 0x48, 0x85, 0xC0, 0x74, 0xBA, 0x49, 0x89, 0xC5, 0x49, 0xC1, 0xE4, 0x20, 0x48,
    0x89, 0x5C, 0x24, 0x20, 0x4C, 0x89, 0xF1, 0x48, 0x89, 0xFA, 0x4D, 0x89, 0xE0, 0x49, 0x89, 0xC1,
    0xE8, 0x1D, 0xFF, 0xFF, 0xFF, 0x89, 0xC5, 0x49, 0x8B, 0x46, 0x08, 0x48, 0x89, 0x44, 0x24, 0x28,
    0x48, 0x89, 0x5C, 0x24, 0x20, 0x48, 0x89, 0xF9, 0x4C, 0x89, 0xE2, 0x41, 0x89, 0xE8, 0x4D, 0x89,
    0xE9, 0xE8, 0xDC, 0x0E, 0x00, 0x00, 0x85, 0xED, 0x0F, 0x84, 0x74, 0xFF, 0xFF, 0xFF, 0x0F, 0xB6,
    0x46, 0x50, 0x48, 0x83, 0xF8, 0x0D, 0x0F, 0x87, 0x59, 0x01, 0x00, 0x00, 0x4B, 0x8D, 0x0C, 0x7F,
    0x48, 0x8D, 0x96, 0xDA, 0x00, 0x00, 0x00, 0x0F, 0xB7, 0x14, 0x4A, 0x49, 0x89, 0xD4, 0x49, 0xC1,
    0xE4, 0x20, 0x48, 0x8D, 0x0D, 0x45, 0x44, 0x25, 0x00, 0x48, 0x63, 0x04, 0x81, 0x48, 0x01, 0xC8,
    0xFF, 0xE0,
};
constexpr std::size_t ResumeOffsetInLoop   = CommitResumeRva - CommitLoopRva;
constexpr std::size_t TargetReadOffsetInLoop = 0x3DBF3D - CommitLoopRva;

// 63042C + 13 * 4: the relative entry for op 13, 3DC117 - 63042C.
constexpr std::uint8_t OpJumpEntry13Witness[]{ 0xEB, 0xBC, 0xDA, 0xFF };

// 3DC117: cmp dword [rdi+8],4 / setne al / jmp 3DBF25. The 12 bytes replaced.
constexpr std::uint8_t ItemPercentArmWitness[]{
    0x83, 0x7F, 0x08, 0x04, 0x0F, 0x95, 0xC0, 0xE9, 0x02, 0xFE, 0xFF, 0xFF,
};

// 3DC710: mov rax,[rdi+0A0h] / test / jz / mov ecx,[rax]. The owner unit and its type.
constexpr std::uint8_t OwnerLoadWitness[]{
    0x48, 0x8B, 0x87, 0xA0, 0x00, 0x00, 0x00, 0x48, 0x85, 0xC0, 0x0F, 0x84, 0x0F, 0x01, 0x00, 0x00,
    0x8B, 0x08, 0xE9, 0x0D, 0x01, 0x00, 0x00,
};

// 3DC834: op 11 needs a player or monster owner, op 13 needs an item owner,
// ops 1 and 13 on an item owner take the base Stats path.
constexpr std::uint8_t OwnerGatesWitness[]{
    0x83, 0xFD, 0x0B, 0x0F, 0x94, 0xC2, 0x83, 0xF9, 0x02, 0x41, 0x0F, 0x93, 0xC0, 0x44, 0x84, 0xC2,
    0x0F, 0x85, 0xB6, 0xFD, 0xFF, 0xFF, 0x83, 0xFD, 0x0D, 0x0F, 0x94, 0xC2, 0x83, 0xF9, 0x04, 0x41,
    0x0F, 0x95, 0xC0, 0x44, 0x84, 0xC2, 0x0F, 0x85, 0xA0, 0xFD, 0xFF, 0xFF, 0x83, 0xF9, 0x04, 0x0F,
    0x95, 0xC1, 0x83, 0xFD, 0x0B, 0x0F, 0x94, 0xC2, 0x08, 0xCA,
};

// 3DC90F: owner unit +88h -> its base Stats (+30h pointer, +38h count),
// binary search on 16-byte entries for the target key at layer 0.
constexpr std::uint8_t OwnerBaseWitness[]{
    0xC7, 0x44, 0x24, 0x44, 0x00, 0x00, 0x00, 0x00, 0x48, 0x85, 0xC0, 0x0F, 0x84, 0xE5, 0xFC, 0xFF,
    0xFF, 0x48, 0x8B, 0x88, 0x88, 0x00, 0x00, 0x00, 0x48, 0x85, 0xC9, 0x0F, 0x84, 0xD0, 0xFC, 0xFF,
    0xFF, 0x4C, 0x8B, 0x41, 0x38, 0x4D, 0x85, 0xC0, 0x0F, 0x84, 0x7F, 0x02, 0x00, 0x00, 0x4C, 0x8B,
    0x49, 0x30, 0x31, 0xD2, 0x4D, 0x89, 0xC3, 0x4D, 0x89, 0xC2, 0xEB, 0x11, 0x0F, 0x1F, 0x44, 0x00,
    0x00, 0x4D, 0x89, 0xD3, 0x4D, 0x85, 0xD2, 0x0F, 0x84, 0x62, 0x02, 0x00, 0x00, 0x49, 0xD1, 0xEA,
    0x4D, 0x8D, 0x3C, 0x12, 0x49, 0xC1, 0xE7, 0x04, 0x4B, 0x39, 0x34, 0x39, 0x73, 0xE3, 0x4C, 0x01,
    0xD2, 0x48, 0xFF, 0xC2, 0x49, 0xF7, 0xD2, 0x4D, 0x01, 0xDA, 0xEB, 0xD5,
};

// 831DD0: ReadWideUnitBaseStat, a thunk to 3D8AD0.
constexpr std::uint8_t ReadBaseExportWitness[]{
    0x48, 0x83, 0xEC, 0x28, 0xE8, 0xF7, 0x6C, 0xBA, 0xFF, 0x89, 0xC0, 0x48, 0x83, 0xC4, 0x28, 0xC3,
};

// 3D8AD0: (unit, statId, layer) -> unit+88h base Stats value for key
// statId<<32|layer; 0 for no unit, no list, statId >= 8000h or no record.
constexpr std::uint8_t ReadBaseImplWitness[]{
    0x56, 0x57, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x31, 0xC0, 0x48, 0x85, 0xC9, 0x0F, 0x84, 0xDE, 0x00,
    0x00, 0x00, 0x48, 0x8B, 0xB1, 0x88, 0x00, 0x00, 0x00, 0x48, 0x85, 0xF6, 0x41, 0x0F, 0x94, 0xC1,
    0x81, 0xFA, 0x00, 0x80, 0x00, 0x00, 0x41, 0x0F, 0x93, 0xC2, 0x45, 0x08, 0xCA, 0x0F, 0x85, 0xBD,
    0x00, 0x00, 0x00, 0x44, 0x89, 0xC7, 0x0F, 0xB6, 0x89, 0xBD, 0x01, 0x00, 0x00, 0x48, 0x89, 0xD3,
    0xFF, 0x15, 0xDA, 0x66, 0x32, 0x00, 0x48, 0x89, 0xC1, 0x31, 0xC0, 0x48, 0x85, 0xC9, 0x0F, 0x84,
    0x9C, 0x00, 0x00, 0x00, 0x48, 0xC1, 0xE3, 0x20, 0x89, 0xFA, 0x48, 0x09, 0xDA, 0x4C, 0x8B, 0x4E,
    0x38, 0x31, 0xC0, 0x4D, 0x85, 0xC9, 0x74, 0x3F, 0x4C, 0x8B, 0x56, 0x30, 0x45, 0x31, 0xC0, 0x4C,
    0x89, 0xCF, 0x4D, 0x89, 0xCB, 0xEB, 0x11, 0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x4C, 0x89, 0xDF, 0x4D, 0x85, 0xDB, 0x74, 0x22, 0x49, 0xD1, 0xEB, 0x4B, 0x8D, 0x1C, 0x03, 0x48,
    0xC1, 0xE3, 0x04, 0x49, 0x39, 0x14, 0x1A, 0x73, 0xE7, 0x4D, 0x01, 0xD8, 0x49, 0xFF, 0xC0, 0x49,
    0xF7, 0xD3, 0x49, 0x01, 0xFB, 0xEB, 0xD9, 0x45, 0x31, 0xC0, 0x4D, 0x39, 0xC8, 0x74, 0x41, 0x4C,
    0x8B, 0x4E, 0x30, 0x49, 0xC1, 0xE0, 0x04, 0x4B, 0x39, 0x14, 0x01, 0x75, 0x33, 0x4D, 0x01, 0xC1,
    0x41, 0x8B, 0x41, 0x08, 0xF6, 0x41, 0x05, 0x02, 0x74, 0x26, 0x83, 0x7E, 0x1C, 0x00, 0x79, 0x20,
    0x48, 0x8B, 0x96, 0xA0, 0x00, 0x00, 0x00, 0x48, 0x85, 0xD2, 0x74, 0x14, 0x83, 0x3A, 0x01, 0x77,
    0x0F, 0x8B, 0x51, 0x28, 0x39, 0xD0, 0x7D, 0x08, 0x0F, 0xB6, 0x49, 0x14, 0xD3, 0xE2, 0x89, 0xD0,
    0x48, 0x83, 0xC4, 0x20, 0x5B, 0x5F, 0x5E, 0xC3,
};

static_assert(sizeof(ItemPercentArmWitness) == SiteSize);
static_assert(CommitLoopWitness[ResumeOffsetInLoop] == 0x22 && CommitLoopWitness[ResumeOffsetInLoop + 1] == 0x44
    && CommitLoopWitness[ResumeOffsetInLoop + 2] == 0x24 && CommitLoopWitness[ResumeOffsetInLoop + 3] == 0x34);
static_assert(CommitLoopWitness[TargetReadOffsetInLoop] == 0x46 && CommitLoopWitness[TargetReadOffsetInLoop + 1] == 0x0F
    && CommitLoopWitness[TargetReadOffsetInLoop + 2] == 0xB7 && CommitLoopWitness[TargetReadOffsetInLoop + 5] == 0x54);
// The arm's own jmp lands on the resume block: 3DC11E + 5 + FFFFFE02h = 3DBF25.
static_assert(ItemPercentArmRva + 7 + JumpSize - 0x1FE == CommitResumeRva);

struct CoreWitness {
    std::uintptr_t      rva;
    const std::uint8_t* bytes;
    std::size_t         size;
    const char*         name;
};

template <std::size_t N>
constexpr auto W(std::uintptr_t rva, const std::uint8_t (&bytes)[N], const char* name) noexcept -> CoreWitness {
    return { rva, bytes, N, name };
}

constexpr CoreWitness CoreWitnesses[]{
    W(EvaluateUpdateRva, EvaluatePrologueWitness, "evaluate-and-update prologue"),
    W(CommitLoopRva, CommitLoopWitness, "per-target commit loop"),
    W(OpJumpEntry13Rva, OpJumpEntry13Witness, "op 13 switch entry"),
    W(ItemPercentArmRva, ItemPercentArmWitness, "op 13 commit decision"),
    W(OwnerLoadRva, OwnerLoadWitness, "op 13 owner unit read"),
    W(OwnerGatesRva, OwnerGatesWitness, "op 11 / op 13 owner gates"),
    W(OwnerBaseRva, OwnerBaseWitness, "op 13 owner base Stats read"),
    W(ReadBaseExportRva, ReadBaseExportWitness, "ReadWideUnitBaseStat export"),
    W(ReadBaseImplRva, ReadBaseImplWitness, "ReadWideUnitBaseStat body"),
};

// ---------------------------------------------------------------------------
// The stub
// ---------------------------------------------------------------------------
//
// +00  48 83 EC 20              sub   rsp, 20h            ; shadow, stays aligned
// +04  48 89 F9                 mov   rcx, rdi            ; statList
// +07  42 0F B7 54 7E 54        movzx edx, word [rsi+r15*2+54h] ; target stat id
// +0D  48 B8 imm64              mov   rax, AllowCommit
// +17  FF D0                    call  rax
// +19  48 83 C4 20              add   rsp, 20h
// +1D  0F B6 C0                 movzx eax, al
// +20  FF 25 00 00 00 00        jmp   qword ptr [rip+0]
// +26  dq                       D2RCore+3DBF25
// +2E  83 7F 08 04              cmp   dword [rdi+8], 4    ; the native decision,
// +32  0F 95 C0                 setne al                  ; used after unload
// +35  FF 25 00 00 00 00        jmp   qword ptr [rip+0]
// +3B  dq                       D2RCore+3DBF25
constexpr std::uint8_t DecisionStub[]{
    0x48, 0x83, 0xEC, 0x20, 0x48, 0x89, 0xF9, 0x42, 0x0F, 0xB7, 0x54, 0x7E, 0x54, 0x48, 0xB8, 0x88,
    0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x20, 0x0F, 0xB6, 0xC0,
    0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x83, 0x7F,
    0x08, 0x04, 0x0F, 0x95, 0xC0, 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55,
    0x66, 0x77, 0x88,
};

constexpr std::size_t StubPredicateImm64  = 0x0F;
constexpr std::size_t StubResumeSlot      = 0x26;
constexpr std::size_t StubNativeOffset    = 0x2E;
constexpr std::size_t StubNativeResumeSlot = 0x3B;

constexpr std::size_t HookPageBytes  = 4'096;
constexpr std::size_t StubPageOffset = 0x00;

constexpr auto MovabsBefore(const std::uint8_t* code, std::size_t slot) noexcept -> bool {
    return slot >= 2 && code[slot - 2] == 0x48 && code[slot - 1] == 0xB8;
}

constexpr auto JumpBefore(const std::uint8_t* code, std::size_t slot) noexcept -> bool {
    return slot >= 6 && code[slot - 6] == 0xFF && code[slot - 5] == 0x25
        && code[slot - 4] == 0x00 && code[slot - 3] == 0x00
        && code[slot - 2] == 0x00 && code[slot - 1] == 0x00;
}

static_assert(sizeof(DecisionStub) == 0x43);
static_assert(MovabsBefore(DecisionStub, StubPredicateImm64));
static_assert(JumpBefore(DecisionStub, StubResumeSlot));
static_assert(JumpBefore(DecisionStub, StubNativeResumeSlot));
static_assert(StubResumeSlot + 8 == StubNativeOffset);
// The native tail replays the first 7 bytes of the replaced arm.
static_assert(DecisionStub[StubNativeOffset] == ItemPercentArmWitness[0]
    && DecisionStub[StubNativeOffset + 1] == ItemPercentArmWitness[1]
    && DecisionStub[StubNativeOffset + 2] == ItemPercentArmWitness[2]
    && DecisionStub[StubNativeOffset + 3] == ItemPercentArmWitness[3]
    && DecisionStub[StubNativeOffset + 4] == ItemPercentArmWitness[4]
    && DecisionStub[StubNativeOffset + 5] == ItemPercentArmWitness[5]
    && DecisionStub[StubNativeOffset + 6] == ItemPercentArmWitness[6]);
// The target id read is the loop's own read with r12d swapped for edx.
static_assert(DecisionStub[0x07] == 0x42 && DecisionStub[0x08] == 0x0F && DecisionStub[0x09] == 0xB7
    && DecisionStub[0x0A] == 0x54 && DecisionStub[0x0B] == 0x7E && DecisionStub[0x0C] == 0x54);
static_assert(StubPageOffset + sizeof(DecisionStub) <= HookPageBytes);

// Turns the stub into the native decision alone: jmp short +2Ch, landing on +2Eh.
constexpr std::uint8_t StubToNative[]{ 0xEB, 0x2C };
static_assert(static_cast<std::size_t>(0x02 + StubToNative[1]) == StubNativeOffset);

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

struct Config {
    bool enabled    = true;
    bool logRepairs = false;
};

constexpr char DefaultConfigToml[] = R"toml(# Enhanced Damage Fix (celestialrayone.enhanced-damage-fix)
#
# Fixes the classic "ED/dmg bug": Enhanced Damage on any item that is not a
# weapon (helm, armor, shield, belt, boots, gloves, jewel, charm, ring, amulet)
# was lost whenever that item also carried flat minimum or maximum damage at the
# moment the Enhanced Damage was applied.
#
# What players saw:
#   - item with +max damage, ED jewel socketed: ED only raised minimum damage
#   - item with +min damage, ED jewel socketed: ED only raised maximum damage
#   - jewel with ED and +max (or +min): the matching side was lost
#   - jewel with ED and both +min and +max (Protector's Stone): no ED at all
#   - ED socketed first and the flat damage later: worked, hence the
#     dependence on socketing order
#   - inside a weapon: always worked
#
# Why it happened:
#   Enhanced Damage is two stats, item_maxdamage_percent and
#   item_mindamage_percent. Both use itemstatcost.txt op 13, which adds the
#   percent of the item's OWN BASE damage. Only weapons have base damage. After
#   applying it there, the stat engine keeps the percent from also reaching the
#   character so weapon ED is not counted twice. The engine took that decision
#   by asking "does this item have any min/max damage at all?", flat bonuses
#   included, instead of "did op 13 multiply base damage here?". So on a helm or
#   jewel with flat damage the percent was held back from the character while
#   op 13 had multiplied nothing. The max side is decided against max damage and
#   the min side against min damage, hence the one-sided cases.
#
# What the plugin changes:
#   That one decision now uses op 13's own condition: the percent stays on the
#   item only when the item has base damage of that kind.
#     - weapons: exactly as before
#     - every other item: its Enhanced Damage always reaches the character and
#       multiplies total damage like any other off-weapon Enhanced Damage, no
#       matter what else the item carries or the order things were socketed in
#     - nothing is counted twice: op 13 never multiplies flat damage
#     - Enhanced Defense and max durability % also use op 13; they keep the
#       game's own decision
#
# Existing characters are fixed when they load: item stats are rebuilt from the
# save and pass through the corrected decision.
#
# Balance: every non-weapon item that combines Enhanced Damage with flat damage
# now gives its full Enhanced Damage.
#
# Technical: under D2RLoader 1.3.1 the stat engine runs in D2RCore.dll. The
# plugin replaces the 12-byte op 13 decision at D2RCore RVA 0x3DC117 with a jump
# to its own code and checks D2RCore's bytes first. On any other D2RCore build
# it installs nothing and says so in the log and the console.
#
# Console command: edfix (install state and counters)

[enhanced_damage_fix]

# Master switch. false leaves the game completely vanilla.
enabled = true

# Write the first 16 repaired decisions to the plugin log: the items.txt row of
# the item and the damage stat (min or max side). The first repair is always
# logged once. Leave false unless you are checking a specific item.
log_repairs = false
)toml";

constexpr std::uint32_t MaximumConfigBytes = 32'768;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

enum class InstallState : std::uint8_t {
    NotLoaded,
    DisabledByConfig,
    CoreMissing,
    UnsupportedCore,
    InstallFailed,
    Active,
    PartiallyActive,
};

using ReadUnitBaseStatFn = std::uint32_t(__fastcall*)(void* unit, std::uint32_t statId, std::uint32_t layer) noexcept;

const D2RL::PluginContext* Context{};
std::uintptr_t             CoreBase{};
std::uintptr_t             CoreSize{};
ReadUnitBaseStatFn         ReadUnitBaseStat{};
Config                     Settings{};
InstallState               State{ InstallState::NotLoaded };
void*                      HookPage{};
bool                       SitePatched{};
std::uint8_t               SiteJump[SiteSize]{};

std::atomic<std::uint64_t> RepairedMaximum{};
std::atomic<std::uint64_t> RepairedMinimum{};
std::atomic<std::uint64_t> WeaponDecisions{};
std::atomic<std::uint64_t> OtherStatDecisions{};
std::atomic<std::uint64_t> LoggedRepairs{};
std::atomic<std::int32_t>  LastRepairedRow{ -1 };
std::atomic<std::uint32_t> LastRepairedStat{};

constexpr std::uint64_t MaximumLoggedRepairs = 16;

template <typename T>
auto Read(const void* base, std::size_t offset) noexcept -> T {
    T value{};
    std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(T));
    return value;
}

// ---------------------------------------------------------------------------
// Config parsing (small TOML subset, no external dependency)
// ---------------------------------------------------------------------------

auto Trim(std::string_view value) noexcept -> std::string_view {
    std::size_t first = 0;
    while (first < value.size()
            && (value[first] == ' ' || value[first] == '\t' || value[first] == '\r' || value[first] == '\n')) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first
            && (value[last - 1] == ' ' || value[last - 1] == '\t' || value[last - 1] == '\r'
                || value[last - 1] == '\n')) {
        --last;
    }
    return value.substr(first, last - first);
}

void ParseBool(std::string_view key, std::string_view value, bool& out) noexcept {
    if (value == "true" || value == "1") {
        out = true;
    } else if (value == "false" || value == "0") {
        out = false;
    } else {
        D2RL::LogWarnF(Context, "EnhancedDamageFix: %.*s = %.*s is not true or false; keeping %s.",
            static_cast<int>(key.size()), key.data(), static_cast<int>(value.size()), value.data(),
            out ? "true" : "false");
    }
}

void ApplyConfigLine(std::string_view key, std::string_view value) noexcept {
    if (key == "enabled") {
        ParseBool(key, value, Settings.enabled);
    } else if (key == "log_repairs") {
        ParseBool(key, value, Settings.logRepairs);
    }
}

void ParseConfig(std::string_view text) noexcept {
    std::size_t cursor = 0;
    while (cursor <= text.size()) {
        const std::size_t breakAt = text.find('\n', cursor);
        const std::size_t end = breakAt == std::string_view::npos ? text.size() : breakAt;
        std::string_view line = Trim(text.substr(cursor, end - cursor));
        cursor = end + 1;
        const std::size_t comment = line.find('#');
        if (comment != std::string_view::npos) line = Trim(line.substr(0, comment));
        if (!line.empty() && line.front() != '[') {
            const std::size_t equals = line.find('=');
            if (equals != std::string_view::npos) {
                ApplyConfigLine(Trim(line.substr(0, equals)), Trim(line.substr(equals + 1)));
            }
        }
        if (breakAt == std::string_view::npos) break;
    }
}

void ReadConfiguration() noexcept {
    if (!Context->EnsureConfig(DefaultConfigToml)) {
        Context->LogWarn("EnhancedDamageFix: the config file could not be created; using defaults.");
        return;
    }
    std::string buffer(MaximumConfigBytes, '\0');
    if (!Context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size()), nullptr)) {
        Context->LogWarn("EnhancedDamageFix: the config file could not be read; using defaults.");
        return;
    }
    buffer.resize(std::strlen(buffer.c_str()));
    ParseConfig(buffer);
}

// ---------------------------------------------------------------------------
// The corrected decision
// ---------------------------------------------------------------------------
//
// Called from the stub at D2RCore 3DC117 with the engine's own statList (rdi)
// and the op 13 target being decided (word [rsi+r15*2+54h]). Returns the al the
// engine expects there: 1 lets the percent stat be committed to this statlist,
// and from there reach the socket host and the character; 0 withholds it.
auto __fastcall AllowCommit(const void* statList, std::uint32_t targetStat) noexcept -> std::uint8_t {
    const auto listOwnerType = Read<std::uint32_t>(statList, StatListOwnerTypeOffset);
    if (listOwnerType != UnitTypeItem) return 1;  // native, and the common case: no reads beyond this

    auto* owner = Read<void*>(statList, StatListOwnerOffset);
    const bool ownerIsItem = owner != nullptr && Read<std::uint32_t>(owner, UnitTypeOffset) == UnitTypeItem;
    std::int32_t ownerBase = 0;
    if (ownerIsItem && IsPhysicalDamageStat(targetStat)) {
        ownerBase = static_cast<std::int32_t>(ReadUnitBaseStat(owner, targetStat, 0));
    }

    switch (Decide(listOwnerType, targetStat, ownerIsItem, ownerBase)) {
    case Verdict::NotItemList:
        return 1;
    case Verdict::OtherStat:
        OtherStatDecisions.fetch_add(1, std::memory_order_relaxed);
        return 0;
    case Verdict::OnItemBase:
        WeaponDecisions.fetch_add(1, std::memory_order_relaxed);
        return 0;
    case Verdict::Repaired:
        break;
    }

    const bool minimum = IsMinimumDamageStat(targetStat);
    (minimum ? RepairedMinimum : RepairedMaximum).fetch_add(1, std::memory_order_relaxed);
    const std::int32_t row = ownerIsItem ? Read<std::int32_t>(owner, UnitClassIdOffset) : -1;
    LastRepairedRow.store(row, std::memory_order_relaxed);
    LastRepairedStat.store(targetStat, std::memory_order_relaxed);

    const std::uint64_t logged = LoggedRepairs.fetch_add(1, std::memory_order_relaxed);
    if (logged == 0 || (Settings.logRepairs && logged < MaximumLoggedRepairs)) {
        D2RL::LogInfoF(Context,
            "EnhancedDamageFix: repaired Enhanced Damage on items.txt row %d against stat %u (%s side); "
            "the game would have withheld it.",
            row, targetStat, minimum ? "minimum" : "maximum");
    }
    return 1;
}

// ---------------------------------------------------------------------------
// D2RCore access
// ---------------------------------------------------------------------------

auto LocateCore() noexcept -> bool {
    HMODULE module = GetModuleHandleW(CoreModuleName);
    if (module == nullptr) return false;
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + static_cast<std::uintptr_t>(dos->e_lfanew));
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    CoreBase = base;
    CoreSize = nt->OptionalHeader.SizeOfImage;
    return true;
}

auto CoreAddress(std::uintptr_t rva) noexcept -> std::uintptr_t {
    return CoreBase + rva;
}

auto CoreMatches(const CoreWitness& witness) noexcept -> bool {
    if (witness.rva + witness.size > CoreSize) return false;
    return std::memcmp(reinterpret_cast<const void*>(CoreAddress(witness.rva)), witness.bytes, witness.size) == 0;
}

auto VerifyCore() noexcept -> bool {
    bool ok = true;
    for (const CoreWitness& witness : CoreWitnesses) {
        if (!CoreMatches(witness)) {
            D2RL::LogErrorF(Context,
                "EnhancedDamageFix: D2RCore %s at RVA 0x%llX does not match D2RCore.dll 1.3.1, or another "
                "plugin already changed it.",
                witness.name, static_cast<unsigned long long>(witness.rva));
            ok = false;
        }
    }
    if (!ok) return false;

    const FARPROC exported = GetProcAddress(reinterpret_cast<HMODULE>(CoreBase), ReadBaseStatExport);
    if (reinterpret_cast<std::uintptr_t>(exported) != CoreAddress(ReadBaseExportRva)) {
        D2RL::LogErrorF(Context,
            "EnhancedDamageFix: D2RCore export %s is not at RVA 0x%llX; this is not D2RCore.dll 1.3.1.",
            ReadBaseStatExport, static_cast<unsigned long long>(ReadBaseExportRva));
        return false;
    }
    ReadUnitBaseStat = reinterpret_cast<ReadUnitBaseStatFn>(CoreAddress(ReadBaseExportRva));
    return true;
}

// Writes code and reads it back. D2RCore is not D2R.exe, so the loader's patch
// API (D2R.exe RVAs only) cannot reach it.
auto WriteCode(std::uintptr_t address, const std::uint8_t* bytes, std::size_t size) noexcept -> bool {
    auto* target = reinterpret_cast<void*>(address);
    DWORD previous = 0;
    if (!VirtualProtect(target, size, PAGE_EXECUTE_READWRITE, &previous)) return false;
    std::memcpy(target, bytes, size);
    DWORD ignored = 0;
    VirtualProtect(target, size, previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, size);
    return std::memcmp(target, bytes, size) == 0;
}

// ---------------------------------------------------------------------------
// Hook page and site
// ---------------------------------------------------------------------------

auto CanEncodeRel32(std::uintptr_t from, std::uintptr_t to) noexcept -> bool {
    const std::int64_t delta =
        static_cast<std::int64_t>(to) - static_cast<std::int64_t>(from) - static_cast<std::int64_t>(JumpSize);
    return delta >= INT32_MIN && delta <= INT32_MAX;
}

// A page within rel32 reach of the site, above D2RCore first, then below.
auto AllocateNear(std::uintptr_t hint, std::size_t size) noexcept -> void* {
    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    const auto granularity = static_cast<std::uintptr_t>(systemInfo.dwAllocationGranularity);
    const auto aligned = hint & ~(granularity - 1U);
    for (std::uintptr_t delta = granularity; delta < 0x7000'0000ULL; delta += granularity) {
        const std::uintptr_t candidates[2]{ aligned + delta, delta < aligned ? aligned - delta : 0 };
        for (const std::uintptr_t candidate : candidates) {
            if (candidate == 0 || !CanEncodeRel32(hint, candidate)) continue;
            if (auto* memory = VirtualAlloc(reinterpret_cast<void*>(candidate), size,
                    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) {
                return memory;
            }
        }
    }
    return nullptr;
}

auto PageAddress(std::size_t offset) noexcept -> std::uintptr_t {
    return reinterpret_cast<std::uintptr_t>(HookPage) + offset;
}

void WriteQword(std::uint8_t* at, std::uint64_t value) noexcept {
    std::memcpy(at, &value, sizeof(value));
}

auto BuildHookPage() noexcept -> bool {
    HookPage = AllocateNear(CoreAddress(ItemPercentArmRva), HookPageBytes);
    if (HookPage == nullptr) {
        Context->LogError("EnhancedDamageFix: no hook page was available within rel32 reach of D2RCore.");
        return false;
    }

    auto* page = static_cast<std::uint8_t*>(HookPage);
    std::memset(page, 0xCC, HookPageBytes);
    std::memcpy(page + StubPageOffset, DecisionStub, sizeof(DecisionStub));
    WriteQword(page + StubPageOffset + StubPredicateImm64, reinterpret_cast<std::uint64_t>(&AllowCommit));
    WriteQword(page + StubPageOffset + StubResumeSlot, CoreAddress(CommitResumeRva));
    WriteQword(page + StubPageOffset + StubNativeResumeSlot, CoreAddress(CommitResumeRva));

    DWORD previous = 0;
    if (!VirtualProtect(page, HookPageBytes, PAGE_EXECUTE_READ, &previous)) {
        Context->LogError("EnhancedDamageFix: hook page protection could not be finalized.");
        VirtualFree(HookPage, 0, MEM_RELEASE);
        HookPage = nullptr;
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), page, HookPageBytes);
    return true;
}

// Leaves the stub running the native decision only. After this nothing in the
// page reaches this DLL.
auto RetargetStubToNative() noexcept -> bool {
    if (HookPage == nullptr) return true;
    auto* page = static_cast<std::uint8_t*>(HookPage);
    DWORD previous = 0;
    if (!VirtualProtect(page, HookPageBytes, PAGE_EXECUTE_READWRITE, &previous)) return false;
    std::memcpy(page + StubPageOffset, StubToNative, sizeof(StubToNative));
    DWORD ignored = 0;
    VirtualProtect(page, HookPageBytes, previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), page, HookPageBytes);
    return true;
}

// E9 rel32 to the stub, the rest of the 12-byte arm NOP.
void EncodeSiteJump(std::uintptr_t site, std::uintptr_t target, std::uint8_t* out) noexcept {
    out[0] = 0xE9;
    const auto displacement = static_cast<std::int32_t>(
        static_cast<std::int64_t>(target) - static_cast<std::int64_t>(site + JumpSize));
    std::memcpy(out + 1, &displacement, sizeof(displacement));
    std::memset(out + JumpSize, 0x90, SiteSize - JumpSize);
}

auto PatchSite() noexcept -> bool {
    const std::uintptr_t site = CoreAddress(ItemPercentArmRva);
    if (!CanEncodeRel32(site, PageAddress(StubPageOffset))) {
        Context->LogError("EnhancedDamageFix: the hook page is out of rel32 reach of the site.");
        return false;
    }
    EncodeSiteJump(site, PageAddress(StubPageOffset), SiteJump);
    if (!WriteCode(site, SiteJump, SiteSize)) {
        Context->LogError("EnhancedDamageFix: the op 13 decision in D2RCore could not be written.");
        return false;
    }
    SitePatched = true;
    return true;
}

// Puts the native 12 bytes back, only while the site still holds this plugin's
// jump.
auto RestoreSite() noexcept -> bool {
    if (!SitePatched) return true;
    const std::uintptr_t site = CoreAddress(ItemPercentArmRva);
    if (std::memcmp(reinterpret_cast<const void*>(site), SiteJump, SiteSize) != 0) {
        Context->LogError("EnhancedDamageFix: the site no longer holds this plugin's jump; left as found.");
        return false;
    }
    if (!WriteCode(site, ItemPercentArmWitness, SiteSize)) return false;
    SitePatched = false;
    return true;
}

// Returns false only when nothing can reach this DLL, so the loader may unload it.
auto Install() noexcept -> bool {
    if (!BuildHookPage()) {
        State = InstallState::InstallFailed;
        return false;
    }
    if (PatchSite()) {
        State = InstallState::Active;
        return true;
    }
    // Only a site holding this plugin's jump can reach the page. Point the page
    // at the native decision first, then make sure the native bytes are back.
    RetargetStubToNative();
    const std::uintptr_t site = CoreAddress(ItemPercentArmRva);
    const bool untouched =
        std::memcmp(reinterpret_cast<const void*>(site), ItemPercentArmWitness, SiteSize) == 0;
    if (untouched || WriteCode(site, ItemPercentArmWitness, SiteSize)) {
        State = InstallState::InstallFailed;
        return false;
    }
    Context->LogError(
        "EnhancedDamageFix: the site could not be restored; staying loaded so it keeps a valid target.");
    State = InstallState::PartiallyActive;
    return true;
}

// ---------------------------------------------------------------------------
// Console command
// ---------------------------------------------------------------------------

auto StateName() noexcept -> const char* {
    switch (State) {
    case InstallState::DisabledByConfig: return "disabled by config";
    case InstallState::CoreMissing:      return "D2RCore.dll not found, nothing installed";
    case InstallState::UnsupportedCore:  return "unsupported D2RCore build, nothing installed (see log)";
    case InstallState::InstallFailed:    return "install failed, nothing active (see log)";
    case InstallState::Active:           return "active";
    case InstallState::PartiallyActive:  return "PARTIALLY active, see log";
    default:                             return "not loaded";
    }
}

auto __cdecl StatusCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept
        -> D2RL::ConsoleCommandResult {
    if (command == nullptr || command->plugin == nullptr) return D2RL::ConsoleCommandResult::Failed;

    char line[384];
    std::snprintf(line, sizeof(line), "Enhanced Damage Fix: %s%s", StateName(),
        SitePatched ? " (D2RCore op 13 decision at RVA 0x3DC117)." : ".");
    command->plugin->WriteConsoleMessage(line);

    std::snprintf(line, sizeof(line),
        "  repaired: max side %llu, min side %llu | weapons, left to the game %llu | "
        "Enhanced Defense / durability, left to the game %llu",
        static_cast<unsigned long long>(RepairedMaximum.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(RepairedMinimum.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(WeaponDecisions.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(OtherStatDecisions.load(std::memory_order_relaxed)));
    command->plugin->WriteConsoleMessage(line);

    const std::int32_t row = LastRepairedRow.load(std::memory_order_relaxed);
    if (row >= 0) {
        std::snprintf(line, sizeof(line), "  last repair: items.txt row %d, stat %u.", row,
            LastRepairedStat.load(std::memory_order_relaxed));
        command->plugin->WriteConsoleMessage(line);
    }
    return D2RL::ConsoleCommandResult::Handled;
}

void RegisterStatusCommand() noexcept {
    if (!Context->RegisterConsoleCommand("edfix", &StatusCommand,
            "Shows Enhanced Damage Fix install state and how many Enhanced Damage commits it repaired.")) {
        Context->LogWarn("EnhancedDamageFix: the console command was refused.");
    }
}

constexpr D2RL::PluginInfo Info{
    .infoSize    = D2RL::PluginInfoSize,
    .abiVersion  = D2RL_PLUGIN_ABI_VERSION,
    .id          = "celestialrayone.enhanced-damage-fix",
    .name        = "Enhanced Damage Fix",
    .version     = "1.0.0",
    .author      = "CelestialRayOne",
    .description = "Enhanced Damage on armor, jewels, charms and every other non-weapon item reaches the "
                   "character even when the item also adds minimum or maximum damage.",
    .flags       = D2RL::PluginFlags::Shared | D2RL::PluginFlags::NativeHooks,
};

}  // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
    return &Info;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
    if (!D2RL::HasContext(context)) return false;
    Context = context;

    ReadConfiguration();
    RegisterStatusCommand();

    if (!Settings.enabled) {
        State = InstallState::DisabledByConfig;
        Context->LogInfo("EnhancedDamageFix: disabled by config.");
        return true;
    }
    if (!LocateCore()) {
        State = InstallState::CoreMissing;
        Context->LogError("EnhancedDamageFix: D2RCore.dll is not loaded; nothing was installed.");
        return true;
    }
    if (!VerifyCore()) {
        State = InstallState::UnsupportedCore;
        Context->LogError("EnhancedDamageFix: this D2RCore build does not match 1.3.1; nothing was installed.");
        return true;
    }
    if (!Install()) {
        Context->LogError("EnhancedDamageFix: install failed; the game is unchanged.");
        return false;
    }

    D2RL::LogInfoF(Context,
        "EnhancedDamageFix: %s. D2RCore 0x3DC117 now commits Enhanced Damage on non-weapon items; "
        "weapons, Enhanced Defense and durability keep the game's decision.",
        StateName());
    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    if (Context == nullptr) return;
    RetargetStubToNative();
    RestoreSite();
    // The hook page is deliberately kept: a thread may be inside the stub now.
}

}  // namespace CelestialRayOne::EnhancedDamageFix
