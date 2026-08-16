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

## C. WATER — mostly data-backed; the rest are DELIBERATE coupled deviations (CORRECTED 2026-08-16)
Re-reading `ps_water` against `water_system_re.txt §3` (the 37-param `WaterFile::params` table) shows
the water is **far more grounded than a keyword sweep suggests**. The sweep's "authored 0.5 blend" was a
false positive (line 734 `0.5` is a hemisphere remap `y*0.5+0.5`, not a blend weight — the real blend is
`refl_strength = water_params[7].y` = **param[29] REFLECTION_STRENGTH=0.75, data-backed**).

DATA-BACKED (from `water_params[]` b2, WaterFile record):
| Term | Source | Param |
|---|---|---|
| Fresnel bias | `water_params[0].x` | `[0] FRESNEL_BIAS` |
| Reflection bias | `water_params[0].y` | `[1] REFLECTION_BIAS` |
| Bump UV scale 0/1 | `water_params[1].zw / [2].xy` | `[6-9] NM_SCALE` |
| Reflection strength | `water_params[7].y` | `[29] REFLECTION_STRENGTH` |
| Glitter power / strength | `water_params[9].x / [8].w` | `[36]/[35] GLITTER_*` |
| Surface / deep colour | `water_params[4-5]` | surface/deep colour |

DELIBERATE DEVIATIONS (documented in-code, mutually coupled — do NOT "ground" piecemeal):
| Constant | Value | Why it deviates |
|---|---|---|
| Fresnel curve | Schlick `pow(1-N·V,5)` | spec's retail linear `saturate(1-N·V+bias)` assumes the near-horizontal `Nf` (NORMAL_SCALE=0.05); the native uses a broad `Nf` instead |
| `Nf` up-scale | `1.0` (broad) not `0.05` | the spec's near-horizontal `Nf` + high-freq PF40 normal map = white-noise sparkle; broad `Nf` fixes it (comment lines 722-725) |
| Ripple slope damp | `0.35` | same anti-noise damping of the summed bump normals |
| Night fade / desat | `0.05,0.45 / 0.22, (.010,.018,.050)` | matches the atmosphere sky pass's night fade so water reflects the real night sky |
| Distance fade / shoreline | `75.0 / 256.0` | HDR-less target compensations (no retail exposure/depth-edge stage in this pass) |
| Glitter attenuation | `*0.25` | HDR-less: retail applies exposure the native pass lacks |

These are a **coherent anti-noise / HDR-less approximation**, not sloppy guesses. Porting the retail
fresnel formula alone would reintroduce the sparkle noise the broad-`Nf` choice removes (they are coupled).
The only clean-parity path is the full planar-RTT water (below), which supplies a real reflection/refraction
signal so the near-horizontal `Nf` no longer aliases.

**Assessment:** tiers A+B (all the *land* lighting) are grounded or spec-sanctioned — compliant. Tier C
is confined to the **procedural water surface** shaping.

**Water technique divergence (RESOLVED by disasm 2026-08-16, `Shaders.sbk` shader 62 `PSHADER_WATERPATCH`):**
retail water is **planar RTT reflection/refraction**, NOT analytic. Its PS:
- samples a **reflection buffer** (`g_ReflectionSampler` c13) + **refraction buffer** (`g_RefractionSampler`
  c14) — each needs its own scene pass (mirrored / behind-surface).
- `[30] fresnel = saturate(g_FresnelBias(c77) - N·V_term)` — a **bias**, not a `pow(x,k)` exponent.
- `[31] o0 = lerp(refraction, reflection, fresnel)`.
- Data-backed material params (from the WaterFile record, seen in the bank): `m_FresnelBias`,
  `m_ReflectionScale/Strength`, `m_RefractionScale`, `m_SpecularReflectionFactor`,
  `m_SurfaceWaterColour`, `m_DeepWaterColour`, `m_MaxRefractDistanceFactor`, `m_ReflectionBias`.

Our native water is **analytic** (sky-color reflection + procedural fresnel + sun glitter) — a deliberate
simplification that avoids the two extra RTT passes. Consequently the tier-C constants (`pow(fresnel,5.0)`,
ripple `0.35`, etc.) have **no retail scalar equivalent** — retail's fresnel is a bias + RTT lerp, a
different model. So they are NOT ungrounded guesses passed off as data; they are the analytic stand-in's
shaping, and grounding them 1:1 is not possible without switching techniques.

**The data-backed path to real parity** (opt-in, a real renderer feature, NOT started): implement planar
reflection/refraction RTT (two extra scene passes + the two buffers) and drive it from the WaterFile
`m_*` params above via `WaterConstants` (b2). Until then the analytic water stays as the documented
simplification. Recorded so no session mistakes tier C for grounded values OR for sloppy guessing.

**Also recorded (see `env_ambient_fog_shader_re.txt` ADDENDUM 2026-08-16):** the retail world-fog block
in material PS `shader[515]` was decoded — it is a height/distance fog + sun-directional inscatter via
`c66/c67/c68` (not the c4/c5/c64 first assumed, and not Rayleigh/Mie). The native's linear distance fog
(tier A) is grounded in the same Fogging record; a retail-technique inscatter upgrade is blocked on a
Ghidra population-trace of `g_AtmosphericParameters` (do NOT guess the field mapping).
