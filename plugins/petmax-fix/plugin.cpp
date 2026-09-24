// ===========================================================================
// Pet Max Lock (celestialrayone.petmax-lock)
// ===========================================================================
//
// A skill whose skills.txt petmax formula works out to 0 or less cannot be
// cast at all. It gets the game's own wrong-weapon treatment, the one a
// throwing skill gets while a sword is equipped: the skill button shows the
// invalid look, the client refuses the cast and the character says
// "Impossible", and the server refuses the command. Skills with a blank petmax
// column are never touched. Players only.
//
// ---------------------------------------------------------------------------
// 1. Why petmax 0 still gives one pet
// ---------------------------------------------------------------------------
//   The summon code raises the evaluated petmax to at least 1 before it counts
//   pets (D2MOO PlayerPets.cpp: std::max(petmax, 1); Skills.cpp: "if (nPetMax
//   < 1) nPetMax = 1"). The plugin does not touch the summon code: it stops
//   the cast before it starts, through the check the game already uses for
//   a weapon that cannot use the skill.
//
// ---------------------------------------------------------------------------
// 2. The skill use check, 33F360 (unit, skill) -> state
// ---------------------------------------------------------------------------
//   D2Common's "can this unit use this skill now" check. 0 means usable.
//   2 is the weapon state: 33F1F0 matches the equipped weapons against the
//   skill's item type requirements (record words +38h / +44h, checked through
//   3406B0) and a failure returns 2. 1 is the mana state, 8 the cooldown gate
//   3404A0; other values are other reasons (no level, aura, passive...).
//   Its callers, and what they do with a non-zero state:
//     client  21B170, which feeds
//               - the skill button refresh 239380 (switch at 23948B): states
//                 2, 3, 4, 7 and 9 draw the layout's invalidTint (button
//                 +3036); state 1 would draw notEnoughManaTint (+2988)
//               - the input paths, e.g. 100760: any state but 0 and 5 refuses
//                 the cast and hands the feedback id from the table at 22A1F98
//                 (state 1 -> 22, states 2..4 -> 20, state 9 -> 21) to
//                 106B70, which plays the class voice line for it; id 20 is
//                 slot 2 of the class's sound table 22A5920, "Impossible"
//     server  4FDCA0 / 4FDEF0, the player skill command validation: any
//             non-zero state refuses the command
//   Both sides turn states 1, 2 and 4 on a skill flagged AttackNoMana (record
//   byte +27h, bit 10h) into a normal attack.
//   The hook runs the native check first and only turns a 0 into a 2, so
//   every native reason keeps its own state.
//
// ---------------------------------------------------------------------------
// 3. Reading petmax
// ---------------------------------------------------------------------------
//   The skill keyword switch 33B3B0 (param, dispatch at 33B3EA through the
//   table at 33C8A0) evaluates petmax as case 71: 33BFBD loads the formula at
//   skills record +114h and calls the evaluator 3B5160. Its case 58 loads
//   calc4 at +19Ch, the calc4 offset already proven in game, so the case
//   numbering is D2MOO's (71 petmax, 72 skpoints). A blank formula column
//   compiles to FFFFFFFFh, outside the formula pool, and is skipped.
//     skill -> record   [skill+0]; skill id = the record's first word (33E080)
//     level             33D1E0 (unit, skill, 1, 0), D2RCore ReadWideSkillLevel
//     context           34A0E0 (unit)
//     formula           3B5160 (context, unit, calc, skillId, level)
//     unit type         34B9D0 (unit), 0 = player
//   petmax is evaluated on every check, so a formula that depends on level,
//   synergies or stats locks and unlocks the skill as those change.
//
// ---------------------------------------------------------------------------
// 4. Other plugins
// ---------------------------------------------------------------------------
//   Nothing else in this repo hooks 33F360. crossbow-charges hooks the
//   cooldown gate 3404A0, which 33F360 calls; the two stack without contact.
//
// ===========================================================================

#include <D2RLPlugin/api.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

// ---------------------------------------------------------------------------
// Addresses (D2RLoader process image, D2R 3.3)
// ---------------------------------------------------------------------------

constexpr std::uint64_t UseStateRva          = 0x33F360;
constexpr std::uint64_t KeywordDispatchRva   = 0x33B3EA;
constexpr std::uint64_t PetMaxJumpEntryRva   = 0x33C9BC;  // table 33C8A0 + 71 * 4
constexpr std::uint64_t PetMaxCaseRva        = 0x33BFBD;
constexpr std::uint64_t Calc4CaseRva         = 0x33BDDC;
constexpr std::uint64_t SkillIdRva           = 0x33E080;
constexpr std::uint64_t EvaluateFormulaRva   = 0x3B5160;
constexpr std::uint64_t DataContextRva       = 0x34A0E0;
constexpr std::uint64_t UnitTypeRva          = 0x34B9D0;
constexpr std::uint64_t SkillLevelRva        = 0x33D1E0;  // loader thunk
constexpr std::uint64_t FeedbackTableRva     = 0x22A1F98;

constexpr std::size_t   SkillRecordOffset    = 0x00;   // skill -> skills record
constexpr std::size_t   RecordSkillIdOffset  = 0x00;   // signed word
constexpr std::size_t   PetMaxFormulaOffset  = 0x114;
constexpr std::uint32_t BlankFormula         = 0xFFFFFFFFU;
constexpr std::int32_t  UsableState          = 0;
constexpr std::int32_t  WrongWeaponState     = 2;
constexpr std::uint32_t PlayerUnitType       = 0;

// ---------------------------------------------------------------------------
// Byte witnesses
// ---------------------------------------------------------------------------

// 33F360: push rsi / push rdi / sub rsp,28h / mov rdi,rdx / mov rsi,rcx /
// test rdx,rdx. The inline hook's expected bytes.
// 33B3EA: cmp ebx,7Eh / ja / lea rdx,[image base] / movsxd rax,ebx /
// mov ecx,[rdx+rax*4+33C8A0h] / add rcx,rdx / jmp rcx.
// 33C9BC: the jump table entry for param 71 = 33BFBD.
// 33BFBD: case 71, r8d = [r11+114h] (petmax), call 3B5160.
// 33BDDC: case 58, r8d = [r11+19Ch] (calc4), call 3B5160.
// 33E080: test rcx,rcx / jnz / xor eax,eax / ret / mov rax,[rcx] /
// movsx eax,word [rax] / ret.
// 22A1F98: the client feedback id per state, one dword each for states 0..9:
// 0, 22, 20, 20, 20, 0, 0, 0, 0, 21. State 2 -> 20, the "Impossible" line.
constexpr std::uint8_t UseStateEntryBytes[]{
    0x40,0x56,0x57,0x48,0x83,0xEC,0x28,0x48,0x8B,0xFA,0x48,0x8B,0xF1,0x48,0x85,0xD2 };
constexpr std::uint8_t KeywordDispatchBytes[]{
    0x83,0xFB,0x7E,0x0F,0x87,0x8E,0x14,0x00,0x00,0x48,0x8D,0x15,0x06,0x4C,0xCC,0xFF,
    0x48,0x63,0xC3,0x8B,0x8C,0x82,0xA0,0xC8,0x33,0x00,0x48,0x03,0xCA,0xFF,0xE1 };
constexpr std::uint8_t PetMaxJumpEntryBytes[]{
    0xBD,0xBF,0x33,0x00 };
constexpr std::uint8_t PetMaxCaseBytes[]{
    0x8B,0x44,0x24,0x70,0x44,0x8B,0xCE,0x45,0x8B,0x83,0x14,0x01,0x00,0x00,0x48,0x8B,
    0xD5,0x40,0x0F,0xB6,0xCF,0x89,0x44,0x24,0x20,0xE8,0x85,0x91,0x07,0x00 };
constexpr std::uint8_t Calc4CaseBytes[]{
    0x8B,0x44,0x24,0x70,0x44,0x8B,0xCE,0x45,0x8B,0x83,0x9C,0x01,0x00,0x00,0x48,0x8B,
    0xD5,0x40,0x0F,0xB6,0xCF,0x89,0x44,0x24,0x20,0xE8,0x66,0x93,0x07,0x00 };
constexpr std::uint8_t SkillIdBytes[]{
    0x48,0x85,0xC9,0x75,0x03,0x33,0xC0,0xC3,0x48,0x8B,0x01,0x0F,0xBF,0x00,0xC3 };
constexpr std::uint8_t EvaluateFormulaBytes[]{
    0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x20,0x57,
    0x41,0x56,0x41,0x57,0x48,0x81,0xEC,0xB0,0x00,0x00,0x00 };
constexpr std::uint8_t DataContextBytes[]{
    0x48,0x83,0xEC,0x28,0x48,0x85,0xC9,0x75,0x1A,0x88,0x4C,0x24,0x30,0x48,0x8D,0x4C,
    0x24,0x30 };
constexpr std::uint8_t UnitTypeBytes[]{
    0x48,0x83,0xEC,0x28,0x48,0x85,0xC9,0x75,0x1D,0x88,0x4C,0x24,0x30,0x48,0x8D,0x4C };
constexpr std::uint8_t LoaderThunkBytes[]{ 0xFF,0x25 };
constexpr std::uint8_t FeedbackTableBytes[]{
    0x00,0x00,0x00,0x00,0x16,0x00,0x00,0x00,0x14,0x00,0x00,0x00,0x14,0x00,0x00,0x00,
    0x14,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x15,0x00,0x00,0x00 };

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

const std::array<Witness, 10> Witnesses{
    W(KeywordDispatchRva, KeywordDispatchBytes, "skill keyword dispatch"),
    W(PetMaxJumpEntryRva, PetMaxJumpEntryBytes, "petmax keyword entry"),
    W(PetMaxCaseRva, PetMaxCaseBytes, "petmax formula read"),
    W(Calc4CaseRva, Calc4CaseBytes, "calc4 formula read"),
    W(SkillIdRva, SkillIdBytes, "skill id"),
    W(EvaluateFormulaRva, EvaluateFormulaBytes, "skill formula"),
    W(DataContextRva, DataContextBytes, "data context"),
    W(UnitTypeRva, UnitTypeBytes, "unit type"),
    W(SkillLevelRva, LoaderThunkBytes, "skill level"),
    W(FeedbackTableRva, FeedbackTableBytes, "cast refusal feedback table"),
};

// ---------------------------------------------------------------------------
// Native functions
// ---------------------------------------------------------------------------

using UseStateFn    = std::int32_t(__fastcall*)(void* unit, void* skill);
using EvaluateFn    = std::int32_t(__fastcall*)(std::uint8_t context, void* unit, std::uint32_t calc,
                          std::int32_t skillId, std::int32_t level);
using DataContextFn = std::uint8_t(__fastcall*)(void* unit);
using UnitTypeFn    = std::uint32_t(__fastcall*)(void* unit);
using SkillLevelFn  = std::int32_t(__fastcall*)(void* unit, void* skill, std::uint64_t bonus, std::uint64_t unused);

const D2RL::PluginContext* Context{};
std::uintptr_t             Base{};

UseStateFn    OriginalUseState{};
EvaluateFn    Evaluate{};
DataContextFn DataContext{};
UnitTypeFn    UnitType{};
SkillLevelFn  SkillLevel{};

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

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

enum class PluginState { NotLoaded, DisabledByConfig, UnsupportedBuild, InstallFailed, Active };

PluginState               State{ PluginState::NotLoaded };
std::atomic<bool>         Active{};
std::atomic<std::uint64_t> ChecksHeld{};
std::atomic<std::int32_t> LastHeldSkill{ -1 };

struct Config {
    bool enabled{ true };
};
Config Settings{};

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

constexpr char DefaultConfigToml[] =
    "# Pet Max Lock (celestialrayone.petmax-lock)\n"
    "#\n"
    "# A skill whose skills.txt petmax formula works out to 0 or less cannot be\n"
    "# cast. It behaves exactly like a throwing skill with a sword equipped:\n"
    "#   - its skill button shows the invalid look (invalidTint in the layout)\n"
    "#   - trying to cast it is refused and the character says \"Impossible\"\n"
    "#   - the server refuses it as well\n"
    "#   - a skill flagged AttackNoMana swings a normal attack instead\n"
    "# Without the plugin the game treats petmax 0 as 1, which is why such a\n"
    "# skill could still summon one pet.\n"
    "#\n"
    "# petmax is worked out every time the game checks the skill, so a formula\n"
    "# that depends on skill level, synergies or stats locks and unlocks the\n"
    "# skill as those change. Skills with a blank petmax column are never\n"
    "# affected. Only players are affected.\n"
    "#\n"
    "# Console: petmaxlock shows the install state and how often a skill was\n"
    "# held back.\n"
    "\n"
    "# Set to false to turn the plugin off.\n"
    "enabled = true\n";

constexpr std::size_t MaximumConfigBytes = 8192;

auto IsBlank(char c) noexcept -> bool {
    return c == ' ' || c == '\t' || c == '\r';
}

// Value of `key = value` on its own line, comments and blanks trimmed.
auto FindValue(const std::string& text, const char* key, std::string& value) -> bool {
    const std::size_t keyLength = std::strlen(key);
    std::size_t line = 0;
    while (line < text.size()) {
        std::size_t end = text.find('\n', line);
        if (end == std::string::npos) end = text.size();
        std::size_t cursor = line;
        while (cursor < end && IsBlank(text[cursor])) ++cursor;
        if (text.compare(cursor, keyLength, key) == 0) {
            std::size_t after = cursor + keyLength;
            while (after < end && IsBlank(text[after])) ++after;
            if (after < end && text[after] == '=') {
                ++after;
                while (after < end && IsBlank(text[after])) ++after;
                std::size_t stop = text.find('#', after);
                if (stop == std::string::npos || stop > end) stop = end;
                while (stop > after && IsBlank(text[stop - 1])) --stop;
                value = text.substr(after, stop - after);
                return true;
            }
        }
        line = end + 1;
    }
    return false;
}

void ReadConfiguration() {
    if (!Context->EnsureConfig(DefaultConfigToml)) {
        Context->LogWarn("PetMaxLock: the config file could not be created; using defaults.");
        return;
    }
    std::string buffer(MaximumConfigBytes, '\0');
    if (!Context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size()), nullptr)) {
        Context->LogWarn("PetMaxLock: the config file could not be read; using defaults.");
        return;
    }
    buffer.resize(std::strlen(buffer.c_str()));
    std::string value;
    if (FindValue(buffer, "enabled", value)) {
        if (value == "true") {
            Settings.enabled = true;
        } else if (value == "false") {
            Settings.enabled = false;
        } else {
            D2RL::LogWarnF(Context, "PetMaxLock: enabled = %s is not true or false; using true.", value.c_str());
        }
    }
}

// ---------------------------------------------------------------------------
// Hook
// ---------------------------------------------------------------------------

// 33F360. The native check runs first; only a usable skill (state 0) whose
// petmax formula is filled and works out to 0 or less is turned into the
// weapon state.
std::int32_t __fastcall HookedUseState(void* unit, void* skill) {
    const std::int32_t state = OriginalUseState(unit, skill);
    if (state != UsableState || !Active.load(std::memory_order_relaxed) || unit == nullptr || skill == nullptr
            || UnitType(unit) != PlayerUnitType) {
        return state;
    }
    void* record = Read<void*>(skill, SkillRecordOffset);
    if (record == nullptr) return state;
    const std::uint32_t formula = Read<std::uint32_t>(record, PetMaxFormulaOffset);
    if (formula == BlankFormula) return state;
    const std::int32_t skillId = Read<std::int16_t>(record, RecordSkillIdOffset);
    const std::int32_t level   = SkillLevel(unit, skill, 1, 0);
    if (Evaluate(DataContext(unit), unit, formula, skillId, level) > 0) return state;
    ChecksHeld.fetch_add(1, std::memory_order_relaxed);
    LastHeldSkill.store(skillId, std::memory_order_relaxed);
    return WrongWeaponState;
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

auto VerifyAll() -> bool {
    bool ok = true;
    for (const Witness& witness : Witnesses) {
        if (!Context->CheckExpectedBytes(witness.rva, witness.bytes, witness.size)) {
            D2RL::LogErrorF(Context, "PetMaxLock: %s at RVA 0x%llX does not match.", witness.name,
                static_cast<unsigned long long>(witness.rva));
            ok = false;
        }
    }
    return ok;
}

void BindNatives() {
    Evaluate    = At<EvaluateFn>(EvaluateFormulaRva);
    DataContext = At<DataContextFn>(DataContextRva);
    UnitType    = At<UnitTypeFn>(UnitTypeRva);
    SkillLevel  = At<SkillLevelFn>(SkillLevelRva);
}

auto StateName(PluginState state) noexcept -> const char* {
    switch (state) {
    case PluginState::NotLoaded:        return "not loaded";
    case PluginState::DisabledByConfig: return "disabled by config";
    case PluginState::UnsupportedBuild: return "unsupported build, nothing installed";
    case PluginState::InstallFailed:    return "install failed, nothing active";
    case PluginState::Active:           return "active";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Console
// ---------------------------------------------------------------------------

auto __cdecl StatusCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept
        -> D2RL::ConsoleCommandResult {
    char line[192];
    std::snprintf(line, sizeof(line), "Pet Max Lock: %s.", StateName(State));
    command->plugin->WriteConsoleMessage(line);
    const std::int32_t last = LastHeldSkill.load();
    if (last >= 0) {
        std::snprintf(line, sizeof(line), "  skill checks held back: %llu, last skill id %d.",
            static_cast<unsigned long long>(ChecksHeld.load()), last);
    } else {
        std::snprintf(line, sizeof(line), "  no skill has been held back yet.");
    }
    command->plugin->WriteConsoleMessage(line);
    return D2RL::ConsoleCommandResult::Handled;
}

constexpr D2RL::PluginInfo PluginInfoData{
    .infoSize    = D2RL::PluginInfoSize,
    .abiVersion  = D2RL_PLUGIN_ABI_VERSION,
    .id          = "celestialrayone.petmax-lock",
    .name        = "Pet Max Lock",
    .version     = "1.1.0",
    .author      = "CelestialRayOne",
    .description = "Skills whose petmax formula works out to 0 or less cannot be cast, shown and refused "
                   "like a skill the equipped weapon cannot use.",
    .flags       = D2RL::PluginFlags::Shared | D2RL::PluginFlags::NativeHooks,
};

}  // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
    return &PluginInfoData;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
    Context = context;
    if (Context == nullptr || Context->exeBase == 0) return false;
    Base = Context->exeBase;

    if (!Context->RegisterConsoleCommand("petmaxlock", &StatusCommand,
            "Show Pet Max Lock install state and how often a skill was held back.")) {
        Context->LogWarn("PetMaxLock: the console command could not be registered.");
    }

    ReadConfiguration();
    if (!Settings.enabled) {
        State = PluginState::DisabledByConfig;
        Context->LogInfo("PetMaxLock: disabled by config.");
        return true;
    }
    if (!VerifyAll()) {
        State = PluginState::UnsupportedBuild;
        Context->LogError("PetMaxLock: this D2R build does not match; nothing was installed.");
        return true;
    }
    BindNatives();
    if (!Context->InstallInlineHook(UseStateRva, UseStateEntryBytes, sizeof(UseStateEntryBytes), &HookedUseState,
            &OriginalUseState)) {
        State = PluginState::InstallFailed;
        Context->LogError("PetMaxLock: the skill use check at RVA 0x33F360 could not be hooked; nothing is active.");
        return true;
    }
    Active.store(true);
    State = PluginState::Active;
    Context->LogInfo("PetMaxLock: active (skill use check 0x33F360, petmax formula at skills record +0x114).");
    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    Active.store(false);
}
