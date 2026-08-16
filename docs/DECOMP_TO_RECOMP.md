# Decomp → Recomp leverage map

**Verdict (2026-08-16): yes — the decomp/native work concretely and decisively helps the recomp,
and the leverage is already wired into the codebase rather than aspirational.** This doc consolidates
what the decompilation (`ghidra_out/`) and the standalone **Fable2Native** renderer give the
**Fable2Recomp** effort, per-frontier, as *decomp-asset → recomp-action → discriminating-test* with
exact `file:line` cites. Produced by a verified multi-agent pass (5 mappers → per-frontier
leverage → adversarial verify → synth); every lever below is CONFIRMED or PLAUSIBLE against current
source.

**The shape of the help.** The recomp runs the real TU1 PPC game translated to C++; its native GPU
backend (`rexglue-src/src/graphics/native/*`) is a Xenos PM4 → DXBC/SPIR-V translator that runs the
game's *own* shaders. Fable2Native is a *separate* app that renders the same cooked world from
hand-written shaders driven by decomp specs. So the two do **not** share code — the decomp/native
value is (a) a **behavioral oracle** (native renders chapter2slums clearly non-black, so a black
recomp export is a *defect*, not a legit dark material), and (b) the decomp'd algorithms that tell
the recomp what the correct behavior is. The oracle is **qualitative** (a non-black target), not a
numeric per-draw diff — native and the recomp do not share `material_index→SRV` mapping or
per-vertex `base_color`, so a pixel-exact compare would need the `DUMP_PS_KEY` path extended to dump
bound texels (not currently wired).

---

## Frontier 1 — black loaded world (`--gpu_plugin native`) · **highest leverage**

The loaded 3D world renders black. Dominant material PS `translator_cache_key 98B4F32B94897A9C`
exports a color register that zero-inits and is written **only** inside `p0`-predicated blocks; the
long-standing suspect is the Xenos→DXBC predicate translation leaving the export unwritten.

**Ground-truth correction from the decomp map:** the DXBC translator is **structurally correct**.
Predication is emitted as real `OpIf`/`OpEndIf` control flow on `p0 = system_temp_ps_pc_p0_a0_.z`
(`dxbc_translator.cpp:1646-1703`); `p0` zero-inits (`:948`); every `setp_*` writes the same `.z`
lane (`dxbc_translator_alu.cpp:815-867` scalar, `369-447` vector); `kill_gt` is a proper
`OpIf`+`OpDiscard` (`_alu.cpp:23-49`), **not** color-zeroing. So this is **not** a broken if-block
emission — the frontier is **WHY `p0` evaluates false at runtime** (predicate value / setp inputs /
constant sources), or a *dropped* setp, not a lane-mismatch. `world_shading_model_re.txt:204-206`
confirms this `r8=0` world bug is **orthogonal** to the black-skin composite (Frontier 2).

Every knob below is **env-gated and ships in-tree — zero source edits to run.**

| Lever | Decomp asset | Recomp action | Discriminating test |
|---|---|---|---|
| **force-predicate-true A/B** (CONFIRMED) | `GetPredicateSource()→LF(1.0f)` when `force_predicate_true_` (`dxbc_translator.cpp:1653-1656`); env `REXGPU_NATIVE_FORCE_PREDICATE_TRUE` (`d3d12_pm4_backend.cpp:1868-1872`, log `:1917`) | none — run flag off vs on | Same save+level, `=0` vs `=1`, screenshot; confirm log `:1917` fires. `=1` lights world ⇒ predicate value is the gate; `=1` still black ⇒ hypothesis **refuted** |
| **static DXBC dump** (CONFIRMED) | `REXGPU_NATIVE_COMPOSITOR_DUMP_PATH` + `REXGPU_NATIVE_DUMP_PS_KEY=98B4F32B94897A9C` → `translation->Dump(path,"dxbc")` (`d3d12_pm4_backend.cpp:2318-2342`) | none — inspect emitted `*.dxbc_disasm` | Grep the `o0`-source temp: any write **outside** an `if`-block ⇒ black is a constant/texture value, not predication. **Decides before any rebuild** |
| **force-pixel-color** (CONFIRMED) | unconditional `OpMov(color[0], 0.25)` at top of `CompletePixelShader()` (`dxbc_translator_om.cpp:2635-2640`); env `REXGPU_NATIVE_FORCE_PIXEL_COLOR` | none | 3-arm: baseline / `FORCE_PIXEL_COLOR=1` / `FORCE_PREDICATE_TRUE=1`. Gray world under `FORCE_PIXEL_COLOR` but black under `FORCE_PREDICATE_TRUE` ⇒ zero is **downstream** (export / `kColorExpBias` `om.cpp:1548`), not predication |
| **dropped-setp audit** (CONFIRMED) | lane consistency already holds in-source (setp writes `.z`, GetPredicateSource reads `.z`) | inspect `cf_exec_predicate_written_` skip (`dxbc_translator.cpp:1695-1698`) | In the dumped disasm, confirm a `setp` writes the `p0` `.z` lane **before** the `if(p0)` color-write guard — hunts a *dropped/reordered* setp, not a wrong-lane bug |
| **native oracle** (PLAUSIBLE, qualitative) | Fable2Native `ps_main` (`native_world_renderer.cpp:576-646`) renders chapter2slums non-black | — | Sanity target for the A/B result only; a numeric per-draw diff needs `DUMP_PS_KEY` extended to dump bound texels + `base_color` (**not wired**) |

**#1 recommended action:** the force-predicate-true A/B, run alongside the static DXBC dump in one
session. Both are zero-edit and decide the hypothesis before any rebuild.

---

## Frontier 2 — adult hero/dog black skin (guest morph-composite path)

Two conflicting framings must be disambiguated by **measurement**, not eyeball:
- **(A)** guest-side upload failure (converterId gate `0x82B94DE0`, or dispatch no-op branch in
  `0x82B8D170` leaving source textures garbage), vs
- **(B)** guest composite is correct (dumped composite RT `rt_0C0EB000` is flesh-toned) and the
  residual bug is in the GPU-plugin composite resolve (R/B swap or gamma).

`hero_appearance_morph.txt` §E currently asserts the composite **resolves correctly** and the bug is
**downstream** — which favors (B) and makes the byte-order test the decisive first step.

| Lever | Decomp asset | Recomp action | Discriminating test |
|---|---|---|---|
| **RT-dump byte-order compare** (CONFIRMED, decisive first test) | guest tap `command_processor.cpp:3261-3283` (`$FABLE2_RT_DUMP/rt_<addr>_<len>_fmt<n>.raw`); `rt_0C0EB000` flesh-toned per `hero_appearance_morph.txt:241,256-266` | none first — `FABLE2_RT_DUMP=<dir>` | Reshape `rt_0C0EB000_*.raw` as ARGB 512×512 vs native `ps_main`. Flesh bytes sampled blue/desaturated ⇒ framing **B** (d3d12 resolve channel/gamma); head-zero/black ⇒ framing **A** → dispatch A/B |
| **dispatch A/B** (CONFIRMED) | faithful `0x82B8D170` reimpl (`MorphFillTrace.cpp:710`); dispatch per `morph_upload_tiling_re.txt:29-30` | `FABLE2_COMPLETIONDIAG=1` censuses suspects; `FABLE2_MORPHUPLOAD_FIX=1` forces `flags=(flags&~0x3)\|0x10` to `82B8CDF8` (`:765`) | Attribute a positive result by which log fires — `FORCE#` (dispatch) vs `RETRY#` (converterId, `:652`) |
| **DXN(0x28) subCode2 all-zero** (PLAUSIBLE, secondary/additive) | `hero_appearance_morph.txt:256-258,284` — fur/normal DXN observed decoding all-zero (single run) | filter `82B94DE0` hook `DECODE#` log for `subCode=2 w>=128` (`MorphFillTrace.cpp:639-647`) | DXT1 nonzero but subCode2 zero with correct conv `0x1A200154` ⇒ separate decoder-jumptable fix (`82B95110[2]`, **not** reimplemented in src) |
| **POOLBOOST guardrail** (CONFIRMED) | `AtlasFillDiag.cpp` default-ON bucket 16→64 (`:94-101`); `BLACKSKIN_RE.md:416-429` | use counters `c_bucketAllocFail`/ALL-ZERO/ZERO-CONTENT (`:147-150`) as the pass/fail oracle | Don't ship a default-ON content fix that regresses these counters with POOLBOOST on |

**Dead ends (do not revisit):** the wrong-offset *placement* framing was refuted; `FABLE2_SLOT_HEAL`
default-ON was a user-confirmed fully-black regression (reverted — must stay opt-in).

---

## Frontier 3 — heap / VA-layout during world load · **decomp-SOLVED boot blocker**

The historic world-load FATAL was **not** heap exhaustion. It was a guest-CRT bug: `_output_l`
returned a buffer **pointer** as a char count → `hkString` grow ~1.06GB (`0x42740000`) → `BaseHeap`
page-count-too-big. **Fixed** in `Fable2Recomp/src/WorldLoadAllocTrace.cpp` (`rex__vsnprintf_0` clamp
`:36-42` + Havok error-dispatch skip `:48-56`). Remaining help = confirm the fix holds and treat the
512MB physical wall as a later capacity/quality lift.

| Lever | Decomp asset | Recomp action | Discriminating test |
|---|---|---|---|
| **world-load FATAL solved** (CONFIRMED) | `WorldLoadAllocTrace.cpp` clamp/skip; `AllocRange` page-count-too-big at `xmemory.cpp:1234` | keep the override; same-class hunt via the `NtAllocateVirtualMemory` diag block (≥`0x10000000`, caller_lr + back-chain) | ⚠ true A/B = **remove the whole `rex__vsnprintf_0` override**, not just comment the clamp (the clamp fired 0×/run; the fix works by reshaping `_output_l`'s nondeterministic return) |
| **512MB physical is the only real wall** (CONFIRMED) | `TranslatePhysical &0x1FFFFFFF` 1:1 (`xmemory.h:365`); `MmQueryStatistics` count cosmetic (`xboxkrnl_memory.cpp:512`); texture pool in E0 alias (`TexturePool_Init 0x82B60910`) | reroute via shipped-but-unwired `HostTextureHeap` (`host_texture_heap.h`) by weak-overriding `Function_82B3D6D0` | `FABLE2_MEMDIAG=1` peak physical vs v40; failure clears only when actual physical HEAP backing grows, not when the reported count is raised |
| **reserve-conflict probes are benign** (CONFIRMED) | power-of-two-throttled placement search (`xmemory.cpp:1147-1159`); `NATIVE_PC_MEMORY.md` ~178 (3,856 probes, no OOM) | do **not** add retry/relocation for probes | A passing run still emits thousands; real failures = un-throttled host-alloc-failed (`xmemory.cpp:1182`) |
| **E0 pre-commit required** (CONFIRMED) | `heap.cpp:168-175` pre-commits the `0xE0000000` view (job workers write GPU cmd buffers into uncommitted E0 pages) | template for residual AVs: check the faulting addr is an A0/C0/E0 alias in Reserve-not-Commit | A/B `heap.cpp:168-175` present vs removed; removed build takes an intermittent AV writing into E0 (`~0xff4f6xxx`) |
| **page-size mis-route** (PLAUSIBLE, unobserved) | 64KB→512MB physical, 4KB→~2GB virtual (`xboxkrnl_memory.cpp:172-173`); `decomp_membudget_session.txt:93-96` | add `(size, page_size, chosen-heap)` log for allocs ≥16MB at `NtAllocateVirtualMemory_entry` | only meaningful once a mis-routed alloc is actually observed — none on record |

---

## Ranked next actions (for the recomp)

1. **Black-world force-predicate-true A/B** using native as the qualitative oracle
   (`REXGPU_NATIVE_FORCE_PREDICATE_TRUE=0` vs `1`, `--gpu_plugin native`, screenshot; confirm log
   `d3d12_pm4_backend.cpp:1917`). Zero source edits.
2. **Black-world static DXBC dump** (`REXGPU_NATIVE_DUMP_PS_KEY=98B4F32B94897A9C`) — decides the
   hypothesis before any rebuild. Run in the same session as #1.
3. **Black-world 3-arm disambiguation** (baseline / `FORCE_PIXEL_COLOR` / `FORCE_PREDICATE_TRUE`) —
   separates "export never written" from "export written but zeroed downstream".
4. **Black-skin RT-dump byte-order test** (`FABLE2_RT_DUMP`) — decides framing A vs B.
5. **Heap/VA** — keep the `WorldLoadAllocTrace.cpp` fix; hunt same-class bugs via the
   `NtAllocateVirtualMemory` diag block; treat 512MB physical as a later `HostTextureHeap` lift.
6. *(enabler)* extend the `DUMP_PS_KEY` path (`d3d12_pm4_backend.cpp:2322-2342`) to also dump bound
   albedo/normal texels + interpolated `base_color`, so native's closed shading formula can be
   evaluated offline for a **true pixel-exact** recomp-vs-native diff (currently **not** wired).
