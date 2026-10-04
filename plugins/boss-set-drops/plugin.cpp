// Boss Set Unique Drop
//
// Drops a named unique (uniqueitems.txt index) when the last monster of a
// configured set of monster ids dies, and replaces the game's hardcoded uber
// Tristram drop, which gives the last of the three uber bosses a RANDOM unique
// large charm. Vanilla has a single unique large charm, so that random pick is
// always the Hellfire Torch; a mod with several is not.
//
// Everything below was read out of the live D2RLoader 1.3.1 image (D2R
// 3.3.93847) in IDA before being relied on, and every byte the plugin checks
// was copied from that image. The four function entries that the
// RuffnecKk-D2RLoader-Suite corpus also documents match it byte for byte.
//
// ---------------------------------------------------------------------------
// The vanilla drop: QUESTSFX_UberPrimeEvil at 0x5A8450
// ---------------------------------------------------------------------------
//   monumod.txt questcomplete (umod 22) has a mode-change callback, 0x4A1480,
//   in the umod table at 0x2396210 (row 22, column 1, through the dispatcher
//   0x498F80). On a death mode change it switches on the monster class;
//   704, 705 and 709 go to 0x5A8450. That routine sets one flag per boss in
//   the game (704 -> +1A5FC, 705 -> +1A5F8, 709 -> +1A5F4), and once all three
//   are set it runs, with rbx = game and rsi = the boss that died:
//
//     5A84C2  83 BB FC A5 01 00 00   cmp  dword [rbx+1A5FCh], 0 (+F8h, +F4h)
//     5A84E9  BA 63 6D 32 20         mov  edx, 'cm2 '
//     5A84EE  48 89 6C 24 58         mov  [rsp+58h], rbp
//     5A84F3  48 8B CE               mov  rcx, rsi
//     5A84F6  E8 -> 34F390           set the boss's drop item code
//     5A84FB  33 ED                  xor  ebp, ebp
//     5A84FD  4C 8D 4C 24 50         lea  r9, [rsp+50h]          ; &item level
//     ...                            stack args from rbp (0) and -1
//     5A8519  44 8D 45 07            lea  r8d, [rbp+7]           ; quality 7
//     5A851D  E8 -> 43FBF0           drop an item at the boss
//     5A852A  E8 -> 46F310           RefillWideItemCharges(game, 0, item)
//     5A852F  BA 73 74 64 20         mov  edx, 'std '
//     5A853C  8B 83 40 01 00 00      mov  eax, [rbx+140h]        ; client count
//     5A8561  41 B8 02 00 00 00      mov  r8d, 2                 ; normal
//     5A8572  E8 -> 43FBF0           one Standard of Heroes per client
//
//   The unique is never named. 0x43FBF0 hands quality 7 and the base code to
//   the item generator, and the unique pick 0x443910 rolls among every
//   eligible unique of that base, weighted by rarity.
//
//   The patch: the two bytes at 5A84FD become EB 30, jmp 5A852F. The xor at
//   5A84FB has already run (the Standard loop stores rbp as its zero stack
//   arguments), the drop code is overwritten with 'std ' right after, and the
//   Standard of Heroes drops are untouched. Only the random unique and its
//   recharge call are skipped. The bytes go back on unload.
//
// ---------------------------------------------------------------------------
// Kill detection: the monster death-mode callback
// ---------------------------------------------------------------------------
//   Every monster death enters mode 0 through the monster mode table at
//   0x1D1A200: 0x20 bytes per mode, start callback first, so slot 0 holds
//   MONSTERMODE_DeathModeCallback 0x444F50(game, modeChange) next to 0x445970,
//   and mode 1 starts with 0x445A80 and 0x444F30. Nothing else references
//   0x444F50. At 444F75 it loads the dying unit from modeChange+08h, and at
//   444FBE it calls D2RCore (j_D2RCore_1370), which runs the normal drop and
//   raises D2RLoader's monster-death event. D2RCore builds that event from the
//   same two pointers (game id = word game+08h, class = unit+04h, GUID =
//   unit+08h) but hands plugins only the numbers, and a GUID alone does not
//   lead back to the unit: SERVER_GetUnitByIdAndType 0x9A5A0 found nothing
//   for the dead uber Baal. So the plugin takes the pointers where D2RCore
//   does: slot 0 is swapped (compare-exchange) for a wrapper that calls what
//   the slot held, 0x444F50 or an entry hook another plugin put on it, and
//   then counts the kill. Any monster id works, not only the three with the
//   questcomplete monumod.
//
// ---------------------------------------------------------------------------
// The replacement drop: 0x43FBF0 step for step, plus the unique row
// ---------------------------------------------------------------------------
//   0x43FBF0 (game, unit, quality, int* itemLevel, out request, a6, a7):
//     item level   monster: stat 12 (level) through 0x2F5020, at least 1
//     base         0x313EF0(game+106h data context, drop code), -1 = unknown
//     origin       0x34A040(unit, &coords), x low dword, y high dword
//     room         UNITS_GetRoom 0x34B440
//     position     (x+2, y+3) when 0x38DFB0(room, x+2, y+3) finds it, else (x, y)
//     free spot    0x3632F0(room, &position, &origin, game+124h ? 3 : 1,
//                  3E01h, 801h, 1) returns the drop room, position in/out
//     request      0xC8 bytes, zero from +44h:
//                  +00 unit  +08 seed 0  +10 game  +18 item level
//                  +20 item id  +24 spawn type 3  +28 x  +2C y  +30 room
//                  +38 word 1  +3A word game+128h  +3C force 0  +40 quality
//     create       0x43CD50(game, &request, 0)
//
//   The one addition is the field a treasureclass entry naming a unique sets:
//   request+50h = uniqueitems row + 1. In 0x443910 every row is tested on its
//   code (+28h against the base's items code), version (+24h), flags word
//   (+2Ch), ladder seasons (+8Ch, +8Eh) and lvl (signed word +38h, against the
//   item level). When request+50h - 1 is the row being tested and it passed,
//   the row is taken at once, without the rarity roll and without the
//   spawnable bit or dropconditioncalc that random candidates also need. Then
//   the once-per-game rule: a row already marked in the game's bitfield at
//   game+3638h fails the pick (the item falls back to rare or magic), and a
//   row without nolimit (flags bit 1) is marked. A row that does not pass the
//   tests is skipped, and the pick rolls among the other uniques of the base,
//   so the plugin raises the item level to the unique's lvl when it is lower.
//
//   Afterwards the plugin calls 0x46F310 (RefillWideItemCharges) with (game,
//   null, item), the same call 5A852A makes, and checks the result with
//   ITEMS_GetItemQuality 0x36CF60 and the file index getter 0x36CE70
//   (ItemData+34h), both of which 0x443910 calls. 0x2F5020 and 0x46F310 are
//   the game entries D2RCore routes to ReadWideUnitStat and
//   RefillWideItemCharges, so the plugin calls exactly what the game calls.
//
// ---------------------------------------------------------------------------
// uniqueitems.txt, compiled by 0x31F1D0
// ---------------------------------------------------------------------------
//   Records of 348 bytes: +00 word row, +02 index (31 chars + NUL), +22 string
//   id, +24 version, +28 code, +2C flags (spawnable bit 0, nolimit 1,
//   disabled 2, disablechronicle 3), +34 rarity, +38 lvl, +3A lvl req,
//   +8C/+8E first/last ladder season, +90 dropconditioncalc, properties from
//   +98. The live rows are at DataTables+13C8h with a qword count at +13D0h,
//   DataTables from GetDataTablesForContext 0x300A90(game+106h), exactly as
//   0x443910 reads them. The plugin finds the configured name there at drop
//   time, so the row always comes from the table the game is using.
//
// ---------------------------------------------------------------------------
// Verification before anything is installed
// ---------------------------------------------------------------------------
//   Exact bytes of the live image, compared through CheckExpectedBytes:
//   - 5A84C2..5A8577, the whole flag check and drop block listed above;
//   - in 0x43FBF0 the level read at 43FC88, the base lookup at 43FDDA and the
//     position, request and create block 43FE10..43FF3B;
//   - in 0x443910 the table read at 443A70 and the lvl and row test at 443C5D;
//   - the entries of 0x34B440, 0x36CE70 and 0x36CF60;
//   - the mode table slot and its neighbours, and 444F75..444FBE in 0x444F50.
//   Every function the plugin calls is pinned by one of these, by its entry
//   or by the exact call the game itself makes to it, so a moved function is
//   caught before it is ever called. Any failure leaves the game untouched
//   and says why in the plugin log.

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
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace CelestialRayOne::BossSetUniqueDrop {
namespace {

// ---------------------------------------------------------------------------
// Native anchors
// ---------------------------------------------------------------------------

// QUESTSFX_UberPrimeEvil: the checked block, and the patch inside it.
constexpr std::uint64_t UberDropBlockRva     = 0x5A84C2;   // first flag test .. last Standard drop call
constexpr std::uint64_t UberCharmDropSiteRva = 0x5A84FD;   // lea r9, [rsp+50h]   <- patched
constexpr std::uint64_t UberStandardCodeRva  = 0x5A852F;   // mov edx, 'std '     <- jump target

// 0x43FBF0 (drop an item at a unit): the blocks checked.
constexpr std::uint64_t DropLevelRva         = 0x43FC88;
constexpr std::uint64_t DropItemIdRva        = 0x43FDDA;
constexpr std::uint64_t DropPlaceRva         = 0x43FE10;

// 0x443910 (unique pick): the blocks checked.
constexpr std::uint64_t PickTablesRva        = 0x443A70;
constexpr std::uint64_t PickRowRva           = 0x443C5D;

// The monster mode table: 0x20 bytes per mode, start callback first.
constexpr std::uint64_t DeathModeSlotRva     = 0x1D1A200;  // mode 0 start, holds 0x444F50
constexpr std::uint64_t DeathModeSecondRva   = 0x445970;   // mode 0 +08h
constexpr std::uint64_t NeutralModeStartRva  = 0x445A80;   // mode 1 start
constexpr std::uint64_t NeutralModeSecondRva = 0x444F30;   // mode 1 +08h
constexpr std::uint64_t DeathModeUnitRva     = 0x444F75;   // mov rdi, [rdx+8] ...
constexpr std::size_t   ModeEntrySize        = 0x20;

// Functions, each pinned by its entry or by the exact call the game makes.
constexpr std::uint64_t SetDropItemCodeRva   = 0x34F390;
constexpr std::uint64_t DropItemAtUnitRva    = 0x43FBF0;
constexpr std::uint64_t RefillChargesRva     = 0x46F310;   // RefillWideItemCharges (D2RCore)
constexpr std::uint64_t UnitStatRva          = 0x2F5020;   // ReadWideUnitStat (D2RCore)
constexpr std::uint64_t ItemIdFromCodeRva    = 0x313EF0;
constexpr std::uint64_t UnitCoordsRva        = 0x34A040;
constexpr std::uint64_t UnitRoomRva          = 0x34B440;
constexpr std::uint64_t RoomFindCoordsRva    = 0x38DFB0;
constexpr std::uint64_t FreeDropCoordsRva    = 0x3632F0;
constexpr std::uint64_t CreateItemRva        = 0x43CD50;
constexpr std::uint64_t DataTablesRva        = 0x300A90;
constexpr std::uint64_t ItemLevelRva         = 0x36CCF0;
constexpr std::uint64_t DeathModeCallbackRva = 0x444F50;   // MONSTERMODE_DeathModeCallback
constexpr std::uint64_t ItemFileIndexRva     = 0x36CE70;
constexpr std::uint64_t ItemQualityRva       = 0x36CF60;

constexpr std::uint32_t CallSize = 5;

// The destination of the E8 rel32 call at callRva inside a witness block.
constexpr auto CallTarget(const std::uint8_t* block, std::uint64_t blockRva, std::uint64_t callRva) noexcept
        -> std::uint64_t {
    const auto at = static_cast<std::size_t>(callRva - blockRva);
    if (block[at] != 0xE8) return 0;
    const std::uint32_t raw = static_cast<std::uint32_t>(block[at + 1])
        | (static_cast<std::uint32_t>(block[at + 2]) << 8)
        | (static_cast<std::uint32_t>(block[at + 3]) << 16)
        | (static_cast<std::uint32_t>(block[at + 4]) << 24);
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(callRva + CallSize) + static_cast<std::int32_t>(raw));
}

// The patch: lea r9, [rsp+50h] -> jmp 5A852F over the unique charm drop.
constexpr std::uint8_t UberCharmDropOriginal[]{ 0x4C, 0x8D };
constexpr std::uint8_t UberCharmDropSkip[]{ 0xEB, 0x30 };

// 5A84C2..5A8577: the three flag tests, the 'cm2 ' unique drop and its
// recharge, the 'std ' code and the Standard of Heroes loop (see the header).
constexpr std::uint8_t UberDropBlockWitness[]{
    0x83, 0xBB, 0xFC, 0xA5, 0x01, 0x00, 0x00, 0x0F, 0x84, 0xB8, 0x00, 0x00,
    0x00, 0x83, 0xBB, 0xF8, 0xA5, 0x01, 0x00, 0x00, 0x0F, 0x84, 0xAB, 0x00,
    0x00, 0x00, 0x83, 0xBB, 0xF4, 0xA5, 0x01, 0x00, 0x00, 0x0F, 0x84, 0x9E,
    0x00, 0x00, 0x00, 0xBA, 0x63, 0x6D, 0x32, 0x20, 0x48, 0x89, 0x6C, 0x24,
    0x58, 0x48, 0x8B, 0xCE, 0xE8, 0x95, 0x6E, 0xDA, 0xFF, 0x33, 0xED, 0x4C,
    0x8D, 0x4C, 0x24, 0x50, 0x89, 0x6C, 0x24, 0x30, 0x48, 0x8B, 0xD6, 0xC7,
    0x44, 0x24, 0x28, 0xFF, 0xFF, 0xFF, 0xFF, 0x48, 0x8B, 0xCB, 0x48, 0x89,
    0x6C, 0x24, 0x20, 0x44, 0x8D, 0x45, 0x07, 0xE8, 0xCE, 0x76, 0xE9, 0xFF,
    0x4C, 0x8B, 0xC0, 0x33, 0xD2, 0x48, 0x8B, 0xCB, 0xE8, 0xE1, 0x6D, 0xEC,
    0xFF, 0xBA, 0x73, 0x74, 0x64, 0x20, 0x48, 0x8B, 0xCE, 0xE8, 0x54, 0x6E,
    0xDA, 0xFF, 0x8B, 0x83, 0x40, 0x01, 0x00, 0x00, 0x85, 0xC0, 0x7E, 0x3C,
    0x48, 0x89, 0x7C, 0x24, 0x60, 0x8B, 0xF8, 0x0F, 0x1F, 0x00, 0x89, 0x6C,
    0x24, 0x30, 0x4C, 0x8D, 0x4C, 0x24, 0x50, 0xC7, 0x44, 0x24, 0x28, 0xFF,
    0xFF, 0xFF, 0xFF, 0x41, 0xB8, 0x02, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xD6,
    0x48, 0x89, 0x6C, 0x24, 0x20, 0x48, 0x8B, 0xCB, 0xE8, 0x79, 0x76, 0xE9,
    0xFF,
};

// 43FC88: monster item level, call 2F5020(unit, 12, 0), then at least 1.
constexpr std::uint8_t DropLevelWitness[]{
    0x45, 0x33, 0xC0, 0x49, 0x8B, 0xCF, 0x41, 0x8D, 0x50, 0x0C, 0xE8, 0x89,
    0x53, 0xEB, 0xFF, 0x41, 0x8B, 0xCD, 0x3B, 0xC1, 0x0F, 0x4F, 0xC8,
};

// 43FDDA: base item, call 313EF0(game+106h, code), -1 = unknown code.
constexpr std::uint8_t DropItemIdWitness[]{
    0x0F, 0xB6, 0x8F, 0x06, 0x01, 0x00, 0x00, 0x8B, 0xD0, 0xE8, 0x08, 0x41,
    0xED, 0xFF, 0x8B, 0xD8, 0x83, 0xF8, 0xFF,
};

// 43FE10..43FF3B: 34A040 coords, 34B440 room, game+124h search size,
// 38DFB0 at (x+2, y+3), 3632F0(room, &pos, &origin, size, 3E01h, 801h, 1),
// the 0xC8-byte request and call 43CD50(game, &request, 0).
constexpr std::uint8_t DropPlaceWitness[]{
    0x48, 0x8D, 0x54, 0x24, 0x60, 0x49, 0x8B, 0xCF, 0xE8, 0x23, 0xA2, 0xF0,
    0xFF, 0x49, 0x8B, 0xCF, 0xE8, 0x1B, 0xB6, 0xF0, 0xFF, 0x83, 0xBF, 0x24,
    0x01, 0x00, 0x00, 0x00, 0x4C, 0x8B, 0xC8, 0x8B, 0x74, 0x24, 0x60, 0x49,
    0x8B, 0xC9, 0x48, 0x89, 0x44, 0x24, 0x68, 0xB8, 0x03, 0x00, 0x00, 0x00,
    0x41, 0x0F, 0x44, 0xC5, 0x44, 0x8B, 0x6C, 0x24, 0x64, 0x41, 0x83, 0xC5,
    0x03, 0x89, 0x44, 0x24, 0x44, 0x83, 0xC6, 0x02, 0x45, 0x8B, 0xC5, 0x8B,
    0xD6, 0xE8, 0x52, 0xE1, 0xF4, 0xFF, 0x48, 0x85, 0xC0, 0x74, 0x0B, 0x89,
    0x74, 0x24, 0x50, 0x44, 0x89, 0x6C, 0x24, 0x54, 0xEB, 0x0A, 0x48, 0x8B,
    0x44, 0x24, 0x60, 0x48, 0x89, 0x44, 0x24, 0x50, 0x44, 0x8B, 0x4C, 0x24,
    0x44, 0x4C, 0x8D, 0x44, 0x24, 0x60, 0x48, 0x8B, 0x4C, 0x24, 0x68, 0x48,
    0x8D, 0x54, 0x24, 0x50, 0xBE, 0x01, 0x00, 0x00, 0x00, 0x89, 0x74, 0x24,
    0x30, 0xC7, 0x44, 0x24, 0x28, 0x01, 0x08, 0x00, 0x00, 0xC7, 0x44, 0x24,
    0x20, 0x01, 0x3E, 0x00, 0x00, 0xE8, 0x46, 0x34, 0xF2, 0xFF, 0x48, 0x8B,
    0xC8, 0x48, 0x85, 0xC0, 0x0F, 0x84, 0x1D, 0x01, 0x00, 0x00, 0x33, 0xC0,
    0x48, 0x89, 0x4D, 0xA0, 0x0F, 0x57, 0xC0, 0x89, 0x45, 0x8C, 0x89, 0x45,
    0xAC, 0x48, 0x8D, 0x54, 0x24, 0x70, 0x89, 0x45, 0x34, 0x45, 0x33, 0xC0,
    0x48, 0x89, 0x44, 0x24, 0x78, 0x48, 0x8B, 0xCF, 0x0F, 0xB7, 0x87, 0x28,
    0x01, 0x00, 0x00, 0x66, 0x89, 0x45, 0xAA, 0x8B, 0x44, 0x24, 0x50, 0x89,
    0x45, 0x98, 0x8B, 0x44, 0x24, 0x54, 0x89, 0x45, 0x9C, 0x8B, 0x44, 0x24,
    0x48, 0x89, 0x45, 0xB0, 0x41, 0x8B, 0x04, 0x24, 0x89, 0x45, 0x88, 0x0F,
    0x11, 0x45, 0xB4, 0x4C, 0x89, 0x7C, 0x24, 0x70, 0x0F, 0x11, 0x45, 0xC4,
    0x48, 0x89, 0x7D, 0x80, 0x0F, 0x11, 0x45, 0xD4, 0x89, 0x5D, 0x90, 0x0F,
    0x11, 0x45, 0xE4, 0xC7, 0x45, 0x94, 0x03, 0x00, 0x00, 0x00, 0x0F, 0x11,
    0x45, 0xF4, 0x66, 0x89, 0x75, 0xA8, 0x0F, 0x11, 0x45, 0x04, 0x0F, 0x11,
    0x45, 0x14, 0x0F, 0x11, 0x45, 0x24, 0xE8, 0x15, 0xCE, 0xFF, 0xFF,
};

// 443A70: call 300A90(context), rows at +13C8h, count at +13D0h.
constexpr std::uint8_t PickTablesWitness[]{
    0xE8, 0x1B, 0xD0, 0xEB, 0xFF, 0x48, 0x89, 0x44, 0x24, 0x38, 0x48, 0x8B,
    0x98, 0xC8, 0x13, 0x00, 0x00, 0x48, 0x8B, 0x88, 0xD0, 0x13, 0x00, 0x00,
};

// 443C5D: call 36CCF0 item level, movsx lvl (row+38h), jg skip, then
// mov eax, [request+50h] / test / jz / dec / movsxd / cmp with the row / je take.
constexpr std::uint8_t PickRowWitness[]{
    0xE8, 0x8E, 0x90, 0xF2, 0xFF, 0x0F, 0xBF, 0x4B, 0x14, 0x3B, 0xC8, 0x0F,
    0x8F, 0x94, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x8C, 0x24, 0x90, 0x00, 0x00,
    0x00, 0x8B, 0x41, 0x50, 0x85, 0xC0, 0x74, 0x16, 0xFF, 0xC8, 0x48, 0x63,
    0xC8, 0x48, 0x3B, 0xCF, 0x0F, 0x84, 0x6B, 0x02, 0x00, 0x00,
};

// 444F75..444FBE in 0x444F50: mov rdi, [rdx+8] (the dying unit), mov rbx, rdx,
// mov rdx, rcx, mov rsi, rcx, the game calls 48FF40, 491130 and 448A30, then
// rcx = game and rdx = modeChange for D2RCore's handler at 444FBE.
constexpr std::uint8_t DeathModeUnitWitness[]{
    0x48, 0x8B, 0x7A, 0x08, 0x48, 0x8B, 0xDA, 0x48, 0x8B, 0xD1, 0x48, 0x8B,
    0xF1, 0x4C, 0x8B, 0xC7, 0x48, 0x8D, 0x4D, 0xA0, 0xE8, 0xB2, 0xAF, 0x04,
    0x00, 0x45, 0x33, 0xC0, 0x48, 0x8B, 0xD7, 0x48, 0x8B, 0xCE, 0xF2, 0x0F,
    0x10, 0x00, 0xF2, 0x0F, 0x11, 0x45, 0x88, 0xE8, 0x8B, 0xC1, 0x04, 0x00,
    0x44, 0x0F, 0xB6, 0x43, 0x20, 0x48, 0x8B, 0xD7, 0x48, 0x8B, 0xCE, 0xE8,
    0x7B, 0x3A, 0x00, 0x00, 0x45, 0x33, 0xC0, 0x48, 0x8B, 0xD3, 0x48, 0x8B,
    0xCE,
};

// 0x34B440 UNITS_GetRoom(unit): the room through the unit's path.
constexpr std::uint8_t UnitRoomWitness[]{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9,
    0x75, 0x13, 0x88, 0x4C, 0x24, 0x30, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8,
    0x54, 0xA7, 0xFF, 0xFF, 0x84, 0xC0, 0x74, 0x01, 0xCC, 0x8B, 0x0B, 0x83,
    0xE9, 0x02, 0x74, 0x25, 0x83, 0xE9, 0x02, 0x74, 0x20, 0x83, 0xF9, 0x01,
    0x74, 0x1B, 0x48, 0x8B, 0x4B, 0x38, 0x48, 0x85, 0xC9, 0x74, 0x0A, 0x48,
    0x83, 0xC4, 0x20, 0x5B, 0xE9, 0xAB, 0x67, 0xFF, 0xFF, 0x33, 0xC0, 0x48,
    0x83, 0xC4, 0x20, 0x5B, 0xC3, 0x48, 0x8B, 0x43, 0x38, 0x48, 0x8B, 0x00,
    0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3,
};

// 0x36CE70: an item's file index (unique row for a unique), ItemData+34h.
constexpr std::uint8_t ItemFileIndexWitness[]{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9,
    0x75, 0x13, 0x88, 0x4C, 0x24, 0x30, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8,
    0xB4, 0xB7, 0xFF, 0xFF, 0x84, 0xC0, 0x74, 0x01, 0xCC, 0x48, 0x8B, 0xCB,
    0xE8, 0x37, 0xEB, 0xFD, 0xFF, 0x83, 0xF8, 0x04, 0x74, 0x14, 0x48, 0x8D,
    0x4C, 0x24, 0x30, 0xC6, 0x44, 0x24, 0x30, 0x00, 0xE8, 0x63, 0xBA, 0xFF,
    0xFF, 0x84, 0xC0, 0x74, 0x01, 0xCC, 0x48, 0x8B, 0xCB, 0xE8, 0x46, 0xD6,
    0xFD, 0xFF, 0x48, 0x8B, 0xD8, 0x48, 0x85, 0xC0, 0x75, 0x1C, 0x48, 0x8D,
    0x4C, 0x24, 0x30, 0x88, 0x44, 0x24, 0x30, 0xE8, 0x20, 0xAD, 0xFF, 0xFF,
    0x84, 0xC0, 0x74, 0x01, 0xCC, 0x8B, 0x43, 0x34, 0x48, 0x83, 0xC4, 0x20,
    0x5B, 0xC3, 0x8B, 0x40, 0x34, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3,
};

// 0x36CF60 ITEMS_GetItemQuality(item).
constexpr std::uint8_t ItemQualityWitness[]{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9,
    0x74, 0x0A, 0xE8, 0x5D, 0xEA, 0xFD, 0xFF, 0x83, 0xF8, 0x04, 0x74, 0x19,
    0x48, 0x8D, 0x4C, 0x24, 0x30, 0xC6, 0x44, 0x24, 0x30, 0x00, 0xE8, 0x09,
    0xA6, 0xFF, 0xFF, 0x84, 0xC0, 0x74, 0x01, 0xCC, 0x48, 0x85, 0xDB, 0x74,
    0x2D, 0x48, 0x8B, 0xCB, 0xE8, 0x37, 0xEA, 0xFD, 0xFF, 0x83, 0xF8, 0x04,
    0x75, 0x20, 0x48, 0x8B,
};

static_assert(CallTarget(UberDropBlockWitness, UberDropBlockRva, 0x5A84F6) == SetDropItemCodeRva);
static_assert(CallTarget(UberDropBlockWitness, UberDropBlockRva, 0x5A851D) == DropItemAtUnitRva);
static_assert(CallTarget(UberDropBlockWitness, UberDropBlockRva, 0x5A852A) == RefillChargesRva);
static_assert(CallTarget(UberDropBlockWitness, UberDropBlockRva, 0x5A8537) == SetDropItemCodeRva);
static_assert(CallTarget(UberDropBlockWitness, UberDropBlockRva, 0x5A8572) == DropItemAtUnitRva);
static_assert(CallTarget(DropLevelWitness, DropLevelRva, 0x43FC92) == UnitStatRva);
static_assert(CallTarget(DropItemIdWitness, DropItemIdRva, 0x43FDE3) == ItemIdFromCodeRva);
static_assert(CallTarget(DropPlaceWitness, DropPlaceRva, 0x43FE18) == UnitCoordsRva);
static_assert(CallTarget(DropPlaceWitness, DropPlaceRva, 0x43FE20) == UnitRoomRva);
static_assert(CallTarget(DropPlaceWitness, DropPlaceRva, 0x43FE59) == RoomFindCoordsRva);
static_assert(CallTarget(DropPlaceWitness, DropPlaceRva, 0x43FEA5) == FreeDropCoordsRva);
static_assert(CallTarget(DropPlaceWitness, DropPlaceRva, 0x43FF36) == CreateItemRva);
static_assert(CallTarget(PickTablesWitness, PickTablesRva, 0x443A70) == DataTablesRva);
static_assert(CallTarget(PickRowWitness, PickRowRva, 0x443C5D) == ItemLevelRva);
static_assert(CallTarget(DeathModeUnitWitness, DeathModeUnitRva, 0x444F89) == 0x48FF40);
static_assert(CallTarget(DeathModeUnitWitness, DeathModeUnitRva, 0x444FA0) == 0x491130);
static_assert(CallTarget(DeathModeUnitWitness, DeathModeUnitRva, 0x444FB0) == 0x448A30);
static_assert(DeathModeUnitWitness[0] == 0x48 && DeathModeUnitWitness[1] == 0x8B && DeathModeUnitWitness[2] == 0x7A
    && DeathModeUnitWitness[3] == 0x08);
static_assert(DeathModeSlotRva % 8 == 0);
static_assert(UberCharmDropSiteRva + sizeof(UberCharmDropSkip) + UberCharmDropSkip[1] == UberStandardCodeRva);
static_assert(UberDropBlockWitness[UberCharmDropSiteRva - UberDropBlockRva] == 0x4C
    && UberDropBlockWitness[UberCharmDropSiteRva - UberDropBlockRva + 1] == 0x8D
    && UberDropBlockWitness[UberCharmDropSiteRva - UberDropBlockRva + 2] == 0x4C
    && UberDropBlockWitness[UberCharmDropSiteRva - UberDropBlockRva + 3] == 0x24
    && UberDropBlockWitness[UberCharmDropSiteRva - UberDropBlockRva + 4] == 0x50);
static_assert(UberDropBlockWitness[UberStandardCodeRva - UberDropBlockRva] == 0xBA
    && UberDropBlockWitness[UberStandardCodeRva - UberDropBlockRva + 1] == 0x73);

// ---------------------------------------------------------------------------
// Native layouts
// ---------------------------------------------------------------------------

constexpr std::size_t   UnitTypeOffset          = 0x00;
constexpr std::size_t   UnitClassOffset         = 0x04;
constexpr std::uint32_t UnitTypeMonster         = 1;

constexpr std::size_t   GameIdOffset            = 0x08;    // word, the id D2RCore reports
constexpr std::size_t   ModeChangeUnitOffset    = 0x08;
constexpr std::size_t   GameDataContextOffset   = 0x106;
constexpr std::size_t   GameDropSearchOffset    = 0x124;
constexpr std::size_t   GameItemFormatOffset    = 0x128;

constexpr std::size_t   TablesUniqueRowsOffset  = 0x13C8;
constexpr std::size_t   TablesUniqueCountOffset = 0x13D0;
constexpr std::size_t   UniqueRowSize           = 348;
constexpr std::size_t   UniqueIndexOffset       = 0x02;
constexpr std::size_t   UniqueIndexCapacity     = 32;
constexpr std::size_t   UniqueIndexMaxLength    = 31;
constexpr std::size_t   UniqueCodeOffset        = 0x28;
constexpr std::size_t   UniqueLevelOffset       = 0x38;
constexpr std::uint64_t UniqueRowLimit          = 0x7FFF;

constexpr std::int32_t  LevelStatId             = 12;
constexpr std::int32_t  QualityUnique           = 7;
constexpr std::int32_t  SpawnTypeGround         = 3;
constexpr std::int32_t  DropSearchMask          = 0x3E01;
constexpr std::int32_t  DropSearchField         = 0x801;
constexpr std::uint32_t InvalidItemId           = 0xFFFFFFFFU;

struct Coords {
    std::int32_t x;
    std::int32_t y;
};
static_assert(sizeof(Coords) == 8);

// The request 0x43FBF0 builds on its stack and hands to 0x43CD50.
struct ItemDrop {
    void*         unit;              // +00
    void*         seed;              // +08
    void*         game;              // +10
    std::int32_t  itemLevel;         // +18
    std::uint32_t reserved1C;        // +1C
    std::uint32_t itemId;            // +20
    std::int32_t  spawnType;         // +24
    std::int32_t  x;                 // +28
    std::int32_t  y;                 // +2C
    void*         room;              // +30
    std::uint16_t unitInitFlags;     // +38
    std::uint16_t itemFormat;        // +3A
    std::int32_t  force;             // +3C
    std::int32_t  quality;           // +40
    std::uint8_t  reserved44[0x0C];  // +44
    std::int32_t  itemIndex;         // +50  uniqueitems row + 1
    std::uint32_t flags1;            // +54
    std::uint8_t  reserved58[0x68];  // +58
    std::uint32_t flags2;            // +C0
    std::uint32_t reservedC4;        // +C4
};
static_assert(sizeof(ItemDrop) == 0xC8);
static_assert(offsetof(ItemDrop, game) == 0x10 && offsetof(ItemDrop, itemLevel) == 0x18);
static_assert(offsetof(ItemDrop, itemId) == 0x20 && offsetof(ItemDrop, spawnType) == 0x24);
static_assert(offsetof(ItemDrop, x) == 0x28 && offsetof(ItemDrop, y) == 0x2C);
static_assert(offsetof(ItemDrop, room) == 0x30 && offsetof(ItemDrop, unitInitFlags) == 0x38);
static_assert(offsetof(ItemDrop, itemFormat) == 0x3A && offsetof(ItemDrop, force) == 0x3C);
static_assert(offsetof(ItemDrop, quality) == 0x40 && offsetof(ItemDrop, itemIndex) == 0x50);
static_assert(offsetof(ItemDrop, flags1) == 0x54 && offsetof(ItemDrop, flags2) == 0xC0);

using DeathModeFn      = std::int32_t (*)(void* game, void* modeChange);
using UnitPointerFn    = void* (*)(void* unit);
using UnitCoordsFn     = void (*)(void* unit, Coords* coords);
using RoomFindCoordsFn = void* (*)(void* room, std::int32_t x, std::int32_t y);
using FreeDropCoordsFn = void* (*)(void* room, Coords* position, Coords* origin, std::int32_t size,
    std::int32_t mask, std::int32_t field, std::int32_t flag);
using CreateItemFn     = void* (*)(void* game, ItemDrop* request, std::int32_t useSeed);
using ItemIdFromCodeFn = std::uint32_t (*)(std::uint8_t dataContext, std::uint32_t code);
using DataTablesFn     = const std::uint8_t* (*)(std::uint8_t dataContext);
using ItemValueFn      = std::uint32_t (*)(void* item);
using ReadUnitStatFn   = std::int32_t (*)(void* unit, std::int32_t statId, std::int32_t layer);
using RefillChargesFn  = void (*)(void* game, void* player, void* item);

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

constexpr char          MainTable[]        = "boss_set_unique_drop";
constexpr char          RuleTable[]        = "boss_set_unique_drop.rule";
constexpr std::int32_t  MaximumMonsterId   = 0x7FFF;
constexpr std::size_t   MaximumRules       = 256;
constexpr std::size_t   MaximumSetSize     = 64;
constexpr std::size_t   InitialConfigBytes = 16'384;
constexpr std::size_t   MaximumConfigBytes = 1'048'576;
constexpr std::size_t   WatchedBytes       = (MaximumMonsterId + 1) / 8;

constexpr char DefaultConfigToml[] =
    "# Boss Set Unique Drop\n"
    "#\n"
    "# Drops a named unique when the LAST monster of a configured set dies.\n"
    "#\n"
    "# Each [[boss_set_unique_drop.rule]] block is one set: a list of monster ids\n"
    "# and a unique. Once every monster in the list has died in the same game, in\n"
    "# any order, the unique drops at the monster that died last, the way the game\n"
    "# drops the Hellfire Torch from the last of the three uber bosses.\n"
    "#\n"
    "# This replaces the game's own uber Tristram drop. Vanilla never names the\n"
    "# Torch: it drops \"a random unique large charm\", which in a mod with several\n"
    "# unique large charms can be any of them. While this plugin is enabled that\n"
    "# random charm no longer drops, and the uber bosses drop what the rules below\n"
    "# say. The Standard of Heroes they drop for each player is not touched.\n"
    "#\n"
    "# Kills are counted for every monster, not only the uber bosses, so any set\n"
    "# of bosses works.\n"
    "#\n"
    "# How the unique drops\n"
    "#   - Like a treasureclass entry that names a unique: the game builds that\n"
    "#     exact unique on its base item, at the killed monster's position.\n"
    "#   - Item level is the killed monster's level, raised to the unique's lvl\n"
    "#     when it is lower, so the level can never stop the named unique.\n"
    "#   - The game's other unique rules still apply. A unique drops once per game\n"
    "#     unless its nolimit column is set; completing a set again in the same\n"
    "#     game then gives a magic or rare item of that base instead, exactly what\n"
    "#     vanilla does with a second Torch. The row must also pass the game's own\n"
    "#     checks on its code, version, flag and ladder season columns.\n"
    "#\n"
    "# Progress\n"
    "#   - Counted per game, in memory only. Leaving the game clears it.\n"
    "#   - After a set drops its unique it starts over, so killing the whole set\n"
    "#     again in the same game counts again.\n"
    "#   - A monster can be in several sets; each set counts its kill.\n"
    "#\n"
    "# Server side only. Single player hosts its own server, so it applies there.\n"
    "#\n"
    "# Console command: bossdrop (status, every set, progress in the current game)\n"
    "#\n"
    "# Each unique name is looked up in uniqueitems.txt as soon as the game loads\n"
    "# its tables, and the result is written to the plugin log.\n"
    "\n"
    "[boss_set_unique_drop]\n"
    "\n"
    "# Master switch. false installs nothing, and the uber bosses keep the game's\n"
    "# own random unique large charm drop.\n"
    "enabled = true\n"
    "\n"
    "# One block per set:\n"
    "#   monsters  monstats.txt hcIdx values (the row id), 0 to 32767, on one line\n"
    "#             or several, # comments allowed. Repeating an id changes nothing.\n"
    "#   unique    the uniqueitems.txt index column, exactly as written there, in\n"
    "#             double quotes (single quotes when the name contains a \").\n"
    "#             31 characters at most, the length the game keeps.\n"
    "#\n"
    "# Any mistake in any block (an id that is not a number, a missing unique, ...)\n"
    "# stops the plugin from installing and is reported in the plugin log, so a\n"
    "# typo can never make a set smaller than intended. The game then keeps its\n"
    "# vanilla drop.\n"
    "\n"
    "# The uber Tristram set: Uber Mephisto, Uber Diablo, Uber Baal.\n"
    "[[boss_set_unique_drop.rule]]\n"
    "monsters = [704, 705, 709]\n"
    "unique = \"Hellfire Torch\"\n"
    "\n"
    "# Another set, written over several lines (remove the leading # to use it):\n"
    "#\n"
    "# [[boss_set_unique_drop.rule]]\n"
    "# monsters = [\n"
    "#     1086,   # first boss\n"
    "#     1087,   # second boss\n"
    "#     1088,   # third boss\n"
    "# ]\n"
    "# unique = \"Your Unique Index\"\n";

struct Rule {
    int                       line = 0;
    std::vector<std::int32_t> monsters;
    std::string               unique;
};

struct RuleDraft {
    int                       line = 0;
    std::vector<std::int32_t> monsters;
    bool                      monstersSet = false;
    std::string               unique;
    bool                      uniqueSet = false;
    bool                      broken = false;
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

enum class InstallState : std::uint8_t {
    NotAttempted,
    DisabledByConfig,
    ConfigError,
    ServiceUnavailable,
    UnsupportedBuild,
    InstallFailed,
    Armed,
};

struct RuleRuntime {
    std::uint64_t completions = 0;
    std::uint64_t dropped     = 0;
    std::string   tableCheck  = "not checked yet (tables not loaded)";
    std::string   lastOutcome = "none yet";
};

struct GameProgress {
    std::vector<std::vector<std::uint8_t>> killed;
    std::vector<std::uint32_t>             count;
};

const D2RL::PluginContext*              Context{};
std::uintptr_t                          Base{};
bool                                    Enabled = true;
bool                                    ConfigFileWasRead{};
std::vector<Rule>                       Rules;
std::array<std::uint8_t, WatchedBytes>  Watched{};
std::atomic<InstallState>               State{ InstallState::NotAttempted };
std::atomic<bool>                       Armed{};
bool                                    SitePatched{};

const D2RL::LifecycleService*           LifecycleApi{};
const D2RL::DataTableService*           DataTableApi{};
D2RL::Lifecycle::ListenerHandle         JoinListener{};
D2RL::Lifecycle::ListenerHandle         LeaveListener{};
D2RL::Lifecycle::ListenerHandle         TablesListener{};

std::mutex                              ProgressMutex;
std::unordered_map<std::uint32_t, GameProgress> Progress;
std::vector<RuleRuntime>                Runtime;

DeathModeFn      OriginalDeathMode{};
bool             SlotPatched{};
UnitPointerFn    UnitRoom{};
UnitCoordsFn     UnitCoords{};
RoomFindCoordsFn RoomFindCoords{};
FreeDropCoordsFn FreeDropCoords{};
CreateItemFn     CreateItem{};
ItemIdFromCodeFn ItemIdFromCode{};
DataTablesFn     GetDataTables{};
ItemValueFn      ItemQuality{};
ItemValueFn      ItemFileIndex{};
ReadUnitStatFn   ReadUnitStat{};
RefillChargesFn  RefillCharges{};

template <typename T>
auto ReadAt(const void* base, std::size_t offset) noexcept -> T {
    T value{};
    std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(T));
    return value;
}

template <typename Function>
auto At(std::uint64_t rva) noexcept -> Function {
    return reinterpret_cast<Function>(Base + rva);
}

auto IsWatched(std::uint32_t monsterId) noexcept -> bool {
    if (monsterId > static_cast<std::uint32_t>(MaximumMonsterId)) return false;
    return ((Watched[monsterId >> 3] >> (monsterId & 7U)) & 1U) != 0;
}

void MarkWatched(std::int32_t monsterId) noexcept {
    const auto index = static_cast<std::uint32_t>(monsterId);
    Watched[index >> 3] = static_cast<std::uint8_t>(Watched[index >> 3] | (1U << (index & 7U)));
}

// "704 705 709", stopping before `limit` characters.
auto JoinIds(const std::vector<std::int32_t>& ids, std::size_t limit) -> std::string {
    std::string out;
    char number[16]{};
    for (const std::int32_t id : ids) {
        const int length = std::snprintf(number, sizeof(number), out.empty() ? "%d" : " %d", id);
        if (length <= 0) continue;
        if (out.size() + static_cast<std::size_t>(length) + 4 > limit) {
            out += " ...";
            break;
        }
        out.append(number, static_cast<std::size_t>(length));
    }
    return out;
}

auto CodeText(std::uint32_t code) -> std::string {
    std::string text;
    for (int shift = 0; shift < 32; shift += 8) {
        const char character = static_cast<char>((code >> shift) & 0xFFU);
        if (character == '\0' || character == ' ') break;
        text.push_back(character >= 0x20 && character < 0x7F ? character : '?');
    }
    return text;
}

// ---------------------------------------------------------------------------
// Config parsing (small TOML subset, no external dependency)
// ---------------------------------------------------------------------------

auto Trim(std::string_view value) noexcept -> std::string_view {
    std::size_t first = 0;
    while (first < value.size()
            && (value[first] == ' ' || value[first] == '\t' || value[first] == '\r' || value[first] == '\n')) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first
            && (value[last - 1] == ' ' || value[last - 1] == '\t' || value[last - 1] == '\r'
                || value[last - 1] == '\n')) {
        --last;
    }
    return value.substr(first, last - first);
}

// Cuts a # comment, ignoring # inside "basic" and 'literal' strings.
auto StripComment(std::string_view line) noexcept -> std::string_view {
    bool inBasic = false;
    bool inLiteral = false;
    bool escaped = false;
    for (std::size_t index = 0; index < line.size(); ++index) {
        const char character = line[index];
        if (inBasic) {
            if (escaped) escaped = false;
            else if (character == '\\') escaped = true;
            else if (character == '"') inBasic = false;
            continue;
        }
        if (inLiteral) {
            if (character == '\'') inLiteral = false;
            continue;
        }
        if (character == '"') inBasic = true;
        else if (character == '\'') inLiteral = true;
        else if (character == '#') return line.substr(0, index);
    }
    return line;
}

auto WithoutSpaces(std::string_view text) -> std::string {
    std::string out;
    for (const char character : text) {
        if (character != ' ' && character != '\t') out.push_back(character);
    }
    return out;
}

int ConfigErrors = 0;

void ConfigError(int line, const char* format, ...) noexcept {
    char message[640]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    ++ConfigErrors;
    D2RL::LogErrorF(Context, "BossSetUniqueDrop: config line %d: %s", line, message);
}

void ConfigWarning(int line, const char* format, ...) noexcept {
    char message[640]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    D2RL::LogWarnF(Context, "BossSetUniqueDrop: config line %d: %s", line, message);
}

// Decimal, optional leading '+', '_' separators allowed as in TOML.
auto ParseMonsterId(std::string_view text, std::int32_t& out) noexcept -> bool {
    std::size_t index = 0;
    if (!text.empty() && text[0] == '+') index = 1;
    std::int32_t value = 0;
    bool anyDigit = false;
    for (; index < text.size(); ++index) {
        const char character = text[index];
        if (character == '_') continue;
        if (character < '0' || character > '9') return false;
        anyDigit = true;
        value = value * 10 + (character - '0');
        if (value > MaximumMonsterId) return false;
    }
    if (!anyDigit) return false;
    out = value;
    return true;
}

// The text between the brackets, lines already joined with commas.
void ApplyIdList(std::string_view text, RuleDraft& rule, int line) {
    std::size_t cursor = 0;
    while (cursor <= text.size()) {
        const std::size_t comma = text.find(',', cursor);
        const std::size_t end = comma == std::string_view::npos ? text.size() : comma;
        const std::string_view element = Trim(text.substr(cursor, end - cursor));
        if (!element.empty()) {
            std::int32_t monsterId = 0;
            if (!ParseMonsterId(element, monsterId)) {
                const std::string shown(element.substr(0, 24));
                ConfigError(line, "monsters entry '%s' is not a whole number from 0 to %d.",
                    shown.c_str(), MaximumMonsterId);
                rule.broken = true;
            } else if (std::find(rule.monsters.begin(), rule.monsters.end(), monsterId) != rule.monsters.end()) {
                ConfigWarning(line, "monster %d is listed twice in the same set; it counts once.", monsterId);
            } else {
                rule.monsters.push_back(monsterId);
            }
        }
        if (comma == std::string_view::npos) break;
        cursor = comma + 1;
    }
}

// "basic" (escapes \" \\ \t \n \r) or 'literal'. Nothing may follow it.
auto ParseString(std::string_view value, std::string& out, const char*& error) -> bool {
    out.clear();
    if (value.empty() || (value.front() != '"' && value.front() != '\'')) {
        error = "must be in quotes";
        return false;
    }
    std::size_t index = 1;
    bool closed = false;
    if (value.front() == '\'') {
        const std::size_t close = value.find('\'', 1);
        if (close != std::string_view::npos) {
            out.assign(value.substr(1, close - 1));
            index = close + 1;
            closed = true;
        }
    } else {
        for (; index < value.size(); ++index) {
            const char character = value[index];
            if (character == '"') {
                ++index;
                closed = true;
                break;
            }
            if (character != '\\') {
                out.push_back(character);
                continue;
            }
            if (++index >= value.size()) break;
            switch (value[index]) {
            case '"':  out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case 't':  out.push_back('\t'); break;
            case 'n':  out.push_back('\n'); break;
            case 'r':  out.push_back('\r'); break;
            default:
                error = "has an escape other than \\\" \\\\ \\t \\n \\r";
                return false;
            }
        }
    }
    if (!closed) {
        error = "is missing its closing quote";
        return false;
    }
    if (!Trim(value.substr(index)).empty()) {
        error = "has text after the closing quote";
        return false;
    }
    return true;
}

enum class Table : std::uint8_t { Root, Main, Rule, Other };

void ParseConfig(std::string_view text, std::vector<RuleDraft>& rules) {
    Table       table = Table::Root;
    bool        capturing = false;
    int         captureLine = 0;
    std::string arrayText;
    int         lineNumber = 0;

    std::size_t cursor = 0;
    while (cursor < text.size()) {
        std::size_t end = text.find('\n', cursor);
        if (end == std::string_view::npos) end = text.size();
        ++lineNumber;
        const std::string_view line = Trim(StripComment(text.substr(cursor, end - cursor)));
        cursor = end + 1;

        if (capturing) {
            if (!line.empty() && line.front() == '[') {
                ConfigError(captureLine, "monsters is missing its closing ']'.");
                rules.back().broken = true;
                capturing = false;
            } else {
                const std::size_t close = line.find(']');
                arrayText.append(line.substr(0, close));
                arrayText.push_back(',');
                if (close == std::string_view::npos) continue;
                capturing = false;
                if (!Trim(line.substr(close + 1)).empty()) {
                    ConfigError(lineNumber, "unexpected text after the closing ']' of monsters.");
                    rules.back().broken = true;
                }
                ApplyIdList(arrayText, rules.back(), captureLine);
                continue;
            }
        }

        if (line.empty()) continue;

        if (line.front() == '[') {
            const bool arrayTable = line.size() >= 2 && line[1] == '[';
            const std::size_t close = line.find(arrayTable ? "]]" : "]");
            if (close == std::string_view::npos) {
                ConfigError(lineNumber, "table header without its closing bracket.");
                table = Table::Other;
                continue;
            }
            const std::size_t open = arrayTable ? 2 : 1;
            const std::string name = WithoutSpaces(line.substr(open, close - open));
            if (arrayTable && name == RuleTable) {
                if (rules.size() >= MaximumRules) {
                    ConfigError(lineNumber, "more than %zu sets.", MaximumRules);
                    table = Table::Other;
                    continue;
                }
                RuleDraft draft{};
                draft.line = lineNumber;
                rules.push_back(std::move(draft));
                table = Table::Rule;
            } else if (!arrayTable && name == MainTable) {
                table = Table::Main;
            } else if (name == RuleTable || name == MainTable) {
                ConfigError(lineNumber, "%s needs %s brackets.", name.c_str(),
                    arrayTable ? "single [ ]" : "double [[ ]]");
                table = Table::Other;
            } else {
                ConfigWarning(lineNumber, "unknown table [%s] ignored.", name.c_str());
                table = Table::Other;
            }
            continue;
        }

        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            ConfigError(lineNumber, "expected key = value.");
            if (table == Table::Rule) rules.back().broken = true;
            continue;
        }
        const std::string_view key = Trim(line.substr(0, equals));
        const std::string_view value = Trim(line.substr(equals + 1));
        const std::string keyText(key);

        if (table == Table::Main) {
            if (key == "enabled") {
                if (value == "true") Enabled = true;
                else if (value == "false") Enabled = false;
                else ConfigError(lineNumber, "enabled must be true or false.");
            } else {
                ConfigWarning(lineNumber, "unknown key '%s' ignored.", keyText.c_str());
            }
            continue;
        }
        if (table != Table::Rule) {
            ConfigWarning(lineNumber, "key '%s' outside [%s] and [[%s]] ignored.", keyText.c_str(),
                MainTable, RuleTable);
            continue;
        }

        RuleDraft& rule = rules.back();
        if (key == "monsters") {
            if (rule.monstersSet) {
                ConfigError(lineNumber, "monsters is given twice in the same set.");
                rule.broken = true;
                continue;
            }
            rule.monstersSet = true;
            if (value.empty() || value.front() != '[') {
                ConfigError(lineNumber, "monsters must be a list in square brackets, for example [704, 705, 709].");
                rule.broken = true;
                continue;
            }
            const std::size_t close = value.find(']');
            if (close == std::string_view::npos) {
                arrayText.assign(value.substr(1));
                arrayText.push_back(',');
                capturing = true;
                captureLine = lineNumber;
                continue;
            }
            if (!Trim(value.substr(close + 1)).empty()) {
                ConfigError(lineNumber, "unexpected text after the closing ']' of monsters.");
                rule.broken = true;
            }
            ApplyIdList(value.substr(1, close - 1), rule, lineNumber);
        } else if (key == "unique") {
            if (rule.uniqueSet) {
                ConfigError(lineNumber, "unique is given twice in the same set.");
                rule.broken = true;
                continue;
            }
            rule.uniqueSet = true;
            const char* error = "";
            if (!ParseString(value, rule.unique, error)) {
                ConfigError(lineNumber, "unique %s.", error);
                rule.broken = true;
            }
        } else {
            ConfigWarning(lineNumber, "unknown key '%s' ignored.", keyText.c_str());
        }
    }

    if (capturing) {
        ConfigError(captureLine, "monsters is missing its closing ']'.");
        rules.back().broken = true;
    }
}

auto ReadConfigText(std::string& out) noexcept -> bool {
    std::string buffer(InitialConfigBytes, '\0');
    std::uint32_t required = 0;
    if (!Context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size()), &required)) {
        if (required < buffer.size() || required >= MaximumConfigBytes) return false;
        buffer.assign(static_cast<std::size_t>(required) + 1, '\0');
        if (!Context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size()), &required)) {
            return false;
        }
    }
    buffer.resize(std::strlen(buffer.c_str()));
    out = std::move(buffer);
    return true;
}

// False when anything is wrong: nothing is installed then, so a typo can
// never shrink a set or drop the wrong thing.
auto ReadConfiguration() -> bool {
    std::string text;
    if (!Context->EnsureConfig(DefaultConfigToml)) {
        Context->LogWarn("BossSetUniqueDrop: the config file could not be created; using the embedded defaults.");
        text = DefaultConfigToml;
    } else if (!ReadConfigText(text)) {
        Context->LogWarn("BossSetUniqueDrop: the config file could not be read; using the embedded defaults.");
        text = DefaultConfigToml;
    } else {
        ConfigFileWasRead = true;
    }

    ConfigErrors = 0;
    std::vector<RuleDraft> drafts;
    ParseConfig(text, drafts);

    for (RuleDraft& draft : drafts) {
        if (!draft.monstersSet) {
            ConfigError(draft.line, "this set has no monsters list.");
            draft.broken = true;
        } else if (draft.monsters.empty() && !draft.broken) {
            ConfigError(draft.line, "this set's monsters list is empty.");
            draft.broken = true;
        } else if (draft.monsters.size() > MaximumSetSize) {
            ConfigError(draft.line, "this set has more than %zu monsters.", MaximumSetSize);
            draft.broken = true;
        }
        if (!draft.uniqueSet) {
            ConfigError(draft.line, "this set has no unique.");
            draft.broken = true;
        } else if (draft.unique.empty() && !draft.broken) {
            ConfigError(draft.line, "this set's unique is empty.");
            draft.broken = true;
        } else if (draft.unique.size() > UniqueIndexMaxLength) {
            ConfigError(draft.line, "unique \"%s\" is longer than the %zu characters uniqueitems.txt keeps "
                "for an index, so no row can match it.", draft.unique.c_str(), UniqueIndexMaxLength);
            draft.broken = true;
        }
    }

    if (ConfigErrors > 0) {
        D2RL::LogErrorF(Context, "BossSetUniqueDrop: NOT installed, %d config error%s (see above). The game "
            "keeps its own uber Tristram drop.", ConfigErrors, ConfigErrors == 1 ? "" : "s");
        return false;
    }

    Rules.clear();
    Watched.fill(0);
    for (RuleDraft& draft : drafts) {
        Rule rule{ .line = draft.line, .monsters = std::move(draft.monsters), .unique = std::move(draft.unique) };
        for (const std::int32_t monsterId : rule.monsters) MarkWatched(monsterId);
        Rules.push_back(std::move(rule));
    }
    Runtime.assign(Rules.size(), RuleRuntime{});
    return true;
}

// ---------------------------------------------------------------------------
// uniqueitems.txt lookup
// ---------------------------------------------------------------------------

enum class NameMatch : std::uint8_t { Exact, CaseOnly, NotFound };

struct UniqueRow {
    std::int32_t  index = -1;
    std::uint32_t code  = 0;
    std::int32_t  level = 0;
    char          name[UniqueIndexCapacity]{};
};

auto EqualsIgnoringCase(const char* left, std::string_view right) noexcept -> bool {
    for (std::size_t index = 0; index < right.size(); ++index) {
        char a = left[index];
        char b = right[index];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

void FillRow(const std::uint8_t* record, std::uint64_t index, UniqueRow& out) noexcept {
    out.index = static_cast<std::int32_t>(index);
    out.code = ReadAt<std::uint32_t>(record, UniqueCodeOffset);
    out.level = ReadAt<std::int16_t>(record, UniqueLevelOffset);
    std::memcpy(out.name, record + UniqueIndexOffset, UniqueIndexCapacity);
    out.name[UniqueIndexCapacity - 1] = '\0';
}

// Exact match first; a match that differs only in letter case is used too,
// with a warning from the caller.
auto FindUnique(const std::uint8_t* rows, std::uint64_t count, std::string_view name, UniqueRow& out) noexcept
        -> NameMatch {
    std::uint64_t caseOnly = UniqueRowLimit + 1;
    for (std::uint64_t index = 0; index < count; ++index) {
        const std::uint8_t* record = rows + index * UniqueRowSize;
        const auto* recordName = reinterpret_cast<const char*>(record + UniqueIndexOffset);
        std::size_t length = 0;
        while (length < UniqueIndexCapacity && recordName[length] != '\0') ++length;
        if (length != name.size()) continue;
        if (std::memcmp(recordName, name.data(), length) == 0) {
            FillRow(record, index, out);
            return NameMatch::Exact;
        }
        if (caseOnly > UniqueRowLimit && EqualsIgnoringCase(recordName, name)) caseOnly = index;
    }
    if (caseOnly <= UniqueRowLimit) {
        FillRow(rows + caseOnly * UniqueRowSize, caseOnly, out);
        return NameMatch::CaseOnly;
    }
    return NameMatch::NotFound;
}

// The table the game itself uses for this game's drops.
auto FindUniqueForGame(void* game, std::string_view name, UniqueRow& out) noexcept -> NameMatch {
    const std::uint8_t* tables = GetDataTables(ReadAt<std::uint8_t>(game, GameDataContextOffset));
    if (tables == nullptr) return NameMatch::NotFound;
    const auto* rows = ReadAt<const std::uint8_t*>(tables, TablesUniqueRowsOffset);
    const auto count = ReadAt<std::uint64_t>(tables, TablesUniqueCountOffset);
    if (rows == nullptr || count == 0 || count > UniqueRowLimit) return NameMatch::NotFound;
    return FindUnique(rows, count, name, out);
}

// ---------------------------------------------------------------------------
// The drop
// ---------------------------------------------------------------------------

// 0x43FBF0's position and request, plus the unique row.
auto DropUniqueAtUnit(void* game, void* monster, std::uint32_t itemId, std::int32_t row,
        std::int32_t itemLevel) noexcept -> void* {
    Coords origin{};
    UnitCoords(monster, &origin);
    void* room = UnitRoom(monster);
    if (room == nullptr) return nullptr;

    Coords position = origin;
    if (RoomFindCoords(room, origin.x + 2, origin.y + 3) != nullptr) {
        position.x = origin.x + 2;
        position.y = origin.y + 3;
    }
    const std::int32_t searchSize = ReadAt<std::int32_t>(game, GameDropSearchOffset) == 0 ? 1 : 3;
    void* dropRoom = FreeDropCoords(room, &position, &origin, searchSize, DropSearchMask, DropSearchField, 1);
    if (dropRoom == nullptr) return nullptr;

    ItemDrop request{};
    request.unit = monster;
    request.game = game;
    request.itemLevel = itemLevel;
    request.itemId = itemId;
    request.spawnType = SpawnTypeGround;
    request.x = position.x;
    request.y = position.y;
    request.room = dropRoom;
    request.unitInitFlags = 1;
    request.itemFormat = ReadAt<std::uint16_t>(game, GameItemFormatOffset);
    request.quality = QualityUnique;
    request.itemIndex = row + 1;
    return CreateItem(game, &request, 0);
}

void RecordOutcome(std::size_t ruleIndex, bool dropped, const char* text) {
    std::lock_guard<std::mutex> lock(ProgressMutex);
    if (ruleIndex >= Runtime.size()) return;
    if (dropped) ++Runtime[ruleIndex].dropped;
    Runtime[ruleIndex].lastOutcome = text;
}

void DropForRule(std::size_t ruleIndex, void* game, void* monster, std::uint32_t classId) {
    const Rule& rule = Rules[ruleIndex];
    char text[512]{};

    UniqueRow unique{};
    const NameMatch match = FindUniqueForGame(game, rule.unique, unique);
    if (match == NameMatch::NotFound) {
        std::snprintf(text, sizeof(text), "monster %u completed the set, but \"%s\" is not an index in the "
            "uniqueitems.txt this game uses; nothing dropped.", classId, rule.unique.c_str());
        D2RL::LogErrorF(Context, "BossSetUniqueDrop: set %zu: %s", ruleIndex + 1, text);
        RecordOutcome(ruleIndex, false, text);
        return;
    }
    if (match == NameMatch::CaseOnly) {
        D2RL::LogWarnF(Context, "BossSetUniqueDrop: set %zu: \"%s\" matched uniqueitems.txt \"%s\" only when "
            "letter case is ignored; using it.", ruleIndex + 1, rule.unique.c_str(), unique.name);
    }

    std::int32_t itemLevel = ReadUnitStat(monster, LevelStatId, 0);
    if (itemLevel < 1) itemLevel = 1;
    const std::int32_t monsterLevel = itemLevel;
    if (unique.level > itemLevel) itemLevel = unique.level;

    const std::uint8_t dataContext = ReadAt<std::uint8_t>(game, GameDataContextOffset);
    const std::uint32_t itemId = ItemIdFromCode(dataContext, unique.code);
    if (itemId == InvalidItemId) {
        std::snprintf(text, sizeof(text), "\"%s\" (row %d) has code '%s', which is not in the item tables; "
            "nothing dropped.", unique.name, unique.index, CodeText(unique.code).c_str());
        D2RL::LogErrorF(Context, "BossSetUniqueDrop: set %zu: %s", ruleIndex + 1, text);
        RecordOutcome(ruleIndex, false, text);
        return;
    }

    void* item = DropUniqueAtUnit(game, monster, itemId, unique.index, itemLevel);
    if (item == nullptr) {
        std::snprintf(text, sizeof(text), "the game found no free spot near monster %u or refused to create "
            "\"%s\"; nothing dropped.", classId, unique.name);
        D2RL::LogErrorF(Context, "BossSetUniqueDrop: set %zu: %s", ruleIndex + 1, text);
        RecordOutcome(ruleIndex, false, text);
        return;
    }
    RefillCharges(game, nullptr, item);

    const std::uint32_t quality = ItemQuality(item);
    const auto fileIndex = static_cast<std::int32_t>(ItemFileIndex(item));
    if (quality == static_cast<std::uint32_t>(QualityUnique) && fileIndex == unique.index) {
        std::snprintf(text, sizeof(text), "\"%s\" (row %d) dropped by monster %u at item level %d%s.",
            unique.name, unique.index, classId, itemLevel,
            itemLevel != monsterLevel ? " (raised from the monster's level to the unique's lvl)" : "");
        D2RL::LogInfoF(Context, "BossSetUniqueDrop: set %zu: %s", ruleIndex + 1, text);
        RecordOutcome(ruleIndex, true, text);
    } else if (quality == static_cast<std::uint32_t>(QualityUnique)) {
        std::snprintf(text, sizeof(text), "the game rejected \"%s\" (row %d) and rolled unique row %d of the "
            "same base instead. Its row failed the unique pick's checks on the code, version, flag or ladder "
            "season columns.", unique.name, unique.index, fileIndex);
        D2RL::LogErrorF(Context, "BossSetUniqueDrop: set %zu: %s", ruleIndex + 1, text);
        RecordOutcome(ruleIndex, false, text);
    } else {
        std::snprintf(text, sizeof(text), "\"%s\" (row %d) was not created; the game made a quality %u item "
            "of its base. Most likely it already dropped in this game (one per game unless nolimit is set), "
            "otherwise its row failed the code, version, flag or ladder season checks.",
            unique.name, unique.index, quality);
        D2RL::LogWarnF(Context, "BossSetUniqueDrop: set %zu: %s", ruleIndex + 1, text);
        RecordOutcome(ruleIndex, false, text);
    }
}

// ---------------------------------------------------------------------------
// Listeners
// ---------------------------------------------------------------------------

void EnsureShape(GameProgress& game) {
    if (game.killed.size() == Rules.size()) return;
    game.killed.assign(Rules.size(), {});
    game.count.assign(Rules.size(), 0);
    for (std::size_t index = 0; index < Rules.size(); ++index) {
        game.killed[index].assign(Rules[index].monsters.size(), 0);
    }
}

// Runs after the game's own death-mode callback has returned, so the normal
// drop and D2RCore's monster-death event are already done.
void OnMonsterDied(void* game, void* monster) noexcept {
    if (ReadAt<std::uint32_t>(monster, UnitTypeOffset) != UnitTypeMonster) return;
    const std::uint32_t classId = ReadAt<std::uint32_t>(monster, UnitClassOffset);
    if (!IsWatched(classId)) return;
    const std::uint32_t gameId = ReadAt<std::uint16_t>(game, GameIdOffset);

    std::array<std::uint16_t, MaximumRules> completed{};
    std::size_t completedCount = 0;
    {
        std::lock_guard<std::mutex> lock(ProgressMutex);
        GameProgress& progress = Progress[gameId];
        EnsureShape(progress);
        for (std::size_t ruleIndex = 0; ruleIndex < Rules.size(); ++ruleIndex) {
            const std::vector<std::int32_t>& monsters = Rules[ruleIndex].monsters;
            for (std::size_t slot = 0; slot < monsters.size(); ++slot) {
                if (static_cast<std::uint32_t>(monsters[slot]) != classId) continue;
                if (progress.killed[ruleIndex][slot] == 0) {
                    progress.killed[ruleIndex][slot] = 1;
                    ++progress.count[ruleIndex];
                }
                if (progress.count[ruleIndex] >= monsters.size()) {
                    std::fill(progress.killed[ruleIndex].begin(), progress.killed[ruleIndex].end(),
                        std::uint8_t{ 0 });
                    progress.count[ruleIndex] = 0;
                    ++Runtime[ruleIndex].completions;
                    completed[completedCount++] = static_cast<std::uint16_t>(ruleIndex);
                    D2RL::LogInfoF(Context, "BossSetUniqueDrop: set %zu (%s): monster %u died, set complete "
                        "in game %u.", ruleIndex + 1, Rules[ruleIndex].unique.c_str(), classId, gameId);
                } else {
                    D2RL::LogInfoF(Context, "BossSetUniqueDrop: set %zu (%s): monster %u died, %u of %zu "
                        "in game %u.", ruleIndex + 1, Rules[ruleIndex].unique.c_str(), classId,
                        progress.count[ruleIndex], monsters.size(), gameId);
                }
                break;
            }
        }
    }
    for (std::size_t index = 0; index < completedCount; ++index) {
        DropForRule(completed[index], game, monster, classId);
    }
}

// Sits in the monster mode table's death slot, with the game's own ABI.
auto DeathModeHook(void* game, void* modeChange) noexcept -> std::int32_t {
    const std::int32_t result = OriginalDeathMode(game, modeChange);
    if (Armed.load(std::memory_order_acquire) && game != nullptr && modeChange != nullptr) {
        void* monster = ReadAt<void*>(modeChange, ModeChangeUnitOffset);
        if (monster != nullptr) OnMonsterDied(game, monster);
    }
    return result;
}

void __cdecl OnGameBoundary(const D2RL::PluginContext*, const D2RL::Lifecycle::GameplayEvent*, void*) noexcept {
    std::lock_guard<std::mutex> lock(ProgressMutex);
    Progress.clear();
}

// Early feedback: every configured name against uniqueitems.txt as loaded.
void __cdecl OnDataTablesLoaded(const D2RL::PluginContext*, const D2RL::Lifecycle::DataTablesLoadedEvent*,
        void*) noexcept {
    if (DataTableApi == nullptr) return;
    constexpr D2RL::DataTables::Bank Banks[]{
        D2RL::DataTables::Bank::Classic, D2RL::DataTables::Bank::Lod, D2RL::DataTables::Bank::Rotw,
    };
    struct BankRows {
        const std::uint8_t* rows = nullptr;
        std::uint64_t       count = 0;
    };
    std::array<BankRows, 3> banks{};
    std::size_t usable = 0;
    for (std::size_t index = 0; index < 3; ++index) {
        D2RL::DataTables::TableView view{};
        view.structSize = D2RL::DataTables::TableViewSize;
        if (DataTableApi->getTable(Context, Banks[index], D2RL::DataTables::TableId::UniqueItems, &view)
                != D2RL::DataTables::Result::Success) {
            continue;
        }
        if (!D2RL::DataTables::HasTableViewField(&view, D2RL::DataTables::TableViewRequiredSize)
                || view.rows == nullptr || view.rowSize != UniqueRowSize || view.rowCount == 0) {
            D2RL::LogWarnF(Context, "BossSetUniqueDrop: uniqueitems bank %u has rows of %u bytes, not %zu; "
                "the early name check skips it.", static_cast<unsigned>(Banks[index]), view.rowSize, UniqueRowSize);
            continue;
        }
        banks[index] = { static_cast<const std::uint8_t*>(view.rows), view.rowCount };
        ++usable;
    }
    if (usable == 0) {
        Context->LogWarn("BossSetUniqueDrop: uniqueitems.txt was not readable through the data table service; "
            "the names are checked when a set completes instead.");
        return;
    }

    for (std::size_t ruleIndex = 0; ruleIndex < Rules.size(); ++ruleIndex) {
        const Rule& rule = Rules[ruleIndex];
        UniqueRow found{};
        NameMatch best = NameMatch::NotFound;
        for (const BankRows& bank : banks) {
            if (bank.rows == nullptr) continue;
            UniqueRow row{};
            const NameMatch match = FindUnique(bank.rows, bank.count, rule.unique, row);
            if (match == NameMatch::Exact || (match == NameMatch::CaseOnly && best == NameMatch::NotFound)) {
                found = row;
                best = match;
            }
            if (best == NameMatch::Exact) break;
        }
        char text[384]{};
        if (best == NameMatch::NotFound) {
            std::snprintf(text, sizeof(text), "\"%s\" NOT FOUND in uniqueitems.txt (index column); this set "
                "will drop nothing.", rule.unique.c_str());
            D2RL::LogErrorF(Context, "BossSetUniqueDrop: set %zu: %s", ruleIndex + 1, text);
        } else {
            std::snprintf(text, sizeof(text), "\"%s\" is uniqueitems row %d, code '%s', lvl %d%s.", found.name,
                found.index, CodeText(found.code).c_str(), found.level,
                best == NameMatch::CaseOnly ? " (letter case differs from the config)" : "");
            D2RL::LogInfoF(Context, "BossSetUniqueDrop: set %zu: %s", ruleIndex + 1, text);
        }
        std::lock_guard<std::mutex> lock(ProgressMutex);
        if (ruleIndex < Runtime.size()) Runtime[ruleIndex].tableCheck = text;
    }
}

// ---------------------------------------------------------------------------
// Verification and installation
// ---------------------------------------------------------------------------

auto Verify(std::uint64_t rva, const std::uint8_t* expected, std::size_t size, const char* label) noexcept -> bool {
    if (Context->CheckExpectedBytes(rva, expected, static_cast<std::uint32_t>(size))) return true;
    D2RL::LogErrorF(Context, "BossSetUniqueDrop: NOT installed. %s at 0x%llX does not match the verified D2R "
        "image, or another plugin already owns it. Nothing was patched.", label,
        static_cast<unsigned long long>(rva));
    return false;
}

struct Witness {
    std::uint64_t       rva;
    const std::uint8_t* bytes;
    std::size_t         size;
    const char*         label;
};

constexpr Witness Witnesses[]{
    { UberDropBlockRva, UberDropBlockWitness, sizeof(UberDropBlockWitness), "The uber Tristram drop block" },
    { DropLevelRva, DropLevelWitness, sizeof(DropLevelWitness), "The drop routine's item level read" },
    { DropItemIdRva, DropItemIdWitness, sizeof(DropItemIdWitness), "The drop routine's base item lookup" },
    { DropPlaceRva, DropPlaceWitness, sizeof(DropPlaceWitness), "The drop routine's position and create block" },
    { PickTablesRva, PickTablesWitness, sizeof(PickTablesWitness), "The unique pick's table read" },
    { PickRowRva, PickRowWitness, sizeof(PickRowWitness), "The unique pick's lvl and row test" },
    { DeathModeUnitRva, DeathModeUnitWitness, sizeof(DeathModeUnitWitness), "The death-mode callback's unit load" },
    { UnitRoomRva, UnitRoomWitness, sizeof(UnitRoomWitness), "UNITS_GetRoom" },
    { ItemFileIndexRva, ItemFileIndexWitness, sizeof(ItemFileIndexWitness), "The item file index getter" },
    { ItemQualityRva, ItemQualityWitness, sizeof(ItemQualityWitness), "ITEMS_GetItemQuality" },
};

// The slot and its neighbours hold absolute pointers into the image.
auto VerifyModeTable() noexcept -> bool {
    struct Pointer {
        std::size_t   offset;
        std::uint64_t rva;
    };
    constexpr Pointer Pointers[]{
        { 0x00, DeathModeCallbackRva },
        { 0x08, DeathModeSecondRva },
        { ModeEntrySize + 0x00, NeutralModeStartRva },
        { ModeEntrySize + 0x08, NeutralModeSecondRva },
    };
    const auto* table = reinterpret_cast<const void*>(Base + DeathModeSlotRva);
    for (const Pointer& pointer : Pointers) {
        const auto value = ReadAt<std::uint64_t>(table, pointer.offset);
        if (value == Base + pointer.rva) continue;
        D2RL::LogErrorF(Context, "BossSetUniqueDrop: NOT installed. The monster mode table at 0x%llX+%zX holds "
            "0x%llX instead of 0x%llX; another plugin already redirected it, or this is not the verified D2R "
            "image. Nothing was patched.", static_cast<unsigned long long>(DeathModeSlotRva), pointer.offset,
            static_cast<unsigned long long>(value), static_cast<unsigned long long>(Base + pointer.rva));
        return false;
    }
    return true;
}

// Atomic, and only when the slot still holds `expected`.
auto SwapDeathModeSlot(std::uint64_t expected, std::uint64_t desired) noexcept -> bool {
    auto* slot = reinterpret_cast<LONG64*>(Base + DeathModeSlotRva);
    DWORD previous = 0;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &previous)) return false;
    const LONG64 seen = InterlockedCompareExchange64(slot, static_cast<LONG64>(desired), static_cast<LONG64>(expected));
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(*slot), previous, &ignored);
    return seen == static_cast<LONG64>(expected);
}

auto HookAddress() noexcept -> std::uint64_t {
    return reinterpret_cast<std::uint64_t>(&DeathModeHook);
}

void RestoreUberDrop() noexcept {
    if (!SitePatched) return;
    if (Context->PatchBytes(UberCharmDropSiteRva, UberCharmDropSkip, sizeof(UberCharmDropSkip),
            UberCharmDropOriginal, sizeof(UberCharmDropOriginal))) {
        SitePatched = false;
    } else {
        Context->LogError("BossSetUniqueDrop: the uber Tristram unique charm drop could not be restored.");
    }
}

auto VerifyNativeContract() noexcept -> bool {
    for (const Witness& witness : Witnesses) {
        if (!Verify(witness.rva, witness.bytes, witness.size, witness.label)) return false;
    }
    if (!VerifyModeTable()) return false;
    OriginalDeathMode = At<DeathModeFn>(DeathModeCallbackRva);
    UnitRoom       = At<UnitPointerFn>(UnitRoomRva);
    UnitCoords     = At<UnitCoordsFn>(UnitCoordsRva);
    RoomFindCoords = At<RoomFindCoordsFn>(RoomFindCoordsRva);
    FreeDropCoords = At<FreeDropCoordsFn>(FreeDropCoordsRva);
    CreateItem     = At<CreateItemFn>(CreateItemRva);
    ItemIdFromCode = At<ItemIdFromCodeFn>(ItemIdFromCodeRva);
    GetDataTables  = At<DataTablesFn>(DataTablesRva);
    ItemQuality    = At<ItemValueFn>(ItemQualityRva);
    ItemFileIndex  = At<ItemValueFn>(ItemFileIndexRva);
    ReadUnitStat   = At<ReadUnitStatFn>(UnitStatRva);
    RefillCharges  = At<RefillChargesFn>(RefillChargesRva);
    return true;
}

void UnregisterListeners() noexcept {
    if (LifecycleApi == nullptr) return;
    if (JoinListener != 0) LifecycleApi->unregisterGameplayEventListener(Context, JoinListener);
    if (LeaveListener != 0) LifecycleApi->unregisterGameplayEventListener(Context, LeaveListener);
    if (TablesListener != 0) LifecycleApi->unregisterDataTablesLoadedListener(Context, TablesListener);
    JoinListener = 0;
    LeaveListener = 0;
    TablesListener = 0;
}

auto RegisterListeners() noexcept -> bool {
    const D2RL::Lifecycle::GameplayEventListener joined{
        .structSize = D2RL::Lifecycle::GameplayEventListenerSize,
        .flags      = 0,
        .kind       = D2RL::Lifecycle::GameplayEventKind::GameJoined,
        .reserved   = 0,
        .callback   = &OnGameBoundary,
        .userData   = nullptr,
    };
    const D2RL::Lifecycle::GameplayEventListener left{
        .structSize = D2RL::Lifecycle::GameplayEventListenerSize,
        .flags      = 0,
        .kind       = D2RL::Lifecycle::GameplayEventKind::GameLeft,
        .reserved   = 0,
        .callback   = &OnGameBoundary,
        .userData   = nullptr,
    };
    using D2RL::Lifecycle::Result;
    if (LifecycleApi->registerGameplayEventListener(Context, &joined, &JoinListener) != Result::Success
            || LifecycleApi->registerGameplayEventListener(Context, &left, &LeaveListener) != Result::Success) {
        Context->LogError("BossSetUniqueDrop: NOT installed. The game join/leave listener was refused.");
        UnregisterListeners();
        return false;
    }
    if (DataTableApi != nullptr) {
        const D2RL::Lifecycle::DataTablesLoadedListener tables{
            .structSize = D2RL::Lifecycle::DataTablesLoadedListenerSize,
            .flags      = 0,
            .callback   = &OnDataTablesLoaded,
            .userData   = nullptr,
        };
        if (LifecycleApi->registerDataTablesLoadedListener(Context, &tables, &TablesListener) != Result::Success) {
            Context->LogWarn("BossSetUniqueDrop: the table-load listener was refused; unique names are checked "
                "when a set completes instead.");
            TablesListener = {};
        }
    }
    return true;
}

void Install(bool configUsable) noexcept {
    if (!configUsable) {
        State.store(InstallState::ConfigError, std::memory_order_release);
        return;
    }
    if (!Enabled) {
        State.store(InstallState::DisabledByConfig, std::memory_order_release);
        Context->LogInfo("BossSetUniqueDrop: not installed, turned off in the config file. The uber bosses keep "
            "the game's own drop.");
        return;
    }

    if (Context->QueryService(&LifecycleApi) != D2RL::ServiceQueryResult::Success
            || !D2RL::HasLifecycleServiceField(LifecycleApi, D2RL::LifecycleServiceRequiredSize)) {
        LifecycleApi = nullptr;
        State.store(InstallState::ServiceUnavailable, std::memory_order_release);
        Context->LogError("BossSetUniqueDrop: NOT installed. D2RLoader's lifecycle service (game join/leave "
            "events) is not available.");
        return;
    }
    if (Context->QueryService(&DataTableApi) != D2RL::ServiceQueryResult::Success
            || !D2RL::HasDataTableServiceField(DataTableApi, D2RL::DataTableServiceRequiredSize)) {
        DataTableApi = nullptr;
    }

    if (!VerifyNativeContract()) {
        State.store(InstallState::UnsupportedBuild, std::memory_order_release);
        return;
    }
    if (!RegisterListeners()) {
        State.store(InstallState::InstallFailed, std::memory_order_release);
        return;
    }
    if (!Context->PatchBytes(UberCharmDropSiteRva, UberCharmDropOriginal, sizeof(UberCharmDropOriginal),
            UberCharmDropSkip, sizeof(UberCharmDropSkip))) {
        // Without the patch the random charm would drop next to the named
        // unique, so nothing is installed.
        UnregisterListeners();
        State.store(InstallState::InstallFailed, std::memory_order_release);
        Context->LogError("BossSetUniqueDrop: NOT installed. The uber Tristram unique charm drop at 0x5A84FD "
            "could not be patched out.");
        return;
    }
    SitePatched = true;
    if (!SwapDeathModeSlot(Base + DeathModeCallbackRva, HookAddress())) {
        // Without kill tracking the uber bosses would drop nothing, so the
        // charm drop goes back too.
        RestoreUberDrop();
        UnregisterListeners();
        State.store(InstallState::InstallFailed, std::memory_order_release);
        Context->LogError("BossSetUniqueDrop: NOT installed. The monster death-mode slot at 0x1D1A200 could not "
            "be redirected.");
        return;
    }
    SlotPatched = true;
    Armed.store(true, std::memory_order_release);
    State.store(InstallState::Armed, std::memory_order_release);

    D2RL::LogInfoF(Context, "BossSetUniqueDrop: installed. The uber Tristram random unique charm is replaced; "
        "%zu set%s:", Rules.size(), Rules.size() == 1 ? "" : "s");
    for (std::size_t index = 0; index < Rules.size(); ++index) {
        D2RL::LogInfoF(Context, "BossSetUniqueDrop:   set %zu: \"%s\" when all of %s have died.", index + 1,
            Rules[index].unique.c_str(), JoinIds(Rules[index].monsters, 400).c_str());
    }
    if (Rules.empty()) {
        Context->LogWarn("BossSetUniqueDrop: no sets are configured, so the uber bosses drop no unique at all.");
    }
}

// ---------------------------------------------------------------------------
// Console command
// ---------------------------------------------------------------------------

auto StateText(InstallState state) noexcept -> const char* {
    switch (state) {
    case InstallState::Armed:              return "active, uber Tristram random charm replaced";
    case InstallState::DisabledByConfig:   return "off in the config file (vanilla uber drop)";
    case InstallState::ConfigError:        return "NOT ACTIVE, config errors, see the log (vanilla uber drop)";
    case InstallState::ServiceUnavailable: return "NOT ACTIVE, no lifecycle service, see the log";
    case InstallState::UnsupportedBuild:   return "NOT ACTIVE, unrecognised game build, see the log";
    case InstallState::InstallFailed:      return "NOT ACTIVE, install failed, see the log";
    case InstallState::NotAttempted:
    default:                               return "NOT ACTIVE, never attempted";
    }
}

auto __cdecl StatusCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept
        -> D2RL::ConsoleCommandResult {
    if (command == nullptr || command->plugin == nullptr) return D2RL::ConsoleCommandResult::Failed;
    const D2RL::PluginContext* plugin = command->plugin;

    char line[512]{};
    std::snprintf(line, sizeof(line), "Boss Set Unique Drop: %s | config %s | %zu set%s",
        StateText(State.load(std::memory_order_acquire)),
        ConfigFileWasRead ? "loaded" : "NOT READ (embedded defaults)", Rules.size(),
        Rules.size() == 1 ? "" : "s");
    plugin->WriteConsoleMessage(line);

    std::lock_guard<std::mutex> lock(ProgressMutex);
    for (std::size_t index = 0; index < Rules.size(); ++index) {
        const Rule& rule = Rules[index];
        const RuleRuntime& runtime = Runtime[index];
        std::snprintf(line, sizeof(line), "set %zu: \"%s\" <- %s | completed %llu, dropped %llu",
            index + 1, rule.unique.c_str(), JoinIds(rule.monsters, 200).c_str(),
            static_cast<unsigned long long>(runtime.completions), static_cast<unsigned long long>(runtime.dropped));
        plugin->WriteConsoleMessage(line);
        std::snprintf(line, sizeof(line), "  table: %s", runtime.tableCheck.c_str());
        plugin->WriteConsoleMessage(line);
        std::snprintf(line, sizeof(line), "  last: %s", runtime.lastOutcome.c_str());
        plugin->WriteConsoleMessage(line);
        for (const auto& [gameId, game] : Progress) {
            if (index >= game.killed.size()) continue;
            std::vector<std::int32_t> dead;
            for (std::size_t slot = 0; slot < rule.monsters.size(); ++slot) {
                if (game.killed[index][slot] != 0) dead.push_back(rule.monsters[slot]);
            }
            std::snprintf(line, sizeof(line), "  game %u: %zu of %zu dead%s%s", gameId, dead.size(),
                rule.monsters.size(), dead.empty() ? "" : ": ", JoinIds(dead, 200).c_str());
            plugin->WriteConsoleMessage(line);
        }
    }
    return D2RL::ConsoleCommandResult::Handled;
}

constexpr D2RL::PluginInfo Info{
    .infoSize    = D2RL::PluginInfoSize,
    .abiVersion  = D2RL_PLUGIN_ABI_VERSION,
    .id          = "celestialrayone.boss-set-unique-drop",
    .name        = "Boss Set Unique Drop",
    .version     = "1.0.0",
    .author      = "CelestialRayOne",
    .description = "Drops a named unique when the last monster of a configured set dies, replacing the "
                   "uber Tristram random unique charm.",
    // Server: monster deaths and drops are decided by the server.
    .flags       = D2RL::PluginFlags::Server | D2RL::PluginFlags::NativeHooks,
};

static_assert(D2RL::HasValidPluginRole(Info.flags), "Exactly one execution role must be set.");
static_assert(D2RL::HasOnlyKnownPluginFlags(Info.flags), "Unknown plugin flag set.");

}  // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
    return &Info;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
    if (!D2RL::HasContext(context)) return false;
    Context = context;
    Base = context->exeBase;

    // Registered first, and the load always succeeds: returning false would
    // unload the DLL and take the command with it.
    if (!Context->RegisterConsoleCommand("bossdrop", &StatusCommand,
            "Reports Boss Set Unique Drop status, sets and progress in the current game.")) {
        Context->LogWarn("BossSetUniqueDrop: the status console command was refused.");
    }

    const bool configUsable = ReadConfiguration();
    Install(configUsable);
    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    if (Context == nullptr) return;
    Armed.store(false, std::memory_order_release);
    if (SlotPatched) {
        if (SwapDeathModeSlot(HookAddress(), Base + DeathModeCallbackRva)) {
            SlotPatched = false;
        } else {
            Context->LogError("BossSetUniqueDrop: the monster death-mode slot no longer points at this plugin "
                "and was left alone.");
        }
    }
    RestoreUberDrop();
    UnregisterListeners();
}

}  // namespace CelestialRayOne::BossSetUniqueDrop
