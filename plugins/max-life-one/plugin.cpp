// Max Life One
//
// Adds a configurable itemstatcost stat (default id 512). While a player or
// monster has that stat, its maximum life is 1 and nothing can raise or lower
// it. Damage still kills it.
//
// Everything below was read out of D2RLoader 1.3.0 and disassembled before
// being relied on: the D2RLoader.exe process dump (md5
// afdf8d05662885e48af48b8208892178) and D2RCore.dll (SHA-256
// ae1ea9b7f97af5b89a550281e6a6c6b6e9c74e73ac8759e6558b40e751428cd0).
// D2RCore addresses are RVAs into that D2RCore.dll.
//
// ---------------------------------------------------------------------------
// Where maximum life lives in 1.3.0
// ---------------------------------------------------------------------------
//   D2RLoader 1.3.0 moved the whole stat store into D2RCore. The game's stat
//   functions are 10-byte import thunks (jmp [rip+slot] / nop x4) into D2RCore
//   exports:
//
//     0x2F4D20  max life of a unit        -> ReadWideMaxLife
//     0x2F5020  STATLIST_GetUnitStat      -> ReadWideUnitStat
//     0x2F5C60  STATLIST_UnitGetStatValue -> ReadWideItemEventStat
//     0x2F5170  (?, unit, stat)           -> ReadWideUnitStatCallback
//     0x2F6CD0  (context, list, stat, layer) -> ReadWideEffectiveStat
//     0x2F5D90  (context, list, stat)     -> ReadWideEffectiveStatZero
//     0x2F7D10  STATLIST_SetUnitStat      -> SetWideUnitStat
//     0x46FDD0  server stat callback      -> OnWideServerStatChanged
//
//   The total of maxhp is computed inside D2RCore (evaluate 0x3E6540, commit
//   0x3E6EF0). No game code computes maximum life any more, so the rule sits
//   on the D2RCore side of those thunks.
//
// ---------------------------------------------------------------------------
// Why the hooks sit on the D2RCore exports, not on the game thunks
// ---------------------------------------------------------------------------
//   dodge-cap, critical-strike-damage and smite-hit-cap verify the thunk at
//   0x2F5020 byte for byte when they load (FF 25 / nop x4 / old body). An
//   inline hook on that thunk rewrites its first five bytes, so whichever of
//   them loads after this plugin would refuse to install.
//
//   This plugin hooks the exports inside D2RCore.dll, resolved by name with
//   GetProcAddress. It uses no game RVA. Every caller of the thunks (the game,
//   D2RCore features that call the game entry points, other plugins) still
//   goes through the untouched thunks and reaches the hook.
//
//   D2RLoader 1.3.1's hook installer only accepts hook sites inside D2R.exe
//   ("patch range is outside D2R.exe"), so the plugin installs these hooks
//   itself. For each export it checks the first instructions byte for byte
//   against D2RCore.dll 1.3.1, copies them into a trampoline that ends with an
//   absolute jump back past them (rel32 calls and jumps and the RIP-relative
//   cookie load re-aimed at their original targets), and turns the export
//   entry into jmp rel32 to a relay that jumps to the hook. Relays and
//   trampolines share one page allocated within rel32 reach of D2RCore. The
//   entries are restored on unload and when a later hook fails to install.
//
//   Every export's first bytes relocate cleanly (checked per export against
//   D2RCore.dll 1.3.1, including that nothing branches back into them): sub rsp / call rel32 (ReadWideUnitStat, ReadWideEffectiveStat),
//   jmp rel32 (ReadWideItemEventStat, ReadWideMaxLife), sub rsp / mov / xor
//   (ReadWideUnitStatCallback, ReadWideEffectiveStatZero), sub rsp / mov
//   rax,[rip+cookie] (OnWideServerStatChanged), push x4 (SetWideUnitStat,
//   AddWideUnitStat). None of them branches back into its first five bytes.
//
// ---------------------------------------------------------------------------
// ABIs (x64, from the export bodies)
// ---------------------------------------------------------------------------
//   ReadWideMaxLife          (unit) -> int32
//   ReadWideUnitStat         (unit, stat, layer) -> int32
//   ReadWideItemEventStat    (unit, stat, layer) -> int32   (same body)
//   ReadWideUnitStatCallback (unused, unit, stat) -> int32  (layer 0)
//   ReadWideEffectiveStat    (context byte, list, stat, layer) -> int32
//   ReadWideEffectiveStatZero(context byte, list, stat) -> int32 (layer 0)
//     reads the list's totals when the list is extended ([list+1Ch] < 0),
//     its own base values otherwise.
//   SetWideUnitStat          (unit, stat, value, layer) -> bool
//   AddWideUnitStat          (unit, stat, delta, layer) -> void
//   OnWideServerStatChanged  (game, owner, unit, key, old, new) -> void
//     key = stat << 32 | layer.
//
//   Fields the rule reads, each read the same way by D2RCore's own
//   ReadWideUnitStat (0x3E2E40):
//     unit+00h   dword  unit type (0 player, 1 monster)
//     unit+88h   qword  stat list
//     list+1Ch   dword  flags, sign bit = extended list
//     list+A0h   qword  owner unit of an extended list
//
// ---------------------------------------------------------------------------
// What the engine does when maximum life changes
// ---------------------------------------------------------------------------
//   When the total of a stat whose itemstatcost row has fCallback changes,
//   D2RCore calls the unit's stat callback (0x3E5E00 / 0x3E6EF0:
//   [list+2AF0h] = callback, [list+2AF8h] = game, [list+0A0h] = owner,
//   test byte [isc+5],4 = fCallback). The callback installed on server units
//   is the game's 0x46FDD0 (loaded at unit creation by 0x4252C0 and
//   0x543120), the thunk to OnWideServerStatChanged. Its branch for maxhp,
//   maxmana and maxstamina (0x3D6CB9):
//
//     if (old > 0 && old != new && current > 0)
//         current = clamp(current * new / max(old, 256), 1, new)
//     then, for a monster's life, damage regeneration is rebuilt from new.
//
//   maxhp has fCallback in vanilla, hitpoints does not. The client callback
//   (OnWideClientStatChanged, 0x3D73E0) has no life logic: the client's life
//   value comes from the server.
//
// ---------------------------------------------------------------------------
// The rule
// ---------------------------------------------------------------------------
//   "Has the stat": total of the configured stat, layer 0, is non-zero, on a
//   player or monster. Life is stored in 256ths, so 1 life = 256, the same
//   value D2RCore itself writes as the life floor after a life cost (0x3E7B32:
//   mov r9d,100h for stat 6).
//
//   Reads. ReadWideMaxLife, and stat 7 layer 0 through ReadWideUnitStat,
//   ReadWideItemEventStat, ReadWideUnitStatCallback, ReadWideEffectiveStat and
//   ReadWideEffectiveStatZero (the last two only on the unit's own extended
//   list) answer 256. Base reads (ReadWideUnitBaseStat, ReadWideListStat) are
//   left alone: level-up builds the new base maxhp from them, and overriding
//   them would destroy the character's real base life.
//
//   Callback. The configured stat appearing is forwarded to the engine as
//   maxhp going from its real total to 256, and disappearing as 256 back to
//   the real total, so the engine's own rescale moves current life. A real
//   maxhp change while the stat is present is forwarded as 256 -> 256, which
//   the engine treats as no change. The configured stat's row needs fCallback
//   or the engine never reports it; the plugin checks every itemstatcost bank
//   after each table load and stays off while a bank has the row without it.
//
//   Writes. SetWideUnitStat / AddWideUnitStat on stat 6 layer 0 are held at
//   256 while the stat is present. Monster spawn and unique-modifier code
//   write life directly, not through a maximum life read, possibly after the
//   stat is already on the unit. D2RCore's own internal life writes were all
//   checked (0x3E3E60 call sites): the maxhp rescale above, Crushing Blow
//   (0x3D91AD, life minus damage, floored at 0) and blood mana running out
//   (0x3E7B32, life set to exactly 256) are the only ones, and none of them
//   can go above 1 life.

// ---------------------------------------------------------------------------
// Client display
// ---------------------------------------------------------------------------
//   The client reads maximum life through the same hooked exports, from its
//   own copy of the unit:
//
//     0x14F3959  character screen life line  j_ReadWideMaxLife -> "%d / %d"
//     0x266A8D   HUD life check              j_ReadWideMaxLife
//
//   So the client shows 1 only when its copy of the unit carries the stat.
//   The server copy gets it wherever the stat comes from; the client copy
//   gets it only through the engine's own sync, which each itemstatcost
//   column gates:
//
//     states (auras, curses, skill states): the 0xA8 state stat packet
//       (D2RCore 0x3EFD51) skips every stat whose Send Bits is 0.
//     stats set on a player: only rows with Saved (flags bit 11) enter the
//       player's changed-stat list (D2RCore 0x3E3A64), which is what
//       VisitWideChangedStats (0x7AA370) hands the game for the player's own
//       client.
//     items: the item writer skips every stat whose Save Bits is 0
//       (0x37F158: cmp byte [row+15h],0). In this image the same writer
//       also clamps the stat id to 1FFh in 9 bits (0x37F186 .. 0x37F1A1).
//
//   The plugin prints those three columns after each table load, and logs
//   once per player when the server copy has had the stat for 3 seconds but
//   the client copy still does not.

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

namespace CelestialRayOne::MaxLifeOne {
namespace {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

constexpr std::uint32_t StatHitpoints = 6;
constexpr std::uint32_t StatMaxHp     = 7;
constexpr std::int32_t  OneLife       = 256;
constexpr std::uint64_t MaxHpKey      = static_cast<std::uint64_t>(StatMaxHp) << 32;

constexpr std::uint32_t UnitTypePlayer  = 0;
constexpr std::uint32_t UnitTypeMonster = 1;

// D2RCore bounds every stat id with cmp edx,8000h.
constexpr std::int64_t MaximumStatId      = 0x7FFF;
constexpr std::size_t  MaximumConfigBytes = 32'768;

constexpr std::size_t UnitTypeOffset      = 0x00;
constexpr std::size_t UnitIdOffset        = 0x08;
constexpr std::size_t UnitStatListOffset  = 0x88;
constexpr std::size_t StatListFlagsOffset = 0x1C;
constexpr std::size_t StatListOwnerOffset = 0xA0;

// Compiled itemstatcost row: flags dword at +4 (fCallback = bit 10, Saved =
// bit 11), Send Bits byte at +8, Save Bits byte at +15h.
constexpr std::size_t   IscFlagsOffset    = 4;
constexpr std::uint32_t IscFlagCallback   = 0x400;
constexpr std::uint32_t IscFlagSaved      = 0x800;
constexpr std::size_t   IscSendBitsOffset = 0x08;
constexpr std::size_t   IscSaveBitsOffset = 0x15;

// How long a player's client copy may lag behind the server copy before the
// missing stat is reported.
constexpr std::uint64_t ClientCopyGraceMs = 3000;

constexpr char CoreModuleName[] = "D2RCore.dll";

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

struct Config {
    bool         enabled = true;
    std::int64_t statId  = 512;
};

constexpr char DefaultConfigToml[] =
    "# Max Life One\n"
    "#\n"
    "# Adds a configurable itemstatcost stat. While a player or monster has that\n"
    "# stat, its maximum life is 1 and nothing can raise or lower it. Damage still\n"
    "# kills it.\n"
    "#\n"
    "# In game:\n"
    "# - Gaining the stat is a maximum life change like any other: the unit keeps\n"
    "#   its life percentage, so a unit at full life ends at 1 of 1 and a wounded\n"
    "#   unit ends below 1 life.\n"
    "# - Losing the stat changes maximum life back the same way, so a unit at\n"
    "#   1 of 1 returns to full life.\n"
    "# - While the stat is present, items, level ups, Battle Orders, vitality and\n"
    "#   life penalties do not change maximum life, and potions, healing, leech and\n"
    "#   regeneration stop at 1 life.\n"
    "#\n"
    "# Data requirement, itemstatcost.txt, the row for stat_id:\n"
    "# - fCallback must be 1. Without it the game never reports the stat appearing\n"
    "#   or disappearing, so current life would not drop to 1. The plugin checks\n"
    "#   the row every time the tables load, and stays off (with an error in its\n"
    "#   log) while a data set has the row without fCallback.\n"
    "#\n"
    "# Client display (character screen, life orb):\n"
    "# - The client reads maximum life from its own copy of the unit, so it shows 1\n"
    "#   only when the stat reaches that copy. The server enforces the rule either\n"
    "#   way. The engine sends the stat to the client through the same row:\n"
    "#   - states (auras, curses, skill states): Send Bits must be above 0.\n"
    "#   - stats set directly on a player: Saved must be 1.\n"
    "#   - items: Save Bits must be above 0.\n"
    "# - The plugin prints these three columns after each table load, and logs a\n"
    "#   warning when a player's client copy is still missing the stat 3 seconds\n"
    "#   after the server copy got it.\n"
    "#\n"
    "# Console command: maxlifeone (state, table check, client sync, hooks,\n"
    "# counters).\n"
    "\n"
    "[max_life_one]\n"
    "\n"
    "# Master switch.\n"
    "enabled = true\n"
    "\n"
    "# itemstatcost.txt id of the stat. Any non-zero value on a player or monster\n"
    "# applies the rule. 6 (hitpoints) and 7 (maxhp) are refused.\n"
    "stat_id = 512\n";

// ---------------------------------------------------------------------------
// Native function types
// ---------------------------------------------------------------------------

using ReadMaxLifeFn = std::int32_t(__fastcall*)(void* unit) noexcept;
using ReadUnitStatFn =
    std::int32_t(__fastcall*)(void* unit, std::uint32_t stat, std::uint32_t layer) noexcept;
using ReadUnitStatCallbackFn =
    std::int32_t(__fastcall*)(void* unused, void* unit, std::uint32_t stat) noexcept;
// The data context arrives as a byte in cl; the full register is forwarded
// unchanged.
using ReadEffectiveStatFn = std::int32_t(__fastcall*)(
    std::uintptr_t context, void* statList, std::uint32_t stat, std::uint32_t layer) noexcept;
using ReadEffectiveStatZeroFn = std::int32_t(__fastcall*)(
    std::uintptr_t context, void* statList, std::uint32_t stat) noexcept;
using StatChangedFn = void(__fastcall*)(void* game, void* owner, void* unit,
    std::uint64_t key, std::int32_t oldValue, std::int32_t newValue) noexcept;
using SetUnitStatFn = bool(__fastcall*)(
    void* unit, std::uint32_t stat, std::int32_t value, std::uint32_t layer) noexcept;
using AddUnitStatFn = void(__fastcall*)(
    void* unit, std::uint32_t stat, std::int32_t delta, std::uint32_t layer) noexcept;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

enum class BankState : std::uint8_t {
    NotChecked = 0,
    NoRow,
    Ready,
    MissingCallback,
    Unavailable,
};

const D2RL::PluginContext*       Context{};
const D2RL::DataTableServiceV1*  DataTables{};
Config                           Settings{};
std::uint32_t                    RuleStatId = 512;

// Written by MinHook before each hook is enabled.
void* OriginalReadMaxLife{};
void* OriginalReadUnitStat{};
void* OriginalReadItemEventStat{};
void* OriginalReadUnitStatCallback{};
void* OriginalReadEffectiveStat{};
void* OriginalReadEffectiveStatZero{};
void* OriginalServerStatChanged{};
void* OriginalSetUnitStat{};
void* OriginalAddUnitStat{};

// The rule runs only while every hook is installed and the itemstatcost
// check passed. Until then every hook forwards untouched. Either can happen
// first, so both paths arm.
std::atomic<bool> HooksReady{};
std::atomic<bool> TablesOk{};
std::atomic<bool> Armed{};

std::array<std::atomic<BankState>, 3> Banks{};
std::atomic<std::uint64_t>            TablesRevision{};

// Sync columns of the checked row: Send Bits | Save Bits << 8 | Saved << 16
// | present << 24.
std::atomic<std::uint32_t> RowSync{};

// Players whose server copy carries the stat. since = GetTickCount64 at the
// gain, 0 = free slot.
struct ServerPlayer {
    std::atomic<std::uint32_t> id{};
    std::atomic<std::uint64_t> since{};
    std::atomic<bool>          reported{};
};
std::array<ServerPlayer, 16> ServerPlayers{};
std::atomic<std::uint64_t>   ClientCopiesMissing{};

std::atomic<std::uint64_t> MaxLifeReads{};
std::atomic<std::uint64_t> Gains{};
std::atomic<std::uint64_t> Losses{};
std::atomic<std::uint64_t> HeldChanges{};
std::atomic<std::uint64_t> ClampedWrites{};

// ---------------------------------------------------------------------------
// Field access
// ---------------------------------------------------------------------------

auto ReadU32(const void* base, std::size_t offset) noexcept -> std::uint32_t {
    std::uint32_t value = 0;
    std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(value));
    return value;
}

auto ReadPointer(const void* base, std::size_t offset) noexcept -> void* {
    void* value = nullptr;
    std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(value));
    return value;
}

// ---------------------------------------------------------------------------
// Originals
// ---------------------------------------------------------------------------

auto CallReadUnitStat(void* unit, std::uint32_t stat, std::uint32_t layer) noexcept
        -> std::int32_t {
    return reinterpret_cast<ReadUnitStatFn>(OriginalReadUnitStat)(unit, stat, layer);
}

// ---------------------------------------------------------------------------
// The rule
// ---------------------------------------------------------------------------

auto IsLifeUnit(const void* unit) noexcept -> bool {
    const std::uint32_t type = ReadU32(unit, UnitTypeOffset);
    return type == UnitTypePlayer || type == UnitTypeMonster;
}

auto UnitHasRule(void* unit) noexcept -> bool {
    if (unit == nullptr || !Armed.load(std::memory_order_relaxed)) return false;
    if (!IsLifeUnit(unit)) return false;
    return CallReadUnitStat(unit, RuleStatId, 0) != 0;
}

// The unit whose own extended stat list this is, or null.
auto OwnerOfUnitList(void* statList) noexcept -> void* {
    if (statList == nullptr) return nullptr;
    if (static_cast<std::int32_t>(ReadU32(statList, StatListFlagsOffset)) >= 0) {
        return nullptr;
    }
    void* owner = ReadPointer(statList, StatListOwnerOffset);
    if (owner == nullptr || ReadPointer(owner, UnitStatListOffset) != statList) {
        return nullptr;
    }
    return owner;
}

void ForgetServerPlayers() noexcept {
    for (ServerPlayer& player : ServerPlayers) {
        player.since.store(0, std::memory_order_relaxed);
    }
}

void NoteServerPlayer(const void* owner, bool has) noexcept {
    if (ReadU32(owner, UnitTypeOffset) != UnitTypePlayer) return;
    const std::uint32_t id = ReadU32(owner, UnitIdOffset);
    ServerPlayer* emptySlot = nullptr;
    for (ServerPlayer& player : ServerPlayers) {
        if (player.since.load(std::memory_order_relaxed) == 0) {
            if (emptySlot == nullptr) emptySlot = &player;
            continue;
        }
        if (player.id.load(std::memory_order_relaxed) == id) {
            if (!has) player.since.store(0, std::memory_order_relaxed);
            return;
        }
    }
    if (has && emptySlot != nullptr) {
        emptySlot->id.store(id, std::memory_order_relaxed);
        emptySlot->reported.store(false, std::memory_order_relaxed);
        emptySlot->since.store(GetTickCount64(), std::memory_order_relaxed);
    }
}

void DescribeRowSync(char* out, std::size_t outSize) noexcept {
    const std::uint32_t sync = RowSync.load(std::memory_order_relaxed);
    if ((sync >> 24) == 0) {
        std::snprintf(out, outSize, "row not checked");
        return;
    }
    std::snprintf(out, outSize, "states %s (Send Bits %u), stats set on a player %s "
        "(Saved %u), items %s (Save Bits %u)",
        (sync & 0xFF) != 0 ? "yes" : "NO", sync & 0xFF,
        ((sync >> 16) & 1) != 0 ? "yes" : "NO", (sync >> 16) & 1,
        ((sync >> 8) & 0xFF) != 0 ? "yes" : "NO", (sync >> 8) & 0xFF);
}

// A player read without the rule whose server copy has had the stat past the
// grace period is a client copy the stat never reached.
void CheckClientCopy(const void* unit) noexcept {
    if (ReadU32(unit, UnitTypeOffset) != UnitTypePlayer) return;
    const std::uint32_t id = ReadU32(unit, UnitIdOffset);
    for (ServerPlayer& player : ServerPlayers) {
        const std::uint64_t since = player.since.load(std::memory_order_relaxed);
        if (since == 0 || player.id.load(std::memory_order_relaxed) != id) continue;
        if (GetTickCount64() - since < ClientCopyGraceMs) return;
        if (player.reported.exchange(true, std::memory_order_relaxed)) return;
        ClientCopiesMissing.fetch_add(1, std::memory_order_relaxed);
        char sync[160];
        DescribeRowSync(sync, sizeof(sync));
        D2RL::LogWarnF(Context,
            "MaxLifeOne: player %u has stat %u on the server but not on the client, so the "
            "character screen and life orb show the real maximum life. The client copy only "
            "receives the stat through its itemstatcost row: %s.", id, RuleStatId, sync);
        return;
    }
}

// UnitHasRule for the read hooks: also reports client copies the stat never
// reached.
auto ReadRule(void* unit) noexcept -> bool {
    if (UnitHasRule(unit)) return true;
    if (unit != nullptr && Armed.load(std::memory_order_relaxed)) CheckClientCopy(unit);
    return false;
}

auto AnswerOneLife() noexcept -> std::int32_t {
    MaxLifeReads.fetch_add(1, std::memory_order_relaxed);
    return OneLife;
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

auto __fastcall HookReadMaxLife(void* unit) noexcept -> std::int32_t {
    if (ReadRule(unit)) return AnswerOneLife();
    return reinterpret_cast<ReadMaxLifeFn>(OriginalReadMaxLife)(unit);
}

auto __fastcall HookReadUnitStat(void* unit, std::uint32_t stat, std::uint32_t layer) noexcept
        -> std::int32_t {
    if (stat == StatMaxHp && layer == 0 && ReadRule(unit)) return AnswerOneLife();
    return CallReadUnitStat(unit, stat, layer);
}

auto __fastcall HookReadItemEventStat(void* unit, std::uint32_t stat, std::uint32_t layer) noexcept
        -> std::int32_t {
    if (stat == StatMaxHp && layer == 0 && ReadRule(unit)) return AnswerOneLife();
    return reinterpret_cast<ReadUnitStatFn>(OriginalReadItemEventStat)(unit, stat, layer);
}

auto __fastcall HookReadUnitStatCallback(void* unused, void* unit, std::uint32_t stat) noexcept
        -> std::int32_t {
    if (stat == StatMaxHp && ReadRule(unit)) return AnswerOneLife();
    return reinterpret_cast<ReadUnitStatCallbackFn>(OriginalReadUnitStatCallback)(
        unused, unit, stat);
}

auto __fastcall HookReadEffectiveStat(std::uintptr_t context, void* statList, std::uint32_t stat,
        std::uint32_t layer) noexcept -> std::int32_t {
    if (stat == StatMaxHp && layer == 0 && Armed.load(std::memory_order_relaxed)
            && ReadRule(OwnerOfUnitList(statList))) {
        return AnswerOneLife();
    }
    return reinterpret_cast<ReadEffectiveStatFn>(OriginalReadEffectiveStat)(
        context, statList, stat, layer);
}

auto __fastcall HookReadEffectiveStatZero(std::uintptr_t context, void* statList,
        std::uint32_t stat) noexcept -> std::int32_t {
    if (stat == StatMaxHp && Armed.load(std::memory_order_relaxed)
            && ReadRule(OwnerOfUnitList(statList))) {
        return AnswerOneLife();
    }
    return reinterpret_cast<ReadEffectiveStatZeroFn>(OriginalReadEffectiveStatZero)(
        context, statList, stat);
}

void __fastcall HookServerStatChanged(void* game, void* owner, void* unit, std::uint64_t key,
        std::int32_t oldValue, std::int32_t newValue) noexcept {
    const auto original = reinterpret_cast<StatChangedFn>(OriginalServerStatChanged);
    const auto stat  = static_cast<std::uint32_t>(key >> 32);
    const auto layer = static_cast<std::uint32_t>(key);

    if (!Armed.load(std::memory_order_relaxed) || game == nullptr || owner == nullptr
            || layer != 0 || !IsLifeUnit(owner)) {
        original(game, owner, unit, key, oldValue, newValue);
        return;
    }

    if (stat == RuleStatId) {
        original(game, owner, unit, key, oldValue, newValue);
        const bool had = oldValue != 0;
        const bool has = newValue != 0;
        if (had == has) return;
        NoteServerPlayer(owner, has);
        const std::int32_t realMaximum = CallReadUnitStat(owner, StatMaxHp, 0);
        if (realMaximum <= 0) return;
        if (has) {
            original(game, owner, unit, MaxHpKey, realMaximum, OneLife);
            Gains.fetch_add(1, std::memory_order_relaxed);
        } else {
            original(game, owner, unit, MaxHpKey, OneLife, realMaximum);
            Losses.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }

    if (stat == StatMaxHp && CallReadUnitStat(owner, RuleStatId, 0) != 0) {
        original(game, owner, unit, key, OneLife, OneLife);
        HeldChanges.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    original(game, owner, unit, key, oldValue, newValue);
}

auto __fastcall HookSetUnitStat(void* unit, std::uint32_t stat, std::int32_t value,
        std::uint32_t layer) noexcept -> bool {
    if (stat == StatHitpoints && layer == 0 && value > OneLife && UnitHasRule(unit)) {
        value = OneLife;
        ClampedWrites.fetch_add(1, std::memory_order_relaxed);
    }
    return reinterpret_cast<SetUnitStatFn>(OriginalSetUnitStat)(unit, stat, value, layer);
}

void __fastcall HookAddUnitStat(void* unit, std::uint32_t stat, std::int32_t delta,
        std::uint32_t layer) noexcept {
    if (stat == StatHitpoints && layer == 0 && delta > 0 && UnitHasRule(unit)) {
        const std::int64_t current = CallReadUnitStat(unit, StatHitpoints, 0);
        if (current + delta > OneLife) {
            const std::int64_t allowed = OneLife - current;
            ClampedWrites.fetch_add(1, std::memory_order_relaxed);
            if (allowed == 0) return;
            delta = static_cast<std::int32_t>(allowed);
        }
    }
    reinterpret_cast<AddUnitStatFn>(OriginalAddUnitStat)(unit, stat, delta, layer);
}

// ---------------------------------------------------------------------------
// Hook installation
// ---------------------------------------------------------------------------

// One instruction of an export's entry. dispOffset is 0 for an instruction
// copied as is, else the offset of its rel32 or RIP disp32, which the
// trampoline re-aims at the same absolute target.
struct Instruction {
    std::uint8_t length;
    std::uint8_t dispOffset;
};

constexpr std::size_t MaxPatchSize = 11;

struct HookSpec {
    const char*                             exportName;
    void*                                   target;
    void**                                  original;
    std::uint8_t                            patchSize;
    std::array<std::uint8_t, MaxPatchSize>  expected;      // D2RCore.dll 1.3.1
    std::array<Instruction, 2>              instructions;  // together patchSize bytes
    bool                                    installed;
    std::uint8_t*                           entry;
    std::array<std::uint8_t, MaxPatchSize>  written;
};

// ReadWideUnitStat first: the rule reads the configured stat through it.
std::array<HookSpec, 9> Hooks{{
    {"ReadWideUnitStat", reinterpret_cast<void*>(&HookReadUnitStat), &OriginalReadUnitStat,
        9, {0x48, 0x83, 0xEC, 0x28, 0xE8, 0xE7, 0x6D, 0xBA, 0xFF},
        {{{4, 0}, {5, 1}}}, false, nullptr, {}},
    {"ReadWideItemEventStat", reinterpret_cast<void*>(&HookReadItemEventStat),
        &OriginalReadItemEventStat,
        5, {0xE9, 0xDB, 0x6D, 0xBA, 0xFF},
        {{{5, 1}, {0, 0}}}, false, nullptr, {}},
    {"ReadWideMaxLife", reinterpret_cast<void*>(&HookReadMaxLife), &OriginalReadMaxLife,
        5, {0xE9, 0xAB, 0x67, 0xBA, 0xFF},
        {{{5, 1}, {0, 0}}}, false, nullptr, {}},
    {"ReadWideUnitStatCallback", reinterpret_cast<void*>(&HookReadUnitStatCallback),
        &OriginalReadUnitStatCallback,
        7, {0x48, 0x83, 0xEC, 0x28, 0x48, 0x89, 0xD1},
        {{{7, 0}, {0, 0}}}, false, nullptr, {}},
    {"ReadWideEffectiveStat", reinterpret_cast<void*>(&HookReadEffectiveStat),
        &OriginalReadEffectiveStat,
        9, {0x48, 0x83, 0xEC, 0x28, 0xE8, 0x07, 0x5B, 0xBA, 0xFF},
        {{{4, 0}, {5, 1}}}, false, nullptr, {}},
    {"ReadWideEffectiveStatZero", reinterpret_cast<void*>(&HookReadEffectiveStatZero),
        &OriginalReadEffectiveStatZero,
        7, {0x48, 0x83, 0xEC, 0x28, 0x45, 0x31, 0xC9},
        {{{7, 0}, {0, 0}}}, false, nullptr, {}},
    {"OnWideServerStatChanged", reinterpret_cast<void*>(&HookServerStatChanged),
        &OriginalServerStatChanged,
        11, {0x48, 0x83, 0xEC, 0x48, 0x48, 0x8B, 0x05, 0x55, 0x81, 0xEC, 0xFF},
        {{{4, 0}, {7, 3}}}, false, nullptr, {}},
    {"SetWideUnitStat", reinterpret_cast<void*>(&HookSetUnitStat), &OriginalSetUnitStat,
        5, {0x41, 0x56, 0x56, 0x57, 0x53},
        {{{5, 0}, {0, 0}}}, false, nullptr, {}},
    {"AddWideUnitStat", reinterpret_cast<void*>(&HookAddUnitStat), &OriginalAddUnitStat,
        5, {0x41, 0x56, 0x56, 0x57, 0x53},
        {{{5, 0}, {0, 0}}}, false, nullptr, {}},
}};

constexpr std::size_t HookPageBytes    = 4'096;
constexpr std::size_t RelaySlotBytes   = 16;     // FF 25 00 00 00 00 <abs64>
constexpr std::size_t TrampolineOffset = 0x200;
constexpr std::size_t TrampolineBytes  = 32;     // <= 11 copied + 14 jump back
static_assert(Hooks.size() * RelaySlotBytes <= TrampolineOffset);
static_assert(TrampolineOffset + Hooks.size() * TrampolineBytes <= HookPageBytes);
static_assert(MaxPatchSize + 14 <= TrampolineBytes);

std::uint8_t* HookPage{};

auto InstalledHookCount() noexcept -> std::size_t {
    std::size_t count = 0;
    for (const HookSpec& hook : Hooks) {
        if (hook.installed) ++count;
    }
    return count;
}

auto ModuleImageSize(HMODULE module) noexcept -> std::size_t {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        reinterpret_cast<const std::uint8_t*>(module) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE
            || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return 0;
    }
    return nt->OptionalHeader.SizeOfImage;
}

auto IsExecutable(std::uintptr_t address) noexcept -> bool {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) == 0) {
        return false;
    }
    const DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE
        | PAGE_EXECUTE_WRITECOPY;
    return info.State == MEM_COMMIT && (info.Protect & executable) != 0;
}

auto FitsRel32(std::int64_t delta) noexcept -> bool {
    return delta >= INT32_MIN && delta <= INT32_MAX;
}

// A page no further than a rel32 from anywhere in D2RCore.dll.
auto AllocateNearCore(std::uintptr_t coreBase, std::size_t coreSize) noexcept -> std::uint8_t* {
    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    const auto granularity = static_cast<std::uintptr_t>(systemInfo.dwAllocationGranularity);
    auto candidate = (coreBase + coreSize + granularity - 1) & ~(granularity - 1);
    for (; FitsRel32(static_cast<std::int64_t>(candidate + HookPageBytes)
                - static_cast<std::int64_t>(coreBase));
            candidate += granularity) {
        if (void* page = VirtualAlloc(reinterpret_cast<void*>(candidate), HookPageBytes,
                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) {
            return static_cast<std::uint8_t*>(page);
        }
    }
    return nullptr;
}

void WriteAbsoluteJump(std::uint8_t* at, std::uintptr_t target) noexcept {
    static constexpr std::uint8_t JumpQwordRip[]{0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
    std::memcpy(at, JumpQwordRip, sizeof(JumpQwordRip));
    const std::uint64_t address = target;
    std::memcpy(at + sizeof(JumpQwordRip), &address, sizeof(address));
}

// Copies the export's entry instructions to the trampoline and re-aims every
// rel32 and RIP disp32 at its original target. False if one cannot reach.
auto BuildTrampoline(const HookSpec& hook, std::uint8_t* trampoline) noexcept -> bool {
    std::size_t offset = 0;
    for (const Instruction& instruction : hook.instructions) {
        if (instruction.length == 0) break;
        const std::uint8_t* source = hook.entry + offset;
        std::uint8_t* destination  = trampoline + offset;
        std::memcpy(destination, source, instruction.length);
        if (instruction.dispOffset != 0) {
            std::int32_t displacement = 0;
            std::memcpy(&displacement, source + instruction.dispOffset, sizeof(displacement));
            const auto absolute = reinterpret_cast<std::intptr_t>(source) + instruction.length
                + displacement;
            const auto moved = static_cast<std::int64_t>(absolute)
                - (reinterpret_cast<std::intptr_t>(destination) + instruction.length);
            if (!FitsRel32(moved)) return false;
            const auto newDisplacement = static_cast<std::int32_t>(moved);
            std::memcpy(destination + instruction.dispOffset, &newDisplacement,
                sizeof(newDisplacement));
        }
        offset += instruction.length;
    }
    if (offset != hook.patchSize) return false;
    WriteAbsoluteJump(trampoline + offset,
        reinterpret_cast<std::uintptr_t>(hook.entry) + hook.patchSize);
    return true;
}

auto WriteCode(std::uint8_t* at, const std::uint8_t* bytes, std::size_t size) noexcept -> bool {
    DWORD previous = 0;
    if (!VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &previous)) return false;
    std::memcpy(at, bytes, size);
    DWORD ignored = 0;
    VirtualProtect(at, size, previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), at, size);
    return true;
}

// Puts back every export entry that still holds this plugin's jump.
void UninstallHooks() noexcept {
    for (HookSpec& hook : Hooks) {
        if (!hook.installed || hook.entry == nullptr) continue;
        if (std::memcmp(hook.entry, hook.written.data(), hook.patchSize) == 0
                && WriteCode(hook.entry, hook.expected.data(), hook.patchSize)) {
            hook.installed = false;
        }
    }
}

auto InstallHooks() noexcept -> bool {
    const HMODULE core = GetModuleHandleA(CoreModuleName);
    if (core == nullptr) {
        D2RL::LogError(Context, "MaxLifeOne: D2RCore.dll is not loaded.");
        return false;
    }
    const std::size_t coreSize = ModuleImageSize(core);
    if (coreSize == 0) {
        D2RL::LogError(Context, "MaxLifeOne: D2RCore.dll has no readable PE header.");
        return false;
    }
    const auto coreBase = reinterpret_cast<std::uintptr_t>(core);

    // Resolve and verify every export before anything is written.
    for (HookSpec& hook : Hooks) {
        const FARPROC address = GetProcAddress(core, hook.exportName);
        if (address == nullptr) {
            D2RL::LogErrorF(Context,
                "MaxLifeOne: D2RCore.dll has no export named %s. This plugin was built "
                "against D2RLoader 1.3.1.", hook.exportName);
            return false;
        }
        const auto target = reinterpret_cast<std::uintptr_t>(address);
        if (target < coreBase || target + hook.patchSize > coreBase + coreSize
                || !IsExecutable(target)) {
            D2RL::LogErrorF(Context,
                "MaxLifeOne: export %s at 0x%llX is not executable code inside D2RCore.dll.",
                hook.exportName, static_cast<unsigned long long>(target));
            return false;
        }
        hook.entry = reinterpret_cast<std::uint8_t*>(target);
        if (std::memcmp(hook.entry, hook.expected.data(), hook.patchSize) != 0) {
            D2RL::LogErrorF(Context,
                "MaxLifeOne: D2RCore!%s does not start with the instructions of the "
                "D2RLoader 1.3.1 build this plugin was verified against, or another "
                "plugin already hooked it.", hook.exportName);
            return false;
        }
    }

    HookPage = AllocateNearCore(coreBase, coreSize);
    if (HookPage == nullptr) {
        D2RL::LogError(Context, "MaxLifeOne: no page was free within reach of D2RCore.dll.");
        return false;
    }
    std::memset(HookPage, 0xCC, HookPageBytes);
    for (std::size_t index = 0; index < Hooks.size(); ++index) {
        HookSpec& hook = Hooks[index];
        WriteAbsoluteJump(HookPage + index * RelaySlotBytes,
            reinterpret_cast<std::uintptr_t>(hook.target));
        if (!BuildTrampoline(hook, HookPage + TrampolineOffset + index * TrampolineBytes)) {
            D2RL::LogErrorF(Context,
                "MaxLifeOne: the entry of D2RCore!%s could not be relocated.", hook.exportName);
            return false;
        }
        *hook.original = HookPage + TrampolineOffset + index * TrampolineBytes;
    }
    DWORD previous = 0;
    if (!VirtualProtect(HookPage, HookPageBytes, PAGE_EXECUTE_READ, &previous)) {
        D2RL::LogError(Context, "MaxLifeOne: the hook page could not be made executable.");
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), HookPage, HookPageBytes);

    for (std::size_t index = 0; index < Hooks.size(); ++index) {
        HookSpec& hook = Hooks[index];
        const auto relay = reinterpret_cast<std::intptr_t>(HookPage + index * RelaySlotBytes);
        const auto displacement = static_cast<std::int64_t>(relay)
            - (reinterpret_cast<std::intptr_t>(hook.entry) + 5);
        if (!FitsRel32(displacement)) {
            UninstallHooks();
            D2RL::LogErrorF(Context, "MaxLifeOne: the relay for %s is out of reach.",
                hook.exportName);
            return false;
        }
        hook.written.fill(0x90);
        hook.written[0] = 0xE9;
        const auto rel32 = static_cast<std::int32_t>(displacement);
        std::memcpy(hook.written.data() + 1, &rel32, sizeof(rel32));
        if (!WriteCode(hook.entry, hook.written.data(), hook.patchSize)) {
            UninstallHooks();
            D2RL::LogErrorF(Context, "MaxLifeOne: D2RCore!%s could not be written.",
                hook.exportName);
            return false;
        }
        hook.installed = true;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Tables check
// ---------------------------------------------------------------------------

constexpr std::array<D2RL::DataTables::Bank, 3> BankOrder{
    D2RL::DataTables::Bank::Classic,
    D2RL::DataTables::Bank::Lod,
    D2RL::DataTables::Bank::Rotw,
};

auto BankStateName(BankState state) noexcept -> const char* {
    switch (state) {
    case BankState::NotChecked:      return "not checked";
    case BankState::NoRow:           return "no row";
    case BankState::Ready:           return "ok";
    case BankState::MissingCallback: return "NO fCallback";
    case BankState::Unavailable:     return "unavailable";
    }
    return "?";
}

auto CheckBank(const D2RL::PluginContext* context, D2RL::DataTables::Bank bank) noexcept
        -> BankState {
    D2RL::DataTables::RowView row{};
    row.structSize = D2RL::DataTables::RowViewSize;
    const D2RL::DataTables::Result result = DataTables->getRow(context, bank,
        D2RL::DataTables::TableId::ItemStatCost, RuleStatId, &row);
    if (result == D2RL::DataTables::Result::NotFound
            || result == D2RL::DataTables::Result::InvalidArgument) {
        return BankState::NoRow;
    }
    if (result != D2RL::DataTables::Result::Success
            || !D2RL::DataTables::HasRowViewField(&row, D2RL::DataTables::RowViewRequiredSize)
            || row.row == nullptr || row.rowSize < IscFlagsOffset + sizeof(std::uint32_t)) {
        return BankState::Unavailable;
    }
    const std::uint32_t flags = ReadU32(row.row, IscFlagsOffset);
    if (row.rowSize > IscSaveBitsOffset) {
        const auto* bytes = static_cast<const std::uint8_t*>(row.row);
        RowSync.store(bytes[IscSendBitsOffset]
                | (static_cast<std::uint32_t>(bytes[IscSaveBitsOffset]) << 8)
                | ((flags & IscFlagSaved) != 0 ? 1U << 16 : 0U) | (1U << 24),
            std::memory_order_relaxed);
    }
    return (flags & IscFlagCallback) != 0 ? BankState::Ready : BankState::MissingCallback;
}

void __cdecl OnDataTablesLoaded(const D2RL::PluginContext* context,
        const D2RL::Lifecycle::DataTablesLoadedEvent* event, void*) noexcept {
    if (DataTables == nullptr) return;
    if (D2RL::Lifecycle::HasDataTablesLoadedEventField(event,
            D2RL::Lifecycle::DataTablesLoadedEventRequiredSize)) {
        TablesRevision.store(event->revision, std::memory_order_relaxed);
    }

    RowSync.store(0, std::memory_order_relaxed);
    bool anyReady   = false;
    bool anyMissing = false;
    for (std::size_t index = 0; index < BankOrder.size(); ++index) {
        const BankState state = CheckBank(context, BankOrder[index]);
        Banks[index].store(state, std::memory_order_relaxed);
        anyReady   = anyReady || state == BankState::Ready;
        anyMissing = anyMissing || state == BankState::MissingCallback;
    }

    const bool tablesOk = anyReady && !anyMissing;
    TablesOk.store(tablesOk, std::memory_order_relaxed);
    Armed.store(tablesOk && HooksReady.load(std::memory_order_relaxed),
        std::memory_order_relaxed);

    char banks[160];
    std::snprintf(banks, sizeof(banks), "classic %s, lod %s, rotw %s",
        BankStateName(Banks[0].load(std::memory_order_relaxed)),
        BankStateName(Banks[1].load(std::memory_order_relaxed)),
        BankStateName(Banks[2].load(std::memory_order_relaxed)));

    if (tablesOk) {
        char sync[160];
        DescribeRowSync(sync, sizeof(sync));
        D2RL::LogInfoF(context, "MaxLifeOne: itemstatcost row %u checked (%s). Reaches the "
            "client through: %s.", RuleStatId, banks, sync);
        const std::uint32_t row = RowSync.load(std::memory_order_relaxed);
        if ((row & 0xFF) == 0 && ((row >> 8) & 0xFF) == 0 && ((row >> 16) & 1) == 0) {
            D2RL::LogWarnF(context,
                "MaxLifeOne: row %u reaches no client. Players keep 1 life, but their "
                "character screen and life orb show the real maximum life.", RuleStatId);
        }
    } else if (anyMissing) {
        D2RL::LogErrorF(context,
            "MaxLifeOne: OFF. itemstatcost.txt row %u has fCallback = 0 (%s). Without it "
            "the game never reports the stat appearing or disappearing, so current life "
            "would not drop to 1. Set fCallback to 1 on that row.", RuleStatId, banks);
    } else {
        D2RL::LogErrorF(context,
            "MaxLifeOne: OFF. No itemstatcost.txt data set has a row %u (%s).",
            RuleStatId, banks);
    }
}

void __cdecl OnGameChanged(const D2RL::PluginContext*, const D2RL::Lifecycle::GameplayEvent*,
        void*) noexcept {
    ForgetServerPlayers();
}

auto RegisterTablesCheck() noexcept -> bool {
    if (Context->QueryService(D2RL::ServiceId::DataTable, D2RL::DataTableServiceV1Version,
            &DataTables) != D2RL::ServiceQueryResult::Success
            || !D2RL::HasDataTableServiceV1Field(DataTables,
                D2RL::DataTableServiceV1RequiredSize)) {
        DataTables = nullptr;
        D2RL::LogError(Context, "MaxLifeOne: the data table service is unavailable.");
        return false;
    }
    const D2RL::LifecycleServiceV1* lifecycle = nullptr;
    if (Context->QueryService(D2RL::ServiceId::Lifecycle, D2RL::LifecycleServiceV1Version,
            &lifecycle) != D2RL::ServiceQueryResult::Success
            || !D2RL::HasLifecycleServiceV1Field(lifecycle,
                D2RL::LifecycleServiceV1RequiredSize)) {
        D2RL::LogError(Context, "MaxLifeOne: the lifecycle service is unavailable.");
        return false;
    }
    D2RL::Lifecycle::DataTablesLoadedListener listener{};
    listener.structSize = D2RL::Lifecycle::DataTablesLoadedListenerSize;
    listener.callback   = &OnDataTablesLoaded;
    D2RL::Lifecycle::ListenerHandle handle = D2RL::Lifecycle::InvalidHandle;
    if (lifecycle->registerDataTablesLoadedListener(Context, &listener, &handle)
            != D2RL::Lifecycle::Result::Success
            || handle == D2RL::Lifecycle::InvalidHandle) {
        D2RL::LogError(Context, "MaxLifeOne: the data table listener was refused.");
        return false;
    }
    for (const D2RL::Lifecycle::GameplayEventKind kind :
            {D2RL::Lifecycle::GameplayEventKind::GameJoined,
             D2RL::Lifecycle::GameplayEventKind::GameLeft}) {
        D2RL::Lifecycle::GameplayEventListener game{};
        game.structSize = D2RL::Lifecycle::GameplayEventListenerSize;
        game.kind       = kind;
        game.callback   = &OnGameChanged;
        D2RL::Lifecycle::ListenerHandle gameHandle = D2RL::Lifecycle::InvalidHandle;
        if (lifecycle->registerGameplayEventListener(Context, &game, &gameHandle)
                != D2RL::Lifecycle::Result::Success) {
            D2RL::LogWarn(Context,
                "MaxLifeOne: a game join/leave listener was refused; the client copy report "
                "may repeat across games.");
        }
    }
    return true;
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
    if (key == "enabled") {
        if (!ParseBool(value, Settings.enabled)) {
            D2RL::LogWarn(Context, "MaxLifeOne: enabled is not true or false; keeping true.");
        }
    } else if (key == "stat_id") {
        if (ParseInteger(value, number)) {
            Settings.statId = number;
        } else {
            D2RL::LogWarn(Context, "MaxLifeOne: stat_id is not a number; keeping 512.");
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
        if (line.empty() || line.front() == '[') {
            if (breakAt == std::string_view::npos) break;
            continue;
        }

        const std::size_t equals = line.find('=');
        if (equals != std::string_view::npos) {
            ApplyConfigLine(Trim(line.substr(0, equals)), Trim(line.substr(equals + 1)));
        }
        if (breakAt == std::string_view::npos) break;
    }
}

void ReadConfiguration() noexcept {
    if (!Context->EnsureConfig(DefaultConfigToml)) {
        D2RL::LogWarn(Context, "MaxLifeOne: config file could not be created; using defaults.");
        return;
    }
    std::string buffer(MaximumConfigBytes, '\0');
    std::uint32_t required = 0;
    if (!Context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size()),
            &required)) {
        D2RL::LogWarn(Context, "MaxLifeOne: config file could not be read; using defaults.");
        return;
    }
    buffer.resize(std::strlen(buffer.c_str()));
    ParseConfig(buffer);
}

// ---------------------------------------------------------------------------
// Console command
// ---------------------------------------------------------------------------

auto StateName() noexcept -> const char* {
    if (!Settings.enabled) return "disabled by config";
    if (!HooksReady.load(std::memory_order_relaxed)) return "hooks not installed, see log";
    if (TablesRevision.load(std::memory_order_relaxed) == 0) return "waiting for tables";
    return Armed.load(std::memory_order_relaxed) ? "armed" : "OFF, see log";
}

auto __cdecl StatusCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command,
        void*) noexcept -> D2RL::ConsoleCommandResult {
    if (command == nullptr || command->plugin == nullptr) {
        return D2RL::ConsoleCommandResult::Failed;
    }
    char sync[160];
    DescribeRowSync(sync, sizeof(sync));
    char message[768];
    std::snprintf(message, sizeof(message),
        "Max Life One: %s | stat %u | itemstatcost rev %llu: classic %s, lod %s, rotw %s | "
        "reaches the client through: %s | client copies without the stat %llu | "
        "hooks %zu/%zu | max life reads %llu | stat gained %llu | stat lost %llu | "
        "max life changes held %llu | life writes held %llu",
        StateName(), RuleStatId,
        static_cast<unsigned long long>(TablesRevision.load(std::memory_order_relaxed)),
        BankStateName(Banks[0].load(std::memory_order_relaxed)),
        BankStateName(Banks[1].load(std::memory_order_relaxed)),
        BankStateName(Banks[2].load(std::memory_order_relaxed)),
        sync,
        static_cast<unsigned long long>(ClientCopiesMissing.load(std::memory_order_relaxed)),
        InstalledHookCount(), Hooks.size(),
        static_cast<unsigned long long>(MaxLifeReads.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(Gains.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(Losses.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(HeldChanges.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(ClampedWrites.load(std::memory_order_relaxed)));
    command->plugin->WriteConsoleMessage(message);
    return D2RL::ConsoleCommandResult::Handled;
}

void RegisterStatusCommand() noexcept {
    if (!Context->RegisterConsoleCommand("maxlifeone", &StatusCommand,
            "Reports Max Life One state, the itemstatcost check, hooks and counters.")) {
        D2RL::LogWarn(Context, "MaxLifeOne: the status console command was refused.");
    }
}

constexpr D2RL::PluginInfo Info{
    .infoSize    = D2RL::PluginInfoSize,
    .apiVersion  = D2RL_PLUGIN_API_VERSION,
    .id          = "celestialrayone.max-life-one",
    .name        = "Max Life One",
    .version     = "1.0.2",
    .author      = "CelestialRayOne",
    .description = "Adds a configurable stat that sets a unit's maximum life to 1.",
    .flags       = D2RL::PluginFlags::Shared | D2RL::PluginFlags::NativeHooks,
};

}  // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
    return &Info;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept
        -> bool {
    if (!D2RL::HasContext(context)) return false;
    Context = context;

    ReadConfiguration();
    if (!Settings.enabled) {
        D2RL::LogInfo(Context, "MaxLifeOne: disabled by configuration.");
        RegisterStatusCommand();
        return true;
    }
    if (Settings.statId < 0 || Settings.statId > MaximumStatId
            || Settings.statId == StatHitpoints || Settings.statId == StatMaxHp) {
        D2RL::LogErrorF(Context,
            "MaxLifeOne: stat_id %lld is not usable. It must be 0 to 32767 and not 6 "
            "(hitpoints) or 7 (maxhp).", static_cast<long long>(Settings.statId));
        return false;
    }
    RuleStatId = static_cast<std::uint32_t>(Settings.statId);

    if (!RegisterTablesCheck()) return false;
    if (!InstallHooks()) return false;
    HooksReady.store(true, std::memory_order_relaxed);
    Armed.store(TablesOk.load(std::memory_order_relaxed), std::memory_order_relaxed);

    D2RL::LogInfoF(Context,
        "MaxLifeOne: %zu hooks installed on D2RCore exports for stat %u. The rule arms "
        "after itemstatcost loads and its row is checked.", InstalledHookCount(), RuleStatId);
    RegisterStatusCommand();
    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    Armed.store(false, std::memory_order_relaxed);
    HooksReady.store(false, std::memory_order_relaxed);
    // The page stays allocated: a thread may still be inside a trampoline.
    UninstallHooks();
}

}  // namespace CelestialRayOne::MaxLifeOne
