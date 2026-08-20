# The embedded VM

`NativeScriptVM` wraps an embedded Lua 5.1 state behind a small, **Lua-free** C++
surface — callers never include `lua.h`. It boots like the game's own VM
(`lua_newstate` + open the standard library), binds natives with the retail
register-class-method pattern, and adds the machinery quests need: object handles,
persistent callbacks, and coroutine-safe argument access.

## Binding natives

A native is a plain function that reads its args and pushes results through the VM:

```cpp
vm.register_native("Player", "GetPosition", [](NativeScriptVM& v) -> int {
    auto p = /* … */;
    v.push_number(p[0]); v.push_number(p[1]); v.push_number(p[2]);
    return 3;                     // number of values pushed
});
```

- `register_native(class, method, fn)` — binds `Class.Method` (creates the global
  class table if needed), the retail pattern.
- `register_global(name, fn)` — binds a bare global function (`RunScript`,
  `GetPlayerHero`, …).
- `register_enum(name, {…})` — a read-only constant table.

## Object handles

Quests hold **objects** — a hero entity, a quest, an event — and call methods on
them (`hero:GetName()`, `event:GetID()`). The VM represents each as a Lua table
carrying a **lightuserdata id** and a **shared per-tag methods metatable**:

```cpp
vm.register_object_method("Entity", "GetName", /* … */);
vm.push_handle("Entity", uid);      // → a Lua object; obj:GetName() dispatches
std::uint64_t id = vm.arg_handle(1); // read the id back from a self argument
```

Handles are **cached per (tag, id)** in the registry, so the same entity always
yields the **same** Lua object — `==` works, and scripts can stash fields on it.
Ids are lightuserdata (not numbers) precisely because `lua_Number` is `float32` and
couldn't hold a 64-bit id exactly.

## Persistent callbacks

The game's managers hand their `Update` function to the engine and expect to be
called back every frame. The VM captures these into the Lua registry:

```cpp
// SetQuestUpdateFunction(fn) →
g->quest_update_ref = v.ref_arg(1);
// each tick →
vm.call_ref(g->quest_update_ref);
```

`ref_arg` / `ref_arg_field` / `call_ref` are how
[managers](managers.md) get driven.

## The coroutine-state fix (important)

Quests run inside **coroutines**. When a native is called from a resumed coroutine,
Lua passes the **coroutine's** `lua_State`, not the main one. The first
implementation always read arguments from the stored main state — so any native
called from a quest read **empty/zero arguments**. The visible symptom: a quest's
own `print("Terminating quest now")` logged an *empty string*.

The fix routes the argument/push helpers to the **actual calling state** for the
duration of each call:

```cpp
int native_trampoline(lua_State* s) {
    // s is the real calling state — a coroutine thread inside a quest.
    return vm->dispatch_from(s, idx);   // sets cur_() = s for this call
}
// arg_string / arg_number / push_* / arg_handle all use cur_()
```

This affects **every** native invoked from a quest coroutine, which is most of
them. It was the final bug between "the quest coroutine runs" and "the quest
completes and prints."

## Pitfalls

Two port-bug classes have silently dropped work more than once. Check both **first**
whenever a native "does nothing":

!!! danger "Scripts call natives method-style — `self` is argument 1"
    `AIManager:RequestCutsceneOnEntity(entity, id, opts)` puts the *manager table*
    in argument 1 and the entity in argument 2. A binding that reads the entity from
    argument 1 gets a table, the id reads back as `0`, and every call is dropped —
    with **no error**, because nothing threw. When a native looks correct but has no
    effect, print the argument count and types before theorising.

!!! danger "`lua_Number` is `float32` — 32-bit ids do not round-trip"
    The 360's Lua was built with `float32` numbers, so anything above 2²⁴ loses low
    bits: `0x8EB51907` comes back as `0x8EB51900`. Never pass a hash, GUID, or uid
    through Lua as a *number*. Two working answers:

    - **Object ids** cross as **lightuserdata** inside a handle table.
    - **Record ids** cross as **dense tokens** — small sequential integers mapped
      back to GUIDs host-side (`gdb_token_for` / `gdb_guid_for_token`).

A third, less obvious one: a native that returns the *wrong shape* fails just as
quietly. `Entity:GetPosition()` returns **one `CVector3`**, not three numbers,
because the game's own scripts write
`QuestManager.HeroEntity:GetPosition() + CVector3(0, 0, 24)` — that `+` only works
if the left operand is a vector. Return-value *fidelity* matters as much as the
value.

## Capturing output

The VM overrides the global `print` to append into a host-visible log
(`game.script_log`), and records every unimplemented native the scripts call as a
ranked **stub-miss worklist** (`vm.stub_misses()`).

!!! note "Where"
    `Fable2Native/include/f2/native_script.h` and
    `Fable2Native/src/native_script.cpp`.
