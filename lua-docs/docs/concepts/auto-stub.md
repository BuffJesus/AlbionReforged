# The auto-stub

The game's scripts call **thousands** of natives — far more than the runtime
implements today. The **auto-stub** absorbs the unimplemented ones so the scripts
run anyway, while being careful not to break the game's *own* globals. Getting this
balance right was one of the harder problems.

## The tension

Both native class tables (`Player`, `GUI`, `Stats`, …) and the game's own runtime
globals (quest types like `MyFirstQuest`, managers like `QuestManager`) are
Capitalized Lua globals. A naïve auto-stub that fabricates a value for *every*
unknown Capitalized global breaks the game in two ways:

1. **It blocks the game's own definitions.** `NewQuestThread` checks *"is a quest
   named X already registered?"* by reading `_G.X`. If the stub fabricates a value,
   the check sees a phantom and refuses to register the quest.
2. **It corrupts real tables.** If the stub wraps a real game table (like
   `GeneralScriptManager`) that was already loaded, reads of that table's *nil*
   fields turn into stubs — breaking the manager's own list-walking logic.

## The rule

The auto-stub fabricates a stub **only for names the retail binary actually exposes
as a native.** Everything else reads as `nil`, so the game's own globals define and
check cleanly.

"Is this a native?" is answered from the decomp catalog
(`ghidra_out/lua_natives5_catalog.tsv`, ~2800 class + global names), embedded as
`native_lua_catalog.inc` and queried by an `__is_native(name)` predicate:

```lua
setmetatable(_G, { __index = function(t, k)
  if type(k) == "string" and __is_native(k) then
    return make_stub(k)          -- a real native we haven't implemented → stub it
  end
  return nil                     -- the game's own global → let it be defined
end })
```

The install pass likewise wraps a real, already-loaded table **only if its name is
a native** — so `GeneralScriptManager` and quest types stay untouched.

## Stub behaviour

A stub is a **black-hole**: callable, indexable, and chainable, so any call shape a
script uses survives.

| Access | Result |
|---|---|
| `Class.Method()` | the black-hole (a no-op returning the black-hole) |
| `Class.Field.Sub` | the black-hole (indexing never throws) |
| `GetSomething()` (a global fn) | the black-hole |
| `Class.IsFoo()` / `HasFoo()` (a predicate) | **`false`** — to avoid truthiness drift |

Predicate-named natives return real `false` so that `if Class.IsReady() then …`
doesn't wrongly take the true branch on a truthy stub. Every unique miss is logged
once to the **stub-miss worklist** (`vm.stub_misses()`) — the ranked to-do list of
natives worth implementing next.

## Why this matters

With the catalog-aware auto-stub, the boot loads **160 miscellaneous scripts with
0 missing natives** and the quest bank runs — the scripts that *do* need a real
native get one, and the ones that don't fall through harmlessly, without the
auto-stub ever standing on the game's own toes.

!!! note "Where"
    `Fable2Native/src/native_script.cpp` (`install_autostub`, the `__is_native`
    predicate) and `Fable2Native/include/f2/native_lua_catalog.inc` (the generated
    native-name set).
