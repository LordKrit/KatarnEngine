import bpy, re, os
bpy.ops.wm.read_factory_settings(use_empty=True)
CM='/home/claude/vcl/in/SWBF2_DarthVader/Assets/Meshes/darthvader_01_mesh.fbx'
bpy.ops.import_scene.fbx(filepath=CM)
classic={o.name:o for o in bpy.data.objects}
CR=[o for o in classic.values() if o.type=='ARMATURE'][0]
bpy.ops.import_scene.fbx(filepath='/home/claude/vbf2/in/vader.fbx')
new=[o for o in bpy.data.objects if o.name not in classic]
R=[o for o in new if o.type=='ARMATURE'][0]
for o in list(new):
    if o.type in ('CAMERA','LIGHT') or o.name.startswith('Cube'): bpy.data.objects.remove(o,do_unlink=True)
new=[o for o in bpy.data.objects if o.name not in classic]
src={'mat_body':'mat_body','mat_glove':'mat_glove','mat_innercape':'mat_innercape','helmet':'mat_helmet'}
def find_classic(p): return [o for n,o in classic.items() if n.startswith(p) and o.type=='MESH'][0]
for o in new:
    if o.type!='MESH' or 'cape_mesh' in o.name: continue
    key=[k for k in src if k in o.name][0]; s=find_classic(src[key])
    for g in list(o.vertex_groups): o.vertex_groups.remove(g)
    for g in s.vertex_groups: o.vertex_groups.new(name=g.name)
    same = len(s.data.vertices)==len(o.data.vertices)
    if same:
        import numpy as np
        a=np.array([s.matrix_world@v.co for v in s.data.vertices]); b=np.array([o.matrix_world@v.co for v in o.data.vertices])
        same = np.abs(a-b).max()<0.002
    if same:
        for v in s.data.vertices:
            for ge in v.groups: o.vertex_groups[ge.group].add([v.index],ge.weight,'REPLACE')
        how='index'
    else:
        bpy.context.view_layer.objects.active=o
        m=o.modifiers.new('dt','DATA_TRANSFER'); m.object=s; m.use_object_transform=True
        m.use_vert_data=True; m.data_types_verts={'VGROUP_WEIGHTS'}; m.vert_mapping='POLYINTERP_NEAREST'
        m.layers_vgroup_select_src='ALL'; m.layers_vgroup_select_dst='NAME'
        bpy.ops.object.datalayer_transfer if False else None
        while o.modifiers[0]!=m: bpy.ops.object.modifier_move_up(modifier='dt')
        bpy.ops.object.modifier_apply(modifier='dt'); how='transfer'
    nz=sum(1 for v in o.data.vertices if not any(g.weight>0.01 for g in v.groups))
    print('WEIGHTS',o.name,'<-',s.name,how,'unweighted',nz,'/',len(o.data.vertices))
    mw=o.matrix_world.copy(); o.parent=R; o.matrix_world=mw
    if not any(md.type=='ARMATURE' for md in o.modifiers): o.modifiers.new('arm','ARMATURE')
    for md in o.modifiers:
        if md.type=='ARMATURE': md.object=R
for o in list(classic.values()): bpy.data.objects.remove(o,do_unlink=True)

# ---- chest armour: rigid to Spine2; upper sleeves tucked under it so raised arms don't poke through ----
import bmesh, numpy as np
from mathutils import Vector
bo=[x for x in bpy.data.objects if 'mat_body' in x.name][0]
img=bpy.data.images.load('/home/claude/vbf2/in/tex/Textures/Body_Metallic.png'); W,H=img.size
px=np.array(img.pixels[:]).reshape(H,W,4)[...,0]
uvd=bo.data.uv_layers.active.data
met=np.zeros(len(bo.data.vertices))
for l in bo.data.loops:
    u,v=uvd[l.index].uv; met[l.vertex_index]=max(met[l.vertex_index],px[int(v%1*(H-1)),int(u%1*(W-1))])
bm=bmesh.new(); bm.from_mesh(bo.data); bm.verts.ensure_lookup_table()
seen=set(); isl=[]
for v in bm.verts:
    if v.index in seen: continue
    st=[v]; cur=[]; seen.add(v.index)
    while st:
        a=st.pop(); cur.append(a.index)
        for e in a.link_edges:
            b=e.other_vert(a)
            if b.index not in seen: seen.add(b.index); st.append(b)
    isl.append(np.array(cur))
bm.free()
P=np.array([bo.matrix_world@v.co for v in bo.data.vertices])
armor=[]
for c in isl:
    mn=P[c].min(0); mx=P[c].max(0); ax=np.abs(P[c][:,0]).max()
    if mn[2]>=1.32 and mx[2]<=1.56 and ax<=0.31 and ax>0.11: armor.extend(c.tolist())
armor=set(armor)
print('ARMOR verts',len(armor))
sp=bo.vertex_groups.get('Spine2') or bo.vertex_groups.new(name='Spine2')
for i in armor:
    for ge in list(bo.data.vertices[i].groups): bo.vertex_groups[ge.group].add([i],0.0,'REPLACE')
    sp.add([i],1.0,'REPLACE')
# outer ends of the armour become shoulder pads: they follow the shoulder and upper arm
def _sm(a,b,x):
    t=min(1,max(0,(x-a)/(b-a))); return t*t*(3-2*t)
pads=0
for i in armor:
    x=P[i][0]; t=_sm(0.14,0.22,abs(x))
    if t<=0: continue
    side='Left' if x>0 else 'Right'
    sh=bo.vertex_groups.get(side+'Shoulder') or bo.vertex_groups.new(name=side+'Shoulder')
    ar=bo.vertex_groups.get(side+'Arm') or bo.vertex_groups.new(name=side+'Arm')
    sp.add([i],1.0-t,'REPLACE'); sh.add([i],0.35*t,'REPLACE'); ar.add([i],0.65*t,'REPLACE'); pads+=1
    nrm=bo.matrix_world.to_3x3()@bo.data.vertices[i].normal
    bo.data.vertices[i].co=bo.matrix_world.inverted()@(Vector(P[i])+nrm.normalized()*0.006*t)
print('PAD verts',pads)
# sleeves: tucked a little under the pads near the shoulder
mwR=R.matrix_world; mi=bo.matrix_world.inverted(); moved=0
for side in ('Left','Right'):
    a=mwR@R.data.bones[side+'Arm'].head_local; b=mwR@R.data.bones[side+'ForeArm'].head_local
    d=(b-a); d.normalize()
    for i,v in enumerate(bo.data.vertices):
        if i in armor: continue
        p=Vector(P[i]); t=(p-a).dot(d)
        if t<-0.06 or t>0.12 or (p.x>0)!=(a.x>0) or abs(p.x)<0.12: continue
        axis=a+d*max(t,0); r=p-axis
        k=1.0-min(1,max(0,(t-0.02)/0.10)); up=max(0.0,r.normalized().z)
        v.co=mi@(axis+r*(1.0-0.18*k*(0.3+0.7*up))); moved+=1
print('SLEEVE tucked',moved)
exec(open('/home/claude/vbf2/vfix.py').read())


cm=[o for o in bpy.data.objects if 'mat_cape' in o.name][0]
sp=cm.vertex_groups.get('Spine2') or cm.vertex_groups.new(name='Spine2')
drop={g.index for g in cm.vertex_groups if re.search(r'(Arm|Elbow|Deltoid|Knee|Leg|Foot|Toe|^Hips$|^Spine$|^Spine1$)',g.name)}
for v in cm.data.vertices:
    gone=0
    for ge in list(v.groups):
        if ge.group in drop and ge.weight>0: gone+=ge.weight; cm.vertex_groups[ge.group].add([v.index],0.0,'REPLACE')
    if gone>0: sp.add([v.index],gone,'ADD')
cm.name='cape'
T='/home/claude/vbf2/in/tex/Textures/'
for m in bpy.data.materials:
    key={'Body':'Body','Gloves':'Gloves','Helmet':'Helmet','Cape':'Cape','Innercape':'Innercape'}.get(m.name)
    if not key: continue
    m.use_nodes=True; nt=m.node_tree; nt.nodes.clear()
    out=nt.nodes.new('ShaderNodeOutputMaterial'); b=nt.nodes.new('ShaderNodeBsdfPrincipled'); t=nt.nodes.new('ShaderNodeTexImage'); t.image=bpy.data.images.load(T+key+'_Base_Color.png')
    nt.links.new(t.outputs['Color'],b.inputs['Base Color']); nt.links.new(b.outputs[0],out.inputs[0])
R.data.pose_position='REST'
def export(p):
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.export_scene.gltf(filepath=p,export_format='GLB',use_selection=True,export_animations=False,export_skins=True,export_apply=False,export_yup=True)
export('/home/claude/vbf2/outr/vader.glb')
meshes=[o for o in bpy.data.objects if o.type=='MESH']
prev=1.0
for lod,r in ((1,0.5),(2,0.25),(3,0.1)):
    for o in meshes:
        bpy.context.view_layer.objects.active=o
        d=o.modifiers.new('dec','DECIMATE'); d.ratio=r/prev
        while o.modifiers[0]!=d: bpy.ops.object.modifier_move_up(modifier='dec')
        bpy.ops.object.modifier_apply(modifier='dec')
    prev=r; export('/home/claude/vbf2/outr/vader_lod%d.glb'%lod)
print('DONE')
