"""Correspondence between a source skeleton and the JKA _humanoid base pose.

conv:   source world -> JKA model space (rotation, uniform scale, offset), from
        the skeleton itself (up = hips->head, side = right->left arm), so
        Y-up/Z-up, metres/centimetres and facing direction don't matter.
D[b]:   per JKA bone, the rotation that turns the source bind pose of that
        bone's segment onto the JKA base pose (hierarchical swing; the pelvis
        and chest also match the left-right axis). Used both to fit a mesh to
        the JKA skeleton and to retarget animations.
"""
import numpy as np

# JKA bone -> bone its segment points at (first one that is mapped wins)
SEGMENT = {
    'pelvis': None, 'lower_lumbar': ['upper_lumbar', 'thoracic'], 'upper_lumbar': ['thoracic'],
    'thoracic': None, 'cervical': ['cranium'], 'cranium': [],
    'lclavical': ['lhumerus'], 'rclavical': ['rhumerus'],
    'lhumerus': ['lradius'], 'rhumerus': ['rradius'], 'lradius': ['lhand'], 'rradius': ['rhand'],
    'lhand': ['l_d2_j1', 'l_d4_j1'], 'rhand': ['r_d2_j1', 'r_d4_j1'],
    'lfemurYZ': ['ltibia'], 'rfemurYZ': ['rtibia'], 'ltibia': ['ltalus'], 'rtibia': ['rtalus'], 'ltalus': [], 'rtalus': [],
}
for sd in 'lr':
    for d in ('d1', 'd2', 'd4'):
        SEGMENT['%s_%s_j1' % (sd, d)] = ['%s_%s_j2' % (sd, d)]
        SEGMENT['%s_%s_j2' % (sd, d)] = []


def orth(M):
    U, _, Vt = np.linalg.svd(M[:3, :3])
    R = U @ Vt
    if np.linalg.det(R) < 0:
        U[:, -1] *= -1; R = U @ Vt
    return R


def frame(up, lat):
    up = up / np.linalg.norm(up)
    lat = lat - up * np.dot(lat, up); lat /= np.linalg.norm(lat)
    return np.column_stack([lat, np.cross(up, lat), up])


def swing(a, b):
    if np.linalg.norm(a) < 1e-9 or np.linalg.norm(b) < 1e-9:
        return np.eye(3)
    a = a / np.linalg.norm(a); b = b / np.linalg.norm(b)
    v = np.cross(a, b); c = float(np.dot(a, b))
    if c < -0.999999:
        axis = np.cross(a, [1, 0, 0]);
        if np.linalg.norm(axis) < 1e-6: axis = np.cross(a, [0, 1, 0])
        axis /= np.linalg.norm(axis)
        return 2 * np.outer(axis, axis) - np.eye(3)
    K = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + K + K @ K / (1 + c)


class Fit:
    def __init__(self, src, gla, m, mp):
        self.src, self.gla, self.m, self.mp = src, gla, m, mp
        B = gla.bones; bi = gla.byName
        self.jpos = {b['name']: b['base'][:3, 3] for b in B}
        self.jrot = {b['name']: b['base'][:3, :3] for b in B}
        sp = lambda b: mp.pos[m[b]]
        # global conversion
        head_s = sp('cranium'); hips_s = sp('pelvis')
        feet_s = [sp(k) for k in ('ltalus', 'rtalus') if k in m]
        lat_s = sp('lhumerus') - sp('rhumerus')
        jp = self.jpos
        up_s = head_s - hips_s
        up_j = jp['cranium'] - jp['pelvis']
        R0 = frame(up_j, jp['lhumerus'] - jp['rhumerus']) @ frame(up_s, lat_s).T
        if feet_s:
            hs = np.linalg.norm(head_s - np.mean(feet_s, 0))
            hj = np.linalg.norm(jp['cranium'] - 0.5 * (jp['ltalus'] + jp['rtalus']))
        else:
            hs, hj = np.linalg.norm(up_s), np.linalg.norm(up_j)
        self.s = hj / hs
        self.R0 = R0
        self.t0 = jp['pelvis'] - self.s * R0 @ hips_s
        # converted source bind pose
        self.q = {n: self.conv_p(mp.pos[n]) for n in src.joints}
        self.qr = {n: R0 @ orth(src.bind[mp.idx[n]]) for n in src.joints}
        # per-JKA-bone alignment D, parents first
        self.D = {}
        order = [B[i]['name'] for i in gla.order]      # parents first
        for b in order:
            par = B[bi[b.lower()]]['parent']
            Dp = self.D.get(B[par]['name'], np.eye(3)) if par >= 0 else np.eye(3)
            if b not in m:
                self.D[b] = Dp; continue
            if b == 'pelvis' or b == 'thoracic':
                up_end = 'lower_lumbar' if b == 'pelvis' else 'cervical'
                lat = ('lfemurYZ', 'rfemurYZ') if b == 'pelvis' else ('lhumerus', 'rhumerus')
                if up_end in m and lat[0] in m and lat[1] in m:
                    Fs = frame(self.q[m[up_end]] - self.q[m[b]], self.q[m[lat[0]]] - self.q[m[lat[1]]])
                    Fj = frame(jp[up_end] - jp[b], jp[lat[0]] - jp[lat[1]])
                    self.D[b] = Fj @ Fs.T; continue
            if b in ('cervical', 'cranium') and 'thoracic' in self.D:
                # neck and head turn with the chest's alignment: both skeletons' rest poses hold
                # the head upright, so swinging the neck onto Movie Duels' (differently leaning)
                # neck would tilt the face up or down, and aligning the head on its own would
                # kink the neck between them
                self.D[b] = self.D['thoracic']; continue
            end = next((e for e in (SEGMENT.get(b) or []) if e in m and np.linalg.norm(self.q[m[e]] - self.q[m[b]]) > 1e-6), None)
            if end is None:
                self.D[b] = Dp; continue
            vs = Dp @ (self.q[m[end]] - self.q[m[b]])
            vj = jp[end] - jp[b]
            self.D[b] = swing(vs, vj) @ Dp
        # source joint -> JKA bone it moves with (itself if mapped, else nearest mapped ancestor)
        inv = {n: b for b, n in m.items()}
        self.owner = {}
        for n in src.joints:
            k = n
            while k is not None and k not in inv:
                k = mp.par[k]
            self.owner[n] = inv[k] if k is not None else 'pelvis'

    def conv_p(self, p):
        return self.s * self.R0 @ p + self.t0

    def bone_xform(self, b):
        """3x4 affine taking a converted source point near JKA bone b onto the JKA skeleton"""
        n = self.m[b] if b in self.m else self.m['pelvis']
        q = self.q[n]; p = self.jpos[b] if b in self.m else self.jpos['pelvis']
        D = self.D[b]
        K = np.eye(3)
        end = next((e for e in (SEGMENT.get(b) or []) if e in self.m), None)
        if end is not None:
            ls = np.linalg.norm(self.q[self.m[end]] - q); lj = np.linalg.norm(self.jpos[end] - p)
            if ls > 1e-6:
                k = float(np.clip(lj / ls, 0.6, 1.6))
                u = (self.jpos[end] - p) / max(lj, 1e-9)
                K = np.eye(3) + (k - 1) * np.outer(u, u)
        A = K @ D
        return A, p - A @ q, D
