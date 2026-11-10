# KatarnEngine

KatarnEngine is a Direct3D 12 renderer for **Movie Duels** / **OpenJK** single player (Star Wars Jedi Knight: Jedi Academy),
with the look of a modern engine (Unreal / Frostbite style): physically based deferred lighting, HD worlds cooked from
Blender, and rend2-compatible materials.

It is a fork of [MovieDuels-SJE-SP-Rend2](https://github.com/MovieDuels/MovieDuels-SJE-SP-Rend2) (itself built on
[OpenJK](https://github.com/JACoders/OpenJK)). The renderer is `code/rd-katarn` (built as
`MovieDuels-rdsp-katarn_x86_64.dll`); the game, the engine and the other renderers are unchanged from upstream apart
from small fixes. See [README_MovieDuels.md](README_MovieDuels.md) for the original project.

## Features
- **Direct3D 12**, bindless resources; the game's GL 1.x calls are translated to D3D12 (`code/rd-katarn/katarn/kt_gl.cpp`)
- **Deferred PBR**: GBuffer, GGX lighting, cascaded sun shadows, SSAO, screen-space reflections, volumetric sun shafts,
  height fog, HDR with auto exposure, bloom, ACES tone mapping, FXAA
- **HD worlds** (`.kworld`): Blender scenes exported and cooked into clustered geometry with LODs, baked materials,
  BC7 textures, terrain splat materials, still and flowing water, lights and sky (tools in `tools/katarn`)
- **rend2-compatible materials**: `.shader` and `.mtr` files, `normalMap`, `rmoMap` / `ormMap`, `specularMap`, `skin`;
  Ghoul2 surfaces up to 16000 vertices
- **Characters** lit per pixel: by the HD world's sun and sky, or by the light grid of classic BSP maps
- **Lightsabers**: blade light on the scene, rend2-style dynamic glow, white-hot core
- Water / waterfall FX pass, crash reports (`katarn_crash.txt`), log (`katarn_log.txt`)

## Building (Windows x64)
```
cmake -B build -DBuildSPKatarn=ON
cmake --build build --config Release --target MovieDuels-rdsp-katarn_x86_64
```
Also builds with MinGW-w64 (cross compile from Linux). Shaders are HLSL (`code/rd-katarn/katarn/shaders`), compiled at
run time (shader model 5.1); after editing one, run `python3 gen.py` in that folder to re-embed them.

Run Movie Duels with `+set cl_renderer rdsp-katarn`.

## HD world tools (`tools/katarn`)
- `katarn_export.py`: Blender 5.2 exporter (meshes, materials as node graphs, lights, sky)
- `katarn_cook.cpp`: cooker (clusters, LODs, material baking, BC7, collision `.map` for WzMap)
- `Export_NabooTheed.bat`, `Cook_NabooTheed.bat`, `Collision_NabooTheed.bat`: example pipeline

## Useful cvars
`kt_shadows`, `kt_ssao`, `kt_ssr`, `kt_volumetric`, `kt_bloom`, `kt_exposure`, `kt_fxaa`, `kt_dynamicGlow`,
`kt_saberLight`, `kt_saberGlowScale`, `kt_debugView` (1 albedo, 2 normals, 3 AO, 4 shadows), `kt_speeds`

## License
GPLv2, like OpenJK and Movie Duels (see [LICENSE.txt](LICENSE.txt)). Jedi Academy and its assets belong to their owners;
this repository contains source code only.
