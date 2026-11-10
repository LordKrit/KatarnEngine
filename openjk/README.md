# KatarnEngine for OpenJK

The KatarnEngine DirectX 12 renderer ported to plain [OpenJK](https://github.com/JACoders/OpenJK), for both games:

| | DLL | select with |
|---|---|---|
| Single player | `rdsp-katarn_x86_64.dll` | `+set cl_renderer rdsp-katarn` |
| Multiplayer | `rd-katarn_x86_64.dll` | `+set cl_renderer rd-katarn` |

Both use the stock OpenJK exe, game code and menus.

- `openjk-katarn.patch`: the changes on top of OpenJK `260c59c2`. Adds `code/rd-katarn` (SP rd-vanilla front end plus
  the `katarn/` D3D12 back end), `codemp/rd-katarn` (the MP rd-vanilla front end with the same `katarn/` module), the
  `BuildSPKatarn` / `BuildMPKatarn` CMake options, a `void*` size check for MinGW cross builds, and a
  `SHADER_MAX_VERTEXES` guard (the SP renderer uses 16000).
- `package/`: launch scripts and config of the SP Naboo Theed + Tali build.
- `package_mp/`: launch scripts of the MP build.

Build (MinGW-w64 cross compile from Linux, as used here):

    git clone https://github.com/JACoders/OpenJK && cd OpenJK && git checkout 260c59c2 && git apply ../openjk-katarn.patch
    cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchains/x86_64-w64-mingw32.cmake -DCMAKE_BUILD_TYPE=Release \
          -DBuildMPDed=OFF -DBuildMPRend2=OFF -DBuildJK2SPEngine=OFF -DBuildJK2SPGame=OFF -DBuildJK2SPRdVanilla=OFF
    make -C build -j16

The renderer DLL must come from the same OpenJK version as the exe. HD worlds (`<map>.kworld`) are found in `maps/`
of the game folder or in the folder named by `kt_worldPath`.

The `katarn/` module is shared: `code/rd-katarn/katarn` and `codemp/rd-katarn/katarn` are copies of the same files
(MP builds define `KT_MP`).
