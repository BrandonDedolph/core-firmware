#!/usr/bin/env python3
"""Regenerate the disclosure chevron's coverage mask (core/ui/chrome.c
g_chev_cov, and docs/screens/render.py CHEV_COV).

A ">" polyline with round caps and joins, drawn 16x oversize with PIL and
box-filtered down to W x H, so each byte is the fraction (0..255) of that
panel pixel the stroke covers. The firmware blends it through
text_blend565 exactly as it blends a glyph's coverage. One-shot: run it
only when the chevron's size or weight changes, and paste the output into
both files.

    tools/.venv/bin/python3 tools/chevron_mask.py            # 4x7, 1.25 px
    tools/.venv/bin/python3 tools/chevron_mask.py 5 9 1.5    # W H stroke
"""
import sys
from PIL import Image, ImageDraw

def mask(w, h, stroke):
    Z = 16
    im = Image.new("L", (w * Z, h * Z), 0)
    d = ImageDraw.Draw(im)
    x0, x1 = stroke * Z * 0.7, w * Z - stroke * Z * 0.7
    pts = [(x0, stroke * Z * 0.7), (x1, h * Z / 2), (x0, h * Z - stroke * Z * 0.7)]
    d.line(pts, fill=255, width=int(stroke * Z), joint="curve")
    r = stroke * Z / 2
    for (x, y) in pts:
        d.ellipse((x - r, y - r, x + r, y + r), fill=255)
    px = list(im.resize((w, h), Image.BOX).getdata())
    return [px[i * w:(i + 1) * w] for i in range(h)]

def main(argv):
    w, h, stroke = (int(argv[1]), int(argv[2]), float(argv[3])) if len(argv) > 3 else (4, 7, 1.25)
    rows = mask(w, h, stroke)
    print("/* %dx%d, stroke %.2f px -- tools/chevron_mask.py */" % (w, h, stroke))
    for r in rows:
        print("    " + ", ".join("%3d" % v for v in r) + ",")

if __name__ == "__main__":
    main(sys.argv)
