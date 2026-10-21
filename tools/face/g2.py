"""Ghoul2 (.glm/.gla) reading and writing helpers."""
import struct, numpy as np

def mat34(f):  # 12 floats -> 4x4
    m = np.eye(4); m[:3, :4] = np.array(f, dtype=np.float64).reshape(3, 4); return m

class GLA:
    def __init__(self, path):
        d = self.d = open(path, 'rb').read()
        ident, ver = struct.unpack_from('<4si', d, 0)
        self.name = d[8:72].split(b'\0')[0].decode()
        self.scale, self.numFrames, self.ofsFrames, self.numBones, self.ofsPool, self.ofsSkel, self.ofsEnd = struct.unpack_from('<f6i', d, 72)
        offs = struct.unpack_from('<%di' % self.numBones, d, 100)
        self.bones = []
        for i in range(self.numBones):
            p = 100 + offs[i]
            name = d[p:p + 64].split(b'\0')[0].decode()
            flags, parent = struct.unpack_from('<Ii', d, p + 64)
            base = mat34(struct.unpack_from('<12f', d, p + 72))
            inv = mat34(struct.unpack_from('<12f', d, p + 120))
            nch = struct.unpack_from('<i', d, p + 168)[0]
            ch = struct.unpack_from('<%di' % nch, d, p + 172)
            self.bones.append(dict(name=name, flags=flags, parent=parent, base=base, inv=inv, children=list(ch)))
        self.byName = {b['name'].lower(): i for i, b in enumerate(self.bones)}
        # parents first (a GLA does not guarantee it: the face bones hang off 'face', bone 52)
        self.order = []
        done = set()
        def visit(i):
            if i in done: return
            p = self.bones[i]['parent']
            if p >= 0: visit(p)
            done.add(i); self.order.append(i)
        for i in range(self.numBones): visit(i)

    def comp(self, frame, bone):
        off = self.ofsFrames + (frame * self.numBones + bone) * 3
        idx = self.d[off] | (self.d[off + 1] << 8) | (self.d[off + 2] << 16)
        return self.d[self.ofsPool + idx * 14:self.ofsPool + idx * 14 + 14]

def uncomp(c):
    w, x, y, z, tx, ty, tz = struct.unpack('<7H', c)
    w, x, y, z = [v / 16383.0 - 2.0 for v in (w, x, y, z)]
    m = np.eye(4)
    m[:3, :3] = quat_to_mat(np.array([w, x, y, z]))
    m[:3, 3] = [v / 64.0 - 512.0 for v in (tx, ty, tz)]
    return m

def comp(m):
    q = mat_to_quat(m[:3, :3])
    if q[0] < 0: q = -q
    vals = [int(round((v + 2.0) * 16383.0)) for v in q]
    t = [int(round((v + 512.0) * 64.0)) for v in m[:3, 3]]
    vals = [max(0, min(65535, v)) for v in vals + t]
    return struct.pack('<7H', *vals)

def quat_to_mat(q):
    w, x, y, z = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
                     [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
                     [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)]])

def mat_to_quat(R):
    # robust (Shepperd)
    t = np.trace(R)
    if t > 0:
        s = np.sqrt(t + 1.0) * 2; w = 0.25 * s
        x = (R[2, 1] - R[1, 2]) / s; y = (R[0, 2] - R[2, 0]) / s; z = (R[1, 0] - R[0, 1]) / s
    elif R[0, 0] > R[1, 1] and R[0, 0] > R[2, 2]:
        s = np.sqrt(1.0 + R[0, 0] - R[1, 1] - R[2, 2]) * 2; w = (R[2, 1] - R[1, 2]) / s
        x = 0.25 * s; y = (R[0, 1] + R[1, 0]) / s; z = (R[0, 2] + R[2, 0]) / s
    elif R[1, 1] > R[2, 2]:
        s = np.sqrt(1.0 + R[1, 1] - R[0, 0] - R[2, 2]) * 2; w = (R[0, 2] - R[2, 0]) / s
        x = (R[0, 1] + R[1, 0]) / s; y = 0.25 * s; z = (R[1, 2] + R[2, 1]) / s
    else:
        s = np.sqrt(1.0 + R[2, 2] - R[0, 0] - R[1, 1]) * 2; w = (R[1, 0] - R[0, 1]) / s
        x = (R[0, 2] + R[2, 0]) / s; y = (R[1, 2] + R[2, 1]) / s; z = 0.25 * s
    q = np.array([w, x, y, z]); return q / np.linalg.norm(q)
