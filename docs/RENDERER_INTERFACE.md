# Native world-renderer interface (environment ↔ gameplay coordination)

The world renderer (`native_world_renderer.cpp` + `native_vulkan_world_renderer.cpp`, both
backends, parity-disciplined) is owned by the **environment/rendering session**. The **gameplay
session** consumes it (feeds data, sets flags). This doc is the contract so the two sessions don't
collide in the renderer files.

## 1. Per-instance draw-distance culling — ✅ SHIPPED (commit e0aa3ba)

The renderer draws one range per instance, so distance culling is per-instance.

**Interface (gameplay sets, renderer applies):**
- `NativeInstance.max_draw_distance` (world units; **0 = never cull**, default → existing scenes
  byte-identical). Or emit it as the optional trailing token on the F2SCENE line:
  `instance <mesh> <px py pz> <rx ry rz> <scale> [max_draw_distance]`.
- The cook should set it from the entity's **DrawDistance** GDB field (grounded values from the
  gameplay session: `MaxDrawDistance=200`, `BillboardDistance=70`).

**Renderer side (done):** `make_geometry` bakes each range's world-space bounding sphere
(`center`+`radius`); both draw loops (opaque/water + shadow) skip a range when
`dist(eye, center) - radius > max_draw_distance`. Camera eye cached (`camera_eye_`) so the shadow
pass (runs before `render()`) culls consistently (a culled instance casts no shadow either).
Verified both backends: aggressive `dist=1` culls the whole town except huge-radius backdrop;
default renders the full town.

## 2. Dynamic-mesh path for hero skinning — ✅ RENDERER CHANNEL SHIPPED (48a1c11); awaits consumer

**BUILT 2026-08-16 (both backends):** `set_character_pose(mesh_index, model_positions)` on both world
renderers. The vertex buffer is upload-heap/host-visible, so each hero mesh's vertices are rewritten
IN PLACE (no separate buffer / index rebase / draw change). `make_geometry` captures each `hero*`
mesh's vertex sub-range + instance transform; `set_character_pose` writes
`place_vertex(model_pos, hero xform)` — the SAME transform as the bind bake (so bind input reproduces
the bake; `AnimationPlayer::skin()` output animates it). `character_mesh_count()` /
`character_mesh_vertex_count(i)` expose the contract. Positions-only (normals stay at bind → animated-
hero lighting is approximate for now). No-op if unused. Correct-by-construction; end-to-end animation
awaits the **consumer**: gameplay per frame does `animPlayer.update(dt); animPlayer.skin(heroBind,
positions); worldRenderer.set_character_pose(0, positions);` — which needs the **hero
SkinnedVertex/AnimClip cook** (gameplay's pipeline, via `fable_pose.py`). Ping me when it lands to wire
+ verify on screen; I can also add normal-skinning + multi-mesh once a real hero scene exists.

--- (original design, resolved + now built) ---
### (was) READY TO BUILD (anim math shipped: P5 b188043)

**UPDATE 2026-08-16:** the gameplay session shipped the skinning MATH (`native_animation.h/.cpp`,
`AnimationPlayer` + LBS, grounded in `anim_runtime_sampler_re.txt`/`anim_pose_re.txt`). It's a
STANDALONE module — it does NOT touch the world renderer. `AnimationPlayer::skin(base, out_positions)`
outputs **CPU-skinned MODEL-space positions** per frame. So the A/B below is settled: **Option A
(CPU-skinned positions)**. The renderer-display integration is the only remaining piece, and it's mine.

**Agreed interface (proposed):** world renderer method
`set_character_pose(const std::vector<std::array<float,3>>& model_positions)` — gameplay calls
`AnimationPlayer::skin(...)` each frame and passes the result; the renderer applies the character
instance's transform (rotation/scale/translation) → world and updates the character range's vertices
in a **dynamic (upload-heap) vertex buffer** (kept separate from the static world buffer; the
character range draws from it). Both backends, parity. I'll build this against the `SkinnedVertex`/
`AnimClip` contract in `native_animation.h`; ping me to wire it (or I'll build it proactively next).

--- (original design, now resolved) ---
### (was) blocked on anim-sampler RE

Runtime hero animation needs the character mesh's vertices to change per frame. The cook already
bakes a **static idle pose**; per-frame playback needs the skinned vertices each frame. The renderer
bakes all geometry into one static vertex buffer today, and the hero is an `is_character` range with
only a rigid `character_offset` + bob applied in the VS.

**Decision needed from the gameplay session (the data shape) before I build:**
- **Option A — CPU-skinned vertices (recommended for one hero):** gameplay computes the skinned
  character vertices on CPU (bone matrices from the RE'd anim sampler × the MDL skin weights) and
  pushes them each frame. Renderer provides `set_character_vertices(range_index, span<Vertex>)` and
  carves the character range into a **dynamic (upload-heap) vertex buffer** re-uploaded per frame.
  No vertex-format change; simplest; flexible. Cost: a few-thousand-vertex upload per frame (fine
  for one hero).
- **Option B — GPU skinning:** renderer skins in the VS from bone matrices. Needs the cook to add
  **bone IDs + weights** to the native `Vertex` (the 28-byte skinned MDL stride carries them,
  `world_shading_model_re.txt §1`) + a bone-matrix cbuffer/SSBO + `set_bone_matrices(span<mat4>)`.
  More renderer plumbing + a vertex-format change, but no per-frame vertex upload.

**Renderer side I'll build once the shape is chosen** (both backends, additive): the dynamic buffer
or bone-matrix upload + the setter, wired into the existing character range. **Blocked only on:**
(1) the anim-sampler RE (bone matrices — the gameplay session's research agent, `anim_havok §F.2`),
and (2) the A/B choice above. Ping me with the choice + the data shape and I'll land it.

## 3. Water fidelity — reflection RTT ✅ both backends · Fresnel ✅ both backends · refraction grab-pass ✅ D3D12 / 🚧 Vulkan

Retail water (`Shaders.sbk` shader 62 `PSHADER_WATERPATCH`) is planar reflection/refraction, not
analytic (see `docs/RENDERER_GROUNDING_AUDIT.md`). **Phase 1 (D3D12, done + verified):** the frontend
owns a `kSceneColorFormat` reflection RT + its own depth + an SRV (mirrors the shadow-map split);
`render_reflection` replays opaque geometry MIRRORED about the derived water plane
(`water_plane_y_` = radius-weighted mean of water `DrawRange` centres) via a `vs_reflect` clip-plane VS
variant (`SV_ClipDistance` drops submerged geo; cull stays NONE so no winding flip). The water PS
samples it by screen-space uv perturbed by the bump normal (data-backed `REFLECTION_SCALE`
param[25/26]). A second cbuffer slice carries the reflected VP/eye/clip-plane (avoids the single-buffer
race). Analytic-sky fallback when no target/water; `FABLE2NATIVE_NO_REFLECT` forces it.
Verified with a red-tint A/B (town reflects only on water pixels where the mirror ray hits it).
**Phase 2 (Vulkan, done + verified):** renderer-owned reflection image/render-pass/framebuffer +
single-sample pipeline (mirrors the Vulkan shadow pass); the reflected VP is `multiply(vp, R_col)`
(column-major); Camera UBO binding 0 is a **dynamic** uniform buffer with two slices (offset 0 = main,
aligned slice = reflection) so the per-material sets aren't duplicated; the clip plane is a **fragment
discard** (pushed `clip_plane` — avoids the `gl_ClipDistance` device feature, same result); water frag
samples binding 8 gated by a pushed `reflection_enabled` (Vulkan UBO has no `viewport_size`). Red-tint
A/B is pixel-consistent with D3D12. **Both backends now at parity.**
**Phase 3a (Fresnel grounding, DONE + verified — 475b5a6):** the water Fresnel is now the grounded
retail linear form `saturate(1 - dot(V,Nf) + FRESNEL_BIAS)` (`water_system_re.txt §3 step 3`,
`FRESNEL_BIAS = water_params[0].x`), replacing the Schlick `pow(1-N·V,5)` stand-in — the real reflection
RT (phase 1/2) removed the analytic-sky sparkle that forced the approximation. Both backends, clean.

**Phase 3b (refraction grab-pass) — D3D12 SHIPPED + verified (5c35ddb); Vulkan parity in flight.**
CORRECTION of an earlier wrong reversal: the town/ocean water IS **retail program 57 = shader-table
entry 65 (`PSHADER_OCEAN_WATER`)**, and its binding table declares **`g_RefractionSampler` (c14)** AND
`g_ReflectionSampler` (c13) — both real texture tiles (compilers strip unused samplers; and §5 step 1's
dual bump-map fetch proves the subagent tfetch decode that claimed "tiles are dead code" was wrong). So
retail samples an explicit refraction TILE; the native alpha-blend was a documented port *stand-in*
(`§5 step 6`), NOT ground truth. The grab-pass is therefore GROUNDED (I was wrong to decline it), and
program 57 IS the ocean water — no separate WATERPATCH(62) level needed; the existing `out_realwater*`
scenes are program 57.
- **D3D12 impl:** frontend owns a window-sized HDR-colour copy + SRV; `render()` copies the opaque HDR
  scene (scene BEHIND the water) into it between the opaque and water passes (same bound-resource copy
  as the depth copy); water PS samples it as the refraction tile (t6) by screen-space uv distorted by
  `REFRACTION_SCALE` (param[27/28] = `water_params[6].w`/`[7].x`), tinted by `water_opacity`
  (`params[9].y`), output opaque (alpha suppresses the `ONE/SRC_ALPHA` framebuffer). `FABLE2NATIVE_NO_REFRACT`
  A/B. **Verified:** pure-refraction red-tint shows the `.water` body sampling the scene-behind (island
  edge through it); grounded combine renders clean. Exact per-packet combine NOT machine-verified
  (subagent decode unreliable) — uses §5 structure + data-backed params.
- **Vulkan parity — scoped, needs a render-pass split:** Vulkan draws opaque+water in ONE render pass
  (subpasses under MSAA); you can't `vkCmdCopyImage` a colour attachment mid-pass, and subpass input
  attachments can't do the distorted (offset) sample. So the world render pass must split into
  opaque(+sky) → copy HDR colour → water (a frontend-orchestration refactor). Until then Vulkan keeps
  §5's grounded alpha-blend refraction stand-in (a fidelity gap, not a correctness gap).

## Coordination rules
- Only the environment session edits `native_world_renderer.cpp` / `native_vulkan_world_renderer.cpp`
  / `native_scene.h` renderer structs. Gameplay sets the interface fields + feeds data.
- Every renderer change is D3D12 + Vulkan in the same commit (parity is non-negotiable).
