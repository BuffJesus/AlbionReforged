# MENU_SYSTEM_RE - pause/menu extender map

2026-07-18 autonomous pass. Scope: current pause-menu "Mod Menu" integration and the route to a real menu extender.

## Current verdict

This document is behavioral evidence for the native frontend. The shipping menu is being rebuilt
in `Fable2Native` with stable command IDs; the GUI VM path below remains useful for oracle captures
and compatibility experiments, not as the native runtime's ownership model.


The GUI-bank hook path works. In `Fable2_491.log` the pause-menu entry emitted:

- `MMDBG_SEL1_MM_Open`
- `ModBridge5`
- `[modbridge] request code=5 (fireEvent)`
- `[modbridge] executed on gameplay L=...`

So the GUI VM can signal the host, and the host can queue the existing gameplay-VM R3/MMB mod menu. This is enough for a first extender: add GUI entries in `guiscripts.bnk`, convert selected entries into `ModBridgeN` events, and let the gameplay VM execute the action.

## Files involved

- `tools/lua_mod/gui_modmenu_hook.lua`
  - Replaces `scripts\gameface\guisetup.lua`.
  - Loads the original GUI setup chain, then hooks the active `g_Menu`.
  - Appends a top-level pause item named `MM_Open` with text `Mod Menu`.
  - Detection uses the parent element's `MenuPopulationScript == "PauseMenu"`.

- `tools/lua_mod/gui_expandablemenuinput_hook.lua`
  - Replaces `scripts\gameface\expandablemenu\expandablemenuinput.lua`.
  - Owns input registration and `SelectMenuItem`.
  - If highlighted item name starts with `MM_`, emits `ModBridge5` and returns without pushing a native GUI button event.

- `tools/lua_mod/apply_gui_mod.py`
  - Applies/reverts/checks the two hook entries in `Fable2Recomp/assets/game/data/guiscripts.bnk`.
  - Now compares full hook bytes, not only markers, so source edits reapply correctly.

- `Fable2Recomp/src/ConsoleInjector.cpp`
  - Hooks native `fireEvent`.
  - Reads event name directly from the GUI Lua stack.
  - Swallows `ModBridgeN`, writes/queues the request, and the gameplay VM consumes it via `modbridge_cmd.lua`.

## Why the earlier pause row desynced

The pause hub is not a normal pure population-table list. Its stock root buttons are fetched from the compiled scene (`pausemainmenu.bgf`) while Lua population rows are appended through `PopulationTable`. That mixed source is why the first folder-style attempt could render text but leave `NumberOfItems`, `SpawnedItems`, highlight rows, and selectable slots out of sync.

The current safer shape is a single leaf row:

```lua
pt[n + 1] = { Name = 'MM_Open', TextTag = 'Mod Menu', IconTexture = icon }
```

and the custom action is handled before the stock `GUI:PushButtonEvent`.

## Extender design

Use the game's GUI VM only as the visible selector layer:

1. Add menu rows with stable names: `MM_Open`, `MM_Warp`, `MM_Items`, etc.
2. In patched `SelectMenuItem`, translate `MM_*` into `ModBridgeN` or `ModBridge:<name>`.
3. Host `fireEvent` captures that event and queues a gameplay request.
4. Gameplay VM runs the real action using the already-proven `Debug.Mod` / mod menu path.

This avoids calling gameplay natives from the GUI VM, which is unsafe/wrong-state.

## Next RE targets

- Replace numeric `ModBridgeN` with a string command payload if the Lua stack has arg2 available for `fireEvent`; otherwise encode command names in the event string (`ModBridge_Open`, `ModBridge_Warp`).
- Find the native scene population point for `pausemainmenu.bgf` so future entries can become first-class scene buttons instead of mixed Lua rows.
- Map `GUI:PushButtonEvent(name, EventType, IDLow, IDHigh)` event types for submenu/open/close/action. That would let a custom page open inside Gameface instead of always delegating to the gameplay `ShowMenuBox`.
- If the Gameface path stays brittle, promote the long-term extender to a native overlay/MCM panel and keep the pause item as a launcher.

## Current runtime state

`guiscripts.bnk` is patched and verified current after removing a duplicate `ModBridge5` emission from `gui_modmenu_hook.lua`; selection dispatch now lives only in `gui_expandablemenuinput_hook.lua`.
# Native-port update

The GUI VM findings remain behavioral evidence. The product frontend is now a native C++23
subsystem in `Fable2Native`: boot video, title, main menu, options, mod manager, and input use
stable command IDs rather than visual-index manipulation. GUI-bank hooks remain oracle/compatibility
adapters.
