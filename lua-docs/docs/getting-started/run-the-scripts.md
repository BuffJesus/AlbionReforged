# Run the scripts

The native runtime boots the game's own scripts and drives its real managers. This
page shows the two entry points: the front-end app flag, and the programmatic API.

## From the app

Both front-ends (D3D12 and Vulkan) take opt-in scripting flags:

```bash
f2native_frontend.exe --scripting --game-scripts \
  --game-dir "<path to extracted Fable II>" \
  --ui-root  "<path to title UI assets>"
```

- `--scripting` boots the embedded Lua VM and registers the native API.
- `--game-scripts` loads the game's own bank (`generalsetupscript` → 159
  miscellaneous scripts + the managers) and wires the manager tick.

The app stays interactive; the scripting layer runs alongside it.

## From code

The runtime is a small, embeddable C++ surface (`NativeGame` + `NativeScriptVM`).
The boot + quest-run path, end to end:

```cpp
f2::NativeGame game;
game.enable_scripting();                 // create the Lua 5.1 VM + core natives
game.boot_game_scripts(data_root);       // generalsetupscript + real managers
game.load_quest_scripts();               // QuestManager + 27 quests + gameflow

auto& vm = *game.script_vm;

// Load the shipped example quest; it self-registers MyFirstQuest.
vm.run_source("RunScript('quests/myfirstquest.lua')");

// Instantiate + schedule through the REAL QuestManager.
vm.run_source("__q = MyFirstQuest:new(); QuestManager.AddQuestThread(__q)");

vm.run_source("QuestManager.Update()");        // Update runs → yields at WaitFor
vm.run_source("MyFirstQuest.QuestOver = true"); // release the wait
vm.run_source("QuestManager.Update()");        // WaitFor exits → prints + ends
// game.script_log now contains "Terminating quest now"
```

Each `InWorld` tick, the runtime resumes the three managers in the retail order —
**Quest → General → AI** — by calling the `Update` callbacks the game's scripts
registered via `SetQuestUpdateFunction` / `SetGeneralScriptManager` /
`SetAIManager`.

## What you get, and what you don't

!!! success "Runs"
    - The full boot chain (managers, enums, interactables, helpers).
    - Real quest coroutines: `Update`, `WaitFor`, message polling, completion.
    - `MyFirstQuest` to its own completion print.

!!! warning "Stubbed (flagged)"
    - Runtime-spawned entities have no GDB archetype (no mesh / AI / animation).
    - `SearchTools` honours the **name** filter only (no area, no script filter).
    - Save / permanents registration.

    See the [status page](../status.md) for the full list.

## Observing script output

The runtime overrides Lua's global `print` to capture into
`game.script_log` — so a quest's own `print(...)` is observable without touching
stdout. The **missing-native worklist** (`vm.stub_misses()`) is the ranked list of
natives the loaded scripts called that the runtime doesn't implement yet — the
to-do list for deeper coverage.

## The stub census — the measurement harness

Before theorising about a quest, **measure it**. `test_childhood_stub_census` boots
the scripts, runs a real quest for a horizon of frames, and writes
`docs/childhood_stub_census.txt` containing:

- the **call ranking** of stubbed natives and a **first-call timeline**;
- the **spin set** (natives hammered every frame — usually a wait that never exits);
- **silent coroutine errors**, with traceback and the last stubbed native called
  before the death — the only way to see an error the managers discard;
- **waits entered**, and **entity threads with their match counts**
  (`matched=0` = a branch that never runs);
- **dialogue actually spoken**, **staged actions**, entity positions and distance to
  the hero;
- the game's **own `Debug.Error` calls**.

Knobs:

| Env var | Effect |
|---|---|
| `FABLE2NATIVE_CENSUS_SCENE` | path to a cooked `.f2scene` — measure with the world live |
| `FABLE2NATIVE_CENSUS_NAMES` | extra entity names to track |
| `FABLE2NATIVE_CENSUS_FRAMES` | raise the horizon (default 10 s at 60 Hz) |
| `FABLE2NATIVE_CENSUS_SKIP_CUTSCENES` | diagnostic arm: make `PlayCutscene` return immediately |

Every quest bug on this track was found with this harness; static reading of the
decompiled loops sent us down the wrong branch twice.
