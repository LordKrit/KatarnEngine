import sys, numpy as np
sys.path.insert(0,'/home/claude/retarget')
from render import read_glm, skin_matrices, pose, render
from g2 import GLA
from PIL import Image
out=sys.argv[1]; ims=[]
for job in sys.argv[2:]:
    glm,gla,f,label=job.split('|'); f=int(f)
    g=GLA(gla); S=skin_matrices(g,f); posed=pose(read_glm(glm),S)
    for side in 'rl':
        h=g.byName[side+'hand']; c=(S[h]@g.bones[h]['base'])[:3,3]
        sub=[]
        for s,P in posed:
            near=np.linalg.norm(P-c,axis=1)<float(__import__("os").environ.get("HR","6"))
            t=s['tris'][near[s['tris']].all(1)]
            if len(t): sub.append((dict(s,tris=t,name='x'),P))
        for yaw in (0,90,180):
            ims.append(render(sub,yaw,260,None,'%s f%d %s y%d'%(label,f,side,yaw)))
cols=6; W=Image.new('RGB',(260*cols,260*((len(ims)+cols-1)//cols)),'white')
for k,im in enumerate(ims): W.paste(im,((k%cols)*260,(k//cols)*260))
W.save(out)
