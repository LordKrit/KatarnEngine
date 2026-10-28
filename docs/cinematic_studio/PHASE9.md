# Cinematic Studio: Phase 9 (Director mode and final render) + scale mode + HD text

Status: **built** (exe, game DLL, both renderers), core **unit-tested**. **Not yet run in game.**

## RENDER tab
- **In / out:** SET (I) / SET (O) at the playhead (or the I and O keys); `0` and END reset them. The range
  shows in light blue on the ruler. Length and frame count at the scene's fps (24 / 30 / 60, changeable here).
- **Letterbox:** none, 1.85 : 1 or 2.39 : 1. While editing, thin guides show where the bars will be; in the
  preview and the render the bars are drawn.
- **Camera:** director (the CUTS row) or the current view.
- **PLAY THE CUT:** plays in to out, clean (no editor), through the cut track, with the letterbox, then
  comes back. ESC or H stops it. Sounds, events and voices play as usual.
- **RENDER AVI:** writes `demos/<name>.avi` (the name field; the project name by default):
  - **frame-exact:** each video frame is one 1/fps step of the scene and of the game (the game runs
    with `fixedtime`, the video takes every drawn frame: `cl_aviFixedFrames`, new in this exe), however long
    a frame takes to draw. Heavy scenes render slower than real time but play back smoothly.
  - at the game's resolution; motion JPEG or not by `cl_aviMotionJpeg`.
  - **no sound** in a frame-exact render (the mixer runs on real time). For the game's sound, do a PREVIEW
    while recording with the console's `video`, and lay the sound under the render.
  - cheats (`helpUsObi`) are switched on for `fixedtime` while it renders and put back afterwards.
  - ESC stops it (the file keeps what was rendered). Closing the studio or changing level stops it too.
- Console: `studio preview`, `studio render [name]`.

## Scale mode (S)
- **S** toggles scale mode. Select a prop or an actor: **drag** in the viewport left / right to shrink /
  grow it (x2 per 60 units, SHIFT faster), or use the **wheel** over the viewport (5 % a notch, SHIFT 25 %).
  A line above the timeline shows the name and the scale. S again leaves it.
- The prop and actor inspectors have a scale row: - / + (SHIFT = 25 %), 1x, and S.
- Undoable (a drag is one step), saved, and kept when a prop respawns or is duplicated. It's how big the
  model is drawn; collision boxes keep their size.

## HD text
The editor's text was the game's small font scaled down in the old 640x480 2D space, so on a widescreen
every letter was stretched about a third wider and soft. Now (`cs_hdText 1`, default; RENDER tab: HD TEXT):
- the menu font (crisper, drawn smaller from bigger letters), sized like before, a little larger;
- drawn with the letters' real proportions on any aspect ratio: a new font style flag (`STYLE_ASPECT`) in
  both renderers of this build. A renderer without it ignores the flag (the text is then just wider).
`cs_hdText 0` goes back to the classic font.

## Builds
- Both renderers are built again (they carry the font flag). rend2 needed its shader packer as a native tool
  in the cross build (it was a Windows .exe the build couldn't run); now it builds here.

## Modified files
| File | Change |
|---|---|
| `code/qcommon/qfiles.h`, `code/rd-common/tr_font.cpp` | `STYLE_ASPECT`: draw / measure text unstretched on widescreens |
| `code/client/cl_avi.cpp` | `cl_aviFixedFrames`: one video frame per drawn frame, no audio |
| `code/cgame/cs_core.h/.cpp` | `Object::scale`, `SetScale`, `Host::SetEntityScale`, JSON |
| `code/cgame/cs_props.cpp` | entity scale |
| `code/cgame/cs_replay.cpp` | reading cvars |
| `code/cgame/cs_ui.cpp` | RENDER tab (in / out, letterbox, preview, frame-exact render), scale mode, HD text |
| `tests/cinematic_studio/test_core.cpp` | scale (undo, limits, duplicate, respawn, save / load) |

## Known limitations
- Renders have no audio (see above), and come out at the game's resolution (set r_mode / r_customwidth first).
- No image-sequence output yet (AVI only).
