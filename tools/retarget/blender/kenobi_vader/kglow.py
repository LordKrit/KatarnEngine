"""Kenobi Vaders: chest box and belt lights glow like the ROTS ones (bright red/green pixels of the
colour map), combined with BF2's own body glow map. Rewrites body_glow.jpg and repacks the main pk3s."""
import os, zipfile, numpy as np
from PIL import Image, ImageFilter
T = '/home/claude/kv/in/Kenobi_Darth_Vader/Textures/'
emis = np.asarray(Image.open(T + 'Default/Body_Emissive.png').convert('RGB').resize((2048, 2048), Image.LANCZOS)).astype(np.float32)
for name, sub in (('vader_kenobi', 'Default'), ('vader_kenobi_broken', 'Damaged')):
    a = np.asarray(Image.open(T + sub + '/Body_Base_color.png').convert('RGB').resize((2048, 2048), Image.LANCZOS)).astype(np.float32)
    mx = a.max(2); mn = a.min(2); sat = (mx - mn) / np.maximum(mx, 1)
    m = np.clip((sat - 0.35) / 0.2, 0, 1) * np.clip((mx - 70) / 40, 0, 1)
    m = np.asarray(Image.fromarray((m * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(1.5))).astype(np.float32) / 255
    # only near the lights BF2 marks as emissive (box and belt): the damaged body's red burns stay unlit
    near = np.asarray(Image.fromarray(((emis.max(2) > 20) * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(41))) > 0
    m = m * near
    g = np.maximum(a * m[..., None] * 1.6, emis * 1.6)
    work = '/home/claude/kv/pk_' + name
    Image.fromarray(np.clip(g, 0, 255).astype(np.uint8)).save(os.path.join(work, 'models/players', name, 'body_glow.jpg'), quality=95)
    print(name, 'lit %.3f%%' % ((g.max(2) > 40).mean() * 100))
    face = lambda p: os.path.basename(p).split('.')[0].split('_')[0] in ('helmet', 'head', 'eyes') and '_humanoid_' not in p
    out = '/mnt/user-data/outputs/zzz_%s.pk3' % name
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        for root, _, files in os.walk(work):
            for f in files:
                full = os.path.join(root, f); rel = os.path.relpath(full, work)
                if '_humanoid_' not in rel and not face(rel): z.write(full, rel)
    print(out, os.path.getsize(out))
