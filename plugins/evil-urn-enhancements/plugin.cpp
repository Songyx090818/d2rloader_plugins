// Evil Urn
//
// Three evil urn features in one plugin. Each one is switched on and off in
// its own section of celestialrayone.evil-urn.toml, and each one is checked
// against the game on its own: a feature whose checks fail is left out and
// reported in the log and by the console command, the other two still
// install.
//
//   [spawn_chance]  per-room chance of a random urn, by difficulty
//   [boss_affixes]  extra random affixes for the boss an urn spawns
//   [blocklist]     monster types an urn must never spawn
//
// Built against D2R 3.3 in the D2RLoader 1.3.1 process image (base
// 0x140000000), PluginSDK 0.3.0. Everything below was read out of the live
// image and disassembled before being relied on.
//
// ===========================================================================
// The urn itself: objects.txt OperateFn 68 at 0x590960
// ===========================================================================
//   The OperateFn table is at 0x23A51C0, slots 0 to 74. Its NULL slots match
//   2.4 and D2MOO (0, 11, 13, 17, 35-38, 60, 62-64), slot 74 is new in 3.x,
//   and the slots Ruff's corpus names line up: 6 = 0x5DC570 TowerTome,
//   15 = 0x58F680 Portal, 32 = 0x528270 Bank. Slot 68 (0x23A53E0) holds
//   0x590960, which is D2MOO's OBJECTS_OperateFunction68_EvilUrn step for
//   step: neutral-mode gate, chest drop, operating mode, ENDANIM event, then
//
//     5909F4  48 8B 4F 18           mov  rcx, [rdi+18h]
//     5909F8  BA FF 00 00 00        mov  edx, 0FFh
//     5909FD  E8 FE 30 BC FF        call 153B00                 ; seed roll 0..254
//     590A02  3B 83 44 01 00 00     cmp  eax, [rbx+144h]        ; objects.txt Parm4
//     590A08  0F 87 C6 00 00 00     ja   590AD4                 ; no spawn
//     ...                                                       ; eax = urn level id
//     590A24  48 8B 0F              mov  rcx, [rdi]             ; game
//     590A27  8B D0                 mov  edx, eax
//     590A29  48 8B 89 D0 01 00 00  mov  rcx, [rcx+1D0h]
//     590A30  E8 0B 7D F7 FF        call 508740                 ; monster region of the level
//     590A35  0F B6 48 10           movzx ecx, byte [rax+10h]   ; monster type count
//     590A39  48 8D 70 14           lea  rsi, [rax+14h]
//     590A3D  48 6B D1 34           imul rdx, rcx, 34h
//     590A41  48 8D 58 14           lea  rbx, [rax+14h]         ; rbx = first entry
//     590A45  48 03 F2              add  rsi, rdx               ; rsi = end
//     590A48  48 3B DE              cmp  rbx, rsi
//     590A4B  0F 84 7E 00 00 00     je   590ACF                 ; no types
//   loop:
//     590A51  48 8B 07              mov  rax, [rdi]
//     590A54  0F BF 13              movsx edx, word [rbx]       ; the entry's monstats id
//     590A57  0F B6 88 06 01 00 00  movzx ecx, byte [rax+106h]  ; data context
//     590A5E  E8 4D 6B B0 FF        call 975B0                  ; its MonStats2 record
//     590A63  48 85 C0              test rax, rax
//     590A66  75 11                 jne  590A79
//     590A68  ...                   assert, then 590A88
//     590A79  0F B6 80 F0 00 00 00  movzx eax, byte [rax+0F0h]  ; mode flags
//     590A80  23 05 52 8C 80 01     and  eax, [1D996D8]         ; gdwBitMasks[2] = 4, walk
//     590A86  75 0B                 jne  590A93                 ; spawn
//     590A88  48 83 C3 34           add  rbx, 34h               ; next type
//     590A8C  48 3B DE              cmp  rbx, rsi
//     590A8F  75 C0                 jne  590A51
//     590A91  EB 3C                 jmp  590ACF                 ; list exhausted, no spawn
//     590A93  48 8B 4F 08           mov  rcx, [rdi+8]
//     590A97  0F BF 1B              movsx ebx, word [rbx]
//     ...
//     590ACA  E8 B1 8D F0 FF        call 499880                 ; the boss pack
//
//   The spawn chance column at record +144h is the one D2R 3.3's objects.txt
//   names Parm4 (its loader maps Parm0..Parm4 to +134h..+144h; 2.4 and D2MOO
//   called the slot Parm7). The urn row has 255 there, so an opened urn
//   always tries to spawn. The urn spawns the FIRST type in the level's list
//   that has a walk mode. 0x975B0 is the MonStats2-from-monstats-id getter:
//   monstats stride 1FCh, MonStats2 index word at +4Ch, MonStats2 stride
//   128h, null for an id out of range.
//
// ===========================================================================
// [spawn_chance]: per-room urn chance by difficulty
// ===========================================================================
//   0x510F00  the per-room object filler. For each of the level's 8 ObjGrp
//             slots it rolls the room seed % 100 and fires the group when the
//             roll is <= ObjPrb, so ObjPrb N means (N + 1)% per room. A second
//             roll % 100 walks the group's members, adding up PROB, and the
//             first member the roll falls under is placed through
//             PopulateFn table[objects.txt PopulateFn](game, room, DENSITY,
//             object id, 100).
//   0x239BC10 the PopulateFn table, 10 slots, slot 0 NULL:
//               1 0x50D340   2 0x50CF10   3 0x50D1D0   4 0x50C370
//               5 0x50C8C0   6 0x50E070   7 0x50DB60   8 0x50C180
//               9 0x50E1F0
//             The only other caller is 0x510880 (a single group, same call).
//   0x50E1F0  PopulateFn 9, used by the urn (objects.txt row 466):
//             (game, room, density, objectId, chance). It turns the room size
//             into a try count, (density * ((w * h) >> 7)) >> 8 in subtiles,
//             and places at most ONE object, stopping at the first try that
//             finds a free spot. Its only references are its table slot and
//             its unwind entry.
//   0x2EFBF0  room seed accessor: test rcx,rcx / je / lea rax,[rcx+0A0h] / ret.
//             The game's roll is seed = lo * 0x6AC690C5 + hi (64-bit), stored
//             back as (lo, hi), value = low 32 bits.
//   0x48FF40  game context builder: movzx eax, byte [rdx+104h] at +19h is
//             the difficulty byte (0 Normal, 1 Nightmare, 2 Hell).
//
//   Slot 9 of the PopulateFn table is replaced with PopulateHook. For an
//   object id on the list it rolls room seed % txt_percent and keeps the
//   placement only when the roll is below the current difficulty's percent,
//   so a room gets the urn with exactly the difficulty percent. A difficulty
//   whose percent equals txt_percent does not roll at all. Every other
//   object, and every kept placement, goes to the original unchanged.
//
// ===========================================================================
// [boss_affixes]: extra random affixes for the urn's boss
// ===========================================================================
//   0x499880  the boss-pack spawner the urn calls at 590ACA. After creating
//             the boss it calls, at 0x499ACD,
//               0x49E160 (game, boss, championChance, minimum, maximum, list)
//             with minimum = maximum = difficulty + 1 (movzx r9d, difficulty /
//             inc r9d / the same value stored as the fifth argument), then
//             spawns the minions (0x49EAD0), which copy the boss's xfer
//             affixes.
//   0x49E160  the affix roll. With championChance set it first rolls
//             seed % 100 against monumod.txt constants row 0 (20 in ESR,
//             "Champion chance to spawn instead of Unique"); on a hit the
//             pack becomes a champion pack (rank flag 4) with one champion
//             affix and the function returns. Otherwise it rolls
//             minimum + seed % (maximum - minimum + 1) affixes, capped so the
//             monster holds at most 9, drawing without repeats and stopping
//             early when nothing is left to draw. Its callers are 0x499880
//             and 0x49AC30 (only reached from 0x591600, not from the urn).
//
//   Slot 68 of the OperateFn table is replaced with UrnOperateHook, which
//   counts the urn calls in progress on this thread and calls the urn
//   unchanged. 0x49E160 gets an inline hook: while an urn call is in
//   progress, minimum and maximum are both raised by extra_affixes. Inside
//   the urn call the only path to 0x49E160 is the boss-pack spawn, so exactly
//   the urn's boss is affected. The champion roll, the cap and the draw are
//   the game's own.
//
// ===========================================================================
// [blocklist]: monster types an urn must never spawn
// ===========================================================================
//   Port of the ESR D2R 2.4 memory patches (hook 3BF80C redirecting the urn's
//   call to sub_1400A4440 into the wrapper cave 36AE90, with a zero-terminated
//   id table at 36AEC4) to D2R 3.3. In 2.4 the walk test was a call,
//   sub_1400A4440(id, 2), so the cave could wrap it and return 0 for a
//   blocked id. In 3.3 that call no longer exists: the compiler inlined it
//   into the three instructions at 590A79. There is nothing to wrap, so this
//   feature takes over the test itself.
//
//   The 15 bytes at 590A79 (movzx / and / jne) become a jmp into a stub in a
//   private region next to the image, the rest NOPed. The stub replays the
//   test. When the type has a walk mode it also looks the entry's id up in a
//   bitmap built from the config. A blocked id leaves through 590A88, the
//   exit a type without a walk mode takes, and everything else leaves
//   through 590A93, the untouched spawn. The urn's own loop therefore does
//   the skipping, and a level whose types are all blocked or unable to walk
//   takes the vanilla list-exhausted path and spawns nothing.
//
//   Why the window is safe:
//     - it starts and ends on instruction boundaries, the only branch into it
//       is the jne at 590A66 onto its first byte, and nothing references a
//       byte inside it;
//     - rbx (the entry) and rsi (the end) are nonvolatile and never touched;
//     - rax, rcx, rdx and r8 are dead on both exits: 590A88 either loops to
//       590A51, which reloads rax, edx and ecx before its call, or runs out
//       to 590ACF, which reloads rcx and calls; 590A93 reloads rcx and
//       calls, then sets rdx, r8 and r9 before the spawn call;
//     - the flags are dead on both exits, and the stub never uses the stack.
//
//   The stub was assembled with keystone and decoded back with capstone, and
//   every relative jump lands on an instruction start. It was then emulated
//   with unicorn against the original 15 bytes: 5,640 cases over mode-flag
//   bytes, ids 0 to 65535 and several blocklists, and 11,520 more over all 256
//   mode-flag bytes with the region built from the constants in this file.
//   Retargeted (the state it is put in on unload) it matches the original
//   exactly. Armed, it spawns exactly when the type can walk and is not
//   blocked, with rbx, rsi, rdi, rbp, rsp and r9-r15 preserved and both
//   counters exact.
//
// ===========================================================================
// How the three fit together
// ===========================================================================
//   [boss_affixes] owns OperateFn slot 68 and the entry of 0x49E160;
//   [blocklist] owns the 15 bytes at 590A79 inside the urn; [spawn_chance]
//   owns PopulateFn slot 9. None of them touches what another one owns, and
//   none of the checks one feature makes reads bytes another one changes.

#include <D2RLPlugin/api.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace CelestialRayOne::EvilUrn {
namespace {

const D2RL::PluginContext* Context{};
std::uintptr_t             Base{};

// ---------------------------------------------------------------------------
// Native anchors
// ---------------------------------------------------------------------------

constexpr std::uint64_t OperateFunction68Rva = 0x590960;
constexpr std::uint64_t SpawnChanceRva       = 0x5909F4;
constexpr std::uint64_t RegionWalkRva        = 0x590A24;
constexpr std::uint64_t WalkTestRva          = 0x590A79;
constexpr std::uint64_t NextTypeRva          = 0x590A88;
constexpr std::uint64_t SpawnRva             = 0x590A93;
constexpr std::uint64_t WalkModeMaskRva      = 0x1D996D8;   // gdwBitMasks[2]

constexpr std::uint32_t WalkTestSize = 15;
constexpr std::uint32_t JumpSize     = 5;

// 590960: push rdi / sub rsp,50h / mov rdi,rcx / mov rcx,[rcx+8] /
// call 34AB60 (unit mode) / test / je / return 0 / xor edx,edx /
// mov rcx,rdi / call 5930B0 (chest drop) / test rax,rax / je 590AF4.
constexpr std::uint8_t OperateEntryWitness[]{
    0x40, 0x57, 0x48, 0x83, 0xEC, 0x50, 0x48, 0x8B, 0xF9, 0x48, 0x8B, 0x49,
    0x08, 0xE8, 0xEE, 0xA1, 0xDB, 0xFF, 0x85, 0xC0, 0x74, 0x08, 0x33, 0xC0,
    0x48, 0x83, 0xC4, 0x50, 0x5F, 0xC3, 0x33, 0xD2, 0x48, 0x8B, 0xCF, 0xE8,
    0x28, 0x27, 0x00, 0x00, 0x48, 0x85, 0xC0, 0x0F, 0x84, 0x63, 0x01, 0x00,
    0x00,
};

// 5909F4: the spawn chance, seed roll 0..254 against objects.txt Parm7.
constexpr std::uint8_t SpawnChanceWitness[]{
    0x48, 0x8B, 0x4F, 0x18, 0xBA, 0xFF, 0x00, 0x00, 0x00, 0xE8, 0xFE, 0x30,
    0xBC, 0xFF, 0x3B, 0x83, 0x44, 0x01, 0x00, 0x00, 0x0F, 0x87, 0xC6, 0x00,
    0x00, 0x00,
};

// 590A24: the monster region of the urn's level and the whole type loop,
// with the walk test window at +55h.
constexpr std::uint8_t RegionWalkWitness[]{
    0x48, 0x8B, 0x0F, 0x8B, 0xD0, 0x48, 0x8B, 0x89, 0xD0, 0x01, 0x00, 0x00,
    0xE8, 0x0B, 0x7D, 0xF7, 0xFF, 0x0F, 0xB6, 0x48, 0x10, 0x48, 0x8D, 0x70,
    0x14, 0x48, 0x6B, 0xD1, 0x34, 0x48, 0x8D, 0x58, 0x14, 0x48, 0x03, 0xF2,
    0x48, 0x3B, 0xDE, 0x0F, 0x84, 0x7E, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x07,
    0x0F, 0xBF, 0x13, 0x0F, 0xB6, 0x88, 0x06, 0x01, 0x00, 0x00, 0xE8, 0x4D,
    0x6B, 0xB0, 0xFF, 0x48, 0x85, 0xC0, 0x75, 0x11, 0x48, 0x8D, 0x4C, 0x24,
    0x60, 0xE8, 0x7E, 0x8B, 0xB6, 0xFF, 0x84, 0xC0, 0x74, 0x12, 0xCC, 0xEB,
    0x0F, 0x0F, 0xB6, 0x80, 0xF0, 0x00, 0x00, 0x00, 0x23, 0x05, 0x52, 0x8C,
    0x80, 0x01, 0x75, 0x0B, 0x48, 0x83, 0xC3, 0x34, 0x48, 0x3B, 0xDE, 0x75,
    0xC0, 0xEB, 0x3C,
};

// 590A93: the spawn path reads the id back from [rbx] and calls 499880.
constexpr std::uint8_t SpawnCallWitness[]{
    0x48, 0x8B, 0x4F, 0x08, 0x0F, 0xBF, 0x1B, 0xE8, 0xA1, 0xA9, 0xDB, 0xFF,
    0x48, 0x8B, 0x0F, 0x44, 0x8B, 0xCB, 0x66, 0x89, 0x6C, 0x24, 0x40, 0x45,
    0x33, 0xC0, 0xC7, 0x44, 0x24, 0x38, 0x01, 0x00, 0x00, 0x00, 0x48, 0x8B,
    0xD0, 0x66, 0x89, 0x6C, 0x24, 0x30, 0x66, 0x89, 0x6C, 0x24, 0x28, 0xC7,
    0x44, 0x24, 0x20, 0x01, 0x00, 0x00, 0x00, 0xE8, 0xB1, 0x8D, 0xF0, 0xFF,
};

constexpr std::size_t WalkTestOffsetInWitness = WalkTestRva - RegionWalkRva;
constexpr const std::uint8_t* WalkTestOriginal = RegionWalkWitness + WalkTestOffsetInWitness;

static_assert(WalkTestOffsetInWitness == 0x55);
static_assert(sizeof(RegionWalkWitness) == SpawnRva - RegionWalkRva);
static_assert(SpawnChanceRva + sizeof(SpawnChanceWitness) <= RegionWalkRva);
static_assert(OperateFunction68Rva + sizeof(OperateEntryWitness) <= SpawnChanceRva);
static_assert(RegionWalkWitness[WalkTestOffsetInWitness] == 0x0F
    && RegionWalkWitness[WalkTestOffsetInWitness + 1] == 0xB6
    && RegionWalkWitness[WalkTestOffsetInWitness + 7] == 0x23
    && RegionWalkWitness[WalkTestOffsetInWitness + 13] == 0x75
    && RegionWalkWitness[WalkTestOffsetInWitness + 14] == 0x0B);
static_assert(NextTypeRva == WalkTestRva + WalkTestSize);
static_assert(SpawnRva == NextTypeRva + 0x0B);

// ---------------------------------------------------------------------------
// [spawn_chance] anchors
// ---------------------------------------------------------------------------

constexpr std::uint64_t PopulateTableRva  = 0x239BC10;
constexpr std::size_t   PopulateSlotCount = 10;
constexpr std::size_t   PopulateHookSlot  = 9;
constexpr std::uint64_t PopulateFn9Rva    = 0x50E1F0;
constexpr std::uint64_t RoomSeedAccessRva = 0x2EFBF0;
constexpr std::uint64_t DifficultyReadRva = 0x48FF59;   // inside 0x48FF40

// Every slot of the table, as RVAs. Checked whole before the swap, so a
// different build is refused instead of patched.
constexpr std::array<std::uint64_t, PopulateSlotCount> ExpectedPopulateSlots{
    0x000000, 0x50D340, 0x50CF10, 0x50D1D0, 0x50C370,
    0x50C8C0, 0x50E070, 0x50DB60, 0x50C180, 0x50E1F0,
};
static_assert(ExpectedPopulateSlots[PopulateHookSlot] == PopulateFn9Rva);

// 50E1F0: push rbp/rsi/rdi/r12-r15 / sub rsp,60h / security cookie /
// movzx edi, r8b (density) / mov r14d, r9d (object id) / mov rbp, rdx (room) /
// mov rsi, rcx (game) / test rcx, rcx / jne.
constexpr std::uint8_t PopulateFn9Witness[]{
    0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
    0x48, 0x83, 0xEC, 0x60, 0x48, 0x8B, 0x05, 0xC1, 0xD0, 0x4B, 0x02, 0x48,
    0x33, 0xC4, 0x48, 0x89, 0x44, 0x24, 0x58, 0x41, 0x0F, 0xB6, 0xF8, 0x45,
    0x8B, 0xF1, 0x48, 0x8B, 0xEA, 0x48, 0x8B, 0xF1, 0x48, 0x85, 0xC9, 0x75,
};

// 2EFBF0: the room seed lives at room + 0A0h.
constexpr std::uint8_t RoomSeedWitness[]{
    0x48, 0x85, 0xC9, 0x74, 0x08, 0x48, 0x8D, 0x81, 0xA0, 0x00, 0x00, 0x00,
    0xC3, 0x33, 0xC0, 0xC3,
};
constexpr std::size_t RoomSeedOffset = 0xA0;

// 48FF59: movzx eax, byte [rdx+104h], the game's difficulty byte.
constexpr std::uint8_t DifficultyWitness[]{ 0x0F, 0xB6, 0x82, 0x04, 0x01, 0x00, 0x00 };
constexpr std::size_t  GameDifficultyOffset = 0x104;

constexpr std::uint64_t SeedMultiplier  = 0x6AC690C5;   // 1791398085
constexpr std::uint32_t DifficultyCount = 3;

using PopulateFn = void*(__fastcall*)(void* game, void* room, std::uint8_t density,
                                      std::uint32_t objectId, std::uint16_t chance);

// ---------------------------------------------------------------------------
// [boss_affixes] anchors
// ---------------------------------------------------------------------------

constexpr std::uint64_t OperateTableRva = 0x23A51C0;
constexpr std::size_t   UrnOperateSlot  = 68;
constexpr std::uint64_t BossPackRva     = 0x499880;
constexpr std::uint64_t AffixCallRva    = 0x499AAD;
constexpr std::uint64_t AffixRollRva    = 0x49E160;

constexpr std::size_t SpawnCallOffsetInWitness = 0x37;   // the E8 at 590ACA
static_assert(SpawnCallWitness[SpawnCallOffsetInWitness] == 0xE8);

// 499AAD: movzx r9d, byte [rbp-51h] (difficulty) / lea rax,[rbp-11h] /
// mov r8d,[rbp+5Fh] / inc r9d / mov [rsp+28h],rax / mov rdx,r12 /
// mov rcx,r15 / mov [rsp+20h],r9d / call 49E160.
constexpr std::uint8_t AffixCallWitness[]{
    0x44, 0x0F, 0xB6, 0x4D, 0xAF, 0x48, 0x8D, 0x45, 0xEF, 0x44, 0x8B, 0x45,
    0x5F, 0x41, 0xFF, 0xC1, 0x48, 0x89, 0x44, 0x24, 0x28, 0x49, 0x8B, 0xD4,
    0x49, 0x8B, 0xCF, 0x44, 0x89, 0x4C, 0x24, 0x20, 0xE8, 0x8E, 0x46, 0x00,
    0x00,
};
constexpr std::size_t AffixCallOffsetInWitness = 0x20;   // the E8 at 499ACD
static_assert(AffixCallWitness[AffixCallOffsetInWitness] == 0xE8);

// 49E160: push rbx/rbp/rsi/rdi/r12/r14/r15 / sub rsp,160h. Eighteen bytes,
// whole instructions, nothing relative; the next instruction is the
// security cookie load.
constexpr std::uint8_t AffixRollPrologue[]{
    0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48,
    0x81, 0xEC, 0x60, 0x01, 0x00, 0x00,
};

using OperateFn   = std::int64_t(__fastcall*)(void* operate);
using AffixRollFn = std::int64_t(__fastcall*)(void* game, void* monster, std::int32_t championChance,
                                              std::int32_t minimum, std::int32_t maximum, void* list);

constexpr std::int32_t MaximumAffixes = 9;   // the game's own cap per monster

// ---------------------------------------------------------------------------
// Hook region
// ---------------------------------------------------------------------------
//   One allocation within rel32 reach of the image, never freed:
//     +0000  code page      PAGE_EXECUTE_READ   the stub below
//     +1000  counters page  PAGE_READWRITE      written by the stub
//     +2000  bitmap, 8 KB   PAGE_READONLY       bit n set = monstats id n blocked
//   The stub reads and writes nothing but the game and this region, so it
//   never depends on this DLL being loaded.

constexpr std::size_t PageBytes       = 0x1000;
constexpr std::size_t CodeOffset      = 0x0000;
constexpr std::size_t CountersOffset  = 0x1000;
constexpr std::size_t BitmapOffset    = 0x2000;
constexpr std::size_t BitmapBytes     = 0x2000;   // one bit per 16-bit id
constexpr std::size_t HookRegionBytes = BitmapOffset + BitmapBytes;

struct UrnCounters {
    std::uint64_t skipped;         // +00  blocked walking types passed over
    std::uint64_t spawned;         // +08  types handed to the spawn
    std::uint32_t lastSkippedId;   // +10  id of the last blocked type passed over
};
static_assert(offsetof(UrnCounters, skipped) == 0x00);
static_assert(offsetof(UrnCounters, spawned) == 0x08);
static_assert(offsetof(UrnCounters, lastSkippedId) == 0x10);

// +00  0F B6 80 F0 00 00 00     movzx eax, byte ptr [rax+0F0h]   ; 590A79
// +07  48 B9 dq                 mov   rcx, gdwBitMasks[2]
// +11  23 01                    and   eax, dword ptr [rcx]        ; 590A80
// +13  75 15                    jne   +2Ah                        ; 590A86
// +15  FF 25 00 00 00 00        jmp   qword ptr [rip+0]           ; next type
// +1B  dq                       590A88
// +23  CC x 7                   padding, so the dispatch qword is 8-aligned
// +2A  FF 25 00 00 00 00        jmp   qword ptr [rip+0]           ; dispatch
// +30  dq                       +38h armed, +6Ah retargeted
// +38  0F B7 0B                 movzx ecx, word ptr [rbx]         ; the entry's id
// +3B  48 BA dq                 mov   rdx, bitmap
// +45  89 C8                    mov   eax, ecx
// +47  C1 E8 03                 shr   eax, 3
// +4A  0F B6 04 02              movzx eax, byte ptr [rdx+rax]
// +4E  41 89 C8                 mov   r8d, ecx
// +51  41 83 E0 07              and   r8d, 7
// +55  44 0F A3 C0              bt    eax, r8d
// +59  72 1D                    jc    +78h                        ; blocked
// +5B  48 BA dq                 mov   rdx, counters
// +65  F0 48 FF 42 08           lock inc qword ptr [rdx+8]        ; spawned
// +6A  FF 25 00 00 00 00        jmp   qword ptr [rip+0]           ; spawn
// +70  dq                       590A93
// +78  48 BA dq                 mov   rdx, counters
// +82  F0 48 FF 02              lock inc qword ptr [rdx]          ; skipped
// +86  89 4A 10                 mov   dword ptr [rdx+10h], ecx    ; last skipped id
// +89  EB 8A                    jmp   +15h                        ; next type
constexpr std::uint8_t FilterStub[]{
    0x0F, 0xB6, 0x80, 0xF0, 0x00, 0x00, 0x00, 0x48, 0xB9, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x23, 0x01, 0x75, 0x15, 0xFF, 0x25, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xCC,
    0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0xB7, 0x0B, 0x48,
    0xBA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x89, 0xC8, 0xC1,
    0xE8, 0x03, 0x0F, 0xB6, 0x04, 0x02, 0x41, 0x89, 0xC8, 0x41, 0x83, 0xE0,
    0x07, 0x44, 0x0F, 0xA3, 0xC0, 0x72, 0x1D, 0x48, 0xBA, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x48, 0xFF, 0x42, 0x08, 0xFF, 0x25,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x48, 0xBA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x48,
    0xFF, 0x02, 0x89, 0x4A, 0x10, 0xEB, 0x8A,
};
constexpr std::size_t StubMaskSlot        = 0x09;
constexpr std::size_t StubNextTypeSlot    = 0x1B;
constexpr std::size_t StubDispatchSlot    = 0x30;
constexpr std::size_t StubFilterOffset    = 0x38;
constexpr std::size_t StubBitmapSlot      = 0x3D;
constexpr std::size_t StubSpawnedSlot     = 0x5D;
constexpr std::size_t StubSpawnJumpOffset = 0x6A;
constexpr std::size_t StubSpawnSlot       = 0x70;
constexpr std::size_t StubSkippedSlot     = 0x7A;

constexpr auto JumpBefore(std::size_t slot) noexcept -> bool {
    return slot >= 6 && FilterStub[slot - 6] == 0xFF && FilterStub[slot - 5] == 0x25
        && FilterStub[slot - 4] == 0x00 && FilterStub[slot - 3] == 0x00
        && FilterStub[slot - 2] == 0x00 && FilterStub[slot - 1] == 0x00;
}

constexpr auto MovImm64Before(std::size_t slot, std::uint8_t opcode) noexcept -> bool {
    return slot >= 2 && FilterStub[slot - 2] == 0x48 && FilterStub[slot - 1] == opcode;
}

static_assert(sizeof(FilterStub) == 0x8B);
static_assert(sizeof(FilterStub) <= PageBytes);
static_assert(MovImm64Before(StubMaskSlot, 0xB9));
static_assert(JumpBefore(StubNextTypeSlot));
static_assert(JumpBefore(StubDispatchSlot));
static_assert((CodeOffset + StubDispatchSlot) % 8 == 0);
static_assert(StubFilterOffset == StubDispatchSlot + 8 && FilterStub[StubFilterOffset] == 0x0F
    && FilterStub[StubFilterOffset + 1] == 0xB7 && FilterStub[StubFilterOffset + 2] == 0x0B);
static_assert(MovImm64Before(StubBitmapSlot, 0xBA));
static_assert(MovImm64Before(StubSpawnedSlot, 0xBA));
static_assert(StubSpawnSlot == StubSpawnJumpOffset + 6 && JumpBefore(StubSpawnSlot));
static_assert(MovImm64Before(StubSkippedSlot, 0xBA));

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

constexpr std::int32_t MinimumMonsterId   = 0;
constexpr std::int32_t MaximumMonsterId   = 0x7FFF;
constexpr std::int32_t MaximumObjectId    = 0xFFFF;
constexpr std::size_t  MaximumObjectIds   = 64;
constexpr std::size_t  InitialConfigBytes = 16'384;
constexpr std::size_t  MaximumConfigBytes = 1'048'576;
constexpr std::size_t  RejectedSampleSize = 5;

static_assert(static_cast<std::size_t>(MaximumMonsterId) < BitmapBytes * 8);

constexpr char DefaultConfigToml[] =
    "# Evil Urn\n"
    "#\n"
    "# Three evil urn features in one plugin. Each section below switches one on\n"
    "# or off on its own. A feature whose checks against the game fail is left\n"
    "# out and reported in the log and by the console command; the other two\n"
    "# still install.\n"
    "#\n"
    "#   [spawn_chance]  per-room chance of a random urn, by difficulty\n"
    "#   [boss_affixes]  extra random affixes for the boss an urn spawns\n"
    "#   [blocklist]     monster types an urn must never spawn\n"
    "#\n"
    "# Server side only. In single player the game hosts its own server, so it\n"
    "# applies there too.\n"
    "#\n"
    "# Console command: evilurn (status, settings and counters of all three)\n"
    "#\n"
    "# Read when the plugin loads. Restart the game after editing.\n"
    "\n"
    "[spawn_chance]\n"
    "\n"
    "# HOW URNS ARE PLACED (txt side)\n"
    "#   levels.txt gives every zone with monsters object group 131 (objgroup.txt\n"
    "#   \"evilurn\": the urn alone, DENSITY 127, PROB 100) with ObjPrb 5. The game\n"
    "#   rolls 0-99 per room and fires a group when the roll is <= ObjPrb, so\n"
    "#   ObjPrb 5 is 6% per room. Siege 1, Barricade 1 and Barricade 2 have no free\n"
    "#   ObjGrp slot, so their urn sits in group 103 at PROB 6 instead, with that\n"
    "#   group firing every room: also 6%. The urn uses objects.txt PopulateFn 9,\n"
    "#   which places at most one urn per room.\n"
    "#\n"
    "# WHAT THIS DOES\n"
    "#   The txt files alone give txt_percent on every difficulty. When the game\n"
    "#   is about to place one of the objects below, the placement is kept only\n"
    "#   part of the time, rolled on the room's own seed:\n"
    "#     kept = difficulty percent / txt_percent\n"
    "#   so the chance per room becomes exactly the difficulty percent. With the\n"
    "#   defaults: Normal 2/6, Nightmare 4/6, Hell 6/6, so 2%, 4% and 6%.\n"
    "#\n"
    "#   This can only lower the txt chance, never raise it. To raise the Hell\n"
    "#   chance, raise the txt chance first (ObjPrb of group 131 in levels.txt is\n"
    "#   chance - 1, and the urn's PROB in group 103 is the chance itself), then\n"
    "#   set txt_percent to match.\n"
    "#\n"
    "#   Rooms roll when they are first populated, so a change shows up in newly\n"
    "#   created games.\n"
    "\n"
    "# false leaves the txt chance on every difficulty.\n"
    "enabled = true\n"
    "\n"
    "# The per-room chance the txt files give, 1 to 100. Must match the txt\n"
    "# setup above, or every difficulty is scaled from the wrong base.\n"
    "txt_percent = 6\n"
    "\n"
    "# Per-room chance on each difficulty, 0 to txt_percent.\n"
    "normal_percent = 2\n"
    "nightmare_percent = 4\n"
    "hell_percent = 6\n"
    "\n"
    "# objects.txt row ids this applies to, 0 to 65535. Only objects that use\n"
    "# PopulateFn 9 pass through here. 466 = IceCaveEvilUrn.\n"
    "object_ids = [466]\n"
    "\n"
    "[boss_affixes]\n"
    "\n"
    "# HOW AN URN BOSS GETS ITS AFFIXES\n"
    "#   When an urn spawns its pack, the game first rolls monumod.txt constants\n"
    "#   row 0 (20 in ESR, \"Champion chance to spawn instead of Unique\"): on a\n"
    "#   hit it is a champion pack with one champion affix. Otherwise the leader\n"
    "#   is a unique boss with (difficulty + 1) random affixes: 1 on Normal, 2 on\n"
    "#   Nightmare, 3 on Hell.\n"
    "#\n"
    "# WHAT THIS DOES\n"
    "#   Adds extra_affixes to that count for urn bosses only. The default of 1\n"
    "#   gives 2, 3 and 4. Champion packs and bosses from any other source are\n"
    "#   unchanged.\n"
    "#   - A monster holds at most 9 affixes; the game stops there.\n"
    "#   - Affixes are drawn without repeats. If nothing is left to draw, the\n"
    "#     boss ends up with fewer.\n"
    "#   - The minions copy the boss's transferable affixes (monumod.txt xfer)\n"
    "#     as usual, extra ones included.\n"
    "\n"
    "# false leaves urn bosses at the vanilla count.\n"
    "enabled = true\n"
    "\n"
    "# Extra random affixes for an urn boss, 0 to 8. 0 = vanilla.\n"
    "extra_affixes = 1\n"
    "\n"
    "[blocklist]\n"
    "\n"
    "# How an urn picks: when its objects.txt Parm4 roll succeeds (255 on the urn\n"
    "# row, so always), the urn walks the monster types the game rolled for its\n"
    "# level from levels.txt, and spawns the FIRST one that has a walk mode\n"
    "# (monstats2.txt mWL) as a boss pack.\n"
    "#\n"
    "# A blocked type is passed over exactly like a type that cannot walk: the urn\n"
    "# moves on to the next type, so a boss pack still spawns from whatever else\n"
    "# the level holds. When every type in the level is blocked or cannot walk,\n"
    "# the urn spawns nothing, the same as vanilla.\n"
    "#\n"
    "# Only urn spawns change. Blocked monsters still spawn everywhere else.\n"
    "\n"
    "# false installs nothing for the blocklist.\n"
    "enabled = true\n"
    "\n"
    "# The monsters an urn must never spawn, as monstats.txt hcIdx values,\n"
    "# 0 to 32767. An empty list, [], blocks nothing and installs no hook.\n"
    "# The list may be written on one line or spread over several, and may\n"
    "# carry # comments:\n"
    "#\n"
    "#   blocked_monster_ids = [1121, 1122]\n"
    "#\n"
    "#   blocked_monster_ids = [\n"
    "#     1121,   # corruptedzakarumarcher\n"
    "#     1122,\n"
    "#   ]\n"
    "#\n"
    "# The default is the list the ESR 2.4 patch blocked.\n"
    "blocked_monster_ids = [\n"
    "    1121,   # corruptedzakarumarcher\n"
    "    1122,   # corruptedzakarumpriest\n"
    "    1123,   # corruptedzakarumtrickster\n"
    "    1124,   # corruptedzakarumfanatic\n"
    "    1125,   # corruptedzakarumvenomancer\n"
    "]\n";

struct SpawnChanceSettings {
    bool                                       enabled    = true;
    std::uint32_t                              txtPercent = 6;
    std::array<std::uint32_t, DifficultyCount> percent{ 2, 4, 6 };
    std::vector<std::uint32_t>                 objectIds{ 466 };
};

struct BossAffixSettings {
    bool          enabled      = true;
    std::uint32_t extraAffixes = 1;
};

struct BlocklistSettings {
    bool enabled = true;
};

struct ListReport {
    bool         found        = false;
    bool         malformed    = false;
    bool         unterminated = false;
    std::int32_t rejected     = 0;
    std::string  rejectedSample;
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

enum class FeatureState : std::uint8_t {
    NotAttempted,
    DisabledByConfig,
    NothingToDo,
    ConfigError,
    UnsupportedBuild,
    InstallFailed,
    Installed,
};

constexpr std::array<const char*, DifficultyCount> DifficultyNames{ "Normal", "Nightmare", "Hell" };

bool ConfigFileWasRead{};

// [spawn_chance]
SpawnChanceSettings                                     SpawnConfig{};
std::atomic<FeatureState>                               SpawnState{ FeatureState::NotAttempted };
void**                                                  PopulateSlot{};
std::atomic<PopulateFn>                                 OriginalPopulate{};
std::array<std::atomic<std::uint64_t>, DifficultyCount> UrnsKept{};
std::array<std::atomic<std::uint64_t>, DifficultyCount> UrnsDropped{};

// [boss_affixes]
BossAffixSettings          AffixConfig{};
std::atomic<FeatureState>  AffixState{ FeatureState::NotAttempted };
void**                     OperateSlot{};
std::atomic<OperateFn>     OriginalOperate{};
std::atomic<AffixRollFn>   OriginalRoll{};
std::atomic<std::uint64_t> UrnsOpened{};
std::atomic<std::uint64_t> BossesRaised{};
thread_local std::int32_t  UrnDepth = 0;

// [blocklist]
BlocklistSettings                     BlockConfig{};
std::atomic<FeatureState>             BlockState{ FeatureState::NotAttempted };
std::array<std::uint8_t, BitmapBytes> BlockedBits{};
std::int32_t                          BlockedCount{};
void*                                 HookRegion{};
bool                                  SitePatched{};

auto IsBlocked(std::int32_t monsterId) noexcept -> bool {
    if (monsterId < MinimumMonsterId || monsterId > MaximumMonsterId) return false;
    const auto index = static_cast<std::size_t>(monsterId);
    return ((BlockedBits[index >> 3] >> (index & 7U)) & 1U) != 0;
}

void MarkBlocked(std::int32_t monsterId) noexcept {
    const auto index = static_cast<std::size_t>(monsterId);
    BlockedBits[index >> 3] = static_cast<std::uint8_t>(BlockedBits[index >> 3] | (1U << (index & 7U)));
}

void ClearBlocked() noexcept {
    BlockedBits.fill(0);
    BlockedCount = 0;
}

// ---------------------------------------------------------------------------
// Config parsing (small TOML subset, no external dependency)
// ---------------------------------------------------------------------------

auto Trim(std::string_view value) noexcept -> std::string_view {
    std::size_t first = 0;
    while (first < value.size()
            && (value[first] == ' ' || value[first] == '\t'
                || value[first] == '\r' || value[first] == '\n')) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first
            && (value[last - 1] == ' ' || value[last - 1] == '\t'
                || value[last - 1] == '\r' || value[last - 1] == '\n')) {
        --last;
    }
    return value.substr(first, last - first);
}

auto StripComment(std::string_view line) noexcept -> std::string_view {
    const std::size_t hash = line.find('#');
    return hash == std::string_view::npos ? line : line.substr(0, hash);
}

auto ParseBool(std::string_view value, bool& out) noexcept -> bool {
    if (value == "true") { out = true; return true; }
    if (value == "false") { out = false; return true; }
    return false;
}

// Decimal, optional leading '+', '_' separators allowed as in TOML.
auto ParseMonsterId(std::string_view text, std::int32_t& out) noexcept -> bool {
    std::size_t index = 0;
    if (!text.empty() && text[0] == '+') index = 1;
    std::int32_t value = 0;
    bool anyDigit = false;
    for (; index < text.size(); ++index) {
        const char character = text[index];
        if (character == '_') continue;
        if (character < '0' || character > '9') return false;
        anyDigit = true;
        value = value * 10 + (character - '0');
        if (value > MaximumMonsterId) return false;
    }
    if (!anyDigit) return false;
    out = value;
    return true;
}

// The text between the brackets, one or more lines already joined with
// commas. Empty elements (a trailing comma, a blank line) are skipped.
void ApplyIdList(std::string_view text, ListReport& report) noexcept {
    std::size_t cursor = 0;
    while (cursor <= text.size()) {
        const std::size_t comma = text.find(',', cursor);
        const std::size_t end = comma == std::string_view::npos ? text.size() : comma;
        const std::string_view element = Trim(text.substr(cursor, end - cursor));
        if (!element.empty()) {
            std::int32_t monsterId = 0;
            if (ParseMonsterId(element, monsterId)) {
                if (!IsBlocked(monsterId)) {
                    MarkBlocked(monsterId);
                    ++BlockedCount;
                }
            } else {
                ++report.rejected;
                if (report.rejected <= static_cast<std::int32_t>(RejectedSampleSize)) {
                    if (!report.rejectedSample.empty()) report.rejectedSample += ", ";
                    report.rejectedSample.append(element.substr(0, 24));
                }
            }
        }
        if (comma == std::string_view::npos) break;
        cursor = comma + 1;
    }
}

// One value from the TOML. A list keeps the text between its brackets, the
// lines of a multi-line list joined with commas.
struct ConfigValue {
    std::string text;
    bool        list         = false;
    bool        unterminated = false;
};
using ConfigMap = std::map<std::string, ConfigValue>;   // "section.key", the last assignment wins

auto ReadToml(std::string_view text) -> ConfigMap {
    ConfigMap   values;
    std::string section;
    std::string pendingKey;
    std::string listText;
    bool        capturing = false;

    std::size_t cursor = 0;
    while (cursor < text.size()) {
        std::size_t end = text.find('\n', cursor);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view line = Trim(StripComment(text.substr(cursor, end - cursor)));
        cursor = end + 1;

        if (capturing) {
            if (!line.empty() && line.front() == '[') {
                // A table header before the closing bracket: the list was
                // never closed. Record that and read the header normally.
                values[pendingKey] = ConfigValue{ listText, true, true };
                capturing = false;
            } else {
                const std::size_t close = line.find(']');
                listText.append(line.substr(0, close));
                listText.push_back(',');
                if (close != std::string_view::npos) {
                    values[pendingKey] = ConfigValue{ listText, true, false };
                    capturing = false;
                }
                continue;
            }
        }
        if (line.empty()) continue;

        if (line.front() == '[') {
            const std::size_t close = line.find(']');
            section.assign(Trim(line.substr(1, close == std::string_view::npos ? close : close - 1)));
            continue;
        }

        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) continue;
        std::string            key   = section + "." + std::string(Trim(line.substr(0, equals)));
        const std::string_view value = Trim(line.substr(equals + 1));

        if (!value.empty() && value.front() == '[') {
            const std::string_view inside = value.substr(1);
            const std::size_t      close  = inside.find(']');
            if (close != std::string_view::npos) {
                values[key] = ConfigValue{ std::string(inside.substr(0, close)), true, false };
            } else {
                pendingKey = std::move(key);
                listText.assign(inside);
                listText.push_back(',');
                capturing = true;
            }
        } else {
            values[key] = ConfigValue{ std::string(value), false, false };
        }
    }
    if (capturing) values[pendingKey] = ConfigValue{ listText, true, true };
    return values;
}

auto Find(const ConfigMap& values, const char* key) noexcept -> const ConfigValue* {
    const auto it = values.find(key);
    return it == values.end() ? nullptr : &it->second;
}

// `enabled` is forgiving, as it always was for the blocklist: a bad value
// keeps the default and only warns.
void ReadEnabled(const ConfigMap& values, const char* key, const char* section, bool& out) noexcept {
    const ConfigValue* value = Find(values, key);
    if (value == nullptr) return;
    if (value->list || !ParseBool(value->text, out)) {
        D2RL::LogWarnF(Context, "EvilUrn [%s]: enabled must be true or false; keeping %s.",
            section, out ? "true" : "false");
    }
}

auto ParseNumber(std::string_view text, std::uint32_t maximum, std::uint32_t& out) noexcept -> bool {
    std::size_t index = 0;
    if (!text.empty() && text[0] == '+') index = 1;
    std::uint64_t value = 0;
    bool anyDigit = false;
    for (; index < text.size(); ++index) {
        const char character = text[index];
        if (character == '_') continue;
        if (character < '0' || character > '9') return false;
        anyDigit = true;
        value = value * 10 + static_cast<std::uint64_t>(character - '0');
        if (value > maximum) return false;
    }
    if (!anyDigit) return false;
    out = static_cast<std::uint32_t>(value);
    return true;
}

auto ReadNumber(const ConfigMap& values, const char* key, std::uint32_t maximum, std::uint32_t& out) noexcept
        -> bool {
    const ConfigValue* value = Find(values, key);
    if (value == nullptr) return true;   // missing keeps the default
    return !value->list && ParseNumber(value->text, maximum, out);
}

auto ReadSpawnChance(const ConfigMap& values, SpawnChanceSettings& out) noexcept -> bool {
    ReadEnabled(values, "spawn_chance.enabled", "spawn_chance", out.enabled);
    if (!ReadNumber(values, "spawn_chance.txt_percent", 100, out.txtPercent) || out.txtPercent == 0) {
        Context->LogError("EvilUrn [spawn_chance]: txt_percent must be a whole number from 1 to 100. "
                          "Not installed.");
        return false;
    }
    constexpr std::array<const char*, DifficultyCount> keys{
        "spawn_chance.normal_percent", "spawn_chance.nightmare_percent", "spawn_chance.hell_percent" };
    for (std::size_t difficulty = 0; difficulty < DifficultyCount; ++difficulty) {
        if (!ReadNumber(values, keys[difficulty], 100, out.percent[difficulty])) {
            D2RL::LogErrorF(Context, "EvilUrn [spawn_chance]: %s must be a whole number from 0 to 100. "
                "Not installed.", keys[difficulty] + 13);
            return false;
        }
        if (out.percent[difficulty] > out.txtPercent) {
            D2RL::LogErrorF(Context, "EvilUrn [spawn_chance]: the %s percent is above txt_percent; this can "
                "only lower the txt chance. Not installed.", DifficultyNames[difficulty]);
            return false;
        }
    }

    const ConfigValue* ids = Find(values, "spawn_chance.object_ids");
    if (ids == nullptr) return true;
    if (!ids->list || ids->unterminated) {
        Context->LogError("EvilUrn [spawn_chance]: object_ids must be a list in square brackets, for "
                          "example [466]. Not installed.");
        return false;
    }
    out.objectIds.clear();
    const std::string_view text(ids->text);
    std::size_t cursor = 0;
    while (cursor <= text.size()) {
        const std::size_t comma = text.find(',', cursor);
        const std::size_t end = comma == std::string_view::npos ? text.size() : comma;
        const std::string_view element = Trim(text.substr(cursor, end - cursor));
        if (!element.empty()) {
            std::uint32_t id = 0;
            if (!ParseNumber(element, MaximumObjectId, id)) {
                Context->LogError("EvilUrn [spawn_chance]: object_ids holds a value that is not a whole "
                                  "number from 0 to 65535. Not installed.");
                return false;
            }
            bool duplicate = false;
            for (const std::uint32_t existing : out.objectIds) duplicate = duplicate || existing == id;
            if (!duplicate) {
                if (out.objectIds.size() >= MaximumObjectIds) {
                    Context->LogError("EvilUrn [spawn_chance]: object_ids holds more than 64 ids. "
                                      "Not installed.");
                    return false;
                }
                out.objectIds.push_back(id);
            }
        }
        if (comma == std::string_view::npos) break;
        cursor = comma + 1;
    }
    return true;
}

auto ReadBossAffixes(const ConfigMap& values, BossAffixSettings& out) noexcept -> bool {
    ReadEnabled(values, "boss_affixes.enabled", "boss_affixes", out.enabled);
    if (!ReadNumber(values, "boss_affixes.extra_affixes", MaximumAffixes - 1, out.extraAffixes)) {
        Context->LogError("EvilUrn [boss_affixes]: extra_affixes must be a whole number from 0 to 8. "
                          "Not installed.");
        return false;
    }
    return true;
}

auto ReadBlocklist(const ConfigMap& values, BlocklistSettings& out) noexcept -> bool {
    ReadEnabled(values, "blocklist.enabled", "blocklist", out.enabled);
    ClearBlocked();

    ListReport         report{};
    const ConfigValue* ids = Find(values, "blocklist.blocked_monster_ids");
    if (ids == nullptr) {
        Context->LogWarn("EvilUrn [blocklist]: blocked_monster_ids is missing, so nothing is blocked.");
        return true;
    }
    if (!ids->list) {
        Context->LogError("EvilUrn [blocklist]: blocked_monster_ids must be a list in square brackets, "
                          "for example [1121, 1122]. Nothing is blocked.");
        return false;
    }
    if (ids->unterminated) {
        Context->LogError("EvilUrn [blocklist]: blocked_monster_ids is missing its closing ']'. "
                          "Nothing is blocked.");
        return false;
    }
    report.found = true;
    ApplyIdList(ids->text, report);

    if (report.rejected > 0) {
        D2RL::LogWarnF(Context,
            "EvilUrn [blocklist]: %d entr%s in blocked_monster_ids ignored (%s%s). Every entry "
            "must be a whole number from %d to %d.",
            report.rejected, report.rejected == 1 ? "y was" : "ies were",
            report.rejectedSample.c_str(),
            report.rejected > static_cast<std::int32_t>(RejectedSampleSize) ? ", ..." : "",
            MinimumMonsterId, MaximumMonsterId);
    }
    return true;
}

auto ReadConfigText(std::string& out) noexcept -> bool {
    std::string buffer(InitialConfigBytes, '\0');
    std::uint32_t required = 0;
    if (!Context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size()), &required)) {
        if (required < buffer.size() || required >= MaximumConfigBytes) return false;
        buffer.assign(static_cast<std::size_t>(required) + 1, '\0');
        if (!Context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size()), &required)) {
            return false;
        }
    }
    buffer.resize(std::strlen(buffer.c_str()));
    out = std::move(buffer);
    return true;
}

struct ConfigOutcome {
    bool spawnUsable = false;
    bool affixUsable = false;
    bool blockUsable = false;
};

auto ReadConfiguration() noexcept -> ConfigOutcome {
    std::string text;
    if (!Context->EnsureConfig(DefaultConfigToml)) {
        Context->LogWarn("EvilUrn: the config file could not be created; using the embedded defaults.");
        text = DefaultConfigToml;
    } else if (!ReadConfigText(text)) {
        Context->LogWarn("EvilUrn: the config file could not be read; using the embedded defaults.");
        text = DefaultConfigToml;
    } else {
        ConfigFileWasRead = true;
    }

    const ConfigMap values = ReadToml(text);
    ConfigOutcome   outcome{};
    SpawnConfig = SpawnChanceSettings{};
    AffixConfig = BossAffixSettings{};
    BlockConfig = BlocklistSettings{};
    outcome.spawnUsable = ReadSpawnChance(values, SpawnConfig);
    outcome.affixUsable = ReadBossAffixes(values, AffixConfig);
    outcome.blockUsable = ReadBlocklist(values, BlockConfig);
    return outcome;
}

// Appends " id id id" in ascending order, stopping before `limit` characters.
void AppendBlockedIds(std::string& out, std::size_t limit) noexcept {
    char number[16]{};
    for (std::int32_t monsterId = MinimumMonsterId; monsterId <= MaximumMonsterId; ++monsterId) {
        if (!IsBlocked(monsterId)) continue;
        const int length = std::snprintf(number, sizeof(number), " %d", monsterId);
        if (length <= 0) continue;
        if (out.size() + static_cast<std::size_t>(length) + 4 > limit) {
            out += " ...";
            return;
        }
        out.append(number, static_cast<std::size_t>(length));
    }
}

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

auto Verify(std::uint64_t rva, const std::uint8_t* expected, std::size_t size, const char* section,
        const char* label) noexcept -> bool {
    if (Context->CheckExpectedBytes(rva, expected, static_cast<std::uint32_t>(size))) return true;
    D2RL::LogErrorF(Context,
        "EvilUrn [%s]: NOT installed. %s at 0x%llX does not match the verified D2R image, or another "
        "plugin already owns it. Nothing was patched.",
        section, label, static_cast<unsigned long long>(rva));
    return false;
}

// Where the E8 call at rva goes, as an RVA.
auto CallTarget(std::uint64_t rva) noexcept -> std::uint64_t {
    std::int32_t displacement = 0;
    std::memcpy(&displacement, reinterpret_cast<const void*>(Base + rva + 1), sizeof(displacement));
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(rva + 5) + displacement);
}

auto SlotAddress(std::uint64_t tableRva, std::size_t slot) noexcept -> void** {
    return reinterpret_cast<void**>(Base + tableRva) + slot;
}

auto WriteSlot(void** slot, void* value) noexcept -> bool {
    DWORD previous = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &previous)) return false;
    *slot = value;
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), previous, &ignored);
    return *slot == value;
}

// ---------------------------------------------------------------------------
// [spawn_chance]: the hook and its install
// ---------------------------------------------------------------------------

auto IsTrackedObject(std::uint32_t objectId) noexcept -> bool {
    for (const std::uint32_t id : SpawnConfig.objectIds) {
        if (id == objectId) return true;
    }
    return false;
}

// The game's own seed step, on the room's own seed.
auto NextRoomRandom(void* room) noexcept -> std::uint32_t {
    auto* seed = reinterpret_cast<std::uint32_t*>(static_cast<std::byte*>(room) + RoomSeedOffset);
    const std::uint64_t next = static_cast<std::uint64_t>(seed[0]) * SeedMultiplier + seed[1];
    seed[0] = static_cast<std::uint32_t>(next);
    seed[1] = static_cast<std::uint32_t>(next >> 32);
    return static_cast<std::uint32_t>(next);
}

auto __fastcall PopulateHook(void* game, void* room, std::uint8_t density, std::uint32_t objectId,
                             std::uint16_t chance) noexcept -> void* {
    const PopulateFn original = OriginalPopulate.load(std::memory_order_acquire);
    if (original == nullptr) return nullptr;

    if (game != nullptr && room != nullptr && IsTrackedObject(objectId)) {
        std::uint32_t difficulty = *(static_cast<const std::uint8_t*>(game) + GameDifficultyOffset);
        if (difficulty >= DifficultyCount) difficulty = DifficultyCount - 1;
        const std::uint32_t keep = SpawnConfig.percent[difficulty];
        if (keep < SpawnConfig.txtPercent && NextRoomRandom(room) % SpawnConfig.txtPercent >= keep) {
            UrnsDropped[difficulty].fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        UrnsKept[difficulty].fetch_add(1, std::memory_order_relaxed);
    }
    return original(game, room, density, objectId, chance);
}

void InstallSpawnChance(bool configUsable) noexcept {
    constexpr const char* Section = "spawn_chance";
    if (!SpawnConfig.enabled) {
        SpawnState.store(FeatureState::DisabledByConfig, std::memory_order_release);
        Context->LogInfo("EvilUrn [spawn_chance]: not installed, turned off in the config file.");
        return;
    }
    if (!configUsable) {
        SpawnState.store(FeatureState::ConfigError, std::memory_order_release);
        return;
    }
    if (SpawnConfig.objectIds.empty()) {
        SpawnState.store(FeatureState::NothingToDo, std::memory_order_release);
        Context->LogInfo("EvilUrn [spawn_chance]: not installed, object_ids is empty.");
        return;
    }

    for (std::size_t index = 0; index < PopulateSlotCount; ++index) {
        void* const          live     = *SlotAddress(PopulateTableRva, index);
        const std::uintptr_t expected = ExpectedPopulateSlots[index] == 0 ? 0 : Base + ExpectedPopulateSlots[index];
        if (reinterpret_cast<std::uintptr_t>(live) != expected) {
            SpawnState.store(FeatureState::UnsupportedBuild, std::memory_order_release);
            D2RL::LogErrorF(Context,
                "EvilUrn [spawn_chance]: NOT installed. PopulateFn slot %u holds %p, expected %p: another "
                "game build, or another plugin owns the slot.",
                static_cast<unsigned>(index), live, reinterpret_cast<void*>(expected));
            return;
        }
    }
    if (!Verify(PopulateFn9Rva, PopulateFn9Witness, sizeof(PopulateFn9Witness), Section, "PopulateFn 9")
            || !Verify(RoomSeedAccessRva, RoomSeedWitness, sizeof(RoomSeedWitness), Section,
                   "The room seed accessor")
            || !Verify(DifficultyReadRva, DifficultyWitness, sizeof(DifficultyWitness), Section,
                   "The game's difficulty read")) {
        SpawnState.store(FeatureState::UnsupportedBuild, std::memory_order_release);
        return;
    }

    PopulateSlot = SlotAddress(PopulateTableRva, PopulateHookSlot);
    OriginalPopulate.store(reinterpret_cast<PopulateFn>(*PopulateSlot), std::memory_order_release);
    if (!WriteSlot(PopulateSlot, reinterpret_cast<void*>(&PopulateHook))) {
        OriginalPopulate.store(nullptr);
        PopulateSlot = nullptr;
        SpawnState.store(FeatureState::InstallFailed, std::memory_order_release);
        Context->LogError("EvilUrn [spawn_chance]: NOT installed. The PopulateFn table could not be written.");
        return;
    }
    SpawnState.store(FeatureState::Installed, std::memory_order_release);
    D2RL::LogInfoF(Context, "EvilUrn [spawn_chance]: installed. txt %u%%, Normal %u%%, Nightmare %u%%, "
        "Hell %u%% per room.", SpawnConfig.txtPercent, SpawnConfig.percent[0], SpawnConfig.percent[1],
        SpawnConfig.percent[2]);
}

// ---------------------------------------------------------------------------
// [boss_affixes]: the hooks and their install
// ---------------------------------------------------------------------------

auto __fastcall UrnOperateHook(void* operate) noexcept -> std::int64_t {
    const OperateFn original = OriginalOperate.load(std::memory_order_acquire);
    if (original == nullptr) return 0;
    UrnsOpened.fetch_add(1, std::memory_order_relaxed);
    ++UrnDepth;
    const std::int64_t result = original(operate);
    --UrnDepth;
    return result;
}

auto __fastcall AffixRollHook(void* game, void* monster, std::int32_t championChance, std::int32_t minimum,
                              std::int32_t maximum, void* list) noexcept -> std::int64_t {
    const AffixRollFn original = OriginalRoll.load(std::memory_order_acquire);
    if (original == nullptr) return 0;
    if (UrnDepth > 0 && AffixConfig.extraAffixes > 0) {
        const auto extra = static_cast<std::int32_t>(AffixConfig.extraAffixes);
        minimum += extra;
        maximum += extra;
        BossesRaised.fetch_add(1, std::memory_order_relaxed);
    }
    return original(game, monster, championChance, minimum, maximum, list);
}

void InstallBossAffixes(bool configUsable) noexcept {
    constexpr const char* Section = "boss_affixes";
    if (!AffixConfig.enabled) {
        AffixState.store(FeatureState::DisabledByConfig, std::memory_order_release);
        Context->LogInfo("EvilUrn [boss_affixes]: not installed, turned off in the config file.");
        return;
    }
    if (!configUsable) {
        AffixState.store(FeatureState::ConfigError, std::memory_order_release);
        return;
    }
    if (AffixConfig.extraAffixes == 0) {
        AffixState.store(FeatureState::NothingToDo, std::memory_order_release);
        Context->LogInfo("EvilUrn [boss_affixes]: not installed, extra_affixes is 0.");
        return;
    }

    void* const slotValue = *SlotAddress(OperateTableRva, UrnOperateSlot);
    if (slotValue != reinterpret_cast<void*>(Base + OperateFunction68Rva)) {
        AffixState.store(FeatureState::UnsupportedBuild, std::memory_order_release);
        D2RL::LogErrorF(Context,
            "EvilUrn [boss_affixes]: NOT installed. OperateFn slot 68 holds %p, expected %p: another game "
            "build, or another plugin owns the slot.",
            slotValue, reinterpret_cast<void*>(Base + OperateFunction68Rva));
        return;
    }
    if (!Verify(OperateFunction68Rva, OperateEntryWitness, sizeof(OperateEntryWitness), Section,
               "The evil urn (OperateFn 68) entry")
            || !Verify(SpawnRva, SpawnCallWitness, sizeof(SpawnCallWitness), Section,
                   "The urn's boss pack spawn")
            || !Verify(AffixCallRva, AffixCallWitness, sizeof(AffixCallWitness), Section,
                   "The boss pack's affix roll call")) {
        AffixState.store(FeatureState::UnsupportedBuild, std::memory_order_release);
        return;
    }
    if (CallTarget(SpawnRva + SpawnCallOffsetInWitness) != BossPackRva
            || CallTarget(AffixCallRva + AffixCallOffsetInWitness) != AffixRollRva) {
        AffixState.store(FeatureState::UnsupportedBuild, std::memory_order_release);
        Context->LogError("EvilUrn [boss_affixes]: NOT installed. The urn's spawn call or the boss pack's "
                          "affix roll call goes somewhere else in this build.");
        return;
    }

    void* original = nullptr;
    if (!Context->InstallInlineHook(AffixRollRva, AffixRollPrologue, sizeof(AffixRollPrologue),
            reinterpret_cast<void*>(&AffixRollHook), &original) || original == nullptr) {
        AffixState.store(FeatureState::InstallFailed, std::memory_order_release);
        Context->LogError("EvilUrn [boss_affixes]: NOT installed. The hook on the affix roll (0x49E160) "
                          "was refused.");
        return;
    }
    OriginalRoll.store(reinterpret_cast<AffixRollFn>(original), std::memory_order_release);

    OperateSlot = SlotAddress(OperateTableRva, UrnOperateSlot);
    OriginalOperate.store(reinterpret_cast<OperateFn>(*OperateSlot), std::memory_order_release);
    if (!WriteSlot(OperateSlot, reinterpret_cast<void*>(&UrnOperateHook))) {
        // The roll hook stays in place but never sees an urn call, so it
        // passes everything through unchanged.
        OriginalOperate.store(nullptr);
        OperateSlot = nullptr;
        AffixState.store(FeatureState::InstallFailed, std::memory_order_release);
        Context->LogError("EvilUrn [boss_affixes]: NOT installed. The OperateFn table could not be written.");
        return;
    }
    AffixState.store(FeatureState::Installed, std::memory_order_release);
    D2RL::LogInfoF(Context, "EvilUrn [boss_affixes]: installed. Urn bosses get %u extra random affixes "
        "(Normal %u, Nightmare %u, Hell %u).", AffixConfig.extraAffixes, 1 + AffixConfig.extraAffixes,
        2 + AffixConfig.extraAffixes, 3 + AffixConfig.extraAffixes);
}

// ---------------------------------------------------------------------------
// Hook region and site
// ---------------------------------------------------------------------------

auto CanEncodeRel32(std::uintptr_t from, std::uintptr_t to) noexcept -> bool {
    const std::int64_t delta =
        static_cast<std::int64_t>(to) - static_cast<std::int64_t>(from) - JumpSize;
    return delta >= INT32_MIN && delta <= INT32_MAX;
}

auto AllocateNear(std::uintptr_t hint, std::size_t size) noexcept -> void* {
    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    const auto granularity = static_cast<std::uintptr_t>(systemInfo.dwAllocationGranularity);
    const auto aligned = hint & ~(granularity - 1U);
    for (std::uintptr_t delta = granularity; delta < 0x7000'0000ULL; delta += granularity) {
        const auto candidate = aligned + delta;
        if (!CanEncodeRel32(hint, candidate + size)) break;
        if (auto* memory = VirtualAlloc(reinterpret_cast<void*>(candidate), size,
                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) {
            return memory;
        }
    }
    return nullptr;
}

auto RegionAddress(std::size_t offset) noexcept -> std::uintptr_t {
    return reinterpret_cast<std::uintptr_t>(HookRegion) + offset;
}

void WriteQword(std::uint8_t* at, std::uint64_t value) noexcept {
    std::memcpy(at, &value, sizeof(value));
}

// E9 rel32 to the stub, the rest of the window filled with NOP.
void EncodeSiteJump(std::uint8_t* out) noexcept {
    const std::uintptr_t site = Base + WalkTestRva;
    const std::uintptr_t target = RegionAddress(CodeOffset);
    out[0] = 0xE9;
    const auto displacement = static_cast<std::int32_t>(
        static_cast<std::int64_t>(target) - static_cast<std::int64_t>(site + JumpSize));
    std::memcpy(out + 1, &displacement, sizeof(displacement));
    std::memset(out + JumpSize, 0x90, WalkTestSize - JumpSize);
}

auto ReadCounters() noexcept -> UrnCounters {
    UrnCounters copy{};
    if (HookRegion == nullptr) return copy;
    const auto* live = reinterpret_cast<const volatile UrnCounters*>(RegionAddress(CountersOffset));
    copy.skipped = live->skipped;
    copy.spawned = live->spawned;
    copy.lastSkippedId = live->lastSkippedId;
    return copy;
}

auto BuildHookRegion() noexcept -> bool {
    HookRegion = AllocateNear(Base + WalkTestRva, HookRegionBytes);
    if (HookRegion == nullptr) {
        Context->LogError("EvilUrn [blocklist]: no hook region was available within rel32 reach "
                          "of the urn.");
        return false;
    }

    auto* region = static_cast<std::uint8_t*>(HookRegion);
    std::memset(region + CodeOffset, 0xCC, PageBytes);
    std::memset(region + CountersOffset, 0, PageBytes);
    std::memcpy(region + BitmapOffset, BlockedBits.data(), BitmapBytes);

    std::uint8_t* code = region + CodeOffset;
    std::memcpy(code, FilterStub, sizeof(FilterStub));
    WriteQword(code + StubMaskSlot, Base + WalkModeMaskRva);
    WriteQword(code + StubNextTypeSlot, Base + NextTypeRva);
    WriteQword(code + StubDispatchSlot, RegionAddress(CodeOffset + StubFilterOffset));
    WriteQword(code + StubBitmapSlot, RegionAddress(BitmapOffset));
    WriteQword(code + StubSpawnedSlot, RegionAddress(CountersOffset));
    WriteQword(code + StubSpawnSlot, Base + SpawnRva);
    WriteQword(code + StubSkippedSlot, RegionAddress(CountersOffset));

    DWORD previous = 0;
    if (!VirtualProtect(code, PageBytes, PAGE_EXECUTE_READ, &previous)
            || !VirtualProtect(region + BitmapOffset, BitmapBytes, PAGE_READONLY, &previous)) {
        Context->LogError("EvilUrn [blocklist]: hook region protection could not be finalized.");
        VirtualFree(HookRegion, 0, MEM_RELEASE);
        HookRegion = nullptr;
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), code, PageBytes);

    if (!CanEncodeRel32(Base + WalkTestRva, RegionAddress(CodeOffset))) {
        Context->LogError("EvilUrn [blocklist]: hook region displacement validation failed.");
        VirtualFree(HookRegion, 0, MEM_RELEASE);
        HookRegion = nullptr;
        return false;
    }
    return true;
}

// Points the dispatch at the spawn jump, so the stub is the original test
// again and never reads the bitmap. The qword is 8-aligned, so the store is
// a single atomic write even while a game thread runs the stub.
auto RetargetStubToVanilla() noexcept -> bool {
    if (HookRegion == nullptr) return true;
    auto* code = static_cast<std::uint8_t*>(HookRegion) + CodeOffset;
    DWORD previous = 0;
    if (!VirtualProtect(code, PageBytes, PAGE_EXECUTE_READWRITE, &previous)) return false;
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(code + StubDispatchSlot),
        static_cast<LONG64>(RegionAddress(CodeOffset + StubSpawnJumpOffset)));
    DWORD ignored = 0;
    VirtualProtect(code, PageBytes, previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), code, PageBytes);
    return true;
}

auto RestoreSite() noexcept -> bool {
    if (!SitePatched) return true;
    std::uint8_t current[WalkTestSize]{};
    EncodeSiteJump(current);
    if (!Context->PatchBytes(WalkTestRva, current, WalkTestSize, WalkTestOriginal, WalkTestSize)) {
        return false;
    }
    SitePatched = false;
    return true;
}

// ---------------------------------------------------------------------------
// [blocklist]: install
// ---------------------------------------------------------------------------

auto VerifyNativeContract() noexcept -> bool {
    constexpr const char* Section = "blocklist";
    return Verify(OperateFunction68Rva, OperateEntryWitness, sizeof(OperateEntryWitness), Section,
               "The evil urn (OperateFn 68) entry")
        && Verify(SpawnChanceRva, SpawnChanceWitness, sizeof(SpawnChanceWitness), Section,
               "The urn's Parm4 spawn roll")
        && Verify(RegionWalkRva, RegionWalkWitness, sizeof(RegionWalkWitness), Section,
               "The urn's monster type loop")
        && Verify(SpawnRva, SpawnCallWitness, sizeof(SpawnCallWitness), Section,
               "The urn's boss pack spawn");
}

void InstallBlocklist(bool configUsable) noexcept {
    if (!BlockConfig.enabled) {
        BlockState.store(FeatureState::DisabledByConfig, std::memory_order_release);
        Context->LogInfo("EvilUrn [blocklist]: not installed, turned off in the config file.");
        return;
    }
    if (!configUsable) {
        BlockState.store(FeatureState::ConfigError, std::memory_order_release);
        return;
    }
    if (BlockedCount == 0) {
        BlockState.store(FeatureState::NothingToDo, std::memory_order_release);
        Context->LogInfo("EvilUrn [blocklist]: not installed, blocked_monster_ids is empty.");
        return;
    }
    if (!VerifyNativeContract()) {
        BlockState.store(FeatureState::UnsupportedBuild, std::memory_order_release);
        return;
    }
    if (!BuildHookRegion()) {
        BlockState.store(FeatureState::InstallFailed, std::memory_order_release);
        return;
    }

    std::uint8_t jump[WalkTestSize]{};
    EncodeSiteJump(jump);
    if (!Context->PatchBytes(WalkTestRva, WalkTestOriginal, WalkTestSize, jump, WalkTestSize)) {
        // Nothing points at the region, and nothing in it points at this DLL.
        BlockState.store(FeatureState::InstallFailed, std::memory_order_release);
        Context->LogError("EvilUrn [blocklist]: NOT installed. The urn's walk test at 0x590A79 "
                          "could not be redirected.");
        return;
    }
    SitePatched = true;
    BlockState.store(FeatureState::Installed, std::memory_order_release);

    std::string ids;
    AppendBlockedIds(ids, 900);
    D2RL::LogInfoF(Context, "EvilUrn [blocklist]: installed. %d monster id%s blocked:%s",
        BlockedCount, BlockedCount == 1 ? "" : "s", ids.c_str());
}

// ---------------------------------------------------------------------------
// Console command
// ---------------------------------------------------------------------------

auto StateText(FeatureState state, const char* nothingToDo) noexcept -> const char* {
    switch (state) {
    case FeatureState::Installed:        return "active";
    case FeatureState::DisabledByConfig: return "off in the config file";
    case FeatureState::NothingToDo:      return nothingToDo;
    case FeatureState::ConfigError:      return "NOT ACTIVE, its config could not be read, see the log";
    case FeatureState::UnsupportedBuild: return "NOT ACTIVE, unrecognised game build or already hooked, see the log";
    case FeatureState::InstallFailed:    return "NOT ACTIVE, the hook failed, see the log";
    case FeatureState::NotAttempted:
    default:                             return "NOT ACTIVE, never attempted";
    }
}

auto __cdecl StatusCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command,
        void*) noexcept -> D2RL::ConsoleCommandResult {
    if (command == nullptr || command->plugin == nullptr) return D2RL::ConsoleCommandResult::Failed;
    const D2RL::PluginContext* plugin = command->plugin;

    char line[512]{};
    std::snprintf(line, sizeof(line), "Evil Urn 1.0.0 | config %s",
        ConfigFileWasRead ? "loaded" : "NOT READ (embedded defaults)");
    plugin->WriteConsoleMessage(line);

    std::string objects;
    for (const std::uint32_t id : SpawnConfig.objectIds) {
        objects += objects.empty() ? "" : " ";
        objects += std::to_string(id);
    }
    std::snprintf(line, sizeof(line),
        "Spawn chance: %s | txt %u%% | Normal %u%% Nightmare %u%% Hell %u%% | objects %s | kept/dropped: "
        "Normal %llu/%llu, Nightmare %llu/%llu, Hell %llu/%llu",
        StateText(SpawnState.load(std::memory_order_acquire), "off, object_ids is empty"),
        SpawnConfig.txtPercent, SpawnConfig.percent[0], SpawnConfig.percent[1], SpawnConfig.percent[2],
        objects.empty() ? "none" : objects.c_str(),
        static_cast<unsigned long long>(UrnsKept[0].load()), static_cast<unsigned long long>(UrnsDropped[0].load()),
        static_cast<unsigned long long>(UrnsKept[1].load()), static_cast<unsigned long long>(UrnsDropped[1].load()),
        static_cast<unsigned long long>(UrnsKept[2].load()), static_cast<unsigned long long>(UrnsDropped[2].load()));
    plugin->WriteConsoleMessage(line);

    const std::uint32_t extra = AffixConfig.extraAffixes;
    std::snprintf(line, sizeof(line),
        "Boss affixes: %s | +%u: Normal %u, Nightmare %u, Hell %u | urns opened %llu, bosses given extra "
        "affixes %llu",
        StateText(AffixState.load(std::memory_order_acquire), "off, extra_affixes is 0"),
        extra, 1 + extra, 2 + extra, 3 + extra,
        static_cast<unsigned long long>(UrnsOpened.load()), static_cast<unsigned long long>(BossesRaised.load()));
    plugin->WriteConsoleMessage(line);

    const UrnCounters counters = ReadCounters();
    if (counters.skipped > 0) {
        std::snprintf(line, sizeof(line),
            "Blocklist: %s | %d id%s blocked | blocked types skipped %llu, last %u | packs spawned %llu",
            StateText(BlockState.load(std::memory_order_acquire), "off, blocked_monster_ids is empty"),
            BlockedCount, BlockedCount == 1 ? "" : "s",
            static_cast<unsigned long long>(counters.skipped), counters.lastSkippedId,
            static_cast<unsigned long long>(counters.spawned));
    } else {
        std::snprintf(line, sizeof(line),
            "Blocklist: %s | %d id%s blocked | blocked types skipped 0 | packs spawned %llu",
            StateText(BlockState.load(std::memory_order_acquire), "off, blocked_monster_ids is empty"),
            BlockedCount, BlockedCount == 1 ? "" : "s",
            static_cast<unsigned long long>(counters.spawned));
    }
    plugin->WriteConsoleMessage(line);

    std::string ids = "Blocked ids:";
    AppendBlockedIds(ids, 380);
    plugin->WriteConsoleMessage(BlockedCount == 0 ? "Blocked ids: none" : ids.c_str());
    return D2RL::ConsoleCommandResult::Handled;
}

constexpr D2RL::PluginInfo Info{
    .infoSize    = D2RL::PluginInfoSize,
    .abiVersion  = D2RL_PLUGIN_ABI_VERSION,
    .id          = "celestialrayone.evil-urn",
    .name        = "Evil Urn",
    .version     = "1.0.0",
    .author      = "CelestialRayOne",
    .description = "Evil urn spawn chance by difficulty, extra affixes for urn bosses and a monster "
                   "blocklist for urn spawns.",
    // Server: object populate and operate functions only run on the server.
    .flags       = D2RL::PluginFlags::Server | D2RL::PluginFlags::NativeHooks,
};

static_assert(D2RL::HasValidPluginRole(Info.flags), "Exactly one execution role must be set.");
static_assert(D2RL::HasOnlyKnownPluginFlags(Info.flags), "Unknown plugin flag set.");

}  // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
    return &Info;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
    if (!D2RL::HasContext(context)) return false;
    Context = context;
    Base = context->exeBase;

    // Registered first, and the load always succeeds: returning false would
    // unload the DLL and take the command with it, leaving no in-game way to
    // see why a feature is off.
    if (!Context->RegisterConsoleCommand("evilurn", &StatusCommand,
            "Reports the status, settings and counters of the three Evil Urn features.")) {
        Context->LogWarn("EvilUrn: the status console command was refused.");
    }

    const ConfigOutcome outcome = ReadConfiguration();
    InstallBlocklist(outcome.blockUsable);
    InstallBossAffixes(outcome.affixUsable);
    InstallSpawnChance(outcome.spawnUsable);
    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    if (Context == nullptr) return;

    if (OperateSlot != nullptr) {
        const OperateFn original = OriginalOperate.load();
        if (original != nullptr && *OperateSlot == reinterpret_cast<void*>(&UrnOperateHook)) {
            WriteSlot(OperateSlot, reinterpret_cast<void*>(original));
        }
        OperateSlot = nullptr;
    }
    OriginalOperate.store(nullptr);
    OriginalRoll.store(nullptr);

    if (PopulateSlot != nullptr) {
        const PopulateFn original = OriginalPopulate.load();
        if (original != nullptr && *PopulateSlot == reinterpret_cast<void*>(&PopulateHook)) {
            WriteSlot(PopulateSlot, reinterpret_cast<void*>(original));
        }
        PopulateSlot = nullptr;
    }
    OriginalPopulate.store(nullptr);

    if (SitePatched) {
        const bool retargeted = RetargetStubToVanilla();
        if (!RestoreSite()) {
            Context->LogError(retargeted
                ? "EvilUrn [blocklist]: the urn's walk test could not be restored; it keeps running "
                  "the original test through the hook region, without the blocklist."
                : "EvilUrn [blocklist]: the urn's walk test could not be restored and the stub could "
                  "not be retargeted; the blocklist stays in effect.");
        }
        // The region is deliberately kept: a thread may be inside the stub right
        // now, and a site that could not be restored still needs it.
    }
}

}  // namespace CelestialRayOne::EvilUrn
