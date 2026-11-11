KatarnEngine for OpenJK Multiplayer
===================================

Plain OpenJK multiplayer (stock OpenJK exe, game code, menus and server browser) running the KatarnEngine
DirectX 12 renderer: deferred lighting, SSAO, screen-space reflections, reflection probes, HDR bloom, rend2
normal / parallax / specular maps, and HD worlds (.kworld) with sun shadows where a map has one.

Needs: Windows 10/11, a DirectX 12 graphics card, and Jedi Academy (Steam) for its base assets.

Play
  KatarnEngine_MP.bat     OpenJK MP with KatarnEngine (extra arguments are passed on, e.g. +connect <address>)
  OpenGL_Fallback_MP.bat  the same with OpenJK's OpenGL renderer

Any OpenJK MP install can use the renderer too: copy rd-katarn_x86_64.dll next to openjk.x86_64.exe and start with
+set cl_renderer rd-katarn (the renderer must come from the same OpenJK version as the exe).

Console (~)
  kt_ssr, kt_ssao, kt_probes, kt_probeScale, kt_parallax, kt_bloom, kt_exposure, kt_tonemap ...
  KatarnEngine settings (see the KatarnEngine README). katarn_log.txt next to the exe logs the renderer.

Files
  openjk.x86_64.exe, SDL2.dll      OpenJK multiplayer (unmodified OpenJK source)
  rd-katarn_x86_64.dll             KatarnEngine renderer for OpenJK MP
  rd-vanilla_x86_64.dll            OpenJK's OpenGL renderer (fallback)
  home\base\                       jampgame / cgame / ui x86_64 DLLs built from the same source; config, screenshots

HD worlds (Naboo Theed)
  Naboo_Theed_MP.bat      a local free-for-all on naboo_theed (or: devmap naboo_theed; it is also in Create Server)
  KatarnEngine_MP.bat reads the .kworld files straight from KatarnEngine's home\MD\maps (kt_worldPath) and copies
  each one's collision .bsp into home\base\maps when it is newer, so a re-cook reaches MP on the next start.
  Elsewhere: put <map>.kworld in home\base\maps or point kt_worldPath at its folder; the server needs the .bsp.

Online play: servers and other players use their usual files; the renderer only changes how this client draws.
