// =============================================================================
//  Armageddon as Aura  -  celestialrayone.armageddon-aura
//
//  Makes skills that use skills.txt srvdofunc 124 (Armageddon, and in D2R 3.3
//  also Hurricane) bootstrap correctly when they are driven as an aura rather
//  than cast by hand.
//
//  THE DEFECT
//
//  SKILLS_SrvDo124 opens with a cast-context gate:
//
//      used = UNITS_GetUsedSkill(unit);          // [[unit+0x100]+0x18]
//      if (!used || SkillId(used) != skillId) return 0;
//
//  Nothing on the aura side ever installs that pointer. The item_aura
//  (itemstatcost 151) driver calls the server skill do-handler with its
//  itemCast argument set, and the do-handler skips its entire used-skill
//  resolution block on that path. Same for the periodic re-fire of a learned
//  aura and for the re-fire after a respawn. So 124 is called on schedule,
//  self-rejects, and never creates the aura state nor schedules the
//  ACTIVESTATE periodic event that actually performs the skill.
//
//  THE FIX
//
//  Wrap 124. If the used-skill pointer already names this skill we are on a
//  real cast: step aside, the stock handler runs untouched. Otherwise resolve
//  the skill from the unit's own skill list with the SAME resolver and the
//  SAME selection flag that srvdofunc 146 uses (0x33DD40, selectionFlag =
//  false), install it as the used skill for the duration of one call, run the
//  stock handler through the trampoline, then restore the previous pointer.
//
//  Resolving it exactly the way 146 does matters: 124 stores the periodic
//  seed at skillNode+0x24 and 146 reads it back from the node its own
//  resolver returns. A different node means the seed lands somewhere 146
//  never looks.
//
//  SEQUENCE FREEZE
//
//  Stock 124 sets the unit's "skill action taken this frame" flag
//  (unit+0x124 bit 0x40) immediately after its gate. The per-frame animation
//  code refuses to advance the current sequence while that bit is set. Fine
//  for a one-shot cast; fatal for an aura, which re-sets it every few frames
//  and freezes a player mid-leap or mid-whirlwind forever. On the borrowed
//  path only, this plugin restores the bit to whatever it was before the
//  call. Real casts never reach that code.
//
//  THE AURA REFRESH THREW AWAY THE PENDING METEOR ROUND
//
//  Stock 124 ends by deleting every pending ACTIVESTATE event (type 5) of the
//  skill and scheduling a new one at frame + Param4 (delete call 0x5760FA,
//  set call 0x576124, Param4 read from record +0x1C4 at 0x5760DB). Right for
//  a cast. As an aura, 124 runs again on every aura refresh (every perdelay
//  frames, floored at 5, on an aligned frame grid: 0x434CC0, 0x437230), so a
//  meteor round only survives if it falls due before the next refresh.
//
//  Events due on the same frame run in the order they were scheduled:
//  0x48B5C0 hands 0x48C4D0 the bucket's oldest node (0x48C130 slot) and it
//  walks toward newer ones. With one item and perdelay == Param4 the meteor
//  round is scheduled inside the item tick, before that tick re-arms itself,
//  so it runs first and survives. With two items on the same skill, the
//  first item's re-arm is scheduled before the round (the second item's
//  refresh deleted and rescheduled it), runs first and deletes it again.
//  Every round is lost. perdelay < Param4 loses every round with any number
//  of items, and perdelay a multiple of Param4 loses the round that lands on
//  a refresh frame.
//
//  On the borrowed path only, the plugin hides the pending type 5 event from
//  124's delete (type byte set to 0xFD for the length of the call), then
//  removes the round 124 scheduled with the engine's own delete and gives
//  the kept round that round's skill level and cast id. The kept round keeps
//  its frame and its place in that frame's queue, so meteors fall every
//  Param4 frames whatever perdelay is and however many items grant the aura.
//
//  Ported from the ESR 2.4 byte patch (hook 363780 -> 191-byte cave at
//  386C60). No cave is needed here; the loader owns the trampoline, which
//  also frees 386C60 and removes the srvdofunc-73 dispatch landmine that
//  cave sat on.
// =============================================================================

#include <D2RLPlugin/api.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace CelestialRayOne::ArmageddonAura {
namespace {

constexpr char PluginVersion[] = "1.1.0";

// ---------------------------------------------------------------------------
//  Native struct layout (all offsets proven from the 3.3 image, see below)
// ---------------------------------------------------------------------------

// Unit -> D2SkillList. Proven by the tail of UNITS_GetSkillList (0x34B6E0),
// which ends in  mov rax,[rbx+0x100].
constexpr std::size_t UnitSkillListOffset = 0x100;

// D2SkillList +0x18 = "skill currently being used". Proven by
// UNITS_GetUsedSkill (0x34BA40) -> 0x33E090, which returns [list+0x18].
constexpr std::size_t SkillListUsedIndex = 0x18 / sizeof(void*);

// Unit +0x124, bit 0x40 = "a skill action was taken this frame". Proven by
// the setter at 0x34E790:  mov eax,[rcx+0x124] / or eax,0x40  (or and 0xBF).
constexpr std::size_t   UnitActionFlagsOffset = 0x124;
constexpr std::uint32_t SkillActionTakenBit   = 0x40;

// Unit -> first event of the unit. Proven by UNITS_GetEventList (0x48FE50),
// whose body ends in  mov rax,[rcx+0x148].
constexpr std::size_t UnitEventListOffset = 0x148;

// Event node layout. Proven by EVENT_SetEvent's store block (0x48BF55):
//   mov [rax+4],ebp (frame) / mov [rax+18h],ecx (customId) /
//   mov [rax+1Ch],ecx (customParam) / mov [rax+20h],ecx (third argument)
// and by EVENT_Delete's node walk (0x48B8F0):
//   mov rbp,[rbx+38h] (next event of this unit) / cmp [rbx+8],r15 (unit) /
//   movzx eax,byte [rbx] (type) / cmp [rbx+18h],r14d (customId, 0 = any)
constexpr std::size_t EventTypeOffset        = 0x00;
constexpr std::size_t EventFlagsOffset       = 0x02;
constexpr std::size_t EventCustomIdOffset    = 0x18;
constexpr std::size_t EventCustomParamOffset = 0x1C;
constexpr std::size_t EventCastIdOffset      = 0x20;
constexpr std::size_t EventUnitNextOffset    = 0x38;

// Flags word: bit 0 is set while the event's callback runs (0x48C526), bit 3
// is set by EVENT_Delete on a running event, which is freed after it returns.
constexpr std::uint16_t EventRunningFlag        = 0x0001;
constexpr std::uint16_t EventDeleteDeferredFlag = 0x0008;

// Type 5 = ACTIVESTATE. Its handler (0x435520) runs the aurastate's states.txt
// srvactivefunc (146 for Armageddon) with customId as the skill id,
// customParam as the skill level and the third argument as the cast id.
// 124 and 146 both use the skill id as customId.
constexpr std::uint8_t ActiveStateEventType = 5;

// Event types are 0..14 (EVENT_SetEvent asserts < 15), so no real event uses
// this. It is also not the 0xFE the aura-reactivation plugin hides its own
// events with, so neither plugin can ever restore the other's events.
constexpr std::uint8_t HiddenEventType = 0xFD;

constexpr std::size_t MaximumUnitEvents = 65536;

// ---------------------------------------------------------------------------
//  Native RVAs (D2R 3.3, D2RLoader process image, base 0x140000000)
// ---------------------------------------------------------------------------

// SKILLS_SrvDo124_Armageddon_Hurricane(game, unit, skillId, skillLevel)
constexpr std::uint64_t SrvDo124Rva = 0x575DE0;

// UNITS_GetSkillList(unit) -> [unit+0x100]
constexpr std::uint64_t GetSkillListRva = 0x34B6E0;

// SKILLS_GetSkillId(skillNode) -> *(int16*)*(void**)skillNode
constexpr std::uint64_t GetSkillIdRva = 0x33E080;

// SKILLS_GetHighestLevelSkillFromUnitAndId(unit, skillId, selectionFlag)
constexpr std::uint64_t GetHighestLevelSkillRva = 0x33DD40;

// UNITS_SetSkillActionTaken(unit, nonZeroToSet) -> unit+0x124 bit 0x40
constexpr std::uint64_t SetSkillActionTakenRva = 0x34E790;

// srvdofunc dispatch table. Slot 124 must resolve to SrvDo124Rva; that is the
// load-time proof that we hooked the function skills.txt actually dispatches.
constexpr std::uint64_t SrvDoTableRva    = 0x238EA00;
constexpr std::size_t   SrvDo124TableIdx = 124;

// The tail of 124: read Param4, delete (5, skillId), schedule (5, frame +
// Param4, skillId, level, cast id). The meteor timer fix relies on exactly
// this sequence, so it is fingerprinted rather than assumed.
constexpr std::uint64_t SrvDo124TimerTailRva = 0x5760DB;

// EVENT_Delete(game, unit, eventType, customId). Called through its entry.
// The entry itself is not fingerprinted because the aura-reactivation plugin
// inline-hooks it (its hook passes every type 5 delete to the stock code);
// the node walk below it is fingerprinted instead.
constexpr std::uint64_t EventDeleteRva     = 0x48B890;
constexpr std::uint64_t EventDeleteWalkRva = 0x48B8F0;

// UNITS_GetEventList(unit) -> [unit+0x148]. Only fingerprinted; the plugin
// reads the field directly.
constexpr std::uint64_t GetUnitEventListRva = 0x48FE50;

// EVENT_SetEvent's node store block, proving the node field offsets.
constexpr std::uint64_t SetEventStoreRva = 0x48BF55;

// ---------------------------------------------------------------------------
//  Entry byte signatures, read straight out of the 3.3 image
// ---------------------------------------------------------------------------

constexpr auto SrvDo124Expected = std::to_array<std::uint8_t>({
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x44, 0x89, 0x4C,
    0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41,
    0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC,
    0x40, 0x48, 0x8B, 0xF2, 0x48, 0x8B, 0xE9,
});

constexpr auto GetSkillListExpected = std::to_array<std::uint8_t>({
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
    0xD9, 0x48, 0x85, 0xC9, 0x75, 0x20, 0x88, 0x4C,
    0x24, 0x30, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8,
    0xD4, 0x98, 0xFF, 0xFF, 0x84, 0xC0, 0x74, 0x01,
    0xCC, 0x48, 0x8B, 0x83, 0x00, 0x01, 0x00, 0x00,
});

constexpr auto GetSkillIdExpected = std::to_array<std::uint8_t>({
    0x48, 0x85, 0xC9, 0x75, 0x03, 0x33, 0xC0, 0xC3,
    0x48, 0x8B, 0x01, 0x0F, 0xBF, 0x00, 0xC3,
});

constexpr auto GetHighestLevelSkillExpected = std::to_array<std::uint8_t>({
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
    0xEC, 0x20, 0x41, 0x0F, 0xB6, 0xD8, 0x8B, 0xFA,
    0xE8, 0x8B, 0xD9, 0x00, 0x00, 0x48, 0x85, 0xC0,
});

constexpr auto SetSkillActionTakenExpected = std::to_array<std::uint8_t>({
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x85, 0xC9, 0x75,
    0x18, 0x88, 0x4C, 0x24, 0x30, 0x48, 0x8D, 0x4C,
    0x24, 0x30, 0xE8, 0x29, 0x6F, 0xFF, 0xFF, 0x84,
    0xC0, 0x74, 0x27, 0xCC, 0x48, 0x83, 0xC4, 0x28,
});

// mov r12d,[rax+1C4h] / mov ebx,[rbp+170h] / mov r9d,r14d / mov r8d,5 /
// mov rdx,rsi / mov rcx,rbp / add ebx,r12d / call EVENT_Delete /
// mov rcx,rsi / call UNITS_GetCastId / mov [rsp+30h],eax / mov r9d,ebx /
// mov [rsp+28h],r13d / mov r8d,5 / mov rdx,rsi / mov [rsp+20h],r14d /
// mov rcx,rbp / call EVENT_SetEvent
constexpr auto SrvDo124TimerTailExpected = std::to_array<std::uint8_t>({
    0x44, 0x8B, 0xA0, 0xC4, 0x01, 0x00, 0x00, 0x8B,
    0x9D, 0x70, 0x01, 0x00, 0x00, 0x45, 0x8B, 0xCE,
    0x41, 0xB8, 0x05, 0x00, 0x00, 0x00, 0x48, 0x8B,
    0xD6, 0x48, 0x8B, 0xCD, 0x41, 0x03, 0xDC, 0xE8,
    0x91, 0x57, 0xF1, 0xFF, 0x48, 0x8B, 0xCE, 0xE8,
    0xB9, 0x55, 0xDD, 0xFF, 0x89, 0x44, 0x24, 0x30,
    0x44, 0x8B, 0xCB, 0x44, 0x89, 0x6C, 0x24, 0x28,
    0x41, 0xB8, 0x05, 0x00, 0x00, 0x00, 0x48, 0x8B,
    0xD6, 0x44, 0x89, 0x74, 0x24, 0x20, 0x48, 0x8B,
    0xCD, 0xE8, 0xF7, 0x55, 0xF1, 0xFF,
});

constexpr auto EventDeleteWalkExpected = std::to_array<std::uint8_t>({
    0x48, 0x8B, 0x6B, 0x38, 0x4C, 0x39, 0x7B, 0x08,
    0x74, 0x0F, 0x48, 0x8D, 0x4C, 0x24, 0x60, 0xE8,
    0x2C, 0xF3, 0xFF, 0xFF, 0x84, 0xC0, 0x74, 0x01,
    0xCC, 0x0F, 0xB6, 0x03, 0x41, 0x3B, 0xC4, 0x0F,
    0x85, 0x44, 0x01, 0x00, 0x00, 0x45, 0x85, 0xF6,
    0x74, 0x0A, 0x44, 0x39, 0x73, 0x18, 0x0F, 0x85,
    0x35, 0x01, 0x00, 0x00,
});

constexpr auto GetUnitEventListExpected = std::to_array<std::uint8_t>({
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x85, 0xC9, 0x75,
    0x1A, 0x88, 0x4C, 0x24, 0x30, 0x48, 0x8D, 0x4C,
    0x24, 0x30, 0xE8, 0xB9, 0xDD, 0xFF, 0xFF, 0x84,
    0xC0, 0x74, 0x01, 0xCC, 0x33, 0xC0, 0x48, 0x83,
    0xC4, 0x28, 0xC3, 0x48, 0x8B, 0x81, 0x48, 0x01,
    0x00, 0x00, 0x48, 0x83, 0xC4, 0x28, 0xC3,
});

constexpr auto SetEventStoreExpected = std::to_array<std::uint8_t>({
    0x8B, 0x4C, 0x24, 0x78, 0x44, 0x8B, 0xC5, 0x48,
    0x8B, 0xD8, 0x89, 0x68, 0x04, 0x89, 0x48, 0x18,
    0x8B, 0x8C, 0x24, 0x80, 0x00, 0x00, 0x00, 0x89,
    0x48, 0x1C, 0x8B, 0x8C, 0x24, 0x88, 0x00, 0x00,
    0x00, 0x89, 0x48, 0x20,
});

// ---------------------------------------------------------------------------
//  Native signatures
// ---------------------------------------------------------------------------

using ServerSkillFn =
    std::int32_t(__fastcall*)(void*, void*, std::int32_t, std::int32_t) noexcept;
using GetSkillListFn         = void*(__fastcall*)(void*) noexcept;
using GetSkillIdFn           = std::int32_t(__fastcall*)(void*) noexcept;
using GetHighestLevelSkillFn = void*(__fastcall*)(void*, std::int32_t, bool) noexcept;
using SetSkillActionTakenFn  = void(__fastcall*)(void*, std::int32_t) noexcept;
using EventDeleteFn          = void(__fastcall*)(void*, void*, std::int32_t, std::int32_t) noexcept;

// ---------------------------------------------------------------------------
//  Default configuration. This text is what EnsureConfig writes to
//  <scope>\d2rloader\config\celestialrayone.armageddon-aura.toml on first run,
//  so it is both the defaults and the plugin's documentation. Keep it in sync
//  with config/celestialrayone.armageddon-aura.toml in the repo.
// ---------------------------------------------------------------------------

constexpr char DefaultConfigToml[] = R"TOML(
# =============================================================================
#  Armageddon as Aura            plugin id: celestialrayone.armageddon-aura
#  Target: D2R 3.3 (D2RLoader image)                      Role: Server
# =============================================================================
#
#  WHAT IS BROKEN IN THE STOCK GAME
#
#  Armageddon is written as a CAST skill. Before its server handler does
#  anything at all it asks the engine "is this unit casting me right now?".
#  It answers that by reading the unit's "skill currently being used" pointer
#  and comparing that skill's id against its own. If they do not match it
#  returns immediately and does nothing.
#
#  That gate is correct for a normal cast. It is fatal for an aura, because
#  none of the paths that drive an aura ever fill that pointer in:
#
#    * An item_aura grant (itemstatcost 151) re-fires the skill on a timer.
#      The engine calls the skill handler with its "item cast" argument set,
#      and on that path it skips the whole block that would have resolved
#      and installed the used skill.
#    * The periodic re-fire that keeps a learned aura alive does the same.
#    * So does the re-fire that happens when you respawn.
#
#  The handler is therefore called on schedule, bails at its own first line,
#  and never creates the aura state or schedules the periodic event that
#  makes Armageddon actually rain. What you see in game is:
#
#    * The aura does nothing, or only works right after you manually cast it.
#    * It dies permanently on death, because the state is torn down when you
#      die and nothing ever bootstraps it again.
#    * When it is forced to run some other way, it runs with no owner
#      context, so anything its calcs read about the owner is garbage.
#
#  WHAT THIS PLUGIN DOES
#
#  It wraps the Armageddon server handler. On entry it reads the unit's
#  "skill currently being used" pointer and takes one of two paths:
#
#    * REAL CAST. The pointer already names this skill. The plugin steps
#      aside entirely and the stock handler runs untouched. Nothing about a
#      normal cast changes in any way.
#
#    * BORROWED CONTEXT. The pointer is empty or names a different skill.
#      The plugin looks this skill up on the unit's own skill list, lends it
#      to the engine as the used skill for the duration of exactly one call,
#      runs the stock handler, and then puts the previous value back. The
#      handler now passes its own gate and performs its full, ordinary
#      bootstrap: it creates the aura state, schedules the periodic event,
#      and stores the random seed that the active-state handler consumes on
#      every tick.
#
#  Because the skill is borrowed from the unit's real skill list rather than
#  faked, the aura runs with a proper owner context. That is what makes owner
#  stats and the real skill level readable inside its calcs, and it is why
#  the aura comes back by itself after you die instead of needing a re-equip.
#
#  IT ALSO FIXES THE LEAP / WHIRLWIND FREEZE
#
#  The stock handler sets the unit's "a skill action was taken this frame"
#  flag the moment it gets past its gate. The per-frame animation code reads
#  that flag and, when it is set, does not advance the unit's current
#  sequence. That is harmless for a one-off cast, which happens once.
#
#  It is not harmless for an aura. The aura ticks every few frames and re-sets
#  the flag each time, so a player who is mid-leap or mid-whirlwind is frozen
#  in place for as long as the aura is running.
#
#  On the borrowed-context path only, this plugin puts that flag back exactly
#  the way it found it once the handler returns. If the flag was already set
#  because the player genuinely acted on that frame, it stays set. Real casts
#  never reach this code and keep the flag exactly as the game intended.
#
#  IT ALSO KEEPS THE METEORS FALLING
#
#  The start handler was written for a cast: every time it runs it throws
#  away the pending meteor round and schedules a new one Param4 frames later.
#  As an aura it runs again on every refresh, and depending on perdelay,
#  Param4 and how many items grant the aura, that loses some or all of the
#  meteor rounds. Two items granting the same aura with perdelay equal to
#  Param4 is the classic case: one item works, two items produce no meteors.
#  The [aura_refresh] section below explains it and turns the fix on or off.
#
#  WHAT YOU NEED IN YOUR TXT FILES
#
#    * skills.txt: srvdofunc = 124 on the skill, and aura = 1.
#    * The skill must be GRANTED to the unit, normally as an oskill, so that
#      it exists on the unit's skill list. That is the skill the plugin
#      lends. With no grant there is nothing to lend and the plugin steps
#      aside, leaving the stock (broken) behaviour.
#    * Do NOT also apply the aurastate from a property. The handler creates
#      that state itself now, and a second property-applied copy of the same
#      state fights with it.
#    * states.txt: leave the aurastate's srvactivefunc alone. That is what
#      drives the per-tick Armageddon behaviour, and it only ever runs
#      because this plugin got the state created in the first place.
#
#    * The level the aura runs at is the AURA level: the total item_aura for
#      this skill across everything the unit wears. Two items with a level 1
#      aura run it at level 2. That level is lvl in the skill's own calcs
#      (auralencalc, aurarangecalc), it is the level of every meteor, and it
#      is the lvl a meteor's hit function gets for the target state's
#      aurastatcalc. The oskill only has to exist; its level is never read.
#    * Param4 is the number of frames between meteor rounds. perdelay is how
#      often the aura refreshes itself (never less than 5 frames). With
#      keep_meteor_timer on, the two no longer interfere.
#
#  NOTE FOR 3.3: the srvdofunc 124 handler is shared by Armageddon AND
#  Hurricane. With an empty skill list below, both are covered.
#
# =============================================================================


# Master switch. Set to false to unload the fix and leave the game stock.
enabled = true


[skills]
# Which skills the fix applies to, listed by their skills.txt *Id.
#
# Leave the list EMPTY to apply it to every skill that uses srvdofunc 124.
# That is the recommended setting and matches the behaviour of the original
# byte patch this plugin replaces.
#
# Fill it in only if you want to scope the fix. Because srvdofunc 124 is
# shared with Hurricane in 3.3, an empty list covers Hurricane too.
#
# Example:  ids = [ 249, 1305 ]
ids = []


[sequence]
# Restore the "skill action taken this frame" flag to whatever it was before
# the handler ran, on the borrowed-context path only. This is the leap /
# whirlwind freeze fix described above.
#
# Leave this on. Turn it off only to prove that some other patch is causing a
# sequence stall, because turning it off brings the freeze back.
restore_action_flag = true


[aura_refresh]
# Keep the pending meteor round when the aura refreshes itself.
#
# THE PROBLEM
#
#   Two timers drive an Armageddon aura:
#
#     * The aura timer. Every perdelay frames (never less than 5) the aura
#       refreshes itself and runs the Armageddon start handler again. Every
#       item that grants the aura has its own aura timer, and all of them
#       fire on the same frame.
#     * The meteor timer. Every Param4 frames the aura state's active
#       handler drops meteors and schedules the next round.
#
#   The start handler throws away the pending meteor round and schedules a
#   new one Param4 frames later, every time it runs. So a round only
#   survives if it falls due before the next refresh:
#
#     * perdelay shorter than Param4: no meteors at all, with any number of
#       items.
#     * perdelay a multiple of Param4: the round that falls on a refresh
#       frame is lost, every time.
#     * perdelay equal to Param4 and two or more items granting the aura: no
#       meteors at all. Things due on the same frame run in the order they
#       were scheduled. With one item, the meteor round was scheduled before
#       that item's timer, so it runs first and survives. With two items,
#       the first item's timer was scheduled before the round (the second
#       item's refresh had just rescheduled it), so it runs first and throws
#       the round away, and the second item's refresh puts it back in the
#       same losing position. That repeats forever.
#
# WHAT CHANGES
#
#   When the aura refreshes and a meteor round is already pending, that
#   round is kept exactly where it is. Everything else the refresh did
#   before still happens: the aura state and its duration are renewed, the
#   random seed is refreshed, and the pending round is given the current
#   skill level, so equipping or removing an item still changes the level
#   of the next meteors. Meteors fall every Param4 frames, whatever perdelay
#   is and however many items grant the aura.
#
#   A real cast of the skill is not touched: it still restarts the meteor
#   timer, which is what the game intends for a cast.
#
# Set to false to get the stock timer behaviour back.
keep_meteor_timer = true


[diagnostics]
# Writes a short burst of lines to the plugin log covering the first few
# jumpstarts and kept meteor rounds, then goes quiet so it cannot spam a
# per-frame path.
#
# Running counters are always available from the in-game console command
# "armageddon-aura", whether this is on or off.
enabled = false
)TOML";

// ---------------------------------------------------------------------------
//  Configuration
// ---------------------------------------------------------------------------

constexpr std::size_t MaxHandledSkills = 64;

struct Config {
    bool                      enabled{true};
    bool                      restoreActionFlag{true};
    bool                      keepMeteorTimer{true};
    bool                      diagnostics{false};
    std::vector<std::int32_t> skillIds{};
};

std::string Trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end   = text.size();
    const auto isSpace = [](char c) noexcept {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    while (begin < end && isSpace(text[begin])) ++begin;
    while (end > begin && isSpace(text[end - 1])) --end;
    return std::string(text.substr(begin, end - begin));
}

bool ParseBool(const std::string& value, bool& destination) {
    if (value == "true") {
        destination = true;
        return true;
    }
    if (value == "false") {
        destination = false;
        return true;
    }
    return false;
}

bool ParseIntArray(const std::string& value, std::vector<std::int32_t>& destination) {
    if (value.size() < 2 || value.front() != '[' || value.back() != ']') return false;

    destination.clear();
    const std::string inner = value.substr(1, value.size() - 2);
    std::size_t       pos   = 0;
    while (pos < inner.size()) {
        std::size_t comma = inner.find(',', pos);
        if (comma == std::string::npos) comma = inner.size();

        const std::string token = Trim(std::string_view(inner).substr(pos, comma - pos));
        if (!token.empty()) {
            char*      stop   = nullptr;
            const long parsed = std::strtol(token.c_str(), &stop, 10);
            if (stop == nullptr || *stop != '\0') return false;
            destination.push_back(static_cast<std::int32_t>(parsed));
        }
        pos = comma + 1;
    }
    return true;
}

// Deliberately small and forgiving: it understands exactly the keys this
// plugin owns, ignores anything else with a warning, and never refuses to
// load over a config typo. That keeps the plugin free of a TOML dependency,
// so dropping the folder in and adding one line to the build is enough.
bool ParseConfig(std::string_view text, Config& result, std::string& error, std::string& unknown) {
    Config      parsed{};
    std::string section;

    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end  = text.find('\n', pos);
        const bool  last = (end == std::string_view::npos);
        if (last) end = text.size();

        std::string line = Trim(text.substr(pos, end - pos));
        pos              = last ? text.size() : end + 1;

        const std::size_t comment = line.find('#');
        if (comment != std::string::npos) line = Trim(std::string_view(line).substr(0, comment));
        if (line.empty()) continue;

        if (line.front() == '[' && line.back() == ']') {
            section = Trim(std::string_view(line).substr(1, line.size() - 2));
            continue;
        }

        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) {
            error = "malformed line: " + line;
            return false;
        }

        const std::string key   = Trim(std::string_view(line).substr(0, equals));
        const std::string value = Trim(std::string_view(line).substr(equals + 1));
        const std::string qualified = section.empty() ? key : (section + "." + key);

        if (qualified == "enabled") {
            if (!ParseBool(value, parsed.enabled)) {
                error = "enabled must be true or false";
                return false;
            }
        } else if (qualified == "skills.ids") {
            if (!ParseIntArray(value, parsed.skillIds)) {
                error = "skills.ids must be a list of skill ids, for example [ 249, 1305 ]";
                return false;
            }
        } else if (qualified == "sequence.restore_action_flag") {
            if (!ParseBool(value, parsed.restoreActionFlag)) {
                error = "sequence.restore_action_flag must be true or false";
                return false;
            }
        } else if (qualified == "aura_refresh.keep_meteor_timer") {
            if (!ParseBool(value, parsed.keepMeteorTimer)) {
                error = "aura_refresh.keep_meteor_timer must be true or false";
                return false;
            }
        } else if (qualified == "diagnostics.enabled") {
            if (!ParseBool(value, parsed.diagnostics)) {
                error = "diagnostics.enabled must be true or false";
                return false;
            }
        } else {
            if (!unknown.empty()) unknown += ", ";
            unknown += qualified;
        }
    }

    result = std::move(parsed);
    return true;
}

// ---------------------------------------------------------------------------
//  Plugin state
// ---------------------------------------------------------------------------

const D2RL::PluginContext* Context{};
std::uint8_t*              Base{};
Config                     Settings{};
char                       ConfigSource[32]{"built-in defaults"};
char                       RuntimeBuild[64]{"unknown"};
std::atomic_bool           Operational{};

GetSkillListFn         GetSkillList{};
GetSkillIdFn           GetSkillId{};
GetHighestLevelSkillFn GetHighestLevelSkill{};
SetSkillActionTakenFn  SetSkillActionTaken{};
EventDeleteFn          EventDelete{};
ServerSkillFn          OriginalSrvDo124{};

// Copied out of Settings.skillIds at load so the hot path never touches a
// container that could be reallocated from another thread.
std::array<std::int32_t, MaxHandledSkills> HandledSkills{};
std::size_t                                HandledSkillCount{};

std::atomic<std::uint64_t> RealCastCalls{};
std::atomic<std::uint64_t> Jumpstarts{};
std::atomic<std::uint64_t> SkillNotGranted{};
std::atomic<std::uint64_t> SequenceFlagRestores{};
std::atomic<std::uint64_t> MeteorTimersKept{};
std::atomic<std::uint32_t> DiagnosticsBudget{};

void LogInfo(const char* message) noexcept {
    if (Context) Context->LogInfo(message);
}

void LogWarn(const char* message) noexcept {
    if (Context) Context->LogWarn(message);
}

void LogError(const char* message) noexcept {
    if (Context) Context->LogError(message);
}

// ---------------------------------------------------------------------------
//  Hot path
// ---------------------------------------------------------------------------

bool IsSkillHandled(std::int32_t skillId) noexcept {
    if (HandledSkillCount == 0) return true;  // empty list = every 124 skill
    for (std::size_t index = 0; index < HandledSkillCount; ++index) {
        if (HandledSkills[index] == skillId) return true;
    }
    return false;
}

bool HasSkillActionFlag(const void* unit) noexcept {
    const auto* flags = reinterpret_cast<const std::uint32_t*>(
        static_cast<const std::uint8_t*>(unit) + UnitActionFlagsOffset);
    return (*flags & SkillActionTakenBit) != 0;
}

void ReportJumpstart(std::int32_t skillId, std::int32_t skillLevel) noexcept {
    if (!Settings.diagnostics) return;
    if (DiagnosticsBudget.load(std::memory_order_relaxed) == 0) return;
    if (DiagnosticsBudget.fetch_sub(1, std::memory_order_relaxed) == 0) return;

    char message[192]{};
    std::snprintf(
        message,
        sizeof(message),
        "ArmageddonAura: jumpstarted skill %d at level %d from a borrowed cast context.",
        skillId,
        skillLevel);
    LogInfo(message);
}

void ReportTimerKept(std::int32_t skillId, std::int32_t skillLevel) noexcept {
    if (!Settings.diagnostics) return;
    if (DiagnosticsBudget.load(std::memory_order_relaxed) == 0) return;
    if (DiagnosticsBudget.fetch_sub(1, std::memory_order_relaxed) == 0) return;

    char message[192]{};
    std::snprintf(
        message,
        sizeof(message),
        "ArmageddonAura: aura refresh of skill %d at level %d kept the pending meteor round.",
        skillId,
        skillLevel);
    LogInfo(message);
}

// ---------------------------------------------------------------------------
//  Meteor timer kept across aura refreshes
// ---------------------------------------------------------------------------

std::uint8_t* FirstUnitEvent(const void* unit) noexcept {
    std::uint8_t* event = nullptr;
    std::memcpy(&event, static_cast<const std::uint8_t*>(unit) + UnitEventListOffset, sizeof(event));
    return event;
}

std::uint8_t* NextUnitEvent(const std::uint8_t* event) noexcept {
    std::uint8_t* next = nullptr;
    std::memcpy(&next, event + EventUnitNextOffset, sizeof(next));
    return next;
}

std::uint32_t ReadEventU32(const std::uint8_t* event, std::size_t offset) noexcept {
    std::uint32_t value = 0;
    std::memcpy(&value, event + offset, sizeof(value));
    return value;
}

std::uint16_t ReadEventFlags(const std::uint8_t* event) noexcept {
    std::uint16_t value = 0;
    std::memcpy(&value, event + EventFlagsOffset, sizeof(value));
    return value;
}

bool IsSkillEvent(const std::uint8_t* event, std::uint8_t type, std::int32_t skillId) noexcept {
    return event[EventTypeOffset] == type
        && ReadEventU32(event, EventCustomIdOffset) == static_cast<std::uint32_t>(skillId);
}

// Hides every pending meteor round of this skill from the delete at the end
// of 124. Running and already deleted events are left alone. Returns how many
// were hidden; 0 means there is nothing to keep and 124 bootstraps normally.
std::size_t HidePendingMeteorRounds(void* unit, std::int32_t skillId) noexcept {
    std::size_t   hidden = 0;
    std::uint8_t* event  = FirstUnitEvent(unit);
    for (std::size_t walked = 0; event != nullptr && walked < MaximumUnitEvents; ++walked) {
        if (IsSkillEvent(event, ActiveStateEventType, skillId)
            && (ReadEventFlags(event) & (EventRunningFlag | EventDeleteDeferredFlag)) == 0) {
            event[EventTypeOffset] = HiddenEventType;
            ++hidden;
        }
        event = NextUnitEvent(event);
    }
    return hidden;
}

// After 124: the round it just scheduled is the only visible one, so the
// engine's own delete removes exactly that one (and unlinks it from its frame
// bucket). The hidden rounds get its level and cast id and are shown again.
void KeepPendingMeteorRounds(void* game, void* unit, std::int32_t skillId) noexcept {
    bool          scheduled = false;
    std::uint32_t level     = 0;
    std::uint32_t castId    = 0;

    std::uint8_t* event = FirstUnitEvent(unit);
    for (std::size_t walked = 0; event != nullptr && walked < MaximumUnitEvents; ++walked) {
        if (IsSkillEvent(event, ActiveStateEventType, skillId)
            && (ReadEventFlags(event) & EventDeleteDeferredFlag) == 0) {
            scheduled = true;
            level     = ReadEventU32(event, EventCustomParamOffset);
            castId    = ReadEventU32(event, EventCastIdOffset);
            break;
        }
        event = NextUnitEvent(event);
    }

    if (scheduled) {
        EventDelete(game, unit, ActiveStateEventType, skillId);
    }

    event = FirstUnitEvent(unit);
    for (std::size_t walked = 0; event != nullptr && walked < MaximumUnitEvents; ++walked) {
        if (IsSkillEvent(event, HiddenEventType, skillId)) {
            if (scheduled) {
                std::memcpy(event + EventCustomParamOffset, &level, sizeof(level));
                std::memcpy(event + EventCastIdOffset, &castId, sizeof(castId));
            }
            event[EventTypeOffset] = ActiveStateEventType;
        }
        event = NextUnitEvent(event);
    }
}

std::int32_t __fastcall HookSrvDo124(
        void*        game,
        void*        unit,
        std::int32_t skillId,
        std::int32_t skillLevel) noexcept {
    const ServerSkillFn original = OriginalSrvDo124;
    if (original == nullptr) return 0;

    if (!Operational.load(std::memory_order_acquire) || unit == nullptr) {
        return original(game, unit, skillId, skillLevel);
    }
    if (!IsSkillHandled(skillId)) {
        return original(game, unit, skillId, skillLevel);
    }

    auto** skillList = static_cast<void**>(GetSkillList(unit));
    if (skillList == nullptr) {
        return original(game, unit, skillId, skillLevel);
    }

    void* const used = skillList[SkillListUsedIndex];
    if (used != nullptr && GetSkillId(used) == skillId) {
        // Genuine cast of this skill. The stock gate passes on its own, so
        // leave everything, including the sequence flag, exactly as it is.
        RealCastCalls.fetch_add(1, std::memory_order_relaxed);
        return original(game, unit, skillId, skillLevel);
    }

    // Same resolver and same selection flag that srvdofunc 146 uses, so the
    // node we lend is the node 146 later reads the periodic seed back from.
    void* const node = GetHighestLevelSkill(unit, skillId, false);
    if (node == nullptr) {
        // The unit does not have the skill. Almost always a missing oskill
        // grant. Nothing to lend, so run stock and leave the game alone.
        SkillNotGranted.fetch_add(1, std::memory_order_relaxed);
        return original(game, unit, skillId, skillLevel);
    }

    const bool actionFlagWasSet = HasSkillActionFlag(unit);

    // An aura refresh must not restart the meteor timer. Skill id 0 is never
    // hidden: the engine's delete treats customId 0 as "every event".
    const std::size_t hidden =
        (Settings.keepMeteorTimer && skillId > 0) ? HidePendingMeteorRounds(unit, skillId) : 0;

    skillList[SkillListUsedIndex] = node;
    const std::int32_t result     = original(game, unit, skillId, skillLevel);
    skillList[SkillListUsedIndex] = used;

    if (hidden != 0) {
        KeepPendingMeteorRounds(game, unit, skillId);
        MeteorTimersKept.fetch_add(1, std::memory_order_relaxed);
        ReportTimerKept(skillId, skillLevel);
    }

    // Stock 124 sets the sequence-suppress bit once it is past its gate. Put
    // it back the way we found it so an aura tick cannot stall a leap or a
    // whirlwind. If it was already set because the player really did act this
    // frame, it stays set.
    if (Settings.restoreActionFlag && !actionFlagWasSet && HasSkillActionFlag(unit)) {
        SetSkillActionTaken(unit, 0);
        SequenceFlagRestores.fetch_add(1, std::memory_order_relaxed);
    }

    Jumpstarts.fetch_add(1, std::memory_order_relaxed);
    ReportJumpstart(skillId, skillLevel);
    return result;
}

// ---------------------------------------------------------------------------
//  Load-time validation
// ---------------------------------------------------------------------------

struct NativeSite {
    std::uint64_t       rva;
    const std::uint8_t* expected;
    std::uint32_t       expectedSize;
    const char*         name;
};

bool ValidateNativeImage() noexcept {
    const NativeSite sites[] = {
        {SrvDo124Rva, SrvDo124Expected.data(),
         static_cast<std::uint32_t>(SrvDo124Expected.size()), "srvdofunc 124 handler"},
        {GetSkillListRva, GetSkillListExpected.data(),
         static_cast<std::uint32_t>(GetSkillListExpected.size()), "UNITS_GetSkillList"},
        {GetSkillIdRva, GetSkillIdExpected.data(),
         static_cast<std::uint32_t>(GetSkillIdExpected.size()), "SKILLS_GetSkillId"},
        {GetHighestLevelSkillRva, GetHighestLevelSkillExpected.data(),
         static_cast<std::uint32_t>(GetHighestLevelSkillExpected.size()),
         "SKILLS_GetHighestLevelSkillFromUnitAndId"},
        {SetSkillActionTakenRva, SetSkillActionTakenExpected.data(),
         static_cast<std::uint32_t>(SetSkillActionTakenExpected.size()),
         "UNITS_SetSkillActionTaken"},
    };

    const NativeSite timerSites[] = {
        {SrvDo124TimerTailRva, SrvDo124TimerTailExpected.data(),
         static_cast<std::uint32_t>(SrvDo124TimerTailExpected.size()), "srvdofunc 124 meteor timer tail"},
        {EventDeleteWalkRva, EventDeleteWalkExpected.data(),
         static_cast<std::uint32_t>(EventDeleteWalkExpected.size()), "EVENT_Delete node walk"},
        {GetUnitEventListRva, GetUnitEventListExpected.data(),
         static_cast<std::uint32_t>(GetUnitEventListExpected.size()), "UNITS_GetEventList"},
        {SetEventStoreRva, SetEventStoreExpected.data(),
         static_cast<std::uint32_t>(SetEventStoreExpected.size()), "EVENT_SetEvent node stores"},
    };

    for (const auto& site : timerSites) {
        if (!Settings.keepMeteorTimer) break;
        if (!Context->CheckExpectedBytes(site.rva, site.expected, site.expectedSize)) {
            char message[256]{};
            std::snprintf(
                message,
                sizeof(message),
                "ArmageddonAura: native fingerprint mismatch at 0x%llX (%s), needed by "
                "aura_refresh.keep_meteor_timer. Refusing to load.",
                static_cast<unsigned long long>(site.rva),
                site.name);
            LogError(message);
            return false;
        }
    }

    for (const auto& site : sites) {
        if (!Context->CheckExpectedBytes(site.rva, site.expected, site.expectedSize)) {
            char message[256]{};
            std::snprintf(
                message,
                sizeof(message),
                "ArmageddonAura: native fingerprint mismatch at 0x%llX (%s). Refusing to load.",
                static_cast<unsigned long long>(site.rva),
                site.name);
            LogError(message);
            return false;
        }
    }

    // Every fingerprint above already proved this is the expected image, so
    // reading the dispatch table is safe by this point. Slot 124 must resolve
    // to the function we are about to hook, otherwise skills.txt srvdofunc 124
    // dispatches somewhere else and hooking it would achieve nothing.
    auto* const  table = reinterpret_cast<void* const*>(Base + SrvDoTableRva);
    void* const  slot  = table[SrvDo124TableIdx];
    void* const  want  = reinterpret_cast<void*>(Base + SrvDo124Rva);
    if (slot != want) {
        char message[256]{};
        std::snprintf(
            message,
            sizeof(message),
            "ArmageddonAura: srvdofunc table slot 124 resolves to 0x%llX, expected 0x%llX. Refusing to load.",
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(slot)),
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(want)));
        LogError(message);
        return false;
    }

    return true;
}

void ResolveNativeFunctions() noexcept {
    GetSkillList = reinterpret_cast<GetSkillListFn>(Base + GetSkillListRva);
    GetSkillId   = reinterpret_cast<GetSkillIdFn>(Base + GetSkillIdRva);
    GetHighestLevelSkill =
        reinterpret_cast<GetHighestLevelSkillFn>(Base + GetHighestLevelSkillRva);
    SetSkillActionTaken =
        reinterpret_cast<SetSkillActionTakenFn>(Base + SetSkillActionTakenRva);
    EventDelete = reinterpret_cast<EventDeleteFn>(Base + EventDeleteRva);
}

bool LoadConfig() noexcept {
    if (!Context->EnsureConfig(DefaultConfigToml)) {
        LogWarn("ArmageddonAura: config file could not be created, using built-in defaults.");
        return true;
    }

    try {
        std::vector<char> buffer(32768, '\0');
        std::uint32_t     required = 0;

        if (!Context->ReadConfig(
                buffer.data(), static_cast<std::uint32_t>(buffer.size()), &required)) {
            if (required == 0 || required <= buffer.size()) {
                LogWarn("ArmageddonAura: config file could not be read, using built-in defaults.");
                return true;
            }
            buffer.assign(static_cast<std::size_t>(required) + 1, '\0');
            if (!Context->ReadConfig(
                    buffer.data(), static_cast<std::uint32_t>(buffer.size()), nullptr)) {
                LogWarn("ArmageddonAura: config file could not be read, using built-in defaults.");
                return true;
            }
        }
        buffer.back() = '\0';

        Config      parsed{};
        std::string error;
        std::string unknown;
        if (!ParseConfig(std::string_view(buffer.data()), parsed, error, unknown)) {
            char message[320]{};
            std::snprintf(
                message,
                sizeof(message),
                "ArmageddonAura: config rejected (%s). Refusing to load so the setting you meant is not silently ignored.",
                error.c_str());
            LogError(message);
            return false;
        }

        if (!unknown.empty()) {
            char message[320]{};
            std::snprintf(
                message,
                sizeof(message),
                "ArmageddonAura: ignoring unknown config setting(s): %s.",
                unknown.c_str());
            LogWarn(message);
        }

        Settings = std::move(parsed);
        std::snprintf(ConfigSource, sizeof(ConfigSource), "%s", "config file");

        HandledSkillCount = 0;
        for (const std::int32_t id : Settings.skillIds) {
            if (HandledSkillCount >= MaxHandledSkills) {
                LogWarn("ArmageddonAura: skills.ids holds more than 64 entries, the extras are ignored.");
                break;
            }
            HandledSkills[HandledSkillCount++] = id;
        }
        return true;
    } catch (...) {
        LogWarn("ArmageddonAura: config could not be processed, using built-in defaults.");
        return true;
    }
}

// NOTE: there is deliberately no process-wide or session-wide lock here.
//
// An earlier revision took a named mutex in the Local\ namespace to refuse a
// second copy of this plugin. That namespace is per session, not per process,
// so any D2R process that had not finished exiting still owned the name and
// the next launch refused itself for no reason. The observed symptom was the
// Extensions tab showing the plugin as errored on one launch and loading
// cleanly on the next, with nothing wrong with the game image at all.
//
// Nothing is lost by removing it. D2RLoader already refuses to load two
// plugins with the same id, including across the global and mod scopes, and
// InstallInlineHook below refuses a second hook on the same function, which
// this plugin already reports with a clear message. Those are per-process
// guards on the thing that actually matters, which is the hook.

auto StatusCommand(
        D2R::Game::Client*,
        const D2RL::ConsoleCommandContext* command,
        void*) noexcept -> D2RL::ConsoleCommandResult {
    if (command == nullptr || command->plugin == nullptr) {
        return D2RL::ConsoleCommandResult::Failed;
    }

    char scope[192]{};
    if (HandledSkillCount == 0) {
        std::snprintf(scope, sizeof(scope), "every srvdofunc 124 skill");
    } else {
        int written = std::snprintf(scope, sizeof(scope), "skill ids");
        for (std::size_t index = 0; index < HandledSkillCount && written > 0
                                    && written < static_cast<int>(sizeof(scope)); ++index) {
            written += std::snprintf(
                scope + written,
                sizeof(scope) - static_cast<std::size_t>(written),
                " %d",
                HandledSkills[index]);
        }
    }

    char message[768]{};
    std::snprintf(
        message,
        sizeof(message),
        "Armageddon as Aura %s: %s; scope=%s; jumpstarts=%llu; real casts passed through=%llu; "
        "skill not granted=%llu; sequence flag restores=%llu; meteor rounds kept=%llu; "
        "restore_action_flag=%s; keep_meteor_timer=%s; config=%s; build=%s.",
        PluginVersion,
        Operational.load(std::memory_order_acquire) ? "active" : "inactive",
        scope,
        static_cast<unsigned long long>(Jumpstarts.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(RealCastCalls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(SkillNotGranted.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(SequenceFlagRestores.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(MeteorTimersKept.load(std::memory_order_relaxed)),
        Settings.restoreActionFlag ? "true" : "false",
        Settings.keepMeteorTimer ? "true" : "false",
        ConfigSource,
        RuntimeBuild);

    command->plugin->WriteConsoleMessage(message);
    return D2RL::ConsoleCommandResult::Handled;
}

void ResetState() noexcept {
    Operational.store(false, std::memory_order_release);
    Settings.enabled           = true;
    Settings.restoreActionFlag = true;
    Settings.keepMeteorTimer   = true;
    Settings.diagnostics       = false;
    Settings.skillIds.clear();
    std::snprintf(ConfigSource, sizeof(ConfigSource), "%s", "built-in defaults");
    std::snprintf(RuntimeBuild, sizeof(RuntimeBuild), "%s", "unknown");
    HandledSkillCount = 0;
    HandledSkills.fill(0);
    RealCastCalls.store(0, std::memory_order_relaxed);
    Jumpstarts.store(0, std::memory_order_relaxed);
    SkillNotGranted.store(0, std::memory_order_relaxed);
    SequenceFlagRestores.store(0, std::memory_order_relaxed);
    MeteorTimersKept.store(0, std::memory_order_relaxed);
    DiagnosticsBudget.store(8, std::memory_order_relaxed);
}

}  // namespace
}  // namespace CelestialRayOne::ArmageddonAura

namespace {

constexpr D2RL::PluginInfo Info{
    .infoSize    = D2RL::PluginInfoSize,
    .abiVersion  = D2RL_PLUGIN_ABI_VERSION,
    .id          = "celestialrayone.armageddon-aura",
    .name        = "Armageddon as Aura",
    .version     = "1.1.0",
    .author      = "CelestialRayOne",
    .description = "Lets srvdofunc 124 skills bootstrap and persist as auras.",
    .flags       = D2RL::PluginFlags::Server | D2RL::PluginFlags::NativeHooks,
};

}  // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
    return &Info;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
    using namespace CelestialRayOne::ArmageddonAura;

    Context = context;
    Base    = context != nullptr ? reinterpret_cast<std::uint8_t*>(context->exeBase) : nullptr;
    ResetState();

    if (Context == nullptr || Base == nullptr) return false;

    if (context->buildName != nullptr) {
        std::snprintf(RuntimeBuild, sizeof(RuntimeBuild), "%s", context->buildName);
    }

    if (!LoadConfig()) return false;

    if (!Settings.enabled) {
        LogInfo("Armageddon as Aura 1.1.0 loaded disabled by config; the game runs stock.");
        return true;
    }

    if (!ValidateNativeImage()) return false;

    ResolveNativeFunctions();

    if (!Context->InstallInlineHook(
            SrvDo124Rva,
            SrvDo124Expected.data(),
            static_cast<std::uint32_t>(SrvDo124Expected.size()),
            &HookSrvDo124,
            &OriginalSrvDo124)
        || OriginalSrvDo124 == nullptr) {
        LogError("ArmageddonAura: the srvdofunc 124 handler is already hooked by another plugin, or the hook was refused.");
        return false;
    }

    Operational.store(true, std::memory_order_release);

    if (!Context->RegisterConsoleCommand(
            "armageddon-aura",
            StatusCommand,
            "Show Armageddon as Aura status and counters.")) {
        LogWarn("ArmageddonAura: status console command could not be registered.");
    }

    {
        char message[320]{};
        std::snprintf(
            message,
            sizeof(message),
            "Armageddon as Aura 1.1.0 by CelestialRayOne active; scope=%s; config=%s; build=%s.",
            HandledSkillCount == 0 ? "every srvdofunc 124 skill" : "explicit skill id list",
            ConfigSource,
            RuntimeBuild);
        LogInfo(message);
    }

    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    using namespace CelestialRayOne::ArmageddonAura;
    Operational.store(false, std::memory_order_release);
}
