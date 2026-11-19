"""Draws the launcher's paper and canvas textures (assets/), the same recipe as
the game's own PC menus (runtime/pc_menu.cpp MakePaper, PaintOlive): value
noise, fibres, coffee rings and creases on the paper; painted canvas, worn
lighter in patches, for the olive band. Bad Company's band is the same canvas
in gunmetal grey.

  python make_textures.py        (needs Pillow and numpy; writes assets/)
"""
import os

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'assets')
W, H = 1100, 720  # the launcher's window


def rand(x, y, s):
    """A hash of integer lattice points to 0..1."""
    h = (x.astype(np.int64) * 374761393 + y.astype(np.int64) * 668265263 + s * 2246822519) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    return ((h ^ (h >> 16)) & 0xFFFF) / 65535.0


def noise(x, y, s):
    xi, yi = np.floor(x), np.floor(y)
    fx, fy = x - xi, y - yi
    fx, fy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
    a, b = rand(xi, yi, s), rand(xi + 1, yi, s)
    c, d = rand(xi, yi + 1, s), rand(xi + 1, yi + 1, s)
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy


def fbm(x, y, s):
    v, amp = np.zeros_like(x), 0.5
    for o in range(5):
        v += amp * noise(x, y, s + o)
        x, y, amp = x * 2.03, y * 2.03, amp * 0.5
    return v


def save(px, name, quality=82):
    Image.fromarray(np.clip(px, 0, 255).astype(np.uint8), 'RGB').save(os.path.join(OUT, name), quality=quality,
                                                                      optimize=True)


def paper():
    y, x = np.mgrid[0:H, 0:W].astype(np.float64)
    n = fbm(x / 150, y / 150, 1)
    fine = rand(x, y, 7) - 0.5
    fibre = noise(x / 1.2, y / 24, 3) - 0.5
    shade = (n - 0.5) * 30 + fine * 8 + fibre * 6
    dx, dy = (x - W * 0.6) / (W * 0.6), (y - H * 0.5) / (H * 0.5)
    shade -= np.maximum(0, np.sqrt(dx * dx * 0.8 + dy * dy) - 0.6) * 60
    for rx, ry, rr in ((930, 150, 70), (1010, 600, 42), (640, 640, 92)):  # coffee rings
        wob = (fbm(np.arctan2(y - ry, x - rx) * 3 + 10, np.full_like(x, rr), 31) - 0.5) * rr * 0.25
        d = np.hypot(x - rx, y - ry) + wob
        band = np.exp(-(((d - rr) / 3.5) ** 2)) * 0.6 + np.where(d < rr, 0.12, 0) * fbm(x / 25, y / 25, 9)
        shade -= band * 22 * fbm(x / 15, y / 15, 33) * 1.6
    # Creases: a fold down the middle of the page and across.
    crease = np.exp(-(((x - 760) / 1.8) ** 2)) + np.exp(-(((y - 380) / 1.8) ** 2)) * 0.7
    shade += -crease * 14 + np.exp(-(((x - 763) / 3) ** 2)) * 7
    save(np.dstack([222 + shade, 210 + shade * 0.97, 178 + shade * 0.85]), 'paper.jpg', 85)


def canvas(name, base, blue_k):
    y, x = np.mgrid[0:H, 0:W].astype(np.float64)
    n = fbm(x / 110, y / 110, 21)
    weave = (noise(x / 1.2, y / 1.2, 22) - 0.5) * 10 + np.where((x + y) % 3 == 0, 2.0, 0.0)
    wear = np.maximum(0, fbm(x / 36, y / 36, 23) - 0.62) * 110
    shade = (n - 0.5) * 30 + weave + wear
    r, g, b = base
    save(np.dstack([r + shade, g + shade, b + shade * blue_k]), name)


if __name__ == '__main__':
    os.makedirs(OUT, exist_ok=True)
    paper()
    canvas('olive.jpg', (78, 86, 36), 0.6)      # pc_menu.cpp kOlive
    canvas('gunmetal.jpg', (58, 61, 56), 0.95)  # Bad Company's
