@echo off
rem KatarnEngine for OpenJK multiplayer: plain OpenJK MP (menus, game code, servers) with the KatarnEngine DirectX 12
rem renderer. Edit JA= if Jedi Academy is installed somewhere else.
rem HD worlds (Naboo Theed): the .kworld files are read straight from KatarnEngine's home\MD\maps (no 5 GB copy),
rem and each one's collision .bsp is copied into home\base\maps when it is newer, so a re-cook reaches MP too.
setlocal
cd /d "%~dp0"
set "JA=D:\Steam\steamapps\common\Jedi Academy\GameData"
set "HD=%~dp0..\home\MD\maps"

if not exist "%JA%\base\assets0.pk3" (
  echo Jedi Academy's base\assets0.pk3 was not found in "%JA%". Edit JA= in this file.
  pause
  exit /b 1
)

if not exist "home\base\maps" mkdir "home\base\maps"
if not exist "home\base\shaders" mkdir "home\base\shaders"
if exist "%HD%" (
  for %%W in ("%HD%\*.kworld") do (
    if exist "%HD%\%%~nW.bsp" xcopy /D /Y /Q "%HD%\%%~nW.bsp" "home\base\maps\" >nul
  )
)
if exist "%~dp0..\home\MD\shaders\katarn_hd.shader" xcopy /D /Y /Q "%~dp0..\home\MD\shaders\katarn_hd.shader" "home\base\shaders\" >nul

start "" openjk.x86_64.exe +set fs_basepath "%JA%" +set fs_homepath "%~dp0home" +set cl_renderer rd-katarn +set kt_worldPath "%HD%" %*
