// Respec Counter
//
// [d2rcore.player] enable_respec turns on the game's own respec shortcut:
// Alt + click a stat button resets every stat and skill point. This plugin
// puts a counter on that shortcut, saved on the character, and shows the
// count in the stat button's tooltip.
//
// ---------------------------------------------------------------------------
// The counter
// ---------------------------------------------------------------------------
//   The character's own value (base value, layer 0) of itemstatcost row
//   stat_id (237 by default). A new character starts with starting_respecs
//   (5). Each shortcut respec costs 1; at 0 the shortcut does nothing. The
//   plugin never adds to it: the Token of Absolution's misc.txt pSpell does.
//
//   D2RLoader's extended character save (D2RCore WritePlayerStats and
//   ReadPlayerStats) writes every own stat whose CSvBits is 1 to 32 with a
//   16-bit stat id and reads it back, so the counter persists when its row
//   has CSvBits above 0.
//
// ---------------------------------------------------------------------------
// The shortcut (host)
// ---------------------------------------------------------------------------
//   The client sends opcode 39h: one opcode byte, then a dword NPC unit id.
//   The server callback 4B2530 (game, player, packet, size) copies the packet,
//   and when the id is -1 and the game flags at game+5658h have bit 800000h
//   (what enable_respec sets) it skips every NPC and quest check, calls the
//   respec 580F20 and returns 0. Any other id is Akara's quest respec, which
//   this plugin leaves alone. With the flag off, -1 fails the NPC lookup and
//   the callback returns 1 without a respec.
//
//   The hook reads the counter before the original runs. 0 or less: refused,
//   the original never runs and nothing changes. Otherwise the original runs
//   and, when it returns 0, the counter is written back one lower. The respec
//   only resets skills (refunded to stat 5) and stats 0 to 3 (refunded to
//   stat 4), so it never touches the counter.
//
// ---------------------------------------------------------------------------
// New characters (host)
// ---------------------------------------------------------------------------
//   52D770 (game, player, act) writes a brand-new character's starting stats
//   from charstats.txt. Its only two callers, 420070 and 5355B0, allocate a
//   new player unit and then hand out the starting stats, items and skill.
//   The hook runs the original, then writes starting_respecs.
//
// ---------------------------------------------------------------------------
// The tooltip (client)
// ---------------------------------------------------------------------------
//   The stat button is a CharacterStatLevelUpButtonWidget (vtable 1FDD938,
//   stored by its factory at 14EE1C8). Its tooltip is the layout's
//   tooltipString, resolved once at load into a game string inside the
//   widget: text pointer at +88h, length at +90h. 875980 draws a hovered
//   widget's tooltip by handing that pointer to the tooltip dispatcher
//   (875A44), which copies the text. For this widget only, the hook swaps in
//   a copy with the first %d replaced by the local player's counter for the
//   length of the call, then puts the original pointer and length back.
//
// ---------------------------------------------------------------------------
// Native pieces, D2RLoader 1.3.1 process image. Every one is checked byte for
// byte at load, before any hook is installed.
// ---------------------------------------------------------------------------
//   4B2530  opcode 39h server callback, hooked
//   4B259F  its shortcut test: cmp eax, -1 / test dword [rdi+5658h], 800000h
//   52D770  new character starting stats, hooked
//   875980  hovered widget tooltip draw, hooked
//   875A44  mov rcx, [rdi+88h]: the tooltip text it hands on
//   14EE19A mov edx, 0B90h: CharacterStatLevelUpButtonWidget object size
//   14EE1C8 lea rax, [1FDD938] / mov dword [rbx+0B88h], 0 / mov [rbx], rax
//   2F48C0  own value read (unit, stat, layer), loader thunk into D2RCore
//   2F7D10  own value write (unit, stat, value, layer), loader thunk
//   8B2D0   local player index        9A480   local player from that index

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
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace {

constexpr char PluginIdText[]  = "celestialrayone.respec-counter";
constexpr char PluginVersion[] = "1.0.0";

const D2RL::PluginContext* Context{};
std::uintptr_t             Base{};

// ---------------------------------------------------------------------------
// Addresses (RVAs into the D2RLoader 1.3.1 process image)
// ---------------------------------------------------------------------------

constexpr std::uint64_t RespecCommandRva    = 0x4B2530;
constexpr std::uint64_t ShortcutTestRva     = 0x4B259F;
constexpr std::uint64_t StartingStatsRva    = 0x52D770;
constexpr std::uint64_t WidgetTooltipRva    = 0x875980;
constexpr std::uint64_t TooltipTextLoadRva  = 0x875A44;
constexpr std::uint64_t StatButtonSizeRva   = 0x14EE19A;
constexpr std::uint64_t StatButtonVtableLea = 0x14EE1C8;
constexpr std::uint64_t ReadOwnStatRva      = 0x2F48C0;
constexpr std::uint64_t WriteOwnStatRva     = 0x2F7D10;
constexpr std::uint64_t LocalPlayerIndexRva = 0x08B2D0;
constexpr std::uint64_t PlayerFromIndexRva  = 0x09A480;

// The lea at 14EE1C8 is 7 bytes long and its displacement is 00AEF769.
constexpr std::uint64_t StatButtonVtableRva = 0x1FDD938;
static_assert(StatButtonVtableLea + 7 + 0x00AEF769 == StatButtonVtableRva);

// Inside the widget: the tooltip string's text pointer and length.
constexpr std::size_t TooltipTextOffset = 0x88;
constexpr std::size_t TooltipSizeOffset = 0x90;

// Opcode 39h: opcode byte, then the NPC unit id. -1 is the shortcut.
constexpr std::int32_t  RespecPacketSize = 5;
constexpr std::uint32_t ShortcutNpcId    = 0xFFFFFFFFU;

// Compiled itemstatcost row: flags dword at +4 (Saved bit 11, CSvSigned bit
// 12), CSvBits byte at +0Ah.
constexpr std::size_t   IscFlagsOffset    = 0x04;
constexpr std::uint32_t IscFlagSaved      = 0x800;
constexpr std::uint32_t IscFlagCsvSigned  = 0x1000;
constexpr std::size_t   IscCsvBitsOffset  = 0x0A;

// Longest tooltip text the hook formats; anything longer is left alone.
constexpr std::size_t MaxTooltipBytes = 1024;

// ---------------------------------------------------------------------------
// Expected bytes
// ---------------------------------------------------------------------------

// Loader thunks: jmp [rip+slot], then four nops. The slot offset changes with
// the loader version, so only the fixed parts are checked.
constexpr std::uint8_t ThunkHead[]{ 0xFF,0x25 };
constexpr std::uint8_t ThunkTail[]{ 0x90,0x90,0x90,0x90 };

// 4B2530 prologue, the bytes the hook displaces: push rbp / push rbx /
// push rsi / push rdi / push r14 / lea rbp, [rsp-37h] / sub rsp, 0C0h.
// Nothing relative; the next instruction is the rip-relative cookie load.
constexpr std::uint8_t RespecCommandPrologue[]{
    0x40,0x55,0x53,0x56,0x57,0x41,0x56,0x48,0x8D,0x6C,0x24,0xC9,0x48,0x81,0xEC,0xC0,
    0x00,0x00,0x00 };
// 4B259F: mov eax, [rbp-28h] (the dword after the opcode) / xor bl, bl /
// cmp eax, -1 / jne npc path / test dword [rdi+5658h], 800000h.
constexpr std::uint8_t ShortcutTestBytes[]{
    0x8B,0x45,0xD8,0x32,0xDB,0x83,0xF8,0xFF,0x75,0x10,0xF7,0x87,0x58,0x56,0x00,0x00,
    0x00,0x00,0x80,0x00 };
// 52D770 prologue: mov [rsp+8], rbx / mov [rsp+10h], rbp / push rsi /
// push rdi / push r14 / sub rsp, 20h.
constexpr std::uint8_t StartingStatsPrologue[]{
    0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x56,0x57,0x41,0x56,0x48,0x83,
    0xEC,0x20 };
// 875980 prologue: push rdi / sub rsp, 40h / mov rdi, rcx. The next
// instruction is a rel32 call, so the displaced bytes stop here.
constexpr std::uint8_t WidgetTooltipPrologue[]{
    0x40,0x57,0x48,0x83,0xEC,0x40,0x48,0x8B,0xF9 };
// 875A44: mov rcx, [rdi+88h], the first argument of the tooltip dispatch.
constexpr std::uint8_t TooltipTextLoadBytes[]{ 0x48,0x8B,0x8F,0x88,0x00,0x00,0x00 };
// 14EE19A: mov edx, 0B90h, the allocation size of the stat button.
constexpr std::uint8_t StatButtonSizeBytes[]{ 0xBA,0x90,0x0B,0x00,0x00 };
// 14EE1C8: lea rax, [1FDD938] / mov dword [rbx+0B88h], 0 / mov [rbx], rax.
constexpr std::uint8_t StatButtonVtableBytes[]{
    0x48,0x8D,0x05,0x69,0xF7,0xAE,0x00,0xC7,0x83,0x88,0x0B,0x00,0x00,0x00,0x00,0x00,
    0x00,0x48,0x89,0x03 };
// 8B2D0: mov eax, [local player index]; ret.
constexpr std::uint8_t LocalPlayerIndexBytes[]{ 0x8B,0x05,0x2E,0x84,0x99,0x02,0xC3 };
// 9A480 (index): an index of 8 or more gives no player.
constexpr std::uint8_t PlayerFromIndexBytes[]{
    0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x83,0xF9,0x08 };

struct Witness {
    std::uint64_t       rva;
    const std::uint8_t* bytes;
    std::uint32_t       size;
    const char*         name;
};

template <std::size_t N>
constexpr auto W(std::uint64_t rva, const std::uint8_t (&bytes)[N], const char* name) noexcept -> Witness {
    return { rva, bytes, static_cast<std::uint32_t>(N), name };
}

const std::array<Witness, 13> Witnesses{
    W(RespecCommandRva, RespecCommandPrologue, "respec command callback"),
    W(ShortcutTestRva, ShortcutTestBytes, "respec shortcut test"),
    W(StartingStatsRva, StartingStatsPrologue, "new character starting stats"),
    W(WidgetTooltipRva, WidgetTooltipPrologue, "widget tooltip draw"),
    W(TooltipTextLoadRva, TooltipTextLoadBytes, "widget tooltip text"),
    W(StatButtonSizeRva, StatButtonSizeBytes, "stat button size"),
    W(StatButtonVtableLea, StatButtonVtableBytes, "stat button vtable"),
    W(ReadOwnStatRva, ThunkHead, "own stat read"),
    W(ReadOwnStatRva + 6, ThunkTail, "own stat read"),
    W(WriteOwnStatRva, ThunkHead, "own stat write"),
    W(WriteOwnStatRva + 6, ThunkTail, "own stat write"),
    W(LocalPlayerIndexRva, LocalPlayerIndexBytes, "local player index"),
    W(PlayerFromIndexRva, PlayerFromIndexBytes, "local player"),
};

// ---------------------------------------------------------------------------
// Native functions
// ---------------------------------------------------------------------------

using ReadStatFn      = std::int32_t(__fastcall*)(void* unit, std::int32_t stat, std::int32_t layer);
using WriteStatFn     = void(__fastcall*)(void* unit, std::int32_t stat, std::int32_t value, std::int32_t layer);
using PlayerIndexFn   = std::int32_t(__fastcall*)();
using PlayerFromFn    = void*(__fastcall*)(std::int32_t index);

using RespecCommandFn = std::int64_t(__fastcall*)(void* game, void* player, const std::uint8_t* packet, std::int32_t size) noexcept;
using StartingStatsFn = std::int64_t(__fastcall*)(void* game, void* player, std::uint8_t act) noexcept;
using WidgetTooltipFn = std::int64_t(__fastcall*)(void* widget) noexcept;

ReadStatFn    ReadOwnStat{};
WriteStatFn   WriteOwnStat{};
PlayerIndexFn LocalPlayerIndex{};
PlayerFromFn  PlayerFromIndex{};

RespecCommandFn OriginalRespecCommand{};
StartingStatsFn OriginalStartingStats{};
WidgetTooltipFn OriginalWidgetTooltip{};

std::uintptr_t StatButtonVtable{};

template <typename T>
auto At(std::uint64_t rva) noexcept -> T {
    return reinterpret_cast<T>(Base + rva);
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
// Services and state
// ---------------------------------------------------------------------------

const D2RL::LifecycleService* LifecycleSvc{};
const D2RL::DataTableService* TableSvc{};

enum class PluginState { NotLoaded, DisabledByConfig, UnsupportedBuild, InstallFailed, Active };
PluginState       State{ PluginState::NotLoaded };
std::atomic<bool> Active{};
std::atomic<bool> TooltipActive{};

// Counters, shown by the console command.
std::atomic<std::uint64_t> ShortcutRespecs{};
std::atomic<std::uint64_t> ShortcutRefused{};
std::atomic<std::uint64_t> ShortcutFailed{};
std::atomic<std::uint64_t> AkaraPassed{};
std::atomic<std::uint64_t> CharactersSeeded{};

// What the client saw on the stat button, for the console command.
enum : std::uint32_t { TooltipNotSeen = 0, TooltipWithMarker = 1, TooltipWithoutMarker = 2 };
std::atomic<std::uint32_t> TooltipSeen{ TooltipNotSeen };

// The last change the host made, for the console command.
std::atomic<std::int32_t> LastBefore{ -1 };
std::atomic<std::int32_t> LastAfter{ -1 };

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

constexpr std::size_t MaximumConfigBytes = 16'384;

// The default config, identical to celestialrayone.respec-counter.toml.
constexpr char DefaultConfigToml[] =
    "# Respec Counter\n"
    "#\n"
    "# Puts a counter, saved on the character, on D2RLoader's built-in respec\n"
    "# shortcut, and shows the count in the stat button's tooltip.\n"
    "#\n"
    "# The shortcut\n"
    "#   [d2rcore.player] enable_respec = true turns on the game's own respec\n"
    "#   shortcut: Alt + click a stat button (the + next to Strength, Dexterity,\n"
    "#   Vitality or Energy). It resets every stat and skill point.\n"
    "#   - Each use costs 1 from the counter.\n"
    "#   - At 0 the shortcut does nothing.\n"
    "#   - Akara's quest respec is not counted and not blocked.\n"
    "#\n"
    "# The counter\n"
    "#   The character's own value of the itemstatcost.txt row stat_id.\n"
    "#   - A new character starts with starting_respecs.\n"
    "#   - The plugin only spends it. Anything else may add to it, for example a\n"
    "#     misc.txt pSpell that adds the stat (the Token of Absolution).\n"
    "#   - Characters made before this plugin was installed start at 0.\n"
    "#\n"
    "# itemstatcost.txt row for stat_id\n"
    "#   - CSvBits above 0. Required: D2RLoader saves a character's own stats\n"
    "#     only when CSvBits is above 0. 8 holds 0 to 255.\n"
    "#   - Saved = 1. Required: the game sends a change made on the character to\n"
    "#     that player's client only for rows with Saved = 1, and the tooltip\n"
    "#     reads the client's copy.\n"
    "#   - CSvSigned 0, no op, ValShift blank: the values are plain counts.\n"
    "#   The plugin log reports this check every time the tables load.\n"
    "#\n"
    "# The tooltip\n"
    "#   The stat button's tooltip is the string TooltipCharacterScreenSpendStatPoint.\n"
    "#   The plugin replaces the first %d in it with the counter, for example:\n"
    "#     \"Spend Stat Point\\nRespecs Left: %d\"\n"
    "#   \\n starts the second row. A language whose text has no %d shows as is.\n"
    "#   With the plugin off, a %d in that string shows as %d.\n"
    "#\n"
    "# Multiplayer\n"
    "#   The host spends the counter and gives new characters their starting\n"
    "#   value, so the host needs this plugin. Each client needs it for its own\n"
    "#   tooltip.\n"
    "#\n"
    "# Console command\n"
    "#   respeccounter   status, the itemstatcost check, counters and your count.\n"
    "\n"
    "[respec_counter]\n"
    "\n"
    "# Master switch.\n"
    "enabled = true\n"
    "\n"
    "# itemstatcost.txt row of the counter.\n"
    "stat_id = 237\n"
    "\n"
    "# Respecs a new character starts with.\n"
    "starting_respecs = 5\n";

struct Config {
    bool         enabled{ true };
    std::int32_t statId{ 237 };
    std::int32_t startingRespecs{ 5 };
};

Config      Settings;
std::string ConfigProblems;

void Problem(std::string_view text) {
    if (!ConfigProblems.empty()) ConfigProblems += "; ";
    ConfigProblems.append(text);
}

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

// Drops a trailing # comment that is not inside a quoted string.
auto StripComment(std::string_view line) noexcept -> std::string_view {
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '"') quoted = !quoted;
        if (line[i] == '#' && !quoted) return line.substr(0, i);
    }
    return line;
}

auto ParseBool(std::string_view value, bool& out) noexcept -> bool {
    if (value == "true") { out = true; return true; }
    if (value == "false") { out = false; return true; }
    return false;
}

auto ParseInt(std::string_view value, std::int32_t minimum, std::int32_t maximum, std::int32_t& out) noexcept -> bool {
    if (value.empty()) return false;
    std::int64_t result   = 0;
    std::size_t  i        = 0;
    bool         negative = false;
    if (value[0] == '-' || value[0] == '+') {
        negative = value[0] == '-';
        i = 1;
    }
    if (i >= value.size()) return false;
    for (; i < value.size(); ++i) {
        if (value[i] == '_') continue;
        if (value[i] < '0' || value[i] > '9') return false;
        result = result * 10 + (value[i] - '0');
        if (result > 1'000'000'000) return false;
    }
    if (negative) result = -result;
    if (result < minimum || result > maximum) return false;
    out = static_cast<std::int32_t>(result);
    return true;
}

void ApplyConfigLine(std::string_view section, std::string_view key, std::string_view value) {
    if (section != "respec_counter") return;
    char note[160];
    const auto bad = [&](const char* what) {
        std::snprintf(note, sizeof(note), "%.*s: %s, default kept", static_cast<int>(key.size()), key.data(), what);
        Problem(note);
    };
    if (key == "enabled") {
        if (!ParseBool(value, Settings.enabled)) bad("expected true or false");
    } else if (key == "stat_id") {
        if (!ParseInt(value, 0, 32767, Settings.statId)) bad("expected 0 to 32767");
    } else if (key == "starting_respecs") {
        if (!ParseInt(value, 0, 65535, Settings.startingRespecs)) bad("expected 0 to 65535");
    }
}

void ParseConfig(std::string_view text) {
    std::string_view section;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        const std::string_view line = Trim(StripComment(text.substr(0, end)));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
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

void ReadConfiguration() {
    if (!Context->EnsureConfig(DefaultConfigToml)) {
        Context->LogWarn("Respec Counter: the config file could not be created; using defaults.");
    } else {
        std::string buffer(MaximumConfigBytes, '\0');
        if (Context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size()), nullptr)) {
            buffer.resize(std::strlen(buffer.c_str()));
            ParseConfig(buffer);
        } else {
            Context->LogWarn("Respec Counter: the config file could not be read; using defaults.");
        }
    }
    if (!ConfigProblems.empty()) {
        D2RL::LogWarnF(Context, "Respec Counter: %s", ConfigProblems.c_str());
    }
}

// ---------------------------------------------------------------------------
// The counter on a unit
// ---------------------------------------------------------------------------

auto ReadCounter(void* unit) noexcept -> std::int32_t {
    return ReadOwnStat(unit, Settings.statId, 0);
}

void WriteCounter(void* unit, std::int32_t value) noexcept {
    WriteOwnStat(unit, Settings.statId, value, 0);
}

auto LocalPlayer() noexcept -> void* {
    return PlayerFromIndex(LocalPlayerIndex());
}

// At most 20 refusal lines per 10 seconds, in case someone holds Alt and
// keeps clicking at 0.
std::atomic<std::uint64_t> RefusalLogWindow{};
std::atomic<std::uint32_t> RefusalLogLines{};

auto RefusalLogAllowed() noexcept -> bool {
    const std::uint64_t window = GetTickCount64() / 10'000;
    if (RefusalLogWindow.exchange(window) != window) RefusalLogLines.store(0);
    return RefusalLogLines.fetch_add(1) < 20;
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

// Opcode 39h on the host. The shortcut (NPC id -1) is counted, everything else
// goes straight to the game.
auto __fastcall HookRespecCommand(void* game, void* player, const std::uint8_t* packet, std::int32_t size) noexcept
        -> std::int64_t {
    const RespecCommandFn original = OriginalRespecCommand;
    if (!Active.load(std::memory_order_relaxed) || player == nullptr || packet == nullptr
            || size != RespecPacketSize) {
        return original(game, player, packet, size);
    }
    std::uint32_t npcId = 0;
    std::memcpy(&npcId, packet + 1, sizeof(npcId));
    if (npcId != ShortcutNpcId) {
        AkaraPassed.fetch_add(1);
        return original(game, player, packet, size);
    }

    const std::int32_t left = ReadCounter(player);
    if (left <= 0) {
        ShortcutRefused.fetch_add(1);
        if (RefusalLogAllowed()) {
            D2RL::LogInfoF(Context, "Respec Counter: shortcut respec refused, the character has %d respecs left.",
                left);
        }
        return 0;
    }

    const std::int64_t result = original(game, player, packet, size);
    if (result != 0) {
        // No respec happened (enable_respec off: the game failed the NPC
        // lookup for -1). Nothing is spent.
        ShortcutFailed.fetch_add(1);
        return result;
    }
    WriteCounter(player, left - 1);
    ShortcutRespecs.fetch_add(1);
    LastBefore.store(left);
    LastAfter.store(ReadCounter(player));
    D2RL::LogInfoF(Context, "Respec Counter: shortcut respec used, respecs left %d -> %d.", left,
        LastAfter.load());
    return result;
}

// A brand-new character's starting stats on the host; the counter is added
// right after them.
auto __fastcall HookStartingStats(void* game, void* player, std::uint8_t act) noexcept -> std::int64_t {
    const std::int64_t result = OriginalStartingStats(game, player, act);
    if (Active.load(std::memory_order_relaxed) && player != nullptr) {
        WriteCounter(player, Settings.startingRespecs);
        CharactersSeeded.fetch_add(1);
        LastBefore.store(0);
        LastAfter.store(ReadCounter(player));
        D2RL::LogInfoF(Context, "Respec Counter: new character, %d respecs.", LastAfter.load());
    }
    return result;
}

// Hovered widget tooltip on the client. Only the stat button is touched.
auto __fastcall HookWidgetTooltip(void* widget) noexcept -> std::int64_t {
    const WidgetTooltipFn original = OriginalWidgetTooltip;
    if (!TooltipActive.load(std::memory_order_relaxed) || widget == nullptr
            || Read<std::uintptr_t>(widget, 0) != StatButtonVtable) {
        return original(widget);
    }
    char* const         text = Read<char*>(widget, TooltipTextOffset);
    const std::uint64_t size = Read<std::uint64_t>(widget, TooltipSizeOffset);
    if (text == nullptr || size == 0 || size >= MaxTooltipBytes) return original(widget);

    const std::string_view view(text, static_cast<std::size_t>(size));
    const std::size_t      marker = view.find("%d");
    if (marker == std::string_view::npos) {
        TooltipSeen.store(TooltipWithoutMarker, std::memory_order_relaxed);
        return original(widget);
    }
    TooltipSeen.store(TooltipWithMarker, std::memory_order_relaxed);

    void* const player = LocalPlayer();
    if (player == nullptr) return original(widget);
    const std::int32_t left = ReadCounter(player);

    // The copy lives on this stack frame for the whole call; the dispatcher
    // copies the text before it returns.
    char      formatted[MaxTooltipBytes + 16];
    const int written = std::snprintf(formatted, sizeof(formatted), "%.*s%d%.*s", static_cast<int>(marker), text,
        left, static_cast<int>(view.size() - marker - 2), text + marker + 2);
    if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(formatted)) return original(widget);

    Write<char*>(widget, TooltipTextOffset, formatted);
    Write<std::uint64_t>(widget, TooltipSizeOffset, static_cast<std::uint64_t>(written));
    const std::int64_t result = original(widget);
    Write<char*>(widget, TooltipTextOffset, text);
    Write<std::uint64_t>(widget, TooltipSizeOffset, size);
    return result;
}

// ---------------------------------------------------------------------------
// itemstatcost check, every time the tables load (game thread)
// ---------------------------------------------------------------------------

enum : std::uint32_t { RowNotChecked = 0, RowMissing = 1, RowFound = 2, RowUnreadable = 3 };

constexpr std::array<D2RL::DataTables::Bank, 3> Banks{
    D2RL::DataTables::Bank::Classic, D2RL::DataTables::Bank::Lod, D2RL::DataTables::Bank::Rotw };
constexpr std::array<const char*, 3> BankNames{ "classic", "lod", "rotw" };

struct RowCheck {
    std::uint32_t state;
    std::uint32_t saved;
    std::uint32_t csvSigned;
    std::uint32_t csvBits;
};

// Per bank: state | Saved << 8 | CSvSigned << 9 | CSvBits << 16.
std::array<std::atomic<std::uint32_t>, 3> RowStates{};

auto Pack(const RowCheck& check) noexcept -> std::uint32_t {
    return check.state | (check.saved << 8) | (check.csvSigned << 9) | (check.csvBits << 16);
}

auto Unpack(std::uint32_t value) noexcept -> RowCheck {
    return { value & 0xFFU, (value >> 8) & 1U, (value >> 9) & 1U, (value >> 16) & 0xFFU };
}

auto CheckRow(const D2RL::PluginContext* context, D2RL::DataTables::Bank bank) noexcept -> RowCheck {
    D2RL::DataTables::RowView row{};
    row.structSize = D2RL::DataTables::RowViewSize;
    const D2RL::DataTables::Result result = TableSvc->getRow(context, bank, D2RL::DataTables::TableId::ItemStatCost,
        static_cast<std::uint32_t>(Settings.statId), &row);
    if (result == D2RL::DataTables::Result::NotFound || result == D2RL::DataTables::Result::InvalidArgument) {
        return { RowMissing, 0, 0, 0 };
    }
    if (result != D2RL::DataTables::Result::Success
            || !D2RL::DataTables::HasRowViewField(&row, D2RL::DataTables::RowViewRequiredSize)
            || row.row == nullptr || row.rowSize <= IscCsvBitsOffset) {
        return { RowUnreadable, 0, 0, 0 };
    }
    const std::uint32_t flags = Read<std::uint32_t>(row.row, IscFlagsOffset);
    return { RowFound, (flags & IscFlagSaved) != 0 ? 1U : 0U, (flags & IscFlagCsvSigned) != 0 ? 1U : 0U,
        Read<std::uint8_t>(row.row, IscCsvBitsOffset) };
}

void DescribeRows(char* out, std::size_t size) noexcept {
    out[0] = '\0';
    std::size_t used = 0;
    for (std::size_t i = 0; i < Banks.size() && used < size; ++i) {
        const RowCheck check = Unpack(RowStates[i].load());
        const char*    comma = i == 0 ? "" : ", ";
        int written = 0;
        switch (check.state) {
        case RowMissing:
            written = std::snprintf(out + used, size - used, "%s%s no row", comma, BankNames[i]);
            break;
        case RowFound:
            written = std::snprintf(out + used, size - used, "%s%s Saved %u CSvBits %u CSvSigned %u", comma,
                BankNames[i], check.saved, check.csvBits, check.csvSigned);
            break;
        case RowUnreadable:
            written = std::snprintf(out + used, size - used, "%s%s unreadable", comma, BankNames[i]);
            break;
        default:
            written = std::snprintf(out + used, size - used, "%s%s not checked yet", comma, BankNames[i]);
            break;
        }
        if (written < 0) break;
        used += static_cast<std::size_t>(written);
    }
}

void __cdecl OnDataTablesLoaded(const D2RL::PluginContext* context, const D2RL::Lifecycle::DataTablesLoadedEvent*,
        void*) noexcept {
    if (context == nullptr || TableSvc == nullptr) return;
    bool anyRow     = false;
    bool anyUnsaved = false;
    bool anyNoBits  = false;
    bool anyTooBig  = false;
    for (std::size_t i = 0; i < Banks.size(); ++i) {
        const RowCheck check = CheckRow(context, Banks[i]);
        RowStates[i].store(Pack(check));
        if (check.state != RowFound) continue;
        anyRow     = true;
        anyUnsaved = anyUnsaved || check.saved == 0;
        anyNoBits  = anyNoBits || check.csvBits == 0;
        if (check.csvBits > 0 && check.csvBits < 31) {
            const std::int64_t largest = (std::int64_t{ 1 } << (check.csvBits - check.csvSigned)) - 1;
            anyTooBig = anyTooBig || Settings.startingRespecs > largest;
        }
    }
    char rows[256];
    DescribeRows(rows, sizeof(rows));
    if (!anyRow) {
        D2RL::LogErrorF(context, "Respec Counter: itemstatcost.txt has no row %d (%s). The counter cannot work "
            "until the row exists.", Settings.statId, rows);
        return;
    }
    if (anyNoBits) {
        D2RL::LogErrorF(context, "Respec Counter: itemstatcost.txt row %d has CSvBits 0 (%s). The counter is not "
            "saved, so every character comes back with 0 respecs.", Settings.statId, rows);
    }
    if (anyUnsaved) {
        D2RL::LogWarnF(context, "Respec Counter: itemstatcost.txt row %d has Saved = 0 (%s). The host spends the "
            "counter, but the client is not told, so the tooltip shows a stale count.", Settings.statId, rows);
    }
    if (anyTooBig) {
        D2RL::LogWarnF(context, "Respec Counter: starting_respecs %d does not fit row %d's CSvBits (%s); the save "
            "cuts it down.", Settings.startingRespecs, Settings.statId, rows);
    }
    if (!anyNoBits && !anyUnsaved && !anyTooBig) {
        D2RL::LogInfoF(context, "Respec Counter: itemstatcost.txt row %d checked (%s).", Settings.statId, rows);
    }
}

// ---------------------------------------------------------------------------
// Console
// ---------------------------------------------------------------------------

auto StateName(PluginState state) noexcept -> const char* {
    switch (state) {
    case PluginState::NotLoaded:        return "not loaded";
    case PluginState::DisabledByConfig: return "disabled by config";
    case PluginState::UnsupportedBuild: return "unsupported build, nothing hooked";
    case PluginState::InstallFailed:    return "a hook failed to install, counting is off";
    case PluginState::Active:           return "active";
    }
    return "?";
}

auto Count(const std::atomic<std::uint64_t>& counter) noexcept -> unsigned long long {
    return static_cast<unsigned long long>(counter.load());
}

auto __cdecl StatusCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept
        -> D2RL::ConsoleCommandResult {
    if (command == nullptr || command->plugin == nullptr) return D2RL::ConsoleCommandResult::Failed;
    char       line[400];
    const auto say = [&](const char* text) { command->plugin->WriteConsoleMessage(text); };

    std::snprintf(line, sizeof(line), "Respec Counter %s: %s. Stat %d, new characters start with %d.",
        PluginVersion, StateName(State), Settings.statId, Settings.startingRespecs);
    say(line);
    if (State != PluginState::Active && State != PluginState::InstallFailed) return D2RL::ConsoleCommandResult::Handled;

    char rows[256];
    DescribeRows(rows, sizeof(rows));
    std::snprintf(line, sizeof(line), "  itemstatcost row %d: %s.", Settings.statId, rows);
    say(line);
    const char* tooltip = "the stat button has not been drawn yet";
    switch (TooltipSeen.load()) {
    case TooltipWithMarker:    tooltip = "the stat button's text has %d, the count is filled in"; break;
    case TooltipWithoutMarker: tooltip = "the stat button's text has no %d, add one to TooltipCharacterScreenSpendStatPoint"; break;
    default: break;
    }
    std::snprintf(line, sizeof(line), "  tooltip: %s; %s.", TooltipActive.load() ? "hooked" : "not hooked", tooltip);
    say(line);
    std::snprintf(line, sizeof(line), "  host: %llu shortcut respecs, %llu refused at 0, %llu not done by the game, "
        "%llu Akara respecs passed through, %llu new characters given their respecs.", Count(ShortcutRespecs),
        Count(ShortcutRefused), Count(ShortcutFailed), Count(AkaraPassed), Count(CharactersSeeded));
    say(line);
    if (LastAfter.load() >= 0) {
        std::snprintf(line, sizeof(line), "  host, last change: %d -> %d.", LastBefore.load(), LastAfter.load());
        say(line);
    }
    if (void* const local = LocalPlayer(); local != nullptr) {
        std::snprintf(line, sizeof(line), "  your client sees %d respecs left.", ReadCounter(local));
        say(line);
    }
    return D2RL::ConsoleCommandResult::Handled;
}

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------

auto VerifyAll() noexcept -> bool {
    bool ok = true;
    for (const Witness& witness : Witnesses) {
        if (!Context->CheckExpectedBytes(witness.rva, witness.bytes, witness.size)) {
            D2RL::LogErrorF(Context, "Respec Counter: %s at RVA 0x%llX does not match this build.", witness.name,
                static_cast<unsigned long long>(witness.rva));
            ok = false;
        }
    }
    return ok;
}

void BindNatives() noexcept {
    ReadOwnStat      = At<ReadStatFn>(ReadOwnStatRva);
    WriteOwnStat     = At<WriteStatFn>(WriteOwnStatRva);
    LocalPlayerIndex = At<PlayerIndexFn>(LocalPlayerIndexRva);
    PlayerFromIndex  = At<PlayerFromFn>(PlayerFromIndexRva);
    StatButtonVtable = Base + StatButtonVtableRva;
}

void QueryServices() noexcept {
    if (Context->QueryService(&LifecycleSvc) != D2RL::ServiceQueryResult::Success
            || !D2RL::HasLifecycleServiceField(LifecycleSvc, D2RL::LifecycleServiceRequiredSize)) {
        LifecycleSvc = nullptr;
    }
    if (Context->QueryService(&TableSvc) != D2RL::ServiceQueryResult::Success
            || !D2RL::HasDataTableServiceField(TableSvc, D2RL::DataTableServiceRequiredSize)) {
        TableSvc = nullptr;
    }
    if (LifecycleSvc == nullptr || TableSvc == nullptr) {
        Context->LogWarn("Respec Counter: the lifecycle or data table service is unavailable; the itemstatcost "
            "check is off.");
        return;
    }
    const D2RL::Lifecycle::DataTablesLoadedListener tables{
        .structSize = D2RL::Lifecycle::DataTablesLoadedListenerSize,
        .callback   = &OnDataTablesLoaded,
    };
    D2RL::Lifecycle::ListenerHandle handle = D2RL::Lifecycle::InvalidHandle;
    if (LifecycleSvc->registerDataTablesLoadedListener(Context, &tables, &handle)
            != D2RL::Lifecycle::Result::Success) {
        Context->LogWarn("Respec Counter: the table listener was refused; the itemstatcost check is off.");
    }
}

// The two host hooks go in together: a gate without the starting value would
// lock every new character out, a starting value without the gate would be
// meaningless. The tooltip hook is separate.
auto InstallHooks() noexcept -> bool {
    if (!Context->InstallInlineHook(RespecCommandRva, RespecCommandPrologue,
            static_cast<std::uint32_t>(sizeof(RespecCommandPrologue)), &HookRespecCommand,
            &OriginalRespecCommand)) {
        Context->LogError("Respec Counter: the respec command hook at 0x4B2530 failed to install.");
        return false;
    }
    if (!Context->InstallInlineHook(StartingStatsRva, StartingStatsPrologue,
            static_cast<std::uint32_t>(sizeof(StartingStatsPrologue)), &HookStartingStats,
            &OriginalStartingStats)) {
        Context->LogError("Respec Counter: the new character hook at 0x52D770 failed to install.");
        return false;
    }
    return true;
}

auto InstallTooltipHook() noexcept -> bool {
    if (!Context->InstallInlineHook(WidgetTooltipRva, WidgetTooltipPrologue,
            static_cast<std::uint32_t>(sizeof(WidgetTooltipPrologue)), &HookWidgetTooltip,
            &OriginalWidgetTooltip)) {
        Context->LogError("Respec Counter: the tooltip hook at 0x875980 failed to install; the tooltip shows "
            "the string as written.");
        return false;
    }
    return true;
}

constexpr D2RL::PluginInfo PluginInfoData{
    .infoSize    = D2RL::PluginInfoSize,
    .abiVersion  = D2RL_PLUGIN_ABI_VERSION,
    .id          = PluginIdText,
    .name        = "Respec Counter",
    .version     = PluginVersion,
    .author      = "CelestialRayOne",
    .description = "Saved respec counter for D2RLoader's Alt + click respec shortcut, shown in the stat button "
                   "tooltip.",
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

    if (!Context->RegisterConsoleCommand("respeccounter", &StatusCommand,
            "Respec Counter status: the itemstatcost check, counters and your respecs left.")) {
        Context->LogWarn("Respec Counter: the console command could not be registered.");
    }

    ReadConfiguration();
    if (!Settings.enabled) {
        State = PluginState::DisabledByConfig;
        Context->LogInfo("Respec Counter: disabled by config.");
        return true;
    }
    if (!VerifyAll()) {
        State = PluginState::UnsupportedBuild;
        Context->LogError("Respec Counter: this D2R build does not match; nothing was hooked.");
        return true;
    }
    BindNatives();
    QueryServices();

    if (!InstallHooks()) {
        State = PluginState::InstallFailed;
        return true;
    }
    Active.store(true);
    TooltipActive.store(InstallTooltipHook());
    State = PluginState::Active;
    D2RL::LogInfoF(Context, "Respec Counter %s: active, stat %d, new characters start with %d respecs.",
        PluginVersion, Settings.statId, Settings.startingRespecs);
    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    Active.store(false);
    TooltipActive.store(false);
}
