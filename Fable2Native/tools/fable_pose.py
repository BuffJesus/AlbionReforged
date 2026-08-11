"""Bake a Fable II idle-animation pose into a skinned MDL's vertices (linear-blend skinning)
at cook time, so cooked characters stand naturally instead of in the raw bind (A-)pose.

Verbatim port of ghidra_out/anim_pose_re.txt §6 — validated bit-exact against the AssetBrowser
display skinner (ModelPreview.cpp): row-vector matrices (DirectXMath), L = S*R*T, world =
L*world_parent, invBind = inverse(restWorld), skin = invBind*world, p' = p_bind * sum(w_k*skin_k).
Anim overrides: quaternion ABSOLUTE for all bones, translation ROOT-only (AnimPlayer semantics).

Depends on numpy + fable_anim_format (the AssetBrowser addon cook_levels already puts on sys.path).
"""
import math

import numpy as np
import fable_anim_format as A

# The recommended child/human idle: a 143-bone held standing idle, ~1° whole-clip drift,
# retargets 100% by name to the hero skeleton (anim_pose_re.txt §5).
DEFAULT_IDLE_CLIP = "id_1B78A889"


def load_anim_bank(data_dir):
    """Load (clips, AnimDataFile) from <data_dir>/fable2_anims.animation_{toc,data}."""
    from pathlib import Path
    d = Path(data_dir)
    toc = (d / "fable2_anims.animation_toc").read_bytes()
    dat = (d / "fable2_anims.animation_data").read_bytes()
    clips = A.load_toc_bytes(toc, log=lambda *a: None)
    df = A.AnimDataFile(dat)
    return clips, df


def _qmat(q):
    x, y, z, w = q
    n = math.sqrt(x * x + y * y + z * z + w * w) or 1.0
    x, y, z, w = x / n, y / n, z / n, w / n
    return np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w), 0.0],
        [2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w), 0.0],
        [2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y), 0.0],
        [0.0, 0.0, 0.0, 1.0]])


def _lmat(tf, qover=None, tadd=None):
    q = tf[0:4] if qover is None else qover
    t = list(tf[4:7])
    s = tf[7:10]
    if tadd is not None:
        t = [t[0] + tadd[0], t[1] + tadd[1], t[2] + tadd[2]]
    S = np.diag([s[0], s[1], s[2], 1.0])
    R = _qmat(q)
    T = np.eye(4)
    T[3, 0:3] = t
    return S @ R @ T


def _worlds(info, locals_):
    n = info.bone_count
    W = [None] * n

    def solve(i):
        if W[i] is not None:
            return W[i]
        p = info.bones[i].parent_id
        W[i] = locals_[i] if (p < 0 or p >= n) else locals_[i] @ solve(p)
        return W[i]

    for i in range(n):
        solve(i)
    return W


def build_inv_bind(info):
    restW = _worlds(info, [_lmat(info.bone_transforms[i]) for i in range(info.bone_count)])
    return [np.linalg.inv(restW[i]) for i in range(info.bone_count)]


def _build_lookup(info):
    lk = {}
    for i, b in enumerate(info.bones):
        nm = A.normalise_bone_name(b.name)
        base = nm[7:] if nm.startswith("shadow_") else nm
        for k in (nm, base, "shadow_" + base):
            lk.setdefault(k, []).append(i)
    return lk


def _track_to_model(clip, info, lk, tc):
    res = [-1] * tc
    tm = clip.track_map
    if not tm:
        for i in range(min(tc, info.bone_count)):
            res[i] = i
        return res
    for i in range(min(tc, len(tm))):
        nm = A.normalise_bone_name(tm[i].name)
        if nm in ("", "locator"):
            continue
        base = nm[7:] if nm.startswith("shadow_") else nm
        for k in (nm, base, "shadow_" + base):
            if k in lk:
                res[i] = lk[k][0]
                break
    return res


def pose_skinned_mesh(info, data_file, clip, frame, geom, inv_bind, lk):
    """Return posed positions[flat xyz] for one skinned geom, LBS-baked to clip@frame."""
    dec = A.AnimDecoder(data_file, log=lambda *a: None)
    h = data_file.parse_clip_header(clip)
    pose = dec.sample_frame(clip, frame)
    tc = min(h.bone_count, len(pose.bone_quats) // 4)
    t2m = _track_to_model(clip, info, lk, tc)

    aq = [None] * info.bone_count
    at = [None] * info.bone_count
    for tr in range(tc):
        mb = t2m[tr]
        if mb < 0:
            continue
        aq[mb] = pose.bone_quats[tr * 4:tr * 4 + 4]
        if info.bones[mb].parent_id < 0:
            at[mb] = pose.bone_trans[tr * 3:tr * 3 + 3]

    locals_ = [_lmat(info.bone_transforms[i], aq[i], at[i]) for i in range(info.bone_count)]
    W = _worlds(info, locals_)
    skin = [inv_bind[i] @ W[i] for i in range(info.bone_count)]

    P = np.asarray(geom.positions, float).reshape(-1, 3)
    bid = np.asarray(geom.bone_ids).reshape(-1, 4)
    bw = np.asarray(geom.bone_weights, float).reshape(-1, 4)
    outP = np.zeros_like(P)
    for v in range(len(P)):
        M = np.zeros((4, 4))
        ws = 0.0
        for k in range(4):
            w = bw[v, k]
            if w <= 0.0:
                continue
            bidx = int(bid[v, k])
            if bidx >= info.bone_count:
                continue
            M += skin[bidx] * w
            ws += w
        if ws < 1e-4:
            M = np.eye(4)
        outP[v] = (np.array([P[v, 0], P[v, 1], P[v, 2], 1.0]) @ M)[0:3]
    return outP.reshape(-1).tolist()


def make_poser(info, clips, data_file, clip_name=DEFAULT_IDLE_CLIP, frame=0):
    """Return a callable pose(geom)->positions[flat xyz], or None if the clip/skeleton is
    unusable (caller falls back to the bind pose). Precomputes invBind + the name lookup once."""
    if not getattr(info, "bones", None) or not getattr(info, "bone_transforms", None):
        return None
    if len(info.bone_transforms) != info.bone_count:
        return None
    clip = next((c for c in clips if c.name == clip_name), None)
    if clip is None:
        return None
    inv_bind = build_inv_bind(info)
    lk = _build_lookup(info)

    def pose(geom):
        if not getattr(geom, "skinned", False) or not geom.bone_ids or not geom.bone_weights:
            return None
        return pose_skinned_mesh(info, data_file, clip, frame, geom, inv_bind, lk)

    return pose
