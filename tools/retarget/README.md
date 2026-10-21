# jka_retarget: any skeleton → JKA / Movie Duels `_humanoid`

An offline converter, so it works with any Movie Duels build and needs no engine changes. It does two jobs:

- **Models:** a rigged character from glTF/GLB, FBX, DAE, SMD, X, … becomes a `.glm` on the `_humanoid`
  skeleton. Every JKA/MD animation then plays on it, sabers bolt to its hands, dismemberment
  works, and its high-poly detail is kept.
- **Animations:** animations from the same kinds of files are retargeted onto `_humanoid` and appended
  to a `.gla`, with `animation.cfg` lines that point Movie Duels animations (BOTH_RUN1, BOTH_STAND1, …)
  at them.

Tested with:
- **Mixamo:** Soldier, Michelle, X Bot.
- **A plain custom rig:** CesiumMan.
- **Renamed copies:** Unreal mannequin names, Valve biped names.
- **Formats:** GLB, plus FBX and DAE through assimp.

## Install
```
pip install numpy pillow pygltflib
```
For FBX/DAE/SMD you also need the `assimp` command-line tool:
- Windows: the assimp release zip from GitHub, or `vcpkg install assimp`, with `assimp.exe` on your PATH.
- Linux: `apt install assimp-utils`.

glTF/GLB works without it. In Blender, *File → Export → glTF 2.0* is the most reliable route.

## Use
```
# 1. check the bone mapping
python jka_retarget.py map  hero.fbx --gla _humanoid.gla

# 2. model -> pk3 (glm, skin, textures, NPC file)
python jka_retarget.py model hero.fbx --gla _humanoid.gla --name hero --out zz_hero.pk3 \
       --tags models/players/kyle/model.glm --anim-name models/players/_humanoid/_humanoid

# 3. animations -> new .gla + animation.cfg lines
python jka_retarget.py anim mocap.fbx --gla _humanoid.gla --out _humanoid.gla.new \
       --take "Idle=BOTH_STAND1" --take "Run=BOTH_RUN1" --loop --cfg animation_new.cfg
```

**Options:**

| Option | What it does |
|---|---|
| `--gla` | The target skeleton. Use Movie Duels' own `models/players/_humanoid/_humanoid.gla`, or a variant such as `_humanoid_vader`. |
| `--tags` | Any existing model on the same skeleton. Its bolt tags (`*r_hand`, `*l_hand`, `*back`, `*chestg`, `*head_*`, `*hip_*`, …) are copied, so sabers, holsters and effects attach. Repeat it to fill missing tags from another model. |
| `--anim-name` | The skeleton path stored in the model (default: the name inside the `.gla`). |
| `--map bones.json` | Fixes or extends the automatic mapping, e.g. `{"lclavical": "L_Collar", "upper_lumbar": null}`. |
| `--max-verts` | Vertices per surface (default 1000, the stock engine limit). Surfaces are split automatically, also at 32 bones each. |
| `--root-motion` | Keeps the source's forward drift in animations. The default removes it, because JKA moves the character itself. |

The new frames are appended after the existing ones, which are left byte-for-byte unchanged. The
`animation.cfg` lines replace whichever animation names you choose.

## How it works
1. **Loading:** the source is read through glTF (other formats go through assimp first), with the bind
   pose taken from the inverse bind matrices.
2. **Bone mapping, by structure:**
   - The hips are the joint where two legs and a spine branch off.
   - The chest is the joint where two arms and a neck branch off.
   - Each limb follows its longest chain, skipping twist, roll, helper and IK bones.
   - The hand is where the fingers split off.
   - Names are only used for left/right and for which finger is which. Without names, left and right
     come from the direction the toes point, so any naming scheme works.
3. **Space and scale:** the conversion to JKA space comes from the skeleton itself (up = hips→head,
   side = right→left arm), so Y-up/Z-up, metres/centimetres and facing direction don't matter. The
   scale is set from the character's height.
4. **Fitting the model:** each bone segment is rotated onto the `_humanoid` base pose (A-pose) and
   stretched along its length to the JKA bone length. Joints land exactly on the JKA skeleton, so JKA
   animations bend the mesh at the right places. Weights move to the matching JKA bones. Bones with no
   JKA equivalent (twist, extra fingers, accessories) follow their nearest mapped parent.
5. **Surfaces:** split into JKA body parts (`hips`, `torso`, `head`, `l_arm`, `r_arm`, `l_hand`, `r_hand`,
   `l_leg`, `r_leg`) with the standard hierarchy, so dismemberment and the game's surface code see a
   normal model.
6. **Retargeting animations:** world-space rotation deltas from the bind pose, corrected by the same
   per-bone alignment, plus the hips translation. Results are written in Ghoul2's compressed
   quaternion format.

## Known limits
- **One LOD.** It's written as the full-detail model.
- **Proportions:** limb lengths are fitted to JKA's. Very stylised proportions (huge heads, long arms)
  stretch or squash towards the JKA ones.
- **Unmapped detail:** fingers beyond thumb, index and ring, and face, tail and cape bones, become
  rigid parts of their parent. Cloth is better handled by the mesh cloth system.
- **Textures** are copied as PNG/JPG with the base colour only. Normal and specular maps need a shader.
