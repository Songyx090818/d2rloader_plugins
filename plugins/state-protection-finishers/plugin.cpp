// ---------------------------------------------------------------------------
// Finisher state protection  -  D2RLoader plugin (D2R 3.3)
//
// Port of the two ESR 2.4 memory patches:
//     "Protect states 208/223/234/250 from finisher teardown - Hook"  35623C
//     "Protect states 208/223/234/250 from finisher teardown - Cave"  19E7D0
//
// What it does
//     Every server tick the game walks a unit's progressive/aura states and,
//     for each one, calls a small helper that subtracts the skill's decquant
//     from the remaining charge count. When the count reaches zero (or the
//     skill row pointer is null) the helper takes the EXPIRE path and the
//     state is torn off the unit. For a handful of ESR states that must
//     survive the finisher discharge, this plugin intercepts the expire path,
//     compares the state id against a configured list and, on a match, does
//     exactly what the "a charge still remains" path does: it sets the
//     survive flag in the caller's frame and returns without running either
//     expire call. Any other state id replays the two stolen instructions and
//     falls straight back into the stock expire, byte for byte.
//
//     The survive flag is the part that keeps the CLIENT overlay alive. The
//     caller reads that byte back after the loop; when it is clear it runs the
//     state-bitmap merge and the refresh that tells the client the state is
//     gone. Setting it keeps the server state AND stops the client being told
//     to drop the overlay. Leaving it out was the bug that took the 2.4
//     version three iterations to find.
//
// 2.4 -> 3.3 map (every line re-derived from the 3.3 image, nothing carried
// over on trust)
//
//     role                     2.4 (D2R.exe)       3.3 (D2RLoader image)
//     -----------------------  ------------------  ---------------------
//     charge/expire helper     sub_1403560C0       sub_14056BFE0  (0x1B3)
//     its caller, the state    sub_140355B00       sub_14056C3E0  (0x5F4)
//       walk + srvdofunc
//       dispatch
//     arg-pack register        rbx (3560C9)        rdi (56BFE6)
//     count > 0 gate           356107 jle 35623C   56C041 jle 56C15C
//     skill row != 0 gate      356115 jz  35623C   56C04E jz  56C15C
//     survive flag write       35611B/35611F       56C054/56C05D
//     expire block entry       35623C              56C15C   <- hook site
//     resume after stolen      356243              56C163
//     clean exit               356264              56C188
//     srvdofunc table          1414BF500           14238EA00
//
//     The arg pack grew from 7 slots to 11 and the order changed, so the two
//     offsets the cave uses moved:
//
//     pack slot                2.4        3.3
//     -----------------------  ---------  ---------
//     &pUnit                   +0x18      +0x00
//     &pGame                   -          +0x08
//     &skillId                 +0x00      +0x10
//     &progressiveCount        +0x08      +0x18
//     &rollFlagByte            -          +0x20
//     &pSkillsTxtRecord        +0x10      +0x28
//     &pStatList               +0x20      +0x30
//     &surviveFlagByte         +0x28      +0x38     <- written on a match
//     &stateId (int16)         +0x30      +0x40     <- read and compared
//     &auraStatCalc inputs     -          +0x48, +0x50
//
//     Proof for the two that matter, read straight out of the image:
//       56C08D  mov rax,[rdi+40h] / 56C09A movsx edx,word [rax] / call
//               STATES_ToggleState(unit, stateId, 1)      -> +0x40 is the id
//       56C054  mov rax,[rdi+38h] / 56C05D mov byte [rax],1 -> +0x38 is the flag
//     And the caller builds all eleven slots unconditionally, one lea of its
//     own stack locals after another, at 56C554..56C5BB - so every slot is a
//     valid pointer into one ~0x120 byte frame on every call.
//
// How it is installed
//     The 2.4 version parked its cave in the body of an unused client skill
//     function. That is what later armed a crash when a skills.txt row started
//     using that column number. Here the same ~120 bytes live in a page the
//     plugin allocates within jump range of the image, so no game function is
//     stomped and no txt column can ever dispatch into it. The only byte the
//     plugin writes inside D2R's own code is the 7-byte jmp at the expire
//     block entry, and it is written only after the loader has verified the
//     original bytes.
//
//     The frame-locality guard from the 2.4 cave is kept as-is: before
//     dereferencing a pack slot the relay checks the pointer lands within
//     +/-4KB of the pack itself. The pack slots are the caller's own stack
//     locals, so a real slot always passes and a corrupted one is never
//     touched. 2.4 measured this: no guard left 392 faults in the stress
//     emulation, a null+canonical guard left 140, this guard left 0.
//
// Console command "finisherstates" reports the install state and how many
// expires were seen and protected.
//
// Runtime settings live in
//     d2rloader/config/celestialrayone.finisher-state-protection.toml
// The file is written with the defaults below on first load.
// ---------------------------------------------------------------------------

#include <D2RLPlugin/api.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace {

constexpr const char* PluginIdText = "celestialrayone.finisher-state-protection";

// ---------------------------------------------------------------------------
// Default config. Also the plugin's documentation, as shipped to the user.
// ---------------------------------------------------------------------------
constexpr const char DefaultToml[] = R"TOML(# celestialrayone.finisher-state-protection
#
# Finisher state protection: stops chosen states from being torn off a unit
# when a progressive/finisher skill spends its last charge.
#
# The problem it solves
#   A skill with progressive charges (skills.txt decquant, aurastate and the
#   aurastat columns) loses one charge per server tick. When the count hits
#   zero the engine expires the state: it drops the state's stat list from the
#   unit and tells the client to clear the overlay. ESR has a few states that
#   carry a permanent overlay or a control stat and must not be removed by
#   that discharge.
#
# What the plugin changes
#   Only the expire step, and only for the state ids listed below. A listed
#   state takes the same exit the engine uses when a charge still remains: the
#   "a state survived this tick" flag is set and neither expire call runs.
#   Everything else - the charge subtraction, the stat refresh, the aura stat
#   calc path, every state id not listed - is untouched stock code.
#
#   Setting that flag is what keeps the client overlay on screen. Without it
#   the server keeps the state but the caller runs its bitmap merge and refresh,
#   and the client is told to drop the overlay.
#
# Where it hooks
#   sub_14056BFE0, the per-state charge helper called from the state walk
#   sub_14056C3E0. The hook is the 7 bytes at RVA 0x56C15C, the entry of the
#   expire block. The relay code lives in a page the plugin allocates, not in
#   D2R's own code, so nothing in the game image is overwritten beyond that
#   one jump, and no skills.txt column can dispatch into it by accident.
#
#   The plugin verifies the exact bytes of every site it depends on before it
#   touches anything. On a D2R build that does not match it installs nothing
#   and says so in its log and in the "finisherstates" console command.
#
# Port of the ESR 2.4 patch pair "Protect states 208/223/234/250 from finisher
# teardown" (hook 35623C + cave 19E7D0) to D2R 3.3.

[celestialrayone.finisher-state-protection]

# Master switch.
enabled = true

# State ids (the *ID column of states.txt) that must survive the discharge.
# Up to 16 ids. Order does not matter. An empty list installs nothing.
#
#   208  223  234  250   <- the four ESR states the 2.4 patch protected
states = [208, 223, 234, 250]
)TOML";

// ---------------------------------------------------------------------------
// Build-specific constants. Every RVA below was read from the 3.3 image
// (140000000.D2RLoader.exe, base 0x140000000).
// ---------------------------------------------------------------------------

// sub_14056BFE0 - the per-state charge/expire helper.
constexpr std::uint64_t ChargeHelperRva = 0x0056BFE0ULL;
// Entry of its expire block, the 7 bytes the jump replaces.
constexpr std::uint64_t ExpireHookRva = 0x0056C15CULL;
// Where the relay resumes after replaying the two stolen instructions.
constexpr std::uint64_t ExpireResumeRva = 0x0056C163ULL;
// The clean exit: restores rsi, then add rsp,40h / pop rdi / ret. No
// dereference on the way out, which is why the protected path lands here and
// not on the epilogue at 56C18D (that one is only reachable before rsi is
// spilled).
constexpr std::uint64_t CleanExitRva = 0x0056C188ULL;
// The two gates that reach the expire block, the survive-flag write, the
// state-id read, the caller's pack construction and the caller's read-back of
// the flag. All witnesses, none of them patched.
constexpr std::uint64_t GatesRva = 0x0056C03BULL;
constexpr std::uint64_t StateIdReadRva = 0x0056C08DULL;
constexpr std::uint64_t CallerPackRva = 0x0056C554ULL;
constexpr std::uint64_t CallerFlagReadRva = 0x0056C9B0ULL;

// Arg-pack offsets used by the relay.
constexpr std::uint8_t PackUnitOffset = 0x00;
constexpr std::uint8_t PackStatListOffset = 0x30;
constexpr std::uint8_t PackSurviveFlagOffset = 0x38;
constexpr std::uint8_t PackStateIdOffset = 0x40;

// 40 57 48 83 EC 40 48 8B F9 48 8B 09 48 8B 09
//   push rdi / sub rsp,40h / mov rdi,rcx / mov rcx,[rcx] / mov rcx,[rcx]
constexpr auto ChargeHelperPrologue = std::to_array<std::uint8_t>({
    0x40, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B, 0xF9, 0x48, 0x8B, 0x09,
    0x48, 0x8B, 0x09});

// 56C15C mov rdx,[rdi+30h] / 56C160 mov rcx,[rdi]  -> replaced by the jump
constexpr auto ExpireHookExpected = std::to_array<std::uint8_t>({
    0x48, 0x8B, 0x57, 0x30, 0x48, 0x8B, 0x0F});

// 56C163 mov rdx,[rdx] / mov rcx,[rcx] / call STATLIST expire
// 56C16E mov rax,[rdi+8] ... movzx ecx,byte[rcx+106h] / call STATLIST free
// 56C188 mov rsi,[rsp+50h]
constexpr auto ExpireBlockTail = std::to_array<std::uint8_t>({
    0x48, 0x8B, 0x12, 0x48, 0x8B, 0x09, 0xE8, 0xB2, 0xB7, 0xD8, 0xFF, 0x48,
    0x8B, 0x47, 0x08, 0x48, 0x8B, 0x57, 0x30, 0x48, 0x8B, 0x08, 0x48, 0x8B,
    0x12, 0x0F, 0xB6, 0x89, 0x06, 0x01, 0x00, 0x00, 0xE8, 0xF8, 0x7F, 0xD8,
    0xFF, 0x48, 0x8B, 0x74, 0x24});

// 56C03B mov rax,[rdi+18h] / cmp [rax],esi / jle 56C15C
// 56C047 mov rax,[rdi+28h] / cmp [rax],rsi / jz  56C15C
// 56C054 mov rax,[rdi+38h] / mov [rsp+20h],si / mov byte[rax],1
// Proves both gates land on the hook site and that +0x38 is the survive flag.
constexpr auto GatesAndFlagWrite = std::to_array<std::uint8_t>({
    0x48, 0x8B, 0x47, 0x18, 0x39, 0x30, 0x0F, 0x8E, 0x15, 0x01, 0x00, 0x00,
    0x48, 0x8B, 0x47, 0x28, 0x48, 0x39, 0x30, 0x0F, 0x84, 0x08, 0x01, 0x00,
    0x00, 0x48, 0x8B, 0x47, 0x38, 0x66, 0x89, 0x74, 0x24, 0x20, 0xC6, 0x00,
    0x01});

// 56C08D mov rax,[rdi+40h] / mov r8d,1 / mov rcx,[rdi] / movsx edx,word[rax]
// Proves +0x40 is the state id and +0x00 is the unit slot.
constexpr auto StateIdRead = std::to_array<std::uint8_t>({
    0x48, 0x8B, 0x47, 0x40, 0x41, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x48, 0x8B,
    0x0F, 0x0F, 0xBF, 0x10});

// 56C554.. the caller writing all eleven pack slots as lea of its own locals.
// Slot 7 (+0x38) <- [rsp+40h], slot 8 (+0x40) <- [rsp+58h].
constexpr auto CallerPackBuild = std::to_array<std::uint8_t>({
    0x48, 0x89, 0x4D, 0x80, 0x8B, 0xD0, 0x48, 0x8B, 0x45, 0x30, 0x48, 0x8D,
    0x4D, 0x30, 0x48, 0x89, 0x4D, 0x88, 0x41, 0xB6, 0x01, 0x48, 0x8D, 0x4D,
    0x40, 0x44, 0x88, 0x75, 0xD8, 0x48, 0x89, 0x4D, 0x90, 0x48, 0x8D, 0x4C,
    0x24, 0x44, 0x48, 0x89, 0x4D, 0x98, 0x48, 0x8D, 0x4D, 0x48, 0x48, 0x89,
    0x4D, 0xA0, 0x48, 0x8D, 0x4C, 0x24, 0x50, 0x48, 0x89, 0x4D, 0xA8, 0x48,
    0x8D, 0x4C, 0x24, 0x60, 0x48, 0x89, 0x4D, 0xB0, 0x48, 0x8D, 0x4C, 0x24,
    0x40, 0x48, 0x89, 0x4D, 0xB8, 0x48, 0x8D, 0x4C, 0x24, 0x58, 0x48, 0x89,
    0x4D, 0xC0});

// 56C9B0 test eax,eax / jnz / cmp [rsp+40h],al / jnz / mov rcx,[rbp+38h] /
// call the state-bitmap merge + client refresh. This is the consumer of the
// survive flag: the merge runs only while the flag is still clear.
constexpr auto CallerFlagRead = std::to_array<std::uint8_t>({
    0x85, 0xC0, 0x75, 0x0F, 0x38, 0x44, 0x24, 0x40, 0x75, 0x09, 0x48, 0x8B,
    0x4D, 0x38, 0xE8});

struct Witness {
    std::uint64_t rva;
    const std::uint8_t* bytes;
    std::uint32_t size;
    const char* what;
};

constexpr std::array<Witness, 7> Witnesses{{
    {ChargeHelperRva, ChargeHelperPrologue.data(),
     static_cast<std::uint32_t>(ChargeHelperPrologue.size()),
     "charge helper prologue"},
    {GatesRva, GatesAndFlagWrite.data(),
     static_cast<std::uint32_t>(GatesAndFlagWrite.size()),
     "expire gates and survive-flag write"},
    {StateIdReadRva, StateIdRead.data(),
     static_cast<std::uint32_t>(StateIdRead.size()),
     "state-id pack slot read"},
    {ExpireHookRva, ExpireHookExpected.data(),
     static_cast<std::uint32_t>(ExpireHookExpected.size()),
     "expire block entry (hook site)"},
    {ExpireResumeRva, ExpireBlockTail.data(),
     static_cast<std::uint32_t>(ExpireBlockTail.size()),
     "expire block body and clean exit"},
    {CallerPackRva, CallerPackBuild.data(),
     static_cast<std::uint32_t>(CallerPackBuild.size()),
     "caller arg-pack construction"},
    {CallerFlagReadRva, CallerFlagRead.data(),
     static_cast<std::uint32_t>(CallerFlagRead.size()),
     "caller survive-flag read-back"},
}};

// ---------------------------------------------------------------------------
// Relay page layout
//
//   +0x0000  0xCC filler. Code starts past any conceivable prolog length so
//            the host function's unwind data describes the relay correctly.
//   +0x0100  relay code (93 + 7 * stateCount bytes, 205 max)
//   +0x01D0  resume slot   -> ExpireResumeRva
//   +0x01D8  clean exit    -> CleanExitRva
//   +0x1000  protected counter (second page, read/write, never executable)
//   +0x1008  passed-through counter
// ---------------------------------------------------------------------------
constexpr std::size_t RelayPageBytes = 0x2000;
constexpr std::size_t RelayCodeOffset = 0x0100;
constexpr std::size_t RelayResumeSlot = 0x01D0;
constexpr std::size_t RelayExitSlot = 0x01D8;
constexpr std::size_t RelayHitCounter = 0x1000;
constexpr std::size_t RelayMissCounter = 0x1008;
constexpr std::size_t RelayCodeLimit = RelayResumeSlot - RelayCodeOffset;

constexpr std::size_t MaxStates = 16;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
enum class PluginState : std::uint8_t {
    NotLoaded,
    DisabledByConfig,
    NoStatesConfigured,
    UnsupportedBuild,
    InstallFailed,
    Active,
};

struct Config {
    bool enabled = true;
    std::array<std::uint16_t, MaxStates> states{208, 223, 234, 250};
    std::size_t stateCount = 4;
};

const D2RL::PluginContext* g_ctx = nullptr;
std::uintptr_t g_base = 0;
Config g_config{};
PluginState g_state = PluginState::NotLoaded;
std::uint8_t* g_relayPage = nullptr;
RUNTIME_FUNCTION g_relayUnwind{};
bool g_unwindRegistered = false;

void Log(int level, const char* format, ...) noexcept {
    if (!g_ctx) return;
    char buffer[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (level == 0) g_ctx->LogInfo(buffer);
    else if (level == 1) g_ctx->LogWarn(buffer);
    else g_ctx->LogError(buffer);
}

std::uint64_t CounterValue(std::size_t offset) noexcept {
    if (!g_relayPage) return 0;
    std::uint64_t value = 0;
    std::memcpy(&value, g_relayPage + offset, sizeof(value));
    return value;
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
std::string_view Trim(std::string_view text) noexcept {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

// Accepts "states = [208, 223]" and the single-value form "states = 208".
void ParseStates(std::string_view value, std::size_t lineNumber) noexcept {
    if (!value.empty() && value.front() == '[') {
        const auto close = value.find(']');
        if (close == std::string_view::npos) {
            Log(1, "finisher-state-protection: config line %zu ignored, the state list is not closed.", lineNumber);
            return;
        }
        value = value.substr(1, close - 1);
    }
    std::size_t count = 0;
    std::size_t position = 0;
    bool overflowed = false;
    while (position <= value.size()) {
        auto end = value.find(',', position);
        if (end == std::string_view::npos) end = value.size();
        const auto item = Trim(value.substr(position, end - position));
        position = end + 1;
        if (item.empty()) continue;
        unsigned long parsed = 0;
        bool digits = true;
        for (const char character : item) {
            if (character < '0' || character > '9') { digits = false; break; }
            parsed = parsed * 10 + static_cast<unsigned long>(character - '0');
            if (parsed > 65535UL) { digits = false; break; }
        }
        if (!digits) {
            Log(1, "finisher-state-protection: config line %zu has a state that is not a number in 0..65535, it was skipped.", lineNumber);
            continue;
        }
        if (count >= MaxStates) { overflowed = true; continue; }
        g_config.states[count++] = static_cast<std::uint16_t>(parsed);
    }
    if (overflowed) {
        Log(1, "finisher-state-protection: more than %zu states listed, only the first %zu are used.", MaxStates, MaxStates);
    }
    g_config.stateCount = count;
}

void ParseConfig(std::string_view text) noexcept {
    std::size_t position = 0;
    std::size_t lineNumber = 0;
    while (position < text.size()) {
        auto end = text.find('\n', position);
        if (end == std::string_view::npos) end = text.size();
        auto line = text.substr(position, end - position);
        position = end + 1;
        ++lineNumber;
        if (const auto hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);
        line = Trim(line);
        if (line.empty() || line.front() == '[') continue;
        const auto equals = line.find('=');
        if (equals == std::string_view::npos) {
            Log(1, "finisher-state-protection: config line %zu ignored, expected key = value.", lineNumber);
            continue;
        }
        const auto key = Trim(line.substr(0, equals));
        const auto value = Trim(line.substr(equals + 1));
        if (key == "enabled") {
            if (value == "true") g_config.enabled = true;
            else if (value == "false") g_config.enabled = false;
            else Log(1, "finisher-state-protection: config line %zu ignored, enabled must be true or false.", lineNumber);
        } else if (key == "states") {
            ParseStates(value, lineNumber);
        } else {
            Log(1, "finisher-state-protection: config line %zu has an unknown key.", lineNumber);
        }
    }
}

void LoadConfig() noexcept {
    g_config = Config{};
    if (!g_ctx->EnsureConfig(DefaultToml)) {
        Log(1, "finisher-state-protection: could not create the default config, using built-in defaults.");
    }
    std::string buffer(16384, '\0');
    std::uint32_t required = 0;
    if (!g_ctx->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size()), &required)) {
        if (required > buffer.size()) {
            buffer.assign(required, '\0');
            if (!g_ctx->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size()), &required)) {
                Log(1, "finisher-state-protection: could not read the config, using built-in defaults.");
                return;
            }
        } else {
            Log(1, "finisher-state-protection: could not read the config, using built-in defaults.");
            return;
        }
    }
    ParseConfig(std::string_view(buffer.c_str()));
}

// ---------------------------------------------------------------------------
// Relay code
//
// Entered from 56C15C with rdi = the arg pack. rax, rcx and rdx are all dead
// there: the stock code at 56C15C..56C166 overwrites rcx and rdx before using
// them, and rax is not read again until 56C16E. Flags are dead too, the
// conditional jump that reached this block has already consumed them.
//
//     lock inc [passed-through counter]      only on the stock path
//     mov   rax,[rdi+40h]                    &stateId
//     mov   rdx,rax / sub rdx,rdi
//     add   rdx,1000h / cmp rdx,2000h / ja   frame-locality guard
//     movzx edx,word [rax]                   the state id
//     cmp   dx,<id> / je protected           once per configured id
//   stock:
//     mov   rdx,[rdi+30h]                    replay the two stolen
//     mov   rcx,[rdi]                        instructions
//     jmp   [resume slot]                    -> 56C163, stock expire
//   protected:
//     lock inc [protected counter]
//     mov   rax,[rdi+38h]                    &surviveFlag
//     <the same frame-locality guard>
//     mov   byte [rax],1                     what 56C05D does when a charge
//     jmp   [exit slot]                      remains  -> 56C188, clean exit
// ---------------------------------------------------------------------------
struct Emitter {
    std::uint8_t* buffer;
    std::size_t capacity;
    std::size_t size = 0;

    void Byte(std::uint8_t value) noexcept {
        if (size < capacity) buffer[size] = value;
        ++size;
    }
    void Bytes(std::initializer_list<std::uint8_t> values) noexcept {
        for (const std::uint8_t value : values) Byte(value);
    }
    void Dword(std::uint32_t value) noexcept {
        Byte(static_cast<std::uint8_t>(value));
        Byte(static_cast<std::uint8_t>(value >> 8));
        Byte(static_cast<std::uint8_t>(value >> 16));
        Byte(static_cast<std::uint8_t>(value >> 24));
    }
};

constexpr std::size_t SlotGuardBytes = 26;   // slot load + locality guard, ja included
constexpr std::size_t StateReadBytes = 3;    // movzx edx,word [rax]
constexpr std::size_t FlagWriteBytes = 3;    // mov byte [rax],1
constexpr std::size_t GuardBytes = SlotGuardBytes + StateReadBytes;
constexpr std::size_t PerStateBytes = 7;     // cmp dx,imm16 + je
constexpr std::size_t StockPathBytes = 21;   // lock inc + 2 replays + jmp [rip]
constexpr std::size_t ProtectedPathBytes = 8 + SlotGuardBytes + FlagWriteBytes + 6;

std::size_t RelayCodeSize(std::size_t stateCount) noexcept {
    return GuardBytes + PerStateBytes * stateCount + StockPathBytes + ProtectedPathBytes;
}

// Emits "mov <reg>,[rdi+offset]" into rax, then the guard, leaving rax = the
// slot value and falling through only when it lands inside the caller frame.
void EmitSlotLoadAndGuard(Emitter& code, std::uint8_t offset, std::size_t bailTarget) noexcept {
    code.Bytes({0x48, 0x8B, 0x47, offset});                    // mov rax,[rdi+off]
    code.Bytes({0x48, 0x89, 0xC2});                            // mov rdx,rax
    code.Bytes({0x48, 0x29, 0xFA});                            // sub rdx,rdi
    code.Bytes({0x48, 0x81, 0xC2, 0x00, 0x10, 0x00, 0x00});    // add rdx,1000h
    code.Bytes({0x48, 0x81, 0xFA, 0x00, 0x20, 0x00, 0x00});    // cmp rdx,2000h
    const std::size_t next = code.size + 2;
    code.Bytes({0x77, static_cast<std::uint8_t>(bailTarget - next)});  // ja bail
}

std::size_t BuildRelayCode(std::uint8_t* buffer, std::size_t capacity, std::uintptr_t codeAddress) noexcept {
    Emitter code{buffer, capacity};
    const std::size_t stateCount = g_config.stateCount;
    const std::size_t stockPath = GuardBytes + PerStateBytes * stateCount;
    const std::size_t protectedPath = stockPath + StockPathBytes;

    const auto ripRelative = [&](std::size_t instructionEnd, std::size_t slotOffset) {
        const std::uintptr_t rip = codeAddress + instructionEnd;
        const std::uintptr_t slot = reinterpret_cast<std::uintptr_t>(g_relayPage) + slotOffset;
        return static_cast<std::uint32_t>(slot - rip);
    };

    EmitSlotLoadAndGuard(code, PackStateIdOffset, stockPath);
    code.Bytes({0x0F, 0xB7, 0x10});                            // movzx edx,word[rax]
    for (std::size_t index = 0; index < stateCount; ++index) {
        const std::uint16_t id = g_config.states[index];
        code.Bytes({0x66, 0x81, 0xFA, static_cast<std::uint8_t>(id),
                    static_cast<std::uint8_t>(id >> 8)});      // cmp dx,id
        const std::size_t next = code.size + 2;
        code.Bytes({0x74, static_cast<std::uint8_t>(protectedPath - next)});  // je protected
    }

    // stock path
    code.Bytes({0xF0, 0x48, 0xFF, 0x05});                      // lock inc qword[rip+d]
    code.Dword(ripRelative(code.size + 4, RelayMissCounter));
    code.Bytes({0x48, 0x8B, 0x57, PackStatListOffset});        // mov rdx,[rdi+30h]
    code.Bytes({0x48, 0x8B, 0x0F});                            // mov rcx,[rdi]
    code.Bytes({0xFF, 0x25});                                  // jmp qword[rip+d]
    code.Dword(ripRelative(code.size + 4, RelayResumeSlot));

    // protected path
    code.Bytes({0xF0, 0x48, 0xFF, 0x05});
    code.Dword(ripRelative(code.size + 4, RelayHitCounter));
    // The guard bails straight to the exit jump, skipping only the flag write.
    EmitSlotLoadAndGuard(code, PackSurviveFlagOffset, code.size + SlotGuardBytes + FlagWriteBytes);
    code.Bytes({0xC6, 0x00, 0x01});                            // mov byte[rax],1
    code.Bytes({0xFF, 0x25});
    code.Dword(ripRelative(code.size + 4, RelayExitSlot));

    return code.size;
}

// ---------------------------------------------------------------------------
// Relay page
// ---------------------------------------------------------------------------
std::uint8_t* AllocateRelayPage() noexcept {
    const auto* image = reinterpret_cast<const std::uint8_t*>(g_base);
    std::int32_t ntHeaders = 0;
    std::memcpy(&ntHeaders, image + 0x3C, sizeof(ntHeaders));
    std::uint32_t sizeOfImage = 0;
    std::memcpy(&sizeOfImage, image + static_cast<std::size_t>(ntHeaders) + 0x50, sizeof(sizeOfImage));

    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    const std::uintptr_t granularity = systemInfo.dwAllocationGranularity ? systemInfo.dwAllocationGranularity : 0x10000;
    const auto alignUp = [granularity](std::uintptr_t value) {
        return (value + granularity - 1) & ~(granularity - 1);
    };

    // The only rel32 that has to reach is the jump written at ExpireHookRva.
    const std::uintptr_t limit = g_base + ExpireHookRva + 0x7FF00000ULL;
    std::uintptr_t address = alignUp(g_base + sizeOfImage);
    while (address + RelayPageBytes < limit) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) == 0) break;
        const auto regionEnd = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (info.State == MEM_FREE && address + RelayPageBytes <= regionEnd) {
            if (auto* page = VirtualAlloc(reinterpret_cast<LPVOID>(address), RelayPageBytes,
                                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
                return static_cast<std::uint8_t*>(page);
            }
        }
        const auto next = alignUp(regionEnd);
        if (next <= address) break;
        address = next;
    }
    return nullptr;
}

// The relay is entered mid-function with the host's frame already established,
// so a stack walk out of it must use the host's unwind data. Code starts
// 0x100 into the page, past any prolog length, so the whole host frame unwinds.
void RegisterRelayUnwind() noexcept {
    DWORD64 imageBase = 0;
    const auto* host = RtlLookupFunctionEntry(g_base + ExpireHookRva, &imageBase, nullptr);
    if (!host || imageBase != g_base || (host->UnwindData & 1U) != 0) {
        Log(1, "finisher-state-protection: no unwind data for the host function, stack walks stop at the relay.");
        return;
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(g_relayPage);
    g_relayUnwind.BeginAddress = static_cast<DWORD>(begin - g_base);
    g_relayUnwind.EndAddress = static_cast<DWORD>(begin + RelayCodeOffset + RelayCodeLimit - g_base);
    g_relayUnwind.UnwindData = host->UnwindData;
    g_unwindRegistered = RtlAddFunctionTable(&g_relayUnwind, 1, g_base) != 0;
    if (!g_unwindRegistered) {
        Log(1, "finisher-state-protection: RtlAddFunctionTable refused the relay table.");
    }
}

bool BuildRelay() noexcept {
    const std::size_t needed = RelayCodeSize(g_config.stateCount);
    if (needed > RelayCodeLimit) {
        Log(2, "finisher-state-protection: %zu states need %zu relay bytes, only %zu are available.",
            g_config.stateCount, needed, RelayCodeLimit);
        return false;
    }

    g_relayPage = AllocateRelayPage();
    if (!g_relayPage) {
        Log(2, "finisher-state-protection: no free page within jump range of the game image.");
        return false;
    }
    std::memset(g_relayPage, 0xCC, RelayPageBytes - 0x1000);
    std::memset(g_relayPage + 0x1000, 0x00, 0x1000);

    const std::uintptr_t codeAddress = reinterpret_cast<std::uintptr_t>(g_relayPage) + RelayCodeOffset;
    const std::uint64_t resume = static_cast<std::uint64_t>(g_base + ExpireResumeRva);
    const std::uint64_t exit = static_cast<std::uint64_t>(g_base + CleanExitRva);
    std::memcpy(g_relayPage + RelayResumeSlot, &resume, sizeof(resume));
    std::memcpy(g_relayPage + RelayExitSlot, &exit, sizeof(exit));

    const std::size_t written = BuildRelayCode(g_relayPage + RelayCodeOffset, RelayCodeLimit, codeAddress);
    if (written != needed) {
        Log(2, "finisher-state-protection: relay emitter wrote %zu bytes, expected %zu.", written, needed);
        return false;
    }

    // Code page executable, counter page stays plain read/write.
    DWORD previous = 0;
    if (!VirtualProtect(g_relayPage, RelayPageBytes - 0x1000, PAGE_EXECUTE_READ, &previous)) {
        Log(2, "finisher-state-protection: could not make the relay page executable.");
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), g_relayPage, RelayPageBytes - 0x1000);
    RegisterRelayUnwind();
    return true;
}

std::uint64_t RelayCodeRva() noexcept {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(g_relayPage) + RelayCodeOffset - g_base);
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------
bool ValidateBuild() noexcept {
    bool ok = true;
    for (const auto& witness : Witnesses) {
        if (!g_ctx->CheckExpectedBytes(witness.rva, witness.bytes, witness.size)) {
            Log(2, "finisher-state-protection: %s does not match at RVA 0x%llX.",
                witness.what, static_cast<unsigned long long>(witness.rva));
            ok = false;
        }
    }
    return ok;
}

void Install() noexcept {
    if (!BuildRelay()) {
        g_state = PluginState::InstallFailed;
        return;
    }
    const bool hooked = g_ctx->PatchJmpRel32(
        ExpireHookRva, ExpireHookExpected.data(),
        static_cast<std::uint32_t>(ExpireHookExpected.size()),
        RelayCodeRva(), static_cast<std::uint32_t>(ExpireHookExpected.size()));
    if (!hooked) {
        Log(2, "finisher-state-protection: the expire hook at RVA 0x%llX could not be written.",
            static_cast<unsigned long long>(ExpireHookRva));
        g_state = PluginState::InstallFailed;
        return;
    }
    g_state = PluginState::Active;

    char list[128] = {0};
    int used = 0;
    for (std::size_t index = 0; index < g_config.stateCount && used >= 0 && used < static_cast<int>(sizeof(list)) - 1; ++index) {
        const char* separator = (index == 0) ? "" : ", ";
        used += std::snprintf(list + used, sizeof(list) - static_cast<std::size_t>(used),
                              "%s%u", separator, static_cast<unsigned>(g_config.states[index]));
    }
    Log(0, "finisher-state-protection: active, protecting state ids %s.", list);
}

// ---------------------------------------------------------------------------
// Console command
// ---------------------------------------------------------------------------
const char* StateName(PluginState state) noexcept {
    switch (state) {
    case PluginState::NotLoaded: return "not loaded";
    case PluginState::DisabledByConfig: return "disabled by config";
    case PluginState::NoStatesConfigured: return "idle, the state list in the config is empty";
    case PluginState::UnsupportedBuild: return "unsupported build, nothing installed (see the plugin log)";
    case PluginState::InstallFailed: return "FAILED to install (see the plugin log)";
    case PluginState::Active: return "active";
    }
    return "unknown";
}

D2RL::ConsoleCommandResult StatusCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept {
    if (!command || !command->plugin) return D2RL::ConsoleCommandResult::Failed;
    char line[256];
    std::snprintf(line, sizeof(line), "finisher-state-protection: %s", StateName(g_state));
    command->plugin->WriteConsoleMessage(line);

    int used = std::snprintf(line, sizeof(line), "  protected states:");
    for (std::size_t index = 0; index < g_config.stateCount && used >= 0 && used < static_cast<int>(sizeof(line)) - 1; ++index) {
        used += std::snprintf(line + used, sizeof(line) - static_cast<std::size_t>(used),
                              " %u", static_cast<unsigned>(g_config.states[index]));
    }
    command->plugin->WriteConsoleMessage(line);

    std::snprintf(line, sizeof(line), "  discharges protected: %llu, passed through: %llu",
                  static_cast<unsigned long long>(CounterValue(RelayHitCounter)),
                  static_cast<unsigned long long>(CounterValue(RelayMissCounter)));
    command->plugin->WriteConsoleMessage(line);

    std::snprintf(line, sizeof(line), "  relay unwind data: %s",
                  g_unwindRegistered ? "registered" : "not registered");
    command->plugin->WriteConsoleMessage(line);
    return D2RL::ConsoleCommandResult::Handled;
}

constexpr D2RL::PluginInfo PluginInfoData{
    .infoSize = D2RL::PluginInfoSize,
    .apiVersion = D2RL_PLUGIN_API_VERSION,
    .id = "celestialrayone.finisher-state-protection",
    .name = "Finisher State Protection",
    .version = "1.0.0",
    .author = "CelestialRayOne",
    .description = "Keeps chosen states alive when a progressive skill spends its last charge. Port of the ESR 2.4 patch pair.",
    .flags = D2RL::PluginFlags::Server | D2RL::PluginFlags::NativeHooks,
};

}  // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
    return &PluginInfoData;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
    g_ctx = context;
    if (!g_ctx || g_ctx->exeBase == 0) return false;
    g_base = g_ctx->exeBase;

    if (!g_ctx->RegisterConsoleCommand("finisherstates", StatusCommand,
                                       "Show Finisher State Protection install state and counters.")) {
        Log(1, "finisher-state-protection: console command could not be registered.");
    }

    LoadConfig();
    if (!g_config.enabled) {
        g_state = PluginState::DisabledByConfig;
        Log(0, "finisher-state-protection: disabled by config.");
        return true;
    }
    if (g_config.stateCount == 0) {
        g_state = PluginState::NoStatesConfigured;
        Log(1, "finisher-state-protection: the state list is empty, nothing was installed.");
        return true;
    }
    if (!ValidateBuild()) {
        g_state = PluginState::UnsupportedBuild;
        Log(2, "finisher-state-protection: this D2R build does not match, nothing was installed.");
        return true;
    }
    Install();
    Log(0, "finisher-state-protection: %s (plugin id %s).", StateName(g_state), PluginIdText);
    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
}
