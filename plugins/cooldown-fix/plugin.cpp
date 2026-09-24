// ---------------------------------------------------------------------------
// local-cooldowns
//
// Makes D2R's skills.txt `localdelay` behave as a TRUE per-skill cooldown.
//
// WHAT IS ACTUALLY WRONG  (measured in game, not deduced)
// -------------------------------------------------------
// The SERVER is correct. Its state-185 statlist carries flags 0x8002 and the
// expire callback sub_14043AC00, its expire tracks the true minimum, and the
// sweep fires per skill:
//
//     list @1B63E6D80 flags=8002 cb=server expire=3770.0
//          stat=359 layer=683 value=3770
//          stat=359 layer=693 value=3865
//     SWEEP now=3770 entries=2 -> prunes 683, re-arms to 3865
//     SWEEP now=3865 entries=1 -> prunes 693
//
// The CLIENT's state-185 statlist is a different object on a different unit:
//
//     list @1B63DC430 flags=0 cb=NONE expire=0.0
//          stat=359 layer=683 value=150760
//          stat=359 layer=693 value=154560
//
// flags 0, no expire callback, expire 0.0. The statlist tick only visits a
// sublist when flag 0x8000 (callback path) or flag 0x2 (plain timer) is set,
// so that list is NEVER ticked, NEVER swept and NEVER pruned.
//
// The cooldown setter cannot have built it: its create path allocates with
// flag 0x8000 and installs the callback. The state-sync path created it
// first, holding the SERVER's frame value 3770 for layer 683 while the client
// clock was at ~150760 ms. From then on the state lookup hands the client's
// cooldown code that list, the setter always takes its "already exists"
// branch and never installs the callback or the timer flag.
//
// Consequence, matching both reported symptoms exactly: nothing on the client
// releases a skill at its own expiry. The client is released in one shot when
// the SERVER's list finally empties and clears state 185, which is when the
// LAST cooldown expires.
//
// D2RLOADER 1.3.0  (re-derived against the D2RLoader.exe image and D2RCore.dll)
// ---------------------------------------------------------------------------
// 1.3.0 moved the cooldown storage into D2RCore, but the defect is intact:
//
//   - 0x339E60, the setter, is now a loader thunk:
//         FF 25 disp32 / 90       jmp [SetWideSkillCooldown import] / nop
//     D2RCore!SetWideSkillCooldown (unit, expiry, skillId, callback) still
//     creates a missing list with GAME 0x2F7300(0x8000, expiry, type, id) and
//     [list+88h] = callback, and on an existing list only records the minimum,
//     sets state 185 and writes stat 359 at layer skillId. It never adopts.
//     It reads skillId as the full 32-bit r8d.
//
//   - D2RCore!ReceiveWideStateStatPacket (packet 0xA8) builds a missing state
//     list with GAME 0x2F7300(0, 0, type, id) and attaches it with no
//     callback: the same callback-less client list as before.
//
//   - D2RCore!ExpireWideUnitStats (behind the tick thunk 0x2F82F0) still skips
//     a sublist unless [list+1Ch] & 0x8002, subtracts from the float at
//     [list+24h], and for flag 0x8000 calls [list+88h] with ([list], state,
//     list) once it reaches 0.
//
//   - D2RCore!ExpireWideSkillCooldowns (behind the sweep thunk 0x33EA20)
//     still drops every stat 359 entry whose expiry is at or below the clock,
//     returns the next expiry, and clears state 185 when nothing is left.
//     The client callback 0x217FC0, which calls the sweep and re-arms the
//     list, and the cooldown gate 0x3404A0, which reads stat 359 from the
//     same list, are still native.
//
// So the same adoption fixes it. What changed is where it attaches: the
// setter's entry is loader-owned code, so this plugin no longer hooks it.
// Both native callers are retargeted instead, and their call reaches the
// loader's setter exactly as before:
//
//   client  sub_140217B90   217BF7  E8 -> 339E60, callback 217FC0 in r9
//   server  sub_140439500   439565  E8 -> 339E60, callback 43AC00 in r9
//
// Only the rel32 of each five-byte call changes, to a stub that jumps into
// HookedSetLocalCooldown, which has the setter's own ABI. These two calls are
// the only references to 0x339E60 in the image besides its .pdata entry.
//
// D2RLOADER 1.3.1  (re-derived against the 1.3.1 D2RLoader.exe dump and D2RCore.dll)
// ---------------------------------------------------------------------------
// The defect is unchanged:
//
//   - SetWideSkillCooldown now runs D2RCore 0x3CF850. On an existing state-185
//     list it still only records the minimum, sets state 185 and writes stat
//     359 at layer skillId. Only the create path allocates with 0x8000 and
//     stores [list+88h].
//
//   - ExpireWideUnitStats now runs D2RCore 0x3DAE20. It still skips a sublist
//     unless [list+1Ch] & 0x8002, counts down the float at [list+24h] and
//     calls [list+88h] when it reaches 0.
//
// The game image is unchanged too: both call windows below and the setter
// thunk are byte-identical to 1.3.0.
//
// What changed is the patch API. D2RCore's rel32 patch routine (0x47C760) now
// refuses a call or jump whose TARGET lies outside D2R.exe:
//
//     targetRva >= SizeOfImage  ->  "patch range is outside D2R.exe"
//
// SDK 0.3.0 documents the same rule: the target is another RVA in D2R.exe.
// 1.1.0 aimed both calls at a relay page allocated next to the image, so the
// loader refused the client call at 0x217BF7 and the plugin did not load.
//
// 1.2.0 keeps the exact same hook and moves only the relay into the image:
// 14 of the 15 int3 padding bytes after sub_140217B90's ret, at 0x217C31.
// Nothing executes or references that padding; its only reference is the
// .pdata end address of sub_140217B90. The stub is
//
//     217C31  FF 25 00 00 00 00   jmp qword ptr [rip+0]
//     217C37  <8-byte address of HookedSetLocalCooldown>
//
// written with the loader's byte patch, and both calls become call 217C31
// through the loader's rel32 patch, whose target is now inside D2R.exe.
//
// THE FIX
// -------
// Adopt the list. When the setter has used an existing state-185 statlist
// that carries no expire callback, install the callback it was handed and set
// the 0x8000 timer flag. The next client tick sees expire 0.0, runs the
// callback, and the machinery takes it from there: the client callback prunes
// against the client clock through the sweep, re-arms to the next surviving
// expiry, and state 185 is cleared only when the list is truly empty.
//
// Stale frame-scale values left by the sync sit far below the millisecond
// clock, so the first sweep drops them as expired, which is correct for
// cooldowns that ended long ago. If that empties the list the tick tears it
// down and the next cast builds a clean one through the create path.
//
// The server list already has a callback, so the adoption is a no-op there.
//
// Verified against the D2RLoader 1.3.1 D2RLoader.exe dump and D2RCore.dll.
// Built against PluginSDK v4 (D2RL_PLUGIN_API_VERSION 4).
// ---------------------------------------------------------------------------

#include <D2RLPlugin/api.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

// ---------------------------------------------------------------------------
// D2RLoader.exe RVAs
// ---------------------------------------------------------------------------

// SKILLS_SetLocalCooldown(unit, absoluteExpiry, skillId, cb). In 1.3.0 a thunk
// to D2RCore!SetWideSkillCooldown. Returns 2 if it created the list, 1 if the
// new cooldown is the earliest, 0 otherwise.
constexpr std::uint64_t kRvaSetLocalCooldown = 0x339E60;

// sub_1402F5940 - returns the statlist owned by a state, or null. Native, and
// what D2RCore itself calls for state 185.
constexpr std::uint64_t kRvaStatlistGetStateList = 0x2F5940;

constexpr std::int32_t kStateLocalCooldown = 185;   // 0xB9

constexpr std::size_t kStatlistFlags          = 0x1C;  // 28
constexpr std::size_t kStatlistExpireCallback = 0x88;  // 136

// The tick only visits a sublist when this flag (or flag 2) is set. With
// 0x8000 it takes the callback path, the one that prunes per skill.
constexpr std::int32_t kStatlistFlagHasExpireCallback = 0x8000;

// Setter entry: jmp qword ptr [rip+disp32] and a nop, then the untouched
// push r13 / push r14 / push r15 of the old prologue. disp32 locates the
// loader's import slot; it moves between loader builds and is never read.
constexpr std::uint8_t kSetterThunkJump[] = { 0xFF, 0x25 };
constexpr std::uint64_t kSetterThunkTailOffset = 6;
constexpr std::uint8_t kSetterThunkTail[] = { 0x90, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 };

// client, sub_140217B90, 28 bytes at 217BE4:
//   lea ecx,[rdi+rdi*4] / mov r8d,ebp / lea r9,[217FC0] / lea edx,[rax+rcx*8] /
//   mov rcx,rsi / call 339E60 / test eax,eax / je 217C1C
constexpr std::uint64_t kClientWindowRva = 0x217BE4;
constexpr std::uint64_t kClientCallRva   = 0x217BF7;
constexpr std::uint8_t kClientWindow[] = {
    0x8D, 0x0C, 0xBF, 0x44, 0x8B, 0xC5, 0x4C, 0x8D, 0x0D, 0xCF, 0x03, 0x00,
    0x00, 0x8D, 0x14, 0xC8, 0x48, 0x8B, 0xCE, 0xE8, 0x64, 0x22, 0x12, 0x00,
    0x85, 0xC0, 0x74, 0x1C,
};

// server, sub_140439500, 30 bytes at 439550:
//   add ebx,[rsi+170h] / lea r9,[43AC00] / mov edx,ebx / mov r8d,ebp /
//   mov rcx,rdi / call 339E60 / test eax,eax / je 4395C8
constexpr std::uint64_t kServerWindowRva = 0x439550;
constexpr std::uint64_t kServerCallRva   = 0x439565;
constexpr std::uint8_t kServerWindow[] = {
    0x03, 0x9E, 0x70, 0x01, 0x00, 0x00, 0x4C, 0x8D, 0x0D, 0xA3, 0x16, 0x00,
    0x00, 0x8B, 0xD3, 0x44, 0x8B, 0xC5, 0x48, 0x8B, 0xCF, 0xE8, 0xF6, 0x08,
    0xF0, 0xFF, 0x85, 0xC0, 0x74, 0x5A,
};

constexpr std::uint32_t kCallSize = 5;

struct CallSite {
    const char*         name;
    std::uint64_t       windowRva;
    const std::uint8_t* window;
    std::uint32_t       windowSize;
    std::uint64_t       callRva;
};

constexpr std::array<CallSite, 2> kSites{{
    { "client", kClientWindowRva, kClientWindow,
      static_cast<std::uint32_t>(sizeof(kClientWindow)), kClientCallRva },
    { "server", kServerWindowRva, kServerWindow,
      static_cast<std::uint32_t>(sizeof(kServerWindow)), kServerCallRva },
}};

static_assert(kClientCallRva - kClientWindowRva + kCallSize <= sizeof(kClientWindow));
static_assert(kServerCallRva - kServerWindowRva + kCallSize <= sizeof(kServerWindow));

// Both windows hold a call to the setter thunk. Proven here from their bytes.
constexpr auto CallTargetRva(const std::uint8_t* window, std::uint64_t windowRva,
                             std::uint64_t callRva) -> std::uint64_t {
    const std::size_t at = static_cast<std::size_t>(callRva - windowRva);
    const std::uint32_t raw = static_cast<std::uint32_t>(window[at + 1])
        | (static_cast<std::uint32_t>(window[at + 2]) << 8)
        | (static_cast<std::uint32_t>(window[at + 3]) << 16)
        | (static_cast<std::uint32_t>(window[at + 4]) << 24);
    return callRva + kCallSize + static_cast<std::int64_t>(static_cast<std::int32_t>(raw));
}
static_assert(kClientWindow[kClientCallRva - kClientWindowRva] == 0xE8);
static_assert(kServerWindow[kServerCallRva - kServerWindowRva] == 0xE8);
static_assert(CallTargetRva(kClientWindow, kClientWindowRva, kClientCallRva) == kRvaSetLocalCooldown);
static_assert(CallTargetRva(kServerWindow, kServerWindowRva, kServerCallRva) == kRvaSetLocalCooldown);

// The relay stub, inside D2R.exe so the loader accepts it as a rel32 target.
// sub_140217B90 ends with ret at 217C30; 15 int3 bytes pad it up to the next
// function at 217C40. The window check covers the ret and all 15 bytes.
constexpr std::uint64_t kPaddingWindowRva = 0x217C30;
constexpr std::uint8_t kPaddingWindow[] = {
    0xC3, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
    0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
};

// One 14-byte absolute jump: FF 25 00 00 00 00 <abs64>.
constexpr std::uint64_t kStubRva          = 0x217C31;
constexpr std::uint32_t kStubSize         = 14;
constexpr std::size_t   kStubTargetOffset = 6;

static_assert(kStubRva == kPaddingWindowRva + 1);
static_assert(kStubRva + kStubSize <= kPaddingWindowRva + sizeof(kPaddingWindow));

constexpr auto CanReachStub(std::uint64_t callRva) -> bool {
    const std::int64_t delta = static_cast<std::int64_t>(kStubRva)
        - static_cast<std::int64_t>(callRva + kCallSize);
    return delta >= INT32_MIN && delta <= INT32_MAX;
}
static_assert(CanReachStub(kClientCallRva) && CanReachStub(kServerCallRva));

using StubBytes = std::array<std::uint8_t, kStubSize>;

constexpr StubBytes kPadding = [] {
    StubBytes padding{};
    padding.fill(0xCC);
    return padding;
}();

// The loader's setter reads skillId as the full 32-bit r8d, so it is passed
// on as 32 bits, exactly as both native callers load it.
using SetLocalCooldownFn = std::int32_t (*)(void* unit,
                                            std::int32_t expiry,
                                            std::int32_t skillId,
                                            void* expireCallback);
using GetStateStatlistFn = void* (*)(void* unit, std::int32_t stateId);

const D2RL::PluginContext* g_context          = nullptr;
std::uintptr_t             g_base             = 0;
SetLocalCooldownFn         g_nativeSetter     = nullptr;
GetStateStatlistFn         g_getStateStatlist = nullptr;
StubBytes                  g_stub             = kPadding;  // what 0x217C31 holds now
bool                       g_stubWritten      = false;
std::array<bool, 2>        g_patched{};

std::atomic<std::uint32_t> g_setterCalls{0};
std::atomic<std::uint32_t> g_adoptions{0};
std::atomic<std::uint32_t> g_alreadyOwned{0};

template <typename T>
T* AtOffset(void* p, std::size_t offset) {
    return reinterpret_cast<T*>(static_cast<std::uint8_t*>(p) + offset);
}

auto OriginalCallBytes(const CallSite& site) noexcept -> const std::uint8_t* {
    return site.window + (site.callRva - site.windowRva);
}

std::int32_t HookedSetLocalCooldown(void* unit,
                                    std::int32_t expiry,
                                    std::int32_t skillId,
                                    void* expireCallback) noexcept {
    const std::int32_t result =
        g_nativeSetter(unit, expiry, skillId, expireCallback);

    g_setterCalls.fetch_add(1, std::memory_order_relaxed);

    if (unit == nullptr || expireCallback == nullptr) {
        return result;
    }

    void* statlist = g_getStateStatlist(unit, kStateLocalCooldown);
    if (statlist == nullptr) {
        return result;
    }

    void** slot = AtOffset<void*>(statlist, kStatlistExpireCallback);
    if (*slot != nullptr) {
        // Already a real cooldown list: the server's case, and the client's
        // once adopted. Nothing to do.
        g_alreadyOwned.fetch_add(1, std::memory_order_relaxed);
        return result;
    }

    // A state-185 statlist the sync path built. Give it the callback the
    // cooldown code was handed, plus the flag that makes the tick visit it.
    // Callback first: the tick calls [list+88h] without a null check once
    // 0x8000 is set.
    *slot = expireCallback;
    *AtOffset<std::int32_t>(statlist, kStatlistFlags) |= kStatlistFlagHasExpireCallback;
    g_adoptions.fetch_add(1, std::memory_order_relaxed);

    return result;
}

// ---------------------------------------------------------------------------
// Relay stub and call-site patching
// ---------------------------------------------------------------------------

auto EncodeStub(std::uint64_t target) noexcept -> StubBytes {
    StubBytes stub{ 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
    std::memcpy(stub.data() + kStubTargetOffset, &target, sizeof(target));
    return stub;
}

auto EncodeCall(std::uint64_t fromRva, std::uint64_t toRva) noexcept
        -> std::array<std::uint8_t, kCallSize> {
    std::array<std::uint8_t, kCallSize> bytes{ 0xE8 };
    const auto displacement = static_cast<std::int32_t>(
        static_cast<std::int64_t>(toRva) - static_cast<std::int64_t>(fromRva + kCallSize));
    std::memcpy(bytes.data() + 1, &displacement, sizeof(displacement));
    return bytes;
}

// Rewrites the stub through the loader, checked against what it holds now.
auto WriteStub(const StubBytes& next) noexcept -> bool {
    if (!g_context->PatchBytes(kStubRva, g_stub.data(), kStubSize, next.data(), kStubSize)) {
        return false;
    }
    g_stub = next;
    return true;
}

// Points the stub straight at the native setter. A call already on its way
// through the stub then runs native code only.
auto RetargetStubToNative() noexcept -> bool {
    if (!g_stubWritten) return true;
    return WriteStub(EncodeStub(g_base + kRvaSetLocalCooldown));
}

// Writes the original call back over every patched site.
auto RestoreSites() noexcept -> bool {
    bool restored = true;
    for (std::size_t i = kSites.size(); i-- > 0;) {
        if (!g_patched[i]) continue;
        const CallSite& site = kSites[i];
        const auto current = EncodeCall(site.callRva, kStubRva);
        if (g_context->PatchBytes(site.callRva, current.data(), kCallSize,
                OriginalCallBytes(site), kCallSize)) {
            g_patched[i] = false;
        } else {
            restored = false;
        }
    }
    return restored;
}

auto VerifyNativeContract() noexcept -> bool {
    if (!g_context->CheckExpectedBytes(kRvaSetLocalCooldown, kSetterThunkJump,
                sizeof(kSetterThunkJump))
            || !g_context->CheckExpectedBytes(kRvaSetLocalCooldown + kSetterThunkTailOffset,
                kSetterThunkTail, sizeof(kSetterThunkTail))) {
        g_context->LogError("local-cooldowns: RVA 0x339E60 is not the D2RLoader 1.3.0 thunk "
                            "to D2RCore SetWideSkillCooldown, refusing to load");
        return false;
    }
    for (const auto& site : kSites) {
        if (!g_context->CheckExpectedBytes(site.windowRva, site.window, site.windowSize)) {
            char line[200];
            std::snprintf(line, sizeof(line),
                "local-cooldowns: the %s cooldown call at RVA 0x%llX does not match this "
                "build, or another plugin already owns it; refusing to load",
                site.name, static_cast<unsigned long long>(site.callRva));
            g_context->LogError(line);
            return false;
        }
    }
    if (!g_context->CheckExpectedBytes(kPaddingWindowRva, kPaddingWindow,
            static_cast<std::uint32_t>(sizeof(kPaddingWindow)))) {
        g_context->LogError("local-cooldowns: the padding at RVA 0x217C31 is not free int3 "
                            "padding in this build, or another plugin already uses it; "
                            "refusing to load");
        return false;
    }
    return true;
}

// Returns false when nothing in the image reaches this DLL any more.
auto InstallCallSites() noexcept -> bool {
    if (!WriteStub(EncodeStub(reinterpret_cast<std::uint64_t>(&HookedSetLocalCooldown)))) {
        g_context->LogError("local-cooldowns: the relay stub at RVA 0x217C31 could not be written");
        return false;
    }
    g_stubWritten = true;

    for (std::size_t i = 0; i < kSites.size(); ++i) {
        const CallSite& site = kSites[i];
        if (g_context->PatchCallRel32(site.callRva, OriginalCallBytes(site), kCallSize,
                kStubRva, kCallSize)) {
            g_patched[i] = true;
            continue;
        }

        char line[160];
        std::snprintf(line, sizeof(line),
            "local-cooldowns: the %s cooldown call at RVA 0x%llX could not be redirected",
            site.name, static_cast<unsigned long long>(site.callRva));
        g_context->LogError(line);

        if (RestoreSites()) {
            // Nothing calls the stub any more: give the padding back.
            if (WriteStub(kPadding)) g_stubWritten = false;
            return false;
        }
        RetargetStubToNative();
        g_context->LogError("local-cooldowns: rollback failed, staying loaded so every "
                            "patched call keeps a valid target");
        return true;
    }
    return true;
}

auto StatusCommand(D2R::Game::Client* client,
                   const D2RL::ConsoleCommandContext* command,
                   void* userData) noexcept -> D2RL::ConsoleCommandResult {
    (void)client;
    (void)userData;

    if (command == nullptr || command->plugin == nullptr) {
        return D2RL::ConsoleCommandResult::Failed;
    }

    char line[256];

    std::snprintf(line, sizeof(line),
                  "local-cooldowns: client call %s, server call %s (setter RVA 0x%llX, "
                  "stub RVA 0x%llX)",
                  g_patched[0] ? "HOOKED" : "not hooked",
                  g_patched[1] ? "HOOKED" : "not hooked",
                  static_cast<unsigned long long>(kRvaSetLocalCooldown),
                  static_cast<unsigned long long>(kStubRva));
    command->plugin->WriteConsoleMessage(line);

    std::snprintf(line, sizeof(line),
                  "  setter calls %u, lists adopted %u, already owned %u",
                  g_setterCalls.load(std::memory_order_relaxed),
                  g_adoptions.load(std::memory_order_relaxed),
                  g_alreadyOwned.load(std::memory_order_relaxed));
    command->plugin->WriteConsoleMessage(line);

    if (g_adoptions.load(std::memory_order_relaxed) == 0 &&
        g_setterCalls.load(std::memory_order_relaxed) > 0) {
        command->plugin->WriteConsoleMessage(
            "  no list needed adopting yet - cast a skill with a localdelay");
    }

    return D2RL::ConsoleCommandResult::Handled;
}

constexpr D2RL::PluginInfo kPluginInfo{
    .infoSize    = D2RL::PluginInfoSize,
    .apiVersion  = D2RL_PLUGIN_API_VERSION,
    .id          = "celestialrayone.local-cooldowns",
    .name        = "True Local Cooldowns",
    .version     = "1.2.0",
    .author      = "CelestialRayOne",
    .description = "Gives the client's local-cooldown statlist the expire "
                   "callback it is missing, so each skill comes off cooldown "
                   "at its own expiry instead of when the last one does.",
    .flags       = D2RL::PluginFlags::Shared | D2RL::PluginFlags::NativeHooks,
};

}  // namespace

// ---------------------------------------------------------------------------
// Exports. Note the shape: GetPluginInfo takes no arguments and returns a
// pointer to a static PluginInfo.
// ---------------------------------------------------------------------------

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
    return &kPluginInfo;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
    if (context == nullptr) {
        return false;
    }

    g_context = context;
    context->LogInfo("local-cooldowns: load entered");

    g_base = context->exeBase;
    if (g_base == 0) {
        context->LogError("local-cooldowns: context exeBase is 0");
        return false;
    }

    g_nativeSetter =
        reinterpret_cast<SetLocalCooldownFn>(g_base + kRvaSetLocalCooldown);
    g_getStateStatlist =
        reinterpret_cast<GetStateStatlistFn>(g_base + kRvaStatlistGetStateList);

    if (!VerifyNativeContract()) {
        return false;
    }

    if (!InstallCallSites()) {
        return false;
    }

    context->LogInfo("local-cooldowns: client and server cooldown calls redirected "
                     "(RVA 0x217BF7, 0x439565) through the stub at RVA 0x217C31");

    if (!context->RegisterConsoleCommand("localcooldowns", StatusCommand,
                                         "Report local-cooldown hook status and "
                                         "adoption counts.")) {
        context->LogWarn("local-cooldowns: console command registration failed");
    }

    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    if (g_context == nullptr || !g_stubWritten) return;
    RetargetStubToNative();
    RestoreSites();
    // The stub is deliberately left in place, aimed at the native setter: a
    // thread may be inside its jump right now, and a site that could not be
    // restored still needs it.
}
