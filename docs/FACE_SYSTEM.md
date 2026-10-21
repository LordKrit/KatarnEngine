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
