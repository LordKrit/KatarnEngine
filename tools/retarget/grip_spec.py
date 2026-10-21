import sys, json, numpy as np
sys.path.insert(0,'/home/claude/retarget')
from render import read_glm, skin_matrices, pose
from g2 import GLA
glm, gla, frame, out = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
g=GLA(gla); S=skin_matrices(g,frame); surfs=read_glm(glm)
H=np.vstack([h['P'] for h in read_glm('/tmp/hilt/models/weapons2/saber_plasma/saber_dooku.glm') if not h['flags']&3])
near=H[np.linalg.norm(H,axis=1)<4]; C=near.mean(0); _,_,Vt=np.linalg.svd(near-C); A=Vt[0]
def bw(P):
    s0=P[1]-P[0]; s2=P[0]-P[2]; a1=s2/np.linalg.norm(s2); a0=s0-np.dot(s0,a1)*a1; a0/=np.linalg.norm(a0)
    a2=np.cross(s0,s2); a2/=np.linalg.norm(a2); M=np.eye(4); M[:3,0]=a1; M[:3,1]=a0; M[:3,2]=-a2; M[:3,3]=P[2]; return M
spec={}
for side in 'rl':
    t=[s for s in surfs if s['name']=='*%s_hand'%side][0]; M=bw(pose([dict(t,flags=0)],S)[0][1])
    h=g.byName[side+'hand']; W=S[h]@g.bones[h]['base']; R=W[:3,:3]/np.linalg.norm(W[:3,0])
    c=M[:3,:3]@C+M[:3,3]; d=M[:3,:3]@A
    spec[side]=dict(axis=list(R.T@d), centre=list(R.T@(c-W[:3,3])), radius=1.15)
json.dump(spec,open(out,'w'),indent=1); print(spec)
