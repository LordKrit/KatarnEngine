# blend2jka - Blender scene -> Jedi Academy map (WzMap)

Used for Jedi_Room_V1.blend (Blender 5.2 file).

1. `run_bpy.sh export_jka.py` - bpy 5.2 (pip `bpy==5.2.3` in /home/claude/bpyenv; clean env avoids a glog abort):
   applies modifiers (subdivision capped at 1, 0 for tiny parts), writes `models/map_objects/<name>/<name>.obj/.mtl`
   in Quake units (1 m = 40), bakes each material's base colour (and emission) in its UV space on a unit plane
   (works for Basic.png-palette x RGB-tint node setups), dumps active lights + bounds to scene.json.
2. `make_map.py` - shaders (lightmap x baked texture; room shell `cull none` so rend2's sun shadow map sees back
   faces; glass blended; emissive = surfacelight + glow stage) and the .map: sky shell (seal), invisible clip
   collision, misc_model (`spawnflags 4` = lightmapped, `_ls 0.125`), lights, player start.
   Sun = the Blender area light outside the window: `q3map_sun` (baked) + `q3gl2_sun` same direction (rend2 live
   sun shadows, r_sunlightMode 1 darkens the baked light under moving things).
3. `compile.sh` - WzMap.exe through wine (BSP -meta, VIS, LIGHT -fast -samples 2 -bounce 1), cwd = the base dir
   so assimp finds the .mtl. 4. `package.sh` - zz_<name>.pk3.

WzMap fix needed for OBJ materials: pm_assimp FS_TextureFileExists returns only the extension, so shader names
became "png" - fixed in tools/sojourn/wzmap/wzmap_mingw.patch.
