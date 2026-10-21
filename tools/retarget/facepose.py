import sys, numpy as np
sys.path.insert(0,'/home/claude/retarget')
from render import read_glm, skin_matrices, pose, render
from g2 import GLA, uncomp
from PIL import Image
glm, gla, face, out, frame = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], int(sys.argv[5])
names = sys.argv[6].split(',')
g=GLA(gla); surfs=read_glm(glm); S=skin_matrices(g,frame)
poses={}; cur=None
for line in open(face):
    t=line.split()
    if not t or t[0].startswith('//'): continue
    if t[0]=='pose': cur=poses.setdefault(t[1],{})
    elif t[0]=='end': cur=None
    elif cur is not None:
        r=np.array(list(map(float,t[1:4]))); tr=np.array(list(map(float,t[4:7])))
        cur[g.byName[t[0].lower()]]=(r,tr)
def rodr(r):
    a=np.linalg.norm(r)
    if a<1e-9: return np.eye(3)
    k=r/a; K=np.array([[0,-k[2],k[1]],[k[2],0,-k[0]],[-k[1],k[0],0]]); return np.eye(3)+np.sin(a)*K+(1-np.cos(a))*K@K
def apply(p):
    G=[S[i]@g.bones[i]['base'] for i in range(g.numBones)]; Gn=list(G)
    for i in g.order:
        pa=g.bones[i]['parent']
        if pa<0: continue
        L=np.eye(4)
        if i in p: L[:3,:3]=rodr(p[i][0]); L[:3,3]=p[i][1]*np.cbrt(abs(np.linalg.det(g.bones[i]['base'][:3,:3])))
        Gn[i]=Gn[pa]@np.linalg.inv(G[pa])@G[i]@L
    return [Gn[i]@np.linalg.inv(g.bones[i]['base']) for i in range(g.numBones)]
cr=g.byName['cranium']; hc=(S[cr]@g.bones[cr]['base'])[:3,3]
mouth=(S[g.byName['facial_lod1_c_mouth']]@g.bones[g.byName['facial_lod1_c_mouth']]['base'])[:3,3]
ims=[]
for n in names:
    Sp=apply(poses.get(n,{})); posed=pose(surfs,Sp); sub=[]
    for s,P in posed:
        if s['name'].lower().startswith(('hair','cape')): continue
        near=np.linalg.norm(P-(hc+mouth)/2,axis=1)<4.2
        t=s['tris'][near[s['tris']].all(1)]
        if len(t):
            u=np.unique(t); m=-np.ones(len(P),int); m[u]=np.arange(len(u)); sub.append((dict(s,tris=m[t]),P[u]))
    for yaw in (0,60):
        ims.append(render(sub,yaw,300,None,'%s y%d'%(n,yaw)))
cols=6; W=Image.new('RGB',(300*cols,300*((len(ims)+cols-1)//cols)),'white')
for k,im in enumerate(ims): W.paste(im,((k%cols)*300,(k//cols)*300))
W.save(out)
