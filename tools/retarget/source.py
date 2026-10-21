"""Load a rigged character (glTF/GLB directly; FBX, SMD, DAE, X, ... via the assimp CLI)."""
import os, subprocess, tempfile, struct, base64
import numpy as np
import pygltflib

COMP = {5120: np.int8, 5121: np.uint8, 5122: np.int16, 5123: np.uint16, 5125: np.uint32, 5126: np.float32}
NCOMP = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}


def to_gltf(path):
    """Return a .glb path for any input format assimp can read."""
    ext = os.path.splitext(path)[1].lower()
    if ext in ('.glb', '.gltf'):
        return path
    out = os.path.join(tempfile.mkdtemp(prefix='jkaimp_'), os.path.splitext(os.path.basename(path))[0] + '.glb')
    r = subprocess.run(['assimp', 'export', path, out, '-f', 'glb2', '-lbw', '-jiv'], capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        raise SystemExit('assimp could not convert %s:\n%s%s' % (path, r.stdout[-2000:], r.stderr[-2000:]))
    return out


def trs_matrix(t, r, s):
    x, y, z, w = r
    R = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
                  [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
                  [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)]])
    M = np.eye(4); M[:3, :3] = R * np.array(s)[None, :]; M[:3, 3] = t
    return M


class Source:
    def __init__(self, path):
        self.orig_dir = os.path.dirname(os.path.abspath(path))
        self.path = to_gltf(path)
        self.g = g = pygltflib.GLTF2().load(self.path)
        self.blob = g.binary_blob() if self.path.lower().endswith('.glb') else None
        self.base_dir = os.path.dirname(self.path)
        n = len(g.nodes)
        self.parent = [-1] * n
        for i, nd in enumerate(g.nodes):
            for c in nd.children or []:
                self.parent[c] = i
        self.names = [nd.name or 'node%d' % i for i, nd in enumerate(g.nodes)]
        self.rest_trs = []
        for nd in g.nodes:
            if nd.matrix:
                M = np.array(nd.matrix, dtype=np.float64).reshape(4, 4).T
                t = M[:3, 3].copy(); s = np.linalg.norm(M[:3, :3], axis=0)
                R = M[:3, :3] / s[None, :]
                from g2 import mat_to_quat
                w, x, y, z = mat_to_quat(R)
                self.rest_trs.append((t, np.array([x, y, z, w]), s))
            else:
                self.rest_trs.append((np.array(nd.translation or [0, 0, 0], float),
                                      np.array(nd.rotation or [0, 0, 0, 1], float),
                                      np.array(nd.scale or [1, 1, 1], float)))
        if not g.skins:
            raise SystemExit('no skin (skeleton) in %s' % path)
        # use the skin with the most joints
        self.skin = max(g.skins, key=lambda s: len(s.joints))
        self.joints = list(self.skin.joints)
        ibm = self.acc(self.skin.inverseBindMatrices).reshape(-1, 4, 4).transpose(0, 2, 1) if self.skin.inverseBindMatrices is not None \
            else np.tile(np.eye(4), (len(self.joints), 1, 1))
        self.ibm = ibm.astype(np.float64)
        self.rest_world = self.world(self.local_rest())
        # mesh node(s) using this skin
        self.mesh_nodes = [i for i, nd in enumerate(g.nodes) if nd.mesh is not None and nd.skin is not None
                           and g.skins[nd.skin] is self.skin]
        mesh_world = self.rest_world[self.mesh_nodes[0]] if self.mesh_nodes else np.eye(4)
        self.mesh_world = mesh_world
        # bind pose of each joint, in world space (what the mesh was skinned in)
        # glTF ignores a skinned mesh's own node transform: vertex = sum w * jointWorld * IBM * v.
        # At rest jointWorld * IBM is the same mesh->world matrix for every joint (unless the
        # rest pose differs from the bind pose); take it from the first joint and put the
        # mesh and the bind pose in world space, the same space the animations sample to.
        self.skin_to_world = self.rest_world[self.joints[0]] @ self.ibm[0]
        self.bind = np.array([self.skin_to_world @ np.linalg.inv(self.ibm[k]) for k in range(len(self.joints))])

    # --- buffers -------------------------------------------------------
    def buffer_bytes(self, bi):
        b = self.g.buffers[bi]
        if b.uri is None:
            return self.blob
        if b.uri.startswith('data:'):
            return base64.b64decode(b.uri.split(',', 1)[1])
        return open(os.path.join(self.base_dir, b.uri), 'rb').read()

    def acc(self, ai):
        a = self.g.accessors[ai]
        nc = NCOMP[a.type]; dt = np.dtype(COMP[a.componentType])
        if a.bufferView is None:
            return np.zeros((a.count, nc), dtype=np.float64)
        bv = self.g.bufferViews[a.bufferView]
        data = self.buffer_bytes(bv.buffer)
        off = (bv.byteOffset or 0) + (a.byteOffset or 0)
        stride = bv.byteStride or nc * dt.itemsize
        if stride == nc * dt.itemsize:
            arr = np.frombuffer(data, dtype=dt, count=a.count * nc, offset=off).reshape(a.count, nc)
        else:
            arr = np.array([np.frombuffer(data, dtype=dt, count=nc, offset=off + k * stride) for k in range(a.count)])
        arr = arr.astype(np.float64)
        if a.normalized:
            if dt == np.uint8: arr /= 255.0
            elif dt == np.uint16: arr /= 65535.0
            elif dt == np.int8: arr = np.maximum(arr / 127.0, -1)
            elif dt == np.int16: arr = np.maximum(arr / 32767.0, -1)
        return arr if nc > 1 else arr[:, 0]

    # --- hierarchy -----------------------------------------------------
    def local_rest(self):
        return [trs_matrix(*trs) for trs in self.rest_trs]

    def world(self, local):
        n = len(local); W = [None] * n
        def get(i):
            if W[i] is None:
                W[i] = local[i] if self.parent[i] < 0 else get(self.parent[i]) @ local[i]
            return W[i]
        for i in range(n): get(i)
        return np.array(W)

    # --- meshes ----------------------------------------------------------
    def meshes(self):
        """Yield dicts per primitive: pos, nrm, uv, joints (skin-joint indices), weights, tris, material."""
        out = []
        for ni in self.mesh_nodes:
            mesh = self.g.meshes[self.g.nodes[ni].mesh]
            for pi, p in enumerate(mesh.primitives):
                at = p.attributes
                if at.POSITION is None or at.JOINTS_0 is None:
                    continue
                pos = self.acc(at.POSITION)
                nrm = self.acc(at.NORMAL) if at.NORMAL is not None else np.zeros_like(pos)
                S = self.skin_to_world
                pos = pos @ S[:3, :3].T + S[:3, 3]
                L = S[:3, :3]; NL = np.linalg.inv(L).T
                nrm = nrm @ NL.T
                nrm /= np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-12)
                uv = self.acc(at.TEXCOORD_0) if at.TEXCOORD_0 is not None else np.zeros((len(pos), 2))
                J = self.acc(at.JOINTS_0).astype(int); W = self.acc(at.WEIGHTS_0)
                if getattr(at, 'JOINTS_1', None) is not None:
                    J = np.hstack([J, self.acc(at.JOINTS_1).astype(int)]); W = np.hstack([W, self.acc(at.WEIGHTS_1)])
                idx = self.acc(p.indices).astype(int) if p.indices is not None else np.arange(len(pos))
                tris = idx.reshape(-1, 3)
                out.append(dict(name='%s_%d' % (mesh.name or 'mesh', pi), pos=pos, nrm=nrm, uv=uv, J=J, W=W, tris=tris,
                                material=p.material))
        return out

    def material_image(self, mi):
        """(name, bytes, ext) of a material's base colour texture, or None."""
        if mi is None:
            return None
        m = self.g.materials[mi]
        name = (m.name or 'mat%d' % mi)
        pbr = m.pbrMetallicRoughness
        if not pbr or not pbr.baseColorTexture:
            return (name, None, None)
        tex = self.g.textures[pbr.baseColorTexture.index]
        img = self.g.images[tex.source]
        if img.bufferView is not None:
            bv = self.g.bufferViews[img.bufferView]
            data = self.buffer_bytes(bv.buffer)[(bv.byteOffset or 0):(bv.byteOffset or 0) + bv.byteLength]
        elif img.uri and img.uri.startswith('data:'):
            data = base64.b64decode(img.uri.split(',', 1)[1])
        elif img.uri:
            data = None
            from urllib.parse import unquote
            u = unquote(img.uri).replace('\\', '/')
            for p in (os.path.join(self.base_dir, u), os.path.join(self.orig_dir, u), os.path.join(self.orig_dir, os.path.basename(u))):
                if os.path.exists(p):
                    data = open(p, 'rb').read(); break
            if data is None:   # case-insensitive look-up next to the source file
                want = os.path.basename(u).lower()
                for f in os.listdir(self.orig_dir):
                    if f.lower() == want:
                        data = open(os.path.join(self.orig_dir, f), 'rb').read(); break
        else:
            data = None
        if data and data[:4] == b'\x89PNG': ext = '.png'
        elif data and data[:2] == b'\xff\xd8': ext = '.jpg'
        elif data:
            # TGA, DDS, ...: re-encode as PNG
            try:
                from PIL import Image
                import io
                b = io.BytesIO(); Image.open(io.BytesIO(data)).save(b, 'PNG'); data = b.getvalue(); ext = '.png'
            except Exception:
                ext = '.tga'
        else:
            ext = None
        return (name, data, ext)

    # --- animations ------------------------------------------------------
    def animations(self):
        return [a.name or 'anim%d' % i for i, a in enumerate(self.g.animations)]

    def sample(self, ai, fps=20.0):
        """World transforms of every node, sampled at fps. Returns (times, [frames][nodes] 4x4)."""
        a = self.g.animations[ai]
        tracks = {}
        tmax = 0.0
        for ch in a.channels:
            s = a.samplers[ch.sampler]
            t = self.acc(s.input); v = self.acc(s.output)
            if s.interpolation == 'CUBICSPLINE':
                v = v.reshape(len(t), 3, -1)[:, 1]
            tracks[(ch.target.node, ch.target.path)] = (t, v, s.interpolation)
            tmax = max(tmax, float(t[-1]))
        n = max(2, int(round(tmax * fps)) + 1)
        times = np.linspace(0, tmax, n)
        frames = []
        for tt in times:
            local = []
            for i, (t0, r0, s0) in enumerate(self.rest_trs):
                tr, ro, sc = t0, r0, s0
                for path in ('translation', 'rotation', 'scale'):
                    k = (i, path)
                    if k in tracks:
                        val = interp(*tracks[k], tt, path == 'rotation')
                        if path == 'translation': tr = val
                        elif path == 'rotation': ro = val
                        else: sc = val
                local.append(trs_matrix(tr, ro, sc))
            frames.append(self.world(local))
        return times, frames


def interp(t, v, mode, x, quat):
    if x <= t[0]: return v[0]
    if x >= t[-1]: return v[-1]
    k = int(np.searchsorted(t, x)) - 1
    if mode == 'STEP': return v[k]
    f = (x - t[k]) / max(1e-9, t[k + 1] - t[k])
    a, b = v[k], v[k + 1]
    if quat:
        if np.dot(a, b) < 0: b = -b
        q = a * (1 - f) + b * f
        return q / np.linalg.norm(q)
    return a * (1 - f) + b * f
