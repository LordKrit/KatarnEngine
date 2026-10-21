"""turn and move each saber bolt so the hilt's grip lies on the grip line (hand frame) from the spec"""
import sys, json, struct, numpy as np
sys.path.insert(0,'/home/claude/retarget')
from render import read_glm, skin_matrices, pose
from g2 import GLA
glm, gla, spec, out, frame = sys.argv[1], sys.argv[2], json.load(open(sys.argv[3])), sys.argv[4], int(sys.argv[5])
g=GLA(gla); S=skin_matrices(g,frame); surfs=read_glm(glm)
H=np.vstack([h['P'] for h in read_glm('/tmp/hilt/models/weapons2/saber_plasma/saber_dooku.glm') if not h['flags']&3])
near=H[np.linalg.norm(H,axis=1)<4]; C=near.mean(0); _,_,Vt=np.linalg.svd(near-C); A=Vt[0]
def bw(P):
    s0=P[1]-P[0]; s2=P[0]-P[2]; a1=s2/np.linalg.norm(s2); a0=s0-np.dot(s0,a1)*a1; a0/=np.linalg.norm(a0)
    a2=np.cross(s0,s2); a2/=np.linalg.norm(a2); M=np.eye(4); M[:3,0]=a1; M[:3,1]=a0; M[:3,2]=-a2; M[:3,3]=P[2]; return M
def rot(u,v):
    u=u/np.linalg.norm(u); v=v/np.linalg.norm(v); a=np.cross(u,v); s=np.linalg.norm(a); c=u@v
    if s<1e-9: return np.eye(3)
    k=a/s; K=np.array([[0,-k[2],k[1]],[k[2],0,-k[0]],[-k[1],k[0],0]]); return np.eye(3)+s*K+(1-c)*K@K
d=bytearray(open(glm,'rb').read())
nb,nl,ofsL,ns,_=struct.unpack_from('<5i',d,140); tab=struct.unpack_from('<%di'%ns,d,164)
names=[d[164+tab[i]:164+tab[i]+64].split(b'\0')[0].decode() for i in range(ns)]
for side,sp in spec.items():
    t=[s for s in surfs if s['name']=='*%s_hand'%side][0]; M=bw(pose([dict(t,flags=0)],S)[0][1])
    h=g.byName[side+'hand']; W=S[h]@g.bones[h]['base']; R=W[:3,:3]/np.linalg.norm(W[:3,0])
    ax=R@np.array(sp['axis']); c=R@np.array(sp['centre'])+W[:3,3]
    cur=M[:3,:3]@A
    if cur@ax<0: ax=-ax
    Q=rot(cur,ax); Rn=Q@M[:3,:3]
    org=c-Rn@C
    print(side,'turned %.0f deg, moved %.2f'%(np.degrees(np.arccos(np.clip(cur@ax,-1,1))),np.linalg.norm(org-M[:3,3])))
    X,Y=Rn[:,0],Rn[:,1]; Wv=[org+2*X,org+2*X+3*Y,org]
    lso=ofsL
    for L in range(nl):
        base=lso+4
        for i in range(ns):
            so=base+struct.unpack_from('<i',d,base+i*4)[0]
            _,idx,_,nv,ofsV,nt,ofsT,nbr,ofsBR,_=struct.unpack_from('<10i',d,so)
            if names[idx]!='*%s_hand'%side: continue
            refs=struct.unpack_from('<%di'%nbr,d,so+ofsBR)
            for k in range(3):
                o=so+ofsV+k*32; b=refs[struct.unpack_from('<I',d,o+24)[0]&31]
                struct.pack_into('<3f',d,o+12,*(np.linalg.inv(S[b])@np.append(Wv[k],1))[:3])
        lso+=struct.unpack_from('<i',d,lso)[0]
open(out,'wb').write(d)
