"""Recreate a q3map2 .srf (surface extra) file for a compiled RBSP map so q3map2 -light can
relight it without the original compile files.  usage: make_srf.py map.bsp [sampleSize]"""
import struct, sys, re, math

path = sys.argv[1]
sample = int(sys.argv[2]) if len(sys.argv) > 2 else 16
d = open(path, 'rb').read()
assert d[:4] == b'RBSP'
lumps = [struct.unpack_from('<ii', d, 8 + i * 8) for i in range(18)]

def lump(i):
    o, l = lumps[i]
    return d[o:o + l]

ents = lump(0).decode('latin1')
blocks = re.findall(r'\{([^{}]*)\}', ents)
model_ent = {0: 0}
for ei, b in enumerate(blocks):
    m = re.search(r'"model"\s*"\*(\d+)"', b)
    if m:
        model_ent[int(m.group(1))] = ei

models = lump(7)
surf_ent = {}
for mi in range(len(models) // 40):
    fs, ns = struct.unpack_from('<ii', models, mi * 40 + 24)
    for s in range(fs, fs + ns):
        surf_ent[s] = model_ent.get(mi, 0)

verts = lump(10)
VS = 80
def vpos(i):
    return struct.unpack_from('<3f', verts, i * VS)
def vnorm(i):
    return struct.unpack_from('<3f', verts, i * VS + 52)

surfs = lump(13)
SS = 148
out = ['default\n{\n\tsampleSize %d\n}\n' % sample]
n = len(surfs) // SS
for s in range(n):
    f = struct.unpack_from('<7i', surfs, s * SS)
    shader, fog, stype, fv, nv, fi, ni = f
    pw, ph = struct.unpack_from('<2i', surfs, s * SS + SS - 8)
    # average normal -> dominant axis
    nx = ny = nz = 0.0
    for v in range(fv, fv + nv):
        a, b, c = vnorm(v); nx += a; ny += b; nz += c
    ax = max(range(3), key=lambda k: abs((nx, ny, nz)[k]))
    axis = [0, 0, 0]; axis[ax] = 1 if (nx, ny, nz)[ax] >= 0 else -1
    lines = ['%d\n{\n' % s, '\tentity %d\n' % surf_ent.get(s, 0),
             '\tlightmapAxis ( %d %d %d )\n' % tuple(axis)]
    if surf_ent.get(s, 0) != 0:
        lines.append('\tcastShadows 0\n\treceiveShadows 1\n')
    if stype == 2 and pw > 0 and ph > 0:   # patch: longest control polyline
        longest = 0.0
        for r in range(ph):
            L = sum(math.dist(vpos(fv + r * pw + c), vpos(fv + r * pw + c + 1)) for c in range(pw - 1))
            longest = max(longest, L)
        for c in range(pw):
            L = sum(math.dist(vpos(fv + r * pw + c), vpos(fv + (r + 1) * pw + c)) for r in range(ph - 1))
            longest = max(longest, L)
        lines.append('\tlongestCurve %f\n' % max(longest, 1.0))
    lines.append('}\n')
    out.append(''.join(lines))
open(path[:-4] + '.srf', 'w').write(''.join(out))
print('wrote', n, 'surfaces')
