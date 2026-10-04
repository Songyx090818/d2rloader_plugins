// Absorb All Damage
//
// skills.txt auraeventfunc 40 absorbs every direct damage type of a hit
// (physical, fire, lightning, cold, magic and poison) instead of physical
// damage only.
//
// Everything below was read out of the D2RLoader 1.3.1 process dump (md5
// 8af7a47ce9077de0378c69af2930854d) with IDA, and out of D2RCore.dll 1.3.1
// (SHA-256 2a868d013d2e0830bd2d9e04b918b19e46a73cf726c833e70d089b948fdeb5a2).
// Every game address the plugin uses is guarded by a byte witness at load.
//
// ---------------------------------------------------------------------------
// auraeventfunc 40 as the game ships it (0x524470, table 0x238E5C0 + 40*8)
// ---------------------------------------------------------------------------
//   (game, event, unit, attacker, hit, skillId, ...) -> 0 or 1
//   skill = 0x97790(context, skillId); state = skill+A0h (aurastate), bounded
//   by 0x2141A0; list = 0x2F5940(unit, state); skill+76h (aurastat2) must be
//   above 0 on that list; pool = skill+74h (aurastat1), both read through
//   0x2F5DF0 (context, list, stat, 0). context = byte at game+106h.
//   Then it subtracts ONLY hit+18h (physical) from the pool, writes the pool
//   back (0x2F7C00), and at 0 removes the state (0x3354C0, 0x2F7920,
//   0x2F4180). If something was absorbed and there is an attacker, it plays
//   the overlay skill+162h facing the attacker (0x349020). It reads nothing
//   else from the hit.
//   D2RCore dispatches it through its own wrapper (D2RCore 0x3D1CC0), which
//   reads table entry 40 and calls it with the same nine arguments.
//
// ---------------------------------------------------------------------------
// What the plugin does
// ---------------------------------------------------------------------------
//   An inline hook on 0x524470 reads the pool the same way the original
//   does, takes damage out of the real hit in the order physical (+18h),
//   fire (+20h), lightning (+2Ch), cold (+34h), magic (+30h), poison (+38h),
//   then calls the original with a stand-in hit whose physical field holds
//   the total taken. The original then lowers the pool, removes the state at
//   0 and plays the overlay exactly as it always does, for any damage type.
//   Nothing of the original is rewritten.
//
//   The field offsets are the game's own: SUNITDMG_CalculateTotalDamage
//   (0x44DF10) labels them Phys/Fire/Ligt/Cold/Magc/Pois in its resistance
//   table and sums exactly these six into the hit's life damage (+134h) at
//   0x44EC71. The absorb event (11) fires at 0x44EB25, before resistances.
//
//   Poison: the hit carries damage per frame (+38h) and length (+3Ch). The
//   hit deals one frame of it as life damage, the poison state the rest, so
//   absorbing per-frame damage d costs d * (length + 1). When the states.txt
//   poison row (state 2) has record byte +3Ch set, the damage commit applies
//   poison per source instead (0x44D145 .. 0x44D262): entries at hit+40h,
//   count at hit+48h, 12 bytes each {source, damage, length}, then the rest
//   (+38h minus their damage, +3Ch minus their length) as one more poison
//   when hit byte +0Ch or +14h is set. The plugin absorbs those entries one
//   by one and keeps +38h equal to their sum plus the rest.
//
// ---------------------------------------------------------------------------
// ABIs (x64)
// ---------------------------------------------------------------------------
//   0x97790   (context, skillId) -> skills.txt record
//   0x2141A0  (context) -> states.txt rows      0x2141E0 (context) -> itemstatcost rows
//   0x2F5940  (unit, state) -> the state's stat list
//   0x2F5DF0  (context, list, stat, layer) -> value (loader stub to ReadWideListStat)
//   0x300A90  (context) -> data tables: states rows at +290h, count at +298h,
//             row size 44h

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

namespace CelestialRayOne::AbsorbAllDamage {
namespace {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

constexpr std::uint64_t EventFunc40Rva       = 0x524470;
constexpr std::uint64_t PoisonSourcesSiteRva = 0x44D145;
constexpr std::uint64_t SkillRecordRva       = 0x097790;
constexpr std::uint64_t StateCountRva        = 0x2141A0;
constexpr std::uint64_t StatCountRva         = 0x2141E0;
constexpr std::uint64_t StateStatListRva     = 0x2F5940;
constexpr std::uint64_t ReadListStatRva      = 0x2F5DF0;
constexpr std::uint64_t DataTablesRva        = 0x300A90;

constexpr std::size_t GameDataContextOffset = 0x106;

// skills.txt record
constexpr std::size_t SkillAuraStat1Offset = 0x74;  // aurastat1: the pool
constexpr std::size_t SkillAuraStat2Offset = 0x76;  // aurastat2: the maximum
constexpr std::size_t SkillAuraStateOffset = 0xA0;  // aurastate

// The hit
constexpr std::size_t HitPhysicalOffset      = 0x18;
constexpr std::size_t HitFireOffset          = 0x20;
constexpr std::size_t HitLightningOffset     = 0x2C;
constexpr std::size_t HitMagicOffset         = 0x30;
constexpr std::size_t HitColdOffset          = 0x34;
constexpr std::size_t HitPoisonOffset        = 0x38;
constexpr std::size_t HitPoisonLengthOffset  = 0x3C;
constexpr std::size_t HitPoisonSourcesOffset = 0x40;
constexpr std::size_t HitPoisonCountOffset   = 0x48;
constexpr std::size_t HitPoisonRestFlagA     = 0x0C;
constexpr std::size_t HitPoisonRestFlagB     = 0x14;
constexpr std::size_t PoisonSourceStride     = 12;
constexpr std::size_t PoisonSourceDamage     = 4;
constexpr std::size_t PoisonSourceLength     = 8;

// The original only touches +18h of the hit it is given.
constexpr std::size_t StandInBytes = 0x200;

// Data tables
constexpr std::size_t   StatesRowsOffset      = 0x290;
constexpr std::size_t   StatesCountOffset     = 0x298;
constexpr std::size_t   StatesRowSize         = 0x44;
constexpr std::size_t   StatesPerSourceOffset = 0x3C;
constexpr std::uint64_t PoisonState           = 2;

constexpr std::uint64_t MaximumPoisonSources = 4096;
constexpr std::uint32_t HookPrologueBytes    = 22;
constexpr std::size_t   MaximumConfigBytes   = 16'384;

// ---------------------------------------------------------------------------
// Byte witnesses (D2RLoader 1.3.1 dump)
// ---------------------------------------------------------------------------

// The whole of auraeventfunc 40, 0x524470 .. 0x52473F. Its calls prove every
// helper address above except 0x300A90; its body proves it reads only +18h
// of the hit.
constexpr std::uint8_t EventFunc40Body[] {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x20, 0x56, 0x57,
    0x41, 0x54, 0x41, 0x55, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x49, 0x8B,
    0xF1, 0x4D, 0x8B, 0xE8, 0x48, 0x8B, 0xF9, 0x4D, 0x85, 0xC0, 0x75, 0x2D,
    0x48, 0x8D, 0x4C, 0x24, 0x70, 0x44, 0x88, 0x44, 0x24, 0x70, 0xE8, 0x7D,
    0x97, 0xFF, 0xFF, 0x84, 0xC0, 0x74, 0x01, 0xCC, 0x33, 0xC0, 0x48, 0x8B,
    0x5C, 0x24, 0x68, 0x48, 0x8B, 0x6C, 0x24, 0x78, 0x48, 0x83, 0xC4, 0x30,
    0x41, 0x5F, 0x41, 0x5D, 0x41, 0x5C, 0x5F, 0x5E, 0xC3, 0x48, 0x83, 0xBC,
    0x24, 0x80, 0x00, 0x00, 0x00, 0x00, 0x75, 0x11, 0x48, 0x8D, 0x4C, 0x24,
    0x70, 0xC6, 0x44, 0x24, 0x70, 0x00, 0xE8, 0x75, 0x97, 0xFF, 0xFF, 0xEB,
    0xC6, 0x8B, 0x94, 0x24, 0x88, 0x00, 0x00, 0x00, 0x0F, 0xB6, 0x89, 0x06,
    0x01, 0x00, 0x00, 0xE8, 0xA0, 0x32, 0xB7, 0xFF, 0x48, 0x8B, 0xD8, 0x48,
    0x85, 0xC0, 0x74, 0xB0, 0x66, 0x83, 0xB8, 0xA0, 0x00, 0x00, 0x00, 0x00,
    0x7C, 0x17, 0x0F, 0xB6, 0x8F, 0x06, 0x01, 0x00, 0x00, 0xE8, 0x92, 0xFC,
    0xCE, 0xFF, 0x0F, 0xBF, 0x8B, 0xA0, 0x00, 0x00, 0x00, 0x3B, 0xC8, 0x7C,
    0x14, 0x48, 0x8D, 0x4C, 0x24, 0x70, 0xC6, 0x44, 0x24, 0x70, 0x00, 0xE8,
    0xA8, 0x94, 0xFF, 0xFF, 0x84, 0xC0, 0x74, 0x01, 0xCC, 0x66, 0x83, 0xBB,
    0xA0, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x8C, 0x6D, 0xFF, 0xFF, 0xFF, 0x0F,
    0xB6, 0x8F, 0x06, 0x01, 0x00, 0x00, 0x0F, 0xBF, 0xAB, 0xA0, 0x00, 0x00,
    0x00, 0xE8, 0x52, 0xFC, 0xCE, 0xFF, 0x3B, 0xE8, 0x0F, 0x8D, 0x52, 0xFF,
    0xFF, 0xFF, 0x8B, 0xD5, 0x49, 0x8B, 0xCD, 0xE8, 0xE0, 0x13, 0xDD, 0xFF,
    0x4C, 0x8B, 0xF8, 0x48, 0x85, 0xC0, 0x0F, 0x84, 0x3C, 0xFF, 0xFF, 0xFF,
    0x0F, 0xBF, 0x6B, 0x76, 0x85, 0xED, 0x0F, 0x88, 0x30, 0xFF, 0xFF, 0xFF,
    0x0F, 0xB6, 0x8F, 0x06, 0x01, 0x00, 0x00, 0xE8, 0x5C, 0xFC, 0xCE, 0xFF,
    0x3B, 0xE8, 0x0F, 0x8D, 0x1C, 0xFF, 0xFF, 0xFF, 0x0F, 0xB6, 0x8F, 0x06,
    0x01, 0x00, 0x00, 0x45, 0x33, 0xC9, 0x44, 0x8B, 0xC5, 0x49, 0x8B, 0xD7,
    0xE8, 0x4F, 0x18, 0xDD, 0xFF, 0x85, 0xC0, 0x0F, 0x8E, 0xFF, 0xFE, 0xFF,
    0xFF, 0x44, 0x0F, 0xBF, 0x63, 0x74, 0x45, 0x85, 0xE4, 0x0F, 0x88, 0xF1,
    0xFE, 0xFF, 0xFF, 0x0F, 0xB6, 0x8F, 0x06, 0x01, 0x00, 0x00, 0xE8, 0x1D,
    0xFC, 0xCE, 0xFF, 0x44, 0x3B, 0xE0, 0x0F, 0x8D, 0xDC, 0xFE, 0xFF, 0xFF,
    0x0F, 0xB6, 0x8F, 0x06, 0x01, 0x00, 0x00, 0x45, 0x33, 0xC9, 0x45, 0x8B,
    0xC4, 0x4C, 0x89, 0x74, 0x24, 0x60, 0x49, 0x8B, 0xD7, 0xE8, 0x0A, 0x18,
    0xDD, 0xFF, 0x32, 0xD2, 0x45, 0x33, 0xF6, 0x8B, 0xE8, 0x85, 0xC0, 0x7E,
    0x3F, 0x48, 0x8B, 0x8C, 0x24, 0x80, 0x00, 0x00, 0x00, 0x8B, 0x41, 0x18,
    0x85, 0xC0, 0x7E, 0x6D, 0x3B, 0xE8, 0x7F, 0x07, 0x2B, 0xC5, 0x41, 0x8B,
    0xEE, 0xEB, 0x05, 0x2B, 0xE8, 0x41, 0x8B, 0xC6, 0x89, 0x41, 0x18, 0x44,
    0x8B, 0xCD, 0x0F, 0xB6, 0x8F, 0x06, 0x01, 0x00, 0x00, 0x45, 0x8B, 0xC4,
    0x49, 0x8B, 0xD7, 0x90, 0x44, 0x89, 0x74, 0x24, 0x20, 0xE8, 0xD2, 0x35,
    0xDD, 0xFF, 0xB2, 0x01, 0x85, 0xED, 0x75, 0x39, 0x0F, 0xBF, 0x93, 0xA0,
    0x00, 0x00, 0x00, 0x45, 0x33, 0xC0, 0x49, 0x8B, 0xCD, 0xE8, 0x7A, 0x0E,
    0xE1, 0xFF, 0x49, 0x8B, 0xD7, 0x49, 0x8B, 0xCD, 0xE8, 0xCF, 0x32, 0xDD,
    0xFF, 0x0F, 0xB6, 0x8F, 0x06, 0x01, 0x00, 0x00, 0x49, 0x8B, 0xD7, 0xE8,
    0x20, 0xFB, 0xDC, 0xFF, 0x4C, 0x8B, 0x74, 0x24, 0x60, 0x8D, 0x45, 0x01,
    0xE9, 0x3D, 0xFE, 0xFF, 0xFF, 0x48, 0x85, 0xF6, 0x0F, 0x84, 0xBB, 0x00,
    0x00, 0x00, 0x84, 0xD2, 0x0F, 0x84, 0xB3, 0x00, 0x00, 0x00, 0x66, 0x44,
    0x39, 0xB3, 0x62, 0x01, 0x00, 0x00, 0x0F, 0x8C, 0xA5, 0x00, 0x00, 0x00,
    0x0F, 0xB6, 0x8F, 0x06, 0x01, 0x00, 0x00, 0xE8, 0xC8, 0xFA, 0xCE, 0xFF,
    0x0F, 0xBF, 0x8B, 0x62, 0x01, 0x00, 0x00, 0x3B, 0xC8, 0x0F, 0x8F, 0x8A,
    0x00, 0x00, 0x00, 0x8B, 0x16, 0x8B, 0xCA, 0x83, 0xE9, 0x02, 0x74, 0x23,
    0x83, 0xE9, 0x02, 0x74, 0x1E, 0x83, 0xF9, 0x01, 0x74, 0x19, 0x48, 0x8B,
    0x4E, 0x38, 0x48, 0x85, 0xC9, 0x74, 0x0B, 0xE8, 0x68, 0xD3, 0xE1, 0xFF,
    0x8B, 0x16, 0x8B, 0xF8, 0xEB, 0x0C, 0x41, 0x8B, 0xFE, 0xEB, 0x07, 0x48,
    0x8B, 0x46, 0x38, 0x8B, 0x78, 0x14, 0x83, 0xEA, 0x02, 0x74, 0x1D, 0x83,
    0xEA, 0x02, 0x74, 0x18, 0x83, 0xFA, 0x01, 0x74, 0x13, 0x48, 0x8B, 0x4E,
    0x38, 0x48, 0x85, 0xC9, 0x74, 0x12, 0xE8, 0x29, 0xD3, 0xE1, 0xFF, 0x44,
    0x8B, 0xF0, 0xEB, 0x08, 0x48, 0x8B, 0x46, 0x38, 0x44, 0x8B, 0x70, 0x10,
    0x44, 0x8B, 0xC7, 0x41, 0x8B, 0xD6, 0x49, 0x8B, 0xCD, 0xE8, 0x5E, 0x54,
    0xE2, 0xFF, 0x0F, 0xB6, 0xC8, 0xE8, 0x16, 0xA4, 0xE8, 0xFF, 0x0F, 0xBF,
    0x93, 0x62, 0x01, 0x00, 0x00, 0x45, 0x33, 0xC0, 0x0F, 0xB6, 0xC0, 0x49,
    0x8B, 0xCD, 0x03, 0xD0, 0xE8, 0xEF, 0x48, 0xE2, 0xFF, 0x4C, 0x8B, 0x74,
    0x24, 0x60, 0xB8, 0x01, 0x00, 0x00, 0x00, 0xE9, 0x6A, 0xFD, 0xFF, 0xFF,
};  // 720 bytes

// 0x44D145, the damage commit's poison block: call 0x300A90, states count at
// +298h above 2, rows at +290h, row 2 * 44h, byte +3Ch, then the per-source
// loop over hit+40h / hit+48h in 12-byte steps.
constexpr std::uint8_t PoisonSourcesWitness[] {
    0x41, 0x0F, 0xB6, 0x8F, 0x06, 0x01, 0x00, 0x00, 0xE8, 0x3E, 0x39, 0xEB,
    0xFF, 0x48, 0x8B, 0xD8, 0x48, 0x83, 0xB8, 0x98, 0x02, 0x00, 0x00, 0x02,
    0x77, 0x14, 0x48, 0x8D, 0x4C, 0x24, 0x40, 0xC6, 0x44, 0x24, 0x40, 0x00,
    0xE8, 0x92, 0x67, 0xDC, 0xFF, 0x84, 0xC0, 0x74, 0x01, 0xCC, 0x48, 0x83,
    0xBB, 0x98, 0x02, 0x00, 0x00, 0x02, 0x0F, 0x86, 0xD9, 0x01, 0x00, 0x00,
    0x48, 0x81, 0xC3, 0x90, 0x02, 0x00, 0x00, 0x48, 0xC7, 0x44, 0x24, 0x50,
    0x02, 0x00, 0x00, 0x00, 0x48, 0x83, 0x7B, 0x08, 0x02, 0x77, 0x1E, 0x48,
    0x8D, 0x44, 0x24, 0x50, 0x48, 0x89, 0x5C, 0x24, 0x70, 0x48, 0x8D, 0x4C,
    0x24, 0x68, 0x48, 0x89, 0x44, 0x24, 0x68, 0xE8, 0x6F, 0x68, 0xDC, 0xFF,
    0x84, 0xC0, 0x74, 0x01, 0xCC, 0x48, 0x6B, 0x44, 0x24, 0x50, 0x44, 0x48,
    0x03, 0x03, 0x0F, 0x84, 0x95, 0x01, 0x00, 0x00, 0x80, 0x78, 0x3C, 0x00,
    0x0F, 0x84, 0x8B, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x5F, 0x40, 0x33, 0xC0,
    0x44, 0x8B, 0xE0, 0x44, 0x8B, 0xE8, 0x48, 0x8B, 0x47, 0x48, 0x48, 0x8D,
    0x0C, 0x40, 0x48, 0x8D, 0x04, 0x8B, 0x48, 0x3B, 0xD8, 0x74, 0x41, 0x4C,
    0x8B, 0xF8, 0x90, 0x8B, 0x03, 0x48, 0x8B, 0xD6, 0x44, 0x8B, 0x43, 0x04,
    0x49, 0x8B, 0xCE, 0x44, 0x8B, 0x4B, 0x08, 0x45, 0x03, 0xE0, 0x89, 0x44,
    0x24, 0x30, 0x45, 0x03, 0xE9, 0xC7, 0x44, 0x24, 0x28, 0xFF, 0xFF, 0xFF,
    0xFF, 0xC7, 0x44, 0x24, 0x20, 0xFF, 0xFF, 0xFF, 0xFF, 0xE8, 0xA1, 0x46,
    0x00, 0x00, 0x48, 0x83, 0xC3, 0x0C, 0x49, 0x3B, 0xDF, 0x75, 0xC8, 0x4C,
    0x8B, 0x7C, 0x24, 0x60,
};  // 232 bytes

// ---------------------------------------------------------------------------
// Native function types
// ---------------------------------------------------------------------------

using EventFuncFn = std::int64_t(__fastcall*)(void* game, std::uint64_t event, void* unit,
    void* attacker, void* hit, std::uint32_t skillId, std::uint32_t arg7, std::uint32_t arg8,
    std::uint64_t arg9) noexcept;
using SkillRecordFn   = const std::uint8_t*(__fastcall*)(std::uint32_t context, std::uint32_t skillId) noexcept;
using RowCountFn      = std::int32_t(__fastcall*)(std::uint32_t context) noexcept;
using StateStatListFn = void*(__fastcall*)(void* unit, std::uint32_t state) noexcept;
using ReadListStatFn  = std::int32_t(__fastcall*)(std::uint32_t context, void* list, std::uint32_t stat,
    std::uint32_t layer) noexcept;
using DataTablesFn    = const std::uint8_t*(__fastcall*)(std::uint32_t context) noexcept;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

struct Config {
    bool enabled = true;
};

constexpr char DefaultConfigToml[] =
    "# Absorb All Damage\n"
    "#\n"
    "# Makes skills.txt auraeventfunc 40 absorb every kind of damage, not only\n"
    "# physical damage.\n"
    "#\n"
    "# How auraeventfunc 40 works (unchanged):\n"
    "# - The skill's aurastate on the unit holds the absorb pool in aurastat1 and\n"
    "#   the maximum in aurastat2. aurastat2 must be above 0 or nothing is\n"
    "#   absorbed.\n"
    "# - Pool and damage use the same unit: 256 = 1 life.\n"
    "# - When the pool reaches 0 the state is removed.\n"
    "# - When something is absorbed and the hit has an attacker, the skill's hit\n"
    "#   overlay plays.\n"
    "# - Damage is absorbed before resistances, the same as Bone Armor and\n"
    "#   Cyclone Armor.\n"
    "#\n"
    "# What the plugin changes:\n"
    "# - The pool now takes, in this order: physical, fire, lightning, cold,\n"
    "#   magic, poison.\n"
    "# - Poison costs damage per frame x (length + 1): the hit deals one frame of\n"
    "#   poison right away and the poison state deals the rest. When the pool\n"
    "#   cannot cover all of it, the poison damage per frame drops by what the\n"
    "#   pool can pay, and the pool empties (rounded up, so no sliver is left).\n"
    "#   A poison whose damage is fully absorbed is not applied at all.\n"
    "# - When the states.txt poison row keeps a separate poison per source, each\n"
    "#   source is absorbed on its own.\n"
    "# - Not absorbed: burning (fire damage over time), and the length of chill\n"
    "#   and freeze. Chill and freeze still apply, as with Cyclone Armor.\n"
    "# - Every skills.txt row that uses auraeventfunc 40 gets this behaviour.\n"
    "#\n"
    "# Console command: absorballdamage (state and counters).\n"
    "\n"
    "[absorb_all_damage]\n"
    "\n"
    "# Master switch.\n"
    "enabled = true\n";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

enum DamageKind : std::size_t {
    KindPhysical = 0,
    KindFire,
    KindLightning,
    KindCold,
    KindMagic,
    KindPoison,
    KindCount,
};

struct DirectKind {
    std::size_t offset;
    DamageKind  kind;
};

// Absorb order. Poison comes last, on its own.
constexpr DirectKind DirectKinds[] {
    { HitPhysicalOffset,  KindPhysical },
    { HitFireOffset,      KindFire },
    { HitLightningOffset, KindLightning },
    { HitColdOffset,      KindCold },
    { HitMagicOffset,     KindMagic },
};

constexpr const char* KindNames[KindCount] { "physical", "fire", "lightning", "cold", "magic", "poison" };

const D2RL::PluginContext* Context{};
Config                     Settings{};
std::uintptr_t             Base{};

EventFuncFn     Original{};
SkillRecordFn   SkillRecord{};
RowCountFn      StateCount{};
RowCountFn      StatCount{};
StateStatListFn StateStatList{};
ReadListStatFn  ReadListStat{};
DataTablesFn    GameDataTables{};

std::atomic<bool> Installed{false};
std::atomic<bool> Armed{false};

std::array<std::atomic<std::uint64_t>, KindCount> Absorbed{};
std::atomic<std::uint64_t> Hits{0};
std::atomic<std::uint64_t> Breaks{0};
std::atomic<std::uint64_t> Vanilla{0};
std::atomic<std::uint64_t> BadPoisonLists{0};

// ---------------------------------------------------------------------------
// Helpers
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
auto Read(const void* base, std::size_t offset) noexcept -> T {
    T value{};
    std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(T));
    return value;
}

template <typename T>
void Write(void* base, std::size_t offset, T value) noexcept {
    std::memcpy(static_cast<std::uint8_t*>(base) + offset, &value, sizeof(T));
}

// ---------------------------------------------------------------------------
// The rule
// ---------------------------------------------------------------------------

// The same reads, bounds and order as 0x5244DD .. 0x5245E1.
auto ReadPool(const void* game, void* unit, std::uint32_t skillId, std::int32_t& pool) noexcept -> bool {
    const std::uint32_t context = Read<std::uint8_t>(game, GameDataContextOffset);
    const std::uint8_t* skill = SkillRecord(context, skillId);
    if (skill == nullptr) return false;

    const std::int16_t state = Read<std::int16_t>(skill, SkillAuraStateOffset);
    if (state < 0 || state >= StateCount(context)) return false;
    void* list = StateStatList(unit, static_cast<std::uint32_t>(state));
    if (list == nullptr) return false;

    const std::int16_t maximum = Read<std::int16_t>(skill, SkillAuraStat2Offset);
    if (maximum < 0 || maximum >= StatCount(context)) return false;
    if (ReadListStat(context, list, static_cast<std::uint32_t>(maximum), 0) <= 0) return false;

    const std::int16_t stat = Read<std::int16_t>(skill, SkillAuraStat1Offset);
    if (stat < 0 || stat >= StatCount(context)) return false;
    pool = ReadListStat(context, list, static_cast<std::uint32_t>(stat), 0);
    return true;
}

void TakeDirect(std::uint8_t* hit, const DirectKind& kind, std::int64_t& budget) noexcept {
    if (budget <= 0) return;
    const std::int32_t value = Read<std::int32_t>(hit, kind.offset);
    if (value <= 0) return;
    const std::int64_t take = value < budget ? value : budget;
    Write<std::int32_t>(hit, kind.offset, static_cast<std::int32_t>(value - take));
    budget -= take;
    Absorbed[kind.kind].fetch_add(static_cast<std::uint64_t>(take), std::memory_order_relaxed);
}

// Poison of perFrame per frame for length frames costs perFrame * (length + 1).
// Returns how much perFrame drops. When the pool cannot pay all of it, the drop
// is rounded up and the pool empties.
auto TakePerFrame(std::int32_t perFrame, std::int32_t length, std::int64_t& budget) noexcept -> std::int32_t {
    if (perFrame <= 0 || budget <= 0) return 0;
    const std::int64_t frames = length > 0 ? static_cast<std::int64_t>(length) + 1 : 1;
    const std::int64_t cost   = static_cast<std::int64_t>(perFrame) * frames;
    if (budget >= cost) {
        budget -= cost;
        return perFrame;
    }
    const std::int64_t drop = (budget + frames - 1) / frames;
    budget = 0;
    return static_cast<std::int32_t>(drop);
}

// The same test as the damage commit at 0x44D14D .. 0x44D1C9.
auto PoisonPerSource(const void* game) noexcept -> bool {
    const std::uint32_t context = Read<std::uint8_t>(game, GameDataContextOffset);
    const std::uint8_t* tables = GameDataTables(context);
    if (tables == nullptr || Read<std::uint64_t>(tables, StatesCountOffset) <= PoisonState) return false;
    const auto* rows = Read<const std::uint8_t*>(tables, StatesRowsOffset);
    return rows != nullptr && rows[PoisonState * StatesRowSize + StatesPerSourceOffset] != 0;
}

void TakePoison(const void* game, std::uint8_t* hit, std::int64_t& budget) noexcept {
    std::int32_t total = Read<std::int32_t>(hit, HitPoisonOffset);
    if (budget <= 0 || total <= 0) return;
    const std::int32_t length = Read<std::int32_t>(hit, HitPoisonLengthOffset);
    const std::int64_t before = budget;

    if (PoisonPerSource(game)) {
        auto* sources = Read<std::uint8_t*>(hit, HitPoisonSourcesOffset);
        const auto count = Read<std::uint64_t>(hit, HitPoisonCountOffset);
        if (count > MaximumPoisonSources || (count != 0 && sources == nullptr)) {
            BadPoisonLists.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        std::int64_t sourceDamage = 0;
        std::int64_t sourceLength = 0;
        for (std::uint64_t index = 0; index < count; ++index) {
            std::uint8_t* source = sources + index * PoisonSourceStride;
            std::int32_t damage = Read<std::int32_t>(source, PoisonSourceDamage);
            const std::int32_t sourceFrames = Read<std::int32_t>(source, PoisonSourceLength);
            const std::int32_t drop = TakePerFrame(damage, sourceFrames, budget);
            if (drop > 0) {
                damage -= drop;
                total  -= drop;
                Write<std::int32_t>(source, PoisonSourceDamage, damage);
            }
            sourceDamage += damage;
            sourceLength += sourceFrames;
        }
        const std::int64_t restDamage = static_cast<std::int64_t>(total) - sourceDamage;
        const std::int64_t restLength = static_cast<std::int64_t>(length) - sourceLength;
        const bool restLasts = (hit[HitPoisonRestFlagA] != 0 || hit[HitPoisonRestFlagB] != 0)
            && restLength > 0;
        if (restDamage > 0 && restDamage <= 0x7FFFFFFF && restLength <= 0x7FFFFFFF) {
            total -= TakePerFrame(static_cast<std::int32_t>(restDamage),
                restLasts ? static_cast<std::int32_t>(restLength) : 0, budget);
        }
    } else {
        total -= TakePerFrame(total, length, budget);
    }

    Write<std::int32_t>(hit, HitPoisonOffset, total);
    Absorbed[KindPoison].fetch_add(static_cast<std::uint64_t>(before - budget), std::memory_order_relaxed);
}

std::int64_t __fastcall HookEventFunc40(void* game, std::uint64_t event, void* unit, void* attacker,
        void* hit, std::uint32_t skillId, std::uint32_t arg7, std::uint32_t arg8,
        std::uint64_t arg9) noexcept {
    std::int32_t pool = 0;
    if (!Armed.load(std::memory_order_relaxed) || game == nullptr || unit == nullptr || hit == nullptr
            || !ReadPool(game, unit, skillId, pool) || pool <= 0) {
        if (Armed.load(std::memory_order_relaxed)) Vanilla.fetch_add(1, std::memory_order_relaxed);
        return Original(game, event, unit, attacker, hit, skillId, arg7, arg8, arg9);
    }

    auto* bytes = static_cast<std::uint8_t*>(hit);
    std::int64_t budget = pool;
    for (const DirectKind& kind : DirectKinds) TakeDirect(bytes, kind, budget);
    TakePoison(game, bytes, budget);

    Hits.fetch_add(1, std::memory_order_relaxed);
    if (budget == 0) Breaks.fetch_add(1, std::memory_order_relaxed);

    alignas(16) std::uint8_t standIn[StandInBytes] {};
    Write<std::int32_t>(standIn, HitPhysicalOffset, static_cast<std::int32_t>(pool - budget));
    return Original(game, event, unit, attacker, standIn, skillId, arg7, arg8, arg9);
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

auto Install() noexcept -> bool {
    if (!Context->CheckExpectedBytes(EventFunc40Rva, EventFunc40Body, ByteCount(EventFunc40Body))) {
        D2RL::LogError(Context, "AbsorbAllDamage: auraeventfunc 40 at 0x524470 does not match "
            "D2RLoader 1.3.1; wrong game build or another plugin patched it.");
        return false;
    }
    if (!Context->CheckExpectedBytes(PoisonSourcesSiteRva, PoisonSourcesWitness,
            ByteCount(PoisonSourcesWitness))) {
        D2RL::LogError(Context, "AbsorbAllDamage: the poison block at 0x44D145 does not match "
            "D2RLoader 1.3.1; wrong game build or another plugin patched it.");
        return false;
    }

    SkillRecord    = At<SkillRecordFn>(SkillRecordRva);
    StateCount     = At<RowCountFn>(StateCountRva);
    StatCount      = At<RowCountFn>(StatCountRva);
    StateStatList  = At<StateStatListFn>(StateStatListRva);
    ReadListStat   = At<ReadListStatFn>(ReadListStatRva);
    GameDataTables = At<DataTablesFn>(DataTablesRva);

    if (!Context->InstallInlineHook(EventFunc40Rva, EventFunc40Body, HookPrologueBytes,
            &HookEventFunc40, &Original) || Original == nullptr) {
        D2RL::LogError(Context, "AbsorbAllDamage: the hook on auraeventfunc 40 (0x524470) was refused.");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Config parsing (small TOML subset)
// ---------------------------------------------------------------------------

auto Trim(std::string_view value) noexcept -> std::string_view {
    std::size_t first = 0;
    while (first < value.size()
            && (value[first] == ' ' || value[first] == '\t' || value[first] == '\r'
                || value[first] == '\n')) {
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

void ParseConfig(std::string_view text) noexcept {
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        const std::size_t breakAt = text.find('\n', cursor);
        const std::size_t end = breakAt == std::string_view::npos ? text.size() : breakAt;
        std::string_view line = Trim(text.substr(cursor, end - cursor));
        cursor = end + 1;

        const std::size_t comment = line.find('#');
        if (comment != std::string_view::npos) line = Trim(line.substr(0, comment));
        if (line.empty() || line.front() == '[') continue;

        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) continue;
        const std::string_view key   = Trim(line.substr(0, equals));
        const std::string_view value = Trim(line.substr(equals + 1));
        if (key == "enabled") {
            if (value == "true") {
                Settings.enabled = true;
            } else if (value == "false") {
                Settings.enabled = false;
            } else {
                D2RL::LogWarn(Context, "AbsorbAllDamage: enabled is not true or false; keeping true.");
            }
        }
    }
}

void ReadConfiguration() noexcept {
    if (!Context->EnsureConfig(DefaultConfigToml)) {
        D2RL::LogWarn(Context, "AbsorbAllDamage: config file could not be created; using defaults.");
        return;
    }
    std::string buffer(MaximumConfigBytes, '\0');
    std::uint32_t required = 0;
    if (!Context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size()), &required)) {
        D2RL::LogWarn(Context, "AbsorbAllDamage: config file could not be read; using defaults.");
        return;
    }
    buffer.resize(std::strlen(buffer.c_str()));
    ParseConfig(buffer);
}

// ---------------------------------------------------------------------------
// Console command
// ---------------------------------------------------------------------------

auto Life(DamageKind kind) noexcept -> unsigned long long {
    return static_cast<unsigned long long>(Absorbed[kind].load(std::memory_order_relaxed) / 256U);
}

auto __cdecl StatusCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command,
        void*) noexcept -> D2RL::ConsoleCommandResult {
    if (command == nullptr || command->plugin == nullptr) return D2RL::ConsoleCommandResult::Failed;
    const char* state = !Settings.enabled ? "disabled by config"
        : Armed.load(std::memory_order_relaxed) ? "armed" : "not installed, see log";
    char message[640];
    std::snprintf(message, sizeof(message),
        "Absorb All Damage: %s | hits with a pool %llu | pools emptied %llu | absorbed (life): "
        "%s %llu, %s %llu, %s %llu, %s %llu, %s %llu, %s %llu | left to the game (no pool) %llu | "
        "unreadable poison lists %llu",
        state,
        static_cast<unsigned long long>(Hits.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(Breaks.load(std::memory_order_relaxed)),
        KindNames[KindPhysical], Life(KindPhysical), KindNames[KindFire], Life(KindFire),
        KindNames[KindLightning], Life(KindLightning), KindNames[KindCold], Life(KindCold),
        KindNames[KindMagic], Life(KindMagic), KindNames[KindPoison], Life(KindPoison),
        static_cast<unsigned long long>(Vanilla.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(BadPoisonLists.load(std::memory_order_relaxed)));
    command->plugin->WriteConsoleMessage(message);
    return D2RL::ConsoleCommandResult::Handled;
}

void RegisterStatusCommand() noexcept {
    if (!Context->RegisterConsoleCommand("absorballdamage", &StatusCommand,
            "Reports Absorb All Damage state and counters.")) {
        D2RL::LogWarn(Context, "AbsorbAllDamage: the status console command was refused.");
    }
}

constexpr D2RL::PluginInfo Info{
    .infoSize    = D2RL::PluginInfoSize,
    .abiVersion  = D2RL_PLUGIN_ABI_VERSION,
    .id          = "celestialrayone.absorb-all-damage",
    .name        = "Absorb All Damage",
    .version     = "1.0.0",
    .author      = "CelestialRayOne",
    .description = "auraeventfunc 40 absorbs physical, fire, lightning, cold, magic and poison damage.",
    .flags       = D2RL::PluginFlags::Server | D2RL::PluginFlags::NativeHooks,
};

}  // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
    return &Info;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
    if (!D2RL::HasContext(context)) return false;
    Context = context;
    Base    = context->exeBase;
    if (Base == 0) {
        D2RL::LogError(Context, "AbsorbAllDamage: the game image base is unknown.");
        return false;
    }

    ReadConfiguration();
    RegisterStatusCommand();
    if (!Settings.enabled) {
        D2RL::LogInfo(Context, "AbsorbAllDamage: disabled by configuration.");
        return true;
    }
    if (!Install()) return false;

    Installed.store(true, std::memory_order_relaxed);
    Armed.store(true, std::memory_order_relaxed);
    D2RL::LogInfo(Context, "AbsorbAllDamage: auraeventfunc 40 now absorbs physical, fire, lightning, "
        "cold, magic and poison damage.");
    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    Armed.store(false, std::memory_order_relaxed);
}

}  // namespace CelestialRayOne::AbsorbAllDamage
