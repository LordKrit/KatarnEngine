"""grip line just inside the palm, along the knuckles, in the hand bone's frame"""
import sys, json, numpy as np
sys.path.insert(0,'/home/claude/retarget')
from render import skin_matrices
from g2 import GLA
gla, frame, out, off = sys.argv[1], int(sys.argv[2]), sys.argv[3], float(sys.argv[4])
g=GLA(gla); S=skin_matrices(g,frame)
J=lambda n:(S[g.byName[n]]@g.bones[g.byName[n]]['base'])[:3,3]
spec={}
for side,nm in (('r','right'),('l','left')):
    h=g.byName[side+'hand']; W=S[h]@g.bones[h]['base']; R=W[:3,:3]/np.linalg.norm(W[:3,0])
    i1,p1,t1,hp=J(nm+'handindex1'),J(nm+'handpinky1'),J(nm+'handthumb1'),W[:3,3]
    ax=p1-i1; ax/=np.linalg.norm(ax)
    n=np.cross(i1-hp,p1-hp); n/=np.linalg.norm(n)
    if np.dot(t1-hp,n)<0: n=-n
    fwd=(i1+p1)/2-hp; fwd-=np.dot(fwd,ax)*ax; fwd/=np.linalg.norm(fwd)
    c=(i1+p1)/2 + n*off - fwd*0.3
    spec[side]=dict(axis=list(map(float,R.T@ax)),centre=list(map(float,R.T@(c-hp))),radius=1.15)
json.dump(spec,open(out,'w')); print(spec)
