import bpy, re
bpy.ops.wm.read_factory_settings(use_empty=True)
M='/home/claude/vcl/in/SWBF2_DarthVader/Assets/Meshes/'
bpy.ops.import_scene.fbx(filepath=M+'darthvader_01_mesh.fbx')
bpy.ops.import_scene.fbx(filepath=M+'darthvader_01_cape_mesh.fbx')
R=bpy.data.objects['Reference']; R2=bpy.data.objects['Reference.001']
for o in list(bpy.data.objects):
    if o.type in ('CAMERA','LIGHT') or o.name=='Cube': bpy.data.objects.remove(o,do_unlink=True)
print('R world', R.matrix_world, 'R2', R2.matrix_world)
names={b.name for b in R.data.bones}
cm=bpy.data.objects['mat_cape']
print('cape groups missing', [g.name for g in cm.vertex_groups if g.name not in names])
mw=cm.matrix_world.copy(); cm.parent=R; cm.matrix_world=mw
for m in cm.modifiers:
    if m.type=='ARMATURE': m.object=R
bpy.data.objects.remove(R2,do_unlink=True)
MAP={'mat_body':'body','mat_glove':'gloves','mat_helmet':'helmet','mat_innercape':'innercape','mat_cape':'cape',
     'mat_beltEmGreen':'emgreen','mat_beltEmRed':'emred','mat_emissiveChestGreen':'emgreen','mat_emissiveChest':'emred'}
for o in bpy.data.objects:
    if o.type!='MESH': continue
    key=[k for k in MAP if o.name.startswith(k)][0]; mn=MAP[key]
    mat=bpy.data.materials.get(mn)
    if not mat:
        mat=bpy.data.materials.new(mn); mat.use_nodes=True; nt=mat.node_tree; nt.nodes.clear()
        out=nt.nodes.new('ShaderNodeOutputMaterial'); b=nt.nodes.new('ShaderNodeBsdfPrincipled'); t=nt.nodes.new('ShaderNodeTexImage')
        T='/home/claude/vcl/in/SWBF2_DarthVader/Assets/Textures/'
        f={'body':'body/T_DarthVader_01_Body_CS.png','gloves':'gloves/T_DarthVader_01_Glove_CS.png','helmet':'helmet/T_DarthVader_01_Helmet_CS.png','innercape':'skirt/T_DarthVader_01_InnerCape_CS.png','cape':'cape/T_DarthVader_01_Cape_C.png'}.get(mn,'body/T_DarthVader_01_Body_CS.png')
        img=bpy.data.images.load(T+f); img.name=mn; t.image=img
        nt.links.new(t.outputs['Color'],b.inputs['Base Color']); nt.links.new(b.outputs[0],out.inputs[0])
    o.data.materials.clear(); o.data.materials.append(mat)
    for md in o.modifiers:
        if md.type=='ARMATURE': md.object=R
    if not any(md.type=='ARMATURE' for md in o.modifiers): print('NOARM',o.name)
    if mn=='cape': o.name='cape'
# cape: arm/leg + low spine weights -> Spine2 (as dooku)
sp=cm.vertex_groups.get('Spine2') or cm.vertex_groups.new(name='Spine2')
drop={g.index for g in cm.vertex_groups if re.search(r'(Arm|Elbow|Deltoid|Knee|Leg|Foot|Toe|^Hips$|^Spine$|^Spine1$)',g.name)}
for v in cm.data.vertices:
    gone=0
    for ge in list(v.groups):
        if ge.group in drop and ge.weight>0: gone+=ge.weight; cm.vertex_groups[ge.group].add([v.index],0.0,'REPLACE')
    if gone>0: sp.add([v.index],gone,'ADD')
import numpy as np
for o in bpy.data.objects:
    if o.type=='MESH':
        P=np.array([o.matrix_world@v.co for v in o.data.vertices]); print('BOX',o.name,P.min(0).round(2),P.max(0).round(2))
R.data.pose_position='REST'
def export(p):
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.export_scene.gltf(filepath=p,export_format='GLB',use_selection=True,export_animations=False,export_skins=True,export_apply=False,export_yup=True)
export('/home/claude/vcl/out/vader.glb')
meshes=[o for o in bpy.data.objects if o.type=='MESH' and len(o.data.vertices)>300]
prev=1.0
for lod,r in ((1,0.5),(2,0.25),(3,0.1)):
    for o in meshes:
        bpy.context.view_layer.objects.active=o
        d=o.modifiers.new('dec','DECIMATE'); d.ratio=r/prev
        while o.modifiers[0]!=d: bpy.ops.object.modifier_move_up(modifier='dec')
        bpy.ops.object.modifier_apply(modifier='dec')
    prev=r; export('/home/claude/vcl/out/vader_lod%d.glb'%lod)
print('DONE')
