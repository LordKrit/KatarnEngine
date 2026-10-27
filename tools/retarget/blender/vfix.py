# ---- remove body geometry hidden under the chest armour; lift the cape off the armour ----
from mathutils.bvhtree import BVHTree
import bmesh
mwb=bo.matrix_world
bm=bmesh.new(); bm.from_mesh(bo.data); bm.verts.ensure_lookup_table(); bm.faces.ensure_lookup_table()
av=[mwb@bm.verts[i].co for i in sorted(armor)]
amap={i:k for k,i in enumerate(sorted(armor))}
af=[[amap[v.index] for v in f.verts] for f in bm.faces if all(v.index in armor for v in f.verts)]
abvh=BVHTree.FromPolygons(av,af)
n3=mwb.to_3x3()
hidden=set()
for v in bm.verts:
    if v.index in armor: continue
    p=mwb@v.co
    if p.z<1.30 or abs(p.x)>globals().get('HID_X',0.32): continue
    gs=bo.data.vertices[v.index].groups
    if gs:
        top=max(gs,key=lambda g:g.weight)
        if 'Arm' in bo.vertex_groups[top.group].name: continue
    nn=(n3@v.normal).normalized()
    hit=abvh.ray_cast(p-nn*0.002, nn, 0.08)
    if hit[0] is not None: hidden.add(v.index)
faces=[f for f in bm.faces if all(v.index in hidden for v in f.verts)]
bmesh.ops.delete(bm, geom=faces, context='FACES_ONLY')
loose=[v for v in bm.verts if not v.link_faces]
bmesh.ops.delete(bm, geom=loose, context='VERTS')
bm.to_mesh(bo.data); bm.free()
print('HIDDEN body faces removed',len(faces))
# armour index set is stale now: rebuild armour BVH from the original positions only for the cape step
cmo=[x for x in bpy.data.objects if 'cape' in x.name and x.type=='MESH' and 'inner' not in x.name][0]
mwc=cmo.matrix_world; mic=mwc.inverted(); lifted=0
for v in cmo.data.vertices:
    p=mwc@v.co
    if p.z<1.30: continue
    loc,nrm,idx,dist=abvh.find_nearest(p)
    if loc is None or dist>0.04: continue
    out=(p-loc)
    side=out.dot(nrm)
    need=0.012-side            # keep the cape 1.2 cm outside the armour
    if need>0:
        v.co=mic@(p+nrm.normalized()*need); lifted+=1
print('CAPE verts lifted off armour',lifted)
