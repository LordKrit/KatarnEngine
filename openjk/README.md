# KatarnEngine for OpenJK

The KatarnEngine DirectX 12 renderer ported to plain [OpenJK](https://github.com/JACoders/OpenJK) single player
(`rdsp-katarn_x86_64.dll`), so it runs with the stock OpenJK exe, game code and menus.

- `openjk-katarn.patch`: one commit on top of OpenJK `260c59c2`. Adds `code/rd-katarn` (OpenJK's rd-vanilla front end
  plus the `katarn/` D3D12 back end), the `BuildSPKatarn` CMake option, a `void*` size check for MinGW cross builds,
  and a `SHADER_MAX_VERTEXES` guard (the renderer uses 16000).
- `package/`: the launch scripts and config used for the Naboo Theed + Tali build.

Build (MinGW-w64 cross compile from Linux, as used here):

    git clone https://github.com/JACoders/OpenJK && cd OpenJK && git checkout 260c59c2 && git am ../openjk-katarn.patch
    cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchains/x86_64-w64-mingw32.cmake -DCMAKE_BUILD_TYPE=Release \
          -DBuildMPEngine=OFF -DBuildMPDed=OFF -DBuildMPGame=OFF -DBuildMPCGame=OFF -DBuildMPUI=OFF -DBuildMPRdVanilla=OFF \
          -DBuildMPRend2=OFF -DBuildJK2SPEngine=OFF -DBuildJK2SPGame=OFF -DBuildJK2SPRdVanilla=OFF
    make -C build -j16

Run with `+set cl_renderer rdsp-katarn`. HD worlds (`<map>.kworld`) are found in `maps/` of the game folder or in
the folder named by `kt_worldPath`.
