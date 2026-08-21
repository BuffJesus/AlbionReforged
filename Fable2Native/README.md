# Fable2Native

This is the standalone PC-native runtime. It intentionally does not link ReXGlue,
Xenia, generated PPC code, or any Xbox compatibility layer.

The initial implementation establishes the native scene/data boundary. Legacy BNK,
MDL, TEX, EHF, GDB, and Lua readers will be used by an offline cooker and will emit
this runtime's native package format. They are not runtime dependencies.

Build the current core probe from this directory:

```powershell
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config RelWithDebInfo
ctest --test-dir build -C RelWithDebInfo --output-on-failure
```

The current probe accepts a text scene package:

```text
F2SCENE 1
mesh terrain 3 3
vertex 0 0 0 0 1 0 0 0
vertex 1 0 0 0 1 0 1 0
vertex 0 0 1 0 1 0 0 1
index 0 1 2
instance terrain 0 0 0 0 0 0 1
```

Cooked materials may append explicit texture references:

```text
material stone 0.7 0.7 0.7 1 albedo=pubgames/common/bar_focus.dds
```

The paths resolve against the selected source's `data` directory at runtime.

This is deliberately small and temporary. The next cooker milestone replaces the
text transport with a versioned binary package produced from real Fable II data.

For a real exported/glued MDL, the first offline bridge is available now:

```powershell
python tools\cook_mdl.py path\to\model.mdl cooked\model.f2scene
build\RelWithDebInfo\f2native_probe.exe cooked\model.f2scene
```

This first bridge cooks geometry, UVs, and the MDL's diffuse/normal/specular texture
references. Texture decoding currently covers the local DXT1/DXT5 DDS set; level placement,
terrain, animation, and streaming are the next data layer rather than 360 GPU or memory
emulation.

On Windows, launch the standalone native D3D12 frontend:

```powershell
build\RelWithDebInfo\f2native_frontend.exe
```

Use `--skip-intro` for the title screen or add `--start-menu` to begin directly at the main menu
while working on front-end art and input. Use `--controller-prompts` to preview the captured
Xbox-style A prompt without a connected pad. `--ui-only` runs the visual front-end harness
without a full extracted game tree; it is for UI iteration only and does not enable gameplay.
The D3D12 frontend's title and main-menu image layers use the native UI quad renderer. ImGui
remains temporarily for glyphs and input hit regions while the native text atlas is completed.

There is now a **single** frontend executable that hosts both presentation backends. The active
backend is selected at launch (restart-applied) — most robustly by a `--backend` flag, otherwise by
the persisted Options setting (`%LOCALAPPDATA%\Fable2Native\renderer.txt`), falling back to D3D12:

```powershell
cmake -S . -B build -DF2NATIVE_ENABLE_VULKAN=ON   # Vulkan needs the Vulkan SDK + glslc
cmake --build build --config RelWithDebInfo
build\RelWithDebInfo\f2native_frontend.exe --backend d3d12    # or: --backend vulkan
```

If the build has no Vulkan support, the selection always resolves to D3D12. The Options page exposes
the same choice (Video → Renderer); it persists and applies on the next launch. D3D12 and Vulkan
builds install their generated SPIR-V under `vulkan_shaders/` next to the frontend executable.
are presentation backends for the native runtime; neither runs the Xbox 360 renderer.
The frontend state and gameplay clock use fixed 60 Hz simulation steps, so uncapped rendering does
not accelerate menus, loading, or future game logic.

When inspecting a cooked scene in `World`, the native camera supports free flight on both
backends: `WASD` moves forward/back/left/right, `Q`/`E` move down/up, arrow keys look, and `Shift`
boosts movement. The camera is framed automatically when entering World and resets when leaving it.
Hero-enabled scenes also expose a lightweight inspection controller on both backends: `IJKL` moves
hero draw ranges in the ground plane, `U`/`O` adjust height, and `Shift` boosts. The offset resets
when leaving World or loading a scene without `hero*` meshes; this is a render/debug milestone, not
runtime animation or collision physics.
Hero-enabled scenes emit a `hero_start` directive, so World automatically frames the child at
PlayerStart and shows the live offset/control overlay in the upper-left corner. Press `R` to reset
the hero offset and `F` to reframe the inspection camera. While a movement key is held, the baked
idle mesh receives a small synchronized locomotion bob and the overlay changes from `IDLE` to
`WALK`; skeletal runtime animation remains future work.

The frontend requires user-supplied game data. On first launch it opens a folder picker;
choose the extracted Fable II game directory containing `data\dir.manifest`. The selected
source is remembered under the user's local application data directory and can be overridden
with `--game-dir path\to\extracted\game`. For an ISO backup, run the installer:

```powershell
build\RelWithDebInfo\f2native_installer.exe --iso path\to\game.iso --out path\to\Fable2NativeGame
```

The installer extracts the Xbox 360 XDVDFS filesystem, skips `$SystemUpdate`, validates the
resulting Fable II source, and writes only to the destination selected by the user. No ISO or
game assets are shipped by this project.

For the current vertical-slice world handoff, pass a user-cooked scene package:

```powershell
build\RelWithDebInfo\f2native_frontend.exe --game-dir path\to\Fable2NativeGame --scene cooked\model.f2scene
```

The same scene can be viewed through Vulkan:

```powershell
build\RelWithDebInfo\f2native_frontend.exe --backend vulkan --game-dir path\to\Fable2NativeGame --scene cooked\model.f2scene
```

For a first local-art validation, an extracted DDS can be supplied explicitly:

```powershell
build\RelWithDebInfo\f2native_frontend.exe --backend vulkan `
  --game-dir path\to\Fable2NativeGame `
  --scene cooked\model.f2scene `
  --texture path\to\data\pubgames\common\bar_focus.dds
```

The native texture bridge currently decodes the legacy DXT1 and DXT5 files found in the local
Fable II data set into RGBA8 before uploading them to D3D12 or Vulkan. Each material draw range
binds its own albedo descriptor; missing or unsupported textures use a white fallback. Original
DDS files remain cooker/runtime inputs and are never copied into the repository.

Selecting New Game reaches a real native geometry pass on either backend. Without `--scene`, the
app uses an asset-free procedural fallback so the frontend-to-world transition remains testable.

Cook the extracted startup movies into a native package with FFmpeg:

```powershell
python tools\cook_videos.py ..\Fable2Recomp\assets\game\data\art\videos cooked
```

This produces `cooked\videos\manifest.json` plus modern MP4 assets. The runtime video
player owns sequencing and skip behavior. Both frontends now decode the MP4 frames through
Media Foundation and upload real RGBA pixels to their native presentation backend. Pass the
cooker directory to either frontend:

```powershell
build\RelWithDebInfo\f2native_frontend.exe `
  --game-dir path\to\Fable2NativeGame `
  --video-root cooked

build\RelWithDebInfo\f2native_frontend.exe --backend vulkan `
  --game-dir path\to\Fable2NativeGame `
  --video-root cooked
```

The decoder probe can validate one local movie without opening a frontend:

```powershell
build\RelWithDebInfo\f2native_video_probe.exe cooked\videos\microsoft_logo.mp4
```

The original Bink files and the cooked MP4 files remain user-local runtime/cooker inputs only.

The frontends use D3D12 or Vulkan and Dear ImGui for the native UI bring-up. They have no Xenos,
PM4, EDRAM, guest address space, or ReXGlue dependency. The final art/UI renderer will consume
cooked Fable assets through the same backend-neutral native scene layer.

The title and main-menu layer accepts keyboard, mouse, and XInput controller input. Its default
command set follows the captured Fable II front-end: New Game, Continue, Language, and Subtitles.
Prompt labels follow the last device used, and keyboard labels follow
`%LOCALAPPDATA%\Fable2Native\bindings.ini`:

```ini
accept=Enter
back=Escape
up=Up
down=Down
skip=Space
```

Real front-end art is a user-local runtime input. Export/cook the desired Fable II textures into a
directory with this `ui_manifest.ini` (the repository ships no art). Windows accepts DDS, PNG,
and BMP files:

```ini
title_background=title_background.dds
main_background=main_background.dds
logo=logo.dds
button_accept=button_accept.dds
button_back=button_back.dds
ambient_atlas=ambient_atlas.png
ambient_baseline=ambient_baseline.png
ambient_detail=ambient_detail.png
# Or use a deterministic comma-separated detail sequence:
# ambient_detail_frames=ambient_detail_000.png,ambient_detail_001.png
title_font=title_font.ttf
```

`ambient_atlas` is optional. When supplied, the title uses the recovered
source-alpha three-slice geometry and timing from the live PM4 evidence; when
absent, it keeps the procedural fallback. `ambient_detail` is an optional
same-sized user export of the captured detail sampler; when present, the
runtime applies the recovered `detail.rgb + detail.a * main.rgb` material
equation to the atlas. The retail animation streams changing detail textures,
so a single supplied detail snapshot is static-frame parity. For animation,
`ambient_detail_frames` accepts a comma-separated sequence and advances it at
the measured 60 Hz cadence after the title reveal boundary. `ambient_baseline` is also optional and may be a user-owned transparent composite of the 18 static
format-18 draws; it is shown from the measured 5.90-second boundary.
`title_font` optionally points to a user-converted TTF/OTF export of the
Maiandra GD Fable font and becomes the native UI default when valid.

Launch either frontend with `--ui-root path\to\cooked-ui`. Missing UI files use the procedural
bring-up layout, so the runtime remains testable before the full front-end cooker is complete.
