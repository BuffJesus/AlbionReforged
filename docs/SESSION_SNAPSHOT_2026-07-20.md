# Session snapshot — 2026-07-20 (the artifact-hunt marathon)

## ▶▶▶ LATEST STATE (2026-07-20 very late — RESUME HERE for black-skin)
**The black-skin bug is now COLOR/FORMAT + TEMPORAL, not black/missing.** Under GPU readback the
hero skin AND the dog render WITH FULL DETAIL but the WRONG COLOR — pale BLUISH-GREY/desaturated
(blue-shifted), flickering/glowing across frames. The composite guest data is CORRECT (dumped
`rt_0C0EB000_100000` = natural flesh face in ARGB byte order) — so on-screen blue = an **R/B channel
swap (or gamma/format mismatch)** between the composite RESOLVE and the model SAMPLE. That's the
concrete fixable lead. readback modes: none=black, fast=glowing-white(delayed), **FULL=detail+bluish
(synchronous)=closest**. The full CPU pipeline (deliver→decode→tile) is PROVEN WORKING this session;
the bug is purely the GPU composite resolve color-space. ⚠ The USER does NOT launch the game — Claude
launches it under `renderdoccmd capture` and the user views the captured PNG thumbnails. No shipped
visual fix yet; all changes env-gated OFF/reverted. Full detail: memory `fable2-black-skin-renderdoc`
(top RESUME block). Reusable: FABLE2_RT_DUMP + scratchpad/rtdump untile script + headless renderdoccmd.
NEXT: fix the R/B-swap/gamma on the composite resolve; verify with `renderdoccmd thumb`.

### Uncommitted disk state (no git here) — files modified this late session
- `Fable2Recomp/src/MorphFillTrace.cpp` — P2 weak-override of `0x82B8D170` (REFUTED, env off), decoder
  `0x82B94DE0` dst-head probe (proved decode WORKS), tiler `0x8300CB48` probe (proved tile→GPU mem works).
- `Fable2Recomp/src/MorphFillPump.cpp` — SLOT_HEAL reverted to default-OFF (its default-on = fully-black
  regression); consume census now logs `+10vtbl` (proved wrap+0x10 is NOT a decompress job → workflow
  fix refuted); `SafeG32`/`SafeHexDump` (SEH-guarded, fixed a diag AV).
- `rexglue-src/src/graphics/native/pm4_parser.{h,cpp}` — PM4 P0 shadow parser, complete (UNKNOWN=0).
- `rexglue-src/src/graphics/command_processor.cpp` — PM4 tap in `ExecutePrimaryBuffer` + `Pm4IbResolve`.
- `rexglue-src/src/graphics/CMakeLists.txt` — added `native/pm4_parser.cpp`.
- `rexglue-src/src/graphics/d3d12/command_processor.cpp` — RenderDoc `Start/EndFrameCapture` bracket in
  `IssueSwap` (env FABLE2_RDOC_CAP); `FABLE2_RT_DUMP` in `IssueCopy_ReadbackResolvePath` (dumps resolved
  bytes); resolve-readback format log (copy_dest_fmt/endian). ALL env-gated OFF by default.
- Binaries staged in `Fable2Recomp/out/build/win-amd64-nightly/` (Fable2.exe + rexgpu-xenos.dll +
  rexruntime.dll). Ghidra labels applied (90 fn + 5 net); TSVs `ghidra_out/labels_resume_2026-07-20.tsv`,
  `labels_net_resume_2026-07-20.tsv`.
- Workflows (both COMPLETED, no live agents): scripts saved under
  `…/81417202-…/workflows/scripts/pm4-desync-hunt-*.js` and `blackskin-fillpath-hunt-*.js` (re-runnable
  via Workflow {scriptPath}). Their findings are integrated into the memory files.
- Capture assets (Claude-run; user reviews these): `scratchpad/full/fl_frame21733.png` (readback=full,
  DOG+HERO visible, bluish-grey skin), `scratchpad/rb/rbf_frame9804.png` (readback=fast, glowing),
  `scratchpad/rtdump/` (RT .raw dumps + untile PNGs + `_untiled_all.png`).


One-page state capture in case of session/usage cutoff. Fuller narrative: HANDOFF.md ▶▶ 2026-07-20
sections (top). Memory: `fable2-black-skin-renderdoc` + `fable2-autonomous-test-harness`.

## Where the artifact hunt stands (the greenish-black hero-skin patches)

**★★ TERMINAL STATE (run 595, the last test of the session): the converterId-gate fix was ALSO
refuted by ground truth — all 80 decoder calls had CORRECT converterIds and ALL returned success
(0 rejects, 0 retries). The suspects NEVER REACH THE DECODER: their parsed header takes the
DISPATCH NO-OP BRANCH (82B8D170/82B8D028: mode==0 with flags lacking all of 1|2|0x10 → falls
through, nothing copies). ▶▶ THE NEXT-SESSION TASK = P2: force the no-op case through the proven
route — either (a) patch the flags word in the SOURCE resource bytes before the parse (need the
reader get-data ptr in the 82B8D170 hook), or (b) RE 82B8D170's exact arg signature + call
82B8CDF8(res, reader, hdr, idx, g) directly, or (c) find WHY these resources' headers carry
no-op flags (compare a suspect's header bytes vs a healthy composite's — the header fields are
in the first words of the resource; log them in the consume hook). The DECODE#/RETRY# hook
(FABLE2_MORPHUPLOAD_FIX) stays as scaffold. Elimination trail now complete through SEVEN layers:
wedge→refusals→readback→decompress→placement→decoder-gate→DISPATCH NO-OP (the confirmed gap).**

**★ FINAL UPDATE (post-snapshot): the tiling agent REFUTED tiling and found the true root —
THE UPLOAD NEVER RUNS. `ghidra_out/morph_upload_tiling_re.txt` (full chain decompiled): resource
mips are Lionhead delta-BC1; the mip decoder `texdecode_mip 0x82B94DE0` gates on
converterId == Xenos DXT fetch dword (0x1A200152/53/54 by subCode 0|0xC/1/2), derived from the
LIVE fetch constant; the suspects' mips=1 realloc-with-provider create builds a WRONG fetch
constant → decoder returns 0 forever → the eternal ret=0 retries/Finalize refusals AND the
deterministic garbage (creation-time leftovers) are ONE bug. FIX IMPLEMENTED
(`src/MorphFillTrace.cpp`, env `FABLE2_MORPHUPLOAD_FIX=1`, default OFF): hook 0x82B94DE0 — on
ret==0 with converterId mismatch, retry once with the correct constant; plus DECODE#/RETRY#
diagnostics under COMPLETIONDIAG. VERDICT PENDING (user load at session end). Deeper root fix
later: correct the fetch-constant at the StreamMips 0x821D7A98 create site (compare suspect vs
healthy tex+0x1C..+0x30 from the DECODE logs). rexgpu note: GPU-class fmts are CPU-TILED in guest
memory (tiler 8300C290); fmt-0x23-class is LINEAR — plugin must follow the fetch constant.**

Superseded hypothesis below (kept for the elimination trail): 360 TILING / MIP-PLACEMENT bug.
Evidence chain:
- The artifact = 6-7 streamed morph-source textures (DXT1 512²/1024², guest objs `4216Axxx`)
  whose guest bytes are: **first 16 bytes ZERO, data mid/late, ~66% zero, byte-identical hashes
  every run**. Zero-head DXT1 = black; scrambled placement = greenish noise.
- Consume census: every consume is a **whole-resource read** (`off=0 len=FFFFFFFF`), data pointer
  already populated → data IS delivered whole; ONE copy lands displaced. Suspects are created
  `mips=1`; healthy siblings `mips=6` → mips-dependent placement divergence = top candidate.
- **Dead theories (each implemented + tested + user-verified "no change"):** streaming wedge
  (v5 slot-heal — REAL bug, fixed, keep on), QueryMulti refusals, retry-map parking, v4 forced
  advance, GPU readback (GummiFableII port already in runtime; tested all configs), missing
  decompress (Fix A never engages — no pending jobs at consume), Fix B +0x10 zeroing
  (REGRESSION — leave OFF), cancels (0 fire in gameplay).
- **BACKGROUND AGENT IN FLIGHT at snapshot time** → `ghidra_out/morph_upload_tiling_re.txt`
  (incremental; readable even if cut off): full RE of upload `Function_82B95CE0` (mip-walk with
  format table `DAT_8331dadc` stride 0x70), consume variants `0x82B8D028→82B8CAC0/CDF8`,
  `0x82B8D170`/`82B8BA38` header parse, tiling location, and a fix design.
- **Verification loop is machine-visible**: `[compdiag] CONTENT-SUSPECT` list + `head=[...]` hex
  (zlib/zero detection) + consume census `[decompfix] CONSUME#` + `[compdiag] CANCEL#`. A correct
  fix = suspects' heads become nonzero, list empties. Then user visual.

## Today's shipped & verified wins (independent of the artifact)
1. **v5 slot-heal** (`FABLE2_SLOT_HEAL=1`, MorphFillPump.cpp hook on `sub_821F8F48`): fixes the
   REAL minutes-long wedge class (canceled in-flight read + single-request gate deadlock).
   Verified firing correctly incl. on the champion key; zero false positives. Recommend
   default-on.
2. **Autonomous test harness**: runtime scripted-input channel `FABLE2_INPUT_TAPS=<file>`
   (MnK driver; `TAP A|B|…|DPAD_*|LSTICK_UP/DOWN/LEFT/RIGHT` — sticks because menus read ANALOG
   stick state, never dpad/VK_PAD) — focus-independent, works minimized. One-command repro:
   `Fable2Recomp/tools/artifact_repro.ps1 -Arm loadsave -Seconds N [-Pump] [-RetryPoll 1|2]
   [-SlotHeal] [-WholeRead]` (stages/restores mystartup, `--gpu_plugin=xenos` REQUIRED,
   event-driven menu nav w/ save verification by texturemorphs size 48976=Hero001 "Hero 2",
   real-load marker = `region_specific_*` bank mounts, offline per-texture analyzer).
   Menu nav validated to 'Load Game' highlight; full end-to-end unproven (user usually loaded).
3. **Retry protocol + heal wave decoded** (`healwave_requery_re.txt`): driver `0x821F7A10`
   per-slot state machine; `0x82B6C960` = CANCEL (not WaitReady); reclaim `0x821F7830` re-queries
   state-3 slots = the heal wave.
4. **Refill/decompress stack decoded** (`refill_decompress_re.txt`): compression = per-entry
   class (raw 0x8200E02C / compressed 0x8200E058), eager whole-entry decompressed cache at load,
   deferred decompress jobs (vtbl 0x8200E134) behind forwarding wrapper 0x820F9DA8 (+0x10 inner).
5. **Render pipeline mapped** (`frame_end_layer_re.txt` + `renderqueue_catalog_re.txt`):
   3D-Engine thread → DeviceState_FrameDispatch 0x82388380 → Frame_Present 0x82B6F1D0.
   ⚠ RenderQueue (76 SubmitToFrame sites) = 2D OVERLAY ONLY (FillRect2D/DrawText2D — dev menu,
   profiler, subtitles) → own-renderer seam remains the PM4 ring; queue = free host overlay
   channel for Studio. ~120 Ghidra labels applied; several bogus noReturn poisons repaired.
6. **Front-end menu input model** (`frontend_menu_re.txt`): analog-stick nav, save-fan events
   LOAD_CARD_ACTIVE/SELECTED → load kick 0x82447160; menu-open marker = 'FrontEndMainMenu_New
   Game' button spawn (NOT the GUI_SCREEN_OPTIONS preload).

## Modified/created files (NO git in this folder — these are the disk state)
- `Fable2Recomp/src/MorphFillTrace.cpp` — QM refusal classifier, FINAL-STATE per-tex table,
  content sampler (zero%+hash+head-hex), CANCEL hook, Fix B wholeread (default OFF), summary
  cadence FINAL%2048.
- `Fable2Recomp/src/MorphFillPump.cpp` — v3/v4 retry-poll (default OFF), v5 slot-heal, Fix A
  decomp-fix + consume census (arm with FABLE2_DECOMP_FIX=1), map walkers.
- `rexglue-src/src/input/mnk/mnk_input_driver.cpp` + `include/rex/input/mnk/mnk_input_driver.h`
  — scripted-input tap channel (buttons + synthetic lstick axes). Rebuild:
  `rexglue-src/build_runtime2.cmd`, then rebuild exe target (POST_BUILD restages the dll).
- `Fable2Recomp/tools/artifact_repro.ps1` — the one-command A/B harness.
- `Fable2Recomp/assets/game/data/scripts/Startup/mystartup_loadsave.lua` — staged (its
  SetInitialSaveGameName route does NOT work; tap-channel nav is the working route).
- `out/build/win-amd64-nightly/Fable2.toml` — `readback_resolve="some"`,
  `readback_resolve_only_dest_bases=""` (allow-all; REVERT to the 0x1E-exclusion after the
  content fix), `log_resolve_readback=true`.
- Docs: `HANDOFF.md` (three big 2026-07-20 sections), this snapshot, memory files updated.
- `ghidra_out/`: healwave_requery_re.txt, frontend_menu_re.txt, frame_end_layer_re.txt,
  renderqueue_catalog_re.txt, refill_decompress_re.txt (+~50-row labels TSV UNAPPLIED),
  morph_upload_tiling_re.txt (agent in flight), decomp_* scratch files.

## Env-flag reference (all default OFF unless noted)
`FABLE2_FILLTRACE=1` `FABLE2_COMPLETIONDIAG=1` (diag; cheap) · `FABLE2_SLOT_HEAL=1` (v5, good)
· `FABLE2_DECOMP_FIX=1` (Fix A scaffold + consume census) · `FABLE2_RETRY_POLL=1|2` (v3/v4
scaffold) · `FABLE2_TEXSTREAM_PUMP=2` (v2 guard) · `FABLE2_REFILL_WHOLEREAD=1` (Fix B —
REGRESSION, keep OFF) · `FABLE2_INPUT_TAPS=<file>` (tap channel) · `FABLE2_DRAWDIST=2.0`
`FABLE2_MODAPI=1` `FABLE2_MODTEXT=1` (standard). Launch: nightly tree +
`--game_data_root … --allow_game_relative_writes true --gpu_plugin=xenos --mnk_mode true`.

## ▶▶ RESUMED 2026-07-20 (post-crash) — P2 DISPATCH FIX IMPLEMENTED + BUILT
The next-session task (P2) is now DONE in code and compiled (nightly, BUILD_EXIT=0):
`src/MorphFillTrace.cpp` — new weak-override `REX_HOOK_RAW(sub_82B8D170)` (`texres_materialize_whole`).
Faithful reimplementation of the parse+dispatch (verbatim from decomp), on a guest-stack scratch
header (lowers r1 for callee frames). Parsed hdr offsets: +0x8 mode, +0xC flags, +0x10/+0x14 w/h,
+0x18 fmtEnum, +0x1C mips, +0x50 subCode. When `FABLE2_MORPHUPLOAD_FIX=1` AND a suspect
(mode==0, `(flags&0x13)==0`, fmt∈{0x23,0x25,0x28}, w≥64): set `flags|=0x10` on the parsed struct →
forces the intended `82B8CDF8` (CDF8) linear-upload route instead of the no-op fall-through.
`FABLE2_COMPLETIONDIAG=1` logs `[morphupload] MATERIALIZE#/FORCE#` (header words, NO-OP/SUSPECT tags)
— gives suspect-vs-healthy header ground truth at THIS site. Default OFF (both paths inert without
the env). ▶ PENDING: run with `FABLE2_MORPHUPLOAD_FIX=1 FABLE2_COMPLETIONDIAG=1` on the adult
Hero001 save → confirm MATERIALIZE logs show suspects as NO-OP, FORCE# fires, CONTENT-SUSPECT heads
go nonzero, then USER VISUAL. (The old decoder-converterId retry hook above it is REFUTED but kept
as scaffold — it never fires now since suspects reach the FORCE path first.)

## Immediate next steps (in order)
1. ~~Read morph_upload_tiling_re.txt → implement the placement fix~~ SUPERSEDED. P2 dispatch fix
   IMPLEMENTED+BUILT (see RESUMED block above) → RUN with the two envs → verify via
   head-hex/CONTENT-SUSPECT + MATERIALIZE/FORCE logs → user visual.
2. ~~Apply the refill labels TSV~~ DONE (2026-07-20 resume): 90 function labels applied via
   LabelApply from `ghidra_out/labels_resume_2026-07-20.tsv` (frame_end_layer + morph_upload_tiling
   + refill_decompress, deduped 98→90; 8 remaining are DATA vtables, need a label-not-function pass).
3. ⛔ Default-on FABLE2_SLOT_HEAL = REVERTED (default-OFF again): it caused a USER-CONFIRMED
   FULLY-BLACK skin regression (fires on the champion morph slot 00568ED5/0000663D → forced re-query
   blacks the whole morph texture). Keep SLOT_HEAL strictly opt-in (=1). ⚠ Revert the toml readback
   filter ONLY AFTER the black-skin fix is verified (still pending).
4. Own-renderer: PM4-ring P0 tracer (per OWN_RENDERER.md; the RenderQueue detour is closed).
