// ---------------------------------------------------------------------------
// celestialrayone.whirlwind
//
// Whirlwind rework for Diablo II: Resurrected 3.3.93847.
//
//   ctc_while_uninterruptible  item procs fire while the caster is
//                              Uninterruptible (global), optional allow list
//                              and chance (the WhirlwindCTCChance behaviour)
//   proc_start_fix             a proc keeps the caster's used skill and mode
//   dual_wield_same_target     both weapons may strike the same target
//   cyclone                    Whirlwind follows the cursor while held
//   death_while_whirlwinding   a lethal hit kills during Whirlwind
//   potions_while_whirlwinding potions can be used during Whirlwind
//   whirlwind_with_bows        sequence skills work with bows and crossbows
//   whirlwind_speed_sync       the server starts Whirlwind at the same movement
//                              speed the client does
//
// Every address is an RVA against the executable base. Nothing is written
// unless the bytes at the site, and at every game function the part calls,
// match the verified 3.3.93847 image as hosted by D2RLoader 1.3.0 byte for
// byte. A part whose bytes do not match installs nothing; the other parts are
// unaffected.
//
// D2RLoader 1.3.0 took over two game functions the cyclone server part calls:
// the skill resource payment 00436830 (now D2RCore ConsumeWideSkillResource)
// and the skill level reader 0033D1E0 (now D2RCore ReadWideSkillLevel). Their
// entries are loader jumps; the plugin still calls them there and checks the
// jump shape plus the untouched rest of each function, never the loader-owned
// pointer. The native code that calls them is unchanged.
// ---------------------------------------------------------------------------

#include <D2RLPlugin/api.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

const D2RL::PluginContext* g_context = nullptr;

struct Witness {
	std::uint64_t       rva;
	const std::uint8_t* bytes;
	std::uint32_t       size;
	const char*         what;
};

// ---------------------------------------------------------------------------
// Verified bytes (generated)
// ---------------------------------------------------------------------------

// Generated from bytes read out of the 3.3.93847 image as hosted by
// D2RLoader 1.3.0. Do not edit by hand.
constexpr std::uint8_t W_CastOnTarget_5896E0[] = {0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x6C,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x57,0x41,0x56,0x41,0x57,0x48,0x83,0xEC,0x70,0x49,0x8B,0xF1,0x41,0x8B,0xE8,0x44,0x8B,0xF2,0x48,0x8B,0xD9,0x48,0x85,0xC9,0x0F,0x84,0xF0,0x00,0x00,0x00,0x4D,0x85,0xC9,0x0F,0x84,0xE7,0x00,0x00,0x00,0xE8,0xE5,0x67,0xF0,0xFF,0x48,0x8B,0xF8,0x48,0x85,0xC0,0x0F,0x84,0xD6,0x00,0x00,0x00,0xBA,0x36,0x00,0x00,0x00,0x48,0x8B,0xCB,0xE8,0x7C,0xBA,0xDA,0xFF,0x85,0xC0};
constexpr std::uint8_t W_CastFailAtPos_5897FD[] = {0x33,0xC0,0x4C,0x8D,0x5C,0x24,0x70,0x49,0x8B,0x5B,0x28,0x49,0x8B,0x6B,0x30,0x49,0x8B,0x73,0x38,0x49,0x8B,0xE3,0x41,0x5F,0x41,0x5E,0x5F,0xC3,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x6C,0x24,0x18,0x56,0x57,0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x83,0xEC,0x70,0x41,0x8B,0xE9,0x45,0x8B,0xF0,0x44,0x8B,0xFA,0x48,0x8B,0xD9,0x48,0x85,0xC9,0x0F,0x84,0xCA,0x00,0x00,0x00,0xE8,0xB0,0x66,0xF0,0xFF,0x48,0x8B,0xF0,0x48,0x85,0xC0,0x0F,0x84,0xB9,0x00,0x00,0x00,0xBA,0x36,0x00,0x00,0x00,0x48,0x8B,0xCB,0xE8,0x47,0xB9,0xDA,0xFF,0x85,0xC0};
constexpr Witness CtcWitnesses[] = {
	{0x5896E0ULL, W_CastOnTarget_5896E0, 86, "CastOnTarget"},
	{0x5897FDULL, W_CastFailAtPos_5897FD, 110, "CastFailAtPos"},
};
constexpr std::uint8_t W_ProcCall_231219[] = {0xE8,0xA2,0x42,0x10,0x00,0x44,0x8B,0x84,0x24,0xC0,0x00,0x00,0x00,0x41,0xB9,0xFF,0xFF,0xFF,0xFF,0x41,0x8B,0xD6,0xC7,0x44,0x24,0x20,0x01,0x00,0x00,0x00,0x48,0x8B,0xCB};
constexpr std::uint8_t W_ProcCall_23123F[] = {0x85,0xC0,0x0F,0x84,0xBA,0x00,0x00,0x00,0x45,0x85,0xFF,0x74,0x11,0xBA,0x76,0x00,0x00,0x00,0x48,0x8B,0xCD,0x44,0x8D,0x42,0x8B,0xE8};
constexpr std::uint8_t W_ProcTail_231315[] = {0x49,0x8B,0xD4,0xE8,0xA3,0xCD,0x11,0x00,0xEB,0x0E,0x44,0x8B,0x44,0x24,0x44,0x8B,0x54,0x24,0x48,0xE8,0xA3,0xDF,0x11,0x00,0x8B,0x54,0x24,0x4C,0x48,0x8B,0xCB};
constexpr std::uint8_t W_ProcTail_231339[] = {0x4C,0x8B,0xA4,0x24,0xB0,0x00,0x00,0x00,0x8B,0xC6,0x48,0x83};
constexpr std::uint8_t W_SkillStartA_218490[] = {0x48,0x8B,0xCF,0xE8,0x38,0x35,0x13,0x00,0x48,0x8B,0xCF,0x84,0xC0,0x75,0x0C,0x45,0x85,0xFF,0x75,0x07,0xE8,0xF7,0xF8,0xFF,0xFF,0xEB,0x08,0x48,0x8B,0xD3,0xE8,0xBD,0x72,0x12,0x00,0x45,0x33,0xE4,0x8B,0xD8,0x48,0x8B,0xCF,0x85,0xC0,0x74,0x04,0x8B,0xD0,0xEB,0x3B,0xBA,0x0C,0x00,0x00,0x00,0xE8,0xE3,0xCC,0x11,0x00,0x85,0xC0,0x74,0x0C,0x0F,0xB6,0x46,0x25,0x84,0x05,0x01,0x12,0xB8,0x01,0x75,0x39,0x48,0x8B,0xCF,0xE8,0xEB,0x34,0x13,0x00,0x48,0x8B,0xCF,0x85,0xC0,0x74,0x0D,0xE8,0xDF,0x34,0x13,0x00};
constexpr std::uint8_t W_SkillStartB_2184F1[] = {0x83,0xF8,0x01,0x75,0x20,0x48,0x8B,0xCF,0xBA,0x01,0x00,0x00,0x00};
constexpr std::uint8_t W_SkillStartB_218503[] = {0x44,0x0F,0xB6,0xE0,0x45,0x85,0xE4,0x74,0x0A,0x8B,0xD3,0x48,0x8B,0xCF,0xE8,0x3A,0x12,0xEE,0xFF,0x48,0x0F,0xBF,0x8E,0x48,0x01,0x00,0x00,0x41,0xBE,0x01,0x00,0x00,0x00,0x66,0x85,0xC9,0x78,0x62,0x3B,0x0D,0x91,0x72,0x14,0x02,0x7D,0x5A,0x4C,0x8D,0x0D,0x58,0x70,0x14,0x02,0x4D,0x8B,0x0C,0xC9,0x4D,0x85,0xC9,0x74,0x4A,0x44,0x8B,0x45,0xB7,0x41,0x8B,0xD7,0x48,0x8B,0xCF,0x41,0xFF,0xD1,0x44,0x8B,0xF0,0x85,0xC0,0x75,0x36,0x33,0xD2,0x41,0xB8,0xFF,0xFF,0xFF,0xFF,0x48,0x8B,0xCF,0xE8,0xCB,0x6E,0x13,0x00,0x48,0x8B,0xCF,0xE8,0x63,0x34,0x13,0x00,0x48,0x8B,0xCF,0x85,0xC0,0x74,0x0D,0xE8,0x57,0x34,0x13,0x00,0x83,0xF8,0x01,0x75,0x0D,0x48,0x8B,0xCF,0xBA,0x01,0x00,0x00,0x00};
constexpr std::uint8_t W_SkillStartB_21858B[] = {0xE8,0x40,0x2D,0xE7,0xFF};
constexpr std::uint8_t W_GetUsedSkill_34BA40[] = {0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0x48,0x85,0xC9,0x75,0x13};
constexpr Witness ProcFixWitnesses[] = {
	{0x231219ULL, W_ProcCall_231219, 33, "ProcCall"},
	{0x23123FULL, W_ProcCall_23123F, 26, "ProcCall"},
	{0x231315ULL, W_ProcTail_231315, 31, "ProcTail"},
	{0x231339ULL, W_ProcTail_231339, 12, "ProcTail"},
	{0x218490ULL, W_SkillStartA_218490, 97, "SkillStartA"},
	{0x2184F1ULL, W_SkillStartB_2184F1, 13, "SkillStartB"},
	{0x218503ULL, W_SkillStartB_218503, 131, "SkillStartB"},
	{0x21858BULL, W_SkillStartB_21858B, 5, "SkillStartB"},
	{0x34BA40ULL, W_GetUsedSkill_34BA40, 14, "GetUsedSkill"},
};
constexpr std::uint8_t W_TickHead_5691C4[] = {0x49,0x8D,0x7F,0x30,0x45,0x33,0xFF,0x48,0x89,0x7C,0x24,0x60,0x44,0x89,0x7C,0x24,0x50,0xEB,0x4C,0xE8,0xD4,0xB0,0xEB,0xFF,0x48,0x8B,0xD0,0x48,0x8B,0xCB,0xE8,0x19,0x1A,0x00,0x00,0x41,0x03,0x85,0x70,0x01,0x00,0x00,0x48,0x8B,0xCB,0x41,0x89,0x47,0x2C,0xE8,0xC6,0xF5,0xDD,0xFF,0x33,0xC9,0x84,0xC0,0x0F,0x95,0xC1,0xFF,0xC1,0x49,0x8D,0x7F,0x30,0x89,0x4C,0x24,0x40,0x45,0x33,0xFF,0x48,0x89,0x7C,0x24,0x60,0x44,0x89,0x7C,0x24,0x50,0xEB,0x09,0x66,0x0F,0x1F,0x44,0x00,0x00,0x45,0x33,0xFF,0x8B,0x07,0x45,0x33,0xC9,0x4C,0x89,0x7C,0x24,0x38,0x45,0x33,0xC0,0x89,0x44,0x24,0x30,0x48,0x8B,0xD3,0xC7,0x44,0x24,0x28,0x03,0x00,0x00,0x00,0x49,0x8B,0xCD,0xC7,0x44,0x24,0x20,0x05,0x00,0x00,0x00,0xE8,0x81,0x9F,0xEC,0xFF,0x48,0x8B,0xF0,0x48,0x85,0xC0,0x75,0x13,0xC7,0x07,0xFF,0xFF,0xFF,0xFF,0x41,0x8B,0xFF};
constexpr std::uint8_t W_TickTail_569465[] = {0x8B,0x7C,0x24,0x40,0x45,0x33,0xC0,0x41,0x8B,0xD4,0x48,0x8B,0xCB,0xE8,0xC9,0x48,0xDD,0xFF,0x4C,0x8B,0xF8,0x48,0x85,0xC0,0x74,0x6F,0x48,0x8B,0xC8,0xE8,0x79,0x37,0xDD,0xFF,0x8B,0xC8,0x8B,0xD0,0x0F,0xBA,0xE9,0x0D,0x0F,0xBA,0xF0,0x0D,0x81,0xE2,0x00,0x20,0x00,0x00,0x0F,0x44,0xC1,0x49,0x8B,0xCF,0x8B,0xD0,0xE8,0x7A,0x57,0xDD,0xFF,0x8B,0x44,0x24,0x50,0xFF,0xC0,0x3B,0xC7,0x89,0x44,0x24,0x50,0x48,0x8B,0x7C,0x24,0x60};
constexpr std::uint8_t W_TickTail_5694BD[] = {0x8B,0x7C,0x24,0x44,0x8B,0x74,0x24,0x48,0x83,0xFF,0x03,0x75,0x1D,0x8B,0x44,0x24,0x4C,0x45,0x8B,0xCC,0x89,0x74,0x24,0x28};
constexpr Witness DualWieldWitnesses[] = {
	{0x5691C4ULL, W_TickHead_5691C4, 156, "TickHead"},
	{0x569465ULL, W_TickTail_569465, 82, "TickTail"},
	{0x5694BDULL, W_TickTail_5694BD, 24, "TickTail"},
};
constexpr std::uint8_t W_SqFrame_42A940[] = {0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x48,0x89,0x7C,0x24,0x20,0x41,0x56,0x48,0x83,0xEC,0x20,0x48,0x8B,0xF9,0x41,0x8B,0xF1,0x48,0x8B,0xCA,0x45,0x8B,0xF0,0x48,0x8B,0xDA,0xE8,0xD2,0x10,0xF2,0xFF,0x48,0x8B,0xE8,0x48,0x85,0xC0,0x74,0x72,0x8B,0xD6,0x48,0x8B,0xCB,0xE8,0x10,0x30,0xF2,0xFF,0x48,0x8B,0xCD,0xE8,0x78,0x22,0xF1,0xFF,0x8B,0xF0,0xA8,0x01,0x74,0x29,0x48,0x8B,0xD3,0x48,0x8B,0xCF,0xE8,0xB7,0x71,0x06,0x00,0x48,0x8B,0xD3,0x48,0x8B,0xCF};
constexpr std::uint8_t W_SqFrame_42A9A4[] = {0x83,0xF8,0x02,0x75,0x0E,0x0B,0xF0,0x48,0x8B,0xCD,0x8B,0xD6,0xE8,0x6B,0x42,0xF1,0xFF,0xEB,0x15,0x48,0x8B,0xCB,0xE8,0x91,0x53,0xF2,0xFF,0x85,0xC0,0x75,0x14,0x41,0x8D,0x46,0xFF,0x83,0xF8,0x01,0x77,0x0B,0x48,0x8B,0xD3,0x48,0x8B,0xCF,0xE8,0x39,0xC1,0x00,0x00,0x48,0x8B,0xCB,0xE8,0xE1,0x18,0xF2,0xFF,0x85,0xC0,0xB8,0x01,0x00,0x00,0x00,0x74,0x05,0xB8,0x02,0x00,0x00,0x00,0x48,0x8B,0x5C,0x24,0x30,0x48,0x8B,0x6C,0x24,0x38,0x48,0x8B,0x74,0x24,0x40,0x48,0x8B,0x7C,0x24,0x48,0x48,0x83,0xC4,0x20,0x41,0x5E,0xC3};
constexpr std::uint8_t W_ServerStart_56A4A0[] = {0x49,0x8B,0xCE,0xE8,0x08,0xAD,0xDC,0xFF,0x85,0xC0,0x0F,0x85,0xF3,0x03,0x00,0x00,0x49,0x8B,0xD6,0x49,0x8B,0xCF,0xE8,0x65,0x59,0xF2,0xFF,0x48,0x8B,0xD0,0x49,0x8B,0xCE,0x48,0x8B,0xF8,0xE8,0x07,0x03,0xDE,0xFF,0x48,0x85,0xFF,0x74,0x33,0x45,0x33,0xC9,0x44,0x8B,0xC0,0x48,0x8B,0xD7,0x49,0x8B,0xCE,0xE8,0x71,0xE1,0xDD,0xFF,0x85,0xC0};
constexpr std::uint8_t W_ServerStart_56A4E3[] = {0x41,0x8B,0xD4,0x49,0x8B,0xCE,0xE8,0xC2,0xE0,0xFC,0xFF,0x4C,0x8B,0xC7,0x49,0x8B,0xD6,0x49,0x8B,0xCF,0xE8,0xB4,0xE7,0xEC,0xFF,0xE9,0xA4,0x03,0x00,0x00,0x49,0x8B,0xCE,0xE8,0x77,0x09,0xDE,0xFF,0x48,0x8B,0xD8,0x48,0x85,0xC0,0x0F,0x84,0x83,0x03,0x00,0x00,0x49,0x8B,0xCE,0xE8,0xB3,0x14,0xDE,0xFF,0x33,0xF6,0x85,0xC0,0x40,0x0F,0x94,0xC6,0x89,0x75,0xDF,0x48,0x85,0xFF,0x74,0x2C,0x85,0xF6,0x74,0x28,0x44,0x8B,0x45,0xDB,0x48,0x8B,0xCB,0x8B,0x55,0xD7,0xE8,0x10,0x85,0xDD,0xFF,0xBA,0x01,0x0C,0x00,0x00,0x48,0x8B,0xCB,0xE8,0xF3,0x81,0xDD,0xFF,0xBF,0x09,0x1C,0x00,0x00,0xE8,0x89,0x6D,0xDD,0xFF,0xEB,0x2E,0xB8,0x01,0x3C,0x00,0x00,0xBF,0x09,0x1C,0x00,0x00,0x85,0xF6,0xBA,0x01,0x0C,0x00,0x00,0x48,0x8B,0xCB,0x0F,0x45,0xC7,0x8B,0xF8,0xE8,0xC9,0x81,0xDD,0xFF,0x85,0xF6,0x74,0x07,0xE8,0x60,0x6D,0xDD,0xFF,0xEB,0x05,0xB8,0x07,0x00,0x00,0x00,0x8B,0xD0,0x48,0x8B,0xCB,0xE8,0xDF,0x84,0xDD,0xFF,0x41,0xB9,0x01,0x00,0x00,0x00,0x45,0x33,0xC0,0x49,0x8B,0xD6,0x48,0x8B,0xCB,0xE8,0xCB,0x68,0xDD,0xFF,0x85,0xC0,0x0F,0x84,0xD4,0x02,0x00,0x00,0x49,0x8B,0xCE,0xE8,0xCB,0xDB,0xEC,0xFF,0x8B,0xD0,0x4C,0x8D,0x05,0x52,0x3A,0x7D,0x01,0x41,0xB9,0x50,0x04,0x00,0x00,0x48,0x8B,0xCB,0xE8,0x14,0x85,0xDD,0xFF,0xBA,0x01,0x04,0x00,0x00,0x48,0x8B,0xCB,0xE8,0x67,0x81,0xDD,0xFF,0x48,0x8D,0x55,0xE7,0x48,0x8B,0xCB,0xE8,0xFB,0x74,0xDD,0xFF,0x85,0xC0,0x0F,0x8E,0x94,0x02,0x00,0x00,0xFF,0xC8,0x48,0x63,0xC8,0x48,0x8B,0x45,0xE7,0x48,0x8D,0x14,0x88,0x49,0x8B,0xCE,0x0F,0xB7,0x02,0x89,0x45,0xD7,0x0F,0xB7,0x42,0x02,0x89,0x45,0xDB,0xE8,0x61,0x72,0xDD,0xFF,0x8B,0x7D,0xDB,0x49,0x8B,0xCE,0x8B,0x75,0xD7,0x8B,0xD8,0xE8,0x21,0x0E,0xDE,0xFF,0x8B,0x4D,0xDF,0x44,0x8B,0xCB,0x83,0xF1,0x01,0x44,0x8B,0xC7,0xFF,0xC1,0x8B,0xD6,0xC1,0xE1,0x07,0x89,0x4C,0x24,0x20,0x48,0x8B,0xC8,0xE8,0xB2,0x96,0xDF,0xFF,0xBA,0x01,0x00,0x00,0x00,0x49,0x8B,0xCE,0xE8,0xD5,0x41,0xF2,0xFF,0xBA,0x12,0x00,0x00,0x00,0x49,0x8B,0xCE,0x44,0x8D,0x42,0xEF,0xE8,0x64,0xAE,0xDC,0xFF,0x48,0x8B,0x5D,0x67,0xBA,0x01,0x00,0x00,0x00,0x48,0x8B,0xCB,0xE8,0xB3,0x45,0xDD,0xFF,0x8B,0x45,0xD7,0x33,0xF6,0x89,0x43,0x24,0x8B,0x45,0xDB,0x89,0x43,0x28,0xC7,0x43,0x30,0xFF,0xFF,0xFF,0xFF,0x89,0x73,0x2C,0x66,0x41,0x83,0xBD,0xA0,0x00,0x00,0x00,0xFF,0x7C,0x19};
constexpr std::uint8_t W_SkillRecFlags_33CBC0[] = {0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0x48,0x85,0xC9,0x74,0x06,0x48,0x83,0x39,0x00,0x75,0x21,0x48,0x8D,0x4C,0x24,0x30,0xC6,0x44,0x24,0x30,0x00,0xE8,0xDD,0xA3,0xFF,0xFF,0x84,0xC0,0x74,0x01,0xCC,0x48,0x85,0xDB,0x75,0x08,0x33,0xC0,0x48,0x83,0xC4,0x20,0x5B,0xC3,0x48,0x8B,0x03,0x48,0x83,0xC4,0x20,0x5B,0xC3,0xCC,0xCC,0x48,0x85,0xC9,0x74,0x04,0x8B,0x41,0x14,0xC3,0x33,0xC0,0xC3};
constexpr std::uint8_t W_SetFlags_33EC20[] = {0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x8B,0xFA,0x48,0x8B,0xD9};
constexpr Witness ServerFrameWitnesses[] = {
	{0x42A940ULL, W_SqFrame_42A940, 95, "SqFrame"},
	{0x42A9A4ULL, W_SqFrame_42A9A4, 100, "SqFrame"},
	{0x56A4A0ULL, W_ServerStart_56A4A0, 65, "ServerStart"},
	{0x56A4E3ULL, W_ServerStart_56A4E3, 429, "ServerStart"},
	{0x33CBC0ULL, W_SkillRecFlags_33CBC0, 76, "SkillRecFlags"},
	{0x33EC20ULL, W_SetFlags_33EC20, 15, "SetFlags"},
	{0x34BA40ULL, W_GetUsedSkill_34BA40, 14, "GetUsedSkill"},
};
constexpr std::uint8_t W_Dispatcher_4F2FA0[] = {0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x20,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x83,0xEC,0x20,0x41,0x0F,0xB6,0x30,0x45,0x8B,0xE1,0x4D,0x8B,0xF8,0x48,0x8B,0xFA,0x4C,0x8B,0xE9,0xBB,0x01,0x00,0x00,0x00,0x40,0x80,0xFE,0x67,0x72,0x14,0x48,0x8D,0x4C,0x24,0x60,0xC6,0x44,0x24,0x60,0x00,0xE8,0x8A,0xE2,0xFE,0xFF,0x84,0xC0,0x74,0x01,0xCC,0x48,0x8D,0x15,0x0E,0xD0,0xB0,0xFF,0x4C,0x8D,0xB2,0x90,0xA7,0xD2,0x01,0x49,0x83,0x3C,0xF6,0x00,0x4D,0x8D,0x34,0xF6,0x75,0x1B,0x48,0x8D,0x4C,0x24,0x60,0xC6,0x44,0x24,0x60,0x00,0xE8,0x2D,0xE2,0xFE,0xFF,0x84,0xC0,0x74,0x01,0xCC,0x48,0x8D,0x15,0xE1,0xCF,0xB0,0xFF,0x8D,0x46,0xFF,0x83,0xF8,0x41,0x77,0x35,0x48,0x98,0x0F,0xB6,0x84,0x02,0xF0,0x30,0x4F,0x00,0x8B,0x8C,0x82,0xE0,0x30,0x4F,0x00,0x48,0x03,0xCA,0xFF,0xE1,0x48,0x8B,0xCF,0xE8,0x1B,0x7B,0xE5,0xFF,0x83,0xF8,0x11,0x75,0x45,0x33,0xDB,0xEB,0x41,0x48,0x8B,0xCF,0xE8,0x6A,0x92,0xE5,0xFF,0x8B,0xD8,0x85,0xC0,0xEB,0x23,0x48,0x8B,0xCF,0xE8,0x5C,0x92,0xE5,0xFF,0x85,0xC0,0x75,0x13,0x8D,0x50,0x36,0x48,0x8B,0xCF};
constexpr std::uint8_t W_Dispatcher_4F3073[] = {0x85,0xC0,0x75,0x04,0x33,0xDB,0xEB,0x14,0x40,0x80,0xFE,0x26,0x74,0x0E,0x8B,0xD6,0x48,0x8D,0x0D,0x0E,0x64};
constexpr std::uint8_t W_UseItem_4F40C0[] = {0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x56,0x48,0x8D,0xAC,0x24,0x58,0xFF,0xFF,0xFF,0x48,0x81,0xEC,0xA8,0x01,0x00,0x00,0x48,0x8B,0x05,0xE9,0x71,0x4D,0x02,0x48,0x33,0xC4,0x48,0x89,0x85,0x90,0x00,0x00,0x00,0x45,0x33,0xE4,0x48,0x89,0x4C,0x24,0x70,0xBB,0x05,0x00,0x00,0x00,0x4C,0x89,0x65,0x88,0x8B,0xF3,0x48,0x89,0x54,0x24,0x68,0x45,0x0F,0xB6,0xF1,0x4C,0x89,0x44,0x24,0x78,0x48,0x8D,0x7D,0xF0,0x66,0x90,0x48,0x8B,0xCF,0xE8,0x18,0xED,0xE8,0xFF,0x48,0x83,0xC7,0x10,0x48,0x83,0xEE,0x01,0x75,0xEE,0x48,0x8D,0x7D,0x40,0x48,0x8B,0xCF,0xE8,0x02,0xED,0xE8,0xFF,0x48,0x83,0xC7,0x10,0x48,0x83,0xEB,0x01,0x75,0xEE,0x48,0x8B,0x4C,0x24,0x68,0x8D,0x53,0x36};
constexpr std::uint8_t W_UseItem_4F4145[] = {0x85,0xC0,0x0F,0x85,0x36,0x1E,0x00,0x00,0x48,0x8B,0x4C,0x24,0x68,0xE8,0x69,0x81,0xE5,0xFF,0x85,0xC0,0x0F,0x85,0x24,0x1E,0x00,0x00,0x48,0x8B,0x44,0x24,0x78,0x8D,0x53,0x04,0x48,0x8B,0x4C,0x24,0x70,0x44,0x8B,0x40,0x0A,0xE8,0x0B,0xBD,0xF9,0xFF,0x48,0x89,0x45};
constexpr std::uint8_t W_CheckItemType_373890[] = {0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x6C,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x57,0x41,0x56,0x41,0x57,0x48,0x83,0xEC,0x20};
constexpr Witness PotionWitnesses[] = {
	{0x4F2FA0ULL, W_Dispatcher_4F2FA0, 206, "Dispatcher"},
	{0x4F3073ULL, W_Dispatcher_4F3073, 21, "Dispatcher"},
	{0x4F40C0ULL, W_UseItem_4F40C0, 128, "UseItem"},
	{0x4F4145ULL, W_UseItem_4F4145, 51, "UseItem"},
	{0x373890ULL, W_CheckItemType_373890, 24, "CheckItemType"},
	{0x34BA40ULL, W_GetUsedSkill_34BA40, 14, "GetUsedSkill"},
	{0x33CBC0ULL, W_SkillRecFlags_33CBC0, 76, "SkillRecFlags"},
};
constexpr std::uint8_t W_SeqResolver_3CB890[] = {0x40,0x55,0x56,0x48,0x83,0xEC,0x38,0x48,0x8B,0xF1,0xE8,0xA1,0x01,0xF8,0xFF,0x48,0x85,0xC0,0x0F,0x84,0x57,0x01,0x00,0x00,0x48,0x8B,0xD0,0x48,0x8B,0xCE,0xE8,0x0D,0x23,0xF7,0xFF,0x48,0x63,0xE8,0x85,0xC0,0x0F,0x8E,0x41,0x01,0x00,0x00,0x48,0x89,0x5C,0x24,0x50,0x48,0x8B,0xCE,0x48,0x89,0x7C,0x24,0x60,0xE8,0x00,0x01,0xF8,0xFF,0x33,0xFF,0x85,0xC0,0x0F,0x85,0xC9,0x00,0x00,0x00,0x4C,0x89,0x74,0x24,0x68,0x48,0x8D,0x15,0xFA,0x12,0x94,0x01,0x4C,0x89,0x7C,0x24,0x30,0x41,0xB8,0x5C,0x06,0x00,0x00,0x4C,0x8D,0x3D,0x08,0x47,0xC3,0xFF,0x48,0x8B,0xCE,0x4D,0x8B,0xB4,0xEF,0x50,0x66,0x38,0x02,0x49,0x8B,0x06,0x0F,0xB6,0x58,0x02,0xE8,0x51,0xEA,0xF7,0xFF,0x48,0x8B,0xD0,0xC7,0x44,0x24,0x20,0x01,0x00,0x00,0x00,0x44,0x8B,0xCB,0x4C,0x8D,0x44,0x24,0x58,0x48,0x8B,0xCE,0xE8,0xD6,0xB4,0xFE,0xFF,0x8B,0x4C,0x24,0x58,0x48,0x8D,0x05,0xFF,0xAD,0xFB,0x01,0x48,0x8D,0x15,0x68,0xAE,0xFB,0x01,0x0F,0x1F,0x40,0x00,0x3B,0x08,0x74,0x12,0xFF,0xC7,0x48,0x83,0xC0,0x08,0x48,0x3B,0xC2,0x7C,0xF1,0xBB,0xFF,0xFF,0xFF,0xFF,0xEB,0x10,0x48,0x63,0xC7,0x41,0x8B,0x9C,0xC7,0x30,0x67,0x38,0x02,0x83,0xFB,0xFF,0x75,0x14,0x48,0x8D,0x4C,0x24,0x58,0xC6,0x44,0x24,0x58,0x00,0xE8,0x9B,0xFC,0xFF,0xFF,0x84,0xC0,0x74,0x01,0xCC,0x4C,0x8B,0x7C,0x24,0x30,0x48,0x63,0xC3,0x48,0x8B,0x5C,0x24,0x50};
constexpr std::uint8_t W_SeqResolver_3CB98F[] = {0x4C,0x8B,0x74,0x24,0x68,0x48,0x8B,0xC7,0x48,0x8B,0x7C,0x24,0x60,0x48,0x83,0xC4,0x38,0x5E,0x5D,0xC3,0x83,0xF8,0x01,0x75,0x43,0x41,0xB8,0x83,0x06,0x00,0x00,0x48,0x8D,0x15,0x2B,0x12,0x94,0x01,0x48,0x8B,0xCE,0xE8,0xA3,0xDE,0xF7,0xFF,0x48,0x8B,0xCE,0x8B,0xD8,0xE8,0x19,0xE7,0xF7,0xFF,0x8B,0xD3,0x0F,0xB6,0xC8,0xE8,0x0F,0xBD,0xCC,0xFF,0x48,0x85,0xC0,0x74,0x15,0x48,0x8B,0xCE,0xE8,0x02,0xE7,0xF7,0xFF,0x0F,0xB6,0xC8,0x8B,0xD5,0xE8,0xF8,0xB5,0xFC,0xFF,0x48,0x8B,0xF8,0x48,0x8B,0x5C,0x24,0x50,0x48,0x8B,0xC7,0x48,0x8B,0x7C,0x24,0x60,0x48,0x83,0xC4,0x38,0x5E,0x5D,0xC3,0x33,0xC0,0x48,0x83,0xC4,0x38,0x5E,0x5D,0xC3};
constexpr Witness SequenceWitnesses[] = {
	{0x3CB890ULL, W_SeqResolver_3CB890, 247, "SeqResolver"},
	{0x3CB98FULL, W_SeqResolver_3CB98F, 121, "SeqResolver"},
};
constexpr std::uint8_t W_ItemUseLock_1C7360[] = {0x48,0x83,0xEC,0x28,0xBA,0x02,0x00,0x00,0x00,0xE8,0xA2,0xE4,0x12,0x00,0x48,0x85,0xC0,0x75,0x29,0xE8,0x58,0x3F,0xEC,0xFF,0x8B,0xC8,0xE8,0x01,0x31,0xED,0xFF,0x48,0x85,0xC0,0x74,0x11,0xBA,0x36,0x00,0x00,0x00,0x48,0x8B,0xC8};
constexpr std::uint8_t W_ItemUseLock_1C7391[] = {0x85,0xC0,0x75,0x07,0x33,0xC0,0x48,0x83,0xC4,0x28,0xC3,0xB8,0x01,0x00,0x00,0x00,0x48,0x83,0xC4};
constexpr std::uint8_t W_LocalPlayer_230A90[] = {0xFF,0x0F,0x84,0xF5,0x02,0x00,0x00,0x48,0x85,0xC0,0x0F,0x84,0xEC,0x02,0x00,0x00,0xE8,0x2B,0xA8,0xE5,0xFF,0x8B,0xC8,0xE8,0xD4,0x99,0xE6,0xFF,0x4C,0x3B,0xF0,0x75,0x29,0x41,0xB8,0x1A,0x05,0x00,0x00,0x48,0x8D,0x15,0xF2,0x30,0xAB,0x01,0x49,0x8B,0xCF,0xE8,0xBA,0xD5,0x10,0x00,0x3B,0xC7,0x75,0x10,0x49,0x8B,0xCF,0xE8,0x2E,0xC1,0x10,0x00,0xA8,0x01};
constexpr Witness PotionClientWitnesses[] = {
	{0x1C7360ULL, W_ItemUseLock_1C7360, 44, "ItemUseLock"},
	{0x1C7391ULL, W_ItemUseLock_1C7391, 19, "ItemUseLock"},
	{0x230A90ULL, W_LocalPlayer_230A90, 68, "LocalPlayer"},
	{0x373890ULL, W_CheckItemType_373890, 24, "CheckItemType"},
	{0x34BA40ULL, W_GetUsedSkill_34BA40, 14, "GetUsedSkill"},
	{0x33CBC0ULL, W_SkillRecFlags_33CBC0, 76, "SkillRecFlags"},
};
// 00436830: jmp qword [rip+disp32] into D2RCore ConsumeWideSkillResource, the
// loader's four NOPs, then the untouched body. disp32 is loader-owned.
constexpr std::uint8_t W_PayThunk_436830[] = {0xFF,0x25};
constexpr std::uint8_t W_PayBody_436836[] = {0x90,0x90,0x90,0x90,0x56,0x57,0x41,0x56,0x48,0x83,0xEC,0x40,0x48,0x8B,0xF1,0x45,0x8B,0xF1};
constexpr std::uint8_t W_ManaCost_33AA00[] = {0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x41,0x8B,0xF8,0x33,0xDB,0xE8,0x7C,0xCD,0xD5,0xFF};
// 0033D1E0: jmp qword [rip+disp32] into D2RCore ReadWideSkillLevel, then the
// untouched body. disp32 is loader-owned.
constexpr std::uint8_t W_SkillLevelThunk_33D1E0[] = {0xFF,0x25};
constexpr std::uint8_t W_SkillLevelBody_33D1E6[] = {0x48,0x8B,0xF9,0x48,0x85,0xC9,0x75,0x1B,0x88,0x4C,0x24,0x30};
// 0043B427, native skill start 0043B3E0: skills.txt record from the game's data
// context (game+106h), then, unless it is an item cast, the cast gate
// 00436750(unit), refusing the cast when it returns 0.
constexpr std::uint8_t W_CastGateCall_43B427[] = {0x0F,0xB6,0x8D,0x06,0x01,0x00,0x00,0x41,0x8B,0xD6,0xE8,0x5A,0xC3,0xC5,0xFF,0x48,0x8B,0xF8,0x48,0x85,0xC0,0x74,0x7E,0x83,0xBC,0x24,0x90,0x00,0x00,0x00,0x00,0x75,0x0C,0x48,0x8B,0xCB,0xE8,0x00,0xB3,0xFF,0xFF,0x85,0xC0,0x74,0x68};
// 0043B59E, same function, after the start function succeeded: unless it is an
// item cast, pay through 00436830(game, unit, skill id, level) with 32-bit id
// and level, and ignore the result.
constexpr std::uint8_t W_PayCall_43B59E[] = {0x83,0xBC,0x24,0x90,0x00,0x00,0x00,0x00,0x75,0x11,0x45,0x8B,0xCC,0x45,0x8B,0xC6,0x48,0x8B,0xD3,0x48,0x8B,0xCD,0xE8,0x77,0xB2,0xFF,0xFF,0x0F,0xB6,0x47,0x24};
constexpr std::uint8_t W_CollisionReset_3636D0[] = {0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xEC,0x30};
constexpr std::uint8_t W_PathSetTarget_342A50[] = {0x48,0x85,0xC9,0x74,0x11,0x66,0x89,0x51,0x10,0x66,0x44,0x89,0x41,0x12,0x48,0xC7,0x41,0x70,0x00,0x00,0x00,0x00,0xC3};
constexpr std::uint8_t W_PathGetX_341A20[] = {0x0F,0xB7,0x41,0x02,0xC3};
constexpr std::uint8_t W_PathGetY_341A30[] = {0x0F,0xB7,0x41,0x06,0xC3};
// 00342AE0 PATH_SetVelocity: refuses while the owner's used skill has flag
// 0x1000, else velocity at path+A0h and max velocity at path+A8h.
constexpr std::uint8_t W_PathSetVelocity_342AE0[] = {0x48,0x85,0xC9,0x74,0x55,0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0x8B,0xFA,0x48,0x8B,0x49,0x40,0x48,0x85,0xC9,0x74,0x18,0xE8,0x3E,0x8F,0x00,0x00,0x48,0x85,0xC0,0x74,0x0E,0x48,0x8B,0xC8,0xE8,0xF1,0xA0,0xFF,0xFF,0x0F,0xBA,0xE0,0x0C,0x72,0x1B,0x3B,0xBB,0xA0,0x00,0x00,0x00,0x74,0x07,0xC7,0x43,0x4C,0x0F,0x00,0x00,0x00,0x89,0xBB,0xA0,0x00,0x00,0x00,0x89,0xBB,0xA8,0x00,0x00,0x00};
// 00350B40 UNITS_UpdateAnimRateAndVelocity, the game's movement speed function.
constexpr std::uint8_t W_SpeedFnEntry_350B40[] = {0x48,0x85,0xC9,0x0F,0x84,0x6A,0x01,0x00,0x00,0x55,0x56,0x48,0x8B,0xEC,0x48,0x83,0xEC,0x68,0x8B,0x01,0x48,0x8B,0xF1,0x89,0x45,0x30,0x83,0xF8,0x03,0x0F,0x8F,0x4A,0x01,0x00,0x00,0x8B,0x41,0x04,0x4C,0x8D,0x4D,0xC8,0x89,0x45,0xCC};
// 00350FDC..003510B9, same function: the velocity branch (stat 96 through
// 003516C0 index 4, plus stat 67, floor 25) and the gate that sends a unit in a
// passive-skill mode there when its used skill has (flags & 0x1001) == 1.
constexpr std::uint8_t W_SpeedFnVelocity_350FDC[] = {0x83,0x3E,0x05,0x75,0x12,0x48,0x8D,0x4D,0x18,0x44,0x88,0x75,0x18,0xE8,0xB2,0x43,0xFF,0xFF,0x84,0xC0,0x74,0x01,0xCC,0x4C,0x8B,0x7E,0x38,0x4D,0x85,0xFF,0x0F,0x84,0x8C,0xFC,0xFF,0xFF,0xBA,0x04,0x00,0x00,0x00,0x48,0x8B,0xCE,0xE8,0xB3,0x06,0x00,0x00,0x45,0x33,0xC0,0x48,0x8B,0xCE,0x8B,0xF8,0x41,0x8D,0x50,0x43,0xE8,0x02,0x40,0xFA,0xFF,0x49,0x8B,0xCF,0x03,0xF8,0xE8,0xB8,0x0C,0xFF,0xFF,0x83,0xF8,0x11,0x75,0x17,0x49,0x8B,0xCF,0xE8,0xAB,0x08,0xFF,0xFF,0x66,0x0F,0x6E,0xCF,0x0F,0x5B,0xC9,0xF3,0x0F,0x59,0xC1,0xF3,0x0F,0x2C,0xF8,0x8B,0x4D,0x30,0xB8,0x19,0x00,0x00,0x00,0x8B,0x5D,0xC8,0x3B,0xF8,0x0F,0x4C,0xF8,0x85,0xC9,0x0F,0x84,0x84,0x05,0x00,0x00,0x83,0xF9,0x01,0x0F,0x84,0x5A,0x05,0x00,0x00,0x41,0x8B,0xCE,0xE9,0x83,0x05,0x00,0x00,0x48,0x63,0xC3,0x48,0x8D,0x3D,0x09,0xF6,0x9A,0x01,0x48,0x8D,0x0C,0x80,0x49,0x8D,0x04,0x8F,0x44,0x39,0x70,0x04,0x0F,0x85,0x53,0xFF,0xFF,0xFF,0x44,0x39,0x30,0x0F,0x84,0x9B,0xFE,0xFF,0xFF,0x48,0x8B,0x8E,0x00,0x01,0x00,0x00,0xE8,0xF2,0xCF,0xFE,0xFF,0x48,0x85,0xC0,0x74,0x29,0x48,0x8B,0xC8,0xE8,0x55,0xBB,0xFE,0xFF,0x25,0x01,0x10,0x00,0x00,0x83,0xF8,0x01,0x0F,0x84,0x23,0xFF,0xFF,0xFF};
// 01D00518: player mode 18 (SQ) row of the speed rules table 01D003B0, five
// dwords: by passive skill 1, velocity 0, skill attack rate 1, attack rate 0,
// anim speed 0.
constexpr std::uint8_t W_PlayerModeSpeedRules_1D00518[] = {0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
// 01D00B90: move entry of the rate stat table 01D00B60: diminished, 150, stat 96.
constexpr std::uint8_t W_MoveRateStat_1D00B90[] = {0x01,0x00,0x00,0x00,0x96,0x00,0x00,0x00,0x60,0x00,0x00,0x00};
constexpr Witness CycloneServerWitnesses[] = {
	{0x56A4A0ULL, W_ServerStart_56A4A0, 65, "ServerStart"},
	{0x56A4E3ULL, W_ServerStart_56A4E3, 429, "ServerStart"},
	{0x42A940ULL, W_SqFrame_42A940, 95, "SqFrame"},
	{0x42A9A4ULL, W_SqFrame_42A9A4, 100, "SqFrame"},
	{0x33CBC0ULL, W_SkillRecFlags_33CBC0, 76, "SkillRecFlags"},
	{0x33EC20ULL, W_SetFlags_33EC20, 15, "SetFlags"},
	{0x34BA40ULL, W_GetUsedSkill_34BA40, 14, "GetUsedSkill"},
	{0x436830ULL, W_PayThunk_436830, 2, "PayThunk"},
	{0x436836ULL, W_PayBody_436836, 18, "PayThunk"},
	{0x33AA00ULL, W_ManaCost_33AA00, 20, "ManaCost"},
	{0x33D1E0ULL, W_SkillLevelThunk_33D1E0, 2, "SkillLevelThunk"},
	{0x33D1E6ULL, W_SkillLevelBody_33D1E6, 12, "SkillLevelThunk"},
	{0x43B427ULL, W_CastGateCall_43B427, 45, "CastGateCall"},
	{0x43B59EULL, W_PayCall_43B59E, 31, "PayCall"},
	{0x3636D0ULL, W_CollisionReset_3636D0, 20, "CollisionReset"},
	{0x342A50ULL, W_PathSetTarget_342A50, 23, "PathSetTarget"},
	{0x341A20ULL, W_PathGetX_341A20, 5, "PathGetX"},
	{0x341A30ULL, W_PathGetY_341A30, 5, "PathGetY"},
	{0x342AE0ULL, W_PathSetVelocity_342AE0, 80, "PathSetVelocity"},
	{0x350B40ULL, W_SpeedFnEntry_350B40, 45, "SpeedFunction"},
	{0x350FDCULL, W_SpeedFnVelocity_350FDC, 221, "SpeedFunction"},
	{0x1D00518ULL, W_PlayerModeSpeedRules_1D00518, 20, "SpeedRules"},
	{0x1D00B90ULL, W_MoveRateStat_1D00B90, 12, "MoveRateStat"},
};
constexpr std::uint8_t W_ClientStart_230AE0[] = {0xCE,0x48,0x8D,0x55,0xC0,0xE8,0x96,0x41,0xFE,0xFF,0x85,0xC0,0x0F,0x84,0x9A,0x02,0x00,0x00,0x49,0x8B,0xCE,0xE8,0x76,0x9A,0xE6,0xFF,0x48,0x8B,0xD0,0x49,0x8B,0xCE,0x48,0x8B,0xF8,0xE8,0xC8,0x9C,0x11,0x00,0x48,0x85,0xFF,0x74,0x25,0x45,0x33,0xC9,0x44,0x8B,0xC0,0x48,0x8B,0xD7,0x49,0x8B,0xCE,0xE8,0x32,0x7B,0x11,0x00,0x85,0xC0};
constexpr std::uint8_t W_ClientStart_230B22[] = {0x48,0x8B,0xD7,0x49,0x8B,0xCE,0xE8,0xC3,0xE6,0xFF,0xFF,0xE9,0xC6,0x01,0x00,0x00,0x48,0x89,0xB4,0x24,0x98,0x00,0x00,0x00,0x49,0x8B,0xCE,0x4C,0x89,0xA4,0x24,0xA0,0x00,0x00,0x00,0xE8,0x86,0xAE,0x11,0x00,0x33,0xF6,0x85,0xC0,0x40,0x0F,0x94,0xC6,0x85,0xF6,0x74,0x1D,0x48,0x85,0xFF,0x74,0x0F,0x44,0x8B,0x45,0xC4,0x48,0x8B,0xCB,0x8B,0x55,0xC0,0xE8,0xE6,0x1E,0x11,0x00,0xE8,0x71,0x07,0x11,0x00,0x8B,0xF8,0xEB,0x05,0xBF,0x07,0x00,0x00,0x00,0xB8,0x01,0x3C,0x00,0x00,0xB9,0x09,0x1C,0x00,0x00,0x85,0xF6,0x44,0x8B,0xE6,0xBA,0x01,0x0C,0x00,0x00,0x0F,0x45,0xC1,0x41,0x83,0xF4,0x01,0x41,0xFF,0xC4,0x89,0x45,0xC8,0x48,0x8B,0xCB,0x41,0xC1,0xE4,0x07,0xE8,0x9B,0x1B,0x11,0x00,0x8B,0xD7,0x48,0x8B,0xCB,0xE8,0xC1,0x1E,0x11,0x00,0x48,0x8B,0xD3,0x49,0x8B,0xCE,0xE8,0x36,0x53,0xFE,0xFF,0x84,0xC0,0x0F,0x84,0x19,0x01,0x00,0x00,0x49,0x8B,0xCE,0x4C,0x89,0x6C,0x24,0x68,0xE8,0x11,0x95,0x11,0x00,0x48,0x8D,0x15,0xDA,0x2F,0xAB,0x01,0x44,0x0F,0xB6,0xE8,0x49,0x8B,0xCE,0x85,0xF6,0x0F,0x84,0x91,0x00,0x00,0x00,0x41,0xB8,0x4D,0x05,0x00,0x00,0xE8,0x70,0x8C,0x11,0x00,0x41,0x0F,0xB6,0xCD,0x48,0x63,0xF8,0xE8,0x94,0xFE,0x0C,0x00,0x48,0x8B,0xF0,0x85,0xFF,0x78,0x09,0x48,0x3B,0xB8,0x48,0x12,0x00,0x00,0x72,0x1A,0x48,0x8D,0x4D,0x38,0xC6,0x45,0x38,0x00,0xE8,0xF7,0x48,0xE5,0xFF,0x84,0xC0,0x74,0x01,0xCC,0x85,0xFF,0x0F,0x88,0x82,0x00,0x00,0x00,0x48,0x8B,0xC7,0x48,0x3B,0xBE,0x48,0x12,0x00,0x00,0x73,0x76,0x48,0x8D,0xBE,0x40,0x12,0x00,0x00,0x48,0x89,0x45,0x38,0x48,0x3B,0x47,0x08,0x72,0x1A,0x48,0x8D,0x45,0x38,0x48,0x89,0x7D,0xE0,0x48,0x8D,0x4D,0xD8,0x48,0x89,0x45,0xD8,0xE8,0xB8,0x54,0xE5,0xFF,0x84,0xC0,0x74,0x01,0xCC,0x48,0x69,0x45,0x38,0xD0,0x00,0x00,0x00,0x48,0x03,0x07,0x74,0x3E,0x0F,0xB6,0x50,0x3C,0x41,0xB9,0x50,0x05,0x00,0x00,0xEB,0x20,0x41,0xB8,0x55,0x05,0x00,0x00,0xE8,0xDF,0x8B,0x11,0x00,0x8B,0xD0,0x41,0x0F,0xB6,0xCD,0xE8,0x54,0x6A,0xE6,0xFF,0x41,0xB9,0x56,0x05,0x00,0x00,0x0F,0xBF,0x50,0x68,0x4C,0x8D,0x05,0x13,0x2F,0xAB,0x01,0xC1,0xE2,0x08,0x48,0x8B,0xCB,0xE8,0x38,0x1E,0x11,0x00,0xBA,0x01,0x04,0x00,0x00,0x48,0x8B,0xCB,0xE8,0x8B,0x1A,0x11,0x00,0x48,0x8D,0x55,0xD0,0x48,0x8B,0xCB,0xE8,0x1F,0x0E,0x11,0x00,0x4C,0x8B,0x6C,0x24,0x68,0x85,0xC0,0x75,0x44,0x48,0x8D,0x4D,0x38,0x88,0x45,0x38,0xE8,0x3A,0xDA,0xFF,0xFF,0x84,0xC0,0x74,0x01,0xCC,0x8B,0x55,0xC8,0x48,0x8B,0xCB,0xE8,0x5A,0x1A,0x11,0x00,0x33,0xC0,0x48,0x8B,0xB4,0x24,0x98,0x00,0x00,0x00,0x4C,0x8B,0xA4,0x24,0xA0,0x00,0x00,0x00,0x4C,0x8B,0x7C,0x24,0x60,0x48,0x8B,0x9C,0x24,0x90,0x00,0x00,0x00,0x48,0x83,0xC4,0x70,0x41,0x5E,0x5F,0x5D,0xC3,0xFF,0xC8,0x48,0x63,0xC8,0x48,0x8B,0x45,0xD0,0x48,0x8D,0x14,0x88,0x49,0x8B,0xCE,0x0F,0xB7,0x02,0x89,0x45,0xC0,0x0F,0xB7,0x42,0x02,0x89,0x45,0xC4,0xE8,0x40,0x0B,0x11,0x00,0x8B,0x7D,0xC4,0x49,0x8B,0xCE,0x8B,0x75,0xC0,0x8B,0xD8,0xE8,0x00,0xA7,0x11,0x00,0x44,0x8B,0xCB,0x44,0x89,0x64,0x24,0x20,0x44,0x8B,0xC7,0x8B,0xD6,0x48,0x8B,0xC8,0xE8,0x9B,0x2F,0x13,0x00,0xBA,0x01,0x00,0x00,0x00,0x49,0x8B,0xCF,0xE8,0xBE,0xDE,0x10,0x00,0x8B,0x4D,0xC0,0x33,0xD2,0x41,0x89,0x4F,0x24,0x8B,0x4D,0xC4,0x41,0x89,0x4F,0x28,0x49,0x8B,0xCE,0xE8,0xB6,0x40,0xFE,0xFF,0x49,0x8B,0xCE,0xE8,0x4E,0x68,0xFE,0xFF,0xB8,0x01,0x00,0x00,0x00,0xE9};
constexpr std::uint8_t W_ClientUpdate_FFCE1[] = {0x0F,0x84,0x8B,0x00,0x00,0x00,0x48,0x8B,0xC8,0xE8,0x11,0xCF,0x23,0x00,0x8B,0xD8,0x41,0x84,0xC6,0x74,0x41,0x41,0x0F,0xB6,0xD6,0x48,0x8B,0xCE};
constexpr std::uint8_t W_ClientUpdate_FFD02[] = {0x85,0xC0,0x74,0x18,0x83,0xCB,0x02,0x48,0x8B,0xCF,0x8B,0xD3,0xE8,0x0D,0xEF,0x23,0x00,0x48,0x8B,0xCE,0xE8,0x45,0x6F};
constexpr std::uint8_t W_IssuerTail_FAFFE[] = {0x01,0x00,0x00,0x48,0x8B,0xCB,0xE8,0xB7,0x98,0x1F,0x00,0x44,0x0F,0xB7,0xCE,0xBA,0x48,0x01,0x00,0x00,0x48,0x8B,0xCB,0x44,0x8D,0x40,0x01,0xE8,0xF2,0xCC,0x1F,0x00,0x45,0x8B,0xCE,0x45,0x8B,0xC7,0x48,0x8B,0xD3,0x41,0x8B,0xCD,0xE8,0xD1,0x0F,0x00,0x00,0x45,0x84,0xE4};
constexpr std::uint8_t W_GetLeftSkill_34A540[] = {0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0x48,0x85,0xC9,0x75,0x13};
constexpr std::uint8_t W_GetRightSkill_34B400[] = {0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0x48,0x85,0xC9,0x75,0x13};
constexpr std::uint8_t W_SendCommand_FC000[] = {0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x6C,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x57,0x41,0x56,0x41,0x57,0x48,0x83,0xEC,0x30};
constexpr Witness CycloneClientWitnesses[] = {
	{0x230AE0ULL, W_ClientStart_230AE0, 64, "ClientStart"},
	{0x230B22ULL, W_ClientStart_230B22, 614, "ClientStart"},
	{0x0FFCE1ULL, W_ClientUpdate_FFCE1, 28, "ClientUpdate"},
	{0x0FFD02ULL, W_ClientUpdate_FFD02, 23, "ClientUpdate"},
	{0x230A90ULL, W_LocalPlayer_230A90, 68, "LocalPlayer"},
	{0x0FAFFEULL, W_IssuerTail_FAFFE, 52, "IssuerTail"},
	{0x33CBC0ULL, W_SkillRecFlags_33CBC0, 76, "SkillRecFlags"},
	{0x34BA40ULL, W_GetUsedSkill_34BA40, 14, "GetUsedSkill"},
	{0x34A540ULL, W_GetLeftSkill_34A540, 14, "GetLeftSkill"},
	{0x34B400ULL, W_GetRightSkill_34B400, 14, "GetRightSkill"},
	{0x0FC000ULL, W_SendCommand_FC000, 24, "SendCommand"},
	{0x3636D0ULL, W_CollisionReset_3636D0, 20, "CollisionReset"},
	{0x342A50ULL, W_PathSetTarget_342A50, 23, "PathSetTarget"},
	{0x341A20ULL, W_PathGetX_341A20, 5, "PathGetX"},
	{0x341A30ULL, W_PathGetY_341A30, 5, "PathGetY"},
	{0x342AE0ULL, W_PathSetVelocity_342AE0, 80, "PathSetVelocity"},
	{0x350B40ULL, W_SpeedFnEntry_350B40, 45, "SpeedFunction"},
	{0x350FDCULL, W_SpeedFnVelocity_350FDC, 221, "SpeedFunction"},
	{0x1D00518ULL, W_PlayerModeSpeedRules_1D00518, 20, "SpeedRules"},
	{0x1D00B90ULL, W_MoveRateStat_1D00B90, 12, "MoveRateStat"},
};
// 0056A420, server Whirlwind start (srvstfunc 38): skills.txt record 00097790,
// token 0033DD40, target 00432B70. The native path block follows at 0056A4A0.
constexpr std::uint8_t W_ServerStartHead_56A420[] = {0x48,0x89,0x5C,0x24,0x10,0x44,0x89,0x4C,0x24,0x20,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0x6C,0x24,0xD9,0x48,0x81,0xEC,0xA0,0x00,0x00,0x00,0x4C,0x8B,0xF2,0x4C,0x8B,0xF9,0x0F,0xB6,0x89,0x06,0x01,0x00,0x00,0x41,0x8B,0xD0,0x45,0x8B,0xE0,0xE8,0x37,0xD3,0xB2,0xFF,0x4C,0x8B,0xE8,0x48,0x85,0xC0,0x0F,0x84,0x3E,0x04,0x00,0x00,0x45,0x33,0xC0,0x41,0x8B,0xD4,0x49,0x8B,0xCE,0xE8,0xCD,0x38,0xDD,0xFF,0x48,0x89,0x45,0x67,0x48,0x85,0xC0,0x0F,0x84,0x18,0x04,0x00,0x00,0x4C,0x8D,0x4D,0xDB,0x49,0x8B,0xD6,0x4C,0x8D,0x45,0xD7,0x49,0x8B,0xCF,0xE8,0xDD,0x86,0xEC,0xFF,0x85,0xC0,0x0F,0x84,0xFD,0x03,0x00,0x00,0xBA,0x0C,0x00,0x00,0x00};
constexpr Witness SpeedSyncWitnesses[] = {
	{0x56A420ULL, W_ServerStartHead_56A420, 128, "ServerStart"},
	{0x56A4A0ULL, W_ServerStart_56A4A0, 65, "ServerStart"},
	{0x56A4E3ULL, W_ServerStart_56A4E3, 429, "ServerStart"},
	{0x33CBC0ULL, W_SkillRecFlags_33CBC0, 76, "SkillRecFlags"},
	{0x34BA40ULL, W_GetUsedSkill_34BA40, 14, "GetUsedSkill"},
	{0x342AE0ULL, W_PathSetVelocity_342AE0, 80, "PathSetVelocity"},
	{0x350B40ULL, W_SpeedFnEntry_350B40, 45, "SpeedFunction"},
	{0x350FDCULL, W_SpeedFnVelocity_350FDC, 221, "SpeedFunction"},
	{0x1D00518ULL, W_PlayerModeSpeedRules_1D00518, 20, "SpeedRules"},
	{0x1D00B90ULL, W_MoveRateStat_1D00B90, 12, "MoveRateStat"},
};
constexpr std::uint8_t P_Executor[] = {0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x83,0xEC,0x50};
constexpr std::uint8_t P_HeldRight[] = {0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xF9,0x48,0x8B,0xDA};
constexpr std::uint8_t P_HeldLeft[] = {0x40,0x55,0x57,0x48,0x83,0xEC,0x38,0x48,0x8B,0xF9,0x33,0xED,0x48,0x8B,0x49,0x08};
constexpr std::uint8_t P_CastOnTarget[] = {0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x6C,0x24,0x18,0x48,0x89,0x74,0x24,0x20};
constexpr std::uint8_t P_CastAtPos[] = {0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x6C,0x24,0x18,0x56,0x57,0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x83,0xEC,0x70};
constexpr std::uint8_t P_Dispatcher[] = {0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x20,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x83,0xEC,0x20};
constexpr std::uint8_t P_UseItem[] = {0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x56,0x48,0x8D,0xAC,0x24,0x58,0xFF,0xFF,0xFF,0x48,0x81,0xEC,0xA8,0x01,0x00,0x00};
constexpr std::uint8_t P_ItemUseLock[] = {0x48,0x83,0xEC,0x28,0xBA,0x02,0x00,0x00,0x00};
constexpr std::uint8_t P_WhirlwindStart[] = {0x48,0x89,0x5C,0x24,0x10,0x44,0x89,0x4C,0x24,0x20,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57};

// ---------------------------------------------------------------------------
// Part bookkeeping
// ---------------------------------------------------------------------------

enum class PartState : std::uint8_t { NotAttempted, Installed, DisabledByConfig, UnsupportedBuild, InstallFailed };

enum class Part : std::size_t {
	CtcGates,
	CtcPolicy,
	ProcStartFix,
	DualWield,
	CycloneServer,
	CycloneClient,
	MeleeSpinServer,
	MeleeSpinClient,
	DeathWhileWhirlwinding,
	PotionsWhileWhirlwinding,
	PotionsWhileWhirlwindingClient,
	WhirlwindWithBows,
	WhirlwindSpeedSync,
	Count,
};

constexpr std::array<const char*, static_cast<std::size_t>(Part::Count)> PartNames {
	"CTC while Uninterruptible (gates)",
	"CTC while Uninterruptible (allow list / chance)",
	"Proc start fix (client)",
	"Dual wield same target",
	"Cyclone (server)",
	"Cyclone (client)",
	"Spin at melee targets (server)",
	"Spin at melee targets (client)",
	"Death while Whirlwinding",
	"Potions while Whirlwinding (server)",
	"Potions while Whirlwinding (client)",
	"Whirlwind with bows and crossbows",
	"Whirlwind speed sync (server start)",
};

std::array<PartState, static_cast<std::size_t>(Part::Count)> g_parts {};

void SetPart(Part part, PartState state) noexcept {
	g_parts[static_cast<std::size_t>(part)] = state;
}

auto PartIs(Part part, PartState state) noexcept -> bool {
	return g_parts[static_cast<std::size_t>(part)] == state;
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

constexpr const char* DefaultConfigToml = R"toml(# Whirlwind - Whirlwind rework for Diablo II: Resurrected 3.3
#
# Eight parts, each behind its own switch:
#   [ctc_while_uninterruptible]  item procs (chance to cast) fire while the
#                                caster is Uninterruptible, optionally filtered
#   [proc_start_fix]             a proc no longer takes over the caster's skill
#                                and animation on the client (the stuck
#                                Whirlwind after a Corpse Explosion proc)
#   [dual_wield_same_target]     both dual-wielded weapons may strike the same
#                                target in one Whirlwind hit
#   [cyclone]                    Whirlwind follows the cursor for as long as
#                                the button is held, and only that long
#   [death_while_whirlwinding]   a lethal hit kills the character during
#                                Whirlwind instead of waiting for it to end
#   [potions_while_whirlwinding] potions can be used during Whirlwind
#   [whirlwind_with_bows]        Whirlwind works with bows and crossbows
#   [whirlwind_speed_sync]       the server starts Whirlwind at the same
#                                movement speed the client does
#
# Safety: the plugin verifies the original bytes of every site it touches, and
# of every game function it calls, before it installs anything. On a game build
# it does not recognise, the affected part installs nothing, the reason is
# logged, and the plugin stays loaded so the `whirlwind` console command can
# tell you what happened.
#
# This file lives at <scope>\d2rloader\config\celestialrayone.whirlwind.toml
# and is created with these defaults on first run. Delete it to get them back.
# Changes are read at plugin load, so restart the game after editing.


[whirlwind]

# Master switch for the whole plugin.
#   true  - the parts below are installed as configured.
#   false - the DLL stays loaded and the console command still answers, but
#           nothing is installed.
# Default: true
enabled = true


[ctc_while_uninterruptible]

# Vanilla refuses every item proc while the caster is in the Uninterruptible
# state (Whirlwind, and any other skill that sets it). With this on, procs
# fire. This is global: it applies to every Uninterruptible caster, not only
# Whirlwind.
# Sites: RVA 00589736 (unit-target procs) and RVA 0058986B (position procs).
# Default: true
enabled = true

# Chance, in percent, that a proc which already passed its own roll is let
# through while the caster is Uninterruptible. 100 lets every such proc
# through. 0 blocks them all. Values above 100 count as 100.
# Default: 100
chance_percent = 100

# Skill ids allowed to proc while the caster is Uninterruptible. Any skill not
# in the list is blocked while Uninterruptible. An empty list means no filter:
# every skill is allowed (the chance above still applies).
# Example: allowed_skills = [ 56, 59, 64 ]
# Default: []
allowed_skills = []


[proc_start_fix]

# Some procs run the full client skill start. That start writes the proc skill
# into the caster's used-skill slot and switches the caster's animation to the
# proc's mode, which the server never does for a proc. A Whirlwind in progress
# then loses its client-side owner, never gets cleaned up, and every later
# Whirlwind is refused. With this on, a proc keeps the caster's used skill and
# animation mode exactly as they were.
# Sites: RVA 0023123A, 002184FE, 00218586, 00231334 (client).
# Default: true
enabled = true


[dual_wield_same_target]

# When dual wielding, Whirlwind swings both weapons each hit. Vanilla makes the
# second weapon look for the next target after the one the first weapon just
# struck, so with two or more enemies in range the weapons never strike the
# same enemy. With this on, the second weapon may strike the same enemy (or the
# next one if the first swing killed it). Rotation across enemies from one hit
# to the next is unchanged.
# Site: RVA 005694B7 (server).
# Default: true
enabled = true


[cyclone]

# Whirlwind follows the cursor while the button is held and ends when it is
# released. A single click gives a Whirlwind that lasts only the grace window
# below. Holding the button never ends the Whirlwind by itself: when the
# character reaches the point it was heading for it keeps spinning until the
# cursor gives it somewhere new to go.
# Steering never sets a movement speed of its own. The character moves at the
# speed the game gives a Whirlwinding unit: walk speed scaled by faster
# run/walk and by slows and haste, updated by the game whenever those change.
# The one time steering asks for a speed is when a new route starts from a
# standstill, because arriving at a point stops the path; it then runs the
# game's own movement speed function (RVA 00350B40), the same one the game
# runs itself.
# Default: true
enabled = true

# How long, in milliseconds, Whirlwind keeps going after the last held-button
# update. This is what "released" means: once the button is up, updates stop,
# and the Whirlwind ends this long after the last one. Movement stops at that
# moment; the spin animation finishes its current turn, like a vanilla
# Whirlwind ending. Keep it short. Too short can end a held Whirlwind during a
# frame hitch.
# Default: 120
hold_grace_ms = 120

# What to do when the cursor is on or very close to the character.
#   "keep_heading"  - keep moving in the last direction the cursor gave.
#   "spin_in_place" - stop moving and spin until the cursor moves away.
# Default: "keep_heading"
near_cursor = "keep_heading"

# How close, in subtiles, counts as "very close" for near_cursor.
# Default: 3
near_cursor_radius = 3

# While the button is held, Whirlwind pays its normal cost again every this
# many game frames (25 frames is one second). Mana skills pay mana, skills cast
# from charges use a charge. If the cost cannot be paid, Whirlwind ends. 0
# turns the recurring cost off, so only the first cast pays.
# Default: 25
mana_interval_frames = 25

# Vanilla Whirlwind turns into a normal attack when it is aimed at a target
# that is already in melee range. With this on, Whirlwind always spins.
# Sites: RVA 0056A4E1 (server), RVA 00230B20 (client).
# Default: true
spin_at_melee_targets = true


[death_while_whirlwinding]

# Whirlwind makes the character Uninterruptible, and the game never kills an
# Uninterruptible character on the spot: a lethal hit only marks the death as
# pending (state 92), and the death happens when the Uninterruptible action
# ends. With a cyclone Whirlwind that end might never come. With this on, the
# frame a death becomes pending the Whirlwind ends, and the game's own
# pending-death code kills the character right there, with the normal
# Whirlwind teardown.
# Site: shares the redirect at RVA 0042A99F (server).
# Default: true
enabled = true


[potions_while_whirlwinding]

# Vanilla blocks item use while the character is Uninterruptible, in three
# places. The client's item-use lock refuses the belt keys, inventory use and
# every other use path before anything is sent; the server then ignores the
# use message (0x26, belt and inventory alike) in its message dispatcher, and
# refuses it again in the use-item handler. With this on, using a potion is let
# through all three while the character is Whirlwinding. Every other item stays
# blocked exactly as before.
# "Potion" is item type poti (itemtypes.txt row 9) and every type that
# inherits from it.
# Sites: RVA 001C7360, 001C738C (client);
#        RVA 004F2FA0, 004F306E, 004F40C0, 004F4140 (server).
# Default: true
enabled = true


[whirlwind_with_bows]

# Player skill sequences are compiled into the game, one entry per weapon
# class, and Whirlwind's sequence has no entry for bows or crossbows. With a
# missile weapon the skill starts with no sequence to run: it spins for one
# tick, stops, and leaves the character stuck Uninterruptible. With this on,
# when the equipped weapon class has no entry, the sequence of the bare-handed
# (hth) entry is used instead. Whirlwind's hand-to-hand entry is the same
# 8-step sequence every melee weapon uses, so the hit timing is identical.
# This covers every sequence skill that lacks an entry for the equipped weapon
# class, not only Whirlwind, exactly like the 2.4 patch did. Sequences that do
# have an entry for the weapon are untouched. Client and server share the code.
# Site: RVA 003CB987 (sequence resolver 003CB890).
# Default: true
enabled = true


[whirlwind_speed_sync]

# The game moves a Whirlwinding character at walk speed scaled by faster
# run/walk and by slows and haste (velocitypercent). The client applies that
# the moment Whirlwind starts. The server start sets plain walk speed and only
# switches to the scaled speed when a buff or debuff makes the game recompute
# it. With any faster run/walk the two sides move at different speeds, drift
# apart, and the server keeps pulling the character back, which shows as
# stutter. The longer the Whirlwind, the worse it gets, so cyclone makes it
# very visible. With this on, the server runs the game's movement speed
# function right after a player's Whirlwind starts, exactly as the client
# start does, so both sides move at the same speed from the first frame.
# Players only. Monster Whirlwind is unchanged.
# Site: RVA 0056A420 (server Whirlwind start, srvstfunc 38).
# Default: true
enabled = true
)toml";

struct Config {
	bool enabled            = true;
	bool ctcEnabled         = true;
	int  ctcChancePercent   = 100;
	bool procStartFix       = true;
	bool dualWieldSameTarget = true;
	bool cycloneEnabled     = true;
	int  holdGraceMs        = 120;
	bool keepHeading        = true;
	int  nearCursorRadius   = 3;
	int  manaIntervalFrames = 25;
	bool spinAtMeleeTargets = true;
	bool deathWhileWhirlwinding = true;
	bool potionsWhileWhirlwinding = true;
	bool whirlwindWithBows = true;
	bool whirlwindSpeedSync = true;
};

Config                      g_config {};
std::array<bool, 4096>      g_ctcAllowed {};
bool                        g_ctcAllowListActive = false;
std::uint32_t               g_serverGraceFrames  = 5;

// Finds `key` inside `[section]` and returns a pointer to the first
// non-blank character after '=', or nullptr. Comments (#) are skipped.
auto FindValue(const char* toml, const char* section, const char* key) noexcept -> const char* {
	const std::size_t sectionLength = std::strlen(section);
	const std::size_t keyLength     = std::strlen(key);
	bool              inSection     = false;
	for (const char* line = toml; line != nullptr && *line != '\0';) {
		const char* next = std::strchr(line, '\n');
		const char* end  = next != nullptr ? next : line + std::strlen(line);
		const char* p    = line;
		while (p < end && (*p == ' ' || *p == '\t')) {
			++p;
		}
		if (p < end && *p == '[') {
			const char* close = static_cast<const char*>(std::memchr(p, ']', static_cast<std::size_t>(end - p)));
			inSection = close != nullptr && static_cast<std::size_t>(close - p - 1) == sectionLength && std::strncmp(p + 1, section, sectionLength) == 0;
		} else if (inSection && p < end && *p != '#' && static_cast<std::size_t>(end - p) > keyLength && std::strncmp(p, key, keyLength) == 0) {
			const char* q = p + keyLength;
			while (q < end && (*q == ' ' || *q == '\t')) {
				++q;
			}
			if (q < end && *q == '=') {
				++q;
				while (q < end && (*q == ' ' || *q == '\t')) {
					++q;
				}
				return q < end ? q : nullptr;
			}
		}
		line = next != nullptr ? next + 1 : nullptr;
	}
	return nullptr;
}

void ReadBool(const char* toml, const char* section, const char* key, bool& value) noexcept {
	if (const char* v = FindValue(toml, section, key); v != nullptr) {
		if (std::strncmp(v, "true", 4) == 0) {
			value = true;
		} else if (std::strncmp(v, "false", 5) == 0) {
			value = false;
		}
	}
}

void ReadInt(const char* toml, const char* section, const char* key, int& value, int minimum, int maximum) noexcept {
	if (const char* v = FindValue(toml, section, key); v != nullptr) {
		char*      end    = nullptr;
		const long parsed = std::strtol(v, &end, 10);
		if (end != v) {
			value = static_cast<int>(parsed < minimum ? minimum : (parsed > maximum ? maximum : parsed));
		}
	}
}

void ReadNearCursor(const char* toml, bool& keepHeading) noexcept {
	if (const char* v = FindValue(toml, "cyclone", "near_cursor"); v != nullptr) {
		if (std::strncmp(v, "\"spin_in_place\"", 15) == 0) {
			keepHeading = false;
		} else if (std::strncmp(v, "\"keep_heading\"", 14) == 0) {
			keepHeading = true;
		}
	}
}

void ReadAllowList(const char* toml) noexcept {
	g_ctcAllowed.fill(false);
	g_ctcAllowListActive = false;
	const char* v        = FindValue(toml, "ctc_while_uninterruptible", "allowed_skills");
	if (v == nullptr || *v != '[') {
		return;
	}
	for (const char* p = v + 1; *p != '\0' && *p != ']' && *p != '\n';) {
		if (*p >= '0' && *p <= '9') {
			char*      end = nullptr;
			const long id  = std::strtol(p, &end, 10);
			if (id >= 0 && id < static_cast<long>(g_ctcAllowed.size())) {
				g_ctcAllowed[static_cast<std::size_t>(id)] = true;
				g_ctcAllowListActive                       = true;
			}
			p = end;
		} else {
			++p;
		}
	}
}

void LoadConfiguration(const D2RL::PluginContext* context) noexcept {
	if (!context->EnsureConfig(DefaultConfigToml)) {
		context->LogWarn("Could not create or open celestialrayone.whirlwind.toml. Built-in defaults are used.");
	}
	static std::array<char, 16384> buffer {};
	std::uint32_t                  required = 0;
	if (!context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size() - 1), &required)) {
		context->LogWarn("Could not read celestialrayone.whirlwind.toml. Built-in defaults are used.");
		ReadAllowList("");
	} else {
		buffer.back() = '\0';
		const char* t = buffer.data();
		ReadBool(t, "whirlwind", "enabled", g_config.enabled);
		ReadBool(t, "ctc_while_uninterruptible", "enabled", g_config.ctcEnabled);
		ReadInt(t, "ctc_while_uninterruptible", "chance_percent", g_config.ctcChancePercent, 0, 100);
		ReadAllowList(t);
		ReadBool(t, "proc_start_fix", "enabled", g_config.procStartFix);
		ReadBool(t, "dual_wield_same_target", "enabled", g_config.dualWieldSameTarget);
		ReadBool(t, "cyclone", "enabled", g_config.cycloneEnabled);
		ReadInt(t, "cyclone", "hold_grace_ms", g_config.holdGraceMs, 40, 2000);
		ReadNearCursor(t, g_config.keepHeading);
		ReadInt(t, "cyclone", "near_cursor_radius", g_config.nearCursorRadius, 1, 20);
		ReadInt(t, "cyclone", "mana_interval_frames", g_config.manaIntervalFrames, 0, 1000);
		ReadBool(t, "cyclone", "spin_at_melee_targets", g_config.spinAtMeleeTargets);
		ReadBool(t, "death_while_whirlwinding", "enabled", g_config.deathWhileWhirlwinding);
		ReadBool(t, "potions_while_whirlwinding", "enabled", g_config.potionsWhileWhirlwinding);
		ReadBool(t, "whirlwind_with_bows", "enabled", g_config.whirlwindWithBows);
		ReadBool(t, "whirlwind_speed_sync", "enabled", g_config.whirlwindSpeedSync);
	}
	// The server keeps a Whirlwind alive a little longer than the client does,
	// so a client aim can never reach a server whose Whirlwind already ended.
	g_serverGraceFrames = static_cast<std::uint32_t>((g_config.holdGraceMs + 39) / 40 + 2);
	D2RL::LogInfoF(
		context,
		"config: enabled=%d ctc=%d chance=%d allow_list=%d proc_fix=%d dual_wield=%d cyclone=%d grace_ms=%d keep_heading=%d radius=%d mana_frames=%d spin_melee=%d death=%d potions=%d bows=%d speed_sync=%d",
		g_config.enabled, g_config.ctcEnabled, g_config.ctcChancePercent, g_ctcAllowListActive, g_config.procStartFix,
		g_config.dualWieldSameTarget, g_config.cycloneEnabled, g_config.holdGraceMs, g_config.keepHeading,
		g_config.nearCursorRadius, g_config.manaIntervalFrames, g_config.spinAtMeleeTargets, g_config.deathWhileWhirlwinding, g_config.potionsWhileWhirlwinding, g_config.whirlwindWithBows, g_config.whirlwindSpeedSync);
}

// ---------------------------------------------------------------------------
// Verification and relay page
// ---------------------------------------------------------------------------

template <std::size_t N>
auto VerifyAll(const D2RL::PluginContext* context, const Witness (&list)[N], const char* part) noexcept -> bool {
	for (const Witness& w : list) {
		if (!context->CheckExpectedBytes(w.rva, w.bytes, w.size)) {
			D2RL::LogErrorF(context, "%s NOT installed: bytes at RVA %08llX (%s) do not match 3.3.93847 under D2RLoader 1.3.0.", part,
				static_cast<unsigned long long>(w.rva), w.what);
			return false;
		}
	}
	return true;
}

constexpr std::size_t RelayPageBytes = 4096;
constexpr std::size_t RelayServerPathStatus = 0x000;
constexpr std::size_t RelayClientPathStep   = 0x020;
constexpr std::size_t RelayProcStart        = 0x040;
constexpr std::size_t RelayModeCheck        = 0x060;
constexpr std::size_t RelaySetNeutral       = 0x080;
constexpr std::size_t RelayProcTail         = 0x0A0;
constexpr std::size_t RelayDualWield        = 0x0C0;
constexpr std::size_t RelayDispatchState    = 0x0E0;
constexpr std::size_t RelayUseItemState     = 0x100;
constexpr std::size_t RelayItemLockState    = 0x120;
constexpr std::size_t RelaySequenceFallback = 0x140;

std::uint8_t* g_relayPage = nullptr;

auto WithinRel32(std::uintptr_t next, std::uintptr_t target) noexcept -> bool {
	const std::int64_t delta = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(next);
	return delta >= INT32_MIN && delta <= INT32_MAX;
}

auto AllocateNear(std::uintptr_t hint, std::size_t size) noexcept -> std::uint8_t* {
	SYSTEM_INFO info {};
	GetSystemInfo(&info);
	const auto granularity = static_cast<std::uintptr_t>(info.dwAllocationGranularity);
	const auto aligned     = hint & ~(granularity - 1U);
	for (std::uintptr_t delta = granularity; delta < 0x70000000ULL; delta += granularity) {
		const auto candidate = aligned + delta;
		if (!WithinRel32(hint, candidate + size)) {
			break;
		}
		if (auto* memory = VirtualAlloc(reinterpret_cast<void*>(candidate), size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) {
			return static_cast<std::uint8_t*>(memory);
		}
	}
	return nullptr;
}

void WriteAbsoluteJump(std::uint8_t* at, std::uint64_t target) noexcept {
	static constexpr std::uint8_t JumpQwordRip[] { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
	std::memcpy(at, JumpQwordRip, sizeof(JumpQwordRip));
	std::memcpy(at + sizeof(JumpQwordRip), &target, sizeof(target));
}

// ---------------------------------------------------------------------------
// Game layout used by the handlers. Each offset is confirmed by the verified
// bytes above or by the decompiled function named next to it.
// ---------------------------------------------------------------------------

struct PathPoint {
	std::uint16_t x;
	std::uint16_t y;
};

// Command handed to the held-button handlers 00101FA0 / 001013B0 (layout from
// 00100BB0 and 00102490).
struct SkillCommand {
	std::uint32_t flags;          // 1 left, 2 right, 4 press, 8 held, 0x10 release
	std::uint32_t pad04;
	void*         player;         // +0x08
	void*         hovered;        // +0x10
	std::int32_t  x;              // +0x18 target subtile x
	std::int32_t  y;              // +0x1C target subtile y
	std::int32_t  locationOpcode; // +0x20
	std::int32_t  unitOpcode;     // +0x24
	void*         skill;          // +0x28
};
static_assert(offsetof(SkillCommand, player) == 0x08);
static_assert(offsetof(SkillCommand, x) == 0x18);
static_assert(offsetof(SkillCommand, skill) == 0x28);

constexpr std::uint32_t CommandHeld          = 0x08;
constexpr std::int16_t  WhirlwindSrvDoFunc   = 76;
constexpr int           StateUninterruptable = 54;
constexpr int           StateDeathDelay      = 92;   // 0048E820 / 0044AA57
constexpr std::uint8_t  PacketUseItem        = 0x26; // 004C13B0 -> 004F40C0
constexpr std::int32_t  UnitTypeItem         = 4;    // 004F4164: lea edx,[rbx+4]
constexpr std::int32_t  ItemTypePotion       = 9;    // itemtypes.txt row 9 (poti)
constexpr std::uint32_t TokenActive          = 0x01;
constexpr std::uint32_t TokenFinishing       = 0x02;
constexpr int           MaskPathfind         = 0x0C01; // 0056A540
constexpr int           MaskMoveTest         = 0x0401; // 0056A5CC
constexpr int           CollisionPlayer      = 0x80;   // 0056A62F: (1 xor 1)+1 << 7
constexpr std::uint64_t ClientAimIntervalMs  = 40;     // one game frame
constexpr int           MaxAimDistance       = 40;     // server accepts up to 50 (004FE1F0)

inline auto U32(void* object, std::size_t offset) noexcept -> std::uint32_t& {
	return *static_cast<std::uint32_t*>(static_cast<void*>(static_cast<std::uint8_t*>(object) + offset));
}
inline auto UnitType(void* unit) noexcept -> std::uint32_t { return U32(unit, 0x00); }         // 002310B0 et al: *unit == 2
inline auto GameFrame(void* game) noexcept -> std::uint32_t { return U32(game, 0x170); }       // 0056A84C: game+368
inline auto GameDataContext(void* game) noexcept -> std::uint8_t { return static_cast<std::uint8_t*>(game)[0x106]; } // 0043B427
inline auto SkillRecord(void* skill) noexcept -> void* { return *static_cast<void**>(skill); } // 0033CBC0: mov rax,[rbx]
inline auto RecordSkillId(void* record) noexcept -> std::int16_t { return *static_cast<std::int16_t*>(record); }
inline auto RecordSrvDoFunc(void* record) noexcept -> std::int16_t { return static_cast<std::int16_t*>(record)[39]; } // +78, 0043AED3
inline auto TokenDestX(void* skill) noexcept -> std::uint32_t& { return U32(skill, 0x24); }    // 0056A672
inline auto TokenDestY(void* skill) noexcept -> std::uint32_t& { return U32(skill, 0x28); }    // 0056A678
inline auto UnitCastId(void* unit) noexcept -> std::uint32_t& { return U32(unit, 0x12C); }     // 0034F430: unit+300
inline auto PathVelocity(void* path) noexcept -> std::uint32_t { return U32(path, 0xA0); }     // 00342B24: mov [rbx+0A0h],edi

// Native functions (RVA noted; each is proven by a witness).
struct Native {
	void* (*getUsedSkill)(void*)                                        = nullptr; // 0034BA40
	std::uint32_t (*getSkillFlags)(void*)                               = nullptr; // 0033CC00
	void (*setSkillFlags)(void*, std::uint32_t)                         = nullptr; // 0033EC20
	void* (*getLeftSkill)(void*)                                        = nullptr; // 0034A540
	void* (*getRightSkill)(void*)                                       = nullptr; // 0034B400
	void (*setUsedSkill)(void*, void*, std::uint32_t)                   = nullptr; // 0034F430
	std::int32_t (*checkState)(void*, std::int32_t)                     = nullptr; // 003351B0
	void* (*getDynamicPath)(void*)                                      = nullptr; // 0034AE80
	void* (*getRoom)(void*)                                             = nullptr; // 0034B440
	std::int32_t (*collisionPattern)(void*)                             = nullptr; // 00341870
	void (*collisionReset)(void*, std::int32_t, std::int32_t, std::int32_t, std::int32_t) = nullptr; // 003636D0
	void (*collisionSet)(void*, std::int32_t, std::int32_t, std::int32_t, std::int32_t)   = nullptr; // 00363CF0
	void (*pathSetTarget)(void*, std::int32_t, std::int32_t)            = nullptr; // 00342A50
	void (*pathSetMoveMask)(void*, std::int32_t)                        = nullptr; // 00342740
	std::int32_t (*playerPathType)()                                    = nullptr; // 003412E0
	void (*pathSetType)(void*, std::int32_t)                            = nullptr; // 00342A70
	std::int32_t (*pathGetPoints)(void*, PathPoint**)                   = nullptr; // 00341AE0
	std::int32_t (*pathGetX)(void*)                                     = nullptr; // 00341A20
	std::int32_t (*pathGetY)(void*)                                     = nullptr; // 00341A30
	std::int32_t (*serverPathCompute)(void*, void*, std::int32_t, std::int32_t) = nullptr; // 00340E70
	// 00350B40, the game's movement speed function (UNITS_UpdateAnimRateAndVelocity).
	void (*updateAnimRateAndVelocity)(void*)                            = nullptr; // 00350B40
	// 00436830, a D2RLoader jump to D2RCore ConsumeWideSkillResource. The only
	// native caller (0043B5B4) ignores the result, so the plugin does too. Id
	// and level travel as full-width registers so D2RCore sees clean values
	// whichever width it reads.
	void (*manaConsume)(void*, void*, std::uint64_t, std::uint64_t)     = nullptr; // 00436830
	std::int32_t (*manaCost)(std::uint8_t, std::int32_t, std::int32_t) = nullptr; // 0033AA00
	// 0033D1E0, a D2RLoader jump to D2RCore ReadWideSkillLevel; same call shape
	// as the native cast gate (unit, skill, 1, 0).
	std::int32_t (*skillLevel)(void*, void*, std::uint64_t, std::uint64_t) = nullptr; // 0033D1E0
	// 00436750, the native cast gate: can the unit pay for its used skill.
	std::int32_t (*canPayUsedSkill)(void*)                              = nullptr; // 00436750
	std::uint8_t (*clientPathCompute)(void*, void*)                     = nullptr; // 00215EF0
	std::int64_t (*sendCommand)(std::int32_t, void*, std::int32_t, std::int32_t) = nullptr; // 000FC000
	std::int32_t (*localPlayerIndex)()                                  = nullptr; // 0008B2D0
	void* (*playerFromIndex)(std::int32_t)                              = nullptr; // 0009A480
	void* (*getServerUnit)(void*, std::int32_t, std::uint32_t)          = nullptr; // 0048FE80
	std::int32_t (*checkItemTypeId)(void*, std::int32_t)                = nullptr; // 00373890
	// Originals of redirected calls.
	std::int32_t (*serverPathStatus)(void*, void*)                      = nullptr; // 00491AC0
	std::int32_t (*clientPathStep)(void*, std::int32_t)                 = nullptr; // 000F9A00
	std::int32_t (*procStart)(void*, std::int32_t, std::int32_t, std::int32_t, std::int32_t) = nullptr; // 00217640
	std::uint8_t (*modeCheck)(void*, std::int32_t)                      = nullptr; // 000F99B0
	std::int64_t (*setNeutral)(void*, std::int32_t)                     = nullptr; // 000F9860
	std::int64_t (*procTail)(void*, std::int32_t)                       = nullptr; // 0034E790
};

Native g_native {};

template <typename Fn>
void Bind(Fn& fn, std::uintptr_t base, std::uint64_t rva) noexcept {
	fn = reinterpret_cast<Fn>(base + rva);
}

void BindNatives(std::uintptr_t base) noexcept {
	Bind(g_native.getUsedSkill, base, 0x34BA40);
	Bind(g_native.getSkillFlags, base, 0x33CC00);
	Bind(g_native.setSkillFlags, base, 0x33EC20);
	Bind(g_native.getLeftSkill, base, 0x34A540);
	Bind(g_native.getRightSkill, base, 0x34B400);
	Bind(g_native.setUsedSkill, base, 0x34F430);
	Bind(g_native.checkState, base, 0x3351B0);
	Bind(g_native.getDynamicPath, base, 0x34AE80);
	Bind(g_native.getRoom, base, 0x34B440);
	Bind(g_native.collisionPattern, base, 0x341870);
	Bind(g_native.collisionReset, base, 0x3636D0);
	Bind(g_native.collisionSet, base, 0x363CF0);
	Bind(g_native.pathSetTarget, base, 0x342A50);
	Bind(g_native.pathSetMoveMask, base, 0x342740);
	Bind(g_native.playerPathType, base, 0x3412E0);
	Bind(g_native.pathSetType, base, 0x342A70);
	Bind(g_native.pathGetPoints, base, 0x341AE0);
	Bind(g_native.pathGetX, base, 0x341A20);
	Bind(g_native.pathGetY, base, 0x341A30);
	Bind(g_native.serverPathCompute, base, 0x340E70);
	Bind(g_native.updateAnimRateAndVelocity, base, 0x350B40);
	Bind(g_native.manaConsume, base, 0x436830);
	Bind(g_native.manaCost, base, 0x33AA00);
	Bind(g_native.skillLevel, base, 0x33D1E0);
	Bind(g_native.canPayUsedSkill, base, 0x436750);
	Bind(g_native.clientPathCompute, base, 0x215EF0);
	Bind(g_native.sendCommand, base, 0x0FC000);
	Bind(g_native.localPlayerIndex, base, 0x08B2D0);
	Bind(g_native.playerFromIndex, base, 0x09A480);
	Bind(g_native.getServerUnit, base, 0x48FE80);
	Bind(g_native.checkItemTypeId, base, 0x373890);
	Bind(g_native.serverPathStatus, base, 0x491AC0);
	Bind(g_native.clientPathStep, base, 0x0F9A00);
	Bind(g_native.procStart, base, 0x217640);
	Bind(g_native.modeCheck, base, 0x0F99B0);
	Bind(g_native.setNeutral, base, 0x0F9860);
	Bind(g_native.procTail, base, 0x34E790);
}

auto IsWhirlwindSkill(void* skill) noexcept -> bool {
	if (skill == nullptr) {
		return false;
	}
	void* record = SkillRecord(skill);
	return record != nullptr && RecordSrvDoFunc(record) == WhirlwindSrvDoFunc;
}

// The same endpoint bookkeeping the native starts do (0056A5E0..0056A678 on the
// server, 00230CBC..00230D6E on the client): release the old destination's
// collision reservation, reserve the new one, store it in the token.
void FinishRetarget(void* unit, void* skill, void* path, void* room, std::int32_t pattern) noexcept {
	g_native.pathSetMoveMask(path, MaskMoveTest);
	std::int32_t endX   = 0;
	std::int32_t endY   = 0;
	PathPoint*   points = nullptr;
	const std::int32_t count = g_native.pathGetPoints(path, &points);
	if (count > 0 && points != nullptr) {
		endX = points[count - 1].x;
		endY = points[count - 1].y;
	}
	if (endX == 0 || endY == 0) {
		// No route (aim inside a wall, or on the character): the character
		// stays where it is and keeps spinning. The token must never hold a
		// zero destination, the tick treats that as "remove Whirlwind".
		endX = g_native.pathGetX(path);
		endY = g_native.pathGetY(path);
	}
	if (room != nullptr && endX != 0 && endY != 0) {
		g_native.collisionSet(room, endX, endY, pattern, CollisionPlayer);
	}
	if (endX != 0 && endY != 0) {
		TokenDestX(skill) = static_cast<std::uint32_t>(endX);
		TokenDestY(skill) = static_cast<std::uint32_t>(endY);
	}
	(void)unit;
}

auto BeginRetarget(void* unit, void* skill, std::int32_t x, std::int32_t y, void*& path, void*& room, std::int32_t& pattern) noexcept -> bool {
	path = g_native.getDynamicPath(unit);
	if (path == nullptr) {
		return false;
	}
	room    = g_native.getRoom(unit);
	pattern = g_native.collisionPattern(unit);
	const std::uint32_t oldX = TokenDestX(skill);
	const std::uint32_t oldY = TokenDestY(skill);
	if (room != nullptr && oldX != 0 && oldY != 0) {
		g_native.collisionReset(room, static_cast<std::int32_t>(oldX), static_cast<std::int32_t>(oldY), pattern, CollisionPlayer);
	}
	g_native.pathSetTarget(path, x, y);
	g_native.pathSetMoveMask(path, MaskPathfind);
	g_native.pathSetType(path, g_native.playerPathType());
	return true;
}

// Movement speed after a retarget. The path compute keeps the path's velocity
// as it is, but arriving at the last point of a route zeroes it (00380FD0
// writes 0 to path+A0h when the unit sits on the centre of its final point),
// which is why the native starts write a velocity after their compute. A
// retarget never invents one: the velocity the game set stays, including
// every change the game made for faster run/walk, slows and haste. Only a
// route that starts from a standstill asks the game's movement speed function
// 00350B40 for it. For a player in mode 18 whose used skill has
// (flags & 0x1001) == 1, which a running Whirlwind always has, that function
// sets walk speed scaled by faster run/walk and velocitypercent, the same
// value the native client start gets from it (00230D75 -> 00214E30). So
// client and server always agree.
void RestoreMovementSpeed(void* unit, void* path) noexcept {
	if (PathVelocity(path) == 0) {
		g_native.updateAnimRateAndVelocity(unit);
	}
}

void ServerRetarget(void* unit, void* skill, std::int32_t x, std::int32_t y) noexcept {
	void*        path    = nullptr;
	void*        room    = nullptr;
	std::int32_t pattern = 0;
	if (!BeginRetarget(unit, skill, x, y, path, room, pattern)) {
		return;
	}
	if (g_native.serverPathCompute(path, unit, 0, 1) != 0) {
		RestoreMovementSpeed(unit, path);
	}
	FinishRetarget(unit, skill, path, room, pattern);
}

void ClientRetarget(void* unit, void* skill, std::int32_t x, std::int32_t y) noexcept {
	void*        path    = nullptr;
	void*        room    = nullptr;
	std::int32_t pattern = 0;
	if (!BeginRetarget(unit, skill, x, y, path, room, pattern)) {
		return;
	}
	if (g_native.clientPathCompute(unit, path) != 0) {
		RestoreMovementSpeed(unit, path);
	}
	FinishRetarget(unit, skill, path, room, pattern);
}

// ---------------------------------------------------------------------------
// Cyclone: server
// ---------------------------------------------------------------------------

struct ServerSession {
	void*         unit            = nullptr;
	std::uint32_t lastSeenFrame   = 0;
	std::uint32_t lastAimFrame    = 0;
	std::uint32_t nextChargeFrame = 0;
};

std::array<ServerSession, 32> g_serverSessions {};

auto ServerSessionFor(void* unit, std::uint32_t frame) noexcept -> ServerSession& {
	ServerSession* freeSlot = nullptr;
	for (ServerSession& s : g_serverSessions) {
		if (s.unit == unit) {
			if (frame - s.lastSeenFrame <= 1) {
				return s;
			}
			freeSlot = &s;
			break;
		}
		if (freeSlot == nullptr && (s.unit == nullptr || frame - s.lastSeenFrame > 250)) {
			freeSlot = &s;
		}
	}
	if (freeSlot == nullptr) {
		freeSlot = &g_serverSessions[0];
	}
	// A new Whirlwind: its first frame counts as the first held update.
	freeSlot->unit            = unit;
	freeSlot->lastSeenFrame   = frame;
	freeSlot->lastAimFrame    = frame;
	freeSlot->nextChargeFrame = frame + static_cast<std::uint32_t>(g_config.manaIntervalFrames);
	return *freeSlot;
}

void EndServerSession(ServerSession& s) noexcept {
	s.unit = nullptr;
}

// Pays one interval the way the native skill start pays a cast (0043B3E0):
// the cast gate 00436750 decides whether the used skill can be paid for, then
// the resource payment 00436830 pays it and its result is not used. The gate
// is the check every cast of this skill faces (mana, or charges for a skill
// cast from charges), so a held Whirlwind ends exactly when a fresh cast would
// be refused. The caller has already made sure the used skill is Whirlwind.
auto ChargeWhirlwind(void* game, void* unit, void* skill) noexcept -> bool {
	void* record = SkillRecord(skill);
	if (record == nullptr) {
		return true;
	}
	const std::int32_t skillId = RecordSkillId(record);
	const std::int32_t level   = g_native.skillLevel(unit, skill, 1, 0);
	if (skillId < 0 || level <= 0) {
		return true;
	}
	if (g_native.manaCost(GameDataContext(game), skillId, level) <= 0) {
		return true; // a skill with no cost is never drained
	}
	if (g_native.canPayUsedSkill(unit) == 0) {
		return false;
	}
	g_native.manaConsume(game, unit, static_cast<std::uint64_t>(skillId), static_cast<std::uint64_t>(level));
	return true;
}

// Replaces the call to the path-status query at 0042A99F inside the sequence
// frame handler 0042A940. Status 2 means "path finished": the handler then
// requests the finish (token bit1) and runs the skill's do-function, which
// removes Whirlwind. While the button is held, a finished path is reported as
// still running, so the character spins where it stands until the next aim.
// When the hold lapses, or the recurring cost cannot be paid, status 2 is
// reported so the native finish ends Whirlwind at once.
bool g_cycloneServerActive = false;
bool g_deathFixActive      = false;

void ForgetServerSession(void* unit) noexcept {
	for (ServerSession& s : g_serverSessions) {
		if (s.unit == unit) {
			EndServerSession(s);
		}
	}
}

std::int32_t __fastcall OnServerPathStatus(void* game, void* unit) noexcept {
	const std::int32_t status = g_native.serverPathStatus(game, unit);
	if (game == nullptr || unit == nullptr || UnitType(unit) != 0) {
		return status;
	}
	void* const skill = g_native.getUsedSkill(unit);
	if (!IsWhirlwindSkill(skill)) {
		return status;
	}
	const std::uint32_t flags = g_native.getSkillFlags(skill);
	if ((flags & TokenActive) == 0 || (flags & TokenFinishing) != 0) {
		return status;
	}
	// A lethal hit on an Uninterruptible unit only marks the death as pending
	// (0044AA57 sets state 92); the death itself happens when Uninterruptible is
	// cleared (0048E820). Report the path finished: the native finish runs the
	// tick, RemoveWhirlwind 0056BB60 clears Uninterruptible, and the game's own
	// pending-death code kills the character in that same call.
	if (g_deathFixActive && g_native.checkState(unit, StateDeathDelay) != 0) {
		ForgetServerSession(unit);
		return 2;
	}
	if (!g_cycloneServerActive) {
		return status;
	}
	const std::uint32_t frame = GameFrame(game);
	ServerSession&      s     = ServerSessionFor(unit, frame);
	s.lastSeenFrame           = frame;
	if (frame - s.lastAimFrame > g_serverGraceFrames) {
		EndServerSession(s);
		return 2;
	}
	if (g_config.manaIntervalFrames > 0 && frame >= s.nextChargeFrame) {
		s.nextChargeFrame = frame + static_cast<std::uint32_t>(g_config.manaIntervalFrames);
		if (!ChargeWhirlwind(game, unit, skill)) {
			EndServerSession(s);
			return 2;
		}
	}
	return status == 2 ? 1 : status;
}

using PositionExecutorFn = std::int32_t(__fastcall*)(void*, void*, std::uint16_t, std::uint16_t, std::int32_t, void*);
PositionExecutorFn g_originalPositionExecutor = nullptr;

// Entry hook on the server position executor 004FDB40, reached from the
// skill-on-location packets. A command for the Whirlwind that is currently
// running is the client's aim update: retarget the live path, renew the hold,
// and never pass it on, so the native code cannot queue it as a new cast.
// Returns 1, the executor's own "handled, nothing further" result.
std::int32_t __fastcall OnPositionExecutor(void* game, void* player, std::uint16_t x, std::uint16_t y, std::int32_t argument, void* activeSkill) noexcept {
	if (g_cycloneServerActive && game != nullptr && player != nullptr && activeSkill != nullptr && UnitType(player) == 0) {
		void* const used = g_native.getUsedSkill(player);
		if (used == activeSkill && IsWhirlwindSkill(used)) {
			const std::uint32_t flags = g_native.getSkillFlags(used);
			if ((flags & TokenActive) != 0) {
				if ((flags & TokenFinishing) == 0) {
					const std::uint32_t frame = GameFrame(game);
					ServerSession&      s     = ServerSessionFor(player, frame);
					s.lastAimFrame            = frame;
					ServerRetarget(player, used, x, y);
				}
				return 1;
			}
		}
	}
	return g_originalPositionExecutor(game, player, x, y, argument, activeSkill);
}

// ---------------------------------------------------------------------------
// Whirlwind speed sync (server start)
// ---------------------------------------------------------------------------

using WhirlwindStartFn = std::int64_t(__fastcall*)(void*, void*, std::uint32_t, std::uint32_t);
WhirlwindStartFn g_originalWhirlwindStart = nullptr;

// Entry hook on the server Whirlwind start 0056A420 (srvstfunc 38). Its path
// block sets plain walk speed (00438180 at 0056A5B0, 00342AE0 at 0056A5C7)
// before it sets the token active (0033EC20 at 0056A668), and nothing after that runs
// the movement speed function 00350B40 unless the skill has an aura state
// (00574480). The server's mode 18 start 0042A8B0 runs it too early, through
// 004919C0, while the token is still inactive, so its velocity branch is
// skipped. The client start 00230A10 sets the token active first and then
// runs the function through 00214E30, so the client moves at walk speed scaled
// by faster run/walk and velocitypercent while the server moves at plain walk
// speed. After a start that left a player's Whirlwind active, run the same
// function the client ran, at the same point in the start.
std::int64_t __fastcall OnServerWhirlwindStart(void* game, void* unit, std::uint32_t skillId, std::uint32_t skillLevel) noexcept {
	const std::int64_t result = g_originalWhirlwindStart(game, unit, skillId, skillLevel);
	if (static_cast<std::uint32_t>(result) != 0 && unit != nullptr && UnitType(unit) == 0) {
		void* const used = g_native.getUsedSkill(unit);
		if (IsWhirlwindSkill(used) && (g_native.getSkillFlags(used) & TokenActive) != 0) {
			g_native.updateAnimRateAndVelocity(unit);
		}
	}
	return result;
}

// ---------------------------------------------------------------------------
// Cyclone: client (local player only)
// ---------------------------------------------------------------------------

struct ClientSession {
	void*         unit        = nullptr;
	std::uint64_t lastSeenMs  = 0;
	std::uint64_t lastAimMs   = 0;
	std::uint64_t lastSendMs  = 0;
	float         headingX    = 0.0F;
	float         headingY    = 0.0F;
	bool          haveHeading = false;
};

ClientSession g_client {};

auto ClientSessionFor(void* unit, std::uint64_t now) noexcept -> ClientSession& {
	if (g_client.unit != unit || now - g_client.lastSeenMs > 90) {
		g_client             = {};
		g_client.unit        = unit;
		g_client.lastSeenMs  = now;
		g_client.lastAimMs   = now;
	}
	return g_client;
}

auto LocalPlayer() noexcept -> void* {
	return g_native.playerFromIndex(g_native.localPlayerIndex());
}

// Replaces the path step at 000FFCFD inside the client unit update 000FFBB0.
// A nonzero result makes the update request the finish and run the skill's
// client do-function, the same thing a vanilla Whirlwind reaching its end
// does. Mirrors the server rule: finished paths keep spinning while held,
// and the lapse of the hold reports "finished".
std::int32_t __fastcall OnClientPathStep(void* unit, std::int32_t flag) noexcept {
	const std::int32_t finished = g_native.clientPathStep(unit, flag);
	if (unit == nullptr || UnitType(unit) != 0 || unit != LocalPlayer()) {
		return finished;
	}
	void* const skill = g_native.getUsedSkill(unit);
	if (!IsWhirlwindSkill(skill)) {
		return finished;
	}
	const std::uint32_t flags = g_native.getSkillFlags(skill);
	if ((flags & TokenActive) == 0 || (flags & TokenFinishing) != 0) {
		return finished;
	}
	const std::uint64_t now = GetTickCount64();
	ClientSession&      s   = ClientSessionFor(unit, now);
	s.lastSeenMs            = now;
	if (now - s.lastAimMs > static_cast<std::uint64_t>(g_config.holdGraceMs)) {
		s.unit = nullptr;
		return 1;
	}
	return 0;
}

auto HandleCycloneAim(SkillCommand* command, void* skill) noexcept -> bool {
	if (command == nullptr || skill == nullptr || (command->flags & CommandHeld) == 0) {
		return false;
	}
	void* const player = command->player;
	if (player == nullptr || UnitType(player) != 0) {
		return false;
	}
	void* const used = g_native.getUsedSkill(player);
	if (used != skill || !IsWhirlwindSkill(used)) {
		return false;
	}
	const std::uint32_t flags = g_native.getSkillFlags(used);
	if ((flags & TokenActive) == 0) {
		return false;
	}
	const std::uint64_t now = GetTickCount64();
	ClientSession&      s   = ClientSessionFor(player, now);
	// A Whirlwind that is finishing, or whose hold already lapsed, is never
	// revived from here. The next held update after it ends starts a fresh
	// cast through the vanilla path, on both sides at once.
	if ((flags & TokenFinishing) != 0 || now - s.lastAimMs > static_cast<std::uint64_t>(g_config.holdGraceMs)) {
		return true;
	}
	s.lastAimMs = now;
	if (s.lastSendMs != 0 && now - s.lastSendMs < ClientAimIntervalMs) {
		return true;
	}

	void* const        path = g_native.getDynamicPath(player);
	if (path == nullptr) {
		return true;
	}
	const std::int32_t px = g_native.pathGetX(path);
	const std::int32_t py = g_native.pathGetY(path);
	std::int32_t       dx = command->x - px;
	std::int32_t       dy = command->y - py;
	const std::int32_t r  = g_config.nearCursorRadius;
	std::int32_t       ax = px;
	std::int32_t       ay = py;
	if (dx * dx + dy * dy > r * r) {
		const float length = std::sqrt(static_cast<float>(dx * dx + dy * dy));
		s.headingX         = static_cast<float>(dx) / length;
		s.headingY         = static_cast<float>(dy) / length;
		s.haveHeading      = true;
		if (length > static_cast<float>(MaxAimDistance)) {
			dx = static_cast<std::int32_t>(std::lround(s.headingX * static_cast<float>(MaxAimDistance)));
			dy = static_cast<std::int32_t>(std::lround(s.headingY * static_cast<float>(MaxAimDistance)));
		}
		ax = px + dx;
		ay = py + dy;
	} else if (g_config.keepHeading && s.haveHeading) {
		const float runway = static_cast<float>(r * 2 + 2 > 6 ? r * 2 + 2 : 6);
		ax                 = px + static_cast<std::int32_t>(std::lround(s.headingX * runway));
		ay                 = py + static_cast<std::int32_t>(std::lround(s.headingY * runway));
	}
	if (ax <= 0 || ay <= 0 || ax > 0xFFFF || ay > 0xFFFF) {
		return true;
	}

	ClientRetarget(player, used, ax, ay);
	// Plain skill-on-location for the slot that holds the skill. No pierce
	// counter bump (stat 328): both sides only bump it when a cast starts
	// (000FB004 client, 0042D39E server), and an aim update is not a cast.
	const std::int32_t opcode = used == g_native.getLeftSkill(player) ? 0x05 : 0x0C;
	g_native.sendCommand(opcode, player, ax, ay);
	s.lastSendMs = now;
	return true;
}

using HeldSkillFn = std::int64_t(__fastcall*)(SkillCommand*, void*);
using HeldLeftFn  = std::int64_t(__fastcall*)(SkillCommand*);
HeldSkillFn g_originalHeldSkill = nullptr;
HeldLeftFn  g_originalHeldLeft  = nullptr;

// Held right button (and left+0x400) handler 00101FA0. Vanilla asks the busy
// gate 0009A8C0 next, which refuses every command while Whirlwind runs.
std::int64_t __fastcall OnHeldSkillCommand(SkillCommand* command, void* skill) noexcept {
	if (HandleCycloneAim(command, skill)) {
		return 1;
	}
	return g_originalHeldSkill(command, skill);
}

// Held left button handler 001013B0; it resolves the left skill itself.
std::int64_t __fastcall OnHeldLeftCommand(SkillCommand* command) noexcept {
	if (command != nullptr && command->player != nullptr && HandleCycloneAim(command, g_native.getLeftSkill(command->player))) {
		return 1;
	}
	return g_originalHeldLeft(command);
}

// ---------------------------------------------------------------------------
// Proc start fix (client)
// ---------------------------------------------------------------------------

struct ProcScope {
	void*         unit      = nullptr;
	void*         usedSkill = nullptr;
	std::uint32_t castId    = 0;
	bool          open      = false;
};

ProcScope g_proc {};

// 0023123A: the client item-effect cast 002310B0 calls the full skill start.
std::int32_t __fastcall OnProcStart(void* unit, std::int32_t skillId, std::int32_t level, std::int32_t owner, std::int32_t itemCast) noexcept {
	if (unit != nullptr && !g_proc.open) {
		g_proc.unit      = unit;
		g_proc.usedSkill = g_native.getUsedSkill(unit);
		g_proc.castId    = UnitCastId(unit);
		g_proc.open      = true;
	}
	return g_native.procStart(unit, skillId, level, owner, itemCast);
}

// 002184FE: the only mode-change check in the client skill start 00218030.
std::uint8_t __fastcall OnProcModeCheck(void* unit, std::int32_t mode) noexcept {
	if (g_proc.open && unit == g_proc.unit) {
		return 0; // a proc does not change the caster's mode (the server never does)
	}
	return g_native.modeCheck(unit, mode);
}

// 00218586: a failed start forces the caster to neutral.
std::int64_t __fastcall OnProcSetNeutral(void* unit, std::int32_t mode) noexcept {
	if (g_proc.open && unit == g_proc.unit) {
		return 0;
	}
	return g_native.setNeutral(unit, mode);
}

// 00231334: the last call of 002310B0 on every path after the proc start.
std::int64_t __fastcall OnProcTail(void* unit, std::int32_t flag) noexcept {
	const std::int64_t result = g_native.procTail(unit, flag);
	if (g_proc.open && unit == g_proc.unit) {
		if (g_native.getUsedSkill(unit) != g_proc.usedSkill || UnitCastId(unit) != g_proc.castId) {
			g_native.setUsedSkill(unit, g_proc.usedSkill, g_proc.castId);
		}
		g_proc = {};
	}
	return result;
}

// ---------------------------------------------------------------------------
// Potions while Whirlwinding (server)
// ---------------------------------------------------------------------------

// The player whose potion use is being let past the Uninterruptible checks.
// Server game thread only.
void* g_potionPlayer = nullptr;

auto IsPotionUseWhileWhirlwinding(void* game, void* player, const std::uint8_t* packet) noexcept -> bool {
	if (game == nullptr || player == nullptr || packet == nullptr || packet[0] != PacketUseItem || UnitType(player) != 0) {
		return false;
	}
	void* const used = g_native.getUsedSkill(player);
	if (!IsWhirlwindSkill(used) || (g_native.getSkillFlags(used) & TokenActive) == 0) {
		return false;
	}
	std::uint32_t guid = 0;
	std::memcpy(&guid, packet + 10, sizeof(guid)); // 004F416C: mov r8d,[rax+0Ah]
	void* const item = g_native.getServerUnit(game, UnitTypeItem, guid);
	return item != nullptr && g_native.checkItemTypeId(item, ItemTypePotion) != 0;
}

using PacketDispatchFn = std::int64_t(__fastcall*)(void*, void*, std::uint8_t*, std::uint32_t);
using UseItemFn        = std::int64_t(__fastcall*)(void*, void*, std::uint8_t*, std::uint8_t);
PacketDispatchFn g_originalPacketDispatch = nullptr;
UseItemFn        g_originalUseItem        = nullptr;

// Entry hook on the server message dispatcher 004F2FA0. Its default branch
// ignores every message while the player is Uninterruptible (004F306E).
std::int64_t __fastcall OnPacketDispatch(void* game, void* player, std::uint8_t* packet, std::uint32_t size) noexcept {
	const bool open = g_potionPlayer == nullptr && size == 36 && IsPotionUseWhileWhirlwinding(game, player, packet);
	if (open) {
		g_potionPlayer = player;
	}
	const std::int64_t result = g_originalPacketDispatch(game, player, packet, size);
	if (open) {
		g_potionPlayer = nullptr;
	}
	return result;
}

// Entry hook on the use-item handler 004F40C0, reached for message 0x26 both
// directly and when a queued use is processed later. It refuses the use while
// the player is Uninterruptible (004F4140).
std::int64_t __fastcall OnUseItem(void* game, void* player, std::uint8_t* packet, std::uint8_t queued) noexcept {
	const bool open = g_potionPlayer == nullptr && IsPotionUseWhileWhirlwinding(game, player, packet);
	if (open) {
		g_potionPlayer = player;
	}
	const std::int64_t result = g_originalUseItem(game, player, packet, queued);
	if (open) {
		g_potionPlayer = nullptr;
	}
	return result;
}

// Replaces the two STATES_CheckState(player, 54) calls above. Identical to the
// native call except for the potion use currently being processed.
std::int32_t __fastcall OnUseItemStateCheck(void* unit, std::int32_t state) noexcept {
	if (g_potionPlayer != nullptr && unit == g_potionPlayer && state == StateUninterruptable) {
		return 0;
	}
	return g_native.checkState(unit, state);
}

// ---------------------------------------------------------------------------
// Potions while Whirlwinding (client)
// ---------------------------------------------------------------------------

// The item whose client-side use lock is being evaluated. Client thread only.
void* g_clientPotionItem = nullptr;

auto IsClientPotionWhileWhirlwinding(void* item) noexcept -> bool {
	if (item == nullptr || UnitType(item) != static_cast<std::uint32_t>(UnitTypeItem)) {
		return false;
	}
	void* const player = LocalPlayer();
	if (player == nullptr) {
		return false;
	}
	void* const used = g_native.getUsedSkill(player);
	if (!IsWhirlwindSkill(used) || (g_native.getSkillFlags(used) & TokenActive) == 0) {
		return false;
	}
	return g_native.checkItemTypeId(item, ItemTypePotion) != 0;
}

using ItemUseLockFn = std::int64_t(__fastcall*)(void*);
ItemUseLockFn g_originalItemUseLock = nullptr;

// Entry hook on the client item-use lock 001C7360. The belt keys (00229594),
// inventory use (00161F20) and every other client use path ask it first and
// send nothing when it answers "locked". Vanilla locks every item while the
// local player is Uninterruptible (001C738C).
std::int64_t __fastcall OnItemUseLock(void* item) noexcept {
	const bool open = g_clientPotionItem == nullptr && IsClientPotionWhileWhirlwinding(item);
	if (open) {
		g_clientPotionItem = item;
	}
	const std::int64_t result = g_originalItemUseLock(item);
	if (open) {
		g_clientPotionItem = nullptr;
	}
	return result;
}

// Replaces STATES_CheckState(localPlayer, 54) at 001C738C. The item pointer is
// gone by then (rcx is not preserved past 001C7369), hence the entry scope.
std::int32_t __fastcall OnItemUseLockStateCheck(void* unit, std::int32_t state) noexcept {
	if (g_clientPotionItem != nullptr && state == StateUninterruptable) {
		return 0;
	}
	return g_native.checkState(unit, state);
}

// ---------------------------------------------------------------------------
// CTC while Uninterruptible
// ---------------------------------------------------------------------------

std::uint32_t g_rng = 0x9E3779B9U;

auto CtcRoll() noexcept -> std::uint32_t {
	g_rng ^= g_rng << 13;
	g_rng ^= g_rng >> 17;
	g_rng ^= g_rng << 5;
	return g_rng % 100U;
}

auto CtcAllows(std::int32_t skillId) noexcept -> bool {
	if (g_ctcAllowListActive && (skillId < 0 || skillId >= static_cast<std::int32_t>(g_ctcAllowed.size()) || !g_ctcAllowed[static_cast<std::size_t>(skillId)])) {
		return false;
	}
	if (g_config.ctcChancePercent >= 100) {
		return true;
	}
	if (g_config.ctcChancePercent <= 0) {
		return false;
	}
	return CtcRoll() < static_cast<std::uint32_t>(g_config.ctcChancePercent);
}

using CastOnTargetFn   = std::int32_t(__fastcall*)(void*, std::int32_t, std::int32_t, void*, std::int32_t);
using CastAtPositionFn = std::int32_t(__fastcall*)(void*, std::int32_t, std::int32_t, std::int32_t, std::int32_t, std::int32_t);
CastOnTargetFn   g_originalCastOnTarget   = nullptr;
CastAtPositionFn g_originalCastAtPosition = nullptr;

// 005896E0 SKILLITEM_CastOnTarget. With its Uninterruptible gate NOPed, the
// policy decides here; a refusal returns 0, the native gate's own result.
std::int32_t __fastcall OnCastOnTarget(void* caster, std::int32_t skillId, std::int32_t level, void* target, std::int32_t flag) noexcept {
	if (caster != nullptr && g_native.checkState(caster, StateUninterruptable) != 0 && !CtcAllows(skillId)) {
		return 0;
	}
	return g_originalCastOnTarget(caster, skillId, level, target, flag);
}

// 00589820 SKILLITEM_CastAtPosition, same rule.
std::int32_t __fastcall OnCastAtPosition(void* caster, std::int32_t skillId, std::int32_t level, std::int32_t x, std::int32_t y, std::int32_t flag) noexcept {
	if (caster != nullptr && g_native.checkState(caster, StateUninterruptable) != 0 && !CtcAllows(skillId)) {
		return 0;
	}
	return g_originalCastAtPosition(caster, skillId, level, x, y, flag);
}

// ---------------------------------------------------------------------------
// Installation
// ---------------------------------------------------------------------------

constexpr std::uint8_t CtcGateTarget[] = {0x0F, 0x85, 0xC1, 0x00, 0x00, 0x00};
constexpr std::uint8_t CtcGatePosition[] = {0x0F, 0x85, 0xA4, 0x00, 0x00, 0x00};
constexpr std::uint8_t MeleeBranchServer[] = {0x74, 0x1E};
constexpr std::uint8_t MeleeBranchClient[] = {0x74, 0x10};
constexpr std::uint8_t MeleeSkipServer[] = {0xEB, 0x1E};
constexpr std::uint8_t MeleeSkipClient[] = {0xEB, 0x10};
constexpr std::uint8_t DualWieldBackEdge[] = {0x0F, 0x8C, 0x63, 0xFD, 0xFF, 0xFF};

struct CallSite {
	std::uint64_t      rva;
	std::array<std::uint8_t, 5> original;
	std::size_t        relay;
};

constexpr CallSite SiteServerPathStatus {0x42A99F, {0xE8, 0x1C, 0x71, 0x06, 0x00}, RelayServerPathStatus};
constexpr CallSite SiteClientPathStep   {0x0FFCFD, {0xE8, 0xFE, 0x9C, 0xFF, 0xFF}, RelayClientPathStep};
constexpr CallSite SiteProcStart        {0x23123A, {0xE8, 0x01, 0x64, 0xFE, 0xFF}, RelayProcStart};
constexpr CallSite SiteModeCheck        {0x2184FE, {0xE8, 0xAD, 0x14, 0xEE, 0xFF}, RelayModeCheck};
constexpr CallSite SiteSetNeutral       {0x218586, {0xE8, 0xD5, 0x12, 0xEE, 0xFF}, RelaySetNeutral};
constexpr CallSite SiteProcTail         {0x231334, {0xE8, 0x57, 0xD4, 0x11, 0x00}, RelayProcTail};
constexpr CallSite SiteDispatchState    {0x4F306E, {0xE8, 0x3D, 0x21, 0xE4, 0xFF}, RelayDispatchState};
constexpr CallSite SiteUseItemState     {0x4F4140, {0xE8, 0x6B, 0x10, 0xE4, 0xFF}, RelayUseItemState};
constexpr CallSite SiteItemLockState    {0x1C738C, {0xE8, 0x1F, 0xDE, 0x16, 0x00}, RelayItemLockState};

auto PrepareRelayPage(const D2RL::PluginContext* context) noexcept -> bool {
	const auto base = static_cast<std::uintptr_t>(context->exeBase);
	g_relayPage     = AllocateNear(base + 0x300000, RelayPageBytes);
	if (g_relayPage == nullptr) {
		context->LogError("No relay page could be allocated within rel32 reach of the game image.");
		return false;
	}
	std::memset(g_relayPage, 0xCC, RelayPageBytes);
	WriteAbsoluteJump(g_relayPage + RelayServerPathStatus, reinterpret_cast<std::uint64_t>(&OnServerPathStatus));
	WriteAbsoluteJump(g_relayPage + RelayClientPathStep, reinterpret_cast<std::uint64_t>(&OnClientPathStep));
	WriteAbsoluteJump(g_relayPage + RelayProcStart, reinterpret_cast<std::uint64_t>(&OnProcStart));
	WriteAbsoluteJump(g_relayPage + RelayModeCheck, reinterpret_cast<std::uint64_t>(&OnProcModeCheck));
	WriteAbsoluteJump(g_relayPage + RelaySetNeutral, reinterpret_cast<std::uint64_t>(&OnProcSetNeutral));
	WriteAbsoluteJump(g_relayPage + RelayProcTail, reinterpret_cast<std::uint64_t>(&OnProcTail));
	WriteAbsoluteJump(g_relayPage + RelayDispatchState, reinterpret_cast<std::uint64_t>(&OnUseItemStateCheck));
	WriteAbsoluteJump(g_relayPage + RelayUseItemState, reinterpret_cast<std::uint64_t>(&OnUseItemStateCheck));
	WriteAbsoluteJump(g_relayPage + RelayItemLockState, reinterpret_cast<std::uint64_t>(&OnItemUseLockStateCheck));
	// Dual wield: entered from the loop back-edge 005694B7 with rdi = &token+30h
	// (reloaded at 005694B2). dec dword [rdi] turns "search after the GUID just
	// struck" into "search from that GUID", then resume the loop at 00569220.
	// Sequence fallback: entered from the player path of the sequence resolver
	// 003CB890 at 003CB987, where rax = weapon-class slot and r14 = the
	// sequence's slot array (24-byte slots, rows pointer first). Replays the two
	// displaced LEAs, then swaps an empty slot (NULL rows) for slot 0 before
	// resuming at 003CB98F. Slot 0 is never empty: the resolver has already
	// read its first row at 003CB903.
	static constexpr std::uint8_t SequenceFallbackStub[] {
		0x48, 0x8D, 0x0C, 0x40, // lea  rcx, [rax+rax*2]
		0x49, 0x8D, 0x3C, 0xCE, // lea  rdi, [r14+rcx*8]
		0x48, 0x83, 0x3F, 0x00, // cmp  qword ptr [rdi], 0
		0x49, 0x0F, 0x44, 0xFE, // cmove rdi, r14
	};
	std::memcpy(g_relayPage + RelaySequenceFallback, SequenceFallbackStub, sizeof(SequenceFallbackStub));
	WriteAbsoluteJump(g_relayPage + RelaySequenceFallback + sizeof(SequenceFallbackStub), base + 0x3CB98F);
	g_relayPage[RelayDualWield]     = 0xFF;
	g_relayPage[RelayDualWield + 1] = 0x0F;
	WriteAbsoluteJump(g_relayPage + RelayDualWield + 2, base + 0x569220);
	DWORD previous = 0;
	if (!VirtualProtect(g_relayPage, RelayPageBytes, PAGE_EXECUTE_READ, &previous)) {
		VirtualFree(g_relayPage, 0, MEM_RELEASE);
		g_relayPage = nullptr;
		context->LogError("The relay page could not be made executable.");
		return false;
	}
	FlushInstructionCache(GetCurrentProcess(), g_relayPage, RelayPageBytes);
	return true;
}

auto RedirectCall(const D2RL::PluginContext* context, const CallSite& site) noexcept -> bool {
	const auto base   = static_cast<std::uintptr_t>(context->exeBase);
	const auto next   = base + site.rva + 5;
	const auto target = reinterpret_cast<std::uintptr_t>(g_relayPage) + site.relay;
	if (!WithinRel32(next, target)) {
		return false;
	}
	const auto displacement = static_cast<std::int32_t>(static_cast<std::int64_t>(target) - static_cast<std::int64_t>(next));
	std::array<std::uint8_t, 5> bytes {0xE8};
	std::memcpy(bytes.data() + 1, &displacement, sizeof(displacement));
	return context->PatchBytes(site.rva, site.original.data(), 5, bytes.data(), 5);
}

void InstallCtc(const D2RL::PluginContext* context) noexcept {
	if (!g_config.ctcEnabled) {
		SetPart(Part::CtcGates, PartState::DisabledByConfig);
		SetPart(Part::CtcPolicy, PartState::DisabledByConfig);
		return;
	}
	if (!VerifyAll(context, CtcWitnesses, PartNames[static_cast<std::size_t>(Part::CtcGates)])) {
		SetPart(Part::CtcGates, PartState::UnsupportedBuild);
		SetPart(Part::CtcPolicy, PartState::UnsupportedBuild);
		return;
	}
	const bool policyNeeded = g_ctcAllowListActive || g_config.ctcChancePercent < 100;
	if (policyNeeded) {
		// Hooks first: if they cannot go in, the gates stay closed rather than
		// open more widely than the config asks for.
		if (!context->InstallInlineHook(0x5896E0, P_CastOnTarget, sizeof(P_CastOnTarget), reinterpret_cast<void*>(&OnCastOnTarget), reinterpret_cast<void**>(&g_originalCastOnTarget))
			|| !context->InstallInlineHook(0x589820, P_CastAtPos, sizeof(P_CastAtPos), reinterpret_cast<void*>(&OnCastAtPosition), reinterpret_cast<void**>(&g_originalCastAtPosition))) {
			SetPart(Part::CtcPolicy, PartState::InstallFailed);
			SetPart(Part::CtcGates, PartState::InstallFailed);
			context->LogError("CTC while Uninterruptible NOT installed: the policy hooks failed, so the gates were left closed.");
			return;
		}
		SetPart(Part::CtcPolicy, PartState::Installed);
	} else {
		SetPart(Part::CtcPolicy, PartState::DisabledByConfig);
	}
	const bool gates = context->PatchNop(0x589736, CtcGateTarget, sizeof(CtcGateTarget), sizeof(CtcGateTarget))
		&& context->PatchNop(0x58986B, CtcGatePosition, sizeof(CtcGatePosition), sizeof(CtcGatePosition));
	SetPart(Part::CtcGates, gates ? PartState::Installed : PartState::InstallFailed);
}

void InstallProcFix(const D2RL::PluginContext* context) noexcept {
	if (!g_config.procStartFix) {
		SetPart(Part::ProcStartFix, PartState::DisabledByConfig);
		return;
	}
	if (!VerifyAll(context, ProcFixWitnesses, PartNames[static_cast<std::size_t>(Part::ProcStartFix)])) {
		SetPart(Part::ProcStartFix, PartState::UnsupportedBuild);
		return;
	}
	// Order: the scope closer and the two in-scope refusals first, the scope
	// opener last, so a partial install can never open a scope nobody closes.
	const bool ok = RedirectCall(context, SiteProcTail) && RedirectCall(context, SiteModeCheck)
		&& RedirectCall(context, SiteSetNeutral) && RedirectCall(context, SiteProcStart);
	SetPart(Part::ProcStartFix, ok ? PartState::Installed : PartState::InstallFailed);
}

void InstallDualWield(const D2RL::PluginContext* context) noexcept {
	if (!g_config.dualWieldSameTarget) {
		SetPart(Part::DualWield, PartState::DisabledByConfig);
		return;
	}
	if (!VerifyAll(context, DualWieldWitnesses, PartNames[static_cast<std::size_t>(Part::DualWield)])) {
		SetPart(Part::DualWield, PartState::UnsupportedBuild);
		return;
	}
	const auto base   = static_cast<std::uintptr_t>(context->exeBase);
	const auto next   = base + 0x5694B7 + 6;
	const auto target = reinterpret_cast<std::uintptr_t>(g_relayPage) + RelayDualWield;
	if (!WithinRel32(next, target)) {
		SetPart(Part::DualWield, PartState::InstallFailed);
		return;
	}
	const auto displacement = static_cast<std::int32_t>(static_cast<std::int64_t>(target) - static_cast<std::int64_t>(next));
	std::array<std::uint8_t, 6> bytes {0x0F, 0x8C};
	std::memcpy(bytes.data() + 2, &displacement, sizeof(displacement));
	const bool ok = context->PatchBytes(0x5694B7, DualWieldBackEdge, sizeof(DualWieldBackEdge), bytes.data(), 6);
	SetPart(Part::DualWield, ok ? PartState::Installed : PartState::InstallFailed);
}

// The redirect at 0042A99F serves both the cyclone and the death fix, and is
// installed once for whichever of them asks first.
auto InstallServerFrameRedirect(const D2RL::PluginContext* context) noexcept -> PartState {
	static PartState state = PartState::NotAttempted;
	if (state != PartState::NotAttempted) {
		return state;
	}
	if (!VerifyAll(context, ServerFrameWitnesses, "Whirlwind frame redirect")) {
		state = PartState::UnsupportedBuild;
	} else {
		state = RedirectCall(context, SiteServerPathStatus) ? PartState::Installed : PartState::InstallFailed;
	}
	return state;
}

void InstallDeathFix(const D2RL::PluginContext* context) noexcept {
	if (!g_config.deathWhileWhirlwinding) {
		SetPart(Part::DeathWhileWhirlwinding, PartState::DisabledByConfig);
		return;
	}
	const PartState frame = InstallServerFrameRedirect(context);
	g_deathFixActive      = frame == PartState::Installed;
	SetPart(Part::DeathWhileWhirlwinding, frame);
}

void InstallPotions(const D2RL::PluginContext* context) noexcept {
	if (!g_config.potionsWhileWhirlwinding) {
		SetPart(Part::PotionsWhileWhirlwinding, PartState::DisabledByConfig);
		SetPart(Part::PotionsWhileWhirlwindingClient, PartState::DisabledByConfig);
		return;
	}
	// In every half the redirected checks behave exactly like the native call
	// until a hook opens a potion scope, so they go in first.
	if (!VerifyAll(context, PotionWitnesses, PartNames[static_cast<std::size_t>(Part::PotionsWhileWhirlwinding)])) {
		SetPart(Part::PotionsWhileWhirlwinding, PartState::UnsupportedBuild);
	} else {
		const bool ok = RedirectCall(context, SiteUseItemState) && RedirectCall(context, SiteDispatchState)
			&& context->InstallInlineHook(0x4F40C0, P_UseItem, sizeof(P_UseItem), reinterpret_cast<void*>(&OnUseItem), reinterpret_cast<void**>(&g_originalUseItem))
			&& context->InstallInlineHook(0x4F2FA0, P_Dispatcher, sizeof(P_Dispatcher), reinterpret_cast<void*>(&OnPacketDispatch), reinterpret_cast<void**>(&g_originalPacketDispatch));
		SetPart(Part::PotionsWhileWhirlwinding, ok ? PartState::Installed : PartState::InstallFailed);
	}
	if (!VerifyAll(context, PotionClientWitnesses, PartNames[static_cast<std::size_t>(Part::PotionsWhileWhirlwindingClient)])) {
		SetPart(Part::PotionsWhileWhirlwindingClient, PartState::UnsupportedBuild);
	} else {
		const bool ok = RedirectCall(context, SiteItemLockState)
			&& context->InstallInlineHook(0x1C7360, P_ItemUseLock, sizeof(P_ItemUseLock), reinterpret_cast<void*>(&OnItemUseLock), reinterpret_cast<void**>(&g_originalItemUseLock));
		SetPart(Part::PotionsWhileWhirlwindingClient, ok ? PartState::Installed : PartState::InstallFailed);
	}
}

void InstallSequenceFallback(const D2RL::PluginContext* context) noexcept {
	if (!g_config.whirlwindWithBows) {
		SetPart(Part::WhirlwindWithBows, PartState::DisabledByConfig);
		return;
	}
	if (!VerifyAll(context, SequenceWitnesses, PartNames[static_cast<std::size_t>(Part::WhirlwindWithBows)])) {
		SetPart(Part::WhirlwindWithBows, PartState::UnsupportedBuild);
		return;
	}
	static constexpr std::uint8_t SlotSelect[] {0x48, 0x8D, 0x0C, 0x40, 0x49, 0x8D, 0x3C, 0xCE};
	const auto base   = static_cast<std::uintptr_t>(context->exeBase);
	const auto next   = base + 0x3CB987 + 5;
	const auto target = reinterpret_cast<std::uintptr_t>(g_relayPage) + RelaySequenceFallback;
	if (!WithinRel32(next, target)) {
		SetPart(Part::WhirlwindWithBows, PartState::InstallFailed);
		return;
	}
	const auto displacement = static_cast<std::int32_t>(static_cast<std::int64_t>(target) - static_cast<std::int64_t>(next));
	std::array<std::uint8_t, 8> bytes {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
	std::memcpy(bytes.data() + 1, &displacement, sizeof(displacement));
	const bool ok = context->PatchBytes(0x3CB987, SlotSelect, sizeof(SlotSelect), bytes.data(), static_cast<std::uint32_t>(bytes.size()));
	SetPart(Part::WhirlwindWithBows, ok ? PartState::Installed : PartState::InstallFailed);
}

// No relay needed: an entry hook through the loader.
void InstallSpeedSync(const D2RL::PluginContext* context) noexcept {
	if (!g_config.whirlwindSpeedSync) {
		SetPart(Part::WhirlwindSpeedSync, PartState::DisabledByConfig);
		return;
	}
	if (!VerifyAll(context, SpeedSyncWitnesses, PartNames[static_cast<std::size_t>(Part::WhirlwindSpeedSync)])) {
		SetPart(Part::WhirlwindSpeedSync, PartState::UnsupportedBuild);
		return;
	}
	const bool ok = context->InstallInlineHook(0x56A420, P_WhirlwindStart, sizeof(P_WhirlwindStart), reinterpret_cast<void*>(&OnServerWhirlwindStart), reinterpret_cast<void**>(&g_originalWhirlwindStart));
	SetPart(Part::WhirlwindSpeedSync, ok ? PartState::Installed : PartState::InstallFailed);
}

void InstallCyclone(const D2RL::PluginContext* context) noexcept {

	if (!g_config.cycloneEnabled) {
		SetPart(Part::CycloneServer, PartState::DisabledByConfig);
		SetPart(Part::CycloneClient, PartState::DisabledByConfig);
		SetPart(Part::MeleeSpinServer, PartState::DisabledByConfig);
		SetPart(Part::MeleeSpinClient, PartState::DisabledByConfig);
		return;
	}
	if (VerifyAll(context, CycloneServerWitnesses, PartNames[static_cast<std::size_t>(Part::CycloneServer)])) {
		const bool ok = InstallServerFrameRedirect(context) == PartState::Installed
			&& context->InstallInlineHook(0x4FDB40, P_Executor, sizeof(P_Executor), reinterpret_cast<void*>(&OnPositionExecutor), reinterpret_cast<void**>(&g_originalPositionExecutor));
		g_cycloneServerActive = ok;
		SetPart(Part::CycloneServer, ok ? PartState::Installed : PartState::InstallFailed);
		if (!g_config.spinAtMeleeTargets) {
			SetPart(Part::MeleeSpinServer, PartState::DisabledByConfig);
		} else {
			const bool spin = context->PatchBytes(0x56A4E1, MeleeBranchServer, 2, MeleeSkipServer, 2);
			SetPart(Part::MeleeSpinServer, spin ? PartState::Installed : PartState::InstallFailed);
		}
	} else {
		SetPart(Part::CycloneServer, PartState::UnsupportedBuild);
		SetPart(Part::MeleeSpinServer, PartState::UnsupportedBuild);
	}
	if (VerifyAll(context, CycloneClientWitnesses, PartNames[static_cast<std::size_t>(Part::CycloneClient)])) {
		const bool ok = RedirectCall(context, SiteClientPathStep)
			&& context->InstallInlineHook(0x101FA0, P_HeldRight, sizeof(P_HeldRight), reinterpret_cast<void*>(&OnHeldSkillCommand), reinterpret_cast<void**>(&g_originalHeldSkill))
			&& context->InstallInlineHook(0x1013B0, P_HeldLeft, sizeof(P_HeldLeft), reinterpret_cast<void*>(&OnHeldLeftCommand), reinterpret_cast<void**>(&g_originalHeldLeft));
		SetPart(Part::CycloneClient, ok ? PartState::Installed : PartState::InstallFailed);
		if (!g_config.spinAtMeleeTargets) {
			SetPart(Part::MeleeSpinClient, PartState::DisabledByConfig);
		} else {
			const bool spin = context->PatchBytes(0x230B20, MeleeBranchClient, 2, MeleeSkipClient, 2);
			SetPart(Part::MeleeSpinClient, spin ? PartState::Installed : PartState::InstallFailed);
		}
	} else {
		SetPart(Part::CycloneClient, PartState::UnsupportedBuild);
		SetPart(Part::MeleeSpinClient, PartState::UnsupportedBuild);
	}
}

auto StateText(PartState state) noexcept -> const char* {
	switch (state) {
	case PartState::Installed: return "active";
	case PartState::DisabledByConfig: return "off in config";
	case PartState::UnsupportedBuild: return "NOT installed: game bytes do not match 3.3.93847 under D2RLoader 1.3.0";
	case PartState::InstallFailed: return "NOT installed: install failed (see log)";
	default: return "not attempted";
	}
}

auto WhirlwindCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	const D2RL::PluginContext* context = command != nullptr && command->plugin != nullptr ? command->plugin : g_context;
	if (context == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	char line[256];
	for (std::size_t i = 0; i < g_parts.size(); ++i) {
		std::snprintf(line, sizeof(line), "%s: %s", PartNames[i], StateText(g_parts[i]));
		context->WriteConsoleMessage(line);
	}
	return D2RL::ConsoleCommandResult::Handled;
}

constexpr D2RL::PluginInfo WhirlwindInfo {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = "celestialrayone.whirlwind",
	.name        = "Whirlwind",
	.version     = "0.4.0",
	.author      = "CelestialRayOne",
	.description = "Whirlwind rework: procs while Uninterruptible, proc start fix, dual wield same target, cyclone steering, death and potions while Whirlwinding, Whirlwind with bows, movement speed sync.",
	.flags       = D2RL::PluginFlags::Shared | D2RL::PluginFlags::NativeHooks,
};

static_assert(D2RL::HasValidPluginRole(WhirlwindInfo.flags), "Exactly one execution role must be set.");
static_assert(D2RL::HasFlag(WhirlwindInfo.flags, D2RL::PluginFlags::NativeHooks), "Inline hooks require the NativeHooks flag.");

} // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
	return &WhirlwindInfo;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr || context->exeBase == 0) {
		return false;
	}
	g_context = context;
	if (!context->RegisterConsoleCommand("whirlwind", WhirlwindCommand, "Report which Whirlwind parts are active.")) {
		context->LogWarn("The whirlwind console command was not registered.");
	}
	LoadConfiguration(context);
	g_rng ^= static_cast<std::uint32_t>(GetTickCount64());
	if (g_rng == 0) {
		g_rng = 0x9E3779B9U;
	}
	if (!g_config.enabled) {
		g_parts.fill(PartState::DisabledByConfig);
		context->LogWarn("whirlwind is disabled in its config file. Nothing was installed.");
		return true;
	}
	BindNatives(static_cast<std::uintptr_t>(context->exeBase));
	const bool relays = PrepareRelayPage(context);
	InstallCtc(context);
	InstallSpeedSync(context);
	if (relays) {
		InstallProcFix(context);
		InstallDualWield(context);
		InstallCyclone(context);
		InstallDeathFix(context);
		InstallPotions(context);
		InstallSequenceFallback(context);
	} else {
		SetPart(Part::ProcStartFix, PartState::InstallFailed);
		SetPart(Part::DualWield, PartState::InstallFailed);
		SetPart(Part::CycloneServer, PartState::InstallFailed);
		SetPart(Part::CycloneClient, PartState::InstallFailed);
		SetPart(Part::MeleeSpinServer, PartState::InstallFailed);
		SetPart(Part::MeleeSpinClient, PartState::InstallFailed);
		SetPart(Part::DeathWhileWhirlwinding, PartState::InstallFailed);
		SetPart(Part::PotionsWhileWhirlwinding, PartState::InstallFailed);
		SetPart(Part::PotionsWhileWhirlwindingClient, PartState::InstallFailed);
		SetPart(Part::WhirlwindWithBows, PartState::InstallFailed);
	}
	for (std::size_t i = 0; i < g_parts.size(); ++i) {
		D2RL::LogInfoF(context, "%s: %s", PartNames[i], StateText(g_parts[i]));
	}
	return true;
}

// Inline hooks cannot be withdrawn and their trampolines live in this module,
// so the DLL stays resident for the life of the process.
D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {}
