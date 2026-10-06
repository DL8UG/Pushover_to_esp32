#!/usr/bin/env python3
"""Turn the simulator's 1-bit PBM screens into e-paper looking PNGs.

Usage: sim_png.py <dir with *.pbm> <overview png>

Writes <name>.png (3x, one per screen) next to each PBM and an overview
of the screens listed in OVERVIEW. Python standard library only.
"""

import struct
import sys
import zlib
from pathlib import Path

SCALE = 3
PAPER = (232, 230, 223)  # e-paper white is a light warm grey
INK = (34, 37, 42)
BEZEL = (58, 60, 64)
BACKGROUND = (246, 246, 244)

OVERVIEW = [
    "home-many", "home-empty", "home-not-connected",
    "msg-short", "msg-umlauts", "msg-emergency",
]
OVERVIEW_COLS = 3
OVERVIEW_SCALE = 2
BORDER = 10  # bezel around each screen, in output pixels
GAP = 24


def read_pbm(path):
    data = path.read_bytes()
    parts = data.split(maxsplit=3)
    if parts[0] != b"P4":
        raise ValueError(f"{path}: not a binary PBM")
    w, h = int(parts[1]), int(parts[2])
    raw = parts[3]
    stride = (w + 7) // 8
    return w, h, [[bool(raw[y * stride + x // 8] & (0x80 >> (x % 8))) for x in range(w)]
                  for y in range(h)]


def write_png(path, w, h, rows):
    """rows: list of h bytearrays with w RGB pixels each."""
    raw = b"".join(b"\0" + bytes(r) for r in rows)

    def chunk(tag, body):
        return (struct.pack(">I", len(body)) + tag + body
                + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF))

    path.write_bytes(b"\x89PNG\r\n\x1a\n"
                     + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(raw, 9))
                     + chunk(b"IEND", b""))


def screen_rows(pixels, scale, border):
    """Scaled screen with a dark bezel, as RGB rows."""
    h, w = len(pixels), len(pixels[0])
    out_w = w * scale + 2 * border
    rows = [bytearray(BEZEL * out_w) for _ in range(border)]
    for line in pixels:
        row = bytearray(BEZEL * border)
        for px in line:
            row += bytearray((INK if px else PAPER) * scale)
        row += bytearray(BEZEL * border)
        rows += [bytearray(row) for _ in range(scale)]
    rows += [bytearray(BEZEL * out_w) for _ in range(border)]
    return rows


def main():
    src, overview = Path(sys.argv[1]), Path(sys.argv[2])
    screens = {}
    for pbm in sorted(src.glob("*.pbm")):
        _, _, pixels = read_pbm(pbm)
        screens[pbm.stem] = pixels
        rows = screen_rows(pixels, SCALE, 0)
        write_png(pbm.with_suffix(".png"), len(rows[0]) // 3, len(rows), rows)

    tiles = [screen_rows(screens[n], OVERVIEW_SCALE, BORDER) for n in OVERVIEW]
    tw, th = len(tiles[0][0]) // 3, len(tiles[0])
    n_rows = (len(tiles) + OVERVIEW_COLS - 1) // OVERVIEW_COLS
    out_w = OVERVIEW_COLS * tw + (OVERVIEW_COLS + 1) * GAP
    out_h = n_rows * th + (n_rows + 1) * GAP
    canvas = [bytearray(BACKGROUND * out_w) for _ in range(out_h)]
    for i, tile in enumerate(tiles):
        x0 = GAP + (i % OVERVIEW_COLS) * (tw + GAP)
        y0 = GAP + (i // OVERVIEW_COLS) * (th + GAP)
        for y, row in enumerate(tile):
            canvas[y0 + y][x0 * 3:(x0 + tw) * 3] = row
    overview.parent.mkdir(parents=True, exist_ok=True)
    write_png(overview, out_w, out_h, canvas)
    print(f"wrote {len(screens)} PNGs to {src}, overview {overview}")


if __name__ == "__main__":
    main()
