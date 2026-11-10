KatarnEngine for OpenJK
=======================

Plain OpenJK single player (stock OpenJK exe, game code and menus) running the KatarnEngine DirectX 12 renderer,
with the Naboo Theed HD map and Tali as the player.

Needs: Windows 10/11, a DirectX 12 graphics card, and Jedi Academy (Steam) for its base assets.

Play
  Play_Naboo.bat          starts straight into Naboo Theed as Tali
  KatarnEngine_OpenJK.bat starts at the normal Jedi Academy menu (type "naboo" in the console to load Theed)

Console (~)
  give all     every weapon, including the saber (F5 does the same)
  tali         switch the player model to Tali
  kt_fogScale, kt_tonemap, kt_ssr, kt_shadows ...   KatarnEngine settings (see the KatarnEngine README)

Files
  openjk_sp.x86_64.exe, SDL2.dll   OpenJK single player (unmodified OpenJK source)
  rdsp-katarn_x86_64.dll           KatarnEngine renderer for OpenJK
  rdsp-vanilla_x86_64.dll          OpenJK's OpenGL renderer (fallback: +set cl_renderer rdsp-vanilla)
  home\katarn\                     game folder: jagamex86_64.dll, the Naboo BSP, Tali, autoexec_sp.cfg, saves
  home\base\jagamex86_64.dll       the same game DLL, so the base folder never picks up another one

Paths (top of the .bat files)
  JA = Jedi Academy GameData folder (base\assets0.pk3 lives there)
  KE = KatarnEngine's home\MD folder, where Naboo was cooked (naboo_theed.kworld and .bsp)

Tali: drop your own Tali pk3 into home\katarn. If none is there, zz_tali.pk3 is copied from Movie Duels.
