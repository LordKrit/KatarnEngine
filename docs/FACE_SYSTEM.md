# Face system (lipsync, expressions, blinks)

Any Ghoul2 player/NPC model whose skeleton has face bones can use it. Nothing changes for
models without a face file.

## Files
- `ext_data/faces/<model folder>.face` - face bones (base pose) and named poses. Written by
  `tools/face/face_tool.py <skeleton.gla> <out.face>` from the recipes in `tools/face/face_poses.py`
  (BF2 `FACIAL_` rigs; add a dict there for other rig families).
- `<voice sound path>.lip` next to a voice line (same name, `.lip` instead of `.mp3/.wav`): Rhubarb
  Lip Sync TSV output (`rhubarb -f tsv -o line.lip line.wav`), lines of `seconds<TAB>shape`.

## Poses
Mouth shapes (Rhubarb letters): `A` M/B/P, `B` consonants, `C` E, `D` A, `E` O, `F` OO/W,
`G` F/V, `H` L, `X` rest. Also `blink`, and expressions: `angry smirk smile sneer sad pain
surprise squint` (add more in face_poses.py).

## Runtime
- A voice sound on an entity (CHAN_VOICE, CHAN_VOICE_ATTEN, CHAN_VOICE_GLOBAL) loads its `.lip`;
  without one the mouth follows the voice volume.
- Blinks every 2-6 s; eyes shut when dead.
- Expressions blend on top of speech.

## Commands (console, or `exec` from cutscene scripts)
    faceexpr <who> <pose|none> [weight 1] [seconds 0=hold] [blend ms 250]
    facelip  <who> <sound path>
    facelook <who> <who2|off>
`<who>`: `player`, an entity number, or a targetname / NPC_targetname / script_targetname.

## Custom audio (cutscenes)
    cineaudio play <file> [who]     plays a sound as someone's voice, with lipsync
    cineaudio stop [who]
A bare file name is looked up in `sound/cine/` (e.g. `cineaudio play dooku.mp3` plays
`sound/cine/dooku.mp3` on the player). Make its lipsync with `tools/face/make_lipsync.bat`
(Rhubarb Lip Sync) and put `dooku.lip` beside it; without one the mouth follows the volume.

## Face life (idle behaviour)

Every character with a face rig now has an idle layer on top of lipsync and expressions:
irregular blinks (some double or half), small eye saccades and brief glances, slow drift between
moods, brow/lid micro motion, and combat tension that builds while fighting and fades afterwards.
Each character's timing is randomised and staggered, so actors never blink or shift together.

- Profiles: `ext_data/faces/profiles.txt`, one per model folder (plus `default`). Keys: `mood <pose> <freq>`,
  `moodStrength`, `moodHold`, `blink`, `doubleBlink`, `halfBlink`, `saccade <min> <max> <deg>`,
  `glance <chance> <deg>`, `micro`, `combat <pose> <max> [rise fall]`, `talkEmphasis`.
- Priorities: lipsync owns the mouth while talking (idle mouth movement drops to 15%); a deliberate
  look target (`facelook`, scripts) keeps the eyes on it, with only tiny saccades; a manual
  `faceexpr` fades the idle moods out (blinks and gaze keep going).
- `faceidle <who> on|off|<profile>` turns it off/on or forces a profile.
- Dialogue: stressed syllables (mouth opening wide after a closed shape, or a volume jump) lift the brows.
  A `.lip` file may also carry expression cues, one per line: `e <seconds> <pose> [weight] [blend ms]`
  (`e 2.4 none` clears). They play with the line and relax shortly after it ends.
