# Entities & the object model

Quests manipulate the world through **entity objects** — the hero, NPCs, spawned
creatures. In Lua these look like objects with methods (`hero:GetPosition()`); in
the runtime they're backed by the native entity graph. This page is about how they
cross the boundary.

## Objects across the boundary

The runtime represents every game object (entity, quest, event) as a Lua **table**
carrying:

- a **`__id`** — a lightuserdata encoding a 64-bit id (not a number, because
  `lua_Number` is `float32` and would lose precision), and
- a shared **per-tag methods metatable**, so `obj:Method()` dispatches to the
  registered native for that tag.

```cpp
vm.register_object_method("Entity", "GetPosition", /* … reads arg_handle(1) … */);
vm.push_handle("Entity", uid);   // → a Lua object the scripts can call methods on
```

Objects are **cached per (tag, id)** in the Lua registry, so pushing the same
entity twice returns the **same** Lua table. That gives scripts what they expect:
`entityA == entityB` is true for the same entity, and fields stashed on an entity
persist.

## The hero and entity methods

`GetPlayerHero()` returns the hero entity object; `QuestManager.HeroEntity` is set
to it at gameflow init. The runtime binds the entity methods quests use most:

| Method | Backed by |
|---|---|
| `GetPosition()` / `SetPosition(x,y,z)` | the entity's transform |
| `GetName()` | a per-entity name table |
| `GetID()` / `GetIDFromEntity(e)` | the entity uid |
| `IsAlive()` | health component (alive unless killed) |
| `Kill()` | sets health to zero |

Spawning goes through `Debug.CreateEntityAt(class, name, x, y, z)`, which creates a
native entity with a transform and returns its handle. (The `class` → GDB archetype
resolution is a flagged stand-in until the GDB cook is wired.)

## Entity search — the current gap

Quests find world entities by name via `SearchTools`:

```lua
function QuestThreadBase.GetAllEntitiesWithName(self, name, area)
  local s = SearchTools.StartNewSearch(area)
  SearchTools.FilterWithName(s, name)
  return (SearchTools.GetSearchResults(s))
end
```

Today `SearchTools.GetSearchResults` returns an **empty list** — no named world
entities are streamed yet — so `StartNewEntityThread` finds nothing to spawn. This
is why the entity-thread half of a quest (the QuestGiver / EvilTwin sub-threads in
`MyFirstQuest`) is a flagged stand-in. Wiring real entity search + world streaming
is the [next frontier](../status.md#next-frontier); the quest's own logic already
runs.

!!! note "Where"
    Object bridge: `Fable2Native/src/native_script.cpp` (`push_handle`,
    `arg_handle`, `register_object_method`). Entity + `SearchTools` bindings:
    `Fable2Native/src/native_bindings.cpp`.
