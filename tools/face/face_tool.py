"""Write ext_data/faces/<model>.face for the engine's face system (code/cgame/cg_face.cpp).

Poses are authored in the model's head space at the bind pose (x = character's left, y = back,
z = up, game units) as world-space moves of face bones; this tool turns them into each bone's
local rotation / translation, accounting for the face bone hierarchy, which is what the engine
applies on top of the animation.

usage: face_tool.py <skeleton .gla> <out .face> [preview]
The pose recipes live in face_poses.py (one dict per model rig family; BF2 FACIAL_ rigs here).
"""
import sys, numpy as np
from g2 import GLA

def axis_angle_to_R(axis, deg):
    a = np.radians(deg); k = np.asarray(axis, float); k = k / np.linalg.norm(k)
    K = np.array([[0, -k[2], k[1]], [k[2], 0, -k[0]], [-k[1], k[0], 0]])
    return np.eye(3) + np.sin(a) * K + (1 - np.cos(a)) * K @ K

def R_to_rotvec(R):
    c = np.clip((np.trace(R) - 1) / 2, -1, 1); a = np.arccos(c)
    if a < 1e-8: return np.zeros(3)
    v = np.array([R[2, 1] - R[1, 2], R[0, 2] - R[2, 0], R[1, 0] - R[0, 1]]) / (2 * np.sin(a))
    return v * a

class Rig:
    def __init__(self, gla_path, prefix='facial_'):
        self.g = g = GLA(gla_path)
        self.W = {}       # bind world (unit scale) per bone index
        for i, b in enumerate(g.bones):
            M = b['base'].copy(); s = np.cbrt(abs(np.linalg.det(M[:3, :3])))
            M[:3, :3] /= s; self.W[i] = M
        self.face = [i for i, b in enumerate(g.bones) if b['name'].lower().startswith(prefix) or b['name'].lower().startswith('eyeball_')]
        self.order = [i for i in g.order if i in set(self.face)]
    def idx(self, name):
        return self.g.byName[name.lower()]

    def solve(self, deltas):
        """deltas: {bone index: 4x4 world-space delta applied about nothing (already includes pivot)}
        returns {bone: (rotvec, t)} local overrides so that each bone ends at delta * (its parent's
        moved frame * bind relative transform)."""
        g = self.g; Wn = {}; out = {}
        for i in self.order:
            p = g.bones[i]['parent']
            Wp = self.W[p]; Wp_new = Wn.get(p, Wp)
            rel = np.linalg.inv(Wp) @ self.W[i]
            inherited = Wp_new @ rel
            D = deltas.get(i, np.eye(4))
            target = D @ inherited
            Wn[i] = target
            L = np.linalg.inv(inherited) @ target
            if np.abs(L - np.eye(4)).max() > 1e-6:
                out[i] = (R_to_rotvec(L[:3, :3]), L[:3, 3].copy())
        return out

def rot_about(R, pivot):
    D = np.eye(4); D[:3, :3] = R; D[:3, 3] = pivot - R @ pivot; return D

def move(v):
    D = np.eye(4); D[:3, 3] = v; return D

def build(rig, recipe):
    """recipe: list of ops
       ('rot', [bones], axis, deg, pivot_bone, weight)  rotate bones about the pivot bone's position
       ('move', [bones], (dx, dy, dz))                    translate bones (head space)"""
    deltas = {}
    for op in recipe:
        if op[0] == 'rot':
            _, bones, axis, deg, pivot, *w = op
            wt = w[0] if w else 1.0
            for b in bones:
                for i in expand(rig, b):
                    pv = rig.W[i][:3, 3] if pivot is None else rig.W[rig.idx(pivot)][:3, 3]
                    deltas[i] = rot_about(axis_angle_to_R(axis, deg * wt), pv) @ deltas.get(i, np.eye(4))
        elif op[0] == 'move':
            _, bones, v = op
            for b in bones:
                for i in expand(rig, b):
                    deltas[i] = move(np.asarray(v, float)) @ deltas.get(i, np.eye(4))
    return deltas

def expand(rig, pat):
    import re
    rx = re.compile(pat, re.I)
    return [i for i in rig.face if rx.fullmatch(rig.g.bones[i]['name'])]

def write_face(rig, poses, path):
    used = sorted({i for p in poses.values() for i in p})
    with open(path, 'w') as f:
        f.write('face 1\n// written by face_tool.py; poses are local rotation (axis*angle, rad) + translation per bone\n')
        for i in used:
            B = rig.g.bones[i]['base']
            f.write('bone %s %s\n' % (rig.g.bones[i]['name'], ' '.join('%.6f' % v for v in B[:3, :4].ravel())))
        for name, p in poses.items():
            f.write('pose %s\n' % name)
            for i, (r, t) in sorted(p.items()):
                # the local frame of the engine is the bone's base pose frame, which may carry scale
                s = np.cbrt(abs(np.linalg.det(rig.g.bones[i]['base'][:3, :3])))
                f.write('%s %.5f %.5f %.5f %.5f %.5f %.5f\n' % (rig.g.bones[i]['name'], *r, *(t / s)))
            f.write('end\n')

if __name__ == '__main__':
    import face_poses
    rig = Rig(sys.argv[1])
    poses = {n: rig.solve(build(rig, rec)) for n, rec in face_poses.BF2.items()}
    write_face(rig, poses, sys.argv[2])
    print('wrote %s: %d poses, %d bones' % (sys.argv[2], len(poses), len({i for p in poses.values() for i in p})))
