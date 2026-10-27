"""Per-model skeleton: keep the model exactly as made, and convert the animations instead.

The new skeleton has every _humanoid bone (same names, so the game code that turns
heads, aims arms or bolts sabers finds them) placed at the model's own joints, plus
all the model's extra bones (cape, fingers, face, twist). A _humanoid bone's base
orientation is the Movie Duels base orientation swung onto the model's limb, so an
animation frame gives each _humanoid bone exactly Movie Duels' world rotation;
positions come from the model's own bone lengths.
"""
import os, re, struct, zipfile, shutil
import numpy as np
from g2 import GLA, mat_to_quat
from fit import Fit
import glmwrite


# ---------------------------------------------------------------- vectorised helpers
def decode_frames(gla, f0, f1):
    """skin matrices (F, nb, 4, 4) of frames [f0, f1)"""
    nb = gla.numBones; F = f1 - f0
    raw = np.frombuffer(gla.d, np.uint8, count=F * nb * 3, offset=gla.ofsFrames + f0 * nb * 3).reshape(F, nb, 3).astype(np.int64)
    idx = raw[..., 0] | (raw[..., 1] << 8) | (raw[..., 2] << 16)
    pool = np.frombuffer(gla.d, np.uint16, offset=gla.ofsPool, count=((len(gla.d) - gla.ofsPool) // 14) * 7).reshape(-1, 7)
    c = pool[idx].astype(np.float64)
    q = c[..., :4] / 16383.0 - 2.0
    t = c[..., 4:] / 64.0 - 512.0
    L = np.zeros((F, nb, 4, 4)); L[..., 3, 3] = 1
    L[..., :3, :3] = quat_to_mat_v(q); L[..., :3, 3] = t
    S = np.zeros_like(L)
    for i in gla.order:
        p = gla.bones[i]['parent']
        S[:, i] = L[:, i] if p < 0 else S[:, p] @ L[:, i]
    return S


def quat_to_mat_v(q):
    w, x, y, z = q[..., 0], q[..., 1], q[..., 2], q[..., 3]
    M = np.empty(q.shape[:-1] + (3, 3))
    M[..., 0, 0] = 1 - 2 * (y * y + z * z); M[..., 0, 1] = 2 * (x * y - w * z); M[..., 0, 2] = 2 * (x * z + w * y)
    M[..., 1, 0] = 2 * (x * y + w * z); M[..., 1, 1] = 1 - 2 * (x * x + z * z); M[..., 1, 2] = 2 * (y * z - w * x)
    M[..., 2, 0] = 2 * (x * z - w * y); M[..., 2, 1] = 2 * (y * z + w * x); M[..., 2, 2] = 1 - 2 * (x * x + y * y)
    return M


def mat_to_quat_v(R):
    """(..., 3, 3) -> (..., 4) w x y z, w >= 0"""
    m00, m11, m22 = R[..., 0, 0], R[..., 1, 1], R[..., 2, 2]
    q = np.empty(R.shape[:-2] + (4,))
    tr = m00 + m11 + m22
    c0 = tr > 0
    c1 = (~c0) & (m00 > m11) & (m00 > m22)
    c2 = (~c0) & (~c1) & (m11 > m22)
    c3 = ~(c0 | c1 | c2)
    s = np.sqrt(np.maximum(tr + 1, 1e-12)) * 2
    q[c0] = np.stack([0.25 * s, (R[..., 2, 1] - R[..., 1, 2]) / s, (R[..., 0, 2] - R[..., 2, 0]) / s, (R[..., 1, 0] - R[..., 0, 1]) / s], -1)[c0]
    s = np.sqrt(np.maximum(1 + m00 - m11 - m22, 1e-12)) * 2
    q[c1] = np.stack([(R[..., 2, 1] - R[..., 1, 2]) / s, 0.25 * s, (R[..., 0, 1] + R[..., 1, 0]) / s, (R[..., 0, 2] + R[..., 2, 0]) / s], -1)[c1]
    s = np.sqrt(np.maximum(1 + m11 - m00 - m22, 1e-12)) * 2
    q[c2] = np.stack([(R[..., 0, 2] - R[..., 2, 0]) / s, (R[..., 0, 1] + R[..., 1, 0]) / s, 0.25 * s, (R[..., 1, 2] + R[..., 2, 1]) / s], -1)[c2]
    s = np.sqrt(np.maximum(1 + m22 - m00 - m11, 1e-12)) * 2
    q[c3] = np.stack([(R[..., 1, 0] - R[..., 0, 1]) / s, (R[..., 0, 2] + R[..., 2, 0]) / s, (R[..., 1, 2] + R[..., 2, 1]) / s, 0.25 * s], -1)[c3]
    q /= np.linalg.norm(q, axis=-1, keepdims=True)
    q[q[..., 0] < 0] *= -1
    return q


def compress_v(L):
    """(N, 4, 4) -> (N, 14) bytes"""
    q = mat_to_quat_v(L[:, :3, :3])
    a = np.clip(np.round((q + 2.0) * 16383.0), 0, 65535)
    t = np.clip(np.round((L[:, :3, 3] + 512.0) * 64.0), 0, 65535)
    return np.concatenate([a, t], 1).astype('<u2')


def orthonorm(M):
    U, _, Vt = np.linalg.svd(M)
    R = U @ Vt
    if np.linalg.det(R) < 0:
        U[:, -1] *= -1; R = U @ Vt
    return R


def axis_rot(ax, th):
    """(F,3,3) rotations about a fixed unit axis by angles th (F,)"""
    K = np.array([[0, -ax[2], ax[1]], [ax[2], 0, -ax[0]], [-ax[1], ax[0], 0]])
    th = np.asarray(th, float)
    return np.eye(3)[None] + np.sin(th)[:, None, None] * K[None] + (1 - np.cos(th))[:, None, None] * (K @ K)[None]


# ---------------------------------------------------------------- skeleton
# saber grip, degrees of flexion per finger joint (knuckle, middle, tip)
GRIP = (68, 85, 45)
GRIP_THUMB = (30, 40, 30)
# Force lightning: the open, spread "claw" (index nearly straight, the others curling a little more each)
LIGHT = {1: (0, 5, 5), 2: (5, 8, 5), 3: (14, 20, 10), 4: (22, 28, 14), 5: (30, 34, 16)}
SPREAD = {1: 0, 2: 8, 3: 0, 4: 8, 5: 16}      # degrees each finger fans out from the middle finger


class NewSkeleton:
    def __init__(self, src, md, m, mp, fit, keep_extras=False, fingers=False, extra_re=None):
        self.src, self.md, self.m, self.fit = src, md, m, mp and fit
        B = md.bones
        self.bsc = [float(np.cbrt(abs(np.linalg.det(b['base'][:3, :3])))) for b in B]
        mdR = [b['base'][:3, :3] / s for b, s in zip(B, self.bsc)]
        mdP = [b['base'][:3, 3] for b in B]
        self.mdR, self.mdP = mdR, mdP
        names, parents, R, P = [], [], [], []
        inv = {n: b for b, n in m.items()}
        # 1. the Movie Duels bones, same order
        for i, b in enumerate(B):
            nm = b['name']; names.append(nm); parents.append(b['parent'])
            if nm in m:
                R.append(fit.D[nm].T @ mdR[i]); P.append(fit.q[m[nm]])
            else:
                R.append(None); P.append(None)
        # unmapped _humanoid bones: carried by the nearest mapped ancestor's md->model transform
        for i in md.order:
            if R[i] is not None: continue
            a = B[i]['parent']
            while a >= 0 and B[a]['name'] not in m:
                a = B[a]['parent']
            if a < 0:
                R[i] = mdR[i].copy(); P[i] = mdP[i].copy()
            else:
                an = B[a]['name']; Dt = fit.D[an].T
                R[i] = Dt @ mdR[i]; P[i] = fit.q[m[an]] + Dt @ (mdP[i] - mdP[a])
        self.nmd = len(B)
        # 2. the model's other joints
        index = {}
        for k, b in enumerate(B):
            if b['name'] in m: index[m[b['name']]] = k
        extras = [n for n in src.joints if n not in index] if keep_extras else []
        self.curl = {}
        finger_plan = self.plan_fingers(src, md, m, mp, fit, index) if fingers else {}
        if fingers:
            extras = [n for n in finger_plan]
        # named extra bones (e.g. eyeballs): rigid children of their mapped parent, with that
        # parent's base orientation, so the game can turn them with the parent's angle convention
        same_as_parent = set()
        if extra_re:
            for n in src.joints:
                if n not in index and n not in extras and re.search(extra_re, src.names[n]):
                    extras.append(n)
                    if re.search(r'^eyeball_', src.names[n]): same_as_parent.add(n)
        # parents first
        def depth(n):
            d = 0
            while mp.par[n] is not None: n = mp.par[n]; d += 1
            return d
        extras.sort(key=depth)
        for n in extras:
            p = mp.par[n]
            while p is not None and p not in index:
                p = mp.par[p]
            k = len(names)
            nm = re.sub(r'[^A-Za-z0-9_]', '_', src.names[n])[:60]
            while nm.lower() in {x.lower() for x in names}: nm += '_'
            names.append(nm)
            parents.append(index[p] if p is not None else md.byName['pelvis'])
            R.append(R[parents[-1]].copy() if n in same_as_parent else fit.qr[n]); P.append(fit.q[n])
            index[n] = k
        for n, plan in finger_plan.items():
            if plan is None: continue
            mdb, gain, axis_world, rest, grip, light, sax, spread = plan
            Rb = orthonorm(fit.qr[n])
            self.curl[index[n]] = (mdb, gain, Rb.T @ axis_world, rest, grip, light, Rb.T @ sax, spread, self._fk[n][0])
        if self.curl:
            I = np.eye(4)[None, None].repeat(len(B), 1)
            G0 = I @ np.array([b['base'] for b in B])[None]
            self.fang0 = self.finger_flex(G0[..., :3, :3] / np.array(self.bsc)[None, :, None, None], G0[..., :3, 3])
        if len(names) > 256:
            raise SystemExit('skeleton would have %d bones (engine limit 256)' % len(names))
        self.names, self.parents = names, parents
        self.base = []
        for r, p in zip(R, P):
            M = np.eye(4); M[:3, :3] = orthonorm(r); M[:3, 3] = p; self.base.append(M)
        self.base = np.array(self.base)
        self.index = index                # source joint node -> new bone index
        self.order = []
        seen = set()
        def visit(i):
            if i in seen: return
            if parents[i] >= 0: visit(parents[i])
            seen.add(i); self.order.append(i)
        for i in range(len(names)): visit(i)
        # leg length ratio for the hips height
        def leg(P_, l, t, a):
            return np.linalg.norm(P_[l] - P_[t]) + np.linalg.norm(P_[t] - P_[a])
        bi = md.byName
        new_leg = 0.5 * (leg(self.base[:, :3, 3], bi['lfemuryz'], bi['ltibia'], bi['ltalus']) + leg(self.base[:, :3, 3], bi['rfemuryz'], bi['rtibia'], bi['rtalus']))
        md_leg = 0.5 * (leg(np.array(mdP), bi['lfemuryz'], bi['ltibia'], bi['ltalus']) + leg(np.array(mdP), bi['rfemuryz'], bi['rtibia'], bi['rtalus']))
        self.hip_k = new_leg / md_leg

    # ------------------------------------------------------------ fingers
    @staticmethod
    def bend_axis(hand, index1, pinky1, thumb1, joint, nxt):
        """unit axis about which a positive rotation curls the segment joint->nxt towards the palm"""
        n = np.cross(index1 - hand, pinky1 - hand); n /= np.linalg.norm(n)
        if np.dot(thumb1 - hand, n) < 0: n = -n
        d = nxt - joint; d /= max(np.linalg.norm(d), 1e-9)
        ax = np.cross(d, n); ax /= max(np.linalg.norm(ax), 1e-9)
        if np.dot(np.cross(ax, d), n) < 0: ax = -ax
        return ax

    def plan_fingers(self, src, md, m, mp, fit, index):
        """model finger joints to add as bones: {node: (md bone, gain, bend axis) or None (rigid)}"""
        from bonemap import finger_of, TWIST
        weighted = set()
        for prim in src.meshes():
            for J, W in zip(prim['J'], prim['W']):
                for j, w in zip(J, W):
                    if w > 1e-4: weighted.add(src.joints[int(j)])
        plan = {}
        bi = md.byName
        self.md_axis = {}
        self._fk = {}
        for side in 'lr':
            hn = m.get(side + 'hand')
            if hn is None: continue
            q = fit.q
            chains = {}
            for c in mp.kids[hn]:
                f = finger_of(src.names[c])
                if f is None: continue
                ch = [x for x in mp.chain(c) if not re.search(r'(_end$|end$)', src.names[x], re.I)]
                chains[f] = ch
            if not all(k in chains for k in (1, 2, 5 if 5 in chains else 4)): continue
            def first_curl(ch):
                return ch[1:] if len(ch) >= 4 and src.names[ch[0]].rstrip('_').endswith('0') else ch
            idx1 = first_curl(chains[2])[0]; pk1 = first_curl(chains.get(5, chains.get(4)))[0]; th1 = first_curl(chains[1])[0]
            src_for = {1: 'd1', 2: 'd2', 3: 'd2', 4: 'd4', 5: 'd4'}
            # Movie Duels finger bones' own bend axes (base pose, world)
            P = self.mdP
            mh = bi[side + 'hand']
            for d_ in ('d1', 'd2', 'd4'):
                j1 = bi.get('%s_%s_j1' % (side, d_)); j2 = bi.get('%s_%s_j2' % (side, d_))
                if j1 is None or j2 is None: continue
                ax = self.bend_axis(P[mh], P[bi['%s_d2_j1' % side]], P[bi['%s_d4_j1' % side]], P[bi['%s_d1_j1' % side]], P[j1], P[j2])
                self.md_axis[j1] = self.mdR[j1].T @ ax
                self.md_axis[j2] = self.mdR[j2].T @ ax
            for f, ch in chains.items():
                curl = first_curl(ch)
                for n in ch:
                    if n in weighted or any(x in weighted for x in ch[ch.index(n):]):
                        plan[n] = None
                pn = np.cross(q[idx1] - q[hn], q[pk1] - q[hn]); pn /= np.linalg.norm(pn)
                if np.dot(q[th1] - q[hn], pn) < 0: pn = -pn
                for k, n in enumerate(curl[:3]):
                    if n not in plan: continue
                    nxt = curl[k + 1] if k + 1 < len(curl) else None
                    tip = q[nxt] if nxt is not None else q[n] + (q[n] - q[curl[k - 1]])
                    prv = q[mp.par[n]] if mp.par[n] is not None else q[hn]
                    if f == 1:
                        # the thumb closes across the palm, towards the index finger's middle joint
                        ic = first_curl(chains[2]); tgt = q[ic[0]] - q[n]
                        nn = tgt - (tip - q[n]) * np.dot(tgt, tip - q[n]) / max(np.dot(tip - q[n], tip - q[n]), 1e-9)
                        nn /= max(np.linalg.norm(nn), 1e-9)
                    else:
                        nn = pn
                    d = (tip - q[n]) / max(np.linalg.norm(tip - q[n]), 1e-9)
                    ax = np.cross(d, nn); ax /= max(np.linalg.norm(ax), 1e-9)
                    rest = self.flex(q[n] - prv, tip - q[n], nn)
                    mdb = bi['%s_%s_%s' % (side, src_for[f], 'j1' if k == 0 else 'j2')]
                    gain = 1.0 if k < 2 else 0.8
                    # Movie Duels hands are modelled holding the saber: its finger bones at their
                    # base pose mean "gripping". The model's fingers take a grip there, and the
                    # animation's finger movement away from base opens or closes them further.
                    grip = GRIP_THUMB[k] if f == 1 else GRIP[k]
                    sax = np.zeros(3)
                    if k == 0 and f != 1:
                        mid = first_curl(chains.get(3, chains[2]))[0]
                        lat = q[n] - q[mid]
                        sax = nn.copy()
                        if np.dot(np.cross(sax, d), lat) < 0: sax = -sax
                    plan[n] = (mdb, gain, ax, float(rest), np.radians(grip), np.radians(LIGHT[f][k]), sax, np.radians(SPREAD[f]))
                    self._fk[n] = (side, f, k)
        return plan

    @staticmethod
    def flex(a, b, n):
        """signed bend from direction a to direction b, positive towards the palm normal n (arrays broadcast)"""
        ax = np.cross(a, n)
        ax = ax / np.maximum(np.linalg.norm(ax, axis=-1, keepdims=True), 1e-9)
        return np.arctan2(np.sum(np.cross(a, b) * ax, axis=-1), np.sum(a * b, axis=-1))

    def finger_flex(self, Rmd, Pmd):
        """absolute flexion (F,) of each Movie Duels finger bone: j1 against the hand, j2 against j1"""
        bi = self.md.byName; out = {}
        for side in 'lr':
            try:
                h = bi[side + 'hand']; i1 = bi[side + '_d2_j1']; r1 = bi[side + '_d4_j1']; t1 = bi[side + '_d1_j1']
            except KeyError:
                continue
            n = np.cross(Pmd[:, i1] - Pmd[:, h], Pmd[:, r1] - Pmd[:, h])
            n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)
            n *= np.sign(np.sum((Pmd[:, t1] - Pmd[:, h]) * n, axis=1))[:, None]
            for d_ in ('d1', 'd2', 'd4'):
                j1 = bi.get('%s_%s_j1' % (side, d_)); j2 = bi.get('%s_%s_j2' % (side, d_))
                if j1 is None or j2 is None: continue
                seg = self.mdP[j2] - self.mdP[j1]
                s1 = Rmd[:, j1] @ (self.mdR[j1].T @ seg)
                s2 = Rmd[:, j2] @ (self.mdR[j2].T @ seg)
                out[j1] = self.flex(Pmd[:, j1] - Pmd[:, h], s1, n)
                out[j2] = self.flex(s1, s2, n)
        return out

    def finger_angles(self, Rmd):
        """signed curl angle (F,) of each Movie Duels finger bone, relative to its base pose"""
        out = {}
        for j, ax in self.md_axis.items():
            p = self.md.bones[j]['parent']
            L0 = self.mdR[p].T @ self.mdR[j]
            Lt = np.einsum('fji,fjk->fik', Rmd[:, p], Rmd[:, j])
            D = np.einsum('ji,fjk->fik', L0, Lt)
            q = mat_to_quat_v(D)                   # w >= 0
            v = q[:, 1:]; s = np.linalg.norm(v, axis=1)
            ang = 2 * np.arctan2(s, q[:, 0])
            axis = v / np.maximum(s, 1e-9)[:, None]
            out[j] = ang * (axis @ ax)
        return out

    # ------------------------------------------------------------ animation
    def fit_thumbs(self, frame, clearance=0.7, log=print):
        """close each thumb only as far as it stays clear of the curled fingers at a gripping frame"""
        fk = getattr(self, '_fk', {})
        for side in 'lr':
            thumb = [self.index[n] for n, (sd, f, k) in fk.items() if sd == side and f == 1]
            fingers = [self.index[n] for n, (sd, f, k) in fk.items() if sd == side and f >= 2]
            if not thumb or not fingers: continue
            orig = {b: self.curl[b] for b in thumb}
            for sc in (1.0, 0.85, 0.7, 0.55, 0.4, 0.25, 0.1):
                for b in thumb:
                    c = list(orig[b]); c[4] = c[3] + (orig[b][4] - orig[b][3]) * sc   # grip -> towards rest
                    self.curl[b] = tuple(c)
                _, G = self.convert(frame, frame + 1, want_G=True)
                P = G[0, :, :3, 3]
                # thumb points: joints and midpoints of its segments (plus a tip past the last joint)
                tp = [P[b] for b in thumb]
                last = sorted(thumb, key=lambda b: self.base[b][2, 3])[0]
                pts = tp + [(tp[i] + tp[j]) / 2 for i in range(len(tp)) for j in range(i + 1, len(tp))]
                fp = [P[b] for b in fingers]
                fp += [(P[b] + P[self.parents[b]]) / 2 for b in fingers if self.parents[b] in fingers]
                dmin = min(np.linalg.norm(a - c) for a in pts for c in fp)
                if dmin >= clearance: break
            log('  %s thumb closes to %d%% of the grip (clearance %.2f)' % (side, round(sc * 100), dmin))

    def fist_center(self, side, frame=None):
        """bind-space centre of the closed hand (where a saber hilt sits): mean of the knuckles
        and the last finger joints of index..pinky, at a frame where the hand grips"""
        fk = getattr(self, '_fk', {})
        bones = [self.index[n] for n, (sd, f, k) in fk.items() if sd == side and f >= 2 and k in (0, 2)]
        if len(bones) < 4: return None
        frame = 0 if frame is None else frame
        _, G = self.convert(frame, frame + 1, want_G=True)
        h = self.md.byName[side + 'hand']
        S = G[0, h] @ np.linalg.inv(self.base[h])
        c = np.mean([G[0, b][:3, 3] for b in bones], axis=0)
        return (np.linalg.inv(S) @ np.append(c, 1.0))[:3]

    def convert(self, f0, f1, want_G=False):
        md = self.md; nb = len(self.names); F = f1 - f0
        S = decode_frames(md, f0, f1)
        Gmd = S @ np.array([b['base'] for b in md.bones])[None]
        Rmd = Gmd[..., :3, :3] / np.array(self.bsc)[None, :, None, None]
        Pmd = Gmd[..., :3, 3]
        base = self.base; bR = base[:, :3, :3]; bP = base[:, :3, 3]
        R = np.zeros((F, nb, 3, 3)); P = np.zeros((F, nb, 3))
        fang = self.finger_flex(Rmd, Pmd) if self.curl else {}
        # lightning hand pose: inside the lightning animations, for a hand the animation opens
        lw = {}
        lf = getattr(self, 'lightning_frames', None)
        if self.curl and lf is not None:
            inl = lf[f0:f1].astype(float)
            for side in 'lr':
                j = md.byName.get(side + '_d2_j1')
                if j is None or j not in fang: continue
                lw[side] = inl * np.clip((self.fang0[j] - fang[j]) / np.radians(20), 0, 1)
        # the free (left) hand holds a hilt too in two-handed grips (hands together, e.g. the strong
        # style), and in dual / staff animations: there it closes all the way like the saber hand
        lgrip = None
        if self.curl and 'l' in getattr(self, 'relax', {}):
            bi_ = md.byName
            d = np.linalg.norm(Pmd[:, bi_['lhand']] - Pmd[:, bi_['rhand']], axis=1)
            together = np.clip((14.0 - d) / 6.0, 0, 1)            # hands within ~8 units: one grip
            hf = getattr(self, 'hold_frames', None)
            held = hf[f0:f1].astype(float) if hf is not None else 0.0
            lgrip = np.maximum(together, held)
        pel = md.byName['pelvis']; root = md.byName['model_root']
        for i in self.order:
            par = self.parents[i]
            if i < self.nmd:
                R[:, i] = Rmd[:, i]
                ck = getattr(self, 'clavicle_k', 1.0)
                if ck != 1.0 and md.bones[i]['name'].lower() in ('lclavical', 'rclavical') and par >= 0:
                    # damp the shrug: only part of the clavicle's turn away from its base pose (relative
                    # to the chest). BF2-style shoulder armour is built for clavicles that barely move.
                    L0 = self.mdR[par].T @ self.mdR[i]
                    L = np.einsum('fji,fjk->fik', Rmd[:, par], Rmd[:, i])
                    D = np.einsum('ji,fjk->fik', L0, L)
                    q = mat_to_quat_v(D)
                    ang = 2 * np.arctan2(np.linalg.norm(q[:, 1:], axis=1), q[:, 0])
                    axis = q[:, 1:] / np.maximum(np.linalg.norm(q[:, 1:], axis=1), 1e-9)[:, None]
                    h = 0.5 * ang * ck
                    qk = np.concatenate([np.cos(h)[:, None], axis * np.sin(h)[:, None]], 1)
                    R[:, i] = Rmd[:, par] @ L0[None] @ quat_to_mat_v(qk)
            else:
                R[:, i] = R[:, par] @ (bR[par].T @ bR[i])[None]
                if i in self.curl:
                    mdb, gain, ax, rest, grip, light, sax, spread, side = self.curl[i]
                    # the free (left) hand rests half open, as Movie Duels' own hands are modelled; it closes
                    # into a fist as the animation curls its fingers (melee, two-handed grips)
                    rx = getattr(self, 'relax', {})
                    gr = grip * rx.get(side, 1.0)
                    gk = 1.6 if side == 'l' and side in rx else 1.0
                    if side == 'l' and lgrip is not None:
                        # holding: the saber hand's grip and gain, blended in as the hands come together
                        gr = gr + (grip * rx.get('r', 1.0) - gr) * lgrip
                        gk = gk + (1.0 - gk) * lgrip
                    th = np.clip(gr + gain * gk * (fang[mdb] - self.fang0[mdb]), np.radians(-15), np.radians(105))
                    w = lw.get(side)
                    if w is not None:
                        th = (1 - w) * th + w * light
                    R[:, i] = R[:, i] @ axis_rot(ax, th - rest)
                    if w is not None and spread:
                        R[:, i] = R[:, i] @ axis_rot(sax, w * spread)
            if i == root or par < 0:
                P[:, i] = bP[i]
                if i < self.nmd: R[:, i] = bR[i]
            elif i == pel:
                P[:, i] = bP[i] + self.hip_k * (Pmd[:, i] - self.mdP[i])
                P[:, i, 2] -= getattr(self, 'ground_drop', 0.0)   # soles on the ground the game expects
            else:
                off = bR[par].T @ (bP[i] - bP[par])
                P[:, i] = P[:, par] + np.einsum('fij,j->fi', R[:, par], off)
        if getattr(self, 'arm_ik', False):
            self._arm_ik(R, P, Pmd)
        G = np.zeros((F, nb, 4, 4)); G[..., :3, :3] = R; G[..., :3, 3] = P; G[..., 3, 3] = 1
        if want_G: return None, G
        Sn = G @ np.linalg.inv(base)[None]
        Ln = np.empty_like(Sn)
        for i in range(nb):
            par = self.parents[i]
            Ln[:, i] = Sn[:, i] if par < 0 else np.linalg.inv(Sn[:, par]) @ Sn[:, i]
        return Ln


    # ------------------------------------------------------------ arm IK
    def _arm_ik(self, R, P, Pmd):
        """Each hand goes where the Movie Duels animation puts it, measured from the midpoint between the
        shoulders (so two-handed grips meet, hands reach the hilt, the hips, the other arm as animated),
        and the shoulder and elbow bend to reach it. The model's own arm lengths stay; the hand keeps
        the animation's rotation. Everything below the upper arm (forearm twist, hand, fingers) follows."""
        md = self.md; bi = md.byName
        hl, hr = bi['lhumerus'], bi['rhumerus']
        mid_m = 0.5 * (P[:, hl] + P[:, hr]); mid_a = 0.5 * (Pmd[:, hl] + Pmd[:, hr])
        def rot_between(u, v):
            u = u / np.linalg.norm(u, axis=1, keepdims=True); v = v / np.linalg.norm(v, axis=1, keepdims=True)
            ax = np.cross(u, v); sn = np.linalg.norm(ax, axis=1); cs = np.einsum('fi,fi->f', u, v)
            k = ax / np.maximum(sn, 1e-12)[:, None]
            K = np.zeros((len(u), 3, 3)); K[:, 0, 1] = -k[:, 2]; K[:, 0, 2] = k[:, 1]; K[:, 1, 0] = k[:, 2]; K[:, 1, 2] = -k[:, 0]; K[:, 2, 0] = -k[:, 1]; K[:, 2, 1] = k[:, 0]
            return np.eye(3)[None] + sn[:, None, None] * K + (1 - cs)[:, None, None] * (K @ K)
        # children lists for re-posing what hangs below the upper arm
        kids = {}
        for i, p in enumerate(self.parents): kids.setdefault(p, []).append(i)
        def subtree(i):
            out = []; st = [i]
            while st:
                j = st.pop(); out.append(j); st.extend(kids.get(j, []))
            return out
        # Hands apart, each arm keeps the animation's own extension (shoulder-to-hand distance as a share
        # of the arm's length), so elbows bend exactly as animated whatever the model's arm length.
        # (Taking the hand offset in absolute units straightened short-armed models' elbows to ~0 deg:
        # stiff arms at idle.) Hands together (two-handed grips), both go to the same point measured
        # from between the shoulders, scaled by arm length, so the grip still meets.
        def reach(Pm, sd):
            return (np.linalg.norm(Pm[:, bi[sd + 'radius']] - Pm[:, bi[sd + 'humerus']], axis=1) +
                    np.linalg.norm(Pm[:, bi[sd + 'hand']] - Pm[:, bi[sd + 'radius']], axis=1))
        k_arm = 0.5 * (reach(P, 'l') / reach(Pmd, 'l') + reach(P, 'r') / reach(Pmd, 'r'))
        dh = np.linalg.norm(Pmd[:, bi['lhand']] - Pmd[:, bi['rhand']], axis=1)
        together = np.clip((14.0 - dh) / 6.0, 0, 1)[:, None]
        for side in 'lr':
            h, r, w = bi[side + 'humerus'], bi[side + 'radius'], bi[side + 'hand']
            S = P[:, h]; E = P[:, r]; W = P[:, w]
            a = np.linalg.norm(E - S, axis=1); b = np.linalg.norm(W - E, axis=1)
            vmd = Pmd[:, w] - Pmd[:, h]; lmd = np.linalg.norm(vmd, axis=1)
            frac = lmd / np.maximum(reach(Pmd, side), 1e-6)
            T_one = S + vmd / np.maximum(lmd, 1e-9)[:, None] * ((a + b) * frac)[:, None]
            T_two = mid_m + (Pmd[:, w] - mid_a) * k_arm[:, None]
            T = T_one + (T_two - T_one) * together
            d = T - S; dl = np.linalg.norm(d, axis=1)
            dl2 = np.clip(dl, np.abs(a - b) + 1e-3, a + b - 1e-3)
            dn = d / np.maximum(dl, 1e-9)[:, None]
            T2 = S + dn * dl2[:, None]
            # elbow in the plane of the current (animated) elbow
            pole = E - S; pole = pole - np.einsum('fi,fi->f', pole, dn)[:, None] * dn
            pn = pole / np.maximum(np.linalg.norm(pole, axis=1), 1e-9)[:, None]
            ca = np.clip((a * a + dl2 * dl2 - b * b) / (2 * a * dl2), -1, 1)
            E2 = S + dn * (a * ca)[:, None] + pn * (a * np.sqrt(1 - ca * ca))[:, None]
            Q1 = rot_between(E - S, E2 - S)
            Q2 = rot_between(np.einsum('fij,fj->fi', Q1, W - E), T2 - E2)
            # upper arm subtree turns by Q1 about the shoulder; the forearm subtree then by Q2 about the elbow,
            # except the hand (and its fingers), which keep the animation's world rotation
            up = subtree(h); fore = set(subtree(r)); hand = set(subtree(w))
            # the saber tag bone hangs off the forearm but carries the hand's hold: keep it with the hand
            tb = bi.get(side + 'hang_tag_bone')
            if tb is not None and tb in fore: hand |= set(subtree(tb))
            for j in up:
                P[:, j] = S + np.einsum('fij,fj->fi', Q1, P[:, j] - S)
                if j not in hand: R[:, j] = Q1 @ R[:, j]
            for j in fore:
                P[:, j] = E2 + np.einsum('fij,fj->fi', Q2, P[:, j] - E2)
                if j not in hand: R[:, j] = Q2 @ R[:, j]
            # the hand keeps its animated rotation: move its subtree rigidly to the reached wrist
            # re-pose the hand subtree from the animation's rotations about the new wrist
            # (positions were rotated with the arm above; undo those rotations for the hand's children)
            Qt = Q2 @ Q1
            for j in hand:
                if j == w: continue
                P[:, j] = P[:, w] + np.einsum('fji,fj->fi', Qt, P[:, j] - P[:, w])

    def write_gla(self, path, gla_name, chunk=2048, log=print):
        md = self.md; nb = len(self.names); NF = md.numFrames
        pool = {}; plist = []
        idx = np.empty((NF, nb), np.int64)
        for f0 in range(0, NF, chunk):
            f1 = min(NF, f0 + chunk)
            L = self.convert(f0, f1).reshape(-1, 4, 4)
            C = compress_v(L)
            keys = C.view(np.dtype((np.void, 14))).ravel()
            u, inv = np.unique(keys, return_inverse=True)
            ids = np.empty(len(u), np.int64)
            for k, key in enumerate(u):
                kb = key.tobytes()
                j = pool.get(kb)
                if j is None:
                    j = len(plist); pool[kb] = j; plist.append(kb)
                ids[k] = j
            idx[f0:f1] = ids[inv].reshape(f1 - f0, nb)
            log('  frames %d-%d of %d, %d unique bones so far' % (f0, f1, NF, len(plist)))
        if len(plist) >= 1 << 24:
            raise SystemExit('too many unique bone poses for the .gla format')
        # header + skeleton
        skel = bytearray(); offs = []
        children = [[] for _ in range(nb)]
        for i, p in enumerate(self.parents):
            if p >= 0: children[p].append(i)
        for i in range(nb):
            offs.append(nb * 4 + len(skel))
            M = self.base[i]; Minv = np.linalg.inv(M)
            skel += struct.pack('<64sIi', self.names[i].encode()[:63], 0, self.parents[i])
            skel += struct.pack('<12f', *M[:3, :4].ravel()) + struct.pack('<12f', *Minv[:3, :4].ravel())
            skel += struct.pack('<i', len(children[i])) + struct.pack('<%di' % len(children[i]), *children[i])
        ofsSkel = 100 + nb * 4        # first skeleton entry, after the offset table (as the stock files)
        ofsFrames = 100 + nb * 4 + len(skel)
        fr = np.empty((NF, nb, 3), np.uint8)
        fr[..., 0] = idx & 255; fr[..., 1] = (idx >> 8) & 255; fr[..., 2] = (idx >> 16) & 255
        frb = fr.tobytes()
        pad = (-(ofsFrames + len(frb))) % 4
        ofsPool = ofsFrames + len(frb) + pad
        ofsEnd = ofsPool + 14 * len(plist)
        hdr = struct.pack('<4si64sf6i', b'2LGA', 6, gla_name.encode()[:63], 1.0, NF, ofsFrames, nb, ofsPool, ofsSkel, ofsEnd)
        assert len(hdr) == 100
        with open(path, 'wb') as f:
            f.write(hdr); f.write(struct.pack('<%di' % nb, *offs)); f.write(bytes(skel)); f.write(frb); f.write(b'\0' * pad)
            f.write(b''.join(plist))
        return ofsEnd
