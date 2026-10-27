# Cinematic Studio: Phase 1 (Foundation)

Status: **built** for Windows x64 (MinGW cross-build of the SP executable and game DLL) and
**unit-tested** (core: 292 checks pass under AddressSanitizer/UBSan). **Not yet run in game.**

## What Phase 1 delivers
- An in-game studio over the live 3D view, opened with **F10** or the console command `studio`.
  The game keeps running (or freeze it with **P** / FREEZE WORLD).
- **Layout:** title bar with SAVE/X, the seven workspace tabs, a **scene outliner** on the left, the live
  **viewport**, a tab panel / **inspector** on the right, **transport** controls, and a **timeline** with a
  ruler, a playhead and one track per scene object.
- **Actors**
  - Spawn any NPC type in front of the view (ACTORS tab or `+ACTOR`). Studio actors are spawned
    cinematic: they hold their mark until you turn AI on.
  - Adopt level NPCs into the scene: click one in the viewport, then ADD TO SCENE.
  - Select actors in the outliner or by clicking them in the viewport; the selection is bracketed.
  - The inspector shows entity, model, skeleton (`_humanoid` profile), position, yaw, health, and the
    current legs and torso animation.
  - Move and turn the selected actor: arrows, PgUp/PgDn, Q/E, with SHIFT for x4 and CTRL for fine steps.
- **Cameras**
  - Add a scene camera from the current view.
  - Look through it or **pilot** it. The free camera is the right mouse button + WASD, with E/Q for
    up/down and the wheel for field of view. SET FROM VIEW and fov -/+ are in the camera inspector.
  - **V** or PLAYER VIEW returns to the player's view.
- **Transport**
  - Play/pause, stop, frame step (`,` `.`; SHIFT for a second), go to start/end.
  - 24/30/60 FPS, speed 0.1x to 2x, loop, scene length.
  - Timecode and frame counter. Click or drag on the ruler to scrub.
  - The cinematic clock is in microseconds and runs on real time, independent of the render frame rate.
- **Undo/redo** (CTRL+Z / CTRL+Y, or the buttons) covers add, delete, move/turn (nudges within a second
  merge into one step), rename and AI on/off.
- **Projects** (PROJECT tab or console)
  - Save, load and recover autosave, with a recent list.
  - Files go in `CinematicStudio/Projects/<name>/project.json` and `scenes/scene_01.json` (versioned schema).
  - The project lists the NPC types it needs. On load, missing ones become warnings and the actor stays
    in the scene, marked "(missing)", so saving again doesn't lose it.
  - Projects remember their map; loading on another map needs `force`.
  - Autosave every 2 minutes when changed, to `<project>/autosave/`.
- **Configurable hotkeys:** `CinematicStudio/hotkeys.cfg` is written with the defaults on first open; edit
  it to rebind (`<action> <KEY>` or `CTRL+<KEY>`).

## Console commands
`studio` (toggle) · `studio open|close` · `studio save [name]` · `studio load <name> [force]` ·
`studio recover [name]` · `studio new` · `studio spawn <npc>` · `studio camera` · `studio play|pause|stop` ·
`studio fps 24|30|60` · `studio undo|redo` · `studio freeze` · `studio list`

On first run the studio binds **F10** to `studio` (once; rebind it like any key).

## Modified source files
| File | Change |
|---|---|
| `code/client/vmachine.h` | cgame exports `CG_KEY_EVENT`, `CG_MOUSE_EVENT` (appended) |
| `code/client/cl_keys.cpp` | `CL_StudioWantsInput()`; key down/up and typed characters go to the studio first while `cs_active` is 1 (not when the console or a menu is open; the console key always works) |
| `code/client/cl_input.cpp` | mouse movement goes to the studio first |
| `code/client/client.h` | prototype |
| `code/cgame/cg_main.cpp` | vmMain cases for the new exports; `CS_Init` / `CS_Shutdown` |
| `code/cgame/cg_consolecmds.cpp` | `studio` command |
| `code/cgame/cg_view.cpp` | `CS_Frame` + `CS_ApplyView` after the view is built (the ICARUS camera is untouched) |
| `code/cgame/cg_draw.cpp` | the studio draws instead of the HUD while open |
| `code/game/NPC_spawn.cpp` | `CS_SpawnNPC()`: spawns an NPC type at a spot, cinematic behaviour |
| `code/game/CMakeLists.txt` | new files |

## New source files
| File | What |
|---|---|
| `code/cgame/cs_json.h/.cpp` | dependency-free JSON reader/writer |
| `code/cgame/cs_core.h/.cpp` | Scene Manager + Project Manager: objects, clock, selection, undo/redo, save/load/autosave (engine-independent, unit-tested) |
| `code/cgame/cs_host.cpp` | the game side: actors are real NPC entities, files through the game filesystem, entity queries |
| `code/cgame/cs_studio.h` | glue declarations |
| `code/cgame/cs_ui.cpp` | the editor: panels, input, studio camera, viewport picking, hotkeys |
| `tests/cinematic_studio/test_core.cpp`, `run.sh` | core unit tests |
| `docs/cinematic_studio/ARCHITECTURE.md` | audit, architecture, file plan |

## Building
- **MSVC (your usual build):** build the solution as before. The exe and the game DLL must both come
  from this branch: the exe sends the new cgame events.
- **MinGW cross-build (what I used):** CMake with `cmake/Toolchains/x86_64-w64-mingw32.cmake`,
  `-DBuildSPEngine=ON -DBuildSPGame=ON`. MinGW also needs three build-only shims that aren't in the repo:
  - `-fpermissive` (existing MD code relies on MSVC leniency);
  - an include shim for `VersionHelpers.h` / `ShlObj_core.h` (case and SDK name differences);
  - `_get_pgmptr` (missing from mingw-w64).
- **Tests:** `sh tests/cinematic_studio/run.sh`.

## Test procedure (in game)
1. Start a map (e.g. via the MD menu), open the console and check `cs_active` is 0, then press **F10**.
2. The panels appear and a cursor shows. Press TAB to hide/show the panels and F10 to close; gameplay controls come back.
3. ACTORS tab → click the NPC field, type `md_vader`, ENTER. Vader appears in front of the view, facing you, standing still.
4. Click him in the viewport: an orange bracket appears and the inspector shows his skeleton and animations.
   Move him with the arrows and Q/E. CTRL+Z undoes, CTRL+Y redoes.
5. Hold the right mouse button and fly with WASD. Press +CAMERA, fly elsewhere, then CAMERA tab → click
   the camera to view through it, or PILOT it to move it.
6. Press SPACE (play), click/drag the ruler (scrub), use `,` `.` (step), and the FPS/speed/LOOP buttons. Press P to freeze the world and P again to unfreeze.
7. PROJECT tab → type a name → SAVE. Then NEW → click again, then LOAD (or click it in RECENT): the actors respawn where they were.
8. Rename a project's NPC type in `scenes/scene_01.json` to something that doesn't exist and load it: you get a warning and the actor shows "(missing)".

## Known limitations (Phase 1)
- No keyframes yet. The clock and timeline run, but nothing is animated by them; every object shows
  one bar for the whole scene (Phase 4).
- No transform gizmos yet; moving is by keyboard (Phase 2).
- No drag-and-drop yet, and the panels are fixed: no docking or resizing.
- Actors turn about their up axis only; pitch and roll come from the animation.
- Freeze uses the game's own pause (`cl_paused`). Opening the Escape menu and closing it unfreezes.
- Level NPCs adopted into a project are respawned by NPC type when the project is loaded, not matched to the level's own entity.
- The studio owns the keyboard while open, so you can't play and edit at the same time. Live Gameplay
  mode is Phase 9.
- Only scene 1 per project for now; the format already lists scenes.

## Next (Phase 2: Actor Manager)
- An animation browser for the selected actor, built from its skeleton's `animation.cfg`: search,
  preview, scrub, speed, loop, freeze pose, torso and legs separately.
- Animation profile switching with skeleton validation (same bone list, frame ranges in range).
  Incompatible profiles are refused with an explanation; nothing is silently deformed.
- Mouse transform gizmos (move on a plane, turn), and duplicate.
- Model and skin choice per actor, saber and weapon state.
- Actor tracks on the timeline (position, animation), ready for Phase 4 keys.
