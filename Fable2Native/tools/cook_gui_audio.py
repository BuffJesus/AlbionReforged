#!/usr/bin/env python3
"""Cook Fable II frontend GUI sound events into a native PC audio package.

The runtime NEVER ships a sound. This offline cooker reads the *user's own* base-game audio
bank (`data/audio/gui.bnk`) and produces a user-local package of decoded PCM WAVs, one per
retail SE_GUI_* frontend event (docs/RETAIL_FRONTEND_SPEC.md §8), plus an `audio_manifest.ini`
that `NativeFrontendAudio` (Fable2Native/src/native_audio.cpp) consumes by event name.

Pipeline (all proven against the retail bank):
  gui.bnk  --bnk_reader-->  named entry (Xbox 'xma\\0' + RIFF/XMA2, big-endian header fields)
           --strip xma\\0 + rebuild canonical little-endian XMA2WAVEFORMATEX-->  ffmpeg (xma2)
           -->  PCM s16le WAV  -->  <output_root>/audio/<SE_GUI_EVENT>.wav

The event -> bank-wav association is evidence-based: the fired events come from the frontend
decompilation, and the bank entries are the game's own named GUI wavs. (The gui.adb binary
event->sample linkage is a separate RE task; the audio bytes here are already fully base-game.)
"""

from __future__ import annotations

import argparse
import struct
import subprocess
import sys
from pathlib import Path

# Reuse the AssetBrowser's proven BNK reader (the libf2data format boundary) rather than
# re-implementing the container.
_ASSET_BROWSER_ARCHIVE = (
    Path(__file__).resolve().parents[2] / "Fable2AssetBrowser" / "source" / "Archive"
)


# Retail frontend SE_GUI event -> its GUI bank wav (base-game entry name inside gui.bnk).
# Keys match NativeFrontendSound::se_gui_event_name(); values are gui.bnk entry basenames.
EVENT_TO_BANK_WAV = {
    "SE_GUI_SLIDE_MENU_UP": "guislide-scroll_01_alt.wav",
    "SE_GUI_SLIDE_MENU_DOWN": "guislide-scroll_01_alt.wav",
    "SE_GUI_SELECTION_LEFT": "guislide-scroll_01_alt.wav",
    "SE_GUI_SELECTION_RIGHT": "guislide-scroll_01_alt.wav",
    "SE_GUI_MENU_BOX_SELECT": "guiclick-scroll_01.wav",
    "SE_GUI_MENU_BOX_CANCEL": "guiexitsubmenu_01_alt.wav",
}

# Frontend music (retail event MUSIC_MENU) — a real base-game asset in data/audio/music.bnk.
# manifest_key -> (music.bnk entry basename, cooked output filename). The runtime's Music slot
# default is menu_interlude.wav (native_audio.cpp).
MUSIC_TRACKS = {
    "music": ("menu_interlude.wav", "menu_interlude.wav"),
}

XMA2_SAMPLE_RATE = 48000  # Xbox 360 GUI banks are 48 kHz (retail fmt field reads ~47984).


def rebuild_xma2_riff(entry_bytes: bytes) -> bytes:
    """Turn a gui.bnk 'xma\\0'-prefixed Xbox WAV into an ffmpeg-decodable RIFF/XMA2 file.

    The Xbox file has an 'xma\\0' magic prefix and a fmt/XMA2 header whose fields ffmpeg's LE
    wav demuxer cannot init from (0 channels). We strip the prefix, read channels + the XMA2
    stream params, and emit a canonical little-endian XMA2WAVEFORMATEX 'fmt ' chunk + 'data'.
    """
    body = entry_bytes[4:] if entry_bytes[:4] == b"xma\x00" else entry_bytes
    fi = body.find(b"fmt ")
    if fi < 0:
        raise ValueError("no 'fmt ' chunk")
    # tag/channels are stored little-endian and read correctly (tag 0x0166, channels e.g. 2).
    channels = struct.unpack_from("<H", body, fi + 8 + 2)[0] or 2

    samples_encoded = 0
    bytes_per_block = 0x8000
    xi = body.find(b"XMA2", fi + 4)
    if xi >= 0:
        chunk = body[xi + 8:]
        # Xbox 'XMA2' legacy chunk is big-endian; SamplesEncoded @+8, BytesPerBlock @+24.
        if len(chunk) >= 12:
            samples_encoded = struct.unpack_from(">I", chunk, 8)[0]
        if len(chunk) >= 28:
            bytes_per_block = struct.unpack_from(">I", chunk, 24)[0] or 0x8000

    di = body.find(b"data")
    if di < 0:
        raise ValueError("no 'data' chunk")
    data_size = struct.unpack_from("<I", body, di + 4)[0]
    packets = body[di + 8: di + 8 + data_size]

    # Canonical little-endian XMA2WAVEFORMATEX (0x34-byte body) ffmpeg's xma2 decoder accepts.
    fmt = struct.pack(
        "<HHIIHHH",
        0x0166, channels, XMA2_SAMPLE_RATE, XMA2_SAMPLE_RATE * channels * 2,
        4, 16, 0x22,
    )
    fmt += struct.pack("<HI", 1, 3 if channels >= 2 else 1)  # NumStreams, ChannelMask
    fmt += struct.pack("<IIII", samples_encoded, bytes_per_block, 0, samples_encoded)
    fmt += struct.pack("<II", 0, 0)             # LoopBegin, LoopLength
    fmt += struct.pack("<BBH", 0, 4, 1)         # LoopCount, EncoderVersion, BlockCount
    inner = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt \
        + b"data" + struct.pack("<I", len(packets)) + packets
    return b"RIFF" + struct.pack("<I", len(inner)) + inner


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("game_dir", type=Path,
                        help="extracted Fable II game dir (or its 'data' dir); locates audio/gui.bnk")
    parser.add_argument("output_root", type=Path,
                        help="native package directory receiving native_audio/")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--archive", type=Path, default=_ASSET_BROWSER_ARCHIVE,
                        help="AssetBrowser Archive dir providing bnk_reader.py")
    args = parser.parse_args()

    # Accept a game root (<dir>/data/audio/gui.bnk) or a data dir (<dir>/audio/gui.bnk).
    candidates = [args.game_dir / "data" / "audio" / "gui.bnk",
                  args.game_dir / "audio" / "gui.bnk"]
    gui_bnk = next((p for p in candidates if p.is_file()), None)
    if gui_bnk is None:
        parser.error(f"missing base-game GUI bank; looked for: {', '.join(map(str, candidates))}")
    sys.path.insert(0, str(args.archive))
    try:
        import bnk_reader  # noqa: E402  (path set above)
    except ImportError as exc:
        parser.error(f"could not import bnk_reader from {args.archive}: {exc}")

    # native_audio/ matches the runtime's default audio-root (<ui-root>/native_audio).
    audio_dir = args.output_root / "native_audio"
    audio_dir.mkdir(parents=True, exist_ok=True)

    tmp = audio_dir / "_tmp_xma.wav"
    manifest: list[tuple[str, str]] = []

    def decode_from_bank(reader, entries, bank_wav: str, out_wav: Path) -> str | None:
        """Extract a named xma\\0 entry, decode to PCM WAV. Returns None on success, else an error."""
        entry_name = entries.get(bank_wav)
        if entry_name is None:
            return f"'{bank_wav}' not in bank"
        reader.extract_file(entry_name, str(tmp))
        try:
            riff = rebuild_xma2_riff(tmp.read_bytes())
        except Exception as exc:  # malformed header
            return f"header rebuild failed: {exc}"
        riff_path = audio_dir / (out_wav.stem + ".xma.wav")
        riff_path.write_bytes(riff)
        result = subprocess.run(
            [args.ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
             "-i", str(riff_path), "-acodec", "pcm_s16le", str(out_wav)],
            capture_output=True, text=True,
        )
        riff_path.unlink(missing_ok=True)
        if result.returncode != 0 or not out_wav.is_file():
            return f"ffmpeg xma2 decode failed: {result.stderr.strip()}"
        return None

    def cook_bank(bank_path: Path, jobs, label: str) -> int:
        """jobs: list of (manifest_key, bank_wav_basename, out_basename)."""
        reader = bnk_reader.BNKReader(str(bank_path))
        entries = {Path(e.name.replace("\\", "/")).name: e.name for e in reader.file_entries}
        done = 0
        for key, bank_wav, out_name in jobs:
            out_wav = audio_dir / out_name
            err = decode_from_bank(reader, entries, bank_wav, out_wav)
            if err:
                print(f"  SKIP {key}: {err}")
                continue
            manifest.append((key, out_wav.name))
            done += 1
            print(f"  OK   {key} <- {label}:{bank_wav} -> {out_wav.name} ({out_wav.stat().st_size} B)")
        reader.close()
        return done

    # Frontend SFX from gui.bnk.
    sfx_jobs = [(ev, wav, f"{ev}.wav") for ev, wav in EVENT_TO_BANK_WAV.items()]
    total = len(sfx_jobs)
    cooked = cook_bank(gui_bnk, sfx_jobs, "gui.bnk")

    # Frontend music from music.bnk (optional — skip cleanly if that bank isn't present).
    music_bnk = gui_bnk.parent / "music.bnk"
    if music_bnk.is_file():
        music_jobs = [(key, bank_wav, out_name) for key, (bank_wav, out_name) in MUSIC_TRACKS.items()]
        total += len(music_jobs)
        cooked += cook_bank(music_bnk, music_jobs, "music.bnk")
    else:
        print(f"  note: {music_bnk.name} not found next to gui.bnk; frontend music not cooked")

    tmp.unlink(missing_ok=True)

    # Flat key=value manifest matching native_audio.cpp's parser (SE_GUI_* / music keys accepted directly).
    lines = ["# Cooked from the user's own data/audio/{gui,music}.bnk by cook_gui_audio.py — no shipped assets.",
             "# key = SE_GUI event name or 'music' (native_audio.cpp resolves these); value = cooked wav."]
    for key, name in manifest:
        lines.append(f"{key} = {name}")
    (audio_dir / "audio_manifest.ini").write_text("\n".join(lines) + "\n", encoding="utf-8")

    print(f"cooked {cooked}/{total} frontend audio assets into {audio_dir}")
    return 0 if cooked else 1


if __name__ == "__main__":
    raise SystemExit(main())
