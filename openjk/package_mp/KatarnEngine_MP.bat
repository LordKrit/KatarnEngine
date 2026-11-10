@echo off
rem KatarnEngine for OpenJK multiplayer: plain OpenJK MP (menus, game code, servers) with the KatarnEngine DirectX 12
rem renderer. Edit JA= if Jedi Academy is installed somewhere else.
setlocal
cd /d "%~dp0"
set "JA=D:\Steam\steamapps\common\Jedi Academy\GameData"

if not exist "%JA%\base\assets0.pk3" (
  echo Jedi Academy's base\assets0.pk3 was not found in "%JA%". Edit JA= in this file.
  pause
  exit /b 1
)

start "" openjk.x86_64.exe +set fs_basepath "%JA%" +set fs_homepath "%~dp0home" +set cl_renderer rd-katarn %*
