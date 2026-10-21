import sys, os, zipfile, io, numpy as np
from PIL import Image
from render import read_glm, skin_matrices, pose, render
from g2 import GLA
pk3, gla_path, outpng = sys.argv[1], sys.argv[2], sys.argv[3]
frames = [('base', None), ('stand', 44800), ('run', 44900+6), ('saber', 424+13), ('lightning', 45355+5)]
if len(sys.argv) > 4: frames = [(f.split('=')[0], int(f.split('=')[1]) if f.split('=')[1] != 'base' else None) for f in sys.argv[4].split(',')]
z = zipfile.ZipFile(pk3); glm = [n for n in z.namelist() if n.endswith('model.glm')][0]
open('/tmp/pv.glm','wb').write(z.read(glm))
tex = {}
for n in z.namelist():
    if n.lower().endswith(('.png','.jpg','.tga')):
        tex[os.path.splitext(n)[0]] = np.array(Image.open(io.BytesIO(z.read(n))).convert('RGB').resize((512,512)))
surfs = read_glm('/tmp/pv.glm'); gla = GLA(gla_path)
ims = []
for label, f in frames:
    S = [np.eye(4)] * gla.numBones if f is None else skin_matrices(gla, f)
    P = pose(surfs, S)
    for yaw in (0, 90):
        ims.append(render(P, yaw, 360, tex, '%s %s' % (label, 'front' if yaw == 0 else 'side')))
W = Image.new('RGB', (360 * len(frames), 720), 'white')
for k, im in enumerate(ims): W.paste(im, ((k // 2) * 360, (k % 2) * 360))
W.save(outpng); print('saved', outpng)
