// Hex Debuff Installer (auraeventfunc 36, WarApplyHexDebuff).
//
// See celestialrayone.hex-debuff-installer.toml for the full write-up. In short:
//
//   auraeventfunc 36 puts the skill's auratargetstate on the unit it hits. Only when
//   that state is 217 (hexpurgedebuff, hardcoded: `cmp dword [rsp+74h], 0D9h` at
//   51FFB6, `jne 520060` at 51FFC3) does it also register the skill's Param1 as an
//   event function on that unit for damagedbymissile, damagedinmelee and hextrigger.
//   The registrations carry no installer: sub_140438470 passes a literal 0 to the
//   single-event registrar sub_140438230 (`mov qword [rsp+48h], 0` at 438504), so
//   every node stores installer type 6, id -1.
//
//   auraeventfunc 33 (SkillActivateSubskill) tests aurastatcalc4 by whether the column
//   holds a formula (`cmp dword [rsi+94h], 0` at 42F3A9), not by its value. When it
//   does, the caster is looked up from the node's installer (sub_14048FE80). Type 6
//   finds nothing and the function returns at 42F3CE, before the chance roll and
//   before the cast. So Param1 = 33 on a hex can never cast anything.
//
//   This plugin makes two independent changes:
//
//   1. ANY TARGET STATE
//      NOPs the `jne` at 51FFC3, so Param1 is registered for every auratargetstate,
//      not only hexpurgedebuff.
//
//   2. RECORD INSTALLER
//      Gives the registrar the hex caster as the installer for the registrations
//      auraeventfunc 36 makes. The engine then stores the caster's unit type and id
//      on each node through its own code path.
//
// D2RLoader 1.3.0 (re-checked against its D2RLoader.exe image and D2RCore.dll):
//
//   The registrar at 438230 is now a loader thunk to D2RCore!RegisterWideSkillEffect
//   (jmp [rip+disp32] and four nops over its first ten bytes), so it can no longer be
//   inline hooked. D2RCore keeps the same ten arguments and the same installer
//   handling: with a unit it stores the unit's type (+0) and id (+8) on the node, with
//   null it stores type 6, id -1.
//
//   Event handlers now come from D2RCore's own table, but its entries for 33 and 36
//   are forwarders that read the game's auraeventfunc table at every call, so
//   auraeventfunc 36 still runs the native function at 51FDA0 and its inline hook
//   still applies. auraeventfunc 33 still runs the native 42F300.
//
//   auraeventfunc 36 installs Param1 through sub_140438470, whose only registrar call
//   is at 438536. Instead of hooking the registrar, the plugin retargets that one
//   call to a near relay into HookRegisterEvent, which calls the registrar thunk
//   with the same arguments. Read from the live 1.3.0 image:
//
//     4384FA  mov  ecx, [rsp+0B0h]        state
//     438501  mov  r9d, r13d              skill id
//     438504  mov  qword [rsp+48h], 0     installer: none
//     43850D  mov  rdx, r14               unit
//     438510  jmp  loader stub            + 6 nops, replaces two stock instructions
//                                         stub: mov  [rsp+40h], ecx        state
//                                               mov  dword [rsp+44h], 0    upper half
//                                               mov  ecx, [rsp+0A8h]       group
//                                               jmp  43851B
//     43851B  mov  [rsp+38h], ecx         group
//     43851F  mov  rcx, r15               game
//     438522  mov  [rsp+30h], eax         auraeventfunc index
//     43852D  mov  [rsp+28h], eax         param
//     438531  mov  [rsp+20h], r12d        skill level
//     438536  call 438230                 <- retargeted
//
//   The stub exists because D2RCore reads two arguments at full 64-bit width: the
//   skill id (r9, stored as a qword; its event handler forwarders refuse to run a
//   node whose skill id has a non-zero upper half) and the state (the ninth
//   argument, stored as a qword and compared as a qword by UnregisterWideEffects when
//   the effects are removed). The hook therefore takes and forwards both at full
//   width, exactly as the caller built them.
//
//   r8d (event id) is loaded just before 4384FA. auraeventfunc 36
//   reaches the registrar only through that helper (`call 438470` at 52005B), and
//   438536 is the helper's only registrar call. The registrar's other callers (42FB5D,
//   43843A, 43861C) are not redirected.
//
// Built against D2R 3.3 (module 140000000.D2RLoader.exe) for D2RLoader 1.3.0 and
// PluginSDK v4.

#include <D2RLPlugin/api.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

// ---------------------------------------------------------------------------
// RVAs, all read out of the 3.3 image
// ---------------------------------------------------------------------------

// auraeventfunc 36, WarApplyHexDebuff. Table off_14238E5C0, slot 36.
constexpr std::uint64_t HexDebuffRva = 0x0051FDA0;

// Single-event registrar. Its tenth argument is the installer unit; with a unit it
// stores that unit's type and id on the node, with null it leaves type 6, id -1.
// In D2RLoader 1.3.0 a thunk to D2RCore!RegisterWideSkillEffect.
constexpr std::uint64_t RegisterEventRva = 0x00438230;

// Thunk shape: FF 25 disp32 at 438230, then four nops and the untouched tail of the
// old prologue (mov [rsp+18h], rdi / push r14). disp32 locates the loader's import
// slot, moves between loader builds and is not pinned.
constexpr std::uint8_t ExpectedRegisterThunkJump[] { 0xFF, 0x25 };
constexpr std::uint64_t RegisterThunkTailRva = RegisterEventRva + 6;
constexpr std::uint8_t ExpectedRegisterThunkTail[] {
	0x90, 0x90, 0x90, 0x90, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x41, 0x56,
};

// sub_140438470's registrar call and its argument setup, in the live 1.3.0 image.
// The rel32 of the loader's jmp at 438510 moves between loader builds, so it is
// the one gap: 13 bytes at 438504 up to and including the jmp opcode, then 38
// bytes from the six nops at 438515 through `call 438230` (E8 F5 FC FF FF) at
// 438536.
constexpr std::uint64_t InstallSetupRva      = 0x00438504;
constexpr std::uint64_t LoaderJumpRva        = 0x00438510;
constexpr std::uint64_t InstallCallWindowRva = 0x00438515;
constexpr std::uint64_t InstallCallRva       = 0x00438536;
constexpr std::uint8_t ExpectedInstallSetup[] {
	0x48, 0xC7, 0x44, 0x24, 0x48, 0x00, 0x00, 0x00, 0x00, 0x49, 0x8B, 0xD6, 0xE9,
};
constexpr std::uint8_t ExpectedInstallCallWindow[] {
	0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x89, 0x4C, 0x24, 0x38, 0x49, 0x8B, 0xCF, 0x89,
	0x44, 0x24, 0x30, 0x8B, 0x84, 0x24, 0xA0, 0x00, 0x00, 0x00, 0x89, 0x44, 0x24, 0x28,
	0x44, 0x89, 0x64, 0x24, 0x20, 0xE8, 0xF5, 0xFC, 0xFF, 0xFF,
};

// The loader stub the jmp at 438510 enters, 20 bytes up to its jmp back:
// mov [rsp+40h], ecx / mov dword [rsp+44h], 0 / mov ecx, [rsp+0A8h] / jmp.
constexpr std::uint8_t ExpectedLoaderStub[] {
	0x89, 0x4C, 0x24, 0x40, 0xC7, 0x44, 0x24, 0x44, 0x00, 0x00, 0x00, 0x00, 0x8B, 0x8C,
	0x24, 0xA8, 0x00, 0x00, 0x00, 0xE9,
};
constexpr std::uint32_t CallBytes = 5;

// One 14-byte absolute jump: FF 25 00 00 00 00 <abs64>.
constexpr std::size_t RelayPageBytes   = 4'096;
constexpr std::size_t JumpTargetOffset = 6;

// Prologue of auraeventfunc 36. Sixteen bytes, no rip-relative operands, ends on an
// instruction boundary (the next instruction is `sub rsp, 0A0h` at 51FDB0).
//   48 89 5C 24 10 mov [rsp+10h], rbx / 55 push rbp / 56 push rsi / 57 push rdi
//   41 54 push r12 / 41 55 push r13 / 41 56 push r14 / 41 57 push r15
constexpr std::uint8_t ExpectedHexDebuffPrologue[] {
	0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
};

// The hexpurgedebuff gate: cmp dword [rsp+74h], 0D9h / mov ebp, 1 / jne 520060.
// Checked in full before anything is written, so the NOP can only ever land on this
// exact sequence. `mov ebp, 1` is kept: it is the function's return value.
constexpr std::uint64_t HexGateRva    = 0x0051FFB6;
constexpr std::uint64_t HexGateJneRva = 0x0051FFC3;

constexpr std::uint8_t ExpectedHexGate[] {
	0x81, 0x7C, 0x24, 0x74, 0xD9, 0x00, 0x00, 0x00,  // cmp dword [rsp+74h], 0D9h
	0xBD, 0x01, 0x00, 0x00, 0x00,                    // mov ebp, 1
	0x0F, 0x85, 0x97, 0x00, 0x00, 0x00,              // jne 520060
};
constexpr std::uint8_t ExpectedHexGateJne[] { 0x0F, 0x85, 0x97, 0x00, 0x00, 0x00 };

// Group argument auraeventfunc 36 passes to the registrar (`mov [rsp+28h], ebp` with
// ebp = 1 at 520053). The skill id it passes is its own sixth argument
// (`mov r14d, [rsp+108h]` at 51FE21, then `mov r8d, r14d` at 520038).
constexpr std::int32_t HexInstallGroup = 1;

// ---------------------------------------------------------------------------
// Game function types
// ---------------------------------------------------------------------------

// The nine arguments the event dispatcher (sub_1405881E0) passes to every
// auraeventfunc: game, event id, the unit that owns the event node (for a hex, the
// hex caster), the other unit, damage, skill id, skill level, node param and a
// pointer to the node's installer type/id pair.
using HexDebuffFn = std::int64_t(__fastcall*)(void*        game,
                                               std::uint32_t eventId,
                                               void*        unit,
                                               void*        other,
                                               void*        damage,
                                               std::int32_t skillId,
                                               std::int32_t skillLevel,
                                               std::int32_t param,
                                               void*        installerRef) noexcept;

// game, the unit that receives the event, event id, skill id, skill level, node
// param, auraeventfunc index, group, state, installer unit. Skill id and state are
// full 64-bit slots: D2RCore stores both as qwords.
using RegisterEventFn = std::int64_t(__fastcall*)(void*         game,
                                                   void*         unit,
                                                   std::int32_t  eventId,
                                                   std::uint64_t skillId,
                                                   std::int32_t  skillLevel,
                                                   std::int32_t  param,
                                                   std::uint32_t eventFunc,
                                                   std::int32_t  group,
                                                   std::uint64_t state,
                                                   void*         installer) noexcept;

HexDebuffFn     OriginalHexDebuff     = nullptr;
RegisterEventFn OriginalRegisterEvent = nullptr;

const D2RL::PluginContext* Context      = nullptr;
std::uintptr_t             ImageBase    = 0;
void*                      RelayPage    = nullptr;
bool                       CallPatched  = false;

// The hex caster and hex skill while auraeventfunc 36 runs on this thread.
struct ActiveHex {
	void*        caster  = nullptr;
	std::int32_t skillId = -1;
};

thread_local ActiveHex CurrentHex {};

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

struct Settings {
	bool anyTargetState  = true;
	bool recordInstaller = true;
};

constexpr auto ByteSize(std::size_t size) noexcept -> std::uint32_t {
	return static_cast<std::uint32_t>(size);
}

template <std::size_t Size>
constexpr auto ByteCount(const std::uint8_t (&)[Size]) noexcept -> std::uint32_t {
	return ByteSize(Size);
}

constexpr auto IsBlank(char c) noexcept -> bool {
	return c == ' ' || c == '\t' || c == '\r';
}

// Finds `key = value` at the start of a line. The config is a single table, so
// section headers and comments are simply skipped rather than tracked.
auto FindConfigValue(const char* text, const char* key, const char*& value, std::size_t& length) noexcept -> bool {
	if (text == nullptr || key == nullptr) {
		return false;
	}

	const std::size_t keyLength = std::strlen(key);

	for (const char* line = text; *line != '\0';) {
		const char* cursor = line;
		while (IsBlank(*cursor)) {
			++cursor;
		}

		if (std::strncmp(cursor, key, keyLength) == 0) {
			const char* after = cursor + keyLength;
			while (IsBlank(*after)) {
				++after;
			}

			if (*after == '=') {
				++after;
				while (IsBlank(*after)) {
					++after;
				}

				const char* end = after;
				while (*end != '\0' && *end != '\n' && *end != '#') {
					++end;
				}
				while (end > after && IsBlank(end[-1])) {
					--end;
				}

				value  = after;
				length = static_cast<std::size_t>(end - after);
				return true;
			}
		}

		while (*line != '\0' && *line != '\n') {
			++line;
		}
		if (*line == '\n') {
			++line;
		}
	}

	return false;
}

auto ReadBool(const char* text, const char* key, bool fallback) noexcept -> bool {
	const char* value  = nullptr;
	std::size_t length = 0;
	if (!FindConfigValue(text, key, value, length)) {
		return fallback;
	}

	if (length == 4 && std::strncmp(value, "true", 4) == 0) {
		return true;
	}
	if (length == 5 && std::strncmp(value, "false", 5) == 0) {
		return false;
	}
	return fallback;
}

auto LoadSettings(const D2RL::PluginContext* context) noexcept -> Settings {
	Settings settings {};

	std::array<char, 16'384> buffer {};
	if (!context->ReadConfig(buffer.data(), ByteSize(buffer.size()))) {
		context->LogWarn("Could not read celestialrayone.hex-debuff-installer.toml; using defaults.");
		return settings;
	}
	buffer.back() = '\0';

	settings.anyTargetState  = ReadBool(buffer.data(), "any_target_state", settings.anyTargetState);
	settings.recordInstaller = ReadBool(buffer.data(), "record_installer", settings.recordInstaller);
	return settings;
}

// ---------------------------------------------------------------------------
// The hooks
// ---------------------------------------------------------------------------

auto __fastcall HookHexDebuff(void*         game,
                              std::uint32_t eventId,
                              void*         unit,
                              void*         other,
                              void*         damage,
                              std::int32_t  skillId,
                              std::int32_t  skillLevel,
                              std::int32_t  param,
                              void*         installerRef) noexcept -> std::int64_t {
	const HexDebuffFn original = OriginalHexDebuff;
	if (original == nullptr) {
		return 0;
	}

	// Saved and restored rather than cleared, so a nested call can never leave the
	// outer one without its caster.
	const ActiveHex previous = CurrentHex;
	CurrentHex               = ActiveHex { .caster = unit, .skillId = skillId };

	const std::int64_t result = original(game, eventId, unit, other, damage, skillId, skillLevel, param, installerRef);

	CurrentHex = previous;
	return result;
}

auto __fastcall HookRegisterEvent(void*         game,
                                  void*         unit,
                                  std::int32_t  eventId,
                                  std::uint64_t skillId,
                                  std::int32_t  skillLevel,
                                  std::int32_t  param,
                                  std::uint32_t eventFunc,
                                  std::int32_t  group,
                                  std::uint64_t state,
                                  void*         installer) noexcept -> std::int64_t {
	const RegisterEventFn original = OriginalRegisterEvent;
	if (original == nullptr) {
		return 0;
	}

	// Only the registrations auraeventfunc 36 itself makes: no installer, group 1 and
	// the hex skill's own id. Any other registration, including one made by something
	// nested inside auraeventfunc 36 for a different skill, passes through untouched.
	const ActiveHex hex = CurrentHex;
	if (hex.caster != nullptr && installer == nullptr && group == HexInstallGroup
	    && skillId == static_cast<std::uint32_t>(hex.skillId)) {
		installer = hex.caster;
	}

	return original(game, unit, eventId, skillId, skillLevel, param, eventFunc, group, state, installer);
}

auto AllocateNear(std::uintptr_t hint, std::size_t size) noexcept -> void* {
	SYSTEM_INFO systemInfo {};
	GetSystemInfo(&systemInfo);
	const auto granularity = static_cast<std::uintptr_t>(systemInfo.dwAllocationGranularity);
	const auto aligned     = hint & ~(granularity - 1U);
	for (std::uintptr_t delta = granularity; delta < 0x7000'0000ULL; delta += granularity) {
		const auto candidate = aligned + delta;
		const std::int64_t reach =
			static_cast<std::int64_t>(candidate + size) - static_cast<std::int64_t>(hint + CallBytes);
		if (reach > INT32_MAX) {
			break;
		}
		if (auto* memory = VirtualAlloc(reinterpret_cast<void*>(candidate), size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) {
			return memory;
		}
	}
	return nullptr;
}

// Follows the loader's jmp at 438510 and compares its stub. The stub lives in loader
// memory, so it is read only after VirtualQuery shows it committed and executable.
auto LoaderStubMatches() noexcept -> bool {
	std::int32_t displacement = 0;
	std::memcpy(&displacement, reinterpret_cast<const std::uint8_t*>(ImageBase + LoaderJumpRva + 1), sizeof(displacement));
	const auto* stub = reinterpret_cast<const std::uint8_t*>(
		static_cast<std::intptr_t>(ImageBase + LoaderJumpRva + CallBytes) + displacement);

	MEMORY_BASIC_INFORMATION info {};
	if (VirtualQuery(stub, &info, sizeof(info)) != sizeof(info) || info.State != MEM_COMMIT) {
		return false;
	}
	const DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
	if ((info.Protect & executable) == 0 || (info.Protect & PAGE_GUARD) != 0) {
		return false;
	}
	const auto* regionEnd = static_cast<const std::uint8_t*>(info.BaseAddress) + info.RegionSize;
	if (stub + sizeof(ExpectedLoaderStub) > regionEnd) {
		return false;
	}
	return std::memcmp(stub, ExpectedLoaderStub, sizeof(ExpectedLoaderStub)) == 0;
}

auto EncodeCall(std::uintptr_t from, std::uintptr_t to) noexcept -> std::array<std::uint8_t, CallBytes> {
	std::array<std::uint8_t, CallBytes> bytes { 0xE8 };
	const auto displacement =
		static_cast<std::int32_t>(static_cast<std::int64_t>(to) - static_cast<std::int64_t>(from + CallBytes));
	std::memcpy(bytes.data() + 1, &displacement, sizeof(displacement));
	return bytes;
}

// Points the relay at the registrar thunk, so a call already on its way through the
// relay never lands in an unloaded DLL.
auto RetargetRelayToRegistrar() noexcept -> bool {
	if (RelayPage == nullptr) {
		return true;
	}
	auto* page     = static_cast<std::uint8_t*>(RelayPage);
	DWORD previous = 0;
	if (!VirtualProtect(page, RelayPageBytes, PAGE_EXECUTE_READWRITE, &previous)) {
		return false;
	}
	const std::uint64_t registrar = ImageBase + RegisterEventRva;
	std::memcpy(page + JumpTargetOffset, &registrar, sizeof(registrar));
	DWORD ignored = 0;
	VirtualProtect(page, RelayPageBytes, previous, &ignored);
	FlushInstructionCache(GetCurrentProcess(), page, RelayPageBytes);
	return true;
}

auto RestoreInstallCall() noexcept -> bool {
	if (!CallPatched) {
		return true;
	}
	const auto current      = EncodeCall(ImageBase + InstallCallRva, reinterpret_cast<std::uintptr_t>(RelayPage));
	const auto* original    = ExpectedInstallCallWindow + (InstallCallRva - InstallCallWindowRva);
	if (!Context->PatchBytes(InstallCallRva, current.data(), CallBytes, original, CallBytes)) {
		return false;
	}
	CallPatched = false;
	return true;
}

// Retargets the registrar call in sub_140438470 to a relay into HookRegisterEvent.
auto InstallRegistrarCall(const D2RL::PluginContext* context) noexcept -> bool {
	if (!context->CheckExpectedBytes(RegisterEventRva, ExpectedRegisterThunkJump, ByteCount(ExpectedRegisterThunkJump))
	    || !context->CheckExpectedBytes(RegisterThunkTailRva, ExpectedRegisterThunkTail, ByteCount(ExpectedRegisterThunkTail))) {
		context->LogError("0x438230 is not the D2RLoader 1.3.0 thunk to D2RCore RegisterWideSkillEffect; "
		                  "record_installer not applied.");
		return false;
	}
	if (!context->CheckExpectedBytes(InstallSetupRva, ExpectedInstallSetup, ByteCount(ExpectedInstallSetup))
	    || !context->CheckExpectedBytes(InstallCallWindowRva, ExpectedInstallCallWindow, ByteCount(ExpectedInstallCallWindow))) {
		context->LogError("The registrar call at 0x438536 or its argument setup does not match the D2RLoader "
		                  "1.3.0 image, or another plugin already owns it; record_installer not applied.");
		return false;
	}
	if (!LoaderStubMatches()) {
		context->LogError("The loader stub entered from 0x438510 no longer stores the state and group the "
		                  "way this plugin expects; record_installer not applied.");
		return false;
	}

	OriginalRegisterEvent = reinterpret_cast<RegisterEventFn>(ImageBase + RegisterEventRva);

	RelayPage = AllocateNear(ImageBase + InstallCallRva, RelayPageBytes);
	if (RelayPage == nullptr) {
		context->LogError("No relay page within rel32 reach of 0x438536; record_installer not applied.");
		return false;
	}
	auto* page = static_cast<std::uint8_t*>(RelayPage);
	std::memset(page, 0xCC, RelayPageBytes);
	static constexpr std::uint8_t JumpQwordRip[] { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
	std::memcpy(page, JumpQwordRip, sizeof(JumpQwordRip));
	const auto hook = reinterpret_cast<std::uint64_t>(&HookRegisterEvent);
	std::memcpy(page + JumpTargetOffset, &hook, sizeof(hook));
	DWORD previous = 0;
	if (!VirtualProtect(page, RelayPageBytes, PAGE_EXECUTE_READ, &previous)) {
		context->LogError("Relay page protection could not be finalized; record_installer not applied.");
		VirtualFree(RelayPage, 0, MEM_RELEASE);
		RelayPage = nullptr;
		return false;
	}
	FlushInstructionCache(GetCurrentProcess(), page, RelayPageBytes);

	const auto* original = ExpectedInstallCallWindow + (InstallCallRva - InstallCallWindowRva);
	if (!context->PatchCallRel32(InstallCallRva, original, CallBytes,
	                             reinterpret_cast<std::uintptr_t>(RelayPage) - ImageBase, CallBytes)) {
		context->LogError("The registrar call at 0x438536 could not be redirected; record_installer not applied.");
		VirtualFree(RelayPage, 0, MEM_RELEASE);
		RelayPage = nullptr;
		return false;
	}
	CallPatched = true;
	return true;
}

auto InstallHooks(const D2RL::PluginContext* context) noexcept -> bool {
	// Registrar call first: on its own it does nothing, because the caster is only
	// ever set by the auraeventfunc 36 hook.
	if (!InstallRegistrarCall(context)) {
		return false;
	}

	if (!context->InstallInlineHook(HexDebuffRva,
	                                ExpectedHexDebuffPrologue,
	                                ByteCount(ExpectedHexDebuffPrologue),
	                                HookHexDebuff,
	                                &OriginalHexDebuff)) {
		context->LogError("Inline hook at 0x51FDA0 (auraeventfunc 36) failed; wrong game build, "
		                  "or the prologue does not match.");
		const bool retargeted = RetargetRelayToRegistrar();
		if (RestoreInstallCall()) {
			VirtualFree(RelayPage, 0, MEM_RELEASE);
			RelayPage = nullptr;
		} else if (!retargeted) {
			context->LogError("The registrar call could not be restored and its relay could not be "
			                  "retargeted; the relay is kept so the call stays valid.");
		}
		return false;
	}

	context->LogInfo("auraeventfunc 36 hooked and the registrar call at 0x438536 redirected; hex event "
	                 "nodes now record the hex caster as installer.");
	return true;
}

auto ApplyAnyTargetState(const D2RL::PluginContext* context) noexcept -> bool {
	if (!context->CheckExpectedBytes(HexGateRva, ExpectedHexGate, ByteCount(ExpectedHexGate))) {
		context->LogError("The site at 0x51FFB6 is not the hexpurgedebuff gate; wrong game build, or "
		                  "something else already owns it. Leaving the gate in place.");
		return false;
	}

	if (!context->PatchNop(HexGateJneRva, ExpectedHexGateJne, ByteCount(ExpectedHexGateJne), ByteCount(ExpectedHexGateJne))) {
		context->LogError("Failed to NOP the hexpurgedebuff gate at 0x51FFC3.");
		return false;
	}

	context->LogInfo("hexpurgedebuff gate removed; auraeventfunc 36 registers Param1 for every auratargetstate.");
	return true;
}

constexpr D2RL::PluginInfo HexDebuffInstallerInfo {
	.infoSize    = D2RL::PluginInfoSize,
	.apiVersion  = D2RL_PLUGIN_API_VERSION,
	.id          = "celestialrayone.hex-debuff-installer",
	.name        = "Hex Debuff Installer",
	.version     = "1.2.1",
	.author      = "CelestialRayOne",
	.description = "auraeventfunc 36 registers Param1 on the hexed unit for every auratargetstate, "
	               "and those event nodes record the hex caster as installer, so auraeventfunc 33 "
	               "can cast a real skill from any hex.",
	.flags       = D2RL::PluginFlags::Server | D2RL::PluginFlags::NativeHooks,
};

}  // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
	return &HexDebuffInstallerInfo;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr) {
		return false;
	}
	Context   = context;
	ImageBase = context->exeBase;

	const Settings settings = LoadSettings(context);

	if (!settings.anyTargetState && !settings.recordInstaller) {
		context->LogInfo("any_target_state and record_installer are both false; nothing to do.");
		return true;
	}

	bool gate = true;
	if (settings.anyTargetState) {
		gate = ApplyAnyTargetState(context);
	} else {
		context->LogInfo("any_target_state is false; the hexpurgedebuff gate is left in place.");
	}

	bool installer = true;
	if (settings.recordInstaller) {
		installer = InstallHooks(context);
	} else {
		context->LogInfo("record_installer is false; auraeventfunc 36 and the event registrar are left unhooked.");
	}

	// The two changes are independent, so only a total failure unloads the plugin.
	if (!gate && !installer) {
		context->LogError("Neither change could be applied; unloading.");
		return false;
	}

	return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
	if (Context == nullptr || RelayPage == nullptr) {
		return;
	}
	RetargetRelayToRegistrar();
	RestoreInstallCall();
	// The relay page is kept: a thread may be inside a jump through it right now.
}
