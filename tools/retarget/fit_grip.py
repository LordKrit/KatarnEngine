"""Seat the saber bolt in the BF2 fist: grip axis through the hole of the closed fingers."""
import sys, struct, numpy as np
sys.path.insert(0, '/home/claude/retarget')
from render import read_glm, skin_matrices, pose
from g2 import GLA
glm, gla, out, frame = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
hilt = read_glm('/tmp/hilt/models/weapons2/saber_plasma/saber_dooku.glm')
HP = np.vstack([h['P'] for h in hilt if not h['flags'] & 3])
near = HP[np.linalg.norm(HP, axis=1) < float(__import__('os').environ.get('GR','3.5'))]
GRIP_C = near.mean(0); _, _, Vt = np.linalg.svd(near - GRIP_C); GRIP_AX = Vt[0]
print('hilt grip centre', np.round(GRIP_C,2), 'axis', np.round(GRIP_AX,2), len(near))
g = GLA(gla); S = skin_matrices(g, frame); surfs = read_glm(glm)
def bolt_world(P):
    s0 = P[1]-P[0]; s2 = P[0]-P[2]
    a1 = s2/np.linalg.norm(s2); a0 = s0-np.dot(s0,a1)*a1; a0 /= np.linalg.norm(a0)
    a2 = np.cross(s0,s2); a2 /= np.linalg.norm(a2)
    M = np.eye(4); M[:3,0]=a1; M[:3,1]=a0; M[:3,2]=-a2; M[:3,3]=P[2]; return M
d = bytearray(open(glm,'rb').read())
numBones,numLODs,ofsLODs,ns,_ = struct.unpack_from('<5i',d,140)
tab = struct.unpack_from('<%di'%ns,d,164)
names = [d[164+tab[i]:164+tab[i]+64].split(b'\0')[0].decode() for i in range(ns)]
for side, nm in (('r','right'),('l','left')):
    fb = {g.byName[n] for n in g.byName if n.startswith(nm+'hand') and 'thumb' not in n}
    # finger vertices (mostly weighted to finger bones)
    pts = []
    for s in surfs:
        if s['flags'] & 3: continue
        P = pose([s], S)[0][1]
        for k, ws in enumerate(s['W']):
            if sum(w for b, w in ws if b in fb) > 0.6: pts.append(P[k])
    pts = np.array(pts)
    J = lambda n: (S[g.byName[n]] @ g.bones[g.byName[n]]['base'])[:3,3]
    tag = [s for s in surfs if s['name']=='*%s_hand'%side][0]
    M = bolt_world(pose([dict(tag,flags=0)],S)[0][1])
    ax = M[:3,:3] @ GRIP_AX                        # the hilt's grip direction, rotation kept as it is
    # cross-section perpendicular to the grip axis
    u = np.cross(ax, [0,0,1.0]); u /= np.linalg.norm(u); v = np.cross(ax, u)
    c0 = pts.mean(0)
    Q = np.stack([(pts-c0)@u, (pts-c0)@v], 1)
    best = None
    for x in np.linspace(Q[:,0].min(), Q[:,0].max(), 60):
        for y in np.linspace(Q[:,1].min(), Q[:,1].max(), 60):
            dd = np.linalg.norm(Q-[x,y],axis=1)
            # inside: points on all sides (angular coverage)
            ang = np.arctan2(Q[:,1]-y, Q[:,0]-x)[dd < 3.0]
            if len(ang) < 20: continue
            h = np.histogram(ang, 8, (-np.pi,np.pi))[0]
            if (h > 0).sum() < 6: continue
            m = dd.min()
            if best is None or m > best[0]: best = (m, x, y)
    m, x, y = best
    proj = (pts-c0)@ax
    org = c0 + u*x + v*y + ax*(0.5*(proj.min()+proj.max()))
    print(side, 'hole radius %.2f' % m, 'grip centre was %.2f from the hole' % np.linalg.norm(org - (M[:3,:3] @ GRIP_C + M[:3,3])),
          '')
    # new frame: Y = grip axis, keep the old X as close as possible
    # translation only: the hilt's grip centre goes to the fist hole
    shift = org - (M[:3,:3] @ GRIP_C + M[:3,3])
    T0 = pose([dict(tag,flags=0)],S)[0][1][:3]
    W = [T0[k] + shift for k in range(3)]
    lso = ofsLODs
    for L in range(numLODs):
        base = lso+4
        for i in range(ns):
            so = base + struct.unpack_from('<i',d,base+i*4)[0]
            _,idx,_,nv,ofsV,nt,ofsT,nbr,ofsBR,_ = struct.unpack_from('<10i',d,so)
            if names[idx] != '*%s_hand'%side: continue
            refs = struct.unpack_from('<%di'%nbr,d,so+ofsBR)
            for k in range(3):
                o = so+ofsV+k*32; b = refs[struct.unpack_from('<I',d,o+24)[0]&31]
                struct.pack_into('<3f', d, o+12, *(np.linalg.inv(S[b]) @ np.append(W[k],1))[:3])
        lso += struct.unpack_from('<i',d,lso)[0]
open(out,'wb').write(d)
