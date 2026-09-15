// Smite
//
// Three independent Smite features, each switchable on its own in the TOML.
// A disabled feature installs nothing at all.
//
//   1. Multi-hit                   one cast lands N times, N from a
//                                  skills.txt calc column          (server)
//   2. Holy Shield display fix     the damage line reads a configurable
//                                  skill instead of vanilla 117    (client)
//   3. item_normaldamage display   the damage line includes the
//      fix                         "adds X damage" stat            (client)
//
// Features 2 and 3 are display only and change no damage. Because the plugin
// touches both the server do-func and the client description handler, its role
// is Shared.
//
// ===========================================================================
// NATIVE BACKGROUND
// ===========================================================================
// All of it read out of the live D2R 3.3 image under D2RLoader and
// disassembled before being relied on here.
//
// ---------------------------------------------------------------------------
// Server: the Smite do-func
// ---------------------------------------------------------------------------
//
//   0x238EA00  pSrvDoFunc table, entry N at base + 8N, index bound 190.
//              Dispatched by the skill do-handler at 0x43ACB0 through the
//              skills.txt srvdofunc column at skill-record offset 78.
//   0x564930   slot 150, Smite. __fastcall(game, unit, skillId, skillLevel)
//              -> 1 on a landed hit, 0 when nothing happened.
//
//              Identified by content, not by slot number alone: it resolves
//              the equipped shield, reads the shield's min and max damage
//              bytes at item-record offsets 271 and 272 and shifts them left
//              by 8 into 256ths, adds the Holy Shield bonus by reading state
//              101 off the caster and taking that state's own source skill
//              and level, evaluates calc1 as the damage percent and calc2 as
//              bonus damage, then applies:
//
//                0x564CDB  call 0x44B600   apply damage    (game, att, tgt, &dmg, 0x80)
//                0x564CF1  call 0x44B2B0   durability drain and the melee
//                                          event pair, domeleeattack and
//                                          attackedinmelee (game, att, tgt, 0, 1)
//                0x564CF6  mov r13d, 1     the success return
//
//   0x97790    SKILLS_GetSkillsTxtRecord(gameVersion, skillId), stride 748.
//              Its first call is GetDataTables at 0x300A90.
//   0x3B5160   SKILLS_EvaluateFormula(gameVersion, unit, poolOffset, skillId,
//              level), the Skills-pool evaluator. A blank .txt formula column
//              holds 0xFFFFFFFF, and the engine tests for that before calling
//              in (see the ToHit evaluator at 0x339040), so this plugin does
//              the same.
//   game+262   the game version byte, read the same way Smite itself reads it.
//
// ---------------------------------------------------------------------------
// skills.txt calc column offsets, D2R 3.3
// ---------------------------------------------------------------------------
//
//   Rebuilt from the .txt column binder 0x302380, which hands its descriptor
//   array to LoadExcelTable with a record size of 748. Each descriptor is 32
//   bytes: name at +0, type at +8, record offset at +16.
//
//     calc1  400    calc6  420
//     calc2  404    calc7  424
//     calc3  408    calc8  428
//     calc4  412    calc9  432
//     calc5  416    calc10 436
//
//   Cross-checked three ways rather than trusted from the binder alone: the
//   ToHit evaluator 0x339040 reads record[144..146], which is 0x240/0x244/
//   0x248, and the same reconstruction puts ToHit, LevToHit and ToHitCalc at
//   exactly those offsets; the reconstruction lands cltstopfunc at 0x2E8,
//   flush against the 0x2EC record size; and it puts seqnum at +0x33, which
//   is where the native sequence selector reads it.
//
// ---------------------------------------------------------------------------
// Why the whole do-func is repeated for multi-hit
// ---------------------------------------------------------------------------
//
//   The 2.4 build looped only the two calls at the tail and replayed one saved
//   copy of the damage struct. That cannot be carried across: the 3.3 damage
//   struct is about 0x180 bytes and carries two embedded small-buffer arenas,
//   a tagged pointer with a size, a capacity of 0x8000000000000010 and a
//   0xC0-byte inline buffer, which the function's own epilogue destructs.
//   Snapshotting and restoring across those would leak the moment an arena
//   spilled to the heap.
//
//   Repeating the do-func is the same feature with none of that exposure. It
//   needs no stack layout, no struct copy and no assembly. Every hit runs the
//   full path, so each one applies damage, drains durability and fires
//   domeleeattack and attackedinmelee. One behavioural difference is worth
//   knowing: each hit rolls its own damage between the shield's min and max
//   instead of replaying a single roll. The mean is unchanged, and criticals
//   were already rolled per hit in 2.4 because they happen inside the applier.
//   If a hit returns 0 the target is gone or out of reach and the loop stops
//   there rather than hammering a corpse.
//
// ---------------------------------------------------------------------------
// Client: finding the descdam=10 display handler
// ---------------------------------------------------------------------------
//
//   D2R compiles .txt formulas into one pool per table and gives each pool its
//   own evaluator wrapper off GetDataTablesForContext (0x300A90):
//
//       0x3B5160   Skills pool      DataTables +0x138 / +0x140
//       0x3B5000   SkillDesc pool   DataTables +0x150 / +0x158
//
//   Every caller of 0x3B5000 is a skilldesc.txt descdam or descatt handler,
//   which bounds that family to 0x280720..0x28F6E0. Inside it, 0x283A80 is the
//   only one that resolves the equipped shield, and its body matches the 2.4
//   descdam=10 handler line for line. That is the Smite damage line.
//
//   skilldesc.txt itself: 300-byte (0x12C) records, descdam at +16, descatt at
//   +18, ddam calc1 at +20, ddam calc2 at +24. Taken from the same binder,
//   whose LoadExcelTable call for skilldesc passes record size 0x12C, and
//   confirmed by the last column landing at +0x12A.
//
// ---------------------------------------------------------------------------
// The block the display fixes touch, at 0x283B26..0x283BBB
// ---------------------------------------------------------------------------
//
//   Register contract:
//       rsi  = the unit whose sheet is being drawn
//       dil  = the data context byte
//       rax  = the shield's compiled ItemsTxt record
//       r12d = displayed min damage, r13d = displayed max damage
//
//     0x283B26  mov   edx,117                     \
//     0x283B2B  mov   r8d,-1                       |
//     0x283B31  mov   rcx,rsi                      |
//     0x283B34  movzx r12d,byte ptr [rax+0x10F]    |  shield min damage
//     0x283B3C  movzx r13d,byte ptr [rax+0x110]    |  shield max damage
//     0x283B44  call  0x33DCD0                     |  SKILLS_GetSkillById(unit,117,-1)
//     0x283B51  mov   edx,101                      |  state 101, Holy Shield
//     0x283B59  call  0x3351B0                     |  has-state
//     0x283B6F  call  0x33D1E0                     |  SKILLS_GetBaseLevel
//     0x283B7F  mov   r8d,117                      |  skill min damage
//     0x283B8E  call  0x3385B0                     |  >> 8, added to r12d
//     0x283B99  mov   r8d,117                      |  skill max damage
//     0x283BB1  call  0x338160                    /   >> 8, added to r13d
//
//   The vanilla skill id is three plain imm32 loads, which is the one place
//   3.3 is easier than 2.4: 2.4 hid it in a statlist walk comparing a word
//   against 117, and 546 did not fit the imm8 encoding, so it needed a cave.
//   Here it is three dword writes and nothing moves.
//
//   The state the display tests for stays 101. That is deliberate: the server
//   side also keys off state 101 and reads the source skill from the state, so
//   a replacement Holy Shield has to apply state 101 through its skills.txt
//   aurastate column either way.
//
//   After this block the handler folds in the skill's own param3 and param4,
//   the shield's strength and dexterity bonuses, then damagepercent (stat 25),
//   item_mindamage_percent (17) and item_maxdamage_percent (18), and scales
//   the shield damage by the accumulated percent before formatting the line.
//
// ---------------------------------------------------------------------------
// Where item_normaldamage goes
// ---------------------------------------------------------------------------
//
//   2.4 computed the stat-111 contribution on the side, with its own
//   100 + strength + damagepercent factor, and added it to the finished min
//   and max. That double-counted the flat min/max percent term and missed the
//   shield's own strength and dexterity bonuses.
//
//   3.3 gets it for free: adding the stat to r12d and r13d at the point the
//   shield damage is read sends it through the engine's own scaling, which is
//   exactly the treatment the real damage calculation gives it. The displayed
//   number is the scaled sum rather than two separately scaled halves.
//
//   0x2F5020   GetUnitStat(unit, statId, layer) -> int32.

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
#include <initializer_list>
#include <string>
#include <string_view>

namespace CelestialRayOne::Smite {
namespace {

// ---------------------------------------------------------------------------
// Native anchors
// ---------------------------------------------------------------------------

constexpr std::uint64_t SmiteDoFuncRva     = 0x564930;
constexpr std::uint64_t GetSkillRecordRva  = 0x097790;
constexpr std::uint64_t EvaluateFormulaRva = 0x3B5160;
constexpr std::uint64_t GetUnitStatRva     = 0x2F5020;

// The three vanilla skill-117 immediates in the descdam=10 handler.
constexpr std::uint64_t HolyShieldLookupRva = 0x283B26;  // mov edx,117
constexpr std::uint64_t HolyShieldMinDamRva = 0x283B7F;  // mov r8d,117
constexpr std::uint64_t HolyShieldMaxDamRva = 0x283B99;  // mov r8d,117
constexpr std::uint64_t HolyShieldLookupImm = HolyShieldLookupRva + 1;
constexpr std::uint64_t HolyShieldMinDamImm = HolyShieldMinDamRva + 2;
constexpr std::uint64_t HolyShieldMaxDamImm = HolyShieldMaxDamRva + 2;

// Where the display reads the shield's min and max damage.
constexpr std::uint64_t ShieldDamageReadRva   = 0x283B34;
constexpr std::uint64_t ShieldDamageResumeRva = 0x283B44;
constexpr std::uint32_t ShieldDamageReadSize  = 16;

constexpr std::size_t   GameVersionOffset   = 262;
constexpr std::size_t   SkillRecordCalc1    = 400;
constexpr std::size_t   SkillRecordCalcStep = 4;
constexpr std::uint32_t BlankFormula        = 0xFFFFFFFFu;

constexpr std::int32_t  LowestCalcField     = 5;
constexpr std::int32_t  HighestCalcField    = 10;

constexpr std::size_t   MaximumConfigBytes  = 32'768;
constexpr std::size_t   RelayBytes          = 4'096;

// 0x564930 entry, 26 bytes, through the frame setup.
//   push rbp / push rbx / push rsi / push rdi / push r12 / push r14 / push r15
//   lea rbp,[rsp-0x100] / sub rsp,0x200
constexpr auto SmiteEntryExpected = std::to_array<std::uint8_t>({
    0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41,
    0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x00,
    0xFF, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x00, 0x02,
    0x00, 0x00,
});

// 0x97790 entry, 24 bytes. The trailing call is GetDataTables at 0x300A90.
constexpr auto GetSkillRecordExpected = std::to_array<std::uint8_t>({
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
    0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48,
    0x63, 0xF2, 0xE8, 0xE9, 0x92, 0x26, 0x00, 0x48,
});

// 0x3B5160 entry, 24 bytes. The Skills-pool formula evaluator.
constexpr auto EvaluateFormulaExpected = std::to_array<std::uint8_t>({
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
    0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57,
    0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0xB0,
});

// 0x2F5020, the unit stat getter (unit in rcx, statId in edx, layer in r8d). In
// D2RLoader 1.3.0 its first ten bytes are a thunk to D2RCore!ReadWideUnitStat:
//   2F5020  FF 25 disp32   jmp [rip+disp32]   disp32 locates the loader's import
//                                             slot and is not compared
//   2F5026  90 90 90 90    nop x4
//   2F502A  ...            the untouched remainder of the old body
// ReadWideUnitStat keys its lookup on the full 32-bit layer register.
constexpr std::uint64_t GetUnitStatThunkNopsRva = GetUnitStatRva + 6;
constexpr std::uint64_t GetUnitStatBodyRva      = GetUnitStatRva + 10;
constexpr auto GetUnitStatThunkJump = std::to_array<std::uint8_t>({ 0xFF, 0x25 });
constexpr auto GetUnitStatThunkNops = std::to_array<std::uint8_t>({ 0x90, 0x90, 0x90, 0x90 });
constexpr auto GetUnitStatBody = std::to_array<std::uint8_t>({
    0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83,
    0xEC, 0x20, 0x41, 0x0F, 0xB7, 0xE8, 0x8B, 0xFA,
    0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9,
});

// 0x283B26: mov edx,117 / mov r8d,-1 / mov rcx,rsi
constexpr auto HolyShieldLookupExpected = std::to_array<std::uint8_t>({
    0xBA, 0x75, 0x00, 0x00, 0x00, 0x41, 0xB8, 0xFF,
    0xFF, 0xFF, 0xFF, 0x48, 0x8B, 0xCE,
});

// 0x283B7F: mov r8d,117 / mov rdx,rsi / movzx ecx,dil / mov ebx,eax
constexpr auto HolyShieldMinDamExpected = std::to_array<std::uint8_t>({
    0x41, 0xB8, 0x75, 0x00, 0x00, 0x00, 0x48, 0x8B,
    0xD6, 0x40, 0x0F, 0xB6, 0xCF, 0x8B, 0xD8,
});

// 0x283B99: mov r8d,117 / mov dword ptr [rsp+0x20],1
constexpr auto HolyShieldMaxDamExpected = std::to_array<std::uint8_t>({
    0x41, 0xB8, 0x75, 0x00, 0x00, 0x00, 0xC7, 0x44,
    0x24, 0x20, 0x01, 0x00, 0x00, 0x00,
});

// 0x283B34, 16 bytes:
//   movzx r12d, byte ptr [rax+0x10F]
//   movzx r13d, byte ptr [rax+0x110]
constexpr auto ShieldDamageReadExpected = std::to_array<std::uint8_t>({
    0x44, 0x0F, 0xB6, 0xA0, 0x0F, 0x01, 0x00, 0x00,
    0x44, 0x0F, 0xB6, 0xA8, 0x10, 0x01, 0x00, 0x00,
});

// The vanilla immediate the three skill-id writes replace.
constexpr auto VanillaSkillImmediate = std::to_array<std::uint8_t>({
    0x75, 0x00, 0x00, 0x00,
});

using SmiteDoFuncFn = std::int64_t(__fastcall*)(
    void*, void*, std::int32_t, std::uint32_t) noexcept;
using GetSkillRecordFn = void*(__fastcall*)(
    std::uint8_t, std::int32_t) noexcept;
using EvaluateFormulaFn = std::int32_t(__fastcall*)(
    std::uint8_t, void*, std::uint32_t, std::int32_t, std::int32_t) noexcept;
using GetUnitStatFn = std::int32_t(__fastcall*)(
    void*, std::int32_t, std::uint32_t) noexcept;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

struct Config {
    bool         multiHit            = true;
    std::int32_t calcField           = 5;
    std::int32_t maxHits             = 100;

    bool         holyShieldEnabled   = true;
    std::int32_t holyShieldSkillId   = 546;

    bool         normalDamageEnabled = true;
    std::int32_t normalDamageStatId  = 111;
};

constexpr char DefaultConfigToml[] =
    "# Smite\n"
    "#\n"
    "# Three independent Smite features. Each can be turned off on its own and\n"
    "# a disabled feature installs nothing at all. Features 2 and 3 are display\n"
    "# only and change no damage.\n"
    "#\n"
    "# Console: smite\n"
    "\n"
    "# =====================================================================\n"
    "# 1. Multi-hit\n"
    "# =====================================================================\n"
    "#\n"
    "# One Smite cast lands N times instead of once. Every hit runs the full\n"
    "# Smite path, so each one applies damage, drains durability and fires the\n"
    "# melee events domeleeattack and attackedinmelee. That is the point of the\n"
    "# feature: N hits means N on-strike procs, N life and mana steal events and\n"
    "# N crushing blow / open wounds rolls, not one hit multiplied.\n"
    "#\n"
    "# N is a skills.txt formula on the Smite row, evaluated per cast against\n"
    "# the caster at that cast's skill level. It can reference stats and other\n"
    "# calc columns like any other skill formula. Leave the column blank and\n"
    "# Smite behaves exactly as it does in vanilla.\n"
    "#\n"
    "# This covers every skill whose skills.txt srvdofunc is 150, not just the\n"
    "# Smite row, and it covers Smite however it is reached, including a Charge\n"
    "# that chains into it.\n"
    "#\n"
    "# Each hit rolls its own damage between the shield's min and max, rather\n"
    "# than one roll replayed N times. The average is the same. If a hit does\n"
    "# not connect the remaining hits are dropped instead of being applied to a\n"
    "# corpse, and a Smite cast from inside a Smite hit takes its single\n"
    "# vanilla hit so a Smite-on-striking proc cannot multiply itself.\n"
    "\n"
    "[multi_hit]\n"
    "\n"
    "multi_hit_enabled = true\n"
    "\n"
    "# Which skills.txt calc column holds the hit count. 5 through 10.\n"
    "#\n"
    "# calc1 and calc2 are already used by Smite itself: the engine reads calc1\n"
    "# as the damage percent applied to the shield damage and calc2 as flat\n"
    "# bonus damage, and calc4 feeds the elemental damage when the etype column\n"
    "# is set. calc3 is free but is left out of range here so the choice stays\n"
    "# clear of everything Smite already reads.\n"
    "#\n"
    "# Record offsets these map to, for reference:\n"
    "#   calc5 416   calc6 420   calc7 424   calc8 428   calc9 432   calc10 436\n"
    "calc_field = 5\n"
    "\n"
    "# Safety ceiling on the evaluated hit count. This is not a balance knob:\n"
    "# it exists so a formula that goes wrong cannot stall the server thread in\n"
    "# a runaway loop. Raise it if a build legitimately needs more hits.\n"
    "max_hits = 100\n"
    "\n"
    "# =====================================================================\n"
    "# 2. Holy Shield display fix\n"
    "# =====================================================================\n"
    "#\n"
    "# Smite's damage line adds the Holy Shield skill's bonus damage while the\n"
    "# aura is up, but it hardcodes vanilla skill 117. The server side reads\n"
    "# that bonus off the live state and so follows whatever skill actually\n"
    "# cast it, which means a mod that replaces Holy Shield with its own skill\n"
    "# shows the bonus as zero while the real damage includes it.\n"
    "#\n"
    "# The state the display tests for stays 101, the Holy Shield state. That\n"
    "# is deliberate: the server keys off state 101 too, so a replacement Holy\n"
    "# Shield has to apply state 101 through its skills.txt aurastate column\n"
    "# either way.\n"
    "\n"
    "[holy_shield_display]\n"
    "\n"
    "holy_shield_enabled = true\n"
    "\n"
    "# The skill whose bonus damage the Smite line should read. 117 is vanilla\n"
    "# Holy Shield, which makes this fix a no-op.\n"
    "holy_shield_skill_id = 546\n"
    "\n"
    "# =====================================================================\n"
    "# 3. item_normaldamage display fix\n"
    "# =====================================================================\n"
    "#\n"
    "# item_normaldamage, the \"adds X damage\" stat, applies to Smite's real\n"
    "# damage but the vanilla line leaves it out, so gear that raises Smite\n"
    "# noticeably does not move the number on the sheet.\n"
    "#\n"
    "# The stat is folded into the shield damage at the point the display reads\n"
    "# it, which sends it through the same scaling the shield damage already\n"
    "# gets: the skill's own per-level bonus, the shield's strength and\n"
    "# dexterity bonuses, damagepercent, and the min and max damage percent\n"
    "# stats. That is the treatment the real damage calculation gives it.\n"
    "\n"
    "[item_normaldamage_display]\n"
    "\n"
    "item_normaldamage_enabled = true\n"
    "\n"
    "# itemstatcost.txt id of the stat to fold in. 111 is item_normaldamage.\n"
    "item_normaldamage_stat_id = 111\n";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

const D2RL::PluginContext* Context{};
std::uint8_t*              Base{};
Config                     Settings{};

SmiteDoFuncFn     OriginalSmite{};
GetSkillRecordFn  GetSkillRecord{};
EvaluateFormulaFn EvaluateFormula{};
GetUnitStatFn     GetUnitStat{};

void* RelayPage{};
bool  MultiHitInstalled{};
bool  HolyShieldInstalled{};
bool  NormalDamageInstalled{};

// Smite runs on the server thread, but this costs nothing and removes the
// question. It only guards nesting: a proc that casts Smite from inside a
// Smite hit takes its single vanilla hit instead of multiplying again.
thread_local bool InMultiHit = false;

std::atomic<std::uint64_t> LandedCastsSeen{};
std::atomic<std::int32_t>  LastHitCount{};
std::atomic<std::uint64_t> CastsMultiplied{};
std::atomic<std::uint64_t> ExtraHitsApplied{};
std::atomic<std::uint64_t> HitsCutShort{};
std::atomic<std::uint64_t> LinesDrawn{};
std::atomic<std::uint64_t> LinesAdjusted{};

auto CalcOffset() noexcept -> std::size_t {
    return SkillRecordCalc1
        + static_cast<std::size_t>(Settings.calcField - 1) * SkillRecordCalcStep;
}

// ---------------------------------------------------------------------------
// Config parsing (small hand-rolled TOML subset, no external dependency)
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
    if (value == "true" || value == "1") { out = true; return true; }
    if (value == "false" || value == "0") { out = false; return true; }
    return false;
}

auto ParseInteger(std::string_view value, std::int64_t& out) noexcept -> bool {
    if (value.empty()) return false;
    bool negative = false;
    std::size_t index = 0;
    if (value[0] == '+' || value[0] == '-') {
        negative = value[0] == '-';
        index = 1;
    }
    if (index >= value.size()) return false;
    std::int64_t accumulator = 0;
    for (; index < value.size(); ++index) {
        const char character = value[index];
        if (character == '_') continue;
        if (character < '0' || character > '9') return false;
        accumulator = accumulator * 10 + (character - '0');
        if (accumulator > 0x7FFF'FFFFLL) return false;
    }
    out = negative ? -accumulator : accumulator;
    return true;
}

void ApplyConfigLine(std::string_view key, std::string_view value) noexcept {
    std::int64_t number = 0;
    if (key == "multi_hit_enabled") {
        ParseBool(value, Settings.multiHit);
    } else if (key == "calc_field") {
        if (ParseInteger(value, number)
                && number >= LowestCalcField && number <= HighestCalcField) {
            Settings.calcField = static_cast<std::int32_t>(number);
        }
    } else if (key == "max_hits") {
        if (ParseInteger(value, number) && number >= 1) {
            Settings.maxHits = static_cast<std::int32_t>(number);
        }
    } else if (key == "holy_shield_enabled") {
        ParseBool(value, Settings.holyShieldEnabled);
    } else if (key == "holy_shield_skill_id") {
        if (ParseInteger(value, number) && number >= 0 && number <= 0x7FFF) {
            Settings.holyShieldSkillId = static_cast<std::int32_t>(number);
        }
    } else if (key == "item_normaldamage_enabled") {
        ParseBool(value, Settings.normalDamageEnabled);
    } else if (key == "item_normaldamage_stat_id") {
        if (ParseInteger(value, number) && number >= 0 && number <= 0xFFFF) {
            Settings.normalDamageStatId = static_cast<std::int32_t>(number);
        }
    }
}

void ParseConfig(std::string_view text) noexcept {
    std::size_t cursor = 0;
    while (cursor <= text.size()) {
        const std::size_t breakAt = text.find('\n', cursor);
        const std::size_t end = breakAt == std::string_view::npos ? text.size() : breakAt;
        std::string_view line = Trim(text.substr(cursor, end - cursor));
        cursor = end + 1;
        if (breakAt == std::string_view::npos && line.empty()) break;

        const std::size_t comment = line.find('#');
        if (comment != std::string_view::npos) line = Trim(line.substr(0, comment));
        if (line.empty() || line.front() == '[') continue;

        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) continue;
        ApplyConfigLine(Trim(line.substr(0, equals)), Trim(line.substr(equals + 1)));

        if (breakAt == std::string_view::npos) break;
    }
}

void ReadConfiguration() noexcept {
    if (!Context->EnsureConfig(DefaultConfigToml)) {
        Context->LogWarn("Smite: config file could not be created; using defaults.");
        return;
    }
    std::string buffer(MaximumConfigBytes, '\0');
    std::uint32_t required = 0;
    if (!Context->ReadConfig(buffer.data(),
            static_cast<std::uint32_t>(buffer.size()), &required)) {
        Context->LogWarn("Smite: config file could not be read; using defaults.");
        return;
    }
    buffer.resize(std::strlen(buffer.c_str()));
    ParseConfig(buffer);

    if (Settings.calcField < LowestCalcField) Settings.calcField = LowestCalcField;
    if (Settings.calcField > HighestCalcField) Settings.calcField = HighestCalcField;
    if (Settings.maxHits < 1) Settings.maxHits = 1;
}

// ---------------------------------------------------------------------------
// Multi-hit
// ---------------------------------------------------------------------------

auto ResolveHitCount(void* game, void* unit, std::int32_t skillId,
        std::uint32_t skillLevel) noexcept -> std::int32_t {
    if (!game || !GetSkillRecord || !EvaluateFormula) return 1;

    const std::uint8_t version =
        *(static_cast<const std::uint8_t*>(game) + GameVersionOffset);

    const auto* record = static_cast<const std::uint8_t*>(
        GetSkillRecord(version, skillId));
    if (!record) return 1;

    std::uint32_t poolOffset = BlankFormula;
    std::memcpy(&poolOffset, record + CalcOffset(), sizeof(poolOffset));
    if (poolOffset == BlankFormula) return 1;

    std::int32_t hits = EvaluateFormula(version, unit, poolOffset, skillId,
        static_cast<std::int32_t>(skillLevel));
    if (hits < 1) hits = 1;
    if (hits > Settings.maxHits) hits = Settings.maxHits;
    return hits;
}

auto __fastcall SmiteDetour(void* game, void* unit, std::int32_t skillId,
        std::uint32_t skillLevel) noexcept -> std::int64_t {
    const std::int64_t landed = OriginalSmite(game, unit, skillId, skillLevel);

    // 0 means the cast did not connect: no target, out of reach, or blocked.
    // There is nothing to repeat.
    if (landed == 0 || InMultiHit) return landed;

    LandedCastsSeen.fetch_add(1, std::memory_order_relaxed);
    const std::int32_t hits = ResolveHitCount(game, unit, skillId, skillLevel);
    LastHitCount.store(hits, std::memory_order_relaxed);
    if (hits <= 1) return landed;

    InMultiHit = true;
    std::int32_t applied = 0;
    for (std::int32_t index = 1; index < hits; ++index) {
        if (OriginalSmite(game, unit, skillId, skillLevel) == 0) {
            HitsCutShort.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        ++applied;
    }
    InMultiHit = false;

    CastsMultiplied.fetch_add(1, std::memory_order_relaxed);
    ExtraHitsApplied.fetch_add(static_cast<std::uint64_t>(applied),
        std::memory_order_relaxed);
    return landed;
}

// ---------------------------------------------------------------------------
// item_normaldamage display, reached from the relay at 0x283B34
// ---------------------------------------------------------------------------

// The unit is the one whose sheet is being drawn, and it carries the stat, so
// no owner resolution is needed. A missing stat reads as 0 and the relay's two
// adds become no-ops, which leaves the vanilla line untouched.
auto __fastcall NormalDamageBonus(void* unit) noexcept -> std::int32_t {
    LinesDrawn.fetch_add(1, std::memory_order_relaxed);
    if (!unit || !GetUnitStat) return 0;
    const std::int32_t value = GetUnitStat(unit, Settings.normalDamageStatId, 0);
    if (value <= 0) return 0;
    LinesAdjusted.fetch_add(1, std::memory_order_relaxed);
    return value;
}

// ---------------------------------------------------------------------------
// Relay page
// ---------------------------------------------------------------------------

auto CanEncodeRel32(std::uintptr_t from, std::uintptr_t to) noexcept -> bool {
    const std::int64_t delta =
        static_cast<std::int64_t>(to) - static_cast<std::int64_t>(from) - 5;
    return delta >= INT32_MIN && delta <= INT32_MAX;
}

auto AllocateNear(void* hint, std::size_t size) noexcept -> void* {
    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    const auto granularity =
        static_cast<std::uintptr_t>(systemInfo.dwAllocationGranularity);
    const auto aligned =
        reinterpret_cast<std::uintptr_t>(hint) & ~(granularity - 1U);
    for (std::uintptr_t delta = granularity;
            delta < 0x7000'0000ULL; delta += granularity) {
        const auto candidate = aligned + delta;
        if (!CanEncodeRel32(reinterpret_cast<std::uintptr_t>(hint), candidate)) break;
        if (auto* memory = VirtualAlloc(reinterpret_cast<void*>(candidate), size,
                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) {
            return memory;
        }
    }
    return nullptr;
}

// Builds the stub that replaces the 16-byte shield-damage read.
//
// Entry contract, proven by the very bytes being replaced and by the block
// around them: rax is the shield's ItemsTxt record, rsi is the unit, r12d and
// r13d are about to receive the shield's min and max damage, and rcx, rdx and
// r8 already hold the arguments for the call at the resume address.
//
// The two stolen instructions run first, while rax is still the record. Every
// volatile register, the flags and xmm0-5 are then saved, because the replaced
// block contained no call and the surrounding code assumes they survive. The
// bonus is folded into r12d and r13d before the restores, since those two are
// callee-saved and therefore cross the callback untouched. pushfq happens
// before the adds and popfq after them, so the flags the resume point sees are
// the ones it would have seen in vanilla.
auto BuildNormalDamageRelay(std::uint8_t* out, const void* callback,
        std::uintptr_t continuation) noexcept -> std::size_t {
    std::size_t index = 0;
    const auto emit = [&](std::initializer_list<std::uint8_t> bytes) {
        for (const auto byte : bytes) out[index++] = byte;
    };

    // The stolen pair, byte for byte.
    for (const auto byte : ShieldDamageReadExpected) out[index++] = byte;

    emit({0x50});                                     // push rax
    emit({0x51});                                     // push rcx
    emit({0x52});                                     // push rdx
    emit({0x41, 0x50});                               // push r8
    emit({0x41, 0x51});                               // push r9
    emit({0x41, 0x52});                               // push r10
    emit({0x41, 0x53});                               // push r11
    emit({0x9C});                                     // pushfq
    emit({0x55});                                     // push rbp
    emit({0x48, 0x89, 0xE5});                         // mov rbp, rsp
    emit({0x48, 0x83, 0xE4, 0xF0});                   // and rsp, -16
    emit({0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00}); // sub rsp, 0x80

    // movaps [rsp+0x20 + 0x10*i], xmmI
    for (std::uint8_t slot = 0; slot < 6; ++slot) {
        emit({0x0F, 0x29,
              static_cast<std::uint8_t>(0x44 + (slot << 3)),
              0x24,
              static_cast<std::uint8_t>(0x20 + (slot * 0x10))});
    }

    emit({0x48, 0x89, 0xF1});                         // mov rcx, rsi
    emit({0x48, 0xB8});                               // mov rax, imm64
    const auto target = reinterpret_cast<std::uint64_t>(callback);
    std::memcpy(out + index, &target, sizeof(target));
    index += sizeof(target);
    emit({0xFF, 0xD0});                               // call rax

    emit({0x41, 0x01, 0xC4});                         // add r12d, eax
    emit({0x41, 0x01, 0xC5});                         // add r13d, eax

    // movaps xmmI, [rsp+0x20 + 0x10*i]
    for (std::uint8_t slot = 0; slot < 6; ++slot) {
        emit({0x0F, 0x28,
              static_cast<std::uint8_t>(0x44 + (slot << 3)),
              0x24,
              static_cast<std::uint8_t>(0x20 + (slot * 0x10))});
    }

    emit({0x48, 0x89, 0xEC});                         // mov rsp, rbp
    emit({0x5D});                                     // pop rbp
    emit({0x9D});                                     // popfq
    emit({0x41, 0x5B});                               // pop r11
    emit({0x41, 0x5A});                               // pop r10
    emit({0x41, 0x59});                               // pop r9
    emit({0x41, 0x58});                               // pop r8
    emit({0x5A});                                     // pop rdx
    emit({0x59});                                     // pop rcx
    emit({0x58});                                     // pop rax
    emit({0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});       // jmp qword ptr [rip+0]

    const auto resume = static_cast<std::uint64_t>(continuation);
    std::memcpy(out + index, &resume, sizeof(resume));
    index += sizeof(resume);
    return index;
}

// ---------------------------------------------------------------------------
// Verification and installation
// ---------------------------------------------------------------------------

template <std::size_t Size>
auto Verify(std::uint64_t rva, const std::array<std::uint8_t, Size>& expected,
        const char* label) noexcept -> bool {
    if (Context->CheckExpectedBytes(rva, expected.data(),
            static_cast<std::uint32_t>(expected.size()))) {
        return true;
    }
    char message[240];
    std::snprintf(message, sizeof(message),
        "Smite: %s at 0x%llX does not match this build, or is already owned by "
        "another plugin. That feature is not installed.",
        label, static_cast<unsigned long long>(rva));
    Context->LogError(message);
    return false;
}

auto InstallMultiHit() noexcept -> bool {
    if (!Verify(SmiteDoFuncRva, SmiteEntryExpected, "the Smite do-func")
        || !Verify(GetSkillRecordRva, GetSkillRecordExpected,
                   "the skills.txt record getter")
        || !Verify(EvaluateFormulaRva, EvaluateFormulaExpected,
                   "the skill formula evaluator")) {
        return false;
    }

    GetSkillRecord = reinterpret_cast<GetSkillRecordFn>(Base + GetSkillRecordRva);
    EvaluateFormula = reinterpret_cast<EvaluateFormulaFn>(Base + EvaluateFormulaRva);

    if (!Context->InstallInlineHook(
            SmiteDoFuncRva,
            SmiteEntryExpected.data(),
            static_cast<std::uint32_t>(SmiteEntryExpected.size()),
            &SmiteDetour,
            &OriginalSmite)) {
        Context->LogError("Smite: the Smite do-func could not be hooked.");
        return false;
    }
    MultiHitInstalled = true;
    return true;
}

auto InstallHolyShieldDisplay() noexcept -> bool {
    // All three sites are verified before any of them is written, so a
    // mismatch cannot leave the damage line half-patched.
    if (!Verify(HolyShieldLookupRva, HolyShieldLookupExpected,
                "the Holy Shield skill lookup in the Smite damage line")
        || !Verify(HolyShieldMinDamRva, HolyShieldMinDamExpected,
                   "the Holy Shield minimum-damage read")
        || !Verify(HolyShieldMaxDamRva, HolyShieldMaxDamExpected,
                   "the Holy Shield maximum-damage read")) {
        return false;
    }

    const auto skillId = static_cast<std::uint32_t>(Settings.holyShieldSkillId);
    const auto* expected = VanillaSkillImmediate.data();
    const auto expectedSize =
        static_cast<std::uint32_t>(VanillaSkillImmediate.size());

    if (!Context->PatchWriteU32(HolyShieldLookupImm, expected, expectedSize, skillId)
        || !Context->PatchWriteU32(HolyShieldMinDamImm, expected, expectedSize, skillId)
        || !Context->PatchWriteU32(HolyShieldMaxDamImm, expected, expectedSize, skillId)) {
        Context->LogError(
            "Smite: the Holy Shield skill id in the damage line could not be "
            "redirected.");
        return false;
    }
    HolyShieldInstalled = true;
    return true;
}

auto InstallNormalDamageDisplay() noexcept -> bool {
    if (!Verify(ShieldDamageReadRva, ShieldDamageReadExpected,
                "the shield damage read in the Smite damage line")
        || !Verify(GetUnitStatRva, GetUnitStatThunkJump, "the unit stat getter thunk")
        || !Verify(GetUnitStatThunkNopsRva, GetUnitStatThunkNops, "the unit stat getter thunk padding")
        || !Verify(GetUnitStatBodyRva, GetUnitStatBody, "the unit stat getter body")) {
        return false;
    }

    GetUnitStat = reinterpret_cast<GetUnitStatFn>(Base + GetUnitStatRva);

    RelayPage = AllocateNear(Base + ShieldDamageReadRva, RelayBytes);
    if (!RelayPage) {
        Context->LogError(
            "Smite: no relay page was available within rel32 reach.");
        return false;
    }

    auto* relay = static_cast<std::uint8_t*>(RelayPage);
    const auto relayBase = reinterpret_cast<std::uintptr_t>(relay);
    const auto imageBase = reinterpret_cast<std::uintptr_t>(Base);

    BuildNormalDamageRelay(relay,
        reinterpret_cast<const void*>(&NormalDamageBonus),
        imageBase + ShieldDamageResumeRva);

    DWORD previousProtection = 0;
    if (!VirtualProtect(relay, RelayBytes, PAGE_EXECUTE_READ, &previousProtection)) {
        Context->LogError("Smite: relay page protection could not be finalized.");
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), relay, RelayBytes);

    if (!CanEncodeRel32(imageBase + ShieldDamageReadRva, relayBase)) {
        Context->LogError("Smite: relay displacement validation failed.");
        return false;
    }

    if (!Context->PatchJmpRel32(
            ShieldDamageReadRva,
            ShieldDamageReadExpected.data(),
            static_cast<std::uint32_t>(ShieldDamageReadExpected.size()),
            relayBase - imageBase,
            ShieldDamageReadSize)) {
        Context->LogError("Smite: the shield damage read could not be redirected.");
        return false;
    }
    NormalDamageInstalled = true;
    return true;
}

// ---------------------------------------------------------------------------
// Console command
// ---------------------------------------------------------------------------

auto __cdecl StatusCommand(D2R::Game::Client*,
        const D2RL::ConsoleCommandContext* command, void*) noexcept
        -> D2RL::ConsoleCommandResult {
    if (!command || !command->plugin) return D2RL::ConsoleCommandResult::Failed;
    char message[640];
    std::snprintf(message, sizeof(message),
        "Smite: multi-hit %s (calc%d, offset %zu, cap %d) | holy shield display "
        "%s (skill %d) | item_normaldamage display %s (stat %d) | landed casts "
        "seen %llu | last hit count %d | multiplied casts %llu | extra hits %llu | "
        "cut short %llu | lines drawn %llu | lines with a bonus %llu",
        MultiHitInstalled ? "on" : "off",
        Settings.calcField,
        CalcOffset(),
        Settings.maxHits,
        HolyShieldInstalled ? "on" : "off",
        Settings.holyShieldSkillId,
        NormalDamageInstalled ? "on" : "off",
        Settings.normalDamageStatId,
        static_cast<unsigned long long>(LandedCastsSeen.load(std::memory_order_relaxed)),
        LastHitCount.load(std::memory_order_relaxed),
        static_cast<unsigned long long>(CastsMultiplied.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(ExtraHitsApplied.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(HitsCutShort.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(LinesDrawn.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(LinesAdjusted.load(std::memory_order_relaxed)));
    command->plugin->WriteConsoleMessage(message);
    return D2RL::ConsoleCommandResult::Handled;
}

constexpr D2RL::PluginInfo Info{
    .infoSize = D2RL::PluginInfoSize,
    .apiVersion = D2RL_PLUGIN_API_VERSION,
    .id = "celestialrayone.smite-multihit",
    .name = "Smite",
    .version = "1.1.1",
    .author = "CelestialRayOne",
    .description =
        "Smite multi-hit driven by a skills.txt calc column, plus the Holy "
        "Shield and item_normaldamage fixes for its damage display.",
    .flags = D2RL::PluginFlags::Shared | D2RL::PluginFlags::NativeHooks,
};

}  // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept
        -> const D2RL::PluginInfo* {
    return &Info;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(
        const D2RL::PluginContext* context) noexcept -> bool {
    if (!D2RL::HasContext(context)) return false;
    Context = context;
    Base = reinterpret_cast<std::uint8_t*>(context->exeBase);

    ReadConfiguration();

    const bool anythingRequested = Settings.multiHit
        || Settings.holyShieldEnabled
        || Settings.normalDamageEnabled;

    // A feature that cannot install is reported and left off rather than
    // taking the other two down with it. Each one owns a different site.
    if (Settings.multiHit) InstallMultiHit();
    if (Settings.holyShieldEnabled) InstallHolyShieldDisplay();
    if (Settings.normalDamageEnabled) InstallNormalDamageDisplay();

    if (!MultiHitInstalled && !HolyShieldInstalled && !NormalDamageInstalled) {
        if (!anythingRequested) {
            Context->LogInfo("Smite: every feature is disabled; nothing installed.");
            return true;
        }
        Context->LogError(
            "Smite: every requested feature failed to install. Refusing to load.");
        return false;
    }

    char message[432];
    std::snprintf(message, sizeof(message),
        "Smite: multi-hit %s from skills.txt calc%d (record offset %zu, cap %d); "
        "Holy Shield display %s (skill %d at 0x283B26 / 0x283B7F / 0x283B99); "
        "item_normaldamage display %s (stat %d folded in at 0x283B34).",
        MultiHitInstalled ? "armed" : "off",
        Settings.calcField,
        CalcOffset(),
        Settings.maxHits,
        HolyShieldInstalled ? "armed" : "off",
        Settings.holyShieldSkillId,
        NormalDamageInstalled ? "armed" : "off",
        Settings.normalDamageStatId);
    Context->LogInfo(message);

    if (!Context->RegisterConsoleCommand("smite", &StatusCommand,
            "Reports Smite plugin status and counters.")) {
        Context->LogWarn("Smite: the status console command was refused.");
    }
    return true;
}

}  // namespace CelestialRayOne::Smite
