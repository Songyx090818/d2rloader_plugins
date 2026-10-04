// ---------------------------------------------------------------------------
// Lightning Fury: bolt sync and caster return  -  D2RLoader plugin (D2R 3.3)
//
// Two parts, both for missiles that use the Lightning Fury hit functions
// (missiles.txt pSrvHitFunc 20 on the server, pCltHitFunc 25 on the client).
//
// 1) Bolt sync
//    Stock D2R picks the bolt targets twice. The server hit function picks
//    the units that take damage; the client hit function separately picks the
//    units it draws bolts to. Nothing passes the server's choice to the
//    client, so the drawn bolts and the damaging bolts disagree whenever more
//    units are in range than there are bolts.
//
//    The fix makes the server's bolts the only bolts, with no data edits:
//      * server: every bolt created by the Lightning Fury bolt callback is
//        remembered (unit pointer + GUID + game) and sent, through the game's
//        own create-missile sender, to every client that has the bolt's room
//        in sight (the room's client list, the same list the stock code uses
//        when a unit comes into sight). The thrower is one of those clients.
//        The stock code never sends a missile without ClientSend, and skips
//        the owner even with it, so it cannot be relied on here.
//      * the sender only accepts missiles whose missiles.txt row has
//        ClientSend or whose force-send byte (unit +0x1B4) is set. The plugin
//        sets that byte for the duration of its own send call and restores it
//        afterwards; inside the sender it is read only by that gate.
//      * the sender is hooked so a later stock send of an already sent bolt
//        is dropped, which keeps it to one copy per client.
//      * client: the client hit function spawns no bolts of its own for a
//        javelin row whose server function is 20 and that names a bolt in
//        HitSubMissile1, because the server copies are on their way.
//      * client: each server bolt arrives as a create-missile packet (0x73).
//        Stock D2R would queue it and mirror the server unit, but the
//        server's remove packet for a bolt that lives a few frames cancels
//        that queued create before it is built. So the plugin captures these
//        packets and creates each one as an ordinary client bolt, through the
//        same client create function and parameters the stock client bolt
//        callback uses, at the server's spawn point and flying to the
//        server's target. It then lives its own visual lifetime.
//    ClientSend in missiles.txt is not needed; rows that already have it work
//    the same way.
//
// 2) Caster return (port of the four 2.4 static patches)
//    When a hit fires exactly bolt_count bolts, the skill's skills.txt
//    aurafilter has the configured bit set, and the caster is within range of
//    the impact point, the single bolt is fired at the CASTER instead of at a
//    random enemy. The client mirror of this is only used for rows that are
//    not synced; synced rows receive the server's own caster bolt.
//
// Game functions involved (RVAs into the running main module, D2RLoader.exe,
// taken from the pe-sieve dump rebased to 0x140000000, build 3.3.93847):
//
//   0x45EC20  server Lightning Fury hit function (pSrvHitFunc 20)
//   0x4327D0  server "run <callback> for every valid target near (x,y)"
//             f(pGame, caster, x, y, range, aurafilter, callback, wrapper, 1,
//               file, line)
//   0x464FE0  server per-target bolt callback, f(iteratorCtx, target).
//             Referenced only by 0x45EC20.
//   0x465123  inside 0x464FE0: call 0x5371A0 (create missile) with
//             rcx = pGame, rdx = missile params. The result is discarded by
//             the game; bolt sync redirects this call to catch the new bolt.
//   0x5371A0  server create missile, f(pGame, params) -> missile unit
//   0x537BC0  server create-missile packet (0x73) sender,
//             f(client, pGame, missile, velocity). Sends only when the
//             missile's row has ClientSend or the missile's force-send byte
//             (unit +0x1B4) is set (gate at 0x537C97). Hooked by this plugin.
//   0x538690  stock "send unit to client": for a missile it calls 0x537BC0
//             only when the missile's owner is NOT that client's player.
//   0x34B440  room of a unit, f(unit) -> room
//   0x2EFB20  clients that have a room in sight: [room+0x68] array,
//             [room+0x70] count
//   0x166040  missiles.txt record, f(dataContext, id) -> record or null
//
//   0x1ADA50  client Lightning Fury hit function (pCltHitFunc 25)
//   0x2142D0  client counterpart of 0x4327D0,
//             f(caster, x, y, range, aurafilter, callback, wrapper, 1)
//   0x1BC840  client per-target bolt callback. Referenced only by 0x1ADA50.
//
//   0x12D290  client handler of packet 0x73: queues up to 5 missiles
//   0x131900  client "ProcessServerMissiles": builds the queued missiles once
//             per client update (through D2RLoader's own rebuild function)
//   0x09A5D0  client unit by (id, type)
//   0x1B7760  client create missile, f(params, 0)
//
// Layouts read here (each one confirmed against this build):
//
//   unit:            +0x00 unit type, +0x04 class id (0x349860),
//                    +0x08 GUID (0x34A330), +0x38 path (0x34AE80),
//                    +0x1B4 missile force-send byte (0x537CA3),
//                    +0x1BD data context (0x34A0E0)
//   path:            +0xA0 velocity (0x341D30)
//   missiles.txt:    +0x24 flags byte, ClientSend = 0x20 (loader entry built
//                    at 0x3C2D35, bit 5; the 0x73 sender tests it at 0x537C97)
//                    +0x2E pSrvHitFunc (read by the hit dispatcher at 0x463E1D)
//                    +0x46 HitSubMissile1, +0x4E CltHitSubMissile1 (loader)
//   packet 0x73:     +0x05 skill, +0x09 castId, +0x0F owner type, +0x10 owner
//                    id, +0x14 count; entry at +0x15: +0x0E x, +0x12 y,
//                    +0x16/+0x17 target dx/dy (signed bytes), +0x18 class
//                    (word), +0x1C skill level (built by 0x537BC0, read by
//                    0x12D290)
//   client params:   +0x00 flags (1 spawn at x/y, 0x20 fly to target),
//                    +0x08 owner, +0x20 missile, +0x24/+0x28 x/y,
//                    +0x2C/+0x30 target, +0x3C skill, +0x40 level,
//                    +0x68 castId (as filled by 0x1BC840, read by 0x1B7760)
//
//   iterator callback context, built by the two iterators on their own stack
//     server: +0x00 pGame  +0x08 caster  +0x10 counter(i32)  +0x18 wrapper
//     client: +0x00 caster +0x08 counter(i32)                +0x10 wrapper
//
//   Lightning Fury wrapper, built by the two hit functions
//     server: +0x00 caster +0x08 missile +0x10 skillId +0x14 skillLvl
//             +0x18 castId +0x1C boltCount +0x20 srvSubMissileId
//     client: +0x00 caster +0x08 missile +0x10 skillId +0x14 skillLvl
//             +0x18 boltCount +0x1C cltSubMissileId +0x20 castId
//
// Runtime settings live in d2rloader/config/celestialrayone.lf-caster-priority.toml.
// The file is created with defaults on first load.
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
// Build-specific constants: target search hooks (both parts)
// --------------------------------------------------------------------------

constexpr std::uint64_t kSrvTargetIteratorRva = 0x004327D0ULL;
constexpr std::uint64_t kSrvBoltCallbackRva   = 0x00464FE0ULL;
constexpr std::uint64_t kCltTargetIteratorRva = 0x002142D0ULL;
constexpr std::uint64_t kCltBoltCallbackRva   = 0x001BC840ULL;

// Prologue bytes, used as a build check. A mismatch means this plugin is
// running on a build it was not compiled for, and it refuses to load.
constexpr std::uint8_t kSrvTargetIteratorBytes[]{
	0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18,
	0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x60,
};
constexpr std::uint8_t kSrvBoltCallbackBytes[]{
	0x48, 0x89, 0x5C, 0x24, 0x20, 0x56, 0x48, 0x81, 0xEC, 0xE0, 0x00, 0x00, 0x00,
};
constexpr std::uint8_t kCltTargetIteratorBytes[]{
	0x40, 0x55, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
	0x48, 0x8D, 0x6C, 0x24, 0xF1, 0x48, 0x81, 0xEC, 0xA8, 0x00, 0x00, 0x00,
};
constexpr std::uint8_t kCltBoltCallbackBytes[]{
	0x40, 0x53, 0x48, 0x81, 0xEC, 0xE0, 0x00, 0x00, 0x00,
};

// --------------------------------------------------------------------------
// Build-specific constants: bolt sync
// --------------------------------------------------------------------------

// The call inside the server bolt callback that creates each bolt.
constexpr std::uint64_t kSrvBoltCreateCallRva = 0x00465123ULL;
constexpr std::uint8_t  kSrvBoltCreateCallBytes[]{0xE8, 0x78, 0x20, 0x0D, 0x00};

// The instructions leading up to that call, checked as a whole so the
// register contract (rcx = pGame, rdx = params) is proven, not assumed.
constexpr std::uint64_t kSrvBoltCreateContextRva = 0x00465117ULL;
constexpr std::uint8_t  kSrvBoltCreateContextBytes[]{
	0x48, 0x8B, 0x0E,             // mov  rcx, [rsi]         pGame from the iterator context
	0x48, 0x8D, 0x54, 0x24, 0x30, // lea  rdx, [rsp+30h]     missile params
	0x89, 0x7C, 0x24, 0x60,       // mov  [rsp+60h], edi     params target y
	0xE8, 0x78, 0x20, 0x0D, 0x00, // call 0x5371A0           create missile
};

constexpr std::uint64_t kCreateMissileRva = 0x005371A0ULL;
constexpr std::uint8_t  kCreateMissileBytes[]{
	0x40, 0x55, 0x57, 0x41, 0x56, 0x48, 0x8D, 0x6C, 0x24, 0xC0,
	0x48, 0x81, 0xEC, 0x40, 0x01, 0x00, 0x00,
};

constexpr std::uint64_t kSendMissileRva = 0x00537BC0ULL;
constexpr std::uint8_t  kSendMissileBytes[]{
	0x40, 0x55, 0x53, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24,
	0x50, 0xFF, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0xB0, 0x01, 0x00, 0x00,
};

// The sender's own gate: record byte +0x24 tested against the ClientSend
// mask, then the per-missile force byte at unit +0x1B4.
constexpr std::uint64_t kSendGateRva = 0x00537C97ULL;
constexpr std::uint8_t  kSendGateBytes[]{
	0x0F, 0xB6, 0x40, 0x24,                   // movzx eax, byte [rax+24h]
	0x84, 0x05, 0x43, 0x1A, 0x86, 0x01,       // test  [ClientSend mask 0x20], al
	0x75, 0x20,                               // jnz   send
	0x80, 0xBB, 0xB4, 0x01, 0x00, 0x00, 0x00, // cmp   byte [rbx+1B4h], 0
};

// Room of a unit (UNITS_GetRoom), f(unit) -> room.
constexpr std::uint64_t kGetUnitRoomRva = 0x0034B440ULL;
constexpr std::uint8_t  kGetUnitRoomBytes[]{
	0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9, 0x75, 0x13,
	0x88, 0x4C, 0x24, 0x30, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8, 0x54, 0xA7, 0xFF, 0xFF,
};

// Clients that have a room in sight: array at room +0x68, count at +0x70.
// This is the list the stock code uses to send a unit to every client that
// sees it (0x48F4F0 -> 0x538690).
constexpr std::uint64_t kRoomClientsReadRva = 0x002EFB20ULL;
constexpr std::uint8_t  kRoomClientsReadBytes[]{
	0x48, 0x8B, 0x41, 0x68, // mov rax, [rcx+68h]   client array
	0x48, 0x89, 0x02,       // mov [rdx], rax
	0x48, 0x8B, 0x41, 0x70, // mov rax, [rcx+70h]   client count
	0x49, 0x89, 0x00,       // mov [r8], rax
	0xC3,
};

// Bounds-checked: null for a negative or out-of-range id.
constexpr std::uint64_t kGetMissileRecordRva = 0x00166040ULL;
constexpr std::uint8_t  kGetMissileRecordBytes[]{
	0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x63, 0xDA,
	0xE8, 0x42, 0xAA, 0x19, 0x00, 0x85, 0xDB, 0x78, 0x4E,
};

// Field witnesses for the direct reads below. Each one is the instruction in
// the game's own getter that reads the field.
constexpr std::uint64_t kClassIdReadRva = 0x00349886ULL; // UNITS_GetClassId
constexpr std::uint8_t  kClassIdReadBytes[]{0x8B, 0x41, 0x04, 0x48, 0x83, 0xC4, 0x28, 0xC3};
constexpr std::uint64_t kUnitGuidReadRva = 0x0034A356ULL; // UNITS_GetUnitId
constexpr std::uint8_t  kUnitGuidReadBytes[]{0x8B, 0x41, 0x08, 0x48, 0x83, 0xC4, 0x28, 0xC3};
constexpr std::uint64_t kDataContextReadRva = 0x0034A103ULL; // unit data context
constexpr std::uint8_t  kDataContextReadBytes[]{0x0F, 0xB6, 0x81, 0xBD, 0x01, 0x00, 0x00, 0x48, 0x83, 0xC4, 0x28, 0xC3};
constexpr std::uint64_t kPathVelocityReadRva = 0x00341D30ULL; // path velocity
constexpr std::uint8_t  kPathVelocityReadBytes[]{0x8B, 0x81, 0xA0, 0x00, 0x00, 0x00, 0xC3};
constexpr std::uint64_t kSrvHitFuncReadRva = 0x00463E1DULL; // movsx rcx, word [r13+2Eh]
constexpr std::uint8_t  kSrvHitFuncReadBytes[]{0x49, 0x0F, 0xBF, 0x4D, 0x2E};

// missiles.txt row count of a data context: [tables + 0x1128], tables from
// 0x300A90(context). Both proven by the record lookup's own bounds check.
constexpr std::uint64_t kGetDataTablesRva = 0x00300A90ULL;
constexpr std::uint64_t kMissileCountReadRva = 0x00166049ULL;
constexpr std::uint8_t  kMissileCountReadBytes[]{
	0xE8, 0x42, 0xAA, 0x19, 0x00,             // call 0x300A90      tables of the context
	0x85, 0xDB, 0x78, 0x4E,                   // test ebx, ebx / js
	0x48, 0x8B, 0xCB,                         // mov  rcx, rbx
	0x48, 0x3B, 0x98, 0x28, 0x11, 0x00, 0x00, // cmp  rbx, [rax+1128h]   row count
};

// Client: handler of the server's create-missile packet (0x73). It only
// queues the packet's missiles; 0x131900 creates them later in the client
// update, through D2RLoader's replacement of the rebuild function.
constexpr std::uint64_t kCltMissilePacketRva = 0x0012D290ULL;
constexpr std::uint8_t  kCltMissilePacketBytes[]{
	0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x57,
	0x48, 0x8D, 0x6C, 0x24, 0xC9, 0x48, 0x81, 0xEC, 0xB0, 0x00, 0x00, 0x00,
};

// Client: "ProcessServerMissiles", once per client update.
constexpr std::uint64_t kCltProcessServerMissilesRva = 0x00131900ULL;
constexpr std::uint8_t  kCltProcessServerMissilesBytes[]{
	0x40, 0x53, 0x56, 0x57, 0x41, 0x54, 0x48, 0x83, 0xEC, 0x38,
	0x4C, 0x89, 0x6C, 0x24, 0x70, 0x4C, 0x89, 0x7C, 0x24, 0x30,
};

// Client unit by (id ecx, type edx).
constexpr std::uint64_t kCltGetUnitRva = 0x0009A5D0ULL;
constexpr std::uint8_t  kCltGetUnitBytes[]{
	0x4C, 0x63, 0xCA, 0x48, 0x8D, 0x05, 0x36, 0x93, 0x98, 0x02, 0x8B, 0xD1, 0x44, 0x8B,
	0xC1, 0x49, 0x8B, 0xC9, 0x83, 0xE2, 0x7F, 0x48, 0xC1, 0xE1, 0x0A, 0x48, 0x03, 0xC8,
};

// Client create missile, f(params, 0): the same function the stock client
// bolt callback (0x1BC840) uses.
constexpr std::uint64_t kCltCreateMissileRva = 0x001B7760ULL;
constexpr std::uint8_t  kCltCreateMissileBytes[]{
	0x89, 0x54, 0x24, 0x10, 0x55, 0x57, 0x41, 0x55, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xC1,
	0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xF9, 0x48, 0x8B, 0x49, 0x08,
};

// --------------------------------------------------------------------------
// Layout constants
// --------------------------------------------------------------------------

constexpr std::size_t kUnitClassIdOffset     = 0x04;
constexpr std::size_t kUnitGuidOffset        = 0x08;
constexpr std::size_t kUnitPathOffset        = 0x38;
constexpr std::size_t kUnitForceSendOffset   = 0x1B4;
constexpr std::size_t kUnitDataContextOffset = 0x1BD;
constexpr std::size_t kPathVelocityOffset    = 0xA0;

// Lightning Fury bolts remembered by the server so the sender hook can tell
// them apart from every other missile. Bolts live for a few seconds at most,
// so the newest 4096 cover every bolt that can still be in flight.
constexpr std::size_t kBoltRegistrySize = 4096;

// Create-missile packet 0x73 (built by 0x537BC0, parsed by 0x12D290).
constexpr std::uint8_t kPacketCreateMissile = 0x73;
constexpr std::size_t  kPktSkillId    = 0x05;
constexpr std::size_t  kPktCastId     = 0x09;
constexpr std::size_t  kPktOwnerType  = 0x0F;
constexpr std::size_t  kPktOwnerId    = 0x10;
constexpr std::size_t  kPktCount      = 0x14;
constexpr std::size_t  kPktEntry      = 0x15; // first of up to 5 entries, 67 bytes each
constexpr std::size_t  kEntX          = 0x0E;
constexpr std::size_t  kEntY          = 0x12;
constexpr std::size_t  kEntTargetDx   = 0x16; // signed byte
constexpr std::size_t  kEntTargetDy   = 0x17; // signed byte
constexpr std::size_t  kEntClassId    = 0x18; // word
constexpr std::size_t  kEntLevel      = 0x1C; // byte

constexpr std::size_t kTablesMissileCountOffset = 0x1128;

constexpr std::size_t kRoomClientsOffset     = 0x68;
constexpr std::size_t kRoomClientCountOffset = 0x70;

// Client missile params, the layout the stock client bolt callback fills.
constexpr std::size_t   kParamsSize        = 0xA0;
constexpr std::size_t   kParamFlags        = 0x00;
constexpr std::size_t   kParamOwner        = 0x08;
constexpr std::size_t   kParamMissileId    = 0x20;
constexpr std::size_t   kParamX            = 0x24;
constexpr std::size_t   kParamY            = 0x28;
constexpr std::size_t   kParamTargetX      = 0x2C;
constexpr std::size_t   kParamTargetY      = 0x30;
constexpr std::size_t   kParamSkillId      = 0x3C;
constexpr std::size_t   kParamSkillLevel   = 0x40;
constexpr std::size_t   kParamCastId       = 0x68;
constexpr std::uint32_t kParamFlagPosition = 0x01; // spawn at x/y
constexpr std::uint32_t kParamFlagTarget   = 0x20; // fly to target x/y

constexpr std::size_t kMaxDataContexts = 8;
constexpr std::size_t kPendingBoltsMax = 512;

constexpr std::size_t kPathDwordXOffset = 0x10;
constexpr std::size_t kPathDwordYOffset = 0x14;
constexpr std::size_t kPathWordXOffset  = 0x02;
constexpr std::size_t kPathWordYOffset  = 0x06;

constexpr std::size_t   kRecordSrvHitFuncOffset      = 0x2E;
constexpr std::size_t   kRecordHitSubMissileOffset    = 0x46;
constexpr std::size_t   kRecordCltHitSubMissileOffset = 0x4E;
constexpr std::int16_t  kLightningFurySrvHitFunc     = 20;

constexpr std::size_t kSrvWrapperBoltCountOffset = 0x1C;
constexpr std::size_t kCltWrapperBoltCountOffset = 0x18;
constexpr std::size_t kCltWrapperMissileOffset   = 0x08;

// Defaults, overridden by the plugin's TOML config.
constexpr std::int32_t kDefaultFilterBit = 0x40000000;
constexpr std::int32_t kDefaultBoltCount = 1;

// --------------------------------------------------------------------------
// Game function types
// --------------------------------------------------------------------------

using SrvTargetIteratorFn = std::int64_t(__fastcall*)(
	void*         pGame,
	void*         caster,
	std::int32_t  x,
	std::int32_t  y,
	std::int32_t  range,
	std::int32_t  filter,
	void*         callback,
	void*         wrapper,
	std::int32_t  flag,
	const char*   file,
	std::int32_t  line) noexcept;

using CltTargetIteratorFn = std::int64_t(__fastcall*)(
	void*         caster,
	std::uint32_t x,
	std::uint32_t y,
	std::int32_t  range,
	std::int32_t  filter,
	void*         callback,
	void*         wrapper,
	std::int32_t  flag) noexcept;

using BoltCallbackFn     = std::uint32_t(__fastcall*)(void* iteratorCtx, void* targetUnit) noexcept;
using CreateMissileFn    = void*(__fastcall*)(void* pGame, void* params) noexcept;
using SendMissileFn      = std::uint64_t(__fastcall*)(void* client, void* pGame, void* missile, std::int32_t velocity) noexcept;
using GetUnitRoomFn      = void*(__fastcall*)(void* unit) noexcept;
using GetMissileRecordFn = const std::uint8_t*(__fastcall*)(std::uint32_t dataContext, std::int32_t missileId) noexcept;
using GetDataTablesFn    = const std::uint8_t*(__fastcall*)(std::uint32_t dataContext) noexcept;
using CltPacketFn        = void(__fastcall*)(const std::uint8_t* packet, std::uintptr_t passThrough) noexcept;
using CltProcessFn       = void(__fastcall*)() noexcept;
using CltGetUnitFn       = void*(__fastcall*)(std::uint32_t unitId, std::uint32_t unitType) noexcept;
using CltCreateMissileFn = void*(__fastcall*)(void* params, std::uint32_t extra) noexcept;

struct SrvIteratorCtx {
	void*        pGame;
	void*        caster;
	std::int32_t counter;
	std::int32_t padding;
	void*        wrapper;
};

struct CltIteratorCtx {
	void*        caster;
	std::int32_t counter;
	std::int32_t padding;
	void*        wrapper;
};

static_assert(sizeof(SrvIteratorCtx) == 0x20, "server iterator context layout");
static_assert(sizeof(CltIteratorCtx) == 0x18, "client iterator context layout");
static_assert(offsetof(SrvIteratorCtx, wrapper) == 0x18, "server wrapper slot");
static_assert(offsetof(CltIteratorCtx, wrapper) == 0x10, "client wrapper slot");

// --------------------------------------------------------------------------
// State
// --------------------------------------------------------------------------

const D2RL::PluginContext* g_context = nullptr;

SrvTargetIteratorFn g_originalSrvIterator = nullptr;
CltTargetIteratorFn g_originalCltIterator = nullptr;

BoltCallbackFn     g_srvBoltCallback      = nullptr;
BoltCallbackFn     g_cltBoltCallback      = nullptr;
CreateMissileFn    g_createMissile        = nullptr;
SendMissileFn      g_originalSendMissile  = nullptr; // trampoline of the hooked sender
GetUnitRoomFn      g_getUnitRoom          = nullptr;
GetMissileRecordFn g_getMissileRecord     = nullptr;

struct Settings {
	bool         syncBolts            = true;
	bool         enabled              = true;
	bool         hookClient           = true;
	std::int32_t filterBit            = kDefaultFilterBit;
	std::int32_t boltCount            = kDefaultBoltCount;
	bool         requireCasterInRange = true;
};

Settings g_settings{};

bool g_srvHooked  = false;
bool g_cltHooked  = false;
bool g_syncServer = false; // create-call redirect AND sender hook installed
bool g_syncActive = false; // every server and client hook of bolt sync is installed

volatile LONG g_srvRedirects         = 0;
volatile LONG g_cltRedirects         = 0;
volatile LONG g_syncBoltsSent        = 0; // bolts sent to at least one client
volatile LONG g_syncPacketsSent      = 0; // one per client per bolt
volatile LONG g_syncNoViewer         = 0; // bolts whose room no client has in sight
volatile LONG g_syncStockSkipped     = 0; // stock sends of an already sent bolt, dropped
volatile LONG g_cltLocalBoltsSkipped = 0;
volatile LONG g_cltBoltsReceived     = 0;
volatile LONG g_cltBoltsCreated      = 0;
volatile LONG g_cltBoltsFailed       = 0;

GetDataTablesFn    g_getDataTables            = nullptr;
CltPacketFn        g_originalCltMissilePacket = nullptr;
CltProcessFn       g_originalCltProcess       = nullptr;
CltGetUnitFn       g_cltGetUnit               = nullptr;
CltCreateMissileFn g_cltCreateMissile         = nullptr;

bool g_cltPacketHooked  = false;
bool g_cltProcessHooked = false;

// Client side: classes that are a Lightning Fury bolt, per data context, built
// from missiles.txt (rows with pSrvHitFunc 20, their HitSubMissile1).
SRWLOCK      g_cltLock = SRWLOCK_INIT;
bool         g_boltClassBuilt[kMaxDataContexts]{};
std::uint8_t g_boltClassBits[kMaxDataContexts][65536 / 8]{};

struct PendingBolt {
	std::uint32_t ownerType;
	std::uint32_t ownerId;
	std::int32_t  skillId;
	std::uint32_t castId;
	std::int32_t  classId;
	std::int32_t  x;
	std::int32_t  y;
	std::int32_t  targetX;
	std::int32_t  targetY;
	std::int32_t  level;
};

PendingBolt g_pendingBolts[kPendingBoltsMax]{};
std::size_t g_pendingCount = 0;

// One bit per missiles.txt row id, so each row is reported once.
std::uint8_t g_notedVisual[65536 / 8]{};

// Server-side registry of Lightning Fury bolts. Only touched on the server
// thread (bolt creation and the sender both run there).
struct RememberedBolt {
	const void*   unit;
	const void*   game;
	std::uint32_t guid;
	std::uint32_t padding;
};

RememberedBolt g_boltRegistry[kBoltRegistrySize]{};
std::size_t    g_boltRegistryNext = 0;
std::uint8_t   g_boltClasses[65536 / 8]{}; // quick pre-check: classes ever used as a Lightning Fury bolt

std::uint8_t* g_relayPage = nullptr;

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------

auto ReadInt32(const void* base, std::size_t offset) noexcept -> std::int32_t {
	std::int32_t value = 0;
	std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(value));
	return value;
}

auto ReadInt16(const void* base, std::size_t offset) noexcept -> std::int16_t {
	std::int16_t value = 0;
	std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(value));
	return value;
}

auto ReadU8(const void* base, std::size_t offset) noexcept -> std::uint8_t {
	return *(static_cast<const std::uint8_t*>(base) + offset);
}

auto ReadU16(const void* base, std::size_t offset) noexcept -> std::uint16_t {
	std::uint16_t value = 0;
	std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(value));
	return value;
}

void WriteU32(void* base, std::size_t offset, std::uint32_t value) noexcept {
	std::memcpy(static_cast<std::uint8_t*>(base) + offset, &value, sizeof(value));
}

void WritePtr(void* base, std::size_t offset, const void* value) noexcept {
	std::memcpy(static_cast<std::uint8_t*>(base) + offset, &value, sizeof(value));
}

auto ReadPtr(const void* base, std::size_t offset) noexcept -> void* {
	void* value = nullptr;
	std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(value));
	return value;
}

auto TestAndSetNoted(std::uint8_t* bits, std::int32_t id) noexcept -> bool {
	if (id < 0 || id >= 65536) {
		return true;
	}
	const auto         index = static_cast<std::uint32_t>(id);
	const std::uint8_t mask  = static_cast<std::uint8_t>(1U << (index & 7U));
	const bool         was   = (bits[index >> 3U] & mask) != 0;
	bits[index >> 3U] = static_cast<std::uint8_t>(bits[index >> 3U] | mask);
	return was;
}

// missiles.txt record of a missile unit, using the unit's own data context.
auto RecordOfMissileId(const void* unit, std::int32_t missileId) noexcept -> const std::uint8_t* {
	if (unit == nullptr || missileId < 0 || g_getMissileRecord == nullptr) {
		return nullptr;
	}
	const std::uint32_t dataContext = ReadU8(unit, kUnitDataContextOffset);
	return g_getMissileRecord(dataContext, missileId);
}

auto ClassIdOf(const void* unit) noexcept -> std::int32_t {
	return ReadInt32(unit, kUnitClassIdOffset);
}

// Reproduces the position read the game performs inline at every one of these
// call sites: dword coordinates for unit types 2, 4 and 5, word coordinates
// for everything else.
auto GetUnitPosition(const void* unit, std::int32_t* outX, std::int32_t* outY) noexcept -> bool {
	if (unit == nullptr) {
		return false;
	}

	const std::uint32_t type = *reinterpret_cast<const std::uint32_t*>(unit);
	const auto*         path = static_cast<const std::uint8_t*>(ReadPtr(unit, kUnitPathOffset));
	if (path == nullptr) {
		return false;
	}

	if (type == 2U || (type - 4U) < 2U) {
		*outX = ReadInt32(path, kPathDwordXOffset);
		*outY = ReadInt32(path, kPathDwordYOffset);
	} else {
		*outX = static_cast<std::int32_t>(*reinterpret_cast<const std::uint16_t*>(path + kPathWordXOffset));
		*outY = static_cast<std::int32_t>(*reinterpret_cast<const std::uint16_t*>(path + kPathWordYOffset));
	}
	return true;
}

// Same gate the enemy search itself applies to a candidate: range^2 >= dist^2.
auto CasterIsInRange(const void* caster, std::int32_t x, std::int32_t y, std::int32_t range) noexcept -> bool {
	if (range <= 0) {
		return false;
	}

	// The iterators treat a zero coordinate as "use the caster's own position",
	// in which case the caster is trivially at the impact point.
	if (x == 0 || y == 0) {
		return true;
	}

	if (!g_settings.requireCasterInRange) {
		return true;
	}

	std::int32_t casterX = 0;
	std::int32_t casterY = 0;
	if (!GetUnitPosition(caster, &casterX, &casterY)) {
		return false;
	}

	const std::int64_t dx = static_cast<std::int64_t>(casterX) - x;
	const std::int64_t dy = static_cast<std::int64_t>(casterY) - y;
	return static_cast<std::int64_t>(range) * range >= dx * dx + dy * dy;
}

auto ShouldRedirect(const void* wrapper, std::size_t boltCountOffset, std::int32_t filter) noexcept -> bool {
	return wrapper != nullptr
		&& (filter & g_settings.filterBit) != 0
		&& ReadInt32(wrapper, boltCountOffset) == g_settings.boltCount;
}

// --------------------------------------------------------------------------
// Bolt sync
// --------------------------------------------------------------------------

auto TestBit(const std::uint8_t* bits, std::int32_t id) noexcept -> bool {
	if (id < 0 || id >= 65536) {
		return false;
	}
	const auto index = static_cast<std::uint32_t>(id);
	return (bits[index >> 3U] & static_cast<std::uint8_t>(1U << (index & 7U))) != 0;
}

void SetBit(std::uint8_t* bits, std::int32_t id) noexcept {
	if (id < 0 || id >= 65536) {
		return;
	}
	const auto index = static_cast<std::uint32_t>(id);
	bits[index >> 3U] = static_cast<std::uint8_t>(bits[index >> 3U] | (1U << (index & 7U)));
}

// Client side: true when the server will send this hit's bolts, so the
// client must not draw its own. That is the case for every javelin row whose
// server function is 20 and that names a bolt in HitSubMissile1: exactly the
// condition under which the server hit function creates bolts.
auto ServerSendsBolts(const void* wrapper) noexcept -> bool {
	if (wrapper == nullptr) {
		return false;
	}

	const void* javelin = ReadPtr(wrapper, kCltWrapperMissileOffset);
	if (javelin == nullptr) {
		return false;
	}

	const std::int32_t  javelinId     = ClassIdOf(javelin);
	const std::uint8_t* javelinRecord = RecordOfMissileId(javelin, javelinId);
	if (javelinRecord == nullptr || ReadInt16(javelinRecord, kRecordSrvHitFuncOffset) != kLightningFurySrvHitFunc) {
		return false;
	}

	const std::int32_t boltId = ReadInt16(javelinRecord, kRecordHitSubMissileOffset);
	if (RecordOfMissileId(javelin, boltId) == nullptr) {
		return false;
	}

	const std::int32_t clientBoltId = ReadInt16(javelinRecord, kRecordCltHitSubMissileOffset);
	if (clientBoltId >= 0 && clientBoltId != boltId && !TestAndSetNoted(g_notedVisual, javelinId) && g_context != nullptr) {
		char message[256]{};
		std::snprintf(message, sizeof(message),
		              "missiles.txt row %d: synced bolts are drawn as row %d (HitSubMissile1). Its CltHitSubMissile1 names row %d, which is no longer drawn for this javelin.",
		              static_cast<int>(javelinId), static_cast<int>(boltId), static_cast<int>(clientBoltId));
		g_context->LogWarn(message);
	}
	return true;
}

void RememberBolt(void* pGame, void* bolt) noexcept {
	RememberedBolt& slot = g_boltRegistry[g_boltRegistryNext];
	slot.unit    = bolt;
	slot.game    = pGame;
	slot.guid    = static_cast<std::uint32_t>(ReadInt32(bolt, kUnitGuidOffset));
	slot.padding = 0;
	g_boltRegistryNext = (g_boltRegistryNext + 1U) % kBoltRegistrySize;
	SetBit(g_boltClasses, ClassIdOf(bolt));
}

// Pointer, GUID and game must all match, so a recycled unit block or a later
// game can never be mistaken for a remembered bolt.
auto IsRememberedBolt(const void* pGame, const void* unit) noexcept -> bool {
	if (unit == nullptr || !TestBit(g_boltClasses, ClassIdOf(unit))) {
		return false;
	}
	const auto  guid  = static_cast<std::uint32_t>(ReadInt32(unit, kUnitGuidOffset));
	std::size_t index = g_boltRegistryNext;
	for (std::size_t i = 0; i < kBoltRegistrySize; ++i) {
		index = (index + kBoltRegistrySize - 1U) % kBoltRegistrySize;
		const RememberedBolt& slot = g_boltRegistry[index];
		if (slot.unit == unit && slot.guid == guid && slot.game == pGame) {
			return true;
		}
	}
	return false;
}

// Runs the stock sender with the missile's force-send byte set, then puts the
// byte back exactly as it was.
auto SendForced(void* client, void* pGame, void* missile, std::int32_t velocity) noexcept -> std::uint64_t {
	auto* const        force = static_cast<std::uint8_t*>(missile) + kUnitForceSendOffset;
	const std::uint8_t saved = *force;
	*force = 1;
	const std::uint64_t result = g_originalSendMissile(client, pGame, missile, velocity);
	*force = saved;
	return result;
}

// Server side, right after a Lightning Fury bolt is created: send it to every
// client that has the bolt's room in sight, the thrower's client included.
// The stock code never sends a missile without ClientSend to any client, and
// even with ClientSend it skips the owner's client, so the plugin sends it
// itself through the stock sender, using the same client list the stock code
// uses when a unit comes into a client's sight.
void SyncNewBolt(void* pGame, void* bolt) noexcept {
	RememberBolt(pGame, bolt);

	const void* room = g_getUnitRoom(bolt);
	if (room == nullptr) {
		::InterlockedIncrement(&g_syncNoViewer);
		return;
	}

	void* const* clients = static_cast<void* const*>(ReadPtr(room, kRoomClientsOffset));
	std::uint64_t count  = 0;
	std::memcpy(&count, static_cast<const std::uint8_t*>(room) + kRoomClientCountOffset, sizeof(count));
	if (clients == nullptr || count == 0) {
		::InterlockedIncrement(&g_syncNoViewer);
		return;
	}

	// Same velocity argument the stock path passes (path velocity): it decides
	// whether the packet carries the bolt's target point.
	const void*        path     = ReadPtr(bolt, kUnitPathOffset);
	const std::int32_t velocity = (path != nullptr) ? ReadInt32(path, kPathVelocityOffset) : 0;

	bool sent = false;
	for (std::uint64_t i = 0; i < count; ++i) {
		void* const client = clients[i];
		if (client == nullptr) {
			continue;
		}
		SendForced(client, pGame, bolt, velocity);
		::InterlockedIncrement(&g_syncPacketsSent);
		sent = true;
	}
	if (sent) {
		::InterlockedIncrement(&g_syncBoltsSent);
	} else {
		::InterlockedIncrement(&g_syncNoViewer);
	}
}

// Target of the redirected `call 0x5371A0` inside the server bolt callback.
// Creates the bolt exactly as the game would, then syncs it.
auto __fastcall OnServerBoltCreate(void* pGame, void* params) noexcept -> void* {
	void* const bolt = g_createMissile(pGame, params);
	if (bolt != nullptr && g_syncActive) {
		SyncNewBolt(pGame, bolt);
	}
	return bolt;
}

// Hook on the stock create-missile sender. A remembered Lightning Fury bolt
// was already sent to every client in sight when it was created, so a later
// stock send of the same bolt (a ClientSend row coming into a client's sight)
// is dropped instead of creating a second copy. Everything else passes
// through untouched. The plugin's own sends call the original directly.
auto __fastcall HookSendMissile(void* client, void* pGame, void* missile, std::int32_t velocity) noexcept -> std::uint64_t {
	if (g_syncActive && IsRememberedBolt(pGame, missile)) {
		::InterlockedIncrement(&g_syncStockSkipped);
		return 0;
	}
	const SendMissileFn original = g_originalSendMissile;
	return (original != nullptr) ? original(client, pGame, missile, velocity) : 0;
}

// --------------------------------------------------------------------------
// Bolt sync, client half: the server's bolts become client bolts
// --------------------------------------------------------------------------
//
// The server's bolt arrives as a create-missile packet (0x73). Stock D2R
// queues it and later rebuilds it as a mirror of the server unit, which the
// server's remove-missile packet deletes again. A Lightning Fury bolt lives
// only a few frames on the server, so its remove packet can arrive in the
// same client update and cancel the queued create before it is ever built.
// The rebuild itself is also D2RLoader's own replacement code.
//
// So Lightning Fury bolts take a separate path: their packet is captured and
// turned into an ordinary client bolt, created through the same client create
// function and parameter block the stock client bolt callback uses, at the
// server's spawn point and flying to the server's target. It then lives its
// own visual lifetime exactly like a stock client bolt, and the server's later
// remove packet finds nothing to cancel.

// Rebuilds the "is a Lightning Fury bolt" class set of one data context from
// missiles.txt. Caller holds g_cltLock exclusively.
void BuildBoltClasses(std::uint32_t dataContext) noexcept {
	std::uint8_t* bits = g_boltClassBits[dataContext];
	std::memset(bits, 0, sizeof(g_boltClassBits[0]));

	const std::uint8_t* tables = g_getDataTables(dataContext);
	if (tables != nullptr) {
		std::uint64_t count = 0;
		std::memcpy(&count, tables + kTablesMissileCountOffset, sizeof(count));
		if (count > 65536U) {
			count = 65536U;
		}
		for (std::uint32_t id = 0; id < count; ++id) {
			const std::uint8_t* record = g_getMissileRecord(dataContext, static_cast<std::int32_t>(id));
			if (record != nullptr && ReadInt16(record, kRecordSrvHitFuncOffset) == kLightningFurySrvHitFunc) {
				SetBit(bits, ReadInt16(record, kRecordHitSubMissileOffset));
			}
		}
	}
	g_boltClassBuilt[dataContext] = true;
}

auto IsBoltClass(std::uint32_t dataContext, std::int32_t classId) noexcept -> bool {
	if (dataContext >= kMaxDataContexts) {
		return false;
	}
	::AcquireSRWLockExclusive(&g_cltLock);
	if (!g_boltClassBuilt[dataContext]) {
		BuildBoltClasses(dataContext);
	}
	const bool result = TestBit(g_boltClassBits[dataContext], classId);
	::ReleaseSRWLockExclusive(&g_cltLock);
	return result;
}

// True when the packet is one Lightning Fury bolt; it is then queued for the
// plugin and must not reach the stock handler.
auto CaptureServerBolt(const std::uint8_t* packet) noexcept -> bool {
	if (packet[0] != kPacketCreateMissile || packet[kPktCount] != 1) {
		return false;
	}

	const std::uint8_t* entry     = packet + kPktEntry;
	const std::uint32_t ownerType = packet[kPktOwnerType];
	const auto          ownerId   = static_cast<std::uint32_t>(ReadInt32(packet, kPktOwnerId));
	void* const         owner     = g_cltGetUnit(ownerId, ownerType);
	if (owner == nullptr) {
		return false;
	}

	const std::int32_t classId = ReadU16(entry, kEntClassId);
	if (!IsBoltClass(ReadU8(owner, kUnitDataContextOffset), classId)) {
		return false;
	}

	PendingBolt bolt{};
	bolt.ownerType = ownerType;
	bolt.ownerId   = ownerId;
	bolt.skillId   = ReadInt32(packet, kPktSkillId);
	bolt.castId    = static_cast<std::uint32_t>(ReadInt32(packet, kPktCastId));
	bolt.classId   = classId;
	bolt.x         = ReadInt32(entry, kEntX);
	bolt.y         = ReadInt32(entry, kEntY);
	bolt.targetX   = bolt.x + static_cast<std::int8_t>(entry[kEntTargetDx]);
	bolt.targetY   = bolt.y + static_cast<std::int8_t>(entry[kEntTargetDy]);
	bolt.level     = entry[kEntLevel];

	::AcquireSRWLockExclusive(&g_cltLock);
	const bool queued = g_pendingCount < kPendingBoltsMax;
	if (queued) {
		g_pendingBolts[g_pendingCount++] = bolt;
	}
	::ReleaseSRWLockExclusive(&g_cltLock);
	if (queued) {
		::InterlockedIncrement(&g_cltBoltsReceived);
	}
	return queued;
}

// The handler reads only its packet argument; the second register is passed
// through unchanged so the stock handler sees exactly what the dispatcher sent.
void __fastcall HookCltMissilePacket(const std::uint8_t* packet, std::uintptr_t passThrough) noexcept {
	if (g_syncActive && packet != nullptr && CaptureServerBolt(packet)) {
		return;
	}
	const CltPacketFn original = g_originalCltMissilePacket;
	if (original != nullptr) {
		original(packet, passThrough);
	}
}

// Creates the captured bolts as client bolts, in the same client update and
// at the same point where the stock code creates server missiles.
void CreatePendingBolts() noexcept {
	static PendingBolt bolts[kPendingBoltsMax]; // client update thread only
	std::size_t        count = 0;

	::AcquireSRWLockExclusive(&g_cltLock);
	count = g_pendingCount;
	if (count != 0) {
		std::memcpy(bolts, g_pendingBolts, count * sizeof(PendingBolt));
		g_pendingCount = 0;
	}
	::ReleaseSRWLockExclusive(&g_cltLock);

	for (std::size_t i = 0; i < count; ++i) {
		const PendingBolt& bolt  = bolts[i];
		void* const        owner = g_cltGetUnit(bolt.ownerId, bolt.ownerType);
		if (owner == nullptr) {
			::InterlockedIncrement(&g_cltBoltsFailed);
			continue;
		}

		alignas(16) std::uint8_t params[kParamsSize]{};
		WriteU32(params, kParamFlags, kParamFlagPosition | kParamFlagTarget);
		WritePtr(params, kParamOwner, owner);
		WriteU32(params, kParamMissileId, static_cast<std::uint32_t>(bolt.classId));
		WriteU32(params, kParamX, static_cast<std::uint32_t>(bolt.x));
		WriteU32(params, kParamY, static_cast<std::uint32_t>(bolt.y));
		WriteU32(params, kParamTargetX, static_cast<std::uint32_t>(bolt.targetX));
		WriteU32(params, kParamTargetY, static_cast<std::uint32_t>(bolt.targetY));
		WriteU32(params, kParamSkillId, static_cast<std::uint32_t>(bolt.skillId));
		WriteU32(params, kParamSkillLevel, static_cast<std::uint32_t>(bolt.level));
		WriteU32(params, kParamCastId, bolt.castId);

		if (g_cltCreateMissile(params, 0) != nullptr) {
			::InterlockedIncrement(&g_cltBoltsCreated);
		} else {
			::InterlockedIncrement(&g_cltBoltsFailed);
		}
	}
}

void __fastcall HookCltProcessServerMissiles() noexcept {
	if (g_syncActive) {
		CreatePendingBolts();
	}
	const CltProcessFn original = g_originalCltProcess;
	if (original != nullptr) {
		original();
	}
}

// A data reload can change missiles.txt: rebuild the class sets on next use.
void __cdecl OnDataTablesLoaded(const D2RL::PluginContext* context, const D2RL::Lifecycle::DataTablesLoadedEvent* event, void* userData) noexcept {
	(void)context;
	(void)event;
	(void)userData;
	::AcquireSRWLockExclusive(&g_cltLock);
	for (bool& built : g_boltClassBuilt) {
		built = false;
	}
	::ReleaseSRWLockExclusive(&g_cltLock);
}

// --------------------------------------------------------------------------
// Relay page for the call redirect (a rel32 call cannot reach the DLL)
// --------------------------------------------------------------------------

constexpr std::size_t kRelayPageBytes   = 4096;
constexpr std::size_t kRelayBoltCreate  = 0x000;

auto WithinRel32(std::uintptr_t next, std::uintptr_t target) noexcept -> bool {
	const std::int64_t delta = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(next);
	return delta >= INT32_MIN && delta <= INT32_MAX;
}

auto AllocateNear(std::uintptr_t hint, std::size_t size) noexcept -> std::uint8_t* {
	SYSTEM_INFO info{};
	::GetSystemInfo(&info);
	const auto granularity = static_cast<std::uintptr_t>(info.dwAllocationGranularity);
	const auto aligned     = hint & ~(granularity - 1U);
	for (std::uintptr_t delta = granularity; delta < 0x70000000ULL; delta += granularity) {
		const auto candidate = aligned + delta;
		if (!WithinRel32(hint, candidate + size)) {
			break;
		}
		if (auto* memory = ::VirtualAlloc(reinterpret_cast<void*>(candidate), size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) {
			return static_cast<std::uint8_t*>(memory);
		}
	}
	return nullptr;
}

void WriteAbsoluteJump(std::uint8_t* at, std::uint64_t target) noexcept {
	static constexpr std::uint8_t kJumpQwordRip[]{0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
	std::memcpy(at, kJumpQwordRip, sizeof(kJumpQwordRip));
	std::memcpy(at + sizeof(kJumpQwordRip), &target, sizeof(target));
}

auto PrepareRelayPage(std::uintptr_t base) noexcept -> bool {
	g_relayPage = AllocateNear(base + kSrvBoltCreateCallRva, kRelayPageBytes);
	if (g_relayPage == nullptr) {
		return false;
	}
	std::memset(g_relayPage, 0xCC, kRelayPageBytes);
	WriteAbsoluteJump(g_relayPage + kRelayBoltCreate, reinterpret_cast<std::uint64_t>(&OnServerBoltCreate));

	DWORD previous = 0;
	if (!::VirtualProtect(g_relayPage, kRelayPageBytes, PAGE_EXECUTE_READ, &previous)) {
		::VirtualFree(g_relayPage, 0, MEM_RELEASE);
		g_relayPage = nullptr;
		return false;
	}
	::FlushInstructionCache(::GetCurrentProcess(), g_relayPage, kRelayPageBytes);
	return true;
}

auto RedirectBoltCreateCall(const D2RL::PluginContext* context, std::uintptr_t base) noexcept -> bool {
	const auto next   = base + kSrvBoltCreateCallRva + 5U;
	const auto target = reinterpret_cast<std::uintptr_t>(g_relayPage) + kRelayBoltCreate;
	if (!WithinRel32(next, target)) {
		return false;
	}
	const auto   displacement = static_cast<std::int32_t>(static_cast<std::int64_t>(target) - static_cast<std::int64_t>(next));
	std::uint8_t bytes[5]{0xE8};
	std::memcpy(bytes + 1, &displacement, sizeof(displacement));
	return context->PatchBytes(kSrvBoltCreateCallRva,
	                           kSrvBoltCreateCallBytes, static_cast<std::uint32_t>(sizeof(kSrvBoltCreateCallBytes)),
	                           bytes, static_cast<std::uint32_t>(sizeof(bytes)));
}

// --------------------------------------------------------------------------
// Config file
// --------------------------------------------------------------------------

constexpr const char* kDefaultConfigToml =
	"# Lightning Fury: bolt sync and caster return\n"
	"# d2rl-celestialrayone-lightning-fury-caster-return.dll\n"
	"#\n"
	"# Covers every missile that uses the Lightning Fury hit functions:\n"
	"# missiles.txt pSrvHitFunc 20 on the server, pCltHitFunc 25 on the client.\n"
	"# A hit releases sHitPar2 bolts at units within sHitPar1 of the impact point\n"
	"# (the skill supplies both when the columns are blank). The bolt is the\n"
	"# missile named in the javelin row's HitSubMissile1 column.\n"
	"#\n"
	"# Console command: lf-caster-priority (state of both parts plus counters).\n"
	"#\n"
	"# ---------------------------------------------------------------------------\n"
	"# Part 1: bolt sync\n"
	"# ---------------------------------------------------------------------------\n"
	"# Stock D2R picks the bolt targets twice. The server picks the units that\n"
	"# take damage. Your client separately picks the units it draws bolts to,\n"
	"# using cHitPar1, cHitPar2 and CltHitSubMissile1. Nothing ties the two\n"
	"# together, so drawn bolts often fly at monsters that take no damage.\n"
	"#\n"
	"# With sync on, the server sends its own bolts to every client, the thrower\n"
	"# included, and clients stop drawing their own guesses. Every bolt on screen\n"
	"# is a bolt the server created, flying at the unit it actually targets.\n"
	"#\n"
	"# No missiles.txt edits are needed. The plugin applies this to every row\n"
	"# with pSrvHitFunc 20 that names a bolt in HitSubMissile1, and only to bolts\n"
	"# created by that hit; other uses of the same bolt row stay stock.\n"
	"#\n"
	"# Each client draws the server's bolt as an ordinary client bolt, created at\n"
	"# the server's spawn point and flying to the server's target, so it shows for\n"
	"# its full visual lifetime even when the server bolt dies a frame later.\n"
	"#\n"
	"#   * Synced bolts are drawn using the HitSubMissile1 row. CltHitSubMissile1\n"
	"#     is no longer used for these rows. The plugin log names every javelin\n"
	"#     row where the two columns point at different missiles.\n"
	"#   * Chains work the same way: a bolt that is itself a Lightning Fury\n"
	"#     missile syncs its own bolts when it hits.\n"
	"#   * The javelin itself is still drawn by your client's own prediction, so\n"
	"#     the bolts start where the server's javelin hit.\n"
	"#\n"
	"# Multiplayer: everyone in a TCP/IP game must run the same build of this\n"
	"# plugin. The server alone decides targets and damage, exactly as in stock\n"
	"# D2R, so a joining player cannot abuse this.\n"
	"\n"
	"[lf-bolt-sync]\n"
	"\n"
	"# true:  the server's bolts are what every client draws.\n"
	"# false: stock behaviour, client and server pick their bolts separately.\n"
	"sync_bolts = true\n"
	"\n"
	"# ---------------------------------------------------------------------------\n"
	"# Part 2: caster return\n"
	"# ---------------------------------------------------------------------------\n"
	"# A hit sends its bolt at the caster instead of at a random enemy when:\n"
	"#   * the hit fires exactly bolt_count bolts,\n"
	"#   * the skill's skills.txt aurafilter has aurafilter_bit set,\n"
	"#   * the caster is inside the hit's range of the impact point.\n"
	"# Otherwise the stock enemy search runs untouched.\n"
	"\n"
	"[lf-caster-priority]\n"
	"\n"
	"# Master switch for caster return. false leaves the hooks installed but inert.\n"
	"enabled = true\n"
	"\n"
	"# Mirror caster return on the client. Only used while sync_bolts is false;\n"
	"# with sync on, the client already receives the server's own caster bolt.\n"
	"hook_client = true\n"
	"\n"
	"# skills.txt aurafilter bit that marks a skill as caster return.\n"
	"# Decimal or 0x hex. Default is bit 30, the bit the 2.4 patches used.\n"
	"aurafilter_bit = 0x40000000\n"
	"\n"
	"# Bolt count the hit must have for its bolt to go to the caster.\n"
	"# 1 means only single-bolt hits are affected.\n"
	"bolt_count = 1\n"
	"\n"
	"# Require the caster to be within the hit's range of the impact point.\n"
	"# false redirects the bolt no matter how far away the caster is.\n"
	"require_caster_in_range = true\n";

auto IsSpace(char c) noexcept -> bool {
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Finds "key" at the start of a line and returns the text after its '='.
// Section headers are ignored, so every key name must be unique in the file.
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

void LoadSettings(const D2RL::PluginContext* context) noexcept {
	if (!context->EnsureConfig(kDefaultConfigToml)) {
		context->LogWarn("Could not create the config file, using built in defaults.");
		return;
	}

	static char   toml[8192]{};
	std::uint32_t requiredSize = 0;
	if (!context->ReadConfig(toml, static_cast<std::uint32_t>(sizeof(toml)), &requiredSize)) {
		context->LogWarn("Could not read the config file, using built in defaults.");
		return;
	}
	toml[sizeof(toml) - 1] = '\0';

	Settings settings{};
	settings.syncBolts            = ReadBool(toml, "sync_bolts", settings.syncBolts);
	settings.enabled              = ReadBool(toml, "enabled", settings.enabled);
	settings.hookClient           = ReadBool(toml, "hook_client", settings.hookClient);
	settings.filterBit            = ReadInt(toml, "aurafilter_bit", settings.filterBit);
	settings.boltCount            = ReadInt(toml, "bolt_count", settings.boltCount);
	settings.requireCasterInRange = ReadBool(toml, "require_caster_in_range", settings.requireCasterInRange);

	if (settings.filterBit == 0) {
		context->LogWarn("aurafilter_bit is 0, which would match every skill. Keeping the default bit.");
		settings.filterBit = kDefaultFilterBit;
	}
	if (settings.boltCount < 1) {
		context->LogWarn("bolt_count below 1 can never match. Keeping the default.");
		settings.boltCount = kDefaultBoltCount;
	}

	g_settings = settings;
}

// --------------------------------------------------------------------------
// Hooks
// --------------------------------------------------------------------------

auto __fastcall HookSrvTargetIterator(
	void*         pGame,
	void*         caster,
	std::int32_t  x,
	std::int32_t  y,
	std::int32_t  range,
	std::int32_t  filter,
	void*         callback,
	void*         wrapper,
	std::int32_t  flag,
	const char*   file,
	std::int32_t  line) noexcept -> std::int64_t {

	if (g_settings.enabled
		&& callback == reinterpret_cast<void*>(g_srvBoltCallback)
		&& caster != nullptr
		&& ShouldRedirect(wrapper, kSrvWrapperBoltCountOffset, filter)
		&& CasterIsInRange(caster, x, y, range)) {

		SrvIteratorCtx ctx{};
		ctx.pGame   = pGame;
		ctx.caster  = caster;
		ctx.counter = 0;
		ctx.padding = 0;
		ctx.wrapper = wrapper;

		// Goes through the game's own bolt callback, so with bolt sync on the
		// caster bolt is created through the redirected call and synced too.
		g_srvBoltCallback(&ctx, caster);
		::InterlockedIncrement(&g_srvRedirects);
		return 0;
	}

	const SrvTargetIteratorFn original = g_originalSrvIterator;
	if (original == nullptr) {
		return 0;
	}
	return original(pGame, caster, x, y, range, filter, callback, wrapper, flag, file, line);
}

auto __fastcall HookCltTargetIterator(
	void*         caster,
	std::uint32_t x,
	std::uint32_t y,
	std::int32_t  range,
	std::int32_t  filter,
	void*         callback,
	void*         wrapper,
	std::int32_t  flag) noexcept -> std::int64_t {

	if (callback == reinterpret_cast<void*>(g_cltBoltCallback)) {
		// Bolt sync: the server's copies of these bolts are on their way, so
		// the client draws nothing of its own for this hit.
		if (g_syncActive && ServerSendsBolts(wrapper)) {
			::InterlockedIncrement(&g_cltLocalBoltsSkipped);
			return 0;
		}

		if (g_settings.enabled
			&& g_settings.hookClient
			&& caster != nullptr
			&& ShouldRedirect(wrapper, kCltWrapperBoltCountOffset, filter)
			&& CasterIsInRange(caster, static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), range)) {

			CltIteratorCtx ctx{};
			ctx.caster  = caster;
			ctx.counter = 0;
			ctx.padding = 0;
			ctx.wrapper = wrapper;

			g_cltBoltCallback(&ctx, caster);
			::InterlockedIncrement(&g_cltRedirects);
			return 0;
		}
	}

	const CltTargetIteratorFn original = g_originalCltIterator;
	if (original == nullptr) {
		return 0;
	}
	return original(caster, x, y, range, filter, callback, wrapper, flag);
}

// --------------------------------------------------------------------------
// Plugin plumbing
// --------------------------------------------------------------------------

constexpr D2RL::PluginInfo kPluginInfo{
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = "celestialrayone.lf-caster-priority",
	.name        = "Lightning Fury Bolt Sync and Caster Return",
	.version     = "1.4.0",
	.author      = "CelestialRayOne",
	.description = "Lightning Fury bolts drawn by every client are the server's own bolts (no missiles.txt edits needed), and single-bolt hits can return to the caster (skills.txt aurafilter bit 0x40000000).",
	.flags       = D2RL::PluginFlags::Shared | D2RL::PluginFlags::NativeHooks,
};

template <std::size_t Size>
constexpr auto ByteCount(const std::uint8_t (&)[Size]) noexcept -> std::uint32_t {
	return static_cast<std::uint32_t>(Size);
}

auto CheckSite(const D2RL::PluginContext* context,
               std::uint64_t             rva,
               const std::uint8_t*       bytes,
               std::uint32_t             size,
               const char*               what) noexcept -> bool {
	if (context->CheckExpectedBytes(rva, bytes, size)) {
		return true;
	}

	char message[192]{};
	std::snprintf(message, sizeof(message),
	              "%s at RVA 0x%08llX does not match the expected bytes - wrong game build, or another plugin got there first.",
	              what, static_cast<unsigned long long>(rva));
	context->LogError(message);
	return false;
}

auto CheckSyncSites(const D2RL::PluginContext* context) noexcept -> bool {
	bool ok = true;
	ok = CheckSite(context, kSrvBoltCreateContextRva, kSrvBoltCreateContextBytes, ByteCount(kSrvBoltCreateContextBytes), "Server bolt create call") && ok;
	ok = CheckSite(context, kCreateMissileRva, kCreateMissileBytes, ByteCount(kCreateMissileBytes), "Server create missile") && ok;
	ok = CheckSite(context, kSendMissileRva, kSendMissileBytes, ByteCount(kSendMissileBytes), "Server create-missile packet sender") && ok;
	ok = CheckSite(context, kSendGateRva, kSendGateBytes, ByteCount(kSendGateBytes), "ClientSend / force-send gate in the sender") && ok;
	ok = CheckSite(context, kGetUnitRoomRva, kGetUnitRoomBytes, ByteCount(kGetUnitRoomBytes), "Unit room lookup") && ok;
	ok = CheckSite(context, kRoomClientsReadRva, kRoomClientsReadBytes, ByteCount(kRoomClientsReadBytes), "Room client list fields") && ok;
	ok = CheckSite(context, kGetMissileRecordRva, kGetMissileRecordBytes, ByteCount(kGetMissileRecordBytes), "missiles.txt record lookup") && ok;
	ok = CheckSite(context, kClassIdReadRva, kClassIdReadBytes, ByteCount(kClassIdReadBytes), "Unit class id field") && ok;
	ok = CheckSite(context, kUnitGuidReadRva, kUnitGuidReadBytes, ByteCount(kUnitGuidReadBytes), "Unit GUID field") && ok;
	ok = CheckSite(context, kDataContextReadRva, kDataContextReadBytes, ByteCount(kDataContextReadBytes), "Unit data context field") && ok;
	ok = CheckSite(context, kPathVelocityReadRva, kPathVelocityReadBytes, ByteCount(kPathVelocityReadBytes), "Path velocity field") && ok;
	ok = CheckSite(context, kSrvHitFuncReadRva, kSrvHitFuncReadBytes, ByteCount(kSrvHitFuncReadBytes), "missiles.txt pSrvHitFunc field") && ok;
	ok = CheckSite(context, kMissileCountReadRva, kMissileCountReadBytes, ByteCount(kMissileCountReadBytes), "missiles.txt row count field") && ok;
	ok = CheckSite(context, kCltMissilePacketRva, kCltMissilePacketBytes, ByteCount(kCltMissilePacketBytes), "Client create-missile packet handler") && ok;
	ok = CheckSite(context, kCltProcessServerMissilesRva, kCltProcessServerMissilesBytes, ByteCount(kCltProcessServerMissilesBytes), "Client server-missile pass") && ok;
	ok = CheckSite(context, kCltGetUnitRva, kCltGetUnitBytes, ByteCount(kCltGetUnitBytes), "Client unit lookup") && ok;
	ok = CheckSite(context, kCltCreateMissileRva, kCltCreateMissileBytes, ByteCount(kCltCreateMissileBytes), "Client create missile") && ok;
	return ok;
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
	              "lf bolt sync: %s. Server: %ld bolts sent (%ld packets), %ld bolts with no client in sight, %ld duplicate stock sends dropped.",
	              g_syncActive ? "ON" : "OFF",
	              static_cast<long>(g_syncBoltsSent),
	              static_cast<long>(g_syncPacketsSent),
	              static_cast<long>(g_syncNoViewer),
	              static_cast<long>(g_syncStockSkipped));
	command->plugin->WriteConsoleMessage(message);

	std::snprintf(message, sizeof(message),
	              "lf bolt sync client: %ld hits drew no local bolts, %ld server bolts received, %ld created, %ld failed.",
	              static_cast<long>(g_cltLocalBoltsSkipped),
	              static_cast<long>(g_cltBoltsReceived),
	              static_cast<long>(g_cltBoltsCreated),
	              static_cast<long>(g_cltBoltsFailed));
	command->plugin->WriteConsoleMessage(message);

	std::snprintf(message, sizeof(message),
	              "lf caster return: %s, server hook %s, client hook %s, redirects %ld/%ld (server/client), aurafilter_bit 0x%08X, bolt_count %d, require_caster_in_range %s.",
	              g_settings.enabled ? "enabled" : "DISABLED",
	              g_srvHooked ? "on" : "OFF",
	              g_cltHooked ? "on" : "OFF",
	              static_cast<long>(g_srvRedirects),
	              static_cast<long>(g_cltRedirects),
	              static_cast<unsigned int>(g_settings.filterBit),
	              static_cast<int>(g_settings.boltCount),
	              g_settings.requireCasterInRange ? "true" : "false");
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
	g_context = context;

	LoadSettings(context);

	const auto moduleBase = static_cast<std::uintptr_t>(context->exeBase);
	if (moduleBase == 0) {
		context->LogError("Could not resolve the main module base.");
		return false;
	}

	// The target searches carry both parts. Verify them before touching anything.
	if (!CheckSite(context, kSrvTargetIteratorRva, kSrvTargetIteratorBytes, ByteCount(kSrvTargetIteratorBytes), "Server target search")
		|| !CheckSite(context, kSrvBoltCallbackRva, kSrvBoltCallbackBytes, ByteCount(kSrvBoltCallbackBytes), "Server Lightning Fury bolt callback")
		|| !CheckSite(context, kCltTargetIteratorRva, kCltTargetIteratorBytes, ByteCount(kCltTargetIteratorBytes), "Client target search")
		|| !CheckSite(context, kCltBoltCallbackRva, kCltBoltCallbackBytes, ByteCount(kCltBoltCallbackBytes), "Client Lightning Fury bolt callback")) {
		return false;
	}

	const bool syncSitesOk = g_settings.syncBolts && CheckSyncSites(context);

	g_srvBoltCallback  = reinterpret_cast<BoltCallbackFn>(moduleBase + kSrvBoltCallbackRva);
	g_cltBoltCallback  = reinterpret_cast<BoltCallbackFn>(moduleBase + kCltBoltCallbackRva);
	g_createMissile    = reinterpret_cast<CreateMissileFn>(moduleBase + kCreateMissileRva);
	g_getUnitRoom      = reinterpret_cast<GetUnitRoomFn>(moduleBase + kGetUnitRoomRva);
	g_getMissileRecord = reinterpret_cast<GetMissileRecordFn>(moduleBase + kGetMissileRecordRva);
	g_getDataTables    = reinterpret_cast<GetDataTablesFn>(moduleBase + kGetDataTablesRva);
	g_cltGetUnit       = reinterpret_cast<CltGetUnitFn>(moduleBase + kCltGetUnitRva);
	g_cltCreateMissile = reinterpret_cast<CltCreateMissileFn>(moduleBase + kCltCreateMissileRva);

	if (!context->InstallInlineHook(kSrvTargetIteratorRva,
	                                kSrvTargetIteratorBytes,
	                                ByteCount(kSrvTargetIteratorBytes),
	                                &HookSrvTargetIterator,
	                                &g_originalSrvIterator)) {
		context->LogError("Failed to install the server target-search hook.");
		return false;
	}
	g_srvHooked = true;

	// From here on the plugin never returns false: a detour is live, and
	// returning false makes the loader unload this DLL with a hook still
	// pointing into it.

	// Bolt sync, server half. Installed before the client half: if the client
	// half fails, the server half is switched off again, so the plugin can
	// never leave a client skipping bolts that nobody sends.
	if (!g_settings.syncBolts) {
		context->LogInfo("sync_bolts is false: Lightning Fury bolts stay client-predicted (stock).");
	} else if (!syncSitesOk) {
		context->LogError("Bolt sync NOT installed: see the byte mismatches above. Caster return is unaffected.");
	} else if (!PrepareRelayPage(moduleBase)) {
		context->LogError("Bolt sync NOT installed: no relay page could be allocated within rel32 reach of the game image.");
	} else if (!RedirectBoltCreateCall(context, moduleBase)) {
		context->LogError("Bolt sync NOT installed: the bolt create call could not be redirected.");
	} else if (!context->InstallInlineHook(kSendMissileRva,
	                                       kSendMissileBytes,
	                                       ByteCount(kSendMissileBytes),
	                                       &HookSendMissile,
	                                       &g_originalSendMissile)) {
		context->LogError("Bolt sync NOT installed: the create-missile sender hook failed. The redirected create call passes through unchanged.");
	} else {
		g_syncServer = true;
	}

	// Bolt sync, client side of the server's bolts: capture their packets and
	// create them as client bolts.
	if (g_syncServer) {
		if (context->InstallInlineHook(kCltMissilePacketRva,
		                               kCltMissilePacketBytes,
		                               ByteCount(kCltMissilePacketBytes),
		                               &HookCltMissilePacket,
		                               &g_originalCltMissilePacket)) {
			g_cltPacketHooked = true;
		} else {
			context->LogError("Bolt sync NOT active: the client create-missile packet hook failed.");
		}
		if (g_cltPacketHooked
			&& context->InstallInlineHook(kCltProcessServerMissilesRva,
			                              kCltProcessServerMissilesBytes,
			                              ByteCount(kCltProcessServerMissilesBytes),
			                              &HookCltProcessServerMissiles,
			                              &g_originalCltProcess)) {
			g_cltProcessHooked = true;
		} else if (g_cltPacketHooked) {
			context->LogError("Bolt sync NOT active: the client server-missile pass hook failed.");
		}

		const D2RL::LifecycleService* lifecycle = nullptr;
		if (context->QueryService(&lifecycle) == D2RL::ServiceQueryResult::Success
			&& D2RL::HasLifecycleServiceField(lifecycle, D2RL::LifecycleServiceRequiredSize)) {
			const D2RL::Lifecycle::DataTablesLoadedListener listener{
				.structSize = D2RL::Lifecycle::DataTablesLoadedListenerSize,
				.flags      = 0,
				.callback   = OnDataTablesLoaded,
				.userData   = nullptr,
			};
			D2RL::Lifecycle::ListenerHandle handle = D2RL::Lifecycle::InvalidHandle;
			if (lifecycle->registerDataTablesLoadedListener(context, &listener, &handle) != D2RL::Lifecycle::Result::Success) {
				context->LogWarn("Could not register for data reloads: after a missiles.txt reload, restart the game before testing Lightning Fury.");
			}
		} else {
			context->LogWarn("Lifecycle service unavailable: after a missiles.txt reload, restart the game before testing Lightning Fury.");
		}
	}

	const bool needClientHook = g_settings.hookClient || g_syncServer;
	if (!needClientHook) {
		context->LogWarn("hook_client is false and bolt sync is off: the client hook is not installed.");
	} else if (context->InstallInlineHook(kCltTargetIteratorRva,
	                                      kCltTargetIteratorBytes,
	                                      ByteCount(kCltTargetIteratorBytes),
	                                      &HookCltTargetIterator,
	                                      &g_originalCltIterator)) {
		g_cltHooked = true;
	} else {
		context->LogError("Failed to install the client target-search hook. Bolt sync is switched off; caster return stays active on the server.");
	}

	g_syncActive = g_syncServer && g_cltPacketHooked && g_cltProcessHooked && g_cltHooked;

	if (!context->RegisterConsoleCommand("lf-caster-priority", StatusCommand, "Report Lightning Fury bolt sync and caster return state.")) {
		context->LogWarn("Console command was not registered.");
	}

	char summary[224]{};
	std::snprintf(summary, sizeof(summary),
	              "Lightning Fury plugin loaded: bolt sync %s, caster return %s (aurafilter bit 0x%08X, bolt count %d).",
	              g_syncActive ? "ON" : "OFF",
	              g_settings.enabled ? "enabled" : "disabled in config",
	              static_cast<unsigned int>(g_settings.filterBit),
	              static_cast<int>(g_settings.boltCount));
	context->LogInfo(summary);
	return true;
}
