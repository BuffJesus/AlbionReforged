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
while working on front-end art and input.

The Vulkan frontend is available when the Vulkan SDK and `glslc` are installed:

```powershell
cmake -S . -B build -DF2NATIVE_ENABLE_VULKAN=ON
cmake --build build --config RelWithDebInfo
build\RelWithDebInfo\f2native_frontend_vulkan.exe
```

Both frontends use the same native scene package and user-selected game source. D3D12 and Vulkan
are presentation backends for the native runtime; neither backend runs the Xbox 360 renderer.
The frontend state and gameplay clock use fixed 60 Hz simulation steps, so uncapped rendering does
not accelerate menus, loading, or future game logic.

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
build\RelWithDebInfo\f2native_frontend_vulkan.exe --game-dir path\to\Fable2NativeGame --scene cooked\model.f2scene
```

For a first local-art validation, an extracted DDS can be supplied explicitly:

```powershell
build\RelWithDebInfo\f2native_frontend_vulkan.exe `
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

build\RelWithDebInfo\f2native_frontend_vulkan.exe `
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

The title and main-menu layer accepts keyboard, mouse, and XInput controller input. Prompt labels
follow the last device used, and keyboard labels follow `%LOCALAPPDATA%\Fable2Native\bindings.ini`:

```ini
accept=Enter
back=Escape
up=Up
down=Down
skip=Space
```

Real front-end art is a user-local runtime input. Export/cook the desired Fable II DDS textures
into a directory with this `ui_manifest.ini` (the repository ships no art):

```ini
title_background=title_background.dds
main_background=main_background.dds
logo=logo.dds
button_accept=button_accept.dds
button_back=button_back.dds
```

Launch either frontend with `--ui-root path\to\cooked-ui`. Missing UI files use the procedural
bring-up layout, so the runtime remains testable before the full front-end cooker is complete.
