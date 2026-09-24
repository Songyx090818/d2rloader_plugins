// Spelldesc Decimals
//
// Lets an item's spelldesc string show its number with decimals: the string
// uses %.1f, %.2f or %.3f and calc1 gives the number in tenths, hundredths or
// thousandths.
//
// Everything below was read out of the D2RLoader 1.3.0 process image and is
// checked byte for byte at load. Nothing is installed when a check fails.
//
// ---------------------------------------------------------------------------
// Where the number is printed
// ---------------------------------------------------------------------------
//   The spelldesc line builder 2C1B80 (items record in rdi) evaluates calc1
//   ([rdi+0A4h]) with the item formula evaluator 3B4880, which returns an int.
//   Modes 4 and 2 (mode 2 after its stat1 life or mana scaling at 2C1D5F) both
//   continue at 2C1C93 and print the number through one call:
//     2C1C93  0F B7 8F B6 00 00 00   movzx ecx, word [rdi+0B6h]  ; spelldescstr
//     2C1C9A  E8 BD 99 B6 03         call  GetNamespacedStringById
//     2C1C9F  48 8B D0               mov   rdx, rax              ; the string
//     2C1CA2  48 8D 8C 24 80 00 00 00  lea rcx, [rsp+80h]        ; 512-byte buffer
//     2C1CAA  44 8B C3               mov   r8d, ebx              ; the number
//     2C1CAD  E8 DE 7F E4 FF         call  109C90                ; format
//   109C90 is int f(char* out, const char* format, ...): it spills the
//   variadic registers and calls the game's formatter 122A670 with a 512-byte
//   limit. Mode 3 prints its number with a hard-coded "%d" at 2C1D14 instead,
//   so it keeps whole numbers.
//
// ---------------------------------------------------------------------------
// The change
// ---------------------------------------------------------------------------
//   The call at 2C1CAD goes to HookedFormat instead, which receives the same
//   three registers. When the string's first conversion is %.1f, %.2f or
//   %.3f, it writes the number as fixed point (value / 10^N with N decimals,
//   in exact integer maths), swaps that conversion for %s and hands the new
//   string and the digits to 109C90. Any other string goes to 109C90 exactly
//   as the game passed it.

#include <D2RLPlugin/api.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

namespace {

constexpr char PluginIdText[] = "celestialrayone.spelldesc-decimals";

const D2RL::PluginContext* Context{};
std::uintptr_t             Base{};

// ---------------------------------------------------------------------------
// Native contract
// ---------------------------------------------------------------------------

constexpr std::uint64_t WindowRva     = 0x2C1C93;
constexpr std::uint64_t CallRva       = 0x2C1CAD;
constexpr std::uint64_t FormatRva     = 0x109C90;
constexpr std::size_t   CallSize      = 5;
constexpr std::size_t   CallOffset    = CallRva - WindowRva;

// 2C1C93..2C1CBD: spelldescstr lookup, the three arguments and the format call.
constexpr std::uint8_t Window[]{
    0x0F,0xB7,0x8F,0xB6,0x00,0x00,0x00,
    0xE8,0xBD,0x99,0xB6,0x03,
    0x48,0x8B,0xD0,
    0x48,0x8D,0x8C,0x24,0x80,0x00,0x00,0x00,
    0x44,0x8B,0xC3,
    0xE8,0xDE,0x7F,0xE4,0xFF,
    0x4C,0x8D,0x84,0x24,0x80,0x00,0x00,0x00,
    0xBA,0x00,0x04,0x00,0x00 };
static_assert(CallOffset == 26);

// 109C90: spill rdx, r8, r9 / sub rsp,28h / r8 = format / r9 = va / edx = 200h / call 122A670.
constexpr std::uint8_t FormatEntry[]{
    0x48,0x89,0x54,0x24,0x10, 0x4C,0x89,0x44,0x24,0x18, 0x4C,0x89,0x4C,0x24,0x20,
    0x48,0x83,0xEC,0x28, 0x4C,0x8B,0xC2, 0x4C,0x8D,0x4C,0x24,0x40, 0xBA,0x00,0x02,0x00,0x00 };

using FormatFn = int (*)(char* out, const char* format, ...);
FormatFn OriginalFormat{};

// ---------------------------------------------------------------------------
// State and config
// ---------------------------------------------------------------------------

enum class PluginState { NotLoaded, DisabledByConfig, UnsupportedBuild, InstallFailed, Active };
PluginState State{ PluginState::NotLoaded };

constexpr char DefaultConfigToml[] =
    "# Spelldesc Decimals\n"
    "#\n"
    "# Item tooltips (weapons.txt, armor.txt, misc.txt) can print a spelldesc line:\n"
    "# spelldescstr with a number from calc1. The formulas only work in whole\n"
    "# numbers, so this plugin lets that string ask for a number with decimals.\n"
    "#\n"
    "# How to use it:\n"
    "#   In the string that spelldescstr points to, write %.1f, %.2f or %.3f where\n"
    "#   the number goes. calc1 then gives the number in tenths, hundredths or\n"
    "#   thousandths, and the tooltip shows it with that many decimals.\n"
    "#   Strings that use %d are left exactly as before.\n"
    "#\n"
    "# Example, reload time in seconds (a frame is 0.04 seconds):\n"
    "#   calc1  = skill('Attack'.clc6)*4\n"
    "#   string = Reload Time: %.2f\n"
    "#   72 frames shows as 2.88\n"
    "# For one decimal: calc1 = skill('Attack'.clc6)*2/5 shows 2.8 (cut off),\n"
    "#   or (skill('Attack'.clc6)*2+2)/5 shows 2.9 (rounded).\n"
    "#\n"
    "# spelldesc modes 2 and 4 read the string this way. Mode 3 always prints a\n"
    "# whole number after the string.\n"
    "#\n"
    "# Console command: spelldescdecimals (install status)\n"
    "\n"
    "[spelldesc_decimals]\n"
    "\n"
    "# Master switch.\n"
    "enabled = true\n";

constexpr std::size_t MaximumConfigBytes = 16 * 1024;
bool Enabled{ true };

auto Trim(std::string_view value) noexcept -> std::string_view {
    std::size_t first = 0;
    while (first < value.size() && (value[first] == ' ' || value[first] == '\t'
            || value[first] == '\r' || value[first] == '\n')) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first && (value[last - 1] == ' ' || value[last - 1] == '\t'
            || value[last - 1] == '\r' || value[last - 1] == '\n')) {
        --last;
    }
    return value.substr(first, last - first);
}

void ParseConfig(std::string_view text) {
    std::string_view section;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (const std::size_t hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);
        line = Trim(line);
        if (line.empty()) continue;
        if (line.front() == '[') {
            if (line.back() == ']') section = Trim(line.substr(1, line.size() - 2));
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos || section != "spelldesc_decimals") continue;
        const std::string_view key   = Trim(line.substr(0, equals));
        const std::string_view value = Trim(line.substr(equals + 1));
        if (key == "enabled") {
            if (value == "true") Enabled = true;
            else if (value == "false") Enabled = false;
            else Context->LogWarn("SpelldescDecimals: enabled: expected true or false, default kept.");
        }
    }
}

void ReadConfiguration() {
    if (!Context->EnsureConfig(DefaultConfigToml)) {
        Context->LogWarn("SpelldescDecimals: the config file could not be created; using defaults.");
        return;
    }
    std::string buffer(MaximumConfigBytes, '\0');
    if (!Context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size()), nullptr)) {
        Context->LogWarn("SpelldescDecimals: the config file could not be read; using defaults.");
        return;
    }
    buffer.resize(std::strlen(buffer.c_str()));
    ParseConfig(buffer);
}

// ---------------------------------------------------------------------------
// Hook
// ---------------------------------------------------------------------------

// The first conversion in the string, if it is %.1f, %.2f or %.3f.
auto FindDecimalConversion(const char* format, std::size_t& position, int& decimals) noexcept -> bool {
    for (std::size_t i = 0; format[i] != '\0'; ++i) {
        if (format[i] != '%') continue;
        if (format[i + 1] == '%') { ++i; continue; }   // literal percent sign
        if (format[i + 1] == '.' && format[i + 2] >= '1' && format[i + 2] <= '3' && format[i + 3] == 'f') {
            position = i;
            decimals = format[i + 2] - '0';
            return true;
        }
        return false;
    }
    return false;
}

// Replaces the call at 2C1CAD: rcx = 512-byte buffer, rdx = the string, r8d = calc1.
int __fastcall HookedFormat(char* out, const char* format, int value) noexcept {
    std::size_t position = 0;
    int         decimals = 0;
    if (format == nullptr || !FindDecimalConversion(format, position, decimals)) {
        return OriginalFormat(out, format, value);
    }
    const std::size_t length = std::strlen(format);
    char rewritten[1024];
    if (length + 1 > sizeof(rewritten)) return OriginalFormat(out, format, value);

    // Exact fixed point: value / 10^decimals.
    long long scale = 1;
    for (int i = 0; i < decimals; ++i) scale *= 10;
    const bool      negative  = value < 0;
    const long long magnitude = negative ? -static_cast<long long>(value) : static_cast<long long>(value);
    char number[48];
    std::snprintf(number, sizeof(number), "%s%lld.%0*lld", negative ? "-" : "", magnitude / scale, decimals,
        magnitude % scale);

    // Same string with the 4-character %.Nf swapped for %s.
    std::memcpy(rewritten, format, position);
    rewritten[position]     = '%';
    rewritten[position + 1] = 's';
    std::memcpy(rewritten + position + 2, format + position + 4, length - position - 4 + 1);
    return OriginalFormat(out, rewritten, number);
}

// ---------------------------------------------------------------------------
// Relay page and install
// ---------------------------------------------------------------------------

constexpr std::size_t RelayPageBytes = 4'096;
void* RelayPage{};
bool  Patched{};

auto CanEncodeRel32(std::uintptr_t from, std::uintptr_t to) noexcept -> bool {
    const std::int64_t delta = static_cast<std::int64_t>(to) - static_cast<std::int64_t>(from + CallSize);
    return delta >= std::numeric_limits<std::int32_t>::min() && delta <= std::numeric_limits<std::int32_t>::max();
}

auto AllocateNear(std::uintptr_t site) noexcept -> void* {
    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    const auto granularity = static_cast<std::uintptr_t>(systemInfo.dwAllocationGranularity);
    const auto aligned     = site & ~(granularity - 1U);
    for (std::uintptr_t delta = granularity; delta < 0x7000'0000ULL; delta += granularity) {
        const auto candidate = aligned + delta;
        if (!CanEncodeRel32(site, candidate + RelayPageBytes)) break;
        if (void* memory = VirtualAlloc(reinterpret_cast<void*>(candidate), RelayPageBytes,
                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) {
            return memory;
        }
    }
    return nullptr;
}

// jmp qword [rip+0] / dq HookedFormat
auto BuildRelayPage() -> bool {
    RelayPage = AllocateNear(Base + CallRva);
    if (RelayPage == nullptr) return false;
    auto* page = static_cast<std::uint8_t*>(RelayPage);
    std::memset(page, 0xCC, RelayPageBytes);
    const std::uint8_t jump[]{ 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
    std::memcpy(page, jump, sizeof(jump));
    const auto target = reinterpret_cast<std::uint64_t>(&HookedFormat);
    std::memcpy(page + sizeof(jump), &target, sizeof(target));
    DWORD previous = 0;
    const bool sealed = VirtualProtect(page, RelayPageBytes, PAGE_EXECUTE_READ, &previous) != FALSE;
    FlushInstructionCache(GetCurrentProcess(), page, RelayPageBytes);
    return sealed;
}

void EncodeCall(std::uint8_t (&out)[CallSize]) noexcept {
    const auto rel = static_cast<std::int32_t>(
        static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(RelayPage))
        - static_cast<std::int64_t>(Base + CallRva + CallSize));
    out[0] = 0xE8;
    std::memcpy(out + 1, &rel, sizeof(rel));
}

void RemovePatch() {
    if (!Patched) return;
    std::uint8_t current[CallSize]{};
    EncodeCall(current);
    if (Context->PatchBytes(CallRva, current, CallSize, Window + CallOffset, CallSize)) Patched = false;
    // The relay page is kept: a thread may be inside it right now.
}

auto Install() -> bool {
    if (!Context->CheckExpectedBytes(WindowRva, Window, sizeof(Window))) {
        D2RL::LogErrorF(Context, "SpelldescDecimals: the spelldesc format call at RVA 0x%llX does not match this "
            "build.", static_cast<unsigned long long>(WindowRva));
        State = PluginState::UnsupportedBuild;
        return false;
    }
    if (!Context->CheckExpectedBytes(FormatRva, FormatEntry, sizeof(FormatEntry))) {
        D2RL::LogErrorF(Context, "SpelldescDecimals: the formatter at RVA 0x%llX does not match this build.",
            static_cast<unsigned long long>(FormatRva));
        State = PluginState::UnsupportedBuild;
        return false;
    }
    OriginalFormat = reinterpret_cast<FormatFn>(Base + FormatRva);
    if (!BuildRelayPage()) {
        Context->LogError("SpelldescDecimals: no relay page could be placed within reach of the game code.");
        State = PluginState::InstallFailed;
        return false;
    }
    std::uint8_t call[CallSize]{};
    EncodeCall(call);
    Patched = Context->PatchBytes(CallRva, Window + CallOffset, CallSize, call, CallSize);
    if (!Patched) {
        Context->LogError("SpelldescDecimals: the format call could not be patched.");
        State = PluginState::InstallFailed;
        return false;
    }
    State = PluginState::Active;
    return true;
}

auto StateName(PluginState state) noexcept -> const char* {
    switch (state) {
    case PluginState::NotLoaded:        return "not loaded";
    case PluginState::DisabledByConfig: return "disabled by config";
    case PluginState::UnsupportedBuild: return "unsupported build, nothing installed";
    case PluginState::InstallFailed:    return "install failed, nothing active";
    case PluginState::Active:           return "active: %.1f, %.2f and %.3f in spelldesc strings show decimals";
    }
    return "?";
}

auto __cdecl StatusCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept
        -> D2RL::ConsoleCommandResult {
    char line[160];
    std::snprintf(line, sizeof(line), "Spelldesc Decimals: %s.", StateName(State));
    command->plugin->WriteConsoleMessage(line);
    return D2RL::ConsoleCommandResult::Handled;
}

constexpr D2RL::PluginInfo PluginInfoData{
    .infoSize    = D2RL::PluginInfoSize,
    .apiVersion  = D2RL_PLUGIN_API_VERSION,
    .id          = PluginIdText,
    .name        = "Spelldesc Decimals",
    .version     = "1.0.0",
    .author      = "CelestialRayOne",
    .description = "Item spelldesc strings can show their number with decimals (%.1f, %.2f, %.3f).",
    .flags       = D2RL::PluginFlags::Shared | D2RL::PluginFlags::NativeHooks,
};

} // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
    return &PluginInfoData;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
    Context = context;
    if (Context == nullptr || Context->exeBase == 0) return false;
    Base = Context->exeBase;

    if (!Context->RegisterConsoleCommand("spelldescdecimals", &StatusCommand,
            "Show Spelldesc Decimals install state.")) {
        Context->LogWarn("SpelldescDecimals: the console command could not be registered.");
    }
    ReadConfiguration();
    if (!Enabled) {
        State = PluginState::DisabledByConfig;
        Context->LogInfo("SpelldescDecimals: disabled by config.");
        return true;
    }
    if (!Install()) {
        Context->LogError("SpelldescDecimals: nothing was installed.");
        return true;
    }
    Context->LogInfo("SpelldescDecimals: active; %.1f, %.2f and %.3f in spelldesc strings show decimals.");
    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    if (Context != nullptr) RemovePatch();
}
