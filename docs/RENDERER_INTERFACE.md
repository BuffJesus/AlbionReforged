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

## 3. Planar reflection RTT water — ✅ BOTH BACKENDS SHIPPED (D3D12 f8e0b8e, Vulkan fcd8d77)

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
**Phase 3 (optional, NOT started) = refraction RT + full `saturate(fresnel_bias - N·V)` grounding.**

## Coordination rules
- Only the environment session edits `native_world_renderer.cpp` / `native_vulkan_world_renderer.cpp`
  / `native_scene.h` renderer structs. Gameplay sets the interface fields + feeds data.
- Every renderer change is D3D12 + Vulkan in the same commit (parity is non-negotiable).
