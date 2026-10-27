# Cape tools

- `patch_cape.py in.glm out.glm new_positions.npy [surface]` - write an edited cape shape (same mesh,
  same vertex order, e.g. re-exported from a Blender import of the .glm) into a model. LOD0 exactly;
  lower LODs get the displacement of their nearest LOD0 points. Normals recomputed.
  Check first that UV-seam copies of a point weren't pulled apart in the edit (they tear in game):
  weld them to their midpoint.
- `swap_cape.py in.glm in.gla mesh.pkl out.glm surf_for_mat0 [surf_for_mat1 ...]` - replace whole
  surfaces with a different mesh (bind pose, JKA units, weights by bone name), e.g. another model's
  cape. Every LOD gets the full new mesh.

Blender 5.x .blend files: read them with the `bpy` 5.x wheel (`uv venv -p 3.13; uv pip install bpy==5.2.3`),
and end scripts with `os._exit(0)` (the wheel crashes on normal shutdown).
