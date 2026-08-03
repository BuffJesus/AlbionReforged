# Ancestor's Chest / Hero's Legacy RE Notes

Updated: 2026-07-15

The Chamber-of-Fate "visit the Fable II website" popup is not a network call. It is a local branch
inside the real ancestor's chest script.

## Script Map

- `scripts\quests\generictriggers.lua`
  - Defines `SpecialChest` via `QuestManager.NewEntityThread('SpecialChest')`.
  - `SpecialChest.CustomUpdate` is the real ancestor / otherworldly-content chest logic.
- `scripts\quests\gameflow.lua`
  - Initializes `Gameflow.Online*` flags to `NotGiven`.
- `scripts\miscellaneous\saveload\postscriptsloaded.lua`
  - Re-applies the same `NotGiven` online flags on load and sets `DLCChestNeedsToRecheck`.
- `scripts\miscellaneous\weaponinventory.lua`
  - Defines `Debug.AddAllCarbonatedPrizes(hero)`. This is Pub Games / Carbonated content, not the
    ancestor's chest.

## Real Ancestor's Chest Contents

From `generictriggers.lua`, `SpecialChest.CustomUpdate` adds items to `self.Entity`, the chest:

- `ObjectInventory_L_Longsword_Spartan`
- `ObjectClothingHatSpartanM`
- `ObjectClothingCoatSpartanM`
- `ObjectClothingTrousersSpartanM`
- `ObjectClothingBootsSpartanM`
- `ObjectClothingGlovesSpartanM`
- `ObjectInventoryHeroTitleMasterChief`
- `ObjectClothingHatMascotM`
- `ObjectClothingCoatMascotM`
- `ObjectClothingBootsMascotM`
- `ObjectInventoryBookExpressionFakeout`
- `ObjectInventoryGiftToyDollHero`
- `ObjectInventoryDyeClothingRarePink`
- `ObjectTattooFaceLionhead`
- `ObjectTattooTorsoLionhead`
- `ObjectInventoryGoldBag_Online`

## Gate Logic

The chest starts by clearing internal booleans like `ArmourInChest`, `MascotHatInChest`, and
`GoldInChest`.

The Spartan set is gated by:

```lua
Gameflow.OnlineArmour == "NotGiven"
Stats.IsCollectorsEdition(GetPlayerHero())
```

The mascot/chicken suit, expression book, Hero Doll, dye, and Lionhead tattoos are gated by:

```lua
Gameflow.OnlineX == "NotGiven"
PlayerWebsiteUnlocks.IsItemUnlocked(GetPlayerHero(), item_id)
```

The online gold bag is gated by:

```lua
XboxLive.GetYetToCollectUnlockedGold() > 0
```

If any contents are added, the script calls `Chest.Unlock(self.Entity)`. After the player opens/takes
the contents, it flips the matching `Gameflow.Online*` flags to `Given` and zeroes the pending online
gold.

If no contents qualify, it calls:

```lua
Chest.SnapToClosed(self.Entity)
GUI.DisplayMessageBox("GUI_WEBSITE_GOLD")
```

That is the website popup.

## Important Correction

`Gameflow.Online* = "Given"` means "already claimed", not "owned". For the chest to fill, the flag
must still be `NotGiven`, while the entitlement check must return true. The current hook therefore
keeps those flags at `NotGiven` until the chest itself flips them to `Given`.

`PlayerWebsiteUnlocks` is a table with `IsItemUnlocked`, not a callable function.

## Pub Games / Carbonated Content

`Debug.AddAllCarbonatedPrizes(hero)` in `weaponinventory.lua` grants this separate set:

- `ObjectInventoryCarbonatedChocolates`
- `ObjectInventoryCarbonatedBed`
- `ObjectInventoryCarbonatedPotionStrength`
- `ObjectInventoryCarbonatedTattooFace`
- `ObjectInventoryCarbonatedCutlass`
- `ObjectInventoryCarbonatedPieApple`
- `ObjectInventoryCarbonatedBookExpression`
- `ObjectInventoryCarbonatedTattooBody`
- `ObjectInventoryCarbonatedPotionSkill`
- `ObjectInventoryCarbonatedRing`
- `ObjectInventoryCarbonatedHairstyle`
- `ObjectInventoryCarbonatedBookDog`
- `ObjectInventoryCarbonatedCoat`
- `ObjectInventoryCarbonatedPotionWill`
- `ObjectInventoryCarbonatedPistol`

This is useful for a separate Pub Games unlock mod, but it should not be mixed into the ancestor's
chest unless deliberately adding bonus content.

## Current Mod Strategy

`tools/lua_mod/myconsolehook0.lua` hijacks `AppearanceEnum` from `generalsetupscript`, runs the
original enum, and wraps `GeneralScriptManager.Update`. Each tick it:

- patches `Stats.IsCollectorsEdition` to return true,
- patches `PlayerWebsiteUnlocks.IsItemUnlocked` to return true,
- patches `XboxLive.GetYetToCollectUnlockedGold` to return a positive value once,
- keeps unclaimed online flags at `NotGiven`.

This lets the game's own `SpecialChest.CustomUpdate` populate and unlock the real chest. The loot is
therefore scoped to the ancestor's chest rather than globally granted.
