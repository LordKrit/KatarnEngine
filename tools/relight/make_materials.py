"""Generate PBR-style material maps for a JKA map's textures so rend2 renders them with
real surface detail.  For every texture a map uses, writes next to it:
  <tex>_nh.png : normal map (rgb) + height (alpha) -> bumps + parallax depth
  <tex>_rmo.png: roughness / metalness / ambient occlusion
rend2 loads these automatically (see ParseStage / the _nh and _rmo lookups).

Materials come from the shader's surface material (marble, metal, tiles, stone...) so polished
floors stay glossy and sandstone stays matte; the texture itself adds variation (grime and
mortar are rougher, mortar lines become grooves).

usage: make_materials.py map.bsp <assets dir> <out dir> [--strength 1.0]
"""
import struct, sys, os, re, glob, argparse
import numpy as np
from PIL import Image
from scipy.ndimage import gaussian_filter, sobel

ap = argparse.ArgumentParser()
ap.add_argument('bsp'); ap.add_argument('assets'); ap.add_argument('out')
ap.add_argument('--strength', type=float, default=1.0)
A = ap.parse_args()

d = open(A.bsp, 'rb').read()
o, l = struct.unpack_from('<ii', d, 8 + 1 * 8)
sh = d[o:o + l]
shaders = [(sh[i:i + 64].split(b'\0')[0].decode('latin1'), struct.unpack_from('<i', sh, i + 64)[0]) for i in range(0, len(sh), 72)]

text = {}
for f in glob.glob(os.path.join(A.assets, 'shaders', '*.shader')):
    t = re.sub(r'//[^\n]*', '', open(f, encoding='latin1').read())
    for m in re.finditer(r'([^\s{}]+)\s*\{((?:[^{}]|\{[^{}]*\})*)\}', t):
        text.setdefault(m.group(1).lower(), m.group(2))

def find(name):
    base = re.sub(r'\.(tga|jpg|png)$', '', name, flags=re.I)
    for ext in ('.jpg', '.tga', '.png'):
        p = os.path.join(A.assets, base + ext)
        dp, fn = os.path.split(p)
        if os.path.isdir(dp):
            for e in os.listdir(dp):
                if e.lower() == fn.lower():
                    return os.path.join(dp, e), base
    return None, None

# material -> (roughness, metal, bump strength, height depth)
MAT = {
    0:  (0.65, 0.0, 1.0, 0.6),   # none
    3:  (0.35, 0.85, 0.6, 0.3),  # solid metal
    4:  (0.40, 0.80, 0.6, 0.3),  # hollow metal
    10: (0.05, 0.0, 0.2, 0.0),   # glass
    11: (0.85, 0.0, 1.2, 0.7),   # concrete
    12: (0.18, 0.0, 0.5, 0.3),   # marble
    23: (0.90, 0.0, 1.6, 1.0),   # rock
    26: (0.22, 0.0, 0.7, 0.4),   # tiles
    25: (0.35, 0.0, 0.5, 0.2),   # plastic
    32: (0.30, 0.6, 0.6, 0.3),   # computer
    8:  (0.95, 0.0, 1.4, 0.8),   # sand
    28: (0.80, 0.0, 1.0, 0.5),   # plaster
}

done = {}
for name, flags in shaders:
    body = text.get(name.lower(), '')
    if re.search(r'surfaceparm\s+(sky|nodraw)', body, re.I):
        continue
    cands = []
    for m in re.finditer(r'\bmap\s+(\S+)', body, re.I):
        if not m.group(1).startswith('$'): cands.append(m.group(1)); break
    cands.append(name)
    for c in cands:
        path, base = find(c)
        if path: break
    if not path or base.lower() in done:
        continue
    mat = flags & 0x1f
    rough0, metal0, bump, depth = MAT.get(mat, MAT[0])
    # a name hint helps untagged shaders
    lname = base.lower()
    if mat == 0:
        if any(k in lname for k in ('floor', 'tile', 'marble', 'polish')): rough0, metal0, bump, depth = MAT[26]
        elif any(k in lname for k in ('metal', 'pipe', 'steel', 'chrome', 'grate')): rough0, metal0, bump, depth = MAT[3]
        elif any(k in lname for k in ('stone', 'rock', 'wall', 'brick', 'sand')): rough0, metal0, bump, depth = MAT[11]

    im = Image.open(path)
    has_alpha = im.mode in ('RGBA', 'LA') or 'transparency' in im.info
    rgb = np.asarray(im.convert('RGB'), np.float32) / 255.0
    H, W = rgb.shape[:2]
    lum = rgb @ np.array([0.299, 0.587, 0.114], np.float32)

    # height: large-scale shape from low frequencies (blocks bulge, mortar sinks) + fine grain
    s = max(W, H) / 256.0
    low = gaussian_filter(lum, 6 * s, mode='wrap')
    mid = gaussian_filter(lum, 1.5 * s, mode='wrap')
    height = (mid - gaussian_filter(lum, 12 * s, mode='wrap')) * 1.5 + (lum - mid) * 0.6
    height = (height - height.min()) / max(height.max() - height.min(), 1e-5)

    # normals from the height (tangent space, +y up in image = -row)
    k = 2.5 * bump * A.strength * (256.0 / max(W, H)) * 8.0
    gx = sobel(height, axis=1, mode='wrap') * k
    gy = sobel(height, axis=0, mode='wrap') * k
    n = np.stack([-gx, gy, np.ones_like(gx)], -1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    nh = np.concatenate([n * 0.5 + 0.5, (height * depth + (1 - depth))[..., None]], -1)

    # roughness: base material, rougher in grime / mortar / dark crevices
    cavity = np.clip((low - lum) * 4.0, 0, 1)
    detail = np.clip(np.abs(lum - mid) * 6.0, 0, 1)
    rough = np.clip(rough0 + cavity * 0.35 + detail * 0.15 * (1 - rough0), 0.03, 1.0)
    metal = np.full_like(rough, metal0)
    ao = np.clip(1.0 - cavity * 0.6, 0.3, 1.0)
    rmo = np.stack([rough, metal, ao], -1)

    rel = os.path.relpath(base, '.')
    outbase = os.path.join(A.out, rel)
    os.makedirs(os.path.dirname(outbase), exist_ok=True)
    Image.fromarray((nh * 255 + 0.5).astype(np.uint8), 'RGBA').save(outbase + '_nh.png', optimize=True)
    Image.fromarray((rmo * 255 + 0.5).astype(np.uint8), 'RGB').save(outbase + '_rmo.png', optimize=True)
    done[base.lower()] = (mat, rough0)

print(len(done), 'textures got material maps')
