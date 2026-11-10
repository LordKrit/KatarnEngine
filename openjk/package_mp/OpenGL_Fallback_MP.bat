@echo off
rem The same OpenJK MP build with OpenJK's own OpenGL renderer (rd-vanilla), to compare or if DirectX 12 fails.
setlocal
cd /d "%~dp0"
set "JA=D:\Steam\steamapps\common\Jedi Academy\GameData"
start "" openjk.x86_64.exe +set fs_basepath "%JA%" +set fs_homepath "%~dp0home" +set cl_renderer rd-vanilla %*
