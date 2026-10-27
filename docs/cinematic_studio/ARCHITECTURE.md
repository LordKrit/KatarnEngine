# JK BUILD: Cinematic Studio — audit, architecture and implementation plan

Branch `cinematic-studio`, forked from the Movie Duels fork `renderer-limits` at `f6443d2`.

## 1. Audit of the existing code (what's already there)

| # | System | Where | Finding |
|---|---|---|---|
| 1 | Cinematic camera | `code/cgame/cg_camera.cpp` (1940 lines) | Only the stock ICARUS script camera: `CGCam_Move/Pan/Zoom/Roll/Follow/Track/Shake/Fade`, ROFF playback, `cam_enable` / `cam_disable` debug commands. **No Catmull-Rom path tool exists in this source.** An earlier, separate experiment (`GameData\Launch_Cinematic_Editor.cmd`, `fs_game cine_editor`, built on plain OpenJK) had camera interpolation, but its source isn't in this repo or on the computer. The studio's camera paths are therefore new code; the ICARUS camera is left untouched so map scripts and cutscenes keep working. |
| 2 | Actors / NPCs | `code/game/NPC_spawn.cpp` (`Svcmd_NPC_f`, `NPC_Spawn_Do`), `NPC_stats.cpp`, `NPC*.cpp`, `b_*.cpp` | SP game and cgame are in **one DLL** (`MovieDuels-gamex86_64.dll`): cgame reads `g_entities` directly (`cent->gent`). The studio can therefore use authoritative server-side entity state for selection, recording and playback without a network protocol. NPCs spawn by NPC type (`npc spawn <type>`), with MD's NPC files in `ext_data/npcs/*.npc`. |
| 3 | `_humanoid` resolution | `NPC_stats.cpp` `G_ParseAnimFileSet(skeleton, model)` | The `.glm` names its skeleton (`animName`, e.g. `models/players/_humanoid_vader/_humanoid`). The game loads `animation.cfg` from that `.gla`'s folder and caches it in `level.knownAnimFileSets`; each client has `clientInfo.animFileIndex`. Animation IDs (`animNumber_t`, `animtable.h`) are shared, but **frame ranges are per skeleton**. Swapping profiles only works between `.gla`s with the same bone list; otherwise it needs retargeting (`tools/retarget`). The studio validates bone lists before allowing a switch (Phase 2). |
| 4 | Ghoul2 animation | `code/game/G2*`, `code/rd-*/G2_*`, `gi.G2API_SetBoneAnim(Index)` | Legs/torso animations run through `PM_SetAnim` → `G_SetAnim` → `G2API_SetBoneAnimIndex` on the `model_root` / `lower_lumbar` bones with blend times. Bone overrides (`G2API_SetBoneAngles`) exist for head/torso looks and are what the posing/face code uses. |
| 5 | CineAudio | `code/cgame/cg_face.cpp` `CG_CineAudio_f` | `cineaudio play <file> [who]` / `stop`: plays a sound as a character's voice with lip sync from the sound's amplitude. Timeline audio tracks (Phase 8) will drive this. |
| 6 | Facial expressions | `code/cgame/cg_face.cpp`, `ext_data/faces/profiles.txt`, `.face` files | Face life: blinks, saccades, expressions and lip sync for models with a face rig (currently dooku_bf2). Models without one are skipped. Expression tracks (Phase 8) will feed this. |
| 7 | Replay / demo | — | **SP has no demo recording.** `code/client` has no demo code; `sv_savegame.cpp` serializes the full game state (used by save games), which is a candidate for replay **checkpoints** (Phase 5). |
| 8 | UI / menus | `code/ui` (in the exe), `.menu` files in `ui/`; cgame 2D drawing (`CG_FillRect`, `cgi_R_Font_DrawString`) | The SP menu system **pauses the game while open** (`cl_paused`), so it can't host a live editor. The studio is drawn by cgame over the 3D view, with its own input route (below). |
| 9 | Input | `code/client/cl_keys.cpp`, `cl_input.cpp` | SP only has CONSOLE and UI key catchers; cgame never sees keys or the mouse. Phase 1 adds a studio input route to cgame (`CG_KEY_EVENT`, `CG_MOUSE_EVENT`). |
| 10 | Pause | `cl_paused` | Freezes the server (the world) while the client keeps drawing: used for the studio's "Freeze world". |

### Risks
- **Exe ↔ DLL interface**: new cgame exports (`CG_KEY_EVENT`, `CG_MOUSE_EVENT`) are appended to `cgameExport_t`. The exe and game DLL from this branch must be used together (as with any MD build).
- **Replay determinism**: NPC AI uses `Q_irand` / `random()` and frame-time-dependent physics; re-simulation won't reproduce a fight. Replay must play back recorded state (Phase 5), not re-run AI.
- **Skeleton mismatch**: forcing an animation profile onto an incompatible skeleton corrupts poses. Validate first (Phase 2).
- **Performance**: recording every entity every server frame (20 Hz) is cheap; per-render-frame recording of Ghoul2 bone overrides is not. Record animation *state* (anim number, start time, speed, blend), not bones.
- **Save games**: studio actors are ordinary NPC entities and are saved with save games; the studio scene itself is saved separately as a project, never in save games.

## 2. Architecture

All studio code lives in the SP game DLL (game + cgame), in new `cs_*.cpp` files, plus a small exe change for input.

```
           exe (client)                        game DLL
 ┌───────────────────────────┐    ┌────────────────────────────────────────────┐
 │ cl_keys / cl_input        │──▶ │ cgame: cs_ui.cpp  (panels, input, drawing) │
 │  studio input route       │    │        cs_view.cpp (viewport cameras) [P6] │
 └───────────────────────────┘    │   │                                        │
                                  │   ▼                                        │
                                  │ cs_scene.cpp   Scene Manager: objects,     │
                                  │                clock, selection, undo      │
                                  │ cs_project.cpp Project Manager: JSON save/ │
                                  │                load, autosave, validation  │
                                  │ cs_json.cpp    tiny JSON reader/writer     │
                                  │ cs_actor.cpp   Actor Manager (spawn, xform)│
                                  │ cs_record.cpp  Recorder / ghosts       [P3]│
                                  │ cs_timeline.cpp Tracks, keys, interp   [P4]│
                                  │ cs_replay.cpp  Combat replay           [P5]│
                                  │ cs_camera.cpp  Camera Director         [P6]│
                                  │ cs_path.cpp / cs_prop.cpp              [P7]│
                                  │ cs_audio.cpp   CineAudio + faces       [P8]│
                                  └────────────────────────────────────────────┘
```

**Module map (spec section 1)**
1. Scene Manager: `cs_scene.cpp`. Scene objects (actor, camera, prop, audio), the cinematic clock (int64 microseconds; frames derived from the project fps, independent of render fps), selection, undo/redo command stack.
2. Actor Manager: `cs_actor.cpp`. Wraps NPC spawning and removal; transforms; model, NPC type and animation profile; owns the entity ↔ scene object link.
3. Recorder: `cs_record.cpp` (Phase 3). Per-actor takes: 20 Hz state samples (origin, angles, velocity, legs/torso anim + start time + speed, saber state, weapon, view angles), plus an event stream (ignite, fire, force power, hit, death, sound, effect).
4. Timeline Editor: `cs_timeline.cpp` (Phase 4). Tracks of typed keys, interpolation modes (step, linear, smooth, Catmull-Rom, Bezier, hold), clips (trim, split, loop, offset, speed).
5. Camera Director: `cs_camera.cpp` + `cs_view.cpp` (Phase 6). Cameras as scene objects; path keys with Catmull-Rom; cut track; live camera recording. Applied in `CG_DrawActiveFrame` after `CG_CalcViewValues`, so the ICARUS camera still works when the studio isn't driving the view.
6. Animation Editor: in `cs_actor.cpp` + the UI (Phase 2). Browser from `animTable` + the actor's `animation.cfg`, preview by setting anims on the actor.
7. Combat Replay: `cs_replay.cpp` (Phase 5). Records **all** entities (not just actors) as state samples + events + periodic checkpoints; replays by driving entities with AI off; reverse scrubbing restores the nearest earlier checkpoint and plays forward.
8. Audio Director: `cs_audio.cpp` (Phase 8). Drives `cineaudio` and face expressions from tracks.
9. Prop Manager: `cs_prop.cpp` (Phase 7). `misc_model` / Ghoul2 props, bolt attachments.
10. Project Manager: `cs_project.cpp`. Versioned JSON under `CinematicStudio/Projects/<name>/`, autosave, missing-asset validation.

**Input route (exe)**: when cvar `cs_active` is 1 and neither the console nor a menu is open, key, char and mouse events go to cgame first (`CG_KEY_EVENT` / `CG_MOUSE_EVENT`). cgame returns 1 if it used the event; otherwise the event falls through to normal bindings, so `~` (console) and gameplay keys still work when the studio wants them to.

## 3. File-level plan

| Phase | New files | Changed files |
|---|---|---|
| 1 Foundation | `code/cgame/cs_ui.cpp`, `code/cgame/cs_scene.cpp`, `code/cgame/cs_project.cpp`, `code/cgame/cs_json.cpp`, `code/cgame/cs_studio.h`, `tests/cinematic_studio/` | `code/client/vmachine.h` (exports), `code/client/cl_keys.cpp`, `cl_input.cpp`, `cl_main.cpp` (input route), `code/cgame/cg_main.cpp` (vmMain, init, commands), `cg_draw.cpp` (draw hook), `cg_consolecmds.cpp`, `CMakeLists` |
| 2 Actor Manager | `cs_actor.cpp`, `cs_anim.cpp` | `NPC_spawn.cpp` (spawn returning the entity), `NPC_stats.cpp` (profile query) |
| 3 Recorder | `cs_record.cpp` | `g_active.cpp` / `NPC.cpp` (drive ghosts, AI off), `cg_players.cpp` (ghost translucency) |
| 4 Timeline | `cs_timeline.cpp`, `cs_interp.cpp` | `cs_ui.cpp` |
| 5 Replay | `cs_replay.cpp` | `g_main.cpp` (frame hook), `sv_savegame.cpp` (checkpoint reuse) |
| 6 Camera | `cs_camera.cpp`, `cs_view.cpp` | `cg_view.cpp` |
| 7 Paths/props | `cs_path.cpp`, `cs_prop.cpp` | `NPC_move.cpp`, `g_misc_model.cpp` |
| 8 Audio/face | `cs_audio.cpp` | `cg_face.cpp` |
| 9 Director | — | `cs_ui.cpp`, `cs_view.cpp` |
