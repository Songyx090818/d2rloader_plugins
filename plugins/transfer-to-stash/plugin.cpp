// Transfer to Stash
//
// A "Transfer to Stash" button on the inventory panel, shown only while the
// stash is open, that sends every Advanced Stash item in the inventory and in
// the Horadric Cube to the stash. Where the gold display sits is up to the
// mod's own inventory layout; the plugin does not move it.
//
// Everything below was read out of the D2RLoader 1.3.1 process image, and
// every entry used here is checked byte for byte at load. Nothing is installed
// when a check fails.
//
// ---------------------------------------------------------------------------
// 1. The Ctrl+click this copies, 2AAAA0 (the whole function)
// ---------------------------------------------------------------------------
//   Both inventory click handlers (228AB0 legacy, 2C7540 modern) run it on a
//   Ctrl+click while the stash (UI state 18h) is open, once 15A0B0 (item) says
//   the item may go to the Advanced Stash. 15A0B0 checks UI state 18h, then
//   the item class in the Advanced Stash registry (15F320), which only holds
//   classes that have a slot in the stash layout. 2AAAA0 then does:
//     item = widget->vtable[C8h](widget)
//     if (1C7360(item) == 0)                         the item is not busy
//         dest = 46DA50(9A480(8B2D0()))              the Advanced Stash owner
//         if (dest != 0)
//             placement = { 0, 0, engaged 0 }
//             15F8B0(item, dest, 4, 36CFE0(item), 1, &placement)
//     1A0780(3, 0, 0, 0, 0)                          sound 3
//   The plugin runs the same sequence for every item, with the widget lookup
//   replaced by a walk over the player's items (34A360 -> 388C10 / 38ABA0),
//   keeping those on page 0 (inventory) and page 3 (cube). Sound 3 plays once
//   per press instead of once per item.
//
// ---------------------------------------------------------------------------
// 2. Why cube items go straight to the stash
// ---------------------------------------------------------------------------
//   15F8B0 with mode 1 and an owner carrying state 209 (the Advanced Stash)
//   never looks at the source page: it checks the class registration again
//   (15F320) and calls 159A30, which sends packet 63h, operation 0, item GUID,
//   owner GUID. The server handler 4C4AF0 -> 4F92F0 then needs:
//     - 46DC00: the item's items.txt record +1BDh, which is misc.txt
//       AdvancedStashStackable, not 0
//     - the owner's slot for the class, when it has one, below its cap
//     - 472590 lifting the item from where it sits. 474220 has its own page 3
//       (cube) branch, and 474700 accepts pages 0 and 3 anywhere (page 4 only
//       near the bank).
//   So a cube item needs no stop in the inventory and no free inventory space.
//
// ---------------------------------------------------------------------------
// 3. Busy items
// ---------------------------------------------------------------------------
//   When 15F8B0 sends a transfer it calls 1C6AC0(item), which gives the item a
//   mode 2 stat list (state 54). 1C7360 reports such an item as busy until the
//   server answers, so pressing the button again before that skips it.
//   1C7360 is called but not checked: the whirlwind plugin hooks it.
//
// ---------------------------------------------------------------------------
// 4. The inventory panel, 22BA70 (hooked)
// ---------------------------------------------------------------------------
//   PlayerInventoryPanel's update, slot 2 of the table at 1CE36D0 (its only
//   reference). It refreshes gold_amount and gold_button, switches the gold
//   button to deposit while UI state 18h is open, and ends in the base widget
//   update. The hook places the button, then runs it.
//     856220 (widget, name)       direct child by name: children at +58h,
//                                 count at +60h, name at child +8h
//     8562A0 (widget, rectOut)    rect at +70h {x, y, width, height} in parent
//                                 space; +52h means fill the parent
//     856FB0 (widget, point)      writes x and y at +70h / +74h
//     widget vtable +48h / +50h   set enabled / visible: for ButtonWidget
//                                 837D0 writes +50h, F02A0 writes +51h
//     widget vtable +58h          the widget's type; a type's parent at +10h
//     86F0B0 / 2C5B80             ButtonWidget / InventoryGridWidget types
//   The widget parent is at +30h (8562A0 follows it for fill-parent rects).
//   Grid width: the grid's own rect width, or else cellCount.x * cellSize.x,
//   the layout fields the game loads the grid with: InventoryGridWidget
//   cellCount at +628h (field table 236C1B0), InventoryItemWidget cellSize at
//   +5B8h (field table 236A530).
//
// ---------------------------------------------------------------------------
// 5. The button
// ---------------------------------------------------------------------------
//   Two ButtonWidgets added to the stock PlayerInventory panel through the
//   SDK, one for the keyboard/mouse layouts and one for the controller
//   layouts. Their sprites are embedded in the DLL and handed to D2RCore as
//   plugin resources: frame 0 normal, 1 pressed, 2 disabled, 3 hovered.
//     keyboard/mouse  368x58 per frame (184x29 px at 1080p), small panel font,
//                     right edge 54 units (27 px) inside the grid's right edge
//     controller      404x82 per frame (202x41 px at 1080p), one font size up,
//                     right edge on the grid frame's outer edge (10 units
//                     outside the cells), A prompt "Transfer to Stash"
//   The game draws the labels with its own font. ButtonWidget takes its size
//   from its sprite frame (870310 copies the image's size into the button
//   rect), so a button stays hidden until its sprite is loaded and that size
//   is known. Each sits under the grid, centred on the gold row. On a
//   controller it is reached from gold_button (the mod's layout points
//   gold_button's "right" at it). The click message is consumed by the
//   plugin's UI message listener, which queues the transfer on the UI thread.
//   The plugin moves only its own button; the gold widgets are only read, for
//   the row the button sits on.
//
// ---------------------------------------------------------------------------
// 6. The Controls key
// ---------------------------------------------------------------------------
//   "Transfer to Stash" in Options > Controls, in its own "Stash" section, with
//   no default key, through the SDK's input service. A press while the stash
//   is open queues the same transfer as the button; a key held down counts as
//   one press. The input service runs the handler during input processing, so
//   the handler only reads the stash UI state (0CE500, a byte read) and queues
//   the work on the UI thread.

#include <D2RLPlugin/api.h>

#include "resource_ids.h"

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
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

namespace {

constexpr char PluginIdText[]  = "celestialrayone.transfer-to-stash";
constexpr char PluginVersion[] = "1.4.0";
constexpr char SourceFile[]    = "transfer-to-stash";

const D2RL::PluginContext* Context{};
std::uintptr_t             Base{};

// ---------------------------------------------------------------------------
// Addresses (RVAs into the D2RLoader 1.3.1 image)
// ---------------------------------------------------------------------------

// Hooked
constexpr std::uint64_t InventoryUpdateRva     = 0x22BA70;

// Called
constexpr std::uint64_t LocalDataContextRva    = 0x08B2D0;
constexpr std::uint64_t LocalPlayerRva         = 0x09A480;
constexpr std::uint64_t UiStateOpenRva         = 0x0CE500;
constexpr std::uint64_t CanQuickMoveRva        = 0x15A0B0;
constexpr std::uint64_t TransferRva            = 0x15F8B0;
constexpr std::uint64_t PlaySoundRva           = 0x1A0780;
constexpr std::uint64_t InteractionBlockedRva  = 0x1C7360;   // not checked, see section 3
constexpr std::uint64_t GridTypeRva            = 0x2C5B80;
constexpr std::uint64_t UnitInventoryRva       = 0x34A360;
constexpr std::uint64_t UnitTypeRva            = 0x34B9D0;
constexpr std::uint64_t ItemPageRva            = 0x36CFE0;
constexpr std::uint64_t FirstItemRva           = 0x388C10;
constexpr std::uint64_t NextItemRva            = 0x38ABA0;
constexpr std::uint64_t StashDestinationRva    = 0x46DA50;
constexpr std::uint64_t FindChildRva           = 0x856220;
constexpr std::uint64_t WidgetRectRva          = 0x8562A0;
constexpr std::uint64_t SetWidgetPositionRva   = 0x856FB0;
constexpr std::uint64_t ButtonTypeRva          = 0x86F0B0;

// Checked only: the Ctrl+click copied here and the ButtonWidget flag setters
constexpr std::uint64_t QuickMoveActionRva     = 0x2AAAA0;
constexpr std::uint64_t ButtonSetEnabledRva    = 0x0837D0;
constexpr std::uint64_t ButtonSetVisibleRva    = 0x0F02A0;

// Data: the layout field tables the game loads the grid widget with
constexpr std::uint64_t CellCountFieldRva      = 0x236C1B0;   // InventoryGridWidget "cellCount"
constexpr std::uint64_t CellCountNameRva       = 0x1CF39E0;
constexpr std::uint64_t CellSizeFieldRva       = 0x236A530;   // InventoryItemWidget "cellSize"
constexpr std::uint64_t CellSizeNameRva        = 0x1CF1530;
constexpr std::size_t   FieldNameOffset        = 0x00;
constexpr std::size_t   FieldOffsetOffset      = 0x10;
constexpr std::uint64_t CellCountOffset        = 0x628;
constexpr std::uint64_t CellSizeOffset         = 0x5B8;

// Widget layout
constexpr std::size_t WidgetParentOffset       = 0x30;
constexpr std::size_t WidgetEnabledOffset      = 0x50;
constexpr std::size_t WidgetVisibleOffset      = 0x51;
constexpr std::size_t WidgetFillParentOffset   = 0x52;
constexpr std::size_t WidgetChildrenOffset     = 0x58;
constexpr std::size_t WidgetChildCountOffset   = 0x60;
constexpr std::size_t VtableSetEnabled         = 0x48 / 8;
constexpr std::size_t VtableSetVisible         = 0x50 / 8;
constexpr std::size_t VtableType               = 0x58 / 8;
constexpr std::size_t TypeParentOffset         = 0x10;
constexpr std::uint64_t MaxChildren            = 4096;
constexpr std::size_t MaxTypeDepth             = 64;

// Game values
constexpr std::int32_t StashUiState            = 0x18;
constexpr std::int32_t ItemUnitType            = 4;
constexpr std::uint8_t InventoryPage           = 0;
constexpr std::uint8_t CubePage                = 3;
constexpr std::uint8_t AdvancedStashPage       = 4;
constexpr std::uint8_t QuickMoveMode           = 1;
constexpr std::int32_t QuickMoveSound          = 3;
constexpr std::size_t  MaxItemsWalked          = 4096;
constexpr std::size_t  MaxBatch                = 1024;

// Widget names
constexpr char GridName[]        = "grid";
constexpr char GoldButtonName[]  = "gold_button";
constexpr char GoldAmountName[]  = "gold_amount";

// ---------------------------------------------------------------------------
// The button
// ---------------------------------------------------------------------------

constexpr std::string_view ClickTarget  = "PanelManager";
constexpr std::string_view ClickCommand = "OpenPanel";
constexpr std::string_view ClickText    = "CelestialRayOneTransferToStash";

// Text in the game's font over the plugin's own sprite. Normal and pressed use
// the gold of the inventory's gold amount; hover is lighter. The position is
// set at run time (section 4). "filename" is the sprite's resource path
// relative to data/hd/global/ui, without the extension.
constexpr char DesktopLayout[] = R"json({
    "type": "ButtonWidget", "name": "celestialrayone.transfer-to-stash/TransferButton",
    "fields": {
        "rect": { "x": 0, "y": 0 },
        "filename": "d2rloader/celestialrayone.transfer-to-stash/transferbutton",
        "normalFrame": 0,
        "pressedFrame": 1,
        "disabledFrame": 2,
        "hoveredFrame": 3,
        "textString": "Transfer to Stash",
        "pointSize": "$SmallPanelFontSize",
        "textColor": { "r": 207, "g": 193, "b": 119, "a": 255 },
        "textHoverColor": { "r": 240, "g": 226, "b": 160, "a": 255 },
        "textDisabledColor": { "r": 110, "g": 105, "b": 95, "a": 255 },
        "pressLabelOffset": { "x": 0, "y": 2 },
        "onClickMessage": "PanelManager:OpenPanel:CelestialRayOneTransferToStash"
    }
}
)json";

constexpr char ControllerLayout[] = R"json({
    "type": "ButtonWidget", "name": "celestialrayone.transfer-to-stash/TransferButtonController",
    "fields": {
        "rect": { "x": 0, "y": 0 },
        "filename": "d2rloader/celestialrayone.transfer-to-stash/transferbuttoncontroller",
        "navigation": {
            "left": { "name": "gold_button" },
            "up": { "name": "grid" }
        },
        "buttonLegendAction": "select",
        "buttonLegendString": "Transfer to Stash",
        "normalFrame": 0,
        "pressedFrame": 1,
        "disabledFrame": 2,
        "hoveredFrame": 3,
        "textString": "Transfer to Stash",
        "pointSize": "$SmallMediumFontSize",
        "textColor": { "r": 207, "g": 193, "b": 119, "a": 255 },
        "textHoverColor": { "r": 240, "g": 226, "b": 160, "a": 255 },
        "textDisabledColor": { "r": 110, "g": 105, "b": 95, "a": 255 },
        "pressLabelOffset": { "x": 0, "y": 2 },
        "onClickMessage": "PanelManager:OpenPanel:CelestialRayOneTransferToStash"
    }
}
)json";

struct ButtonSpec {
    const char*                    label;            // for the log and the console
    const char*                    localId;
    const char*                    widgetName;
    const char*                    layoutPath;
    const char*                    layout;
    std::uint64_t                  layoutSize;
    int                            spriteId;         // RCDATA ids, resource_ids.h
    int                            lowendSpriteId;
    const char*                    spritePath;
    const char*                    lowendSpritePath;
    D2RL::Panels::ChildLayoutFlags flags;
    std::int32_t                   rightInset;       // layout units inside the grid's right edge (negative: outside)
};

const std::array<ButtonSpec, 2> Buttons{ {
    { "keyboard/mouse", "TransferButton", "celestialrayone.transfer-to-stash/TransferButton",
      "data/global/ui/layouts/celestialrayone.transfer-to-stash/TransferButtonhd.json",
      DesktopLayout, sizeof(DesktopLayout) - 1,
      TRANSFER_BUTTON_SPRITE_ID, TRANSFER_BUTTON_LOWEND_SPRITE_ID,
      "data/hd/global/ui/d2rloader/celestialrayone.transfer-to-stash/transferbutton.sprite",
      "data/hd/global/ui/d2rloader/celestialrayone.transfer-to-stash/transferbutton.lowend.sprite",
      D2RL::Panels::ChildLayoutFlags::KeyboardMouseOnly, 54 },
    { "controller", "TransferButtonController", "celestialrayone.transfer-to-stash/TransferButtonController",
      "data/global/ui/layouts/celestialrayone.transfer-to-stash/TransferButtonControllerhd.json",
      ControllerLayout, sizeof(ControllerLayout) - 1,
      TRANSFER_BUTTON_CONTROLLER_SPRITE_ID, TRANSFER_BUTTON_CONTROLLER_LOWEND_SPRITE_ID,
      "data/hd/global/ui/d2rloader/celestialrayone.transfer-to-stash/transferbuttoncontroller.sprite",
      "data/hd/global/ui/d2rloader/celestialrayone.transfer-to-stash/transferbuttoncontroller.lowend.sprite",
      D2RL::Panels::ChildLayoutFlags::ControllerOnly, -10 },
} };

// ---------------------------------------------------------------------------
// Byte witnesses
// ---------------------------------------------------------------------------

// 22BA70: mov r11,rsp / mov [r11+20h],rbx / push rbp, rsi, rdi, r13, r14 /
// lea rbp,[rsp-70h] / sub rsp,170h
constexpr std::uint8_t InventoryUpdateBytes[]{
    0x4C,0x8B,0xDC,0x49,0x89,0x5B,0x20,0x55,0x56,0x57,0x41,0x55,0x41,0x56,0x48,0x8D,
    0x6C,0x24,0x90,0x48,0x81,0xEC,0x70,0x01,0x00,0x00 };
constexpr std::uint8_t QuickMoveActionBytes[]{
    0x40,0x53,0x48,0x83,0xEC,0x40,0x48,0x8B,0x01,0xFF,0x90,0xC8,0x00,0x00,0x00,0x48,
    0x8B,0xD8,0x48,0x85,0xC0,0x74,0x60,0x48,0x8B,0xC8,0xE8,0xA1,0xC8,0xF1,0xFF,0x85,
    0xC0,0x75,0x54,0x48,0x89,0x7C,0x24,0x50,0xE8,0x03,0x08,0xDE,0xFF,0x8B,0xC8,0xE8,
    0xAC,0xF9,0xDE,0xFF,0x48,0x8B,0xC8,0xE8,0x74,0x2F,0x1C,0x00,0x48,0x8B,0xF8,0x48,
    0x85,0xC0,0x74,0x2E,0x48,0x8B,0xCB,0xC6,0x44,0x24,0x38,0x00,0xE8,0xEF,0x24,0x0C,
    0x00,0x48,0x8D,0x4C,0x24,0x30,0x44,0x0F,0xB6,0xC8,0x48,0x89,0x4C,0x24,0x28,0x41,
    0xB0,0x04,0x48,0x8B,0xCB,0xC6,0x44,0x24,0x20,0x01,0x48,0x8B,0xD7,0xE8,0x9E,0x4D,
    0xEB,0xFF,0x48,0x8B,0x7C,0x24,0x50,0x33,0xD2,0xC6,0x44,0x24,0x20,0x00,0x45,0x33,
    0xC9,0x45,0x33,0xC0,0x8D,0x4A,0x03,0xE8,0x54,0x5C,0xEF,0xFF,0x48,0x83,0xC4,0x40,
    0x5B,0xC3 };
constexpr std::uint8_t LocalDataContextBytes[]{ 0x8B,0x05,0x2E,0x84,0x99,0x02,0xC3 };
constexpr std::uint8_t LocalPlayerBytes[]{
    0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x83,0xF9,0x08,0x0F,0x83,0x85,
    0x00,0x00,0x00,0x8B,0xD9,0x48,0x89,0x5C,0x24,0x38,0x48,0x83,0xFB,0x08,0x72,0x19 };
constexpr std::uint8_t UiStateOpenBytes[]{
    0x48,0x63,0xC1,0x48,0x8D,0x0D,0x96,0xC8,0x95,0x02,0x0F,0xB6,0x04,0x08,0xC3 };
constexpr std::uint8_t CanQuickMoveBytes[]{
    0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0xB9,0x18,0x00,0x00,0x00,0xE8,0x3D,
    0x44,0xF7,0xFF,0x84,0xC0,0x74,0x3E,0xE8,0xB4,0x12,0xF3,0xFF,0x0F,0xB6,0xC8,0xBA };
constexpr std::uint8_t TransferBytes[]{
    0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0xAC,
    0x24,0x08,0xFF,0xFF,0xFF,0x48,0x81,0xEC,0xF8,0x01,0x00,0x00,0x48,0x8B,0x05,0xF5 };
constexpr std::uint8_t PlaySoundBytes[]{
    0x40,0x55,0x56,0x57,0x41,0x56,0x41,0x57,0x48,0x8D,0x6C,0x24,0xD1,0x48,0x81,0xEC,
    0x00,0x01,0x00,0x00,0x48,0x8B,0x05,0x2D,0xAB,0x82,0x02,0x48,0x33,0xC4,0x48,0x89 };
constexpr std::uint8_t GridTypeBytes[]{
    0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8D,0x0D,0x03,0xD7,0xA1,0x01,0xE8,0xFE,0xD4,
    0x58,0x00,0x48,0x85,0xC0,0x75,0x13,0x48,0x8D,0x4C,0x24,0x30,0x88,0x44,0x24,0x30 };
constexpr std::uint8_t UnitInventoryBytes[]{
    0x48,0x89,0x5C,0x24,0x18,0x56,0x48,0x83,0xEC,0x20,0x48,0x8B,0xF1,0x48,0x85,0xC9,
    0x75,0x13,0x88,0x4C,0x24,0x30,0x48,0x8D,0x4C,0x24,0x30,0xE8,0x70,0xCC,0xFF,0xFF };
constexpr std::uint8_t UnitTypeBytes[]{
    0x48,0x83,0xEC,0x28,0x48,0x85,0xC9,0x75,0x1D,0x88,0x4C,0x24,0x30,0x48,0x8D,0x4C,
    0x24,0x30,0xE8,0x39,0x9E,0xFF,0xFF,0x84,0xC0,0x74,0x01,0xCC,0xB8,0x06,0x00,0x00 };
constexpr std::uint8_t ItemPageBytes[]{
    0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0x48,0x85,0xC9,0x74,0x0A,0xE8,0xDD,
    0xE9,0xFD,0xFF,0x83,0xF8,0x04,0x74,0x19,0x48,0x8D,0x4C,0x24,0x30,0xC6,0x44,0x24 };
constexpr std::uint8_t FirstItemBytes[]{
    0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0x48,0x85,0xC9,0x74,0x2E,0x81,0x39,
    0x04,0x03,0x02,0x01,0x74,0x1C,0x48,0x8D,0x4C,0x24,0x30,0xC6,0x44,0x24,0x30,0x00 };
constexpr std::uint8_t NextItemBytes[]{
    0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0x48,0x85,0xC9,0x75,0x10,0x88,0x4C,
    0x24,0x30,0x48,0x8D,0x4C,0x24,0x30,0xE8,0x84,0x98,0xFF,0xFF,0xEB,0x67,0xE8,0x0D };
constexpr std::uint8_t StashDestinationBytes[]{
    0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x85,0xC9,0x75,0x1B,0x88,0x4C,0x24,0x30,0x48,
    0x8D,0x4C,0x24,0x30,0xE8,0xC7,0xF3,0xFF,0xFF,0x84,0xC0,0x74,0x01,0xCC,0x33,0xC0 };
constexpr std::uint8_t FindChildBytes[]{
    0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xEC,0x20,0x48,
    0x8B,0x59,0x58,0x48,0x8B,0xF2,0x48,0x8B,0x41,0x60,0x48,0x8D,0x3C,0xC3,0x48,0x3B };
constexpr std::uint8_t WidgetRectBytes[]{
    0x40,0x53,0x48,0x83,0xEC,0x30,0x80,0x79,0x52,0x00,0x48,0x8B,0xDA,0x74,0x2A,0x48,
    0x8B,0x49,0x30,0x48,0x8D,0x54,0x24,0x20,0xE8,0xE3,0xFF,0xFF,0xFF,0x33,0xC0,0x48 };
constexpr std::uint8_t SetWidgetPositionBytes[]{
    0x48,0x89,0x5C,0x24,0x10,0x57,0x48,0x83,0xEC,0x20,0x80,0x79,0x52,0x00,0x48,0x8B,
    0xFA,0x48,0x8B,0xD9,0x74,0x14,0x48,0x8D,0x4C,0x24,0x30,0xC6,0x44,0x24,0x30,0x00,
    0xE8,0x8B,0xDA,0xFF,0xFF,0x84,0xC0,0x74,0x01,0xCC,0x48,0x8B,0x07,0x89,0x43,0x70,
    0x48,0xC1,0xE8,0x20,0x89,0x43,0x74,0x48,0x8B,0x5C,0x24,0x38,0x48,0x83,0xC4,0x20,
    0x5F,0xC3 };
constexpr std::uint8_t ButtonTypeBytes[]{
    0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8D,0x0D,0x63,0x24,0x48,0x01,0xE8,0xCE,0x3F,
    0xFE,0xFF,0x48,0x85,0xC0,0x75,0x13,0x48,0x8D,0x4C,0x24,0x30,0x88,0x44,0x24,0x30 };
constexpr std::uint8_t ButtonSetEnabledBytes[]{ 0x88,0x51,0x50,0xC3 };   // mov [rcx+50h],dl / ret
constexpr std::uint8_t ButtonSetVisibleBytes[]{ 0x88,0x51,0x51,0xC3 };   // mov [rcx+51h],dl / ret

struct Witness {
    std::uint64_t       rva;
    const std::uint8_t* bytes;
    std::uint32_t       size;
    const char*         name;
};

template <std::size_t Size>
constexpr auto W(std::uint64_t rva, const std::uint8_t (&bytes)[Size], const char* name) noexcept -> Witness {
    return Witness{ rva, bytes, static_cast<std::uint32_t>(Size), name };
}

const std::array<Witness, 21> Witnesses{
    W(InventoryUpdateRva, InventoryUpdateBytes, "inventory panel update"),
    W(QuickMoveActionRva, QuickMoveActionBytes, "Ctrl+click Advanced Stash action"),
    W(LocalDataContextRva, LocalDataContextBytes, "local data context"),
    W(LocalPlayerRva, LocalPlayerBytes, "local player"),
    W(UiStateOpenRva, UiStateOpenBytes, "UI state open"),
    W(CanQuickMoveRva, CanQuickMoveBytes, "Advanced Stash eligibility"),
    W(TransferRva, TransferBytes, "item transfer request"),
    W(PlaySoundRva, PlaySoundBytes, "play sound"),
    W(GridTypeRva, GridTypeBytes, "InventoryGridWidget type"),
    W(UnitInventoryRva, UnitInventoryBytes, "unit inventory"),
    W(UnitTypeRva, UnitTypeBytes, "unit type"),
    W(ItemPageRva, ItemPageBytes, "item page"),
    W(FirstItemRva, FirstItemBytes, "first inventory item"),
    W(NextItemRva, NextItemBytes, "next inventory item"),
    W(StashDestinationRva, StashDestinationBytes, "Advanced Stash owner"),
    W(FindChildRva, FindChildBytes, "child widget by name"),
    W(WidgetRectRva, WidgetRectBytes, "widget rect"),
    W(SetWidgetPositionRva, SetWidgetPositionBytes, "set widget position"),
    W(ButtonTypeRva, ButtonTypeBytes, "ButtonWidget type"),
    W(ButtonSetEnabledRva, ButtonSetEnabledBytes, "ButtonWidget set enabled"),
    W(ButtonSetVisibleRva, ButtonSetVisibleBytes, "ButtonWidget set visible"),
};

// ---------------------------------------------------------------------------
// Native functions
// ---------------------------------------------------------------------------

struct WidgetRect {
    std::int32_t x;
    std::int32_t y;
    std::int32_t width;
    std::int32_t height;
};
static_assert(sizeof(WidgetRect) == 16);

// The placement 15F8B0 fills in: x and y, and whether they were found.
struct TransferPlacement {
    std::int32_t x;
    std::int32_t y;
    std::uint8_t engaged;
    std::uint8_t padding[7];
};
static_assert(sizeof(TransferPlacement) == 16 && offsetof(TransferPlacement, engaged) == 8);

using LocalDataContextFn   = std::uint32_t(__fastcall*)();
using LocalPlayerFn        = void*(__fastcall*)(std::uint32_t index);
using UiStateOpenFn        = std::uint8_t(__fastcall*)(std::int32_t state);
using CanQuickMoveFn       = bool(__fastcall*)(void* item);
using TransferFn           = std::uint8_t(__fastcall*)(void* item, void* owner, std::uint8_t destinationPage,
                                                       std::uint8_t sourcePage, std::uint8_t mode,
                                                       TransferPlacement* placement);
using PlaySoundFn          = std::uint64_t(__fastcall*)(std::int32_t sound, void* unit, std::int32_t delay,
                                                        std::int32_t option, std::uint8_t flag);
using InteractionBlockedFn = std::int32_t(__fastcall*)(void* item);
using TypeFn               = void*(__fastcall*)();
using UnitInventoryFn      = void*(__fastcall*)(void* unit, const char* file, std::int32_t line);
using UnitTypeFn           = std::int32_t(__fastcall*)(void* unit);
using ItemPageFn           = std::uint8_t(__fastcall*)(void* item);
using FirstItemFn          = void*(__fastcall*)(void* inventory);
using NextItemFn           = void*(__fastcall*)(void* item);
using StashDestinationFn   = void*(__fastcall*)(void* player);
using FindWidgetFn         = void*(__fastcall*)(void* parent, const char* name);
using WidgetRectFn         = WidgetRect*(__fastcall*)(void* widget, WidgetRect* rect);
using SetWidgetPositionFn  = void(__fastcall*)(void* widget, const std::int32_t* point);
using InventoryUpdateFn    = std::uint64_t(__fastcall*)(void* panel);
using WidgetBoolFn         = void(__fastcall*)(void* widget, bool value);
using WidgetTypeFn         = void*(__fastcall*)(void* widget);

LocalDataContextFn   LocalDataContext{};
LocalPlayerFn        LocalPlayer{};
UiStateOpenFn        UiStateOpen{};
CanQuickMoveFn       CanQuickMove{};
TransferFn           Transfer{};
PlaySoundFn          PlaySound{};
InteractionBlockedFn InteractionBlocked{};
TypeFn               GridType{};
TypeFn               ButtonType{};
UnitInventoryFn      UnitInventory{};
UnitTypeFn           UnitType{};
ItemPageFn           ItemPage{};
FirstItemFn          FirstItem{};
NextItemFn           NextItem{};
StashDestinationFn   StashDestination{};
FindWidgetFn         FindChild{};
WidgetRectFn         GetWidgetRect{};
SetWidgetPositionFn  SetWidgetPosition{};
InventoryUpdateFn    OriginalInventoryUpdate{};

template <typename Function>
auto At(std::uint64_t rva) noexcept -> Function {
    return reinterpret_cast<Function>(Base + rva);
}

void BindNatives() {
    LocalDataContext   = At<LocalDataContextFn>(LocalDataContextRva);
    LocalPlayer        = At<LocalPlayerFn>(LocalPlayerRva);
    UiStateOpen        = At<UiStateOpenFn>(UiStateOpenRva);
    CanQuickMove       = At<CanQuickMoveFn>(CanQuickMoveRva);
    Transfer           = At<TransferFn>(TransferRva);
    PlaySound          = At<PlaySoundFn>(PlaySoundRva);
    InteractionBlocked = At<InteractionBlockedFn>(InteractionBlockedRva);
    GridType           = At<TypeFn>(GridTypeRva);
    ButtonType         = At<TypeFn>(ButtonTypeRva);
    UnitInventory      = At<UnitInventoryFn>(UnitInventoryRva);
    UnitType           = At<UnitTypeFn>(UnitTypeRva);
    ItemPage           = At<ItemPageFn>(ItemPageRva);
    FirstItem          = At<FirstItemFn>(FirstItemRva);
    NextItem           = At<NextItemFn>(NextItemRva);
    StashDestination   = At<StashDestinationFn>(StashDestinationRva);
    FindChild          = At<FindWidgetFn>(FindChildRva);
    GetWidgetRect      = At<WidgetRectFn>(WidgetRectRva);
    SetWidgetPosition  = At<SetWidgetPositionFn>(SetWidgetPositionRva);
}

// ---------------------------------------------------------------------------
// State and counters
// ---------------------------------------------------------------------------

enum class PluginState {
    Loading,
    DisabledByConfig,
    UnsupportedBuild,
    ServicesMissing,
    InstallFailed,
    ButtonFailed,
    Active,
};

auto StateName(PluginState state) noexcept -> const char* {
    switch (state) {
    case PluginState::Loading:          return "loading";
    case PluginState::DisabledByConfig: return "disabled in the config";
    case PluginState::UnsupportedBuild: return "not installed, this D2R build does not match";
    case PluginState::ServicesMissing:  return "not installed, D2RLoader lacks a needed service";
    case PluginState::InstallFailed:    return "not installed, the inventory panel hook failed";
    case PluginState::ButtonFailed:     return "the button could not be added";
    case PluginState::Active:           return "active";
    }
    return "unknown";
}

PluginState       State = PluginState::Loading;
std::string       VerifyFailures;
std::atomic<bool> Active{};
bool              CellFieldsValid{};

std::atomic<std::uint64_t> PanelUpdates{};     // inventory updates with the button in the panel
std::atomic<std::uint64_t> ButtonMoves{};
std::atomic<std::uint64_t> Presses{};          // button clicks
std::atomic<std::uint64_t> HotkeyPresses{};    // Controls key presses with the stash open
std::atomic<std::uint64_t> QueueFailures{};
std::atomic<std::uint64_t> ItemsSent{};
std::atomic<bool>          TransferQueued{};

auto Count(const std::atomic<std::uint64_t>& counter) noexcept -> unsigned long long {
    return static_cast<unsigned long long>(counter.load(std::memory_order_relaxed));
}

// What the last inventory update found, for the console command.
struct PlacementView {
    bool         seen{};
    const char*  layout{};
    bool         stashOpen{};
    bool         shown{};
    bool         placed{};
    bool         haveGrid{};
    bool         haveGold{};
    WidgetRect   grid{};
    std::int32_t gridWidth{};
    WidgetRect   row{};
    WidgetRect   button{};
    const char*  problem{};
};

// What the last press did, for the console command.
struct TransferView {
    bool          ran{};
    const char*   stopped{};
    std::uint32_t carried{};
    std::uint32_t busy{};
    std::uint32_t notStashable{};
    std::uint32_t sent{};
    std::uint32_t refused{};
};

std::mutex    ViewMutex;
PlacementView LastPlacement;
TransferView  LastTransfer;

// One warning per problem per session.
std::atomic<bool> WarnedNotButton{};
std::atomic<bool> WarnedNoGrid{};
std::atomic<bool> WarnedNoGold{};
std::atomic<bool> WarnedFillParent{};

void WarnOnce(std::atomic<bool>& flag, const char* message) {
    if (!flag.exchange(true)) Context->LogWarn(message);
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

constexpr char DefaultConfigToml[] =
    "# Transfer to Stash\n"
    "#\n"
    "# Adds a \"Transfer to Stash\" button to the inventory panel.\n"
    "#\n"
    "# What it does\n"
    "#   - The button sits under the inventory grid, on the right, centred on the\n"
    "#     gold row. It only shows while the stash is open.\n"
    "#       keyboard/mouse  right edge 27 px (at 1080p) inside the grid's right edge\n"
    "#       controller      right edge on the grid frame's outer edge; a bigger\n"
    "#                       button with the next font size up\n"
    "#   - Pressing it does what Ctrl+click does with the stash open, for every item\n"
    "#     in the inventory and in the Horadric Cube at once. The game picks the\n"
    "#     stash tab for each item.\n"
    "#   - The position comes from the inventory layout the game loaded, so a mod's\n"
    "#     own inventory layout needs no numbers here. Where the gold display sits\n"
    "#     is up to the mod's layout; the plugin does not move it.\n"
    "#   - On a controller the d-pad reaches the button from the gold button, once\n"
    "#     the mod's controller inventory layout points gold_button's \"right\"\n"
    "#     navigation at celestialrayone.transfer-to-stash/TransferButtonController;\n"
    "#     A presses it.\n"
    "#   - Keyboard key: Options > Controls > Stash > Transfer to Stash, no default\n"
    "#     key. It does the same as the button, and only while the stash is open.\n"
    "#     Holding it down counts as one press.\n"
    "#   - The button art is inside the DLL. A mod can replace it with its own files\n"
    "#     under data/hd/global/ui/d2rloader/celestialrayone.transfer-to-stash/:\n"
    "#       transferbutton.sprite, transferbutton.lowend.sprite\n"
    "#         4 frames of 368x58 (184x29 for lowend)\n"
    "#       transferbuttoncontroller.sprite, transferbuttoncontroller.lowend.sprite\n"
    "#         4 frames of 404x82 (202x41 for lowend)\n"
    "#     Frame order: normal, pressed, disabled, hovered.\n"
    "#\n"
    "# Which items move\n"
    "#   - Items whose misc.txt row has AdvancedStashStackable = 1 and that have a\n"
    "#     slot in the stash layout (bankexpansionlayouthd.json). That is the same\n"
    "#     rule Ctrl+click uses.\n"
    "#   - Items already on their way to the stash (a transfer the server has not\n"
    "#     answered yet) are skipped.\n"
    "#   - The server still checks every item. A stash slot that is full keeps its\n"
    "#     items where they are.\n"
    "#\n"
    "# Limits\n"
    "#   - Works in single player and in TCP/IP games: the requests are the game's\n"
    "#     own Ctrl+click requests, so the host checks them as usual.\n"
    "#\n"
    "# Console command\n"
    "#   transfertostash   status, where the button was placed, the Controls key,\n"
    "#                     and what the last press sent\n"
    "#\n"
    "[transfer_to_stash]\n"
    "\n"
    "# Master switch. false installs nothing.\n"
    "enabled = true\n";

constexpr std::uint32_t MaximumConfigBytes = 64 * 1024;

struct Settings {
    bool enabled = true;
};
Settings Config;
std::string ConfigProblems;

auto Trim(std::string_view text) noexcept -> std::string_view {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
    return text;
}

auto StripComment(std::string_view line) noexcept -> std::string_view {
    const std::size_t hash = line.find('#');
    return hash == std::string_view::npos ? line : line.substr(0, hash);
}

void ParseConfig(std::string_view text) {
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
        if (equals == std::string_view::npos || section != "transfer_to_stash") continue;
        const std::string_view key   = Trim(line.substr(0, equals));
        const std::string_view value = Trim(line.substr(equals + 1));
        if (key == "enabled") {
            if (value == "true") Config.enabled = true;
            else if (value == "false") Config.enabled = false;
            else ConfigProblems += "enabled: expected true or false. ";
        }
    }
}

void ReadConfiguration() {
    if (!Context->EnsureConfig(DefaultConfigToml)) {
        Context->LogWarn("TransferToStash: the config file could not be created; using defaults.");
        return;
    }
    std::string buffer(MaximumConfigBytes, '\0');
    if (!Context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size()), nullptr)) {
        Context->LogWarn("TransferToStash: the config file could not be read; using defaults.");
        return;
    }
    buffer.resize(std::strlen(buffer.c_str()));
    ParseConfig(buffer);
    if (!ConfigProblems.empty()) D2RL::LogWarnF(Context, "TransferToStash: %s", ConfigProblems.c_str());
}

// ---------------------------------------------------------------------------
// Checks
// ---------------------------------------------------------------------------

auto VerifyAll() -> bool {
    bool ok = true;
    const auto check = [&](const Witness& witness) {
        if (Context->CheckExpectedBytes(witness.rva, witness.bytes, witness.size)) return;
        ok = false;
        char line[160];
        std::snprintf(line, sizeof(line), "%s (%llX); ", witness.name, static_cast<unsigned long long>(witness.rva));
        VerifyFailures += line;
    };
    for (const Witness& witness : Witnesses) check(witness);
    if (!ok) D2RL::LogErrorF(Context, "TransferToStash: byte check failed: %s", VerifyFailures.c_str());
    return ok;
}

// The grid's cell fields, read from the field tables the game loads layouts
// with: the entry's name pointer and its field offset must be the ones above.
auto CheckField(std::uint64_t fieldRva, std::uint64_t nameRva, const char* name, std::uint64_t offset) noexcept -> bool {
    const auto* entry = reinterpret_cast<const std::uint8_t*>(Base + fieldRva);
    std::uintptr_t namePointer{};
    std::uint64_t  fieldOffset{};
    std::memcpy(&namePointer, entry + FieldNameOffset, sizeof(namePointer));
    std::memcpy(&fieldOffset, entry + FieldOffsetOffset, sizeof(fieldOffset));
    return namePointer == Base + nameRva
        && std::memcmp(reinterpret_cast<const char*>(Base + nameRva), name, std::strlen(name) + 1) == 0
        && fieldOffset == offset;
}

// ---------------------------------------------------------------------------
// Widgets
// ---------------------------------------------------------------------------

template <typename T>
auto Read(const void* base, std::size_t offset) noexcept -> T {
    T value{};
    std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(T));
    return value;
}

auto VirtualAt(void* widget, std::size_t slot) noexcept -> void* {
    void** vtable = Read<void**>(widget, 0);
    return vtable != nullptr ? vtable[slot] : nullptr;
}

// The game's own type test: the widget's type, then its parent types.
auto IsA(void* widget, void* type) noexcept -> bool {
    if (widget == nullptr || type == nullptr) return false;
    auto getType = reinterpret_cast<WidgetTypeFn>(VirtualAt(widget, VtableType));
    if (getType == nullptr) return false;
    void* current = getType(widget);
    for (std::size_t depth = 0; current != nullptr && depth < MaxTypeDepth; ++depth) {
        if (current == type) return true;
        current = Read<void*>(current, TypeParentOffset);
    }
    return false;
}

auto RectOf(void* widget) noexcept -> WidgetRect {
    WidgetRect rect{};
    GetWidgetRect(widget, &rect);
    return rect;
}

void MoveTo(void* widget, std::int32_t x, std::int32_t y) {
    if (Read<std::uint8_t>(widget, WidgetFillParentOffset) != 0) {
        WarnOnce(WarnedFillParent, "TransferToStash: the button fills its parent and cannot be moved.");
        return;
    }
    const std::int32_t point[2]{ x, y };
    SetWidgetPosition(widget, point);
}

void SetShown(void* widget, bool shown) noexcept {
    const std::uint8_t wanted = shown ? 1 : 0;
    if (Read<std::uint8_t>(widget, WidgetVisibleOffset) != wanted) {
        reinterpret_cast<WidgetBoolFn>(VirtualAt(widget, VtableSetVisible))(widget, shown);
    }
    if (Read<std::uint8_t>(widget, WidgetEnabledOffset) != wanted) {
        reinterpret_cast<WidgetBoolFn>(VirtualAt(widget, VtableSetEnabled))(widget, shown);
    }
}

// The inventory grid: the direct child named "grid", or else the first direct
// child that is an InventoryGridWidget.
auto FindGrid(void* panel) noexcept -> void* {
    void* const gridType = GridType();
    void* grid = FindChild(panel, GridName);
    if (grid != nullptr && IsA(grid, gridType)) return grid;
    void** const children = Read<void**>(panel, WidgetChildrenOffset);
    const auto count = Read<std::uint64_t>(panel, WidgetChildCountOffset);
    if (children == nullptr || count > MaxChildren) return nullptr;
    for (std::uint64_t i = 0; i < count; ++i) {
        if (children[i] != nullptr && IsA(children[i], gridType)) return children[i];
    }
    return nullptr;
}

auto GridWidth(void* grid, const WidgetRect& rect) noexcept -> std::int32_t {
    if (rect.width > 0) return rect.width;
    if (!CellFieldsValid) return 0;
    const auto columns = Read<std::int32_t>(grid, CellCountOffset);
    const auto cell    = Read<std::int32_t>(grid, CellSizeOffset);
    if (columns <= 0 || columns > 64 || cell <= 0 || cell > 1024) return 0;
    return columns * cell;
}

// ---------------------------------------------------------------------------
// Hook: the inventory panel update
// ---------------------------------------------------------------------------

void Arrange(void* panel) {
    const ButtonSpec* spec = nullptr;
    void* button = nullptr;
    for (const ButtonSpec& candidate : Buttons) {
        button = FindChild(panel, candidate.widgetName);
        if (button != nullptr) {
            spec = &candidate;
            break;
        }
    }
    if (button == nullptr) return;   // neither button is loaded in this panel
    if (!IsA(button, ButtonType()) || Read<void*>(button, WidgetParentOffset) != panel) {
        WarnOnce(WarnedNotButton, "TransferToStash: the widget named after the button is not the plugin's button; "
                                  "nothing is placed.");
        return;
    }
    PanelUpdates.fetch_add(1, std::memory_order_relaxed);

    PlacementView view{};
    view.seen      = true;
    view.layout    = spec->label;
    view.stashOpen = UiStateOpen(StashUiState) != 0;
    view.button    = RectOf(button);

    void* const grid = FindGrid(panel);
    if (grid != nullptr) {
        view.grid      = RectOf(grid);
        view.gridWidth = GridWidth(grid, view.grid);
        view.haveGrid  = view.gridWidth > 0;
    }
    void* const coin   = FindChild(panel, GoldButtonName);
    void* const amount = FindChild(panel, GoldAmountName);
    view.haveGold = coin != nullptr || amount != nullptr;
    if (coin != nullptr) view.row = RectOf(coin);
    if (view.row.height <= 0 && amount != nullptr) view.row = RectOf(amount);

    if (!view.haveGrid) {
        view.problem = "no inventory grid with a known width";
        WarnOnce(WarnedNoGrid, "TransferToStash: the inventory panel has no grid with a known width; the button "
                               "stays hidden.");
    } else if (!view.haveGold) {
        view.problem = "no gold_button or gold_amount in the inventory panel";
        WarnOnce(WarnedNoGold, "TransferToStash: the inventory panel has no gold_button or gold_amount; the "
                               "button has no row to sit on and stays hidden.");
    } else if (view.button.width <= 0 || view.button.height <= 0) {
        view.problem = "the button has no size yet (its sprite is not loaded)";
    } else if (view.row.height <= 0) {
        view.problem = "the gold row has no size yet";
    } else {
        // Right edge rightInset inside the grid's right edge (outside when
        // negative), centred on the gold row.
        const std::int32_t x = view.grid.x + view.gridWidth - spec->rightInset - view.button.width;
        const std::int32_t y = view.row.y + view.row.height / 2 - view.button.height / 2;
        if (view.button.x != x || view.button.y != y) {
            MoveTo(button, x, y);
            view.button.x = x;
            view.button.y = y;
            ButtonMoves.fetch_add(1, std::memory_order_relaxed);
        }
        view.placed = true;
    }

    view.shown = view.stashOpen && view.placed;
    SetShown(button, view.shown);

    std::lock_guard lock(ViewMutex);
    LastPlacement = view;
}

std::uint64_t __fastcall HookedInventoryUpdate(void* panel) {
    if (Active.load(std::memory_order_relaxed) && panel != nullptr) Arrange(panel);
    return OriginalInventoryUpdate(panel);
}

// ---------------------------------------------------------------------------
// The transfer
// ---------------------------------------------------------------------------

void FinishTransfer(TransferView& view, const char* stopped) {
    view.ran     = true;
    view.stopped = stopped;
    ItemsSent.fetch_add(view.sent, std::memory_order_relaxed);
    {
        std::lock_guard lock(ViewMutex);
        LastTransfer = view;
    }
    if (stopped != nullptr) {
        D2RL::LogInfoF(Context, "TransferToStash: nothing sent, %s.", stopped);
        return;
    }
    D2RL::LogInfoF(Context, "TransferToStash: %u sent, %u refused by the client check; %u items in the inventory and "
        "cube, %u busy, %u not for the Advanced Stash.", view.sent, view.refused, view.carried, view.busy,
        view.notStashable);
}

// Section 1, for every item on page 0 or 3. Runs on the UI thread.
void __cdecl RunTransfer(const D2RL::PluginContext*, void*) noexcept {
    TransferQueued.store(false);
    if (!Active.load(std::memory_order_relaxed)) return;

    TransferView view{};
    if (UiStateOpen(StashUiState) == 0) return FinishTransfer(view, "the stash is closed");
    void* const player = LocalPlayer(LocalDataContext());
    if (player == nullptr) return FinishTransfer(view, "there is no local player");
    void* const inventory = UnitInventory(player, SourceFile, __LINE__);
    if (inventory == nullptr) return FinishTransfer(view, "the player has no inventory");
    void* const destination = StashDestination(player);
    if (destination == nullptr) return FinishTransfer(view, "the Advanced Stash is not loaded");

    std::array<void*, MaxBatch> batch{};
    std::size_t batchSize = 0;
    std::size_t walked    = 0;
    for (void* item = FirstItem(inventory); item != nullptr; item = NextItem(item)) {
        if (++walked > MaxItemsWalked) return FinishTransfer(view, "the item list did not end");
        if (UnitType(item) != ItemUnitType) continue;
        const std::uint8_t page = ItemPage(item);
        if (page != InventoryPage && page != CubePage) continue;
        ++view.carried;
        if (InteractionBlocked(item) != 0) {
            ++view.busy;
            continue;
        }
        if (!CanQuickMove(item)) {
            ++view.notStashable;
            continue;
        }
        if (batchSize < batch.size()) batch[batchSize++] = item;
    }

    for (std::size_t i = 0; i < batchSize; ++i) {
        void* const item = batch[i];
        TransferPlacement placement{};
        if (Transfer(item, destination, AdvancedStashPage, ItemPage(item), QuickMoveMode, &placement) != 0) {
            ++view.sent;
        } else {
            ++view.refused;
        }
    }
    if (batchSize != 0) PlaySound(QuickMoveSound, nullptr, 0, 0, 0);
    FinishTransfer(view, nullptr);
}

// ---------------------------------------------------------------------------
// Services: the button layout, its click and the UI thread
// ---------------------------------------------------------------------------

const D2RL::ResourceService*    ResourceSvc{};
const D2RL::PanelService*       PanelSvc{};
const D2RL::SharedEventService* EventSvc{};
const D2RL::ThreadService*      ThreadSvc{};
const D2RL::InputService*       InputSvc{};

std::atomic<D2RL::Input::ActionHandle> HotkeyAction{ D2RL::Input::InvalidHandle };
std::atomic<bool>                      HotkeyHeld{};

struct ButtonHandles {
    D2RL::Resources::RegistrationHandle layout = D2RL::Resources::InvalidHandle;
    D2RL::Resources::RegistrationHandle sprite = D2RL::Resources::InvalidHandle;
    D2RL::Resources::RegistrationHandle lowend = D2RL::Resources::InvalidHandle;
    D2RL::Panels::ChildLayoutHandle     child  = D2RL::Panels::InvalidChildLayoutHandle;
};
std::array<ButtonHandles, 2>       Handles{};
D2RL::SharedEvents::ListenerHandle ClickListener = D2RL::SharedEvents::InvalidHandle;

auto QueryServices() -> bool {
    bool ok = true;
    if (Context->QueryService(&ResourceSvc) != D2RL::ServiceQueryResult::Success
            || !D2RL::HasResourceServiceField(ResourceSvc, D2RL::ResourceServiceRequiredSize)) {
        Context->LogError("TransferToStash: D2RLoader has no resource service.");
        ok = false;
    }
    if (Context->QueryService(&PanelSvc) != D2RL::ServiceQueryResult::Success
            || !D2RL::HasPanelServiceField(PanelSvc, D2RL::PanelServiceUnregisterChildLayoutFieldEnd)) {
        Context->LogError("TransferToStash: D2RLoader has no panel service with child layouts.");
        ok = false;
    }
    if (Context->QueryService(&EventSvc) != D2RL::ServiceQueryResult::Success
            || !D2RL::HasSharedEventServiceField(EventSvc, D2RL::SharedEventServiceRequiredSize)) {
        Context->LogError("TransferToStash: D2RLoader has no shared event service.");
        ok = false;
    }
    if (Context->QueryService(&ThreadSvc) != D2RL::ServiceQueryResult::Success
            || !D2RL::HasThreadServiceField(ThreadSvc, D2RL::ThreadServiceRequiredSize)) {
        Context->LogError("TransferToStash: D2RLoader has no thread service.");
        ok = false;
    }
    return ok;
}

// Both the button and the Controls key end here. A press while a transfer is
// already queued joins that one.
auto QueueTransfer() noexcept -> bool {
    if (!Active.load(std::memory_order_relaxed)) return false;
    if (TransferQueued.exchange(true)) return true;
    if (ThreadSvc->runOnUiThread(Context, &RunTransfer, nullptr) != D2RL::Threads::Result::Success) {
        TransferQueued.store(false);
        QueueFailures.fetch_add(1, std::memory_order_relaxed);
        Context->LogWarn("TransferToStash: the transfer could not be queued on the UI thread.");
        return false;
    }
    return true;
}

auto __cdecl OnUiMessage(const D2RL::PluginContext*, const D2RL::SharedEvents::UiMessageEvent* event, void*) noexcept
        -> D2RL::SharedEvents::UiMessageAction {
    if (event == nullptr || event->structSize < D2RL::SharedEvents::UiMessageEventRequiredSize
            || event->target == nullptr || event->command == nullptr || event->text == nullptr
            || ClickTarget != event->target || ClickCommand != event->command || ClickText != event->text) {
        return D2RL::SharedEvents::UiMessageAction::Continue;
    }
    // The message is private to this plugin, so it never goes on to PanelManager.
    Presses.fetch_add(1, std::memory_order_relaxed);
    (void)QueueTransfer();
    return D2RL::SharedEvents::UiMessageAction::Consume;
}

// The Controls key: the same transfer as the button, only while the stash is
// open. Key repeats while it is held are not presses.
auto __cdecl OnHotkey(const D2RL::PluginContext*, const D2RL::Input::ActionEvent* event, void*) noexcept
        -> D2RL::Input::ActionResult {
    if (!D2RL::Input::HasActionEventField(event, D2RL::Input::ActionEventRequiredSize)) {
        return D2RL::Input::ActionResult::Ignored;
    }
    if (event->kind == D2RL::Input::ActionEventKind::Released) {
        HotkeyHeld.store(false);
        return D2RL::Input::ActionResult::Ignored;
    }
    if (event->kind != D2RL::Input::ActionEventKind::Pressed || HotkeyHeld.exchange(true)) {
        return D2RL::Input::ActionResult::Ignored;
    }
    if (!Active.load(std::memory_order_relaxed) || UiStateOpen(StashUiState) == 0) {
        return D2RL::Input::ActionResult::Ignored;
    }
    HotkeyPresses.fetch_add(1, std::memory_order_relaxed);
    return QueueTransfer() ? D2RL::Input::ActionResult::Handled : D2RL::Input::ActionResult::Ignored;
}

auto RegisterHotkey() -> bool {
    if (Context->QueryService(&InputSvc) != D2RL::ServiceQueryResult::Success
            || !D2RL::HasInputServiceField(InputSvc, D2RL::InputServiceRequiredSize)) {
        InputSvc = nullptr;
        Context->LogWarn("TransferToStash: D2RLoader has no input service; there is no Controls key, the button "
                         "still works.");
        return false;
    }
    const D2RL::Input::ActionRegistration registration{
        .structSize       = D2RL::Input::ActionRegistrationSize,
        .flags            = 0,
        .logicalId        = "transfer-to-stash",
        .displayName      = "Transfer to Stash",
        .category         = "Stash",
        .defaultPrimary   = { .key = D2RL::Input::Key::None, .modifier = D2RL::Input::Modifier::None },
        .defaultSecondary = { .key = D2RL::Input::Key::None, .modifier = D2RL::Input::Modifier::None },
        .callback         = &OnHotkey,
        .userData         = nullptr,
    };
    D2RL::Input::ActionHandle handle = D2RL::Input::InvalidHandle;
    const D2RL::Input::Result result = InputSvc->registerAction(Context, &registration, &handle);
    if (result != D2RL::Input::Result::Success || handle == D2RL::Input::InvalidHandle) {
        D2RL::LogErrorF(Context, "TransferToStash: the Controls entry was refused (result %u); the button still "
            "works.", static_cast<unsigned>(result));
        return false;
    }
    HotkeyAction.store(handle);
    return true;
}

void RemoveButton() {
    if (ClickListener != D2RL::SharedEvents::InvalidHandle && EventSvc != nullptr) {
        (void)EventSvc->unregisterUiMessageListener(Context, ClickListener);
    }
    ClickListener = D2RL::SharedEvents::InvalidHandle;
    for (ButtonHandles& handles : Handles) {
        if (handles.child != D2RL::Panels::InvalidChildLayoutHandle && PanelSvc != nullptr) {
            (void)PanelSvc->unregisterChildLayout(Context, handles.child);
        }
        handles.child = D2RL::Panels::InvalidChildLayoutHandle;
        for (auto* handle : { &handles.layout, &handles.sprite, &handles.lowend }) {
            if (*handle != D2RL::Resources::InvalidHandle && ResourceSvc != nullptr) {
                (void)ResourceSvc->unregisterResource(Context, *handle);
            }
            *handle = D2RL::Resources::InvalidHandle;
        }
    }
}

// An RCDATA resource of this DLL (plugin.rc), checked to be a sprite.
auto EmbeddedSprite(int id, const void*& bytes, std::uint64_t& size) -> bool {
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&EmbeddedSprite), &module)) {
        return false;
    }
    HRSRC found = FindResourceW(module, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));   // RT_RCDATA
    HGLOBAL loaded = found != nullptr ? LoadResource(module, found) : nullptr;
    bytes = loaded != nullptr ? LockResource(loaded) : nullptr;
    size  = found != nullptr ? SizeofResource(module, found) : 0;
    return bytes != nullptr && size > 40 && std::memcmp(bytes, "SpA1", 4) == 0;
}

auto RegisterSprite(int id, const char* path, D2RL::Resources::RegistrationHandle& handle) -> bool {
    const void*   bytes{};
    std::uint64_t size{};
    if (!EmbeddedSprite(id, bytes, size)) {
        D2RL::LogErrorF(Context, "TransferToStash: the embedded sprite %d is missing from the DLL.", id);
        return false;
    }
    const D2RL::Resources::ResourceRegistration sprite{
        .structSize = D2RL::Resources::ResourceRegistrationSize,
        .flags      = 0,
        .path       = path,
        .bytes      = bytes,
        .byteCount  = size,
    };
    const auto result = ResourceSvc->registerResource(Context, &sprite, &handle);
    if (result != D2RL::Resources::Result::Success) {
        D2RL::LogErrorF(Context, "TransferToStash: %s could not be registered (result %u).", path,
            static_cast<unsigned>(result));
        return false;
    }
    return true;
}

auto AddButton() -> bool {
    for (std::size_t i = 0; i < Buttons.size(); ++i) {
        const ButtonSpec& spec    = Buttons[i];
        ButtonHandles&    handles = Handles[i];
        if (!RegisterSprite(spec.spriteId, spec.spritePath, handles.sprite)
                || !RegisterSprite(spec.lowendSpriteId, spec.lowendSpritePath, handles.lowend)) {
            RemoveButton();
            return false;
        }
        const D2RL::Resources::ResourceRegistration layout{
            .structSize = D2RL::Resources::ResourceRegistrationSize,
            .flags      = 0,
            .path       = spec.layoutPath,
            .bytes      = spec.layout,
            .byteCount  = spec.layoutSize,
        };
        const auto layoutResult = ResourceSvc->registerResource(Context, &layout, &handles.layout);
        if (layoutResult != D2RL::Resources::Result::Success) {
            D2RL::LogErrorF(Context, "TransferToStash: the %s button layout could not be registered (result %u).",
                spec.label, static_cast<unsigned>(layoutResult));
            RemoveButton();
            return false;
        }
    }

    const D2RL::SharedEvents::UiMessageListener listener{
        .structSize = D2RL::SharedEvents::UiMessageListenerSize,
        .flags      = 0,
        .priority   = 10000,
        .reserved   = 0,
        .callback   = &OnUiMessage,
        .userData   = nullptr,
    };
    const auto listenerResult = EventSvc->registerUiMessageListener(Context, &listener, &ClickListener);
    if (listenerResult != D2RL::SharedEvents::Result::Success) {
        D2RL::LogErrorF(Context, "TransferToStash: the button click listener could not be registered (result %u).",
            static_cast<unsigned>(listenerResult));
        RemoveButton();
        return false;
    }

    for (std::size_t i = 0; i < Buttons.size(); ++i) {
        const ButtonSpec& spec = Buttons[i];
        const D2RL::Panels::ChildLayoutRegistration child{
            .structSize = D2RL::Panels::ChildLayoutRegistrationSize,
            .flags      = spec.flags,
            .stockPanel = D2RL::Panels::StockPanel::PlayerInventory,
            .reserved   = 0,
            .localId    = spec.localId,
        };
        const auto childResult = PanelSvc->registerChildLayout(Context, &child, &Handles[i].child);
        if (childResult != D2RL::Panels::Result::Success) {
            D2RL::LogErrorF(Context, "TransferToStash: the %s button could not be added to the inventory panel "
                "(result %u).", spec.label, static_cast<unsigned>(childResult));
            RemoveButton();
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Console command
// ---------------------------------------------------------------------------

auto __cdecl StatusCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept
        -> D2RL::ConsoleCommandResult {
    if (command == nullptr || command->plugin == nullptr) return D2RL::ConsoleCommandResult::Failed;
    char line[400];
    const auto say = [&](const char* text) { command->plugin->WriteConsoleMessage(text); };

    std::snprintf(line, sizeof(line), "Transfer to Stash %s: %s.", PluginVersion, StateName(State));
    say(line);
    if (!VerifyFailures.empty()) {
        std::snprintf(line, sizeof(line), "  byte check failed: %s", VerifyFailures.c_str());
        say(line);
    }

    PlacementView placement;
    TransferView  transfer;
    {
        std::lock_guard lock(ViewMutex);
        placement = LastPlacement;
        transfer  = LastTransfer;
    }

    if (!placement.seen) {
        say("  inventory panel: the button has not been seen yet (open the inventory with keyboard and mouse).");
    } else {
        std::snprintf(line, sizeof(line), "  inventory panel: button seen in %llu updates; grid at %d,%d, %d wide; "
            "gold row at y %d, %d high.", Count(PanelUpdates), placement.grid.x, placement.grid.y,
            placement.gridWidth, placement.row.y, placement.row.height);
        say(line);
        std::snprintf(line, sizeof(line), "  %s button: at %d,%d, %dx%d, %s (stash %s), moved %llu times.",
            placement.layout != nullptr ? placement.layout : "?", placement.button.x, placement.button.y,
            placement.button.width, placement.button.height,
            placement.shown ? "shown" : "hidden", placement.stashOpen ? "open" : "closed", Count(ButtonMoves));
        say(line);
        if (placement.problem != nullptr) {
            std::snprintf(line, sizeof(line), "  placement: %s.", placement.problem);
            say(line);
        }
    }

    if (!transfer.ran) {
        say("  last press: none yet.");
    } else if (transfer.stopped != nullptr) {
        std::snprintf(line, sizeof(line), "  last press: nothing sent, %s.", transfer.stopped);
        say(line);
    } else {
        std::snprintf(line, sizeof(line), "  last press: %u sent, %u refused by the client check; %u items in the "
            "inventory and cube, %u busy, %u not for the Advanced Stash.", transfer.sent, transfer.refused,
            transfer.carried, transfer.busy, transfer.notStashable);
        say(line);
    }
    std::snprintf(line, sizeof(line), "  Controls key (Stash > Transfer to Stash): %s, %llu presses with the stash "
        "open.", HotkeyAction.load() != D2RL::Input::InvalidHandle ? "registered" : "not registered",
        Count(HotkeyPresses));
    say(line);
    std::snprintf(line, sizeof(line), "  totals: %llu button presses, %llu items sent, %llu presses not queued.",
        Count(Presses), Count(ItemsSent), Count(QueueFailures));
    say(line);
    return D2RL::ConsoleCommandResult::Handled;
}

constexpr D2RL::PluginInfo PluginInfoData{
    .infoSize    = D2RL::PluginInfoSize,
    .abiVersion  = D2RL_PLUGIN_ABI_VERSION,
    .id          = PluginIdText,
    .name        = "Transfer to Stash",
    .version     = PluginVersion,
    .author      = "CelestialRayOne",
    .description = "A Transfer to Stash button on the inventory panel, shown while the stash is open, that sends "
                   "every Advanced Stash item in the inventory and the cube to the stash.",
    .flags       = D2RL::PluginFlags::Client | D2RL::PluginFlags::NativeHooks,
};

} // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
    return &PluginInfoData;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
    Context = context;
    if (Context == nullptr || Context->exeBase == 0) return false;
    Base = Context->exeBase;

    if (!Context->RegisterConsoleCommand("transfertostash", &StatusCommand,
            "Transfer to Stash status: where the button was placed, and what the last press sent.")) {
        Context->LogWarn("TransferToStash: the console command could not be registered.");
    }

    ReadConfiguration();
    if (!Config.enabled) {
        State = PluginState::DisabledByConfig;
        Context->LogInfo("TransferToStash: disabled by config.");
        return true;
    }
    if (!VerifyAll()) {
        State = PluginState::UnsupportedBuild;
        Context->LogError("TransferToStash: this D2R build does not match; nothing was installed.");
        return true;
    }
    CellFieldsValid = CheckField(CellCountFieldRva, CellCountNameRva, "cellCount", CellCountOffset)
                   && CheckField(CellSizeFieldRva, CellSizeNameRva, "cellSize", CellSizeOffset);
    if (!CellFieldsValid) {
        Context->LogWarn("TransferToStash: the grid cell fields do not match; only the grid's own width is used.");
    }
    BindNatives();
    if (!QueryServices()) {
        State = PluginState::ServicesMissing;
        return true;
    }

    // The hook goes in first: a button without it would sit, always shown, in
    // the panel's corner.
    if (!Context->InstallInlineHook(InventoryUpdateRva, InventoryUpdateBytes, sizeof(InventoryUpdateBytes),
            &HookedInventoryUpdate, &OriginalInventoryUpdate)) {
        State = PluginState::InstallFailed;
        Context->LogError("TransferToStash: the inventory panel hook could not be installed; nothing was installed.");
        return true;
    }
    Active.store(true);
    if (!AddButton()) {
        State = PluginState::ButtonFailed;
        return true;
    }
    const bool hotkey = RegisterHotkey();

    State = PluginState::Active;
    D2RL::LogInfoF(Context, "TransferToStash %s: active (buttons %s and %s, Controls key %s, grid cell fields %s).",
        PluginVersion, Buttons[0].widgetName, Buttons[1].widgetName, hotkey ? "Stash > Transfer to Stash" : "none",
        CellFieldsValid ? "checked" : "not used");
    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    Active.store(false);
    if (Context == nullptr) return;
    const D2RL::Input::ActionHandle action = HotkeyAction.exchange(D2RL::Input::InvalidHandle);
    if (action != D2RL::Input::InvalidHandle && InputSvc != nullptr) {
        (void)InputSvc->unregisterAction(Context, action);
    }
    RemoveButton();
}
