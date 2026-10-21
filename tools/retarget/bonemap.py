"""Map a source skeleton (any naming) onto the JKA _humanoid bones.

Names are used when they say something (left/right, thumb, twist ...); the
structure is worked out from the tree and the bind pose, so Mixamo, Unreal,
Valve biped, Blender rigify-style and plain game-rip skeletons all map the
same way. Anything can be overridden with a JSON file {"jka bone": "source joint"}.
"""
import re, json
import numpy as np

TWIST = re.compile(r'(twist|roll|helper|_end$|end$|nub|tip$|ik_|_ik|ik$|pole|target|socket|attach|weapon|prop|dummy|null)', re.I)


def side_of(name):
    n = name.lower()
    n = re.sub(r'([a-z])([A-Z])', r'\1_\2', name).lower()
    toks = re.split(r'[^a-z0-9]+', n)
    joined = n.replace(':', '_')
    if 'left' in joined or 'l' in toks or re.search(r'(^|[^a-z])l[0-9_.]', joined) or joined.endswith('_l') or re.search(r'_l_', joined):
        return 'l'
    if 'right' in joined or 'r' in toks or re.search(r'(^|[^a-z])r[0-9_.]', joined) or joined.endswith('_r') or re.search(r'_r_', joined):
        return 'r'
    return None


def finger_of(name):
    n = name.lower()
    for key, f in (('thumb', 1), ('index', 2), ('middle', 3), ('ring', 4), ('pinky', 5), ('little', 5)):
        if key in n:
            return f
    m = re.search(r'finger\s*_?([0-4])', n)        # Valve Finger0..4 (0 = thumb)
    if m:
        return int(m.group(1)) + 1
    return None


class Mapper:
    def __init__(self, src):
        self.src = src
        J = src.joints
        self.jset = set(J)
        self.idx = {n: k for k, n in enumerate(J)}         # node -> joint slot
        self.pos = {n: src.bind[self.idx[n]][:3, 3] for n in J}
        self.kids = {n: [] for n in J}
        self.par = {}
        for n in J:
            p = src.parent[n]
            while p >= 0 and p not in self.jset:
                p = src.parent[p]
            self.par[n] = p if p >= 0 else None
            if p >= 0:
                self.kids[p].append(n)
        self.name = {n: src.names[n] for n in J}

    # --- tree helpers -------------------------------------------------
    def depth(self, n):
        return 1 + max([self.depth(c) for c in self.kids[n]] or [0])

    def size(self, n):
        return 1 + sum(self.size(c) for c in self.kids[n])

    def deep_kids(self, n, d=2):
        return [c for c in self.kids[n] if self.depth(c) >= d and not TWIST.search(self.name[c])]

    def chain(self, n):
        """main path from n: follow the deepest non-twist child"""
        out = [n]
        while True:
            ks = [c for c in self.kids[out[-1]] if not TWIST.search(self.name[c])] or self.kids[out[-1]]
            if not ks:
                return out
            out.append(max(ks, key=lambda c: (self.depth(c), self.size(c))))

    def skip_twist_up(self, n):
        while n is not None and TWIST.search(self.name[n]):
            n = self.par[n]
        return n

    # --- mapping ------------------------------------------------------
    def auto(self):
        roots = [n for n in self.src.joints if self.par[n] is None]
        # hips: first joint (breadth first) with 3 deep branches (2 legs + spine)
        hips = None
        queue = list(roots)
        while queue:
            n = queue.pop(0)
            if len(self.deep_kids(n, 2)) >= 3:
                hips = n; break
            queue += self.kids[n]
        if hips is None:
            raise SystemExit('could not find the hips (a joint with two legs and a spine below it)')
        br = sorted(self.deep_kids(hips, 2), key=self.size, reverse=True)
        spine0 = br[0]
        # chest: first joint up the spine with 3 deep branches (2 arms + neck)
        s = spine0; spine = [s]
        while len(self.deep_kids(s, 2)) < 3:
            ks = self.deep_kids(s, 2) or self.kids[s]
            if not ks:
                raise SystemExit('could not find the chest (a joint with two arms and a neck)')
            s = max(ks, key=self.size); spine.append(s)
        chest = s
        up = self.pos[chest] - self.pos[hips]; up /= np.linalg.norm(up)
        cb = self.deep_kids(chest, 2)
        neck = max(cb, key=lambda c: np.dot(self.pos[self.chain(c)[-1]] - self.pos[chest], up))
        arms = [c for c in cb if c != neck]
        arms = sorted(arms, key=self.size, reverse=True)[:2]
        legs = [c for c in br[1:]][:2]
        if len(legs) < 2 or len(arms) < 2:
            raise SystemExit('could not find two legs and two arms')

        m = {'pelvis': hips}
        # spine: hips child .. chest
        m['thoracic'] = chest
        sp = [x for x in spine if x != chest]
        if sp:
            m['lower_lumbar'] = sp[0]
            if len(sp) >= 2:
                m['upper_lumbar'] = sp[len(sp) // 2]
        nc = self.chain(neck)
        m['cervical'] = nc[0]
        m['cranium'] = nc[1] if len(nc) > 1 and not TWIST.search(self.name[nc[1]]) else nc[0]

        # forward: from the feet (heel -> toe) if possible, else chest facing
        def limb(start, is_arm):
            c = self.chain(start)
            if is_arm:
                hand = None
                for x in c:
                    if len([k for k in self.kids[x] if not TWIST.search(self.name[k])]) >= 2:
                        hand = x; break
                if hand is None:
                    hand = c[-1] if len(c) < 4 or not TWIST.search(self.name[c[-1]]) else c[-2]
                    hand = c[min(len(c) - 1, 3 if len(c) >= 4 else len(c) - 1)] if len(c) >= 4 else c[-1]
                fore = self.skip_twist_up(self.par[hand])
                upper = self.skip_twist_up(self.par[fore])
                clav = self.skip_twist_up(self.par[upper])
                out = dict(upper=upper, fore=fore, hand=hand)
                if clav is not None and clav != chest:
                    out['clav'] = clav
                return out
            c = [x for x in c if not TWIST.search(self.name[x])]
            out = dict(thigh=c[0])
            if len(c) > 1: out['calf'] = c[1]
            if len(c) > 2: out['foot'] = c[2]
            if len(c) > 3: out['toe'] = c[3]
            return out

        L = [limb(a, True) for a in arms]
        G = [limb(l, False) for l in legs]
        lat_arm = self.pos[L[0]['upper']] - self.pos[L[1]['upper']]
        # which is left? names first, then geometry (left = up x forward)
        def sides(parts, key):
            ss = [side_of(self.name[p[key]]) for p in parts]
            if ss[0] and ss[1] and ss[0] != ss[1]:
                return ss
            return None
        fwd = None
        for g in G:
            if 'toe' in g and 'foot' in g:
                f = self.pos[g['toe']] - self.pos[g['foot']]; f -= up * np.dot(f, up)
                fwd = f if fwd is None else fwd + f
        sa = sides(L, 'upper') or sides(L, 'hand')
        if sa is None:
            if fwd is None:
                raise SystemExit('cannot tell left from right: no L/R in the names and no toes. Use --map.')
            left = np.cross(up, fwd)
            sa = ['l', 'r'] if np.dot(lat_arm, left) > 0 else ['r', 'l']
        sg = sides(G, 'thigh')
        if sg is None:
            lat_leg = self.pos[G[0]['thigh']] - self.pos[G[1]['thigh']]
            sg = ['l', 'r'] if np.dot(lat_leg, lat_arm if sa[0] == 'l' else -lat_arm) > 0 else ['r', 'l']
        for part, sd in zip(L, sa):
            m[sd + 'humerus'] = part['upper']; m[sd + 'radius'] = part['fore']; m[sd + 'hand'] = part['hand']
            if 'clav' in part:
                m[sd + 'clavical'] = part['clav']
            self.fingers(part['hand'], sd, m)
        for part, sd in zip(G, sg):
            m[sd + 'femurYZ'] = part['thigh']
            if 'calf' in part: m[sd + 'tibia'] = part['calf']
            if 'foot' in part: m[sd + 'talus'] = part['foot']
            if 'toe' in part: self.toe = getattr(self, 'toe', {}); self.toe[sd] = part['toe']
        self.hips, self.chest, self.up = hips, chest, up
        return m

    def fingers(self, hand, sd, m):
        ks = [k for k in self.kids[hand] if not TWIST.search(self.name[k])]
        named = {}
        for k in ks:
            f = finger_of(self.name[k])
            if f: named[f] = k
        if not named and len(ks) >= 3:
            # unnamed: thumb = the root that sits furthest from the others
            P = np.array([self.pos[k] for k in ks]); cen = P.mean(0)
            th = ks[int(np.argmax(np.linalg.norm(P - cen, axis=1)))]
            rest = [k for k in ks if k != th]
            named[1] = th
            named[2] = rest[0]
            named[4] = rest[-1]
        for f, jk in ((1, 'd1'), (2, 'd2'), (4, 'd4')):
            k = named.get(f) or (named.get(5) if f == 4 else None) or (named.get(3) if f in (2, 4) else None)
            if k is None:
                continue
            c = self.chain(k)
            m['%s_%s_j1' % (sd, jk)] = c[0]
            if len(c) > 1 and not TWIST.search(self.name[c[1]]):
                m['%s_%s_j2' % (sd, jk)] = c[1]


def build_map(src, override=None):
    mp = Mapper(src)
    ov = (json.load(open(override)) if isinstance(override, str) else override) if override else None
    if ov and 'pelvis' in ov:
        m = {}                     # a full map: don't guess at all
    else:
        m = mp.auto()
    if ov:
        byname = {src.names[n].lower(): n for n in src.joints}
        for jb, sn in ov.items():
            if sn is None:
                m.pop(jb, None)
            elif sn.lower() in byname:
                m[jb] = byname[sn.lower()]
            else:
                raise SystemExit('--map: no joint named %s' % sn)
    return m, mp
