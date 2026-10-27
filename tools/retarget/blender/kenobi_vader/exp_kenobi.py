# Kenobi (BF2) Darth Vader -> glb + LODs for jka_retarget, built like vader_bf2_rots
# (rigid chest armour whose outer ends ride the shoulders as pads, hidden undersuit removed,
#  cape kept off the armour, cape on the upper torso only).
# blender -b --python exp_kenobi.py -- <fbx> <outdir>
import bpy, re, os, sys
import numpy as np, bmesh
from mathutils import Vector
argv = sys.argv[sys.argv.index('--') + 1:]
FBX, OUT = argv[0], argv[1]
os.makedirs(OUT, exist_ok=True)
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=FBX)
for o in list(bpy.data.objects):
    if o.type in ('CAMERA', 'LIGHT') or o.name.startswith('Cube'): bpy.data.objects.remove(o, do_unlink=True)
R = [o for o in bpy.data.objects if o.type == 'ARMATURE'][0]
# names the shared fix-up scripts expect; material names become the shader names
REN = {'Body': ('mat_body', 'body'), 'Cape': ('mat_cape', 'cape'), 'Duster': ('mat_innercape', 'innercape'),
       'Gloves': ('mat_glove', 'gloves'), 'Helmet': ('helmet', 'helmet'), 'Head': ('mat_head', 'head'), 'Eyes': ('mat_eyes', 'eyes')}
for o in list(bpy.data.objects):
    if o.type != 'MESH': continue
    on, mn = REN[o.name]
    o.name = on; o.data.name = on if on != 'mat_innercape' else 'duster'   # glTF mesh name: 'cape' = cloth
    mat = bpy.data.materials.new(mn); mat.use_nodes = True; nt = mat.node_tree; nt.nodes.clear()
    out = nt.nodes.new('ShaderNodeOutputMaterial'); b = nt.nodes.new('ShaderNodeBsdfPrincipled'); t = nt.nodes.new('ShaderNodeTexImage')
    img = bpy.data.images.new(mn, 4, 4); img.name = mn; t.image = img
    nt.links.new(t.outputs['Color'], b.inputs['Base Color']); nt.links.new(b.outputs[0], out.inputs[0])
    o.data.materials.clear(); o.data.materials.append(mat)
    for md in o.modifiers:
        if md.type == 'ARMATURE': md.object = R

# ---- chest armour: rigid to Spine2, outer ends as shoulder pads ----
bo = bpy.data.objects['mat_body']
bm = bmesh.new(); bm.from_mesh(bo.data); bm.verts.ensure_lookup_table()
seen = set(); isl = []
for v in bm.verts:
    if v.index in seen: continue
    st = [v]; cur = []; seen.add(v.index)
    while st:
        a = st.pop(); cur.append(a.index)
        for e in a.link_edges:
            b = e.other_vert(a)
            if b.index not in seen: seen.add(b.index); st.append(b)
    isl.append(np.array(cur))
bm.free()
P = np.array([bo.matrix_world @ v.co for v in bo.data.vertices])
armor = []
for c in isl:
    mn = P[c].min(0); mx = P[c].max(0); ax = np.abs(P[c][:, 0]).max()
    if mn[2] >= 1.32 and mx[2] <= 1.56 and ax <= 0.31 and ax > 0.11: armor.extend(c.tolist())
armor = set(armor)
print('ARMOR verts', len(armor))
sp = bo.vertex_groups.get('Spine2') or bo.vertex_groups.new(name='Spine2')
for i in armor:
    for ge in list(bo.data.vertices[i].groups): bo.vertex_groups[ge.group].add([i], 0.0, 'REPLACE')
    sp.add([i], 1.0, 'REPLACE')
def _sm(a, b, x):
    t = min(1, max(0, (x - a) / (b - a))); return t * t * (3 - 2 * t)
pads = 0
for i in armor:
    x = P[i][0]; t = _sm(0.14, 0.22, abs(x))
    if t <= 0: continue
    side = 'Left' if x > 0 else 'Right'
    sh = bo.vertex_groups.get(side + 'Shoulder') or bo.vertex_groups.new(name=side + 'Shoulder')
    ar = bo.vertex_groups.get(side + 'Arm') or bo.vertex_groups.new(name=side + 'Arm')
    sp.add([i], 1.0 - t, 'REPLACE'); sh.add([i], 0.35 * t, 'REPLACE'); ar.add([i], 0.65 * t, 'REPLACE'); pads += 1
    nrm = bo.matrix_world.to_3x3() @ bo.data.vertices[i].normal
    bo.data.vertices[i].co = bo.matrix_world.inverted() @ (Vector(P[i]) + nrm.normalized() * 0.006 * t)
print('PAD verts', pads)
mwR = R.matrix_world; mi = bo.matrix_world.inverted(); moved = 0
for side in ('Left', 'Right'):
    a = mwR @ R.data.bones[side + 'Arm'].head_local; b = mwR @ R.data.bones[side + 'ForeArm'].head_local
    d = (b - a); d.normalize()
    for i, v in enumerate(bo.data.vertices):
        if i in armor: continue
        p = Vector(P[i]); t = (p - a).dot(d)
        if t < -0.06 or t > 0.12 or (p.x > 0) != (a.x > 0) or abs(p.x) < 0.12: continue
        axis = a + d * max(t, 0); r = p - axis
        k = 1.0 - min(1, max(0, (t - 0.02) / 0.10)); up = max(0.0, r.normalized().z)
        v.co = mi @ (axis + r * (1.0 - 0.18 * k * (0.3 + 0.7 * up))); moved += 1
print('SLEEVE tucked', moved)
exec(open('/home/claude/vbf2/vfix.py').read())      # hidden undersuit out, cape off the armour
exec(open('/home/claude/vbf2/vfix2.py').read())     # cape inner lining (if any) out

cm = bpy.data.objects['mat_cape']
sp = cm.vertex_groups.get('Spine2') or cm.vertex_groups.new(name='Spine2')
drop = {g.index for g in cm.vertex_groups if re.search(r'(Arm|Elbow|Deltoid|Knee|Leg|Foot|Toe|^Hips$|^Spine$|^Spine1$)', g.name)}
for v in cm.data.vertices:
    gone = 0
    for ge in list(v.groups):
        if ge.group in drop and ge.weight > 0: gone += ge.weight; cm.vertex_groups[ge.group].add([v.index], 0.0, 'REPLACE')
    if gone > 0: sp.add([v.index], gone, 'ADD')
cm.name = 'cape'
R.data.pose_position = 'REST'
def export(p):
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.export_scene.gltf(filepath=p, export_format='GLB', use_selection=True, export_animations=False, export_skins=True, export_apply=False, export_yup=True)
export(os.path.join(OUT, 'vader.glb'))
meshes = [o for o in bpy.data.objects if o.type == 'MESH']
prev = 1.0
for lod, r in ((1, 0.5), (2, 0.25), (3, 0.1)):
    for o in meshes:
        bpy.context.view_layer.objects.active = o
        dm = o.modifiers.new('dec', 'DECIMATE'); dm.ratio = r / prev
        while o.modifiers[0] != dm: bpy.ops.object.modifier_move_up(modifier='dec')
        bpy.ops.object.modifier_apply(modifier='dec')
    prev = r; export(os.path.join(OUT, 'vader_lod%d.glb' % lod))
print('DONE')
