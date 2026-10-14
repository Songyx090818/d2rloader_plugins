// Snapshot Sounds Fix
//
// See celestialrayone.snapshot-sounds-fix.toml for the write-up. In short:
//
//   D2RLoader keeps a "base compiler snapshot" of the unmodded game tables in
//   d2rloader/data/compiler. When it has to build that snapshot (first launch on
//   a machine, after a D2R patch, after a D2RLoader update that changes its
//   compiler, or when the snapshot is damaged), D2RCore's InitializeDataTables
//   runs the game's table load twice:
//
//     pass 1  unmounts mods/<mod>/<mod>.mpq, loads every table from the base
//             game, reloads sounds.txt from the base game for the snapshot
//             (D2RCore 0x267C60), then mounts the mod again.
//     pass 2  frees the tables except sounds.txt and loads everything again from
//             the mod. sounds.txt is never reloaded.
//
//   The session then runs with the base game's sounds.txt while every other table
//   comes from the mod. Every row a mod appended to sounds.txt is missing, so
//   monsounds.txt Taunt, skills.txt sound columns and the rest link to sound rows
//   that do not exist. The first boss taunt that uses one crashes the client in
//   the sound group picker (0x1400DA360, read at 0x1400DA4A0).
//
// The fix: a table may only be linked against a sounds.txt that came from the
// same files. Every sounds.txt load is watched and tagged with whether the mod
// archive was mounted. When the game's table load starts with the mod mounted
// while a bank still holds a sounds.txt loaded without it, that bank's sounds.txt
// is reloaded first, with the same two calls the game makes when it is asked to
// reload sounds itself (0x3016FE..0x30170F). Sessions that never build a snapshot
// are never touched.
//
// Built against D2R 3.3 (module 140000000.D2RLoader.exe, D2RLoader 1.3.1) and
// PluginSDK 0.3.0. Every address below is pinned by a byte check before
// anything is hooked.

#include <D2RLPlugin/api.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

// ---------------------------------------------------------------------------
// Addresses, all read out of the 3.3 image
// ---------------------------------------------------------------------------

// The game's InitializeDataTables(reloadSounds). D2RCore's InitializeDataTables
// export calls it once per pass through its import slot. Banks 2 and 3 are
// loaded; sounds.txt is reloaded first only when reloadSounds is nonzero.
constexpr std::uint64_t InitDataTablesRva = 0x00301590;
constexpr std::uint8_t ExpectedInitDataTablesPrologue[] {
	0x48, 0x8B, 0xC4,                          // mov rax, rsp
	0x88, 0x48, 0x08,                          // mov [rax+8], cl
	0x55,                                      // push rbp
	0x48, 0x8D, 0x68, 0xA1,                    // lea rbp, [rax-5Fh]
	0x48, 0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00,  // sub rsp, 0A0h
};

// Loads sounds.txt into one bank: rebuilds the bank's sound name list (the
// linker every other table resolves sound names against) and its sound rows.
// Callers: sound init 0x1400D9960, the reload block below, and D2RCore 0x267C60.
constexpr std::uint64_t LoadSoundsRva = 0x003B15D0;
constexpr std::uint8_t ExpectedLoadSoundsPrologue[] {
	0x48, 0x89, 0x5C, 0x24, 0x10,  // mov [rsp+10h], rbx
	0x48, 0x89, 0x74, 0x24, 0x18,  // mov [rsp+18h], rsi
	0x55,                          // push rbp
	0x57,                          // push rdi
	0x41, 0x56,                    // push r14
};

// The step the game runs right after every sounds.txt load.
constexpr std::uint64_t FinishSoundsRva = 0x003B0E40;

// Inside InitDataTables: the game's own "reload sounds" block. Proves both
// callees and the order the plugin repeats.
constexpr std::uint64_t SoundReloadBlockRva = 0x003016FE;
constexpr std::uint8_t ExpectedSoundReloadBlock[] {
	0x74, 0x10,                    // je past the reload (reloadSounds is 0)
	0x48, 0x8B, 0xCF,              // mov rcx, rdi      bank tables
	0xE8, 0xC8, 0xFE, 0x0A, 0x00,  // call 0x3B15D0     load sounds.txt
	0x48, 0x8B, 0xCF,              // mov rcx, rdi
	0xE8, 0x30, 0xF7, 0x0A, 0x00,  // call 0x3B0E40     finish the load
};

// GetDataTables(bank) 0x300A90: lea rcx, [2A9A580h] / add rax, rax /
// mov rax, [rcx+rax*8]. One DataTables pointer per bank, 16 bytes apart.
constexpr std::uint64_t BankTableReadRva = 0x00300AC0;
constexpr std::uint8_t ExpectedBankTableRead[] {
	0x48, 0x8D, 0x0D, 0xB9, 0x9A, 0x79, 0x02,  // lea rcx, [rva 2A9A580h]
	0x48, 0x03, 0xC0,                          // add rax, rax
	0x48, 0x8B, 0x04, 0xC1,                    // mov rax, [rcx+rax*8]
};
constexpr std::uint64_t BankTablesRva   = 0x02A9A580;
constexpr std::size_t   BankTablesStride = 16;
constexpr std::size_t   BankCount        = 4;

// Mounting the mod (0x2EE960) stores the handle of mods/<mod>/<mod>.mpq here;
// unmounting (0x9FFF80) frees it and writes 0. Nonzero means the mod is mounted.
constexpr std::uint64_t ModArchiveStoreRva = 0x002EE9B7;
constexpr std::uint8_t ExpectedModArchiveStore[] { 0x48, 0x89, 0x05, 0x52, 0xBA, 0x7A, 0x02 };  // mov [rva 2A9A410h], rax
constexpr std::uint64_t ModArchiveRva = 0x02A9A410;

// Sounds row count of a bank, read only for the log and the console command.
// DATATBLS_GetSoundsTxtRecordCount 0x3B20C0: mov cl, 3 / call 0x300A90 /
// mov rbx, [rax+4E8h].
constexpr std::uint64_t SoundsCountReadRva = 0x003B20C6;
constexpr std::uint8_t ExpectedSoundsCountRead[] {
	0xB1, 0x03,                                // mov cl, 3
	0xE8, 0xC3, 0xE9, 0xF4, 0xFF,              // call 0x300A90
	0x48, 0x8B, 0x98, 0xE8, 0x04, 0x00, 0x00,  // mov rbx, [rax+4E8h]
};
constexpr std::size_t SoundsCountOffset = 0x4E8;

// ---------------------------------------------------------------------------
// Game function types
// ---------------------------------------------------------------------------

using InitDataTablesFn = std::uint64_t(__fastcall*)(std::uint8_t reloadSounds) noexcept;
using LoadSoundsFn     = std::uint64_t(__fastcall*)(void* tables) noexcept;
using FinishSoundsFn   = void(__fastcall*)(void* tables) noexcept;

const D2RL::PluginContext* Context                = nullptr;
std::uintptr_t             ExeBase                = 0;
InitDataTablesFn           OriginalInitDataTables = nullptr;
LoadSoundsFn               OriginalLoadSounds     = nullptr;
FinishSoundsFn             FinishSounds           = nullptr;

// Where each bank's current sounds.txt came from.
enum class SoundSource : std::uint8_t {
	Unknown  = 0,  // not seen loading since the plugin loaded
	BaseGame = 1,  // loaded while no mod archive was mounted
	Mod      = 2,  // loaded while the mod archive was mounted
};

std::array<std::atomic<std::uint8_t>, BankCount>  BankSource {};
std::array<std::atomic<std::uint64_t>, BankCount> BankRows {};
std::atomic<std::uint64_t>                        Reloads { 0 };

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

struct Settings {
	bool enabled = true;
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
// section headers and comments are simply skipped.
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

// Written by EnsureConfig when the file does not exist yet. It is the plugin's
// documentation as shipped.
constexpr char DefaultConfigToml[] = R"toml(# celestialrayone.snapshot-sounds-fix
#
# Snapshot Sounds Fix
#
# Built for D2RLoader 1.3.1 (Diablo II: Resurrected 3.3). Read when the plugin
# loads; restart the game after editing.
#
# What goes wrong without it
#   D2RLoader keeps a snapshot of the unmodded game tables in
#   d2rloader/data/compiler. It rebuilds that snapshot on the first launch on a
#   machine, after a D2R patch, after a D2RLoader update that changes its
#   compiler, or when the snapshot is damaged. During that rebuild it loads the
#   tables twice: first from the base game for the snapshot, then from the mod.
#   sounds.txt is loaded from the base game in the first pass and never loaded
#   again, so for that whole session the game uses the base game's sounds.txt
#   with the mod's other tables.
#   Every sound the mod added to sounds.txt is missing in that session. Taunts in
#   monsounds.txt, sounds in skills.txt and every other sound column point at
#   rows that do not exist. The first boss whose Taunt is a mod-added sound (King
#   Leoric, Lachdanan, the Black Knight boss, the Lich King, Rathma, Lucion, the
#   treasure goblin) crashes the game when it taunts.
#   Sessions that do not rebuild the snapshot were always fine, which is why it
#   only hits some players and only once in a while.
#
# What the plugin does
#   Before the game loads the mod's tables, it checks where sounds.txt came from.
#   If it came from the base game while the mod is mounted, it loads the mod's
#   sounds.txt first, the same way the game reloads sounds itself. Nothing else
#   changes, and sessions that never rebuild the snapshot are not touched.
#   The plugin log says when this happened and how many sound rows the reload
#   brought back.
#
# Console
#   soundbanks   where each table bank's sounds.txt came from, its row count,
#                and how many reloads were needed this session.

# Turn the fix on or off. Off leaves the game exactly as stock D2RLoader runs it.
enabled = true
)toml";

auto LoadSettings(const D2RL::PluginContext* context) noexcept -> Settings {
	Settings settings {};
	if (!context->EnsureConfig(DefaultConfigToml)) {
		context->LogWarn("Could not create celestialrayone.snapshot-sounds-fix.toml; using defaults.");
		return settings;
	}
	std::array<char, 8'192> buffer {};
	if (!context->ReadConfig(buffer.data(), ByteSize(buffer.size()))) {
		context->LogWarn("Could not read celestialrayone.snapshot-sounds-fix.toml; using defaults.");
		return settings;
	}
	buffer.back()    = '\0';
	settings.enabled = ReadBool(buffer.data(), "enabled", settings.enabled);
	return settings;
}

// ---------------------------------------------------------------------------
// Game state
// ---------------------------------------------------------------------------

auto ModArchiveMounted() noexcept -> bool {
	return *reinterpret_cast<void* const*>(ExeBase + ModArchiveRva) != nullptr;
}

auto BankTables(std::size_t bank) noexcept -> void* {
	return *reinterpret_cast<void* const*>(ExeBase + BankTablesRva + bank * BankTablesStride);
}

auto BankOf(const void* tables) noexcept -> int {
	if (tables == nullptr) {
		return -1;
	}
	for (std::size_t bank = 0; bank < BankCount; ++bank) {
		if (BankTables(bank) == tables) {
			return static_cast<int>(bank);
		}
	}
	return -1;
}

auto SoundRows(const void* tables) noexcept -> std::uint64_t {
	return *reinterpret_cast<const std::uint64_t*>(static_cast<const std::uint8_t*>(tables) + SoundsCountOffset);
}

auto SourceName(std::uint8_t source) noexcept -> const char* {
	switch (static_cast<SoundSource>(source)) {
	case SoundSource::BaseGame:
		return "base game";
	case SoundSource::Mod:
		return "mod";
	default:
		return "not loaded since the plugin started";
	}
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

// Every sounds.txt load, from the game or from D2RCore, passes through here and
// is tagged with whether the mod archive was mounted at the time.
auto __fastcall HookLoadSounds(void* tables) noexcept -> std::uint64_t {
	const LoadSoundsFn original = OriginalLoadSounds;
	if (original == nullptr) {
		return 0;
	}
	const std::uint64_t result = original(tables);
	const int           bank   = BankOf(tables);
	if (bank >= 0) {
		const SoundSource source = ModArchiveMounted() ? SoundSource::Mod : SoundSource::BaseGame;
		BankSource[static_cast<std::size_t>(bank)].store(static_cast<std::uint8_t>(source));
		BankRows[static_cast<std::size_t>(bank)].store(tables != nullptr ? SoundRows(tables) : 0);
	}
	return result;
}

// The game's table load. Before any table is linked against sounds.txt, every
// bank whose sounds.txt came from the base game is reloaded from the mod.
auto __fastcall HookInitDataTables(std::uint8_t reloadSounds) noexcept -> std::uint64_t {
	const InitDataTablesFn original = OriginalInitDataTables;
	if (original == nullptr) {
		return 0;
	}
	// A nonzero argument makes the game reload sounds.txt itself, from whatever is
	// mounted, before the other tables. Nothing to do in that case.
	if (reloadSounds == 0 && ModArchiveMounted()) {
		for (std::size_t bank = 0; bank < BankCount; ++bank) {
			if (BankSource[bank].load() != static_cast<std::uint8_t>(SoundSource::BaseGame)) {
				continue;
			}
			void* tables = BankTables(bank);
			if (tables == nullptr) {
				continue;
			}
			const std::uint64_t before = SoundRows(tables);
			HookLoadSounds(tables);  // same call as the game's reload block, and it re-tags the bank
			FinishSounds(tables);
			Reloads.fetch_add(1);
			if (Context != nullptr) {
				char message[320] {};
				std::snprintf(message,
				              sizeof(message),
				              "Bank %u held the base game's sounds.txt (%llu rows) while the mod is mounted. "
				              "Reloaded it from the mod (%llu rows) before the tables link to it.",
				              static_cast<unsigned>(bank),
				              static_cast<unsigned long long>(before),
				              static_cast<unsigned long long>(SoundRows(tables)));
				Context->LogInfo(message);
			}
		}
	}
	return original(reloadSounds);
}

// ---------------------------------------------------------------------------
// Console
// ---------------------------------------------------------------------------

auto __cdecl SoundBanksCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept
        -> D2RL::ConsoleCommandResult {
	char line[256] {};
	std::snprintf(line,
	              sizeof(line),
	              "Snapshot Sounds Fix: mod archive %s, %llu sounds.txt reload(s) this session.",
	              ModArchiveMounted() ? "mounted" : "not mounted",
	              static_cast<unsigned long long>(Reloads.load()));
	command->plugin->WriteConsoleMessage(line);
	for (std::size_t bank = 0; bank < BankCount; ++bank) {
		void* tables = BankTables(bank);
		if (tables == nullptr) {
			continue;
		}
		std::snprintf(line,
		              sizeof(line),
		              "  bank %u: sounds.txt from the %s, %llu rows now.",
		              static_cast<unsigned>(bank),
		              SourceName(BankSource[bank].load()),
		              static_cast<unsigned long long>(SoundRows(tables)));
		command->plugin->WriteConsoleMessage(line);
	}
	return D2RL::ConsoleCommandResult::Handled;
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

struct Witness {
	std::uint64_t       rva;
	const std::uint8_t* bytes;
	std::uint32_t       size;
	const char*         what;
};

auto CheckWitnesses(const D2RL::PluginContext* context) noexcept -> bool {
	const Witness witnesses[] {
		{ InitDataTablesRva, ExpectedInitDataTablesPrologue, ByteCount(ExpectedInitDataTablesPrologue),
		  "0x301590 is not the game's InitializeDataTables prologue" },
		{ LoadSoundsRva, ExpectedLoadSoundsPrologue, ByteCount(ExpectedLoadSoundsPrologue),
		  "0x3B15D0 is not the sounds.txt loader prologue" },
		{ SoundReloadBlockRva, ExpectedSoundReloadBlock, ByteCount(ExpectedSoundReloadBlock),
		  "0x3016FE is not the game's sounds reload block (0x3B15D0, 0x3B0E40)" },
		{ BankTableReadRva, ExpectedBankTableRead, ByteCount(ExpectedBankTableRead),
		  "0x300AC0 does not read the bank table array at 0x2A9A580" },
		{ ModArchiveStoreRva, ExpectedModArchiveStore, ByteCount(ExpectedModArchiveStore),
		  "0x2EE9B7 does not store the mod archive handle at 0x2A9A410" },
		{ SoundsCountReadRva, ExpectedSoundsCountRead, ByteCount(ExpectedSoundsCountRead),
		  "0x3B20C6 does not read the sounds row count at +0x4E8" },
	};
	for (const Witness& witness : witnesses) {
		if (!context->CheckExpectedBytes(witness.rva, witness.bytes, witness.size)) {
			char message[200] {};
			std::snprintf(message, sizeof(message), "%s; wrong game build. Nothing was hooked.", witness.what);
			context->LogError(message);
			return false;
		}
	}
	return true;
}

constexpr D2RL::PluginInfo SnapshotSoundsInfo {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = "celestialrayone.snapshot-sounds-fix",
	.name        = "Snapshot Sounds Fix",
	.version     = "1.0.0",
	.author      = "CelestialRayOne",
	.description = "After D2RLoader rebuilds its base compiler snapshot, reloads sounds.txt from the mod so "
	               "mod-added sounds and boss taunts exist for the rest of the session.",
	.flags       = D2RL::PluginFlags::Client | D2RL::PluginFlags::NativeHooks,
};

}  // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
	return &SnapshotSoundsInfo;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr) {
		return false;
	}
	const Settings settings = LoadSettings(context);
	if (!settings.enabled) {
		context->LogInfo("enabled is false; nothing hooked.");
		return true;
	}
	ExeBase = context->exeBase;
	if (ExeBase == 0) {
		context->LogError("exeBase is zero; cannot resolve game addresses.");
		return false;
	}
	if (!CheckWitnesses(context)) {
		return false;
	}
	Context      = context;
	FinishSounds = reinterpret_cast<FinishSoundsFn>(ExeBase + FinishSoundsRva);

	// The loader hook first: it only records where sounds.txt came from, so it is
	// harmless on its own if the second hook is refused.
	if (!context->InstallInlineHook(LoadSoundsRva,
	                                ExpectedLoadSoundsPrologue,
	                                ByteCount(ExpectedLoadSoundsPrologue),
	                                HookLoadSounds,
	                                &OriginalLoadSounds)) {
		context->LogError("Inline hook at 0x3B15D0 (sounds.txt loader) failed. Nothing was hooked.");
		return false;
	}
	if (!context->InstallInlineHook(InitDataTablesRva,
	                                ExpectedInitDataTablesPrologue,
	                                ByteCount(ExpectedInitDataTablesPrologue),
	                                HookInitDataTables,
	                                &OriginalInitDataTables)) {
		context->LogError("Inline hook at 0x301590 (InitializeDataTables) failed; the fix is not active.");
		return true;  // the loader hook only records, so staying loaded is safe
	}

	if (!context->RegisterConsoleCommand("soundbanks", &SoundBanksCommand,
	                                     "Shows where each table bank's sounds.txt came from and its row count.")) {
		context->LogWarn("The soundbanks console command was refused.");
	}
	context->LogInfo("Watching sounds.txt loads; a base-game sounds.txt is reloaded from the mod before the mod's "
	                 "tables are built.");
	return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
	Context                = nullptr;
	OriginalInitDataTables = nullptr;
	OriginalLoadSounds     = nullptr;
	FinishSounds           = nullptr;
}
