// Summon Resist Cap
//
// Listed monsters resolve their resistances through the same capped branch the
// game already uses for players: base 75, raised by the per-element max-resist
// stat, hard-ceilinged at 95, floored at -100. Physical (damageresist) keeps
// the engine's own 50 cap. The nightmare/hell difficulty penalty is not applied
// to them.
//
// Verified sites, D2RLoader image (3.2.92777 / 3.3.93847 common space):
//   0x4523E0  SUNITDMG_ApplyResistancesAndAbsorb(ctx, resistanceRecord, dontAbsorb)
//   0x452468  difficulty-penalty gate, reads ctx+0x24
//   0x45249C  cap gate: 39 6E 24 (cmp [rsi+24h], ebp) + 75 57 (jnz 0x4524F8)
//   0x4524A1  first instruction of the capping block
//   0x4524F8  first instruction after the capping block
//   0x34B9D0  UNITS_GetUnitType(unit)
//   0x349860  UNITS_GetUnitClassId(unit) -> [unit+4], the monstats row
//   0x34A0E0  UNITS_GetUnitDataContext(unit)
//   0x0976E0  DATATBLS_GetMonStatsTxtRecordForContext(context, classId),
//             returns records + 508 * classId
//
// The resolver reads ctx+0x24 exactly twice and nowhere else. A non-zero value
// skips the difficulty penalty at 0x452468 and then skips the entire capping
// block at 0x45249C. That single flag is why an ally can pass 100 and become
// immune.
//
// This plugin replaces the five bytes of the cap gate ONLY, with a jmp rel32
// into a relay page, and leaves the function entry pristine so plugins that
// hook the entry (monsterdisplay, for one) keep working. Patching the gate
// rather than the entry also means the penalty gate has already run and already
// skipped the penalty by the time the relay is reached, so nothing has to be
// done about the difficulty penalty at all.
//
// Which monsters get capped is an explicit list of monstats.txt row ids in the
// TOML. An earlier version tried to read the Align column at runtime; the row
// lookup could not be verified without a live table, so the list is the honest
// mechanism. The diagnostics below report every monstats row that reaches the
// gate so the list can be filled in from real gameplay rather than guesswork.
//
// D2RLoader 1.3.1: the loader now accepts a rel32 jump only when its target is
// inside D2R.exe, and the relay page is not. The gate therefore jumps to a
// 5-byte "jmp relay" trampoline in the int3 padding after the resolver
// (0x4526B6; the resolver's ret is at 0x4526B5, the next function starts at
// 0x4526C0), and the trampoline jumps on to the relay. A jmp changes no
// register, flag or stack slot, so the relay sees exactly what the gate saw.
// Re-verified against the 1.3.1 image: rsi = ctx (mov rsi,rcx at 0x4523FD)
// and ebp = 0 (xor ebp,ebp at 0x4523F5) with no later write to either before
// the gate; the gate, both continuations (0x4524A1 mov edx,[r14+0Ch],
// 0x4524F8 cmp dword [r14+8],24h, neither reads the flags) and the four game
// functions the callback calls are unchanged.

#include <D2RLPlugin/api.h>

// Windows.h defines min and max as macros unless this is set, which breaks any
// later std::numeric_limits<...>::min() / max() call.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr char DefaultConfig[] = R"toml(# Summon Resist Cap
# File format version. Leave this value unchanged.
config_version = 4
enabled = true

[allies]
# Cap every monster whose monstats.txt Align column is 1.
use_align_column = true
# Extra monstats rows to cap regardless of their Align value. Normally empty.
extra_monster_ids = []

[d2rl]
match = true
)toml";

// ---------------------------------------------------------------- native map

constexpr std::uintptr_t CapGateRva = 0x45249C;
constexpr std::uintptr_t CapBlockRva = 0x4524A1;      // gate falls through here
constexpr std::uintptr_t SkipCapRva = 0x4524F8;       // original jnz target
constexpr std::uintptr_t GetUnitTypeRva = 0x34B9D0;
constexpr std::uintptr_t GetUnitClassIdRva = 0x349860;
constexpr std::uintptr_t GetUnitDataContextRva = 0x34A0E0;
constexpr std::uintptr_t MonStatsGetRecordRva = 0x0976E0;

constexpr std::uint32_t CapGatePatchSize = 5U;

// cmp dword ptr [rsi+24h], ebp   /   jnz 0x4524F8
constexpr std::array<std::uint8_t, 5> CapGateExpected{
    0x39, 0x6E, 0x24, 0x75, 0x57,
};
// In-image trampoline (D2RLoader 1.3.1), 10 x int3 after the resolver.
constexpr std::uintptr_t TrampolineRva = 0x4526B6;
constexpr std::array<std::uint8_t, 10> TrampolineRunExpected{
    0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
};
constexpr std::array<std::uint8_t, 5> TrampolineSlotExpected{
    0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
};
constexpr std::array<std::uint8_t, 18> GetUnitTypeExpected{
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x85, 0xC9, 0x75, 0x1D,
    0x88, 0x4C, 0x24, 0x30, 0x48, 0x8D, 0x4C, 0x24, 0x30,
};

// Layout of the resolver's first argument, read straight off 0x4523E0.
constexpr std::size_t DefenderOffset = 0x18;      // ctx[3], stats are read from it
constexpr std::size_t UncappedFlagOffset = 0x24;  // gates penalty + cap

constexpr std::size_t MonStatsRecordSize = 508U;  // from the getter itself
constexpr std::size_t MonStatsAiOffset = 0x52;    // signed word, <= 0x9A
// monstats.txt Align. Located in the column parser at sub_14039DB30, where the
// "Align" descriptor sits at rbp+0x340 and its record-offset store is
// mov qword [rbp+0x350], 0x87, and CONFIRMED in game: an allied row reads 1
// here while ordinary monsters read 0. 0 = enemy, 1 = ally, 2 = neutral.
constexpr std::size_t MonStatsAlignOffset = 0x87;
constexpr std::uint8_t AlignAlly = 1U;

constexpr std::int32_t UnitTypeMonster = 1;
constexpr std::size_t MaxAllyIds = 256U;
constexpr std::size_t MaxObservedRows = 24U;

constexpr std::size_t RelaySize = 81U;
constexpr std::size_t RelayCallbackSlot = 29U;   // imm64 of mov rax, callback
constexpr std::size_t RelayCapJumpSlot = 60U;    // rel32 of jmp CapBlockRva
constexpr std::size_t RelayCapJumpEnd = 64U;
constexpr std::size_t RelaySkipJumpSlot = 77U;   // rel32 of jmp SkipCapRva
constexpr std::size_t RelaySkipJumpEnd = 81U;

constexpr std::uint32_t ConfigReadBufferBytes = 16384U;

using GetUnitTypeFn = std::int32_t(__fastcall*)(void*) noexcept;
using GetUnitClassIdFn = std::int32_t(__fastcall*)(void*) noexcept;
using GetUnitDataContextFn = std::uint8_t(__fastcall*)(void*) noexcept;
using MonStatsGetRecordFn =
    const std::uint8_t*(__fastcall*)(std::uint8_t, std::int32_t) noexcept;

struct Config {
    bool enabled{true};
    bool useAlignColumn{true};
    std::int32_t allyIds[MaxAllyIds]{};
    std::size_t allyIdCount{};
};

struct ObservedRow {
    std::int32_t classId;
    std::int32_t ai;
    std::uint32_t alignByte;
    std::uint64_t hits;
};

const D2RL::PluginContext* Context{};
std::uint8_t* Base{};
Config Settings{};
std::string LoadedConfigPath{"embedded defaults"};
std::string RuntimeBuild{"<unavailable>"};
void* RelayPage{};

GetUnitTypeFn GetUnitType{};
GetUnitClassIdFn GetUnitClassId{};
GetUnitDataContextFn GetUnitDataContext{};
MonStatsGetRecordFn MonStatsGetRecord{};

std::atomic_bool Operational{};
std::atomic<std::uint64_t> GateCalls{};
std::atomic<std::uint64_t> PlayerGates{};
std::atomic<std::uint64_t> AllyCaps{};
std::atomic<std::uint64_t> ListCaps{};
std::atomic<std::uint64_t> MonsterSkips{};

ObservedRow Observed[MaxObservedRows]{};
std::atomic<std::size_t> ObservedCount{};

constexpr D2RL::PluginInfo Info{
    .infoSize = D2RL::PluginInfoSize,
    .apiVersion = D2RL_PLUGIN_API_VERSION,
    .id = "celestialrayone.summon-resist-cap",
    .name = "Summon Resist Cap",
    .version = "3.1.2",
    .author = "CelestialRayOne",
    .description =
        "Caps listed monsters' resistances the way player resistances are capped.",
    .flags = D2RL::PluginFlags::Server | D2RL::PluginFlags::NativeHooks,
};

template <typename T>
auto At(std::uintptr_t rva) noexcept -> T {
    return reinterpret_cast<T>(Base + rva);
}

auto VerifyBytes(
        std::uintptr_t rva,
        const std::uint8_t* expected,
        std::size_t size,
        const char* what) noexcept -> bool {
    if (Context->CheckExpectedBytes(
            rva, expected, static_cast<std::uint32_t>(size))) {
        return true;
    }
    char message[192]{};
    std::snprintf(
        message,
        sizeof(message),
        "SummonResistCap: %s at 0x%llX does not match the expected bytes; "
        "plugin refused.",
        what,
        static_cast<unsigned long long>(rva));
    Context->LogError(message);
    return false;
}

// ------------------------------------------------------------------ config

auto Trim(std::string value) noexcept -> std::string {
    const auto isSpace = [](char c) {
        return std::isspace(static_cast<unsigned char>(c)) != 0;
    };
    while (!value.empty() && isSpace(value.front())) value.erase(value.begin());
    while (!value.empty() && isSpace(value.back())) value.pop_back();
    return value;
}

auto ParseBool(const std::string& value, bool fallback) noexcept -> bool {
    if (value == "true" || value == "1") return true;
    if (value == "false" || value == "0") return false;
    return fallback;
}

// Accepts "[357, 358]", "357,358" or "357 358". Anything that is not a digit
// separates entries, so brackets and commas need no special handling.
void ParseIdList(const std::string& value) noexcept {
    Settings.allyIdCount = 0U;
    std::int32_t current = 0;
    bool have = false;
    for (const char c : value) {
        if (c >= '0' && c <= '9') {
            current = current * 10 + (c - '0');
            have = true;
            continue;
        }
        if (have && Settings.allyIdCount < MaxAllyIds) {
            Settings.allyIds[Settings.allyIdCount++] = current;
        }
        current = 0;
        have = false;
    }
    if (have && Settings.allyIdCount < MaxAllyIds) {
        Settings.allyIds[Settings.allyIdCount++] = current;
    }
}

void ApplyConfigLine(
        const std::string& section,
        const std::string& key,
        const std::string& value) noexcept {
    if (section.empty()) {
        if (key == "enabled") {
            Settings.enabled = ParseBool(value, Settings.enabled);
        }
    } else if (section == "allies") {
        if (key == "use_align_column") {
            Settings.useAlignColumn = ParseBool(value, Settings.useAlignColumn);
        } else if (key == "extra_monster_ids" || key == "monster_ids") {
            ParseIdList(value);
        }
    }
}

void ParseConfig(const std::string& text) noexcept {
    std::string section;
    std::size_t begin = 0U;
    while (begin <= text.size()) {
        std::size_t end = text.find('\n', begin);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(begin, end - begin);
        begin = end + 1U;

        const auto comment = line.find('#');
        if (comment != std::string::npos) line.erase(comment);
        line = Trim(line);
        if (line.empty()) continue;

        if (line.front() == '[' && line.back() == ']'
                && line.find('=') == std::string::npos) {
            section = Trim(line.substr(1U, line.size() - 2U));
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        ApplyConfigLine(
            section,
            Trim(line.substr(0U, equals)),
            Trim(line.substr(equals + 1U)));
    }
}

auto NarrowPath(const wchar_t* wide) noexcept -> std::string {
    if (wide == nullptr || wide[0] == L'\0') return {};
    const int needed =
        ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};
    std::string narrow(static_cast<std::size_t>(needed - 1), '\0');
    ::WideCharToMultiByte(
        CP_UTF8, 0, wide, -1, narrow.data(), needed, nullptr, nullptr);
    return narrow;
}

auto ConfigPathForDisplay() noexcept -> std::string {
    constexpr std::size_t needed =
        offsetof(D2RL::PluginContext, pluginConfigPath) + sizeof(const wchar_t*);
    if (Context->contextSize < needed) return {};
    return NarrowPath(Context->pluginConfigPath);
}

auto LoadConfig() noexcept -> bool {
    Settings = Config{};
    LoadedConfigPath = "embedded defaults";

    if (!Context->EnsureConfig(DefaultConfig)) {
        Context->LogWarn(
            "SummonResistCap: the TOML could not be created; embedded defaults "
            "are in use.");
        return true;
    }

    std::string text(ConfigReadBufferBytes, '\0');
    std::uint32_t required = 0U;
    if (!Context->ReadConfig(
            text.data(), static_cast<std::uint32_t>(text.size()), &required)) {
        if (required > text.size()) {
            text.assign(required, '\0');
            if (!Context->ReadConfig(text.data(), required, &required)) {
                Context->LogWarn(
                    "SummonResistCap: the TOML could not be read; embedded "
                    "defaults are in use.");
                return true;
            }
        } else {
            Context->LogWarn(
                "SummonResistCap: the TOML could not be read; embedded "
                "defaults are in use.");
            return true;
        }
    }

    text.resize(std::strlen(text.c_str()));
    ParseConfig(text);

    const std::string path = ConfigPathForDisplay();
    LoadedConfigPath = path.empty() ? std::string{"<plugin config>"} : path;
    return true;
}

// ---------------------------------------------------------------- diagnostics

auto IsPlausiblePointer(const void* pointer) noexcept -> bool {
    const auto value = reinterpret_cast<std::uintptr_t>(pointer);
    return value >= 0x10000ULL && (value & 7U) == 0U;
}

// monstats records are 508 bytes apart, which is not a multiple of 8, so every
// odd row sits on a 4-byte boundary. Checking them for 8-byte alignment throws
// away half the table.
auto IsPlausibleRecord(const void* pointer) noexcept -> bool {
    const auto value = reinterpret_cast<std::uintptr_t>(pointer);
    return value >= 0x10000ULL && (value & 3U) == 0U;
}

auto MonStatsRecordFor(void* unit, std::int32_t classId) noexcept
        -> const std::uint8_t* {
    if (MonStatsGetRecord == nullptr || GetUnitDataContext == nullptr) {
        return nullptr;
    }
    const std::uint8_t* record =
        MonStatsGetRecord(GetUnitDataContext(unit), classId);
    if (!IsPlausibleRecord(record)) return nullptr;
    // The AI column proves this is a monstats record and not something else.
    const std::uint16_t ai =
        *reinterpret_cast<const std::uint16_t*>(record + MonStatsAiOffset);
    return ai <= 0x9AU ? record : nullptr;
}

// Records every monstats row that reaches the gate, with the row's AI id and
// the byte the Align column was believed to live at. Purely informational: it
// is how the monster_ids list gets filled in, and it is also how the Align
// offset can be confirmed later from real units.
void NoteRow(void* unit, std::int32_t classId) noexcept {
    const std::size_t count = ObservedCount.load(std::memory_order_acquire);
    for (std::size_t i = 0U; i < count && i < MaxObservedRows; ++i) {
        if (Observed[i].classId == classId) {
            ++Observed[i].hits;
            return;
        }
    }
    if (count >= MaxObservedRows) return;

    ObservedRow row{classId, -1, 0xFFFFFFFFU, 1U};
    const std::uint8_t* record = MonStatsRecordFor(unit, classId);
    if (record != nullptr) {
        row.ai =
            *reinterpret_cast<const std::int16_t*>(record + MonStatsAiOffset);
        row.alignByte = record[MonStatsAlignOffset];
    }
    Observed[count] = row;
    ObservedCount.store(count + 1U, std::memory_order_release);
}

// ------------------------------------------------------------ gate callback

auto IsListedAlly(std::int32_t classId) noexcept -> bool {
    for (std::size_t i = 0U; i < Settings.allyIdCount; ++i) {
        if (Settings.allyIds[i] == classId) return true;
    }
    return false;
}

// Returns non-zero to run the engine's capping block, zero to skip it exactly
// as the original jnz did. Called from the relay with RCX = ctx.
auto EvaluateCapGate(void* ctx) noexcept -> std::int32_t {
    if (ctx == nullptr) return 0;

    auto* raw = reinterpret_cast<std::uint8_t*>(ctx);
    const std::uint32_t uncapped =
        *reinterpret_cast<const std::uint32_t*>(raw + UncappedFlagOffset);

    GateCalls.fetch_add(1U, std::memory_order_relaxed);

    // Vanilla capped path: a player. Nothing to decide.
    if (uncapped == 0U) {
        PlayerGates.fetch_add(1U, std::memory_order_relaxed);
        return 1;
    }
    if (!Operational.load(std::memory_order_acquire)
            || GetUnitType == nullptr || GetUnitClassId == nullptr) {
        return 0;
    }

    void* defender = *reinterpret_cast<void**>(raw + DefenderOffset);
    if (!IsPlausiblePointer(defender)) return 0;
    if (GetUnitType(defender) != UnitTypeMonster) return 0;

    const std::int32_t classId = GetUnitClassId(defender);
    NoteRow(defender, classId);

    if (Settings.useAlignColumn) {
        const std::uint8_t* record = MonStatsRecordFor(defender, classId);
        if (record != nullptr && record[MonStatsAlignOffset] == AlignAlly) {
            AllyCaps.fetch_add(1U, std::memory_order_relaxed);
            return 1;
        }
    }
    if (IsListedAlly(classId)) {
        ListCaps.fetch_add(1U, std::memory_order_relaxed);
        return 1;
    }
    MonsterSkips.fetch_add(1U, std::memory_order_relaxed);
    return 0;
}

// ------------------------------------------------------------------- relay

auto CanEncodeRel32(std::uintptr_t from, std::uintptr_t to) noexcept -> bool {
    const auto delta =
        static_cast<std::int64_t>(to) - static_cast<std::int64_t>(from);
    return delta >= static_cast<std::int64_t>(INT32_MIN)
        && delta <= static_cast<std::int64_t>(INT32_MAX);
}

auto AllocateNear(std::uintptr_t hint, std::size_t size) noexcept -> void* {
    SYSTEM_INFO systemInfo{};
    ::GetSystemInfo(&systemInfo);
    const auto granularity =
        static_cast<std::uintptr_t>(systemInfo.dwAllocationGranularity);
    if (granularity == 0U) return nullptr;

    const auto aligned = hint & ~(granularity - 1U);
    for (std::uintptr_t delta = granularity;
            delta < 0x70000000ULL; delta += granularity) {
        if (aligned > UINTPTR_MAX - delta) break;
        const auto candidate = aligned + delta;
        if (!CanEncodeRel32(hint, candidate)) break;
        if (auto* memory = ::VirtualAlloc(
                reinterpret_cast<void*>(candidate),
                size,
                MEM_COMMIT | MEM_RESERVE,
                PAGE_READWRITE)) {
            return memory;
        }
    }
    return nullptr;
}

void WriteRel32(
        std::uint8_t* relay,
        std::size_t slot,
        std::size_t nextInstruction,
        std::uintptr_t target) noexcept {
    const auto from =
        reinterpret_cast<std::uintptr_t>(relay) + nextInstruction;
    const auto displacement =
        static_cast<std::int32_t>(static_cast<std::int64_t>(target)
            - static_cast<std::int64_t>(from));
    std::memcpy(relay + slot, &displacement, sizeof(displacement));
}

auto InstallRelay() noexcept -> bool {
    const auto gateAddress = reinterpret_cast<std::uintptr_t>(Base) + CapGateRva;
    RelayPage = AllocateNear(gateAddress, RelaySize);
    if (RelayPage == nullptr) {
        Context->LogError(
            "SummonResistCap: no relay page was available within rel32 reach.");
        return false;
    }

    // The stack pointer is held in RBP across the callback. RBP is callee-saved
    // in the x64 calling convention, so EvaluateCapGate and every game function
    // it calls must hand it back unchanged. Up to 3.1.0 the relay held it in
    // R11, which is volatile: any callee may overwrite R11, and whenever one did,
    // the relay loaded that garbage into RSP and the next pop faulted with an
    // unusable stack, which Windows answers by ending the process with no crash
    // report. RBP itself is pushed first and popped before the branch, so the
    // host function gets its own RBP back on both exits.
    static constexpr std::uint8_t Template[RelaySize] = {
        0x9C,                                // pushfq
        0x50, 0x51, 0x52,                    // push rax, rcx, rdx
        0x41, 0x50, 0x41, 0x51,              // push r8, r9
        0x41, 0x52, 0x41, 0x53,              // push r10, r11
        0x55,                                // push rbp
        0x48, 0x89, 0xE5,                    // mov rbp, rsp
        0x48, 0x83, 0xE4, 0xF0,              // and rsp, -16
        0x48, 0x83, 0xEC, 0x20,              // sub rsp, 32
        0x48, 0x8B, 0xCE,                    // mov rcx, rsi
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,  // mov rax, EvaluateCapGate
        0xFF, 0xD0,                          // call rax
        0x48, 0x89, 0xEC,                    // mov rsp, rbp
        0x5D,                                // pop rbp
        0x85, 0xC0,                          // test eax, eax
        0x74, 0x11,                          // jz  +17 -> skip epilogue
        0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59,  // pop r11, r10, r9
        0x41, 0x58, 0x5A, 0x59, 0x58, 0x9D,  // pop r8, rdx, rcx, rax, popfq
        0xE9, 0, 0, 0, 0,                    // jmp CapBlockRva
        0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59,  // pop r11, r10, r9
        0x41, 0x58, 0x5A, 0x59, 0x58, 0x9D,  // pop r8, rdx, rcx, rax, popfq
        0xE9, 0, 0, 0, 0,                    // jmp SkipCapRva
    };
    // Layout checks, so the slot constants can never drift from the bytes.
    static_assert(Template[RelayCallbackSlot - 2U] == 0x48
        && Template[RelayCallbackSlot - 1U] == 0xB8, "callback slot");
    static_assert(Template[RelayCapJumpSlot - 1U] == 0xE9
        && RelayCapJumpEnd == RelayCapJumpSlot + 4U, "cap jump slot");
    static_assert(Template[RelaySkipJumpSlot - 1U] == 0xE9
        && RelaySkipJumpEnd == RelaySkipJumpSlot + 4U
        && RelaySkipJumpEnd == RelaySize, "skip jump slot");
    static_assert(Template[RelayCapJumpSlot - 15U] == 0x74
        && Template[RelayCapJumpSlot - 14U] == RelayCapJumpEnd - (RelayCapJumpSlot - 13U),
        "jz lands on the skip epilogue");

    auto* relay = static_cast<std::uint8_t*>(RelayPage);
    std::memcpy(relay, Template, RelaySize);

    const auto callback = reinterpret_cast<std::uintptr_t>(&EvaluateCapGate);
    std::memcpy(relay + RelayCallbackSlot, &callback, sizeof(callback));
    WriteRel32(
        relay,
        RelayCapJumpSlot,
        RelayCapJumpEnd,
        reinterpret_cast<std::uintptr_t>(Base) + CapBlockRva);
    WriteRel32(
        relay,
        RelaySkipJumpSlot,
        RelaySkipJumpEnd,
        reinterpret_cast<std::uintptr_t>(Base) + SkipCapRva);

    DWORD previousProtection{};
    if (!::VirtualProtect(
            relay, RelaySize, PAGE_EXECUTE_READ, &previousProtection)) {
        Context->LogError(
            "SummonResistCap: relay page protection could not be finalized.");
        return false;
    }
    ::FlushInstructionCache(::GetCurrentProcess(), relay, RelaySize);

    const auto relayAddress = reinterpret_cast<std::uintptr_t>(relay);
    const auto baseAddress = reinterpret_cast<std::uintptr_t>(Base);
    const auto trampolineAddress = baseAddress + TrampolineRva;
    const auto trampolineDisplacement = static_cast<std::int64_t>(relayAddress)
        - static_cast<std::int64_t>(trampolineAddress + CapGatePatchSize);
    if (relayAddress < baseAddress || !CanEncodeRel32(gateAddress, relayAddress)
            || trampolineDisplacement < static_cast<std::int64_t>(INT32_MIN)
            || trampolineDisplacement > static_cast<std::int64_t>(INT32_MAX)) {
        Context->LogError(
            "SummonResistCap: relay displacement validation failed.");
        return false;
    }

    // "jmp relay" into the padding first. Nothing reaches it until the gate
    // below is aimed at it.
    std::array<std::uint8_t, CapGatePatchSize> trampoline{0xE9};
    const auto trampolineRel32 = static_cast<std::int32_t>(trampolineDisplacement);
    std::memcpy(trampoline.data() + 1, &trampolineRel32, sizeof(trampolineRel32));
    if (!Context->PatchBytes(
            TrampolineRva,
            TrampolineSlotExpected.data(),
            CapGatePatchSize,
            trampoline.data(),
            CapGatePatchSize)) {
        Context->LogError(
            "SummonResistCap: the trampoline at 0x4526B6 could not be written; "
            "plugin refused.");
        return false;
    }

    if (!Context->PatchJmpRel32(
            CapGateRva,
            CapGateExpected.data(),
            CapGatePatchSize,
            TrampolineRva,
            CapGatePatchSize)) {
        Context->LogError(
            "SummonResistCap: the cap gate at 0x45249C is already owned or does "
            "not match; plugin refused.");
        return false;
    }
    return true;
}

// ----------------------------------------------------------------- console

void DescribeGatePatch(char* out, std::size_t size) noexcept {
    const auto* gate = reinterpret_cast<const std::uint8_t*>(Base + CapGateRva);
    const char* state = "FOREIGN";
    if (gate[0] == 0x39 && gate[1] == 0x6E && gate[2] == 0x24
            && gate[3] == 0x75 && gate[4] == 0x57) {
        state = "VANILLA (our patch is NOT installed)";
    } else if (gate[0] == 0xE9) {
        std::int32_t displacement{};
        std::memcpy(&displacement, gate + 1, sizeof(displacement));
        const auto target =
            reinterpret_cast<std::uintptr_t>(gate + 5) + displacement;
        const auto* trampoline = Base + TrampolineRva;
        state = "FOREIGN (jmp elsewhere)";
        if (target == reinterpret_cast<std::uintptr_t>(trampoline)
                && trampoline[0] == 0xE9) {
            std::int32_t hop{};
            std::memcpy(&hop, trampoline + 1, sizeof(hop));
            const auto relay =
                reinterpret_cast<std::uintptr_t>(trampoline + 5) + hop;
            if (relay == reinterpret_cast<std::uintptr_t>(RelayPage)) state = "LIVE";
        }
    }
    std::snprintf(
        out,
        size,
        "%02X %02X %02X %02X %02X = %s",
        gate[0], gate[1], gate[2], gate[3], gate[4], state);
}

auto Status(
        D2R::Game::Client*,
        const D2RL::ConsoleCommandContext* command,
        void*) noexcept -> D2RL::ConsoleCommandResult {
    if (command == nullptr || command->plugin == nullptr) {
        return D2RL::ConsoleCommandResult::Failed;
    }
    char gate[128]{};
    DescribeGatePatch(gate, sizeof(gate));
    char message[1024]{};
    std::size_t used = static_cast<std::size_t>(std::snprintf(
        message,
        sizeof(message),
        "Summon Resist Cap 3.1.2: active=%s; build=%s; gate 0x45249C: %s; "
        "gate calls=%llu; players=%llu; capped=%llu by Align / %llu by list; "
        "skipped=%llu; Align column=%s; extra rows=%zu:",
        Operational.load(std::memory_order_acquire) ? "true" : "false",
        RuntimeBuild.c_str(),
        gate,
        static_cast<unsigned long long>(GateCalls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(PlayerGates.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(AllyCaps.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(ListCaps.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(MonsterSkips.load(std::memory_order_relaxed)),
        Settings.useAlignColumn ? "on" : "off",
        Settings.allyIdCount));
    if (Settings.allyIdCount == 0U && used < sizeof(message)) {
        used += static_cast<std::size_t>(std::snprintf(
            message + used, sizeof(message) - used, " none"));
    }
    for (std::size_t i = 0U;
            i < Settings.allyIdCount && i < 24U && used < sizeof(message); ++i) {
        used += static_cast<std::size_t>(std::snprintf(
            message + used, sizeof(message) - used, " %d", Settings.allyIds[i]));
    }
    std::snprintf(
        message + used, sizeof(message) - used, "; config=%s.",
        LoadedConfigPath.c_str());
    command->plugin->WriteConsoleMessage(message);
    return D2RL::ConsoleCommandResult::Handled;
}

auto Probe(
        D2R::Game::Client*,
        const D2RL::ConsoleCommandContext* command,
        void*) noexcept -> D2RL::ConsoleCommandResult {
    if (command == nullptr || command->plugin == nullptr) {
        return D2RL::ConsoleCommandResult::Failed;
    }
    const std::size_t count = ObservedCount.load(std::memory_order_acquire);
    char message[1400]{};
    std::size_t used = static_cast<std::size_t>(std::snprintf(
        message,
        sizeof(message),
        "monstats rows seen at the resistance resolver: %zu", count));
    if (count == 0U) {
        std::snprintf(
            message + used,
            sizeof(message) - used,
            ". Nothing has resolved damage yet, so no rows are known.");
        command->plugin->WriteConsoleMessage(message);
        return D2RL::ConsoleCommandResult::Handled;
    }
    for (std::size_t i = 0U;
            i < count && i < MaxObservedRows && used < sizeof(message); ++i) {
        used += static_cast<std::size_t>(std::snprintf(
            message + used,
            sizeof(message) - used,
            " | row %d ai=%d byte0x87=%u hits=%llu%s",
            Observed[i].classId,
            Observed[i].ai,
            Observed[i].alignByte,
            static_cast<unsigned long long>(Observed[i].hits),
            (Observed[i].alignByte == AlignAlly && Settings.useAlignColumn)
                || IsListedAlly(Observed[i].classId) ? " CAPPED" : ""));
    }
    command->plugin->WriteConsoleMessage(message);
    return D2RL::ConsoleCommandResult::Handled;
}

}  // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept
        -> const D2RL::PluginInfo* {
    return &Info;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(
        const D2RL::PluginContext* context) noexcept -> bool {
    if (!D2RL::HasContext(context)
            || context->apiVersion != D2RL_PLUGIN_API_VERSION) {
        return false;
    }
    Context = context;
    Base = reinterpret_cast<std::uint8_t*>(context->exeBase);
    if (Base == nullptr || !LoadConfig()) return false;

    const auto* runtimeBuild = D2RL::GetBuildName(context);
    if (runtimeBuild != nullptr && runtimeBuild[0] != '\0') {
        RuntimeBuild = runtimeBuild;
    }

    if (!context->RegisterConsoleCommand(
            "summon-resist-cap",
            Status,
            "Show Summon Resist Cap settings and diagnostic counters.")) {
        context->LogWarn(
            "SummonResistCap: optional status command was not registered.");
    }
    if (!context->RegisterConsoleCommand(
            "summon-resist-probe",
            Probe,
            "List the monstats rows that reached the resistance resolver.")) {
        context->LogWarn(
            "SummonResistCap: optional probe command was not registered.");
    }

    if (!Settings.enabled) {
        context->LogInfo(
            "SummonResistCap: loaded disabled; no patch was installed.");
        return true;
    }

    if (!VerifyBytes(
            GetUnitTypeRva,
            GetUnitTypeExpected.data(),
            GetUnitTypeExpected.size(),
            "UNITS_GetUnitType")
        || !VerifyBytes(
            CapGateRva,
            CapGateExpected.data(),
            CapGateExpected.size(),
            "the resistance cap gate")
        || !VerifyBytes(
            TrampolineRva,
            TrampolineRunExpected.data(),
            TrampolineRunExpected.size(),
            "the int3 padding after the resolver")) {
        return false;
    }
    GetUnitType = At<GetUnitTypeFn>(GetUnitTypeRva);
    GetUnitClassId = At<GetUnitClassIdFn>(GetUnitClassIdRva);
    GetUnitDataContext = At<GetUnitDataContextFn>(GetUnitDataContextRva);
    MonStatsGetRecord = At<MonStatsGetRecordFn>(MonStatsGetRecordRva);

    // Armed before the patch lands: the relay can be entered the instant the
    // bytes change, and an unarmed callback would report vanilla behaviour.
    Operational.store(true, std::memory_order_release);
    if (!InstallRelay()) {
        Operational.store(false, std::memory_order_release);
        return false;
    }

    char message[512]{};
    std::snprintf(
        message,
        sizeof(message),
        "Summon Resist Cap 3.1.2 active for observed D2R %s; Align column=%s, "
        "%zu extra rows; cap gate 0x45249C relayed; function entry 0x4523E0 "
        "left untouched; config=%s.",
        RuntimeBuild.c_str(),
        Settings.useAlignColumn ? "on" : "off",
        Settings.allyIdCount,
        LoadedConfigPath.c_str());
    context->LogInfo(message);
    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    // The relay stays mapped and the gate keeps calling it, so the callback
    // must remain valid. Disarmed, it reproduces vanilla behaviour exactly.
    Operational.store(false, std::memory_order_release);
}
