// Progressive Stack Calc
//
// Ports the 2.4 "make prgstack work off calc3 instead" pair of memory
// overrides to D2R 3.3, as a plugin with the calc column configurable.
//
// What the original edit actually did (recovered by disassembling the 2.4
// caves rather than trusting their description):
//
//   The hook did NOT replace the prgstack test. It retargeted the existing
//   conditional jump, so the cave ran only on the branch where the prgstack
//   bit was CLEAR. The cave then evaluated the skill's calc3 column and, if
//   the result was > 0, jumped to the fallthrough, i.e. behaved as though
//   prgstack were set. So the real rule is:
//
//       stack the lower stages  <=>  prgstack == 1  OR  calcN > 0
//
//   That is reproduced here exactly. A row that already has prgstack set in
//   skills.txt keeps vanilla behaviour and never reaches this code.
//
// Native background, all of it read out of the live 3.3 image (the dumped
// 140000000.D2RLoader.exe) and disassembled before being relied on here:
//
//   prgstack is a bit column. From the skills.txt binder sub_140302380 its
//   descriptor is type 0x1D (bit), record offset 0x24, bit index 7, and the
//   mask global 0x141D996EC is entry 7 of the power-of-two table at
//   0x141D996D0, so the test is [skillRecord+0x24] & 0x80.
//
//   SERVER, sub_14056C3E0 (progressive release; dispatches srvprgfunc1..3
//   from the word array at skillRecord+80 through the srvdofunc table at
//   0x238EA00):
//     0x56C78D  movzx ecx,byte [rdx+0x24]
//     0x56C791  test  byte [rip+0x182CF55],cl        ; mask 0x80 = prgstack
//     0x56C797  je    0x56C8A7                       ; <- the six bytes moved
//     0x56C79D  fallthrough: the loop that fires stages 1..N-1
//   Live state at 0x56C797: rdx = skills.txt record, rbp+0x30 = game,
//   rbp+0x38 = unit, [rsp+0x5C] = skill id, [rsp+0x4C] = skill level.
//   rbp is a real frame pointer here (prologue: mov rax,rsp / mov [rax+8],rcx
//   / mov [rax+0x10],rdx / push rbp / lea rbp,[rax-0x28]), so rbp+0x30 and
//   rbp+0x38 are the home slots of the game and unit arguments.
//
//   CLIENT, sub_140216380 (the mirror; dispatches cltprgfunc1..3 from the
//   word array at skillRecord+332 through the table at 0x14235F7D0):
//     0x2165D0  movzx ecx,byte [rsi+0x24]
//     0x2165E2  test  byte [rip+0x1B83104],cl
//     0x2165F2  je    0x2166B6                       ; <- the six bytes moved
//     0x2165F8  fallthrough: the loop that fires stages 1..N-1
//   Live state at 0x2165F2: rsi = skills.txt record, rdi = unit,
//   r15b = data context (set at 0x2163EB from sub_14034A0E0, before every
//   path into the gate), [rsp+0xC0] = skill id, [rsp+0xC8] = skill level.
//
//   Both vanilla branch destinations begin with a flag-setting or
//   flag-indifferent instruction (0x56C79D mov ecx,[rsp+0x44]; 0x56C8A7
//   xor edi,edi; 0x2165F8 movsxd r14,r13d; 0x2166B6 movsxd rax,r13d), so
//   restoring the incoming flags on both paths is safe.
//
//   SKILLS_EvaluateFormula @ 0x3B5160, ABI
//     int __fastcall(uint8 dataContext, void* unit, uint32 poolOffset,
//                    int skillId, int skillLevel)
//   It bounds-checks poolOffset against the compiled formula pool itself and
//   returns 0 for anything out of range, so a blank column can never misfire.
//   The stub still tests for 0xFFFFFFFF first and skips the call outright,
//   because a blank column is the common case on this path.
//
//   skills.txt calc column offsets, read from the same binder:
//     calc1 400 (0x190), calc2 404, calc3 408, calc4 412, calc5 416,
//     calc6 420, calc7 424, calc8 428, calc9 432, calc10 436 (0x1B4)
//   Corroborated by Static Field (sub_1405546B0), which passes record+400
//   and record+404 straight to the evaluator.
//
// The six bytes of each "je rel32" are rewritten to a "je rel32" aimed at a
// relay stub allocated within rel32 reach. The stub preserves every general
// purpose register, the stack pointer, the flags and xmm0-5, evaluates the
// configured column, and returns into either the vanilla jump target or the
// vanilla fallthrough by way of a destination slot reserved below its saves.
// The 2.4 cave did not do that: it clobbered ebx on the server side and r11d
// on the client side on the assumption that both were dead.

#include <D2RLPlugin/api.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace CelestialRayOne::PrgStackCalc {
namespace {

// ---------------------------------------------------------------------------
// Native anchors
// ---------------------------------------------------------------------------

constexpr std::uintptr_t ProgressiveReleaseRva   = 0x56C3E0;
constexpr std::uintptr_t ServerGateContextRva    = 0x56C785;
constexpr std::uintptr_t ServerGateRva           = 0x56C797;
constexpr std::uintptr_t ServerFallthroughRva    = 0x56C79D;
constexpr std::uintptr_t ServerSkipTargetRva     = 0x56C8A7;

constexpr std::uintptr_t ClientReleaseRva        = 0x216380;
constexpr std::uintptr_t ClientGateContextRva    = 0x2165D0;
constexpr std::uintptr_t ClientGateRva           = 0x2165F2;
constexpr std::uintptr_t ClientFallthroughRva    = 0x2165F8;
constexpr std::uintptr_t ClientSkipTargetRva     = 0x2166B6;

constexpr std::uintptr_t EvaluateFormulaRva      = 0x3B5160;

// skills.txt record layout, from the binder sub_140302380.
constexpr std::uint32_t Calc1Offset              = 0x190;
constexpr std::uint32_t CalcStride               = 4;
constexpr std::uint32_t FirstCalcColumn          = 1;
constexpr std::uint32_t LastCalcColumn           = 10;

// Frame slots the stubs read, before the stub's own saves are accounted for.
constexpr std::uint32_t GameDataContextOffset    = 0x106;   // game + 262
constexpr std::uint32_t ServerSkillIdSlot        = 0x5C;
constexpr std::uint32_t ServerSkillLevelSlot     = 0x4C;
constexpr std::uint32_t ClientSkillIdSlot        = 0xC0;
constexpr std::uint32_t ClientSkillLevelSlot     = 0xC8;

// lea rsp,[rsp-8] + 7 pushes + pushfq = 0x48 bytes on top of the host frame.
constexpr std::uint32_t SavedBytes               = 0x48;

constexpr std::uint32_t GateSize                 = 6;       // 0F 84 rel32
constexpr std::size_t   MaximumConfigBytes       = 32'768;
constexpr std::size_t   RelayBytes               = 4'096;
constexpr std::size_t   StubCapacity             = 320;

// 0x56C3E0 entry, 36 bytes. sub_14056C3E0 prologue.
constexpr auto ProgressiveReleaseExpected = std::to_array<std::uint8_t>({
    0x4D, 0x85, 0xC9, 0x0F, 0x84, 0xEA, 0x05, 0x00,
    0x00, 0x48, 0x8B, 0xC4, 0x44, 0x89, 0x40, 0x18,
    0x48, 0x89, 0x50, 0x10, 0x48, 0x89, 0x48, 0x08,
    0x55, 0x48, 0x8D, 0x68, 0xD8, 0x48, 0x81, 0xEC,
    0x20, 0x01, 0x00, 0x00,
});

// 0x56C785, 24 bytes, ending on the gate itself.
//   mov rdx,[rsp+0x50] / mov r15d,eax / movzx ecx,byte [rdx+0x24]
//   test byte [rip+0x182CF55],cl / je 0x56C8A7
constexpr auto ServerGateContextExpected = std::to_array<std::uint8_t>({
    0x48, 0x8B, 0x54, 0x24, 0x50, 0x44, 0x8B, 0xF8,
    0x0F, 0xB6, 0x4A, 0x24, 0x84, 0x0D, 0x55, 0xCF,
    0x82, 0x01, 0x0F, 0x84, 0x0A, 0x01, 0x00, 0x00,
});

// The six bytes that move.
constexpr auto ServerGateExpected = std::to_array<std::uint8_t>({
    0x0F, 0x84, 0x0A, 0x01, 0x00, 0x00,
});

// 0x56C79D, the stacking path.  mov ecx,[rsp+0x44] / xor edi,edi
constexpr auto ServerFallthroughExpected = std::to_array<std::uint8_t>({
    0x8B, 0x4C, 0x24, 0x44, 0x33, 0xFF,
});

// 0x56C8A7, the vanilla jump target.  xor edi,edi / mov rcx,[rbp+0x38]
constexpr auto ServerSkipTargetExpected = std::to_array<std::uint8_t>({
    0x33, 0xFF, 0x48, 0x8B, 0x4D, 0x38,
});

// 0x216380 entry, 24 bytes. sub_140216380 prologue.
//   mov rax,rsp / mov [rax+0x20],r9d / mov [rax+0x18],r8d
//   push rbx / push rbp / push rdi / push r13 / sub rsp,0x88
constexpr auto ClientReleaseExpected = std::to_array<std::uint8_t>({
    0x48, 0x8B, 0xC4, 0x44, 0x89, 0x48, 0x20, 0x44,
    0x89, 0x40, 0x18, 0x53, 0x55, 0x57, 0x41, 0x55,
    0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00, 0x44,
});

// 0x2165D0, 40 bytes, ending on the gate itself.
constexpr auto ClientGateContextExpected = std::to_array<std::uint8_t>({
    0x0F, 0xB6, 0x4E, 0x24, 0x48, 0x8D, 0x15, 0xF5,
    0x91, 0x14, 0x02, 0x41, 0xFF, 0xCD, 0x89, 0x44,
    0x24, 0x50, 0x84, 0x0D, 0x04, 0x31, 0xB8, 0x01,
    0x8B, 0xE8, 0x44, 0x89, 0xAC, 0x24, 0xE0, 0x00,
    0x00, 0x00, 0x0F, 0x84, 0xBE, 0x00, 0x00, 0x00,
});

constexpr auto ClientGateExpected = std::to_array<std::uint8_t>({
    0x0F, 0x84, 0xBE, 0x00, 0x00, 0x00,
});

// 0x2165F8, the stacking path.  movsxd r14,r13d / test r13d,r13d
constexpr auto ClientFallthroughExpected = std::to_array<std::uint8_t>({
    0x4D, 0x63, 0xF5, 0x45, 0x85, 0xED,
});

// 0x2166B6, the vanilla jump target.  movsxd rax,r13d / lea rbx,[rsi+rax*2]
constexpr auto ClientSkipTargetExpected = std::to_array<std::uint8_t>({
    0x49, 0x63, 0xC5, 0x48, 0x8D, 0x1C, 0x46,
});

// 0x3B5160 entry, 32 bytes. SKILLS_EvaluateFormula.
constexpr auto EvaluateFormulaExpected = std::to_array<std::uint8_t>({
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
    0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57,
    0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0xB0,
    0x00, 0x00, 0x00, 0x45, 0x8B, 0xF1, 0x41, 0x8B,
});

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

struct Config {
    bool          enabled     = true;
    std::uint32_t calcColumn  = 3;
    bool          server      = true;
    bool          client      = true;
};

constexpr char DefaultConfigToml[] =
    "# Progressive Stack Calc\n"
    "#\n"
    "# Lets a skills.txt calc column decide, per skill and per level, whether a\n"
    "# progressive (charge-up) skill fires every stage it has built up or only\n"
    "# the final one. Vanilla decides that with the boolean prgstack column,\n"
    "# which is fixed for the whole row.\n"
    "#\n"
    "# Rule the engine ends up with:\n"
    "#\n"
    "#     stack the lower stages  <=>  prgstack = 1  OR  <calc column> > 0\n"
    "#\n"
    "# A row that already has prgstack = 1 is untouched and never reaches this\n"
    "# code. On every other row the calc column decides, so a blank or\n"
    "# zero-valued column is exactly vanilla.\n"
    "\n"
    "[prgstack_calc]\n"
    "\n"
    "# Master switch. false installs nothing at all.\n"
    "enabled = true\n"
    "\n"
    "# Which skills.txt calc column carries the formula. 1 to 10.\n"
    "#\n"
    "#   Site     skills.txt calc<n>, read off the skill the charge belongs to,\n"
    "#            evaluated with that skill's id and level, so the usual clc\n"
    "#            and lvl terms all work.\n"
    "#   Meaning  greater than 0 stacks, 0 or less or blank does not.\n"
    "#   Scope    only rows the progressive release path actually runs on, that\n"
    "#            is rows with progressive = 1 and a valid aurastate.\n"
    "#   Trap     do not pick a column the skill's own srvprgfunc or cltprgfunc\n"
    "#            already reads, or the two uses will fight over it. calc3 is\n"
    "#            the one the 2.4 build used and is free on the charge-up rows.\n"
    "#   Cost     one formula evaluation per progressive release, and none at\n"
    "#            all when the column is blank.\n"
    "#   Default  3\n"
    "calc_column = 3\n"
    "\n"
    "# The two halves can be switched independently, but they are meant to be\n"
    "# kept in step: the server half decides which srvprgfunc stages actually\n"
    "# fire, the client half decides which cltprgfunc stages are drawn. Turning\n"
    "# on only one gives visuals without effects, or effects without visuals.\n"
    "#\n"
    "#   Site     server sub_14056C3E0 at 0x56C797, srvprgfunc1..3\n"
    "#   Default  true\n"
    "server = true\n"
    "\n"
    "#   Site     client sub_140216380 at 0x2165F2, cltprgfunc1..3\n"
    "#   Default  true\n"
    "client = true\n";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

const D2RL::PluginContext* Context{};
std::uint8_t*              Base{};
Config                     Settings{};
void*                      RelayPage{};
bool                       ServerInstalled{};
bool                       ClientInstalled{};

// ---------------------------------------------------------------------------
// Config parsing (small hand-rolled TOML subset, no external dependency)
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

auto ParseBool(std::string_view value, bool& out) noexcept -> bool {
    if (value == "true" || value == "1") { out = true; return true; }
    if (value == "false" || value == "0") { out = false; return true; }
    return false;
}

auto ParseInteger(std::string_view value, std::int64_t& out) noexcept -> bool {
    if (value.empty()) return false;
    bool negative = false;
    std::size_t index = 0;
    if (value[0] == '+' || value[0] == '-') {
        negative = value[0] == '-';
        index = 1;
    }
    if (index >= value.size()) return false;
    std::int64_t accumulator = 0;
    for (; index < value.size(); ++index) {
        const char character = value[index];
        if (character == '_') continue;
        if (character < '0' || character > '9') return false;
        accumulator = accumulator * 10 + (character - '0');
        if (accumulator > 0x7FFF'FFFFLL) return false;
    }
    out = negative ? -accumulator : accumulator;
    return true;
}

void ApplyConfigLine(std::string_view key, std::string_view value) noexcept {
    std::int64_t number = 0;
    if (key == "enabled") {
        ParseBool(value, Settings.enabled);
    } else if (key == "calc_column") {
        if (ParseInteger(value, number)
                && number >= static_cast<std::int64_t>(FirstCalcColumn)
                && number <= static_cast<std::int64_t>(LastCalcColumn)) {
            Settings.calcColumn = static_cast<std::uint32_t>(number);
        }
    } else if (key == "server") {
        ParseBool(value, Settings.server);
    } else if (key == "client") {
        ParseBool(value, Settings.client);
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
        if (line.empty() || line.front() == '[') continue;

        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) continue;
        ApplyConfigLine(Trim(line.substr(0, equals)), Trim(line.substr(equals + 1)));

        if (breakAt == std::string_view::npos) break;
    }
}

auto ReadConfiguration() noexcept -> bool {
    if (!Context->EnsureConfig(DefaultConfigToml)) {
        Context->LogWarn(
            "PrgStackCalc: config file could not be created; using defaults.");
        return true;
    }
    std::string buffer(MaximumConfigBytes, '\0');
    std::uint32_t required = 0;
    if (!Context->ReadConfig(buffer.data(),
            static_cast<std::uint32_t>(buffer.size()), &required)) {
        Context->LogWarn(
            "PrgStackCalc: config file could not be read; using defaults.");
        return true;
    }
    buffer.resize(std::strlen(buffer.c_str()));
    ParseConfig(buffer);
    return true;
}

auto CalcOffset() noexcept -> std::uint32_t {
    return Calc1Offset + CalcStride * (Settings.calcColumn - FirstCalcColumn);
}

// ---------------------------------------------------------------------------
// Relay page
// ---------------------------------------------------------------------------

auto CanEncodeRel32(std::uintptr_t from, std::uintptr_t to) noexcept -> bool {
    const std::int64_t delta =
        static_cast<std::int64_t>(to) - static_cast<std::int64_t>(from) - 5;
    return delta >= INT32_MIN && delta <= INT32_MAX;
}

auto AllocateNear(void* hint, std::size_t size) noexcept -> void* {
    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    const auto granularity =
        static_cast<std::uintptr_t>(systemInfo.dwAllocationGranularity);
    const auto aligned =
        reinterpret_cast<std::uintptr_t>(hint) & ~(granularity - 1U);
    for (std::uintptr_t delta = granularity;
            delta < 0x7000'0000ULL; delta += granularity) {
        const auto candidate = aligned + delta;
        if (!CanEncodeRel32(reinterpret_cast<std::uintptr_t>(hint), candidate)) break;
        if (auto* memory = VirtualAlloc(reinterpret_cast<void*>(candidate), size,
                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) {
            return memory;
        }
    }
    return nullptr;
}

// Builds the relay a retargeted "je" lands in.
//
// Entry contract, proven by the bytes immediately before each gate: on the
// server rdx is the skills.txt record and rbp is the host frame pointer; on
// the client rsi is the record, rdi the unit and r15b the data context. The
// gate is reached with the flags the host produced, and every register in the
// host frame is live, so nothing may be left disturbed.
//
// Shape:
//   reserve a destination slot, save the volatiles, the flags and xmm0-5,
//   read the five evaluator inputs out of the host frame, evaluate the calc
//   column, pick the vanilla fallthrough when the result is > 0 and the
//   vanilla jump target otherwise, write it into the slot, restore
//   everything, and RET into it.
//
// The slot is reserved with LEA rather than SUB on purpose: SUB writes the
// flags, and at that point the host's flags have not been captured yet.
auto BuildStub(std::uint8_t* out, bool server, std::uint32_t calcOffset,
        std::uintptr_t evaluator, std::uintptr_t skipTarget,
        std::uintptr_t stackTarget) noexcept -> std::size_t {
    std::size_t index = 0;
    const auto emit = [&](std::initializer_list<std::uint8_t> bytes) {
        for (const auto byte : bytes) out[index++] = byte;
    };
    const auto emit32 = [&](std::uint32_t value) {
        std::memcpy(out + index, &value, sizeof(value));
        index += sizeof(value);
    };
    const auto emit64 = [&](std::uint64_t value) {
        std::memcpy(out + index, &value, sizeof(value));
        index += sizeof(value);
    };

    emit({0x48, 0x8D, 0x64, 0x24, 0xF8});       // lea rsp,[rsp-8]  destination slot
    emit({0x50});                               // push rax
    emit({0x51});                               // push rcx
    emit({0x52});                               // push rdx
    emit({0x41, 0x50});                         // push r8
    emit({0x41, 0x51});                         // push r9
    emit({0x41, 0x52});                         // push r10
    emit({0x41, 0x53});                         // push r11
    emit({0x9C});                               // pushfq

    if (server) {
        emit({0x44, 0x8B, 0x9A}); emit32(calcOffset);           // mov r11d,[rdx+calc]
        emit({0x4C, 0x8B, 0x55, 0x38});                         // mov r10,[rbp+0x38]   unit
        emit({0x48, 0x8B, 0x45, 0x30});                         // mov rax,[rbp+0x30]   game
        emit({0x0F, 0xB6, 0x80}); emit32(GameDataContextOffset);// movzx eax,byte [rax+0x106]
        emit({0x44, 0x8B, 0x8C, 0x24}); emit32(ServerSkillIdSlot + SavedBytes);
        emit({0x44, 0x8B, 0x84, 0x24}); emit32(ServerSkillLevelSlot + SavedBytes);
    } else {
        emit({0x44, 0x8B, 0x9E}); emit32(calcOffset);           // mov r11d,[rsi+calc]
        emit({0x49, 0x89, 0xFA});                               // mov r10,rdi          unit
        emit({0x41, 0x0F, 0xB6, 0xC7});                         // movzx eax,r15b       context
        emit({0x44, 0x8B, 0x8C, 0x24}); emit32(ClientSkillIdSlot + SavedBytes);
        emit({0x44, 0x8B, 0x84, 0x24}); emit32(ClientSkillLevelSlot + SavedBytes);
    }

    emit({0x55});                               // push rbp
    emit({0x48, 0x89, 0xE5});                   // mov rbp,rsp
    emit({0x48, 0x83, 0xE4, 0xF0});             // and rsp,-16
    emit({0x48, 0x81, 0xEC}); emit32(0xA0);     // sub rsp,0xA0
    for (std::uint8_t slot = 0; slot < 6; ++slot) {
        emit({0x0F, 0x29,                       // movaps [rsp+0x30+0x10*i],xmmI
              static_cast<std::uint8_t>(0x84 | (slot << 3)), 0x24});
        emit32(0x30U + slot * 0x10U);
    }
    emit({0x4C, 0x89, 0x44, 0x24, 0x20});       // mov [rsp+0x20],r8    arg5 skill level
    emit({0x89, 0xC1});                         // mov ecx,eax          arg1 data context
    emit({0x4C, 0x89, 0xD2});                   // mov rdx,r10          arg2 unit
    emit({0x45, 0x89, 0xD8});                   // mov r8d,r11d         arg3 pool offset
    emit({0x31, 0xC0});                         // xor eax,eax
    emit({0x41, 0x83, 0xF8, 0xFF});             // cmp r8d,-1
    emit({0x74, 0x0C});                         // je +12   blank column, result stays 0
    emit({0x48, 0xB8}); emit64(evaluator);      // movabs rax,SKILLS_EvaluateFormula
    emit({0xFF, 0xD0});                         // call rax
    emit({0x85, 0xC0});                         // test eax,eax
    emit({0x48, 0xB8}); emit64(skipTarget);     // movabs rax,<vanilla je target>
    emit({0x49, 0xBA}); emit64(stackTarget);    // movabs r10,<vanilla fallthrough>
    emit({0x49, 0x0F, 0x4F, 0xC2});             // cmovg rax,r10
    emit({0x48, 0x89, 0x45, 0x48});             // mov [rbp+0x48],rax   destination slot
    for (std::uint8_t slot = 0; slot < 6; ++slot) {
        emit({0x0F, 0x28,                       // movaps xmmI,[rsp+0x30+0x10*i]
              static_cast<std::uint8_t>(0x84 | (slot << 3)), 0x24});
        emit32(0x30U + slot * 0x10U);
    }
    emit({0x48, 0x89, 0xEC});                   // mov rsp,rbp
    emit({0x5D});                               // pop rbp
    emit({0x9D});                               // popfq
    emit({0x41, 0x5B});                         // pop r11
    emit({0x41, 0x5A});                         // pop r10
    emit({0x41, 0x59});                         // pop r9
    emit({0x41, 0x58});                         // pop r8
    emit({0x5A});                               // pop rdx
    emit({0x59});                               // pop rcx
    emit({0x58});                               // pop rax
    emit({0xC3});                               // ret
    return index;
}

// ---------------------------------------------------------------------------
// Verification and installation
// ---------------------------------------------------------------------------

template <std::size_t Size>
auto Verify(std::uintptr_t rva, const std::array<std::uint8_t, Size>& expected,
        const char* label) noexcept -> bool {
    if (Context->CheckExpectedBytes(rva, expected.data(),
            static_cast<std::uint32_t>(expected.size()))) {
        return true;
    }
    char message[224];
    std::snprintf(message, sizeof(message),
        "PrgStackCalc: %s at 0x%llX does not match build 93847, or is already "
        "owned by another plugin. Refusing to load.",
        label, static_cast<unsigned long long>(rva));
    Context->LogError(message);
    return false;
}

auto VerifyNativeContract() noexcept -> bool {
    if (!Verify(EvaluateFormulaRva, EvaluateFormulaExpected, "formula evaluator")) {
        return false;
    }
    if (Settings.server
        && (!Verify(ProgressiveReleaseRva, ProgressiveReleaseExpected,
                    "server progressive release")
            || !Verify(ServerGateContextRva, ServerGateContextExpected,
                       "server prgstack gate context")
            || !Verify(ServerFallthroughRva, ServerFallthroughExpected,
                       "server stacking path")
            || !Verify(ServerSkipTargetRva, ServerSkipTargetExpected,
                       "server non-stacking path"))) {
        return false;
    }
    if (Settings.client
        && (!Verify(ClientReleaseRva, ClientReleaseExpected,
                    "client progressive release")
            || !Verify(ClientGateContextRva, ClientGateContextExpected,
                       "client prgstack gate context")
            || !Verify(ClientFallthroughRva, ClientFallthroughExpected,
                       "client stacking path")
            || !Verify(ClientSkipTargetRva, ClientSkipTargetExpected,
                       "client non-stacking path"))) {
        return false;
    }
    return true;
}

// Rewrites the six bytes of a "je rel32" so it lands in the stub instead.
template <std::size_t Size>
auto RetargetGate(std::uintptr_t gateRva,
        const std::array<std::uint8_t, Size>& expected,
        std::uintptr_t stubAddress, const char* label) noexcept -> bool {
    const auto imageBase = reinterpret_cast<std::uintptr_t>(Base);
    const auto gate = imageBase + gateRva;
    const std::int64_t delta =
        static_cast<std::int64_t>(stubAddress)
        - static_cast<std::int64_t>(gate + GateSize);
    if (delta < INT32_MIN || delta > INT32_MAX) {
        Context->LogError(
            "PrgStackCalc: the relay page is out of rel32 reach of a gate.");
        return false;
    }
    std::uint8_t bytes[GateSize] = {0x0F, 0x84, 0, 0, 0, 0};
    const auto rel32 = static_cast<std::int32_t>(delta);
    std::memcpy(bytes + 2, &rel32, sizeof(rel32));

    if (!Context->PatchBytes(gateRva, expected.data(),
            static_cast<std::uint32_t>(expected.size()), bytes, GateSize)) {
        char message[192];
        std::snprintf(message, sizeof(message),
            "PrgStackCalc: the %s gate at 0x%llX could not be retargeted.",
            label, static_cast<unsigned long long>(gateRva));
        Context->LogError(message);
        return false;
    }
    return true;
}

auto InstallHooks() noexcept -> bool {
    const auto imageBase = reinterpret_cast<std::uintptr_t>(Base);
    const auto hintRva = Settings.server ? ServerGateRva : ClientGateRva;

    RelayPage = AllocateNear(Base + hintRva, RelayBytes);
    if (!RelayPage) {
        Context->LogError(
            "PrgStackCalc: no relay page was available within rel32 reach.");
        return false;
    }
    auto* relay = static_cast<std::uint8_t*>(RelayPage);
    const auto relayBase = reinterpret_cast<std::uintptr_t>(relay);
    const auto calcOffset = CalcOffset();

    std::size_t cursor = 0;
    std::uintptr_t serverStub = 0;
    std::uintptr_t clientStub = 0;

    if (Settings.server) {
        serverStub = relayBase + cursor;
        cursor += BuildStub(relay + cursor, true, calcOffset,
            imageBase + EvaluateFormulaRva,
            imageBase + ServerSkipTargetRva,
            imageBase + ServerFallthroughRva);
        cursor = (cursor + 15) & ~static_cast<std::size_t>(15);
    }
    if (Settings.client) {
        clientStub = relayBase + cursor;
        cursor += BuildStub(relay + cursor, false, calcOffset,
            imageBase + EvaluateFormulaRva,
            imageBase + ClientSkipTargetRva,
            imageBase + ClientFallthroughRva);
        cursor = (cursor + 15) & ~static_cast<std::size_t>(15);
    }
    if (cursor > RelayBytes) {
        Context->LogError("PrgStackCalc: the relay page overflowed.");
        return false;
    }

    DWORD previousProtection = 0;
    if (!VirtualProtect(relay, RelayBytes, PAGE_EXECUTE_READ, &previousProtection)) {
        Context->LogError(
            "PrgStackCalc: relay page protection could not be finalized.");
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), relay, RelayBytes);

    if (Settings.server) {
        if (!RetargetGate(ServerGateRva, ServerGateExpected, serverStub, "server")) {
            return false;
        }
        ServerInstalled = true;
    }
    if (Settings.client) {
        if (!RetargetGate(ClientGateRva, ClientGateExpected, clientStub, "client")) {
            return false;
        }
        ClientInstalled = true;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Console command
// ---------------------------------------------------------------------------

auto __cdecl StatusCommand(D2R::Game::Client*,
        const D2RL::ConsoleCommandContext* command, void*) noexcept
        -> D2RL::ConsoleCommandResult {
    if (!command || !command->plugin) return D2RL::ConsoleCommandResult::Failed;
    char message[352];
    std::snprintf(message, sizeof(message),
        "Progressive Stack Calc: %s | column calc%u (record +%u) | "
        "server %s | client %s | rule: prgstack = 1 OR calc%u > 0",
        Settings.enabled ? "on" : "off",
        Settings.calcColumn,
        CalcOffset(),
        ServerInstalled ? "armed" : "off",
        ClientInstalled ? "armed" : "off",
        Settings.calcColumn);
    command->plugin->WriteConsoleMessage(message);
    return D2RL::ConsoleCommandResult::Handled;
}

constexpr D2RL::PluginInfo Info{
    .infoSize = D2RL::PluginInfoSize,
    .abiVersion = D2RL_PLUGIN_ABI_VERSION,
    .id = "celestialrayone.prgstack-calc",
    .name = "Progressive Stack Calc",
    .version = "1.0.0",
    .author = "CelestialRayOne",
    .description =
        "Makes the prgstack decision for charge-up skills come from a "
        "configurable skills.txt calc column instead of the fixed boolean.",
    .flags = D2RL::PluginFlags::Shared | D2RL::PluginFlags::NativeHooks,
};

}  // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept
        -> const D2RL::PluginInfo* {
    return &Info;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(
        const D2RL::PluginContext* context) noexcept -> bool {
    if (!D2RL::HasContext(context)) return false;
    Context = context;
    Base = reinterpret_cast<std::uint8_t*>(context->exeBase);

    if (!ReadConfiguration()) return false;

    if (!Settings.enabled) {
        Context->LogInfo("PrgStackCalc: disabled by configuration.");
        return true;
    }
    if (!Settings.server && !Settings.client) {
        Context->LogInfo(
            "PrgStackCalc: both halves are disabled; no hooks installed.");
        return true;
    }
    if (!VerifyNativeContract()) return false;
    if (!InstallHooks()) return false;

    char message[320];
    std::snprintf(message, sizeof(message),
        "PrgStackCalc: armed on skills.txt calc%u (skill record +%u). "
        "Server gate 0x%llX %s, client gate 0x%llX %s. A row keeps vanilla "
        "behaviour unless prgstack is set or the column evaluates above zero.",
        Settings.calcColumn,
        CalcOffset(),
        static_cast<unsigned long long>(ServerGateRva),
        ServerInstalled ? "retargeted" : "untouched",
        static_cast<unsigned long long>(ClientGateRva),
        ClientInstalled ? "retargeted" : "untouched");
    Context->LogInfo(message);

    if (!Context->RegisterConsoleCommand("prgstack", &StatusCommand,
            "Reports Progressive Stack Calc status.")) {
        Context->LogWarn(
            "PrgStackCalc: the status console command was refused.");
    }
    return true;
}

}  // namespace CelestialRayOne::PrgStackCalc
