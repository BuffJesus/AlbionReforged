# RE Queue

Updated: 2026-07-15

This is the current practical queue for unattended or between-test reverse engineering. It favors
work that directly improves modding leverage.

## 1. Finish Ancestor's Chest Validation

Goal: verify the current `SpecialChest` gate hook in-game.

What is known:

- `SpecialChest.CustomUpdate` is the real chest logic.
- The website popup is `GUI_WEBSITE_GOLD` when no content qualifies.
- Online flags must be `NotGiven` before open, not `Given`.
- `PlayerWebsiteUnlocks.IsItemUnlocked` is the website entitlement method.

Next checks:

- Open the Chamber-of-Fate ancestor's chest on a save where it was not already claimed.
- Confirm no website popup.
- Confirm the chest grants the Spartan set, mascot/chicken suit, Hero Doll, rare dye, Lionhead
  tattoos, expression book, and online gold bag.
- If it still only grants gold, wrap `SpecialChest.CustomUpdate` directly and patch gates inside that
  wrapper immediately before calling the original method.

## 2. Make Lua/BNK Tools Production-Grade

Current tools:

- `tools/lua_mod/bnk_repack.py`
- `tools/lua_mod/luadis.py`
- `tools/lua_mod/script_index.py`

Next improvements:

- Add a `verify` command that round-trips a BNK and checks decompressed content of every entry.
- Add `script_index.py refs <symbol>` to show script, function/proto, and nearby disassembly lines.
- Add a safe `apply_mod.py --revert` command instead of documenting manual copy.
- Add `apply_mod.py --check` to confirm the current BNK contains the hook and that the original backup
  exists.
- Preserve script extraction paths under `tools/lua_mod/extracted/` only when explicitly requested.

## 3. Lua API Catalog

Goal: build a searchable catalog of Lua-facing native/game APIs and examples.

High-value APIs already seen:

- `Inventory.AddItemOfType(entity, object_id)`
- `Money.Modify(hero, amount)`
- `Chest.Unlock(entity)`
- `Chest.SnapToClosed(entity)`
- `Chest.IsOpen(entity)`
- `Stats.IsCollectorsEdition(hero)`
- `Stats.AddUnlockedTitle(hero, title_id)`
- `PlayerWebsiteUnlocks.IsItemUnlocked(hero, object_id)`
- `XboxLive.GetYetToCollectUnlockedGold()`
- `XboxLive.ZeroYetToCollectUnlockedGold()`
- `QuestManager.NewEntityThread(name)`
- `GeneralScriptManager.AddScript(script)`
- `GeneralScriptManager.Update()`

Next scripts to mine:

- `generictriggers.lua` - entity thread patterns, chests, doors, triggers.
- `weaponinventory.lua` - item/debug grants and item object IDs.
- `onactionusetakeitems.lua` - chest open / take-items flow.
- `questmanager.lua` - save/load and entity-thread lifecycle.
- `gameflow.lua` and `postscriptsloaded.lua` - global progression/unlock flags.

## 4. Item and Unlock Database

Goal: generate a structured CSV/JSON of object IDs from Lua strings and GDB data.

Start with:

- Ancestor chest contents from `docs/ANCESTORS_CHEST_RE.md`.
- Pub Games / Carbonated prizes from `weaponinventory.lua`.
- Gift dolls and legendary weapons from `weaponinventory.lua`.
- Online flags from `gameflow.lua` / `postscriptsloaded.lua`.

Useful output:

- `tools/lua_mod/catalog/items.csv`
- fields: `object_id,type/source,script,notes`

## 5. Load/Save Lifecycle

Goal: understand persistent script state cleanly enough to support mods without brittle update
wrappers.

Known:

- `GeneralScriptManager.LoadFromSave(saved)` wipes `CurrentlyRunningScripts`.
- Wrapping `LoadFromSave` directly broke save-load.
- Wrapping `GeneralScriptManager.Update` survives and works.

Next:

- Disassemble and annotate `GeneralScriptManager` completely.
- Disassemble `QuestManager.LoadFromSave` callers and identify safe post-load callbacks.
- Find a native or Lua "post scripts loaded / post save loaded" event that can register mods more
  cleanly.

## 6. Asset and Level Placement

Goal: locate the Chamber-of-Fate/Guild Cave level and entity placement for the ancestor chest.

Next:

- Use AssetBrowser / level BNK tools to find the placed `SpecialChest` entity.
- Map entity thread names to level placement records.
- Confirm the exact level name for Guild Cave / Chamber of Fate.
- Document how entity scripts attach to placed objects.

This is the path to precise location-scoped mods instead of global script hooks.

## 7. Engine-Side Mod API

Goal: move from BNK patching toward a first-class mod loader.

Next:

- Read `Fable2Recomp/src/ModSupport.cpp`.
- Confirm what loose-file overlay paths currently support.
- Add an official script injection path that loads `mods/*/scripts/startup/*.lua` after base scripts.
- Keep BNK patching as a compatibility path, but prefer overlay/mod packages for future work.

## 8. Rendering/Runtime Polish

Useful but secondary to modding:

- Strip or gate heavy audio/krnl diagnostic logging once the current tests are done.
- Investigate adult hero black skin: likely GPU skin texture morph/compositing.
- Investigate intermittent GPU TDR around adult-intro rendering.
- Prewarm or async-compile D3D12 pipeline states to reduce traversal hitches.
