# Blender Integration for Fable II Modding

Authoritative plan for bringing Fable II assets into Blender for viewing, editing,
and (eventually) round-trip export into the game's mod-overlay pipeline.

**Status:** design + first scaffolding increment (2026-07-19). Nothing in the
`bpy`-facing addons has been run in Blender yet — every addon is marked
**⚠ NEEDS BLENDER TESTING** with a manual test procedure.

> Scope note: this doc covers the *Blender* side. The C++ format code it plugs
> into is documented at the source of truth — `Fable2AssetBrowser/source/src/MDL`,
> `.../animations`, `.../Level` — and in `docs/SYSTEMS_ANALYSIS.md`,
> `docs/MODDING_ENVIRONMENT.md`. Read those for format internals; this doc treats
> them as the spec and focuses on the integration surfaces.

---

## 0. TL;DR / recommendation

- **Two tracks, both real, and they converge on one seam.**
  - **Track A — pure-Python `bpy` addon.** Ships today, no Blender build, easy to
    iterate. Good enough for level layout import (already exists:
    `FableLevelImporter.py`) and for the *common-case* MDL mesh/skeleton.
  - **Track B — source-level / native.** Reuse the AssetBrowser's C++ format
    decoders (the emerging **`libf2`**) directly, so Blender parses MDL/AnimBank/
    BNK with the *same* battle-tested code the AssetBrowser uses, instead of a
    blind Python reimplementation.
- **The recommended bridge is NOT a Blender fork.** Blender's own official glTF
  addon already demonstrates the pattern we want: **a native shared library
  (`bf_intern_draco_bridge.dll`) bundled beside a Python addon and loaded via
  `ctypes`** (`scripts/addons_core/io_scene_gltf2/io/com/library.py` +
  `io/exp/draco.py`). We copy that pattern exactly: build **`libf2` as a shared
  lib exporting a flat `extern "C"` API**, bundle it beside a Fable Blender addon,
  and drive it from Python via `ctypes`. **No Blender source changes, no fork, no
  tracking upstream.**
- **Do NOT maintain a custom Blender fork** (Track B-hard) unless a future feature
  genuinely needs to touch Blender's C/C++ core (it won't for asset I/O). Forking
  means owning a multi-hour build and rebasing on Blender `main` forever, for zero
  benefit over the ctypes-bridge approach.
- **Concrete path:** keep the Python addons as the UI/DCC glue and the near-term
  importer; stand up `libf2` (already begun as `f2tool`/P1 in
  `docs/MODDING_ENVIRONMENT.md`) with a thin `f2blender` C-API; have the addon
  prefer the DLL and fall back to pure Python when it's absent. That fallback is
  what makes Track A and Track B the *same addon*, not two codebases.

**Open decisions for the user** (see §8): (1) commit to the **libf2 .dll + ctypes**
bridge (recommended) vs. a pure-Python long game; (2) who builds/ships the
`libf2` DLL and for which platforms; (3) whether animation import goes through the
AssetBrowser's existing FBX export (safe, available now) or waits for a native
AnimBank path in libf2.

---

## 1. How the CURRENT pipeline works

Today's flow is **AssetBrowser (C++) → intermediate files → Blender (Python)**, and
it is **import-only, level-only, via FBX/glTF intermediates**.

```
  Fable II BNKs ──▶ AssetBrowser  ──(Level::ExportAsync)──▶  <level>.fable  (JSON manifest)
                     C++ decoders                            models/*.fbx|*.glb
                                                             terrain/*.fbx|*.glb  + DDS/BINs
                                                                    │
                                                                    ▼
                                          Blender: FableLevelImporter.py
                                          (File ▸ Import ▸ Fable 2 Level (.fable))
                                          → collection-instances a model library
                                            per prop, places instances by matrix,
                                            builds a DDS-splat terrain material
```

### 1.1 The `.fable` manifest schema

Written by `Level::ExportAsync` in
`Fable2AssetBrowser/source/src/Level/LevelExport.cpp` (the JSON is assembled by
hand near line ~1272). Consumed by `FableLevelImporter.py`. Fields observed:

```jsonc
{
  "version": 1,
  "format": "FBX" | "GLB",          // which intermediate the models/terrain use
  "level": { "name": "<folder>", "source": "<full/level/path>" },

  "models": [                        // the model LIBRARY (unique meshes)
    { "source": "art/.../foo.mdl",   //   original in-game path (lower/slash)
      "file":   "models/foo.fbx",    //   exported intermediate, relative to .fable
      "instances": 12 }              //   how many placements reference it
  ],

  "instances": [                     // per-placement transforms
    { "model":   "models/foo.fbx",   //   -> matches models[].file
      "source":  "art/.../foo.mdl",
      "type":    <int>,              //   level record kind
      "hash":    <uint64>,
      "position":[x, z, y],          //   NOTE axis order: values[0], values[2], values[1]
      "matrix":  [16 floats],        //   row-major 4x4 (see write_matrix_json)
      "fullTransform": true|false,   //   true => raw[] carries a full 3x3+scale
      "flags":   [f0, f1, f2],
      "raw":     [20 floats] }       //   raw level record values (importer rebuilds
                                     //   the matrix from these — see raw_prop_matrix)
  ],

  "terrain": {
    "mesh":    "terrain/terrain.fbx",
    "exported": true|false,
    "sourceEhf": "<path>",
    "width": W, "height": H, "tileSize": T,
    "minHeight": f, "maxHeight": f,
    "materialsParsed": true|false,
    "splat":   { "file": "terrain/splat_indices.bin", "width": W, "height": H },
    "weightMaps": { "width": W, "height": H, "files": ["terrain/w0.dds", ...] },
    "uvSets":  { "tiled": 0, "weights": 1 },
    "paintResources": [ { "width":W, "height":H, "pixelFormat":P }, ... ],
    "lods": [
      { "index": i, "materialFlags": F,
        "strings": [6 material path strings],
        "files":   [6 DDS relative paths],   // base-colour tiles per layer
        "weight":  "terrain/weightN.dds",    // splat weight for this LOD layer
        "params":  [...] }                   // scale etc. (importer reads params[0][0])
    ]
  }
}
```

### 1.2 What `FableLevelImporter.py` does with it

- Imports each `models[].file` (FBX/glTF) once into a hidden **data collection**
  (`<Level>_ModelLibrary_<model>`), then creates a **collection-instance empty**
  per `instances[]` entry, transformed by `raw_prop_matrix()`.
- Coordinate handling is centralised in a set of `mathutils.Matrix` constants —
  `FABLE_TO_BLENDER`, `GLTF_TO_BLENDER`, `RAW_MODEL_TO_FABLE_LOCAL`,
  `FBX_MODEL_TO_FABLE_LOCAL` — and `raw_prop_matrix()` rebuilds a placement matrix
  from the 20-float `raw[]` record (position swizzle `x, z, y`; either a full
  3x3+scale or a sin/cos yaw + per-axis scale). **These constants are the
  authoritative Fable↔Blender basis change and are reused by the new addons.**
- Builds a terrain **splat material**: for each LOD layer, a tiled base-colour DDS
  × a weight DDS, `MULTIPLY` then `ADD`-composited into the Principled BSDF base
  colour. Includes a hand-rolled 32-bit uncompressed-DDS RGBA loader
  (`load_rgba_dds`) so packed weight maps load without an external DDS plugin.

---

## 2. Gap analysis

| Gap | Detail | Which track closes it |
|---|---|---|
| **Import only** | No path back into the game. The level editor already ships durable **edit-sets** + **"Export Edits as Mod"** (overlay under `assets/mods/enabled/<name>/` + `f2ab_mod.json`; see memory `fable2-level-editset-mod-export`). Blender edits currently can't feed that. | A (writer) + B (encoders) |
| **Level only** | No single-asset (character/prop/weapon) MDL import, no skeleton, no animation. | A (common case) / B (all cases) |
| **FBX/glTF intermediate** | Every model round-trips through a huge FBX writer (`MdlFbxExport.cpp`) and Blender's FBX importer. Slow, lossy at the margins, and couples us to FBX quirks. | B removes the intermediate |
| **No skeleton / no skin in the level path** | The level importer instances rigid meshes; it never builds an armature. The FBX path *does* carry skeleton+skin+anim, but only when you export a single MDL to FBX by hand. | A (native MDL) / B |
| **No animation** | AnimBank clips (hash-addressed, 4-mode Bézier codec) are only reachable via the AssetBrowser's FBX-with-animations export. Nothing imports them as Blender **actions**. | B (native) or A-via-FBX |
| **Format drift risk** | A blind Python reimplementation of the MDL parser can silently diverge from the C++ (the real parser has ~5 heuristic recovery scanners). | **B eliminates this** by reusing the C++ |

---

## 3. Blender's integration surfaces (source-level study)

Studied from the shallow clone at `third_party/blender/` (Blender `main`) plus the
Python API docs.

### 3.1 Add-on / Extension architecture

- **Add-ons are Python.** An importer is a Python module that registers an
  `Operator` subclass (using `bpy_extras.io_utils.ImportHelper`) and appends a menu
  func to `bpy.types.TOPBAR_MT_file_import`. Reference:
  `scripts/addons_core/io_scene_fbx/__init__.py` (`bl_info`, `register()`,
  `menu_func_import`). Our `FableLevelImporter.py` already follows this exactly.
- **Legacy `bl_info` dict vs. the new Extensions manifest.** Modern Blender (4.2+)
  prefers an **Extension** = a package with a `blender_manifest.toml`
  (`schema_version`, `id`, `version`, `blender_version_min`, `type = "add-on"`,
  and crucially **`[build]` + wheel/platform bundling**). Legacy single-file
  addons with a `bl_info` dict still load. **Plan:** keep `bl_info` for now for
  simplicity; graduate to a `blender_manifest.toml` package when we bundle the
  native `libf2` DLL (the manifest is how you declare platform-specific binary
  payloads and Python `wheels`).
- **The FBX importer is 100% Python** (`import_fbx.py`, `fbx_utils.py`,
  `parse_fbx.py` — no compiled helper), and leans on **`numpy`** for bulk vertex
  work and on `Mesh.from_pydata` / `foreach_set`. This is the performance model our
  Python-side code should imitate (avoid per-vertex Python loops for big meshes).

### 3.2 The native-library seam (the important precedent)

Blender's **own glTF add-on ships a native C++ library and calls it from Python
via `ctypes`** — this is the exact mechanism we want:

- **Bridge lib:** `intern/draco_bridge/` builds `bf_intern_draco_bridge` **as a
  `SHARED` library** (`add_library(... SHARED ...)` in its `CMakeLists.txt`),
  exporting a flat C API decorated with
  `extern "C" __declspec(dllexport) <ret> __cdecl` (the `API()` macro in
  `intern/draco_bridge/intern/common.h`). The API is handle-based:
  `decoderCreate()` → opaque `Decoder*`, `decoderDecode(dec, data, len)`,
  `decoderGetVertexCount(dec)`, `decoderCopyAttribute(dec, id, out)`,
  `decoderRelease(dec)` (`intern/draco_bridge/intern/decoder.h`).
- **Locator:** `io/com/library.py :: dll_path()` picks
  `{name}.dll` / `lib{name}.so` / `lib{name}.dylib` by `sys.platform` and finds it
  **beside the add-on's `__init__.py`** (or under `resource_path('SYSTEM_LIBS')`
  for system builds).
- **Loader:** `io/exp/draco.py` does `cdll.LoadLibrary(str(dll_path(...)))` then
  sets `restype`/`argtypes` for each function and passes raw buffers (`c_void_p`,
  `c_size_t`) — zero-copy from Python `bytes`/`numpy` into C++.

**Conclusion:** we can achieve "native, reuses the C++ format code" **without
touching Blender's tree at all** — build `libf2` as a shared lib with a flat
`extern "C"` API and load it exactly like Draco. That is Track B, minus the fork.

### 3.3 The C/C++ module layout (for completeness)

`source/blender/` holds the app core (`blenkernel`, `blenlib`, `bmesh`,
`makesdna`/`makesrna` for the DNA/RNA data model and the Python type wrappers,
`python/` for the `bpy` binding). `intern/` holds self-contained C/C++ helper libs
(ghost, cycles, guardedalloc, **draco_bridge**…). `bpy` itself can be built as a
Python module (`build_files/cmake/config/bpy_module.cmake`,
`WITH_PYTHON_MODULE`). We do **not** need to add code under `source/blender/`:
adding a new decoder there would mean forking. The `intern/*_bridge` pattern is the
sanctioned "native helper for a Python addon" location, and even that only matters
**if** we choose to build our bridge *inside* a Blender checkout. We don't have
to — `libf2` can live entirely in the AssetBrowser/Fable2RE tree and just be
copied next to the addon.

---

## 4. Track A vs Track B — honest comparison

### Track A — pure-Python `bpy` addon
- **Pros:** ships now; zero native build; trivial to install (drop-in .py);
  cross-platform for free; easy for contributors to read/patch; already proven by
  `FableLevelImporter.py`.
- **Cons:** the format code must be *reimplemented* in Python and kept in sync
  with the C++ by hand. For MDL that is genuinely risky — the real parser
  (`ModelParser.cpp`) has multiple heuristic byte-scan recovery paths (foliage
  36/48-byte strides, polymsh brute-scan, townhouse multi-instance, cloth blocks,
  the engine-record LOD parser). Reproducing all of them blind, untested, is a
  correctness minefield. Pure Python is also slow on large meshes without numpy
  care.
- **Verdict:** correct and sufficient for **level layout** and the **common-case
  MDL** (28-byte skinned + 20-byte alt vertices). Not the place to chase every
  weird MDL variant.

### Track B — native (`libf2` shared lib + `ctypes`)
- **Pros:** reuses the **exact** C++ decoders the AssetBrowser already ships, so a
  mesh that displays correctly in the AssetBrowser displays correctly in Blender —
  no drift, all recovery heuristics included. Fast. Directly advances the
  project's "own our C++ modding environment / libf2" direction
  (`docs/MODDING_ENVIRONMENT.md` P1). Matches the glTF/Draco precedent one-to-one.
- **Cons:** needs a compiled `libf2.dll` (and eventually `.so`/`.dylib`); someone
  must build and ship it per platform; adds a build target and a stable C ABI to
  maintain. The C-API has to marshal std::vector-shaped results across the FFI
  (handle + copy-out, like Draco).
- **Verdict:** the destination. The cost is "own a small stable C ABI over
  libf2", which we want anyway for `f2tool`/CI.

### Track B-hard — custom Blender fork (rejected)
Building/patching Blender's own tree to add a Fable importer in C++. **Rejected:**
multi-hour builds, perpetual rebasing on upstream, and it buys nothing over the
ctypes bridge for asset I/O. Only revisit if we ever need to change Blender's core
behaviour (we don't for import/export).

### Recommendation
**Track A now, converging into Track B via a `libf2` `.dll` loaded by `ctypes`,
with pure-Python fallback.** One addon, two backends. The addon prefers the native
lib when present (correctness + speed) and falls back to the Python parser when
it's absent (portability + zero-install). This is precisely the Draco model and it
makes the two tracks the *same* codebase rather than a costly either/or.

---

## 5. The `libf2` bridge design (`f2blender` C-API)

A thin, flat, handle-based C API over the AssetBrowser decoders — modelled on
`draco_bridge`. Lives in the Fable2RE tree (e.g. a future
`Fable2AssetBrowser/source/libf2/` + `f2blender.h`), built `SHARED`, copied beside
the addon. Sketch (names illustrative; to be implemented on the C++ side by the
libf2/recomp track, NOT in this doc's scope):

```c
// f2blender.h  — extern "C" __declspec(dllexport) ... __cdecl  (Draco-style API())
typedef struct F2Mdl F2Mdl;

F2Mdl*   f2_mdl_open(const void* data, size_t len, const char* src_path); // parse
void     f2_mdl_close(F2Mdl*);

int      f2_mdl_bone_count(F2Mdl*);
int      f2_mdl_bone_parent(F2Mdl*, int bone);
size_t   f2_mdl_bone_name(F2Mdl*, int bone, char* out, size_t cap);
int      f2_mdl_bone_transform(F2Mdl*, int bone, float out11[11]); // quat,pos,scale,pad

int      f2_mdl_geom_count(F2Mdl*);
int      f2_mdl_geom_counts(F2Mdl*, int gi, int* out_verts, int* out_indices, int* out_skinned);
void     f2_mdl_geom_positions(F2Mdl*, int gi, float* out /*3*verts*/);
void     f2_mdl_geom_uvs      (F2Mdl*, int gi, float* out /*2*verts*/);
void     f2_mdl_geom_indices  (F2Mdl*, int gi, uint32_t* out /*indices*/);
void     f2_mdl_geom_skin     (F2Mdl*, int gi, uint16_t* ids /*4*verts*/, float* wts /*4*verts*/);
size_t   f2_mdl_geom_material (F2Mdl*, int gi, int slot, char* out, size_t cap); // 0=diffuse,1=normal,...

// AnimBank (feeds Blender actions):
typedef struct F2Anim F2Anim;
F2Anim*  f2_anim_open(const char* clip_hash_or_name, F2Mdl* retarget_to);
int      f2_anim_frames(F2Anim*); float f2_anim_fps(F2Anim*);
int      f2_anim_sample(F2Anim*, int frame, float* quats /*4*bones*/, float* trans /*3*roots*/);
void     f2_anim_close(F2Anim*);
```

The Python side loads it with the **same three helpers as glTF** — a `dll_path()`
by `sys.platform`, `cdll.LoadLibrary`, and per-function `restype`/`argtypes` — then
copies results into `numpy` arrays for `Mesh.foreach_set`. Our
`fable_libf2_bridge.py` (§6) already implements the locator + loader + graceful
absence, so wiring real functions is just adding `argtypes`.

---

## 6. What ships in this increment (files)

All new, self-contained, registerable. Under `Fable2AssetBrowser/source/addons/`
except the design doc.

1. **`docs/BLENDER_INTEGRATION.md`** — this document.
2. **`fable_mdl_format.py`** — pure-Python, **no-`bpy`** MDL parser. A faithful
   port of the *deterministic* core of `ModelParser.cpp` (header → bones →
   bind-pose transforms → mesh/material list → the 28-byte skinned "normal" and
   20-byte "alt" vertex layouts → strip→triangles → submesh split). Heuristic
   recovery paths (foliage/cloth/polymsh-scan/multi-instance/StringBlock) are
   **intentionally not ported**; the parser **raises `MdlParseError` with a byte
   offset** instead of guessing, so divergence is visible. Runs stand-alone
   (`python fable_mdl_format.py file.mdl` prints a summary) — this is the Track A
   engine **and** the fallback backend for the bridge. ⚠ NEEDS TESTING against a
   real exported MDL.
3. **`fable_libf2_bridge.py`** — the **Track A↔B bridge**. Locates + loads a
   `libf2`/`f2blender` shared lib via `ctypes` using the glTF `dll_path()` pattern
   (`{name}.dll` / `lib{name}.so` / `lib{name}.dylib`, beside the addon), and
   exposes a uniform `load_mdl(path) -> (info, geoms)` that returns the **same
   objects** as `fable_mdl_format.py`. If the DLL is absent or lacks a symbol, it
   transparently falls back to the pure-Python parser. Today the native branch is
   scaffolding (the `f2blender` DLL doesn't exist yet) — but the seam is real and
   matches Draco, so when libf2 ships a DLL this file is where `argtypes` get
   wired. ⚠ NEEDS TESTING once a DLL exists; the fallback path needs testing now.
4. **`FableMdlImporter.py`** — the `bpy` addon: `File ▸ Import ▸ Fable 2 Model
   (.mdl)`. Calls `fable_libf2_bridge.load_mdl()`, builds a Blender **mesh**
   (positions/UVs via `from_pydata` + a UV layer), an **armature** from the bones
   (edit-bones placed from the world-space bind pose derived from the
   `BoneTransforms`), binds the mesh with an **Armature modifier** + per-bone
   **vertex groups/weights**, and reuses the `FABLE_TO_BLENDER` basis from
   `FableLevelImporter.py`. Verbose logging throughout. ⚠ NEEDS BLENDER TESTING.
5. **`fable_anim_format.py`** — pure-Python, **no-`bpy`** AnimBank decoder (the
   animation companion to the MDL parser). A faithful port of the **deterministic**
   codec: the `animation_toc` container (`AnimBank` magic 1/5 → clip records
   key0/key1/data_offset/frame_count/fps/events + special *track-map* records that
   name each bone track), the `animation_data` blob (magic `0xCEA5EBED` v7 → per-clip
   header at `data_offset` + per-bone bit-offset directory), and the **4-mode
   Bézier-quantized decode** (mode 0/1 constants, mode 2 quantized constant, mode 3
   the cubic-Bézier run-length keyframe curve incl. the quantized curve-index table
   and the per-32-frame page table). Produces per-frame per-bone `{quat, trans}`
   samples; clips are addressed **by hash** (`id_%08X`) and tracks are named **by
   bone name** via the track map. Unlike the MDL heuristics, this codec is fully
   deterministic, so the port is defensible; it still **raises `AnimParseError`
   with a byte offset** on any bad container/header rather than guessing. Runs
   stand-alone (`--selftest` runs the codec unit tests; `<root>` loads + decodes
   real files). ⚠ NEEDS TESTING against the real two-file pair (the codec math and
   a synthetic clip are verified — see §6a).
6. **`FableAnimImporter.py`** — the `bpy` addon: `File ▸ Import ▸ Fable 2 Animation
   (.animation_toc)`. Loads the toc/data pair, decodes a chosen clip through the
   bridge, and writes a Blender **Action** onto a previously-imported Fable
   **armature**: matches decoded tracks → pose bones **by normalised bone name**
   (mirroring `AnimRigMap.h`, incl. the `shadow_` alias), builds `rotation_quaternion`
   fcurves (and root/optional `location` fcurves), converting each sample into
   Blender space with the same `FABLE_TO_BLENDER` basis the mesh importer uses.
   Unmatched tracks are **skipped and loudly logged**. Sets the scene frame range +
   fps so the user can scrub immediately. ⚠ NEEDS BLENDER TESTING.
7. **`fable_libf2_bridge.py`** — extended with anim entry points
   (`load_anim_toc` / `open_anim_data` / `decode_clip`) that have the **same
   native-fallback shape** as `load_mdl`: they call the Python codec today and are
   where the native `f2_anim_*` C-API gets wired when the DLL ships.

### Why the pure-Python MDL parser AND the bridge, not one or the other
The coordinator's ask is a native, C++-reusing integration; the constraint is that
I cannot build or test anything. The bridge (`fable_libf2_bridge.py`) is the
*shape* of Track B and is verifiable by inspection against the glTF precedent — but
its native branch can't be exercised until libf2 ships a DLL. The pure-Python
parser gives the user something **testable today** (Track A) *and* doubles as the
bridge's fallback backend, so the increment is useful immediately and is exactly
the seam Track B slots into. This is the "produce a correct testable artifact over
an ambitious unverifiable one" call, made explicitly.

### 6a. Verification done (no Blender / no build required)

The **no-`bpy`** format modules were verified by byte-compiling and running
stand-alone (Python 3), the same way the MDL parser was:

- **MDL** (`fable_mdl_format.py`): half-float decode, strip→triangle winding, and a
  hand-built synthetic `MeshFile` (bone table, bind-pose transforms, 5-string
  material table, 28-byte skinned vertex layout) parse end-to-end with correct
  positions/UVs/bone-ids/weights/indices — validating the byte-offset arithmetic
  against `ModelParser.cpp`.
- **Anim codec** (`fable_anim_format.py`): a `--selftest` asserts the dequant
  round-trips (`dequant_unit`/`_unit_bits`/`_world_bits`), the `mode3_curve_factor`
  cubic-Bézier endpoints + linear default, the `BitReader` LSB-within-big-endian-word
  semantics, and FNV-1/normalise. A **synthetic mode-2 clip** (2 bones, 1 frame)
  decodes through the real `_decode_block`+`_interpolate` and matches the expected
  dequantized quat/translation. A separate **synthetic mode-3 keyframe clip** (a
  world-24 translation channel with a linear-curve segment over a 5-frame block)
  decodes to the exact per-frame ramp — exercising the full mode-3 bit layout
  (first_raw, value_scale, packed index/value bits, segment table, curve factor).
  The **bridge** anim path (`fable_libf2_bridge.decode_clip`) was confirmed to route
  through the Python fallback and produce identical output.

Still **unverified** (needs the real assets / Blender, hence the ⚠ marks): decode
against the actual `fable2_anims.animation_{toc,data}` pair; the Blender importers'
armature/skin/action construction and the anim→bone name retarget on a real rig.

### 6b. Packaging — one installable add-on + a headless test runner

Everything above is consolidated so the user installs **one** thing and validates
the cores with **one** command.

- **`Fable2AssetBrowser/source/addons/fable2_io/`** — the umbrella add-on package.
  Its `__init__.py` carries a `bl_info` dict AND loads the three existing
  single-file importers (`FableLevelImporter`, `FableMdlImporter`,
  `FableAnimImporter`) and calls each one's own `register()` — **no code
  duplication**, single source of truth per importer. Enabling it adds all three
  `File ▸ Import` entries at once. The `_import_module()` helper handles both
  layouts: importer modules bundled *inside* the package (Extensions build) or
  sitting *next to* it in `addons/` (dev/classic install). Verified headlessly with
  a mock `bpy`: all three register + unregister.
- **`fable2_io/blender_manifest.toml`** — a valid **Extension** manifest
  (`schema_version = "1.0.0"`, `type = "add-on"`, `blender_version_min = "4.2.0"`,
  `files` permission). **Format choice:** ship the manifest (matches the repo's
  5.x Blender clone, which is Extensions-era) *and* keep `bl_info` in `__init__.py`
  so the same package still installs the classic "Install from Disk" way on
  3.6–4.1. One package, both routes. The Extensions route requires the sibling
  `.py` modules to be bundled inside the package (README documents this); the
  classic route works with them alongside.
- **`run_selftests.py`** (no `bpy`) — runs every deterministic-core self-test (MDL
  synthetic parse + math, the AnimBank codec `--selftest`, and the bridge anim
  fallback) and prints a pass/fail summary. **Confirmed green: `3/3 passed`,
  exit 0.** This is the "validate before opening Blender" gate.
- **`README.md`** — per-importer descriptions, both install routes, and the full
  step-by-step **Manual Test Plan** (export MDL/anim/level from the AssetBrowser →
  install → import each → what to verify), folding in the per-file test procedures.

Install/test story in one line for the user:

```
cd Fable2AssetBrowser/source/addons && python run_selftests.py   # expect 3/3
```

then in Blender: `Edit ▸ Preferences ▸ Add-ons ▸ Install from Disk…` → the
`fable2_io` package → enable **Fable II Asset Importers** → follow README's Manual
Test Plan.

---

## 7. Roadmap

**Phase 0 — level import (DONE):** `FableLevelImporter.py` +
`Level::ExportAsync`/`.fable`.

**Phase 1 — native single-MDL import (THIS INCREMENT, needs testing):** mesh + UVs
+ armature + skin for the common MDL cases, pure-Python, with the ctypes bridge
seam in place. Deliverables §6.2–§6.4.

**Phase 2 — libf2 DLL backs the importer:** the libf2/recomp track builds
`f2blender.{dll,so,dylib}` exporting the §5 API (reusing `ModelParser`,
`AnimDecoder`, `AnimBank`). Wire `argtypes` in `fable_libf2_bridge.py`; the addon
now imports *every* MDL variant correctly (foliage/cloth/multi-instance included),
fast. No new Blender-side format code.

**Phase 3 — animation as actions (STARTED THIS INCREMENT, pure-Python, needs
testing):** import AnimBank clips onto the armature as Blender **Actions** (fcurves
per bone rotation / root+optional translation), matching decoded tracks → pose
bones **by normalised bone name** (the AssetBrowser's `AnimRigMap.h` approach). The
pure-Python codec (`fable_anim_format.py`, §6.5) and importer (`FableAnimImporter.py`,
§6.6) now cover this directly — **no FBX intermediate needed** — because the
AnimBank codec is deterministic and was portable/verifiable (unlike the MDL
heuristics). Three ways this can be sourced, in order of preference now that the
Python codec exists:
  - **(a) pure-Python native decode (this increment):** `fable_anim_format.py`
    decodes the 4-mode Bézier codec straight from `fable2_anims.animation_{toc,data}`.
    Testable today; the reference the native path later replaces.
  - **(b) libf2 `f2_anim_*` (Phase 2/3 destination):** the native C-API in libf2
    reuses `AnimDecoder.cpp`/`AnimBank.cpp` verbatim — faster, and eliminates the
    "keep the Python port in sync with the C++" burden. **What it replaces:** the
    entire body of `fable_anim_format.py` (container walk + bit reader + the 4-mode
    decode + interpolation); `fable_libf2_bridge.decode_clip` already has the
    fallback seam so only `argtypes` wiring changes. `FableAnimImporter.py` is
    unaffected (it consumes `DecodedClip` either way).
  - **(c) via FBX (fallback / cross-check):** the AssetBrowser's
    `mdl_to_fbx_full(..., include_animations=true)` bakes compatible clips into FBX;
    useful to cross-check the Python decode against the shipping exporter, but no
    longer the primary path.

  Retarget notes: matching is **by normalised bone name** (`normalise_bone_name` +
  `shadow_` alias), which is what the AssetBrowser actually uses. The FNV-1 bone
  hash (basis `0x811C9DC5`, prime `0x01000193`; recovered `hkaSkeleton` convention
  in `ghidra_out/havok_spec.txt`) is exposed in `fable_anim_format.py` for callers
  that want to key by the game's bone hash, but is **not** required for name
  retargeting (in the C++ the FNV-1 mix is only a rig-compatibility *signature*).

**Phase 4 — round-trip EXPORT into the mod pipeline:** a Blender exporter that
writes edits back so they feed the **existing** edit-set / "Export Edits as Mod"
overlay (`assets/mods/enabled/<name>/` + `f2ab_mod.json`; memory
`fable2-level-editset-mod-export`). Two sub-tracks mirroring import:
  - **Level edits** (move/scale/add/remove props) → emit the level editor's
    edit-set records (offset-based EDIT + additions, bnk-stamp guarded). This is
    data-only and low-risk — the *near-term* export target.
  - **Mesh/skeleton/anim authoring** → needs **encoders** libf2 does not yet have
    (the MDL/AnimBank/tex writers are the "destination" formats per
    `docs/MODDING.md`). Longer horizon; export new meshes via the modern-format
    loaders the decomp will grow, not by re-encoding original MDL blindly.

**Phase 5 — packaging:** graduate the addon set to a Blender **Extension**
(`blender_manifest.toml`) that bundles the `libf2` DLL per-platform (as glTF
bundles Draco), so install is one drag-and-drop.

---

## 8. Decisions for the user

1. **Bridge strategy (recommended: libf2 .dll + ctypes).** Confirm we build
   `libf2` as a shared lib with a flat `extern "C"` API (Draco-style) and load it
   from the addon via `ctypes`, keeping the pure-Python parser as fallback —
   rather than committing to pure Python forever or forking Blender. *Recommend:
   yes, the ctypes bridge.*
2. **Who ships the DLL / which platforms.** libf2 currently targets win-amd64
   (matches the recomp toolchain). Windows-first is fine; `.so`/`.dylib` later.
   Needs an owner on the libf2/recomp side to add the `f2blender` target.
3. **Animation sourcing for Phase 3.** FBX-via-AssetBrowser (available now, extra
   intermediate) vs. native `f2_anim_*` in libf2 (no intermediate, waits on the
   DLL). *Recommend: FBX path to prove actions import, then swap to native.*
4. **Export ambition for Phase 4.** Confirm the near-term export target is **level
   edit-sets only** (data-only, feeds the shipped mod overlay), deferring
   new-mesh/anim authoring to the modern-format loaders. *Recommend: yes.*

---

## 8a. When the user returns — pick up here

The Blender track is at a deliberate pause: what's built is installable and its
deterministic cores are verified, but the next steps need **you** (decisions +
in-Blender testing with real assets). Two things to do, in order:

**(1) Run the real-data validation checklist** (needs Blender + game assets; the
full step-by-step is in `Fable2AssetBrowser/source/addons/README.md` → *Manual Test
Plan*). Summary:
  - [ ] `python run_selftests.py` → expect `3/3 passed` (do this first, no Blender).
  - [ ] Install the `fable2_io` add-on; confirm three `File ▸ Import` entries.
  - [ ] Import a **skinned MDL** → mesh + UVs + armature + weights look right
        (spot-check in Weight Paint); compare to the AssetBrowser's FBX of the same.
  - [ ] Decode against the real `fable2_anims.animation_{toc,data}` — first via
        `python fable_anim_format.py <game_data_root>` (headless: lists + decodes a
        clip), then import an **Animation** onto that armature and **scrub** — bones
        move; check console for `matched N/M tracks`.
  - [ ] Import a **level (.fable)** → terrain + prop instances place correctly.
  - [ ] Note anything wrong (screenshots) so the Blender-side fixes can be scoped —
        likely suspects: root-bone axis (the `fbx_root_quat` fixups we deliberately
        skipped), anim→bone name retarget coverage, MDL variant support.

**(2) Answer the four decisions in §8** (they gate the remaining big items):
  - [ ] **Bridge strategy** — commit to `libf2` DLL + `ctypes` with Python fallback?
        (unblocks Phase 2: native decode for every MDL/anim variant, fast)
  - [ ] **DLL ownership/platforms** — who builds `f2blender.{dll,so,dylib}` and for
        which OSes? (Windows-first matches the recomp toolchain)
  - [ ] **Animation sourcing** — the pure-Python path now exists, so the choice is
        really "keep Python vs. move decode into libf2"; FBX is now only a
        cross-check. (unblocks Phase 3 native)
  - [ ] **Export ambition** — confirm Phase 4 near-term = **level edit-sets only**
        (feeds the shipped mod overlay), deferring new-mesh/anim authoring.

Until (2) is answered and (1) surfaces the concrete Blender-side bugs, there is no
further non-blocking Blender work — the track waits here.

---

## 9. References

- Current addon: `Fable2AssetBrowser/source/addons/FableLevelImporter.py`
- Level export + `.fable` schema: `Fable2AssetBrowser/source/src/Level/LevelExport.cpp/.h`
- MDL decode (authoritative format spec): `.../src/MDL/ModelParser.cpp` + `.h`,
  `MdlExportCommon.h`, `MdlFbxExport.cpp`;
  010 template `.../Archive/010EditorScripts/mdl parser.bt`
- Animation: `.../src/animations/AnimBank.*`, `AnimDecoder.*`, `AnimDataFile.*`,
  `AnimRigMap.h`; Havok/skeleton RE `ghidra_out/havok_spec.txt`
- Blender native-lib precedent (studied from `third_party/blender/`):
  `scripts/addons_core/io_scene_gltf2/io/com/library.py` (`dll_path`),
  `.../io/exp/draco.py` (`cdll.LoadLibrary` + `restype`/`argtypes`),
  `intern/draco_bridge/CMakeLists.txt` (`add_library(... SHARED ...)`),
  `intern/draco_bridge/intern/common.h` (`API()` = `extern "C" __declspec(dllexport)`),
  `.../intern/decoder.h` (handle-based C API shape)
- Blender addon pattern: `scripts/addons_core/io_scene_fbx/__init__.py`
- Blender Python API (armatures/skinning), consulted:
  <https://docs.blender.org/api/current/bpy.types.EditBone.html>,
  <https://docs.blender.org/api/current/info_gotchas_armatures_and_bones.html>,
  <https://docs.blender.org/manual/en/latest/animation/armatures/skinning/parenting.html>
- Project direction: `docs/MODDING_ENVIRONMENT.md` (libf2/f2tool P1),
  `docs/MODDING.md`, `docs/SYSTEMS_ANALYSIS.md`
