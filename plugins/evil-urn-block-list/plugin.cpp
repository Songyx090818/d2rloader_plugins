// Evil Urn Blocklist
//
// Keeps a configurable list of monster types (monstats.txt hcIdx) from being
// spawned by evil urns. When the urn's pick lands on a blocked type it moves
// on to the next type of the level, exactly as it does for a type that cannot
// walk, so a boss pack still spawns from whatever else the level holds.
//
// Port of the ESR D2R 2.4 memory patches (hook 3BF80C redirecting the urn's
// call to sub_1400A4440 into the wrapper cave 36AE90, with a zero-terminated
// id table at 36AEC4) to D2R 3.3 on D2RLoader 1.3.0. Everything below was
// read out of the live D2RLoader.exe image and disassembled before being
// relied on.
//
// ---------------------------------------------------------------------------
// The urn: objects.txt OperateFn 68 at 0x590960 (2.4 sub_1403BF6D0)
// ---------------------------------------------------------------------------
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
//     590A02  3B 83 44 01 00 00     cmp  eax, [rbx+144h]        ; objects.txt Parm7
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
//   So the urn spawns the FIRST type in the level's list that has a walk
//   mode, as in 2.4. 0x975B0 is the MonStats2-from-monstats-id getter:
//   monstats stride 1FCh, MonStats2 index word at +4Ch, MonStats2 stride
//   128h, null for an id out of range. 0x499880 is the same boss-pack
//   spawner the preset-unique path in 0x50A050 calls.
//
// ---------------------------------------------------------------------------
// Why the 2.4 cave cannot be copied
// ---------------------------------------------------------------------------
//   In 2.4 the walk test was a call, sub_1400A4440(id, 2), so the cave could
//   wrap it and return 0 for a blocked id. In 3.3 that call no longer exists:
//   the compiler inlined it into the three instructions at 590A79. There is
//   nothing to wrap, so this plugin takes over the test itself.
//
// ---------------------------------------------------------------------------
// The hook
// ---------------------------------------------------------------------------
//   The 15 bytes at 590A79 (movzx / and / jne) become a jmp into a stub in a
//   private region next to the image, the rest NOPed. The stub replays the
//   test. When the type has a walk mode it also looks the entry's id up in a
//   bitmap built from the config. A blocked id leaves through 590A88, the
//   exit a type without a walk mode takes, and everything else leaves
//   through 590A93, the untouched spawn. The urn's own loop therefore does
//   the skipping, and a level whose types are all blocked or unable to walk
//   takes the vanilla list-exhausted path and spawns nothing. That is the
//   behaviour of the ESR 2.4 patch, which was confirmed in game.
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
#include <string>
#include <string_view>

namespace CelestialRayOne::EvilUrnBlocklist {
namespace {

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

constexpr char         ConfigSection[]    = "evil_urn_blocklist";
constexpr std::int32_t MinimumMonsterId   = 0;
constexpr std::int32_t MaximumMonsterId   = 0x7FFF;
constexpr std::size_t  InitialConfigBytes = 16'384;
constexpr std::size_t  MaximumConfigBytes = 1'048'576;
constexpr std::size_t  RejectedSampleSize = 5;

static_assert(static_cast<std::size_t>(MaximumMonsterId) < BitmapBytes * 8);

constexpr char DefaultConfigToml[] =
    "# Evil Urn Blocklist\n"
    "#\n"
    "# Keeps the monster types listed below from being spawned by evil urns\n"
    "# (objects.txt OperateFn 68, IceCaveEvilUrn in Act 5).\n"
    "#\n"
    "# How an urn picks: when its objects.txt Parm7 roll succeeds, the urn walks\n"
    "# the monster types the game rolled for its level from levels.txt, and spawns\n"
    "# the FIRST one that has a walk mode (monstats2.txt mWL) as a boss pack.\n"
    "#\n"
    "# A blocked type is passed over exactly like a type that cannot walk: the urn\n"
    "# moves on to the next type, so a boss pack still spawns from whatever else\n"
    "# the level holds. When every type in the level is blocked or cannot walk,\n"
    "# the urn spawns nothing, the same as vanilla.\n"
    "#\n"
    "# Only urn spawns change. Blocked monsters still spawn everywhere else.\n"
    "#\n"
    "# Server side only. In single player the game hosts its own server, so it\n"
    "# applies there too.\n"
    "#\n"
    "# Console command: evilurn (status, blocked ids and counters)\n"
    "\n"
    "[evil_urn_blocklist]\n"
    "\n"
    "# Master switch. false installs nothing at all.\n"
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

struct Settings {
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

enum class HookState : std::uint8_t {
    NotAttempted,
    DisabledByConfig,
    NoBlockedIds,
    ConfigError,
    UnsupportedBuild,
    InstallFailed,
    Installed,
};

const D2RL::PluginContext*                 Context{};
std::uintptr_t                             Base{};
Settings                                   Config{};
std::array<std::uint8_t, BitmapBytes>      BlockedBits{};
std::int32_t                               BlockedCount{};
bool                                       ConfigFileWasRead{};
std::atomic<HookState>                     State{ HookState::NotAttempted };
void*                                      HookRegion{};
bool                                       SitePatched{};

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

void ParseConfig(std::string_view text, ListReport& report) noexcept {
    std::string_view section;
    std::string      arrayText;
    bool             capturing = false;

    std::size_t cursor = 0;
    while (cursor < text.size()) {
        std::size_t end = text.find('\n', cursor);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view line = Trim(StripComment(text.substr(cursor, end - cursor)));
        cursor = end + 1;

        if (capturing) {
            if (!line.empty() && line.front() == '[') {
                // A table header before the closing bracket: the list was
                // never closed. Drop it and read the header normally.
                capturing = false;
                report.unterminated = true;
                ClearBlocked();
            } else {
                const std::size_t close = line.find(']');
                arrayText.append(line.substr(0, close));
                arrayText.push_back(',');
                if (close != std::string_view::npos) {
                    capturing = false;
                    ApplyIdList(arrayText, report);
                }
                continue;
            }
        }

        if (line.empty()) continue;

        if (line.front() == '[') {
            const std::size_t close = line.find(']');
            section = Trim(line.substr(1, close == std::string_view::npos ? close : close - 1));
            continue;
        }

        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos || section != ConfigSection) continue;
        const std::string_view key = Trim(line.substr(0, equals));
        const std::string_view value = Trim(line.substr(equals + 1));

        if (key == "enabled") {
            if (!ParseBool(value, Config.enabled)) {
                D2RL::LogWarnF(Context,
                    "EvilUrnBlocklist: enabled must be true or false; keeping %s.",
                    Config.enabled ? "true" : "false");
            }
        } else if (key == "blocked_monster_ids") {
            // The last assignment wins, as with every other key.
            ClearBlocked();
            report.found = true;
            report.malformed = false;
            report.unterminated = false;
            report.rejected = 0;
            report.rejectedSample.clear();
            if (value.empty() || value.front() != '[') {
                report.malformed = true;
                continue;
            }
            const std::size_t close = value.find(']');
            if (close == std::string_view::npos) {
                arrayText.assign(value.substr(1));
                arrayText.push_back(',');
                capturing = true;
            } else {
                arrayText.assign(value.substr(1, close - 1));
                ApplyIdList(arrayText, report);
            }
        }
    }

    if (capturing) {
        report.unterminated = true;
        ClearBlocked();
    }
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

auto ReadConfiguration() noexcept -> bool {
    std::string text;
    if (!Context->EnsureConfig(DefaultConfigToml)) {
        Context->LogWarn("EvilUrnBlocklist: the config file could not be created; using the "
                         "embedded defaults.");
        text = DefaultConfigToml;
    } else if (!ReadConfigText(text)) {
        Context->LogWarn("EvilUrnBlocklist: the config file could not be read; using the "
                         "embedded defaults.");
        text = DefaultConfigToml;
    } else {
        ConfigFileWasRead = true;
    }

    ListReport report{};
    ParseConfig(text, report);

    if (!report.found) {
        Context->LogWarn("EvilUrnBlocklist: blocked_monster_ids is missing from "
                         "[evil_urn_blocklist], so nothing is blocked.");
    } else if (report.malformed) {
        Context->LogError("EvilUrnBlocklist: blocked_monster_ids must be a list in square "
                          "brackets, for example [1121, 1122]. Nothing is blocked.");
        return false;
    } else if (report.unterminated) {
        Context->LogError("EvilUrnBlocklist: blocked_monster_ids is missing its closing ']'. "
                          "Nothing is blocked.");
        return false;
    }

    if (report.rejected > 0) {
        D2RL::LogWarnF(Context,
            "EvilUrnBlocklist: %d entr%s in blocked_monster_ids ignored (%s%s). Every entry "
            "must be a whole number from %d to %d.",
            report.rejected, report.rejected == 1 ? "y was" : "ies were",
            report.rejectedSample.c_str(),
            report.rejected > static_cast<std::int32_t>(RejectedSampleSize) ? ", ..." : "",
            MinimumMonsterId, MaximumMonsterId);
    }
    return true;
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
        Context->LogError("EvilUrnBlocklist: no hook region was available within rel32 reach "
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
        Context->LogError("EvilUrnBlocklist: hook region protection could not be finalized.");
        VirtualFree(HookRegion, 0, MEM_RELEASE);
        HookRegion = nullptr;
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), code, PageBytes);

    if (!CanEncodeRel32(Base + WalkTestRva, RegionAddress(CodeOffset))) {
        Context->LogError("EvilUrnBlocklist: hook region displacement validation failed.");
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
// Verification and installation
// ---------------------------------------------------------------------------

auto Verify(std::uint64_t rva, const std::uint8_t* expected, std::size_t size,
        const char* label) noexcept -> bool {
    if (Context->CheckExpectedBytes(rva, expected, static_cast<std::uint32_t>(size))) return true;
    D2RL::LogErrorF(Context,
        "EvilUrnBlocklist: NOT installed. %s at 0x%llX does not match the verified D2R "
        "image, or another plugin already owns it. Nothing was patched.",
        label, static_cast<unsigned long long>(rva));
    return false;
}

auto VerifyNativeContract() noexcept -> bool {
    return Verify(OperateFunction68Rva, OperateEntryWitness, sizeof(OperateEntryWitness),
               "The evil urn (OperateFn 68) entry")
        && Verify(SpawnChanceRva, SpawnChanceWitness, sizeof(SpawnChanceWitness),
               "The urn's Parm7 spawn roll")
        && Verify(RegionWalkRva, RegionWalkWitness, sizeof(RegionWalkWitness),
               "The urn's monster type loop")
        && Verify(SpawnRva, SpawnCallWitness, sizeof(SpawnCallWitness),
               "The urn's boss pack spawn");
}

void InstallHook(bool configUsable) noexcept {
    if (!Config.enabled) {
        State.store(HookState::DisabledByConfig, std::memory_order_release);
        Context->LogInfo("EvilUrnBlocklist: not installed, turned off in the config file.");
        return;
    }
    if (!configUsable) {
        State.store(HookState::ConfigError, std::memory_order_release);
        return;
    }
    if (BlockedCount == 0) {
        State.store(HookState::NoBlockedIds, std::memory_order_release);
        Context->LogInfo("EvilUrnBlocklist: not installed, blocked_monster_ids is empty.");
        return;
    }
    if (!VerifyNativeContract()) {
        State.store(HookState::UnsupportedBuild, std::memory_order_release);
        return;
    }
    if (!BuildHookRegion()) {
        State.store(HookState::InstallFailed, std::memory_order_release);
        return;
    }

    std::uint8_t jump[WalkTestSize]{};
    EncodeSiteJump(jump);
    if (!Context->PatchBytes(WalkTestRva, WalkTestOriginal, WalkTestSize, jump, WalkTestSize)) {
        // Nothing points at the region, and nothing in it points at this DLL.
        State.store(HookState::InstallFailed, std::memory_order_release);
        Context->LogError("EvilUrnBlocklist: NOT installed. The urn's walk test at 0x590A79 "
                          "could not be redirected.");
        return;
    }
    SitePatched = true;
    State.store(HookState::Installed, std::memory_order_release);

    std::string ids;
    AppendBlockedIds(ids, 900);
    D2RL::LogInfoF(Context, "EvilUrnBlocklist: installed. %d monster id%s blocked:%s",
        BlockedCount, BlockedCount == 1 ? "" : "s", ids.c_str());
}

// ---------------------------------------------------------------------------
// Console command
// ---------------------------------------------------------------------------

auto StateText(HookState state) noexcept -> const char* {
    switch (state) {
    case HookState::Installed:        return "active";
    case HookState::DisabledByConfig: return "off in the config file";
    case HookState::NoBlockedIds:     return "off, blocked_monster_ids is empty";
    case HookState::ConfigError:      return "NOT ACTIVE, blocked_monster_ids could not be read, see the log";
    case HookState::UnsupportedBuild: return "NOT ACTIVE, unrecognised game build or the urn is already hooked, see the log";
    case HookState::InstallFailed:    return "NOT ACTIVE, the hook failed, see the log";
    case HookState::NotAttempted:
    default:                          return "NOT ACTIVE, never attempted";
    }
}

auto __cdecl StatusCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command,
        void*) noexcept -> D2RL::ConsoleCommandResult {
    if (command == nullptr || command->plugin == nullptr) return D2RL::ConsoleCommandResult::Failed;
    const D2RL::PluginContext* plugin = command->plugin;

    const UrnCounters counters = ReadCounters();
    char header[384]{};
    if (counters.skipped > 0) {
        std::snprintf(header, sizeof(header),
            "Evil Urn Blocklist: %s | config %s | %d id%s blocked | blocked types skipped %llu, "
            "last %u | packs spawned %llu",
            StateText(State.load(std::memory_order_acquire)),
            ConfigFileWasRead ? "loaded" : "NOT READ (embedded defaults)",
            BlockedCount, BlockedCount == 1 ? "" : "s",
            static_cast<unsigned long long>(counters.skipped), counters.lastSkippedId,
            static_cast<unsigned long long>(counters.spawned));
    } else {
        std::snprintf(header, sizeof(header),
            "Evil Urn Blocklist: %s | config %s | %d id%s blocked | blocked types skipped 0 | "
            "packs spawned %llu",
            StateText(State.load(std::memory_order_acquire)),
            ConfigFileWasRead ? "loaded" : "NOT READ (embedded defaults)",
            BlockedCount, BlockedCount == 1 ? "" : "s",
            static_cast<unsigned long long>(counters.spawned));
    }
    plugin->WriteConsoleMessage(header);

    std::string ids = "ids:";
    AppendBlockedIds(ids, 380);
    plugin->WriteConsoleMessage(BlockedCount == 0 ? "ids: none" : ids.c_str());
    return D2RL::ConsoleCommandResult::Handled;
}

constexpr D2RL::PluginInfo Info{
    .infoSize    = D2RL::PluginInfoSize,
    .apiVersion  = D2RL_PLUGIN_API_VERSION,
    .id          = "celestialrayone.evil-urn-blocklist",
    .name        = "Evil Urn Blocklist",
    .version     = "1.0.0",
    .author      = "CelestialRayOne",
    .description = "Keeps a configurable list of monster types out of evil urn spawns; the "
                   "urn moves on to the next type of the level instead.",
    // Server: object operate functions only run on the server.
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
    // see why nothing is blocked.
    if (!Context->RegisterConsoleCommand("evilurn", &StatusCommand,
            "Reports Evil Urn Blocklist status, blocked ids and counters.")) {
        Context->LogWarn("EvilUrnBlocklist: the status console command was refused.");
    }

    const bool configUsable = ReadConfiguration();
    InstallHook(configUsable);
    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    if (Context == nullptr || !SitePatched) return;
    const bool retargeted = RetargetStubToVanilla();
    if (!RestoreSite()) {
        Context->LogError(retargeted
            ? "EvilUrnBlocklist: the urn's walk test could not be restored; it keeps running "
              "the original test through the hook region, without the blocklist."
            : "EvilUrnBlocklist: the urn's walk test could not be restored and the stub could "
              "not be retargeted; the blocklist stays in effect.");
    }
    // The region is deliberately kept: a thread may be inside the stub right
    // now, and a site that could not be restored still needs it.
}

}  // namespace CelestialRayOne::EvilUrnBlocklist
