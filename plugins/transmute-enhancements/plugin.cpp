// ===========================================================================
// Transmute Hotkey (celestialrayone.transmute-hotkey)
// ===========================================================================
//
// D2RLoader's own "Transmute Horadric Cube" action (Controls menu) works with
// the cube closed and repeats while the key is held. The TOML documents the
// behaviour.
//
// Built against D2R 3.3 in the D2RLoader 1.3.1 process image (base
// 0x140000000), D2RCore.dll 1.3.1-beta and PluginSDK 0.3.0. Client side.
//
// ---------------------------------------------------------------------------
// 1. The native action
// ---------------------------------------------------------------------------
//   D2RCore registers the action "cube_transmute" ("Transmute Horadric Cube")
//   at D2RCore+0x1CD9CA with the callback D2RCore+0x328950. The callback:
//     - returns Ignored unless the event kind ([event+10h]) is 1 (pressed), so
//       holding or releasing the key does nothing;
//     - returns Ignored unless panel 0x19 (Horadric Cube) or 0x18 (stash) is
//       open, through game 0xCE500;
//     - otherwise posts the UI message "Convert" to HoradricCubePanelMessage or
//       BankPanelMessage, exactly like clicking Convert.
//
// ---------------------------------------------------------------------------
// 2. What Convert does
// ---------------------------------------------------------------------------
//   Both panels handle Convert with two calls that take no arguments: the
//   cube panel's message handler 0x2CCFA0 (at 0x2CD03A) and the stash panel's
//   0x23BCF0 (at 0x23C23C) both run 0x2CC060 then 0x2CD720. 0x2CD720 walks
//   the local player's inventory, collects the items on page 3 (the cube),
//   finds the cube itself (item code 'box '), sorts the list and sends it
//   through the CCMD_TRANSMUTE builder 0xED120. Nothing in it needs a panel.
//   The server side (packet handler 0x4C1590, check 0x4F2490) only refuses a
//   dead player, state 54, an item on the cursor, a list that does not match
//   the cube's contents, or a cube that is not a 'box ' item, and applies the
//   transmute in a later update (0x4ADAD0). It does not require the cube to
//   be open.
//
// ---------------------------------------------------------------------------
// 3. What the plugin does
// ---------------------------------------------------------------------------
//   The callback's 17 byte prologue (push rbp / push rsi / sub rsp,268h /
//   lea rbp,[rsp+80h]) is replaced with a jump to TransmuteAction, which:
//     - on press, transmutes at once (0x2CC060 then 0x2CD720, the Convert
//       sequence) and starts the repeat;
//     - while the key is held, transmutes again after repeat_delay_ms, then
//       every repeat_interval_ms, polled on the UI thread through D2RLoader's
//       ThreadService;
//     - stops on release, when the bound key (and its modifier) is no longer
//       physically down, or outside a game (D2RCore!IsInGame).
//   The whole native callback body and its registration are byte-checked
//   before anything is written, and the prologue is put back on unload.
// ===========================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <D2RLPlugin/api.h>

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

// PURE-BEGIN ------------------------------------------------------------------

struct TransmuteConfig {
    bool         enabled          = true;
    std::int64_t repeatDelayMs    = 350;
    std::int64_t repeatIntervalMs = 200;
};

constexpr std::int64_t MinimumMs = 50;
constexpr std::int64_t MaximumMs = 60000;

// Reads enabled, repeat_delay_ms and repeat_interval_ms. Other keys are
// ignored. Returns false with a message on a malformed value.
auto ParseConfig(std::string_view text, TransmuteConfig& out, std::string& error) -> bool {
    std::size_t lineNumber = 0;
    std::size_t position   = 0;
    while (position <= text.size()) {
        std::size_t end = text.find('\n', position);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(position, end - position);
        position              = end + 1;
        ++lineNumber;

        const std::size_t comment = line.find('#');
        if (comment != std::string_view::npos) line = line.substr(0, comment);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) line.remove_suffix(1);
        if (line.empty() || line.front() == '[') {
            if (end == text.size()) break;
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            error = "line " + std::to_string(lineNumber) + ": expected key = value";
            return false;
        }
        std::string_view key   = line.substr(0, equals);
        std::string_view value = line.substr(equals + 1);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.remove_suffix(1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);

        if (key == "enabled") {
            if (value == "true") {
                out.enabled = true;
            } else if (value == "false") {
                out.enabled = false;
            } else {
                error = "line " + std::to_string(lineNumber) + ": enabled must be true or false";
                return false;
            }
        } else if (key == "repeat_delay_ms" || key == "repeat_interval_ms") {
            std::int64_t number = 0;
            std::size_t  digits = 0;
            for (const char c : value) {
                if (c == '_') continue;
                if (c < '0' || c > '9') {
                    digits = 0;
                    break;
                }
                number = number * 10 + (c - '0');
                if (number > MaximumMs) break;
                ++digits;
            }
            if (digits == 0 || number < MinimumMs || number > MaximumMs) {
                error = "line " + std::to_string(lineNumber) + ": " + std::string(key) + " must be a whole number from 50 to 60000";
                return false;
            }
            (key == "repeat_delay_ms" ? out.repeatDelayMs : out.repeatIntervalMs) = number;
        }
        if (end == text.size()) break;
    }
    return true;
}

// PURE-END --------------------------------------------------------------------

constexpr char DefaultConfig[] = R"toml(# celestialrayone.transmute-hotkey
#
# Changes D2RLoader's "Transmute Horadric Cube" key (Options > Controls):
#   - it transmutes with the Horadric Cube closed, from anywhere in the game,
#     not only while the Horadric Cube or stash panel is open;
#   - holding the key keeps transmuting until you let go.
#
# The key binding itself stays in D2RLoader's Controls menu.
#
# HOW IT TRANSMUTES
#   The key does exactly what the Convert button does: the game collects the
#   items in your cube and sends the transmute to the server. The server's
#   own checks still apply: you must be alive, hold nothing on the cursor,
#   and have the cube in your inventory. An empty cube does nothing.
#
# REPEAT WHILE HELD
#   The first transmute happens when you press the key. If you keep holding
#   it, the next one comes after repeat_delay_ms, then one every
#   repeat_interval_ms until the key is released. The repeat also stops if
#   you leave the game.
#   A repeat that arrives before the server has finished the previous
#   transmute is refused by the server and does nothing, so very short
#   intervals only waste requests. The minimum is 50.
#
# Read when the game starts. Restart the game after editing.
# Console: transmutehotkey shows the state and how many transmutes it sent.

enabled = true

# Milliseconds between the first transmute and the first repeat.
repeat_delay_ms = 350

# Milliseconds between repeats while the key stays down.
repeat_interval_ms = 200
)toml";

// ---------------------------------------------------------------------------
// D2RCore 1.3.1-beta witnesses
// ---------------------------------------------------------------------------

// The native cube_transmute callback, the whole function.
constexpr std::size_t CoreCallbackOffset = 0x328950;
constexpr std::array<std::uint8_t, 346> CoreCallbackBody{
    0x55, 0x56, 0x48, 0x81, 0xEC, 0x68, 0x02, 0x00, 0x00, 0x48, 0x8D, 0xAC,
    0x24, 0x80, 0x00, 0x00, 0x00, 0x48, 0xC7, 0x85, 0xE0, 0x01, 0x00, 0x00,
    0xFE, 0xFF, 0xFF, 0xFF, 0x31, 0xF6, 0x48, 0x85, 0xD2, 0x0F, 0x84, 0x27,
    0x01, 0x00, 0x00, 0x83, 0x7A, 0x10, 0x01, 0x0F, 0x85, 0x1D, 0x01, 0x00,
    0x00, 0xB9, 0x19, 0x00, 0x00, 0x00, 0xFF, 0x15, 0x54, 0xA1, 0x3D, 0x00,
    0x90, 0x84, 0xC0, 0x74, 0x09, 0x48, 0x8D, 0x35, 0x40, 0xBA, 0x2F, 0x00,
    0xEB, 0x1B, 0xB9, 0x18, 0x00, 0x00, 0x00, 0xFF, 0x15, 0x3B, 0xA1, 0x3D,
    0x00, 0x90, 0x84, 0xC0, 0x0F, 0x84, 0xF0, 0x00, 0x00, 0x00, 0x48, 0x8D,
    0x35, 0x43, 0xBA, 0x2F, 0x00, 0x48, 0x8D, 0x4D, 0xA0, 0x41, 0xB8, 0x20,
    0x01, 0x00, 0x00, 0x31, 0xD2, 0xE8, 0x3A, 0x1B, 0x21, 0x00, 0x0F, 0x57,
    0xC0, 0x0F, 0x29, 0x85, 0x30, 0x01, 0x00, 0x00, 0x0F, 0x29, 0x85, 0x20,
    0x01, 0x00, 0x00, 0x0F, 0x29, 0x85, 0x10, 0x01, 0x00, 0x00, 0x0F, 0x29,
    0x85, 0x00, 0x01, 0x00, 0x00, 0x0F, 0x29, 0x85, 0xF0, 0x00, 0x00, 0x00,
    0x0F, 0x29, 0x85, 0xE0, 0x00, 0x00, 0x00, 0x0F, 0x29, 0x85, 0xD0, 0x00,
    0x00, 0x00, 0x0F, 0x29, 0x85, 0xC0, 0x00, 0x00, 0x00, 0x48, 0xC7, 0x85,
    0x40, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x29, 0x85, 0xC0,
    0x01, 0x00, 0x00, 0x0F, 0x29, 0x85, 0xB0, 0x01, 0x00, 0x00, 0x0F, 0x29,
    0x85, 0xA0, 0x01, 0x00, 0x00, 0x0F, 0x29, 0x85, 0x90, 0x01, 0x00, 0x00,
    0x0F, 0x29, 0x85, 0x80, 0x01, 0x00, 0x00, 0x0F, 0x29, 0x85, 0x70, 0x01,
    0x00, 0x00, 0x0F, 0x29, 0x85, 0x60, 0x01, 0x00, 0x00, 0x0F, 0x29, 0x85,
    0x50, 0x01, 0x00, 0x00, 0x48, 0xC7, 0x85, 0xD0, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x48, 0x8D, 0x8D, 0xC0, 0x00, 0x00, 0x00, 0x48, 0x89,
    0xF2, 0xFF, 0x15, 0x11, 0x7F, 0x3D, 0x00, 0x90, 0x48, 0x8D, 0x15, 0xA9,
    0xB9, 0x2F, 0x00, 0x48, 0x8D, 0x8D, 0x50, 0x01, 0x00, 0x00, 0xFF, 0x15,
    0xFC, 0x7E, 0x3D, 0x00, 0x90, 0x48, 0x8D, 0x4D, 0xA0, 0x48, 0x8D, 0x95,
    0xC0, 0x00, 0x00, 0x00, 0x4C, 0x8D, 0x85, 0x50, 0x01, 0x00, 0x00, 0xFF,
    0x15, 0xFB, 0x7E, 0x3D, 0x00, 0x90, 0x48, 0x8D, 0x4D, 0xA0, 0xFF, 0x15,
    0xE8, 0x90, 0x3D, 0x00, 0x90, 0xBE, 0x01, 0x00, 0x00, 0x00, 0x89, 0xF0,
    0x48, 0x81, 0xC4, 0x68, 0x02, 0x00, 0x00, 0x5E, 0x5D, 0xC3,
};
constexpr std::size_t CoreCallbackStolen = 17;  // push rbp / push rsi / sub rsp,268h / lea rbp,[rsp+80h]

// The registration: logical id "cube_transmute", display name "Transmute
// Horadric Cube", callback lea at +0x25 (to +0x328950), register call.
constexpr std::size_t CoreRegistrationOffset = 0x1CD9CA;
constexpr std::array<std::uint8_t, 61> CoreRegistration{
    0x0F, 0x10, 0x05, 0x8F, 0x65, 0x46, 0x00, 0x0F, 0x29, 0x45, 0xB0, 0x48,
    0x8D, 0x05, 0x94, 0x65, 0x46, 0x00, 0x48, 0x89, 0x45, 0xC0, 0x48, 0xC7,
    0x45, 0xC8, 0x17, 0x00, 0x00, 0x00, 0x0F, 0x57, 0xC0, 0x0F, 0x29, 0x45,
    0xD0, 0x48, 0x8D, 0x05, 0x5A, 0xAF, 0x15, 0x00, 0x48, 0x89, 0x45, 0xE0,
    0xC6, 0x45, 0xE8, 0x00, 0x48, 0x8D, 0x4D, 0xB0, 0xE8, 0x49, 0x48, 0x26,
    0x00,
};
constexpr std::size_t CoreRegistrationCallbackLea = 0x25;

// ---------------------------------------------------------------------------
// Game image witnesses (D2RLoader 1.3.1 process image)
// ---------------------------------------------------------------------------

constexpr std::uint64_t ConvertPrepareRva = 0x002CC060;
constexpr std::array<std::uint8_t, 24> ConvertPrepareEntry{
    0x40, 0x55, 0x48, 0x83, 0xEC, 0x20, 0xE8, 0x65, 0xF2, 0xDB, 0xFF, 0x8B,
    0xC8, 0xE8, 0x0E, 0xE4, 0xDC, 0xFF, 0x48, 0x8B, 0xE8, 0x48, 0x85, 0xC0,
};
constexpr std::uint64_t TransmuteRva = 0x002CD720;
constexpr std::array<std::uint8_t, 31> TransmuteEntry{
    0x4C, 0x8B, 0xDC, 0x55, 0x53, 0x48, 0x8D, 0x6C, 0x24, 0x88, 0x48, 0x81,
    0xEC, 0x78, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x05, 0x90, 0xDB, 0x6F, 0x02,
    0x48, 0x33, 0xC4, 0x48, 0x89, 0x45, 0x50,
};
// The stash panel's Convert: call 0x2CC060 / call 0x2CD720.
constexpr std::uint64_t StashConvertRva = 0x0023C23C;
constexpr std::array<std::uint8_t, 10> StashConvert{
    0xE8, 0x1F, 0xFE, 0x08, 0x00, 0xE8, 0xDA, 0x14, 0x09, 0x00,
};

static_assert(CoreCallbackBody[0] == 0x55 && CoreCallbackBody[1] == 0x56 && CoreCallbackBody[9] == 0x48 && CoreCallbackBody[16] == 0x00);
static_assert(CoreRegistration[CoreRegistrationCallbackLea] == 0x48 && CoreRegistration[CoreRegistrationCallbackLea + 1] == 0x8D);
static_assert(StashConvert[0] == 0xE8 && StashConvert[5] == 0xE8);

using VoidFn     = void(__fastcall*)();
using IsInGameFn = bool(__cdecl*)() noexcept;

const D2RL::PluginContext*  Context   = nullptr;
const D2RL::ThreadService*  ThreadSvc = nullptr;
std::uintptr_t              Base      = 0;
std::atomic<bool>           Active{ false };
TransmuteConfig             Settings{};
std::string                 ConfigError;

VoidFn     ConvertPrepare = nullptr;
VoidFn     Transmute      = nullptr;
IsInGameFn IsInGame       = nullptr;

// UI thread only: the action callback and the poll both run there.
bool          Held         = false;
bool          Polling      = false;
std::uint32_t HeldKey      = 0;
std::uint32_t HeldModifier = 0;
ULONGLONG     NextRepeat   = 0;

std::atomic<std::uint64_t> Presses{ 0 };
std::atomic<std::uint64_t> Transmutes{ 0 };
std::atomic<std::uint64_t> Repeats{ 0 };

enum class PluginState { NotLoaded, DisabledByConfig, BadConfig, UnsupportedBuild, InstallFailed, Running };
PluginState State = PluginState::NotLoaded;

std::uint8_t*                CoreCallback = nullptr;
std::array<std::uint8_t, 32> SavedPrologue{};
bool                         PrologueReplaced = false;

void LogLine(int level, const char* format, ...) noexcept {
    if (Context == nullptr) return;
    char    buffer[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (level == 0) {
        Context->LogInfo(buffer);
    } else if (level == 1) {
        Context->LogWarn(buffer);
    } else {
        Context->LogError(buffer);
    }
}

template <typename T>
auto At(std::uint64_t rva) noexcept -> T {
    return reinterpret_cast<T>(Base + static_cast<std::uintptr_t>(rva));
}

// The Convert button's two calls.
void TransmuteNow() noexcept {
    ConvertPrepare();
    Transmute();
    Transmutes.fetch_add(1, std::memory_order_relaxed);
}

auto ModifierKey(std::uint32_t modifier) noexcept -> int {
    switch (static_cast<D2RL::Input::Modifier>(modifier)) {
    case D2RL::Input::Modifier::Shift: return VK_SHIFT;
    case D2RL::Input::Modifier::Control: return VK_CONTROL;
    case D2RL::Input::Modifier::Alt: return VK_MENU;
    default: return 0;
    }
}

auto KeyStillDown() noexcept -> bool {
    if (HeldKey == 0) return false;
    if ((GetAsyncKeyState(static_cast<int>(HeldKey)) & 0x8000) == 0) return false;
    const int modifier = ModifierKey(HeldModifier);
    return modifier == 0 || (GetAsyncKeyState(modifier) & 0x8000) != 0;
}

void __cdecl Poll(const D2RL::PluginContext*, void*) noexcept;

void QueuePoll() noexcept {
    if (Polling || ThreadSvc == nullptr) return;
    if (ThreadSvc->runOnUiThread(Context, &Poll, nullptr) == D2RL::Threads::Result::Success) {
        Polling = true;
    } else {
        Held = false;
    }
}

void __cdecl Poll(const D2RL::PluginContext*, void*) noexcept {
    Polling = false;
    if (!Held || !Active.load(std::memory_order_relaxed)) return;
    if (!KeyStillDown() || IsInGame == nullptr || !IsInGame()) {
        Held = false;
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (now >= NextRepeat) {
        TransmuteNow();
        Repeats.fetch_add(1, std::memory_order_relaxed);
        NextRepeat = now + static_cast<ULONGLONG>(Settings.repeatIntervalMs);
    }
    QueuePoll();
}

// Replaces D2RCore's cube_transmute callback.
auto __cdecl TransmuteAction(const D2RL::PluginContext*, const D2RL::Input::ActionEvent* event, void*) noexcept
    -> D2RL::Input::ActionResult {
    if (event == nullptr || !Active.load(std::memory_order_relaxed)) return D2RL::Input::ActionResult::Ignored;
    if (event->kind == D2RL::Input::ActionEventKind::Released) {
        Held = false;
        return D2RL::Input::ActionResult::Ignored;
    }
    if (event->kind != D2RL::Input::ActionEventKind::Pressed) return D2RL::Input::ActionResult::Ignored;
    if (Held) return D2RL::Input::ActionResult::Handled;  // auto-repeat of a key already held
    if (IsInGame == nullptr || !IsInGame()) return D2RL::Input::ActionResult::Ignored;

    Presses.fetch_add(1, std::memory_order_relaxed);
    HeldKey      = 0;
    HeldModifier = 0;
    if (D2RL::Input::HasActionEventField(event, D2RL::Input::ActionEventRequiredSize)) {
        HeldKey      = static_cast<std::uint32_t>(event->binding.key);
        HeldModifier = static_cast<std::uint32_t>(event->binding.modifier);
    }
    TransmuteNow();
    // The repeat needs the bound key to see when it is let go. Without it the
    // press transmutes once, like the native action.
    if (HeldKey == 0) return D2RL::Input::ActionResult::Handled;
    Held       = true;
    NextRepeat = GetTickCount64() + static_cast<ULONGLONG>(Settings.repeatDelayMs);
    QueuePoll();
    return D2RL::Input::ActionResult::Handled;
}

auto ReadConfiguration() noexcept -> bool {
    if (!Context->EnsureConfig(DefaultConfig)) {
        LogLine(1, "TransmuteHotkey: the configuration file could not be created; using the defaults.");
    }
    std::vector<char> buffer(8192, '\0');
    std::uint32_t     required = 0;
    bool read = Context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size()), &required);
    if (!read && required > buffer.size() && required <= (1u << 20)) {
        buffer.assign(static_cast<std::size_t>(required) + 1, '\0');
        read = Context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size()), nullptr);
    }
    if (!read) {
        LogLine(1, "TransmuteHotkey: the configuration file could not be read; using the defaults.");
        return true;
    }
    TransmuteConfig parsed{};
    std::string     error;
    if (!ParseConfig(std::string_view(buffer.data()), parsed, error)) {
        ConfigError = error;
        LogLine(2, "TransmuteHotkey: the TOML is malformed (%s); nothing was installed.", error.c_str());
        return false;
    }
    Settings = parsed;
    return true;
}

auto LocateCore() noexcept -> bool {
    const HMODULE core = GetModuleHandleW(L"D2RCore.dll");
    if (core == nullptr) {
        LogLine(2, "TransmuteHotkey: D2RCore.dll is not loaded.");
        return false;
    }
    IsInGame = reinterpret_cast<IsInGameFn>(reinterpret_cast<void*>(GetProcAddress(core, "IsInGame")));
    if (IsInGame == nullptr) {
        LogLine(2, "TransmuteHotkey: D2RCore does not export IsInGame.");
        return false;
    }
    auto* base = reinterpret_cast<std::uint8_t*>(core);
    const auto* dos = base;
    std::uint32_t ntOffset = 0;
    std::memcpy(&ntOffset, dos + 0x3C, sizeof(ntOffset));
    std::uint32_t imageSize = 0;
    std::memcpy(&imageSize, dos + ntOffset + 0x50, sizeof(imageSize));
    if (CoreCallbackOffset + CoreCallbackBody.size() > imageSize || CoreRegistrationOffset + CoreRegistration.size() > imageSize) {
        LogLine(2, "TransmuteHotkey: D2RCore is smaller than the 1.3.1 build.");
        return false;
    }
    if (std::memcmp(base + CoreRegistrationOffset, CoreRegistration.data(), CoreRegistration.size()) != 0) {
        LogLine(2, "TransmuteHotkey: D2RCore's Transmute Horadric Cube registration is not the 1.3.1 code; nothing installed.");
        return false;
    }
    std::int32_t displacement = 0;
    std::memcpy(&displacement, base + CoreRegistrationOffset + CoreRegistrationCallbackLea + 3, sizeof(displacement));
    const auto registered = static_cast<std::size_t>(static_cast<std::int64_t>(CoreRegistrationOffset + CoreRegistrationCallbackLea + 7) + displacement);
    if (registered != CoreCallbackOffset) {
        LogLine(2, "TransmuteHotkey: the action registers a different callback; nothing installed.");
        return false;
    }
    if (std::memcmp(base + CoreCallbackOffset, CoreCallbackBody.data(), CoreCallbackBody.size()) != 0) {
        LogLine(2, "TransmuteHotkey: D2RCore's transmute callback is not the 1.3.1 code (or another plugin hooked it); nothing installed.");
        return false;
    }
    CoreCallback = base + CoreCallbackOffset;
    return true;
}

auto Verify() noexcept -> bool {
    struct Witness { std::uint64_t rva; const std::uint8_t* bytes; std::uint32_t size; const char* name; };
    const Witness witnesses[]{
        { ConvertPrepareRva, ConvertPrepareEntry.data(), static_cast<std::uint32_t>(ConvertPrepareEntry.size()), "Convert step 1, 0x2CC060" },
        { TransmuteRva, TransmuteEntry.data(), static_cast<std::uint32_t>(TransmuteEntry.size()), "Convert step 2, 0x2CD720" },
        { StashConvertRva, StashConvert.data(), static_cast<std::uint32_t>(StashConvert.size()), "stash panel Convert, 0x23C23C" },
    };
    for (const auto& w : witnesses) {
        if (!Context->CheckExpectedBytes(w.rva, w.bytes, w.size)) {
            LogLine(2, "TransmuteHotkey: game bytes differ at RVA 0x%llX (%s); this is not the D2RLoader 1.3.1 image.",
                static_cast<unsigned long long>(w.rva), w.name);
            return false;
        }
    }
    // Both Convert calls in the stash panel must land on the two entries above.
    std::int32_t first = 0, second = 0;
    std::memcpy(&first, StashConvert.data() + 1, sizeof(first));
    std::memcpy(&second, StashConvert.data() + 6, sizeof(second));
    if (StashConvertRva + 5 + static_cast<std::uint64_t>(static_cast<std::int64_t>(first)) != ConvertPrepareRva
        || StashConvertRva + 10 + static_cast<std::uint64_t>(static_cast<std::int64_t>(second)) != TransmuteRva) {
        LogLine(2, "TransmuteHotkey: the stash panel's Convert does not call the expected functions.");
        return false;
    }
    return true;
}

auto ReplacePrologue() noexcept -> bool {
    std::array<std::uint8_t, 32> patch{};
    patch.fill(0xCC);
    static constexpr std::uint8_t JumpIndirect[6]{ 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };  // jmp [rip+0]
    std::memcpy(patch.data(), JumpIndirect, sizeof(JumpIndirect));
    const auto destination = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&TransmuteAction));
    std::memcpy(patch.data() + sizeof(JumpIndirect), &destination, sizeof(destination));

    DWORD previous = 0;
    if (!VirtualProtect(CoreCallback, CoreCallbackStolen, PAGE_EXECUTE_READWRITE, &previous)) return false;
    std::memcpy(SavedPrologue.data(), CoreCallback, CoreCallbackStolen);
    std::memcpy(CoreCallback, patch.data(), CoreCallbackStolen);
    DWORD ignored = 0;
    VirtualProtect(CoreCallback, CoreCallbackStolen, previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), CoreCallback, CoreCallbackStolen);
    PrologueReplaced = std::memcmp(CoreCallback, patch.data(), CoreCallbackStolen) == 0;
    return PrologueReplaced;
}

void RestorePrologue() noexcept {
    if (!PrologueReplaced || CoreCallback == nullptr) return;
    DWORD previous = 0;
    if (!VirtualProtect(CoreCallback, CoreCallbackStolen, PAGE_EXECUTE_READWRITE, &previous)) return;
    std::memcpy(CoreCallback, SavedPrologue.data(), CoreCallbackStolen);
    DWORD ignored = 0;
    VirtualProtect(CoreCallback, CoreCallbackStolen, previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), CoreCallback, CoreCallbackStolen);
    PrologueReplaced = false;
}

auto StateName() noexcept -> const char* {
    switch (State) {
    case PluginState::NotLoaded: return "not loaded";
    case PluginState::DisabledByConfig: return "disabled in the TOML";
    case PluginState::BadConfig: return "TOML error, nothing installed";
    case PluginState::UnsupportedBuild: return "game or D2RCore build does not match, nothing installed";
    case PluginState::InstallFailed: return "install failed, nothing active";
    case PluginState::Running: return "active";
    }
    return "?";
}

auto Status(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
    if (command == nullptr || command->plugin == nullptr) return D2RL::ConsoleCommandResult::Failed;
    char line[512];
    std::snprintf(line, sizeof(line), "Transmute Hotkey 1.0.0: %s%s%s", StateName(), ConfigError.empty() ? "" : " | ", ConfigError.c_str());
    command->plugin->WriteConsoleMessage(line);
    std::snprintf(line, sizeof(line), "  repeat after %lld ms, then every %lld ms | presses %llu, transmutes sent %llu (repeats %llu)",
        static_cast<long long>(Settings.repeatDelayMs), static_cast<long long>(Settings.repeatIntervalMs),
        static_cast<unsigned long long>(Presses.load()), static_cast<unsigned long long>(Transmutes.load()),
        static_cast<unsigned long long>(Repeats.load()));
    command->plugin->WriteConsoleMessage(line);
    return D2RL::ConsoleCommandResult::Handled;
}

constexpr D2RL::PluginInfo Info{
    .infoSize    = D2RL::PluginInfoSize,
    .abiVersion  = D2RL_PLUGIN_ABI_VERSION,
    .id          = "celestialrayone.transmute-hotkey",
    .name        = "Transmute Hotkey",
    .version     = "1.0.0",
    .author      = "CelestialRayOne",
    .description = "The Transmute Horadric Cube key works with the cube closed and repeats while held.",
    .flags       = D2RL::PluginFlags::Client | D2RL::PluginFlags::NativeHooks,
    .reserved    = {},
};

static_assert(D2RL::HasValidPluginRole(Info.flags), "Exactly one execution role must be set.");
static_assert(D2RL::HasOnlyKnownPluginFlags(Info.flags), "Unknown plugin flag set.");

}  // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
    return &Info;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
    if (!D2RL::HasContext(context) || context->abiVersion != D2RL_PLUGIN_ABI_VERSION || context->exeBase == 0) return false;
    Context = context;
    Base    = context->exeBase;

    if (!Context->RegisterConsoleCommand("transmutehotkey", &Status, "Show the Transmute Hotkey state and counters.")) {
        LogLine(1, "TransmuteHotkey: the console command could not be registered.");
    }
    if (!ReadConfiguration()) {
        State = PluginState::BadConfig;
        return true;
    }
    if (!Settings.enabled) {
        State = PluginState::DisabledByConfig;
        LogLine(0, "TransmuteHotkey: disabled in the TOML.");
        return true;
    }
    if (Context->QueryService(&ThreadSvc) != D2RL::ServiceQueryResult::Success
        || !D2RL::HasThreadServiceField(ThreadSvc, D2RL::ThreadServiceRequiredSize)) {
        ThreadSvc = nullptr;
        LogLine(1, "TransmuteHotkey: D2RLoader's thread service is unavailable; holding the key will not repeat.");
    }
    if (!LocateCore() || !Verify()) {
        State = PluginState::UnsupportedBuild;
        return true;
    }
    ConvertPrepare = At<VoidFn>(ConvertPrepareRva);
    Transmute      = At<VoidFn>(TransmuteRva);
    Active.store(true);
    if (!ReplacePrologue()) {
        Active.store(false);
        RestorePrologue();
        State = PluginState::InstallFailed;
        LogLine(2, "TransmuteHotkey: D2RCore's transmute callback could not be replaced; nothing installed.");
        return true;
    }
    State = PluginState::Running;
    LogLine(0, "TransmuteHotkey: active (cube may be closed; repeat after %lld ms, every %lld ms).",
        static_cast<long long>(Settings.repeatDelayMs), static_cast<long long>(Settings.repeatIntervalMs));
    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    Active.store(false);
    Held = false;
    RestorePrologue();
}
