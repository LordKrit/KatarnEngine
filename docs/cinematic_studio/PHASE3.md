# Cinematic Studio: Phase 3 (Ghost Performance Recorder + animated props)

Status: **built** (Windows x64 game DLL), core **unit-tested**. **Not yet run in game.**

## What it does
You play the scene yourself (move, fight, swing the saber) and the studio records it as a **take**.
A take plays back on a studio actor exactly as you performed it: where you were, which way you faced,
and the exact frame of your legs and torso animations, plus saber on/off. Then you record the next
part over it while the earlier takes play back: **overdub**.

### Recording
- **ACTORS tab > RECORD NEW PERFORMER (YOU)**: records you; on stop, a new actor appears where you started,
  as your character's NPC type (if you play as an NPC) or the NPC type in the ACTORS field (md_vader...).
- **Select an actor > RECORD TAKE**: records a new take *for that actor* (it's hidden while you record).
- The take starts at the **playhead**. The studio closes, a 3-second countdown runs (ACTORS tab:
  COUNTDOWN 0-5 s), then the scene plays in real time while you perform. Other actors' takes and clips play
  as usual, shown see-through (GHOST OTHERS, on by default) so you can react to them.
- **F10 stops and keeps** the take; the studio reopens on it. `studio rec cancel` throws it away.
  Recording past the scene's end makes the scene longer.
- 30 samples per second; playback interpolates positions, facing and animation frames between them.

### Takes
- An actor can have several takes; one is **active** (drives the actor). The actor inspector lists them:
  USE / ACTIVE switches, NONE turns them all off. Takes are named Take 1, Take 2...
- Click a take on the timeline (purple) to edit it: start (- / +, SHIFT = 1 s), USE, RE-RECORD (records a
  new take from the same start; the old one stays), DELETE (or DEL). Drag it along the timeline to retime it.
- Outside its range a take holds its first / last pose. A **clip that is playing** wins over the take
  (so you can replace part of a performance with a clip); a clip holding its last frame doesn't.
- Takes are undoable and saved in the project (`scenes/scene_01.json`, project format version 2;
  version 1 projects still load).

### Animated props (your request)
**PROP: ON** in the actor inspector turns an actor into an animated prop, like a noclipped puppet:
- no AI (it never walks off or fights), no gravity, no collision (others walk through it, it can't be
  pushed), can't be hurt or targeted;
- it stays exactly where you put it and only the studio moves and animates it (clips, takes, dragging).
An actor becomes a prop automatically when it gets its first take. PROP: OFF gives it back to the game
(AI follows the AI button). Undoable and saved.

**GHOST** draws an actor see-through, as a guide.

## Commands
`studio rec` (selected actor, or a new performer), `studio rec new`, `studio rec stop`, `studio rec cancel`,
`studio countdown <0-10>`. `F10` (`studio`) stops a recording.

## Modified / new files
| File | Change |
|---|---|
| `code/cgame/cs_core.h/.cpp` | `TakeSample`, `Take` (+ interpolation), `Object::prop/ghost/takes/activeTake`, `AddTake/SetTakeStart/RenameTake/SetActiveTake/RemoveTake/SetProp/SetGhost`, JSON v2; undo edits only coalesce with edits of the same kind |
| `code/cgame/cs_actor.cpp` | prop mode (`SetActorProp`), hide, ghost alpha, sabers, sampling the player |
| `code/cgame/cs_ui.cpp` | recorder (countdown, recording, overlay, stop/keep/cancel), take playback in the scene evaluation, inspector (PROP, GHOST, RECORD TAKE, takes list), take editor, takes on the timeline (select, drag), ACTORS tab recorder |
| `code/cgame/cs_studio.h` | recorder API, `CS_DrawOverlay` |
| `code/cgame/cg_draw.cpp` | the REC overlay over the HUD while recording |
| `code/cgame/cg_players.cpp` | ghosts drawn see-through |
| `code/game/g_local.h` | `CS_GhostAlpha` |
| `tests/cinematic_studio/test_core.cpp` | takes (interpolation, angle wrap, animation change / loop), prop mode, take editing + undo, save/load, damaged take, version 1 projects |

## Fixed along the way
- Undo merging: quick edits of *different* kinds on the same object within a second (e.g. switching the
  profile and then dragging a clip) were merged into one undo step. Now only the same kind of edit merges.

## Test procedure
1. F10. ACTORS tab: type your performer's NPC type (e.g. `md_vader`). Put the playhead at 0.
2. RECORD NEW PERFORMER (YOU). After the countdown, walk, turn on your saber, swing, crouch, jump. F10.
   A Vader appears with Take 1. SPACE: he repeats what you did. Scrub the ruler: he follows frame by frame.
3. Select him, move the playhead back to 0, ACTORS tab > RECORD NEW PERFORMER again and duel his ghost.
   F10: two actors, both playing back together.
4. Select the first actor > RECORD TAKE: Take 2. USE / ACTIVE switches between them. Drag a take on the
   timeline, CTRL+Z.
5. PROP: ON on a normal NPC with AI on: it stops, floats where it is if you drag it up, and you can walk
   through it. PROP: OFF.
6. Save, NEW, load: actors, takes, props come back.

## Known limitations
- Records the **player** only (one performer at a time; overdub for more). Recording NPCs and the whole
  fight is Phase 5 (combat replay).
- Plays back position, facing, legs/torso animation frames and saber on/off. Not yet: weapon changes,
  force-power effects, saber trails/hit sparks, sounds, head/aim pitch, and damage. Those are events for Phase 5.
- The take always plays at the place it was recorded (no offset when you drag the actor); position
  tracks/offsets come with the Phase 4 timeline.
- Playback poses whole animations frame by frame without blending between samples of different animations.
- A take recorded on one skeleton plays any animation the actor's skeleton also has; ones it lacks are skipped.
- Recording is lost if the level changes mid-take.

## Next (Phase 4: Timeline)
Typed tracks with keys and interpolation (position/rotation offsets for actors and takes, camera paths
prep), clip blending, trim/split/razor, multiple selection, snapping.

## Phase 3.1 (after the first in-game test: "acts like a robot")
**Why it looked robotic:** playback posed every frame by hand with no blending, and the actor stood still
as far as the game knew (zero velocity), and the game turns and swings the legs from the velocity. Aim
pitch wasn't replayed either.

**Now:**
- Takes also record velocity and each animation's engine speed and looping. During playback the engine
  plays the recorded animations itself at that speed and **blends into each new one** (ACTORS tab:
  TAKE BLEND, 100 ms by default, 0-200). The studio only puts an animation back when it drifts more than
  2.5 frames from the recording. Paused or scrubbing, frames are still posed exactly.
- Velocity and aim (pitch) are replayed, so legs turn and swing and torso and head aim like yours did.
- **Recording NPCs:** while you record, NPCs fighting you are recorded too, each into its own take
  (ACTORS tab: RECORD NPCS: SCENE AI = scene actors with AI on; ALL NEARBY = also level NPCs within 2000
  units; NO). A level NPC joins the scene. An NPC that died gets a fresh actor that plays the take, death included.
- **Fighting your copies:** earlier takes are **solid** while you record over them (SOLID TAKES), so your
  saber lands on them; they still can't be hurt or pushed.
- **Letting a copy fight back:** PROP: OFF + AI: ON on one of your copies hands it to the game. It fights
  you live and, with RECORD NPCS on, its new moves are recorded as its next take. Takes only drive props.
- Takes are shared between undo snapshots instead of copied (a long session with many takes could have
  used a lot of memory).
- Projects saved with the first Phase 3 build load; their velocity is estimated from the positions.

**Still not replayed:** weapon changes, force powers, sounds, hit effects, damage. An NPC's take shows
what it did, but if you re-record yourself against its playback, it won't react to you again (hand it back
to AI for that).
