# Contributing

The Lua runtime is deliberately small and layered, so extending it is mostly a
matter of implementing the next native the game's scripts ask for.

## The loop

1. **Run the scripts** (a boot + quest run) and read the ranked
   `vm.stub_misses()` worklist — the natives the game's own code called but the
   runtime doesn't implement yet.
2. **Decompile the caller** to see exactly how the native is used and what it must
   return (scalar? boolean? an object?). See
   [Read the scripts](getting-started/read-the-scripts.md).
3. **Ground it in the decomp.** Check `ghidra_out/lua_natives5_catalog.tsv` for the
   native's class/address, and the relevant spec (`quest_system.txt`,
   `gameflow_progression.txt`, …) for semantics.
4. **Implement it** in `native_bindings.cpp`, mapping onto the native systems
   (entities, message bus, game state). If it returns an object, use
   `push_handle`; if it's a new object class, `register_object_method`.
5. **Prove it** with an `F2_CHECK` test that drives a real script path.

## Rules of the road

- **Run the game's scripts; don't rewrite them.** Add natives, not Lua logic. The
  quest/gameflow logic is the game's compiled Lua.
- **Ground everything, flag guesses.** If a value or behaviour isn't in the decomp,
  implement a sensible default and mark it a flagged stand-in — don't present it as
  fact. The [status page](status.md) tracks every stand-in.
- **Post messages symbolically.** Producers must post with the enum value from the
  game's own `EMessageEventType` (loaded from Lua), never a guessed constant.
- **Use `F2_CHECK` in tests**, not `assert` — the suite strips `assert` under
  `NDEBUG`.
- **Respect the coroutine state.** Natives run inside quest coroutines; the VM
  already routes args to the calling state, but don't cache a `lua_State` across
  calls.

## High-value next steps

Straight from the [status page](status.md#next-frontier):

- **Entity search + world streaming** so `StartNewEntityThread` spawns real entity
  threads (unlocks the QuestGiver/EvilTwin half of `MyFirstQuest` and most quests).
- **The `GeneralScriptManager` boot-coroutine path.**
- **`QC010_Childhood`** — the real first chapter (needs GUI message boxes, gameflow
  position, the interaction chain).
- **The save/permanents subsystem** so quest state persists.

## Building the docs

```bash
pip install -r lua-docs/requirements-docs.txt
cd lua-docs
mkdocs serve        # live preview at http://127.0.0.1:8000
mkdocs build        # static site → ./site
```

The site publishes to GitHub Pages via the workflow in
`.github/workflows/lua-docs.yml` on pushes to `main` that touch `lua-docs/`.
