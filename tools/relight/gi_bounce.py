"""Add path-traced bounce light to a compiled JKA (RBSP) map, keeping its original lighting.

The map's existing lightmaps are the direct light. For every lightmap texel we cast
cosine-weighted rays over the hemisphere, look up the light leaving whatever they hit
(that surface's lightmap x its texture colour), and add the gathered light back on.
Repeating that gives multiple bounces. Original lights are never lost, which matters
because compiled maps no longer contain their light entities.

usage: gi_bounce.py in.bsp out.bsp --assets <dir with textures/ shaders/> [--bounces 2]
       [--rays 48] [--strength 1.0] [--preview out.png]
"""
import struct, sys, os, re, glob, argparse, math
import numpy as np
from PIL import Image
from embreex import rtcore_scene as rtcs
from embreex.mesh_construction import TriangleMesh

ap = argparse.ArgumentParser()
ap.add_argument('inp'); ap.add_argument('out')
ap.add_argument('--assets', action='append', default=[])
ap.add_argument('--bounces', type=int, default=2)
ap.add_argument('--rays', type=int, default=48)
ap.add_argument('--strength', type=float, default=1.0)
ap.add_argument('--default-albedo', type=float, default=0.45)
ap.add_argument('--grid', type=int, default=1, help='also add bounce to the light grid (characters)')
ap.add_argument('--preview')
A = ap.parse_args()

d = bytearray(open(A.inp, 'rb').read())
assert d[:4] == b'RBSP'
L = [list(struct.unpack_from('<ii', d, 8 + i * 8)) for i in range(18)]
def lump(i): o, l = L[i]; return bytes(d[o:o + l])

SURF_SKY, SURF_NODRAW = 0x2000, 0x200000
sh = lump(1)
shaders = [(sh[i:i + 64].split(b'\0')[0].decode('latin1'),) + struct.unpack_from('<ii', sh, i + 64) for i in range(0, len(sh), 72)]
verts = np.frombuffer(lump(10), dtype=np.dtype([('xyz', '<f4', 3), ('st', '<f4', 2), ('lm', '<f4', (4, 2)),
                                                 ('n', '<f4', 3), ('c', 'u1', (4, 4))]))
idx = np.frombuffer(lump(11), '<i4')
SS = 148
sraw = lump(13)
nsurf = len(sraw) // SS
lmraw = lump(14)
NLM = len(lmraw) // (128 * 128 * 3)
lm = np.frombuffer(lmraw, np.uint8).reshape(NLM, 128, 128, 3).astype(np.float32) / 255.0
print(f'{nsurf} surfaces, {len(verts)} verts, {NLM} lightmaps')

# ---------------- albedo per shader ----------------
shader_text = {}
for root in A.assets:
    for f in glob.glob(os.path.join(root, 'shaders', '*.shader')):
        t = open(f, encoding='latin1').read()
        t = re.sub(r'//[^\n]*', '', t)
        for m in re.finditer(r'([^\s{}]+)\s*\{((?:[^{}]|\{[^{}]*\})*)\}', t):
            shader_text.setdefault(m.group(1).lower(), m.group(2))

def find_image(name):
    base = re.sub(r'\.(tga|jpg|png)$', '', name, flags=re.I)
    for root in A.assets:
        for ext in ('.jpg', '.tga', '.png'):
            p = os.path.join(root, base + ext)
            if os.path.exists(p): return p
            # case-insensitive fallback
            dirp, fn = os.path.split(p)
            if os.path.isdir(dirp):
                for e in os.listdir(dirp):
                    if e.lower() == fn.lower(): return os.path.join(dirp, e)
    return None

albedo_cache = {}
def albedo(si):
    name = shaders[si][0]
    if name in albedo_cache: return albedo_cache[name]
    cands = []
    body = shader_text.get(name.lower())
    if body:
        m = re.search(r'qer_editorimage\s+(\S+)', body, re.I)
        if m: cands.append(m.group(1))
        for m in re.finditer(r'\bmap\s+(\S+)', body, re.I):
            if not m.group(1).startswith('$'): cands.append(m.group(1))
    cands.append(name)
    col = None
    for c in cands:
        p = find_image(c)
        if p:
            try:
                im = Image.open(p).convert('RGB').resize((16, 16), Image.BILINEAR)
                a = np.asarray(im, np.float32).reshape(-1, 3).mean(0) / 255.0
                col = a ** 2.2 * 1.0   # textures are sRGB-ish
                col = np.clip(col / max(col.max(), 1e-3) * min(col.max(), 0.85), 0.03, 0.85)
                break
            except Exception:
                pass
    if col is None: col = np.full(3, A.default_albedo ** 2.2 * 2.0, np.float32)
    albedo_cache[name] = col
    return col

# ---------------- geometry ----------------
tri_v = []; tri_lmuv = []; tri_lmnum = []; tri_alb = []
surf_tris = []
for s in range(nsurf):
    shader, fog, stype, fv, nv, fi, ni = struct.unpack_from('<7i', sraw, s * SS)
    lmnum = struct.unpack_from('<4i', sraw, s * SS + 36)[0]
    pw, ph = struct.unpack_from('<2i', sraw, s * SS + SS - 8)
    sf = shaders[shader][1] if 0 <= shader < len(shaders) else 0
    if sf & (SURF_SKY | SURF_NODRAW): continue
    if stype in (1, 3):
        tris = idx[fi:fi + ni].reshape(-1, 3) + fv
    elif stype == 2 and pw > 1 and ph > 1:
        t = []
        for r in range(ph - 1):
            for c in range(pw - 1):
                a = fv + r * pw + c
                t += [(a, a + pw, a + 1), (a + 1, a + pw, a + pw + 1)]
        tris = np.array(t, np.int64)
    else:
        continue
    start = len(tri_v)
    for tr in tris:
        tri_v.append(verts['xyz'][tr]); tri_lmuv.append(verts['lm'][tr, 0]); tri_lmnum.append(lmnum)
        tri_alb.append(albedo(shader))
    surf_tris.append((s, start, len(tri_v), lmnum))
tri_v = np.array(tri_v, np.float32); tri_lmuv = np.array(tri_lmuv, np.float32)
tri_lmnum = np.array(tri_lmnum, np.int32); tri_alb = np.array(tri_alb, np.float32)
print(f'{len(tri_v)} triangles, {len(albedo_cache)} shaders, '
      f'{sum(1 for v in albedo_cache.values() if not np.allclose(v, A.default_albedo ** 2.2 * 2.0))} with textures')

tri_n = np.cross(tri_v[:, 1] - tri_v[:, 0], tri_v[:, 2] - tri_v[:, 0])
tri_n /= np.maximum(np.linalg.norm(tri_n, axis=1, keepdims=True), 1e-9)
scene = rtcs.EmbreeScene()
TriangleMesh(scene, tri_v)

# ---------------- lightmap texels -> world ----------------
texel_pos = np.zeros((NLM, 128, 128, 3), np.float32)
texel_n = np.zeros((NLM, 128, 128, 3), np.float32)
texel_w = np.zeros((NLM, 128, 128), np.float32)
for ti in range(len(tri_v)):
    ln = tri_lmnum[ti]
    if ln < 0 or ln >= NLM: continue
    uv = tri_lmuv[ti] * 128.0 - 0.5
    if not np.isfinite(uv).all(): continue
    x0, y0 = np.floor(uv.min(0)).astype(int); x1, y1 = np.ceil(uv.max(0)).astype(int)
    x0, y0 = max(x0, 0), max(y0, 0); x1, y1 = min(x1, 127), min(y1, 127)
    if x1 < x0 or y1 < y0: continue
    gx, gy = np.meshgrid(np.arange(x0, x1 + 1), np.arange(y0, y1 + 1))
    p = np.stack([gx.ravel(), gy.ravel()], 1).astype(np.float32)
    a, b, c = uv
    den = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
    if abs(den) < 1e-9: continue
    w0 = ((b[1] - c[1]) * (p[:, 0] - c[0]) + (c[0] - b[0]) * (p[:, 1] - c[1])) / den
    w1 = ((c[1] - a[1]) * (p[:, 0] - c[0]) + (a[0] - c[0]) * (p[:, 1] - c[1])) / den
    w2 = 1 - w0 - w1
    inside = (w0 > -0.08) & (w1 > -0.08) & (w2 > -0.08)
    if not inside.any(): continue
    v = tri_v[ti]
    nrm = np.cross(v[1] - v[0], v[2] - v[0]); nl = np.linalg.norm(nrm)
    if nl < 1e-6: continue
    nrm /= nl
    w = np.stack([w0, w1, w2], 1)[inside]
    pos = w @ v
    px = p[inside].astype(int)
    # keep the most "inside" triangle per texel
    score = w.min(1) + 1.0
    cur = texel_w[ln, px[:, 1], px[:, 0]]
    upd = score > cur
    texel_pos[ln, px[upd, 1], px[upd, 0]] = pos[upd]
    texel_n[ln, px[upd, 1], px[upd, 0]] = nrm
    texel_w[ln, px[upd, 1], px[upd, 0]] = score[upd]
mask = texel_w > 0
P = texel_pos[mask]; N = texel_n[mask]
print(f'{mask.sum()} lit texels to trace')

def sample_lm(cur, tri, u, v):
    """radiance leaving the hit point: its lightmap x albedo (bilinear lookup)"""
    uvt = tri_lmuv[tri]
    uv = uvt[:, 0] * (1 - u - v)[:, None] + uvt[:, 1] * u[:, None] + uvt[:, 2] * v[:, None]
    ln = tri_lmnum[tri]
    ok = (ln >= 0) & (ln < NLM)
    uv = np.nan_to_num(uv, nan=0.0, posinf=0.0, neginf=0.0)
    x = np.clip(uv[:, 0] * 128 - 0.5, 0, 126.999); y = np.clip(uv[:, 1] * 128 - 0.5, 0, 126.999)
    xi, yi = x.astype(int), y.astype(int); fx, fy = (x - xi)[:, None], (y - yi)[:, None]
    lnc = np.clip(ln, 0, NLM - 1)
    c = (cur[lnc, yi, xi] * (1 - fx) * (1 - fy) + cur[lnc, yi, xi + 1] * fx * (1 - fy)
         + cur[lnc, yi + 1, xi] * (1 - fx) * fy + cur[lnc, yi + 1, xi + 1] * fx * fy)
    return np.where(ok[:, None], c * tri_alb[tri], 0.0)

def cosine_dirs(n, k, rng):
    # k cosine-weighted directions around each normal
    u1 = rng.random((len(n), k)); u2 = rng.random((len(n), k))
    r = np.sqrt(u1); th = 2 * np.pi * u2
    x, y, z = r * np.cos(th), r * np.sin(th), np.sqrt(1 - u1)
    up = np.where(np.abs(n[:, 2:3]) < 0.9, [[0, 0, 1]], [[1, 0, 0]])
    t = np.cross(up, n); t /= np.linalg.norm(t, axis=1, keepdims=True)
    b = np.cross(n, t)
    return (x[..., None] * t[:, None] + y[..., None] * b[:, None] + z[..., None] * n[:, None])

direct = lm.copy()
cur = lm.copy()
total_bounce = np.zeros_like(lm)
rng = np.random.default_rng(1)
CH = 20000
for bounce in range(A.bounces):
    gathered = np.zeros((len(P), 3), np.float32)
    for s0 in range(0, len(P), CH):
        p = P[s0:s0 + CH]; n = N[s0:s0 + CH]
        dirs = cosine_dirs(n, A.rays, rng).reshape(-1, 3).astype(np.float32)
        org = np.repeat(p + n * 0.75, A.rays, 0).astype(np.float32)
        res = scene.run(org, dirs, output=1)
        tri = res['primID']; hit = tri >= 0
        rad = np.zeros((len(org), 3), np.float32)
        back = np.zeros(len(org), bool)
        if hit.any():
            rad[hit] = sample_lm(cur, tri[hit], res['u'][hit], res['v'][hit])
            back[hit] = np.einsum('ij,ij->i', tri_n[tri[hit]], dirs[hit]) > 0.0
        g = rad.reshape(-1, A.rays, 3).mean(1)
        # texels whose rays mostly hit back faces sit inside geometry (lightmap padding): no bounce
        bf = back.reshape(-1, A.rays).mean(1)
        g[bf > 0.2] = 0.0
        gathered[s0:s0 + CH] = g
        print(f'\r bounce {bounce + 1}: {min(s0 + CH, len(P))}/{len(P)}', end='', flush=True)
    print()
    b = np.zeros_like(lm); b[mask] = gathered * A.strength
    # denoise: 5x5 mean over valid texels of the same lightmap page
    from scipy.ndimage import uniform_filter
    valid = (mask & (b.sum(-1) > 0)).astype(np.float32)
    num = np.stack([uniform_filter(b[..., c] * valid, size=(1, 5, 5)) for c in range(3)], -1)
    den = uniform_filter(valid, size=(1, 5, 5))[..., None]
    b = np.where(valid[..., None] > 0, num / np.maximum(den, 1e-4), 0.0).astype(np.float32)
    total_bounce += b
    cur = direct + b     # next bounce gathers from (direct + this bounce)

# fill seams: dilate bounce into unlit texels so bilinear filtering doesn't darken edges
from scipy.ndimage import grey_dilation
out = direct + total_bounce
out = np.clip(out, 0, 1)
newlm = (out * 255 + 0.5).astype(np.uint8)
newlm[~mask] = (lm[~mask] * 255 + 0.5).astype(np.uint8)
o, l = L[14]
d[o:o + l] = newlm.tobytes()

# ---------------- light grid (characters): add bounce from the surrounding surfaces ----
if A.grid:
    o, l = L[15]
    G = np.frombuffer(bytes(d[o:o + l]), np.uint8).reshape(-1, 30).copy()  # 4 ambient + 4 direct rgb, 4 styles, 2 latlong
    gl = lump(17); garr = np.frombuffer(gl, '<u2')
    # grid origin/size from the world model bounds, like the engine
    mins = np.array(struct.unpack_from('<3f', lump(7), 0)); maxs = np.array(struct.unpack_from('<3f', lump(7), 12))
    ents = lump(0).decode('latin1')
    m = re.search(r'"gridsize"\s*"([^"]+)"', ents)
    gsize = np.array([float(x) for x in m.group(1).split()]) if m else np.array([64., 64., 128.])
    gorg = gsize * np.ceil(mins / gsize)
    gmax = gsize * np.floor(maxs / gsize)
    gb = ((gmax - gorg) / gsize + 1).astype(int)
    print('light grid', gb, 'cells', len(garr), 'unique', len(G))
    # one probe per unique grid entry: use the first cell that references it
    first = {}
    for cell in range(len(garr)):
        e = int(garr[cell])
        if e not in first: first[e] = cell
    cells = np.array([first.get(e, 0) for e in range(len(G))])
    cx = cells % gb[0]; cy = (cells // gb[0]) % gb[1]; cz = cells // (gb[0] * gb[1])
    gp = (gorg + np.stack([cx, cy, cz], 1) * gsize).astype(np.float32)
    k = 64
    amb = np.zeros((len(gp), 3), np.float32)
    for s0 in range(0, len(gp), CH):
        p = gp[s0:s0 + CH]
        dirs = rng.normal(size=(len(p) * k, 3)).astype(np.float32)
        dirs /= np.linalg.norm(dirs, axis=1, keepdims=True)
        org = np.repeat(p, k, 0)
        res = scene.run(org, dirs, output=1)
        tri = res['primID']; hit = tri >= 0
        rad = np.zeros((len(org), 3), np.float32)
        if hit.any():
            rad[hit] = sample_lm(total_bounce, tri[hit], res['u'][hit], res['v'][hit])
        amb[s0:s0 + CH] = rad.reshape(-1, k, 3).mean(1)
    valid = G[:, 24] != 255   # style 0 present
    add = np.clip(amb * 255 * 1.5, 0, 255)
    G[valid, 0:3] = np.clip(G[valid, 0:3].astype(np.float32) + add[valid], 0, 255).astype(np.uint8)
    d[o:o + l] = G.tobytes()

open(A.out, 'wb').write(d)
print('wrote', A.out, 'mean lightmap', lm.mean() * 255, '->', newlm.mean())

if A.preview:
    def atlas(a, n=16):
        k = min(len(a), n); cols = 4; rows = (k + cols - 1) // cols
        img = np.zeros((rows * 128, cols * 128, 3), np.uint8)
        for i in range(k): img[(i // cols) * 128:(i // cols + 1) * 128, (i % cols) * 128:(i % cols + 1) * 128] = a[i]
        return img
    a = atlas((lm * 255).astype(np.uint8)); b = atlas(newlm)
    Image.fromarray(np.hstack([a, np.zeros((a.shape[0], 8, 3), np.uint8), b])).save(A.preview)
