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
| `GetPosition()` | the entity's transform, as a **`CVector3`** (one value, not three scalars) |
| `GetPositionXYZ()` | the same transform as three plain numbers |
| `SetPosition(x,y,z)` | move the entity |
| `GetName()` | a per-entity name table |
| `GetID()` / `GetIDFromEntity(e)` | the entity uid |
| `IsAlive()` | health component (alive unless killed) |
| `Kill()` | sets health to zero |

Spawning goes through `Debug.CreateEntityAt(class, name, pos)` — the game's own
calls pass a **`CVector3`** as the third argument
(`Debug.CreateEntityAt("ObjectLimboInventory", "", CVector3(0, 0, 0))` in
`gameflow`), so the binding accepts a vector as well as three scalars. It creates a
native entity with a transform and returns its handle. (The `class` → GDB archetype
resolution is a flagged stand-in — see below.)

## Entity search — now real

Quests find world entities by name via `SearchTools`:

```lua
function QuestThreadBase.GetAllEntitiesWithName(self, name, area)
  local s = SearchTools.StartNewSearch(area)
  SearchTools.FilterWithName(s, name)
  return (SearchTools.GetSearchResults(s))
end
```

This **works**: the name filter resolves against the world's named entities, so a
quest's `StartNewEntityThread` spawns a real entity thread per match. Names come
from the cooked level cast, `Debug.CreateEntityAt`, and the hero.

**Flagged:** only the *name* filter is honoured — `StartNewSearch`'s area and
`FilterWithScriptFilter` are no-ops. A script filter *narrows* a set, so leaving it
a no-op keeps the name-filtered set intact rather than dropping it; an area filter
would only ever remove matches. Both are conservative in the direction that keeps a
quest running.

!!! tip "`matched=0` is a diagnosis, not a crash"
    `StartNewEntityThread` spawns **one thread per match** and silently does nothing
    when there are none. The [stub census](../getting-started/run-the-scripts.md)
    reports the match count per entity thread, so a branch that never runs is
    visible instead of invisible.

## Where the cast comes from

The authored cast of a quest is [GDB data](authored-data.md), not script data.
`tools/cook_quest_markers.py` extracts it into the runtime's named-entity table,
carrying each entity's `gdbGuid` so animation slots and components stay reachable.

!!! warning "Creatures and markers carry position on *different* components"
    A **marker**'s position is on `PhysicsSimpleComponent` (FNV-1
    `0x619F96CF`) — *not* `SimpleTransformComponent`, as older notes claimed.
    A **creature**'s is on `PhysicsSimulationCharacterNavigatorComponent.Position`.
    Reading only the first is why the cast once looked position-less.

**Flagged — the structural gap:** anything *spawned at runtime* (`Debug.CreateEntityAt`,
e.g. `CreatureDogHero`) has **no authored record**, so no mesh, no AI, and no
resolvable animation. Closing it means archetype instantiation from GDB
(`ghidra_out/gdb_instantiation_re.txt`) — the biggest remaining item.

!!! note "Where"
    Object bridge: `Fable2Native/src/native_script.cpp` (`push_handle`,
    `arg_handle`, `register_object_method`). Entity + `SearchTools` bindings:
    `Fable2Native/src/native_bindings.cpp`. Cast cook: `tools/cook_quest_markers.py`.
