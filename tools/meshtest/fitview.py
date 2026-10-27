import sys, struct, numpy as np
sys.path.insert(0,'/home/claude/md/tools/retarget')
from render import read_glm
from g2 import GLA
import matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
from matplotlib.patches import Circle
glm,gla,rec,fit,out=sys.argv[1:6]
s=read_glm(glm); nb=GLA(gla).numBones
ncv=sum(len(x['P']) for x in s if x['name']=='cape')
d=open(rec,'rb').read(); fs=20+nb*48+ncv*6
B=np.frombuffer(d,np.float32,nb*12,20).reshape(nb,3,4)
P=[]
for x in s:
    if x['flags']&3 or x['name']=='cape' or not len(x['P']): continue
    for i,ws in enumerate(x['W']):
        p=np.zeros(3)
        for b,w in ws: p+=w*(B[b][:,:3]@x['P'][i]+B[b][:,3])
        P.append(p)
P=np.array(P)
F=[l.split() for l in open(fit) if l.startswith('FIT')]
F=[(np.array(list(map(float,f[3:6]))),np.array(list(map(float,f[7:10]))),float(f[11])) for f in F]
fig,ax=plt.subplots(1,2,figsize=(10,8))
for k,(i,j,lab) in enumerate(((0,2,'front x-z'),(1,2,'side y-z'))):
    a=ax[k]; a.scatter(P[:,i],P[:,j],s=0.2,c='0.6'); a.set_aspect('equal'); a.set_title(lab)
    for A,Bp,r in F:
        for t in np.linspace(0,1,6):
            q=A+(Bp-A)*t; a.add_patch(Circle((q[i],q[j]),r,fill=False,ec='r',lw=0.6))
plt.savefig(out,dpi=80)
