# Native renderer session snapshot — 2026-08-02

## Resume here

The native D3D12 renderer remains the active path. Frontend/title, loading, UI, and the final
compositor present; the loaded Hero001 world remains flat blue-gray with UI/glows visible. No Fable
process or debugger remains after the last repro (`Fable2_1459.log`).

## Critical shader identity correction

Late-world `gameplay draw ... ps=` values are translator cache keys. The
`REXGPU_NATIVE_FORCE_PIXEL_COLOR_TARGET` selector compares guest microcode hashes. Exact mappings
from `Fable2_1459.log` are:

| Translator cache key | Guest microcode hash |
| --- | --- |
| `18F21758F4DC7D83` | `70E7786A87CAEFF5` |
| `62BC938CB35BCC47` | `4E0825A6D3B60F88` |
| `802C36EB2C3261DC` | `A820293DEE9C9A2D` |
| `CFCC25A84593468B` | `CCA25F8031D8EADD` |
| `2938CD4379D4F909` | `DB9F19BA0E43675E` |

The earlier `TARGET=world` and direct `TARGET=18F...` runs therefore did not force the intended
steady-world shaders. Do not use them as evidence against world coverage. The next valid magenta A/B
should target one of the guest hashes above, starting with `70E7786A87CAEFF5`.

## Evidence locked in this session

- The Xenos 1D texture resource fix is real: loading screens became fully detailed (map/tomb/text/
  tutorial) instead of malformed/flat. Keep that fix.
- Fullscreen interpolator and global forced-color probes prove coverage, RTV binding, resolve, shader
  export, and present are alive.
- Forced exposure=1, forced predicate-true, and always-pass gameplay depth do not restore the world.
- Scaling translated pixel output by 16 raises the compositor-bound HDR measurement from approximately
  `maxRGB=1.0605, meanMaxRGB=0.0811` to `maxRGB=16.97, meanMaxRGB=1.2974`. Real output exists; it is
  materially under-scaled or computed incorrectly.
- The final compositor binds live varied HDR resource `0x19C67000`, 1120x720,
  R16G16B16A16_FLOAT, with finite/nonzero texels. Exposure/LUT and compositor binding are not the
  current gate.
- `Fable2_1459.log` now includes `ps_guest=` beside late-world draws for future correlation.

## Screenshot/focus caveat

`tools/winshot_confirm.ps1` normally calls `ShowWindow`/`SetForegroundWindow` before capture. That can
change presentation/visibility and was the reason title/menu/loading screens reappeared during the
earlier investigation. It is not passive observation. Use `-NoActivate` only when Fable is already
foreground; otherwise it may capture the desktop/IDE. `artifact_repro.ps1` explicitly focuses Fable at
launch so repro runs are deterministic. Capture filenames currently use generic `win_v1_*` names;
use log timestamps/run number rather than assuming `-Tag` is encoded in the filename.

## Current diagnostic/build state

- Native DLL rebuild/stage succeeds. Diagnostic behavior remains environment-gated and default-off.
- Useful variables include `REXGPU_NATIVE_FORCE_PIXEL_COLOR`,
  `REXGPU_NATIVE_FORCE_PIXEL_COLOR_TARGET`,
  `REXGPU_NATIVE_FORCE_PIXEL_COLOR_SOLID_TARGET`,
  `REXGPU_NATIVE_COMPOSITOR_BINDING_TRACE`, and `REXGPU_NATIVE_HDR_INPUT_TRACE`.
- Diagnostic-only source additions currently log native pixel inventory and `ps_guest` mapping; keep
  them until the valid shader-target A/B is complete.

## Next run

1. Build/stage `rexgpu-native.dll`.
2. Run a solid-magenta probe against guest hash `70E7786A87CAEFF5`.
3. If geometry changes, probe the other four guest hashes individually, then run sample mode with
   solid unset to inspect the real first texture samples.
4. If none changes the world, instrument native vertex/raster/depth/resolve acceptance. Do not revisit
   exposure or screenshot activation as renderer causes without new measurements.
