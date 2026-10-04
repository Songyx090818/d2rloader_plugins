// =============================================================================
// Dynamic EType (celestialrayone.dynamic-etype)
// =============================================================================
//
// A skills.txt calc column decides a skill's EType once per cast. Everything
// that reads EType during that cast sees the decided element: elemental
// damage and its mastery, physical to elemental conversion, missile damage,
// D2RCore's "+X to <element> Skills" level bonus, and the missiles.txt EType of
// every missile the cast creates that has elemental damage of its own.
//
// Everything below was read out of the live D2RLoader 1.3.1 image (D2R
// 3.3.93847, base 0x140000000) and D2RCore.dll. Every byte the plugin writes
// or relies on is checked at load; nothing is installed when one differs.
//
// -----------------------------------------------------------------------------
// The column
// -----------------------------------------------------------------------------
//   skills.txt binder 0x302380 stores "EType" as type 13 (elemtypes.txt code
//   to row number) at record +0x284 (0x306624 / 0x30662F), one byte. So at
//   runtime EType is a number like SrcDam, and a calc can drive it. calc1 to
//   calc10 are dwords at record +0x190 to +0x1B4; a blank one is 0xFFFFFFFF.
//
// -----------------------------------------------------------------------------
// Who reads EType
// -----------------------------------------------------------------------------
//   Readers take the byte straight out of the shared skills.txt row, inline,
//   in dozens of places:
//     0x338340  elemental max getter, reads +0x284 at 0x338495 and hands it
//               to the mastery function 0x337F40 (the min getter 0x3387D0 too)
//     0x3BF9B0  missile damage builder, reads the linked skill's +0x284 at
//               0x3BFF5F and switches on it (1 fire, 2 ltng, 3 mag, 4/12 cold,
//               5 pois, 6 life, 7 mana, 8 stam, 9 stun, 11 burn)
//     0x5622C0  Charge, conversion element: cmp byte [r13+284h],0 at 0x5627EA,
//               movzx eax,byte [r13+284h] at 0x56281F (every melee skill
//               function that converts physical damage does the same)
//     D2RCore   the skill bonus level behind the ReadWideSkillLevel export
//               (D2RCore+0x832ED0 -> +0x3DDD80) reads +0x284 at +0x3DDF08 and
//               adds item_elemskill (stat 126) layered by it. It is the only
//               EType reader in D2RCore and nothing is cached.
//   Hooking every reader is not possible within reason, so the plugin binds
//   the element to the row itself, but only while the server runs code for
//   that cast, and puts the row's own value back the moment it returns.
//
//   missiles.txt has its own EType (record +0x10C, one byte). It is read:
//     0x3BF9B0  at creation, for a missile with no Skill and no MissileSkill
//               flag (3BFB5C): EMin/EMax and that row's ELen go under it.
//               D2RCore's AssignWideMissileDamage then copies every element
//               field to the missile's stats as it is, with no EType check.
//     0x3BB060  (dataContext, missile id) -> row+0x10C, on every hit, by the
//               functions that pick by the missile's own element: pSrvDmgFunc
//               1's conversion target (0x465160, also used by 5 and 12), the
//               per-element damage reader 0x465D60 (pSrvHitFunc 1, 9, 13, 31
//               and pSrvDmgFunc 5, 12, 15: the explosion reads only that
//               element's damage stats and length), pSrvDmgFunc 12 and 15 and
//               pSrvHitFunc 13 directly. All of them run inside the missile
//               hit resolution 0x4639A0; missile do functions that deal damage
//               call 0x4639A0 too. The standard hit (0x465420) reads every
//               element and needs none of this.
//   So a missile whose damage was built cold but whose row still says fire
//   would look for fire damage and fire length on hit and find neither: its
//   explosions deal nothing, conversion goes to fire, and cold never chills.
//
// -----------------------------------------------------------------------------
// One element per cast
// -----------------------------------------------------------------------------
//   Every cast has an id at unit+0x12C. The decision is made once and kept
//   under (game, unit, skill, cast id):
//     skill start    0x436480 (game, unit). Mode start of every player and
//                    monster skill, before the start function. Evaluates the
//                    calc with the level the row's own element gives, stores
//                    the decision, then runs the start with the row bound, so
//                    the level the game computes inside already carries the
//                    new element's "+X to <element> Skills".
//     skill perform  0x436B10 (game, unit). Every do event of the cast (each
//                    hit of a sequence too). Reuses the start's decision for
//                    the same cast id; the level it computes, the do-handler
//                    it calls and the aura timer it arms all see the element.
//   Code that runs on its own, without a start, decides on each run and keeps
//   the decision for the rest of that game frame, so a proc's start function
//   and its do-handler agree:
//     start dispatcher 0x43B3E0 (game, unit, skill, level, flag)
//     do-handler       0x43ACB0, see below (procs, aura and item aura pulses)
//     indexed do       0x435490 (game, unit, function, skill, level, cast id),
//                      skill functions that missile functions run
//   Missiles keep the decision under (game, missile cast id, skill row), so a
//   sub-missile created seconds later gets the element of the cast it belongs
//   to. A sub-missile takes its parent's cast id (missile creator 0x537A17)
//   and its parent's skill (missile data +0xC, every missile function copies
//   it). The row is the linked skills.txt row, or for a missile with its own
//   damage, the skill it was created for.
//
// -----------------------------------------------------------------------------
// Sites
// -----------------------------------------------------------------------------
//   Inline hooks (D2RLoader, function entries, nothing relative inside):
//     0x436480  48 89 5C 24 08 55 56 57 41 56 41 57 48 83 EC 50
//     0x436B10  48 89 5C 24 10 48 89 6C 24 18 56 48 83 EC 40
//     0x43B3E0  48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57
//
//   Do-handler 0x43ACB0 (game, unit, skill, level, consume, itemCast,
//   itemEffect). Its entry belongs to srcdam-calc-field and cast-on-cast
//   verifies its first 33 bytes, so the scope sits inside the body, where
//   rdi = game, rbx = unit, r13d = skill id and the level is at [rsp+0D8h]:
//     0x43ACFC  E8 8F CA C5 FF        call 97790, the record fetch. Becomes a
//               call to stub A, which makes that call, then opens the scope.
//     0x43AE1A  48 8B 9C 24 C8 00 00 00   mov rbx,[rsp+0C8h], the first
//               instruction of the one epilogue every exit reaches (early
//               exits fall through 0x43AE18, the normal one jumps from
//               0x43B2A7). Becomes a call to stub B plus 3 NOPs; stub B closes
//               the scope, keeps rax, replays the mov and returns to the NOPs.
//   rsp is the same at both sites, so the host rsp names the scope: stub B
//   only closes a scope opened by stub A in the same do-handler call.
//
//   Missile damage builder 0x3BF9B0 has exactly one caller:
//     0x537A6D  E8 3E 7F E8 FF  in the missile creator 0x5371A0, after the new
//               missile got its cast id. Becomes a call to a thunk that jumps
//               to HookedMissileDamage, which has the builder's own signature.
//   Indexed do 0x435490 starts with cmp r8d,0BEh / ja rel32, so its two
//   callers are redirected instead, the same way:
//     0x453466  E8 25 20 FE FF  in missile function 0x4533C0
//     0x459340  E8 4B C1 FD FF  in missile function 0x4592A0
//
//   Missile hit resolution 0x4639A0 (game, missile, target, forced). Its entry
//   belongs to crossbow-charges, so the scope sits inside the body, where
//   r12 = game and r14 = missile. The whole body (4639A0..4640E2) was
//   disassembled: one ret, no jump out, rsp only moves in prologue/epilogue.
//     0x4639FE  E8 3D 26 D0 FF        call 166040, the missiles.txt record
//               fetch, straight-line from the entry. Becomes a call to stub C,
//               which makes that call, then opens the scope on the row's
//               EType byte.
//     0x463AEB  48 8B 8D 10 01 00 00  mov rcx,[rbp+110h], the first
//               instruction of the one epilogue every exit reaches (463AE6
//               falls through, 463D73 and 4640DD jump). Becomes a call to stub
//               D plus 2 NOPs; stub D closes the scope, keeps rax, replays the
//               mov and returns to the NOPs before xor rcx,rsp.
//
//   The four stubs have UNWIND_INFO registered for them, so a stack walk
//   through any of them reaches the game frame. The thunks are a single jmp
//   and need none (an address with no entry is unwound as a leaf).
//
//   Not covered: 0x435520, the other table dispatcher, has no caller and no
//   reference in this build or in D2RCore.
//
// -----------------------------------------------------------------------------
// Other plugins
// -----------------------------------------------------------------------------
//   Untouched: srcdam-calc-field's hook on 0x43ACB0's entry and cast-on-cast's
//   33-byte witness there, cast-on-cast's 0x43AF06..0x43AF25 and 0x43B0EC,
//   crossbow-charges' 0x43B18D and its 0x5379F5..0x537A22 window,
//   damage-overflow-fix and srcdam-calc-field inside 0x3BF9B0,
//   prgstack-calc-field's 36-byte witness on 0x56C3E0 (the progressive release
//   is only called by skill functions, so it already runs inside a scope),
//   crossbow-charges' hook on 0x4639A0's entry (16 bytes), and the witnesses
//   Ruff's doll-explosion keeps on 0x4639A0..0x4639BE and 0x4640D8.
//
// Console: "dynamic-etype" for status and counters, "dynamic-etype log on|off"
// for the decision log. Settings: d2rloader/config/celestialrayone.dynamic-etype.toml
// =============================================================================

#include <D2RLPlugin/api.h>

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
#include <limits>

namespace CelestialRayOne::DynamicEType {
namespace {

constexpr char PluginVersion[] = "1.1.0";

// -----------------------------------------------------------------------------
// Native anchors
// -----------------------------------------------------------------------------

constexpr std::uint64_t SkillsRecordRva      = 0x097790;  // (dataContext, skillId) -> skills.txt record
constexpr std::uint64_t EvaluateFormulaRva   = 0x3B5160;  // (dataContext, unit, calc, skillId, level)
constexpr std::uint64_t SkillLevelRva        = 0x33D1E0;  // loader thunk to ReadWideSkillLevel(unit, skill, 1, 0)
constexpr std::uint64_t DataContextRva       = 0x34A0E0;  // (unit) -> data context
constexpr std::uint64_t MissileRecordRva     = 0x166040;  // (dataContext, missileId) -> missiles.txt record
constexpr std::uint64_t MissileOwnerRva      = 0x490300;  // (game, missile) -> owner
constexpr std::uint64_t SkillStartRva        = 0x436480;
constexpr std::uint64_t SkillPerformRva      = 0x436B10;
constexpr std::uint64_t StartDispatcherRva   = 0x43B3E0;
constexpr std::uint64_t DoRecordWindowRva    = 0x43ACEC;
constexpr std::uint64_t DoRecordCallRva      = 0x43ACFC;
constexpr std::uint64_t DoEpilogueWindowRva  = 0x43AE18;
constexpr std::uint64_t DoEpilogueRva        = 0x43AE1A;
constexpr std::uint64_t MissileDamageRva     = 0x3BF9B0;
constexpr std::uint64_t MissileDamageWinRva  = 0x537A53;
constexpr std::uint64_t MissileDamageCallRva = 0x537A6D;
constexpr std::uint64_t IndexedDoRva         = 0x435490;
constexpr std::uint64_t IndexedDoWinARva     = 0x453449;
constexpr std::uint64_t IndexedDoCallARva    = 0x453466;
constexpr std::uint64_t IndexedDoWinBRva     = 0x459323;
constexpr std::uint64_t IndexedDoCallBRva    = 0x459340;
constexpr std::uint64_t MissileHitRva        = 0x4639A0;  // hit resolution (game, missile, target, forced)
constexpr std::uint64_t HitFrameWindowRva    = 0x4639B0;
constexpr std::uint64_t HitRecordWindowRva   = 0x4639D0;
constexpr std::uint64_t HitRecordCallRva     = 0x4639FE;
constexpr std::uint64_t HitEpilogueWindowRva = 0x463AE6;
constexpr std::uint64_t HitEpilogueRva       = 0x463AEB;
constexpr std::uint64_t UnitClassIdRva       = 0x349860;  // (unit) -> class id, called right before the fetch

// -----------------------------------------------------------------------------
// Layouts
// -----------------------------------------------------------------------------

constexpr std::size_t GameDataContextOffset = 0x106;
constexpr std::size_t GameFrameOffset       = 0x170;
constexpr std::size_t UnitTypeOffset        = 0x000;
constexpr std::size_t UnitClassOffset       = 0x004;
constexpr std::size_t UnitDataOffset        = 0x010;  // missile data for missiles
constexpr std::size_t UnitGameOffset        = 0x0D8;  // written by the server unit allocator 4905A0
constexpr std::size_t UnitSkillListOffset   = 0x100;
constexpr std::size_t UnitFlagsExOffset     = 0x128;
constexpr std::size_t UnitCastIdOffset      = 0x12C;
constexpr std::size_t SkillListUsedOffset   = 0x018;  // 34BA40 -> 33E090: [[unit+100h]+18h]
constexpr std::size_t SkillNodeRecordOffset = 0x000;  // the node's skills.txt record (33DBA0)
constexpr std::uint32_t ServerUnitFlag      = 0x04000000;
constexpr std::uint32_t UnitTypeMissile     = 3;

constexpr std::size_t   SkillIdOffset       = 0x000;  // the record's first word (33E080)
constexpr std::size_t   SkillETypeOffset    = 0x284;
constexpr std::size_t   SkillCalc1Offset    = 0x190;
constexpr std::size_t   SkillCalcStride     = 4;
constexpr std::uint32_t BlankCalc           = 0xFFFFFFFFU;

// missiles.txt, as the damage builder reads it (3BFACE..3BFE46).
constexpr std::size_t  MissileSkillOffset      = 438;   // Skill
constexpr std::size_t  MissileFlagsOffset      = 37;    // the byte holding MissileSkill
constexpr std::uint8_t MissileSkillFlag        = 0x80;  // mask global 0x141D996EC
constexpr std::size_t  MissileDataSkillOffset  = 12;    // the skill that created the missile (3BDC70)
constexpr std::size_t  MissileDataLevelOffset  = 14;    // its level (3BD6A0)
constexpr std::size_t  MissileETypeOffset      = 0x10C; // missiles.txt EType, read by 3BF9B0 and 3BB060

constexpr std::int32_t Keep       = -1;  // leave the row's own EType
constexpr std::int32_t MaxElement = 12;  // elemtypes.txt rows 1..12

constexpr int CalcColumnMin     = 1;
constexpr int CalcColumnMax     = 10;
constexpr int CalcColumnDefault = 9;

constexpr std::size_t CallSize = 5;

// -----------------------------------------------------------------------------
// Witnesses, all read from the live image
// -----------------------------------------------------------------------------

constexpr std::uint8_t SkillsRecordBytes[]{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x63, 0xF2 };
constexpr std::uint8_t EvaluateFormulaBytes[]{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57 };
constexpr std::uint8_t LoaderThunkBytes[]{ 0xFF, 0x25 };
constexpr std::uint8_t DataContextBytes[]{
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x85, 0xC9, 0x75, 0x1A, 0x88, 0x4C, 0x24, 0x30, 0x48, 0x8D, 0x4C, 0x24, 0x30 };
constexpr std::uint8_t MissileRecordBytes[]{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x63, 0xDA, 0xE8, 0x42, 0xAA, 0x19, 0x00 };
constexpr std::uint8_t MissileOwnerBytes[]{
    0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xFA, 0x48, 0x8B, 0xF1, 0x48, 0x85, 0xD2,
    0x75, 0x20 };
constexpr std::uint8_t SkillStartBytes[]{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x50 };
constexpr std::uint8_t SkillPerformBytes[]{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x48, 0x83, 0xEC, 0x40 };
constexpr std::uint8_t StartDispatcherBytes[]{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57 };
constexpr std::uint8_t MissileDamageBytes[]{
    0x4C, 0x89, 0x4C, 0x24, 0x20, 0x4C, 0x89, 0x44, 0x24, 0x18, 0x55, 0x57, 0x41, 0x56, 0x48, 0x81, 0xEC, 0xA0, 0x00,
    0x00, 0x00 };
constexpr std::uint8_t IndexedDoBytes[]{
    0x41, 0x81, 0xF8, 0xBE, 0x00, 0x00, 0x00, 0x0F, 0x87, 0x82, 0x00, 0x00, 0x00 };

// 43ACEC: movzx ecx,byte [rcx+106h] / xor r15d,r15d / mov edx,r8d /
// mov r12d,r15d / call 97790 / mov rsi,rax / test rax,rax / jz 43AE18 /
// mov rcx,rbx.
constexpr std::uint8_t DoRecordWindow[]{
    0x0F, 0xB6, 0x89, 0x06, 0x01, 0x00, 0x00, 0x45, 0x33, 0xFF, 0x41, 0x8B, 0xD0, 0x45, 0x8B, 0xE7,
    0xE8, 0x8F, 0xCA, 0xC5, 0xFF, 0x48, 0x8B, 0xF0, 0x48, 0x85, 0xC0, 0x0F, 0x84, 0x0B, 0x01, 0x00,
    0x00, 0x48, 0x8B, 0xCB };
constexpr std::size_t DoRecordCallOffset = DoRecordCallRva - DoRecordWindowRva;

// 43AE18: xor eax,eax / mov rbx,[rsp+0C8h] / add rsp,80h / pop r15..rbp / ret.
constexpr std::uint8_t DoEpilogueWindow[]{
    0x33, 0xC0, 0x48, 0x8B, 0x9C, 0x24, 0xC8, 0x00, 0x00, 0x00, 0x48, 0x81, 0xC4, 0x80, 0x00, 0x00,
    0x00, 0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5F, 0x5E, 0x5D, 0xC3 };
constexpr std::size_t DoEpilogueOffset = DoEpilogueRva - DoEpilogueWindowRva;
constexpr std::size_t DoEpilogueSize   = 8;  // the mov rbx,[rsp+0C8h]

// 537A53: mov [rsp+30h],al / mov r9,r15 / movzx eax,byte [rsp+58h] /
// mov r8,rbx / mov [rsp+28h],al / mov rdx,rsi / mov [rsp+20h],edi /
// call 3BF9B0 / mov r9d,edi.
constexpr std::uint8_t MissileDamageWindow[]{
    0x88, 0x44, 0x24, 0x30, 0x4D, 0x8B, 0xCF, 0x0F, 0xB6, 0x44, 0x24, 0x58, 0x4C, 0x8B, 0xC3, 0x88,
    0x44, 0x24, 0x28, 0x48, 0x8B, 0xD6, 0x89, 0x7C, 0x24, 0x20, 0xE8, 0x3E, 0x7F, 0xE8, 0xFF, 0x44,
    0x8B, 0xCF };
constexpr std::size_t MissileDamageCallOffset = MissileDamageCallRva - MissileDamageWinRva;

// 453449: mov rcx,rbx / call 34B6C0 (cast id) / mov r8d,[r15+70h] / mov r9d,ebp /
// mov [rsp+28h],eax / mov rdx,rbx / mov rcx,r14 / mov [rsp+20h],edi / call 435490.
constexpr std::uint8_t IndexedDoWindowA[]{
    0x48, 0x8B, 0xCB, 0xE8, 0x6F, 0x82, 0xEF, 0xFF, 0x45, 0x8B, 0x47, 0x70, 0x44, 0x8B, 0xCD, 0x89,
    0x44, 0x24, 0x28, 0x48, 0x8B, 0xD3, 0x49, 0x8B, 0xCE, 0x89, 0x7C, 0x24, 0x20, 0xE8, 0x25, 0x20,
    0xFE, 0xFF, 0x48, 0x8B, 0x5C, 0x24, 0x50 };
constexpr std::size_t IndexedDoCallOffsetA = IndexedDoCallARva - IndexedDoWinARva;

// 459323: mov rcx,rdi / call 34B6C0 (cast id) / mov r8d,[r14+60h] / mov r9d,ebp /
// mov [rsp+28h],eax / mov rdx,rdi / mov rcx,rsi / mov [rsp+20h],ebx / call 435490.
constexpr std::uint8_t IndexedDoWindowB[]{
    0x48, 0x8B, 0xCF, 0xE8, 0x95, 0x23, 0xEF, 0xFF, 0x45, 0x8B, 0x46, 0x60, 0x44, 0x8B, 0xCD, 0x89,
    0x44, 0x24, 0x28, 0x48, 0x8B, 0xD7, 0x48, 0x8B, 0xCE, 0x89, 0x5C, 0x24, 0x20, 0xE8, 0x4B, 0xC1,
    0xFD, 0xFF, 0x48, 0x8B, 0xD7, 0x48, 0x8B, 0xCE };
constexpr std::size_t IndexedDoCallOffsetB = IndexedDoCallBRva - IndexedDoWinBRva;

// 4639B0: lea rbp,[rsp-120h] / sub rsp,220h. After these the hit resolution
// never moves rsp until its epilogue (the whole body 4639A0..4640E2 was
// disassembled: no push, no pop, no other rsp write), so its rsp names it.
constexpr std::uint8_t HitFrameWindow[]{
    0x48, 0x8D, 0xAC, 0x24, 0xE0, 0xFE, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x20, 0x02, 0x00, 0x00 };

// 4639D0: mov r14,rdx / mov [rsp+38h],r9d / mov r15,r8 / lea rdx,[source file] /
// mov r12,rcx / mov r8d,1F31h / mov rcx,r14 / call 349860 /
// movzx ecx,byte [r12+106h] / mov edx,eax / call 166040 / mov rdx,r14 /
// mov rcx,r12 / mov r13,rax. Straight-line code from the entry: every hit
// passes the fetch, with r12 = game and r14 = missile.
constexpr std::uint8_t HitRecordWindow[]{
    0x4C, 0x8B, 0xF2, 0x44, 0x89, 0x4C, 0x24, 0x38, 0x4D, 0x8B, 0xF8, 0x48, 0x8D, 0x15, 0x4E, 0x73,
    0x8B, 0x01, 0x4C, 0x8B, 0xE1, 0x41, 0xB8, 0x31, 0x1F, 0x00, 0x00, 0x49, 0x8B, 0xCE, 0xE8, 0x6D,
    0x5E, 0xEE, 0xFF, 0x41, 0x0F, 0xB6, 0x8C, 0x24, 0x06, 0x01, 0x00, 0x00, 0x8B, 0xD0, 0xE8, 0x3D,
    0x26, 0xD0, 0xFF, 0x49, 0x8B, 0xD6, 0x49, 0x8B, 0xCC, 0x4C, 0x8B, 0xE8 };
constexpr std::size_t HitRecordCallOffset = HitRecordCallRva - HitRecordWindowRva;
constexpr std::size_t HitClassCallOffset  = 0x1E;

// 463AE6: mov eax,1 / mov rcx,[rbp+110h] / xor rcx,rsp / call cookie check /
// mov rbx,[rsp+278h] / add rsp,220h / pop r15..rbp / ret. The function's only
// ret; every exit reaches 463AEB (falling through 463AE6, or jmp from 463D73
// and 4640DD).
constexpr std::uint8_t HitEpilogueWindow[]{
    0xB8, 0x01, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x8D, 0x10, 0x01, 0x00, 0x00, 0x48, 0x33, 0xCC, 0xE8,
    0x56, 0xD6, 0xE6, 0x00, 0x48, 0x8B, 0x9C, 0x24, 0x78, 0x02, 0x00, 0x00, 0x48, 0x81, 0xC4, 0x20,
    0x02, 0x00, 0x00, 0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5F, 0x5E, 0x5D, 0xC3 };
constexpr std::size_t HitEpilogueOffset = HitEpilogueRva - HitEpilogueWindowRva;
constexpr std::size_t HitEpilogueSize   = 7;  // the mov rcx,[rbp+110h]

// The target a call inside a witness encodes, so the asserts below prove each
// redirected call is the call the comments say it is.
constexpr auto CallTarget(const std::uint8_t* window, std::uint64_t windowRva, std::size_t offset) noexcept
    -> std::uint64_t {
    const std::uint32_t raw = static_cast<std::uint32_t>(window[offset + 1])
        | (static_cast<std::uint32_t>(window[offset + 2]) << 8)
        | (static_cast<std::uint32_t>(window[offset + 3]) << 16)
        | (static_cast<std::uint32_t>(window[offset + 4]) << 24);
    const std::int64_t displacement = static_cast<std::int32_t>(raw);
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(windowRva + offset + CallSize) + displacement);
}

static_assert(DoRecordWindow[DoRecordCallOffset] == 0xE8
    && CallTarget(DoRecordWindow, DoRecordWindowRva, DoRecordCallOffset) == SkillsRecordRva,
    "0x43ACFC must be the do-handler's call to the skills.txt record getter");
static_assert(DoEpilogueWindow[DoEpilogueOffset] == 0x48 && DoEpilogueWindow[DoEpilogueOffset + 1] == 0x8B
    && DoEpilogueWindow[DoEpilogueOffset + 4] == 0xC8 && DoEpilogueWindow[DoEpilogueOffset + DoEpilogueSize] == 0x48
    && DoEpilogueWindow[DoEpilogueOffset + DoEpilogueSize + 1] == 0x81,
    "0x43AE1A must be mov rbx,[rsp+0C8h] followed by add rsp,80h");
static_assert(MissileDamageWindow[MissileDamageCallOffset] == 0xE8
    && CallTarget(MissileDamageWindow, MissileDamageWinRva, MissileDamageCallOffset) == MissileDamageRva,
    "0x537A6D must be the creator's call to the missile damage builder");
static_assert(IndexedDoWindowA[IndexedDoCallOffsetA] == 0xE8
    && CallTarget(IndexedDoWindowA, IndexedDoWinARva, IndexedDoCallOffsetA) == IndexedDoRva,
    "0x453466 must call the indexed do");
static_assert(IndexedDoWindowB[IndexedDoCallOffsetB] == 0xE8
    && CallTarget(IndexedDoWindowB, IndexedDoWinBRva, IndexedDoCallOffsetB) == IndexedDoRva,
    "0x459340 must call the indexed do");
static_assert(HitRecordWindow[HitRecordCallOffset] == 0xE8
    && CallTarget(HitRecordWindow, HitRecordWindowRva, HitRecordCallOffset) == MissileRecordRva
    && HitRecordWindow[HitClassCallOffset] == 0xE8
    && CallTarget(HitRecordWindow, HitRecordWindowRva, HitClassCallOffset) == UnitClassIdRva,
    "0x4639FE must be the hit resolution's missiles.txt record fetch, right after the class id call");
static_assert(HitEpilogueWindow[HitEpilogueOffset] == 0x48 && HitEpilogueWindow[HitEpilogueOffset + 1] == 0x8B
    && HitEpilogueWindow[HitEpilogueOffset + 2] == 0x8D && HitEpilogueWindow[HitEpilogueOffset + 3] == 0x10
    && HitEpilogueWindow[HitEpilogueOffset + 4] == 0x01 && HitEpilogueWindow[HitEpilogueOffset + HitEpilogueSize] == 0x48
    && HitEpilogueWindow[HitEpilogueOffset + HitEpilogueSize + 1] == 0x33,
    "0x463AEB must be mov rcx,[rbp+110h] followed by xor rcx,rsp");

struct Witness {
    std::uint64_t       rva;
    const std::uint8_t* bytes;
    std::uint32_t       size;
    const char*         what;
};

template <std::size_t Size>
constexpr auto W(std::uint64_t rva, const std::uint8_t (&bytes)[Size], const char* what) noexcept -> Witness {
    return { rva, bytes, static_cast<std::uint32_t>(Size), what };
}

constexpr std::array<Witness, 18> Witnesses{
    W(SkillsRecordRva, SkillsRecordBytes, "skills.txt record getter"),
    W(EvaluateFormulaRva, EvaluateFormulaBytes, "skill formula evaluator"),
    W(SkillLevelRva, LoaderThunkBytes, "skill level (loader thunk)"),
    W(DataContextRva, DataContextBytes, "unit data context"),
    W(MissileRecordRva, MissileRecordBytes, "missiles.txt record getter"),
    W(MissileOwnerRva, MissileOwnerBytes, "missile owner"),
    W(SkillStartRva, SkillStartBytes, "skill start"),
    W(SkillPerformRva, SkillPerformBytes, "skill perform"),
    W(StartDispatcherRva, StartDispatcherBytes, "start function dispatcher"),
    W(MissileDamageRva, MissileDamageBytes, "missile damage builder"),
    W(IndexedDoRva, IndexedDoBytes, "indexed skill function"),
    W(DoRecordWindowRva, DoRecordWindow, "do-handler record fetch"),
    W(DoEpilogueWindowRva, DoEpilogueWindow, "do-handler epilogue"),
    W(MissileDamageWinRva, MissileDamageWindow, "missile damage call"),
    W(IndexedDoWinARva, IndexedDoWindowA, "indexed skill function call A"),
    W(HitFrameWindowRva, HitFrameWindow, "missile hit resolution frame"),
    W(HitRecordWindowRva, HitRecordWindow, "missile hit resolution record fetch"),
    W(HitEpilogueWindowRva, HitEpilogueWindow, "missile hit resolution epilogue"),
};
constexpr Witness IndexedDoWitnessB = W(IndexedDoWinBRva, IndexedDoWindowB, "indexed skill function call B");

// -----------------------------------------------------------------------------
// Native function types
// -----------------------------------------------------------------------------

using SkillsRecordFn    = std::uint8_t*(__fastcall*)(std::uint8_t dataContext, std::int32_t skillId);
using EvaluateFormulaFn = std::int32_t(__fastcall*)(std::uint8_t dataContext, void* unit, std::uint32_t calc,
                                                     std::int32_t skillId, std::int32_t level);
using SkillLevelFn      = std::int32_t(__fastcall*)(void* unit, void* skill, std::int32_t withBonus, std::int32_t flags);
using DataContextFn     = std::uint8_t(__fastcall*)(void* unit);
using MissileRecordFn   = std::uint8_t*(__fastcall*)(std::uint8_t dataContext, std::int32_t missileId);
using MissileOwnerFn    = void*(__fastcall*)(void* game, void* missile);

using SkillStartFn = std::int64_t(__fastcall*)(void* game, void* unit) noexcept;
using SkillPerformFn = void*(__fastcall*)(void* game, void* unit) noexcept;
using StartDispatcherFn = std::int64_t(__fastcall*)(void* game, void* unit, std::int32_t skillId, std::int32_t level,
                                                     std::int32_t flag) noexcept;
using MissileDamageFn = std::int64_t(__fastcall*)(void* damage, void* owner, void* source, void* missile,
                                                   std::int32_t level, std::uint8_t flagA, std::uint8_t flagB);
using IndexedDoFn = void(__fastcall*)(void* game, void* unit, std::int32_t function, std::int32_t skillId,
                                       std::int32_t level, std::uint32_t castId);

// -----------------------------------------------------------------------------
// State
// -----------------------------------------------------------------------------

struct Settings {
    bool enabled      = true;
    int  calcColumn   = CalcColumnDefault;
    bool logDecisions = false;
};

const D2RL::PluginContext* Context{};
std::uintptr_t             Base{};
Settings                   Config{};
std::size_t                CalcOffset = SkillCalc1Offset + (CalcColumnDefault - 1) * SkillCalcStride;
std::atomic<bool>          Active{ false };
std::atomic<bool>          LogDecisions{ false };

SkillsRecordFn    SkillsRecord{};
EvaluateFormulaFn EvaluateFormula{};
SkillLevelFn      SkillLevel{};
DataContextFn     DataContext{};
MissileRecordFn   MissileRecord{};
MissileOwnerFn    MissileOwner{};
MissileDamageFn   MissileDamage{};
IndexedDoFn       IndexedDo{};

SkillStartFn      OriginalSkillStart{};
SkillPerformFn    OriginalSkillPerform{};
StartDispatcherFn OriginalStartDispatcher{};

struct Counters {
    std::atomic<std::uint64_t> evaluated{};   // calc evaluations
    std::atomic<std::uint64_t> changed{};     // of those, an element other than the row's own
    std::atomic<std::uint64_t> reused{};      // decisions reused within a cast or frame
    std::atomic<std::uint64_t> scopes{};      // scopes opened
    std::atomic<std::uint64_t> writes{};      // scopes that actually rewrote the row
    std::atomic<std::uint64_t> missiles{};    // creation scopes on a linked skills.txt row
    std::atomic<std::uint64_t> ownRow{};      // creation scopes on a missile's own missiles.txt EType
    std::atomic<std::uint64_t> hits{};        // hit scopes on a missile's own missiles.txt EType
    std::atomic<std::uint64_t> overflow{};    // scopes refused because the stack was full
};
Counters Stats{};

struct LastDecision {
    std::int32_t  skillId = -1;
    std::int32_t  element = Keep;
    std::int32_t  rowElement = -1;
    std::uint32_t unitType = 0;
    std::uint32_t castId = 0;
    const char*   site = "";
};
LastDecision Last{};
SRWLOCK      LastLock = SRWLOCK_INIT;

template <typename T>
auto Read(const void* base, std::size_t offset) noexcept -> T {
    T value{};
    std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(T));
    return value;
}

auto UnitType(const void* unit) noexcept -> std::uint32_t { return Read<std::uint32_t>(unit, UnitTypeOffset); }
auto CastIdOf(const void* unit) noexcept -> std::uint32_t { return Read<std::uint32_t>(unit, UnitCastIdOffset); }
auto IsServerUnit(const void* unit) noexcept -> bool {
    return unit != nullptr && (Read<std::uint32_t>(unit, UnitFlagsExOffset) & ServerUnitFlag) != 0;
}
auto GameContext(const void* game) noexcept -> std::uint8_t { return Read<std::uint8_t>(game, GameDataContextOffset); }
auto GameFrame(const void* game) noexcept -> std::int32_t { return Read<std::int32_t>(game, GameFrameOffset); }

auto UsedSkillNode(const void* unit) noexcept -> void* {
    const auto* list = Read<const std::uint8_t*>(unit, UnitSkillListOffset);
    return list != nullptr ? Read<void*>(list, SkillListUsedOffset) : nullptr;
}

auto CalcOf(const std::uint8_t* record) noexcept -> std::uint32_t { return Read<std::uint32_t>(record, CalcOffset); }

auto ElementName(std::int32_t element) noexcept -> const char* {
    static constexpr const char* Names[]{ "none", "fire", "ltng", "mag", "cold", "pois", "life", "mana", "stam",
                                          "stun", "rand", "burn", "frze" };
    return (element >= 0 && element <= MaxElement) ? Names[element] : "row";
}

// -----------------------------------------------------------------------------
// Decisions
// -----------------------------------------------------------------------------

// Evaluates the configured calc of the row in the given unit's context. The
// result is an elemtypes.txt row number; 0 or anything outside 1..12 keeps the
// row's own EType.
auto Evaluate(std::uint8_t dataContext, void* contextUnit, const std::uint8_t* record, std::int32_t skillId,
              std::int32_t level, const char* site, std::uint32_t castId) noexcept -> std::int32_t {
    const std::uint32_t calc = CalcOf(record);
    if (calc == BlankCalc) return Keep;
    const std::int32_t value = EvaluateFormula(dataContext, contextUnit, calc, skillId, level);
    const std::int32_t element = (value >= 1 && value <= MaxElement) ? value : Keep;
    const std::int32_t rowElement = record[SkillETypeOffset];
    Stats.evaluated.fetch_add(1, std::memory_order_relaxed);
    if (element != Keep && element != rowElement) Stats.changed.fetch_add(1, std::memory_order_relaxed);
    {
        AcquireSRWLockExclusive(&LastLock);
        Last = { skillId, element, rowElement, UnitType(contextUnit), castId, site };
        ReleaseSRWLockExclusive(&LastLock);
    }
    if (LogDecisions.load(std::memory_order_relaxed) && Context != nullptr) {
        D2RL::LogInfoF(Context,
            "DynamicEType: %s, skill %d level %d, unit type %u, cast %u: calc%d = %d -> %s (row %s)", site,
            skillId, level, UnitType(contextUnit), castId, Config.calcColumn, value, ElementName(element),
            ElementName(rowElement));
    }
    return element;
}

// Decisions per (game, unit, skill), tagged with the cast id and frame they
// were made in. A slot collision just forgets an older decision, which only
// costs one more evaluation.
struct CastDecision {
    const void*   game;
    const void*   unit;
    std::int32_t  skillId;
    std::uint32_t castId;
    std::int32_t  frame;
    std::int32_t  element;
    bool          fromStart;
    bool          used;
};

constexpr std::size_t CastSlots = 1024;
std::array<CastDecision, CastSlots> Casts{};
SRWLOCK CastLock = SRWLOCK_INIT;

auto CastSlot(const void* game, const void* unit, std::int32_t skillId) noexcept -> std::size_t {
    std::uint64_t h = reinterpret_cast<std::uintptr_t>(unit) * 0x9E3779B97F4A7C15ULL;
    h ^= reinterpret_cast<std::uintptr_t>(game) + 0x632BE59BD9B4E019ULL + (h << 6) + (h >> 2);
    h ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(skillId)) * 0xBF58476D1CE4E5B9ULL;
    return static_cast<std::size_t>((h ^ (h >> 31)) % CastSlots);
}

auto FindCast(const void* game, const void* unit, std::int32_t skillId, CastDecision* out) noexcept -> bool {
    AcquireSRWLockShared(&CastLock);
    const CastDecision& slot = Casts[CastSlot(game, unit, skillId)];
    const bool hit = slot.used && slot.game == game && slot.unit == unit && slot.skillId == skillId;
    if (hit) *out = slot;
    ReleaseSRWLockShared(&CastLock);
    return hit;
}

void StoreCast(const void* game, const void* unit, std::int32_t skillId, std::uint32_t castId, std::int32_t frame,
               std::int32_t element, bool fromStart) noexcept {
    AcquireSRWLockExclusive(&CastLock);
    Casts[CastSlot(game, unit, skillId)] = { game, unit, skillId, castId, frame, element, fromStart, true };
    ReleaseSRWLockExclusive(&CastLock);
}

// Decisions per (game, missile cast id, skill row). A missile's damage is
// built once with its cast's element and read again on every hit, so the
// hit needs the same decision possibly seconds later. Four ways per set and
// the oldest entry goes first: a cast's entry is only lost after four newer
// casts landed in the same one of 1024 sets while its missiles still fly.
struct MissileDecision {
    const void*   game;
    std::uint32_t castId;
    std::int32_t  skillId;
    std::int32_t  element;
    std::uint64_t stamp;   // 0 = empty
};

constexpr std::size_t MissileSets = 1024;
constexpr std::size_t MissileWays = 4;
std::array<MissileDecision, MissileSets * MissileWays> Missiles{};
std::uint64_t MissileClock = 0;
SRWLOCK MissileLock = SRWLOCK_INIT;

auto MissileSet(const void* game, std::uint32_t castId, std::int32_t skillId) noexcept -> std::size_t {
    std::uint64_t h = (static_cast<std::uint64_t>(castId) << 20) ^ static_cast<std::uint32_t>(skillId);
    h ^= reinterpret_cast<std::uintptr_t>(game) * 0x9E3779B97F4A7C15ULL;
    h ^= h >> 29;
    return static_cast<std::size_t>(h % MissileSets) * MissileWays;
}

auto FindMissile(const void* game, std::uint32_t castId, std::int32_t skillId, std::int32_t* element) noexcept -> bool {
    AcquireSRWLockShared(&MissileLock);
    const std::size_t set = MissileSet(game, castId, skillId);
    bool hit = false;
    for (std::size_t way = 0; way < MissileWays && !hit; ++way) {
        const MissileDecision& slot = Missiles[set + way];
        hit = slot.stamp != 0 && slot.game == game && slot.castId == castId && slot.skillId == skillId;
        if (hit) *element = slot.element;
    }
    ReleaseSRWLockShared(&MissileLock);
    return hit;
}

void StoreMissile(const void* game, std::uint32_t castId, std::int32_t skillId, std::int32_t element) noexcept {
    AcquireSRWLockExclusive(&MissileLock);
    const std::size_t set = MissileSet(game, castId, skillId);
    std::size_t target = set;
    for (std::size_t way = 0; way < MissileWays; ++way) {
        const MissileDecision& slot = Missiles[set + way];
        if (slot.stamp != 0 && slot.game == game && slot.castId == castId && slot.skillId == skillId) {
            target = set + way;
            break;
        }
        if (slot.stamp < Missiles[target].stamp) target = set + way;  // empty (0) or older
    }
    Missiles[target] = { game, castId, skillId, element, ++MissileClock };
    ReleaseSRWLockExclusive(&MissileLock);
}

// -----------------------------------------------------------------------------
// Scopes
// -----------------------------------------------------------------------------
//   A scope binds one row's EType byte for as long as the game runs code for
//   one cast: a skills.txt row (+0x284) while the cast's own code runs, or a
//   missile's missiles.txt row (+0x10C) while that missile's damage is built
//   or while it hits. Scopes nest strictly (every opener's code returns before
//   the opener does) and each one keeps the value it found and puts it back,
//   so an inner scope on the same byte restores the outer one's element. The
//   stack is per thread; only server threads ever open one.

enum class ScopeKind : std::uint8_t { SkillRow, MissileRow };

struct Scope {
    const void*    contextUnit;  // the unit the decision belongs to (a missile's owner)
    std::int32_t   skillId;      // the skills.txt row the decision belongs to
    std::int32_t   element;      // the decision; Keep = the byte's own value
    std::uint8_t*  etype;
    std::uint8_t   saved;
    bool           wrote;
    ScopeKind      kind;
    std::uintptr_t frame;        // host rsp for the stub-opened scopes, 0 otherwise
};

constexpr int MaxScopes = 64;
thread_local std::array<Scope, MaxScopes> Scopes{};
thread_local int ScopeDepth = 0;

// The cast's own code is running for this skill: its skills.txt row is bound.
auto FindEnclosing(const void* contextUnit, std::int32_t skillId) noexcept -> const Scope* {
    for (int index = ScopeDepth - 1; index >= 0; --index) {
        const Scope& scope = Scopes[static_cast<std::size_t>(index)];
        if (scope.kind == ScopeKind::SkillRow && scope.contextUnit == contextUnit && scope.skillId == skillId) {
            return &scope;
        }
    }
    return nullptr;
}

// The byte's value before any open scope bound it: the outermost scope on it
// saved that, otherwise it is the current value.
auto OwnValue(const std::uint8_t* etype) noexcept -> std::uint8_t {
    for (int index = 0; index < ScopeDepth; ++index) {
        const Scope& scope = Scopes[static_cast<std::size_t>(index)];
        if (scope.etype == etype) return scope.saved;
    }
    return *etype;
}

// Keep binds the byte's own value, so a cast that keeps its EType sees the
// row's element even inside another cast's scope on the same row.
auto OpenScopeAt(const void* contextUnit, std::int32_t skillId, std::uint8_t* etype, std::int32_t element,
                 std::uintptr_t frame, ScopeKind kind) noexcept -> bool {
    if (ScopeDepth >= MaxScopes) {
        Stats.overflow.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const std::uint8_t target = element != Keep ? static_cast<std::uint8_t>(element) : OwnValue(etype);
    Scope& scope = Scopes[static_cast<std::size_t>(ScopeDepth++)];
    scope.contextUnit = contextUnit;
    scope.skillId = skillId;
    scope.element = element;
    scope.etype = etype;
    scope.saved = *etype;
    scope.wrote = false;
    scope.kind = kind;
    scope.frame = frame;
    if (target != scope.saved) {
        *etype = target;
        scope.wrote = true;
        Stats.writes.fetch_add(1, std::memory_order_relaxed);
    }
    Stats.scopes.fetch_add(1, std::memory_order_relaxed);
    return true;
}

auto OpenScope(const void* contextUnit, std::int32_t skillId, std::uint8_t* record, std::int32_t element,
               std::uintptr_t frame) noexcept -> bool {
    return OpenScopeAt(contextUnit, skillId, record + SkillETypeOffset, element, frame, ScopeKind::SkillRow);
}

// A missile's own missiles.txt EType. Opened when the cast decided an element,
// or when an outer scope bound this row and the missile needs the row's own.
auto OpenMissileRowScope(const void* owner, std::int32_t skillId, std::uint8_t* missileRow, std::int32_t element,
                         std::uintptr_t frame) noexcept -> bool {
    std::uint8_t* etype = missileRow + MissileETypeOffset;
    if (element == Keep && OwnValue(etype) == *etype) return false;
    return OpenScopeAt(owner, skillId, etype, element, frame, ScopeKind::MissileRow);
}

void CloseScope() noexcept {
    if (ScopeDepth <= 0) return;
    const Scope& scope = Scopes[static_cast<std::size_t>(--ScopeDepth)];
    if (scope.wrote) *scope.etype = scope.saved;
}

struct ScopeGuard {
    bool open;
    ~ScopeGuard() { if (open) CloseScope(); }
};

// -----------------------------------------------------------------------------
// Where the element comes from
// -----------------------------------------------------------------------------

enum class Site { Start, Perform, Dispatch, Do, Indexed };

auto SiteName(Site site) noexcept -> const char* {
    switch (site) {
    case Site::Start:    return "skill start";
    case Site::Perform:  return "skill perform";
    case Site::Dispatch: return "start function";
    case Site::Do:       return "do-handler";
    case Site::Indexed:  return "indexed skill function";
    }
    return "?";
}

// The element for a unit's own cast. Start always decides anew (a new cast is
// starting). Perform reuses the start's decision for the same cast id. The
// rest reuse a decision of the same cast id made in the same frame, so a proc's
// start function and its do-handler agree, and every aura pulse decides again.
auto UnitElement(Site site, void* game, void* unit, std::int32_t skillId, const std::uint8_t* record,
                 std::int32_t level, std::uint32_t castId) noexcept -> std::int32_t {
    const std::int32_t frame = GameFrame(game);
    if (site != Site::Start) {
        CastDecision found{};
        if (FindCast(game, unit, skillId, &found) && found.castId == castId
                && (site == Site::Perform ? found.fromStart : found.frame == frame)) {
            Stats.reused.fetch_add(1, std::memory_order_relaxed);
            return found.element;
        }
    }
    const std::int32_t element = Evaluate(GameContext(game), unit, record, skillId, level, SiteName(site), castId);
    StoreCast(game, unit, skillId, castId, frame, element, site == Site::Start);
    return element;
}

// Shared by the start and perform hooks: the used skill, its record and the
// level the row's own element gives.
struct UsedSkill {
    void*         node;
    std::int32_t  skillId;
    std::uint8_t* record;
};

auto ReadUsedSkill(void* game, void* unit, UsedSkill* out) noexcept -> bool {
    if (!Active.load(std::memory_order_relaxed) || game == nullptr || unit == nullptr || !IsServerUnit(unit)) {
        return false;
    }
    void* node = UsedSkillNode(unit);
    if (node == nullptr) return false;
    const auto* nodeRecord = Read<const std::uint8_t*>(node, SkillNodeRecordOffset);
    if (nodeRecord == nullptr) return false;
    const std::int32_t skillId = Read<std::int16_t>(nodeRecord, SkillIdOffset);
    if (skillId < 0) return false;
    std::uint8_t* record = SkillsRecord(GameContext(game), skillId);
    if (record == nullptr || CalcOf(record) == BlankCalc) return false;
    if (FindEnclosing(unit, skillId) != nullptr) return false;
    *out = { node, skillId, record };
    return true;
}

// -----------------------------------------------------------------------------
// Hooks
// -----------------------------------------------------------------------------

auto __fastcall HookedSkillStart(void* game, void* unit) noexcept -> std::int64_t {
    UsedSkill used{};
    if (!ReadUsedSkill(game, unit, &used)) return OriginalSkillStart(game, unit);
    const std::int32_t level = SkillLevel(unit, used.node, 1, 0);
    const std::int32_t element = UnitElement(Site::Start, game, unit, used.skillId, used.record, level, CastIdOf(unit));
    const ScopeGuard guard{ OpenScope(unit, used.skillId, used.record, element, 0) };
    return OriginalSkillStart(game, unit);
}

auto __fastcall HookedSkillPerform(void* game, void* unit) noexcept -> void* {
    UsedSkill used{};
    if (!ReadUsedSkill(game, unit, &used)) return OriginalSkillPerform(game, unit);
    const std::int32_t level = SkillLevel(unit, used.node, 1, 0);
    const std::int32_t element = UnitElement(Site::Perform, game, unit, used.skillId, used.record, level,
                                             CastIdOf(unit));
    const ScopeGuard guard{ OpenScope(unit, used.skillId, used.record, element, 0) };
    return OriginalSkillPerform(game, unit);
}

auto __fastcall HookedStartDispatcher(void* game, void* unit, std::int32_t skillId, std::int32_t level,
                                      std::int32_t flag) noexcept -> std::int64_t {
    if (!Active.load(std::memory_order_relaxed) || game == nullptr || unit == nullptr || skillId < 0
            || !IsServerUnit(unit) || FindEnclosing(unit, skillId) != nullptr) {
        return OriginalStartDispatcher(game, unit, skillId, level, flag);
    }
    std::uint8_t* record = SkillsRecord(GameContext(game), skillId);
    if (record == nullptr || CalcOf(record) == BlankCalc) return OriginalStartDispatcher(game, unit, skillId, level, flag);
    const std::int32_t element = UnitElement(Site::Dispatch, game, unit, skillId, record, level, CastIdOf(unit));
    const ScopeGuard guard{ OpenScope(unit, skillId, record, element, 0) };
    return OriginalStartDispatcher(game, unit, skillId, level, flag);
}

// Stub A, after the do-handler fetched the record. frame = the do-handler's rsp.
void __fastcall DoScopeOpen(void* game, void* unit, std::int32_t skillId, std::uint8_t* record, std::uintptr_t frame,
                            std::int32_t level) noexcept {
    if (!Active.load(std::memory_order_relaxed) || game == nullptr || unit == nullptr || record == nullptr
            || CalcOf(record) == BlankCalc || !IsServerUnit(unit) || FindEnclosing(unit, skillId) != nullptr) {
        return;
    }
    const std::int32_t element = UnitElement(Site::Do, game, unit, skillId, record, level, CastIdOf(unit));
    OpenScope(unit, skillId, record, element, frame);
}

// Stub B, at the do-handler's epilogue. Closes only the scope stub A opened in
// this same call; anything else on the stack belongs to an outer frame.
void __fastcall DoScopeClose(std::uintptr_t frame) noexcept {
    if (ScopeDepth <= 0) return;
    const Scope& top = Scopes[static_cast<std::size_t>(ScopeDepth - 1)];
    if (top.kind == ScopeKind::SkillRow && top.frame == frame) CloseScope();
}

// The skills.txt row the damage builder 0x3BF9B0 reads EType from, or -1 when
// it reads the missile's own missiles.txt EType. Mirrors 3BFACE..3BFE46:
// Skill > 0 links the row; the MissileSkill flag prefers the skill the missile
// was created for, falling back to Skill.
auto LinkedSkillRow(void* missile, const std::uint8_t* row) noexcept -> std::int32_t {
    if (missile == nullptr || row == nullptr) return -1;
    const std::int32_t skill = Read<std::int16_t>(row, MissileSkillOffset);
    const bool         flag  = (row[MissileFlagsOffset] & MissileSkillFlag) != 0;
    if (!flag) return skill > 0 ? skill : -1;
    const auto* data = Read<const std::uint8_t*>(missile, UnitDataOffset);
    if (data != nullptr) {
        const std::int32_t created = Read<std::int16_t>(data, MissileDataSkillOffset);
        if (created > 0) return created;
    }
    return skill >= 0 ? skill : -1;
}

// The skill the missile was created for (missile data +0xC). Sub-missiles get
// their parent's, so every missile of a cast names the cast's skill.
auto CreatedForSkill(void* missile) noexcept -> std::int32_t {
    const auto* data = Read<const std::uint8_t*>(missile, UnitDataOffset);
    return data != nullptr ? Read<std::int16_t>(data, MissileDataSkillOffset) : -1;
}

auto MissileLevel(void* missile) noexcept -> std::int32_t {
    const auto* data = Read<const std::uint8_t*>(missile, UnitDataOffset);
    return data != nullptr ? Read<std::int16_t>(data, MissileDataLevelOffset) : 0;
}

// The skills.txt row whose decision a missile follows: its linked row when the
// builder takes the element from a skills.txt row, otherwise the skill it was
// created for (its element then comes from its own missiles.txt EType).
auto DecisionRow(void* missile, const std::uint8_t* missileRow, bool* linked) noexcept -> std::int32_t {
    const std::int32_t row = LinkedSkillRow(missile, missileRow);
    *linked = row >= 0;
    return *linked ? row : CreatedForSkill(missile);
}

// The cast's element for one of its missiles, kept under (game, cast id, row)
// for the missile's hits and for sub-missiles created later. Asked in order:
// the cast's own code running right now, a decision already kept for this
// cast's missiles, the decision the cast made for its caster, and only then a
// new evaluation in the owner's context.
auto MissileCastElement(void* game, void* owner, std::int32_t skillId, const std::uint8_t* record,
                        std::uint32_t castId, std::int32_t level, const char* site) noexcept -> std::int32_t {
    std::int32_t element = Keep;
    if (const Scope* enclosing = FindEnclosing(owner, skillId); enclosing != nullptr) {
        element = enclosing->element;
        StoreMissile(game, castId, skillId, element);
        Stats.reused.fetch_add(1, std::memory_order_relaxed);
        return element;
    }
    if (FindMissile(game, castId, skillId, &element)) {
        Stats.reused.fetch_add(1, std::memory_order_relaxed);
        return element;
    }
    CastDecision cast{};
    if (FindCast(game, owner, skillId, &cast) && cast.castId == castId) {
        StoreMissile(game, castId, skillId, cast.element);
        Stats.reused.fetch_add(1, std::memory_order_relaxed);
        return cast.element;
    }
    element = Evaluate(GameContext(game), owner, record, skillId, level, site, castId);
    StoreMissile(game, castId, skillId, element);
    return element;
}

// In place of the creator's call to the damage builder 0x3BF9B0, same
// signature. The new missile already carries its cast id, skill and level.
//   Linked missile: the builder reads the skills.txt row's EType (3BFF5F),
//   so that row is bound, unless the cast's own code already bound it.
//   Missile with its own damage: the builder reads the missile's missiles.txt
//   EType (3BFB5C) and puts EMin/EMax and its ELen under that element, so that
//   byte is bound to the element of the cast the missile was created for.
//   A blank missiles.txt EType has no element to follow and stays blank.
auto __fastcall HookedMissileDamage(void* damage, void* owner, void* source, void* missile, std::int32_t level,
                                    std::uint8_t flagA, std::uint8_t flagB) -> std::int64_t {
    if (!Active.load(std::memory_order_relaxed) || owner == nullptr || missile == nullptr || !IsServerUnit(owner)
            || UnitType(missile) != UnitTypeMissile) {
        return MissileDamage(damage, owner, source, missile, level, flagA, flagB);
    }
    void* game = Read<void*>(missile, UnitGameOffset);
    if (game == nullptr) game = Read<void*>(owner, UnitGameOffset);
    if (game == nullptr) return MissileDamage(damage, owner, source, missile, level, flagA, flagB);
    const std::uint8_t dataContext = DataContext(missile);
    std::uint8_t* missileRow = MissileRecord(dataContext, Read<std::int32_t>(missile, UnitClassOffset));
    if (missileRow == nullptr) return MissileDamage(damage, owner, source, missile, level, flagA, flagB);

    bool linked = false;
    const std::int32_t skillId = DecisionRow(missile, missileRow, &linked);
    std::uint8_t* record = skillId >= 0 ? SkillsRecord(dataContext, skillId) : nullptr;
    const bool dynamic = record != nullptr && CalcOf(record) != BlankCalc;
    const std::uint32_t castId = CastIdOf(missile);

    if (linked) {
        if (!dynamic) return MissileDamage(damage, owner, source, missile, level, flagA, flagB);
        if (FindEnclosing(owner, skillId) != nullptr) {
            // Created by the cast's own code: the row is already bound.
            MissileCastElement(game, owner, skillId, record, castId, level, "missile");
            return MissileDamage(damage, owner, source, missile, level, flagA, flagB);
        }
        const std::int32_t element = MissileCastElement(game, owner, skillId, record, castId, level, "missile");
        Stats.missiles.fetch_add(1, std::memory_order_relaxed);
        const ScopeGuard guard{ OpenScope(owner, skillId, record, element, 0) };
        return MissileDamage(damage, owner, source, missile, level, flagA, flagB);
    }

    std::int32_t element = Keep;
    if (dynamic && OwnValue(missileRow + MissileETypeOffset) != 0) {
        element = MissileCastElement(game, owner, skillId, record, castId, level, "missile (own EType)");
    }
    const bool opened = OpenMissileRowScope(owner, skillId, missileRow, element, 0);
    if (opened) Stats.ownRow.fetch_add(1, std::memory_order_relaxed);
    const ScopeGuard guard{ opened };
    return MissileDamage(damage, owner, source, missile, level, flagA, flagB);
}

// Stub C, after the hit resolution 0x4639A0 fetched the hitting missile's
// missiles.txt row. frame = the hit resolution's rsp. For the whole hit, the
// missile's own EType carries its cast's element: the hit and damage
// functions that read it (3BB060) then look for the damage, the length and
// the conversion target where the creation put them.
void __fastcall HitScopeOpen(void* game, void* missile, std::uint8_t* missileRow, std::uintptr_t frame) noexcept {
    if (!Active.load(std::memory_order_relaxed) || game == nullptr || missile == nullptr || missileRow == nullptr
            || UnitType(missile) != UnitTypeMissile) {
        return;
    }
    std::uint8_t* etype = missileRow + MissileETypeOffset;
    const std::uint8_t own = OwnValue(etype);
    const bool bound = own != *etype;
    if (own == 0 && !bound) return;  // no element of its own, nothing bound: the common case

    std::int32_t element = Keep;
    void* owner = nullptr;
    bool linked = false;
    const std::int32_t skillId = DecisionRow(missile, missileRow, &linked);
    if (skillId >= 0 && own != 0) {
        std::uint8_t* record = SkillsRecord(GameContext(game), skillId);
        if (record != nullptr && CalcOf(record) != BlankCalc) {
            const std::uint32_t castId = CastIdOf(missile);
            if (FindMissile(game, castId, skillId, &element)) {
                Stats.reused.fetch_add(1, std::memory_order_relaxed);
            } else {
                // No longer kept (or created before the plugin was active):
                // decide the way its creation would have.
                owner = MissileOwner(game, missile);
                element = (owner != nullptr && IsServerUnit(owner))
                    ? MissileCastElement(game, owner, skillId, record, castId, MissileLevel(missile), "missile hit")
                    : Keep;
            }
        }
    }
    if (element == Keep && !bound) return;
    if (OpenMissileRowScope(owner, skillId, missileRow, element, frame)) {
        Stats.hits.fetch_add(1, std::memory_order_relaxed);
    }
}

// Stub D, at the hit resolution's epilogue. Closes only the scope stub C
// opened in this same call.
void __fastcall HitScopeClose(std::uintptr_t frame) noexcept {
    if (ScopeDepth <= 0) return;
    const Scope& top = Scopes[static_cast<std::size_t>(ScopeDepth - 1)];
    if (top.kind == ScopeKind::MissileRow && top.frame == frame) CloseScope();
}

// In place of the missile functions' calls to the indexed do 0x435490, same
// signature. The unit is usually the missile itself; the decision then belongs
// to its owner and to the missile's cast.
void __fastcall HookedIndexedDo(void* game, void* unit, std::int32_t function, std::int32_t skillId,
                                std::int32_t level, std::uint32_t castId) {
    if (!Active.load(std::memory_order_relaxed) || game == nullptr || unit == nullptr || skillId < 0
            || !IsServerUnit(unit)) {
        IndexedDo(game, unit, function, skillId, level, castId);
        return;
    }
    std::uint8_t* record = SkillsRecord(GameContext(game), skillId);
    if (record == nullptr || CalcOf(record) == BlankCalc) {
        IndexedDo(game, unit, function, skillId, level, castId);
        return;
    }

    const bool missile = UnitType(unit) == UnitTypeMissile;
    void* contextUnit = missile ? MissileOwner(game, unit) : unit;
    if (contextUnit == nullptr || FindEnclosing(contextUnit, skillId) != nullptr) {
        IndexedDo(game, unit, function, skillId, level, castId);
        return;
    }
    std::int32_t element = Keep;
    if (missile) {
        if (FindMissile(game, castId, skillId, &element)) {
            Stats.reused.fetch_add(1, std::memory_order_relaxed);
        } else {
            element = Evaluate(GameContext(game), contextUnit, record, skillId, level, "missile skill function",
                               castId);
            StoreMissile(game, castId, skillId, element);
        }
    } else {
        element = UnitElement(Site::Indexed, game, unit, skillId, record, level, castId);
    }
    const ScopeGuard guard{ OpenScope(contextUnit, skillId, record, element, 0) };
    IndexedDo(game, unit, function, skillId, level, castId);
}

// -----------------------------------------------------------------------------
// Stub region
// -----------------------------------------------------------------------------
//   One allocation within rel32 reach of every site, never freed (a thread may
//   be inside it when the plugin unloads):
//     +000  zero, so no RUNTIME_FUNCTION has UnwindData 0
//     +010  UNWIND_INFO, alloc 38h (stubs A and C)
//     +018  UNWIND_INFO, alloc 28h (stubs B and D)
//     +020  RUNTIME_FUNCTION table, 4 entries (to +050)
//     +060  thunk: jmp HookedMissileDamage     +080  thunk: jmp HookedIndexedDo
//     +100  stub A (69 bytes), slots +148 skills.txt record getter, +150 DoScopeOpen
//     +180  stub B (38 bytes), slot +1A8 DoScopeClose
//     +200  stub C (50 bytes), slots +248 missiles.txt record getter, +250 HitScopeOpen
//     +280  stub D (37 bytes), slot +2A8 HitScopeClose
//
//   All four stubs were assembled with keystone, decoded back with capstone
//   and emulated with unicorn: arguments, 16-byte alignment at every call,
//   the record and return value kept, rbx replayed from [host rsp+0C8h] (B),
//   rcx replayed from [rbp+110h] (D), every non-volatile register untouched,
//   and the null-record paths skipping the scope.

constexpr std::size_t RegionBytes         = 0x1000;
constexpr std::size_t UnwindAOffset       = 0x010;
constexpr std::size_t UnwindBOffset       = 0x018;
constexpr std::size_t FunctionTableOffset = 0x020;
constexpr std::size_t FunctionTableCount  = 4;
constexpr std::size_t MissileThunkOffset  = 0x060;
constexpr std::size_t IndexedThunkOffset  = 0x080;
constexpr std::size_t StubAOffset         = 0x100;
constexpr std::size_t StubBOffset         = 0x180;
constexpr std::size_t StubCOffset         = 0x200;
constexpr std::size_t StubDOffset         = 0x280;
static_assert(FunctionTableOffset + FunctionTableCount * 12 <= MissileThunkOffset);

// Entered by the call at 43ACFC (return address 43AD01).
constexpr std::uint8_t StubA[]{
    0x48, 0x83, 0xEC, 0x38,                    // sub  rsp, 38h
    0xFF, 0x15, 0x3E, 0x00, 0x00, 0x00,        // call [rip+3Eh]          ; +48: record getter 97790
    0x48, 0x85, 0xC0,                          // test rax, rax
    0x74, 0x31,                                // jz   +40h
    0x48, 0x89, 0x44, 0x24, 0x30,              // mov  [rsp+30h], rax     ; the record
    0x49, 0x89, 0xC1,                          // mov  r9, rax            ; record
    0x48, 0x8D, 0x44, 0x24, 0x40,              // lea  rax, [rsp+40h]     ; the do-handler's rsp
    0x48, 0x89, 0x44, 0x24, 0x20,              // mov  [rsp+20h], rax     ; frame
    0x8B, 0x80, 0xD8, 0x00, 0x00, 0x00,        // mov  eax, [rax+0D8h]    ; level argument
    0x48, 0x89, 0x44, 0x24, 0x28,              // mov  [rsp+28h], rax     ; level
    0x48, 0x89, 0xF9,                          // mov  rcx, rdi           ; game
    0x48, 0x89, 0xDA,                          // mov  rdx, rbx           ; unit
    0x45, 0x89, 0xE8,                          // mov  r8d, r13d          ; skill id
    0xFF, 0x15, 0x15, 0x00, 0x00, 0x00,        // call [rip+15h]          ; +50: DoScopeOpen
    0x48, 0x8B, 0x44, 0x24, 0x30,              // mov  rax, [rsp+30h]     ; the record, as the game expects
    0x48, 0x83, 0xC4, 0x38,                    // add  rsp, 38h
    0xC3,                                      // ret
};
constexpr std::size_t StubARecordSlot = 0x48;
constexpr std::size_t StubAOpenSlot   = 0x50;
static_assert(sizeof(StubA) == 0x45 && StubARecordSlot >= sizeof(StubA));
static_assert(StubA[0x06] + 0x0A == StubARecordSlot && StubA[0x37] + 0x3B == StubAOpenSlot);

// Entered by the call at 43AE1A (return address 43AE1F, three NOPs, then
// add rsp,80h at 43AE22).
constexpr std::uint8_t StubB[]{
    0x48, 0x83, 0xEC, 0x28,                    // sub  rsp, 28h
    0x48, 0x89, 0x44, 0x24, 0x20,              // mov  [rsp+20h], rax     ; the do-handler's result
    0x48, 0x8D, 0x4C, 0x24, 0x30,              // lea  rcx, [rsp+30h]     ; the do-handler's rsp
    0xFF, 0x15, 0x14, 0x00, 0x00, 0x00,        // call [rip+14h]          ; +28: DoScopeClose
    0x48, 0x8B, 0x44, 0x24, 0x20,              // mov  rax, [rsp+20h]
    0x48, 0x8B, 0x9C, 0x24, 0xF8, 0x00, 0x00, 0x00,  // mov rbx, [rsp+0F8h]  ; replays mov rbx,[rsp+0C8h]
    0x48, 0x83, 0xC4, 0x28,                    // add  rsp, 28h
    0xC3,                                      // ret
};
constexpr std::size_t StubBCloseSlot = 0x28;
static_assert(sizeof(StubB) == 0x26 && StubBCloseSlot >= sizeof(StubB) && StubB[0x10] + 0x14 == StubBCloseSlot);
static_assert(StubBOffset + StubBCloseSlot + 8 <= RegionBytes && StubAOffset + StubAOpenSlot + 8 <= StubBOffset);

// Entered by the call at 4639FE (return address 463A03). r12 = game,
// r14 = missile, rcx/edx already hold the fetch's arguments.
constexpr std::uint8_t StubC[]{
    0x48, 0x83, 0xEC, 0x38,                    // sub  rsp, 38h
    0xFF, 0x15, 0x3E, 0x00, 0x00, 0x00,        // call [rip+3Eh]          ; +48: record getter 166040
    0x48, 0x85, 0xC0,                          // test rax, rax
    0x74, 0x1E,                                // jz   +2Dh
    0x48, 0x89, 0x44, 0x24, 0x30,              // mov  [rsp+30h], rax     ; the missiles.txt row
    0x49, 0x89, 0xC0,                          // mov  r8, rax            ; row
    0x4C, 0x8D, 0x4C, 0x24, 0x40,              // lea  r9, [rsp+40h]      ; the hit resolution's rsp
    0x4C, 0x89, 0xE1,                          // mov  rcx, r12           ; game
    0x4C, 0x89, 0xF2,                          // mov  rdx, r14           ; missile
    0xFF, 0x15, 0x28, 0x00, 0x00, 0x00,        // call [rip+28h]          ; +50: HitScopeOpen
    0x48, 0x8B, 0x44, 0x24, 0x30,              // mov  rax, [rsp+30h]     ; the row, as the game expects
    0x48, 0x83, 0xC4, 0x38,                    // add  rsp, 38h
    0xC3,                                      // ret
};
constexpr std::size_t StubCRecordSlot = 0x48;
constexpr std::size_t StubCOpenSlot   = 0x50;
static_assert(sizeof(StubC) == 0x32 && StubCRecordSlot >= sizeof(StubC));
static_assert(StubC[0x06] + 0x0A == StubCRecordSlot && StubC[0x24] + 0x28 == StubCOpenSlot);

// Entered by the call at 463AEB (return address 463AF0, two NOPs, then
// xor rcx,rsp at 463AF2 and the cookie check).
constexpr std::uint8_t StubD[]{
    0x48, 0x83, 0xEC, 0x28,                    // sub  rsp, 28h
    0x48, 0x89, 0x44, 0x24, 0x20,              // mov  [rsp+20h], rax     ; the hit result
    0x48, 0x8D, 0x4C, 0x24, 0x30,              // lea  rcx, [rsp+30h]     ; the hit resolution's rsp
    0xFF, 0x15, 0x14, 0x00, 0x00, 0x00,        // call [rip+14h]          ; +28: HitScopeClose
    0x48, 0x8B, 0x44, 0x24, 0x20,              // mov  rax, [rsp+20h]
    0x48, 0x8B, 0x8D, 0x10, 0x01, 0x00, 0x00,  // mov  rcx, [rbp+110h]    ; replays the patched instruction
    0x48, 0x83, 0xC4, 0x28,                    // add  rsp, 28h
    0xC3,                                      // ret
};
constexpr std::size_t StubDCloseSlot = 0x28;
static_assert(sizeof(StubD) == 0x25 && StubDCloseSlot >= sizeof(StubD) && StubD[0x10] + 0x14 == StubDCloseSlot);
static_assert(StubCOffset + StubCOpenSlot + 8 <= StubDOffset && StubDOffset + StubDCloseSlot + 8 <= RegionBytes);

// version 1, prolog 4 bytes, one code: at +4, UWOP_ALLOC_SMALL.
constexpr std::uint8_t UnwindA[]{ 0x01, 0x04, 0x01, 0x00, 0x04, 0x62, 0x00, 0x00 };  // 38h
constexpr std::uint8_t UnwindB[]{ 0x01, 0x04, 0x01, 0x00, 0x04, 0x42, 0x00, 0x00 };  // 28h

std::uint8_t* Region{};
bool          UnwindRegistered{};
bool          DoRecordPatched{};
bool          DoEpiloguePatched{};
bool          MissileDamagePatched{};
bool          IndexedDoPatchedA{};
bool          IndexedDoPatchedB{};
bool          HitRecordPatched{};
bool          HitEpiloguePatched{};
bool          HooksInstalled{};

auto CanEncodeRel32(std::uintptr_t from, std::uintptr_t to) noexcept -> bool {
    const std::int64_t delta = static_cast<std::int64_t>(to) - static_cast<std::int64_t>(from + CallSize);
    return delta >= std::numeric_limits<std::int32_t>::min() && delta <= std::numeric_limits<std::int32_t>::max();
}

auto AllocateNear(std::uintptr_t low, std::uintptr_t high, std::size_t size) noexcept -> std::uint8_t* {
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const auto granularity = static_cast<std::uintptr_t>(system.dwAllocationGranularity);
    const auto aligned = high & ~(granularity - 1U);
    for (std::uintptr_t delta = granularity; delta < 0x7000'0000ULL; delta += granularity) {
        const auto candidate = aligned + delta;
        if (!CanEncodeRel32(low, candidate + size)) break;
        if (void* memory = VirtualAlloc(reinterpret_cast<void*>(candidate), size, MEM_COMMIT | MEM_RESERVE,
                PAGE_READWRITE)) {
            return static_cast<std::uint8_t*>(memory);
        }
    }
    return nullptr;
}

void WriteQword(std::uint8_t* at, std::uint64_t value) noexcept { std::memcpy(at, &value, sizeof(value)); }
void WriteDword(std::uint8_t* at, std::uint32_t value) noexcept { std::memcpy(at, &value, sizeof(value)); }

void EmitThunk(std::uint8_t* at, const void* target) noexcept {
    const std::uint8_t jump[]{ 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };  // jmp [rip+0]
    std::memcpy(at, jump, sizeof(jump));
    WriteQword(at + sizeof(jump), reinterpret_cast<std::uint64_t>(target));
}

auto BuildRegion() noexcept -> bool {
    const std::uintptr_t low  = Base + DoRecordCallRva;       // the lowest site
    const std::uintptr_t high = Base + MissileDamageCallRva;  // the highest site
    Region = AllocateNear(low, high, RegionBytes);
    if (Region == nullptr) {
        Context->LogError("DynamicEType: no memory was free within rel32 reach of the hook sites.");
        return false;
    }
    std::memset(Region, 0xCC, RegionBytes);
    std::memset(Region, 0, MissileThunkOffset);
    std::memcpy(Region + UnwindAOffset, UnwindA, sizeof(UnwindA));
    std::memcpy(Region + UnwindBOffset, UnwindB, sizeof(UnwindB));
    const std::uint32_t table[FunctionTableCount][3]{
        { static_cast<std::uint32_t>(StubAOffset), static_cast<std::uint32_t>(StubAOffset + sizeof(StubA)),
          static_cast<std::uint32_t>(UnwindAOffset) },
        { static_cast<std::uint32_t>(StubBOffset), static_cast<std::uint32_t>(StubBOffset + sizeof(StubB)),
          static_cast<std::uint32_t>(UnwindBOffset) },
        { static_cast<std::uint32_t>(StubCOffset), static_cast<std::uint32_t>(StubCOffset + sizeof(StubC)),
          static_cast<std::uint32_t>(UnwindAOffset) },
        { static_cast<std::uint32_t>(StubDOffset), static_cast<std::uint32_t>(StubDOffset + sizeof(StubD)),
          static_cast<std::uint32_t>(UnwindBOffset) },
    };
    for (std::size_t entry = 0; entry < FunctionTableCount; ++entry) {
        for (std::size_t field = 0; field < 3; ++field) {
            WriteDword(Region + FunctionTableOffset + entry * sizeof(RUNTIME_FUNCTION) + field * 4,
                       table[entry][field]);
        }
    }
    EmitThunk(Region + MissileThunkOffset, reinterpret_cast<const void*>(&HookedMissileDamage));
    EmitThunk(Region + IndexedThunkOffset, reinterpret_cast<const void*>(&HookedIndexedDo));
    std::memcpy(Region + StubAOffset, StubA, sizeof(StubA));
    WriteQword(Region + StubAOffset + StubARecordSlot, Base + SkillsRecordRva);
    WriteQword(Region + StubAOffset + StubAOpenSlot, reinterpret_cast<std::uint64_t>(&DoScopeOpen));
    std::memcpy(Region + StubBOffset, StubB, sizeof(StubB));
    WriteQword(Region + StubBOffset + StubBCloseSlot, reinterpret_cast<std::uint64_t>(&DoScopeClose));
    std::memcpy(Region + StubCOffset, StubC, sizeof(StubC));
    WriteQword(Region + StubCOffset + StubCRecordSlot, Base + MissileRecordRva);
    WriteQword(Region + StubCOffset + StubCOpenSlot, reinterpret_cast<std::uint64_t>(&HitScopeOpen));
    std::memcpy(Region + StubDOffset, StubD, sizeof(StubD));
    WriteQword(Region + StubDOffset + StubDCloseSlot, reinterpret_cast<std::uint64_t>(&HitScopeClose));

    DWORD previous = 0;
    if (!VirtualProtect(Region, RegionBytes, PAGE_EXECUTE_READ, &previous)) {
        Context->LogError("DynamicEType: the stub region could not be made executable.");
        VirtualFree(Region, 0, MEM_RELEASE);
        Region = nullptr;
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), Region, RegionBytes);
    UnwindRegistered = RtlAddFunctionTable(reinterpret_cast<PRUNTIME_FUNCTION>(Region + FunctionTableOffset),
                                           static_cast<DWORD>(FunctionTableCount),
                                           reinterpret_cast<DWORD64>(Region)) != FALSE;
    if (!UnwindRegistered) {
        Context->LogWarn("DynamicEType: unwind data for the stubs was not registered; stack walks stop inside them.");
    }
    return true;
}

void EncodeCall(std::uint64_t siteRva, std::size_t regionOffset, std::uint8_t* out) noexcept {
    const std::uintptr_t site = Base + siteRva;
    const auto displacement = static_cast<std::int32_t>(
        static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(Region) + regionOffset)
        - static_cast<std::int64_t>(site + CallSize));
    out[0] = 0xE8;
    std::memcpy(out + 1, &displacement, sizeof(displacement));
}

// Replaces `size` original bytes at siteRva with a call into the region, NOPs
// after it. The original bytes are the expected bytes, so a site something
// else already changed is never written.
auto RedirectSite(std::uint64_t siteRva, const std::uint8_t* original, std::size_t size, std::size_t regionOffset)
    -> bool {
    if (!CanEncodeRel32(Base + siteRva, reinterpret_cast<std::uintptr_t>(Region) + regionOffset)) return false;
    std::uint8_t patch[16]{};
    std::memset(patch, 0x90, sizeof(patch));
    EncodeCall(siteRva, regionOffset, patch);
    return Context->PatchBytes(siteRva, original, static_cast<std::uint32_t>(size), patch,
                               static_cast<std::uint32_t>(size));
}

void RestoreSite(std::uint64_t siteRva, const std::uint8_t* original, std::size_t size, std::size_t regionOffset,
                 bool& patched) {
    if (!patched || Region == nullptr) return;
    std::uint8_t current[16]{};
    std::memset(current, 0x90, sizeof(current));
    EncodeCall(siteRva, regionOffset, current);
    if (Context->PatchBytes(siteRva, current, static_cast<std::uint32_t>(size), original,
                            static_cast<std::uint32_t>(size))) {
        patched = false;
    }
}

void RemovePatches() {
    RestoreSite(HitRecordCallRva, HitRecordWindow + HitRecordCallOffset, CallSize, StubCOffset, HitRecordPatched);
    RestoreSite(HitEpilogueRva, HitEpilogueWindow + HitEpilogueOffset, HitEpilogueSize, StubDOffset,
                HitEpiloguePatched);
    RestoreSite(IndexedDoCallBRva, IndexedDoWindowB + IndexedDoCallOffsetB, CallSize, IndexedThunkOffset,
                IndexedDoPatchedB);
    RestoreSite(IndexedDoCallARva, IndexedDoWindowA + IndexedDoCallOffsetA, CallSize, IndexedThunkOffset,
                IndexedDoPatchedA);
    RestoreSite(MissileDamageCallRva, MissileDamageWindow + MissileDamageCallOffset, CallSize, MissileThunkOffset,
                MissileDamagePatched);
    RestoreSite(DoEpilogueRva, DoEpilogueWindow + DoEpilogueOffset, DoEpilogueSize, StubBOffset, DoEpiloguePatched);
    RestoreSite(DoRecordCallRva, DoRecordWindow + DoRecordCallOffset, CallSize, StubAOffset, DoRecordPatched);
    // The region stays: a thread may be inside a stub right now.
}

// Order matters: each epilogue (closer) goes in before its record fetch
// (opener), so no scope can ever be opened that nothing closes; removal runs
// the other way round.
auto InstallPatches() -> bool {
    DoEpiloguePatched = RedirectSite(DoEpilogueRva, DoEpilogueWindow + DoEpilogueOffset, DoEpilogueSize, StubBOffset);
    if (!DoEpiloguePatched) return false;
    DoRecordPatched = RedirectSite(DoRecordCallRva, DoRecordWindow + DoRecordCallOffset, CallSize, StubAOffset);
    if (!DoRecordPatched) return false;
    MissileDamagePatched = RedirectSite(MissileDamageCallRva, MissileDamageWindow + MissileDamageCallOffset, CallSize,
                                        MissileThunkOffset);
    if (!MissileDamagePatched) return false;
    IndexedDoPatchedA = RedirectSite(IndexedDoCallARva, IndexedDoWindowA + IndexedDoCallOffsetA, CallSize,
                                     IndexedThunkOffset);
    if (!IndexedDoPatchedA) return false;
    IndexedDoPatchedB = RedirectSite(IndexedDoCallBRva, IndexedDoWindowB + IndexedDoCallOffsetB, CallSize,
                                     IndexedThunkOffset);
    if (!IndexedDoPatchedB) return false;
    HitEpiloguePatched = RedirectSite(HitEpilogueRva, HitEpilogueWindow + HitEpilogueOffset, HitEpilogueSize,
                                      StubDOffset);
    if (!HitEpiloguePatched) return false;
    HitRecordPatched = RedirectSite(HitRecordCallRva, HitRecordWindow + HitRecordCallOffset, CallSize, StubCOffset);
    return HitRecordPatched;
}

auto InstallHooks() -> bool {
    return Context->InstallInlineHook(SkillStartRva, SkillStartBytes, sizeof(SkillStartBytes), &HookedSkillStart,
                                      &OriginalSkillStart)
        && Context->InstallInlineHook(SkillPerformRva, SkillPerformBytes, sizeof(SkillPerformBytes),
                                      &HookedSkillPerform, &OriginalSkillPerform)
        && Context->InstallInlineHook(StartDispatcherRva, StartDispatcherBytes, sizeof(StartDispatcherBytes),
                                      &HookedStartDispatcher, &OriginalStartDispatcher);
}

auto VerifyAll() -> bool {
    bool ok = true;
    for (const Witness& witness : Witnesses) {
        if (!Context->CheckExpectedBytes(witness.rva, witness.bytes, witness.size)) {
            D2RL::LogErrorF(Context, "DynamicEType: %s at RVA 0x%llX does not match this build.", witness.what,
                static_cast<unsigned long long>(witness.rva));
            ok = false;
        }
    }
    if (!Context->CheckExpectedBytes(IndexedDoWitnessB.rva, IndexedDoWitnessB.bytes, IndexedDoWitnessB.size)) {
        D2RL::LogErrorF(Context, "DynamicEType: %s at RVA 0x%llX does not match this build.", IndexedDoWitnessB.what,
            static_cast<unsigned long long>(IndexedDoWitnessB.rva));
        ok = false;
    }
    return ok;
}

template <typename T>
auto At(std::uint64_t rva) noexcept -> T {
    return reinterpret_cast<T>(Base + rva);
}

void BindNatives() noexcept {
    SkillsRecord    = At<SkillsRecordFn>(SkillsRecordRva);
    EvaluateFormula = At<EvaluateFormulaFn>(EvaluateFormulaRva);
    SkillLevel      = At<SkillLevelFn>(SkillLevelRva);
    DataContext     = At<DataContextFn>(DataContextRva);
    MissileRecord   = At<MissileRecordFn>(MissileRecordRva);
    MissileOwner    = At<MissileOwnerFn>(MissileOwnerRva);
    MissileDamage   = At<MissileDamageFn>(MissileDamageRva);
    IndexedDo       = At<IndexedDoFn>(IndexedDoRva);
}

// -----------------------------------------------------------------------------
// Config
// -----------------------------------------------------------------------------

constexpr const char* ConfigTable = "dynamic_etype";

constexpr const char* DefaultConfigToml = R"TOML(# Dynamic EType (celestialrayone.dynamic-etype)
#
# Lets a skills.txt calc column decide a skill's EType, once per cast.
#
# HOW TO USE IT
#   Put a formula in the chosen calc column of a skills.txt row. When a cast
#   of that skill starts, the formula is evaluated in the caster's context and
#   its result becomes the skill's EType for that whole cast. The result is an
#   elemtypes.txt row number:
#
#       1 fire    2 ltng    3 mag     4 cold    5 pois    6 life
#       7 mana    8 stam    9 stun   10 rand   11 burn   12 frze
#
#   0, a blank column, or anything outside 1 to 12 keeps the row's own EType.
#   Example, cold while a stat is above 2, the row's own element otherwise:
#       (stat('stat name'.accr)>2)?4:0
#
# WHAT FOLLOWS THE ELEMENT
#   Everything the game reads from EType while it runs that cast:
#   - the elemental damage and the matching mastery (Fire Mastery, ...)
#   - physical to elemental conversion in melee skill functions
#   - "+X to <element> Skills" on the cast's level, so a Fireball turned cold
#     gets +Cold Skills and loses +Fire Skills
#   - missiles whose missiles.txt Skill column (or MissileSkill flag) points at
#     the skill, including sub-missiles created later by those missiles
#   - missiles of the cast with damage of their own (missiles.txt EType, EMin,
#     EMax, ELen), such as bolts or lingering fire a missile leaves behind:
#     their missiles.txt EType takes the cast's element when they are created
#     and again every time they hit, so explosions, area damage and
#     pSrvDmgFunc 1 conversion all land in the cast's element
#
# ELEMENT LENGTH (ELen)
#   The length that comes with an element (chill for cold, the duration of
#   poison and burn, stun) is the ELen of the row the damage comes from,
#   whatever element that row ends up as:
#   - the skill and its linked missiles: skills.txt ELen, ELevLen1 to 3 and
#     ELenSymPerCalc
#   - a missile with damage of its own: its missiles.txt ELen and ELevLen1 to 3
#   A row with a blank ELen gives the new element no length. A fire row only
#   chills after turning cold if that row has ELen filled.
#
# WHAT DOES NOT
#   - a missile whose missiles.txt EType is blank has no element of its own
#     and stays without one
#   - tooltips and the skill tree: they keep showing the row's EType
#
# WHEN IT IS DECIDED
#   Once at the start of each cast, kept for every hit of that cast (a whole
#   sequence, a whole channel tick, every missile it fires and every hit of
#   those missiles). Procs decide when they fire. Aura pulses decide on every
#   pulse. Changing the stat mid-cast does not change the cast already running.
#
# MONSTERS
#   The formula runs for every unit that casts the skill: players, monsters,
#   hirelings, summons. Never read stat('tohit') in it: the game ends the
#   process when a formula reads attack rating for anything but a player.
#
# WHICH COLUMN TO PICK
#   calc1 to calc6 are also read by hardcoded skill functions on some skills,
#   so a skill that uses them for its own behaviour will fight over the cell.
#   calc7 to calc10 are not read by anything hardcoded. srcdam-calc-field uses
#   calc10 by default, so this one defaults to calc9.
#
# CONSOLE
#   dynamic-etype            status and counters
#   dynamic-etype log on     writes every decision to the plugin log
#   dynamic-etype log off

[dynamic_etype]

# Master switch. false leaves the game completely untouched.
enabled = true

# Which skills.txt column holds the EType formula. 1 to 10 = calc1 to calc10.
calc_column = 9

# Writes one log line per decision. Useful once, noisy forever.
log_decisions = false
)TOML";

struct TextSpan {
    const char* data = nullptr;
    std::size_t size = 0;
};

constexpr auto IsSpace(char c) noexcept -> bool { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
constexpr auto ToLower(char c) noexcept -> char { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

auto Trim(TextSpan span) noexcept -> TextSpan {
    while (span.size > 0 && IsSpace(span.data[0])) {
        ++span.data;
        --span.size;
    }
    while (span.size > 0 && IsSpace(span.data[span.size - 1])) --span.size;
    return span;
}

auto EqualsIgnoreCase(TextSpan span, const char* literal) noexcept -> bool {
    std::size_t index = 0;
    for (; index < span.size && literal[index] != '\0'; ++index) {
        if (ToLower(span.data[index]) != ToLower(literal[index])) return false;
    }
    return index == span.size && literal[index] == '\0';
}

auto FindTomlValue(const char* toml, const char* table, const char* key, TextSpan* value) noexcept -> bool {
    bool        inTable = (table == nullptr);
    const char* cursor  = toml;
    while (*cursor != '\0') {
        const char* lineEnd = cursor;
        while (*lineEnd != '\0' && *lineEnd != '\n') ++lineEnd;
        TextSpan line{ cursor, static_cast<std::size_t>(lineEnd - cursor) };
        for (std::size_t index = 0; index < line.size; ++index) {
            if (line.data[index] == '#') {
                line.size = index;
                break;
            }
        }
        line = Trim(line);
        if (line.size > 0) {
            if (line.data[0] == '[') {
                const TextSpan name{ line.data + 1, line.size >= 2 ? line.size - 2 : 0 };
                inTable = (table == nullptr) || EqualsIgnoreCase(Trim(name), table);
            } else if (inTable) {
                for (std::size_t index = 0; index < line.size; ++index) {
                    if (line.data[index] != '=') continue;
                    const TextSpan name = Trim(TextSpan{ line.data, index });
                    TextSpan       raw  = Trim(TextSpan{ line.data + index + 1, line.size - index - 1 });
                    if (EqualsIgnoreCase(name, key)) {
                        if (raw.size >= 2 && (raw.data[0] == '"' || raw.data[0] == '\'')
                                && raw.data[raw.size - 1] == raw.data[0]) {
                            raw.data += 1;
                            raw.size -= 2;
                        }
                        *value = raw;
                        return true;
                    }
                    break;
                }
            }
        }
        cursor = (*lineEnd == '\0') ? lineEnd : lineEnd + 1;
    }
    return false;
}

auto ParseBool(TextSpan span, bool fallback) noexcept -> bool {
    if (EqualsIgnoreCase(span, "true") || EqualsIgnoreCase(span, "1") || EqualsIgnoreCase(span, "on")) return true;
    if (EqualsIgnoreCase(span, "false") || EqualsIgnoreCase(span, "0") || EqualsIgnoreCase(span, "off")) return false;
    return fallback;
}

auto ParseInt(TextSpan span, int fallback) noexcept -> int {
    if (span.size == 0) return fallback;
    std::size_t index = 0;
    bool negative = false;
    if (span.data[0] == '+' || span.data[0] == '-') {
        negative = span.data[0] == '-';
        index = 1;
    }
    if (index >= span.size) return fallback;
    long long value = 0;
    for (; index < span.size; ++index) {
        const char digit = span.data[index];
        if (digit < '0' || digit > '9') return fallback;
        value = value * 10 + (digit - '0');
        if (value > 1'000'000) return fallback;
    }
    return static_cast<int>(negative ? -value : value);
}

void ReadConfiguration() noexcept {
    if (!Context->EnsureConfig(DefaultConfigToml)) {
        Context->LogWarn("DynamicEType: the config file could not be created, using built-in defaults.");
    }
    static char buffer[16'384];
    std::uint32_t required = 0;
    if (!Context->ReadConfig(buffer, static_cast<std::uint32_t>(sizeof(buffer) - 1), &required)) {
        Context->LogWarn("DynamicEType: the config file could not be read, using built-in defaults.");
        return;
    }
    buffer[sizeof(buffer) - 1] = '\0';
    TextSpan value{};
    if (FindTomlValue(buffer, ConfigTable, "enabled", &value)) Config.enabled = ParseBool(value, Config.enabled);
    if (FindTomlValue(buffer, ConfigTable, "calc_column", &value)) Config.calcColumn = ParseInt(value, Config.calcColumn);
    if (FindTomlValue(buffer, ConfigTable, "log_decisions", &value)) {
        Config.logDecisions = ParseBool(value, Config.logDecisions);
    }
    if (Config.calcColumn < CalcColumnMin || Config.calcColumn > CalcColumnMax) {
        D2RL::LogWarnF(Context, "DynamicEType: calc_column = %d is out of range, using calc%d.", Config.calcColumn,
            CalcColumnDefault);
        Config.calcColumn = CalcColumnDefault;
    }
    CalcOffset = SkillCalc1Offset + static_cast<std::size_t>(Config.calcColumn - 1) * SkillCalcStride;
}

// -----------------------------------------------------------------------------
// Console
// -----------------------------------------------------------------------------

auto StatusCommand(D2R::Game::Client* client, const D2RL::ConsoleCommandContext* command, void* userData) noexcept
    -> D2RL::ConsoleCommandResult {
    (void)client;
    (void)userData;
    if (command == nullptr || command->plugin == nullptr) return D2RL::ConsoleCommandResult::Failed;
    const D2RL::PluginContext* plugin = command->plugin;

    const TextSpan args = Trim(TextSpan{ command->args != nullptr ? command->args : "",
                                         command->args != nullptr ? command->argsLength : 0U });
    if (args.size > 0) {
        if (EqualsIgnoreCase(args, "log on")) {
            LogDecisions.store(true);
            plugin->WriteConsoleMessage("DynamicEType: decision log on.");
            return D2RL::ConsoleCommandResult::Handled;
        }
        if (EqualsIgnoreCase(args, "log off")) {
            LogDecisions.store(false);
            plugin->WriteConsoleMessage("DynamicEType: decision log off.");
            return D2RL::ConsoleCommandResult::Handled;
        }
        plugin->WriteConsoleMessage("usage: dynamic-etype [log on|log off]");
        return D2RL::ConsoleCommandResult::InvalidArguments;
    }

    char line[256]{};
    std::snprintf(line, sizeof(line), "DynamicEType %s: %s, skills.txt calc%d, decision log %s", PluginVersion,
        Active.load() ? "active" : (Config.enabled ? "NOT active, see the plugin log" : "disabled in config"),
        Config.calcColumn, LogDecisions.load() ? "on" : "off");
    plugin->WriteConsoleMessage(line);
    std::snprintf(line, sizeof(line),
        "hooks %s; sites: do-handler %s/%s, missile damage %s, indexed do %s/%s, missile hit %s/%s; unwind %s",
        HooksInstalled ? "in" : "out", DoRecordPatched ? "open" : "-", DoEpiloguePatched ? "close" : "-",
        MissileDamagePatched ? "on" : "off", IndexedDoPatchedA ? "on" : "off", IndexedDoPatchedB ? "on" : "off",
        HitRecordPatched ? "open" : "-", HitEpiloguePatched ? "close" : "-",
        UnwindRegistered ? "registered" : "missing");
    plugin->WriteConsoleMessage(line);
    std::snprintf(line, sizeof(line),
        "evaluated %llu (changed %llu), reused %llu, scopes %llu (row rewritten %llu), overflow %llu",
        static_cast<unsigned long long>(Stats.evaluated.load()), static_cast<unsigned long long>(Stats.changed.load()),
        static_cast<unsigned long long>(Stats.reused.load()), static_cast<unsigned long long>(Stats.scopes.load()),
        static_cast<unsigned long long>(Stats.writes.load()), static_cast<unsigned long long>(Stats.overflow.load()));
    plugin->WriteConsoleMessage(line);
    std::snprintf(line, sizeof(line),
        "missiles: linked row %llu, own missiles.txt EType at creation %llu, own EType on hit %llu",
        static_cast<unsigned long long>(Stats.missiles.load()), static_cast<unsigned long long>(Stats.ownRow.load()),
        static_cast<unsigned long long>(Stats.hits.load()));
    plugin->WriteConsoleMessage(line);

    AcquireSRWLockShared(&LastLock);
    const LastDecision last = Last;
    ReleaseSRWLockShared(&LastLock);
    if (last.skillId < 0) {
        plugin->WriteConsoleMessage("no decision yet.");
    } else {
        std::snprintf(line, sizeof(line), "last: %s, skill %d, unit type %u, cast %u -> %s (row %s)", last.site,
            last.skillId, last.unitType, last.castId, ElementName(last.element), ElementName(last.rowElement));
        plugin->WriteConsoleMessage(line);
    }
    return D2RL::ConsoleCommandResult::Handled;
}

constexpr D2RL::PluginInfo PluginInfoData{
    .infoSize    = D2RL::PluginInfoSize,
    .abiVersion  = D2RL_PLUGIN_ABI_VERSION,
    .id          = "celestialrayone.dynamic-etype",
    .name        = "Dynamic EType",
    .version     = PluginVersion,
    .author      = "CelestialRayOne",
    .description = "A skills.txt calc column decides a skill's EType once per cast; masteries, conversion, "
                   "+element skills and the cast's missiles (linked or with their own EType) follow it.",
    .flags       = D2RL::PluginFlags::Server | D2RL::PluginFlags::NativeHooks,
};

} // namespace
} // namespace CelestialRayOne::DynamicEType

using namespace CelestialRayOne::DynamicEType;

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
    return &PluginInfoData;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
    Context = context;
    if (Context == nullptr || Context->exeBase == 0) return false;
    Base = Context->exeBase;

    if (!Context->RegisterConsoleCommand("dynamic-etype", &StatusCommand,
            "Dynamic EType status and counters. \"dynamic-etype log on|off\" writes every decision to the log.")) {
        Context->LogWarn("DynamicEType: the console command could not be registered.");
    }

    ReadConfiguration();
    LogDecisions.store(Config.logDecisions);
    if (!Config.enabled) {
        Context->LogInfo("DynamicEType: disabled in config, nothing was installed.");
        return true;
    }
    if (!VerifyAll()) {
        Context->LogError("DynamicEType: this D2R build does not match; nothing was installed.");
        return true;
    }
    BindNatives();
    if (!BuildRegion()) return true;

    // The scopes only open once Active is set, so the hooks and sites can go
    // in one by one without a half-installed state ever binding a row.
    HooksInstalled = InstallHooks();
    if (!HooksInstalled || !InstallPatches()) {
        RemovePatches();
        Context->LogError("DynamicEType: a hook or patch could not be installed; the plugin unloads.");
        return false;
    }
    Active.store(true);
    D2RL::LogInfoF(Context, "DynamicEType %s: active, EType comes from skills.txt calc%d (decision log %s).",
        PluginVersion, Config.calcColumn, Config.logDecisions ? "on" : "off");
    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    Active.store(false);
    if (Context != nullptr) RemovePatches();
}
