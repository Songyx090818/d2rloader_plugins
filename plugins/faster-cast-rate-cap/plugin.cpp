// ---------------------------------------------------------------------------
// cast-rate-cap
//
// Raises (or lowers) the point at which faster cast rate stops paying.
//
// Vanilla stops the cast animation rate at 175 percent, which the engine's own
// diminishing-return curve reaches at exactly 200 faster cast rate. Every point
// past 200 is dead. This plugin makes that ceiling follow a faster cast rate
// number you choose, so the curve keeps paying up to the cap you set.
//
// Target: Diablo II: Resurrected 3.3.93847 (also 3.2.92777, Steam 3.3.93787 --
// those builds share an executable identity for patching purposes).
//
// Safety model: the address below is an RVA against image base 0x140000000, and
// nothing is written unless the bytes already at that RVA match byte for byte.
// On any build the plugin does not recognise it installs nothing, logs loudly
// and stays loaded so the console command can explain why. An unrecognised
// build is therefore a no-op, never a mis-patch.
//
// Configuration: <scope>\d2rloader\config\celestialrayone.cast-rate-cap.toml,
// created with documented defaults on first run. The default is 200, the
// vanilla cap, so installing this plugin and changing nothing changes nothing.
// The config is read once at load, so edits need a game restart.
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
// State
// ---------------------------------------------------------------------------

// Why the patch is not in. "You turned it off" and "this build is not
// supported" must never be reported as the same thing.
enum class FixState : std::uint8_t {
	NotAttempted,
	Installed,
	DisabledByConfig,
	UnsupportedBuild,
	InstallFailed,
};

std::atomic<FixState> State { FixState::NotAttempted };

void SetState(FixState state) noexcept {
	State.store(state, std::memory_order_release);
}

auto GetState() noexcept -> FixState {
	return State.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// The site
// ---------------------------------------------------------------------------
//
//   sub_140350B40 is the merged animation-rate updater: one function that
//   computes the animation rate for attacking, casting, blocking, hit recovery
//   and movement, dispatching on unit type and animation mode. Casting is the
//   branch taken for a player in mode 10, a monster in mode 7, and a player
//   running a sequence skill whose own animation mode is cast:
//
//     00350C14  BA 02 00 00 00      mov  edx, 2         ; rate kind 2 = cast
//     00350C19  48 8B CE            mov  rcx, rsi       ; the unit
//     00350C1C  E8 <rel32>          call 003516C0       ; diminished stat
//     00350C21  BB FF 7F 00 00      mov  ebx, 7FFFh
//     00350C26  8D 48 64            lea  ecx, [rax+64h] ; + 100
//     00350C29  B8 AF 00 00 00      mov  eax, 0AFh      ; <- the ceiling, 175
//     00350C2E  3B C8               cmp  ecx, eax
//     00350C30  0F 4F C8            cmovg ecx, eax      ; clamp
//     00350C33  ...                 rate * AnimationSpeed / 100
//
//   003516C0 is the shared diminishing-return helper. It reads a five row
//   table at 141D00B60, twelve bytes per row, {diminish flag, constant, stat
//   id}: attack {1,120,93}, hit recovery {1,120,99}, cast {1,120,105}, block
//   {1,120,102}, movement {1,150,96}. For a diminished row it returns
//   value * K / (K + value). So the cast branch is:
//
//     effective = 120 * FCR / (FCR + 120)
//     rate      = min(100 + effective, 175)
//     animSpeed = AnimationSpeed * rate / 100
//     frames    = ceil(256 * FramesPerDirection / animSpeed) - 1
//
//   effective reaches 75 at exactly 200 FCR, which is 175, which is the
//   ceiling. That is the whole of the vanilla FCR cap: it is not a separate
//   rule, it is this one immediate.
//
//   The fix rewrites that immediate to the rate the curve produces at the
//   configured cap, so the cap lands exactly where you asked:
//
//     ceiling(cap) = 100 + 120 * cap / (cap + 120)
//
//   which gives 175 at 200 (vanilla, unchanged) and 196 at 500. Five bytes, one
//   instruction, one operand. The instruction, its length and every surrounding
//   branch are untouched, so there is nothing to relocate and no hook.
//
//   Scope: this is the cast branch only. Attack, block, hit recovery and
//   movement have their own ceilings elsewhere in the same function and are not
//   touched. Monsters cast through this branch too, so a monster carrying FCR
//   is subject to the same cap.
//
//   The 95 byte window 00350C14..00350C72 is verified before writing, which
//   also proves the rate kind, the helper it calls, the + 100 base and the
//   clamp pair that consumes the immediate. The five byte pattern occurs
//   exactly once inside it.

constexpr std::uint64_t CastRateWindowRva = 0x00350C14ULL;
constexpr std::uint64_t CastClampRva      = 0x00350C29ULL;
constexpr std::uint64_t CastRateHelperRva = 0x003516C0ULL;  // call at 00350C1C

// RVA 0x350C14, 95 bytes. The cast branch of the animation-rate updater.
constexpr std::uint8_t CastRateWindow[] {
	0xBA, 0x02, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xCE, 0xE8, 0x9F, 0x0A, 0x00,
	0x00, 0xBB, 0xFF, 0x7F, 0x00, 0x00, 0x8D, 0x48, 0x64, 0xB8, 0xAF, 0x00,
	0x00, 0x00, 0x3B, 0xC8, 0x0F, 0x4F, 0xC8, 0xB8, 0x1F, 0x85, 0xEB, 0x51,
	0x41, 0x0F, 0xAF, 0xCC, 0xF7, 0xE1, 0x8B, 0xFA, 0xC1, 0xEF, 0x05, 0x3B,
	0xFB, 0x76, 0x12, 0x48, 0x8D, 0x4D, 0x18, 0xC6, 0x45, 0x18, 0x00, 0xE8,
	0x4C, 0x56, 0xFF, 0xFF, 0x84, 0xC0, 0x74, 0x01, 0xCC, 0x45, 0x33, 0xF6,
	0x85, 0xFF, 0x44, 0x0F, 0x4F, 0xF7, 0x44, 0x3B, 0xF3, 0x41, 0x0F, 0x4C,
	0xDE, 0x0F, 0xBF, 0xC3, 0x89, 0x46, 0x54, 0x66, 0x89, 0x5E, 0x68,
};

// mov eax, 0AFh
constexpr std::uint8_t CastClampOriginal[] {
	0xB8, 0xAF, 0x00, 0x00, 0x00,
};

constexpr std::size_t CastClampImmediateOffset = 1;

// ---------------------------------------------------------------------------
// The curve
// ---------------------------------------------------------------------------

constexpr std::int32_t CastRateBase   = 100;  // lea ecx, [rax+64h] at 00350C26
constexpr std::int32_t DiminishK      = 120;  // table row at 141D00B60 + 24
constexpr std::int32_t VanillaFcrCap  = 200;
constexpr std::int32_t VanillaCeiling = 175;  // the stock immediate

// 120 * cap / (cap + 120) is strictly below 120, so the ceiling can never pass
// 219 no matter how large the cap is. It gets there at 14280.
constexpr std::int32_t SaturatedCap     = 14280;
constexpr std::int32_t SaturatedCeiling = CastRateBase + DiminishK - 1;

constexpr std::int32_t MinFcrCap = 0;
constexpr std::int32_t MaxFcrCap = 100000;

// The rate the engine's own curve produces at `cap`, computed the way the
// engine computes it: integer divide, truncating.
constexpr auto CeilingForCap(std::int32_t cap) noexcept -> std::int32_t {
	if (cap <= 0) {
		return CastRateBase;
	}
	const auto wide = static_cast<std::int64_t>(cap);
	return CastRateBase + static_cast<std::int32_t>((DiminishK * wide) / (wide + DiminishK));
}

static_assert(sizeof(CastRateWindow) == 95, "Verified cast branch length changed.");
static_assert(sizeof(CastClampOriginal) == 5, "The site must stay one five byte instruction.");
static_assert(IsSubrangeOf(CastClampOriginal, CastRateWindow, CastClampRva - CastRateWindowRva), "The replaced bytes must sit inside the verified window at the stated offset.");
static_assert(CastClampOriginal[0] == 0xB8 && CastClampOriginal[CastClampImmediateOffset] == VanillaCeiling, "The site is not mov eax, 175.");
static_assert(CastRateWindow[0] == 0xBA && CastRateWindow[1] == 0x02, "The window must open on rate kind 2, the cast kind.");
static_assert(CastRateWindow[8] == 0xE8 && CallTargetRva(CastRateWindow, CastRateWindowRva, 8) == CastRateHelperRva, "The diminishing-return helper is not the callee witnessed at 00350C1C.");
static_assert(CastRateWindow[0x12] == 0x8D && CastRateWindow[0x13] == 0x48 && CastRateWindow[0x14] == CastRateBase, "The base added before the clamp is not 100.");
static_assert(CastRateWindow[0x1A] == 0x3B && CastRateWindow[0x1B] == 0xC8, "cmp ecx, eax must follow the immediate.");
static_assert(CastRateWindow[0x1C] == 0x0F && CastRateWindow[0x1D] == 0x4F && CastRateWindow[0x1E] == 0xC8, "cmovg ecx, eax must consume the immediate.");
static_assert(CeilingForCap(VanillaFcrCap) == VanillaCeiling, "The curve does not reproduce the stock ceiling at the stock cap.");
static_assert(CeilingForCap(500) == 196, "The curve does not reproduce the value the 2.4 patch shipped for a 500 cap.");
static_assert(CeilingForCap(SaturatedCap) == SaturatedCeiling, "The saturation point moved.");
static_assert(CeilingForCap(MaxFcrCap) == SaturatedCeiling, "The curve must be saturated at the largest accepted cap.");
static_assert(CeilingForCap(MinFcrCap) == CastRateBase, "A zero cap must leave the rate at the base.");

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
//
// The loader hands the plugin the raw text of its own TOML file; there is no
// parser in the SDK. This is a small line scanner that understands exactly what
// this file needs: [section] headers, `key = true` / `key = false`, `key = 123`,
// `#` comments (whole line or trailing), and both LF and CRLF. Anything it does
// not understand it ignores, which is the right failure mode for a config file
// a player edits by hand.
//
// The defaults are the safe direction: if the file is missing, unreadable or
// truncated, the cap stays at the vanilla 200 and the plugin says so in the log.

constexpr const char* DefaultConfigToml =
R"toml(# Cast Rate Cap - move the point where faster cast rate stops paying
#
# Vanilla stops the cast animation rate at 175 percent. The engine's own
# diminishing-return curve reaches 175 at exactly 200 faster cast rate, so
# every point of FCR past 200 does nothing at all. That ceiling is a single
# number in the executable, and this plugin sets it from the cap you want.
#
# Safety: the plugin verifies the original bytes at the patch site before it
# writes anything. On a game build it does not recognise it writes nothing no
# matter what this file says, logs loudly, and stays loaded so the
# `cast-rate-cap` console command can tell you why.
#
# This file lives at <scope>\d2rloader\config\celestialrayone.cast-rate-cap.toml
# and is created with these defaults on first run. Delete it to get them back.
# Changes are read at plugin load, so restart the game after editing.


[cast-rate-cap]

# Master switch.
#   true  - the cap below is applied.
#   false - the DLL stays loaded and the console command still answers, but
#           nothing is written. Vanilla behaviour.
# Default: true
enabled = true

# The faster cast rate value at which FCR stops paying, in the same points you
# see on the character screen.
#
# 200 is the vanilla cap, so the default changes nothing: install the plugin,
# leave this alone, and the game behaves exactly as it did. Raise it to let FCR
# keep paying further.
#
# How it works: the engine turns your FCR into an animation-rate bonus with
#
#     effective = 120 * FCR / (FCR + 120)
#     rate      = 100 + effective, stopped at a ceiling
#
# and the ceiling is what this plugin writes. It is set to the rate the curve
# itself produces at your cap, so the cap lands exactly where you asked:
#
#     cap     20    50   100   150   200   300   400   500   600  1000  2000
#     rate   117   135   154   166   175   185   192   196   200   207   213
#
# The curve is asymptotic, so the ceiling can never pass 219 however large a
# cap you set. It arrives there at 14280 and anything higher is the same thing.
# Accepted range is 0 to 100000; a value outside it is ignored and the vanilla
# 200 is used instead.
#
# Worth knowing before you pick a number: cast speed is measured in whole
# frames, so a higher cap only buys something when it crosses a frame boundary.
# The frames per cast are
#
#     frames = ceil(256 * FramesPerDirection / (AnimationSpeed * rate / 100)) - 1
#
# For a 14 frame cast animation at the usual AnimationSpeed of 256, vanilla's
# 200 cap gives 7 frames and nothing under a 600 cap improves on it, because
# 600 is where the rate finally reaches 200. A 16 frame animation gets 9 frames
# at the vanilla cap, 8 at a 400 cap and 7 at a 600 cap. Setting 500 when your
# casters run 14 frame animations buys nothing, so check the animation length
# you actually care about rather than picking a large round number.
#
# Scope: this is the cast rate only. Attack speed, block rate, hit recovery and
# run speed have their own separate ceilings and are not touched. Monsters cast
# through the same code, so a monster carrying FCR gets the same cap.
# Default: 200
fcr_cap = 200
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
	const char* text   = literal;
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

// Walks the file and hands every `section.key = value` pair to `visit`.
template <typename Visit>
void ForEachSetting(const char* toml, Visit visit) noexcept {
	if (toml == nullptr) {
		return;
	}

	std::array<char, 64> currentSection {};

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

				const Slice name = Trim({ trimmed.begin + 1, close });
				std::size_t used = 0;
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

					visit(currentSection.data(), name, raw);
				}
			}
		}

		line = (*lineEnd == '\0') ? lineEnd : lineEnd + 1;
	}
}

// Reads section.key as a boolean. Returns true only when the key was present
// and spelled true or false; `value` is left untouched otherwise.
auto ReadConfigBool(const char* toml, const char* section, const char* key, bool& value) noexcept -> bool {
	bool found = false;
	ForEachSetting(toml, [&](const char* currentSection, Slice name, Slice raw) noexcept {
		if (std::strcmp(currentSection, section) != 0 || !SliceEquals(name, key)) {
			return;
		}
		if (SliceEquals(raw, "true")) {
			value = true;
			found = true;
		} else if (SliceEquals(raw, "false")) {
			value = false;
			found = true;
		}
	});
	return found;
}

// Reads section.key as a whole number inside [low, high]. Anything that is not
// a plain run of digits, and anything outside the range, leaves `value`
// untouched and returns false, so a typo falls back to the default rather than
// to zero.
auto ReadConfigInt(
	const char*    toml,
	const char*    section,
	const char*    key,
	std::int32_t   low,
	std::int32_t   high,
	std::int32_t&  value,
	bool&          sawKey,
	bool&          sawBadValue) noexcept -> bool {
	bool found = false;
	ForEachSetting(toml, [&](const char* currentSection, Slice name, Slice raw) noexcept {
		if (std::strcmp(currentSection, section) != 0 || !SliceEquals(name, key)) {
			return;
		}

		sawKey = true;

		if (raw.begin >= raw.end) {
			sawBadValue = true;
			return;
		}

		std::int64_t parsed = 0;
		for (const char* cursor = raw.begin; cursor < raw.end; ++cursor) {
			if (*cursor < '0' || *cursor > '9') {
				sawBadValue = true;
				return;
			}
			parsed = parsed * 10 + (*cursor - '0');
			if (parsed > high) {
				sawBadValue = true;
				return;
			}
		}

		if (parsed < low) {
			sawBadValue = true;
			return;
		}

		value       = static_cast<std::int32_t>(parsed);
		sawBadValue = false;
		found       = true;
	});
	return found;
}

struct CastRateConfig {
	bool         pluginEnabled { true };
	std::int32_t fcrCap        { VanillaFcrCap };
};

CastRateConfig Config {};

// Only meaningful for the log line; the console command reads the state.
bool ConfigFileWasRead = false;

void LoadConfiguration(const D2RL::PluginContext* context) noexcept {
	if (context == nullptr) {
		return;
	}

	if (!context->EnsureConfig(DefaultConfigToml)) {
		context->LogWarn(
			"Could not create or open celestialrayone.cast-rate-cap.toml. "
			"Running with defaults: the vanilla 200 cap.");
		return;
	}

	std::array<char, 16384> buffer {};
	std::uint32_t           requiredSize = 0;

	if (!context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size() - 1), &requiredSize)) {
		context->LogWarn(
			"Could not read celestialrayone.cast-rate-cap.toml. "
			"Running with defaults: the vanilla 200 cap.");
		return;
	}

	if (requiredSize >= buffer.size()) {
		D2RL::LogWarnF(
			context,
			"celestialrayone.cast-rate-cap.toml is %u bytes, larger than the %zu byte read buffer. "
			"Running with defaults: the vanilla 200 cap.",
			requiredSize,
			buffer.size());
		return;
	}

	buffer[buffer.size() - 1] = '\0';
	ConfigFileWasRead         = true;

	(void)ReadConfigBool(buffer.data(), "cast-rate-cap", "enabled", Config.pluginEnabled);

	bool sawKey      = false;
	bool sawBadValue = false;
	(void)ReadConfigInt(buffer.data(), "cast-rate-cap", "fcr_cap", MinFcrCap, MaxFcrCap, Config.fcrCap, sawKey, sawBadValue);

	if (sawBadValue) {
		D2RL::LogWarnF(
			context,
			"fcr_cap in celestialrayone.cast-rate-cap.toml is not a whole number between %d and %d. "
			"Using %d instead.",
			MinFcrCap,
			MaxFcrCap,
			Config.fcrCap);
	}

	D2RL::LogInfoF(
		context,
		"config: enabled=%s, fcr_cap=%d (cast rate ceiling %d, vanilla is %d at %d).",
		Config.pluginEnabled ? "true" : "false",
		Config.fcrCap,
		CeilingForCap(Config.fcrCap),
		VanillaCeiling,
		VanillaFcrCap);
}

// ---------------------------------------------------------------------------
// Installation
// ---------------------------------------------------------------------------

const D2RL::PluginContext* LoadedContext = nullptr;

bool                         SitePatched = false;
std::array<std::uint8_t, sizeof(CastClampOriginal)> SiteWritten {};

constexpr const char* FixLabel = "cast rate cap";

auto InstallCastRateCap(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr) {
		return false;
	}

	if (!Config.pluginEnabled) {
		SetState(FixState::DisabledByConfig);
		D2RL::LogInfoF(context, "%s not installed: turned off in the config file.", FixLabel);
		return false;
	}

	if (!context->CheckExpectedBytes(CastRateWindowRva, CastRateWindow, ByteCount(CastRateWindow))) {
		SetState(FixState::UnsupportedBuild);
		D2RL::LogErrorF(
			context,
			"%s NOT installed: the bytes at RVA 00350C14 do not match the verified 3.3.93847 cast branch. "
			"This build is not supported, or another patch already owns the site; nothing was patched.",
			FixLabel);
		return false;
	}

	const std::int32_t ceiling = CeilingForCap(Config.fcrCap);

	SiteWritten = { CastClampOriginal[0], 0x00, 0x00, 0x00, 0x00 };
	const auto immediate = static_cast<std::uint32_t>(ceiling);
	SiteWritten[CastClampImmediateOffset + 0] = static_cast<std::uint8_t>(immediate & 0xFFU);
	SiteWritten[CastClampImmediateOffset + 1] = static_cast<std::uint8_t>((immediate >> 8) & 0xFFU);
	SiteWritten[CastClampImmediateOffset + 2] = static_cast<std::uint8_t>((immediate >> 16) & 0xFFU);
	SiteWritten[CastClampImmediateOffset + 3] = static_cast<std::uint8_t>((immediate >> 24) & 0xFFU);

	if (!context->PatchBytes(
			CastClampRva,
			CastClampOriginal,
			ByteCount(CastClampOriginal),
			SiteWritten.data(),
			static_cast<std::uint32_t>(SiteWritten.size()))) {
		SetState(FixState::InstallFailed);
		D2RL::LogErrorF(context, "%s NOT installed: PatchBytes failed at RVA 00350C29.", FixLabel);
		return false;
	}

	SitePatched = true;
	SetState(FixState::Installed);
	D2RL::LogInfoF(
		context,
		"%s installed at RVA 00350C29: faster cast rate now pays up to %d, cast rate ceiling %d%s.",
		FixLabel,
		Config.fcrCap,
		ceiling,
		ceiling == VanillaCeiling ? " (unchanged, this is the vanilla cap)" : "");
	return true;
}

void WithdrawCastRateCap() noexcept {
	const D2RL::PluginContext* context = LoadedContext;
	if (context == nullptr || !SitePatched) {
		return;
	}

	if (context->PatchBytes(
			CastClampRva,
			SiteWritten.data(),
			static_cast<std::uint32_t>(SiteWritten.size()),
			CastClampOriginal,
			ByteCount(CastClampOriginal))) {
		SitePatched = false;
	}
}

// ---------------------------------------------------------------------------
// Console command
// ---------------------------------------------------------------------------

auto CastRateCapCommand(
	D2R::Game::Client*                 client,
	const D2RL::ConsoleCommandContext* command,
	void*                              userData) noexcept -> D2RL::ConsoleCommandResult {
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
		"cast-rate-cap: config %s.",
		ConfigFileWasRead ? "loaded" : "NOT READ (defaults in use)");
	context->WriteConsoleMessage(header);

	char message[256] {};

	switch (GetState()) {
	case FixState::Installed:
		std::snprintf(
			message,
			sizeof(message),
			"active. Faster cast rate pays up to %d (cast rate ceiling %d). Vanilla is %d at %d%s.",
			Config.fcrCap,
			CeilingForCap(Config.fcrCap),
			VanillaCeiling,
			VanillaFcrCap,
			CeilingForCap(Config.fcrCap) == VanillaCeiling ? ", so this is stock behaviour" : "");
		context->WriteConsoleMessage(message);
		break;

	case FixState::DisabledByConfig:
		context->WriteConsoleWarning(
			"OFF because the config file turns it off. The game build is fine, and the cap is the vanilla 200.");
		break;

	case FixState::UnsupportedBuild:
		context->WriteConsoleError(
			"NOT ACTIVE. This game build is not recognised and nothing was patched. The cap is the vanilla 200.");
		break;

	case FixState::InstallFailed:
		context->WriteConsoleError(
			"NOT ACTIVE. The bytes matched but the patch could not be written. See the plugin log.");
		break;

	case FixState::NotAttempted:
	default:
		context->WriteConsoleError("NOT ACTIVE. Installation was never attempted.");
		break;
	}

	return D2RL::ConsoleCommandResult::Handled;
}

constexpr D2RL::PluginInfo CastRateCapInfo {
	.infoSize    = D2RL::PluginInfoSize,
	.apiVersion  = D2RL_PLUGIN_API_VERSION,
	.id          = "celestialrayone.cast-rate-cap",
	.name        = "Cast Rate Cap",
	.version     = "0.1.0",
	.author      = "CelestialRayOne",
	.description = "Moves the point where faster cast rate stops paying, from the vanilla 200 to a cap you choose.",
	.flags       = D2RL::PluginFlags::Client | D2RL::PluginFlags::NativeHooks,
};

static_assert(D2RL::HasValidPluginRole(CastRateCapInfo.flags), "Exactly one execution role must be set.");
static_assert(D2RL::HasOnlyKnownPluginFlags(CastRateCapInfo.flags), "Unknown plugin flag set.");

} // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
	return &CastRateCapInfo;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr) {
		return false;
	}

	LoadedContext = context;

	// Registered before anything is written on purpose. Returning false from
	// this function unloads the DLL and takes the console command with it,
	// which would leave a user on an unsupported build with no in-game signal
	// at all. This plugin would rather stay loaded and be able to say that it
	// did nothing.
	if (!context->RegisterConsoleCommand(
			"cast-rate-cap",
			CastRateCapCommand,
			"Report the faster cast rate cap this plugin applied.")) {
		context->LogWarn("The cast-rate-cap console command was not registered.");
	}

	if (const char* build = D2RL::GetBuildVersion(context); build != nullptr) {
		D2RL::LogInfoF(context, "cast-rate-cap loading against build %s.", build);
	}

	LoadConfiguration(context);

	(void)InstallCastRateCap(context);
	return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
	WithdrawCastRateCap();
}
