@echo off
rem KatarnEngine for OpenJK: plain OpenJK single player (menus, game code) with the KatarnEngine DirectX 12 renderer.
rem Edit JA= if Jedi Academy is installed somewhere else.
setlocal
cd /d "%~dp0"
set "JA=D:\Steam\steamapps\common\Jedi Academy\GameData"
set "GAME=%~dp0home\katarn"

if not exist "%JA%\base\assets0.pk3" (
  echo Jedi Academy's base\assets0.pk3 was not found in "%JA%". Edit JA= in this file.
  pause
  exit /b 1
)
if not exist "%GAME%\maps\naboo_theed.kworld" (
  echo Naboo is missing from home\katarn\maps. Run Copy_Naboo_Into_Package.bat first.
  pause
  exit /b 1
)

set ARGS=+set fs_basepath "%JA%" +set fs_homepath "%~dp0home" +set fs_game katarn +set cl_renderer rdsp-katarn +set g_char_model tali +set g_char_skin_head model_default +set g_char_skin_torso model_default +set g_char_skin_legs model_default
start "" openjk_sp.x86_64.exe %ARGS% +set r_mode -1 +set r_customwidth 2560 +set r_customheight 1440 +set cg_fov 80 +set cg_fovAspectAdjust 1 +set cg_fovViewmodelAdjust 1
