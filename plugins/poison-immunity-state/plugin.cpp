// Poison Immunity
//
// Any player or monster that has a configurable state (ESR: state 25,
// "poisonimmunity" in states.txt) is immune to poison:
//   - the poisoned state is never applied to it, and
//   - the poison part of a hit is left out of the life that hit removes.
// Physical, fire, lightning, cold and magic damage from the same hit still
// land normally.
//
// Port of the ESR D2R 2.4 memory patches (state gate hook 321D9D into cave
// 3633D8, life-total hook 32677E into cave 363410) to D2R 3.3 on
// D2RLoader 1.3.0. Everything below was read out of the live D2RLoader.exe
// image and disassembled before being relied on.
//
// ---------------------------------------------------------------------------
// Why the 2.4 caves cannot be copied
// ---------------------------------------------------------------------------
//   Both 2.4 caves tested the state inline, [statlist+0AC8h] & 2000000h.
//   D2RLoader 1.3.0 owns state storage (32,768 states), so this plugin asks
//   the game's own predicate instead. 3.3 makes the same call itself inside
//   the damage calculation, for states 133 and 131:
//
//     44EBA3  45 39 77 3C           cmp  [r15+3Ch], r14d      ; poison length
//     44EBA7  7E 15                 jle  44EBBE
//     44EBA9  BA 85 00 00 00        mov  edx, 85h             ; state 133
//     44EBAE  48 8B CE              mov  rcx, rsi             ; defender
//     44EBB1  E8 FA 65 EE FF        call 3351B0               ; STATES_CheckState
//
//   STATES_CheckState @ 0x3351B0, int32 (unit, stateId), bounds the id first:
//
//     3351DF  48 8B CE              mov  rcx, rsi
//     3351E2  E8 F9 4E 01 00        call 34A0E0               ; data context, unit+1BDh
//     3351E7  0F B6 C8              movzx ecx, al
//     3351EA  E8 A1 B8 FC FF        call 300A90               ; data tables
//     3351EF  48 8B B8 98 02 00 00  mov  rdi, [rax+298h]      ; states count
//     ...                           cmp ebx, edi / jl ok, else assert
//     335281  E9 9A 0F FC FF        jmp  2F6220               ; the state getter
//
//   An id at or past the states count raises an assert inside D2RCore, so the
//   plugin bounds the configured id against that same count before calling
//   the predicate. The 2.4 cave had the same count gate.
//
// ---------------------------------------------------------------------------
// Part 1: the poison state, sub_1404518C0 (2.4 sub_140321D70)
// ---------------------------------------------------------------------------
//   void (attacker, defender, damage, length, source5, source6, source7)
//
//   Called from the damage resolver SUNITDMG_ExecuteEvents 0x44CE80, at
//   44D21A, 44D262 and 44D380, with r8d = [D2Damage+38h] (poison damage) and
//   r9d = [D2Damage+3Ch] (poison length). The hook sits on the entry, so it
//   covers any other caller as well.
//
//     4518C0  45 85 C9              test r9d, r9d             ; length
//     4518C3  0F 8E 61 03 00 00     jle  451C2A               ; the bare ret
//     4518C9  48 8B C4              mov  rax, rsp
//     ...                           push rbp/rsi/r14/r15, sub rsp,68h
//     4518DC  48 8B F2              mov  rsi, rdx             ; defender
//     4518E2  45 85 C0              test r8d, r8d             ; damage
//     4518E5  0F 8E 35 03 00 00     jle  451C20               ; epilogue
//     451903  ...                   GetDataTables(game context)+298h > 2
//     451A35  8D 53 02 48 8B CE 44 8D 43 01 E8 7C 3A EE FF
//                                   STATES_ToggleState(defender, 2, 1)
//     ...                           stat 74 = -damage on the new poison list
//     451C20  48 83 C4 68           add  rsp, 68h
//     451C24  41 5F 41 5E 5E 5D     pop  r15 / r14 / rsi / rbp
//     451C2A  C3                    ret
//
//   The 2.4 gate sat after both early-outs and left through the epilogue, so
//   for every caller it is the same as returning at the entry when damage > 0,
//   length > 0 and the defender has the state. No branch in the function
//   lands inside its first 9 bytes.
//
//   Those 9 bytes start with a relative jle, so they are not handed to
//   InstallInlineHook. They become a jmp to a stub that enters
//   HookedApplyPoison with the function's own ABI. The original is reached
//   through a second stub that replays test r9d,r9d / jle with absolute
//   targets, 451C2A and 4518C9.
//
// ---------------------------------------------------------------------------
// Part 2: the life a hit removes, SUNITDMG_CalculateTotalDamage 0x44DF10
// (2.4 sub_140325AF0)
// ---------------------------------------------------------------------------
//   void (game, attacker, defender, D2Damage*)
//   r15 = D2Damage (44DF54 mov r15,r9) and rsi = defender (44DF5E mov rsi,r8)
//   for the whole function; neither is written again before the epilogue.
//   Component offsets, from the function's own labels:
//     +18h Phys  +20h Fire  +2Ch Ligt  +30h Magc  +34h Cold  +38h Pois  +3Ch Plen
//
//     44EC71  41 8B 47 30           mov  eax, [r15+30h]       ; Magc
//     44EC75  4D 8B CD              mov  r9, r13              ; event 16 args
//     44EC78  41 03 47 34           add  eax, [r15+34h]       ; Cold
//     44EC7C  4C 8B C6              mov  r8, rsi
//     44EC7F  41 03 47 18           add  eax, [r15+18h]       ; Phys
//     44EC83  BA 10 00 00 00        mov  edx, 10h
//     44EC88  41 03 47 2C           add  eax, [r15+2Ch]       ; Ligt
//     44EC8C  41 03 47 38           add  eax, [r15+38h]       ; Pois  <- 8 byte hook
//     44EC90  41 03 47 20           add  eax, [r15+20h]       ; Fire
//     44EC94  48 8B 4C 24 30        mov  rcx, [rsp+30h]       ; resume
//     44EC99  41 89 87 34 01 00 00  mov  [r15+134h], eax      ; life damage total
//
//   SUNITDMG_ExecuteEvents subtracts [D2Damage+134h] from life with a floor
//   of 0 (44D062..44D093), so the poison part of a hit can kill.
//
//   With no poison on the hit, the stub replays the two original instructions
//   verbatim. Otherwise it saves every volatile register, the flags and
//   xmm0-5, asks PoisonHitpointTerm, adds the answer into the saved eax,
//   restores everything and replays add eax,[r15+20h], so rax and the flags
//   on exit match the original instructions exactly. edx, r8 and r9 already
//   hold the event 16 arguments here and come back unchanged. No branch in
//   the function lands inside the window.
//
//   Both stubs were assembled with keystone at their page offsets, decoded
//   back with capstone, and emulated against the original instructions with
//   randomized registers, flags, xmm state and stack alignment.

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
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace CelestialRayOne::PoisonImmunity {
namespace {

// ---------------------------------------------------------------------------
// Native anchors
// ---------------------------------------------------------------------------

constexpr std::uint64_t ApplyPoisonRva          = 0x4518C0;
constexpr std::uint64_t ApplyPoisonContinueRva  = 0x4518C9;
constexpr std::uint64_t ApplyPoisonReturnRva    = 0x451C2A;
constexpr std::uint64_t ApplyPoisonCountGateRva = 0x451903;
constexpr std::uint64_t ApplyPoisonStateRva     = 0x451A35;
constexpr std::uint64_t ApplyPoisonEpilogueRva  = 0x451C20;

constexpr std::uint64_t CalculateTotalDamageRva = 0x44DF10;
constexpr std::uint64_t NativeStateCheckRva     = 0x44EBA3;
constexpr std::uint64_t HitpointTotalRva        = 0x44EC71;
constexpr std::uint64_t PoisonTermRva           = 0x44EC8C;
constexpr std::uint64_t PoisonTermResumeRva     = 0x44EC94;

constexpr std::uint64_t CheckStateRva           = 0x3351B0;
constexpr std::uint64_t CheckStateCountRva      = 0x3351DF;
constexpr std::uint64_t GetDataContextRva       = 0x34A0E0;
constexpr std::uint64_t GetDataTablesRva        = 0x300A90;

constexpr std::size_t StatesCountOffset = 0x298;

constexpr std::uint32_t ApplyPoisonHookSize = 9;
constexpr std::uint32_t PoisonTermHookSize  = 8;
constexpr std::uint32_t JumpSize            = 5;

// 4518C0: the displaced entry (test r9d,r9d / jle 451C2A), then the frame
// setup that proves r8d = damage, r9d = length and rsi = defender.
constexpr std::uint8_t ApplyPoisonEntryWitness[]{
    0x45, 0x85, 0xC9, 0x0F, 0x8E, 0x61, 0x03, 0x00, 0x00, 0x48, 0x8B, 0xC4,
    0x55, 0x56, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x68, 0x45, 0x8B,
    0xF1, 0x41, 0x8B, 0xE8, 0x48, 0x8B, 0xF2, 0x4C, 0x8B, 0xF9, 0x45, 0x85,
    0xC0, 0x0F, 0x8E, 0x35, 0x03, 0x00, 0x00,
};

// 451903: movzx ecx,[rax+106h] / call 300A90 / mov rbx,rax /
// cmp qword [rax+298h],2. Live proof that the states count sits at +298h of
// the data tables.
constexpr std::uint8_t ApplyPoisonCountGateWitness[]{
    0x0F, 0xB6, 0x88, 0x06, 0x01, 0x00, 0x00, 0xE8, 0x81, 0xF1, 0xEA, 0xFF,
    0x48, 0x8B, 0xD8, 0x48, 0x83, 0xB8, 0x98, 0x02, 0x00, 0x00, 0x02,
};

// 451A35: lea edx,[rbx+2] / mov rcx,rsi / lea r8d,[rbx+1] / call 3354C0.
// rbx is 0 on this path, so this is STATES_ToggleState(defender, 2, 1).
constexpr std::uint8_t ApplyPoisonStateWitness[]{
    0x8D, 0x53, 0x02, 0x48, 0x8B, 0xCE, 0x44, 0x8D, 0x43, 0x01, 0xE8, 0x7C,
    0x3A, 0xEE, 0xFF,
};

// 451C20: add rsp,68h / pop r15 / pop r14 / pop rsi / pop rbp / ret.
constexpr std::uint8_t ApplyPoisonEpilogueWitness[]{
    0x48, 0x83, 0xC4, 0x68, 0x41, 0x5F, 0x41, 0x5E, 0x5E, 0x5D, 0xC3,
};

// 44DF10: the prologue through mov rsi,r8. Fails if the loader ever takes
// the function over, which would leave the body hook unreachable.
constexpr std::uint8_t CalculateTotalDamageWitness[]{
    0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41,
    0x57, 0x48, 0x8D, 0xAC, 0x24, 0x68, 0xFD, 0xFF, 0xFF, 0x48, 0x81, 0xEC,
    0x98, 0x03, 0x00, 0x00, 0x48, 0x8B, 0x05, 0x95, 0xD3, 0x57, 0x02, 0x48,
    0x33, 0xC4, 0x48, 0x89, 0x85, 0x80, 0x02, 0x00, 0x00, 0x4C, 0x8B, 0xEA,
    0x48, 0x89, 0x4C, 0x24, 0x30, 0x0F, 0xB6, 0x91, 0x04, 0x01, 0x00, 0x00,
    0x48, 0x8B, 0xF9, 0x48, 0x89, 0x4C, 0x24, 0x40, 0x4D, 0x8B, 0xF9, 0x0F,
    0xB6, 0x89, 0x06, 0x01, 0x00, 0x00, 0x49, 0x8B, 0xF0,
};

// 44EBA3: the native state test this plugin mirrors (state 133, defender).
constexpr std::uint8_t NativeStateCheckWitness[]{
    0x45, 0x39, 0x77, 0x3C, 0x7E, 0x15, 0xBA, 0x85, 0x00, 0x00, 0x00, 0x48,
    0x8B, 0xCE, 0xE8, 0xFA, 0x65, 0xEE, 0xFF,
};

// 44EC71: the whole life total, the displaced 8 bytes at +1Bh, and the
// store to [r15+134h].
constexpr std::uint8_t HitpointTotalWitness[]{
    0x41, 0x8B, 0x47, 0x30, 0x4D, 0x8B, 0xCD, 0x41, 0x03, 0x47, 0x34, 0x4C,
    0x8B, 0xC6, 0x41, 0x03, 0x47, 0x18, 0xBA, 0x10, 0x00, 0x00, 0x00, 0x41,
    0x03, 0x47, 0x2C, 0x41, 0x03, 0x47, 0x38, 0x41, 0x03, 0x47, 0x20, 0x48,
    0x8B, 0x4C, 0x24, 0x30, 0x41, 0x89, 0x87, 0x34, 0x01, 0x00, 0x00, 0x4C,
    0x89, 0x7C, 0x24, 0x20,
};

// 3351DF: the id bound inside STATES_CheckState, data context -> data
// tables -> [+298h].
constexpr std::uint8_t CheckStateCountWitness[]{
    0x48, 0x8B, 0xCE, 0xE8, 0xF9, 0x4E, 0x01, 0x00, 0x0F, 0xB6, 0xC8, 0xE8,
    0xA1, 0xB8, 0xFC, 0xFF, 0x48, 0x8B, 0xB8, 0x98, 0x02, 0x00, 0x00,
};

constexpr std::size_t PoisonTermOffsetInWitness = PoisonTermRva - HitpointTotalRva;

// ---------------------------------------------------------------------------
// Hook page
// ---------------------------------------------------------------------------

constexpr std::size_t HookPageBytes = 4'096;

constexpr std::size_t ApplierEntryStubOffset    = 0x00;
constexpr std::size_t ApplierOriginalStubOffset = 0x10;
constexpr std::size_t SummationStubOffset       = 0x40;

// +00  FF 25 00 00 00 00        jmp  qword ptr [rip+0]
// +06  dq                       HookedApplyPoison, or the original-entry stub
constexpr std::uint8_t ApplierEntryStub[]{
    0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00,
};
constexpr std::size_t ApplierEntryTargetSlot = 0x06;

// +00  45 85 C9                 test r9d, r9d
// +03  7E 0E                    jle  +13h
// +05  FF 25 00 00 00 00        jmp  qword ptr [rip+0]
// +0B  dq                       4518C9
// +13  FF 25 00 00 00 00        jmp  qword ptr [rip+0]
// +19  dq                       451C2A
constexpr std::uint8_t ApplierOriginalStub[]{
    0x45, 0x85, 0xC9, 0x7E, 0x0E, 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x25, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
constexpr std::size_t ApplierContinueSlot = 0x0B;
constexpr std::size_t ApplierReturnSlot   = 0x19;

// +00  FF 25 00 00 00 00        jmp  qword ptr [rip+0]
// +06  dq                       check path, or the plain path
// +0E  41 03 47 38              add  eax, [r15+38h]            ; plain path
// +12  41 03 47 20              add  eax, [r15+20h]
// +16  FF 25 00 00 00 00        jmp  qword ptr [rip+0]
// +1C  dq                       44EC94
// +24  41 83 7F 38 00           cmp  dword ptr [r15+38h], 0    ; check path
// +29  74 E3                    je   +0Eh
// +2B  push rax, rcx, rdx, r8, r9, r10, r11 / pushfq / push rbp
// +38  48 89 E5                 mov  rbp, rsp
// +3B  48 83 E4 F0              and  rsp, -16
// +3F  48 81 EC 80 00 00 00     sub  rsp, 80h
// +46  movaps [rsp+20h..70h], xmm0..xmm5
// +64  48 89 F1                 mov  rcx, rsi                  ; defender
// +67  41 8B 57 38              mov  edx, [r15+38h]            ; poison damage
// +6B  48 B8 dq                 mov  rax, PoisonHitpointTerm
// +75  FF D0                    call rax
// +77  01 45 40                 add  dword ptr [rbp+40h], eax  ; saved eax
// +7A  movaps xmm0..xmm5, [rsp+20h..70h]
// +98  48 89 EC                 mov  rsp, rbp
// +9B  pop rbp / popfq / pop r11, r10, r9, r8, rdx, rcx, rax
// +A8  41 03 47 20              add  eax, [r15+20h]
// +AC  FF 25 00 00 00 00        jmp  qword ptr [rip+0]
// +B2  dq                       44EC94
constexpr std::uint8_t SummationStub[]{
    0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x41, 0x03, 0x47, 0x38, 0x41, 0x03, 0x47, 0x20, 0xFF, 0x25,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x41, 0x83, 0x7F, 0x38, 0x00, 0x74, 0xE3, 0x50, 0x51, 0x52, 0x41, 0x50,
    0x41, 0x51, 0x41, 0x52, 0x41, 0x53, 0x9C, 0x55, 0x48, 0x89, 0xE5, 0x48,
    0x83, 0xE4, 0xF0, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00, 0x0F, 0x29,
    0x44, 0x24, 0x20, 0x0F, 0x29, 0x4C, 0x24, 0x30, 0x0F, 0x29, 0x54, 0x24,
    0x40, 0x0F, 0x29, 0x5C, 0x24, 0x50, 0x0F, 0x29, 0x64, 0x24, 0x60, 0x0F,
    0x29, 0x6C, 0x24, 0x70, 0x48, 0x89, 0xF1, 0x41, 0x8B, 0x57, 0x38, 0x48,
    0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0xFF, 0xD0, 0x01,
    0x45, 0x40, 0x0F, 0x28, 0x44, 0x24, 0x20, 0x0F, 0x28, 0x4C, 0x24, 0x30,
    0x0F, 0x28, 0x54, 0x24, 0x40, 0x0F, 0x28, 0x5C, 0x24, 0x50, 0x0F, 0x28,
    0x64, 0x24, 0x60, 0x0F, 0x28, 0x6C, 0x24, 0x70, 0x48, 0x89, 0xEC, 0x5D,
    0x9D, 0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58,
    0x41, 0x03, 0x47, 0x20, 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
constexpr std::size_t SummationDispatchSlot    = 0x06;
constexpr std::size_t SummationPlainOffset     = 0x0E;
constexpr std::size_t SummationPlainResumeSlot = 0x1C;
constexpr std::size_t SummationCheckOffset     = 0x24;
constexpr std::size_t SummationCallbackImm64   = 0x6D;
constexpr std::size_t SummationCheckResumeSlot = 0xB2;

constexpr auto JumpBefore(const std::uint8_t* code, std::size_t slot) noexcept -> bool {
    return slot >= 6 && code[slot - 6] == 0xFF && code[slot - 5] == 0x25
        && code[slot - 4] == 0x00 && code[slot - 3] == 0x00
        && code[slot - 2] == 0x00 && code[slot - 1] == 0x00;
}

static_assert(sizeof(ApplierEntryStub) == 14);
static_assert(sizeof(ApplierOriginalStub) == 33);
static_assert(sizeof(SummationStub) == 186);
static_assert(ApplierEntryStubOffset + sizeof(ApplierEntryStub) <= ApplierOriginalStubOffset);
static_assert(ApplierOriginalStubOffset + sizeof(ApplierOriginalStub) <= SummationStubOffset);
static_assert(SummationStubOffset + sizeof(SummationStub) <= HookPageBytes);
static_assert(JumpBefore(ApplierEntryStub, ApplierEntryTargetSlot));
static_assert(JumpBefore(ApplierOriginalStub, ApplierContinueSlot));
static_assert(JumpBefore(ApplierOriginalStub, ApplierReturnSlot));
static_assert(ApplierOriginalStub[0] == 0x45 && ApplierOriginalStub[3] == 0x7E);
static_assert(JumpBefore(SummationStub, SummationDispatchSlot));
static_assert(JumpBefore(SummationStub, SummationPlainResumeSlot));
static_assert(JumpBefore(SummationStub, SummationCheckResumeSlot));
static_assert(SummationStub[SummationCallbackImm64 - 2] == 0x48 && SummationStub[SummationCallbackImm64 - 1] == 0xB8);
static_assert(SummationStub[SummationPlainOffset] == 0x41 && SummationStub[SummationPlainOffset + 3] == 0x38);
static_assert(SummationStub[SummationCheckOffset] == 0x41 && SummationStub[SummationCheckOffset + 1] == 0x83);
static_assert(sizeof(HitpointTotalWitness) >= PoisonTermOffsetInWitness + PoisonTermHookSize);
static_assert(ApplyPoisonHookSize <= sizeof(ApplyPoisonEntryWitness));

constexpr std::size_t MaximumConfigBytes = 32'768;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

constexpr std::int32_t DefaultStateId = 25;
constexpr std::int32_t MaximumStateId = 0x7FFF;

struct Config {
    bool         enabled        = true;
    std::int32_t stateId        = DefaultStateId;
    bool         blockState     = true;
    bool         excludeFromHit = true;
};

constexpr char DefaultConfigToml[] =
    "# Poison Immunity\n"
    "#\n"
    "# Any player or monster that has the state below is immune to poison.\n"
    "# ESR repurposes states.txt state 25 (the old fetishaura row) as\n"
    "# poisonimmunity, so 25 is the default. Set state_id to whichever\n"
    "# states.txt state should grant the immunity.\n"
    "#\n"
    "# Two parts, each can be switched off on its own:\n"
    "#\n"
    "#   block_poison_state\n"
    "#     The poisoned state is never applied to an immune unit, so there is\n"
    "#     no poison damage over time.\n"
    "#\n"
    "#   exclude_poison_from_hit_damage\n"
    "#     The poison part of a hit no longer counts toward the life that hit\n"
    "#     removes. Poison over time cannot kill a player, but the hit that\n"
    "#     applies the poison is ordinary damage and can, which is how a player\n"
    "#     at 1 life still died. Physical, fire, lightning, cold and magic damage\n"
    "#     from the same hit still land.\n"
    "#\n"
    "# Keep both on for full immunity, the same as the ESR 2.4 patches.\n"
    "#\n"
    "# A unit that gains the state while already poisoned keeps that poison\n"
    "# until it runs out.\n"
    "#\n"
    "# Console command: poisonimmunity (status and counters)\n"
    "\n"
    "[poison_immunity]\n"
    "\n"
    "# Master switch.\n"
    "enabled = true\n"
    "\n"
    "# states.txt state that grants poison immunity, 0 to 32767.\n"
    "state_id = 25\n"
    "\n"
    "# Never apply the poisoned state to an immune unit.\n"
    "block_poison_state = true\n"
    "\n"
    "# Leave poison out of the life an immune unit loses from a hit.\n"
    "exclude_poison_from_hit_damage = true\n";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

enum class HookState : std::uint8_t {
    NotLoaded,
    DisabledByConfig,
    Armed,
    PartiallyArmed,
};

using CheckStateFn     = std::int32_t(__fastcall*)(void* unit, std::int32_t stateId);
using GetDataContextFn = std::uint8_t(__fastcall*)(void* unit);
using GetDataTablesFn  = void*(__fastcall*)(std::uint8_t context);
using ApplyPoisonFn    = void(__fastcall*)(std::uint64_t attacker, std::uint64_t defender,
    std::uint64_t damage, std::uint64_t length, std::uint64_t source5,
    std::uint64_t source6, std::uint64_t source7);

const D2RL::PluginContext* Context{};
std::uintptr_t             Base{};
CheckStateFn               CheckState{};
GetDataContextFn           GetDataContext{};
GetDataTablesFn            GetDataTables{};
ApplyPoisonFn              OriginalApplyPoison{};
Config                     Settings{};
HookState                  State{ HookState::NotLoaded };
void*                      HookPage{};
bool                       ApplyPoisonPatched{};
bool                       PoisonTermPatched{};

std::atomic<std::uint64_t> BlockedStates{};
std::atomic<std::uint64_t> ExcludedHits{};
std::atomic<bool>          StateIdPastCount{};

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

auto ParseBool(std::string_view value, bool& out) noexcept -> bool {
    if (value == "true" || value == "1") { out = true; return true; }
    if (value == "false" || value == "0") { out = false; return true; }
    return false;
}

auto ParseInteger(std::string_view value, std::int32_t& out) noexcept -> bool {
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
        if (accumulator > 1'000'000) return false;
    }
    out = static_cast<std::int32_t>(negative ? -accumulator : accumulator);
    return true;
}

bool StateIdInvalid = false;

void ApplyConfigLine(std::string_view key, std::string_view value) noexcept {
    std::int32_t number = 0;
    if (key == "enabled") {
        ParseBool(value, Settings.enabled);
    } else if (key == "state_id") {
        if (ParseInteger(value, number) && number >= 0 && number <= MaximumStateId) {
            Settings.stateId = number;
        } else {
            StateIdInvalid = true;
        }
    } else if (key == "block_poison_state") {
        ParseBool(value, Settings.blockState);
    } else if (key == "exclude_poison_from_hit_damage") {
        ParseBool(value, Settings.excludeFromHit);
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
        Context->LogWarn("PoisonImmunity: config file could not be created; using defaults.");
    } else {
        std::string buffer(MaximumConfigBytes, '\0');
        if (Context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size()), nullptr)) {
            buffer.resize(std::strlen(buffer.c_str()));
            ParseConfig(buffer);
        } else {
            Context->LogWarn("PoisonImmunity: config file could not be read; using defaults.");
        }
    }
    if (StateIdInvalid) {
        D2RL::LogWarnF(Context,
            "PoisonImmunity: state_id must be a whole number from 0 to %d; using %d.",
            MaximumStateId, Settings.stateId);
    }
}

// ---------------------------------------------------------------------------
// The immunity test, mirroring STATES_CheckState's own id bound
// ---------------------------------------------------------------------------

auto HasImmunityState(void* unit) noexcept -> bool {
    if (unit == nullptr) return false;

    const std::uint8_t context = GetDataContext(unit);
    const auto* tables = static_cast<const std::uint8_t*>(GetDataTables(context));
    if (tables == nullptr) return false;

    std::uint64_t statesCount = 0;
    std::memcpy(&statesCount, tables + StatesCountOffset, sizeof(statesCount));
    if (static_cast<std::uint64_t>(Settings.stateId) >= statesCount) {
        if (!StateIdPastCount.exchange(true, std::memory_order_relaxed)) {
            D2RL::LogWarnF(Context,
                "PoisonImmunity: state_id %d is past the %llu states loaded from "
                "states.txt, so no unit can be immune.",
                Settings.stateId, static_cast<unsigned long long>(statesCount));
        }
        return false;
    }

    return CheckState(unit, Settings.stateId) != 0;
}

// Reached from the entry stub with the poison applier's own ABI. Every
// argument is carried as a full 64-bit value so the forwarded call is
// bit-identical to the one the resolver made.
void __fastcall HookedApplyPoison(std::uint64_t attacker, std::uint64_t defender,
        std::uint64_t damage, std::uint64_t length, std::uint64_t source5,
        std::uint64_t source6, std::uint64_t source7) noexcept {
    if (static_cast<std::int32_t>(damage) > 0 && static_cast<std::int32_t>(length) > 0
            && HasImmunityState(reinterpret_cast<void*>(defender))) {
        BlockedStates.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    OriginalApplyPoison(attacker, defender, damage, length, source5, source6, source7);
}

// Reached from the summation stub only when the hit carries poison. Returns
// the value the life total gets for the poison term.
std::int32_t __fastcall PoisonHitpointTerm(void* defender, std::int32_t poisonDamage) noexcept {
    if (!HasImmunityState(defender)) return poisonDamage;
    ExcludedHits.fetch_add(1, std::memory_order_relaxed);
    return 0;
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

// E9 rel32 to the stub, the rest of the window filled with NOP.
void EncodeSiteJump(std::uintptr_t site, std::uintptr_t target, std::uint8_t* out,
        std::uint32_t windowSize) noexcept {
    out[0] = 0xE9;
    const auto displacement = static_cast<std::int32_t>(
        static_cast<std::int64_t>(target) - static_cast<std::int64_t>(site + JumpSize));
    std::memcpy(out + 1, &displacement, sizeof(displacement));
    std::memset(out + JumpSize, 0x90, windowSize - JumpSize);
}

// Points both stubs back at native behaviour. After this nothing in the page
// can reach this DLL.
auto RetargetStubsToVanilla() noexcept -> bool {
    if (HookPage == nullptr) return true;
    auto* page = static_cast<std::uint8_t*>(HookPage);
    DWORD previous = 0;
    if (!VirtualProtect(page, HookPageBytes, PAGE_EXECUTE_READWRITE, &previous)) return false;
    WriteQword(page + ApplierEntryStubOffset + ApplierEntryTargetSlot,
        PageAddress(ApplierOriginalStubOffset));
    WriteQword(page + SummationStubOffset + SummationDispatchSlot,
        PageAddress(SummationStubOffset + SummationPlainOffset));
    DWORD ignored = 0;
    VirtualProtect(page, HookPageBytes, previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), page, HookPageBytes);
    return true;
}

auto RestoreSites() noexcept -> bool {
    bool restored = true;
    std::uint8_t current[16]{};
    if (PoisonTermPatched) {
        EncodeSiteJump(Base + PoisonTermRva, PageAddress(SummationStubOffset), current,
            PoisonTermHookSize);
        if (Context->PatchBytes(PoisonTermRva, current, PoisonTermHookSize,
                HitpointTotalWitness + PoisonTermOffsetInWitness, PoisonTermHookSize)) {
            PoisonTermPatched = false;
        } else {
            restored = false;
        }
    }
    if (ApplyPoisonPatched) {
        EncodeSiteJump(Base + ApplyPoisonRva, PageAddress(ApplierEntryStubOffset), current,
            ApplyPoisonHookSize);
        if (Context->PatchBytes(ApplyPoisonRva, current, ApplyPoisonHookSize,
                ApplyPoisonEntryWitness, ApplyPoisonHookSize)) {
            ApplyPoisonPatched = false;
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
        "PoisonImmunity: %s at 0x%llX does not match the verified D2R image, or "
        "another plugin already owns it. Refusing to load.",
        label, static_cast<unsigned long long>(rva));
    return false;
}

auto VerifyNativeContract() noexcept -> bool {
    if (!Verify(CheckStateCountRva, CheckStateCountWitness, sizeof(CheckStateCountWitness),
            "the STATES_CheckState id bound")) {
        return false;
    }
    if (Settings.blockState
        && (!Verify(ApplyPoisonRva, ApplyPoisonEntryWitness, sizeof(ApplyPoisonEntryWitness),
                "the poison applier entry")
            || !Verify(ApplyPoisonCountGateRva, ApplyPoisonCountGateWitness,
                sizeof(ApplyPoisonCountGateWitness), "the poison applier states count gate")
            || !Verify(ApplyPoisonStateRva, ApplyPoisonStateWitness,
                sizeof(ApplyPoisonStateWitness), "the poison state application")
            || !Verify(ApplyPoisonEpilogueRva, ApplyPoisonEpilogueWitness,
                sizeof(ApplyPoisonEpilogueWitness), "the poison applier epilogue"))) {
        return false;
    }
    if (Settings.excludeFromHit
        && (!Verify(CalculateTotalDamageRva, CalculateTotalDamageWitness,
                sizeof(CalculateTotalDamageWitness), "the damage calculation entry")
            || !Verify(NativeStateCheckRva, NativeStateCheckWitness,
                sizeof(NativeStateCheckWitness), "the native state 133 test")
            || !Verify(HitpointTotalRva, HitpointTotalWitness, sizeof(HitpointTotalWitness),
                "the life damage total"))) {
        return false;
    }
    return true;
}

auto BuildHookPage() noexcept -> bool {
    const std::uint64_t lowest = Settings.excludeFromHit ? PoisonTermRva : ApplyPoisonRva;
    HookPage = AllocateNear(Base + lowest, HookPageBytes);
    if (HookPage == nullptr) {
        Context->LogError("PoisonImmunity: no hook page was available within rel32 reach.");
        return false;
    }

    auto* page = static_cast<std::uint8_t*>(HookPage);
    std::memset(page, 0xCC, HookPageBytes);

    std::memcpy(page + ApplierEntryStubOffset, ApplierEntryStub, sizeof(ApplierEntryStub));
    WriteQword(page + ApplierEntryStubOffset + ApplierEntryTargetSlot,
        reinterpret_cast<std::uint64_t>(&HookedApplyPoison));

    std::memcpy(page + ApplierOriginalStubOffset, ApplierOriginalStub, sizeof(ApplierOriginalStub));
    WriteQword(page + ApplierOriginalStubOffset + ApplierContinueSlot, Base + ApplyPoisonContinueRva);
    WriteQword(page + ApplierOriginalStubOffset + ApplierReturnSlot, Base + ApplyPoisonReturnRva);

    std::memcpy(page + SummationStubOffset, SummationStub, sizeof(SummationStub));
    WriteQword(page + SummationStubOffset + SummationDispatchSlot,
        PageAddress(SummationStubOffset + SummationCheckOffset));
    WriteQword(page + SummationStubOffset + SummationPlainResumeSlot, Base + PoisonTermResumeRva);
    WriteQword(page + SummationStubOffset + SummationCheckResumeSlot, Base + PoisonTermResumeRva);
    WriteQword(page + SummationStubOffset + SummationCallbackImm64,
        reinterpret_cast<std::uint64_t>(&PoisonHitpointTerm));

    DWORD previous = 0;
    if (!VirtualProtect(page, HookPageBytes, PAGE_EXECUTE_READ, &previous)) {
        Context->LogError("PoisonImmunity: hook page protection could not be finalized.");
        VirtualFree(HookPage, 0, MEM_RELEASE);
        HookPage = nullptr;
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), page, HookPageBytes);

    OriginalApplyPoison = reinterpret_cast<ApplyPoisonFn>(PageAddress(ApplierOriginalStubOffset));

    if ((Settings.blockState
            && !CanEncodeRel32(Base + ApplyPoisonRva, PageAddress(ApplierEntryStubOffset)))
        || (Settings.excludeFromHit
            && !CanEncodeRel32(Base + PoisonTermRva, PageAddress(SummationStubOffset)))) {
        Context->LogError("PoisonImmunity: hook page displacement validation failed.");
        VirtualFree(HookPage, 0, MEM_RELEASE);
        HookPage = nullptr;
        return false;
    }
    return true;
}

// Returns false only when nothing in the image can reach this DLL any more,
// so the loader may safely unload it.
auto InstallHooks() noexcept -> bool {
    if (!BuildHookPage()) return false;

    std::uint8_t bytes[16]{};
    bool failed = false;

    if (Settings.blockState) {
        EncodeSiteJump(Base + ApplyPoisonRva, PageAddress(ApplierEntryStubOffset), bytes,
            ApplyPoisonHookSize);
        if (Context->PatchBytes(ApplyPoisonRva, ApplyPoisonEntryWitness, ApplyPoisonHookSize,
                bytes, ApplyPoisonHookSize)) {
            ApplyPoisonPatched = true;
        } else {
            Context->LogError("PoisonImmunity: the poison applier entry could not be redirected.");
            failed = true;
        }
    }

    if (!failed && Settings.excludeFromHit) {
        EncodeSiteJump(Base + PoisonTermRva, PageAddress(SummationStubOffset), bytes,
            PoisonTermHookSize);
        if (Context->PatchBytes(PoisonTermRva, HitpointTotalWitness + PoisonTermOffsetInWitness,
                PoisonTermHookSize, bytes, PoisonTermHookSize)) {
            PoisonTermPatched = true;
        } else {
            Context->LogError("PoisonImmunity: the life damage total could not be redirected.");
            failed = true;
        }
    }

    if (!failed) {
        State = HookState::Armed;
        return true;
    }

    // The page is kept on every failure path: a thread may already be inside
    // a stub, and after the retarget nothing in it points into this DLL.
    const bool retargeted = RetargetStubsToVanilla();
    const bool restored = RestoreSites();
    if (restored && retargeted) return false;
    if (retargeted) {
        Context->LogError(
            "PoisonImmunity: a site could not be restored; it now runs native code "
            "through the hook page, without immunity.");
        return false;
    }
    Context->LogError(
        "PoisonImmunity: rollback failed, staying loaded so the patched sites keep "
        "a valid target. Immunity is only partially applied.");
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
        "Poison Immunity: %s | state %d%s | block poison state %s, blocked %llu | "
        "exclude poison from hit damage %s, excluded %llu",
        StateName(), Settings.stateId,
        StateIdPastCount.load(std::memory_order_relaxed) ? " (past the states.txt count!)" : "",
        ApplyPoisonPatched ? "on" : "off",
        static_cast<unsigned long long>(BlockedStates.load(std::memory_order_relaxed)),
        PoisonTermPatched ? "on" : "off",
        static_cast<unsigned long long>(ExcludedHits.load(std::memory_order_relaxed)));
    command->plugin->WriteConsoleMessage(message);
    return D2RL::ConsoleCommandResult::Handled;
}

void RegisterStatusCommand() noexcept {
    if (!Context->RegisterConsoleCommand("poisonimmunity", &StatusCommand,
            "Reports Poison Immunity status and counters.")) {
        Context->LogWarn("PoisonImmunity: the status console command was refused.");
    }
}

constexpr D2RL::PluginInfo Info{
    .infoSize = D2RL::PluginInfoSize,
    .abiVersion = D2RL_PLUGIN_ABI_VERSION,
    .id = "celestialrayone.poison-immunity",
    .name = "Poison Immunity",
    .version = "1.0.0",
    .author = "CelestialRayOne",
    .description =
        "Units with a configurable state are immune to poison: no poisoned state, "
        "and no poison damage from the hit that applies it.",
    .flags = D2RL::PluginFlags::Server | D2RL::PluginFlags::NativeHooks,
};

}  // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
    return &Info;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
    if (!D2RL::HasContext(context)) return false;
    Context = context;
    Base = context->exeBase;
    CheckState = reinterpret_cast<CheckStateFn>(Base + CheckStateRva);
    GetDataContext = reinterpret_cast<GetDataContextFn>(Base + GetDataContextRva);
    GetDataTables = reinterpret_cast<GetDataTablesFn>(Base + GetDataTablesRva);

    ReadConfiguration();

    if (!Settings.enabled || (!Settings.blockState && !Settings.excludeFromHit)) {
        State = HookState::DisabledByConfig;
        Context->LogInfo(Settings.enabled
            ? "PoisonImmunity: block_poison_state and exclude_poison_from_hit_damage are "
              "both off; no hooks installed."
            : "PoisonImmunity: disabled by configuration.");
        RegisterStatusCommand();
        return true;
    }

    if (!VerifyNativeContract()) return false;
    if (!InstallHooks()) return false;

    D2RL::LogInfoF(Context,
        "PoisonImmunity: %s. state %d, block poison state %s, exclude poison from hit damage %s.",
        StateName(), Settings.stateId, ApplyPoisonPatched ? "on" : "off",
        PoisonTermPatched ? "on" : "off");

    RegisterStatusCommand();
    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    if (Context == nullptr || HookPage == nullptr) return;
    RetargetStubsToVanilla();
    RestoreSites();
    // The hook page is deliberately kept: a thread may be inside a stub right
    // now, and any site that could not be restored still needs it.
}

}  // namespace CelestialRayOne::PoisonImmunity
