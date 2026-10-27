# ---- cape: keep the outer shell only (the inner lining is a second copy that pokes through) ----
import bmesh, numpy as np
from mathutils import Vector
cmo=[x for x in bpy.data.objects if x.type=='MESH' and 'mat_cape' in x.name][0]
bm=bmesh.new(); bm.from_mesh(cmo.data); bm.verts.ensure_lookup_table()
seen=set(); isl=[]
for v in bm.verts:
    if v.index in seen: continue
    st=[v]; cur=[]; seen.add(v.index)
    while st:
        a=st.pop(); cur.append(a)
        for e in a.link_edges:
            b=e.other_vert(a)
            if b.index not in seen: seen.add(b.index); st.append(b)
    isl.append(cur)
big=sorted([c for c in isl if len(c)>400],key=len,reverse=True)[:2]
if len(big)==2:
    mw=cmo.matrix_world
    def spread(c):  # mean distance from the vertical body axis: the lining sits closer
        return np.mean([ (Vector(((mw@v.co).x,(mw@v.co).y-0.0,0))).length for v in c])
    inner=min(big,key=spread)
    bmesh.ops.delete(bm,geom=inner,context='VERTS')
    bm.to_mesh(cmo.data); print('CAPE lining removed',len(inner))
bm.free()
# ---- roll / bend bones: superseded - the retarget now sends roll joints to Movie Duels' twist bones ----
if False:
  pass
# (old weight spreading kept below for reference, disabled)
SPREAD_ROLLS = False
# ---- roll / bend bones: spread their weight between the joints they sit between, so twist and bend blend smoothly ----
R_=[x for x in bpy.data.objects if x.type=='ARMATURE'][0]
mwR=R_.matrix_world
def J(n): return mwR@R_.data.bones[n].head_local
for o in bpy.data.objects:
    if o.type!='MESH': continue
    gi={g.name:g.index for g in o.vertex_groups}
    rolls=[n for n in gi if re.search(r'(Roll|Bend)',n) and re.match(r'(Left|Right)',n)] if SPREAD_ROLLS else []
    if not rolls: continue
    mw=o.matrix_world; cnt=0
    for v in o.data.vertices:
        add={}
        for ge in list(v.groups):
            n=o.vertex_groups[ge.group].name
            if n not in rolls or ge.weight<=0: continue
            side='Left' if n.startswith('Left') else 'Right'
            p=mw@v.co
            if 'ForeArm' in n:   a,b,na,nb=J(side+'ForeArm'),J(side+'Hand'),side+'ForeArm',side+'Hand'; k=0.5
            elif 'Elbow' in n:   a,b,na,nb=J(side+'Arm'),J(side+'ForeArm'),side+'Arm',side+'ForeArm'; k=1.0
            elif 'UpLeg' in n:   a,b,na,nb=J(side+'UpLeg'),J(side+'Leg'),side+'UpLeg',side+'Leg'; k=0.3
            elif 'Arm' in n:     a,b,na,nb=J(side+'Arm'),J(side+'ForeArm'),side+'Arm',side+'ForeArm'; k=0.5
            else: continue
            d=b-a; t=max(0.0,min(1.0,(p-a).dot(d)/d.length_squared))
            if 'Elbow' in n: t=0.5
            wb=ge.weight*t*k if 'Elbow' not in n else ge.weight*0.5
            add[na]=add.get(na,0)+ge.weight-wb; add[nb]=add.get(nb,0)+wb
            o.vertex_groups[ge.group].add([v.index],0.0,'REPLACE')
        for n,w in add.items():
            g=o.vertex_groups.get(n) or o.vertex_groups.new(name=n)
            cur=0.0
            for ge in v.groups:
                if ge.group==g.index: cur=ge.weight
            g.add([v.index],cur+w,'REPLACE'); cnt+=1
    print('ROLL weights spread',o.name,cnt)
