#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <D2RLPlugin/api.h>

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Native contract, D2RLoader 1.3.1 (D2RCore.dll 1.3.1-beta, SHA-256 2a868d01...).
//
// Aura Enchanted (monumod.txt id 30) no longer runs the game's own routine
// 0x495CD0: its spawn table slot (0x2395FE0 + 30 * 8) points at
// D2RCore!ApplyAuraEnchanted. That export picks a path from the d2rloader.toml
// setting d2rcore.game_rules.aura_enchanted_selection, a byte in D2RCore .data:
//
//   3.1 (byte 0)  calls the game's 0x495CD0, which reads the herald table in
//                 D2R.exe at 0x1D1DF20. This plugin does not touch that table.
//   3.2 (byte 1, the default)  D2RCore's own code. For a monster whose
//                 MonsterData+0x1A flags hold 0x200 (herald) it takes a value
//                 modulo 3 and picks one of three records compiled into D2RCore
//                 .rdata: 0 Concentration 113, 1 Vigor 115, 2 HeraldThorns 428.
//                 The skill (word at record + 0x14) is applied as item_aura
//                 (stat 151) at level (mlvl + [+08h]) * [+0Ch] / [+10h],
//                 clamped to 1..99.
//
// This plugin rewrites the skill id of the HeraldThorns record. The record is
// located through the code that reads it, not by a fixed address: the herald
// branch of ApplyAuraEnchanted is byte-checked (its three record displacements
// excepted), the record address is decoded from its lea, and all six record
// fields are checked before the write. No other D2RCore code references these
// records.
// ---------------------------------------------------------------------------

constexpr wchar_t CoreModuleName[] = L"D2RCore.dll";
constexpr char    CoreExportName[] = "ApplyAuraEnchanted";

// ApplyAuraEnchanted+0x641, the herald branch of the 3.2 path:
//   +641 mov rax,[r14+10h] / test byte ptr [rax+1Bh],2 / je end   herald flag 0x200
//   +64F movzx eax,di / imul eax,eax,6AC690C5h / add eax,29Ah / value % 3
//   +671 cmp edx,1
//   +674 lea rdx,[Vigor record]            displacement at window +0x36
//   +67B lea r8,[HeraldThorns record]      displacement at window +0x3D
//   +682 cmove r8,rdx / cmp eax,ecx
//   +688 lea rcx,[Concentration record]    displacement at window +0x4A
//   +68F cmovne rcx,r8
//   +693 add esi,[rcx+8] / movzx ebx,word ptr [rcx+14h] / imul esi,[rcx+0Ch]
constexpr std::size_t HeraldBranchOffset = 0x641;
constexpr std::array<std::uint8_t, 93> HeraldBranch{
    0x49, 0x8B, 0x46, 0x10, 0xF6, 0x40, 0x1B, 0x02,
    0x0F, 0x84, 0xEB, 0xFE, 0xFF, 0xFF, 0x0F, 0xB7,
    0xC7, 0x69, 0xC0, 0xC5, 0x90, 0xC6, 0x6A, 0x05,
    0x9A, 0x02, 0x00, 0x00, 0xB9, 0xAB, 0xAA, 0xAA,
    0xAA, 0x48, 0x0F, 0xAF, 0xC8, 0x48, 0xC1, 0xE9,
    0x21, 0x8D, 0x0C, 0x49, 0x89, 0xC2, 0x29, 0xCA,
    0x83, 0xFA, 0x01, 0x48, 0x8D, 0x15, 0x00, 0x00,
    0x00, 0x00, 0x4C, 0x8D, 0x05, 0x00, 0x00, 0x00,
    0x00, 0x4C, 0x0F, 0x44, 0xC2, 0x39, 0xC8, 0x48,
    0x8D, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x49, 0x0F,
    0x45, 0xC8, 0x03, 0x71, 0x08, 0x0F, 0xB7, 0x59,
    0x14, 0x0F, 0xAF, 0x71, 0x0C,
};
constexpr std::array<std::size_t, 3> HeraldBranchDisplacements{0x36, 0x3D, 0x4A};
constexpr std::size_t                ThornsDisplacementAt = 0x3D;

// ApplyAuraEnchanted+0x4C2: movzx eax,byte ptr [selection] / test al,al /
// three argument reloads / je to the call of the game's 0x495CD0. The byte is
// aura_enchanted_selection: 0 = 3.1, nonzero = 3.2. Read only, for the log and
// the console.
constexpr std::size_t SelectionReadOffset = 0x4C2;
constexpr std::array<std::uint8_t, 31> SelectionRead{
    0x0F, 0xB6, 0x05, 0x00, 0x00, 0x00, 0x00, 0x84,
    0xC0, 0x48, 0x8B, 0x8D, 0xB0, 0x00, 0x00, 0x00,
    0x44, 0x8B, 0x85, 0xBC, 0x00, 0x00, 0x00, 0x8B,
    0x95, 0x9C, 0x00, 0x00, 0x00, 0x74, 0x52,
};
constexpr std::size_t SelectionDisplacementAt = 0x03;

// The HeraldThorns record as shipped: minLevel 0, weight 1, levelOffset 0,
// multiplier 1, divisor 10, skill 428.
constexpr std::array<std::int32_t, 6> ThornsRecordExpected{0, 1, 0, 1, 10, 428};
constexpr std::size_t                 RecordSkillField = 5;  // int32 index, byte offset 0x14
constexpr std::int32_t                VanillaSkill     = 428;
constexpr std::int32_t                MaxSkill         = 0xFFFF;  // the 3.2 path reads a word

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

constexpr char ConfigKey[] = "thorns_slot_skill";

constexpr char DefaultConfig[] = R"toml(# celestialrayone.herald-aura
#
# Replaces HeraldThorns in the herald extra-aura roll with another skill.
#
# Every herald with Aura Enchanted rolls one extra aura, all three equally
# likely: Concentration (113), Vigor (115) or HeraldThorns (428). This plugin
# swaps HeraldThorns for the skill below. Nothing else about the roll changes:
#   - level = monster level / 10, kept between 1 and 99
#   - it is applied like an item aura (stat 151, item_aura), so the skill must
#     be an aura skill in skills.txt
#
# WHEN IT APPLIES
#   Only while d2rloader.toml has d2rcore.game_rules.aura_enchanted_selection
#   at 3.2, which is D2RLoader's default. At 3.1 D2RLoader runs the game's own
#   Aura Enchanted routine, which reads the herald table inside D2R.exe; the
#   static patch herald-aura-428-to-555.json changes that table instead.
#   Herald replacements made by an Encounters plugin use their own aura list
#   and are not affected.
#
# Read when the plugin loads. Restart the game after editing.
# Console: type herald-aura to see the live skill id and which path is active.

# skills.txt *Id that replaces HeraldThorns. 428 = vanilla (no change).
thorns_slot_skill = 555
)toml";

struct Config {
    std::int32_t skill = VanillaSkill;
    bool         found = false;
};

auto IsBlank(char c) noexcept -> bool {
    return c == ' ' || c == '\t' || c == '\r';
}

// Reads `thorns_slot_skill = <integer>`. Returns false on a malformed value.
auto ParseConfig(std::string_view text, Config& out) noexcept -> bool {
    const std::string_view key(ConfigKey);
    std::size_t            position = 0;
    while (position < text.size()) {
        std::size_t end = text.find('\n', position);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(position, end - position);
        position              = end + 1;

        const std::size_t comment = line.find('#');
        if (comment != std::string_view::npos) line = line.substr(0, comment);
        std::size_t start = 0;
        while (start < line.size() && IsBlank(line[start])) ++start;
        line = line.substr(start);
        if (line.substr(0, key.size()) != key) continue;

        std::string_view rest = line.substr(key.size());
        std::size_t      cursor = 0;
        while (cursor < rest.size() && IsBlank(rest[cursor])) ++cursor;
        if (cursor >= rest.size() || rest[cursor] != '=') continue;
        ++cursor;
        while (cursor < rest.size() && IsBlank(rest[cursor])) ++cursor;

        std::int64_t value  = 0;
        std::size_t  digits = 0;
        for (; cursor < rest.size(); ++cursor) {
            const char c = rest[cursor];
            if (c == '_') continue;
            if (c < '0' || c > '9') break;
            value = value * 10 + (c - '0');
            ++digits;
            if (value > MaxSkill) return false;
        }
        while (cursor < rest.size() && IsBlank(rest[cursor])) ++cursor;
        if (digits == 0 || cursor != rest.size() || value < 1) return false;

        out.skill = static_cast<std::int32_t>(value);
        out.found = true;
        return true;
    }
    return true;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

const D2RL::PluginContext* Context         = nullptr;
std::int32_t*              ThornsSkill     = nullptr;  // D2RCore .rdata, record + 0x14
const std::uint8_t*        SelectionByte   = nullptr;  // D2RCore .data
std::int32_t               ConfiguredSkill = VanillaSkill;
std::int32_t               WrittenSkill    = VanillaSkill;
bool                       Written         = false;
const char*                InactiveReason  = "not loaded";

auto IsCommittedIn(const void* address, std::size_t size, HMODULE module, bool executable) noexcept -> bool {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info)) return false;
    if (info.State != MEM_COMMIT || info.AllocationBase != static_cast<void*>(module)) return false;
    if ((info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    const auto* regionEnd = static_cast<const std::uint8_t*>(info.BaseAddress) + info.RegionSize;
    if (static_cast<const std::uint8_t*>(address) + size > regionEnd) return false;
    if (!executable) return true;
    const DWORD execute = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (info.Protect & execute) != 0;
}

template <std::size_t Size, std::size_t Masks>
auto MatchesExcept(const std::uint8_t* actual, const std::array<std::uint8_t, Size>& expected,
                   const std::array<std::size_t, Masks>& displacements) noexcept -> bool {
    for (std::size_t index = 0; index < Size; ++index) {
        bool masked = false;
        for (const std::size_t at : displacements) {
            if (index >= at && index < at + sizeof(std::int32_t)) masked = true;
        }
        if (!masked && actual[index] != expected[index]) return false;
    }
    return true;
}

// Target of a rip-relative operand whose disp32 sits at `at` and ends the instruction.
auto RipTarget(const std::uint8_t* window, std::size_t at) noexcept -> const std::uint8_t* {
    std::int32_t displacement = 0;
    std::memcpy(&displacement, window + at, sizeof(displacement));
    return window + at + sizeof(displacement) + displacement;
}

// Finds the HeraldThorns skill field and the selection byte, checking every
// byte the plugin relies on. Logs and returns false on any mismatch.
auto Locate() noexcept -> bool {
    const HMODULE core = GetModuleHandleW(CoreModuleName);
    if (core == nullptr) {
        InactiveReason = "D2RCore.dll is not loaded";
        Context->LogError("HeraldAura: D2RCore.dll is not loaded; plugin refused.");
        return false;
    }
    const auto* function = reinterpret_cast<const std::uint8_t*>(GetProcAddress(core, CoreExportName));
    if (function == nullptr
            || !IsCommittedIn(function, HeraldBranchOffset + HeraldBranch.size(), core, true)) {
        InactiveReason = "ApplyAuraEnchanted not found";
        Context->LogError("HeraldAura: D2RCore does not export ApplyAuraEnchanted; plugin refused.");
        return false;
    }

    const std::uint8_t* branch = function + HeraldBranchOffset;
    if (!MatchesExcept(branch, HeraldBranch, HeraldBranchDisplacements)) {
        InactiveReason = "herald branch does not match";
        Context->LogError(
            "HeraldAura: the herald branch of D2RCore!ApplyAuraEnchanted is not the D2RLoader 1.3.1 "
            "code; plugin refused.");
        return false;
    }
    const std::array<std::size_t, 1> selectionMask{SelectionDisplacementAt};
    const std::uint8_t*              selection = function + SelectionReadOffset;
    if (!MatchesExcept(selection, SelectionRead, selectionMask)) {
        InactiveReason = "selection check does not match";
        Context->LogError(
            "HeraldAura: the aura_enchanted_selection check in D2RCore!ApplyAuraEnchanted is not the "
            "D2RLoader 1.3.1 code; plugin refused.");
        return false;
    }

    const std::uint8_t* record     = RipTarget(branch, ThornsDisplacementAt);
    const std::uint8_t* setting    = RipTarget(selection, SelectionDisplacementAt);
    const std::size_t   recordSize = ThornsRecordExpected.size() * sizeof(std::int32_t);
    if (!IsCommittedIn(record, recordSize, core, false) || !IsCommittedIn(setting, 1, core, false)) {
        InactiveReason = "decoded addresses are outside D2RCore";
        Context->LogError("HeraldAura: a decoded D2RCore address is not mapped data; plugin refused.");
        return false;
    }
    std::array<std::int32_t, 6> fields{};
    std::memcpy(fields.data(), record, recordSize);
    if (fields != ThornsRecordExpected) {
        InactiveReason = "HeraldThorns record does not match";
        char message[256]{};
        std::snprintf(message, sizeof(message),
            "HeraldAura: the HeraldThorns record reads %d/%d/%d/%d/%d/%d, expected 0/1/0/1/10/428; another "
            "plugin may have changed it. Plugin refused.",
            fields[0], fields[1], fields[2], fields[3], fields[4], fields[5]);
        Context->LogError(message);
        return false;
    }

    ThornsSkill   = reinterpret_cast<std::int32_t*>(const_cast<std::uint8_t*>(record)) + RecordSkillField;
    SelectionByte = setting;
    return true;
}

// Writes the skill id into D2RCore .rdata and reads it back.
auto WriteSkill(std::int32_t skill) noexcept -> bool {
    DWORD previous = 0;
    if (!VirtualProtect(ThornsSkill, sizeof(skill), PAGE_READWRITE, &previous)) return false;
    std::memcpy(ThornsSkill, &skill, sizeof(skill));
    DWORD ignored = 0;
    VirtualProtect(ThornsSkill, sizeof(skill), previous, &ignored);
    std::int32_t check = 0;
    std::memcpy(&check, ThornsSkill, sizeof(check));
    return check == skill;
}

auto SelectionText() noexcept -> const char* {
    if (SelectionByte == nullptr) return "unknown";
    return *SelectionByte != 0 ? "3.2" : "3.1";
}

auto LoadConfig() noexcept -> bool {
    ConfiguredSkill = VanillaSkill;
    if (!Context->EnsureConfig(DefaultConfig)) {
        Context->LogWarn("HeraldAura: the configuration file could not be created; HeraldThorns is kept.");
        return true;
    }
    std::vector<char> buffer(4096, '\0');
    std::uint32_t     required = 0;
    bool read = Context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size()), &required);
    if (!read && required > buffer.size() && required <= (1u << 20)) {
        buffer.assign(static_cast<std::size_t>(required) + 1, '\0');
        read = Context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size()), nullptr);
    }
    if (!read) {
        Context->LogWarn("HeraldAura: the configuration file could not be read; HeraldThorns is kept.");
        return true;
    }
    buffer.back() = '\0';

    Config parsed{};
    if (!ParseConfig(std::string_view(buffer.data()), parsed)) {
        Context->LogError(
            "HeraldAura: thorns_slot_skill must be a whole number from 1 to 65535; plugin refused. Fix "
            "or delete the TOML and start again.");
        return false;
    }
    if (!parsed.found) {
        Context->LogWarn("HeraldAura: thorns_slot_skill is missing from the TOML; HeraldThorns is kept.");
        return true;
    }
    ConfiguredSkill = parsed.skill;
    return true;
}

auto Status(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept
        -> D2RL::ConsoleCommandResult {
    if (command == nullptr || command->plugin == nullptr) return D2RL::ConsoleCommandResult::Failed;

    char message[512]{};
    if (ThornsSkill == nullptr) {
        std::snprintf(message, sizeof(message), "Herald Aura 1.0.0: not installed (%s). See the plugin log.",
                      InactiveReason);
    } else {
        std::int32_t live = 0;
        std::memcpy(&live, ThornsSkill, sizeof(live));
        const bool d2rcorePath = SelectionByte != nullptr && *SelectionByte != 0;
        std::snprintf(message, sizeof(message),
            "Herald Aura 1.0.0: HeraldThorns slot = skill %d (configured %d, vanilla %d) | "
            "aura_enchanted_selection = %s: %s",
            live, ConfiguredSkill, VanillaSkill, SelectionText(),
            d2rcorePath ? "heralds roll this slot."
                        : "D2RLoader uses the game's own herald table, so this slot is not used.");
    }
    command->plugin->WriteConsoleMessage(message);
    return D2RL::ConsoleCommandResult::Handled;
}

constexpr D2RL::PluginInfo Info{
    .infoSize    = D2RL::PluginInfoSize,
    .abiVersion  = D2RL_PLUGIN_ABI_VERSION,
    .id          = "celestialrayone.herald-aura",
    .name        = "Herald Aura",
    .version     = "1.0.0",
    .author      = "CelestialRayOne",
    .description = "Replaces HeraldThorns in the herald extra-aura roll with a configurable aura skill.",
    .flags       = D2RL::PluginFlags::Server | D2RL::PluginFlags::NativeHooks,
};

static_assert(D2RL::HasValidPluginRole(Info.flags), "Exactly one execution role must be set.");
static_assert(D2RL::HasOnlyKnownPluginFlags(Info.flags), "Unknown plugin flag set.");

} // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
    return &Info;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
    if (!D2RL::HasContext(context) || context->abiVersion != D2RL_PLUGIN_ABI_VERSION) return false;
    Context        = context;
    ThornsSkill    = nullptr;
    SelectionByte  = nullptr;
    Written        = false;
    InactiveReason = "not loaded";

    if (!LoadConfig()) return false;

    if (!context->RegisterConsoleCommand(
            "herald-aura", Status,
            "Show the HeraldThorns-slot skill and whether D2RLoader's herald roll uses it.")) {
        context->LogWarn("HeraldAura: optional status command was not registered.");
    }

    if (!Locate()) return false;

    if (ConfiguredSkill != VanillaSkill) {
        if (!WriteSkill(ConfiguredSkill)) {
            WriteSkill(VanillaSkill);
            InactiveReason = "the write did not hold";
            ThornsSkill    = nullptr;
            context->LogError("HeraldAura: the HeraldThorns record could not be written; plugin refused.");
            return false;
        }
        WrittenSkill = ConfiguredSkill;
        Written      = true;
    }

    char message[384]{};
    std::snprintf(message, sizeof(message),
        "Herald Aura 1.0.0 by CelestialRayOne active: HeraldThorns slot = skill %d (vanilla %d). "
        "aura_enchanted_selection is %s%s",
        ConfiguredSkill, VanillaSkill, SelectionText(),
        *SelectionByte != 0 ? ", so heralds roll it."
                            : ": D2RLoader uses the game's own herald table, so this plugin has no effect "
                              "until it is 3.2.");
    if (*SelectionByte != 0) {
        context->LogInfo(message);
    } else {
        context->LogWarn(message);
    }
    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    if (Written && ThornsSkill != nullptr) {
        std::int32_t live = 0;
        std::memcpy(&live, ThornsSkill, sizeof(live));
        if (live == WrittenSkill) WriteSkill(VanillaSkill);
    }
    Written       = false;
    ThornsSkill   = nullptr;
    SelectionByte = nullptr;
    Context       = nullptr;
}
