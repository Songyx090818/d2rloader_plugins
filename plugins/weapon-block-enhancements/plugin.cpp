//
// Weapon Block Rules - D2RLoader plugin (celestialrayone.weapon-block-rules)
//
// WHAT IT DOES
//   Stock D2R only lets passive_weaponblock (itemstatcost.txt row 348) roll
//   when the defender's weapon class is 13, the dual hand-to-hand class. The
//   game builds that class only for an Assassin holding a weapon in each hand.
//   This plugin replaces that single check with ESR's rule, for any class:
//
//     - a claw in each hand, or
//     - a sword in each hand, or
//     - one sword wielded with both hands and nothing in the other hand.
//
//   One claw never counts. A Barbarian's two-handed sword held next to a
//   shield never counts. The weapon types come from the config file.
//
//   A player's block animation only starts when the game has one for the
//   character and the weapons in hand. Stock data has none for some sword
//   setups (for example AIBL2HS, an Assassin with a two-handed sword), and the
//   game would fall back to its 2048-frame placeholder record, locking the
//   character in the block mode for over a minute. Those blocks now happen without the
//   animation, exactly like a block that lands inside the faster-block-rate
//   window. Claws have their animation and play it as before.
//
//   Nothing else about weapon block changes: the stat's value and param, the
//   percent roll, shield block rolling first, no weapon block while walking or
//   running, and the order against dodge, avoid and evade all stay stock.
//   Monsters and hirelings keep the stock rule. A player shapeshifted into a
//   monster form (werewolf, werebear) keeps the stock rule too, exactly as
//   before. The character screen's chance-to-block line follows the same rule
//   as the server roll, so it never shows a chance the roll will not honour.
//
// HOW (D2RLoader 1.3.1 process image, base 0x140000000, RVAs below)
//   Weapon block asks its weapon-class question at three places, each a call
//   to the composite weapon-class function 3B6E00 (unit, inventory, &class,
//   -1, 1) whose out value is then compared to 13. This plugin takes the two
//   that serve players:
//
//     ApplyDodge 44EF70, player branch (server roll)
//       44EFB4  call 450070              weapon block chance (D2RCore
//                                        ReadWidePassiveWeaponBlock)
//       44EFD3  call 34A360              inventory
//       44EFF1  call 3B6E00              weapon class -> [rsp+68h]
//       44EFF6  cmp  dword [rsp+68h], 0Dh
//       44EFFB  jne  44F063              no weapon block
//
//     char.cpp chance-to-block line 1514CA0 (client display)
//       1514D05 call 1514E00             weapon block chance
//       1514D3C call 3B6E00              weapon class -> [rsp+50h]
//       1514D41 cmp  dword [rsp+50h], 0Dh
//       1514D46 cmovne esi, r15d         shown chance = 0
//
//   Each call is redirected to its own 5-byte "jmp relay" stub written into
//   the int3 padding right after 3B6E00 (3B6F21 server, 3B6F26 display,
//   3B6F2B block animation; the loader only accepts a rel32 call whose target
//   is inside the game image).
//   The stub jumps through a relay page to a function with the composite's
//   own ABI. It runs the stock composite call first, then writes 13 when
//   ESR's rule is met and a class other than 13 when it is not. At both sites
//   the out value is read by that one compare only, and the return value is
//   not read at all, so nothing else sees the answer.
//
//   The third place, the monster branch of ApplyDodge (call at 44F14A), is
//   not touched.
//
//   The block animation starts from the damage finalizer 44A9B0, player
//   branch, for a BLOCK or WEAPONBLOCK result (0x8010) once the faster block
//   rate window has passed:
//       44AEC4  call 42D2C0 (game, unit, 0, 9, 0, 0, 0, 0)   player mode 9, BL
//       44AED9  call 2F7D10 (unit, 95, frame)                 lastblockframe
//   That call is redirected the same way. The hook builds the animation name
//   exactly as the game does when the mode changes (token conversion 33A1F0,
//   name builder 3B6700, e.g. AIBL2HS) and looks it up in the animation table
//   (3E3AD0). Found: the stock mode change runs. Missing: it is skipped; the
//   lastblockframe write after it still runs, as in stock.
//
// GAME FUNCTIONS CALLED (entry bytes verified before anything is patched)
//   3B6E00  composite weapon class (unit, inventory, int* class, mode, flag)
//   3886D0  item in a body location (inventory, bodyloc): 4 right, 5 left arm
//   36AD20  item usable: false when broken (0x100) or disabled (0x4000)
//   373890  item is of itemtype (item, typeId), follows Equiv1/Equiv2
//   388910  wield type (unit, inventory, 0): 2 = weapon held with both hands
//   34FB00  unit extended flag test (unit, mask): 8 = shapeshifted
//   33A1F0  shapeshift token conversion (unit, &type, &class, &mode, file, line)
//   3494F0  unit animation or sequence mode
//   34B9D0  unit type: 0 = player
//   34A0E0  unit data context: 1 Classic, 2 LoD, 3 RotW
//   42D2C0  player mode change (game, unit, target, mode, 0, 0, 0, 0)
//   349860  unit class id
//   34A360  unit inventory
//   3B6700  animation name builder (unit, class, mode, type, inventory, name,
//           8, &weaponClass, 0, 1, bank)
//   3E3AD0  animation table lookup (bank, name): null when missing
//
// If the plugin unloads, or a later step of the install fails, every relay is
// first pointed straight at its stock function, so a site that could not be
// restored still reaches native code instead of code in an unloaded DLL.

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
#include <string>
#include <string_view>
#include <vector>

namespace CelestialRayOne::WeaponBlockRules {
namespace {

// ---------------------------------------------------------------------------
// Game image contract (D2RLoader 1.3.1)
// ---------------------------------------------------------------------------

constexpr std::uint64_t CompositeRva          = 0x3B6E00;
constexpr std::uint64_t GetItemFromBodyLocRva = 0x3886D0;
constexpr std::uint64_t ItemUsableRva         = 0x36AD20;
constexpr std::uint64_t CheckItemTypeRva      = 0x373890;
constexpr std::uint64_t GetWieldTypeRva       = 0x388910;
constexpr std::uint64_t TestUnitFlagExRva     = 0x34FB00;
constexpr std::uint64_t ConvertTokenRva       = 0x33A1F0;
constexpr std::uint64_t GetAnimModeRva        = 0x3494F0;
constexpr std::uint64_t GetUnitTypeRva        = 0x34B9D0;
constexpr std::uint64_t DataContextRva        = 0x34A0E0;
constexpr std::uint64_t ChangePlayerModeRva   = 0x42D2C0;
constexpr std::uint64_t GetClassIdRva         = 0x349860;
constexpr std::uint64_t GetInventoryRva       = 0x34A360;
constexpr std::uint64_t BuildAnimNameRva      = 0x3B6700;
constexpr std::uint64_t FindAnimRecordRva     = 0x3E3AD0;

// The composite front end passes this file and line to the token conversion
// (3B6E47 lea r14, [rip+...] and 3B6EAA mov dword [rsp+28h], 22Fh). The same
// pair is passed here so the call is identical to the game's own.
constexpr std::uint64_t CompositSourceFileRva = 0x1D0AED0;
constexpr std::int32_t  CompositConvertLine   = 0x22F;
constexpr char          CompositSourceFile[]  = "D2Common\\src\\Composit\\Composit.cpp";

constexpr std::uint8_t CompositeEntry[]{
    0x44, 0x89, 0x4C, 0x24, 0x20, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x50,
    0x49, 0x8B, 0xF0, 0x48, 0x8B, 0xEA, 0x48, 0x8B, 0xF9, 0x48, 0x85, 0xC9,
    0x75, 0x20 };
constexpr std::uint8_t GetItemFromBodyLocEntry[]{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x63,
    0xFA, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9, 0x75, 0x20 };
constexpr std::uint8_t ItemUsableEntry[]{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9,
    0x0F, 0x84, 0xC5, 0x00, 0x00, 0x00, 0xE8, 0x99, 0x0C, 0xFE, 0xFF, 0x83,
    0xF8, 0x04, 0x74, 0x14 };
constexpr std::uint8_t CheckItemTypeEntry[]{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89,
    0x74, 0x24, 0x20, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20,
    0x48, 0x63, 0xF2, 0x48, 0x8B, 0xF9, 0x48, 0x85, 0xC9, 0x0F, 0x84, 0x9E,
    0x00, 0x00, 0x00, 0xE8, 0x14, 0x81, 0xFD, 0xFF, 0x83, 0xF8, 0x04 };
constexpr std::uint8_t GetWieldTypeEntry[]{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57,
    0x41, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x41, 0x0F, 0xB6, 0xE8, 0x48, 0x8B,
    0xFA, 0x4C, 0x8B, 0xF1, 0x48, 0x85, 0xD2, 0x75, 0x32 };
constexpr std::uint8_t TestUnitFlagExEntry[]{
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x85, 0xC9, 0x75, 0x1A, 0x88, 0x4C, 0x24,
    0x30, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8, 0x49, 0x4F, 0xFF, 0xFF, 0x84,
    0xC0, 0x74, 0x01, 0xCC, 0x33, 0xC0, 0x48, 0x83, 0xC4, 0x28, 0xC3, 0x85,
    0x91, 0x28, 0x01, 0x00, 0x00 };
constexpr std::uint8_t ConvertTokenEntry[]{
    0x4C, 0x89, 0x4C, 0x24, 0x20, 0x55, 0x41, 0x55, 0x41, 0x57, 0x48, 0x83,
    0xEC, 0x50, 0x4C, 0x8B, 0xEA, 0x4D, 0x8B, 0xF8, 0xBA, 0x08, 0x00, 0x00,
    0x00, 0x48, 0x8B, 0xE9, 0xE8, 0xEF, 0x58, 0x01, 0x00 };
constexpr std::uint8_t GetAnimModeEntry[]{
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x85, 0xC9, 0x75, 0x1A, 0x88, 0x4C, 0x24,
    0x30, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8, 0xF9, 0xB7, 0xFF, 0xFF, 0x84,
    0xC0, 0x74, 0x01, 0xCC, 0x33, 0xC0, 0x48, 0x83, 0xC4, 0x28, 0xC3, 0x83,
    0x39, 0x01, 0x77, 0x0F, 0x48, 0x83, 0x79, 0x40, 0x00 };
constexpr std::uint8_t GetUnitTypeEntry[]{
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x85, 0xC9, 0x75, 0x1D, 0x88, 0x4C, 0x24,
    0x30, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8, 0x39, 0x9E, 0xFF, 0xFF, 0x84,
    0xC0, 0x74, 0x01, 0xCC, 0xB8, 0x06, 0x00, 0x00, 0x00, 0x48, 0x83, 0xC4,
    0x28, 0xC3, 0x8B, 0x01 };
constexpr std::uint8_t DataContextEntry[]{
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x85, 0xC9, 0x75, 0x1A, 0x88, 0x4C, 0x24,
    0x30, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8, 0x49, 0xC7, 0xFF, 0xFF, 0x84,
    0xC0, 0x74, 0x01, 0xCC, 0x32, 0xC0, 0x48, 0x83, 0xC4, 0x28, 0xC3, 0x0F,
    0xB6, 0x81, 0xBD, 0x01 };

constexpr std::uint8_t ChangePlayerModeEntry[]{
    0x48, 0x89, 0x5C, 0x24, 0x20, 0x4C, 0x89, 0x44, 0x24, 0x18, 0x55, 0x56,
    0x57, 0x48, 0x83, 0xEC, 0x30, 0x41, 0x8B, 0xF1, 0x48, 0x8B, 0xDA, 0x48,
    0x8B, 0xF9, 0x48, 0x85, 0xC9, 0x75, 0x13 };
constexpr std::uint8_t GetClassIdEntry[]{
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x85, 0xC9, 0x75, 0x1D, 0x88, 0x4C, 0x24,
    0x30, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8, 0x49, 0xCB, 0xFF, 0xFF, 0x84,
    0xC0, 0x74, 0x01, 0xCC, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0x48, 0x83, 0xC4,
    0x28, 0xC3, 0x8B, 0x41, 0x04 };
constexpr std::uint8_t GetInventoryEntry[]{
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
    0xF1, 0x48, 0x85, 0xC9, 0x75, 0x13, 0x88, 0x4C, 0x24, 0x30, 0x48, 0x8D,
    0x4C, 0x24, 0x30, 0xE8, 0x70, 0xCC, 0xFF, 0xFF, 0x84, 0xC0, 0x74, 0x01,
    0xCC };
constexpr std::uint8_t BuildAnimNameEntry[]{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x55, 0x57,
    0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8B, 0xEC, 0x48, 0x83, 0xEC,
    0x60, 0x41, 0x8B, 0xD8, 0x44, 0x8B, 0xF2, 0x4C, 0x8B, 0xE9, 0x45, 0x85,
    0xC9, 0x0F, 0x84, 0xC8, 0x03, 0x00, 0x00 };
constexpr std::uint8_t FindAnimRecordEntry[]{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48,
    0x83, 0xEC, 0x20, 0x44, 0x0F, 0xB6, 0x02, 0x48, 0x8B, 0xFA, 0x4C, 0x8B,
    0xCA, 0x45, 0x84, 0xC0, 0x74, 0x1E };

struct Callable {
    std::uint64_t       rva;
    const std::uint8_t* bytes;
    std::uint32_t       size;
    const char*         name;
};

template <std::size_t N>
constexpr auto C(std::uint64_t rva, const std::uint8_t (&bytes)[N], const char* name) -> Callable {
    return { rva, bytes, static_cast<std::uint32_t>(N), name };
}

constexpr std::array<Callable, 15> Callables{
    C(CompositeRva,          CompositeEntry,          "composite weapon class 3B6E00"),
    C(GetItemFromBodyLocRva, GetItemFromBodyLocEntry, "item from body location 3886D0"),
    C(ItemUsableRva,         ItemUsableEntry,         "item usable 36AD20"),
    C(CheckItemTypeRva,      CheckItemTypeEntry,      "item type check 373890"),
    C(GetWieldTypeRva,       GetWieldTypeEntry,       "wield type 388910"),
    C(TestUnitFlagExRva,     TestUnitFlagExEntry,     "unit flag test 34FB00"),
    C(ConvertTokenRva,       ConvertTokenEntry,       "shapeshift token conversion 33A1F0"),
    C(GetAnimModeRva,        GetAnimModeEntry,        "unit mode 3494F0"),
    C(GetUnitTypeRva,        GetUnitTypeEntry,        "unit type 34B9D0"),
    C(DataContextRva,        DataContextEntry,        "unit data context 34A0E0"),
    C(ChangePlayerModeRva,   ChangePlayerModeEntry,   "player mode change 42D2C0"),
    C(GetClassIdRva,         GetClassIdEntry,         "unit class id 349860"),
    C(GetInventoryRva,       GetInventoryEntry,       "unit inventory 34A360"),
    C(BuildAnimNameRva,      BuildAnimNameEntry,      "animation name builder 3B6700"),
    C(FindAnimRecordRva,     FindAnimRecordEntry,     "animation table lookup 3E3AD0"),
};

// The two weapon-class calls. Each witness runs from the "mov rdx, rax" after
// the inventory call to the instruction that reads the answer, so the loader
// proves the argument set-up, the call and the compare to 13 all at once.
constexpr std::size_t CallSize = 5;

constexpr std::uint8_t ServerWitness[]{
    0x48, 0x8B, 0xD0,                                // 44EFD8 mov rdx, rax
    0xC7, 0x44, 0x24, 0x20, 0x01, 0x00, 0x00, 0x00,  // 44EFDB mov dword [rsp+20h], 1
    0x41, 0xB9, 0xFF, 0xFF, 0xFF, 0xFF,              // 44EFE3 mov r9d, -1
    0x4C, 0x8D, 0x44, 0x24, 0x68,                    // 44EFE9 lea r8, [rsp+68h]
    0x48, 0x8B, 0xCF,                                // 44EFEE mov rcx, rdi
    0xE8, 0x0A, 0x7E, 0xF6, 0xFF,                    // 44EFF1 call 3B6E00
    0x83, 0x7C, 0x24, 0x68, 0x0D,                    // 44EFF6 cmp dword [rsp+68h], 0Dh
    0x75, 0x66,                                      // 44EFFB jne 44F063
};
constexpr std::uint8_t DisplayWitness[]{
    0x48, 0x8B, 0xD0,                                // 1514D25 mov rdx, rax
    0xC7, 0x44, 0x24, 0x20, 0x01, 0x00, 0x00, 0x00,  // 1514D28 mov dword [rsp+20h], 1
    0x45, 0x8D, 0x4F, 0xFF,                          // 1514D30 lea r9d, [r15-1]
    0x48, 0x8B, 0xCD,                                // 1514D34 mov rcx, rbp
    0x4C, 0x8D, 0x44, 0x24, 0x50,                    // 1514D37 lea r8, [rsp+50h]
    0xE8, 0xBF, 0x20, 0xEA, 0xFE,                    // 1514D3C call 3B6E00
    0x83, 0x7C, 0x24, 0x50, 0x0D,                    // 1514D41 cmp dword [rsp+50h], 0Dh
    0x41, 0x0F, 0x45, 0xF7,                          // 1514D46 cmovne esi, r15d
};

// The block animation start. The witness runs from the argument set-up to the
// lastblockframe write that follows, so the loader proves the mode (9), the
// call and what comes after it.
constexpr std::uint8_t BlockAnimationWitness[]{
    0x33, 0xF6,                                      // 44AEA5 xor esi, esi
    0x45, 0x33, 0xC0,                                // 44AEA7 xor r8d, r8d
    0x89, 0x74, 0x24, 0x38,                          // 44AEAA mov [rsp+38h], esi
    0x48, 0x8B, 0xD7,                                // 44AEAE mov rdx, rdi
    0x89, 0x74, 0x24, 0x30,                          // 44AEB1 mov [rsp+30h], esi
    0x48, 0x8B, 0xCD,                                // 44AEB5 mov rcx, rbp
    0x89, 0x74, 0x24, 0x28,                          // 44AEB8 mov [rsp+28h], esi
    0x44, 0x8D, 0x4E, 0x09,                          // 44AEBC lea r9d, [rsi+9]
    0x89, 0x74, 0x24, 0x20,                          // 44AEC0 mov [rsp+20h], esi
    0xE8, 0xF7, 0x23, 0xFE, 0xFF,                    // 44AEC4 call 42D2C0
    0x44, 0x8B, 0x85, 0x70, 0x01, 0x00, 0x00,        // 44AEC9 mov r8d, [rbp+170h]
    0x8D, 0x56, 0x5F,                                // 44AED0 lea edx, [rsi+5Fh]
    0x45, 0x33, 0xC9,                                // 44AED3 xor r9d, r9d
    0x48, 0x8B, 0xCF,                                // 44AED6 mov rcx, rdi
    0xE8, 0x32, 0xCE, 0xEA, 0xFF,                    // 44AED9 call 2F7D10
};

enum class SiteKind : std::uint8_t { Server, Display, BlockAnimation };
constexpr std::size_t KindCount = 3;

struct Site {
    SiteKind            kind;
    const char*         name;
    std::uint64_t       witnessRva;
    const std::uint8_t* witness;
    std::uint32_t       witnessSize;
    std::uint64_t       callRva;
};

constexpr std::array<Site, KindCount> Sites{ {
    { SiteKind::Server,  "server weapon block roll", 0x44EFD8, ServerWitness,
      static_cast<std::uint32_t>(sizeof(ServerWitness)), 0x44EFF1 },
    { SiteKind::Display, "character screen chance to block", 0x1514D25, DisplayWitness,
      static_cast<std::uint32_t>(sizeof(DisplayWitness)), 0x1514D3C },
    { SiteKind::BlockAnimation, "player block animation", 0x44AEA5, BlockAnimationWitness,
      static_cast<std::uint32_t>(sizeof(BlockAnimationWitness)), 0x44AEC4 },
} };

static_assert(0x44EFF1 - 0x44EFD8 + CallSize <= sizeof(ServerWitness));
static_assert(0x1514D3C - 0x1514D25 + CallSize <= sizeof(DisplayWitness));
static_assert(0x44AEC4 - 0x44AEA5 + CallSize <= sizeof(BlockAnimationWitness));

// In-image stubs. 3B6F20 is the ret of the composite front end, followed by
// 15 int3 up to the next function at 3B6F30: exactly three 5-byte stubs,
// server at 3B6F21, display at 3B6F26, block animation at 3B6F2B.
constexpr std::uint64_t PaddingRva = 0x3B6F20;
constexpr std::uint8_t  PaddingExpected[]{
    0xC3, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
    0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC };
constexpr std::array<std::uint64_t, KindCount> StubRva{ 0x3B6F21, 0x3B6F26, 0x3B6F2B };
constexpr std::uint8_t  StubSlotExpected[CallSize]{ 0xCC, 0xCC, 0xCC, 0xCC, 0xCC };
static_assert(StubRva[0] > PaddingRva && StubRva[1] == StubRva[0] + CallSize);
static_assert(StubRva[2] == StubRva[1] + CallSize);
static_assert(StubRva[2] + CallSize <= PaddingRva + sizeof(PaddingExpected));

// Relay page: one absolute jump per site, FF 25 00 00 00 00 <abs64>.
constexpr std::size_t RelayPageBytes   = 4'096;
constexpr std::size_t JumpTargetOffset = 6;
constexpr std::array<std::size_t, KindCount> RelayOffset{ 0, 16, 32 };

// What each relay forwards to once it is retargeted to stock code.
constexpr std::array<std::uint64_t, KindCount> VanillaRva{ CompositeRva, CompositeRva, ChangePlayerModeRva };

// Game values used by the rule.
constexpr std::int32_t  PlayerUnitType    = 0;
constexpr std::uint32_t ShapeshiftedFlag  = 0x8;
constexpr std::int32_t  BodyLocRightArm   = 4;
constexpr std::int32_t  BodyLocLeftArm    = 5;
constexpr std::int32_t  WieldTwoHanded    = 2;
constexpr std::int32_t  WeaponClassDual   = 13;   // "ht2", what both sites test for
constexpr std::int32_t  WeaponClassHth    = 0;    // any class other than 13 means "no"
constexpr std::int32_t  ModeFromUnit      = -1;
constexpr std::uint64_t AnimNameBytes     = 8;

constexpr std::size_t MaximumItemTypes   = 8;
constexpr std::size_t MaximumConfigBytes = 32'768;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

struct Config {
    bool                     enabled{ true };
    std::vector<std::string> clawTypes{ "h2h" };
    std::vector<std::string> swordTypes{ "swor" };
};

constexpr char DefaultConfigToml[] =
    "# Weapon Block Rules\n"
    "#\n"
    "# Decides WHICH WEAPONS let passive_weaponblock (itemstatcost.txt row 348)\n"
    "# roll. Stock D2R only allows an Assassin holding a weapon in each hand.\n"
    "# With this plugin, a character of ANY class can weapon block while holding:\n"
    "#\n"
    "#   - a claw in each hand, or\n"
    "#   - a sword in each hand, or\n"
    "#   - one sword wielded with both hands and nothing in the other hand.\n"
    "#\n"
    "# Never counts: one claw, a claw and a sword, a one-handed sword on its own,\n"
    "# and a sword next to a shield, including a Barbarian's two-handed sword\n"
    "# held in one hand next to a shield. Broken or disabled weapons never count.\n"
    "#\n"
    "# Everything else about weapon block stays stock:\n"
    "#   - the chance is the passive_weaponblock total, rolled 0 to 99, so 100 or\n"
    "#     more always blocks; it is not capped at 75 like shield block\n"
    "#   - shield block rolls first; weapon block only rolls if that fails\n"
    "#   - no weapon block while walking or running\n"
    "#   - a melee skill with alwayshit=1 in skills.txt skips the roll\n"
    "#   - monsters and hirelings keep the stock rule\n"
    "#   - a player shapeshifted into a monster form (werewolf, werebear) keeps\n"
    "#     the stock rule; a form that is a player class uses this plugin's rule\n"
    "#\n"
    "# The stat's param still works as before: param 0 always counts, any other\n"
    "# param counts only while a hand holds that itemtypes.txt row. When several\n"
    "# params qualify, the highest total wins; they do not add up.\n"
    "#\n"
    "# DATA NOTE: vanilla Weapon Block has passiveitype = h2h in skills.txt, so\n"
    "# its value only counts while a claw is in hand and a sword wielder gets\n"
    "# nothing from the skill. Leave passiveitype blank on that skill for swords\n"
    "# to use it; this plugin still requires the claw pair or the swords.\n"
    "#\n"
    "# A block only plays the character's block animation when the game has one\n"
    "# for the weapons in hand. Stock data has none for some sword setups, for\n"
    "# example AIBL2HS (an Assassin with a two-handed sword), so those blocks\n"
    "# happen without an animation instead of freezing the character. If you add\n"
    "# a block animation for that weapon class later, it plays.\n"
    "#\n"
    "# The character screen's chance-to-block line follows the same rule.\n"
    "# Console command: wbrules (status and counters).\n"
    "\n"
    "# Master switch. false = stock behaviour, nothing is patched.\n"
    "enabled = true\n"
    "\n"
    "# itemtypes.txt codes that count as claws. Both hands must hold one.\n"
    "# A code also covers the types under it (h2h covers h2h2).\n"
    "claw_item_types = [\"h2h\"]\n"
    "\n"
    "# itemtypes.txt codes that count as swords: one in each hand, or a single\n"
    "# one wielded with both hands.\n"
    "sword_item_types = [\"swor\"]\n";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

enum class HookState : std::uint8_t { NotLoaded, DisabledByConfig, Armed, PartiallyArmed };

enum class Match : std::uint8_t { Unresolved, None, ClawPair, SwordPair, TwoHandedSword };
constexpr std::size_t MatchCount = 5;

using CompositeFn          = std::int32_t(__fastcall*)(void* unit, void* inventory, std::int32_t* weaponClass,
                                                       std::int32_t mode, std::int32_t applyShapeshiftMode);
using GetItemFromBodyLocFn = void*(__fastcall*)(void* inventory, std::int32_t bodyLoc);
using ItemUsableFn         = std::int32_t(__fastcall*)(void* item);
using CheckItemTypeFn      = std::int32_t(__fastcall*)(void* item, std::int32_t itemType);
using GetWieldTypeFn       = std::int32_t(__fastcall*)(void* unit, void* inventory, std::uint8_t policy);
using TestUnitFlagExFn     = std::int32_t(__fastcall*)(void* unit, std::uint32_t mask);
using ConvertTokenFn       = std::int64_t(__fastcall*)(void* unit, std::int32_t* unitType, std::uint32_t* classId,
                                                       std::int32_t* mode, const char* file, std::int32_t line);
using GetAnimModeFn        = std::int32_t(__fastcall*)(void* unit);
using GetUnitTypeFn        = std::int32_t(__fastcall*)(void* unit);
using DataContextFn        = std::uint8_t(__fastcall*)(void* unit);
using ChangePlayerModeFn   = std::int64_t(__fastcall*)(void* game, void* unit, void* target, std::uint32_t mode,
                                                       std::uint32_t a5, std::int32_t a6, std::int32_t a7,
                                                       std::int32_t a8);
using GetClassIdFn         = std::int32_t(__fastcall*)(void* unit, const char* file, std::int32_t line);
using GetInventoryFn       = void*(__fastcall*)(void* unit, const char* file, std::int32_t line);
using BuildAnimNameFn      = std::int64_t(__fastcall*)(void* unit, std::uint32_t classId, std::int32_t mode,
                                                       std::int32_t unitType, void* inventory, char* name,
                                                       std::uint64_t nameSize, std::int32_t* weaponClassCode,
                                                       std::int32_t withPath, std::int32_t withWeaponClass,
                                                       std::uint8_t bank);
using FindAnimRecordFn     = void*(__fastcall*)(std::uint32_t bank, char* name);

const D2RL::PluginContext*     Context{};
std::uint8_t*                  Base{};
Config                         Settings{};
HookState                      State{ HookState::NotLoaded };
const D2RL::DataTableService*  DataTables{};
void*                          RelayPage{};
std::array<bool, KindCount>    StubWritten{};
std::array<bool, KindCount>    Patched{};

CompositeFn          Composite{};
GetItemFromBodyLocFn GetItemFromBodyLoc{};
ItemUsableFn         ItemUsable{};
CheckItemTypeFn      CheckItemType{};
GetWieldTypeFn       GetWieldType{};
TestUnitFlagExFn     TestUnitFlagEx{};
ConvertTokenFn       ConvertToken{};
GetAnimModeFn        GetAnimMode{};
GetUnitTypeFn        GetUnitType{};
DataContextFn        DataContext{};
ChangePlayerModeFn   ChangePlayerMode{};
GetClassIdFn         GetClassId{};
GetInventoryFn       GetInventory{};
BuildAnimNameFn      BuildAnimName{};
FindAnimRecordFn     FindAnimRecord{};
const char*          SourceFile{};

// Resolved itemtypes.txt rows, per data bank (index 1..3 = the unit's data
// context byte). Written on every table load, read by the hook.
struct TypeList {
    std::array<std::atomic<std::int32_t>, MaximumItemTypes> ids{};
    std::atomic<std::uint32_t> count{};
};
struct BankTypes {
    TypeList          claws;
    TypeList          swords;
    std::atomic<bool> ready{};
};
std::array<BankTypes, 4> Banks{};

std::array<std::array<std::atomic<std::uint64_t>, MatchCount>, KindCount> Answers{};
std::array<std::atomic<std::uint64_t>, KindCount> Refused{};      // stock said 13, the rule said no
std::array<std::atomic<std::uint64_t>, KindCount> MonsterForm{};  // shapeshifted into a monster form
std::atomic<std::uint64_t> BlockAnimationsPlayed{};
std::atomic<std::uint64_t> BlockAnimationsSkipped{};

auto KindIndex(SiteKind kind) noexcept -> std::size_t { return static_cast<std::size_t>(kind); }

template <typename T>
auto At(std::uint64_t rva) noexcept -> T {
    return reinterpret_cast<T>(Base + rva);
}

// ---------------------------------------------------------------------------
// Config parsing (small hand-rolled TOML subset, no external dependency)
// ---------------------------------------------------------------------------

auto Trim(std::string_view value) noexcept -> std::string_view {
    std::size_t first = 0;
    while (first < value.size()
            && (value[first] == ' ' || value[first] == '\t' || value[first] == '\r' || value[first] == '\n')) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first
            && (value[last - 1] == ' ' || value[last - 1] == '\t' || value[last - 1] == '\r' || value[last - 1] == '\n')) {
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

// ["a", "b"] -> codes of 1 to 4 characters each.
auto ParseCodeList(std::string_view value, std::vector<std::string>& out) -> bool {
    if (value.size() < 2 || value.front() != '[' || value.back() != ']') return false;
    std::string_view inner = Trim(value.substr(1, value.size() - 2));
    std::vector<std::string> codes;
    while (!inner.empty()) {
        const std::size_t comma = inner.find(',');
        const std::string_view item = Trim(inner.substr(0, comma));
        if (!item.empty()) {
            if (item.size() < 3 || item.front() != '"' || item.back() != '"') return false;
            const std::string_view code = item.substr(1, item.size() - 2);
            if (code.empty() || code.size() > 4) return false;
            codes.emplace_back(code);
        }
        if (comma == std::string_view::npos) break;
        inner = inner.substr(comma + 1);
    }
    if (codes.size() > MaximumItemTypes) return false;
    out = std::move(codes);
    return true;
}

void ApplyConfigLine(std::string_view key, std::string_view value) {
    bool ok = true;
    if (key == "enabled") {
        ok = ParseBool(value, Settings.enabled);
    } else if (key == "claw_item_types") {
        ok = ParseCodeList(value, Settings.clawTypes);
    } else if (key == "sword_item_types") {
        ok = ParseCodeList(value, Settings.swordTypes);
    }
    if (!ok) {
        D2RL::LogWarnF(Context, "WeaponBlockRules: config line '%.*s' has an invalid value, default kept.",
            static_cast<int>(key.size()), key.data());
    }
}

void ParseConfig(std::string_view text) {
    std::size_t cursor = 0;
    while (cursor <= text.size()) {
        const std::size_t breakAt = text.find('\n', cursor);
        const std::size_t end = breakAt == std::string_view::npos ? text.size() : breakAt;
        const std::string_view line = Trim(StripComment(text.substr(cursor, end - cursor)));
        cursor = end + 1;
        if (!line.empty() && line.front() != '[') {
            const std::size_t equals = line.find('=');
            if (equals != std::string_view::npos) {
                ApplyConfigLine(Trim(line.substr(0, equals)), Trim(line.substr(equals + 1)));
            }
        }
        if (breakAt == std::string_view::npos) break;
    }
}

void ReadConfiguration() {
    if (!Context->EnsureConfig(DefaultConfigToml)) {
        Context->LogWarn("WeaponBlockRules: config file could not be created; using defaults.");
        return;
    }
    std::string buffer(MaximumConfigBytes, '\0');
    if (!Context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size()), nullptr)) {
        Context->LogWarn("WeaponBlockRules: config file could not be read; using defaults.");
        return;
    }
    buffer.resize(std::strlen(buffer.c_str()));
    ParseConfig(buffer);
}

// ---------------------------------------------------------------------------
// itemtypes.txt codes -> rows, on every table load
// ---------------------------------------------------------------------------

auto FourCc(std::string_view code, char pad) noexcept -> std::uint32_t {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        const auto c = static_cast<std::uint8_t>(i < code.size() ? code[i] : pad);
        value |= static_cast<std::uint32_t>(c) << (8 * i);
    }
    return value;
}

// The lookup compares the stored code exactly. D2RCore documents its codes as
// zero-padded; a code shorter than 4 characters is also tried space-padded, so
// the result does not depend on which padding the compiled rows use.
auto FindItemType(const D2RL::PluginContext* context, D2RL::DataTables::Bank bank, std::string_view code,
                  std::int32_t& rowIndex) noexcept -> bool {
    const char pads[2]{ '\0', ' ' };
    for (const char pad : pads) {
        D2RL::DataTables::RowView row{};
        row.structSize = D2RL::DataTables::RowViewSize;
        const auto result = DataTables->findRowByCode(context, bank, D2RL::DataTables::TableId::ItemTypes,
                                                      FourCc(code, pad), &row);
        if (result == D2RL::DataTables::Result::Success
                && D2RL::DataTables::HasRowViewField(&row, D2RL::DataTables::RowViewRequiredSize)) {
            rowIndex = static_cast<std::int32_t>(row.rowIndex);
            return true;
        }
        if (code.size() >= 4) break;
    }
    return false;
}

auto ResolveList(const D2RL::PluginContext* context, D2RL::DataTables::Bank bank,
                 const std::vector<std::string>& codes, TypeList& list, std::string& missing) -> std::uint32_t {
    std::uint32_t count = 0;
    for (const std::string& code : codes) {
        std::int32_t rowIndex = -1;
        if (count < MaximumItemTypes && FindItemType(context, bank, code, rowIndex)) {
            list.ids[count].store(rowIndex, std::memory_order_relaxed);
            ++count;
        } else {
            if (!missing.empty()) missing += ", ";
            missing += code;
        }
    }
    list.count.store(count, std::memory_order_release);
    return count;
}

void ResolveItemTypes(const D2RL::PluginContext* context) {
    if (DataTables == nullptr) return;
    for (std::uint32_t bank = 1; bank <= 3; ++bank) {
        BankTypes& types = Banks[bank];
        types.ready.store(false, std::memory_order_release);
        std::string missing;
        const auto tableBank = static_cast<D2RL::DataTables::Bank>(bank);
        const std::uint32_t claws  = ResolveList(context, tableBank, Settings.clawTypes, types.claws, missing);
        const std::uint32_t swords = ResolveList(context, tableBank, Settings.swordTypes, types.swords, missing);
        types.ready.store(true, std::memory_order_release);
        if (missing.empty()) {
            D2RL::LogInfoF(context, "WeaponBlockRules: bank %u: %u claw and %u sword item types found.",
                bank, claws, swords);
        } else {
            D2RL::LogWarnF(context, "WeaponBlockRules: bank %u: %u claw and %u sword item types found; "
                "not in itemtypes.txt: %s.", bank, claws, swords, missing.c_str());
        }
    }
}

void __cdecl OnDataTablesLoaded(const D2RL::PluginContext* context,
                                const D2RL::Lifecycle::DataTablesLoadedEvent*, void*) noexcept {
    ResolveItemTypes(context);
}

// ---------------------------------------------------------------------------
// The rule
// ---------------------------------------------------------------------------

auto IsAnyOf(void* item, const TypeList& list) noexcept -> bool {
    const std::uint32_t count = list.count.load(std::memory_order_acquire);
    for (std::uint32_t i = 0; i < count && i < MaximumItemTypes; ++i) {
        if (CheckItemType(item, list.ids[i].load(std::memory_order_relaxed)) != 0) return true;
    }
    return false;
}

auto Usable(void* item) noexcept -> bool {
    return item != nullptr && ItemUsable(item) != 0;
}

// What the defender holds, judged by ESR's rule.
auto HeldWeapons(void* unit, void* inventory) noexcept -> Match {
    const std::uint8_t context = DataContext(unit);
    if (context < 1 || context > 3) return Match::Unresolved;
    const BankTypes& types = Banks[context];
    if (!types.ready.load(std::memory_order_acquire)) return Match::Unresolved;
    if (inventory == nullptr) return Match::None;

    void* right = GetItemFromBodyLoc(inventory, BodyLocRightArm);
    void* left  = GetItemFromBodyLoc(inventory, BodyLocLeftArm);

    // Both hands occupied: a pair of claws or a pair of swords, nothing else.
    if (right != nullptr && left != nullptr) {
        if (!Usable(right) || !Usable(left)) return Match::None;
        if (IsAnyOf(right, types.claws) && IsAnyOf(left, types.claws)) return Match::ClawPair;
        if (IsAnyOf(right, types.swords) && IsAnyOf(left, types.swords)) return Match::SwordPair;
        return Match::None;
    }

    // One hand occupied: a single sword, wielded with both hands. The game's
    // own wield type answers "both hands" for a two-handed sword, and for a
    // Barbarian's two-handed sword only while the other hand is empty.
    void* held = right != nullptr ? right : left;
    if (!Usable(held) || !IsAnyOf(held, types.swords)) return Match::None;
    return GetWieldType(unit, inventory, 0) == WieldTwoHanded ? Match::TwoHandedSword : Match::None;
}

// Same effective token as the composite front end: a shapeshifted player may
// stand in a monster form (states.txt gfxtype 1), whose stock answer is kept.
auto InMonsterForm(void* unit, std::int32_t mode) noexcept -> bool {
    if (TestUnitFlagEx(unit, ShapeshiftedFlag) == 0) return false;
    std::int32_t  unitType = PlayerUnitType;
    std::uint32_t classId  = 0;
    std::int32_t  form     = mode == ModeFromUnit ? GetAnimMode(unit) : mode;
    ConvertToken(unit, &unitType, &classId, &form, SourceFile, CompositConvertLine);
    return unitType != PlayerUnitType;
}

auto Answer(SiteKind kind, void* unit, void* inventory, std::int32_t* weaponClass, std::int32_t mode,
            std::int32_t applyShapeshiftMode) noexcept -> std::int32_t {
    const std::int32_t code = Composite(unit, inventory, weaponClass, mode, applyShapeshiftMode);
    if (unit == nullptr || weaponClass == nullptr) return code;
    if (GetUnitType(unit) != PlayerUnitType) return code;

    const std::size_t k = KindIndex(kind);
    if (InMonsterForm(unit, mode)) {
        MonsterForm[k].fetch_add(1, std::memory_order_relaxed);
        return code;
    }

    const Match match = HeldWeapons(unit, inventory);
    Answers[k][static_cast<std::size_t>(match)].fetch_add(1, std::memory_order_relaxed);
    if (match == Match::Unresolved) return code;
    if (match != Match::None) {
        *weaponClass = WeaponClassDual;
    } else if (*weaponClass == WeaponClassDual) {
        *weaponClass = WeaponClassHth;
        Refused[k].fetch_add(1, std::memory_order_relaxed);
    }
    return code;
}

// Reached through the stub and the relay with the composite's own ABI, one
// entry point per site. The caller only reads the out value, and only to
// compare it to 13.
std::int32_t __fastcall ServerWeaponClass(void* unit, void* inventory, std::int32_t* weaponClass,
                                          std::int32_t mode, std::int32_t applyShapeshiftMode) noexcept {
    return Answer(SiteKind::Server, unit, inventory, weaponClass, mode, applyShapeshiftMode);
}

std::int32_t __fastcall DisplayWeaponClass(void* unit, void* inventory, std::int32_t* weaponClass,
                                           std::int32_t mode, std::int32_t applyShapeshiftMode) noexcept {
    return Answer(SiteKind::Display, unit, inventory, weaponClass, mode, applyShapeshiftMode);
}

// Whether the game has an animation for this unit in this mode, built the way
// the mode change itself builds it: the unit's type and class, shapeshift
// token conversion, the weapon class of the hands, then the animation table.
// A miss here is exactly the case where the game would log "Failed to
// retrieve animation file" and fall back to its 2048-frame placeholder.
auto HasAnimation(void* unit, std::int32_t mode) noexcept -> bool {
    std::int32_t  unitType = GetUnitType(unit);
    std::uint32_t classId  = static_cast<std::uint32_t>(GetClassId(unit, SourceFile, CompositConvertLine));
    std::int32_t  animMode = mode;
    ConvertToken(unit, &unitType, &classId, &animMode, SourceFile, CompositConvertLine);
    void* inventory = GetInventory(unit, SourceFile, CompositConvertLine);
    const std::uint8_t bank = DataContext(unit);
    char name[AnimNameBytes]{};
    std::int32_t weaponClassCode = 0;
    BuildAnimName(unit, classId, animMode, unitType, inventory, name, AnimNameBytes, &weaponClassCode, 0, 1, bank);
    return FindAnimRecord(bank, name) != nullptr;
}

// Reached through the stub and the relay with the mode change's own ABI.
// The caller does not read the return value.
std::int64_t __fastcall BlockAnimationStart(void* game, void* unit, void* target, std::uint32_t mode,
                                            std::uint32_t a5, std::int32_t a6, std::int32_t a7,
                                            std::int32_t a8) noexcept {
    if (unit != nullptr && !HasAnimation(unit, static_cast<std::int32_t>(mode))) {
        BlockAnimationsSkipped.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    BlockAnimationsPlayed.fetch_add(1, std::memory_order_relaxed);
    return ChangePlayerMode(game, unit, target, mode, a5, a6, a7, a8);
}

// ---------------------------------------------------------------------------
// Relay, stub and call sites
// ---------------------------------------------------------------------------

auto CanEncodeRel32(std::uintptr_t from, std::uintptr_t to) noexcept -> bool {
    const std::int64_t delta = static_cast<std::int64_t>(to) - static_cast<std::int64_t>(from) - CallSize;
    return delta >= INT32_MIN && delta <= INT32_MAX;
}

auto AllocateNear(std::uintptr_t hint, std::size_t size) noexcept -> void* {
    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    const auto granularity = static_cast<std::uintptr_t>(systemInfo.dwAllocationGranularity);
    const auto aligned = hint & ~(granularity - 1U);
    for (std::uintptr_t delta = granularity; delta < 0x7000'0000ULL; delta += granularity) {
        const auto candidate = aligned + delta;
        if (!CanEncodeRel32(hint, candidate)) break;
        if (auto* memory = VirtualAlloc(reinterpret_cast<void*>(candidate), size,
                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) {
            return memory;
        }
    }
    return nullptr;
}

auto EncodeRel32(std::uint8_t opcode, std::uintptr_t from, std::uintptr_t to) noexcept
        -> std::array<std::uint8_t, CallSize> {
    std::array<std::uint8_t, CallSize> bytes{ opcode };
    const auto displacement = static_cast<std::int32_t>(
        static_cast<std::int64_t>(to) - static_cast<std::int64_t>(from + CallSize));
    std::memcpy(bytes.data() + 1, &displacement, sizeof(displacement));
    return bytes;
}

auto OriginalCallBytes(const Site& site) noexcept -> const std::uint8_t* {
    return site.witness + (site.callRva - site.witnessRva);
}

auto RelayAddress(SiteKind kind) noexcept -> std::uintptr_t {
    return reinterpret_cast<std::uintptr_t>(RelayPage) + RelayOffset[KindIndex(kind)];
}

void WriteRelay(SiteKind kind, std::uint64_t target) noexcept {
    static constexpr std::uint8_t JumpQwordRip[]{ 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
    auto* slot = reinterpret_cast<std::uint8_t*>(RelayAddress(kind));
    std::memcpy(slot, JumpQwordRip, sizeof(JumpQwordRip));
    std::memcpy(slot + JumpTargetOffset, &target, sizeof(target));
}

// Points both relays at the stock composite. Anything still calling a stub
// then runs native code only.
auto RetargetRelaysToVanilla() noexcept -> bool {
    if (!RelayPage) return true;
    DWORD previous = 0;
    if (!VirtualProtect(RelayPage, RelayPageBytes, PAGE_EXECUTE_READWRITE, &previous)) return false;
    for (std::size_t k = 0; k < KindCount; ++k) {
        WriteRelay(static_cast<SiteKind>(k), reinterpret_cast<std::uint64_t>(Base) + VanillaRva[k]);
    }
    DWORD ignored = 0;
    VirtualProtect(RelayPage, RelayPageBytes, previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), RelayPage, RelayPageBytes);
    return true;
}

// Writes the original call back over every patched site.
auto RestoreSites() noexcept -> bool {
    const auto imageBase = reinterpret_cast<std::uintptr_t>(Base);
    bool restored = true;
    for (std::size_t i = Sites.size(); i-- > 0;) {
        if (!Patched[i]) continue;
        const Site& site = Sites[i];
        const auto current = EncodeRel32(0xE8, imageBase + site.callRva, imageBase + StubRva[KindIndex(site.kind)]);
        if (Context->PatchBytes(site.callRva, current.data(), CallSize, OriginalCallBytes(site), CallSize)) {
            Patched[i] = false;
        } else {
            restored = false;
        }
    }
    return restored;
}

// Puts the int3 padding back once no site calls a stub any more.
auto RestoreStubs() noexcept -> bool {
    const auto imageBase = reinterpret_cast<std::uintptr_t>(Base);
    bool restored = true;
    for (std::size_t k = 0; k < KindCount; ++k) {
        if (!StubWritten[k]) continue;
        const auto kind = static_cast<SiteKind>(k);
        const auto current = EncodeRel32(0xE9, imageBase + StubRva[k], RelayAddress(kind));
        if (Context->PatchBytes(StubRva[k], current.data(), CallSize, StubSlotExpected, CallSize)) {
            StubWritten[k] = false;
        } else {
            restored = false;
        }
    }
    return restored;
}

void ReleaseRelayPage() noexcept {
    if (RelayPage) VirtualFree(RelayPage, 0, MEM_RELEASE);
    RelayPage = nullptr;
}

auto VerifyContract() noexcept -> bool {
    for (const Callable& callable : Callables) {
        if (!Context->CheckExpectedBytes(callable.rva, callable.bytes, callable.size)) {
            D2RL::LogErrorF(Context, "WeaponBlockRules: the %s does not match the verified D2R image. "
                "Refusing to load.", callable.name);
            return false;
        }
    }
    if (!Context->CheckExpectedBytes(CompositSourceFileRva, reinterpret_cast<const std::uint8_t*>(CompositSourceFile),
            static_cast<std::uint32_t>(sizeof(CompositSourceFile)))) {
        Context->LogError("WeaponBlockRules: the Composit.cpp source path at 0x1D0AED0 does not match. "
            "Refusing to load.");
        return false;
    }
    if (!Context->CheckExpectedBytes(PaddingRva, PaddingExpected, static_cast<std::uint32_t>(sizeof(PaddingExpected)))) {
        Context->LogError("WeaponBlockRules: the int3 padding at 0x3B6F21 is not free in this build, or another "
            "plugin already uses it. Refusing to load.");
        return false;
    }
    for (const Site& site : Sites) {
        if (!Context->CheckExpectedBytes(site.witnessRva, site.witness, site.witnessSize)) {
            D2RL::LogErrorF(Context, "WeaponBlockRules: the %s call at 0x%llX does not match the verified D2R "
                "image, or another plugin already owns it. Refusing to load.",
                site.name, static_cast<unsigned long long>(site.callRva));
            return false;
        }
    }
    return true;
}

void BindGameFunctions() noexcept {
    Composite          = At<CompositeFn>(CompositeRva);
    GetItemFromBodyLoc = At<GetItemFromBodyLocFn>(GetItemFromBodyLocRva);
    ItemUsable         = At<ItemUsableFn>(ItemUsableRva);
    CheckItemType      = At<CheckItemTypeFn>(CheckItemTypeRva);
    GetWieldType       = At<GetWieldTypeFn>(GetWieldTypeRva);
    TestUnitFlagEx     = At<TestUnitFlagExFn>(TestUnitFlagExRva);
    ConvertToken       = At<ConvertTokenFn>(ConvertTokenRva);
    GetAnimMode        = At<GetAnimModeFn>(GetAnimModeRva);
    GetUnitType        = At<GetUnitTypeFn>(GetUnitTypeRva);
    DataContext        = At<DataContextFn>(DataContextRva);
    ChangePlayerMode   = At<ChangePlayerModeFn>(ChangePlayerModeRva);
    GetClassId         = At<GetClassIdFn>(GetClassIdRva);
    GetInventory       = At<GetInventoryFn>(GetInventoryRva);
    BuildAnimName      = At<BuildAnimNameFn>(BuildAnimNameRva);
    FindAnimRecord     = At<FindAnimRecordFn>(FindAnimRecordRva);
    SourceFile         = reinterpret_cast<const char*>(Base + CompositSourceFileRva);
}

// Returns false only when nothing in the image can reach this DLL any more,
// so the loader may safely unload it.
auto InstallHooks() noexcept -> bool {
    const auto imageBase = reinterpret_cast<std::uintptr_t>(Base);

    // Searched upward from the lower stub, so the higher one is within reach
    // too (both are re-checked before a byte is written).
    RelayPage = AllocateNear(imageBase + StubRva[0], RelayPageBytes);
    bool reachable = RelayPage != nullptr;
    for (std::size_t k = 0; reachable && k < KindCount; ++k) {
        reachable = CanEncodeRel32(imageBase + StubRva[k], RelayAddress(static_cast<SiteKind>(k)));
    }
    if (!reachable) {
        Context->LogError("WeaponBlockRules: no relay page was available within rel32 reach of 0x3B6F21.");
        ReleaseRelayPage();
        return false;
    }

    std::memset(RelayPage, 0xCC, RelayPageBytes);
    WriteRelay(SiteKind::Server,         reinterpret_cast<std::uint64_t>(&ServerWeaponClass));
    WriteRelay(SiteKind::Display,        reinterpret_cast<std::uint64_t>(&DisplayWeaponClass));
    WriteRelay(SiteKind::BlockAnimation, reinterpret_cast<std::uint64_t>(&BlockAnimationStart));
    DWORD previous = 0;
    if (!VirtualProtect(RelayPage, RelayPageBytes, PAGE_EXECUTE_READ, &previous)) {
        Context->LogError("WeaponBlockRules: relay page protection could not be finalized.");
        ReleaseRelayPage();
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), RelayPage, RelayPageBytes);

    for (std::size_t k = 0; k < KindCount; ++k) {
        const auto kind = static_cast<SiteKind>(k);
        const auto jump = EncodeRel32(0xE9, imageBase + StubRva[k], RelayAddress(kind));
        if (!Context->PatchBytes(StubRva[k], StubSlotExpected, CallSize, jump.data(), CallSize)
                || !Context->CheckExpectedBytes(StubRva[k], jump.data(), CallSize)) {
            D2RL::LogErrorF(Context, "WeaponBlockRules: the stub at 0x%llX could not be written.",
                static_cast<unsigned long long>(StubRva[k]));
            if (RestoreStubs()) ReleaseRelayPage();
            return false;
        }
        StubWritten[k] = true;
    }

    for (std::size_t i = 0; i < Sites.size(); ++i) {
        const Site& site = Sites[i];
        const std::uint64_t stub = StubRva[KindIndex(site.kind)];
        const auto redirected = EncodeRel32(0xE8, imageBase + site.callRva, imageBase + stub);
        if (Context->PatchCallRel32(site.callRva, OriginalCallBytes(site), CallSize, stub, CallSize)
                && Context->CheckExpectedBytes(site.callRva, redirected.data(), CallSize)) {
            Patched[i] = true;
            continue;
        }
        D2RL::LogErrorF(Context, "WeaponBlockRules: the %s call at 0x%llX could not be redirected.",
            site.name, static_cast<unsigned long long>(site.callRva));

        const bool retargeted = RetargetRelaysToVanilla();
        if (RestoreSites()) {
            if (RestoreStubs()) ReleaseRelayPage();
            return false;
        }
        State = HookState::PartiallyArmed;
        Context->LogError(retargeted
            ? "WeaponBlockRules: some sites could not be restored; they now forward to the stock functions, so "
              "weapon block uses the stock rule."
            : "WeaponBlockRules: rollback failed, staying loaded so the patched sites keep a valid target.");
        return true;
    }

    State = HookState::Armed;
    return true;
}

auto RegisterDataTables() noexcept -> bool {
    if (Context->QueryService(&DataTables) != D2RL::ServiceQueryResult::Success
            || !D2RL::HasDataTableServiceField(DataTables, D2RL::DataTableServiceRequiredSize)) {
        DataTables = nullptr;
        Context->LogError("WeaponBlockRules: the data table service is unavailable.");
        return false;
    }
    const D2RL::LifecycleService* lifecycle = nullptr;
    if (Context->QueryService(&lifecycle) != D2RL::ServiceQueryResult::Success
            || !D2RL::HasLifecycleServiceField(lifecycle, D2RL::LifecycleServiceRequiredSize)) {
        Context->LogError("WeaponBlockRules: the lifecycle service is unavailable.");
        return false;
    }
    D2RL::Lifecycle::DataTablesLoadedListener tables{};
    tables.structSize = D2RL::Lifecycle::DataTablesLoadedListenerSize;
    tables.callback   = &OnDataTablesLoaded;
    D2RL::Lifecycle::ListenerHandle handle = D2RL::Lifecycle::InvalidHandle;
    if (lifecycle->registerDataTablesLoadedListener(Context, &tables, &handle) != D2RL::Lifecycle::Result::Success
            || handle == D2RL::Lifecycle::InvalidHandle) {
        Context->LogError("WeaponBlockRules: the data table listener was refused.");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Console command
// ---------------------------------------------------------------------------

auto StateName() noexcept -> const char* {
    switch (State) {
    case HookState::DisabledByConfig: return "disabled by config";
    case HookState::Armed:            return "armed";
    case HookState::PartiallyArmed:   return "PARTIALLY armed, see log";
    default:                          return "not loaded";
    }
}

auto Count(SiteKind kind, Match match) noexcept -> unsigned long long {
    return static_cast<unsigned long long>(
        Answers[KindIndex(kind)][static_cast<std::size_t>(match)].load(std::memory_order_relaxed));
}

auto __cdecl StatusCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept
        -> D2RL::ConsoleCommandResult {
    if (!command || !command->plugin) return D2RL::ConsoleCommandResult::Failed;

    char types[160];
    std::snprintf(types, sizeof(types), "item types (claw/sword) Classic %u/%u, LoD %u/%u, RotW %u/%u",
        Banks[1].claws.count.load(), Banks[1].swords.count.load(),
        Banks[2].claws.count.load(), Banks[2].swords.count.load(),
        Banks[3].claws.count.load(), Banks[3].swords.count.load());

    char message[512];
    for (const SiteKind kind : { SiteKind::Server, SiteKind::Display }) {
        const std::size_t k = KindIndex(kind);
        std::snprintf(message, sizeof(message),
            "%s: claw pair %llu, sword pair %llu, two-handed sword %llu, no %llu (stock 13 refused %llu), "
            "monster form %llu, types not loaded %llu",
            kind == SiteKind::Server ? "Server roll" : "Character screen",
            Count(kind, Match::ClawPair), Count(kind, Match::SwordPair), Count(kind, Match::TwoHandedSword),
            Count(kind, Match::None), static_cast<unsigned long long>(Refused[k].load()),
            static_cast<unsigned long long>(MonsterForm[k].load()), Count(kind, Match::Unresolved));
        if (kind == SiteKind::Server) {
            char header[256];
            std::snprintf(header, sizeof(header), "Weapon Block Rules: %s | %s", StateName(), types);
            command->plugin->WriteConsoleMessage(header);
        }
        command->plugin->WriteConsoleMessage(message);
    }
    std::snprintf(message, sizeof(message), "Block animation: played %llu, skipped (no animation for the weapons "
        "in hand) %llu",
        static_cast<unsigned long long>(BlockAnimationsPlayed.load()),
        static_cast<unsigned long long>(BlockAnimationsSkipped.load()));
    command->plugin->WriteConsoleMessage(message);
    return D2RL::ConsoleCommandResult::Handled;
}

void RegisterStatusCommand() noexcept {
    if (!Context->RegisterConsoleCommand("wbrules", &StatusCommand,
            "Reports Weapon Block Rules status and per-rule counters.")) {
        Context->LogWarn("WeaponBlockRules: the status console command was refused.");
    }
}

constexpr D2RL::PluginInfo Info{
    .infoSize    = D2RL::PluginInfoSize,
    .abiVersion  = D2RL_PLUGIN_ABI_VERSION,
    .id          = "celestialrayone.weapon-block-rules",
    .name        = "Weapon Block Rules",
    .version     = "1.1.0",
    .author      = "CelestialRayOne",
    .description = "Lets any class weapon block with two claws, two swords, or one sword wielded with both "
                   "hands, in the server roll and on the character screen.",
    .flags       = D2RL::PluginFlags::Shared | D2RL::PluginFlags::NativeHooks,
};

}  // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
    return &Info;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
    if (!D2RL::HasContext(context)) return false;
    Context = context;
    Base = reinterpret_cast<std::uint8_t*>(context->exeBase);

    ReadConfiguration();

    if (!Settings.enabled) {
        State = HookState::DisabledByConfig;
        Context->LogInfo("WeaponBlockRules: disabled by configuration.");
        RegisterStatusCommand();
        return true;
    }

    if (!VerifyContract()) return false;
    BindGameFunctions();
    if (!RegisterDataTables()) return false;
    if (!InstallHooks()) return false;

    D2RL::LogInfoF(Context, "WeaponBlockRules: %s. %zu claw and %zu sword item type code(s) configured; rows "
        "are resolved when the data tables load.", StateName(), Settings.clawTypes.size(),
        Settings.swordTypes.size());
    RegisterStatusCommand();
    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    if (!Context || !RelayPage) return;
    RetargetRelaysToVanilla();
    RestoreSites();
    // The relay page and the stubs are deliberately kept: a thread may be
    // inside a jump through them right now, and any site that could not be
    // restored still needs them to reach the stock functions.
}

}  // namespace CelestialRayOne::WeaponBlockRules
