// Monster affix rework (monumod.txt unique monster mods).
//
// See celestialrayone.monster-affixes.toml for the behaviour write-up.
//
// Since 1.4.0 the plugin no longer writes the affix pointer tables. Every
// change is made in code: inline hooks on the affix dispatchers and spawn
// routines, and one-byte `ret` patches on the vanilla routines it removes.
//
//   1. SKILLS CAST WHEN STRUCK (Fire / Cold / Lightning Enchanted, Teleportation,
//      Stone Skin). SUNITDMG_FinalizeDamage 0x44A9B0 calls the "struck" affix
//      dispatcher 0x498EE0 after the get-hit mode change (0x44AD08) and on hits
//      that do not flinch or are soft hits (0x44AD39). It is hooked: the vanilla
//      dispatch runs first, then the plugin casts the configured skills for the
//      struck monster's affixes, through the native item-skill casters the game
//      uses for "chance to cast when struck" (EventFunc21 0x5837F0). Lightning's
//      vanilla bolt routine 0x4A0020 is patched to return at once when a
//      lightning skill is set.
//
//   2. FIRE / COLD ENCHANTED DEATH EFFECTS: the corpse explosion 0x49F650 and the
//      cold nova 0x49F5B0 are only reachable through their death event; both
//      start with `ret` now.
//
//   3. SPECTRAL HIT: the random-element routine 0x4A11E0 and its missile copy
//      0x4A0F80 start with `ret`. The mode-change dispatcher 0x498F80 (called by
//      D2GAME_ModeChange 0x4475C0 after the new mode is set) is hooked; on A1,
//      A2, SC and S1-S4 the plugin casts one of up to four skills, each with its
//      own localdelay lockout. A skill that is locked is not picked.
//
//   4. CURSED: the "landed a hit" dispatcher 0x4990C0 (called by
//      SUNITDMG_ExecuteEvents 0x44CE80) is replaced by the same loop, except that
//      Cursed is handled by the plugin: a random skill from the pool, cast at the
//      unit that was hit, minions included. Monsters with rank flag 800h still go
//      to whatever routine the affix table holds for Cursed (D2RLoader's own).
//
//   5. SPAWN BONUSES: Extra Strong 0x496180 is replaced, Extra Fast 0x496300 is
//      wrapped (vanilla walk speed, then attack and cast rate), Mana Burn's spawn
//      routine 0x496CE0 starts with `ret` (the drain happens per hit in
//      FinalizeDamage instead).
//
//   6. LOCKOUT: after a cast the affix is locked for the cast skill's localdelay,
//      a calc at skill record +0x238 evaluated through 0x3B5160, or for the
//      configured frame count (Teleportation). Item-style casts never apply
//      localdelay on their own (the do handler 0x43ACB0 only applies it when
//      spending resources).
//
//   7. RESISTANCE: every affix resist bonus lives in one routine, 0x38E430 (spawn
//      entry of affixes 8, 27 and 28, tail-called by the spawn entries of 9, 17,
//      18, 23 and 25). It is replaced: only Magic Resistant does anything, with
//      the new rules. Ghostly's own physical resist (0x4957F2) is untouched.
//
//   8. MAGIC RESISTANT ON MINIONS: SpawnMinions 0x49EAD0 copies the owner's
//      xfer affixes into each minion and sets rank flag 10h through 0x38E710 at
//      0x49ED68, but never runs their spawn entries. The plugin hooks 0x38E710
//      and, for that one call site (return address 0x49ED6D), runs Magic
//      Resistant on the new minion. A minion that went inactive is rebuilt by
//      0x499210, which does run the spawn entries; the resist routine accepts
//      minions there too.
//
// Built against D2R 3.3 in the D2RLoader 1.3.1 process image (base 0x140000000),
// PluginSDK 0.3.0. Every hook site and patch site is checked byte for byte before
// anything is written; nothing is installed for a feature whose check fails.

#include <D2RLPlugin/api.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace {

// ---------------------------------------------------------------------------
// Hook sites
// ---------------------------------------------------------------------------

// Affix resist routine (unit, affixId, unique). push rbx / push rdi /
// sub rsp 38h / mov ebx,edx / mov rdi,rcx: twelve bytes, no relative operands.
constexpr std::uint64_t ResistBonusRva = 0x0038E430;
constexpr std::uint8_t  ResistBonusPrologue[] { 0x40, 0x53, 0x57, 0x48, 0x83, 0xEC, 0x38, 0x8B, 0xDA, 0x48, 0x8B, 0xF9 };

// Monster rank flag setter (unit, flag, set). mov [rsp+10h],rbx /
// mov [rsp+18h],rsi / push rdi / sub rsp 20h: fifteen bytes.
constexpr std::uint64_t SetRankFlagRva = 0x0038E710;
constexpr std::uint8_t  SetRankFlagPrologue[] {
	0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20 };

// SpawnMinions 0x49EAD0, end of the per-minion loop: lea edx,[rdi+10h] /
// mov r8b,1 / mov rcx,r15 / call 0x38E710. The call returns to 0x49ED6D.
constexpr std::uint64_t MinionFlagSiteRva   = 0x0049ED5F;
constexpr std::uint64_t MinionFlagReturnRva = 0x0049ED6D;
constexpr std::uint8_t  MinionFlagSiteBytes[] {
	0x8D, 0x57, 0x10, 0x41, 0xB0, 0x01, 0x49, 0x8B, 0xCF, 0xE8, 0xA3, 0xF9, 0xEE, 0xFF };

// SUNITDMG_FinalizeDamage (game, attacker, defender, damage). push rbx, rbp,
// rdi, r14 / sub rsp 88h: thirteen bytes.
constexpr std::uint64_t FinalizeDamageRva = 0x0044A9B0;
constexpr std::uint8_t  FinalizeDamagePrologue[] { 0x40, 0x53, 0x55, 0x57, 0x41, 0x56, 0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00 };

// SUNITDMG_ExecuteEvents (game, attacker, defender, bMissile, damage). Six
// register pushes: nine bytes.
constexpr std::uint64_t ExecuteEventsRva = 0x0044CE80;
constexpr std::uint8_t  ExecuteEventsPrologue[] { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57 };

// The three affix dispatchers the plugin hooks, all (game, unit) and all with
// the same prologue: mov [rsp+20h],rbp / push rsi / push rdi / push r14 /
// sub rsp 20h, thirteen bytes.
//   0x498F80 every mode change (slot 1), from D2GAME_ModeChange
//   0x4990C0 the monster landed a hit (slot 3), from ExecuteEvents
//   0x498EE0 the monster was struck (slot 4), from FinalizeDamage
constexpr std::uint64_t ModeDispatchRva   = 0x00498F80;
constexpr std::uint64_t LandedDispatchRva = 0x004990C0;
constexpr std::uint64_t StruckDispatchRva = 0x00498EE0;
constexpr std::uint8_t  DispatchPrologue[] { 0x48, 0x89, 0x6C, 0x24, 0x20, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20 };

// Spawn routines. Extra Strong: push rbp / push rsi / sub rsp 38h /
// mov ebp,r8d / mov rsi,rcx, thirteen bytes. Extra Fast: push rdi /
// sub rsp 20h / mov rdi,rcx, nine bytes.
constexpr std::uint64_t StrongSpawnRva = 0x00496180;
constexpr std::uint8_t  StrongSpawnPrologue[] { 0x40, 0x55, 0x56, 0x48, 0x83, 0xEC, 0x38, 0x41, 0x8B, 0xE8, 0x48, 0x8B, 0xF1 };
constexpr std::uint64_t FastSpawnRva   = 0x00496300;
constexpr std::uint8_t  FastSpawnPrologue[] { 0x40, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF9 };

// Vanilla routines that start with `ret` once patched. Each one is reachable
// only for its own affix (checked: no other code calls them).
struct ReturnPatch {
	std::uint64_t       rva;
	const std::uint8_t* expected;
	const std::uint8_t* replacement;
	std::uint32_t       size;
	const char*         name;
};
constexpr std::uint8_t RetOverPushRex[]      { 0x40, 0x55 };              // push rbp (REX)
constexpr std::uint8_t RetOverPushRexRbx[]   { 0x40, 0x53 };              // push rbx (REX)
constexpr std::uint8_t RetOverTestR9[]       { 0x45, 0x85, 0xC9 };        // test r9d,r9d
constexpr std::uint8_t RetOverStoreRbx[]     { 0x48, 0x89, 0x5C, 0x24, 0x08 };  // mov [rsp+8],rbx
constexpr std::uint8_t Ret2[] { 0xC3, 0x90 };
constexpr std::uint8_t Ret3[] { 0xC3, 0x90, 0x90 };
constexpr std::uint8_t Ret5[] { 0xC3, 0x90, 0x90, 0x90, 0x90 };

constexpr ReturnPatch FireDeathPatch     { 0x0049F650, RetOverPushRex, Ret2, 2, "Fire Enchanted corpse explosion 0x49F650" };
constexpr ReturnPatch ColdDeathPatch     { 0x0049F5B0, RetOverTestR9, Ret3, 3, "Cold Enchanted death nova 0x49F5B0" };
constexpr ReturnPatch LightningBoltPatch { 0x004A0020, RetOverTestR9, Ret3, 3, "Lightning Enchanted charged bolts 0x4A0020" };
constexpr ReturnPatch SpectralElementPatch { 0x004A11E0, RetOverStoreRbx, Ret5, 5, "Spectral Hit random element 0x4A11E0" };
constexpr ReturnPatch SpectralMissilePatch { 0x004A0F80, RetOverStoreRbx, Ret5, 5, "Spectral Hit missile element 0x4A0F80" };
constexpr ReturnPatch ManaBurnSpawnPatch { 0x00496CE0, RetOverPushRexRbx, Ret2, 2, "Mana Burn spawn routine 0x496CE0" };

// Affix callback table, read only: 45 rows (monumod.txt id) x 6 pointers.
// The replacement landed-hit dispatcher reads slot 3 from it.
constexpr std::uint64_t AffixTableRva = 0x02396210;
constexpr std::uint32_t AffixSlots    = 6;
constexpr std::uint32_t SlotLandedHit = 3;

// ---------------------------------------------------------------------------
// Game functions
// ---------------------------------------------------------------------------

constexpr std::uint64_t GetUnitStatRva     = 0x002F5020;  // loader thunk, (unit, stat, layer)
constexpr std::uint64_t SetUnitStatRva     = 0x002F7D10;  // loader thunk, (unit, stat, value, layer)
constexpr std::uint64_t AddUnitStatRva     = 0x002F34F0;  // loader thunk, (unit, stat, value, layer)
constexpr std::uint64_t UnitTypeRva        = 0x0034B9D0;  // (unit)
constexpr std::uint64_t UnitIdRva          = 0x0034A330;  // (unit, file, line)
constexpr std::uint64_t UnitModeRva        = 0x0034AB60;  // (unit), mode at unit+0Ch
constexpr std::uint64_t UnitSeedRva        = 0x0034A1E0;  // (unit) -> seed pair at unit+28h
constexpr std::uint64_t UnitGameRva        = 0x0048FF00;  // (unit) -> game at unit+D8h
constexpr std::uint64_t SetUnitFlagRva     = 0x0034E140;  // (unit, flag, set)
constexpr std::uint64_t PathPointXRva      = 0x0034B8F0;  // (unit)
constexpr std::uint64_t PathPointYRva      = 0x0034B940;  // (unit)
constexpr std::uint64_t TargetUnitRva      = 0x0048FE20;  // (game, unit)
constexpr std::uint64_t MissileOwnerRva    = 0x00490300;  // (game, missile), may return null
constexpr std::uint64_t AffixListRva       = 0x0038E310;  // (unit) -> 9 affix bytes
constexpr std::uint64_t HasRankFlagRva     = 0x0038E870;  // (unit, mask) -> uint8
constexpr std::uint64_t AffixConstantRva   = 0x00498E10;  // (dataContext, row) -> monumod.txt constants
constexpr std::uint64_t SkillsRecordRva    = 0x00097790;  // (dataContext, skillId)
constexpr std::uint64_t EvaluateRva        = 0x003B5160;  // (dataContext, unit, calc, skillId, level)
constexpr std::uint64_t CastOnTargetRva    = 0x005896E0;  // (caster, skillId, level, target, flag)
constexpr std::uint64_t CastAtPositionRva  = 0x00589820;  // (caster, skillId, level, x, y, flag)

// Entry witnesses for the helpers whose bytes are stable. The two casters are
// not witnessed: other plugins (whirlwind-follow-cursor) hook their entries.
constexpr std::uint8_t LoaderThunkBytes[] { 0xFF, 0x25 };
constexpr std::uint8_t UnitTypeBytes[] {
	0x48, 0x83, 0xEC, 0x28, 0x48, 0x85, 0xC9, 0x75, 0x1D, 0x88, 0x4C, 0x24, 0x30, 0x48, 0x8D, 0x4C };
constexpr std::uint8_t UnitIdBytes[] {
	0x48, 0x83, 0xEC, 0x28, 0x48, 0x85, 0xC9, 0x75, 0x1D, 0x88, 0x4C, 0x24, 0x30, 0x48, 0x8D, 0x4C, 0x24, 0x30 };
constexpr std::uint8_t MissileOwnerBytes[] {
	0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xFA, 0x48, 0x8B, 0xF1,
	0x48, 0x85, 0xD2, 0x75, 0x20 };
constexpr std::uint8_t SkillsRecordBytes[] {
	0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x63, 0xF2 };
constexpr std::uint8_t EvaluateBytes[] {
	0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57,
	0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0xB0, 0x00, 0x00, 0x00 };

using GetUnitStatFn    = std::int32_t(__fastcall*)(void* unit, std::int32_t stat, std::int32_t layer) noexcept;
using SetUnitStatFn    = std::uint8_t(__fastcall*)(void* unit, std::int32_t stat, std::int32_t value, std::int32_t layer) noexcept;
using AddUnitStatFn    = std::uint8_t(__fastcall*)(void* unit, std::int32_t stat, std::int32_t value, std::int32_t layer) noexcept;
using UnitTypeFn       = std::uint32_t(__fastcall*)(void* unit) noexcept;
using UnitIdFn         = std::uint32_t(__fastcall*)(void* unit, const char* file, std::int32_t line) noexcept;
using UnitModeFn       = std::uint32_t(__fastcall*)(void* unit) noexcept;
using UnitSeedFn       = std::uint32_t*(__fastcall*)(void* unit) noexcept;
using UnitGameFn       = void*(__fastcall*)(void* unit) noexcept;
using SetUnitFlagFn    = void(__fastcall*)(void* unit, std::uint32_t flag, std::int32_t set) noexcept;
using PathPointFn      = std::int32_t(__fastcall*)(void* unit) noexcept;
using TargetUnitFn     = void*(__fastcall*)(void* game, void* unit) noexcept;
using MissileOwnerFn   = void*(__fastcall*)(void* game, void* missile) noexcept;
using AffixListFn      = const std::uint8_t*(__fastcall*)(void* unit) noexcept;
using HasRankFlagFn    = std::uint8_t(__fastcall*)(void* unit, std::uint32_t mask) noexcept;
using AffixConstantFn  = std::int32_t(__fastcall*)(std::uint8_t context, std::uint32_t row) noexcept;
using SkillsRecordFn   = const std::uint8_t*(__fastcall*)(std::uint8_t context, std::int32_t skillId) noexcept;
using EvaluateFn       = std::int32_t(__fastcall*)(std::uint8_t context, void* unit, std::uint32_t calc,
                                                   std::int32_t skillId, std::int32_t level) noexcept;
using CastOnTargetFn   = std::int32_t(__fastcall*)(void* caster, std::int32_t skillId, std::int32_t level,
                                                   void* target, std::int32_t flag) noexcept;
using CastAtPositionFn = std::int32_t(__fastcall*)(void* caster, std::int32_t skillId, std::int32_t level,
                                                   std::int32_t x, std::int32_t y, std::int32_t flag) noexcept;
using FinalizeDamageFn = void(__fastcall*)(void* game, void* attacker, void* defender, void* damage) noexcept;
using ExecuteEventsFn  = void(__fastcall*)(void* game, void* attacker, void* defender, std::int32_t missile,
                                           void* damage) noexcept;
using SetRankFlagFn    = std::uint64_t(__fastcall*)(void* unit, std::uint32_t flag, std::uint32_t set) noexcept;
using DispatchFn       = std::uint64_t(__fastcall*)(void* game, void* unit) noexcept;
using AffixCallbackFn  = std::uint64_t(__fastcall*)(void* game, void* unit, std::uint64_t affix, std::uint64_t unique) noexcept;
using SpawnFn          = void(__fastcall*)(void* unit, std::int32_t affix, std::int32_t unique) noexcept;

GetUnitStatFn    GetUnitStat    = nullptr;
SetUnitStatFn    SetUnitStat    = nullptr;
AddUnitStatFn    AddUnitStat    = nullptr;
UnitTypeFn       UnitType       = nullptr;
UnitIdFn         UnitId         = nullptr;
UnitModeFn       UnitMode       = nullptr;
UnitSeedFn       UnitSeed       = nullptr;
UnitGameFn       UnitGame       = nullptr;
SetUnitFlagFn    SetUnitFlag    = nullptr;
PathPointFn      PathPointX     = nullptr;
PathPointFn      PathPointY     = nullptr;
TargetUnitFn     TargetUnit     = nullptr;
MissileOwnerFn   MissileOwner   = nullptr;
AffixListFn      AffixList      = nullptr;
HasRankFlagFn    HasRankFlag    = nullptr;
AffixConstantFn  AffixConstant  = nullptr;
SkillsRecordFn   SkillsRecord   = nullptr;
EvaluateFn       Evaluate       = nullptr;
CastOnTargetFn   CastOnTarget   = nullptr;
CastAtPositionFn CastAtPosition = nullptr;

FinalizeDamageFn OriginalFinalizeDamage = nullptr;
ExecuteEventsFn  OriginalExecuteEvents  = nullptr;
SetRankFlagFn    OriginalSetRankFlag    = nullptr;
DispatchFn       OriginalModeDispatch   = nullptr;
DispatchFn       OriginalStruckDispatch = nullptr;
SpawnFn          OriginalFastSpawn      = nullptr;

// ---------------------------------------------------------------------------
// Layouts and ids
// ---------------------------------------------------------------------------

constexpr std::size_t UnitMonsterDataOffset  = 0x10;   // Unit+10h -> MonsterData
constexpr std::size_t MonsterRankFlagsOffset = 0x1A;   // MonsterData+1Ah, uint16
constexpr std::size_t GameDifficultyOffset   = 0x104;  // Game+104h, uint8
constexpr std::size_t GameDataContextOffset  = 0x106;  // Game+106h, uint8
constexpr std::size_t GameFrameOffset        = 0x170;  // Game+170h, int32
constexpr std::size_t DamageResultFlags      = 0x04;   // D2Damage+4, result flags
constexpr std::size_t SkillLocalDelayCalc    = 0x238;  // skills.txt localdelay calc, +568
constexpr std::size_t AffixListLength        = 9;

constexpr std::uint32_t UnitTypePlayer   = 0;
constexpr std::uint32_t UnitTypeMonster  = 1;
constexpr std::uint32_t UnitTypeMissile  = 3;
constexpr std::uint32_t UnitTypeItem     = 4;
constexpr std::uint16_t ExcludedRanks    = 0x0410;      // minion 10h and 400h, as the vanilla routines test
constexpr std::uint16_t MinionRank       = 0x0010;
constexpr std::uint16_t UniqueRank       = 0x0008;
constexpr std::uint16_t Rank400          = 0x0400;      // vanilla's other exclusion, kept
constexpr std::uint16_t MagicResistExcludedRanks = Rank400;
constexpr std::uint16_t CursedSpecialRank = 0x0800;     // D2RLoader's Cursed routine treats these monsters apart
constexpr std::uint32_t ItemCastFlag     = 0x10000000;  // set around the cast exactly as EventFunc21 does
constexpr std::uint16_t ResultSuccessfulHit = 0x0001;

constexpr std::int32_t AffixExtraStrong        = 5;
constexpr std::int32_t AffixExtraFast          = 6;
constexpr std::int32_t AffixCursed             = 7;
constexpr std::int32_t AffixMagicResistant     = 8;
constexpr std::int32_t AffixFireEnchanted      = 9;
constexpr std::int32_t AffixLightningEnchanted = 17;
constexpr std::int32_t AffixColdEnchanted      = 18;
constexpr std::int32_t AffixManaBurn           = 25;
constexpr std::int32_t AffixTeleportation      = 26;
constexpr std::int32_t AffixSpectralHit        = 27;
constexpr std::int32_t AffixStoneSkin          = 28;

constexpr std::int32_t StatMana          = 8;
constexpr std::int32_t StatMaxMana       = 9;
constexpr std::int32_t StatLevel         = 12;
constexpr std::int32_t StatDamagePercent = 25;
constexpr std::int32_t StatAttackRate    = 68;

// Spell damage: passive_fire/ltng/cold/pois_mastery, passive_mag2_mastery,
// passive_wind_mastery.
constexpr std::array<std::int32_t, 6> SpellMasteryStats { 329, 330, 331, 332, 482, 483 };

// monumod.txt constants row D2RLoader's Cursed routine rolls its proc chance on.
constexpr std::uint32_t CursedChanceRow = 34;
// Amplify Damage, the skill the stock Cursed routine casts. Used when
// cursed_skills is empty.
constexpr std::int32_t  StockCurseSkill = 66;

// Monster animation modes that start an attack or a cast: A1, A2, SC, S1-S4.
constexpr std::uint32_t ModeAttack1 = 4;
constexpr std::uint32_t ModeAttack2 = 5;
constexpr std::uint32_t ModeCast    = 7;
constexpr std::uint32_t ModeSkill1  = 8;
constexpr std::uint32_t ModeSkill4  = 11;

constexpr std::int32_t  NoSkill         = -1;
constexpr std::int32_t  MaxSkillId      = 0xFFFE;
constexpr std::int32_t  UseLocalDelay   = -1;
constexpr std::int32_t  UseMonsterLevel = 0;
constexpr std::uint32_t WholeAffix      = 0xFFFF;   // lockout shared by every skill of the affix
constexpr std::size_t   MaxCursedSkills = 16;

// Magic Resistant: +50 to each resist, capped at 95. What the cap leaves unused
// is converted: one point per two for absorb / dodge-avoid-evade, one per one
// for poison length reduction. A resist already at 95 or more converts all 50.
constexpr std::int32_t MagicResistBoost = 50;
constexpr std::int32_t MagicResistCap   = 95;

struct ResistConversion {
	std::int32_t                resistStat;
	std::array<std::int32_t, 3> outputs;
	std::int32_t                outputCount;
	std::int32_t                pointsPerBonus;
};

constexpr std::array<ResistConversion, 6> MagicResistTable { {
	{ 39, { 142, 0, 0 }, 1, 2 },        // fireresist      -> item_absorbfire_percent
	{ 43, { 148, 0, 0 }, 1, 2 },        // coldresist      -> item_absorbcold_percent
	{ 41, { 144, 0, 0 }, 1, 2 },        // lightresist     -> item_absorblight_percent
	{ 37, { 146, 0, 0 }, 1, 2 },        // magicresist     -> item_absorbmagic_percent
	{ 36, { 338, 339, 340 }, 3, 2 },    // damageresist    -> passive_dodge, passive_avoid, passive_evade
	{ 45, { 110, 0, 0 }, 1, 1 },        // poisonresist    -> item_poisonlengthresist
} };

// ---------------------------------------------------------------------------
// Settings and state
// ---------------------------------------------------------------------------

struct Settings {
	std::int32_t                fireSkill      = NoSkill;
	std::int32_t                coldSkill      = NoSkill;
	std::int32_t                lightningSkill = NoSkill;
	std::int32_t                stoneSkinSkill = NoSkill;
	std::array<std::int32_t, 4> spectralSkills { NoSkill, NoSkill, NoSkill, NoSkill };

	std::int32_t strongDamagePercent      = 150;
	std::int32_t strongSpellDamagePercent = 100;

	std::array<std::int32_t, 3> fastAttackRate { 25, 50, 75 };
	std::array<std::int32_t, 3> fastCastRate { 25, 50, 75 };
	std::int32_t                fastCastRateStat = 105;

	std::array<std::int32_t, MaxCursedSkills> cursedSkills {};
	std::uint32_t                             cursedCount = 0;

	std::int32_t manaBurnPercent = 10;

	std::int32_t teleportSkill          = 184;
	std::int32_t teleportChance         = 25;
	std::int32_t teleportCooldownFrames = 25;
};

Settings Config {};
bool     ConfigFileRead = false;

std::array<std::int32_t, 4> SpectralPool {};
std::uint32_t               SpectralPoolSize = 0;

const D2RL::PluginContext* Context = nullptr;
std::uintptr_t             Base    = 0;

bool ResistRoutineInstalled = false;
bool MinionMagicResist      = false;
bool StruckCaptureInstalled = false;
bool HitCaptureInstalled    = false;
bool StruckHookInstalled    = false;
bool ModeHookInstalled      = false;
bool LandedHookInstalled    = false;
bool StrongHookInstalled    = false;
bool FastHookInstalled      = false;
bool FireDeathPatched       = false;
bool ColdDeathPatched       = false;
bool LightningBoltsPatched  = false;
bool SpectralElementPatched = false;
bool SpectralMissilePatched = false;
bool ManaBurnPatched        = false;

// What actually runs, decided once at load from the settings and what got
// installed.
bool FireActive      = false;
bool ColdActive      = false;
bool LightningActive = false;
bool StoneSkinActive = false;
bool TeleportActive  = false;
bool SpectralActive  = false;
bool CursedActive    = false;
bool ManaBurnActive  = false;

std::array<std::int32_t, MaxCursedSkills> CursedPool {};
std::uint32_t                             CursedPoolSize = 0;

struct AffixCounters {
	std::atomic<std::uint64_t> casts { 0 };
	std::atomic<std::uint64_t> lockedOut { 0 };
	std::atomic<std::uint64_t> missingSkill { 0 };
};

AffixCounters FireCounters;
AffixCounters ColdCounters;
AffixCounters LightningCounters;
AffixCounters SpectralCounters;
AffixCounters CursedCounters;
AffixCounters TeleportCounters;
AffixCounters StoneSkinCounters;
std::atomic<std::uint64_t> MagicResistApplied { 0 };
std::atomic<std::uint64_t> MinionMagicResistApplied { 0 };
std::atomic<std::uint64_t> CursedDelegated { 0 };
std::atomic<std::uint64_t> CursedMinionCasts { 0 };
std::atomic<std::uint64_t> SpectralRerolls { 0 };
std::atomic<std::uint64_t> ManaBurnHits { 0 };

// The struck and landed-hit dispatchers do not receive the other unit;
// FinalizeDamage and ExecuteEvents do. Saved and restored around every call,
// so nested damage keeps its own pair.
thread_local void* HitAttacker = nullptr;
thread_local void* HitDefender = nullptr;

// A skill cast by an affix must not trigger another affix cast synchronously.
thread_local std::int32_t AffixCastDepth = 0;

struct LockoutEntry {
	void*        game;
	std::int32_t setFrame;
	std::int32_t expireFrame;
};

std::mutex                                      LockoutMutex;
std::unordered_map<std::uint64_t, LockoutEntry> Lockouts;
constexpr std::size_t                           LockoutPruneSize = 4096;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

template <typename T>
auto At(std::uint64_t rva) noexcept -> T {
	return reinterpret_cast<T>(Base + rva);
}

template <std::size_t Size>
constexpr auto ByteCount(const std::uint8_t (&)[Size]) noexcept -> std::uint32_t {
	return static_cast<std::uint32_t>(Size);
}

template <typename T>
auto ReadField(const void* base, std::size_t offset) noexcept -> T {
	T value {};
	std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(T));
	return value;
}

void LogInfo(const char* message) noexcept {
	if (Context != nullptr) Context->LogInfo(message);
}

void LogWarn(const char* message) noexcept {
	if (Context != nullptr) Context->LogWarn(message);
}

void LogError(const char* message) noexcept {
	if (Context != nullptr) Context->LogError(message);
}

auto RankFlags(void* unit) noexcept -> std::uint16_t {
	const void* monsterData = ReadField<void*>(unit, UnitMonsterDataOffset);
	return monsterData != nullptr ? ReadField<std::uint16_t>(monsterData, MonsterRankFlagsOffset) : 0;
}

auto HasAffix(void* unit, std::int32_t affix) noexcept -> bool {
	const std::uint8_t* list = AffixList(unit);
	if (list == nullptr) return false;
	for (std::size_t i = 0; i < AffixListLength && list[i] != 0; ++i) {
		if (list[i] == affix) return true;
	}
	return false;
}

// Same LCG step as the game's own rolls on the unit seed (EventFunc21).
auto NextRoll(void* unit) noexcept -> std::uint32_t {
	std::uint32_t* seed = UnitSeed(unit);
	if (seed == nullptr) return 0;
	const std::uint64_t next = static_cast<std::uint64_t>(seed[1]) + 0x6AC690C5ULL * static_cast<std::uint64_t>(seed[0]);
	std::memcpy(seed, &next, sizeof(next));
	return static_cast<std::uint32_t>(next);
}

auto IsAttackOrCastMode(std::uint32_t mode) noexcept -> bool {
	return mode == ModeAttack1 || mode == ModeAttack2 || mode == ModeCast
	    || (mode >= ModeSkill1 && mode <= ModeSkill4);
}

auto CountersFor(std::int32_t affix) noexcept -> AffixCounters* {
	switch (affix) {
	case AffixFireEnchanted: return &FireCounters;
	case AffixColdEnchanted: return &ColdCounters;
	case AffixLightningEnchanted: return &LightningCounters;
	case AffixSpectralHit: return &SpectralCounters;
	case AffixCursed: return &CursedCounters;
	case AffixTeleportation: return &TeleportCounters;
	case AffixStoneSkin: return &StoneSkinCounters;
	default: return nullptr;
	}
}

auto GameFrame(void* game) noexcept -> std::int32_t {
	return ReadField<std::int32_t>(game, GameFrameOffset);
}

// ---------------------------------------------------------------------------
// Lockout, keyed by game, monster GUID, affix and (Spectral Hit) skill
// ---------------------------------------------------------------------------

auto LockoutKey(std::uint32_t guid, std::int32_t affix, std::uint32_t sub) noexcept -> std::uint64_t {
	return (static_cast<std::uint64_t>(guid) << 24) | (static_cast<std::uint64_t>(affix & 0xFF) << 16)
	     | static_cast<std::uint64_t>(sub & 0xFFFF);
}

auto IsLockedOut(void* game, std::uint32_t guid, std::int32_t affix, std::uint32_t sub, std::int32_t frame) noexcept
	-> bool {
	const std::lock_guard<std::mutex> guard(LockoutMutex);
	const auto it = Lockouts.find(LockoutKey(guid, affix, sub));
	if (it == Lockouts.end()) return false;

	const LockoutEntry& entry = it->second;
	// A different game, or a frame counter that went backwards, means the entry
	// belongs to a game that has ended. It never blocks.
	if (entry.game != game || frame < entry.setFrame) {
		Lockouts.erase(it);
		return false;
	}
	return frame < entry.expireFrame;
}

void SetLockout(void* game, std::uint32_t guid, std::int32_t affix, std::uint32_t sub, std::int32_t frame,
                std::int32_t frames) noexcept {
	try {
		const std::lock_guard<std::mutex> guard(LockoutMutex);
		if (Lockouts.size() >= LockoutPruneSize) {
			for (auto it = Lockouts.begin(); it != Lockouts.end();) {
				const LockoutEntry& entry = it->second;
				if (entry.game != game || frame < entry.setFrame || frame >= entry.expireFrame) {
					it = Lockouts.erase(it);
				} else {
					++it;
				}
			}
		}
		Lockouts[LockoutKey(guid, affix, sub)] = LockoutEntry { game, frame, frame + frames };
	} catch (...) {
		// Out of memory: the cast already happened, only its lockout is lost.
	}
}

auto LockoutCount() noexcept -> std::size_t {
	const std::lock_guard<std::mutex> guard(LockoutMutex);
	return Lockouts.size();
}

// ---------------------------------------------------------------------------
// The cast, shared by every affix
// ---------------------------------------------------------------------------

// Casts skillId for the monster the way EventFunc21 casts a "when struck" item
// skill: flag 10000000h on, unit target when there is one, path point when
// there is not, flag off. The lockout starts with the cast and lasts the
// skill's localdelay (evaluated for this monster at the cast level) unless a
// fixed frame count is given. lockoutSub is WholeAffix, or the skill id for a
// per-skill lockout.
void CastAffixSkill(void* game, void* unit, std::int32_t affix, std::int32_t skillId, void* target, std::int32_t level,
                    std::int32_t fixedLockout, std::uint32_t lockoutSub) noexcept {
	AffixCounters* counters = CountersFor(affix);

	const std::int32_t  frame = GameFrame(game);
	const std::uint32_t guid  = UnitId(unit, __FILE__, __LINE__);
	if (IsLockedOut(game, guid, affix, lockoutSub, frame)) {
		if (counters != nullptr) counters->lockedOut.fetch_add(1, std::memory_order_relaxed);
		return;
	}

	const std::uint8_t  context = ReadField<std::uint8_t>(game, GameDataContextOffset);
	const std::uint8_t* record  = SkillsRecord(context, skillId);
	if (record == nullptr) {
		if (counters != nullptr) counters->missingSkill.fetch_add(1, std::memory_order_relaxed);
		return;
	}

	if (level == UseMonsterLevel) level = GetUnitStat(unit, StatLevel, 0);
	if (level < 1) level = 1;

	std::int32_t lockout = fixedLockout;
	if (lockout == UseLocalDelay) {
		lockout = Evaluate(context, unit, ReadField<std::uint32_t>(record, SkillLocalDelayCalc), skillId, level);
	}

	if (target != nullptr) {
		const std::uint32_t targetType = UnitType(target);
		if (target == unit || targetType == UnitTypeMissile || targetType == UnitTypeItem) {
			target = nullptr;
		}
	}

	++AffixCastDepth;
	SetUnitFlag(unit, ItemCastFlag, 1);
	if (target != nullptr) {
		CastOnTarget(unit, skillId, level, target, 0);
	} else {
		const std::int32_t y = PathPointY(unit);
		const std::int32_t x = PathPointX(unit);
		CastAtPosition(unit, skillId, level, x, y, 0);
	}
	SetUnitFlag(unit, ItemCastFlag, 0);
	--AffixCastDepth;

	if (counters != nullptr) counters->casts.fetch_add(1, std::memory_order_relaxed);
	if (lockout > 0) SetLockout(game, guid, affix, lockoutSub, frame, lockout);
}

// ---------------------------------------------------------------------------
// Affix behaviour
// ---------------------------------------------------------------------------

// Struck: Fire / Cold / Lightning Enchanted, Stone Skin, Teleportation.
// Uniques and superuniques only, like vanilla Lightning Enchanted.
void ProcessStruck(void* game, void* unit) noexcept {
	if (game == nullptr || unit == nullptr || AffixCastDepth > 0) return;
	if (HasRankFlag(unit, UniqueRank) == 0) return;

	const std::uint8_t* list = AffixList(unit);
	if (list == nullptr) return;
	void* attacker = HitDefender == unit ? HitAttacker : nullptr;

	for (std::size_t i = 0; i < AffixListLength && list[i] != 0; ++i) {
		const std::int32_t affix = list[i];
		switch (affix) {
		case AffixFireEnchanted:
			if (FireActive) {
				CastAffixSkill(game, unit, affix, Config.fireSkill, attacker, UseMonsterLevel, UseLocalDelay, WholeAffix);
			}
			break;
		case AffixColdEnchanted:
			if (ColdActive) {
				CastAffixSkill(game, unit, affix, Config.coldSkill, attacker, UseMonsterLevel, UseLocalDelay, WholeAffix);
			}
			break;
		case AffixLightningEnchanted:
			if (LightningActive) {
				CastAffixSkill(game, unit, affix, Config.lightningSkill, attacker, UseMonsterLevel, UseLocalDelay,
				               WholeAffix);
			}
			break;
		case AffixStoneSkin:
			if (StoneSkinActive) {
				CastAffixSkill(game, unit, affix, Config.stoneSkinSkill, attacker, UseMonsterLevel, UseLocalDelay,
				               WholeAffix);
			}
			break;
		case AffixTeleportation: {
			if (!TeleportActive) break;
			// Roll only while the cooldown is over, so a blocked hit leaves the seed alone.
			const std::uint32_t guid = UnitId(unit, __FILE__, __LINE__);
			if (IsLockedOut(game, guid, affix, WholeAffix, GameFrame(game))) {
				TeleportCounters.lockedOut.fetch_add(1, std::memory_order_relaxed);
				break;
			}
			if (static_cast<std::int32_t>(NextRoll(unit) % 100U) >= Config.teleportChance) break;
			CastAffixSkill(game, unit, affix, Config.teleportSkill, attacker, UseMonsterLevel,
			               Config.teleportCooldownFrames, WholeAffix);
			break;
		}
		default: break;
		}
	}
}

// Spectral Hit, at the start of A1, A2, SC and S1-S4. Same eligibility as the
// vanilla routine: uniques, or anything that is not a minion. Every skill has
// its own lockout; a locked skill is never picked, and nothing is cast when all
// of them are locked.
void ProcessSpectral(void* game, void* unit) noexcept {
	if (game == nullptr || unit == nullptr || AffixCastDepth > 0) return;
	if (!IsAttackOrCastMode(UnitMode(unit))) return;
	if (!HasAffix(unit, AffixSpectralHit)) return;
	if (HasRankFlag(unit, UniqueRank) == 0 && (RankFlags(unit) & ExcludedRanks) != 0) return;

	const std::int32_t  frame = GameFrame(game);
	const std::uint32_t guid  = UnitId(unit, __FILE__, __LINE__);

	std::array<std::int32_t, 4> ready {};
	std::uint32_t               readyCount = 0;
	for (std::uint32_t i = 0; i < SpectralPoolSize; ++i) {
		const std::int32_t skill = SpectralPool[i];
		if (!IsLockedOut(game, guid, AffixSpectralHit, static_cast<std::uint32_t>(skill), frame)) {
			ready[readyCount++] = skill;
		}
	}
	if (readyCount == 0) {
		SpectralCounters.lockedOut.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	if (readyCount < SpectralPoolSize) SpectralRerolls.fetch_add(1, std::memory_order_relaxed);

	const std::uint32_t index = readyCount > 1 ? NextRoll(unit) % readyCount : 0;
	const std::int32_t  skill = ready[index];
	CastAffixSkill(game, unit, AffixSpectralHit, skill, TargetUnit(game, unit), UseMonsterLevel, UseLocalDelay,
	               static_cast<std::uint32_t>(skill));
}

// Cursed, when the monster lands a hit. Monsters with rank flag 800h go to the
// routine the affix table holds (D2RLoader's own Amplify Damage routine).
// Everyone else, minions included, follows that routine's rules (chance from
// monumod.txt constants row 34, level = monster level / 5 + 1) but casts a
// random skill from the pool at the unit that was hit. Units with rank flag
// 400h are skipped, as vanilla skips them.
void ProcessCursed(void* game, void* unit, std::uint64_t affix, std::uint64_t unique) noexcept {
	const std::uint16_t ranks = RankFlags(unit);
	if ((ranks & CursedSpecialRank) != 0) {
		CursedDelegated.fetch_add(1, std::memory_order_relaxed);
		const std::uint64_t slot = ReadField<std::uint64_t>(
			reinterpret_cast<const void*>(Base),
			AffixTableRva + (static_cast<std::uint64_t>(AffixCursed) * AffixSlots + SlotLandedHit) * sizeof(std::uint64_t));
		if (slot != 0) reinterpret_cast<AffixCallbackFn>(static_cast<std::uintptr_t>(slot))(game, unit, affix, unique);
		return;
	}
	if ((ranks & Rank400) != 0 || AffixCastDepth > 0 || CursedPoolSize == 0) return;

	const std::uint8_t context = ReadField<std::uint8_t>(game, GameDataContextOffset);
	const std::int32_t chance  = AffixConstant(context, CursedChanceRow);
	if (static_cast<std::int32_t>(NextRoll(unit) % 100U) >= chance) return;

	const std::uint32_t index = CursedPoolSize > 1 ? NextRoll(unit) % CursedPoolSize : 0;
	const std::int32_t  level = GetUnitStat(unit, StatLevel, 0) / 5 + 1;
	void* target = HitAttacker == unit && HitDefender != nullptr ? HitDefender : TargetUnit(game, unit);
	if ((ranks & MinionRank) != 0) CursedMinionCasts.fetch_add(1, std::memory_order_relaxed);
	CastAffixSkill(game, unit, AffixCursed, CursedPool[index], target, level, UseLocalDelay, WholeAffix);
}

void ApplyManaBurn(void* game, void* attacker, void* defender) noexcept {
	if (game == nullptr || attacker == nullptr || defender == nullptr) return;
	if (UnitType(defender) != UnitTypePlayer) return;

	void* source = attacker;
	if (UnitType(source) == UnitTypeMissile) source = MissileOwner(game, source);
	if (source == nullptr || UnitType(source) != UnitTypeMonster || !HasAffix(source, AffixManaBurn)) return;

	const std::int64_t maxMana = GetUnitStat(defender, StatMaxMana, 0);
	if (maxMana <= 0) return;
	const std::int64_t mana = GetUnitStat(defender, StatMana, 0);
	if (mana <= 0) return;

	std::int64_t left = mana - maxMana * Config.manaBurnPercent / 100;
	if (left < 0) left = 0;
	SetUnitStat(defender, StatMana, static_cast<std::int32_t>(left), 0);
	ManaBurnHits.fetch_add(1, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

auto LiveQword(std::uint64_t rva) noexcept -> std::uint64_t {
	return ReadField<std::uint64_t>(reinterpret_cast<const void*>(Base), rva);
}

auto SlotRva(std::uint64_t affix, std::uint32_t slot) noexcept -> std::uint64_t {
	return AffixTableRva + (affix * AffixSlots + slot) * sizeof(std::uint64_t);
}

void __fastcall HookFinalizeDamage(void* game, void* attacker, void* defender, void* damage) noexcept {
	void* const savedAttacker = HitAttacker;
	void* const savedDefender = HitDefender;
	HitAttacker = attacker;
	HitDefender = defender;
	OriginalFinalizeDamage(game, attacker, defender, damage);
	HitAttacker = savedAttacker;
	HitDefender = savedDefender;

	if (ManaBurnActive && damage != nullptr
	    && (ReadField<std::uint16_t>(damage, DamageResultFlags) & ResultSuccessfulHit) != 0) {
		ApplyManaBurn(game, attacker, defender);
	}
}

void __fastcall HookExecuteEvents(void* game, void* attacker, void* defender, std::int32_t missile, void* damage) noexcept {
	void* const savedAttacker = HitAttacker;
	void* const savedDefender = HitDefender;
	HitAttacker = attacker;
	HitDefender = defender;
	OriginalExecuteEvents(game, attacker, defender, missile, damage);
	HitAttacker = savedAttacker;
	HitDefender = savedDefender;
}

// 0x498EE0, the struck dispatcher: the vanilla affix callbacks first, then the
// plugin's when-struck skills.
std::uint64_t __fastcall HookStruckDispatch(void* game, void* unit) noexcept {
	const std::uint64_t result = OriginalStruckDispatch(game, unit);
	ProcessStruck(game, unit);
	return result;
}

// 0x498F80, the mode-change dispatcher: the vanilla affix callbacks first (the
// new mode is already set), then Spectral Hit.
std::uint64_t __fastcall HookModeDispatch(void* game, void* unit) noexcept {
	const std::uint64_t result = OriginalModeDispatch(game, unit);
	if (SpectralActive) ProcessSpectral(game, unit);
	return result;
}

// Replaces 0x4990C0, the landed-hit dispatcher. Same loop as the original
// (unique flag from rank 8h, all nine affix bytes, slot 3 of each row, null
// slots skipped) except that Cursed is handled by the plugin.
std::uint64_t __fastcall HookLandedDispatch(void* game, void* unit) noexcept {
	const std::uint64_t unique = HasRankFlag(unit, UniqueRank);
	const std::uint8_t* list   = AffixList(unit);
	if (list == nullptr || list[0] == 0) return 0;

	for (std::size_t i = 0; i < AffixListLength; ++i) {
		const std::uint64_t affix = list[i];
		if (affix == static_cast<std::uint64_t>(AffixCursed) && CursedActive) {
			ProcessCursed(game, unit, affix, unique);
			continue;
		}
		const std::uint64_t slot = LiveQword(SlotRva(affix, SlotLandedHit));
		if (slot != 0) reinterpret_cast<AffixCallbackFn>(static_cast<std::uintptr_t>(slot))(game, unit, affix, unique);
	}
	return 0;
}

// 0x496180, Extra Strong at spawn: flat physical and spell damage, no attack
// rating. Replaces the vanilla routine.
void __fastcall HookStrongSpawn(void* unit, std::int32_t, std::int32_t) noexcept {
	if (unit == nullptr || UnitType(unit) != UnitTypeMonster) return;
	if (Config.strongDamagePercent != 0) AddUnitStat(unit, StatDamagePercent, Config.strongDamagePercent, 0);
	if (Config.strongSpellDamagePercent != 0) {
		for (const std::int32_t stat : SpellMasteryStats) AddUnitStat(unit, stat, Config.strongSpellDamagePercent, 0);
	}
}

// 0x496300, Extra Fast at spawn: the vanilla walk speed routine, then attack
// and cast rate by difficulty.
void __fastcall HookFastSpawn(void* unit, std::int32_t affix, std::int32_t unique) noexcept {
	OriginalFastSpawn(unit, affix, unique);
	if (unit == nullptr || UnitType(unit) != UnitTypeMonster) return;

	void* game = UnitGame(unit);
	if (game == nullptr) return;
	std::uint32_t difficulty = ReadField<std::uint8_t>(game, GameDifficultyOffset);
	if (difficulty > 2) difficulty = 2;

	const std::int32_t attackRate = Config.fastAttackRate[difficulty];
	const std::int32_t castRate   = Config.fastCastRate[difficulty];
	if (attackRate != 0) AddUnitStat(unit, StatAttackRate, attackRate, 0);
	if (castRate != 0) AddUnitStat(unit, Config.fastCastRateStat, castRate, 0);
}

void ApplyMagicResistant(void* unit) noexcept {
	for (const ResistConversion& entry : MagicResistTable) {
		const std::int32_t current = GetUnitStat(unit, entry.resistStat, 0);

		std::int32_t gain = 0;
		if (current < MagicResistCap) {
			gain = MagicResistCap - current;
			if (gain > MagicResistBoost) gain = MagicResistBoost;
		}
		if (gain > 0) AddUnitStat(unit, entry.resistStat, gain, 0);

		const std::int32_t bonus = (MagicResistBoost - gain) / entry.pointsPerBonus;
		if (bonus <= 0) continue;
		for (std::int32_t i = 0; i < entry.outputCount; ++i) {
			AddUnitStat(unit, entry.outputs[static_cast<std::size_t>(i)], bonus, 0);
		}
	}
	MagicResistApplied.fetch_add(1, std::memory_order_relaxed);
}

// Replaces the affix resist routine. Only Magic Resistant does anything;
// Stone Skin no longer doubles defense. Vanilla skipped units with rank flag
// 10h (minion) or 400h. 400h stays skipped; minions get Magic Resistant too
// once the minion spawn hook is in place (otherwise they stay excluded, so
// minions are never treated two different ways). A missing MonsterData
// passes, as it does in the vanilla routine.
void __fastcall HookResistBonus(void* unit, std::int32_t affix, std::int32_t) noexcept {
	if (unit == nullptr || UnitType(unit) != UnitTypeMonster || affix != AffixMagicResistant) return;
	const std::uint16_t ranks = RankFlags(unit);
	if ((ranks & MagicResistExcludedRanks) != 0) return;
	if ((ranks & MinionRank) != 0 && !MinionMagicResist) return;
	ApplyMagicResistant(unit);
}

#if defined(_MSC_VER)
#define AFFIX_RETURN_ADDRESS() _ReturnAddress()
#else
#define AFFIX_RETURN_ADDRESS() __builtin_return_address(0)
#endif

// Rank flag setter. SpawnMinions sets flag 10h on each new minion right after
// copying the owner's xfer affixes into it; at that one call site, a minion
// that received Magic Resistant gets it applied, since SpawnMinions never runs
// affix spawn entries. Every other call passes straight through.
std::uint64_t __fastcall HookSetRankFlag(void* unit, std::uint32_t flag, std::uint32_t set) noexcept {
	const void* const caller = AFFIX_RETURN_ADDRESS();
	const std::uint64_t result = OriginalSetRankFlag(unit, flag, set);

	if (caller == reinterpret_cast<const void*>(Base + MinionFlagReturnRva) && (flag & 0xFFFFU) == MinionRank
	    && (set & 0xFFU) != 0 && unit != nullptr && UnitType(unit) == UnitTypeMonster
	    && (RankFlags(unit) & MagicResistExcludedRanks) == 0 && HasAffix(unit, AffixMagicResistant)) {
		ApplyMagicResistant(unit);
		MinionMagicResistApplied.fetch_add(1, std::memory_order_relaxed);
	}
	return result;
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

constexpr auto IsBlank(char c) noexcept -> bool {
	return c == ' ' || c == '\t' || c == '\r';
}

// Finds `key = value` at the start of a line. The config is one flat table, so
// comments and section headers are simply skipped.
auto FindConfigValue(const char* text, const char* key, const char*& value, std::size_t& length) noexcept -> bool {
	const std::size_t keyLength = std::strlen(key);
	for (const char* line = text; *line != '\0';) {
		const char* cursor = line;
		while (IsBlank(*cursor)) ++cursor;

		if (std::strncmp(cursor, key, keyLength) == 0) {
			const char* after = cursor + keyLength;
			while (IsBlank(*after)) ++after;
			if (*after == '=') {
				++after;
				while (IsBlank(*after)) ++after;
				const char* end = after;
				while (*end != '\0' && *end != '\n' && *end != '#') ++end;
				while (end > after && IsBlank(end[-1])) --end;
				value  = after;
				length = static_cast<std::size_t>(end - after);
				return true;
			}
		}

		while (*line != '\0' && *line != '\n') ++line;
		if (*line == '\n') ++line;
	}
	return false;
}

// Parses an optionally negative whole number. Returns false for anything else.
auto ParseInteger(const char* value, std::size_t length, std::int64_t& result) noexcept -> bool {
	std::size_t i        = 0;
	bool        negative = false;
	if (length > 0 && value[0] == '-') {
		negative = true;
		i        = 1;
	}
	if (i >= length) return false;

	std::int64_t number = 0;
	for (; i < length; ++i) {
		const char c = value[i];
		if (c == '_') continue;
		if (c < '0' || c > '9') return false;
		number = number * 10 + (c - '0');
		if (number > 1'000'000'000) return false;
	}
	result = negative ? -number : number;
	return true;
}

// Reads a whole number in [minimum, maximum]. Missing keys keep the fallback;
// bad values keep it and log why.
auto ReadNumber(const char* text, const char* key, std::int32_t minimum, std::int32_t maximum,
                std::int32_t fallback) noexcept -> std::int32_t {
	const char* value  = nullptr;
	std::size_t length = 0;
	if (!FindConfigValue(text, key, value, length) || length == 0) return fallback;

	std::int64_t number = 0;
	if (!ParseInteger(value, length, number) || number < minimum || number > maximum) {
		char message[192] {};
		std::snprintf(message, sizeof(message), "%s must be a whole number from %d to %d; using %d.", key, minimum,
		              maximum, fallback);
		LogWarn(message);
		return fallback;
	}
	return static_cast<std::int32_t>(number);
}

auto ReadSkillId(const char* text, const char* key, std::int32_t fallback) noexcept -> std::int32_t {
	return ReadNumber(text, key, NoSkill, MaxSkillId, fallback);
}

// Reads `key = [a, b, c]`: skills.txt ids, -1 entries skipped.
void ReadSkillList(const char* text, const char* key, Settings& settings) noexcept {
	const char* value  = nullptr;
	std::size_t length = 0;
	settings.cursedCount = 0;
	if (!FindConfigValue(text, key, value, length) || length < 2 || value[0] != '[' || value[length - 1] != ']') {
		if (length != 0) {
			char message[160] {};
			std::snprintf(message, sizeof(message), "%s must be a list such as [66, 72]; no skills used.", key);
			LogWarn(message);
		}
		return;
	}

	std::size_t i = 1;
	while (i < length - 1) {
		while (i < length - 1 && (IsBlank(value[i]) || value[i] == ',')) ++i;
		if (i >= length - 1) break;
		std::size_t end = i;
		while (end < length - 1 && value[end] != ',' && !IsBlank(value[end])) ++end;

		std::int64_t number = 0;
		if (!ParseInteger(value + i, end - i, number) || number < NoSkill || number > MaxSkillId) {
			char message[160] {};
			std::snprintf(message, sizeof(message), "%s holds an entry that is not a skill id; it is skipped.", key);
			LogWarn(message);
		} else if (number != NoSkill) {
			if (settings.cursedCount < MaxCursedSkills) {
				settings.cursedSkills[settings.cursedCount++] = static_cast<std::int32_t>(number);
			} else {
				LogWarn("cursed_skills holds more than 16 skills; the rest are ignored.");
				break;
			}
		}
		i = end;
	}
}

constexpr char DefaultConfigToml[] = R"toml(# celestialrayone.monster-affixes
#
# Monster affix rework for the unique monster mods in monumod.txt.
# Changes are read when the plugin loads. Restart the game after editing.
# Built for D2RLoader 1.3.1 (Diablo II: Resurrected 3.3).
# Console command: monsteraffixes (what is installed, casts, lockouts).
#
# An affix only spawns when its monumod.txt row has enabled = 1 and a pick
# weight (upick). Nothing here changes which affixes can roll.
#
# Every skill below is a skills.txt *Id. -1 turns that entry off.
#
# HOW THE SKILLS ARE CAST
#   The monster casts the skill the way the game casts a "chance to cast when
#   struck" item skill: at a unit, or at the point it is moving or aiming
#   toward when there is no unit. The cast costs nothing and plays no
#   animation. Skill level = the monster's level, except for Cursed.
#
# WHEN STRUCK
#   Fire, Cold and Lightning Enchanted, Teleportation and Stone Skin trigger
#   every time the monster is struck by a hit that makes it flinch, a hit
#   that does not, or a soft hit. The killing blow triggers nothing. The
#   skill is aimed at the attacker. Only uniques and superuniques trigger;
#   minions that inherit the affix (monumod.txt xfer = 1) do not.
#
# LOCKOUT
#   After a cast, that affix cannot cast again on that monster for the cast
#   skill's localdelay in skills.txt, in frames (25 = 1 second). localdelay is
#   evaluated as a formula for the monster at the cast level, so lvl and other
#   calc terms work. Blank or 0 means no lockout. Teleportation uses its own
#   cooldown below instead. Spectral Hit locks each of its skills separately.

# FIRE / COLD / LIGHTNING ENCHANTED (when struck)
#   Fire Enchanted no longer explodes on death and Cold Enchanted no longer
#   releases its nova on death (always, not configurable).
#   Lightning Enchanted: setting a skill replaces its vanilla charged bolts;
#   with -1 the vanilla bolts stay.
#   What else these affixes still give: extra fire / lightning / cold damage
#   on the monster's attacks at spawn (monlvl.txt DM / L-DM by difficulty x
#   monumod.txt constants), plus cold length for Cold Enchanted.
#
# NOTE ON MINIONS: the game copies the boss's affixes (xfer = 1) into its
#   minions but does not run the affix spawn bonuses for them when they first
#   spawn. A minion only gets spawn bonuses (enchanted damage, Extra Strong,
#   Extra Fast) after it goes inactive and the game rebuilds it. Magic
#   Resistant is the exception: this plugin applies it to minions right away.
#   Cursed and Mana Burn work on minions from the start (they act when the
#   monster lands a hit, not at spawn).
fire_enchanted_skill = -1
cold_enchanted_skill = -1
lightning_enchanted_skill = -1

# STONE SKIN (when struck)
#   Stone Skin no longer doubles defense or adds resistance.
stone_skin_skill = -1

# TELEPORTATION (when struck)
#   The affix still gives the monster its teleport skill (skills.txt Id 184)
#   as before. On top of that, each struck hit has teleport_chance percent to
#   cast teleport_skill at the attacker. After a cast nothing fires for
#   teleport_cooldown_frames (25 = 1 second). While the cooldown runs no roll
#   is made. -1 for teleport_skill turns this part off.
teleport_skill = 184
teleport_chance = 25
teleport_cooldown_frames = 25

# SPECTRAL HIT (attack and cast animations)
#   Each time the monster starts an attack or cast animation (A1, A2, SC,
#   S1 to S4), it casts one of the skills below at its target, picked at
#   random among the ones that are ready. Every skill has its own lockout
#   (its own localdelay): a skill still locked is never picked, another ready
#   one is picked instead, and when all of them are locked nothing is cast.
#   Only the entries that are not -1 are used. Uniques only, as before.
#   The vanilla routine (a random element added to the monster's damage and to
#   every missile it fires) is always removed, even with all four set to -1.
spectral_hit_skill_1 = -1
spectral_hit_skill_2 = -1
spectral_hit_skill_3 = -1
spectral_hit_skill_4 = -1

# CURSED (when the monster lands a hit)
#   Same trigger and chance as before: monumod.txt constants row 34 (Proc
#   chance for the curse mod), rolled each time the monster lands a hit. On a
#   success it casts one skill picked at random from this list, at the unit it
#   hit, at level = monster level / 5 + 1. The picked skill's localdelay
#   applies as a lockout.
#   Minions that inherit the affix (monumod.txt xfer = 1) curse too.
#   An empty list means Amplify Damage (skills.txt Id 66), cast the same way.
#   Monsters with the special rank flag 800h (bound demons) keep the stock
#   routine untouched: Amplify Damage with its own chance.
cursed_skills = []

# EXTRA STRONG (at spawn)
#   Flat bonuses, the same for every difficulty. No attack rating bonus any
#   more. (Minions: see NOTE ON MINIONS above.)
#   strong_damage_percent: damagepercent (stat 25)
#   strong_spell_damage_percent: added to each of passive_fire_mastery,
#   passive_ltng_mastery, passive_cold_mastery, passive_pois_mastery,
#   passive_mag2_mastery and passive_wind_mastery (stats 329 to 332, 482, 483)
strong_damage_percent = 150
strong_spell_damage_percent = 100

# EXTRA FAST (at spawn)
#   Walk speed is unchanged. On top of it, flat attack rate (stat 68,
#   attackrate) and cast rate, per difficulty. (Minions: see NOTE ON MINIONS
#   above.)
#   fast_cast_rate_stat is the itemstatcost.txt id the cast rate goes to:
#   105 is item_fastercastrate (93 is item_fasterattackrate).
fast_attack_rate_normal = 25
fast_attack_rate_nightmare = 50
fast_attack_rate_hell = 75
fast_cast_rate_normal = 25
fast_cast_rate_nightmare = 50
fast_cast_rate_hell = 75
fast_cast_rate_stat = 105

# MANA BURN (when the monster lands a hit)
#   No mana drain damage any more. Instead every successful hit on a player
#   burns mana_burn_percent of that player's maximum mana from the current
#   mana. Missiles count, and minions that inherit the affix burn too.
#   -1 keeps the stock mana drain.
mana_burn_percent = 10

# RESISTANCE (always on, nothing to configure)
#   No affix grants resistance any more. Removed: Fire / Cold / Lightning
#   Enchanted (+75 to their element), Poison (+75 poison), Mana Burn
#   (+20 magic), Spectral Hit (+20 fire, cold, lightning), Stone Skin
#   (+50 physical) and the old Magic Resistant (+40 fire, cold, lightning).
#   Ghostly champions keep their 80% physical resist (a separate routine).
#
# MAGIC RESISTANT (always on, nothing to configure)
#   At spawn, each resist separately gets +50, capped at 95.
#   What the cap leaves unused is converted:
#     fire, cold, lightning, magic: 1% absorb of that element per 2 points
#     physical: 1 dodge, 1 avoid and 1 evade per 2 points
#     poison: 1% poison length reduction per point
#   A resist already at 95 or more gets no boost and converts all 50.
#   Applied once at spawn, never re-evaluated. The boss gets it, and so do its
#   minions that inherit the affix (monumod.txt xfer = 1); vanilla gave the
#   minions nothing.
)toml";

void LoadSettings() noexcept {
	if (!Context->EnsureConfig(DefaultConfigToml)) {
		LogWarn("Could not create celestialrayone.monster-affixes.toml; built-in defaults are used.");
	} else {
		std::array<char, 24'576> buffer {};
		if (!Context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size()))) {
			LogWarn("Could not read celestialrayone.monster-affixes.toml; built-in defaults are used.");
		} else {
			buffer.back()  = '\0';
			ConfigFileRead = true;

			const char* text = buffer.data();
			Settings&   c    = Config;

			c.fireSkill         = ReadSkillId(text, "fire_enchanted_skill", c.fireSkill);
			c.coldSkill         = ReadSkillId(text, "cold_enchanted_skill", c.coldSkill);
			c.lightningSkill    = ReadSkillId(text, "lightning_enchanted_skill", c.lightningSkill);
			c.stoneSkinSkill    = ReadSkillId(text, "stone_skin_skill", c.stoneSkinSkill);
			c.spectralSkills[0] = ReadSkillId(text, "spectral_hit_skill_1", c.spectralSkills[0]);
			c.spectralSkills[1] = ReadSkillId(text, "spectral_hit_skill_2", c.spectralSkills[1]);
			c.spectralSkills[2] = ReadSkillId(text, "spectral_hit_skill_3", c.spectralSkills[2]);
			c.spectralSkills[3] = ReadSkillId(text, "spectral_hit_skill_4", c.spectralSkills[3]);

			c.teleportSkill          = ReadSkillId(text, "teleport_skill", c.teleportSkill);
			c.teleportChance         = ReadNumber(text, "teleport_chance", 0, 100, c.teleportChance);
			c.teleportCooldownFrames = ReadNumber(text, "teleport_cooldown_frames", 0, 1'000'000, c.teleportCooldownFrames);

			ReadSkillList(text, "cursed_skills", c);

			c.strongDamagePercent      = ReadNumber(text, "strong_damage_percent", -1'000'000, 1'000'000,
			                                        c.strongDamagePercent);
			c.strongSpellDamagePercent = ReadNumber(text, "strong_spell_damage_percent", -1'000'000, 1'000'000,
			                                        c.strongSpellDamagePercent);

			c.fastAttackRate[0] = ReadNumber(text, "fast_attack_rate_normal", -1'000, 1'000, c.fastAttackRate[0]);
			c.fastAttackRate[1] = ReadNumber(text, "fast_attack_rate_nightmare", -1'000, 1'000, c.fastAttackRate[1]);
			c.fastAttackRate[2] = ReadNumber(text, "fast_attack_rate_hell", -1'000, 1'000, c.fastAttackRate[2]);
			c.fastCastRate[0]   = ReadNumber(text, "fast_cast_rate_normal", -1'000, 1'000, c.fastCastRate[0]);
			c.fastCastRate[1]   = ReadNumber(text, "fast_cast_rate_nightmare", -1'000, 1'000, c.fastCastRate[1]);
			c.fastCastRate[2]   = ReadNumber(text, "fast_cast_rate_hell", -1'000, 1'000, c.fastCastRate[2]);
			c.fastCastRateStat  = ReadNumber(text, "fast_cast_rate_stat", 0, 0xFFFF, c.fastCastRateStat);

			c.manaBurnPercent = ReadNumber(text, "mana_burn_percent", -1, 100, c.manaBurnPercent);
		}
	}

	SpectralPoolSize = 0;
	for (const std::int32_t skill : Config.spectralSkills) {
		if (skill != NoSkill) SpectralPool[SpectralPoolSize++] = skill;
	}

	CursedPoolSize = 0;
	for (std::uint32_t i = 0; i < Config.cursedCount; ++i) CursedPool[CursedPoolSize++] = Config.cursedSkills[i];
	if (CursedPoolSize == 0) CursedPool[CursedPoolSize++] = StockCurseSkill;
}

// ---------------------------------------------------------------------------
// Installation
// ---------------------------------------------------------------------------

auto CheckWitnesses() noexcept -> bool {
	struct Witness {
		std::uint64_t       rva;
		const std::uint8_t* bytes;
		std::uint32_t       size;
		const char*         name;
	};
	const Witness witnesses[] {
		{ GetUnitStatRva, LoaderThunkBytes, ByteCount(LoaderThunkBytes), "stat read" },
		{ SetUnitStatRva, LoaderThunkBytes, ByteCount(LoaderThunkBytes), "stat write" },
		{ AddUnitStatRva, LoaderThunkBytes, ByteCount(LoaderThunkBytes), "stat add" },
		{ UnitTypeRva, UnitTypeBytes, ByteCount(UnitTypeBytes), "unit type" },
		{ UnitIdRva, UnitIdBytes, ByteCount(UnitIdBytes), "unit id" },
		{ MissileOwnerRva, MissileOwnerBytes, ByteCount(MissileOwnerBytes), "missile owner" },
		{ SkillsRecordRva, SkillsRecordBytes, ByteCount(SkillsRecordBytes), "skills.txt record" },
		{ EvaluateRva, EvaluateBytes, ByteCount(EvaluateBytes), "skill formula" },
	};

	bool ok = true;
	for (const Witness& witness : witnesses) {
		if (!Context->CheckExpectedBytes(witness.rva, witness.bytes, witness.size)) {
			char message[192] {};
			std::snprintf(message, sizeof(message), "Entry of the %s helper at 0x%llX does not match; wrong game build.",
			              witness.name, static_cast<unsigned long long>(witness.rva));
			LogError(message);
			ok = false;
		}
	}
	return ok;
}

void ResolveFunctions() noexcept {
	GetUnitStat    = At<GetUnitStatFn>(GetUnitStatRva);
	SetUnitStat    = At<SetUnitStatFn>(SetUnitStatRva);
	AddUnitStat    = At<AddUnitStatFn>(AddUnitStatRva);
	UnitType       = At<UnitTypeFn>(UnitTypeRva);
	UnitId         = At<UnitIdFn>(UnitIdRva);
	UnitMode       = At<UnitModeFn>(UnitModeRva);
	UnitSeed       = At<UnitSeedFn>(UnitSeedRva);
	UnitGame       = At<UnitGameFn>(UnitGameRva);
	SetUnitFlag    = At<SetUnitFlagFn>(SetUnitFlagRva);
	PathPointX     = At<PathPointFn>(PathPointXRva);
	PathPointY     = At<PathPointFn>(PathPointYRva);
	TargetUnit     = At<TargetUnitFn>(TargetUnitRva);
	MissileOwner   = At<MissileOwnerFn>(MissileOwnerRva);
	AffixList      = At<AffixListFn>(AffixListRva);
	HasRankFlag    = At<HasRankFlagFn>(HasRankFlagRva);
	AffixConstant  = At<AffixConstantFn>(AffixConstantRva);
	SkillsRecord   = At<SkillsRecordFn>(SkillsRecordRva);
	Evaluate       = At<EvaluateFn>(EvaluateRva);
	CastOnTarget   = At<CastOnTargetFn>(CastOnTargetRva);
	CastAtPosition = At<CastAtPositionFn>(CastAtPositionRva);
}

// Installs an inline hook and logs the outcome. original may be null for a
// hook that replaces the routine outright.
template <std::size_t Size>
auto Hook(std::uint64_t rva, const std::uint8_t (&prologue)[Size], void* hook, void** original, const char* name) noexcept
	-> bool {
	const bool ok = Context->InstallInlineHook(rva, prologue, ByteCount(prologue), hook, original)
	             && (original == nullptr || *original != nullptr);
	char message[224] {};
	if (ok) {
		std::snprintf(message, sizeof(message), "Hooked %s at 0x%llX.", name, static_cast<unsigned long long>(rva));
		LogInfo(message);
	} else {
		std::snprintf(message, sizeof(message),
		              "Inline hook on %s at 0x%llX failed: wrong game build, or its first bytes are not the "
		              "expected ones.",
		              name, static_cast<unsigned long long>(rva));
		LogError(message);
	}
	return ok;
}

auto Patch(const ReturnPatch& patch) noexcept -> bool {
	const bool ok = Context->PatchBytes(patch.rva, patch.expected, patch.size, patch.replacement, patch.size);
	char message[224] {};
	if (ok) {
		std::snprintf(message, sizeof(message), "Disabled %s (now returns at once).", patch.name);
		LogInfo(message);
	} else {
		std::snprintf(message, sizeof(message),
		              "Patching %s failed: wrong game build, or its first bytes are not the expected ones.", patch.name);
		LogError(message);
	}
	return ok;
}

void InstallResistRoutine() noexcept {
	ResistRoutineInstalled = Hook(ResistBonusRva, ResistBonusPrologue, reinterpret_cast<void*>(&HookResistBonus),
	                              nullptr, "the affix resist routine");
}

void InstallMinionMagicResist() noexcept {
	if (!ResistRoutineInstalled) return;
	if (!Context->CheckExpectedBytes(MinionFlagSiteRva, MinionFlagSiteBytes, ByteCount(MinionFlagSiteBytes))) {
		LogError("Minion flag call site at 0x49ED5F (SpawnMinions) does not match; minions keep vanilla Magic "
		         "Resistant (none).");
		return;
	}
	MinionMagicResist = Hook(SetRankFlagRva, SetRankFlagPrologue, reinterpret_cast<void*>(&HookSetRankFlag),
	                         reinterpret_cast<void**>(&OriginalSetRankFlag), "the rank flag setter");
}

void InstallAll() noexcept {
	const bool struckWanted = Config.fireSkill != NoSkill || Config.coldSkill != NoSkill
	                       || Config.lightningSkill != NoSkill || Config.stoneSkinSkill != NoSkill
	                       || (Config.teleportSkill != NoSkill && Config.teleportChance > 0);
	const bool manaBurnWanted = Config.manaBurnPercent >= 0;

	InstallResistRoutine();
	InstallMinionMagicResist();

	// Who hit whom, for the struck skills, Mana Burn and Cursed.
	if (struckWanted || manaBurnWanted) {
		StruckCaptureInstalled = Hook(FinalizeDamageRva, FinalizeDamagePrologue,
		                              reinterpret_cast<void*>(&HookFinalizeDamage),
		                              reinterpret_cast<void**>(&OriginalFinalizeDamage), "FinalizeDamage");
	}
	HitCaptureInstalled = Hook(ExecuteEventsRva, ExecuteEventsPrologue, reinterpret_cast<void*>(&HookExecuteEvents),
	                           reinterpret_cast<void**>(&OriginalExecuteEvents), "ExecuteEvents");

	// Death effects and the Spectral Hit element routines go in every case.
	FireDeathPatched       = Patch(FireDeathPatch);
	ColdDeathPatched       = Patch(ColdDeathPatch);
	SpectralElementPatched = Patch(SpectralElementPatch);
	SpectralMissilePatched = Patch(SpectralMissilePatch);

	// When struck.
	if (struckWanted) {
		StruckHookInstalled = Hook(StruckDispatchRva, DispatchPrologue, reinterpret_cast<void*>(&HookStruckDispatch),
		                           reinterpret_cast<void**>(&OriginalStruckDispatch), "the struck affix dispatcher");
	}
	if (StruckHookInstalled) {
		FireActive      = Config.fireSkill != NoSkill;
		ColdActive      = Config.coldSkill != NoSkill;
		StoneSkinActive = Config.stoneSkinSkill != NoSkill;
		TeleportActive  = Config.teleportSkill != NoSkill && Config.teleportChance > 0;
		if (Config.lightningSkill != NoSkill) {
			// Without the bolt patch the monster would do both, so the skill waits for it.
			LightningBoltsPatched = Patch(LightningBoltPatch);
			LightningActive       = LightningBoltsPatched;
		}
	}

	// Spectral Hit.
	if (SpectralPoolSize > 0) {
		ModeHookInstalled = Hook(ModeDispatchRva, DispatchPrologue, reinterpret_cast<void*>(&HookModeDispatch),
		                         reinterpret_cast<void**>(&OriginalModeDispatch), "the mode-change affix dispatcher");
		SpectralActive = ModeHookInstalled;
	}

	// Cursed.
	LandedHookInstalled = Hook(LandedDispatchRva, DispatchPrologue, reinterpret_cast<void*>(&HookLandedDispatch),
	                           nullptr, "the landed-hit affix dispatcher");
	CursedActive = LandedHookInstalled;

	// Spawn bonuses.
	StrongHookInstalled = Hook(StrongSpawnRva, StrongSpawnPrologue, reinterpret_cast<void*>(&HookStrongSpawn), nullptr,
	                           "the Extra Strong spawn routine");
	FastHookInstalled = Hook(FastSpawnRva, FastSpawnPrologue, reinterpret_cast<void*>(&HookFastSpawn),
	                         reinterpret_cast<void**>(&OriginalFastSpawn), "the Extra Fast spawn routine");

	// Mana Burn: the stock drain goes only once the per-hit drain is in place.
	if (manaBurnWanted) {
		if (StruckCaptureInstalled) {
			ManaBurnPatched = Patch(ManaBurnSpawnPatch);
			ManaBurnActive  = ManaBurnPatched;
		} else {
			LogError("Mana Burn needs the FinalizeDamage hook, which is not installed; the stock mana drain stays.");
		}
	}

	char message[512] {};
	std::snprintf(message, sizeof(message),
	              "Struck skills: fire %d (%s), cold %d (%s), lightning %d (%s), stone skin %d (%s), teleport %d at "
	              "%d%% every %d frames (%s). Spectral Hit: %u skill%s (%s). Cursed: %u skill%s (%s). Mana Burn %d%% "
	              "(%s). Death effects: fire %s, cold %s.",
	              Config.fireSkill, FireActive ? "on" : "off", Config.coldSkill, ColdActive ? "on" : "off",
	              Config.lightningSkill, LightningActive ? "on" : "off", Config.stoneSkinSkill,
	              StoneSkinActive ? "on" : "off", Config.teleportSkill, Config.teleportChance,
	              Config.teleportCooldownFrames, TeleportActive ? "on" : "off", SpectralPoolSize,
	              SpectralPoolSize == 1 ? "" : "s", SpectralActive ? "on" : "off", CursedPoolSize,
	              CursedPoolSize == 1 ? "" : "s", CursedActive ? "on" : "off", Config.manaBurnPercent,
	              ManaBurnActive ? "on" : "off", FireDeathPatched ? "removed" : "STOCK",
	              ColdDeathPatched ? "removed" : "STOCK");
	LogInfo(message);
}

// ---------------------------------------------------------------------------
// Console
// ---------------------------------------------------------------------------

auto OnOff(bool value) noexcept -> const char* {
	return value ? "on" : "OFF";
}

auto __cdecl StatusCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept
	-> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr) return D2RL::ConsoleCommandResult::Failed;
	const D2RL::PluginContext* plugin = command->plugin;

	char line[512] {};
	std::snprintf(line, sizeof(line),
	              "Monster Affixes 1.4.0: config %s | hooks: resist %s, minion flag %s, finalize %s, events %s, "
	              "struck %s, mode %s, landed %s, strong %s, fast %s",
	              ConfigFileRead ? "loaded" : "NOT READ (defaults)", OnOff(ResistRoutineInstalled),
	              OnOff(MinionMagicResist), OnOff(StruckCaptureInstalled), OnOff(HitCaptureInstalled),
	              OnOff(StruckHookInstalled), OnOff(ModeHookInstalled), OnOff(LandedHookInstalled),
	              OnOff(StrongHookInstalled), OnOff(FastHookInstalled));
	plugin->WriteConsoleMessage(line);

	std::snprintf(line, sizeof(line),
	              "disabled routines: fire death %s, cold death %s, spectral element %s, spectral missile %s, "
	              "lightning bolts %s, mana burn spawn %s",
	              OnOff(FireDeathPatched), OnOff(ColdDeathPatched), OnOff(SpectralElementPatched),
	              OnOff(SpectralMissilePatched), LightningBoltsPatched ? "on" : "stock",
	              ManaBurnPatched ? "on" : "stock");
	plugin->WriteConsoleMessage(line);

	std::snprintf(line, sizeof(line),
	              "magic resistant applied %llu (minions at spawn %llu) | mana burn hits %llu | lockouts held %llu",
	              static_cast<unsigned long long>(MagicResistApplied.load(std::memory_order_relaxed)),
	              static_cast<unsigned long long>(MinionMagicResistApplied.load(std::memory_order_relaxed)),
	              static_cast<unsigned long long>(ManaBurnHits.load(std::memory_order_relaxed)),
	              static_cast<unsigned long long>(LockoutCount()));
	plugin->WriteConsoleMessage(line);

	struct Row {
		const char*          name;
		std::int32_t         skill;
		bool                 active;
		const AffixCounters* counters;
	};
	const Row rows[] {
		{ "fire", Config.fireSkill, FireActive, &FireCounters },
		{ "cold", Config.coldSkill, ColdActive, &ColdCounters },
		{ "lightning", Config.lightningSkill, LightningActive, &LightningCounters },
		{ "stone skin", Config.stoneSkinSkill, StoneSkinActive, &StoneSkinCounters },
		{ "teleport", Config.teleportSkill, TeleportActive, &TeleportCounters },
		{ "spectral", SpectralPoolSize > 0 ? SpectralPool[0] : NoSkill, SpectralActive, &SpectralCounters },
		{ "cursed", CursedPoolSize > 0 ? CursedPool[0] : NoSkill, CursedActive, &CursedCounters },
	};
	for (const Row& row : rows) {
		std::snprintf(line, sizeof(line), "%s: skill %d %s | casts %llu | locked out %llu | skill missing %llu",
		              row.name, row.skill, row.active ? "on" : "off",
		              static_cast<unsigned long long>(row.counters->casts.load(std::memory_order_relaxed)),
		              static_cast<unsigned long long>(row.counters->lockedOut.load(std::memory_order_relaxed)),
		              static_cast<unsigned long long>(row.counters->missingSkill.load(std::memory_order_relaxed)));
		plugin->WriteConsoleMessage(line);
	}

	std::snprintf(line, sizeof(line),
	              "spectral pool %d %d %d %d (picked past a locked skill %llu) | cursed pool %u, minion casts %llu, "
	              "bound demons sent to the stock routine %llu",
	              Config.spectralSkills[0], Config.spectralSkills[1], Config.spectralSkills[2], Config.spectralSkills[3],
	              static_cast<unsigned long long>(SpectralRerolls.load(std::memory_order_relaxed)), CursedPoolSize,
	              static_cast<unsigned long long>(CursedMinionCasts.load(std::memory_order_relaxed)),
	              static_cast<unsigned long long>(CursedDelegated.load(std::memory_order_relaxed)));
	plugin->WriteConsoleMessage(line);
	return D2RL::ConsoleCommandResult::Handled;
}

constexpr D2RL::PluginInfo MonsterAffixesInfo {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = "celestialrayone.monster-affixes",
	.name        = "Monster Affixes",
	.version     = "1.4.0",
	.author      = "CelestialRayOne",
	.description = "Monster affix rework: skills cast when struck (enchanted, Stone Skin, Teleportation), no enchanted "
	               "death effects, Spectral Hit skills on attack and cast with per-skill lockouts, Cursed skill pool "
	               "shared with minions, new Extra Strong, Extra Fast and Mana Burn bonuses, localdelay lockouts, no "
	               "affix resistances and a new Magic Resistant that minions share.",
	.flags       = D2RL::PluginFlags::Server | D2RL::PluginFlags::NativeHooks,
};

}  // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
	return &MonsterAffixesInfo;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr) return false;
	Context = context;
	Base    = context->exeBase;
	if (Base == 0) {
		LogError("exeBase is zero; cannot resolve game functions.");
		return false;
	}

	if (!CheckWitnesses()) {
		LogError("Helper functions do not match this game build; nothing installed.");
		return false;
	}
	ResolveFunctions();
	LoadSettings();
	InstallAll();

	if (!Context->RegisterConsoleCommand("monsteraffixes", &StatusCommand,
	                                     "Monster Affixes status: installed parts, skills, cast and lockout counters.")) {
		LogWarn("Could not register the monsteraffixes console command.");
	}

	if (!ResistRoutineInstalled && !FireDeathPatched && !ColdDeathPatched && !StrongHookInstalled && !FastHookInstalled
	    && !LandedHookInstalled) {
		LogError("Nothing could be installed; unloading.");
		return false;
	}
	return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
	FireActive      = false;
	ColdActive      = false;
	LightningActive = false;
	StoneSkinActive = false;
	TeleportActive  = false;
	SpectralActive  = false;
	CursedActive    = false;
	ManaBurnActive  = false;
	MinionMagicResist      = false;
	OriginalFinalizeDamage = nullptr;
	OriginalExecuteEvents  = nullptr;
	OriginalSetRankFlag    = nullptr;
	OriginalModeDispatch   = nullptr;
	OriginalStruckDispatch = nullptr;
	OriginalFastSpawn      = nullptr;
	Context                = nullptr;
}
