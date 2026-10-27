"""Write an edited cape shape (bind pose, same vertex order as LOD0) into a .glm: LOD0 exactly, lower LODs
by carrying the LOD0 displacement over to their nearest LOD0 points. Normals are recomputed from the
triangles (keeping each vertex's old facing).  usage: patch_cape.py in.glm out.glm new_positions.npy [surface]"""
import sys, struct, numpy as np
from scipy.spatial import cKDTree
src, dst, npy = sys.argv[1:4]; SURF = sys.argv[4] if len(sys.argv) > 4 else 'cape'
d = bytearray(open(src, 'rb').read())
numBones, numLODs, ofsLODs, ns, ofsSH = struct.unpack_from('<5i', d, 140)
tab = struct.unpack_from('<%di' % ns, d, 164)
names = [d[164 + tab[i]:164 + tab[i] + 64].split(b'\0')[0].decode() for i in range(ns)]
NEW = np.load(npy)

def surf(lodStart, i):
    so = lodStart + 4 + struct.unpack_from('<i', d, lodStart + 4 + i * 4)[0]
    _, idx, _, nv, ofsV, nt, ofsT, _, _, _ = struct.unpack_from('<10i', d, so)
    return so, idx, nv, ofsV, nt, ofsT

def read(so, nv, ofsV, nt, ofsT):
    P = np.array([struct.unpack_from('<3f', d, so + ofsV + k * 32 + 12) for k in range(nv)])
    N = np.array([struct.unpack_from('<3f', d, so + ofsV + k * 32) for k in range(nv)])
    UV = np.array(struct.unpack_from('<%df' % (nv * 2), d, so + ofsV + nv * 32)).reshape(-1, 2)
    T = np.array(struct.unpack_from('<%di' % (nt * 3), d, so + ofsT)).reshape(-1, 3)
    return P, N, UV, T

def write(so, nv, ofsV, P, N):
    for k in range(nv):
        struct.pack_into('<3f', d, so + ofsV + k * 32, *N[k]); struct.pack_into('<3f', d, so + ofsV + k * 32 + 12, *P[k])

def normals(P, T, Nold):
    N = np.zeros_like(P)
    f = np.cross(P[T[:, 1]] - P[T[:, 0]], P[T[:, 2]] - P[T[:, 0]])
    for c in range(3): np.add.at(N, T[:, c], f)
    # seams: welded positions share one normal
    key = {}
    for k, p in enumerate(np.round(P, 3)): key.setdefault(tuple(p), []).append(k)
    for ks in key.values():
        if len(ks) > 1: N[ks] = N[ks].sum(0)
    N /= np.maximum(np.linalg.norm(N, axis=1, keepdims=True), 1e-9)
    flip = (N * Nold).sum(1) < 0
    N[flip] *= -1
    return N

lodStart = ofsLODs; base = None
for lod in range(numLODs):
    for i in range(ns):
        so, idx, nv, ofsV, nt, ofsT = surf(lodStart, i)
        if names[idx] != SURF: continue
        P, N, UV, T = read(so, nv, ofsV, nt, ofsT)
        if lod == 0:
            assert len(NEW) == nv, (len(NEW), nv)
            base = (P.copy(), NEW - P)
            Pn = NEW
        else:
            P0, D0 = base
            dist, nn = cKDTree(P0).query(P, k=4)
            w = 1.0 / np.maximum(dist, 1e-3); w /= w.sum(1, keepdims=True)
            Pn = P + (D0[nn] * w[..., None]).sum(1)
        write(so, nv, ofsV, Pn, normals(Pn, T, N))
        print('LOD %d %s: %d verts, mean move %.2f' % (lod, SURF, nv, np.linalg.norm(Pn - P, axis=1).mean()))
    lodStart += struct.unpack_from('<i', d, lodStart)[0]
open(dst, 'wb').write(d)
