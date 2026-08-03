# LEVEL_LOAD_RE — the LS_* level-load state machines (Track C decomp)

*2026-07-18. Evidence: `ghidra_out/ls_scan1..4.txt`, `ghidra_out/ls_decomp1..5.txt`; labels applied
from `ghidra_out/labels_20260718_lsstates.tsv` (29 rows). All addresses are guest (TU1 exe,
Ghidra project `Fable2_TU1`).*

The "LS_*" names at **0x820F8B44+** (the HANDOFF lead) turn out to be **two cooperating state
machines** that stream a level's *graphics payloads* in the background, pumped once per frame from
`TextureSystem_FrameUpdate @0x82181B68`:

1. **LevelGraphicsFile** — loads the engine level file (the `LevelGraphicsFile`-magic payload the
   AssetBrowser calls the "engine level" / `.lev` graphics data) + the `.lmp` lightmap.
2. **EngineResourceList** — loads and applies the `<level>.engine_data` payload
   (`EngineResourceList` magic, version 3 — the resource-hash list our editor patches).

They are *not* the game-side entity/script load (the `.save` entity registry is consumed
elsewhere — see Open leads).

---

## 1. String tables (state names)

Names are plain C strings; each machine has a **name pointer table in .data indexed by state**
(debug-name lookup):

**LevelGraphicsFile — 8 states, `const char* names[8]` @ 0x83317D4C**

| idx | name (string addr) |
|---|---|
| 0 | `LS_NOT_LOADED` (0x820F6C0C) |
| 1 | `LS_LOADING_LIGHTMAP_DATA` (0x820F8C04) |
| 2 | `LS_BEGIN_LOADING_ENGINE_RESOURCE_LIST` (0x820F8BDC) |
| 3 | `LS_WAITING_FOR_ENGINE_RESOURCE_LIST_TO_LOAD` (0x820F8BB0) |
| 4 | `LS_BEGIN_LOADING_ENGINE_LEVEL_FILE` (0x820F8B8C) |
| 5 | `LS_WAITING_FOR_ENGINE_LEVEL_FILE_TO_LOAD` (0x820F8B60) |
| 6 | `LS_PROCESSING_ENGINE_LEVEL` (0x820F8B44) |
| 7 | `LS_LOADED` (0x820F6B84) |

**EngineResourceList — 6 states, `const char* names[6]` @ 0x83318584**

| idx | name (string addr) |
|---|---|
| 0 | `LS_NOT_LOADED` (0x820F6C0C) |
| 1 | `LS_BEGIN_LOADING_RESOURCE_LIST` (0x820F6BEC) |
| 2 | `LS_WAITING_FOR_RESOURCE_LIST_LOAD` (0x820F6BC8) |
| 3 | `LS_BUILDING_RESOURCE_LIST` (0x820F6BAC) |
| 4 | `LS_LOADING_TEXTURE_ATLASES` (0x820F6B90) |
| 5 | `LS_LOADED` (0x820F6B84) |

Related class-name strings: `"LevelGraphicsFile"` @0x820F8C20, `"EngineResourceList"` @0x820F6C1C,
`"lmp"` @0x820F8C34, `"engine_data"` @0x820F8C38, `"Unsupported engine level version "` @0x820F8C58.

## 2. Where state lives + dispatcher mechanism

- **LevelGraphicsFile is an object** (heap): current state = `this+4` (int), state-entry time in
  seconds = `this+8` (double). The instance hangs off the streaming context at **ctx+0xD4C**
  (ctx flags: +0xE6C pause-ish, **+0xE80 = level-graphics-loaded flag**, set when the dispatcher
  returns nonzero). The context sits in the model-streaming array @0x8349F79C.
- **EngineResourceList state is global**: state @ **0x83496E34**, entry time @ **0x83496E38**,
  async reader object @ **0x8349FA14**, target name-hash @ **0x8349FA48**, atlas-pending flag
  @ **0x83496E32**, texture-atlas array (32 × 8-byte entries) @ **0x8349FA10**, current atlas index
  @ **0x83318580** (init −1), magic lh_string @ **0x8349FA4C**.
- **Transitions**: an "advance" helper increments the state and stamps
  `(QPC − 0x834970A0)/0x834970A8` seconds — `LevelGraphicsFile_AdvanceState @0x82ABAA88` and
  `EngineResourceList_AdvanceState @0x82A51BA0`. Failure paths reset state to 0.
- **Dispatchers are switch statements**, one call per frame:
  `LevelGraphicsFile_StateDispatch @0x82ABAAE8` (bctr jump table @0x82ABAB24) and
  `EngineResourceList_StateDispatch @0x82A51A60`. Return convention (both): **0 = busy (stay),
  1 = idle/failed, 2 = load complete**.
- **The pump** is `TextureSystem_FrameUpdate @0x82181B68`: `bl 0x82A51A60` @0x82181C68 (only while
  ERL state ∈ 1..4), `bl 0x82ABAAE8` @0x82181CEC and @0x82181D60 (LGF object from ctx+0xD4C; on
  nonzero return sets ctx+0xE80).

## 3. LevelGraphicsFile machine — per-state behavior

| st | name | handler | what it does |
|---|---|---|---|
| 0 | NOT_LOADED | inline @0x82ABACA4 | reset: `this+4=0`, timestamp; dispatcher returns 1 |
| 1 | LOADING_LIGHTMAP_DATA | `LevelGraphicsFile_State1_LoadLightmap @0x82ABACE8` | builds `<level>.lmp` name and starts the lightmap load |
| 2 | BEGIN_LOADING_ENGINE_RESOURCE_LIST | inline @0x82ABAB58 | builds `<this+0x10 level name>.engine_data`, hashes it (`lh_string_name_hash @0x82C037F0`) → **stw hash @0x8349FA48** (0x82ABABC4), sets atlas flag 0x83496E32=1 (0x82ABABC8), **kicks the ERL machine 0→1** via `bl 0x82A51BA0` (0x82ABABCC), then advances itself |
| 3 | WAITING_FOR_ENGINE_RESOURCE_LIST | inline @0x82ABABEC | **pumps the ERL dispatcher** (`bl 0x82A51A60`); advances when it returns nonzero |
| 4 | BEGIN_LOADING_ENGINE_LEVEL_FILE | inline @0x82ABAC0C | `resource_async_open_by_name @0x82C698F0(this+0x10, &this+0x14, 0, −1, 1, 1)` — async open of the level graphics file by name hash; fail → reset |
| 5 | WAITING_FOR_ENGINE_LEVEL_FILE | inline @0x82ABAC50 | polls reader `this+0x14` status (vtbl+0xC): 0 wait, 1 advance, else reset |
| 6 | PROCESSING_ENGINE_LEVEL | `LevelGraphicsFile_State6_ParseEngineLevel @0x82ABAEF0` | **the parser** (see §5) |
| 7 | LOADED | inline @0x82ABACC4 | returns 2 and resets to 0 |

Unload: `LevelGraphicsFile_Unload @0x82AB9708` (called from the streaming manager's level-switch
paths @0x82A53EE4 / 0x82A54360 / 0x82A5439C / 0x82A5448C) releases the atlas array entries and the
per-level streaming data (ctx entry+0xD50 via 0x82A54E78), then sets state back to 0.

## 4. EngineResourceList machine — per-state behavior

| st | name | handler | what it does |
|---|---|---|---|
| 0 | NOT_LOADED | inline | reset; return 1. (Kicked 0→1 by LGF state 2, see above) |
| 1 | BEGIN_LOADING_RESOURCE_LIST | inline | `resource_async_open_by_hash @0x82C68E50(hash@0x8349FA48, &reader@0x8349FA14, 0, −1, 1, 1)`; fail → reset |
| 2 | WAITING_FOR_RESOURCE_LIST_LOAD | `EngineResourceList_State2_WaitLoad @0x82A51C00` | poll reader status; if wait exceeds float threshold @0x8209BE04, bump reader priority (vtbl+0x24(obj,3) + vtbl+0x10) |
| 3 | BUILDING_RESOURCE_LIST | `EngineResourceList_State3_Build @0x82A51CE8` | **parses engine_data** (see §5) |
| 4 | LOADING_TEXTURE_ATLASES | inline | waits until atlas entry `*(0x83318580*8 + [0x8349FA10])` is ready (`0x82AAF190`); advance |
| 5 | LOADED | inline | return 2; reset |

Static init: `EngineResourceList_StaticInit @0x82A50724` (allocates the 32-entry atlas array
@0x8349FA10 + manager @0x8349FA0C); `cinit_engine_resource_list_magic @0x8328C8F8` assigns
`"EngineResourceList"` into the global magic string @0x8349FA4C.

## 5. File formats consumed, and by which function

### engine level (`LevelGraphicsFile` payload) — parsed by 0x82ABAEF0

Structure (verified **identical** to the AssetBrowser's `ParseEngineLevel` in
`Fable2AssetBrowser/source/src/Level/LevelLoader.cpp` @~2681):

- magic: 17 chars `LevelGraphicsFile` (read `bstream_read_string_n @0x82A21330`, compared
  `lh_string_neq_cstr @0x8229B1B8`)
- u32 version, **accepted 11..12** (else error string @0x820F8C58)
- u32 record count, then per record a u32 **type**:

| type | handler | payload |
|---|---|---|
| 2 | `EngineLevel_ReadType2_PropModelBlock @0x82AB9CF8` (bl @0x82ABB3DC) | 4 model-path strings (model/shadow/LOD/extra) + u32 instance count + per instance {3 flag bytes, u64 hash, floats (reader 0x82A21910)} |
| 4 | inline in 0x82ABAEF0 | name string + 8 bytes, name → hash |
| 5 | `EngineLevel_ReadType5_OpenSubFile @0x82AB9818` (bl @0x82ABB374) | name string; path-resolved against base lh_string @0x8349F7CC (`path_resolve_with_base @0x821BD660`), opened (0x82C6F810) and wrapped in a 16 KB buffered stream (`bstream_ctor_from_file @0x82A22840`) |
| 0x15 (21) | `EngineLevel_ReadType21_InstancedPropBlock @0x82ABA800` (bl @0x82ABB3C4) | 2 strings + u64 + 2 flag bytes + counts + packed instances (v11: f32×4; v12: vec3 + half quat + half scale) |
| 0x20 (32) | `EngineLevel_ReadType32_NamedResourceHash @0x82AB99F8` (bl @0x82ABB3A0) | name string → hash |

**★ Editor relevance:** the Type-2 records that `LevelEdit.cpp append_additions_to_level` appends
are consumed by **0x82AB9CF8**, reached only through LGF state 6. Its version gate (11..12) and
field layout match the editor exactly, so appended records go down the same code path as stock
ones. Per-record strings are read with `bstream_read_string @0x82A23028` and converted to resource
handles by name hash (`lh_string_name_hash @0x82C037F0` → the `texttag_get_cached_hash` primitive),
i.e. **new model paths must resolve through the same hashed-name resource lookup** — the model must
be present in the mounted banks (or its hash added to engine_data, next section) to bind.

### engine_data (`EngineResourceList` payload) — parsed by 0x82A51CE8

- magic string compared against global @0x8349FA4C (`"EngineResourceList"`)
- u32 version, **must be exactly 3** (matches `LevelEdit.cpp` "engine_data version != 3")
- 1 byte flag (endianness/format bit passed into the entry reader 0x82A22258)
- **three** u32 hash arrays, stored into growable arrays @0x8349FA18 / @0x8349FA28 / @0x8349FA38
  (count + count×u32 each). The editor's resource-list patcher edits array 1 — the guest reads all
  three, so trailing lists must be preserved when repacking (the editor's append-in-place approach
  is safe).
- then `0x822B4FD0(mgr@0x8349FA0C, 0x1E)` and, if flag 0x83496E32 set, selects/creates this level's
  **texture-atlas entry** → index stored @0x83318580; entry marked loading (+0x34=1, +0x30=0x200000,
  refcount +0x38++). State 4 then waits for that atlas.

### lmp (lightmaps)
Loaded first (LGF state 1, `@0x82ABACE8`, name `<level>.lmp`). Not parsed further this session.

### How files are opened
Both machines open payloads via the **hashed-name async resource system**:
`resource_async_open_by_name @0x82C698F0` → `resource_async_open_by_hash @0x82C68E50` (manager
global @0x8333597C, lookup vtbl+0x10, open 0x82C68CD0). This is the read-side of the bank mounts
(`bank_open_by_name @0x82C6D7C8`, `mount_tu1_data_bnk @0x822F2F30` chain): whatever is in the
mounted `levels.bnk`/`streaming.bnk` is addressable by name hash. Returns 0 for unknown hashes —
i.e. **a mod asset with a name not present in any mounted bank simply fails the open** (LGF state 4
resets the machine).

## 6. Bonus: mesh loading cluster (same rdata neighborhood)

The vtable right before the LS_* strings (@0x820F8B20) belongs to the mesh streaming class:
`mesh_file_read_header @0x82AB4DC8` (magic `MeshFile`/`DefMeshF`, **version 0x21..0x24**, version
@this+0x90; error strings @0x820F8A80/0x820F8AB4/0x820F8B04), 3 async reader slots @this+0xA4 →
parsed mesh objects (0xF0 bytes, ctor 0x82AB0A30) @this+0x98: `mesh_stream_poll_slots @0x82AB4768`,
`mesh_stream_finalize_slot @0x82AB49D0`, `mesh_stream_release_slots @0x82AB47E8`. This is where the
Type-2 model paths ultimately get streamed as MDL data.

## 7. Open leads

- **Who starts the LGF machine** (writes `this+4 = 1` and the level name into `this+0x10`): the
  setter is register-relative (no imm ref); the unload callers put it in the streaming manager
  region 0x82A53E00–0x82A54500 — decompile that next (also covers `0x82A54500` per-entry streaming).
- **Name-table consumers**: nothing materializes 0x83317D4C / 0x83318584 via lis/addi — the debug
  name lookup is either dead (stripped logging) or reached via a larger struct base. Low value.
- **`.save` entity registry / game-side level load**: not in these machines — this pair is purely
  graphics streaming. The gameplay-side consumption (entities, GDB `Globals/globals.gdb`, script
  banks) is a separate path (Track C follow-up; start from the bank mounts and
  `Debug.LoadLevel`/gameflow natives in `lua_natives5_catalog.tsv`).
- Type-21 loop2 (second instance array) and the Type-5 sub-file's 2 u32s were not fully traced
  (decompiler false-noreturn on `0x82214DD8` truncates tails; use `DisasmRange` on the tails if
  needed).
- `0x82AAF190` (atlas-ready poll) and `0x82A52490` (hash-array reserve) named only in comments —
  label once confirmed.
