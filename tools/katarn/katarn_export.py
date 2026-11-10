"""KatarnEngine scene exporter for Blender 5.x.

Writes a scene folder that tools/katarn_cook turns into a .kworld for the KatarnEngine renderer:

  scene.json        materials, meshes (pack offsets), instances, lights, sun, world, camera
  pack_000.bin ...  mesh data, at most ~256 MB per file
                    per mesh: float32 x 8 per vertex (pos xyz, normal xyz, uv), then uint32 indices
  textures/         every image the materials use (packed images are written out)

Meshes stay in their own object space and every object or instance (collection instances, geometry
node instances, particles) becomes one instance with a matrix, so repeated props are stored once.
Units are Blender metres; the cooker converts to Quake units.

Run from a command prompt (nothing in the .blend is changed or saved):
  blender.exe -b Naboo_Theed.blend -P katarn_export.py -- <output folder>
or open the .blend, Scripting tab, open this file, Run Script (output: <blend folder>/export/<blend name>).
"""
import bpy, json, os, re, sys, time, math, shutil
import numpy as np

PACK_LIMIT = 256 << 20
t0 = time.time()

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
blend = bpy.data.filepath
stem = os.path.splitext(os.path.basename(blend))[0] if blend else "scene"
OUT = argv[0] if argv else os.path.join(os.path.dirname(blend), "export", re.sub(r"[^a-z0-9_]+", "_", stem.lower()))
TEX = os.path.join(OUT, "textures")
os.makedirs(TEX, exist_ok=True)
logf = open(os.path.join(OUT, "export_log.txt"), "w", encoding="utf-8")


def log(*a):
    s = " ".join(str(x) for x in a)
    print("[katarn] " + s, flush=True)
    logf.write(s + "\n"); logf.flush()


log("blend", blend, "->", OUT, "| Blender", bpy.app.version_string)


def _excepthook(t, v, tb):
    import traceback
    log("EXPORT FAILED:\n" + "".join(traceback.format_exception(t, v, tb)))
sys.excepthook = _excepthook

# --------------------------------------------------------------------------- images
images = {}      # image name -> exported record


def _readable(path):
    """the cooker reads PNG, JPEG, TGA, BMP and Radiance HDR"""
    try:
        with open(path, "rb") as f:
            h = f.read(4)
    except OSError:
        return False
    ext = os.path.splitext(path)[1].lower()
    if h[:4] == b"\x89PNG" or h[:2] == b"\xff\xd8" or h[:2] == b"BM" or h[:2] == b"#?":
        return True
    return ext == ".tga" and h[:4] != b"DDS "


def _resave(img, name):
    """re-encode through Blender (DDS / EXR / TIFF / BC6H ... -> PNG or HDR)"""
    w, h = img.size
    if w <= 0 or h <= 0:
        return None
    px = np.empty(w * h * 4, np.float32)
    img.pixels.foreach_get(px)
    is_float = bool(img.is_float)
    out = os.path.splitext(name)[0] + (".hdr" if is_float else ".png")
    tmp = bpy.data.images.new("__katarn_tmp", w, h, alpha=True, float_buffer=is_float)
    try:
        tmp.colorspace_settings.name = img.colorspace_settings.name
    except Exception:
        pass
    tmp.pixels.foreach_set(px)
    tmp.filepath_raw = os.path.join(TEX, out)
    tmp.file_format = "HDR" if is_float else "PNG"
    tmp.save()
    bpy.data.images.remove(tmp)
    return out


def export_image(img):
    if img is None:
        return None
    if img.name in images:
        return images[img.name]["file"]
    rec = {"file": None, "colorspace": img.colorspace_settings.name, "size": list(img.size)}
    base = re.sub(r"[^A-Za-z0-9_.-]+", "_", img.name)
    try:
        if img.source == "TILED":
            log("  skip UDIM image", img.name)
        elif img.packed_file is not None:
            ext = {"PNG": ".png", "JPEG": ".jpg", "TARGA": ".tga", "TARGA_RAW": ".tga", "OPEN_EXR": ".exr",
                   "HDR": ".hdr", "BMP": ".bmp", "TIFF": ".tif", "WEBP": ".webp"}.get(img.file_format, ".png")
            name = os.path.splitext(base)[0] + ext
            open(os.path.join(TEX, name), "wb").write(img.packed_file.data)
            rec["file"] = name
        elif img.source in ("FILE", "SEQUENCE", "MOVIE"):
            src = bpy.path.abspath(img.filepath, library=img.library)
            if os.path.isfile(src):
                name = os.path.splitext(base)[0] + os.path.splitext(src)[1].lower()
                dst = os.path.join(TEX, name)
                if not os.path.exists(dst) or os.path.getsize(dst) != os.path.getsize(src):
                    shutil.copyfile(src, dst)
                rec["file"] = name
            else:
                log("  missing image file", img.name, src)
        elif img.has_data:          # generated / rendered: save a copy
            name = os.path.splitext(base)[0] + ".png"
            c = img.copy(); c.filepath_raw = os.path.join(TEX, name); c.file_format = "PNG"; c.save()
            bpy.data.images.remove(c)
            rec["file"] = name
        if rec["file"] and not _readable(os.path.join(TEX, rec["file"])):
            conv = _resave(img, rec["file"])
            log("  converted", rec["file"], "->", conv)
            if conv:
                try:
                    os.remove(os.path.join(TEX, rec["file"]))
                except OSError:
                    pass
            rec["file"] = conv
    except Exception as e:
        log("  image failed", img.name, e)
    images[img.name] = rec
    return rec["file"]


# --------------------------------------------------------------------------- materials
PASS = {"MIX_RGB", "MIX", "HUE_SAT", "GAMMA", "BRIGHTCONTRAST", "INVERT", "CURVE_RGB", "VALTORGB", "MATH",
        "MAP_RANGE", "CLAMP", "RGBTOBW", "SEPARATE_COLOR", "SEPRGB", "SEPARATE_RGB", "NORMAL_MAP", "BUMP",
        "REROUTE", "GROUP", "ATTRIBUTE", "VECT_MATH", "COMBINE_COLOR"}


def trace_image(sock, depth=0):
    """first image texture upstream of a socket -> (image, output socket name, channel, through)"""
    if sock is None or not sock.is_linked or depth > 12:
        return None
    link = sock.links[0]
    n, out = link.from_node, link.from_socket
    if n.type == "TEX_IMAGE":
        return {"image": n.image, "out": out.name, "chan": None, "via": []}
    if n.type in ("SEPARATE_COLOR", "SEPRGB", "SEPARATE_RGB"):
        r = trace_image(n.inputs[0], depth + 1)
        if r:
            r["chan"] = {"Red": 0, "R": 0, "Green": 1, "G": 1, "Blue": 2, "B": 2}.get(out.name)
        return r
    if n.type == "GROUP" and n.node_tree:
        # follow into the group to its output, then back out through the matching input
        for gn in n.node_tree.nodes:
            if gn.type == "GROUP_OUTPUT":
                s = gn.inputs.get(out.name)
                r = trace_image(s, depth + 1) if s else None
                if r: return r
    for inp in n.inputs:
        if inp.is_linked and inp.type in ("RGBA", "VALUE", "VECTOR"):
            r = trace_image(inp, depth + 1)
            if r:
                r["via"].append(n.type)
                return r
    return None


def val(sock):
    if sock is None: return None
    v = sock.default_value
    try: return [float(x) for x in v]
    except TypeError: return float(v)


def find_principled(node, depth=0):
    if node is None or depth > 8: return None
    if node.type in ("BSDF_PRINCIPLED", "BSDF_DIFFUSE", "EMISSION", "BSDF_GLOSSY"):
        return node
    if node.type in ("MIX_SHADER", "ADD_SHADER"):
        found = []
        for inp in node.inputs:
            if inp.type == "SHADER" and inp.is_linked:
                f = find_principled(inp.links[0].from_node, depth + 1)
                if f: found.append(f)
        found.sort(key=lambda f: f.type != "BSDF_PRINCIPLED")   # prefer a Principled BSDF
        return found[0] if found else None
    if node.type == "GROUP" and node.node_tree:
        for gn in node.node_tree.nodes:
            if gn.type == "GROUP_OUTPUT" and gn.inputs and gn.inputs[0].is_linked:
                return find_principled(gn.inputs[0].links[0].from_node, depth + 1)
    return None


def has_shader(node, kind, depth=0):
    """a shader node of this type anywhere in the mix / add / group tree feeding the output"""
    if node is None or depth > 12: return False
    if node.type == kind: return True
    if node.type in ("MIX_SHADER", "ADD_SHADER", "REROUTE"):
        return any(has_shader(i.links[0].from_node, kind, depth + 1) for i in node.inputs
                   if i.is_linked and (i.type == "SHADER" or node.type == "REROUTE"))
    if node.type == "GROUP" and node.node_tree:
        for gn in node.node_tree.nodes:
            if gn.type == "GROUP_OUTPUT":
                for i in gn.inputs:
                    if i.type == "SHADER" and i.is_linked and has_shader(i.links[0].from_node, kind, depth + 1):
                        return True
    return False


def tex_ref(t):
    if not t: return None
    f = export_image(t["image"])
    if not f: return None
    return {"file": f, "out": t["out"], "chan": t["chan"], "via": t["via"]}


materials = {}
groups = {}          # node group trees used by materials

_SKIP_PROPS = {"rna_type", "name", "label", "location", "width", "width_hidden", "height", "dimensions", "inputs", "outputs",
               "internal_links", "parent", "use_custom_color", "color", "select", "show_options", "show_preview", "hide",
               "mute", "show_texture", "bl_idname", "bl_label", "bl_description", "bl_icon", "bl_static_type", "bl_width_default",
               "bl_width_min", "bl_width_max", "bl_height_default", "bl_height_min", "bl_height_max", "type", "location_absolute",
               "warning_propagation", "color_tag", "texture_mapping", "color_mapping", "image_user", "interface", "is_active_output"}


def _val(v):
    if isinstance(v, str):
        return v
    if isinstance(v, bool):
        return v
    if isinstance(v, set):
        return sorted(v)
    try:
        return [float(x) for x in v]
    except TypeError:
        if isinstance(v, bool):
            return v
        if isinstance(v, (int, float)):
            return float(v)
        return str(v)


def dump_tree(nt):
    """every node with its properties, unlinked input values and links (sockets by identifier)"""
    nodes = {}
    for n in nt.nodes:
        d = {"type": n.type, "props": {}, "inputs": [], "outputs": []}
        for prop in n.bl_rna.properties:
            k = prop.identifier
            if k in _SKIP_PROPS or prop.type in ("POINTER", "COLLECTION"):
                continue
            try:
                d["props"][k] = _val(getattr(n, k))
            except Exception:
                pass
        if n.type == "TEX_IMAGE" and n.image is not None:
            d["props"]["image"] = export_image(n.image)
            d["props"]["colorspace"] = n.image.colorspace_settings.name
            d["props"]["alpha_mode"] = n.image.alpha_mode
        if n.type == "TEX_ENVIRONMENT" and n.image is not None:
            d["props"]["image"] = export_image(n.image)
            d["props"]["colorspace"] = n.image.colorspace_settings.name
        if n.type == "VALTORGB":
            cr = n.color_ramp
            d["props"]["ramp"] = {"interpolation": cr.interpolation, "color_mode": cr.color_mode,
                                  "elements": [[e.position] + list(e.color) for e in cr.elements]}
        if n.type in ("CURVE_RGB", "CURVE_VEC", "CURVE_FLOAT"):
            try:
                n.mapping.initialize()
                d["props"]["curves"] = [[[p.location[0], p.location[1]] for p in c.points] for c in n.mapping.curves]
            except Exception:
                pass
        if n.type == "GROUP" and n.node_tree is not None:
            d["props"]["tree"] = n.node_tree.name
            if n.node_tree.name not in groups:
                groups[n.node_tree.name] = None
                groups[n.node_tree.name] = dump_tree(n.node_tree)
        for s in n.inputs:
            e = {"id": s.identifier, "name": s.name, "type": s.type, "enabled": bool(getattr(s, "enabled", True))}
            if hasattr(s, "default_value"):
                try:
                    e["value"] = _val(s.default_value)
                except Exception:
                    pass
            if s.is_linked and s.links:
                lk = s.links[0]
                if not lk.is_muted:
                    e["link"] = [lk.from_node.name, lk.from_socket.identifier]
            d["inputs"].append(e)
        for s in n.outputs:
            e = {"id": s.identifier, "name": s.name, "type": s.type, "enabled": bool(getattr(s, "enabled", True))}
            if n.type in ("VALUE", "RGB") and hasattr(s, "default_value"):
                e["value"] = _val(s.default_value)
            d["outputs"].append(e)
        if n.type == "OUTPUT_MATERIAL" or n.type == "OUTPUT_WORLD":
            d["props"]["active"] = bool(n.is_active_output)
        nodes[n.name] = d
    return {"nodes": nodes}


def material(m):
    if m is None:
        m_name = "__default"
        if m_name not in materials:
            materials[m_name] = {"baseColor": [0.8, 0.8, 0.8, 1], "roughness": 0.6, "metallic": 0.0}
        return m_name
    if m.name in materials:
        return m.name
    info = {"baseColor": list(m.diffuse_color), "roughness": float(m.roughness), "metallic": float(m.metallic),
            "twoSided": not m.use_backface_culling, "alphaMode": "OPAQUE"}
    rm = getattr(m, "surface_render_method", "DITHERED")
    info["renderMethod"] = rm
    nt = m.node_tree if m.use_nodes else None
    if nt is not None:
        try:
            info["graph"] = dump_tree(nt)
        except Exception as e:
            log("  graph dump failed for", m.name, e)
    if nt:
        out = next((n for n in nt.nodes if n.type == "OUTPUT_MATERIAL" and n.is_active_output), None) or \
              next((n for n in nt.nodes if n.type == "OUTPUT_MATERIAL"), None)
        sh = find_principled(out.inputs[0].links[0].from_node) if out and out.inputs[0].is_linked else None
        if sh is not None:
            info["shader"] = sh.type
            g = sh.inputs.get
            if sh.type == "EMISSION":
                info["emission"] = val(g("Color")); info["emissionStrength"] = val(g("Strength"))
                info["emissionTex"] = tex_ref(trace_image(g("Color")))
                info["baseColor"] = [0, 0, 0, 1]
            else:
                bc = g("Base Color") or g("Color")
                info["baseColor"] = val(bc)
                info["baseColorTex"] = tex_ref(trace_image(bc))
                for key, sock in (("roughness", "Roughness"), ("metallic", "Metallic"), ("alpha", "Alpha"),
                                  ("specular", "Specular IOR Level"), ("ior", "IOR"),
                                  ("emissionStrength", "Emission Strength"), ("transmission", "Transmission Weight"),
                                  ("coat", "Coat Weight"), ("sheen", "Sheen Weight"), ("subsurface", "Subsurface Weight")):
                    s = g(sock)
                    if s is not None:
                        info[key] = val(s)
                        t = tex_ref(trace_image(s))
                        if t: info[key + "Tex"] = t
                e = g("Emission Color") or g("Emission")
                if e is not None:
                    info["emission"] = val(e)
                    t = tex_ref(trace_image(e))
                    if t: info["emissionTex"] = t
                n = g("Normal")
                if n is not None and n.is_linked:
                    nn = n.links[0].from_node
                    if nn.type == "NORMAL_MAP":
                        info["normalStrength"] = val(nn.inputs.get("Strength"))
                        info["normalTex"] = tex_ref(trace_image(nn.inputs.get("Color")))
                    elif nn.type == "BUMP":
                        info["bumpStrength"] = val(nn.inputs.get("Strength"))
                        info["bumpDistance"] = val(nn.inputs.get("Distance"))
                        info["bumpTex"] = tex_ref(trace_image(nn.inputs.get("Height")))
                        nn2 = nn.inputs.get("Normal")
                        if nn2 is not None and nn2.is_linked and nn2.links[0].from_node.type == "NORMAL_MAP":
                            nm = nn2.links[0].from_node
                            info["normalStrength"] = val(nm.inputs.get("Strength"))
                            info["normalTex"] = tex_ref(trace_image(nm.inputs.get("Color")))
        if info.get("alphaTex") or (isinstance(info.get("alpha"), float) and info["alpha"] < 0.999) or \
                (out and out.inputs[0].is_linked and has_shader(out.inputs[0].links[0].from_node, "BSDF_TRANSPARENT")):
            info["alphaMode"] = "BLEND" if rm == "BLENDED" else "MASK"
    for k in [k for k, v in info.items() if v is None]:
        del info[k]
    materials[m.name] = info
    return m.name


# --------------------------------------------------------------------------- meshes
packs, pack_f, pack_i, pack_off = [], None, -1, 0


def pack_write(buf):
    global pack_f, pack_i, pack_off
    if pack_f is None or pack_off + len(buf) > PACK_LIMIT and pack_off > 0:
        if pack_f: pack_f.close()
        pack_i += 1
        name = "pack_%03d.bin" % pack_i
        packs.append(name)
        pack_f = open(os.path.join(OUT, name), "wb"); pack_off = 0
    off = pack_off
    pack_f.write(buf); pack_off += len(buf)
    return packs[-1], off


meshes, mesh_key, instances = [], {}, []
tri_total = inst_tri_total = 0


def export_mesh(ob):
    global tri_total
    try:
        me = ob.to_mesh()
    except RuntimeError:
        return None
    if me is None:
        return None
    try:
        nl, nv, np_ = len(me.loops), len(me.vertices), len(me.polygons)
        me.calc_loop_triangles()
        nt = len(me.loop_triangles)
        if nt == 0:
            return None
        tl = np.empty(nt * 3, np.int32); me.loop_triangles.foreach_get("loops", tl)
        tp = np.empty(nt, np.int32); me.loop_triangle_polygons.foreach_get("value", tp)
        pm = np.empty(np_, np.int32); me.polygons.foreach_get("material_index", pm)
        lv = np.empty(nl, np.int32); me.loops.foreach_get("vertex_index", lv)
        co = np.empty(nv * 3, np.float32); me.vertices.foreach_get("co", co)
        cn = np.empty(nl * 3, np.float32); me.corner_normals.foreach_get("vector", cn)
        uv = np.zeros(nl * 2, np.float32)
        if me.uv_layers.active is not None:
            me.uv_layers.active.uv.foreach_get("vector", uv)
        rows = np.empty((nl, 8), np.float32)
        rows[:, 0:3] = co.reshape(-1, 3)[lv]
        rows[:, 3:6] = cn.reshape(-1, 3)
        rows[:, 6:8] = uv.reshape(-1, 2)
        rows[:, 6:8] = np.round(rows[:, 6:8], 6)
        # weld identical corners
        key = np.ascontiguousarray(rows).view(np.dtype((np.void, 32))).ravel()
        _, first, inv = np.unique(key, return_index=True, return_inverse=True)
        verts = rows[first]
        idx = inv.astype(np.uint32)[tl].reshape(-1, 3)
        # slot -> material name
        slots = [s.material for s in ob.material_slots] or list(me.materials)
        tmat = pm[tp]
        order = np.argsort(tmat, kind="stable")
        idx = idx[order]; tmat = tmat[order]
        subs, start = [], 0
        for mi in np.unique(tmat):
            cnt = int((tmat == mi).sum())
            m = slots[mi] if 0 <= mi < len(slots) else None
            subs.append({"material": material(m), "start": start * 3, "count": cnt * 3})
            start += cnt
        p = verts[:, 0:3]
        rec = {"vertexCount": int(len(verts)), "indexCount": int(nt * 3), "submeshes": subs,
               "bounds": [p.min(0).tolist(), p.max(0).tolist()]}
        f, off = pack_write(verts.tobytes() + idx.astype(np.uint32).tobytes())
        rec["pack"], rec["offset"] = f, off
        tri_total += nt
        return rec
    finally:
        ob.to_mesh_clear()


def export_hair(ob):
    """hair / curves objects (grass, ivy strands): each strand becomes a flat ribbon as wide as its radius x2,
    facing up-ish, with UVs running along the strand (u across, v along) so card textures map like on hair cards"""
    global tri_total
    cv = ob.data
    try:
        pts = cv.points; crv = cv.curves
        npt = len(pts); ncv = len(crv)
    except Exception:
        return None
    if npt < 2 or ncv == 0:
        return None
    co = np.empty(npt * 3, np.float32); pts.foreach_get("position", co); co = co.reshape(-1, 3)
    rad = np.full(npt, 0.005, np.float32)
    try:
        pts.foreach_get("radius", rad)
    except Exception:
        pass
    off = np.empty(ncv + 1, np.int32)
    try:
        cv.curve_offset_data.foreach_get("value", off)
    except Exception:
        off = np.array([c.first_point_index for c in crv] + [npt], np.int32)
    verts, idx = [], []
    base = 0
    for c in range(ncv):
        a, b = int(off[c]), int(off[c + 1])
        if b - a < 2:
            continue
        P = co[a:b]
        T = np.gradient(P, axis=0); T /= np.maximum(np.linalg.norm(T, axis=1, keepdims=True), 1e-8)
        up = np.where(np.abs(T[:, 2:3]) > 0.9, np.array([[1, 0, 0]], np.float32), np.array([[0, 0, 1]], np.float32))
        side = np.cross(T, up); side /= np.maximum(np.linalg.norm(side, axis=1, keepdims=True), 1e-8)
        N = np.cross(side, T)
        w = np.maximum(rad[a:b], 0.002)[:, None]
        n = b - a
        v = np.linspace(0, 1, n, dtype=np.float32)[:, None]
        L = np.hstack([P - side * w, N, np.zeros((n, 1), np.float32), v])
        R = np.hstack([P + side * w, N, np.ones((n, 1), np.float32), v])
        verts.append(np.vstack([L, R]))
        for i in range(n - 1):
            l0, l1, r0, r1 = base + i, base + i + 1, base + n + i, base + n + i + 1
            idx += [l0, r0, l1, l1, r0, r1]
        base += 2 * n
        if base > 4_000_000:
            break
    if not idx:
        return None
    verts = np.vstack(verts).astype(np.float32)
    idx = np.array(idx, np.uint32)
    slots = [s.material for s in ob.material_slots] or list(cv.materials)
    m = slots[0] if slots else None
    nt = len(idx) // 3
    rec = {"vertexCount": int(len(verts)), "indexCount": int(len(idx)), "submeshes": [{"material": material(m), "start": 0, "count": int(len(idx))}],
           "bounds": [verts[:, 0:3].min(0).tolist(), verts[:, 0:3].max(0).tolist()], "hair": True}
    f, o = pack_write(verts.tobytes() + idx.tobytes())
    rec["pack"], rec["offset"] = f, o
    tri_total += nt
    return rec


GEOM = {"MESH", "CURVE", "SURFACE", "META", "FONT", "CURVES"}
lights = []


# --------------------------------------------------------------------------- render visibility
# The export reads the evaluated (viewport) scene, but what Blender renders can be much more: objects and collections
# hidden in the viewport for speed, particle systems showing a fraction of their instances, modifiers switched off
# in the viewport, lower subdivision levels. Switch everything to its render setting first (the .blend is never
# saved, so nothing changes on disk).
def match_render_settings():
    vl = bpy.context.view_layer
    n_ob = n_col = n_mod = n_ps = 0

    def walk(lc):
        nonlocal n_col
        if lc.exclude:
            return
        col = lc.collection
        if col.hide_render:            # not rendered: leave it out entirely
            try:
                lc.exclude = True
            except Exception:
                pass
            return
        try:
            if lc.hide_viewport or col.hide_viewport:
                n_col += 1
            lc.hide_viewport = False
            if col.hide_viewport and not col.library:
                col.hide_viewport = False
        except Exception as e:
            log("  note: collection %s: %s" % (col.name, e))
        for ch in lc.children:
            walk(ch)
    walk(vl.layer_collection)
    for ob in scene.objects:
        if ob.hide_render:
            continue
        try:
            if ob.hide_get(view_layer=vl) or ob.hide_viewport:
                n_ob += 1
            ob.hide_set(False, view_layer=vl)
            ob.hide_viewport = False
        except Exception:
            pass
        for m in ob.modifiers:
            try:
                if m.show_viewport != m.show_render:
                    m.show_viewport = m.show_render; n_mod += 1
                if m.type in ("SUBSURF", "MULTIRES") and hasattr(m, "render_levels"):
                    lv = "levels" if m.type == "SUBSURF" else "levels"
                    if getattr(m, lv) != m.render_levels:
                        setattr(m, lv, m.render_levels); n_mod += 1
            except Exception:
                pass
        for ps in getattr(ob, "particle_systems", []):
            st = ps.settings
            try:
                if st.display_percentage != 100: st.display_percentage = 100; n_ps += 1
                if st.child_type != "NONE" and st.child_percent != st.rendered_child_count:
                    st.child_percent = st.rendered_child_count; n_ps += 1
                if st.render_type in ("OBJECT", "COLLECTION") and st.display_method != "RENDER":
                    st.display_method = "RENDER"; n_ps += 1
            except Exception:
                pass
    log("render visibility: %d objects and %d collections unhidden, %d modifier / subdivision settings, %d particle settings set to render"
        % (n_ob, n_col, n_mod, n_ps))


if os.environ.get("KATARN_VIEWPORT_ONLY"):
    log("render visibility: skipped (KATARN_VIEWPORT_ONLY), exporting what the viewport shows")
else:
    try:
        match_render_settings()
    except Exception as e:
        import traceback
        log("render visibility failed, exporting what the viewport shows:\n" + traceback.format_exc())
log("evaluating the scene...")
bpy.context.view_layer.update()
dg = bpy.context.evaluated_depsgraph_get()
dg.update()
skipped_types = {}
scene = bpy.context.scene
n_inst = 0
last = time.time()
for inst in dg.object_instances:
    ob = inst.object
    orig = ob.original
    if not inst.is_instance:
        if orig.hide_render:
            continue
    elif inst.parent is not None and inst.parent.original.hide_render:
        continue
    M = [x for row in inst.matrix_world for x in row]
    if ob.type == "LIGHT":
        L = ob.data
        mw = inst.matrix_world
        d = (mw.to_3x3() @ __import__("mathutils").Vector((0, 0, -1))).normalized()
        rec = {"name": orig.name, "type": L.type, "color": list(L.color), "energy": float(L.energy),
               "position": list(mw.translation), "direction": list(d),
               "radius": float(getattr(L, "shadow_soft_size", 0.0)), "shadow": bool(getattr(L, "use_shadow", True))}
        if L.type == "SPOT":
            rec["spotSize"] = float(L.spot_size); rec["spotBlend"] = float(L.spot_blend)
        if L.type == "AREA":
            rec["shape"] = L.shape; rec["size"] = [float(L.size), float(getattr(L, "size_y", L.size))]
        if L.type == "SUN":
            rec["angle"] = float(L.angle)
        lights.append(rec)
        continue
    if ob.type not in GEOM:
        continue
    # one mesh record per evaluated object (shared by all instances of it)
    if inst.is_instance:
        key = ("I", ob.name, ob.data.name if ob.data else "")
    else:
        key = ("O", orig.name)
    mid = mesh_key.get(key)
    if mid is None:
        rec = export_mesh(ob)
        if rec is None and ob.type == "CURVES":
            rec = export_hair(ob)
        mid = -1
        if rec:
            rec["name"] = ob.name
            mid = len(meshes); meshes.append(rec)
        else:
            skipped_types[ob.type] = skipped_types.get(ob.type, 0) + 1
        mesh_key[key] = mid
    if mid < 0:
        continue
    instances.append({"mesh": mid, "matrix": M, "object": (inst.parent.original.name + "/" if inst.is_instance and inst.parent else "") + orig.name})
    inst_tri_total += meshes[mid]["indexCount"] // 3
    n_inst += 1
    if time.time() - last > 5:
        last = time.time()
        log("  %d instances, %d unique meshes, %.1fM unique tris, %.0fs" % (n_inst, len(meshes), tri_total / 1e6, time.time() - t0))
if pack_f: pack_f.close()
if skipped_types:
    log("objects with no surface to export (by type):", skipped_types)

# --------------------------------------------------------------------------- world, sun, camera
world = {}
w = scene.world
if w is not None:
    world["color"] = list(w.color)
    if w.use_nodes and w.node_tree:
        try:
            world["graph"] = dump_tree(w.node_tree)
        except Exception as e:
            log("  world graph dump failed", e)
        for n in w.node_tree.nodes:
            if n.type == "BACKGROUND":
                c = n.inputs.get("Color")
                world["color"] = val(c); world["strength"] = val(n.inputs.get("Strength"))
                t = trace_image(c)
                if t and t["image"]:
                    world["environment"] = export_image(t["image"])
            if n.type == "TEX_SKY":
                world["sky"] = {"type": n.sky_type}
                for a in ("sun_elevation", "sun_rotation", "altitude", "air_density", "dust_density", "ozone_density",
                          "sun_intensity", "sun_size", "turbidity"):
                    if hasattr(n, a): world["sky"][a] = float(getattr(n, a))
cam = None
if scene.camera is not None:
    c = scene.camera
    cam = {"matrix": [x for row in c.matrix_world for x in row], "lens": float(c.data.lens),
           "sensor": float(c.data.sensor_width), "type": c.data.type}

allb = [m["bounds"] for m in meshes]
out = {
    "format": "katarn-scene", "version": 2, "source": os.path.basename(blend), "blender": bpy.app.version_string,
    "units": "metre", "unitScale": scene.unit_settings.scale_length,
    "meshes": meshes, "instances": instances, "materials": materials, "lights": lights,
    "world": world, "camera": cam, "images": images, "packs": packs, "groups": groups,
    "view": {"exposure": float(scene.view_settings.exposure), "look": scene.view_settings.look,
             "transform": scene.view_settings.view_transform},
    "stats": {"uniqueTriangles": tri_total, "instancedTriangles": inst_tri_total, "instances": len(instances),
              "meshes": len(meshes), "materials": len(materials), "lights": len(lights), "images": len(images),
              "seconds": round(time.time() - t0, 1)},
}
json.dump(out, open(os.path.join(OUT, "scene.json"), "w"), indent=1)
log("done: %d meshes, %d instances, %.2fM unique / %.2fM drawn triangles, %d materials, %d lights, %d images, %d packs, %.0fs"
    % (len(meshes), len(instances), tri_total / 1e6, inst_tri_total / 1e6, len(materials), len(lights), len(images),
       len(packs), time.time() - t0))
logf.close()
