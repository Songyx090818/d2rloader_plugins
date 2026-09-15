// ---------------------------------------------------------------------------
// engine-stability
//
// Crash guards and vanilla engine bug fixes, each behind its own switch.
//
// A guard exists to stop a crash. It is either a no-op or the difference
// between a hard crash and a normal frame, and where stopping the crash also
// changes what the engine does, its section below says exactly what.
//
// A fix corrects a vanilla engine bug that is not a crash. Fixes do change
// gameplay: they make the engine apply what its own data already says.
//
// Target: D2RLoader 1.3.0, which runs Diablo II: Resurrected 3.3 inside
// D2RLoader.exe and moves part of the engine into D2RCore.dll.
//
// Safety model: every game address below is an RVA against image base
// 0x140000000, and nothing is installed unless the bytes already at that RVA
// match byte for byte. Three entries patch D2RCore.dll instead; those are found
// through named D2RCore exports and verified the same way before a byte is
// written (see "D2RCore sites"). On any build the plugin does not recognise it installs nothing, logs
// loudly and stays loaded so the console command can explain why. An
// unrecognised build is therefore a no-op, never a mis-patch.
//
// Configuration: every guard and fix has an on/off switch in
// <scope>\d2rloader\config\celestialrayone.engine-stability.toml, all true by
// default. The file is created with documented defaults on first run. The
// config is read once at load, so edits need a game restart. A guard that the
// config turned off and a guard the build refused are reported differently by
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

// ---------------------------------------------------------------------------
// Guard identity
// ---------------------------------------------------------------------------

enum class Guard : std::size_t {
	DeadUnitLookup,
	LifeDrainWhileDead,
	EventHandlerRecursion,
	MissileSkillAttackRating,
	DurabilityGearRefresh,
	FrozenOrbBurstOnWall,
	ZeroSkillDescriptionLine,
	ResocketStatInflation,
	BloodManaCastGate,
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
// Guard 1: client unit-by-id hash lookup, tombstoned unit id
// ---------------------------------------------------------------------------
//
// sub_14009F270 is the shared bottom of the client's unit-by-id lookup. Its
// public wrapper sub_14009A5D0(unitId, unitType) resolves the per-type bucket
// array out of the client unit hash table at 0x2A23910 and forwards here:
//
//     sub_14009F270(&hashTable[128 * unitType], unitId & 0x7F, unitId, unitType)
//
// The function walks a chain and compares [node+8] against the requested id and
// [node] against the requested type. It never validates the id it was handed.
//
// The failure it has to be guarded against: a unit that dies while something it
// spawned is still alive. Freeing a unit stamps a tombstone on the block (unit
// id -1) and returns it to the pool, so every later resolution of that owner
// arrives here with id -1. That masks to bucket 0x7F and the walk follows
// whatever that slot holds, which is where the dangling chain node is.
//
// The guard: an id of -1 is never a live unit, so answer "not found" without
// walking. Returning null is this function's own not-found result -- it already
// ends in xor eax,eax / retn when the chain runs out -- so every one of its
// call sites already handles it. That is what makes this safe to do in the
// callee rather than in any particular caller, which matters because the
// captured 2.4 dumps arrived through more than one caller.
//
// Verified body at RVA 0x09F270 (41 bytes):
//
//   0009F270  48 63 C2                 movsxd rax, edx
//   0009F273  48 8B 04 C1              mov    rax, [rcx+rax*8]
//   0009F277  48 85 C0                 test   rax, rax
//   0009F27A  74 1A                    jz     0009F296
//   0009F27C  44 39 40 08              cmp    [rax+8], r8d
//   0009F280  75 05                    jnz    0009F287
//   0009F282  44 39 08                 cmp    [rax], r9d
//   0009F285  74 11                    jz     0009F298
//   0009F287  48 8B 88 58 01 00 00     mov    rcx, [rax+158h]
//   0009F28E  48 8B C1                 mov    rax, rcx
//   0009F291  48 85 C9                 test   rcx, rcx
//   0009F294  EB E4                    jmp    0009F27A
//   0009F296  33 C0                    xor    eax, eax
//   0009F298  C3                       retn
//
// The hook displaces the first 7 bytes (two instructions). Both are position
// independent -- no rip-relative operand, no branch -- so they relocate into
// the trampoline cleanly. No branch inside the function targets any address in
// 0x09F270..0x09F276, so nothing can land in the middle of the displaced
// region.

constexpr std::uint64_t UnitHashLookupRva = 0x0009F270ULL;

// Checked in full before anything is installed. Verifying the whole body, not
// just the bytes being displaced, means the plugin also refuses on a build
// where the not-found tail this guard relies on has changed.
constexpr std::uint8_t UnitHashLookupBody[] {
	0x48, 0x63, 0xC2,
	0x48, 0x8B, 0x04, 0xC1,
	0x48, 0x85, 0xC0,
	0x74, 0x1A,
	0x44, 0x39, 0x40, 0x08,
	0x75, 0x05,
	0x44, 0x39, 0x08,
	0x74, 0x11,
	0x48, 0x8B, 0x88, 0x58, 0x01, 0x00, 0x00,
	0x48, 0x8B, 0xC1,
	0x48, 0x85, 0xC9,
	0xEB, 0xE4,
	0x33, 0xC0,
	0xC3,
};

// The bytes the inline hook displaces: movsxd rax, edx / mov rax, [rcx+rax*8].
constexpr std::uint8_t UnitHashLookupPrologue[] {
	0x48, 0x63, 0xC2,
	0x48, 0x8B, 0x04, 0xC1,
};

constexpr std::int32_t TombstoneUnitId = -1;

static_assert(sizeof(UnitHashLookupBody) == 41, "Verified body length changed.");
static_assert(sizeof(UnitHashLookupPrologue) == 7, "Verified prologue length changed.");
static_assert(sizeof(UnitHashLookupPrologue) >= 5, "An inline hook needs at least 5 displaced bytes for a rel32 jump.");
static_assert(IsLeadingBytesOf(UnitHashLookupPrologue, UnitHashLookupBody), "The displaced bytes must be the leading bytes of the verified body.");

using UnitHashLookupFn = void*(__fastcall*)(
	void*         bucketArray,
	std::int32_t  bucketIndex,
	std::int32_t  unitId,
	std::int32_t  unitType) noexcept;

UnitHashLookupFn OriginalUnitHashLookup = nullptr;

auto __fastcall HookUnitHashLookup(
	void*        bucketArray,
	std::int32_t bucketIndex,
	std::int32_t unitId,
	std::int32_t unitType) noexcept -> void* {
	if (unitId == TombstoneUnitId) {
		CountSuppressed(Guard::DeadUnitLookup);
		return nullptr;
	}

	const UnitHashLookupFn original = OriginalUnitHashLookup;
	return original != nullptr ? original(bucketArray, bucketIndex, unitId, unitType) : nullptr;
}

// ---------------------------------------------------------------------------
// Guard 5: life stolen per hit applied to a player that is already dead
// ---------------------------------------------------------------------------
//
// "Life stolen per hit" is ItemEventFunc 28, the slot both itemstatcost's
// itemeventfunc columns and skills.txt's auraeventfunc columns index.
//
// D2RLOADER 1.3.0 MOVED THE HANDLER. Event nodes are registered through D2RCore,
// with D2RCore's own handler table, and its entry 28 no longer reaches the game's
// life-drain applier at 00583A70. It calls D2RCore's shared leech routine in mode
// 3 (D2RCore.dll as shipped with 1.3.0, RVAs in that DLL):
//
//   003DBB20  handler 28
//     003DBB33  test r8, r8 / je            no unit: nothing
//     003DBB38  mov  rax, [rsp+78h]          packed stat, the 6th argument
//     003DBB42  mov  dword [rsp+20h], 0
//     003DBB4F  mov  edx, 3                  mode 3
//     003DBB54  call 003E2820                leech routine
//
//   003E2820  leech routine (this, mode, unit, other, percent, packed stat)
//     003E2848  mov  rsi, r8                 the leecher, read and written
//     003E284B  mov  edi, edx                mode
//     003E28D2  cmp  edi, 2 / setb al
//     003E28D8  lea  r14d, [rax*2+6]         modes 2 and 3 write stat 6, life,
//     003E28E0  lea  edx, [rax*2+7]          capped by stat 7; 0 and 1 are mana
//
// It reads the drain stat, reads current and maximum life, adds the amount,
// clamps, writes life back and schedules event 151. Nothing in it asks whether
// the unit is alive, so the bug the 1.2.x guard stopped is unchanged.
//
// What goes wrong without the guard: after UNITS_Die has committed life = 0,
// the player spends the whole death animation in modes DT and DD before the
// corpse-spawn pass finishes. A leeching missile that lands in that window
// runs this routine against the corpse, and it puts life back. The accumulated
// life is then cashed in at the anim-complete transition and the player
// revives in place: the server has them dead, the client has them alive with
// positive life, and nothing but Save and Exit clears it. ESR reaches this
// constantly because its auras keep spawning leeching missiles after the
// caster dies.
//
// The guard: in mode 3, if the leecher is a player in a death animation, return
// 0 without touching life and without scheduling the event. 0 is the routine's
// own "did nothing" return, used by all of its early exits, so handler 28 and
// the dispatcher already handle it. Modes 0 to 2, living players, monsters and
// hirelings are not touched, because the gate requires mode 3, unit type 0 and
// a death mode.
//
// The two unit fields and both mode numbers:
//   +0x00 dwType      the leech routine's own player test is cmp dword [rsi], 0
//   +0x0C dwMode      the only head dword left; +0x10 is the unit-data pointer
//   type 0            player
//   mode 0 / mode 17  PMODE_DEATH / PMODE_DEAD, 17 slots apart in the engine's
//                     own mode-name table
//
// The routine is hooked at its entry. Its first 14 bytes are seven pushes and
// sub rsp, 40h, all position independent; see "D2RCore sites" for how it is
// found and verified.

constexpr std::size_t UnitTypeOffset     = 0x00;
constexpr std::size_t UnitAnimModeOffset = 0x0C;

constexpr std::int32_t UnitTypePlayer  = 0;
constexpr std::int32_t PlayerModeDying = 0;   // PMODE_DEATH, the death animation
constexpr std::int32_t PlayerModeDead  = 17;  // PMODE_DEAD, the corpse

// Mode 3 of D2RCore's leech routine: flat life per hit, handler 28.
constexpr std::uint32_t CoreLeechModeLifePerHit = 3;

// Reads the two head fields without assuming anything about alignment or
// aliasing. Null is not a dead player: the routine's own null check should be
// the one that runs, not this guard.
auto IsPlayerInDeathAnimation(const void* unit) noexcept -> bool {
	if (unit == nullptr) {
		return false;
	}

	const auto* head = static_cast<const unsigned char*>(unit);

	std::int32_t unitType = 0;
	std::int32_t animMode = 0;
	std::memcpy(&unitType, head + UnitTypeOffset, sizeof(unitType));
	std::memcpy(&animMode, head + UnitAnimModeOffset, sizeof(animMode));

	return unitType == UnitTypePlayer
		&& (animMode == PlayerModeDying || animMode == PlayerModeDead);
}

// Six arguments: rcx, rdx, r8, r9 and two stack slots, all carried as full 64
// bit values so the forwarded call is bit-identical to the one the handler made.
using CoreLeechFn = std::int64_t(__fastcall*)(
	std::uint64_t self,
	std::uint64_t mode,
	const void*   leecher,
	std::uint64_t other,
	std::uint64_t percent,
	std::uint64_t packedStat) noexcept;

CoreLeechFn OriginalCoreLeech = nullptr;

auto __fastcall HookCoreLeech(
	std::uint64_t self,
	std::uint64_t mode,
	const void*   leecher,
	std::uint64_t other,
	std::uint64_t percent,
	std::uint64_t packedStat) noexcept -> std::int64_t {
	if (static_cast<std::uint32_t>(mode) == CoreLeechModeLifePerHit && IsPlayerInDeathAnimation(leecher)) {
		CountSuppressed(Guard::LifeDrainWhileDead);
		return 0;
	}

	const CoreLeechFn original = OriginalCoreLeech;
	return original != nullptr ? original(self, mode, leecher, other, percent, packedStat) : 0;
}

// True when a mov rax, imm64 / call rax pair brackets the eight byte slot at
// `offset`, so a target written there is the one that instruction calls.
template <std::size_t Size>
constexpr auto HasMovRaxImm64CallBefore(const std::uint8_t (&code)[Size], std::size_t offset) noexcept -> bool {
	return offset >= 2
		&& offset + 10 <= Size
		&& code[offset - 2] == 0x48
		&& code[offset - 1] == 0xB8
		&& code[offset + 8] == 0xFF
		&& code[offset + 9] == 0xD0;
}

// ---------------------------------------------------------------------------
// Guard 7: event handler recursion
// ---------------------------------------------------------------------------
//
// D2RLOADER 1.3.0 MOVED THE DISPATCHER. The game's unit event dispatcher at
// 005881E0 now starts with a jump to D2RCore!DispatchWideEffects, so the loop the
// 1.2.x guard patched is dead code. D2RCore's loop walks the same handler nodes
// at [unit+0E0h], and for each node whose event id matches it marks the node in
// progress, runs the handler, then clears the mark (D2RCore.dll RVAs):
//
//   003DC4B0  mov  r15, [r12+38h]            next node
//   003DC4B5  mov  r12, r15
//   003DC4B8  test r15, r15 / je 003DC682     end of list
//   003DC4C1  movzx eax, byte [r12]           node event id
//   003DC4C6  cmp  edi, eax                   requested event id     <- site
//   003DC4C8  jne  003DC4B0                   next node
//   003DC4CA  movzx r13d, word [r12+2]        flags as they were on entry
//   003DC4D0  mov  eax, r13d
//   003DC4D3  or   eax, 1
//   003DC4D6  mov  word [r12+2], ax           mark in progress
//   ...                                       run the handler
//   003DC60F  test r13b, 1
//   003DC613  jne  003DC4B5                   nested run: keep the mark
//   003DC619  movzx eax, word [r12+2]
//   003DC621  and  ecx, 0FFFFFFFEh
//   003DC624  mov  word [r12+2], cx           clear the mark
//
// What goes wrong: a handler can raise its own event on the same unit before it
// returns. The dispatcher then reaches the same node, still marked in progress,
// and runs its handler again, with nothing limiting the depth. The chain behind
// the 2.4 crash: event 7 (domeleeattack) runs the chance to cast on attack
// handler, the skill it casts drains item durability, and the durability drain
// raises event 7 again. A proc that always fires recurses until the stack
// overflows. D2RCore's loop keeps the vanilla design exactly: r13b remembers
// whether the mark was already set, so a nested run neither clears the mark nor
// frees the node, but it still runs the handler.
//
// The guard: a node only matches when its event id matches AND it is not in
// progress. A node whose handler is already running is skipped exactly like a
// node for a different event. Every other node still runs in the nested
// dispatch, and the outer run of the skipped node finishes untouched. Same test
// as the shipped 2.4 fix (hook 389EE7, cave 3633C4) and the 1.2.x guard.
//
// What changes besides the crash: a proc can no longer trigger itself from
// inside its own handler. In stock, a chance-based proc could chain into another
// roll of itself during the same attack; that nested roll no longer happens.
// Other procs are unaffected.
//
// The site is rewritten in place, 10 bytes:
//
//   003DC4C6  E9 <rel32>               jmp    D2RCore relay page + 00h
//   003DC4CB  90 90 90 90 90           never executed
//
// and the relay stub does the original instructions plus the new test:
//
//   +00  39 C7                         cmp    edi, eax
//   +02  75 1C                         jne    +20
//   +04  41 F6 44 24 02 01             test   byte ptr [r12+2], 1
//   +0A  75 14                         jne    +20
//   +0C  45 0F B7 6C 24 02             movzx  r13d, word ptr [r12+2]
//   +12  FF 25 00 00 00 00 <abs64>     jmp    003DC4D0                 ; run the handler
//   +20  FF 25 00 00 00 00 <abs64>     jmp    003DC4B0                 ; next node
//
// Flags are dead on both exits: 003DC4D3 rewrites them before anything reads
// them, and 003DC4B0 reaches test r15, r15 first. eax is reloaded at 003DC4C1
// on the next-node path and overwritten at 003DC4D0 on the run path. r13 is only
// read after the handler call, so leaving it stale on the skip path is harmless.
// Nothing branches into 003DC4C7..003DC4CF. The match window 003DC4B0..003DC4DB
// and the unmark window 003DC608..003DC629 are verified before patching.

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

constexpr std::uint8_t CoreRecursionSiteOriginal[] {
	0x39, 0xC7,                          // cmp edi, eax
	0x75, 0xE6,                          // jne 003DC4B0
	0x45, 0x0F, 0xB7, 0x6C, 0x24, 0x02,  // movzx r13d, word ptr [r12+2]
};

// jmp rel32 to the relay stub, rel32 filled at install / five dead NOPs.
constexpr std::uint8_t CoreRecursionSiteTemplate[] {
	0xE9, 0x00, 0x00, 0x00, 0x00,
	0x90, 0x90, 0x90, 0x90, 0x90,
};

constexpr std::uint8_t CoreRecursionStub[] {
	0x39, 0xC7,
	0x75, 0x1C,
	0x41, 0xF6, 0x44, 0x24, 0x02, 0x01,
	0x75, 0x14,
	0x45, 0x0F, 0xB7, 0x6C, 0x24, 0x02,
	0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

constexpr std::size_t CoreRecursionRunTargetOffset  = 0x18;
constexpr std::size_t CoreRecursionNextTargetOffset = 0x26;

static_assert(sizeof(CoreRecursionSiteOriginal) == sizeof(CoreRecursionSiteTemplate), "A byte patch must not change the length of the region it replaces.");
static_assert(sizeof(CoreRecursionStub) == 46, "Recursion stub length changed.");
static_assert(HasAbsoluteJumpBefore(CoreRecursionStub, CoreRecursionRunTargetOffset), "Run target must follow its jmp.");
static_assert(HasAbsoluteJumpBefore(CoreRecursionStub, CoreRecursionNextTargetOffset), "Next target must follow its jmp.");
static_assert(CoreRecursionStub[3] == 0x20 - 0x04 && CoreRecursionStub[11] == 0x20 - 0x0C, "Both stub branches must land on the next-node jump.");

// ---------------------------------------------------------------------------
// Fix 1: skill attack rating on missiles (vanilla bug, port of the 2.4 fix at 315B5D)
// ---------------------------------------------------------------------------
//
//   The missile resolver 4639A0 passes the MISSILE's own stat 19 as the skill
//   AR% bonus (463C4B..463C5F). Missile creation sub_1405371A0 (game, params)
//   writes that stat only when the creator set params flag 0x1000:
//
//     537B23  mov eax,[r14]             <- hook: r14 = params, r15 = missile
//     537B26  bt eax,0Ch / jae 537B42   flag 0x1000 clear: no stat 19
//     537B2C  mov r8d,[r14+58h]         params attack rating
//     537B3A  call 2F7D10               set unit stat (missile, 19, AR, 0)
//     537B3F  mov eax,[r14]
//     537B42  bt eax,11h                <- rejoin, expects eax = flags
//
//   CreateSkillMissile 4333F0 sets the flag for players from 339040 (unit,
//   skill, level), the skills.txt ToHit/LevToHit/ToHitCalc evaluator
//   (4336B5..4336DF). Missiles that skill functions build themselves, such as
//   Multiple Shot (srvdofunc 8), never get it. The hook keeps the flagged
//   path exactly and, for player owners only, evaluates 339040 from params
//   +3Ch skill and +40h level when the flag is clear. Params +8 is the owner.
//   Skill ids are bounds-checked against the skills table count (tables
//   +11B8h, as 097790 does) so the evaluator's assert path is never reached.
//   The params struct is never modified, so a builder that reuses it for its
//   next missile cannot pick up a stale value. No branch from outside the
//   hook lands in 537B24..537B41, and the function has no indirect jumps.
//
//   The site is rewritten in place, 16 bytes:
//
//     537B23  mov rcx,r14 / mov rdx,r15 / call relay page + 00h / mov eax,[r14] / jmp 537B42
//
//   The relay jumps into StampMissileAttackRating. Before the plugin unloads it
//   is pointed at a native stub that does exactly what the replaced bytes did,
//   then the original bytes go back.
//
//   Moved here unchanged from the Attack Rating plugin, where it shipped first.

constexpr std::uint64_t MissileCreationStampRva   = 0x00537B23ULL;
constexpr std::uint64_t CreateSkillMissileCallRva = 0x004336A2ULL;
constexpr std::uint64_t SkillRecordGetterRva      = 0x00097790ULL;

// Each callee is proven by a witnessed call site inside a window verified at load.
constexpr std::uint64_t SetUnitStatRva   = 0x002F7D10ULL;  // call at 537B3A
constexpr std::uint64_t SkillToHitRva    = 0x00339040ULL;  // call at 4336BE
constexpr std::uint64_t GetDataTablesRva = 0x00300A90ULL;  // call at 0977A2

constexpr std::size_t   UnitDataContextOffset           = 0x1BD;
constexpr std::size_t   SkillsCountOffset               = 0x11B8;  // cmp at 0977B1
constexpr std::size_t   MissileParamsFlagsOffset        = 0x00;
constexpr std::size_t   MissileParamsOwnerOffset        = 0x08;
constexpr std::size_t   MissileParamsSkillOffset        = 0x3C;
constexpr std::size_t   MissileParamsSkillLevelOffset   = 0x40;
constexpr std::size_t   MissileParamsAttackRatingOffset = 0x58;
constexpr std::uint32_t MissileParamsAttackRatingFlag   = 0x1000;
constexpr std::int32_t  StatToHit                       = 19;

// RVA 0x537B23, 58 bytes. Missile creation: the params flag 0x1000 stat 19 stamp and its rejoin.
constexpr std::uint8_t CreationStampWindow[] {
	0x41, 0x8B, 0x06, 0x0F, 0xBA, 0xE0, 0x0C, 0x73, 0x16, 0x45, 0x8B, 0x46,
	0x58, 0x45, 0x33, 0xC9, 0x49, 0x8B, 0xCF, 0x41, 0x8D, 0x51, 0x13, 0xE8,
	0xD1, 0x01, 0xDC, 0xFF, 0x41, 0x8B, 0x06, 0x0F, 0xBA, 0xE0, 0x11, 0x73,
	0x15, 0x49, 0x8B, 0xCF, 0xE8, 0x30, 0x35, 0xE8, 0xFF, 0x83, 0xC8, 0x02,
	0x49, 0x8B, 0xCF, 0x8B, 0xD0, 0xE8, 0x73, 0x58, 0xE8, 0xFF,
};

// RVA 0x4336A2, 62 bytes. CreateSkillMissile: skill ToHit evaluation and the call into missile creation.
constexpr std::uint8_t SkillToHitCallWindow[] {
	0x44, 0x0F, 0xB6, 0xC3, 0x48, 0x8B, 0xD7, 0x48, 0x8B, 0xCE, 0xE8, 0x6F,
	0x5C, 0x00, 0x00, 0x85, 0xC0, 0x7E, 0x2C, 0x45, 0x8B, 0xC6, 0x41, 0x8B,
	0xD7, 0x48, 0x8B, 0xCF, 0xE8, 0x7D, 0x59, 0xF0, 0xFF, 0x85, 0xC0, 0x74,
	0x0B, 0x81, 0x4C, 0x24, 0x40, 0x00, 0x10, 0x00, 0x00, 0x89, 0x45, 0xA7,
	0x48, 0x8D, 0x54, 0x24, 0x40, 0x48, 0x8B, 0xCE, 0xE8, 0xC1, 0x3A, 0x10,
	0x00, 0xEB,
};

// RVA 0x97790, 80 bytes. Skills table record getter: data tables call and the count at +11B8h.
constexpr std::uint8_t SkillRecordWindow[] {
	0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48,
	0x83, 0xEC, 0x30, 0x48, 0x63, 0xF2, 0xE8, 0xE9, 0x92, 0x26, 0x00, 0x48,
	0x8B, 0xF8, 0x48, 0x8B, 0xDE, 0x85, 0xF6, 0x78, 0x09, 0x48, 0x3B, 0x98,
	0xB8, 0x11, 0x00, 0x00, 0x72, 0x18, 0x48, 0x8D, 0x4C, 0x24, 0x48, 0xC6,
	0x44, 0x24, 0x48, 0x00, 0xE8, 0x57, 0xEC, 0xFE, 0xFF, 0x84, 0xC0, 0x74,
	0x01, 0xCC, 0x85, 0xF6, 0x78, 0x55, 0x48, 0x3B, 0x9F, 0xB8, 0x11, 0x00,
	0x00, 0x73, 0x4C, 0x48, 0x81, 0xC7, 0xB0, 0x11,
};

// Written at 537B23. rel32 filled at install.
constexpr std::uint8_t MissileHookCode[] {
	0x4C, 0x89, 0xF1, 0x4C, 0x89, 0xFA, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x41,
	0x8B, 0x06, 0xEB, 0x0F,
};

constexpr std::uint32_t MissileHookRel32Offset = 7;
constexpr std::size_t   MissileHookSize        = 16;

// If params flag 0x1000: set unit stat (missile, 19, params AR, 0), else return. Target filled at install.
constexpr std::uint8_t MissileFallbackStub[] {
	0xF7, 0x01, 0x00, 0x10, 0x00, 0x00, 0x75, 0x01, 0xC3, 0x44, 0x8B, 0x41,
	0x58, 0x45, 0x31, 0xC9, 0x48, 0x89, 0xD1, 0xBA, 0x13, 0x00, 0x00, 0x00,
	0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00,
};

constexpr std::size_t MissileFallbackTargetOffset = 30;

static_assert(sizeof(MissileHookCode) == MissileHookSize && MissileHookRel32Offset + 4 <= MissileHookSize, "Missile hook shape changed.");
static_assert(sizeof(CreationStampWindow) >= MissileHookSize, "The hook must sit inside the verified creation window.");
static_assert(CreationStampWindow[0x17] == 0xE8 && CallTargetRva(CreationStampWindow, MissileCreationStampRva, 0x17) == SetUnitStatRva, "SetUnitStat is not the callee witnessed at 537B3A.");
static_assert(SkillToHitCallWindow[0x1C] == 0xE8 && CallTargetRva(SkillToHitCallWindow, CreateSkillMissileCallRva, 0x1C) == SkillToHitRva, "SkillToHit is not the callee witnessed at 4336BE.");
static_assert(SkillRecordWindow[0x12] == 0xE8 && CallTargetRva(SkillRecordWindow, SkillRecordGetterRva, 0x12) == GetDataTablesRva, "GetDataTables is not the callee witnessed at 0977A2.");
static_assert(HasAbsoluteJumpBefore(MissileFallbackStub, MissileFallbackTargetOffset), "Fallback target must follow its jmp.");

using GetDataTablesFn = void* (*)(std::uint8_t context);
using SetUnitStatFn   = void (*)(void* unit, std::int32_t statId, std::int32_t value, std::int32_t layer);
using SkillToHitFn    = std::int32_t (*)(void* unit, std::int32_t skillId, std::int32_t skillLevel);

GetDataTablesFn GetDataTables = nullptr;
SetUnitStatFn   SetUnitStat   = nullptr;
SkillToHitFn    SkillToHit    = nullptr;

template <typename T>
auto ReadAt(const void* base, std::size_t offset) noexcept -> T {
	T value {};
	std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(T));
	return value;
}

// Reached from 537B23 through the relay page with rcx = missile creation
// params and rdx = the new missile. Replaces the params flag 0x1000 stamp of
// stat 19.
void StampMissileAttackRating(void* params, void* missile) noexcept {
	const auto flags = ReadAt<std::uint32_t>(params, MissileParamsFlagsOffset);
	if ((flags & MissileParamsAttackRatingFlag) != 0) {
		SetUnitStat(missile, StatToHit, ReadAt<std::int32_t>(params, MissileParamsAttackRatingOffset), 0);
		return;
	}

	void* const owner = ReadAt<void*>(params, MissileParamsOwnerOffset);
	if (owner == nullptr || ReadAt<std::int32_t>(owner, UnitTypeOffset) != UnitTypePlayer) {
		return;
	}

	const auto skill      = ReadAt<std::int32_t>(params, MissileParamsSkillOffset);
	const auto skillLevel = ReadAt<std::int32_t>(params, MissileParamsSkillLevelOffset);
	if (skill < 0 || skillLevel <= 0) {
		return;
	}

	const void* tables = GetDataTables(ReadAt<std::uint8_t>(owner, UnitDataContextOffset));
	if (tables == nullptr || static_cast<std::uint64_t>(skill) >= ReadAt<std::uint64_t>(tables, SkillsCountOffset)) {
		return;
	}

	const std::int32_t toHit = SkillToHit(owner, skill, skillLevel);
	if (toHit == 0) {
		return;
	}

	SetUnitStat(missile, StatToHit, toHit, 0);
	CountSuppressed(Guard::MissileSkillAttackRating);  // counts missiles stamped
}

// ---------------------------------------------------------------------------
// Fix 2: durability tick forces a full client gear refresh (port of the 2.4 fix at DAB1F)
// ---------------------------------------------------------------------------
//
//   Server: when a hit wears an item down, ApplyDurabilityLoss 441B10 writes the
//   new durability (stat 72) and sends it as server message 62, the item stat
//   update.
//
//   D2RLOADER 1.3.0 MOVED THE CLIENT SIDE. The game's message 62 handler now
//   starts with a jump to D2RCore!ReceiveWideItemStatPacket, so the stat apply
//   at 001C7710 that the 1.2.x fix patched is dead code. D2RCore decodes the
//   message and applies it in its own routine (D2RCore.dll RVAs), where the
//   decoded update is at rdi: +4 stat id, +8 value, +0Ch layer, +10h set flag:
//
//     003DE2D2  mov  ebx, [rdi+4]            stat id
//     003DE2D5  cmp  rbx, 0CCh               charged skills: own branch
//     003DE357  call                          set the stat on the item
//     003DE35C  call GAME+08B2D0 / GAME+09A480  local player
//     003DE372  je   003DE400                no local player: leave
//     003DE378  cmp  dword [rdi+4], 46h      <- site: stat 70 (quantity)?
//     003DE37C  jne  003DE3A6
//     003DE37E  ...                          quantity: its own small block
//     003DE3A6  call GAME+08B2D0 / GAME+09A480  local player again
//     003DE3BC  call GAME+1C9610             FULL gear refresh
//     003DE3C2  mov  al, 1                   shared "handled" exit
//     003DE3C4  jmp  003DE400
//
//   So every stat but 204 and 70 still runs the game's gear refresh 1C9610.
//   That function snapshots both hand slots, walks the 11 body locations,
//   re-merges usable charms, detaches and re-applies the stats of every equipped
//   item, re-selects BOTH mouse skills, and rebuilds the player composite if a
//   hand slot changed.
//
//   What goes wrong: durability ticks all the time in melee, so that teardown
//   runs mid-fight. A skill granted by an equipped item blips to 0 inside it and
//   the mouse skill is re-selected under the skill that is running. Whirlwind
//   granted by a weapon visibly stops spinning while the server keeps
//   whirlwinding; an indestructible weapon never sends the message and never
//   stutters.
//
//   Why skipping it is correct: an item's CURRENT durability cannot change what
//   is equipped or what any item grants. Breaking and un-breaking are separate
//   server paths (46E680 and 46E7A0) that change the item flags with their own
//   item update, and when the un-break path sends stat 72 the client item still
//   carries the broken flag, so the refresh it triggered could not change
//   anything either. The engine already skips the refresh for quantity.
//
//   The fix: stat 72 leaves through the shared exit 003DE3C2 after the stat has
//   been applied but before the refresh, with the same "handled" result the
//   refresh path returns. Every other stat takes exactly the stock path.
//
//   The site is rewritten in place, 6 bytes:
//
//     003DE378  E9 <rel32> 90           jmp  D2RCore relay page + 40h
//
//   and the relay stub:
//
//     +00  83 7F 04 46                  cmp  dword [rdi+4], 46h
//     +04  74 14                        je   +1A
//     +06  83 7F 04 48                  cmp  dword [rdi+4], 48h
//     +0A  74 1C                        je   +28
//     +0C  FF 25 00 00 00 00 <abs64>    jmp  003DE3A6     ; stock refresh path
//     +1A  FF 25 00 00 00 00 <abs64>    jmp  003DE37E     ; stock quantity block
//     +28  FF 25 00 00 00 00 <abs64>    jmp  003DE3C2     ; shared exit, no refresh
//
//   No register is written and the stack is not touched. The flags are dead at
//   all three destinations: 003DE3A6 is a call, 003DE37E is a cmp, and 003DE3C2
//   is mov al, 1 followed by a jmp. Nothing branches into 003DE379..003DE37D.
//   The window 003DE35C..003DE3C5 is verified before patching, which proves the
//   site, all three destinations and the two local player lookups, and the
//   refresh call's slot is read back and must hold the game's 1C9610.

constexpr std::uint64_t GearRefreshRva = 0x001C9610ULL;

constexpr std::int32_t StatQuantity   = 70;
constexpr std::int32_t StatDurability = 72;

// cmp dword [rdi+4], 46h / jne 003DE3A6
constexpr std::uint8_t CoreDurabilitySiteOriginal[] {
	0x83, 0x7F, 0x04, 0x46,
	0x75, 0x28,
};

// jmp rel32 to the relay stub, rel32 filled at install / one dead NOP.
constexpr std::uint8_t CoreDurabilitySiteTemplate[] {
	0xE9, 0x00, 0x00, 0x00, 0x00,
	0x90,
};

constexpr std::uint8_t CoreDurabilityStub[] {
	0x83, 0x7F, 0x04, 0x46,
	0x74, 0x14,
	0x83, 0x7F, 0x04, 0x48,
	0x74, 0x1C,
	0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

constexpr std::size_t CoreDurabilityRefreshTargetOffset  = 0x12;
constexpr std::size_t CoreDurabilityQuantityTargetOffset = 0x20;
constexpr std::size_t CoreDurabilityExitTargetOffset     = 0x2E;

static_assert(sizeof(CoreDurabilitySiteOriginal) == sizeof(CoreDurabilitySiteTemplate), "A byte patch must not change the length of the region it replaces.");
static_assert(CoreDurabilitySiteOriginal[3] == StatQuantity && CoreDurabilityStub[3] == StatQuantity && CoreDurabilityStub[9] == StatDurability, "Stub stat ids changed.");
static_assert(sizeof(CoreDurabilityStub) == 54, "Durability stub length changed.");
static_assert(CoreDurabilityStub[5] == 0x1A - 0x06 && CoreDurabilityStub[11] == 0x28 - 0x0C, "Stub branches must land on the quantity and exit jumps.");
static_assert(HasAbsoluteJumpBefore(CoreDurabilityStub, CoreDurabilityRefreshTargetOffset), "Refresh target must follow its jmp.");
static_assert(HasAbsoluteJumpBefore(CoreDurabilityStub, CoreDurabilityQuantityTargetOffset), "Quantity target must follow its jmp.");
static_assert(HasAbsoluteJumpBefore(CoreDurabilityStub, CoreDurabilityExitTargetOffset), "Exit target must follow its jmp.");

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
// Fix 4: skill description line printed even when both of its calcs are zero
//        (port of the 2.4 fix at 1B3524)
// ---------------------------------------------------------------------------
//
//   sub_140287A50 builds one line of a skill's description. It is handed the
//   skilldesc.txt record and a line index, evaluates that line's desccalca and
//   desccalcb, then dispatches on the line's descline number:
//
//     00287AC5  mov  r8d, [rax+rdi*4+98h]   desccalca expression for this line
//     00287AE9  call 003B5000               evaluate it  -> r15d
//     00287B07  mov  r8d, [r12+rax*4+E0h]   desccalcb expression for this line
//     00287B0F  call 003B5000               evaluate it  -> r12d
//     00287B20  movzx esi, [rax+rdi*2+50h]  desctexta
//     00287BD6  movzx eax, byte [rdx+rcx+3Eh]  descline for this line
//     00287BDB  dec  eax
//     00287BDD  cmp  eax, 4Eh               descline 1..79
//     00287BE0  ja   00287FD1               out of range: emit nothing
//     00287BEF  jmp  [table 00289F88 + n*4]
//
//   So r15d is desccalca and r12d is desccalcb, exactly as on 2.4. Entry 74 of
//   that table is descline 75, whose handler is 00289AAA:
//
//     00289AAA  mov  ebx, 1506h             the empty-string id, 5382
//     00289AAF  cmp  si, bx                 desctexta is blank?
//     00289AB2  jne  00289AC8
//     00289ABE  call 00287320               assert, then emit nothing
//     00289AC8  movzx ecx, si
//     00289ACB  call desctexta resolve      in D2RLoader 1.3.0 the loader's
//                                           j_GetNamespacedStringById thunk,
//                                           whose rel32 is not compared
//     00289AD0  mov  r9d, r15d              desccalca
//     00289AD3  mov  [rsp+20h], r12d        desccalcb
//     00289AE4  call 008911B0               "<text>: <a>-<b>"
//
//   The only thing it refuses on is a blank string id. It never looks at the
//   two numbers, so a line whose calcs both come out zero still prints, which
//   is where "Physical Damage: 0-0" comes from. Its neighbour descline 74 shows
//   what the intended shape is: it does test r15d, r15d and jumps to 00287FD1
//   when desccalca is zero, emitting nothing.
//
//   The fix gives descline 75 the same check, widened to both numbers: emit
//   only when desccalca or desccalcb is non-zero, otherwise take the same
//   00287FD1 exit that descline 74 already uses. That exit is the switch's own
//   out-of-range path, and it restores r15 from [rsp+6D8h] before leaving, so
//   it is the correct place to land rather than a hand-made return.
//
//   Eight bytes are rewritten in place, the two moves that load the call's
//   arguments:
//
//     00289AD0  E9 <rel32>                  jmp relay page + 100h
//     00289AD5  90 90 90                    never executed
//
//   and the relay stub tests first, then replays them:
//
//     +00  45 85 FF                         test r15d, r15d
//     +03  75 13                            jne emit
//     +05  45 85 E4                         test r12d, r12d
//     +08  75 0E                            jne emit
//     +0A  FF 25 00 00 00 00 <abs64>        jmp 00287FD1     ; both zero, no line
//     +18  45 8B CF                         mov r9d, r15d    ; stock
//     +1B  44 89 64 24 20                   mov [rsp+20h], r12d ; stock
//     +20  FF 25 00 00 00 00 <abs64>        jmp 00289AD8
//
//   Nothing else changes: a line with either number non-zero is byte-identical
//   to stock, and every other descline keeps its own handler. Scope is descline
//   75 only, on whichever skilldesc.txt rows use it.
//
//   The 68 byte handler 00289AAA..00289AED is verified before patching, which
//   also proves the 5382 sentinel, the desctexta resolve and the formatter call
//   surrounding the site. IDA shows exactly one entry into the replaced bytes,
//   the fall-through from the call at 00289ACB, so the three NOPs are dead.

constexpr std::uint64_t DesclineHandler75Rva = 0x00289AAAULL;
constexpr std::uint64_t DesclineSiteRva      = 0x00289AD0ULL;
constexpr std::uint64_t DesclineResumeRva    = 0x00289AD8ULL;
constexpr std::uint64_t DesclineNoEmitRva    = 0x00287FD1ULL;

// The rel32 of the desctexta resolve call at 00289ACB. It targets a loader thunk
// that moves between loader builds, so the body check leaves these 4 bytes out.
constexpr std::uint32_t DesclineResolveRel32Offset = 0x22;
constexpr std::uint32_t DesclineResolveRel32End    = 0x26;

// RVA 0x289AAA, 68 bytes. The descline 75 handler.
constexpr std::uint8_t DesclineHandler75Body[] {
	0xBB, 0x06, 0x15, 0x00, 0x00, 0x66, 0x3B, 0xF3, 0x75, 0x14, 0x48, 0x8D,
	0x4C, 0x24, 0x40, 0xC6, 0x44, 0x24, 0x40, 0x00, 0xE8, 0x5D, 0xD8, 0xFF,
	0xFF, 0xE9, 0x04, 0xE5, 0xFF, 0xFF, 0x0F, 0xB7, 0xCE, 0xE8, 0x8C, 0x1B,
	0xBA, 0x03, 0x45, 0x8B, 0xCF, 0x44, 0x89, 0x64, 0x24, 0x20, 0x4C, 0x8B,
	0xC0, 0x48, 0x8D, 0x4D, 0xF0, 0xBA, 0xC8, 0x00, 0x00, 0x00, 0xE8, 0xC7,
	0x76, 0x60, 0x00, 0xE9, 0xD5, 0x03, 0x00, 0x00,
};

// mov r9d, r15d / mov [rsp+20h], r12d
constexpr std::uint8_t DesclineSiteOriginal[] {
	0x45, 0x8B, 0xCF,
	0x44, 0x89, 0x64, 0x24, 0x20,
};

// jmp rel32 to the relay stub, rel32 filled at install / three dead NOPs.
constexpr std::uint8_t DesclineSiteTemplate[] {
	0xE9, 0x00, 0x00, 0x00, 0x00,
	0x90, 0x90, 0x90,
};

constexpr std::uint32_t DesclineSiteRel32Offset = 1;

constexpr std::uint8_t DesclineStub[] {
	0x45, 0x85, 0xFF,
	0x75, 0x13,
	0x45, 0x85, 0xE4,
	0x75, 0x0E,
	0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x45, 0x8B, 0xCF,
	0x44, 0x89, 0x64, 0x24, 0x20,
	0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

constexpr std::size_t DesclineNoEmitTargetOffset = 0x10;
constexpr std::size_t DesclineEmitOffset         = 0x18;
constexpr std::size_t DesclineResumeTargetOffset = 0x26;

static_assert(sizeof(DesclineHandler75Body) == 68, "Verified descline 75 handler length changed.");
static_assert(sizeof(DesclineSiteOriginal) == sizeof(DesclineSiteTemplate), "A byte patch must not change the length of the region it replaces.");
static_assert(IsSubrangeOf(DesclineSiteOriginal, DesclineHandler75Body, DesclineSiteRva - DesclineHandler75Rva), "The replaced bytes must sit inside the verified handler at the stated offset.");
static_assert(DesclineResumeRva == DesclineSiteRva + sizeof(DesclineSiteOriginal), "The stub must rejoin at the instruction after the site.");
static_assert(DesclineHandler75Body[0] == 0xBB && DesclineHandler75Body[1] == 0x06 && DesclineHandler75Body[2] == 0x15, "The handler must open on the 5382 empty-string sentinel.");
static_assert(DesclineHandler75Body[0x21] == 0xE8, "The desctexta resolve call is not where the handler map says it is.");
static_assert(DesclineHandler75Body[0x3A] == 0xE8, "The line formatter call is not where the handler map says it is.");
static_assert(IsSubrangeOf(DesclineSiteOriginal, DesclineStub, DesclineEmitOffset), "The stub's emit block must replay the bytes it displaced.");
static_assert(DesclineStub[0] == 0x45 && DesclineStub[1] == 0x85 && DesclineStub[2] == 0xFF, "The stub must open on test r15d, r15d.");
static_assert(DesclineStub[5] == 0x45 && DesclineStub[6] == 0x85 && DesclineStub[7] == 0xE4, "The second test must be test r12d, r12d.");
static_assert(DesclineStub[4] == DesclineEmitOffset - 0x05 && DesclineStub[9] == DesclineEmitOffset - 0x0A, "Both stub branches must land on the emit block.");
static_assert(HasAbsoluteJumpBefore(DesclineStub, DesclineNoEmitTargetOffset), "No-emit target must follow its jmp.");
static_assert(HasAbsoluteJumpBefore(DesclineStub, DesclineResumeTargetOffset), "Resume target must follow its jmp.");
static_assert(sizeof(DesclineStub) == 46, "Descline stub length changed.");

// ---------------------------------------------------------------------------
// Fix 5: socketed gem and rune stats accumulate across resocketing
//        (port of the 2.4 fix at 3079DE, re-derived against this build)
// ---------------------------------------------------------------------------
//
//   READ OUT OF 3.3, NOT INHERITED FROM 2.4. Two earlier attempts at this port
//   failed because both the storage shape and the itemtype row came from the
//   2.4 write-up. The shape was right in substance and wrong in its offsets,
//   and the row was wrong outright. Everything below is witnessed here.
//
//   WHERE A SOCKETABLE'S PROPERTIES LIVE. 00475140 applies a socket filler's
//   properties. It is handed the filler and the host, and it applies to the
//   FILLER, never to the host:
//
//     004751B9  BA 14 00 00 00           mov  edx, 14h     itemtypes row 20
//     004751C3  48 8B CF                 mov  rcx, rdi     the filler
//     004751C6  E8 C5 E6 EF FF           call 00373890     ITEMS_CheckItemTypeId
//               on a match, applies the filler's gems-table properties in mode 2
//     00475283  BA 4B 00 00 00           mov  edx, 4Bh     itemtypes row 75
//     00475288  48 8B CF                 mov  rcx, rdi     the filler
//     0047528B  E8 00 E6 EF FF           call 00373890
//               on a match, applies the filler's gems-table properties in mode 5
//
//   and it finishes by merging the filler's stat list into the host. Both rows
//   reach the same gems-table record lookup, so both kinds of filler have their
//   socketed properties re-derived from the table on every socket. A filler
//   matching neither row, which is what a jewel is, has nothing applied to it
//   at all: what it carries is its own rolled affixes, and they are the only
//   copy in existence.
//
//   The properties are posted as a CHILD of the filler's stat list. 002F2CB0
//   links a child by writing next at +68h, previous at +70h and parent at +78h
//   on the child, then updating the owner's chain head at +90h. Children posted
//   for item properties carry flags 40h and state 0, and the engine's own
//   lookup for them is 002F5B80(unit, state, flags): it resolves the unit's
//   stat list at unit+88h, walks the +90h chain and matches on flags while the
//   state is 0. THE 2.4 NOTE'S +68h HEAD IS WRONG ON THIS BUILD. Here +68h is
//   the child's own next link, so reading it as the head walks into the sibling
//   list instead of into the children.
//
//   THE BUG. Pulling a filler back out lands in 0038BA30, which removes it from
//   its container and detaches its stats:
//
//     0038BB9C  mov  rcx, [rdi+8]        the container's owner, the host item
//     0038BBA0  mov  rdx, rsi            the filler being removed
//     0038BBA3  call 002F8290            expire the filler's stat list
//
//   002F8290 resolves the filler's list and expires that one node in 002F6E50,
//   which unlinks it and subtracts its totals from the host. What it does NOT
//   do is touch the children at +90h. The filler keeps them, still holding the
//   socketed contribution inside its own totals, so the next socket derives the
//   properties again and posts a second copy on top of the first. Every
//   resocket adds another, and the host is handed the inflated figure.
//
//   THE FIX expires those children, and only for a filler the engine will
//   rebuild them for. The gate is neither a guess nor a setting: it is the two
//   itemtype rows taken straight out of 00475140's own verified bytes, so this
//   fires on exactly the fillers the applier fires on and on nothing else. If a
//   future build changes either row, the window stops matching and the fix
//   declines to install rather than stripping the wrong kind of item. A jewel
//   is not covered by either row, so its affixes are never touched.
//
//   ORDER MATTERS AND IT IS NOT THE OBVIOUS ONE. The host was given the
//   filler's totals with the children already folded in, and the stock detach
//   subtracts that same array, so the children must still be in it when the
//   detach runs. Expiring them first shrinks the array the detach is about to
//   subtract and strands the difference on the host forever. The stub therefore
//   replays the stock call FIRST. By then the filler is unlinked from the host,
//   so the subtractions land on the filler alone, which is exactly where they
//   belong: its totals are clean for the next time it is socketed.
//
//   The walk re-asks 002F5B80 each pass instead of following links itself,
//   which makes it the exact inverse of the get-or-create the applier uses:
//   whatever that call would find and add a second copy to, this one finds and
//   expires. A counter bounds it at eight so a chain that somehow fails to
//   unlink cannot spin. The next socket then finds nothing, creates a fresh
//   child and applies exactly one copy.
//
//   SITE CHOICE. 0038BA30 is the shared bottom of every removal path, so one
//   patch covers the cube rem recipe, the cube unsocket recipe and anything
//   else that takes a filler out of an item. The cube output builder at
//   005269C0 reaches it through 00389820, and that chain is what the 2.4 site
//   inside the cube function itself corresponds to here. Guarding the shared
//   callee rather than each caller is the same choice the dead unit guards
//   above make. An ordinary item moved between pages also reaches this site and
//   fails the gate, so it takes a path identical to stock.
//
//   The five byte call is replaced in place:
//
//     0038BBA3  E9 <rel32>              jmp relay page + 140h
//
//   and the stub keeps rcx and rdx exactly as the two stock instructions before
//   the site left them, so the replayed call needs no argument setup. rsi holds
//   the filler across the whole region and is non-volatile, so it survives
//   every call the stub makes:
//
//     +00  sub  rsp, 40h                shadow space plus two scratch slots
//     +04  call 002F8290                stock detach, replayed first
//     +10  mov  rcx, rsi / mov edx, <row 20> / call 00373890
//     +24  test eax, eax / jnz gated
//     +28  mov  rcx, rsi / mov edx, <row 75> / call 00373890
//     +3C  test eax, eax / jz  done       neither row, nothing to expire
//     +40  gated: mov rcx, rsi / call 0034A0E0    this unit's data context
//     +4F  movzx eax, al / mov [rsp+38h], eax
//     +56  mov  dword [rsp+30h], 8       loop bound
//     +5E  loop: mov rcx, rsi / xor edx, edx / mov r8d, 40h
//     +69  call 002F5B80                 state 0, flags 40h child, if any
//     +75  test rax, rax / jz done       none left
//     +7A  mov  rdx, rax / mov ecx, [rsp+38h]
//     +81  call 002F6E50                 expire it, no free
//     +8D  dec  dword [rsp+30h] / jnz loop
//     +93  done: add rsp, 40h
//     +97  jmp  0038BBA8
//
//   Every skip lands on the same done label, so a non-socketable, a filler with
//   no stat list and a gem that was never socketed all take a path whose only
//   difference from stock is the stack adjustment around the replayed call.
//
//   The 64 byte window 0038BB70..0038BBAF is verified before patching, which
//   also proves the two argument loads and that the call at the site really is
//   002F8290. IDA shows one entry into the replaced bytes, the fall-through
//   from 0038BBA0. The two applier windows are verified for the same reason,
//   and the gate rows are then read back out of them rather than written down
//   here twice.

constexpr std::uint64_t DetachWindowRva      = 0x0038BB70ULL;
constexpr std::uint64_t DetachSiteRva        = 0x0038BBA3ULL;
constexpr std::uint64_t DetachResumeRva      = 0x0038BBA8ULL;
constexpr std::uint64_t ExpireItemStatsRva   = 0x002F8290ULL;  // call at 0038BBA3
constexpr std::uint64_t CheckItemTypeIdRva   = 0x00373890ULL;
constexpr std::uint64_t FindChildStatListRva = 0x002F5B80ULL;
constexpr std::uint64_t GetDataContextRva    = 0x0034A0E0ULL;
constexpr std::uint64_t ExpireStatNodeRva    = 0x002F6E50ULL;

// The two itemtype gates inside the socket property applier at 00475140. Each
// window is a whole number of instructions ending on the call to
// ITEMS_CheckItemTypeId, so verifying it proves both the row number and the
// callee. The rows themselves are read back out of these arrays below, which is
// what keeps this fix and the applier from ever disagreeing.
constexpr std::uint64_t ApplierGateARva = 0x004751B9ULL;
constexpr std::uint64_t ApplierGateBRva = 0x00475283ULL;

// mov edx, 14h / mov [rsp+58h], r15 / mov rcx, rdi / call 00373890
constexpr std::uint8_t ApplierGateA[] {
	0xBA, 0x14, 0x00, 0x00, 0x00, 0x4C, 0x89, 0x7C, 0x24, 0x58, 0x48, 0x8B,
	0xCF, 0xE8, 0xC5, 0xE6, 0xEF, 0xFF,
};

// mov edx, 4Bh / mov rcx, rdi / call 00373890
constexpr std::uint8_t ApplierGateB[] {
	0xBA, 0x4B, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xCF, 0xE8, 0x00, 0xE6, 0xEF,
	0xFF,
};

constexpr std::size_t ApplierGateARowOffset  = 1;
constexpr std::size_t ApplierGateACallOffset = 13;
constexpr std::size_t ApplierGateBRowOffset  = 1;
constexpr std::size_t ApplierGateBCallOffset = 8;

// The gate the stub applies, lifted out of the applier's own bytes rather than
// written down as a number of its own. Both are one byte wide in the immediate,
// which the asserts below pin.
constexpr std::uint32_t SocketableRowA = ApplierGateA[ApplierGateARowOffset];
constexpr std::uint32_t SocketableRowB = ApplierGateB[ApplierGateBRowOffset];

// RVA 0x38BB70, 64 bytes. The detach tail of the container-removal routine.
constexpr std::uint8_t DetachWindow[] {
	0xE8, 0x5B, 0x9C, 0xFE, 0xFF, 0x48, 0x8B, 0x4F, 0x08, 0x48, 0x85, 0xC9,
	0x74, 0x2A, 0xE8, 0x4D, 0xFE, 0xFB, 0xFF, 0x83, 0xF8, 0x06, 0x7C, 0x14,
	0x48, 0x8D, 0x4C, 0x24, 0x40, 0xC6, 0x44, 0x24, 0x40, 0x00, 0xE8, 0x19,
	0x8F, 0xFF, 0xFF, 0x84, 0xC0, 0x74, 0x01, 0xCC, 0x48, 0x8B, 0x4F, 0x08,
	0x48, 0x8B, 0xD6, 0xE8, 0xE8, 0xC6, 0xF6, 0xFF, 0x41, 0xB8, 0x0E, 0x02,
	0x00, 0x00, 0x48, 0x8D,
};

// call 002F8290
constexpr std::uint8_t DetachSiteOriginal[] {
	0xE8, 0xE8, 0xC6, 0xF6, 0xFF,
};

// jmp rel32 to the relay stub, rel32 filled at install. No padding needed: the
// call and the jump are both five bytes.
constexpr std::uint8_t DetachSiteTemplate[] {
	0xE9, 0x00, 0x00, 0x00, 0x00,
};

constexpr std::uint32_t DetachSiteRel32Offset = 1;

constexpr std::uint8_t ResocketStub[] {
	0x48, 0x83, 0xEC, 0x40, 0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0xFF, 0xD0, 0x48, 0x8B, 0xCE, 0xBA, 0x00, 0x00, 0x00, 0x00,
	0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xD0,
	0x85, 0xC0, 0x75, 0x18, 0x48, 0x8B, 0xCE, 0xBA, 0x00, 0x00, 0x00, 0x00,
	0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xD0,
	0x85, 0xC0, 0x74, 0x53, 0x48, 0x8B, 0xCE, 0x48, 0xB8, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xD0, 0x0F, 0xB6, 0xC0, 0x89, 0x44,
	0x24, 0x38, 0xC7, 0x44, 0x24, 0x30, 0x08, 0x00, 0x00, 0x00, 0x48, 0x8B,
	0xCE, 0x33, 0xD2, 0x41, 0xB8, 0x40, 0x00, 0x00, 0x00, 0x48, 0xB8, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xD0, 0x48, 0x85, 0xC0,
	0x74, 0x19, 0x48, 0x8B, 0xD0, 0x8B, 0x4C, 0x24, 0x38, 0x48, 0xB8, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xD0, 0xFF, 0x4C, 0x24,
	0x30, 0x75, 0xCB, 0x48, 0x83, 0xC4, 0x40, 0xFF, 0x25, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

constexpr std::size_t ResocketDetachOffset    = 0x06;
constexpr std::size_t ResocketRowAOffset      = 0x14;
constexpr std::size_t ResocketCheckAOffset    = 0x1A;
constexpr std::size_t ResocketRowBOffset      = 0x2C;
constexpr std::size_t ResocketCheckBOffset    = 0x32;
constexpr std::size_t ResocketContextOffset   = 0x45;
constexpr std::size_t ResocketFindChildOffset = 0x6B;
constexpr std::size_t ResocketExpireOffset    = 0x83;
constexpr std::size_t ResocketResumeOffset    = 0x9D;
constexpr std::size_t ResocketGatedOffset     = 0x40;
constexpr std::size_t ResocketLoopOffset      = 0x5E;
constexpr std::size_t ResocketDoneOffset      = 0x93;

static_assert(sizeof(DetachWindow) == 64, "Verified detach window length changed.");
static_assert(sizeof(DetachSiteOriginal) == sizeof(DetachSiteTemplate), "A byte patch must not change the length of the region it replaces.");
static_assert(IsSubrangeOf(DetachSiteOriginal, DetachWindow, DetachSiteRva - DetachWindowRva), "The replaced bytes must sit inside the verified window at the stated offset.");
static_assert(DetachResumeRva == DetachSiteRva + sizeof(DetachSiteOriginal), "The stub must rejoin at the instruction after the site.");
static_assert(DetachWindow[DetachSiteRva - DetachWindowRva] == 0xE8 && CallTargetRva(DetachWindow, DetachWindowRva, DetachSiteRva - DetachWindowRva) == ExpireItemStatsRva, "The site does not call the stat list expire.");
static_assert(DetachWindow[0x2C] == 0x48 && DetachWindow[0x2D] == 0x8B && DetachWindow[0x2E] == 0x4F && DetachWindow[0x2F] == 0x08, "The owner argument load is not mov rcx, [rdi+8].");
static_assert(DetachWindow[0x30] == 0x48 && DetachWindow[0x31] == 0x8B && DetachWindow[0x32] == 0xD6, "The item argument load is not mov rdx, rsi.");

// The gate rows are only trustworthy if each window really is a mov edx of a
// one-byte row followed by a call to the itemtype check, and if both windows
// call the same function the stub calls.
static_assert(ApplierGateA[0] == 0xBA && ApplierGateA[2] == 0x00 && ApplierGateA[3] == 0x00 && ApplierGateA[4] == 0x00, "Applier gate A does not begin with mov edx, <one byte row>.");
static_assert(ApplierGateB[0] == 0xBA && ApplierGateB[2] == 0x00 && ApplierGateB[3] == 0x00 && ApplierGateB[4] == 0x00, "Applier gate B does not begin with mov edx, <one byte row>.");
static_assert(ApplierGateA[ApplierGateACallOffset - 3] == 0x48 && ApplierGateA[ApplierGateACallOffset - 2] == 0x8B && ApplierGateA[ApplierGateACallOffset - 1] == 0xCF, "Applier gate A does not pass the filler in rcx.");
static_assert(ApplierGateB[ApplierGateBCallOffset - 3] == 0x48 && ApplierGateB[ApplierGateBCallOffset - 2] == 0x8B && ApplierGateB[ApplierGateBCallOffset - 1] == 0xCF, "Applier gate B does not pass the filler in rcx.");
static_assert(ApplierGateA[ApplierGateACallOffset] == 0xE8 && CallTargetRva(ApplierGateA, ApplierGateARva, ApplierGateACallOffset) == CheckItemTypeIdRva, "Applier gate A does not call the itemtype check.");
static_assert(ApplierGateB[ApplierGateBCallOffset] == 0xE8 && CallTargetRva(ApplierGateB, ApplierGateBRva, ApplierGateBCallOffset) == CheckItemTypeIdRva, "Applier gate B does not call the itemtype check.");
static_assert(SocketableRowA != SocketableRowB, "The two applier gates must test different itemtype rows.");

static_assert(ResocketStub[0] == 0x48 && ResocketStub[1] == 0x83 && ResocketStub[2] == 0xEC && ResocketStub[3] == 0x40, "The stub must open by reserving shadow space and two scratch slots.");
static_assert(ResocketStub[ResocketDoneOffset] == 0x48 && ResocketStub[ResocketDoneOffset + 1] == 0x83 && ResocketStub[ResocketDoneOffset + 2] == 0xC4 && ResocketStub[ResocketDoneOffset + 3] == 0x40, "The done label must release the stub's stack.");
static_assert(ResocketStub[ResocketRowAOffset - 1] == 0xBA, "Row A must be the immediate of mov edx.");
static_assert(ResocketStub[ResocketRowBOffset - 1] == 0xBA, "Row B must be the immediate of mov edx.");
static_assert(ResocketStub[0x26] == 0x75 && ResocketStub[0x27] == ResocketGatedOffset - 0x28, "The row A match must land on the gated block.");
static_assert(ResocketStub[0x3E] == 0x74 && ResocketStub[0x3F] == ResocketDoneOffset - 0x40, "The neither-row skip must land on done.");
static_assert(ResocketStub[0x78] == 0x74 && ResocketStub[0x79] == ResocketDoneOffset - 0x7A, "The empty child chain skip must land on done.");
static_assert(ResocketStub[0x91] == 0x75 && static_cast<std::int8_t>(ResocketStub[0x92]) == static_cast<std::int8_t>(ResocketLoopOffset - 0x93), "The loop back edge must land on the child lookup.");
static_assert(ResocketStub[0x61] == 0x33 && ResocketStub[0x62] == 0xD2, "The child lookup must ask for state 0.");
static_assert(ResocketStub[0x63] == 0x41 && ResocketStub[0x64] == 0xB8 && ResocketStub[0x65] == 0x40, "The child lookup must ask for flags 40h.");
static_assert(HasMovRaxImm64CallBefore(ResocketStub, ResocketDetachOffset), "Replayed detach target must be called by the instruction it follows.");
static_assert(HasMovRaxImm64CallBefore(ResocketStub, ResocketCheckAOffset), "Row A itemtype check target must be called by the instruction it follows.");
static_assert(HasMovRaxImm64CallBefore(ResocketStub, ResocketCheckBOffset), "Row B itemtype check target must be called by the instruction it follows.");
static_assert(HasMovRaxImm64CallBefore(ResocketStub, ResocketContextOffset), "Data context target must be called by the instruction it follows.");
static_assert(HasMovRaxImm64CallBefore(ResocketStub, ResocketFindChildOffset), "Child lookup target must be called by the instruction it follows.");
static_assert(HasMovRaxImm64CallBefore(ResocketStub, ResocketExpireOffset), "Node expire target must be called by the instruction it follows.");
static_assert(HasAbsoluteJumpBefore(ResocketStub, ResocketResumeOffset), "Resume target must follow its jmp.");
static_assert(ResocketDetachOffset < ResocketCheckAOffset, "The stock detach must be replayed before anything else the stub does.");
static_assert(sizeof(ResocketStub) == 165, "Resocket stub length changed.");

// ---------------------------------------------------------------------------
// Fix 6: blood mana skills refused by the server
//        (port of the 2.4 fix at 294F40, re-derived against this build)
// ---------------------------------------------------------------------------
//
//   States.txt blood_mana, state 114, makes a skill cost life instead of mana.
//   With the state up and a skill whose mana cost is above current mana but
//   affordable in life, the cast plays on the client -- animation, missiles,
//   overlays -- and the server does nothing at all. No cooldown, no damage, no
//   missile collisions.
//
//   00436750 is the server-only pre-cast affordability gate. Exactly two call
//   sites, and both drop the cast when it refuses: the srvstfunc dispatcher
//   0043B3E0 at 0043B44B, and the srvdofunc dispatcher 0043ACB0 at 0043AEBE.
//   Its whole body, 221 bytes:
//
//     00436762  call 0034B9D0             unit type; non-player -> return 1
//     0043676E  call 0034BA40             the pending skill node
//     00436782  call 0033CC10             charge item guid
//     00436787  cmp  eax, -1              not -1 -> charges: [rbx+50h] > 0
//     004367AE  call 0033CBC0             the Skills.txt record
//     004367C8  call 0033D1E0             skill level
//     004367CD  movzx ecx, [rsi+228h]     manashift
//     004367DC  movsx eax, [rsi+22Ch]     lvlmana
//     004367E6  movsx eax, [rsi+22Ah]     mana
//     004367EF  shl  ebx, cl              cost = (mana + lvlmana*(lvl-1)) << shift
//     004367F1  cmp  word [rsi+4Eh], 74h  the srvdofunc 116 exemption
//     0043680A  lea  edx, [r8+8]          <- stat 8, mana
//     0043680E  call 002F5020             GetUnitStat(unit, stat, layer)
//     00436813  cmp  eax, ebx
//     00436815  jge  00436796             mana >= cost -> return 1
//
//   There is no blood_mana branch anywhere in it. It reads mana and nothing
//   else, so with mana under the cost it refuses however much life the player
//   has. The client never calls this function, which is exactly why the two
//   sides disagree and why the visuals play with no server behind them.
//
//   THE PAYMENT SIDE ALREADY KNOWS BETTER. D2GAME_SKILLMANA_Consume 00436830
//   asks STATES_CheckState(unit, 114) and, when the state is there, pays
//   through 00584E80. That function refuses on GetUnitStat(unit, 6, 0) < cost
//   and otherwise subtracts exactly that much life; it never looks at mana. So
//   under blood_mana the resource actually spent is life, and the gate in
//   front of it was admitting and refusing casts on the strength of a number
//   that would never be touched.
//
//   The fix asks the same question in the gate, immediately before the
//   resource read: when STATES_CheckState(unit, 114) answers yes, read stat 6
//   (hitpoints) instead of stat 8 (mana). The gate then admits exactly the
//   casts the payment will accept. This is also what the engine's own shared
//   affordability check has always done; this gate is the odd one out, and was
//   on 2.4 as well.
//
//   HOW IT IS WRITTEN. The whole function is replaced in place. 218 bytes of
//   code plus CC padding to the original 221, so the next function at 00436830
//   is untouched and no code cave or relay page is needed anywhere:
//
//     - The prologue is kept byte for byte -- mov [rsp+8],rbx / mov
//       [rsp+10h],rsi / push rdi / sub rsp,20h -- along with the frame size
//       and the [rsp+30h] and [rsp+38h] save slots, so the function's existing
//       .pdata unwind record stays correct. Nothing else in the body touches
//       rsp, so the frame the unwinder is told about is the frame that exists
//       at every instruction.
//     - Every call is the same callee with the same arguments in the same
//       order. The room for the new branch came from merging the two copies of
//       the epilogue, folding the three Skills.txt record loads behind one
//       lea rdx,[rsi+228h], and dec eax for sub eax,1 (same SF for the cmovns
//       that reads it).
//     - The new block is: mov rcx,rdi / mov edx,114 / call 003351B0 / xor
//       edx,edx / mov dl,8 / test eax,eax / jz +2 / mov dl,6, and then the
//       stock xor r8d,r8d / mov rcx,rdi / call 002F5020 / cmp eax,ebx.
//       STATES_CheckState is not documented to return 1, so the result is
//       branched on rather than used as a number.
//
//   VERIFICATION. Every rel32 in the replacement was re-resolved from the
//   emitted bytes: all eight calls land on the callees named above and all
//   eleven jumps land on an instruction boundary inside the body. Emulated
//   against the stock bytes with the eight callees stubbed: 7104 scenarios
//   across unit type, node presence, charges, record presence, level, the
//   three Skills.txt mana columns, srvdofunc, the 116 flag, the state answer
//   and both stat values. Every divergence from stock is a blood_mana case and
//   in each one the new answer is exactly life >= cost with stat 6 read. A
//   separate 3840 scenario run with the state absent is identical to stock in
//   return value, in which stat id and layer it reads, in its callee sequence
//   and in rbx, rsi and rsp on exit.
//
//   SCOPE. State 114 only, server side, and the gate has already returned 1
//   for non-players before it gets here. Charges still bypass everything: a
//   node with a charge item returns on [rbx+50h] > 0 before a cost is
//   computed. While the state is up mana is ignored in BOTH directions -- a
//   cast affordable in life goes through with no mana, and a cast that used to
//   start on mana alone while life was under the cost is now refused up front
//   rather than reaching 00584E80, which would have stripped the state and
//   left the player on 1 life.

constexpr std::uint64_t BloodManaGateRva    = 0x00436750ULL;
constexpr std::uint64_t StatesCheckStateRva = 0x003351B0ULL;  // call at 436803
constexpr std::uint64_t GetUnitStatRva      = 0x002F5020ULL;  // call at 43680E / 436818

// Offsets into the 221 byte body, for the witnesses below.
constexpr std::size_t BloodManaStockStatOffset  = 0xBA;  // lea edx, [r8+8]
constexpr std::size_t BloodManaStockCallOffset  = 0xBE;  // call GetUnitStat
constexpr std::size_t BloodManaStateArgOffset   = 0xAE;  // mov edx, 114
constexpr std::size_t BloodManaStateCallOffset  = 0xB3;  // call STATES_CheckState
constexpr std::size_t BloodManaManaIdOffset     = 0xBA;  // mov dl, 8
constexpr std::size_t BloodManaLifeIdOffset     = 0xC0;  // mov dl, 6
constexpr std::size_t BloodManaStatCallOffset   = 0xC8;  // call GetUnitStat
constexpr std::size_t BloodManaCodeSize         = 218;

constexpr std::int32_t StatHitpointsId = 6;
constexpr std::int32_t StatManaId      = 8;
constexpr std::int32_t BloodManaStateId = 114;

// mov [rsp+8], rbx / mov [rsp+10h], rsi / push rdi / sub rsp, 20h. The frame
// the function's .pdata unwind record describes; the replacement keeps it.
constexpr std::uint8_t BloodManaGatePrologueWitness[] {
	0x48, 0x89, 0x5C, 0x24, 0x08,
	0x48, 0x89, 0x74, 0x24, 0x10,
	0x57,
	0x48, 0x83, 0xEC, 0x20,
};

// RVA 0x436750, 221 bytes. The stock server pre-cast affordability gate.
constexpr std::uint8_t BloodManaGateBody[] {
	0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48,
	0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF9, 0xE8, 0x69, 0x52, 0xF1, 0xFF, 0x85,
	0xC0, 0x75, 0x2B, 0x48, 0x8B, 0xCF, 0xE8, 0xCD, 0x52, 0xF1, 0xFF, 0x48,
	0x8B, 0xD8, 0x48, 0x85, 0xC0, 0x0F, 0x84, 0x9C, 0x00, 0x00, 0x00, 0x48,
	0x8B, 0xC8, 0xE8, 0x89, 0x64, 0xF0, 0xFF, 0x83, 0xF8, 0xFF, 0x74, 0x1F,
	0x83, 0x7B, 0x50, 0x00, 0x0F, 0x8E, 0x85, 0x00, 0x00, 0x00, 0xB8, 0x01,
	0x00, 0x00, 0x00, 0x48, 0x8B, 0x5C, 0x24, 0x30, 0x48, 0x8B, 0x74, 0x24,
	0x38, 0x48, 0x83, 0xC4, 0x20, 0x5F, 0xC3, 0x48, 0x8B, 0xCB, 0xE8, 0x0D,
	0x64, 0xF0, 0xFF, 0x48, 0x8B, 0xF0, 0x48, 0x85, 0xC0, 0x74, 0x60, 0x45,
	0x33, 0xC9, 0x48, 0x8B, 0xD3, 0x48, 0x8B, 0xCF, 0x45, 0x8D, 0x41, 0x01,
	0xE8, 0x13, 0x6A, 0xF0, 0xFF, 0x0F, 0xB6, 0x8E, 0x28, 0x02, 0x00, 0x00,
	0x33, 0xDB, 0x83, 0xE8, 0x01, 0x0F, 0x49, 0xD8, 0x0F, 0xBF, 0x86, 0x2C,
	0x02, 0x00, 0x00, 0x0F, 0xAF, 0xD8, 0x0F, 0xBF, 0x86, 0x2A, 0x02, 0x00,
	0x00, 0x03, 0xD8, 0xD3, 0xE3, 0x66, 0x83, 0x7E, 0x4E, 0x74, 0x75, 0x0C,
	0x48, 0x8B, 0xCF, 0xE8, 0x90, 0xF8, 0xEF, 0xFF, 0x85, 0xC0, 0x75, 0x92,
	0x45, 0x33, 0xC0, 0x48, 0x8B, 0xCF, 0x41, 0x8D, 0x50, 0x08, 0xE8, 0x0D,
	0xE8, 0xEB, 0xFF, 0x3B, 0xC3, 0x0F, 0x8D, 0x7B, 0xFF, 0xFF, 0xFF, 0x48,
	0x8B, 0x5C, 0x24, 0x30, 0x33, 0xC0, 0x48, 0x8B, 0x74, 0x24, 0x38, 0x48,
	0x83, 0xC4, 0x20, 0x5F, 0xC3,
};

// The replacement. Same length, same prologue, same frame, same callees.
constexpr std::uint8_t BloodManaGatePatched[] {
	0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48,
	0x83, 0xEC, 0x20, 0x48, 0x89, 0xCF, 0xE8, 0x69, 0x52, 0xF1, 0xFF, 0x85,
	0xC0, 0x75, 0x23, 0x48, 0x89, 0xF9, 0xE8, 0xCD, 0x52, 0xF1, 0xFF, 0x48,
	0x89, 0xC3, 0x48, 0x85, 0xC0, 0x74, 0x19, 0x48, 0x89, 0xC1, 0xE8, 0x8D,
	0x64, 0xF0, 0xFF, 0x83, 0xF8, 0xFF, 0x74, 0x1E, 0x83, 0x7B, 0x50, 0x00,
	0x7E, 0x06, 0x31, 0xC0, 0xFF, 0xC0, 0xEB, 0x02, 0x31, 0xC0, 0x48, 0x8B,
	0x5C, 0x24, 0x30, 0x48, 0x8B, 0x74, 0x24, 0x38, 0x48, 0x83, 0xC4, 0x20,
	0x5F, 0xC3, 0x48, 0x89, 0xD9, 0xE8, 0x12, 0x64, 0xF0, 0xFF, 0x48, 0x89,
	0xC6, 0x48, 0x85, 0xC0, 0x74, 0xDE, 0x45, 0x31, 0xC9, 0x48, 0x89, 0xDA,
	0x48, 0x89, 0xF9, 0x45, 0x8D, 0x41, 0x01, 0xE8, 0x18, 0x6A, 0xF0, 0xFF,
	0x48, 0x8D, 0x96, 0x28, 0x02, 0x00, 0x00, 0x0F, 0xB6, 0x0A, 0x31, 0xDB,
	0xFF, 0xC8, 0x0F, 0x49, 0xD8, 0x0F, 0xBF, 0x42, 0x04, 0x0F, 0xAF, 0xD8,
	0x0F, 0xBF, 0x42, 0x02, 0x01, 0xC3, 0xD3, 0xE3, 0x66, 0x83, 0x7E, 0x4E,
	0x74, 0x75, 0x0C, 0x48, 0x89, 0xF9, 0xE8, 0x99, 0xF8, 0xEF, 0xFF, 0x85,
	0xC0, 0x75, 0x93, 0x48, 0x89, 0xF9, 0xBA, 0x72, 0x00, 0x00, 0x00, 0xE8,
	0xA8, 0xE9, 0xEF, 0xFF, 0x31, 0xD2, 0xB2, 0x08, 0x85, 0xC0, 0x74, 0x02,
	0xB2, 0x06, 0x45, 0x31, 0xC0, 0x48, 0x89, 0xF9, 0xE8, 0x03, 0xE8, 0xEB,
	0xFF, 0x39, 0xD8, 0x0F, 0x8D, 0x69, 0xFF, 0xFF, 0xFF, 0xE9, 0x6A, 0xFF,
	0xFF, 0xFF, 0xCC, 0xCC, 0xCC,
};

static_assert(sizeof(BloodManaGateBody) == 221, "Verified blood mana gate length changed.");
static_assert(sizeof(BloodManaGateBody) == sizeof(BloodManaGatePatched), "A byte patch must not change the length of the region it replaces.");
static_assert(BloodManaGateBody[sizeof(BloodManaGateBody) - 1] == 0xC3, "The verified body must end on the function's ret.");
static_assert(IsLeadingBytesOf(BloodManaGatePrologueWitness, BloodManaGateBody) && IsLeadingBytesOf(BloodManaGatePrologueWitness, BloodManaGatePatched), "The replacement must keep the stock prologue so the .pdata unwind record stays correct.");
static_assert(BloodManaGateBody[BloodManaStockStatOffset] == 0x41 && BloodManaGateBody[BloodManaStockStatOffset + 1] == 0x8D && BloodManaGateBody[BloodManaStockStatOffset + 2] == 0x50 && BloodManaGateBody[BloodManaStockStatOffset + 3] == StatManaId, "The stock gate must ask for the mana stat with lea edx, [r8+8].");
static_assert(BloodManaGateBody[BloodManaStockCallOffset] == 0xE8 && CallTargetRva(BloodManaGateBody, BloodManaGateRva, BloodManaStockCallOffset) == GetUnitStatRva, "GetUnitStat is not the callee witnessed at 43680E.");
static_assert(BloodManaGatePatched[BloodManaStateArgOffset] == 0xBA && BloodManaGatePatched[BloodManaStateArgOffset + 1] == BloodManaStateId, "The state argument must be the immediate of mov edx.");
static_assert(BloodManaGatePatched[BloodManaStateCallOffset] == 0xE8 && CallTargetRva(BloodManaGatePatched, BloodManaGateRva, BloodManaStateCallOffset) == StatesCheckStateRva, "The state question must call STATES_CheckState.");
static_assert(BloodManaGatePatched[BloodManaManaIdOffset] == 0xB2 && BloodManaGatePatched[BloodManaManaIdOffset + 1] == StatManaId, "The default stat id must be mana.");
static_assert(BloodManaGatePatched[BloodManaLifeIdOffset] == 0xB2 && BloodManaGatePatched[BloodManaLifeIdOffset + 1] == StatHitpointsId, "The blood mana stat id must be hitpoints.");
static_assert(BloodManaGatePatched[BloodManaStatCallOffset] == 0xE8 && CallTargetRva(BloodManaGatePatched, BloodManaGateRva, BloodManaStatCallOffset) == GetUnitStatRva, "The replacement must read the stat through the same getter as stock.");
static_assert(BloodManaGatePatched[BloodManaCodeSize - 5] == 0xE9, "The replacement's last instruction must be the jmp to its zero exit.");
static_assert(BloodManaGatePatched[BloodManaCodeSize] == 0xCC && BloodManaGatePatched[BloodManaCodeSize + 1] == 0xCC && BloodManaGatePatched[BloodManaCodeSize + 2] == 0xCC, "The tail of the replaced body must be int3 padding, not code.");

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
# Engine Stability 0.10.1
# Crash guards and vanilla engine fixes for Diablo II: Resurrected.
#
# Built against D2RLoader 1.3.0: Diablo II: Resurrected 3.3 inside
# D2RLoader.exe, plus D2RCore.dll for three of the entries below. Those three
# are verified the same way but do not appear in D2RLoader's own diagnostics.
# =============================================================================
#
# TWO KINDS OF ENTRY, AND THE DIFFERENCE MATTERS
#
#   [guards] stop crashes. A guard is the difference between a hard crash and a
#   normal frame. Most are invisible when nothing is going wrong. Where
#   stopping the crash also changes what the engine does, the entry says so
#   under "Also changes". Turning a guard off restores stock behaviour, which
#   for these entries means the crash comes back.
#
#   [fixes] correct engine bugs that are not crashes. Fixes DO change
#   gameplay: they make the engine apply what its own data already says.
#   Turning a fix off restores stock behaviour.
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
#   true   Guards and fixes are installed as configured below.
#   false  The DLL stays loaded and the console command still answers, but not
#          one hook is installed. Use this to rule the plugin out while chasing
#          a crash, without moving the DLL out of the folder.
# Default: true
enabled = true


# =============================================================================
[guards]
# =============================================================================

# Client unit-by-id lookup on a tombstoned unit id.
#   Site:     RVA 0009F270, the shared bottom of the client's unit-by-id
#             lookup.
#   Symptom:  Freeing a unit stamps a tombstone on the block (unit id -1) and
#             returns it to the pool, so every later resolution of that owner
#             arrives with id -1. That masks to bucket 0x7F and the walk
#             follows whatever the slot happens to hold, which is where the
#             dangling node is. Reliable repro: a summon dies while the
#             missiles from its own on-death proc are still in flight.
#   Guard:    An id of -1 is never a live unit, so the lookup answers "not
#             found" without walking. Null is this function's own not-found
#             result, so every call site already handles it. That is why the
#             guard sits in the callee and not in any one caller: the captured
#             dumps arrived through more than one.
#   Cost:     One compare against -1 per lookup. Any live id touches nothing
#             else.
# Default: true
client_unit_lookup_tombstone = true

# Life stolen per hit paid to an already dead player.
#   Site:     D2RCore.dll, the entry of its shared leech routine, which
#             ItemEventFunc 28 calls in D2RLoader 1.3.0. That is the slot both
#             the itemeventfunc columns of ItemStatCost.txt and the
#             auraeventfunc columns of Skills.txt index.
#   Symptom:  When a player dies the engine commits life to 0, then spends the
#             whole death animation in the dying and dead modes before the
#             corpse-spawn pass finishes. A leeching hit landing in that window
#             runs the leech routine against the corpse, which writes life
#             straight back into the life stat. That life is cashed in at the
#             animation-complete transition and the player revives in place:
#             server has them dead, client has them alive, and only Save and
#             Exit clears it. Auras that keep spawning leeching missiles after
#             their caster dies hit this constantly.
#   Guard:    If the unit about to be paid life per hit is a player in a
#             death animation, the routine returns without writing life and
#             without scheduling its follow-up event. That is the routine's own
#             do-nothing return, which its other early exits already use.
#             Living players, monsters, hirelings and mana leech are exactly as
#             before.
#   Cost:     Two struct reads and up to three compares per leeching hit.
# Default: true
lifesteal_while_dead = true

# Event handler recursion.
#   Site:     D2RCore.dll, inside DispatchWideEffects, the server's unit event
#             dispatcher in D2RLoader 1.3.0.
#   Symptom:  A handler can raise its own event on the same unit before it
#             returns, and the dispatcher then runs that handler again while
#             the first run is still going, with nothing limiting the depth.
#             The known case is a chance-to-cast-on-attack proc whose skill
#             drains item durability, which raises the attack event again. A
#             proc that always fires recurses until the stack overflows.
#   Guard:    A handler already running is skipped when its event is raised
#             again from inside it. Every other handler still runs and the
#             first run finishes normally.
#   Also changes: a proc can no longer trigger itself from inside its own
#             handler. In stock, a chance-based proc could chain into another
#             roll of itself within the same attack. That extra roll no longer
#             happens. Other procs are unaffected.
#   Cost:     One bit test per handler whose event matches.
# Default: true
event_handler_recursion = true


# =============================================================================
[fixes]
# =============================================================================

# Skill attack rating lost on player missiles.
#   Site:     RVA 00537B23, in server missile creation.
#   Bug:      A skill's ToHit, LevToHit and ToHitCalc give players an attack
#             rating percent, but a missile only receives it when the game
#             creates it through CreateSkillMissile. Missiles that skill
#             functions create themselves, such as Multiple Shot's srvdofunc 8,
#             roll to hit with no skill bonus at all, even though the Character
#             Screen counts it.
#   Fix:      Every player missile gets its skill's ToHit as it is created, the
#             same value CreateSkillMissile would have given it.
#   Scope:    Missiles that never roll to hit are unaffected. Monsters and
#             hirelings keep vanilla behaviour.
#   Cost:     One ToHit evaluation per player missile created without one.
# Default: true
skill_attack_rating_on_missiles = true

# Durability loss forcing a full gear refresh.
#   Site:     D2RCore.dll, in the client's item stat update handler (server
#             message 62), ReceiveWideItemStatPacket in D2RLoader 1.3.0.
#   Bug:      Every time a hit costs an item a point of durability, the server
#             sends the new value and the client answers that one number by
#             tearing down and re-applying the stats of every equipped item and
#             re-selecting both mouse skills. In melee that happens constantly,
#             mid-fight. A skill granted by an item, Whirlwind on a weapon for
#             instance, drops to 0 for an instant inside that teardown: the
#             character stops spinning while the server is still whirlwinding.
#             Indestructible weapons never stutter because they never lose
#             durability.
#   Fix:      The new durability is still applied to the item, but the gear
#             refresh is skipped for it. Durability cannot change what you have
#             equipped or what your items grant. Breaking and repairing a
#             broken item go through their own updates, untouched.
#   Cost:     One extra compare per item stat update, and it removes an entire
#             gear refresh from every durability loss.
# Default: true
durability_tick_gear_refresh = true

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

# Skill description line printed when both of its numbers are zero.
#   Site:     RVA 00289AD0, inside the SkillDesc.txt descline 75 handler.
#   Bug:      A skill tooltip is built one line at a time, each line naming a
#             descline function, a text key and the two calc expressions
#             desccalca and desccalcb. Descline 75 prints "<text>: <a>-<b>",
#             for example "Physical Damage: 12-25". Its handler refuses to draw
#             only when the text key is empty. It never looks at the two
#             numbers, so a line whose calcs both evaluate to zero still
#             prints, and you get "Physical Damage: 0-0" on skills with no
#             physical damage at that level. The neighbouring descline 74
#             already has the check this one is missing.
#   Fix:      Descline 75 draws only when desccalca or desccalcb is non-zero.
#             When both are zero the line is skipped through the same exit the
#             engine already uses for a descline it will not draw, so the rest
#             of the tooltip is unaffected and the lines below move up. A line
#             with either number non-zero behaves exactly as before.
#   Scope:    Descline 75 only, on whichever SkillDesc.txt rows use it. No
#             other descline number is touched, and nothing on the item side is
#             touched.
#   Cost:     Two compares per descline 75 line drawn.
# Default: true
hide_zero_skill_description_line = true

# Socketed gem and rune stats accumulating across resocketing.
#   Site:     RVA 0038BBA3, in the routine that removes an item from its
#             container.
#   Bug:      When a gem or rune is socketed, its properties are applied to the
#             gem or rune ITSELF, as a child entry hanging off its own stat
#             list, and the host is then handed that whole stat list. When the
#             filler is pulled back out the engine expires only the filler's
#             own entry, and expiring an entry splices it out of the chain
#             rather than taking its children with it. The child survives with
#             the socketed properties still in it, so the next socket derives
#             them from the gemsockets table a second time and applies them on
#             top of the ones that never left. Every resocket adds another
#             copy, and the values climb without limit.
#   Fix:      The engine's own detach runs first, unchanged, so the host gives
#             back exactly what it was given. Only then are the leftover
#             entries on the filler expired, which subtracts them from the
#             filler's own totals and leaves it clean for the next socket. A
#             filler that was never socketed has no such entries, and an
#             ordinary item moved between pages fails the gate. Both take a
#             path identical to stock.
#   Scope:    GEMS AND RUNES ONLY, AND IT WORKS OUT WHICH BY ITSELF. A gem or
#             rune has its socketed properties rebuilt from the gemsockets
#             table on every socket, so dropping the leftovers costs nothing. A
#             jewel carries rolled affixes with nothing to rebuild them from,
#             so its entry is the only copy and dropping it would return the
#             jewel empty. There is no row number for you to set here: the fix
#             reads the two Itemtypes rows out of the engine's own
#             socket-property routine at 00475140 and fires on exactly the
#             fillers that routine fires on. A jewel matches neither row, so
#             nothing of a jewel is ever touched. If a future patch moves those
#             rows, the fix notices and installs nothing rather than stripping
#             the wrong kind of item.
#   Cost:     One or two itemtype checks per item removed from a container.
# Default: true
resocket_stat_inflation = true

# Blood mana skills silently refused by the server.
#   Site:     RVA 00436750, the server's pre-cast affordability gate.
#   Bug:      States.txt blood_mana, state 114, makes a skill cost life instead
#             of mana. The server gate that decides whether a cast may start at
#             all has no blood_mana branch: it reads mana and nothing else. So
#             with the state up and mana below the skill's cost, the client
#             plays the entire cast -- animation, missiles, overlays -- and the
#             server drops it on the floor. No cooldown, no damage, no missile
#             collisions. The payment path immediately behind that gate already
#             knows about the state and pays in life; only the gate does not.
#   Fix:      The gate asks whether the unit has state 114 and, when it does,
#             weighs the cost against life instead of mana. That is the same
#             question the payment already asks, so the gate now admits exactly
#             the casts the payment will accept, and it is what the engine's
#             own shared affordability check has always done.
#   Also changes: while blood_mana is up, mana is ignored in BOTH directions. A
#             cast you can afford in life goes through with no mana at all,
#             and a cast you could previously start on mana alone while low on
#             life is refused up front instead of reaching the payment, which
#             would have stripped the state and left you on 1 life.
#   Scope:    State 114 only, server side. Without the state the gate behaves
#             exactly as stock, down to which stat it reads. Skills paid for
#             with item charges bypass the cost check entirely, as before.
#   Cost:     One state check per cast attempt.
# Default: true
blood_mana_cast_gate = true

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
	bool pluginEnabled                    { true };
	bool clientUnitLookupTombstone        { true };
	bool lifestealWhileDead               { true };
	bool eventHandlerRecursion            { true };
	bool skillAttackRatingOnMissiles      { true };
	bool durabilityTickGearRefresh        { true };
	bool frozenOrbBurstOnWall             { true };
	bool hideZeroSkillDescriptionLine     { true };
	bool resocketStatInflation            { true };
	bool bloodManaCastGate                { true };
	bool skillFreezeAiResume              { true };
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
			"Running with defaults: every guard ON.");
		return;
	}

	std::array<char, 32768> buffer {};
	std::uint32_t           requiredSize = 0;

	if (!context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size() - 1), &requiredSize)) {
		context->LogWarn(
			"Could not read celestialrayone.engine-stability.toml. "
			"Running with defaults: every guard ON.");
		return;
	}

	if (requiredSize >= buffer.size()) {
		D2RL::LogWarnF(
			context,
			"celestialrayone.engine-stability.toml is %u bytes, larger than the %zu byte read buffer. "
			"Running with defaults: every guard ON.",
			requiredSize,
			buffer.size());
		return;
	}

	buffer[buffer.size() - 1] = '\0';
	ConfigFileWasRead         = true;

	(void)ReadConfigBool(buffer.data(), "engine-stability", "enabled", Config.pluginEnabled);
	(void)ReadConfigBool(buffer.data(), "guards", "client_unit_lookup_tombstone", Config.clientUnitLookupTombstone);
	(void)ReadConfigBool(buffer.data(), "guards", "lifesteal_while_dead", Config.lifestealWhileDead);
	(void)ReadConfigBool(buffer.data(), "guards", "event_handler_recursion", Config.eventHandlerRecursion);
	(void)ReadConfigBool(buffer.data(), "fixes", "skill_attack_rating_on_missiles", Config.skillAttackRatingOnMissiles);
	(void)ReadConfigBool(buffer.data(), "fixes", "durability_tick_gear_refresh", Config.durabilityTickGearRefresh);
	(void)ReadConfigBool(buffer.data(), "fixes", "frozen_orb_burst_on_wall", Config.frozenOrbBurstOnWall);
	(void)ReadConfigBool(buffer.data(), "fixes", "hide_zero_skill_description_line", Config.hideZeroSkillDescriptionLine);
	(void)ReadConfigBool(buffer.data(), "fixes", "resocket_stat_inflation", Config.resocketStatInflation);
	(void)ReadConfigBool(buffer.data(), "fixes", "blood_mana_cast_gate", Config.bloodManaCastGate);
	(void)ReadConfigBool(buffer.data(), "fixes", "skill_freeze_ai_resume", Config.skillFreezeAiResume);

	D2RL::LogInfoF(
		context,
		"config: enabled=%s, client_unit_lookup_tombstone=%s, "
		"lifesteal_while_dead=%s, event_handler_recursion=%s, "
		"skill_attack_rating_on_missiles=%s, durability_tick_gear_refresh=%s, "
		"frozen_orb_burst_on_wall=%s, hide_zero_skill_description_line=%s, "
		"resocket_stat_inflation=%s, blood_mana_cast_gate=%s, "
		"skill_freeze_ai_resume=%s.",
		Config.pluginEnabled ? "true" : "false",
		Config.clientUnitLookupTombstone ? "true" : "false",
		Config.lifestealWhileDead ? "true" : "false",
		Config.eventHandlerRecursion ? "true" : "false",
		Config.skillAttackRatingOnMissiles ? "true" : "false",
		Config.durabilityTickGearRefresh ? "true" : "false",
		Config.frozenOrbBurstOnWall ? "true" : "false",
		Config.hideZeroSkillDescriptionLine ? "true" : "false",
		Config.resocketStatInflation ? "true" : "false",
		Config.bloodManaCastGate ? "true" : "false",
		Config.skillFreezeAiResume ? "true" : "false");
}

// ---------------------------------------------------------------------------
// Installation
// ---------------------------------------------------------------------------

// One row per inline hook. Everything that differs between the four hook sites
// lives here so the install and report paths stay single-copy.
struct InlineGuardSite {
	Guard               guard;
	const char*         label;      // how the guard is named in log and console
	const char*         eventNoun;  // what its suppression counter counts
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

const InlineGuardSite InlineGuardSites[] {
	{
		.guard        = Guard::DeadUnitLookup,
		.label        = "dead-unit lookup guard",
		.eventNoun    = "tombstoned lookups suppressed",
		.rvaText      = "0009F270",
		.rva          = UnitHashLookupRva,
		.body         = UnitHashLookupBody,
		.bodySize     = ByteCount(UnitHashLookupBody),
		.prologue     = UnitHashLookupPrologue,
		.prologueSize = ByteCount(UnitHashLookupPrologue),
		.hook         = reinterpret_cast<void*>(&HookUnitHashLookup),
		.original     = reinterpret_cast<void**>(&OriginalUnitHashLookup),
		.enabled      = &Config.clientUnitLookupTombstone,
	},
};

constexpr const char* SkillFreezeFixLabel = "skill-freeze AI resume fix";
constexpr const char* BloodManaFixLabel  = "blood mana cast gate fix";

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

auto InstallBloodManaCastGateFix(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr) {
		return false;
	}

	if (!Config.pluginEnabled || !Config.bloodManaCastGate) {
		SetGuardState(Guard::BloodManaCastGate, GuardState::DisabledByConfig);
		D2RL::LogInfoF(context, "%s not installed: turned off in the config file.", BloodManaFixLabel);
		return false;
	}

	// The window and the patched region are the same 221 bytes here, so this
	// check is the same bytes PatchBytes will check. It is kept because it is
	// what separates "this build is not the one this was written against" from
	// "the bytes matched and the write failed", and those must never be
	// reported as the same thing.
	if (!context->CheckExpectedBytes(BloodManaGateRva, BloodManaGateBody, ByteCount(BloodManaGateBody))) {
		SetGuardState(Guard::BloodManaCastGate, GuardState::UnsupportedBuild);
		D2RL::LogErrorF(
			context,
			"%s NOT installed: the bytes at RVA 00436750 do not match the verified 3.3.93847 gate. "
			"This build is not supported; nothing was patched.",
			BloodManaFixLabel);
		return false;
	}

	if (!context->PatchBytes(
			BloodManaGateRva,
			BloodManaGateBody,
			ByteCount(BloodManaGateBody),
			BloodManaGatePatched,
			ByteCount(BloodManaGatePatched))) {
		SetGuardState(Guard::BloodManaCastGate, GuardState::InstallFailed);
		D2RL::LogErrorF(context, "%s NOT installed: PatchBytes failed at RVA 00436750.", BloodManaFixLabel);
		return false;
	}

	SetGuardState(Guard::BloodManaCastGate, GuardState::Installed);
	D2RL::LogInfoF(context, "%s installed at RVA 00436750.", BloodManaFixLabel);
	return true;
}

// ---------------------------------------------------------------------------
// Relay page: fixes 1, 3, 4 and 5
// ---------------------------------------------------------------------------
//
// All of them sit in the middle of a function, where the loader's inline hook
// cannot go: the bytes they replace include a branch or call, which does not
// relocate. Each site is rewritten in place to reach one page allocated within
// rel32 reach of the image:
//
//   +00h  missile relay, jmp qword ptr [rip+0] to StampMissileAttackRating,
//         pointed at the missile fallback stub before the plugin unloads
//   +40h  missile fallback stub, native code only
//   +B0h  frozen orb hit-unit spill stub, native code only
//   +D0h  frozen orb burst gate stub, native code only
//   +100h zero skill description line stub, native code only
//   +140h resocket child expire stub, native code only
//
// The page is never freed. A thread may be jumping through it at any time, and
// the native stubs need nothing from this DLL.

constexpr std::size_t RelayPageBytes           = 4096;
constexpr std::size_t MissileRelayOffset       = 0x00;
constexpr std::size_t MissileFallbackOffset    = 0x40;
constexpr std::size_t FrozenOrbSpillStubOffset = 0xB0;
constexpr std::size_t FrozenOrbGateStubOffset  = 0xD0;
constexpr std::size_t DesclineStubOffset       = 0x100;
constexpr std::size_t ResocketStubOffset       = 0x140;
constexpr std::size_t AbsoluteJumpBytes        = 14;
constexpr std::size_t AbsoluteJumpTargetOffset = 6;

static_assert(MissileRelayOffset + AbsoluteJumpBytes <= MissileFallbackOffset, "Missile relay overlaps the missile fallback.");
static_assert(MissileFallbackOffset + sizeof(MissileFallbackStub) <= FrozenOrbSpillStubOffset, "Missile fallback overlaps the frozen orb spill stub.");
static_assert(FrozenOrbSpillStubOffset + sizeof(FrozenOrbSpillStub) <= FrozenOrbGateStubOffset, "Frozen orb spill stub overlaps the frozen orb gate stub.");
static_assert(FrozenOrbGateStubOffset + sizeof(FrozenOrbGateStub) <= DesclineStubOffset, "Frozen orb gate stub overlaps the descline stub.");
static_assert(DesclineStubOffset + sizeof(DesclineStub) <= ResocketStubOffset, "Descline stub overlaps the resocket stub.");
static_assert(ResocketStubOffset + sizeof(ResocketStub) <= RelayPageBytes, "Resocket stub does not fit the relay page.");

constexpr const char* EventRecursionGuardLabel = "event handler recursion guard";
constexpr const char* MissileFixLabel          = "skill attack rating on missiles fix";
constexpr const char* DurabilityFixLabel       = "durability gear refresh fix";
constexpr const char* FrozenOrbFixLabel        = "frozen orb burst-on-wall fix";
constexpr const char* DesclineFixLabel         = "zero-value skill description line fix";
constexpr const char* ResocketFixLabel         = "resocket stat inflation fix";

const D2RL::PluginContext* LoadedContext    = nullptr;
void*                      RelayPage        = nullptr;
bool                       RelayPageRefused = false;

bool MissileSitePatched        = false;
bool FrozenOrbSpillSitePatched = false;
bool FrozenOrbGateSitePatched  = false;
bool DesclineSitePatched       = false;
bool ResocketSitePatched       = false;
std::array<std::uint8_t, MissileHookSize>                    MissileSiteWritten {};
std::array<std::uint8_t, sizeof(FrozenOrbSpillTemplate)>     FrozenOrbSpillSiteWritten {};
std::array<std::uint8_t, sizeof(FrozenOrbGateTemplate)>      FrozenOrbGateSiteWritten {};
std::array<std::uint8_t, sizeof(DesclineSiteTemplate)>       DesclineSiteWritten {};
std::array<std::uint8_t, sizeof(DetachSiteTemplate)>         ResocketSiteWritten {};

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

void WriteAbsoluteJump(std::uint8_t* at, std::uint64_t target) noexcept {
	static constexpr std::uint8_t JumpQwordRip[] { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
	std::memcpy(at, JumpQwordRip, sizeof(JumpQwordRip));
	std::memcpy(at + AbsoluteJumpTargetOffset, &target, sizeof(target));
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
	void*      page      = AllocateNear(imageBase + MissileCreationStampRva, RelayPageBytes);
	if (page == nullptr) {
		context->LogError("No relay page could be allocated within rel32 reach of the game image.");
		return false;
	}

	auto* bytes = static_cast<std::uint8_t*>(page);
	std::memset(bytes, 0xCC, RelayPageBytes);

	WriteAbsoluteJump(bytes + MissileRelayOffset, reinterpret_cast<std::uint64_t>(&StampMissileAttackRating));

	const std::uint64_t setUnitStat = imageBase + SetUnitStatRva;
	std::memcpy(bytes + MissileFallbackOffset, MissileFallbackStub, sizeof(MissileFallbackStub));
	std::memcpy(bytes + MissileFallbackOffset + MissileFallbackTargetOffset, &setUnitStat, sizeof(setUnitStat));

	const std::uint64_t spillResume = imageBase + FrozenOrbSpillResumeRva;
	std::memcpy(bytes + FrozenOrbSpillStubOffset, FrozenOrbSpillStub, sizeof(FrozenOrbSpillStub));
	std::memcpy(bytes + FrozenOrbSpillStubOffset + FrozenOrbSpillResumeTargetOffset, &spillResume, sizeof(spillResume));

	const std::uint64_t burstRefuse = imageBase + FrozenOrbEpilogueRva;
	const std::uint64_t burstSpawn  = imageBase + FrozenOrbBurstRva;
	std::memcpy(bytes + FrozenOrbGateStubOffset, FrozenOrbGateStub, sizeof(FrozenOrbGateStub));
	std::memcpy(bytes + FrozenOrbGateStubOffset + FrozenOrbGateRefuseTargetOffset, &burstRefuse, sizeof(burstRefuse));
	std::memcpy(bytes + FrozenOrbGateStubOffset + FrozenOrbGateBurstTargetOffset, &burstSpawn, sizeof(burstSpawn));

	const std::uint64_t lineSkip   = imageBase + DesclineNoEmitRva;
	const std::uint64_t lineResume = imageBase + DesclineResumeRva;
	std::memcpy(bytes + DesclineStubOffset, DesclineStub, sizeof(DesclineStub));
	std::memcpy(bytes + DesclineStubOffset + DesclineNoEmitTargetOffset, &lineSkip, sizeof(lineSkip));
	std::memcpy(bytes + DesclineStubOffset + DesclineResumeTargetOffset, &lineResume, sizeof(lineResume));

	// The two gate rows are copied out of the applier windows, not out of a
	// setting, so the stub cannot disagree with the routine it is undoing.
	std::memcpy(bytes + ResocketStubOffset, ResocketStub, sizeof(ResocketStub));
	const std::uint32_t rowA        = SocketableRowA;
	const std::uint32_t rowB        = SocketableRowB;
	const std::uint64_t checkType   = imageBase + CheckItemTypeIdRva;
	const std::uint64_t findChild   = imageBase + FindChildStatListRva;
	const std::uint64_t dataContext = imageBase + GetDataContextRva;
	const std::uint64_t nodeExpire  = imageBase + ExpireStatNodeRva;
	const std::uint64_t itemDetach  = imageBase + ExpireItemStatsRva;
	const std::uint64_t detachBack  = imageBase + DetachResumeRva;
	std::memcpy(bytes + ResocketStubOffset + ResocketRowAOffset, &rowA, sizeof(rowA));
	std::memcpy(bytes + ResocketStubOffset + ResocketRowBOffset, &rowB, sizeof(rowB));
	std::memcpy(bytes + ResocketStubOffset + ResocketCheckAOffset, &checkType, sizeof(checkType));
	std::memcpy(bytes + ResocketStubOffset + ResocketCheckBOffset, &checkType, sizeof(checkType));
	std::memcpy(bytes + ResocketStubOffset + ResocketFindChildOffset, &findChild, sizeof(findChild));
	std::memcpy(bytes + ResocketStubOffset + ResocketContextOffset, &dataContext, sizeof(dataContext));
	std::memcpy(bytes + ResocketStubOffset + ResocketExpireOffset, &nodeExpire, sizeof(nodeExpire));
	std::memcpy(bytes + ResocketStubOffset + ResocketDetachOffset, &itemDetach, sizeof(itemDetach));
	std::memcpy(bytes + ResocketStubOffset + ResocketResumeOffset, &detachBack, sizeof(detachBack));

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

auto InstallMissileSkillAttackRatingFix(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr) {
		return false;
	}

	if (!Config.pluginEnabled || !Config.skillAttackRatingOnMissiles) {
		SetGuardState(Guard::MissileSkillAttackRating, GuardState::DisabledByConfig);
		D2RL::LogInfoF(context, "%s not installed: turned off in the config file.", MissileFixLabel);
		return false;
	}

	struct Witness {
		std::uint64_t       rva;
		const std::uint8_t* bytes;
		std::uint32_t       size;
		const char*         what;
	};
	const Witness witnesses[] {
		{ MissileCreationStampRva, CreationStampWindow, ByteCount(CreationStampWindow), "missile creation stamp at RVA 00537B23" },
		{ CreateSkillMissileCallRva, SkillToHitCallWindow, ByteCount(SkillToHitCallWindow), "CreateSkillMissile ToHit call at RVA 004336A2" },
		{ SkillRecordGetterRva, SkillRecordWindow, ByteCount(SkillRecordWindow), "skills table record getter at RVA 00097790" },
	};
	for (const Witness& witness : witnesses) {
		if (!context->CheckExpectedBytes(witness.rva, witness.bytes, witness.size)) {
			SetGuardState(Guard::MissileSkillAttackRating, GuardState::UnsupportedBuild);
			D2RL::LogErrorF(
				context,
				"%s NOT installed: the %s does not match the verified 3.3.93847 bytes. "
				"This build is not supported, or another patch already owns the site; nothing was patched.",
				MissileFixLabel,
				witness.what);
			return false;
		}
	}

	if (!PrepareRelayPage(context)) {
		SetGuardState(Guard::MissileSkillAttackRating, GuardState::InstallFailed);
		D2RL::LogErrorF(context, "%s NOT installed: no relay page.", MissileFixLabel);
		return false;
	}

	const auto imageBase = static_cast<std::uintptr_t>(context->exeBase);
	GetDataTables = reinterpret_cast<GetDataTablesFn>(imageBase + GetDataTablesRva);
	SetUnitStat   = reinterpret_cast<SetUnitStatFn>(imageBase + SetUnitStatRva);
	SkillToHit    = reinterpret_cast<SkillToHitFn>(imageBase + SkillToHitRva);

	const auto next   = imageBase + MissileCreationStampRva + MissileHookRel32Offset + sizeof(std::int32_t);
	const auto target = reinterpret_cast<std::uintptr_t>(RelayPage) + MissileRelayOffset;
	if (!WithinRel32(next, target)) {
		SetGuardState(Guard::MissileSkillAttackRating, GuardState::InstallFailed);
		D2RL::LogErrorF(context, "%s NOT installed: the relay page is out of rel32 reach of RVA 00537B23.", MissileFixLabel);
		return false;
	}

	const auto displacement = static_cast<std::int32_t>(static_cast<std::int64_t>(target) - static_cast<std::int64_t>(next));
	std::memcpy(MissileSiteWritten.data(), MissileHookCode, MissileHookSize);
	std::memcpy(MissileSiteWritten.data() + MissileHookRel32Offset, &displacement, sizeof(displacement));

	if (!context->PatchBytes(
			MissileCreationStampRva,
			CreationStampWindow,
			static_cast<std::uint32_t>(MissileHookSize),
			MissileSiteWritten.data(),
			static_cast<std::uint32_t>(MissileSiteWritten.size()))) {
		SetGuardState(Guard::MissileSkillAttackRating, GuardState::InstallFailed);
		D2RL::LogErrorF(context, "%s NOT installed: PatchBytes failed at RVA 00537B23.", MissileFixLabel);
		return false;
	}

	MissileSitePatched = true;
	SetGuardState(Guard::MissileSkillAttackRating, GuardState::Installed);
	D2RL::LogInfoF(context, "%s installed at RVA 00537B23.", MissileFixLabel);
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


auto InstallZeroSkillDescriptionLineFix(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr) {
		return false;
	}

	if (!Config.pluginEnabled || !Config.hideZeroSkillDescriptionLine) {
		SetGuardState(Guard::ZeroSkillDescriptionLine, GuardState::DisabledByConfig);
		D2RL::LogInfoF(context, "%s not installed: turned off in the config file.", DesclineFixLabel);
		return false;
	}

	if (!context->CheckExpectedBytes(DesclineHandler75Rva, DesclineHandler75Body, DesclineResolveRel32Offset)
		|| !context->CheckExpectedBytes(
			DesclineHandler75Rva + DesclineResolveRel32End,
			DesclineHandler75Body + DesclineResolveRel32End,
			ByteCount(DesclineHandler75Body) - DesclineResolveRel32End)) {
		SetGuardState(Guard::ZeroSkillDescriptionLine, GuardState::UnsupportedBuild);
		D2RL::LogErrorF(
			context,
			"%s NOT installed: the bytes at RVA 00289AAA do not match the verified 3.3.93847 descline 75 handler. "
			"This build is not supported, or another patch already owns the site; nothing was patched.",
			DesclineFixLabel);
		return false;
	}

	if (!PrepareRelayPage(context)) {
		SetGuardState(Guard::ZeroSkillDescriptionLine, GuardState::InstallFailed);
		D2RL::LogErrorF(context, "%s NOT installed: no relay page.", DesclineFixLabel);
		return false;
	}

	const auto imageBase = static_cast<std::uintptr_t>(context->exeBase);
	const auto relayPage = reinterpret_cast<std::uintptr_t>(RelayPage);
	const auto next      = imageBase + DesclineSiteRva + DesclineSiteRel32Offset + sizeof(std::int32_t);
	const auto target    = relayPage + DesclineStubOffset;

	if (!WithinRel32(next, target)) {
		SetGuardState(Guard::ZeroSkillDescriptionLine, GuardState::InstallFailed);
		D2RL::LogErrorF(context, "%s NOT installed: the relay page is out of rel32 reach of RVA 00289AD0.", DesclineFixLabel);
		return false;
	}

	const auto displacement = static_cast<std::int32_t>(static_cast<std::int64_t>(target) - static_cast<std::int64_t>(next));
	std::memcpy(DesclineSiteWritten.data(), DesclineSiteTemplate, sizeof(DesclineSiteTemplate));
	std::memcpy(DesclineSiteWritten.data() + DesclineSiteRel32Offset, &displacement, sizeof(displacement));

	if (!context->PatchBytes(
			DesclineSiteRva,
			DesclineSiteOriginal,
			ByteCount(DesclineSiteOriginal),
			DesclineSiteWritten.data(),
			static_cast<std::uint32_t>(DesclineSiteWritten.size()))) {
		SetGuardState(Guard::ZeroSkillDescriptionLine, GuardState::InstallFailed);
		D2RL::LogErrorF(context, "%s NOT installed: PatchBytes failed at RVA 00289AD0.", DesclineFixLabel);
		return false;
	}

	DesclineSitePatched = true;
	SetGuardState(Guard::ZeroSkillDescriptionLine, GuardState::Installed);
	D2RL::LogInfoF(context, "%s installed at RVA 00289AD0.", DesclineFixLabel);
	return true;
}


auto InstallResocketStatInflationFix(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr) {
		return false;
	}

	if (!Config.pluginEnabled || !Config.resocketStatInflation) {
		SetGuardState(Guard::ResocketStatInflation, GuardState::DisabledByConfig);
		D2RL::LogInfoF(context, "%s not installed: turned off in the config file.", ResocketFixLabel);
		return false;
	}

	if (!context->CheckExpectedBytes(DetachWindowRva, DetachWindow, ByteCount(DetachWindow))) {
		SetGuardState(Guard::ResocketStatInflation, GuardState::UnsupportedBuild);
		D2RL::LogErrorF(
			context,
			"%s NOT installed: the bytes at RVA 0038BB70 do not match the verified 3.3.93847 container-removal detach. "
			"This build is not supported, or another patch already owns the site; nothing was patched.",
			ResocketFixLabel);
		return false;
	}

	// The gate rows are only correct because they came out of these two
	// windows. If either has moved, the fix has no way to tell a gem from a
	// jewel on this build and must not run: getting that backwards empties
	// jewels, which is worse than leaving the inflation in place.
	if (!context->CheckExpectedBytes(ApplierGateARva, ApplierGateA, ByteCount(ApplierGateA))
		|| !context->CheckExpectedBytes(ApplierGateBRva, ApplierGateB, ByteCount(ApplierGateB))) {
		SetGuardState(Guard::ResocketStatInflation, GuardState::UnsupportedBuild);
		D2RL::LogErrorF(
			context,
			"%s NOT installed: the itemtype gates at RVA 004751B9 and 00475283 inside the socket property "
			"routine do not match this build, so which fillers are safe to clean cannot be established. "
			"Nothing was patched.",
			ResocketFixLabel);
		return false;
	}

	if (!PrepareRelayPage(context)) {
		SetGuardState(Guard::ResocketStatInflation, GuardState::InstallFailed);
		D2RL::LogErrorF(context, "%s NOT installed: no relay page.", ResocketFixLabel);
		return false;
	}

	const auto imageBase = static_cast<std::uintptr_t>(context->exeBase);
	const auto relayPage = reinterpret_cast<std::uintptr_t>(RelayPage);
	const auto next      = imageBase + DetachSiteRva + DetachSiteRel32Offset + sizeof(std::int32_t);
	const auto target    = relayPage + ResocketStubOffset;

	if (!WithinRel32(next, target)) {
		SetGuardState(Guard::ResocketStatInflation, GuardState::InstallFailed);
		D2RL::LogErrorF(context, "%s NOT installed: the relay page is out of rel32 reach of RVA 0038BBA3.", ResocketFixLabel);
		return false;
	}

	const auto displacement = static_cast<std::int32_t>(static_cast<std::int64_t>(target) - static_cast<std::int64_t>(next));
	std::memcpy(ResocketSiteWritten.data(), DetachSiteTemplate, sizeof(DetachSiteTemplate));
	std::memcpy(ResocketSiteWritten.data() + DetachSiteRel32Offset, &displacement, sizeof(displacement));

	if (!context->PatchBytes(
			DetachSiteRva,
			DetachSiteOriginal,
			ByteCount(DetachSiteOriginal),
			ResocketSiteWritten.data(),
			static_cast<std::uint32_t>(ResocketSiteWritten.size()))) {
		SetGuardState(Guard::ResocketStatInflation, GuardState::InstallFailed);
		D2RL::LogErrorF(context, "%s NOT installed: PatchBytes failed at RVA 0038BBA3.", ResocketFixLabel);
		return false;
	}

	ResocketSitePatched = true;
	SetGuardState(Guard::ResocketStatInflation, GuardState::Installed);
	D2RL::LogInfoF(
		context,
		"%s installed at RVA 0038BBA3, cleaning fillers on Itemtypes.txt rows %u and %u, "
		"which are the rows the socket property routine at 00475140 applies to.",
		ResocketFixLabel,
		SocketableRowA,
		SocketableRowB);
	return true;
}

// On unload the missile relay is pointed at the native fallback first, so a
// call already on its way through it never lands in an unloaded DLL, then every
// site gets its original bytes back. The page itself is kept.
void WithdrawRelaySites() noexcept {
	const D2RL::PluginContext* context = LoadedContext;
	if (context == nullptr || RelayPage == nullptr) {
		return;
	}

	auto* bytes    = static_cast<std::uint8_t*>(RelayPage);
	DWORD previous = 0;
	if (VirtualProtect(RelayPage, RelayPageBytes, PAGE_EXECUTE_READWRITE, &previous)) {
		const std::uint64_t fallback = reinterpret_cast<std::uint64_t>(bytes) + MissileFallbackOffset;
		std::memcpy(bytes + MissileRelayOffset + AbsoluteJumpTargetOffset, &fallback, sizeof(fallback));
		DWORD ignored = 0;
		VirtualProtect(RelayPage, RelayPageBytes, previous, &ignored);
		FlushInstructionCache(GetCurrentProcess(), RelayPage, RelayPageBytes);
	}

	if (MissileSitePatched
		&& context->PatchBytes(
			MissileCreationStampRva,
			MissileSiteWritten.data(),
			static_cast<std::uint32_t>(MissileSiteWritten.size()),
			CreationStampWindow,
			static_cast<std::uint32_t>(MissileHookSize))) {
		MissileSitePatched = false;
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

	if (DesclineSitePatched
		&& context->PatchBytes(
			DesclineSiteRva,
			DesclineSiteWritten.data(),
			static_cast<std::uint32_t>(DesclineSiteWritten.size()),
			DesclineSiteOriginal,
			ByteCount(DesclineSiteOriginal))) {
		DesclineSitePatched = false;
	}

	if (ResocketSitePatched
		&& context->PatchBytes(
			DetachSiteRva,
			ResocketSiteWritten.data(),
			static_cast<std::uint32_t>(ResocketSiteWritten.size()),
			DetachSiteOriginal,
			ByteCount(DetachSiteOriginal))) {
		ResocketSitePatched = false;
	}
}

// ---------------------------------------------------------------------------
// D2RCore sites: guard 5, guard 7 and fix 2
// ---------------------------------------------------------------------------
//
// In D2RLoader 1.3.0 the three routines these entries patch are D2RCore code,
// and the loader's patch API only addresses the game image. These three sites
// are therefore found, verified, written and restored by the plugin itself:
//
//   Found:    each routine is reached from a named D2RCore export. The export's
//             stub is compared byte for byte, leaving out only its rip-relative
//             displacements and the rel32 of its call to the routine, which move
//             between loader builds. The routine is that call's target.
//               DispatchWideEffects        -> dispatch loop             guard 7
//               ReceiveWideItemStatPacket  -> item stat apply           fix 2
//               RegisterWideSkillEffect    -> event handler table, entry 28
//                                             -> leech routine          guard 5
//   Verified: every window a patch relies on is compared the same way, and every
//             address read is checked to be committed memory inside D2RCore.dll
//             (code must also be executable), before a byte is written.
//   Written:  guard 7 and fix 2 jump from their site into a stub on one page
//             allocated within rel32 reach of D2RCore. Guard 5 replaces the leech
//             routine's first 14 bytes with an absolute jump into this DLL, and a
//             trampoline on the same page replays them.
//   Restored: on unload each site gets its original bytes back if it still holds
//             what the plugin wrote. The page is kept.
//
// D2RLoader's diagnostics do not list these three patches, because the loader
// did not write them. The console command reports them like every other entry.
//
// Page layout:
//   +00h  event handler recursion stub
//   +40h  durability gear refresh stub
//   +80h  leech routine trampoline

constexpr wchar_t CoreModuleName[] = L"D2RCore.dll";

constexpr std::size_t CoreRecursionStubOffset   = 0x00;
constexpr std::size_t CoreDurabilityStubOffset  = 0x40;
constexpr std::size_t CoreLeechTrampolineOffset = 0x80;
constexpr std::size_t CoreLeechPatchBytes       = 14;

static_assert(CoreRecursionStubOffset + sizeof(CoreRecursionStub) <= CoreDurabilityStubOffset, "Recursion stub overlaps the durability stub.");
static_assert(CoreDurabilityStubOffset + sizeof(CoreDurabilityStub) <= CoreLeechTrampolineOffset, "Durability stub overlaps the leech trampoline.");
static_assert(CoreLeechTrampolineOffset + CoreLeechPatchBytes + AbsoluteJumpBytes <= RelayPageBytes, "Leech trampoline does not fit the page.");
static_assert(CoreLeechPatchBytes == AbsoluteJumpBytes, "The leech entry patch is exactly one absolute jump.");

// A byte range a comparison leaves out.
struct ByteGap {
	std::size_t offset;
	std::size_t size;
};

// --- Export stubs, D2RCore.dll as shipped with D2RLoader 1.3.0 --------------

// DispatchWideEffects, 66 bytes up to its call of the dispatch loop.
//   +17h  mov r11, [rip+disp32]      security cookie     disp32 not compared
//   +3Dh  call dispatch loop                             rel32 not compared
constexpr std::uint8_t CoreDispatchStub[] {
	0x48, 0x83, 0xEC, 0x48, 0x4C, 0x89, 0xC0, 0x41, 0x89, 0xD0, 0x48, 0x89,
	0xCA, 0x48, 0x8B, 0x4C, 0x24, 0x70, 0x4C, 0x8B, 0x54, 0x24, 0x78, 0x4C,
	0x8B, 0x1D, 0x72, 0xE1, 0xEC, 0xFF, 0x49, 0x31, 0xE3, 0x4C, 0x89, 0x5C,
	0x24, 0x40, 0x4C, 0x89, 0x54, 0x24, 0x38, 0x48, 0x89, 0x4C, 0x24, 0x28,
	0x4C, 0x89, 0x4C, 0x24, 0x20, 0x48, 0x8D, 0x4C, 0x24, 0x38, 0x49, 0x89,
	0xC1, 0xE8, 0x2E, 0x11, 0xC3, 0xFF,
};

constexpr ByteGap CoreDispatchStubGaps[] { { 0x1A, 4 }, { 0x3E, 4 } };
constexpr std::size_t CoreDispatchCallOffset = 0x3D;

// RegisterWideSkillEffect, 84 bytes up to its call of the registrar.
//   +2Ch  lea rdi, [rip+disp32]      D2RCore's event handler table, handed to
//                                    the registrar, which stores table[func] on
//                                    every node it creates
//   +4Fh  call registrar                                 rel32 not compared
constexpr std::uint8_t CoreRegisterStub[] {
	0x56, 0x57, 0x48, 0x83, 0xEC, 0x58, 0x8B, 0x84, 0x24, 0x90, 0x00, 0x00,
	0x00, 0x44, 0x8B, 0x94, 0x24, 0x98, 0x00, 0x00, 0x00, 0x44, 0x8B, 0x9C,
	0x24, 0xA0, 0x00, 0x00, 0x00, 0x8B, 0xB4, 0x24, 0xA8, 0x00, 0x00, 0x00,
	0x0F, 0x28, 0x84, 0x24, 0xB0, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x3D, 0xED,
	0x88, 0xE1, 0xFF, 0x48, 0x89, 0x7C, 0x24, 0x50, 0x0F, 0x11, 0x44, 0x24,
	0x40, 0x89, 0x74, 0x24, 0x38, 0x44, 0x89, 0x5C, 0x24, 0x30, 0x44, 0x89,
	0x54, 0x24, 0x28, 0x89, 0x44, 0x24, 0x20, 0xE8, 0x0C, 0x13, 0xC3, 0xFF,
};

constexpr ByteGap CoreRegisterStubGaps[] { { 0x2F, 4 }, { 0x50, 4 } };
constexpr std::size_t CoreRegisterTableLeaOffset = 0x2C;
constexpr std::size_t CoreLeechHandlerIndex      = 28;

// ReceiveWideItemStatPacket, 85 bytes up to its call of the apply routine.
//   +04h  mov rax, [rip+disp32]      security cookie     disp32 not compared
//   +3Dh  call message decoder                           rel32 not compared
//   +50h  call apply routine                             rel32 not compared
constexpr std::uint8_t CoreItemStatStub[] {
	0x48, 0x83, 0xEC, 0x68, 0x48, 0x8B, 0x05, 0xD5, 0xD5, 0xEC, 0xFF, 0x48,
	0x31, 0xE0, 0x48, 0x89, 0x44, 0x24, 0x60, 0x48, 0x85, 0xC9, 0x74, 0x3D,
	0x0F, 0xB6, 0x41, 0x01, 0x0F, 0x57, 0xC0, 0x0F, 0x29, 0x44, 0x24, 0x40,
	0xC6, 0x44, 0x24, 0x50, 0x00, 0x48, 0x89, 0x4C, 0x24, 0x30, 0x48, 0x89,
	0x44, 0x24, 0x38, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0x48, 0x8D, 0x54, 0x24,
	0x40, 0xE8, 0xBE, 0x1D, 0xC3, 0xFF, 0x84, 0xC0, 0x74, 0x0F, 0x48, 0x8D,
	0x4C, 0x24, 0x2F, 0x48, 0x8D, 0x54, 0x24, 0x40, 0xE8, 0x9B, 0x23, 0xC3,
	0xFF,
};

constexpr ByteGap CoreItemStatStubGaps[] { { 0x07, 4 }, { 0x3E, 4 }, { 0x51, 4 } };
constexpr std::size_t CoreItemStatCallOffset = 0x50;

// --- Guard 7 windows, offsets from the dispatch loop's entry (003DC460) -----

constexpr std::size_t CoreDispatchMatchOffset = 0x050;  // 003DC4B0
constexpr std::size_t CoreDispatchAfterOffset = 0x1A8;  // 003DC608
constexpr std::size_t CoreRecursionSiteOffset = 0x066;  // 003DC4C6
constexpr std::size_t CoreRecursionRunOffset  = 0x070;  // 003DC4D0
constexpr std::size_t CoreRecursionNextOffset = 0x050;  // 003DC4B0

// 003DC4B0..003DC4DB: next node, end test, event id match, mark in progress.
constexpr std::uint8_t CoreDispatchMatchWindow[] {
	0x4D, 0x8B, 0x7C, 0x24, 0x38, 0x4D, 0x89, 0xFC, 0x4D, 0x85, 0xFF, 0x0F,
	0x84, 0xC1, 0x01, 0x00, 0x00, 0x41, 0x0F, 0xB6, 0x04, 0x24, 0x39, 0xC7,
	0x75, 0xE6, 0x45, 0x0F, 0xB7, 0x6C, 0x24, 0x02, 0x44, 0x89, 0xE8, 0x83,
	0xC8, 0x01, 0x66, 0x41, 0x89, 0x44, 0x24, 0x02,
};

// 003DC608..003DC629: after the handler, keep or clear the mark.
constexpr std::uint8_t CoreDispatchAfterWindow[] {
	0x89, 0xC5, 0x4D, 0x8B, 0x7C, 0x24, 0x38, 0x41, 0xF6, 0xC5, 0x01, 0x0F,
	0x85, 0x9C, 0xFE, 0xFF, 0xFF, 0x41, 0x0F, 0xB7, 0x44, 0x24, 0x02, 0x89,
	0xC1, 0x83, 0xE1, 0xFE, 0x66, 0x41, 0x89, 0x4C, 0x24, 0x02,
};

static_assert(IsSubrangeOf(CoreRecursionSiteOriginal, CoreDispatchMatchWindow, CoreRecursionSiteOffset - CoreDispatchMatchOffset), "The replaced bytes must sit inside the verified match window.");
static_assert(CoreDispatchMatchWindow[CoreRecursionRunOffset - CoreDispatchMatchOffset] == 0x44, "The run exit must start with mov eax, r13d.");
static_assert(CoreDispatchMatchWindow[0x18] == 0x75 && static_cast<std::int8_t>(CoreDispatchMatchWindow[0x19]) == -0x1A, "The stock jne must target the next-node block at the start of the window.");
static_assert(CoreDispatchAfterWindow[0x07] == 0x41 && CoreDispatchAfterWindow[0x08] == 0xF6 && CoreDispatchAfterWindow[0x09] == 0xC5 && CoreDispatchAfterWindow[0x0A] == 0x01, "The unmark path must still test r13b, 1.");

// --- Guard 5 windows ---------------------------------------------------------

// Handler 28 (003DBB20), 57 bytes up to its call of the leech routine.
//   +04h  mov rax, [rip+disp32]      security cookie     disp32 not compared
//   +2Fh  mov edx, 3                 mode 3
//   +34h  call leech routine                             rel32 not compared
constexpr std::uint8_t CoreLeechHandlerWindow[] {
	0x48, 0x83, 0xEC, 0x48, 0x48, 0x8B, 0x05, 0x55, 0xD9, 0x29, 0x00, 0x48,
	0x31, 0xE0, 0x48, 0x89, 0x44, 0x24, 0x40, 0x4D, 0x85, 0xC0, 0x74, 0x23,
	0x48, 0x8B, 0x44, 0x24, 0x78, 0x48, 0x89, 0x44, 0x24, 0x28, 0xC7, 0x44,
	0x24, 0x20, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x4C, 0x24, 0x3F, 0xBA,
	0x03, 0x00, 0x00, 0x00, 0xE8, 0xC7, 0x6C, 0x00, 0x00,
};

constexpr ByteGap CoreLeechHandlerGaps[] { { 0x07, 4 }, { 0x35, 4 } };
constexpr std::size_t CoreLeechHandlerCallOffset = 0x34;

// Leech routine entry (003E2820), 56 bytes. The first 14 are the ones patched:
// push r15 / push r14 / push r12 / push rsi / push rdi / push rbp / push rbx /
// sub rsp, 40h, all position independent.
//   +0Eh  mov rax, [rip+disp32]      security cookie     disp32 not compared
//   +28h  mov rsi, r8                the leecher
//   +2Bh  mov edi, edx               mode
constexpr std::uint8_t CoreLeechEntryWindow[] {
	0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x56, 0x57, 0x55, 0x53, 0x48, 0x83,
	0xEC, 0x40, 0x48, 0x8B, 0x05, 0x4B, 0x6C, 0x29, 0x00, 0x48, 0x31, 0xE0,
	0x48, 0x89, 0x44, 0x24, 0x38, 0x31, 0xED, 0x4D, 0x85, 0xC0, 0x0F, 0x84,
	0x77, 0x01, 0x00, 0x00, 0x4C, 0x89, 0xC6, 0x89, 0xD7, 0x44, 0x8B, 0xA4,
	0x24, 0xA0, 0x00, 0x00, 0x00, 0x83, 0xFA, 0x01,
};

constexpr ByteGap CoreLeechEntryGaps[] { { 0x11, 4 } };

// 003E28D0..003E28E6: modes 2 and 3 read and write stat 6 (life), capped by 7.
constexpr std::size_t CoreLeechLifeStatOffset = 0xB0;
constexpr std::uint8_t CoreLeechLifeStatWindow[] {
	0x31, 0xC0, 0x83, 0xFF, 0x02, 0x0F, 0x92, 0xC0, 0x44, 0x8D, 0x34, 0x45,
	0x06, 0x00, 0x00, 0x00, 0x8D, 0x14, 0x45, 0x07, 0x00, 0x00, 0x00,
};

static_assert(CoreLeechHandlerWindow[CoreLeechHandlerCallOffset] == 0xE8, "The leech call is not where the handler map says it is.");
static_assert(CoreLeechHandlerWindow[0x2F] == 0xBA && CoreLeechHandlerWindow[0x30] == CoreLeechModeLifePerHit, "Handler 28 must call the leech routine in mode 3.");
static_assert(CoreLeechEntryGaps[0].offset >= CoreLeechPatchBytes, "The patched entry bytes must all be compared.");
static_assert(CoreLeechEntryWindow[0x28] == 0x4C && CoreLeechEntryWindow[0x29] == 0x89 && CoreLeechEntryWindow[0x2A] == 0xC6, "The leecher must be r8.");
static_assert(CoreLeechEntryWindow[0x2B] == 0x89 && CoreLeechEntryWindow[0x2C] == 0xD7, "The mode must be edx.");

// --- Fix 2 windows, offsets from the item stat apply routine's entry (003DE290)

constexpr std::size_t CoreItemStatIdOffset          = 0x042;  // 003DE2D2
constexpr std::size_t CoreItemStatSiteWindowOffset  = 0x0CC;  // 003DE35C
constexpr std::size_t CoreDurabilitySiteOffset      = 0x0E8;  // 003DE378
constexpr std::size_t CoreDurabilityQuantityOffset  = 0x0EE;  // 003DE37E
constexpr std::size_t CoreDurabilityRefreshOffset   = 0x116;  // 003DE3A6
constexpr std::size_t CoreDurabilityExitOffset      = 0x132;  // 003DE3C2
constexpr std::size_t CoreGearRefreshCallOffset     = 0x12C;  // 003DE3BC

// 003DE2D2: mov ebx, [rdi+4] / cmp rbx, 0CCh -- the update's stat id is [rdi+4].
constexpr std::uint8_t CoreItemStatIdWindow[] {
	0x8B, 0x5F, 0x04, 0x48, 0x81, 0xFB, 0xCC, 0x00, 0x00, 0x00,
};

// 003DE35C..003DE3C5: local player, quantity test, quantity block, refresh path.
// The six call qword ptr [rip+disp32] displacements are not compared.
constexpr std::uint8_t CoreItemStatSiteWindow[] {
	0xFF, 0x15, 0x5E, 0xFA, 0x29, 0x00, 0x89, 0xC1, 0xFF, 0x15, 0x86, 0xFA,
	0x29, 0x00, 0x48, 0x89, 0xC1, 0xB0, 0x01, 0x48, 0x85, 0xC9, 0x0F, 0x84,
	0x88, 0x00, 0x00, 0x00, 0x83, 0x7F, 0x04, 0x46, 0x75, 0x28, 0x83, 0x7F,
	0x08, 0x00, 0x7E, 0x7C, 0x48, 0x8B, 0x4E, 0x10, 0x48, 0x85, 0xC9, 0x74,
	0x73, 0xF6, 0x41, 0x19, 0x40, 0x74, 0x6D, 0x48, 0x89, 0xF1, 0xBA, 0x00,
	0x40, 0x00, 0x00, 0x45, 0x31, 0xC0, 0xFF, 0x15, 0xA4, 0xF2, 0x29, 0x00,
	0xEB, 0x1C, 0xFF, 0x15, 0x14, 0xFA, 0x29, 0x00, 0x89, 0xC1, 0xFF, 0x15,
	0x3C, 0xFA, 0x29, 0x00, 0x48, 0x85, 0xC0, 0x74, 0x09, 0x48, 0x89, 0xC1,
	0xFF, 0x15, 0xC6, 0xEB, 0x29, 0x00, 0xB0, 0x01, 0xEB, 0x3A,
};

constexpr ByteGap CoreItemStatSiteGaps[] {
	{ 0x02, 4 }, { 0x0A, 4 }, { 0x44, 4 }, { 0x4C, 4 }, { 0x54, 4 }, { 0x62, 4 },
};

static_assert(IsSubrangeOf(CoreDurabilitySiteOriginal, CoreItemStatSiteWindow, CoreDurabilitySiteOffset - CoreItemStatSiteWindowOffset), "The replaced bytes must sit inside the verified window.");
static_assert(CoreDurabilityQuantityOffset == CoreDurabilitySiteOffset + sizeof(CoreDurabilitySiteOriginal), "The quantity block is the instruction after the site.");
static_assert(CoreDurabilityRefreshOffset == CoreDurabilityQuantityOffset + CoreDurabilitySiteOriginal[5], "The refresh path is the stock jne target.");
static_assert(CoreItemStatSiteWindow[CoreDurabilityQuantityOffset - CoreItemStatSiteWindowOffset] == 0x83, "The quantity block must start with its cmp.");
static_assert(CoreItemStatSiteWindow[CoreDurabilityRefreshOffset - CoreItemStatSiteWindowOffset] == 0xFF, "The refresh path must start with its local player call.");
static_assert(CoreItemStatSiteWindow[CoreDurabilityExitOffset - CoreItemStatSiteWindowOffset] == 0xB0, "The shared exit must start with mov al, 1.");
static_assert(CoreItemStatSiteWindow[CoreGearRefreshCallOffset - CoreItemStatSiteWindowOffset] == 0xFF && CoreItemStatSiteWindow[CoreGearRefreshCallOffset - CoreItemStatSiteWindowOffset + 1] == 0x15, "The gear refresh call is not where the map says it is.");

// --- Runtime ----------------------------------------------------------------

constexpr const char* LifestealGuardLabel = "lifesteal-while-dead guard";

HMODULE       CoreModule         = nullptr;
void*         CorePage           = nullptr;
bool          CorePageRefused    = false;
std::uint8_t* CoreRecursionSite  = nullptr;
std::uint8_t* CoreDurabilitySite = nullptr;
std::uint8_t* CoreLeechEntry     = nullptr;
std::array<std::uint8_t, sizeof(CoreRecursionSiteTemplate)>  CoreRecursionWritten {};
std::array<std::uint8_t, sizeof(CoreDurabilitySiteTemplate)> CoreDurabilityWritten {};
std::array<std::uint8_t, CoreLeechPatchBytes>                CoreLeechWritten {};

// True when [address, address + size) is committed memory inside D2RCore.dll,
// and executable when `code` is set.
auto IsCoreMemory(const std::uint8_t* address, std::size_t size, bool code) noexcept -> bool {
	if (CoreModule == nullptr || address == nullptr) {
		return false;
	}
	MEMORY_BASIC_INFORMATION info {};
	if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info)) {
		return false;
	}
	if (info.State != MEM_COMMIT || info.AllocationBase != static_cast<void*>(CoreModule)
		|| (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
		return false;
	}
	const auto* regionEnd = static_cast<const std::uint8_t*>(info.BaseAddress) + info.RegionSize;
	if (address + size > regionEnd) {
		return false;
	}
	if (!code) {
		return true;
	}
	const DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
	return (info.Protect & executable) != 0;
}

auto CoreMatchesBytes(
	const std::uint8_t* at,
	const std::uint8_t* expected,
	std::size_t         size,
	const ByteGap*      gaps,
	std::size_t         gapCount) noexcept -> bool {
	if (!IsCoreMemory(at, size, true)) {
		return false;
	}
	for (std::size_t index = 0; index < size; ++index) {
		bool skipped = false;
		for (std::size_t gap = 0; gap < gapCount; ++gap) {
			if (index >= gaps[gap].offset && index < gaps[gap].offset + gaps[gap].size) {
				skipped = true;
				break;
			}
		}
		if (!skipped && at[index] != expected[index]) {
			return false;
		}
	}
	return true;
}

template <std::size_t Size, std::size_t GapCount>
auto CoreMatches(const std::uint8_t* at, const std::uint8_t (&expected)[Size], const ByteGap (&gaps)[GapCount]) noexcept -> bool {
	return CoreMatchesBytes(at, expected, Size, gaps, GapCount);
}

template <std::size_t Size>
auto CoreMatches(const std::uint8_t* at, const std::uint8_t (&expected)[Size]) noexcept -> bool {
	return CoreMatchesBytes(at, expected, Size, nullptr, 0);
}

// Target of a verified instruction whose rip-relative operand ends it.
auto Rel32Target(const std::uint8_t* instruction, std::size_t displacementOffset, std::size_t length) noexcept -> const std::uint8_t* {
	std::int32_t displacement = 0;
	std::memcpy(&displacement, instruction + displacementOffset, sizeof(displacement));
	return reinterpret_cast<const std::uint8_t*>(
		reinterpret_cast<std::uintptr_t>(instruction) + length
		+ static_cast<std::uintptr_t>(static_cast<std::intptr_t>(displacement)));
}

auto CoreExport(const char* name) noexcept -> const std::uint8_t* {
	return CoreModule != nullptr ? reinterpret_cast<const std::uint8_t*>(GetProcAddress(CoreModule, name)) : nullptr;
}

auto CoreRva(const std::uint8_t* address) noexcept -> unsigned long long {
	return static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(CoreModule));
}

auto LocateCoreDispatchLoop() noexcept -> const std::uint8_t* {
	const auto* stub = CoreExport("DispatchWideEffects");
	if (stub == nullptr || !CoreMatches(stub, CoreDispatchStub, CoreDispatchStubGaps)) {
		return nullptr;
	}
	const auto* loop = Rel32Target(stub + CoreDispatchCallOffset, 1, 5);
	if (!CoreMatches(loop + CoreDispatchMatchOffset, CoreDispatchMatchWindow)
		|| !CoreMatches(loop + CoreDispatchAfterOffset, CoreDispatchAfterWindow)) {
		return nullptr;
	}
	return loop;
}

auto LocateCoreLeechRoutine() noexcept -> const std::uint8_t* {
	const auto* stub = CoreExport("RegisterWideSkillEffect");
	if (stub == nullptr || !CoreMatches(stub, CoreRegisterStub, CoreRegisterStubGaps)) {
		return nullptr;
	}
	const auto* table = Rel32Target(stub + CoreRegisterTableLeaOffset, 3, 7);
	const auto* entry = table + CoreLeechHandlerIndex * sizeof(std::uint64_t);
	if (!IsCoreMemory(entry, sizeof(std::uint64_t), false)) {
		return nullptr;
	}
	std::uint64_t handlerAddress = 0;
	std::memcpy(&handlerAddress, entry, sizeof(handlerAddress));
	const auto* handler = reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(handlerAddress));
	if (!CoreMatches(handler, CoreLeechHandlerWindow, CoreLeechHandlerGaps)) {
		return nullptr;
	}
	const auto* leech = Rel32Target(handler + CoreLeechHandlerCallOffset, 1, 5);
	if (!CoreMatches(leech, CoreLeechEntryWindow, CoreLeechEntryGaps)
		|| !CoreMatches(leech + CoreLeechLifeStatOffset, CoreLeechLifeStatWindow)) {
		return nullptr;
	}
	return leech;
}

auto LocateCoreItemStatApply(std::uintptr_t imageBase) noexcept -> const std::uint8_t* {
	const auto* stub = CoreExport("ReceiveWideItemStatPacket");
	if (stub == nullptr || !CoreMatches(stub, CoreItemStatStub, CoreItemStatStubGaps)) {
		return nullptr;
	}
	const auto* apply = Rel32Target(stub + CoreItemStatCallOffset, 1, 5);
	if (!CoreMatches(apply + CoreItemStatIdOffset, CoreItemStatIdWindow)
		|| !CoreMatches(apply + CoreItemStatSiteWindowOffset, CoreItemStatSiteWindow, CoreItemStatSiteGaps)) {
		return nullptr;
	}
	// The refresh call reads its target out of D2RCore's resolved game-function
	// table. It must be the game's own gear refresh, or skipping it means nothing.
	const auto* slot = Rel32Target(apply + CoreGearRefreshCallOffset, 2, 6);
	if (!IsCoreMemory(slot, sizeof(std::uint64_t), false)) {
		return nullptr;
	}
	std::uint64_t refresh = 0;
	std::memcpy(&refresh, slot, sizeof(refresh));
	if (refresh != imageBase + GearRefreshRva) {
		return nullptr;
	}
	return apply;
}

auto PrepareCorePage(const D2RL::PluginContext* context, const std::uint8_t* nearAddress) noexcept -> bool {
	if (CorePage != nullptr) {
		return true;
	}
	if (CorePageRefused) {
		return false;
	}
	CorePageRefused = true;

	void* page = AllocateNear(reinterpret_cast<std::uintptr_t>(nearAddress), RelayPageBytes);
	if (page == nullptr) {
		context->LogError("No relay page could be allocated within rel32 reach of D2RCore.dll.");
		return false;
	}
	std::memset(page, 0xCC, RelayPageBytes);
	DWORD previous = 0;
	if (!VirtualProtect(page, RelayPageBytes, PAGE_EXECUTE_READ, &previous)) {
		context->LogError("The D2RCore relay page could not be made executable.");
		VirtualFree(page, 0, MEM_RELEASE);
		return false;
	}
	FlushInstructionCache(GetCurrentProcess(), page, RelayPageBytes);

	CorePage        = page;
	CorePageRefused = false;
	return true;
}

// Only ever called while the plugin loads, before anything can run the page.
auto WriteCorePage(std::size_t offset, const std::uint8_t* bytes, std::size_t size) noexcept -> bool {
	if (CorePage == nullptr || offset + size > RelayPageBytes) {
		return false;
	}
	DWORD previous = 0;
	if (!VirtualProtect(CorePage, RelayPageBytes, PAGE_READWRITE, &previous)) {
		return false;
	}
	std::memcpy(static_cast<std::uint8_t*>(CorePage) + offset, bytes, size);
	DWORD ignored = 0;
	const bool executable = VirtualProtect(CorePage, RelayPageBytes, PAGE_EXECUTE_READ, &ignored) != 0;
	FlushInstructionCache(GetCurrentProcess(), CorePage, RelayPageBytes);
	return executable;
}

auto WriteCoreCode(std::uint8_t* at, const std::uint8_t* bytes, std::size_t size) noexcept -> bool {
	DWORD previous = 0;
	if (!VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &previous)) {
		return false;
	}
	std::memcpy(at, bytes, size);
	DWORD ignored = 0;
	VirtualProtect(at, size, previous, &ignored);
	FlushInstructionCache(GetCurrentProcess(), at, size);
	return true;
}

auto EncodeRel32Jump(std::uint8_t* written, const std::uint8_t* site, std::uintptr_t target) noexcept -> bool {
	const auto next = reinterpret_cast<std::uintptr_t>(site) + 5;
	if (!WithinRel32(next, target)) {
		return false;
	}
	const auto displacement = static_cast<std::int32_t>(static_cast<std::int64_t>(target) - static_cast<std::int64_t>(next));
	std::memcpy(written + 1, &displacement, sizeof(displacement));
	return true;
}

auto InstallEventRecursionGuard(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr) {
		return false;
	}
	if (!Config.pluginEnabled || !Config.eventHandlerRecursion) {
		SetGuardState(Guard::EventHandlerRecursion, GuardState::DisabledByConfig);
		D2RL::LogInfoF(context, "%s not installed: turned off in the config file.", EventRecursionGuardLabel);
		return false;
	}

	const auto* loop = LocateCoreDispatchLoop();
	if (loop == nullptr) {
		SetGuardState(Guard::EventHandlerRecursion, GuardState::UnsupportedBuild);
		D2RL::LogErrorF(
			context,
			"%s NOT installed: D2RCore.dll's DispatchWideEffects or its dispatch loop does not match the verified "
			"D2RLoader 1.3.0 bytes, or another patch already owns the site. Nothing was patched.",
			EventRecursionGuardLabel);
		return false;
	}

	auto* site = const_cast<std::uint8_t*>(loop + CoreRecursionSiteOffset);
	if (!PrepareCorePage(context, site)) {
		SetGuardState(Guard::EventHandlerRecursion, GuardState::InstallFailed);
		D2RL::LogErrorF(context, "%s NOT installed: no D2RCore relay page.", EventRecursionGuardLabel);
		return false;
	}

	std::array<std::uint8_t, sizeof(CoreRecursionStub)> stub {};
	std::memcpy(stub.data(), CoreRecursionStub, sizeof(CoreRecursionStub));
	const auto runHandler = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(loop + CoreRecursionRunOffset));
	const auto nextNode   = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(loop + CoreRecursionNextOffset));
	std::memcpy(stub.data() + CoreRecursionRunTargetOffset, &runHandler, sizeof(runHandler));
	std::memcpy(stub.data() + CoreRecursionNextTargetOffset, &nextNode, sizeof(nextNode));

	std::memcpy(CoreRecursionWritten.data(), CoreRecursionSiteTemplate, sizeof(CoreRecursionSiteTemplate));
	const auto target = reinterpret_cast<std::uintptr_t>(CorePage) + CoreRecursionStubOffset;
	if (!EncodeRel32Jump(CoreRecursionWritten.data(), site, target)
		|| !WriteCorePage(CoreRecursionStubOffset, stub.data(), stub.size())
		|| !WriteCoreCode(site, CoreRecursionWritten.data(), CoreRecursionWritten.size())) {
		SetGuardState(Guard::EventHandlerRecursion, GuardState::InstallFailed);
		D2RL::LogErrorF(context, "%s NOT installed: the D2RCore site at RVA %llX could not be written.", EventRecursionGuardLabel, CoreRva(site));
		return false;
	}

	CoreRecursionSite = site;
	SetGuardState(Guard::EventHandlerRecursion, GuardState::Installed);
	D2RL::LogInfoF(context, "%s installed at D2RCore.dll RVA %llX.", EventRecursionGuardLabel, CoreRva(site));
	return true;
}

auto InstallLifestealWhileDeadGuard(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr) {
		return false;
	}
	if (!Config.pluginEnabled || !Config.lifestealWhileDead) {
		SetGuardState(Guard::LifeDrainWhileDead, GuardState::DisabledByConfig);
		D2RL::LogInfoF(context, "%s not installed: turned off in the config file.", LifestealGuardLabel);
		return false;
	}

	const auto* leech = LocateCoreLeechRoutine();
	if (leech == nullptr) {
		SetGuardState(Guard::LifeDrainWhileDead, GuardState::UnsupportedBuild);
		D2RL::LogErrorF(
			context,
			"%s NOT installed: D2RCore.dll's RegisterWideSkillEffect, its handler 28 or the leech routine does not "
			"match the verified D2RLoader 1.3.0 bytes, or another patch already owns the routine. Nothing was patched.",
			LifestealGuardLabel);
		return false;
	}

	auto* entry = const_cast<std::uint8_t*>(leech);
	if (!PrepareCorePage(context, entry)) {
		SetGuardState(Guard::LifeDrainWhileDead, GuardState::InstallFailed);
		D2RL::LogErrorF(context, "%s NOT installed: no D2RCore relay page.", LifestealGuardLabel);
		return false;
	}

	std::array<std::uint8_t, CoreLeechPatchBytes + AbsoluteJumpBytes> trampoline {};
	std::memcpy(trampoline.data(), CoreLeechEntryWindow, CoreLeechPatchBytes);
	WriteAbsoluteJump(trampoline.data() + CoreLeechPatchBytes, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(entry) + CoreLeechPatchBytes));
	if (!WriteCorePage(CoreLeechTrampolineOffset, trampoline.data(), trampoline.size())) {
		SetGuardState(Guard::LifeDrainWhileDead, GuardState::InstallFailed);
		D2RL::LogErrorF(context, "%s NOT installed: the trampoline could not be written.", LifestealGuardLabel);
		return false;
	}
	OriginalCoreLeech = reinterpret_cast<CoreLeechFn>(reinterpret_cast<std::uintptr_t>(CorePage) + CoreLeechTrampolineOffset);

	WriteAbsoluteJump(CoreLeechWritten.data(), reinterpret_cast<std::uint64_t>(&HookCoreLeech));
	if (!WriteCoreCode(entry, CoreLeechWritten.data(), CoreLeechWritten.size())) {
		SetGuardState(Guard::LifeDrainWhileDead, GuardState::InstallFailed);
		D2RL::LogErrorF(context, "%s NOT installed: the D2RCore routine at RVA %llX could not be written.", LifestealGuardLabel, CoreRva(entry));
		return false;
	}

	CoreLeechEntry = entry;
	SetGuardState(Guard::LifeDrainWhileDead, GuardState::Installed);
	D2RL::LogInfoF(context, "%s installed at D2RCore.dll RVA %llX.", LifestealGuardLabel, CoreRva(entry));
	return true;
}

auto InstallDurabilityGearRefreshFix(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr) {
		return false;
	}
	if (!Config.pluginEnabled || !Config.durabilityTickGearRefresh) {
		SetGuardState(Guard::DurabilityGearRefresh, GuardState::DisabledByConfig);
		D2RL::LogInfoF(context, "%s not installed: turned off in the config file.", DurabilityFixLabel);
		return false;
	}

	const auto* apply = LocateCoreItemStatApply(static_cast<std::uintptr_t>(context->exeBase));
	if (apply == nullptr) {
		SetGuardState(Guard::DurabilityGearRefresh, GuardState::UnsupportedBuild);
		D2RL::LogErrorF(
			context,
			"%s NOT installed: D2RCore.dll's ReceiveWideItemStatPacket or its item stat apply routine does not match "
			"the verified D2RLoader 1.3.0 bytes, or another patch already owns the site. Nothing was patched.",
			DurabilityFixLabel);
		return false;
	}

	auto* site = const_cast<std::uint8_t*>(apply + CoreDurabilitySiteOffset);
	if (!PrepareCorePage(context, site)) {
		SetGuardState(Guard::DurabilityGearRefresh, GuardState::InstallFailed);
		D2RL::LogErrorF(context, "%s NOT installed: no D2RCore relay page.", DurabilityFixLabel);
		return false;
	}

	std::array<std::uint8_t, sizeof(CoreDurabilityStub)> stub {};
	std::memcpy(stub.data(), CoreDurabilityStub, sizeof(CoreDurabilityStub));
	const auto refresh  = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(apply + CoreDurabilityRefreshOffset));
	const auto quantity = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(apply + CoreDurabilityQuantityOffset));
	const auto exit     = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(apply + CoreDurabilityExitOffset));
	std::memcpy(stub.data() + CoreDurabilityRefreshTargetOffset, &refresh, sizeof(refresh));
	std::memcpy(stub.data() + CoreDurabilityQuantityTargetOffset, &quantity, sizeof(quantity));
	std::memcpy(stub.data() + CoreDurabilityExitTargetOffset, &exit, sizeof(exit));

	std::memcpy(CoreDurabilityWritten.data(), CoreDurabilitySiteTemplate, sizeof(CoreDurabilitySiteTemplate));
	const auto target = reinterpret_cast<std::uintptr_t>(CorePage) + CoreDurabilityStubOffset;
	if (!EncodeRel32Jump(CoreDurabilityWritten.data(), site, target)
		|| !WriteCorePage(CoreDurabilityStubOffset, stub.data(), stub.size())
		|| !WriteCoreCode(site, CoreDurabilityWritten.data(), CoreDurabilityWritten.size())) {
		SetGuardState(Guard::DurabilityGearRefresh, GuardState::InstallFailed);
		D2RL::LogErrorF(context, "%s NOT installed: the D2RCore site at RVA %llX could not be written.", DurabilityFixLabel, CoreRva(site));
		return false;
	}

	CoreDurabilitySite = site;
	SetGuardState(Guard::DurabilityGearRefresh, GuardState::Installed);
	D2RL::LogInfoF(context, "%s installed at D2RCore.dll RVA %llX.", DurabilityFixLabel, CoreRva(site));
	return true;
}

// The leech entry goes first, so no new call reaches this DLL, then the two
// jumps into the page. A site that no longer holds what the plugin wrote is
// left alone. The page is kept: a thread may be inside it.
void WithdrawCoreSites() noexcept {
	if (CoreLeechEntry != nullptr
		&& std::memcmp(CoreLeechEntry, CoreLeechWritten.data(), CoreLeechWritten.size()) == 0
		&& WriteCoreCode(CoreLeechEntry, CoreLeechEntryWindow, CoreLeechPatchBytes)) {
		CoreLeechEntry = nullptr;
	}
	if (CoreRecursionSite != nullptr
		&& std::memcmp(CoreRecursionSite, CoreRecursionWritten.data(), CoreRecursionWritten.size()) == 0
		&& WriteCoreCode(CoreRecursionSite, CoreRecursionSiteOriginal, sizeof(CoreRecursionSiteOriginal))) {
		CoreRecursionSite = nullptr;
	}
	if (CoreDurabilitySite != nullptr
		&& std::memcmp(CoreDurabilitySite, CoreDurabilityWritten.data(), CoreDurabilityWritten.size()) == 0
		&& WriteCoreCode(CoreDurabilitySite, CoreDurabilitySiteOriginal, sizeof(CoreDurabilitySiteOriginal))) {
		CoreDurabilitySite = nullptr;
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

	for (const InlineGuardSite& site : InlineGuardSites) {
		ReportGuard(context, site.guard, site.label, site.eventNoun);
	}

	ReportGuard(context, Guard::LifeDrainWhileDead, LifestealGuardLabel, "post-death leeches blocked");
	ReportGuard(context, Guard::EventHandlerRecursion, EventRecursionGuardLabel, nullptr);
	ReportGuard(context, Guard::MissileSkillAttackRating, MissileFixLabel, "missiles given skill attack rating");
	ReportGuard(context, Guard::DurabilityGearRefresh, DurabilityFixLabel, nullptr);
	ReportGuard(context, Guard::FrozenOrbBurstOnWall, FrozenOrbFixLabel, nullptr);
	ReportGuard(context, Guard::ZeroSkillDescriptionLine, DesclineFixLabel, nullptr);
	ReportGuard(context, Guard::ResocketStatInflation, ResocketFixLabel, nullptr);
	ReportGuard(context, Guard::BloodManaCastGate, BloodManaFixLabel, nullptr);
	ReportGuard(context, Guard::SkillFreezeAiResume, SkillFreezeFixLabel, "skill-freeze thaws handed to the engine thaw");

	return D2RL::ConsoleCommandResult::Handled;
}

constexpr D2RL::PluginInfo EngineStabilityInfo {
	.infoSize    = D2RL::PluginInfoSize,
	.apiVersion  = D2RL_PLUGIN_API_VERSION,
	.id          = "celestialrayone.engine-stability",
	.name        = "Engine Stability",
	.version     = "0.10.1",
	.author      = "CelestialRayOne",
	.description = "Crash guards and vanilla engine bug fixes for Diablo II: Resurrected.",
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
			"Report which engine stability guards and fixes are active.")) {
		context->LogWarn("The engine-stability console command was not registered.");
	}

	if (const char* build = D2RL::GetBuildVersion(context); build != nullptr) {
		D2RL::LogInfoF(context, "engine-stability loading against build %s.", build);
	}

	LoadConfiguration(context);

	if (!Config.pluginEnabled) {
		for (const InlineGuardSite& site : InlineGuardSites) {
			SetGuardState(site.guard, GuardState::DisabledByConfig);
		}
		SetGuardState(Guard::LifeDrainWhileDead, GuardState::DisabledByConfig);
		SetGuardState(Guard::EventHandlerRecursion, GuardState::DisabledByConfig);
		SetGuardState(Guard::MissileSkillAttackRating, GuardState::DisabledByConfig);
		SetGuardState(Guard::DurabilityGearRefresh, GuardState::DisabledByConfig);
		SetGuardState(Guard::FrozenOrbBurstOnWall, GuardState::DisabledByConfig);
		SetGuardState(Guard::ZeroSkillDescriptionLine, GuardState::DisabledByConfig);
		SetGuardState(Guard::ResocketStatInflation, GuardState::DisabledByConfig);
		SetGuardState(Guard::BloodManaCastGate, GuardState::DisabledByConfig);
		SetGuardState(Guard::SkillFreezeAiResume, GuardState::DisabledByConfig);
		context->LogWarn("engine-stability is disabled in its config file. Nothing was installed.");
		return true;
	}

	// Each guard is installed independently. One refusing, whether because the
	// config turned it off or because its site did not verify, must never take
	// the others down with it.
	unsigned installed = 0;

	for (const InlineGuardSite& site : InlineGuardSites) {
		if (InstallInlineGuard(context, site)) {
			++installed;
		}
	}

	CoreModule = GetModuleHandleW(CoreModuleName);
	if (CoreModule == nullptr) {
		context->LogError("D2RCore.dll is not loaded; the three D2RCore entries cannot be verified and will not install.");
	}

	if (InstallLifestealWhileDeadGuard(context)) {
		++installed;
	}

	if (InstallEventRecursionGuard(context)) {
		++installed;
	}

	if (InstallMissileSkillAttackRatingFix(context)) {
		++installed;
	}

	if (InstallDurabilityGearRefreshFix(context)) {
		++installed;
	}

	if (InstallFrozenOrbBurstOnWallFix(context)) {
		++installed;
	}

	if (InstallZeroSkillDescriptionLineFix(context)) {
		++installed;
	}

	// No longer held back. The applier is 00475140, the storage shape is read
	// out of this build rather than inherited from 2.4, and the gate rows come
	// from the applier's own bytes. See the Fix 5 section for the witnesses.
	if (InstallResocketStatInflationFix(context)) {
		++installed;
	}

	if (InstallBloodManaCastGateFix(context)) {
		++installed;
	}

	if (InstallSkillFreezeAiResumeFix(context)) {
		++installed;
	}

	if (installed == 0) {
		context->LogError("engine-stability loaded with NO guards or fixes active.");
		return true;
	}

	D2RL::LogInfoF(context, "engine-stability loaded with %u of %zu guards and fixes active.", installed, GuardCount);
	return true;
}

// The loader's inline hooks cannot be withdrawn and their trampolines live in
// this module, so the loader is expected to keep the DLL resident for the life
// of the process. The relay sites and the D2RCore sites can be withdrawn, so
// they are, D2RCore first because its leech hook calls into this DLL.
D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
	WithdrawCoreSites();
	WithdrawRelaySites();
}
