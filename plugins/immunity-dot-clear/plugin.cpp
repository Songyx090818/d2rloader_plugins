// Immunity Clears DoTs
//
// When a monster becomes immune to every damage type, the life drains that
// are already running on it end on the spot: poison, bleed, burning and any
// other timed effect that drains life through negative life regeneration
// (stat 74, hpregen). Once applied, those drains are no longer poison or
// physical damage, only negative regeneration, so without this plugin they
// keep hurting a boss through its invulnerable phase.
//
// Immunity is judged per drain, against the unit that applied it, exactly the
// way the game's damage code judges a hit from that unit, Sunder included.
//
// 1.1.0: 1.0.0 read the monster's raw resistance stats. Sunder never changes
// those: the game applies it per attacker, inside the damage formula only.
// ESR bosses keep 100+ in every resistance and are broken with Sunder, so
// 1.0.0 saw every boss as permanently immune and ended every drain on the
// frame after it landed.
//
// 1.1.1: the resistance floor, the immunity line, the cap ceiling and the
// Sanctuary state are read from the running game instead of being pinned. ESR's
// resistance floor patch (-1000 at 0x4524C4 and 0x4524E7) changed the pinned
// floor bytes, so 1.1.0 refused to load next to it. The plugin now follows
// whatever values a static patch puts there, the same way the damage code does.
//
// Built for D2R 3.3 on D2RLoader 1.3.1 (PluginSDK 0.3.0). Everything below was
// read out of the live D2RLoader.exe image and D2RCore.dll and disassembled
// before being relied on.
//
// ---------------------------------------------------------------------------
// Where it hooks: the monster stat-regeneration tick, 0x448C00
// ---------------------------------------------------------------------------
//   D2GAME_MONSTER_ApplyStatRegen (game, unit, ...). It is monster event 3:
//   the dispatcher 0x447420 takes the image base (447498 lea r14) and calls
//   [rdi*8+1D1A7A0h+r14] with rcx = game, rdx = unit (447527..44756F). The
//   table holds 0x448C00 for event 3 and 0x42EA80 for event 12; both entries
//   are checked at load.
//
//   The tick runs every frame while the monster's life regeneration is not
//   zero and is the only place negative regeneration removes a monster's
//   life. It reads the unit's stat 74 first (448C20..448C34). The plugin runs
//   before it, so the tick already reads the regeneration without the drains
//   that were ended.
//
// ---------------------------------------------------------------------------
// What counts as a drain
// ---------------------------------------------------------------------------
//   Poison (0x4518C0), burning (0x451380) and every state applied through
//   D2RLoader's timed-state function (0x433D20, how the bleed plugin applies
//   bleed) build the same kind of effect: a child stat list with timer flag 2,
//   an end frame, stat 74 = -damage, a removal callback and an event 12 for
//   the end frame. The plugin takes every child list in the unit's merged
//   chain (unit+88h, +90h, next +68h, as 0x2F5940 walks it) with
//     flags (+1Ch): not extended (sign bit), timer flag 2, no custom-timer
//                   flag 8000h, no unmerged flag 2000h
//     own stat 74 below 0, read with the list stat reader 0x2F5DF0 the poison
//     applier itself uses.
//
// ---------------------------------------------------------------------------
// Who applied it
// ---------------------------------------------------------------------------
//   STATLIST_AllocStatList 0x2F7300 (flags, end frame, type, id) stores the
//   source's unit type at +8 and unit id at +0Ch; 0x2F6E30 / 0x2F6E20 read
//   them back. Poison, burning and the timed-state function all pass the
//   attacker; for a missile hit 0x436E90 has already swapped the missile for
//   its owner. The regen tick's own kill credit (448E1A..448E2E) finds the
//   killer exactly this way: SUNIT_GetServerUnit 0x48FE80 (game, type, id).
//
// ---------------------------------------------------------------------------
// What counts as immune: SUNITDMG_ApplyResistancesAndAbsorb 0x4523E0
// ---------------------------------------------------------------------------
//   SUNITDMG_CalculateTotalDamage 0x44DF10 builds a context at rsp+40h
//     +00h game  +10h attacker  +18h defender
//     +24h 1 when the defender is a monster and not a hireling (44DFAE)
//   and twelve 40h-byte records from rbp-80h (44EC40 walks them):
//     +08h resistance stat  +0Ch max stat  +10h -enemy-res stat
//     +14h Sunder stat
//   For the six damage types (44E14D..44E454, constants at 1D1ACE0 and
//   1D1AD20):
//     physical   36  -1   366 in Reign of the Warlock games, else -1   192
//     magic      37  38   358 in Reign of the Warlock games, else -1   193
//     fire       39  40   333   189
//     lightning  41  42   334   190
//     cold       43  44   335   187
//     poison     45  46   336   191
//   ("Reign of the Warlock game" is 0x4040B0: game data context == 3.)
//   For each record the resolver does, with rsi = context, r14 = record and
//   ebp = 0 (4523F5):
//     45242A  resistance = 0x2F5020(defender, record+8, 0)
//     452444  resistance = Sunder 0x44F6D0(context, record, resistance)
//     452452  resistance = -enemy-res 0x44F8B0(context, record, resistance)
//     452481  0x092A10 decides a floor; it is "xor al,al / ret" in this
//             build, so that floor never applies
//     4524C0  a value of 0 or less becomes max(value, floor) and is final;
//             the floor is the imm32 at 4524C5 (-100 stock, -1000 with ESR's
//             resistance floor patch)
//     45249C  the player cap, skipped when context+24h is set; it ends at
//             min(..., ceiling), the imm32 at 4524DE (95 stock)
//     4524F8  physical: 0 if the attacker has the Sanctuary state (imm32 at
//             452504, 47 stock) and the defender is undead (0x3AF2C0)
//     452547  damage *= line - min(resistance, line), line = imm32 at 452548
//             (100 stock)
//   So a monster is immune to a damage type from that attacker exactly when
//   the result reaches the line. The four immediates are read from the
//   running game each time; only the instructions around them are pinned. The plugin builds the same context and record
//   and calls the same Sunder and -enemy-res functions, then applies the
//   Sanctuary rule. Sunder reads context +0, +10h, +18h, +24h and record +8,
//   +14h; -enemy-res reads context +0, +10h, +18h, +24h and record +8, +10h
//   (44F6D0.., 44F8B0..); neither reads anything else of either.
//   D2RCore's ApplyResistanceComponentWithPrimeEvilThreat, which now calls
//   0x4523E0 for the game, only adds Prime Evil threat bookkeeping afterwards.
//
//   Capped defenders end at or below the ceiling, so while the ceiling is
//   under the line they are never immune: hirelings (0x3AF240 non-zero, so
//   context+24h is 0 and the player cap applies), and, while the Summon
//   Resist Cap plugin is active (its jmp at the 45249C gate), allied monsters:
//   monstats.txt Align, a byte at record +87h (column parser 39DE8E /
//   39E035), equal to 1, which is what that plugin caps. If a patch ever
//   raises the ceiling to the line, the plugin says so once in the log and
//   still leaves capped defenders alone.
//
// ---------------------------------------------------------------------------
// How a drain is ended: the game's own state-expiry event
// ---------------------------------------------------------------------------
//   Event 12 is 0x42EA80: ExpireUnitStatLists(unit, [game+170h], 1.0), a
//   loader thunk at 0x2F82F0 to D2RCore ExpireWideUnitStats. With a frame, it
//   removes every timer-flag-2 list (no 8000h) whose end frame is at or before
//   that frame: it takes the list's stats off the unit, runs the list's
//   removal callback (poison's stacking-aware 0x4505E0, the default 0x436240,
//   or D2RCore's default), then frees the list. The callbacks turn the state
//   off and tell clients, so the overlay clears.
//
//   The plugin sets each qualifying drain's end frame to the current frame
//   with the game's own setter 0x2F7AB0 and runs event 12 for the unit: what
//   happens when the drains run out by themselves. Nothing of the removal is
//   reimplemented. Afterwards it checks that the drains it ended are gone.

#include <D2RLPlugin/api.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

namespace CelestialRayOne::ImmunityDotClear {
namespace {

// ---------------------------------------------------------------------------
// Native anchors
// ---------------------------------------------------------------------------

constexpr std::uint64_t MonsterStatRegenRva          = 0x448C00;
constexpr std::uint64_t MonsterStatRegenArgumentsRva = 0x448C20;
constexpr std::uint64_t MonsterEventBaseRva          = 0x447498;
constexpr std::uint64_t MonsterEventDispatchRva      = 0x447527;
constexpr std::uint64_t MonsterEventTableRva         = 0x1D1A7A0;
constexpr std::uint64_t StatListExpiryEventRva       = 0x42EA80;
constexpr std::uint64_t ExpireUnitStatListsRva       = 0x2F82F0;
constexpr std::uint64_t SetStatListExpireFrameRva    = 0x2F7AB0;
constexpr std::uint64_t GetUnitStatRva               = 0x2F5020;
constexpr std::uint64_t ReadListStatRva              = 0x2F5DF0;
constexpr std::uint64_t GetDataContextRva            = 0x34A0E0;
constexpr std::uint64_t GetUnitTypeRva               = 0x34B9D0;
constexpr std::uint64_t GetClassIdRva                = 0x349860;
constexpr std::uint64_t GetStatListExRva             = 0x34B870;
constexpr std::uint64_t StateListExtendedGateRva     = 0x2F5991;
constexpr std::uint64_t StateListChainRva            = 0x2F59BB;
constexpr std::uint64_t ListOwnerGuidRva             = 0x2F6E20;
constexpr std::uint64_t ListOwnerTypeRva             = 0x2F6E30;
constexpr std::uint64_t GetServerUnitRva             = 0x48FE80;
constexpr std::uint64_t SunderRva                    = 0x44F6D0;
constexpr std::uint64_t PierceRva                    = 0x44F8B0;
constexpr std::uint64_t ImmunitySwitchRva            = 0x092A10;
constexpr std::uint64_t CheckStateRva                = 0x3351B0;
constexpr std::uint64_t IsUndeadRva                  = 0x3AF2C0;
constexpr std::uint64_t IsHirelingRva                = 0x3AF240;
constexpr std::uint64_t MonStatsRecordRva            = 0x0976E0;
constexpr std::uint64_t RotwGameRva                  = 0x4040B0;
constexpr std::uint64_t ResolverStepsRva             = 0x45242A;
constexpr std::uint64_t ResolverSwitchRva            = 0x452481;
constexpr std::uint64_t ResolverSignRva              = 0x4524C0;
constexpr std::uint64_t FloorImmRva                  = 0x4524C5;
constexpr std::uint64_t ResolverFloorTailRva         = 0x4524C9;
constexpr std::uint64_t ResolverCapGateRva           = 0x45249C;
constexpr std::uint64_t CeilingRva                   = 0x4524DD;
constexpr std::uint64_t CeilingImmRva                = 0x4524DE;
constexpr std::uint64_t CeilingTailRva               = 0x4524E2;
constexpr std::uint64_t SanctuaryHeadRva             = 0x4524F8;
constexpr std::uint64_t SanctuaryStateRva            = 0x452503;
constexpr std::uint64_t SanctuaryStateImmRva         = 0x452504;
constexpr std::uint64_t SanctuaryTailRva             = 0x452508;
constexpr std::uint64_t ImmunityLineRva              = 0x452547;
constexpr std::uint64_t ImmunityLineImmRva           = 0x452548;
constexpr std::uint64_t ImmunityLineTailRva          = 0x45254C;
constexpr std::uint64_t UncappedFlagRva              = 0x44DFAE;
constexpr std::uint64_t ResistanceLoopRva            = 0x44EC40;
constexpr std::uint64_t ResistanceLoopTailRva        = 0x44EC5F;
constexpr std::uint64_t RecordsHeadRva               = 0x44E14D;
constexpr std::uint64_t PhysicalSunderRva            = 0x44E202;
constexpr std::uint64_t ElementalIdsRva              = 0x44E215;
constexpr std::uint64_t MagicRecordRva               = 0x44E25C;
constexpr std::uint64_t PoisonLoadRva                = 0x44E317;
constexpr std::uint64_t PoisonStoreRva               = 0x44E3C9;
constexpr std::uint64_t PoisonIdsRva                 = 0x44E440;
constexpr std::uint64_t ElementConstantsRva          = 0x1D1ACE0;
constexpr std::uint64_t PoisonConstantsRva           = 0x1D1AD20;
constexpr std::uint64_t AlignNameRva                 = 0x39DE8E;
constexpr std::uint64_t AlignColumnRva               = 0x39E035;

constexpr std::size_t MonsterStatRegenEvent = 3;
constexpr std::size_t StatListExpiryEvent   = 12;

// The thunks keep an FF 25 disp32 jump into D2RLoader's import slot, then
// padding, then the untouched remainder of the old body. The slot moves
// between loader builds, so only the opcode, the padding and the body are
// pinned.
constexpr std::uint64_t ThunkPaddingOffset    = 6;
constexpr std::uint64_t ExpireThunkBodyOffset = 7;
constexpr std::uint64_t PaddedThunkBodyOffset = 10;

// ---------------------------------------------------------------------------
// Witnesses, cut from the live image
// ---------------------------------------------------------------------------

// 448C00: push rbx / push rbp / push rdi / sub rsp,90h / mov rax,[cookie] /
// xor rax,rsp / mov [rsp+70h],rax / mov rdi,rdx / xor r8d,r8d. Hooked.
constexpr std::uint8_t RegenEntryWitness[]{
    0x40, 0x53, 0x55, 0x57, 0x48, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00, 0x48,
    0x8B, 0x05, 0xB6, 0x26, 0x58, 0x02, 0x48, 0x33, 0xC4, 0x48, 0x89, 0x44,
    0x24, 0x70, 0x48, 0x8B, 0xFA, 0x45, 0x33, 0xC0,
};

// 448C20: mov rbp,rcx (game) / mov rcx,rdi (unit) / lea edx,[r8+4Ah] /
// call 2F5020 / mov rcx,rdi / mov ebx,eax. The tick reads stat 74 first.
constexpr std::uint8_t RegenArgumentsWitness[]{
    0x48, 0x8B, 0xE9, 0x48, 0x8B, 0xCF, 0x41, 0x8D, 0x50, 0x4A, 0xE8, 0xF1,
    0xC3, 0xEA, 0xFF, 0x48, 0x8B, 0xCF, 0x8B, 0xD8,
};

// 447498: lea r14,[image base].
constexpr std::uint8_t MonsterEventBaseWitness[]{
    0x4C, 0x8D, 0x35, 0x61, 0x8B, 0xBB, 0xFF,
};

// 447527: lea rdi,[rdi*8+1D1A7A0h] .. mov r10,[rdi+r14] .. rdx = unit,
// rcx = game .. call r10.
constexpr std::uint8_t MonsterEventDispatchWitness[]{
    0x48, 0x8D, 0x3C, 0xFD, 0xA0, 0xA7, 0xD1, 0x01, 0x4A, 0x83, 0x3C, 0x37,
    0x00, 0x75, 0x14, 0x48, 0x8D, 0x4C, 0x24, 0x40, 0xC6, 0x44, 0x24, 0x40,
    0x00, 0xE8, 0xEB, 0xF3, 0xFF, 0xFF, 0x84, 0xC0, 0x74, 0x01, 0xCC, 0x4E,
    0x8B, 0x14, 0x37, 0x4D, 0x85, 0xD2, 0x74, 0x1F, 0x8B, 0x84, 0x24, 0x88,
    0x00, 0x00, 0x00, 0x44, 0x8B, 0xC5, 0x44, 0x8B, 0x8C, 0x24, 0x80, 0x00,
    0x00, 0x00, 0x48, 0x8B, 0xD3, 0x48, 0x8B, 0xCE, 0x89, 0x44, 0x24, 0x20,
    0x41, 0xFF, 0xD2,
};

// 42EA80: event 12, ExpireUnitStatLists(unit, [game+170h], 1.0).
constexpr std::uint8_t ExpiryEventWitness[]{
    0xF3, 0x0F, 0x10, 0x15, 0x7C, 0xBE, 0x88, 0x01, 0x48, 0x8B, 0xC2, 0x8B,
    0x91, 0x70, 0x01, 0x00, 0x00, 0x48, 0x8B, 0xC8, 0xE9, 0x57, 0x98, 0xEC,
    0xFF,
};

// FF 25 disp32 at every loader thunk.
constexpr std::uint8_t ThunkJumpWitness[]{
    0xFF, 0x25,
};

// 2F82F6: the one padding byte of the expiry thunk.
constexpr std::uint8_t ExpireThunkPaddingWitness[]{
    0x90,
};

// 2F82F7: the old expiry body: xmm7 = step, ebp = frame, rbx = unit.
constexpr std::uint8_t ExpireThunkBodyWitness[]{
    0x0F, 0x29, 0x7C, 0x24, 0x20, 0x0F, 0x28, 0xFA, 0x8B, 0xEA, 0x48, 0x8B,
    0xD9, 0x48, 0x85, 0xC9, 0x75, 0x23,
};

// 2F7AB0: if (list) { if (frame > 0) flags |= 2; [list+24h] = (float)frame; }
constexpr std::uint8_t SetExpireFrameWitness[]{
    0x48, 0x85, 0xC9, 0x74, 0x14, 0x85, 0xD2, 0x7E, 0x04, 0x83, 0x49, 0x1C,
    0x02, 0x66, 0x0F, 0x6E, 0xC2, 0x0F, 0x5B, 0xC0, 0xF3, 0x0F, 0x11, 0x41,
    0x24, 0xC3,
};

// 2F5026 / 2F5DF6: the four padding bytes of the stat reader thunks.
constexpr std::uint8_t UnitStatThunkPaddingWitness[]{
    0x90, 0x90, 0x90, 0x90,
};

// 2F502A: the old unit stat reader body: (unit, statId, layer).
constexpr std::uint8_t UnitStatThunkBodyWitness[]{
    0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x41, 0x0F,
    0xB7, 0xE8, 0x8B, 0xFA, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9, 0x75, 0x2A,
};

constexpr std::uint8_t ListStatThunkPaddingWitness[]{
    0x90, 0x90, 0x90, 0x90,
};

// 2F5DFA: the old list stat reader body: (dataContext, list, statId, layer).
constexpr std::uint8_t ListStatThunkBodyWitness[]{
    0x57, 0x48, 0x83, 0xEC, 0x20, 0x41, 0x0F, 0xB7, 0xF1, 0x41, 0x8B, 0xD8,
    0x48, 0x8B, 0xFA, 0x48, 0x85, 0xD2, 0x75, 0x12,
};

// 34A0E0: returns byte [unit+1BDh], the data context.
constexpr std::uint8_t GetDataContextWitness[]{
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x85, 0xC9, 0x75, 0x1A, 0x88, 0x4C, 0x24,
    0x30, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8, 0x49, 0xC7, 0xFF, 0xFF, 0x84,
    0xC0, 0x74, 0x01, 0xCC, 0x32, 0xC0, 0x48, 0x83, 0xC4, 0x28, 0xC3, 0x0F,
    0xB6, 0x81, 0xBD, 0x01, 0x00, 0x00, 0x48, 0x83, 0xC4, 0x28, 0xC3,
};

// 34B9D0: returns dword [unit+0], the unit type.
constexpr std::uint8_t GetUnitTypeWitness[]{
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x85, 0xC9, 0x75, 0x1D, 0x88, 0x4C, 0x24,
    0x30, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8, 0x39, 0x9E, 0xFF, 0xFF, 0x84,
    0xC0, 0x74, 0x01, 0xCC, 0xB8, 0x06, 0x00, 0x00, 0x00, 0x48, 0x83, 0xC4,
    0x28, 0xC3, 0x8B, 0x01, 0x48, 0x83, 0xC4, 0x28, 0xC3,
};

// 349860: returns dword [unit+4], the class id (monstats.txt row).
constexpr std::uint8_t GetClassIdWitness[]{
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x85, 0xC9, 0x75, 0x1D, 0x88, 0x4C, 0x24,
    0x30, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8, 0x49, 0xCB, 0xFF, 0xFF, 0x84,
    0xC0, 0x74, 0x01, 0xCC, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0x48, 0x83, 0xC4,
    0x28, 0xC3, 0x8B, 0x41, 0x04, 0x48, 0x83, 0xC4, 0x28, 0xC3,
};

// 34B870: returns [unit+88h], the unit's extended stat list.
constexpr std::uint8_t GetStatListExWitness[]{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9,
    0x75, 0x20, 0x88, 0x4C, 0x24, 0x30, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8,
    0x44, 0x9B, 0xFF, 0xFF, 0x84, 0xC0, 0x74, 0x01, 0xCC, 0x48, 0x8B, 0x83,
    0x88, 0x00, 0x00, 0x00, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3,
};

// 2F5991: cmp dword [list+1Ch],0 / jge (not extended: no children).
constexpr std::uint8_t StateListExtendedGateWitness[]{
    0x83, 0x7F, 0x1C, 0x00, 0x7D, 0x58,
};

// 2F59BB: merged chain [list+90h], state [child+20h], next [child+68h],
// then the unmerged chain [list+98h].
constexpr std::uint8_t StateListChainWitness[]{
    0x48, 0x8B, 0x87, 0x90, 0x00, 0x00, 0x00, 0x48, 0x85, 0xC0, 0x74, 0x0E,
    0x39, 0x58, 0x20, 0x74, 0x25, 0x48, 0x8B, 0x40, 0x68, 0x48, 0x85, 0xC0,
    0x75, 0xF2, 0x48, 0x8B, 0x87, 0x98, 0x00, 0x00, 0x00, 0x48, 0x85, 0xC0,
    0x74, 0x0E, 0x39, 0x58, 0x20, 0x74, 0x0B, 0x48, 0x8B, 0x40, 0x68,
};

// 2F6E20: source unit id [list+0Ch]; 2F6E30: source unit type [list+8], 6
// for no list.
constexpr std::uint8_t ListOwnerWitness[]{
    0x48, 0x85, 0xC9, 0x75, 0x03, 0x33, 0xC0, 0xC3, 0x8B, 0x41, 0x0C, 0xC3,
    0xCC, 0xCC, 0xCC, 0xCC, 0xB8, 0x06, 0x00, 0x00, 0x00, 0x48, 0x85, 0xC9,
    0x74, 0x03, 0x8B, 0x41, 0x08, 0xC3,
};

// 48FE80: SUNIT_GetServerUnit (game, type, id).
constexpr std::uint8_t ServerUnitWitness[]{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48,
    0x83, 0xEC, 0x20, 0x41, 0x8B, 0xD8, 0x8B, 0xF2, 0x48, 0x8B, 0xF9, 0x48,
    0x85, 0xC9,
};

// 44F6D0: Sunder (context, record, resistance): record+14h, context+24h,
// context+10h through 2F5020, record+8 and context+18h through 2F48C0,
// context+0 -> game+106h.
constexpr std::uint8_t SunderWitness[]{
    0x40, 0x53, 0x55, 0x57, 0x48, 0x81, 0xEC, 0xD0, 0x00, 0x00, 0x00, 0x48,
    0x8B, 0x05, 0xE6, 0xBB, 0x57, 0x02, 0x48, 0x33, 0xC4, 0x48, 0x89, 0x84,
    0x24, 0xA0, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xFA, 0x44, 0x89, 0x44, 0x24,
    0x24, 0x8B, 0x52, 0x14, 0x41, 0x8B, 0xE8, 0x48, 0x8B, 0xD9, 0x83, 0xFA,
    0xFF, 0x0F, 0x84, 0x84, 0x01, 0x00, 0x00, 0x83, 0x79, 0x24, 0x00, 0x0F,
    0x84, 0x7A, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x49, 0x10, 0x45, 0x33, 0xC0,
    0xE8, 0x03, 0x59, 0xEA, 0xFF, 0x89, 0x44, 0x24, 0x28, 0x85, 0xC0, 0x0F,
    0x84, 0x62, 0x01, 0x00, 0x00, 0x8B, 0x57, 0x08, 0x45, 0x33, 0xC0, 0x48,
    0x8B, 0x4B, 0x18,
};

// 44F8B0: -enemy-res (context, record, resistance): record+10h, the 092A10
// switch, "resistance >= 100 and context+24h set -> unchanged", context+10h,
// record+8, context+18h.
constexpr std::uint8_t PierceWitness[]{
    0x40, 0x53, 0x55, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0xC8, 0x00, 0x00,
    0x00, 0x48, 0x8B, 0x05, 0x04, 0xBA, 0x57, 0x02, 0x48, 0x33, 0xC4, 0x48,
    0x89, 0x84, 0x24, 0xA0, 0x00, 0x00, 0x00, 0x83, 0x7A, 0x10, 0xFF, 0x41,
    0x8B, 0xF0, 0x48, 0x8B, 0xEA, 0x48, 0x8B, 0xD9, 0x0F, 0x84, 0xB4, 0x01,
    0x00, 0x00, 0x48, 0x8B, 0x09, 0xE8, 0x26, 0x31, 0xC4, 0xFF, 0x84, 0xC0,
    0x75, 0x0F, 0x83, 0xFE, 0x64, 0x7C, 0x0A, 0x83, 0x7B, 0x24, 0x00, 0x0F,
    0x85, 0x99, 0x01, 0x00, 0x00, 0x8B, 0x55, 0x10, 0x45, 0x33, 0xC0, 0x48,
    0x8B, 0x4B, 0x10, 0xE8, 0x14, 0x57, 0xEA, 0xFF, 0x44, 0x8B, 0xF8, 0x85,
    0xC0, 0x0F, 0x84, 0x7F, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x0B, 0xE8, 0xF1,
    0x30, 0xC4, 0xFF, 0x84, 0xC0, 0x74, 0x08, 0x41, 0x2B, 0xF7, 0xE9, 0x6B,
    0x01, 0x00, 0x00,
};

// 092A10: xor al,al / ret.
constexpr std::uint8_t ImmunitySwitchWitness[]{
    0x32, 0xC0, 0xC3,
};

// 45242A: read the defender's resistance, Sunder, -enemy-res, mov ebx,eax.
constexpr std::uint8_t ResolverStepsWitness[]{
    0x83, 0xFA, 0xFF, 0x74, 0x0C, 0x48, 0x8B, 0x49, 0x18, 0x45, 0x33, 0xC0,
    0xE8, 0xE5, 0x2B, 0xEA, 0xFF, 0x44, 0x8B, 0xC0, 0x49, 0x8B, 0xD6, 0x48,
    0x8B, 0xCE, 0xE8, 0x87, 0xD2, 0xFF, 0xFF, 0x44, 0x8B, 0xC0, 0x49, 0x8B,
    0xD6, 0x48, 0x8B, 0xCE, 0xE8, 0x59, 0xD4, 0xFF, 0xFF, 0x8B, 0xD8,
};

// 452481: the 092A10 floor, only when the switch returns non-zero.
constexpr std::uint8_t ResolverSwitchWitness[]{
    0x48, 0x8B, 0x0E, 0xE8, 0x87, 0x05, 0xC4, 0xFF, 0x84, 0xC0, 0x74, 0x33,
    0x48, 0x8B, 0x46, 0x08, 0x8B, 0x88, 0x80, 0x00, 0x00, 0x00, 0x3B, 0xD9,
    0x0F, 0x4E, 0xD9,
};

// 4524C0: test ebx,ebx / jg cap gate / mov ecx, imm32. The imm32 (the floor,
// 4524C5) is not pinned: ESR's resistance floor patch changes it.
constexpr std::uint8_t ResolverSignWitness[]{
    0x85, 0xDB, 0x7F, 0xD8, 0xB9,
};

// 4524C9: cmp ebx,ecx / cmovg ecx,ebx / jmp final: max(value, floor).
constexpr std::uint8_t ResolverFloorTailWitness[]{
    0x3B, 0xD9, 0x0F, 0x4F, 0xCB, 0xEB, 0x51,
};

// 4524DD: mov eax, imm32 (the ceiling, 4524DE, not pinned); 4524E2: cmp ecx,
// eax / cmovl eax,ecx: the cap ends at min(cap, ceiling).
constexpr std::uint8_t CeilingOpcodeWitness[]{
    0xB8,
};

constexpr std::uint8_t CeilingTailWitness[]{
    0x3B, 0xC8, 0x0F, 0x4C, 0xC1,
};

// 4524F8: cmp [record+8], 24h (physical) / jne / mov rcx, attacker;
// 452503: mov edx, imm32 (the Sanctuary state, 452504, not pinned);
// 452508: call STATES_CheckState / test / je / mov rcx, defender / call the
// undead test / test / cmovne ebx, ebp (0) / mov ecx, ebx.
constexpr std::uint8_t SanctuaryHeadWitness[]{
    0x41, 0x83, 0x7E, 0x08, 0x24, 0x75, 0x20, 0x48, 0x8B, 0x4E, 0x10,
};

constexpr std::uint8_t SanctuaryOpcodeWitness[]{
    0xBA,
};

constexpr std::uint8_t SanctuaryTailWitness[]{
    0xE8, 0xA3, 0x2C, 0xEE, 0xFF, 0x85, 0xC0, 0x74, 0x0E, 0x48, 0x8B, 0x4E,
    0x18, 0xE8, 0xA6, 0xCD, 0xF5, 0xFF, 0x85, 0xC0, 0x0F, 0x45, 0xDD, 0x8B,
    0xCB,
};

// 452547: mov ecx, imm32 (the line, 452548, not pinned); 45254C: cmp ebp,ecx /
// mov eax,ecx / cmovl eax,ebp / sub ecx,eax / mov ebp,eax: damage *= line -
// min(resistance, line).
constexpr std::uint8_t ImmunityLineOpcodeWitness[]{
    0xB9,
};

constexpr std::uint8_t ImmunityLineTailWitness[]{
    0x3B, 0xE9, 0x8B, 0xC1, 0x0F, 0x4C, 0xC5, 0x2B, 0xC8, 0x8B, 0xE8,
};

// 44DFAE: context+24h = (defender is a monster and 3AF240(defender) == 0).
constexpr std::uint8_t UncappedFlagWitness[]{
    0x48, 0x8B, 0xCE, 0xE8, 0x1A, 0xDA, 0xEF, 0xFF, 0x83, 0xF8, 0x01, 0x75,
    0x17, 0x48, 0x8B, 0xCE, 0xE8, 0x7D, 0x12, 0xF6, 0xFF, 0x85, 0xC0, 0x75,
    0x0B, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x89, 0x44, 0x24, 0x64, 0xEB, 0x05,
    0x44, 0x89, 0x74, 0x24, 0x64,
};

// 44EC40: records from rbp-80h, context at rsp+40h, 40h per record. The call
// at 44EC5A is the loader's and is not pinned.
constexpr std::uint8_t ResistanceLoopWitness[]{
    0x48, 0x8D, 0x5D, 0x80, 0x83, 0x7B, 0x24, 0x00, 0x74, 0x05, 0x45, 0x85,
    0xE4, 0x74, 0x22, 0x44, 0x8B, 0xC7, 0x48, 0x8D, 0x4C, 0x24, 0x40, 0x48,
    0x8B, 0xD3,
};

constexpr std::uint8_t ResistanceLoopTailWitness[]{
    0x44, 0x8B, 0x64, 0x24, 0x60, 0x41, 0xFF, 0xC6, 0x48, 0x83, 0xC3, 0x40,
    0x41, 0x83, 0xFE, 0x0C,
};

// 3AF240: hireling test (returns 0 for any other unit).
constexpr std::uint8_t HirelingTestWitness[]{
    0x40, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF1, 0xE8, 0x82, 0xC7,
    0xF9, 0xFF, 0x83, 0xF8, 0x01, 0x75, 0x5D, 0x48, 0x89, 0x5C, 0x24, 0x30,
    0x48, 0x8D, 0x15, 0x71, 0xA0, 0x95, 0x01, 0x41, 0xB8, 0x0A, 0x03, 0x00,
    0x00, 0x48, 0x89, 0x7C, 0x24, 0x38, 0x48, 0x8B, 0xCE, 0xE8, 0xEE, 0xA5,
    0xF9, 0xFF, 0x48, 0x8B, 0xCE, 0x8B, 0xF8, 0xE8, 0x64, 0xAE, 0xF9, 0xFF,
    0x33, 0xDB, 0x48, 0x8B, 0xCE, 0x3C, 0x01, 0x0F, 0x95, 0xC3, 0xE8, 0x55,
    0xAE, 0xF9, 0xFF, 0x44, 0x8B, 0xC7, 0x8B, 0xD3, 0x0F, 0xB6, 0xC8, 0xE8,
    0x98, 0x74, 0xFE, 0xFF, 0x48, 0x8B, 0x7C, 0x24, 0x38, 0x48, 0x8B, 0x5C,
    0x24, 0x30, 0x48, 0x85, 0xC0, 0x74, 0x09, 0x8B, 0x40, 0x0C, 0x48, 0x83,
    0xC4, 0x20, 0x5E, 0xC3, 0x33, 0xC0, 0x48, 0x83, 0xC4, 0x20, 0x5E, 0xC3,
};

// 3AF2C0: undead test (monstats flags at record +3Dh).
constexpr std::uint8_t UndeadTestWitness[]{
    0x40, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF9, 0xE8, 0x02, 0xC7,
    0xF9, 0xFF, 0x83, 0xF8, 0x01, 0x75, 0x57, 0x41, 0xB8, 0x35, 0x01, 0x00,
    0x00, 0x48, 0x89, 0x5C, 0x24, 0x30, 0x48, 0x8D, 0x15, 0xEB, 0x9F, 0x95,
    0x01, 0x48, 0x8B, 0xCF, 0xE8, 0x73, 0xA5, 0xF9, 0xFF, 0x48, 0x8B, 0xCF,
    0x8B, 0xD8, 0xE8, 0xE9, 0xAD, 0xF9, 0xFF, 0x8B, 0xD3, 0x0F, 0xB6, 0xC8,
    0xE8, 0xDF, 0x83, 0xCE, 0xFF, 0x48, 0x8B, 0x5C, 0x24, 0x30, 0x48, 0x85,
    0xC0, 0x74, 0x1F, 0x0F, 0xB6, 0x40, 0x3D, 0x85, 0x05, 0xC7, 0xA3, 0x9E,
    0x01, 0x75, 0x08, 0x85, 0x05, 0xC3, 0xA3, 0x9E, 0x01, 0x74, 0x0B, 0xB8,
    0x01, 0x00, 0x00, 0x00, 0x48, 0x83, 0xC4, 0x20, 0x5F, 0xC3, 0x33, 0xC0,
    0x48, 0x83, 0xC4, 0x20, 0x5F, 0xC3,
};

// 3351B0: STATES_CheckState (unit, state).
constexpr std::uint8_t CheckStateWitness[]{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48,
    0x83, 0xEC, 0x20, 0x8B, 0xDA, 0x48, 0x8B, 0xF1,
};

// 0976E0: DATATBLS_GetMonStatsTxtRecordForContext (context, classId).
constexpr std::uint8_t MonStatsRecordWitness[]{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48,
    0x83, 0xEC, 0x30, 0x48, 0x63, 0xF2, 0xE8, 0x99, 0x93, 0x26, 0x00, 0x48,
    0x8B, 0xF8, 0x48, 0x8B, 0xDE, 0x85, 0xF6, 0x78, 0x09, 0x48, 0x3B, 0x98,
    0x60, 0x0F, 0x00, 0x00, 0x72, 0x18,
};

// 4040B0: game data context == 3 (Reign of the Warlock).
constexpr std::uint8_t RotwGameWitness[]{
    0x80, 0xB9, 0x06, 0x01, 0x00, 0x00, 0x03, 0x0F, 0x94, 0xC0, 0xC3,
};

// 44E14D: physical record +8 = 24h, +0Ch = -1; the 4040B0 test; the fire,
// lightning and cold constant loads; physical +10h = 16Eh or -1.
constexpr std::uint8_t RecordsHeadWitness[]{
    0xC7, 0x45, 0x88, 0x24, 0x00, 0x00, 0x00, 0xC7, 0x45, 0x8C, 0xFF, 0xFF,
    0xFF, 0xFF, 0xE8, 0x50, 0x5F, 0xFB, 0xFF, 0x66, 0x0F, 0x6F, 0x05, 0x78,
    0xCB, 0x8C, 0x01, 0x84, 0xC0, 0xB9, 0xFF, 0xFF, 0xFF, 0xFF, 0x66, 0x0F,
    0x7F, 0x45, 0xD0, 0x66, 0x0F, 0x6F, 0x05, 0x74, 0xCB, 0x8C, 0x01, 0x48,
    0x8D, 0x05, 0xE9, 0xC9, 0x8C, 0x01, 0x48, 0x89, 0x45, 0xB0, 0xBF, 0x02,
    0x00, 0x00, 0x00, 0x49, 0x8D, 0x47, 0x20, 0x66, 0x0F, 0x7F, 0x45, 0x10,
    0x66, 0x0F, 0x6F, 0x05, 0x63, 0xCB, 0x8C, 0x01, 0xBA, 0x6E, 0x01, 0x00,
    0x00, 0x48, 0x89, 0x45, 0xC0, 0x0F, 0x45, 0xCA, 0x89, 0x4D, 0x90,
};

// 44E202: physical +14h = 0C0h, absorb -1 / -1.
constexpr std::uint8_t PhysicalSunderWitness[]{
    0xC7, 0x45, 0x94, 0xC0, 0x00, 0x00, 0x00, 0x48, 0xC7, 0x45, 0x98, 0xFF,
    0xFF, 0xFF, 0xFF,
};

// 44E215: fire 27h / 28h, lightning 29h / 2Ah, cold 2Bh / 2Ch, cold constants.
constexpr std::uint8_t ElementalIdsWitness[]{
    0xC7, 0x45, 0xC8, 0x27, 0x00, 0x00, 0x00, 0xC7, 0x45, 0xCC, 0x28, 0x00,
    0x00, 0x00, 0x48, 0x89, 0x7D, 0xE0, 0xC6, 0x45, 0xF8, 0x08, 0xC7, 0x45,
    0x08, 0x29, 0x00, 0x00, 0x00, 0xC7, 0x45, 0x0C, 0x2A, 0x00, 0x00, 0x00,
    0x48, 0x89, 0x7D, 0x20, 0xC6, 0x45, 0x38, 0x08, 0xC7, 0x45, 0x48, 0x2B,
    0x00, 0x00, 0x00, 0xC7, 0x45, 0x4C, 0x2C, 0x00, 0x00, 0x00, 0x66, 0x0F,
    0x7F, 0x45, 0x50,
};

// 44E25C: magic 25h / 26h, the 4040B0 test, +14h = 0C1h, +10h = 166h or -1.
constexpr std::uint8_t MagicRecordWitness[]{
    0xC7, 0x85, 0x88, 0x00, 0x00, 0x00, 0x25, 0x00, 0x00, 0x00, 0xC7, 0x85,
    0x8C, 0x00, 0x00, 0x00, 0x26, 0x00, 0x00, 0x00, 0xE8, 0x3B, 0x5E, 0xFB,
    0xFF, 0x66, 0x0F, 0x6F, 0x05, 0x93, 0xCA, 0x8C, 0x01, 0x84, 0xC0, 0x48,
    0x8D, 0x05, 0xF6, 0xC8, 0x8C, 0x01, 0xC7, 0x85, 0x94, 0x00, 0x00, 0x00,
    0xC1, 0x00, 0x00, 0x00, 0xB9, 0xFF, 0xFF, 0xFF, 0xFF, 0x48, 0x89, 0x85,
    0xB0, 0x00, 0x00, 0x00, 0xBA, 0x66, 0x01, 0x00, 0x00, 0xC7, 0x85, 0x98,
    0x00, 0x00, 0x00, 0x92, 0x00, 0x00, 0x00, 0x0F, 0x45, 0xCA, 0xC7, 0x85,
    0x9C, 0x00, 0x00, 0x00, 0x93, 0x00, 0x00, 0x00, 0x89, 0x8D, 0x90, 0x00,
    0x00, 0x00,
};

// 44E317: the poison constant load.
constexpr std::uint8_t PoisonLoadWitness[]{
    0x66, 0x0F, 0x6F, 0x05, 0x01, 0xCA, 0x8C, 0x01,
};

// 44E3C9: stored into the poison record's +10h.
constexpr std::uint8_t PoisonStoreWitness[]{
    0x66, 0x0F, 0x7F, 0x85, 0x90, 0x01, 0x00, 0x00,
};

// 44E440: poison 2Dh / 2Eh.
constexpr std::uint8_t PoisonIdsWitness[]{
    0xC7, 0x85, 0x88, 0x01, 0x00, 0x00, 0x2D, 0x00, 0x00, 0x00, 0xC7, 0x85,
    0x8C, 0x01, 0x00, 0x00, 0x2E, 0x00, 0x00, 0x00,
};

// 1D1ACE0: fire 333 / 189, lightning 334 / 190, cold 335 / 187 (-enemy-res,
// Sunder, then two absorb stats each).
constexpr std::uint8_t ElementConstantsWitness[]{
    0x4D, 0x01, 0x00, 0x00, 0xBD, 0x00, 0x00, 0x00, 0x8E, 0x00, 0x00, 0x00,
    0x8F, 0x00, 0x00, 0x00, 0x4E, 0x01, 0x00, 0x00, 0xBE, 0x00, 0x00, 0x00,
    0x90, 0x00, 0x00, 0x00, 0x91, 0x00, 0x00, 0x00, 0x4F, 0x01, 0x00, 0x00,
    0xBB, 0x00, 0x00, 0x00, 0x94, 0x00, 0x00, 0x00, 0x95, 0x00, 0x00, 0x00,
};

// 1D1AD20: poison 336 / 191.
constexpr std::uint8_t PoisonConstantsWitness[]{
    0x50, 0x01, 0x00, 0x00, 0xBF, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
};

// 39DE8E: monstats.txt column "Align" at descriptor rbp+340h.
constexpr std::uint8_t AlignNameWitness[]{
    0x48, 0x8D, 0x05, 0x67, 0x85, 0x96, 0x01, 0x48, 0x89, 0x85, 0x40, 0x03,
    0x00, 0x00,
};

// 39E035: its type 4 (byte) and record offset 87h.
constexpr std::uint8_t AlignColumnWitness[]{
    0x48, 0xC7, 0x85, 0x48, 0x03, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x48,
    0xC7, 0x85, 0x50, 0x03, 0x00, 0x00, 0x87, 0x00, 0x00, 0x00,
};

// ---------------------------------------------------------------------------
// Native layout
// ---------------------------------------------------------------------------

constexpr std::size_t UnitStatListOffset       = 0x88;   // 34B870
constexpr std::size_t StatListFlagsOffset      = 0x1C;   // 2F5991, 2F7AB9
constexpr std::size_t StatListStateOffset      = 0x20;   // 2F59C7
constexpr std::size_t StatListFirstChildOffset = 0x90;   // 2F59BB
constexpr std::size_t StatListNextChildOffset  = 0x68;   // 2F59CC
constexpr std::size_t GameFrameOffset          = 0x170;  // 42EA8B
constexpr std::size_t MonStatsAlignOffset      = 0x87;   // 39E040

constexpr std::uint32_t ExtendedListFlag = 0x80000000U;  // sign bit, 2F5991
constexpr std::uint32_t TimedListFlag    = 0x00000002U;  // 2F7AB9
constexpr std::uint32_t CustomTimerFlag  = 0x00008000U;  // D2RCore expiry
constexpr std::uint32_t UnmergedListFlag = 0x00002000U;  // D2RCore attach

// The resistance context and record, as 0x44DF10 lays them out.
constexpr std::size_t ContextSize           = 0x40;
constexpr std::size_t ContextGameOffset     = 0x00;
constexpr std::size_t ContextAttackerOffset = 0x10;
constexpr std::size_t ContextDefenderOffset = 0x18;
constexpr std::size_t ContextUncappedOffset = 0x24;
constexpr std::size_t RecordSize            = 0x40;
constexpr std::size_t RecordDamageOffset    = 0x00;
constexpr std::size_t RecordResistOffset    = 0x08;
constexpr std::size_t RecordMaxOffset       = 0x0C;
constexpr std::size_t RecordPierceOffset    = 0x10;
constexpr std::size_t RecordSunderOffset    = 0x14;
constexpr std::size_t RecordAbsorbOffset    = 0x18;

constexpr std::uint32_t MonsterUnitType       = 1;
constexpr std::int32_t  HitpointRegenStatId   = 74;
constexpr std::int32_t  PhysicalResistStat    = 36;
constexpr std::uint8_t  JmpOpcode             = 0xE9;   // Summon Resist Cap's gate
constexpr std::uint8_t  AlignAlly             = 1;

constexpr std::size_t   MaximumDrainsPerTick  = 64;
constexpr std::size_t   MaximumSourcesPerTick = 16;
constexpr std::uint64_t MaximumClearLogLines  = 100;
constexpr std::uint64_t MaximumDebugLines     = 2000;

struct DamageType {
    const char*  name;
    std::int32_t resistStat;
    std::int32_t maxStat;
    std::int32_t pierceStat;
    bool         pierceOnlyInRotw;
    std::int32_t sunderStat;
};

// 44E14D..44E454 with the constants at 1D1ACE0 / 1D1AD20.
constexpr std::array<DamageType, 6> DamageTypes{ {
    { "physical",  36, -1, 366, true,  192 },
    { "magic",     37, 38, 358, true,  193 },
    { "fire",      39, 40, 333, false, 189 },
    { "lightning", 41, 42, 334, false, 190 },
    { "cold",      43, 44, 335, false, 187 },
    { "poison",    45, 46, 336, false, 191 },
} };
constexpr std::size_t DamageTypeCount = DamageTypes.size();

// Every narrow argument goes out zero-extended to 64 bits: D2RCore builds
// its lookup keys from full registers.
using RegenTickFn      = std::uint64_t(__fastcall*)(void* game, void* unit,
                             std::uint64_t argument3, std::uint64_t argument4) noexcept;
using ExpiryEventFn    = void(__fastcall*)(void* game, void* unit);
using SetExpireFrameFn = void(__fastcall*)(void* statList, std::uint64_t frame);
using GetUnitStatFn    = std::int32_t(__fastcall*)(void* unit, std::uint64_t statId,
                             std::uint64_t layer);
using ReadListStatFn   = std::int32_t(__fastcall*)(std::uint64_t dataContext, void* statList,
                             std::uint64_t statId, std::uint64_t layer);
using GetDataContextFn = std::uint8_t(__fastcall*)(void* unit);
using GetUnitTypeFn    = std::uint32_t(__fastcall*)(void* unit);
using GetClassIdFn     = std::uint32_t(__fastcall*)(void* unit);
using ListOwnerFn      = std::uint32_t(__fastcall*)(void* statList);
using GetServerUnitFn  = void*(__fastcall*)(void* game, std::uint64_t type, std::uint64_t id);
using ResistStepFn     = std::int32_t(__fastcall*)(void* context, void* record,
                             std::uint64_t resistance);
using CheckStateFn     = std::int32_t(__fastcall*)(void* unit, std::uint64_t state);
using UnitTestFn       = std::uint32_t(__fastcall*)(void* unit);
using MonStatsRecordFn = const std::uint8_t*(__fastcall*)(std::uint64_t dataContext,
                             std::uint64_t classId);
using RotwGameFn       = std::uint8_t(__fastcall*)(void* game);

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

constexpr std::size_t MaximumConfigBytes = 32'768;

struct Config {
    bool enabled = true;
    std::array<bool, DamageTypeCount> immuneTo{ true, true, true, true, true, true };
    bool logClears = true;
    bool debug = false;
};

constexpr char DefaultConfigToml[] =
    "# Immunity Clears DoTs\n"
    "#\n"
    "# When a monster becomes immune to every damage type, the life drains\n"
    "# already running on it end on the spot: poison, bleed, burning, and any\n"
    "# other timed effect that drains life through negative life regeneration\n"
    "# (itemstatcost.txt hpregen, stat 74). Once applied, those drains are no\n"
    "# longer poison or physical damage, only negative regeneration, so without\n"
    "# this plugin they keep hurting a boss through its invulnerable phase.\n"
    "#\n"
    "# How immunity is judged\n"
    "#   - Every drain remembers the unit that applied it (the same record the\n"
    "#     game uses to credit a poison kill). Immunity is judged against that\n"
    "#     unit, exactly the way the game's damage code judges a hit from it:\n"
    "#     the monster's resistance, then that unit's Sunder, then its -enemy\n"
    "#     resistance, then the Sanctuary rule (physical counts as 0 against an\n"
    "#     undead monster while the unit has Sanctuary on it).\n"
    "#   - A drain ends when, against its own source, every damage type listed in\n"
    "#     immune_to is at 100 or more. 100 is the game's own immunity line: at\n"
    "#     100 resistance a hit of that type deals no damage.\n"
    "#   - So a boss whose immunities a player breaks with Sunder keeps taking\n"
    "#     that player's poison and bleed. When the boss turns invulnerable and\n"
    "#     even the sundered values reach 100, the drains end.\n"
    "#   - If the unit that applied a drain is no longer in the game, its Sunder\n"
    "#     cannot be known, so that drain is left to run out on its own.\n"
    "#   - The resistance floor, the immunity line (100), the cap ceiling (95) and\n"
    "#     the Sanctuary state are read from the running game, so static patches\n"
    "#     that change them, like ESR's -1000 resistance floor, are followed.\n"
    "#   - Hirelings are capped like players by the game, and allied monsters\n"
    "#     (monstats.txt Align 1) are capped while the Summon Resist Cap plugin is\n"
    "#     active, so neither is ever treated as immune. Players are never\n"
    "#     touched either: their caps keep them below 100.\n"
    "#\n"
    "# How a drain ends\n"
    "#   - Each drain that qualifies gets the current frame as its end frame, and\n"
    "#     the game's own state expiry runs for the monster: the same thing that\n"
    "#     happens when a poison runs out by itself. The state goes away, its\n"
    "#     overlay clears and players see it end.\n"
    "#   - This happens right before the monster's life regeneration for that\n"
    "#     frame, so no life is lost to a drain that has ended.\n"
    "#   - Any timed effect with negative life regeneration counts, so poison,\n"
    "#     bleed and burning need no state ids here. Effects without negative\n"
    "#     life regeneration (curses, auras, buffs) are left alone.\n"
    "#\n"
    "# Console command: immunitydots (status, counters, and the last decision with\n"
    "# every effective resistance it used)\n"
    "\n"
    "[immunity_dot_clear]\n"
    "\n"
    "# Master switch.\n"
    "enabled = true\n"
    "\n"
    "# Damage types the monster must be immune to, against the drain's source,\n"
    "# before that drain ends. Any of: physical, magic, fire, lightning, cold,\n"
    "# poison. Remove one only if your bosses' invulnerable phase does not cover\n"
    "# that damage type. Write the list on one line.\n"
    "immune_to = [\"physical\", \"magic\", \"fire\", \"lightning\", \"cold\", \"poison\"]\n"
    "\n"
    "# Write one line to the plugin log for each monster whose drains are ended:\n"
    "# monster class id (monstats.txt row), how many drains ended, game frame.\n"
    "# Stops after 100 lines per session.\n"
    "log_clears = true\n"
    "\n"
    "# Write a line to the plugin log every time the decision for a drain changes:\n"
    "# the monster, the drain's state and life drain per frame, its source, the\n"
    "# effective value of every listed resistance against that source, and\n"
    "# whether the drain keeps running or ends. Stops after 2000 lines per session.\n"
    "debug = false\n";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

enum class HookState : std::uint8_t {
    NotLoaded,
    DisabledByConfig,
    Armed,
    Failed,
};

const D2RL::PluginContext* Context{};
std::uintptr_t             Base{};
Config                     Settings{};
HookState                  State{ HookState::NotLoaded };

RegenTickFn      OriginalRegenTick{};
ExpiryEventFn    RunExpiryEvent{};
SetExpireFrameFn SetExpireFrame{};
GetUnitStatFn    GetUnitStat{};
ReadListStatFn   ReadListStat{};
GetDataContextFn GetDataContext{};
GetUnitTypeFn    GetUnitType{};
GetClassIdFn     GetClassId{};
ListOwnerFn      GetListOwnerId{};
ListOwnerFn      GetListOwnerType{};
GetServerUnitFn  GetServerUnit{};
ResistStepFn     ApplySunder{};
ResistStepFn     ApplyPierce{};
CheckStateFn     CheckState{};
UnitTestFn       IsUndead{};
UnitTestFn       IsHireling{};
MonStatsRecordFn GetMonStatsRecord{};
RotwGameFn       IsRotwGame{};

std::atomic<std::uint64_t> MonstersCleared{};
std::atomic<std::uint64_t> DrainsEnded{};
std::atomic<std::uint64_t> DrainsLeft{};
std::atomic<std::uint64_t> ClearLogLines{};
std::atomic<std::uint64_t> DebugLines{};
std::atomic<bool>          LeftoverWarned{};
std::atomic<bool>          CeilingWarned{};

// The last decision, for the console command.
struct Decision {
    bool          valid = false;
    std::uint32_t monsterClass = 0;
    std::int32_t  frame = 0;
    std::int32_t  state = 0;
    std::int32_t  regen = 0;
    std::uint32_t sourceType = 0;
    std::uint32_t sourceId = 0;
    bool          sourceFound = false;
    bool          ends = false;
    std::uint32_t evaluatedMask = 0;
    std::array<std::int32_t, DamageTypeCount> resist{};
};
std::mutex DecisionLock;
Decision   LastDecision{};

// What debug already logged per drain list, so only changes are logged.
struct DebugMemory {
    const void*   list = nullptr;
    std::uint64_t signature = 0;
};
std::array<DebugMemory, 128> DebugMemories{};
std::size_t                  DebugNextSlot = 0;

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

auto ParseBool(std::string_view value, bool& out) noexcept -> bool {
    if (value == "true") { out = true; return true; }
    if (value == "false") { out = false; return true; }
    return false;
}

auto EqualsIgnoringCase(std::string_view left, std::string_view right) noexcept -> bool {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        char a = left[index];
        char b = right[index];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

// ["physical", "fire"]: one to six damage type names on one line, each in
// double or single quotes. A trailing comma is allowed, as in TOML.
auto ParseDamageTypes(std::string_view value, std::array<bool, DamageTypeCount>& out) noexcept -> bool {
    if (value.size() < 2 || value.front() != '[' || value.back() != ']') return false;
    std::string_view inner = Trim(value.substr(1, value.size() - 2));
    std::array<bool, DamageTypeCount> parsed{};
    bool any = false;
    while (!inner.empty()) {
        const char quote = inner.front();
        if (quote != '"' && quote != '\'') return false;
        const std::size_t close = inner.find(quote, 1);
        if (close == std::string_view::npos) return false;
        const std::string_view name = inner.substr(1, close - 1);
        bool known = false;
        for (std::size_t index = 0; index < DamageTypeCount; ++index) {
            if (EqualsIgnoringCase(name, DamageTypes[index].name)) {
                parsed[index] = true;
                known = true;
            }
        }
        if (!known) return false;
        any = true;
        inner = Trim(inner.substr(close + 1));
        if (inner.empty()) break;
        if (inner.front() != ',') return false;
        inner = Trim(inner.substr(1));
    }
    if (!any) return false;
    out = parsed;
    return true;
}

// The value ends at the first # outside quotes.
auto StripComment(std::string_view line) noexcept -> std::string_view {
    char quote = 0;
    for (std::size_t index = 0; index < line.size(); ++index) {
        const char character = line[index];
        if (quote != 0) {
            if (character == quote) quote = 0;
        } else if (character == '"' || character == '\'') {
            quote = character;
        } else if (character == '#') {
            return line.substr(0, index);
        }
    }
    return line;
}

std::string ConfigProblems;

void NoteProblem(const char* text) noexcept {
    ConfigProblems += text;
    ConfigProblems += ' ';
}

void ApplyConfigLine(std::string_view section, std::string_view key, std::string_view value) noexcept {
    if (section != "immunity_dot_clear") return;
    if (key == "enabled") {
        if (!ParseBool(value, Settings.enabled)) {
            NoteProblem("immunity_dot_clear.enabled must be true or false; using true.");
        }
    } else if (key == "immune_to") {
        if (!ParseDamageTypes(value, Settings.immuneTo)) {
            NoteProblem("immunity_dot_clear.immune_to must be a one-line list of physical, magic, "
                        "fire, lightning, cold and poison; using all six.");
        }
    } else if (key == "log_clears") {
        if (!ParseBool(value, Settings.logClears)) {
            NoteProblem("immunity_dot_clear.log_clears must be true or false; using true.");
        }
    } else if (key == "debug") {
        if (!ParseBool(value, Settings.debug)) {
            NoteProblem("immunity_dot_clear.debug must be true or false; using false.");
        }
    } else if (key == "resistance_stats") {
        NoteProblem("immunity_dot_clear.resistance_stats was replaced by immune_to in 1.1.0 and "
                    "is ignored; replace the config file with the current one.");
    }
}

void ParseConfig(std::string_view text) noexcept {
    std::string_view section;
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        const std::size_t breakAt = text.find('\n', cursor);
        const std::size_t end = breakAt == std::string_view::npos ? text.size() : breakAt;
        const std::string_view line = Trim(StripComment(text.substr(cursor, end - cursor)));
        cursor = end + 1;
        if (line.empty()) continue;
        if (line.front() == '[') {
            if (line.back() == ']') section = Trim(line.substr(1, line.size() - 2));
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) continue;
        ApplyConfigLine(section, Trim(line.substr(0, equals)), Trim(line.substr(equals + 1)));
    }
}

void ReadConfiguration() noexcept {
    if (!Context->EnsureConfig(DefaultConfigToml)) {
        Context->LogWarn("ImmunityDotClear: config file could not be created; using defaults.");
    } else {
        std::string buffer(MaximumConfigBytes, '\0');
        if (Context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size()), nullptr)) {
            buffer.resize(std::strlen(buffer.c_str()));
            ParseConfig(buffer);
        } else {
            Context->LogWarn("ImmunityDotClear: config file could not be read; using defaults.");
        }
    }
    if (!ConfigProblems.empty()) {
        D2RL::LogWarnF(Context, "ImmunityDotClear: %s", ConfigProblems.c_str());
    }
}

auto FormatImmuneTo(char* output, std::size_t capacity) noexcept -> const char* {
    std::size_t used = 0;
    output[0] = '\0';
    for (std::size_t index = 0; index < DamageTypeCount && used < capacity; ++index) {
        if (!Settings.immuneTo[index]) continue;
        const int written = std::snprintf(output + used, capacity - used, "%s%s",
            used == 0 ? "" : ", ", DamageTypes[index].name);
        if (written < 0) break;
        used += static_cast<std::size_t>(written);
    }
    return output;
}

// ---------------------------------------------------------------------------
// Memory helpers
// ---------------------------------------------------------------------------

auto ReadPointer(const void* base, std::size_t offset) noexcept -> void* {
    void* value = nullptr;
    std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(value));
    return value;
}

auto ReadInt32(const void* base, std::size_t offset) noexcept -> std::int32_t {
    std::int32_t value = 0;
    std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(value));
    return value;
}

template <class T>
void Write(void* base, std::size_t offset, T value) noexcept {
    std::memcpy(static_cast<std::uint8_t*>(base) + offset, &value, sizeof(value));
}

auto AsArgument(std::int32_t value) noexcept -> std::uint64_t {
    return static_cast<std::uint64_t>(static_cast<std::uint32_t>(value));
}

template <class Function>
auto At(std::uint64_t rva) noexcept -> Function {
    return reinterpret_cast<Function>(Base + rva);
}

// ---------------------------------------------------------------------------
// The drains
// ---------------------------------------------------------------------------

struct Drain {
    void*        list = nullptr;
    std::int32_t regen = 0;
};

struct Drains {
    std::array<Drain, MaximumDrainsPerTick> items{};
    std::size_t                             count = 0;
};

auto IsExtendedList(const void* statList) noexcept -> bool {
    return statList != nullptr && ReadInt32(statList, StatListFlagsOffset) < 0;
}

// The merged chain, as 0x2F5940 walks it. At most MaximumDrainsPerTick are
// taken in one tick; any beyond that end on the next tick.
void CollectDrains(const void* statList, std::uint64_t dataContext, Drains& drains) noexcept {
    drains.count = 0;
    for (void* child = ReadPointer(statList, StatListFirstChildOffset); child != nullptr;
            child = ReadPointer(child, StatListNextChildOffset)) {
        const auto flags = static_cast<std::uint32_t>(ReadInt32(child, StatListFlagsOffset));
        if ((flags & ExtendedListFlag) != 0) continue;
        if ((flags & TimedListFlag) == 0) continue;
        if ((flags & (CustomTimerFlag | UnmergedListFlag)) != 0) continue;
        const std::int32_t regen = ReadListStat(dataContext, child, AsArgument(HitpointRegenStatId), 0);
        if (regen >= 0) continue;
        drains.items[drains.count++] = { child, regen };
        if (drains.count == drains.items.size()) return;
    }
}

// How many of the given lists are still attached afterwards.
auto CountStillAttached(const void* statList, const Drains& drains) noexcept -> std::size_t {
    std::size_t still = 0;
    for (void* child = ReadPointer(statList, StatListFirstChildOffset); child != nullptr;
            child = ReadPointer(child, StatListNextChildOffset)) {
        for (std::size_t index = 0; index < drains.count; ++index) {
            if (drains.items[index].list == child) {
                ++still;
                break;
            }
        }
    }
    return still;
}

// ---------------------------------------------------------------------------
// Immunity, the way 0x4523E0 judges a hit
// ---------------------------------------------------------------------------

// Hirelings are capped like players by the game itself (context+24h is 0),
// and allied monsters are capped by the Summon Resist Cap plugin while its
// jump sits on the cap gate. A capped defender stays at 95 or below.
auto IsCapped(void* monster, std::uint64_t dataContext) noexcept -> bool {
    if (IsHireling(monster) != 0) return true;
    std::uint8_t gate = 0;
    std::memcpy(&gate, reinterpret_cast<const void*>(Base + ResolverCapGateRva), sizeof(gate));
    if (gate != JmpOpcode) return false;
    const std::uint8_t* record = GetMonStatsRecord(dataContext, GetClassId(monster));
    return record != nullptr && record[MonStatsAlignOffset] == AlignAlly;
}

// The four values the resolver takes from its own instructions, read the way
// it reads them: from the running code, after any static patch.
struct LiveRules {
    std::int32_t floor = 0;      // 4524C5
    std::int32_t ceiling = 0;    // 4524DE
    std::int32_t sanctuary = 0;  // 452504
    std::int32_t line = 0;       // 452548
};

auto ReadCodeImm32(std::uint64_t rva) noexcept -> std::int32_t {
    std::int32_t value = 0;
    std::memcpy(&value, reinterpret_cast<const void*>(Base + rva), sizeof(value));
    return value;
}

auto ReadLiveRules() noexcept -> LiveRules {
    LiveRules rules{};
    rules.floor     = ReadCodeImm32(FloorImmRva);
    rules.ceiling   = ReadCodeImm32(CeilingImmRva);
    rules.sanctuary = ReadCodeImm32(SanctuaryStateImmRva);
    rules.line      = ReadCodeImm32(ImmunityLineImmRva);
    return rules;
}

auto EffectiveResistance(void* game, void* monster, void* source, const DamageType& type,
        bool rotw, const LiveRules& rules) noexcept -> std::int32_t {
    alignas(16) std::uint8_t context[ContextSize]{};
    alignas(16) std::uint8_t record[RecordSize]{};
    std::int32_t damage = 1;
    Write<void*>(context, ContextGameOffset, game);
    Write<void*>(context, ContextAttackerOffset, source);
    Write<void*>(context, ContextDefenderOffset, monster);
    Write<std::uint32_t>(context, ContextUncappedOffset, 1);
    Write<void*>(record, RecordDamageOffset, &damage);
    Write<std::int32_t>(record, RecordResistOffset, type.resistStat);
    Write<std::int32_t>(record, RecordMaxOffset, type.maxStat);
    Write<std::int32_t>(record, RecordPierceOffset,
        type.pierceOnlyInRotw && !rotw ? -1 : type.pierceStat);
    Write<std::int32_t>(record, RecordSunderOffset, type.sunderStat);
    Write<std::int64_t>(record, RecordAbsorbOffset, -1);

    std::int32_t resistance = GetUnitStat(monster, AsArgument(type.resistStat), 0);
    resistance = ApplySunder(context, record, AsArgument(resistance));
    resistance = ApplyPierce(context, record, AsArgument(resistance));
    if (resistance > 0) {
        if (type.resistStat == PhysicalResistStat && CheckState(source, AsArgument(rules.sanctuary)) != 0
                && IsUndead(monster) != 0) {
            resistance = 0;
        }
    } else if (resistance < rules.floor) {
        resistance = rules.floor;
    }
    return resistance;
}

struct Verdict {
    bool          immune = false;
    std::uint32_t evaluatedMask = 0;
    std::array<std::int32_t, DamageTypeCount> resist{};
};

// Immune to every listed damage type from this source. Stops at the first
// one under 100 unless every value is wanted for the debug log.
auto Judge(void* game, void* monster, void* source, bool everyValue, const LiveRules& rules) noexcept
        -> Verdict {
    Verdict verdict{};
    const bool rotw = IsRotwGame(game) != 0;
    bool immune = true;
    bool any = false;
    for (std::size_t index = 0; index < DamageTypeCount; ++index) {
        if (!Settings.immuneTo[index]) continue;
        any = true;
        const std::int32_t value = EffectiveResistance(game, monster, source, DamageTypes[index], rotw, rules);
        verdict.resist[index] = value;
        verdict.evaluatedMask |= 1U << index;
        if (value < rules.line) {
            immune = false;
            if (!everyValue) break;
        }
    }
    verdict.immune = any && immune;
    return verdict;
}

struct SourceMemo {
    void*   source = nullptr;
    Verdict verdict{};
};

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

auto FormatResistances(const Decision& decision, char* output, std::size_t capacity) noexcept -> const char* {
    std::size_t used = 0;
    output[0] = '\0';
    for (std::size_t index = 0; index < DamageTypeCount && used < capacity; ++index) {
        if ((decision.evaluatedMask & (1U << index)) == 0) continue;
        const int written = std::snprintf(output + used, capacity - used, "%s%s %d",
            used == 0 ? "" : ", ", DamageTypes[index].name, decision.resist[index]);
        if (written < 0) break;
        used += static_cast<std::size_t>(written);
    }
    if (used == 0) std::snprintf(output, capacity, "not evaluated");
    return output;
}

auto FormatDecision(const Decision& decision, char* output, std::size_t capacity) noexcept -> const char* {
    if (!decision.valid) {
        std::snprintf(output, capacity, "none yet");
        return output;
    }
    char values[256];
    if (decision.sourceFound) {
        FormatResistances(decision, values, sizeof(values));
    } else {
        std::snprintf(values, sizeof(values), "source no longer in the game");
    }
    std::snprintf(output, capacity,
        "monster class %u at frame %d, drain state %d (%d per frame) from unit type %u id %u: %s -> %s",
        decision.monsterClass, decision.frame, decision.state, decision.regen, decision.sourceType,
        decision.sourceId, values, decision.ends ? "ends" : "keeps running");
    return output;
}

auto Signature(const Decision& decision) noexcept -> std::uint64_t {
    std::uint64_t hash = 1469598103934665603ULL;
    auto mix = [&hash](std::uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ULL;
    };
    mix(decision.sourceType);
    mix(decision.sourceId);
    mix(decision.sourceFound ? 1 : 0);
    mix(decision.ends ? 1 : 0);
    mix(decision.evaluatedMask);
    for (const std::int32_t value : decision.resist) mix(static_cast<std::uint32_t>(value));
    return hash;
}

void Trace(const void* list, const Decision& decision) noexcept {
    {
        std::lock_guard guard(DecisionLock);
        LastDecision = decision;
    }
    if (!Settings.debug) return;

    const std::uint64_t signature = Signature(decision);
    DebugMemory* memory = nullptr;
    for (DebugMemory& slot : DebugMemories) {
        if (slot.list == list) {
            memory = &slot;
            break;
        }
    }
    if (memory != nullptr && memory->signature == signature) return;
    if (memory == nullptr) {
        memory = &DebugMemories[DebugNextSlot];
        DebugNextSlot = (DebugNextSlot + 1) % DebugMemories.size();
        memory->list = list;
    }
    memory->signature = signature;

    if (DebugLines.fetch_add(1, std::memory_order_relaxed) >= MaximumDebugLines) return;
    char line[512];
    FormatDecision(decision, line, sizeof(line));
    D2RL::LogInfoF(Context, "ImmunityDotClear debug: %s.", line);
}

void Report(void* unit, std::size_t ended, std::size_t left, std::int32_t frame) noexcept {
    const std::uint32_t classId = GetClassId(unit);
    MonstersCleared.fetch_add(1, std::memory_order_relaxed);
    DrainsEnded.fetch_add(ended, std::memory_order_relaxed);
    DrainsLeft.fetch_add(left, std::memory_order_relaxed);

    if (Settings.logClears
            && ClearLogLines.fetch_add(1, std::memory_order_relaxed) < MaximumClearLogLines) {
        D2RL::LogInfoF(Context,
            "ImmunityDotClear: monster class %u is immune to every listed damage type from the "
            "source of %u life drain(s); ended them at frame %d.",
            classId, static_cast<unsigned>(ended), frame);
    }
    if (left != 0 && !LeftoverWarned.exchange(true, std::memory_order_relaxed)) {
        D2RL::LogWarnF(Context,
            "ImmunityDotClear: %u life drain(s) on monster class %u were still there after the "
            "expiry event at frame %d. D2RCore's stat expiry no longer removes them the way "
            "D2RLoader 1.3.1 did; the plugin needs re-checking against this loader.",
            static_cast<unsigned>(left), classId, frame);
    }
}

// ---------------------------------------------------------------------------
// The tick
// ---------------------------------------------------------------------------

void EndDrainsIfImmune(void* game, void* unit) noexcept {
    if (game == nullptr || unit == nullptr) return;
    if (GetUnitType(unit) != MonsterUnitType) return;

    void* const statList = ReadPointer(unit, UnitStatListOffset);
    if (!IsExtendedList(statList)) return;

    const std::uint64_t dataContext = GetDataContext(unit);
    Drains drains{};
    CollectDrains(statList, dataContext, drains);
    if (drains.count == 0) return;

    const LiveRules rules = ReadLiveRules();
    if (IsCapped(unit, dataContext)) {
        if (rules.ceiling >= rules.line && !CeilingWarned.exchange(true, std::memory_order_relaxed)) {
            D2RL::LogWarnF(Context,
                "ImmunityDotClear: the resistance ceiling in this game is %d, at or above the immunity line "
                "%d, so capped units (hirelings, Summon Resist Cap allies) can be immune. The plugin does not "
                "judge capped units; their drains run out as before.",
                rules.ceiling, rules.line);
        }
        return;
    }

    const std::int32_t frame = ReadInt32(game, GameFrameOffset);
    if (frame <= 0) return;

    std::array<SourceMemo, MaximumSourcesPerTick> memos{};
    std::size_t memoCount = 0;
    Drains ending{};
    const std::uint32_t monsterClass = GetClassId(unit);

    for (std::size_t index = 0; index < drains.count; ++index) {
        const Drain& drain = drains.items[index];
        Decision decision{};
        decision.valid        = true;
        decision.monsterClass = monsterClass;
        decision.frame        = frame;
        decision.state        = ReadInt32(drain.list, StatListStateOffset);
        decision.regen        = drain.regen;
        decision.sourceType   = GetListOwnerType(drain.list);
        decision.sourceId     = GetListOwnerId(drain.list);

        void* const source = GetServerUnit(game, decision.sourceType, decision.sourceId);
        decision.sourceFound = source != nullptr;
        if (source != nullptr) {
            const SourceMemo* known = nullptr;
            for (std::size_t memo = 0; memo < memoCount; ++memo) {
                if (memos[memo].source == source) {
                    known = &memos[memo];
                    break;
                }
            }
            Verdict verdict{};
            if (known != nullptr) {
                verdict = known->verdict;
            } else {
                verdict = Judge(game, unit, source, Settings.debug, rules);
                if (memoCount < memos.size()) memos[memoCount++] = { source, verdict };
            }
            decision.ends          = verdict.immune;
            decision.evaluatedMask = verdict.evaluatedMask;
            decision.resist        = verdict.resist;
        }
        Trace(drain.list, decision);
        if (decision.ends) ending.items[ending.count++] = drain;
    }
    if (ending.count == 0) return;

    for (std::size_t index = 0; index < ending.count; ++index) {
        SetExpireFrame(ending.items[index].list, AsArgument(frame));
    }
    RunExpiryEvent(game, unit);

    void* const statListAfter = ReadPointer(unit, UnitStatListOffset);
    const std::size_t left = IsExtendedList(statListAfter) ? CountStillAttached(statListAfter, ending) : 0;
    Report(unit, ending.count - left, left, frame);
}

std::uint64_t __fastcall HookedRegenTick(void* game, void* unit, std::uint64_t argument3,
        std::uint64_t argument4) noexcept {
    EndDrainsIfImmune(game, unit);
    return OriginalRegenTick(game, unit, argument3, argument4);
}

// ---------------------------------------------------------------------------
// Verification
// ---------------------------------------------------------------------------

auto Verify(std::uint64_t rva, const std::uint8_t* expected, std::size_t size,
        const char* label) noexcept -> bool {
    if (Context->CheckExpectedBytes(rva, expected, static_cast<std::uint32_t>(size))) return true;
    D2RL::LogErrorF(Context,
        "ImmunityDotClear: %s at 0x%llX does not match the verified D2R image, or another "
        "plugin already owns it.",
        label, static_cast<unsigned long long>(rva));
    return false;
}

auto VerifyThunk(std::uint64_t rva, const std::uint8_t* padding, std::size_t paddingSize,
        const std::uint8_t* body, std::size_t bodySize, std::uint64_t bodyOffset,
        const char* label) noexcept -> bool {
    return Verify(rva, ThunkJumpWitness, sizeof(ThunkJumpWitness), label)
        && Verify(rva + ThunkPaddingOffset, padding, paddingSize, label)
        && Verify(rva + bodyOffset, body, bodySize, label);
}

// The dispatcher witness proves the table address; the two entries prove
// which handlers are events 3 and 12 in the running game.
auto VerifyEventTable() noexcept -> bool {
    const std::uintptr_t table = Base + MonsterEventTableRva;
    std::uintptr_t regenHandler = 0;
    std::uintptr_t expiryHandler = 0;
    std::memcpy(&regenHandler, reinterpret_cast<const void*>(table + MonsterStatRegenEvent * 8),
        sizeof(regenHandler));
    std::memcpy(&expiryHandler, reinterpret_cast<const void*>(table + StatListExpiryEvent * 8),
        sizeof(expiryHandler));
    if (regenHandler == Base + MonsterStatRegenRva && expiryHandler == Base + StatListExpiryEventRva) {
        return true;
    }
    D2RL::LogErrorF(Context,
        "ImmunityDotClear: the monster event table no longer maps event 3 to 0x448C00 and "
        "event 12 to 0x42EA80 (found 0x%llX and 0x%llX).",
        static_cast<unsigned long long>(regenHandler - Base),
        static_cast<unsigned long long>(expiryHandler - Base));
    return false;
}

#define IDC_VERIFY(rva, witness, label) Verify(rva, witness, sizeof(witness), label)

auto VerifyTickContract() noexcept -> bool {
    return IDC_VERIFY(MonsterStatRegenRva, RegenEntryWitness, "the monster regeneration tick entry")
        && IDC_VERIFY(MonsterStatRegenArgumentsRva, RegenArgumentsWitness,
                "the monster regeneration tick arguments")
        && IDC_VERIFY(MonsterEventBaseRva, MonsterEventBaseWitness, "the monster event dispatcher base")
        && IDC_VERIFY(MonsterEventDispatchRva, MonsterEventDispatchWitness,
                "the monster event dispatcher call")
        && VerifyEventTable()
        && IDC_VERIFY(StatListExpiryEventRva, ExpiryEventWitness, "the stat expiry event")
        && VerifyThunk(ExpireUnitStatListsRva, ExpireThunkPaddingWitness,
                sizeof(ExpireThunkPaddingWitness), ExpireThunkBodyWitness,
                sizeof(ExpireThunkBodyWitness), ExpireThunkBodyOffset, "the stat expiry thunk")
        && IDC_VERIFY(SetStatListExpireFrameRva, SetExpireFrameWitness, "the stat list end frame setter")
        && VerifyThunk(GetUnitStatRva, UnitStatThunkPaddingWitness,
                sizeof(UnitStatThunkPaddingWitness), UnitStatThunkBodyWitness,
                sizeof(UnitStatThunkBodyWitness), PaddedThunkBodyOffset,
                "the unit stat reader thunk")
        && VerifyThunk(ReadListStatRva, ListStatThunkPaddingWitness,
                sizeof(ListStatThunkPaddingWitness), ListStatThunkBodyWitness,
                sizeof(ListStatThunkBodyWitness), PaddedThunkBodyOffset,
                "the list stat reader thunk")
        && IDC_VERIFY(GetDataContextRva, GetDataContextWitness, "the unit data context getter")
        && IDC_VERIFY(GetUnitTypeRva, GetUnitTypeWitness, "the unit type getter")
        && IDC_VERIFY(GetClassIdRva, GetClassIdWitness, "the unit class id getter")
        && IDC_VERIFY(GetStatListExRva, GetStatListExWitness, "the unit stat list getter")
        && IDC_VERIFY(StateListExtendedGateRva, StateListExtendedGateWitness,
                "the extended stat list test")
        && IDC_VERIFY(StateListChainRva, StateListChainWitness, "the stat list child chain");
}

auto VerifyImmunityContract() noexcept -> bool {
    return IDC_VERIFY(ListOwnerGuidRva, ListOwnerWitness, "the stat list source getters")
        && IDC_VERIFY(GetServerUnitRva, ServerUnitWitness, "the server unit lookup")
        && IDC_VERIFY(SunderRva, SunderWitness, "the Sunder step")
        && IDC_VERIFY(PierceRva, PierceWitness, "the -enemy resistance step")
        && IDC_VERIFY(ImmunitySwitchRva, ImmunitySwitchWitness, "the immunity floor switch")
        && IDC_VERIFY(ResolverStepsRva, ResolverStepsWitness, "the resistance steps")
        && IDC_VERIFY(ResolverSwitchRva, ResolverSwitchWitness, "the resistance floor")
        && IDC_VERIFY(ResolverSignRva, ResolverSignWitness, "the resistance sign test")
        && IDC_VERIFY(ResolverFloorTailRva, ResolverFloorTailWitness, "the resistance floor")
        && IDC_VERIFY(CeilingRva, CeilingOpcodeWitness, "the resistance ceiling")
        && IDC_VERIFY(CeilingTailRva, CeilingTailWitness, "the resistance ceiling")
        && IDC_VERIFY(SanctuaryHeadRva, SanctuaryHeadWitness, "the Sanctuary rule")
        && IDC_VERIFY(SanctuaryStateRva, SanctuaryOpcodeWitness, "the Sanctuary rule")
        && IDC_VERIFY(SanctuaryTailRva, SanctuaryTailWitness, "the Sanctuary rule")
        && IDC_VERIFY(ImmunityLineRva, ImmunityLineOpcodeWitness, "the resistance damage rule")
        && IDC_VERIFY(ImmunityLineTailRva, ImmunityLineTailWitness, "the resistance damage rule")
        && IDC_VERIFY(UncappedFlagRva, UncappedFlagWitness, "the uncapped defender flag")
        && IDC_VERIFY(ResistanceLoopRva, ResistanceLoopWitness, "the resistance record loop")
        && IDC_VERIFY(ResistanceLoopTailRva, ResistanceLoopTailWitness, "the resistance record stride")
        && IDC_VERIFY(IsHirelingRva, HirelingTestWitness, "the hireling test")
        && IDC_VERIFY(IsUndeadRva, UndeadTestWitness, "the undead test")
        && IDC_VERIFY(CheckStateRva, CheckStateWitness, "STATES_CheckState")
        && IDC_VERIFY(MonStatsRecordRva, MonStatsRecordWitness, "the monstats record getter")
        && IDC_VERIFY(RotwGameRva, RotwGameWitness, "the Reign of the Warlock test")
        && IDC_VERIFY(RecordsHeadRva, RecordsHeadWitness, "the physical record")
        && IDC_VERIFY(PhysicalSunderRva, PhysicalSunderWitness, "the physical Sunder stat")
        && IDC_VERIFY(ElementalIdsRva, ElementalIdsWitness, "the fire, lightning and cold records")
        && IDC_VERIFY(MagicRecordRva, MagicRecordWitness, "the magic record")
        && IDC_VERIFY(PoisonLoadRva, PoisonLoadWitness, "the poison constant load")
        && IDC_VERIFY(PoisonStoreRva, PoisonStoreWitness, "the poison constant store")
        && IDC_VERIFY(PoisonIdsRva, PoisonIdsWitness, "the poison record")
        && IDC_VERIFY(ElementConstantsRva, ElementConstantsWitness, "the elemental stat constants")
        && IDC_VERIFY(PoisonConstantsRva, PoisonConstantsWitness, "the poison stat constants")
        && IDC_VERIFY(AlignNameRva, AlignNameWitness, "the monstats Align column name")
        && IDC_VERIFY(AlignColumnRva, AlignColumnWitness, "the monstats Align column offset");
}

#undef IDC_VERIFY

void BindNatives() noexcept {
    RunExpiryEvent    = At<ExpiryEventFn>(StatListExpiryEventRva);
    SetExpireFrame    = At<SetExpireFrameFn>(SetStatListExpireFrameRva);
    GetUnitStat       = At<GetUnitStatFn>(GetUnitStatRva);
    ReadListStat      = At<ReadListStatFn>(ReadListStatRva);
    GetDataContext    = At<GetDataContextFn>(GetDataContextRva);
    GetUnitType       = At<GetUnitTypeFn>(GetUnitTypeRva);
    GetClassId        = At<GetClassIdFn>(GetClassIdRva);
    GetListOwnerId    = At<ListOwnerFn>(ListOwnerGuidRva);
    GetListOwnerType  = At<ListOwnerFn>(ListOwnerTypeRva);
    GetServerUnit     = At<GetServerUnitFn>(GetServerUnitRva);
    ApplySunder       = At<ResistStepFn>(SunderRva);
    ApplyPierce       = At<ResistStepFn>(PierceRva);
    CheckState        = At<CheckStateFn>(CheckStateRva);
    IsUndead          = At<UnitTestFn>(IsUndeadRva);
    IsHireling        = At<UnitTestFn>(IsHirelingRva);
    GetMonStatsRecord = At<MonStatsRecordFn>(MonStatsRecordRva);
    IsRotwGame        = At<RotwGameFn>(RotwGameRva);
}

auto InstallHook() noexcept -> bool {
    if (!Context->InstallInlineHook(MonsterStatRegenRva, RegenEntryWitness,
            static_cast<std::uint32_t>(sizeof(RegenEntryWitness)), &HookedRegenTick,
            &OriginalRegenTick)
            || OriginalRegenTick == nullptr) {
        OriginalRegenTick = nullptr;
        Context->LogError("ImmunityDotClear: the monster regeneration tick hook at 0x448C00 "
                          "could not be installed.");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Console command
// ---------------------------------------------------------------------------

auto StateName(HookState state) noexcept -> const char* {
    switch (state) {
    case HookState::DisabledByConfig: return "disabled by config";
    case HookState::Armed:            return "armed";
    case HookState::Failed:           return "FAILED, see log";
    default:                          return "not loaded";
    }
}

auto __cdecl StatusCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command,
        void*) noexcept -> D2RL::ConsoleCommandResult {
    if (command == nullptr || command->plugin == nullptr) return D2RL::ConsoleCommandResult::Failed;

    char immuneTo[128];
    FormatImmuneTo(immuneTo, sizeof(immuneTo));
    const LiveRules rules = ReadLiveRules();

    char message[768];
    std::snprintf(message, sizeof(message),
        "ImmunityDotClear 1.1.1: %s | immune at %d+ against the drain's source to: %s | floor %d, ceiling %d, "
        "Sanctuary state %d | debug %s | monsters cleared %llu, drains ended %llu, still there after expiry %llu",
        StateName(State), rules.line, immuneTo, rules.floor, rules.ceiling, rules.sanctuary,
        Settings.debug ? "on" : "off",
        static_cast<unsigned long long>(MonstersCleared.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(DrainsEnded.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(DrainsLeft.load(std::memory_order_relaxed)));
    command->plugin->WriteConsoleMessage(message);

    Decision decision{};
    {
        std::lock_guard guard(DecisionLock);
        decision = LastDecision;
    }
    char last[512];
    FormatDecision(decision, last, sizeof(last));
    std::snprintf(message, sizeof(message), "ImmunityDotClear last decision: %s", last);
    command->plugin->WriteConsoleMessage(message);
    return D2RL::ConsoleCommandResult::Handled;
}

void RegisterStatusCommand() noexcept {
    if (!Context->RegisterConsoleCommand("immunitydots", &StatusCommand,
            "Reports Immunity Clears DoTs status, counters and its last decision.")) {
        Context->LogWarn("ImmunityDotClear: the status console command was refused.");
    }
}

constexpr D2RL::PluginInfo Info{
    .infoSize = D2RL::PluginInfoSize,
    .abiVersion = D2RL_PLUGIN_ABI_VERSION,
    .id = "celestialrayone.immunity-dot-clear",
    .name = "Immunity Clears DoTs",
    .version = "1.1.1",
    .author = "CelestialRayOne",
    .description =
        "When a monster becomes immune to every damage type against a drain's source, Sunder "
        "included, that poison, bleed or other life drain ends.",
    .flags = D2RL::PluginFlags::Server | D2RL::PluginFlags::NativeHooks,
};

}  // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
    return &Info;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
    if (!D2RL::HasContext(context)) return false;
    Context = context;
    Base = context->exeBase;

    BindNatives();
    ReadConfiguration();

    char immuneTo[128];
    FormatImmuneTo(immuneTo, sizeof(immuneTo));

    if (!Settings.enabled) {
        State = HookState::DisabledByConfig;
        Context->LogInfo("ImmunityDotClear: disabled by config.");
        RegisterStatusCommand();
        return true;
    }

    if (!VerifyTickContract() || !VerifyImmunityContract() || !InstallHook()) {
        State = HookState::Failed;
        Context->LogError("ImmunityDotClear: nothing was installed. Refusing to load.");
        return false;
    }

    State = HookState::Armed;
    const LiveRules rules = ReadLiveRules();
    D2RL::LogInfoF(Context,
        "ImmunityDotClear 1.1.1: armed. A drain ends when the monster is at %d or more against the "
        "drain's source, Sunder included, in: %s. Live rules: floor %d, ceiling %d, Sanctuary state %d. "
        "Debug log %s.",
        rules.line, immuneTo, rules.floor, rules.ceiling, rules.sanctuary, Settings.debug ? "on" : "off");
    RegisterStatusCommand();
    return true;
}

}  // namespace CelestialRayOne::ImmunityDotClear
