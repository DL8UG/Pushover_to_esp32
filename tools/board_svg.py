#!/usr/bin/env python3
"""Draw the board (Waveshare ESP32-S3-ePaper-1.54, front view) as SVG,
with a simulator screen in the display window and the buttons labelled.

Usage: board_svg.py <screen.pbm> <output.svg>

Called by test/sim/Makefile, so the picture always shows the current UI.
Own drawing after the published outline dimensions (mm); 10 units = 1 mm.
Python standard library only.
"""

import sys
from pathlib import Path

# Outline in mm (Waveshare dimension drawing)
CASE_W, CASE_H, CASE_R = 39.8, 53.0, 4.5
WIN = 27.8                  # display window, square
WIN_X = (CASE_W - WIN) / 2  # 6.0
WIN_Y = CASE_H - 14.3 - WIN  # 10.9
BOOT_Y, PWR_Y = 31.3, 41.9  # button centres on the right side
SD_Y0, SD_Y1 = 8.0, 23.0    # micro SD slot on the right side

S = 10  # SVG units per mm
OX, OY = 40, 40  # case origin in the SVG

PAPER = "#e4e2db"
INK = "#24272c"
CASE = "#f6f6f4"
CASE_EDGE = "#c8c8c4"
SHADOW = "#00000014"
TEXT = "#3a3d42"
MUTED = "#7a7d82"
FONT = "font-family='Helvetica, Arial, sans-serif'"


def read_pbm(path):
    data = path.read_bytes()
    parts = data.split(maxsplit=3)
    w, h, raw = int(parts[1]), int(parts[2]), parts[3]
    stride = (w + 7) // 8
    return w, h, lambda x, y: bool(raw[y * stride + x // 8] & (0x80 >> (x % 8)))


def screen_path(w, h, ink):
    """One path of horizontal runs - compact even for 200x200 pixels."""
    d = []
    for y in range(h):
        x = 0
        while x < w:
            if ink(x, y):
                start = x
                while x < w and ink(x, y):
                    x += 1
                d.append(f"M{start} {y}h{x - start}v1h{start - x}z")
            else:
                x += 1
    return "".join(d)


def mm(v):
    return round(v * S, 1)


def main():
    pbm, out = Path(sys.argv[1]), Path(sys.argv[2])
    w, h, ink = read_pbm(pbm)

    cx0, cy0 = OX, OY
    cw, ch = mm(CASE_W), mm(CASE_H)
    right = cx0 + cw
    width, height = cx0 + cw + 470, cy0 + ch + 60

    # active area: 200 px inside the window with a thin border
    active = WIN - 1.2
    scale = mm(active) / w
    sx, sy = cx0 + mm(WIN_X + 0.6), cy0 + mm(WIN_Y + 0.6)

    el = []
    a = el.append
    a(f"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 {width} {height}' "
      f"width='{width}' height='{height}' {FONT}>")
    a("<title>Waveshare ESP32-S3-ePaper-1.54 running Pushover_to_esp32</title>")

    # case: shadow, body, side buttons, USB-C
    a(f"<rect x='{cx0 + 6}' y='{cy0 + 10}' width='{cw}' height='{ch}' rx='{mm(CASE_R)}' fill='{SHADOW}'/>")
    for y in (BOOT_Y, PWR_Y):
        a(f"<rect x='{right - 8}' y='{cy0 + mm(y) - 30}' width='18' height='60' rx='7' "
          f"fill='{CASE}' stroke='{CASE_EDGE}' stroke-width='2'/>")
    a(f"<rect x='{cx0}' y='{cy0}' width='{cw}' height='{ch}' rx='{mm(CASE_R)}' "
      f"fill='{CASE}' stroke='{CASE_EDGE}' stroke-width='2'/>")
    a(f"<rect x='{right - 5}' y='{cy0 + mm(SD_Y0)}' width='4' height='{mm(SD_Y1 - SD_Y0)}' rx='2' fill='{CASE_EDGE}'/>")
    a(f"<rect x='{cx0 + cw / 2 - 45}' y='{cy0 + ch - 4}' width='90' height='8' rx='4' fill='{CASE_EDGE}'/>")

    # display window and screen
    a(f"<rect x='{cx0 + mm(WIN_X)}' y='{cy0 + mm(WIN_Y)}' width='{mm(WIN)}' height='{mm(WIN)}' "
      f"rx='3' fill='{PAPER}' stroke='#9a9a96' stroke-width='3'/>")
    a(f"<path transform='translate({sx} {sy}) scale({scale:.4f})' fill='{INK}' "
      f"shape-rendering='crispEdges' d='{screen_path(w, h, ink)}'/>")

    # labels
    def callout(button_y, text_y, title, lines):
        by, ty = cy0 + mm(button_y), cy0 + mm(text_y)
        x1, x2 = right + 16, right + 70
        a(f"<path d='M{x1} {by}L{x1 + 30} {ty}H{x2}' fill='none' stroke='{MUTED}' stroke-width='2'/>")
        a(f"<circle cx='{x1}' cy='{by}' r='4' fill='{MUTED}'/>")
        a(f"<text x='{x2 + 12}' y='{ty + 9}' font-size='26' font-weight='bold' fill='{TEXT}'>{title}</text>")
        for i, line in enumerate(lines):
            a(f"<text x='{x2 + 12}' y='{ty + 40 + i * 28}' font-size='22' fill='{MUTED}'>{line}</text>")

    callout(BOOT_Y, BOOT_Y - 8, "BOOT", ["short: previous message", "long: clear it and older ones"])
    callout(PWR_Y, PWR_Y + 2, "PWR", ["short: next message", "long: clear it and older ones"])
    a(f"<text x='{right + 82}' y='{cy0 + 60}' font-size='22' fill='{MUTED}'>both long: mute alarm</text>")
    a(f"<text x='{right + 82}' y='{cy0 + 32}' font-size='26' font-weight='bold' fill='{TEXT}'>Buttons on the right side</text>")
    a(f"<text x='{cx0 + cw / 2}' y='{cy0 + ch + 44}' font-size='22' fill='{MUTED}' "
      f"text-anchor='middle'>USB-C</text>")
    a("</svg>")
    out.write_text("\n".join(el) + "\n")
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
