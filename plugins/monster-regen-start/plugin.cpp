// Monster Regen Start
//
// A monster starts regenerating life as soon as it gains hpregen, whatever
// gave it, instead of only after the next hit.
//
// Everything below was read out of the D2RLoader 1.3.1 process dump (md5
// 8af7a47ce9077de0378c69af2930854d) with IDA, and out of D2RCore.dll 1.3.1
// (SHA-256 2a868d013d2e0830bd2d9e04b918b19e46a73cf726c833e70d089b948fdeb5a2).
// Every game address the plugin uses is guarded by a byte witness at load.
//
// ---------------------------------------------------------------------------
// The bug
// ---------------------------------------------------------------------------
//   A monster regenerates only while its stat-regen timer (event type 3) runs.
//   The timer's handler, the monster regen tick at 0x448C00, reschedules
//   itself every frame, but deletes itself when hpregen is 0 and when life
//   would pass maximum life. The game starts it again in two places only:
//     0x44D2C7  the damage commit: life damage of the hit (+0x134) is not 0,
//               the defender is a monster, it survived, and it already has
//               hpregen -> EVENT_Delete + EVENT_SetEvent(type 3, frame + 1)
//     0x4519F9  the poison applier, for a monster
//   Nothing else starts it. hpregen added by a skill state (aurastat), a
//   curse or an item therefore does nothing until the monster takes life
//   damage again. Under an absorb shield (auraeventfunc 40, Bone Armor,
//   Cyclone Armor) every hit deals 0 life damage, so it never starts, and the
//   hit that breaks the shield removes the state, and its hpregen, before the
//   leftover damage lands.
//
// ---------------------------------------------------------------------------
// The fix
// ---------------------------------------------------------------------------
//   When the total of a stat whose itemstatcost row has fCallback changes,
//   D2RCore calls the unit's stat callback (D2RCore 0x3DBDC7 and 0x3DD054:
//   call [list+2AF0h] after test byte [isc+5],4). Every monster gets the
//   game's 0x46FDD0 as that callback at creation (0x5432E6: lea r8,0x46FDD0,
//   passed to the stat-list allocator 0x2F65D0, which stores it at
//   list+2AF0h). 0x46FDD0 is D2RLoader's stub jmp [0x3E2A3F0], whose pointer
//   leads to D2RCore's OnWideServerStatChanged.
//
//   The plugin swaps that one pointer for its own handler, which calls the
//   original first and then, when hpregen (layer 0) changes to a value other
//   than 0 on a live monster, restarts the timer the same way the damage
//   commit does. No game code is patched. Max Life One hooks the D2RCore
//   function the pointer leads to; the two chain, in either load order.
//
//   Live monster: unit type 1, still registered in the game
//   (SUNIT_GetServerUnit returns the same unit), in a room, and life above 0.
//   Stat lists are freed while a unit is removed from the game; without this
//   check a timer could be scheduled on a unit that is about to be freed.
//
// ---------------------------------------------------------------------------
// ABIs (x64)
// ---------------------------------------------------------------------------
//   stat callback        (game, owner, unit, key = stat << 32 | layer,
//                         old total, new total) -> void
//   EVENT_SetEvent       0x48B720 (game, unit, type, frame, customId,
//                         customParam, arg7) -> void
//   EVENT_Delete         0x48B890 (game, unit, type, customId) -> void
//   SUNIT_GetServerUnit  0x48FE80 (game, type, id) -> unit
//   UNITS_GetRoom        0x34B440 (unit) -> room
//   read unit stat       0x2F5020 (unit, stat, layer) -> int32
//   game frame           dword at game+170h
//   unit type, unit id   dwords at unit+00h, unit+08h
//
//   D2RCore's own symbol table names 0x48B720, 0x48B890, 0x48FE80, 0x34B440
//   and 0x46FDD0 for this build, and calls them with these arguments.

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

namespace CelestialRayOne::MonsterRegenStart {
namespace {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

constexpr std::uint32_t StatHitpoints      = 6;
constexpr std::uint32_t StatHpRegen        = 74;
constexpr std::uint32_t UnitTypeMonster    = 1;
constexpr std::int32_t  EventTypeStatRegen = 3;

constexpr std::size_t UnitTypeOffset  = 0x00;
constexpr std::size_t UnitIdOffset    = 0x08;
constexpr std::size_t GameFrameOffset = 0x170;

// Compiled itemstatcost row: flags dword at +4, fCallback = bit 10.
constexpr std::size_t   IscFlagsOffset  = 4;
constexpr std::uint32_t IscFlagCallback = 0x400;

constexpr std::size_t MaximumConfigBytes = 16'384;

constexpr std::uint64_t StatCallbackThunkRva = 0x46FDD0;
constexpr std::uint64_t StatCallbackSlotRva  = 0x3E2A3F0;  // 0x46FDD6 + 039BA61Ah
constexpr std::uint64_t MonsterCreateSiteRva = 0x5432E3;
constexpr std::uint64_t RegenEventSiteRva    = 0x448C72;
constexpr std::uint64_t RegenLifeSiteRva     = 0x448CC0;
constexpr std::uint64_t RegenOwnerSiteRva    = 0x448E0F;

constexpr std::uint64_t SetEventRva     = 0x48B720;
constexpr std::uint64_t DeleteEventRva  = 0x48B890;
constexpr std::uint64_t ServerUnitRva   = 0x48FE80;
constexpr std::uint64_t UnitRoomRva     = 0x34B440;
constexpr std::uint64_t ReadUnitStatRva = 0x2F5020;

// ---------------------------------------------------------------------------
// Byte witnesses (D2RLoader 1.3.1 dump)
// ---------------------------------------------------------------------------

// 0x46FDD0: jmp [0x3E2A3F0] / nop. The stat callback every monster and player
// gets at creation.
constexpr std::uint8_t StatCallbackThunkBytes[] {
    0xFF, 0x25, 0x1A, 0xA6, 0x9B, 0x03, 0x90,
};

// 0x5432E3, monster creation: mov r9,r14 / lea r8,[0x46FDD0] / xor edx,edx /
// mov rcx,rbx / call 0x2F65D0 (stat-list allocator, callback in r8).
constexpr std::uint8_t MonsterCreateSiteBytes[] {
    0x4D, 0x8B, 0xCE, 0x4C, 0x8D, 0x05, 0xE3, 0xCA, 0xF2, 0xFF, 0x33, 0xD2,
    0x48, 0x8B, 0xCB, 0xE8, 0xD9, 0x32, 0xDB, 0xFF,
};

// 0x448C72, regen tick: mov r9d,[rbp+170h] / inc r9d / three zero stack
// arguments / r8d = 3 / call 0x48B720 (EVENT_SetEvent), then r9d = 0 /
// r8d = 3 / call 0x48B890 (EVENT_Delete).
constexpr std::uint8_t RegenEventWitness[] {
    0x44, 0x8B, 0x8D, 0x70, 0x01, 0x00, 0x00, 0x48, 0x8B, 0xD7, 0x4C, 0x89,
    0xBC, 0x24, 0x80, 0x00, 0x00, 0x00, 0x41, 0xFF, 0xC1, 0x45, 0x33, 0xFF,
    0x48, 0x8B, 0xCD, 0x44, 0x89, 0x7C, 0x24, 0x30, 0x44, 0x89, 0x7C, 0x24,
    0x28, 0x44, 0x89, 0x7C, 0x24, 0x20, 0x45, 0x8D, 0x47, 0x03, 0xE8, 0x7B,
    0x2A, 0x04, 0x00, 0x85, 0xDB, 0x75, 0x17, 0x45, 0x33, 0xC9, 0x45, 0x8D,
    0x47, 0x03, 0x48, 0x8B, 0xD7, 0x48, 0x8B, 0xCD, 0xE8, 0xD5, 0x2B, 0x04,
    0x00,
};  // 73 bytes

// 0x448CC0, regen tick: call 0x2F5020 (unit, 6, 0) = life, call 0x2F4D20 =
// maximum life, call 0x34B440 (unit) = room.
constexpr std::uint8_t RegenLifeWitness[] {
    0x45, 0x33, 0xC0, 0x48, 0x89, 0xB4, 0x24, 0xC0, 0x00, 0x00, 0x00, 0x48,
    0x8B, 0xCF, 0x4C, 0x89, 0xB4, 0x24, 0x88, 0x00, 0x00, 0x00, 0x41, 0x8D,
    0x50, 0x06, 0xE8, 0x41, 0xC3, 0xEA, 0xFF, 0x48, 0x8B, 0xCF, 0x8B, 0xF0,
    0xE8, 0x37, 0xC0, 0xEA, 0xFF, 0x44, 0x8B, 0xF0, 0x85, 0xDB, 0x79, 0x29,
    0x81, 0xFE, 0x00, 0x01, 0x00, 0x00, 0x7D, 0x21, 0x48, 0x8B, 0xCF, 0xE8,
    0x40, 0x27, 0xF0, 0xFF,
};  // 64 bytes

// 0x448E0F, regen tick: the stat list's owner type and id, then
// call 0x48FE80 (game, type, id) = SUNIT_GetServerUnit.
constexpr std::uint8_t RegenOwnerWitness[] {
    0x48, 0x8B, 0xCE, 0xE8, 0x09, 0xE0, 0xEA, 0xFF, 0x48, 0x8B, 0xCE, 0x8B,
    0xD8, 0xE8, 0x0F, 0xE0, 0xEA, 0xFF, 0x44, 0x8B, 0xC3, 0x8B, 0xD0, 0x48,
    0x8B, 0xCD, 0xE8, 0x52, 0x70, 0x04, 0x00,
};  // 31 bytes

// ---------------------------------------------------------------------------
// Native function types
// ---------------------------------------------------------------------------

using StatChangedFn = void(__fastcall*)(void* game, void* owner, void* unit, std::uint64_t key,
    std::int32_t oldValue, std::int32_t newValue) noexcept;
using SetEventFn = void(__fastcall*)(void* game, void* unit, std::int32_t type, std::int32_t frame,
    std::int32_t customId, std::int32_t customParam, std::int32_t arg7) noexcept;
using DeleteEventFn = void(__fastcall*)(void* game, void* unit, std::int32_t type,
    std::int32_t customId) noexcept;
using ServerUnitFn   = void*(__fastcall*)(void* game, std::uint32_t type, std::uint32_t id) noexcept;
using UnitRoomFn     = void*(__fastcall*)(void* unit) noexcept;
using ReadUnitStatFn = std::int32_t(__fastcall*)(void* unit, std::uint32_t stat, std::uint32_t layer) noexcept;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

struct Config {
    bool enabled = true;
};

constexpr char DefaultConfigToml[] =
    "# Monster Regen Start\n"
    "#\n"
    "# Makes a monster start regenerating life as soon as it gains hpregen,\n"
    "# whatever gave it (an aura or skill state, a curse, an item, a monster mod).\n"
    "#\n"
    "# Why it is needed:\n"
    "# - A monster only regenerates while its regeneration timer runs.\n"
    "# - The game stops that timer at full life and whenever hpregen is 0.\n"
    "# - The game starts it again in two places only: a hit that deals life\n"
    "#   damage to a monster that survives and already has hpregen, and a poison\n"
    "#   landing.\n"
    "# - So hpregen that arrives any other way does nothing until the monster is\n"
    "#   hit again. With an absorb shield up, hits deal no life damage, so the\n"
    "#   regeneration never starts at all.\n"
    "#\n"
    "# What the plugin does:\n"
    "# - Whenever a monster's total hpregen changes to a value other than 0, the\n"
    "#   plugin starts its regeneration timer, exactly the way the game does after\n"
    "#   a hit. The game then stops it by itself at full life or at 0 hpregen.\n"
    "# - Negative hpregen (a draining state) starts ticking right away as well,\n"
    "#   instead of waiting for the next hit.\n"
    "# - Hirelings and summons are monsters, so they are covered too.\n"
    "# - Players are not touched: their regeneration never stops.\n"
    "# - Dead monsters, and monsters being removed from the game, are skipped.\n"
    "#\n"
    "# Data requirement, itemstatcost.txt, row hpregen (74):\n"
    "# - fCallback must be 1. Without it the game never reports hpregen changing.\n"
    "#   The plugin checks the row every time the tables load, and stays off\n"
    "#   (with an error in its log) while a data set has the row without it.\n"
    "# - fCallback on hpregen has no other effect: the game's stat-change handler\n"
    "#   has no special case for hpregen.\n"
    "#\n"
    "# Console command: monsterregen (state, table check, counters).\n"
    "\n"
    "[monster_regen_start]\n"
    "\n"
    "# Master switch.\n"
    "enabled = true\n";

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

const D2RL::PluginContext*      Context{};
const D2RL::DataTableService* DataTables{};
Config                          Settings{};
std::uintptr_t                  Base{};

SetEventFn     SetEvent{};
DeleteEventFn  DeleteEvent{};
ServerUnitFn   ServerUnit{};
UnitRoomFn     UnitRoom{};
ReadUnitStatFn ReadUnitStat{};

void**                     Slot{};
std::atomic<StatChangedFn> OriginalStatChanged{};
bool                       OriginalInCore{};

std::atomic<bool>          HooksReady{false};
std::atomic<bool>          TablesOk{false};
std::atomic<bool>          Armed{false};
std::atomic<std::uint64_t> TablesRevision{0};
std::array<std::atomic<BankState>, 3> Banks{};

std::atomic<std::uint64_t> HpRegenChanges{0};
std::atomic<std::uint64_t> TimerStarts{0};
std::atomic<std::uint64_t> SkippedGone{0};
std::atomic<std::uint64_t> SkippedDead{0};

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

auto ReadU32(const void* base, std::size_t offset) noexcept -> std::uint32_t {
    std::uint32_t value = 0;
    std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(value));
    return value;
}

auto IsExecutable(DWORD protect) noexcept -> bool {
    const DWORD base = protect & 0xFF;
    return base == PAGE_EXECUTE || base == PAGE_EXECUTE_READ || base == PAGE_EXECUTE_READWRITE
        || base == PAGE_EXECUTE_WRITECOPY;
}

// ---------------------------------------------------------------------------
// The rule
// ---------------------------------------------------------------------------

// Same calls, same arguments as the damage commit at 0x44D2EF / 0x44D317 and
// the regen tick at 0x448CA0.
void StartRegenTimer(void* game, void* monster) noexcept {
    const std::uint32_t id = ReadU32(monster, UnitIdOffset);
    if (ServerUnit(game, UnitTypeMonster, id) != monster || UnitRoom(monster) == nullptr) {
        SkippedGone.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (ReadUnitStat(monster, StatHitpoints, 0) <= 0) {
        SkippedDead.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const std::uint32_t frame = ReadU32(game, GameFrameOffset);
    DeleteEvent(game, monster, EventTypeStatRegen, 0);
    SetEvent(game, monster, EventTypeStatRegen, static_cast<std::int32_t>(frame + 1U), 0, 0, 0);
    TimerStarts.fetch_add(1, std::memory_order_relaxed);
}

void __fastcall HookStatChanged(void* game, void* owner, void* unit, std::uint64_t key,
        std::int32_t oldValue, std::int32_t newValue) noexcept {
    const StatChangedFn original = OriginalStatChanged.load(std::memory_order_acquire);
    if (original != nullptr) {
        original(game, owner, unit, key, oldValue, newValue);
    }

    // D2RCore's own handler reads the stat as the low word of the key's high
    // dword (0x3CC69A: shr rbp,20h / movzx r12d,bp); the layer is the low dword.
    if (static_cast<std::uint32_t>((key >> 32) & 0xFFFF) != StatHpRegen
            || static_cast<std::uint32_t>(key) != 0) {
        return;
    }
    if (!Armed.load(std::memory_order_relaxed) || newValue == 0 || game == nullptr
            || owner == nullptr) {
        return;
    }
    if (ReadU32(owner, UnitTypeOffset) != UnitTypeMonster) return;

    HpRegenChanges.fetch_add(1, std::memory_order_relaxed);
    StartRegenTimer(game, owner);
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

auto CheckWitnesses() noexcept -> bool {
    struct Witness {
        std::uint64_t       rva;
        const std::uint8_t* bytes;
        std::uint32_t       size;
        const char*         name;
    };
    const Witness witnesses[] {
        { StatCallbackThunkRva, StatCallbackThunkBytes, ByteCount(StatCallbackThunkBytes),
            "stat callback stub" },
        { MonsterCreateSiteRva, MonsterCreateSiteBytes, ByteCount(MonsterCreateSiteBytes),
            "monster stat callback install" },
        { RegenEventSiteRva, RegenEventWitness, ByteCount(RegenEventWitness),
            "regen timer calls" },
        { RegenLifeSiteRva, RegenLifeWitness, ByteCount(RegenLifeWitness),
            "regen life and room calls" },
        { RegenOwnerSiteRva, RegenOwnerWitness, ByteCount(RegenOwnerWitness),
            "regen unit lookup call" },
    };
    bool ok = true;
    for (const Witness& witness : witnesses) {
        if (!Context->CheckExpectedBytes(witness.rva, witness.bytes, witness.size)) {
            D2RL::LogErrorF(Context,
                "MonsterRegenStart: the %s at 0x%llX does not match D2RLoader 1.3.1; wrong game "
                "build or another plugin patched it.", witness.name,
                static_cast<unsigned long long>(witness.rva));
            ok = false;
        }
    }
    return ok;
}

auto WriteSlot(void* expected, void* replacement) noexcept -> bool {
    DWORD previous = 0;
    if (!VirtualProtect(Slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &previous)) return false;
    const void* seen = InterlockedCompareExchangePointer(Slot, replacement, expected);
    DWORD ignored = 0;
    VirtualProtect(Slot, sizeof(void*), previous, &ignored);
    return seen == expected && *reinterpret_cast<void* volatile*>(Slot) == replacement;
}

auto InstallHook() noexcept -> bool {
    SetEvent     = At<SetEventFn>(SetEventRva);
    DeleteEvent  = At<DeleteEventFn>(DeleteEventRva);
    ServerUnit   = At<ServerUnitFn>(ServerUnitRva);
    UnitRoom     = At<UnitRoomFn>(UnitRoomRva);
    ReadUnitStat = At<ReadUnitStatFn>(ReadUnitStatRva);

    Slot = At<void**>(StatCallbackSlotRva);
    void* current = *reinterpret_cast<void* volatile*>(Slot);
    if (current == reinterpret_cast<void*>(&HookStatChanged)) {
        D2RL::LogError(Context, "MonsterRegenStart: the stat callback already points here.");
        return false;
    }
    MEMORY_BASIC_INFORMATION info{};
    if (current == nullptr || VirtualQuery(current, &info, sizeof(info)) == 0
            || info.State != MEM_COMMIT || !IsExecutable(info.Protect)) {
        D2RL::LogErrorF(Context,
            "MonsterRegenStart: the stat callback pointer at 0x%llX holds %p, which is not code. "
            "D2RLoader has not bound it.", static_cast<unsigned long long>(StatCallbackSlotRva),
            current);
        return false;
    }
    const HMODULE core = GetModuleHandleW(L"D2RCore.dll");
    OriginalInCore = core != nullptr && info.AllocationBase == reinterpret_cast<void*>(core);
    OriginalStatChanged.store(reinterpret_cast<StatChangedFn>(current), std::memory_order_release);

    if (!WriteSlot(current, reinterpret_cast<void*>(&HookStatChanged))) {
        OriginalStatChanged.store(nullptr, std::memory_order_release);
        D2RL::LogError(Context, "MonsterRegenStart: the stat callback pointer could not be written.");
        return false;
    }
    D2RL::LogInfoF(Context,
        "MonsterRegenStart: stat callback pointer 0x%llX now leads here, then to %p (%s).",
        static_cast<unsigned long long>(StatCallbackSlotRva), current,
        OriginalInCore ? "D2RCore.dll" : "another module");
    return true;
}

void UninstallHook() noexcept {
    const StatChangedFn original = OriginalStatChanged.load(std::memory_order_acquire);
    if (Slot == nullptr || original == nullptr) return;
    if (!WriteSlot(reinterpret_cast<void*>(&HookStatChanged), reinterpret_cast<void*>(original))) {
        D2RL::LogWarn(Context,
            "MonsterRegenStart: the stat callback pointer was changed by someone else; left as is.");
    }
}

// ---------------------------------------------------------------------------
// Tables check: itemstatcost.txt row hpregen needs fCallback
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

auto CheckBank(const D2RL::PluginContext* context, D2RL::DataTables::Bank bank) noexcept -> BankState {
    D2RL::DataTables::RowView row{};
    row.structSize = D2RL::DataTables::RowViewSize;
    const D2RL::DataTables::Result result = DataTables->getRow(context, bank,
        D2RL::DataTables::TableId::ItemStatCost, StatHpRegen, &row);
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
    return (flags & IscFlagCallback) != 0 ? BankState::Ready : BankState::MissingCallback;
}

void __cdecl OnDataTablesLoaded(const D2RL::PluginContext* context,
        const D2RL::Lifecycle::DataTablesLoadedEvent* event, void*) noexcept {
    if (DataTables == nullptr) return;
    if (D2RL::Lifecycle::HasDataTablesLoadedEventField(event,
            D2RL::Lifecycle::DataTablesLoadedEventRequiredSize)) {
        TablesRevision.store(event->revision, std::memory_order_relaxed);
    }

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
    Armed.store(tablesOk && HooksReady.load(std::memory_order_relaxed), std::memory_order_relaxed);

    char banks[160];
    std::snprintf(banks, sizeof(banks), "classic %s, lod %s, rotw %s",
        BankStateName(Banks[0].load(std::memory_order_relaxed)),
        BankStateName(Banks[1].load(std::memory_order_relaxed)),
        BankStateName(Banks[2].load(std::memory_order_relaxed)));

    if (tablesOk) {
        D2RL::LogInfoF(context, "MonsterRegenStart: itemstatcost.txt row hpregen checked (%s).",
            banks);
    } else if (anyMissing) {
        D2RL::LogErrorF(context,
            "MonsterRegenStart: OFF. itemstatcost.txt row hpregen (74) has fCallback = 0 (%s). "
            "Without it the game never reports hpregen changing. Set fCallback to 1 on that row.",
            banks);
    } else {
        D2RL::LogErrorF(context, "MonsterRegenStart: OFF. No data set has an itemstatcost.txt "
            "row 74 (%s).", banks);
    }
}

auto RegisterTablesCheck() noexcept -> bool {
    if (Context->QueryService(&DataTables) != D2RL::ServiceQueryResult::Success
            || !D2RL::HasDataTableServiceField(DataTables, D2RL::DataTableServiceRequiredSize)) {
        DataTables = nullptr;
        D2RL::LogError(Context, "MonsterRegenStart: the data table service is unavailable.");
        return false;
    }
    const D2RL::LifecycleService* lifecycle = nullptr;
    if (Context->QueryService(&lifecycle) != D2RL::ServiceQueryResult::Success
            || !D2RL::HasLifecycleServiceField(lifecycle, D2RL::LifecycleServiceRequiredSize)) {
        D2RL::LogError(Context, "MonsterRegenStart: the lifecycle service is unavailable.");
        return false;
    }
    D2RL::Lifecycle::DataTablesLoadedListener listener{};
    listener.structSize = D2RL::Lifecycle::DataTablesLoadedListenerSize;
    listener.callback   = &OnDataTablesLoaded;
    D2RL::Lifecycle::ListenerHandle handle = D2RL::Lifecycle::InvalidHandle;
    if (lifecycle->registerDataTablesLoadedListener(Context, &listener, &handle)
            != D2RL::Lifecycle::Result::Success
            || handle == D2RL::Lifecycle::InvalidHandle) {
        D2RL::LogError(Context, "MonsterRegenStart: the data table listener was refused.");
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
                D2RL::LogWarn(Context, "MonsterRegenStart: enabled is not true or false; keeping true.");
            }
        }
    }
}

void ReadConfiguration() noexcept {
    if (!Context->EnsureConfig(DefaultConfigToml)) {
        D2RL::LogWarn(Context, "MonsterRegenStart: config file could not be created; using defaults.");
        return;
    }
    std::string buffer(MaximumConfigBytes, '\0');
    std::uint32_t required = 0;
    if (!Context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size()), &required)) {
        D2RL::LogWarn(Context, "MonsterRegenStart: config file could not be read; using defaults.");
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
    if (!HooksReady.load(std::memory_order_relaxed)) return "not installed, see log";
    if (TablesRevision.load(std::memory_order_relaxed) == 0) return "waiting for tables";
    return Armed.load(std::memory_order_relaxed) ? "armed" : "OFF, see log";
}

auto __cdecl StatusCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command,
        void*) noexcept -> D2RL::ConsoleCommandResult {
    if (command == nullptr || command->plugin == nullptr) return D2RL::ConsoleCommandResult::Failed;
    char message[512];
    std::snprintf(message, sizeof(message),
        "Monster Regen Start: %s | itemstatcost rev %llu hpregen fCallback: classic %s, lod %s, "
        "rotw %s | monster hpregen changes %llu | timers started %llu | skipped, not in game %llu "
        "| skipped, dead %llu",
        StateName(),
        static_cast<unsigned long long>(TablesRevision.load(std::memory_order_relaxed)),
        BankStateName(Banks[0].load(std::memory_order_relaxed)),
        BankStateName(Banks[1].load(std::memory_order_relaxed)),
        BankStateName(Banks[2].load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(HpRegenChanges.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(TimerStarts.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(SkippedGone.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(SkippedDead.load(std::memory_order_relaxed)));
    command->plugin->WriteConsoleMessage(message);
    return D2RL::ConsoleCommandResult::Handled;
}

void RegisterStatusCommand() noexcept {
    if (!Context->RegisterConsoleCommand("monsterregen", &StatusCommand,
            "Reports Monster Regen Start state, the itemstatcost check and counters.")) {
        D2RL::LogWarn(Context, "MonsterRegenStart: the status console command was refused.");
    }
}

constexpr D2RL::PluginInfo Info{
    .infoSize    = D2RL::PluginInfoSize,
    .abiVersion  = D2RL_PLUGIN_ABI_VERSION,
    .id          = "celestialrayone.monster-regen-start",
    .name        = "Monster Regen Start",
    .version     = "1.0.0",
    .author      = "CelestialRayOne",
    .description = "Monsters start regenerating as soon as they gain hpregen, not only after a hit.",
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
        D2RL::LogError(Context, "MonsterRegenStart: the game image base is unknown.");
        return false;
    }

    ReadConfiguration();
    RegisterStatusCommand();
    if (!Settings.enabled) {
        D2RL::LogInfo(Context, "MonsterRegenStart: disabled by configuration.");
        return true;
    }
    if (!CheckWitnesses()) return false;
    if (!RegisterTablesCheck()) return false;
    if (!InstallHook()) return false;

    HooksReady.store(true, std::memory_order_relaxed);
    Armed.store(TablesOk.load(std::memory_order_relaxed), std::memory_order_relaxed);
    D2RL::LogInfo(Context,
        "MonsterRegenStart: installed. It arms after itemstatcost.txt loads and row hpregen is checked.");
    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    Armed.store(false, std::memory_order_relaxed);
    HooksReady.store(false, std::memory_order_relaxed);
    UninstallHook();
}

}  // namespace CelestialRayOne::MonsterRegenStart
