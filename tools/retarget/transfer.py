"""Copy MD Dooku's saber hold onto the BF2 hand: the bolt is expressed in a frame built from each hand's
own landmarks (wrist, index and pinky knuckles, thumb), offsets scaled by knuckle width."""
import sys, json, struct, numpy as np
sys.path.insert(0,'/home/claude/retarget')
from render import read_glm, skin_matrices, pose
from g2 import GLA
mdglm, mdgla, bfglm, bfgla, out, spec_out = sys.argv[1:7]; frames=[int(x) for x in sys.argv[7].split(',')]
def bw(P):
    s0=P[1]-P[0]; s2=P[0]-P[2]; a1=s2/np.linalg.norm(s2); a0=s0-np.dot(s0,a1)*a1; a0/=np.linalg.norm(a0)
    a2=np.cross(s0,s2); a2/=np.linalg.norm(a2); M=np.eye(4); M[:3,0]=a1; M[:3,1]=a0; M[:3,2]=-a2; M[:3,3]=P[2]; return M
def landmarks(g,S,side,md):
    J=lambda n:(S[g.byName[n]]@g.bones[g.byName[n]]['base'])[:3,3]
    nm={'r':'right','l':'left'}[side]
    if md: h,i,p,t=J(side+'hand'),J(side+'_d2_j1'),J(side+'_d4_j1'),J(side+'_d1_j1')
    else: h,i,p,t=J(side+'hand'),J(nm+'handindex1'),J(nm+'handpinky1'),J(nm+'handthumb1')
    x=p-i; w=np.linalg.norm(x); x/=w                     # across the knuckles
    k=(i+p)/2; y=k-h; y-=np.dot(y,x)*x; y/=np.linalg.norm(y) # wrist -> knuckles
    z=np.cross(x,y)
    if np.dot(t-h,z)<0: z=-z; x=-x                        # palm side (thumb side)
    F=np.eye(4); F[:3,0]=x; F[:3,1]=y; F[:3,2]=z; F[:3,3]=k
    return F,w
mg,ms=GLA(mdgla),read_glm(mdglm); bg,bs=GLA(bfgla),read_glm(bfglm)
d=bytearray(open(bfglm,'rb').read())
nb,nl,ofsL,ns,_=struct.unpack_from('<5i',d,140); tab=struct.unpack_from('<%di'%ns,d,164)
names=[d[164+tab[i]:164+tab[i]+64].split(b'\0')[0].decode() for i in range(ns)]
H=np.vstack([h['P'] for h in read_glm('/tmp/hilt/models/weapons2/saber_plasma/saber_dooku.glm') if not h['flags']&3])
near=H[np.linalg.norm(H,axis=1)<4]; C=near.mean(0); _,_,Vt=np.linalg.svd(near-C); A=Vt[0]
spec={}
for side in 'rl':
    rels=[]
    for f in frames:
        S=skin_matrices(mg,f); t=[s for s in ms if s['name']=='*%s_hand'%side][0]
        M=bw(pose([dict(t,flags=0)],S)[0][1]); F,w=landmarks(mg,S,side,True)
        R=np.linalg.inv(F)@M; R[:3,3]/=w; rels.append(R)
    rel=np.mean(rels,0); U,_,Vt2=np.linalg.svd(rel[:3,:3]); rel[:3,:3]=U@Vt2
    print(side,'MD bolt in knuckle frame (per knuckle width):',np.round(rel[:3,3],2),'spread',np.round(np.std([r[:3,3] for r in rels],0),2))
    f=frames[0]; S=skin_matrices(bg,f); F,w=landmarks(bg,S,side,False)
    rr=rel.copy(); rr[:3,3]*=w; W=F@rr
    X,Y,O=W[:3,0],W[:3,1],W[:3,3]; Wv=[O+2*X,O+2*X+3*Y,O]
    h=bg.byName[side+'hand']; HB=S[h]@bg.bones[h]['base']; Rh=HB[:3,:3]/np.linalg.norm(HB[:3,0])
    spec[side]=dict(axis=list(map(float,Rh.T@(W[:3,:3]@A))),centre=list(map(float,Rh.T@(W[:3,:3]@C+O-HB[:3,3]))),radius=1.15)
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
open(out,'wb').write(d); json.dump(spec,open(spec_out,'w')); print(spec)
