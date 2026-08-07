#!/usr/bin/env python3
"""Cook Fable II Bink startup movies into a native PC video sequence."""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
from pathlib import Path


# Clips the native front end references: the two boot logos + the title-idle attract movie.
DEFAULT_CLIPS = (
    "microsoft_logo",
    "lionhead_logo",
    "attract_mode",
)


def duration_seconds(ffprobe: str, source: Path) -> float:
    result = subprocess.run(
        [ffprobe, "-v", "error", "-show_entries", "format=duration",
         "-of", "default=nw=1:nk=1", str(source)],
        check=True,
        capture_output=True,
        text=True,
    )
    return float(result.stdout.strip())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input_root", type=Path,
                        help="directory containing the extracted .bik files")
    parser.add_argument("output_root", type=Path,
                        help="native package directory receiving videos/")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    parser.add_argument("--clip", action="append", dest="clips",
                        help="clip basename; may be repeated (default: startup sequence)")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    ffmpeg = shutil.which(args.ffmpeg)
    ffprobe = shutil.which(args.ffprobe)
    if not ffmpeg or not ffprobe:
        parser.error("ffmpeg and ffprobe are required for video cooking")

    clips = args.clips or list(DEFAULT_CLIPS)
    video_dir = args.output_root / "videos"
    manifest = []
    for clip_id in clips:
        source = args.input_root / f"{clip_id}.bik"
        output = video_dir / f"{clip_id}.mp4"
        if not source.is_file():
            parser.error(f"missing source video: {source}")

        duration = duration_seconds(ffprobe, source)
        manifest.append({
            "id": clip_id,
            "asset": f"videos/{clip_id}.mp4",
            "duration_seconds": duration,
            "skippable": True,
        })
        if args.dry_run:
            continue

        video_dir.mkdir(parents=True, exist_ok=True)
        subprocess.run([
            ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
            "-i", str(source),
            "-c:v", "libx264", "-preset", "medium", "-crf", "18",
            "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "192k",
            str(output),
        ], check=True)

    if not args.dry_run:
        (video_dir / "manifest.json").write_text(
            json.dumps({"version": 1, "clips": manifest}, indent=2) + "\n",
            encoding="utf-8",
        )
    print(json.dumps({"version": 1, "clips": manifest}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
