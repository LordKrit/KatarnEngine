#!/usr/bin/env python3
"""rd-sojourn base assets: procedural stand-ins for the textures Warzone's renderer loads by name
(ocean / lake waves, foam, caustics, sky domes, noise, env maps, grass blades). Warzone shipped its own
art for these; the GPL source repo has none, so these are generated here.
usage: make_assets.py <out.pk3>"""
import io, sys, zipfile
import numpy as np
from PIL import Image, ImageFilter

rng = np.random.default_rng(1234)


def tileable_noise(size, octaves=6, persistence=0.5, base=4, seed=0):
    """fBm value noise that tiles (bilinear on a wrapped lattice)"""
    r = np.random.default_rng(seed)
    out = np.zeros((size, size), np.float64)
    amp, tot = 1.0, 0.0
    for o in range(octaves):
        cells = base * (2 ** o)
        if cells > size: break
        lat = r.random((cells, cells))
        x = np.arange(size) * cells / size
        i0 = np.floor(x).astype(int); f = x - i0; i1 = (i0 + 1) % cells
        f = f * f * (3 - 2 * f)
        a = lat[np.ix_(i0, i0)]; b = lat[np.ix_(i0, i1)]
        c = lat[np.ix_(i1, i0)]; d = lat[np.ix_(i1, i1)]
        fy = f[:, None]; fx = f[None, :]
        out += amp * ((a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy)
        tot += amp; amp *= persistence
    out /= tot
    return (out - out.min()) / (out.max() - out.min() + 1e-9)


def to_img(a, mode='L'):
    return Image.fromarray(np.clip(a * 255, 0, 255).astype(np.uint8), mode)


def normal_from_height(h, strength=4.0):
    dx = (np.roll(h, -1, 1) - np.roll(h, 1, 1)) * strength
    dy = (np.roll(h, -1, 0) - np.roll(h, 1, 0)) * strength
    n = np.dstack([-dx, -dy, np.ones_like(h)])
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    return Image.fromarray(((n * 0.5 + 0.5) * 255).astype(np.uint8), 'RGB')


def waves(seed, size=512, sharp=1.0):
    h = tileable_noise(size, 7, 0.55, 4, seed)
    h = 1 - np.abs(h * 2 - 1) ** sharp          # ridged: wave crests
    return h


files = {}


def put(name, img, fmt=None):
    b = io.BytesIO()
    fmt = fmt or name.rsplit('.', 1)[1].upper().replace('JPG', 'JPEG').replace('TGA', 'TGA')
    if fmt == 'JPEG' and img.mode == 'RGBA': img = img.convert('RGB')
    img.save(b, fmt, **({'quality': 92} if fmt == 'JPEG' else {}))
    files[name] = b.getvalue()


# --- noise / lookup
put('gfx/random.png', Image.fromarray(rng.integers(0, 256, (256, 256, 4), dtype=np.uint8), 'RGBA'))
put('gfx/random2K.tga', Image.fromarray(rng.integers(0, 256, (2048, 2048, 3), dtype=np.uint8), 'RGB'))
put('gfx/random2Ka.tga', Image.fromarray(rng.integers(0, 256, (2048, 2048, 4), dtype=np.uint8), 'RGBA'))
put('gfx/smoothNoise.jpg', to_img(tileable_noise(512, 6, 0.5, 4, 7)).convert('RGB'))
put('gfx/ssdoNoise.png', Image.fromarray(rng.integers(0, 256, (64, 64, 3), dtype=np.uint8), 'RGB'))
put('gfx/cloudtile.png', to_img(tileable_noise(512, 7, 0.55, 4, 21) ** 1.5))
put('gfx/defaultSplatControl.jpg', Image.new('RGB', (64, 64), (255, 0, 0)))
put('gfx/tessControlImage.png', Image.new('RGB', (64, 64), (255, 255, 255)))
put('gfx/forcefield.png', to_img(tileable_noise(256, 5, 0.6, 8, 3)).convert('RGB'))

# --- water: ocean + lake (diffuse, normal, spec, ao)
for nm, seed, sharp, col in (('ocean', 11, 1.4, (0.02, 0.22, 0.32)), ('lake', 12, 1.0, (0.05, 0.28, 0.30))):
    h = waves(seed, 512, sharp)
    put('gfx/waves/%swaves_n.png' % nm, normal_from_height(h, 6.0))
    d = np.dstack([col[0] + h * 0.10, col[1] + h * 0.18, col[2] + h * 0.20])
    put('gfx/waves/%swaves_d.png' % nm, to_img(d, 'RGB'))
    put('gfx/waves/%swaves_s.png' % nm, to_img(0.3 + h * 0.7).convert('RGB'))
    put('gfx/waves/%swaves_ao.png' % nm, to_img(0.6 + h * 0.4).convert('RGB'))
for i, suf in enumerate(('', '02', '03', '04')):
    f = tileable_noise(512, 6, 0.6, 8, 40 + i)
    f = np.clip((f - 0.55) * 4, 0, 1)
    put('textures/water/waterFoamGrey%s.png' % suf, to_img(f).convert('RGB'))
c = waves(77, 512, 3.0) ** 6
put('textures/water/waterCausicsMap.png', to_img(c).convert('RGB'))

# --- sky domes, sun, moons, aurora
cl1 = tileable_noise(1024, 8, 0.55, 4, 101); cl2 = tileable_noise(1024, 8, 0.6, 4, 102)
put('textures/skydomes/default_clouds1.png', Image.merge('RGBA', [to_img(np.ones_like(cl1) * 0.95)] * 3 + [to_img(np.clip((cl1 - 0.45) * 2.5, 0, 1))]))
put('textures/skydomes/default_clouds2.png', Image.merge('RGBA', [to_img(np.ones_like(cl2) * 0.9)] * 3 + [to_img(np.clip((cl2 - 0.5) * 2.5, 0, 1))]))
y = np.linspace(0, 1, 256)[:, None] * np.ones((1, 16))
put('textures/skydomes/default_tint.png', to_img(np.dstack([0.35 + 0.3 * y, 0.55 + 0.25 * y, 0.85 + 0.1 * y]), 'RGB'))
put('textures/skydomes/default_tint2.png', to_img(np.dstack([0.95 - 0.4 * y, 0.55 - 0.2 * y, 0.35 + 0.2 * y]), 'RGB'))
yy, xx = np.mgrid[-1:1:256j, -1:1:256j]; r = np.sqrt(xx ** 2 + yy ** 2)
sun = np.clip(1.2 - r * 1.6, 0, 1) ** 2
put('textures/skydomes/default_sun.png', Image.merge('RGBA', [to_img(np.ones_like(sun))] * 3 + [to_img(sun)]))
moon_d = 0.65 + 0.35 * tileable_noise(256, 6, 0.6, 4, 55)
moon_a = (r < 0.95).astype(float)
for nm in ('textures/skydomes/default_moon.png', 'gfx/moons/moon.png'):
    put(nm, Image.merge('RGBA', [to_img(moon_d)] * 3 + [to_img(moon_a)]))
put('gfx/moons/greenworld.png', Image.merge('RGBA', [to_img(moon_d * 0.4), to_img(moon_d * 0.8), to_img(moon_d * 0.5), to_img(moon_a)]))
band = np.exp(-((yy - 0.1 * np.sin(xx * 6)) ** 2) * 20) * tileable_noise(256, 5, 0.6, 8, 61)
for i, nm in enumerate(('gfx/misc/aurora1.png', 'gfx/misc/aurora2.png')):
    put(nm, Image.merge('RGBA', [to_img(band * 0.2), to_img(band), to_img(band * (0.5 + 0.3 * i)), to_img(band)]))

# --- env maps (sky gradient spheres)
env = np.dstack([0.45 + 0.4 * (1 - (yy + 1) / 2), 0.5 + 0.35 * (1 - (yy + 1) / 2), 0.55 + 0.4 * (1 - (yy + 1) / 2)])
put('textures/genericEnvMap/envmap.png', to_img(env, 'RGB'))
put('textures/genericEnvMap/envmap_spec.png', to_img(np.ones_like(yy) * 0.5).convert('RGB'))

# --- grass blades (alpha cut-out) for GRASS_ENABLED maps
W, H = 256, 256
g = np.zeros((H, W)); colr = np.zeros((H, W, 3))
for b in range(70):
    x0 = rng.uniform(8, W - 8); lean = rng.uniform(-40, 40); hgt = rng.uniform(0.45, 1.0) * H; wid = rng.uniform(2, 5)
    for yv in range(int(H - hgt), H):
        t = (H - yv) / hgt
        xc = x0 + lean * t * t; w = wid * (1 - t) + 0.5
        lo, hi = int(max(0, xc - w)), int(min(W, xc + w + 1))
        g[yv, lo:hi] = 1
        colr[yv, lo:hi] = (0.18 + 0.25 * t, 0.35 + 0.35 * t, 0.10 + 0.08 * t)
put('models/warzone/foliage/grass01.png', Image.merge('RGBA', [to_img(colr[..., 0]), to_img(colr[..., 1]), to_img(colr[..., 2]), to_img(g)]))

# --- roads
rd = 0.35 + 0.15 * tileable_noise(512, 6, 0.6, 16, 90)
put('textures/roads/defaultRoad01.png', to_img(np.dstack([rd, rd * 0.95, rd * 0.9]), 'RGB'))

out = sys.argv[1]
with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
    for n, d in sorted(files.items()): z.writestr(n, d)
print('%s: %d files' % (out, len(files)))
