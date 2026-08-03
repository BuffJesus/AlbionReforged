# Fable II Native Modding API — design (the "SKSE tier", built into our recomp)

The keystone of the native modding layer: expose new C++ functions to the game's Lua VM
(`register_native`), then build the targeted hooks (GetText, asset-load, input, UI...) on top. Because
we own the recomp, this is first-class — not blind offset-scraping. This doc captures the mechanism
(reverse-engineered, not guessed) and the build path.

## Ownership context: recomp → decomp → our own engine
- **ReXGlue recomp = the bootstrap (temporary).** Static PPC→C++ translation that runs the game now.
  Modding here = hooks into translated guest functions (address-tied).
- **Decompilation = ownership (the real work).** RE + rewrite subsystems as clean C++, verified against
  the recomp as a behavioral oracle. Each decompiled subsystem is *ours*, not a translated blob.
- **The native modding API graduates.** `register_native`/`GetText`/asset-hooks start as recomp hooks
  (work now) and become native features of our own subsystem code as we decompile the Lua-binding,
  text, and loader systems. The API surface stays stable; the implementation moves from "hook the
  translation" to "our engine code."

## `register_native` — mechanism (all RE-confirmed)

A Lua `lua_CFunction` is a guest function pointer the VM calls indirectly. In the recomp, indirect
calls resolve via `ResolveIndirectFunction(guest_addr)` → `FunctionDispatcher::GetFunction(addr)` →
a `PPCFunc* = void(PPCContext&, uint8_t* base)`. So to expose a host function to Lua:

1. **Host function** with signature `void fn(PPCContext& ctx, uint8_t* base)`. On entry `ctx.r3 = L`
   (the lua_State*). Read args off L's Lua stack, do host work, push results, set `ctx.r3 = <nresults>`.
2. **A guest address** for it. Constraint (function_dispatcher.cpp:477): the address must lie inside a
   registered module range (the exe's ~0x82000000–0x83xxxxxx). Options:
   - Reserve a small pool of currently-unmapped/padding addresses inside the exe range and
     `dispatcher->SetFunction(addr, &fn)` them at init (preferred — no regen).
   - Or declare stub entry points in the codegen manifest (regen path).
3. **Register into the dispatcher:** `GetBoundFunctionDispatcher()->SetFunction(addr, &fn)`.
4. **Bind into Lua:** in the `rex_lhRegisterAppBindings(L)` hook, after the real bindings, call the
   game's Lua C-API via the `rex::CallFrame` pattern:
   `lua_pushcfunction(L, addr)` then `lua_setfield(L, LUA_GLOBALSINDEX, name)` (or build a module
   table). This makes `name(...)` callable from any game Lua.

### Lua C-API entry points (guest addresses)
Already known: `luaD_call` 0x82228B70, `luaL_loadfile` 0x82A25990, `lua_getfield`, `rex_lhCreateLuaState`
0x82451B38, `rex_lhRegisterAppBindings`.

**★ PREFERRED PATH — the game's OWN registrar is confirmed (2026-07-17): `lua_register_native`
@0x82353EF8.** It is the single helper the game calls ~hundreds of times to build the Debug/GUI/
Inventory/... tables — it "pushes a cclosure over (cfuncLo,cfuncHi) and `lua_setfield(name)`" (verified
in the catalog: it's the registrar column for AllowHenchman@0x8233EBF0, SetEnableParticleUpdate@0x823510F8,
SetParticleFrustumCulling, … — see `ghidra_out/lua_natives5_catalog.tsv`). Calling THIS to bind our native
= the game's exact convention in one call, so we do NOT need to pin `lua_pushcfunction`+`lua_setfield`
separately. Supporting primitives also confirmed: `lua_push_native_closure` @0x823CE420 (the cclosure
helper it uses), `lua_pushcclosure` @0x8219AA80 (Lua 5.1), `luaL_register` @0x82A25200 (stdlib table
registrar). NOTE the native is stored as a 64-bit (cfuncLo,cfuncHi) member-fn-ptr pair — for a plain host
PPCFunc at a reserved guest slot, pass the slot addr as cfuncLo (cfuncHi handling TBD from the signature).

**★ SIGNATURE PINNED (2026-07-17)** by disassembling the AllowHenchman registration in the recomp
(`Fable2_recomp.16.cpp`, fn `sub_82340328`, call site 0x82340378): the registrar (both `0x82353EF8`
AND its getter-variant sibling `0x82606E70`) is
  **`registrar(r3 = table-builder obj, r4 = name string ptr, r5 = L (lua_State*), r6 = cfunc guest addr)`**
— confirmed because `r6` computes to `0x8233EBF0` = the AllowHenchman native addr, `r5 = r31` = the fn's
entry L, `r4` = a rodata string ptr, `r3 = r1+104` = a stack-local builder. There is NO cfuncHi word in
this (plain-function) case — r6 is a single guest addr. The registering fn is `sub_82340328(r3=L,
r4=moduleDesc)`; it builds the table-builder at r1+104 via **`sub_823CE5C0(r3=&builder, r4=moduleDesc)`**
before registering. So binding a native = construct/obtain a builder, then call the registrar with our
name + our slot addr.

**★ NATIVE-ADDRESS POOL — constraint pinned (2026-07-17).** `FunctionDispatcher::SetFunction(addr,fn)`
(function_dispatcher.cpp:472) requires `addr` in a registered module range: `FindModuleByAddress` =
`code_base ≤ addr < thunk_limit`, where `thunk_limit = image_base + image_size + (code_size +
kThunkReserveSize)*2` (a big THUNK-RESERVE region ABOVE the image). `function_table_` is a `std::map`
(not a fixed-size slot array), so ANY in-range, non-colliding address works — no array bounds. The last
translated function is `0x832C99A8`, so the skeleton's `kNativePoolBase = 0x83F00000` is above real code
and (given image+2×code_size) almost certainly below `thunk_limit` → valid — but VERIFY via SetFunction's
bool return (it logs "outside all registered module ranges" + returns false if wrong), never hardcode-trust.
Cleaner still: mirror `GetOrCreateThunk` and allocate from the module's thunk region programmatically.

**REMAINING (impl, needs runtime rebuild + game test):** obtain/construct the table-builder object for a
target namespace (or bind our natives into a fresh "Mod" table), wire `ModApi.cpp` into CMakeLists +
`InstallHooks()`, build the from-source runtime, test in-game.
For arg marshalling inside a native (`lua_gettop`, `lua_tonumber`, `lua_pushnumber`, …) disassemble
`rex_lhRegisterAppBindings` / a stock native body — those calling conventions are right there.

### Marshalling helper (mirrors RunGuestFileOnL)
Host-side `L_pushnumber(ctx, base, L, x)`, `L_tonumber(ctx, base, L, i)`, etc. each set up a
`rex::CallFrame`, load args into `r3..`, call the guest C-API fn, read `r3`. A tiny arg/return shim
lets a native be written almost like a normal `int(lua_State*)`.

## Build / safety
- New module `Fable2Recomp/src/ModApi.cpp`, **gated behind an env var** (like `ConsoleInjector.cpp`);
  with it unset the hooks tail-call the originals → default build byte-identical.
- **NOT in CMakeLists by default** until the C-API addresses are pinned and it's proven — add to the
  Fable2 target + `InstallHooks()` when ready, then `rexglue-src/build_runtime2.cmd` + copy the dll
  (see CLAUDE.md §5 build gotcha).

## After `register_native`: the targeted hooks (thin, once the bridge exists)
- **GetText override** (custom text — quest headers/box/item names): ✅ DONE + user-confirmed
  (`Fable2Recomp/src/ModText.cpp`, gated `FABLE2_MODTEXT=1`, in CMakeLists). Correct target =
  **`sub_8237AA98` @0x8237AA98** — the PER-RENDER tag-id→wstring resolver (`r3`=out wstring, `r4`=babel
  DB, `r5`=&(u32 tag-id)); if `*r5` ∈ our mod-id set, fill `r3` with our UTF-16 via the game's own
  `sub_822D6408`, else tail-call the original. ⚠ The earlier `sub_82374980` target cited here was WRONG
  (boot-only tagStrObj DB-load, never hit per render); do not use it. FNV-1: basis 0x811C9DC5, prime
  0x01000193 (mult-then-xor). This is the first native modding hook proven end-to-end; next = expose
  `ModText_Set(tag,str)` via `register_native` so mods register strings dynamically.
- **Asset-load hooks** (custom images/models/audio + a real mod VFS): intercept the resource loader.
- **Input hook** (hotkeys / real console): feed keyboard/pad → Lua callbacks.
- **UI/HUD overlay** (mod-config menu): native ImGui-style overlay (cleaner than the gameface GUI VM).
- **Engine-limit removals** (RAM/draw-dist/entity-caps/res): patch the caps in our runtime.
- Each of the above, once `register_native` exists, is exposed to modders as ordinary Lua.

## Status
**★★★ register_native: DONE + PROVEN (2026-07-18).** `Fable2Recomp/src/ModApi.cpp` (in CMakeLists,
gated `FABLE2_MODAPI=1`) exposes host natives as **`Debug.ModPing()` / `Debug.ModTextSet(tag, text)`**
— self-test log-verified (bind → VM `luaD_call` → host fn ran) and stable through real gameplay.
Final mechanism (differs from the plan above in two ways worth knowing):
- **Address pool superseded:** `rex::Runtime::instance()->function_dispatcher()->AllocateThunk(fn, 0)`
  is callable straight from the exe (rexruntime exports all symbols) — no reserved pool, no regen.
- **Bind path:** `lua_pushcclosure` (sub_8219AA80) + `lua_setfield` (rex_lua_setfield @0x82A246C8)
  into the **existing Debug class table** — NOT into `_G`, and NOT via the game registrar
  @0x82353EF8 (unneeded). ⚠ `_G` is metamethod-proxied: global sets divert to a reflection backing
  store, missed reads auto-vivify self-referential proxy tables, and *calling* a pre-init proxy AVs
  natively. Class tables are plain and raw-installed but only populate ~20-30s into boot, so the bind
  runs at a luaD_call depth-0 safe point gated on (a) a RAW host-side table-walk readiness probe and
  (b) ≥64 free Lua-stack slots (stack growth at that point moves the stack under the engine's live
  StkIds → heap corruption). Full RE detail: memory `fable2-modapi-register-native`.
**GetText override: DONE + shipped** (`ModText.cpp`, target `sub_8237AA98`; now thread-safe and
dynamically enabled by `ModText_Set`).
**Current native surface (2026-07-18):** `Debug.Mod.Ping()`, `Debug.Mod.TextSet(tag, str)` (feeds
the GetText override), `Debug.Mod.Log(msg)` (host log `[modlua] ...` — the mod debug channel), plus
`Debug.ModPing/ModTextSet/ModLog` aliases. The `Debug.Mod` table is published script-side by the
gamescripts hook (a plain table raw-stored into Debug — a top-level `Mod` global cannot work, the
env proxy swallows it); the host binds into it at the safe point (zero-pin design — the
`lua_createtable` hunt dead-ended on luaL_register's fully-inlined body). ConsoleInjector's
injection paths are gated by `fable2::modapi::ReadyForInjection` (raw class-table probe + Lua-stack
headroom) — the boot heap-corruption crash class is closed.
Next: more natives (asset-load, input, UI overlay).
# Native-port update

The long-term API is a clean C++23 service in `Fable2Native`. The existing Lua/register_native
bridge is retained as a compatibility/oracle adapter while the native script host is reconstructed.
