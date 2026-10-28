# ReShade with Cinematic Studio

ReShade is a separate program that you install yourself. It's free, from reshade.me. This build is set up so its effects work, depth-based effects included.

1. Run the ReShade installer and choose
   `GameData\MD-cinematic studio\Phase1-test\MovieDuels-SP.x86_64.exe`.
2. For the rendering API, pick **OpenGL**. Then pick the effect packages you want (the standard ones are fine).
3. Start the studio with `Launch_Cinematic_Studio.bat` and press **Home** to open ReShade's overlay.
4. **Depth effects (DOF, ambient occlusion, depth fog, MXAO and so on):**
   - Use the rend2 renderer (`cl_renderer rdsp-rend2`).
   - This build copies the world's depth to the window for ReShade. The setting is `r_reshadeDepth 1`, and it's on by default.
   - In ReShade's **Add-ons / Generic Depth** tab, pick the depth buffer that matches your resolution. If the depth comes out upside down, set `RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN=1` in the global preprocessor definitions.

Notes
- ReShade's effects also apply to the studio's menus. Hide the editor with **H** to see clean frames.
- **The studio's frame-exact AVI render (RENDER tab) captures the frame before ReShade runs, so the AVI won't include ReShade effects.** To capture the ReShade look, use ReShade's own screenshot key or a screen recorder while the director preview plays.
- To remove ReShade, delete `opengl32.dll`, `ReShade.ini` and the `reshade-shaders` folder from Phase1-test.

# Atmosphere haze
`r_atmosDensity` now defaults to **0**, so haze no longer switches on when a map loads.
- To turn it on: `r_atmosDensity 0.00012`.
- To make it stick for one map: `grade save` writes it into that map's grade file.
- The sun shafts have their own switch: `r_atmosShafts 0` turns them off, and `r_atmos 0` turns the whole effect off.
