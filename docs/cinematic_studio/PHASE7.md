# Cinematic Studio: Phase 7 (Props: real map objects)

Status: **built** (Windows x64 game DLL), core **unit-tested**. **Not yet run in game.**

## What it does
Props are **real map entities**, spawned at runtime the way the level loader spawns them from a map's
key / values, so they behave like they would in a map: breakables break, items can be picked up,
effect runners run their effect, turrets shoot, `NPC_*` spawners spawn, `target_*` / `trigger_*` work
with `target` / `targetname`, movers move. Plus **models** on their own.

- **PROPS tab > ENTITY CLASSES:** every class the game can spawn (all map entities and every item /
  weapon / ammo / holocron), with a search box. Pick one, ADD KEY for its keys (`model`, `health`,
  `fxFile`, `spawnflags`, `target`, `targetname`, `count`, `wait`, `NPC_type`, `modelscale`... the same keys
  a map editor uses: "key value"), then SPAWN IN FRONT OF VIEW (or double-click it).
- **PROPS tab > MODELS:** a folder (`models/map_objects` by default, change it to browse others) and its
  `.md3` / `.glm` files (and one level of sub-folders). A model spawns as `cs_model`: just the model,
  not solid (`.glm` ones go through the game's `misc_model_ghoul`). Use `misc_model_breakable` with a
  `model` key for a solid / breakable one.
- **Prop inspector** (select it in the outliner, the viewport or its lane):
  - its keys, each with `x` to remove; SET KEY adds / changes one ("health 200"); changing keys spawns it
    again with them (undoable). If the new keys don't spawn, it keeps the old ones.
  - RESPAWN (after it broke / was picked up: the outliner shows "missing"), DUPLICATE, KEY (K).
  - **held by:** NEXT attaches it to an actor's bone: `*r_hand`, `*l_hand`, `*head_top`, `*head_front`,
    `*back`, `*chestg`, `*hip_r`, `*hip_l`... (NEXT cycles, TYPE for any bone / tag name), with an offset
    (X / Y / Z, SHIFT = 8) and a turn (pitch / yaw / roll, 15 or 45 degrees). It's placed on the bone
    every drawn frame, so it follows the hand exactly, also during takes. It warns when the bone isn't
    on that model.
- **Moving props:** drag in the viewport (CTRL+drag turns), arrows / PgUp / PgDn / Q / E, or **K** for
  keys: props use camera-style keys (position + pitch / yaw / roll) with the four interpolations, so a
  crate can fly, spin and land. Dragging a keyed prop keys it at the playhead.
- Props show in the viewport (green diamond, name, box when selected), the outliner ("P") and the timeline.
- Saved with the project (class, keys, prop keys, attachment); the project lists the models it needs.

## Modified / new files
| File | Change |
|---|---|
| `code/game/g_spawn.cpp` | `CS_SpawnMapEntity` (spawn any class from key / values at runtime), `CS_ListSpawnClasses` |
| `code/cgame/cs_props.cpp` (new) | spawning props / models, moving them, class and model lists, bone attachment poses, drawing attached props on the bone |
| `code/cgame/cg_ents.cpp` | attached props are drawn where the studio puts them this frame |
| `code/cgame/cs_core.h/.cpp` | prop objects (`AddProp`, `SetPropKey`, `SetAttach`; keys, undo, duplicate, save / load) |
| `code/cgame/cs_ui.cpp` | PROPS tab, prop inspector, props in the viewport / picking / dragging / keys |
| `code/game/CMakeLists.txt` | cs_props.cpp |
| `tests/cinematic_studio/test_core.cpp` | props: spawn, keys respawn / kept on failure, attach, keys, remove / undo, duplicate, save / load, clear |

## Known limitations
- Brush entities (`func_door`, `func_breakable`...) need a brush model of the level (`model *12`): any
  inline model the map has can be reused, but new brush shapes can't be made outside the map compiler.
- `misc_model` itself only exists in the compiled map; models use `cs_model` instead.
- Lights are compiled into the map; a dynamic light needs an entity that makes one (e.g. `fx_runner`
  with a light effect).
- An attached prop follows the bone where it's drawn; for the game (collisions, PVS) it's moved every frame.
