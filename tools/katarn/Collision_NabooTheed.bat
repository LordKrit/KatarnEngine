@echo off
rem Rebuilds only the collision of Naboo Theed from the .kworld you already cooked (no re-cook):
rem   home\MD\maps\naboo_theed.map  ->  WzMap  ->  home\MD\maps\naboo_theed.bsp
rem Options for katarn_cook: --collision-budget 250000 (more = more accurate, slower WzMap),
rem   --collision-error 4 (floors, units), --collision-walls 4 (walls = error x this), --collision-pad 8192 (margin around the city)
setlocal
cd /d "%~dp0"
for %%I in ("%~dp0..") do set "ROOT=%%~fI"
set "GAMEDATA=D:\Steam\steamapps\common\Jedi Academy\GameData"
set "OUT=%ROOT%\home\MD\maps"
echo Building collision from the cooked world...
katarn_cook.exe "%ROOT%\export\naboo_theed" "%OUT%" naboo_theed --collision-only
if errorlevel 1 goto fail
move /y "%OUT%\katarn_hd.shader" "%ROOT%\home\MD\shaders\katarn_hd.shader" >nul
echo.
echo Compiling the collision BSP (WzMap, this is the slow part)...
if exist "%OUT%\naboo_theed.bsp" move /y "%OUT%\naboo_theed.bsp" "%OUT%\naboo_theed_old.bsp" >nul
WzMap.exe -game ja -fs_basepath "%GAMEDATA%" -fs_basepath "%ROOT%\home" -fs_game MD -meta -notjunc -nosubdivide "%OUT%\naboo_theed.map" > "%OUT%\naboo_theed_bsp_log.txt" 2>&1
if not exist "%OUT%\naboo_theed.bsp" goto fail
echo.
echo Done. Start bin\KatarnEngine_NabooTheed.bat
pause
exit /b 0
:fail
echo.
echo Something went wrong - send me the logs in %OUT%
if exist "%OUT%\naboo_theed_old.bsp" if not exist "%OUT%\naboo_theed.bsp" move /y "%OUT%\naboo_theed_old.bsp" "%OUT%\naboo_theed.bsp" >nul
pause
exit /b 1
