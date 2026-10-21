import bpy
bpy.ops.wm.open_mainfile(filepath='/mnt/user-data/uploads/dooku4.blend')
R=bpy.data.objects['Reference.001']
# simple materials: base colour image (+ alpha for cards) so glTF carries the right texture
def base_image(m):
    for n in m.node_tree.nodes:
        if n.type=='TEX_IMAGE' and n.image:
            for o in n.outputs:
                for l in o.links:
                    if l.to_socket.name in ('Base Color','Base Colour','DA Colour'):
                        return n.image
for m in bpy.data.materials:
    if not m.node_tree: continue
    img=base_image(m)
    if not img: continue
    nt=m.node_tree; nt.nodes.clear()
    out=nt.nodes.new('ShaderNodeOutputMaterial'); b=nt.nodes.new('ShaderNodeBsdfPrincipled'); t=nt.nodes.new('ShaderNodeTexImage'); t.image=img
    nt.links.new(t.outputs['Color'], b.inputs['Base Color']); nt.links.new(b.outputs[0], out.inputs[0])
    print('MAT', m.name, '<-', img.name)
cm=bpy.data.objects['mat_cape.003']
if not cm.data.materials or cm.data.materials[0] is None:
    cm.data.materials.clear(); cm.data.materials.append(bpy.data.materials['cape'])
import bmesh
exec(open('/tmp/d4/cape_split.py').read().split("if __name__")[0])
bm=bmesh.new(); bm.from_mesh(cm.data); isl=split_cape(cm)
outer=set(isl[0]); inner=set(isl[1])
# trim pieces -> own object (rigid), inner lining deleted, outer shell = cloth
trim=cm.copy(); trim.data=cm.data.copy(); trim.name='mat_trim'; trim.data.name='mat_trim'
bpy.context.collection.objects.link(trim)
def keep_only(obj, keep):
    b=bmesh.new(); b.from_mesh(obj.data); b.verts.ensure_lookup_table()
    bmesh.ops.delete(b, geom=[v for v in b.verts if v.index not in keep], context='VERTS')
    b.to_mesh(obj.data); b.free()
allv=set(range(len(cm.data.vertices)))
keep_only(trim, allv-outer-inner)
keep_only(cm, outer)
print('CAPE verts', len(cm.data.vertices), 'TRIM verts', len(trim.data.vertices))
# the cape hangs from the shoulders: drop its arm and leg weights, or the
# animated cape (what the cloth follows) is dragged round the arms and legs
import re as _re
drop=[g for g in cm.vertex_groups if _re.search(r'(Arm|Elbow|Deltoid|Knee|Leg)', g.name)]
dropi={g.index for g in drop}
sp=cm.vertex_groups['Spine2']
# (the part over the shoulders keeps them: it rides on the shoulders when the arms move)
mw=cm.matrix_world
for v in cm.data.vertices:
    z=(mw @ v.co).z
    k=1.0   # v7: the cape drops its arm and leg weights completely          # 0 over the shoulders and upper back, 1 from mid-back down
    if k<=0: continue
    gone=0.0
    for ge in list(v.groups):
        if ge.group in dropi:
            gone+=ge.weight*k
            cm.vertex_groups[ge.group].add([v.index], ge.weight*(1-k), 'REPLACE')
    if gone>0: sp.add([v.index], gone, 'ADD')
print('CAPE dropped groups', len(drop))
# the cape hangs from the shoulders: its reference shape moves with the upper torso only. Weights on the
# hips and lower spine follow the LEGS animation in Ghoul2 (the torso animation starts at lower_lumbar),
# so a torso twist while the legs run would shear the cape and push the chest through it
lowsp={g.index for g in cm.vertex_groups if g.name in ('Hips','Spine','Spine1')}
moved=0
for v in cm.data.vertices:
    gone=0.0
    for ge in list(v.groups):
        if ge.group in lowsp and ge.weight>0:
            gone+=ge.weight; cm.vertex_groups[ge.group].add([v.index],0.0,'REPLACE')
    if gone>0: sp.add([v.index],gone,'ADD'); moved+=1
print('CAPE moved to Spine2 from hips/lower spine', moved)
# a little more room over the shoulders and upper back: the cape there is lifted by 1 cm along its own
# vertex normals (the same amount everywhere, so the surface stays as smooth as it was), fading out
# between 1.36 and 1.28 m. Vertices stitched to the clasp trim stay put.
from mathutils.kdtree import KDTree as _KDT
kt2=_KDT(len(trim.data.vertices))
for i_,tv in enumerate(trim.data.vertices): kt2.insert(trim.matrix_world @ tv.co, i_)
kt2.balance()
mwi2=cm.matrix_world.inverted(); n3=cm.matrix_world.to_3x3()
lifted=0
for v in cm.data.vertices:
    w=cm.matrix_world @ v.co
    k=min(1.0,max(0.0,(w.z-1.28)/0.08))
    if k<=0: continue
    dt=kt2.find(w)[2]
    k*=min(1.0,max(0.0,(dt-0.005)/0.03))      # ease in away from the trim seam
    if k<=0: continue
    nrm=(n3 @ v.normal).normalized()
    if nrm.y < 0 and w.y < 0: pass
    v.co=mwi2 @ (w+nrm*0.010*k); lifted+=1
print('CAPE shoulders lifted', lifted)

# eyeballs: one bone each (eyeball_l / eyeball_r) at the eye's centre, child of the head, so the
# game can turn the eyes towards what he looks at; eyeball vertices go fully to them
eo=[o for o in bpy.data.objects if o.type=='MESH' and 'eyes_mesh' in o.name][0]
import numpy as _np
ew=eo.matrix_world
EP=_np.array([ew @ v.co for v in eo.data.vertices])
sideL=EP[:,0]>0                                  # he faces -Y: his left is +X
cen={'eyeball_l':EP[sideL].mean(0),'eyeball_r':EP[~sideL].mean(0)}
print('EYES', {k:[round(x,4) for x in v] for k,v in cen.items()}, 'verts', int(sideL.sum()), int((~sideL).sum()))
bpy.context.view_layer.objects.active=R
bpy.ops.object.mode_set(mode='EDIT')
eb=R.data.edit_bones; ri=R.matrix_world.inverted()
from mathutils import Vector
for nm,c in cen.items():
    b=eb.new(nm); h=ri @ Vector(c); b.head=h; b.tail=h+ri.to_3x3() @ Vector((0,-0.02,0)); b.parent=eb['Head']
bpy.ops.object.mode_set(mode='OBJECT')
for g in list(eo.vertex_groups): eo.vertex_groups.remove(g)
gl=eo.vertex_groups.new(name='eyeball_l'); gr=eo.vertex_groups.new(name='eyeball_r')
gl.add([i for i in range(len(EP)) if sideL[i]],1.0,'REPLACE'); gr.add([i for i in range(len(EP)) if not sideL[i]],1.0,'REPLACE')
meshes=[o for o in bpy.data.objects if o.type=='MESH']
for o in meshes:
    for m in list(o.modifiers):
        if m.type=='SUBSURF': o.modifiers.remove(m)
        elif m.type=='ARMATURE': m.object=R
    mw=o.matrix_world.copy(); o.parent=R; o.matrix_world=mw
for o in list(bpy.data.objects):
    if o not in meshes and o!=R: bpy.data.objects.remove(o, do_unlink=True)
R.data.pose_position='REST'
def export(path):
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.export_scene.gltf(filepath=path, export_format='GLB', use_selection=True, export_animations=False, export_skins=True, export_apply=False, export_yup=True)
export('/tmp/d4/i/dooku4.glb')
# lower detail levels: each one about half (then less) of the previous
prev=1.0
for lod,ratio in ((1,0.5),(2,0.25),(3,0.1)):
    for o in meshes:
        bpy.context.view_layer.objects.active=o
        dm=o.modifiers.new('dec','DECIMATE'); dm.ratio=ratio/prev
        while o.modifiers[0]!=dm: bpy.ops.object.modifier_move_up(modifier='dec')
        bpy.ops.object.modifier_apply(modifier='dec')
    prev=ratio
    print('LOD',lod, sum(len(o.data.vertices) for o in meshes))
    export('/tmp/d4/i/dooku4_lod%d.glb'%lod)
