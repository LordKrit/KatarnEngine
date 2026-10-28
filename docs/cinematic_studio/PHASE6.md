# Cinematic Studio: Phase 6 (Camera Director)

Status: **built** (Windows x64 game DLL), core **unit-tested**. **Not yet run in game.**

## What it does
- **Camera keys.** Select a camera, put the playhead somewhere, **K** (or KEY (K)): a key with the
  camera's position, aim (pitch, yaw), **roll** and **field of view**. Keys show as diamonds on the
  camera's lane; click / drag / DEL / TO NEXT (smooth, linear, curve = Catmull-Rom through the keys,
  step) like actor keys. Between keys the camera moves along that path, turning and zooming with it.
  To change a key: PILOT the camera, fly to the new spot, K (or the key editor's SET FROM THE VIEW).
  CLEAR KEYS removes them all.
- **Paths in the viewport:** dots along each keyed camera's path, squares at its keys, a short line
  showing where each camera looks. CAMERA tab > PATHS hides them.
- **Live camera recording:** REC MOVE on, PILOT the camera, SPACE: as the scene plays, your flight
  (position, aim, wheel zoom) is keyed 10 times a second as a curve. Pause / stop / switch camera to
  finish; it replaces that stretch of keys in one undo step.
- **Cut track** (top row of the timeline, "CUTS"): **CUT HERE** cuts to the selected camera at the
  playhead. Each cut runs until the next, shaded per shot and named. Click a cut's edge to select it,
  drag it in time (snaps), DEL deletes it; click a shot to select its camera. One cut per moment.
- **Director view** (**C**, or DIRECTOR on the cut track / camera inspector): the viewport shows what the
  cut track says: the live camera, with its keys, look-at and shake. Scrub or play to watch the edit.
  The live camera is marked [LIVE] in the viewport. Picking a camera yourself (LOOK THROUGH / PILOT /
  PLAYER VIEW / flying away) leaves director view.
- **Look at:** a camera can aim at an actor (NEXT cycles through them): it keeps its keyed position and
  turns to the actor's chest / head as it moves.
- **Shake:** 0-10, a handheld wobble (pitch, yaw, a little roll), the same every playback.
- FOV of an unkeyed camera: - / +, undoable (with keys, FOV comes from the keys).

## Modified files
| File | Change |
|---|---|
| `code/cgame/cs_core.h/.cpp` | keys carry pitch / roll / fov; one interpolation for all 7 channels (angles wrap); `EvaluateCameraKeys`; camera cuts (`AddCut/RemoveCut/MoveCut/ActiveCamera/Cuts`), look-at, shake, fov setters, `ReplaceKeys`; JSON (cameras save keys, cuts, look-at, shake) |
| `code/cgame/cs_ui.cpp` | camera poses from keys / look-at / shake, director view, camera keys, live recording, cut track row, camera inspector, paths and look direction in the viewport, C hotkey |
| `tests/cinematic_studio/test_core.cpp` | camera keys (angles across 180, roll, fov), cuts (one per time, move, undo), look-at, shake, fov, recorded moves, save / load |

## Test procedure
1. CAMERA tab > ADD FROM VIEW twice in two places (Camera 01, Camera 02).
2. Camera 01: PILOT, playhead 0 s, K; playhead 4 s, fly elsewhere, wheel to zoom, K. PLAYER VIEW, then
   LOOK THROUGH and SPACE: the camera flies and zooms between the keys. The path shows in the viewport.
3. Camera 02: LOOK AT > NEXT to an actor, SHAKE 3.
4. Playhead 0: select Camera 01 > CUT HERE. Playhead 2 s: Camera 02 > CUT HERE. Press C and SPACE:
   the view cuts from the moving shot to the shaky close-up following the actor. Drag the cut.
5. REC MOVE, PILOT Camera 02, SPACE, fly around while the fight plays; pause: the move is keyed.
6. Save, load: keys, cuts, look-at, shake come back.

## Known limitations
- No depth of field / motion blur: the renderer has no such pass to drive (Phase 9 can add one for
  final renders). Roll, FOV, shake, look-at and cuts are in.
- No camera-to-actor attachment (a camera riding on a moving actor); look-at aims, it doesn't follow.
- Cuts are hard cuts (no dissolve).
- Live recording keys 10 times a second; very fast flicks are smoothed by the curve.

## Next (Phase 7: Paths and props)
Props (misc_model / Ghoul2 models placed, keyed and attached to actors' bones) and actor paths.
