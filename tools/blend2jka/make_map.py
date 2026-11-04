#!/usr/bin/env python3
"""Writes maps/jedi_room.map + shaders/jedi_room.shader from scene.json (export_jka.py output).

Lighting, matching the Blender day setup:
  - the warm area light outside the window (Area.001) -> the sun: q3map_sun (baked, so the blinds' stripes land in
    the lightmap) + q3gl2_sun on the same direction, so rend2 casts the same sun as live shadows (r_sunlightMode 1
    darkens the baked light under characters, sabers and anything that moves through the shaft)
  - the four warm fill points -> light entities; the two wall lamps (emission 60 in Blender) -> light entities + glow
  - the cool blue Blender suns -> q3map_skylight ambient through the window
"""
import json, math, os
from PIL import Image

R = "/home/claude/jroom"
s = json.load(open(os.path.join(R, "scene.json")))
NAME = s["name"]
PK3 = os.path.join(R, "pk3")
mdir = os.path.join(PK3, "models", "map_objects", NAME)
os.makedirs(os.path.join(PK3, "shaders"), exist_ok=True)
os.makedirs(os.path.join(PK3, "maps"), exist_ok=True)

# ---------- shaders
SHELL = {"Wall01", "Wall02", "WallRED", "Ceiling", "Floor", "WoodMOG", "Door", "Grey Metal", "Grey Metal Light"}
out = ["// %s - generated from Jedi_Room_V1.blend (export_jka.py / make_map.py)\n" % NAME]
for mname, m in sorted(s["materials"].items()):
    sh = m["shader"]
    tex = m.get("tex", sh + ".png")
    lines = [sh, "{", "\tqer_editorimage %s" % tex]
    if mname in SHELL:
        # two-sided: the room is single-sided geometry, so the sun's shadow map has to see its back faces too
        lines.append("\tcull none")
    if m["alpha"] < 0.99:
        lines += ["\tsurfaceparm trans", "\tsurfaceparm nonsolid", "\tsurfaceparm nolightmap", "\tq3map_nonplanar",
                  "\tcull none", "\tsort seeThrough",
                  "\t{", "\t\tmap %s" % tex, "\t\tblendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA",
                  "\t\trgbGen lightingDiffuse", "\t\talphaGen const %.2f" % m["alpha"], "\t}", "}", ""]
        out.append("\n".join(lines)); continue
    emis = m["emission"]
    glow = emis and max(emis[:3]) > 0.01
    if glow:
        # solid emission colour for the glow pass (the baked EMIT pass is unreliable for Principled emission)
        c = [min(255, int(255 * x / max(emis[:3]))) for x in emis[:3]]
        Image.new("RGB", (16, 16), tuple(c)).save(os.path.join(mdir, os.path.basename(sh) + "_glow.png"))
        lines += ["\tq3map_surfacelight %d" % (1500 if emis[3] > 20 else 150),
                  "\tq3map_lightRGB %.3f %.3f %.3f" % tuple(x / max(emis[:3]) for x in emis[:3])]
    lines += ["\tq3map_nonplanar", "\tq3map_shadeAngle 60",
              "\t{", "\t\tmap $lightmap", "\t}",
              "\t{", "\t\tmap %s" % tex, "\t\tblendFunc GL_DST_COLOR GL_ZERO", "\t}"]
    if glow:
        lines += ["\t{", "\t\tmap %s_glow.png" % sh, "\t\tblendFunc GL_ONE GL_ONE", "\t\tglow", "\t}"]
    lines += ["}", ""]
    out.append("\n".join(lines))

# sun from the Blender area light outside the window
area = [l for l in s["lights"] if l["type"] == "AREA"][0]
d = area["dir"]                        # light travel direction
toward = [-d[0], -d[1], -d[2]]
yaw = math.degrees(math.atan2(toward[1], toward[0])) % 360
elev = math.degrees(math.asin(max(-1, min(1, toward[2]))))
sc = area["color"]
sky_col = s["world"]["colors"].get("Color.001", [0.9, 0.85, 0.8]) if s["world"] else [0.9, 0.85, 0.8]
out.append("""textures/%(n)s/sky
{
	qer_editorimage gfx/colors/white
	surfaceparm sky
	surfaceparm noimpact
	surfaceparm nolightmap
	surfaceparm nomarks
	q3map_sun %(r).3f %(g).3f %(b).3f 260 %(yaw).1f %(elev).1f
	q3map_skylight 45 4
	q3map_lightRGB 0.62 0.68 0.95
	q3gl2_sun %(r).3f %(g).3f %(b).3f 260 %(yaw).1f %(elev).1f 1.0 0.55
	skyparms - 512 -
	{
		map $whiteimage
		rgbGen const ( %(sr).3f %(sg).3f %(sb).3f )
	}
}

textures/%(n)s/clip
{
	qer_trans 0.4
	surfaceparm nodraw
	surfaceparm nolightmap
	surfaceparm trans
	surfaceparm nomarks
}
""" % {"n": NAME, "r": sc[0], "g": sc[1], "b": sc[2], "yaw": yaw, "elev": elev,
       "sr": sky_col[0], "sg": sky_col[1], "sb": sky_col[2]})
open(os.path.join(PK3, "shaders", NAME + ".shader"), "w").write("\n".join(out))

# ---------- map
def brush(mins, maxs, shader):
    x0, y0, z0 = mins; x1, y1, z1 = maxs
    t = "%s 0 0 0 0.5 0.5 0 0 0" % shader
    planes = [
        ((x0, y0, z1), (x0, y1, z1), (x1, y1, z1)),   # top
        ((x0, y1, z0), (x0, y0, z0), (x1, y0, z0)),   # bottom
        ((x0, y0, z0), (x0, y1, z0), (x0, y1, z1)),   # west
        ((x1, y1, z0), (x1, y0, z0), (x1, y0, z1)),   # east
        ((x1, y0, z0), (x0, y0, z0), (x0, y0, z1)),   # south
        ((x0, y1, z0), (x1, y1, z0), (x1, y1, z1)),   # north
    ]
    b = ["{"]
    for p in planes:
        b.append("( %g %g %g ) ( %g %g %g ) ( %g %g %g ) %s" % (p[0] + p[1] + p[2] + (t,)))
    b.append("}")
    return "\n".join(b)

B = s["bounds"]
fl = B["Plane"]            # floor slab: its extents are the room's inner walls
ce = B["Plane.001"]
ix0, iy0 = fl[0][0], fl[0][1]
ix1, iy1 = fl[1][0], fl[1][1]
floor_z = fl[1][2]
ceil_z = ce[0][2]
brushes = []
# sky shell around everything (seals the map; the sun and skylight come in through the window)
allmin = [min(v[0][i] for v in B.values()) for i in range(3)]
allmax = [max(v[1][i] for v in B.values()) for i in range(3)]
pad = 96
lo = [math.floor(allmin[0] - pad), math.floor(allmin[1] - pad), math.floor(allmin[2] - pad)]
hi = [math.ceil(allmax[0] + pad), math.ceil(allmax[1] + pad * 2), math.ceil(allmax[2] + pad)]
T = 16
sky = "%s/sky" % NAME
brushes += [brush((lo[0] - T, lo[1] - T, lo[2] - T), (hi[0] + T, hi[1] + T, lo[2]), sky),
            brush((lo[0] - T, lo[1] - T, hi[2]), (hi[0] + T, hi[1] + T, hi[2] + T), sky),
            brush((lo[0] - T, lo[1] - T, lo[2]), (lo[0], hi[1] + T, hi[2]), sky),
            brush((hi[0], lo[1] - T, lo[2]), (hi[0] + T, hi[1] + T, hi[2]), sky),
            brush((lo[0], lo[1] - T, lo[2]), (hi[0], lo[1], hi[2]), sky),
            brush((lo[0], hi[1], lo[2]), (hi[0], hi[1] + T, hi[2]), sky)]
# invisible collision (the visible room is a model)
clip = "%s/clip" % NAME
W = 12
brushes += [brush((ix0 - W, iy0 - W, floor_z - W), (ix1 + W, iy1 + W, floor_z), clip),     # floor
            brush((ix0 - W, iy0 - W, ceil_z), (ix1 + W, iy1 + W, ceil_z + W), clip),       # ceiling
            brush((ix0 - W, iy0, floor_z), (ix0, iy1, ceil_z), clip),                      # west wall
            brush((ix1, iy0, floor_z), (ix1 + W, iy1, ceil_z), clip),                      # east wall
            brush((ix0 - W, iy0 - W, floor_z), (ix1 + W, iy0, ceil_z), clip),              # south wall (door)
            brush((ix0 - W, iy1, floor_z), (ix1 + W, iy1 + W, ceil_z), clip)]              # north wall (window)
bed = B["BedMat02"]; bed1 = B["BedMat01"]
brushes.append(brush((bed[0][0], bed[0][1], floor_z), (bed[1][0], bed[1][1], max(bed[1][2], bed1[1][2])), clip))

ents = []
ws = ['"classname" "worldspawn"', '"message" "Jedi Room"', '"gridsize" "32 32 32"', '"_ambient" "4"',
      '"_color" "0.62 0.68 0.95"']
ents.append("{\n" + "\n".join(ws) + "\n" + "\n".join(brushes) + "\n}")
ents.append('{\n"classname" "misc_model"\n"model" "models/map_objects/%s/%s.obj"\n"origin" "0 0 0"\n"spawnflags" "4"\n"_ls" "0.125"\n"_cs" "1"\n"_rs" "1"\n}' % (NAME, NAME))

def light(pos, color, value, extra=""):
    return '{\n"classname" "light"\n"origin" "%g %g %g"\n"light" "%d"\n"_color" "%.3f %.3f %.3f"\n%s}' % (
        pos[0], pos[1], pos[2], value, color[0], color[1], color[2], extra)

for l in s["lights"]:
    if l["type"] == "POINT":
        ents.append(light(l["pos"], l["color"], int(l["energy"] * 22)))
# the wall lamps glow at strength 60 in Blender and light the room; put a light just in front of each lamp
for lamp, nudge in (("LIGHT01", -10), ("LIGHT02", 10)):
    b = B[lamp]
    c = [(b[0][i] + b[1][i]) / 2 for i in range(3)]
    c[0] += nudge
    ents.append(light(c, (0.89, 0.75, 0.46), 170))
# player start on the open floor, facing the bed and the window
ents.append('{\n"classname" "info_player_start"\n"origin" "%g %g %g"\n"angle" "45"\n}' % (ix0 + 30, iy0 + 40, floor_z + 25))
open(os.path.join(PK3, "maps", NAME + ".map"), "w").write("\n".join(ents) + "\n")
print("sun yaw %.1f elev %.1f, %d brushes, %d entities" % (yaw, elev, len(brushes), len(ents)))
