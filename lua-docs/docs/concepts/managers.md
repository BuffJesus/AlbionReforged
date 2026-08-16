# Script managers

Fable II's scripting is driven by three **managers**, each a plain Lua table with
an `Update` method the engine calls every frame:

| Manager | Script | Drives |
|---|---|---|
| `QuestManager` | `quests/questmanager.lua` | quest threads (chapters, side quests) |
| `GeneralScriptManager` | `miscellaneous/generalscriptmanager.lua` | general scripts (systems, ambient logic) |
| `AIManager` | `miscellaneous/aimanager.lua` | AI behaviour glue |

The key insight: **the managers are pure Lua, and Lua owns the scheduling.** The
native runtime does not implement a scheduler — it just calls each manager's
`Update` once per tick and lets the manager resume its own coroutines.

## Registration

Each manager hands itself to the engine through a native, at the end of its script:

```lua
-- generalscriptmanager.lua
SetGeneralScriptManager(GeneralScriptManager)
-- questmanager.lua
SetQuestUpdateFunction(QuestManager.Update)
-- aimanager.lua
SetAIManager(AIManager)
```

The runtime captures the callback (`ref_arg` / `ref_arg_field`) and calls it back
every `InWorld` tick, in the retail order **Quest → General → AI**.

```cpp
// boot wiring (simplified)
SetQuestUpdateFunction  → g->quest_update_ref   = v.ref_arg(1);
SetGeneralScriptManager → g->general_update_ref = v.ref_arg_field(1, "Update");
SetAIManager            → g->ai_update_ref      = v.ref_arg_field(1, "Update");
// per tick
call_ref(quest_update_ref); call_ref(general_update_ref); call_ref(ai_update_ref);
```

## The coroutine model

`AddScript` / `AddQuestThread` wrap a script object's `Update` in a coroutine and
link it into a running list. Each frame, `Update` walks the list and resumes each
live coroutine, pruning the dead ones:

```lua
-- GeneralScriptManager.AddScript (decompiled, condensed)
function GeneralScriptManager.AddScript(obj)
  obj.co_update = coroutine.create(obj.Update)
  GeneralScriptManager.Insert(obj)
end

-- GeneralScriptManager.Update (condensed)
function GeneralScriptManager.Update()
  local node = GeneralScriptManager.CurrentlyRunningScripts
  while node do
    coroutine.resume(node.value.co_update, node.value)
    if coroutine.status(node.value.co_update) == "dead" then
      -- unlink node
    end
    node = node.next
  end
end
```

Because the schedule lives in Lua, the runtime never needs to understand quest
priorities, suspension, or restart — it only needs the coroutine to be resumable,
which means the natives it calls must behave (see the
[coroutine-state fix](the-vm.md#the-coroutine-state-fix-important)).

## Boot order

The managers don't all load together. Two setup scripts, both driven by
`RunScript`, load them:

```
generalsetupscript.lua   → GeneralScriptManager + 159 miscellaneous scripts (enums,
                           interactables, helpers)      [loaded by boot_game_scripts]
questsetupscript.lua      → QuestManager + LuaEnums + Feats + Gameflow + 27 quest
                           modules                       [loaded by load_quest_scripts]
```

!!! note "`BaseObjects` is `_G`"
    The managers are written as `BaseObjects.QuestManager = {...}` but referenced as
    globals (`QuestManager.NewQuestThread`). That works because `BaseObjects` is
    aliased to the global table (`BaseObjects = _G`) before the quest bank runs — so
    `BaseObjects.QuestManager` *is* the global `QuestManager`.

## Known frontier

The `GeneralScriptManager.Update` path can still throw on a boot script that
schedules a coroutine the general manager can't resume cleanly. It's isolated (the
per-manager tick is guarded) and doesn't block quests, but it's on the
[to-do list](../status.md#next-frontier).
