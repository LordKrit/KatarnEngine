"""Blender -> JK "HD world" exporter (run inside Blender):

  blender -b scene.blend --python export_hdw.py -- <out dir> <map name> [units-per-metre]

Writes, under <out dir>:
  maps/<name>.hdw                  chunked render geometry (see code/rd-rend2 tr_hdworld.cpp)
  maps/<name>.hdw.json             bounds, suggested spawn, sun
  textures/hd/<name>/<tex>.jpg     colour (Frostbite _CS rgb)
  textures/hd/<name>/<tex>_n.png   normal (from _NAM rg)
  textures/hd/<name>/<tex>_rmo.png roughness (1 - _CS alpha) / metal / AO (_NAM b)
  shaders/hd_<name>.shader         one shader per material

Geometry is grouped per material and split into cubic chunks so the renderer can cull it.
Frostbite-imported scenes (BF2 EA) are metric and Z-up; JK units are ~inches.
"""
import bpy, bmesh, sys, os, struct, json, math, re
import numpy as np

argv = sys.argv[sys.argv.index('--') + 1:]
OUT, NAME = argv[0], argv[1]
UPM = float(argv[2]) if len(argv) > 2 else 39.37
CHUNK = 1024.0
os.makedirs(os.path.join(OUT, 'maps'), exist_ok=True)
TEXDIR = os.path.join(OUT, 'textures', 'hd', NAME)
os.makedirs(TEXDIR, exist_ok=True)
os.makedirs(os.path.join(OUT, 'shaders'), exist_ok=True)

def clean(s):
    return re.sub(r'[^A-Za-z0-9_]', '_', s).lower()[:48]

# ---------------- materials / textures ----------------
def image_array(img):
    w, h = img.size
    a = np.empty(w * h * 4, np.float32)
    img.pixels.foreach_get(a)
    return a.reshape(h, w, 4)[::-1]      # Blender stores bottom-up

from PIL import Image

def find_images(mat):
    cs = nam = None
    if mat and mat.use_nodes:
        for n in mat.node_tree.nodes:
            if n.type != 'TEX_IMAGE' or not n.image or not n.outputs[0].links:
                continue
            to = n.outputs[0].links[0].to_node.type
            if to == 'GROUP' and cs is None: cs = n.image
            elif to == 'SEPARATE_COLOR' and nam is None: nam = n.image
            elif to == 'BSDF_PRINCIPLED' and cs is None: cs = n.image
    return cs, nam

mat_info = {}
def material_shader(mat):
    key = mat.name if mat else '__none'
    if key in mat_info:
        return mat_info[key]['shader']
    shader = 'textures/hd/%s/%s' % (NAME, clean(key))
    info = {'shader': shader, 'alpha': False}
    cs, nam = find_images(mat)
    base = os.path.join(TEXDIR, clean(key))
    try:
        if cs:
            c = image_array(cs)
            rgb = (np.clip(c[..., :3], 0, 1) * 255).astype(np.uint8)
            alpha = None
            if nam:
                nm = image_array(nam)
                if nm.shape[:2] != c.shape[:2]:
                    nm = np.asarray(Image.fromarray((nm * 255).astype(np.uint8)).resize((c.shape[1], c.shape[0])), np.float32) / 255
                if mat is not None and getattr(mat, 'blend_method', 'OPAQUE') != 'OPAQUE' and nm[..., 3].min() < 0.5:
                    alpha = (nm[..., 3] * 255).astype(np.uint8); info['alpha'] = True
                # normal: rg -> xyz
                x = nm[..., 0] * 2 - 1; y = nm[..., 1] * 2 - 1
                z = np.sqrt(np.clip(1 - x * x - y * y, 0, 1))
                n = np.stack([x, y, z], -1) * 0.5 + 0.5
                Image.fromarray((n * 255).astype(np.uint8)).save(base + '_n.png', optimize=True)
                ao = nm[..., 2]
            else:
                ao = np.ones(c.shape[:2], np.float32)
            rough = 1.0 - np.clip(c[..., 3], 0, 1)
            if rough.mean() < 0.08: rough[:] = 0.63            # no smoothness channel
            rough = np.maximum(rough, 0.2)
            if ao.mean() < 0.25: ao = np.ones_like(ao)        # blue channel isn't AO here
            rmo = np.stack([rough, np.zeros_like(rough), ao], -1)
            Image.fromarray((rmo * 255).astype(np.uint8)).save(base + '_rmo.png', optimize=True)
            if alpha is not None:
                Image.fromarray(np.dstack([rgb, alpha])).save(base + '.png', optimize=True)
            else:
                Image.fromarray(rgb).save(base + '.jpg', quality=92)
            info['tex'] = True
        else:
            col = (0.7, 0.7, 0.7)
            if mat and mat.use_nodes:
                for n in mat.node_tree.nodes:
                    if n.type == 'BSDF_PRINCIPLED':
                        col = tuple(n.inputs['Base Color'].default_value[:3]); break
            elif mat:
                col = tuple(mat.diffuse_color[:3])
            c = (np.clip(np.array(col), 0, 1) ** (1 / 2.2) * 255).astype(np.uint8)
            Image.fromarray(np.tile(c, (8, 8, 1))).save(base + '.jpg', quality=95)
            info['tex'] = True
    except Exception as e:
        print('texture failed for', key, e)
    mat_info[key] = info
    return shader

# ---------------- geometry ----------------
chunks = {}      # (shader, cx, cy, cz) -> [positions, normals, uvs]
deps = bpy.context.evaluated_depsgraph_get()
mn = np.full(3, 1e9); mx = np.full(3, -1e9)
ntri = 0
for ob in bpy.data.objects:
    if ob.type != 'MESH' or ob.hide_render:
        continue
    ev = ob.evaluated_get(deps)
    me = ev.to_mesh()
    if not me.polygons:
        ev.to_mesh_clear(); continue
    me.calc_loop_triangles()
    try: me.calc_normals_split()
    except Exception: pass
    M = np.array(ob.matrix_world)
    nv = len(me.vertices)
    co = np.empty(nv * 3, np.float32); me.vertices.foreach_get('co', co); co = co.reshape(-1, 3)
    co = (co @ M[:3, :3].T + M[:3, 3]) * UPM
    nl = len(me.loops)
    lnor = np.empty(nl * 3, np.float32); me.loops.foreach_get('normal', lnor); lnor = lnor.reshape(-1, 3)
    N3 = np.linalg.inv(M[:3, :3]).T
    lnor = lnor @ N3.T; lnor /= np.maximum(np.linalg.norm(lnor, axis=1, keepdims=True), 1e-9)
    if me.uv_layers.active:
        uv = np.empty(nl * 2, np.float32); me.uv_layers.active.data.foreach_get('uv', uv); uv = uv.reshape(-1, 2)
        uv[:, 1] = 1.0 - uv[:, 1]
    else:
        uv = np.zeros((nl, 2), np.float32)
    nt = len(me.loop_triangles)
    tl = np.empty(nt * 3, np.int32); me.loop_triangles.foreach_get('loops', tl); tl = tl.reshape(-1, 3)
    tv = np.empty(nt * 3, np.int32); me.loop_triangles.foreach_get('vertices', tv); tv = tv.reshape(-1, 3)
    tm = np.empty(nt, np.int32); me.loop_triangles.foreach_get('material_index', tm)
    mats = [ms.material for ms in ob.material_slots] or [None]
    P = co[tv]                                  # (nt,3,3)
    if np.linalg.det(M[:3, :3]) < 0:            # mirrored object: keep winding
        P = P[:, ::-1]; tl = tl[:, ::-1]
    cen = P.mean(1)
    cell = np.floor(cen / CHUNK).astype(int)
    mn = np.minimum(mn, P.reshape(-1, 3).min(0)); mx = np.maximum(mx, P.reshape(-1, 3).max(0))
    for mi in np.unique(tm):
        mat = mats[mi] if mi < len(mats) else None
        sh = material_shader(mat)
        sel = tm == mi
        for c in np.unique(cell[sel], axis=0):
            s2 = sel & np.all(cell == c, axis=1)
            k = (sh, int(c[0]), int(c[1]), int(c[2]))
            ch = chunks.setdefault(k, [[], [], []])
            ch[0].append(P[s2].reshape(-1, 3)); ch[1].append(lnor[tl[s2]].reshape(-1, 3)); ch[2].append(uv[tl[s2]].reshape(-1, 2))
    ntri += nt
    ev.to_mesh_clear()
print('triangles', ntri, 'chunks', len(chunks), 'materials', len(mat_info))

# recentre so the map sits around the origin (JK world limits)
centre = np.round((mn + mx) / 2 / 64) * 64
centre[2] = round(mn[2] / 64) * 64 + 0   # floor of the scene near z = 0
shaders = sorted({k[0] for k in chunks})
sidx = {s: i for i, s in enumerate(shaders)}

with open(os.path.join(OUT, 'maps', NAME + '.hdw'), 'wb') as f:
    f.write(b'HDW1'); f.write(struct.pack('<III', 1, len(shaders), len(chunks)))
    for s in shaders:
        f.write(s.encode()[:63].ljust(64, b'\0'))
    for k, (ps, ns, us) in chunks.items():
        p = np.concatenate(ps) - centre; n = np.concatenate(ns); u = np.concatenate(us)
        # weld identical vertices to get an index buffer
        v = np.concatenate([p, n, u], 1).astype(np.float32)
        q = np.round(v * np.array([64, 64, 64, 256, 256, 256, 4096, 4096])).astype(np.int64)
        _, first, inv = np.unique(q, axis=0, return_index=True, return_inverse=True)
        verts = v[first]; idx = inv.reshape(-1).astype(np.uint32)
        f.write(struct.pack('<I6fII', sidx[k[0]], *verts[:, :3].min(0), *verts[:, :3].max(0), len(verts), len(idx)))
        f.write(verts.tobytes()); f.write(idx.tobytes())

# shaders
with open(os.path.join(OUT, 'shaders', 'hd_%s.shader' % NAME), 'w') as f:
    for key, info in mat_info.items():
        s = info['shader']
        if info.get('alpha'):
            f.write('%s\n{\n\tcull none\n\t{\n\t\tmap %s.png\n\t\talphaFunc GE128\n\t\tdepthWrite\n\t\trgbGen lightingDiffuse\n\t}\n}\n\n' % (s, s))
        else:
            f.write('%s\n{\n\t{\n\t\tmap %s.jpg\n\t\trgbGen lightingDiffuse\n\t}\n}\n\n' % (s, s))

sun = [o for o in bpy.data.objects if o.type == 'LIGHT' and o.data.type == 'SUN']
sd = None
if sun:
    d = np.array(sun[0].matrix_world)[:3, 2]; sd = (d / np.linalg.norm(d)).tolist()
json.dump({'bounds': [(mn - centre).tolist(), (mx - centre).tolist()], 'centre': centre.tolist(),
           'units_per_metre': UPM, 'sun_dir': sd, 'triangles': int(ntri)},
          open(os.path.join(OUT, 'maps', NAME + '.hdw.json'), 'w'), indent=1)
print('done')
