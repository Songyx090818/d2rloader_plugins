// ---------------------------------------------------------------------------
// Charge impact  -  D2RLoader plugin (D2R 3.x)
//
// Port of the three 2.4 static patches:
//     "Charge Missile-on-Hit Hook"   (2.4 RVA 386020)
//     "Charge Missile-on-Hit Cave"   (2.4 RVA 149B82C, 578 bytes)
//     "Charge srvdofunc - damage conversion uses Calc5 instead of Calc4"
//                                    (2.4 RVA 385F13)
//
// Behaviour, identical to the 2.4 caves:
//     When Charge (srvdofunc 67) connects and the engine queues the combat
//     record for that hit, the plugin
//         * evaluates one skills.txt calc column of the charging skill and
//           spawns that many server missiles,
//         * spawns them ON the target and aims them through it: the
//           destination is 2*target - caster, so they carry on in the charge
//           direction,
//         * then runs the impact skill once. The 2.4 cave called
//           SrvDo150_Smite(pGame, pUnit, 525, chargeLevel); here the handler
//           is taken from the impact skill's own skills.txt srvdofunc column,
//           so retuning that row in the txt keeps working.
//     The missile id is the one the engine itself would pick for the skill:
//     srvmissilea for a non progressive skill, srvmissilea/b/c by charge
//     count for a progressive one.
//     Only hits on monsters spawn missiles, which is the 2.4 cave's opening
//     test (cmp dword ptr [r12], 1).
//
// What is configurable
//     Every calc column this touches is a config value in the range 1..10.
//     skills.txt in 3.x really does carry calc1 through calc10: the column
//     names sit at 0x1CF9CD0..0x1CF9D18 and the txt loader at 0x302380 binds
//     them to record offsets 0x190, 0x194 ... 0x1B4, four bytes each.
//
// How it is implemented on 3.x
//     The 2.4 version replaced an instruction in the middle of the Charge
//     handler with a jmp into a code cave. A mid function hook cannot be
//     expressed as a C function (the detour would be entered without a return
//     address on the stack), so this plugin hooks the ENTRY of two functions:
//
//       0x5622C0  srvdofunc 67, Charge. Wrapping it marks "a charge is
//                 running on this thread" and carries the skill id and level.
//                 Verified through the srvdofunc table at 0x238EA00 slot 67,
//                 which must point at this exact address.
//       0x44B600  SUNITDMG_PrepareAndQueueCombatRecord
//                 (game, attacker, defender, D2Damage*, flag). The Charge
//                 handler calls it exactly once per landed impact, then
//                 drains durability (0x44B2B0) and sets overlay 147
//                 (0x349020). That call is the 2.4 hook site, so firing right
//                 after the trampoline reproduces the old insertion point
//                 exactly: damage first, then missiles, then the impact
//                 skill. Charge's melee range test (0x348650) only measures
//                 distance, so no other tick of the skill reaches this call.
//     Both hooks are inert for everyone else: the damage hook does one bool
//     test and forwards to the trampoline.
//
// Game functions used (RVAs into the running main module, D2RLoader.exe):
//     0x097790  DATATBLS_GetSkillsTxtRecordForContext(context, skillId)
//     0x3B5160  SKILLS_EvaluateSkillFormula(context, unit, code, skill, lvl)
//     0x571700  SKILLS_GetProgressiveSkillMissileId(unit, skillId)
//     0x1B89A0  missiles.txt row count for a context, used for the same bound
//               check the engine performs before creating a missile
//     0x5371A0  MISSILES_CreateMissileFromParams(game, params)
//     0x341A20  path X, movzx eax, word ptr [rcx+2]
//     0x341A30  path Y, movzx eax, word ptr [rcx+6]
//
// Missile params struct, 0x98 bytes, confirmed from two native builders
// (srvdofunc 11 at 0x5562B0 and the skill start wrapper at 0x4333F0):
//     +0x00 flags 0x21   +0x08 owner       +0x20 missile id
//     +0x24 nX           +0x28 nY          +0x2C nTargetX   +0x30 nTargetY
//     +0x34 skill id     +0x40 skill level
// Everything else stays zero, which is what the corrected 2.4 cave did.
// Missiles built this way do not carry the attack rating stamp themselves;
// the attack-rating-overhaul plugin's missile fix covers that at creation.
//
// unit:  +0x00 unit type (1 = monster),  +0x38 path pointer
// path:  unit types 2, 4 and 5 keep the position as dwords at +0x10/+0x14,
//        every other type as words at +0x02/+0x06. That is what the engine
//        does inline at each of these sites.
//
// Runtime settings live in
// d2rloader/config/celestialrayone.charge-impact.toml, created with defaults
// on first load. The values below are only the fallbacks used when a key is
// missing or the file cannot be read.
// ---------------------------------------------------------------------------

#include <D2RLPlugin/api.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

// --------------------------------------------------------------------------
// Build-specific constants
// --------------------------------------------------------------------------

constexpr std::uint64_t kChargeSrvDoRva          = 0x005622C0ULL;
constexpr std::uint64_t kQueueCombatRecordRva    = 0x0044B600ULL;
constexpr std::uint64_t kSrvDoTableRva           = 0x0238EA00ULL;
constexpr std::uint64_t kGetSkillsRecordRva      = 0x00097790ULL;
constexpr std::uint64_t kEvalSkillFormulaRva     = 0x003B5160ULL;
constexpr std::uint64_t kProgressiveMissileRva   = 0x00571700ULL;
constexpr std::uint64_t kMissileRecordCountRva   = 0x001B89A0ULL;
constexpr std::uint64_t kCreateMissileRva        = 0x005371A0ULL;
constexpr std::uint64_t kPathGetXRva             = 0x00341A20ULL;
constexpr std::uint64_t kPathGetYRva             = 0x00341A30ULL;

// The two calc reads inside the Charge handler, both `mov r8d, [r13+disp32]`
// feeding SKILLS_EvaluateSkillFormula. The displacement is the skills.txt
// record offset of the calc column, so repointing them is a four byte write.
//     0x5627CB  calc1 -> the charge's own damage
//     0x5627F8  calc4 -> the elemental conversion percentage
constexpr std::uint64_t kChargeDamageCalcReadRva     = 0x005627CBULL;
constexpr std::uint64_t kChargeConversionCalcReadRva = 0x005627F8ULL;
constexpr std::uint64_t kCalcDisplacementOffset      = 3ULL;

// Prologue bytes, used as a build check. A mismatch means this plugin is
// running on a build it was not compiled for, and it refuses to load.
constexpr std::uint8_t kChargeSrvDoBytes[]{
	0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
	0x48, 0x8D, 0xAC, 0x24, 0x08, 0xFF, 0xFF, 0xFF,
	0x48, 0x81, 0xEC, 0xF8, 0x01, 0x00, 0x00,
};
constexpr std::uint8_t kQueueCombatRecordBytes[]{
	0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x30,
	0x49, 0x8B, 0xD9, 0x49, 0x8B, 0xF0, 0x48, 0x8B, 0xEA, 0x4C, 0x8B, 0xF1,
	0x48, 0x85, 0xD2, 0x74, 0x05, 0x4D, 0x85, 0xC0, 0x75, 0x26,
};
constexpr std::uint8_t kGetSkillsRecordBytes[]{
	0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x20,
	0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x63, 0xF2,
};
constexpr std::uint8_t kEvalSkillFormulaBytes[]{
	0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10,
	0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x41, 0x56, 0x41, 0x57,
};
constexpr std::uint8_t kProgressiveMissileBytes[]{
	0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10,
	0x57, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0xDA, 0x48, 0x8B, 0xF1,
};
constexpr std::uint8_t kMissileRecordCountBytes[]{
	0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0xE8, 0xE5, 0x80, 0x14, 0x00,
	0x48, 0x8B, 0x98, 0x28, 0x11, 0x00, 0x00,
};
constexpr std::uint8_t kCreateMissileBytes[]{
	0x40, 0x55, 0x57, 0x41, 0x56, 0x48, 0x8D, 0x6C, 0x24, 0xC0,
	0x48, 0x81, 0xEC, 0x40, 0x01, 0x00, 0x00,
};
constexpr std::uint8_t kPathGetXBytes[]{ 0x0F, 0xB7, 0x41, 0x02, 0xC3 };
constexpr std::uint8_t kPathGetYBytes[]{ 0x0F, 0xB7, 0x41, 0x06, 0xC3 };

// `mov r8d, [r13+0x190]` and `mov r8d, [r13+0x19C]`.
constexpr std::uint8_t kChargeDamageCalcReadBytes[]{ 0x45, 0x8B, 0x85, 0x90, 0x01, 0x00, 0x00 };
constexpr std::uint8_t kChargeConversionCalcReadBytes[]{ 0x45, 0x8B, 0x85, 0x9C, 0x01, 0x00, 0x00 };

// --------------------------------------------------------------------------
// Layout constants
// --------------------------------------------------------------------------

constexpr std::size_t kGameDataContextOffset = 0x106; // game -> txt context byte
constexpr std::size_t kUnitTypeOffset        = 0x00;
constexpr std::size_t kUnitPathOffset        = 0x38;
constexpr std::size_t kPathDwordXOffset      = 0x10;
constexpr std::size_t kPathDwordYOffset      = 0x14;

constexpr std::uint32_t kUnitTypeMonster = 1;

// skills.txt record layout
constexpr std::size_t  kRecordSrvDoFuncOffset = 78;    // int16
constexpr std::size_t  kRecordCalcBaseOffset  = 0x190; // calc1, then +4 per column
constexpr std::int32_t kCalcColumnCount       = 10;    // calc1 .. calc10

// The engine refuses to dispatch a srvdofunc above this value
// (cmp word ptr [record+78], 0BEh in the server skill handler at 0x43ACB0).
constexpr std::int32_t kMaxSrvDoFuncIndex = 190;

// Missile params struct built on the stack for MISSILES_CreateMissileFromParams.
constexpr std::size_t kMissileParamsSize        = 0xA0; // native frame uses 0x98
constexpr std::size_t kParamsFlagsOffset        = 0x00;
constexpr std::size_t kParamsOwnerOffset        = 0x08;
constexpr std::size_t kParamsMissileIdOffset    = 0x20;
constexpr std::size_t kParamsOriginXOffset      = 0x24;
constexpr std::size_t kParamsOriginYOffset      = 0x28;
constexpr std::size_t kParamsTargetXOffset      = 0x2C;
constexpr std::size_t kParamsTargetYOffset      = 0x30;
constexpr std::size_t kParamsSkillIdOffset      = 0x34;
constexpr std::size_t kParamsSkillLevelOffset   = 0x40;
constexpr std::uint32_t kParamsExplicitCoordFlags = 0x21;

// Defaults. These reproduce what ESR shipped on 2.4: the missile count comes
// from calc1, the charge damage stays on calc1 (vanilla), and the elemental
// conversion is moved off calc4 onto calc5.
constexpr std::int32_t kDefaultMissileCountCalc = 1;
constexpr std::int32_t kVanillaDamageCalc       = 1;
constexpr std::int32_t kDefaultDamageCalc       = 1;
constexpr std::int32_t kVanillaConversionCalc   = 4;
constexpr std::int32_t kDefaultConversionCalc   = 5;
constexpr std::int32_t kDefaultImpactSkillId    = 525;

// --------------------------------------------------------------------------
// Game function types
// --------------------------------------------------------------------------

// The txt context is passed in ecx as a zero extended byte, so it is typed as
// a dword here to guarantee the high bits are clear.
using ChargeSrvDoFn        = std::int64_t(__fastcall*)(void* game, void* unit, std::int32_t skillId, std::int32_t skillLevel) noexcept;
using SrvDoFn              = std::int64_t(__fastcall*)(void* game, void* unit, std::int32_t skillId, std::int32_t skillLevel) noexcept;
using QueueCombatRecordFn  = std::int64_t(__fastcall*)(void* game, void* attacker, void* defender, void* damage, std::uint8_t flag) noexcept;
using GetSkillsRecordFn    = const std::uint8_t*(__fastcall*)(std::uint32_t context, std::int32_t skillId) noexcept;
using EvalSkillFormulaFn   = std::int32_t(__fastcall*)(std::uint32_t context, void* unit, std::int32_t code, std::int32_t skillId, std::int32_t skillLevel) noexcept;
using ProgressiveMissileFn = std::int32_t(__fastcall*)(void* unit, std::int32_t skillId) noexcept;
using MissileRowCountFn    = std::int32_t(__fastcall*)(std::uint32_t context) noexcept;
using CreateMissileFn      = void*(__fastcall*)(void* game, void* params) noexcept;
using PathGetCoordFn       = std::int32_t(__fastcall*)(void* path) noexcept;

// --------------------------------------------------------------------------
// State
// --------------------------------------------------------------------------

std::uintptr_t g_moduleBase = 0;

ChargeSrvDoFn       g_originalChargeSrvDo   = nullptr;
QueueCombatRecordFn g_originalQueueCombat   = nullptr;

GetSkillsRecordFn    g_getSkillsRecord   = nullptr;
EvalSkillFormulaFn   g_evalSkillFormula  = nullptr;
ProgressiveMissileFn g_progressiveMissile = nullptr;
MissileRowCountFn    g_missileRowCount   = nullptr;
CreateMissileFn      g_createMissile     = nullptr;
PathGetCoordFn       g_pathGetX          = nullptr;
PathGetCoordFn       g_pathGetY          = nullptr;
SrvDoFn const*       g_srvDoTable        = nullptr;

struct Settings {
	bool         enabled            = true;
	bool         missilesEnabled    = true;
	std::int32_t missileCountCalc   = kDefaultMissileCountCalc;
	bool         monstersOnly       = true;
	std::int32_t maxMissiles        = 0; // 0 = no clamp, which is 2.4 behaviour
	std::int32_t damageCalc         = kDefaultDamageCalc;
	std::int32_t conversionCalc     = kDefaultConversionCalc;
	bool         impactSkillEnabled = true;
	std::int32_t impactSkillId      = kDefaultImpactSkillId;
};

Settings g_settings{};

// Per thread, because the impact skill can itself start another skill and the
// server may run more than one thread. Nesting is handled by save/restore.
struct ChargeScope {
	bool         active     = false;
	void*        unit       = nullptr;
	std::int32_t skillId    = 0;
	std::int32_t skillLevel = 0;
	bool         fired      = false;
};

thread_local ChargeScope t_charge{};

volatile LONG g_impacts        = 0;
volatile LONG g_missilesSpawned = 0;
volatile LONG g_impactSkillRuns = 0;
bool          g_damagePatched   = false;
bool          g_conversionPatched = false;

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------

auto BytePointer(void* base, std::size_t offset) noexcept -> std::uint8_t* {
	return static_cast<std::uint8_t*>(base) + offset;
}

auto ReadInt32(const std::uint8_t* base, std::size_t offset) noexcept -> std::int32_t {
	std::int32_t value = 0;
	std::memcpy(&value, base + offset, sizeof(value));
	return value;
}

auto ReadInt16(const std::uint8_t* base, std::size_t offset) noexcept -> std::int16_t {
	std::int16_t value = 0;
	std::memcpy(&value, base + offset, sizeof(value));
	return value;
}

void WriteInt32(std::uint8_t* base, std::size_t offset, std::int32_t value) noexcept {
	std::memcpy(base + offset, &value, sizeof(value));
}

void WritePointer(std::uint8_t* base, std::size_t offset, void* value) noexcept {
	std::memcpy(base + offset, &value, sizeof(value));
}

// skills.txt record offset of calcN. Callers pass a value already validated
// to 1..10 by the config loader.
auto CalcRecordOffset(std::int32_t column) noexcept -> std::size_t {
	return kRecordCalcBaseOffset + 4u * static_cast<std::size_t>(column - 1);
}

auto GameContext(void* game) noexcept -> std::uint32_t {
	return *BytePointer(game, kGameDataContextOffset);
}

auto UnitType(void* unit) noexcept -> std::uint32_t {
	return *reinterpret_cast<const std::uint32_t*>(BytePointer(unit, kUnitTypeOffset));
}

// Reproduces the position read the engine performs inline at every one of
// these call sites: dword coordinates for unit types 2, 4 and 5, the word
// pair for everything else.
auto GetUnitPosition(void* unit, std::int32_t* outX, std::int32_t* outY) noexcept -> bool {
	if (unit == nullptr) {
		return false;
	}

	void* path = *reinterpret_cast<void* const*>(BytePointer(unit, kUnitPathOffset));
	if (path == nullptr) {
		return false;
	}

	const std::uint32_t type = UnitType(unit);
	if (type == 2u || (type - 4u) < 2u) {
		*outX = *reinterpret_cast<const std::int32_t*>(BytePointer(path, kPathDwordXOffset));
		*outY = *reinterpret_cast<const std::int32_t*>(BytePointer(path, kPathDwordYOffset));
	} else {
		*outX = g_pathGetX(path);
		*outY = g_pathGetY(path);
	}
	return true;
}

// --------------------------------------------------------------------------
// Impact work
// --------------------------------------------------------------------------

// Evaluates the configured calc column the way the 2.4 cave did: an empty
// column, or a formula that yields less than one, means a single missile.
auto ResolveMissileCount(std::uint32_t context, const std::uint8_t* record, void* unit) noexcept -> std::int32_t {
	const std::int32_t code = ReadInt32(record, CalcRecordOffset(g_settings.missileCountCalc));
	std::int32_t count = 1;

	if (code >= 0) {
		const std::int32_t evaluated =
			g_evalSkillFormula(context, unit, code, t_charge.skillId, t_charge.skillLevel);
		if (evaluated > 1) {
			count = evaluated;
		}
	}

	if (g_settings.maxMissiles > 0 && count > g_settings.maxMissiles) {
		count = g_settings.maxMissiles;
	}
	return count;
}

void SpawnMissiles(void* game, void* unit, void* target, std::uint32_t context, const std::uint8_t* record) noexcept {
	const std::int32_t missileId = g_progressiveMissile(unit, t_charge.skillId);
	if (missileId < 0 || missileId >= g_missileRowCount(context)) {
		return;
	}

	std::int32_t targetX = 0;
	std::int32_t targetY = 0;
	std::int32_t casterX = 0;
	std::int32_t casterY = 0;
	if (!GetUnitPosition(target, &targetX, &targetY) || !GetUnitPosition(unit, &casterX, &casterY)) {
		return;
	}

	// Spawn on the target, aim through it, exactly like the 2.4 cave and like
	// the native srvdofunc 11 builder.
	const std::int32_t throughX = 2 * targetX - casterX;
	const std::int32_t throughY = 2 * targetY - casterY;

	const std::int32_t count = ResolveMissileCount(context, record, unit);
	for (std::int32_t index = 0; index < count; ++index) {
		alignas(16) std::uint8_t params[kMissileParamsSize]{};

		WriteInt32(params, kParamsFlagsOffset, static_cast<std::int32_t>(kParamsExplicitCoordFlags));
		WritePointer(params, kParamsOwnerOffset, unit);
		WriteInt32(params, kParamsMissileIdOffset, missileId);
		WriteInt32(params, kParamsOriginXOffset, targetX);
		WriteInt32(params, kParamsOriginYOffset, targetY);
		WriteInt32(params, kParamsTargetXOffset, throughX);
		WriteInt32(params, kParamsTargetYOffset, throughY);
		WriteInt32(params, kParamsSkillIdOffset, t_charge.skillId);
		WriteInt32(params, kParamsSkillLevelOffset, t_charge.skillLevel);

		g_createMissile(game, params);
		::InterlockedIncrement(&g_missilesSpawned);
	}
}

// Runs the impact skill once, at the charge's own level, through whatever
// server handler that skill's skills.txt row declares.
void RunImpactSkill(void* game, void* unit, std::uint32_t context) noexcept {
	if (!g_settings.impactSkillEnabled || g_settings.impactSkillId < 0) {
		return;
	}

	const std::uint8_t* record = g_getSkillsRecord(context, g_settings.impactSkillId);
	if (record == nullptr) {
		return;
	}

	const std::int32_t srvDoFunc = ReadInt16(record, kRecordSrvDoFuncOffset);
	if (srvDoFunc <= 0 || srvDoFunc > kMaxSrvDoFuncIndex) {
		return;
	}

	const SrvDoFn handler = g_srvDoTable[srvDoFunc];
	if (handler == nullptr) {
		return;
	}

	handler(game, unit, g_settings.impactSkillId, t_charge.skillLevel);
	::InterlockedIncrement(&g_impactSkillRuns);
}

void OnChargeImpact(void* game, void* unit, void* target) noexcept {
	if (game == nullptr || unit == nullptr || target == nullptr) {
		return;
	}
	if (g_settings.monstersOnly && UnitType(target) != kUnitTypeMonster) {
		return;
	}

	const std::uint32_t  context = GameContext(game);
	const std::uint8_t*  record  = g_getSkillsRecord(context, t_charge.skillId);
	if (record == nullptr) {
		return;
	}

	::InterlockedIncrement(&g_impacts);

	if (g_settings.missilesEnabled) {
		SpawnMissiles(game, unit, target, context, record);
	}
	RunImpactSkill(game, unit, context);
}

// --------------------------------------------------------------------------
// Hooks
// --------------------------------------------------------------------------

auto __fastcall HookChargeSrvDo(void* game, void* unit, std::int32_t skillId, std::int32_t skillLevel) noexcept -> std::int64_t {
	const ChargeSrvDoFn original = g_originalChargeSrvDo;
	if (original == nullptr) {
		return 0;
	}
	if (!g_settings.enabled) {
		return original(game, unit, skillId, skillLevel);
	}

	const ChargeScope previous = t_charge;

	t_charge.active     = true;
	t_charge.unit       = unit;
	t_charge.skillId    = skillId;
	t_charge.skillLevel = skillLevel;
	t_charge.fired      = false;

	const std::int64_t result = original(game, unit, skillId, skillLevel);

	t_charge = previous;
	return result;
}

auto __fastcall HookQueueCombatRecord(void* game, void* attacker, void* defender, void* damage, std::uint8_t flag) noexcept -> std::int64_t {
	const QueueCombatRecordFn original = g_originalQueueCombat;
	const std::int64_t result = original != nullptr
		? original(game, attacker, defender, damage, flag)
		: 0;

	if (t_charge.active
		&& !t_charge.fired
		&& attacker != nullptr
		&& attacker == t_charge.unit
		&& defender != nullptr) {

		// Set before doing the work: the impact skill queues combat records of
		// its own, and this is what keeps it from re-entering.
		t_charge.fired = true;
		OnChargeImpact(game, attacker, defender);
	}

	return result;
}

// --------------------------------------------------------------------------
// Config file
// --------------------------------------------------------------------------

constexpr const char* kDefaultConfigToml =
	"# Charge impact\n"
	"#\n"
	"# Port of the 2.4 ESR charge patches. When Charge connects, the skill\n"
	"# spawns a number of missiles through the target and then runs an impact\n"
	"# skill once. The missile itself is the skill's srvmissilea row (or\n"
	"# srvmissilea/b/c when the skill is progressive), the same one the engine\n"
	"# would pick for a normal cast.\n"
	"#\n"
	"# Every *_calc key names a skills.txt calc column and accepts 1 to 10.\n"
	"\n"
	"[charge-impact]\n"
	"\n"
	"# Master switch. false leaves both hooks installed but inert. The two calc\n"
	"# redirects below are applied at load and are not affected by this key.\n"
	"enabled = true\n"
	"\n"
	"# --- missiles ------------------------------------------------------------\n"
	"\n"
	"# Spawn missiles on impact at all.\n"
	"missiles_enabled = true\n"
	"\n"
	"# skills.txt calc column of the CHARGING skill that gives the number of\n"
	"# missiles. It is a full formula field, so skill('x'.blvl) and the rest of\n"
	"# the usual syntax work. An empty column, or a result below 1, spawns one\n"
	"# missile. 2.4 used calc1.\n"
	"# Careful: calc1 is also the charge's own damage formula unless damage_calc\n"
	"# below moves it, so pointing the count at a free column is usually the\n"
	"# cleaner setup.\n"
	"missile_count_calc = 1\n"
	"\n"
	"# Only spawn when the unit the charge connected with is a monster. This is\n"
	"# the 2.4 cave's own filter. false also fires on players and mercenaries.\n"
	"missiles_monsters_only = true\n"
	"\n"
	"# Upper bound on the missiles one impact may spawn, as a guard against a\n"
	"# runaway formula. 0 means no clamp, which is what 2.4 did.\n"
	"max_missiles = 0\n"
	"\n"
	"# --- calc columns the Charge handler reads -------------------------------\n"
	"\n"
	"# Column the charge reads for its own damage. Vanilla is 1; leaving it at 1\n"
	"# writes nothing to the binary.\n"
	"damage_calc = 1\n"
	"\n"
	"# Column the charge reads for the elemental damage conversion percentage.\n"
	"# Vanilla is 4. ESR shipped 5 on 2.4, which is the default here; setting it\n"
	"# back to 4 leaves the instruction untouched.\n"
	"conversion_calc = 5\n"
	"\n"
	"# --- impact skill --------------------------------------------------------\n"
	"\n"
	"# Run a second skill on the charging unit when the charge connects.\n"
	"impact_skill_enabled = true\n"
	"\n"
	"# skills.txt Id of that skill. It runs at the charge's own skill level,\n"
	"# through the server handler its own srvdofunc column names, so retuning\n"
	"# the row keeps working. 2.4 hardcoded skill 525 on srvdofunc 150 (Smite).\n"
	"# -1 disables it.\n"
	"impact_skill_id = 525\n";

auto IsSpace(char c) noexcept -> bool {
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Finds "key" at the start of a line and returns the text after its '='.
// Small hand rolled scan: the config has a handful of scalar keys and pulling
// in a TOML parser for that would be silly.
auto FindValue(const char* text, const char* key) noexcept -> const char* {
	if (text == nullptr || key == nullptr) {
		return nullptr;
	}

	std::size_t keyLength = 0;
	while (key[keyLength] != '\0') {
		++keyLength;
	}

	const char* line = text;
	while (*line != '\0') {
		const char* cursor = line;
		while (*cursor == ' ' || *cursor == '\t') {
			++cursor;
		}

		if (*cursor != '#') {
			std::size_t index = 0;
			while (index < keyLength && cursor[index] == key[index]) {
				++index;
			}
			if (index == keyLength) {
				const char* after = cursor + keyLength;
				while (*after == ' ' || *after == '\t') {
					++after;
				}
				if (*after == '=') {
					++after;
					while (*after == ' ' || *after == '\t') {
						++after;
					}
					return after;
				}
			}
		}

		while (*line != '\0' && *line != '\n') {
			++line;
		}
		if (*line == '\n') {
			++line;
		}
	}
	return nullptr;
}

auto ReadBool(const char* text, const char* key, bool fallback) noexcept -> bool {
	const char* value = FindValue(text, key);
	if (value == nullptr) {
		return fallback;
	}
	if (value[0] == 't' || value[0] == 'T' || value[0] == '1') {
		return true;
	}
	if (value[0] == 'f' || value[0] == 'F' || value[0] == '0') {
		return false;
	}
	return fallback;
}

auto ReadInt(const char* text, const char* key, std::int32_t fallback) noexcept -> std::int32_t {
	const char* value = FindValue(text, key);
	if (value == nullptr) {
		return fallback;
	}

	bool negative = false;
	if (*value == '-') {
		negative = true;
		++value;
	}

	std::int64_t parsed = 0;
	bool         digits = false;

	if (value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
		value += 2;
		while (true) {
			std::int32_t digit = -1;
			if (*value >= '0' && *value <= '9') {
				digit = *value - '0';
			} else if (*value >= 'a' && *value <= 'f') {
				digit = *value - 'a' + 10;
			} else if (*value >= 'A' && *value <= 'F') {
				digit = *value - 'A' + 10;
			} else if (*value == '_') {
				++value;
				continue;
			}
			if (digit < 0) {
				break;
			}
			parsed = parsed * 16 + digit;
			digits = true;
			++value;
			if (parsed > 0xFFFFFFFFLL) {
				return fallback;
			}
		}
	} else {
		while (*value >= '0' && *value <= '9') {
			parsed = parsed * 10 + (*value - '0');
			digits = true;
			++value;
			if (parsed > 0xFFFFFFFFLL) {
				return fallback;
			}
		}
	}

	if (!digits || !(IsSpace(*value) || *value == '\0' || *value == '#')) {
		return fallback;
	}

	const std::int32_t result = static_cast<std::int32_t>(static_cast<std::uint32_t>(parsed));
	return negative ? -result : result;
}

auto ReadCalcColumn(const D2RL::PluginContext* context,
                    const char*                text,
                    const char*                key,
                    std::int32_t               fallback) noexcept -> std::int32_t {
	const std::int32_t value = ReadInt(text, key, fallback);
	if (value >= 1 && value <= kCalcColumnCount) {
		return value;
	}

	char message[192]{};
	std::snprintf(message, sizeof(message),
	              "%s is %d, outside the calc1..calc%d range skills.txt has. Keeping calc%d.",
	              key, static_cast<int>(value), static_cast<int>(kCalcColumnCount), static_cast<int>(fallback));
	context->LogWarn(message);
	return fallback;
}

void LoadSettings(const D2RL::PluginContext* context) noexcept {
	if (!context->EnsureConfig(kDefaultConfigToml)) {
		context->LogWarn("Could not create the config file, using built in defaults.");
		return;
	}

	char          toml[8192]{};
	std::uint32_t requiredSize = 0;
	if (!context->ReadConfig(toml, static_cast<std::uint32_t>(sizeof(toml)), &requiredSize)) {
		context->LogWarn("Could not read the config file, using built in defaults.");
		return;
	}
	toml[sizeof(toml) - 1] = '\0';

	Settings settings{};
	settings.enabled            = ReadBool(toml, "enabled", settings.enabled);
	settings.missilesEnabled    = ReadBool(toml, "missiles_enabled", settings.missilesEnabled);
	settings.missileCountCalc   = ReadCalcColumn(context, toml, "missile_count_calc", settings.missileCountCalc);
	settings.monstersOnly       = ReadBool(toml, "missiles_monsters_only", settings.monstersOnly);
	settings.maxMissiles        = ReadInt(toml, "max_missiles", settings.maxMissiles);
	settings.damageCalc         = ReadCalcColumn(context, toml, "damage_calc", settings.damageCalc);
	settings.conversionCalc     = ReadCalcColumn(context, toml, "conversion_calc", settings.conversionCalc);
	settings.impactSkillEnabled = ReadBool(toml, "impact_skill_enabled", settings.impactSkillEnabled);
	settings.impactSkillId      = ReadInt(toml, "impact_skill_id", settings.impactSkillId);

	if (settings.maxMissiles < 0) {
		context->LogWarn("max_missiles is negative, treating it as no clamp.");
		settings.maxMissiles = 0;
	}

	g_settings = settings;
}

// --------------------------------------------------------------------------
// Plugin plumbing
// --------------------------------------------------------------------------

constexpr D2RL::PluginInfo kPluginInfo{
	.infoSize    = D2RL::PluginInfoSize,
	.apiVersion  = D2RL_PLUGIN_API_VERSION,
	.id          = "celestialrayone.charge-impact",
	.name        = "Charge Impact",
	.version     = "1.0.0",
	.author      = "CelestialRayOne",
	.description = "Charge spawns calc-driven missiles through its target on impact and runs an impact skill, with configurable calc columns.",
	.flags       = D2RL::PluginFlags::Shared | D2RL::PluginFlags::NativeHooks,
};

template <std::size_t Size>
constexpr auto ByteCount(const std::uint8_t (&)[Size]) noexcept -> std::uint32_t {
	return static_cast<std::uint32_t>(Size);
}

auto CheckSite(const D2RL::PluginContext* context,
               std::uint64_t              rva,
               const std::uint8_t*        bytes,
               std::uint32_t              size,
               const char*                what) noexcept -> bool {
	if (context->CheckExpectedBytes(rva, bytes, size)) {
		return true;
	}

	char message[224]{};
	std::snprintf(message, sizeof(message),
	              "%s at RVA 0x%08llX does not match the expected bytes - wrong game build, or another plugin got there first.",
	              what, static_cast<unsigned long long>(rva));
	context->LogError(message);
	return false;
}

// The srvdofunc table slot for Charge must point at the function this plugin
// hooks. One check covers both the table address and the handler address.
auto CheckSrvDoTable(const D2RL::PluginContext* context) noexcept -> bool {
	constexpr std::size_t kChargeSrvDoFuncIndex = 67;

	g_srvDoTable = reinterpret_cast<SrvDoFn const*>(g_moduleBase + kSrvDoTableRva);

	const auto slot     = reinterpret_cast<std::uintptr_t>(g_srvDoTable[kChargeSrvDoFuncIndex]);
	const auto expected = static_cast<std::uintptr_t>(g_moduleBase + kChargeSrvDoRva);
	if (slot == expected) {
		return true;
	}

	char message[224]{};
	std::snprintf(message, sizeof(message),
	              "srvdofunc slot 67 holds 0x%08llX, expected the Charge handler at 0x%08llX. Refusing to load.",
	              static_cast<unsigned long long>(slot - g_moduleBase),
	              static_cast<unsigned long long>(kChargeSrvDoRva));
	context->LogError(message);
	return false;
}

auto ApplyCalcRedirect(const D2RL::PluginContext* context,
                       std::uint64_t              readRva,
                       const std::uint8_t*        expected,
                       std::uint32_t              expectedSize,
                       std::int32_t               column,
                       const char*                what) noexcept -> bool {
	if (!CheckSite(context, readRva, expected, expectedSize, what)) {
		return false;
	}

	const std::uint64_t displacementRva = readRva + kCalcDisplacementOffset;
	const auto          value           = static_cast<std::uint32_t>(CalcRecordOffset(column));

	if (!context->PatchWriteU32(displacementRva,
	                            expected + kCalcDisplacementOffset,
	                            sizeof(std::uint32_t),
	                            value)) {
		char message[192]{};
		std::snprintf(message, sizeof(message), "%s could not be repointed to calc%d.", what, static_cast<int>(column));
		context->LogError(message);
		return false;
	}

	char message[192]{};
	std::snprintf(message, sizeof(message), "%s now reads calc%d (record offset 0x%03X).",
	              what, static_cast<int>(column), static_cast<unsigned int>(value));
	context->LogInfo(message);
	return true;
}

auto __cdecl StatusCommand(D2R::Game::Client*                 client,
                           const D2RL::ConsoleCommandContext* command,
                           void*                              userData) noexcept -> D2RL::ConsoleCommandResult {
	(void)client;
	(void)userData;

	if (command == nullptr || command->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}

	char message[256]{};
	std::snprintf(message, sizeof(message),
	              "charge-impact: %s, impacts %ld, missiles %ld, impact skill runs %ld.",
	              g_settings.enabled ? "enabled" : "DISABLED",
	              static_cast<long>(g_impacts),
	              static_cast<long>(g_missilesSpawned),
	              static_cast<long>(g_impactSkillRuns));
	command->plugin->WriteConsoleMessage(message);

	std::snprintf(message, sizeof(message),
	              "charge-impact: count calc%d, monsters only %s, max %d; damage calc%d %s, conversion calc%d %s.",
	              static_cast<int>(g_settings.missileCountCalc),
	              g_settings.monstersOnly ? "true" : "false",
	              static_cast<int>(g_settings.maxMissiles),
	              static_cast<int>(g_settings.damageCalc),
	              g_damagePatched ? "(patched)" : "(stock)",
	              static_cast<int>(g_settings.conversionCalc),
	              g_conversionPatched ? "(patched)" : "(stock)");
	command->plugin->WriteConsoleMessage(message);

	std::snprintf(message, sizeof(message),
	              "charge-impact: impact skill %s, id %d.",
	              g_settings.impactSkillEnabled ? "on" : "off",
	              static_cast<int>(g_settings.impactSkillId));
	command->plugin->WriteConsoleMessage(message);
	return D2RL::ConsoleCommandResult::Handled;
}

} // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
	return &kPluginInfo;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr) {
		return false;
	}

	LoadSettings(context);

	g_moduleBase = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
	if (g_moduleBase == 0) {
		context->LogError("Could not resolve the main module base.");
		return false;
	}

	// Verify every site before touching anything.
	if (!CheckSite(context, kChargeSrvDoRva, kChargeSrvDoBytes, ByteCount(kChargeSrvDoBytes), "Charge server handler")
		|| !CheckSite(context, kQueueCombatRecordRva, kQueueCombatRecordBytes, ByteCount(kQueueCombatRecordBytes), "Combat record builder")
		|| !CheckSite(context, kGetSkillsRecordRva, kGetSkillsRecordBytes, ByteCount(kGetSkillsRecordBytes), "skills.txt record lookup")
		|| !CheckSite(context, kEvalSkillFormulaRva, kEvalSkillFormulaBytes, ByteCount(kEvalSkillFormulaBytes), "Skill formula evaluator")
		|| !CheckSite(context, kProgressiveMissileRva, kProgressiveMissileBytes, ByteCount(kProgressiveMissileBytes), "Progressive missile lookup")
		|| !CheckSite(context, kMissileRecordCountRva, kMissileRecordCountBytes, ByteCount(kMissileRecordCountBytes), "missiles.txt row count")
		|| !CheckSite(context, kCreateMissileRva, kCreateMissileBytes, ByteCount(kCreateMissileBytes), "Missile creation")
		|| !CheckSite(context, kPathGetXRva, kPathGetXBytes, ByteCount(kPathGetXBytes), "Path X getter")
		|| !CheckSite(context, kPathGetYRva, kPathGetYBytes, ByteCount(kPathGetYBytes), "Path Y getter")
		|| !CheckSrvDoTable(context)) {
		return false;
	}

	g_getSkillsRecord    = reinterpret_cast<GetSkillsRecordFn>(g_moduleBase + kGetSkillsRecordRva);
	g_evalSkillFormula   = reinterpret_cast<EvalSkillFormulaFn>(g_moduleBase + kEvalSkillFormulaRva);
	g_progressiveMissile = reinterpret_cast<ProgressiveMissileFn>(g_moduleBase + kProgressiveMissileRva);
	g_missileRowCount    = reinterpret_cast<MissileRowCountFn>(g_moduleBase + kMissileRecordCountRva);
	g_createMissile      = reinterpret_cast<CreateMissileFn>(g_moduleBase + kCreateMissileRva);
	g_pathGetX           = reinterpret_cast<PathGetCoordFn>(g_moduleBase + kPathGetXRva);
	g_pathGetY           = reinterpret_cast<PathGetCoordFn>(g_moduleBase + kPathGetYRva);

	// Calc redirects first: they are plain writes, so a failure here still
	// leaves the image exactly as it was.
	if (g_settings.damageCalc != kVanillaDamageCalc) {
		if (!ApplyCalcRedirect(context,
		                       kChargeDamageCalcReadRva,
		                       kChargeDamageCalcReadBytes,
		                       ByteCount(kChargeDamageCalcReadBytes),
		                       g_settings.damageCalc,
		                       "Charge damage formula")) {
			return false;
		}
		g_damagePatched = true;
	}

	if (g_settings.conversionCalc != kVanillaConversionCalc) {
		if (!ApplyCalcRedirect(context,
		                       kChargeConversionCalcReadRva,
		                       kChargeConversionCalcReadBytes,
		                       ByteCount(kChargeConversionCalcReadBytes),
		                       g_settings.conversionCalc,
		                       "Charge elemental conversion formula")) {
			return false;
		}
		g_conversionPatched = true;
	}

	if (!context->InstallInlineHook(kChargeSrvDoRva,
	                                kChargeSrvDoBytes,
	                                ByteCount(kChargeSrvDoBytes),
	                                &HookChargeSrvDo,
	                                &g_originalChargeSrvDo)) {
		context->LogError("Failed to install the Charge handler hook.");
		return false;
	}

	// From here on the plugin never returns false: the first detour is live,
	// and returning false makes the loader unload this DLL with a hook still
	// pointing into it.
	if (!context->InstallInlineHook(kQueueCombatRecordRva,
	                                kQueueCombatRecordBytes,
	                                ByteCount(kQueueCombatRecordBytes),
	                                &HookQueueCombatRecord,
	                                &g_originalQueueCombat)) {
		context->LogError("Failed to install the combat record hook. Charge impacts will not spawn missiles.");
	}

	if (!context->RegisterConsoleCommand("charge-impact", StatusCommand, "Report charge impact state and counters.")) {
		context->LogWarn("Console command was not registered.");
	}

	char summary[256]{};
	std::snprintf(summary, sizeof(summary),
	              "Charge impact loaded: %s, count calc%d, damage calc%d, conversion calc%d, impact skill %d.",
	              g_settings.enabled ? "enabled" : "disabled in config",
	              static_cast<int>(g_settings.missileCountCalc),
	              static_cast<int>(g_settings.damageCalc),
	              static_cast<int>(g_settings.conversionCalc),
	              g_settings.impactSkillEnabled ? static_cast<int>(g_settings.impactSkillId) : -1);
	context->LogInfo(summary);
	return true;
}
