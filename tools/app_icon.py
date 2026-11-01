#!/usr/bin/env python3
"""Render Core's mark into the desktop app's window icon.

The mark is the boot screen's ring and dot, on a 40-unit box: a ring of
radius 17 stroked 3 wide, a dot of radius 6.5 with a hole of radius 2.2.
The same numbers are drawn as vectors by core/cli/internal/app/mark.go;
this renders them to PNGs for the Windows icon resource (winres.json's
RT_GROUP_ICON "#1", the id Gio's Windows backend loads).

Each size is drawn at 8x and box-filtered down (the mean of each 8x8
block), which is the anti-aliasing. Ink #1a1714 (PAL_LINEN's ink) on
transparent; the hole is transparent too, so the mark sits on whatever the
taskbar is.

    tools/.venv/bin/python3 -I tools/app_icon.py
    cd core/cli && go generate ./cmd/core-app
"""
import os
import sys

from PIL import Image, ImageDraw

SIZES = (16, 24, 32, 48, 64, 128, 256)
INK = (0x1A, 0x17, 0x14, 0xFF)
SURFACE = (0xF4, 0xF1, 0xEC, 0xFF)
RADIUS = 0.22  # tile corner, fraction of the side
MARK = 0.70    # mark's box, fraction of the side
SS = 8

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "core", "cli", "cmd", "core-app", "winres", "icon")


def mark(size):
    n = size * SS
    tile = Image.new("L", (n, n), 0)
    ImageDraw.Draw(tile).rounded_rectangle((0, 0, n - 1, n - 1), radius=RADIUS * n, fill=255)

    # The mark's coverage on its own box, centred on the tile.
    m = Image.new("L", (n, n), 0)
    d = ImageDraw.Draw(m)
    s = n * MARK / 40.0
    c = n / 2.0

    def disc(r, fill):
        d.ellipse((c - r * s, c - r * s, c + r * s, c + r * s), fill=fill)

    disc(17 + 1.5, 255)  # ring outer edge
    disc(17 - 1.5, 0)    # ring inner edge
    disc(6.5, 255)       # dot
    disc(2.2, 0)         # hole (surface: it is cut out of the dot only)

    big = Image.new("RGBA", (n, n), SURFACE[:3] + (0,))
    big.paste(Image.new("RGBA", (n, n), SURFACE), (0, 0), tile)
    big.paste(Image.new("RGBA", (n, n), INK), (0, 0), m)
    # Downsample premultiplied so the transparent corners do not bleed
    # their (invisible) colour into the tile's edge.
    r, g, b, a = big.split()
    pre = Image.merge("RGB", [Image.composite(ch, Image.new("L", (n, n), 0), a) for ch in (r, g, b)])
    pre = pre.resize((size, size), Image.BOX)
    a = a.resize((size, size), Image.BOX)
    out = Image.new("RGBA", (size, size))
    px, pa, po = pre.load(), a.load(), out.load()
    for y in range(size):
        for x in range(size):
            al = pa[x, y]
            if al:
                po[x, y] = tuple(min(255, round(v * 255 / al)) for v in px[x, y]) + (al,)
    return out


def main():
    os.makedirs(OUT, exist_ok=True)
    for size in SIZES:
        path = os.path.join(OUT, f"mark-{size}.png")
        mark(size).save(path, optimize=True)
        print(os.path.relpath(path, os.path.join(HERE, "..")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
