// =============================================================================
//  Armageddon as Aura  -  celestialrayone.armageddon-aura
//
//  Makes skills that use skills.txt srvdofunc 124 (Armageddon, and in D2R 3.3
//  also Hurricane) bootstrap correctly when they are driven as an aura rather
//  than cast by hand.
//
//  THE DEFECT
//
//  SKILLS_SrvDo124 opens with a cast-context gate:
//
//      used = UNITS_GetUsedSkill(unit);          // [[unit+0x100]+0x18]
//      if (!used || SkillId(used) != skillId) return 0;
//
//  Nothing on the aura side ever installs that pointer. The item_aura
//  (itemstatcost 151) driver calls the server skill do-handler with its
//  itemCast argument set, and the do-handler skips its entire used-skill
//  resolution block on that path. Same for the periodic re-fire of a learned
//  aura and for the re-fire after a respawn. So 124 is called on schedule,
//  self-rejects, and never creates the aura state nor schedules the
//  ACTIVESTATE periodic event that actually performs the skill.
//
//  THE FIX
//
//  Wrap 124. If the used-skill pointer already names this skill we are on a
//  real cast: step aside, the stock handler runs untouched. Otherwise resolve
//  the skill from the unit's own skill list with the SAME resolver and the
//  SAME selection flag that srvdofunc 146 uses (0x33DD40, selectionFlag =
//  false), install it as the used skill for the duration of one call, run the
//  stock handler through the trampoline, then restore the previous pointer.
//
//  Resolving it exactly the way 146 does matters: 124 stores the periodic
//  seed at skillNode+0x24 and 146 reads it back from the node its own
//  resolver returns. A different node means the seed lands somewhere 146
//  never looks.
//
//  SEQUENCE FREEZE
//
//  Stock 124 sets the unit's "skill action taken this frame" flag
//  (unit+0x124 bit 0x40) immediately after its gate. The per-frame animation
//  code refuses to advance the current sequence while that bit is set. Fine
//  for a one-shot cast; fatal for an aura, which re-sets it every few frames
//  and freezes a player mid-leap or mid-whirlwind forever. On the borrowed
//  path only, this plugin restores the bit to whatever it was before the
//  call. Real casts never reach that code.
//
//  Ported from the ESR 2.4 byte patch (hook 363780 -> 191-byte cave at
//  386C60). No cave is needed here; the loader owns the trampoline, which
//  also frees 386C60 and removes the srvdofunc-73 dispatch landmine that
//  cave sat on.
// =============================================================================

#include <D2RLPlugin/api.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace CelestialRayOne::ArmageddonAura {
namespace {

constexpr char PluginVersion[] = "1.0.0";

// ---------------------------------------------------------------------------
//  Native struct layout (all offsets proven from the 3.3 image, see below)
// ---------------------------------------------------------------------------

// Unit -> D2SkillList. Proven by the tail of UNITS_GetSkillList (0x34B6E0),
// which ends in  mov rax,[rbx+0x100].
constexpr std::size_t UnitSkillListOffset = 0x100;

// D2SkillList +0x18 = "skill currently being used". Proven by
// UNITS_GetUsedSkill (0x34BA40) -> 0x33E090, which returns [list+0x18].
constexpr std::size_t SkillListUsedIndex = 0x18 / sizeof(void*);

// Unit +0x124, bit 0x40 = "a skill action was taken this frame". Proven by
// the setter at 0x34E790:  mov eax,[rcx+0x124] / or eax,0x40  (or and 0xBF).
constexpr std::size_t   UnitActionFlagsOffset = 0x124;
constexpr std::uint32_t SkillActionTakenBit   = 0x40;

// ---------------------------------------------------------------------------
//  Native RVAs (D2R 3.3, D2RLoader process image, base 0x140000000)
// ---------------------------------------------------------------------------

// SKILLS_SrvDo124_Armageddon_Hurricane(game, unit, skillId, skillLevel)
constexpr std::uint64_t SrvDo124Rva = 0x575DE0;

// UNITS_GetSkillList(unit) -> [unit+0x100]
constexpr std::uint64_t GetSkillListRva = 0x34B6E0;

// SKILLS_GetSkillId(skillNode) -> *(int16*)*(void**)skillNode
constexpr std::uint64_t GetSkillIdRva = 0x33E080;

// SKILLS_GetHighestLevelSkillFromUnitAndId(unit, skillId, selectionFlag)
constexpr std::uint64_t GetHighestLevelSkillRva = 0x33DD40;

// UNITS_SetSkillActionTaken(unit, nonZeroToSet) -> unit+0x124 bit 0x40
constexpr std::uint64_t SetSkillActionTakenRva = 0x34E790;

// srvdofunc dispatch table. Slot 124 must resolve to SrvDo124Rva; that is the
// load-time proof that we hooked the function skills.txt actually dispatches.
constexpr std::uint64_t SrvDoTableRva    = 0x238EA00;
constexpr std::size_t   SrvDo124TableIdx = 124;

// ---------------------------------------------------------------------------
//  Entry byte signatures, read straight out of the 3.3 image
// ---------------------------------------------------------------------------

constexpr auto SrvDo124Expected = std::to_array<std::uint8_t>({
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x44, 0x89, 0x4C,
    0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41,
    0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC,
    0x40, 0x48, 0x8B, 0xF2, 0x48, 0x8B, 0xE9,
});

constexpr auto GetSkillListExpected = std::to_array<std::uint8_t>({
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
    0xD9, 0x48, 0x85, 0xC9, 0x75, 0x20, 0x88, 0x4C,
    0x24, 0x30, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8,
    0xD4, 0x98, 0xFF, 0xFF, 0x84, 0xC0, 0x74, 0x01,
    0xCC, 0x48, 0x8B, 0x83, 0x00, 0x01, 0x00, 0x00,
});

constexpr auto GetSkillIdExpected = std::to_array<std::uint8_t>({
    0x48, 0x85, 0xC9, 0x75, 0x03, 0x33, 0xC0, 0xC3,
    0x48, 0x8B, 0x01, 0x0F, 0xBF, 0x00, 0xC3,
});

constexpr auto GetHighestLevelSkillExpected = std::to_array<std::uint8_t>({
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
    0xEC, 0x20, 0x41, 0x0F, 0xB6, 0xD8, 0x8B, 0xFA,
    0xE8, 0x8B, 0xD9, 0x00, 0x00, 0x48, 0x85, 0xC0,
});

constexpr auto SetSkillActionTakenExpected = std::to_array<std::uint8_t>({
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x85, 0xC9, 0x75,
    0x18, 0x88, 0x4C, 0x24, 0x30, 0x48, 0x8D, 0x4C,
    0x24, 0x30, 0xE8, 0x29, 0x6F, 0xFF, 0xFF, 0x84,
    0xC0, 0x74, 0x27, 0xCC, 0x48, 0x83, 0xC4, 0x28,
});

// ---------------------------------------------------------------------------
//  Native signatures
// ---------------------------------------------------------------------------

using ServerSkillFn =
    std::int32_t(__fastcall*)(void*, void*, std::int32_t, std::int32_t) noexcept;
using GetSkillListFn         = void*(__fastcall*)(void*) noexcept;
using GetSkillIdFn           = std::int32_t(__fastcall*)(void*) noexcept;
using GetHighestLevelSkillFn = void*(__fastcall*)(void*, std::int32_t, bool) noexcept;
using SetSkillActionTakenFn  = void(__fastcall*)(void*, std::int32_t) noexcept;

// ---------------------------------------------------------------------------
//  Default configuration. This text is what EnsureConfig writes to
//  <scope>\d2rloader\config\celestialrayone.armageddon-aura.toml on first run,
//  so it is both the defaults and the plugin's documentation. Keep it in sync
//  with config/celestialrayone.armageddon-aura.toml in the repo.
// ---------------------------------------------------------------------------

constexpr char DefaultConfigToml[] = R"TOML(
# =============================================================================
#  Armageddon as Aura            plugin id: celestialrayone.armageddon-aura
#  Target: D2R 3.3 (D2RLoader image)                      Role: Server
# =============================================================================
#
#  WHAT IS BROKEN IN THE STOCK GAME
#
#  Armageddon is written as a CAST skill. Before its server handler does
#  anything at all it asks the engine "is this unit casting me right now?".
#  It answers that by reading the unit's "skill currently being used" pointer
#  and comparing that skill's id against its own. If they do not match it
#  returns immediately and does nothing.
#
#  That gate is correct for a normal cast. It is fatal for an aura, because
#  none of the paths that drive an aura ever fill that pointer in:
#
#    * An item_aura grant (itemstatcost 151) re-fires the skill on a timer.
#      The engine calls the skill handler with its "item cast" argument set,
#      and on that path it skips the whole block that would have resolved
#      and installed the used skill.
#    * The periodic re-fire that keeps a learned aura alive does the same.
#    * So does the re-fire that happens when you respawn.
#
#  The handler is therefore called on schedule, bails at its own first line,
#  and never creates the aura state or schedules the periodic event that
#  makes Armageddon actually rain. What you see in game is:
#
#    * The aura does nothing, or only works right after you manually cast it.
#    * It dies permanently on death, because the state is torn down when you
#      die and nothing ever bootstraps it again.
#    * When it is forced to run some other way, it runs with no owner
#      context, so anything its calcs read about the owner is garbage.
#
#  WHAT THIS PLUGIN DOES
#
#  It wraps the Armageddon server handler. On entry it reads the unit's
#  "skill currently being used" pointer and takes one of two paths:
#
#    * REAL CAST. The pointer already names this skill. The plugin steps
#      aside entirely and the stock handler runs untouched. Nothing about a
#      normal cast changes in any way.
#
#    * BORROWED CONTEXT. The pointer is empty or names a different skill.
#      The plugin looks this skill up on the unit's own skill list, lends it
#      to the engine as the used skill for the duration of exactly one call,
#      runs the stock handler, and then puts the previous value back. The
#      handler now passes its own gate and performs its full, ordinary
#      bootstrap: it creates the aura state, schedules the periodic event,
#      and stores the random seed that the active-state handler consumes on
#      every tick.
#
#  Because the skill is borrowed from the unit's real skill list rather than
#  faked, the aura runs with a proper owner context. That is what makes owner
#  stats and the real skill level readable inside its calcs, and it is why
#  the aura comes back by itself after you die instead of needing a re-equip.
#
#  IT ALSO FIXES THE LEAP / WHIRLWIND FREEZE
#
#  The stock handler sets the unit's "a skill action was taken this frame"
#  flag the moment it gets past its gate. The per-frame animation code reads
#  that flag and, when it is set, does not advance the unit's current
#  sequence. That is harmless for a one-off cast, which happens once.
#
#  It is not harmless for an aura. The aura ticks every few frames and re-sets
#  the flag each time, so a player who is mid-leap or mid-whirlwind is frozen
#  in place for as long as the aura is running.
#
#  On the borrowed-context path only, this plugin puts that flag back exactly
#  the way it found it once the handler returns. If the flag was already set
#  because the player genuinely acted on that frame, it stays set. Real casts
#  never reach this code and keep the flag exactly as the game intended.
#
#  WHAT YOU NEED IN YOUR TXT FILES
#
#    * skills.txt: srvdofunc = 124 on the skill, and aura = 1.
#    * The skill must be GRANTED to the unit, normally as an oskill, so that
#      it exists on the unit's skill list. That is the skill the plugin
#      lends. With no grant there is nothing to lend and the plugin steps
#      aside, leaving the stock (broken) behaviour.
#    * Do NOT also apply the aurastate from a property. The handler creates
#      that state itself now, and a second property-applied copy of the same
#      state fights with it.
#    * states.txt: leave the aurastate's srvactivefunc alone. That is what
#      drives the per-tick Armageddon behaviour, and it only ever runs
#      because this plugin got the state created in the first place.
#
#  NOTE FOR 3.3: the srvdofunc 124 handler is shared by Armageddon AND
#  Hurricane. With an empty skill list below, both are covered.
#
# =============================================================================


# Master switch. Set to false to unload the fix and leave the game stock.
enabled = true


[skills]
# Which skills the fix applies to, listed by their skills.txt *Id.
#
# Leave the list EMPTY to apply it to every skill that uses srvdofunc 124.
# That is the recommended setting and matches the behaviour of the original
# byte patch this plugin replaces.
#
# Fill it in only if you want to scope the fix. Because srvdofunc 124 is
# shared with Hurricane in 3.3, an empty list covers Hurricane too.
#
# Example:  ids = [ 249, 1305 ]
ids = []


[sequence]
# Restore the "skill action taken this frame" flag to whatever it was before
# the handler ran, on the borrowed-context path only. This is the leap /
# whirlwind freeze fix described above.
#
# Leave this on. Turn it off only to prove that some other patch is causing a
# sequence stall, because turning it off brings the freeze back.
restore_action_flag = true


[diagnostics]
# Writes a short burst of lines to the plugin log covering the first few
# jumpstarts, then goes quiet so it cannot spam a per-frame path.
#
# Running counters are always available from the in-game console command
# "armageddon-aura", whether this is on or off.
enabled = false
)TOML";

// ---------------------------------------------------------------------------
//  Configuration
// ---------------------------------------------------------------------------

constexpr std::size_t MaxHandledSkills = 64;

struct Config {
    bool                      enabled{true};
    bool                      restoreActionFlag{true};
    bool                      diagnostics{false};
    std::vector<std::int32_t> skillIds{};
};

std::string Trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end   = text.size();
    const auto isSpace = [](char c) noexcept {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    while (begin < end && isSpace(text[begin])) ++begin;
    while (end > begin && isSpace(text[end - 1])) --end;
    return std::string(text.substr(begin, end - begin));
}

bool ParseBool(const std::string& value, bool& destination) {
    if (value == "true") {
        destination = true;
        return true;
    }
    if (value == "false") {
        destination = false;
        return true;
    }
    return false;
}

bool ParseIntArray(const std::string& value, std::vector<std::int32_t>& destination) {
    if (value.size() < 2 || value.front() != '[' || value.back() != ']') return false;

    destination.clear();
    const std::string inner = value.substr(1, value.size() - 2);
    std::size_t       pos   = 0;
    while (pos < inner.size()) {
        std::size_t comma = inner.find(',', pos);
        if (comma == std::string::npos) comma = inner.size();

        const std::string token = Trim(std::string_view(inner).substr(pos, comma - pos));
        if (!token.empty()) {
            char*      stop   = nullptr;
            const long parsed = std::strtol(token.c_str(), &stop, 10);
            if (stop == nullptr || *stop != '\0') return false;
            destination.push_back(static_cast<std::int32_t>(parsed));
        }
        pos = comma + 1;
    }
    return true;
}

// Deliberately small and forgiving: it understands exactly the keys this
// plugin owns, ignores anything else with a warning, and never refuses to
// load over a config typo. That keeps the plugin free of a TOML dependency,
// so dropping the folder in and adding one line to the build is enough.
bool ParseConfig(std::string_view text, Config& result, std::string& error, std::string& unknown) {
    Config      parsed{};
    std::string section;

    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end  = text.find('\n', pos);
        const bool  last = (end == std::string_view::npos);
        if (last) end = text.size();

        std::string line = Trim(text.substr(pos, end - pos));
        pos              = last ? text.size() : end + 1;

        const std::size_t comment = line.find('#');
        if (comment != std::string::npos) line = Trim(std::string_view(line).substr(0, comment));
        if (line.empty()) continue;

        if (line.front() == '[' && line.back() == ']') {
            section = Trim(std::string_view(line).substr(1, line.size() - 2));
            continue;
        }

        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) {
            error = "malformed line: " + line;
            return false;
        }

        const std::string key   = Trim(std::string_view(line).substr(0, equals));
        const std::string value = Trim(std::string_view(line).substr(equals + 1));
        const std::string qualified = section.empty() ? key : (section + "." + key);

        if (qualified == "enabled") {
            if (!ParseBool(value, parsed.enabled)) {
                error = "enabled must be true or false";
                return false;
            }
        } else if (qualified == "skills.ids") {
            if (!ParseIntArray(value, parsed.skillIds)) {
                error = "skills.ids must be a list of skill ids, for example [ 249, 1305 ]";
                return false;
            }
        } else if (qualified == "sequence.restore_action_flag") {
            if (!ParseBool(value, parsed.restoreActionFlag)) {
                error = "sequence.restore_action_flag must be true or false";
                return false;
            }
        } else if (qualified == "diagnostics.enabled") {
            if (!ParseBool(value, parsed.diagnostics)) {
                error = "diagnostics.enabled must be true or false";
                return false;
            }
        } else {
            if (!unknown.empty()) unknown += ", ";
            unknown += qualified;
        }
    }

    result = std::move(parsed);
    return true;
}

// ---------------------------------------------------------------------------
//  Plugin state
// ---------------------------------------------------------------------------

const D2RL::PluginContext* Context{};
std::uint8_t*              Base{};
Config                     Settings{};
char                       ConfigSource[32]{"built-in defaults"};
char                       RuntimeBuild[64]{"unknown"};
std::atomic_bool           Operational{};

GetSkillListFn         GetSkillList{};
GetSkillIdFn           GetSkillId{};
GetHighestLevelSkillFn GetHighestLevelSkill{};
SetSkillActionTakenFn  SetSkillActionTaken{};
ServerSkillFn          OriginalSrvDo124{};

// Copied out of Settings.skillIds at load so the hot path never touches a
// container that could be reallocated from another thread.
std::array<std::int32_t, MaxHandledSkills> HandledSkills{};
std::size_t                                HandledSkillCount{};

std::atomic<std::uint64_t> RealCastCalls{};
std::atomic<std::uint64_t> Jumpstarts{};
std::atomic<std::uint64_t> SkillNotGranted{};
std::atomic<std::uint64_t> SequenceFlagRestores{};
std::atomic<std::uint32_t> DiagnosticsBudget{};

void LogInfo(const char* message) noexcept {
    if (Context) Context->LogInfo(message);
}

void LogWarn(const char* message) noexcept {
    if (Context) Context->LogWarn(message);
}

void LogError(const char* message) noexcept {
    if (Context) Context->LogError(message);
}

// ---------------------------------------------------------------------------
//  Hot path
// ---------------------------------------------------------------------------

bool IsSkillHandled(std::int32_t skillId) noexcept {
    if (HandledSkillCount == 0) return true;  // empty list = every 124 skill
    for (std::size_t index = 0; index < HandledSkillCount; ++index) {
        if (HandledSkills[index] == skillId) return true;
    }
    return false;
}

bool HasSkillActionFlag(const void* unit) noexcept {
    const auto* flags = reinterpret_cast<const std::uint32_t*>(
        static_cast<const std::uint8_t*>(unit) + UnitActionFlagsOffset);
    return (*flags & SkillActionTakenBit) != 0;
}

void ReportJumpstart(std::int32_t skillId, std::int32_t skillLevel) noexcept {
    if (!Settings.diagnostics) return;
    if (DiagnosticsBudget.load(std::memory_order_relaxed) == 0) return;
    if (DiagnosticsBudget.fetch_sub(1, std::memory_order_relaxed) == 0) return;

    char message[192]{};
    std::snprintf(
        message,
        sizeof(message),
        "ArmageddonAura: jumpstarted skill %d at level %d from a borrowed cast context.",
        skillId,
        skillLevel);
    LogInfo(message);
}

std::int32_t __fastcall HookSrvDo124(
        void*        game,
        void*        unit,
        std::int32_t skillId,
        std::int32_t skillLevel) noexcept {
    const ServerSkillFn original = OriginalSrvDo124;
    if (original == nullptr) return 0;

    if (!Operational.load(std::memory_order_acquire) || unit == nullptr) {
        return original(game, unit, skillId, skillLevel);
    }
    if (!IsSkillHandled(skillId)) {
        return original(game, unit, skillId, skillLevel);
    }

    auto** skillList = static_cast<void**>(GetSkillList(unit));
    if (skillList == nullptr) {
        return original(game, unit, skillId, skillLevel);
    }

    void* const used = skillList[SkillListUsedIndex];
    if (used != nullptr && GetSkillId(used) == skillId) {
        // Genuine cast of this skill. The stock gate passes on its own, so
        // leave everything, including the sequence flag, exactly as it is.
        RealCastCalls.fetch_add(1, std::memory_order_relaxed);
        return original(game, unit, skillId, skillLevel);
    }

    // Same resolver and same selection flag that srvdofunc 146 uses, so the
    // node we lend is the node 146 later reads the periodic seed back from.
    void* const node = GetHighestLevelSkill(unit, skillId, false);
    if (node == nullptr) {
        // The unit does not have the skill. Almost always a missing oskill
        // grant. Nothing to lend, so run stock and leave the game alone.
        SkillNotGranted.fetch_add(1, std::memory_order_relaxed);
        return original(game, unit, skillId, skillLevel);
    }

    const bool actionFlagWasSet = HasSkillActionFlag(unit);

    skillList[SkillListUsedIndex] = node;
    const std::int32_t result     = original(game, unit, skillId, skillLevel);
    skillList[SkillListUsedIndex] = used;

    // Stock 124 sets the sequence-suppress bit once it is past its gate. Put
    // it back the way we found it so an aura tick cannot stall a leap or a
    // whirlwind. If it was already set because the player really did act this
    // frame, it stays set.
    if (Settings.restoreActionFlag && !actionFlagWasSet && HasSkillActionFlag(unit)) {
        SetSkillActionTaken(unit, 0);
        SequenceFlagRestores.fetch_add(1, std::memory_order_relaxed);
    }

    Jumpstarts.fetch_add(1, std::memory_order_relaxed);
    ReportJumpstart(skillId, skillLevel);
    return result;
}

// ---------------------------------------------------------------------------
//  Load-time validation
// ---------------------------------------------------------------------------

struct NativeSite {
    std::uint64_t       rva;
    const std::uint8_t* expected;
    std::uint32_t       expectedSize;
    const char*         name;
};

bool ValidateNativeImage() noexcept {
    const NativeSite sites[] = {
        {SrvDo124Rva, SrvDo124Expected.data(),
         static_cast<std::uint32_t>(SrvDo124Expected.size()), "srvdofunc 124 handler"},
        {GetSkillListRva, GetSkillListExpected.data(),
         static_cast<std::uint32_t>(GetSkillListExpected.size()), "UNITS_GetSkillList"},
        {GetSkillIdRva, GetSkillIdExpected.data(),
         static_cast<std::uint32_t>(GetSkillIdExpected.size()), "SKILLS_GetSkillId"},
        {GetHighestLevelSkillRva, GetHighestLevelSkillExpected.data(),
         static_cast<std::uint32_t>(GetHighestLevelSkillExpected.size()),
         "SKILLS_GetHighestLevelSkillFromUnitAndId"},
        {SetSkillActionTakenRva, SetSkillActionTakenExpected.data(),
         static_cast<std::uint32_t>(SetSkillActionTakenExpected.size()),
         "UNITS_SetSkillActionTaken"},
    };

    for (const auto& site : sites) {
        if (!Context->CheckExpectedBytes(site.rva, site.expected, site.expectedSize)) {
            char message[256]{};
            std::snprintf(
                message,
                sizeof(message),
                "ArmageddonAura: native fingerprint mismatch at 0x%llX (%s). Refusing to load.",
                static_cast<unsigned long long>(site.rva),
                site.name);
            LogError(message);
            return false;
        }
    }

    // Every fingerprint above already proved this is the expected image, so
    // reading the dispatch table is safe by this point. Slot 124 must resolve
    // to the function we are about to hook, otherwise skills.txt srvdofunc 124
    // dispatches somewhere else and hooking it would achieve nothing.
    auto* const  table = reinterpret_cast<void* const*>(Base + SrvDoTableRva);
    void* const  slot  = table[SrvDo124TableIdx];
    void* const  want  = reinterpret_cast<void*>(Base + SrvDo124Rva);
    if (slot != want) {
        char message[256]{};
        std::snprintf(
            message,
            sizeof(message),
            "ArmageddonAura: srvdofunc table slot 124 resolves to 0x%llX, expected 0x%llX. Refusing to load.",
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(slot)),
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(want)));
        LogError(message);
        return false;
    }

    return true;
}

void ResolveNativeFunctions() noexcept {
    GetSkillList = reinterpret_cast<GetSkillListFn>(Base + GetSkillListRva);
    GetSkillId   = reinterpret_cast<GetSkillIdFn>(Base + GetSkillIdRva);
    GetHighestLevelSkill =
        reinterpret_cast<GetHighestLevelSkillFn>(Base + GetHighestLevelSkillRva);
    SetSkillActionTaken =
        reinterpret_cast<SetSkillActionTakenFn>(Base + SetSkillActionTakenRva);
}

bool LoadConfig() noexcept {
    if (!Context->EnsureConfig(DefaultConfigToml)) {
        LogWarn("ArmageddonAura: config file could not be created, using built-in defaults.");
        return true;
    }

    try {
        std::vector<char> buffer(32768, '\0');
        std::uint32_t     required = 0;

        if (!Context->ReadConfig(
                buffer.data(), static_cast<std::uint32_t>(buffer.size()), &required)) {
            if (required == 0 || required <= buffer.size()) {
                LogWarn("ArmageddonAura: config file could not be read, using built-in defaults.");
                return true;
            }
            buffer.assign(static_cast<std::size_t>(required) + 1, '\0');
            if (!Context->ReadConfig(
                    buffer.data(), static_cast<std::uint32_t>(buffer.size()), nullptr)) {
                LogWarn("ArmageddonAura: config file could not be read, using built-in defaults.");
                return true;
            }
        }
        buffer.back() = '\0';

        Config      parsed{};
        std::string error;
        std::string unknown;
        if (!ParseConfig(std::string_view(buffer.data()), parsed, error, unknown)) {
            char message[320]{};
            std::snprintf(
                message,
                sizeof(message),
                "ArmageddonAura: config rejected (%s). Refusing to load so the setting you meant is not silently ignored.",
                error.c_str());
            LogError(message);
            return false;
        }

        if (!unknown.empty()) {
            char message[320]{};
            std::snprintf(
                message,
                sizeof(message),
                "ArmageddonAura: ignoring unknown config setting(s): %s.",
                unknown.c_str());
            LogWarn(message);
        }

        Settings = std::move(parsed);
        std::snprintf(ConfigSource, sizeof(ConfigSource), "%s", "config file");

        HandledSkillCount = 0;
        for (const std::int32_t id : Settings.skillIds) {
            if (HandledSkillCount >= MaxHandledSkills) {
                LogWarn("ArmageddonAura: skills.ids holds more than 64 entries, the extras are ignored.");
                break;
            }
            HandledSkills[HandledSkillCount++] = id;
        }
        return true;
    } catch (...) {
        LogWarn("ArmageddonAura: config could not be processed, using built-in defaults.");
        return true;
    }
}

// NOTE: there is deliberately no process-wide or session-wide lock here.
//
// An earlier revision took a named mutex in the Local\ namespace to refuse a
// second copy of this plugin. That namespace is per session, not per process,
// so any D2R process that had not finished exiting still owned the name and
// the next launch refused itself for no reason. The observed symptom was the
// Extensions tab showing the plugin as errored on one launch and loading
// cleanly on the next, with nothing wrong with the game image at all.
//
// Nothing is lost by removing it. D2RLoader already refuses to load two
// plugins with the same id, including across the global and mod scopes, and
// InstallInlineHook below refuses a second hook on the same function, which
// this plugin already reports with a clear message. Those are per-process
// guards on the thing that actually matters, which is the hook.

auto StatusCommand(
        D2R::Game::Client*,
        const D2RL::ConsoleCommandContext* command,
        void*) noexcept -> D2RL::ConsoleCommandResult {
    if (command == nullptr || command->plugin == nullptr) {
        return D2RL::ConsoleCommandResult::Failed;
    }

    char scope[192]{};
    if (HandledSkillCount == 0) {
        std::snprintf(scope, sizeof(scope), "every srvdofunc 124 skill");
    } else {
        int written = std::snprintf(scope, sizeof(scope), "skill ids");
        for (std::size_t index = 0; index < HandledSkillCount && written > 0
                                    && written < static_cast<int>(sizeof(scope)); ++index) {
            written += std::snprintf(
                scope + written,
                sizeof(scope) - static_cast<std::size_t>(written),
                " %d",
                HandledSkills[index]);
        }
    }

    char message[640]{};
    std::snprintf(
        message,
        sizeof(message),
        "Armageddon as Aura %s: %s; scope=%s; jumpstarts=%llu; real casts passed through=%llu; "
        "skill not granted=%llu; sequence flag restores=%llu; restore_action_flag=%s; config=%s; build=%s.",
        PluginVersion,
        Operational.load(std::memory_order_acquire) ? "active" : "inactive",
        scope,
        static_cast<unsigned long long>(Jumpstarts.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(RealCastCalls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(SkillNotGranted.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(SequenceFlagRestores.load(std::memory_order_relaxed)),
        Settings.restoreActionFlag ? "true" : "false",
        ConfigSource,
        RuntimeBuild);

    command->plugin->WriteConsoleMessage(message);
    return D2RL::ConsoleCommandResult::Handled;
}

void ResetState() noexcept {
    Operational.store(false, std::memory_order_release);
    Settings.enabled           = true;
    Settings.restoreActionFlag = true;
    Settings.diagnostics       = false;
    Settings.skillIds.clear();
    std::snprintf(ConfigSource, sizeof(ConfigSource), "%s", "built-in defaults");
    std::snprintf(RuntimeBuild, sizeof(RuntimeBuild), "%s", "unknown");
    HandledSkillCount = 0;
    HandledSkills.fill(0);
    RealCastCalls.store(0, std::memory_order_relaxed);
    Jumpstarts.store(0, std::memory_order_relaxed);
    SkillNotGranted.store(0, std::memory_order_relaxed);
    SequenceFlagRestores.store(0, std::memory_order_relaxed);
    DiagnosticsBudget.store(8, std::memory_order_relaxed);
}

}  // namespace
}  // namespace CelestialRayOne::ArmageddonAura

namespace {

constexpr D2RL::PluginInfo Info{
    .infoSize    = D2RL::PluginInfoSize,
    .apiVersion  = D2RL_PLUGIN_API_VERSION,
    .id          = "celestialrayone.armageddon-aura",
    .name        = "Armageddon as Aura",
    .version     = "1.0.0",
    .author      = "CelestialRayOne",
    .description = "Lets srvdofunc 124 skills bootstrap and persist as auras.",
    .flags       = D2RL::PluginFlags::Server | D2RL::PluginFlags::NativeHooks,
};

}  // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
    return &Info;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
    using namespace CelestialRayOne::ArmageddonAura;

    Context = context;
    Base    = context != nullptr ? reinterpret_cast<std::uint8_t*>(context->exeBase) : nullptr;
    ResetState();

    if (Context == nullptr || Base == nullptr) return false;

    if (context->buildName != nullptr) {
        std::snprintf(RuntimeBuild, sizeof(RuntimeBuild), "%s", context->buildName);
    }

    if (!LoadConfig()) return false;

    if (!Settings.enabled) {
        LogInfo("Armageddon as Aura 1.0.0 loaded disabled by config; the game runs stock.");
        return true;
    }

    if (!ValidateNativeImage()) return false;

    ResolveNativeFunctions();

    if (!Context->InstallInlineHook(
            SrvDo124Rva,
            SrvDo124Expected.data(),
            static_cast<std::uint32_t>(SrvDo124Expected.size()),
            &HookSrvDo124,
            &OriginalSrvDo124)
        || OriginalSrvDo124 == nullptr) {
        LogError("ArmageddonAura: the srvdofunc 124 handler is already hooked by another plugin, or the hook was refused.");
        return false;
    }

    Operational.store(true, std::memory_order_release);

    if (!Context->RegisterConsoleCommand(
            "armageddon-aura",
            StatusCommand,
            "Show Armageddon as Aura status and counters.")) {
        LogWarn("ArmageddonAura: status console command could not be registered.");
    }

    {
        char message[320]{};
        std::snprintf(
            message,
            sizeof(message),
            "Armageddon as Aura 1.0.0 by CelestialRayOne active; scope=%s; config=%s; build=%s.",
            HandledSkillCount == 0 ? "every srvdofunc 124 skill" : "explicit skill id list",
            ConfigSource,
            RuntimeBuild);
        LogInfo(message);
    }

    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    using namespace CelestialRayOne::ArmageddonAura;
    Operational.store(false, std::memory_order_release);
}
