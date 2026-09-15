// Death Penalty Safe Levels
//
// Dying inside a listed level costs nothing: no experience loss and no gold
// loss. Every other level behaves exactly like vanilla. The list is empty by
// default, so installing this plugin without editing its config changes
// nothing at all.
//
// Port of the ESR D2R 2.4 memory patch pair "Skip Exp Loss in Protected
// Levels" (hook 28D9A0 -> 149B5BC cave, 32-byte level bitmap) to D2R 3.3.
// Everything below was read out of the live D2RLoader.exe image and
// disassembled before being relied on.
//
// ---------------------------------------------------------------------------
// The death penalty, sub_140424AC0 @ RVA 0x424AC0
// ---------------------------------------------------------------------------
//   D2Game\src\Player\Player.cpp, asserts on line 978. Windows x64 ABI
//   (Game* rcx, Unit* dyingPlayer rdx, Unit* killer r8) -> int32.
//
//     424AC0  40 53                push  rbx
//     424AC2  56                   push  rsi
//     424AC3  57                   push  rdi
//     424AC4  48 83 EC 40          sub   rsp, 40h        ; 8-byte prologue
//     424AC8  49 8B D8             mov   rbx, r8         ; killer
//     424ACB  48 8B F2             mov   rsi, rdx        ; dying player
//     424ACE  48 8B F9             mov   rdi, rcx        ; game
//     424AD1  E8 DA 11 00 00       call  425CB0          ; the gold stage
//
//   It runs two stages. The gold stage is a separate function in 3.3 (it was
//   inlined in the 2.4 build, which is why sub_14028D9A0 was 555 bytes and
//   this one is 343). The experience stage is the rest of the body:
//
//     - a killer check. GetUnitType(killer) and, for a minion,
//       GetUnitType(GetOwner(killer)): a player kill costs no experience.
//     - difficulty record through 0x300830, DeathExpPenalty at record + 8.
//     - stat 12 (level), and the experience thresholds for level-1 and level
//       through 0x300920, giving the span of the current level.
//     - loss = span * DeathExpPenalty / 100, floored so the player can never
//       drop below the start of the current level.
//     - the write that makes it real:
//
//         424BEE  45 33 C9         xor   r9d, r9d
//         424BF1  44 8B C7         mov   r8d, edi        ; new experience
//         424BF4  48 8B CE         mov   rcx, rsi        ; dying player
//         424BF7  41 8D 51 0D      lea   edx, [r9+0Dh]   ; stat 13, experience
//         424BFB  E8 10 31 ED FF   call  2F7D10          ; STATLIST_SetUnitStat
//
//   Exactly one caller, at 0x42A441, and it discards the return value: the
//   next instruction is xor r8d,r8d for the following call. Returning early
//   is therefore free, which is what the 2.4 cave did with its bare RET.
//
// ---------------------------------------------------------------------------
// The gold stage, sub_140425CB0 @ RVA 0x425CB0
// ---------------------------------------------------------------------------
//   Same three arguments. Reads stat 12 (level), 14 (gold) and 15 (goldbank),
//   applies the classic "lose 20%, capped at level * 500" rule and writes
//   stats 14, 15 and 175. Exactly one caller: the function above.
//
//   The 2.4 patch replaced the whole penalty function with a RET, so it
//   already skipped gold as well as experience. Now that the stages are
//   separate functions the two are separate switches, and gold is left alone
//   by default: dying in a safe level costs experience nothing and gold the
//   usual amount. Set protect_gold to reproduce the 2.4 behaviour exactly.
//
//   When experience is protected and gold is not, the hook cannot simply let
//   the original run, because the original is what takes the experience. It
//   calls the gold stage itself and then returns.
//
// ---------------------------------------------------------------------------
// Which level the dying player is in
// ---------------------------------------------------------------------------
//   Identical chain to 2.4, and each step is verified against the accessor
//   that performs it rather than trusted from a note:
//
//     UNITS_GetRoom            0x34B440   (unit) -> ActiveRoom*
//     DUNGEON_GetLevelIdFromRoom 0x2EFC10  mov rcx,[rcx+18h]; jmp 360FC0
//     DRLGROOM_GetLevelId      0x360FC0   mov rax,[rcx+90h]; mov eax,[rax+1F8h]
//
//   So ActiveRoom + 0x18 -> DrlgRoom, DrlgRoom + 0x90 -> Level, Level + 0x1F8
//   -> level id. Those last two bodies are byte-checked at load, which means
//   the three offsets below are verified against the image, not assumed.
//
//   The walk is done inline with a null check at every step, exactly like the
//   cave did, rather than calling 0x2EFC10: DRLGROOM_GetLevelId dereferences
//   its DrlgRoom and its Level without checking either.
//
//   The 2.4 bitmap was 32 bytes because level ids were treated as 0..255.
//   In 3.3 the Levels.txt record-count guard at 0x330446 compares against
//   0x400, so ids run 0..1023 and the set here is sized for that.

#include <D2RLPlugin/api.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace CelestialRayOne::DeathPenaltySafeLevels {
namespace {

// ---------------------------------------------------------------------------
// Native anchors
// ---------------------------------------------------------------------------

constexpr std::uint64_t DeathPenaltyRva = 0x424AC0;
constexpr std::uint64_t GoldPenaltyRva  = 0x425CB0;
constexpr std::uint64_t UnitsGetRoomRva = 0x34B440;
constexpr std::uint64_t RoomToDrlgRva   = 0x2EFC10;
constexpr std::uint64_t DrlgToLevelRva  = 0x360FC0;

// push rbx / push rsi / push rdi / sub rsp,40h / mov rbx,r8 / mov rsi,rdx /
// mov rdi,rcx / call 425CB0 / test rbx,rbx / jz 424B08 / mov rcx,rbx
constexpr std::uint8_t DeathPenaltyEntry[]{
    0x40, 0x53, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x40,
    0x49, 0x8B, 0xD8, 0x48, 0x8B, 0xF2, 0x48, 0x8B, 0xF9,
    0xE8, 0xDA, 0x11, 0x00, 0x00,
    0x48, 0x85, 0xDB, 0x74, 0x2D, 0x48, 0x8B, 0xCB,
};

constexpr std::uint8_t DeathPenaltyPrologue[]{
    0x40, 0x53, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x40,
};

// The experience write itself. This is what proves 0x424AC0 is the function
// that takes experience away, and that it takes it from argument two.
// xor r9d,r9d / mov r8d,edi / mov rcx,rsi / lea edx,[r9+0Dh] / call 2F7D10
constexpr std::uint64_t ExperienceWriteRva = 0x424BEE;
constexpr std::uint8_t  ExperienceWriteWitness[]{
    0x45, 0x33, 0xC9, 0x44, 0x8B, 0xC7, 0x48, 0x8B, 0xCE,
    0x41, 0x8D, 0x51, 0x0D, 0xE8, 0x10, 0x31, 0xED, 0xFF,
};

// mov [rsp+10h],rbx / push rbp / push rsi / push rdi / push r12 / push r13 /
// push r14 / push r15 / sub rsp,20h / mov rdi,rdx / mov r12,r8 /
// xor r8d,r8d / mov r13,rbp
constexpr std::uint8_t GoldPenaltyEntry[]{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57,
    0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
    0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xFA,
    0x4D, 0x8B, 0xE0, 0x45, 0x33, 0xC0, 0x4C, 0x8B, 0xE9,
};

constexpr std::uint8_t GoldPenaltyPrologue[]{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57,
};

// push rbx / sub rsp,20h / mov rbx,rcx / test rcx,rcx / jnz 34B461 /
// mov [rsp+30h],cl / lea rcx,[rsp+30h] / call 345BB0
constexpr std::uint8_t UnitsGetRoomEntry[]{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9,
    0x48, 0x85, 0xC9, 0x75, 0x13, 0x88, 0x4C, 0x24, 0x30,
    0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8, 0x54, 0xA7, 0xFF, 0xFF,
};

// test rcx,rcx / jnz +3 / xor eax,eax / ret / mov rcx,[rcx+18h] / jmp 360FC0
// The whole point of checking this one is the +18h.
constexpr std::uint8_t RoomToDrlgBody[]{
    0x48, 0x85, 0xC9, 0x75, 0x03, 0x33, 0xC0, 0xC3,
    0x48, 0x8B, 0x49, 0x18, 0xE9, 0x9F, 0x13, 0x07, 0x00,
};

// mov rax,[rcx+90h] / mov eax,[rax+1F8h] / ret
// And this one is checked for the +90h and the +1F8h.
constexpr std::uint8_t DrlgToLevelBody[]{
    0x48, 0x8B, 0x81, 0x90, 0x00, 0x00, 0x00,
    0x8B, 0x80, 0xF8, 0x01, 0x00, 0x00, 0xC3,
};

constexpr std::size_t ActiveRoomDrlgRoomOffset = 0x18;
constexpr std::size_t DrlgRoomLevelOffset      = 0x90;
constexpr std::size_t LevelIdOffset            = 0x1F8;

template <std::size_t Size>
constexpr auto ByteCount(const std::uint8_t (&)[Size]) noexcept -> std::uint32_t {
    return static_cast<std::uint32_t>(Size);
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

constexpr std::int32_t MinimumLevelId = 0;
constexpr std::int32_t MaximumLevelId = 1023;
constexpr std::size_t  LevelSetWords  = 16;             // 1024 bits
constexpr std::size_t  MaximumConfigBytes = 65'536;
constexpr std::size_t  MaximumArrayChars  = 32'768;

constexpr char ConfigSection[] = "death_penalty_safe_levels";

constexpr char DefaultConfigToml[] =
    "# Death Penalty Safe Levels\n"
    "#\n"
    "# Dying inside one of the levels listed below costs nothing: the death\n"
    "# penalty is skipped entirely for that death. Every other level keeps the\n"
    "# vanilla penalty, and nothing else about dying changes: the corpse, the\n"
    "# dropped items, the walk back and the town respawn all behave normally.\n"
    "#\n"
    "# Two separate things are skipped, each with its own switch:\n"
    "#\n"
    "#   experience  the death experience loss. Vanilla takes\n"
    "#               DeathExpPenalty percent (difficultylevels.txt) of the\n"
    "#               experience span of your current level, and never lets you\n"
    "#               fall below the start of that level.\n"
    "#               Protected by default.\n"
    "#   gold        the death gold loss. Vanilla takes 20 percent of carried\n"
    "#               plus stashed gold, capped at character level * 500.\n"
    "#               NOT protected by default, so you still pay it.\n"
    "#\n"
    "# Being killed by another player already costs no experience in vanilla,\n"
    "# so this plugin changes nothing about PvP deaths.\n"
    "#\n"
    "# Server side only. In single player the game hosts its own server, so it\n"
    "# applies there too.\n"
    "\n"
    "[death_penalty_safe_levels]\n"
    "\n"
    "# Master switch. false installs nothing at all.\n"
    "enabled = true\n"
    "\n"
    "# The levels that are safe to die in, as Levels.txt Id values, 0 to 1023.\n"
    "# An empty list means this plugin does nothing, which is the default.\n"
    "# The list may be written on one line or spread over several, and may\n"
    "# carry # comments:\n"
    "#\n"
    "#   protected_levels = [141, 146, 149]\n"
    "#\n"
    "#   protected_levels = [\n"
    "#     141,   # my boss arena\n"
    "#     146,\n"
    "#   ]\n"
    "#\n"
    "# For reference, the levels the ESR 2.4 patch protected were:\n"
    "#   141, 146, 149, 154, 159, 162, 175, 180, 181, 184,\n"
    "#   190, 197, 198, 199, 202, 203, 206, 211, 213\n"
    "protected_levels = []\n"
    "\n"
    "# Skip the experience loss in those levels.\n"
    "protect_experience = true\n"
    "\n"
    "# Skip the gold loss in those levels. Off by default: a safe level is\n"
    "# about not losing progress, and gold is not progress. The original ESR\n"
    "# 2.4 patch replaced the whole penalty function and so skipped gold as\n"
    "# well, so set this to true to reproduce that exactly.\n"
    "protect_gold = false\n";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

enum class HookState : std::uint8_t {
    NotAttempted,
    DisabledByConfig,
    NoProtectedLevels,
    UnsupportedBuild,
    InstallFailed,
    Installed,
};

using DeathPenaltyFn =
    std::uint64_t(__fastcall*)(void* game, void* player, void* killer) noexcept;
using UnitsGetRoomFn = void*(__fastcall*)(void* unit) noexcept;

const D2RL::PluginContext* Context{};

DeathPenaltyFn OriginalDeathPenalty{};
DeathPenaltyFn OriginalGoldPenalty{};
DeathPenaltyFn NativeGoldPenalty{};
UnitsGetRoomFn UnitsGetRoom{};

struct Settings {
    bool enabled           = true;
    bool protectExperience = true;
    bool protectGold       = false;
};

Settings                              Config{};
std::array<std::uint64_t, LevelSetWords> ProtectedLevels{};
std::int32_t                          ProtectedCount{};
bool                                  ConfigFileWasRead{};

std::atomic<HookState>    ExperienceHook{ HookState::NotAttempted };
std::atomic<HookState>    GoldHook{ HookState::NotAttempted };
std::atomic<bool>         Armed{ false };
std::atomic<std::uint64_t> ExperienceSaves{};
std::atomic<std::uint64_t> GoldSaves{};

auto IsProtectedLevel(std::int32_t levelId) noexcept -> bool {
    if (levelId < MinimumLevelId || levelId > MaximumLevelId) {
        return false;
    }
    const auto index = static_cast<std::size_t>(levelId);
    return (ProtectedLevels[index >> 6] >> (index & 63U)) & 1ULL;
}

void MarkProtectedLevel(std::int32_t levelId) noexcept {
    const auto index = static_cast<std::size_t>(levelId);
    ProtectedLevels[index >> 6] |= 1ULL << (index & 63U);
}

// ---------------------------------------------------------------------------
// Level lookup
// ---------------------------------------------------------------------------

// Returns -1 when the level cannot be determined. Every step is null checked,
// because the native accessors that perform the last two are not.
auto LevelIdOfUnit(void* unit) noexcept -> std::int32_t {
    if (unit == nullptr || UnitsGetRoom == nullptr) {
        return -1;
    }

    auto* room = static_cast<std::uint8_t*>(UnitsGetRoom(unit));
    if (room == nullptr) {
        return -1;
    }

    auto* drlgRoom = *reinterpret_cast<std::uint8_t**>(room + ActiveRoomDrlgRoomOffset);
    if (drlgRoom == nullptr) {
        return -1;
    }

    auto* level = *reinterpret_cast<std::uint8_t**>(drlgRoom + DrlgRoomLevelOffset);
    if (level == nullptr) {
        return -1;
    }

    return *reinterpret_cast<std::int32_t*>(level + LevelIdOffset);
}

auto DyingInProtectedLevel(void* player) noexcept -> bool {
    return Armed.load(std::memory_order_acquire) && IsProtectedLevel(LevelIdOfUnit(player));
}

// ---------------------------------------------------------------------------
// The hooks
// ---------------------------------------------------------------------------

// Installed only when protect_experience is on. Skipping the whole function
// skips the gold stage with it, so when gold is deliberately left alone the
// gold stage is run here by hand before returning.
auto __fastcall HookDeathPenalty(void* game, void* player, void* killer) noexcept
        -> std::uint64_t {
    if (!DyingInProtectedLevel(player)) {
        return OriginalDeathPenalty(game, player, killer);
    }

    if (!Config.protectGold && NativeGoldPenalty != nullptr) {
        NativeGoldPenalty(game, player, killer);
    }

    ExperienceSaves.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

// Installed only when protect_gold is on. It is reached only when the
// experience hook let the original run, since that hook returns before the
// gold stage is ever called.
auto __fastcall HookGoldPenalty(void* game, void* player, void* killer) noexcept
        -> std::uint64_t {
    if (!DyingInProtectedLevel(player)) {
        return OriginalGoldPenalty(game, player, killer);
    }

    GoldSaves.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

// ---------------------------------------------------------------------------
// Config file reading (small hand-rolled TOML subset, no dependency)
// ---------------------------------------------------------------------------

struct Slice {
    const char* begin;
    const char* end;
};

constexpr auto IsBlank(char character) noexcept -> bool {
    return character == ' ' || character == '\t' || character == '\r';
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

auto EndOfLine(const char* line) noexcept -> const char* {
    while (*line != '\0' && *line != '\n') {
        ++line;
    }
    return line;
}

// Strips a trailing # comment, then trims.
auto CleanLine(const char* begin, const char* end) noexcept -> Slice {
    Slice raw{ begin, end };
    for (const char* cursor = raw.begin; cursor < raw.end; ++cursor) {
        if (*cursor == '#') {
            raw.end = cursor;
            break;
        }
    }
    return Trim(raw);
}

// Finds the raw right-hand side of section.key on a single line. Last
// assignment wins, and the caller's value is left alone unless the key was
// present and parsed cleanly.
auto FindConfigValue(const char* toml, const char* section, const char* key,
                     Slice& value) noexcept -> bool {
    if (toml == nullptr) {
        return false;
    }

    std::array<char, 64> currentSection{};
    bool                 found = false;

    for (const char* line = toml; *line != '\0';) {
        const char* lineEnd = EndOfLine(line);
        const Slice trimmed = CleanLine(line, lineEnd);

        if (trimmed.begin < trimmed.end) {
            if (*trimmed.begin == '[') {
                const char* close = trimmed.begin;
                while (close < trimmed.end && *close != ']') {
                    ++close;
                }
                const Slice name = Trim({ trimmed.begin + 1, close });
                std::size_t used = 0;
                for (const char* cursor = name.begin;
                        cursor < name.end && used + 1 < currentSection.size(); ++cursor) {
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
                    if (std::strcmp(currentSection.data(), section) == 0
                            && SliceEquals(name, key)) {
                        value = Trim({ equals + 1, trimmed.end });
                        found = true;
                    }
                }
            }
        }

        line = (*lineEnd == '\0') ? lineEnd : lineEnd + 1;
    }

    return found;
}

auto ReadConfigBool(const char* toml, const char* section, const char* key,
                    bool& value) noexcept -> bool {
    Slice raw{};
    if (!FindConfigValue(toml, section, key, raw)) {
        return false;
    }
    if (SliceEquals(raw, "true")) {
        value = true;
        return true;
    }
    if (SliceEquals(raw, "false")) {
        value = false;
        return true;
    }
    return false;
}

// Collects everything between the brackets of section.key, across as many
// lines as the array spans, with # comments removed. Returns false when the
// key is absent or the array is never closed.
auto ReadConfigArrayText(const char* toml, const char* section, const char* key,
                         std::string& out, bool& unterminated) noexcept -> bool {
    unterminated = false;
    if (toml == nullptr) {
        return false;
    }

    std::array<char, 64> currentSection{};
    std::string          capture;
    bool                 capturing = false;
    bool                 found     = false;

    const auto absorb = [&](const char* begin, const char* end) noexcept -> bool {
        for (const char* cursor = begin; cursor < end; ++cursor) {
            if (*cursor == ']') {
                out       = capture;
                found     = true;
                capturing = false;
                return true;
            }
            if (capture.size() < MaximumArrayChars) {
                capture.push_back(*cursor);
            }
        }
        return false;
    };

    for (const char* line = toml; *line != '\0';) {
        const char* lineEnd = EndOfLine(line);
        const Slice trimmed = CleanLine(line, lineEnd);

        if (capturing) {
            absorb(trimmed.begin, trimmed.end);
        } else if (trimmed.begin < trimmed.end) {
            if (*trimmed.begin == '[') {
                const char* close = trimmed.begin;
                while (close < trimmed.end && *close != ']') {
                    ++close;
                }
                const Slice name = Trim({ trimmed.begin + 1, close });
                std::size_t used = 0;
                for (const char* cursor = name.begin;
                        cursor < name.end && used + 1 < currentSection.size(); ++cursor) {
                    currentSection[used++] = *cursor;
                }
                currentSection[used] = '\0';
            } else {
                const char* equals = trimmed.begin;
                while (equals < trimmed.end && *equals != '=') {
                    ++equals;
                }
                if (equals < trimmed.end) {
                    const Slice name  = Trim({ trimmed.begin, equals });
                    const Slice value = Trim({ equals + 1, trimmed.end });
                    if (std::strcmp(currentSection.data(), section) == 0
                            && SliceEquals(name, key)
                            && value.begin < value.end && *value.begin == '[') {
                        capture.clear();
                        capturing = true;
                        absorb(value.begin + 1, value.end);
                    }
                }
            }
        }

        line = (*lineEnd == '\0') ? lineEnd : lineEnd + 1;
    }

    unterminated = capturing;
    return found;
}

auto ParseLevelId(std::string_view text, std::int32_t& value) noexcept -> bool {
    if (text.empty()) {
        return false;
    }

    std::size_t  index = 0;
    std::int64_t parsed = 0;
    int          base   = 10;

    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base  = 16;
        index = 2;
    }

    bool anyDigit = false;
    for (; index < text.size(); ++index) {
        const char character = text[index];
        if (character == '_') {
            continue;
        }

        int digit = -1;
        if (character >= '0' && character <= '9') {
            digit = character - '0';
        } else if (base == 16 && character >= 'a' && character <= 'f') {
            digit = character - 'a' + 10;
        } else if (base == 16 && character >= 'A' && character <= 'F') {
            digit = character - 'A' + 10;
        }

        if (digit < 0 || digit >= base) {
            return false;
        }

        anyDigit = true;
        parsed   = parsed * base + digit;
        if (parsed > MaximumLevelId + 1) {
            return false;
        }
    }

    if (!anyDigit) {
        return false;
    }

    value = static_cast<std::int32_t>(parsed);
    return true;
}

auto TrimView(std::string_view text) noexcept -> std::string_view {
    std::size_t first = 0;
    while (first < text.size()
            && (IsBlank(text[first]) || text[first] == '\n')) {
        ++first;
    }
    std::size_t last = text.size();
    while (last > first
            && (IsBlank(text[last - 1]) || text[last - 1] == '\n')) {
        --last;
    }
    return text.substr(first, last - first);
}

// Splits the captured array text on commas and records every element that is
// a level id in range. Returns how many elements were rejected.
auto ApplyLevelList(std::string_view text) noexcept -> std::int32_t {
    std::int32_t rejected = 0;
    std::size_t  cursor   = 0;

    while (cursor <= text.size()) {
        const std::size_t comma = text.find(',', cursor);
        const std::size_t end   = comma == std::string_view::npos ? text.size() : comma;
        const std::string_view element = TrimView(text.substr(cursor, end - cursor));
        cursor = end + 1;

        if (!element.empty()) {
            std::int32_t levelId = 0;
            if (ParseLevelId(element, levelId) && levelId >= MinimumLevelId
                    && levelId <= MaximumLevelId) {
                if (!IsProtectedLevel(levelId)) {
                    MarkProtectedLevel(levelId);
                    ++ProtectedCount;
                }
            } else {
                ++rejected;
            }
        }

        if (comma == std::string_view::npos) {
            break;
        }
    }

    return rejected;
}

void ReadConfiguration(const D2RL::PluginContext* context) noexcept {
    if (!context->EnsureConfig(DefaultConfigToml)) {
        context->LogWarn("Config file could not be created. Using defaults, "
                         "which protect no levels at all.");
        return;
    }

    std::string buffer(MaximumConfigBytes, '\0');
    if (!context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size()),
                             nullptr)) {
        context->LogWarn("Config file could not be read. Using defaults, "
                         "which protect no levels at all.");
        return;
    }

    buffer.resize(std::strlen(buffer.c_str()));
    ConfigFileWasRead = true;

    (void)ReadConfigBool(buffer.c_str(), ConfigSection, "enabled", Config.enabled);
    (void)ReadConfigBool(buffer.c_str(), ConfigSection, "protect_experience",
                         Config.protectExperience);
    (void)ReadConfigBool(buffer.c_str(), ConfigSection, "protect_gold",
                         Config.protectGold);

    std::string arrayText;
    bool        unterminated = false;
    if (!ReadConfigArrayText(buffer.c_str(), ConfigSection, "protected_levels",
                             arrayText, unterminated)) {
        if (unterminated) {
            context->LogWarn("protected_levels is missing its closing bracket. "
                             "No levels are protected.");
        }
        return;
    }

    const std::int32_t rejected = ApplyLevelList(arrayText);
    if (rejected > 0) {
        D2RL::LogWarnF(context,
            "protected_levels: %d entr%s ignored. Every entry must be a whole "
            "number from %d to %d.",
            rejected, rejected == 1 ? "y was" : "ies were",
            MinimumLevelId, MaximumLevelId);
    }
}

// ---------------------------------------------------------------------------
// Verification and installation
// ---------------------------------------------------------------------------

// Checks the two accessor bodies the level walk copies its offsets from, plus
// the entry of the room lookup it calls. A mismatch here means the offsets in
// this file can no longer be trusted for this build.
auto VerifyLevelLookup(const D2RL::PluginContext* context) noexcept -> bool {
    if (!context->CheckExpectedBytes(UnitsGetRoomRva, UnitsGetRoomEntry,
                                     ByteCount(UnitsGetRoomEntry))) {
        context->LogError(
            "NOT installed: UNITS_GetRoom at RVA 0034B440 does not match the "
            "verified 3.3.93847 entry. Nothing was patched.");
        return false;
    }

    if (!context->CheckExpectedBytes(RoomToDrlgRva, RoomToDrlgBody,
                                     ByteCount(RoomToDrlgBody))) {
        context->LogError(
            "NOT installed: the active-room accessor at RVA 002EFC10 no longer "
            "reads room + 0x18, so the level lookup cannot be trusted. "
            "Nothing was patched.");
        return false;
    }

    if (!context->CheckExpectedBytes(DrlgToLevelRva, DrlgToLevelBody,
                                     ByteCount(DrlgToLevelBody))) {
        context->LogError(
            "NOT installed: the level-id accessor at RVA 00360FC0 no longer "
            "reads room + 0x90 and level + 0x1F8, so the level lookup cannot "
            "be trusted. Nothing was patched.");
        return false;
    }

    return true;
}

auto VerifyDeathPenalty(const D2RL::PluginContext* context) noexcept -> bool {
    if (!context->CheckExpectedBytes(DeathPenaltyRva, DeathPenaltyEntry,
                                     ByteCount(DeathPenaltyEntry))) {
        context->LogError(
            "NOT installed: the death penalty at RVA 00424AC0 does not match "
            "the verified 3.3.93847 entry, or another plugin already owns it. "
            "Nothing was patched.");
        return false;
    }

    if (!context->CheckExpectedBytes(ExperienceWriteRva, ExperienceWriteWitness,
                                     ByteCount(ExperienceWriteWitness))) {
        context->LogError(
            "NOT installed: the experience write at RVA 00424BEE does not "
            "match the verified 3.3.93847 bytes, so RVA 00424AC0 cannot be "
            "confirmed as the function that takes experience away. "
            "Nothing was patched.");
        return false;
    }

    return true;
}

auto VerifyGoldPenalty(const D2RL::PluginContext* context) noexcept -> bool {
    if (!context->CheckExpectedBytes(GoldPenaltyRva, GoldPenaltyEntry,
                                     ByteCount(GoldPenaltyEntry))) {
        context->LogError(
            "Gold protection NOT installed: the gold stage at RVA 00425CB0 "
            "does not match the verified 3.3.93847 entry, or another plugin "
            "already owns it.");
        return false;
    }
    return true;
}

// Returns true when at least one hook is live.
auto InstallHooks(const D2RL::PluginContext* context) noexcept -> bool {
    if (!Config.enabled) {
        ExperienceHook.store(HookState::DisabledByConfig, std::memory_order_release);
        GoldHook.store(HookState::DisabledByConfig, std::memory_order_release);
        context->LogInfo("Not installed: turned off in the config file.");
        return false;
    }

    if (ProtectedCount == 0) {
        ExperienceHook.store(HookState::NoProtectedLevels, std::memory_order_release);
        GoldHook.store(HookState::NoProtectedLevels, std::memory_order_release);
        context->LogInfo(
            "Not installed: protected_levels is empty, so there is nothing to "
            "protect. Add level ids to the config file and restart.");
        return false;
    }

    if (!Config.protectExperience && !Config.protectGold) {
        ExperienceHook.store(HookState::DisabledByConfig, std::memory_order_release);
        GoldHook.store(HookState::DisabledByConfig, std::memory_order_release);
        context->LogInfo(
            "Not installed: protect_experience and protect_gold are both off.");
        return false;
    }

    if (!VerifyLevelLookup(context)) {
        ExperienceHook.store(HookState::UnsupportedBuild, std::memory_order_release);
        GoldHook.store(HookState::UnsupportedBuild, std::memory_order_release);
        return false;
    }

    // The gold stage first: it is the inner function, and the experience hook
    // short-circuits it entirely when both are on.
    if (Config.protectGold) {
        if (!VerifyGoldPenalty(context)) {
            GoldHook.store(HookState::UnsupportedBuild, std::memory_order_release);
        } else if (!context->InstallInlineHook(GoldPenaltyRva, GoldPenaltyPrologue,
                                               ByteCount(GoldPenaltyPrologue),
                                               HookGoldPenalty, &OriginalGoldPenalty)
                   || OriginalGoldPenalty == nullptr) {
            GoldHook.store(HookState::InstallFailed, std::memory_order_release);
            context->LogError(
                "Gold protection NOT installed: InstallInlineHook failed at "
                "RVA 00425CB0.");
        } else {
            GoldHook.store(HookState::Installed, std::memory_order_release);
        }
    } else {
        GoldHook.store(HookState::DisabledByConfig, std::memory_order_release);
    }

    if (Config.protectExperience) {
        if (!VerifyDeathPenalty(context)) {
            ExperienceHook.store(HookState::UnsupportedBuild, std::memory_order_release);
        } else if (!context->InstallInlineHook(DeathPenaltyRva, DeathPenaltyPrologue,
                                               ByteCount(DeathPenaltyPrologue),
                                               HookDeathPenalty, &OriginalDeathPenalty)
                   || OriginalDeathPenalty == nullptr) {
            ExperienceHook.store(HookState::InstallFailed, std::memory_order_release);
            context->LogError(
                "Experience protection NOT installed: InstallInlineHook failed "
                "at RVA 00424AC0.");
        } else {
            ExperienceHook.store(HookState::Installed, std::memory_order_release);
        }
    } else {
        ExperienceHook.store(HookState::DisabledByConfig, std::memory_order_release);
    }

    const bool experienceLive =
        ExperienceHook.load(std::memory_order_acquire) == HookState::Installed;
    const bool goldLive = GoldHook.load(std::memory_order_acquire) == HookState::Installed;

    if (!experienceLive && !goldLive) {
        return false;
    }

    // Only now, so a hook that fires between installation and this point
    // still forwards to the original instead of reading a half-built set.
    Armed.store(true, std::memory_order_release);

    D2RL::LogInfoF(context,
        "Installed. %d protected level%s, experience %s, gold %s.",
        ProtectedCount, ProtectedCount == 1 ? "" : "s",
        experienceLive ? "protected" : (Config.protectExperience ? "FAILED" : "off"),
        goldLive ? "protected" : (Config.protectGold ? "FAILED" : "off"));
    return true;
}

// ---------------------------------------------------------------------------
// Console command
// ---------------------------------------------------------------------------

auto StateText(HookState state) noexcept -> const char* {
    switch (state) {
    case HookState::Installed:         return "active";
    case HookState::DisabledByConfig:  return "off in the config file";
    case HookState::NoProtectedLevels: return "off, protected_levels is empty";
    case HookState::UnsupportedBuild:  return "NOT ACTIVE, unrecognised game build";
    case HookState::InstallFailed:     return "NOT ACTIVE, the hook failed, see the log";
    case HookState::NotAttempted:
    default:                           return "NOT ACTIVE, never attempted";
    }
}

auto __cdecl SafeLevelsCommand(D2R::Game::Client*,
        const D2RL::ConsoleCommandContext* command, void*) noexcept
        -> D2RL::ConsoleCommandResult {
    if (command == nullptr || command->plugin == nullptr) {
        return D2RL::ConsoleCommandResult::Failed;
    }
    const D2RL::PluginContext* context = command->plugin;

    char header[224]{};
    std::snprintf(header, sizeof(header),
        "death-penalty-safe-levels: config %s, %d protected level%s.",
        ConfigFileWasRead ? "loaded" : "NOT READ (defaults in use)",
        ProtectedCount, ProtectedCount == 1 ? "" : "s");
    context->WriteConsoleMessage(header);

    char body[256]{};
    std::snprintf(body, sizeof(body),
        "experience: %s, deaths made free so far: %llu | gold: %s, deaths made "
        "free so far: %llu",
        StateText(ExperienceHook.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(ExperienceSaves.load(std::memory_order_relaxed)),
        StateText(GoldHook.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(GoldSaves.load(std::memory_order_relaxed)));
    context->WriteConsoleMessage(body);

    // The list itself, wrapped so a long one stays readable.
    char line[256]{};
    int  used  = std::snprintf(line, sizeof(line), "levels:");
    bool empty = true;
    for (std::int32_t levelId = MinimumLevelId; levelId <= MaximumLevelId; ++levelId) {
        if (!IsProtectedLevel(levelId)) {
            continue;
        }
        empty = false;
        if (used > static_cast<int>(sizeof(line)) - 8) {
            context->WriteConsoleMessage(line);
            used = std::snprintf(line, sizeof(line), "levels:");
        }
        used += std::snprintf(line + used, sizeof(line) - static_cast<std::size_t>(used),
                              " %d", levelId);
    }
    context->WriteConsoleMessage(empty ? "levels: none" : line);

    return D2RL::ConsoleCommandResult::Handled;
}

constexpr D2RL::PluginInfo Info{
    .infoSize    = D2RL::PluginInfoSize,
    .apiVersion  = D2RL_PLUGIN_API_VERSION,
    .id          = "celestialrayone.death-penalty-safe-levels",
    .name        = "Death Penalty Safe Levels",
    .version     = "1.0.0",
    .author      = "CelestialRayOne",
    .description = "Skips the experience and gold loss on death in a "
                   "configurable list of levels.",
    // Server: the death penalty is applied server side only.
    .flags       = D2RL::PluginFlags::Server | D2RL::PluginFlags::NativeHooks,
};

static_assert(D2RL::HasValidPluginRole(Info.flags),
              "Exactly one execution role must be set.");
static_assert(D2RL::HasOnlyKnownPluginFlags(Info.flags), "Unknown plugin flag set.");
static_assert(D2RL::HasFlag(Info.flags, D2RL::PluginFlags::NativeHooks),
              "Inline hooks require the NativeHooks flag.");

}  // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
    return &Info;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept
        -> bool {
    if (!D2RL::HasContext(context)) {
        return false;
    }
    Context = context;

    UnitsGetRoom = reinterpret_cast<UnitsGetRoomFn>(context->exeBase + UnitsGetRoomRva);
    NativeGoldPenalty =
        reinterpret_cast<DeathPenaltyFn>(context->exeBase + GoldPenaltyRva);

    // Registered before anything is installed on purpose. Returning false from
    // this function unloads the DLL and takes the console command with it,
    // which would leave someone on an unsupported build with no in-game signal
    // at all. This plugin would rather stay loaded and be able to say that it
    // did nothing.
    if (!context->RegisterConsoleCommand("safelevels", &SafeLevelsCommand,
            "Reports which levels are safe to die in and whether the hooks are live.")) {
        context->LogWarn("The status console command was refused.");
    }

    ReadConfiguration(context);
    InstallHooks(context);
    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    // The SDK exposes no hook removal, so the trampolines stay. Disarming
    // makes both hooks pure pass-throughs from here on.
    Armed.store(false, std::memory_order_release);
}

}  // namespace CelestialRayOne::DeathPenaltySafeLevels
