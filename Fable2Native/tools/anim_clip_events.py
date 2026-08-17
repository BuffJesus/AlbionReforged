#!/usr/bin/env python3
"""Identify Fable II animation clips by their AUTHORED EVENTS (data-backed, no name guessing).

The anim bank keys clips by an opaque "id_<HASH>" with no source names, and the hash is NOT a
plain fnv1/crc32 of the semantic name (verified: known names like "Greeting"/"IdleStretch" match
zero TOC hashes). BUT each clip carries authored events (fable_anim_format TOC string table), which
reveal its PURPOSE from the data:
  * a forward WALK carries FootstepLeftWalk / FootstepRightWalk (+ FOOT_PLANT) events,
  * a hit-react carries SE_BANDIT_PAIN etc.; a dodge-roll carries SE_COLLISION;BODYROLL,
  * an idle carries no events (or FootstepLeft/RightIdle).

This is how cook_hero_anim.py's walk clip (id_AD8C7C90) was identified — not guessed.

Usage:
  anim_clip_events.py <game_root> vocab                 # rank all event names by frequency
  anim_clip_events.py <game_root> clip id_AD8C7C90      # dump one clip's events
  anim_clip_events.py <game_root> find FootstepLeftRun  # clips carrying an event (+ hero-rig fit)
"""

from __future__ import annotations

import argparse
import sys
from collections import Counter
from pathlib import Path

_ADDONS = Path(__file__).resolve().parents[2] / "Fable2AssetBrowser" / "source" / "addons"
sys.path.insert(0, str(_ADDONS))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import fable_anim_format as A  # noqa: E402
import fable_pose  # noqa: E402


def _load(game_root: Path):
    toc = (game_root / "data" / "fable2_anims.animation_toc").read_bytes()
    clips = A.load_toc_bytes(toc, log=lambda *a: None)
    df = A.AnimDataFile((game_root / "data" / "fable2_anims.animation_data").read_bytes())
    return clips, df


def cmd_vocab(clips, _df, _args):
    c = Counter(e.name for cl in clips for e in cl.events)
    for name, n in c.most_common(60):
        print(f"{n:6d}  {name}")


def cmd_clip(clips, df, args):
    key = int(args.name[3:], 16) if args.name.lower().startswith("id_") else int(args.name, 16)
    cl = next((c for c in clips if c.key0 == key), None)
    if not cl:
        raise SystemExit(f"clip not found: {args.name}")
    h = df.parse_clip_header(cl)
    print(f"{cl.name}: fps={cl.fps} frames={getattr(h, 'frame_count', '?')} events={len(cl.events)}")
    for e in cl.events:
        print(f"  t={e.time:.3f}  '{e.name}'  param='{e.param}'")


def cmd_find(clips, _df, args):
    hits = [c for c in clips if any(e.name == args.name for e in c.events)]
    print(f"{len(hits)} clips carry event '{args.name}':")
    for c in sorted(hits, key=lambda c: c.toc_frame_count)[:40]:
        evs = sorted({e.name for e in c.events})
        print(f"  {c.name} frames={c.toc_frame_count} fps={c.fps} events={evs[:5]}")


def main(argv=None) -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("game_root", type=Path)
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("vocab").set_defaults(func=cmd_vocab)
    c = sub.add_parser("clip"); c.add_argument("name"); c.set_defaults(func=cmd_clip)
    f = sub.add_parser("find"); f.add_argument("name"); f.set_defaults(func=cmd_find)
    args = p.parse_args(argv)
    clips, df = _load(args.game_root)
    args.func(clips, df, args)


if __name__ == "__main__":
    main()
