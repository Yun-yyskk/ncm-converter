"""Draws the application icon (a blue tile with a white music note).

Run:  python tools/make-icon.py
Writes res/app.ico (multi-size) and docs/icon.png (512 px preview).
"""
from PIL import Image, ImageDraw
import math
import os

MASTER = 2048                     # drawn at 2x of the largest icon size
BASE = 1024                       # design grid
SCALE = MASTER / BASE

TOP = (47, 141, 224)              # #2F8DE0
BOTTOM = (0, 88, 176)             # #0058B0


def bezier(p0, p1, p2, p3, steps=64):
    pts = []
    for i in range(steps + 1):
        t = i / steps
        mt = 1 - t
        x = mt**3 * p0[0] + 3 * mt**2 * t * p1[0] + 3 * mt * t**2 * p2[0] + t**3 * p3[0]
        y = mt**3 * p0[1] + 3 * mt**2 * t * p1[1] + 3 * mt * t**2 * p2[1] + t**3 * p3[1]
        pts.append((x, y))
    return pts


def rotated_ellipse(cx, cy, rx, ry, degrees, steps=96):
    a = math.radians(degrees)
    pts = []
    for i in range(steps):
        t = 2 * math.pi * i / steps
        x = rx * math.cos(t)
        y = ry * math.sin(t)
        pts.append((cx + x * math.cos(a) - y * math.sin(a),
                    cy + x * math.sin(a) + y * math.cos(a)))
    return pts


def build():
    s = lambda v: v * SCALE
    canvas = Image.new("RGBA", (MASTER, MASTER), (0, 0, 0, 0))

    # tile with a soft vertical gradient
    tile = Image.new("RGBA", (MASTER, MASTER))
    draw = ImageDraw.Draw(tile)
    for y in range(MASTER):
        t = y / (MASTER - 1)
        colour = tuple(round(TOP[i] + (BOTTOM[i] - TOP[i]) * t) for i in range(3)) + (255,)
        draw.line([(0, y), (MASTER, y)], fill=colour)
    mask = Image.new("L", (MASTER, MASTER), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        [0, 0, MASTER - 1, MASTER - 1], radius=s(206), fill=255)
    canvas.paste(tile, (0, 0), mask)

    note = Image.new("RGBA", (MASTER, MASTER), (0, 0, 0, 0))
    nd = ImageDraw.Draw(note)
    white = (255, 255, 255, 255)

    # flag (drawn first so the stem covers its root)
    outer = bezier((574, 218), (712, 208), (806, 300), (792, 470))
    inner = bezier((792, 470), (736, 414), (700, 300), (574, 316))
    nd.polygon([(s(x), s(y)) for x, y in outer + inner], fill=white)

    # stem
    nd.rounded_rectangle([s(518), s(214), s(578), s(700)], radius=s(27), fill=white)

    # note head
    nd.polygon([(s(x), s(y)) for x, y in rotated_ellipse(408, 700, 158, 112, -24)], fill=white)

    canvas.alpha_composite(note)

    out = canvas.resize((256, 256), Image.LANCZOS)
    return out, canvas


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(here)
    icon256, master = build()

    ico = os.path.join(repo, "res", "app.ico")
    icon256.save(ico, format="ICO",
                 sizes=[(256, 256), (128, 128), (64, 64), (48, 48),
                        (40, 40), (32, 32), (24, 24), (20, 20), (16, 16)])
    preview = master.resize((512, 512), Image.LANCZOS)
    preview.save(os.path.join(repo, "docs", "icon.png"))
    print("wrote", ico)
    print("wrote", os.path.join(repo, "docs", "icon.png"))


if __name__ == "__main__":
    main()
