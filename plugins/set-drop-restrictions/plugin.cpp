// Cow Set Drops
//
// Replaces D2R's hardcoded Cow King's Leathers drop rule with a configurable
// one: a list of monsters (monstats.txt hcIdx) and a list of restricted sets
// (sets.txt rows). The hardcoded row 29 check is removed completely; only the
// rows in the config are restricted, and only drops from the listed monsters
// can roll them. Defaults: monsters 391 and 735 (the vanilla pair), no
// restricted rows.
//
// Built for D2R 3.3 on D2RLoader. Every byte below was read out of the live
// D2RLoader.exe image and disassembled before being relied on.
//
// ---------------------------------------------------------------------------
// The vanilla rule, part 1: the drop gets tagged (0x444680)
// ---------------------------------------------------------------------------
//   0x444680 is the TC item-spawn helper. Its only caller is the TC core
//   0x4404F0, once per item the treasure class produces. It fills the drop
//   request on its stack (request = rsp+60h, request+0C0h = [rbp+20h]) and
//   hands it to the item creator 0x43CD50. rdi is the dropping unit and ebx
//   is 1 from 4446E2 on; nothing between there and 444852 writes ebx.
//
//     44480E  E8 BD 71 F0 FF          call 34B9D0              ; unit type
//     444813  3B C3                   cmp  eax, ebx            ; 1 = monster
//     444815  75 38                   jne  44484F              ; not a monster: no tag
//     444817  41 B8 55 0F 00 00       mov  r8d, 0F55h
//     44481D  48 8D 15 9C 54 8D 01    lea  rdx, <source file name>
//     444824  48 8B CF                mov  rcx, rdi
//     444827  E8 34 50 F0 FF          call 349860              ; eax = unit+4, the monstats id
//     44482C  3D 87 01 00 00          cmp  eax, 187h           ; 391 hellbovine
//     444831  74 1F                   je   444852              ; tag
//     444833  41 B8 55 0F 00 00       mov  r8d, 0F55h          ; the same lookup again
//     444839  48 8D 15 80 54 8D 01    lea  rdx, <source file name>
//     444840  48 8B CF                mov  rcx, rdi
//     444843  E8 18 50 F0 FF          call 349860
//     444848  3D DF 02 00 00          cmp  eax, 2DFh           ; 735 cowking
//     44484D  74 03                   je   444852              ; tag
//     44484F  8B 5D 20                mov  ebx, [rbp+20h]      ; no tag: the request flags (zeroed)
//     444852  0B 9D B8 00 00 00       or   ebx, [rbp+0B8h]     ; tag: ebx is 1. Plus the TC's own flags
//     444858  48 8D 54 24 60          lea  rdx, [rsp+60h]      ; the request
//     44485D  45 33 C0                xor  r8d, r8d
//     444860  89 5D 20                mov  [rbp+20h], ebx      ; request+0C0h, bit 0 = the tag
//     444863  48 8B CE                mov  rcx, rsi            ; game
//     444866  E8 E5 84 FF FF          call 43CD50              ; create the item
//
//   0x349860 only returns unit+4 (or -1 with an assert for a null unit).
//
// ---------------------------------------------------------------------------
// The vanilla rule, part 2: the set item picker (0x58C2F0)
// ---------------------------------------------------------------------------
//   Reached from the set-quality branch of 0x442D60 through 0x58ACD0 for every
//   item whose format is not 0. r14 is the drop request (mov r14, r8 at
//   58C321). rbx walks the setitems.txt records at record+22h, 1CCh apart
//   (add rbx, 22h at 58C3B4; add rbx, 1CCh at 58C4AA). record+30h is the
//   `set` column: the setitems loader 0x31C2D0 stores it at offset 48 after
//   resolving the set's name against the sets.txt list, so it holds the
//   sets.txt row number. record+2 holds the set item's own `index` name.
//   By 58C411 the row has already passed the code, version, flag, ladder and
//   level tests.
//
//     58C411  66 83 7B 0E 1D          cmp  word [rbx+0Eh], 1Dh   ; set row 29?
//     58C416  75 0A                   jne  58C422                ; other sets: carry on
//     58C418  41 F6 86 C0 00 00 00 01 test byte [r14+0C0h], 1    ; the tag
//     58C420  74 7C                   je   58C49E                ; untagged: skip the row
//     58C422  41 8B 46 50             mov  eax, [r14+50h]        ; forced row, then pooling
//     ...
//     58C49E  0F B7 74 24 30          movzx esi, word [rsp+30h]  ; skip: next row
//
//   The test runs before the forced-row check at 58C422, so a treasure class
//   that names a set item directly does not get around it either.
//
// ---------------------------------------------------------------------------
// The hooks
// ---------------------------------------------------------------------------
//   Tag site, 35 bytes at 44482C (both class-id compares): E9 rel32 into the
//   tag stub, the rest NOPed. eax already holds the monstats id from the
//   lookup at 444827. The stub tags (exit 444852, ebx still 1) when the id is
//   0..32767 and its bit is set in the monster bitmap, otherwise it leaves
//   through 44484F.
//
//   Picker site, 17 bytes at 58C411 (the row 29 test): E9 rel32 into the
//   picker stub, the rest NOPed. When the record's set row has its bit set in
//   the set bitmap, the stub requires the tag and leaves through 58C49E
//   without it. Every other row leaves through 58C422.
//
//   Why the windows are safe:
//     - both start and end on instruction boundaries and nothing branches
//       into them except onto their first byte (the jne at 444815 targets
//       44484F, the end of the tag window);
//     - the stubs write only rax, rcx, rdx and r8, and those are dead on
//       every exit: 444852 and 44484F reload rdx, r8 and rcx before the
//       3-argument call to 43CD50; 58C422 reloads eax, then writes ecx, r8
//       and rdx before reading them on every path; 58C49E reloads esi and
//       eax and the loop writes rcx, rdx and r8 before its calls;
//     - the flags are dead on every exit, and the stubs never use the stack;
//     - rbx, rsi, rdi, rbp, r9 to r15 and rsp are never touched.
//
//   The stubs were assembled with keystone and decoded back with capstone.
//   Both were emulated with unicorn against the original bytes mapped at
//   their real addresses: 1,507 monstats ids (0..1499, the 32767/32768
//   boundary, 65535, 65536, 7FFFFFFFh and -1) for the tag stub and all 65,536
//   set rows with four tag-byte values for the picker stub, under several
//   lists. The vanilla paths and the default monster list match the original
//   exactly, a [29] row list matches the original picker exactly, the
//   preserved registers come out unchanged and the counters are exact.

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
#include <string>
#include <string_view>

namespace CelestialRayOne::CowSetDrops {
namespace {

// ---------------------------------------------------------------------------
// Native anchors
// ---------------------------------------------------------------------------

constexpr std::uint64_t TagFunctionRva    = 0x444680;   // TC item-spawn helper
constexpr std::uint64_t TagContextRva     = 0x444802;
constexpr std::uint64_t TagSiteRva        = 0x44482C;   // cmp eax, 187h ... je 444852
constexpr std::uint64_t TagNoTagRva       = 0x44484F;   // mov ebx, [rbp+20h]
constexpr std::uint64_t TagTagRva         = 0x444852;   // or ebx, [rbp+0B8h]
constexpr std::uint64_t PickerFunctionRva = 0x58C2F0;   // set item picker
constexpr std::uint64_t PickerSiteRva     = 0x58C411;   // cmp word [rbx+0Eh], 1Dh ... je 58C49E
constexpr std::uint64_t PickerEligibleRva = 0x58C422;   // mov eax, [r14+50h]
constexpr std::uint64_t PickerSkipRva     = 0x58C49E;   // movzx esi, word [rsp+30h]

constexpr std::uint32_t TagSiteSize    = 35;
constexpr std::uint32_t PickerSiteSize = 17;
constexpr std::uint32_t JumpSize       = 5;

// 444680: mov r11, rsp / push rbp, rbx, rsi, rdi, r14 / lea rbp, [rsp-60h] /
// sub rsp, 160h / cookie / mov r14d, r8d / mov rdi, rdx / mov rsi, rcx / ... /
// mov ebx, 1 at 4446E2.
constexpr std::uint8_t TagEntryWitness[]{
    0x4C, 0x8B, 0xDC, 0x55, 0x53, 0x56, 0x57, 0x41, 0x56, 0x48, 0x8D, 0x6C,
    0x24, 0xA0, 0x48, 0x81, 0xEC, 0x60, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x05,
    0x2C, 0x6C, 0x58, 0x02, 0x48, 0x33, 0xC4, 0x48, 0x89, 0x45, 0x30, 0x44,
    0x89, 0x4C, 0x24, 0x50, 0x45, 0x8B, 0xF0, 0x48, 0x8B, 0xFA, 0x48, 0x8B,
    0xF1, 0x45, 0x85, 0xC0, 0x0F, 0x88, 0xB3, 0x01, 0x00, 0x00, 0x4D, 0x89,
    0x63, 0xD0, 0x48, 0x8D, 0x54, 0x24, 0x48, 0x4D, 0x89, 0x6B, 0xC8, 0x48,
    0x8B, 0xCF, 0x4D, 0x89, 0x7B, 0xC0, 0xE8, 0x6D, 0x59, 0xF0, 0xFF, 0x48,
    0x8B, 0xCF, 0xE8, 0x65, 0x6D, 0xF0, 0xFF, 0x83, 0xBE, 0x24, 0x01, 0x00,
    0x00, 0x00, 0xBB, 0x01, 0x00, 0x00, 0x00, 0x44, 0x8B, 0x7C, 0x24, 0x48,
    0x41, 0xBD, 0x03, 0x00, 0x00, 0x00, 0x44, 0x8B, 0x64, 0x24, 0x4C, 0x44,
};

// 444802..44486B: the unit type test, both class-id compares (the tag site at
// +2Ah), both exits, the flag store and the call to the item creator 43CD50.
constexpr std::uint8_t TagContextWitness[]{
    0x48, 0x8B, 0xCF, 0x8B, 0x85, 0xB0, 0x00, 0x00, 0x00, 0x89, 0x45, 0xB0,
    0xE8, 0xBD, 0x71, 0xF0, 0xFF, 0x3B, 0xC3, 0x75, 0x38, 0x41, 0xB8, 0x55,
    0x0F, 0x00, 0x00, 0x48, 0x8D, 0x15, 0x9C, 0x54, 0x8D, 0x01, 0x48, 0x8B,
    0xCF, 0xE8, 0x34, 0x50, 0xF0, 0xFF, 0x3D, 0x87, 0x01, 0x00, 0x00, 0x74,
    0x1F, 0x41, 0xB8, 0x55, 0x0F, 0x00, 0x00, 0x48, 0x8D, 0x15, 0x80, 0x54,
    0x8D, 0x01, 0x48, 0x8B, 0xCF, 0xE8, 0x18, 0x50, 0xF0, 0xFF, 0x3D, 0xDF,
    0x02, 0x00, 0x00, 0x74, 0x03, 0x8B, 0x5D, 0x20, 0x0B, 0x9D, 0xB8, 0x00,
    0x00, 0x00, 0x48, 0x8D, 0x54, 0x24, 0x60, 0x45, 0x33, 0xC0, 0x89, 0x5D,
    0x20, 0x48, 0x8B, 0xCE, 0xE8, 0xE5, 0x84, 0xFF, 0xFF,
};

// 58C2F0..58C4BF: the picker's entry (mov r14, r8 at 58C321; add rbx, 22h at
// 58C3B4) and its whole row loop, with the picker site at +121h.
constexpr std::uint8_t PickerWitness[]{
    0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
    0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0x60, 0x08, 0x00, 0x00, 0x48,
    0x8B, 0x05, 0xBA, 0xEF, 0x43, 0x02, 0x48, 0x33, 0xC4, 0x48, 0x89, 0x84,
    0x24, 0x50, 0x08, 0x00, 0x00, 0x48, 0x8B, 0xFA, 0x48, 0x89, 0x54, 0x24,
    0x48, 0x4D, 0x8B, 0xF0, 0x48, 0x8D, 0x15, 0xD5, 0x2A, 0x7B, 0x01, 0x4C,
    0x8B, 0xE9, 0x33, 0xC0, 0x48, 0x8B, 0xCF, 0x89, 0x44, 0x24, 0x3C, 0x41,
    0xB8, 0xE3, 0x04, 0x00, 0x00, 0x33, 0xED, 0xE8, 0x1C, 0xD5, 0xDB, 0xFF,
    0x41, 0x0F, 0xB6, 0x8D, 0x06, 0x01, 0x00, 0x00, 0x8B, 0xD0, 0xE8, 0xBD,
    0x7D, 0xD8, 0xFF, 0x48, 0x8B, 0xD8, 0x48, 0x85, 0xC0, 0x0F, 0x84, 0xF8,
    0x01, 0x00, 0x00, 0x48, 0x8B, 0xCF, 0xE8, 0x69, 0x6D, 0xDE, 0xFF, 0x0F,
    0xB7, 0xF0, 0x66, 0x89, 0x44, 0x24, 0x30, 0x8B, 0x83, 0x80, 0x00, 0x00,
    0x00, 0x48, 0x8B, 0xCF, 0x89, 0x44, 0x24, 0x34, 0xE8, 0x6F, 0x09, 0xDE,
    0xFF, 0x41, 0x0F, 0xB6, 0x8D, 0x06, 0x01, 0x00, 0x00, 0x89, 0x44, 0x24,
    0x40, 0xE8, 0xFE, 0x46, 0xD7, 0xFF, 0x33, 0xFF, 0x4C, 0x8B, 0xE0, 0x48,
    0x8B, 0x98, 0xA8, 0x13, 0x00, 0x00, 0x48, 0x39, 0xB8, 0xB0, 0x13, 0x00,
    0x00, 0x0F, 0x86, 0xAC, 0x01, 0x00, 0x00, 0x8B, 0x44, 0x24, 0x34, 0x4C,
    0x8D, 0x7C, 0x24, 0x54, 0x48, 0x83, 0xC3, 0x22, 0x0F, 0x1F, 0x84, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x3B, 0x43, 0x06, 0x0F, 0x85, 0xDE, 0x00, 0x00,
    0x00, 0x66, 0x83, 0x3B, 0x64, 0x72, 0x0A, 0x66, 0x83, 0xFE, 0x64, 0x0F,
    0x82, 0xCE, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x4B, 0x0A, 0xE8, 0x5E, 0xE4,
    0xDD, 0xFF, 0x84, 0xC0, 0x0F, 0x84, 0xB4, 0x00, 0x00, 0x00, 0x44, 0x0F,
    0xB7, 0x43, 0x6A, 0x49, 0x8B, 0xCD, 0x0F, 0xB7, 0x53, 0x68, 0xE8, 0xE5,
    0x7B, 0xE7, 0xFF, 0x84, 0xC0, 0x0F, 0x84, 0x9B, 0x00, 0x00, 0x00, 0x0F,
    0xBF, 0x43, 0x12, 0x3B, 0x44, 0x24, 0x40, 0x0F, 0x8F, 0x8D, 0x00, 0x00,
    0x00, 0x66, 0x83, 0x7B, 0x0E, 0x1D, 0x75, 0x0A, 0x41, 0xF6, 0x86, 0xC0,
    0x00, 0x00, 0x00, 0x01, 0x74, 0x7C, 0x41, 0x8B, 0x46, 0x50, 0x85, 0xC0,
    0x74, 0x0C, 0x8D, 0x48, 0xFF, 0x48, 0x3B, 0xCF, 0x0F, 0x84, 0xA9, 0x00,
    0x00, 0x00, 0x0F, 0xB6, 0x43, 0x0A, 0x84, 0x05, 0x90, 0xD2, 0x80, 0x01,
    0x74, 0x5C, 0x44, 0x8B, 0x43, 0x6E, 0x41, 0x83, 0xF8, 0xFF, 0x74, 0x14,
    0x49, 0x8B, 0x16, 0x41, 0x0F, 0xB6, 0x8D, 0x06, 0x01, 0x00, 0x00, 0xE8,
    0x24, 0x8F, 0xE2, 0xFF, 0x85, 0xC0, 0x74, 0x3E, 0x48, 0x63, 0xC7, 0x48,
    0x3B, 0xC7, 0x75, 0x04, 0x85, 0xFF, 0x79, 0x14, 0x48, 0x8D, 0x4C, 0x24,
    0x38, 0xC6, 0x44, 0x24, 0x38, 0x00, 0xE8, 0xE5, 0xA4, 0xAF, 0xFF, 0x84,
    0xC0, 0x74, 0x01, 0xCC, 0x8B, 0x43, 0x16, 0xB9, 0x01, 0x00, 0x00, 0x00,
    0x85, 0xC0, 0x41, 0x89, 0x7F, 0xFC, 0x0F, 0x44, 0xC1, 0xFF, 0x44, 0x24,
    0x3C, 0x03, 0xE8, 0x41, 0x89, 0x07, 0x49, 0x83, 0xC7, 0x08, 0x0F, 0xB7,
    0x74, 0x24, 0x30, 0x8B, 0x44, 0x24, 0x34, 0x48, 0xFF, 0xC7, 0x48, 0x81,
    0xC3, 0xCC, 0x01, 0x00, 0x00, 0x49, 0x3B, 0xBC, 0x24, 0xB0, 0x13, 0x00,
    0x00, 0x0F, 0x82, 0x01, 0xFF, 0xFF, 0xFF,
};

constexpr std::size_t TagSiteOffset    = TagSiteRva - TagContextRva;
constexpr std::size_t PickerSiteOffset = PickerSiteRva - PickerFunctionRva;
constexpr const std::uint8_t* TagSiteOriginal    = TagContextWitness + TagSiteOffset;
constexpr const std::uint8_t* PickerSiteOriginal = PickerWitness + PickerSiteOffset;

static_assert(sizeof(TagEntryWitness) == 0x78);
static_assert(sizeof(TagContextWitness) == 0x69);
static_assert(sizeof(PickerWitness) == 0x1CF);
static_assert(TagSiteOffset == 0x2A && PickerSiteOffset == 0x121);
static_assert(TagNoTagRva == TagSiteRva + TagSiteSize);
static_assert(PickerEligibleRva == PickerSiteRva + PickerSiteSize);
// cmp eax, 187h / je +1Fh lands on 444852; cmp eax, 2DFh / je +3 lands on 444852.
static_assert(TagSiteOriginal[0] == 0x3D && TagSiteOriginal[1] == 0x87 && TagSiteOriginal[2] == 0x01
    && TagSiteOriginal[5] == 0x74 && TagSiteRva + 7 + TagSiteOriginal[6] == TagTagRva);
static_assert(TagSiteOriginal[28] == 0x3D && TagSiteOriginal[29] == 0xDF && TagSiteOriginal[30] == 0x02
    && TagSiteOriginal[33] == 0x74 && TagSiteRva + 35 + TagSiteOriginal[34] == TagTagRva);
static_assert(TagContextWitness[TagNoTagRva - TagContextRva] == 0x8B
    && TagContextWitness[TagTagRva - TagContextRva] == 0x0B);
// cmp word [rbx+0Eh], 1Dh / jne +0Ah lands on 58C422; je +7Ch lands on 58C49E.
static_assert(PickerSiteOriginal[0] == 0x66 && PickerSiteOriginal[3] == 0x0E && PickerSiteOriginal[4] == 0x1D
    && PickerSiteOriginal[5] == 0x75 && PickerSiteRva + 7 + PickerSiteOriginal[6] == PickerEligibleRva);
static_assert(PickerSiteOriginal[7] == 0x41 && PickerSiteOriginal[8] == 0xF6 && PickerSiteOriginal[10] == 0xC0
    && PickerSiteOriginal[15] == 0x74 && PickerSiteRva + 17 + PickerSiteOriginal[16] == PickerSkipRva);
static_assert(PickerWitness[PickerSkipRva - PickerFunctionRva] == 0x0F
    && PickerWitness[PickerSkipRva - PickerFunctionRva + 1] == 0xB7);

// ---------------------------------------------------------------------------
// Hook region
// ---------------------------------------------------------------------------
//   One allocation within rel32 reach of the image, never freed:
//     +0000  code page        PAGE_EXECUTE_READ  tag stub at +000, picker stub at +100
//     +1000  counters page    PAGE_READWRITE     written by the stubs
//     +2000  monster bitmap   PAGE_READONLY      4 KB, bit n = monstats id n may tag
//     +3000  set row bitmap   PAGE_READONLY      8 KB, bit n = sets.txt row n restricted
//   The set bitmap covers every 16-bit value, so the picker stub needs no
//   bounds check; rows above 32767 are never set. The stubs read and write
//   nothing but the game and this region, so they never depend on this DLL.

constexpr std::size_t PageBytes           = 0x1000;
constexpr std::size_t CodeOffset          = 0x0000;
constexpr std::size_t TagStubOffset       = CodeOffset + 0x000;
constexpr std::size_t PickerStubOffset    = CodeOffset + 0x100;
constexpr std::size_t CountersOffset      = 0x1000;
constexpr std::size_t MonsterBitmapOffset = 0x2000;
constexpr std::size_t MonsterBitmapBytes  = 0x1000;
constexpr std::size_t SetBitmapOffset     = 0x3000;
constexpr std::size_t SetBitmapBytes      = 0x2000;
constexpr std::size_t HookRegionBytes     = SetBitmapOffset + SetBitmapBytes;

struct DropCounters {
    std::uint64_t tagged;         // +00  drops tagged by monster_ids
    std::uint64_t allowed;        // +08  restricted rows kept, the drop was tagged
    std::uint64_t denied;         // +10  restricted rows skipped, the drop was not tagged
    std::uint32_t lastTaggedId;   // +18  monstats id of the last tagged drop
    std::uint32_t lastRow;        // +1C  set row of the last restricted row checked
    char          lastItem[32];   // +20  its setitems.txt index (record+2)
    std::uint8_t  lastAllowed;    // +40  1 kept, 0 skipped
};
static_assert(offsetof(DropCounters, tagged) == 0x00);
static_assert(offsetof(DropCounters, allowed) == 0x08);
static_assert(offsetof(DropCounters, denied) == 0x10);
static_assert(offsetof(DropCounters, lastTaggedId) == 0x18);
static_assert(offsetof(DropCounters, lastRow) == 0x1C);
static_assert(offsetof(DropCounters, lastItem) == 0x20);
static_assert(offsetof(DropCounters, lastAllowed) == 0x40);

// Tag stub. Entered from 44482C with eax = monstats id, ebx = 1.
// +00  FF 25 02 00 00 00     jmp   qword ptr [rip+2]          ; dispatch
// +06  CC CC
// +08  dq                    +10h armed, +63h vanilla
// +10  41 89 C0              mov   r8d, eax                   ; armed
// +13  3D FF 7F 00 00        cmp   eax, 7FFFh
// +18  77 3B                 ja    +55h                       ; out of range: no tag
// +1A  48 BA dq              mov   rdx, monster bitmap
// +24  89 C1                 mov   ecx, eax
// +26  C1 E9 03              shr   ecx, 3
// +29  0F B6 0C 0A           movzx ecx, byte ptr [rdx+rcx]
// +2D  83 E0 07              and   eax, 7
// +30  0F A3 C1              bt    ecx, eax
// +33  73 20                 jnc   +55h                       ; not listed: no tag
// +35  48 BA dq              mov   rdx, counters
// +3F  F0 48 FF 02           lock inc qword ptr [rdx]         ; tagged
// +43  44 89 42 18           mov   dword ptr [rdx+18h], r8d   ; last tagged id
// +47  FF 25 00 00 00 00     jmp   qword ptr [rip+0]          ; tag
// +4D  dq                    444852
// +55  FF 25 00 00 00 00     jmp   qword ptr [rip+0]          ; no tag
// +5B  dq                    44484F
// +63  3D 87 01 00 00        cmp   eax, 187h                  ; vanilla: the original test
// +68  74 DD                 je    +47h
// +6A  3D DF 02 00 00        cmp   eax, 2DFh
// +6F  74 D6                 je    +47h
// +71  EB E2                 jmp   +55h
constexpr std::uint8_t TagStub[]{
    0xFF, 0x25, 0x02, 0x00, 0x00, 0x00, 0xCC, 0xCC, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x41, 0x89, 0xC0, 0x3D, 0xFF, 0x7F, 0x00, 0x00,
    0x77, 0x3B, 0x48, 0xBA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x89, 0xC1, 0xC1, 0xE9, 0x03, 0x0F, 0xB6, 0x0C, 0x0A, 0x83, 0xE0, 0x07,
    0x0F, 0xA3, 0xC1, 0x73, 0x20, 0x48, 0xBA, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0xF0, 0x48, 0xFF, 0x02, 0x44, 0x89, 0x42, 0x18, 0xFF,
    0x25, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x3D, 0x87, 0x01, 0x00, 0x00, 0x74, 0xDD, 0x3D, 0xDF,
    0x02, 0x00, 0x00, 0x74, 0xD6, 0xEB, 0xE2,
};
constexpr std::size_t TagDispatchSlot  = 0x08;
constexpr std::size_t TagArmedOffset   = 0x10;
constexpr std::size_t TagBitmapSlot    = 0x1C;
constexpr std::size_t TagCountersSlot  = 0x37;
constexpr std::size_t TagTagSlot       = 0x4D;
constexpr std::size_t TagNoTagSlot     = 0x5B;
constexpr std::size_t TagVanillaOffset = 0x63;

// Picker stub. Entered from 58C411 with rbx = setitems record+22h, r14 = request.
// +00  FF 25 02 00 00 00     jmp   qword ptr [rip+2]          ; dispatch
// +06  CC CC
// +08  dq                    +10h armed, +98h vanilla
// +10  0F B7 43 0E           movzx eax, word ptr [rbx+0Eh]    ; armed: the set row
// +14  41 89 C0              mov   r8d, eax
// +17  48 BA dq              mov   rdx, set bitmap
// +21  89 C1                 mov   ecx, eax
// +23  C1 E9 03              shr   ecx, 3
// +26  0F B6 0C 0A           movzx ecx, byte ptr [rdx+rcx]
// +2A  83 E0 07              and   eax, 7
// +2D  0F A3 C1              bt    ecx, eax
// +30  73 41                 jnc   +73h                       ; not restricted: eligible
// +32  48 BA dq              mov   rdx, counters
// +3C  44 89 42 1C           mov   dword ptr [rdx+1Ch], r8d   ; last row
// +40  48 8B 4B E0           mov   rcx, [rbx-20h]             ; record+2, the item's
// +44  48 89 4A 20           mov   [rdx+20h], rcx             ; index name, 32 bytes
// +48  48 8B 4B E8           mov   rcx, [rbx-18h]
// +4C  48 89 4A 28           mov   [rdx+28h], rcx
// +50  48 8B 4B F0           mov   rcx, [rbx-10h]
// +54  48 89 4A 30           mov   [rdx+30h], rcx
// +58  48 8B 4B F8           mov   rcx, [rbx-8]
// +5C  48 89 4A 38           mov   [rdx+38h], rcx
// +60  41 F6 86 C0 00 00 00 01  test byte ptr [r14+0C0h], 1   ; the tag
// +68  74 17                 jz    +81h
// +6A  F0 48 FF 42 08        lock inc qword ptr [rdx+8]       ; allowed
// +6F  C6 42 40 01           mov   byte ptr [rdx+40h], 1
// +73  FF 25 00 00 00 00     jmp   qword ptr [rip+0]          ; eligible
// +79  dq                    58C422
// +81  F0 48 FF 42 10        lock inc qword ptr [rdx+10h]     ; denied
// +86  C6 42 40 00           mov   byte ptr [rdx+40h], 0
// +8A  FF 25 00 00 00 00     jmp   qword ptr [rip+0]          ; skip
// +90  dq                    58C49E
// +98  66 83 7B 0E 1D        cmp   word ptr [rbx+0Eh], 1Dh    ; vanilla: the original test
// +9D  75 D4                 jne   +73h
// +9F  41 F6 86 C0 00 00 00 01  test byte ptr [r14+0C0h], 1
// +A7  74 E1                 je    +8Ah
// +A9  EB C8                 jmp   +73h
constexpr std::uint8_t PickerStub[]{
    0xFF, 0x25, 0x02, 0x00, 0x00, 0x00, 0xCC, 0xCC, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x0F, 0xB7, 0x43, 0x0E, 0x41, 0x89, 0xC0, 0x48,
    0xBA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x89, 0xC1, 0xC1,
    0xE9, 0x03, 0x0F, 0xB6, 0x0C, 0x0A, 0x83, 0xE0, 0x07, 0x0F, 0xA3, 0xC1,
    0x73, 0x41, 0x48, 0xBA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x44, 0x89, 0x42, 0x1C, 0x48, 0x8B, 0x4B, 0xE0, 0x48, 0x89, 0x4A, 0x20,
    0x48, 0x8B, 0x4B, 0xE8, 0x48, 0x89, 0x4A, 0x28, 0x48, 0x8B, 0x4B, 0xF0,
    0x48, 0x89, 0x4A, 0x30, 0x48, 0x8B, 0x4B, 0xF8, 0x48, 0x89, 0x4A, 0x38,
    0x41, 0xF6, 0x86, 0xC0, 0x00, 0x00, 0x00, 0x01, 0x74, 0x17, 0xF0, 0x48,
    0xFF, 0x42, 0x08, 0xC6, 0x42, 0x40, 0x01, 0xFF, 0x25, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x48, 0xFF,
    0x42, 0x10, 0xC6, 0x42, 0x40, 0x00, 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x66, 0x83, 0x7B, 0x0E,
    0x1D, 0x75, 0xD4, 0x41, 0xF6, 0x86, 0xC0, 0x00, 0x00, 0x00, 0x01, 0x74,
    0xE1, 0xEB, 0xC8,
};
constexpr std::size_t PickerDispatchSlot  = 0x08;
constexpr std::size_t PickerArmedOffset   = 0x10;
constexpr std::size_t PickerBitmapSlot    = 0x19;
constexpr std::size_t PickerCountersSlot  = 0x34;
constexpr std::size_t PickerEligibleSlot  = 0x79;
constexpr std::size_t PickerSkipSlot      = 0x90;
constexpr std::size_t PickerVanillaOffset = 0x98;

constexpr auto JumpBefore(const std::uint8_t* stub, std::size_t slot) noexcept -> bool {
    return slot >= 6 && stub[slot - 6] == 0xFF && stub[slot - 5] == 0x25 && stub[slot - 4] == 0x00
        && stub[slot - 3] == 0x00 && stub[slot - 2] == 0x00 && stub[slot - 1] == 0x00;
}

constexpr auto DispatchJump(const std::uint8_t* stub) noexcept -> bool {
    return stub[0] == 0xFF && stub[1] == 0x25 && stub[2] == 0x02 && stub[3] == 0x00
        && stub[4] == 0x00 && stub[5] == 0x00;
}

constexpr auto MovRdxImm64Before(const std::uint8_t* stub, std::size_t slot) noexcept -> bool {
    return slot >= 2 && stub[slot - 2] == 0x48 && stub[slot - 1] == 0xBA;
}

static_assert(sizeof(TagStub) == 0x73 && sizeof(PickerStub) == 0xAB);
static_assert(PickerStubOffset - TagStubOffset >= sizeof(TagStub));
static_assert(PickerStubOffset + sizeof(PickerStub) <= PageBytes);
static_assert(DispatchJump(TagStub) && DispatchJump(PickerStub));
static_assert((TagStubOffset + TagDispatchSlot) % 8 == 0 && (PickerStubOffset + PickerDispatchSlot) % 8 == 0);
static_assert(TagStub[TagArmedOffset] == 0x41 && TagStub[TagVanillaOffset] == 0x3D);
static_assert(MovRdxImm64Before(TagStub, TagBitmapSlot) && MovRdxImm64Before(TagStub, TagCountersSlot));
static_assert(JumpBefore(TagStub, TagTagSlot) && JumpBefore(TagStub, TagNoTagSlot));
static_assert(PickerStub[PickerArmedOffset] == 0x0F && PickerStub[PickerVanillaOffset] == 0x66);
static_assert(MovRdxImm64Before(PickerStub, PickerBitmapSlot) && MovRdxImm64Before(PickerStub, PickerCountersSlot));
static_assert(JumpBefore(PickerStub, PickerEligibleSlot) && JumpBefore(PickerStub, PickerSkipSlot));

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

constexpr char         ConfigSection[]    = "cow_set_drops";
constexpr std::int32_t MinimumId          = 0;
constexpr std::int32_t MaximumId          = 0x7FFF;
constexpr std::size_t  IdBitmapBytes      = 0x1000;   // ids 0..32767
constexpr std::size_t  InitialConfigBytes = 16'384;
constexpr std::size_t  MaximumConfigBytes = 1'048'576;
constexpr std::size_t  RejectedSampleSize = 5;

constexpr std::array<std::int32_t, 2> DefaultMonsterIds{ 391, 735 };

static_assert(static_cast<std::size_t>(MaximumId) < IdBitmapBytes * 8);
static_assert(IdBitmapBytes <= MonsterBitmapBytes && IdBitmapBytes <= SetBitmapBytes);

constexpr char DefaultConfigToml[] =
    "# Cow Set Drops\n"
    "#\n"
    "# Replaces the game's hardcoded Cow King's Leathers rule with a configurable one.\n"
    "#\n"
    "# The vanilla rule: a set item whose set is sets.txt row 29 (Cow King's\n"
    "# Leathers) can only roll on an item dropped by a monster with monstats.txt\n"
    "# hcIdx 391 (hellbovine) or 735 (cowking). It has nothing to do with the cow\n"
    "# level itself: a hellbovine anywhere can drop the set, nothing else can.\n"
    "#\n"
    "# This plugin removes the row 29 check completely. Only the sets listed in\n"
    "# restricted_set_rows are restricted, and only drops from the monsters listed\n"
    "# in monster_ids can roll them. With the defaults below no set is restricted,\n"
    "# so monster_ids does nothing until a set is listed.\n"
    "#\n"
    "# When a restricted set item is ruled out, the game picks another set item of\n"
    "# the same base. When there is none, the item drops as a magic item with\n"
    "# doubled durability, the same fallback as any failed set roll in vanilla.\n"
    "#\n"
    "# Server side only. In single player the game hosts its own server, so it\n"
    "# applies there too.\n"
    "#\n"
    "# Console command: cowsets (status, both lists, counters and the last\n"
    "# restricted set item checked)\n"
    "\n"
    "[cow_set_drops]\n"
    "\n"
    "# Master switch. false installs nothing, which keeps the vanilla row 29 rule.\n"
    "enabled = true\n"
    "\n"
    "# Monsters whose drops may roll the restricted sets, as monstats.txt hcIdx\n"
    "# values, 0 to 32767. A list may span several lines and carry # comments.\n"
    "# The default is the vanilla pair. A missing key also means this default.\n"
    "monster_ids = [\n"
    "    391,   # hellbovine\n"
    "    735,   # cowking\n"
    "]\n"
    "\n"
    "# Sets that only those monsters can drop, by the row number the game gives the\n"
    "# set: count the sets in sets.txt from 0, starting with the first one under the\n"
    "# header. 0 to 32767. [] restricts nothing, and so does a missing key.\n"
    "# [29] brings back the vanilla rule for Cow King's Leathers.\n"
    "# cowsets shows the set item name of the last restricted row checked, which\n"
    "# confirms the number in game.\n"
    "restricted_set_rows = []\n";

struct Settings {
    bool enabled = true;
};

struct IdList {
    const char*                               key;
    std::array<std::uint8_t, IdBitmapBytes>   bits{};
    std::int32_t                              count = 0;
    bool                                      found = false;
    bool                                      malformed = false;
    bool                                      unterminated = false;
    std::int32_t                              rejected = 0;
    std::string                               rejectedSample;

    auto Has(std::int32_t id) const noexcept -> bool {
        if (id < MinimumId || id > MaximumId) return false;
        const auto index = static_cast<std::size_t>(id);
        return ((bits[index >> 3] >> (index & 7U)) & 1U) != 0;
    }

    void Mark(std::int32_t id) noexcept {
        if (Has(id)) return;
        const auto index = static_cast<std::size_t>(id);
        bits[index >> 3] = static_cast<std::uint8_t>(bits[index >> 3] | (1U << (index & 7U)));
        ++count;
    }

    void ClearIds() noexcept {
        bits.fill(0);
        count = 0;
    }

    // The key was found in the file: its value replaces the default.
    void Restart() noexcept {
        ClearIds();
        found = true;
        malformed = false;
        unterminated = false;
        rejected = 0;
        rejectedSample.clear();
    }
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

enum class HookState : std::uint8_t {
    NotAttempted,
    DisabledByConfig,
    ConfigError,
    UnsupportedBuild,
    InstallFailed,
    Installed,
};

const D2RL::PluginContext* Context{};
std::uintptr_t             Base{};
Settings                   Config{};
IdList                     MonsterIds{ "monster_ids" };
IdList                     RestrictedRows{ "restricted_set_rows" };
bool                       ConfigFileWasRead{};
std::atomic<HookState>     State{ HookState::NotAttempted };
void*                      HookRegion{};
bool                       TagSitePatched{};
bool                       PickerSitePatched{};

// ---------------------------------------------------------------------------
// Config parsing (small TOML subset, no external dependency)
// ---------------------------------------------------------------------------

auto Trim(std::string_view value) noexcept -> std::string_view {
    std::size_t first = 0;
    while (first < value.size()
            && (value[first] == ' ' || value[first] == '\t'
                || value[first] == '\r' || value[first] == '\n')) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first
            && (value[last - 1] == ' ' || value[last - 1] == '\t'
                || value[last - 1] == '\r' || value[last - 1] == '\n')) {
        --last;
    }
    return value.substr(first, last - first);
}

auto StripComment(std::string_view line) noexcept -> std::string_view {
    const std::size_t hash = line.find('#');
    return hash == std::string_view::npos ? line : line.substr(0, hash);
}

auto ParseBool(std::string_view value, bool& out) noexcept -> bool {
    if (value == "true") { out = true; return true; }
    if (value == "false") { out = false; return true; }
    return false;
}

// Decimal, optional leading '+', '_' separators allowed as in TOML.
auto ParseId(std::string_view text, std::int32_t& out) noexcept -> bool {
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
        if (value > MaximumId) return false;
    }
    if (!anyDigit) return false;
    out = value;
    return true;
}

// The text between the brackets, one or more lines already joined with
// commas. Empty elements (a trailing comma, a blank line) are skipped.
void ApplyIds(std::string_view text, IdList& list) noexcept {
    std::size_t cursor = 0;
    while (cursor <= text.size()) {
        const std::size_t comma = text.find(',', cursor);
        const std::size_t end = comma == std::string_view::npos ? text.size() : comma;
        const std::string_view element = Trim(text.substr(cursor, end - cursor));
        if (!element.empty()) {
            std::int32_t id = 0;
            if (ParseId(element, id)) {
                list.Mark(id);
            } else {
                ++list.rejected;
                if (list.rejected <= static_cast<std::int32_t>(RejectedSampleSize)) {
                    if (!list.rejectedSample.empty()) list.rejectedSample += ", ";
                    list.rejectedSample.append(element.substr(0, 24));
                }
            }
        }
        if (comma == std::string_view::npos) break;
        cursor = comma + 1;
    }
}

auto ListForKey(std::string_view key) noexcept -> IdList* {
    if (key == MonsterIds.key) return &MonsterIds;
    if (key == RestrictedRows.key) return &RestrictedRows;
    return nullptr;
}

void ParseConfig(std::string_view text) noexcept {
    std::string_view section;
    std::string      arrayText;
    IdList*          capturing = nullptr;

    std::size_t cursor = 0;
    while (cursor < text.size()) {
        std::size_t end = text.find('\n', cursor);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view line = Trim(StripComment(text.substr(cursor, end - cursor)));
        cursor = end + 1;

        if (capturing != nullptr) {
            if (!line.empty() && line.front() == '[') {
                // A table header before the closing bracket: the list was
                // never closed. Drop it and read the header normally.
                capturing->unterminated = true;
                capturing->ClearIds();
                capturing = nullptr;
            } else {
                const std::size_t close = line.find(']');
                arrayText.append(line.substr(0, close));
                arrayText.push_back(',');
                if (close != std::string_view::npos) {
                    ApplyIds(arrayText, *capturing);
                    capturing = nullptr;
                }
                continue;
            }
        }

        if (line.empty()) continue;

        if (line.front() == '[') {
            const std::size_t close = line.find(']');
            section = Trim(line.substr(1, close == std::string_view::npos ? close : close - 1));
            continue;
        }

        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos || section != ConfigSection) continue;
        const std::string_view key = Trim(line.substr(0, equals));
        const std::string_view value = Trim(line.substr(equals + 1));

        if (key == "enabled") {
            if (!ParseBool(value, Config.enabled)) {
                D2RL::LogWarnF(Context,
                    "CowSetDrops: enabled must be true or false; keeping %s.",
                    Config.enabled ? "true" : "false");
            }
            continue;
        }

        IdList* list = ListForKey(key);
        if (list == nullptr) continue;
        // The last assignment wins, as with every other key.
        list->Restart();
        if (value.empty() || value.front() != '[') {
            list->malformed = true;
            continue;
        }
        const std::size_t close = value.find(']');
        if (close == std::string_view::npos) {
            arrayText.assign(value.substr(1));
            arrayText.push_back(',');
            capturing = list;
        } else {
            arrayText.assign(value.substr(1, close - 1));
            ApplyIds(arrayText, *list);
        }
    }

    if (capturing != nullptr) {
        capturing->unterminated = true;
        capturing->ClearIds();
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

// Logs what is wrong with a list. false = unusable, nothing gets installed.
auto CheckList(const IdList& list, const char* missingMeaning) noexcept -> bool {
    if (!list.found) {
        D2RL::LogWarnF(Context, "CowSetDrops: %s is missing from [%s], so %s.",
            list.key, ConfigSection, missingMeaning);
        return true;
    }
    if (list.malformed) {
        D2RL::LogErrorF(Context,
            "CowSetDrops: %s must be a list in square brackets, for example [391, 735]. "
            "Nothing was installed.", list.key);
        return false;
    }
    if (list.unterminated) {
        D2RL::LogErrorF(Context,
            "CowSetDrops: %s is missing its closing ']'. Nothing was installed.", list.key);
        return false;
    }
    if (list.rejected > 0) {
        D2RL::LogWarnF(Context,
            "CowSetDrops: %d entr%s in %s ignored (%s%s). Every entry must be a whole number "
            "from %d to %d.",
            list.rejected, list.rejected == 1 ? "y was" : "ies were", list.key,
            list.rejectedSample.c_str(),
            list.rejected > static_cast<std::int32_t>(RejectedSampleSize) ? ", ..." : "",
            MinimumId, MaximumId);
    }
    return true;
}

auto ReadConfiguration() noexcept -> bool {
    // Defaults, replaced by the file's own lists when it has them.
    MonsterIds.ClearIds();
    for (const std::int32_t id : DefaultMonsterIds) MonsterIds.Mark(id);
    RestrictedRows.ClearIds();

    std::string text;
    if (!Context->EnsureConfig(DefaultConfigToml)) {
        Context->LogWarn("CowSetDrops: the config file could not be created; using the "
                         "embedded defaults.");
        text = DefaultConfigToml;
    } else if (!ReadConfigText(text)) {
        Context->LogWarn("CowSetDrops: the config file could not be read; using the "
                         "embedded defaults.");
        text = DefaultConfigToml;
    } else {
        ConfigFileWasRead = true;
    }

    ParseConfig(text);

    const bool monstersUsable = CheckList(MonsterIds, "the vanilla pair 391 and 735 is used");
    const bool rowsUsable = CheckList(RestrictedRows, "no set is restricted");
    return monstersUsable && rowsUsable;
}

// Appends " id id id" in ascending order, stopping before `limit` characters.
void AppendIds(const IdList& list, std::string& out, std::size_t limit) noexcept {
    char number[16]{};
    for (std::int32_t id = MinimumId; id <= MaximumId; ++id) {
        if (!list.Has(id)) continue;
        const int length = std::snprintf(number, sizeof(number), " %d", id);
        if (length <= 0) continue;
        if (out.size() + static_cast<std::size_t>(length) + 4 > limit) {
            out += " ...";
            return;
        }
        out.append(number, static_cast<std::size_t>(length));
    }
}

auto ListText(const IdList& list, const char* emptyText, std::size_t limit) noexcept -> std::string {
    std::string text = list.key;
    text += ':';
    if (list.count == 0) {
        text += ' ';
        text += emptyText;
        return text;
    }
    AppendIds(list, text, limit);
    return text;
}

// ---------------------------------------------------------------------------
// Hook region and sites
// ---------------------------------------------------------------------------

auto CanEncodeRel32(std::uintptr_t from, std::uintptr_t to) noexcept -> bool {
    const std::int64_t delta =
        static_cast<std::int64_t>(to) - static_cast<std::int64_t>(from) - JumpSize;
    return delta >= INT32_MIN && delta <= INT32_MAX;
}

auto AllocateNear(std::uintptr_t hint, std::size_t size) noexcept -> void* {
    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    const auto granularity = static_cast<std::uintptr_t>(systemInfo.dwAllocationGranularity);
    const auto aligned = hint & ~(granularity - 1U);
    for (std::uintptr_t delta = granularity; delta < 0x7000'0000ULL; delta += granularity) {
        const auto candidate = aligned + delta;
        if (!CanEncodeRel32(hint, candidate + size)) break;
        if (auto* memory = VirtualAlloc(reinterpret_cast<void*>(candidate), size,
                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) {
            return memory;
        }
    }
    return nullptr;
}

auto RegionAddress(std::size_t offset) noexcept -> std::uintptr_t {
    return reinterpret_cast<std::uintptr_t>(HookRegion) + offset;
}

void WriteQword(std::uint8_t* at, std::uint64_t value) noexcept {
    std::memcpy(at, &value, sizeof(value));
}

// E9 rel32 to the stub, the rest of the window filled with NOP.
void EncodeSiteJump(std::uint64_t siteRva, std::size_t stubOffset, std::uint32_t size,
        std::uint8_t* out) noexcept {
    const std::uintptr_t site = Base + siteRva;
    const std::uintptr_t target = RegionAddress(stubOffset);
    out[0] = 0xE9;
    const auto displacement = static_cast<std::int32_t>(
        static_cast<std::int64_t>(target) - static_cast<std::int64_t>(site + JumpSize));
    std::memcpy(out + 1, &displacement, sizeof(displacement));
    std::memset(out + JumpSize, 0x90, size - JumpSize);
}

auto ReadCounters() noexcept -> DropCounters {
    DropCounters copy{};
    if (HookRegion == nullptr) return copy;
    const auto* live = reinterpret_cast<const volatile DropCounters*>(RegionAddress(CountersOffset));
    copy.tagged = live->tagged;
    copy.allowed = live->allowed;
    copy.denied = live->denied;
    copy.lastTaggedId = live->lastTaggedId;
    copy.lastRow = live->lastRow;
    for (std::size_t index = 0; index < sizeof(copy.lastItem); ++index) {
        copy.lastItem[index] = live->lastItem[index];
    }
    copy.lastAllowed = live->lastAllowed;
    return copy;
}

auto BuildHookRegion() noexcept -> bool {
    HookRegion = AllocateNear(Base + TagSiteRva, HookRegionBytes);
    if (HookRegion == nullptr) {
        Context->LogError("CowSetDrops: no hook region was available within rel32 reach of "
                          "the drop code.");
        return false;
    }
    if (!CanEncodeRel32(Base + TagSiteRva, RegionAddress(TagStubOffset))
            || !CanEncodeRel32(Base + PickerSiteRva, RegionAddress(PickerStubOffset))) {
        Context->LogError("CowSetDrops: the hook region is out of rel32 reach of a hook site.");
        VirtualFree(HookRegion, 0, MEM_RELEASE);
        HookRegion = nullptr;
        return false;
    }

    auto* region = static_cast<std::uint8_t*>(HookRegion);
    std::memset(region + CodeOffset, 0xCC, PageBytes);
    std::memset(region + CountersOffset, 0, PageBytes);
    std::memset(region + MonsterBitmapOffset, 0, MonsterBitmapBytes);
    std::memset(region + SetBitmapOffset, 0, SetBitmapBytes);
    std::memcpy(region + MonsterBitmapOffset, MonsterIds.bits.data(), IdBitmapBytes);
    std::memcpy(region + SetBitmapOffset, RestrictedRows.bits.data(), IdBitmapBytes);

    std::uint8_t* tag = region + TagStubOffset;
    std::memcpy(tag, TagStub, sizeof(TagStub));
    WriteQword(tag + TagDispatchSlot, RegionAddress(TagStubOffset + TagArmedOffset));
    WriteQword(tag + TagBitmapSlot, RegionAddress(MonsterBitmapOffset));
    WriteQword(tag + TagCountersSlot, RegionAddress(CountersOffset));
    WriteQword(tag + TagTagSlot, Base + TagTagRva);
    WriteQword(tag + TagNoTagSlot, Base + TagNoTagRva);

    std::uint8_t* picker = region + PickerStubOffset;
    std::memcpy(picker, PickerStub, sizeof(PickerStub));
    WriteQword(picker + PickerDispatchSlot, RegionAddress(PickerStubOffset + PickerArmedOffset));
    WriteQword(picker + PickerBitmapSlot, RegionAddress(SetBitmapOffset));
    WriteQword(picker + PickerCountersSlot, RegionAddress(CountersOffset));
    WriteQword(picker + PickerEligibleSlot, Base + PickerEligibleRva);
    WriteQword(picker + PickerSkipSlot, Base + PickerSkipRva);

    DWORD previous = 0;
    if (!VirtualProtect(region + CodeOffset, PageBytes, PAGE_EXECUTE_READ, &previous)
            || !VirtualProtect(region + MonsterBitmapOffset, MonsterBitmapBytes + SetBitmapBytes,
                   PAGE_READONLY, &previous)) {
        Context->LogError("CowSetDrops: hook region protection could not be finalized.");
        VirtualFree(HookRegion, 0, MEM_RELEASE);
        HookRegion = nullptr;
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), region + CodeOffset, PageBytes);
    return true;
}

// Points both dispatches at the stubs' vanilla paths, so each stub is the
// original test again and never reads a bitmap. The qwords are 8-aligned, so
// each store is a single atomic write even while a game thread runs a stub.
auto RetargetStubsToVanilla() noexcept -> bool {
    if (HookRegion == nullptr) return true;
    auto* code = static_cast<std::uint8_t*>(HookRegion) + CodeOffset;
    DWORD previous = 0;
    if (!VirtualProtect(code, PageBytes, PAGE_EXECUTE_READWRITE, &previous)) return false;
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(code + TagStubOffset + TagDispatchSlot),
        static_cast<LONG64>(RegionAddress(TagStubOffset + TagVanillaOffset)));
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(code + PickerStubOffset + PickerDispatchSlot),
        static_cast<LONG64>(RegionAddress(PickerStubOffset + PickerVanillaOffset)));
    DWORD ignored = 0;
    VirtualProtect(code, PageBytes, previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), code, PageBytes);
    return true;
}

auto RestoreTagSite() noexcept -> bool {
    if (!TagSitePatched) return true;
    std::uint8_t current[TagSiteSize]{};
    EncodeSiteJump(TagSiteRva, TagStubOffset, TagSiteSize, current);
    if (!Context->PatchBytes(TagSiteRva, current, TagSiteSize, TagSiteOriginal, TagSiteSize)) {
        return false;
    }
    TagSitePatched = false;
    return true;
}

auto RestorePickerSite() noexcept -> bool {
    if (!PickerSitePatched) return true;
    std::uint8_t current[PickerSiteSize]{};
    EncodeSiteJump(PickerSiteRva, PickerStubOffset, PickerSiteSize, current);
    if (!Context->PatchBytes(PickerSiteRva, current, PickerSiteSize, PickerSiteOriginal, PickerSiteSize)) {
        return false;
    }
    PickerSitePatched = false;
    return true;
}

// ---------------------------------------------------------------------------
// Verification and installation
// ---------------------------------------------------------------------------

auto Verify(std::uint64_t rva, const std::uint8_t* expected, std::size_t size,
        const char* label) noexcept -> bool {
    if (Context->CheckExpectedBytes(rva, expected, static_cast<std::uint32_t>(size))) return true;
    D2RL::LogErrorF(Context,
        "CowSetDrops: NOT installed. %s at 0x%llX does not match the verified D2R image, or "
        "another plugin already owns it. Nothing was patched.",
        label, static_cast<unsigned long long>(rva));
    return false;
}

auto VerifyNativeContract() noexcept -> bool {
    return Verify(TagFunctionRva, TagEntryWitness, sizeof(TagEntryWitness),
               "The TC item-spawn helper entry")
        && Verify(TagContextRva, TagContextWitness, sizeof(TagContextWitness),
               "The hellbovine/cowking drop tag")
        && Verify(PickerFunctionRva, PickerWitness, sizeof(PickerWitness),
               "The set item picker");
}

void InstallHooks(bool configUsable) noexcept {
    if (!Config.enabled) {
        State.store(HookState::DisabledByConfig, std::memory_order_release);
        Context->LogInfo("CowSetDrops: not installed, turned off in the config file. The vanilla "
                         "row 29 rule stays.");
        return;
    }
    if (!configUsable) {
        State.store(HookState::ConfigError, std::memory_order_release);
        return;
    }
    if (!VerifyNativeContract()) {
        State.store(HookState::UnsupportedBuild, std::memory_order_release);
        return;
    }
    if (!BuildHookRegion()) {
        State.store(HookState::InstallFailed, std::memory_order_release);
        return;
    }

    std::uint8_t tagJump[TagSiteSize]{};
    EncodeSiteJump(TagSiteRva, TagStubOffset, TagSiteSize, tagJump);
    if (!Context->PatchBytes(TagSiteRva, TagSiteOriginal, TagSiteSize, tagJump, TagSiteSize)) {
        // Nothing points at the region, and nothing in it points at this DLL.
        State.store(HookState::InstallFailed, std::memory_order_release);
        Context->LogError("CowSetDrops: NOT installed. The drop tag at 0x44482C could not be "
                          "redirected.");
        return;
    }
    TagSitePatched = true;

    std::uint8_t pickerJump[PickerSiteSize]{};
    EncodeSiteJump(PickerSiteRva, PickerStubOffset, PickerSiteSize, pickerJump);
    if (!Context->PatchBytes(PickerSiteRva, PickerSiteOriginal, PickerSiteSize, pickerJump,
            PickerSiteSize)) {
        State.store(HookState::InstallFailed, std::memory_order_release);
        RetargetStubsToVanilla();
        if (RestoreTagSite()) {
            Context->LogError("CowSetDrops: NOT installed. The set picker at 0x58C411 could not "
                              "be redirected, so the drop tag was restored too.");
        } else {
            Context->LogError("CowSetDrops: NOT installed. The set picker at 0x58C411 could not "
                              "be redirected and the drop tag could not be restored; it runs the "
                              "vanilla test through the hook region.");
        }
        return;
    }
    PickerSitePatched = true;
    State.store(HookState::Installed, std::memory_order_release);

    const std::string monsters = ListText(MonsterIds, "none", 600);
    const std::string rows = ListText(RestrictedRows, "none, no set is restricted", 600);
    D2RL::LogInfoF(Context, "CowSetDrops: installed. The row 29 rule is gone. %s | %s",
        monsters.c_str(), rows.c_str());
}

// ---------------------------------------------------------------------------
// Console command
// ---------------------------------------------------------------------------

auto StateText(HookState state) noexcept -> const char* {
    switch (state) {
    case HookState::Installed:        return "active";
    case HookState::DisabledByConfig: return "off in the config file (vanilla row 29 rule)";
    case HookState::ConfigError:      return "NOT ACTIVE, a list could not be read, see the log";
    case HookState::UnsupportedBuild: return "NOT ACTIVE, unrecognised game build or the drop code is already hooked, see the log";
    case HookState::InstallFailed:    return "NOT ACTIVE, the hook failed, see the log";
    case HookState::NotAttempted:
    default:                          return "NOT ACTIVE, never attempted";
    }
}

auto __cdecl StatusCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command,
        void*) noexcept -> D2RL::ConsoleCommandResult {
    if (command == nullptr || command->plugin == nullptr) return D2RL::ConsoleCommandResult::Failed;
    const D2RL::PluginContext* plugin = command->plugin;

    const DropCounters counters = ReadCounters();
    char line[400]{};
    std::snprintf(line, sizeof(line),
        "Cow Set Drops: %s | config %s | tagged drops %llu (last monster id %u) | restricted "
        "rows kept %llu, skipped %llu",
        StateText(State.load(std::memory_order_acquire)),
        ConfigFileWasRead ? "loaded" : "NOT READ (embedded defaults)",
        static_cast<unsigned long long>(counters.tagged), counters.lastTaggedId,
        static_cast<unsigned long long>(counters.allowed),
        static_cast<unsigned long long>(counters.denied));
    plugin->WriteConsoleMessage(line);

    plugin->WriteConsoleMessage(ListText(MonsterIds, "none", 380).c_str());
    plugin->WriteConsoleMessage(ListText(RestrictedRows, "none, no set is restricted", 380).c_str());

    if (counters.allowed + counters.denied > 0) {
        char name[sizeof(counters.lastItem) + 1]{};
        for (std::size_t index = 0; index < sizeof(counters.lastItem); ++index) {
            const char character = counters.lastItem[index];
            if (character == '\0') break;
            name[index] = (character >= 0x20 && character < 0x7F) ? character : '?';
        }
        std::snprintf(line, sizeof(line), "last restricted row checked: %u, set item \"%s\", %s",
            counters.lastRow, name, counters.lastAllowed != 0 ? "kept" : "skipped");
        plugin->WriteConsoleMessage(line);
    }
    return D2RL::ConsoleCommandResult::Handled;
}

constexpr D2RL::PluginInfo Info{
    .infoSize    = D2RL::PluginInfoSize,
    .abiVersion  = D2RL_PLUGIN_ABI_VERSION,
    .id          = "celestialrayone.cow-set-drops",
    .name        = "Cow Set Drops",
    .version     = "1.0.0",
    .author      = "CelestialRayOne",
    .description = "Replaces the hardcoded Cow King's Leathers rule: a configurable list of "
                   "monsters that may drop a configurable list of restricted sets.",
    // Server: treasure class drops are only rolled on the server.
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
    // unload the DLL and take the command with it, leaving no in-game way to
    // see why nothing changed.
    if (!Context->RegisterConsoleCommand("cowsets", &StatusCommand,
            "Reports Cow Set Drops status, both lists and counters.")) {
        Context->LogWarn("CowSetDrops: the status console command was refused.");
    }

    const bool configUsable = ReadConfiguration();
    InstallHooks(configUsable);
    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    if (Context == nullptr || (!TagSitePatched && !PickerSitePatched)) return;
    const bool retargeted = RetargetStubsToVanilla();
    const bool tagRestored = RestoreTagSite();
    const bool pickerRestored = RestorePickerSite();
    if (!tagRestored || !pickerRestored) {
        Context->LogError(retargeted
            ? "CowSetDrops: a hook site could not be restored; it keeps running the vanilla "
              "test through the hook region."
            : "CowSetDrops: a hook site could not be restored and the stubs could not be "
              "retargeted; the configured lists stay in effect.");
    }
    // The region is deliberately kept: a thread may be inside a stub right
    // now, and a site that could not be restored still needs it.
}

}  // namespace CelestialRayOne::CowSetDrops
