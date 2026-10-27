"""Glow map for the ROTS Vader body: the chest box and belt lights (the saturated red/green
pixels of the colour map), plus an additive glow stage in the body shader."""
import re, numpy as np
from PIL import Image, ImageFilter
a = np.asarray(Image.open('/home/claude/vbf2/in/tex/Textures/Body_Base_Color.png').convert('RGB')).astype(np.float32)
mx = a.max(2); mn = a.min(2); sat = (mx - mn) / np.maximum(mx, 1)
m = np.clip((sat - 0.35) / 0.2, 0, 1) * np.clip((mx - 70) / 40, 0, 1)
m = np.asarray(Image.fromarray((m * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(1.5))).astype(np.float32) / 255
g = np.clip(a * m[..., None] * 1.6, 0, 255).astype(np.uint8)
img = Image.fromarray(g).resize((2048, 2048), Image.LANCZOS)
for d, k in (('pk', 'vader_bf2'), ('pkr', 'vader_bf2_rots')):
    img.save('/home/claude/vbf2/%s/models/players/%s/body_glow.jpg' % (d, k), quality=95)
    p = '/home/claude/vbf2/%s/shaders/%s.shader' % (d, k)
    s = open(p).read()
    if 'body_glow' not in s:
        blk = 'models/players/%s/body\n{' % k
        i = s.index(blk); j = s.index('\n}\n', i)
        s = s[:j] + '\n\t{\n\t\tmap models/players/%s/body_glow\n\t\tblendFunc GL_ONE GL_ONE\n\t\tglow\n\t\trgbGen identity\n\t}' % k + s[j:]
        open(p, 'w').write(s)
    print(k, 'lit pixels %.3f%%' % ((m > 0.3).mean() * 100))
print(open('/home/claude/vbf2/pkr/shaders/vader_bf2_rots.shader').read()[:700])
