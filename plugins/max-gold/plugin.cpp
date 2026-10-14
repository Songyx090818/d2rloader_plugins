// ---------------------------------------------------------------------------
// gold-capacities
//
// Raises how much gold a character can hold:
//
//                         vanilla              this plugin
//   carried gold          level x 10,000       level x gold_per_level
//   personal stash        2,500,000            stash_gold
//   each shared tab       2,500,000            stash_gold
//   shared stash total    tabs x 2,500,000     tabs x stash_gold
//
// Defaults are ten times vanilla.
//
// Target: D2R 3.3.93847 in the D2RLoader 1.3.1 process image (dump md5
// 8af7a47c...) and D2RLoader's D2RCore.dll 1.3.1 (sha256 2a868d01...). Every
// byte below was read from those two binaries, and every site is checked byte
// for byte before anything is written.
//
// ---------------------------------------------------------------------------
// Where the limits live
// ---------------------------------------------------------------------------
//
// Carried gold: UNITS_GetInventoryGoldLimit, RVA 0x34B320. All 55 call sites
// in the image go through it (auto gold pickup 0x193010, the gold dialog
// 0x14F5610 / 0x14F6190, the server's transaction checks, the load check
// 0x5311F0 described below).
//
//   0x34B320  sub  rsp, 28h
//   0x34B324  xor  r8d, r8d
//   0x34B327  lea  edx, [r8+0Ch]          ; stat 12, level
//   0x34B32B  call 0x2F5020               ; GetUnitStat
//   0x34B330  imul eax, eax, 2710h        ; 10,000, the imm32 at 0x34B332
//   0x34B336  add  rsp, 28h
//   0x34B33A  ret
//
// Personal stash: UNITS_GetStashGoldLimit, RVA 0x34B310, 21 call sites.
//
//   0x34B310  mov  eax, 2625A0h           ; 2,500,000, the imm32 at 0x34B311
//   0x34B315  ret
//
//   The game uses this same function as the limit of EACH shared stash tab.
//   The client's shared deposit (0x14F5330) fills one tab after another up to
//   it, and the server's shared stash loader (0x52FDB0, reached through
//   D2RCore!ReadSharedStashStreams) rejects a tab holding more (0x530012).
//
// Shared stash file check: RVA 0x133962, inside the client routine that
// writes the shared stash file (sub_140133610, reached from 0x131A70, which
// D2RCore!HandleServerControlPacket calls).
//
//   0x133957  cmp  eax, 7FFFFFFFh
//   0x13395C  ja   1339F8
//   0x133962  cmp  eax, 2625A0h           ; 2,500,000, the imm32 at 0x133963
//   0x133967  jg   1339F8                 ; tab over the limit: file not written
//
//   Without this one, raising the per-tab limit would stop the shared stash
//   file from being written as soon as any tab passed 2,500,000.
//
// Shared stash total: D2RCore.dll, export GetSharedStashGoldLimit (1.3.1:
// RVA 0x826200). D2RLoader replaced the game's own version (0x34B2F0, no
// callers left in the image, so it is left alone) with this one, and it
// carries its own copy of 2,500,000:
//
//   +0x12  call [slot]                    ; game 0x388C70, shared tab count
//   +0x19  imul ecx, eax, 2625A0h         ; per tab, the imm32 at +0x1B
//   +0x1F  cmp  eax, 35Bh                 ; 859 tabs, the imm32 at +0x20
//   +0x24  mov  eax, 7FFFFFFFh
//   +0x29  cmovb eax, ecx                 ; tabs < cutoff: tabs x per tab
//
//   859 is the first tab count whose total no longer fits a signed 32-bit
//   integer: INT32_MAX / 2,500,000 + 1. The plugin rewrites the per-tab value
//   and recomputes the cutoff for it the same way, so the total never wraps.
//   D2RLoader's patch API only reaches D2R.exe, which is why this part is
//   written directly, after the exact 50 bytes of the function are matched,
//   and restored on unload.
//
// ---------------------------------------------------------------------------
// Why this plugin refuses to load instead of staying loaded and inactive
// ---------------------------------------------------------------------------
//
// On every character load, 0x5311F0 (called at the end of the loader
// 0x52E910) does:
//
//   if (gold     < 0 || gold     > UNITS_GetInventoryGoldLimit(player)) gold     = 0;
//   if (goldbank < 0 || goldbank > UNITS_GetStashGoldLimit(player))     goldbank = 0;
//
// The gold is emptied, not clamped. A character saved with raised limits and
// loaded without them loses that gold. This plugin is recorded in each
// character's save environment (Shared role), and D2RLoader warns before
// loading a character whose saved plugins are missing. So whenever the game
// side cannot be applied, or the config turns the plugin off, it declines to
// load: the user gets that warning instead of a silent wipe.
//
// The D2RCore part is different: if it cannot be applied, the shared stash
// total simply stays at tabs x 2,500,000. Nothing can be lost that way, so
// the plugin stays loaded and reports it.
// ---------------------------------------------------------------------------

#include <D2RLPlugin/api.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace {

// ---------------------------------------------------------------------------
// Native contract: D2R.exe
// ---------------------------------------------------------------------------

constexpr std::int32_t VanillaGoldPerLevel = 10'000;
constexpr std::int32_t VanillaStashGold    = 2'500'000;

constexpr auto ReadLe32(const std::uint8_t* bytes) noexcept -> std::uint32_t {
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U)
        | (static_cast<std::uint32_t>(bytes[2]) << 16U) | (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

void WriteLe32(std::uint8_t* bytes, std::uint32_t value) noexcept {
    bytes[0] = static_cast<std::uint8_t>(value);
    bytes[1] = static_cast<std::uint8_t>(value >> 8U);
    bytes[2] = static_cast<std::uint8_t>(value >> 16U);
    bytes[3] = static_cast<std::uint8_t>(value >> 24U);
}

// UNITS_GetInventoryGoldLimit, the whole body.
constexpr std::uint64_t CarriedGoldRva = 0x34B320;
constexpr std::uint8_t  CarriedGoldBody[] {
    0x48, 0x83, 0xEC, 0x28,                   // sub  rsp, 28h
    0x45, 0x33, 0xC0,                         // xor  r8d, r8d
    0x41, 0x8D, 0x50, 0x0C,                   // lea  edx, [r8+0Ch]       ; stat 12, level
    0xE8, 0xF0, 0x9C, 0xFA, 0xFF,             // call 2F5020              ; GetUnitStat
    0x69, 0xC0, 0x10, 0x27, 0x00, 0x00,       // imul eax, eax, 2710h     ; 10,000
    0x48, 0x83, 0xC4, 0x28,                   // add  rsp, 28h
    0xC3,                                     // ret
};
constexpr std::uint32_t CarriedGoldValueOffset = 0x12;

// UNITS_GetStashGoldLimit, the whole body.
constexpr std::uint64_t StashGoldRva = 0x34B310;
constexpr std::uint8_t  StashGoldBody[] {
    0xB8, 0xA0, 0x25, 0x26, 0x00,             // mov  eax, 2625A0h        ; 2,500,000
    0xC3,                                     // ret
};
constexpr std::uint32_t StashGoldValueOffset = 0x01;

// The per-tab check in the client routine that writes the shared stash file.
constexpr std::uint64_t FileCheckRva = 0x13394F;
constexpr std::uint8_t  FileCheckWindow[] {
    0x48, 0x8B, 0x45, 0xC8,                   // mov  rax, [rbp-38h]      ; tab header qword at +8
    0x48, 0xC1, 0xE8, 0x20,                   // shr  rax, 20h            ; tab gold, header +0Ch
    0x3D, 0xFF, 0xFF, 0xFF, 0x7F,             // cmp  eax, 7FFFFFFFh
    0x0F, 0x87, 0x96, 0x00, 0x00, 0x00,       // ja   1339F8
    0x3D, 0xA0, 0x25, 0x26, 0x00,             // cmp  eax, 2625A0h        ; 2,500,000
    0x0F, 0x8F, 0x8B, 0x00, 0x00, 0x00,       // jg   1339F8              ; file not written
};
constexpr std::uint32_t FileCheckValueOffset = 0x14;

static_assert(sizeof(CarriedGoldBody) == 27, "Verified body length changed.");
static_assert(sizeof(StashGoldBody) == 6, "Verified body length changed.");
static_assert(sizeof(FileCheckWindow) == 30, "Verified window length changed.");
static_assert(ReadLe32(CarriedGoldBody + CarriedGoldValueOffset) == VanillaGoldPerLevel, "Offset must land on 10,000.");
static_assert(ReadLe32(StashGoldBody + StashGoldValueOffset) == VanillaStashGold, "Offset must land on 2,500,000.");
static_assert(ReadLe32(FileCheckWindow + FileCheckValueOffset) == VanillaStashGold, "Offset must land on 2,500,000.");

struct GameSite {
    const char*         label;
    std::uint64_t       windowRva;
    const std::uint8_t* window;
    std::uint32_t       windowSize;
    std::uint32_t       valueOffset;
};

constexpr GameSite GameSites[] {
    { "carried gold limit", CarriedGoldRva, CarriedGoldBody, sizeof(CarriedGoldBody), CarriedGoldValueOffset },
    { "stash and shared tab limit", StashGoldRva, StashGoldBody, sizeof(StashGoldBody), StashGoldValueOffset },
    { "shared stash file check", FileCheckRva, FileCheckWindow, sizeof(FileCheckWindow), FileCheckValueOffset },
};
constexpr std::size_t GameSiteCount = std::size(GameSites);

// ---------------------------------------------------------------------------
// Native contract: D2RCore.dll
// ---------------------------------------------------------------------------

constexpr wchar_t      CoreModuleName[]    = L"D2RCore.dll";
constexpr char         SharedLimitExport[] = "GetSharedStashGoldLimit";
constexpr std::uint8_t JmpRel32Opcode      = 0xE9;

// D2RCore!GetSharedStashGoldLimit, the whole body (.pdata: 50 bytes). The
// call is rip-relative to D2RCore's own slot, so these bytes do not depend on
// where the DLL is loaded.
constexpr std::uint8_t SharedLimitBody[] {
    0x55,                                     // push rbp
    0x48, 0x83, 0xEC, 0x30,                   // sub  rsp, 30h
    0x48, 0x8D, 0x6C, 0x24, 0x30,             // lea  rbp, [rsp+30h]
    0x48, 0xC7, 0x45, 0xF8, 0xFE, 0xFF, 0xFF, 0xFF, // mov qword ptr [rbp-8], -2
    0xFF, 0x15, 0x48, 0x86, 0xED, 0xFF,       // call [rip-1279B8h]       ; game 388C70, shared tab count
    0x90,                                     // nop
    0x69, 0xC8, 0xA0, 0x25, 0x26, 0x00,       // imul ecx, eax, 2625A0h   ; 2,500,000 per tab
    0x3D, 0x5B, 0x03, 0x00, 0x00,             // cmp  eax, 35Bh           ; 859 tabs
    0xB8, 0xFF, 0xFF, 0xFF, 0x7F,             // mov  eax, 7FFFFFFFh
    0x0F, 0x42, 0xC1,                         // cmovb eax, ecx
    0x48, 0x83, 0xC4, 0x30,                   // add  rsp, 30h
    0x5D,                                     // pop  rbp
    0xC3,                                     // ret
};
constexpr std::size_t SharedPerTabOffset = 0x1B;
constexpr std::size_t SharedCutoffOffset = 0x20;

// The first tab count whose total no longer fits a signed 32-bit integer.
constexpr auto SharedTabCutoff(std::int32_t perTab) noexcept -> std::uint32_t {
    return static_cast<std::uint32_t>(INT32_MAX / perTab) + 1U;
}

static_assert(sizeof(SharedLimitBody) == 50, "Verified body length changed.");
static_assert(ReadLe32(SharedLimitBody + SharedPerTabOffset) == VanillaStashGold, "Offset must land on 2,500,000.");
static_assert(ReadLe32(SharedLimitBody + SharedCutoffOffset) == 859, "Offset must land on the 859-tab cutoff.");
static_assert(SharedTabCutoff(VanillaStashGold) == 859, "The cutoff formula must reproduce D2RCore's own value.");
static_assert(SharedTabCutoff(25'000'000) == 86, "85 x 25,000,000 fits, 86 x does not.");

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

constexpr const char* ConfigSection = "gold-capacities";

constexpr std::int32_t DefaultGoldPerLevel  = 100'000;
constexpr std::int32_t DefaultStashGold     = 25'000'000;
constexpr std::int32_t HighestStorableLevel = 127;                            // vanilla level CSvBits: 7
constexpr std::int32_t MaximumGoldPerLevel  = INT32_MAX / HighestStorableLevel; // 16,909,320
constexpr std::int32_t MaximumStashGold     = INT32_MAX;

static_assert(MaximumGoldPerLevel == 16'909'320, "Level 127 x the maximum must fit a signed 32-bit integer.");

// Kept identical to the shipped celestialrayone.gold-capacities.toml. This is
// what EnsureConfig writes when the file does not exist yet.
constexpr const char* DefaultConfigToml =
R"toml(# Gold Capacities - raises how much gold a character can carry and store
#
#                         vanilla              this plugin
#   carried gold          level x 10,000       level x gold_per_level
#   personal stash        2,500,000            stash_gold
#   each shared tab       2,500,000            stash_gold
#   shared stash total    tabs x 2,500,000     tabs x stash_gold
#
# The game uses one number for the personal stash and for every shared stash
# tab, and D2RLoader builds the shared stash total from the same amount, so
# stash_gold sets all three. The plugin changes the limits themselves, so
# every check the game makes follows them: auto gold pickup, the gold dialog,
# deposits and withdrawals, the server's checks, the "Gold Max" text and the
# check made when the shared stash file is written.
#
# READ BEFORE LOWERING A VALUE OR REMOVING THE PLUGIN
#   When a character loads, the game sets carried gold and personal stash
#   gold to 0 if they are above the limit. It does not cut them down to the
#   limit, it empties them. So:
#   - never lower a value below what a character already holds;
#   - never load a character holding more than the vanilla limits without
#     this plugin running.
#   D2RLoader records this plugin in each character's save environment. When
#   the plugin is missing it warns that a saved plugin is missing before the
#   character loads: cancel at that warning. The plugin also declines to load
#   on purpose when it cannot apply its game changes (for example after a game
#   update) and when enabled = false, so that warning appears in those cases
#   too, instead of the game quietly emptying the gold.
#   A shared stash tab above the limit is not emptied: the character fails to
#   load until the limit is high enough again.
#
# HOW HIGH YOU CAN GO
#   Carried gold and personal stash gold are saved in the character file with
#   the CSvBits of the gold and goldbank rows in itemstatcost.txt. Vanilla is
#   25 bits: 33,554,431 at most. Anything above is cut down to that when the
#   character saves. Shared stash tabs are saved as full 32-bit numbers: up to
#   2,147,483,647 each, and the shared stash total is capped there as well.
#
# This file lives at <scope>\d2rloader\config\celestialrayone.gold-capacities.toml
# and is created with these defaults on first run. Delete it to get them back.
# It is read once at plugin load, so restart the game after editing.
# In TCP/IP games, use the same values on every machine.
#
# Type `gold-capacities` in the console to see the values in effect.


[gold-capacities]

# Master switch.
#   true  - the limits below are applied.
#   false - the plugin does not load at all (see the warning above).
# Default: true
enabled = true

# Carried gold limit = character level x gold_per_level.
# Vanilla is 10000.
#
# Must be between 1 and 16909320. The game multiplies in 32 bits, and that
# range stays safe up to level 127, the highest level a character file holds.
# With vanilla CSvBits, keep level x gold_per_level at or below 33,554,431:
# 338933 or less for a level 99 cap, 305040 or less for a level 110 cap.
#
# Anything outside the range is refused and 100000 is used instead.
# Default: 100000
gold_per_level = 100000

# Personal stash limit, limit of each shared stash tab, and the amount the
# shared stash total is built from (shared tabs x stash_gold, capped at
# 2,147,483,647).
# Vanilla is 2500000.
#
# Must be between 1 and 2147483647.
# With vanilla CSvBits the personal stash keeps at most 33,554,431 through a
# save, so above that the personal stash loses the difference on every save.
#
# Anything outside the range is refused and 25000000 is used instead.
# Default: 25000000
stash_gold = 25000000
)toml";

// The loader hands the plugin the raw text of its own TOML file; there is no
// parser in the SDK. This scanner understands exactly what this file needs:
// [section] headers, `key = value`, `#` comments (whole line or trailing), and
// both LF and CRLF.

struct Slice {
    const char* begin;
    const char* end;
};

constexpr auto IsBlank(char value) noexcept -> bool {
    return value == ' ' || value == '\t' || value == '\r';
}

constexpr auto Trim(Slice slice) noexcept -> Slice {
    while (slice.begin < slice.end && IsBlank(*slice.begin)) {
        ++slice.begin;
    }
    while (slice.end > slice.begin && IsBlank(slice.end[-1])) {
        --slice.end;
    }
    return slice;
}

auto SliceEquals(Slice slice, const char* literal) noexcept -> bool {
    const char* text   = literal;
    const char* cursor = slice.begin;
    while (cursor < slice.end && *text != '\0') {
        if (*cursor != *text) {
            return false;
        }
        ++cursor;
        ++text;
    }
    return cursor == slice.end && *text == '\0';
}

// Finds the raw right-hand side of section.key. The last assignment wins.
auto FindConfigValue(const char* toml, const char* section, const char* key, Slice& value) noexcept -> bool {
    if (toml == nullptr) {
        return false;
    }

    std::array<char, 64> currentSection {};
    bool                 found = false;

    for (const char* line = toml; *line != '\0';) {
        const char* lineEnd = line;
        while (*lineEnd != '\0' && *lineEnd != '\n') {
            ++lineEnd;
        }

        const Slice trimmed = Trim({ line, lineEnd });

        if (trimmed.begin < trimmed.end && *trimmed.begin != '#') {
            if (*trimmed.begin == '[') {
                const char* close = trimmed.begin;
                while (close < trimmed.end && *close != ']') {
                    ++close;
                }

                const Slice name = Trim({ trimmed.begin + 1, close });
                std::size_t used = 0;
                for (const char* cursor = name.begin; cursor < name.end && used + 1 < currentSection.size(); ++cursor) {
                    currentSection[used++] = *cursor;
                }
                currentSection[used] = '\0';
            } else {
                const char* equals = trimmed.begin;
                while (equals < trimmed.end && *equals != '=') {
                    ++equals;
                }

                if (equals < trimmed.end) {
                    const Slice name = Trim({ trimmed.begin, equals });
                    Slice       raw  = Trim({ equals + 1, trimmed.end });

                    for (const char* cursor = raw.begin; cursor < raw.end; ++cursor) {
                        if (*cursor == '#') {
                            raw.end = cursor;
                            break;
                        }
                    }
                    raw = Trim(raw);

                    if (std::strcmp(currentSection.data(), section) == 0 && SliceEquals(name, key)) {
                        value = raw;
                        found = true;
                    }
                }
            }
        }

        line = (*lineEnd == '\0') ? lineEnd : lineEnd + 1;
    }

    return found;
}

// Whole numbers only, optional sign, TOML `_` digit separators allowed.
auto ParseInt(Slice raw, std::int32_t& value) noexcept -> bool {
    const char* cursor   = raw.begin;
    bool        negative = false;
    if (cursor < raw.end && (*cursor == '+' || *cursor == '-')) {
        negative = (*cursor == '-');
        ++cursor;
    }

    bool         sawDigit = false;
    std::int64_t parsed   = 0;
    for (; cursor < raw.end; ++cursor) {
        if (*cursor == '_') {
            continue;
        }
        if (*cursor < '0' || *cursor > '9') {
            return false;
        }
        sawDigit = true;
        parsed   = parsed * 10 + (*cursor - '0');
        if (parsed > 0x7FFFFFFFLL) {
            return false;
        }
    }

    if (!sawDigit) {
        return false;
    }

    value = static_cast<std::int32_t>(negative ? -parsed : parsed);
    return true;
}

struct Settings {
    bool         enabled { true };
    std::int32_t goldPerLevel { DefaultGoldPerLevel };
    std::int32_t stashGold { DefaultStashGold };
};

const D2RL::PluginContext* Context           = nullptr;
std::uintptr_t             Base              = 0;
Settings                   Config {};
bool                       ConfigFileWasRead = false;

void ReadBoundedValue(const char* toml, const char* key, std::int32_t minimum, std::int32_t maximum,
        std::int32_t fallback, std::int32_t& target) noexcept {
    Slice raw {};
    if (!FindConfigValue(toml, ConfigSection, key, raw)) {
        return; // absent: the default stays
    }

    std::int32_t requested = 0;
    if (!ParseInt(raw, requested) || requested < minimum || requested > maximum) {
        D2RL::LogWarnF(Context, "%s = %.*s is not a whole number from %d to %d. Using %d.", key,
            static_cast<int>(raw.end - raw.begin), raw.begin, minimum, maximum, fallback);
        target = fallback;
        return;
    }

    target = requested;
}

void LoadConfiguration() noexcept {
    if (!Context->EnsureConfig(DefaultConfigToml)) {
        Context->LogWarn("Could not create or open celestialrayone.gold-capacities.toml. Running with defaults.");
        return;
    }

    std::array<char, 16384> buffer {};
    std::uint32_t           requiredSize = 0;

    if (!Context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size() - 1), &requiredSize)) {
        Context->LogWarn("Could not read celestialrayone.gold-capacities.toml. Running with defaults.");
        return;
    }

    if (requiredSize >= buffer.size()) {
        D2RL::LogWarnF(Context,
            "celestialrayone.gold-capacities.toml is %u bytes, larger than the %zu byte read buffer. "
            "Running with defaults.",
            requiredSize, buffer.size());
        return;
    }

    buffer[buffer.size() - 1] = '\0';
    ConfigFileWasRead         = true;

    Slice raw {};
    if (FindConfigValue(buffer.data(), ConfigSection, "enabled", raw)) {
        if (SliceEquals(raw, "true")) {
            Config.enabled = true;
        } else if (SliceEquals(raw, "false")) {
            Config.enabled = false;
        } else {
            Context->LogWarn("enabled is neither true nor false. Using true.");
        }
    }

    ReadBoundedValue(buffer.data(), "gold_per_level", 1, MaximumGoldPerLevel, DefaultGoldPerLevel,
        Config.goldPerLevel);
    ReadBoundedValue(buffer.data(), "stash_gold", 1, MaximumStashGold, DefaultStashGold, Config.stashGold);

    D2RL::LogInfoF(Context, "config: enabled=%s, gold_per_level=%d (vanilla %d), stash_gold=%d (vanilla %d).",
        Config.enabled ? "true" : "false", Config.goldPerLevel, VanillaGoldPerLevel, Config.stashGold,
        VanillaStashGold);
}

// ---------------------------------------------------------------------------
// Game side: one loader transaction, all three values or none
// ---------------------------------------------------------------------------

enum class GameState : std::uint8_t {
    NotAttempted,
    Applied,
    VanillaValues,
};

enum class CoreState : std::uint8_t {
    NotAttempted,
    Applied,
    VanillaValues,
    ModuleMissing,
    ExportMissing,
    BodyMismatch,
    WriteFailed,
};

std::atomic<GameState> GamePart { GameState::NotAttempted };
std::atomic<CoreState> CorePart { CoreState::NotAttempted };

auto GameSiteValue(std::size_t index) noexcept -> std::uint32_t {
    return static_cast<std::uint32_t>(index == 0 ? Config.goldPerLevel : Config.stashGold);
}

auto LiveGameBytes(std::uint64_t rva) noexcept -> const std::uint8_t* {
    return reinterpret_cast<const std::uint8_t*>(Base + rva);
}

auto VerifyGameContract() noexcept -> bool {
    for (const GameSite& site : GameSites) {
        if (Context->CheckExpectedBytes(site.windowRva, site.window, site.windowSize)) {
            continue;
        }

        // Tell a different game build apart from a value someone already changed.
        const std::uint8_t* live          = LiveGameBytes(site.windowRva);
        const std::uint32_t valueEnd      = site.valueOffset + 4U;
        const bool          onlyValueDiff = std::memcmp(live, site.window, site.valueOffset) == 0
            && std::memcmp(live + valueEnd, site.window + valueEnd, site.windowSize - valueEnd) == 0;

        if (onlyValueDiff) {
            D2RL::LogErrorF(Context,
                "The %s at RVA 0x%llX already holds %u instead of the vanilla %u: another patch or plugin "
                "changes it (for example a gold limit JSON patch). Remove that one, this plugin replaces it. "
                "Declining to load.",
                site.label, static_cast<unsigned long long>(site.windowRva + site.valueOffset),
                ReadLe32(live + site.valueOffset), ReadLe32(site.window + site.valueOffset));
        } else {
            D2RL::LogErrorF(Context,
                "The %s at RVA 0x%llX does not match the verified D2R 3.3.93847 bytes. This game build is not "
                "supported. Declining to load, nothing was patched.",
                site.label, static_cast<unsigned long long>(site.windowRva));
        }
        return false;
    }
    return true;
}

auto MutationResultName(D2RL::Mutations::Result result) noexcept -> const char* {
    using D2RL::Mutations::Result;
    switch (result) {
    case Result::Success:               return "success";
    case Result::InvalidArgument:       return "invalid argument";
    case Result::Unsupported:           return "unsupported";
    case Result::OwnerInactive:         return "owner inactive";
    case Result::NotFound:              return "not found";
    case Result::InvalidState:          return "invalid state";
    case Result::Conflict:              return "conflict with another patch";
    case Result::ExpectedBytesMismatch: return "expected bytes mismatch";
    case Result::Unavailable:           return "unavailable";
    case Result::CommitFailed:          return "commit failed";
    case Result::RollbackFailed:        return "rollback failed";
    case Result::CallbackFault:         return "callback fault";
    }
    return "unknown";
}

auto FailureReasonName(D2RL::Mutations::FailureReason reason) noexcept -> const char* {
    using D2RL::Mutations::FailureReason;
    switch (reason) {
    case FailureReason::None:                  return "none";
    case FailureReason::EmptyTransaction:      return "empty transaction";
    case FailureReason::InvalidRange:          return "invalid range";
    case FailureReason::Overlap:               return "overlaps another patch";
    case FailureReason::ExpectedBytesMismatch: return "expected bytes mismatch";
    case FailureReason::NativeHooksRequired:   return "NativeHooks flag required";
    case FailureReason::HookPreparationFailed: return "hook preparation failed";
    case FailureReason::MemoryWriteFailed:     return "memory write failed";
    case FailureReason::HookEnableFailed:      return "hook enable failed";
    case FailureReason::TrackingFailed:        return "tracking failed";
    case FailureReason::RollbackFailed:        return "rollback failed";
    }
    return "unknown";
}

// The loader keeps a committed transaction until the plugin unloads, then
// restores every byte itself.
auto ApplyGamePatches() noexcept -> bool {
    const D2RL::MutationService* mutations = nullptr;
    if (Context->QueryService(&mutations) != D2RL::ServiceQueryResult::Success
            || !D2RL::HasMutationServiceField(mutations, D2RL::MutationServiceRequiredSize)) {
        Context->LogError("The loader's mutation service is unavailable. Declining to load, nothing was patched.");
        return false;
    }

    D2RL::Mutations::TransactionHandle transaction = D2RL::Mutations::InvalidTransactionHandle;
    D2RL::Mutations::Result            status      = mutations->beginTransaction(Context, &transaction);
    if (status != D2RL::Mutations::Result::Success) {
        D2RL::LogErrorF(Context, "Could not start a patch transaction (%s). Declining to load.",
            MutationResultName(status));
        return false;
    }

    D2RL::Mutations::OperationHandle operations[GameSiteCount] {};
    std::size_t                      staged = 0;

    for (std::size_t index = 0; index < GameSiteCount; ++index) {
        const GameSite&     site     = GameSites[index];
        const std::uint8_t* original = site.window + site.valueOffset;
        const std::uint32_t value    = GameSiteValue(index);
        if (value == ReadLe32(original)) {
            continue; // the configured value is the vanilla one
        }

        std::uint8_t replacement[4] {};
        WriteLe32(replacement, value);

        const D2RL::Mutations::BytePatchRequest request {
            .structSize   = D2RL::Mutations::BytePatchRequestSize,
            .flags        = 0,
            .rva          = site.windowRva + site.valueOffset,
            .expected     = original,
            .expectedSize = 4,
            .reserved     = 0,
            .bytes        = replacement,
            .size         = 4,
            .reserved2    = 0,
        };

        status = mutations->stageBytePatch(Context, transaction, &request, &operations[index]);
        if (status != D2RL::Mutations::Result::Success) {
            D2RL::LogErrorF(Context, "Staging the %s at RVA 0x%llX failed (%s). Declining to load.", site.label,
                static_cast<unsigned long long>(request.rva), MutationResultName(status));
            mutations->cancelTransaction(Context, transaction);
            return false;
        }
        ++staged;
    }

    if (staged == 0) {
        mutations->cancelTransaction(Context, transaction);
        GamePart.store(GameState::VanillaValues, std::memory_order_release);
        Context->LogInfo("Both values are the vanilla ones: no game change needed.");
        return true;
    }

    D2RL::Mutations::CommitResult commit {};
    commit.structSize = D2RL::Mutations::CommitResultSize;
    status            = mutations->commit(Context, transaction, &commit);
    if (status != D2RL::Mutations::Result::Success || commit.result != D2RL::Mutations::Result::Success) {
        const char* failed = "an unknown site";
        for (std::size_t index = 0; index < GameSiteCount; ++index) {
            if (operations[index] != D2RL::Mutations::InvalidOperationHandle && operations[index] == commit.operation) {
                failed = GameSites[index].label;
            }
        }
        D2RL::LogErrorF(Context,
            "The loader refused the %s (%s, %s). Nothing from this plugin was applied. Declining to load.", failed,
            MutationResultName(commit.result), FailureReasonName(commit.failureReason));
        mutations->cancelTransaction(Context, transaction);
        return false;
    }

    GamePart.store(GameState::Applied, std::memory_order_release);
    return true;
}

// ---------------------------------------------------------------------------
// D2RCore side: the shared stash total
// ---------------------------------------------------------------------------

std::uint8_t*                                      CoreFunction = nullptr;
std::array<std::uint8_t, sizeof(SharedLimitBody)> CoreWritten {};
bool                                               CorePatched = false;

// True when [address, address + size) is committed executable memory of the
// module whose base is module.
auto IsModuleCode(const std::uint8_t* address, std::size_t size, HMODULE module) noexcept -> bool {
    MEMORY_BASIC_INFORMATION info {};
    if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info)) {
        return false;
    }
    if (info.State != MEM_COMMIT || info.AllocationBase != module) {
        return false;
    }
    const auto* regionEnd = static_cast<const std::uint8_t*>(info.BaseAddress) + info.RegionSize;
    if (address + size > regionEnd) {
        return false;
    }
    const DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (info.Protect & executable) != 0 && (info.Protect & PAGE_GUARD) == 0;
}

// Writes code bytes and reads them back.
auto WriteCode(std::uint8_t* target, const std::uint8_t* bytes, std::size_t size) noexcept -> bool {
    DWORD previous = 0;
    if (!VirtualProtect(target, size, PAGE_EXECUTE_READWRITE, &previous)) {
        return false;
    }
    std::memcpy(target, bytes, size);
    DWORD ignored = 0;
    VirtualProtect(target, size, previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, size);
    return std::memcmp(target, bytes, size) == 0;
}

void ApplySharedTotal() noexcept {
    const HMODULE core = GetModuleHandleW(CoreModuleName);
    if (core == nullptr) {
        CorePart.store(CoreState::ModuleMissing, std::memory_order_release);
        Context->LogError("D2RCore.dll is not loaded. The shared stash total stays at tabs x 2,500,000.");
        return;
    }

    auto* function = reinterpret_cast<std::uint8_t*>(GetProcAddress(core, SharedLimitExport));
    if (function == nullptr || !IsModuleCode(function, 5, core)) {
        CorePart.store(CoreState::ExportMissing, std::memory_order_release);
        Context->LogError(
            "D2RCore.dll does not export GetSharedStashGoldLimit. The shared stash total stays at tabs x 2,500,000.");
        return;
    }
    if (function[0] == JmpRel32Opcode) {
        std::int32_t displacement = 0;
        std::memcpy(&displacement, function + 1, sizeof(displacement));
        function += 5 + displacement;
    }

    if (!IsModuleCode(function, sizeof(SharedLimitBody), core)
            || std::memcmp(function, SharedLimitBody, sizeof(SharedLimitBody)) != 0) {
        CorePart.store(CoreState::BodyMismatch, std::memory_order_release);
        Context->LogError(
            "D2RCore!GetSharedStashGoldLimit does not match the verified D2RLoader 1.3.1 bytes (another D2RLoader "
            "version, or something already changed it). Left untouched: the shared stash total stays at tabs x "
            "2,500,000. Carried gold, the personal stash and each shared tab are still raised.");
        return;
    }
    CoreFunction = function;

    if (Config.stashGold == VanillaStashGold) {
        CorePart.store(CoreState::VanillaValues, std::memory_order_release);
        return;
    }

    std::array<std::uint8_t, sizeof(SharedLimitBody)> patched {};
    std::memcpy(patched.data(), SharedLimitBody, sizeof(SharedLimitBody));
    WriteLe32(patched.data() + SharedPerTabOffset, static_cast<std::uint32_t>(Config.stashGold));
    WriteLe32(patched.data() + SharedCutoffOffset, SharedTabCutoff(Config.stashGold));

    if (!WriteCode(function, patched.data(), patched.size())) {
        (void)WriteCode(function, SharedLimitBody, sizeof(SharedLimitBody));
        CorePart.store(CoreState::WriteFailed, std::memory_order_release);
        Context->LogError(
            "Writing D2RCore!GetSharedStashGoldLimit failed. The shared stash total stays at tabs x 2,500,000.");
        return;
    }

    CoreWritten = patched;
    CorePatched = true;
    CorePart.store(CoreState::Applied, std::memory_order_release);
}

void RestoreSharedTotal() noexcept {
    if (!CorePatched || CoreFunction == nullptr) {
        return;
    }
    CorePatched = false;

    if (std::memcmp(CoreFunction, CoreWritten.data(), CoreWritten.size()) != 0) {
        if (Context != nullptr) {
            Context->LogWarn("D2RCore!GetSharedStashGoldLimit changed after this plugin wrote it; left as it is.");
        }
        return;
    }
    if (!WriteCode(CoreFunction, SharedLimitBody, sizeof(SharedLimitBody)) && Context != nullptr) {
        Context->LogError("Could not restore D2RCore!GetSharedStashGoldLimit on unload.");
    }
}

// ---------------------------------------------------------------------------
// Console
// ---------------------------------------------------------------------------

auto GoldCapacitiesCommand(
        D2R::Game::Client*                 client,
        const D2RL::ConsoleCommandContext* command,
        void*                              userData) noexcept -> D2RL::ConsoleCommandResult {
    (void)client;
    (void)userData;

    if (command == nullptr || command->plugin == nullptr) {
        return D2RL::ConsoleCommandResult::Failed;
    }
    const D2RL::PluginContext* context = command->plugin;

    char line[256] {};
    std::snprintf(line, sizeof(line),
        "gold-capacities: config %s. Carried gold = level x %d (vanilla %d). Stash and each shared tab = %d "
        "(vanilla %d).",
        ConfigFileWasRead ? "loaded" : "NOT READ (defaults in use)", Config.goldPerLevel, VanillaGoldPerLevel,
        Config.stashGold, VanillaStashGold);
    context->WriteConsoleMessage(line);

    switch (GamePart.load(std::memory_order_acquire)) {
    case GameState::Applied:
        std::snprintf(line, sizeof(line),
            "game limits: active. Live values: per level %u, stash %u, shared stash file check %u.",
            ReadLe32(LiveGameBytes(CarriedGoldRva + CarriedGoldValueOffset)),
            ReadLe32(LiveGameBytes(StashGoldRva + StashGoldValueOffset)),
            ReadLe32(LiveGameBytes(FileCheckRva + FileCheckValueOffset)));
        context->WriteConsoleMessage(line);
        break;
    case GameState::VanillaValues:
        context->WriteConsoleMessage("game limits: the configured values are the vanilla ones, nothing changed.");
        break;
    case GameState::NotAttempted:
    default:
        context->WriteConsoleError("game limits: NOT ACTIVE. See the plugin log.");
        break;
    }

    switch (CorePart.load(std::memory_order_acquire)) {
    case CoreState::Applied:
        std::snprintf(line, sizeof(line),
            "shared stash total: active, tabs x %u, held at 2147483647 from %u tabs.",
            ReadLe32(CoreFunction + SharedPerTabOffset), ReadLe32(CoreFunction + SharedCutoffOffset));
        context->WriteConsoleMessage(line);
        break;
    case CoreState::VanillaValues:
        context->WriteConsoleMessage("shared stash total: vanilla value configured, D2RCore left unchanged.");
        break;
    case CoreState::ModuleMissing:
    case CoreState::ExportMissing:
    case CoreState::BodyMismatch:
    case CoreState::WriteFailed:
        context->WriteConsoleWarning(
            "shared stash total: NOT raised, stays at tabs x 2500000. D2RCore.dll is not the verified 1.3.1 "
            "build. See the plugin log.");
        break;
    case CoreState::NotAttempted:
    default:
        context->WriteConsoleError("shared stash total: NOT ACTIVE. See the plugin log.");
        break;
    }

    return D2RL::ConsoleCommandResult::Handled;
}

constexpr D2RL::PluginInfo Info {
    .infoSize    = D2RL::PluginInfoSize,
    .abiVersion  = D2RL_PLUGIN_ABI_VERSION,
    .id          = "celestialrayone.gold-capacities",
    .name        = "Gold Capacities",
    .version     = "1.0.0",
    .author      = "CelestialRayOne",
    .description = "Raises the carried gold, personal stash and shared stash gold limits.",
    // Shared: the client builds the dialogs and writes the shared stash file,
    // the server enforces the limits. Shared also records the plugin in each
    // character's save environment, which is what makes D2RLoader warn before
    // a character saved with it loads without it.
    .flags       = D2RL::PluginFlags::Shared,
};

static_assert(D2RL::HasValidPluginRole(Info.flags), "Exactly one execution role must be set.");
static_assert(D2RL::HasOnlyKnownPluginFlags(Info.flags), "Unknown plugin flag set.");

} // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
    return &Info;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
    if (!D2RL::HasContext(context) || context->exeBase == 0) {
        return false;
    }
    Context = context;
    Base    = context->exeBase;

    if (const char* build = D2RL::GetBuildVersion(context); build != nullptr) {
        D2RL::LogInfoF(context, "gold-capacities loading against build %s.", build);
    }

    LoadConfiguration();

    // Declining to load is the safe answer whenever the game limits will not
    // be raised: see the comment at the top of this file.
    if (!Config.enabled) {
        Context->LogWarn(
            "Turned off in the config file. Declining to load, so D2RLoader keeps warning before a character "
            "saved with this plugin loads (the game empties carried and stash gold above the vanilla limits).");
        return false;
    }

    if (!VerifyGameContract() || !ApplyGamePatches()) {
        return false;
    }

    ApplySharedTotal();

    if (!Context->RegisterConsoleCommand("gold-capacities", GoldCapacitiesCommand,
            "Report the gold limits in effect.")) {
        Context->LogWarn("The gold-capacities console command was not registered.");
    }

    D2RL::LogInfoF(Context,
        "Loaded. Carried gold = level x %d, stash and each shared tab = %d, shared stash total %s.",
        Config.goldPerLevel, Config.stashGold,
        CorePart.load(std::memory_order_acquire) == CoreState::Applied
                || CorePart.load(std::memory_order_acquire) == CoreState::VanillaValues
            ? "follows it"
            : "NOT raised (see above)");
    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    // The game side is a loader transaction: D2RLoader restores it itself.
    RestoreSharedTotal();
}
