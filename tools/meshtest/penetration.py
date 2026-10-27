# cape points behind the real body surface (nearest body vertex, its normal), over a recording
import sys, os, numpy as np
sys.path.insert(0,'/home/claude/md/tools/retarget')
from render import read_glm
from g2 import GLA
from scipy.spatial import cKDTree
glm,gla,rec=sys.argv[1:4]; CN=sys.argv[4].split(',') if len(sys.argv)>4 else ['cape']; ZF=float(sys.argv[5]) if len(sys.argv)>5 else 39.0
s=read_glm(glm); nb=GLA(gla).numBones
cl=[x for x in s if x['name'] in CN]; ncv=sum(len(x['P']) for x in cl)
FREE=np.concatenate([x['P'][:,2] for x in cl])<ZF
print('bind z range', np.concatenate([x['P'][:,2] for x in cl]).min(), np.concatenate([x['P'][:,2] for x in cl]).max(), 'free', FREE.sum())
body=[x for x in s if not x['flags']&3 and x['name'] not in CN and len(x['P']) and 'innercape' not in x['shader']]
d=open(rec,'rb').read(); fs=20+nb*48+ncv*6; nf=len(d)//fs
tot=0; deep=0; cnt=0; H={}
for f in range(30,nf,3):
    o=f*fs; B=np.frombuffer(d,np.float32,nb*12,o+20).reshape(nb,3,4)
    C=np.frombuffer(d,np.int16,ncv*3,o+20+nb*48).reshape(-1,3)/64.
    if os.environ.get('ANIM'):
        C=[]
        for x in cl:
            for i,ws in enumerate(x['W']):
                p=np.zeros(3)
                for b,w in ws: p+=w*(B[b][:,:3]@x['P'][i]+B[b][:,3])
                C.append(p)
        C=np.array(C)
    P=[];N=[];BB=[]
    for x in body:
        for i,ws in enumerate(x['W']):
            p=np.zeros(3); n=np.zeros(3)
            for b,w in ws: p+=w*(B[b][:,:3]@x['P'][i]+B[b][:,3]); n+=w*(B[b][:,:3]@x['N'][i])
            P.append(p); N.append(n); BB.append(max(ws,key=lambda t:t[1])[0])
    P=np.array(P); N=np.array(N); N/=np.linalg.norm(N,axis=1,keepdims=True)+1e-9
    dist,idx=cKDTree(P).query(C)
    side=((C-P[idx])*N[idx]).sum(1)
    inside=(side<-0.5)&(dist<6)&FREE
    from collections import Counter
    for b in np.array(BB)[idx[inside]]: H[b]=H.get(b,0)+1
    tot+=inside.sum(); deep+=((side<-2)&(dist<6)&FREE).sum(); cnt+=1
print('cape points behind body surface per frame: %.1f (>2 units deep: %.1f)'%(tot/cnt, deep/cnt))
g=GLA(gla)
print(sorted(((v,{v:k for k,v in g.byName.items()}.get(b,b)) for b,v in H.items()),reverse=True)[:8])
