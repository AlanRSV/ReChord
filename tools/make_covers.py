#!/usr/bin/env python3
"""make_covers.py -- album covers for Now Playing, made on the PC.

    python3 tools/make_covers.py MUSIC_DIR [--force] [--dry-run]

For every folder under MUSIC_DIR that holds audio, writes `cover56.rgb` next to the tracks:
the album's cover cut to a centred square and scaled to 56 x 56, as RGB565 little-endian pixels,
rows top down, exactly 6,272 bytes, no header. Now Playing (firmware/ui/screens.c) draws it at
2x in its 112 px art tile; a folder without one keeps the monogram tile.

The cover comes from an image file in the folder (cover / folder / front / album, .jpg / .jpeg /
.png, any case), else from the first track with an embedded picture (FLAC PICTURE, ID3 APIC, MP4
covr...). Decoding and scaling are ffmpeg's, so ffmpeg must be on PATH (WSL / Linux / macOS).

Existing cover56.rgb files are kept unless --force. Run it again after adding albums, then copy the
folders to the player (or rebuild the simulator's image: sim/mkimage.sh).
"""
import argparse
import os
import shutil
import subprocess
import sys

NAME = "cover56.rgb"
SIZE = 56
BYTES = SIZE * SIZE * 2
AUDIO = {"flac", "wav", "aif", "aiff", "aifc", "mp3", "mp2", "m4a", "m4b", "mp4", "alac", "ogg",
         "oga", "opus", "wv", "dsf", "dff", "ape", "wma", "asf"}
IMAGES = [b + e for b in ("cover", "folder", "front", "album") for e in (".jpg", ".jpeg", ".png")]
# centred square crop, area-averaged downscale, RGB565 out
VF = f"scale={SIZE}:{SIZE}:force_original_aspect_ratio=increase:flags=area,crop={SIZE}:{SIZE},format=rgb565le"


def convert(src, embedded):
    """The cover of `src` as BYTES of RGB565, or None (no picture, or ffmpeg failed)."""
    cmd = ["ffmpeg", "-v", "error", "-nostdin", "-i", src]
    if embedded:
        cmd += ["-map", "0:v:0"]      # the attached picture stream; audio-only files fail here
    cmd += ["-frames:v", "1", "-vf", VF, "-f", "rawvideo", "-"]
    r = subprocess.run(cmd, capture_output=True)
    return r.stdout if r.returncode == 0 and len(r.stdout) == BYTES else None


def folder_cover(folder, files):
    """(pixels, source name) for one album folder, or (None, None)."""
    lower = {f.lower(): f for f in files}
    for name in IMAGES:
        if name in lower:
            px = convert(os.path.join(folder, lower[name]), False)
            if px:
                return px, lower[name]
    for f in sorted(files):
        if f.rsplit(".", 1)[-1].lower() in AUDIO:
            px = convert(os.path.join(folder, f), True)
            if px:
                return px, f
    return None, None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("music_dir")
    ap.add_argument("--force", action="store_true", help=f"remake existing {NAME} files")
    ap.add_argument("--dry-run", action="store_true", help="report, write nothing")
    args = ap.parse_args()
    if not shutil.which("ffmpeg"):
        sys.exit("ffmpeg not found (sudo apt install ffmpeg)")
    if not os.path.isdir(args.music_dir):
        sys.exit(f"{args.music_dir}: not a folder")

    made = kept = none = 0
    for folder, dirs, files in os.walk(args.music_dir):
        dirs[:] = sorted(d for d in dirs if d.upper() != "RECHORD" and not d.startswith("."))
        if not any(f.rsplit(".", 1)[-1].lower() in AUDIO for f in files if "." in f):
            continue
        rel = os.path.relpath(folder, args.music_dir)
        out = os.path.join(folder, NAME)
        if os.path.exists(out) and not args.force:
            kept += 1
            continue
        px, src = folder_cover(folder, files)
        if not px:
            none += 1
            print(f"  no cover   {rel}")
            continue
        if not args.dry_run:
            tmp = out + ".tmp"
            with open(tmp, "wb") as f:
                f.write(px)
            os.replace(tmp, out)
        made += 1
        print(f"  cover      {rel}  (from {src})")
    print(f"{made} made, {kept} kept, {none} folders without a cover"
          + (" (dry run: nothing written)" if args.dry_run else ""))


if __name__ == "__main__":
    main()
