# Rebuilding BF2 Dooku (dooku_bf2)

Inputs (not in the repo - game assets): `dooku4.blend` (BF2 Dooku), Movie Duels'
`models/players/_humanoid_dooku/` (untouched) and `models/players/dooku/model.glm` (MD Dooku, for tags).

1. Blender export (meshes + LODs): `blender -b dooku4.blend --python blender/export_i.py`
   -> dooku4.glb, dooku4_lod1..3.glb
2. Rig + skeleton + animations:
```
python3 jka_retarget.py rig dooku4.glb --gla _humanoid_dooku/_humanoid.gla --map dooku_bf2_map.json \
  --name dooku_bf2 --tags dooku/model.glm --cloth-mesh cape --fingers --arm-ik --relax-left \
  --stock-hand-tags --ground-drop 1.88 --extra-bones '^(eyeball_|FACIAL_(?!.*_end$))' \
  --unbound-bones '^FACIAL_LOD1_[LR]_(Eyeball|Pupil)$' --max-verts 4000 --max-tris 8000 \
  --lod dooku4_lod1.glb --lod dooku4_lod2.glb --lod dooku4_lod3.glb --out zzz_dooku_bf2.pk3
```
   (RGRIP env = saber-hand grip scale, default 1.0)
3. Face file: `python3 face_tool.py <built _humanoid.gla> ext_data/faces/dooku_bf2.face`
4. Textures, rend2 shader (normal/rmo maps, `skin` keyword), teeth shader and the NPC file
   (copied from md_dooku with playerModel dooku_bf2) go in the same pk3.
5. Cape settings: `ext_data/meshcloth.cfg` (dooku_bf2 lines, `above 50 46.5`).

Preview helpers: hiltview.py (saber in hand vs MD Dooku), facepose.py (face poses), preview.py.
