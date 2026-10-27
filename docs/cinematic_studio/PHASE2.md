# Cinematic Studio: Phase 2 (Actor Manager)

Status: **built** (Windows x64 game DLL), core **unit-tested** (318 checks). **Not yet run in game.**

## Fixed from Phase 1
- **Free camera wouldn't move.** The engine sends letter keys as capital-letter codes, while the studio
  listened for lowercase ones. So WASD/E/Q flying and every letter hotkey (F, P, V, Q/E turn,
  CTRL+Z/Y/S) did nothing; the arrows, Space and F10 worked. Letters are now normalized first.

## New in Phase 2
- **ANIMATIONS tab** (or ANIMATIONS... in the actor inspector) for the selected actor:
  - Every animation the actor's skeleton has, from its `animation.cfg`: search box, scrolling list, frame counts.
  - Double-click or PLAY previews it; FREEZE holds the current frame; RELEASE gives the actor back to the game.
  - Legs, torso or both; speed 0.05-4x; LOOP; blend time into the animation (0-1000 ms).
  - A frame range (start/end, SHIFT = 10 frames); clips can use only part of an animation.
  - A frame scrubber: click or drag to pose the actor on any frame. The red marker shows the playing frame.
- **Animation profiles** (`_humanoid_X`), switched per actor without leaving the editor:
  - The picker lists every `models/players/_humanoid*` folder that has a `.gla` and an `animation.cfg`.
  - Before switching, the two `.gla` files must have the **same bones in the same order** (Ghoul2
    addresses bones by index). If they don't, the switch is refused and the reason shown (e.g. "bone 12
    is lhumerus here but rhumerus in _humanoid_X"). Every animation's frames must also exist in the
    target `.gla`.
  - The switch points the model at the target `.gla` (an animation-index offset per entity, added
    where the game sets each animation's `.gla`) and gives the actor that skeleton's animation set.
    OWN goes back. The profile is undoable and saved with the project.
- **Animation clips on the timeline:**
  - ADD CLIP AT PLAYHEAD puts the browsed animation (range, parts, speed, loop) on the actor's track.
  - During playback, stepping and scrubbing, clips **drive the actor frame-exactly** from the scene clock.
  - A non-looping clip holds its last frame and a looping one keeps looping, until the next clip starts.
    Before the first clip the game animates the actor as usual.
  - Clips are drawn on the actor's track, with a faint tail while held. Click a clip to edit it in the
    SCENE inspector: start, length, speed (SHIFT = 1 s steps), parts, loop, delete. Drag clips along the
    track; they snap to whole frames. DEL deletes the selected clip.
  - All clip edits are undoable and saved.
- **Moving actors with the mouse:** drag the selected actor in the viewport to move it along the ground
  plane; CTRL+drag turns it. A dotted ring and a facing marker show at its feet.
- **Duplicate** (button or CTRL+D) copies an actor with its profile and clips, 40 units aside.
- **Change NPC type** respawns the actor as another NPC (Vader to Obi-Wan) in the same place, with its
  profile and clips, undoably. If the new type can't spawn, it goes back to the old one.

## Modified / new files
| File | Change |
|---|---|
| `code/cgame/cs_actor.cpp` (new) | Actor Manager game side: list animations, pose/play/release, current frame, profile list + validation (bone names from the `.gla` files) + switching |
| `code/cgame/cs_core.h/.cpp` | `AnimClip`, `Object::profile/clips`, `SetProfile`, `SetNpcType`, `Duplicate`, `AddClip/SetClip/RemoveClip`, `ActiveClip`, JSON for profile and clips |
| `code/cgame/cs_ui.cpp` | letter-key fix; ANIMATIONS tab; clip drawing, dragging and editor; mouse drag and turn; duplicate, NPC type |
| `code/cgame/cs_studio.h` | Actor Manager API |
| `code/game/bg_panimate.cpp`, `code/game/g_main.cpp` | add the studio's per-entity `.gla` offset where the game sets an animation's `.gla` (0 for everyone else) |
| `code/cgame/cg_players.cpp` | animation events (footsteps etc.) compare against the actor's own `.gla` index |
| `code/game/g_utils.cpp` | `G_FreeEntity` clears the studio's per-entity state |
| `code/game/g_local.h` | declarations |
| `tests/cinematic_studio/test_core.cpp` | profiles, clips, active-clip rules, NPC type change, duplicate, save/load |

## Test procedure
1. F10, then hold the right mouse button and fly with WASD (E up, Q down, SHIFT fast, wheel for field of view).
2. Spawn `md_vader`, select him, then ANIMATIONS tab. Type `stand` in the search box and press Enter,
   then double-click an animation: he plays it. Try LEGS/TORSO, speed, LOOP, and drag the frame bar.
3. Pick another `_humanoid_*` profile with `<` `>`. The line under it says whether it's compatible.
   APPLY a compatible one; the animation list changes to that skeleton's. OWN goes back. CTRL+Z undoes.
4. Choose an animation, move the playhead, then ADD CLIP AT PLAYHEAD. Add a second clip later on. Press
   SPACE: Vader follows the clips. Scrub the ruler: he poses frame by frame. Drag a clip and edit it in SCENE.
5. Drag Vader in the viewport, CTRL+drag to turn, CTRL+D to duplicate, CHANGE NPC TYPE to `md_obiwan`.
6. Save, NEW, load: profiles and clips come back.

## Known limitations
- Switching clips snaps from one pose to the next. Blended transitions on the timeline come with
  Phase 4; the browser's PLAY does blend.
- No keyframed position yet: actors play animations in place on the timeline (Phase 4 adds position tracks).
- Clip trim and split are by the length/frame-range controls only; razor and trim handles come in Phase 4.
- A save game made while an actor uses another profile reloads with its own skeleton (the switch isn't stored in save games).
- No in-game skeleton retargeting between incompatible profiles; that stays an offline job for `tools/retarget`.

## Next (Phase 3: Ghost Performance Recorder)
Record the player's performance (position, view, legs/torso animation state, saber/weapon events),
save takes, replay them as independent actors (ghost preview / solid), overdub while earlier takes play.

## Phase 2.1 (after the first in-game test)
- Fixed: the profile check said "the model doesn't name its skeleton" for the BF2 models (dooku_bf2...).
  The engine can't name their skeleton, so the studio now takes it from the actor's own animation set.
- **FORCE** next to an incompatible profile applies it without bone or frame checks. It's remembered
  per actor and saved; the actor may pose wrongly if the skeletons really differ.
- New profiles: `_humanoid_mocap` (the zzzz_Mocap animation set, as its own profile instead of replacing
  stock `_humanoid`), `_humanoid_MEmocap1`, `_humanoid_rpg`. All three have the standard 53-bone
  skeleton, so they pass the checks. Installed as `GameData\MD\zzz_cs_profile_*.pk3`.

## Phase 2.2
- A profile whose `animation.cfg` runs past the end of its `.gla` (e.g. `_humanoid_MEmocap1`: BOTH_STAND4
  needs frame 25416, the .gla has 25347) no longer refuses to apply: those animations are trimmed to fit
  (dropped if they start past the end) and the log says how many.
- **Extra animations:** names in a profile's `animation.cfg` that the game doesn't know (80 in `_humanoid_rpg`
  such as BOTH_ANAKINKATA, 5 in `_humanoid_MEmocap1` such as BOTH_STAND11) are read by the studio and
  listed with a `*`. They play, freeze, scrub and go on the timeline like any other; the game's own AI
  can't use them.
