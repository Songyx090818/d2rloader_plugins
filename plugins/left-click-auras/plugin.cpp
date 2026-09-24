// =============================================================================
//  Left Click Auras  -  celestialrayone.left-click-auras
//
//  D2R 3.3 under D2RLoader 1.3.1. An aura on the LEFT mouse button runs like an
//  aura on the right button, and both run at the same time. Port of ESR's 2.4
//  left click aura patch set:
//
//    1. LEFT SLOT AURAS. Putting an aura on the left button starts it; taking
//       it off, by any route, stops it and removes its state and bonuses.
//    2. SINGLE EMANATION. The same aura on both buttons emanates once.
//    3. MOVE ONLY CLICKS. A left click on a monster with an aura on the left
//       button moves toward it instead of doing nothing.
//
//  Every address below was read out of the dumped D2RLoader 1.3.0 image, re-checked
//  against the 1.3.1 image (only the SetUnitCastId thunk calls moved), and
//  disassembled before being relied on. Byte witnesses guard every function
//  the plugin changes, so a different build refuses cleanly.
//
// -----------------------------------------------------------------------------
//  HOW THE GAME RUNS AN AURA (server)
// -----------------------------------------------------------------------------
//  0x438A70  AssignSkill(unit, side, skillId, ownerGuid); side != 0 is the left
//            button, side 0 the right one. rbp = unit, r15d = side, r14d = skill.
//      left   0x438AEF  SetLeftActiveSkill, then 0x438B96 test r15d,r15d /
//                       jnz 0x438C85 skips every aura step.
//      right  0x438AF6  old right node into r12, SetRightActiveSkill
//             0x438B9F  old right aura, any aura on the left ignored:
//                       0x438C02 GetStateStatList -> unlink, free
//                       0x438C33 ToggleState(unit, state, 0)
//                       0x438C52 delete (8, -1)
//             0x438C80  new right aura: AuraStart(game, unit, skillId)
//
//  0x43B680  AuraStart: skills.txt record, aura flag byte[rec+24h] & [1D996E4h],
//            aurastate word[rec+A0h] inside the state table, real level, new
//            cast id. State stat list (reused or allocated) gets stat 350 =
//            skill, 351 = level, 202 = cast id, state on. Then
//            0x43B8FD ArmPeriodicSkill(game, unit, skill, level, castId, 1).
//
//  0x43B2B0  ArmPeriodicSkill: event type 8 with custom id -1 when the last
//            argument is 1 (the right button's timer), else the skill id (a
//            timer of its own, level in customParam, cast id in arg7). It
//            deletes that (8, id) event before setting it, so arming again
//            never stacks timers.
//
//  0x437460  the type 8 event(game, unit, customId, customParam, castId)
//      id -1  reads the RIGHT button only (0x437499), performs at 0x43757E.
//      id > 0 revalidates: state live, state stat list, unit still has the
//             skill, stat 350 == skill. Performs at 0x4377BC
//             (ServerDoSkill: rcx=rsi game, rdx=rdi unit, r8d=ebp skill,
//             r9d=r12d level, then 1, 0, 0; eax unused afterwards), re-arms at
//             0x4377E7. Any failure calls 0x432450.
//
//  0x432450  failure({game, unit}, skill, record): deletes (8, skill). It
//            removes the state only for skills with byte[rec+29h] &
//            [1D996E8h], which auras do not have, so a failed aura leaves its
//            state and bonuses on the unit.
//
// -----------------------------------------------------------------------------
//  THE FIX (server)
// -----------------------------------------------------------------------------
//  0x438AEF  call relay on the left branch's SetLeftActiveSkill (rcx unit,
//            edx skill, r8d owner guid; eax unused afterwards). The stock code
//            only gets there after 0x33DCD0 found the skill. Left skill before
//            and after the stock select:
//              - the old left aura is not the new left skill: remove its state
//                stat list and state (kept when the right button's aura uses
//                the same state and its timer runs), delete (8, old skill).
//              - the new left skill is an aura: nothing when it is the same
//                aura with a live (8, skill) timer and its state on; otherwise
//                AuraStart runs with its final arm switched to the skill's own
//                timer (next item).
//  0x43B8FD  call relay. Only for the AuraStart the relay above runs: the last
//            argument becomes 0, and the (unit, skill, cast id) of the timer is
//            remembered. Every other AuraStart runs untouched.
//  0x438C02,
//  0x438C33  call relays inside the right button's aura removal. When the left
//            button holds a running aura with that same state, the state stat
//            list is reported missing and the state stays on. The right timer
//            is still deleted.
//  0x437460  inline hook. A remembered left timer (unit, skill, cast id) whose
//            skill is no longer on the left button fails exactly like a failed
//            revalidation: 0x432450 runs for it. Nothing else is changed, so
//            timers the game creates itself (monsters, used skills) run stock.
//  0x432450  inline hook. For a remembered left timer: the state stat list and
//            state are removed (kept when the right button's running aura uses
//            the same state), then the stock delete runs.
//  0x4377BC  call relay (single_emanation). A remembered left timer whose skill
//            is also on the right button, with the right timer running, skips
//            the perform and keeps re-arming.
//
// -----------------------------------------------------------------------------
//  MOVE ONLY CLICKS (client)
// -----------------------------------------------------------------------------
//  0x102490  world click handler(command). rdi = command; flags dword at +0
//            (1 left button, 2 right, 4 press, 20h shift), player +8, target
//            +10h, skill node +28h (r14 = command+28h).
//  0x102753  call 0x33F770(player, node), which returns dword [node+10h]:
//            nonzero sends the click into the skill branch. There 0x100970
//            asks the usability gate 0x100760 -> 0x21B170 -> classifier
//            0x33F360, which returns 6 for an aura (byte[rec+24h] &
//            [1D996E4h]); the gate lets only 0 and 5 through, so nothing
//            happens. Zero leaves the skill branch: a monster press is sent as
//            a move toward it (0x0FABE0 with the command's unit opcode), NPCs
//            and every other unit type go to 0x101910 (talk, interact, pick
//            up, use, with a move toward the target when out of range).
//            Call relay: lea r8,[r14-28h] hands the command over; a left button
//            click whose node is the player's left skill holding an aura
//            returns 0. Everything else calls 0x33F770.
//
//  Console command "leftclickauras" shows what is installed and counters.
//  Settings: d2rloader/config/celestialrayone.left-click-auras.toml
// =============================================================================

#include <D2RLPlugin/api.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace CelestialRayOne::LeftClickAuras {
namespace {

constexpr char PluginVersion[] = "1.0.0";

// ---------------------------------------------------------------------------
//  Default configuration. EnsureConfig writes this text on first load, and it
//  is the plugin's documentation as shipped.
// ---------------------------------------------------------------------------
constexpr const char DefaultConfigToml[] = R"TOML(# celestialrayone.left-click-auras
#
# Left Click Auras
#   An aura on the left mouse button runs the same way an aura on the right
#   mouse button does, and both can be active at the same time.
#
# Changes are read when the plugin loads. Restart the game after editing.
# Built for D2RLoader 1.3.1 (Diablo II: Resurrected 3.3). On any other build
# the byte checks fail and the plugin loads without changing the game.
#
# Data requirement
#   The game only lets a skill onto the left button when its skills.txt row
#   has leftskill = 1. Aura rows that should work on the left need it.
#
# Compatibility
#   Do not install together with yin's "Aura In Either Skill Slot (Single
#   Active Aura)" patch. It changes the same game code for a different design
#   (one aura at a time), and the byte checks of whichever loads second fail.

# Master switch. false loads the plugin without touching the game.
enabled = true

# -----------------------------------------------------------------------------
# left_slot_auras
#
# The problem
#   The game starts an aura only when it goes on the right mouse button. The
#   left button only records the selection, so an aura there never runs.
#
# What changes
#   Putting an aura on the left button starts it the way the right button
#   does: aura state, real skill level, a timer of its own. The left and right
#   auras keep separate timers, so two auras run side by side.
#
#   The left aura stops, and its state and bonuses are removed, when
#     - another skill goes on the left button,
#     - the aura is no longer on the left button for any other reason (an item
#       or weapon swap took the skill away, the game reset the button),
#     - its timer finds the aura invalid (the skill is gone, its state was
#       taken over).
#
#   Selecting the same aura again keeps it running. After a resurrect, a
#   weapon swap back or entering a game, the game selects the left skill again
#   and the aura starts if it is not running.
#
#   When the left and right auras share an aura state, stopping one of them
#   leaves the state to the other.
#
# Where
#   skill assignment     call at 0x438AEF, where 0x438A70 selects the left
#                        skill
#   aura start           call at 0x43B8FD, arms the left aura's own timer
#   right aura removal   calls at 0x438C02 and 0x438C33, keep a state the left
#                        aura still uses
#   aura timer           0x437460, stops a left aura that left the button
#   aura timer failure   0x432450, removes the state and bonuses
left_slot_auras = true

# -----------------------------------------------------------------------------
# single_emanation
#
# The problem
#   With the same aura on both buttons, both timers perform it and it
#   emanates twice as often.
#
# What changes
#   While the right button holds the same aura and its timer runs, the left
#   timer keeps itself alive without performing, so the aura emanates once.
#   If the right button changes, the left timer performs again on its own.
#
# Where
#   call at 0x4377BC in the aura timer
#
# Needs left_slot_auras.
single_emanation = true

# -----------------------------------------------------------------------------
# move_only_clicks
#
# The problem
#   With an aura on the left button, left clicking a monster does nothing. The
#   click is treated as a cast of the aura on that monster, and auras cannot
#   be cast.
#
# What changes
#   A left click on a monster walks or runs toward it, the same way a click on
#   the ground moves you. Clicks on anything else (NPCs, other players, items,
#   objects, exits) do what they do with no usable skill on the button: talk,
#   interact, pick up, use. Shift clicks and the right button are unchanged.
#
# Where
#   call at 0x102753 in the client's world click handler (0x102490)
move_only_clicks = true

# Writes a line to the plugin log for the first 64 aura starts, stops and
# skipped emanations. Keep false for normal play. The console command
# "leftclickauras" always shows what is installed and the counters.
diagnostics = false
)TOML";

// ---------------------------------------------------------------------------
//  Native layout (D2R 3.3, D2RLoader 1.3.1 process image)
// ---------------------------------------------------------------------------
constexpr std::size_t   GameDataContextOffset        = 0x106;  // movzx ecx,byte [rbp+106h] at 0x43B8C2
constexpr std::size_t   UnitIdOffset                 = 0x08;
constexpr std::size_t   SkillNodeRecordOffset        = 0x00;   // node -> skills.txt record (0x3402D0, 0x102674)
constexpr std::size_t   SkillRecordIdOffset          = 0x00;   // int16 skill id
constexpr std::size_t   SkillRecordFlagsByteOffset   = 0x24;   // & aura mask, 0x438BD4
constexpr std::size_t   SkillRecordCleanupByteOffset = 0x29;   // & cleanup mask, 0x432470
constexpr std::size_t   SkillRecordAuraStateOffset   = 0xA0;   // int16, 0x438BE0
constexpr std::size_t   ClickCommandFlagsOffset      = 0x00;
constexpr std::uint32_t ClickCommandLeftButton       = 0x01;
constexpr std::int32_t  UnitTypePlayer               = 0;
constexpr std::uint8_t  PeriodicSkillEventType       = 8;
constexpr std::uint32_t RightTimerCustomId           = 0xFFFFFFFFU;
constexpr std::size_t   UnitEventListOffset          = 0x148;  // 0x48FE50
constexpr std::size_t   EventTypeOffset              = 0x00;
constexpr std::size_t   EventFlagsOffset             = 0x02;
constexpr std::size_t   EventCustomIdOffset          = 0x18;
constexpr std::size_t   EventUnitNextOffset          = 0x38;
constexpr std::uint16_t EventDeleteDeferredFlag      = 0x0008;
constexpr std::size_t   MaximumUnitEvents            = 65536;

// ---------------------------------------------------------------------------
//  Native RVAs
// ---------------------------------------------------------------------------
constexpr std::uint64_t AssignSkillRva               = 0x438A70;
constexpr std::uint64_t LeftSelectCallRva            = 0x438AEF;
constexpr std::uint64_t SetLeftActiveSkillRva        = 0x33EC70;
constexpr std::uint64_t RightAuraStatListCallRva     = 0x438C02;
constexpr std::uint64_t RightAuraStateOffCallRva     = 0x438C33;
constexpr std::uint64_t PeriodicSkillEventRva        = 0x437460;
constexpr std::uint64_t LeftTimerPerformWindowRva    = 0x437761;
constexpr std::uint64_t LeftTimerPerformCallRva      = 0x4377BC;
constexpr std::uint64_t RevalidationFailureRva       = 0x432450;
constexpr std::uint64_t AuraStartRva                 = 0x43B680;
constexpr std::uint64_t AuraStartArmWindowRva        = 0x43B8C2;
constexpr std::uint64_t AuraStartArmCallRva          = 0x43B8FD;
constexpr std::uint64_t ArmPeriodicSkillRva          = 0x43B2B0;
constexpr std::uint64_t ServerDoSkillRva             = 0x43ACB0;
constexpr std::uint64_t ClickHandlerRva              = 0x102490;
constexpr std::uint64_t ClickGateWindowRva           = 0x102700;
constexpr std::uint64_t ClickGateCallRva             = 0x102753;
constexpr std::uint64_t SkillNodeGateRva             = 0x33F770;
constexpr std::uint64_t GetLeftSkillRva              = 0x34A540;
constexpr std::uint64_t GetRightSkillRva             = 0x34B400;
constexpr std::uint64_t GetGameRva                   = 0x48FF00;
constexpr std::uint64_t CheckStateRva                = 0x3351B0;
constexpr std::uint64_t DeleteEventsByTypeRva        = 0x48B890;
constexpr std::uint64_t DeleteEventsWalkRva          = 0x48B8F0;
constexpr std::uint64_t GetUnitEventListRva          = 0x48FE50;
constexpr std::uint64_t SetEventStoreRva             = 0x48BF55;
constexpr std::uint64_t GetSkillsTxtRecordRva        = 0x097790;
constexpr std::uint64_t GetStateCountRva             = 0x2141A0;
constexpr std::uint64_t ToggleStateRva               = 0x3354C0;
constexpr std::uint64_t GetStateStatListRva          = 0x2F5940;
constexpr std::uint64_t UnlinkStatListRva            = 0x2F7920;
constexpr std::uint64_t FreeStatListRva              = 0x2F4180;
constexpr std::uint64_t GetUnitTypeRva               = 0x34B9D0;
constexpr std::uint64_t AuraFlagMaskRva              = 0x1D996E4;  // test [rip+..],cl at 0x438BD8
constexpr std::uint64_t CleanupFlagMaskRva           = 0x1D996E8;  // test [rip+..],al at 0x432475

// ---------------------------------------------------------------------------
//  Byte witnesses, read from the image
// ---------------------------------------------------------------------------
// 0x438A70..0x438CA3, AssignSkill, whole function. Its call bytes also prove
// the targets used below: 0x3351B0, 0x33EC70, 0x34B400, 0x34A0E0, 0x33E080,
// 0x097790, 0x2141A0, 0x2F5940, 0x2F7920, 0x2F4180, 0x3354C0, 0x48FF00,
// 0x48B890, 0x43B680.
constexpr auto AssignSkillBody = std::to_array<std::uint8_t>({
    0x41, 0x83, 0xF8, 0x05, 0x0F, 0x84, 0x28, 0x02, 0x00, 0x00, 0x44, 0x89,
    0x4C, 0x24, 0x20, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x40,
    0x48, 0x89, 0x5C, 0x24, 0x60, 0x45, 0x8B, 0xF0, 0x44, 0x8B, 0xFA, 0x48,
    0x8B, 0xE9, 0x85, 0xD2, 0x75, 0x1C, 0x41, 0x8D, 0x57, 0x55, 0xE8, 0x0D,
    0xC7, 0xEF, 0xFF, 0x85, 0xC0, 0x74, 0x0F, 0x45, 0x33, 0xC0, 0x41, 0x8D,
    0x57, 0x55, 0x48, 0x8B, 0xCD, 0xE8, 0x0A, 0xCA, 0xEF, 0xFF, 0x44, 0x8B,
    0x44, 0x24, 0x78, 0x41, 0x8B, 0xD6, 0x48, 0x8B, 0xCD, 0xE8, 0x0A, 0x52,
    0xF0, 0xFF, 0x48, 0x8B, 0xD8, 0x48, 0x85, 0xC0, 0x0F, 0x84, 0xC2, 0x01,
    0x00, 0x00, 0x48, 0x89, 0x74, 0x24, 0x68, 0x48, 0x8B, 0xCD, 0x4C, 0x89,
    0x64, 0x24, 0x30, 0x45, 0x33, 0xE4, 0x45, 0x85, 0xFF, 0x74, 0x0F, 0x44,
    0x8B, 0x44, 0x24, 0x78, 0x41, 0x8B, 0xD6, 0xE8, 0x7C, 0x61, 0xF0, 0xFF,
    0xEB, 0x18, 0xE8, 0x05, 0x29, 0xF1, 0xFF, 0x44, 0x8B, 0x44, 0x24, 0x78,
    0x41, 0x8B, 0xD6, 0x48, 0x8B, 0xCD, 0x4C, 0x8B, 0xE0, 0xE8, 0x02, 0x64,
    0xF0, 0xFF, 0x45, 0x33, 0xC9, 0x48, 0x89, 0x7C, 0x24, 0x38, 0x45, 0x33,
    0xC0, 0xC7, 0x44, 0x24, 0x78, 0xFF, 0xFF, 0xFF, 0xFF, 0x48, 0x8D, 0x54,
    0x24, 0x78, 0x48, 0xC7, 0x44, 0x24, 0x20, 0x00, 0x00, 0x00, 0x00, 0x48,
    0x8B, 0xCB, 0xE8, 0x69, 0x3F, 0xF0, 0xFF, 0x48, 0x8B, 0xCD, 0xE8, 0x91,
    0x2E, 0xF1, 0xFF, 0x85, 0xC0, 0x75, 0x48, 0x8B, 0x74, 0x24, 0x78, 0x48,
    0x8D, 0x15, 0x52, 0x08, 0x8E, 0x01, 0x41, 0xB8, 0x95, 0x0F, 0x00, 0x00,
    0x48, 0x8B, 0xCD, 0xE8, 0xD4, 0x17, 0xF1, 0xFF, 0x48, 0x8B, 0xCD, 0x8B,
    0xF8, 0xE8, 0x6A, 0x2E, 0xF1, 0xFF, 0x48, 0x8B, 0xCD, 0x8B, 0xD8, 0xE8,
    0x70, 0x72, 0x05, 0x00, 0x45, 0x8B, 0xCF, 0x89, 0x74, 0x24, 0x28, 0x44,
    0x8B, 0xC7, 0x66, 0x44, 0x89, 0x74, 0x24, 0x20, 0x0F, 0xB6, 0xD3, 0x48,
    0x8B, 0xC8, 0xE8, 0xF5, 0x64, 0x04, 0x00, 0x48, 0x8B, 0xCD, 0xE8, 0x4D,
    0x15, 0xF1, 0xFF, 0x0F, 0xB6, 0xF0, 0x45, 0x85, 0xFF, 0x0F, 0x85, 0xE6,
    0x00, 0x00, 0x00, 0x4D, 0x85, 0xE4, 0x0F, 0x84, 0xAF, 0x00, 0x00, 0x00,
    0x41, 0xB8, 0x9F, 0x0F, 0x00, 0x00, 0x48, 0x8D, 0x15, 0xEB, 0x07, 0x8E,
    0x01, 0x49, 0x8B, 0xCC, 0xE8, 0xC3, 0x54, 0xF0, 0xFF, 0x8B, 0xD0, 0x40,
    0x0F, 0xB6, 0xCE, 0xE8, 0xC8, 0xEB, 0xC5, 0xFF, 0x48, 0x8B, 0xD8, 0x48,
    0x85, 0xC0, 0x0F, 0x84, 0x83, 0x00, 0x00, 0x00, 0x0F, 0xB6, 0x48, 0x24,
    0x84, 0x0D, 0x06, 0x0B, 0x96, 0x01, 0x74, 0x77, 0x0F, 0xB7, 0xB8, 0xA0,
    0x00, 0x00, 0x00, 0x66, 0x85, 0xFF, 0x79, 0x10, 0x40, 0x0F, 0xB6, 0xCE,
    0x0F, 0xBF, 0xFF, 0xE8, 0xA8, 0xB5, 0xDD, 0xFF, 0x3B, 0xF8, 0x7D, 0x3C,
    0x0F, 0xBF, 0xD7, 0x48, 0x8B, 0xCD, 0xE8, 0x39, 0xCD, 0xEB, 0xFF, 0x48,
    0x8B, 0xF8, 0x48, 0x85, 0xC0, 0x74, 0x17, 0x48, 0x8B, 0xD0, 0x48, 0x8B,
    0xCD, 0xE8, 0x06, 0xED, 0xEB, 0xFF, 0x48, 0x8B, 0xD7, 0x40, 0x0F, 0xB6,
    0xCE, 0xE8, 0x5A, 0xB5, 0xEB, 0xFF, 0x0F, 0xBF, 0x93, 0xA0, 0x00, 0x00,
    0x00, 0x45, 0x33, 0xC0, 0x48, 0x8B, 0xCD, 0xE8, 0x88, 0xC8, 0xEF, 0xFF,
    0x48, 0x8B, 0xCD, 0xE8, 0xC0, 0x72, 0x05, 0x00, 0x48, 0x8B, 0xC8, 0x41,
    0xB9, 0xFF, 0xFF, 0xFF, 0xFF, 0x41, 0xB8, 0x08, 0x00, 0x00, 0x00, 0x48,
    0x8B, 0xD5, 0xE8, 0x39, 0x2C, 0x05, 0x00, 0x41, 0x8B, 0xD6, 0x40, 0x0F,
    0xB6, 0xCE, 0xE8, 0x2D, 0xEB, 0xC5, 0xFF, 0x0F, 0xB6, 0x48, 0x24, 0x84,
    0x0D, 0x77, 0x0A, 0x96, 0x01, 0x74, 0x16, 0x48, 0x8B, 0xCD, 0xE8, 0x89,
    0x72, 0x05, 0x00, 0x48, 0x8B, 0xC8, 0x45, 0x8B, 0xC6, 0x48, 0x8B, 0xD5,
    0xE8, 0xFB, 0x29, 0x00, 0x00, 0x48, 0x8B, 0x7C, 0x24, 0x38, 0x48, 0x8B,
    0x74, 0x24, 0x68, 0x4C, 0x8B, 0x64, 0x24, 0x30, 0x48, 0x8B, 0x5C, 0x24,
    0x60, 0x48, 0x83, 0xC4, 0x40, 0x41, 0x5F, 0x41, 0x5E, 0x5D, 0xC3,
});

// 0x437460..0x4375AA, the type 8 event: entry, right button read, perform.
// The call at 0x437545 goes through the loader's SetUnitCastId thunk, 0x3E2B652
// in 1.3.1 (0x3E2B57E in 1.3.0).
constexpr auto PeriodicSkillEventWindow = std::to_array<std::uint8_t>({
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x56, 0x57,
    0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x50, 0x48, 0x89,
    0x4C, 0x24, 0x40, 0x45, 0x8B, 0xF9, 0x48, 0x89, 0x54, 0x24, 0x48, 0x41,
    0x8B, 0xE8, 0x48, 0x8B, 0xFA, 0x48, 0x8B, 0xF1, 0x41, 0x83, 0xF8, 0xFF,
    0x0F, 0x85, 0x91, 0x01, 0x00, 0x00, 0x48, 0x8B, 0xCA, 0xE8, 0x62, 0x3F,
    0xF1, 0xFF, 0x48, 0x8B, 0xD8, 0x48, 0x85, 0xC0, 0x0F, 0x84, 0x61, 0x01,
    0x00, 0x00, 0x41, 0xB8, 0x1B, 0x0E, 0x00, 0x00, 0x48, 0x8D, 0x15, 0xE9,
    0x1E, 0x8E, 0x01, 0x48, 0x8B, 0xC8, 0xE8, 0xC1, 0x6B, 0xF0, 0xFF, 0x45,
    0x33, 0xC9, 0x48, 0x8B, 0xD3, 0x48, 0x8B, 0xCF, 0x8B, 0xE8, 0x45, 0x8D,
    0x41, 0x01, 0xE8, 0x0D, 0x5D, 0xF0, 0xFF, 0x0F, 0xB6, 0x8E, 0x06, 0x01,
    0x00, 0x00, 0x8B, 0xD5, 0x44, 0x8B, 0xE0, 0xE8, 0xAC, 0x02, 0xC6, 0xFF,
    0x48, 0x8B, 0xD8, 0x48, 0x85, 0xC0, 0x0F, 0x84, 0x1B, 0x01, 0x00, 0x00,
    0x0F, 0xB6, 0x48, 0x24, 0x84, 0x0D, 0xEA, 0x21, 0x96, 0x01, 0x0F, 0x84,
    0xA0, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xCF, 0xE8, 0xB8, 0x4D, 0xF1, 0xFF,
    0x85, 0xC0, 0x0F, 0x85, 0x90, 0x00, 0x00, 0x00, 0x41, 0x8D, 0x47, 0x01,
    0xA9, 0xFE, 0xFF, 0xFF, 0xFF, 0x75, 0x1A, 0x48, 0x8D, 0x8C, 0x24, 0x90,
    0x00, 0x00, 0x00, 0xC6, 0x84, 0x24, 0x90, 0x00, 0x00, 0x00, 0x00, 0xE8,
    0xC0, 0xAE, 0xFF, 0xFF, 0x84, 0xC0, 0x74, 0x01, 0xCC, 0x48, 0x8B, 0xCF,
    0xE8, 0x83, 0x41, 0xF1, 0xFF, 0x41, 0x8B, 0xD7, 0x48, 0x8B, 0xCF, 0x8B,
    0xD8, 0xE8, 0x08, 0x41, 0x9F, 0x03, 0xBA, 0x00, 0x00, 0x00, 0x20, 0x41,
    0xB8, 0x01, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xCF, 0xE8, 0xE3, 0x6B, 0xF1,
    0xFF, 0x45, 0x33, 0xF6, 0x45, 0x8B, 0xCC, 0x44, 0x89, 0x74, 0x24, 0x30,
    0x44, 0x8B, 0xC5, 0x44, 0x89, 0x74, 0x24, 0x28, 0x48, 0x8B, 0xD7, 0x48,
    0x8B, 0xCE, 0xC7, 0x44, 0x24, 0x20, 0x01, 0x00, 0x00, 0x00, 0xE8, 0x2D,
    0x37, 0x00, 0x00, 0x45, 0x33, 0xC0, 0xBA, 0x00, 0x00, 0x00, 0x20, 0x48,
    0x8B, 0xCF, 0xE8, 0xAD, 0x6B, 0xF1, 0xFF, 0xC7, 0x44, 0x24, 0x28, 0x01,
    0x00, 0x00, 0x00, 0xE9, 0x36, 0x02, 0x00, 0x00, 0x0F, 0xB6, 0x43, 0x29,
    0x84, 0x05, 0x3E, 0x21, 0x96, 0x01,
});

// 0x437761..0x437800, skill timer perform (0x4377BC -> 0x43ACB0) and re-arm
// (0x4377E7 -> 0x43B2B0). The calls at 0x437783 and 0x4377F1 go through the
// SetUnitCastId thunk, 0x3E2B652 in 1.3.1 (0x3E2B57E in 1.3.0).
constexpr auto LeftTimerPerformWindow = std::to_array<std::uint8_t>({
    0xC6, 0x84, 0x24, 0x90, 0x00, 0x00, 0x00, 0x00, 0xE8, 0x92, 0xAF, 0xFF,
    0xFF, 0x84, 0xC0, 0x74, 0x01, 0xCC, 0x48, 0x8B, 0xCF, 0xE8, 0x45, 0x3F,
    0xF1, 0xFF, 0x41, 0x8B, 0xD7, 0x48, 0x8B, 0xCF, 0x8B, 0xD8, 0xE8, 0xCA,
    0x3E, 0x9F, 0x03, 0xBA, 0x00, 0x00, 0x00, 0x20, 0x41, 0xB8, 0x01, 0x00,
    0x00, 0x00, 0x48, 0x8B, 0xCF, 0xE8, 0xA5, 0x69, 0xF1, 0xFF, 0x45, 0x33,
    0xF6, 0x45, 0x8B, 0xCC, 0x44, 0x89, 0x74, 0x24, 0x30, 0x44, 0x8B, 0xC5,
    0x44, 0x89, 0x74, 0x24, 0x28, 0x48, 0x8B, 0xD7, 0x48, 0x8B, 0xCE, 0xC7,
    0x44, 0x24, 0x20, 0x01, 0x00, 0x00, 0x00, 0xE8, 0xEF, 0x34, 0x00, 0x00,
    0x45, 0x33, 0xC0, 0xBA, 0x00, 0x00, 0x00, 0x20, 0x48, 0x8B, 0xCF, 0xE8,
    0x6F, 0x69, 0xF1, 0xFF, 0x44, 0x89, 0x74, 0x24, 0x28, 0x45, 0x8B, 0xCC,
    0x44, 0x89, 0x7C, 0x24, 0x20, 0x44, 0x8B, 0xC5, 0x48, 0x8B, 0xD7, 0x48,
    0x8B, 0xCE, 0xE8, 0xC4, 0x3A, 0x00, 0x00, 0x8B, 0xD3, 0x48, 0x8B, 0xCF,
    0xE8, 0x5C, 0x3E, 0x9F, 0x03, 0xEB, 0x0F, 0x4C, 0x8B, 0xC3, 0x48, 0x8D,
    0x4C, 0x24, 0x40,
});

// 0x432450..0x432480, the failure path entry and its cleanup mask test.
constexpr auto RevalidationFailureEntry = std::to_array<std::uint8_t>({
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x57, 0x48,
    0x83, 0xEC, 0x20, 0x49, 0x8B, 0xF8, 0x8B, 0xEA, 0x48, 0x8B, 0xD9, 0x4D,
    0x85, 0xC0, 0x0F, 0x84, 0x81, 0x00, 0x00, 0x00, 0x41, 0x0F, 0xB6, 0x40,
    0x29, 0x84, 0x05, 0x6D, 0x72, 0x96, 0x01, 0x74, 0x74, 0x66, 0x41, 0x83,
});

// 0x43B8C2..0x43B914, AuraStart's final arm (0x43B8FD -> 0x43B2B0, native 1).
constexpr auto AuraStartArmWindow = std::to_array<std::uint8_t>({
    0x0F, 0xB6, 0x8D, 0x06, 0x01, 0x00, 0x00, 0x45, 0x8B, 0xCD, 0x41, 0xB8,
    0xCA, 0x00, 0x00, 0x00, 0x90, 0x89, 0x74, 0x24, 0x20, 0x49, 0x8B, 0xD7,
    0xE8, 0x21, 0xC3, 0xEB, 0xFF, 0x4C, 0x8B, 0x7C, 0x24, 0x40, 0xC7, 0x44,
    0x24, 0x28, 0x01, 0x00, 0x00, 0x00, 0x45, 0x8B, 0xCC, 0x45, 0x8B, 0xC6,
    0x44, 0x89, 0x6C, 0x24, 0x20, 0x48, 0x8B, 0xD7, 0x48, 0x8B, 0xCD, 0xE8,
    0xAE, 0xF9, 0xFF, 0xFF, 0x4C, 0x8B, 0xAC, 0x24, 0x80, 0x00, 0x00, 0x00,
    0x48, 0x8B, 0x5C, 0x24, 0x70, 0x4C, 0x8B, 0x64, 0x24, 0x78,
});

// 0x102490..0x1024C0, world click handler entry (mov rdi,rcx).
constexpr auto ClickHandlerEntry = std::to_array<std::uint8_t>({
    0x40, 0x57, 0x48, 0x83, 0xEC, 0x50, 0x48, 0x8B, 0xF9, 0xE8, 0x82, 0xEB,
    0xFF, 0xFF, 0x85, 0xC0, 0x0F, 0x84, 0x4D, 0x06, 0x00, 0x00, 0x48, 0x89,
    0x5C, 0x24, 0x60, 0xB9, 0x18, 0x00, 0x00, 0x00, 0x48, 0x89, 0x74, 0x24,
    0x40, 0x4C, 0x89, 0x64, 0x24, 0x38, 0x4C, 0x89, 0x6C, 0x24, 0x30, 0x4C,
});

// 0x102700..0x102770, the skill node gate call (0x102753 -> 0x33F770).
constexpr auto ClickGateWindow = std::to_array<std::uint8_t>({
    0x49, 0x8B, 0xCF, 0xE8, 0xC8, 0x92, 0x24, 0x00, 0x41, 0xB8, 0x2E, 0x0A,
    0x00, 0x00, 0x48, 0x8D, 0x15, 0x3B, 0x4B, 0xBC, 0x01, 0x49, 0x8B, 0xCF,
    0x8B, 0xE8, 0xE8, 0x11, 0x7C, 0x24, 0x00, 0x83, 0x7C, 0x24, 0x68, 0x00,
    0x8B, 0xF0, 0x75, 0x20, 0x8B, 0xC5, 0x83, 0xE8, 0x02, 0x74, 0x0A, 0x83,
    0xE8, 0x02, 0x74, 0x05, 0x83, 0xF8, 0x01, 0x75, 0x0F, 0xF6, 0xC3, 0x02,
    0x74, 0x0A, 0x41, 0x83, 0xFC, 0x01, 0x0F, 0x85, 0x88, 0x03, 0x00, 0x00,
    0x4C, 0x8B, 0x64, 0x24, 0x70, 0x49, 0x8B, 0x16, 0x49, 0x8B, 0xCC, 0xE8,
    0x18, 0xD0, 0x23, 0x00, 0x85, 0xC0, 0x0F, 0x84, 0x97, 0x01, 0x00, 0x00,
    0x49, 0x8B, 0xCC, 0xE8, 0x78, 0x79, 0x24, 0x00, 0x49, 0x8B, 0x0E, 0x88,
    0x44, 0x24, 0x68, 0x48,
});

// Witnesses shared with celestialrayone.aura-reactivation (same image).

// 0x48B890, EVENT_DeleteByTypeId entry
constexpr auto DeleteEventsByTypeEntry = std::to_array<std::uint8_t>({
    0x40, 0x53, 0x56, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC,
    0x20, 0x45, 0x8B, 0xF1, 0x45, 0x8B, 0xE0, 0x4C, 0x8B, 0xFA, 0x48, 0x8B,
    0xF1, 0x41, 0x83, 0xF8, 0x0E,
});

// 0x48B8F0, its node walk: next at +38h, unit at +08h
constexpr auto DeleteEventsWalkWitness = std::to_array<std::uint8_t>({
    0x48, 0x8B, 0x6B, 0x38, 0x4C, 0x39, 0x7B, 0x08, 0x74, 0x0F, 0x48, 0x8D,
    0x4C, 0x24, 0x60, 0xE8, 0x2C, 0xF3, 0xFF, 0xFF, 0x84, 0xC0, 0x74, 0x01,
    0xCC, 0x0F, 0xB6, 0x03, 0x41, 0x3B, 0xC4, 0x0F, 0x85, 0x44, 0x01, 0x00,
    0x00, 0x45, 0x85, 0xF6, 0x74, 0x0A, 0x44, 0x39, 0x73, 0x18, 0x0F, 0x85,
    0x35, 0x01, 0x00, 0x00,
});

// 0x48FE50, returns [unit+148h]
constexpr auto GetUnitEventListBody = std::to_array<std::uint8_t>({
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x85, 0xC9, 0x75, 0x1A, 0x88, 0x4C, 0x24,
    0x30, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8, 0xB9, 0xDD, 0xFF, 0xFF, 0x84,
    0xC0, 0x74, 0x01, 0xCC, 0x33, 0xC0, 0x48, 0x83, 0xC4, 0x28, 0xC3, 0x48,
    0x8B, 0x81, 0x48, 0x01, 0x00, 0x00, 0x48, 0x83, 0xC4, 0x28, 0xC3,
});

// 0x48BF55, EVENT_SetEvent node stores: customId +18h, customParam +1Ch, arg7 +20h, type +00h
constexpr auto SetEventStoreWitness = std::to_array<std::uint8_t>({
    0x8B, 0x4C, 0x24, 0x78, 0x44, 0x8B, 0xC5, 0x48, 0x8B, 0xD8, 0x89, 0x68,
    0x04, 0x89, 0x48, 0x18, 0x8B, 0x8C, 0x24, 0x80, 0x00, 0x00, 0x00, 0x89,
    0x48, 0x1C, 0x8B, 0x8C, 0x24, 0x88, 0x00, 0x00, 0x00, 0x89, 0x48, 0x20,
    0x48, 0x8B, 0x4C, 0x24, 0x70, 0x8B, 0x50, 0x14, 0x48, 0x89, 0x48, 0x48,
    0x48, 0x8B, 0xCE, 0x40, 0x88, 0x38,
});

// 0x097790
constexpr auto GetSkillsTxtRecordEntry = std::to_array<std::uint8_t>({
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48,
    0x83, 0xEC, 0x30, 0x48, 0x63, 0xF2, 0xE8, 0xE9, 0x92, 0x26, 0x00, 0x48,
});

// 0x2141A0
constexpr auto GetStateCountEntry = std::to_array<std::uint8_t>({
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0xE8, 0xE5, 0xC8, 0x0E, 0x00, 0x48,
    0x8B, 0x98, 0x98, 0x02, 0x00, 0x00, 0x48, 0x63, 0xCB, 0x48, 0x3B, 0xCB,
});

// 0x3354C0
constexpr auto ToggleStateEntry = std::to_array<std::uint8_t>({
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89,
    0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x41, 0x8B, 0xE8, 0x8B,
    0xDA, 0x48, 0x8B, 0xF1,
});

// 0x2F5940
constexpr auto GetStateStatListEntry = std::to_array<std::uint8_t>({
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0xDA,
    0x48, 0x8B, 0xF9, 0x48, 0x85, 0xC9, 0x75, 0x13,
});

// 0x2F7920
constexpr auto UnlinkStatListEntry = std::to_array<std::uint8_t>({
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xDA, 0xE8, 0xB2, 0x27,
    0x05, 0x00, 0x0F, 0xB6, 0xC8, 0x48, 0x8B, 0xD3, 0x48, 0x83, 0xC4, 0x20,
    0x5B, 0xE9,
});

// 0x2F4180
constexpr auto FreeStatListEntry = std::to_array<std::uint8_t>({
    0x48, 0x85, 0xD2, 0x74, 0x0A, 0x83, 0x7A, 0x1C, 0x00, 0x0F, 0x8D, 0x21,
    0x5D, 0x00, 0x00, 0xC3,
});

// 0x34B9D0
constexpr auto GetUnitTypeEntry = std::to_array<std::uint8_t>({
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x85, 0xC9, 0x75, 0x1D, 0x88, 0x4C, 0x24,
    0x30, 0x48, 0x8D, 0x4C,
});

// 0x34A540, left skill getter (as celestialrayone.whirlwind-follow-cursor checks it).
constexpr auto GetLeftSkillEntry = std::to_array<std::uint8_t>({
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9,
    0x75, 0x13,
});

// The six call sites, stock bytes.
constexpr auto LeftSelectCall        = std::to_array<std::uint8_t>({0xE8, 0x7C, 0x61, 0xF0, 0xFF});
constexpr auto RightAuraStatListCall = std::to_array<std::uint8_t>({0xE8, 0x39, 0xCD, 0xEB, 0xFF});
constexpr auto RightAuraStateOffCall = std::to_array<std::uint8_t>({0xE8, 0x88, 0xC8, 0xEF, 0xFF});
constexpr auto LeftTimerPerformCall  = std::to_array<std::uint8_t>({0xE8, 0xEF, 0x34, 0x00, 0x00});
constexpr auto AuraStartArmCall      = std::to_array<std::uint8_t>({0xE8, 0xAE, 0xF9, 0xFF, 0xFF});
constexpr auto ClickGateCall         = std::to_array<std::uint8_t>({0xE8, 0x18, 0xD0, 0x23, 0x00});

// Inline hook entries: whole instructions, none of them relative.
constexpr std::uint32_t PeriodicSkillEventHookBytes  = 22;  // two home saves, five pushes, sub rsp,50h
constexpr std::uint32_t RevalidationFailureHookBytes = 15;  // two home saves, push rdi, sub rsp,20h

// ---------------------------------------------------------------------------
//  Relay page. One page the plugin allocates within rel32 reach of the image;
//  nothing inside D2R's own code is used as a cave.
//
//      +0x000  UNWIND_INFO, no frame (every stub leaves rsp untouched)
//      +0x010  RUNTIME_FUNCTION table, 6 entries
//      +0x100  mov rax,<bridge> / jmp rax      left select
//      +0x120  mov rax,<bridge> / jmp rax      right aura stat list
//      +0x140  mov rax,<bridge> / jmp rax      right aura state off
//      +0x160  mov rax,<bridge> / jmp rax      left timer perform
//      +0x180  mov rax,<bridge> / jmp rax      AuraStart arm
//      +0x1A0  lea r8,[r14-28h] / mov rax,<bridge> / jmp rax   click gate
//
//  A stub is reached by the retargeted call, so the return address is on the
//  stack and the stock arguments are in place: the bridge is entered as if
//  the game had called it. rax and r8 are volatile and not arguments of the
//  replaced callees.
// ---------------------------------------------------------------------------
constexpr std::size_t RelayPageBytes           = 0x1000;
constexpr std::size_t RelayUnwindOffset        = 0x000;
constexpr std::size_t RelayFunctionTableOffset = 0x010;
constexpr std::size_t RelayStubCount           = 6;
enum RelayStub : std::size_t {
    StubLeftSelect        = 0,
    StubRightAuraStatList = 1,
    StubRightAuraStateOff = 2,
    StubLeftTimerPerform  = 3,
    StubAuraStartArm      = 4,
    StubClickGate         = 5,
};
constexpr std::array<std::size_t, RelayStubCount> RelayStubOffsets{0x100, 0x120, 0x140, 0x160, 0x180, 0x1A0};
constexpr std::array<std::size_t, RelayStubCount> RelayStubSizes{12, 12, 12, 12, 12, 16};

// ---------------------------------------------------------------------------
//  Native signatures
// ---------------------------------------------------------------------------
using SetLeftActiveSkillFn  = std::int64_t(__fastcall*)(void* unit, std::int32_t skillId, std::int32_t ownerGuid) noexcept;
using PeriodicSkillEventFn  = void(__fastcall*)(void* game, void* unit, std::int32_t customId, std::int32_t customParam, std::int32_t castId) noexcept;
using RevalidationFailureFn = std::int64_t(__fastcall*)(void** context, std::int32_t skillId, const std::uint8_t* record) noexcept;
using AuraStartFn           = std::int64_t(__fastcall*)(void* game, void* unit, std::int32_t skillId) noexcept;
using ArmPeriodicSkillFn    = std::int64_t(__fastcall*)(void* game, void* unit, std::int32_t skillId, std::int32_t level, std::int32_t castId, std::int32_t rightTimer) noexcept;
using ServerDoSkillFn       = std::int32_t(__fastcall*)(void* game, void* unit, std::int32_t skillId, std::int32_t level, std::int32_t consume, std::int32_t itemCast, std::int32_t itemEffect) noexcept;
using SkillNodeGateFn       = std::uint64_t(__fastcall*)(void* player, void* node) noexcept;
using GetSkillNodeFn        = void*(__fastcall*)(void* unit) noexcept;
using GetGameFn             = void*(__fastcall*)(void* unit) noexcept;
using CheckStateFn          = std::int32_t(__fastcall*)(void* unit, std::int32_t state) noexcept;
using DeleteEventsByTypeFn  = std::uint64_t(__fastcall*)(void* game, void* unit, std::int32_t type, std::uint32_t customId) noexcept;
using GetSkillsTxtRecordFn  = const std::uint8_t*(__fastcall*)(std::uint32_t dataContext, std::int32_t skillId) noexcept;
using GetStateCountFn       = std::int32_t(__fastcall*)(std::uint32_t dataContext) noexcept;
using ToggleStateFn         = void(__fastcall*)(void* unit, std::int32_t state, std::int32_t enable) noexcept;
using GetStateStatListFn    = void*(__fastcall*)(void* unit, std::int32_t state) noexcept;
using UnlinkStatListFn      = void(__fastcall*)(void* unit, void* statList) noexcept;
using FreeStatListFn        = void(__fastcall*)(std::uint32_t dataContext, void* statList) noexcept;
using GetUnitTypeFn         = std::int32_t(__fastcall*)(void* unit) noexcept;

// ---------------------------------------------------------------------------
//  State
// ---------------------------------------------------------------------------
struct Settings {
    bool enabled{true};
    bool leftSlotAuras{true};
    bool singleEmanation{true};
    bool moveOnlyClicks{true};
    bool diagnostics{false};
};

struct InstallState {
    bool selectRelay{false};
    bool timerHook{false};
    bool failureHook{false};
    bool startRelay{false};
    bool rightRemovalRelays{false};
    bool performRelay{false};
    bool clickRelay{false};
};

const D2RL::PluginContext* Context{};
std::uintptr_t             Base{};
Settings                   Config{};
InstallState               Installed{};
char                       ConfigSource[48]{"built-in defaults"};
std::atomic<bool>          ServerActive{false};
std::atomic<bool>          PerformRelayActive{false};
std::atomic<bool>          ClientActive{false};

SetLeftActiveSkillFn  SetLeftActiveSkill{};
PeriodicSkillEventFn  OriginalPeriodicSkillEvent{};
RevalidationFailureFn OriginalRevalidationFailure{};
AuraStartFn           AuraStart{};
ArmPeriodicSkillFn    ArmPeriodicSkill{};
ServerDoSkillFn       ServerDoSkill{};
SkillNodeGateFn       SkillNodeGate{};
GetSkillNodeFn        GetLeftSkill{};
GetSkillNodeFn        GetRightSkill{};
GetGameFn             GetGame{};
CheckStateFn          CheckState{};
DeleteEventsByTypeFn  DeleteEventsByType{};
GetSkillsTxtRecordFn  GetSkillsTxtRecord{};
GetStateCountFn       GetStateCount{};
ToggleStateFn         ToggleState{};
GetStateStatListFn    GetStateStatList{};
UnlinkStatListFn      UnlinkStatList{};
FreeStatListFn        FreeStatList{};
GetUnitTypeFn         GetUnitType{};

std::uint8_t* RelayPage{};
bool          RelayUnwindRegistered{};

struct CallSitePatch {
    std::uint64_t               rva;
    const std::uint8_t*         stock;
    std::size_t                 stub;
    std::array<std::uint8_t, 5> written;
    bool                        applied;
};
std::array<CallSitePatch, RelayStubCount> CallSites{{
    {LeftSelectCallRva, LeftSelectCall.data(), StubLeftSelect, {}, false},
    {RightAuraStatListCallRva, RightAuraStatListCall.data(), StubRightAuraStatList, {}, false},
    {RightAuraStateOffCallRva, RightAuraStateOffCall.data(), StubRightAuraStateOff, {}, false},
    {LeftTimerPerformCallRva, LeftTimerPerformCall.data(), StubLeftTimerPerform, {}, false},
    {AuraStartArmCallRva, AuraStartArmCall.data(), StubAuraStartArm, {}, false},
    {ClickGateCallRva, ClickGateCall.data(), StubClickGate, {}, false},
}};

std::atomic<std::uint64_t> LeftStarts{};
std::atomic<std::uint64_t> LeftStartsRefused{};
std::atomic<std::uint64_t> LeftKeptRunning{};
std::atomic<std::uint64_t> LeftStopsReselect{};
std::atomic<std::uint64_t> LeftStopsOffButton{};
std::atomic<std::uint64_t> LeftStopsFailed{};
std::atomic<std::uint64_t> SharedStatesKept{};
std::atomic<std::uint64_t> EmanationsSkipped{};
std::atomic<std::uint64_t> MoveOnlyClicks{};
std::atomic<std::uint32_t> DiagnosticsBudget{};

template <typename T>
auto At(std::uint64_t rva) noexcept -> T {
    return reinterpret_cast<T>(Base + static_cast<std::uintptr_t>(rva));
}

auto ReadU8(const void* base, std::size_t offset) noexcept -> std::uint8_t {
    return static_cast<const std::uint8_t*>(base)[offset];
}

auto ReadI16(const void* base, std::size_t offset) noexcept -> std::int16_t {
    std::int16_t value = 0;
    std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(value));
    return value;
}

auto ReadU16(const void* base, std::size_t offset) noexcept -> std::uint16_t {
    std::uint16_t value = 0;
    std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(value));
    return value;
}

auto ReadU32(const void* base, std::size_t offset) noexcept -> std::uint32_t {
    std::uint32_t value = 0;
    std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(value));
    return value;
}

auto ReadPointer(const void* base, std::size_t offset) noexcept -> void* {
    void* value = nullptr;
    std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(value));
    return value;
}

auto PutU32(std::uint8_t* at, std::uint32_t value) noexcept -> void {
    std::memcpy(at, &value, sizeof(value));
}

auto PutU64(std::uint8_t* at, std::uint64_t value) noexcept -> void {
    std::memcpy(at, &value, sizeof(value));
}

// ---------------------------------------------------------------------------
//  Logging
// ---------------------------------------------------------------------------
void LogInfo(const char* format, ...) noexcept {
    char message[512]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    Context->LogInfo(message);
}

void LogWarn(const char* format, ...) noexcept {
    char message[512]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    Context->LogWarn(message);
}

void LogError(const char* format, ...) noexcept {
    char message[512]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    Context->LogError(message);
}

auto TakeDiagnosticsLine() noexcept -> bool {
    if (!Config.diagnostics) {
        return false;
    }
    auto budget = DiagnosticsBudget.load(std::memory_order_relaxed);
    while (budget != 0) {
        if (DiagnosticsBudget.compare_exchange_weak(budget, budget - 1, std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
//  Configuration
// ---------------------------------------------------------------------------
auto Trim(std::string_view value) noexcept -> std::string_view {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
        value.remove_suffix(1);
    }
    return value;
}

void ParseConfig(std::string_view text) noexcept {
    std::size_t lineNumber = 0;
    for (std::size_t start = 0; start <= text.size();) {
        ++lineNumber;
        const auto end = text.find('\n', start);
        auto line = text.substr(start, end == std::string_view::npos ? text.size() - start : end - start);
        start = end == std::string_view::npos ? text.size() + 1 : end + 1;

        const auto comment = line.find('#');
        if (comment != std::string_view::npos) {
            line = line.substr(0, comment);
        }
        line = Trim(line);
        if (line.empty()) {
            continue;
        }

        const auto equals = line.find('=');
        if (equals == std::string_view::npos) {
            LogWarn("LeftClickAuras: config line %zu ignored, expected key = value.", lineNumber);
            continue;
        }
        const auto key   = Trim(line.substr(0, equals));
        const auto value = Trim(line.substr(equals + 1));

        bool* target = nullptr;
        if (key == "enabled") {
            target = &Config.enabled;
        } else if (key == "left_slot_auras") {
            target = &Config.leftSlotAuras;
        } else if (key == "single_emanation") {
            target = &Config.singleEmanation;
        } else if (key == "move_only_clicks") {
            target = &Config.moveOnlyClicks;
        } else if (key == "diagnostics") {
            target = &Config.diagnostics;
        } else {
            LogWarn("LeftClickAuras: config line %zu has an unknown key.", lineNumber);
            continue;
        }

        if (value == "true") {
            *target = true;
        } else if (value == "false") {
            *target = false;
        } else {
            LogWarn("LeftClickAuras: config line %zu ignored, the value must be true or false.", lineNumber);
        }
    }
}

void LoadConfig() noexcept {
    Config = Settings{};
    std::snprintf(ConfigSource, sizeof(ConfigSource), "%s", "built-in defaults");
    if (!Context->EnsureConfig(DefaultConfigToml)) {
        LogWarn("LeftClickAuras: could not create the default config, using built-in defaults.");
    }

    std::string buffer(16384, '\0');
    std::uint32_t required = 0;
    if (!Context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size()), &required)) {
        if (required <= buffer.size()) {
            LogWarn("LeftClickAuras: could not read the config, using built-in defaults.");
            return;
        }
        buffer.assign(required, '\0');
        if (!Context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size()), &required)) {
            LogWarn("LeftClickAuras: could not read the config, using built-in defaults.");
            return;
        }
    }
    ParseConfig(std::string_view(buffer.c_str()));
    std::snprintf(ConfigSource, sizeof(ConfigSource), "%s", "config file");
}

// ---------------------------------------------------------------------------
//  Image checks
// ---------------------------------------------------------------------------
auto CheckExact(std::uint64_t rva, const std::uint8_t* bytes, std::size_t size, const char* name) noexcept -> bool {
    if (Context->CheckExpectedBytes(rva, bytes, static_cast<std::uint32_t>(size))) {
        return true;
    }
    LogError("LeftClickAuras: %s at RVA 0x%llX does not match D2RLoader 1.3.1.", name,
             static_cast<unsigned long long>(rva));
    return false;
}

// A function the plugin only calls may already carry another plugin's inline
// hook at its entry. The loader tracks those, and calling through one is fine.
auto CheckCallable(std::uint64_t rva, const std::uint8_t* bytes, std::size_t size, const char* name) noexcept -> bool {
    if (Context->CheckExpectedBytes(rva, bytes, static_cast<std::uint32_t>(size))) {
        return true;
    }

    const D2RL::DiagnosticsService* diagnostics = nullptr;
    if (Context->QueryService(&diagnostics)
            == D2RL::ServiceQueryResult::Success
        && D2RL::HasDiagnosticsServiceField(diagnostics, D2RL::DiagnosticsServiceRequiredSize)
        && diagnostics->queryHookStatus != nullptr) {
        D2RL::Diagnostics::HookQuery query{
            .structSize   = D2RL::Diagnostics::HookQuerySize,
            .flags        = 0,
            .rva          = rva,
            .expected     = bytes,
            .expectedSize = static_cast<std::uint32_t>(size),
            .reserved     = 0,
        };
        D2RL::Diagnostics::HookStatus status{
            .structSize = D2RL::Diagnostics::HookStatusSize,
        };
        if (diagnostics->queryHookStatus(Context, &query, &status) == D2RL::Diagnostics::Result::Success
            && status.state == D2RL::Diagnostics::ModificationState::Tracked
            && status.kind == D2RL::Diagnostics::ModificationKind::InlineHook) {
            LogInfo("LeftClickAuras: %s at RVA 0x%llX is hooked by another plugin, calling through it.", name,
                    static_cast<unsigned long long>(rva));
            return true;
        }
    }

    LogError("LeftClickAuras: %s at RVA 0x%llX does not match D2RLoader 1.3.1.", name,
             static_cast<unsigned long long>(rva));
    return false;
}

auto ValidateServer() noexcept -> bool {
    return CheckExact(AssignSkillRva, AssignSkillBody.data(), AssignSkillBody.size(), "skill assignment")
        && CheckExact(PeriodicSkillEventRva, PeriodicSkillEventWindow.data(), PeriodicSkillEventWindow.size(),
                      "aura timer")
        && CheckExact(LeftTimerPerformWindowRva, LeftTimerPerformWindow.data(), LeftTimerPerformWindow.size(),
                      "aura timer perform")
        && CheckExact(RevalidationFailureRva, RevalidationFailureEntry.data(), RevalidationFailureEntry.size(),
                      "aura timer failure")
        && CheckExact(AuraStartArmWindowRva, AuraStartArmWindow.data(), AuraStartArmWindow.size(),
                      "aura start arm")
        && CheckCallable(DeleteEventsByTypeRva, DeleteEventsByTypeEntry.data(), DeleteEventsByTypeEntry.size(),
                         "EVENT_DeleteByTypeId entry")
        && CheckExact(DeleteEventsWalkRva, DeleteEventsWalkWitness.data(), DeleteEventsWalkWitness.size(),
                      "EVENT_DeleteByTypeId node walk")
        && CheckCallable(GetUnitEventListRva, GetUnitEventListBody.data(), GetUnitEventListBody.size(),
                         "unit event list getter")
        && CheckExact(SetEventStoreRva, SetEventStoreWitness.data(), SetEventStoreWitness.size(),
                      "EVENT_SetEvent node stores")
        && CheckCallable(GetSkillsTxtRecordRva, GetSkillsTxtRecordEntry.data(), GetSkillsTxtRecordEntry.size(),
                         "skills.txt record getter")
        && CheckCallable(GetStateCountRva, GetStateCountEntry.data(), GetStateCountEntry.size(), "state count")
        && CheckCallable(ToggleStateRva, ToggleStateEntry.data(), ToggleStateEntry.size(), "STATES_ToggleState")
        && CheckCallable(GetStateStatListRva, GetStateStatListEntry.data(), GetStateStatListEntry.size(),
                         "state stat list getter")
        && CheckCallable(UnlinkStatListRva, UnlinkStatListEntry.data(), UnlinkStatListEntry.size(),
                         "STATLIST_UnlinkStatList")
        && CheckCallable(FreeStatListRva, FreeStatListEntry.data(), FreeStatListEntry.size(),
                         "STATLIST_FreeStatList")
        && CheckCallable(GetUnitTypeRva, GetUnitTypeEntry.data(), GetUnitTypeEntry.size(), "UNITS_GetUnitType")
        && CheckCallable(GetLeftSkillRva, GetLeftSkillEntry.data(), GetLeftSkillEntry.size(), "left skill getter");
}

auto ValidateClient() noexcept -> bool {
    return CheckExact(ClickHandlerRva, ClickHandlerEntry.data(), ClickHandlerEntry.size(), "world click handler")
        && CheckExact(ClickGateWindowRva, ClickGateWindow.data(), ClickGateWindow.size(), "world click skill gate")
        && CheckCallable(GetLeftSkillRva, GetLeftSkillEntry.data(), GetLeftSkillEntry.size(), "left skill getter");
}

// ---------------------------------------------------------------------------
//  Left timers the plugin armed: (game, unit, skill, cast id). The type 8 event
//  carries the cast id in arg7 and every re-arm keeps it, so a timer the game
//  made on its own (a monster, a used skill) never matches.
// ---------------------------------------------------------------------------
struct ArmedLeftTimer {
    void*        game;
    void*        unit;
    std::int32_t skillId;
    std::int32_t castId;
};

constexpr std::size_t ArmedLeftTimerCapacity = 1024;
std::array<ArmedLeftTimer, ArmedLeftTimerCapacity> ArmedLeftTimers{};
std::size_t ArmedLeftTimerCursor{};
SRWLOCK     ArmedLeftTimerLock = SRWLOCK_INIT;

void RememberLeftTimer(void* game, void* unit, std::int32_t skillId, std::int32_t castId) noexcept {
    AcquireSRWLockExclusive(&ArmedLeftTimerLock);
    for (auto& entry : ArmedLeftTimers) {
        if (entry.unit == unit && entry.skillId == skillId) {
            entry.game   = game;
            entry.castId = castId;
            ReleaseSRWLockExclusive(&ArmedLeftTimerLock);
            return;
        }
    }
    ArmedLeftTimers[ArmedLeftTimerCursor] = ArmedLeftTimer{game, unit, skillId, castId};
    ArmedLeftTimerCursor = (ArmedLeftTimerCursor + 1) % ArmedLeftTimerCapacity;
    ReleaseSRWLockExclusive(&ArmedLeftTimerLock);
}

auto IsLeftTimer(void* game, void* unit, std::int32_t skillId, const std::int32_t* castId) noexcept -> bool {
    AcquireSRWLockShared(&ArmedLeftTimerLock);
    bool found = false;
    for (const auto& entry : ArmedLeftTimers) {
        if (entry.unit == unit && entry.game == game && entry.skillId == skillId
            && (castId == nullptr || entry.castId == *castId)) {
            found = true;
            break;
        }
    }
    ReleaseSRWLockShared(&ArmedLeftTimerLock);
    return found;
}

auto ForgetLeftTimer(void* game, void* unit, std::int32_t skillId) noexcept -> bool {
    AcquireSRWLockExclusive(&ArmedLeftTimerLock);
    bool found = false;
    for (auto& entry : ArmedLeftTimers) {
        if (entry.unit == unit && entry.game == game && entry.skillId == skillId) {
            entry = ArmedLeftTimer{};
            found = true;
        }
    }
    ReleaseSRWLockExclusive(&ArmedLeftTimerLock);
    return found;
}

// The AuraStart call the assignment hook is making on this thread, if any.
struct PendingLeftStart {
    void*        unit;
    std::int32_t skillId;
    bool         armed;
};
thread_local PendingLeftStart* PendingStart = nullptr;

// Set while the timer hook hands a left timer that left the button to 0x432450.
thread_local bool StoppingOffButton = false;

// ---------------------------------------------------------------------------
//  Game data helpers
// ---------------------------------------------------------------------------
auto AuraFlagMask() noexcept -> std::uint8_t {
    return ReadU8(reinterpret_cast<const void*>(Base), static_cast<std::size_t>(AuraFlagMaskRva));
}

auto CleanupFlagMask() noexcept -> std::uint8_t {
    return ReadU8(reinterpret_cast<const void*>(Base), static_cast<std::size_t>(CleanupFlagMaskRva));
}

auto DataContextOf(void* game) noexcept -> std::uint32_t {
    return ReadU8(game, GameDataContextOffset);
}

auto UnitIdOf(void* unit) noexcept -> std::uint32_t {
    return unit != nullptr ? ReadU32(unit, UnitIdOffset) : 0xFFFFFFFFU;
}

// Skill id of a skill node, -1 for none (word [[node]], as 0x3402D0 reads it).
auto NodeSkillId(void* node) noexcept -> std::int32_t {
    if (node == nullptr) {
        return -1;
    }
    const auto* record = static_cast<const std::uint8_t*>(ReadPointer(node, SkillNodeRecordOffset));
    if (record == nullptr) {
        return -1;
    }
    return ReadI16(record, SkillRecordIdOffset);
}

// The skills.txt record when the skill is an aura AuraStart accepts: aura flag
// and an aurastate inside the state table. Null otherwise.
auto AuraRecordFor(std::uint32_t dataContext, std::int32_t skillId) noexcept -> const std::uint8_t* {
    if (skillId <= 0) {
        return nullptr;
    }
    const auto* record = GetSkillsTxtRecord(dataContext, skillId);
    if (record == nullptr || (record[SkillRecordFlagsByteOffset] & AuraFlagMask()) == 0) {
        return nullptr;
    }
    const auto state = static_cast<std::int32_t>(ReadI16(record, SkillRecordAuraStateOffset));
    if (state < 0 || state >= GetStateCount(dataContext)) {
        return nullptr;
    }
    return record;
}

auto AuraStateOf(const std::uint8_t* record) noexcept -> std::int32_t {
    return ReadI16(record, SkillRecordAuraStateOffset);
}

// A type 8 event with this custom id that is not already being deleted.
auto HasLiveTimer(void* unit, std::uint32_t customId) noexcept -> bool {
    if (unit == nullptr) {
        return false;
    }
    void* node = ReadPointer(unit, UnitEventListOffset);
    for (std::size_t walked = 0; node != nullptr && walked < MaximumUnitEvents; ++walked) {
        if (ReadU8(node, EventTypeOffset) == PeriodicSkillEventType
            && ReadU32(node, EventCustomIdOffset) == customId
            && (ReadU16(node, EventFlagsOffset) & EventDeleteDeferredFlag) == 0) {
            return true;
        }
        node = ReadPointer(node, EventUnitNextOffset);
    }
    return false;
}

// The left button holds a running aura that uses this state.
auto LeftAuraUsesState(void* game, void* unit, std::int32_t state) noexcept -> bool {
    const auto skillId = NodeSkillId(GetLeftSkill(unit));
    const auto* record = AuraRecordFor(DataContextOf(game), skillId);
    return record != nullptr && AuraStateOf(record) == state
        && HasLiveTimer(unit, static_cast<std::uint32_t>(skillId));
}

// The right button holds a running aura that uses this state.
auto RightAuraUsesState(void* game, void* unit, std::int32_t state) noexcept -> bool {
    const auto skillId = NodeSkillId(GetRightSkill(unit));
    const auto* record = AuraRecordFor(DataContextOf(game), skillId);
    return record != nullptr && AuraStateOf(record) == state && HasLiveTimer(unit, RightTimerCustomId);
}

enum class RemovalOrder {
    StatListFirst,  // as the right button's removal in 0x438A70
    StateFirst,     // as the failure path 0x432450
};

void RemoveAuraState(void* game, void* unit, const std::uint8_t* record, RemovalOrder order) noexcept {
    const auto dataContext = DataContextOf(game);
    const auto state       = AuraStateOf(record);
    const auto removeStatList = [&]() noexcept {
        if (auto* statList = GetStateStatList(unit, state); statList != nullptr) {
            UnlinkStatList(unit, statList);
            FreeStatList(dataContext, statList);
        }
    };
    if (order == RemovalOrder::StatListFirst) {
        removeStatList();
        ToggleState(unit, state, 0);
    } else {
        ToggleState(unit, state, 0);
        removeStatList();
    }
}

// ---------------------------------------------------------------------------
//  Starting and stopping the left aura
// ---------------------------------------------------------------------------
void StartLeftAura(void* game, void* unit, std::int32_t skillId) noexcept {
    PendingLeftStart pending{unit, skillId, false};
    PendingStart = &pending;
    AuraStart(game, unit, skillId);
    PendingStart = nullptr;

    if (pending.armed) {
        LeftStarts.fetch_add(1, std::memory_order_relaxed);
        if (TakeDiagnosticsLine()) {
            LogInfo("LeftClickAuras: started skill %d on unit %u.", skillId, UnitIdOf(unit));
        }
    } else {
        LeftStartsRefused.fetch_add(1, std::memory_order_relaxed);
        if (TakeDiagnosticsLine()) {
            LogInfo("LeftClickAuras: the game did not start skill %d on unit %u (no skill level).", skillId,
                    UnitIdOf(unit));
        }
    }
}

void StopLeftAura(void* game, void* unit, std::int32_t skillId, const std::uint8_t* record) noexcept {
    ForgetLeftTimer(game, unit, skillId);
    if (RightAuraUsesState(game, unit, AuraStateOf(record))) {
        SharedStatesKept.fetch_add(1, std::memory_order_relaxed);
    } else {
        RemoveAuraState(game, unit, record, RemovalOrder::StatListFirst);
    }
    DeleteEventsByType(game, unit, PeriodicSkillEventType, static_cast<std::uint32_t>(skillId));

    LeftStopsReselect.fetch_add(1, std::memory_order_relaxed);
    if (TakeDiagnosticsLine()) {
        LogInfo("LeftClickAuras: stopped skill %d on unit %u, the left button changed.", skillId, UnitIdOf(unit));
    }
}

void UpdateLeftAura(void* unit, std::int32_t beforeSkillId, std::int32_t afterSkillId) noexcept {
    auto* game = GetGame(unit);
    if (game == nullptr) {
        return;
    }
    const auto dataContext = DataContextOf(game);
    const auto* beforeRecord = AuraRecordFor(dataContext, beforeSkillId);
    const auto* afterRecord  = AuraRecordFor(dataContext, afterSkillId);

    if (beforeRecord != nullptr && afterSkillId != beforeSkillId) {
        StopLeftAura(game, unit, beforeSkillId, beforeRecord);
    }
    if (afterRecord == nullptr) {
        return;
    }
    if (afterSkillId == beforeSkillId && HasLiveTimer(unit, static_cast<std::uint32_t>(afterSkillId))
        && CheckState(unit, AuraStateOf(afterRecord)) != 0) {
        LeftKeptRunning.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    StartLeftAura(game, unit, afterSkillId);
}

// ---------------------------------------------------------------------------
//  Hooks and relay bridges
// ---------------------------------------------------------------------------
// 0x438AEF, AssignSkill's left branch selects the skill it found.
std::int64_t __fastcall BridgeLeftSelect(void* unit, std::int32_t skillId, std::int32_t ownerGuid) noexcept {
    if (!ServerActive.load(std::memory_order_relaxed) || unit == nullptr) {
        return SetLeftActiveSkill(unit, skillId, ownerGuid);
    }
    const auto beforeSkillId = NodeSkillId(GetLeftSkill(unit));
    const auto result        = SetLeftActiveSkill(unit, skillId, ownerGuid);
    UpdateLeftAura(unit, beforeSkillId, NodeSkillId(GetLeftSkill(unit)));
    return result;
}

// 0x43B8FD, AuraStart's arm.
std::int64_t __fastcall BridgeAuraStartArm(void* game, void* unit, std::int32_t skillId, std::int32_t level,
                                           std::int32_t castId, std::int32_t rightTimer) noexcept {
    auto* pending = PendingStart;
    if (pending != nullptr && pending->unit == unit && pending->skillId == skillId) {
        const auto result = ArmPeriodicSkill(game, unit, skillId, level, castId, 0);
        if (result != 0) {
            RememberLeftTimer(game, unit, skillId, castId);
            pending->armed = true;
        }
        return result;
    }
    return ArmPeriodicSkill(game, unit, skillId, level, castId, rightTimer);
}

// 0x438C02, the right button's removal asks for the old aura's state stat list.
void* __fastcall BridgeRightAuraStatList(void* unit, std::int32_t state) noexcept {
    if (ServerActive.load(std::memory_order_relaxed) && unit != nullptr) {
        auto* game = GetGame(unit);
        if (game != nullptr && LeftAuraUsesState(game, unit, state)) {
            SharedStatesKept.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
    }
    return GetStateStatList(unit, state);
}

// 0x438C33, the right button's removal turns the old aura's state off.
void __fastcall BridgeRightAuraStateOff(void* unit, std::int32_t state, std::int32_t enable) noexcept {
    if (ServerActive.load(std::memory_order_relaxed) && unit != nullptr && enable == 0) {
        auto* game = GetGame(unit);
        if (game != nullptr && LeftAuraUsesState(game, unit, state)) {
            return;
        }
    }
    ToggleState(unit, state, enable);
}

// 0x437460
void __fastcall HookPeriodicSkillEvent(void* game, void* unit, std::int32_t customId, std::int32_t customParam,
                                       std::int32_t castId) noexcept {
    if (ServerActive.load(std::memory_order_relaxed) && customId > 0 && game != nullptr && unit != nullptr
        && GetUnitType(unit) == UnitTypePlayer && IsLeftTimer(game, unit, customId, &castId)
        && NodeSkillId(GetLeftSkill(unit)) != customId) {
        LeftStopsOffButton.fetch_add(1, std::memory_order_relaxed);
        if (TakeDiagnosticsLine()) {
            LogInfo("LeftClickAuras: stopped skill %d on unit %u, it is no longer on the left button.", customId,
                    UnitIdOf(unit));
        }
        void* context[2]{game, unit};
        StoppingOffButton = true;
        At<RevalidationFailureFn>(RevalidationFailureRva)(
            context, customId, GetSkillsTxtRecord(DataContextOf(game), customId));
        StoppingOffButton = false;
        return;
    }
    OriginalPeriodicSkillEvent(game, unit, customId, customParam, castId);
}

// 0x432450
std::int64_t __fastcall HookRevalidationFailure(void** context, std::int32_t skillId,
                                                const std::uint8_t* record) noexcept {
    if (ServerActive.load(std::memory_order_relaxed) && context != nullptr && skillId > 0) {
        auto* game = context[0];
        auto* unit = context[1];
        if (game != nullptr && unit != nullptr && ForgetLeftTimer(game, unit, skillId)) {
            if (record != nullptr && (record[SkillRecordCleanupByteOffset] & CleanupFlagMask()) == 0
                && AuraRecordFor(DataContextOf(game), skillId) == record) {
                if (RightAuraUsesState(game, unit, AuraStateOf(record))) {
                    SharedStatesKept.fetch_add(1, std::memory_order_relaxed);
                } else {
                    RemoveAuraState(game, unit, record, RemovalOrder::StateFirst);
                }
            }
            if (!StoppingOffButton) {
                LeftStopsFailed.fetch_add(1, std::memory_order_relaxed);
                if (TakeDiagnosticsLine()) {
                    LogInfo("LeftClickAuras: removed skill %d from unit %u, its timer ended.", skillId,
                            UnitIdOf(unit));
                }
            }
        }
    }
    return OriginalRevalidationFailure(context, skillId, record);
}

// 0x4377BC, a skill timer performs.
std::int32_t __fastcall BridgeLeftTimerPerform(void* game, void* unit, std::int32_t skillId, std::int32_t level,
                                               std::int32_t consume, std::int32_t itemCast,
                                               std::int32_t itemEffect) noexcept {
    if (PerformRelayActive.load(std::memory_order_relaxed) && game != nullptr && unit != nullptr
        && GetUnitType(unit) == UnitTypePlayer && NodeSkillId(GetRightSkill(unit)) == skillId
        && HasLiveTimer(unit, RightTimerCustomId) && IsLeftTimer(game, unit, skillId, nullptr)) {
        EmanationsSkipped.fetch_add(1, std::memory_order_relaxed);
        if (TakeDiagnosticsLine()) {
            LogInfo("LeftClickAuras: skill %d on unit %u emanates from the right button this time.", skillId,
                    UnitIdOf(unit));
        }
        return 0;
    }
    return ServerDoSkill(game, unit, skillId, level, consume, itemCast, itemEffect);
}

// 0x102753, client world click: may this click enter the skill branch.
std::uint64_t __fastcall BridgeClickGate(void* player, void* node, const std::uint8_t* command) noexcept {
    if (ClientActive.load(std::memory_order_relaxed) && player != nullptr && node != nullptr && command != nullptr
        && (ReadU32(command, ClickCommandFlagsOffset) & ClickCommandLeftButton) != 0
        && node == GetLeftSkill(player)) {
        const auto* record = static_cast<const std::uint8_t*>(ReadPointer(node, SkillNodeRecordOffset));
        if (record != nullptr && (record[SkillRecordFlagsByteOffset] & AuraFlagMask()) != 0) {
            MoveOnlyClicks.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
    }
    return SkillNodeGate(player, node);
}

// ---------------------------------------------------------------------------
//  Installation
// ---------------------------------------------------------------------------
void ResolveNativeFunctions() noexcept {
    AuraStart          = At<AuraStartFn>(AuraStartRva);
    ArmPeriodicSkill   = At<ArmPeriodicSkillFn>(ArmPeriodicSkillRva);
    ServerDoSkill      = At<ServerDoSkillFn>(ServerDoSkillRva);
    SetLeftActiveSkill = At<SetLeftActiveSkillFn>(SetLeftActiveSkillRva);
    SkillNodeGate      = At<SkillNodeGateFn>(SkillNodeGateRva);
    GetLeftSkill       = At<GetSkillNodeFn>(GetLeftSkillRva);
    GetRightSkill      = At<GetSkillNodeFn>(GetRightSkillRva);
    GetGame            = At<GetGameFn>(GetGameRva);
    CheckState         = At<CheckStateFn>(CheckStateRva);
    DeleteEventsByType = At<DeleteEventsByTypeFn>(DeleteEventsByTypeRva);
    GetSkillsTxtRecord = At<GetSkillsTxtRecordFn>(GetSkillsTxtRecordRva);
    GetStateCount      = At<GetStateCountFn>(GetStateCountRva);
    ToggleState        = At<ToggleStateFn>(ToggleStateRva);
    GetStateStatList   = At<GetStateStatListFn>(GetStateStatListRva);
    UnlinkStatList     = At<UnlinkStatListFn>(UnlinkStatListRva);
    FreeStatList       = At<FreeStatListFn>(FreeStatListRva);
    GetUnitType        = At<GetUnitTypeFn>(GetUnitTypeRva);
}

auto AllocateRelayPage() noexcept -> std::uint8_t* {
    const auto* image     = reinterpret_cast<const std::uint8_t*>(Base);
    const auto  ntOffset  = static_cast<std::size_t>(ReadU32(image, 0x3C));
    const auto  imageSize = ReadU32(image, ntOffset + 0x50);

    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    const std::uintptr_t granularity =
        systemInfo.dwAllocationGranularity != 0 ? systemInfo.dwAllocationGranularity : 0x10000;
    const auto alignUp = [granularity](std::uintptr_t value) noexcept {
        return (value + granularity - 1) & ~(granularity - 1);
    };

    // The page sits above the image, so the lowest call site is the farthest.
    const std::uintptr_t limit = Base + ClickGateCallRva + 0x7FF00000ULL;
    std::uintptr_t address = alignUp(Base + imageSize);
    while (address + RelayPageBytes < limit) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) == 0) {
            break;
        }
        const auto regionEnd = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (info.State == MEM_FREE && address + RelayPageBytes <= regionEnd) {
            if (auto* page = VirtualAlloc(reinterpret_cast<LPVOID>(address), RelayPageBytes, MEM_RESERVE | MEM_COMMIT,
                                          PAGE_READWRITE)) {
                return static_cast<std::uint8_t*>(page);
            }
        }
        const auto next = alignUp(regionEnd);
        if (next <= address) {
            break;
        }
        address = next;
    }
    return nullptr;
}

void EmitRelay(std::uint8_t* page) noexcept {
    const std::uint8_t noFrameUnwind[4]{0x01, 0x00, 0x00, 0x00};
    std::memcpy(page + RelayUnwindOffset, noFrameUnwind, sizeof(noFrameUnwind));

    const std::array<std::uint64_t, RelayStubCount> bridges{
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&BridgeLeftSelect)),
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&BridgeRightAuraStatList)),
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&BridgeRightAuraStateOff)),
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&BridgeLeftTimerPerform)),
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&BridgeAuraStartArm)),
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&BridgeClickGate)),
    };

    for (std::size_t index = 0; index < RelayStubCount; ++index) {
        auto* stub = page + RelayStubOffsets[index];
        std::size_t at = 0;
        if (index == StubClickGate) {
            const std::uint8_t commandLoad[]{0x4D, 0x8D, 0x46, 0xD8};  // lea r8,[r14-28h]
            std::memcpy(stub, commandLoad, sizeof(commandLoad));
            at = sizeof(commandLoad);
        }
        stub[at]     = 0x48;  // mov rax,imm64
        stub[at + 1] = 0xB8;
        PutU64(stub + at + 2, bridges[index]);
        stub[at + 10] = 0xFF;  // jmp rax
        stub[at + 11] = 0xE0;

        auto* function = page + RelayFunctionTableOffset + index * 12;
        PutU32(function, static_cast<std::uint32_t>(RelayStubOffsets[index]));
        PutU32(function + 4, static_cast<std::uint32_t>(RelayStubOffsets[index] + RelayStubSizes[index]));
        PutU32(function + 8, static_cast<std::uint32_t>(RelayUnwindOffset));
    }
}

auto EnsureRelay() noexcept -> bool {
    if (RelayPage != nullptr) {
        return true;
    }
    auto* page = AllocateRelayPage();
    if (page == nullptr) {
        LogError("LeftClickAuras: no free page within call range of the game image.");
        return false;
    }
    std::memset(page, 0xCC, RelayPageBytes);
    std::memset(page, 0, RelayStubOffsets[0]);
    EmitRelay(page);

    DWORD previous = 0;
    if (!VirtualProtect(page, RelayPageBytes, PAGE_EXECUTE_READ, &previous)) {
        LogError("LeftClickAuras: could not make the relay page executable.");
        VirtualFree(page, 0, MEM_RELEASE);
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), page, RelayPageBytes);

    RelayUnwindRegistered = RtlAddFunctionTable(reinterpret_cast<PRUNTIME_FUNCTION>(page + RelayFunctionTableOffset),
                                                static_cast<DWORD>(RelayStubCount),
                                                static_cast<DWORD64>(reinterpret_cast<std::uintptr_t>(page)))
                         != FALSE;
    if (!RelayUnwindRegistered) {
        LogWarn("LeftClickAuras: unwind data for the relay page was not registered; stack walks stop there.");
    }
    RelayPage = page;
    return true;
}

auto RestoreCallSite(CallSitePatch& site) noexcept -> bool {
    if (!site.applied) {
        return true;
    }
    if (!Context->PatchBytes(site.rva, site.written.data(), 5, site.stock, 5)) {
        LogError("LeftClickAuras: could not restore the call at RVA 0x%llX.", static_cast<unsigned long long>(site.rva));
        return false;
    }
    site.applied = false;
    return true;
}

auto PatchCallSite(CallSitePatch& site) noexcept -> bool {
    const auto from = Base + static_cast<std::uintptr_t>(site.rva) + 5;
    const auto to   = reinterpret_cast<std::uintptr_t>(RelayPage) + RelayStubOffsets[site.stub];
    const auto rel  = static_cast<std::int64_t>(to) - static_cast<std::int64_t>(from);
    if (rel < INT32_MIN || rel > INT32_MAX) {
        LogError("LeftClickAuras: the relay page is out of reach of RVA 0x%llX.",
                 static_cast<unsigned long long>(site.rva));
        return false;
    }
    site.written[0] = 0xE8;
    const auto rel32 = static_cast<std::int32_t>(rel);
    std::memcpy(site.written.data() + 1, &rel32, sizeof(rel32));

    if (!Context->PatchBytes(site.rva, site.stock, 5, site.written.data(), 5)) {
        LogError("LeftClickAuras: the loader refused the call at RVA 0x%llX.", static_cast<unsigned long long>(site.rva));
        return false;
    }
    site.applied = true;
    if (std::memcmp(reinterpret_cast<const std::uint8_t*>(Base + static_cast<std::uintptr_t>(site.rva)),
                    site.written.data(), 5) != 0) {
        LogError("LeftClickAuras: RVA 0x%llX does not hold the bytes that were written.",
                 static_cast<unsigned long long>(site.rva));
        RestoreCallSite(site);
        return false;
    }
    return true;
}

void InstallServer() noexcept {
    if (!ValidateServer()) {
        LogError("LeftClickAuras: left_slot_auras is off, the game image does not match.");
        return;
    }
    if (!EnsureRelay()) {
        LogError("LeftClickAuras: left_slot_auras is off.");
        return;
    }

    auto& selectSite   = CallSites[StubLeftSelect];
    auto& startSite    = CallSites[StubAuraStartArm];
    auto& statListSite = CallSites[StubRightAuraStatList];
    auto& stateOffSite = CallSites[StubRightAuraStateOff];
    const auto restoreServerSites = [&]() noexcept {
        RestoreCallSite(selectSite);
        RestoreCallSite(stateOffSite);
        RestoreCallSite(statListSite);
        RestoreCallSite(startSite);
        Installed.selectRelay = Installed.startRelay = Installed.rightRemovalRelays = false;
    };

    // Stopping comes before starting: the failure and timer hooks and the
    // relays that keep or remove states go in first, the left select last.
    Installed.failureHook = Context->InstallInlineHook(RevalidationFailureRva, RevalidationFailureEntry.data(),
                                                       RevalidationFailureHookBytes, &HookRevalidationFailure,
                                                       &OriginalRevalidationFailure)
                         && OriginalRevalidationFailure != nullptr;
    Installed.timerHook = Installed.failureHook
                       && Context->InstallInlineHook(PeriodicSkillEventRva, PeriodicSkillEventWindow.data(),
                                                     PeriodicSkillEventHookBytes, &HookPeriodicSkillEvent,
                                                     &OriginalPeriodicSkillEvent)
                       && OriginalPeriodicSkillEvent != nullptr;
    if (!Installed.timerHook) {
        // A hook already in place only passes through while ServerActive is false.
        LogError("LeftClickAuras: left_slot_auras is off, a game function is already hooked by another plugin "
                 "(timer failure %s, timer %s).",
                 Installed.failureHook ? "hooked" : "refused", Installed.timerHook ? "hooked" : "refused");
        return;
    }

    Installed.startRelay         = PatchCallSite(startSite);
    Installed.rightRemovalRelays = Installed.startRelay && PatchCallSite(statListSite) && PatchCallSite(stateOffSite);
    Installed.selectRelay        = Installed.rightRemovalRelays && PatchCallSite(selectSite);
    if (!Installed.selectRelay) {
        restoreServerSites();
        LogError("LeftClickAuras: left_slot_auras is off, a call site could not be changed.");
        return;
    }
    ServerActive.store(true, std::memory_order_relaxed);

    if (Config.singleEmanation) {
        Installed.performRelay = PatchCallSite(CallSites[StubLeftTimerPerform]);
        if (Installed.performRelay) {
            PerformRelayActive.store(true, std::memory_order_relaxed);
        } else {
            LogError("LeftClickAuras: single_emanation is off, its call site could not be changed.");
        }
    }
}

void InstallClient() noexcept {
    if (!ValidateClient()) {
        LogError("LeftClickAuras: move_only_clicks is off, the game image does not match.");
        return;
    }
    if (!EnsureRelay()) {
        LogError("LeftClickAuras: move_only_clicks is off.");
        return;
    }
    Installed.clickRelay = PatchCallSite(CallSites[StubClickGate]);
    if (Installed.clickRelay) {
        ClientActive.store(true, std::memory_order_relaxed);
    } else {
        LogError("LeftClickAuras: move_only_clicks is off, its call site could not be changed.");
    }
}

// ---------------------------------------------------------------------------
//  Console
// ---------------------------------------------------------------------------
auto OnOff(bool value) noexcept -> const char* {
    return value ? "on" : "off";
}

auto StatusCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept
    -> D2RL::ConsoleCommandResult {
    if (command == nullptr || command->plugin == nullptr) {
        return D2RL::ConsoleCommandResult::Failed;
    }

    char line[512]{};
    std::snprintf(line, sizeof(line),
                  "Left Click Auras %s (%s): left slot auras %s [left select %s, start %s, right removal %s, "
                  "timer %s, timer failure %s]; single emanation %s; move only clicks %s; diagnostics %s.",
                  PluginVersion, ConfigSource, OnOff(ServerActive.load(std::memory_order_relaxed)),
                  OnOff(Installed.selectRelay), OnOff(Installed.startRelay), OnOff(Installed.rightRemovalRelays),
                  OnOff(Installed.timerHook), OnOff(Installed.failureHook),
                  OnOff(PerformRelayActive.load(std::memory_order_relaxed)),
                  OnOff(ClientActive.load(std::memory_order_relaxed)), OnOff(Config.diagnostics));
    command->plugin->WriteConsoleMessage(line);

    std::snprintf(line, sizeof(line),
                  "Left auras: started %llu, not started by the game %llu, kept running %llu. Stopped: left button "
                  "changed %llu, off the left button %llu, timer ended %llu. Shared states kept %llu.",
                  static_cast<unsigned long long>(LeftStarts.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(LeftStartsRefused.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(LeftKeptRunning.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(LeftStopsReselect.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(LeftStopsOffButton.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(LeftStopsFailed.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(SharedStatesKept.load(std::memory_order_relaxed)));
    command->plugin->WriteConsoleMessage(line);

    std::snprintf(line, sizeof(line), "Emanations left to the right button: %llu. Move only clicks: %llu.",
                  static_cast<unsigned long long>(EmanationsSkipped.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(MoveOnlyClicks.load(std::memory_order_relaxed)));
    command->plugin->WriteConsoleMessage(line);
    return D2RL::ConsoleCommandResult::Handled;
}

void ResetState() noexcept {
    Config    = Settings{};
    Installed = InstallState{};
    ServerActive.store(false, std::memory_order_relaxed);
    PerformRelayActive.store(false, std::memory_order_relaxed);
    ClientActive.store(false, std::memory_order_relaxed);
    OriginalPeriodicSkillEvent  = nullptr;
    OriginalRevalidationFailure = nullptr;
    for (auto* counter : {&LeftStarts, &LeftStartsRefused, &LeftKeptRunning, &LeftStopsReselect,
                          &LeftStopsOffButton, &LeftStopsFailed, &SharedStatesKept, &EmanationsSkipped,
                          &MoveOnlyClicks}) {
        counter->store(0, std::memory_order_relaxed);
    }
    DiagnosticsBudget.store(64, std::memory_order_relaxed);
    AcquireSRWLockExclusive(&ArmedLeftTimerLock);
    ArmedLeftTimers.fill(ArmedLeftTimer{});
    ArmedLeftTimerCursor = 0;
    ReleaseSRWLockExclusive(&ArmedLeftTimerLock);
}

}  // namespace
}  // namespace CelestialRayOne::LeftClickAuras

namespace {

constexpr D2RL::PluginInfo Info{
    .infoSize    = D2RL::PluginInfoSize,
    .abiVersion  = D2RL_PLUGIN_ABI_VERSION,
    .id          = "celestialrayone.left-click-auras",
    .name        = "Left Click Auras",
    .version     = "1.0.1",
    .author      = "CelestialRayOne",
    .description = "Auras on the left mouse button run alongside the right button's aura.",
    .flags       = D2RL::PluginFlags::Shared | D2RL::PluginFlags::NativeHooks,
};

}  // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
    return &Info;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
    using namespace CelestialRayOne::LeftClickAuras;

    if (!D2RL::HasContext(context) || context->exeBase == 0) {
        return false;
    }
    Context = context;
    Base    = context->exeBase;
    ResetState();
    LoadConfig();

    if (!Config.enabled) {
        LogInfo("Left Click Auras %s loaded disabled by config; the game runs stock.", PluginVersion);
        return true;
    }

    ResolveNativeFunctions();
    if (Config.leftSlotAuras) {
        InstallServer();
    }
    if (Config.moveOnlyClicks) {
        InstallClient();
    }

    if (!context->RegisterConsoleCommand("leftclickauras", StatusCommand, "Show Left Click Auras status and counters.")) {
        LogWarn("LeftClickAuras: the leftclickauras console command could not be registered.");
    }

    LogInfo("Left Click Auras %s by CelestialRayOne: left slot auras %s, single emanation %s, move only clicks %s, "
            "build %s.",
            PluginVersion, OnOff(ServerActive.load(std::memory_order_relaxed)),
            OnOff(PerformRelayActive.load(std::memory_order_relaxed)),
            OnOff(ClientActive.load(std::memory_order_relaxed)),
            context->buildName != nullptr ? context->buildName : "unknown");
    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    using namespace CelestialRayOne::LeftClickAuras;
    if (Context == nullptr) {
        return;
    }
    ServerActive.store(false, std::memory_order_relaxed);
    PerformRelayActive.store(false, std::memory_order_relaxed);
    ClientActive.store(false, std::memory_order_relaxed);

    bool allRestored = true;
    for (auto& site : CallSites) {
        allRestored = RestoreCallSite(site) && allRestored;
    }
    if (allRestored && RelayUnwindRegistered && RelayPage != nullptr) {
        RtlDeleteFunctionTable(reinterpret_cast<PRUNTIME_FUNCTION>(RelayPage + RelayFunctionTableOffset));
        RelayUnwindRegistered = false;
    }
}
