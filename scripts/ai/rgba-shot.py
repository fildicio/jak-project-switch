#!/usr/bin/env python3
"""rgba-shot.py (AI-assisted, FIX 115): convert dk_shot_fNNNN.rgba frame dumps
from the Switch deko3d frame recorder into PNGs.

The dump is raw RGBA8, one row of `pitch` bytes (pitch = width*4 for our
framebuffer), stored BOTTOM-UP (deko3d OriginLowerLeft, the GL convention), so we flip
while converting. If a PNG comes out upside down, drop the flip. Two outputs per shot:
  <stem>_rgb.png    color, alpha forced opaque (what the TV shows)
  <stem>_alpha.png  alpha channel as grayscale (dst-alpha diagnosis: the ocean
                    envmap blends DST_ALPHA/ONE, so a wrong alpha field shows
                    up here instantly)

Usage:
  rgba-shot.py <file.rgba> [width] [height] [pitch]
  rgba-shot.py --all <dir> [width] [height] [pitch]     # every dk_shot_*.rgba

Width/height/pitch default to 1280/720/5120 (the dk framebuffer).
Pure stdlib: zlib + struct, no Pillow needed.
"""
import struct
import sys
import zlib
from pathlib import Path


def png_chunk(tag: bytes, data: bytes) -> bytes:
    return (
        struct.pack(">I", len(data))
        + tag
        + data
        + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    )


def write_png(path: Path, w: int, h: int, rows: list[bytes], has_alpha: bool) -> None:
    # rows: top-down, each row is w*(3 or 4) bytes, already filtered with byte 0
    color_type = 6 if has_alpha else 2
    raw = b"".join(b"\x00" + r for r in rows)
    png = (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, color_type, 0, 0, 0))
        + png_chunk(b"IDAT", zlib.compress(raw, 6))
        + png_chunk(b"IEND", b"")
    )
    path.write_bytes(png)


def convert(src: Path, w: int, h: int, pitch: int) -> bool:
    data = src.read_bytes()
    need = pitch * h
    if len(data) < need:
        print(f"{src.name}: too small ({len(data)} B < {need} B) -- skip")
        return False
    rows_rgb = []
    rows_alpha = []
    for y in range(h - 1, -1, -1):  # dump is bottom-up: flip to top-down
        base = y * pitch
        row_rgb = bytearray(w * 3)
        row_a = bytearray(w * 3)
        for x in range(w):
            r, g, b, a = data[base + x * 4 : base + x * 4 + 4]
            i = x * 3
            row_rgb[i] = r
            row_rgb[i + 1] = g
            row_rgb[i + 2] = b
            row_a[i] = a
            row_a[i + 1] = a
            row_a[i + 2] = a
        rows_rgb.append(bytes(row_rgb))
        rows_alpha.append(bytes(row_a))
    write_png(src.with_name(src.stem + "_rgb.png"), w, h, rows_rgb, False)
    write_png(src.with_name(src.stem + "_alpha.png"), w, h, rows_alpha, False)
    print(f"{src.name}: -> {src.stem}_rgb.png + {src.stem}_alpha.png")
    return True


def main() -> int:
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 1
    w, h, pitch = 1280, 720, 1280 * 4
    paths = []
    if args[0] == "--all":
        folder = Path(args[1])
        paths = sorted(folder.glob("dk_shot_*.rgba"))
        rest = args[2:]
    else:
        paths = [Path(args[0])]
        rest = args[1:]
    if len(rest) >= 1:
        w = int(rest[0])
    if len(rest) >= 2:
        h = int(rest[1])
    if len(rest) >= 3:
        pitch = int(rest[2])
    else:
        pitch = w * 4
    ok = 0
    for p in paths:
        ok += 1 if convert(p, w, h, pitch) else 0
    print(f"converted {ok}/{len(paths)}")
    return 0 if ok == len(paths) and paths else 1


if __name__ == "__main__":
    sys.exit(main())
