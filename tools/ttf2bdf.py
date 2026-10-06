#!/usr/bin/env python3
"""Rasterise characters of a TrueType font into a 1-bit BDF file.

Used once for the big digits on the home screen (no BDF font that large
and that bold exists); the result is committed in fonts/ and then goes
through tools/fontgen.py like every other font. Needs Pillow:

    uv run --with pillow tools/ttf2bdf.py \
        /usr/share/fonts/liberation/LiberationSans-Bold.ttf 72 "0123456789+" \
        fonts/liberation-sans-bold-72-digits.bdf
"""

import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

THRESHOLD = 128  # grey level from which a pixel counts as ink


def main():
    ttf, size, chars, out = sys.argv[1], int(sys.argv[2]), sys.argv[3], Path(sys.argv[4])
    font = ImageFont.truetype(ttf, size)
    ascent, descent = font.getmetrics()
    glyphs = []
    for ch in chars:
        # Draw with the baseline at y = ascent on a generous canvas.
        img = Image.new("L", (size * 2, ascent + descent + size), 0)
        ImageDraw.Draw(img).text((size // 2, 0), ch, font=font, fill=255)
        bw = img.point(lambda v: 255 if v >= THRESHOLD else 0)
        bbox = bw.getbbox()
        advance = round(font.getlength(ch))
        if bbox is None:
            glyphs.append((ch, advance, 0, 0, 0, 0, []))
            continue
        x0, y0, x1, y1 = bbox
        rows = []
        for y in range(y0, y1):
            bits = "".join("1" if bw.getpixel((x, y)) else "0" for x in range(x0, x1))
            bits = bits.ljust((len(bits) + 7) // 8 * 8, "0")
            rows.append("".join(f"{int(bits[i:i + 8], 2):02X}" for i in range(0, len(bits), 8)))
        # BDF: x offset from the pen, y offset of the bitmap bottom from the baseline
        glyphs.append((ch, advance, x1 - x0, y1 - y0, x0 - size // 2, ascent - y1, rows))

    name = Path(ttf).stem
    lines = [
        "STARTFONT 2.1",
        f"FONT {name}-{size}",
        f"SIZE {size} 72 72",
        f"FONTBOUNDINGBOX {size * 2} {ascent + descent} 0 {-descent}",
        f"COMMENT Rasterised from {Path(ttf).name} by tools/ttf2bdf.py",
        "STARTPROPERTIES 2",
        f"FONT_ASCENT {ascent}",
        f"FONT_DESCENT {descent}",
        "ENDPROPERTIES",
        f"CHARS {len(glyphs)}",
    ]
    for ch, adv, w, h, xo, yo, rows in glyphs:
        lines += [
            f"STARTCHAR U+{ord(ch):04X}",
            f"ENCODING {ord(ch)}",
            f"DWIDTH {adv} 0",
            f"BBX {w} {h} {xo} {yo}",
            "BITMAP",
            *rows,
            "ENDCHAR",
        ]
    lines.append("ENDFONT")
    out.write_text("\n".join(lines) + "\n")
    print(f"wrote {out} ({len(glyphs)} glyphs, ascent {ascent}, descent {descent})")


if __name__ == "__main__":
    main()
