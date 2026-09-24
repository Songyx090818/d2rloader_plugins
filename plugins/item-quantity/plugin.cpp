// =============================================================================
//  Item Quantity  -  celestialrayone.item-quantity
//
//  A stat on the killer adds extra picks to the monster's own treasure class,
//  so monsters drop more of what they already drop. Port of the 2.4 patch set
//  (hook 2F9317 + caves 1A22E8 / 36ABA0 / 36ABE0) to D2R 3.3 under D2RLoader
//  1.3.0, as a plugin: one 6-byte hook, one stub, everything else in C++ and
//  in the config file.
//
// -----------------------------------------------------------------------------
//  WHERE THE PICKS ARE DECIDED
// -----------------------------------------------------------------------------
//  TREASURECLASS_GenerateDrops 0x441300 resolves the treasure class row and
//  calls the roll at 0x4404F0 (2.4 sub_1402F9210). The roll keeps a stack of
//  treasure-class frames at rbp+0x80, stride 32:
//
//      +0x00  the treasureclassex record
//      +0x08  the working pick counter, decremented once per item dropped
//      +0x18  the depth of this frame
//
//  Frame 0 is the monster's own treasure class, so its counter is [rbp+0x88].
//  It is filled at 0x4406A1 with max(abs(Picks), 1), Picks being the dword at
//  record +0x18 (it was +0x0C on 2.4), and the drop loop starts one
//  instruction later at 0x4406A7. Adding to the counter at exactly that point
//  gives the monster extra picks from its own list, once per drop, which is
//  what the 2.4 cave did.
//
//  Arguments are already homed by then: the monster at [rsp+0x58], the killer
//  at [rbp-0x80], the game at [rsp+0x70], and the record is still in rbx.
//
// -----------------------------------------------------------------------------
//  WHAT THIS BUILD DOES DIFFERENTLY FROM THE 2.4 CAVES
// -----------------------------------------------------------------------------
//  Two of the three 2.4 caves are not needed here.
//
//  Cave B walked a summon's stat list by hand to find its owner. The roll
//  itself resolves a non-player killer through 0x4A53C0 and uses the result to
//  credit the owner's magic find, so this plugin calls the same function: it
//  checks the unit is a monster, reads MonsterData at +0x10, the block at
//  +0x30, and looks the owner up by context/type/id.
//
//  Cave A's itemstatcost bounds checks are not needed either: 0x2F5020 is the
//  (unit, statId, layer) stat entry the roll already uses, so reading the stat
//  off the killer is one call with no table anchors to keep in sync.
//
//  Kept exactly: picks are only ever added when the record's Picks is
//  POSITIVE. A negative Picks is a guaranteed-drop list whose counter is used
//  as an index, and inflating it would index out of range.
//
//  THE STAT IS A MULTIPLIER OF THE MONSTER'S OWN PICKS by default: with
//  Picks 5 a stat of 100 adds 5 more, so the monster rolls its list ten times
//  instead of five. The 2.4 meaning of the stat, a percent chance of ONE
//  extra pick, is still available as mode = "chance".
//
//  THE ENGINE CAPS ITEMS PER DROP AT SIX on the monster path. The roll stores
//  that limit at [rsp+0x68] (default 6 when the caller passes no array of its
//  own, which the monster death path at 0x447DF0 does not) and re-reads it for
//  every item at 0x441157 before deciding to stop. Extra picks past the cap
//  produce nothing, so max_items raises it from the same hook, and only when
//  the caller supplied no array of its own, since raising it for a caller with
//  a fixed-size array would overflow that array.
//
//  Kept exactly: the percent roll never calls the game RNG. It hashes the
//  monster's own drop seed (unit +0x28 / +0x2C through 0x34A1E0), so no draw
//  is consumed and no existing drop behaviour shifts. Deterministic per
//  monster, which is how D2 drops already work.
//
// -----------------------------------------------------------------------------
//  THE STUB
// -----------------------------------------------------------------------------
//  The six bytes at 0x4406A1 become jmp rel32 + nop into a page the plugin
//  allocates in reach of the image. The stub replays the stolen store, saves
//  every volatile register (rax is live across the site), calls the C++ entry
//  with monster, killer, record, &counter and game, and jumps back to
//  0x4406A7. It is entered by a jump, never by a call, so the C++ entry is
//  noexcept and does nothing that can raise: every pointer is checked and
//  every game function it calls is guarded against the arguments that make it
//  assert.
//
//  Console command "itemquantity" shows the settings, what is installed and
//  the counters. Settings: d2rloader/config/celestialrayone.item-quantity.toml
// =============================================================================

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
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace CelestialRayOne::ItemQuantity {
namespace {

constexpr char PluginVersion[] = "1.1.0";

// ---------------------------------------------------------------------------
//  Default configuration, written on first load and doubling as the plugin's
//  documentation.
// ---------------------------------------------------------------------------
constexpr const char DefaultConfigToml[] = R"TOML(# celestialrayone.item-quantity
#
# Item Quantity
#   A stat on whoever gets the kill adds extra picks to the monster's OWN
#   treasure class, so the monster drops more of what it already drops. No
#   treasure class is swapped and no new item list is introduced.
#
#   The stat is an ordinary itemstatcost stat. Put it on gear, on a charm, or
#   on a passive skill through passivestat/passivecalc, the same way magic
#   find is granted.
#
# Changes are read when the plugin loads. Restart the game after editing.
# Built for D2RLoader 1.3.1 (Diablo II: Resurrected 3.3). On any other build
# the byte checks fail and the plugin loads without changing the game.

# Master switch. false loads the plugin without touching the game.
enabled = true

# -----------------------------------------------------------------------------
# stat_id
#
# The itemstatcost row the bonus is read from. The stat must exist in
# itemstatcost.txt or nothing is ever added.
stat_id = 426

# -----------------------------------------------------------------------------
# mode
#
# What the stat means.
#
# "scale"  : a percent of the monster's OWN picks. A monster whose treasure
#            class has Picks 5 rolls its list 5 more times at 100, 10 more at
#            200, and 2 or 3 more at 50 (2 guaranteed plus a 50 percent chance
#            of the third). This is the default.
# "chance" : a percent chance of ONE extra pick, whatever the monster's picks
#            are. 100 always adds exactly one. This is what the 2.4 patch did.
# "flat"   : the stat is a number of extra picks. 3 always adds 3.
#
# Fractions are decided from the monster's own drop seed, not from the game
# RNG, so no roll is consumed and no other drop shifts. The same monster in
# the same game always decides the same way.
mode = "scale"

# -----------------------------------------------------------------------------
# exclude_prime_evils
#
# true keeps prime evils (the primeevil flag in monstats.txt) out of it, so
# boss drops stay exactly as the treasure class describes them.
exclude_prime_evils = true

# -----------------------------------------------------------------------------
# credit_minion_owner
#
# When a summon, a hireling or any other minion lands the kill, the game hands
# the drop code the MINION, not the player. true resolves its owner and reads
# the stat off the owner, the same way the game credits the owner's magic find
# on those kills. false means minion kills get no bonus at all.
#
# A minion whose owner cannot be resolved, or whose owner is not a player,
# never gets a bonus either way.
credit_minion_owner = true

# -----------------------------------------------------------------------------
# max_bonus_picks
#
# An upper limit on the extra picks a single drop can gain. 0 means no limit.
# Note the game still caps how many items one monster can drop, so a very
# large bonus is clipped there regardless.
max_bonus_picks = 0

# -----------------------------------------------------------------------------
# max_items
#
# The game refuses to drop more than six items from one monster, whatever the
# picks say, so extra picks past six are wasted. This raises that limit for
# drops the game did not size itself (monsters and chests); 0 leaves the stock
# six alone. 50 matches the drop cap this mod used on 2.4.
#
# Raising the limit changes drops on its own, without the stat: a treasure
# class with more than six picks already loses items to the cap today.
max_items = 0

# Writes a line to the plugin log for the first 64 drops that gain picks.
# Keep false for normal play. The console command "itemquantity" always shows
# the settings, what is installed and the counters.
diagnostics = false
)TOML";

// LOGIC-BEGIN
// ---------------------------------------------------------------------------
//  Settings
// ---------------------------------------------------------------------------
constexpr std::int32_t MinimumStatId     = 0;
constexpr std::int32_t MaximumStatId     = 4095;
constexpr std::int32_t MaximumBonusLimit = 1000;
constexpr std::int32_t MaximumItemLimit  = 255;

enum class Mode : std::uint8_t {
    Scale,   // a percent of the monster's own picks
    Chance,  // a percent chance of one extra pick (the 2.4 meaning)
    Flat,    // a flat number of extra picks
};

struct Settings {
    bool         enabled{true};
    std::int32_t statId{426};
    Mode         mode{Mode::Scale};
    bool         excludePrimeEvils{true};
    bool         creditMinionOwner{true};
    std::int32_t maxBonusPicks{0};
    std::int32_t maxItems{0};
    bool         diagnostics{false};
};

inline auto ModeName(Mode mode) noexcept -> const char* {
    switch (mode) {
        case Mode::Scale:  return "scale";
        case Mode::Chance: return "chance";
        case Mode::Flat:   return "flat";
    }
    return "scale";
}

// The 2.4 roll, kept byte for byte in meaning: whole picks plus one more if a
// hash of the monster's drop seed lands under the remainder.
inline auto SeedRoll(std::uint32_t seedLow, std::uint32_t seedHigh) noexcept -> std::uint32_t {
    std::uint32_t value = seedLow ^ seedHigh;
    value *= 0x9E3779B1u;
    value >>= 8;
    return value % 100u;
}

// hundredths of a pick -> whole picks, the fraction decided by the roll.
inline auto PicksFromHundredths(std::int64_t hundredths, std::uint32_t roll) noexcept -> std::int64_t {
    if (hundredths <= 0) {
        return 0;
    }
    return hundredths / 100 + ((roll < static_cast<std::uint32_t>(hundredths % 100)) ? 1 : 0);
}

inline auto BonusPicksFor(const Settings& settings, std::int32_t basePicks, std::int32_t stat,
                          std::uint32_t roll) noexcept -> std::int32_t {
    if (stat <= 0 || basePicks <= 0) {
        return 0;
    }
    std::int64_t extra = 0;
    switch (settings.mode) {
        case Mode::Scale: {
            std::int64_t hundredths = static_cast<std::int64_t>(basePicks) * static_cast<std::int64_t>(stat);
            if (hundredths > 100LL * MaximumBonusLimit) {
                hundredths = 100LL * MaximumBonusLimit;
            }
            extra = PicksFromHundredths(hundredths, roll);
            break;
        }
        case Mode::Chance:
            extra = PicksFromHundredths(stat, roll);
            break;
        case Mode::Flat:
            extra = stat;
            break;
    }
    if (extra <= 0) {
        return 0;
    }
    if (settings.maxBonusPicks > 0 && extra > settings.maxBonusPicks) {
        extra = settings.maxBonusPicks;
    }
    if (extra > MaximumBonusLimit) {
        extra = MaximumBonusLimit;
    }
    return static_cast<std::int32_t>(extra);
}

// ---------------------------------------------------------------------------
//  Configuration parser. Accepts the subset of TOML the default file uses:
//  key = true|false and key = <non-negative integer>. Anything it does not
//  understand is an error, never a silent default.
// ---------------------------------------------------------------------------
struct ConfigCursor {
    std::string_view text;
    std::size_t      position{0};
    std::size_t      line{1};
};

inline auto CursorPeek(const ConfigCursor& cursor) noexcept -> char {
    return cursor.position < cursor.text.size() ? cursor.text[cursor.position] : '\0';
}

inline auto CursorAtEnd(const ConfigCursor& cursor) noexcept -> bool {
    return cursor.position >= cursor.text.size();
}

inline void CursorAdvance(ConfigCursor& cursor) noexcept {
    if (cursor.position < cursor.text.size()) {
        if (cursor.text[cursor.position] == '\n') {
            ++cursor.line;
        }
        ++cursor.position;
    }
}

inline void SkipBlank(ConfigCursor& cursor, bool acrossLines) noexcept {
    while (!CursorAtEnd(cursor)) {
        const char current = CursorPeek(cursor);
        if (current == ' ' || current == '\t' || current == '\r') {
            CursorAdvance(cursor);
        } else if (current == '#') {
            while (!CursorAtEnd(cursor) && CursorPeek(cursor) != '\n') {
                CursorAdvance(cursor);
            }
        } else if (current == '\n' && acrossLines) {
            CursorAdvance(cursor);
        } else {
            return;
        }
    }
}

inline auto FormatError(std::string& error, const char* format, ...) noexcept -> bool {
    char message[256]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    error.assign(message);
    return false;
}

inline auto AtWordEnd(std::string_view text, std::size_t at) noexcept -> bool {
    if (at >= text.size()) {
        return true;
    }
    const char next = text[at];
    return next == ' ' || next == '\t' || next == '\r' || next == '\n' || next == '#';
}

inline auto ParseBool(ConfigCursor& cursor, bool* value, std::string& error, std::string_view key) noexcept -> bool {
    const auto rest = cursor.text.substr(cursor.position);
    if (rest.substr(0, 4) == "true" && AtWordEnd(rest, 4)) {
        *value = true;
        cursor.position += 4;
        return true;
    }
    if (rest.substr(0, 5) == "false" && AtWordEnd(rest, 5)) {
        *value = false;
        cursor.position += 5;
        return true;
    }
    return FormatError(error, "line %zu: %.*s must be true or false", cursor.line, static_cast<int>(key.size()),
                       key.data());
}

inline auto ParseMode(ConfigCursor& cursor, Mode* value, std::string& error) noexcept -> bool {
    const char* expected = "mode must be \"scale\", \"chance\" or \"flat\"";
    if (CursorPeek(cursor) != '"') {
        return FormatError(error, "line %zu: %s", cursor.line, expected);
    }
    CursorAdvance(cursor);
    const auto start = cursor.position;
    while (!CursorAtEnd(cursor) && CursorPeek(cursor) != '"' && CursorPeek(cursor) != '\n') {
        CursorAdvance(cursor);
    }
    if (CursorPeek(cursor) != '"') {
        return FormatError(error, "line %zu: mode is missing its closing quote", cursor.line);
    }
    const auto text = cursor.text.substr(start, cursor.position - start);
    CursorAdvance(cursor);
    if (text == "scale") {
        *value = Mode::Scale;
    } else if (text == "chance") {
        *value = Mode::Chance;
    } else if (text == "flat") {
        *value = Mode::Flat;
    } else {
        return FormatError(error, "line %zu: %s", cursor.line, expected);
    }
    return true;
}

inline auto ParseInteger(ConfigCursor& cursor, std::int32_t* value, std::int32_t low, std::int32_t high,
                         std::string& error, std::string_view key) noexcept -> bool {
    const auto start = cursor.position;
    std::int64_t parsed = 0;
    while (!CursorAtEnd(cursor)) {
        const char digit = CursorPeek(cursor);
        if (digit < '0' || digit > '9') {
            break;
        }
        parsed = parsed * 10 + (digit - '0');
        if (parsed > 0x7FFFFFFF) {
            parsed = 0x7FFFFFFF;
        }
        CursorAdvance(cursor);
    }
    if (cursor.position == start) {
        return FormatError(error, "line %zu: %.*s must be a whole number", cursor.line, static_cast<int>(key.size()),
                           key.data());
    }
    if (parsed < low || parsed > high) {
        return FormatError(error, "line %zu: %.*s must be between %d and %d", cursor.line,
                           static_cast<int>(key.size()), key.data(), low, high);
    }
    *value = static_cast<std::int32_t>(parsed);
    return true;
}

inline auto ParseConfig(std::string_view text, Settings& result, std::string& error, std::string& unknown) noexcept
    -> bool {
    Settings parsed{};
    ConfigCursor cursor{text};
    bool seenEnabled = false;
    bool seenStatId = false;
    bool seenMode = false;
    bool seenPrimeEvils = false;
    bool seenMinionOwner = false;
    bool seenMaxBonus = false;
    bool seenMaxItems = false;
    bool seenDiagnostics = false;

    const auto duplicate = [&](bool& seen, std::string_view key) noexcept {
        if (seen) {
            FormatError(error, "line %zu: %.*s appears more than once", cursor.line, static_cast<int>(key.size()),
                        key.data());
            return true;
        }
        seen = true;
        return false;
    };

    for (;;) {
        SkipBlank(cursor, true);
        if (CursorAtEnd(cursor)) {
            break;
        }
        const auto keyStart = cursor.position;
        while (!CursorAtEnd(cursor)) {
            const char current = CursorPeek(cursor);
            const bool keyChar = (current >= 'a' && current <= 'z') || (current >= 'A' && current <= 'Z')
                              || (current >= '0' && current <= '9') || current == '_' || current == '-';
            if (!keyChar) {
                break;
            }
            CursorAdvance(cursor);
        }
        const auto key = text.substr(keyStart, cursor.position - keyStart);
        if (key.empty()) {
            return FormatError(error, "line %zu: expected a setting name", cursor.line);
        }
        SkipBlank(cursor, false);
        if (CursorPeek(cursor) != '=') {
            return FormatError(error, "line %zu: expected = after %.*s", cursor.line, static_cast<int>(key.size()),
                               key.data());
        }
        CursorAdvance(cursor);
        SkipBlank(cursor, false);

        if (key == "enabled") {
            if (duplicate(seenEnabled, key) || !ParseBool(cursor, &parsed.enabled, error, key)) {
                return false;
            }
        } else if (key == "mode") {
            if (duplicate(seenMode, key) || !ParseMode(cursor, &parsed.mode, error)) {
                return false;
            }
        } else if (key == "exclude_prime_evils") {
            if (duplicate(seenPrimeEvils, key) || !ParseBool(cursor, &parsed.excludePrimeEvils, error, key)) {
                return false;
            }
        } else if (key == "credit_minion_owner") {
            if (duplicate(seenMinionOwner, key) || !ParseBool(cursor, &parsed.creditMinionOwner, error, key)) {
                return false;
            }
        } else if (key == "diagnostics") {
            if (duplicate(seenDiagnostics, key) || !ParseBool(cursor, &parsed.diagnostics, error, key)) {
                return false;
            }
        } else if (key == "stat_id") {
            if (duplicate(seenStatId, key)
                || !ParseInteger(cursor, &parsed.statId, MinimumStatId, MaximumStatId, error, key)) {
                return false;
            }
        } else if (key == "max_bonus_picks") {
            if (duplicate(seenMaxBonus, key)
                || !ParseInteger(cursor, &parsed.maxBonusPicks, 0, MaximumBonusLimit, error, key)) {
                return false;
            }
        } else if (key == "max_items") {
            if (duplicate(seenMaxItems, key)
                || !ParseInteger(cursor, &parsed.maxItems, 0, MaximumItemLimit, error, key)) {
                return false;
            }
        } else {
            if (!unknown.empty()) {
                unknown.append(", ");
            }
            unknown.append(key.data(), key.size());
            while (!CursorAtEnd(cursor) && CursorPeek(cursor) != '\n') {
                CursorAdvance(cursor);
            }
        }

        SkipBlank(cursor, false);
        if (!CursorAtEnd(cursor) && CursorPeek(cursor) != '\n') {
            return FormatError(error, "line %zu: unexpected text after the value", cursor.line);
        }
    }

    result = parsed;
    return true;
}
// LOGIC-END

// ---------------------------------------------------------------------------
//  Native layout (D2R 3.3, D2RLoader 1.3.1 process image)
// ---------------------------------------------------------------------------
constexpr std::int32_t UnitTypePlayer  = 0;
constexpr std::int32_t UnitTypeMonster = 1;

constexpr std::size_t TreasureClassPicksOffset = 0x18;  // dword Picks in the runtime record
constexpr std::size_t MonStatsFlagsOffset      = 60;    // flags byte holding primeevil
constexpr std::uint8_t MonStatsPrimeEvilBit    = 0x80;  // bit 7
constexpr const char   SourceFile[]            = "item-quantity";

// The pick-count store inside the treasure-class roll, one instruction before
// the drop loop head at 0x4406A7.
constexpr std::uint64_t HookRva       = 0x4406A1;
constexpr std::uint64_t HookReturnRva = 0x4406A7;
constexpr auto HookStock = std::to_array<std::uint8_t>({0x89, 0x8D, 0x88, 0x00, 0x00, 0x00});

// Frame slots the stub reads, and the window that proves them.
constexpr std::int32_t  MonsterStackOffset = 0x58;  // [rsp+0x58] after the homing store at 0x440547
constexpr std::int32_t  GameStackOffset    = 0x70;  // [rsp+0x70]
constexpr std::int32_t  MaxItemsStackOffset = 0x68;  // [rsp+0x68], re-read per item at 0x441157
constexpr std::uint64_t MaxItemsCheckRva    = 0x441157;
constexpr std::uint64_t MonsterHomeRva     = 0x440547;
constexpr std::uint64_t KillerHomeRva      = 0x44055B;
constexpr std::uint64_t PicksReadRva       = 0x440650;
constexpr std::uint64_t HookWindowRva      = 0x44068D;

// mov eax,[rsp+68h] / cmp [r13],eax / jge : the per-item cap check.
constexpr auto MaxItemsCheckBytes = std::to_array<std::uint8_t>({
    0x8B, 0x44, 0x24, 0x68, 0x41, 0x39, 0x45, 0x00, 0x0F, 0x8D,
});
constexpr auto MonsterHomeBytes = std::to_array<std::uint8_t>({0x48, 0x89, 0x54, 0x24, 0x58});  // mov [rsp+58h],rdx
constexpr auto KillerHomeBytes  = std::to_array<std::uint8_t>({0x4C, 0x89, 0x45, 0x80});        // mov [rbp-80h],r8
constexpr auto PicksReadBytes   = std::to_array<std::uint8_t>({
    0x8B, 0x43, 0x18,                    // mov eax, [rbx+18h]      Picks
    0xF2, 0x0F, 0x10, 0x43, 0x26,        // movsd xmm0, [rbx+26h]
    0x99,                                // cdq
    0x33, 0xC2,                          // xor eax, edx
});
constexpr auto HookWindowBytes = std::to_array<std::uint8_t>({
    0x0F, 0x4F, 0xC8,                    // cmovg ecx, eax          max(abs(Picks), 1)
    0x44, 0x8B, 0xF2,                    // mov r14d, edx
    0x8B, 0x43, 0x2E,                    // mov eax, [rbx+2Eh]
    0x0F, 0x57, 0xFF,                    // xorps xmm7, xmm7
    0x89, 0x85, 0x94, 0x00, 0x00, 0x00,  // mov [rbp+94h], eax
    0x8B, 0xC2,                          // mov eax, edx
    0x89, 0x8D, 0x88, 0x00, 0x00, 0x00,  // mov [rbp+88h], ecx      <- the hook site
    0x49, 0xFF, 0xCE,                    // dec r14                 <- drop loop head
    0xFF, 0xC8,                          // dec eax                 (rax must survive the stub)
    0x49, 0x8B, 0xF6,                    // mov rsi, r14
    0x89, 0x44, 0x24, 0x60,              // mov [rsp+60h], eax
});

// Game functions the C++ entry calls.
constexpr std::uint64_t GetUnitTypeRva     = 0x34B9D0;  // UNITS_GetUnitType(unit)
constexpr std::uint64_t GetClassIdRva      = 0x349860;  // UNITS_GetClassId(unit, file, line)
constexpr std::uint64_t GetSeedRva         = 0x34A1E0;  // UNITS_GetSeed(unit) -> unit + 0x28
constexpr std::uint64_t GetOwnerRva        = 0x4A53C0;  // owner of a minion, or null
constexpr std::uint64_t GetMonStatsRva     = 0x976E0;   // MonStatsTxt record (dataContext, classId)
constexpr std::uint64_t ReadUnitStatRva    = 0x2F5020;  // import thunk -> D2RCore stat read

constexpr auto GetUnitTypeBody = std::to_array<std::uint8_t>({
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x85, 0xC9, 0x75, 0x1D, 0x88, 0x4C, 0x24,
    0x30, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8, 0x39, 0x9E, 0xFF, 0xFF, 0x84,
    0xC0, 0x74, 0x01, 0xCC, 0xB8, 0x06, 0x00, 0x00, 0x00, 0x48, 0x83, 0xC4,
    0x28, 0xC3, 0x8B, 0x01, 0x48, 0x83, 0xC4, 0x28, 0xC3,
});
constexpr auto GetClassIdEntry = std::to_array<std::uint8_t>({
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x85, 0xC9, 0x75, 0x1D, 0x88, 0x4C, 0x24,
    0x30, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8, 0x49, 0xCB, 0xFF, 0xFF, 0x84,
    0xC0, 0x74, 0x01, 0xCC,
});
constexpr auto GetSeedEntry = std::to_array<std::uint8_t>({
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9,
    0x75, 0x1D, 0x88, 0x4C, 0x24, 0x30, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8,
});
constexpr auto GetOwnerEntry = std::to_array<std::uint8_t>({
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0xE8, 0x02, 0x66,
    0xEA, 0xFF, 0x83, 0xF8, 0x01, 0x0F, 0x85, 0x9E, 0x00, 0x00, 0x00, 0x48,
    0x85, 0xDB, 0x74, 0x0D, 0x48, 0x8B, 0xCB, 0xE8,
});
constexpr auto GetMonStatsEntry = std::to_array<std::uint8_t>({
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48,
    0x83, 0xEC, 0x30, 0x48, 0x63, 0xF2, 0xE8, 0x99, 0x93, 0x26, 0x00, 0x48,
    0x8B, 0xF8, 0x48, 0x8B, 0xDE, 0x85, 0xF6, 0x78,
});
// The stat read is an import thunk into D2RCore, the same door the roll uses.
// D2RLoader 1.3.1 moved its import slot (disp32 9A51B303 in 1.3.0).
constexpr auto ReadUnitStatThunk = std::to_array<std::uint8_t>({0xFF, 0x25, 0xF2, 0x51, 0xB3, 0x03});

using GetUnitTypeFn  = std::int32_t(__fastcall*)(void* unit) noexcept;
using GetClassIdFn   = std::int32_t(__fastcall*)(void* unit, const char* file, std::int32_t line) noexcept;
using GetSeedFn      = std::uint32_t*(__fastcall*)(void* unit) noexcept;
using GetOwnerFn     = void*(__fastcall*)(void* unit) noexcept;
using GetMonStatsFn  = const std::uint8_t*(__fastcall*)(std::uint8_t dataContext, std::int32_t classId) noexcept;
using ReadUnitStatFn = std::int32_t(__fastcall*)(void* unit, std::int32_t statId, std::int32_t layer) noexcept;

// ---------------------------------------------------------------------------
//  State
// ---------------------------------------------------------------------------
const D2RL::PluginContext* Context{};
std::uintptr_t             Base{};
Settings                   Config{};
bool                       Installed{false};
char                       ConfigSource[48]{"built-in defaults"};
char                       InactiveReason[160]{};

GetUnitTypeFn  GetUnitType{};
GetClassIdFn   GetClassId{};
GetSeedFn      GetSeed{};
GetOwnerFn     GetOwner{};
GetMonStatsFn  GetMonStats{};
ReadUnitStatFn ReadUnitStat{};

std::atomic<bool> Active{false};

std::uint8_t* StubPage{};
constexpr std::size_t StubPageBytes  = 0x1000;
constexpr std::size_t CallSlotOffset = 0x80;
constexpr std::size_t BackSlotOffset = 0x88;

std::array<std::uint8_t, 6> HookWritten{};
bool                        HookApplied{false};

std::atomic<std::uint64_t> DropsSeen{};
std::atomic<std::uint64_t> DropsBoosted{};
std::atomic<std::uint64_t> PicksAdded{};
std::atomic<std::uint32_t> DiagnosticsBudget{};

template <typename T>
auto At(std::uint64_t rva) noexcept -> T {
    return reinterpret_cast<T>(Base + static_cast<std::uintptr_t>(rva));
}

auto ReadU32(const void* base, std::size_t offset) noexcept -> std::uint32_t {
    std::uint32_t value = 0;
    std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(value));
    return value;
}

// ---------------------------------------------------------------------------
//  Logging
// ---------------------------------------------------------------------------
void LogInfo(const char* format, ...) noexcept {
    char message[512]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    Context->LogInfo(message);
}

void LogWarn(const char* format, ...) noexcept {
    char message[512]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    Context->LogWarn(message);
}

void LogError(const char* format, ...) noexcept {
    char message[512]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    Context->LogError(message);
}

void SetInactive(const char* reason) noexcept {
    std::snprintf(InactiveReason, sizeof(InactiveReason), "%s", reason);
}

auto TakeDiagnosticsLine() noexcept -> bool {
    if (!Config.diagnostics) {
        return false;
    }
    auto budget = DiagnosticsBudget.load(std::memory_order_relaxed);
    while (budget != 0) {
        if (DiagnosticsBudget.compare_exchange_weak(budget, budget - 1, std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
//  The C++ entry the stub calls. Entered by a jump-driven stub, so it must
//  never raise: every pointer is checked and every game function is called
//  only with arguments it accepts.
// ---------------------------------------------------------------------------
extern "C" void __fastcall ApplyBonusPicks(void* monster, void* killer, const void* treasureClass,
                                           std::int32_t* counter, void* game, std::int32_t* maxItems,
                                           const void* callerArray) noexcept {
    if (!Active.load(std::memory_order_relaxed) || counter == nullptr || treasureClass == nullptr || game == nullptr) {
        return;
    }
    // Raise the per-drop item cap, but never for a caller that supplied its
    // own array: its size is what the stock cap protects.
    if (Config.maxItems > 0 && maxItems != nullptr && callerArray == nullptr && *maxItems < Config.maxItems) {
        *maxItems = Config.maxItems;
    }
    // A negative Picks is a guaranteed-drop list whose counter is used as an
    // index. Never touch those.
    const auto basePicks = static_cast<std::int32_t>(ReadU32(treasureClass, TreasureClassPicksOffset));
    if (basePicks <= 0) {
        return;
    }
    if (monster == nullptr || GetUnitType(monster) != UnitTypeMonster) {
        return;
    }
    DropsSeen.fetch_add(1, std::memory_order_relaxed);

    const auto dataContext = *(reinterpret_cast<const std::uint8_t*>(game) + 262);
    if (Config.excludePrimeEvils) {
        const auto classId = GetClassId(monster, SourceFile, 0);
        if (classId < 0) {
            return;
        }
        const auto* record = GetMonStats(dataContext, classId);
        if (record != nullptr && (record[MonStatsFlagsOffset] & MonStatsPrimeEvilBit) != 0) {
            return;
        }
    }

    void* credited = killer;
    if (credited == nullptr) {
        return;
    }
    if (GetUnitType(credited) != UnitTypePlayer) {
        if (!Config.creditMinionOwner || GetUnitType(credited) != UnitTypeMonster) {
            return;
        }
        credited = GetOwner(credited);
        if (credited == nullptr || GetUnitType(credited) != UnitTypePlayer) {
            return;
        }
    }

    const auto stat = ReadUnitStat(credited, Config.statId, 0);
    if (stat <= 0) {
        return;
    }

    std::uint32_t roll = 0;
    if (Config.mode != Mode::Flat) {
        const auto* seed = GetSeed(monster);
        if (seed == nullptr) {
            return;
        }
        roll = SeedRoll(seed[0], seed[1]);
    }
    const auto extra = BonusPicksFor(Config, basePicks, stat, roll);
    if (extra <= 0) {
        return;
    }

    *counter += extra;
    DropsBoosted.fetch_add(1, std::memory_order_relaxed);
    PicksAdded.fetch_add(static_cast<std::uint64_t>(extra), std::memory_order_relaxed);
    if (TakeDiagnosticsLine()) {
        LogInfo("ItemQuantity: %s, stat %d = %d on the kill credit, base picks %d, roll %u, +%d pick(s) "
                "(counter now %d, item cap %d).",
                ModeName(Config.mode), Config.statId, stat, basePicks, roll, extra, *counter,
                maxItems != nullptr ? *maxItems : -1);
    }
}

// ---------------------------------------------------------------------------
//  Image checks
// ---------------------------------------------------------------------------
auto CheckExact(std::uint64_t rva, const std::uint8_t* bytes, std::size_t size, const char* name) noexcept -> bool {
    if (Context->CheckExpectedBytes(rva, bytes, static_cast<std::uint32_t>(size))) {
        return true;
    }
    LogError("ItemQuantity: %s at RVA 0x%llX does not match D2RLoader 1.3.1.", name,
             static_cast<unsigned long long>(rva));
    return false;
}

// A function the plugin only calls may already carry another plugin's inline
// hook at its entry. The loader tracks those, and calling through one is fine.
auto CheckCallable(std::uint64_t rva, const std::uint8_t* bytes, std::size_t size, const char* name) noexcept -> bool {
    if (Context->CheckExpectedBytes(rva, bytes, static_cast<std::uint32_t>(size))) {
        return true;
    }

    const D2RL::DiagnosticsService* diagnostics = nullptr;
    if (Context->QueryService(&diagnostics)
            == D2RL::ServiceQueryResult::Success
        && D2RL::HasDiagnosticsServiceField(diagnostics, D2RL::DiagnosticsServiceRequiredSize)
        && diagnostics->queryHookStatus != nullptr) {
        D2RL::Diagnostics::HookQuery query{
            .structSize   = D2RL::Diagnostics::HookQuerySize,
            .flags        = 0,
            .rva          = rva,
            .expected     = bytes,
            .expectedSize = static_cast<std::uint32_t>(size),
            .reserved     = 0,
        };
        D2RL::Diagnostics::HookStatus status{
            .structSize = D2RL::Diagnostics::HookStatusSize,
        };
        if (diagnostics->queryHookStatus(Context, &query, &status) == D2RL::Diagnostics::Result::Success
            && status.state == D2RL::Diagnostics::ModificationState::Tracked
            && status.kind == D2RL::Diagnostics::ModificationKind::InlineHook) {
            LogInfo("ItemQuantity: %s at RVA 0x%llX is hooked by another plugin, calling through it.", name,
                    static_cast<unsigned long long>(rva));
            return true;
        }
    }

    LogError("ItemQuantity: %s at RVA 0x%llX does not match D2RLoader 1.3.1.", name,
             static_cast<unsigned long long>(rva));
    return false;
}

auto ValidateImage() noexcept -> bool {
    return CheckExact(HookWindowRva, HookWindowBytes.data(), HookWindowBytes.size(), "the pick-count store window")
        && CheckExact(PicksReadRva, PicksReadBytes.data(), PicksReadBytes.size(), "the Picks read")
        && CheckExact(MonsterHomeRva, MonsterHomeBytes.data(), MonsterHomeBytes.size(), "the monster argument store")
        && CheckExact(KillerHomeRva, KillerHomeBytes.data(), KillerHomeBytes.size(), "the killer argument store")
        && CheckExact(MaxItemsCheckRva, MaxItemsCheckBytes.data(), MaxItemsCheckBytes.size(), "the item cap check")
        && CheckExact(ReadUnitStatRva, ReadUnitStatThunk.data(), ReadUnitStatThunk.size(), "the stat-read thunk")
        && CheckCallable(GetUnitTypeRva, GetUnitTypeBody.data(), GetUnitTypeBody.size(), "UNITS_GetUnitType")
        && CheckCallable(GetClassIdRva, GetClassIdEntry.data(), GetClassIdEntry.size(), "UNITS_GetClassId")
        && CheckCallable(GetSeedRva, GetSeedEntry.data(), GetSeedEntry.size(), "UNITS_GetSeed")
        && CheckCallable(GetOwnerRva, GetOwnerEntry.data(), GetOwnerEntry.size(), "the minion owner resolver")
        && CheckCallable(GetMonStatsRva, GetMonStatsEntry.data(), GetMonStatsEntry.size(), "the MonStats record lookup");
}

void ResolveGameFunctions() noexcept {
    GetUnitType  = At<GetUnitTypeFn>(GetUnitTypeRva);
    GetClassId   = At<GetClassIdFn>(GetClassIdRva);
    GetSeed      = At<GetSeedFn>(GetSeedRva);
    GetOwner     = At<GetOwnerFn>(GetOwnerRva);
    GetMonStats  = At<GetMonStatsFn>(GetMonStatsRva);
    ReadUnitStat = At<ReadUnitStatFn>(ReadUnitStatRva);
}

// ---------------------------------------------------------------------------
//  The stub
// ---------------------------------------------------------------------------
auto AllocateStubPage() noexcept -> std::uint8_t* {
    const auto* image = reinterpret_cast<const std::uint8_t*>(Base);
    const auto  ntOffset = static_cast<std::size_t>(ReadU32(image, 0x3C));
    const auto  imageSize = ReadU32(image, ntOffset + 0x50);

    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    const std::uintptr_t granularity =
        systemInfo.dwAllocationGranularity != 0 ? systemInfo.dwAllocationGranularity : 0x10000;
    const auto alignUp = [granularity](std::uintptr_t value) noexcept {
        return (value + granularity - 1) & ~(granularity - 1);
    };

    const std::uintptr_t limit = Base + HookRva + 0x7FF00000ULL;
    std::uintptr_t address = alignUp(Base + imageSize);
    while (address + StubPageBytes < limit) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) == 0) {
            break;
        }
        const auto regionEnd = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (info.State == MEM_FREE && address + StubPageBytes <= regionEnd) {
            if (auto* page =
                    VirtualAlloc(reinterpret_cast<LPVOID>(address), StubPageBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
                return static_cast<std::uint8_t*>(page);
            }
        }
        const auto next = alignUp(regionEnd);
        if (next <= address) {
            break;
        }
        address = next;
    }
    return nullptr;
}

//  mov [rbp+88h], ecx          replay the stolen store
//  push rax/rcx/rdx/r8/r9/r10/r11/rax      rax is live across the site
//  mov rcx, [rsp+98h]          monster ([rsp+58h] plus the 0x40 of pushes)
//  mov rdx, [rbp-80h]          killer
//  mov r8, rbx                 treasure class record
//  lea r9, [rbp+88h]           the pick counter
//  mov rax, [rsp+0B0h]         game ([rsp+70h] plus 0x40)
//  lea r10, [rsp+0A8h]         the per-drop item cap ([rsp+68h] plus 0x40)
//  mov r11, [rbp-60h]          the caller's item array, null on monster drops
//  sub rsp, 40h                shadow space plus three stack arguments
//  mov [rsp+20h], rax / [rsp+28h], r10 / [rsp+30h], r11
//  call [rip -> ApplyBonusPicks]
//  add rsp, 40h / pops / jmp [rip -> 4406A7]
auto BuildStub() noexcept -> bool {
    StubPage = AllocateStubPage();
    if (StubPage == nullptr) {
        LogError("ItemQuantity: no free page within jump range of the game image.");
        return false;
    }
    std::memset(StubPage, 0xCC, StubPageBytes);

    std::array<std::uint8_t, 0x6B> stub{{
        0x89, 0x8D, 0x88, 0x00, 0x00, 0x00,              // mov [rbp+88h], ecx
        0x50,                                            // push rax
        0x51,                                            // push rcx
        0x52,                                            // push rdx
        0x41, 0x50,                                      // push r8
        0x41, 0x51,                                      // push r9
        0x41, 0x52,                                      // push r10
        0x41, 0x53,                                      // push r11
        0x50,                                            // push rax (alignment)
        0x48, 0x8B, 0x8C, 0x24, 0x98, 0x00, 0x00, 0x00,  // mov rcx, [rsp+98h]
        0x48, 0x8B, 0x55, 0x80,                          // mov rdx, [rbp-80h]
        0x4C, 0x8B, 0xC3,                                // mov r8, rbx
        0x4C, 0x8D, 0x8D, 0x88, 0x00, 0x00, 0x00,        // lea r9, [rbp+88h]
        0x48, 0x8B, 0x84, 0x24, 0xB0, 0x00, 0x00, 0x00,  // mov rax, [rsp+0B0h]
        0x4C, 0x8D, 0x94, 0x24, 0xA8, 0x00, 0x00, 0x00,  // lea r10, [rsp+0A8h]
        0x4C, 0x8B, 0x5D, 0xA0,                          // mov r11, [rbp-60h]
        0x48, 0x83, 0xEC, 0x40,                          // sub rsp, 40h
        0x48, 0x89, 0x44, 0x24, 0x20,                    // mov [rsp+20h], rax
        0x4C, 0x89, 0x54, 0x24, 0x28,                    // mov [rsp+28h], r10
        0x4C, 0x89, 0x5C, 0x24, 0x30,                    // mov [rsp+30h], r11
        0xFF, 0x15, 0x00, 0x00, 0x00, 0x00,              // call qword [rip+disp]
        0x48, 0x83, 0xC4, 0x40,                          // add rsp, 40h
        0x58,                                            // pop rax
        0x41, 0x5B,                                      // pop r11
        0x41, 0x5A,                                      // pop r10
        0x41, 0x59,                                      // pop r9
        0x41, 0x58,                                      // pop r8
        0x5A,                                            // pop rdx
        0x59,                                            // pop rcx
        0x58,                                            // pop rax
        0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,              // jmp qword [rip+disp]
    }};
    constexpr std::size_t CallDispOffset = 0x4F + 2;
    constexpr std::size_t CallEnd        = 0x4F + 6;
    constexpr std::size_t BackDispOffset = 0x65 + 2;
    constexpr std::size_t BackEnd        = 0x65 + 6;
    static_assert(CallEnd <= 0x6B && BackEnd == 0x6B, "Stub layout changed.");
    static_assert(BackEnd <= CallSlotOffset, "The stub must not run into its target slots.");

    const auto callDisp = static_cast<std::int32_t>(CallSlotOffset - CallEnd);
    const auto backDisp = static_cast<std::int32_t>(BackSlotOffset - BackEnd);
    std::memcpy(stub.data() + CallDispOffset, &callDisp, sizeof(callDisp));
    std::memcpy(stub.data() + BackDispOffset, &backDisp, sizeof(backDisp));
    std::memcpy(StubPage, stub.data(), stub.size());

    const auto entry = reinterpret_cast<std::uint64_t>(&ApplyBonusPicks);
    const auto back  = static_cast<std::uint64_t>(Base + HookReturnRva);
    std::memcpy(StubPage + CallSlotOffset, &entry, sizeof(entry));
    std::memcpy(StubPage + BackSlotOffset, &back, sizeof(back));

    DWORD previous = 0;
    if (!VirtualProtect(StubPage, StubPageBytes, PAGE_EXECUTE_READ, &previous)) {
        LogError("ItemQuantity: could not make the stub page executable.");
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), StubPage, StubPageBytes);
    return true;
}

auto PatchHook() noexcept -> bool {
    const auto from = Base + static_cast<std::uintptr_t>(HookRva) + 5;
    const auto to   = reinterpret_cast<std::uintptr_t>(StubPage);
    const auto rel  = static_cast<std::int64_t>(to) - static_cast<std::int64_t>(from);
    if (rel < INT32_MIN || rel > INT32_MAX) {
        LogError("ItemQuantity: the stub page is out of reach of RVA 0x%llX.", static_cast<unsigned long long>(HookRva));
        return false;
    }
    HookWritten[0] = 0xE9;
    const auto rel32 = static_cast<std::int32_t>(rel);
    std::memcpy(HookWritten.data() + 1, &rel32, sizeof(rel32));
    HookWritten[5] = 0x90;

    if (!Context->PatchBytes(HookRva, HookStock.data(), 6, HookWritten.data(), 6)) {
        LogError("ItemQuantity: the loader refused the hook at RVA 0x%llX.", static_cast<unsigned long long>(HookRva));
        return false;
    }
    HookApplied = true;
    if (std::memcmp(reinterpret_cast<const std::uint8_t*>(Base + static_cast<std::uintptr_t>(HookRva)),
                    HookWritten.data(), 6)
        != 0) {
        LogError("ItemQuantity: RVA 0x%llX does not hold the bytes that were written.",
                 static_cast<unsigned long long>(HookRva));
        return false;
    }
    return true;
}

void RestoreHook() noexcept {
    if (!HookApplied) {
        return;
    }
    if (!Context->PatchBytes(HookRva, HookWritten.data(), 6, HookStock.data(), 6)) {
        LogError("ItemQuantity: could not restore the hook at RVA 0x%llX.", static_cast<unsigned long long>(HookRva));
        return;
    }
    HookApplied = false;
}

void Install() noexcept {
    if (!ValidateImage()) {
        SetInactive("the game image does not match D2RLoader 1.3.1");
        return;
    }
    ResolveGameFunctions();
    if (!BuildStub()) {
        SetInactive("no stub page could be prepared");
        return;
    }
    Active.store(true, std::memory_order_relaxed);
    if (!PatchHook()) {
        Active.store(false, std::memory_order_relaxed);
        RestoreHook();
        SetInactive("the pick-count store could not be hooked");
        return;
    }
    Installed = true;
}

// ---------------------------------------------------------------------------
//  Configuration
// ---------------------------------------------------------------------------
auto LoadConfig() noexcept -> bool {
    Config = Settings{};
    std::string error;
    std::string unknown;
    if (!ParseConfig(DefaultConfigToml, Config, error, unknown)) {
        LogError("ItemQuantity: the built-in default configuration does not parse (%s).", error.c_str());
        return false;
    }
    std::snprintf(ConfigSource, sizeof(ConfigSource), "%s", "built-in defaults");

    if (!Context->EnsureConfig(DefaultConfigToml)) {
        LogWarn("ItemQuantity: could not create the default config, using built-in defaults.");
        return true;
    }

    std::string buffer(16384, '\0');
    std::uint32_t required = 0;
    if (!Context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size()), &required)) {
        if (required > buffer.size()) {
            buffer.assign(required, '\0');
        }
        if (required > buffer.size()
            || !Context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size()), &required)) {
            LogWarn("ItemQuantity: could not read the config, using built-in defaults.");
            return true;
        }
    }

    Settings parsed{};
    error.clear();
    unknown.clear();
    if (!ParseConfig(std::string_view(buffer.c_str()), parsed, error, unknown)) {
        LogError("ItemQuantity: config rejected (%s). Nothing is changed so a setting you meant is not silently "
                 "ignored; fix the file and restart.",
                 error.c_str());
        return false;
    }
    if (!unknown.empty()) {
        LogWarn("ItemQuantity: ignoring unknown config setting(s): %s.", unknown.c_str());
    }
    Config = parsed;
    std::snprintf(ConfigSource, sizeof(ConfigSource), "%s", "config file");
    return true;
}

// ---------------------------------------------------------------------------
//  Console
// ---------------------------------------------------------------------------
auto OnOff(bool value) noexcept -> const char* {
    return value ? "on" : "off";
}

auto StatusCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept
    -> D2RL::ConsoleCommandResult {
    if (command == nullptr || command->plugin == nullptr) {
        return D2RL::ConsoleCommandResult::Failed;
    }

    char line[512]{};
    std::snprintf(line, sizeof(line),
                  "Item Quantity %s (%s): stat %d, mode %s, prime evils %s, minion owner %s, bonus cap %d, "
                  "item cap %d, hook %s.%s%s",
                  PluginVersion, ConfigSource, Config.statId, ModeName(Config.mode),
                  Config.excludePrimeEvils ? "excluded" : "included", OnOff(Config.creditMinionOwner),
                  Config.maxBonusPicks, Config.maxItems, OnOff(Installed),
                  InactiveReason[0] != '\0' ? " Inactive: " : "", InactiveReason);
    command->plugin->WriteConsoleMessage(line);

    std::snprintf(line, sizeof(line), "Drops examined: %llu. Drops that gained picks: %llu. Picks added: %llu.",
                  static_cast<unsigned long long>(DropsSeen.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(DropsBoosted.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(PicksAdded.load(std::memory_order_relaxed)));
    command->plugin->WriteConsoleMessage(line);
    return D2RL::ConsoleCommandResult::Handled;
}

void ResetState() noexcept {
    Config    = Settings{};
    Installed = false;
    InactiveReason[0] = '\0';
    HookApplied = false;
    Active.store(false, std::memory_order_relaxed);
    DropsSeen.store(0, std::memory_order_relaxed);
    DropsBoosted.store(0, std::memory_order_relaxed);
    PicksAdded.store(0, std::memory_order_relaxed);
    DiagnosticsBudget.store(64, std::memory_order_relaxed);
}

}  // namespace
}  // namespace CelestialRayOne::ItemQuantity

namespace {

constexpr D2RL::PluginInfo Info{
    .infoSize    = D2RL::PluginInfoSize,
    .abiVersion  = D2RL_PLUGIN_ABI_VERSION,
    .id          = "celestialrayone.item-quantity",
    .name        = "Item Quantity",
    .version     = "1.1.1",
    .author      = "CelestialRayOne",
    .description = "A stat on the killer adds extra picks to the monster's own treasure class.",
    .flags       = D2RL::PluginFlags::Server | D2RL::PluginFlags::NativeHooks,
};

}  // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
    return &Info;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
    using namespace CelestialRayOne::ItemQuantity;

    if (!D2RL::HasContext(context) || context->exeBase == 0) {
        return false;
    }
    Context = context;
    Base    = context->exeBase;
    ResetState();

    if (!context->RegisterConsoleCommand("itemquantity", StatusCommand,
                                         "Show Item Quantity settings, state and counters.")) {
        LogWarn("ItemQuantity: the itemquantity console command could not be registered.");
    }

    if (!LoadConfig()) {
        Config.enabled = false;
        SetInactive("the config was rejected, see the plugin log");
        return true;
    }
    if (!Config.enabled) {
        SetInactive("disabled by config");
        LogInfo("Item Quantity %s loaded disabled by config; the game runs stock.", PluginVersion);
        return true;
    }

    Install();

    LogInfo("Item Quantity %s by CelestialRayOne: stat %d, mode %s, prime evils %s, minion owner %s, bonus cap %d, "
            "item cap %d, hook %s, config %s, build %s.",
            PluginVersion, Config.statId, ModeName(Config.mode),
            Config.excludePrimeEvils ? "excluded" : "included", OnOff(Config.creditMinionOwner), Config.maxBonusPicks,
            Config.maxItems, OnOff(Installed), ConfigSource,
            context->buildName != nullptr ? context->buildName : "unknown");
    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    using namespace CelestialRayOne::ItemQuantity;
    if (Context == nullptr) {
        return;
    }
    Active.store(false, std::memory_order_relaxed);
    RestoreHook();
}
