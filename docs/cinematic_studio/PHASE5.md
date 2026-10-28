# Cinematic Studio: Phase 5 (Combat replay)

Status: **built** (Windows x64 game DLL), core **unit-tested**. **Not yet run in game.**

## What it does
**Everything that happens in a fight now comes back on playback:** sparks, blood, hit and block sounds,
explosions, pain and death voices, taunts, footsteps, muzzle flashes, and the force powers characters
used (lightning, drain, grip, push / pull blur, speed), plus being shocked, stunned or drained.

### How
- **Events.** Every effect, sound and character event the game shows reaches the cgame as an event. While
  the studio records, it keeps a copy of each (where, what, which character, and the effect / sound by
  name, so it works on another level too). They go on a **world-events track** (orange "E" in the
  outliner, a bar with a tick per event on the timeline). While the scene plays forward, the events play
  again at the same moments, on the actors that replay those characters. Scrubbing doesn't fire them.
- **Force powers** are part of each take sample now. On playback they're shown on the actor only
  while the frame is drawn and put back right after, so replayed lightning can't actually hurt anyone.
- The studio's own playback isn't recorded again when you overdub (its events and actors are skipped).

### Two ways to record a fight
1. **Recording** (Phase 3: RECORD NEW PERFORMER / RECORD TAKE, with RECORD NPCS): now also makes a
   "Fight events" track at the take's start.
2. **Instant replay** (new, like a sports replay): ACTORS tab > **INSTANT REPLAY: ON** (or `studio replay on`).
   Close the studio and just play. The studio keeps the last **30 s** (KEEP 10-60 s, or
   `studio replay <seconds>`, up to 120) of you, every character within 3000 units and every event.
   After a fight you liked, press **F9** (`studio replay`): those seconds become takes on actors (you as
   your NPC type; NPCs join the scene; ones that died or were removed get a new actor that replays them,
   death included) and a "Replay events" track, all at the playhead. The studio opens on it, ready to
   scrub, re-shoot with cameras or overdub. F9 is bound on first run (bind it yourself if F9 is taken).

### Events tracks
Select one (outliner or its bar): count, start, what kinds; **PLAYS: ON / OFF**; drag the bar to retime
(snaps); DEL deletes; undoable; saved with the project.

## Commands
`studio replay` (keep the last seconds), `studio replay on|off`, `studio replay <seconds>`.

## Modified / new files
| File | Change |
|---|---|
| `code/cgame/cs_replay.cpp` (new) | event capture, firing events again, force powers on playback actors |
| `code/cgame/cg_event.cpp`, `cg_playerstate.cpp` | hand each event to the studio before the game handles it |
| `code/cgame/cg_draw.cpp` | end-of-frame hook (puts force powers back) |
| `code/cgame/cs_actor.cpp` | samples record force powers and being shocked / stunned / drained |
| `code/cgame/cs_core.h/.cpp` | `WorldEvent`, events objects (`AddEvents`, `SetEventsStart`, `SetVisible`), JSON; take samples are read by field name (older files keep loading) |
| `code/cgame/cs_ui.cpp` | events from recordings, instant replay buffer + F9, events playback, events lanes and inspector |
| `code/game/CMakeLists.txt` | cs_replay.cpp |
| `tests/cinematic_studio/test_core.cpp` | events tracks (sorting, undo, retime, save/load), force in takes, Phase 3.1/4 take files |

## Test procedure
1. ACTORS tab: INSTANT REPLAY: ON. F10 to close, fight a few NPCs (saber, force lightning / push).
2. F9: the studio opens with actors for you and them and a "Replay events" track. SPACE: the fight plays,
   with sparks, hit sounds, voices, force effects. Scrub: no sounds while scrubbing; poses follow.
3. Select the events track, PLAYS: OFF: silent fight. Drag the bar: sounds move in time.
4. Record (RECORD NEW PERFORMER with RECORD NPCS: ALL NEARBY): a "Fight events" track comes with the takes.
5. Save, load: all of it comes back.

## Known limitations
- Effects bolted onto a model (e.g. some weapon / saber glows started by the game on a bone) aren't
  recorded; effects at a point are.
- Dismemberment: the death is replayed, cut-off limbs and the flying pieces are not.
- Thrown sabers, weapon switches and projectiles in flight aren't replayed (impacts and their sounds are).
- Doors, movers, breakables and items aren't replayed (their sounds and effects are, as events).
- Instant replay records characters within 3000 units of you, at 30 samples a second.

## Next (Phase 6: Camera Director)
Camera paths (keys, Catmull-Rom), a cut track, live camera recording, depth of field / roll / shake.
