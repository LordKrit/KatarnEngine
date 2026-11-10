@echo off
rem Copies the cooked Naboo Theed map (and Tali) into this folder so it runs on its own and can be zipped and shared.
rem Run it again after re-cooking Naboo. About 4.3 GB.
setlocal
cd /d "%~dp0"
set "KE=%~dp0..\home\MD"
set "JA=D:\Steam\steamapps\common\Jedi Academy\GameData"
set "GAME=%~dp0home\katarn"
if not exist "%GAME%\maps" mkdir "%GAME%\maps"
if not exist "%GAME%\shaders" mkdir "%GAME%\shaders"
if not exist "%KE%\maps\naboo_theed.kworld" (
  echo Could not find "%KE%\maps\naboo_theed.kworld". Edit KE= in this file.
  pause
  exit /b 1
)
echo Copying Naboo Theed (4 GB, takes a minute)...
copy /Y "%KE%\maps\naboo_theed.kworld" "%GAME%\maps\"
copy /Y "%KE%\maps\naboo_theed.bsp" "%GAME%\maps\"
if exist "%KE%\maps\naboo_theed.kenv" copy /Y "%KE%\maps\naboo_theed.kenv" "%GAME%\maps\"
copy /Y "%KE%\shaders\katarn_hd.shader" "%GAME%\shaders\"
if not exist "%GAME%\*tali*.pk3" if exist "%JA%\MD\zz_tali.pk3" copy /Y "%JA%\MD\zz_tali.pk3" "%GAME%\"
echo.
echo Done. home\katarn now holds:
dir /b "%GAME%" "%GAME%\maps"
pause
