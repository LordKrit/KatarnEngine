"""close-up previews of a model holding the dooku hilt, the way the game attaches it"""
import sys, numpy as np
sys.path.insert(0, '/home/claude/retarget')
from render import read_glm, skin_matrices, pose, render
from g2 import GLA
from PIL import Image, ImageDraw
def bolt(s, S):
    P = pose([dict(s, flags=0)], S)[0][1][:3]
    sides = [P[(j + 1) % 3] - P[j] for j in range(3)]
    a0 = sides[0] / np.linalg.norm(sides[0]); a1 = sides[2] / np.linalg.norm(sides[2])
    a0 = a0 - np.dot(a0, a1) * a1; a0 /= np.linalg.norm(a0)
    a2 = np.cross(sides[0], sides[2]); a2 /= np.linalg.norm(a2)
    M = np.eye(4); M[:3, 0] = a1; M[:3, 1] = a0; M[:3, 2] = -a2; M[:3, 3] = P[2]; return M
hilt = read_glm('/tmp/hilt/models/weapons2/saber_plasma/saber_dooku.glm')
def scene(glm, gla, f, side='r', blade=float(__import__("os").environ.get("BL","14"))):
    surfs = read_glm(glm); g = GLA(gla); S = skin_matrices(g, f)
    posed = pose(surfs, S)
    tag = [s for s in surfs if s['name'] == '*%s_hand' % side][0]
    M = bolt(tag, S)
    I = [np.eye(4)] * 4096
    for s in hilt:
        if s['name'].startswith('*'):
            if s['name'].startswith('*blade'):
                B = M @ bolt(s, I); o = B[:3, 3]; d = -B[:3, 1]
                # blade as a thin box
                u = np.cross(d, [0, 0, 1.0]); u = u / (np.linalg.norm(u) + 1e-9) * 0.5; v = np.cross(d, u)
                Q = np.array([o + a * u + b * v + t * d for t in (0, blade) for a, b in ((-1, -1), (1, -1), (1, 1), (-1, 1))])
                T = []
                for k in range(4):
                    k2 = (k + 1) % 4; T += [[k, k2, 4 + k], [k2, 4 + k2, 4 + k], [k, 4 + k, k2], [k2, 4 + k, 4 + k2]]
                posed.append((dict(name='cape', shader='HILT', tris=np.array(T)), Q))
            continue
        if s['flags'] & 3: continue
        P = (M[:3, :3] @ s['P'].T).T + M[:3, 3]
        posed.append((dict(s, name='cape', shader='HILT'), P))
    return posed, M[:3, 3]
def crop(posed, centre, yaw, r=float(__import__("os").environ.get("R","26")), size=380, title=''):
    # keep only triangles near the hand, then render
    out = []
    for s, P in posed:
        near = np.linalg.norm(P - centre, axis=1) < r * 0.6
        t = s['tris'][near[s['tris']].all(1)] if len(s['tris']) else s['tris']
        if s.get("shader") == "HILT" and not __import__("os").environ.get("CROPH"): t = s["tris"]
        if len(t):
            used = np.unique(t); remap = -np.ones(len(P), int); remap[used] = np.arange(len(used))
            out.append((dict(s, tris=remap[t]), P[used]))
    return render(out, yaw, size, None, title)
jobs = [l.split('|') for l in sys.argv[2:]]
ims = []
for glm, gla, f, label in jobs:
    posed, c = scene(glm, gla, int(f))
    for yaw in (0, 90, -60):
        ims.append(crop(posed, c, yaw, title='%s f%s yaw%d' % (label, f, yaw)))
cols = 3; W = Image.new('RGB', (380 * cols, 380 * ((len(ims) + cols - 1) // cols)), 'white')
for k, im in enumerate(ims): W.paste(im, ((k % cols) * 380, (k // cols) * 380))
W.save(sys.argv[1]); print('saved')
