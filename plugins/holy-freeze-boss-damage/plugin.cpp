// Holy Freeze Damage
//
// Holy Freeze deals its cold damage to every monster its aura pulses over,
// including the ones monstats.txt marks as unchillable (ColdEffect >= 0, which
// is what prime evils ship with). Those monsters get the damage and nothing
// else: no aura target state, no slow, no shatter roll. Monsters with a
// negative ColdEffect are untouched and keep the full vanilla treatment.
//
// Port of the ESR D2R 2.4 patch set (gate retarget 3886CB, shatter-gate hook
// 38886F, cave 36AD70) to D2R 3.3 on D2RLoader 1.3.0. Every address and byte
// below was read out of the live D2RLoader image and disassembled first.
//
// ---------------------------------------------------------------------------
// The engine, 3.3
// ---------------------------------------------------------------------------
//   srvdofunc table 0x238EA00, slot 81 at 0x238EC88 -> 0x563720, the Holy
//   Freeze server handler. It ends with the range iterator call
//
//     563C93  48 8D 05 E6 D7 FF FF  lea  rax, 561480     ; per-target callback
//     563C9A  48 8B D6              mov  rdx, rsi
//     563C9D  48 89 44 24 30        mov  [rsp+30h], rax  ; iterator arg 7
//
//   so 0x561480 is the callback, and it is reached from nowhere else: that lea
//   is its only code reference in the image.
//
//   0x561480 (2.4 sub_140388650) opens with the gate this plugin is about:
//
//     5614C5  call 48FF40                  ; -> rax, difficulty byte
//     5614CD  movzx r15d, byte [rax]       ; r15 = difficulty, never rewritten
//     5614D1  call 34B9D0                  ; UNITS_GetUnitType(target)
//     5614D6  cmp  eax, 1
//     5614D9  jnz  561515                  ; not a monster -> vanilla path
//     5614EB  call 349860                  ; UNITS_GetClassId -> [unit+4]
//     5614F0  movzx ecx, byte [rsi+106h]   ; pGame+106h = data context
//     5614F9  call 976E0                   ; MonStats record, stride 508
//     5614FE  test rax, rax
//     561501  jz   56150E
//     561503  cmp  byte [r15+rax+1BAh], 0  ; ColdEffect[difficulty], SIGNED
//     56150C  jl   561515                  ; negative -> vanilla path
//     56150E  xor  eax, eax                ; <- site A, 7 bytes
//     561510  jmp  5616C7                  ;    return 0, target skipped whole
//     561515  cmp  dword [rbx+4Ch], 0      ; nPassiveState, aura target state
//     561519  jz   5615BF
//     ...     the state block
//     5615BF  mov  rdx, [rbx+50h]          ; pDamage
//     5615C3  test rdx, rdx
//     5615C6  jz   561675
//     5615D1  call 4494B0                  ; D2Damage copy constructor
//     5615F4  call 560F60                  ; close-range aura damage bonus
//     561612  call 44CE80                  ; SUNITDMG_ExecuteEvents
//     561625  call 44A9B0                  ; SUNITDMG_FinalizeDamage
//     561675  mov  rcx, rdi                ; <- site B, 8 bytes
//     561678  call 34A1E0                  ; UNITS_GetSeed -> unit+28h
//     56167D  ...  LCG, r9d = seed % 100
//     5616AA  mov  edx, 6Bh                ; state 107, shatter
//     5616B5  cmp  r9d, 14h
//     5616B9  setl r8b                     ; the 20% roll
//     5616BD  call 3354C0                  ; STATES_ToggleState(target,107,roll)
//     5616C2  mov  eax, 1
//     5616C7  epilogue, returns
//
//   ColdEffect is the same signed per-difficulty byte as in 2.4, moved from
//   record+412 to record+442 (0x1BA) as the record grew from 476 to 508 bytes.
//
// ---------------------------------------------------------------------------
// Site A: the gate's return 0 becomes a jump into the damage block
// ---------------------------------------------------------------------------
//   56150E, 7 bytes, 33 C0 E9 B2 01 00 00 -> E9 AC 00 00 00 90 90, a jump to
//   5615BF plus two NOP. Same shape as the 2.4 patch at 3886CB, different
//   displacement. The state block is jumped over, so an unchillable monster
//   gets no aura state and no aurastat slow, which is the design: the slow
//   stays gated by ColdEffect, the damage does not.
//
//   Nothing lands inside the window. The only jump into it is jz 56150E at
//   561501, the null-record bail, and it lands on the first byte. That bail
//   now routes to the damage block too, exactly as the 2.4 set did.
//
//   Entering the damage block from the gate is safe: rsi, rdi, rbx, r14 and
//   r15 are all loaded before the gate and are non-volatile, and the damage
//   block is self-contained. 4494B0 is the D2Damage copy constructor and it
//   writes the whole packet including the RAII field at buf+158h that the
//   block's own teardown reads, so this is the same entry the vanilla
//   nPassiveState == 0 path already uses on every pulse.
//
// ---------------------------------------------------------------------------
// Site B: the shatter tail asks whether this target was gated
// ---------------------------------------------------------------------------
//   With site A live the gated target now falls through into the shatter tail,
//   which vanilla never let it reach. The 2.4 set solved that with a cave that
//   re-derived the ColdEffect test; here the same test is re-derived in C++,
//   through the engine's own three calls, and the stub is 81 bytes in a page
//   this plugin allocates.
//
//   561675, 8 bytes, 48 8B CF E8 63 8B DE FF -> E9 <rel32> 90 90 90, resume
//   56167D. Three jumps reach 561675 (5615C6, 56165D and the fallthrough at
//   561671) and all three land on the first byte; nothing lands inside.
//
//   At the tail rdi is the target, rsi is pGame and r15 still holds the
//   difficulty loaded at 5614CD, all three non-volatile and never rewritten in
//   the function. rsp is 16-aligned there (entry + 5 pushes + sub rsp,1F0h), so
//   the stub takes 20h for shadow space, calls, and puts it straight back, and
//   the call to 34A1E0 then runs on the frame's own shadow area exactly as the
//   original instruction did. rax is the only live value at 56167D and it is
//   the seed pointer that call returns.
//
//   Gated targets leave through mov eax,1 / jmp 5616C7, so they never draw from
//   the unit's LCG and never see STATES_ToggleState. That keeps both the engine
//   invariant "unchillable never gets state 107" and the target's seed stream
//   the same as vanilla.
//
//   The stub was assembled with keystone and decoded back with capstone.
//
// ---------------------------------------------------------------------------
// Verified against
// ---------------------------------------------------------------------------
//   D2MOO SKILLS_AuraCallback_HolyFreeze for the gate's logic, Ruff's RVA
//   corpus for 0976E0 (dataContext, classId -> MonStatsTxt*), 34B9D0, 349860,
//   34A1E0, 3354C0, 4494B0, 44CE80, 44A9B0 and Game+106h as the data context,
//   and the ESR 2.4 patch set this replaces.
//
//   soft-hit-removal patches 563BE4..563C29 inside the srvdofunc body; neither
//   site here overlaps it.

#include <D2RLPlugin/api.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace CelestialRayOne::HolyFreezeDamage {
namespace {

// ---------------------------------------------------------------------------
// Native anchors
// ---------------------------------------------------------------------------

constexpr std::uint64_t HandlerRva          = 0x563720;  // srvdofunc 81
constexpr std::uint64_t HandlerCallbackRva  = 0x563C93;  // lea rax, 561480
constexpr std::uint64_t CallbackRva         = 0x561480;  // per-target callback
constexpr std::uint64_t GateRva             = 0x5614AC;  // callback body start
constexpr std::uint64_t GateReturnZeroRva   = 0x56150E;  // site A
constexpr std::uint64_t DamageBlockRva      = 0x5615BF;  // site A target
constexpr std::uint64_t ShatterTailRva      = 0x561675;  // site B
constexpr std::uint64_t ShatterResumeRva    = 0x56167D;  // site B resume
constexpr std::uint64_t ReturnOneRva        = 0x5616C7;  // epilogue, returns 1

constexpr std::uint64_t GetUnitSeedRva      = 0x34A1E0;
constexpr std::uint64_t GetUnitTypeRva      = 0x34B9D0;
constexpr std::uint64_t GetClassIdRva       = 0x349860;
constexpr std::uint64_t GetMonStatsRecordRva = 0x0976E0;

constexpr std::size_t   GameDataContextOffset = 0x106;
constexpr std::size_t   ColdEffectOffset      = 0x1BA;
constexpr std::size_t   MonStatsRecordSize    = 508;
constexpr std::int32_t  UnitTypeMonster       = 1;
constexpr std::uint32_t HighestDifficulty     = 2;

static_assert(ColdEffectOffset + HighestDifficulty < MonStatsRecordSize);

constexpr std::uint32_t GateHookSize    = 7;
constexpr std::uint32_t ShatterHookSize = 8;
constexpr std::uint32_t JumpSize        = 5;

// 563C93: the lea that hands 561480 to the range iterator as its callback,
// then mov rdx,rsi / mov [rsp+30h],rax. Proves the function patched below is
// still the one Holy Freeze installs.
constexpr std::uint8_t HandlerCallbackWitness[]{
    0x48, 0x8D, 0x05, 0xE6, 0xD7, 0xFF, 0xFF, 0x48, 0x8B, 0xD6, 0x48, 0x89,
    0x44, 0x24, 0x30,
};

// 5614AC: the whole callback prologue and gate, through cmp [rbx+4Ch],0. The
// displaced 7 bytes sit at +62h. Proves the register assignment, the three
// engine calls, the data context at pGame+106h and ColdEffect at record+1BAh.
constexpr std::uint8_t GateWitness[]{
    0x48, 0x8B, 0x31, 0x48, 0x8B, 0xFA, 0x4C, 0x8B, 0x71, 0x08, 0x48, 0x8B,
    0xD6, 0x48, 0x8B, 0x59, 0x18, 0x4D, 0x8B, 0xC6, 0x48, 0x8D, 0x4C, 0x24,
    0x50, 0xE8, 0x76, 0xEA, 0xF2, 0xFF, 0x48, 0x8B, 0xCF, 0x44, 0x0F, 0xB6,
    0x38, 0xE8, 0xFA, 0xA4, 0xDE, 0xFF, 0x83, 0xF8, 0x01, 0x75, 0x3A, 0x41,
    0xB8, 0xE5, 0x06, 0x00, 0x00, 0x48, 0x8D, 0x15, 0xA8, 0xC9, 0x7D, 0x01,
    0x48, 0x8B, 0xCF, 0xE8, 0x70, 0x83, 0xDE, 0xFF, 0x0F, 0xB6, 0x8E, 0x06,
    0x01, 0x00, 0x00, 0x8B, 0xD0, 0xE8, 0xE2, 0x61, 0xB3, 0xFF, 0x48, 0x85,
    0xC0, 0x74, 0x0B, 0x41, 0x80, 0xBC, 0x07, 0xBA, 0x01, 0x00, 0x00, 0x00,
    0x7C, 0x07, 0x33, 0xC0, 0xE9, 0xB2, 0x01, 0x00, 0x00, 0x83, 0x7B, 0x4C,
    0x00,
};
constexpr std::size_t GateReturnZeroOffsetInWitness = GateReturnZeroRva - GateRva;

// 561675: the displaced 8 bytes, the LCG, state 107 in edx, the 20% roll, the
// STATES_ToggleState call, mov eax,1 and the epilogue entry at 5616C7.
constexpr std::uint8_t ShatterTailWitness[]{
    0x48, 0x8B, 0xCF, 0xE8, 0x63, 0x8B, 0xDE, 0xFF, 0x45, 0x33, 0xC0, 0x8B,
    0x08, 0x4C, 0x69, 0xC9, 0xC5, 0x90, 0xC6, 0x6A, 0x8B, 0x48, 0x04, 0x4C,
    0x03, 0xC9, 0x44, 0x89, 0x08, 0x49, 0x8B, 0xC9, 0x48, 0xC1, 0xE9, 0x20,
    0x89, 0x48, 0x04, 0xB8, 0x1F, 0x85, 0xEB, 0x51, 0x41, 0xF7, 0xE1, 0xC1,
    0xEA, 0x05, 0x6B, 0xCA, 0x64, 0xBA, 0x6B, 0x00, 0x00, 0x00, 0x44, 0x2B,
    0xC9, 0x48, 0x8B, 0xCF, 0x41, 0x83, 0xF9, 0x14, 0x41, 0x0F, 0x9C, 0xC0,
    0xE8, 0xFE, 0x3D, 0xDD, 0xFF, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x48, 0x8B,
    0x8D, 0xE0, 0x00, 0x00, 0x00, 0x48, 0x33, 0xCC, 0xE8, 0x7A, 0xFA, 0xD6,
    0x00,
};

static_assert(GateReturnZeroOffsetInWitness + GateHookSize <= sizeof(GateWitness));
static_assert(ShatterHookSize <= sizeof(ShatterTailWitness));
static_assert(GateWitness[GateReturnZeroOffsetInWitness] == 0x33
    && GateWitness[GateReturnZeroOffsetInWitness + 1] == 0xC0
    && GateWitness[GateReturnZeroOffsetInWitness + 2] == 0xE9);

// ---------------------------------------------------------------------------
// Hook page
// ---------------------------------------------------------------------------
//
// +00  48 83 EC 20              sub   rsp, 20h            ; shadow, stays aligned
// +04  48 89 F9                 mov   rcx, rdi            ; target
// +07  48 89 F2                 mov   rdx, rsi            ; pGame
// +0A  45 89 F8                 mov   r8d, r15d           ; difficulty
// +0D  48 B8 imm64              mov   rax, TargetIsGated
// +17  FF D0                    call  rax
// +19  48 83 C4 20              add   rsp, 20h
// +1D  84 C0                    test  al, al
// +1F  75 1D                    jnz   +3Eh
// +21  48 89 F9                 mov   rcx, rdi            ; the vanilla tail
// +24  48 B8 imm64              mov   rax, 34A1E0
// +2E  FF D0                    call  rax
// +30  FF 25 00 00 00 00        jmp   qword ptr [rip+0]
// +36  dq                       56167D
// +3E  B8 01 00 00 00           mov   eax, 1              ; gated: no shatter
// +43  FF 25 00 00 00 00        jmp   qword ptr [rip+0]
// +49  dq                       5616C7
constexpr std::uint8_t ShatterStub[]{
    0x48, 0x83, 0xEC, 0x20, 0x48, 0x89, 0xF9, 0x48, 0x89, 0xF2, 0x45, 0x89,
    0xF8, 0x48, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0xFF,
    0xD0, 0x48, 0x83, 0xC4, 0x20, 0x84, 0xC0, 0x75, 0x1D, 0x48, 0x89, 0xF9,
    0x48, 0xB8, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0xFF, 0xD0,
    0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, 0x00, 0x99, 0x88, 0x77, 0x66, 0x55,
    0x44, 0x33, 0xB8, 0x01, 0x00, 0x00, 0x00, 0xFF, 0x25, 0x00, 0x00, 0x00,
    0x00, 0x11, 0x00, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44,
};

constexpr std::size_t StubPredicateImm64 = 0x0F;
constexpr std::size_t StubSeedImm64      = 0x26;
constexpr std::size_t StubResumeSlot     = 0x36;
constexpr std::size_t StubVanillaOffset  = 0x21;
constexpr std::size_t StubSkipOffset     = 0x3E;
constexpr std::size_t StubEpilogueSlot   = 0x49;

constexpr std::size_t HookPageBytes  = 4'096;
constexpr std::size_t StubPageOffset = 0x00;

constexpr auto MovabsBefore(const std::uint8_t* code, std::size_t slot) noexcept -> bool {
    return slot >= 2 && code[slot - 2] == 0x48 && code[slot - 1] == 0xB8;
}

constexpr auto JumpBefore(const std::uint8_t* code, std::size_t slot) noexcept -> bool {
    return slot >= 6 && code[slot - 6] == 0xFF && code[slot - 5] == 0x25
        && code[slot - 4] == 0x00 && code[slot - 3] == 0x00
        && code[slot - 2] == 0x00 && code[slot - 1] == 0x00;
}

static_assert(sizeof(ShatterStub) == 0x51);
static_assert(MovabsBefore(ShatterStub, StubPredicateImm64));
static_assert(MovabsBefore(ShatterStub, StubSeedImm64));
static_assert(JumpBefore(ShatterStub, StubResumeSlot));
static_assert(JumpBefore(ShatterStub, StubEpilogueSlot));
// jnz +1Dh at +1Fh reaches the gated exit, and that exit is mov eax,1.
static_assert(ShatterStub[0x1F] == 0x75
    && static_cast<std::size_t>(0x21 + ShatterStub[0x20]) == StubSkipOffset);
static_assert(ShatterStub[StubSkipOffset] == 0xB8 && ShatterStub[StubSkipOffset + 1] == 0x01);
// The vanilla tail replays the displaced mov rcx,rdi and ends on a jump out.
static_assert(ShatterStub[StubVanillaOffset] == 0x48
    && ShatterStub[StubVanillaOffset + 1] == 0x89
    && ShatterStub[StubVanillaOffset + 2] == 0xF9);
static_assert(StubPageOffset + sizeof(ShatterStub) <= HookPageBytes);

// Turns the stub into the vanilla tail alone: jmp short +1Fh, landing on +21h.
constexpr std::uint8_t StubToVanilla[]{ 0xEB, 0x1F };
static_assert(static_cast<std::size_t>(0x02 + StubToVanilla[1]) == StubVanillaOffset);

constexpr std::uint32_t MaximumConfigBytes = 32'768;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

struct Config {
    bool enabled         = true;
    bool suppressShatter = true;
};

constexpr char DefaultConfigToml[] = R"toml(# Holy Freeze Damage
#
# Holy Freeze deals its cold damage to every monster its aura pulses over,
# including the ones monstats.txt marks as unchillable. A monster is unchillable
# when its ColdEffect column for the current difficulty is 0 or higher, which is
# what prime evils ship with, and vanilla drops such a target at the top of the
# aura's per-target callback: no aura state, no slow, no damage, no shatter.
#
# With this plugin those monsters take the full pulse damage through the normal
# resist math, and nothing else. The slow stays gated by ColdEffect, because the
# aura state block is skipped for them. Monsters with a negative ColdEffect are
# untouched and keep the full vanilla treatment: state, slow, damage, shatter.
#
# Cold-immune targets (cold resist 100 or higher) still take 0. That is the
# resist, not the gate, and it is how Holy Fire already behaves against fire
# immunes.
#
# This is the D2R 3.3 port of the ESR 2.4 memory patch set (gate retarget at
# 3886CB, shatter-gate hook at 38886F, cave at 36AD70). It patches two sites in
# the Holy Freeze per-target callback at 0x561480 and nothing else.
#
# Console command: holyfreeze (status and counters)

[holy_freeze]

# Master switch. False leaves the game completely vanilla.
enabled = true

# Keep the engine invariant that an unchillable monster never gets the shatter
# state (states.txt 107, the 20% roll at the end of the callback).
#
# true   an unchillable monster takes the damage only. This matches the ESR 2.4
#        patch set and is the recommended setting.
# false  an unchillable monster also rolls for shatter like any other target.
#        Only the first patch site is installed and no hook page is allocated.
suppress_shatter = true
)toml";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

enum class HookState : std::uint8_t {
    NotLoaded,
    DisabledByConfig,
    Armed,
    PartiallyArmed,
};

using GetUnitTypeFn       = std::int32_t(__fastcall*)(void* unit) noexcept;
using GetClassIdFn        = std::int32_t(__fastcall*)(void* unit) noexcept;
using GetMonStatsRecordFn = void*(__fastcall*)(std::uint8_t dataContext, std::int32_t classId) noexcept;

const D2RL::PluginContext* Context{};
std::uintptr_t             Base{};
GetUnitTypeFn              GetUnitType{};
GetClassIdFn               GetClassId{};
GetMonStatsRecordFn        GetMonStatsRecord{};
Config                     Settings{};
HookState                  State{ HookState::NotLoaded };
void*                      HookPage{};
bool                       GatePatched{};
bool                       ShatterPatched{};

std::atomic<std::uint64_t> DamagedTargets{};
std::atomic<std::uint64_t> NormalTargets{};
std::atomic<bool>          ReportedFirst{};

// ---------------------------------------------------------------------------
// Config parsing (small TOML subset, no external dependency)
// ---------------------------------------------------------------------------

auto Trim(std::string_view value) noexcept -> std::string_view {
    std::size_t first = 0;
    while (first < value.size()
            && (value[first] == ' ' || value[first] == '\t'
                || value[first] == '\r' || value[first] == '\n')) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first
            && (value[last - 1] == ' ' || value[last - 1] == '\t'
                || value[last - 1] == '\r' || value[last - 1] == '\n')) {
        --last;
    }
    return value.substr(first, last - first);
}

void ParseBool(std::string_view value, bool& out) noexcept {
    if (value == "true" || value == "1") {
        out = true;
    } else if (value == "false" || value == "0") {
        out = false;
    }
}

void ApplyConfigLine(std::string_view key, std::string_view value) noexcept {
    if (key == "enabled") {
        ParseBool(value, Settings.enabled);
    } else if (key == "suppress_shatter") {
        ParseBool(value, Settings.suppressShatter);
    }
}

void ParseConfig(std::string_view text) noexcept {
    std::size_t cursor = 0;
    while (cursor <= text.size()) {
        const std::size_t breakAt = text.find('\n', cursor);
        const std::size_t end = breakAt == std::string_view::npos ? text.size() : breakAt;
        std::string_view line = Trim(text.substr(cursor, end - cursor));
        cursor = end + 1;
        if (breakAt == std::string_view::npos && line.empty()) break;

        const std::size_t comment = line.find('#');
        if (comment != std::string_view::npos) line = Trim(line.substr(0, comment));
        if (!line.empty() && line.front() != '[') {
            const std::size_t equals = line.find('=');
            if (equals != std::string_view::npos) {
                ApplyConfigLine(Trim(line.substr(0, equals)), Trim(line.substr(equals + 1)));
            }
        }
        if (breakAt == std::string_view::npos) break;
    }
}

void ReadConfiguration() noexcept {
    if (!Context->EnsureConfig(DefaultConfigToml)) {
        Context->LogWarn("HolyFreezeDamage: config file could not be created; using defaults.");
        return;
    }
    std::string buffer(MaximumConfigBytes, '\0');
    if (!Context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size()), nullptr)) {
        Context->LogWarn("HolyFreezeDamage: config file could not be read; using defaults.");
        return;
    }
    buffer.resize(std::strlen(buffer.c_str()));
    ParseConfig(buffer);
}

// ---------------------------------------------------------------------------
// The gate test, re-derived at the shatter tail
// ---------------------------------------------------------------------------
//
// Reached from the stub with the callback's own live registers: rdi, rsi and
// r15. It asks the same three engine functions in the same order as the gate at
// 5614D1..561503, so a target is reported gated exactly when vanilla would have
// dropped it, the null-record bail included.
//
// The difficulty bound is this plugin's own: the gate indexes the record with
// an unbounded byte, which is safe there only because the value is always 0, 1
// or 2. Anything else is treated as not gated, so the shatter tail runs vanilla
// rather than reading past the 508-byte record.
auto __fastcall TargetIsGated(void* target, void* game,
        std::uint32_t difficulty) noexcept -> bool {
    if (target == nullptr || game == nullptr || difficulty > HighestDifficulty) {
        NormalTargets.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (GetUnitType(target) != UnitTypeMonster) {
        NormalTargets.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const std::int32_t classId = GetClassId(target);
    const auto dataContext =
        *(static_cast<const std::uint8_t*>(game) + GameDataContextOffset);
    const auto* record =
        static_cast<const std::uint8_t*>(GetMonStatsRecord(dataContext, classId));

    bool gated = true;
    if (record != nullptr) {
        const auto coldEffect =
            static_cast<std::int8_t>(record[ColdEffectOffset + difficulty]);
        gated = coldEffect >= 0;
    }

    if (!gated) {
        NormalTargets.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    DamagedTargets.fetch_add(1, std::memory_order_relaxed);
    if (!ReportedFirst.exchange(true, std::memory_order_relaxed)) {
        D2RL::LogInfoF(Context,
            "HolyFreezeDamage: first unchillable target reached, monstats row %d. It takes "
            "the pulse damage with no aura state, no slow and no shatter.",
            classId);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Hook page and sites
// ---------------------------------------------------------------------------

auto CanEncodeRel32(std::uintptr_t from, std::uintptr_t to) noexcept -> bool {
    const std::int64_t delta =
        static_cast<std::int64_t>(to) - static_cast<std::int64_t>(from) - JumpSize;
    return delta >= INT32_MIN && delta <= INT32_MAX;
}

auto AllocateNear(std::uintptr_t hint, std::size_t size) noexcept -> void* {
    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    const auto granularity = static_cast<std::uintptr_t>(systemInfo.dwAllocationGranularity);
    const auto aligned = hint & ~(granularity - 1U);
    for (std::uintptr_t delta = granularity; delta < 0x7000'0000ULL; delta += granularity) {
        const auto candidate = aligned + delta;
        if (!CanEncodeRel32(hint, candidate)) break;
        if (auto* memory = VirtualAlloc(reinterpret_cast<void*>(candidate), size,
                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) {
            return memory;
        }
    }
    return nullptr;
}

auto PageAddress(std::size_t offset) noexcept -> std::uintptr_t {
    return reinterpret_cast<std::uintptr_t>(HookPage) + offset;
}

void WriteQword(std::uint8_t* at, std::uint64_t value) noexcept {
    std::memcpy(at, &value, sizeof(value));
}

// E9 rel32 to the target, the rest of the window filled with NOP.
void EncodeSiteJump(std::uintptr_t site, std::uintptr_t target, std::uint8_t* out,
        std::uint32_t windowSize) noexcept {
    out[0] = 0xE9;
    const auto displacement = static_cast<std::int32_t>(
        static_cast<std::int64_t>(target) - static_cast<std::int64_t>(site + JumpSize));
    std::memcpy(out + 1, &displacement, sizeof(displacement));
    std::memset(out + JumpSize, 0x90, windowSize - JumpSize);
}

// Leaves the stub running the vanilla tail only. After this nothing in the page
// can reach this DLL.
auto RetargetStubToVanilla() noexcept -> bool {
    if (HookPage == nullptr) return true;
    auto* page = static_cast<std::uint8_t*>(HookPage);
    DWORD previous = 0;
    if (!VirtualProtect(page, HookPageBytes, PAGE_EXECUTE_READWRITE, &previous)) return false;
    std::memcpy(page + StubPageOffset, StubToVanilla, sizeof(StubToVanilla));
    DWORD ignored = 0;
    VirtualProtect(page, HookPageBytes, previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), page, HookPageBytes);
    return true;
}

auto RestoreSites() noexcept -> bool {
    bool restored = true;
    std::uint8_t current[16]{};

    if (ShatterPatched) {
        EncodeSiteJump(Base + ShatterTailRva, PageAddress(StubPageOffset), current,
            ShatterHookSize);
        if (Context->PatchBytes(ShatterTailRva, current, ShatterHookSize,
                ShatterTailWitness, ShatterHookSize)) {
            ShatterPatched = false;
        } else {
            restored = false;
        }
    }

    if (GatePatched) {
        EncodeSiteJump(Base + GateReturnZeroRva, Base + DamageBlockRva, current, GateHookSize);
        if (Context->PatchBytes(GateReturnZeroRva, current, GateHookSize,
                GateWitness + GateReturnZeroOffsetInWitness, GateHookSize)) {
            GatePatched = false;
        } else {
            restored = false;
        }
    }
    return restored;
}

// ---------------------------------------------------------------------------
// Verification and installation
// ---------------------------------------------------------------------------

auto Verify(std::uint64_t rva, const std::uint8_t* expected, std::size_t size,
        const char* label) noexcept -> bool {
    if (Context->CheckExpectedBytes(rva, expected, static_cast<std::uint32_t>(size))) return true;
    D2RL::LogErrorF(Context,
        "HolyFreezeDamage: %s at 0x%llX does not match the verified D2R image, or another "
        "plugin already owns it. Refusing to load.",
        label, static_cast<unsigned long long>(rva));
    return false;
}

auto VerifyNativeContract() noexcept -> bool {
    if (!Verify(HandlerCallbackRva, HandlerCallbackWitness, sizeof(HandlerCallbackWitness),
            "the Holy Freeze callback installation")) {
        return false;
    }
    if (!Verify(GateRva, GateWitness, sizeof(GateWitness), "the ColdEffect gate")) {
        return false;
    }
    if (Settings.suppressShatter
        && !Verify(ShatterTailRva, ShatterTailWitness, sizeof(ShatterTailWitness),
                "the shatter tail")) {
        return false;
    }
    return true;
}

auto BuildHookPage() noexcept -> bool {
    HookPage = AllocateNear(Base + ShatterTailRva, HookPageBytes);
    if (HookPage == nullptr) {
        Context->LogError("HolyFreezeDamage: no hook page was available within rel32 reach.");
        return false;
    }

    auto* page = static_cast<std::uint8_t*>(HookPage);
    std::memset(page, 0xCC, HookPageBytes);
    std::memcpy(page + StubPageOffset, ShatterStub, sizeof(ShatterStub));
    WriteQword(page + StubPageOffset + StubPredicateImm64,
        reinterpret_cast<std::uint64_t>(&TargetIsGated));
    WriteQword(page + StubPageOffset + StubSeedImm64, Base + GetUnitSeedRva);
    WriteQword(page + StubPageOffset + StubResumeSlot, Base + ShatterResumeRva);
    WriteQword(page + StubPageOffset + StubEpilogueSlot, Base + ReturnOneRva);

    DWORD previous = 0;
    if (!VirtualProtect(page, HookPageBytes, PAGE_EXECUTE_READ, &previous)) {
        Context->LogError("HolyFreezeDamage: hook page protection could not be finalized.");
        VirtualFree(HookPage, 0, MEM_RELEASE);
        HookPage = nullptr;
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), page, HookPageBytes);

    if (!CanEncodeRel32(Base + ShatterTailRva, PageAddress(StubPageOffset))) {
        Context->LogError("HolyFreezeDamage: hook page displacement validation failed.");
        VirtualFree(HookPage, 0, MEM_RELEASE);
        HookPage = nullptr;
        return false;
    }
    return true;
}

// Returns false only when nothing in the image can reach this DLL any more, so
// the loader may safely unload it.
auto InstallHooks() noexcept -> bool {
    if (Settings.suppressShatter && !BuildHookPage()) return false;

    std::uint8_t bytes[16]{};
    bool failed = false;

    // The shatter tail goes first. On its own it changes nothing, because
    // vanilla never lets an unchillable target reach it.
    if (Settings.suppressShatter) {
        EncodeSiteJump(Base + ShatterTailRva, PageAddress(StubPageOffset), bytes,
            ShatterHookSize);
        if (Context->PatchBytes(ShatterTailRva, ShatterTailWitness, ShatterHookSize,
                bytes, ShatterHookSize)) {
            ShatterPatched = true;
        } else {
            Context->LogError("HolyFreezeDamage: the shatter tail could not be redirected.");
            failed = true;
        }
    }

    if (!failed) {
        EncodeSiteJump(Base + GateReturnZeroRva, Base + DamageBlockRva, bytes, GateHookSize);
        if (Context->PatchBytes(GateReturnZeroRva, GateWitness + GateReturnZeroOffsetInWitness,
                GateHookSize, bytes, GateHookSize)) {
            GatePatched = true;
        } else {
            Context->LogError("HolyFreezeDamage: the ColdEffect gate could not be redirected.");
            failed = true;
        }
    }

    if (!failed) {
        State = HookState::Armed;
        return true;
    }

    // The page is kept on every failure path: a thread may already be inside
    // the stub, and after the retarget nothing in it points into this DLL.
    const bool retargeted = RetargetStubToVanilla();
    const bool restored = RestoreSites();
    if (restored && retargeted) return false;
    if (retargeted) {
        Context->LogError(
            "HolyFreezeDamage: a site could not be restored; it now runs native code through "
            "the hook page, with no behaviour change.");
        return false;
    }
    Context->LogError(
        "HolyFreezeDamage: rollback failed, staying loaded so the patched site keeps a valid "
        "target. The fix is only partially applied.");
    State = HookState::PartiallyArmed;
    return true;
}

// ---------------------------------------------------------------------------
// Console command
// ---------------------------------------------------------------------------

auto StateName() noexcept -> const char* {
    switch (State) {
    case HookState::DisabledByConfig: return "disabled by config";
    case HookState::Armed:            return "armed";
    case HookState::PartiallyArmed:   return "PARTIALLY armed, see log";
    default:                          return "not loaded";
    }
}

auto __cdecl StatusCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command,
        void*) noexcept -> D2RL::ConsoleCommandResult {
    if (command == nullptr || command->plugin == nullptr) return D2RL::ConsoleCommandResult::Failed;

    char message[384];
    std::snprintf(message, sizeof(message),
        "Holy Freeze Damage: %s | gate retarget %s | shatter suppression %s | "
        "unchillable targets damaged %llu | ordinary targets %llu",
        StateName(),
        GatePatched ? "on" : "off",
        ShatterPatched ? "on" : (Settings.suppressShatter ? "off" : "off by config"),
        static_cast<unsigned long long>(DamagedTargets.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(NormalTargets.load(std::memory_order_relaxed)));
    command->plugin->WriteConsoleMessage(message);
    return D2RL::ConsoleCommandResult::Handled;
}

void RegisterStatusCommand() noexcept {
    if (!Context->RegisterConsoleCommand("holyfreeze", &StatusCommand,
            "Reports Holy Freeze Damage status and counters.")) {
        Context->LogWarn("HolyFreezeDamage: the status console command was refused.");
    }
}

constexpr D2RL::PluginInfo Info{
    .infoSize    = D2RL::PluginInfoSize,
    .abiVersion  = D2RL_PLUGIN_ABI_VERSION,
    .id          = "celestialrayone.holy-freeze-damage",
    .name        = "Holy Freeze Damage",
    .version     = "1.0.0",
    .author      = "CelestialRayOne",
    .description = "Holy Freeze deals its cold damage to unchillable monsters, prime evils "
                   "included. They still get no aura state, no slow and no shatter.",
    .flags       = D2RL::PluginFlags::Server | D2RL::PluginFlags::NativeHooks,
};

}  // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
    return &Info;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
    if (!D2RL::HasContext(context)) return false;
    Context = context;
    Base = context->exeBase;
    GetUnitType = reinterpret_cast<GetUnitTypeFn>(Base + GetUnitTypeRva);
    GetClassId = reinterpret_cast<GetClassIdFn>(Base + GetClassIdRva);
    GetMonStatsRecord = reinterpret_cast<GetMonStatsRecordFn>(Base + GetMonStatsRecordRva);

    ReadConfiguration();

    if (!Settings.enabled) {
        State = HookState::DisabledByConfig;
        Context->LogInfo("HolyFreezeDamage: disabled by configuration.");
        RegisterStatusCommand();
        return true;
    }

    if (!VerifyNativeContract()) return false;
    if (!InstallHooks()) return false;

    D2RL::LogInfoF(Context,
        "HolyFreezeDamage: %s. Gate retarget 56150E -> 5615BF %s, shatter suppression %s.",
        StateName(), GatePatched ? "on" : "off", ShatterPatched ? "on" : "off");

    RegisterStatusCommand();
    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    if (Context == nullptr) return;
    RetargetStubToVanilla();
    RestoreSites();
    // The hook page is deliberately kept: a thread may be inside the stub right
    // now, and a site that could not be restored still needs it.
}

}  // namespace CelestialRayOne::HolyFreezeDamage
