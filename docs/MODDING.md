# Modding Architecture & the Creation-Kit Vision

**Goal:** a Creation-Kit-style toolchain for Fable II — swap/add models, textures, armours, edit
quests, and create **custom landscapes/terrain** — packaged into mods the PC port loads. Not an exact
CK clone; the same *spirit* (an authoring GUI + a mod format the engine loads), adapted to Fable II.
Reference for the "hook-and-extend-the-tool" pattern: Creation-Kit-Platform-Extended.

## The core principle: BNK/Lua are the on-ramp, modern formats are the destination

The original game content lives in Fable's formats (BNK archives, Lua scripts, proprietary
model/texture/level/terrain formats). Two facts shape everything:

1. **You must read the originals** to load and mod the *existing* game. There's no way around
   decoding BNK/Lua/level formats if you want Fable II (vs. building a new game on its engine).
2. **A decompilation frees you from them as a *constraint*.** Once we control the engine's loaders,
   we can teach it to *also* load modern formats (glTF, PNG, heightmaps, friendlier scripts) for
   new/modified content — subsystem by subsystem, only where it's worth it.

So: **learn the format → build the loader → then add modern alternatives.** You can't replace a
format you don't understand, which is why the AssetBrowser (format decoders) and the decompilation
(engine loaders) are both prerequisites, not detours.

## The layers

1. **Read layer** *(mostly done — Fable2AssetBrowser)*
   Decode models (MDL→FBX/glTF), textures (Lh→PNG/DDS), anims (banks + Havok), BNK archives,
   levels, terrain (`HeightfieldLoader`, `EhfChunkParser`, `TerrainTextureRegistry`), Lua
   (decompiler), audio (XMA2). This is the spec for how the game represents content.

2. **Overlay / injection layer** *(scaffolded)*
   - **Loose-file override:** the recomp's VFS (`Fable2Recomp/src/ModSupport.cpp`) resolves modded
     files before the base game — drop a replacement, it shadows the original. No archive editing.
   - **BNK injection:** `Fable2AssetBrowser` `BnkWriter` can REPLACE or ADD entries inside BNKs, and
     rebuild them — the path for per-entry swaps.
   Together these let mods load without hand-editing originals.

3. **Author layer** *(the CK tool — grow from AssetBrowser)*
   Turn the browser into an editor: import a glTF model / PNG texture and repack; edit item/appearance
   defs; edit quests in Lua (with modern tooling); and — the big one — **level/terrain editing**
   (the AssetBrowser already reads heightfields, terrain textures, and level chunks, and exports to
   Blender via `FableLevelImporter.py`; the work is the robust *write* path).

4. **Engine-bridge layer** *(needs the decompilation — Track B)*
   Extend the decompiled loaders so the engine accepts author-layer formats directly (load a glTF as
   a model, a heightmap as terrain, a modern script for a quest), mapping them onto the internal
   structures the read layer taught us. This is what makes the CK author in modern formats instead
   of proprietary ones.

## Milestone ladder (concrete, incremental)
1. Whole-file asset swap via the VFS overlay (works the moment the game boots).
2. Per-entry BNK injection (BnkWriter) — swap one model/texture inside an archive.
3. New models / armours (author in glTF → convert → inject/override).
4. Custom textures (PNG → Lh/DDS → inject/override).
5. Quest edits (Lua) — with a decompiler + editor.
6. **Custom terrain / landscape** — heightmap + terrain-texture authoring → Fable level/EHF write.
7. Full CK-style GUI tying it together, packaging mods + load order + a manager.

## What's needed from each track
- **AssetBrowser (Track C):** the read layer + the author/editor UI. Building now.
- **Decompilation (Track B):** the engine-bridge layer (extend loaders for modern formats) + deep
  systems (item/appearance/quest/terrain internals) needed for authoring, not just swapping.
- **Recomp (Track A):** a booting game to actually run mods in (currently parked upstream).

## Multiplayer note
A recomp/decomp runs the same x86 code on every player, so the cross-architecture floating-point
determinism problem (the classic emulator-PvP desync) largely disappears — a real point in favor of
building co-op/party MP on this foundation. See ROADMAP Track D.
