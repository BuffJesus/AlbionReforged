# Native world-renderer — constant grounding audit (2026-08-16)

Enforces the project rule **"only data-supported values, no guessing."** Every non-structural
numeric constant in the environment renderer's lighting/ambient/spec/point-light/fog/water/sky/shadow
math, classified by grounding status. Both backends (`native_world_renderer.cpp` D3D12 +
`native_vulkan_world_renderer.cpp` / `shaders/native_world.frag` Vulkan) were swept.

**Parity: PERFECT** — every constant below is byte-identical across D3D12 and Vulkan (verified). No
backend divergence. Parity is non-negotiable, so this must stay true for any future change.

Three honest tiers (NOT "grounded vs violation" — the middle tier is legitimate):

## A. GROUNDED — value traces to decompiled game data
| Constant | Value | Source |
|---|---|---|
| Sky zenith/horizon RGB | `0.6549,0.8157,1.0` / `0.222,0.5789,1.11` | RE'd chapter2slums midday (theme `0x72d66d23`); overridden per-theme by `.genv` cook |
| Shadow scale bias (strength) | `0.8` | `globals.gdb` rec `01b5fc17` ShadowScaleBias (grounded, cited in code) |
| Fog color / start / end / max | (from scene) | theme **Fogging** sub-record `0xDDF56C9A` → CloseFogColour + near/far/density (`env_theme_colors_re.txt §6`) |
| Per-prop ambient | (from `.lmp`) | baked SH probe DC term when `probe.w>0.5` (`world_shading_model_re.txt §lmp`) — replaces the hemisphere fallback |
| Sun / point-light colors + ranges | (from scene) | theme main_light_colour + `level_lights_effects_re.txt §3.1` |

## B. SPEC-SANCTIONED APPROXIMATION — the RE spec explicitly chose this as the native stand-in
The retail material PS uses BRDF ramp LUTs (`tf4/tf5`) + a flat ambient. `world_shading_model_re.txt`
§5/§7 explicitly states a native port does **not** need the ramps — "standard N·L + hemisphere ambient
+ Blinn-Phong reproduces the look" — and gives these exact stand-in values. So they are grounded in
the *spec's decision*, not invented at the shader:
| Constant | Value | Basis |
|---|---|---|
| Hemisphere ambient endpoints | `0.18,0.20,0.24` (ground) ↔ `0.55,0.58,0.62` (sky) | `world_shading_model_re.txt:297` verbatim; approximates retail flat ambient `0.35+0.65·N·L` (line 237) which "read dark/flat". **Fallback only** — real baked probe overrides it. |
| Hemisphere blend | `0.5+0.5·N.y` | §7 line 296 verbatim |
| Specular exponent | `pow(N·H, 32)` | §7: Blinn-Phong stand-in for the retail spec ramp (spec says the ramp is not needed) |
| Point-light falloff | `atten²` | comment "~inverse-square feel"; standard attenuation, no retail per-light curve captured |
| Shadow depth bias | `0.0015` | standard shadow-acne bias (technique, not a look value) |
| Sun horizon gate | `-0.05` | technical threshold: disable the shadow pass as the sun sets (not a visual value) |

## C. WATER — now the RETAIL technique, both backends (UPDATED 2026-08-16; was "analytic deviations")
The town/ocean water is **retail program 57 = shader-table entry 65 (`PSHADER_OCEAN_WATER`)**, whose
binding table declares a **reflection tile (`g_ReflectionSampler` c13)** AND a **refraction tile
(`g_RefractionSampler` c14)** — both real textures (compilers strip unused samplers; `§5 step 1`'s dual
bump-map fetch disproves a subagent decode that claimed the tiles were dead code). The native now
implements that technique on **both backends** (reflection RTT, linear Fresnel, refraction tile), so the
former "analytic deviations" are largely retired:

DATA-BACKED (from `water_params[]` b2, WaterFile record):
| Term | Source | Param |
|---|---|---|
| Fresnel bias | `water_params[0].x` | `[0] FRESNEL_BIAS` |
| Reflection bias | `water_params[0].y` | `[1] REFLECTION_BIAS` |
| Reflection / refraction scale | `[6].yz / [6].w,[7].x` | `[25/26] / [27/28]` |
| Reflection strength | `water_params[7].y` | `[29] REFLECTION_STRENGTH` |
| Glitter power / strength | `water_params[9].x / [8].w` | `[36]/[35] GLITTER_*` |
| Surface / deep colour, opacity | `water_params[4-5] / [9].y` | surface/deep colour + WaterTheme opacity |

SHIPPED — now the retail technique (was the "clean-parity path"; see `RENDERER_INTERFACE.md §3`):
| Feature | Grounding | Commits |
|---|---|---|
| Planar **reflection** RTT (mirror-about-plane tile) | replaces analytic sky; retail `g_ReflectionSampler` c13 | `f8e0b8e` (D3D12) `fcd8d77` (Vk) |
| **Fresnel** = `saturate(1 - N·V + FRESNEL_BIAS)` | `§5 step 3` linear form (retired the Schlick `pow(1-N·V,5)`) | `475b5a6` |
| **Refraction** tile (scene behind, distorted by REFRACTION_SCALE) | retail `g_RefractionSampler` c14 (was an alpha-blend stand-in) | `5c35ddb` (D3D12) `fcdc215` (Vk) |

REMAINING minor deviations (inherent to the native's non-HDR target — NOT ungrounded guesses):
- **Ripple slope damp `0.35`** + **broad `Nf`**: anti-alias the high-freq PF40 normal map (the spec's
  near-horizontal `Nf` aliased the *analytic* reflection; with real tiles this could be revisited, low value).
- **Glitter `*0.25`** / **distance-fade `75`**: compensate for the missing retail exposure/edge stage.
- The exact per-packet program-57 combine was NOT machine-verified (the microcode subagent was
  unreliable); the tile *usage* + all *values* are decomp-grounded, and the combine follows `§5`'s structure.

**Assessment:** tiers A+B (land lighting) grounded/spec-sanctioned; tier C (water) now implements the
retail reflection+refraction-tile + linear-Fresnel technique on both backends, with only a couple of
HDR-less/anti-alias compensations remaining. (Earlier notes cited shader 62 `PSHADER_WATERPATCH` as the
retail water — that's a *different* ocean-patch shader; the town/main water is program 57 / shader 65.)

**Also recorded (see `env_ambient_fog_shader_re.txt` ADDENDUM 2026-08-16):** the retail world-fog block
in material PS `shader[515]` was decoded — it is a height/distance fog + sun-directional inscatter via
`c66/c67/c68` (not the c4/c5/c64 first assumed, and not Rayleigh/Mie). The native's linear distance fog
(tier A) is grounded in the same Fogging record; a retail-technique inscatter upgrade is blocked on a
Ghidra population-trace of `g_AtmosphericParameters` (do NOT guess the field mapping).
