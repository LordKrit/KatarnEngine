"""Replace cloth surfaces of a .glm with new meshes (bind pose, JKA units, weights by bone name).
Each LOD gets the full new mesh.  usage: swap_cape.py in.glm in.gla mesh.pkl out.glm surf_for_mat0 surf_for_mat1 ..."""
import sys, struct, pickle, numpy as np
sys.path.insert(0, '/home/claude/md/tools/retarget')
from g2 import GLA
src, gla, pk, dst = sys.argv[1:5]; SURF_FOR_MAT = sys.argv[5:]
d = open(src, 'rb').read()
G = GLA(gla); byname = {k.lower(): v for k, v in G.byName.items()}
D = pickle.load(open(pk, 'rb'))
numBones, numLODs, ofsLODs, ns, ofsSH = struct.unpack_from('<5i', d, 140)
tab = struct.unpack_from('<%di' % ns, d, 164)
names = [d[164 + tab[i]:164 + tab[i] + 64].split(b'\0')[0].decode() for i in range(ns)]

def build(mat):
    """vertices split per (vertex, uv) for this material's faces"""
    idx = {}; V = []; T = []
    for mi, vs, uvs in D['faces']:
        if mi != mat: continue
        tri = []
        for v, uv in zip(vs, uvs):
            k = (v, round(uv[0], 5), round(uv[1], 5))
            if k not in idx: idx[k] = len(V); V.append(k)
            tri.append(idx[k])
        # fan-triangulate (input is triangles already)
        for j in range(1, len(tri) - 1): T.append((tri[0], tri[j], tri[j + 1]))
    return V, T

def surface_bytes(surfIndex, V, T):
    refs = []
    vb = bytearray(); ub = bytearray()
    for v, u0, u1 in V:
        ws = sorted(D['W'][v], key=lambda t: -t[1])[:4]
        tot = sum(w for _, w in ws); ws = [(n, w / tot) for n, w in ws]
        bis = []
        for n, w in ws:
            b = byname[n.lower()]
            if b not in refs: refs.append(b)
            bis.append(refs.index(b))
        assert len(refs) <= 32, 'too many bones on one surface'
        nw = len(ws); u32 = (nw - 1) << 30; wb = [0, 0, 0, 0]
        for k, bi in enumerate(bis): u32 |= bi << (5 * k)
        for k in range(nw - 1):
            q = int(round(ws[k][1] * 1023)); wb[k] = q & 255; u32 |= ((q >> 8) & 3) << (20 + 2 * k)
        vb += struct.pack('<3f3fI4B', *D['N'][v], *D['P'][v], u32, *wb)
        ub += struct.pack('<2f', u0, 1.0 - u1)
    tb = b''.join(struct.pack('<3i', *t) for t in T)
    rb = b''.join(struct.pack('<i', r) for r in refs)
    H = 40; ofsT = H; ofsR = ofsT + len(tb); ofsV = ofsR + len(rb); end = ofsV + len(vb) + len(ub)
    hdr = struct.pack('<10i', 0, surfIndex, 0, len(V), ofsV, len(T), ofsT, len(refs), ofsR, end)
    return bytearray(hdr + tb + rb + vb + ub)

new = {}
for mat, sname in enumerate(SURF_FOR_MAT):
    si = names.index(sname); V, T = build(mat)
    new[si] = (V, T); print('%s <- material %d (%s): %d verts, %d tris' % (sname, mat, D['mats'][mat], len(V), len(T)))

out = bytearray(d[:ofsLODs])
lodStart = ofsLODs
for lod in range(numLODs):
    lodEnd = lodStart + struct.unpack_from('<i', d, lodStart)[0]
    offs = struct.unpack_from('<%di' % ns, d, lodStart + 4)
    blobs = []
    for i in range(ns):
        so = lodStart + 4 + offs[i]
        if i in new: b = surface_bytes(i, *new[i])
        else: b = bytearray(d[so:so + struct.unpack_from('<i', d, so + 36)[0]])
        blobs.append(b)
    base = len(out); tableLen = 4 + 4 * ns
    pos = tableLen; newOffs = []
    for b in blobs: newOffs.append(pos - 4); pos += len(b)   # offsets are relative to the table start (lod+4)
    out += struct.pack('<i', pos)
    out += struct.pack('<%di' % ns, *newOffs)
    for b in blobs:
        struct.pack_into('<i', b, 8, -len(out))   # ofsHeader: back to the file start
        out += b
    lodStart = lodEnd
struct.pack_into('<i', out, 160, len(out))   # header ofsEnd
open(dst, 'wb').write(out)
print('wrote', dst, len(out))
