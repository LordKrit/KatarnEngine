# Stacked lanes, clip repeat and scale, and LIVE playback

## Stacked lanes under an actor
- Click **+** next to an actor's name on the timeline to open three lanes under it. **-** closes them again.
  - **voice**: voice lines, with each file's name shown.
  - **face**: expression blocks, with each pose's name. The darker ends show the blend in and out.
  - **eyes / idle**: eye keys (free, look at someone, or aim) and the stretches where face idle is on or off.
- With the lanes open, the thin coloured marks on the actor's main row go away, so it only shows animation clips, takes and position keys.
- Editing on the lanes:
  - Drag a block to move it.
  - Drag its right edge to make it longer or shorter.
  - Drag an eye key or idle key to move it. Everything snaps to whole frames.
- Clicking an expression selects it. The AUDIO tab then shows it under **FACE**, where you can:
  - change the pose (< >),
  - set the weight and blend (- +),
  - **DUPLICATE AFTER** (an identical copy that starts where this one ends),
  - **DELETE** it.
- When expressions overlap, the one that starts latest wins and blends in over the earlier one.

## Clip edges: repeat and scale (like a Blender NLA strip or an editor's clip)
Drag the right edge of an animation clip:
- **Drag: repeat.** Making the clip longer than one pass of the animation turns looping on, so it repeats. Dark marks inside the clip show where each repeat starts, and the clip's label shows how many passes it covers (`BOTH_WALK1  x3.0`).
- **SHIFT+drag: scale.** The same frames play slower or faster to fill the new length. The speed shows on the clip (`0.50x`).
- While you drag, the top of the timeline shows the repeat count or speed and the new length.

## LIVE: watch the scene with the studio closed
- If you close the studio with F10 while the scene is playing, playback now keeps going instead of stopping.
- The **LIVE** button in the transport bar does the same thing: it plays from the playhead and closes the studio.
- The console command `studio live` does it too.
- If the scene has director cuts, you watch it through them. If it has none, you get the normal game view.
- Live playback ends when the scene ends (unless LOOP is on), or with `studio stop`. Pressing F10 brings the studio back without stopping playback.
