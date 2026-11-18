"""Fits where a level's minimap picture lies on the level (runtime/bot_minimap.cpp's
kFits): the world square it shows (centre x, z and half its width, metres) and
which axis runs the other way, by the correlation of edge strength between the
picture and a reference: the level's own terrain (its detailed heightfield
tile, hill-shaded) or a training run's map.png (runtime/bot_map.cpp's drawing,
with the run's bounds from its data.js). A developer's tool (numpy, scipy,
Pillow); the games use the numbers only.

  python tools/minimap_fit.py <minimap.png> --hf <Heightfield-000.res> [--sea <height>]
  python tools/minimap_fit.py <minimap.png> --map <map.png> <cx> <cz> <half>
      [--start cx,cz,half] [--box x0,z0,x1,z1] [--check <out.png>]

The minimap: <user>/minimaps/<map>.png (the games write it), or the texture
decoded otherwise. The heightfield: tools/fbrb.py extract <level>/terrain-00.fbrb
<dir> TerrainHeightfield (the -000 tile). Orientations tried: as stored,
columns flipped (mirror x: BF1943's), rows flipped (mirror z: Bad Company's).
--check writes a checkerboard of the reference and the fitted picture: edges
must run on across the squares.
"""
import argparse
import struct

import numpy as np
from PIL import Image
from scipy import ndimage


def hill_shade(path, sea=None):
    d = open(path, 'rb').read()
    x0, y0, z0, x1, y1, z1 = struct.unpack('>6f', d[:24])
    n = struct.unpack('>I', d[24:28])[0]
    h = np.frombuffer(d[0x2E:0x2E + 2 * n * n], '<u2').reshape(n, n).astype(np.float32)
    h = (y0 + h / 65535 * (y1 - y0))[::-1]  # rows north first
    cell = (x1 - x0) / n
    lit = np.clip(0.55 + (-np.gradient(h, axis=1) + np.gradient(h, axis=0)) / cell * 0.6, 0, 1) * 200
    if sea is not None:
        lit = np.where(h < sea, 40, lit)
    return lit, ((x0 + x1) / 2, (z0 + z1) / 2, (x1 - x0) / 2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('minimap')
    ap.add_argument('--hf')
    ap.add_argument('--sea', type=float)
    ap.add_argument('--map', nargs=4)
    ap.add_argument('--start', default='0,0,1024')
    ap.add_argument('--box', default='-700,-700,700,700')
    ap.add_argument('--check')
    a = ap.parse_args()
    mm0 = np.asarray(Image.open(a.minimap).convert('L')).astype(np.float32)
    if a.hf:
        ref_img, (cx, cz, half) = hill_shade(a.hf, a.sea)
    else:
        ref_img = np.asarray(Image.open(a.map[0]).convert('L')).astype(np.float32)
        cx, cz, half = map(float, a.map[1:])
    bx0, bz0, bx1, bz1 = map(float, a.box.split(','))
    N = 600
    X, Z = np.meshgrid(np.linspace(bx0, bx1, N), np.linspace(bz1, bz0, N))
    rmpp = 2 * half / ref_img.shape[0]
    ref = ndimage.map_coordinates(ref_img, [((cz + half) - Z) / rmpp - 0.5, (X - (cx - half)) / rmpp - 0.5], order=1)

    def edges(img):
        img = ndimage.gaussian_filter(img, 1.5)
        e = np.hypot(ndimage.sobel(img, 0), ndimage.sobel(img, 1))
        return (e - e.mean()) / (e.std() + 1e-6)

    er = edges(ref)
    orients = {'as stored': mm0, 'mirror x': mm0[:, ::-1], 'mirror z': mm0[::-1]}

    def sample(mm, p):
        H, W = mm.shape
        u = (X - (p[0] - p[2])) / (2 * p[2]) * W - 0.5
        v = ((p[1] + p[2]) - Z) / (2 * p[2]) * H - 0.5
        return ndimage.map_coordinates(mm, [v, u], order=1)

    best = None
    for name, mm in orients.items():
        p = [float(t) for t in a.start.split(',')]
        s0 = float((edges(sample(mm, p)) * er).mean())
        step = 8.0
        while step >= 0.25:
            improved = True
            while improved:
                improved = False
                for i in range(3):
                    for d in (-1, 1):
                        q = list(p)
                        q[i] += d * step
                        s = float((edges(sample(mm, q)) * er).mean())
                        if s > s0:
                            s0, p, improved = s, q, True
            step /= 2
        print('%-9s  cx %8.2f  cz %8.2f  half %8.2f  correlation %.4f' % (name, p[0], p[1], p[2], s0))
        if best is None or s0 > best[0]:
            best = (s0, name, p)
    s0, name, p = best
    print('best: %s, cx %.2f cz %.2f half %.2f' % (name, p[0], p[1], p[2]))
    if a.check:
        rgb = np.asarray(Image.open(a.minimap).convert('RGB')).astype(np.float32)
        rgb = {'as stored': rgb, 'mirror x': rgb[:, ::-1], 'mirror z': rgb[::-1]}[name]
        got = np.stack([sample(rgb[..., c], p) for c in range(3)], -1)
        cb = ((np.arange(N)[:, None] // 60 + np.arange(N)[None, :] // 60) % 2).astype(bool)
        out = np.where(cb[..., None], np.repeat(ref[..., None], 3, -1), got)
        Image.fromarray(np.clip(out, 0, 255).astype(np.uint8)).save(a.check)


if __name__ == '__main__':
    main()
