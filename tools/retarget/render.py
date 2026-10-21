"""Tiny software renderer for checking .glm models on a .gla (no GPU needed)."""
import struct, sys, os, io
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from g2 import GLA, uncomp


def read_glm(path):
    d = open(path, 'rb').read()
    numBones, numLODs, ofsLODs, ns, ofsSH = struct.unpack_from('<5i', d, 140)
    tab = struct.unpack_from('<%di' % ns, d, 164)
    hier = []
    for i in range(ns):
        p = 164 + tab[i]
        hier.append((d[p:p + 64].split(b'\0')[0].decode(), struct.unpack_from('<I', d, p + 64)[0],
                     d[p + 68:p + 132].split(b'\0')[0].decode()))
    lso = ofsLODs + 4
    surfs = []
    for i in range(ns):
        so = lso + struct.unpack_from('<i', d, lso + i * 4)[0]
        _, idx, _, nv, ofsV, nt, ofsT, nbr, ofsBR, _ = struct.unpack_from('<10i', d, so)
        name, flags, shader = hier[idx]
        tris = np.array(struct.unpack_from('<%di' % (nt * 3), d, so + ofsT)).reshape(-1, 3)
        refs = struct.unpack_from('<%di' % nbr, d, so + ofsBR)
        P = np.zeros((nv, 3)); N = np.zeros((nv, 3)); W = []
        for k in range(nv):
            o = so + ofsV + k * 32
            N[k] = struct.unpack_from('<3f', d, o); P[k] = struct.unpack_from('<3f', d, o + 12)
            u32 = struct.unpack_from('<I', d, o + 24)[0]; wb = d[o + 28:o + 32]
            nw = (u32 >> 30) + 1; tot = 0; ws = []
            for w in range(nw):
                b = (u32 >> (5 * w)) & 31
                if w < nw - 1:
                    val = (wb[w] | (((u32 >> (20 + 2 * w)) & 3) << 8)) / 1023.0; tot += val
                else:
                    val = 1 - tot
                ws.append((refs[b], val))
            W.append(ws)
        UV = np.array(struct.unpack_from('<%df' % (nv * 2), d, so + ofsV + nv * 32)).reshape(-1, 2)
        surfs.append(dict(name=name, flags=flags, shader=shader, P=P, N=N, W=W, UV=UV, tris=tris))
    return surfs


def skin_matrices(gla, frame):
    nb = gla.numBones; S = [None] * nb
    for i in gla.order:
        b = gla.bones[i]
        L = uncomp(gla.comp(frame, i))
        S[i] = L if b['parent'] < 0 else S[b['parent']] @ L
    return S


def pose(surfs, S):
    out = []
    for s in surfs:
        if s['flags'] & 3: continue
        P = np.zeros_like(s['P'])
        for k, ws in enumerate(s['W']):
            for b, w in ws:
                P[k] += w * (S[b][:3, :3] @ s['P'][k] + S[b][:3, 3])
        out.append((s, P))
    return out


def render(posed, yaw_deg=0, size=420, textures=None, title=None):
    img = np.full((size, size, 3), 235, np.uint8)
    zb = np.full((size, size), -1e9)
    yaw = np.radians(yaw_deg)
    # JKA: z up, model faces -y. Camera looks at the front from -y, then rotated by yaw
    R = np.array([[np.cos(yaw), np.sin(yaw), 0], [-np.sin(yaw), np.cos(yaw), 0], [0, 0, 1]])
    allp = np.vstack([P for _, P in posed]) @ R.T
    lo, hi = allp.min(0), allp.max(0)
    span = max(hi[0] - lo[0], hi[2] - lo[2]) * 1.1
    cx, cz = (lo[0] + hi[0]) / 2, (lo[2] + hi[2]) / 2
    light = np.array([0.4, -0.7, 0.6]); light /= np.linalg.norm(light)
    for s, P in posed:
        Q = P @ R.T
        X = (Q[:, 0] - cx) / span * size + size / 2
        Y = size / 2 - (Q[:, 2] - cz) / span * size
        Z = -Q[:, 1]
        tex = textures.get(s['shader']) if textures else None
        for t in s['tris']:
            a, b, c = t
            e1 = Q[b] - Q[a]; e2 = Q[c] - Q[a]
            n = np.cross(e1, e2); ln = np.linalg.norm(n)
            if ln < 1e-12: continue
            n /= ln
            # Ghoul2 triangles are clockwise: the front faces the camera when n.y > 0
            if n[1] <= 0 and s['name'] != 'cape': continue   # the cape is two-sided in game (cull disable)
            shade = 0.35 + 0.65 * max(0.0, -np.dot(n, light * np.array([1, 1, 1])) if False else abs(np.dot(n, light)))
            xs = np.array([X[a], X[b], X[c]]); ys = np.array([Y[a], Y[b], Y[c]])
            x0, x1 = int(max(0, np.floor(xs.min()))), int(min(size - 1, np.ceil(xs.max())))
            y0, y1 = int(max(0, np.floor(ys.min()))), int(min(size - 1, np.ceil(ys.max())))
            if x1 < x0 or y1 < y0: continue
            gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
            den = (ys[1] - ys[2]) * (xs[0] - xs[2]) + (xs[2] - xs[1]) * (ys[0] - ys[2])
            if abs(den) < 1e-12: continue
            w0 = ((ys[1] - ys[2]) * (gx - xs[2]) + (xs[2] - xs[1]) * (gy - ys[2])) / den
            w1 = ((ys[2] - ys[0]) * (gx - xs[2]) + (xs[0] - xs[2]) * (gy - ys[2])) / den
            w2 = 1 - w0 - w1
            inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
            if not inside.any(): continue
            z = w0 * Z[a] + w1 * Z[b] + w2 * Z[c]
            sub = zb[y0:y1 + 1, x0:x1 + 1]
            mask = inside & (z > sub)
            if not mask.any(): continue
            sub[mask] = z[mask]
            if tex is not None:
                u = w0 * s['UV'][a, 0] + w1 * s['UV'][b, 0] + w2 * s['UV'][c, 0]
                v = w0 * s['UV'][a, 1] + w1 * s['UV'][b, 1] + w2 * s['UV'][c, 1]
                th, tw = tex.shape[:2]
                col = tex[np.clip((v % 1) * th, 0, th - 1).astype(int), np.clip((u % 1) * tw, 0, tw - 1).astype(int)][:, :, :3]
                col = (col * shade).astype(np.uint8)
                img[y0:y1 + 1, x0:x1 + 1][mask] = col[mask]
            else:
                img[y0:y1 + 1, x0:x1 + 1][mask] = (np.array([150, 160, 185]) * shade).astype(np.uint8)
    im = Image.fromarray(img)
    if title:
        from PIL import ImageDraw
        ImageDraw.Draw(im).text((6, 6), title, fill=(20, 20, 20))
    return im
