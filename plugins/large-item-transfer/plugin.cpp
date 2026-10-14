// Large Item Transfer
// Plugin id: celestialrayone.large-item-transfer
//
// ---------------------------------------------------------------------------
// The bug
// ---------------------------------------------------------------------------
//   The server sends every item to a client in one item message. Both senders
//   give the item's data 244 bytes of room:
//
//     0x479CD0  SendItemAction9C  SerializeItem(item, buffer, 244, 0, 0, ...) + 8
//     0x479EA0  SendItemAction9D  SerializeItem(item, buffer, 244, 0, 0, ...) + 13
//
//   The length field of both messages is one byte, and the client's message
//   reader (0x644870) accepts a 0x9C message only up to 252 bytes and no
//   message above 512. When an item needs more room, the serializer reports
//   0 bytes and the message goes out with an empty item. The client cannot
//   read it:
//     - put into a container (0x9D action 4, handler 0x1C4300): the new item
//       is freed and then passed on as NULL, and the placement routine reads
//       address 0 (crash at 0x34E237);
//     - other handlers drop the item on the client while the server keeps it.
//   D2RCore's extended item format writes 16-bit stat ids and ESR's raised
//   Save Bits make a median stat cost about 30 bits, so an item reaches the
//   limit at roughly 55 stats.
//
// ---------------------------------------------------------------------------
// The fix
// ---------------------------------------------------------------------------
//   Server, only inside the two senders above (recognised by the return
//   address of their SerializeItem call):
//     - D2RCore!SerializeItem writes the item into a 16 KiB buffer. An item
//       that fits one message (244 bytes for 0x9C, 239 for 0x9D so the
//       sender's own 0xFC check stays quiet) is copied back and sent exactly
//       as before.
//     - A larger item leaves a 20-byte marker in the sender's buffer, and the
//       full item waits under a token.
//     - When the sender queues its message (0x4817F0), the full item is queued
//       first as 0x9D messages with the private action 0xC7, at most 255 bytes
//       each, directly followed by the item message itself.
//   Client:
//     - The SCMD 0x9D dispatcher (0x12E490) takes the 0xC7 messages and
//       rebuilds the item. The stock client logs and ignores unknown actions,
//       so no other code ever sees them.
//     - Every item handler reads items through D2RCore!ReadItemHeader and
//       D2RCore!DecodeItem (all 33 decode sites in the handlers 0x1C2B10 to
//       0x1C9410). Given a marker, they read the rebuilt item instead.
//   The pieces travel in the same ordered stream as the item message, in
//   local games and over TCP/IP. Nothing is clamped or dropped: the real item
//   reaches the client. The host and every TCP/IP client need this plugin.
//
// ---------------------------------------------------------------------------
// Messages (all numbers little-endian)
// ---------------------------------------------------------------------------
//   Piece: SCMD 0x9D, action 0xC7. 24-byte header, then up to 231 data bytes.
//     +0 9D  +1 C7  +2 length  +3 version 1  +4 "LITF"
//     +8 token  +12 item bytes  +16 item CRC-32  +20 offset of this piece
//   Marker: replaces the item data of the 0x9C/0x9D message, 20 bytes.
//     +0 4C 49 54 53 C7 9D 5A A5  +8 token  +12 item bytes  +16 item CRC-32
//   The client checks the CRC-32 of the rebuilt item before using it.
//
// ---------------------------------------------------------------------------
// Sites (D2R 3.3.93847 under D2RLoader 1.3.1, D2RCore.dll sha256 2a868d01...)
// ---------------------------------------------------------------------------
//   Game image, loader inline hooks:
//     0x4817F0  queue a server message to one client (client, bytes, size_t)
//               40 55 56 57 48 83 EC 40         push rbp/rsi/rdi; sub rsp,40h
//     0x12E490  client SCMD 0x9D dispatcher (message)
//               40 53 48 81 EC 60 01 00 00      push rbx; sub rsp,160h
//   Game image, checked only (fixes the two return addresses):
//     0x479D97  9C sender: mov r8d,0F4h ... call j_SerializeItem; lea esi,[rax+8]
//               the call returns to 0x479DBD
//     0x479F88  9D sender: mov r8d,0F4h ... call j_SerializeItem; lea esi,[rax+0Dh]
//               the call returns to 0x479FA2
//   D2RCore.dll exports, this plugin's own 5-byte detours (D2RLoader 1.3.1's
//   hook installer only accepts sites inside D2R.exe). Each entry is pushes
//   only and nothing in the function branches back into those five bytes:
//     SerializeItem   55 41 56 56 57   (item, buffer, capacity, a4, a5, gamble, sink) -> bytes
//     DecodeItem      55 41 57 41 56   (item, data, size, a4, consumed*, version, error*) -> result
//     ReadItemHeader  55 41 57 41 56   (a1, data, size, a4, header*, version) -> result
//   The game reaches the three exports through its j_ thunks, so every caller
//   passes the detours. D2RCore itself serializes items only inside
//   SerializeItem, so the two senders are the only network item writers.

#include <D2RLPlugin/api.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#if defined(_MSC_VER)
#define LIT_RETURN_ADDRESS() reinterpret_cast<std::uintptr_t>(_ReturnAddress())
#else
#define LIT_RETURN_ADDRESS() reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))
#endif

namespace CelestialRayOne::LargeItemTransfer {
namespace {

// ===== protocol begin (no Windows or SDK code, also built by the tests) =====

constexpr std::uint8_t ItemUnitCommand        = 0x9C;
constexpr std::uint8_t ItemUnitExCommand      = 0x9D;
constexpr std::size_t  ItemUnitHeaderBytes    = 8;
constexpr std::size_t  ItemUnitExHeaderBytes  = 13;
// Room one message gives an item. 0x9C: the sender's 244-byte buffer, which
// also keeps the total at the client reader's 0x9C cap of 252. 0x9D: 239, so
// the total stays at or below the sender's own 0xFC check.
constexpr std::size_t  ItemUnitPayloadLimit   = 244;
constexpr std::size_t  ItemUnitExPayloadLimit = 239;
constexpr std::size_t  MessageLimitBytes      = 255;
constexpr std::uint8_t PieceAction            = 0xC7;
constexpr std::uint8_t ProtocolVersion        = 1;
constexpr std::size_t  PieceHeaderBytes       = 24;
constexpr std::size_t  PieceDataBytes         = MessageLimitBytes - PieceHeaderBytes;
constexpr std::size_t  MarkerBytes            = 20;
constexpr std::size_t  MaximumItemBytes       = 16 * 1024;
constexpr std::array<std::uint8_t, 4> PieceMagic{0x4C, 0x49, 0x54, 0x46};
constexpr std::array<std::uint8_t, 8> MarkerMagic{0x4C, 0x49, 0x54, 0x53, 0xC7, 0x9D, 0x5A, 0xA5};

static_assert(PieceDataBytes == 231);
static_assert(MarkerBytes <= ItemUnitExPayloadLimit);
static_assert(ItemUnitExHeaderBytes + PieceHeaderBytes < MessageLimitBytes);

struct ItemRef {
    std::uint32_t token;
    std::uint32_t size;
    std::uint32_t crc;
};

auto SameItem(const ItemRef& left, const ItemRef& right) noexcept -> bool {
    return left.token == right.token && left.size == right.size && left.crc == right.crc;
}

void PutU32(std::uint8_t* out, std::uint32_t value) noexcept {
    out[0] = static_cast<std::uint8_t>(value);
    out[1] = static_cast<std::uint8_t>(value >> 8);
    out[2] = static_cast<std::uint8_t>(value >> 16);
    out[3] = static_cast<std::uint8_t>(value >> 24);
}

auto GetU32(const std::uint8_t* in) noexcept -> std::uint32_t {
    return static_cast<std::uint32_t>(in[0])
        | (static_cast<std::uint32_t>(in[1]) << 8)
        | (static_cast<std::uint32_t>(in[2]) << 16)
        | (static_cast<std::uint32_t>(in[3]) << 24);
}

auto Crc32(const std::uint8_t* data, std::size_t size) noexcept -> std::uint32_t {
    std::uint32_t crc = 0xFFFF'FFFFU;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB8'8320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

void WriteMarker(std::uint8_t* out, const ItemRef& item) noexcept {
    std::memcpy(out, MarkerMagic.data(), MarkerMagic.size());
    PutU32(out + 8, item.token);
    PutU32(out + 12, item.size);
    PutU32(out + 16, item.crc);
}

auto ReadMarker(const std::uint8_t* data, std::uint64_t size, ItemRef& item) noexcept -> bool {
    if (data == nullptr || size != MarkerBytes
            || std::memcmp(data, MarkerMagic.data(), MarkerMagic.size()) != 0) {
        return false;
    }
    item = {GetU32(data + 8), GetU32(data + 12), GetU32(data + 16)};
    return item.size > 0 && item.size <= MaximumItemBytes;
}

auto PieceCount(std::uint32_t itemBytes) noexcept -> std::uint32_t {
    return static_cast<std::uint32_t>((itemBytes + PieceDataBytes - 1) / PieceDataBytes);
}

// Writes the piece that starts at offset into out (MessageLimitBytes bytes of
// room) and returns the message length.
auto WritePiece(std::uint8_t* out, const ItemRef& item, const std::uint8_t* bytes,
        std::uint32_t offset) noexcept -> std::size_t {
    const std::size_t data   = std::min<std::size_t>(PieceDataBytes, item.size - offset);
    const std::size_t length = PieceHeaderBytes + data;
    out[0] = ItemUnitExCommand;
    out[1] = PieceAction;
    out[2] = static_cast<std::uint8_t>(length);
    out[3] = ProtocolVersion;
    std::memcpy(out + 4, PieceMagic.data(), PieceMagic.size());
    PutU32(out + 8, item.token);
    PutU32(out + 12, item.size);
    PutU32(out + 16, item.crc);
    PutU32(out + 20, offset);
    std::memcpy(out + PieceHeaderBytes, bytes + offset, data);
    return length;
}

struct Piece {
    ItemRef             item;
    std::uint32_t       offset;
    const std::uint8_t* data;
    std::size_t         dataBytes;
};

// The game's message reader has already made sure message[2] bytes are there.
auto ReadPiece(const std::uint8_t* message, Piece& piece) noexcept -> bool {
    if (message == nullptr || message[0] != ItemUnitExCommand || message[1] != PieceAction) {
        return false;
    }
    const std::size_t length = message[2];
    if (length <= PieceHeaderBytes || message[3] != ProtocolVersion
            || std::memcmp(message + 4, PieceMagic.data(), PieceMagic.size()) != 0) {
        return false;
    }
    piece.item      = {GetU32(message + 8), GetU32(message + 12), GetU32(message + 16)};
    piece.offset    = GetU32(message + 20);
    piece.data      = message + PieceHeaderBytes;
    piece.dataBytes = length - PieceHeaderBytes;
    if (piece.item.size == 0 || piece.item.size > MaximumItemBytes
            || piece.offset >= piece.item.size || piece.offset % PieceDataBytes != 0) {
        return false;
    }
    return piece.dataBytes
        == std::min<std::size_t>(PieceDataBytes, piece.item.size - piece.offset);
}

// Server side: full items waiting for their message to be queued.
class PendingItems {
public:
    auto Put(const ItemRef& item, const std::uint8_t* bytes) noexcept -> bool {
        std::lock_guard<std::mutex> guard(lock_);
        Slot* slot = nullptr;
        for (Slot& candidate : slots_) {
            if (!candidate.used) {
                slot = &candidate;
                break;
            }
            if (slot == nullptr || candidate.stamp < slot->stamp) slot = &candidate;
        }
        try {
            slot->bytes.assign(bytes, bytes + item.size);
        } catch (...) {
            slot->used = false;
            std::vector<std::uint8_t>().swap(slot->bytes);
            return false;
        }
        slot->item  = item;
        slot->used  = true;
        slot->stamp = ++clock_;
        return true;
    }

    auto Take(const ItemRef& item, std::vector<std::uint8_t>& out) noexcept -> bool {
        std::lock_guard<std::mutex> guard(lock_);
        for (Slot& slot : slots_) {
            if (slot.used && SameItem(slot.item, item)) {
                out.swap(slot.bytes);
                std::vector<std::uint8_t>().swap(slot.bytes);
                slot.used = false;
                return true;
            }
        }
        return false;
    }

private:
    struct Slot {
        ItemRef                   item{};
        bool                      used{};
        std::uint64_t             stamp{};
        std::vector<std::uint8_t> bytes;
    };
    static constexpr std::size_t SlotCount = 16;
    std::mutex                    lock_;
    std::array<Slot, SlotCount>   slots_{};
    std::uint64_t                 clock_ = 0;
};

enum class PieceResult : std::uint8_t {
    Partial,
    Completed,
    Duplicate,
    Corrupt,
    NoMemory,
};

// Client side: items being rebuilt, and rebuilt items kept until they age out.
class Reassembler {
public:
    auto Accept(const Piece& piece) noexcept -> PieceResult {
        std::lock_guard<std::mutex> guard(lock_);
        Slot* slot = Find(piece.item);
        if (slot == nullptr) {
            slot = Oldest();
            Reset(*slot);
            try {
                slot->bytes.assign(piece.item.size, 0);
                slot->received.assign(PieceCount(piece.item.size), 0);
            } catch (...) {
                Reset(*slot);
                return PieceResult::NoMemory;
            }
            slot->item    = piece.item;
            slot->missing = PieceCount(piece.item.size);
            slot->used    = true;
        }
        slot->stamp = ++clock_;
        if (slot->rebuilt) return PieceResult::Duplicate;

        const std::size_t index = piece.offset / PieceDataBytes;
        if (slot->received[index] != 0) return PieceResult::Duplicate;
        std::memcpy(slot->bytes.data() + piece.offset, piece.data, piece.dataBytes);
        slot->received[index] = 1;
        if (--slot->missing != 0) return PieceResult::Partial;

        if (Crc32(slot->bytes.data(), slot->bytes.size()) != slot->item.crc) {
            Reset(*slot);
            return PieceResult::Corrupt;
        }
        try {
            slot->rebuilt =
                std::make_shared<const std::vector<std::uint8_t>>(std::move(slot->bytes));
        } catch (...) {
            Reset(*slot);
            return PieceResult::NoMemory;
        }
        std::vector<std::uint8_t>().swap(slot->bytes);
        std::vector<std::uint8_t>().swap(slot->received);
        return PieceResult::Completed;
    }

    auto Rebuilt(const ItemRef& item) noexcept -> std::shared_ptr<const std::vector<std::uint8_t>> {
        std::lock_guard<std::mutex> guard(lock_);
        Slot* slot = Find(item);
        if (slot == nullptr || !slot->rebuilt) return {};
        slot->stamp = ++clock_;
        return slot->rebuilt;
    }

private:
    struct Slot {
        ItemRef                                            item{};
        bool                                               used{};
        std::uint64_t                                      stamp{};
        std::uint32_t                                      missing{};
        std::vector<std::uint8_t>                          bytes;
        std::vector<std::uint8_t>                          received;
        std::shared_ptr<const std::vector<std::uint8_t>>   rebuilt;
    };

    auto Find(const ItemRef& item) noexcept -> Slot* {
        for (Slot& slot : slots_) {
            if (slot.used && SameItem(slot.item, item)) return &slot;
        }
        return nullptr;
    }

    auto Oldest() noexcept -> Slot* {
        Slot* oldest = nullptr;
        for (Slot& slot : slots_) {
            if (!slot.used) return &slot;
            if (oldest == nullptr || slot.stamp < oldest->stamp) oldest = &slot;
        }
        return oldest;
    }

    static void Reset(Slot& slot) noexcept {
        slot.item    = {};
        slot.used    = false;
        slot.missing = 0;
        std::vector<std::uint8_t>().swap(slot.bytes);
        std::vector<std::uint8_t>().swap(slot.received);
        slot.rebuilt.reset();
    }

    static constexpr std::size_t SlotCount = 64;
    std::mutex                    lock_;
    std::array<Slot, SlotCount>   slots_{};
    std::uint64_t                 clock_ = 0;
};

// Detour helpers: plain byte writers, so the tests can check them.
constexpr std::size_t AbsoluteJumpBytes = 14;

void WriteAbsoluteJump(std::uint8_t* at, std::uint64_t target) noexcept {
    at[0] = 0xFF;
    at[1] = 0x25;
    at[2] = 0x00;
    at[3] = 0x00;
    at[4] = 0x00;
    at[5] = 0x00;
    for (int index = 0; index < 8; ++index) {
        at[6 + index] = static_cast<std::uint8_t>(target >> (8 * index));
    }
}

void WriteTrampoline(std::uint8_t* at, const std::uint8_t* stolen, std::size_t stolenBytes,
        std::uint64_t resume) noexcept {
    std::memcpy(at, stolen, stolenBytes);
    WriteAbsoluteJump(at + stolenBytes, resume);
}

auto WriteJumpRel32(std::uint8_t* at, std::uint64_t from, std::uint64_t to) noexcept -> bool {
    const std::int64_t delta = static_cast<std::int64_t>(to) - static_cast<std::int64_t>(from) - 5;
    if (delta < INT32_MIN || delta > INT32_MAX) return false;
    at[0] = 0xE9;
    PutU32(at + 1, static_cast<std::uint32_t>(static_cast<std::int32_t>(delta)));
    return true;
}

// ===== protocol end =====

// ---------------------------------------------------------------------------
// Sites
// ---------------------------------------------------------------------------

constexpr std::uint64_t QueueServerPacketRva = 0x4817F0;
constexpr std::array<std::uint8_t, 8> QueueServerPacketPrologue{
    0x40, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x40,
};

constexpr std::uint64_t ItemUnitExDispatchRva = 0x12E490;
constexpr std::array<std::uint8_t, 9> ItemUnitExDispatchPrologue{
    0x40, 0x53, 0x48, 0x81, 0xEC, 0x60, 0x01, 0x00, 0x00,
};

// 9C sender, from mov r8d,0F4h to the 0xFC check. The call to j_SerializeItem
// (0x143E2B6A0) returns to 0x479DBD.
constexpr std::uint64_t Send9CWindowRva = 0x479D97;
constexpr std::array<std::uint8_t, 47> Send9CWindow{
    0x41, 0xB8, 0xF4, 0x00, 0x00, 0x00, 0x48, 0x89, 0x44, 0x24, 0x30, 0x48,
    0x8B, 0xCB, 0x8B, 0x84, 0x24, 0xB0, 0x01, 0x00, 0x00, 0x89, 0x44, 0x24,
    0x28, 0xC7, 0x44, 0x24, 0x20, 0x00, 0x00, 0x00, 0x00, 0xE8, 0xE3, 0x18,
    0x9B, 0x03, 0x8D, 0x70, 0x08, 0x81, 0xFE, 0xFC, 0x00, 0x00, 0x00,
};
constexpr std::uint64_t Send9CReturnRva = 0x479DBD;

// 9D sender, same shape. The call returns to 0x479FA2.
constexpr std::uint64_t Send9DWindowRva = 0x479F88;
constexpr std::array<std::uint8_t, 35> Send9DWindow{
    0x41, 0xB8, 0xF4, 0x00, 0x00, 0x00, 0x89, 0x4C, 0x24, 0x28, 0x48, 0x8B,
    0xCF, 0xC7, 0x44, 0x24, 0x20, 0x00, 0x00, 0x00, 0x00, 0xE8, 0xFE, 0x16,
    0x9B, 0x03, 0x8D, 0x70, 0x0D, 0x81, 0xFE, 0xFC, 0x00, 0x00, 0x00,
};
constexpr std::uint64_t Send9DReturnRva = 0x479FA2;

static_assert(Send9CWindowRva + 38 == Send9CReturnRva);
static_assert(Send9DWindowRva + 26 == Send9DReturnRva);

constexpr const wchar_t* CoreModuleName = L"D2RCore.dll";
constexpr std::size_t    CoreEntryBytes = 5;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct Settings {
    bool enabled     = true;
    bool logEachItem = false;
};

const D2RL::PluginContext* Context = nullptr;
Settings                   Config{};
std::uintptr_t             Send9CReturn = 0;
std::uintptr_t             Send9DReturn = 0;
std::atomic<bool>          Armed{false};
std::atomic<std::uint32_t> NextToken{0};

PendingItems Pending;
Reassembler  Rebuilder;

std::atomic<std::uint64_t> ItemsSplit{0};
std::atomic<std::uint64_t> PiecesSent{0};
std::atomic<std::uint32_t> LargestItem{0};
std::atomic<std::uint64_t> PendingMissing{0};
std::atomic<std::uint64_t> PendingStoreFailures{0};
std::atomic<std::uint64_t> SerializerFailures{0};
std::atomic<std::uint64_t> PiecesReceived{0};
std::atomic<std::uint64_t> PiecesDuplicate{0};
std::atomic<std::uint64_t> PiecesRejected{0};
std::atomic<std::uint64_t> ItemsRebuilt{0};
std::atomic<std::uint64_t> RebuildCorrupt{0};
std::atomic<std::uint64_t> RebuildNoMemory{0};
std::atomic<std::uint64_t> MarkersResolved{0};
std::atomic<std::uint64_t> MarkersUnresolved{0};
std::atomic<bool>          LoggedFirstSplit{false};
std::atomic<bool>          LoggedPendingMissing{false};
std::atomic<bool>          LoggedCorrupt{false};
std::atomic<bool>          LoggedUnresolved{false};

using SerializeItemFn = std::int32_t (*)(void* item, std::uint8_t* buffer, std::uint64_t capacity,
    std::uint32_t argument4, std::uint32_t argument5, std::uint32_t gamble, void* sink) noexcept;
using DecodeItemFn = std::int32_t (*)(void* item, const std::uint8_t* data, std::uint64_t size,
    std::uint32_t argument4, void* consumed, std::uint32_t version, std::int32_t* error) noexcept;
using ReadItemHeaderFn = std::int32_t (*)(std::uint64_t argument1, const std::uint8_t* data,
    std::uint64_t size, std::uint32_t argument4, void* header, std::uint32_t version) noexcept;
using QueueServerPacketFn = std::uint64_t (*)(void* client, const std::uint8_t* bytes,
    std::uint64_t size) noexcept;
using ItemUnitExDispatchFn = std::uint64_t (*)(std::uint8_t* message, std::uint64_t argument2,
    std::uint64_t argument3, std::uint64_t argument4) noexcept;

void* OriginalSerializeItem      = nullptr;
void* OriginalDecodeItem         = nullptr;
void* OriginalReadItemHeader     = nullptr;
void* OriginalQueueServerPacket  = nullptr;
void* OriginalItemUnitExDispatch = nullptr;

auto NewToken() noexcept -> std::uint32_t {
    std::uint32_t token = NextToken.fetch_add(1, std::memory_order_relaxed);
    if (token == 0) token = NextToken.fetch_add(1, std::memory_order_relaxed);
    return token;
}

void RecordLargest(std::uint32_t bytes) noexcept {
    std::uint32_t seen = LargestItem.load(std::memory_order_relaxed);
    while (bytes > seen
            && !LargestItem.compare_exchange_weak(seen, bytes, std::memory_order_relaxed)) {
    }
}

// One 16 KiB buffer per server thread that sends items, made on first use.
auto ScratchBuffer() noexcept -> std::uint8_t* {
    thread_local std::unique_ptr<std::uint8_t[]> buffer;
    if (!buffer) buffer.reset(new (std::nothrow) std::uint8_t[MaximumItemBytes]);
    return buffer.get();
}

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------

// D2RCore!SerializeItem. The detour is entered by jumps only (game call,
// j_SerializeItem thunk, export entry, relay), so the return address is the
// sender's own call site.
auto HookSerializeItem(void* item, std::uint8_t* buffer, std::uint64_t capacity,
        std::uint32_t argument4, std::uint32_t argument5, std::uint32_t gamble,
        void* sink) noexcept -> std::int32_t {
    const auto original = reinterpret_cast<SerializeItemFn>(OriginalSerializeItem);
    const std::uintptr_t caller = LIT_RETURN_ADDRESS();

    std::size_t limit = 0;
    if (caller == Send9DReturn) {
        limit = ItemUnitExPayloadLimit;
    } else if (caller == Send9CReturn) {
        limit = ItemUnitPayloadLimit;
    }
    if (limit == 0 || buffer == nullptr || capacity < limit
            || !Armed.load(std::memory_order_acquire)) {
        return original(item, buffer, capacity, argument4, argument5, gamble, sink);
    }

    std::uint8_t* scratch = ScratchBuffer();
    if (scratch == nullptr) {
        return original(item, buffer, capacity, argument4, argument5, gamble, sink);
    }
    std::memset(scratch, 0, MaximumItemBytes);
    const std::int32_t written = original(item, scratch, MaximumItemBytes, argument4, argument5,
        gamble, sink);
    if (written <= 0) {
        // Not a size problem: D2RCore refused the item. Same result as before.
        SerializerFailures.fetch_add(1, std::memory_order_relaxed);
        return written;
    }

    const auto bytes = static_cast<std::uint32_t>(written);
    if (bytes <= limit) {
        std::memcpy(buffer, scratch, bytes);
        return written;
    }

    const ItemRef ref{NewToken(), bytes, Crc32(scratch, bytes)};
    if (!Pending.Put(ref, scratch)) {
        PendingStoreFailures.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    WriteMarker(buffer, ref);
    ItemsSplit.fetch_add(1, std::memory_order_relaxed);
    RecordLargest(bytes);

    if (Config.logEachItem || !LoggedFirstSplit.exchange(true, std::memory_order_relaxed)) {
        D2RL::LogInfoF(Context,
            "LargeItemTransfer: item of %u bytes sent in %u pieces with its 0x%02X message "
            "(token %08X).", bytes, PieceCount(bytes),
            caller == Send9DReturn ? ItemUnitExCommand : ItemUnitCommand, ref.token);
    }
    return static_cast<std::int32_t>(MarkerBytes);
}

// Queues the full item in pieces. Called right before its 0x9C/0x9D message.
void QueuePieces(QueueServerPacketFn original, void* client, const ItemRef& ref) noexcept {
    std::vector<std::uint8_t> item;
    if (!Pending.Take(ref, item) || item.size() != ref.size) {
        PendingMissing.fetch_add(1, std::memory_order_relaxed);
        if (!LoggedPendingMissing.exchange(true, std::memory_order_relaxed)) {
            D2RL::LogErrorF(Context,
                "LargeItemTransfer: a marked item message (token %08X) was queued without "
                "its item data. The client will receive an empty item.", ref.token);
        }
        return;
    }
    std::array<std::uint8_t, MessageLimitBytes> piece{};
    for (std::uint32_t offset = 0; offset < ref.size;
            offset += static_cast<std::uint32_t>(PieceDataBytes)) {
        const std::size_t length = WritePiece(piece.data(), ref, item.data(), offset);
        original(client, piece.data(), length);
        PiecesSent.fetch_add(1, std::memory_order_relaxed);
    }
}

// 0x4817F0: queue one server message for one client.
auto HookQueueServerPacket(void* client, const std::uint8_t* bytes, std::uint64_t size) noexcept
        -> std::uint64_t {
    const auto original = reinterpret_cast<QueueServerPacketFn>(OriginalQueueServerPacket);
    if (bytes != nullptr && Armed.load(std::memory_order_acquire)) {
        std::size_t header = 0;
        if (size == ItemUnitExHeaderBytes + MarkerBytes && bytes[0] == ItemUnitExCommand) {
            header = ItemUnitExHeaderBytes;
        } else if (size == ItemUnitHeaderBytes + MarkerBytes && bytes[0] == ItemUnitCommand) {
            header = ItemUnitHeaderBytes;
        }
        ItemRef ref{};
        if (header != 0 && ReadMarker(bytes + header, MarkerBytes, ref)) {
            QueuePieces(original, client, ref);
        }
    }
    return original(client, bytes, size);
}

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

// 0x12E490: SCMD 0x9D dispatcher. Takes this plugin's pieces, passes the rest.
auto HookItemUnitExDispatch(std::uint8_t* message, std::uint64_t argument2,
        std::uint64_t argument3, std::uint64_t argument4) noexcept -> std::uint64_t {
    if (message != nullptr && message[0] == ItemUnitExCommand && message[1] == PieceAction) {
        Piece piece{};
        if (!ReadPiece(message, piece)) {
            PiecesRejected.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        PiecesReceived.fetch_add(1, std::memory_order_relaxed);
        switch (Rebuilder.Accept(piece)) {
        case PieceResult::Completed:
            ItemsRebuilt.fetch_add(1, std::memory_order_relaxed);
            break;
        case PieceResult::Duplicate:
            PiecesDuplicate.fetch_add(1, std::memory_order_relaxed);
            break;
        case PieceResult::Corrupt:
            RebuildCorrupt.fetch_add(1, std::memory_order_relaxed);
            if (!LoggedCorrupt.exchange(true, std::memory_order_relaxed)) {
                D2RL::LogErrorF(Context,
                    "LargeItemTransfer: rebuilt item (token %08X, %u bytes) failed its CRC "
                    "check and was discarded.", piece.item.token, piece.item.size);
            }
            break;
        case PieceResult::NoMemory:
            RebuildNoMemory.fetch_add(1, std::memory_order_relaxed);
            break;
        case PieceResult::Partial:
            break;
        }
        return 0;
    }
    return reinterpret_cast<ItemUnitExDispatchFn>(OriginalItemUnitExDispatch)(message, argument2,
        argument3, argument4);
}

auto ResolveMarker(const std::uint8_t* data, std::uint64_t size, bool count) noexcept
        -> std::shared_ptr<const std::vector<std::uint8_t>> {
    ItemRef ref{};
    if (!ReadMarker(data, size, ref)) return {};
    auto rebuilt = Rebuilder.Rebuilt(ref);
    if (!count) return rebuilt;
    if (rebuilt) {
        MarkersResolved.fetch_add(1, std::memory_order_relaxed);
    } else {
        MarkersUnresolved.fetch_add(1, std::memory_order_relaxed);
        if (!LoggedUnresolved.exchange(true, std::memory_order_relaxed)) {
            D2RL::LogErrorF(Context,
                "LargeItemTransfer: an item message referred to item data (token %08X, %u "
                "bytes) that never arrived. Is this plugin enabled on the host?", ref.token,
                ref.size);
        }
    }
    return rebuilt;
}

// D2RCore!ReadItemHeader.
auto HookReadItemHeader(std::uint64_t argument1, const std::uint8_t* data, std::uint64_t size,
        std::uint32_t argument4, void* header, std::uint32_t version) noexcept -> std::int32_t {
    const auto original = reinterpret_cast<ReadItemHeaderFn>(OriginalReadItemHeader);
    if (size == MarkerBytes) {
        if (const auto rebuilt = ResolveMarker(data, size, false)) {
            return original(argument1, rebuilt->data(), rebuilt->size(), argument4, header,
                version);
        }
    }
    return original(argument1, data, size, argument4, header, version);
}

// D2RCore!DecodeItem.
auto HookDecodeItem(void* item, const std::uint8_t* data, std::uint64_t size,
        std::uint32_t argument4, void* consumed, std::uint32_t version,
        std::int32_t* error) noexcept -> std::int32_t {
    const auto original = reinterpret_cast<DecodeItemFn>(OriginalDecodeItem);
    if (size == MarkerBytes) {
        if (const auto rebuilt = ResolveMarker(data, size, true)) {
            return original(item, rebuilt->data(), rebuilt->size(), argument4, consumed, version,
                error);
        }
    }
    return original(item, data, size, argument4, consumed, version, error);
}

// ---------------------------------------------------------------------------
// D2RCore detours
// ---------------------------------------------------------------------------

struct CoreHook {
    const char*                               exportName;
    void*                                     target;
    void**                                    original;
    std::array<std::uint8_t, CoreEntryBytes>  expected;  // D2RCore.dll 1.3.1
    std::uint8_t*                             entry;
    std::array<std::uint8_t, CoreEntryBytes>  written;
    bool                                      installed;
};

std::array<CoreHook, 3> CoreHooks{{
    {"SerializeItem", reinterpret_cast<void*>(&HookSerializeItem), &OriginalSerializeItem,
        {0x55, 0x41, 0x56, 0x56, 0x57}, nullptr, {}, false},
    {"DecodeItem", reinterpret_cast<void*>(&HookDecodeItem), &OriginalDecodeItem,
        {0x55, 0x41, 0x57, 0x41, 0x56}, nullptr, {}, false},
    {"ReadItemHeader", reinterpret_cast<void*>(&HookReadItemHeader), &OriginalReadItemHeader,
        {0x55, 0x41, 0x57, 0x41, 0x56}, nullptr, {}, false},
}};

constexpr std::size_t HookPageBytes    = 4'096;
constexpr std::size_t RelaySlotBytes   = 16;
constexpr std::size_t TrampolineOffset = 0x100;
constexpr std::size_t TrampolineBytes  = 32;
static_assert(CoreHooks.size() * RelaySlotBytes <= TrampolineOffset);
static_assert(TrampolineOffset + CoreHooks.size() * TrampolineBytes <= HookPageBytes);
static_assert(AbsoluteJumpBytes <= RelaySlotBytes);
static_assert(CoreEntryBytes + AbsoluteJumpBytes <= TrampolineBytes);

std::uint8_t* HookPage = nullptr;

auto ModuleImageSize(HMODULE module) noexcept -> std::size_t {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        reinterpret_cast<const std::uint8_t*>(module) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE
            || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return 0;
    }
    return nt->OptionalHeader.SizeOfImage;
}

auto IsExecutable(std::uintptr_t address) noexcept -> bool {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) == 0) return false;
    const DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE
        | PAGE_EXECUTE_WRITECOPY;
    return info.State == MEM_COMMIT && (info.Protect & executable) != 0;
}

// A page no further than a rel32 from every byte of D2RCore.dll.
auto AllocateNearCore(std::uintptr_t coreBase, std::size_t coreSize) noexcept -> std::uint8_t* {
    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    const auto granularity = static_cast<std::uintptr_t>(systemInfo.dwAllocationGranularity);
    for (std::uintptr_t candidate = (coreBase + coreSize + granularity - 1) & ~(granularity - 1);
            candidate + HookPageBytes - coreBase < 0x7FFF'0000ULL;
            candidate += granularity) {
        if (void* page = VirtualAlloc(reinterpret_cast<void*>(candidate), HookPageBytes,
                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) {
            return static_cast<std::uint8_t*>(page);
        }
    }
    return nullptr;
}

auto WriteCode(std::uint8_t* at, const std::uint8_t* bytes, std::size_t size) noexcept -> bool {
    DWORD previous = 0;
    if (!VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &previous)) return false;
    std::memcpy(at, bytes, size);
    DWORD ignored = 0;
    VirtualProtect(at, size, previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), at, size);
    return true;
}

// Puts back every export entry that still holds this plugin's jump. The page
// stays allocated: a thread may still be inside a trampoline.
void UninstallCoreHooks() noexcept {
    for (CoreHook& hook : CoreHooks) {
        if (!hook.installed || hook.entry == nullptr) continue;
        if (std::memcmp(hook.entry, hook.written.data(), CoreEntryBytes) == 0
                && WriteCode(hook.entry, hook.expected.data(), CoreEntryBytes)) {
            hook.installed = false;
        }
    }
}

auto InstalledCoreHookCount() noexcept -> std::size_t {
    std::size_t count = 0;
    for (const CoreHook& hook : CoreHooks) count += hook.installed ? 1 : 0;
    return count;
}

auto InstallCoreHooks() noexcept -> bool {
    const HMODULE core = GetModuleHandleW(CoreModuleName);
    if (core == nullptr) {
        D2RL::LogError(Context, "LargeItemTransfer: D2RCore.dll is not loaded.");
        return false;
    }
    const std::size_t coreSize = ModuleImageSize(core);
    if (coreSize == 0) {
        D2RL::LogError(Context, "LargeItemTransfer: D2RCore.dll has no readable PE header.");
        return false;
    }
    const auto coreBase = reinterpret_cast<std::uintptr_t>(core);

    // Resolve and check every export before anything is written.
    for (CoreHook& hook : CoreHooks) {
        const FARPROC address = GetProcAddress(core, hook.exportName);
        if (address == nullptr) {
            D2RL::LogErrorF(Context,
                "LargeItemTransfer: D2RCore.dll has no export named %s. This plugin was built "
                "against D2RLoader 1.3.1.", hook.exportName);
            return false;
        }
        const auto target = reinterpret_cast<std::uintptr_t>(address);
        if (target < coreBase || target + CoreEntryBytes > coreBase + coreSize
                || !IsExecutable(target)) {
            D2RL::LogErrorF(Context,
                "LargeItemTransfer: export %s at 0x%llX is not executable code inside "
                "D2RCore.dll.", hook.exportName, static_cast<unsigned long long>(target));
            return false;
        }
        hook.entry = reinterpret_cast<std::uint8_t*>(target);
        if (std::memcmp(hook.entry, hook.expected.data(), CoreEntryBytes) != 0) {
            D2RL::LogErrorF(Context,
                "LargeItemTransfer: D2RCore!%s does not start with the instructions of the "
                "D2RLoader 1.3.1 build this plugin was verified against, or another plugin "
                "already hooked it. Nothing was changed.", hook.exportName);
            return false;
        }
    }

    HookPage = AllocateNearCore(coreBase, coreSize);
    if (HookPage == nullptr) {
        D2RL::LogError(Context, "LargeItemTransfer: no page was free within reach of D2RCore.dll.");
        return false;
    }
    std::memset(HookPage, 0xCC, HookPageBytes);
    for (std::size_t index = 0; index < CoreHooks.size(); ++index) {
        CoreHook& hook = CoreHooks[index];
        std::uint8_t* trampoline = HookPage + TrampolineOffset + index * TrampolineBytes;
        WriteAbsoluteJump(HookPage + index * RelaySlotBytes,
            reinterpret_cast<std::uintptr_t>(hook.target));
        WriteTrampoline(trampoline, hook.expected.data(), CoreEntryBytes,
            reinterpret_cast<std::uintptr_t>(hook.entry) + CoreEntryBytes);
        *hook.original = trampoline;
    }
    DWORD previous = 0;
    if (!VirtualProtect(HookPage, HookPageBytes, PAGE_EXECUTE_READ, &previous)) {
        D2RL::LogError(Context, "LargeItemTransfer: the hook page could not be made executable.");
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), HookPage, HookPageBytes);

    for (std::size_t index = 0; index < CoreHooks.size(); ++index) {
        CoreHook& hook = CoreHooks[index];
        const auto relay = reinterpret_cast<std::uintptr_t>(HookPage + index * RelaySlotBytes);
        if (!WriteJumpRel32(hook.written.data(), reinterpret_cast<std::uintptr_t>(hook.entry),
                relay)) {
            UninstallCoreHooks();
            D2RL::LogErrorF(Context, "LargeItemTransfer: the relay for %s is out of reach.",
                hook.exportName);
            return false;
        }
        if (!WriteCode(hook.entry, hook.written.data(), CoreEntryBytes)) {
            UninstallCoreHooks();
            D2RL::LogErrorF(Context, "LargeItemTransfer: D2RCore!%s could not be written.",
                hook.exportName);
            return false;
        }
        hook.installed = true;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Game hooks
// ---------------------------------------------------------------------------

std::size_t InstalledGameHooks = 0;

auto CheckSenders() noexcept -> bool {
    if (!Context->CheckExpectedBytes(Send9CWindowRva, Send9CWindow.data(),
            static_cast<std::uint32_t>(Send9CWindow.size()))) {
        D2RL::LogError(Context,
            "LargeItemTransfer: the 0x9C item sender (0x479D97) does not match D2R 3.3.93847. "
            "This build is not supported; nothing was patched.");
        return false;
    }
    if (!Context->CheckExpectedBytes(Send9DWindowRva, Send9DWindow.data(),
            static_cast<std::uint32_t>(Send9DWindow.size()))) {
        D2RL::LogError(Context,
            "LargeItemTransfer: the 0x9D item sender (0x479F88) does not match D2R 3.3.93847. "
            "This build is not supported; nothing was patched.");
        return false;
    }
    return true;
}

auto InstallGameHooks() noexcept -> bool {
    if (!Context->InstallInlineHook(QueueServerPacketRva, QueueServerPacketPrologue.data(),
            static_cast<std::uint32_t>(QueueServerPacketPrologue.size()),
            reinterpret_cast<void*>(&HookQueueServerPacket), &OriginalQueueServerPacket)
            || OriginalQueueServerPacket == nullptr) {
        D2RL::LogError(Context,
            "LargeItemTransfer: the server message queue (0x4817F0) could not be hooked. Its "
            "first bytes differ from D2R 3.3.93847, or another plugin hooks it.");
        return false;
    }
    ++InstalledGameHooks;
    if (!Context->InstallInlineHook(ItemUnitExDispatchRva, ItemUnitExDispatchPrologue.data(),
            static_cast<std::uint32_t>(ItemUnitExDispatchPrologue.size()),
            reinterpret_cast<void*>(&HookItemUnitExDispatch), &OriginalItemUnitExDispatch)
            || OriginalItemUnitExDispatch == nullptr) {
        D2RL::LogError(Context,
            "LargeItemTransfer: the client 0x9D dispatcher (0x12E490) could not be hooked. Its "
            "first bytes differ from D2R 3.3.93847, or another plugin hooks it.");
        return false;
    }
    ++InstalledGameHooks;
    return true;
}

void SeedTokens() noexcept {
    const std::uint64_t mixed = GetTickCount64() * 0x9E37'79B9'7F4A'7C15ULL
        ^ (static_cast<std::uint64_t>(GetCurrentProcessId()) << 20);
    NextToken.store(static_cast<std::uint32_t>(mixed >> 32) | 1U, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Config (small hand-rolled TOML subset, no external dependency)
// ---------------------------------------------------------------------------

constexpr const char* DefaultConfigToml = R"(# Large Item Transfer  (celestialrayone.large-item-transfer)
#
# What it does
#   The game sends each item to a player in one message with room for about
#   240 bytes of item data. An item with many stats, or with stats that have
#   large Save Bits in itemstatcost.txt, can need more than that. Without this
#   plugin such an item arrives empty: the game crashes when it is put into the
#   cube, stash or inventory (a cube recipe output, for example), and in other
#   cases the player cannot see the item while the server still has it.
#   With this plugin, an item too large for one message is sent in several
#   pieces right before its message and rebuilt on the player's side. Items
#   that fit are sent exactly as before.
#
# Who needs it
#   Single player: this install. TCP/IP: the host and every player who joins,
#   all with this plugin enabled.
#
# Limits
#   Up to 16 KiB of item data per item, far above any real item.
#   Not changed by this plugin: the server's own internal copies of an item
#   (for example when an area unloads with the item on the ground) hold
#   1024 bytes.
#
# Checking that it works
#   Console command: largeitems
#   Host side: items split, pieces sent, largest item. Player side: pieces
#   received, items rebuilt, markers resolved. Any problem is written to this
#   plugin's log, together with the first split item of each session.

# true: on.  false: the game's own behaviour (oversized items arrive empty).
enabled = true

# true: one log line for every item that needed splitting, with its size.
log_each_item = false
)";

constexpr std::size_t MaximumConfigBytes = 16 * 1024;

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

auto ParseBool(std::string_view value, bool& out) noexcept -> bool {
    if (value == "true" || value == "1") {
        out = true;
        return true;
    }
    if (value == "false" || value == "0") {
        out = false;
        return true;
    }
    return false;
}

void ApplyConfigLine(std::string_view key, std::string_view value) noexcept {
    if (key == "enabled") {
        if (!ParseBool(value, Config.enabled)) {
            Config.enabled = true;
            D2RL::LogWarn(Context, "LargeItemTransfer: enabled is not true or false; keeping true.");
        }
    } else if (key == "log_each_item") {
        if (!ParseBool(value, Config.logEachItem)) {
            Config.logEachItem = false;
            D2RL::LogWarn(Context,
                "LargeItemTransfer: log_each_item is not true or false; keeping false.");
        }
    }
}

void ParseConfig(std::string_view text) noexcept {
    std::size_t cursor = 0;
    while (cursor <= text.size()) {
        const std::size_t breakAt = text.find('\n', cursor);
        const std::size_t end = breakAt == std::string_view::npos ? text.size() : breakAt;
        std::string_view line = Trim(text.substr(cursor, end - cursor));
        cursor = end + 1;

        const std::size_t comment = line.find('#');
        if (comment != std::string_view::npos) line = Trim(line.substr(0, comment));
        if (!line.empty() && line.front() != '[') {
            const std::size_t equals = line.find('=');
            if (equals != std::string_view::npos) {
                ApplyConfigLine(Trim(line.substr(0, equals)), Trim(line.substr(equals + 1)));
            }
        }
        if (breakAt == std::string_view::npos) break;
    }
}

void ReadConfiguration() noexcept {
    if (!Context->EnsureConfig(DefaultConfigToml)) {
        D2RL::LogWarn(Context, "LargeItemTransfer: config file could not be created; using defaults.");
        return;
    }
    std::string buffer;
    try {
        buffer.assign(MaximumConfigBytes, '\0');
    } catch (...) {
        D2RL::LogWarn(Context, "LargeItemTransfer: no memory to read the config; using defaults.");
        return;
    }
    std::uint32_t required = 0;
    if (!Context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size()),
            &required)) {
        D2RL::LogWarn(Context, "LargeItemTransfer: config file could not be read; using defaults.");
        return;
    }
    buffer.resize(std::strlen(buffer.c_str()));
    ParseConfig(buffer);
}

// ---------------------------------------------------------------------------
// Console command
// ---------------------------------------------------------------------------

auto StateName() noexcept -> const char* {
    if (!Config.enabled) return "disabled by config";
    return Armed.load(std::memory_order_relaxed) ? "armed" : "not installed, see log";
}

auto Count(const std::atomic<std::uint64_t>& value) noexcept -> unsigned long long {
    return static_cast<unsigned long long>(value.load(std::memory_order_relaxed));
}

auto __cdecl StatusCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command,
        void*) noexcept -> D2RL::ConsoleCommandResult {
    if (command == nullptr || command->plugin == nullptr) {
        return D2RL::ConsoleCommandResult::Failed;
    }
    char message[768];
    std::snprintf(message, sizeof(message),
        "Large Item Transfer: %s | hooks %u/5 | host: items split %llu, pieces sent %llu, "
        "largest item %u bytes, unsent %llu, store failures %llu, serializer refusals %llu | "
        "player: pieces received %llu, items rebuilt %llu, markers resolved %llu, markers "
        "unresolved %llu, duplicates %llu, rejected %llu, CRC failures %llu, no memory %llu",
        StateName(), static_cast<unsigned>(InstalledCoreHookCount() + InstalledGameHooks),
        Count(ItemsSplit), Count(PiecesSent), LargestItem.load(std::memory_order_relaxed),
        Count(PendingMissing), Count(PendingStoreFailures), Count(SerializerFailures),
        Count(PiecesReceived), Count(ItemsRebuilt), Count(MarkersResolved),
        Count(MarkersUnresolved), Count(PiecesDuplicate), Count(PiecesRejected),
        Count(RebuildCorrupt), Count(RebuildNoMemory));
    command->plugin->WriteConsoleMessage(message);
    return D2RL::ConsoleCommandResult::Handled;
}

void RegisterStatusCommand() noexcept {
    if (!Context->RegisterConsoleCommand("largeitems", &StatusCommand,
            "Reports Large Item Transfer state and its host and player counters.")) {
        D2RL::LogWarn(Context, "LargeItemTransfer: the largeitems console command was refused.");
    }
}

constexpr D2RL::PluginInfo Info{
    .infoSize    = D2RL::PluginInfoSize,
    .abiVersion  = D2RL_PLUGIN_ABI_VERSION,
    .id          = "celestialrayone.large-item-transfer",
    .name        = "Large Item Transfer",
    .version     = "1.0.0",
    .author      = "CelestialRayOne",
    .description = "Sends items too large for one item message in pieces, so they reach the "
                   "client whole instead of empty.",
    .flags       = D2RL::PluginFlags::Shared | D2RL::PluginFlags::NativeHooks,
};

}  // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
    return &Info;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept
        -> bool {
    if (!D2RL::HasContext(context)) return false;
    Context = context;

    ReadConfiguration();
    RegisterStatusCommand();
    if (!Config.enabled) {
        D2RL::LogInfo(Context, "LargeItemTransfer: disabled by configuration.");
        return true;
    }

    const std::uintptr_t gameBase = context->exeBase != 0
        ? context->exeBase
        : reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (gameBase == 0) {
        D2RL::LogError(Context, "LargeItemTransfer: the game image base is unknown.");
        return false;
    }
    if (!CheckSenders()) return false;
    Send9CReturn = gameBase + Send9CReturnRva;
    Send9DReturn = gameBase + Send9DReturnRva;
    SeedTokens();

    if (!InstallCoreHooks()) return false;
    if (!InstallGameHooks()) {
        // D2RLoader removes this plugin's game hooks when the load fails.
        UninstallCoreHooks();
        return false;
    }
    Armed.store(true, std::memory_order_release);

    D2RL::LogInfoF(Context,
        "LargeItemTransfer: armed. %u D2RCore detours (SerializeItem, DecodeItem, "
        "ReadItemHeader) and %u game hooks (0x4817F0, 0x12E490). Items above %u bytes (0x9C) "
        "or %u bytes (0x9D) are sent in pieces of up to %u bytes.",
        static_cast<unsigned>(InstalledCoreHookCount()), static_cast<unsigned>(InstalledGameHooks),
        static_cast<unsigned>(ItemUnitPayloadLimit), static_cast<unsigned>(ItemUnitExPayloadLimit),
        static_cast<unsigned>(PieceDataBytes));
    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    Armed.store(false, std::memory_order_release);
    UninstallCoreHooks();
}

}  // namespace CelestialRayOne::LargeItemTransfer
