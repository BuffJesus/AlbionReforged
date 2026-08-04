# Albion Reforged

An unofficial native C++23 PC port and reconstruction of **Fable II**.

Albion Reforged is building a real PC game runtime: native memory and streaming, D3D12 and Vulkan
renderers, replaceable media, modern asset packages, and an open modding surface. The original
Xbox 360 executable and ReXGlue recompilation remain behavioral oracles for reverse engineering
and parity testing; they are not shipping dependencies of the native runtime.

## Important: no game assets are included

This repository contains no ISO, XEX, BNK, model, texture, video, or other copyrighted game data.
Users provide their own legally obtained game backup during installation. The installer extracts
Xbox 360 XDVDFS data into a user-selected directory, validates it, and the offline cookers create
native packages from that user-owned source.

## Current vertical slice

The native executables currently connect:

`source validation -> boot -> intro sequence contract -> title -> main menu -> loading -> native world`

The D3D12 and Vulkan world renderers accept the same user-cooked `F2SCENE` package through
`--scene`, can consume a user-owned DXT1/DXT5 DDS through `--texture`, and have an asset-free
procedural fallback for bring-up.

## Build

```powershell
cmake -S Fable2Native -B Fable2Native/build -DBUILD_TESTING=ON
cmake --build Fable2Native/build --config RelWithDebInfo
ctest --test-dir Fable2Native/build -C RelWithDebInfo --output-on-failure
```

With the Vulkan SDK installed, configure the additional frontend with
`-DF2NATIVE_ENABLE_VULKAN=ON`. This produces `f2native_frontend_vulkan.exe` beside the D3D12
frontend.

## Install user-owned game data

For an extracted game directory, launch the frontend and select it on first run, or pass it
explicitly:

```powershell
Fable2Native\build\RelWithDebInfo\f2native_frontend.exe `
  --game-dir path\to\extracted\Fable2
```

For an Xbox 360 ISO backup:

```powershell
Fable2Native\build\RelWithDebInfo\f2native_installer.exe `
  --iso path\to\Fable2.iso `
  --out path\to\AlbionReforgedGame
```

The installer extracts the XDVDFS game partition, skips `$SystemUpdate`, and validates the
result. It does not download or provide game data.

For the current native-world handoff:

```powershell
Fable2Native\build\RelWithDebInfo\f2native_frontend.exe `
  --game-dir path\to\AlbionReforgedGame `
  --scene cooked\model.f2scene
```

## Repository layout

| Path | Purpose |
|---|---|
| `Fable2Native/` | Standalone C++23 runtime, D3D12 frontend, installer, tests, and cookers |
| `docs/` | Roadmap, reverse-engineering findings, native-port design, and modding plans |
| `Fable2Native/examples/` | Small asset-free package examples for tests and bring-up |
| `.github/workflows/` | Build and test automation |

Large research checkouts and local oracle builds are intentionally kept outside the public source
boundary. See [CONTRIBUTING.md](CONTRIBUTING.md) and [docs/NATIVE_PORT_PLAN.md](docs/NATIVE_PORT_PLAN.md).

## Project direction

The end state is a faithful, moddable PC game—not a 360 compatibility layer running on Windows.
New runtime systems target readable C++23 and D3D12 first. Legacy BNK/MDL/TEX/EHF/GDB/Lua formats
are cooker inputs; stable native IDs, packages, scripts, events, menus, and saves are the public
modding contract.
