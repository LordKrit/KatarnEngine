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
    if mn[2] >= 1.32 and mx[2] <= 1.56 and ax <= 0.36 and ax > 0.11: armor.extend(c.tolist())   # (Kenobi bells reach |x| 0.33)
armor = set(armor)
print('ARMOR verts', len(armor))
KEEP_SOURCE_ARMOUR = True     # Kenobi: BF2's own armour weights (pads hang at the sides of the shoulders)
HID_X = 0.13 if KEEP_SOURCE_ARMOUR else 0.32   # undersuit removal: only behind the chest plate
sp = bo.vertex_groups.get('Spine2') or bo.vertex_groups.new(name='Spine2')
for i in ([] if KEEP_SOURCE_ARMOUR else armor):
    for ge in list(bo.data.vertices[i].groups): bo.vertex_groups[ge.group].add([i], 0.0, 'REPLACE')
    sp.add([i], 1.0, 'REPLACE')
def _sm(a, b, x):
    t = min(1, max(0, (x - a) / (b - a))); return t * t * (3 - 2 * t)
pads = 0
if KEEP_SOURCE_ARMOUR:
    # BF2 hangs the shoulder bells ~85% on the clavicle bone, which barely moves in BF2. Movie Duels
    # animations lift and roll the clavicles a lot (stances, swings), which puffs the bells up and out.
    # Keep 40% of that weight on the clavicle and give the rest to the chest.
    for i in armor:
        x = P[i][0]
        if abs(x) < 0.15: continue
        side = 'Left' if x > 0 else 'Right'
        g = bo.vertex_groups.get(side + 'Shoulder')
        if g is None: continue
        for ge in bo.data.vertices[i].groups:
            if ge.group == g.index and ge.weight > 0:
                moved_w = ge.weight * 0.6
                g.add([i], ge.weight - moved_w, 'REPLACE'); sp.add([i], moved_w, 'ADD'); pads += 1
                break
    print('PAD verts eased off the clavicle', pads)
    # The whole upper armour (chest strips, collar and shoulder bells) and the undersuit under it hold
    # their T-pose shape: rigid on the chest. (Bells on the clavicles split away from the chest strips
    # and opened black gaps whenever the shoulders moved.)
    from mathutils.bvhtree import BVHTree
    mwb = bo.matrix_world
    bm2 = bmesh.new(); bm2.from_mesh(bo.data); bm2.verts.ensure_lookup_table()
    aidx = sorted(armor); amap = {i: k for k, i in enumerate(aidx)}
    af = [[amap[v.index] for v in f.verts] for f in bm2.faces if all(v.index in armor for v in f.verts)]
    abvh = BVHTree.FromPolygons([mwb @ bm2.verts[i].co for i in aidx], af)
    n3 = mwb.to_3x3(); under = set()
    for v in bm2.verts:
        if v.index in armor: continue
        p = mwb @ v.co
        if p.z < 1.30 or abs(p.x) > 0.32: continue
        gs = bo.data.vertices[v.index].groups
        if gs and re.search(r'Arm|Deltoid', bo.vertex_groups[max(gs, key=lambda g: g.weight).group].name): continue   # sleeves follow the arm
        nn = (n3 @ v.normal).normalized()
        if abvh.ray_cast(p - nn * 0.002, nn, 0.08)[0] is not None: under.add(v.index)
    bm2.free()
    rigid = set(armor) | under
    # Rigid bells keep the A-pose angle; with the arms down in Movie Duels they stick out through the
    # cape. Tilt their outer ends down (about the front-back axis at the shoulder) so they hang over
    # the shoulder the way they do on the real suit.
    import math
    from mathutils import Matrix
    BELL_DEG = 0.0       # (tilting alone can't follow the arm: bells follow it half-way instead, below)
    mwi = bo.matrix_world.inverted(); tilted = 0
    for i in rigid:
        p = bo.matrix_world @ bo.data.vertices[i].co
        ax = abs(p.x)
        t = _sm(0.12, 0.20, ax)
        if t <= 0 or p.z < 1.30: continue
        s = 1 if p.x > 0 else -1
        piv = Vector((s * 0.12, p.y, 1.47))
        rot = Matrix.Rotation(math.radians(BELL_DEG * t) * s, 3, 'Y')   # outer end goes down
        bo.data.vertices[i].co = mwi @ (piv + rot @ (p - piv)); tilted += 1
    print('BELL verts tilted down', tilted)
    BELL_FOLLOW = 0.7     # share of the upper arm's motion the outer end of each bell takes (pulled down onto the arm)
    for i in rigid:
        for ge in list(bo.data.vertices[i].groups): bo.vertex_groups[ge.group].add([i], 0.0, 'REPLACE')
        x = P[i][0]; t = _sm(0.12, 0.21, abs(x)) * BELL_FOLLOW
        if t > 0:
            ar = bo.vertex_groups.get(('Left' if x > 0 else 'Right') + 'Arm')
            ar.add([i], t, 'REPLACE'); sp.add([i], 1.0 - t, 'REPLACE')
        else:
            sp.add([i], 1.0, 'REPLACE')
    print('RIGID upper armour verts', len(armor), 'undersuit under it', len(under))
for i in ([] if KEEP_SOURCE_ARMOUR else armor):
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
for side in (() if KEEP_SOURCE_ARMOUR else ('Left', 'Right')):
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
if not KEEP_SOURCE_ARMOUR:
    exec(open('/home/claude/vbf2/vfix.py').read())  # hidden undersuit out, cape off the armour
# (with BF2's own weights the plates move on their own: the undersuit behind them must stay,
#  and the cape keeps its modelled fit)
exec(open('/home/claude/vbf2/vfix2.py').read())     # cape inner lining (if any) out

cm = bpy.data.objects['mat_cape']
# the cape is modelled draped over T-pose arms, so its sides stand out like wings once the arms come down.
# Draw the sides in below the shoulders so they hang just outside the arms (shape keys/none, so edit coords).
CAPE_TUCK = globals().get('CAPE_TUCK', 0.24)
_mw = cm.matrix_world; _mi = _mw.inverted()
for v in cm.data.vertices:
    p = _mw @ v.co
    k = 1 - CAPE_TUCK * (1 - _sm(1.18, 1.36, p.z))
    p.x *= k; v.co = _mi @ p
# BF2 look: cape lies flat on sloped shoulders (no hump) and the sides fall behind the arms
CAPE_SH_DROP = globals().get('CAPE_SH_DROP', 0.0); CAPE_SH_IN = globals().get('CAPE_SH_IN', 0.0)
CAPE_SWEEP = globals().get('CAPE_SWEEP', 0.05)
_bo = [o for o in bpy.data.objects if o.type == 'MESH' and o.name.startswith('mat_body')]
_cy = sum((_mw @ v.co).y for v in cm.data.vertices) / len(cm.data.vertices)
_by = sum((o.matrix_world @ v.co).y for o in _bo for v in o.data.vertices) / max(1, sum(len(o.data.vertices) for o in _bo))
_back = 1.0 if _cy > _by else -1.0
for v in cm.data.vertices:
    p = _mw @ v.co; ax = abs(p.x)
    sh = _sm(1.26, 1.40, p.z) * (1 - _sm(1.50, 1.62, p.z)) * _sm(0.07, 0.20, ax)
    p.z -= CAPE_SH_DROP * sh; p.x *= 1 - CAPE_SH_IN * sh
    p.y += _back * CAPE_SWEEP * _sm(0.09, 0.20, ax) * (1 - _sm(1.20, 1.38, p.z))
    v.co = _mi @ p
print('CAPE back sign', _back)
# keep the cape on top of the armour: anything under / too close to the body is lifted out along the surface normal
from mathutils.bvhtree import BVHTree
import bmesh
_bm = bmesh.new()
for o in _bo:
    t = o.data.copy(); t.transform(o.matrix_world); _bm.from_mesh(t); bpy.data.meshes.remove(t)
_tree = BVHTree.FromBMesh(_bm); CAPE_GAP = globals().get('CAPE_GAP', 0.012); n_l = 0
for v in cm.data.vertices:
    p = _mw @ v.co
    if p.z < 1.15: continue
    hit = _tree.find_nearest(p)
    if hit[0] is None: continue
    d = (p - hit[0]).dot(hit[1])
    if d < CAPE_GAP:
        v.co = _mi @ (p + hit[1] * (CAPE_GAP - d)); n_l += 1
print('CAPE lifted off armour', n_l)
sp = cm.vertex_groups.get('Spine2') or cm.vertex_groups.new(name='Spine2')
drop = {g.index for g in cm.vertex_groups if re.search(r'(Arm|Elbow|Deltoid|Shoulder|Knee|Leg|Foot|Toe|^Hips$|^Spine$|^Spine1$)', g.name)}   # cape top rides the chest like the armour
for v in cm.data.vertices:
    gone = 0
    for ge in list(v.groups):
        if ge.group in drop and ge.weight > 0: gone += ge.weight; cm.vertex_groups[ge.group].add([v.index], 0.0, 'REPLACE')
    if gone > 0: sp.add([v.index], gone, 'ADD')
if KEEP_SOURCE_ARMOUR:
    # the cape lies over the bells: over the shoulders it follows them the same way (chest + part of the
    # upper arm), so it stays on top of them instead of the bells pushing through
    mwc = cm.matrix_world; n_c = 0
    for v in cm.data.vertices:
        p = mwc @ v.co
        if p.z < 1.30: continue
        # fade in with height so the sides below the shoulder hang from the chest, not the arm (no wings)
        t = _sm(0.12, 0.21, abs(p.x)) * _sm(1.30, 1.46, p.z) * BELL_FOLLOW
        if t <= 0: continue
        ar = cm.vertex_groups.get(('Left' if p.x > 0 else 'Right') + 'Arm') or cm.vertex_groups.new(name=('Left' if p.x > 0 else 'Right') + 'Arm')
        tot = sum(ge.weight for ge in v.groups) or 1.0
        for ge in list(v.groups): cm.vertex_groups[ge.group].add([v.index], ge.weight / tot * (1 - t), 'REPLACE')
        ar.add([v.index], t, 'REPLACE'); n_c += 1
    print('CAPE verts riding the bells', n_c)
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
