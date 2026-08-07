# Retail Frontend Spec — the ground truth Fable2Native implements against

**Created:** 2026-08-06. **Purpose:** one evidence-cited specification of how the *retail* Fable II
frontend actually behaves — state flow, layout, animation, movies, sound, input — recovered from the
XEX/PPC (Ghidra `Fable2_TU1`, the 299 MB generated ReXGlue C++, and the recomp as a live oracle) and
the GUI LuaQ scripts. The native PC frontend (`Fable2Native`) is built from THIS, not from
approximation. Every claim cites its source; unverified items are marked **GAP**.

> Layering rule (confirmed this pass): the LuaQ scripts are a **thin top** that only *fire events*
> (`CAN_PRESS_A`, `END_LEGAL`, `OUTRO_FINISHED`) and choose slides. All the heavy behavior — movie
> playback, sound, timing, UI rendering, screen management — is **native PPC**. So the exe/PPC
> decompilation is the primary source; scripts are the supporting layer.

## Evidence sources & how to regenerate

- **Ghidra headless (per-function clean C):** `analyzeHeadless "D:\Documents\Fable2RE\ghidra_proj"
  Fable2_TU1 -process -noanalysis -readOnly -scriptPath tools\ghidra_label -postScript
  DecompFuncs.java 0xADDR …` (program = `default_tu1.xex`). Discovery: `FindStrHits.java <keywords>`
  (string→xref), `DumpFuncsInRange.java lo hi`.
- **Generated PPC→C++ (whole game):** `Fable2Recomp/generated/Fable2_recomp.*.cpp` (299 MB, gitignored;
  functions appear as `sub_<addr>`). Use to cross-check any Ghidra decomp.
- **GUI LuaQ (270 entries):** `python tools\lua_mod\script_index.py --bnk
  Fable2Recomp\assets\game\data\guiscripts.bnk {list|search|disasm <name>}`.
- **Prior RE, still authoritative:** `ghidra_out/frontend_menu_re.txt` (input model + save-fan),
  `docs/RETAIL_FRONTEND_DRAW_TRUTH.md` (UVs/blend/layer order + animation constants),
  `docs/TITLE_SCREEN_FIDELITY.md` / `docs/TITLE_SCREEN_HANDOFF_2026-08-03.md` (title reveal),
  `docs/LEVEL_LOAD_RE.md` (loading transition), `docs/FRONTEND_FULL_PASS_NEXT_SESSION.md` (options).
- **This pass' new dumps:** `ghidra_out/frontend_cluster_decomp.txt`, `frontend_lua_disasm.txt`,
  `frontend_movie_strhits.txt`, `frontend_movie_sound_decomp.txt`.

---

## 1. Frontend state machine (the "slides")

**Script side —** `scripts/gameface/frontend.lua` (disasm: `ghidra_out/frontend_lua_disasm.txt`).
`_G.FrontEnd` with:

```
SLIDE = { PressStart=1, Title=2, NewGame=3, LoadGame=4, Option=5, Multiplayer=6, Debug=7, COUNT=8 }
TitleItem   = SLIDE.NewGame        -- current title selection
Time=0, ButtonY=0, StarVel=0       -- title animation state
NavUI       = getElement('Scene.Layer.pagenav')
```

Registered on `UIContract` / `IndicatorButton` via `registerForEvent`:

| Event | Handler | Behavior (from disasm protos) |
|---|---|---|
| `OnPressA` | `OnPressConfirm` | if slide==Title → `GoToSlide(TitleItem)` + set `NavUI.opacity=100`; elif slide==PressStart → `GoToSlide(Title)` |
| `OnPressB` | `OnPressBack` | if slide==PressStart → nothing; else `GoToSlide(Title)` + set `NavUI.opacity=0` |
| `OnPressUp` | proto[5] | if slide==Title → `SelectTitleItem(TitleItem-1)` |
| `OnPressDown` | proto[6] | if slide==Title → `SelectTitleItem(TitleItem+1)` |

`SelectTitleItem(item)` (proto[9]): clamp to `NewGame..Debug`; store `TitleItem`; `getElement` the
target layer (`Scene.Layer.{newgame,loadgame,options,multiplayer,debug}`); read its `position.y` →
`ButtonY`; set `IndicatorButton.position.y = ButtonY`. **⇒ the green-A indicator slides to align with
the highlighted title item; that IS the title menu selection animation.**

`GoToSlide`/`ComponentSetSlide` resolve to native `goToSlide` (`art/gui/gameface/uicontract.lua`,
`slidejump.lua`). Slides map 1:1 to `Scene.Layer.*` elements shown/hidden by opacity.

**Native side —** screen/event dispatch hub around `0x826DE740` (registration) with per-event
handlers (`ghidra_out/frontend_cluster_decomp.txt`):
- `826E0D60` — new-game gender select: writes state code (`+0x18 = 3/4/0`) on
  `BOY_ACTIVE/GIRL_ACTIVE/…`.
- `826E13F0` — `OUTRO_FINISHED`: swaps the active screen object (`iVar3+0xbc = pending`, calls
  vtbl init/enter) — this is the screen-transition primitive.
- Input firers `824052F0`/`8240531C` push `OnPressB`/`OnPressA`; direction firers `82404ED0`
  (codes 0x1C–0x1F) / `82405030` (0x20–0x23) push `Up/Down/Left/Right` (two analog-stick groups).

**CFrontEndManager handler map (2026-08-07, `frontend_mgr_bodies.txt`; region dump
`frontend_mgr_funcs.txt` — most entries are `0x8` tail-call thunks, real bodies listed):**
- `826D8008(mgr, evtCode)` — **the event→handler dispatch table lookup**: linear-scans the record
  array `mgr+0x38..+0x3c` (stride `0x18`, key at `entry+0x10`), returns the handler at `entry+4`.
- `826D81C8` — vector push-back helper (stride `0x18`) used to build that table.
- `826D5480(screen, evt)` — frontend **input handler**: when screen-state `*(*(DAT_83496b08+0x1c)+0x28)==2`,
  `evt==0x24`(A)→action `8236FEC8(screen,2)`; `evt==0x2e`→sound slot `0x83497E50` + vtbl+0x10 on `screen+0x108`.
- `826D5AF0` — **accept/confirm**: gate `826C1D98`; fail → sound slot `0x83497E4C`; success → set
  `*(*(DAT_83496b08+0x38)+0x98)=0x1f` and screen-transition via `823DCBE0`/`82426518`/`82426BA8`.
- `826D9620`/`826DA038(_, msg)` — **save/load message handlers**: msg `0x8c`→sound `0x83497DD8`,
  `0x8d`→sound `0x83497DDC`, `0x96`→delete-save (`SaveSlotMap_Find(…,1)`→`82cc29a0`/`82cc2ea0`).
  `826DA038` is guarded by the busy check `*(*(DAT_83496b08+0x1c)+0x2c)!=0 → return`.
- `826E0D60(screen, msg)` — new-game gender (`msg+4` vs event-ID globals `DAT_8349b598/59c/5b0/5a0/5a4`
  → `screen+0x18 = 3`(boy)/`4`(girl)/`0`(cancel)); `826E13F0` — `OUTRO_FINISHED` screen swap
  (`scr+0xbc = pending`; vtbl `+0x1c` enter, `+0x5c` enable toggles; transition `82426CA8`). Both confirm §1/§7.
- Roots: `DAT_83496b08` = GUI/screen manager (`+0x1c` current screen, `+0x38` transition target);
  `DAT_83496ab8` = app root (`+0xc` subsystem, `+0x8c` movie player, `+0x9c` save-slot map).

**CFrontEndManager FULL method map (2026-08-07, `ghidra_out/frontend_mgr_methodmap.txt`).** All real
bodies in `0x826D4000–0x826E2000` now decompiled (the `0x8`-size entries are tail-call thunks).
Highlights beyond the prior list: `826D5914` = **confirm/advance** (builds a screen transition
`82426518`/`824265A0` name `@820ae15c`, fires `SE_GUI_MENU_BOX_SELECT`); `826D5A60` = **back/cancel**
twin (gate `826C1D98`; fail → `SE_GUI_MENU_BOX_CANCEL`); `826D5D00` = **screen tick** (calls each child
widget `vtbl+0xc(dt)` with `dt=DAT_82000b8c`); `826D4808` = an 8-entry event-code(`0x5A..0x61`)→SE_*
**name LUT**; `826D6AF8` = **hash-table insert** that fills the dispatch table `826D8008` scans;
`826D8C78` = the main **object dtor** (vtbl `829B72B8_820c0de4`); `826D9FC8` = **screen-singleton bind**
(`0x8349b4d4`); `826E1C50` = **screen-ready flag setter** (`scr+0xce=arg; scr+0x39=1`, pairs with the
`826E13F0` outro swap); `826D5DF8/826D8B40/826DE658/826D8458/826D9A58` = sub-object + scalar-deleting
dtors. Confirms the manager's shape: input→action (`826D5480`→`826D5914`/`5A60`/`5AF0`, all gated by
`826C1D98`, each firing the matching `SE_GUI_MENU_BOX_*` sound) · dispatch table (`826D81C8`+`826D6AF8`
build, `826D8008` reads) · per-frame update `826D5D00` · save/load `826D9620`/`826DA038` · lifecycle.

## 2. Input model (authoritative — `ghidra_out/frontend_menu_re.txt`)

- Menus navigate on the **analog thumbstick** (digitized to codes 0x1C–0x23), **not the D-pad**. The
  exe never compares `VK_PAD_*`. Keyboard works because W/A/S/D map to the left stick.
- `A`=`OnPressA`→Select, `B`=`OnPressB`→Back (codes 0x24/0x25). Repeat timing gate `CanNavigate`:
  `FIRST_NAVIGATE_INTERVAL` then `SUBSEQUENT_NAVIGATE_INTERVAL` (0.15/0.10/0.20 s).
- Native impl target: Fable2Native input already supports kbd/mouse/XInput; ensure stick-driven
  navigation + the CanNavigate repeat gate, not per-frame dpad steps.

## 3. Boot → intro movies (native)

**Movie names (Bink):** `microsoft_logo.bik` @`0x820A1A4A`, `lionhead_logo.bik` @`0x820A1A76`
(`frontend_movie_strhits.txt`); sequence per `docs/NATIVE_PORT_PLAN.md` =
`microsoft_logo → lionhead_logo → middlewarelogos → intro`.

**Movie native API (Lua-callable, addresses from `lua_natives5_catalog.tsv`):**

| Native | Addr | Notes |
|---|---|---|
| `PlayMovieInternal` | `82306520` | `mgr=822C6C68(); play=823C0D90(mgr, name, 0,0,1)` — **real player = `0x823C0D90`** |
| `PlaySilentMovieInternal` | `82306560` | |
| `PlayLocalisedMovieInternal` | `823065A0` | localised voice track |
| `PlayMovieWithGenderedVoiceInternal` | `823065E0` | gendered VO |
| `PlayLocalisedMovieWithGenderedVoiceInternal` | `82306620` | |
| `IsMoviePlaying` | `82306660` | true when movie state ∈ {9,10} |

(decomp in `ghidra_out/frontend_movie_sound_decomp.txt`).

**Real player `0x823C0D90` — full body decompiled (2026-08-07, `ghidra_out/frontend_movie_reform.txt`).**
`play(mgr=param1, name=param2, syncFlag=param3, localisedFlag=param4)` (the 5th recomp arg is a
tail default). Manager singleton `mgr = 0x834c3598` (lazy ctor `0x823C0C88`: owns a
`RtlInitializeCriticalSection` at `mgr+0x30` and a `hk_malloc(0x14)` intrusive-list node). Flow:
1. Scoped-lock `mgr+0x30` (`Function_82200770`), then **guard `mgr+0x2c`: if nonzero → `return 0`**
   (a movie is already active — retail plays **one movie at a time**).
2. **Path resolution** — non-localised (`localisedFlag==0`): prefix `"GAME:\"` (`@820a1788`); localised:
   `"videos"`/`"\videos\"` (`@820a938c`) + existence-validate (`Function_82383BB8`, bail `return 0`
   on miss). ⇒ movies live at **`GAME:\videos\<name>`**.
3. Load resource into `mgr+0x8` (`Function_821E3108` → `intrusive_ptr`), validity-check
   `Function_82B42340(mgr+8)`. On miss, **fallback drives** `"game:\data\"` (`@820ad404`) and
   `"epi:\"` (`@820ad410`, episodic-DLC drive) via `Function_821E6848`, re-check; still invalid →
   `return 0`.
4. Movie **player object** = `*(*(DAT_83496ab8+0xc)+0x8c)`. If its flags `+0x34 && +0x35` set,
   allocate a `0x80`-sized preview surface (`Function_82CAA250(…,0x80,size,0x80)` + `Function_82349B50`).
5. Activate the player: `Function_822C3F50(player,1)` + `Function_82182C30(player,1)`; **set
   `mgr+0xc = 1`** (manager movie-state = starting/playing).
6. If `syncFlag` set: binary-search a codec/format table (`player+0x48..0x4c`, stride 8, key `<0xc`)
   and recompute `mgr+0xc` from a width field (`LZCOUNT(*(entry+0xc)-1)>>5)+1`) — codec/tiling class.
7. Atomic-inc generation counter `DAT_83497088`; return the `"videos"` handle.

`IsMoviePlaying@82306660` checks a **different** object — `*(*(*(*(DAT_83496ab8+0xc)+0x80)+4)+4)`
list head → `vtbl+4` GetState, **true when GetState ∈ {9,10}**. So two state notions:
`mgr+0xc` (manager) vs the player's `GetState`.

### Movie state machine (resolved) — 2026-08-07, `ghidra_out/frontend_movie_statemachine.txt`

Reformed the two truncated core functions: the manager **tick** `MoviePlayer_FrameUpdate@0x823C1658`
(body 1624B, `frontend_movie_tick_reform.txt`) and the manager **driver/pump** `0x822A9D60`
(body 832B, `frontend_movie_elem_reform.txt`). The **state setter** is `0x823C1CB0(mgr,newState)` =
`stw newState → mgr+0x2c` under the lock. Manager singleton = `DAT_834c3598`.

**Manager state `mgr+0x2c` — full lifecycle `0→1→2→3→4→5→0`:**
- **0 IDLE** — no movie.
- **1 QUEUED** — driver `0x822A9D60`: once the audio bus is idle (`**(DAT_83496b08+0x44)<1`) and a
  visual is wanted (`mgr+0x50`), binds the movie surface (`lh_string @0x82000ca0`), registers a render
  callback node, `setter(mgr,2)`.
- **2 START/OPEN** — tick: `hk_malloc(0xE4)` Bink decoder (vtbl `82A41A98_820f4704`);
  `Function_82A41B48(decoder,nameHash,flags,1)` opens the stream; publishes the two video-texture
  handles to the GUI element (`el+0x7c/+0x80`); `setter(mgr,3)`. (decoder-open fail → does not advance.)
- **3 PLAYING** — tick: `Function_82A41F10(decoder)` decodes each frame; subtitles built and submitted
  to the frontend RenderQueue; **gate `if(*(decoder+0xdd)==0) return;`** — plays while the decoder's
  `+0xdd` end-of-stream flag is clear. When it sets → `setter(mgr,4)`.
- **4 FINISHED** — driver: unbind surface, `Function_82318800(list,10)` stop-broadcast, restore normal
  frontend view; `setter(mgr,5)`; **sequence hand-off**: `if(DAT_83496a5e && el+0x144){ el+0x144=0;
  DisplayCoopScreen(); }`.
- **5 STOP/FREE** — tick: refcount-release the decoder, free the subtitle buffer, tear down the text
  object, restore the GPU reg, `setter(mgr,0)`.

Retail plays **one movie at a time** — `PlayMovie@0x823C0D90` returns 0 if `mgr+0x2c != 0`.

**(a) GetState 9 vs 10.** `IsMoviePlaying@0x82306660` reads a *different* object — the GUI **movie
element** (`el = **(*(*(*(DAT_83496ab8+0xc)+0x80)+4)+4)+8)`) via `vtbl+4` (GetState), true when
`∈{9,10}`. **9 = PLAYING** (element presenting frames; ↔ manager state 3). **10 = FINISHING**
(element still bound / last-frame held during teardown; ↔ manager 4→5 — note the literal `10` is the
stop/transition code passed to `Function_82318800`/`82318718`). Both read "busy" so callers keep the
"movie is up" gate asserted across teardown, avoiding a 1-frame menu flash before the next clip binds.

**(b) Per-clip duration** = intrinsic to the `.bik`: `numFrames / 30` (decoder frame count
`*(decoder+0x40)+8`, `0x1e`=30 fps). No external table; playback ends when the decoder sets `+0xdd`.
⇒ native's frame-count-derived durations are correct.

**(c) Skip** = a forced STOP: the element owner calls `setter(mgr,5)` directly (`li r4,5; bl 0x823C1CB0`
at `0x822A9F18`), short-circuiting PLAYING → STOP/FREE (bypassing 3→4). An A/B during a logo clip is
what routes here through the frontend input firers (§1).

**(d) Sequence driver** = a serialized QUEUE, not a hard switch. Each `PlayMovie` loads exactly one
resource into `mgr+0x8` and refuses a second while `mgr+0x2c != 0`, so the four clips
(`microsoft_logo → lionhead_logo → middlewarelogos → intro`) are strictly serialized: clip N finishes
(4→5→0) before clip N+1's `PlayMovie` succeeds. The ORDER lives in the caller (script/native), not the
manager; the state-4 branch self-advances the queue (and hands off to `DisplayCoopScreen()` after the
intro). ⇒ matches the native impl (`native_frontend.cpp:21` clip list, one serialized `NativeVideoPlayer`).

**Native wiring VALIDATED against this RE (2026-08-07):** `Fable2Native/src/native_frontend.cpp:21`
already sets the clip list `microsoft_logo → lionhead_logo → middlewarelogos → intro` (matches the
retail sequence), loads from a `videos/` folder (matches retail's `\videos\`), and uses separate
`intro_videos_`/`attract_videos_` `NativeVideoPlayer`s driven by explicit `FrontendState` transitions —
which structurally enforces retail's one-movie-at-a-time `mgr+0x2c` guard, and maps GetState `Playing`
↔ `{9,10}`. ⇒ **no code change needed**; the retail truth confirms the existing native intro-video
path. Native's per-clip durations look frame-count-derived (likely fine); the retail
`epi:\`/`game:\data\` fallback drives are a retail-disk concept not relevant to the native `videos/` layout.

## 4. Title screen (native + script)

- Startup BGF `frontendstartupscreen.bgf` names `FXGUI_Logomain{,_Fadein,_Ambient}` →
  `Art\GUI\FrontEnd\Textures\fe_f2logo.tex` (1024×512 DXT1). Also `PressStart`, `Legal`,
  `TEXT_SPLASH_SCREEN_LEGAL`, `maiandra gd` (font). (`docs/TITLE_SCREEN_HANDOFF_2026-08-03.md`.)
- Behavior scripts (tiny; `art/gui/gameface/`) just `fireEvent` on `parent`:
  `behaviorcanpressa.lua`→`CAN_PRESS_A`, `behaviorendlegal.lua`→`END_LEGAL`,
  `behavioroutrofinished.lua`→`OUTRO_FINISHED`. The native side consumes these (screen swap = §1
  `826E13F0`).
- Reveal timing: black/grey hold ≈ **5.9 s** after title entry, then panorama reveal ≈ **1.1 s**
  (source-video); live PM4 shows a ~10-frame (167 ms) geometric atlas slide.
  (`TITLE_SCREEN_HANDOFF_2026-08-03.md`.)
- Reveal draw truth: indexed sprite block seq `95346..95395`, VS `6D15306961102F7D`, PS
  `4B61E208208F3F5B`, atlas `0x0FBE7000` (512×512 fmt 20), detail `tf14` from UV1; material
  `out.rgb = detail.rgb*c47.x + detail.a*main.rgb`, `out.a = main.a*detail.a`, RT0 blend
  `0x07060706` (src-alpha/inv-src-alpha). (`TITLE_SCREEN_HANDOFF_2026-08-03.md`.)
- Title animation state: ⚠ **CORRECTED 2026-08-07** — `Time` and `StarVel` are DECLARED-BUT-UNUSED in
  the shipped `frontend.lua` (`Initialize` sets them to 0; only `ButtonY` is ever re-read, driving the
  green-A indicator's Y). There is NO script-driven star/sparkle motion — the earlier "StarVel = star
  motion" framing is superseded. The reveal effect is **native FXGUI**, not Lua. (RE evidence:
  `docs/TITLE_SCREEN_FIDELITY.md` "Effect RE 2026-08-07", `ghidra_out/title_ui_re/title_sparkle_logic.txt`.)
- Reveal geometry = **procedural, per-frame, native** (`ghidra_out/title_effect_render_re.txt`):
  element-draw driver `Function_82197AE8` runs a keyframe-interpolation ramp; shared quad builder
  `FUN_82A7E2EC` writes the 18-dword vertices from static `.data` templates (unit quad + index list
  `[1,0,2,2,3,1]` @`0x83318540`, attrib/color template @`0x83318070`); submit via GUI primitive
  `Function_82208B10`. Fixed format/positions/indices; **UV/color/alpha computed per frame**. Decoder:
  `tools/decode_title_reveal_geometry.py`. GAP: the checked-in geometry dumps are the MAIN MENU, not the
  black→panorama reveal instant — capture that via `ghidra_out/title_capture/` (harness ready).

## 5. Main menu — layout, animation (authoritative constants)

From `expandablemenuformatting.lua` / `expandablemenuanimation.lua`
(`docs/RETAIL_FRONTEND_DRAW_TRUTH.md`):
- `HIGHLIGHT_INDEX=4`, `NUM_VIS_SLOTS=12`, `ANIMATE_TIME=0.15`, `ANIMATE_IN/OUT_TIME=0.08`.
- Visible scale/opacity profiles: `(0.65,0)`,`(0.75,50)`,`(0.85,75)`,`(1.0,100)`; title size 18;
  truncation widths 200/210/240/305.
- Curved-rail slot offsets — x: `0,22,42,56,56,56,56,56,50,42,22,0`; y:
  `85,69,15,-45,-106,-164,-222,-280,-338,-396,-454,-475`.
- Selection interpolates prev→target slot props linearly over `0.15 s`.

Draw/asset truth (`RETAIL_FRONTEND_DRAW_TRUTH.md`): rows are 3-slice
(`frames_elements` outer rim + `ability_elements`/`frames_04` inner) at UV bands
left`(0,.004)-(.125,.164)`/center`(.125,…)-(.813,…)`/right`(.820,…)-(.945,…)`; selected-A =
`MenuHighlight` group `rim_and_red` then `green_top` + `green_top_translucent` (all `frames_elements`);
side rails = serialized `0FD07000/0FD67000` (L) + `0FEE7000/0FF47000` (R), **not** the 256×1024
`0FE27000` atlas.

## 6. Options (from `docs/FRONTEND_FULL_PASS_NEXT_SESSION.md`)

Scene = `optionsscreen.{fac,bsg,bgf}` (guiscripts entries 161–163; BGF byte-identical to retail).
Hierarchy `Layer→Camera→Foreground→Book→Pages(1..4)+RightFrame+ExpandableMenu→OptionsMenuY`.
Page order **Game, Video, Controls, Audio**; page contents and retail captures in
`resources/options/*.png`. Native additions (Resolution/Anti-Aliasing) must sit inside this structure
and be backed by real swapchain/renderer changes — **GAP** until wired.

## 7. Save/Load fan + new-game gender (native — `frontend_menu_re.txt` §2.3)

- `loadgame` slide; stick L/R rotates the card fan; focused card fires `LOAD_CARD_ACTIVE`
  (state `+0x18=5`, reads that save's `texturemorphs.bin`/`mainsave.bin` preview), `A` →
  `LOAD_CARD_SELECTED` → handler `826E0E58` → start-load worker `82447160(saveArray[idx])`;
  `UNFREEZE` resets. Save list built by `SaveList_FilterByType@82EF0150` (content-type `0x5841091D`).
- New-game gender: element `choosecard`, events `BOY/GIRL_ACTIVE/_SELECTED` → handler `826E0D60`.

## 8. Sound (native)

Native API (`lua_natives5_catalog.tsv`; decomp `frontend_movie_sound_decomp.txt`):
`PlaySound@82307278` (resolves event via `82371E80` on sound-mgr `DAT_83496b08` → vtbl play);
`Sound.PlayEvent@82431A50`, `PlayEventSerialised@82431C08`, `PlayEventAtPitch@82431CF0`,
`IsSoundCategoryPlaying@82431EC0`, `StopSoundCategoryPlaying@82431ED8`, `SetSoundVolume@823B5C88`.
**Menu SFX dispatch + literal event names — FULLY RESOLVED (2026-08-07).** Frontend handlers fire
sounds through **`82371E80(out, sound-mgr=DAT_83496b08, &eventSlot)`** (reformed body,
`frontend_sound_resolver.txt`): builds a 112-byte request (`821D5F08` ctor → `82265220` assign event
→ `82c028e8` submit at level 3), then the caller `82C05EF8(out)` plays it. Each `eventSlot` is an
**`lh_string`** in a `.data` block `~0x83497DBC..0x83497E54`, filled at boot by
`lh_string_assign_cstr(&slot, "SE_GUI_…")` (getters `8324F148/188/888/8C8`, `frontend_sound_slotfill.txt`).
The literal names are static `.rdata` strings (`frontend_sound_names.txt`):

| Slot global | Fired by | **Event name** | Menu action |
|---|---|---|---|
| `0x83497DD8` | `826D9620`/`826DA038` msg `0x8c` | **`SE_GUI_SLIDE_MENU_UP`** | navigate up |
| `0x83497DDC` | `826D9620`/`826DA038` msg `0x8d` | **`SE_GUI_SLIDE_MENU_DOWN`** | navigate down |
| `0x83497E50` | `826D5480` (evt `0x2e`), `82990540` (A=`0x24`) | **`SE_GUI_MENU_BOX_SELECT`** | select / confirm (A) |
| `0x83497E4C` | `826D5AF0` fail path | **`SE_GUI_MENU_BOX_CANCEL`** | back / cancel (B) |

**Full `SE_GUI_*` table** at `0x820B0700..0x820B0954` (for later menus): `SE_GUI_MENU_CLOSE`,
`SLIDE_MENU_UP/DOWN`, `EXPAND_LIST`, `CLOSE_LIST`, `NEW_PAGE`, `PREVIOUS_PAGE`, `BLADE_SLIDE_IN/OUT`,
`ITEM_LATE`, `BLADE_SELECT_ITEM`, `CHANGE_CLOTHING`, `BUY_ABILITY`, `SELL_ABILITY`, `EQUIP_WEAPON`,
`QUIT_GAME`, `USE_ITEM`, `EAT_ITEM`, `DRINK_ITEM`, `BLADE_CONTENTS_SCROLL`, `SLIDER_LEFT/RIGHT`,
`SELECTION_LEFT/RIGHT`, `MENU_BOX_UP/DOWN/CANCEL/SELECT` (all prefixed `SE_GUI_`).

**Ruled out (`frontend_sound_callers.txt`):** the direct-`bl` xref route — `Sound.PlayEvent@82431A50`
and `PlayEventSerialised@82431C08` have one internal caller each; `PlaySound@82307278` has zero. Frontend
SFX go through `82371E80`, not those. ⇒ **no runtime breakpoint needed; names recovered statically.**

### Native implementation (2026-08-07 — WIRED + base-game-sourced, builds+tests green)
- **Identity:** `NativeFrontendSound` (native_audio.h) is keyed to these `SE_GUI_*` names via
  `se_gui_event_name()`; both app input handlers fire the distinct events (Up=`SLIDE_MENU_UP`,
  Down=`SLIDE_MENU_DOWN`, Left/Right=`SELECTION_LEFT/RIGHT`, A=`MENU_BOX_SELECT`, B=`MENU_BOX_CANCEL`).
- **Assets — cooked from the user's base game, nothing shipped** (per `NATIVE_PORT_PLAN.md`):
  `Fable2Native/tools/cook_gui_audio.py <game-dir> <out>` reads the user's `data/audio/gui.bnk`
  (AssetBrowser `bnk_reader`; entries are named GUI wavs), strips the Xbox `xma\0` prefix, rebuilds a
  canonical little-endian XMA2 `fmt ` header, and `ffmpeg` (xma2 decoder) → **PCM s16le 48 kHz**,
  writing `<out>/audio/<SE_GUI_EVENT>.wav` + `audio_manifest.ini` (event→file). Runtime loads by
  event name from `--audio-root` (default `<ui-root>/native_audio`).
- **Note:** the pre-existing shipped `native_audio/*.wav` were raw `xma\0` XMA blobs; the PCM-only
  loader rejected them, so native menu SFX were silent until this cook. `menu_interlude.wav` (music)
  is likewise `xma\0` from a different bank — a separate follow-up (identify its base-game source).
- **`gui.adb` parsed (2026-08-07, `tools/parse_gui_adb.py`, `ghidra_out/gui_adb_format.txt`):** the
  .adb is `LhCoMpReSsEd`+zlib → `LhBiNaRy` with (A) 18-byte event records
  `[u32 FNV1(event)][u32 sampleDefHash][u32 type=0x03000000]…`, (B) a 1-byte-length-prefixed name
  table (68 `SE_GUI_*` + others), (C) a `Waveforms` def section. 97 events + all frontend-6 resolved
  (e.g. `SE_GUI_MENU_BOX_SELECT`→`sample 0x98774457`). **The event→bank-wav step is NOT closable from
  adb+bnk alone**: `sampleDefHash` is a sound-build GUID (not FNV1 of the `.wav` filename, absent from
  the Waveforms section), and gui.bnk carries no hash — only names/indices. So the cooker's
  `EVENT_TO_BANK_WAV` remains the working evidence-based map. **One correction (recommendation only):**
  the four move events `SLIDE_MENU_UP/DOWN`+`SELECTION_LEFT/RIGHT` have **four distinct** sample defs
  in the .adb, so collapsing all onto `guislide-scroll_01_alt.wav` is an approximation — LEFT/RIGHT
  could use a distinct wav (e.g. `guislide-scroll_attack.wav`). Exact binding needs a runtime capture
  of the resolver `82371E80` per event slot. (Do not auto-edit `cook_gui_audio.py`.)

## 9. Loading transition (`docs/LEVEL_LOAD_RE.md`)

Frontend→game hands off to the two graphics-streaming state machines (`LevelGraphicsFile` +
`EngineResourceList`, pumped by `TextureSystem_FrameUpdate@0x82181B68`). The gameplay entity/`.save`
load is a separate path (`Function_82447160` kicks it for a chosen save).

---

## 10. Native implementation checklist (what to build/verify against this spec)

1. **State machine:** implement the exact `SLIDE` set + `TitleItem` model + A/B/Up/Down semantics
   (§1); `pagenav` opacity toggle; `IndicatorButton` Y-tracking as the selection animation.
2. **Movies:** clip list + skip policy from `823C0D90` (§3) driving `native_video.cpp`.
3. **Title:** 5.9 s hold → 1.1 s reveal; the `4B61E208…` material equation; `Time/ButtonY/StarVel`.
4. **Menu:** the exact slot offset/scale/opacity tables + `0.15 s` interpolation (§5); 3-slice rows;
   `MenuHighlight` A-ring layer order.
5. **Options:** Game/Video/Controls/Audio structure; native settings backed by real backend (§6).
6. **Save/Load:** card fan + preview reads + select→load worker semantics (§7).
7. **Sound:** hook move/accept/back to real events once names traced (§8 GAP).

## 11. Open GAPs to close next (all exe/PPC)

- ~~`823C0D90` movie player body~~ **DONE (2026-08-07, §3)**. ~~sub-gap: GetState 9 vs 10, per-clip
  duration, skip, sequence driver~~ **ALL DONE (2026-08-07, §3 "Movie state machine (resolved)",
  `ghidra_out/frontend_movie_statemachine.txt`):** 9=PLAYING/10=FINISHING (element GetState);
  duration = `.bik` numFrames/30; skip = forced `setter(mgr,5)`; sequence = serialized one-at-a-time
  queue driven by `0x822A9D60` (manager states `0→1→2→3→4→5→0`, setter `0x823C1CB0`).
- ~~Frontend menu **sound**~~ **DONE (§8)** — mechanism, action→slot map, AND literal event names
  (`SE_GUI_SLIDE_MENU_UP/DOWN`, `SE_GUI_MENU_BOX_SELECT/CANCEL`) all recovered statically; full
  `SE_GUI_*` table at `0x820B0700`. Nothing left.
- ~~`QuitToFrontEnd@823BB790` real body~~ **DONE (2026-08-07, `ghidra_out/frontend_quit_real.txt`)**:
  `823BB790` is a trampoline (defaults the target name to "") that calls the **real body
  `0x82312530`** (logs `"QuitToFrontEnd(%s)"`, flushes Live stats, dismisses the current screen,
  **sets `subsystem+0x104 = 1`** = the return-to-frontend request, tears down gameflow via
  `Function_82312DD0`; optional target name stashed at `*(*(DAT_83496ab8+8)+0x24)+0x10`).
- ~~Frontend-manager region function map~~ **FULLY DONE (2026-08-07,
  `ghidra_out/frontend_mgr_methodmap.txt`)** — every real body in `0x826D4000–0x826E2000` decompiled
  and role-labelled (see §1). Remaining `size=0x8` entries are tail-call thunks (nothing to add).
- ~~`gui.adb` binary event→sample linkage~~ **PARSED (2026-08-07, `tools/parse_gui_adb.py` +
  `ghidra_out/gui_adb_format.txt`)** — format fully reversed: 18-byte event records
  `[FNV1(event):4][sampleDefHash:4][type=0x03…]`, a length-prefixed name table, and a `Waveforms`
  section. 97 events resolved incl. all frontend-6. LIMIT: `sampleDefHash` is a sound-build GUID, not
  a `.wav`-name hash, so event→wav can't be closed from adb+bnk alone (needs the sound-project db /
  a runtime resolver capture). Recommendation left for `cook_gui_audio.py` (do not auto-edit).
- Options page population/apply path (native owners for Resolution/AA).

**Reusable RE tooling added this pass:** `tools/ghidra_label/ClearNoReturn.java` (unconditionally clears
the `noReturn` flag on `savegprlr`/`restgprlr` register-save helper thunks that Ghidra mis-bounds to a
single `std`/`ld` and mis-flags noReturn, which **truncates every caller's decomp**) + `ReformDecomp.java`
(remove/clear/disasm/recreate a function over an explicit range, default naming). Recipe for a truncated
frontend function `F` calling helper `H`: `ClearNoReturn.java 0xH` then `ReformDecomp.java 0xF_start
0xF_end` (write mode, i.e. **no `-readOnly`**). This is how `823C0D90` was recovered (`H=0x82ca93e8`).
