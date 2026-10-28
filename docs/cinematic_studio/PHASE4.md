# Cinematic Studio: Phase 4 (Timeline + saber clashes)

Status: **built** (Windows x64 game DLL), core **unit-tested**. **Not yet run in game.**

## Saber clashes with takes
The game only makes sabers clash when the defending character's own combat code decides to block,
and a played-back take doesn't run that code, so your saber went through your copies' blades. Now the
studio checks every blade of every take-driven actor against every other blade (yours, NPCs', other
takes'), now and halfway through the last frame's swing. Where they meet:
- the game's own block sound, block sparks and screen flash play there;
- while you're **recording**, your attack (or a live NPC's) **bounces off**, as it does against a real
  block (the game's attack-bounce), so you feel the hit and the bounce is in your take;
- takes clash with each other too, so on playback two recorded duellists spark where their blades met.
A pair can clash again 250 ms later. **CLASH** (transport bar) turns it off.

## Timeline
- **Clips flow like takes now:** while the scene plays, the engine plays a clip's animation at its
  speed and blends into it (**blend in**, 0-1000 ms, 100 ms by default; set it per clip in the clip
  editor). Paused or scrubbing, frames are exact as before. Projects made before get 100 ms.
- **Split** (CTRL+B, or SPLIT in the clip / take editor): cuts the selected clip or take at the playhead.
  The second half carries on from the same point (a split take shares the recording, no copy).
- **Trim takes:** trim in / trim out (- / +, SHIFT = 1 s). Trimming the start keeps the rest where it was.
- **Move and turn takes:** select an actor a take drives and drag it in the viewport: the whole take moves.
  CTRL+drag turns it (about where the recording starts). UP / DOWN raise / lower it, RESET puts it back.
- **Position keys** for actors without a take: **K** (or KEY (K) in the inspector) sets a key at the
  playhead where the actor is. Keys show as yellow diamonds on its lane. Click a key to select it, drag
  it in time, DEL deletes it. Dragging the actor in the viewport at the playhead keys it there.
  Each key says how it moves **to the next**: smooth, linear, curve (Catmull-Rom through the keys) or
  step. Its velocity is given to the game, so a walk clip's legs swing in the direction it moves.
  Keys make the actor a prop.
- **Snapping** (SNAP in the transport, **N**): dragged clips, takes and keys snap to the playhead, 0 and
  the edges of other clips, takes and keys within 5 pixels. Hold SHIFT to drag freely.
- **Zoom:** CTRL+wheel over the timeline zooms about the cursor (down to 0.5 s), SHIFT+wheel pans.
  The ruler shows quarter / half seconds when zoomed in.

## Order of control (per actor, each frame)
Body: the active take, else the position keys. Animation: a clip that is playing, else the active take,
else a clip holding its last frame, else the game. Takes and keys only drive props.

## Fixed
- 3.1 meant "takes only drive props" (so PROP: OFF hands a copy to the game). A typo left that check
  out of the build you tested; it's in now.

## Modified files
| File | Change |
|---|---|
| `code/game/wp_saber.cpp` | `CS_SaberClash`: the game's block sound, effect, flash and attack bounce for a studio clash |
| `code/cgame/cs_actor.cpp` | blade-against-blade clash detection; animation ranges for engine-played clips |
| `code/cgame/cs_core.h/.cpp` | clip blend / anim offset, take trims and offsets, split take / clip, position keys with 4 interpolations, JSON |
| `code/cgame/cs_ui.cpp` | engine-played clips, keys driving bodies, take / key dragging in the viewport, key editor, take and clip editor additions, split, snapping, zoom, keys on lanes, CLASH / SNAP buttons, K / CTRL+B / N |
| `tests/cinematic_studio/test_core.cpp` | trims, offsets (incl. turned velocity), splits, keys + interpolations + undo, save/load |

## Test procedure
1. Record yourself (Take 1). Move the playhead back, record again and attack your copy's saber:
   sparks and the clash sound, and your swing bounces. F10. Play both: they clash where you clashed.
2. Select your copy, drag it in the viewport: the whole performance moves. CTRL+drag turns it. CTRL+Z.
3. Click its take, playhead in the middle, CTRL+B: two takes. Trim in / out on one.
4. Spawn an actor, K at 0 s, move the playhead to 3 s, drag the actor elsewhere (keyed at 3 s).
   Add a walk clip (LOOP). Play: it walks from one key to the other, legs swinging. Click a key,
   TO NEXT: curve / step.
5. Drag clips near the playhead: they snap. SHIFT: they don't. CTRL+wheel on the timeline.

## Known limitations
- Clashes are effects plus your own attack bouncing; a played-back copy can't change what it does
  (it's a recording). For a copy that really reacts, hand it to AI (PROP: OFF, AI: ON).
- Clash detection uses blade positions from the last drawn frame, so a copy that's off screen doesn't clash.
- Saber locks aren't started by studio clashes.
- Keys hold position only (no rotation keys for pitch / roll; cameras get keys in Phase 6).
- No multiple selection or ripple editing yet.

## Next (Phase 5: combat replay)
Record the whole fight (every character and events: hits, force powers, sounds, deaths) and scrub it.
