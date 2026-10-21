"""Close a hand's fingers around a saber grip (a cylinder fixed in the hand's frame) instead of
into a fist: each finger joint curls until the next joint (or the fingertip) touches the cylinder."""
import numpy as np, json

def hand_frame(sk, G, side):
    h = sk.md.byName[side + 'hand']
    F = np.eye(4); F[:3, :3] = G[h][:3, :3]; F[:3, 3] = G[h][:3, 3]
    return F

def wrap(sk, side, frame, axis_h, centre_h, radius, log=print):
    """axis_h, centre_h: the grip line in the hand bone's frame. Sets the finger grips (curl[4])."""
    fk = getattr(sk, '_fk', {})
    chains = {}
    for n, (sd, f, k) in fk.items():
        if sd == side and f >= 2: chains.setdefault(f, {})[k] = sk.index[n]
    def pose():
        _, G = sk.convert(frame, frame + 1, want_G=True); G = G[0]
        F = hand_frame(sk, G, side)
        c = F[:3, :3] @ centre_h + F[:3, 3]; d = F[:3, :3] @ axis_h
        return G, c, d
    def dist(p, c, d):
        v = p - c; return np.linalg.norm(v - np.dot(v, d) * d)
    for f, js in sorted(chains.items()):
        ks = sorted(js)
        for i, k in enumerate(ks):
            b = js[k]
            lo, hi = np.radians(-15), np.radians(105)
            def reach(th):
                c_ = list(sk.curl[b]); c_[4] = th; sk.curl[b] = tuple(c_)
                G, c, d = pose()
                P = G[:, :3, 3]
                if i + 1 < len(ks): q = P[js[ks[i + 1]]]
                else: q = P[b] + 0.8 * (P[b] - P[js[ks[i - 1]]])
                return dist(q, c, d)
            rng = [(10, 95), (15, 100), (5, 80)][min(i, 2)]
            cand = np.radians(np.arange(rng[0], rng[1] + 1, 5))
            errs = [abs(reach(t) - radius) for t in cand]
            th = cand[int(np.argmin(errs))]
            reach(th)
            log('  %s finger %d joint %d grips at %.0f deg' % (side, f, k, np.degrees(th)))
