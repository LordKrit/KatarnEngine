"""Blender (bpy 5.2) -> Jedi Academy map pieces for Jedi_Room_V1.blend.

- applies every modifier (subdivision lowered to keep the BSP light: level 1, level 0 for tiny parts)
- writes one OBJ + MTL in Quake units (1 m = UNITS), each material as models/map_objects/<NAME>/<material>
- bakes each material's base colour (and emission where it glows) to a texture in its own UV space, so the
  Basic.png palette x RGB tint node setups come out exactly as Blender shows them
- dumps the active (day) lights and the room bounds to scene.json for the .map writer
"""
import bpy, bmesh, json, os, re, sys, math
from mathutils import Vector, Matrix

SRC = "/home/claude/jroom/Jedi_Room_V1.blend"
OUT = "/home/claude/jroom/pk3"
NAME = "jedi_room"
UNITS = 40.0                     # Quake units per metre (player is 64 units tall)
SKIP = {"Cube.018", "WindowVolumeDay", "WindowVolumeNight"}   # volumetric fog boxes
BIG_TEX = {"Floor", "Ceiling", "Door"}

bpy.ops.wm.open_mainfile(filepath=SRC)
sc = bpy.context.scene
mdir = os.path.join(OUT, "models", "map_objects", NAME)
os.makedirs(mdir, exist_ok=True)

def safe(n):
    return re.sub(r"[^a-z0-9_]+", "_", n.lower()).strip("_")

objs = [o for o in sc.objects if o.type == "MESH" and o.name not in SKIP and o.visible_get()]

# --- lighter subdivision for a lightmapped BSP
for o in objs:
    small = max(o.dimensions) < 0.15
    for m in o.modifiers:
        if m.type == "SUBSURF":
            m.levels = 0 if small else min(m.levels, 1)
            m.render_levels = m.levels
bpy.context.view_layer.update()
dg = bpy.context.evaluated_depsgraph_get()

# --- OBJ
used = {}
S = Matrix.Scale(UNITS, 4)
lines_v, lines_vt, lines_vn, faces_by_mat = [], [], [], {}
vbase = tbase = nbase = 0
tris_total = 0
for o in objs:
    e = o.evaluated_get(dg)
    me = e.to_mesh()
    me.calc_loop_triangles()
    mw = S @ o.matrix_world
    nmat = o.matrix_world.to_3x3().inverted().transposed()
    uvl = me.uv_layers.active.data if me.uv_layers.active else None
    for v in me.vertices:
        p = mw @ v.co
        lines_v.append("v %.4f %.4f %.4f" % (p.x, p.y, p.z))
    corner_normals = me.corner_normals
    for tri in me.loop_triangles:
        mat = o.material_slots[tri.material_index].material if tri.material_index < len(o.material_slots) else None
        mname = mat.name if mat else "default"
        used[mname] = mat
        f = []
        for li, vi in zip(tri.loops, tri.vertices):
            uv = uvl[li].uv if uvl else (0.0, 0.0)
            n = (nmat @ corner_normals[li].vector).normalized()
            lines_vt.append("vt %.6f %.6f" % (uv[0], uv[1]))
            lines_vn.append("vn %.4f %.4f %.4f" % (n.x, n.y, n.z))
            tbase += 1; nbase += 1
            f.append("%d/%d/%d" % (vbase + vi + 1, tbase, nbase))
        faces_by_mat.setdefault(mname, []).append("f " + " ".join(f))
        tris_total += 1
    vbase += len(me.vertices)
    e.to_mesh_clear()

shader_of = {m: "models/map_objects/%s/%s" % (NAME, safe(m)) for m in used}
with open(os.path.join(mdir, NAME + ".mtl"), "w") as f:
    for m in used:
        f.write("newmtl %s\nKd 1 1 1\nmap_Kd %s.png\n\n" % (safe(m), shader_of[m]))
with open(os.path.join(mdir, NAME + ".obj"), "w") as f:
    f.write("# %s from %s, %d triangles, 1 m = %g units\nmtllib %s.mtl\n" % (NAME, os.path.basename(SRC), tris_total, UNITS, NAME))
    f.write("\n".join(lines_v) + "\n" + "\n".join(lines_vt) + "\n" + "\n".join(lines_vn) + "\n")
    for m, fl in faces_by_mat.items():
        f.write("usemtl %s\n" % safe(m))
        f.write("\n".join(fl) + "\n")
print("OBJ triangles", tris_total, "materials", len(used))

# --- material analysis (emission / alpha) for the shader writer
def principled_nodes(mat):
    return [n for n in mat.node_tree.nodes if n.type == "BSDF_PRINCIPLED"] if mat and mat.node_tree else []

def emission_of(mat):
    if not mat or not mat.node_tree: return None
    for n in mat.node_tree.nodes:
        if n.type == "EMISSION":
            c = n.inputs["Color"].default_value
            return [c[0], c[1], c[2], n.inputs["Strength"].default_value]
        if n.type == "BSDF_PRINCIPLED":
            s = n.inputs["Emission Strength"].default_value
            if s > 0.0:
                ci = n.inputs["Emission Color"]
                c = ci.links[0].from_node.outputs[0].default_value if ci.is_linked and ci.links[0].from_node.type == "RGB" else ci.default_value
                return [c[0], c[1], c[2], s]
    return None

def alpha_of(mat):
    a = 1.0
    for n in principled_nodes(mat):
        i = n.inputs["Alpha"]
        if not i.is_linked: a = min(a, i.default_value)
        t = n.inputs.get("Transmission Weight")
        if t is not None and not t.is_linked and t.default_value > 0.5: a = min(a, 0.35)
    return a

# --- bake each material in UV space on a unit plane
sc.render.engine = "CYCLES"
sc.cycles.device = "CPU"
sc.cycles.samples = 8
sc.render.bake.margin = 4
bm_ = bpy.data.meshes.new("bakeplane")
bm = bmesh.new()
vs = [bm.verts.new(co) for co in ((0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0))]
face = bm.faces.new(vs)
uvlay = bm.loops.layers.uv.new("UVMap")
for loop, uv in zip(face.loops, ((0, 0), (1, 0), (1, 1), (0, 1))):
    loop[uvlay].uv = uv
bm.to_mesh(bm_); bm.free()
plane = bpy.data.objects.new("bakeplane", bm_)
sc.collection.objects.link(plane)
for o in sc.objects: o.select_set(False)
plane.select_set(True)
bpy.context.view_layer.objects.active = plane

matinfo = {}
for mname, mat in used.items():
    info = {"shader": shader_of[mname], "alpha": alpha_of(mat), "emission": emission_of(mat)}
    matinfo[mname] = info
    if mat is None or not mat.node_tree:
        continue
    res = 2048 if mname in BIG_TEX else 1024
    plane.data.materials.clear(); plane.data.materials.append(mat)
    passes = [("DIFFUSE", "", {"COLOR"})]
    if info["emission"] and max(info["emission"][:3]) > 0.0:
        passes.append(("EMIT", "_glow", set()))
    for btype, suffix, pf in passes:
        img = bpy.data.images.new("bake_" + safe(mname) + suffix, res, res, alpha=False)
        node = mat.node_tree.nodes.new("ShaderNodeTexImage")
        node.image = img
        for n in mat.node_tree.nodes: n.select = False
        node.select = True
        mat.node_tree.nodes.active = node
        try:
            if pf:
                bpy.ops.object.bake(type=btype, pass_filter=pf)
            else:
                bpy.ops.object.bake(type=btype)
            img.filepath_raw = os.path.join(mdir, safe(mname) + suffix + ".png")
            img.file_format = "PNG"
            img.save()
            info["tex" + suffix] = shader_of[mname] + suffix + ".png"
        except Exception as ex:
            print("BAKE FAILED", mname, btype, ex)
        mat.node_tree.nodes.remove(node)
    print("baked", mname, info)

# --- lights (active = day setup) and bounds
def to_q(v): return [round(c * UNITS, 2) for c in v]
lights = []
for o in sc.objects:
    if o.type != "LIGHT" or not o.visible_get(): continue
    L = o.data
    d = (o.matrix_world.to_3x3() @ Vector((0, 0, -1))).normalized()
    lights.append({"name": o.name, "type": L.type, "pos": to_q(o.matrix_world.translation),
                   "color": list(L.color), "energy": L.energy, "dir": list(d),
                   "radius": getattr(L, "shadow_soft_size", 0.0) * UNITS,
                   "spot": getattr(L, "spot_size", None) if L.type == "SPOT" else None})
bounds = {}
for o in objs:
    corners = [S @ o.matrix_world @ Vector(c) for c in o.bound_box]
    bounds[o.name] = [[min(c[i] for c in corners) for i in range(3)], [max(c[i] for c in corners) for i in range(3)]]
w = sc.world
world = None
if w and w.node_tree:
    rgbs = {n.name: list(n.outputs[0].default_value)[:3] for n in w.node_tree.nodes if n.type == "RGB"}
    mix = [n for n in w.node_tree.nodes if n.type == "MIX"]
    world = {"colors": rgbs, "night_factor": mix[0].inputs[0].default_value if mix else 0.0}
json.dump({"name": NAME, "units": UNITS, "triangles": tris_total, "materials": matinfo, "lights": lights,
           "bounds": bounds, "world": world}, open(os.path.join("/home/claude/jroom", "scene.json"), "w"), indent=1)
print("done")
