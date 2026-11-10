@echo off
rem KatarnEngine for OpenJK: plain OpenJK single player (menus, game code) with the KatarnEngine DirectX 12 renderer.
rem Edit these two paths if your folders are elsewhere.
setlocal
cd /d "%~dp0"
set "JA=D:\Steam\steamapps\common\Jedi Academy\GameData"
set "KE=%~dp0..\home\MD"
set "GAME=%~dp0home\katarn"

if not exist "%JA%\base\assets0.pk3" (
  echo Jedi Academy's base\assets0.pk3 was not found in "%JA%". Edit JA= in this file.
  pause
  exit /b 1
)
if not exist "%GAME%\maps" mkdir "%GAME%\maps"
if not exist "%GAME%\shaders" mkdir "%GAME%\shaders"
rem Naboo: the BSP (collision, spawn) is copied here when it changes; the 4 GB .kworld is read where it was cooked
if exist "%KE%\maps\naboo_theed.bsp" xcopy /D /Y /Q "%KE%\maps\naboo_theed.bsp" "%GAME%\maps\" >nul
if exist "%KE%\shaders\katarn_hd.shader" xcopy /D /Y /Q "%KE%\shaders\katarn_hd.shader" "%GAME%\shaders\" >nul
rem Tali: put your own Tali pk3 in home\katarn (any name); zz_tali.pk3 is copied from Movie Duels if none is there
if not exist "%GAME%\*tali*.pk3" if exist "%JA%\MD\zz_tali.pk3" copy /Y "%JA%\MD\zz_tali.pk3" "%GAME%\" >nul

set ARGS=+set fs_basepath "%JA%" +set fs_homepath "%~dp0home" +set fs_game katarn +set cl_renderer rdsp-katarn +set kt_worldPath "%KE%\maps" +set g_char_model tali +set g_char_skin_head model_default +set g_char_skin_torso model_default +set g_char_skin_legs model_default
start "" openjk_sp.x86_64.exe %ARGS% +devmap naboo_theed
