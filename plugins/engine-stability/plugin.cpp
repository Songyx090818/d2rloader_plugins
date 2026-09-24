// ---------------------------------------------------------------------------
// engine-stability
//
// Vanilla engine bug fixes, each behind its own switch.
//
// A fix corrects a vanilla engine bug that is not a crash. Fixes do change
// gameplay: they make the engine apply what its own data already says.
//
// Target: D2RLoader 1.3.1, which runs Diablo II: Resurrected 3.3 inside
// D2RLoader.exe and moves part of the engine into D2RCore.dll.
//
// Retired in 0.11.0, because D2RLoader 1.3.1 now does the same thing itself.
// Each was confirmed in the 1.3.1 image or D2RCore.dll before it was removed:
//   client_unit_lookup_tombstone      0009F270 answers null for unit id -1
//                                     before walking the bucket
//   lifesteal_while_dead              D2RCore's leech routine returns early for
//                                     a player in mode 0 (death) or 17 (dead)
//   event_handler_recursion           D2RCore's event dispatcher skips a
//                                     handler whose running bit is already set
//   skill_attack_rating_on_missiles   00537B23 gives a player missile created
//                                     without a ToHit its skill's SkillToHit
//   durability_tick_gear_refresh      D2RCore's item stat update skips the gear
//                                     refresh for durability unless the item
//                                     breaks or is repaired
//   hide_zero_skill_description_line  00289AAA skips descline 75 when both of
//                                     its calcs are zero
//   resocket_stat_inflation           0038BBA3 expires a gem's or rune's
//                                     leftover socket stats after the detach
//   blood_mana_cast_gate              00436750 now enters D2RCore's
//                                     CanAffordWideUsedSkill, which weighs the
//                                     cost against life while state 114 is up
//
// Safety model: every game address below is an RVA against image base
// 0x140000000, and nothing is installed unless the bytes already at that RVA
// match byte for byte. On any build the plugin does not recognise it installs
// nothing, logs loudly and stays loaded so the console command can explain why.
// An unrecognised build is therefore a no-op, never a mis-patch.
//
// Configuration: every fix has an on/off switch in
// <scope>\d2rloader\config\celestialrayone.engine-stability.toml, all true by
// default. The file is created with documented defaults on first run. The
// config is read once at load, so edits need a game restart. A fix that the
// config turned off and a fix the build refused are reported differently by
// the console command -- they are not the same thing and must never look it.
// ---------------------------------------------------------------------------

#include <D2RLPlugin/api.h>

// Windows.h defines min/max as macros unless NOMINMAX is set.
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

namespace {

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

template <std::size_t Size>
constexpr auto ByteCount(const std::uint8_t (&)[Size]) noexcept -> std::uint32_t {
	return static_cast<std::uint32_t>(Size);
}

// True when `prefix` is exactly the leading bytes of `body`. Used to prove at
// compile time that the bytes an inline hook displaces really are the first
// bytes of the body the plugin verified.
template <std::size_t PrefixSize, std::size_t BodySize>
constexpr auto IsLeadingBytesOf(
	const std::uint8_t (&prefix)[PrefixSize],
	const std::uint8_t (&body)[BodySize]) noexcept -> bool {
	if (PrefixSize > BodySize) {
		return false;
	}
	for (std::size_t index = 0; index < PrefixSize; ++index) {
		if (prefix[index] != body[index]) {
			return false;
		}
	}
	return true;
}

// True when `part` appears in `whole` starting at `offset`.
template <std::size_t PartSize, std::size_t WholeSize>
constexpr auto IsSubrangeOf(
	const std::uint8_t (&part)[PartSize],
	const std::uint8_t (&whole)[WholeSize],
	std::size_t offset) noexcept -> bool {
	if (offset + PartSize > WholeSize) {
		return false;
	}
	for (std::size_t index = 0; index < PartSize; ++index) {
		if (part[index] != whole[offset + index]) {
			return false;
		}
	}
	return true;
}

// rva of the callee of the E8 call at `offset` inside a verified window.
template <std::size_t Size>
constexpr auto CallTargetRva(const std::uint8_t (&window)[Size], std::uint64_t windowRva, std::size_t offset) noexcept -> std::uint64_t {
	const auto rel = static_cast<std::int32_t>(
		static_cast<std::uint32_t>(window[offset + 1])
		| (static_cast<std::uint32_t>(window[offset + 2]) << 8)
		| (static_cast<std::uint32_t>(window[offset + 3]) << 16)
		| (static_cast<std::uint32_t>(window[offset + 4]) << 24));
	return static_cast<std::uint64_t>(static_cast<std::int64_t>(windowRva + offset + 5) + rel);
}

// True when the eight byte slot at `offset` is the target of a
// jmp qword ptr [rip+0] that ends right before it.
template <std::size_t Size>
constexpr auto HasAbsoluteJumpBefore(const std::uint8_t (&code)[Size], std::size_t offset) noexcept -> bool {
	return offset >= 6
		&& offset + 8 <= Size
		&& code[offset - 6] == 0xFF
		&& code[offset - 5] == 0x25
		&& code[offset - 4] == 0x00
		&& code[offset - 3] == 0x00
		&& code[offset - 2] == 0x00
		&& code[offset - 1] == 0x00;
}

// ---------------------------------------------------------------------------
// Guard identity
// ---------------------------------------------------------------------------

enum class Guard : std::size_t {
	FrozenOrbBurstOnWall,
	SkillFreezeAiResume,
	Count,
};

constexpr std::size_t GuardCount = static_cast<std::size_t>(Guard::Count);

constexpr auto Index(Guard guard) noexcept -> std::size_t {
	return static_cast<std::size_t>(guard);
}

// Why a guard is not running. "You turned it off" and "this build is not
// supported" must never be reported as the same thing.
enum class GuardState : std::uint8_t {
	NotAttempted,
	Installed,
	DisabledByConfig,
	UnsupportedBuild,
	InstallFailed,
};

std::array<std::atomic<GuardState>, GuardCount> GuardStates {};

// Diagnostics only. The hot paths touch nothing; a counter is only written on
// the rare guarded path.
std::array<std::atomic<std::uint64_t>, GuardCount> SuppressedEvents {};

void SetGuardState(Guard guard, GuardState state) noexcept {
	GuardStates[Index(guard)].store(state, std::memory_order_release);
}

auto GetGuardState(Guard guard) noexcept -> GuardState {
	return GuardStates[Index(guard)].load(std::memory_order_acquire);
}

void CountSuppressed(Guard guard) noexcept {
	SuppressedEvents[Index(guard)].fetch_add(1, std::memory_order_relaxed);
}

auto SuppressedCount(Guard guard) noexcept -> std::uint64_t {
	return SuppressedEvents[Index(guard)].load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Fix 3: the burst hit function never runs when a missile dies on a wall
//        (port of the 2.4 fix at 3D1DCC)
// ---------------------------------------------------------------------------
//
//   sub_14045FB50 is missiles.txt pSrvHitFunc 29, the burst Frozen Orb and
//   everything built on it uses: it spawns a ring of the missile's
//   HitSubMissile around the missile. Being a hit function, the engine calls it
//   for every collision the missile has, and the burst is only meant to happen
//   once, at the end of the missile's life. It tells those apart by the frame
//   counter:
//
//     0045FB91  call 00166040             missiles.txt record
//     0045FBA2  cmp  word ptr [rax+46h], 0  HitSubMissile
//     0045FBA7  jl   0045FD00             none: nothing to spawn, return 1
//     0045FBAD  mov  rcx, rbx             the missile
//     0045FBB0  call 003BB1E0             frames left = ceil(countdown / 40)
//     0045FBB5  test eax, eax
//     0045FBB7  je   0045FBC3             none left: burst
//     0045FBB9  mov  eax, 2               still flying: refuse
//     0045FBBE  jmp  0045FD05             and leave
//
//   The missile step 00463100 decrements that counter every update and only
//   resolves the hit once it has run out, so a missile that lives out its Range
//   arrives here with zero frames left and bursts. A missile stopped by a wall
//   does not: the step sees the collision and resolves the hit with frames
//   still on the clock, so the counter test refuses. The missile dies silently
//   against the wall and the whole ring, with all the damage it carries, is
//   lost.
//
//   The counter is not the right question. The step already says what happened,
//   in the third argument it hands to the hit path: a unit for a collision with
//   a unit, null for a wall and for running out of range.
//
//     004631F0  ResolveHit(game, missile, 0, 1)      out of frames
//     004633D5  ResolveHit(game, missile, 0, 1)      wall
//     004633F2  ResolveHit(game, missile, unit, 0)   unit found in the cell
//
//   ResolveHit 004639A0 passes that argument straight through to the hit
//   function in r8, at both of its dispatch sites and at the third one in
//   00464AC0. So what the frame counter is really being asked is "is this a
//   passing hit on a unit", and the argument answers it exactly.
//
//   The fix: refuse the burst only when there are frames left AND the hit is
//   against a unit. A mid-flight unit collision behaves exactly as before, the
//   end of Range behaves exactly as before, and a wall now bursts.
//
//   r8 does not survive to the counter test: 0045FB7D loads it with an assert
//   line number. So it is spilled first, into [rsp+20h]. That slot is the head
//   of the missile creation block this function builds later; nothing reads or
//   writes it until 0045FBEC zeroes it, which is past both patched sites, and
//   the three calls in between can only touch the shadow space at
//   [rsp+00h..1Fh].
//
//   The spill site is rewritten in place, 6 bytes:
//
//     0045FB6D  E9 <rel32>                jmp  relay page + B0h
//     0045FB72  90                        never executed
//
//   and its relay stub does the two displaced moves, the spill, and rejoins:
//
//     +00  48 8B DA                       mov  rbx, rdx            ; stock
//     +03  48 8B E9                       mov  rbp, rcx            ; stock
//     +06  4C 89 44 24 20                 mov  [rsp+20h], r8       ; the hit unit
//     +0B  FF 25 00 00 00 00 <abs64>      jmp  0045FB73
//
//   The counter test is rewritten in place, 14 bytes:
//
//     0045FBB5  E9 <rel32>                jmp  relay page + D0h
//     0045FBBA  90 x9                     never executed
//
//   and its relay stub adds the second test:
//
//     +00  85 C0                          test eax, eax
//     +02  74 1B                          je   +1Fh                ; no frames left: burst
//     +04  48 83 7C 24 20 00              cmp  qword ptr [rsp+20h], 0
//     +0A  74 13                          je   +1Fh                ; no unit: wall or expiry, burst
//     +0C  B8 02 00 00 00                 mov  eax, 2              ; stock refuse value
//     +11  FF 25 00 00 00 00 <abs64>      jmp  0045FD05            ; stock refuse exit
//     +1F  FF 25 00 00 00 00 <abs64>      jmp  0045FBC3            ; burst
//
//   The refuse path jumps to the function's own epilogue rather than to
//   0045FBB9, because 0045FBB9 is inside the replaced region and is a NOP after
//   the patch; landing there would slide through the NOPs into the burst. Stock
//   0045FBB9 is mov eax,2 / jmp 0045FD05, so setting eax in the stub and going
//   straight to 0045FD05 is the same thing.
//
//   eax is dead on the burst path: stock reaches 0045FBC3 with eax zero from
//   the counter, the fix can reach it with the counter still set, and 0045FBC3
//   is mov rdx,rbx / mov rcx,rbp / call, which rewrites it. The flags are dead
//   at both destinations. Neither stub touches rsp, so the call inside the
//   function is made on exactly the alignment it had before.
//
//   The whole function, 0045FB50..0045FD1F, is verified before patching, which
//   also proves the counter call, both stubs' destinations and the refuse
//   value. Decoded end to end: the only branch targets inside it are 0045FBC3,
//   0045FC90, 0045FCDD, 0045FD00 and 0045FD05, so nothing lands in either
//   replaced region, and 0045FBC3's only source is the je being replaced.

constexpr std::uint64_t FrozenOrbHitRva          = 0x0045FB50ULL;  // missiles.txt pSrvHitFunc 29
constexpr std::uint64_t FrozenOrbSpillSiteRva    = 0x0045FB6DULL;
constexpr std::uint64_t FrozenOrbSpillResumeRva  = 0x0045FB73ULL;
constexpr std::uint64_t FrozenOrbGateSiteRva     = 0x0045FBB5ULL;
constexpr std::uint64_t FrozenOrbBurstRva        = 0x0045FBC3ULL;
constexpr std::uint64_t FrozenOrbEpilogueRva     = 0x0045FD05ULL;
constexpr std::uint64_t MissileCurrentFrameRva   = 0x003BB1E0ULL;  // call at 0045FBB0

// Where the hit unit is parked between the two sites. Third home slot of the
// shadow space this function reserves for its own calls, and the first dword of
// the missile creation block it fills from 0045FBEC.
constexpr std::uint8_t FrozenOrbUnitSlotDisp = 0x20;

// RVA 0x45FB50, 464 bytes. pSrvHitFunc 29, D2Game\src\Missiles\MissMode.cpp.
constexpr std::uint8_t FrozenOrbHitBody[] {
	0x40, 0x53, 0x55, 0x57, 0x48, 0x81, 0xEC, 0xD0, 0x00, 0x00, 0x00, 0x48,
	0x8B, 0x05, 0x66, 0xB7, 0x56, 0x02, 0x48, 0x33, 0xC4, 0x48, 0x89, 0x84,
	0x24, 0xC0, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xDA, 0x48, 0x8B, 0xE9, 0x48,
	0x8B, 0xCB, 0x48, 0x8D, 0x15, 0xB3, 0xB1, 0x8B, 0x01, 0x41, 0xB8, 0x50,
	0x13, 0x00, 0x00, 0xE8, 0xD8, 0x9C, 0xEE, 0xFF, 0x0F, 0xB6, 0x8D, 0x06,
	0x01, 0x00, 0x00, 0x8B, 0xD0, 0xE8, 0xAA, 0x64, 0xD0, 0xFF, 0x48, 0x8B,
	0xF8, 0x48, 0x85, 0xC0, 0x0F, 0x84, 0x5E, 0x01, 0x00, 0x00, 0x66, 0x83,
	0x78, 0x46, 0x00, 0x0F, 0x8C, 0x53, 0x01, 0x00, 0x00, 0x48, 0x8B, 0xCB,
	0xE8, 0x2B, 0xB6, 0xF5, 0xFF, 0x85, 0xC0, 0x74, 0x0A, 0xB8, 0x02, 0x00,
	0x00, 0x00, 0xE9, 0x42, 0x01, 0x00, 0x00, 0x48, 0x8B, 0xD3, 0x48, 0x8B,
	0xCD, 0xE8, 0x32, 0x07, 0x03, 0x00, 0x48, 0x85, 0xC0, 0x0F, 0x84, 0x29,
	0x01, 0x00, 0x00, 0x0F, 0x57, 0xC0, 0x48, 0x89, 0xB4, 0x24, 0x00, 0x01,
	0x00, 0x00, 0x33, 0xC9, 0x4C, 0x89, 0xB4, 0x24, 0x08, 0x01, 0x00, 0x00,
	0x0F, 0x11, 0x44, 0x24, 0x20, 0x48, 0x89, 0x8C, 0x24, 0xB0, 0x00, 0x00,
	0x00, 0x48, 0x8B, 0xCB, 0x0F, 0x11, 0x44, 0x24, 0x30, 0xC7, 0x44, 0x24,
	0x20, 0x02, 0x00, 0x00, 0x00, 0x0F, 0x11, 0x44, 0x24, 0x40, 0x48, 0x89,
	0x44, 0x24, 0x28, 0x0F, 0x11, 0x44, 0x24, 0x50, 0x48, 0x89, 0x5C, 0x24,
	0x30, 0x0F, 0x11, 0x44, 0x24, 0x60, 0x0F, 0x11, 0x44, 0x24, 0x70, 0x0F,
	0x11, 0x84, 0x24, 0x80, 0x00, 0x00, 0x00, 0x0F, 0x11, 0x84, 0x24, 0x90,
	0x00, 0x00, 0x00, 0x0F, 0x11, 0x84, 0x24, 0xA0, 0x00, 0x00, 0x00, 0xE8,
	0x0C, 0xC8, 0xF5, 0xFF, 0x48, 0x8B, 0xCB, 0x89, 0x44, 0x24, 0x5C, 0xE8,
	0x20, 0xB8, 0xF5, 0xFF, 0x48, 0x8B, 0xCB, 0x89, 0x44, 0x24, 0x60, 0xE8,
	0x64, 0xBA, 0xEE, 0xFF, 0x89, 0x84, 0x24, 0x88, 0x00, 0x00, 0x00, 0x4C,
	0x8D, 0x35, 0x96, 0x03, 0xBA, 0xFF, 0x0F, 0xBF, 0x47, 0x46, 0xB9, 0x01,
	0x00, 0x00, 0x00, 0x89, 0x44, 0x24, 0x40, 0x8B, 0x47, 0x70, 0x3B, 0xC1,
	0x0F, 0x4C, 0xC1, 0x8B, 0xF0, 0x48, 0xC1, 0xE6, 0x02, 0x33, 0xDB, 0x66,
	0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00, 0x42, 0x8B, 0x84, 0x33,
	0x50, 0xB4, 0xD1, 0x01, 0x48, 0x8D, 0x54, 0x24, 0x20, 0x89, 0x44, 0x24,
	0x4C, 0x48, 0x8B, 0xCD, 0x42, 0x8B, 0x84, 0x33, 0x50, 0xB5, 0xD1, 0x01,
	0x89, 0x44, 0x24, 0x50, 0xE8, 0xEB, 0x74, 0x0D, 0x00, 0x48, 0x8B, 0xF8,
	0x48, 0x85, 0xC0, 0x74, 0x20, 0x42, 0x8B, 0x94, 0x33, 0x50, 0xB4, 0xD1,
	0x01, 0x48, 0x8B, 0xC8, 0xE8, 0xD3, 0xDB, 0xF5, 0xFF, 0x42, 0x8B, 0x94,
	0x33, 0x50, 0xB5, 0xD1, 0x01, 0x48, 0x8B, 0xCF, 0xE8, 0x43, 0xDD, 0xF5,
	0xFF, 0x48, 0x03, 0xDE, 0x48, 0x81, 0xFB, 0x00, 0x01, 0x00, 0x00, 0x7C,
	0xA7, 0x4C, 0x8B, 0xB4, 0x24, 0x08, 0x01, 0x00, 0x00, 0xB8, 0x03, 0x00,
	0x00, 0x00, 0x48, 0x8B, 0xB4, 0x24, 0x00, 0x01, 0x00, 0x00, 0xEB, 0x05,
	0xB8, 0x01, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x8C, 0x24, 0xC0, 0x00, 0x00,
	0x00, 0x48, 0x33, 0xCC, 0xE8, 0x3B, 0x14, 0xE7, 0x00, 0x48, 0x81, 0xC4,
	0xD0, 0x00, 0x00, 0x00, 0x5F, 0x5D, 0x5B, 0xC3,
};

// mov rbx, rdx / mov rbp, rcx
constexpr std::uint8_t FrozenOrbSpillOriginal[] {
	0x48, 0x8B, 0xDA,
	0x48, 0x8B, 0xE9,
};

// jmp rel32 to the relay stub, rel32 filled at install / one never-executed NOP.
constexpr std::uint8_t FrozenOrbSpillTemplate[] {
	0xE9, 0x00, 0x00, 0x00, 0x00,
	0x90,
};

constexpr std::uint32_t FrozenOrbSpillSiteRel32Offset = 1;

// test eax, eax / je 0045FBC3 / mov eax, 2 / jmp 0045FD05
constexpr std::uint8_t FrozenOrbGateOriginal[] {
	0x85, 0xC0,
	0x74, 0x0A,
	0xB8, 0x02, 0x00, 0x00, 0x00,
	0xE9, 0x42, 0x01, 0x00, 0x00,
};

// jmp rel32 to the relay stub, rel32 filled at install / nine never-executed NOPs.
constexpr std::uint8_t FrozenOrbGateTemplate[] {
	0xE9, 0x00, 0x00, 0x00, 0x00,
	0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
};

constexpr std::uint32_t FrozenOrbGateSiteRel32Offset = 1;

// Native relay stubs. The absolute targets are filled at install.
constexpr std::uint8_t FrozenOrbSpillStub[] {
	0x48, 0x8B, 0xDA,
	0x48, 0x8B, 0xE9,
	0x4C, 0x89, 0x44, 0x24, 0x20,
	0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

constexpr std::size_t FrozenOrbSpillResumeTargetOffset = 0x11;

constexpr std::uint8_t FrozenOrbGateStub[] {
	0x85, 0xC0,
	0x74, 0x1B,
	0x48, 0x83, 0x7C, 0x24, 0x20, 0x00,
	0x74, 0x13,
	0xB8, 0x02, 0x00, 0x00, 0x00,
	0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

constexpr std::size_t FrozenOrbGateRefuseTargetOffset = 0x17;
constexpr std::size_t FrozenOrbGateBurstTargetOffset  = 0x25;

static_assert(sizeof(FrozenOrbHitBody) == 0x1D0, "Verified pSrvHitFunc 29 length changed.");
static_assert(FrozenOrbHitBody[sizeof(FrozenOrbHitBody) - 1] == 0xC3, "The verified body must end on the function's ret.");
static_assert(sizeof(FrozenOrbSpillOriginal) == sizeof(FrozenOrbSpillTemplate), "A byte patch must not change the length of the region it replaces.");
static_assert(sizeof(FrozenOrbGateOriginal) == sizeof(FrozenOrbGateTemplate), "A byte patch must not change the length of the region it replaces.");
static_assert(IsSubrangeOf(FrozenOrbSpillOriginal, FrozenOrbHitBody, FrozenOrbSpillSiteRva - FrozenOrbHitRva), "The replaced bytes must sit inside the verified body at the stated offset.");
static_assert(IsSubrangeOf(FrozenOrbGateOriginal, FrozenOrbHitBody, FrozenOrbGateSiteRva - FrozenOrbHitRva), "The replaced bytes must sit inside the verified body at the stated offset.");
static_assert(FrozenOrbSpillResumeRva == FrozenOrbSpillSiteRva + sizeof(FrozenOrbSpillOriginal), "The spill stub must rejoin at the instruction after the site.");
static_assert(FrozenOrbBurstRva == FrozenOrbGateSiteRva + sizeof(FrozenOrbGateOriginal), "The burst exit is the instruction after the site.");
static_assert(FrozenOrbHitBody[0x45FBB0 - 0x45FB50] == 0xE8 && CallTargetRva(FrozenOrbHitBody, FrozenOrbHitRva, 0x45FBB0 - 0x45FB50) == MissileCurrentFrameRva, "The frame counter is not the callee witnessed at 0045FBB0.");
static_assert(FrozenOrbHitBody[0x45FBBE - 0x45FB50] == 0xE9 && CallTargetRva(FrozenOrbHitBody, FrozenOrbHitRva, 0x45FBBE - 0x45FB50) == FrozenOrbEpilogueRva, "The stock refuse path does not leave through the epilogue at 0045FD05.");
static_assert(FrozenOrbGateOriginal[4] == 0xB8 && FrozenOrbGateOriginal[5] == 0x02, "The stock refuse value is not 2.");
static_assert(FrozenOrbGateStub[12] == FrozenOrbGateOriginal[4] && FrozenOrbGateStub[13] == FrozenOrbGateOriginal[5], "The stub must refuse with the stock value.");
static_assert(IsLeadingBytesOf(FrozenOrbSpillOriginal, FrozenOrbSpillStub), "The spill stub must start with the bytes it displaced.");
static_assert(FrozenOrbSpillStub[6] == 0x4C && FrozenOrbSpillStub[7] == 0x89 && FrozenOrbSpillStub[8] == 0x44 && FrozenOrbSpillStub[9] == 0x24 && FrozenOrbSpillStub[10] == FrozenOrbUnitSlotDisp, "The spill must be mov [rsp+20h], r8.");
static_assert(FrozenOrbGateStub[0] == FrozenOrbGateOriginal[0] && FrozenOrbGateStub[1] == FrozenOrbGateOriginal[1], "The gate stub must start with the stock counter test.");
static_assert(FrozenOrbGateStub[8] == FrozenOrbUnitSlotDisp, "The gate must read the slot the spill wrote.");
static_assert(FrozenOrbGateStub[3] == FrozenOrbGateBurstTargetOffset - 6 - 0x04 && FrozenOrbGateStub[11] == FrozenOrbGateBurstTargetOffset - 6 - 0x0C, "Both stub branches must land on the burst jump.");
static_assert(HasAbsoluteJumpBefore(FrozenOrbSpillStub, FrozenOrbSpillResumeTargetOffset), "Spill rejoin target must follow its jmp.");
static_assert(HasAbsoluteJumpBefore(FrozenOrbGateStub, FrozenOrbGateRefuseTargetOffset), "Refuse target must follow its jmp.");
static_assert(HasAbsoluteJumpBefore(FrozenOrbGateStub, FrozenOrbGateBurstTargetOffset), "Burst target must follow its jmp.");
static_assert(sizeof(FrozenOrbSpillStub) == 25 && sizeof(FrozenOrbGateStub) == 45, "Frozen orb stub length changed.");


// ---------------------------------------------------------------------------
// Fix 7: monster AI never resumes after a skill-applied freeze expires
// ---------------------------------------------------------------------------
//
// A monster's AI runs on a timed event, the one the engine calls AITHINK, event
// type 2 on the monster event list. Each dispatch is what arranges the next
// one, so the AI is a heartbeat: lose a single beat and the monster never
// thinks again.
//
// The monster event dispatcher sub_140447420 refuses to deliver events to a
// frozen unit. Six event types are exempt, everything else is dropped:
//
//   004474B3  mov  edx, 1                      ; the freeze state
//   004474BB  call STATES_CheckState
//   004474C2  jz   004474D4                    ; not frozen, deliver
//   004474C7  call SUNIT_IsDead
//   004474CE  jz   00447572                    ; frozen and alive, DROP
//
// It drops without rescheduling, so the first AI think that lands during a
// freeze destroys the heartbeat, and nothing in the dispatcher brings it back.
// This is 1.10 code, unchanged: D2MOO's MONSTERMODE_EventHandler is the same
// function with the same exempt list.
//
// Cold damage survives it because the cold path re-arms the AI at both ends of
// the freeze. The applier sub_140451570 does it up front, when the freeze goes
// on:
//
//   004517AD  call EVENTS_Delete   (game, unit, AITHINK, 0)
//   004517D7  call EVENT_SetEvent  (game, unit, AITHINK, freezeEnd + 1, 0,0,0)
//
// and its own expiry callback sub_140452240, which is D2MOO's
// SUNITDMG_RemoveFreezeState, does it again on the way out, at the monster's
// MonStats AIdel delay:
//
//   00452397  call EVENTS_Delete   (game, unit, AITHINK, 0)
//   004523BE  call EVENT_SetEvent  (game, unit, AITHINK, AIdel + frame, 0,0,0)
//
// A freeze applied any other way gets neither. Skills, auras and curses build
// their state through the generic applier, which installs sub_140436240 as the
// expiry callback, and that callback does five things, none of them an AI
// re-arm: unregister the stat event, clear the state, refresh the animation
// rate, refresh passives, and for a player only, refresh player state. In
// vanilla nothing applies the freeze state that way, so the gap is never
// reached. A mod that freezes through a skill reaches it every time, and the
// monster thaws into a perfectly valid idle mode with a dead heartbeat: it
// stands still for good, and reloading the area makes it act exactly once
// before hanging again.
//
// The fix invents no recovery behaviour. Once the generic callback has
// finished, a live monster whose expiring state is the freeze state is handed
// to the engine's own freeze thaw at 00452240, the routine the engine already
// runs at the end of a cold-damage freeze. That supplies the AIdel lookup, the
// difficulty selection and the delete-then-arm pair exactly as vanilla does
// them, so a skill freeze now ends the way a cold freeze ends.
//
// The liveness gate is this plugin's, not vanilla's, and it is deliberate. The
// engine's freeze thaw will re-arm the AI on a corpse whenever the expiring
// state is not flagged to persist through death, because a state expiry event
// is never cancelled when a unit dies: UNITS_Die cancels queued mode events on
// the unit's event list, and state expiry lives on a different list that
// survives. Vanilla is saved from that only by States.txt monstaydeath on the
// freeze row. Arming the AI on a corpse stands it back up, so the delegation
// happens only for a unit SUNIT_IsDead calls alive. That predicate is the
// engine's own, the one both callbacks open with: not null, not dead-flagged,
// and not in either of the unit type's two death modes.
//
// The RVAs below are not taken on trust. SUNIT_IsDead and the stay-death test
// are both proved from inside the two verified bodies, which call the same
// pair, and the argument order the hook depends on is proved from the first
// instructions of each: rdx into the unit register, r8d into the state
// register, before anything else happens.
//
// The hook displaces the first 5 bytes, one position independent store. Both
// internal branch targets, 00436286 and 004362D1, are well clear of it.

constexpr std::uint64_t StateRemoveCallbackRva = 0x00436240ULL;
constexpr std::uint64_t RemoveFreezeStateRva   = 0x00452240ULL;
constexpr std::uint64_t UnitIsDeadRva          = 0x0034C2C0ULL;  // call at 43626F and 452253
constexpr std::uint64_t StayDeathRva           = 0x00335B30ULL;  // call at 43627D and 452261
constexpr std::uint64_t StatesToggleStateRva   = 0x003354C0ULL;  // call at 43628E and 452276
constexpr std::uint64_t UnitGetTypeRva         = 0x0034B9D0ULL;  // call at 4362B5

constexpr std::size_t StateRemoveIsDeadOffset     = 47;
constexpr std::size_t StateRemoveStayDeathOffset  = 61;
constexpr std::size_t StateRemoveToggleOffset     = 78;
constexpr std::size_t StateRemoveUnitTypeOffset   = 117;
constexpr std::size_t RemoveFreezeIsDeadOffset    = 19;
constexpr std::size_t RemoveFreezeStayDeathOffset = 33;
constexpr std::size_t RemoveFreezeToggleOffset    = 54;

constexpr std::int32_t StateFreeze     = 1;  // States.txt row 1
constexpr std::int32_t UnitTypeMonster = 1;
constexpr std::size_t  UnitTypeOffset  = 0x00;  // dwType, the unit's first dword

constexpr std::uint8_t StateRemoveCallbackBody[] {
	0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
	0xCA, 0x41, 0x8B, 0xF8, 0x48, 0x8B, 0xDA, 0xE8, 0xA8, 0x9C, 0x05, 0x00,
	0x48, 0x8B, 0xC8, 0x44, 0x8B, 0xCF, 0x41, 0xB8, 0x01, 0x00, 0x00, 0x00,
	0x48, 0x8B, 0xD3, 0xE8, 0x04, 0x24, 0x15, 0x00, 0x48, 0x8B, 0xCB, 0xE8,
	0x4C, 0x60, 0xF1, 0xFF, 0x85, 0xC0, 0x74, 0x0E, 0x8B, 0xD7, 0x48, 0x8B,
	0xCB, 0xE8, 0xAE, 0xF8, 0xEF, 0xFF, 0x85, 0xC0, 0x75, 0x4B, 0x45, 0x33,
	0xC0, 0x8B, 0xD7, 0x48, 0x8B, 0xCB, 0xE8, 0x2D, 0xF2, 0xEF, 0xFF, 0x41,
	0xB8, 0x7F, 0x05, 0x00, 0x00, 0x48, 0x8D, 0x15, 0x00, 0x31, 0x8E, 0x01,
	0x48, 0x8B, 0xCB, 0xE8, 0x98, 0xA8, 0xF1, 0xFF, 0xB2, 0x01, 0x48, 0x8B,
	0xCB, 0xE8, 0x7E, 0x95, 0xF0, 0xFF, 0x48, 0x8B, 0xCB, 0xE8, 0x16, 0x57,
	0xF1, 0xFF, 0x85, 0xC0, 0x75, 0x13, 0x48, 0x8B, 0xCB, 0xE8, 0x3A, 0x9C,
	0x05, 0x00, 0x48, 0x8B, 0xC8, 0x48, 0x8B, 0xD3, 0xE8, 0xCF, 0xA2, 0x0C,
	0x00, 0x48, 0x8B, 0x5C, 0x24, 0x30, 0x48, 0x83, 0xC4, 0x20, 0x5F, 0xC3,
};

// mov [rsp+8], rbx
constexpr std::uint8_t StateRemoveCallbackPrologue[] {
	0x48, 0x89, 0x5C, 0x24, 0x08,
};

// Entry of the cold-damage thaw, down to and including its own clear of the
// state. Enough to prove it is that routine and that it takes the same
// arguments in the same registers.
constexpr std::uint8_t RemoveFreezeStateEntry[] {
	0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x70, 0x48, 0x8B,
	0xCA, 0x41, 0x8B, 0xD8, 0x48, 0x8B, 0xFA, 0xE8, 0x68, 0xA0, 0xEF, 0xFF,
	0x85, 0xC0, 0x74, 0x12, 0x8B, 0xD3, 0x48, 0x8B, 0xCF, 0xE8, 0xCA, 0x38,
	0xEE, 0xFF, 0x85, 0xC0, 0x0F, 0x85, 0x5D, 0x01, 0x00, 0x00, 0x45, 0x33,
	0xC0, 0x8B, 0xD3, 0x48, 0x8B, 0xCF, 0xE8, 0x45, 0x32, 0xEE, 0xFF,
};

constexpr std::uint8_t UnitIsDeadEntry[] {
	0x48, 0x83, 0xEC, 0x28, 0x48, 0x85, 0xC9, 0x75, 0x1D, 0x88, 0x4C, 0x24,
	0x30, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8, 0x59, 0x94, 0xFF, 0xFF, 0x84,
	0xC0, 0x74, 0x4D, 0xCC, 0xB8, 0x01, 0x00, 0x00,
};

static_assert(sizeof(StateRemoveCallbackBody) == 156, "Verified generic state-remove callback length changed.");
static_assert(StateRemoveCallbackBody[sizeof(StateRemoveCallbackBody) - 1] == 0xC3, "The verified body must end on the callback's ret.");
static_assert(sizeof(StateRemoveCallbackPrologue) >= 5, "An inline hook needs at least 5 displaced bytes for a rel32 jump.");
static_assert(IsLeadingBytesOf(StateRemoveCallbackPrologue, StateRemoveCallbackBody), "The displaced bytes must be the leading bytes of the verified body.");
static_assert(sizeof(RemoveFreezeStateEntry) == 59, "Verified cold-damage thaw entry length changed.");
static_assert(sizeof(UnitIsDeadEntry) == 32, "Verified unit death predicate entry length changed.");

// Argument order. Both routines take the unit out of rdx and the state id out
// of r8d before touching anything else, which is the whole basis of the hook's
// signature and of forwarding its arguments on to the thaw.
static_assert(StateRemoveCallbackBody[10] == 0x48 && StateRemoveCallbackBody[11] == 0x8B && StateRemoveCallbackBody[12] == 0xCA, "The generic callback must open by taking the unit from rdx.");
static_assert(StateRemoveCallbackBody[13] == 0x41 && StateRemoveCallbackBody[14] == 0x8B && StateRemoveCallbackBody[15] == 0xF8, "The generic callback must open by taking the state id from r8d.");
static_assert(RemoveFreezeStateEntry[10] == 0x48 && RemoveFreezeStateEntry[11] == 0x8B && RemoveFreezeStateEntry[12] == 0xCA, "The cold-damage thaw must open by taking the unit from rdx.");
static_assert(RemoveFreezeStateEntry[13] == 0x41 && RemoveFreezeStateEntry[14] == 0x8B && RemoveFreezeStateEntry[15] == 0xD8, "The cold-damage thaw must open by taking the state id from r8d.");

// Callees, each proved from inside a verified body rather than asserted.
static_assert(StateRemoveCallbackBody[StateRemoveIsDeadOffset] == 0xE8 && CallTargetRva(StateRemoveCallbackBody, StateRemoveCallbackRva, StateRemoveIsDeadOffset) == UnitIsDeadRva, "The death predicate is not the callee witnessed at 0043626F.");
static_assert(StateRemoveCallbackBody[StateRemoveStayDeathOffset] == 0xE8 && CallTargetRva(StateRemoveCallbackBody, StateRemoveCallbackRva, StateRemoveStayDeathOffset) == StayDeathRva, "The stay-death test is not the callee witnessed at 0043627D.");
static_assert(StateRemoveCallbackBody[StateRemoveToggleOffset] == 0xE8 && CallTargetRva(StateRemoveCallbackBody, StateRemoveCallbackRva, StateRemoveToggleOffset) == StatesToggleStateRva, "The state toggle is not the callee witnessed at 0043628E.");
static_assert(StateRemoveCallbackBody[StateRemoveUnitTypeOffset] == 0xE8 && CallTargetRva(StateRemoveCallbackBody, StateRemoveCallbackRva, StateRemoveUnitTypeOffset) == UnitGetTypeRva, "The unit type getter is not the callee witnessed at 004362B5.");

// The same three callees again, out of the cold-damage thaw. Two independent
// derivations of the death predicate, and proof that the two routines really
// are the pair this fix treats them as.
static_assert(RemoveFreezeStateEntry[RemoveFreezeIsDeadOffset] == 0xE8 && CallTargetRva(RemoveFreezeStateEntry, RemoveFreezeStateRva, RemoveFreezeIsDeadOffset) == UnitIsDeadRva, "The cold-damage thaw does not open with the same death predicate.");
static_assert(RemoveFreezeStateEntry[RemoveFreezeStayDeathOffset] == 0xE8 && CallTargetRva(RemoveFreezeStateEntry, RemoveFreezeStateRva, RemoveFreezeStayDeathOffset) == StayDeathRva, "The cold-damage thaw does not use the same stay-death test.");
static_assert(RemoveFreezeStateEntry[RemoveFreezeToggleOffset] == 0xE8 && CallTargetRva(RemoveFreezeStateEntry, RemoveFreezeStateRva, RemoveFreezeToggleOffset) == StatesToggleStateRva, "The cold-damage thaw does not clear the state through the same toggle.");

// Both routines take the same three arguments. The first is carried through
// untouched: neither reads it, and both re-derive the game from the unit.
using StateRemoveCallbackFn = std::int64_t(__fastcall*)(
	std::uint64_t reserved,
	void*         unit,
	std::uint32_t stateId) noexcept;

using UnitIsDeadFn = std::int32_t(__fastcall*)(const void* unit) noexcept;

StateRemoveCallbackFn OriginalStateRemoveCallback = nullptr;
StateRemoveCallbackFn RemoveFreezeState           = nullptr;
UnitIsDeadFn          UnitIsDead                  = nullptr;

// A monster the engine itself calls alive. Null is not a live monster, and
// neither is anything the plugin could not resolve, because in both cases the
// safe answer is to leave the unit alone.
auto IsLiveMonster(const void* unit) noexcept -> bool {
	if (unit == nullptr || UnitIsDead == nullptr) {
		return false;
	}

	std::int32_t unitType = 0;
	std::memcpy(&unitType, static_cast<const unsigned char*>(unit) + UnitTypeOffset, sizeof(unitType));
	if (unitType != UnitTypeMonster) {
		return false;
	}

	return UnitIsDead(unit) == 0;
}

auto __fastcall HookStateRemoveCallback(
	std::uint64_t reserved,
	void*         unit,
	std::uint32_t stateId) noexcept -> std::int64_t {
	// The generic teardown runs first and unchanged, so the AI is armed against
	// the unit's final state rather than a half torn down one, and the value
	// the engine gets back is the stock one.
	const StateRemoveCallbackFn original = OriginalStateRemoveCallback;
	const std::int64_t          result   = original != nullptr ? original(reserved, unit, stateId) : 0;

	if (stateId != static_cast<std::uint32_t>(StateFreeze) || !IsLiveMonster(unit)) {
		return result;
	}

	const StateRemoveCallbackFn thaw = RemoveFreezeState;
	if (thaw != nullptr) {
		thaw(reserved, unit, stateId);
		CountSuppressed(Guard::SkillFreezeAiResume);  // counts AI heartbeats restarted
	}

	return result;
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
//
// The loader hands the plugin the raw text of its own TOML file; there is no
// parser in the SDK. Rather than pull one in for a handful of booleans, this is
// a small line scanner that understands exactly what this file needs: [section]
// headers, `key = true` / `key = false`, `#` comments (whole line or trailing),
// and both LF and CRLF. Anything it does not understand it ignores, which is
// the right failure mode for a config file a player edits by hand.
//
// The defaults are the safe direction: if the file is missing, unreadable or
// truncated, every guard stays ON and the plugin says so in the log. Losing a
// crash guard because a config file could not be read would be the worse of
// the two outcomes.

// Kept identical to the shipped celestialrayone.engine-stability.toml. This is
// what EnsureConfig writes when the file does not exist yet.
constexpr const char* DefaultConfigToml =
R"toml(# =============================================================================
# Engine Stability 0.11.0
# Vanilla engine fixes for Diablo II: Resurrected.
#
# Built against D2RLoader 1.3.1: Diablo II: Resurrected 3.3 inside
# D2RLoader.exe.
# =============================================================================
#
# WHAT IS LEFT, AND WHY
#
#   D2RLoader 1.3.1 now fixes eight of the ten entries this plugin used to
#   carry, so 0.11.0 removed them. If an older copy of this file still lists
#   their keys, they are ignored:
#     client_unit_lookup_tombstone, lifesteal_while_dead,
#     event_handler_recursion, skill_attack_rating_on_missiles,
#     durability_tick_gear_refresh, hide_zero_skill_description_line,
#     resocket_stat_inflation, blood_mana_cast_gate
#
#   The two below are not fixed by D2RLoader. They correct engine bugs that
#   are not crashes, so they DO change gameplay: they make the engine apply
#   what its own data already says. Turning one off restores stock behaviour.
#
# SAFETY MODEL
#
#   Every site is verified byte for byte before anything is written. On a build
#   the plugin does not recognise it installs nothing, no matter what this file
#   says, logs the reason and stays loaded so it can still answer for itself.
#   An unrecognised build is a no-op, never a mis-patch.
#
#   "Turned off in the config" and "this build was not recognised" are reported
#   as different things, in the log and in the console command. They are not
#   the same problem and never look the same.
#
# USING THIS FILE
#
#   Location: <scope>\d2rloader\config\celestialrayone.engine-stability.toml
#             where <scope> is the game root for a global install, or
#             mods\<mod>\ for a mod-scoped one. Never both.
#
#   Read once at plugin load, so restart the game after editing.
#   Delete the file to have it recreated with these defaults.
#   A missing key keeps its default. An unknown key is ignored.
#   Values are true or false. Anything else is ignored and the default stands.
#
#   Type `engine-stability` in the console to see what is actually live, which
#   is not the same question as what this file asks for.


[engine-stability]

# Master switch for the whole plugin.
#   true   The fixes are installed as configured below.
#   false  The DLL stays loaded and the console command still answers, but not
#          one hook is installed. Use this to rule the plugin out while chasing
#          a problem, without moving the DLL out of the folder.
# Default: true
enabled = true

# =============================================================================
[fixes]
# =============================================================================
# Missile burst skipped when the missile dies on a wall.
#   Site:     RVA 0045FB6D and RVA 0045FBB5, in Missiles.txt pSrvHitFunc 29.
#   Bug:      pSrvHitFunc 29 is the burst used by Frozen Orb and everything
#             built on it: at the end of the missile's life it spawns a ring of
#             the missile's HitSubMissile. It is a hit function, so the engine
#             calls it on every collision, and it decides whether this is the
#             end of life by checking how many frames the missile has left. A
#             missile stopped by a wall still has frames left, so the test
#             refuses and the ring is never spawned. The missile dies silently
#             against the wall and every sub-missile it should have released,
#             with all the damage they carry, is lost. Only a missile that
#             lives out its full Range ever bursts.
#   Fix:      The burst is refused only when the missile still has frames left
#             AND the hit is against a unit, which is the case the frame test
#             was really there to catch. The engine already tells the hit
#             function which of the two it is. Hitting a monster in mid-flight
#             behaves as before, running out of Range behaves as before, dying
#             on a wall now bursts.
#   Scope:    This is a pSrvHitFunc 29 change, not a Frozen Orb change. Every
#             missile in your Missiles.txt using pSrvHitFunc 29 starts bursting
#             on walls. Server side only; pCltHitFunc is untouched.
#   Cost:     One compare per pSrvHitFunc 29 collision.
# Default: true
frozen_orb_burst_on_wall = true
# Monster AI never resuming after a skill-applied freeze.
#   Site:     RVA 00436240, the generic state-removal callback that skills,
#             auras and curses install on every state they apply with a
#             duration.
#   Bug:      A monster's AI runs on a timed event, and each time that event
#             fires it is what arranges the next one. The monster event
#             dispatcher refuses to deliver events to a frozen unit, and it
#             drops them rather than rescheduling them, so the first AI think
#             that lands during a freeze destroys the heartbeat. Cold damage
#             gets away with it because the cold path re-arms the AI twice,
#             once when the freeze goes on and once when it expires. A freeze
#             applied through a skill or an aura gets neither: the callback
#             above clears the state, refreshes the animation rate and
#             refreshes passives, and never touches the AI. The monster thaws
#             into a perfectly valid idle mode and stands there for good.
#             Reloading the area makes it act exactly once, then hang again.
#   Fix:      Once the generic callback has finished, a live monster whose
#             expiring state is freeze is handed to the engine's own freeze
#             thaw, the routine the engine already runs at the end of a
#             cold-damage freeze. That re-arms the AI at the monster's own
#             MonStats AIdel delay. Nothing is reimplemented and no timing is
#             invented: a skill freeze now ends the way a cold freeze ends.
#   Also changes: nothing for cold-damage freeze. Those states carry the cold
#             thaw as their own callback and never reach this one.
#   Scope:    The freeze state on monsters, and only when it expires through
#             the generic callback. Every other state, and every player, is
#             passed straight through.
#   Note:     Dead and dying monsters are excluded on purpose. A state expiry
#             event is not cancelled when a unit dies, so the callback still
#             fires on a corpse, and arming the AI on a corpse stands it back
#             up. Vanilla avoids that only through States.txt monstaydeath on
#             the freeze row; this fix does not rely on that column being set.
#   Cost:     One compare per expiring state.
# Default: true
skill_freeze_ai_resume = true
)toml";

struct Slice {
	const char* begin;
	const char* end;
};

constexpr auto IsBlank(char value) noexcept -> bool {
	return value == ' ' || value == '\t' || value == '\r';
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
	const char* text = literal;
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

// Reads section.key as a boolean. Returns true only when the key was present
// and spelled true or false; `value` is left untouched otherwise.
auto ReadConfigBool(const char* toml, const char* section, const char* key, bool& value) noexcept -> bool {
	if (toml == nullptr) {
		return false;
	}

	std::array<char, 64> currentSection {};
	bool                 found = false;

	for (const char* line = toml; *line != '\0';) {
		const char* lineEnd = line;
		while (*lineEnd != '\0' && *lineEnd != '\n') {
			++lineEnd;
		}

		const Slice trimmed = Trim({ line, lineEnd });

		if (trimmed.begin < trimmed.end && *trimmed.begin != '#') {
			if (*trimmed.begin == '[') {
				const char* close = trimmed.begin;
				while (close < trimmed.end && *close != ']') {
					++close;
				}

				const Slice  name = Trim({ trimmed.begin + 1, close });
				std::size_t  used = 0;
				for (const char* cursor = name.begin; cursor < name.end && used + 1 < currentSection.size(); ++cursor) {
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
					Slice       raw  = Trim({ equals + 1, trimmed.end });

					for (const char* cursor = raw.begin; cursor < raw.end; ++cursor) {
						if (*cursor == '#') {
							raw.end = cursor;
							break;
						}
					}
					raw = Trim(raw);

					if (std::strcmp(currentSection.data(), section) == 0 && SliceEquals(name, key)) {
						if (SliceEquals(raw, "true")) {
							value = true;
							found = true;
						} else if (SliceEquals(raw, "false")) {
							value = false;
							found = true;
						}
					}
				}
			}
		}

		line = (*lineEnd == '\0') ? lineEnd : lineEnd + 1;
	}

	return found;
}

// Reads section.key as a whole number inside [0, high]. Anything that is not a
// plain run of digits, and anything out of range, leaves `value` untouched, so
// a typo falls back to the default rather than to zero.
[[maybe_unused]] auto ReadConfigInt(
	const char*    toml,
	const char*    section,
	const char*    key,
	std::uint32_t  high,
	std::uint32_t& value,
	bool&          sawBadValue) noexcept -> bool {
	if (toml == nullptr) {
		return false;
	}

	std::array<char, 64> currentSection {};
	bool                 found = false;

	for (const char* line = toml; *line != '\0';) {
		const char* lineEnd = line;
		while (*lineEnd != '\0' && *lineEnd != '\n') {
			++lineEnd;
		}

		const Slice trimmed = Trim({ line, lineEnd });

		if (trimmed.begin < trimmed.end && *trimmed.begin != '#') {
			if (*trimmed.begin == '[') {
				const char* close = trimmed.begin;
				while (close < trimmed.end && *close != ']') {
					++close;
				}

				const Slice  name = Trim({ trimmed.begin + 1, close });
				std::size_t  used = 0;
				for (const char* cursor = name.begin; cursor < name.end && used + 1 < currentSection.size(); ++cursor) {
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
					Slice       raw  = Trim({ equals + 1, trimmed.end });

					for (const char* cursor = raw.begin; cursor < raw.end; ++cursor) {
						if (*cursor == '#') {
							raw.end = cursor;
							break;
						}
					}
					raw = Trim(raw);

					if (std::strcmp(currentSection.data(), section) == 0 && SliceEquals(name, key)) {
						if (raw.begin >= raw.end) {
							sawBadValue = true;
						} else {
							std::uint64_t parsed  = 0;
							bool          isDigits = true;
							for (const char* cursor = raw.begin; cursor < raw.end; ++cursor) {
								if (*cursor < '0' || *cursor > '9') {
									isDigits = false;
									break;
								}
								parsed = parsed * 10 + static_cast<std::uint64_t>(*cursor - '0');
								if (parsed > high) {
									isDigits = false;
									break;
								}
							}

							if (isDigits) {
								value       = static_cast<std::uint32_t>(parsed);
								sawBadValue = false;
								found       = true;
							} else {
								sawBadValue = true;
							}
						}
					}
				}
			}
		}

		line = (*lineEnd == '\0') ? lineEnd : lineEnd + 1;
	}

	return found;
}

struct StabilityConfig {
	bool pluginEnabled        { true };
	bool frozenOrbBurstOnWall { true };
	bool skillFreezeAiResume  { true };
};

StabilityConfig Config {};

// Only meaningful for the log line; the console command reads the guard states.
bool ConfigFileWasRead = false;

void LoadConfiguration(const D2RL::PluginContext* context) noexcept {
	if (context == nullptr) {
		return;
	}

	if (!context->EnsureConfig(DefaultConfigToml)) {
		context->LogWarn(
			"Could not create or open celestialrayone.engine-stability.toml. "
			"Running with defaults: every fix ON.");
		return;
	}

	std::array<char, 32768> buffer {};
	std::uint32_t           requiredSize = 0;

	if (!context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size() - 1), &requiredSize)) {
		context->LogWarn(
			"Could not read celestialrayone.engine-stability.toml. "
			"Running with defaults: every fix ON.");
		return;
	}

	if (requiredSize >= buffer.size()) {
		D2RL::LogWarnF(
			context,
			"celestialrayone.engine-stability.toml is %u bytes, larger than the %zu byte read buffer. "
			"Running with defaults: every fix ON.",
			requiredSize,
			buffer.size());
		return;
	}

	buffer[buffer.size() - 1] = '\0';
	ConfigFileWasRead         = true;

	(void)ReadConfigBool(buffer.data(), "engine-stability", "enabled", Config.pluginEnabled);
	(void)ReadConfigBool(buffer.data(), "fixes", "frozen_orb_burst_on_wall", Config.frozenOrbBurstOnWall);
	(void)ReadConfigBool(buffer.data(), "fixes", "skill_freeze_ai_resume", Config.skillFreezeAiResume);

	D2RL::LogInfoF(
		context,
		"config: enabled=%s, frozen_orb_burst_on_wall=%s, skill_freeze_ai_resume=%s.",
		Config.pluginEnabled ? "true" : "false",
		Config.frozenOrbBurstOnWall ? "true" : "false",
		Config.skillFreezeAiResume ? "true" : "false");
}

// ---------------------------------------------------------------------------
// Installation
// ---------------------------------------------------------------------------

// Everything that differs between inline hook sites lives in one row, so the
// install and report paths stay single-copy.
struct InlineGuardSite {
	Guard               guard;
	const char*         label;      // how the fix is named in log and console
	const char*         eventNoun;  // what its counter counts
	const char*         rvaText;    // for messages; %llX of a constant reads badly
	std::uint64_t       rva;
	const std::uint8_t* body;
	std::uint32_t       bodySize;
	const std::uint8_t* prologue;
	std::uint32_t       prologueSize;
	void*               hook;
	void**              original;
	const bool*         enabled;
};

constexpr const char* SkillFreezeFixLabel = "skill-freeze AI resume fix";

auto InstallInlineGuard(const D2RL::PluginContext* context, const InlineGuardSite& site) noexcept -> bool {
	if (context == nullptr) {
		return false;
	}

	if (!Config.pluginEnabled || !*site.enabled) {
		SetGuardState(site.guard, GuardState::DisabledByConfig);
		D2RL::LogInfoF(context, "%s not installed: turned off in the config file.", site.label);
		return false;
	}

	if (!context->CheckExpectedBytes(site.rva, site.body, site.bodySize)) {
		SetGuardState(site.guard, GuardState::UnsupportedBuild);
		D2RL::LogErrorF(
			context,
			"%s NOT installed: the bytes at RVA %s do not match the verified 3.3.93847 body. "
			"This build is not supported; nothing was patched.",
			site.label,
			site.rvaText);
		return false;
	}

	if (!context->InstallInlineHook(site.rva, site.prologue, site.prologueSize, site.hook, site.original)) {
		SetGuardState(site.guard, GuardState::InstallFailed);
		D2RL::LogErrorF(context, "%s NOT installed: InstallInlineHook failed at RVA %s.", site.label, site.rvaText);
		return false;
	}

	if (*site.original == nullptr) {
		SetGuardState(site.guard, GuardState::InstallFailed);
		D2RL::LogErrorF(context, "%s NOT installed: the loader returned no trampoline.", site.label);
		return false;
	}

	SetGuardState(site.guard, GuardState::Installed);
	D2RL::LogInfoF(context, "%s installed at RVA %s.", site.label, site.rvaText);
	return true;
}

// The only inline hook that calls engine routines of its own, so it verifies
// and resolves them before the hook goes in and can reach them. A hook
// installed over an unverified callee would be worse than no hook at all.
const InlineGuardSite SkillFreezeAiResumeSite {
	.guard        = Guard::SkillFreezeAiResume,
	.label        = SkillFreezeFixLabel,
	.eventNoun    = "skill-freeze thaws handed to the engine thaw",
	.rvaText      = "00436240",
	.rva          = StateRemoveCallbackRva,
	.body         = StateRemoveCallbackBody,
	.bodySize     = ByteCount(StateRemoveCallbackBody),
	.prologue     = StateRemoveCallbackPrologue,
	.prologueSize = ByteCount(StateRemoveCallbackPrologue),
	.hook         = reinterpret_cast<void*>(&HookStateRemoveCallback),
	.original     = reinterpret_cast<void**>(&OriginalStateRemoveCallback),
	.enabled      = &Config.skillFreezeAiResume,
};

auto InstallSkillFreezeAiResumeFix(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr) {
		return false;
	}

	if (!Config.pluginEnabled || !Config.skillFreezeAiResume) {
		SetGuardState(Guard::SkillFreezeAiResume, GuardState::DisabledByConfig);
		D2RL::LogInfoF(context, "%s not installed: turned off in the config file.", SkillFreezeFixLabel);
		return false;
	}

	struct Witness {
		std::uint64_t       rva;
		const std::uint8_t* bytes;
		std::uint32_t       size;
		const char*         what;
	};
	const Witness witnesses[] {
		{ RemoveFreezeStateRva, RemoveFreezeStateEntry, ByteCount(RemoveFreezeStateEntry), "cold-damage freeze thaw at RVA 00452240" },
		{ UnitIsDeadRva, UnitIsDeadEntry, ByteCount(UnitIsDeadEntry), "unit death predicate at RVA 0034C2C0" },
	};
	for (const Witness& witness : witnesses) {
		if (!context->CheckExpectedBytes(witness.rva, witness.bytes, witness.size)) {
			SetGuardState(Guard::SkillFreezeAiResume, GuardState::UnsupportedBuild);
			D2RL::LogErrorF(
				context,
				"%s NOT installed: the %s does not match the verified 3.3.93847 bytes. "
				"This build is not supported; nothing was patched.",
				SkillFreezeFixLabel,
				witness.what);
			return false;
		}
	}

	const auto imageBase = static_cast<std::uintptr_t>(context->exeBase);
	RemoveFreezeState    = reinterpret_cast<StateRemoveCallbackFn>(imageBase + RemoveFreezeStateRva);
	UnitIsDead           = reinterpret_cast<UnitIsDeadFn>(imageBase + UnitIsDeadRva);

	if (!InstallInlineGuard(context, SkillFreezeAiResumeSite)) {
		RemoveFreezeState = nullptr;
		UnitIsDead        = nullptr;
		return false;
	}

	return true;
}


// ---------------------------------------------------------------------------
// Relay page: the frozen orb fix
// ---------------------------------------------------------------------------
//
// Both frozen orb sites sit in the middle of pSrvHitFunc 29, where the loader's
// inline hook cannot go: the bytes they replace include a branch or call, which
// does not relocate. Each site is rewritten in place to reach one page
// allocated within rel32 reach of the image:
//
//   +00h  frozen orb hit-unit spill stub, native code only
//   +40h  frozen orb burst gate stub, native code only
//
// The page is never freed. A thread may be jumping through it at any time, and
// the native stubs need nothing from this DLL.

constexpr std::size_t RelayPageBytes           = 4096;
constexpr std::size_t FrozenOrbSpillStubOffset = 0x00;
constexpr std::size_t FrozenOrbGateStubOffset  = 0x40;

static_assert(FrozenOrbSpillStubOffset + sizeof(FrozenOrbSpillStub) <= FrozenOrbGateStubOffset, "Frozen orb spill stub overlaps the frozen orb gate stub.");
static_assert(FrozenOrbGateStubOffset + sizeof(FrozenOrbGateStub) <= RelayPageBytes, "Frozen orb gate stub does not fit the relay page.");

constexpr const char* FrozenOrbFixLabel = "frozen orb burst-on-wall fix";

const D2RL::PluginContext* LoadedContext    = nullptr;
void*                      RelayPage        = nullptr;
bool                       RelayPageRefused = false;

bool FrozenOrbSpillSitePatched = false;
bool FrozenOrbGateSitePatched  = false;
std::array<std::uint8_t, sizeof(FrozenOrbSpillTemplate)> FrozenOrbSpillSiteWritten {};
std::array<std::uint8_t, sizeof(FrozenOrbGateTemplate)>  FrozenOrbGateSiteWritten {};

auto WithinRel32(std::uintptr_t next, std::uintptr_t target) noexcept -> bool {
	const std::int64_t delta = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(next);
	return delta >= INT32_MIN && delta <= INT32_MAX;
}

auto AllocateNear(std::uintptr_t hint, std::size_t size) noexcept -> void* {
	SYSTEM_INFO systemInfo {};
	GetSystemInfo(&systemInfo);
	const auto granularity = static_cast<std::uintptr_t>(systemInfo.dwAllocationGranularity);
	const auto aligned     = hint & ~(granularity - 1U);
	for (std::uintptr_t delta = granularity; delta < 0x70000000ULL; delta += granularity) {
		const auto candidate = aligned + delta;
		if (!WithinRel32(hint, candidate + size)) {
			break;
		}
		if (auto* memory = VirtualAlloc(reinterpret_cast<void*>(candidate), size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) {
			return memory;
		}
	}
	return nullptr;
}

// Allocates and fills the page once, then makes it execute-only. A failure is
// remembered so the second relay user does not retry and log twice.
auto PrepareRelayPage(const D2RL::PluginContext* context) noexcept -> bool {
	if (RelayPage != nullptr) {
		return true;
	}
	if (RelayPageRefused) {
		return false;
	}
	RelayPageRefused = true;

	const auto imageBase = static_cast<std::uintptr_t>(context->exeBase);
	void*      page      = AllocateNear(imageBase + FrozenOrbHitRva, RelayPageBytes);
	if (page == nullptr) {
		context->LogError("No relay page could be allocated within rel32 reach of the game image.");
		return false;
	}

	auto* bytes = static_cast<std::uint8_t*>(page);
	std::memset(bytes, 0xCC, RelayPageBytes);

	const std::uint64_t spillResume = imageBase + FrozenOrbSpillResumeRva;
	std::memcpy(bytes + FrozenOrbSpillStubOffset, FrozenOrbSpillStub, sizeof(FrozenOrbSpillStub));
	std::memcpy(bytes + FrozenOrbSpillStubOffset + FrozenOrbSpillResumeTargetOffset, &spillResume, sizeof(spillResume));

	const std::uint64_t burstRefuse = imageBase + FrozenOrbEpilogueRva;
	const std::uint64_t burstSpawn  = imageBase + FrozenOrbBurstRva;
	std::memcpy(bytes + FrozenOrbGateStubOffset, FrozenOrbGateStub, sizeof(FrozenOrbGateStub));
	std::memcpy(bytes + FrozenOrbGateStubOffset + FrozenOrbGateRefuseTargetOffset, &burstRefuse, sizeof(burstRefuse));
	std::memcpy(bytes + FrozenOrbGateStubOffset + FrozenOrbGateBurstTargetOffset, &burstSpawn, sizeof(burstSpawn));

	DWORD previous = 0;
	if (!VirtualProtect(page, RelayPageBytes, PAGE_EXECUTE_READ, &previous)) {
		context->LogError("The relay page could not be made executable.");
		VirtualFree(page, 0, MEM_RELEASE);
		return false;
	}
	FlushInstructionCache(GetCurrentProcess(), page, RelayPageBytes);

	RelayPage        = page;
	RelayPageRefused = false;
	return true;
}

auto InstallFrozenOrbBurstOnWallFix(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr) {
		return false;
	}

	if (!Config.pluginEnabled || !Config.frozenOrbBurstOnWall) {
		SetGuardState(Guard::FrozenOrbBurstOnWall, GuardState::DisabledByConfig);
		D2RL::LogInfoF(context, "%s not installed: turned off in the config file.", FrozenOrbFixLabel);
		return false;
	}

	if (!context->CheckExpectedBytes(FrozenOrbHitRva, FrozenOrbHitBody, ByteCount(FrozenOrbHitBody))) {
		SetGuardState(Guard::FrozenOrbBurstOnWall, GuardState::UnsupportedBuild);
		D2RL::LogErrorF(
			context,
			"%s NOT installed: the bytes at RVA 0045FB50 do not match the verified 3.3.93847 pSrvHitFunc 29. "
			"This build is not supported, or another patch already owns the site; nothing was patched.",
			FrozenOrbFixLabel);
		return false;
	}

	if (!PrepareRelayPage(context)) {
		SetGuardState(Guard::FrozenOrbBurstOnWall, GuardState::InstallFailed);
		D2RL::LogErrorF(context, "%s NOT installed: no relay page.", FrozenOrbFixLabel);
		return false;
	}

	const auto imageBase = static_cast<std::uintptr_t>(context->exeBase);
	const auto relayPage = reinterpret_cast<std::uintptr_t>(RelayPage);

	const auto spillNext   = imageBase + FrozenOrbSpillSiteRva + FrozenOrbSpillSiteRel32Offset + sizeof(std::int32_t);
	const auto spillTarget = relayPage + FrozenOrbSpillStubOffset;
	const auto gateNext    = imageBase + FrozenOrbGateSiteRva + FrozenOrbGateSiteRel32Offset + sizeof(std::int32_t);
	const auto gateTarget  = relayPage + FrozenOrbGateStubOffset;
	if (!WithinRel32(spillNext, spillTarget) || !WithinRel32(gateNext, gateTarget)) {
		SetGuardState(Guard::FrozenOrbBurstOnWall, GuardState::InstallFailed);
		D2RL::LogErrorF(context, "%s NOT installed: the relay page is out of rel32 reach of RVA 0045FB50.", FrozenOrbFixLabel);
		return false;
	}

	const auto spillDisplacement = static_cast<std::int32_t>(static_cast<std::int64_t>(spillTarget) - static_cast<std::int64_t>(spillNext));
	std::memcpy(FrozenOrbSpillSiteWritten.data(), FrozenOrbSpillTemplate, sizeof(FrozenOrbSpillTemplate));
	std::memcpy(FrozenOrbSpillSiteWritten.data() + FrozenOrbSpillSiteRel32Offset, &spillDisplacement, sizeof(spillDisplacement));

	const auto gateDisplacement = static_cast<std::int32_t>(static_cast<std::int64_t>(gateTarget) - static_cast<std::int64_t>(gateNext));
	std::memcpy(FrozenOrbGateSiteWritten.data(), FrozenOrbGateTemplate, sizeof(FrozenOrbGateTemplate));
	std::memcpy(FrozenOrbGateSiteWritten.data() + FrozenOrbGateSiteRel32Offset, &gateDisplacement, sizeof(gateDisplacement));

	// The spill goes in first because on its own it is inert: it writes the hit
	// unit into a stack slot the function overwrites later and nothing reads it
	// until the gate is in. The gate on its own would read an uninitialised
	// slot, so if it cannot be written the spill is taken back out again.
	if (!context->PatchBytes(
			FrozenOrbSpillSiteRva,
			FrozenOrbSpillOriginal,
			ByteCount(FrozenOrbSpillOriginal),
			FrozenOrbSpillSiteWritten.data(),
			static_cast<std::uint32_t>(FrozenOrbSpillSiteWritten.size()))) {
		SetGuardState(Guard::FrozenOrbBurstOnWall, GuardState::InstallFailed);
		D2RL::LogErrorF(context, "%s NOT installed: PatchBytes failed at RVA 0045FB6D.", FrozenOrbFixLabel);
		return false;
	}
	FrozenOrbSpillSitePatched = true;

	if (!context->PatchBytes(
			FrozenOrbGateSiteRva,
			FrozenOrbGateOriginal,
			ByteCount(FrozenOrbGateOriginal),
			FrozenOrbGateSiteWritten.data(),
			static_cast<std::uint32_t>(FrozenOrbGateSiteWritten.size()))) {
		if (context->PatchBytes(
				FrozenOrbSpillSiteRva,
				FrozenOrbSpillSiteWritten.data(),
				static_cast<std::uint32_t>(FrozenOrbSpillSiteWritten.size()),
				FrozenOrbSpillOriginal,
				ByteCount(FrozenOrbSpillOriginal))) {
			FrozenOrbSpillSitePatched = false;
		}
		SetGuardState(Guard::FrozenOrbBurstOnWall, GuardState::InstallFailed);
		D2RL::LogErrorF(context, "%s NOT installed: PatchBytes failed at RVA 0045FBB5.", FrozenOrbFixLabel);
		return false;
	}
	FrozenOrbGateSitePatched = true;

	SetGuardState(Guard::FrozenOrbBurstOnWall, GuardState::Installed);
	D2RL::LogInfoF(context, "%s installed at RVA 0045FB6D and RVA 0045FBB5.", FrozenOrbFixLabel);
	return true;
}

// On unload every site gets its original bytes back, gate first. The page
// itself is kept: a thread may still be inside a stub.
void WithdrawRelaySites() noexcept {
	const D2RL::PluginContext* context = LoadedContext;
	if (context == nullptr || RelayPage == nullptr) {
		return;
	}

	// Gate first: with only the spill left the function is stock behaviour plus
	// one dead store, which is the safe order to be caught halfway through.
	if (FrozenOrbGateSitePatched
		&& context->PatchBytes(
			FrozenOrbGateSiteRva,
			FrozenOrbGateSiteWritten.data(),
			static_cast<std::uint32_t>(FrozenOrbGateSiteWritten.size()),
			FrozenOrbGateOriginal,
			ByteCount(FrozenOrbGateOriginal))) {
		FrozenOrbGateSitePatched = false;
	}

	if (FrozenOrbSpillSitePatched
		&& context->PatchBytes(
			FrozenOrbSpillSiteRva,
			FrozenOrbSpillSiteWritten.data(),
			static_cast<std::uint32_t>(FrozenOrbSpillSiteWritten.size()),
			FrozenOrbSpillOriginal,
			ByteCount(FrozenOrbSpillOriginal))) {
		FrozenOrbSpillSitePatched = false;
	}
}


// ---------------------------------------------------------------------------
// Console command
// ---------------------------------------------------------------------------

void ReportGuard(
	const D2RL::PluginContext* context,
	Guard                      guard,
	const char*                label,
	const char*                eventNoun) noexcept {
	char message[256] {};

	switch (GetGuardState(guard)) {
	case GuardState::Installed:
		if (eventNoun != nullptr) {
			std::snprintf(
				message,
				sizeof(message),
				"%s: active. %s so far: %llu",
				label,
				eventNoun,
				static_cast<unsigned long long>(SuppressedCount(guard)));
		} else {
			std::snprintf(message, sizeof(message), "%s: active.", label);
		}
		context->WriteConsoleMessage(message);
		break;

	case GuardState::DisabledByConfig:
		std::snprintf(
			message,
			sizeof(message),
			"%s: OFF because the config file turns it off. The game build is fine.",
			label);
		context->WriteConsoleWarning(message);
		break;

	case GuardState::UnsupportedBuild:
		std::snprintf(
			message,
			sizeof(message),
			"%s: NOT ACTIVE. This game build is not recognised and nothing was patched.",
			label);
		context->WriteConsoleError(message);
		break;

	case GuardState::InstallFailed:
		std::snprintf(
			message,
			sizeof(message),
			"%s: NOT ACTIVE. The bytes matched but it could not be installed. See the plugin log.",
			label);
		context->WriteConsoleError(message);
		break;

	case GuardState::NotAttempted:
	default:
		std::snprintf(message, sizeof(message), "%s: NOT ACTIVE. Installation was never attempted.", label);
		context->WriteConsoleError(message);
		break;
	}
}

auto EngineStabilityCommand(
	D2R::Game::Client*                  client,
	const D2RL::ConsoleCommandContext*  command,
	void*                               userData) noexcept -> D2RL::ConsoleCommandResult {
	(void)client;
	(void)userData;

	if (command == nullptr || command->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}

	const D2RL::PluginContext* context = command->plugin;

	char header[160] {};
	std::snprintf(
		header,
		sizeof(header),
		"engine-stability: config %s, plugin %s.",
		ConfigFileWasRead ? "loaded" : "NOT READ (defaults in use)",
		Config.pluginEnabled ? "enabled" : "DISABLED in config");
	context->WriteConsoleMessage(header);

	ReportGuard(context, Guard::FrozenOrbBurstOnWall, FrozenOrbFixLabel, nullptr);
	ReportGuard(context, Guard::SkillFreezeAiResume, SkillFreezeFixLabel, "skill-freeze thaws handed to the engine thaw");

	return D2RL::ConsoleCommandResult::Handled;
}

constexpr D2RL::PluginInfo EngineStabilityInfo {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = "celestialrayone.engine-stability",
	.name        = "Engine Stability",
	.version     = "0.11.0",
	.author      = "CelestialRayOne",
	.description = "Vanilla engine bug fixes for Diablo II: Resurrected.",
	.flags       = D2RL::PluginFlags::Client | D2RL::PluginFlags::NativeHooks,
};

static_assert(D2RL::HasValidPluginRole(EngineStabilityInfo.flags), "Exactly one execution role must be set.");
static_assert(D2RL::HasOnlyKnownPluginFlags(EngineStabilityInfo.flags), "Unknown plugin flag set.");
static_assert(D2RL::HasFlag(EngineStabilityInfo.flags, D2RL::PluginFlags::NativeHooks), "Inline hooks require the NativeHooks flag.");

} // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
	return &EngineStabilityInfo;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr) {
		return false;
	}

	LoadedContext = context;

	// Registered before anything is installed on purpose. Returning false from
	// this function unloads the DLL and takes the console command with it,
	// which would leave a user on an unsupported build with no in-game signal
	// at all. This plugin would rather stay loaded and be able to say that it
	// did nothing.
	if (!context->RegisterConsoleCommand(
			"engine-stability",
			EngineStabilityCommand,
			"Report which engine stability fixes are active.")) {
		context->LogWarn("The engine-stability console command was not registered.");
	}

	if (const char* build = D2RL::GetBuildVersion(context); build != nullptr) {
		D2RL::LogInfoF(context, "engine-stability loading against build %s.", build);
	}

	LoadConfiguration(context);

	if (!Config.pluginEnabled) {
		SetGuardState(Guard::FrozenOrbBurstOnWall, GuardState::DisabledByConfig);
		SetGuardState(Guard::SkillFreezeAiResume, GuardState::DisabledByConfig);
		context->LogWarn("engine-stability is disabled in its config file. Nothing was installed.");
		return true;
	}

	// Each fix is installed independently. One refusing, whether because the
	// config turned it off or because its site did not verify, must never take
	// the other down with it.
	unsigned installed = 0;

	if (InstallFrozenOrbBurstOnWallFix(context)) {
		++installed;
	}

	if (InstallSkillFreezeAiResumeFix(context)) {
		++installed;
	}

	if (installed == 0) {
		context->LogError("engine-stability loaded with NO fixes active.");
		return true;
	}

	D2RL::LogInfoF(context, "engine-stability loaded with %u of %zu fixes active.", installed, GuardCount);
	return true;
}

// The loader's inline hook cannot be withdrawn and its trampoline lives in this
// module, so the loader is expected to keep the DLL resident for the life of
// the process. The relay sites can be withdrawn, so they are.
D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
	WithdrawRelaySites();
}
