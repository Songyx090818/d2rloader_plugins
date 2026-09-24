// =============================================================================
//  Charm Dedup  -  celestialrayone.charm-dedup
//
//  Groups of charms, matched by item code, of which only one charm can be
//  active at a time. The other charms of the group stay where they are, give
//  no bonuses, and are drawn with the red background of an unusable item.
//
//  Built for D2R 3.3 under D2RLoader 1.3.1. Every address below was read out
//  of the dumped D2RLoader 1.3.1 image or D2RCore.dll 1.3.1 and disassembled
//  before being relied on. 1.3.1 moved both thunks, their slots and the two
//  D2RCore exports; the export bodies are the 1.3.0 code, rebuilt (the only
//  change is where D2RCore keeps its own inactive-charm tint flag, which the
//  plugin never touches). Byte witnesses guard all of them, so a different
//  build refuses cleanly and the game runs stock.
//
// -----------------------------------------------------------------------------
//  WHERE A CHARM IS SWITCHED ON AND OFF
// -----------------------------------------------------------------------------
//  D2RLoader moved the charm predicate into D2RCore.dll. All 12 game callers
//  of the stock ITEMS_IsCharmUsable (0x36AE00, now dead) call the import thunk
//  at 0x3E2B61C (jmp [0x3E2ACA0]), which lands on D2RCore!IsCharmUsable:
//
//      IsCharmUsable(item, owner) = item is valid
//                                && item type 13 (charm)
//                                && its page is allowed by the Charm
//                                   Inventory policy (0 and/or custom page 6)
//                                && item requirements are met
//
//  Nothing inside D2RCore calls it directly (no call, no RIP reference), so
//  the thunk is the only door.
//
//  The game attaches a stored charm's stat list to its owner only when that
//  predicate says yes and the stat list is not attached yet:
//      server  0x470CA0 (inventory refresh, run after every item transaction)
//      client  0x1C9610 (client inventory refresh), and the item packet
//              handlers 0x1C37F0, 0x1C4300, 0x1C5060
//  and the client detaches a charm it removes only when the predicate still
//  says yes (0x1C60A0). Stats, skills and item auras all follow the stat list.
//
//  THE RULE, answered for a charm whose code is in a group:
//    1. its stat list is attached to its owner   -> stock answer (stays on)
//    2. another charm of the group is attached   -> no
//    3. an eligible charm of the group has a
//       lower unit id                            -> no
//    4. otherwise                                -> stock answer
//  "Eligible" is D2RCore's own answer for that charm, so a charm in the stash,
//  the cube, on the cursor or with unmet requirements never blocks another.
//
//  Rule 1 means the predicate never turns an active charm off. The game never
//  has to detach anything outside its own paths, and the client removal path
//  above still detaches every charm it attached. No write of any kind is made
//  to an item, a unit or a stat list: the engine does all of that itself.
//  Rule 3 is order independent (unit ids are the same on client and server),
//  so when nothing is active both sides pick the same charm.
//
// -----------------------------------------------------------------------------
//  RED BACKGROUND
// -----------------------------------------------------------------------------
//  The inventory item renderer 0x2C3570 calls the thunk at 0x3E2B298
//  (jmp [0x3E2A808]) -> D2RCore!CheckInventoryItemRequirementsForDisplay at
//  0x2C3774 and takes its red, requirements-not-met path at 0x2C377B when the
//  result is 0. It is the only caller. The plugin returns 0 there for a
//  grouped charm that is eligible but switched off by the rule. D2RCore's own
//  inactive-charm tint flag is left untouched, so the stock red is drawn.
//
// -----------------------------------------------------------------------------
//  HOW THE THUNKS ARE TAKEN
// -----------------------------------------------------------------------------
//  Each 6-byte thunk (FF 25 rel32) becomes jmp rel32 + nop to a jump stub in a
//  page the plugin allocates within reach of the image; the stub jumps to the
//  hook. The hooks call the D2RCore exports resolved by name, after checking
//  that each thunk slot holds exactly that export and that the export bodies
//  are the 1.3.1 bodies. Nothing in D2RCore.dll is modified.
//
//  Console command "charmdedup" shows the settings, what is installed and
//  counters. Settings: d2rloader/config/celestialrayone.charm-dedup.toml
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

namespace CelestialRayOne::CharmDedup {
namespace {

constexpr char PluginVersion[] = "1.0.1";

// ---------------------------------------------------------------------------
//  Default configuration. EnsureConfig writes this text on first load, and it
//  is the plugin's documentation as shipped.
// ---------------------------------------------------------------------------
constexpr const char DefaultConfigToml[] = R"TOML(# celestialrayone.charm-dedup
#
# Charm Dedup
#   Groups of charms of which only one can be active at a time. Every other
#   charm of the group stays where it is, gives no bonuses (no stats, skills
#   or auras) and gets the red background of an item you cannot use. As soon
#   as the active charm leaves, another charm of the group becomes active.
#
#   Charms are matched by item code, the "code" column of misc.txt (armor.txt
#   and weapons.txt codes work too). Class ids are never used, so adding or
#   removing rows in the txt files cannot break a group.
#
# Changes are read when the plugin loads. Restart the game after editing.
# Built for D2RLoader 1.3.1 (Diablo II: Resurrected 3.3). On any other build
# the byte checks fail and the plugin loads without changing the game.

# Master switch. false loads the plugin without touching the game.
enabled = true

# -----------------------------------------------------------------------------
# groups
#
# A list of groups. Each group is a list of item codes, and at most one charm
# whose code is in the group is active at a time.
#
#   ["e30"]                  one code: e30 charms dedup against each other
#   ["c11", "c12", "c13"]    several codes: every c11, c12 and c13 charm
#                            dedups against all of the others
#
# Which charm is the active one
#   1. A charm that is already active stays active. Picking up, buying or
#      moving in another charm of the same group never switches it off.
#   2. When none of them is active (entering the game, or the active one was
#      moved out, dropped or sold), the one with the lowest unit id becomes
#      active. Items that load with the character come first, in save order,
#      then items obtained later in the game, in the order they appeared.
#
# Only charms that would work on their own count. A charm in the stash, in
# the cube, on the cursor, in a page where Charm Inventory disables charms
# (allow_inventory_charms), or whose requirements you do not meet never
# blocks another charm.
#
# Codes have 1 to 4 characters. A code may appear in one group only.
# Up to 64 groups, 64 codes per group and 256 codes in total.
groups = [
    ["c11", "c12"],
]

# -----------------------------------------------------------------------------
# red_background
#
# Draws the charms switched off by a group with the red background of an item
# whose requirements are not met. false keeps the dedup but draws them
# normally.
red_background = true

# Writes a line to the plugin log for the first 64 charms switched off.
# Keep false for normal play. The console command "charmdedup" always shows
# the settings, what is installed and the counters.
diagnostics = false
)TOML";

// LOGIC-BEGIN
// ---------------------------------------------------------------------------
//  Settings and item codes
// ---------------------------------------------------------------------------
constexpr std::size_t  MaximumGroups        = 64;
constexpr std::size_t  MaximumCodesPerGroup = 64;
constexpr std::size_t  MaximumCodes         = 256;
constexpr std::int32_t NoGroup              = -1;

struct CodeEntry {
    std::uint32_t code;
    std::int32_t  group;
};

struct Settings {
    bool                                enabled{true};
    bool                                redBackground{true};
    bool                                diagnostics{false};
    std::array<CodeEntry, MaximumCodes> codes{};
    std::size_t                         codeCount{0};
    std::size_t                         groupCount{0};
};

// D2 packs a base item code into four bytes, space padded: "c11" -> 'c','1','1',' '.
inline auto PackItemCode(std::string_view text, std::uint32_t* packed) noexcept -> bool {
    if (text.empty() || text.size() > 4) {
        return false;
    }
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        std::uint32_t byte = 0x20;
        if (index < text.size()) {
            byte = static_cast<std::uint8_t>(text[index]);
            if (byte <= 0x20 || byte >= 0x7F || byte == '"' || byte == '\\' || byte == '#') {
                return false;
            }
        }
        value |= byte << (8 * index);
    }
    *packed = value;
    return true;
}

inline void UnpackItemCode(std::uint32_t code, char output[5]) noexcept {
    for (std::size_t index = 0; index < 4; ++index) {
        const auto byte = static_cast<char>((code >> (8 * index)) & 0xFF);
        output[index] = (byte >= 0x21 && byte <= 0x7E) ? byte : ' ';
    }
    output[4] = '\0';
    for (std::size_t index = 4; index > 0 && output[index - 1] == ' '; --index) {
        output[index - 1] = '\0';
    }
}

inline auto FindGroup(const Settings& settings, std::uint32_t code) noexcept -> std::int32_t {
    for (std::size_t index = 0; index < settings.codeCount; ++index) {
        if (settings.codes[index].code == code) {
            return settings.codes[index].group;
        }
    }
    return NoGroup;
}

// ---------------------------------------------------------------------------
//  Configuration parser. Accepts the subset of TOML the default file uses:
//  key = true|false, and groups = an array of arrays of strings that may span
//  lines. Anything it does not understand is an error, never a silent default.
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

// Skips blanks and comments. Line breaks are skipped only when allowed.
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

inline auto ParseBool(ConfigCursor& cursor, bool* value, std::string& error, const char* key) noexcept -> bool {
    const auto rest = cursor.text.substr(cursor.position);
    const auto isWordEnd = [](std::string_view text, std::size_t at) noexcept {
        if (at >= text.size()) {
            return true;
        }
        const char next = text[at];
        return next == ' ' || next == '\t' || next == '\r' || next == '\n' || next == '#';
    };
    if (rest.substr(0, 4) == "true" && isWordEnd(rest, 4)) {
        *value = true;
        cursor.position += 4;
        return true;
    }
    if (rest.substr(0, 5) == "false" && isWordEnd(rest, 5)) {
        *value = false;
        cursor.position += 5;
        return true;
    }
    return FormatError(error, "line %zu: %s must be true or false", cursor.line, key);
}

inline auto ParseGroups(ConfigCursor& cursor, Settings& settings, std::string& error) noexcept -> bool {
    if (CursorPeek(cursor) != '[') {
        return FormatError(error, "line %zu: groups must be a list of groups, for example [[\"c11\", \"c12\"]]", cursor.line);
    }
    CursorAdvance(cursor);
    SkipBlank(cursor, true);

    while (CursorPeek(cursor) != ']') {
        if (CursorAtEnd(cursor)) {
            return FormatError(error, "line %zu: the groups list is not closed with ]", cursor.line);
        }
        if (CursorPeek(cursor) != '[') {
            return FormatError(error, "line %zu: each group must be a list of item codes in [ ]", cursor.line);
        }
        if (settings.groupCount >= MaximumGroups) {
            return FormatError(error, "line %zu: more than %zu groups", cursor.line, MaximumGroups);
        }
        const auto group = static_cast<std::int32_t>(settings.groupCount);
        const auto groupLine = cursor.line;
        std::size_t codesInGroup = 0;
        CursorAdvance(cursor);
        SkipBlank(cursor, true);

        while (CursorPeek(cursor) != ']') {
            if (CursorAtEnd(cursor)) {
                return FormatError(error, "line %zu: a group is not closed with ]", groupLine);
            }
            if (CursorPeek(cursor) != '"') {
                return FormatError(error, "line %zu: item codes must be written in double quotes", cursor.line);
            }
            CursorAdvance(cursor);
            const auto start = cursor.position;
            while (!CursorAtEnd(cursor) && CursorPeek(cursor) != '"' && CursorPeek(cursor) != '\n') {
                CursorAdvance(cursor);
            }
            if (CursorPeek(cursor) != '"') {
                return FormatError(error, "line %zu: an item code is missing its closing quote", cursor.line);
            }
            const auto text = cursor.text.substr(start, cursor.position - start);
            CursorAdvance(cursor);

            std::uint32_t packed = 0;
            if (!PackItemCode(text, &packed)) {
                return FormatError(error, "line %zu: \"%.*s\" is not an item code (1 to 4 characters, no spaces)",
                                   cursor.line, static_cast<int>(text.size() > 16 ? 16 : text.size()), text.data());
            }
            if (FindGroup(settings, packed) != NoGroup) {
                return FormatError(error, "line %zu: item code \"%.*s\" appears more than once", cursor.line,
                                   static_cast<int>(text.size()), text.data());
            }
            if (settings.codeCount >= MaximumCodes) {
                return FormatError(error, "line %zu: more than %zu item codes", cursor.line, MaximumCodes);
            }
            if (codesInGroup >= MaximumCodesPerGroup) {
                return FormatError(error, "line %zu: a group has more than %zu item codes", cursor.line, MaximumCodesPerGroup);
            }
            settings.codes[settings.codeCount++] = CodeEntry{packed, group};
            ++codesInGroup;

            SkipBlank(cursor, true);
            if (CursorPeek(cursor) == ',') {
                CursorAdvance(cursor);
                SkipBlank(cursor, true);
            } else if (CursorPeek(cursor) != ']') {
                return FormatError(error, "line %zu: expected , or ] after an item code", cursor.line);
            }
        }
        CursorAdvance(cursor);
        if (codesInGroup == 0) {
            return FormatError(error, "line %zu: a group needs at least one item code", groupLine);
        }
        ++settings.groupCount;

        SkipBlank(cursor, true);
        if (CursorPeek(cursor) == ',') {
            CursorAdvance(cursor);
            SkipBlank(cursor, true);
        } else if (CursorPeek(cursor) != ']') {
            return FormatError(error, "line %zu: expected , or ] after a group", cursor.line);
        }
    }
    CursorAdvance(cursor);
    return true;
}

// Parses the whole file into result. On failure result is untouched and error
// names the line. Unknown keys are reported in unknown and otherwise ignored.
inline auto ParseConfig(std::string_view text, Settings& result, std::string& error, std::string& unknown) noexcept -> bool {
    Settings parsed{};
    ConfigCursor cursor{text};
    bool seenEnabled = false;
    bool seenGroups = false;
    bool seenRed = false;
    bool seenDiagnostics = false;

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
            return FormatError(error, "line %zu: expected = after %.*s", cursor.line, static_cast<int>(key.size()), key.data());
        }
        CursorAdvance(cursor);
        SkipBlank(cursor, false);

        const auto duplicate = [&](bool& seen) noexcept {
            if (seen) {
                return true;
            }
            seen = true;
            return false;
        };

        if (key == "enabled") {
            if (duplicate(seenEnabled)) {
                return FormatError(error, "line %zu: enabled appears more than once", cursor.line);
            }
            if (!ParseBool(cursor, &parsed.enabled, error, "enabled")) {
                return false;
            }
        } else if (key == "red_background") {
            if (duplicate(seenRed)) {
                return FormatError(error, "line %zu: red_background appears more than once", cursor.line);
            }
            if (!ParseBool(cursor, &parsed.redBackground, error, "red_background")) {
                return false;
            }
        } else if (key == "diagnostics") {
            if (duplicate(seenDiagnostics)) {
                return FormatError(error, "line %zu: diagnostics appears more than once", cursor.line);
            }
            if (!ParseBool(cursor, &parsed.diagnostics, error, "diagnostics")) {
                return false;
            }
        } else if (key == "groups") {
            if (duplicate(seenGroups)) {
                return FormatError(error, "line %zu: groups appears more than once", cursor.line);
            }
            if (!ParseGroups(cursor, parsed, error)) {
                return false;
            }
        } else {
            if (!unknown.empty()) {
                unknown.append(", ");
            }
            unknown.append(key.data(), key.size());
            // Skip the value: a bracketed list may span lines, anything else ends with the line.
            if (CursorPeek(cursor) == '[') {
                int depth = 0;
                bool quoted = false;
                while (!CursorAtEnd(cursor)) {
                    const char current = CursorPeek(cursor);
                    if (quoted) {
                        quoted = current != '"';
                    } else if (current == '"') {
                        quoted = true;
                    } else if (current == '#') {
                        while (!CursorAtEnd(cursor) && CursorPeek(cursor) != '\n') {
                            CursorAdvance(cursor);
                        }
                        continue;
                    } else if (current == '[') {
                        ++depth;
                    } else if (current == ']' && --depth == 0) {
                        CursorAdvance(cursor);
                        break;
                    }
                    CursorAdvance(cursor);
                }
            } else {
                while (!CursorAtEnd(cursor) && CursorPeek(cursor) != '\n') {
                    CursorAdvance(cursor);
                }
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

// ---------------------------------------------------------------------------
//  The rule for a charm whose code is in a group and that D2RCore already
//  considers usable. Env supplies the native queries, so the rule is testable.
// ---------------------------------------------------------------------------
enum class Verdict : std::uint8_t {
    AlreadyActive,    // its stat list is attached to the owner: keep the stock answer
    Winner,           // no active charm in the group and no eligible older one
    BlockedByActive,  // another charm of the group is active
    BlockedByOlder,   // an eligible charm of the group has a lower unit id
};

inline auto IsBlocked(Verdict verdict) noexcept -> bool {
    return verdict == Verdict::BlockedByActive || verdict == Verdict::BlockedByOlder;
}

template <typename Env>
inline auto EvaluateGroupedCharm(Env& env, void* item, void* owner, std::int32_t group, std::uint32_t* blockerId) noexcept -> Verdict {
    if (env.IsAttached(item)) {
        return Verdict::AlreadyActive;
    }
    const std::uint32_t itemId = env.UnitId(item);
    bool          older   = false;
    std::uint32_t olderId = 0;
    std::size_t   visited = 0;
    for (void* other = env.FirstItem(owner); other != nullptr && visited < env.MaximumItems();
         other = env.NextItem(other), ++visited) {
        if (other == item || !env.IsItem(other) || env.GroupOf(other) != group) {
            continue;
        }
        if (env.IsAttached(other)) {
            if (blockerId != nullptr) {
                *blockerId = env.UnitId(other);
            }
            return Verdict::BlockedByActive;
        }
        if (!older) {
            const std::uint32_t otherId = env.UnitId(other);
            if (otherId < itemId && env.IsEligible(other, owner)) {
                older   = true;
                olderId = otherId;
            }
        }
    }
    if (older) {
        if (blockerId != nullptr) {
            *blockerId = olderId;
        }
        return Verdict::BlockedByOlder;
    }
    return Verdict::Winner;
}
// LOGIC-END

// ---------------------------------------------------------------------------
//  Native layout (D2R 3.3, D2RLoader 1.3.1 process image)
// ---------------------------------------------------------------------------
constexpr std::int32_t UnitTypeItem          = 4;
constexpr std::size_t  MaximumInventoryItems = 4096;
constexpr const char   SourceFile[]          = "charm-dedup";

// Import thunks D2RLoader generated for the D2RCore replacements.
constexpr std::uint64_t IsCharmUsableThunkRva       = 0x3E2B61C;  // FF 25 7E F6 FF FF (3E2B548 in 1.3.0)
constexpr std::uint64_t IsCharmUsableSlotRva        = 0x3E2ACA0;  // (3E2AC10 in 1.3.0)
constexpr std::uint64_t DisplayRequirementsThunkRva = 0x3E2B298;  // FF 25 6A F5 FF FF (3E2B1CA in 1.3.0)
constexpr std::uint64_t DisplayRequirementsSlotRva  = 0x3E2A808;  // (3E2A780 in 1.3.0)

constexpr auto IsCharmUsableThunk       = std::to_array<std::uint8_t>({0xFF, 0x25, 0x7E, 0xF6, 0xFF, 0xFF});
constexpr auto DisplayRequirementsThunk = std::to_array<std::uint8_t>({0xFF, 0x25, 0x6A, 0xF5, 0xFF, 0xFF});

// Game functions the rule calls.
constexpr std::uint64_t GetItemCodeRva      = 0x36EF50;  // ITEMS_GetItemCode(item) -> packed code
constexpr std::uint64_t GetInventoryRva     = 0x34A360;  // UNITS_GetInventory(unit, file, line)
constexpr std::uint64_t GetFirstItemRva     = 0x388C10;  // INVENTORY_GetFirstItem(inventory)
constexpr std::uint64_t GetNextItemRva      = 0x38ABA0;  // INVENTORY_GetNextItem(item)
constexpr std::uint64_t GetUnitTypeRva      = 0x34B9D0;  // UNITS_GetUnitType(unit) -> [unit+0]
constexpr std::uint64_t GetUnitIdRva        = 0x34A330;  // UNITS_GetUnitId(unit, file, line) -> [unit+8]
constexpr std::uint64_t GetStatListOwnerRva = 0x2F8120;  // STATLIST_GetOwner(item, active*) -> owner or null

constexpr auto GetItemCodeEntry = std::to_array<std::uint8_t>({
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
    0xF9, 0x48, 0x85, 0xC9, 0x75, 0x13, 0x88, 0x4C, 0x24, 0x30, 0x48, 0x8D,
    0x4C, 0x24, 0x30, 0xE8, 0x80, 0x83, 0xFF, 0xFF,
});
constexpr auto GetInventoryEntry = std::to_array<std::uint8_t>({
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
    0xF1, 0x48, 0x85, 0xC9, 0x75, 0x13, 0x88, 0x4C, 0x24, 0x30, 0x48, 0x8D,
    0x4C, 0x24, 0x30, 0xE8, 0x70, 0xCC, 0xFF, 0xFF,
});
constexpr auto GetFirstItemEntry = std::to_array<std::uint8_t>({
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9,
    0x74, 0x2E, 0x81, 0x39, 0x04, 0x03, 0x02, 0x01, 0x74, 0x1C,
});
constexpr auto GetNextItemEntry = std::to_array<std::uint8_t>({
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9,
    0x75, 0x10, 0x88, 0x4C, 0x24, 0x30, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8,
    0x84, 0x98, 0xFF, 0xFF,
});
constexpr auto GetUnitTypeBody = std::to_array<std::uint8_t>({
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x85, 0xC9, 0x75, 0x1D, 0x88, 0x4C, 0x24,
    0x30, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8, 0x39, 0x9E, 0xFF, 0xFF, 0x84,
    0xC0, 0x74, 0x01, 0xCC, 0xB8, 0x06, 0x00, 0x00, 0x00, 0x48, 0x83, 0xC4,
    0x28, 0xC3, 0x8B, 0x01, 0x48, 0x83, 0xC4, 0x28, 0xC3,
});
constexpr auto GetUnitIdBody = std::to_array<std::uint8_t>({
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x85, 0xC9, 0x75, 0x1D, 0x88, 0x4C, 0x24,
    0x30, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8, 0x39, 0xCA, 0xFF, 0xFF, 0x84,
    0xC0, 0x74, 0x01, 0xCC, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0x48, 0x83, 0xC4,
    0x28, 0xC3, 0x8B, 0x41, 0x08, 0x48, 0x83, 0xC4, 0x28, 0xC3,
});
constexpr auto GetStatListOwnerEntry = std::to_array<std::uint8_t>({
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xDA, 0x48, 0x85, 0xC9,
    0x75, 0x15, 0x88, 0x4C, 0x24, 0x30, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8,
    0x44, 0x99, 0xFF, 0xFF,
});

// D2RCore.dll 1.3.1
constexpr wchar_t       CoreModuleName[]                = L"D2RCore.dll";
constexpr char          CoreIsCharmUsableName[]         = "IsCharmUsable";
constexpr char          CoreDisplayRequirementsName[]   = "CheckInventoryItemRequirementsForDisplay";
constexpr std::uint64_t CoreIsCharmUsableRva            = 0x816850;  // 78F7B0 in 1.3.0
constexpr std::uint64_t CoreDisplayRequirementsRva      = 0x815340;  // 78E2A0 in 1.3.0

// IsCharmUsable(item, owner): only rcx and rdx are read, result in eax.
constexpr auto CoreIsCharmUsableBody = std::to_array<std::uint8_t>({
    0x55, 0x56, 0x57, 0x53, 0x48, 0x83, 0xEC, 0x48, 0x48, 0x8D, 0x6C, 0x24,
    0x40, 0x48, 0xC7, 0x45, 0x00, 0xFE, 0xFF, 0xFF, 0xFF, 0x48, 0x89, 0xD6,
    0x48, 0x89, 0xCF, 0xFF, 0x15, 0xAF, 0x92, 0xEE, 0xFF, 0x90, 0x31, 0xDB,
    0x84, 0xC0, 0x74, 0x50, 0x48, 0x89, 0xF9, 0xBA, 0x0D, 0x00, 0x00, 0x00,
    0xFF, 0x15, 0xF2, 0x91, 0xEE, 0xFF, 0x90, 0x85, 0xC0, 0x74, 0x3D, 0x48,
    0x89, 0xF9, 0xFF, 0x15, 0x3C, 0x8E, 0xEE, 0xFF, 0x90, 0x89, 0xC1, 0xE8,
    0x24, 0x85, 0xC0, 0xFF, 0x84, 0xC0, 0x74, 0x28, 0x48, 0x8B, 0x05, 0xB9,
    0x91, 0xEE, 0xFF, 0x0F, 0x57, 0xC0, 0x0F, 0x11, 0x44, 0x24, 0x20, 0xC7,
    0x44, 0x24, 0x30, 0x00, 0x00, 0x00, 0x00, 0x48, 0x89, 0xF9, 0x48, 0x89,
    0xF2, 0x45, 0x31, 0xC0, 0x45, 0x31, 0xC9, 0xFF, 0xD0, 0x90, 0x89, 0xC3,
    0x89, 0xD8, 0x48, 0x83, 0xC4, 0x48, 0x5B, 0x5F, 0x5E, 0x5D, 0xC3,
});

// CheckInventoryItemRequirementsForDisplay(item, unit, a3, a4, a5, a6): reads
// rcx, rdx, r8, r9 and the two stack arguments as one 16-byte movaps.
constexpr auto CoreDisplayRequirementsBody = std::to_array<std::uint8_t>({
    0x55, 0x56, 0x57, 0x53, 0x48, 0x83, 0xEC, 0x48, 0x48, 0x8D, 0x6C, 0x24,
    0x40, 0x48, 0xC7, 0x45, 0x00, 0xFE, 0xFF, 0xFF, 0xFF, 0x48, 0x89, 0xCF,
    0x0F, 0x28, 0x45, 0x50, 0x8B, 0x05, 0xC2, 0x9E, 0xFC, 0xFF, 0x65, 0x48,
    0x8B, 0x0C, 0x25, 0x58, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x1C, 0xC1, 0xC6,
    0x83, 0x20, 0x18, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x05, 0xE3, 0xA6, 0xEE,
    0xFF, 0x0F, 0x11, 0x44, 0x24, 0x20, 0xC7, 0x44, 0x24, 0x30, 0x00, 0x00,
    0x00, 0x00, 0x48, 0x89, 0xF9, 0xFF, 0xD0, 0x90, 0x89, 0xC6, 0x48, 0x85,
    0xFF, 0x74, 0x12, 0x85, 0xF6, 0x74, 0x0E, 0x48, 0x89, 0xF9, 0xFF, 0x15,
    0x2C, 0xA3, 0xEE, 0xFF, 0x90, 0x84, 0xC0, 0x74, 0x0B, 0x89, 0xF0, 0x48,
    0x83, 0xC4, 0x48, 0x5B, 0x5F, 0x5E, 0x5D, 0xC3, 0x48, 0x89, 0xF9, 0xBA,
    0x0D, 0x00, 0x00, 0x00, 0xFF, 0x15, 0xB6, 0xA6, 0xEE, 0xFF, 0x90, 0x85,
    0xC0, 0x74, 0xE2, 0x31, 0xC9, 0xE8, 0xF2, 0x99, 0xC0, 0xFF, 0x84, 0xC0,
    0x75, 0xD7, 0x48, 0x8D, 0x83, 0x20, 0x18, 0x00, 0x00, 0xC6, 0x00, 0x01,
    0x31, 0xF6, 0xEB, 0xC9,
});

// ---------------------------------------------------------------------------
//  Native signatures
// ---------------------------------------------------------------------------
using IsCharmUsableFn       = std::int32_t(__fastcall*)(void* item, void* owner) noexcept;
using DisplayRequirementsFn = std::int32_t(__fastcall*)(void* item, void* unit, std::uint64_t a3, std::uint64_t a4,
                                                        std::uint64_t a5, std::uint64_t a6) noexcept;
using GetItemCodeFn         = std::uint32_t(__fastcall*)(void* item) noexcept;
using GetInventoryFn        = void*(__fastcall*)(void* unit, const char* file, std::int32_t line) noexcept;
using GetFirstItemFn        = void*(__fastcall*)(void* inventory) noexcept;
using GetNextItemFn         = void*(__fastcall*)(void* item) noexcept;
using GetUnitTypeFn         = std::int32_t(__fastcall*)(void* unit) noexcept;
using GetUnitIdFn           = std::uint32_t(__fastcall*)(void* unit, const char* file, std::int32_t line) noexcept;
using GetStatListOwnerFn    = void*(__fastcall*)(void* item, std::int32_t* active) noexcept;

// ---------------------------------------------------------------------------
//  State
// ---------------------------------------------------------------------------
struct InstallState {
    bool usabilityThunk{false};
    bool displayThunk{false};
};

const D2RL::PluginContext* Context{};
std::uintptr_t             Base{};
Settings                   Config{};
InstallState               Installed{};
char                       ConfigSource[48]{"built-in defaults"};
char                       InactiveReason[160]{};

IsCharmUsableFn       OriginalIsCharmUsable{};
DisplayRequirementsFn OriginalDisplayRequirements{};
GetItemCodeFn         GetItemCode{};
GetInventoryFn        GetInventory{};
GetFirstItemFn        GetFirstItem{};
GetNextItemFn         GetNextItem{};
GetUnitTypeFn         GetUnitType{};
GetUnitIdFn           GetUnitId{};
GetStatListOwnerFn    GetStatListOwner{};

std::atomic<bool> RedBackgroundActive{false};

std::uint8_t* StubPage{};
constexpr std::size_t StubPageBytes           = 0x1000;
constexpr std::size_t UsabilityStubOffset     = 0x00;
constexpr std::size_t DisplayStubOffset       = 0x10;

struct ThunkPatch {
    std::uint64_t               rva;
    const std::uint8_t*         stock;
    std::array<std::uint8_t, 6> written;
    bool                        applied;
};
ThunkPatch UsabilityPatch{IsCharmUsableThunkRva, IsCharmUsableThunk.data(), {}, false};
ThunkPatch DisplayPatch{DisplayRequirementsThunkRva, DisplayRequirementsThunk.data(), {}, false};

std::atomic<std::uint64_t> GroupedChecks{};
std::atomic<std::uint64_t> SwitchedOffByActive{};
std::atomic<std::uint64_t> SwitchedOffByOlder{};
std::atomic<std::uint64_t> RedBackgroundsDrawn{};
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

auto ReadU64(const void* base, std::size_t offset) noexcept -> std::uint64_t {
    std::uint64_t value = 0;
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
//  Configuration
// ---------------------------------------------------------------------------
auto LoadConfig() noexcept -> bool {
    Config = Settings{};
    std::string error;
    std::string unknown;
    if (!ParseConfig(DefaultConfigToml, Config, error, unknown)) {
        LogError("CharmDedup: the built-in default configuration does not parse (%s).", error.c_str());
        return false;
    }
    std::snprintf(ConfigSource, sizeof(ConfigSource), "%s", "built-in defaults");

    if (!Context->EnsureConfig(DefaultConfigToml)) {
        LogWarn("CharmDedup: could not create the default config, using built-in defaults.");
        return true;
    }

    std::string buffer(16384, '\0');
    std::uint32_t required = 0;
    if (!Context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size()), &required)) {
        if (required <= buffer.size()) {
            LogWarn("CharmDedup: could not read the config, using built-in defaults.");
            return true;
        }
        buffer.assign(required, '\0');
        if (!Context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size()), &required)) {
            LogWarn("CharmDedup: could not read the config, using built-in defaults.");
            return true;
        }
    }

    Settings parsed{};
    error.clear();
    unknown.clear();
    if (!ParseConfig(std::string_view(buffer.c_str()), parsed, error, unknown)) {
        LogError("CharmDedup: config rejected (%s). Nothing is changed so a setting you meant is not silently "
                 "ignored; fix the file and restart.", error.c_str());
        return false;
    }
    if (!unknown.empty()) {
        LogWarn("CharmDedup: ignoring unknown config setting(s): %s.", unknown.c_str());
    }
    Config = parsed;
    std::snprintf(ConfigSource, sizeof(ConfigSource), "%s", "config file");
    return true;
}

// ---------------------------------------------------------------------------
//  Image checks
// ---------------------------------------------------------------------------
auto CheckExact(std::uint64_t rva, const std::uint8_t* bytes, std::size_t size, const char* name) noexcept -> bool {
    if (Context->CheckExpectedBytes(rva, bytes, static_cast<std::uint32_t>(size))) {
        return true;
    }
    LogError("CharmDedup: %s at RVA 0x%llX does not match D2RLoader 1.3.1.", name, static_cast<unsigned long long>(rva));
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
            LogInfo("CharmDedup: %s at RVA 0x%llX is hooked by another plugin, calling through it.", name,
                    static_cast<unsigned long long>(rva));
            return true;
        }
    }

    LogError("CharmDedup: %s at RVA 0x%llX does not match D2RLoader 1.3.1.", name, static_cast<unsigned long long>(rva));
    return false;
}

auto ValidateGameFunctions() noexcept -> bool {
    return CheckCallable(GetItemCodeRva, GetItemCodeEntry.data(), GetItemCodeEntry.size(), "ITEMS_GetItemCode")
        && CheckCallable(GetInventoryRva, GetInventoryEntry.data(), GetInventoryEntry.size(), "UNITS_GetInventory")
        && CheckCallable(GetFirstItemRva, GetFirstItemEntry.data(), GetFirstItemEntry.size(), "INVENTORY_GetFirstItem")
        && CheckCallable(GetNextItemRva, GetNextItemEntry.data(), GetNextItemEntry.size(), "INVENTORY_GetNextItem")
        && CheckCallable(GetUnitTypeRva, GetUnitTypeBody.data(), GetUnitTypeBody.size(), "UNITS_GetUnitType")
        && CheckCallable(GetUnitIdRva, GetUnitIdBody.data(), GetUnitIdBody.size(), "UNITS_GetUnitId")
        && CheckCallable(GetStatListOwnerRva, GetStatListOwnerEntry.data(), GetStatListOwnerEntry.size(),
                         "STATLIST_GetOwner");
}

void ResolveGameFunctions() noexcept {
    GetItemCode      = At<GetItemCodeFn>(GetItemCodeRva);
    GetInventory     = At<GetInventoryFn>(GetInventoryRva);
    GetFirstItem     = At<GetFirstItemFn>(GetFirstItemRva);
    GetNextItem      = At<GetNextItemFn>(GetNextItemRva);
    GetUnitType      = At<GetUnitTypeFn>(GetUnitTypeRva);
    GetUnitId        = At<GetUnitIdFn>(GetUnitIdRva);
    GetStatListOwner = At<GetStatListOwnerFn>(GetStatListOwnerRva);
}

// Resolves a D2RCore export by name and proves three things: it sits at the
// 1.3.1 RVA, its body is the 1.3.1 body, and the game's thunk slot holds it.
auto ResolveCoreExport(const char* name, std::uint64_t expectedRva, const std::uint8_t* body, std::size_t bodySize,
                       std::uint64_t slotRva) noexcept -> std::uintptr_t {
    const HMODULE module = GetModuleHandleW(CoreModuleName);
    if (module == nullptr) {
        LogError("CharmDedup: D2RCore.dll is not loaded.");
        return 0;
    }
    const auto address = reinterpret_cast<std::uintptr_t>(GetProcAddress(module, name));
    const auto coreBase = reinterpret_cast<std::uintptr_t>(module);
    if (address == 0 || address != coreBase + expectedRva) {
        LogError("CharmDedup: D2RCore.dll export %s is not where D2RCore 1.3.1 has it.", name);
        return 0;
    }
    const auto* image = reinterpret_cast<const std::uint8_t*>(module);
    const auto  ntOffset = static_cast<std::size_t>(ReadU32(image, 0x3C));
    const auto  imageSize = ReadU32(image, ntOffset + 0x50);
    if (expectedRva + bodySize > imageSize
        || std::memcmp(reinterpret_cast<const std::uint8_t*>(address), body, bodySize) != 0) {
        LogError("CharmDedup: D2RCore.dll %s is not the 1.3.1 body this plugin was verified against.", name);
        return 0;
    }
    // The hooks call the export directly, so the slot is only a sanity check.
    // D2RLoader may bind it after plugins load; an unbound slot is accepted,
    // a slot bound to any other code is not.
    const auto slotValue = ReadU64(reinterpret_cast<const void*>(Base), static_cast<std::size_t>(slotRva));
    if (slotValue != address) {
        MEMORY_BASIC_INFORMATION info{};
        const bool executable = slotValue != 0
            && VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(slotValue)), &info, sizeof(info)) != 0
            && info.State == MEM_COMMIT
            && (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
        if (executable) {
            LogError("CharmDedup: the game's import slot for %s (RVA 0x%llX) points at other code, not D2RCore's export.",
                     name, static_cast<unsigned long long>(slotRva));
            return 0;
        }
        LogInfo("CharmDedup: the game's import slot for %s is not bound yet; the thunk is taken regardless.", name);
    }
    return address;
}

// ---------------------------------------------------------------------------
//  The rule, on native units
// ---------------------------------------------------------------------------
struct NativeEnvironment {
    auto IsAttached(void* item) const noexcept -> bool {
        return GetStatListOwner(item, nullptr) != nullptr;
    }
    auto UnitId(void* unit) const noexcept -> std::uint32_t {
        return GetUnitId(unit, SourceFile, 0);
    }
    auto FirstItem(void* owner) const noexcept -> void* {
        auto* inventory = GetInventory(owner, SourceFile, 0);
        return inventory != nullptr ? GetFirstItem(inventory) : nullptr;
    }
    auto NextItem(void* item) const noexcept -> void* {
        return GetNextItem(item);
    }
    auto IsItem(void* unit) const noexcept -> bool {
        return GetUnitType(unit) == UnitTypeItem;
    }
    auto GroupOf(void* item) const noexcept -> std::int32_t {
        return FindGroup(Config, GetItemCode(item));
    }
    auto IsEligible(void* item, void* owner) const noexcept -> bool {
        return OriginalIsCharmUsable(item, owner) != 0;
    }
    auto MaximumItems() const noexcept -> std::size_t {
        return MaximumInventoryItems;
    }
};

// Group of an item unit, or NoGroup. Only item units are asked for a code.
auto GroupOfUnit(void* unit) noexcept -> std::int32_t {
    if (unit == nullptr || GetUnitType(unit) != UnitTypeItem) {
        return NoGroup;
    }
    return FindGroup(Config, GetItemCode(unit));
}

// ---------------------------------------------------------------------------
//  Hooks
// ---------------------------------------------------------------------------
std::int32_t __fastcall HookIsCharmUsable(void* item, void* owner) noexcept {
    const std::int32_t stock = OriginalIsCharmUsable(item, owner);
    if (stock == 0 || item == nullptr || owner == nullptr) {
        return stock;
    }
    const auto group = GroupOfUnit(item);
    if (group == NoGroup) {
        return stock;
    }

    NativeEnvironment env{};
    std::uint32_t blocker = 0;
    const auto verdict = EvaluateGroupedCharm(env, item, owner, group, &blocker);
    GroupedChecks.fetch_add(1, std::memory_order_relaxed);
    if (!IsBlocked(verdict)) {
        return stock;
    }

    (verdict == Verdict::BlockedByActive ? SwitchedOffByActive : SwitchedOffByOlder)
        .fetch_add(1, std::memory_order_relaxed);
    if (TakeDiagnosticsLine()) {
        char code[5]{};
        UnpackItemCode(GetItemCode(item), code);
        LogInfo("CharmDedup: charm %s (unit %u) switched off, group %d already has %s charm unit %u.", code,
                GetUnitId(item, SourceFile, 0), group + 1,
                verdict == Verdict::BlockedByActive ? "the active" : "the older", blocker);
    }
    return 0;
}

std::int32_t __fastcall HookDisplayRequirements(void* item, void* unit, std::uint64_t a3, std::uint64_t a4,
                                                std::uint64_t a5, std::uint64_t a6) noexcept {
    const std::int32_t stock = OriginalDisplayRequirements(item, unit, a3, a4, a5, a6);
    if (stock == 0 || item == nullptr || unit == nullptr || !RedBackgroundActive.load(std::memory_order_relaxed)) {
        return stock;
    }
    const auto group = GroupOfUnit(item);
    if (group == NoGroup || OriginalIsCharmUsable(item, unit) == 0) {
        return stock;
    }

    NativeEnvironment env{};
    if (!IsBlocked(EvaluateGroupedCharm(env, item, unit, group, nullptr))) {
        return stock;
    }
    RedBackgroundsDrawn.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

// ---------------------------------------------------------------------------
//  Installation
// ---------------------------------------------------------------------------
auto AllocateStubPage() noexcept -> std::uint8_t* {
    const auto* image = reinterpret_cast<const std::uint8_t*>(Base);
    const auto  ntOffset = static_cast<std::size_t>(ReadU32(image, 0x3C));
    const auto  imageSize = ReadU32(image, ntOffset + 0x50);

    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    const std::uintptr_t granularity = systemInfo.dwAllocationGranularity != 0 ? systemInfo.dwAllocationGranularity : 0x10000;
    const auto alignUp = [granularity](std::uintptr_t value) noexcept {
        return (value + granularity - 1) & ~(granularity - 1);
    };

    // The page sits above the image, so the lower thunk is the farther one.
    const std::uintptr_t limit = Base + DisplayRequirementsThunkRva + 0x7FF00000ULL;
    std::uintptr_t address = alignUp(Base + imageSize);
    while (address + StubPageBytes < limit) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) == 0) {
            break;
        }
        const auto regionEnd = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (info.State == MEM_FREE && address + StubPageBytes <= regionEnd) {
            if (auto* page = VirtualAlloc(reinterpret_cast<LPVOID>(address), StubPageBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
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

// Two absolute jump stubs: FF 25 00 00 00 00 <target>.
auto BuildStubPage() noexcept -> bool {
    StubPage = AllocateStubPage();
    if (StubPage == nullptr) {
        LogError("CharmDedup: no free page within jump range of the game image.");
        return false;
    }
    std::memset(StubPage, 0xCC, StubPageBytes);
    const auto emit = [](std::size_t offset, std::uintptr_t target) noexcept {
        const std::uint8_t jump[6]{0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
        std::memcpy(StubPage + offset, jump, sizeof(jump));
        const auto value = static_cast<std::uint64_t>(target);
        std::memcpy(StubPage + offset + sizeof(jump), &value, sizeof(value));
    };
    emit(UsabilityStubOffset, reinterpret_cast<std::uintptr_t>(&HookIsCharmUsable));
    emit(DisplayStubOffset, reinterpret_cast<std::uintptr_t>(&HookDisplayRequirements));

    DWORD previous = 0;
    if (!VirtualProtect(StubPage, StubPageBytes, PAGE_EXECUTE_READ, &previous)) {
        LogError("CharmDedup: could not make the stub page executable.");
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), StubPage, StubPageBytes);
    return true;
}

auto PatchThunk(ThunkPatch& patch, std::size_t stubOffset) noexcept -> bool {
    const auto from = Base + static_cast<std::uintptr_t>(patch.rva) + 5;
    const auto to   = reinterpret_cast<std::uintptr_t>(StubPage) + stubOffset;
    const auto rel  = static_cast<std::int64_t>(to) - static_cast<std::int64_t>(from);
    if (rel < INT32_MIN || rel > INT32_MAX) {
        LogError("CharmDedup: the stub page is out of reach of RVA 0x%llX.", static_cast<unsigned long long>(patch.rva));
        return false;
    }
    patch.written[0] = 0xE9;
    const auto rel32 = static_cast<std::int32_t>(rel);
    std::memcpy(patch.written.data() + 1, &rel32, sizeof(rel32));
    patch.written[5] = 0x90;

    if (!Context->PatchBytes(patch.rva, patch.stock, 6, patch.written.data(), 6)) {
        LogError("CharmDedup: the loader refused the thunk at RVA 0x%llX.", static_cast<unsigned long long>(patch.rva));
        return false;
    }
    patch.applied = true;
    if (std::memcmp(reinterpret_cast<const std::uint8_t*>(Base + static_cast<std::uintptr_t>(patch.rva)), patch.written.data(), 6) != 0) {
        LogError("CharmDedup: RVA 0x%llX does not hold the bytes that were written.", static_cast<unsigned long long>(patch.rva));
        return false;
    }
    return true;
}

auto RestoreThunk(ThunkPatch& patch) noexcept -> bool {
    if (!patch.applied) {
        return true;
    }
    if (!Context->PatchBytes(patch.rva, patch.written.data(), 6, patch.stock, 6)) {
        LogError("CharmDedup: could not restore the thunk at RVA 0x%llX.", static_cast<unsigned long long>(patch.rva));
        return false;
    }
    patch.applied = false;
    return true;
}

void Install() noexcept {
    if (!CheckExact(IsCharmUsableThunkRva, IsCharmUsableThunk.data(), IsCharmUsableThunk.size(), "IsCharmUsable import thunk")
        || !ValidateGameFunctions()) {
        SetInactive("the game image does not match D2RLoader 1.3.1");
        return;
    }
    ResolveGameFunctions();

    const auto usability = ResolveCoreExport(CoreIsCharmUsableName, CoreIsCharmUsableRva, CoreIsCharmUsableBody.data(),
                                             CoreIsCharmUsableBody.size(), IsCharmUsableSlotRva);
    if (usability == 0) {
        SetInactive("D2RCore.dll IsCharmUsable does not match 1.3.1");
        return;
    }
    OriginalIsCharmUsable = reinterpret_cast<IsCharmUsableFn>(usability);

    if (!BuildStubPage()) {
        SetInactive("no stub page could be prepared");
        return;
    }

    if (!PatchThunk(UsabilityPatch, UsabilityStubOffset)) {
        RestoreThunk(UsabilityPatch);
        SetInactive("the IsCharmUsable thunk could not be taken");
        return;
    }
    Installed.usabilityThunk = true;

    if (!Config.redBackground) {
        return;
    }
    if (!CheckExact(DisplayRequirementsThunkRva, DisplayRequirementsThunk.data(), DisplayRequirementsThunk.size(),
                    "item background import thunk")) {
        LogWarn("CharmDedup: red_background is off; the dedup itself is active.");
        return;
    }
    const auto display = ResolveCoreExport(CoreDisplayRequirementsName, CoreDisplayRequirementsRva,
                                           CoreDisplayRequirementsBody.data(), CoreDisplayRequirementsBody.size(),
                                           DisplayRequirementsSlotRva);
    if (display == 0) {
        LogWarn("CharmDedup: red_background is off; the dedup itself is active.");
        return;
    }
    OriginalDisplayRequirements = reinterpret_cast<DisplayRequirementsFn>(display);
    RedBackgroundActive.store(true, std::memory_order_relaxed);
    if (!PatchThunk(DisplayPatch, DisplayStubOffset)) {
        RedBackgroundActive.store(false, std::memory_order_relaxed);
        RestoreThunk(DisplayPatch);
        LogWarn("CharmDedup: red_background is off; the dedup itself is active.");
        return;
    }
    Installed.displayThunk = true;
}

// ---------------------------------------------------------------------------
//  Console
// ---------------------------------------------------------------------------
auto OnOff(bool value) noexcept -> const char* {
    return value ? "on" : "off";
}

void DescribeGroups(char* output, std::size_t size) noexcept {
    std::size_t used = 0;
    output[0] = '\0';
    for (std::size_t group = 0; group < Config.groupCount; ++group) {
        const char* separator = group == 0 ? "" : " | ";
        int written = std::snprintf(output + used, size - used, "%s", separator);
        if (written < 0 || static_cast<std::size_t>(written) >= size - used) {
            return;
        }
        used += static_cast<std::size_t>(written);
        bool first = true;
        for (std::size_t index = 0; index < Config.codeCount; ++index) {
            if (Config.codes[index].group != static_cast<std::int32_t>(group)) {
                continue;
            }
            char code[5]{};
            UnpackItemCode(Config.codes[index].code, code);
            written = std::snprintf(output + used, size - used, first ? "%s" : " %s", code);
            if (written < 0 || static_cast<std::size_t>(written) >= size - used) {
                return;
            }
            used += static_cast<std::size_t>(written);
            first = false;
        }
    }
}

auto StatusCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
    if (command == nullptr || command->plugin == nullptr) {
        return D2RL::ConsoleCommandResult::Failed;
    }

    char groups[256]{};
    DescribeGroups(groups, sizeof(groups));
    char line[512]{};
    std::snprintf(line, sizeof(line), "Charm Dedup %s (%s): groups [%s]; dedup %s; red background %s; diagnostics %s.%s%s",
                  PluginVersion, ConfigSource, groups, OnOff(Installed.usabilityThunk), OnOff(Installed.displayThunk),
                  OnOff(Config.diagnostics), InactiveReason[0] != '\0' ? " Inactive: " : "", InactiveReason);
    command->plugin->WriteConsoleMessage(line);

    std::snprintf(line, sizeof(line),
                  "Grouped charm checks: %llu. Switched off because another charm is active: %llu, because an older "
                  "charm takes the slot: %llu. Red backgrounds drawn: %llu.",
                  static_cast<unsigned long long>(GroupedChecks.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(SwitchedOffByActive.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(SwitchedOffByOlder.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(RedBackgroundsDrawn.load(std::memory_order_relaxed)));
    command->plugin->WriteConsoleMessage(line);
    return D2RL::ConsoleCommandResult::Handled;
}

void ResetState() noexcept {
    Config    = Settings{};
    Installed = InstallState{};
    InactiveReason[0] = '\0';
    OriginalIsCharmUsable       = nullptr;
    OriginalDisplayRequirements = nullptr;
    RedBackgroundActive.store(false, std::memory_order_relaxed);
    UsabilityPatch.applied = false;
    DisplayPatch.applied   = false;
    GroupedChecks.store(0, std::memory_order_relaxed);
    SwitchedOffByActive.store(0, std::memory_order_relaxed);
    SwitchedOffByOlder.store(0, std::memory_order_relaxed);
    RedBackgroundsDrawn.store(0, std::memory_order_relaxed);
    DiagnosticsBudget.store(64, std::memory_order_relaxed);
}

}  // namespace
}  // namespace CelestialRayOne::CharmDedup

namespace {

constexpr D2RL::PluginInfo Info{
    .infoSize    = D2RL::PluginInfoSize,
    .abiVersion  = D2RL_PLUGIN_ABI_VERSION,
    .id          = "celestialrayone.charm-dedup",
    .name        = "Charm Dedup",
    .version     = "1.0.0",
    .author      = "CelestialRayOne",
    .description = "Only one charm of each configured item-code group is active; the others get a red background.",
    .flags       = D2RL::PluginFlags::Shared | D2RL::PluginFlags::NativeHooks,
};

}  // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
    return &Info;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
    using namespace CelestialRayOne::CharmDedup;

    if (!D2RL::HasContext(context) || context->exeBase == 0) {
        return false;
    }
    Context = context;
    Base    = context->exeBase;
    ResetState();

    if (!context->RegisterConsoleCommand("charmdedup", StatusCommand, "Show Charm Dedup settings, state and counters.")) {
        LogWarn("CharmDedup: the charmdedup console command could not be registered.");
    }

    if (!LoadConfig()) {
        Config.enabled = false;
        SetInactive("the config was rejected, see the plugin log");
        return true;
    }
    if (!Config.enabled) {
        SetInactive("disabled by config");
        LogInfo("Charm Dedup %s loaded disabled by config; the game runs stock.", PluginVersion);
        return true;
    }
    if (Config.groupCount == 0) {
        SetInactive("no groups configured");
        LogInfo("Charm Dedup %s loaded with no groups; the game runs stock.", PluginVersion);
        return true;
    }

    Install();

    char groups[256]{};
    DescribeGroups(groups, sizeof(groups));
    LogInfo("Charm Dedup %s by CelestialRayOne: groups [%s], dedup %s, red background %s, config %s, build %s.",
            PluginVersion, groups, OnOff(Installed.usabilityThunk), OnOff(Installed.displayThunk), ConfigSource,
            context->buildName != nullptr ? context->buildName : "unknown");
    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    using namespace CelestialRayOne::CharmDedup;
    if (Context == nullptr) {
        return;
    }
    RedBackgroundActive.store(false, std::memory_order_relaxed);
    RestoreThunk(DisplayPatch);
    RestoreThunk(UsabilityPatch);
}
