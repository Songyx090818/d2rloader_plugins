// Soft Hit Removal
//
// Kills made by holy aura damage, Iron Maiden and reflected damage count as
// real kills again: kill credit, the kill counter and "on kill" procs.
//
// Port of the ESR D2R 2.4 memory patches 385075, 388E2C, 36C647 and 3502EE to
// D2R 3.3 on D2RLoader 1.3.0. Every byte below was read out of the live
// D2RLoader.exe image (base 0x140000000) and disassembled before being relied
// on. Built against PluginSDK v4.
//
// ===========================================================================
// What a soft hit is
// ===========================================================================
// A damage packet carries a 16-bit result-flags word at +4. Flag 0x20 makes
// the hit silent for the event system: the kill and killed events, and the
// damage events, only fire when it is clear (byte-verified on 2.4, same logic
// in D2MOO's SUNITDMG_ExecuteEvents, 3.3 0x44CE80). The sources below force
// that flag on top of the skills.txt ResultFlags column.
//
// ===========================================================================
// Sites
// ===========================================================================
// 1. Holy Fire / Holy Shock, skills.txt srvdofunc 66, sub_1405630E0.
//    Table 0x238EA00, read by the skill do-handler at 0x43AEF9
//    (mov r10,[rcx+rax*8+238EA00h], index bound 190), slot 66 at 0x238EC10.
//      563575  0F B7 87 86 01 00 00  movzx eax, word [rdi+186h]   ; ResultFlags
//      56357C  66 83 C8 20           or    ax, 20h               ; removed
//      563580  66 09 85 94 00 00 00  or    [rbp+94h], ax         ; packet +4
//      563587  8B 87 88 01 00 00     mov   eax, [rdi+188h]       ; HitFlags
//
// 2. Holy Freeze, skills.txt srvdofunc 81, sub_140563720, slot 81 at 0x238EC88.
//    Same block, the or at 563C0B. Identity cross-checked through its target
//    callback 0x561480: the monstats ColdEffect gate and the 20% state 107 roll.
//
// 3. Iron Maiden, event function 4, sub_14055B7E0.
//    Event function table 0x238E5C0 (index 0 null, 5 = Life Tap 0x55BCD0, and
//    7, 9, 14, 15, 16, 19, 20, 21 all match Ruff's corpus), slot 4 at 0x238E5E0.
//    The 3.3 compiler builds the flags in cx, so the 2.4 bytes 66 83 C8 20 do
//    not exist here:
//      55BB87  0F B7 8E 86 01 00 00  movzx ecx, word [rsi+186h]  ; ResultFlags
//      55BB8E  66 0F C5 C1 02        pextrw eax, xmm1, 2         ; 0, zeroed packet
//      55BB93  66 83 C9 20           or    cx, 20h               ; removed
//      55BB97  66 0B C1              or    ax, cx
//      55BB9A  66 89 44 24 54        mov   [rsp+54h], ax         ; packet +4
//
// 4. Attacker Takes Damage, event function 6, sub_140582B20, slot at 0x238E5F0.
//    2.4 wrote mov eax,4021h -> mov eax,1. 3.3 loads the constant into ecx:
//      582B71  8B 85 18 01 00 00     mov   eax, [rbp+118h]       ; stat << 16 | layer
//      582B83  E8 ...                call  2F5C60                ; the item stat value
//      582BC6  B9 21 40 00 00        mov   ecx, 4021h            ; -> mov ecx, 1
//      582BED  66 0B C1              or    ax, cx
//      582BFE  66 89 44 24 34        mov   [rsp+34h], ax         ; packet +4
//      582C08  C7 45 74 8D 00 00 00  mov   [rbp+74h], 8Dh        ; hit class
//
// 5. Thorns percent reflect, sub_1404395E0 (D2MOO SKILLS_ApplyThornsDamage).
//    Reached only through two wrappers: 0x436FC0 passes stat 131 (thorns_percent)
//    and 0x436FE0 passes stat 208. Not part of the 2.4 set, off by default.
//      43975A  B8 21 40 00 00        mov   eax, 4021h            ; -> mov eax, 1
//      439766  66 89 44 24 34        mov   [rsp+34h], ax         ; packet +4
//    43975A is where three branches land. The replacement has the same length,
//    so every entry stays on an instruction boundary.
//
// In every window the flags produced by the changed instruction are dead: the
// next flag-writing instruction overwrites them before anything reads them.
// Stock and patched windows were decoded with capstone and resynchronise on
// the same instruction boundaries right after the change.
//
// Flag 0x4000 (sites 4 and 5): D2MOO names it SOFTHIT. It steers the struck
// unit's hit-reaction handling and gates no event. The 2.4 patch wrote 0x0001,
// so this port writes the same.
//
// No hooks, no caves, no allocations: same-length byte swaps only. Each one is
// checked against a witness window first, read back after writing, and put
// back on unload.

#include <D2RLPlugin/api.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace CelestialRayOne::SoftHitRemoval {
namespace {

// ---------------------------------------------------------------------------
// Native anchors
// ---------------------------------------------------------------------------

constexpr std::uint64_t SrvDoFuncTableRva = 0x238EA00;  // skills.txt srvdofunc
constexpr std::uint64_t EventFuncTableRva = 0x238E5C0;  // auraeventfunc / itemeventfunc

constexpr auto TableSlotRva(std::uint64_t table, std::uint64_t index) noexcept -> std::uint64_t {
    return table + 8 * index;
}

constexpr std::uint64_t HolyFireShockHandlerRva       = 0x5630E0;  // srvdofunc 66
constexpr std::uint64_t HolyFreezeHandlerRva          = 0x563720;  // srvdofunc 81
constexpr std::uint64_t IronMaidenHandlerRva          = 0x55B7E0;  // event function 4
constexpr std::uint64_t AttackerTakesDamageHandlerRva = 0x582B20;  // event function 6

// 563555..56359A: hit class, ResultFlags | 20h, HitFlags, packet pointer.
constexpr std::uint64_t HolyFireShockWindowRva = 0x563555;
constexpr std::uint64_t HolyFireShockPatchRva  = 0x56357C;
constexpr std::uint8_t  HolyFireShockWindow[] {
    0x8B, 0x8D, 0xD4, 0x01, 0x00, 0x00, 0x83, 0xC9, 0x0D, 0x89, 0x8D, 0xD4,
    0x01, 0x00, 0x00, 0x8B, 0x87, 0x8C, 0x01, 0x00, 0x00, 0x85, 0xC0, 0x0F,
    0x45, 0xC8, 0x89, 0x8D, 0xD4, 0x01, 0x00, 0x00, 0x0F, 0xB7, 0x87, 0x86,
    0x01, 0x00, 0x00, 0x66, 0x83, 0xC8, 0x20, 0x66, 0x09, 0x85, 0x94, 0x00,
    0x00, 0x00, 0x8B, 0x87, 0x88, 0x01, 0x00, 0x00, 0x09, 0x85, 0x90, 0x00,
    0x00, 0x00, 0x48, 0x8D, 0x85, 0x90, 0x00, 0x00, 0x00,
};

// 563BE4..563C29: the same block in srvdofunc 81.
constexpr std::uint64_t HolyFreezeWindowRva = 0x563BE4;
constexpr std::uint64_t HolyFreezePatchRva  = 0x563C0B;
constexpr std::uint8_t  HolyFreezeWindow[] {
    0x8B, 0x8D, 0xD4, 0x01, 0x00, 0x00, 0x83, 0xC9, 0x0D, 0x89, 0x8D, 0xD4,
    0x01, 0x00, 0x00, 0x8B, 0x83, 0x8C, 0x01, 0x00, 0x00, 0x85, 0xC0, 0x0F,
    0x45, 0xC8, 0x89, 0x8D, 0xD4, 0x01, 0x00, 0x00, 0x0F, 0xB7, 0x83, 0x86,
    0x01, 0x00, 0x00, 0x66, 0x83, 0xC8, 0x20, 0x66, 0x09, 0x85, 0x94, 0x00,
    0x00, 0x00, 0x8B, 0x83, 0x88, 0x01, 0x00, 0x00, 0x09, 0x85, 0x90, 0x00,
    0x00, 0x00, 0x48, 0x8D, 0x85, 0x90, 0x00, 0x00, 0x00,
};

// 55BB87..55BBAF: ResultFlags | 20h built in cx, stored to packet +4, HitFlags.
constexpr std::uint64_t IronMaidenWindowRva = 0x55BB87;
constexpr std::uint64_t IronMaidenPatchRva  = 0x55BB93;
constexpr std::uint8_t  IronMaidenWindow[] {
    0x0F, 0xB7, 0x8E, 0x86, 0x01, 0x00, 0x00, 0x66, 0x0F, 0xC5, 0xC1, 0x02,
    0x66, 0x83, 0xC9, 0x20, 0x66, 0x0B, 0xC1, 0x66, 0x89, 0x44, 0x24, 0x54,
    0x8B, 0x86, 0x88, 0x01, 0x00, 0x00, 0x09, 0x44, 0x24, 0x50, 0x8B, 0x86,
    0x8C, 0x01, 0x00, 0x00,
};

// 582BC6..582C0F: mov ecx,4021h through the hit class store.
constexpr std::uint64_t AttackerTakesDamageWindowRva = 0x582BC6;
constexpr std::uint64_t AttackerTakesDamagePatchRva  = 0x582BC6;
constexpr std::uint8_t  AttackerTakesDamageWindow[] {
    0xB9, 0x21, 0x40, 0x00, 0x00, 0x0F, 0x11, 0x4C, 0x24, 0x40, 0x89, 0x44,
    0x24, 0x48, 0x66, 0x0F, 0xC5, 0xC1, 0x02, 0x0F, 0x11, 0x45, 0x68, 0x48,
    0xC7, 0x44, 0x24, 0x78, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x11, 0x85, 0x88,
    0x00, 0x00, 0x00, 0x66, 0x0B, 0xC1, 0x48, 0xC7, 0x85, 0x88, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xCF, 0x66, 0x89, 0x44, 0x24,
    0x34, 0x0F, 0x11, 0x4C, 0x24, 0x50, 0xC7, 0x45, 0x74, 0x8D, 0x00, 0x00,
    0x00,
};

// 582B71..582B88: splits the registered stat << 16 | layer and reads the item
// stat. Proves the function is the item-stat event handler.
constexpr std::uint64_t AttackerTakesDamageIdentityRva = 0x582B71;
constexpr std::uint8_t  AttackerTakesDamageIdentity[] {
    0x8B, 0x85, 0x18, 0x01, 0x00, 0x00, 0x48, 0x8B, 0xCF, 0x8B, 0xD0, 0x44,
    0x0F, 0xB7, 0xC0, 0xC1, 0xEA, 0x10, 0xE8, 0xD8, 0x30, 0xD7, 0xFF,
};

// 43975A..439772: mov eax,4021h through the hit class store.
constexpr std::uint64_t ThornsPercentWindowRva = 0x43975A;
constexpr std::uint64_t ThornsPercentPatchRva  = 0x43975A;
constexpr std::uint8_t  ThornsPercentWindow[] {
    0xB8, 0x21, 0x40, 0x00, 0x00, 0x89, 0x54, 0x24, 0x48, 0x48, 0x8B, 0xCE,
    0x66, 0x89, 0x44, 0x24, 0x34, 0xC7, 0x45, 0x74, 0x8D, 0x00, 0x00, 0x00,
};

// 436FC0: sub rsp,38h / mov [rsp+20h],131 / call 4395E0 / add rsp,38h / ret.
// Proves 4395E0 is the thorns_percent reflect.
constexpr std::uint64_t ThornsPercentIdentityRva = 0x436FC0;
constexpr std::uint8_t  ThornsPercentIdentity[] {
    0x48, 0x83, 0xEC, 0x38, 0xC7, 0x44, 0x24, 0x20, 0x83, 0x00, 0x00, 0x00,
    0xE8, 0x0F, 0x26, 0x00, 0x00, 0x48, 0x83, 0xC4, 0x38, 0xC3,
};

constexpr std::uint8_t RemoveOrReplacement[] { 0x90, 0x90, 0x90, 0x90 };          // 4x nop
constexpr std::uint8_t MovEcxOneReplacement[] { 0xB9, 0x01, 0x00, 0x00, 0x00 };   // mov ecx, 1
constexpr std::uint8_t MovEaxOneReplacement[] { 0xB8, 0x01, 0x00, 0x00, 0x00 };   // mov eax, 1

// ---------------------------------------------------------------------------
// Site table
// ---------------------------------------------------------------------------

constexpr std::size_t MaxWindowBytes = 96;
constexpr std::size_t MaxPatchBytes  = 16;
constexpr std::size_t MaxConfigBytes = 16'384;

struct Site {
    const char*         key;
    const char*         name;
    std::uint64_t       windowRva;
    const std::uint8_t* window;
    std::uint32_t       windowSize;
    std::uint32_t       patchOffset;
    const std::uint8_t* replacement;
    std::uint32_t       patchSize;
    std::uint64_t       slotRva;       // 0 when the handler is not reached through a table
    std::uint64_t       handlerRva;
    std::uint64_t       identityRva;   // 0 when there is no extra witness
    const std::uint8_t* identity;
    std::uint32_t       identitySize;
    bool                enabledByDefault;
};

constexpr std::array<Site, 5> Sites {{
    {
        .key              = "holy_fire_and_holy_shock",
        .name             = "Holy Fire / Holy Shock (srvdofunc 66)",
        .windowRva        = HolyFireShockWindowRva,
        .window           = HolyFireShockWindow,
        .windowSize       = sizeof(HolyFireShockWindow),
        .patchOffset      = HolyFireShockPatchRva - HolyFireShockWindowRva,
        .replacement      = RemoveOrReplacement,
        .patchSize        = sizeof(RemoveOrReplacement),
        .slotRva          = TableSlotRva(SrvDoFuncTableRva, 66),
        .handlerRva       = HolyFireShockHandlerRva,
        .identityRva      = 0,
        .identity         = nullptr,
        .identitySize     = 0,
        .enabledByDefault = true,
    },
    {
        .key              = "holy_freeze",
        .name             = "Holy Freeze (srvdofunc 81)",
        .windowRva        = HolyFreezeWindowRva,
        .window           = HolyFreezeWindow,
        .windowSize       = sizeof(HolyFreezeWindow),
        .patchOffset      = HolyFreezePatchRva - HolyFreezeWindowRva,
        .replacement      = RemoveOrReplacement,
        .patchSize        = sizeof(RemoveOrReplacement),
        .slotRva          = TableSlotRva(SrvDoFuncTableRva, 81),
        .handlerRva       = HolyFreezeHandlerRva,
        .identityRva      = 0,
        .identity         = nullptr,
        .identitySize     = 0,
        .enabledByDefault = true,
    },
    {
        .key              = "iron_maiden",
        .name             = "Iron Maiden (event function 4)",
        .windowRva        = IronMaidenWindowRva,
        .window           = IronMaidenWindow,
        .windowSize       = sizeof(IronMaidenWindow),
        .patchOffset      = IronMaidenPatchRva - IronMaidenWindowRva,
        .replacement      = RemoveOrReplacement,
        .patchSize        = sizeof(RemoveOrReplacement),
        .slotRva          = TableSlotRva(EventFuncTableRva, 4),
        .handlerRva       = IronMaidenHandlerRva,
        .identityRva      = 0,
        .identity         = nullptr,
        .identitySize     = 0,
        .enabledByDefault = true,
    },
    {
        .key              = "attacker_takes_damage",
        .name             = "Attacker Takes Damage (event function 6)",
        .windowRva        = AttackerTakesDamageWindowRva,
        .window           = AttackerTakesDamageWindow,
        .windowSize       = sizeof(AttackerTakesDamageWindow),
        .patchOffset      = AttackerTakesDamagePatchRva - AttackerTakesDamageWindowRva,
        .replacement      = MovEcxOneReplacement,
        .patchSize        = sizeof(MovEcxOneReplacement),
        .slotRva          = TableSlotRva(EventFuncTableRva, 6),
        .handlerRva       = AttackerTakesDamageHandlerRva,
        .identityRva      = AttackerTakesDamageIdentityRva,
        .identity         = AttackerTakesDamageIdentity,
        .identitySize     = sizeof(AttackerTakesDamageIdentity),
        .enabledByDefault = true,
    },
    {
        .key              = "thorns_percent",
        .name             = "Thorns percent reflect (stats 131 and 208)",
        .windowRva        = ThornsPercentWindowRva,
        .window           = ThornsPercentWindow,
        .windowSize       = sizeof(ThornsPercentWindow),
        .patchOffset      = ThornsPercentPatchRva - ThornsPercentWindowRva,
        .replacement      = MovEaxOneReplacement,
        .patchSize        = sizeof(MovEaxOneReplacement),
        .slotRva          = 0,
        .handlerRva       = 0,
        .identityRva      = ThornsPercentIdentityRva,
        .identity         = ThornsPercentIdentity,
        .identitySize     = sizeof(ThornsPercentIdentity),
        .enabledByDefault = false,
    },
}};

constexpr auto SitesAreWellFormed() noexcept -> bool {
    for (const Site& site : Sites) {
        if (site.windowSize > MaxWindowBytes || site.patchSize == 0 || site.patchSize > MaxPatchBytes
                || site.patchOffset + site.patchSize > site.windowSize) {
            return false;
        }
    }
    return true;
}

static_assert(SitesAreWellFormed());

// The stock bytes at each patch offset are the instruction the 2.4 patch changed.
static_assert(HolyFireShockWindow[HolyFireShockPatchRva - HolyFireShockWindowRva] == 0x66
    && HolyFireShockWindow[HolyFireShockPatchRva - HolyFireShockWindowRva + 3] == 0x20);
static_assert(HolyFreezeWindow[HolyFreezePatchRva - HolyFreezeWindowRva] == 0x66
    && HolyFreezeWindow[HolyFreezePatchRva - HolyFreezeWindowRva + 3] == 0x20);
static_assert(IronMaidenWindow[IronMaidenPatchRva - IronMaidenWindowRva + 2] == 0xC9
    && IronMaidenWindow[IronMaidenPatchRva - IronMaidenWindowRva + 3] == 0x20);
static_assert(AttackerTakesDamageWindow[0] == 0xB9 && AttackerTakesDamageWindow[1] == 0x21
    && AttackerTakesDamageWindow[2] == 0x40);
static_assert(ThornsPercentWindow[0] == 0xB8 && ThornsPercentWindow[1] == 0x21
    && ThornsPercentWindow[2] == 0x40);

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

constexpr char DefaultConfigToml[] = R"TOML(# Soft Hit Removal
#
# A "soft hit" is damage marked with result flag 0x20. It still hurts, but the
# game treats it as silent: when it kills, the kill and killed events do not
# fire. The killer gets no kill credit, the kill counter does not move, and
# "on kill" item and skill procs do not trigger. The same flag also silences
# the damage events of that hit.
#
# D2R forces that flag onto the damage sources below, on top of whatever the
# skills.txt ResultFlags column says. Each switch removes it from one source,
# so kills made by that source count like any other kill. The first four
# switches are the ESR 2.4 memory patches, ported to D2R 3.3.
#
# If a switch finds its change already in place (for example a leftover JSON
# patch), it leaves it alone and says so in the log and in the console command.
#
# Console command: softhit (shows what is applied)
# Changes take effect the next time the game starts.

[soft_hit_removal]

# Master switch. false leaves every source stock.
enabled = true

# Every skill whose skills.txt srvdofunc is 66 (Holy Fire and Holy Shock in
# vanilla). The aura's damage keeps the row's ResultFlags value, without the
# soft hit flag added on top.
holy_fire_and_holy_shock = true

# Every skill whose skills.txt srvdofunc is 81 (Holy Freeze in vanilla).
# Same change.
holy_freeze = true

# Event function 4, skills.txt auraeventfunc 4 (Iron Maiden in vanilla).
# The reflected damage keeps the curse row's ResultFlags value, without the
# soft hit flag added on top.
iron_maiden = true

# Event function 6: itemstatcost itemeventfunc 6 (item_attackertakesdamage in
# vanilla) and skills.txt auraeventfunc 6. This is the ESR 2.4
# "Thorns (aura + item stat)" patch. Stock marks this damage with flags 0x4021;
# the switch writes 0x0001, a plain successful hit, the same value the 2.4
# patch wrote. That also clears 0x4000, which only changes how the struck unit
# reacts to the hit and does not affect events.
attacker_takes_damage = true

# The percent reflect of itemstatcost thorns_percent (stat 131), and of stat
# 208, which runs the same code. This path was NOT part of the ESR 2.4 patch
# set, so it is off by default. Same change as attacker_takes_damage.
thorns_percent = false
)TOML";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

enum class SiteState : std::uint8_t {
    NotLoaded,
    Disabled,
    Applied,
    AlreadyApplied,
    Rerouted,
    Refused,
};

struct SiteRuntime {
    bool      enabled = true;
    SiteState state   = SiteState::NotLoaded;
};

const D2RL::PluginContext*               Context{};
std::uintptr_t                           Base{};
bool                                     MasterEnabled{ true };
std::array<SiteRuntime, Sites.size()>    Runtime{};

// ---------------------------------------------------------------------------
// Config parsing (the small TOML subset the default file uses)
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

auto ParseBool(std::string_view value, bool& out) noexcept -> bool {
    if (value == "true") {
        out = true;
        return true;
    }
    if (value == "false") {
        out = false;
        return true;
    }
    return false;
}

void ApplyConfigLine(std::string_view key, std::string_view value, int lineNumber) noexcept {
    bool* target = nullptr;
    if (key == "enabled") {
        target = &MasterEnabled;
    } else {
        for (std::size_t index = 0; index < Sites.size(); ++index) {
            if (key == Sites[index].key) {
                target = &Runtime[index].enabled;
                break;
            }
        }
    }

    if (target == nullptr) {
        D2RL::LogWarnF(Context, "SoftHitRemoval: config line %d: unknown key \"%.*s\" ignored.",
            lineNumber, static_cast<int>(key.size()), key.data());
        return;
    }

    if (!ParseBool(value, *target)) {
        D2RL::LogWarnF(Context, "SoftHitRemoval: config line %d: \"%.*s\" must be true or false; keeping %s.",
            lineNumber, static_cast<int>(key.size()), key.data(), *target ? "true" : "false");
    }
}

void ParseConfig(std::string_view text) noexcept {
    int lineNumber = 0;
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        const std::size_t breakAt = text.find('\n', cursor);
        const std::size_t end = breakAt == std::string_view::npos ? text.size() : breakAt;
        std::string_view line = text.substr(cursor, end - cursor);
        cursor = end + 1;
        ++lineNumber;

        const std::size_t comment = line.find('#');
        if (comment != std::string_view::npos) {
            line = line.substr(0, comment);
        }
        line = Trim(line);
        if (line.empty() || line.front() == '[') {
            continue;
        }

        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            D2RL::LogWarnF(Context, "SoftHitRemoval: config line %d has no '=' and was ignored.", lineNumber);
            continue;
        }
        ApplyConfigLine(Trim(line.substr(0, equals)), Trim(line.substr(equals + 1)), lineNumber);
    }
}

void ReadConfiguration() noexcept {
    MasterEnabled = true;
    for (std::size_t index = 0; index < Sites.size(); ++index) {
        Runtime[index].enabled = Sites[index].enabledByDefault;
    }

    if (!Context->EnsureConfig(DefaultConfigToml)) {
        Context->LogWarn("SoftHitRemoval: the config file could not be created; using defaults.");
        return;
    }

    std::string buffer(MaxConfigBytes, '\0');
    if (!Context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size()), nullptr)) {
        Context->LogWarn("SoftHitRemoval: the config file could not be read; using defaults.");
        return;
    }
    buffer.resize(std::strlen(buffer.c_str()));
    ParseConfig(buffer);
}

// ---------------------------------------------------------------------------
// Patching
// ---------------------------------------------------------------------------

auto BytesAt(std::uint64_t rva) noexcept -> const std::uint8_t* {
    return reinterpret_cast<const std::uint8_t*>(Base + rva);
}

auto PatchRva(const Site& site) noexcept -> std::uint64_t {
    return site.windowRva + site.patchOffset;
}

auto PatchedWindow(const Site& site) noexcept -> std::array<std::uint8_t, MaxWindowBytes> {
    std::array<std::uint8_t, MaxWindowBytes> bytes{};
    std::memcpy(bytes.data(), site.window, site.windowSize);
    std::memcpy(bytes.data() + site.patchOffset, site.replacement, site.patchSize);
    return bytes;
}

void ApplySite(std::size_t index) noexcept {
    const Site&  site    = Sites[index];
    SiteRuntime& runtime = Runtime[index];
    const auto   rva     = static_cast<unsigned long long>(PatchRva(site));

    if (!MasterEnabled || !runtime.enabled) {
        runtime.state = SiteState::Disabled;
        return;
    }

    // A handler the game no longer dispatches to would take the change silently
    // and do nothing. Say so instead.
    if (site.slotRva != 0) {
        std::uint64_t slot = 0;
        std::memcpy(&slot, BytesAt(site.slotRva), sizeof(slot));
        if (slot != Base + site.handlerRva) {
            runtime.state = SiteState::Rerouted;
            D2RL::LogWarnF(Context,
                "SoftHitRemoval: %s: the dispatch slot at RVA 0x%llX points at 0x%llX, not at the native "
                "handler RVA 0x%llX, so the game does not run the code this switch changes. Nothing written.",
                site.name, static_cast<unsigned long long>(site.slotRva), static_cast<unsigned long long>(slot),
                static_cast<unsigned long long>(site.handlerRva));
            return;
        }
    }

    if (site.identity != nullptr && std::memcmp(BytesAt(site.identityRva), site.identity, site.identitySize) != 0) {
        runtime.state = SiteState::Refused;
        D2RL::LogErrorF(Context,
            "SoftHitRemoval: %s: the witness at RVA 0x%llX does not match the verified D2R image. Nothing written.",
            site.name, static_cast<unsigned long long>(site.identityRva));
        return;
    }

    const auto patched = PatchedWindow(site);

    if (std::memcmp(BytesAt(site.windowRva), site.window, site.windowSize) == 0) {
        if (!Context->PatchBytes(PatchRva(site), site.window + site.patchOffset, site.patchSize,
                site.replacement, site.patchSize)) {
            runtime.state = SiteState::Refused;
            D2RL::LogErrorF(Context, "SoftHitRemoval: %s: the loader refused the write at RVA 0x%llX.",
                site.name, rva);
            return;
        }

        if (std::memcmp(BytesAt(site.windowRva), patched.data(), site.windowSize) != 0) {
            std::array<std::uint8_t, MaxPatchBytes> current{};
            std::memcpy(current.data(), BytesAt(PatchRva(site)), site.patchSize);
            const bool restored = Context->PatchBytes(PatchRva(site), current.data(), site.patchSize,
                site.window + site.patchOffset, site.patchSize);
            runtime.state = SiteState::Refused;
            D2RL::LogErrorF(Context,
                "SoftHitRemoval: %s: the bytes read back at RVA 0x%llX were not the ones written; %s.",
                site.name, rva, restored ? "the stock bytes were put back" : "putting the stock bytes back failed too");
            return;
        }

        runtime.state = SiteState::Applied;
        D2RL::LogInfoF(Context, "SoftHitRemoval: %s: applied at RVA 0x%llX.", site.name, rva);
        return;
    }

    if (std::memcmp(BytesAt(site.windowRva), patched.data(), site.windowSize) == 0) {
        runtime.state = SiteState::AlreadyApplied;
        D2RL::LogWarnF(Context,
            "SoftHitRemoval: %s: RVA 0x%llX already holds this change (a JSON patch or another plugin). "
            "Left alone, and not restored on unload.", site.name, rva);
        return;
    }

    runtime.state = SiteState::Refused;
    D2RL::LogErrorF(Context,
        "SoftHitRemoval: %s: the bytes around RVA 0x%llX do not match the verified D2R image, or another patch "
        "owns them. Nothing written.", site.name, rva);
}

void RestoreSite(std::size_t index) noexcept {
    const Site&  site    = Sites[index];
    SiteRuntime& runtime = Runtime[index];
    if (runtime.state != SiteState::Applied) {
        return;
    }
    if (Context->PatchBytes(PatchRva(site), site.replacement, site.patchSize,
            site.window + site.patchOffset, site.patchSize)
            && std::memcmp(BytesAt(site.windowRva), site.window, site.windowSize) == 0) {
        runtime.state = SiteState::NotLoaded;
    }
}

// ---------------------------------------------------------------------------
// Console command
// ---------------------------------------------------------------------------

auto StateName(SiteState state) noexcept -> const char* {
    switch (state) {
    case SiteState::Disabled:       return "off in config";
    case SiteState::Applied:        return "APPLIED";
    case SiteState::AlreadyApplied: return "already applied by something else";
    case SiteState::Rerouted:       return "NOT APPLIED, handler rerouted (see log)";
    case SiteState::Refused:        return "NOT APPLIED, bytes do not match (see log)";
    default:                        return "not loaded";
    }
}

auto __cdecl StatusCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept
        -> D2RL::ConsoleCommandResult {
    if (command == nullptr || command->plugin == nullptr) {
        return D2RL::ConsoleCommandResult::Failed;
    }

    command->plugin->WriteConsoleMessage(MasterEnabled
        ? "Soft Hit Removal:"
        : "Soft Hit Removal: master switch is off, nothing applied.");

    char line[256];
    for (std::size_t index = 0; index < Sites.size(); ++index) {
        std::snprintf(line, sizeof(line), "  %-26s %s (RVA 0x%llX)", Sites[index].key,
            StateName(Runtime[index].state), static_cast<unsigned long long>(PatchRva(Sites[index])));
        command->plugin->WriteConsoleMessage(line);
    }
    return D2RL::ConsoleCommandResult::Handled;
}

constexpr D2RL::PluginInfo Info {
    .infoSize    = D2RL::PluginInfoSize,
    .apiVersion  = D2RL_PLUGIN_API_VERSION,
    .id          = "celestialrayone.soft-hit-removal",
    .name        = "Soft Hit Removal",
    .version     = "1.0.0",
    .author      = "CelestialRayOne",
    .description = "Holy Fire, Holy Shock, Holy Freeze, Iron Maiden and reflected damage no longer "
                   "deal soft hits, so their kills give kill credit and trigger on-kill effects.",
    .flags       = D2RL::PluginFlags::Server,
};

}  // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
    return &Info;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
    if (!D2RL::HasContext(context)) {
        return false;
    }
    Context = context;
    Base    = context->exeBase;
    if (Base == 0) {
        context->LogError("SoftHitRemoval: exeBase is 0; cannot resolve the game image.");
        return false;
    }

    ReadConfiguration();

    unsigned enabled = 0;
    unsigned live    = 0;
    for (std::size_t index = 0; index < Sites.size(); ++index) {
        ApplySite(index);
        const SiteState state = Runtime[index].state;
        if (state != SiteState::Disabled) {
            ++enabled;
        }
        if (state == SiteState::Applied || state == SiteState::AlreadyApplied) {
            ++live;
        }
    }

    if (enabled > 0 && live == 0) {
        context->LogError("SoftHitRemoval: no enabled switch could be applied; unloading.");
        return false;
    }

    D2RL::LogInfoF(Context, "SoftHitRemoval: %u of %u enabled switches are in effect.", live, enabled);

    if (!context->RegisterConsoleCommand("softhit", &StatusCommand, "Shows which soft hit removals are applied.")) {
        context->LogWarn("SoftHitRemoval: the softhit console command was refused.");
    }
    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    if (Context == nullptr || Base == 0) {
        return;
    }
    for (std::size_t index = Sites.size(); index-- > 0;) {
        RestoreSite(index);
    }
}

}  // namespace CelestialRayOne::SoftHitRemoval
