// Frenzy Speed
//
// A stat makes Frenzy faster: every point takes one frame off each of
// Frenzy's swings. Frenzy and nothing else.
//
// Everything below was read out of the D2RLoader 1.3.1 process image. Every
// site, offset and register used here is checked byte for byte at load, and
// nothing is installed when a check fails.
//
// ---------------------------------------------------------------------------
// 1. How Frenzy is timed
// ---------------------------------------------------------------------------
//   Eastern Sun's skills.txt Frenzy row (574) is anim SQ, seqnum 11,
//   UseAttackRate 1. A player using it sits in mode 18 (SQ) and plays
//   sequence 11, compiled into the exe (resolver 3CB890, table 2386650): the
//   same 17 rows for all 14 weapon classes, 6 bytes each
//   {word, mode, frame, dir, event}:
//     steps 0..8    mode 7 (A1)   frames 1..14, event 1 on step 4 (main hand swing)
//     steps 9..16   mode 15 (S3)  frames 2..11, event 1 on step 9 (off hand swing)
//   The mode start 34C880 calls the sequence installer 34D900 and then the
//   rate update 350B40 (34CB42, 34CB53). The installer leaves:
//     unit+40h  rows        3CB850 (slot +0)
//     unit+48h  length      3CB830 (slot +8) << 8, 17 << 8 for Frenzy
//     unit+4Ch  position    0
//     unit+54h  rate        256, until 350B40 sets it
//     unit+64h  countdown   3CB870 (slot +0Ch) << 8, also 17 << 8
//   Every server frame 3476B0 runs with a multiplier of 1.0 (4459BA loads
//   1.0f from 1CBA904) and, for a sequence (3476F1..347767):
//     position  = (position + rate) mod length
//     countdown = countdown - rate, end flag (unit+128h bit 27) at <= 0
//   So a sequence lasts ceil(length / rate) frames: ceil(4352 / rate) for
//   Frenzy, both swings together.
//
// ---------------------------------------------------------------------------
// 2. Hits
// ---------------------------------------------------------------------------
//   After every advance 351970 asks 3CB6C0 for the frame's event: with the
//   position unchanged it is the current step's own event, otherwise the LAST
//   non-zero event among the steps entered (old step + 1 .. new step). A frame
//   that wraps past the end enters no step at all. So two hits in one frame
//   count once, and a hit skipped by the final, wrapping frame is lost. For
//   Frenzy both hits need at least 3 frames: at 2 the second hit is lost, at
//   1 both are. The plugin plays each length it considers frame by frame
//   (EveryHitLands) and never goes below the shortest one at which every hit
//   of the sequence lands.
//
// ---------------------------------------------------------------------------
// 3. Where the speed is set
// ---------------------------------------------------------------------------
//   UNITS_UpdateAnimRateAndVelocity 350B40 keeps the unit in rsi throughout.
//   Mode 18 with skills.txt UseAttackRate takes the attack path (IAS, weapon
//   speed, dual wield averaging, the -30 sequence penalty, the 15..175 clamp)
//   and the cast and attack paths both end at:
//     350C69  movsx eax, bx
//     350C6C  mov [rsi+54h], eax       ; the rate a sequence advances by
//     350C6F  mov [rsi+68h], bx        ; the same rate as a word
//     350C73  call 362360              ; every path reaches this call
//   The call at 350C73 goes to a relay that hands the unit to the hook and
//   then continues to 362360 (a getter without arguments) exactly as before.
//   The hook acts only when the dword at unit+54h equals the word at
//   unit+68h, meaning the rate came through that store. Paths that write the
//   word alone (block, get hit, movement, fixed speeds) are never touched.
//   Other plugins: crossbow-charges redirects 351597 and checks 351102..351151,
//   351482..35149D and 351597..3515A3; faster-cast-rate-cap patches 350C29 and
//   checks 350C14..350C72; whirlwind-follow-cursor checks 350B40..350B6C and
//   350FDC..3510B8. 350C73..350C77 is in none of them.
//
// ---------------------------------------------------------------------------
// 4. The rule
// ---------------------------------------------------------------------------
//   For a player in mode 18 with a sequence installed whose used skill
//   ([[unit+100h]+18h], record, first word: 34BA40 -> 33E090 -> 33E080) is
//   frenzy_skill_id:
//     game frames = ceil(length / game rate)
//     swings      = steps carrying an event (2 for Frenzy)
//     wanted      = game frames - stat * swings, at least 1
//     frames      = the shortest length from wanted up at which every hit
//                   lands, and only if that is faster than the game
//     rate        = the slowest rate that finishes within frames
//   A negative stat adds frames the same way, taking the fastest rate that
//   needs at least that many. The stat goes through the game's own reader
//   (2F5020, D2RCore's wide stat read), so ids above 511 work. 350B40 is shared
//   code, so the server (timing, hits) and every client (animation) apply the
//   same rule to the same numbers.

#include <D2RLPlugin/api.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>

namespace {

constexpr char        PluginIdText[] = "celestialrayone.frenzy-speed";
constexpr const char* PluginVersion  = "1.0.0";

const D2RL::PluginContext* Context{};
std::uintptr_t             Base{};

// ---------------------------------------------------------------------------
// Addresses (RVAs into the D2RLoader 1.3.1 image)
// ---------------------------------------------------------------------------

// Redirected call
constexpr std::uint64_t RateStoreWindowRva     = 0x350C59;
constexpr std::uint64_t RateStoredCallRva      = 0x350C73;
constexpr std::uint64_t UnitUpdateGetterRva    = 0x362360;
// Read only: the timing model the frame math depends on
constexpr std::uint64_t ModeStartWindowRva     = 0x34CB42;
constexpr std::uint64_t SequenceInstallRva     = 0x34D927;
constexpr std::uint64_t SequenceSlotReadersRva = 0x3CB830;
constexpr std::uint64_t SequenceAdvanceRva     = 0x3476F1;
constexpr std::uint64_t ServerAdvanceCallRva   = 0x4459BA;
constexpr std::uint64_t ServerAdvanceScaleRva  = 0x1CBA904;
constexpr std::uint64_t StepEventRva           = 0x351970;
constexpr std::uint64_t StepScanRva            = 0x3CB6C0;
constexpr std::uint64_t UsedSkillRva           = 0x34BA61;
constexpr std::uint64_t SkillIdRva             = 0x33E080;
// Called
constexpr std::uint64_t GetUnitStatRva         = 0x2F5020;  // loader thunk to D2RCore's wide stat read
// Read only, debug log labels
constexpr std::uint64_t ServerUnitAllocRva     = 0x4906A4;
constexpr std::uint64_t UnitFlagSetterRva      = 0x34E161;

// Unit layout
constexpr std::size_t   UnitTypeOffset              = 0x00;
constexpr std::size_t   UnitModeOffset              = 0x0C;
constexpr std::size_t   UnitSequenceRowsOffset      = 0x40;
constexpr std::size_t   UnitSequenceLengthOffset    = 0x48;
constexpr std::size_t   UnitSequencePositionOffset  = 0x4C;
constexpr std::size_t   UnitRateOffset              = 0x54;
constexpr std::size_t   UnitSequenceCountdownOffset = 0x64;
constexpr std::size_t   UnitRateWordOffset          = 0x68;
constexpr std::size_t   UnitSkillListOffset         = 0x100;
constexpr std::size_t   UnitFlagsExOffset           = 0x128;
constexpr std::size_t   SkillListUsedOffset         = 0x18;
constexpr std::size_t   SequenceRowSize             = 6;
constexpr std::size_t   SequenceRowEventOffset      = 5;
constexpr std::int32_t  PlayerUnitType              = 0;
constexpr std::int32_t  SequenceMode                = 18;
constexpr std::uint32_t ServerUnitFlag              = 0x04000000;  // set by the server unit allocator only

constexpr std::size_t   CallSize = 5;

// ---------------------------------------------------------------------------
// Byte witnesses
// ---------------------------------------------------------------------------

// 350C59..350C8B: the end of the cast and attack paths (rsi = unit). xor r14d,r14d /
// clamp 0..7FFFh / movsx eax,bx / mov [rsi+54h],eax / mov [rsi+68h],bx / call 362360
// (the redirected call, offset 1Ah) / mov [rbp+18h],rsi / call 980E0.
constexpr std::uint8_t RateStoreWindow[]{
    0x45,0x33,0xF6,0x85,0xFF,0x44,0x0F,0x4F,0xF7,0x44,0x3B,0xF3,0x41,0x0F,0x4C,0xDE,
    0x0F,0xBF,0xC3,0x89,0x46,0x54,0x66,0x89,0x5E,0x68,0xE8,0xE8,0x16,0x01,0x00,0x48,
    0x8D,0x55,0x18,0x48,0x89,0x75,0x18,0x48,0x8D,0x88,0x68,0x01,0x00,0x00,0xE8,0x54,
    0x74,0xD4,0xFF,
};

// 362360: push rbx / sub rsp,20h / mov rax,gs:[58h] / mov ecx,[tls index] / mov edx,126Ch.
// A getter without arguments; the relay jumps here with the stack it came in with.
constexpr std::uint8_t UnitUpdateGetterBytes[]{
    0x40,0x53,0x48,0x83,0xEC,0x20,0x65,0x48,0x8B,0x04,0x25,0x58,0x00,0x00,0x00,0x8B,
    0x0D,0x53,0x21,0x25,0x03,
};

// 34CB42..34CB58 in the mode start 34C880: call 34D900 (sequence install), then
// call 350B40 (rate update) at +11h.
constexpr std::uint8_t ModeStartWindow[]{
    0xE8,0xB9,0x0D,0x00,0x00,0x41,0xB8,0x9E,0x09,0x00,0x00,0x49,0x8B,0xD7,0x48,0x8B,
    0xCF,0xE8,0xE8,0x3F,0x00,0x00,
};

// 34D927..34D989 in the installer 34D900 (rbx = unit): rows = 3CB850 -> [rbx+40h];
// [rbx+48h] = 3CB830 (steps << 8); [rbx+54h] = 100h; qword [rbx+4Ch] = 0;
// [rbx+64h] = 3CB870 << 8; first step through 351970 (position 0 to 0).
constexpr std::uint8_t SequenceInstallBytes[]{
    0xE8,0x24,0xDF,0x07,0x00,0x48,0x89,0x43,0x40,0x48,0x85,0xC0,0x75,0x13,0x48,0x8D,
    0x4C,0x24,0x30,0x88,0x44,0x24,0x30,0xE8,0xBD,0x86,0xFF,0xFF,0x84,0xC0,0x74,0x01,
    0xCC,0x48,0x83,0x7B,0x40,0x00,0x74,0x3A,0x48,0x8B,0xCB,0xE8,0xD9,0xDE,0x07,0x00,
    0x89,0x43,0x48,0x48,0x8B,0xCB,0x33,0xC0,0xC7,0x43,0x54,0x00,0x01,0x00,0x00,0x48,
    0x89,0x43,0x4C,0xE8,0x01,0xDF,0x07,0x00,0xC1,0xE0,0x08,0x33,0xD2,0x48,0x8B,0xCB,
    0x89,0x43,0x64,0xE8,0xF1,0x3F,0x00,0x00,0x81,0x8B,0x24,0x01,0x00,0x00,0x00,0x40,
    0x00,0x00,
};

// 3CB830..3CB88B: 3CB830 = slot +8 << 8 (length), 3CB850 = slot +0 (rows),
// 3CB870 = slot +0Ch (countdown steps), each through the resolver 3CB890.
constexpr std::uint8_t SequenceSlotReadersBytes[]{
    0x48,0x83,0xEC,0x28,0xE8,0x57,0x00,0x00,0x00,0x48,0x85,0xC0,0x75,0x05,0x48,0x83,
    0xC4,0x28,0xC3,0x8B,0x40,0x08,0xC1,0xE0,0x08,0x48,0x83,0xC4,0x28,0xC3,0xCC,0xCC,
    0x48,0x83,0xEC,0x28,0xE8,0x37,0x00,0x00,0x00,0x48,0x85,0xC0,0x75,0x05,0x48,0x83,
    0xC4,0x28,0xC3,0x48,0x8B,0x00,0x48,0x83,0xC4,0x28,0xC3,0xCC,0xCC,0xCC,0xCC,0xCC,
    0x48,0x83,0xEC,0x28,0xE8,0x17,0x00,0x00,0x00,0x48,0x85,0xC0,0x75,0x05,0x48,0x83,
    0xC4,0x28,0xC3,0x8B,0x40,0x0C,0x48,0x83,0xC4,0x28,0xC3,
};

// 3476F1..347767 in the per-frame advance 3476B0 (rbx = unit, xmm6 = multiplier):
// advance = trunc([rbx+54h] * xmm6); position [rbx+4Ch] += advance, mod [rbx+48h];
// [rbx+64h] -= advance; 351970(unit, old position); end flag [rbx+128h] bit 1Bh
// set once [rbx+64h] <= 0.
constexpr std::uint8_t SequenceAdvanceBytes[]{
    0x48,0x83,0x7B,0x40,0x00,0xC6,0x43,0x6A,0x00,0x74,0x76,0x8B,0x4B,0x4C,0x66,0x0F,
    0x6E,0x43,0x54,0x81,0xA3,0x24,0x01,0x00,0x00,0xFF,0xBF,0xFF,0xFF,0x44,0x8B,0x4B,
    0x48,0x0F,0x5B,0xC0,0x89,0x4B,0x50,0xF3,0x0F,0x59,0xC6,0xF3,0x4C,0x0F,0x2C,0xC0,
    0x41,0x8D,0x04,0x08,0x89,0x43,0x4C,0x41,0x3B,0xC1,0x7C,0x07,0x99,0x41,0xF7,0xF9,
    0x89,0x53,0x4C,0x44,0x29,0x43,0x64,0x8B,0xD1,0x48,0x8B,0xCB,0xE8,0x2E,0xA2,0x00,
    0x00,0x85,0xC0,0x74,0x0A,0x81,0x8B,0x24,0x01,0x00,0x00,0x00,0x40,0x00,0x00,0x8B,
    0x8B,0x28,0x01,0x00,0x00,0x8B,0xC1,0x0F,0xBA,0xF0,0x1B,0x0F,0xBA,0xE9,0x1B,0x83,
    0x7B,0x64,0x00,0x0F,0x4F,0xC8,
};

// 4459BA..4459CA in the server unit update 445970: movss xmm1,[1CBA904] /
// mov rcx,rbx / call 3476B0.
constexpr std::uint8_t ServerAdvanceCallBytes[]{
    0xF3,0x0F,0x10,0x0D,0x42,0x4F,0x87,0x01,0x48,0x8B,0xCB,0xE8,0xE6,0x1C,0xF0,0xFF,
};

// 1CBA904: 1.0f, the server's advance multiplier.
constexpr std::uint8_t ServerAdvanceScaleBytes[]{
    0x00,0x00,0x80,0x3F,
};

// 351970..3519D0: 3CB6C0(rows [rcx+40h], new position [rcx+4Ch], old position edx,
// &mode, &frame, &dir, &event), then the event byte goes to [unit+6Ah].
constexpr std::uint8_t StepEventBytes[]{
    0x4C,0x8B,0xDC,0x49,0x89,0x5B,0x10,0x57,0x48,0x83,0xEC,0x50,0x8B,0x59,0x58,0x49,
    0x8D,0x43,0x20,0x49,0x89,0x43,0xD8,0x4D,0x8D,0x4B,0x08,0x49,0x8D,0x43,0xE8,0x44,
    0x8B,0xC2,0x8B,0x51,0x4C,0x48,0x8B,0xF9,0x48,0x8B,0x49,0x40,0x49,0x89,0x43,0xD0,
    0x49,0x8D,0x43,0x18,0x49,0x89,0x43,0xC8,0xE8,0x13,0x9D,0x07,0x00,0x8B,0x47,0x5C,
    0x8B,0x4C,0x24,0x60,0x89,0x47,0x60,0x8B,0x44,0x24,0x70,0xC1,0xE0,0x08,0x89,0x47,
    0x5C,0x0F,0xB6,0x44,0x24,0x78,0x88,0x47,0x6A,0x33,0xC0,0x3B,0xCB,0x89,0x4F,0x58,
};

// 3CB6C0..3CB7B4: rows of 6 bytes; mode +2, frame +3, dir +4 of the new step; event:
// the new step's own +5 when new == old, else the last non-zero +5 of steps
// old/256 + 1 .. new/256 (none when the position wrapped).
constexpr std::uint8_t StepScanBytes[]{
    0x40,0x56,0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x83,0xEC,0x28,0x4D,0x8B,0xF1,0x45,
    0x8B,0xF8,0x44,0x8B,0xE2,0x48,0x8B,0xF1,0x48,0x85,0xC9,0x75,0x27,0x48,0x8B,0x44,
    0x24,0x70,0x41,0x89,0x09,0x89,0x08,0x48,0x8B,0x44,0x24,0x78,0x89,0x08,0x48,0x8B,
    0x84,0x24,0x80,0x00,0x00,0x00,0x89,0x08,0x48,0x83,0xC4,0x28,0x41,0x5F,0x41,0x5E,
    0x41,0x5C,0x5E,0xC3,0x48,0x89,0x5C,0x24,0x58,0x41,0x8B,0xDC,0x48,0x89,0x6C,0x24,
    0x60,0x41,0x8B,0xEF,0xC1,0xFB,0x08,0x48,0x63,0xC3,0x48,0x89,0x7C,0x24,0x20,0xC1,
    0xFD,0x08,0x48,0x8D,0x0C,0x40,0x48,0x8D,0x3C,0x4E,0x48,0x85,0xFF,0x75,0x14,0x48,
    0x8D,0x4C,0x24,0x50,0x40,0x88,0x7C,0x24,0x50,0xE8,0x32,0xFF,0xFF,0xFF,0x84,0xC0,
    0x74,0x01,0xCC,0x0F,0xB6,0x47,0x02,0x41,0x89,0x06,0x0F,0xB6,0x4F,0x03,0x48,0x8B,
    0x44,0x24,0x70,0x89,0x08,0x48,0x8B,0x44,0x24,0x78,0x0F,0xB6,0x4F,0x04,0x89,0x08,
    0x45,0x3B,0xE7,0x75,0x10,0x48,0x8B,0x84,0x24,0x80,0x00,0x00,0x00,0x0F,0xB6,0x4F,
    0x05,0x89,0x08,0xEB,0x3F,0x4C,0x8B,0x84,0x24,0x80,0x00,0x00,0x00,0x8D,0x55,0x01,
    0x33,0xC9,0x48,0x63,0xC5,0x48,0xFF,0xC0,0x41,0x89,0x08,0x48,0x8D,0x04,0x40,0x3B,
    0xD3,0x7F,0x21,0x2B,0xDA,0x48,0x8D,0x4E,0x05,0x48,0x8D,0x0C,0x41,0x8D,0x53,0x01,
    0x0F,0xB6,0x01,0x84,0xC0,0x74,0x03,0x41,0x89,0x00,0x48,0x83,0xC1,0x06,0x48,0x83,
    0xEA,0x01,0x75,0xEC,
};

// 34BA61..34BA72: mov rcx,[rbx+100h] / add rsp,20h / pop rbx / jmp 33E090.
constexpr std::uint8_t UsedSkillBytes[]{
    0x48,0x8B,0x8B,0x00,0x01,0x00,0x00,0x48,0x83,0xC4,0x20,0x5B,0xE9,0x1E,0x26,0xFF,
    0xFF,
};

// 33E080..33E09D: skill id = movsx word [[skill]]; then 33E090: used skill = [list+18h].
constexpr std::uint8_t SkillIdBytes[]{
    0x48,0x85,0xC9,0x75,0x03,0x33,0xC0,0xC3,0x48,0x8B,0x01,0x0F,0xBF,0x00,0xC3,0xCC,
    0x48,0x85,0xC9,0x75,0x03,0x33,0xC0,0xC3,0x48,0x8B,0x41,0x18,0xC3,
};

// jmp [rip+disp32], the loader's thunk.
constexpr std::uint8_t LoaderThunkBytes[]{
    0xFF,0x25,
};

// 4906A4..4906D5 in the server unit allocator 4905A0: 34E140(unit, 4000000h, 1). Only
// server units carry that flag.
constexpr std::uint8_t ServerUnitAllocBytes[]{
    0x44,0x89,0x30,0x48,0x8B,0xCF,0x44,0x89,0x68,0x04,0x4C,0x89,0xB8,0xD8,0x00,0x00,
    0x00,0x41,0x0F,0xB6,0x97,0x06,0x01,0x00,0x00,0xE8,0x1E,0xDB,0xEB,0xFF,0xBA,0x00,
    0x00,0x00,0x04,0x41,0xB8,0x01,0x00,0x00,0x00,0x48,0x8B,0xCF,0xE8,0x6B,0xDA,0xEB,
    0xFF,
};

// 34E161..34E174 in 34E140 (unit, flag, set): the flag word is unit+128h.
constexpr std::uint8_t UnitFlagSetterBytes[]{
    0x8B,0x81,0x28,0x01,0x00,0x00,0x45,0x85,0xC0,0x74,0x0D,0x0B,0xC2,0x89,0x81,0x28,
    0x01,0x00,0x00,
};

constexpr auto CallTargetRva(const std::uint8_t* bytes, std::uint64_t windowRva, std::size_t offset) noexcept
        -> std::uint64_t {
    const auto displacement = static_cast<std::int32_t>(static_cast<std::uint32_t>(bytes[offset + 1])
        | static_cast<std::uint32_t>(bytes[offset + 2]) << 8 | static_cast<std::uint32_t>(bytes[offset + 3]) << 16
        | static_cast<std::uint32_t>(bytes[offset + 4]) << 24);
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(windowRva + offset + 5) + displacement);
}

constexpr auto IsCallTo(const std::uint8_t* bytes, std::uint64_t windowRva, std::size_t offset,
        std::uint64_t target) noexcept -> bool {
    return bytes[offset] == 0xE8 && CallTargetRva(bytes, windowRva, offset) == target;
}

static_assert(IsCallTo(RateStoreWindow, 0x350C59, 0x1A, 0x362360), "The redirected call is not call 362360.");
static_assert(RateStoreWindow[0x13] == 0x89 && RateStoreWindow[0x14] == 0x46 && RateStoreWindow[0x15] == 0x54,
    "mov [rsi+54h],eax is not where the rate is stored.");
static_assert(RateStoreWindow[0x16] == 0x66 && RateStoreWindow[0x18] == 0x5E && RateStoreWindow[0x19] == 0x68,
    "mov [rsi+68h],bx is not where the rate word is stored.");
static_assert(IsCallTo(ModeStartWindow, 0x34CB42, 0x00, 0x34D900) && IsCallTo(ModeStartWindow, 0x34CB42, 0x11, 0x350B40),
    "The mode start does not install the sequence before the rate update.");
static_assert(IsCallTo(SequenceInstallBytes, 0x34D927, 0x00, 0x3CB850) && IsCallTo(SequenceInstallBytes, 0x34D927, 0x2B, 0x3CB830)
    && IsCallTo(SequenceInstallBytes, 0x34D927, 0x43, 0x3CB870) && IsCallTo(SequenceInstallBytes, 0x34D927, 0x53, 0x351970),
    "The sequence installer calls are not the verified ones.");
static_assert(IsCallTo(SequenceSlotReadersBytes, 0x3CB830, 0x04, 0x3CB890) && IsCallTo(SequenceSlotReadersBytes, 0x3CB830, 0x24, 0x3CB890)
    && IsCallTo(SequenceSlotReadersBytes, 0x3CB830, 0x44, 0x3CB890), "The slot readers do not use the resolver 3CB890.");
static_assert(IsCallTo(SequenceAdvanceBytes, 0x3476F1, 0x4C, 0x351970), "The advance does not ask 351970 for the event.");
static_assert(IsCallTo(ServerAdvanceCallBytes, 0x4459BA, 0x0B, 0x3476B0), "The server does not advance through 3476B0.");
static_assert(IsCallTo(StepEventBytes, 0x351970, 0x38, 0x3CB6C0), "351970 does not scan through 3CB6C0.");
static_assert(UsedSkillBytes[12] == 0xE9 && CallTargetRva(UsedSkillBytes, 0x34BA61, 12) == 0x33E090,
    "The used skill getter does not continue at 33E090.");


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

const Witness Witnesses[]{
    W(RateStoreWindowRva, RateStoreWindow, "rate store and the redirected call"),
    W(UnitUpdateGetterRva, UnitUpdateGetterBytes, "getter behind the redirected call"),
    W(ModeStartWindowRva, ModeStartWindow, "mode start: sequence install, then rate update"),
    W(SequenceInstallRva, SequenceInstallBytes, "sequence install"),
    W(SequenceSlotReadersRva, SequenceSlotReadersBytes, "sequence length, rows and countdown"),
    W(SequenceAdvanceRva, SequenceAdvanceBytes, "sequence advance"),
    W(ServerAdvanceCallRva, ServerAdvanceCallBytes, "server frame advance call"),
    W(ServerAdvanceScaleRva, ServerAdvanceScaleBytes, "server frame advance multiplier"),
    W(StepEventRva, StepEventBytes, "sequence frame event"),
    W(StepScanRva, StepScanBytes, "sequence event scan"),
    W(UsedSkillRva, UsedSkillBytes, "used skill"),
    W(SkillIdRva, SkillIdBytes, "skill id and used skill from list"),
    W(GetUnitStatRva, LoaderThunkBytes, "stat read"),
};

const Witness LabelWitnesses[]{
    W(ServerUnitAllocRva, ServerUnitAllocBytes, "server unit flag"),
    W(UnitFlagSetterRva, UnitFlagSetterBytes, "unit flag word"),
};

// ---------------------------------------------------------------------------
// Native functions
// ---------------------------------------------------------------------------

using GetStatFn = std::int32_t(__fastcall*)(void* unit, std::int32_t stat, std::int32_t layer);

GetStatFn GetStat{};

// ===== sequence math: begin =====
// Pure functions of the timing model in sections 1 and 2 above, nothing else.

constexpr std::int32_t MaxSequenceSteps = 255;
constexpr std::int32_t StepLength       = 256;
constexpr std::int32_t MaxRate          = 0x7FFF;  // the clamp at 350C5C..350C65

// Frames a sequence of `length` takes at `rate`: the countdown starts at
// `length` and loses `rate` every frame until it is <= 0.
constexpr auto SequenceFrames(std::int32_t length, std::int32_t rate) noexcept -> std::int64_t {
    return rate > 0 ? (static_cast<std::int64_t>(length) + rate - 1) / rate : 0;
}

constexpr auto ClampRate(std::int64_t rate) noexcept -> std::int32_t {
    return static_cast<std::int32_t>(rate < 1 ? 1 : (rate > MaxRate ? MaxRate : rate));
}

// The slowest rate that finishes `length` within `frames` frames.
constexpr auto RateForAtMost(std::int32_t length, std::int64_t frames) noexcept -> std::int32_t {
    if (frames < 1) frames = 1;
    return ClampRate((static_cast<std::int64_t>(length) + frames - 1) / frames);
}

// The fastest rate that needs at least `frames` frames for `length`.
constexpr auto RateForAtLeast(std::int32_t length, std::int64_t frames) noexcept -> std::int32_t {
    if (frames <= 1) return MaxRate;
    return ClampRate((static_cast<std::int64_t>(length) - 1) / (frames - 1));
}

// Plays the sequence frame by frame the way the server does (section 2) and
// tells whether every step carrying an event has that event reported.
inline auto EveryHitLands(const std::uint8_t* events, std::int32_t steps, std::int32_t rate) noexcept -> bool {
    if (steps <= 0 || steps > MaxSequenceSteps || rate <= 0) return false;
    const std::int32_t length = steps * StepLength;
    bool reported[MaxSequenceSteps]{};
    if (events[0] != 0) reported[0] = true;  // the installer's first step: position 0 to 0
    std::int32_t position  = 0;
    std::int64_t countdown = length;
    while (countdown > 0) {
        const std::int32_t old   = position;
        const std::int64_t moved = static_cast<std::int64_t>(old) + rate;
        position  = static_cast<std::int32_t>(moved >= length ? moved % length : moved);
        countdown -= rate;
        if (position == old) {
            if (events[position / StepLength] != 0) reported[position / StepLength] = true;
            continue;
        }
        std::int32_t last = -1;
        for (std::int32_t step = old / StepLength + 1; step <= position / StepLength; ++step) {
            if (events[step] != 0) last = step;
        }
        if (last >= 0) reported[last] = true;
    }
    for (std::int32_t step = 0; step < steps; ++step) {
        if (events[step] != 0 && !reported[step]) return false;
    }
    return true;
}

struct RatePlan {
    std::int32_t rate{};        // rate to store
    std::int32_t swings{};      // steps carrying an event
    std::int64_t gameFrames{};  // frames at the game's own rate
    std::int64_t wanted{};      // frames the stat asks for
    std::int64_t frames{};      // frames at `rate`
    bool         changed{};     // the rate differs from the game's
    bool         heldBack{};    // stopped short of `wanted` so every hit lands
};

inline auto PlanRate(const std::uint8_t* events, std::int32_t steps, std::int32_t gameRate, std::int32_t points) noexcept
        -> RatePlan {
    RatePlan plan{};
    plan.rate = gameRate;
    if (steps <= 0 || steps > MaxSequenceSteps || gameRate <= 0) return plan;
    const std::int32_t length = steps * StepLength;
    for (std::int32_t step = 0; step < steps; ++step) {
        if (events[step] != 0) ++plan.swings;
    }
    const std::int64_t swings = plan.swings > 0 ? plan.swings : 1;
    plan.gameFrames = SequenceFrames(length, gameRate);
    plan.frames     = plan.gameFrames;
    plan.wanted     = plan.gameFrames - static_cast<std::int64_t>(points) * swings;
    if (plan.wanted < 1) plan.wanted = 1;
    if (points > 0) {
        for (std::int64_t frames = plan.wanted; frames < plan.gameFrames; ++frames) {
            const std::int32_t rate = RateForAtMost(length, frames);
            if (rate <= gameRate) break;
            if (!EveryHitLands(events, steps, rate)) continue;
            plan.rate     = rate;
            plan.frames   = SequenceFrames(length, rate);
            plan.changed  = true;
            plan.heldBack = plan.frames > plan.wanted;
            return plan;
        }
        plan.heldBack = true;  // even one frame faster than the game would lose a hit
        return plan;
    }
    if (points < 0) {
        // A rate of 256 or less enters every step on its own frame, so this
        // always ends by the time frames reaches the step count.
        for (std::int64_t frames = plan.wanted; frames <= plan.wanted + length; ++frames) {
            const std::int32_t rate = RateForAtLeast(length, frames);
            if (rate >= gameRate) continue;
            if (!EveryHitLands(events, steps, rate)) continue;
            plan.rate     = rate;
            plan.frames   = SequenceFrames(length, rate);
            plan.changed  = true;
            plan.heldBack = plan.frames > plan.wanted;
            return plan;
        }
    }
    return plan;
}
// ===== sequence math: end =====

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

enum class PluginState { NotLoaded, DisabledByConfig, UnsupportedBuild, InstallFailed, Active };
PluginState       State{ PluginState::NotLoaded };
std::atomic<bool> Active{};
std::atomic<bool> DebugOn{};
bool              LabelsVerified{};

// Counters, shown by the console command and zeroed by "frenzyspeed reset".
std::atomic<std::uint64_t> FrenzyUpdates{};   // speed updates of a player playing the Frenzy sequence
std::atomic<std::uint64_t> RatesChanged{};
std::atomic<std::uint64_t> RatesHeldBack{};   // changed, but stopped short so every hit lands
std::atomic<std::uint64_t> NoPoints{};        // the stat is 0
std::atomic<std::uint64_t> NothingToGain{};   // the game's speed already as fast as the hits allow
std::atomic<std::uint64_t> NotAttackRate{};   // the rate did not come through the attack/cast store
std::atomic<std::uint64_t> OtherShape{};      // sequence fields outside the model (see the hook)

void ResetCounters() noexcept {
    for (std::atomic<std::uint64_t>* counter :
            { &FrenzyUpdates, &RatesChanged, &RatesHeldBack, &NoPoints, &NothingToGain, &NotAttackRate, &OtherShape }) {
        counter->store(0, std::memory_order_relaxed);
    }
}

struct LastChange {
    bool         valid{};
    bool         server{};
    bool         labelled{};
    std::int32_t points{};
    std::int32_t gameRate{};
    RatePlan     plan{};
};
std::mutex LastMutex;
LastChange Last;

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

constexpr char DefaultConfigToml[] =
    "# Frenzy Speed\n"
    "#\n"
    "# A stat that makes Frenzy faster. Every point takes one frame off each of\n"
    "# Frenzy's two swings. Nothing but Frenzy is affected.\n"
    "#\n"
    "# How the game times Frenzy\n"
    "#   Frenzy (skills.txt: anim SQ, seqnum 11) is not two separate attacks. It is\n"
    "#   one sequence compiled into the game, the same for every weapon pair:\n"
    "#     steps 1 to 9    A1 frames (main hand swing), first hit on step 5\n"
    "#     steps 10 to 17  S3 frames (off hand swing), second hit on step 10\n"
    "#   The whole double swing is timed as one piece:\n"
    "#     frames = ceil(4352 / rate)\n"
    "#   where rate is your normal attack speed (IAS, weapon speed, dual wield\n"
    "#   averaging, the -30 sequence penalty and the 175 cap all still apply).\n"
    "#\n"
    "# What the stat does\n"
    "#   Each point takes one frame off each swing, so two frames off the whole\n"
    "#   double swing. A 14 frame double swing (7 per swing) with 2 points becomes\n"
    "#   10 frames (5 per swing). Negative values add frames instead.\n"
    "#   The speed is set when a Frenzy swing starts, so a change in the stat\n"
    "#   shows from the next swing on.\n"
    "#   Only a player playing the Frenzy sequence (current skill frenzy_skill_id)\n"
    "#   is touched. Every other skill, sequence and animation keeps the game's own\n"
    "#   speed.\n"
    "#\n"
    "# Fastest possible Frenzy\n"
    "#   The game registers at most one hit per frame, and none in the frame that\n"
    "#   ends the sequence. Below 3 frames for the whole double swing the second\n"
    "#   hit would be lost (2 frames: first hit only, 1 frame: no hit at all). The\n"
    "#   plugin never makes Frenzy faster than the shortest length at which both\n"
    "#   hits still land, whatever the stat says.\n"
    "#\n"
    "# Server and client\n"
    "#   The game sets the speed in one function shared by the server (timing and\n"
    "#   hits) and every client (animation), and the plugin works inside it, so\n"
    "#   the two always agree.\n"
    "#\n"
    "# Console command\n"
    "#   frenzyspeed               status, counters and the last change\n"
    "#   frenzyspeed debug on|off  log every Frenzy speed change\n"
    "#   frenzyspeed reset         zero the counters\n"
    "#\n"
    "# Log: d2rloader/logs/celestialrayone.frenzy-speed.log\n"
    "\n"
    "[frenzy_speed]\n"
    "\n"
    "# Master switch.\n"
    "enabled = true\n"
    "\n"
    "# Debug log: one line per Frenzy speed change, at most 60 a second. Also\n"
    "# switchable in game (see above).\n"
    "debug = false\n"
    "\n"
    "# skills.txt row id of Frenzy (574 in Eastern Sun). -1 turns the plugin off.\n"
    "frenzy_skill_id = 574\n"
    "\n"
    "# itemstatcost.txt row id of the stat. Each point takes one frame off each\n"
    "# Frenzy swing.\n"
    "frames_stat_id = 916\n";

constexpr std::size_t MaximumConfigBytes = 16 * 1024;

struct Config {
    bool         enabled{ true };
    bool         debug{ false };
    std::int32_t frenzySkill{ 574 };
    std::int32_t framesStat{ 916 };
};

Config      Settings;
std::string ConfigProblems;

void Problem(std::string_view text) {
    if (!ConfigProblems.empty()) ConfigProblems += "; ";
    ConfigProblems.append(text);
}

auto Trim(std::string_view value) noexcept -> std::string_view {
    std::size_t first = 0;
    while (first < value.size() && (value[first] == ' ' || value[first] == '\t'
            || value[first] == '\r' || value[first] == '\n')) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first && (value[last - 1] == ' ' || value[last - 1] == '\t'
            || value[last - 1] == '\r' || value[last - 1] == '\n')) {
        --last;
    }
    return value.substr(first, last - first);
}

// Drops a trailing # comment that is not inside a quoted string.
auto StripComment(std::string_view line) noexcept -> std::string_view {
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '"') quoted = !quoted;
        if (line[i] == '#' && !quoted) return line.substr(0, i);
    }
    return line;
}

auto ParseBool(std::string_view value, bool& out) noexcept -> bool {
    if (value == "true") { out = true; return true; }
    if (value == "false") { out = false; return true; }
    return false;
}

auto ParseInt(std::string_view value, std::int32_t minimum, std::int32_t maximum, std::int32_t& out) noexcept -> bool {
    if (value.empty()) return false;
    std::int64_t result   = 0;
    std::size_t  i        = 0;
    bool         negative = false;
    if (value[0] == '-' || value[0] == '+') {
        negative = value[0] == '-';
        i = 1;
    }
    if (i >= value.size()) return false;
    for (; i < value.size(); ++i) {
        if (value[i] == '_') continue;
        if (value[i] < '0' || value[i] > '9') return false;
        result = result * 10 + (value[i] - '0');
        if (result > 1'000'000'000) return false;
    }
    if (negative) result = -result;
    if (result < minimum || result > maximum) return false;
    out = static_cast<std::int32_t>(result);
    return true;
}

void ApplyConfigLine(std::string_view section, std::string_view key, std::string_view value) {
    if (section != "frenzy_speed") return;
    char note[160];
    const auto bad = [&](const char* what) {
        std::snprintf(note, sizeof(note), "%.*s: %s, default kept", static_cast<int>(key.size()), key.data(), what);
        Problem(note);
    };
    if (key == "enabled") {
        if (!ParseBool(value, Settings.enabled)) bad("expected true or false");
    } else if (key == "debug") {
        if (!ParseBool(value, Settings.debug)) bad("expected true or false");
    } else if (key == "frenzy_skill_id") {
        if (!ParseInt(value, -1, 32767, Settings.frenzySkill)) bad("expected -1 to 32767");
    } else if (key == "frames_stat_id") {
        if (!ParseInt(value, 0, 32767, Settings.framesStat)) bad("expected 0 to 32767");
    }
}

void ParseConfig(std::string_view text) {
    std::string_view section;
    while (!text.empty()) {
        const std::size_t end  = text.find('\n');
        std::string_view  line = Trim(StripComment(text.substr(0, end)));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (line.empty()) continue;
        if (line.front() == '[') {
            if (line.back() == ']') section = Trim(line.substr(1, line.size() - 2));
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) continue;
        ApplyConfigLine(section, Trim(line.substr(0, equals)), Trim(line.substr(equals + 1)));
    }
}

void ReadConfiguration() {
    if (!Context->EnsureConfig(DefaultConfigToml)) {
        Context->LogWarn("FrenzySpeed: the config file could not be created; using defaults.");
    } else {
        std::string buffer(MaximumConfigBytes, '\0');
        if (Context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size()), nullptr)) {
            buffer.resize(std::strlen(buffer.c_str()));
            ParseConfig(buffer);
        } else {
            Context->LogWarn("FrenzySpeed: the config file could not be read; using defaults.");
        }
    }
    if (!ConfigProblems.empty()) {
        D2RL::LogWarnF(Context, "FrenzySpeed: %s", ConfigProblems.c_str());
    }
}

// ---------------------------------------------------------------------------
// Memory helpers
// ---------------------------------------------------------------------------

auto Bytes(void* base, std::size_t offset) noexcept -> std::uint8_t* {
    return static_cast<std::uint8_t*>(base) + offset;
}

template <typename T>
auto Read(void* base, std::size_t offset) noexcept -> T {
    T value{};
    std::memcpy(&value, Bytes(base, offset), sizeof(T));
    return value;
}

template <typename T>
void Write(void* base, std::size_t offset, T value) noexcept {
    std::memcpy(Bytes(base, offset), &value, sizeof(T));
}

// The skill the unit is using, read the way 350B40 reads it: [[unit+100h]+18h]
// (34BA40 -> 33E090), then the record's first word as a signed word (33E080).
auto UsedSkillId(void* unit) noexcept -> std::int32_t {
    void* skills = Read<void*>(unit, UnitSkillListOffset);
    if (skills == nullptr) return -1;
    void* skill = Read<void*>(skills, SkillListUsedOffset);
    if (skill == nullptr) return -1;
    void* record = Read<void*>(skill, 0);
    if (record == nullptr) return -1;
    return static_cast<std::int32_t>(Read<std::int16_t>(record, 0));
}

// ---------------------------------------------------------------------------
// Debug log
// ---------------------------------------------------------------------------

constexpr std::uint32_t DebugLinesPerSecond = 60;
std::atomic<std::uint64_t> DebugWindowStart{};
std::atomic<std::uint32_t> DebugLinesInWindow{};
std::atomic<std::uint64_t> DebugLinesDropped{};

auto DebugAllowed() noexcept -> bool {
    const std::uint64_t now   = GetTickCount64();
    std::uint64_t       start = DebugWindowStart.load(std::memory_order_relaxed);
    if (now - start >= 1000 && DebugWindowStart.compare_exchange_strong(start, now)) {
        const std::uint64_t dropped = DebugLinesDropped.exchange(0);
        DebugLinesInWindow.store(0);
        if (dropped != 0) {
            D2RL::LogInfoF(Context, "FrenzySpeed debug: %llu lines dropped in the last second.",
                static_cast<unsigned long long>(dropped));
        }
    }
    if (DebugLinesInWindow.fetch_add(1) < DebugLinesPerSecond) return true;
    DebugLinesDropped.fetch_add(1);
    return false;
}

auto SideOf(void* unit, bool& labelled) noexcept -> bool {
    labelled = LabelsVerified;
    return LabelsVerified && (Read<std::uint32_t>(unit, UnitFlagsExOffset) & ServerUnitFlag) != 0;
}

auto SideName(bool labelled, bool server) noexcept -> const char* {
    return !labelled ? "unit" : (server ? "server" : "client");
}

// ---------------------------------------------------------------------------
// Hook: the rate 350B40 just stored
// ---------------------------------------------------------------------------

// Reached from the relay in place of the call at 350C73, with the unit 350B40
// is updating (rsi). The rate is already stored at unit+54h and unit+68h.
void __fastcall HookedRateStored(void* unit) noexcept {
    if (!Active.load(std::memory_order_relaxed) || unit == nullptr) return;
    if (Read<std::int32_t>(unit, UnitTypeOffset) != PlayerUnitType) return;
    if (Read<std::int32_t>(unit, UnitModeOffset) != SequenceMode) return;
    void* rows = Read<void*>(unit, UnitSequenceRowsOffset);
    if (rows == nullptr) return;
    const std::int32_t frenzy = Settings.frenzySkill;
    if (frenzy < 0 || UsedSkillId(unit) != frenzy) return;
    FrenzyUpdates.fetch_add(1, std::memory_order_relaxed);

    // Only a rate that came through the store at 350C69..350C72 (the attack
    // and cast paths) is in both fields.
    const std::int32_t gameRate = Read<std::int32_t>(unit, UnitRateOffset);
    if (gameRate <= 0 || gameRate != static_cast<std::int32_t>(Read<std::int16_t>(unit, UnitRateWordOffset))) {
        NotAttackRate.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // The model in sections 1 and 2: whole steps, and a countdown that starts
    // at the length (position + countdown stays equal to the length until the
    // sequence ends, both move by the same amount every frame).
    const std::int32_t length = Read<std::int32_t>(unit, UnitSequenceLengthOffset);
    const std::int32_t steps  = length / StepLength;
    if (length <= 0 || length % StepLength != 0 || steps > MaxSequenceSteps
            || static_cast<std::int64_t>(Read<std::int32_t>(unit, UnitSequencePositionOffset))
                   + Read<std::int32_t>(unit, UnitSequenceCountdownOffset) != length) {
        OtherShape.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const std::int32_t points = GetStat(unit, Settings.framesStat, 0);
    if (points == 0) {
        NoPoints.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::uint8_t events[MaxSequenceSteps]{};
    for (std::int32_t step = 0; step < steps; ++step) {
        events[step] = Read<std::uint8_t>(rows, static_cast<std::size_t>(step) * SequenceRowSize + SequenceRowEventOffset);
    }
    const RatePlan plan = PlanRate(events, steps, gameRate, points);
    if (plan.changed) {
        Write<std::int32_t>(unit, UnitRateOffset, plan.rate);
        Write<std::int16_t>(unit, UnitRateWordOffset, static_cast<std::int16_t>(plan.rate));
        RatesChanged.fetch_add(1, std::memory_order_relaxed);
        if (plan.heldBack) RatesHeldBack.fetch_add(1, std::memory_order_relaxed);
    } else {
        NothingToGain.fetch_add(1, std::memory_order_relaxed);
    }

    bool       labelled = false;
    const bool server   = SideOf(unit, labelled);
    if (std::unique_lock lock(LastMutex, std::try_to_lock); lock.owns_lock()) {
        Last = { true, server, labelled, points, gameRate, plan };
    }
    if (DebugOn.load(std::memory_order_relaxed) && DebugAllowed()) {
        D2RL::LogInfoF(Context, "[%s] Frenzy: stat %d x %d swings, game %lld frames (rate %d), asked %lld, %s %lld "
            "frames (rate %d)%s.", SideName(labelled, server), points, plan.swings,
            static_cast<long long>(plan.gameFrames), gameRate, static_cast<long long>(plan.wanted),
            plan.changed ? "now" : "kept", static_cast<long long>(plan.frames), plan.rate,
            plan.heldBack ? ", held back so every hit lands" : "");
    }
}

// ---------------------------------------------------------------------------
// Relay page and the redirected call
// ---------------------------------------------------------------------------

constexpr std::size_t RelayPageBytes  = 4'096;
constexpr std::size_t RateStoredStub  = 0x00;  // save, HookedRateStored(rsi), restore, jmp 362360
constexpr std::size_t HookSlot        = 0x30;
constexpr std::size_t OriginalSlot    = 0x38;

void*        RelayPage{};
std::uint8_t RedirectedCall[CallSize]{};
bool         CallPatched{};

auto CanEncodeRel32(std::uintptr_t from, std::uintptr_t to) noexcept -> bool {
    const std::int64_t delta = static_cast<std::int64_t>(to) - static_cast<std::int64_t>(from) - static_cast<std::int64_t>(CallSize);
    return delta >= std::numeric_limits<std::int32_t>::min() && delta <= std::numeric_limits<std::int32_t>::max();
}

auto AllocateNear(std::uintptr_t site, std::size_t size) noexcept -> void* {
    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    const auto granularity = static_cast<std::uintptr_t>(systemInfo.dwAllocationGranularity);
    const auto aligned     = site & ~(granularity - 1U);
    for (std::uintptr_t delta = granularity; delta < 0x7000'0000ULL; delta += granularity) {
        const auto candidate = aligned + delta;
        if (!CanEncodeRel32(site, candidate + size)) break;
        if (void* memory = VirtualAlloc(reinterpret_cast<void*>(candidate), size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) {
            return memory;
        }
    }
    return nullptr;
}

void EncodeCall(std::uintptr_t site, std::uintptr_t target, std::uint8_t* out) noexcept {
    out[0] = 0xE8;
    const auto displacement = static_cast<std::int32_t>(static_cast<std::int64_t>(target) - static_cast<std::int64_t>(site + CallSize));
    std::memcpy(out + 1, &displacement, sizeof(displacement));
}

// Entered by the call at 350C73, so [rsp] returns to 350C78. Seven pushes plus
// the return address keep rsp 16-byte aligned for the inner call. The site
// held a call, so the game expects every volatile register to be clobbered;
// they are kept anyway, and the stub leaves by jumping to the original target
// with the stack exactly as it came in, so 362360 returns straight to 350C78.
constexpr std::uint8_t RateStoredStubCode[]{
    0x50,                                // push rax
    0x51,                                // push rcx
    0x52,                                // push rdx
    0x41, 0x50,                          // push r8
    0x41, 0x51,                          // push r9
    0x41, 0x52,                          // push r10
    0x41, 0x53,                          // push r11
    0x48, 0x83, 0xEC, 0x20,              // sub  rsp, 20h
    0x48, 0x8B, 0xCE,                    // mov  rcx, rsi        ; the unit
    0xFF, 0x15, 0x18, 0x00, 0x00, 0x00,  // call [rip+18h]       ; HookSlot
    0x48, 0x83, 0xC4, 0x20,              // add  rsp, 20h
    0x41, 0x5B,                          // pop  r11
    0x41, 0x5A,                          // pop  r10
    0x41, 0x59,                          // pop  r9
    0x41, 0x58,                          // pop  r8
    0x5A,                                // pop  rdx
    0x59,                                // pop  rcx
    0x58,                                // pop  rax
    0xFF, 0x25, 0x0B, 0x00, 0x00, 0x00,  // jmp  [rip+0Bh]       ; OriginalSlot
};
static_assert(sizeof(RateStoredStubCode) == 0x2D, "The stub layout changed; fix the slot displacements.");
static_assert(0x12 + 6 + 0x18 == HookSlot && 0x27 + 6 + 0x0B == OriginalSlot, "Slot displacements do not match.");

auto BuildRelayPage() noexcept -> bool {
    RelayPage = AllocateNear(Base + RateStoredCallRva, RelayPageBytes);
    if (RelayPage == nullptr) return false;
    auto* page = static_cast<std::uint8_t*>(RelayPage);
    std::memset(page, 0xCC, RelayPageBytes);
    std::memcpy(page + RateStoredStub, RateStoredStubCode, sizeof(RateStoredStubCode));
    const auto hook     = reinterpret_cast<std::uint64_t>(&HookedRateStored);
    const auto original = static_cast<std::uint64_t>(Base + UnitUpdateGetterRva);
    std::memcpy(page + RateStoredStub + HookSlot, &hook, sizeof(hook));
    std::memcpy(page + RateStoredStub + OriginalSlot, &original, sizeof(original));
    DWORD      previous = 0;
    const bool sealed   = VirtualProtect(page, RelayPageBytes, PAGE_EXECUTE_READ, &previous) != FALSE;
    FlushInstructionCache(GetCurrentProcess(), page, RelayPageBytes);
    return sealed;
}

// The five bytes at 350C73, call 362360, as the witness holds them.
auto OriginalCall() noexcept -> const std::uint8_t* {
    return RateStoreWindow + (RateStoredCallRva - RateStoreWindowRva);
}

auto PatchCall() noexcept -> bool {
    const std::uintptr_t site   = Base + RateStoredCallRva;
    const std::uintptr_t target = reinterpret_cast<std::uintptr_t>(RelayPage) + RateStoredStub;
    if (!CanEncodeRel32(site, target)) return false;
    EncodeCall(site, target, RedirectedCall);
    return Context->PatchBytes(RateStoredCallRva, OriginalCall(), CallSize, RedirectedCall, CallSize);
}

void RestoreCall() noexcept {
    if (!CallPatched) return;
    if (Context->PatchBytes(RateStoredCallRva, RedirectedCall, CallSize, OriginalCall(), CallSize)) CallPatched = false;
    // The relay page is kept: a thread may be inside it right now.
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

auto VerifyAll() -> bool {
    bool ok = true;
    for (const Witness& witness : Witnesses) {
        if (!Context->CheckExpectedBytes(witness.rva, witness.bytes, witness.size)) {
            D2RL::LogErrorF(Context, "FrenzySpeed: %s at RVA 0x%llX does not match this build.", witness.name,
                static_cast<unsigned long long>(witness.rva));
            ok = false;
        }
    }
    return ok;
}

auto VerifyLabels() -> bool {
    bool ok = true;
    for (const Witness& witness : LabelWitnesses) {
        if (!Context->CheckExpectedBytes(witness.rva, witness.bytes, witness.size)) {
            D2RL::LogWarnF(Context, "FrenzySpeed: %s at RVA 0x%llX does not match; the debug log cannot tell server "
                "from client.", witness.name, static_cast<unsigned long long>(witness.rva));
            ok = false;
        }
    }
    return ok;
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

auto CommandArgs(const D2RL::ConsoleCommandContext* command) -> std::string_view {
    if (command == nullptr || command->args == nullptr || command->argsLength == 0) return {};
    return Trim(std::string_view(command->args, command->argsLength));
}

auto Count(const std::atomic<std::uint64_t>& counter) -> unsigned long long {
    return static_cast<unsigned long long>(counter.load(std::memory_order_relaxed));
}

//   frenzyspeed               state, counters, the last change
//   frenzyspeed debug on|off  the debug log (no argument toggles it)
//   frenzyspeed reset         zero the counters
auto __cdecl StatusCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept
        -> D2RL::ConsoleCommandResult {
    if (command == nullptr || command->plugin == nullptr) return D2RL::ConsoleCommandResult::Failed;
    char       line[320];
    const auto say = [&](const char* text) { command->plugin->WriteConsoleMessage(text); };
    const std::string_view args = CommandArgs(command);
    if (args.substr(0, 5) == "debug") {
        const std::string_view value = Trim(args.substr(5));
        bool on = !DebugOn.load();
        if (value == "on") {
            on = true;
        } else if (value == "off") {
            on = false;
        } else if (!value.empty()) {
            say("Usage: frenzyspeed debug [on|off]");
            return D2RL::ConsoleCommandResult::Handled;
        }
        DebugOn.store(on);
        if (on) {
            D2RL::LogInfoF(Context, "FrenzySpeed debug: on (Frenzy skill %d, frames stat %d).", Settings.frenzySkill,
                Settings.framesStat);
        } else {
            Context->LogInfo("FrenzySpeed debug: off.");
        }
        say(on ? "Frenzy Speed: debug log on, see d2rloader/logs/celestialrayone.frenzy-speed.log."
               : "Frenzy Speed: debug log off.");
        return D2RL::ConsoleCommandResult::Handled;
    }
    if (args == "reset") {
        ResetCounters();
        say("Frenzy Speed: counters reset.");
        return D2RL::ConsoleCommandResult::Handled;
    }
    if (!args.empty()) {
        say("Usage: frenzyspeed [debug [on|off] | reset]");
        return D2RL::ConsoleCommandResult::Handled;
    }

    std::snprintf(line, sizeof(line), "Frenzy Speed %s: %s. Frenzy is skill %d, frames stat %d. Debug log %s.",
        PluginVersion, StateName(State), Settings.frenzySkill, Settings.framesStat, DebugOn.load() ? "on" : "off");
    say(line);
    std::snprintf(line, sizeof(line), "  Frenzy speed updates: %llu, changed %llu (%llu held back so every hit lands), "
        "%llu with the stat at 0, %llu already as fast as the hits allow.", Count(FrenzyUpdates), Count(RatesChanged),
        Count(RatesHeldBack), Count(NoPoints), Count(NothingToGain));
    say(line);
    if (Count(NotAttackRate) != 0 || Count(OtherShape) != 0) {
        std::snprintf(line, sizeof(line), "  left at the game's speed: %llu not on the attack rate (skills.txt "
            "UseAttackRate?), %llu with a sequence outside the model.", Count(NotAttackRate), Count(OtherShape));
        say(line);
    }
    LastChange last{};
    {
        std::lock_guard lock(LastMutex);
        last = Last;
    }
    if (!last.valid) {
        say("  no Frenzy with the stat seen yet.");
    } else {
        std::snprintf(line, sizeof(line), "  last [%s]: stat %d x %d swings, game %lld frames (rate %d), asked %lld, "
            "%s %lld frames (rate %d)%s.", SideName(last.labelled, last.server), last.points, last.plan.swings,
            static_cast<long long>(last.plan.gameFrames), last.gameRate, static_cast<long long>(last.plan.wanted),
            last.plan.changed ? "now" : "kept", static_cast<long long>(last.plan.frames), last.plan.rate,
            last.plan.heldBack ? ", held back so every hit lands" : "");
        say(line);
    }
    return D2RL::ConsoleCommandResult::Handled;
}

constexpr D2RL::PluginInfo PluginInfoData{
    .infoSize    = D2RL::PluginInfoSize,
    .abiVersion  = D2RL_PLUGIN_ABI_VERSION,
    .id          = PluginIdText,
    .name        = "Frenzy Speed",
    .version     = PluginVersion,
    .author      = "CelestialRayOne",
    .description = "A stat takes frames off Frenzy: each point is one frame off each swing.",
    .flags       = D2RL::PluginFlags::Shared | D2RL::PluginFlags::NativeHooks,
};

} // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
    return &PluginInfoData;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
    Context = context;
    if (Context == nullptr || Context->exeBase == 0) return false;
    Base = Context->exeBase;

    if (!Context->RegisterConsoleCommand("frenzyspeed", &StatusCommand,
            "Frenzy Speed status and counters. \"frenzyspeed debug on|off\" writes the debug log, "
            "\"frenzyspeed reset\" zeroes the counters.")) {
        Context->LogWarn("FrenzySpeed: the console command could not be registered.");
    }

    ReadConfiguration();
    if (!Settings.enabled || Settings.frenzySkill < 0) {
        State = PluginState::DisabledByConfig;
        Context->LogInfo("FrenzySpeed: disabled by config.");
        return true;
    }
    if (!VerifyAll()) {
        State = PluginState::UnsupportedBuild;
        Context->LogError("FrenzySpeed: this D2R build does not match; nothing was installed.");
        return true;
    }
    LabelsVerified = VerifyLabels();
    GetStat        = reinterpret_cast<GetStatFn>(Base + GetUnitStatRva);

    if (!BuildRelayPage()) {
        State = PluginState::InstallFailed;
        Context->LogError("FrenzySpeed: no relay page could be placed within reach of the game code.");
        return true;
    }
    DebugOn.store(Settings.debug);
    Active.store(true);
    CallPatched = PatchCall();
    if (!CallPatched) {
        Active.store(false);
        State = PluginState::InstallFailed;
        Context->LogError("FrenzySpeed: the call at RVA 0x350C73 could not be redirected; nothing is active.");
        return true;
    }
    State = PluginState::Active;
    D2RL::LogInfoF(Context, "FrenzySpeed %s: active (Frenzy skill %d, frames stat %d, debug log %s).", PluginVersion,
        Settings.frenzySkill, Settings.framesStat, Settings.debug ? "on" : "off");
    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    Active.store(false);
    if (Context != nullptr) RestoreCall();
}
