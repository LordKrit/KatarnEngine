"""Textures, rend2 shaders, NPC and pk3s for the two Kenobi Vader models."""
import os, shutil, zipfile, numpy as np
from PIL import Image
T = '/home/claude/kv/in/Kenobi_Darth_Vader/Textures/'
NPC_SRC = open('/tmp/vn/ext_data/npcs/md_char_ot.npc').read().splitlines()
# md_vader (the main Movie Duels Darth Vader entry): settings and sounds
a = NPC_SRC.index('md_vader'); b = a + NPC_SRC[a:].index('}')
NPC = '\n'.join(NPC_SRC[a:b + 1])

PARTS = {  # shader name -> (texture prefix, size)
    'body': ('Body', 2048), 'gloves': ('Gloves', 1024), 'helmet': ('Helmet', 2048), 'cape': ('Cape', 1024),
    'innercape': ('Duster', 1024), 'head': ('Head', 2048), 'eyes': ('Eyes', 512)}
ROUGH = {'body': 0.7, 'gloves': 0.72, 'helmet': 0.55, 'innercape': 0.9, 'cape': 0.95}
GLOW = {  # shader name -> (emissive file, glow gain): emissive maps get their own additive glow stage
    'default': {'body': ('Default/Body_Emissive.png', 1.0)},
    'broken': {'body': ('Default/Body_Emissive.png', 1.0), 'helmet': ('Damaged/Helmet_Emissive.png', 2.2),
               'eyes': ('Damaged/Eyes_Emissive.png', 1.6)},
}


def load(path, sz, mode):
    return np.asarray(Image.open(path).convert(mode).resize((sz, sz), Image.LANCZOS)).astype(np.float32) / 255


def build(name, variant, glb_dir):
    src = T + ('Default/' if variant == 'default' else 'Damaged/')
    work = '/home/claude/kv/pk_' + name
    if os.path.isdir(work): shutil.rmtree(work)
    with zipfile.ZipFile('/home/claude/kv/zzz_%s.pk3' % name) as z: z.extractall(work)
    od = os.path.join(work, 'models/players', name)
    for f in os.listdir(od):
        if f.endswith('.png'): os.remove(os.path.join(od, f))
    sh = '// Darth Vader (Obi-Wan Kenobi series), Battlefront II model%s: rend2 normal + roughness/metal/AO maps\n\n' % (
        ' - broken helmet, glowing burns' if variant == 'broken' else '')
    for k, (pre, sz) in PARTS.items():
        if not os.path.exists(src + pre + '_Base_color.png'): continue
        Image.open(src + pre + '_Base_color.png').convert('RGB').resize((sz, sz), Image.LANCZOS).save(os.path.join(od, k + '.jpg'), quality=93)
        nrm = src + pre + '_Normal_OpenGL.png'
        Image.open(nrm).convert('RGB').resize((sz, sz), Image.LANCZOS).save(os.path.join(od, k + '_n.png'), optimize=True)
        r = load(src + pre + '_Roughness.png', sz, 'L')
        # glossier, BF2-style look: leather suit and gloves with a sheen, mirror-like helmet and plates
        r = np.clip(r * ROUGH.get(k, 1.0), 0.08, 1.0)
        m = load(src + pre + '_Metallic.png', sz, 'L') if os.path.exists(src + pre + '_Metallic.png') else np.zeros_like(r)
        ao = load(src + pre + '_Mixed_AO.png', sz, 'L') if os.path.exists(src + pre + '_Mixed_AO.png') else np.ones_like(r)
        ao = 0.5 + 0.5 * ao                 # softened, as on the other BF2 Vaders
        Image.fromarray((np.dstack([r, m, ao]) * 255).astype(np.uint8)).save(os.path.join(od, k + '_rmo.png'), optimize=True)
        # capes are single sheets; the broken helmet is open, so its inside must draw too
        cull = '\tcull disable\n' if k in ('cape', 'innercape') or (k == 'helmet' and variant == 'broken') else ''
        skin = '\t\tskin 1.0 0.45 0.3 0.6\n' if k == 'head' else ''
        glow = ''
        g = GLOW[variant].get(k)
        if g:
            e = Image.open(T + g[0]).convert('RGB').resize((sz, sz), Image.LANCZOS)
            e = Image.fromarray(np.clip(np.asarray(e).astype(np.float32) * g[1], 0, 255).astype(np.uint8))
            e.save(os.path.join(od, k + '_glow.jpg'), quality=95)
            glow = '\t{\n\t\tmap models/players/%s/%s_glow\n\t\tblendFunc GL_ONE GL_ONE\n\t\tglow\n\t\trgbGen identity\n\t}\n' % (name, k)
        sh += ('models/players/%s/%s\n{\n%s\t{\n\t\tmap models/players/%s/%s\n\t\tnormalMap models/players/%s/%s_n\n'
               '\t\trmoMap models/players/%s/%s_rmo\n%s\t\trgbGen lightingDiffuse\n\t}\n%s}\n') % (
            name, k, cull, name, k, name, k, name, k, skin, glow)
        print(name, k, 'glow' if g else '')
    os.makedirs(os.path.join(work, 'shaders'), exist_ok=True)
    open(os.path.join(work, 'shaders', name + '.shader'), 'w').write(sh)
    npc = NPC.replace('md_vader', name, 1).replace('playerModel\tdarthvader', 'playerModel\t' + name)
    npc = npc.replace('"Darth Vader"', '"Darth Vader (Kenobi%s, BF2)"' % (', broken helmet' if variant == 'broken' else ''))
    os.makedirs(os.path.join(work, 'ext_data/npcs'), exist_ok=True)
    open(os.path.join(work, 'ext_data/npcs', name + '.npc'), 'w').write(npc + '\n')
    shutil.copy('/home/claude/vader_in/darthvader/sounds.cfg', od)
    # two pk3s: the skeleton (big) on its own
    face = lambda p: os.path.basename(p).split('.')[0].split('_')[0] in ('helmet', 'head', 'eyes') and '_humanoid_' not in p
    for out, pred in (('/mnt/user-data/outputs/zzz_%s_skel.pk3' % name, lambda p: '_humanoid_' in p),
                      ('/mnt/user-data/outputs/zzz_%s_tex.pk3' % name, face),
                      ('/mnt/user-data/outputs/zzz_%s.pk3' % name, lambda p: '_humanoid_' not in p and not face(p))):
        with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
            for root, _, files in os.walk(work):
                for f in files:
                    full = os.path.join(root, f); rel = os.path.relpath(full, work)
                    if pred(rel): z.write(full, rel)
        print(out, os.path.getsize(out))


build('vader_kenobi', 'default', 'out_reg')
build('vader_kenobi_broken', 'broken', 'out_dmg')
print(NPC.splitlines()[:6])
