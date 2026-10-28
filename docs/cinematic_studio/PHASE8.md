# Cinematic Studio: Phase 8 (Audio and faces)

Status: **built** (Windows x64 game DLL), core **unit-tested**. **Not yet run in game.**

## Voice lines (AUDIO tab, actor selected)
Type a sound (`sound/cine/line1.mp3`, or just `line1.mp3` for sound/cine/), ADD LINE AT PLAYHEAD: the
line plays as that actor's voice when the scene plays through its start, with **lip sync** on a model with
a face rig (from a `.lip` file next to the sound, else from the voice volume, as `cineaudio` does).
Lines show as a thin teal bar at the top of the actor's lane; click one to select it: start, timeline
length, PLAY NOW / STOP, `x` deletes.

## Face tracks (actors with a face rig: `ext_data/faces/<model>.face`)
- **Expressions (faceexpr):** pick a pose of the model (< >: angry, smirk, blink... read from its face
  file), weight, length, blend; ADD EXPRESSION AT PLAYHEAD. It blends in, holds, and blends out at the
  same speed. Overlapping ones: the later one wins while it lasts. Pink bars at the bottom of the lane.
  They follow scrubbing too.
- **Idle (faceidle):** ON HERE / OFF HERE: blinks, glances and moods on or off from that moment on
  (e.g. off for a held stare). Small ticks on the lane.
- **Eye posing:**
  - LOOK AT HERE: eyes and head follow another actor (NEXT picks who) from that moment on (works on any model);
  - AIM HERE: the eyes point at an angle (^ v < >), and move smoothly from one aim key to the next;
  - FREE HERE: back to their own gaze.
- Actors without a face rig show "no face rig": expression / aim tracks are off for them (as the brief asks);
  LOOK AT still turns the head.

## Music and sound tracks
AUDIO tab with nothing selected: ADD MUSIC TRACK / ADD SOUND TRACK (an "S" object). Select it: type a file,
ADD ... AT PLAYHEAD. Music plays as the background track (one at a time, from its start); sounds play as
2D sounds. PLAYS: ON / OFF mutes a track. Blocks with the file name on the lane; click to select.

Sounds start when the scene **plays through** their start (not while scrubbing); pausing or stopping
the scene stops the studio's sounds and music.

## Modified files
| File | Change |
|---|---|
| `code/cgame/cg_face.cpp` | studio API: has a rig, list its poses, set expression / idle, eye posing override |
| `code/cgame/cs_replay.cpp` | look target (eyes / head) |
| `code/cgame/cs_core.h/.cpp` | `SoundClip`, `FaceClip`, `IdleKey`, `EyeKey` (+ evaluation), audio objects, `Scene::Edit`, JSON |
| `code/cgame/cs_ui.cpp` | AUDIO tab, voice / expression / idle / eye / music evaluation and playback, lane marks |
| `tests/cinematic_studio/test_core.cpp` | face clip overlap, idle, eye modes and aim blending, audio tracks, save / load |

## Known limitations
- A sound can't start in its middle: press play before a line / music start to hear it (sound engines here
  have no seek). Timeline lengths of sounds are for the display (the sound plays to its end).
- Pausing stops all sounds, game ones too, for that moment.
- No volume / fade per clip yet.
- Expression poses come from the model's face file; the list falls back to common names for a model
  the game hasn't loaded a face file for yet.
