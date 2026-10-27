# Mesh cloth for Movie Duels (SP renderers)

Simulates the real vertices of chosen Ghoul2 surfaces (capes, robes, skirts, sleeves, hoods) as cloth.
Nothing extra is drawn: the model's own cape moves. It works in both SP renderers (vanilla and rend2)
and is driven by a text config, so modders add cloth to a model without touching code.

This branch is upstream `master` plus mesh cloth only. Nothing else from our fork is included.

## Files

| File | What it is |
|---|---|
| `code/rd-vanilla/tr_meshcloth.cpp/.h` | The simulator. Engine-neutral: no GL, no renderer types. Both renderers compile it. |
| `code/rd-vanilla/tr_ghoul2.cpp` | Vanilla glue: `RB_MeshClothApply` CPU-skins a cloth surface, runs the sim, writes the vertices before drawing. Body colliders, floor trace, hilt colliders. |
| `shared/rd-rend2/tr_ghoul2.cpp` | rend2 glue: same inputs; cloth surfaces are skinned on the CPU and drawn from their own dynamic VBO (`RB_MeshClothDraw`). Everything else stays GPU-skinned. |
| `code/rd-*/tr_init.cpp`, `tr_local.h` | Cvars, `meshcloth_blast` test command, config load at `R_Init`, cleanup at `RE_Shutdown`. |
| `code/rd-*/CMakeLists.txt` | Adds `tr_meshcloth.cpp` to both renderers. |
| `docs/meshcloth/meshcloth.cfg.example` | The config we ship (Palpatine, Vader, Dooku, ...). Goes in a pk3 as `ext_data/meshcloth.cfg`. |

## How it works

- **Groups.** All cloth surfaces of one model LOD are simulated together as one cloth, so pieces stitched
  together (robe plus leg panels) can't separate. Any cloth vertex that touches a non-cloth surface stays attached.
- **Solver.** XPBD (Macklin, Mueller & Chentanez 2016), fixed 16 ms steps with substeps (`r_meshClothIter`).
  Stretch and bend are compliant constraints. Compliance comes from the config's `bend` and `stretch`
  values and is calibrated so 6 substeps match the older position-projection solver.
  `r_meshClothXPBD 0` switches back to plain PBD.
- **Attachments.** Rules per surface (`above`, `near`, `segment`) give each vertex a freedom from 0 to 1 in
  the bind pose. Long-range attachments, a soft tether and an optional reach limit keep cloth from flying off.
- **Collision.**
  - Capsules from `_humanoid` bones: pelvis, spine, neck, head, legs, arms, shoulders, plus a short fist capsule.
  - The body's real shape (`r_meshClothBodyFit`): on first use each model's own body mesh (every non-cloth
    surface) is cut into height slices per torso, hip, leg, neck and head bone, and each slice becomes a
    "stadium" (flat front and back, round sides) reaching out to 95% of the mesh. These ride their bones and
    push hanging cloth fully out, so capes and robes rest on the actual torso, belt, armour and legs instead
    of on a few round tubes. They add to the capsules above (arms keep their tuned capsules), cost about
    15-20% more and are fitted once per model.
  - A saber hilt capsule in each hand, from the `*r_hand` / `*l_hand` bolt tags.
  - A floor plane traced under the character, and wall planes from 8 chest-height traces round it
    (`r_meshClothWalls`), so capes don't swing through walls. The drawn cloth is kept in front of them too.
  - Body capsules are swept through the substeps (last frame's pose to this frame's), so a fast saber arm
    can't jump clean through the cape between two checks, even at low frame rates.
  - Optional self-collision.
  - Cloth edges are also tested against arms, fists and hilts, so thin limbs can't slip between points on coarse cloth.
  - Contacts get Coulomb friction (static and kinetic).
- **Character motion.** Inertia scale, a max felt acceleration (Battlefront II style `maxaccel`), a spin
  limit for fast saber spins, air drag, and wind with gusts.
- **Dense meshes.** `proxy N` simulates a coarse grid of about N of the cloth's own vertices. Every drawn vertex
  follows the grid points around it, and the result is smoothed and pushed off limbs.
- **Cost control.**
  - Distance LOD: beyond `r_meshClothLodDist` half the substeps and no self-collision (about a third cheaper).
  - Beyond `r_meshClothCullDist` no simulation at all: the animated surface is drawn (in rend2 the normal
    GPU-skinned path). The cloth settles afresh, out of sight, when the character comes back into range.
  - Sleep (`r_meshClothSleep`): cloth at rest on a character that isn't moving stops simulating until the pose
    drifts, the wind blows or a blast hits it. A standing NPC costs about half as much.
  - `r_meshClothStats 1` prints characters, simulations, LOD, sleeping, culled, steps and time once a second.
- **Safety.** Teleports and stale instances reset the cloth. NaNs fall back to the animation.
  Instances are keyed per character and pruned after 10 s unseen.

## Config: `ext_data/meshcloth.cfg`

One line per surface: `model surface rule... [options]`. The model is the folder name under
`models/players/`. Coordinates are the model's bind pose (z up, feet at 0).

Rules:
- `above Zpin Zfree`: attached at or above Zpin, fully free at or below Zfree.
- `near x y z Rpin Rfree`: attached within Rpin of the point, free beyond Rfree.
- `segment x0 y0 z0 x1 y1 z1 Rpin Rfree`: the same, measured from a line (shoulder to wrist).

Options:

| Option | Meaning | Default |
|---|---|---|
| `tether T` | how far a free vertex may move from its animated spot (units) | 12 |
| `bend B` | 0 to 1 bending stiffness (0 silk, 1 leather) | 0.3 |
| `stretch S` | 0 to 1 XPBD stretch give (0 rigid) | 0 |
| `inertia I` | 0 to 1 share of the character's motion the cloth feels | cvar |
| `maxaccel A` | units/s² of character acceleration felt, 0 = no limit | cvar |
| `maxspin D` | deg/s the attached edge may turn before the cloth is carried with it | off |
| `spread S` | 0 to 1 sideways pull toward the animated shape (stops capes roping) | 0 |
| `reach R` | max stray = R × distance along the cloth from the attached edge | off |
| `turn T` | 0 to 1 share of the attached edge's turning applied directly | 0 |
| `shape S` | 0 to 1 pull toward the modelled shape | 0 |
| `self` | self-collision for single-layer capes | off |
| `thickness T` | units kept off the body (half) and off itself (full) | 1.2 |
| `weight W` | mass per area: 2 = heavy wool, 0.5 = silk | 1 |
| `friction F` | 0 to 1 kinetic friction on body and floor | 0.3 |
| `sfriction S` | static friction: slower slides than S × press stick | 0.5 |
| `lift L` / `lift off` | units a free point may rise above its attached edge | 2 |
| `smooth S` | 0 to 1 crease smoothing of the drawn shape | 0.5 |
| `proxy N` | simulate a coarse grid of about N points | off |

Example (cape on the shoulders, cloth from mid-back down):

```
dooku_bf2 cape above 50 46.5 tether 40 bend 0.7 inertia 1 self maxaccel 790 thickness 2 proxy 400
```

Note: a pk3 that carries its own `ext_data/meshcloth.cfg` replaces the whole file. Keep all lines in one pack.

## Cvars

| Cvar | Default | |
|---|---|---|
| `r_meshCloth` | 1 | on/off |
| `r_meshClothIter` | 6 | substeps per 16 ms |
| `r_meshClothXPBD` / `r_meshClothXPBDIter` | 1 / 2 | XPBD on, solver iterations per substep |
| `r_meshClothGravity` | 650 | units/s² |
| `r_meshClothDrag` / `r_meshClothAirDrag` | 1.4 / 2.5 | drag through the cloth / along it |
| `r_meshClothWind` / `r_meshClothGust` | "0 0 0" / 0.5 | world wind (units/s), gust amount |
| `r_meshClothInertia` / `r_meshClothMaxAccel` | 1 / 0 | defaults for surfaces that don't set them |
| `r_meshClothShape` | 1 | scales every surface's `shape` |
| `r_meshClothFriction` | 1 | scales all friction (0 = frictionless) |
| `r_meshClothLift` | -2 | -2 = per surface, -1 = no lift limit, N = units |
| `r_meshClothHilt` | 1 | saber hilt colliders on/off |
| `r_meshClothHiltLen` / `Back` / `Radius` | 10 / 4 / 1.8 | hilt capsule past and behind the fist, thickness |
| `r_meshClothWalls` | 1 | wall collision on/off |
| `r_meshClothBodyFit` | 1 | collision with the body's real shape (fitted from the mesh) on/off |
| `r_meshClothLodDist` / `r_meshClothCullDist` | 800 / 3000 | reduced detail / no simulation beyond these distances (0 = off) |
| `r_meshClothSleep` | 1 | resting cloth on still characters stops simulating |
| `r_meshClothStats` | 0 | 1 = print cost and counts once a second |

Commands:
- `meshcloth_blast [strength] [radius]` pushes all cloth near a point 96 units in front of the camera, like an explosion.
- `meshcloth_reload` re-reads `ext_data/meshcloth.cfg` without restarting, for tuning cloth lines live.

## Notes for integrating

- The simulator only needs, per surface: the surface's bind-pose vertices, a bone-matrix callback, the entity
  origin and axis, the capsules, and `mcParams_t`. See `RB_MeshClothApply` (vanilla) for the smallest glue.
- In rend2, cloth surfaces bypass the GPU-skinned VBO and use a small per-character dynamic VBO. One
  `qglBufferSubData` per cloth surface per frame, reused for shadow and extra views.
- Cost is roughly 1 to 2 ms per frame per heavily-simulated cape on one core. Grid proxies make dense meshes cheap.
- Upstream stock models have no cloth until a config line names their surfaces.
