#!/usr/bin/env python3
"""Cook the child hero's runtime skeletal-animation package (.heroanim) for Fable2Native.

The runtime AnimationPlayer (include/f2/native_animation.h) plays clips of per-frame, per-bone
BAKED skin matrices over bind-pose skinned vertices. This offline cooker produces exactly that
from the user's OWN base-game data: it loads the hero MDL (skeleton + skinned geoms with bone
ids/weights) and the animation bank, then for each locomotion clip (idle/walk/run) bakes the
per-frame per-bone skin matrix S_i = invBind_i . world_i via the VALIDATED fable_pose pipeline
(anim_pose_re.txt §6, bit-exact vs the AssetBrowser). No guessing: the codec/retarget/LBS are the
validated tool; this only re-emits its intermediate skin matrices + the bind vertices.

CONVENTION (critical): the runtime skinner (native_animation.cpp transform_point) applies a 3x4
row-major matrix RM as out = RM . [p;1] (column-vector), whereas fable_pose builds a 4x4 S and
applies out = [p;1]_row @ S. So RM[r][c] = S[c][r] -> RM = S.T[:3]. We emit S.T[:3] (12 floats).

Ships NOTHING copyrighted: reads the user's extracted game dir, writes a user-local .heroanim
(bind vertices + baked clips + a reference idle pose for the runtime convention self-test).

Output (.heroanim, little-endian):
  'F2HA' u32; version u32=1; bone_count u32;
  clip_count u32; per clip { hash u32; frame_count u32; fps f32; root_speed f32;
                             f32[frame_count*bone_count*12] skin (S.T[:3] per bone per frame) }
  geom_count u32; per geom { vcount u32; per vertex { f32[3] bind_pos; u16[4] bones; f32[4] wts } }
  ref_geom_count u32; per geom { vcount u32; f32[vcount*3] idle@0 posed positions }  (self-test)
"""

from __future__ import annotations

import argparse
import math
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

# Locate the AssetBrowser addons (fable_mdl_format / fable_anim_format) + this dir (fable_pose /
# cook_levels helpers) the same way cook_levels.py does.
_ADDONS = Path(__file__).resolve().parents[2] / "Fable2AssetBrowser" / "source" / "addons"
sys.path.insert(0, str(_ADDONS))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import fable_mdl_format as mdl  # noqa: E402
import fable_anim_format as A  # noqa: E402
import fable_pose  # noqa: E402
from cook_levels import _bnk_name_index, _resolve  # noqa: E402

# Locomotion clips, identified DATA-BACKED from the anim bank itself (no name-hash guessing).
#
# The bank keys clips by an opaque "id_<HASH>" with no source names, BUT each clip carries authored
# EVENTS (fable_anim_format TOC string table). A clip's PURPOSE is read from those events:
#   * idle id_1B78A889 = fable_pose.DEFAULT_IDLE_CLIP: 0 events, a held-standing idle that retargets
#     100% by name to the hero rig (anim_pose_re §5).
#   * walk id_AD8C7C90 = carries FootstepLeftWalk/FootstepRightWalk events (only footstep events)
#     and decodes to perfectly straight forward root motion (net/path = 1.0) — a genuine forward
#     WALK, proven from the data, matching the child-hero rig (96% track coverage).
# (The earlier id_02EE1AA7/id_8C7D7F7E were root-speed GUESSES and were wrong: their events are
#  SE_BANDIT_PAIN / SE_COLLISION;BODYROLL — a hit-react and a dodge-roll. Removed.)
#   * run id_12457E19 = carries FOOT_PLANT LEFT/RIGHT_FOOT_DOWN (both-feet foot-contact) events,
#     only-locomotion events, straight forward root motion at ~2.35 wu/s (25-frame stride) matching
#     the hero rig — a genuine forward RUN (the human run gait is marked by FOOT_PLANT, not a
#     "FootstepRun" event; found via the same data-backed method — tools/anim_clip_events.py).
#
# ★ DEFINITIVE (verified byte-for-byte from globals.gdb, not events/root-motion inference): a
# locomotion slot NAME resolves to a bank key0 via the GDB — fnv1(slotName) is a GDB FIELD-NAME
# hash on the creature's anim-set record, and that type-4 field's raw u32 value IS the bank key0
# (AnimBank.cpp scan_gdb_animation_fields). The hero-human anim set = GDB record 0x576283C7:
#   Idle -> id_4B706EF5, Walk -> id_49220AA3, Run -> id_4AB9BC89 (also Jog id_80D8CCD6, Sprint
#   id_D29B2DD4, WalkFootstep id_EE74243E). The earlier root-motion picks above were STILL wrong.
CLIPS = ["id_4B706EF5", "id_49220AA3", "id_4AB9BC89"]  # idle, walk, run (GDB-resolved)


def bake_clip_frame(info, data_file, clip, frame, inv_bind, lk):
    """Per-bone runtime 3x4 skin matrices (S.T[:3]) for one clip frame. Mirrors fable_pose.
    pose_skinned_mesh's skin build (lines 108-126) but returns the matrices, transposed to the
    runtime column-vector convention."""
    dec = A.AnimDecoder(data_file, log=lambda *a: None)
    h = data_file.parse_clip_header(clip)
    pose = dec.sample_frame(clip, frame)
    tc = min(h.bone_count, len(pose.bone_quats) // 4)
    t2m = fable_pose._track_to_model(clip, info, lk, tc)

    aq = [None] * info.bone_count
    at = [None] * info.bone_count
    for tr in range(tc):
        mb = t2m[tr]
        if mb < 0:
            continue
        aq[mb] = pose.bone_quats[tr * 4:tr * 4 + 4]
        # IN-PLACE bake: deliberately do NOT apply the root bone's translation
        # (pose.bone_trans for parent_id<0). The retail hero locomotion clips carry forward ROOT
        # MOTION (walk/run translate the root); baking it in would make the skinned mesh drift
        # forward then snap back on loop. Instead the root translation is CONSUMED as world motion
        # by the character controller (measured_root_speed -> the hero's move speed), so the clip
        # cycles in place and the body moves — feet track the ground (anim_runtime_sampler §C).

    locals_ = [fable_pose._lmat(info.bone_transforms[i], aq[i], at[i]) for i in range(info.bone_count)]
    W = fable_pose._worlds(info, locals_)
    out = []
    for i in range(info.bone_count):
        S = inv_bind[i] @ W[i]              # 4x4, row-vector convention (p_row @ S)
        out.append(np.asarray(S).T[:3].reshape(-1))  # -> runtime 3x4 (RM[r][c]=S[c][r])
    return out  # bone_count lists of 12 floats


def cook(header_bnk: Path, body_bnk: Path, hero_model: str, f2tool: Path, out_path: Path):
    clips_toc, data_file = fable_pose.load_anim_bank(header_bnk.parent.parent)

    hidx = _bnk_name_index(header_bnk)
    gidx = _bnk_name_index(body_bnk)
    he = _resolve(hidx, hero_model)
    be = _resolve(gidx, hero_model)
    if not (he and be):
        raise SystemExit(f"hero model not in banks: {hero_model}")
    tmp = Path(tempfile.mkdtemp(prefix="f2heroanim_"))

    def extract(bnk: Path, exact: str, tag: str) -> bytes:
        dst = tmp / tag
        subprocess.run([str(f2tool), "extract", str(bnk), exact, str(dst)], check=True,
                       capture_output=True)
        return dst.read_bytes()

    glued = extract(header_bnk, he, "h.bin") + extract(body_bnk, be, "b.bin")
    info, geoms = mdl.parse(glued, log=lambda m: None, file_path=hero_model)
    geoms = geoms or []
    inv_bind = fable_pose.build_inv_bind(info)
    lk = fable_pose._build_lookup(info)

    # Resolve the target clips present in the bank.
    by_name = {c.name: c for c in clips_toc}
    dec = A.AnimDecoder(data_file, log=lambda *a: None)

    def measured_root_speed(clip, h):
        """Net root translation / duration (wu/s) — DATA-derived from the clip's own root-track
        trajectory. Uses the full 3D net displacement (the retail hero locomotion clips translate
        the root along anim-Y = forward, NOT X/Z), which is the speed the controller consumes as
        world motion for the in-place-baked clip (anim_runtime_sampler_re.txt §B metric)."""
        pts = []
        for f in range(h.frame_count):
            p = dec.sample_frame(clip, f)
            if not p.ok or len(p.bone_trans) < 3:
                return 0.0
            pts.append(p.bone_trans[0:3])
        if len(pts) < 2:
            return 0.0
        d = [pts[-1][k] - pts[0][k] for k in range(3)]
        net = math.sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2])
        dur = h.frame_count / (clip.fps or 30.0)
        return net / dur if dur > 0 else 0.0

    baked = []
    for name in CLIPS:
        clip = by_name.get(name)
        if clip is None:
            print(f"  clip missing: {name} (skipped)", file=sys.stderr)
            continue
        h = data_file.parse_clip_header(clip)
        frame_count = max(1, h.frame_count)
        hash_val = int(name[3:], 16)  # "id_1B78A889" -> 0x1B78A889
        root_speed = measured_root_speed(clip, h)
        frames = [bake_clip_frame(info, data_file, clip, f, inv_bind, lk) for f in range(frame_count)]
        baked.append((hash_val, frame_count, float(clip.fps or 30.0), root_speed, frames))
        print(f"  clip {name}: {frame_count} frames @ {clip.fps:.1f}fps, root_speed {root_speed:.3f}")

    if not baked:
        raise SystemExit("no locomotion clips baked")

    # Reference: idle@0 posed positions per geom (runtime convention self-test).
    idle_clip = by_name.get(CLIPS[0])  # CLIPS[0] = the idle clip id
    ref = []
    for g in geoms:
        posed = fable_pose.pose_skinned_mesh(info, data_file, idle_clip, 0, g, inv_bind, lk) \
            if (getattr(g, "skinned", False) and g.bone_ids and g.bone_weights) else None
        ref.append(posed if posed else list(np.asarray(g.positions, float).reshape(-1)))

    # ---- write the package ----
    buf = bytearray()
    buf += b"F2HA"
    buf += struct.pack("<II", 1, info.bone_count)
    buf += struct.pack("<I", len(baked))
    for hash_val, frame_count, fps, root_speed, frames in baked:
        buf += struct.pack("<IIff", hash_val, frame_count, fps, root_speed)
        for fr in frames:                     # frame_count * bone_count * 12 floats
            for m in fr:
                buf += struct.pack("<12f", *[float(x) for x in m])
    buf += struct.pack("<I", len(geoms))
    for g in geoms:
        pos = np.asarray(g.positions, float).reshape(-1, 3)
        vcount = len(pos)
        skinned = getattr(g, "skinned", False) and g.bone_ids and g.bone_weights
        bid = np.asarray(g.bone_ids).reshape(-1, 4) if skinned else np.zeros((vcount, 4), int)
        bw = np.asarray(g.bone_weights, float).reshape(-1, 4) if skinned else np.zeros((vcount, 4))
        buf += struct.pack("<I", vcount)
        for v in range(vcount):
            buf += struct.pack("<3f", pos[v, 0], pos[v, 1], pos[v, 2])
            buf += struct.pack("<4H", *[int(bid[v, k]) & 0xFFFF for k in range(4)])
            buf += struct.pack("<4f", *[float(bw[v, k]) for k in range(4)])
    buf += struct.pack("<I", len(ref))
    for posed in ref:
        arr = list(posed)
        buf += struct.pack("<I", len(arr) // 3)
        buf += struct.pack("<%df" % len(arr), *[float(x) for x in arr])

    out_path.write_bytes(buf)
    print(f"wrote {out_path} ({len(buf)} bytes): {info.bone_count} bones, {len(baked)} clips, "
          f"{len(geoms)} geoms")


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("game_root", type=Path, help="user's extracted game dir (holds data/Globals + data/fable2_anims.*)")
    p.add_argument("-o", "--out", type=Path, required=True, help="output .heroanim path")
    p.add_argument("--header-bnk", type=Path, default=None)
    p.add_argument("--body-bnk", type=Path, default=None)
    p.add_argument("--hero-model",
                   default=r"Art\Characters\Heros\Child Male\dotXSI\CH_HeroChild_Male\CH_HeroChild_Male.mdl")
    p.add_argument("--f2tool", type=Path,
                   default=Path(__file__).resolve().parents[2] / "Fable2AssetBrowser" / "source" / "build" / "f2tool.exe")
    return p


def main(argv=None) -> None:
    args = build_parser().parse_args(argv)
    header = args.header_bnk or (args.game_root / "data" / "Globals" / "globals_model_headers.bnk")
    body = args.body_bnk or (args.game_root / "data" / "Globals" / "globals_models.bnk")
    cook(header, body, args.hero_model, args.f2tool, args.out)


if __name__ == "__main__":
    main()
