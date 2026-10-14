// =============================================================================
//  Loot Filter  -  celestialrayone.loot-filter
//
//  D2RLAN-style loot filters for D2R 3.3 under D2RLoader 1.3.1, in the JSON
//  format the launcher manages. A filter renames items, hides ground labels,
//  colors label backgrounds and borders, prints chat lines and plays sounds,
//  by rules evaluated against the client's copy of each item.
//
//  Everything below was read out of the dumped D2RLoader 1.3.1 image (md5
//  8af7a47c...) and every entry is checked byte for byte at load. Nothing is
//  installed when a check fails; the game then runs stock.
//
// -----------------------------------------------------------------------------
//  ITEM NAMES: the GetItemName import thunk 0x3E2B634
// -----------------------------------------------------------------------------
//  D2RLoader moved item naming into D2RCore.dll. The game reaches it through
//  the 6-byte thunk FF 25 86 F6 FF FF (jmp [0x3E2ACC0]) from 12 places. The
//  thunk becomes jmp rel32 + nop to a stub page next to the image, exactly as
//  charm-dedup takes its thunks. The original is D2RCore's GetItemName export
//  itself, its opening bytes checked against D2RCore 1.3.1; D2RLoader may bind
//  the game's slot only after plugins load, so the slot is just a sanity
//  check (empty, or that export). Names change only when the call comes
//  from one of these returns, all of which pass a 1024-byte buffer:
//      0x0CBFAA  ground label text   0x0CBEB0 (then copied into 128 bytes)
//      0x2BD98F, 0x2BE1E0             item tooltip 0x2BD480
//      0x2BE9CC, 0x2BEE54             item tooltip 0x2BE560
//      0x2BFC45                       item tooltip 0x2BF1B0
//  The other callers keep the stock name.
//
//  Gold never reaches GetItemName: 0x0CBEB0 builds "<amount> Gold" itself, so
//  that function is hooked too (12 bytes: push rbx/rbp/rsi/rdi, sub rsp,468h),
//  and rewrites only labels the thunk did not already handle.
//
// -----------------------------------------------------------------------------
//  HIDE: the native loot filter check 0x1C1120 (LootFilter.cpp)
// -----------------------------------------------------------------------------
//  (filter, item) -> bool "this ground item gets a label". Both label passes
//  call it: the Alt labels 0x1516D50 and the nearby-item list 0x15192B0. The
//  hook answers false for an item the filter hides, otherwise the native
//  filter's own answer stands. The game skips the check entirely while its
//  loot filter is toggled off, so that toggle also shows hidden items. Hidden
//  items keep their sprite and can still be hovered and picked up.
//  Hook: 13 bytes (mov [rsp+18h],rbx / push rsi / push rdi / push r14 /
//  sub rsp,50h), no RIP-relative operand.
//
// -----------------------------------------------------------------------------
//  LABEL BACKGROUND AND BORDER: the label draw 0x1FA8E0
// -----------------------------------------------------------------------------
//  (record, text, rgba) for each Alt label, called by 0x1517960. The record
//  starts with the label rect {x, y, w, h} and holds the unit id at +16; rgba
//  points at four floats, (0, 0, 0, 0.6) normally and, while hovered, a copy
//  of the color at 0x3440608, (0, 0.25, 0.5, 1). That color is game data the
//  game fills in after plugins load, so the plugin proves the address through
//  the code that copies it (0x1517BCF, movups from it into the record at +20)
//  and compares with the live value when drawing. The draw computes the screen rect (projected
//  through 0x65B030 unless 0x846210 says the labels are already in screen
//  space) and fills it with 0x657B90(x1, y1, x2, y2, rgba, 1) before the text.
//  The hook draws a border the same way first, then passes its own background
//  color. A hovered label keeps the game's hover color, as in D2RLAN.
//  Hook: 15 bytes (mov [rsp+10h],rbx / mov [rsp+18h],rsi / push rdi /
//  sub rsp,40h).
//
// -----------------------------------------------------------------------------
//  DROP ALERTS: the client's ground-item actions of SCMD 0x9C
// -----------------------------------------------------------------------------
//  The 0x9C entry 0x12E2C0 copies the packet (length at +2, 8 to 252 bytes),
//  then switches on the action at +1. Item stat extensions take that entry
//  over, so the plugin stays out of it and hooks the three actions that put an
//  item on the ground instead, each (packet copy) with the item id at +4:
//      0x1C6B60  action 0, a new drop       creates the item, decodes it
//      0x1C3120  action 2, dropped by hand  reuses or recreates the item
//      0x1C4DB0  action 3, seen on ground   recreates the item
//  After the stock handler the item exists, decoded, in mode 3 or 5. The hook
//  looks it up with 0x9A5D0(id, 4) and queues its chat lines and sounds, once
//  per item per game. Hooks: 18, 18 and 13 bytes of register saves and stack
//  setup; in all three the next instruction is the RIP-relative security
//  cookie load, which is not stolen.
//
// -----------------------------------------------------------------------------
//  OUTPUT
// -----------------------------------------------------------------------------
//  Chat: 0x2E97F0(string*) is the console's own "print a system line" call. It
//  copies the text into the chat queue (0x137F910 -> 0x138CF70) and keeps no
//  pointer. String object: {char* data, u64 size, u64 capacity | top bit}.
//  Colors: the renderer only knows the game's color marker, which 0x0CD6E0
//  takes from the "ColorCode" string. The plugin asks 0x0CD6E0 for it once and
//  swaps every ÿc in filter text for it.
//  sounds.txt rows: 0x1A0780(sound, null, 0, 0, 0), as transfer-to-stash uses it.
//  Sound files: miniaudio on a worker thread.
//  Language: 0x5F65F0 (text locale index) and 0x5F6790 (index -> "enUS"...),
//  the value the game logs as TextLocale.
//
// -----------------------------------------------------------------------------
//  ITEM FACTS
// -----------------------------------------------------------------------------
//  Item data 0x34A500: quality +0x00 and flags +0x18 (as the native filter's
//  rule matcher 0x1BFE30 reads them), unique/set row +0x34 (0x36CE70 returns
//  it), item level +0x38. Flags: identified 0x10, ethereal 0x400000, runeword
//  0x4000000. Code 0x36EF50, item types 0x373890, max sockets 0x36EAD0,
//  sockets = stat 194, stats 0x2F5020 (loader thunk to ReadWideUnitStat).
//  Base tier: items.txt record 0x314110 (448-byte rows), code +0x80 compared
//  with norm +0x84 / uber +0x88 / ultra +0x8C after checking +0x80 is the
//  item's own code. Area: room 0x34B440 -> room data 0x192B20 -> level id
//  0x360FC0 (room data +0x90 -> level +0x1F8). Difficulty: level +0x1C8 ->
//  drlg +0x830 (the read MapSense makes). At a vendor: parent inventory
//  0x38AC50 -> owner id 0x388BA0 -> a monster unit 0x9A5D0(id, 1).
//
// -----------------------------------------------------------------------------
//  NAME COLORS BY QUALITY (the filter's qualityColors)
// -----------------------------------------------------------------------------
//  Three places decide the color an item's name is drawn in:
//      0x0CBFF0(item) -> color   ground labels (0x0CBEB0), the Alt label pass
//                                0x1517B30 and the nearby list 0x1519BB0 and
//                                0x1519E80. A plain item gets 13 (inferior,
//                                normal, superior), magic 18, set 19, rare 15,
//                                unique 20, crafted 16, tempered 17.
//      0x2BD360(item, items.txt record, quality) -> color
//                                tooltips 0x2BD480 and 0x2BE560 (and 0x2317E0,
//                                0x2A8EA0). Plain 0, magic 3, set 2, rare 9,
//                                unique 4, crafted 8, tempered 10.
//      0x2BFC6D call 0x0CD6E0(name, broken ? 1 : 2)
//                                the set item tooltip 0x2BF1B0.
//  The first two are hooked (15 bytes each: two register saves, push rdi,
//  sub rsp,20h); the call is redirected through the stub page. A filter color
//  replaces only the color a plain item of that quality gets, so the game's
//  special colors stay: runes, quest items, socketed and ethereal grey,
//  broken red. The color number is the code character minus '0' (ÿc2 -> 2).
//
// -----------------------------------------------------------------------------
//  SOUND VOLUME
// -----------------------------------------------------------------------------
//  0x1A0780 starts a sounds.txt sound and returns its instance id; the
//  instance volume (+0x24, 0-255) starts at 255 and the sound update 0x19EA10
//  reads it every frame. 0x19F900(id, volume) is the game's own setter for it.
//  Sound files play through miniaudio at the same percentage.
//
// -----------------------------------------------------------------------------
//  MINIMAP ICONS
// -----------------------------------------------------------------------------
//  Every frame the minimap pass 0x0D2600 walks every unit of every visible
//  room and calls 0x0D76E0(unit, context) for each, items included. Its
//  marker test 0x0D78F0 turns items down, so the game draws nothing for them.
//  The plugin hooks 0x0D76E0 (13 bytes: mov [rsp+10h],rbp / push rdi /
//  sub rsp,40h / mov rdi,rdx) and, after the game's own work, draws the
//  item's icon the way the game draws its markers (0x0D6DB0):
//      position   0x34AF60 / 0x34AFB0 (an item's x/y, from its static path)
//                 -> 0x0D4910(context, &point, x | y << 32)
//      visible    context +0x18 left, +0x1C top, +0x20 width, +0x24 height
//      scale      context +0x38 (float); the point minus half the sprite size
//                 times the scale is the sprite's corner
//      draw       0x858510(sprite, &{x, y}, &params): RGB tint at +0, the
//                 fade at +16, the frame at +24, the rest zero
//  A sprite is {HD handle, legacy handle}, loaded by 0x858990(sprite, name, 0),
//  which puts "data/hd/global/ui/" in front of the name (string at 0x1D73AF8).
//  The draw has no size, so the plugin resizes each icon file to its
//  iconSize and gives the game that copy through the resource service at
//  data/hd/global/ui/d2rloader/<plugin id>/icons/<name>.sprite (and a half
//  size .lowend.sprite), the way plugin buttons get theirs. Sprites load on
//  the UI thread, never inside the minimap pass.
//
// -----------------------------------------------------------------------------
//  DROPLIGHT
// -----------------------------------------------------------------------------
//  Overlays are how the game puts an effect on one unit. When the game adds
//  one, it calls 0x773C20(layer, {unit, node}), which queues an "add overlay"
//  message (type 10: unit+0xD8 id, node, node+0xA8 overlay id). 0x773C70
//  queues "remove overlay" (type 11), 0x782670 "remove unit" (type 1). The
//  queue is a plain vector (0x78B7A0, no lock), drained each frame by
//  0x77A3B0 on the game's thread. Creating an overlay reads the node at +4
//  (overlay.txt row), +36 (height) and +0xA8 only; 0x662A90 turns the node
//  into data/hd/overlays/<folder><name>.json.
//  The plugin gives each matching ground item a node of its own (row from
//  droplight_overlay), queues it with the game's own 0x773C20, points
//  0x662A90 at its generated OverlayDefinition for those nodes (one per
//  color), and queues the removal before the game removes the item (hook on
//  0x782670).
//  Color: the engine cannot recolor the beam effect, so for each color the
//  plugin makes its own copy of the beam (beam_recolor.h) from the file the
//  mod ships (droplight_beam), and serves it with the definition.
//
//  Console command "lootfilter" shows the state; "lootfilter reload" reloads.
//  Settings: d2rloader/config/celestialrayone.loot-filter.toml
// =============================================================================

#include <D2RLPlugin/api.h>
#include <D2RLPlugin/overlay.h>
#include <D2RLPlugin/resources.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

#if defined(_MSC_VER)
#include <intrin.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#endif

#include "beam_recolor.h"
#include "filter_engine.h"
#include "filter_program.h"
#include "json_value.h"
#include "sound_player.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace CelestialRayOne::LootFilter {
namespace {

using namespace lootfilter;

constexpr char PluginIdText[]  = "celestialrayone.loot-filter";
constexpr char PluginVersion[] = "1.5.1";

const D2RL::PluginContext* Context{};
std::uintptr_t             Base{};

// ---------------------------------------------------------------------------
//  Logging
// ---------------------------------------------------------------------------
void LogInfo(const char* format, ...) noexcept {
    char message[1024]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (Context != nullptr) Context->LogInfo(message);
}

void LogWarn(const char* format, ...) noexcept {
    char message[1024]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (Context != nullptr) Context->LogWarn(message);
}

void LogError(const char* format, ...) noexcept {
    char message[1024]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (Context != nullptr) Context->LogError(message);
}

auto Narrow(const std::wstring& text) -> std::string {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(length > 0 ? length : 0), '\0');
    if (length > 0) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), length, nullptr, nullptr);
    return out;
}

auto Widen(std::string_view text) -> std::wstring {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(length > 0 ? length : 0), L'\0');
    if (length > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
}

// ---------------------------------------------------------------------------
//  Default configuration. EnsureConfig writes this text on first load, and it
//  is the plugin's documentation as shipped.
// ---------------------------------------------------------------------------
constexpr const char DefaultConfigToml[] = R"TOML(# celestialrayone.loot-filter
#
# Loot Filter
#   D2RLAN-style loot filters in JSON. A filter can rename items, hide ground
#   labels, color label backgrounds and borders, recolor item names by quality,
#   print chat lines and play sounds when an item drops, every time it drops.
#   Filter texts can use the game's own translated strings.
#
# Where filters live
#   Filters that ship with the mod:   <game>\mods\<mod>\LootFilters\
#   Your own and imported filters:    <Saved Games>\Diablo II Resurrected\mods\<mod>\LootFilters\
#   Sound files a filter plays sit in a "sounds" folder next to that filter,
#   and the .sprite files its minimap icons use in an "icons" folder.
#   The launcher's Loot Filter tab manages all of this. Which filter is active,
#   its level for each filter and whether sounds play are kept in
#   selection.json in your own filters folder. Changing a filter or the
#   selection while the game runs applies it at once.
#
# Keys (Options > Controls > Loot Filter, no default keys)
#   Next Filter Level    switches to the filter's next level and says which.
#   Show Hidden Items    shows the labels of hidden items until pressed again.
#   The game's own loot filter toggle also shows every label while it is off.
#
# Hiding
#   A hidden item gets no ground label, like the game's own loot filter does,
#   and cannot be hovered, targeted or picked up, with the mouse or a
#   controller. Gold is the exception: it stays pickable, since it is picked
#   up automatically anyway. Show Hidden Items, or turning the game's own loot
#   filter off, shows hidden items again and makes them pickable. Only items
#   on the ground are ever hidden.
#
# Console
#   lootfilter          shows the active filter, its level and any problems.
#   lootfilter reload   reads the selection and the filter again.

[loot_filter]
# Turn the whole plugin off.
enabled = true

# The mod's own filter (a file name in <mod>\LootFilters) used while the
# player has not picked one in the launcher. Empty: no filter.
default_filter = ""

# Your own filters folder. Empty: <Saved Games>\Diablo II Resurrected\mods\<mod>\LootFilters
user_folder = ""

# The overlay.txt row (its "overlay" name) a filter's droplight is attached
# as. The mod needs one looping row for it; its legacy graphics are unused,
# the plugin supplies the HD effect.
droplight_overlay = "droplight"

# The droplight beam, inside the mod's data folder. The mod ships a copy of
# the game's own fx_horadric_light.particles here; for every droplight color
# a filter uses, the plugin makes a recolored copy of it. Without the file,
# droplights keep the game's white beam.
droplight_beam = "hd/vfx/particles/overlays/object/horadric_light/fx_horadric_light.particles"

# Brightness of colored droplight beams, 1 to 100 (percent of the game's
# white beam). The game's beam is so bright that its core clips to white; a
# colored beam at full brightness clips too and loses its color (purple turns
# pinkish white). Lower keeps the color, higher is brighter.
droplight_brightness = 50

# Volume for sound files, 0 to 100. A filter's soundVolume (per rule, or for
# the whole filter) scales it further. Sounds from sounds.txt follow the game's
# own sound volume, scaled by the same soundVolume.
file_sound_volume = 80

# Print the filter's title and level in chat when a game starts and when the
# filter changes. The level key, and a level picked in the launcher during a
# game, always print the filter's level change line.
announce = true

# Text added to the label of a hidden item while Show Hidden Items is on.
hidden_marker = " ÿcO[H]"

# Write one line per item to the plugin log saying what the filter decided and
# why: the item's facts, the rules that matched, whether its label shows and
# who hid it (a rule or the game's own loot filter). For finding out why a
# rule does or does not apply. Leave off for normal play.
debug_log = false
)TOML";

struct Settings {
    bool         enabled{ true };
    std::string  defaultFilter;
    std::wstring userFolder;
    std::string  droplightOverlay{ "droplight" };
    std::string  droplightBeam{ "hd/vfx/particles/overlays/object/horadric_light/fx_horadric_light.particles" };
    int          droplightBrightness{ 50 };
    int          fileSoundVolume{ 80 };
    bool         announce{ true };
    std::string  hiddenMarker{ " \xC3\xBF" "cO[H]" };
    bool         debugLog{};
};
Settings Config;

auto Trim(std::string_view text) -> std::string_view {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
    return text;
}

// Strips a # comment that is not inside a quoted text.
auto StripComment(std::string_view line) -> std::string_view {
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '"') quoted = !quoted;
        else if (line[i] == '#' && !quoted) return line.substr(0, i);
    }
    return line;
}

auto ParseQuoted(std::string_view value, std::string& out) -> bool {
    value = Trim(value);
    if (value.size() < 2 || value.front() != '"' || value.back() != '"') return false;
    out.clear();
    for (std::size_t i = 1; i + 1 < value.size(); ++i) {
        if (value[i] == '\\' && i + 2 < value.size()) {
            ++i;
            out.push_back(value[i] == 'n' ? '\n' : value[i]);
        } else {
            out.push_back(value[i]);
        }
    }
    return true;
}

void ApplyConfigLine(std::string_view section, std::string_view key, std::string_view value) {
    if (section != "loot_filter") return;
    if (key == "enabled") {
        if (value == "true") Config.enabled = true;
        else if (value == "false") Config.enabled = false;
        else LogWarn("LootFilter: enabled: expected true or false, default kept.");
    } else if (key == "default_filter") {
        if (!ParseQuoted(value, Config.defaultFilter)) LogWarn("LootFilter: default_filter: expected a quoted text.");
    } else if (key == "droplight_overlay") {
        if (!ParseQuoted(value, Config.droplightOverlay)) LogWarn("LootFilter: droplight_overlay: expected a quoted text.");
    } else if (key == "droplight_beam") {
        if (!ParseQuoted(value, Config.droplightBeam)) LogWarn("LootFilter: droplight_beam: expected a quoted text.");
    } else if (key == "droplight_brightness") {
        char* end = nullptr;
        const std::string text(value);
        const long number = std::strtol(text.c_str(), &end, 10);
        if (end == text.c_str() || *end != '\0' || number < 1 || number > 100) LogWarn("LootFilter: droplight_brightness: expected 1 to 100.");
        else Config.droplightBrightness = static_cast<int>(number);
    } else if (key == "user_folder") {
        std::string folder;
        if (ParseQuoted(value, folder)) Config.userFolder = Widen(folder);
        else LogWarn("LootFilter: user_folder: expected a quoted text.");
    } else if (key == "file_sound_volume") {
        char* end = nullptr;
        const std::string text(value);
        const long number = std::strtol(text.c_str(), &end, 10);
        if (end == text.c_str() || *end != '\0' || number < 0 || number > 100) LogWarn("LootFilter: file_sound_volume: expected 0 to 100.");
        else Config.fileSoundVolume = static_cast<int>(number);
    } else if (key == "announce") {
        if (value == "true") Config.announce = true;
        else if (value == "false") Config.announce = false;
        else LogWarn("LootFilter: announce: expected true or false, default kept.");
    } else if (key == "hidden_marker") {
        if (!ParseQuoted(value, Config.hiddenMarker)) LogWarn("LootFilter: hidden_marker: expected a quoted text.");
    } else if (key == "debug_log") {
        if (value == "true") Config.debugLog = true;
        else if (value == "false") Config.debugLog = false;
        else LogWarn("LootFilter: debug_log: expected true or false, default kept.");
    }
}

void ReadConfiguration() {
    if (!Context->EnsureConfig(DefaultConfigToml)) {
        LogWarn("LootFilter: the config file could not be created; using defaults.");
        return;
    }
    std::string buffer(16384, '\0');
    if (!Context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size()), nullptr)) {
        LogWarn("LootFilter: the config file could not be read; using defaults.");
        return;
    }
    buffer.resize(std::strlen(buffer.c_str()));
    std::string_view text(buffer);
    std::string_view section;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        const std::string_view line = Trim(StripComment(text.substr(0, end)));
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

// ---------------------------------------------------------------------------
//  Addresses (RVAs into the D2RLoader 1.3.1 image) and their witnesses
// ---------------------------------------------------------------------------

// Hooked
constexpr std::uint64_t GetItemNameThunkRva = 0x3E2B634;
constexpr std::uint64_t GetItemNameSlotRva  = 0x3E2ACC0;
constexpr std::uint64_t LabelTextRva        = 0x0CBEB0;
constexpr std::uint64_t ShowLabelRva        = 0x1C1120;
constexpr std::uint64_t DrawLabelRva        = 0x1FA8E0;
constexpr std::uint64_t NewDropRva          = 0x1C6B60;   // 0x9C action 0
constexpr std::uint64_t HandDropRva         = 0x1C3120;   // 0x9C action 2
constexpr std::uint64_t SeenOnGroundRva     = 0x1C4DB0;   // 0x9C action 3
constexpr std::uint64_t LabelColorRva       = 0x0CBFF0;   // ground label name color
constexpr std::uint64_t NameColorRva        = 0x2BD360;   // tooltip name color
constexpr std::uint64_t SetNameLeadRva      = 0x2BFC5F;   // set tooltip: broken ? 1 : 2, then...
constexpr std::uint64_t SetNameColorCallRva = 0x2BFC6D;   // ...call colorize(name, color)

// Called
constexpr std::uint64_t ReadStatRva         = 0x2F5020;
constexpr std::uint64_t LocalIndexRva       = 0x08B2D0;
constexpr std::uint64_t LocalPlayerRva      = 0x09A480;
constexpr std::uint64_t ClientUnitRva       = 0x09A5D0;
constexpr std::uint64_t PlaySoundRva        = 0x1A0780;
constexpr std::uint64_t ColorizeRva         = 0x0CD6E0;
constexpr std::uint64_t PrintChatRva        = 0x2E97F0;
constexpr std::uint64_t TextLocaleRva       = 0x5F65F0;
constexpr std::uint64_t LocaleNameRva       = 0x5F6790;
constexpr std::uint64_t GetItemDataRva      = 0x34A500;
constexpr std::uint64_t GetItemCodeRva      = 0x36EF50;
constexpr std::uint64_t CheckItemTypeRva    = 0x373890;
constexpr std::uint64_t MaxSocketsRva       = 0x36EAD0;
constexpr std::uint64_t ItemContextRva      = 0x34A0E0;
constexpr std::uint64_t ItemsRecordRva      = 0x314110;
constexpr std::uint64_t ParentInventoryRva  = 0x38AC50;
constexpr std::uint64_t InventoryOwnerRva   = 0x388BA0;
constexpr std::uint64_t UnitRoomRva         = 0x34B440;
constexpr std::uint64_t RoomDataRva         = 0x192B20;
constexpr std::uint64_t RoomLevelIdRva      = 0x360FC0;
constexpr std::uint64_t LabelScreenSpaceRva = 0x846210;
constexpr std::uint64_t ProjectPointRva     = 0x65B030;
constexpr std::uint64_t FillRectRva         = 0x657B90;
constexpr std::uint64_t SetSoundVolumeRva   = 0x19F900;
constexpr std::uint64_t AutomapUnitRva      = 0x0D76E0;   // per-unit minimap draw
constexpr std::uint64_t ProjectAutomapRva   = 0x0D4910;
constexpr std::uint64_t UnitClientXRva      = 0x34AF60;
constexpr std::uint64_t UnitClientYRva      = 0x34AFB0;
constexpr std::uint64_t SpriteLoadRva       = 0x858990;
constexpr std::uint64_t SpriteDrawRva       = 0x858510;
constexpr std::uint64_t SpriteDrawHdRva     = 0x79DA50;
constexpr std::uint64_t UnitSelectTestRva   = 0x09B840;
constexpr std::uint64_t TargetSelectTestRva = 0x1919F0;
constexpr std::uint64_t GameToggleRva       = 0x1FAE30;
constexpr std::uint64_t OverlayAddedRva     = 0x773C20;
constexpr std::uint64_t OverlayRemovedRva   = 0x773C70;
constexpr std::uint64_t UnitRemovedRva      = 0x782670;
constexpr std::uint64_t OverlayPathRva      = 0x662A90;
constexpr std::uint64_t GameStringRva       = 0x0BF810;

// Data
constexpr std::uint64_t HoverColorRva       = 0x3440608;   // filled in by the game after plugins load
constexpr std::uint64_t HoverColorLoadRva   = 0x1517BCF;   // the code that copies it into a hovered label

// Callers of GetItemName whose names the filter may change (call site + 5).
constexpr std::array<std::uint64_t, 6> NameCallSites{ 0x0CBFA5, 0x2BD98A, 0x2BE1DB, 0x2BE9C7, 0x2BEE4F, 0x2BFC40 };
constexpr std::uint64_t GroundLabelReturnRva = 0x0CBFAA;

// Layouts
constexpr std::size_t UnitTypeOffset        = 0x00;
constexpr std::size_t UnitClassOffset       = 0x04;
constexpr std::size_t UnitIdOffset          = 0x08;
constexpr std::size_t UnitModeOffset        = 0x0C;
constexpr std::size_t ItemQualityOffset     = 0x00;
constexpr std::size_t ItemFlagsOffset       = 0x18;
constexpr std::size_t ItemFileIndexOffset   = 0x34;
constexpr std::size_t ItemLevelOffset       = 0x38;
constexpr std::size_t ItemsCodeOffset       = 0x80;
constexpr std::size_t ItemsNormCodeOffset   = 0x84;
constexpr std::size_t ItemsUberCodeOffset   = 0x88;
constexpr std::size_t ItemsUltraCodeOffset  = 0x8C;
constexpr std::size_t RoomLevelOffset       = 0x90;
constexpr std::size_t LevelDrlgOffset       = 0x1C8;
constexpr std::size_t DrlgDifficultyOffset  = 0x830;
constexpr std::size_t LabelUnitIdOffset     = 16;

constexpr std::uint32_t FlagIdentified = 0x10;
constexpr std::uint32_t FlagEthereal   = 0x400000;
constexpr std::uint32_t FlagRuneword   = 0x4000000;
constexpr std::uint32_t UnitTypeItem   = 4;
constexpr std::uint32_t UnitTypeMonster = 1;

// Witnesses
constexpr std::uint8_t GetItemNameThunkBytes[]{ 0xFF, 0x25, 0x86, 0xF6, 0xFF, 0xFF };
constexpr std::uint8_t LabelTextBytes[]{ 0x40, 0x53, 0x55, 0x56, 0x57, 0x48, 0x81, 0xEC, 0x68, 0x04, 0x00, 0x00 };
constexpr std::uint8_t LabelTextTailBytes[]{ 0x48, 0x8B, 0x05, 0x05, 0xF4, 0x8F, 0x02, 0x48, 0x33, 0xC4 };
constexpr std::uint8_t ShowLabelBytes[]{ 0x48, 0x89, 0x5C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x50 };
constexpr std::uint8_t ShowLabelTailBytes[]{ 0x48, 0x8B, 0xDA, 0x48, 0x8B, 0xF9, 0x48, 0x85, 0xD2, 0x0F, 0x84, 0x86, 0x01, 0x00, 0x00 };
constexpr std::uint8_t DrawLabelBytes[]{ 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x40 };
constexpr std::uint8_t DrawLabelTailBytes[]{ 0x49, 0x8B, 0xD8, 0x48, 0x8B, 0xFA, 0x48, 0x8B, 0xF1, 0x4D, 0x85, 0xC0 };
constexpr std::uint8_t NewDropBytes[]{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x70 };
constexpr std::uint8_t NewDropTailBytes[]{ 0x48, 0x8B, 0x05, 0x4F, 0x47, 0x80, 0x02, 0x48, 0x33, 0xC4 };
constexpr std::uint8_t HandDropBytes[]{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x70 };
constexpr std::uint8_t HandDropTailBytes[]{ 0x48, 0x8B, 0x05, 0x8F, 0x81, 0x80, 0x02, 0x48, 0x33, 0xC4 };
constexpr std::uint8_t SeenOnGroundBytes[]{ 0x40, 0x53, 0x56, 0x57, 0x41, 0x54, 0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00 };
constexpr std::uint8_t SeenOnGroundTailBytes[]{ 0x48, 0x8B, 0x05, 0x04, 0x65, 0x80, 0x02, 0x48, 0x33, 0xC4 };
// mov [rsp+10h],rbx / mov [rsp+18h],rsi / push rdi / sub rsp,20h; then mov rsi,rcx / test rcx,rcx / jnz / mov [rsp+30h],cl
constexpr std::uint8_t LabelColorBytes[]{ 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20 };
constexpr std::uint8_t LabelColorTailBytes[]{ 0x48, 0x8B, 0xF1, 0x48, 0x85, 0xC9, 0x75, 0x13, 0x88, 0x4C, 0x24, 0x30 };
// mov [rsp+8],rbx / mov [rsp+10h],rsi / push rdi / sub rsp,20h; then xor ebx,ebx / add r8d,-4 / mov rsi,rdx / mov rdi,rcx / cmp r8d,5 / ja
constexpr std::uint8_t NameColorBytes[]{ 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20 };
constexpr std::uint8_t NameColorTailBytes[]{
    0x33, 0xDB, 0x41, 0x83, 0xC0, 0xFC, 0x48, 0x8B, 0xF2, 0x48, 0x8B, 0xF9, 0x41, 0x83, 0xF8, 0x05, 0x77, 0x42 };
// neg eax / lea rcx,[rbp+2610h] / sbb edx,edx / add edx,2 / call 0x0CD6E0
constexpr std::uint8_t SetNameColorBytes[]{
    0xF7, 0xD8, 0x48, 0x8D, 0x8D, 0x10, 0x26, 0x00, 0x00, 0x1B, 0xD2, 0x83, 0xC2, 0x02, 0xE8, 0x6E, 0xDA, 0xE0, 0xFF };
constexpr std::uint8_t SetNameColorCallBytes[]{ 0xE8, 0x6E, 0xDA, 0xE0, 0xFF };

constexpr std::uint8_t ReadStatBytes[]{ 0xFF, 0x25, 0xF2, 0x51, 0xB3, 0x03 };
constexpr std::uint8_t LocalIndexBytes[]{ 0x8B, 0x05, 0x2E, 0x84, 0x99, 0x02, 0xC3 };
constexpr std::uint8_t LocalPlayerBytes[]{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x83, 0xF9, 0x08, 0x0F, 0x83, 0x85,
    0x00, 0x00, 0x00, 0x8B, 0xD9, 0x48, 0x89, 0x5C, 0x24, 0x38, 0x48, 0x83, 0xFB, 0x08, 0x72, 0x19 };
constexpr std::uint8_t ClientUnitBytes[]{
    0x4C, 0x63, 0xCA, 0x48, 0x8D, 0x05, 0x36, 0x93, 0x98, 0x02, 0x8B, 0xD1, 0x44, 0x8B,
    0xC1, 0x49, 0x8B, 0xC9, 0x83, 0xE2, 0x7F, 0x48, 0xC1, 0xE1, 0x0A, 0x48, 0x03, 0xC8 };
constexpr std::uint8_t PlaySoundBytes[]{
    0x40, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xD1, 0x48, 0x81, 0xEC,
    0x00, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x05, 0x2D, 0xAB, 0x82, 0x02, 0x48, 0x33, 0xC4, 0x48, 0x89 };
constexpr std::uint8_t ColorizeBytes[]{
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x57, 0x48, 0x81, 0xEC, 0x50, 0x04, 0x00, 0x00, 0x48, 0x8B, 0x05,
    0xD4, 0xDB, 0x8F, 0x02, 0x48, 0x33, 0xC4, 0x48, 0x89, 0x84, 0x24, 0x40, 0x04, 0x00, 0x00, 0x80,
    0x39, 0x00, 0x8B, 0xFA, 0x48, 0x8B, 0xD9 };
constexpr std::uint8_t PrintChatBytes[]{
    0x48, 0x83, 0xEC, 0x68, 0x48, 0x8B, 0x05, 0xCD, 0x1A, 0x6E, 0x02, 0x48, 0x33, 0xC4, 0x48, 0x89,
    0x44, 0x24, 0x50, 0x48, 0x83, 0x79, 0x08, 0x00, 0x76, 0x65 };
constexpr std::uint8_t TextLocaleBytes[]{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0x05, 0xC4, 0x7E, 0xDB, 0x01, 0x33, 0xDB, 0x48, 0x8B,
    0x15, 0x73, 0x92, 0xCF, 0x02, 0x83, 0xF8, 0xFF, 0x75, 0x1E };
constexpr std::uint8_t LocaleNameBytes[]{ 0x48, 0x63, 0xC1, 0x48, 0x8D, 0x0D, 0x96, 0xC2, 0x74, 0x01, 0x48, 0x8B, 0x04, 0xC1, 0xC3 };
constexpr std::uint8_t GetItemDataBytes[]{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9, 0x75, 0x1D, 0x88, 0x4C,
    0x24, 0x30, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8, 0x74, 0xC4, 0xFF, 0xFF, 0x84, 0xC0, 0x74, 0x01 };
constexpr std::uint8_t GetItemCodeBytes[]{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF9, 0x48, 0x85, 0xC9,
    0x75, 0x13, 0x88, 0x4C, 0x24, 0x30, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8, 0x80, 0x83, 0xFF, 0xFF };
constexpr std::uint8_t CheckItemTypeBytes[]{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20 };
constexpr std::uint8_t MaxSocketsBytes[]{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48 };
constexpr std::uint8_t ItemContextBytes[]{
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x85, 0xC9, 0x75, 0x1A, 0x88, 0x4C, 0x24, 0x30, 0x48, 0x8D, 0x4C };
constexpr std::uint8_t ItemsRecordBytes[]{
    0x40, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x8B, 0xFA, 0xE8, 0x73, 0xC9, 0xFE, 0xFF, 0x3B, 0xB8, 0xA8 };
constexpr std::uint8_t ParentInventoryBytes[]{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9, 0x74, 0x0A, 0xE8, 0x6D,
    0x0D, 0xFC, 0xFF, 0x83, 0xF8, 0x04, 0x74, 0x19, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xC6, 0x44, 0x24 };
constexpr std::uint8_t InventoryOwnerBytes[]{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9, 0x75, 0x1E, 0x88, 0x4C,
    0x24, 0x30, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8, 0xB4, 0xC2, 0xFF, 0xFF, 0x84, 0xC0, 0x74, 0x30 };
constexpr std::uint8_t UnitRoomBytes[]{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9, 0x75, 0x13,
    0x88, 0x4C, 0x24, 0x30, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8, 0x54, 0xA7, 0xFF, 0xFF };
constexpr std::uint8_t RoomDataBytes[]{ 0x48, 0x8B, 0x41, 0x18, 0xC3 };
constexpr std::uint8_t RoomLevelIdBytes[]{ 0x48, 0x8B, 0x81, 0x90, 0x00, 0x00, 0x00, 0x8B, 0x80, 0xF8, 0x01, 0x00, 0x00, 0xC3 };
constexpr std::uint8_t LabelScreenSpaceBytes[]{
    0x48, 0x8B, 0x05, 0x59, 0x9F, 0xBF, 0x02, 0x48, 0x85, 0xC0, 0x74, 0x08, 0x0F, 0xB6, 0x80, 0xB9, 0x00, 0x00, 0x00, 0xC3 };
constexpr std::uint8_t ProjectPointBytes[]{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0xE8, 0xFE, 0x60, 0x7C, 0x00 };
constexpr std::uint8_t FillRectBytes[]{
    0x44, 0x3B, 0xC1, 0x0F, 0x8C, 0xD3, 0x02, 0x00, 0x00, 0x4C, 0x8B, 0xDC, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x57 };
// movups xmm0,[rip -> 0x3440608] / mov rdx,r14 / mov ecx,ebp / movups [rbx+14h],xmm0:
// the hovered label's color is copied from 0x3440608 into the label record.
// The game's sound volume setter: find the instance with this id in the
// playing list (0x2A960A0, count 0x2A960A8) and store the volume at +0x24.
constexpr std::uint8_t SetSoundVolumeBytes[]{
    0x44, 0x8B, 0xD2, 0x48, 0x85, 0xC9, 0x74, 0x2F, 0x48, 0x8B, 0x05, 0x91, 0x67, 0x8F, 0x02, 0x4C,
    0x8B, 0x05, 0x92, 0x67, 0x8F, 0x02, 0x4E, 0x8D, 0x0C, 0xC0, 0x49, 0x3B, 0xC1, 0x74, 0x18, 0x90,
    0x48, 0x8B, 0x10, 0x48, 0x39, 0x4A, 0x0C, 0x74, 0x0A, 0x48, 0x83, 0xC0, 0x08, 0x49, 0x3B, 0xC1,
    0x75, 0xEE, 0xC3, 0x44, 0x89, 0x52, 0x24, 0xC3 };
// mov [rsp+10h],rbp / push rdi / sub rsp,40h / mov rdi,rdx; then lea r8,[rsp+68h] /
// lea rdx,[rsp+60h] / mov rbp,rcx / call 0x0D78F0 (marker test) / test al,al
constexpr std::uint8_t AutomapUnitBytes[]{ 0x48, 0x89, 0x6C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B, 0xFA };
constexpr std::uint8_t AutomapUnitTailBytes[]{
    0x4C, 0x8D, 0x44, 0x24, 0x68, 0x48, 0x8D, 0x54, 0x24, 0x60, 0x48, 0x8B, 0xE9, 0xE8, 0xF1, 0x01, 0x00, 0x00, 0x84, 0xC0 };
constexpr std::uint8_t ProjectAutomapBytes[]{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57,
    0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x66, 0x0F, 0x6E, 0x41, 0x10, 0x4C, 0x8B, 0xFA };
constexpr std::uint8_t UnitClientXBytes[]{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9, 0x75, 0x13, 0x88, 0x4C,
    0x24, 0x30, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8, 0x94, 0xA3, 0xFF, 0xFF, 0x84, 0xC0, 0x74, 0x01 };
constexpr std::uint8_t UnitClientYBytes[]{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9, 0x75, 0x13, 0x88, 0x4C,
    0x24, 0x30, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8, 0x94, 0x9D, 0xFF, 0xFF, 0x84, 0xC0, 0x74, 0x01 };
constexpr std::uint8_t SpriteLoadBytes[]{
    0x40, 0x53, 0x56, 0x57, 0x48, 0x81, 0xEC, 0x40, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x05, 0x26, 0x29,
    0x17, 0x02, 0x48, 0x33, 0xC4, 0x48, 0x89, 0x84, 0x24, 0x30, 0x01, 0x00, 0x00, 0x48, 0x8B, 0xD9 };
// The HD branch of 0x858510: (hd sprite, position, float scale in xmm2,
// params in r9). 0x858510 never sets xmm2 itself; it passes its caller's
// through, and the game's marker draw (0x0D6DB0) has the minimap scale there.
// movaps xmm8, xmm2 and mov rsi, r9 at +0x30 are the scale and the params.
constexpr std::uint8_t SpriteDrawHdBytes[]{
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x41, 0x56, 0x48, 0x8D, 0xAC, 0x24, 0x70, 0xFF, 0xFF,
    0xFF, 0x48, 0x81, 0xEC, 0x90, 0x01, 0x00, 0x00, 0x44, 0x0F, 0x29, 0x84, 0x24, 0x60, 0x01, 0x00,
    0x00, 0x48, 0x8B, 0x05, 0x50, 0xD8, 0x22, 0x02, 0x48, 0x33, 0xC4, 0x48, 0x89, 0x45, 0xE0, 0x44,
    0x0F, 0x28, 0xC2, 0x49, 0x8B, 0xF1 };
// D2Client's unit select test (D2Client\src\Unit*.cpp): (unit, x, y, mode,
// skill). Mode 0 is the mouse: the hit test of the unit under the cursor
// (items only while their labels are not up, else the label is what is
// clicked). Mode 1 is the controller, which asks TargetingController::
// UnitTestSelect. mov [rsp+20h],rbx, then push rbp,rsi,rdi,r12-r15 /
// mov rbp,rsp / sub rsp,80h.
constexpr std::uint8_t UnitSelectTestBytes[]{ 0x48, 0x89, 0x5C, 0x24, 0x20 };
constexpr std::uint8_t UnitSelectTestTailBytes[]{
    0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8B, 0xEC, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00 };
// TargetingController::UnitTestSelect(controller, player, unit): every
// controller target, the loot target included, and the controller's list of
// nearby items. mov rax,rsp / push rbx,rbp,rsi,rdi, then sub rsp,0C8h /
// mov [rax+8],r12 / mov rsi,r8 / mov [rax+10h],r13 / mov r12,rdx.
constexpr std::uint8_t TargetSelectTestBytes[]{ 0x48, 0x8B, 0xC4, 0x53, 0x55, 0x56, 0x57 };
constexpr std::uint8_t TargetSelectTestTailBytes[]{
    0x48, 0x81, 0xEC, 0xC8, 0x00, 0x00, 0x00, 0x4C, 0x89, 0x60, 0x08, 0x49, 0x8B, 0xF0, 0x4C, 0x89, 0x68, 0x10, 0x4C, 0x8B, 0xE2 };
// The game's toggle table (byte per toggle). The label pass (0x141516D50)
// asks the loot filter (0x1C1120) only while toggle 1 is clear; set, every
// label shows: the game's own loot filter toggle is off.
constexpr std::uint8_t GameToggleBytes[]{
    0x48, 0x83, 0xEC, 0x38, 0x0F, 0xB6, 0xD1, 0x48, 0x3B, 0x15, 0x2A, 0x25, 0x16, 0x02, 0x48, 0x89, 0x54, 0x24, 0x48 };
constexpr std::uint8_t GameToggleFilterOff = 1;
constexpr std::uint8_t SpriteDrawBytes[]{
    0x40, 0x55, 0x53, 0x57, 0x41, 0x56, 0x48, 0x8D, 0x6C, 0x24, 0xD8, 0x48, 0x81, 0xEC, 0x28, 0x01,
    0x00, 0x00, 0x0F, 0x29, 0xB4, 0x24, 0x10, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x05, 0x97, 0x2D, 0x17, 0x02 };
// sub rsp,0A8h; then mov rax,[rdx] / add rcx,1810h / mov [rsp+30h],rax / mov dword [rsp+20h],0Ah
constexpr std::uint8_t OverlayAddedBytes[]{ 0x48, 0x81, 0xEC, 0xA8, 0x00, 0x00, 0x00 };
constexpr std::uint8_t OverlayAddedTailBytes[]{
    0x48, 0x8B, 0x02, 0x48, 0x81, 0xC1, 0x10, 0x18, 0x00, 0x00, 0x48, 0x89, 0x44, 0x24, 0x30, 0xC7, 0x44, 0x24, 0x20, 0x0A, 0x00, 0x00, 0x00 };
constexpr std::uint8_t OverlayRemovedBytes[]{
    0x48, 0x89, 0x5C, 0x24, 0x20, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00 };
// mov [rsp+10h],rdx / push rbx,rbp,rsi,rdi,r15 / sub rsp,0B0h; then mov r8,[rcx+0A0h] / mov rbp,rcx
constexpr std::uint8_t UnitRemovedBytes[]{
    0x48, 0x89, 0x54, 0x24, 0x10, 0x53, 0x55, 0x56, 0x57, 0x41, 0x57, 0x48, 0x81, 0xEC, 0xB0, 0x00, 0x00, 0x00 };
constexpr std::uint8_t UnitRemovedTailBytes[]{ 0x4C, 0x8B, 0x81, 0xA0, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xE9 };
// mov r11,rsp / push rbp / push rdi / lea rbp,[r11-5Fh] / sub rsp,0F8h; then the security cookie load
constexpr std::uint8_t OverlayPathBytes[]{ 0x4C, 0x8B, 0xDC, 0x55, 0x57, 0x49, 0x8D, 0x6B, 0xA1, 0x48, 0x81, 0xEC, 0xF8, 0x00, 0x00, 0x00 };
constexpr std::uint8_t OverlayPathTailBytes[]{ 0x48, 0x8B, 0x05, 0x21, 0x88, 0x36, 0x02, 0x48, 0x33, 0xC4, 0x48, 0x89, 0x45, 0x37 };
constexpr std::uint8_t GameStringBytes[]{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x33,
    0xDB, 0x48, 0x8D, 0x41, 0x18, 0x48, 0x89, 0x59, 0x08, 0x48, 0x8B, 0xF9, 0x48, 0x89, 0x01, 0x48 };
constexpr std::uint8_t HoverColorLoadBytes[]{
    0x0F, 0x10, 0x05, 0x32, 0x8A, 0xF2, 0x01, 0x49, 0x8B, 0xD6, 0x8B, 0xCD, 0x0F, 0x11, 0x43, 0x14 };
// D2RCore.dll 1.3.1, GetItemName export: push rbp/rsi/rdi/rbx, sub rsp,0A8h,
// lea rbp,[rsp+80h], mov [rbp+20h],-2, mov rsi,rdx, mov rdi,rcx, call [stock name].
constexpr std::uint8_t CoreGetItemNameBytes[]{
    0x55, 0x56, 0x57, 0x53, 0x48, 0x81, 0xEC, 0xA8, 0x00, 0x00, 0x00, 0x48, 0x8D, 0xAC, 0x24, 0x80,
    0x00, 0x00, 0x00, 0x48, 0xC7, 0x45, 0x20, 0xFE, 0xFF, 0xFF, 0xFF, 0x48, 0x89, 0xD6, 0x48, 0x89,
    0xCF, 0xFF, 0x15, 0x51, 0x80, 0xEE, 0xFF };

struct Witness {
    std::uint64_t       rva;
    const std::uint8_t* bytes;
    std::size_t         size;
    const char*         what;
};

#define LF_WITNESS(rva, bytes, what) Witness{ rva, bytes, sizeof(bytes), what }
constexpr std::array<Witness, 22> GameWitnesses{
    LF_WITNESS(ReadStatRva, ReadStatBytes, "stat reader thunk"),
    LF_WITNESS(LocalIndexRva, LocalIndexBytes, "local data context"),
    LF_WITNESS(LocalPlayerRva, LocalPlayerBytes, "local player"),
    LF_WITNESS(ClientUnitRva, ClientUnitBytes, "client unit lookup"),
    LF_WITNESS(PlaySoundRva, PlaySoundBytes, "sound player"),
    LF_WITNESS(ColorizeRva, ColorizeBytes, "text colouring"),
    LF_WITNESS(PrintChatRva, PrintChatBytes, "chat line"),
    LF_WITNESS(TextLocaleRva, TextLocaleBytes, "text language"),
    LF_WITNESS(LocaleNameRva, LocaleNameBytes, "language code"),
    LF_WITNESS(GetItemDataRva, GetItemDataBytes, "item data"),
    LF_WITNESS(GetItemCodeRva, GetItemCodeBytes, "item code"),
    LF_WITNESS(CheckItemTypeRva, CheckItemTypeBytes, "item type check"),
    LF_WITNESS(MaxSocketsRva, MaxSocketsBytes, "max sockets"),
    LF_WITNESS(ItemContextRva, ItemContextBytes, "item data context"),
    LF_WITNESS(ItemsRecordRva, ItemsRecordBytes, "items.txt record"),
    LF_WITNESS(ParentInventoryRva, ParentInventoryBytes, "parent inventory"),
    LF_WITNESS(InventoryOwnerRva, InventoryOwnerBytes, "inventory owner"),
    LF_WITNESS(UnitRoomRva, UnitRoomBytes, "unit room"),
    LF_WITNESS(RoomDataRva, RoomDataBytes, "room data"),
    LF_WITNESS(RoomLevelIdRva, RoomLevelIdBytes, "room level id"),
    LF_WITNESS(LabelScreenSpaceRva, LabelScreenSpaceBytes, "label screen space flag"),
    LF_WITNESS(HoverColorLoadRva, HoverColorLoadBytes, "hovered label color"),
};
constexpr std::array<Witness, 3> DrawWitnesses{
    LF_WITNESS(DrawLabelRva + sizeof(DrawLabelBytes), DrawLabelTailBytes, "label draw, after the stolen bytes"),
    LF_WITNESS(ProjectPointRva, ProjectPointBytes, "label projection"),
    LF_WITNESS(FillRectRva, FillRectBytes, "filled rectangle"),
};
#undef LF_WITNESS

// ---------------------------------------------------------------------------
//  Native functions
// ---------------------------------------------------------------------------
struct GameString {
    const char*   data;
    std::uint64_t size;
    std::uint64_t capacity;
};

using ReadStatFn         = std::int32_t(__fastcall*)(void* unit, std::int32_t stat, std::int32_t layer);
using LocalIndexFn       = std::int32_t(__fastcall*)();
using LocalPlayerFn      = void*(__fastcall*)(std::int32_t index);
using ClientUnitFn       = void*(__fastcall*)(std::uint32_t id, std::uint32_t type);
using PlaySoundFn        = std::uint64_t(__fastcall*)(std::int32_t sound, void* unit, std::int32_t delay, std::int32_t option, std::uint8_t flag);
using ColorizeFn         = char*(__fastcall*)(char* buffer, std::int32_t color);
using PrintChatFn        = void(__fastcall*)(const GameString* text);
using TextLocaleFn       = std::int32_t(__fastcall*)();
using LocaleNameFn       = const char*(__fastcall*)(std::int32_t index);
using GetItemDataFn      = std::uint8_t*(__fastcall*)(void* item);
using GetItemCodeFn      = std::uint32_t(__fastcall*)(void* item);
using CheckItemTypeFn    = std::int32_t(__fastcall*)(void* item, std::int32_t type);
using MaxSocketsFn       = std::int32_t(__fastcall*)(void* item);  // returns the count in the low byte
using ItemContextFn      = std::uint8_t(__fastcall*)(void* unit);
using ItemsRecordFn      = std::uint8_t*(__fastcall*)(std::uint8_t context, std::uint32_t classId);
using ParentInventoryFn  = void*(__fastcall*)(void* item);
using InventoryOwnerFn   = std::int32_t(__fastcall*)(void* inventory);
using UnitRoomFn         = void*(__fastcall*)(void* unit);
using RoomDataFn         = std::uint8_t*(__fastcall*)(void* activeRoom);
using RoomLevelIdFn      = std::int32_t(__fastcall*)(void* roomData);
using LabelScreenSpaceFn = bool(__fastcall*)();
using ProjectPointFn     = std::uint64_t(__fastcall*)(std::uint64_t point);
using FillRectFn         = void(__fastcall*)(std::int32_t x1, std::int32_t y1, std::int32_t x2, std::int32_t y2, const float* rgba, std::int32_t mode);
using SetSoundVolumeFn   = void(__fastcall*)(std::uint64_t soundId, std::int32_t volume);

// The game's sprite object (16 bytes) and the draw settings its minimap
// markers pass (0x0D6DB0 builds the same).
struct GameSprite {
    void* hd{};
    void* legacy{};
};
struct SpriteDrawParams {
    float        tint[4]{ 1.0f, 1.0f, 1.0f, 0.0f };  // +0  RGB, 1 = untinted; +12 stays 0
    float        fade{ 1.0f };                       // +16
    std::int32_t mode{};                             // +20
    std::int32_t frame{};                            // +24
    std::uint8_t rest[140]{};                        // +28: flags and source rectangle, all zero
};
static_assert(sizeof(SpriteDrawParams) == 168, "the draw reads up to +0xA4");
struct NativePoint {
    std::int32_t x;
    std::int32_t y;
};
using AutomapUnitFn      = std::int64_t(__fastcall*)(void* unit, void* context) noexcept;
using UnitSelectTestFn   = std::int64_t(__fastcall*)(void* unit, std::int32_t x, std::int32_t y, std::int32_t mode, std::uint32_t skill) noexcept;
using TargetSelectTestFn = std::uint64_t(__fastcall*)(void* controller, void* player, void* unit) noexcept;
using GameToggleFn       = std::uint64_t(__fastcall*)(std::uint8_t toggle) noexcept;
using SpriteDrawHdFn     = std::uint64_t(__fastcall*)(void* hdSprite, const std::int32_t* position, float scale, const SpriteDrawParams* params);
struct OverlayRequest {
    void* unit;
    void* node;
};
using OverlayPostFn      = std::uint64_t(__fastcall*)(void* layer, OverlayRequest* request) noexcept;
using UnitRemovedFn      = std::uint64_t(__fastcall*)(void* layer, void** request) noexcept;
using OverlayPathFn      = void*(__fastcall*)(void* table, void* outString, const std::uint8_t* node) noexcept;
using GameStringFn       = void*(__fastcall*)(void* out, const char* text) noexcept;
using ProjectAutomapFn   = NativePoint*(__fastcall*)(void* context, NativePoint* output, std::uint64_t packedXY) noexcept;
using UnitCoordinateFn   = std::uint32_t(__fastcall*)(void* unit) noexcept;
using SpriteLoadFn       = std::uint64_t(__fastcall*)(GameSprite* sprite, const char* name, std::uint8_t flags);
using SpriteDrawFn       = std::uint64_t(__fastcall*)(GameSprite* sprite, const std::int32_t* position, const SpriteDrawParams* params);
using LabelColorFn       = std::uint32_t(__fastcall*)(void* item) noexcept;
using NameColorFn        = std::uint32_t(__fastcall*)(void* item, const std::uint8_t* itemsRecord, std::int32_t quality) noexcept;

using GetItemNameFn      = void*(__fastcall*)(void* item, char* buffer);
using LabelTextFn        = void(__fastcall*)(void* unit, char* text, std::int32_t size, std::uint32_t* color) noexcept;
using ShowLabelFn        = bool(__fastcall*)(void* filter, void* item) noexcept;
using DrawLabelFn        = std::uint64_t(__fastcall*)(void* record, const char* text, const float* rgba) noexcept;
using GroundActionFn     = std::int64_t(__fastcall*)(const std::uint8_t* packet, std::uint64_t action) noexcept;

ReadStatFn         ReadStat{};
LocalIndexFn       LocalIndex{};
LocalPlayerFn      LocalPlayer{};
ClientUnitFn       ClientUnit{};
PlaySoundFn        NativePlaySound{};
ColorizeFn         NativeColorize{};
PrintChatFn        NativePrintChat{};
TextLocaleFn       TextLocale{};
LocaleNameFn       LocaleName{};
GetItemDataFn      GetItemData{};
GetItemCodeFn      GetItemCode{};
CheckItemTypeFn    CheckItemType{};
MaxSocketsFn       NativeMaxSockets{};
ItemContextFn      ItemContext{};
ItemsRecordFn      ItemsRecord{};
ParentInventoryFn  ParentInventory{};
InventoryOwnerFn   InventoryOwner{};
UnitRoomFn         UnitRoom{};
RoomDataFn         RoomData{};
RoomLevelIdFn      RoomLevelId{};
LabelScreenSpaceFn LabelScreenSpace{};
ProjectPointFn     ProjectPoint{};
FillRectFn         FillRect{};
SetSoundVolumeFn   NativeSetSoundVolume{};
ProjectAutomapFn   ProjectAutomap{};
UnitCoordinateFn   UnitClientX{};
UnitCoordinateFn   UnitClientY{};
SpriteLoadFn       SpriteLoad{};
SpriteDrawFn       SpriteDraw{};
SpriteDrawHdFn     SpriteDrawHd{};
NameColorFn        NativeNameColor{};    // called directly only when its bytes were verified
bool               NameColorVerified{};

GetItemNameFn OriginalGetItemName{};
LabelTextFn   OriginalLabelText{};
ShowLabelFn   OriginalShowLabel{};
DrawLabelFn   OriginalDrawLabel{};
GroundActionFn OriginalNewDrop{};
GroundActionFn OriginalHandDrop{};
GroundActionFn OriginalSeenOnGround{};
LabelColorFn  OriginalLabelColor{};
NameColorFn   OriginalNameColor{};
AutomapUnitFn OriginalAutomapUnit{};
UnitSelectTestFn   OriginalUnitSelectTest{};
TargetSelectTestFn OriginalTargetSelectTest{};
GameToggleFn       NativeGameToggle{};
OverlayPostFn  OriginalOverlayAdded{};
UnitRemovedFn  OriginalUnitRemoved{};
OverlayPathFn  OriginalOverlayPath{};
OverlayPostFn  NativeOverlayRemoved{};
GameStringFn   NativeGameString{};
void PrepareDroplights(const FilterProgram* program);

template <typename T>
auto At(std::uint64_t rva) noexcept -> T {
    return reinterpret_cast<T>(Base + static_cast<std::uintptr_t>(rva));
}

template <typename T>
auto ReadAt(const std::uint8_t* base, std::size_t offset) noexcept -> T {
    T value{};
    std::memcpy(&value, base + offset, sizeof(T));
    return value;
}

void BindNatives() noexcept {
    ReadStat         = At<ReadStatFn>(ReadStatRva);
    LocalIndex       = At<LocalIndexFn>(LocalIndexRva);
    LocalPlayer      = At<LocalPlayerFn>(LocalPlayerRva);
    ClientUnit       = At<ClientUnitFn>(ClientUnitRva);
    NativePlaySound  = At<PlaySoundFn>(PlaySoundRva);
    NativeColorize   = At<ColorizeFn>(ColorizeRva);
    NativePrintChat  = At<PrintChatFn>(PrintChatRva);
    TextLocale       = At<TextLocaleFn>(TextLocaleRva);
    LocaleName       = At<LocaleNameFn>(LocaleNameRva);
    GetItemData      = At<GetItemDataFn>(GetItemDataRva);
    GetItemCode      = At<GetItemCodeFn>(GetItemCodeRva);
    CheckItemType    = At<CheckItemTypeFn>(CheckItemTypeRva);
    NativeMaxSockets = At<MaxSocketsFn>(MaxSocketsRva);
    ItemContext      = At<ItemContextFn>(ItemContextRva);
    ItemsRecord      = At<ItemsRecordFn>(ItemsRecordRva);
    ParentInventory  = At<ParentInventoryFn>(ParentInventoryRva);
    InventoryOwner   = At<InventoryOwnerFn>(InventoryOwnerRva);
    UnitRoom         = At<UnitRoomFn>(UnitRoomRva);
    RoomData         = At<RoomDataFn>(RoomDataRva);
    RoomLevelId      = At<RoomLevelIdFn>(RoomLevelIdRva);
    LabelScreenSpace = At<LabelScreenSpaceFn>(LabelScreenSpaceRva);
    ProjectPoint     = At<ProjectPointFn>(ProjectPointRva);
    FillRect         = At<FillRectFn>(FillRectRva);
    NativeNameColor  = At<NameColorFn>(NameColorRva);
}

// ---------------------------------------------------------------------------
//  State
// ---------------------------------------------------------------------------
enum class PluginState : std::uint8_t { Loading, Active, DisabledByConfig, UnsupportedBuild, InstallFailed };
PluginState State{ PluginState::Loading };

struct Installed {
    bool names{};
    bool goldLabels{};
    bool hide{};
    bool labelColors{};
    bool alerts{};
    bool qualityColors{};
    bool setTooltipColor{};
    bool soundVolume{};
    bool icons{};
    bool droplights{};
    bool pickup{};
};
Installed Hooks;

std::atomic<bool> Active{ false };
std::atomic<bool> ShowHidden{ false };

const D2RL::ThreadService*    ThreadSvc{};
const D2RL::OverlayService*   OverlaySvc{};
D2RL::Overlay::CallbackHandle FrameHandle{ D2RL::Overlay::InvalidCallbackHandle };
std::atomic<std::uint64_t>    FrameCount{};  // one per frame, from the SDK's frame callback
const D2RL::InputService*     InputSvc{};
const D2RL::LifecycleService* LifecycleSvc{};
const D2RL::LocalizationService* LocalizationSvc{};  // optional: {string:KEY} shows the key without it
const D2RL::ResourceService*     ResourceSvc{};      // optional: no minimap icons without it

std::wstring UserFolder;
std::wstring ShippedFolder;
std::wstring ExcelFolder;

struct Selection {
    std::string                          activeFilter;  // "shipped:x.json", "user:x.json" or ""
    std::unordered_map<std::string, int> levels;
    bool                                 playSounds{ true };
    bool                                 fromFile{};
};

struct LoadedFilter {
    std::shared_ptr<const FilterProgram> program;
    std::string                          id;
    std::string                          title;
    int                                  level{};
    std::size_t                          errors{};
    std::size_t                          warnings{};
    bool                                 loadFailed{};
    std::string                          failure;
};

std::shared_mutex   FilterLock;
LoadedFilter        Current;
Selection           CurrentSelection;
std::atomic<std::uint64_t> Revision{ 1 };
std::mutex          ReloadLock;

// ---------------------------------------------------------------------------
//  Folders and files
// ---------------------------------------------------------------------------
// FOLDERID_SavedGames {4C5C32FF-BB9D-43B0-B5B4-2D72E54EAAA4}, spelled out so no
// GUID library is needed.
constexpr GUID SavedGamesFolderId{ 0x4C5C32FF, 0xBB9D, 0x43B0, { 0xB5, 0xB4, 0x2D, 0x72, 0xE5, 0x4E, 0xAA, 0xA4 } };

auto SavedGamesFolder() -> std::wstring {
    PWSTR path = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(SavedGamesFolderId, 0, nullptr, &path)) && path != nullptr) result = path;
    if (path != nullptr) CoTaskMemFree(path);
    return result;
}

void ResolveFolders() {
    const std::string mod = Context->activeMod != nullptr ? Context->activeMod : "";
    if (!Config.userFolder.empty()) {
        UserFolder = Config.userFolder;
    } else {
        const std::wstring saved = SavedGamesFolder();
        if (!saved.empty()) {
            UserFolder = saved + L"\\Diablo II Resurrected\\";
            if (!mod.empty()) UserFolder += L"mods\\" + Widen(mod) + L"\\";
            UserFolder += L"LootFilters";
        }
    }
    // modDirectory is the mod's data folder, <game>\mods\<mod>\<mod>.mpq; the
    // mod's own filters sit next to it in <game>\mods\<mod>\LootFilters, where
    // the launcher lists them.
    const std::wstring modData = Context->modDirectory != nullptr ? Context->modDirectory : L"";
    std::wstring modRoot = modData;
    while (!modRoot.empty() && (modRoot.back() == L'\\' || modRoot.back() == L'/')) modRoot.pop_back();
    if (modRoot.size() > 4 && _wcsicmp(modRoot.c_str() + modRoot.size() - 4, L".mpq") == 0) {
        const std::size_t slash = modRoot.find_last_of(L"\\/");
        modRoot = slash == std::wstring::npos ? std::wstring() : modRoot.substr(0, slash);
    }
    if (modRoot.empty() && Context->scopeRootDirectory != nullptr) modRoot = Context->scopeRootDirectory;
    if (!modRoot.empty()) ShippedFolder = modRoot + L"\\LootFilters";
    const auto isFolder = [](const std::wstring& path) {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    };
    if (!modData.empty() && isFolder(modData + L"\\data\\global\\excel")) {
        ExcelFolder = modData + L"\\data\\global\\excel";
    } else if (!modRoot.empty()) {
        ExcelFolder = modRoot + L"\\" + Widen(mod) + L".mpq\\data\\global\\excel";
    }
    if (!UserFolder.empty()) {
        std::error_code ignored;
        CreateDirectoryW(UserFolder.c_str(), nullptr);
        if (GetFileAttributesW(UserFolder.c_str()) == INVALID_FILE_ATTRIBUTES) {
            // Create the whole chain.
            std::wstring partial;
            for (std::size_t i = 0; i < UserFolder.size(); ++i) {
                partial.push_back(UserFolder[i]);
                if (UserFolder[i] == L'\\' && i > 2) CreateDirectoryW(partial.c_str(), nullptr);
            }
            CreateDirectoryW(UserFolder.c_str(), nullptr);
        }
    }
}

auto ReadWholeFile(const std::wstring& path, std::string& out) -> bool {
    out.clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) != 0 && size.QuadPart >= 0 && size.QuadPart < 32 * 1024 * 1024;
    if (ok) {
        out.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD read = 0;
        ok = out.empty() || (ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) != 0 && read == out.size());
    }
    CloseHandle(file);
    return ok;
}

auto WriteWholeFile(const std::wstring& path, const std::string& text) -> bool {
    const std::wstring temporary = path + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) != 0 && written == text.size();
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

auto FileStamp(const std::wstring& path) -> std::uint64_t {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32 | data.ftLastWriteTime.dwLowDateTime)
         ^ (static_cast<std::uint64_t>(data.nFileSizeHigh) << 48) ^ data.nFileSizeLow;
}

auto SelectionPath() -> std::wstring { return UserFolder.empty() ? std::wstring() : UserFolder + L"\\selection.json"; }

// "shipped:name.json" / "user:name.json" -> full path and folder. False for "" or a bad id.
auto FilterPathOf(const std::string& id, std::wstring& path, std::wstring& folder) -> bool {
    const std::size_t colon = id.find(':');
    if (colon == std::string::npos) return false;
    const std::string where = id.substr(0, colon);
    const std::string file  = id.substr(colon + 1);
    if (file.empty() || file.find_first_of("\\/:") != std::string::npos || file.find("..") != std::string::npos) return false;
    if (where == "shipped") folder = ShippedFolder;
    else if (where == "user") folder = UserFolder;
    else return false;
    if (folder.empty()) return false;
    path = folder + L"\\" + Widen(file);
    return true;
}

auto ReadSelection() -> Selection {
    Selection selection;
    std::string text;
    if (!ReadWholeFile(SelectionPath(), text)) {
        if (!Config.defaultFilter.empty()) selection.activeFilter = "shipped:" + Config.defaultFilter;
        return selection;
    }
    json::Value root;
    json::ParseError error;
    if (!json::Parse(text, root, error) || !root.IsObject()) {
        LogWarn("LootFilter: selection.json is not valid (line %u: %s); the default filter is used.", error.line, error.message.c_str());
        if (!Config.defaultFilter.empty()) selection.activeFilter = "shipped:" + Config.defaultFilter;
        return selection;
    }
    selection.fromFile = true;
    if (const json::Value* active = root.Find("activeFilter"); active != nullptr && active->IsString()) {
        selection.activeFilter = active->string;
    }
    if (const json::Value* levels = root.Find("levels"); levels != nullptr && levels->IsObject()) {
        for (const json::Member& member : levels->object) {
            if (member.value.IsInteger()) selection.levels[member.key] = static_cast<int>(member.value.integer);
        }
    }
    if (const json::Value* sounds = root.Find("playSounds"); sounds != nullptr && sounds->IsBool()) {
        selection.playSounds = sounds->boolean;
    }
    return selection;
}

auto EscapeJson(const std::string& text) -> std::string {
    std::string out;
    for (char c : text) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (static_cast<unsigned char>(c) < 0x20) {
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
            out += buffer;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// Same shape the launcher writes. Only the level of the active filter changes here.
auto WriteSelection(const Selection& selection) -> bool {
    std::string text = "{\n  \"formatVersion\": 1,\n  \"activeFilter\": \"" + EscapeJson(selection.activeFilter) + "\",\n  \"levels\": {";
    bool first = true;
    std::vector<std::pair<std::string, int>> levels(selection.levels.begin(), selection.levels.end());
    std::sort(levels.begin(), levels.end());
    for (const auto& [id, level] : levels) {
        text += first ? "\n" : ",\n";
        text += "    \"" + EscapeJson(id) + "\": " + std::to_string(level);
        first = false;
    }
    text += first ? "},\n" : "\n  },\n";
    text += std::string("  \"playSounds\": ") + (selection.playSounds ? "true" : "false") + "\n}\n";
    return WriteWholeFile(SelectionPath(), text);
}

// ---------------------------------------------------------------------------
//  Names from the mod's txt files
// ---------------------------------------------------------------------------
class TxtNames {
public:
    // Maps the text in `column` to its row number in the compiled table.
    auto Load(const std::wstring& path, const char* column) -> bool {
        names_.clear();
        std::string text;
        if (!ReadWholeFile(path, text)) return false;
        std::size_t lineStart = 0;
        int columnIndex = -1;
        int row = -1;
        while (lineStart < text.size()) {
            std::size_t lineEnd = text.find('\n', lineStart);
            if (lineEnd == std::string::npos) lineEnd = text.size();
            std::string_view line(text.data() + lineStart, lineEnd - lineStart);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            lineStart = lineEnd + 1;
            // D2RLoader's txt compiler skips "Expansion" separator rows, so they
            // take no row number (itemtypes.txt has one before Jewel).
            if (columnIndex >= 0 && line.substr(0, line.find('\t')) == "Expansion") continue;
            int index = 0;
            std::size_t cellStart = 0;
            std::string_view cell;
            bool found = false;
            while (true) {
                const std::size_t tab = line.find('\t', cellStart);
                const std::string_view current = line.substr(cellStart, tab == std::string_view::npos ? std::string_view::npos : tab - cellStart);
                if (columnIndex < 0) {
                    if (current == column) {
                        columnIndex = index;
                        break;
                    }
                } else if (index == columnIndex) {
                    cell = current;
                    found = true;
                    break;
                }
                if (tab == std::string_view::npos) break;
                cellStart = tab + 1;
                ++index;
            }
            if (columnIndex < 0) return false;  // the header must name the column
            if (row >= 0 && found && !cell.empty()) names_.emplace(std::string(cell), row);
            ++row;
        }
        return true;
    }

    [[nodiscard]] auto Find(std::string_view name) const -> int {
        const auto it = names_.find(std::string(name));
        return it == names_.end() ? -1 : it->second;
    }

private:
    std::unordered_map<std::string, int> names_;
};

// ---------------------------------------------------------------------------
//  Minimap icon files: SpA1 sprites, resized and handed to the game
// ---------------------------------------------------------------------------
// SpA1 is the game's HD sprite: a 40-byte header (magic, version 31, frame
// width, atlas width, height, frame count at +20, pixel byte count at +32,
// 4 at +36), then RGBA rows of the whole atlas, frames side by side.
struct SpriteImage {
    int                       width{};
    int                       height{};
    std::vector<std::uint8_t> rgba;
};

auto ReadSpriteFile(const std::wstring& path, SpriteImage& image, std::string& error) -> bool {
    std::string bytes;
    if (!ReadWholeFile(path, bytes)) {
        error = "the file is missing or unreadable";
        return false;
    }
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
    if (bytes.size() < 40 || std::memcmp(data, "SpA1", 4) != 0 || ReadAt<std::uint16_t>(data, 4) != 31) {
        error = "it is not a D2R sprite (SpA1, version 31)";
        return false;
    }
    const std::uint32_t atlasWidth = ReadAt<std::uint32_t>(data, 8);
    const std::uint32_t height     = ReadAt<std::uint32_t>(data, 12);
    const std::uint32_t frames     = ReadAt<std::uint32_t>(data, 20);
    if (atlasWidth == 0 || height == 0 || frames == 0 || atlasWidth % frames != 0 || atlasWidth > 8192 || height > 4096
        || bytes.size() < 40 + static_cast<std::size_t>(atlasWidth) * height * 4) {
        error = "its header does not match its pixels";
        return false;
    }
    // Frame 0 only.
    image.width  = static_cast<int>(atlasWidth / frames);
    image.height = static_cast<int>(height);
    image.rgba.resize(static_cast<std::size_t>(image.width) * image.height * 4);
    for (int y = 0; y < image.height; ++y) {
        std::memcpy(image.rgba.data() + static_cast<std::size_t>(y) * image.width * 4,
                    data + 40 + static_cast<std::size_t>(y) * atlasWidth * 4, static_cast<std::size_t>(image.width) * 4);
    }
    return true;
}

// One axis of a resize, on premultiplied RGBA floats: area averaging when
// shrinking, linear when growing.
void ResizeAxis(const std::vector<float>& source, int sourceLength, int lines, bool horizontal, std::vector<float>& out, int length) {
    out.assign(static_cast<std::size_t>(length) * lines * 4, 0.0f);
    const double scale = static_cast<double>(sourceLength) / length;
    const auto at = [&](int along, int line, int axisLength) -> std::size_t {
        return horizontal ? (static_cast<std::size_t>(line) * axisLength + along) * 4 : (static_cast<std::size_t>(along) * lines + line) * 4;
    };
    for (int line = 0; line < lines; ++line) {
        for (int i = 0; i < length; ++i) {
            float sum[4]{};
            if (length < sourceLength) {
                const double from = i * scale;
                const double to   = (i + 1) * scale;
                for (int j = static_cast<int>(from); j < sourceLength && j < to; ++j) {
                    const double cover = std::min<double>(to, j + 1) - std::max<double>(from, j);
                    if (cover <= 0.0) continue;
                    const std::size_t k = at(j, line, sourceLength);
                    for (int c = 0; c < 4; ++c) sum[c] += static_cast<float>(source[k + c] * cover);
                }
                for (float& value : sum) value = static_cast<float>(value / scale);
            } else {
                const double center = (i + 0.5) * scale - 0.5;
                const int    j0     = std::clamp(static_cast<int>(std::floor(center)), 0, sourceLength - 1);
                const int    j1     = std::min(j0 + 1, sourceLength - 1);
                const float  t      = static_cast<float>(std::clamp(center - std::floor(center), 0.0, 1.0));
                const std::size_t k0 = at(j0, line, sourceLength);
                const std::size_t k1 = at(j1, line, sourceLength);
                for (int c = 0; c < 4; ++c) sum[c] = source[k0 + c] * (1.0f - t) + source[k1 + c] * t;
            }
            const std::size_t k = at(i, line, length);
            for (int c = 0; c < 4; ++c) out[k + c] = sum[c];
        }
    }
}

auto Resize(const SpriteImage& source, int width, int height) -> SpriteImage {
    std::vector<float> premultiplied(source.rgba.size());
    for (std::size_t i = 0; i < source.rgba.size(); i += 4) {
        const float alpha = source.rgba[i + 3] / 255.0f;
        for (int c = 0; c < 3; ++c) premultiplied[i + c] = source.rgba[i + c] / 255.0f * alpha;
        premultiplied[i + 3] = alpha;
    }
    std::vector<float> wide;
    std::vector<float> done;
    ResizeAxis(premultiplied, source.width, source.height, true, wide, width);  // rows: width x source.height
    ResizeAxis(wide, source.height, width, false, done, height);               // columns: width x height
    SpriteImage image;
    image.width  = width;
    image.height = height;
    image.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
        const float alpha = std::clamp(done[i + 3], 0.0f, 1.0f);
        for (int c = 0; c < 3; ++c) {
            const float value = alpha > 0.0f ? done[i + c] / alpha : 0.0f;
            image.rgba[i + c] = static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
        }
        image.rgba[i + 3] = static_cast<std::uint8_t>(std::lround(alpha * 255.0f));
    }
    return image;
}

auto SpriteBytes(const SpriteImage& image) -> std::vector<std::uint8_t> {
    std::vector<std::uint8_t> bytes(40 + image.rgba.size(), 0);
    const auto put16 = [&](std::size_t offset, std::uint16_t value) { std::memcpy(bytes.data() + offset, &value, sizeof(value)); };
    const auto put32 = [&](std::size_t offset, std::uint32_t value) { std::memcpy(bytes.data() + offset, &value, sizeof(value)); };
    std::memcpy(bytes.data(), "SpA1", 4);
    put16(4, 31);
    put16(6, static_cast<std::uint16_t>(image.width));
    put32(8, static_cast<std::uint32_t>(image.width));
    put32(12, static_cast<std::uint32_t>(image.height));
    put32(20, 1);
    put32(32, static_cast<std::uint32_t>(image.rgba.size()));
    put32(36, 4);
    std::memcpy(bytes.data() + 40, image.rgba.data(), image.rgba.size());
    return bytes;
}

std::mutex                                     IconFilesLock;
std::unordered_map<std::string, bool>          IconFilesRegistered;  // sprite name -> registered
std::vector<D2RL::Resources::RegistrationHandle> IconHandles;

auto RegisterSprite(const std::string& path, const std::vector<std::uint8_t>& bytes) -> bool {
    const D2RL::Resources::ResourceRegistration registration{
        .structSize = D2RL::Resources::ResourceRegistrationSize,
        .flags      = 0,
        .path       = path.c_str(),
        .bytes      = bytes.data(),
        .byteCount  = bytes.size(),
    };
    D2RL::Resources::RegistrationHandle handle = D2RL::Resources::InvalidHandle;
    const D2RL::Resources::Result result = ResourceSvc->registerResource(Context, &registration, &handle);
    if (result != D2RL::Resources::Result::Success) {
        LogWarn("LootFilter: the icon sprite %s could not be registered (result %u).", path.c_str(), static_cast<unsigned>(result));
        return false;
    }
    IconHandles.push_back(handle);
    return true;
}

// Reads <icons>\<name>.sprite, resizes it to the icon's size and hands the
// game that copy (and a half-size low-end copy). The name the game loads it
// by carries a hash of the pixels, so an edited icon is a new sprite.
auto PrepareIcon(const std::wstring& iconsFolder, IconSpec& icon) -> bool {
    if (ResourceSvc == nullptr) return false;
    SpriteImage source;
    std::string error;
    if (!ReadSpriteFile(iconsFolder + L"\\" + Widen(icon.name) + L".sprite", source, error)) {
        LogWarn("LootFilter: icon \"%s\": %s.", icon.name.c_str(), error.c_str());
        return false;
    }
    int width  = source.width;
    int height = source.height;
    if (icon.size > 0) {
        if (width >= height) {
            height = std::max(1, static_cast<int>(std::lround(static_cast<double>(height) * icon.size / width)));
            width  = icon.size;
        } else {
            width  = std::max(1, static_cast<int>(std::lround(static_cast<double>(width) * icon.size / height)));
            height = icon.size;
        }
    }
    const SpriteImage full = width == source.width && height == source.height ? source : Resize(source, width, height);
    const SpriteImage low  = Resize(source, std::max(1, width / 2), std::max(1, height / 2));
    const std::vector<std::uint8_t> fullBytes = SpriteBytes(full);
    std::uint32_t hash = 2166136261U;
    for (std::uint8_t byte : fullBytes) hash = (hash ^ byte) * 16777619U;
    std::string key;
    for (char c : icon.name) {
        const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        key.push_back((lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9') || lower == '-' ? lower : '_');
    }
    char suffix[48];
    std::snprintf(suffix, sizeof(suffix), "-%dx%d-%08x", width, height, hash);
    const std::string spriteName = std::string("d2rloader/") + PluginIdText + "/icons/" + key + suffix;
    {
        std::lock_guard<std::mutex> lock(IconFilesLock);
        auto [entry, added] = IconFilesRegistered.try_emplace(spriteName, false);
        if (added) {
            const std::string path = "data/hd/global/ui/" + spriteName;
            entry->second = RegisterSprite(path + ".sprite", fullBytes) && RegisterSprite(path + ".lowend.sprite", SpriteBytes(low));
        }
        if (!entry->second) return false;
    }
    icon.spriteName = spriteName;
    icon.width      = width;
    icon.height     = height;
    return true;
}

void UnregisterIcons() noexcept {
    std::lock_guard<std::mutex> lock(IconFilesLock);
    if (ResourceSvc != nullptr) {
        for (D2RL::Resources::RegistrationHandle handle : IconHandles) (void)ResourceSvc->unregisterResource(Context, handle);
    }
    IconHandles.clear();
    IconFilesRegistered.clear();
}

class FilterNames final : public NameResolver {
public:
    FilterNames(std::wstring soundsFolder, std::wstring iconsFolder)
        : soundsFolder_(std::move(soundsFolder)), iconsFolder_(std::move(iconsFolder)) {}

    auto ItemTypeRow(std::string_view code) -> int override {
        if (!typesLoaded_) {
            typesLoaded_ = true;
            if (!types_.Load(ExcelFolder + L"\\itemtypes.txt", "Code")) LogWarn("LootFilter: itemtypes.txt could not be read from the mod.");
        }
        return types_.Find(code);
    }
    auto StatId(std::string_view name) -> int override {
        if (!statsLoaded_) {
            statsLoaded_ = true;
            if (!stats_.Load(ExcelFolder + L"\\itemstatcost.txt", "Stat")) LogWarn("LootFilter: itemstatcost.txt could not be read from the mod.");
        }
        return stats_.Find(name);
    }
    auto SoundRow(std::string_view name) -> int override {
        if (!soundsLoaded_) {
            soundsLoaded_ = true;
            if (!sounds_.Load(ExcelFolder + L"\\sounds.txt", "Sound")) LogWarn("LootFilter: sounds.txt could not be read from the mod.");
        }
        return sounds_.Find(name);
    }
    auto SoundFile(std::string_view fileName, std::wstring& fullPath) -> bool override {
        fullPath = soundsFolder_ + L"\\" + Widen(fileName);
        const DWORD attributes = GetFileAttributesW(fullPath.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }
    auto IconFile(IconSpec& icon) -> bool override {
        try {
            return PrepareIcon(iconsFolder_, icon);
        } catch (...) {
            return false;
        }
    }

private:
    std::wstring soundsFolder_;
    std::wstring iconsFolder_;
    TxtNames     types_;
    TxtNames     stats_;
    TxtNames     sounds_;
    bool         typesLoaded_{};
    bool         statsLoaded_{};
    bool         soundsLoaded_{};
};

// ---------------------------------------------------------------------------
//  {string:KEY}: the game's own text in the game's language
// ---------------------------------------------------------------------------
class GameStrings final : public StringSource {
public:
    auto GameString(std::string_view key, std::string& text) const -> bool override {
        if (LocalizationSvc == nullptr || key.empty() || key.size() > 256) return false;
        std::lock_guard<std::mutex> lock(lock_);
        const std::string name(key);
        if (const auto found = cache_.find(name); found != cache_.end()) {
            if (!found->second) return false;
            text = *found->second;
            return true;
        }
        // Game text is reached through the d2r: namespace (see the SDK's LOCALIZATION.md).
        const std::string qualified = "d2r:" + name;
        std::string buffer(256, '\0');
        std::uint32_t required = 0;
        D2RL::Localization::Result result = LocalizationSvc->getStringByKey(
            Context, qualified.c_str(), buffer.data(), static_cast<std::uint32_t>(buffer.size()), &required);
        if (result == D2RL::Localization::Result::BufferTooSmall && required > buffer.size() && required < 65536) {
            buffer.assign(required, '\0');
            result = LocalizationSvc->getStringByKey(Context, qualified.c_str(), buffer.data(), required, &required);
        }
        if (cache_.size() > 4096) cache_.clear();
        if (result != D2RL::Localization::Result::Success) {
            cache_.emplace(name, std::nullopt);
            return false;
        }
        buffer.resize(std::strlen(buffer.c_str()));
        cache_.emplace(name, buffer);
        text = std::move(buffer);
        return true;
    }

    // The language or the strings changed.
    void Clear() {
        std::lock_guard<std::mutex> lock(lock_);
        cache_.clear();
    }

private:
    mutable std::mutex                                                     lock_;
    mutable std::unordered_map<std::string, std::optional<std::string>>   cache_;
};
GameStrings Strings;

// ---------------------------------------------------------------------------
//  qualityColors: the active filter's name colors, read by the color hooks
// ---------------------------------------------------------------------------
std::array<std::atomic<char>, 10> QualityColorCodes{};  // index = quality 1..9, '\0' = the game's color

// The color each color picker gives a plain item of each quality (index 1..9).
constexpr std::array<std::uint32_t, 10> LabelPlainColors{ 0, 13, 13, 13, 18, 19, 15, 20, 16, 17 };
constexpr std::array<std::uint32_t, 10> TooltipPlainColors{ 0, 0, 0, 0, 3, 2, 9, 4, 8, 10 };

void PublishQualityColors(const FilterProgram* program) noexcept {
    for (int quality = 1; quality <= 9; ++quality) {
        QualityColorCodes[static_cast<std::size_t>(quality)].store(program != nullptr ? program->qualityColors.For(quality) : '\0');
    }
}

// The filter's color in place of the game's, when the game gave the plain color
// for that quality. Anything else (runes, quest items, socketed or ethereal
// grey, broken red) keeps the game's color.
auto FilterColor(int quality, std::uint32_t gameColor, const std::array<std::uint32_t, 10>& plain) noexcept -> std::uint32_t {
    if (quality < 1 || quality > 9 || gameColor != plain[static_cast<std::size_t>(quality)]) return gameColor;
    const char code = QualityColorCodes[static_cast<std::size_t>(quality)].load(std::memory_order_relaxed);
    return code == '\0' ? gameColor : static_cast<std::uint32_t>(code - '0');
}

// ---------------------------------------------------------------------------
//  Chat lines and sounds, delivered on the UI thread
// ---------------------------------------------------------------------------
struct PendingOutput {
    std::vector<std::string>         chat;
    std::vector<std::pair<int, int>> tableSounds;  // sounds.txt row, volume 0-255
};
std::mutex        OutputLock;
PendingOutput     Pending;
std::atomic<bool> FlushQueued{ false };
std::mutex        MarkerLock;
std::string       ColorMarker;

auto LocalPlayerUnit() noexcept -> void* {
    if (LocalIndex == nullptr || LocalPlayer == nullptr) return nullptr;
    return LocalPlayer(LocalIndex());
}

// The game's color marker, read once from the game's own colouring function.
auto Marker() -> std::string {
    std::lock_guard<std::mutex> lock(MarkerLock);
    if (ColorMarker.empty() && NativeColorize != nullptr) {
        char buffer[1024]{ 'x', '\0' };
        NativeColorize(buffer, 0);
        const std::size_t length = std::strlen(buffer);
        if (length > 2 && buffer[length - 1] == 'x' && buffer[length - 2] == '0') ColorMarker.assign(buffer, length - 2);
    }
    return ColorMarker;
}

auto GameText(const std::string& text) -> std::string {
    const std::string marker = Marker();
    if (marker.empty()) return text;
    static const std::string code = "\xC3\xBF" "c";
    std::string out;
    out.reserve(text.size());
    std::size_t pos = 0;
    while (true) {
        const std::size_t found = text.find(code, pos);
        if (found == std::string::npos) {
            out.append(text, pos, std::string::npos);
            break;
        }
        out.append(text, pos, found - pos);
        out += marker;
        pos = found + code.size();
    }
    return out;
}

// Cuts at most `limit` bytes without splitting a UTF-8 character.
void CopyLimited(char* destination, std::size_t capacity, const std::string& text) {
    if (capacity == 0) return;
    std::size_t length = std::min(text.size(), capacity - 1);
    while (length > 0 && length < text.size() && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80) --length;
    std::memcpy(destination, text.data(), length);
    destination[length] = '\0';
}

void __cdecl FlushOutput(const D2RL::PluginContext*, void*) noexcept {
    FlushQueued.store(false);
    PendingOutput output;
    {
        std::lock_guard<std::mutex> lock(OutputLock);
        std::swap(output, Pending);
    }
    if (!Active.load() || LocalPlayerUnit() == nullptr) {
        if (Config.debugLog && !output.chat.empty()) {
            LogInfo("LootFilter debug: %zu chat line(s) dropped: no player in game yet.", output.chat.size());
        }
        return;
    }
    try {
        for (const std::string& line : output.chat) {
            const std::string text = GameText(line);
            if (text.empty() || NativePrintChat == nullptr) continue;
            const GameString value{ text.c_str(), text.size(), text.size() | 0x8000000000000000ULL };
            NativePrintChat(&value);
            if (Config.debugLog) LogInfo("LootFilter debug: chat line printed: %s", line.c_str());
        }
        for (const auto& [row, volume] : output.tableSounds) {
            if (NativePlaySound == nullptr) continue;
            const std::uint64_t id = NativePlaySound(row, nullptr, 0, 0, 0);
            if (id != 0 && volume < 255 && NativeSetSoundVolume != nullptr) NativeSetSoundVolume(id, volume);
        }
    } catch (...) {
    }
}

void QueueFlush() noexcept {
    if (ThreadSvc == nullptr || FlushQueued.exchange(true)) return;
    if (ThreadSvc->runOnUiThread(Context, &FlushOutput, nullptr) != D2RL::Threads::Result::Success) FlushQueued.store(false);
}

void QueueChat(std::string line) {
    {
        std::lock_guard<std::mutex> lock(OutputLock);
        if (Pending.chat.size() < 64) Pending.chat.push_back(std::move(line));
    }
    QueueFlush();
}

// volume: 0-100, the rule's soundVolume (else the filter's, else 100).
void QueueSound(const SoundRef& sound, int volume) {
    volume = std::clamp(volume, 0, 100);
    if (volume == 0) return;
    if (sound.kind == SoundRef::Kind::TableRow && sound.row >= 0) {
        {
            std::lock_guard<std::mutex> lock(OutputLock);
            if (Pending.tableSounds.size() < 16) Pending.tableSounds.emplace_back(sound.row, (volume * 255 + 50) / 100);
        }
        QueueFlush();
    } else if (sound.kind == SoundRef::Kind::File && !sound.path.empty()) {
        sound::PlayFile(sound.path, Config.fileSoundVolume * volume / 100);
    }
}

// ---------------------------------------------------------------------------
//  Loading the active filter
// ---------------------------------------------------------------------------
auto LevelName(const FilterProgram& program, int level, std::string_view language) -> std::string {
    const Level* found = program.FindLevel(level);
    return found != nullptr ? ExpandPlainText(found->name.Pick(language), &Strings) : std::string();
}

auto CurrentLanguage() -> std::string {
    // Only called on game threads; the locale functions read globals the game set at startup.
    if (TextLocale == nullptr || LocaleName == nullptr) return "enUS";
    const std::int32_t index = TextLocale();
    if (index < 0 || index > 12) return "enUS";
    const char* code = LocaleName(index);
    return code != nullptr && std::strlen(code) == 4 ? std::string(code) : std::string("enUS");
}

// The game's text language as four packed characters, read when the player
// enters a game.
std::atomic<std::uint32_t> LanguagePacked{ 0x53556E65U };  // "enUS"

auto LanguageNow() -> std::string { return UnpackItemCode(LanguagePacked.load()); }

// The filter's levelChangeFormat (or the built-in line) for this level.
auto LevelChangeLine(const LoadedFilter& filter) -> std::string {
    const FilterProgram& program = *filter.program;
    const std::string language = LanguageNow();
    const Text& format = program.notifications.levelChangeFormat;
    const std::string text = format.Empty() ? std::string(DefaultLevelChangeFormat) : format.Pick(language);
    return ExpandPlainText(text, &Strings, LevelName(program, filter.level, language), filter.title.empty() ? filter.id : filter.title);
}

void Announce(const LoadedFilter& filter) {
    if (!Config.announce) return;
    if (filter.loadFailed) {
        QueueChat("\xC3\xBF" "c1Loot filter: " + filter.failure);
        return;
    }
    if (!filter.program) {
        QueueChat("\xC3\xBF" "c5Loot filter: none");
        return;
    }
    std::string line = "\xC3\xBF" "c4Loot filter: \xC3\xBF" "c0" + (filter.title.empty() ? filter.id : filter.title);
    const std::string level = LevelName(*filter.program, filter.level, LanguageNow());
    if (!level.empty()) line += " \xC3\xBF" "c5(" + level + ")";
    if (filter.errors > 0) line += " \xC3\xBF" "c1" + std::to_string(filter.errors) + " problems, see the plugin log";
    QueueChat(line);
}

// Reads the selection and the filter it names. Any thread.
void LoadActiveFilter(bool announce) {
    std::lock_guard<std::mutex> serial(ReloadLock);
    Selection selection = ReadSelection();
    LoadedFilter next;
    next.id = selection.activeFilter;
    if (!selection.activeFilter.empty() && selection.activeFilter != "none") {
        std::wstring path;
        std::wstring folder;
        std::string text;
        if (!FilterPathOf(selection.activeFilter, path, folder)) {
            next.loadFailed = true;
            next.failure = "\"" + selection.activeFilter + "\" is not a filter id";
        } else if (!ReadWholeFile(path, text)) {
            next.loadFailed = true;
            next.failure = "\"" + selection.activeFilter + "\" could not be read";
        } else {
            auto program = std::make_shared<FilterProgram>();
            std::vector<Problem> problems;
            const bool usable = CompileFilter(text, *program, problems);
            if (usable) {
                FilterNames names(folder + L"\\sounds", folder + L"\\icons");
                ResolveNames(*program, names, problems);
            }
            for (const Problem& problem : problems) {
                (problem.severity == Problem::Severity::Error ? ++next.errors : ++next.warnings);
            }
            std::size_t logged = 0;
            for (const Problem& problem : problems) {
                if (++logged > 200) {
                    LogWarn("LootFilter: %zu more problems not shown.", problems.size() - 200);
                    break;
                }
                const std::string line = "LootFilter: " + selection.activeFilter + ": " + problem.ToString();
                if (problem.severity == Problem::Severity::Error) LogError("%s", line.c_str());
                else LogWarn("%s", line.c_str());
            }
            if (!usable) {
                next.loadFailed = true;
                next.failure = "\"" + selection.activeFilter + "\" could not be loaded, see the plugin log";
            } else {
                const auto wanted = selection.levels.find(selection.activeFilter);
                next.level   = program->EffectiveLevel(wanted != selection.levels.end() ? wanted->second : program->defaultLevel);
                next.title   = program->title;
                next.program = std::move(program);
                LogInfo("LootFilter: loaded %s (%s): %zu of %zu rules active, level %d, %zu errors, %zu warnings.",
                    selection.activeFilter.c_str(), next.title.c_str(), next.program->EnabledRuleCount(), next.program->rules.size(),
                    next.level, next.errors, next.warnings);
            }
        }
    } else {
        LogInfo("LootFilter: no filter selected.");
    }
    if (next.loadFailed) LogError("LootFilter: %s.", next.failure.c_str());
    std::string previousId;
    int previousLevel = 0;
    bool hadProgram = false;
    {
        std::unique_lock<std::shared_mutex> lock(FilterLock);
        previousId       = Current.id;
        previousLevel    = Current.level;
        hadProgram       = static_cast<bool>(Current.program);
        Current          = next;
        CurrentSelection = std::move(selection);
    }
    PublishQualityColors(next.program.get());
    Strings.Clear();
    PrepareDroplights(next.program.get());
    Revision.fetch_add(1);
    if (!announce) return;
    // The launcher changed only the level of the same filter: say so like the level key does.
    if (hadProgram && next.program && previousId == next.id && previousLevel != next.level && next.program->HasLevels()) {
        QueueChat(LevelChangeLine(next));
    } else {
        Announce(next);
    }
}

auto Snapshot(LoadedFilter& filter, bool& playSounds) -> bool {
    std::shared_lock<std::shared_mutex> lock(FilterLock);
    filter     = Current;
    playSounds = CurrentSelection.playSounds;
    return static_cast<bool>(filter.program);
}

// ---------------------------------------------------------------------------
//  Folder watcher
// ---------------------------------------------------------------------------
std::thread       Watcher;
HANDLE            WatcherStop{};

void WatchFolders() noexcept {
    std::vector<HANDLE> handles{ WatcherStop };
    for (const std::wstring* folder : { &UserFolder, &ShippedFolder }) {
        if (folder->empty()) continue;
        HANDLE change = FindFirstChangeNotificationW(folder->c_str(), TRUE,
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE);
        if (change != INVALID_HANDLE_VALUE) handles.push_back(change);
    }
    const auto stamp = [] {
        std::uint64_t value = FileStamp(SelectionPath());
        std::string id;
        {
            std::shared_lock<std::shared_mutex> lock(FilterLock);
            id = CurrentSelection.activeFilter;
        }
        std::wstring path;
        std::wstring folder;
        if (FilterPathOf(id, path, folder)) value = value * 1099511628211ULL ^ FileStamp(path);
        return value;
    };
    std::uint64_t last = stamp();
    while (true) {
        const DWORD result = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, INFINITE);
        if (result == WAIT_OBJECT_0 || result == WAIT_FAILED) break;
        const std::size_t index = result - WAIT_OBJECT_0;
        if (index < handles.size()) FindNextChangeNotification(handles[index]);
        if (WaitForSingleObject(WatcherStop, 300) == WAIT_OBJECT_0) break;  // let the writer finish
        const std::uint64_t now = stamp();
        if (now == last) continue;
        last = now;
        try {
            LoadActiveFilter(true);
        } catch (...) {
            LogError("LootFilter: reloading the filter failed.");
        }
        last = stamp();
    }
    for (std::size_t i = 1; i < handles.size(); ++i) FindCloseChangeNotification(handles[i]);
}

// ---------------------------------------------------------------------------
//  Items as the engine sees them
// ---------------------------------------------------------------------------
class GamePlayer final : public PlayerSubject {
public:
    explicit GamePlayer(void* unit) : unit_(unit) {}
    [[nodiscard]] auto Stat(int stat, int param) const -> int override { return unit_ != nullptr ? ReadStat(unit_, stat, param) : 0; }
    [[nodiscard]] auto Difficulty() const -> int override {
        if (difficulty_ == -2) {
            difficulty_ = -1;
            if (void* room = unit_ != nullptr ? UnitRoom(unit_) : nullptr) {
                if (const std::uint8_t* data = RoomData(room)) {
                    if (const auto* level = ReadAt<const std::uint8_t*>(data, RoomLevelOffset)) {
                        if (const auto* drlg = ReadAt<const std::uint8_t*>(level, LevelDrlgOffset)) {
                            const std::uint8_t value = drlg[DrlgDifficultyOffset];
                            if (value <= 2) difficulty_ = value;
                        }
                    }
                }
            }
        }
        return difficulty_;
    }

private:
    void*       unit_;
    mutable int difficulty_{ -2 };
};

class GameItem final : public ItemSubject {
public:
    explicit GameItem(void* unit) : unit_(unit), data_(GetItemData(unit)) {}

    [[nodiscard]] auto Valid() const -> bool { return data_ != nullptr; }
    [[nodiscard]] auto Mode() const -> std::uint32_t { return ReadAt<std::uint32_t>(static_cast<const std::uint8_t*>(unit_), UnitModeOffset); }
    [[nodiscard]] auto Flags() const -> std::uint32_t { return ReadAt<std::uint32_t>(data_, ItemFlagsOffset); }

    [[nodiscard]] auto Code() const -> std::uint32_t override {
        if (!code_) code_ = GetItemCode(unit_);
        return *code_;
    }
    [[nodiscard]] auto Quality() const -> int override { return static_cast<int>(ReadAt<std::uint32_t>(data_, ItemQualityOffset)); }
    [[nodiscard]] auto BaseTier() const -> int override {
        if (!tier_) {
            tier_ = -1;
            const std::uint32_t classId = ReadAt<std::uint32_t>(static_cast<const std::uint8_t*>(unit_), UnitClassOffset);
            const std::uint8_t* record = ItemsRecord(ItemContext(unit_), classId);
            if (record != nullptr && ReadAt<std::uint32_t>(record, ItemsCodeOffset) == Code()) {
                const std::uint32_t code = Code();
                if (ReadAt<std::uint32_t>(record, ItemsUltraCodeOffset) == code) tier_ = 2;
                else if (ReadAt<std::uint32_t>(record, ItemsUberCodeOffset) == code) tier_ = 1;
                else if (ReadAt<std::uint32_t>(record, ItemsNormCodeOffset) == code) tier_ = 0;
            }
        }
        return *tier_;
    }
    [[nodiscard]] auto ItemLevel() const -> int override { return static_cast<int>(ReadAt<std::uint32_t>(data_, ItemLevelOffset)); }
    [[nodiscard]] auto Sockets() const -> int override { return ReadStat(unit_, 194, 0); }
    [[nodiscard]] auto MaxSockets() const -> int override {
        if (!maxSockets_) maxSockets_ = NativeMaxSockets(unit_) & 0xFF;
        return *maxSockets_;
    }
    [[nodiscard]] auto Ethereal() const -> bool override { return (Flags() & FlagEthereal) != 0; }
    [[nodiscard]] auto Identified() const -> bool override { return (Flags() & FlagIdentified) != 0; }
    [[nodiscard]] auto Runeword() const -> bool override { return (Flags() & FlagRuneword) != 0; }
    [[nodiscard]] auto UniqueOrSetId() const -> int override { return ReadAt<std::int32_t>(data_, ItemFileIndexOffset); }
    [[nodiscard]] auto Where() const -> Location override {
        if (!location_) {
            switch (Mode()) {
            case 3:
            case 5: location_ = Location::Ground; break;
            case 1: location_ = Location::Equipped; break;
            case 0:
            case 2: location_ = AtVendor() ? Location::Vendor : Location::Stored; break;
            default: location_ = Location::Other; break;
            }
        }
        return *location_;
    }
    [[nodiscard]] auto Area() const -> int override {
        if (!area_) {
            area_ = -1;
            if (void* room = UnitRoom(unit_)) {
                if (std::uint8_t* data = RoomData(room)) area_ = RoomLevelId(data);
            }
        }
        return *area_;
    }
    [[nodiscard]] auto IsType(int row) const -> bool override { return row >= 0 && CheckItemType(unit_, row) != 0; }
    [[nodiscard]] auto Stat(int stat, int param) const -> int override { return ReadStat(unit_, stat, param); }
    [[nodiscard]] auto IsGold() const -> bool override { return Code() == PackItemCode("gld"); }
    [[nodiscard]] auto DisplayName() const -> std::string override {
        if (OriginalGetItemName == nullptr) return {};
        char buffer[1024]{};
        OriginalGetItemName(unit_, buffer);
        buffer[sizeof(buffer) - 1] = '\0';
        std::string name(buffer);
        // Item names that span lines are stored bottom line first.
        std::replace(name.begin(), name.end(), '\n', ' ');
        return name;
    }
    [[nodiscard]] auto NameColor() const -> char override {
        // The tooltip's own pick (0x2BD360), then the filter's qualityColors.
        const std::uint32_t classId = ReadAt<std::uint32_t>(static_cast<const std::uint8_t*>(unit_), UnitClassOffset);
        const std::uint8_t* record = ItemsRecord(ItemContext(unit_), classId);
        if (record == nullptr) return '\0';
        const int quality = Quality();
        const NameColorFn pick = OriginalNameColor != nullptr ? OriginalNameColor : (NameColorVerified ? NativeNameColor : nullptr);
        if (pick == nullptr) return '\0';
        const std::uint32_t color = FilterColor(quality, pick(unit_, record, quality), TooltipPlainColors);
        return color <= 42 ? static_cast<char>('0' + color) : '\0';
    }

private:
    void*                                unit_;
    const std::uint8_t*                  data_;
    mutable std::optional<std::uint32_t> code_;
    mutable std::optional<int>           tier_;
    mutable std::optional<int>           maxSockets_;
    mutable std::optional<int>           area_;
    mutable std::optional<Location>      location_;

    [[nodiscard]] auto AtVendor() const -> bool {
        void* inventory = ParentInventory(unit_);
        if (inventory == nullptr) return false;
        const std::int32_t owner = InventoryOwner(inventory);
        return owner >= 0 && ClientUnit(static_cast<std::uint32_t>(owner), UnitTypeMonster) != nullptr;
    }
};

// ---------------------------------------------------------------------------
//  Results per item, kept briefly (UI thread)
// ---------------------------------------------------------------------------
struct CachedOutcome {
    std::uint64_t revision{};
    std::uint32_t mode{};
    std::uint32_t flags{};
    ULONGLONG     tick{};
    Outcome       outcome;
};

std::recursive_mutex                             CacheLock;
std::unordered_map<std::uint32_t, CachedOutcome> Cache;
std::unordered_set<std::uint32_t>                DebugLabelLogged;
std::unordered_set<std::uint32_t>                DebugPickupLogged;
std::shared_ptr<const FilterProgram>             CacheProgram;  // keeps sound pointers alive

auto IsItem(void* unit) -> bool {
    return unit != nullptr && ReadAt<std::uint32_t>(static_cast<const std::uint8_t*>(unit), UnitTypeOffset) == UnitTypeItem;
}

// Evaluates `unit`, or returns the result from less than a second ago.
auto Evaluate(void* unit, Outcome& out) -> bool {
    if (!Active.load() || !IsItem(unit)) return false;
    LoadedFilter filter;
    bool playSounds = true;
    if (!Snapshot(filter, playSounds)) return false;
    GameItem item(unit);
    if (!item.Valid()) return false;
    const std::uint32_t id = ReadAt<std::uint32_t>(static_cast<const std::uint8_t*>(unit), UnitIdOffset);
    const std::uint64_t revision = Revision.load();
    const std::uint32_t mode = item.Mode();
    const std::uint32_t flags = item.Flags();
    const ULONGLONG now = GetTickCount64();
    std::lock_guard<std::recursive_mutex> lock(CacheLock);
    if (CacheProgram != filter.program) {
        Cache.clear();
        CacheProgram = filter.program;
    }
    auto found = Cache.find(id);
    if (found != Cache.end() && found->second.revision == revision && found->second.mode == mode && found->second.flags == flags
        && now - found->second.tick < 1000) {
        out = found->second.outcome;
        return true;
    }
    if (Cache.size() > 4096) Cache.clear();
    GamePlayer player(LocalPlayerUnit());
    const std::string language = LanguageNow();
    EvalSettings settings;
    settings.level = filter.level;
    settings.language = language;
    settings.strings = &Strings;
    Outcome outcome;
    try {
        lootfilter::Evaluate(*filter.program, item, &player, settings, outcome);
    } catch (...) {
        return false;
    }
    CachedOutcome& entry = Cache[id];
    entry.revision = revision;
    entry.mode = mode;
    entry.flags = flags;
    entry.tick = now;
    entry.outcome = outcome;
    out = std::move(outcome);
    return true;
}

// ---------------------------------------------------------------------------
//  Droplight: an overlay of the plugin's own on each matching ground item
// ---------------------------------------------------------------------------
constexpr std::size_t  DroplightNodeBytes = 0x100;
constexpr std::int32_t DroplightGuidBase  = 0x7E000000;  // far above the game's own overlay ids
constexpr char DroplightBeam[]    = "data/hd/vfx/particles/overlays/object/horadric_light/fx_horadric_light.particles";
constexpr char DroplightSparkle[] = "data/hd/vfx/particles/overlays/common/valkyriestart/valkriestart_overlay.particles";

std::atomic<void*>        TranslationLayer{};  // captured from the game's own overlay and unit calls
std::atomic<std::int32_t> DroplightRow{ -1 };
std::atomic<std::int32_t> NextDroplightGuid{ DroplightGuidBase };
std::atomic<bool>         DroplightsInUse{};
std::atomic<bool>         DroplightQueued{};
std::atomic<bool>         DroplightReset{};

struct DroplightEntry {
    std::unique_ptr<std::uint8_t[]> node;
    std::string                     key;  // color key; empty for the game's beam
};
std::mutex                                            DroplightLock;
std::unordered_map<std::uint32_t, DroplightEntry>     Droplights;        // by item unit id
std::unordered_map<const std::uint8_t*, std::string>  DroplightNodes;    // our nodes -> color key
std::vector<std::unique_ptr<std::uint8_t[]>>          RetiredNodes;      // freed when the game ends
std::unordered_set<std::uint32_t>                     DroplightPending;
std::unordered_map<std::uint32_t, std::uint64_t>      DroplightChecked;  // id -> filter revision
std::unordered_map<std::string, std::string>          DroplightPaths;    // color key -> generated definition
std::unordered_set<std::string>                       DroplightRegistered;

// "rrggbb" for a rule's color, empty for the game's own beam.
auto DroplightColorKey(const std::optional<Rgba>& color) -> std::string {
    if (!color) return {};
    char text[16];
    std::snprintf(text, sizeof(text), "%02x%02x%02x", color->r, color->g, color->b);
    return text;
}

// One OverlayDefinition per color the filter uses: the entities of the
// droplight example, the beam and the sparkle. For a color, the beam is the
// plugin's recolored copy of the mod's beam file; without a color (or without
// that file) it is the game's own.
void PrepareDroplights(const FilterProgram* program) {
    std::vector<std::optional<Rgba>> colors;
    if (program != nullptr) {
        for (const Rule& rule : program->rules) {
            if (!rule.enabled || !rule.then.droplight) continue;
            const std::optional<Rgba>& color = rule.then.droplight->color;
            if (std::none_of(colors.begin(), colors.end(), [&](const std::optional<Rgba>& c) { return DroplightColorKey(c) == DroplightColorKey(color); })) {
                colors.push_back(color);
            }
        }
    }
    std::int32_t row = -1;
    if (!colors.empty()) {
        TxtNames overlays;
        if (overlays.Load(ExcelFolder + L"\\overlay.txt", "overlay")) row = overlays.Find(Config.droplightOverlay);
        if (row < 0) LogWarn("LootFilter: overlay.txt has no row \"%s\" (droplight_overlay); droplights are off.", Config.droplightOverlay.c_str());
    }
    DroplightRow.store(row);
    // The mod's beam file, for the colors.
    std::vector<std::uint8_t> beamBytes;
    beam::ColorSites beamSites;
    const bool anyColor = std::any_of(colors.begin(), colors.end(), [](const std::optional<Rgba>& c) { return c.has_value(); });
    if (anyColor && row >= 0) {
        constexpr std::wstring_view excelTail = L"\\global\\excel";
        std::wstring beamPath;
        if (ExcelFolder.size() > excelTail.size() && std::wstring_view(ExcelFolder).substr(ExcelFolder.size() - excelTail.size()) == excelTail) {
            beamPath = ExcelFolder.substr(0, ExcelFolder.size() - excelTail.size()) + L"\\" + Widen(Config.droplightBeam);
            std::replace(beamPath.begin(), beamPath.end(), L'/', L'\\');
        }
        std::string text;
        if (beamPath.empty() || !ReadWholeFile(beamPath, text)) {
            LogWarn("LootFilter: the mod has no droplight beam at data/%s (droplight_beam); colored droplights keep the game's white beam.",
                    Config.droplightBeam.c_str());
        } else {
            beamBytes.assign(text.begin(), text.end());
            beamSites = beam::FindColor(beamBytes);
            if (!beamSites.Found()) {
                LogWarn("LootFilter: data/%s does not hold the beam color the plugin recolors; colored droplights keep the game's white beam.",
                        Config.droplightBeam.c_str());
                beamBytes.clear();
            } else if (Config.debugLog) {
                LogInfo("LootFilter debug: droplight beam %s: red stored %zu times, green %zu, blue %zu, alpha %zu.", Config.droplightBeam.c_str(),
                        beamSites.red.size(), beamSites.green.size(), beamSites.blue.size(), beamSites.alpha.size());
            }
        }
    }
    std::unordered_map<std::string, std::string> paths;
    if (row >= 0 && ResourceSvc != nullptr) {
        const std::string none = "\"models\":[],\"skeletons\":[],\"animations\":[],\"textures\":[],\"physics\":[],\"json\":[],";
        const std::string transform = "{\"type\":\"TransformDefinitionComponent\",\"name\":\"component_transform1\",\"position\":{\"x\":0.0,\"y\":0.0,\"z\":0.0},"
            "\"orientation\":{\"x\":0.0,\"y\":0.0,\"z\":0.0,\"w\":1.0},\"scale\":{\"x\":1.0,\"y\":1.0,\"z\":1.0},\"inheritOnlyPosition\":false}";
        const std::string folder = std::string("d2rloader/") + PluginIdText;
        std::lock_guard<std::mutex> lock(IconFilesLock);
        const auto registerOnce = [](const std::string& file, const std::string& text) {
            if (DroplightRegistered.count(file) != 0) return true;
            const std::vector<std::uint8_t> bytes(text.begin(), text.end());
            if (!RegisterSprite(file, bytes)) return false;
            DroplightRegistered.insert(file);
            return true;
        };
        for (const std::optional<Rgba>& color : colors) {
            const std::string key = DroplightColorKey(color);
            const std::string name = key.empty() ? std::string("droplight") : "droplight-" + key + "-" + std::to_string(Config.droplightBrightness);
            std::string beamFile = DroplightBeam;
            if (color && !beamBytes.empty()) {
                const std::vector<std::uint8_t> recolored =
                    beam::Recolor(beamBytes, beamSites, color->r / 255.0, color->g / 255.0, color->b / 255.0, Config.droplightBrightness / 100.0);
                const std::string candidate = "data/hd/vfx/particles/" + folder + "/" + name + ".particles";
                if (registerOnce(candidate, std::string(recolored.begin(), recolored.end()))) beamFile = candidate;
            }
            const std::string definition = std::string("{\"dependencies\":{\"particles\":[{\"path\":\"") + beamFile + "\"},{\"path\":\"" + DroplightSparkle
                + "\"}]," + none + "\"variantdata\":[],\"objecteffects\":[],\"other\":[]},"
                "\"type\":\"OverlayDefinition\",\"name\":\"lootfilter_droplight\",\"entities\":["
                "{\"type\":\"Entity\",\"name\":\"entity_root\",\"id\":1563839986,\"components\":[" + transform
                + ",{\"type\":\"VfxDefinitionComponent\",\"name\":\"entity_vfx_droplight\",\"filename\":\"" + beamFile + "\",\"hardKillOnDestroy\":false}]},"
                "{\"type\":\"Entity\",\"name\":\"droplight\",\"id\":1563839987,\"components\":[" + transform
                + ",{\"type\":\"VfxDefinitionComponent\",\"name\":\"entity_vfx_droplight_sparkle\",\"filename\":\"" + DroplightSparkle
                + "\",\"hardKillOnDestroy\":false}]}]}";
            const std::string file = "data/hd/overlays/" + folder + "/" + name + ".json";
            if (registerOnce(file, definition)) paths.emplace(key, file);
        }
    }
    std::lock_guard<std::mutex> lock(DroplightLock);
    DroplightPaths = std::move(paths);
    DroplightsInUse.store(!DroplightPaths.empty());
    DroplightReset.store(true);  // the next sync removes every droplight; items get re-checked
}

auto NewDroplightNode(std::int32_t row) -> std::unique_ptr<std::uint8_t[]> {
    auto node = std::make_unique<std::uint8_t[]>(DroplightNodeBytes);
    std::memset(node.get(), 0, DroplightNodeBytes);
    const std::int32_t guid = NextDroplightGuid.fetch_add(1);
    std::memcpy(node.get() + 4, &row, sizeof(row));       // overlay.txt row
    std::memcpy(node.get() + 0xA8, &guid, sizeof(guid));  // the overlay's id in the game's overlay map
    return node;
}

// DroplightLock held. The game's own "remove overlay" (0x773C70).
void RemoveDroplight(void* layer, void* unit, DroplightEntry& entry) {
    if (!entry.node) return;
    if (layer != nullptr && unit != nullptr) {
        OverlayRequest request{ unit, entry.node.get() };
        NativeOverlayRemoved(layer, &request);
    }
    DroplightNodes.erase(entry.node.get());
    RetiredNodes.push_back(std::move(entry.node));
}

// UI thread: adds and removes droplights for the items queued since last time.
void __cdecl SyncDroplights(const D2RL::PluginContext*, void*) noexcept {
    DroplightQueued.store(false);
    void* layer = TranslationLayer.load();
    if (!Active.load() || layer == nullptr || !Hooks.droplights) return;
    try {
        std::unordered_set<std::uint32_t> pending;
        std::lock_guard<std::mutex> lock(DroplightLock);
        std::swap(pending, DroplightPending);
        if (DroplightReset.exchange(false)) {
            for (auto& [id, entry] : Droplights) RemoveDroplight(layer, ClientUnit(id, UnitTypeItem), entry);
            Droplights.clear();
        }
        const std::int32_t row = DroplightRow.load();
        for (std::uint32_t id : pending) {
            void* unit = ClientUnit(id, UnitTypeItem);
            if (unit == nullptr) continue;
            Outcome outcome;
            const bool wanted = !DroplightPaths.empty() && row >= 0 && Evaluate(unit, outcome) && outcome.droplight != nullptr
                && DroplightPaths.count(DroplightColorKey(outcome.droplight->color)) != 0;
            auto found = Droplights.find(id);
            if (!wanted) {
                if (found != Droplights.end()) {
                    RemoveDroplight(layer, unit, found->second);
                    Droplights.erase(found);
                }
                continue;
            }
            const std::string key = DroplightColorKey(outcome.droplight->color);
            if (found != Droplights.end()) {
                if (found->second.key == key) continue;
                RemoveDroplight(layer, unit, found->second);
                Droplights.erase(found);
            }
            DroplightEntry entry;
            entry.node = NewDroplightNode(row);
            entry.key = key;
            DroplightNodes[entry.node.get()] = key;
            OverlayRequest request{ unit, entry.node.get() };
            OriginalOverlayAdded(layer, &request);  // the game's own "add overlay"
            if (Config.debugLog) LogInfo("LootFilter debug: droplight added to item %u (%s).", id, key.empty() ? "game's beam" : key.c_str());
            Droplights.emplace(id, std::move(entry));
        }
    } catch (...) {
    }
}

void QueueDroplightCheck(std::uint32_t id) {
    if (!Hooks.droplights || !DroplightsInUse.load(std::memory_order_relaxed)) return;
    {
        std::lock_guard<std::mutex> lock(DroplightLock);
        if (DroplightPending.size() < 4096) DroplightPending.insert(id);
    }
    if (ThreadSvc != nullptr && !DroplightQueued.exchange(true)) {
        if (ThreadSvc->runOnUiThread(Context, &SyncDroplights, nullptr) != D2RL::Threads::Result::Success) DroplightQueued.store(false);
    }
}

// Once per item and filter revision: is a droplight due?
void CheckDroplightOnce(void* unit) noexcept {
    if (!DroplightsInUse.load(std::memory_order_relaxed)) return;
    const std::uint32_t id = ReadAt<std::uint32_t>(static_cast<const std::uint8_t*>(unit), UnitIdOffset);
    const std::uint64_t revision = Revision.load();
    bool queue = false;
    {
        std::lock_guard<std::mutex> lock(DroplightLock);
        auto [entry, added] = DroplightChecked.try_emplace(id, revision);
        if (added || entry->second != revision) {
            entry->second = revision;
            queue = true;
        }
        if (DroplightChecked.size() > 8192) DroplightChecked.clear();
    }
    if (queue) QueueDroplightCheck(id);
}

std::uint64_t __fastcall HookOverlayAdded(void* layer, OverlayRequest* request) noexcept {
    if (layer != nullptr) TranslationLayer.store(layer);
    return OriginalOverlayAdded(layer, request);
}

// Before the game queues an item's removal, queue the removal of its droplight.
std::uint64_t __fastcall HookUnitRemoved(void* layer, void** request) noexcept {
    try {
        if (layer != nullptr) TranslationLayer.store(layer);
        void* unit = request != nullptr ? request[0] : nullptr;
        if (unit != nullptr && IsItem(unit)) {
            const std::uint32_t id = ReadAt<std::uint32_t>(static_cast<const std::uint8_t*>(unit), UnitIdOffset);
            std::lock_guard<std::mutex> lock(DroplightLock);
            auto found = Droplights.find(id);
            if (found != Droplights.end()) {
                RemoveDroplight(layer, unit, found->second);
                Droplights.erase(found);
            }
            DroplightChecked.erase(id);
        }
    } catch (...) {
    }
    return OriginalUnitRemoved(layer, request);
}

// The definition path for our nodes; every other overlay goes to the game.
void* __fastcall HookOverlayPath(void* table, void* outString, const std::uint8_t* node) noexcept {
    if (node != nullptr && outString != nullptr) {
        std::string path;
        {
            std::lock_guard<std::mutex> lock(DroplightLock);
            const auto found = DroplightNodes.find(node);
            if (found != DroplightNodes.end()) {
                const auto definition = DroplightPaths.find(found->second);
                if (definition != DroplightPaths.end()) path = definition->second;
            }
        }
        if (!path.empty()) return NativeGameString(outString, path.c_str());
    }
    return OriginalOverlayPath(table, outString, node);
}

// The item's minimap icon, from the cache when the result is fresh. `keep`
// holds the program the icon belongs to while the caller draws it.
auto IconFor(void* unit, std::shared_ptr<const FilterProgram>& keep) -> const IconSpec* {
    if (!Active.load() || !IsItem(unit)) return nullptr;
    {
        GameItem item(unit);
        if (!item.Valid()) return nullptr;
        const std::uint32_t id = ReadAt<std::uint32_t>(static_cast<const std::uint8_t*>(unit), UnitIdOffset);
        const std::uint64_t revision = Revision.load();
        const std::uint32_t mode = item.Mode();
        const std::uint32_t flags = item.Flags();
        const ULONGLONG now = GetTickCount64();
        std::lock_guard<std::recursive_mutex> lock(CacheLock);
        const auto found = Cache.find(id);
        if (found != Cache.end() && CacheProgram && found->second.revision == revision && found->second.mode == mode
            && found->second.flags == flags && now - found->second.tick < 1000) {
            if (found->second.outcome.icon == nullptr) return nullptr;
            keep = CacheProgram;
            return found->second.outcome.icon;
        }
    }
    Outcome outcome;
    if (!Evaluate(unit, outcome) || outcome.icon == nullptr) return nullptr;
    std::lock_guard<std::recursive_mutex> lock(CacheLock);
    // The icon points into the program Evaluate cached; keep that one alive.
    const auto found = Cache.find(ReadAt<std::uint32_t>(static_cast<const std::uint8_t*>(unit), UnitIdOffset));
    if (found == Cache.end() || found->second.outcome.icon != outcome.icon || !CacheProgram) return nullptr;
    keep = CacheProgram;
    return outcome.icon;
}

// Sprites the game loaded for icons, by sprite name. Entries are never
// removed, so a pointer to one stays valid.
struct LoadedIcon {
    GameSprite sprite;
    bool       tried{};
};
std::mutex                                  IconSpritesLock;
std::unordered_map<std::string, LoadedIcon> IconSprites;
std::atomic<bool>                           IconLoadQueued{};

// UI thread: loads the sprites the minimap asked for.
void __cdecl LoadIconSprites(const D2RL::PluginContext*, void*) noexcept {
    IconLoadQueued.store(false);
    if (!Active.load() || SpriteLoad == nullptr) return;
    std::lock_guard<std::mutex> lock(IconSpritesLock);
    for (auto& [name, icon] : IconSprites) {
        if (icon.tried) continue;
        icon.tried = true;
        try {
            SpriteLoad(&icon.sprite, name.c_str(), 0);
        } catch (...) {
        }
        if (icon.sprite.hd == nullptr) {
            LogWarn("LootFilter: the game did not load the icon sprite %s (legacy graphics, or the file was refused).", name.c_str());
        } else if (Config.debugLog) {
            LogInfo("LootFilter debug: icon sprite %s loaded.", name.c_str());
        }
    }
}

// Ground items with an icon, remembered from the moment the filter gives them
// one until they are picked up. The game hands the minimap only the units of
// the rooms next to the player (0x0D2600 walks the player's near rooms), and
// forgets items further away, so the icons are drawn from this list instead.
struct RememberedIcon {
    std::int32_t level{ -1 };
    std::int32_t act{ -1 };
    std::int32_t x{};
    std::int32_t y{};
    std::string  sprite;
    std::int32_t width{};
    std::int32_t height{};
    Rgba         color{};
};
std::mutex                                        IconMemoryLock;
std::unordered_map<std::uint32_t, RememberedIcon> IconMemory;  // by item unit id
std::atomic<std::uint64_t>                        LastIconFrame{ ~0ULL };

// levels.txt Id -> Act, so icons of the other levels of the act (outdoor
// areas show next to each other) still draw, and other acts' never do.
std::once_flag            LevelActsOnce;
std::vector<std::int32_t> LevelActs;
void LoadLevelActs() noexcept {
    try {
        std::string text;
        if (!ReadWholeFile(ExcelFolder + L"\\levels.txt", text)) return;
        int idColumn = -1, actColumn = -1;
        bool header = true;
        std::size_t start = 0;
        while (start < text.size()) {
            std::size_t end = text.find('\n', start);
            if (end == std::string::npos) end = text.size();
            std::string_view line(text.data() + start, end - start);
            start = end + 1;
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            std::vector<std::string_view> cells;
            for (std::size_t cell = 0;;) {
                const std::size_t tab = line.find('\t', cell);
                cells.push_back(line.substr(cell, tab == std::string_view::npos ? std::string_view::npos : tab - cell));
                if (tab == std::string_view::npos) break;
                cell = tab + 1;
            }
            if (header) {
                for (std::size_t i = 0; i < cells.size(); ++i) {
                    if (cells[i] == "Id") idColumn = static_cast<int>(i);
                    if (cells[i] == "Act") actColumn = static_cast<int>(i);
                }
                if (idColumn < 0 || actColumn < 0) return;
                header = false;
                continue;
            }
            if (cells.size() <= static_cast<std::size_t>(std::max(idColumn, actColumn))) continue;
            const auto number = [](std::string_view text, int& out) {
                if (text.empty() || text.size() > 6) return false;
                out = 0;
                for (char c : text) {
                    if (c < '0' || c > '9') return false;
                    out = out * 10 + (c - '0');
                }
                return true;
            };
            int id = 0, act = 0;
            if (!number(cells[static_cast<std::size_t>(idColumn)], id) || !number(cells[static_cast<std::size_t>(actColumn)], act) || id > 4095) continue;
            if (LevelActs.size() <= static_cast<std::size_t>(id)) LevelActs.resize(static_cast<std::size_t>(id) + 1, -1);
            LevelActs[static_cast<std::size_t>(id)] = act;
        }
    } catch (...) {
    }
}
auto ActOfLevel(int level) -> int {
    std::call_once(LevelActsOnce, LoadLevelActs);
    return level >= 0 && static_cast<std::size_t>(level) < LevelActs.size() ? LevelActs[static_cast<std::size_t>(level)] : -1;
}
auto LevelOfUnit(void* unit) -> int {
    if (void* room = UnitRoom(unit)) {
        if (std::uint8_t* data = RoomData(room)) return RoomLevelId(data);
    }
    return -1;
}

// Remembers (or forgets) the item's icon from what the filter says now.
void RecordIcon(void* unit) noexcept {
    try {
        std::shared_ptr<const FilterProgram> keep;
        const IconSpec* icon = IconFor(unit, keep);
        const std::uint32_t id = ReadAt<std::uint32_t>(static_cast<const std::uint8_t*>(unit), UnitIdOffset);
        RememberedIcon entry;
        if (icon != nullptr && !icon->spriteName.empty()) {
            entry.level = LevelOfUnit(unit);
            entry.act = ActOfLevel(entry.level);
            entry.x = UnitClientX(unit);
            entry.y = UnitClientY(unit);
            entry.sprite = icon->spriteName;
            entry.width = icon->width;
            entry.height = icon->height;
            entry.color = icon->color;
        }
        std::lock_guard<std::mutex> lock(IconMemoryLock);
        if (entry.sprite.empty()) {
            IconMemory.erase(id);
        } else if (IconMemory.size() < 4096 || IconMemory.count(id) != 0) {
            IconMemory[id] = std::move(entry);
        }
    } catch (...) {
    }
}

// As the game draws its own minimap markers (0x0D6DB0): centred on the
// projected point and drawn at the minimap's scale, which the HD draw takes
// as its third argument.
void DrawIconAt(void* context, const RememberedIcon& icon) noexcept {
    GameSprite* sprite = nullptr;
    {
        std::lock_guard<std::mutex> lock(IconSpritesLock);
        auto [entry, added] = IconSprites.try_emplace(icon.sprite);
        if (entry->second.tried) {
            if (entry->second.sprite.hd != nullptr) sprite = &entry->second.sprite;
        } else if (ThreadSvc != nullptr && !IconLoadQueued.exchange(true)) {
            if (ThreadSvc->runOnUiThread(Context, &LoadIconSprites, nullptr) != D2RL::Threads::Result::Success) IconLoadQueued.store(false);
        }
    }
    if (sprite == nullptr) return;
    const auto* view = static_cast<const std::uint8_t*>(context);
    const std::uint64_t packed = static_cast<std::uint32_t>(icon.x) | (static_cast<std::uint64_t>(static_cast<std::uint32_t>(icon.y)) << 32);
    NativePoint point{};
    ProjectAutomap(context, &point, packed);
    const std::int32_t left = ReadAt<std::int32_t>(view, 0x18);
    const std::int32_t top = ReadAt<std::int32_t>(view, 0x1C);
    if (point.x < left || point.y < top || point.x >= left + ReadAt<std::int32_t>(view, 0x20)
        || point.y >= top + ReadAt<std::int32_t>(view, 0x24)) {
        return;  // outside the minimap's own rectangle
    }
    const float scale = ReadAt<float>(view, 0x38);
    const std::int32_t position[2]{
        point.x - static_cast<std::int32_t>(static_cast<float>(icon.width / 2) * scale),
        point.y - static_cast<std::int32_t>(static_cast<float>(icon.height / 2) * scale),
    };
    SpriteDrawParams params{};
    params.tint[0] = icon.color.r / 255.0f;
    params.tint[1] = icon.color.g / 255.0f;
    params.tint[2] = icon.color.b / 255.0f;
    params.fade = icon.color.a / 255.0f;
    SpriteDrawHd(sprite->hd, position, scale, &params);
}

// Once per minimap pass: every remembered icon of the player's act, at the
// item's live position while the game knows the item, else where it lay.
void DrawRememberedIcons(void* context) noexcept {
    void* player = LocalPlayerUnit();
    if (player == nullptr) return;
    const int level = LevelOfUnit(player);
    const int act = ActOfLevel(level);
    std::vector<RememberedIcon> visible;
    {
        std::lock_guard<std::mutex> lock(IconMemoryLock);
        for (auto it = IconMemory.begin(); it != IconMemory.end();) {
            if (void* unit = ClientUnit(it->first, UnitTypeItem)) {
                GameItem item(unit);
                if (item.Valid()) {
                    const std::uint32_t mode = item.Mode();
                    if (mode != 3 && mode != 5) {  // picked up
                        it = IconMemory.erase(it);
                        continue;
                    }
                    it->second.x = UnitClientX(unit);
                    it->second.y = UnitClientY(unit);
                }
            }
            const RememberedIcon& icon = it->second;
            if (icon.act >= 0 && act >= 0 ? icon.act == act : icon.level == level) visible.push_back(icon);
            ++it;
        }
    }
    for (const RememberedIcon& icon : visible) DrawIconAt(context, icon);
}

void __cdecl OnFrame(const D2RL::PluginContext*, const D2RL::Overlay::Frame*, void*) noexcept {
    FrameCount.fetch_add(1, std::memory_order_relaxed);
}

std::int64_t __fastcall HookAutomapUnit(void* unit, void* context) noexcept {
    const std::int64_t result = OriginalAutomapUnit(unit, context);
    if (unit == nullptr || context == nullptr || !Active.load(std::memory_order_relaxed)) return result;
    try {
        bool firstThisPass = false;
        if (FrameHandle != D2RL::Overlay::InvalidCallbackHandle) {
            const std::uint64_t frame = FrameCount.load(std::memory_order_relaxed);
            firstThisPass = LastIconFrame.exchange(frame) != frame;
        } else {
            firstThisPass = unit == LocalPlayerUnit();
        }
        if (ReadAt<std::uint32_t>(static_cast<const std::uint8_t*>(unit), UnitTypeOffset) == UnitTypeItem) {
            RecordIcon(unit);
            CheckDroplightOnce(unit);
        }
        if (firstThisPass) DrawRememberedIcons(context);
    } catch (...) {
    }
    return result;
}

// ---------------------------------------------------------------------------
//  debug_log: one line per item and event, saying what the filter decided
// ---------------------------------------------------------------------------
auto LocationText(Location location) -> const char* {
    switch (location) {
    case Location::Ground: return "on the ground";
    case Location::Stored: return "stored";
    case Location::Equipped: return "equipped";
    case Location::Vendor: return "at a vendor";
    case Location::Other: return "elsewhere";
    }
    return "?";
}

// seen: log each item once (label checks run every frame); null: log every call.
void DebugItem(void* unit, const Outcome* outcome, const std::string& verdict, std::unordered_set<std::uint32_t>* seen) noexcept {
    if (!Config.debugLog || !IsItem(unit)) return;
    try {
        const std::uint32_t id = ReadAt<std::uint32_t>(static_cast<const std::uint8_t*>(unit), UnitIdOffset);
        if (seen != nullptr) {
            std::lock_guard<std::recursive_mutex> lock(CacheLock);
            if (!seen->insert(id).second) return;
        }
        GameItem item(unit);
        if (!item.Valid()) return;
        std::string rules;
        if (outcome != nullptr) {
            for (int number : outcome->matchedRules) rules += (rules.empty() ? "" : ", ") + std::to_string(number);
        }
        char line[1024];
        std::snprintf(line, sizeof(line),
            "LootFilter debug: item %u \"%s\" code %s, quality %d, tier %d, ilvl %d, sockets %d/%d, ethereal %d, identified %d, %s, area %d. "
            "Rules matched: %s.%s%s%s %s",
            id, item.DisplayName().c_str(), UnpackItemCode(item.Code()).c_str(), item.Quality(), item.BaseTier(), item.ItemLevel(),
            item.Sockets(), item.MaxSockets(), item.Ethereal() ? 1 : 0, item.Identified() ? 1 : 0, LocationText(item.Where()), item.Area(),
            outcome == nullptr ? "not evaluated" : rules.empty() ? "none" : rules.c_str(),
            outcome != nullptr && outcome->nameChanged ? " Name changed." : "",
            outcome != nullptr && outcome->background ? " Background set." : "",
            outcome != nullptr && outcome->border ? " Border set." : "",
            verdict.c_str());
        LogInfo("%s", line);
    } catch (...) {
    }
}

// ---------------------------------------------------------------------------
//  Hooks
// ---------------------------------------------------------------------------
thread_local bool NameHookRan{};

auto ReturnRva(void* returnAddress) -> std::uint64_t {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(returnAddress) - Base);
}

auto IsNameCaller(std::uint64_t returnRva) -> bool {
    return std::any_of(NameCallSites.begin(), NameCallSites.end(), [&](std::uint64_t site) { return site + 5 == returnRva; });
}

auto FinishName(const Outcome& outcome, const std::string& original) -> std::string {
    std::string name = outcome.nameChanged ? ApplyNameTemplate(outcome.nameTemplate, original) : original;
    if (outcome.hidden && ShowHidden.load()) name += Config.hiddenMarker;
    return GameText(name);
}

#if defined(_MSC_VER)
#define LF_RETURN_ADDRESS() _ReturnAddress()
#else
#define LF_RETURN_ADDRESS() __builtin_return_address(0)
#endif

#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
void* __fastcall HookGetItemName(void* item, char* buffer) noexcept {
    void* const returnAddress = LF_RETURN_ADDRESS();
    void* const result = OriginalGetItemName(item, buffer);
    const std::uint64_t returnRva = ReturnRva(returnAddress);
    if (returnRva == GroundLabelReturnRva) NameHookRan = true;
    if (buffer == nullptr || !IsNameCaller(returnRva)) return result;
    try {
        Outcome outcome;
        if (!Evaluate(item, outcome) || (!outcome.nameChanged && !(outcome.hidden && ShowHidden.load()))) return result;
        buffer[1023] = '\0';
        const std::string original(buffer);
        CopyLimited(buffer, 1024, FinishName(outcome, original));
    } catch (...) {
    }
    return result;
}

void __fastcall HookLabelText(void* unit, char* text, std::int32_t size, std::uint32_t* color) noexcept {
    NameHookRan = false;
    OriginalLabelText(unit, text, size, color);
    if (NameHookRan || text == nullptr || size <= 1 || color == nullptr) return;
    try {
        // Gold: the label was built without GetItemName.
        Outcome outcome;
        if (!Evaluate(unit, outcome) || (!outcome.nameChanged && !(outcome.hidden && ShowHidden.load()))) return;
        std::string current(text, strnlen(text, static_cast<std::size_t>(size)));
        const std::string marker = Marker();
        if (!marker.empty() && current.compare(0, marker.size(), marker) == 0 && current.size() > marker.size()) {
            current.erase(0, marker.size() + 1);  // the color code the label builder put in front
        }
        char buffer[1024]{};
        CopyLimited(buffer, sizeof(buffer), FinishName(outcome, current));
        NativeColorize(buffer, static_cast<std::int32_t>(*color));
        CopyLimited(text, static_cast<std::size_t>(size), std::string(buffer));
    } catch (...) {
    }
}

bool __fastcall HookShowLabel(void* filter, void* item) noexcept {
    Outcome outcome;
    bool evaluated = false;
    if (item != nullptr) {
        try {
            evaluated = Evaluate(item, outcome);
        } catch (...) {
            evaluated = false;
        }
    }
    if (evaluated && outcome.hidden && !ShowHidden.load()) {
        if (Config.debugLog) {
            DebugItem(item, &outcome, outcome.hiddenBy > 0 ? "Label hidden by rule " + std::to_string(outcome.hiddenBy) + "."
                                                           : std::string("Label hidden: no rule matched and hideUnmatchedItems is on."),
                      &DebugLabelLogged);
        }
        return false;
    }
    const bool shown = OriginalShowLabel(filter, item);
    if (Config.debugLog && item != nullptr) {
        DebugItem(item, evaluated ? &outcome : nullptr,
                  shown ? std::string("Label shown.") : std::string("Label hidden by the game's own loot filter, not by this filter."),
                  &DebugLabelLogged);
    }
    return shown;
}

// A hidden item cannot be hovered, targeted or picked up, with the mouse or a
// controller, for as long as it is hidden: not while Show Hidden Items is on,
// and not while the game's own loot filter toggle is off (every label shows
// then). With its label hidden, the only ways left to reach it were the
// mouse's hit test of the item itself and the controller's targeting.
// Gold stays pickable: it is picked up automatically anyway.
auto HiddenFromPickup(void* unit) noexcept -> bool {
    if (unit == nullptr || !Active.load(std::memory_order_relaxed)) return false;
    if (ReadAt<std::uint32_t>(static_cast<const std::uint8_t*>(unit), UnitTypeOffset) != UnitTypeItem) return false;
    if (ShowHidden.load()) return false;
    if (NativeGameToggle != nullptr && (NativeGameToggle(GameToggleFilterOff) & 0xFF) != 0) return false;
    try {
        if (GameItem item(unit); item.Valid() && item.IsGold()) return false;
        Outcome outcome;
        if (!Evaluate(unit, outcome) || !outcome.hidden) return false;
        if (Config.debugLog) {
            DebugItem(unit, &outcome, "Cannot be hovered or picked up: hidden" + (outcome.hiddenBy > 0 ? " by rule " + std::to_string(outcome.hiddenBy) : std::string()) + ".",
                      &DebugPickupLogged);
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::int64_t __fastcall HookUnitSelectTest(void* unit, std::int32_t x, std::int32_t y, std::int32_t mode, std::uint32_t skill) noexcept {
    if (HiddenFromPickup(unit)) return 0;
    return OriginalUnitSelectTest(unit, x, y, mode, skill);
}

std::uint64_t __fastcall HookTargetSelectTest(void* controller, void* player, void* unit) noexcept {
    if (HiddenFromPickup(unit)) return 0;
    return OriginalTargetSelectTest(controller, player, unit);
}

auto ToFloats(const Rgba& color, float (&out)[4]) -> void {
    out[0] = static_cast<float>(color.r) / 255.0f;
    out[1] = static_cast<float>(color.g) / 255.0f;
    out[2] = static_cast<float>(color.b) / 255.0f;
    out[3] = static_cast<float>(color.a) / 255.0f;
}

std::uint64_t __fastcall HookDrawLabel(void* record, const char* text, const float* rgba) noexcept {
    const float* use = rgba;
    float background[4]{};
    if (record != nullptr && rgba != nullptr && ClientUnit != nullptr) {
        try {
            const auto* bytes = static_cast<const std::uint8_t*>(record);
            void* item = ClientUnit(ReadAt<std::uint32_t>(bytes, LabelUnitIdOffset), UnitTypeItem);
            Outcome outcome;
            const bool hovered = std::memcmp(rgba, reinterpret_cast<const void*>(Base + HoverColorRva), 4 * sizeof(float)) == 0;
            if (item != nullptr && !hovered && Evaluate(item, outcome)) {
                if (outcome.border && FillRect != nullptr) {
                    std::int32_t x = ReadAt<std::int32_t>(bytes, 0);
                    std::int32_t y = ReadAt<std::int32_t>(bytes, 4);
                    const std::int32_t w = ReadAt<std::int32_t>(bytes, 8);
                    const std::int32_t h = ReadAt<std::int32_t>(bytes, 12);
                    if (!LabelScreenSpace()) {
                        const std::uint64_t center = static_cast<std::uint32_t>(x + w / 2)
                            | (static_cast<std::uint64_t>(static_cast<std::uint32_t>(y + h / 2)) << 32);
                        const std::uint64_t projected = ProjectPoint(center);
                        x = static_cast<std::int32_t>(static_cast<std::uint32_t>(projected)) - w / 2;
                        y = static_cast<std::int32_t>(static_cast<std::uint32_t>(projected >> 32)) - h / 2;
                    }
                    float border[4]{};
                    ToFloats(outcome.border->color, border);
                    const std::int32_t width = outcome.border->width;
                    FillRect(x - width, y - width, x + w + width, y + h + width, border, 1);
                }
                if (outcome.background) {
                    ToFloats(*outcome.background, background);
                    use = background;
                }
            }
        } catch (...) {
            use = rgba;
        }
    }
    return OriginalDrawLabel(record, text, use);
}

// Queues the chat lines and sounds of the item a ground action just placed.
void AlertGroundItem(const std::uint8_t* packet) noexcept {
    if (packet == nullptr || !Active.load()) return;
    try {
        const std::uint32_t id = ReadAt<std::uint32_t>(packet, 4);
        void* item = ClientUnit(id, UnitTypeItem);
        if (item == nullptr) return;
        Outcome outcome;
        if (!Evaluate(item, outcome)) return;
        // Every drop and every time an item comes into view alerts; each of these
        // runs once per 0x9C packet, so one event never alerts twice.
        RecordIcon(item);
        if (outcome.droplight != nullptr) QueueDroplightCheck(id);
        if (outcome.chatMessages.empty() && outcome.sounds.empty()) {
            DebugItem(item, &outcome, "On the ground: no chat line or sound for it.", nullptr);
            return;
        }
        bool playSounds = true;
        {
            std::shared_lock<std::shared_mutex> lock(FilterLock);
            playSounds = CurrentSelection.playSounds;
        }
        DebugItem(item, &outcome,
                  "On the ground: " + std::to_string(outcome.chatMessages.size()) + " chat line(s), " + std::to_string(outcome.sounds.size())
                      + (playSounds ? " sound(s)." : " sound(s), sounds are off."),
                  nullptr);
        if (!outcome.chatMessages.empty()) {
            GameItem facts(item);
            const std::string name = facts.DisplayName();
            for (std::string line : outcome.chatMessages) {
                std::size_t link;
                while ((link = line.find(ItemLinkToken)) != std::string::npos) line.replace(link, 1, name);
                QueueChat(std::move(line));
            }
        }
        if (playSounds) {
            for (const SoundPlay& play : outcome.sounds) QueueSound(*play.sound, play.volume);
        }
    } catch (...) {
    }
}

std::int64_t __fastcall HookNewDrop(const std::uint8_t* packet, std::uint64_t action) noexcept {
    const std::int64_t result = OriginalNewDrop(packet, action);
    AlertGroundItem(packet);
    return result;
}

std::int64_t __fastcall HookHandDrop(const std::uint8_t* packet, std::uint64_t action) noexcept {
    const std::int64_t result = OriginalHandDrop(packet, action);
    AlertGroundItem(packet);
    return result;
}

std::int64_t __fastcall HookSeenOnGround(const std::uint8_t* packet, std::uint64_t action) noexcept {
    const std::int64_t result = OriginalSeenOnGround(packet, action);
    AlertGroundItem(packet);
    return result;
}

// qualityColors on ground labels: 0x0CBFF0(item) -> color.
std::uint32_t __fastcall HookLabelColor(void* item) noexcept {
    const std::uint32_t color = OriginalLabelColor(item);
    if (item == nullptr || !Active.load()) return color;
    const std::uint8_t* data = GetItemData(item);
    if (data == nullptr) return color;
    return FilterColor(static_cast<int>(ReadAt<std::uint32_t>(data, ItemQualityOffset)), color, LabelPlainColors);
}

// qualityColors in tooltips: 0x2BD360(item, items.txt record, quality) -> color.
std::uint32_t __fastcall HookNameColor(void* item, const std::uint8_t* itemsRecord, std::int32_t quality) noexcept {
    const std::uint32_t color = OriginalNameColor(item, itemsRecord, quality);
    if (!Active.load()) return color;
    return FilterColor(quality, color, TooltipPlainColors);
}

// qualityColors in the set item tooltip: its colorize call (0x2BFC6D) passes
// broken ? 1 : 2; a set color replaces the 2.
char* __fastcall HookSetNameColorize(char* buffer, std::int32_t color) noexcept {
    if (color == 2 && Active.load()) color = static_cast<std::int32_t>(FilterColor(5, 2, TooltipPlainColors));
    return NativeColorize(buffer, color);
}

// ---------------------------------------------------------------------------
//  The GetItemName thunk (taken like charm-dedup takes its thunks)
// ---------------------------------------------------------------------------
std::uint8_t* StubPage{};
constexpr std::size_t StubPageBytes = 0x1000;
constexpr std::size_t NameThunkSlot = 0;       // jmp HookGetItemName
constexpr std::size_t SetColorizeSlot = 16;    // jmp HookSetNameColorize
std::array<std::uint8_t, 6> ThunkWritten{};
bool ThunkApplied{};
std::array<std::uint8_t, 5> SetColorizeWritten{};
bool SetColorizeApplied{};

auto ReadU32(const void* base, std::size_t offset) noexcept -> std::uint32_t {
    std::uint32_t value = 0;
    std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(value));
    return value;
}

auto AllocateStubPage() noexcept -> std::uint8_t* {
    const auto* image = reinterpret_cast<const std::uint8_t*>(Base);
    const auto ntOffset = static_cast<std::size_t>(ReadU32(image, 0x3C));
    const auto imageSize = ReadU32(image, ntOffset + 0x50);
    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    const std::uintptr_t granularity = systemInfo.dwAllocationGranularity != 0 ? systemInfo.dwAllocationGranularity : 0x10000;
    const auto alignUp = [granularity](std::uintptr_t value) noexcept { return (value + granularity - 1) & ~(granularity - 1); };
    const std::uintptr_t limit = Base + GetItemNameThunkRva + 0x7FF00000ULL;
    std::uintptr_t address = alignUp(Base + imageSize);
    while (address + StubPageBytes < limit) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) == 0) break;
        const auto regionEnd = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (info.State == MEM_FREE && address + StubPageBytes <= regionEnd) {
            if (auto* page = VirtualAlloc(reinterpret_cast<LPVOID>(address), StubPageBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
                return static_cast<std::uint8_t*>(page);
            }
        }
        const auto next = alignUp(regionEnd);
        if (next <= address) break;
        address = next;
    }
    return nullptr;
}

auto InstallNameThunk() noexcept -> bool {
    if (!Context->CheckExpectedBytes(GetItemNameThunkRva, GetItemNameThunkBytes, sizeof(GetItemNameThunkBytes))) {
        LogError("LootFilter: the GetItemName thunk at RVA 0x%llX does not match D2RLoader 1.3.1.",
            static_cast<unsigned long long>(GetItemNameThunkRva));
        return false;
    }
    HMODULE core = GetModuleHandleW(L"D2RCore.dll");
    const FARPROC exported = core != nullptr ? GetProcAddress(core, "GetItemName") : nullptr;
    if (exported == nullptr) {
        LogError("LootFilter: D2RCore.dll's GetItemName export is missing.");
        return false;
    }
    if (std::memcmp(reinterpret_cast<const void*>(exported), CoreGetItemNameBytes, sizeof(CoreGetItemNameBytes)) != 0) {
        LogError("LootFilter: D2RCore's GetItemName does not match D2RLoader 1.3.1.");
        return false;
    }
    // D2RLoader may bind the game's slot only after plugins load: an empty slot
    // is fine, a slot bound to any other code is not.
    const auto slot = ReadAt<std::uintptr_t>(reinterpret_cast<const std::uint8_t*>(Base), static_cast<std::size_t>(GetItemNameSlotRva));
    if (slot != 0 && slot != reinterpret_cast<std::uintptr_t>(exported)) {
        LogError("LootFilter: the game's GetItemName slot points at other code, not D2RCore's export.");
        return false;
    }
    for (std::uint64_t site : NameCallSites) {
        std::uint8_t call[5]{ 0xE8 };
        const auto rel = static_cast<std::int32_t>(static_cast<std::int64_t>(GetItemNameThunkRva) - static_cast<std::int64_t>(site + 5));
        std::memcpy(call + 1, &rel, sizeof(rel));
        if (!Context->CheckExpectedBytes(site, call, sizeof(call))) {
            LogError("LootFilter: the GetItemName call at RVA 0x%llX does not match D2RLoader 1.3.1.", static_cast<unsigned long long>(site));
            return false;
        }
    }
    OriginalGetItemName = reinterpret_cast<GetItemNameFn>(exported);
    if (StubPage == nullptr) {
        LogError("LootFilter: no free page within jump range of the game image.");
        return false;
    }
    const auto from = Base + static_cast<std::uintptr_t>(GetItemNameThunkRva) + 5;
    const auto rel = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(StubPage + NameThunkSlot)) - static_cast<std::int64_t>(from);
    if (rel < INT32_MIN || rel > INT32_MAX) {
        LogError("LootFilter: the stub page is out of reach of the thunk.");
        return false;
    }
    ThunkWritten[0] = 0xE9;
    const auto rel32 = static_cast<std::int32_t>(rel);
    std::memcpy(ThunkWritten.data() + 1, &rel32, sizeof(rel32));
    ThunkWritten[5] = 0x90;
    if (!Context->PatchBytes(GetItemNameThunkRva, GetItemNameThunkBytes, sizeof(GetItemNameThunkBytes), ThunkWritten.data(), 6)) {
        LogError("LootFilter: the loader refused the GetItemName thunk.");
        return false;
    }
    ThunkApplied = true;
    if (std::memcmp(reinterpret_cast<const void*>(Base + static_cast<std::uintptr_t>(GetItemNameThunkRva)), ThunkWritten.data(), 6) != 0) {
        LogError("LootFilter: the GetItemName thunk does not hold the bytes that were written.");
        return false;
    }
    return true;
}

void RestoreNameThunk() noexcept {
    if (!ThunkApplied) return;
    if (Context->PatchBytes(GetItemNameThunkRva, ThunkWritten.data(), 6, GetItemNameThunkBytes, sizeof(GetItemNameThunkBytes))) {
        ThunkApplied = false;
    } else {
        LogError("LootFilter: could not restore the GetItemName thunk.");
    }
}

// The stub page next to the image: an absolute jump per 16-byte slot, all
// written before the page turns executable, so no slot changes while the game
// may run another.
auto PrepareStubPage() noexcept -> bool {
    StubPage = AllocateStubPage();
    if (StubPage == nullptr) return false;
    std::memset(StubPage, 0xCC, StubPageBytes);
    const auto put = [](std::size_t offset, const void* target) {
        const std::uint8_t jump[6]{ 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
        std::memcpy(StubPage + offset, jump, sizeof(jump));
        const auto address = reinterpret_cast<std::uint64_t>(target);
        std::memcpy(StubPage + offset + sizeof(jump), &address, sizeof(address));
    };
    put(NameThunkSlot, reinterpret_cast<const void*>(&HookGetItemName));
    put(SetColorizeSlot, reinterpret_cast<const void*>(&HookSetNameColorize));
    DWORD previous = 0;
    if (!VirtualProtect(StubPage, StubPageBytes, PAGE_EXECUTE_READ, &previous)) {
        VirtualFree(StubPage, 0, MEM_RELEASE);
        StubPage = nullptr;
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), StubPage, StubPageBytes);
    return true;
}

// The set item tooltip's colorize call goes through the stub page.
auto InstallSetNameColor() noexcept -> bool {
    if (StubPage == nullptr) return false;
    if (!Context->CheckExpectedBytes(SetNameLeadRva, SetNameColorBytes, sizeof(SetNameColorBytes))) {
        LogWarn("LootFilter: the set tooltip color call at RVA 0x%llX does not match D2RLoader 1.3.1.",
            static_cast<unsigned long long>(SetNameColorCallRva));
        return false;
    }
    const auto from = Base + static_cast<std::uintptr_t>(SetNameColorCallRva) + 5;
    const auto rel = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(StubPage + SetColorizeSlot)) - static_cast<std::int64_t>(from);
    if (rel < INT32_MIN || rel > INT32_MAX) return false;
    SetColorizeWritten[0] = 0xE8;
    const auto rel32 = static_cast<std::int32_t>(rel);
    std::memcpy(SetColorizeWritten.data() + 1, &rel32, sizeof(rel32));
    if (!Context->PatchBytes(SetNameColorCallRva, SetNameColorCallBytes, sizeof(SetNameColorCallBytes), SetColorizeWritten.data(),
                             static_cast<std::uint32_t>(SetColorizeWritten.size()))) {
        LogWarn("LootFilter: the loader refused the set tooltip color call.");
        return false;
    }
    SetColorizeApplied = true;
    if (std::memcmp(reinterpret_cast<const void*>(Base + static_cast<std::uintptr_t>(SetNameColorCallRva)), SetColorizeWritten.data(),
                    SetColorizeWritten.size()) != 0) {
        LogWarn("LootFilter: the set tooltip color call does not hold the bytes that were written.");
        return false;
    }
    return true;
}

void RestoreSetNameColor() noexcept {
    if (!SetColorizeApplied) return;
    if (Context->PatchBytes(SetNameColorCallRva, SetColorizeWritten.data(), static_cast<std::uint32_t>(SetColorizeWritten.size()),
                            SetNameColorCallBytes, sizeof(SetNameColorCallBytes))) {
        SetColorizeApplied = false;
    } else {
        LogError("LootFilter: could not restore the set tooltip color call.");
    }
}

// ---------------------------------------------------------------------------
//  Keys and game events
// ---------------------------------------------------------------------------
auto __cdecl OnNextLevel(const D2RL::PluginContext*, const D2RL::Input::ActionEvent* event, void*) noexcept -> D2RL::Input::ActionResult {
    if (!D2RL::Input::HasActionEventField(event, D2RL::Input::ActionEventRequiredSize)
        || event->kind != D2RL::Input::ActionEventKind::Pressed) {
        return D2RL::Input::ActionResult::Ignored;
    }
    try {
        LoadedFilter filter;
        Selection selection;
        {
            std::unique_lock<std::shared_mutex> lock(FilterLock);
            if (!Current.program || !Current.program->HasLevels()) return D2RL::Input::ActionResult::Handled;
            Current.level = Current.program->NextLevel(Current.level);
            CurrentSelection.levels[Current.id] = Current.level;
            filter = Current;
            selection = CurrentSelection;
        }
        Revision.fetch_add(1);
        if (!WriteSelection(selection)) LogWarn("LootFilter: selection.json could not be written.");
        QueueChat(LevelChangeLine(filter));
    } catch (...) {
    }
    return D2RL::Input::ActionResult::Handled;
}

auto __cdecl OnShowHidden(const D2RL::PluginContext*, const D2RL::Input::ActionEvent* event, void*) noexcept -> D2RL::Input::ActionResult {
    if (!D2RL::Input::HasActionEventField(event, D2RL::Input::ActionEventRequiredSize)
        || event->kind != D2RL::Input::ActionEventKind::Pressed) {
        return D2RL::Input::ActionResult::Ignored;
    }
    const bool now = !ShowHidden.load();
    ShowHidden.store(now);
    Revision.fetch_add(1);
    try {
        QueueChat(now ? "\xC3\xBF" "c4Loot filter: \xC3\xBF" "c0hidden items shown" : "\xC3\xBF" "c4Loot filter: \xC3\xBF" "c0hidden items hidden");
    } catch (...) {
    }
    return D2RL::Input::ActionResult::Handled;
}

void __cdecl OnGameEvent(const D2RL::PluginContext*, const D2RL::Lifecycle::GameplayEvent* event, void*) noexcept {
    if (event == nullptr) return;
    try {
        {
            std::lock_guard<std::recursive_mutex> lock(CacheLock);
            Cache.clear();
            DebugLabelLogged.clear();
            DebugPickupLogged.clear();
        }
        if (event->kind == D2RL::Lifecycle::GameplayEventKind::GameLeft || event->kind == D2RL::Lifecycle::GameplayEventKind::GameJoined) {
            std::lock_guard<std::mutex> lock(DroplightLock);
            for (auto& [id, entry] : Droplights) {
                DroplightNodes.erase(entry.node.get());
                RetiredNodes.push_back(std::move(entry.node));
            }
            Droplights.clear();
            DroplightChecked.clear();
            DroplightPending.clear();
            if (event->kind == D2RL::Lifecycle::GameplayEventKind::GameLeft) RetiredNodes.clear();
            std::lock_guard<std::mutex> iconLock(IconMemoryLock);
            IconMemory.clear();
        }
        if (event->kind == D2RL::Lifecycle::GameplayEventKind::LocalPlayerReady) {
            LanguagePacked.store(PackItemCode(CurrentLanguage()));
            Strings.Clear();
            Revision.fetch_add(1);
            LoadedFilter filter;
            bool playSounds = true;
            Snapshot(filter, playSounds);
            Announce(filter);
        }
    } catch (...) {
    }
}

auto RegisterAction(const char* logicalId, const char* displayName, D2RL::Input::ActionCallback callback) -> bool {
    const D2RL::Input::ActionRegistration registration{
        .structSize       = D2RL::Input::ActionRegistrationSize,
        .flags            = 0,
        .logicalId        = logicalId,
        .displayName      = displayName,
        .category         = "Loot Filter",
        .defaultPrimary   = { .key = D2RL::Input::Key::None, .modifier = D2RL::Input::Modifier::None },
        .defaultSecondary = { .key = D2RL::Input::Key::None, .modifier = D2RL::Input::Modifier::None },
        .callback         = callback,
        .userData         = nullptr,
    };
    D2RL::Input::ActionHandle handle = D2RL::Input::InvalidHandle;
    const D2RL::Input::Result result = InputSvc->registerAction(Context, &registration, &handle);
    if (result != D2RL::Input::Result::Success || handle == D2RL::Input::InvalidHandle) {
        LogError("LootFilter: the Controls entry \"%s\" was refused (result %u).", displayName, static_cast<unsigned>(result));
        return false;
    }
    return true;
}

auto RegisterGameEvents() -> bool {
    for (const auto kind : { D2RL::Lifecycle::GameplayEventKind::GameJoined, D2RL::Lifecycle::GameplayEventKind::GameLeft,
                             D2RL::Lifecycle::GameplayEventKind::LocalPlayerReady }) {
        const D2RL::Lifecycle::GameplayEventListener listener{
            .structSize = sizeof(D2RL::Lifecycle::GameplayEventListener),
            .flags      = 0,
            .kind       = kind,
            .reserved   = 0,
            .callback   = &OnGameEvent,
            .userData   = nullptr,
        };
        D2RL::Lifecycle::ListenerHandle handle = D2RL::Lifecycle::InvalidHandle;
        if (LifecycleSvc->registerGameplayEventListener(Context, &listener, &handle) != D2RL::Lifecycle::Result::Success) {
            LogError("LootFilter: a game event listener was refused.");
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
//  Console
// ---------------------------------------------------------------------------
auto StateText() -> const char* {
    switch (State) {
    case PluginState::Loading: return "loading";
    case PluginState::Active: return "active";
    case PluginState::DisabledByConfig: return "disabled by config";
    case PluginState::UnsupportedBuild: return "unsupported build (see the plugin log)";
    case PluginState::InstallFailed: return "install failed (see the plugin log)";
    }
    return "unknown";
}

auto __cdecl StatusCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
    if (command == nullptr || command->plugin == nullptr) return D2RL::ConsoleCommandResult::Failed;
    const std::string_view args = command->args != nullptr ? std::string_view(command->args, command->argsLength) : std::string_view{};
    char message[768];
    if (Trim(args) == "reload") {
        if (State != PluginState::Active) {
            command->plugin->WriteConsoleMessage("lootfilter: the plugin is not active.");
            return D2RL::ConsoleCommandResult::Failed;
        }
        try {
            LoadActiveFilter(true);
        } catch (...) {
            return D2RL::ConsoleCommandResult::Failed;
        }
    }
    LoadedFilter filter;
    bool playSounds = true;
    Snapshot(filter, playSounds);
    std::snprintf(message, sizeof(message),
        "lootfilter %s: %s. Hooks: names %s, gold %s, hide %s, label colors %s, drop alerts %s, quality colors %s, set tooltip color %s, "
        "sound volume %s, game strings %s, minimap icons %s, droplights %s, hidden items unpickable %s.",
        PluginVersion, StateText(), Hooks.names ? "on" : "off", Hooks.goldLabels ? "on" : "off", Hooks.hide ? "on" : "off",
        Hooks.labelColors ? "on" : "off", Hooks.alerts ? "on" : "off", Hooks.qualityColors ? "on" : "off",
        Hooks.setTooltipColor ? "on" : "off", Hooks.soundVolume ? "on" : "off", LocalizationSvc != nullptr ? "on" : "off", Hooks.icons ? "on" : "off",
        Hooks.droplights ? "on" : "off", Hooks.pickup ? "on" : "off");
    command->plugin->WriteConsoleMessage(message);
    if (filter.program) {
        std::snprintf(message, sizeof(message), "Filter %s \"%s\": %zu of %zu rules active, level %d (%s), %zu errors, %zu warnings, sounds %s, hidden items %s.",
            filter.id.c_str(), filter.title.c_str(), filter.program->EnabledRuleCount(), filter.program->rules.size(), filter.level,
            LevelName(*filter.program, filter.level, LanguageNow()).c_str(), filter.errors, filter.warnings, playSounds ? "on" : "off",
            ShowHidden.load() ? "shown" : "hidden");
    } else if (filter.loadFailed) {
        std::snprintf(message, sizeof(message), "No filter: %s.", filter.failure.c_str());
    } else {
        std::snprintf(message, sizeof(message), "No filter selected.");
    }
    command->plugin->WriteConsoleMessage(message);
    std::snprintf(message, sizeof(message), "Language %s. Your filters: %s. Mod filters: %s.", LanguageNow().c_str(),
        Narrow(UserFolder).c_str(), Narrow(ShippedFolder).c_str());
    command->plugin->WriteConsoleMessage(message);
    return D2RL::ConsoleCommandResult::Handled;
}

// ---------------------------------------------------------------------------
//  Installation
// ---------------------------------------------------------------------------
auto Verify(const Witness& witness) -> bool {
    if (Context->CheckExpectedBytes(witness.rva, witness.bytes, static_cast<std::uint32_t>(witness.size))) return true;
    LogError("LootFilter: %s at RVA 0x%llX does not match D2RLoader 1.3.1.", witness.what, static_cast<unsigned long long>(witness.rva));
    return false;
}

auto VerifyAll() -> bool {
    bool ok = true;
    for (const Witness& witness : GameWitnesses) ok = Verify(witness) && ok;
    return ok;
}

auto QueryServices() -> bool {
    if (Context->QueryService(&ThreadSvc) != D2RL::ServiceQueryResult::Success
        || !D2RL::HasThreadServiceField(ThreadSvc, D2RL::ThreadServiceRequiredSize)) {
        LogError("LootFilter: the thread service is missing.");
        return false;
    }
    if (Context->QueryService(&InputSvc) != D2RL::ServiceQueryResult::Success
        || !D2RL::HasInputServiceField(InputSvc, D2RL::InputServiceRequiredSize)) {
        LogError("LootFilter: the input service is missing.");
        return false;
    }
    if (Context->QueryService(&LifecycleSvc) != D2RL::ServiceQueryResult::Success
        || !D2RL::HasLifecycleServiceField(LifecycleSvc, D2RL::LifecycleServiceRequiredSize)) {
        LogError("LootFilter: the lifecycle service is missing.");
        return false;
    }
    if (Context->QueryService(&LocalizationSvc) != D2RL::ServiceQueryResult::Success
        || !D2RL::HasLocalizationServiceField(LocalizationSvc, D2RL::LocalizationServiceRequiredSize)) {
        LocalizationSvc = nullptr;
        LogWarn("LootFilter: the localization service is missing; {string:KEY} shows the key.");
    }
    if (Context->QueryService(&ResourceSvc) != D2RL::ServiceQueryResult::Success
        || !D2RL::HasResourceServiceField(ResourceSvc, D2RL::ResourceServiceRequiredSize)) {
        ResourceSvc = nullptr;
        LogWarn("LootFilter: the resource service is missing; minimap icons are off.");
    }
    return true;
}

// Each hook stands alone: when one site is already taken by another plugin or
// differs, only that feature is off and the log says which.
auto InstallHooks() -> bool {
    if (!PrepareStubPage()) LogWarn("LootFilter: no free page within jump range of the game image.");
    Hooks.names = InstallNameThunk();
    if (!Hooks.names) LogWarn("LootFilter: item names stay stock (the GetItemName thunk could not be taken).");
    Hooks.goldLabels = Context->CheckExpectedBytes(LabelTextRva + sizeof(LabelTextBytes), LabelTextTailBytes, sizeof(LabelTextTailBytes))
        && Context->InstallInlineHook(LabelTextRva, LabelTextBytes, sizeof(LabelTextBytes), &HookLabelText, &OriginalLabelText);
    if (!Hooks.goldLabels) LogWarn("LootFilter: gold labels stay stock (the hook at RVA 0x%llX was refused).", static_cast<unsigned long long>(LabelTextRva));
    Hooks.hide = Context->CheckExpectedBytes(ShowLabelRva + sizeof(ShowLabelBytes), ShowLabelTailBytes, sizeof(ShowLabelTailBytes))
        && Context->InstallInlineHook(ShowLabelRva, ShowLabelBytes, sizeof(ShowLabelBytes), &HookShowLabel, &OriginalShowLabel);
    if (!Hooks.hide) LogWarn("LootFilter: hiding is off (the hook at RVA 0x%llX was refused).", static_cast<unsigned long long>(ShowLabelRva));
    const auto installAlert = [](std::uint64_t rva, const std::uint8_t* entry, std::uint32_t entrySize, const std::uint8_t* tail,
                                 std::uint32_t tailSize, GroundActionFn hook, GroundActionFn* original) {
        if (Context->CheckExpectedBytes(rva + entrySize, tail, tailSize)
            && Context->InstallInlineHook(rva, entry, entrySize, hook, original)) {
            return true;
        }
        LogWarn("LootFilter: the ground item action at RVA 0x%llX was refused.", static_cast<unsigned long long>(rva));
        return false;
    };
    const bool newDrop = installAlert(NewDropRva, NewDropBytes, sizeof(NewDropBytes), NewDropTailBytes, sizeof(NewDropTailBytes),
                                      &HookNewDrop, &OriginalNewDrop);
    const bool handDrop = installAlert(HandDropRva, HandDropBytes, sizeof(HandDropBytes), HandDropTailBytes, sizeof(HandDropTailBytes),
                                       &HookHandDrop, &OriginalHandDrop);
    const bool seen = installAlert(SeenOnGroundRva, SeenOnGroundBytes, sizeof(SeenOnGroundBytes), SeenOnGroundTailBytes,
                                   sizeof(SeenOnGroundTailBytes), &HookSeenOnGround, &OriginalSeenOnGround);
    Hooks.alerts = newDrop || handDrop || seen;
    if (!Hooks.alerts) LogWarn("LootFilter: drop chat lines and sounds are off.");
    bool drawOk = true;
    for (const Witness& witness : DrawWitnesses) drawOk = Verify(witness) && drawOk;
    Hooks.labelColors = drawOk && Context->InstallInlineHook(DrawLabelRva, DrawLabelBytes, sizeof(DrawLabelBytes), &HookDrawLabel, &OriginalDrawLabel);
    if (!Hooks.labelColors) LogWarn("LootFilter: label backgrounds and borders are off (the label draw hook was refused).");
    Hooks.qualityColors =
        Context->CheckExpectedBytes(LabelColorRva + sizeof(LabelColorBytes), LabelColorTailBytes, sizeof(LabelColorTailBytes))
        && Context->InstallInlineHook(LabelColorRva, LabelColorBytes, sizeof(LabelColorBytes), &HookLabelColor, &OriginalLabelColor);
    if (!Hooks.qualityColors) LogWarn("LootFilter: qualityColors are off on ground labels (the hook at RVA 0x%llX was refused).",
                                      static_cast<unsigned long long>(LabelColorRva));
    NameColorVerified = Context->CheckExpectedBytes(NameColorRva, NameColorBytes, sizeof(NameColorBytes))
        && Context->CheckExpectedBytes(NameColorRva + sizeof(NameColorBytes), NameColorTailBytes, sizeof(NameColorTailBytes));
    const bool tooltipColors = NameColorVerified
        && Context->InstallInlineHook(NameColorRva, NameColorBytes, sizeof(NameColorBytes), &HookNameColor, &OriginalNameColor);
    if (!tooltipColors) {
        OriginalNameColor = nullptr;
        LogWarn("LootFilter: qualityColors are off in tooltips (the hook at RVA 0x%llX was refused).", static_cast<unsigned long long>(NameColorRva));
    }
    Hooks.qualityColors = Hooks.qualityColors && tooltipColors;
    Hooks.setTooltipColor = InstallSetNameColor();
    Hooks.soundVolume = Context->CheckExpectedBytes(SetSoundVolumeRva, SetSoundVolumeBytes, sizeof(SetSoundVolumeBytes));
    if (Hooks.soundVolume) {
        NativeSetSoundVolume = At<SetSoundVolumeFn>(SetSoundVolumeRva);
    } else {
        LogWarn("LootFilter: the game's sound volume setter at RVA 0x%llX does not match; sounds.txt sounds play at full volume.",
                static_cast<unsigned long long>(SetSoundVolumeRva));
    }
    // A frame counter, so the remembered icons are drawn once per minimap pass.
    if (Context->QueryService(&OverlaySvc) != D2RL::ServiceQueryResult::Success
        || !D2RL::HasOverlayServiceField(OverlaySvc, D2RL::OverlayServiceRequiredSize)) {
        OverlaySvc = nullptr;
    }
    if (OverlaySvc != nullptr) {
        D2RL::Overlay::CallbackRegistration registration{};
        registration.structSize = sizeof(registration);
        registration.flags = 0;
        registration.phase = D2RL::Overlay::Phase::AfterGameUi;
        registration.priority = 0;
        registration.callback = &OnFrame;
        registration.userData = nullptr;
        if (OverlaySvc->registerFrameCallback(Context, &registration, &FrameHandle) != D2RL::Overlay::Result::Success) {
            FrameHandle = D2RL::Overlay::InvalidCallbackHandle;
        }
    }
    const bool minimapReady = ResourceSvc != nullptr && ThreadSvc != nullptr
        && Context->CheckExpectedBytes(AutomapUnitRva + sizeof(AutomapUnitBytes), AutomapUnitTailBytes, sizeof(AutomapUnitTailBytes))
        && Context->CheckExpectedBytes(ProjectAutomapRva, ProjectAutomapBytes, sizeof(ProjectAutomapBytes))
        && Context->CheckExpectedBytes(UnitClientXRva, UnitClientXBytes, sizeof(UnitClientXBytes))
        && Context->CheckExpectedBytes(UnitClientYRva, UnitClientYBytes, sizeof(UnitClientYBytes))
        && Context->CheckExpectedBytes(SpriteLoadRva, SpriteLoadBytes, sizeof(SpriteLoadBytes))
        && Context->CheckExpectedBytes(SpriteDrawRva, SpriteDrawBytes, sizeof(SpriteDrawBytes))
        && Context->CheckExpectedBytes(SpriteDrawHdRva, SpriteDrawHdBytes, sizeof(SpriteDrawHdBytes));
    if (minimapReady) {
        ProjectAutomap = At<ProjectAutomapFn>(ProjectAutomapRva);
        UnitClientX    = At<UnitCoordinateFn>(UnitClientXRva);
        UnitClientY    = At<UnitCoordinateFn>(UnitClientYRva);
        SpriteLoad     = At<SpriteLoadFn>(SpriteLoadRva);
        SpriteDraw     = At<SpriteDrawFn>(SpriteDrawRva);
        SpriteDrawHd   = At<SpriteDrawHdFn>(SpriteDrawHdRva);
        Hooks.icons = Context->InstallInlineHook(AutomapUnitRva, AutomapUnitBytes, sizeof(AutomapUnitBytes), &HookAutomapUnit,
                                                 &OriginalAutomapUnit);
    }
    if (Context->CheckExpectedBytes(UnitSelectTestRva + sizeof(UnitSelectTestBytes), UnitSelectTestTailBytes, sizeof(UnitSelectTestTailBytes))
        && Context->CheckExpectedBytes(TargetSelectTestRva + sizeof(TargetSelectTestBytes), TargetSelectTestTailBytes, sizeof(TargetSelectTestTailBytes))
        && Context->CheckExpectedBytes(GameToggleRva, GameToggleBytes, sizeof(GameToggleBytes))) {
        NativeGameToggle = At<GameToggleFn>(GameToggleRva);
        const bool mouse = Context->InstallInlineHook(UnitSelectTestRva, UnitSelectTestBytes, sizeof(UnitSelectTestBytes), &HookUnitSelectTest,
                                                      &OriginalUnitSelectTest);
        const bool controller = Context->InstallInlineHook(TargetSelectTestRva, TargetSelectTestBytes, sizeof(TargetSelectTestBytes),
                                                           &HookTargetSelectTest, &OriginalTargetSelectTest);
        Hooks.pickup = mouse && controller;
    }
    if (!Hooks.pickup) LogWarn("LootFilter: hidden items can still be picked up (the select tests do not match D2RLoader 1.3.1, or a hook was refused).");
    const bool droplightReady = ResourceSvc != nullptr && ThreadSvc != nullptr
        && Context->CheckExpectedBytes(OverlayAddedRva + sizeof(OverlayAddedBytes), OverlayAddedTailBytes, sizeof(OverlayAddedTailBytes))
        && Context->CheckExpectedBytes(OverlayRemovedRva, OverlayRemovedBytes, sizeof(OverlayRemovedBytes))
        && Context->CheckExpectedBytes(UnitRemovedRva + sizeof(UnitRemovedBytes), UnitRemovedTailBytes, sizeof(UnitRemovedTailBytes))
        && Context->CheckExpectedBytes(OverlayPathRva + sizeof(OverlayPathBytes), OverlayPathTailBytes, sizeof(OverlayPathTailBytes))
        && Context->CheckExpectedBytes(GameStringRva, GameStringBytes, sizeof(GameStringBytes));
    if (droplightReady) {
        NativeOverlayRemoved = At<OverlayPostFn>(OverlayRemovedRva);
        NativeGameString     = At<GameStringFn>(GameStringRva);
        const bool pathHook  = Context->InstallInlineHook(OverlayPathRva, OverlayPathBytes, sizeof(OverlayPathBytes), &HookOverlayPath, &OriginalOverlayPath);
        const bool addHook   = Context->InstallInlineHook(OverlayAddedRva, OverlayAddedBytes, sizeof(OverlayAddedBytes), &HookOverlayAdded, &OriginalOverlayAdded);
        const bool unitHook  = Context->InstallInlineHook(UnitRemovedRva, UnitRemovedBytes, sizeof(UnitRemovedBytes), &HookUnitRemoved, &OriginalUnitRemoved);
        Hooks.droplights = pathHook && addHook && unitHook;
    }
    if (!Hooks.droplights) LogWarn("LootFilter: droplights are off (the overlay code does not match D2RLoader 1.3.1, a hook was refused, or a service is missing).");
    if (!Hooks.icons) {
        LogWarn("LootFilter: minimap icons are off (the minimap code at RVA 0x%llX does not match, another plugin holds that hook, "
                "or a service is missing).", static_cast<unsigned long long>(AutomapUnitRva));
    }
    return Hooks.names || Hooks.goldLabels || Hooks.hide || Hooks.alerts || Hooks.labelColors || Hooks.qualityColors || Hooks.icons;
}

constexpr D2RL::PluginInfo PluginInfoData{
    .infoSize    = D2RL::PluginInfoSize,
    .abiVersion  = D2RL_PLUGIN_ABI_VERSION,
    .id          = PluginIdText,
    .name        = "Loot Filter",
    .version     = PluginVersion,
    .author      = "CelestialRayOne",
    .description = "D2RLAN-style loot filters in JSON: item names, hidden labels, label and name colors, drop chat lines and sounds.",
    .flags       = D2RL::PluginFlags::Client | D2RL::PluginFlags::NativeHooks,
};

}  // namespace
}  // namespace CelestialRayOne::LootFilter

using namespace CelestialRayOne::LootFilter;

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
    return &PluginInfoData;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
    Context = context;
    if (Context == nullptr || Context->exeBase == 0) return false;
    Base = Context->exeBase;
    try {
        if (!Context->RegisterConsoleCommand("lootfilter", &StatusCommand, "Loot filter state; \"lootfilter reload\" reads the filter again.")) {
            LogWarn("LootFilter: the console command could not be registered.");
        }
        ReadConfiguration();
        if (!Config.enabled) {
            State = PluginState::DisabledByConfig;
            LogInfo("LootFilter: disabled by config.");
            return true;
        }
        if (!VerifyAll()) {
            State = PluginState::UnsupportedBuild;
            LogError("LootFilter: this D2R build does not match; nothing was installed.");
            return true;
        }
        BindNatives();
        if (!QueryServices()) {
            State = PluginState::InstallFailed;
            return true;
        }
        ResolveFolders();
        LoadActiveFilter(false);
        if (!InstallHooks()) {
            State = PluginState::InstallFailed;
            LogError("LootFilter: no hook could be installed; the game runs stock.");
            return true;
        }
        if (!RegisterGameEvents()) LogWarn("LootFilter: drop alerts may repeat across games (game events unavailable).");
        if (!RegisterAction("loot-filter-next-level", "Next Filter Level", &OnNextLevel)
            || !RegisterAction("loot-filter-show-hidden", "Show Hidden Items", &OnShowHidden)) {
            LogWarn("LootFilter: the Controls keys are unavailable; the launcher still switches filters and levels.");
        }
        WatcherStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (WatcherStop != nullptr) Watcher = std::thread(WatchFolders);
        Active.store(true);
        State = PluginState::Active;
        LogInfo("LootFilter %s: active. Your filters: %s", PluginVersion, Narrow(UserFolder).c_str());
    } catch (...) {
        State = PluginState::InstallFailed;
        LogError("LootFilter: installation failed with an exception.");
    }
    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    Active.store(false);
    if (OverlaySvc != nullptr && FrameHandle != D2RL::Overlay::InvalidCallbackHandle) {
        (void)OverlaySvc->unregisterFrameCallback(Context, FrameHandle);
        FrameHandle = D2RL::Overlay::InvalidCallbackHandle;
    }
    if (WatcherStop != nullptr) {
        SetEvent(WatcherStop);
        try {
            if (Watcher.joinable()) Watcher.join();
        } catch (...) {
        }
        CloseHandle(WatcherStop);
        WatcherStop = nullptr;
    }
    RestoreNameThunk();
    RestoreSetNameColor();
    UnregisterIcons();
    sound::Shutdown();
}
