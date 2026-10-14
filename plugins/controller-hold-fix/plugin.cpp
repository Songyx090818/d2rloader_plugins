// Controller skill hold fix - D2RLoader plugin for Diablo II: Resurrected 3.3
//
// With a controller, a held skill button keeps casting its skill until the
// game sees the release. Each skill button's release handler (button 1 is
// 0011A9E0) drops the release when, at that moment, the game is being left
// (0008D3D0), the player is dead or missing (00099B60), 000CE500(3) is set,
// the repeat option is off (000E1C60 / 00D2E490), or the skill on that button
// takes the toggle path (00117D20 / 0014A970). The dispatch drops it while a
// panel is open (000CC1E0, 00122F30) and while the controller's skill buttons
// are off (manager+1948h, 00145680). A dropped release leaves the hold set:
// the press/release matcher 00149550 ignores every other press while a
// button is held (+1968h), and ControllerManager::UpdateSkillHold 0014C210
// recasts the held skill every frame, firing its rumble each time.
//
// ControllerManager::Update 0014B720 cancels every hold each frame while a
// panel is open, 000CE500(3) is set, or (mouse mode only) the option is off
// (0014B800..0014B887). This plugin makes that same native cancel also run
// for every other condition that drops a release, by extending its first
// test (the call at 0014B800). A hold therefore never outlives the chance to
// release it.
//
// Every address is an RVA into the D2RLoader 1.3.x process image of D2R 3.3.

#include <D2RLPlugin/api.h>

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

struct Witness {
	std::uint64_t       rva;
	const std::uint8_t* bytes;
	std::uint32_t       size;
	const char*         what;
};

// 0014B7D9..0014B800 and 0014B805..0014B89D, ControllerManager::Update
// 0014B720: the loop over the 8 player slots (edi = slot, rbx = manager) and
// the per-frame cancel. Each iteration first calls 000CC1E0 (panel open) at
// 0014B800, the redirected site, which the patch checks; a true result, like
// 000CE500(3) or (mouse mode) the repeat option being off, jumps to the
// cancel at 0014B831: +1998h = 0, +1968h = -1 (held button and active hold),
// +1974h = -1, and the two 20-byte queue entries at +19DCh. The prologue is
// left out (yin's StatGoldPickup checks or hooks it), and so is the call at
// 0014B89D (the Whirlwind plugin redirects it).
constexpr std::uint8_t W_CancelLoop_14B7D9[] = {0xE8,0x22,0xFB,0xF3,0xFF,0x8B,0xE8,0x2B,0xAB,0x10,0x1F,0x00,0x00,0xE8,0x15,0xFB,0xF3,0xFF,0x45,0x33,0xE4,0x89,0x83,0x10,0x1F,0x00,0x00,0x41,0x8B,0xFC,0x41,0x8B,0xF4,0x66,0x0F,0x1F,0x44,0x00,0x00};
constexpr std::uint8_t W_CancelLoop_14B805[] = {0x84,0xC0,0x75,0x28,0xB9,0x03,0x00,0x00,0x00,0xE8,0xED,0x2C,0xF8,0xFF,0x84,0xC0,0x75,0x1A,0xE8,0xF4,0xC5,0xF2,0xFF,0x84,0xC0,0x75,0x69,0xE8,0x3B,0x64,0xF9,0xFF,0x48,0x8B,0xC8,0xE8,0x63,0x2C,0xBE,0x00,0x85,0xC0,0x75,0x58,0x48,0x69,0xC6,0xB8,0x00,0x00,0x00,0x48,0x89,0x74,0x24,0x50,0x4C,0x89,0xA4,0x18,0x98,0x19,0x00,0x00,0x48,0xC7,0x84,0x18,0x68,0x19,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0x48,0xC7,0x84,0x18,0x74,0x19,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0x48,0x05,0xDC,0x19,0x00,0x00,0x48,0x03,0xC3,0x48,0x8D,0x48,0x28,0x48,0x3B,0xC1,0x74,0x1A,0x90,0x48,0xC7,0x00,0xFF,0xFF,0xFF,0xFF,0x66,0x44,0x89,0x60,0x08,0x4C,0x89,0x60,0x0C,0x48,0x83,0xC0,0x14,0x48,0x3B,0xC1,0x75,0xE7,0x8B,0xCF,0xE8,0xF0,0xEB,0xF4,0xFF,0x48,0x85,0xC0,0x74,0x0D,0x44,0x8B,0xC5,0x8B,0xD7,0x48,0x8B,0xCB};
// 0011A9E0..0011AA4E, the release handler of skill button 1, with every gate
// a release passes before it reaches 00122F30(button, 0).
constexpr std::uint8_t W_ReleaseSlot1_11A9E0[] = {0x48,0x83,0xEC,0x28,0xE8,0xE7,0x29,0xF7,0xFF,0x85,0xC0,0x75,0x6B,0xE8,0xDE,0x08,0xF7,0xFF,0x8B,0xC8,0xE8,0x67,0xF1,0xF7,0xFF,0x84,0xC0,0x75,0x5B,0xB9,0x03,0x00,0x00,0x00,0xE8,0xF9,0x3A,0xFB,0xFF,0x84,0xC0,0x75,0x4D,0xB9,0x01,0x00,0x00,0x00,0x48,0x89,0x5C,0x24,0x20,0xE8,0x06,0xD3,0xFF,0xFF,0x8B,0xD8,0xE8,0x3F,0x72,0xFC,0xFF,0x48,0x8B,0xC8,0xE8,0x67,0x3A,0xC1,0x00,0x85,0xC0,0x74,0x26,0xE8,0x0E,0x9C,0x02,0x00,0x8B,0xD3,0x48,0x8B,0xC8,0xE8,0x34,0xFF,0x02,0x00,0x84,0xC0,0x75,0x13,0x33,0xD2,0x8D,0x4A,0x01,0x48,0x8B,0x5C,0x24,0x20,0x48,0x83,0xC4,0x28};
// 001495AB..001495F8, the matcher 00149550: held button at +1968h (-1 when
// none, other presses ignored while set), second-bar flag at +1964h, which
// adds 6 to the button.
constexpr std::uint8_t W_Matcher_1495AB[] = {0x4C,0x69,0x64,0x24,0x58,0xB8,0x00,0x00,0x00,0x48,0x89,0x9C,0x24,0x88,0x03,0x00,0x00,0x4C,0x89,0xAC,0x24,0x30,0x03,0x00,0x00,0x41,0x83,0xBC,0x3C,0x68,0x19,0x00,0x00,0xFF,0x74,0x18,0x40,0x84,0xF6,0x0F,0x85,0xBF,0x06,0x00,0x00,0x41,0x8B,0xDF,0x41,0x38,0xB4,0x3C,0x64,0x19,0x00,0x00,0x74,0x24,0xEB,0x1E,0x40,0x84,0xF6,0x0F,0x84,0xA7,0x06,0x00,0x00,0x41,0x8B,0xDF,0xE8,0x48,0x39,0x00,0x00};
// 00145680..00145694: a press or release only reaches the matcher while
// manager+1948h is set.
constexpr std::uint8_t W_SkillSlotsActive_145680[] = {0x80,0xB9,0x48,0x19,0x00,0x00,0x00,0x74,0x0A,0x41,0x83,0xF8,0xFF,0x0F,0x85,0xBD,0x3E,0x00,0x00,0xC3};

constexpr Witness HoldWitnesses[] = {
	{0x14B7D9ULL, W_CancelLoop_14B7D9, 39, "CancelLoop"},
	{0x14B805ULL, W_CancelLoop_14B805, 152, "CancelLoop"},
	{0x11A9E0ULL, W_ReleaseSlot1_11A9E0, 110, "ReleaseSlot1"},
	{0x1495ABULL, W_Matcher_1495AB, 77, "Matcher"},
	{0x145680ULL, W_SkillSlotsActive_145680, 20, "SkillSlotsActive"},
};

// Relay stub at the redirected call: hand the loop's manager (rbx) and slot
// (edi) to OnCancelGate. Assembled with keystone, decoded with capstone:
// mov rcx,rbx / mov edx,edi; an absolute jump follows it.
constexpr std::uint8_t  CancelGateStub[] = {0x48,0x89,0xD9,0x89,0xFA};
constexpr std::uint8_t  CancelGateCall[] = {0xE8, 0xDB, 0x09, 0xF8, 0xFF}; // 0014B800: call 000CC1E0
constexpr std::uint64_t CancelGateSite   = 0x14B800;

constexpr const char* DefaultConfigToml = R"toml(# Controller skill hold fix - for Diablo II: Resurrected 3.3
#
# With a controller, holding a skill button keeps casting that skill until
# the game sees the button's release. The game throws a release away when,
# at that moment:
#   - the character is dead or missing (a death, a loading screen),
#   - the game is being left,
#   - the game option that lets a held skill button repeat is off,
#   - the controller's skill buttons are switched off,
#   - or the skill now on that button is a toggle skill (toggleSlotState in
#     controllerskillsettings.json).
# The hold then never ends. The character keeps casting the skill, its
# rumble keeps firing (that is the endless shaking, strongest on skills with
# a strong SkillUseRumble such as Whirlpool), and every other skill button is
# ignored, because a held button blocks all other presses.
#
# The game already cancels holds every frame while a panel is open, one of
# the moments it throws releases away. With this on, it also cancels them in
# every other moment it would throw a release away, so a hold can never
# outlive its release. Holding the button again afterwards starts a new hold
# as usual.
#
# Keep controller-skill-release-fix.json (static patch) installed as well: it
# fixes the skill bar mix-up that lost releases in a different way.
#
# The controllerhold console command shows whether the fix is active and how
# many holds it cancelled, and why. Each cancel is also written to the log.
#
# Site: RVA 0014B800 (the per-frame hold cancel in ControllerManager::Update,
# client).
#
# This file lives at <scope>\d2rloader\config\celestialrayone.controller-skill-hold.toml
# and is created with these defaults on first run. Delete it to get them back.
# Changes are read at plugin load, so restart the game after editing.


[controller_skill_hold]

# Default: true
enabled = true
)toml";

struct Config {
	bool enabled = true;
};

Config                     g_config {};
const D2RL::PluginContext* g_context   = nullptr;
bool                       g_installed = false;

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

void LoadConfiguration(const D2RL::PluginContext* context) noexcept {
	if (!context->EnsureConfig(DefaultConfigToml)) {
		context->LogWarn("Could not create or open celestialrayone.controller-skill-hold.toml. Built-in defaults are used.");
	}
	static std::array<char, 16384> buffer {};
	std::uint32_t                  required = 0;
	if (!context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size() - 1), &required)) {
		context->LogWarn("Could not read celestialrayone.controller-skill-hold.toml. Built-in defaults are used.");
	} else {
		buffer.back() = '\0';
		const char* t = buffer.data();
		ReadBool(t, "controller_skill_hold", "enabled", g_config.enabled);
	}
	D2RL::LogInfoF(context, "config: enabled=%d", g_config.enabled ? 1 : 0);
}

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

template <typename Fn>
void Bind(Fn& fn, std::uintptr_t base, std::uint64_t rva) noexcept {
	fn = reinterpret_cast<Fn>(base + rva);
}

struct Native {
	std::int32_t (*leavingGame)()                = nullptr; // 0008D3D0
	bool (*playerUnavailable)(std::uint32_t)     = nullptr; // 00099B60
	std::int32_t (*buttonSkill)(std::uint32_t)   = nullptr; // 00117D20
	void* (*repeatOption)()                      = nullptr; // 000E1C60
	std::int32_t (*optionEnabled)(void*)         = nullptr; // 00D2E490
	bool (*takesTogglePath)(void*, std::int32_t) = nullptr; // 0014A970
	bool (*panelOpen)()                          = nullptr; // 000CC1E0
};

Native g_native {};

constexpr std::uint32_t Players          = 8;      // 0014B8A7: cmp edi,8
constexpr std::size_t   Stride           = 0xB8;   // 0014B831: imul rax,rsi,0B8h
constexpr std::size_t   SkillButtonsOn   = 0x1948; // 00145680, on the manager
constexpr std::size_t   SecondBarFlag    = 0x1964; // 001495DB
constexpr std::size_t   HeldButton       = 0x1968; // 001495C4; +196Ch, the active hold, shares the qword
constexpr std::size_t   HeldSkill        = 0x1974; // 0014B851
constexpr std::size_t   HoldState        = 0x1998; // 0014B83D
constexpr std::size_t   Queue            = 0x19DC; // 0014B85D
constexpr std::size_t   QueueEntries     = 2;      // 0014B866: lea rcx,[rax+28h]
constexpr std::size_t   QueueEntrySize   = 20;     // 0014B880: add rax,14h
constexpr std::int32_t  SecondBarOffset  = 6;      // 00149605: lea ebx,[r15+6]
constexpr std::int32_t  HighestButton    = 15;     // 0014960C: cmp ebx,0Fh

enum class Reason : std::size_t {
	LeavingGame,
	PlayerUnavailable,
	RepeatOptionOff,
	SkillButtonsOff,
	ToggleSkill,
	Count,
};

constexpr std::array<const char*, static_cast<std::size_t>(Reason::Count)> ReasonText {
	"the game is being left",
	"the character is dead or missing",
	"the repeat option is off",
	"the controller skill buttons are off",
	"the button now holds a toggle skill",
};

std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(Reason::Count)> g_cancels {};

void BindNatives(std::uintptr_t base) noexcept {
	Bind(g_native.leavingGame, base, 0x08D3D0);
	Bind(g_native.playerUnavailable, base, 0x099B60);
	Bind(g_native.buttonSkill, base, 0x117D20);
	Bind(g_native.repeatOption, base, 0x0E1C60);
	Bind(g_native.optionEnabled, base, 0xD2E490);
	Bind(g_native.takesTogglePath, base, 0x14A970);
	Bind(g_native.panelOpen, base, 0x0CC1E0);
}

auto ReadI32(const std::uint8_t* p) noexcept -> std::int32_t {
	std::int32_t v = 0;
	std::memcpy(&v, p, sizeof(v));
	return v;
}

// Something a release would have to end: a held button, an active hold, or a
// queued press.
auto HoldPending(const std::uint8_t* slot) noexcept -> bool {
	if (ReadI32(slot + HeldButton) != -1 || ReadI32(slot + HeldButton + 4) != -1) {
		return true;
	}
	for (std::size_t e = 0; e < QueueEntries; ++e) {
		if (ReadI32(slot + Queue + e * QueueEntrySize) != -1) {
			return true;
		}
	}
	return false;
}

// The gates a release passes before it reaches the matcher, in the order the
// release handler (0011A9E0) and the dispatch (00145680) test them, minus the
// ones the native per-frame cancel already covers. Returns true and the gate
// when the release of this slot's held button would be thrown away now.
auto ReleaseWouldBeDropped(std::uint8_t* manager, std::uint32_t index, Reason& reason) noexcept -> bool {
	if (g_native.leavingGame() != 0) {
		reason = Reason::LeavingGame;
		return true;
	}
	if (g_native.playerUnavailable(index)) {
		reason = Reason::PlayerUnavailable;
		return true;
	}
	if (g_native.optionEnabled(g_native.repeatOption()) == 0) {
		reason = Reason::RepeatOptionOff;
		return true;
	}
	if (manager[SkillButtonsOn] == 0) {
		reason = Reason::SkillButtonsOff;
		return true;
	}
	const std::uint8_t* const slot = manager + Stride * index;
	const std::int32_t        held = ReadI32(slot + HeldButton);
	if (held >= 0) {
		// The handler that receives the release is the physical button's own,
		// and it checks the skill on that button of the first bar.
		const std::int32_t button = slot[SecondBarFlag] != 0 ? held - SecondBarOffset : held;
		if (button >= 0 && button <= HighestButton && g_native.takesTogglePath(manager, g_native.buttonSkill(static_cast<std::uint32_t>(button)))) {
			reason = Reason::ToggleSkill;
			return true;
		}
	}
	return false;
}

constexpr std::size_t RelayPageBytes = 4096;
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

// Replaces the call to 000CC1E0 (panel open) at 0014B800, the first test of
// the per-frame cancel in ControllerManager::Update. Returns what the native
// call returns, or true when this slot has a pending hold whose release would
// be thrown away right now, so the game's own cancel at 0014B831 runs.
bool __fastcall OnCancelGate(void* manager, std::uint32_t index) noexcept {
	if (g_native.panelOpen()) {
		return true;
	}
	if (!g_installed || manager == nullptr || index >= Players) {
		return false;
	}
	auto* const               m    = static_cast<std::uint8_t*>(manager);
	const std::uint8_t* const slot = m + Stride * index;
	if (!HoldPending(slot)) {
		return false;
	}
	Reason reason = Reason::Count;
	if (!ReleaseWouldBeDropped(m, index, reason)) {
		return false;
	}
	g_cancels[static_cast<std::size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
	if (g_context != nullptr) {
		D2RL::LogInfoF(g_context, "Cancelled the skill hold of player slot %u (button %d): %s.", index, ReadI32(slot + HeldButton), ReasonText[static_cast<std::size_t>(reason)]);
	}
	return true;
}

auto InstallCancelGate(const D2RL::PluginContext* context) noexcept -> bool {
	const auto base = static_cast<std::uintptr_t>(context->exeBase);
	g_relayPage     = AllocateNear(base + 0x300000, RelayPageBytes);
	if (g_relayPage == nullptr) {
		context->LogError("No relay page could be allocated within rel32 reach of the game image.");
		return false;
	}
	std::memset(g_relayPage, 0xCC, RelayPageBytes);
	std::memcpy(g_relayPage, CancelGateStub, sizeof(CancelGateStub));
	WriteAbsoluteJump(g_relayPage + sizeof(CancelGateStub), reinterpret_cast<std::uint64_t>(&OnCancelGate));
	DWORD previous = 0;
	if (!VirtualProtect(g_relayPage, RelayPageBytes, PAGE_EXECUTE_READ, &previous)) {
		VirtualFree(g_relayPage, 0, MEM_RELEASE);
		g_relayPage = nullptr;
		return false;
	}
	FlushInstructionCache(GetCurrentProcess(), g_relayPage, RelayPageBytes);
	const auto next   = base + CancelGateSite + 5;
	const auto target = reinterpret_cast<std::uintptr_t>(g_relayPage);
	if (!WithinRel32(next, target)) {
		return false;
	}
	const auto displacement = static_cast<std::int32_t>(static_cast<std::int64_t>(target) - static_cast<std::int64_t>(next));
	std::array<std::uint8_t, 5> call {0xE8};
	std::memcpy(call.data() + 1, &displacement, sizeof(displacement));
	return context->PatchBytes(CancelGateSite, CancelGateCall, sizeof(CancelGateCall), call.data(), call.size());
}

auto HoldCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	const D2RL::PluginContext* context = command != nullptr && command->plugin != nullptr ? command->plugin : g_context;
	if (context == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	char line[256];
	std::snprintf(line, sizeof(line), "Controller skill hold fix: %s", g_installed ? "active" : (g_config.enabled ? "not installed, see the log" : "disabled in the config"));
	context->WriteConsoleMessage(line);
	for (std::size_t r = 0; r < g_cancels.size(); ++r) {
		std::snprintf(line, sizeof(line), "Holds cancelled because %s: %llu", ReasonText[r],
			static_cast<unsigned long long>(g_cancels[r].load(std::memory_order_relaxed)));
		context->WriteConsoleMessage(line);
	}
	return D2RL::ConsoleCommandResult::Handled;
}

constexpr D2RL::PluginInfo HoldInfo {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = "celestialrayone.controller-skill-hold",
	.name        = "Controller skill hold fix",
	.version     = "1.0.0",
	.author      = "CelestialRayOne",
	.description = "Cancels a controller skill hold whenever the game would throw away its release, so a held skill can never keep casting forever.",
	.flags       = D2RL::PluginFlags::Client | D2RL::PluginFlags::NativeHooks,
};

static_assert(D2RL::HasValidPluginRole(HoldInfo.flags), "Exactly one execution role must be set.");

} // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
	return &HoldInfo;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr || context->exeBase == 0) {
		return false;
	}
	g_context = context;
	if (!context->RegisterConsoleCommand("controllerhold", HoldCommand, "Report the controller skill hold fix and its cancels.")) {
		context->LogWarn("The controllerhold console command was not registered.");
	}
	LoadConfiguration(context);
	if (!g_config.enabled) {
		context->LogInfo("Controller skill hold fix: disabled in the config, nothing installed.");
		return true;
	}
	if (!VerifyAll(context, HoldWitnesses, "Controller skill hold fix")) {
		context->LogWarn("Controller skill hold fix: game build not recognised, nothing installed.");
		return true;
	}
	BindNatives(static_cast<std::uintptr_t>(context->exeBase));
	g_installed = InstallCancelGate(context);
	context->LogInfo(g_installed ? "Controller skill hold fix: installed." : "Controller skill hold fix: the redirect at 0014B800 could not be installed.");
	return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {}
