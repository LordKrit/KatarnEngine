#!/usr/bin/env python3
"""jka_retarget - put high-poly models and animations from any skeleton onto the
Jedi Academy / Movie Duels _humanoid skeleton.

  model:  rigged character (glTF/GLB, FBX, SMD, DAE, ...) -> .glm on _humanoid,
          with skin, textures, NPC file and the bolt tags (sabers, holsters, ...)
  anim:   animations from the same kinds of files -> appended to a _humanoid .gla,
          plus animation.cfg lines that point Movie Duels' animations at them
  map:    print how the source skeleton was mapped (check this first)

Examples
  jka_retarget.py map   soldier.fbx --gla _humanoid.gla
  jka_retarget.py model soldier.fbx --gla _humanoid.gla --tags stock_model.glm --name soldier --out soldier.pk3
  jka_retarget.py anim  mocap.fbx   --gla _humanoid.gla --out _humanoid.gla.new \\
                        --take "Idle=BOTH_STAND1" --take "Run=BOTH_RUN1" --cfg animation_new.cfg
"""
import argparse, os, re, sys, zipfile, io, json
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from source import Source
from bonemap import build_map
from fit import Fit, orth
from g2 import GLA, comp, uncomp
import glmwrite

PART_OF = {
    'model_root': 'hips', 'pelvis': 'hips', 'Motion': 'hips', 'ltail': 'hips', 'rtail': 'hips',
    'lower_lumbar': 'torso', 'upper_lumbar': 'torso', 'thoracic': 'torso', 'lclavical': 'torso', 'rclavical': 'torso',
    'cervical': 'head', 'cranium': 'head',
    'lhumerus': 'l_arm', 'lhumerusX': 'l_arm', 'lradius': 'l_arm', 'lradiusX': 'l_arm',
    'rhumerus': 'r_arm', 'rhumerusX': 'r_arm', 'rradius': 'r_arm', 'rradiusX': 'r_arm',
    'lfemurYZ': 'l_leg', 'lfemurX': 'l_leg', 'ltibia': 'l_leg', 'ltalus': 'l_leg',
    'rfemurYZ': 'r_leg', 'rfemurX': 'r_leg', 'rtibia': 'r_leg', 'rtalus': 'r_leg',
}
PART_PARENT = {'hips': None, 'torso': 'hips', 'cape': 'torso', 'head': 'torso', 'l_arm': 'torso', 'r_arm': 'torso',
               'l_hand': 'l_arm', 'r_hand': 'r_arm', 'l_leg': 'hips', 'r_leg': 'hips'}


def part_of(bone):
    if bone in PART_OF: return PART_OF[bone]
    if bone.startswith('l_d') or bone in ('lhand', 'lhang_tag_bone'): return 'l_hand'
    if bone.startswith('r_d') or bone in ('rhand', 'rhang_tag_bone'): return 'r_hand'
    return 'head'          # face bones


def load(args):
    src = Source(args.src)
    gla = GLA(args.gla)
    m, mp = build_map(src, args.map)
    missing = [b for b in ('pelvis', 'thoracic', 'cranium', 'lhumerus', 'rhumerus', 'lradius', 'rradius', 'lhand', 'rhand',
                           'lfemurYZ', 'rfemurYZ', 'ltibia', 'rtibia') if b not in m]
    if missing:
        raise SystemExit('these _humanoid bones have no source joint: %s (use --map)' % ', '.join(missing))
    return src, gla, m, mp, Fit(src, gla, m, mp)


def cmd_map(args):
    src, gla, m, mp, fit = load(args)
    print('source: %s  (%d joints)   scale to JKA units: %.3f' % (args.src, len(src.joints), fit.s))
    for b in gla.bones:
        n = b['name']
        print('  %-16s <- %s' % (n, src.names[m[n]] if n in m else '(follows parent)'))
    unm = [src.names[n] for n in src.joints if n not in m.values()]
    print('source joints that move with their nearest mapped parent: %d' % len(unm))
    print('  ' + ', '.join(unm[:60]) + (' ...' if len(unm) > 60 else ''))


# ----------------------------------------------------------------------------- model
def cmd_model(args):
    src, gla, m, mp, fit = load(args)
    bidx = gla.byName
    name = args.name or re.sub(r'[^a-z0-9_]', '_', os.path.splitext(os.path.basename(args.src))[0].lower())
    xf = {}
    def xform(b):
        if b not in xf: xf[b] = fit.bone_xform(b)
        return xf[b]
    owner_bone = [fit.owner[n] for n in src.joints]          # per skin joint slot

    groups = {}             # (part, material) -> list of triangles of global vertex dicts
    mats = {}
    weld = {}
    cloth_re = re.compile(args.cloth_joints, re.I) if args.cloth_joints else None
    for prim in src.meshes():
        mi_key = prim['material']
        P = np.array([fit.conv_p(p) for p in prim['pos']])
        N = prim['nrm'] @ fit.R0.T
        verts = []
        for i in range(len(P)):
            acc = np.zeros(3); nacc = np.zeros(3); wsum = 0.0; wb = {}
            for j, w in zip(prim['J'][i], prim['W'][i]):
                if w <= 1e-5: continue
                b = owner_bone[int(j)]
                A, c, D = xform(b)
                acc += w * (A @ P[i] + c); nacc += w * (D @ N[i]); wsum += w
                gi = bidx[b.lower()]
                wb[gi] = wb.get(gi, 0.0) + w
            if wsum <= 0:
                A, c, D = xform('pelvis'); acc = A @ P[i] + c; nacc = D @ N[i]; wsum = 1.0; wb = {bidx['pelvis']: 1.0}
            p = acc / wsum
            n = nacc / max(np.linalg.norm(nacc), 1e-9)
            top = sorted(wb.items(), key=lambda kv: -kv[1])[:4]
            v = dict(p=tuple(p), n=tuple(n), tc=tuple(prim['uv'][i]), w=top)
            if cloth_re is not None:
                cw = sum(w for jj, w in zip(prim['J'][i], prim['W'][i]) if w > 1e-5 and cloth_re.search(src.names[src.joints[int(jj)]]))
                v['cloth'] = cw / max(1e-9, sum(prim['W'][i]))
            # weld duplicates (FBX/DAE via assimp arrive one vertex per triangle corner)
            key = (tuple(np.round(p, 4)), tuple(np.round(n, 3)), tuple(np.round(prim['uv'][i], 5)))
            verts.append(weld.setdefault((mi_key, key), v))
        mi = prim['material']
        if mi not in mats:
            mats[mi] = src.material_image(mi)
        for t in prim['tris']:
            a, b_, c = int(t[0]), int(t[1]), int(t[2])
            tot = {}
            for k in (a, b_, c):
                for gb, w in verts[k]['w']:
                    tot[gb] = tot.get(gb, 0) + w
            part = part_of(gla.bones[max(tot, key=tot.get)]['name'])
            if cloth_re is not None and sum(verts[k].get('cloth', 0) for k in (a, b_, c)) >= 1.5:
                part = 'cape'
            # glTF is counter-clockwise, Ghoul2 clockwise
            groups.setdefault((part, mi), []).append((verts[a], verts[c], verts[b_]))

    # material names / textures
    used = {}
    mat_name = {}
    for mi, info in mats.items():
        base = re.sub(r'[^a-z0-9_]', '_', (info[0] if info else 'mat%s' % mi).lower()) or 'mat'
        k = base; n = 2
        while k in used: k = '%s%d' % (base, n); n += 1
        used[k] = info; mat_name[mi] = k

    # split groups into surfaces: <= max verts, <= 32 bones
    surfaces = []           # (part, mat, verts, tris)
    for (part, mi), tris in sorted(groups.items(), key=lambda kv: (list(PART_PARENT).index(kv[0][0]), str(kv[0][1]))):
        cur = None
        for tri in tris:
            if cur is None:
                cur = dict(vmap={}, verts=[], tris=[], bones=set())
            newv = [v for v in tri if id(v) not in cur['vmap']]
            newb = {b for v in tri for b, _ in v['w']} - cur['bones']
            if len(cur['verts']) + len(newv) > args.max_verts or len(cur['tris']) >= getattr(args, 'max_tris', 1300) or len(cur['bones']) + len(newb) > 32:
                surfaces.append((part, mi, cur)); cur = dict(vmap={}, verts=[], tris=[], bones=set())
            idx = []
            for v in tri:
                if id(v) not in cur['vmap']:
                    cur['vmap'][id(v)] = len(cur['verts']); cur['verts'].append(v)
                    cur['bones'] |= {b for b, _ in v['w']}
                idx.append(cur['vmap'][id(v)])
            cur['tris'].append(tuple(idx))
        if cur and cur['tris']:
            surfaces.append((part, mi, cur))

    # hierarchy: canonical part surfaces first (empty placeholders when a part has no mesh)
    hier = []; lod = []; skin = []
    shader_of = lambda mi: 'models/players/%s/%s' % (name, mat_name[mi])
    first = {}
    for part in PART_PARENT:
        first[part] = len(hier)
        hier.append(dict(name=part, flags=0, shader='[nomaterial]', parent=None))
        lod.append(None)
    seen = {}
    for part, mi, cur in surfaces:
        k = (part, mi)
        if lod[first[part]] is None:
            si = first[part]; nm = part
            hier[si]['shader'] = shader_of(mi)
        else:
            seen[part] = seen.get(part, 0) + 1
            nm = '%s_%s%s' % (part, mat_name[mi], '' if seen[part] == 1 else '_%d' % seen[part])
            si = len(hier); hier.append(dict(name=nm, flags=0, shader=shader_of(mi), parent=first[part])); lod.append(None)
        lod[si] = dict(verts=cur['verts'], tris=cur['tris'])
        skin.append('%s,%s' % (nm, shader_of(mi)))
    for part, pp in PART_PARENT.items():
        hier[first[part]]['parent'] = first[pp] if pp else -1
    # empty canonical parts: one invisible degenerate triangle so the surface exists
    for part in PART_PARENT:
        si = first[part]
        if lod[si] is None:
            gb = bidx['pelvis']
            v = dict(p=tuple(gla.bones[gb]['base'][:3, 3]), n=(0, 0, 1), tc=(0, 0), w=[(gb, 1.0)])
            lod[si] = dict(verts=[v, v, v], tris=[(0, 1, 2)])
            hier[si]['flags'] = glmwrite.G2SURF_OFF
            skin.append('%s,*off' % part)
    # bolt tags from reference models (same skeleton): first file wins per tag
    have = set()
    for tf in args.tags or []:
        for tname, pname, raw in glmwrite.read_tags(tf):
            if tname in have: continue
            have.add(tname)
            pidx = next((i for i, h in enumerate(hier) if h['name'] == pname), first['hips'])
            hier.append(dict(name=tname, flags=glmwrite.G2SURF_TAG, shader='[nomaterial]', parent=pidx))
            lod.append(raw)
    for h in hier:
        if h['parent'] is None: h['parent'] = -1

    os.makedirs(os.path.dirname(os.path.abspath(args.out)) or '.', exist_ok=True)
    tmp = args.out + '.glm.tmp'
    glmwrite.write_glm(tmp, 'models/players/%s/model' % name, args.anim_name or gla.name, gla.numBones, hier, [lod])
    nverts = sum(len(s['verts']) for s in lod if isinstance(s, dict))
    ntris = sum(len(s['tris']) for s in lod if isinstance(s, dict))
    z = zipfile.ZipFile(args.out, 'w', zipfile.ZIP_DEFLATED)
    z.write(tmp, 'models/players/%s/model.glm' % name); os.remove(tmp)
    z.writestr('models/players/%s/model_default.skin' % name, '\n'.join(skin) + '\n')
    for k, info in used.items():
        if info and info[1]:
            z.writestr('models/players/%s/%s%s' % (name, k, info[2]), info[1])
    z.writestr('ext_data/npcs/%s.npc' % name,
               '%s\n{\n\tplayerModel\t%s\n\tcustomSkin\tdefault\n\tweapon\tWP_SABER\n\tsaber\tsingle_1\n\tplayerTeam\tTEAM_FREE\n\tenemyTeam\tTEAM_FREE\n\thealth\t100\n}\n' % (name, name))
    z.close()
    print('wrote %s: model "%s", %d surfaces (%d tags), %d verts, %d triangles, %d textures' %
          (args.out, name, len(hier), len(have), nverts, ntris, sum(1 for i in used.values() if i and i[1])))
    print('spawn in game:  npc spawn %s   (or set model %s)' % (name, name))


# ----------------------------------------------------------------------------- rig (per-model skeleton)
def relocate_tag(raw, sk, name=None, grip_frame=None):
    """move a tag surface from the Movie Duels skeleton onto the new one (rigid with its bone);
    the saber tags (*r_hand, *l_hand) go to the centre of the model's closed hand"""
    import struct
    b = bytearray(raw)
    nv, ofsV = struct.unpack_from('<ii', b, 12)
    nbr, ofsBR = struct.unpack_from('<ii', b, 28)
    refs = struct.unpack_from('<%di' % nbr, b, ofsBR)
    for k in range(nv):
        o = ofsV + k * 32
        n = np.array(struct.unpack_from('<3f', b, o)); p = np.array(struct.unpack_from('<3f', b, o + 12))
        u32 = struct.unpack_from('<I', b, o + 24)[0]
        bi = refs[u32 & 31]
        M = sk.base[bi]; R = M[:3, :3] @ sk.mdR[bi].T
        p2 = M[:3, 3] + R @ (p - sk.mdP[bi]); n2 = R @ n
        struct.pack_into('<3f', b, o, *n2); struct.pack_into('<3f', b, o + 12, *p2)
    side = {'*r_hand': 'r', '*l_hand': 'l'}.get(name)
    c = sk.fist_center(side, grip_frame) if side and hasattr(sk, 'fist_center') and not getattr(sk, 'stock_hand_tags', False) else None
    if c is not None and nv >= 3:
        o2 = ofsV + 2 * 32                       # vertex 2 is the tag's origin (MDX_TAG_ORIGIN)
        shift = c - np.array(struct.unpack_from('<3f', b, o2 + 12))
        print('  %s moved %.2f units into the closed hand' % (name, np.linalg.norm(shift)))
        for k in range(nv):
            o = ofsV + k * 32 + 12
            p = np.array(struct.unpack_from('<3f', b, o)) + shift
            struct.pack_into('<3f', b, o, *p)
        lift = getattr(sk, 'hilt_lift', 0.0)
        if lift:
            # stock models put the bolt past the fist toward the blade (JKA hilts have their origin near
            # the emitter end of the grip); move ours the same way so the hand holds the grip, not the tip
            V = [np.array(struct.unpack_from('<3f', b, ofsV + k * 32 + 12)) for k in range(3)]
            s0 = V[1] - V[0]; a1 = (V[0] - V[2]) / np.linalg.norm(V[0] - V[2])
            a0 = s0 - np.dot(s0, a1) * a1; a0 /= np.linalg.norm(a0)
            off = getattr(sk, 'hilt_off', {}).get(side)
            n3 = np.cross(a0, a1)
            mv = -a0 * lift if off is None else (-a0 * off[0] + a1 * off[1] + n3 * off[2])
            for k in range(nv):
                o = ofsV + k * 32 + 12
                struct.pack_into('<3f', b, o, *(np.array(struct.unpack_from('<3f', b, o)) + mv))
            print('  %s bolt moved %s' % (name, np.round(mv, 2)))
    if name and name.startswith('*head') and '_cap_' not in name and nv >= 3:
        # the stock head tags turn with the cranium's new base orientation, which tilts them;
        # on stock models they face level in the base pose, and the game measures the head's
        # facing (look-at, eye tracking) from them, so level them again about their origin
        V = [np.array(struct.unpack_from('<3f', b, ofsV + k * 32 + 12)) for k in range(3)]
        sides = [V[(j + 1) % 3] - V[j] for j in range(3)]
        a0 = sides[0] / np.linalg.norm(sides[0]); a1 = sides[2] / np.linalg.norm(sides[2])
        a0 = a0 - np.dot(a0, a1) * a1; a0 /= np.linalg.norm(a0)
        fwd = -a0                                 # the bolt's NEGATIVE_Y
        lvl = np.array([fwd[0], fwd[1], 0.0]); lvl /= np.linalg.norm(lvl)
        ax = np.cross(fwd, lvl); sn = np.linalg.norm(ax); cs = np.dot(fwd, lvl)
        if sn > 1e-6:
            ax /= sn; K = np.array([[0, -ax[2], ax[1]], [ax[2], 0, -ax[0]], [-ax[1], ax[0], 0]])
            Rl = np.eye(3) + sn * K + (1 - cs) * K @ K
            org = V[2]
            for k in range(nv):
                o = ofsV + k * 32
                n = np.array(struct.unpack_from('<3f', b, o)); p = np.array(struct.unpack_from('<3f', b, o + 12))
                struct.pack_into('<3f', b, o, *(Rl @ n)); struct.pack_into('<3f', b, o + 12, *(org + Rl @ (p - org)))
            print('  %s levelled by %.1f degrees' % (name, np.degrees(np.arctan2(sn, cs))))
    return bytes(b)


NPC_TEMPLATE = """%s
{
\tplayerModel\t%s
\tcustomSkin\tdefault
\tweapon\tWP_SABER
\tsaber\tdooku
\tsaberStyle\t1
\tFP_LEVITATION\t3
\tFP_PUSH\t3
\tFP_PULL\t3
\tFP_SPEED\t3
\tFP_LIGHTNING\t3
\tFP_SABER_OFFENSE\t3
\tFP_SABER_DEFENSE\t3
\tFP_SABERTHROW\t3
\trank\tcaptain
\treactions\t3
\taim\t5
\tmove\t5
\taggression\t5
\tevasion\t5
\tintelligence\t5
\thealth\t500
\tplayerTeam\tTEAM_FREE
\tenemyTeam\tTEAM_FREE
\tclass\tCLASS_REBORN
}
"""

def chunk_groups(args, groups):
    """(part, matkey) -> list of surfaces (dict verts, tris), split at the vertex / triangle / 32 bone limits"""
    out = {}
    for key, tris in groups.items():
        lst = []; cur = None
        for tri in tris:
            if cur is None: cur = dict(vmap={}, verts=[], tris=[], bones=set())
            newv = [v for v in tri if id(v) not in cur['vmap']]
            newb = {b for v in tri for b, _ in v['w']} - cur['bones']
            if len(cur['verts']) + len(newv) > args.max_verts or len(cur['tris']) >= getattr(args, 'max_tris', 1300) or len(cur['bones']) + len(newb) > 32:
                lst.append(cur); cur = dict(vmap={}, verts=[], tris=[], bones=set())
            idx = []
            for v in tri:
                if id(v) not in cur['vmap']:
                    cur['vmap'][id(v)] = len(cur['verts']); cur['verts'].append(v); cur['bones'] |= {b for b, _ in v['w']}
                idx.append(cur['vmap'][id(v)])
            cur['tris'].append(tuple(idx))
        if cur and cur['tris']: lst.append(cur)
        out[key] = lst
    return out


def finish_model(args, name, gla_path, sk, lod_groups, mats, tags, gla, skel):
    """lod_groups: one {(part, matkey): [triangles]} per detail level (0 = full)"""
    used = {}; mat_name = {}
    for mk, info in mats.items():
        base = re.sub(r'[^a-z0-9_]', '_', (info[0] if info else 'mat%s' % mk).lower()) or 'mat'
        k = base; n = 2
        while k in used: k = '%s%d' % (base, n); n += 1
        used[k] = info; mat_name[mk] = k
    import caps as capmod
    lod_groups = [capmod.smooth_parts(g) for g in lod_groups]
    lod_caps = [capmod.make_caps(g) for g in lod_groups]
    print('caps per detail level:', [len(c) for c in lod_caps], sorted(lod_caps[0]))
    chunks = [chunk_groups(args, g) for g in lod_groups]
    keys = sorted({k for c in chunks for k in c}, key=lambda kv: (list(PART_PARENT).index(kv[0]), str(kv[1])))
    nchunk = {k: max(len(c.get(k, [])) for c in chunks) for k in keys}
    hier = []; skin = []
    shader_of = lambda mk: 'models/players/%s/%s' % (name, mat_name[mk])
    first = {}
    for part in PART_PARENT:
        first[part] = len(hier); hier.append(dict(name=part, flags=0, shader='[nomaterial]', parent=None))
    slot = {}            # (part, matkey, chunk) -> hierarchy index
    taken = set(); seen = {}
    for (part, mk) in keys:
        for ci in range(nchunk[(part, mk)]):
            if first[part] not in taken:
                si = first[part]; nm = part; hier[si]['shader'] = shader_of(mk); taken.add(si)
            else:
                seen[part] = seen.get(part, 0) + 1
                nm = 'cape%d' % (seen[part] + 1) if part == 'cape' else '%s_%s%s' % (part, mat_name[mk], '' if seen[part] == 1 else '_%d' % seen[part])
                si = len(hier); hier.append(dict(name=nm, flags=0, shader=shader_of(mk), parent=first[part]))
            slot[(part, mk, ci)] = si
            skin.append('%s,%s' % (nm, shader_of(mk)))
    for part, pp in PART_PARENT.items():
        hier[first[part]]['parent'] = first[pp] if pp else -1
    pel = gla.byName['pelvis']
    dummy_v = dict(p=tuple(sk.base[pel][:3, 3]), n=(0, 0, 1), tc=(0, 0), w=[(pel, 1.0)])
    dummy = lambda: dict(verts=[dummy_v] * 3, tris=[(0, 1, 2)])
    for part in PART_PARENT:
        if first[part] not in taken:
            hier[first[part]]['flags'] = glmwrite.G2SURF_OFF; skin.append('%s,*off' % part)
    cap_slot = {}
    for cname in sorted({c for lc in lod_caps for c in lc}):
        part = next(c[cname]['part'] for c in lod_caps if cname in c)
        cap_slot[cname] = len(hier)
        hier.append(dict(name=cname, flags=glmwrite.G2SURF_OFF, shader=capmod.CAP_SHADER, parent=first[part]))
        skin.append('%s,%s' % (cname, capmod.CAP_SHADER))
    have = []
    for tname, pname, raw in tags:
        if tname in [h[0] for h in have]: continue
        pidx = next((i for i, h in enumerate(hier) if h['name'] == pname), first['hips'])
        hier.append(dict(name=tname, flags=glmwrite.G2SURF_TAG, shader='[nomaterial]', parent=pidx))
        have.append((tname, raw))
    nsurf_mesh = len(hier) - len(have)
    lods = []
    for c, lc in zip(chunks, lod_caps):
        lod = [None] * nsurf_mesh
        for (part, mk, ci), si in slot.items():
            lst = c.get((part, mk), [])
            lod[si] = dict(verts=lst[ci]['verts'], tris=lst[ci]['tris']) if ci < len(lst) else dummy()
        for cname, si in cap_slot.items():
            cp = lc.get(cname)
            lod[si] = dict(verts=cp['verts'], tris=cp['tris']) if cp else dummy()
        for i in range(nsurf_mesh):
            if lod[i] is None: lod[i] = dummy()
        lods.append(lod + [raw for _, raw in have])
    for h in hier:
        if h['parent'] is None: h['parent'] = -1
    tmp = args.out + '.tmp'
    glmwrite.write_glm(tmp + '.glm', 'models/players/%s/model' % name, gla_path, len(sk.names), hier, lods)
    sk.write_gla(tmp + '.gla', gla_path)
    z = zipfile.ZipFile(args.out, 'w', zipfile.ZIP_DEFLATED)
    z.write(tmp + '.glm', 'models/players/%s/model.glm' % name); os.remove(tmp + '.glm')
    z.write(tmp + '.gla', gla_path + '.gla'); os.remove(tmp + '.gla')
    gdir = os.path.dirname(args.gla)
    for f in ('animation.cfg', 'animevents.cfg'):
        if os.path.exists(os.path.join(gdir, f)):
            z.write(os.path.join(gdir, f), 'models/players/%s/%s' % (skel, f))
    z.writestr('models/players/%s/model_default.skin' % name, '\n'.join(skin) + '\n')
    z.writestr('ext_data/npcs/%s.npc' % name, NPC_TEMPLATE % (name, name))
    for k, info in used.items():
        if info and info[1]: z.writestr('models/players/%s/%s%s' % (name, k, info[2]), info[1])
    z.close()
    print('wrote %s: %d surfaces, %d tags, %d detail levels, verts per level %s' % (args.out, len(hier), len(have), len(lods),
          [sum(len(x['verts']) for x in l if isinstance(x, dict)) for l in lods]))


def collect_rig(args, src, fit, gla, newbone_of, owner_of, mats):
    """triangles of one source file grouped by (part, material name)"""
    cloth_re = re.compile(args.cloth_joints, re.I) if args.cloth_joints else None
    mesh_re = re.compile(args.cloth_mesh, re.I) if getattr(args, 'cloth_mesh', None) else None
    newbone = [newbone_of(src.names[n]) for n in src.joints]
    owner_bone = [owner_of(src.names[n]) for n in src.joints]
    groups = {}; weld = {}
    for prim in src.meshes():
        P = np.array([fit.conv_p(p) for p in prim['pos']]); N = prim['nrm'] @ fit.R0.T
        info = src.material_image(prim['material'])
        mk = info[0] if info else 'mat%s' % prim['material']
        if mk not in mats: mats[mk] = info
        verts = []
        for i in range(len(P)):
            wb = {}; own = {}
            for j, w in zip(prim['J'][i], prim['W'][i]):
                if w <= 1e-5: continue
                gi = newbone[int(j)]; wb[gi] = wb.get(gi, 0) + w
                own[owner_bone[int(j)]] = own.get(owner_bone[int(j)], 0) + w
            if not wb: wb = {gla.byName['pelvis']: 1.0}; own = {'pelvis': 1.0}
            n = N[i] / max(np.linalg.norm(N[i]), 1e-9)
            v = dict(p=tuple(P[i]), n=tuple(n), tc=tuple(prim['uv'][i]), w=sorted(wb.items(), key=lambda kv: -kv[1])[:4], own=own)
            if cloth_re is not None:
                cw = sum(w for jj, w in zip(prim['J'][i], prim['W'][i]) if w > 1e-5 and cloth_re.search(src.names[src.joints[int(jj)]]))
                v['cloth'] = cw / max(1e-9, sum(prim['W'][i]))
            key = (tuple(np.round(P[i], 4)), tuple(np.round(n, 3)), tuple(np.round(prim['uv'][i], 5)))
            verts.append(weld.setdefault((mk, key), v))
        for t in prim['tris']:
            a, b_, c = int(t[0]), int(t[1]), int(t[2])
            tot = {}
            for k in (a, b_, c):
                for ob, w in verts[k]['own'].items(): tot[ob] = tot.get(ob, 0) + w
            part = part_of(max(tot, key=tot.get))
            if cloth_re is not None and sum(verts[k].get('cloth', 0) for k in (a, b_, c)) >= 1.5:
                part = 'cape'
            if mesh_re is not None and mesh_re.search(prim['name']):
                part = 'cape'
            groups.setdefault((part, mk), []).append((verts[a], verts[c], verts[b_]))
    return groups


TWIST_RE = re.compile(r'(roll|twist)', re.I)
TWIST_SKIP = re.compile(r'(elbow|knee|bend)', re.I)      # joint-volume helpers: stay on their limb bone
TWIST_BONE = {'lhumerus': 'lhumerusX', 'rhumerus': 'rhumerusX', 'lradius': 'lradiusX', 'rradius': 'rradiusX',
              'lfemurYZ': 'lfemurX', 'rfemurYZ': 'rfemurX'}


def twist_targets(src, m, mp, gla):
    """source twist/roll joints -> the Movie Duels twist bone of the limb segment they sit on.
    A roll joint on the upper arm near the elbow (BF2 'ElbowRoll', Mixamo 'ForeArmRoll' parented to
    the upper arm) belongs to the forearm's twist, so the segment is picked by position along the limb."""
    inv = {n: b for b, n in m.items()}
    have = {b['name'].lower() for b in gla.bones}
    out = {}
    for n in src.joints:
        if n in inv or not TWIST_RE.search(src.names[n]) or TWIST_SKIP.search(src.names[n]): continue
        k = mp.par[n]
        while k is not None and k not in inv: k = mp.par[k]
        if k is None: continue
        b = inv[k]
        if b not in TWIST_BONE: continue
        # which segment: compare the roll joint's position along the upper limb
        child = {'lhumerus': 'lradius', 'rhumerus': 'rradius', 'lfemurYZ': 'ltibia', 'rfemurYZ': 'rtibia',
                 'lradius': 'lhand', 'rradius': 'rhand'}.get(b)
        tb = TWIST_BONE[b]
        if child in m and b in ('lhumerus', 'rhumerus'):
            jp = lambda x: src.rest_world[x][:3, 3]
            p0, p1, pn = jp(m[b]), jp(m[child]), jp(n)
            d = p1 - p0; t = float(np.dot(pn - p0, d) / max(np.dot(d, d), 1e-9))
            if t > 0.85: tb = TWIST_BONE[child]        # sits at the elbow: forearm twist
        if tb.lower() in have: out[n] = tb
    return out


def cmd_rig(args):
    from rig import NewSkeleton
    src, gla, m, mp, fit = load(args)
    sk = NewSkeleton(src, gla, m, mp, fit, fingers=getattr(args, 'fingers', False), extra_re=getattr(args, 'extra_bones', None))
    sk.clavicle_k = getattr(args, 'clavicle', 1.0)
    sk.arm_ik = getattr(args, 'arm_ik', False); sk.relax = ({'l': 0.35, 'r': float(__import__('os').environ.get('RGRIP','0.8'))} if getattr(args, 'relax_left', False) else {}); sk.hilt_lift = getattr(args, 'hilt_lift', 0.0) or 1e-9; sk.stock_hand_tags = getattr(args, 'stock_hand_tags', False); sk.ground_drop = getattr(args, 'ground_drop', 0.0); sk.hilt_off = {k: [float(x) for x in v.split(',')] for k, v in (('r', getattr(args, 'hilt_r', None)), ('l', getattr(args, 'hilt_l', None))) if v}
    name = args.name
    skel = args.skeleton or '_humanoid_%s' % name
    # animation names -> frames, from the skeleton's animation.cfg
    anims = {}
    cfgp = os.path.join(os.path.dirname(args.gla), 'animation.cfg')
    if os.path.exists(cfgp):
        for line in open(cfgp, errors='replace'):
            t = line.split()
            if len(t) >= 3 and not t[0].startswith('//'):
                try: anims[t[0].upper()] = (int(t[1]), int(t[2]))
                except ValueError: pass
    if getattr(args, 'fingers', False):
        lf = np.zeros(gla.numFrames, bool)
        for nm, (a0, n) in anims.items():
            if 'LIGHTNING' in nm and n > 0: lf[a0:a0 + n] = True
        sk.lightning_frames = lf
        hf = np.zeros(gla.numFrames, bool)
        for nm, (a0, n) in anims.items():
            if ('DUAL' in nm or 'STAFF' in nm) and n > 0: hf[a0:a0 + n] = True
        sk.hold_frames = hf
    grip_frame = anims.get('BOTH_STAND1', (0, 1))[0]
    if getattr(args, 'fingers', False): sk.fit_thumbs(grip_frame)
    if getattr(args, 'wrap_grip', None):
        import json, wrap as _w
        spec = json.load(open(args.wrap_grip))
        for side, g in spec.items():
            _w.wrap(sk, side, grip_frame, np.array(g['axis']), np.array(g['centre']), g['radius'])
        sk.fit_thumbs(grip_frame)
    gla_path = 'models/players/%s/_humanoid' % skel
    print('skeleton %s: %d bones (%d Movie Duels + %d from the model), hips scale %.3f' %
          (skel, len(sk.names), sk.nmd, len(sk.names) - sk.nmd, sk.hip_k))
    byname = {src.names[n]: n for n in src.joints}
    unbound = re.compile(args.unbound_bones) if getattr(args, 'unbound_bones', None) else None
    # twist (roll) joints: skin on them goes to Movie Duels' own twist bones (lhumerusX, lradiusX,
    # lfemurX ...), which the animations turn part of the way between the joints. Without this the
    # roll joints' skin rides the parent bone rigidly, every wrist turn happens at the wrist and the
    # arms look stiff (and wrap like a sweet wrapper on big turns).
    twist_to = {} if getattr(args, 'no_twist', False) else twist_targets(src, m, mp, gla)
    if twist_to:
        print('twist joints -> Movie Duels twist bones: ' + ', '.join('%s->%s' % (src.names[n], b) for n, b in sorted(twist_to.items(), key=lambda kv: src.names[kv[0]])))
    def newbone_of(jn):
        n = byname.get(jn)
        if n is None: return gla.byName['pelvis']
        if n in twist_to: return gla.byName[twist_to[n].lower()]
        if unbound is not None and unbound.search(jn):
            return gla.byName[fit.owner[n].lower()]      # bone kept in the skeleton, skin stays on the head
        return sk.index[n] if n in sk.index else gla.byName[fit.owner[n].lower()]
    def owner_of(jn):
        n = byname.get(jn)       # (the bind transform still comes from the main limb bone)
        return fit.owner[n] if n is not None else 'pelvis'
    mats = {}
    lod_groups = [collect_rig(args, src, fit, gla, newbone_of, owner_of, mats)]
    for lf in getattr(args, 'lod', None) or []:
        lod_groups.append(collect_rig(args, Source(lf), fit, gla, newbone_of, owner_of, mats))
    return finish_model(args, name, gla_path, sk, lod_groups, mats,
                        [(t, p, relocate_tag(raw, sk, t, grip_frame if getattr(args, 'fingers', False) else None)) for tf in (args.tags or []) for t, p, raw in glmwrite.read_tags(tf)],
                        gla, skel)

# ----------------------------------------------------------------------------- animations
def retarget_frames(src, gla, m, mp, fit, ai, fps, keep_root_motion):
    times, frames = src.sample(ai, fps)
    B = gla.bones; nb = len(B)
    base = [b['base'] for b in B]
    # GLA base matrices carry the skeleton's build scale (fScale, 0.64 for _humanoid): keep
    # rotations orthonormal and put the scale back only when building the global matrix
    bsc = [float(np.cbrt(abs(np.linalg.det(M[:3, :3])))) for M in base]
    brot = [M[:3, :3] / s for M, s in zip(base, bsc)]
    inv_m = {}
    hips_n = m['pelvis']
    # hips path for drift removal
    hp = np.array([fit.conv_p(F[hips_n][:3, 3]) for F in frames])
    if not keep_root_motion:
        up = np.array([0, 0, 1.0])
        t = np.arange(len(hp))
        for ax in (0, 1):
            k = np.polyfit(t, hp[:, ax], 1) if len(t) > 1 else [0, hp[0, ax]]
            hp[:, ax] -= np.polyval(k, t) - hp[0, ax]
        # stand where the source hips stand in bind
        hp[:, :2] += (fit.q[hips_n] - hp[0])[:2]
    out = []
    for fi, F in enumerate(frames):
        R = [None] * nb; Pp = [None] * nb
        for i in gla.order:
            b = B[i]; n = b['name']; par = b['parent']
            if n in m:
                sn = m[n]
                Rs = fit.R0 @ orth(F[sn]); Rb = fit.qr[sn]
                R[i] = Rs @ Rb.T @ fit.D[n].T @ brot[i]
            elif par >= 0:
                R[i] = R[par] @ brot[par].T @ brot[i]
            else:
                R[i] = brot[i]
            if par < 0:
                Pp[i] = base[i][:3, 3]
            elif n == 'pelvis':
                Pp[i] = fit.jpos['pelvis'] + (hp[fi] - fit.q[hips_n]) * 1.0
            else:
                Pp[i] = Pp[par] + R[par] @ brot[par].T @ (base[i][:3, 3] - base[par][:3, 3])
        S = []
        for i in range(nb):
            G = np.eye(4); G[:3, :3] = R[i] * bsc[i]; G[:3, 3] = Pp[i]
            S.append(G @ np.linalg.inv(base[i]))
        local = []
        for i, b in enumerate(B):
            par = b['parent']
            L = S[i] if par < 0 else np.linalg.inv(S[par]) @ S[i]
            local.append(L)
        out.append(local)
    return times, out


def cmd_anim(args):
    src, gla, m, mp, fit = load(args)
    names = src.animations()
    takes = []
    for t in args.take or []:
        a, _, jname = t.partition('=')
        if a not in names:
            raise SystemExit('no animation "%s" in %s (has: %s)' % (a, args.src, ', '.join(names)))
        takes.append((names.index(a), a, jname or a))
    if not takes:
        takes = [(i, a, a) for i, a in enumerate(names)]
    d = gla.d
    nb = gla.numBones
    # existing pool + frame indices
    pool_len = (gla.ofsEnd - gla.ofsPool) // 14
    pool = [d[gla.ofsPool + k * 14:gla.ofsPool + k * 14 + 14] for k in range(pool_len)]
    pool_idx = {c: k for k, c in enumerate(pool)}
    frames_idx = bytearray(d[gla.ofsFrames:gla.ofsFrames + gla.numFrames * nb * 3])
    cfg = []
    first = gla.numFrames
    for ai, aname, jname in takes:
        times, fr = retarget_frames(src, gla, m, mp, fit, ai, args.fps, args.root_motion)
        for local in fr:
            for L in local:
                c = comp(L)
                k = pool_idx.get(c)
                if k is None:
                    k = len(pool); pool.append(c); pool_idx[c] = k
                frames_idx += bytes((k & 255, (k >> 8) & 255, (k >> 16) & 255))
        n = len(fr)
        loop = 0 if args.loop else -1
        cfg.append('%-24s %6d %5d %5d %5d   // %s from %s' % (jname, first, n, loop, int(round(args.fps)), aname, os.path.basename(args.src)))
        print('  %-20s -> %-20s %4d frames at %g fps, starting at frame %d' % (aname, jname, n, args.fps, first))
        first += n
    if len(pool) >= 1 << 24:
        raise SystemExit('compressed bone pool too large')
    nf = first
    hdr = bytearray(d[:gla.ofsFrames])
    body = bytes(frames_idx)
    pad = (-len(hdr) - len(body)) % 4
    ofsPool = len(hdr) + len(body) + pad
    ofsEnd = ofsPool + len(pool) * 14
    import struct
    struct.pack_into('<f6i', hdr, 72, gla.scale, nf, gla.ofsFrames, nb, ofsPool, gla.ofsSkel, ofsEnd)
    open(args.out, 'wb').write(bytes(hdr) + body + b'\0' * pad + b''.join(pool))
    if args.cfg:
        open(args.cfg, 'w').write('// add these lines to animation.cfg next to the .gla (they replace the same names)\n' + '\n'.join(cfg) + '\n')
    print('wrote %s: %d frames (%d new), pool %d bones' % (args.out, nf, nf - gla.numFrames, len(pool)))
    if args.cfg: print('wrote %s' % args.cfg)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    for c in ('map', 'model', 'anim', 'rig'):
        p = sub.add_parser(c)
        p.add_argument('src', help='glTF/GLB, FBX, SMD, DAE, ... with a skeleton')
        p.add_argument('--gla', required=True, help='target _humanoid.gla')
        p.add_argument('--map', help='JSON {"jka bone": "source joint"} to fix or extend the automatic mapping')
        if c == 'rig':
            p.add_argument('--name', required=True); p.add_argument('--out', required=True)
            p.add_argument('--skeleton', help='skeleton folder (default _humanoid_<name>)')
            p.add_argument('--tags', action='append'); p.add_argument('--max-verts', type=int, default=1000)
            p.add_argument('--max-tris', type=int, default=1300, help='triangles per surface (stock renderers: 1300 is safe; our rend2 build: up to 131070)')
            p.add_argument('--cloth-joints')
            p.add_argument('--hilt-r'); p.add_argument('--hilt-l')
            p.add_argument('--hilt-lift', type=float, default=0.0, help='move the saber bolts this far past the fist toward the blade')
            p.add_argument('--ground-drop', type=float, default=0.0); p.add_argument('--wrap-grip'); p.add_argument('--stock-hand-tags', action='store_true')
            p.add_argument('--relax-left', action='store_true')
            p.add_argument('--arm-ik', action='store_true', help='reach each hand to where Movie Duels puts it relative to the shoulders')
            p.add_argument('--clavicle', type=float, default=1.0, help='share of the animation\'s clavicle (shrug) motion to keep, e.g. 0.3 for BF2 shoulder armour')
            p.add_argument('--no-twist', action='store_true', help='leave twist/roll joints on their parent limb bone (old behaviour: stiff forearms)')
            p.add_argument('--fingers', action='store_true', help='keep the model\'s finger bones and curl them with Movie Duels\' finger animation')
            p.add_argument('--lod', action='append', help='lower detail versions of the same model (same skeleton), in order: LOD1, LOD2, ...')
            p.add_argument('--unbound-bones', help='regex of extra bones that carry no skin (kept for later use; their vertices stay on the nearest Movie Duels bone)')
            p.add_argument('--extra-bones', help='regex of source joints kept as extra bones that follow their parent (e.g. "eyeball")')
            p.add_argument('--cloth-mesh', help='regex of source mesh names that are cloth (whole mesh goes to the cape surfaces)')
        if c == 'model':
            p.add_argument('--name', help='model folder name (default: file name)')
            p.add_argument('--out', required=True, help='output .pk3')
            p.add_argument('--tags', action='append', help='existing _humanoid .glm(s) to copy bolt tags (*r_hand, *back, ...) from')
            p.add_argument('--anim-name', help='skeleton path written into the model (default: the .gla name)')
            p.add_argument('--max-verts', type=int, default=1000, help='vertices per surface (stock engine: 1000)')
            p.add_argument('--cloth-joints', help='regex of source joints that are cloth (e.g. "cape"): those triangles go to separate "cape" surfaces for mesh cloth')
        if c == 'anim':
            p.add_argument('--out', required=True, help='output .gla (the input with the new frames appended)')
            p.add_argument('--take', action='append', help='SourceAnim=JKA_ANIM_NAME (default: all, keeping their names)')
            p.add_argument('--cfg', help='write animation.cfg lines here')
            p.add_argument('--fps', type=float, default=20.0)
            p.add_argument('--loop', action='store_true', help='mark as looping')
            p.add_argument('--root-motion', action='store_true', help='keep horizontal hips movement (JKA moves characters itself)')
    a = ap.parse_args()
    {'map': cmd_map, 'model': cmd_model, 'anim': cmd_anim, 'rig': cmd_rig}[a.cmd](a)


if __name__ == '__main__':
    main()
