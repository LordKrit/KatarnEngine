"""Jedi Academy dismemberment: clean body-part seams and the caps that close them.

JKA cuts a character at fixed seams (neck, shoulders, wrists, waist, hips). Each
body part is its own surface tree, and every cut has two "caps", hidden surfaces
named <part>_cap_<other>_off that the game turns on to close the hole: one on
the body that keeps the part, one on the piece that flies off.

  smooth_parts(groups)    tidies the part each triangle belongs to (no jagged,
                          single-triangle islands along a seam)
  make_caps(groups)       finds every seam opening and fans a cap across it
"""
import numpy as np

PAIRS = {('head', 'torso'), ('torso', 'l_arm'), ('torso', 'r_arm'), ('l_arm', 'l_hand'),
         ('r_arm', 'r_hand'), ('torso', 'hips'), ('hips', 'l_leg'), ('hips', 'r_leg')}
NEIGHBOURS = {}
for a, b in PAIRS:
    NEIGHBOURS.setdefault(a, set()).add(b)
    NEIGHBOURS.setdefault(b, set()).add(a)
CAP_SHADER = 'models/players/stormtrooper/caps'


def pkey(v):
    return tuple(np.round(v['p'], 3))


def ekey(a, b):
    return (a, b) if a <= b else (b, a)


import re
SKIP = re.compile(r'(card|lash|eye|teeth|tongue)', re.I)   # see-through cards and inner bits are no openings


def _tri_list(groups, skip=True):
    """[(part, matkey, tri)] for the body parts (cloth stays out of it)"""
    return [(part, mk, tri) for (part, mk), tris in groups.items() if part in NEIGHBOURS and not (skip and SKIP.search(str(mk)))
            for tri in tris]


def smooth_parts(groups, passes=2):
    """A triangle whose neighbours mostly belong to a part next to its own moves
    to that part. Returns new groups."""
    tl = _tri_list(groups, skip=False)
    edges = {}
    for i, (_, _, t) in enumerate(tl):
        k = [pkey(v) for v in t]
        for a, b in ((0, 1), (1, 2), (2, 0)):
            edges.setdefault(ekey(k[a], k[b]), []).append(i)
    nbr = [[] for _ in tl]
    for lst in edges.values():
        for i in lst:
            nbr[i].extend(j for j in lst if j != i)
    part = [p for p, _, _ in tl]
    for _ in range(passes):
        new = part[:]
        for i in range(len(tl)):
            cnt = {}
            for j in nbr[i]:
                cnt[part[j]] = cnt.get(part[j], 0) + 1
            if not cnt: continue
            best = max(cnt, key=cnt.get)
            if best != part[i] and best in NEIGHBOURS.get(part[i], ()) and cnt[best] * 2 > len(nbr[i]):
                new[i] = best
        part = new
    out = {k: v for k, v in groups.items() if k[0] not in NEIGHBOURS}
    for (p, mk, t), np_ in zip(tl, part):
        out.setdefault((np_, mk), []).append(t)
    return out


def _loops(directed):
    """chain directed boundary edges (key a -> key b, with vertex dicts) into closed loops"""
    nxt = {}
    for a, b, va, vb in directed:
        nxt.setdefault(a, []).append((b, va, vb))
    loops = []
    used = set()
    for a0 in list(nxt):
        for idx0 in range(len(nxt[a0])):
            if (a0, idx0) in used: continue
            loop = []; a = a0; idx = idx0
            while (a, idx) not in used:
                used.add((a, idx))
                b, va, vb = nxt[a][idx]
                loop.append(va)
                if b not in nxt: break
                cand = [i for i in range(len(nxt[b])) if (b, i) not in used]
                if not cand: break
                a, idx = b, cand[0]
            if len(loop) >= 3:
                loops.append(loop)
    return loops


def _merge_weights(vs):
    acc = {}
    for v in vs:
        for b, w in v['w']:
            acc[b] = acc.get(b, 0.0) + w
    top = sorted(acc.items(), key=lambda kv: -kv[1])[:4]
    tot = sum(w for _, w in top) or 1.0
    return [(b, w / tot) for b, w in top]


def make_caps(groups, near=3.0):
    """{cap surface name: dict(part, verts, tris)} for every seam opening"""
    tl = _tri_list(groups)
    owner = {}
    for i, (p, _, t) in enumerate(tl):
        k = [pkey(v) for v in t]
        for a, b in ((0, 1), (1, 2), (2, 0)):
            owner.setdefault(ekey(k[a], k[b]), set()).add(p)
    pts = {}
    for p, _, t in tl:
        for v in t:
            pts.setdefault(p, []).append(v['p'])
    pts = {p: np.array(v) for p, v in pts.items()}
    from scipy.spatial import cKDTree
    trees = {p: cKDTree(v) for p, v in pts.items()}
    caps = {}
    for A in NEIGHBOURS:
        cnt = {}; directed = []
        for p, _, t in tl:
            if p != A: continue
            k = [pkey(v) for v in t]
            for a, b in ((0, 1), (1, 2), (2, 0)):
                cnt[ekey(k[a], k[b])] = cnt.get(ekey(k[a], k[b]), 0) + 1
        for p, _, t in tl:
            if p != A: continue
            k = [pkey(v) for v in t]
            for a, b in ((0, 1), (1, 2), (2, 0)):
                if cnt[ekey(k[a], k[b])] == 1:
                    directed.append((k[a], k[b], t[a], t[b]))
        for loop in _loops(directed):
            P = np.array([v['p'] for v in loop])
            # which part is across this opening: the part sharing its edges, else the nearest one
            votes = {}
            for i in range(len(loop)):
                for q in owner.get(ekey(pkey(loop[i]), pkey(loop[(i + 1) % len(loop)])), ()):
                    if q != A: votes[q] = votes.get(q, 0) + 1
            B = max(votes, key=votes.get) if votes else None
            if B is None:
                best = None
                for q in NEIGHBOURS[A]:
                    if q not in pts: continue
                    d = trees[q].query(P)[0].mean()
                    if d < near and (best is None or d < best[0]): best = (d, q)
                B = best[1] if best else None
            if B is None or B not in NEIGHBOURS[A]:
                continue
            c = P.mean(0)
            # cap normal: out of A, towards B
            n = np.zeros(3)
            for i in range(len(P)):
                n += np.cross(P[i] - c, P[(i + 1) % len(P)] - c)
            if np.linalg.norm(n) < 1e-9: continue
            n /= np.linalg.norm(n)
            if np.dot(n, pts[B].mean(0) - c) < 0: n = -n
            # planar texture coordinates
            u = P[0] - c; u -= n * np.dot(u, n)
            if np.linalg.norm(u) < 1e-9: continue
            u /= np.linalg.norm(u); w_ = np.cross(n, u)
            R = max(np.max(np.abs((P - c) @ u)), np.max(np.abs((P - c) @ w_)), 1e-6)
            mk = lambda p, wts: dict(p=tuple(p), n=tuple(n), tc=(0.5 + 0.5 * np.dot(p - c, u) / R, 0.5 + 0.5 * np.dot(p - c, w_) / R), w=wts)
            cap = caps.setdefault('%s_cap_%s_off' % (A, B), dict(part=A, verts=[], tris=[]))
            base = len(cap['verts'])
            cap['verts'].append(mk(c, _merge_weights(loop)))
            for v in loop:
                cap['verts'].append(mk(np.array(v['p']), list(v['w'])))
            m = len(loop)
            for i in range(m):
                # the boundary edge runs v_i -> v_i+1 in A's winding; the cap triangle runs it
                # the other way, so the cap is wound like the surface around it
                cap['tris'].append((base, base + 1 + (i + 1) % m, base + 1 + i))
    # 32 bones per surface: drop the weakest bones of big caps
    for cap in caps.values():
        bones = {}
        for v in cap['verts']:
            for b, w in v['w']:
                bones[b] = bones.get(b, 0) + w
        if len(bones) > 32:
            keep = set(sorted(bones, key=lambda b: -bones[b])[:32])
            for v in cap['verts']:
                ws = [(b, w) for b, w in v['w'] if b in keep] or [(max(keep, key=lambda b: bones[b]), 1.0)]
                t = sum(w for _, w in ws); v['w'] = [(b, w / t) for b, w in ws]
    return caps
