import sys, numpy as np, os
sys.path.insert(0,'/home/claude/retarget')
from render import read_glm, skin_matrices, pose, render
from g2 import GLA
from PIL import Image
glm, gla, out, frame = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
g=GLA(gla); surfs=read_glm(glm); S0=skin_matrices(g,frame)
kids={}
for i,b in enumerate(g.bones): kids.setdefault(b['parent'],[]).append(i)
def sub(i):
    o=[i]; st=[i]
    while st:
        j=st.pop(); o+=kids.get(j,[]); st+=kids.get(j,[])
    return set(o)
def rotated(S, bone, deg):
    S=[s.copy() for s in S]; i=g.byName[bone.lower()]; W=S[i]@g.bones[i]['base']
    cr=g.byName['cranium']; C=S[cr]@g.bones[cr]['base']; ax=C[:3,0]/np.linalg.norm(C[:3,0])
    a=np.radians(deg); K=np.array([[0,-ax[2],ax[1]],[ax[2],0,-ax[0]],[-ax[1],ax[0],0]]); R=np.eye(3)+np.sin(a)*K+(1-np.cos(a))*K@K
    T=np.eye(4); T[:3,:3]=R; T[:3,3]=W[:3,3]-R@W[:3,3]
    for j in sub(i): S[j]=T@S[j]
    return S
cr=g.byName['cranium']; hc=(S0[cr]@g.bones[cr]['base'])[:3,3]
ims=[]
for lab,S in [('rest',S0)]+[('jaw %+d'%d, rotated(S0,'FACIAL_LOD1_C_Jaw',d)) for d in (12,-12)]:
    posed=pose(surfs,S); subp=[]
    for s,P in posed:
        near=np.linalg.norm(P-hc,axis=1)<9
        t=s['tris'][near[s['tris']].all(1)]
        if len(t):
            u=np.unique(t); m=-np.ones(len(P),int); m[u]=np.arange(len(u)); subp.append((dict(s,tris=m[t]),P[u]))
    for yaw in (0,70):
        ims.append(render(subp,yaw,360,None,'%s y%d'%(lab,yaw)))
W=Image.new('RGB',(360*len(ims),360),'white')
for k,im in enumerate(ims): W.paste(im,(k*360,0))
W.save(out)
