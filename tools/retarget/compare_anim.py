"""Left: the source animation on its own rig. Right: the same frames retargeted onto _humanoid, on a JKA model."""
import sys, zipfile, io, os, numpy as np
from PIL import Image
from source import Source
from bonemap import build_map
from fit import Fit
from g2 import GLA
from render import read_glm, skin_matrices, pose, render
src_path, gla_path, new_gla, pk3, anim, first, picks, out = sys.argv[1:9]
src = Source(src_path); gla = GLA(gla_path); m, mp = build_map(src); fit = Fit(src, gla, m, mp)
names = src.animations(); times, frames = src.sample(names.index(anim), 20.0)
meshes = src.meshes()
z = zipfile.ZipFile(pk3); open('/tmp/cmp.glm','wb').write(z.read([n for n in z.namelist() if n.endswith('model.glm')][0]))
tex = {os.path.splitext(n)[0]: np.array(Image.open(io.BytesIO(z.read(n))).convert('RGB').resize((512,512))) for n in z.namelist() if n.lower().endswith(('.png','.jpg'))}
surfs = read_glm('/tmp/cmp.glm'); g2 = GLA(new_gla)
cols = []
for f in [int(x) for x in picks.split(',')]:
    F = frames[f]
    posed = []
    for mm in meshes:
        Jm = np.array([F[j] @ src.ibm[k] for k, j in enumerate(src.joints)])
        # positions came already through skin_to_world; undo for skinning, then convert to JKA space for viewing
        S2W = src.skin_to_world; Sinv = np.linalg.inv(S2W)
        P0 = (mm['pos'] - S2W[:3,3]) @ Sinv[:3,:3].T
        P = np.zeros_like(P0)
        for c in range(mm['J'].shape[1]):
            M = Jm[mm['J'][:, c]]
            P += mm['W'][:, c:c+1] * (np.einsum('nij,nj->ni', M[:, :3, :3], P0) + M[:, :3, 3])
        P = np.array([fit.conv_p(p) for p in P])
        posed.append((dict(tris=mm['tris'][:, [0, 2, 1]], UV=mm['uv'], shader=None, flags=0), P))
    a = render(posed, 0, 300, None, 'source %s f%d' % (anim, f))
    b = render(pose(surfs, skin_matrices(g2, int(first) + f)), 0, 300, tex, 'JKA _humanoid f%d' % f)
    cols.append((a, b))
W = Image.new('RGB', (300 * len(cols), 600), 'white')
for k, (a, b) in enumerate(cols): W.paste(a, (k * 300, 0)); W.paste(b, (k * 300, 300))
W.save(out); print('saved', out)
