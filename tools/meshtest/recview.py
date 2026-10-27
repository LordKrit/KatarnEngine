"""Render frames of a harness recording: body skinned from the recorded bones, cloth from the recorded vertices.
usage: recview.py model.glm model.gla rec.bin cloth_surface,... out.png t1,t2,..."""
import sys, struct, numpy as np
sys.path.insert(0, '/home/claude/md/tools/retarget')
from render import read_glm, render
from g2 import GLA
from PIL import Image
glm, gla, rec, cloth, out, times = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4].split(','), sys.argv[5], [float(x) for x in sys.argv[6].split(',')]
s = [x for x in read_glm(glm)]
g = GLA(gla); nb = g.numBones
cl = [x for x in s if x['name'] in cloth]
ncv = sum(len(x['P']) for x in cl)
d = open(rec, 'rb').read()
fsz = 20 + nb * 48 + ncv * 6
nf = len(d) // fsz
frames = []
for k in range(nf):
    o = k * fsz
    t = struct.unpack_from('<f', d, o)[0]
    B = np.frombuffer(d, np.float32, nb * 12, o + 20).reshape(nb, 3, 4)
    C = np.frombuffer(d, np.int16, ncv * 3, o + 20 + nb * 48).reshape(-1, 3) / 64.0
    frames.append((t, B, C))
ims = []
for tw in times:
    t, B, C = min(frames, key=lambda f: abs(f[0] - tw))
    posed = []; off = 0
    for x in s:
        if x['flags'] & 3 or not len(x['P']): continue
        if x['name'] in cloth:
            n = len(x['P']); P = C[off:off + n]; off += n
        else:
            P = np.zeros_like(x['P'])
            for i, ws in enumerate(x['W']):
                for b, w in ws: P[i] += w * (B[b][:, :3] @ x['P'][i] + B[b][:, 3])
        posed.append((x, P))
    for yaw in (0, 90):
        im = render(posed, yaw, 420, None, 't=%.2f yaw%d' % (t, yaw))
        ims.append(im if isinstance(im, Image.Image) else Image.fromarray(np.asarray(im)))
W = Image.new('RGB', (420 * len(ims), 420), 'white')
for k, im in enumerate(ims): W.paste(im, (k * 420, 0))
W.save(out); print('saved', out, nf, 'frames')
