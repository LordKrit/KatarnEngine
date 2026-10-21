"""Write Ghoul2 .glm models."""
import struct

G2SURF_TAG = 1
G2SURF_OFF = 2


def pack_weights(ws):
    """ws: list of (local bone ref index, weight) -> (u32, 4 bytes). At most 4 bones, refs < 32."""
    items = sorted(ws, key=lambda kv: -kv[1])[:4]
    tot = sum(v for _, v in items) or 1.0
    items = [(b, v / tot) for b, v in items]
    nw = len(items)
    u32 = (nw - 1) << 30
    wb = [0, 0, 0, 0]
    for i, (b, v) in enumerate(items):
        assert 0 <= b < 32
        u32 |= (b & 31) << (5 * i)
        if i < nw - 1:
            q = max(0, min(1023, int(round(v * 1023.0))))
            wb[i] = q & 255
            u32 |= ((q >> 8) & 3) << (20 + 2 * i)
    return u32, bytes(wb)


def surface_bytes(s, index, abs_off):
    """s: dict(verts=[dict(p,n,tc,w=[(globalBone, weight)])], tris=[(a,b,c)])"""
    refs = sorted({b for v in s['verts'] for b, _ in v['w']})
    if len(refs) > 32:
        raise ValueError('surface uses %d bones (max 32)' % len(refs))
    local = {b: i for i, b in enumerate(refs)}
    nv, nt, nbr = len(s['verts']), len(s['tris']), len(refs)
    ofsT = 40; ofsV = ofsT + nt * 12; ofsBR = ofsV + nv * 40; ofsE = ofsBR + nbr * 4
    out = bytearray(struct.pack('<10i', 0, index, -abs_off, nv, ofsV, nt, ofsT, nbr, ofsBR, ofsE))
    for t in s['tris']:
        out += struct.pack('<3i', *t)
    for v in s['verts']:
        u32, wb = pack_weights([(local[b], w) for b, w in v['w']])
        out += struct.pack('<3f', *v['n']) + struct.pack('<3f', *v['p']) + struct.pack('<I', u32) + wb
    for v in s['verts']:
        out += struct.pack('<2f', *v['tc'])
    out += struct.pack('<%di' % nbr, *refs)
    assert len(out) == ofsE
    return bytes(out)


def write_glm(path, name, anim_name, num_bones, hier, lods):
    """hier: list of dict(name, flags, shader, parent). lods: list (per LOD) of lists of surfaces, one per hier entry,
    either a surface dict or raw bytes of an existing surface (its header index/offset get patched)."""
    ns = len(hier)
    for h in hier:
        h['children'] = [i for i, x in enumerate(hier) if x['parent'] == hier.index(h)]
    hbody = bytearray(); hoffs = []
    for i, h in enumerate(hier):
        hoffs.append(ns * 4 + len(hbody))
        hbody += struct.pack('<64sI64s3i', h['name'].encode()[:63], h['flags'], h['shader'].encode()[:63], 0,
                             h['parent'], len(h['children']))
        hbody += struct.pack('<%di' % len(h['children']), *h['children'])
    hblock = struct.pack('<%di' % ns, *hoffs) + bytes(hbody)
    ofsH = 164
    ofsL = ofsH + len(hblock)
    lodblocks = bytearray()
    for surfs in lods:
        lod_start = ofsL + len(lodblocks)
        body = bytearray(); offs = []
        for i, s in enumerate(surfs):
            rel = 4 + ns * 4 + len(body)
            offs.append(rel - 4)
            if isinstance(s, (bytes, bytearray)):
                b = bytearray(s)
                struct.pack_into('<ii', b, 4, i, -(lod_start + rel))
                body += b
            else:
                body += surface_bytes(s, i, lod_start + rel)
        lodblocks += struct.pack('<i', 4 + ns * 4 + len(body)) + struct.pack('<%di' % ns, *offs) + body
    total = ofsL + len(lodblocks)
    hdr = struct.pack('<4si64s64s7i', b'2LGM', 6, name.encode()[:63], anim_name.encode()[:63], 0, num_bones,
                      len(lods), ofsL, ns, ofsH + ns * 4, total)   # ofsSurfHierarchy = first entry, after the offset table
    assert len(hdr) == 164
    open(path, 'wb').write(hdr + hblock + bytes(lodblocks))
    return total


def read_tags(path):
    """Tag surfaces (and their parent names) from an existing .glm: [(name, parent_name, raw LOD0 surface bytes)]"""
    d = open(path, 'rb').read()
    numBones, numLODs, ofsLODs, ns, ofsSH = struct.unpack_from('<5i', d, 140)
    tab = struct.unpack_from('<%di' % ns, d, 164)
    hier = []
    for i in range(ns):
        p = 164 + tab[i]
        nm = d[p:p + 64].split(b'\0')[0].decode()
        flags = struct.unpack_from('<I', d, p + 64)[0]
        parent = struct.unpack_from('<i', d, p + 136)[0]
        hier.append((nm, flags, parent))
    lso = ofsLODs + 4
    out = []
    for i in range(ns):
        so = lso + struct.unpack_from('<i', d, lso + i * 4)[0]
        ofsEnd = struct.unpack_from('<i', d, so + 36)[0]
        idx = struct.unpack_from('<i', d, so + 4)[0]
        nm, flags, parent = hier[idx]
        if flags & G2SURF_TAG:
            out.append((nm, hier[parent][0] if parent >= 0 else None, d[so:so + ofsEnd]))
    return out
