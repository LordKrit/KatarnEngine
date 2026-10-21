"""Put the BF2 model's saber bolts where the OG Dooku's are, relative to each closed hand.
usage: patch_bolt.py og.glm og.gla bf.glm bf.gla out.glm frames"""
import sys, struct, numpy as np
sys.path.insert(0, '/home/claude/retarget')
from render import read_glm, skin_matrices, pose
from g2 import GLA
ogm, oga, bfm, bfa, out = sys.argv[1:6]; frames = [int(x) for x in sys.argv[6].split(',')]
def bolt_world(P):
    s0 = P[1] - P[0]; s2 = P[0] - P[2]
    a1 = s2 / np.linalg.norm(s2); a0 = s0 - np.dot(s0, a1) * a1; a0 /= np.linalg.norm(a0)
    a2 = np.cross(s0, s2); a2 /= np.linalg.norm(a2)
    M = np.eye(4); M[:3, 0] = a1; M[:3, 1] = a0; M[:3, 2] = -a2; M[:3, 3] = P[2]; return M
def hand_frame(g, S, side):
    h = g.byName[side + 'hand']; W = S[h] @ g.bones[h]['base']
    R = W[:3, :3] / np.linalg.norm(W[:3, 0])
    fing = []
    c = W[:3, 3]
    F = np.eye(4); F[:3, :3] = R; F[:3, 3] = c; return F
OS = read_glm(ogm); BS = read_glm(bfm); OG = GLA(oga); BG = GLA(bfa)
d = bytearray(open(bfm, 'rb').read())
numBones, numLODs, ofsLODs, ns, ofsSH = struct.unpack_from('<5i', d, 140)
tab = struct.unpack_from('<%di' % ns, d, 164)
names = [d[164 + tab[i]:164 + tab[i] + 64].split(b'\0')[0].decode() for i in range(ns)]
for side in 'rl':
    tn = '*%s_hand' % side
    rels = []
    for f in frames:
        So = skin_matrices(OG, f)
        t = [s for s in OS if s['name'] == tn][0]
        Mo = bolt_world(pose([dict(t, flags=0)], So)[0][1])
        rels.append(np.linalg.inv(hand_frame(OG, So, side)) @ Mo)
    rel = np.mean(rels, 0); U, _, Vt = np.linalg.svd(rel[:3, :3]); rel[:3, :3] = U @ Vt
    print(side, 'offset from fist (hand axes)', np.round(rel[:3, 3], 2), 'spread', np.round(np.std([r[:3, 3] for r in rels], 0), 2))
    f = frames[0]; Sb = skin_matrices(BG, f)
    want = hand_frame(BG, Sb, side) @ rel
    X, Y, O = want[:3, 0], want[:3, 1], want[:3, 3]
    Wv = [O + 2 * X, O + 2 * X + 3 * Y, O]          # V0, V1, V2(origin)
    # write into every LOD's copy of the tag surface (tags are rigid on one bone)
    lso = ofsLODs
    for L in range(numLODs):
        base = lso + 4
        for i in range(ns):
            so = base + struct.unpack_from('<i', d, base + i * 4)[0]
            _, idx, _, nv, ofsV, nt, ofsT, nbr, ofsBR, _ = struct.unpack_from('<10i', d, so)
            if names[idx] != tn: continue
            refs = struct.unpack_from('<%di' % nbr, d, so + ofsBR)
            for k in range(3):
                o = so + ofsV + k * 32; u32 = struct.unpack_from('<I', d, o + 24)[0]
                b = refs[u32 & 31]; inv = np.linalg.inv(Sb[b])
                p = (inv @ np.append(Wv[k], 1))[:3]
                struct.pack_into('<3f', d, o + 12, *p)
        lso = lso + struct.unpack_from('<i', d, lso)[0]
open(out, 'wb').write(d); print('wrote', out)
