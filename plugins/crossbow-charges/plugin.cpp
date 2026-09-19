// Crossbow Charges
//
// Crossbow skills fire from a pool of bolts instead of sharing one cooldown,
// the skill buttons show the bolts left and a shadow that clocks down the next
// bolt, and every hit that comes from a crossbow skill (or from anything that
// hit triggers) deals a stat-driven bonus on its final damage.
//
// Everything below was read out of the D2RLoader 1.3.0 process image and
// D2RCore.dll, and every entry, call site and register used here is checked
// byte for byte at load. Nothing is installed when a check fails.
//
// ---------------------------------------------------------------------------
// 1. Which skills are crossbow skills
// ---------------------------------------------------------------------------
//   A skill is a crossbow skill for a unit when the unit is a player holding a
//   crossbow and the skill's skills.txt globaldelay evaluates above 0:
//     weapon   INVENTORY_GetLeftHandWeapon 387690 (inventory)  the attack weapon
//     type     ITEMS_CheckItemTypeId 373890 (item, itemtype)   equivalence aware
//     record   97790 (dataContext, skillId), globaldelay calc at +564 (234h),
//              localdelay calc at +568 (238h)
//     formula  3B5160 (dataContext, unit, calc, skillId, level)
//     level    33D1E0 (unit, skill, 1, 0), D2RCore ReadWideSkillLevel
//   The itemtypes rows are found by code through the DataTables service every
//   time the tables load.
//
// ---------------------------------------------------------------------------
// 2. The cooldown gate, shared by client and server, 3404A0 (unit, skill)
// ---------------------------------------------------------------------------
//   3404A0  40 53 56 57 41 54 41 57 48 83 EC 30 48 8B DA 48 8B F9
//   Returns 0 when the skill is blocked. Its per-skill part is:
//     3351B0(unit, 185)                            state 185 present
//     2F5940(unit, 185)                            its stat list
//     2F5DF0(ctx, list, 359, layer = skill id)     > 0 means blocked
//   (the layer is the skill record's first word, movzx r9d, word [rax]).
//   After that it blocks on the shared cooldown, state 121. For crossbow skills
//   the hook runs the per-skill part unchanged, skips state 121 and blocks at
//   0 bolts. Every other skill and unit goes to the native gate.
//
// ---------------------------------------------------------------------------
// 3. Where globaldelay is applied
// ---------------------------------------------------------------------------
//   Client 215FC0 (unit, skillId, level), the whole function:
//     record; g = formula(+564); if (g > 0) 217B30(unit, g)
//             l = formula(+568); if (l > 0) 217B90(unit, l, skillId)
//   Server 436C70 (game, unit, skillId, level), same shape, ctx = [game+106h]:
//     439470(game, unit, g) and 439500(game, unit, l, skillId)
//   Server do-handler 43ACB0 has its own inline copy inside its
//   consume-resources block, gated only on the unit being a player: the byte
//   tested at 43B152 (cmp byte [rsp+0C0h],0 / je) is written at 43AD17 by
//   sete right after the unit-type call 34B9D0. There is no sequence check, so
//   the block runs on EVERY do-event, including every arrow of a sequence skill
//   like Strafe:
//     43B163  45 8B CD             mov  r9d, r13d          ; skill id
//     43B17B  E8 -> 3B5160         globaldelay
//     43B180  85 C0 7E 0E          test eax,eax / jle
//     43B184  44 8B C0 48 8B D3 48 8B CF
//     43B18D  E8 DE E2 FF FF       call 439470             ; this call
//     43B192  44 8B 86 38 02 00 00 mov  r8d, [rsi+238h]    ; localdelay next
//   For a crossbow cast the global part spends a bolt instead. 215FC0 and
//   436C70 are hooked and rebuilt with only that difference; the call at
//   43B18D is sent through a relay that adds r13d as a fourth argument.
//   Casts triggered by items reach 43ACB0 with consume-resources 0 (the proc
//   caster 589930 passes 0 at 589C93), so they never spend a bolt.
//
//   One cast, one bolt: a cast keeps the cast id it got at skill start
//   (34F430, unit+12Ch) for all of its do-events, so the server spends at most
//   one bolt per cast id. A joined client, whose client apply 215FC0 also runs
//   per do-event, predicts at most one spend per cast id of its own unit (the
//   client skill start 217650 gives each cast an id through 8D5D0).
//
// ---------------------------------------------------------------------------
// 4. The reload clock
// ---------------------------------------------------------------------------
//   The player stat-regeneration event 42E600 (game, unit, a3, a4) re-arms
//   itself every frame through EVENT_SetEvent 48B720 at [game+170h]+1, for
//   every player, dead or alive. The hook advances that player's pool first.
//   The current bolts are written to the current-bolts stat through 2F7D10
//   (D2RCore SetWideUnitStat); D2RCore's widened stat packets carry it to the
//   owning client, and a joined client reads it back through 2F5020. On a
//   host the client side reads the server pool directly.
//
// ---------------------------------------------------------------------------
// 5. Which hits get the damage bonus: cast ids
// ---------------------------------------------------------------------------
//   Every cast has an id, unit+12Ch (34B6C0 reads it). D2RCore's
//   GenerateUnitCastId is a per-game counter at game+16Ch; SetUnitCastId
//   writes unit+12Ch. The game uses it to track where things come from:
//     skill start   34F430 (unit, skill, castId) sets the used skill and the id;
//                   the player mode starts 42D2C0 / 42DE40 pass a fresh id
//     missiles      5379F5: a new missile takes the id it was given, else its
//                   creator's current id, so explosions take their parent's
//     missile hit   462E40 stamps the owner with the missile's id around the
//                   damage and on-hit effects, then restores it
//     procs         589930 generates a fresh id (call at 589C37) while the
//                   owner still carries the id of the hit that triggered it
//     auras         437230 stamps the aura's own id around every pulse
//   So the plugin tags cast ids:
//     - at skill start (34F430 hook), when the skill is a crossbow skill
//     - in item event function 20 (chance to cast on attack, on striking and
//       on kill; table 238E5C0, slot 20 at 238E660 -> 583B30), when the owner
//       carries a tagged id: the fresh id generated at 589C37 is tagged too
//   Event function 21 (chance to cast when struck, 5837F0) is left alone, so
//   those never count, whatever the player cast last.
//   The bonus applies when the attacker is a player whose current cast id is
//   tagged at the moment the damage is resolved.
//
// ---------------------------------------------------------------------------
// 6. Where the bonus is applied
// ---------------------------------------------------------------------------
//   SUNITDMG_ExecuteEvents 44CE80 calls the damage calculation 44DF10 for
//   missile-style damage:
//     44CF87  4C 8B CF 4C 8B C6 49 8B D6 49 8B CF   r9 dmg, r8 def, rdx att, rcx game
//     44CF93  E8 78 0F 00 00                          call 44DF10
//   The call is sent through a relay; the hook runs the calculation, then
//   scales the finished numbers. Inside the calculation the bleed plugin reads
//   the physical damage at 44ECA5 and the poison immunity plugin builds the
//   life total at 44EC71, so bleed sees the unboosted hit and the poison rule
//   is kept (the total is scaled as a whole).
//   Scaled D2Damage fields: +18h physical, +20h fire, +24h burn, +2Ch
//   lightning, +30h magic, +34h cold, +38h poison, +134h life total, and the
//   damage of each per-source poison entry (pointer +40h, count +48h, 12-byte
//   entries, damage at +4). Crushing blow and open wounds are item events that
//   read life directly, so they are untouched.
//
// ---------------------------------------------------------------------------
// 7. The skill buttons
// ---------------------------------------------------------------------------
//   SkillSelectButtonWidget update 2399F0 (widget) runs every frame and calls
//   the refresh 239380, which leaves:
//     +2952 side (1 left, 2 right, controller buttons are right)
//     +3080 skill id (-1 without a skill), +3084 skill owner
//     +3056 QuantityText, +1820 current tint, +2956 normal tint,
//     +3004 cooldown tint (used for classifier code 8)
//   The number is written the way the refresh writes it:
//     visible through vtable +50h, text through 0CE510(text + 136, "%d", n),
//     or text + 512 when bytes +502 and +503 of the text box are both set.
//   The shadow child is found with 856220 (parent, name) and accepted only if
//   its type descriptor chain (vtable +58h, name at +8h, parent at +10h, as
//   built by the descriptor constructor 851030) contains "ImageWidget".
//   An image widget draws the frame at +90h (layout "frame"), with its
//   opacity in the float at +94h (layout "transparency", 1.0 = solid) and its
//   colour in the RGBA float tint at +9Ch (layout "tint"), per the
//   AbstractImageWidget property table at 2470090; 858C10 / 858C50 pass all
//   three to the draw. The tint's alpha does not make an image see-through,
//   the transparency field does.
//
//   The bolts and the shadow on a host (single player or the TCP/IP host) come
//   straight from the server pool in this process, so the shadow runs on the
//   real reload time of the bolt coming back next. A joined client rebuilds
//   the reload queue from the current-bolts stat and the globaldelay of its own
//   casts.
//   The skill node comes from 33DCD0 (unit, skillId, owner), the local player
//   from 9A480(8B2D0()).
//
// ---------------------------------------------------------------------------
// 8. Crossbow attack speed
// ---------------------------------------------------------------------------
//   UNITS_UpdateAnimRateAndVelocity 350B40 (unit) sets a unit's animation
//   rate whenever its mode or stats change. On the attack path, with rsi = the
//   unit, r12d = the animation speed ([[unit+70h]+0Ch]) and r15d = the rate
//   percent (attackrate + diminished IAS + passive bonus 213, -30 in mode 18,
//   clamped 15..175, 250 shapeshifted):
//     351482  41 8B CF B8 1F 85 EB 51 41 0F AF CC F7 E1 8B FA C1 EF 05
//             edi = rate% * AnimSpeed / 100
//     351597  BB FF 7F 00 00       mov ebx, 7FFFh        ; every path lands here
//     35159C  3B FB 0F 86 ...      cmp edi, ebx / jbe -> store at [unit+54h]
//   The wereform rescale in between (35149E..351596) also leaves its result
//   in edi and never writes rsi. The five-byte mov at 351597 becomes a call
//   to a relay that hands (unit, edi) to HookedAttackRate, takes the new rate
//   back into edi, redoes the mov and returns.
//   A frame of animation costs 256, so an attack of FramesPerDirection frames
//   ([[unit+70h]+8]) takes ceil(256 * FPD / rate) - 1 frames, the formula that
//   reproduces the published IAS tables. For the crossbow attack animation
//   (A1 = mode 7, mode at [unit+0Ch]) the rate becomes
//   ceil(256 * FPD / (N + 1)), which gives exactly N frames, with
//   N = default_attack_frames + the attack frames stat. The same function
//   runs on the server (timing) and on every client (animation), so both agree.
//   Everything else keeps the game's own speed: sequence skills (mode 18,
//   Strafe), A2 (only the Assassin has an A2 crossbow animation) and the
//   special animations S1..S4, which skills such as Assassin traps use.
//   whirlwind-follow-cursor byte-checks 350B40's entry and faster-cast-rate-cap
//   patches the cast branch at 350C29; neither touches 351482..3515A3.
//
// ---------------------------------------------------------------------------
// Nothing calls into the game while the plugin loads
// ---------------------------------------------------------------------------
//   The UI type descriptors are built lazily, once, the first time their
//   getter runs. The ImageWidget getter 8599F0 looks its base type up by name
//   at that moment (853090("AbstractImageWidget")) and keeps whatever it got;
//   if the base is not registered yet it only asserts "Check the registration
//   order?" and builds ImageWidget with no parent. 1.0.0 called that getter at
//   plugin load, before the game had registered its widget types, so every
//   ImageWidget lost the Widget and AbstractImageWidget fields (rect, tint,
//   visibility and the rest) and the whole UI broke. Load now only checks
//   bytes and uses loader services; the type check reads descriptor names.
//
// ---------------------------------------------------------------------------
// Other plugins
// ---------------------------------------------------------------------------
//   No site here is touched by any plugin or static patch in the ESR set:
//   srcdam-calc-field hooks 43ACB0's entry, cast-on-cast patches 43AF06 to
//   43AF25, 43B0EC and 589C6F, soft-hits patches event slots 4 and 6, bleed
//   redirects 44ECA5, poison immunity patches inside 44EC71 to 44ECA4.
//   whirlwind-follow-cursor calls 34F430 directly and reaches this hook.

#include <D2RLPlugin/api.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

constexpr char PluginIdText[] = "celestialrayone.crossbow-charges";
constexpr char SourceFile[]   = "crossbow-charges";

const D2RL::PluginContext* Context{};
std::uintptr_t             Base{};

// ---------------------------------------------------------------------------
// Addresses (RVAs into the D2RLoader 1.3.0 image)
// ---------------------------------------------------------------------------

// Hooked
constexpr std::uint64_t CooldownGateRva       = 0x3404A0;
constexpr std::uint64_t ClientApplyRva        = 0x215FC0;
constexpr std::uint64_t ServerApplyRva        = 0x436C70;
constexpr std::uint64_t PlayerRegenRva        = 0x42E600;
constexpr std::uint64_t SetUsedSkillRva       = 0x34F430;
constexpr std::uint64_t ButtonUpdateRva       = 0x2399F0;
// Redirected calls and table slot
constexpr std::uint64_t DoHandlerWindowRva    = 0x43B163;
constexpr std::uint64_t DoHandlerCallRva      = 0x43B18D;
constexpr std::uint64_t ProcCastIdWindowRva   = 0x589C34;
constexpr std::uint64_t ProcCastIdCallRva     = 0x589C37;
constexpr std::uint64_t DamageCalcWindowRva   = 0x44CF87;
constexpr std::uint64_t DamageCalcCallRva     = 0x44CF93;
constexpr std::uint64_t EventFuncSlot20Rva    = 0x238E660;
constexpr std::uint64_t AttackRateWindowRva   = 0x351482;
constexpr std::uint64_t AttackRateSiteRva     = 0x351597;
// Called
constexpr std::uint64_t ServerGlobalDelayRva  = 0x439470;
constexpr std::uint64_t ServerLocalDelayRva   = 0x439500;
constexpr std::uint64_t ClientGlobalDelayRva  = 0x217B30;
constexpr std::uint64_t ClientLocalDelayRva   = 0x217B90;
constexpr std::uint64_t CalculateDamageRva    = 0x44DF10;
constexpr std::uint64_t EventFunc20Rva        = 0x583B30;
constexpr std::uint64_t GenerateCastIdRva     = 0x3E2B578;  // loader thunk
constexpr std::uint64_t SkillsRecordRva       = 0x097790;
constexpr std::uint64_t EvaluateFormulaRva    = 0x3B5160;
constexpr std::uint64_t DataContextRva        = 0x34A0E0;
constexpr std::uint64_t UnitTypeRva           = 0x34B9D0;
constexpr std::uint64_t UnitIdRva             = 0x34A330;
constexpr std::uint64_t IsServerUnitRva       = 0x34F8D0;
constexpr std::uint64_t InventoryRva          = 0x34A360;
constexpr std::uint64_t LeftHandWeaponRva     = 0x387690;
constexpr std::uint64_t CheckItemTypeRva      = 0x373890;
constexpr std::uint64_t GetUnitStatRva        = 0x2F5020;  // loader thunk
constexpr std::uint64_t SetUnitStatRva        = 0x2F7D10;  // loader thunk
constexpr std::uint64_t CheckStateRva         = 0x3351B0;
constexpr std::uint64_t StateStatListRva      = 0x2F5940;
constexpr std::uint64_t ReadListStatRva       = 0x2F5DF0;  // loader thunk
constexpr std::uint64_t SkillLevelRva         = 0x33D1E0;  // loader thunk
constexpr std::uint64_t SkillNodeRva          = 0x33DCD0;
constexpr std::uint64_t LocalPlayerIndexRva   = 0x08B2D0;
constexpr std::uint64_t PlayerFromIndexRva    = 0x09A480;
constexpr std::uint64_t FindChildWidgetRva    = 0x856220;
constexpr std::uint64_t StringFormatRva       = 0x0CE510;

// Game and unit layout
constexpr std::size_t GameFrameOffset       = 0x170;
constexpr std::size_t GameDataContextOffset = 0x106;
constexpr std::size_t UnitCastIdOffset      = 0x12C;
constexpr std::size_t UnitModeOffset        = 0x0C;
constexpr std::size_t UnitAnimRecordOffset  = 0x70;
constexpr std::size_t AnimRecordFramesOffset = 0x08;
constexpr std::size_t SkillGlobalDelayCalc  = 564;
constexpr std::size_t SkillLocalDelayCalc   = 568;
constexpr std::int32_t LocalCooldownState   = 185;
constexpr std::int32_t LocalCooldownStat    = 359;

// SkillSelectButtonWidget and child widgets
constexpr std::size_t ButtonSideOffset         = 2952;
constexpr std::size_t ButtonSkillIdOffset      = 3080;
constexpr std::size_t ButtonSkillOwnerOffset   = 3084;
constexpr std::size_t ButtonQuantityOffset     = 3056;
constexpr std::size_t ButtonTintOffset         = 1820;
constexpr std::size_t ButtonNormalTintOffset   = 2956;
constexpr std::size_t ButtonCooldownTintOffset = 3004;
constexpr std::size_t TextAltFlagA             = 502;
constexpr std::size_t TextAltFlagB             = 503;
constexpr std::size_t TextMainString           = 136;
constexpr std::size_t TextAltString            = 512;
constexpr std::size_t ImageFrameOffset         = 0x90;
constexpr std::size_t ImageTransparencyOffset  = 0x94;
constexpr std::size_t ImageTintOffset          = 0x9C;
constexpr std::size_t WidgetSetVisibleSlot     = 0x50 / 8;
constexpr std::size_t WidgetTypeDescSlot       = 0x58 / 8;
constexpr std::size_t TypeDescParentOffset     = 0x10;

// D2Damage
constexpr std::array<std::size_t, 8> DamageFields{ 0x18, 0x20, 0x24, 0x2C, 0x30, 0x34, 0x38, 0x134 };
constexpr std::size_t DamagePoisonEntries = 0x40;
constexpr std::size_t DamagePoisonCount   = 0x48;
constexpr std::size_t PoisonEntrySize     = 12;
constexpr std::size_t PoisonEntryDamage   = 4;

constexpr std::uint64_t MsPerFrame          = 40;
constexpr std::uint64_t PredictionTimeoutMs = 1500;
constexpr std::size_t   CallSize            = 5;

// ---------------------------------------------------------------------------
// Byte witnesses
// ---------------------------------------------------------------------------

constexpr std::uint8_t CooldownGateBytes[]{
    0x40,0x53,0x56,0x57,0x41,0x54,0x41,0x57,0x48,0x83,0xEC,0x30,0x48,0x8B,0xDA,0x48,0x8B,0xF9 };
constexpr std::uint8_t ClientApplyBytes[]{
    0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x48,
    0x89,0x7C,0x24,0x20,0x41,0x56,0x48,0x83,0xEC,0x30 };
constexpr std::uint8_t ServerApplyBytes[]{
    0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x48,
    0x89,0x7C,0x24,0x20,0x41,0x56,0x48,0x83,0xEC,0x30 };
constexpr std::uint8_t PlayerRegenBytes[]{
    0x48,0x89,0x5C,0x24,0x20,0x55,0x48,0x83,0xEC,0x40,0x41,0x8B,0xC1,0xC7,0x44,0x24,
    0x30,0x00,0x00,0x00,0x00 };
constexpr std::uint8_t SetUsedSkillBytes[]{
    0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xEC,0x20,0x41,
    0x8B,0xF8,0x48,0x8B,0xF2,0x48,0x8B,0xD9 };
constexpr std::uint8_t ButtonUpdateBytes[]{
    0x40,0x53,0x57,0x41,0x56,0x48,0x83,0xEC,0x30,0x48,0x8B,0xF9 };

// 43B163..43B19B: skill id in r13d, globaldelay evaluated, then the call.
constexpr std::uint8_t DoHandlerWindow[]{
    0x45,0x8B,0xCD,0x44,0x8B,0x86,0x34,0x02,0x00,0x00,0x48,0x8B,0xD3,0x0F,0xB6,0x8F,
    0x06,0x01,0x00,0x00,0x89,0x6C,0x24,0x20,0xE8,0xE0,0x9F,0xF7,0xFF,0x85,0xC0,0x7E,
    0x0E,0x44,0x8B,0xC0,0x48,0x8B,0xD3,0x48,0x8B,0xCF,0xE8,0xDE,0xE2,0xFF,0xFF,0x44,
    0x8B,0x86,0x38,0x02,0x00,0x00,0x45,0x8B,0xCD };
constexpr std::size_t DoHandlerCallOffset = DoHandlerCallRva - DoHandlerWindowRva;

// 589C34..589C45: generate the proc's cast id, then set it on the caster.
constexpr std::uint8_t ProcCastIdWindow[]{
    0x49,0x8B,0xCF,0xE8,0x3C,0x19,0x8A,0x03,0x8B,0xD0,0x48,0x8B,0xCB,0xE8,0x38,0x19,
    0x8A,0x03 };
constexpr std::size_t ProcCastIdCallOffset = ProcCastIdCallRva - ProcCastIdWindowRva;

// 44CF87..44CF9B: the missile-style damage calculation call.
constexpr std::uint8_t DamageCalcWindow[]{
    0x4C,0x8B,0xCF,0x4C,0x8B,0xC6,0x49,0x8B,0xD6,0x49,0x8B,0xCF,0xE8,0x78,0x0F,0x00,
    0x00,0x0F,0xB7,0x47,0x04 };
constexpr std::size_t DamageCalcCallOffset = DamageCalcCallRva - DamageCalcWindowRva;

// 351482..35149D: edi = rate% (r15d) * animation speed (r12d) / 100, rsi = unit.
constexpr std::uint8_t AttackRateWindow[]{
    0x41,0x8B,0xCF,0xB8,0x1F,0x85,0xEB,0x51,0x41,0x0F,0xAF,0xCC,0xF7,0xE1,0x8B,0xFA,
    0xC1,0xEF,0x05,0x44,0x39,0x36,0x0F,0x85,0xF9,0x00,0x00,0x00 };
// 351597..3515A3: mov ebx,7FFFh / cmp edi,ebx / jbe store. The first five bytes are replaced.
constexpr std::uint8_t AttackRateSite[]{
    0xBB,0xFF,0x7F,0x00,0x00,0x3B,0xFB,0x0F,0x86,0xB8,0xF6,0xFF,0xFF };

struct Witness {
    std::uint64_t       rva;
    const std::uint8_t* bytes;
    std::uint32_t       size;
    const char*         name;
};

constexpr std::uint8_t ServerGlobalDelayBytes[]{
    0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xEC,0x40,0x48,
    0x8B,0xFA,0x41,0x8B,0xD8,0x48,0x8B,0xF1 };
constexpr std::uint8_t ServerLocalDelayBytes[]{
    0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57 };
constexpr std::uint8_t ClientGlobalDelayBytes[]{
    0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xEC,0x20,0x8B,0xFA };
constexpr std::uint8_t ClientLocalDelayBytes[]{
    0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57 };
constexpr std::uint8_t CalculateDamageBytes[]{
    0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0xAC,
    0x24,0x68,0xFD,0xFF,0xFF,0x48,0x81,0xEC };
constexpr std::uint8_t EventFunc20Bytes[]{
    0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x48,
    0x89,0x7C,0x24,0x20,0x41,0x54,0x41,0x56 };
constexpr std::uint8_t LoaderThunkBytes[]{ 0xFF,0x25 };
constexpr std::uint8_t SkillsRecordBytes[]{
    0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x20,0x57,0x48,0x83,0xEC,0x30,0x48,
    0x63,0xF2 };
constexpr std::uint8_t EvaluateFormulaBytes[]{
    0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x20,0x57,
    0x41,0x56,0x41,0x57,0x48,0x81,0xEC,0xB0,0x00,0x00,0x00 };
constexpr std::uint8_t DataContextBytes[]{
    0x48,0x83,0xEC,0x28,0x48,0x85,0xC9,0x75,0x1A,0x88,0x4C,0x24,0x30,0x48,0x8D,0x4C,
    0x24,0x30 };
constexpr std::uint8_t UnitTypeBytes[]{
    0x48,0x83,0xEC,0x28,0x48,0x85,0xC9,0x75,0x1D,0x88,0x4C,0x24,0x30,0x48,0x8D,0x4C };
constexpr std::uint8_t UnitIdBytes[]{
    0x48,0x83,0xEC,0x28,0x48,0x85,0xC9,0x75,0x1D,0x88,0x4C,0x24,0x30,0x48,0x8D,0x4C,
    0x24,0x30 };
// 34F8F3: mov eax,[rcx+124h] / shr eax,15h / and eax,1
constexpr std::uint64_t IsServerUnitFlagRva = 0x34F8F3;
constexpr std::uint8_t IsServerUnitFlagBytes[]{
    0x8B,0x81,0x24,0x01,0x00,0x00,0xC1,0xE8,0x15,0x83,0xE0,0x01 };
constexpr std::uint8_t InventoryBytes[]{
    0x48,0x89,0x5C,0x24,0x18,0x56,0x48,0x83,0xEC,0x20,0x48,0x8B,0xF1,0x48,0x85,0xC9 };
constexpr std::uint8_t LeftHandWeaponBytes[]{
    0x48,0x89,0x5C,0x24,0x10,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0x48,0x85,0xC9,
    0x75,0x24 };
constexpr std::uint8_t CheckItemTypeBytes[]{
    0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x6C,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x57,
    0x41,0x56,0x41,0x57,0x48,0x83,0xEC,0x20 };
constexpr std::uint8_t CheckStateBytes[]{
    0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xEC,0x20,0x8B,
    0xDA,0x48,0x8B,0xF1 };
constexpr std::uint8_t StateStatListBytes[]{
    0x48,0x89,0x5C,0x24,0x10,0x57,0x48,0x83,0xEC,0x20,0x8B,0xDA,0x48,0x8B,0xF9,0x48,
    0x85,0xC9,0x75,0x13 };
constexpr std::uint8_t SkillNodeBytes[]{
    0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x41,0x8B,0xD8,0x8B,0xFA };
constexpr std::uint8_t LocalPlayerIndexBytes[]{ 0x8B,0x05,0x2E,0x84,0x99,0x02,0xC3 };
constexpr std::uint8_t PlayerFromIndexBytes[]{
    0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x83,0xF9,0x08 };
constexpr std::uint8_t FindChildWidgetBytes[]{
    0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xEC,0x20,0x48,
    0x8B,0x59,0x58 };
constexpr std::uint8_t StringFormatBytes[]{
    0x48,0x8B,0xC4,0x48,0x89,0x50,0x10,0x4C,0x89,0x40,0x18,0x4C,0x89,0x48,0x20,0x53,
    0x55,0x56,0x41,0x56,0x48,0x83,0xEC,0x48 };

template <std::size_t N>
constexpr auto W(std::uint64_t rva, const std::uint8_t (&bytes)[N], const char* name) noexcept -> Witness {
    return { rva, bytes, static_cast<std::uint32_t>(N), name };
}

const std::array<Witness, 33> Witnesses{
    W(CooldownGateRva, CooldownGateBytes, "cooldown gate"),
    W(ClientApplyRva, ClientApplyBytes, "client cooldown step"),
    W(ServerApplyRva, ServerApplyBytes, "server cooldown step"),
    W(PlayerRegenRva, PlayerRegenBytes, "player per-frame event"),
    W(SetUsedSkillRva, SetUsedSkillBytes, "skill start"),
    W(DoHandlerWindowRva, DoHandlerWindow, "skill do-handler shared cooldown call"),
    W(ProcCastIdWindowRva, ProcCastIdWindow, "proc caster cast id"),
    W(DamageCalcWindowRva, DamageCalcWindow, "damage calculation call"),
    W(ServerGlobalDelayRva, ServerGlobalDelayBytes, "server shared cooldown"),
    W(ServerLocalDelayRva, ServerLocalDelayBytes, "server skill cooldown"),
    W(ClientGlobalDelayRva, ClientGlobalDelayBytes, "client shared cooldown"),
    W(ClientLocalDelayRva, ClientLocalDelayBytes, "client skill cooldown"),
    W(CalculateDamageRva, CalculateDamageBytes, "damage calculation"),
    W(EventFunc20Rva, EventFunc20Bytes, "item event function 20"),
    W(GenerateCastIdRva, LoaderThunkBytes, "cast id generator"),
    W(SkillsRecordRva, SkillsRecordBytes, "skills.txt record"),
    W(EvaluateFormulaRva, EvaluateFormulaBytes, "skill formula"),
    W(DataContextRva, DataContextBytes, "data context"),
    W(UnitTypeRva, UnitTypeBytes, "unit type"),
    W(UnitIdRva, UnitIdBytes, "unit id"),
    W(IsServerUnitFlagRva, IsServerUnitFlagBytes, "server unit flag"),
    W(InventoryRva, InventoryBytes, "inventory"),
    W(LeftHandWeaponRva, LeftHandWeaponBytes, "attack weapon"),
    W(CheckItemTypeRva, CheckItemTypeBytes, "item type check"),
    W(GetUnitStatRva, LoaderThunkBytes, "stat read"),
    W(SetUnitStatRva, LoaderThunkBytes, "stat write"),
    W(CheckStateRva, CheckStateBytes, "state check"),
    W(StateStatListRva, StateStatListBytes, "state stat list"),
    W(ReadListStatRva, LoaderThunkBytes, "list stat read"),
    W(SkillLevelRva, LoaderThunkBytes, "skill level"),
    W(SkillNodeRva, SkillNodeBytes, "skill node"),
    W(LocalPlayerIndexRva, LocalPlayerIndexBytes, "local player index"),
    W(PlayerFromIndexRva, PlayerFromIndexBytes, "player from index"),
};

const std::array<Witness, 3> UiWitnesses{
    W(ButtonUpdateRva, ButtonUpdateBytes, "skill button update"),
    W(FindChildWidgetRva, FindChildWidgetBytes, "child widget lookup"),
    W(StringFormatRva, StringFormatBytes, "string format"),
};

// ---------------------------------------------------------------------------
// Native functions
// ---------------------------------------------------------------------------

using GateFn           = std::int32_t(__fastcall*)(void* unit, void* skill);
using ClientApplyFn    = void(__fastcall*)(void* unit, std::int32_t skillId, std::int32_t level);
using ServerApplyFn    = void(__fastcall*)(void* game, void* unit, std::int32_t skillId, std::int32_t level);
using PlayerRegenFn    = std::uint64_t(__fastcall*)(void* game, void* unit, std::int32_t a3, std::int32_t a4);
using SetUsedSkillFn   = std::uint64_t(__fastcall*)(void* unit, void* skill, std::uint32_t castId);
using ButtonUpdateFn   = std::uint64_t(__fastcall*)(void* widget);
using ServerGlobalFn   = std::uint64_t(__fastcall*)(void* game, void* unit, std::int32_t frames);
using ServerLocalFn    = std::uint64_t(__fastcall*)(void* game, void* unit, std::int32_t frames, std::int32_t skillId);
using ClientGlobalFn   = std::uint64_t(__fastcall*)(void* unit, std::int32_t frames);
using ClientLocalFn    = std::uint64_t(__fastcall*)(void* unit, std::int32_t frames, std::int32_t skillId);
using CalculateFn      = void(__fastcall*)(void* game, void* attacker, void* defender, void* damage);
using EventFunc20Fn    = std::uint64_t(__fastcall*)(void* game, std::uint64_t event, void* owner, void* target,
                             void* damage, std::uint64_t packedStat, std::uint64_t a7, std::uint64_t a8, std::uint64_t a9);
using GenerateCastIdFn = std::uint32_t(__fastcall*)(void* game);
using SkillsRecordFn   = void*(__fastcall*)(std::uint8_t context, std::int32_t skillId);
using EvaluateFn       = std::int32_t(__fastcall*)(std::uint8_t context, void* unit, std::uint32_t calc,
                             std::int32_t skillId, std::int32_t level);
using DataContextFn    = std::uint8_t(__fastcall*)(void* unit);
using UnitTypeFn       = std::uint32_t(__fastcall*)(void* unit);
using UnitIdFn         = std::uint32_t(__fastcall*)(void* unit, const char* file, std::int32_t line);
using IsServerUnitFn   = std::uint32_t(__fastcall*)(void* unit);
using InventoryFn      = void*(__fastcall*)(void* unit, const char* file, std::int32_t line);
using WeaponFn         = void*(__fastcall*)(void* inventory);
using CheckItemTypeFn  = std::int32_t(__fastcall*)(void* item, std::int32_t itemType);
using GetStatFn        = std::int32_t(__fastcall*)(void* unit, std::int32_t stat, std::int32_t layer);
using SetStatFn        = std::uint8_t(__fastcall*)(void* unit, std::int32_t stat, std::int32_t value, std::int32_t layer);
using CheckStateFn     = std::int32_t(__fastcall*)(void* unit, std::int32_t state);
using StateStatListFn  = void*(__fastcall*)(void* unit, std::int32_t state);
using ReadListStatFn   = std::int32_t(__fastcall*)(std::uint8_t context, void* list, std::int32_t stat, std::int32_t layer);
using SkillLevelFn     = std::int32_t(__fastcall*)(void* unit, void* skill, std::uint64_t bonus, std::uint64_t unused);
using SkillNodeFn      = void*(__fastcall*)(void* unit, std::int32_t skillId, std::uint32_t owner);
using PlayerIndexFn    = std::int32_t(__fastcall*)();
using PlayerFromFn     = void*(__fastcall*)(std::int32_t index);
using FindChildFn      = void*(__fastcall*)(void* parent, const char* name);
using StringFormatFn   = void*(*)(void* target, const char* format, ...);
using SetVisibleFn     = void(__fastcall*)(void* widget, std::uint64_t visible);
using TypeDescFn       = void*(__fastcall*)(void* widget);

GateFn          OriginalGate{};
ClientApplyFn   OriginalClientApply{};
ServerApplyFn   OriginalServerApply{};
PlayerRegenFn   OriginalPlayerRegen{};
SetUsedSkillFn  OriginalSetUsedSkill{};
ButtonUpdateFn  OriginalButtonUpdate{};

ServerGlobalFn   ServerGlobalDelay{};
ServerLocalFn    ServerLocalDelay{};
ClientGlobalFn   ClientGlobalDelay{};
ClientLocalFn    ClientLocalDelay{};
CalculateFn      CalculateDamage{};
EventFunc20Fn    NativeEventFunc20{};
GenerateCastIdFn GenerateCastId{};
SkillsRecordFn   SkillsRecord{};
EvaluateFn       Evaluate{};
DataContextFn    DataContext{};
UnitTypeFn       UnitType{};
UnitIdFn         UnitId{};
IsServerUnitFn   IsServerUnit{};
InventoryFn      Inventory{};
WeaponFn         LeftHandWeapon{};
CheckItemTypeFn  CheckItemType{};
GetStatFn        GetStat{};
SetStatFn        SetStat{};
CheckStateFn     CheckState{};
StateStatListFn  StateStatList{};
ReadListStatFn   ReadListStat{};
SkillLevelFn     SkillLevel{};
SkillNodeFn      SkillNode{};
PlayerIndexFn    LocalPlayerIndex{};
PlayerFromFn     PlayerFromIndex{};
FindChildFn      FindChild{};
StringFormatFn   StringFormat{};

template <typename T>
auto At(std::uint64_t rva) noexcept -> T {
    return reinterpret_cast<T>(Base + rva);
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

enum class PluginState { NotLoaded, DisabledByConfig, UnsupportedBuild, InstallFailed, Active };
PluginState State{ PluginState::NotLoaded };
std::atomic<bool> Active{};
bool UiActive{};

std::atomic<std::uint64_t> BoltsSpent{};
std::atomic<std::uint64_t> BoltsRefused{};
std::atomic<std::uint64_t> RepeatStepsIgnored{};
std::atomic<std::uint64_t> BoltsReloaded{};
std::atomic<std::uint64_t> CastsTagged{};
std::atomic<std::uint64_t> ProcsTagged{};
std::atomic<std::uint64_t> HitsBoosted{};

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

constexpr char DefaultConfigToml[] =
    "# Crossbow Charges\n"
    "#\n"
    "# Crossbow skills fire from a pool of bolts instead of sharing one cooldown.\n"
    "#\n"
    "# What counts as a crossbow skill\n"
    "#   A skill whose skills.txt `globaldelay` works out above 0 while the player\n"
    "#   holds a crossbow, meaning a weapon of one of the itemtypes.txt types listed\n"
    "#   in crossbow_item_types. Every other skill keeps the normal cooldowns.\n"
    "#\n"
    "# How the bolts work\n"
    "#   - The pool starts full when you enter a game and is not saved.\n"
    "#   - Casting a crossbow skill spends one bolt, however many projectiles it\n"
    "#     fires. A skill's own `localdelay` still applies on top.\n"
    "#   - Each spent bolt reloads for the `globaldelay` frames of the skill that\n"
    "#     spent it. Bolts come back one at a time, in the order they were spent.\n"
    "#   - With no bolts left, crossbow skills cannot be cast.\n"
    "#   - Maximum bolts is default_max_bolts plus the max bolts stat, at least 1.\n"
    "#   - The pool keeps reloading whatever weapon is held. Maximum bolts is only\n"
    "#     read while a crossbow is equipped; otherwise the last crossbow value is\n"
    "#     kept. A lower maximum drops the extra bolts. A higher maximum refills one\n"
    "#     bolt at a time with the last reload time, so swapping weapons can never\n"
    "#     refill anything.\n"
    "#   - Casts triggered by items (chance to cast) never spend bolts.\n"
    "#\n"
    "# Damage bonus\n"
    "#   Hits from crossbow skills, and from everything those hits trigger (chance\n"
    "#   to cast on attack, on striking and on kill, and what those trigger in\n"
    "#   turn), deal (100 + damage_base + damage stat) percent of their final\n"
    "#   damage, after resistances and absorbs. Physical, fire, burn, lightning,\n"
    "#   cold, magic and poison scale. Crushing blow, open wounds and bleed do not.\n"
    "#   Chance to cast when struck is never counted as a crossbow hit.\n"
    "#\n"
    "# Attack speed\n"
    "#   Crossbow attacks (the A1 attack animation) always take exactly\n"
    "#   default_attack_frames plus the attack frames stat, whatever IAS or skill\n"
    "#   attack speed the player has. Negative stat values make attacks faster. A\n"
    "#   total of 0 or less leaves the game's normal speed. Sequence skills such as\n"
    "#   Strafe and the special animations (S1 to S4, used by skills such as\n"
    "#   Assassin traps) keep the game's normal speed.\n"
    "#\n"
    "# Skill buttons\n"
    "#   Crossbow skills show the bolts left in the button's number box, 0 included.\n"
    "#   While any bolt is reloading, the shadow image on the button clocks down the\n"
    "#   bolt that comes back next, over that bolt's own reload time, then restarts\n"
    "#   for the one after it. It goes away when the pool is full. At 0 bolts the button keeps its normal colour; the\n"
    "#   shadow is the only sign.\n"
    "#   The shadow is an ImageWidget child of the skill button, named as below, in\n"
    "#   hudpanelhd.json and controller/hudpanelhd.json. A button without that\n"
    "#   child still shows the number.\n"
    "#\n"
    "# Console command: crossbowcharges (status and counters)\n"
    "\n"
    "[crossbow_charges]\n"
    "\n"
    "# Master switch.\n"
    "enabled = true\n"
    "\n"
    "# itemtypes.txt codes that count as crossbows. Child types count too.\n"
    "crossbow_item_types = [\"xbow\"]\n"
    "\n"
    "# Bolts every crossbow user has. The max bolts stat adds to it.\n"
    "default_max_bolts = 1\n"
    "\n"
    "# Frames every crossbow attack takes. The attack frames stat adds to it.\n"
    "default_attack_frames = 7\n"
    "\n"
    "# itemstatcost.txt rows.\n"
    "#   max bolts:     bolts added to default_max_bolts.\n"
    "#   current bolts: written by this plugin, read only for everything else.\n"
    "#   damage:        percent added to the crossbow damage bonus.\n"
    "#   attack frames: frames added to default_attack_frames.\n"
    "max_bolts_stat_id = 513\n"
    "current_bolts_stat_id = 514\n"
    "damage_stat_id = 515\n"
    "attack_frames_stat_id = 517\n"
    "\n"
    "# Percent always added to the bonus, before the damage stat.\n"
    "# 25 with a +50 item gives 175% damage.\n"
    "damage_base = 25\n"
    "\n"
    "# Name of the shadow ImageWidget inside each skill button.\n"
    "sweep_widget_name = \"CrossbowSweep\"\n"
    "\n"
    "# Frames in the shadow sprite. Frame 0 is the full shadow.\n"
    "sweep_frames = 64\n"
    "\n"
    "# Shadow colour (red, green, blue, 0 to 255) and opacity (percent, 0 is\n"
    "# invisible, 100 is solid). The sprite itself is plain white; these set it.\n"
    "shadow_color = [128, 128, 128]\n"
    "shadow_opacity = 50\n";

constexpr std::size_t MaximumConfigBytes = 64 * 1024;
constexpr std::size_t MaximumItemTypes   = 8;

struct Config {
    bool                       enabled{ true };
    std::vector<std::string>   itemTypes{ "xbow" };
    std::int32_t               defaultMaxBolts{ 1 };
    std::int32_t               maxBoltsStat{ 513 };
    std::int32_t               currentBoltsStat{ 514 };
    std::int32_t               damageStat{ 515 };
    std::int32_t               damageBase{ 25 };
    std::int32_t               defaultAttackFrames{ 7 };
    std::int32_t               attackFramesStat{ 517 };
    std::string                sweepName{ "CrossbowSweep" };
    std::int32_t               sweepFrames{ 64 };
    std::array<std::int32_t, 3> shadowColor{ 128, 128, 128 };
    std::int32_t               shadowOpacity{ 50 };
};

Config      Settings;
std::string ConfigProblems;

void Problem(std::string_view text) {
    if (!ConfigProblems.empty()) ConfigProblems += "; ";
    ConfigProblems.append(text);
}

auto Trim(std::string_view value) noexcept -> std::string_view {
    std::size_t first = 0;
    while (first < value.size() && (value[first] == ' ' || value[first] == '\t'
            || value[first] == '\r' || value[first] == '\n')) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first && (value[last - 1] == ' ' || value[last - 1] == '\t'
            || value[last - 1] == '\r' || value[last - 1] == '\n')) {
        --last;
    }
    return value.substr(first, last - first);
}

// Drops a trailing # comment that is not inside a quoted string.
auto StripComment(std::string_view line) noexcept -> std::string_view {
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '"') quoted = !quoted;
        if (line[i] == '#' && !quoted) return line.substr(0, i);
    }
    return line;
}

auto ParseBool(std::string_view value, bool& out) noexcept -> bool {
    if (value == "true") { out = true; return true; }
    if (value == "false") { out = false; return true; }
    return false;
}

auto ParseInt(std::string_view value, std::int32_t minimum, std::int32_t maximum, std::int32_t& out) noexcept -> bool {
    if (value.empty()) return false;
    std::int64_t result = 0;
    std::size_t  i      = 0;
    bool negative = false;
    if (value[0] == '-' || value[0] == '+') {
        negative = value[0] == '-';
        i = 1;
    }
    if (i >= value.size()) return false;
    for (; i < value.size(); ++i) {
        if (value[i] == '_') continue;
        if (value[i] < '0' || value[i] > '9') return false;
        result = result * 10 + (value[i] - '0');
        if (result > 1'000'000'000) return false;
    }
    if (negative) result = -result;
    if (result < minimum || result > maximum) return false;
    out = static_cast<std::int32_t>(result);
    return true;
}

auto ParseString(std::string_view value, std::string& out) -> bool {
    if (value.size() < 2 || value.front() != '"' || value.back() != '"') return false;
    out.assign(value.substr(1, value.size() - 2));
    return true;
}

// [a, b, c] -> items, each trimmed.
auto SplitArray(std::string_view value, std::vector<std::string_view>& items) -> bool {
    if (value.size() < 2 || value.front() != '[' || value.back() != ']') return false;
    std::string_view inner = Trim(value.substr(1, value.size() - 2));
    items.clear();
    while (!inner.empty()) {
        const std::size_t comma = inner.find(',');
        const std::string_view item = Trim(inner.substr(0, comma));
        if (!item.empty()) items.push_back(item);
        if (comma == std::string_view::npos) break;
        inner = inner.substr(comma + 1);
    }
    return true;
}

void ApplyConfigLine(std::string_view section, std::string_view key, std::string_view value) {
    if (section != "crossbow_charges") return;
    char note[160];
    const auto bad = [&](const char* what) {
        std::snprintf(note, sizeof(note), "%.*s: %s, default kept",
            static_cast<int>(key.size()), key.data(), what);
        Problem(note);
    };
    if (key == "enabled") {
        if (!ParseBool(value, Settings.enabled)) bad("expected true or false");
    } else if (key == "crossbow_item_types") {
        std::vector<std::string_view> items;
        std::vector<std::string>      codes;
        bool ok = SplitArray(value, items) && !items.empty() && items.size() <= MaximumItemTypes;
        for (const std::string_view item : items) {
            std::string code;
            if (!ParseString(item, code) || code.empty() || code.size() > 4) { ok = false; break; }
            codes.push_back(code);
        }
        if (ok) Settings.itemTypes = codes;
        else bad("expected a list of 1 to 8 itemtypes.txt codes, each up to 4 characters");
    } else if (key == "default_max_bolts") {
        if (!ParseInt(value, 0, 1000, Settings.defaultMaxBolts)) bad("expected 0 to 1000");
    } else if (key == "max_bolts_stat_id") {
        if (!ParseInt(value, 0, 32767, Settings.maxBoltsStat)) bad("expected 0 to 32767");
    } else if (key == "current_bolts_stat_id") {
        if (!ParseInt(value, 0, 32767, Settings.currentBoltsStat)) bad("expected 0 to 32767");
    } else if (key == "damage_stat_id") {
        if (!ParseInt(value, 0, 32767, Settings.damageStat)) bad("expected 0 to 32767");
    } else if (key == "default_attack_frames") {
        if (!ParseInt(value, 0, 1000, Settings.defaultAttackFrames)) bad("expected 0 to 1000");
    } else if (key == "attack_frames_stat_id") {
        if (!ParseInt(value, 0, 32767, Settings.attackFramesStat)) bad("expected 0 to 32767");
    } else if (key == "damage_base") {
        if (!ParseInt(value, -100, 100000, Settings.damageBase)) bad("expected -100 to 100000");
    } else if (key == "sweep_widget_name") {
        std::string name;
        if (ParseString(value, name) && !name.empty() && name.size() < 64) Settings.sweepName = name;
        else bad("expected a quoted widget name");
    } else if (key == "sweep_frames") {
        if (!ParseInt(value, 1, 1024, Settings.sweepFrames)) bad("expected 1 to 1024");
    } else if (key == "shadow_color") {
        std::vector<std::string_view> items;
        std::array<std::int32_t, 3>   color{};
        bool ok = SplitArray(value, items) && items.size() == 3;
        for (std::size_t i = 0; ok && i < 3; ++i) ok = ParseInt(items[i], 0, 255, color[i]);
        if (ok) Settings.shadowColor = color;
        else bad("expected [red, green, blue], each 0 to 255");
    } else if (key == "shadow_opacity") {
        if (!ParseInt(value, 0, 100, Settings.shadowOpacity)) bad("expected 0 to 100");
    }
}

void ParseConfig(std::string_view text) {
    std::string_view section;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        std::string_view line = Trim(StripComment(text.substr(0, end)));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (line.empty()) continue;
        if (line.front() == '[') {
            if (line.back() == ']') section = Trim(line.substr(1, line.size() - 2));
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) continue;
        ApplyConfigLine(section, Trim(line.substr(0, equals)), Trim(line.substr(equals + 1)));
    }
}

void ReadConfiguration() {
    if (!Context->EnsureConfig(DefaultConfigToml)) {
        Context->LogWarn("CrossbowCharges: the config file could not be created; using defaults.");
    } else {
        std::string buffer(MaximumConfigBytes, '\0');
        if (Context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size()), nullptr)) {
            buffer.resize(std::strlen(buffer.c_str()));
            ParseConfig(buffer);
        } else {
            Context->LogWarn("CrossbowCharges: the config file could not be read; using defaults.");
        }
    }
    if (!ConfigProblems.empty()) {
        D2RL::LogWarnF(Context, "CrossbowCharges: %s", ConfigProblems.c_str());
    }
}

// ---------------------------------------------------------------------------
// Memory helpers
// ---------------------------------------------------------------------------

auto Bytes(void* base, std::size_t offset) noexcept -> std::uint8_t* {
    return static_cast<std::uint8_t*>(base) + offset;
}

template <typename T>
auto Read(void* base, std::size_t offset) noexcept -> T {
    T value{};
    std::memcpy(&value, Bytes(base, offset), sizeof(T));
    return value;
}

template <typename T>
void Write(void* base, std::size_t offset, T value) noexcept {
    std::memcpy(Bytes(base, offset), &value, sizeof(T));
}

auto IsPlayer(void* unit) noexcept -> bool {
    return unit != nullptr && UnitType(unit) == 0;
}

auto GuidOf(void* unit) noexcept -> std::uint32_t {
    return UnitId(unit, SourceFile, __LINE__);
}

auto CastIdOf(void* unit) noexcept -> std::uint32_t {
    return Read<std::uint32_t>(unit, UnitCastIdOffset);
}

auto SkillIdOf(void* skill) noexcept -> std::int32_t {
    void* record = Read<void*>(skill, 0);
    return record != nullptr ? static_cast<std::int32_t>(Read<std::uint16_t>(record, 0)) : -1;
}

auto ValidCastId(std::uint32_t castId) noexcept -> bool {
    return castId != 0 && castId != 0xFFFFFFFFU;
}

auto LocalPlayer() noexcept -> void* {
    return PlayerFromIndex(LocalPlayerIndex());
}

// ---------------------------------------------------------------------------
// Crossbows
// ---------------------------------------------------------------------------

const D2RL::DataTableServiceV1* DataTables{};

// Index 1..3 = DataTables bank = the unit's data context byte.
struct BankTypes {
    std::array<std::atomic<std::int32_t>, MaximumItemTypes> ids{};
    std::atomic<std::uint32_t> count{};
};
std::array<BankTypes, 4> CrossbowTypes{};

auto FourCc(const std::string& code) noexcept -> std::uint32_t {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        const auto c = static_cast<std::uint8_t>(i < code.size() ? code[i] : ' ');
        value |= static_cast<std::uint32_t>(c) << (8 * i);
    }
    return value;
}

// Runs on every data-table load.
void ResolveCrossbowTypes(const D2RL::PluginContext* context) {
    if (DataTables == nullptr) return;
    std::string   report;
    std::uint32_t found = 0;
    for (std::uint32_t bank = 1; bank <= 3; ++bank) {
        std::uint32_t count = 0;
        for (const std::string& code : Settings.itemTypes) {
            D2RL::DataTables::RowView row{};
            row.structSize = D2RL::DataTables::RowViewSize;
            const auto result = DataTables->findRowByCode(context, static_cast<D2RL::DataTables::Bank>(bank),
                D2RL::DataTables::TableId::ItemTypes, FourCc(code), &row);
            if (result == D2RL::DataTables::Result::Success
                    && D2RL::DataTables::HasRowViewField(&row, D2RL::DataTables::RowViewRequiredSize)
                    && count < MaximumItemTypes) {
                CrossbowTypes[bank].ids[count].store(static_cast<std::int32_t>(row.rowIndex), std::memory_order_relaxed);
                ++count;
            }
        }
        CrossbowTypes[bank].count.store(count, std::memory_order_release);
        found += count;
        char part[48];
        std::snprintf(part, sizeof(part), "%sbank %u: %u of %zu", report.empty() ? "" : ", ",
            bank, count, Settings.itemTypes.size());
        report += part;
    }
    if (found == 0) {
        D2RL::LogErrorF(context, "CrossbowCharges: none of the crossbow item types were found in itemtypes.txt "
            "(%s); no skill counts as a crossbow skill.", report.c_str());
        return;
    }
    D2RL::LogInfoF(context, "CrossbowCharges: crossbow item types found (%s).", report.c_str());
}

auto MatchesBank(void* weapon, const BankTypes& bank) noexcept -> bool {
    const std::uint32_t count = bank.count.load(std::memory_order_acquire);
    for (std::uint32_t i = 0; i < count && i < MaximumItemTypes; ++i) {
        if (CheckItemType(weapon, bank.ids[i].load(std::memory_order_relaxed)) != 0) return true;
    }
    return false;
}

auto HasCrossbow(void* unit) noexcept -> bool {
    void* inventory = Inventory(unit, SourceFile, __LINE__);
    if (inventory == nullptr) return false;
    void* weapon = LeftHandWeapon(inventory);
    if (weapon == nullptr) return false;
    const std::uint8_t context = DataContext(unit);
    if (context >= 1 && context <= 3) return MatchesBank(weapon, CrossbowTypes[context]);
    for (std::size_t bank = 1; bank <= 3; ++bank) {
        if (MatchesBank(weapon, CrossbowTypes[bank])) return true;
    }
    return false;
}

// globaldelay above 0 while a crossbow is held.
auto IsCrossbowSkill(void* unit, void* skill) noexcept -> bool {
    if (skill == nullptr || !HasCrossbow(unit)) return false;
    const std::int32_t skillId = SkillIdOf(skill);
    if (skillId < 0) return false;
    const std::uint8_t context = DataContext(unit);
    void* record = SkillsRecord(context, skillId);
    if (record == nullptr) return false;
    const std::int32_t level = SkillLevel(unit, skill, 1, 0);
    return Evaluate(context, unit, Read<std::uint32_t>(record, SkillGlobalDelayCalc), skillId, level) > 0;
}

// default_max_bolts plus the max bolts stat, at least 1.
auto MaxBolts(void* unit) noexcept -> std::int32_t {
    return std::max(1, Settings.defaultMaxBolts + GetStat(unit, Settings.maxBoltsStat, 0));
}

// ---------------------------------------------------------------------------
// Server pools (game thread)
// ---------------------------------------------------------------------------

struct Pool {
    bool                     initialized{};
    bool                     everSpent{};
    std::int32_t             current{};
    std::int32_t             lastMax{ 1 };
    std::int32_t             lastDuration{ 1 };
    std::int32_t             headStart{};
    std::int32_t             written{ -1 };
    std::uint32_t            lastCastId{};  // cast that spent the last bolt
    std::int32_t             tickFrame{};   // server frame of the last update
    std::uint64_t            tickMs{};      // wall clock of the last update
    std::deque<std::int32_t> reloads;   // frames, front = reloading now
};

constexpr std::size_t TagRingSize = 512;
struct TagRing {
    std::array<std::uint32_t, TagRingSize> ids{};
    std::size_t                            next{};
};

std::mutex                                  ServerMutex;
void*                                       ServerGame{};
std::unordered_map<std::uint32_t, Pool>     Pools;
std::unordered_map<std::uint32_t, TagRing>  Tags;
std::atomic<bool>                           ResetRequested{};

void ResetServerIfNeeded(void* game) {
    const bool requested = ResetRequested.exchange(false);
    if (requested || (game != nullptr && game != ServerGame)) {
        Pools.clear();
        Tags.clear();
        if (game != nullptr) ServerGame = game;
    }
}

auto FrameOf(void* game) noexcept -> std::int32_t {
    return Read<std::int32_t>(game, GameFrameOffset);
}

// Maximum bolts follows the crossbow; without one the last value stays.
void RefreshCapacity(Pool& pool, void* unit, std::int32_t frame) {
    if (HasCrossbow(unit)) {
        pool.lastMax = MaxBolts(unit);
        if (!pool.initialized) {
            pool.initialized = true;
            pool.current     = pool.lastMax;
            pool.reloads.clear();
        }
    }
    if (!pool.initialized) return;
    const std::int32_t max   = pool.lastMax;
    const std::int32_t total = pool.current + static_cast<std::int32_t>(pool.reloads.size());
    if (total > max) {
        while (!pool.reloads.empty() && pool.current + static_cast<std::int32_t>(pool.reloads.size()) > max) {
            pool.reloads.pop_back();
        }
        pool.current = std::min(pool.current, max);
    } else if (total < max) {
        if (!pool.everSpent) {
            pool.current += max - total;
        } else {
            if (pool.reloads.empty()) pool.headStart = frame;
            for (std::int32_t i = total; i < max; ++i) pool.reloads.push_back(pool.lastDuration);
        }
    }
}

void Advance(Pool& pool, std::int32_t frame) {
    while (!pool.reloads.empty()) {
        const std::int32_t duration = std::max(1, pool.reloads.front());
        if (frame - pool.headStart < duration) break;
        pool.headStart += duration;
        pool.reloads.pop_front();
        ++pool.current;
        BoltsReloaded.fetch_add(1, std::memory_order_relaxed);
    }
    if (pool.reloads.empty()) pool.headStart = frame;
}

// Returns the value to write, or -1. The write itself happens after the lock
// is released, since it runs the game's stat-change callback.
auto Commit(Pool& pool) -> std::int32_t {
    if (pool.current == pool.written) return -1;
    pool.written = pool.current;
    return pool.current;
}

void WriteBolts(void* unit, std::int32_t value) {
    if (value >= 0) SetStat(unit, Settings.currentBoltsStat, value, 0);
}

void TickServer(void* game, void* unit) {
    const std::uint32_t guid  = GuidOf(unit);
    std::int32_t        write = -1;
    {
        std::lock_guard lock(ServerMutex);
        ResetServerIfNeeded(game);
        Pool& pool = Pools[guid];
        const std::int32_t frame = FrameOf(game);
        RefreshCapacity(pool, unit, frame);
        if (!pool.initialized) return;
        Advance(pool, frame);
        pool.tickFrame = frame;
        pool.tickMs    = GetTickCount64();
        write = Commit(pool);
    }
    WriteBolts(unit, write);
}

void SpendBolt(void* game, void* unit, std::int32_t frames) {
    const std::uint32_t guid   = GuidOf(unit);
    const std::uint32_t castId = CastIdOf(unit);
    std::int32_t        write  = -1;
    {
        std::lock_guard lock(ServerMutex);
        ResetServerIfNeeded(game);
        Pool& pool = Pools[guid];
        const std::int32_t frame = FrameOf(game);
        RefreshCapacity(pool, unit, frame);
        if (!pool.initialized) return;
        Advance(pool, frame);
        // A later do-event of a cast that already spent (Strafe's next arrow).
        if (ValidCastId(castId) && castId == pool.lastCastId) {
            RepeatStepsIgnored.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        pool.lastCastId = castId;
        if (pool.current <= 0) {
            BoltsRefused.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        --pool.current;
        const std::int32_t duration = std::max(1, frames);
        if (pool.reloads.empty()) pool.headStart = frame;
        pool.reloads.push_back(duration);
        pool.everSpent    = true;
        pool.lastDuration = duration;
        pool.tickFrame    = frame;
        pool.tickMs       = GetTickCount64();
        write = Commit(pool);
    }
    WriteBolts(unit, write);
    BoltsSpent.fetch_add(1, std::memory_order_relaxed);
}

auto ServerBolts(void* unit) -> std::int32_t {
    const std::uint32_t guid = GuidOf(unit);
    std::lock_guard lock(ServerMutex);
    ResetServerIfNeeded(nullptr);
    Pool& pool = Pools[guid];
    if (!pool.initialized) {
        pool.initialized = true;
        pool.lastMax     = MaxBolts(unit);
        pool.current     = pool.lastMax;
    }
    return pool.current;
}

void Tag(std::uint32_t guid, std::uint32_t castId) {
    if (!ValidCastId(castId)) return;
    TagRing& ring = Tags[guid];
    ring.ids[ring.next % TagRingSize] = castId;
    ++ring.next;
}

auto IsTagged(std::uint32_t guid, std::uint32_t castId) -> bool {
    if (!ValidCastId(castId)) return false;
    const auto it = Tags.find(guid);
    if (it == Tags.end()) return false;
    return std::find(it->second.ids.begin(), it->second.ids.end(), castId) != it->second.ids.end();
}

// Set while item event function 20 runs for an owner carrying a tagged id.
thread_local bool          ProcFromCrossbow{};
thread_local std::uint32_t ProcOwner{};

// ---------------------------------------------------------------------------
// Client view (UI thread)
// ---------------------------------------------------------------------------
// On a host the server pool lives in this process and is read directly. A
// joined TCP/IP client only has the current-bolts stat, so it keeps its own
// copy of the reload queue: each drop of the stat is a fired bolt, reloading
// for the globaldelay the client evaluated for its own cast. The drop and the
// cast arrive in either order; a bolt whose cast has not been seen yet gets
// its time the moment the cast arrives. No two locks are ever held together.

struct SweepView {
    std::int32_t bolts{};
    bool         reloading{};
    float        progress{};
};

auto HostView(std::uint32_t guid, std::uint64_t now, SweepView& view) -> bool {
    std::lock_guard lock(ServerMutex);
    const auto it = Pools.find(guid);
    if (it == Pools.end() || !it->second.initialized) return false;
    const Pool& pool = it->second;
    view.bolts     = pool.current;
    view.reloading = !pool.reloads.empty();
    view.progress  = 0.0f;
    if (view.reloading) {
        const double duration  = static_cast<double>(std::max(1, pool.reloads.front()));
        const double sinceTick = now > pool.tickMs
            ? static_cast<double>(now - pool.tickMs) / static_cast<double>(MsPerFrame) : 0.0;
        const double elapsed   = static_cast<double>(pool.tickFrame - pool.headStart) + sinceTick;
        view.progress = static_cast<float>(std::clamp(elapsed / duration, 0.0, 0.999));
    }
    return true;
}

struct Prediction {
    std::int32_t  frames;
    std::uint64_t timeMs;
};

struct Reload {
    std::int32_t frames;
    bool         known;   // false until the client's own cast supplies the time
};

struct ClientPool {
    void*                  unit{};
    std::uint32_t          guid{ 0xFFFFFFFFU };
    bool                   synced{};
    bool                   everSpent{};
    std::int32_t           lastStat{};
    std::int32_t           lastMax{ 1 };
    std::int32_t           lastDuration{ 25 };
    std::uint64_t          headStartMs{};
    std::deque<Reload>     reloads;
    std::deque<Prediction> predictions;   // casts the stat has not shown yet
    std::uint32_t          lastCastId{};  // client cast that made the last prediction
};

std::mutex ClientMutex;
ClientPool Client;

void UpdateRemote(void* unit, std::uint64_t now) {
    const std::uint32_t guid = GuidOf(unit);
    if (unit != Client.unit || guid != Client.guid) {
        Client      = ClientPool{};
        Client.unit = unit;
        Client.guid = guid;
    }
    while (!Client.predictions.empty() && now - Client.predictions.front().timeMs > PredictionTimeoutMs) {
        Client.predictions.pop_front();
    }
    const std::int32_t previousMax = Client.lastMax;
    if (HasCrossbow(unit)) Client.lastMax = MaxBolts(unit);
    const std::int32_t current = GetStat(unit, Settings.currentBoltsStat, 0);
    // A drop to exactly the new, lower maximum is the server clamping the pool
    // after a weapon change, not bolts being fired.
    const bool clamped = Client.lastMax < previousMax && current == Client.lastMax;

    if (!Client.synced) {
        Client.synced   = true;
        Client.lastStat = current;
        for (std::int32_t i = current; i < Client.lastMax; ++i) Client.reloads.push_back({ Client.lastDuration, false });
        Client.headStartMs = now;
    } else if (current < Client.lastStat && !clamped) {
        for (std::int32_t i = current; i < Client.lastStat; ++i) {
            Reload reload{ Client.lastDuration, false };
            if (!Client.predictions.empty()) {
                reload = { Client.predictions.front().frames, true };
                Client.predictions.pop_front();
                Client.lastDuration = reload.frames;
            }
            if (Client.reloads.empty()) Client.headStartMs = now;
            Client.reloads.push_back(reload);
            Client.everSpent = true;
        }
    } else if (current > Client.lastStat) {
        for (std::int32_t i = Client.lastStat; i < current && !Client.reloads.empty(); ++i) {
            Client.reloads.pop_front();
        }
        Client.headStartMs = now;
    }
    Client.lastStat = current;

    const std::int32_t total = current + static_cast<std::int32_t>(Client.reloads.size());
    if (total > Client.lastMax) {
        while (!Client.reloads.empty() && current + static_cast<std::int32_t>(Client.reloads.size()) > Client.lastMax) {
            Client.reloads.pop_back();
        }
    } else if (total < Client.lastMax && Client.everSpent) {
        if (Client.reloads.empty()) Client.headStartMs = now;
        for (std::int32_t i = total; i < Client.lastMax; ++i) Client.reloads.push_back({ Client.lastDuration, true });
    }
}

void PredictRemoteSpend(void* unit, std::int32_t frames, std::uint64_t now) {
    UpdateRemote(unit, now);
    frames = std::max(1, frames);
    for (Reload& reload : Client.reloads) {
        if (!reload.known) {
            reload              = { frames, true };
            Client.lastDuration = frames;
            return;
        }
    }
    Client.predictions.push_back({ frames, now });
}

auto RemoteView(std::uint64_t now) -> SweepView {
    SweepView view{};
    view.bolts     = std::max(0, Client.lastStat - static_cast<std::int32_t>(Client.predictions.size()));
    view.reloading = !Client.reloads.empty();
    if (view.reloading) {
        const auto   durationMs = static_cast<double>(std::max(1, Client.reloads.front().frames)) * MsPerFrame;
        const double elapsed    = now >= Client.headStartMs ? static_cast<double>(now - Client.headStartMs) : 0.0;
        view.progress = static_cast<float>(std::clamp(elapsed / durationMs, 0.0, 0.999));
    }
    return view;
}

// What the local player's buttons and client gate see.
auto LocalView(void* unit, std::uint64_t now) -> SweepView {
    SweepView view{};
    if (HostView(GuidOf(unit), now, view)) return view;
    std::lock_guard lock(ClientMutex);
    UpdateRemote(unit, now);
    return RemoteView(now);
}

// ---------------------------------------------------------------------------
// Hooks: cooldowns
// ---------------------------------------------------------------------------

std::int32_t __fastcall HookedGate(void* unit, void* skill) {
    if (!Active.load(std::memory_order_relaxed) || skill == nullptr || !IsPlayer(unit)
            || !IsCrossbowSkill(unit, skill)) {
        return OriginalGate(unit, skill);
    }
    const std::int32_t skillId = SkillIdOf(skill);
    if (CheckState(unit, LocalCooldownState) != 0) {
        void* list = StateStatList(unit, LocalCooldownState);
        if (list != nullptr && ReadListStat(DataContext(unit), list, LocalCooldownStat, skillId) > 0) return 0;
    }
    std::int32_t bolts = 0;
    if (IsServerUnit(unit) != 0) {
        bolts = ServerBolts(unit);
    } else if (unit == LocalPlayer()) {
        bolts = LocalView(unit, GetTickCount64()).bolts;
    } else {
        bolts = GetStat(unit, Settings.currentBoltsStat, 0);
    }
    return bolts > 0 ? 1 : 0;
}

void __fastcall HookedClientApply(void* unit, std::int32_t skillId, std::int32_t level) {
    if (!Active.load(std::memory_order_relaxed) || !IsPlayer(unit) || unit != LocalPlayer() || !HasCrossbow(unit)) {
        OriginalClientApply(unit, skillId, level);
        return;
    }
    const std::uint8_t context = DataContext(unit);
    void* record = SkillsRecord(context, skillId);
    if (record == nullptr) {
        OriginalClientApply(unit, skillId, level);
        return;
    }
    const std::int32_t global = Evaluate(context, unit, Read<std::uint32_t>(record, SkillGlobalDelayCalc), skillId, level);
    if (global <= 0) {
        OriginalClientApply(unit, skillId, level);
        return;
    }
    const std::uint64_t now = GetTickCount64();
    SweepView host{};
    if (!HostView(GuidOf(unit), now, host)) {
        const std::uint32_t castId = CastIdOf(unit);
        std::lock_guard lock(ClientMutex);
        if (!ValidCastId(castId) || castId != Client.lastCastId) {
            PredictRemoteSpend(unit, global, now);
            Client.lastCastId = castId;
        }
    }
    const std::int32_t local = Evaluate(context, unit, Read<std::uint32_t>(record, SkillLocalDelayCalc), skillId, level);
    if (local > 0) ClientLocalDelay(unit, local, skillId);
}

void __fastcall HookedServerApply(void* game, void* unit, std::int32_t skillId, std::int32_t level) {
    if (!Active.load(std::memory_order_relaxed) || game == nullptr || !IsPlayer(unit) || !HasCrossbow(unit)) {
        OriginalServerApply(game, unit, skillId, level);
        return;
    }
    const std::uint8_t context = Read<std::uint8_t>(game, GameDataContextOffset);
    void* record = SkillsRecord(context, skillId);
    if (record == nullptr) {
        OriginalServerApply(game, unit, skillId, level);
        return;
    }
    const std::int32_t global = Evaluate(context, unit, Read<std::uint32_t>(record, SkillGlobalDelayCalc), skillId, level);
    if (global <= 0) {
        OriginalServerApply(game, unit, skillId, level);
        return;
    }
    SpendBolt(game, unit, global);
    const std::int32_t local = Evaluate(context, unit, Read<std::uint32_t>(record, SkillLocalDelayCalc), skillId, level);
    if (local > 0) ServerLocalDelay(game, unit, local, skillId);
}

// Reached from the relay in place of the call at 43B18D; r13d is the skill id.
void __fastcall HookedDoHandlerGlobal(void* game, void* unit, std::int32_t frames, std::int32_t skillId) {
    (void)skillId;
    if (Active.load(std::memory_order_relaxed) && IsPlayer(unit) && HasCrossbow(unit)) {
        SpendBolt(game, unit, frames);
        return;
    }
    ServerGlobalDelay(game, unit, frames);
}

std::uint64_t __fastcall HookedPlayerRegen(void* game, void* unit, std::int32_t a3, std::int32_t a4) {
    if (Active.load(std::memory_order_relaxed) && game != nullptr && IsPlayer(unit)) TickServer(game, unit);
    return OriginalPlayerRegen(game, unit, a3, a4);
}

// ---------------------------------------------------------------------------
// Hooks: cast ids and damage
// ---------------------------------------------------------------------------

std::uint64_t __fastcall HookedSetUsedSkill(void* unit, void* skill, std::uint32_t castId) {
    const std::uint64_t result = OriginalSetUsedSkill(unit, skill, castId);
    if (!Active.load(std::memory_order_relaxed) || skill == nullptr || !ValidCastId(castId)
            || !IsPlayer(unit) || IsServerUnit(unit) == 0 || !IsCrossbowSkill(unit, skill)) {
        return result;
    }
    const std::uint32_t guid = GuidOf(unit);
    std::lock_guard lock(ServerMutex);
    ResetServerIfNeeded(nullptr);
    Tag(guid, castId);
    CastsTagged.fetch_add(1, std::memory_order_relaxed);
    return result;
}

std::uint64_t __fastcall HookedEventFunc20(void* game, std::uint64_t event, void* owner, void* target,
        void* damage, std::uint64_t packedStat, std::uint64_t a7, std::uint64_t a8, std::uint64_t a9) {
    const bool          previousTagged = ProcFromCrossbow;
    const std::uint32_t previousOwner  = ProcOwner;
    bool          tagged = false;
    std::uint32_t guid   = 0;
    if (Active.load(std::memory_order_relaxed) && IsPlayer(owner) && IsServerUnit(owner) != 0) {
        guid = GuidOf(owner);
        std::lock_guard lock(ServerMutex);
        tagged = IsTagged(guid, CastIdOf(owner));
    }
    ProcFromCrossbow = tagged;
    ProcOwner        = guid;
    const std::uint64_t result = NativeEventFunc20(game, event, owner, target, damage, packedStat, a7, a8, a9);
    ProcFromCrossbow = previousTagged;
    ProcOwner        = previousOwner;
    return result;
}

// Reached from the relay in place of the call at 589C37.
std::uint32_t __fastcall HookedProcCastId(void* game) {
    const std::uint32_t castId = GenerateCastId(game);
    if (ProcFromCrossbow && ValidCastId(castId)) {
        std::lock_guard lock(ServerMutex);
        Tag(ProcOwner, castId);
        ProcsTagged.fetch_add(1, std::memory_order_relaxed);
    }
    return castId;
}

auto Scale(std::int32_t value, std::int64_t numerator) noexcept -> std::int32_t {
    if (value <= 0) return value;
    const std::int64_t scaled = static_cast<std::int64_t>(value) * numerator / 100;
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(scaled, 0, std::numeric_limits<std::int32_t>::max()));
}

void BoostDamage(void* damage, std::int32_t percent) {
    const std::int64_t numerator = std::max<std::int64_t>(0, 100LL + percent);
    for (const std::size_t field : DamageFields) {
        Write<std::int32_t>(damage, field, Scale(Read<std::int32_t>(damage, field), numerator));
    }
    void*               entries = Read<void*>(damage, DamagePoisonEntries);
    const std::uint64_t count   = Read<std::uint64_t>(damage, DamagePoisonCount);
    if (entries != nullptr && count <= 4096) {
        for (std::uint64_t i = 0; i < count; ++i) {
            const std::size_t at = static_cast<std::size_t>(i) * PoisonEntrySize + PoisonEntryDamage;
            Write<std::int32_t>(entries, at, Scale(Read<std::int32_t>(entries, at), numerator));
        }
    }
}

// Reached from the relay in place of the call at 44CF93.
void __fastcall HookedDamageCalc(void* game, void* attacker, void* defender, void* damage) {
    CalculateDamage(game, attacker, defender, damage);
    if (!Active.load(std::memory_order_relaxed) || damage == nullptr || !IsPlayer(attacker)
            || IsServerUnit(attacker) == 0) {
        return;
    }
    const std::uint32_t guid = GuidOf(attacker);
    {
        std::lock_guard lock(ServerMutex);
        if (!IsTagged(guid, CastIdOf(attacker))) return;
    }
    BoostDamage(damage, Settings.damageBase + GetStat(attacker, Settings.damageStat, 0));
    HitsBoosted.fetch_add(1, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Hook: skill buttons
// ---------------------------------------------------------------------------

constexpr std::size_t TypeDescNameOffset = 0x08;
constexpr char        ImageWidgetName[]  = "ImageWidget";

void SetVisible(void* widget, bool visible) {
    auto** vtable = Read<void**>(widget, 0);
    reinterpret_cast<SetVisibleFn>(vtable[WidgetSetVisibleSlot])(widget, visible ? 1U : 0U);
}

// Reads the descriptor names only; never runs a type getter.
auto IsImageWidget(void* widget) -> bool {
    auto** vtable = Read<void**>(widget, 0);
    void*  desc   = reinterpret_cast<TypeDescFn>(vtable[WidgetTypeDescSlot])(widget);
    for (int depth = 0; desc != nullptr && depth < 32; ++depth) {
        const auto* name = Read<const char*>(desc, TypeDescNameOffset);
        if (name != nullptr && std::strncmp(name, ImageWidgetName, sizeof(ImageWidgetName)) == 0) return true;
        desc = Read<void*>(desc, TypeDescParentOffset);
    }
    return false;
}

auto FindSweep(void* button) -> void* {
    void* child = FindChild(button, Settings.sweepName.c_str());
    return child != nullptr && IsImageWidget(child) ? child : nullptr;
}

void WriteQuantity(void* button, std::int32_t bolts) {
    void* text = Read<void*>(button, ButtonQuantityOffset);
    if (text == nullptr) return;
    SetVisible(text, true);
    const bool        alternate = Read<std::uint8_t>(text, TextAltFlagA) != 0 && Read<std::uint8_t>(text, TextAltFlagB) != 0;
    const std::size_t offset    = alternate ? TextAltString : TextMainString;
    StringFormat(Bytes(text, offset), "%d", bolts);
}

// Only the no-bolts reason is cleared; a skill's own cooldown stays red.
void ClearCooldownTint(void* button) {
    if (std::memcmp(Bytes(button, ButtonTintOffset), Bytes(button, ButtonCooldownTintOffset), 16) == 0) {
        std::memcpy(Bytes(button, ButtonTintOffset), Bytes(button, ButtonNormalTintOffset), 16);
    }
}

void DrawSweep(void* sweep, const SweepView& view) {
    if (!view.reloading) {
        SetVisible(sweep, false);
        return;
    }
    const std::int32_t frames = std::max(1, Settings.sweepFrames);
    const std::int32_t frame  = std::clamp(static_cast<std::int32_t>(view.progress * static_cast<float>(frames)), 0, frames - 1);
    Write<std::int32_t>(sweep, ImageFrameOffset, frame);
    Write<float>(sweep, ImageTransparencyOffset, static_cast<float>(Settings.shadowOpacity) / 100.0f);
    Write<float>(sweep, ImageTintOffset + 0, static_cast<float>(Settings.shadowColor[0]) / 255.0f);
    Write<float>(sweep, ImageTintOffset + 4, static_cast<float>(Settings.shadowColor[1]) / 255.0f);
    Write<float>(sweep, ImageTintOffset + 8, static_cast<float>(Settings.shadowColor[2]) / 255.0f);
    Write<float>(sweep, ImageTintOffset + 12, 1.0f);
    SetVisible(sweep, true);
}

std::uint64_t __fastcall HookedButtonUpdate(void* button) {
    const std::uint64_t result = OriginalButtonUpdate(button);
    if (!Active.load(std::memory_order_relaxed) || !UiActive || button == nullptr) return result;

    void* sweep = FindSweep(button);
    const std::int32_t side    = Read<std::int32_t>(button, ButtonSideOffset);
    const std::int32_t skillId = Read<std::int32_t>(button, ButtonSkillIdOffset);
    bool      crossbow = false;
    SweepView view{};
    if ((side == 1 || side == 2) && skillId >= 0) {
        void* player = LocalPlayer();
        if (player != nullptr) {
            void* skill = SkillNode(player, skillId, Read<std::uint32_t>(button, ButtonSkillOwnerOffset));
            if (skill != nullptr && IsCrossbowSkill(player, skill)) {
                crossbow = true;
                view     = LocalView(player, GetTickCount64());
            }
        }
    }
    if (!crossbow) {
        if (sweep != nullptr) SetVisible(sweep, false);
        return result;
    }
    WriteQuantity(button, view.bolts);
    if (view.bolts <= 0) ClearCooldownTint(button);
    if (sweep != nullptr) DrawSweep(sweep, view);
    return result;
}

// ---------------------------------------------------------------------------
// Hook: crossbow attack speed
// ---------------------------------------------------------------------------

auto IsCrossbowAttackMode(std::int32_t mode) noexcept -> bool {
    return mode == 7;   // A1 only; S1..S4 carry non-attack skills such as traps
}

// Reached from the relay at 351597 with the unit and the rate the game just
// worked out; returns the rate to store.
std::int32_t __fastcall HookedAttackRate(void* unit, std::int32_t rate) {
    if (!Active.load(std::memory_order_relaxed) || !IsPlayer(unit)
            || !IsCrossbowAttackMode(Read<std::int32_t>(unit, UnitModeOffset)) || !HasCrossbow(unit)) {
        return rate;
    }
    const std::int32_t frames = Settings.defaultAttackFrames + GetStat(unit, Settings.attackFramesStat, 0);
    if (frames <= 0) return rate;
    void* record = Read<void*>(unit, UnitAnimRecordOffset);
    if (record == nullptr) return rate;
    const std::int32_t fpd = Read<std::int32_t>(record, AnimRecordFramesOffset);
    if (fpd <= 0) return rate;
    // ceil(256 * FPD / (N + 1)): the smallest rate that finishes in N frames.
    const std::int64_t wanted = (256LL * fpd + frames) / (static_cast<std::int64_t>(frames) + 1);
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(wanted, 1, 0x7FFF));
}

// ---------------------------------------------------------------------------
// Relay page and redirected calls
// ---------------------------------------------------------------------------

constexpr std::size_t RelayPageBytes   = 4'096;
constexpr std::size_t DoHandlerStub    = 0x00;  // 45 8B CD / FF 25 00000000 / dq
constexpr std::size_t ProcCastIdStub   = 0x20;  // FF 25 00000000 / dq
constexpr std::size_t DamageCalcStub   = 0x40;  // FF 25 00000000 / dq
constexpr std::size_t AttackRateStub   = 0x60;  // save, call HookedAttackRate(rsi, edi), mov ebx,7FFFh, ret

void* RelayPage{};
bool  DoHandlerPatched{};
bool  ProcCastIdPatched{};
bool  DamageCalcPatched{};
bool  AttackRatePatched{};
bool  EventSlotPatched{};

auto CanEncodeRel32(std::uintptr_t from, std::uintptr_t to) noexcept -> bool {
    const std::int64_t delta = static_cast<std::int64_t>(to) - static_cast<std::int64_t>(from) - static_cast<std::int64_t>(CallSize);
    return delta >= std::numeric_limits<std::int32_t>::min() && delta <= std::numeric_limits<std::int32_t>::max();
}

auto AllocateNear(std::uintptr_t low, std::uintptr_t high, std::size_t size) noexcept -> void* {
    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    const auto granularity = static_cast<std::uintptr_t>(systemInfo.dwAllocationGranularity);
    const auto aligned     = high & ~(granularity - 1U);
    for (std::uintptr_t delta = granularity; delta < 0x7000'0000ULL; delta += granularity) {
        const auto candidate = aligned + delta;
        if (!CanEncodeRel32(low, candidate + size)) break;
        if (void* memory = VirtualAlloc(reinterpret_cast<void*>(candidate), size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) {
            return memory;
        }
    }
    return nullptr;
}

void EncodeCall(std::uintptr_t site, std::uintptr_t target, std::uint8_t* out) noexcept {
    out[0] = 0xE8;
    const auto displacement = static_cast<std::int32_t>(static_cast<std::int64_t>(target) - static_cast<std::int64_t>(site + CallSize));
    std::memcpy(out + 1, &displacement, sizeof(displacement));
}

auto BuildRelayPage() -> bool {
    const std::uintptr_t low  = Base + AttackRateSiteRva;
    const std::uintptr_t high = Base + ProcCastIdCallRva;
    RelayPage = AllocateNear(low, high, RelayPageBytes);
    if (RelayPage == nullptr) return false;
    auto* page = static_cast<std::uint8_t*>(RelayPage);
    std::memset(page, 0xCC, RelayPageBytes);
    const auto put = [&](std::size_t at, const std::uint8_t* prefix, std::size_t prefixSize, const void* target) {
        if (prefixSize != 0) std::memcpy(page + at, prefix, prefixSize);
        const std::uint8_t jump[]{ 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
        std::memcpy(page + at + prefixSize, jump, sizeof(jump));
        const auto address = reinterpret_cast<std::uint64_t>(target);
        std::memcpy(page + at + prefixSize + sizeof(jump), &address, sizeof(address));
    };
    const std::uint8_t moveSkillId[]{ 0x45, 0x8B, 0xCD };  // mov r9d, r13d
    put(DoHandlerStub, moveSkillId, sizeof(moveSkillId), reinterpret_cast<const void*>(&HookedDoHandlerGlobal));
    put(ProcCastIdStub, nullptr, 0, reinterpret_cast<const void*>(&HookedProcCastId));
    put(DamageCalcStub, nullptr, 0, reinterpret_cast<const void*>(&HookedDamageCalc));
    // Entered by the call at 351597, so [rsp] returns to 35159C. Seven pushes
    // plus the return address keep rsp 16-byte aligned for the inner call.
    const std::uint8_t attackRate[]{
        0x50,                                // push rax
        0x51,                                // push rcx
        0x52,                                // push rdx
        0x41, 0x50,                          // push r8
        0x41, 0x51,                          // push r9
        0x41, 0x52,                          // push r10
        0x41, 0x53,                          // push r11
        0x48, 0x83, 0xEC, 0x20,              // sub  rsp, 20h
        0x48, 0x8B, 0xCE,                    // mov  rcx, rsi        ; unit
        0x8B, 0xD7,                          // mov  edx, edi        ; rate
        0xFF, 0x15, 0x1E, 0x00, 0x00, 0x00,  // call [rip+1Eh]       ; slot at +38h
        0x48, 0x83, 0xC4, 0x20,              // add  rsp, 20h
        0x8B, 0xF8,                          // mov  edi, eax
        0x41, 0x5B,                          // pop  r11
        0x41, 0x5A,                          // pop  r10
        0x41, 0x59,                          // pop  r9
        0x41, 0x58,                          // pop  r8
        0x5A,                                // pop  rdx
        0x59,                                // pop  rcx
        0x58,                                // pop  rax
        0xBB, 0xFF, 0x7F, 0x00, 0x00,        // mov  ebx, 7FFFh      ; the replaced instruction
        0xC3,                                // ret
    };
    static_assert(sizeof(attackRate) == 0x31);
    std::memcpy(page + AttackRateStub, attackRate, sizeof(attackRate));
    const auto attackRateTarget = reinterpret_cast<std::uint64_t>(&HookedAttackRate);
    std::memcpy(page + AttackRateStub + 0x38, &attackRateTarget, sizeof(attackRateTarget));
    DWORD previous = 0;
    const bool sealed = VirtualProtect(page, RelayPageBytes, PAGE_EXECUTE_READ, &previous) != FALSE;
    FlushInstructionCache(GetCurrentProcess(), page, RelayPageBytes);
    return sealed;
}

auto RedirectCall(std::uint64_t callRva, const std::uint8_t* original, std::size_t stub) -> bool {
    const std::uintptr_t site   = Base + callRva;
    const std::uintptr_t target = reinterpret_cast<std::uintptr_t>(RelayPage) + stub;
    if (!CanEncodeRel32(site, target)) return false;
    std::uint8_t call[CallSize]{};
    EncodeCall(site, target, call);
    return Context->PatchBytes(callRva, original, CallSize, call, CallSize);
}

void RestoreCall(std::uint64_t callRva, const std::uint8_t* original, std::size_t stub, bool& patched) {
    if (!patched) return;
    std::uint8_t current[CallSize]{};
    EncodeCall(Base + callRva, reinterpret_cast<std::uintptr_t>(RelayPage) + stub, current);
    if (Context->PatchBytes(callRva, current, CallSize, original, CallSize)) patched = false;
}

auto PatchEventSlot() -> bool {
    const std::uint64_t native = Base + EventFunc20Rva;
    const std::uint64_t hooked = reinterpret_cast<std::uint64_t>(&HookedEventFunc20);
    return Context->PatchBytes(EventFuncSlot20Rva, &native, sizeof(native), &hooked, sizeof(hooked));
}

void RestoreEventSlot() {
    if (!EventSlotPatched) return;
    const std::uint64_t native = Base + EventFunc20Rva;
    const std::uint64_t hooked = reinterpret_cast<std::uint64_t>(&HookedEventFunc20);
    if (Context->PatchBytes(EventFuncSlot20Rva, &hooked, sizeof(hooked), &native, sizeof(native))) EventSlotPatched = false;
}

void RemovePatches() {
    RestoreCall(AttackRateSiteRva, AttackRateSite, AttackRateStub, AttackRatePatched);
    RestoreEventSlot();
    RestoreCall(DamageCalcCallRva, DamageCalcWindow + DamageCalcCallOffset, DamageCalcStub, DamageCalcPatched);
    RestoreCall(ProcCastIdCallRva, ProcCastIdWindow + ProcCastIdCallOffset, ProcCastIdStub, ProcCastIdPatched);
    RestoreCall(DoHandlerCallRva, DoHandlerWindow + DoHandlerCallOffset, DoHandlerStub, DoHandlerPatched);
    // The relay page is kept: a thread may be inside it right now.
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

auto VerifyAll() -> bool {
    bool ok = true;
    for (const Witness& witness : Witnesses) {
        if (!Context->CheckExpectedBytes(witness.rva, witness.bytes, witness.size)) {
            D2RL::LogErrorF(Context, "CrossbowCharges: %s at RVA 0x%llX does not match this build.",
                witness.name, static_cast<unsigned long long>(witness.rva));
            ok = false;
        }
    }
    const std::uint64_t slot = Base + EventFunc20Rva;
    if (!Context->CheckExpectedBytes(EventFuncSlot20Rva, &slot, sizeof(slot))) {
        D2RL::LogErrorF(Context, "CrossbowCharges: item event slot 20 at RVA 0x%llX does not point at 0x%llX.",
            static_cast<unsigned long long>(EventFuncSlot20Rva), static_cast<unsigned long long>(EventFunc20Rva));
        ok = false;
    }
    return ok;
}

auto VerifyUi() -> bool {
    bool ok = true;
    for (const Witness& witness : UiWitnesses) {
        if (!Context->CheckExpectedBytes(witness.rva, witness.bytes, witness.size)) {
            D2RL::LogErrorF(Context, "CrossbowCharges: %s at RVA 0x%llX does not match this build.",
                witness.name, static_cast<unsigned long long>(witness.rva));
            ok = false;
        }
    }
    return ok;
}

void BindNatives() {
    ServerGlobalDelay = At<ServerGlobalFn>(ServerGlobalDelayRva);
    ServerLocalDelay  = At<ServerLocalFn>(ServerLocalDelayRva);
    ClientGlobalDelay = At<ClientGlobalFn>(ClientGlobalDelayRva);
    ClientLocalDelay  = At<ClientLocalFn>(ClientLocalDelayRva);
    CalculateDamage   = At<CalculateFn>(CalculateDamageRva);
    NativeEventFunc20 = At<EventFunc20Fn>(EventFunc20Rva);
    GenerateCastId    = At<GenerateCastIdFn>(GenerateCastIdRva);
    SkillsRecord      = At<SkillsRecordFn>(SkillsRecordRva);
    Evaluate          = At<EvaluateFn>(EvaluateFormulaRva);
    DataContext       = At<DataContextFn>(DataContextRva);
    UnitType          = At<UnitTypeFn>(UnitTypeRva);
    UnitId            = At<UnitIdFn>(UnitIdRva);
    IsServerUnit      = At<IsServerUnitFn>(IsServerUnitRva);
    Inventory         = At<InventoryFn>(InventoryRva);
    LeftHandWeapon    = At<WeaponFn>(LeftHandWeaponRva);
    CheckItemType     = At<CheckItemTypeFn>(CheckItemTypeRva);
    GetStat           = At<GetStatFn>(GetUnitStatRva);
    SetStat           = At<SetStatFn>(SetUnitStatRva);
    CheckState        = At<CheckStateFn>(CheckStateRva);
    StateStatList     = At<StateStatListFn>(StateStatListRva);
    ReadListStat      = At<ReadListStatFn>(ReadListStatRva);
    SkillLevel        = At<SkillLevelFn>(SkillLevelRva);
    SkillNode         = At<SkillNodeFn>(SkillNodeRva);
    LocalPlayerIndex  = At<PlayerIndexFn>(LocalPlayerIndexRva);
    PlayerFromIndex   = At<PlayerFromFn>(PlayerFromIndexRva);
    FindChild         = At<FindChildFn>(FindChildWidgetRva);
    StringFormat      = At<StringFormatFn>(StringFormatRva);
}

auto InstallHooks() -> bool {
    return Context->InstallInlineHook(CooldownGateRva, CooldownGateBytes, sizeof(CooldownGateBytes), &HookedGate, &OriginalGate)
        && Context->InstallInlineHook(ClientApplyRva, ClientApplyBytes, sizeof(ClientApplyBytes), &HookedClientApply, &OriginalClientApply)
        && Context->InstallInlineHook(ServerApplyRva, ServerApplyBytes, sizeof(ServerApplyBytes), &HookedServerApply, &OriginalServerApply)
        && Context->InstallInlineHook(PlayerRegenRva, PlayerRegenBytes, sizeof(PlayerRegenBytes), &HookedPlayerRegen, &OriginalPlayerRegen)
        && Context->InstallInlineHook(SetUsedSkillRva, SetUsedSkillBytes, sizeof(SetUsedSkillBytes), &HookedSetUsedSkill, &OriginalSetUsedSkill);
}

auto InstallPatches() -> bool {
    if (!BuildRelayPage()) {
        Context->LogError("CrossbowCharges: no relay page could be placed within reach of the game code.");
        return false;
    }
    DoHandlerPatched = RedirectCall(DoHandlerCallRva, DoHandlerWindow + DoHandlerCallOffset, DoHandlerStub);
    if (!DoHandlerPatched) return false;
    ProcCastIdPatched = RedirectCall(ProcCastIdCallRva, ProcCastIdWindow + ProcCastIdCallOffset, ProcCastIdStub);
    if (!ProcCastIdPatched) return false;
    DamageCalcPatched = RedirectCall(DamageCalcCallRva, DamageCalcWindow + DamageCalcCallOffset, DamageCalcStub);
    if (!DamageCalcPatched) return false;
    EventSlotPatched = PatchEventSlot();
    if (!EventSlotPatched) return false;
    // Optional part: a mismatch here only turns the fixed attack speed off.
    if (Context->CheckExpectedBytes(AttackRateWindowRva, AttackRateWindow, sizeof(AttackRateWindow))
            && Context->CheckExpectedBytes(AttackRateSiteRva, AttackRateSite, sizeof(AttackRateSite))) {
        AttackRatePatched = RedirectCall(AttackRateSiteRva, AttackRateSite, AttackRateStub);
    }
    if (!AttackRatePatched) {
        Context->LogWarn("CrossbowCharges: the animation rate code does not match; crossbow attacks keep the game's own "
                         "speed.");
    }
    return true;
}

auto StateName(PluginState state) noexcept -> const char* {
    switch (state) {
    case PluginState::NotLoaded:        return "not loaded";
    case PluginState::DisabledByConfig: return "disabled by config";
    case PluginState::UnsupportedBuild: return "unsupported build, nothing installed";
    case PluginState::InstallFailed:    return "install failed, nothing active";
    case PluginState::Active:           return "active";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void __cdecl OnDataTablesLoaded(const D2RL::PluginContext* context, const D2RL::Lifecycle::DataTablesLoadedEvent*, void*) noexcept {
    ResolveCrossbowTypes(context);
}

void __cdecl OnGameChanged(const D2RL::PluginContext*, const D2RL::Lifecycle::GameplayEvent*, void*) noexcept {
    ResetRequested.store(true);
    std::lock_guard lock(ClientMutex);
    Client = ClientPool{};
}

auto RegisterLifecycle() -> bool {
    if (Context->QueryService(D2RL::ServiceId::DataTable, D2RL::DataTableServiceV1Version, &DataTables)
            != D2RL::ServiceQueryResult::Success
            || !D2RL::HasDataTableServiceV1Field(DataTables, D2RL::DataTableServiceV1RequiredSize)) {
        DataTables = nullptr;
        Context->LogError("CrossbowCharges: the data table service is unavailable.");
        return false;
    }
    const D2RL::LifecycleServiceV1* lifecycle = nullptr;
    if (Context->QueryService(D2RL::ServiceId::Lifecycle, D2RL::LifecycleServiceV1Version, &lifecycle)
            != D2RL::ServiceQueryResult::Success
            || !D2RL::HasLifecycleServiceV1Field(lifecycle, D2RL::LifecycleServiceV1RequiredSize)) {
        Context->LogError("CrossbowCharges: the lifecycle service is unavailable.");
        return false;
    }
    D2RL::Lifecycle::DataTablesLoadedListener tables{};
    tables.structSize = D2RL::Lifecycle::DataTablesLoadedListenerSize;
    tables.callback   = &OnDataTablesLoaded;
    D2RL::Lifecycle::ListenerHandle handle = D2RL::Lifecycle::InvalidHandle;
    if (lifecycle->registerDataTablesLoadedListener(Context, &tables, &handle) != D2RL::Lifecycle::Result::Success
            || handle == D2RL::Lifecycle::InvalidHandle) {
        Context->LogError("CrossbowCharges: the data table listener was refused.");
        return false;
    }
    for (const D2RL::Lifecycle::GameplayEventKind kind :
            { D2RL::Lifecycle::GameplayEventKind::GameJoined, D2RL::Lifecycle::GameplayEventKind::GameLeft }) {
        D2RL::Lifecycle::GameplayEventListener game{};
        game.structSize = D2RL::Lifecycle::GameplayEventListenerSize;
        game.kind       = kind;
        game.callback   = &OnGameChanged;
        D2RL::Lifecycle::ListenerHandle gameHandle = D2RL::Lifecycle::InvalidHandle;
        if (lifecycle->registerGameplayEventListener(Context, &game, &gameHandle) != D2RL::Lifecycle::Result::Success) {
            Context->LogWarn("CrossbowCharges: a game join/leave listener was refused; pools are still reset when "
                             "a new game starts.");
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Console
// ---------------------------------------------------------------------------

auto __cdecl StatusCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept
        -> D2RL::ConsoleCommandResult {
    char line[256];
    const auto say = [&](const char* text) { command->plugin->WriteConsoleMessage(text); };
    std::snprintf(line, sizeof(line), "Crossbow Charges: %s%s.", StateName(State),
        State == PluginState::Active && !UiActive ? " (skill button visuals off)" : "");
    say(line);
    std::snprintf(line, sizeof(line), "  stats: max %d, current %d, damage %d; damage base %d; shadow '%s', %d frames.",
        Settings.maxBoltsStat, Settings.currentBoltsStat, Settings.damageStat, Settings.damageBase,
        Settings.sweepName.c_str(), Settings.sweepFrames);
    say(line);
    std::snprintf(line, sizeof(line), "  crossbow attacks: %s, %d frames + stat %d.",
        AttackRatePatched ? "fixed speed on" : "game speed (not installed)", Settings.defaultAttackFrames,
        Settings.attackFramesStat);
    say(line);
    std::snprintf(line, sizeof(line), "  crossbow item types per bank: classic %u, lod %u, rotw %u.",
        CrossbowTypes[1].count.load(), CrossbowTypes[2].count.load(), CrossbowTypes[3].count.load());
    say(line);
    std::snprintf(line, sizeof(line),
        "  bolts spent %llu, refused %llu, repeat steps ignored %llu, reloaded %llu; casts tagged %llu, procs tagged %llu, "
        "hits boosted %llu.",
        static_cast<unsigned long long>(BoltsSpent.load()), static_cast<unsigned long long>(BoltsRefused.load()),
        static_cast<unsigned long long>(RepeatStepsIgnored.load()),
        static_cast<unsigned long long>(BoltsReloaded.load()), static_cast<unsigned long long>(CastsTagged.load()),
        static_cast<unsigned long long>(ProcsTagged.load()), static_cast<unsigned long long>(HitsBoosted.load()));
    say(line);
    {
        std::lock_guard lock(ServerMutex);
        for (const auto& [guid, pool] : Pools) {
            const std::int32_t left = pool.reloads.empty() ? 0
                : std::max(0, pool.reloads.front() - (pool.tickFrame - pool.headStart));
            std::snprintf(line, sizeof(line), "  server player %u: %d of %d bolts, %zu reloading, next bolt in %d frames%s.",
                guid, pool.current, pool.lastMax, pool.reloads.size(), left,
                pool.initialized ? "" : " (not started)");
            say(line);
        }
    }
    {
        std::lock_guard lock(ClientMutex);
        if (Client.synced) {
            std::snprintf(line, sizeof(line), "  joined client copy: %d bolts, max %d, %zu reloading, %zu casts waiting.",
                Client.lastStat, Client.lastMax, Client.reloads.size(), Client.predictions.size());
            say(line);
        }
    }
    return D2RL::ConsoleCommandResult::Handled;
}

constexpr D2RL::PluginInfo PluginInfoData{
    .infoSize    = D2RL::PluginInfoSize,
    .apiVersion  = D2RL_PLUGIN_API_VERSION,
    .id          = PluginIdText,
    .name        = "Crossbow Charges",
    .version     = "1.1.1",
    .author      = "CelestialRayOne",
    .description = "Crossbow skills fire from a reloading pool of bolts with a skill-button shadow, and crossbow "
                   "hits deal a stat-driven damage bonus.",
    .flags       = D2RL::PluginFlags::Shared | D2RL::PluginFlags::NativeHooks,
};

} // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
    return &PluginInfoData;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
    Context = context;
    if (Context == nullptr || Context->exeBase == 0) return false;
    Base = Context->exeBase;

    if (!Context->RegisterConsoleCommand("crossbowcharges", &StatusCommand,
            "Show Crossbow Charges install state, bolt pools and counters.")) {
        Context->LogWarn("CrossbowCharges: the console command could not be registered.");
    }

    ReadConfiguration();
    if (!Settings.enabled) {
        State = PluginState::DisabledByConfig;
        Context->LogInfo("CrossbowCharges: disabled by config.");
        return true;
    }
    if (!VerifyAll()) {
        State = PluginState::UnsupportedBuild;
        Context->LogError("CrossbowCharges: this D2R build does not match; nothing was installed.");
        return true;
    }
    BindNatives();
    UiActive = VerifyUi();
    if (!UiActive) {
        Context->LogWarn("CrossbowCharges: the skill button parts do not match; bolts and damage work, the buttons "
                         "show nothing extra.");
    }

    if (!RegisterLifecycle()) {
        State = PluginState::InstallFailed;
        return true;
    }
    if (!InstallHooks() || !InstallPatches()) {
        RemovePatches();
        State = PluginState::InstallFailed;
        Context->LogError("CrossbowCharges: a hook or patch could not be installed; the plugin unloads.");
        return false;
    }
    if (UiActive && !Context->InstallInlineHook(ButtonUpdateRva, ButtonUpdateBytes, sizeof(ButtonUpdateBytes),
            &HookedButtonUpdate, &OriginalButtonUpdate)) {
        UiActive = false;
        Context->LogWarn("CrossbowCharges: the skill button hook could not be installed; the buttons show nothing extra.");
    }
    Active.store(true);
    State = PluginState::Active;
    D2RL::LogInfoF(Context, "CrossbowCharges: active (max bolts stat %d, current bolts stat %d, damage stat %d, "
        "damage base %d%%, skill buttons %s, attack frames %d + stat %d %s).", Settings.maxBoltsStat,
        Settings.currentBoltsStat, Settings.damageStat, Settings.damageBase, UiActive ? "on" : "off",
        Settings.defaultAttackFrames, Settings.attackFramesStat, AttackRatePatched ? "on" : "off");
    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    Active.store(false);
    if (Context != nullptr) RemovePatches();
}
