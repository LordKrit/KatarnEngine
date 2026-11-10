@echo off
rem Cooks export\naboo_theed (from Export_NabooTheed.bat) into a KatarnEngine HD world:
rem   home\MD\maps\naboo_theed.kworld  - the visible world (clusters + LODs, baked materials, sky, lights)
rem   home\MD\maps\naboo_theed.bsp     - collision, spawn point (compiled with WzMap)
rem Logs: home\MD\maps\naboo_theed_cook_log.txt and naboo_theed_bsp_log.txt
setlocal
cd /d "%~dp0"
for %%I in ("%~dp0..") do set "ROOT=%%~fI"
set "GAMEDATA=D:\Steam\steamapps\common\Jedi Academy\GameData"
set "OUT=%ROOT%\home\MD\maps"
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%ROOT%\home\MD\shaders" mkdir "%ROOT%\home\MD\shaders"
echo Cooking Naboo Theed (this takes a while)...
katarn_cook.exe "%ROOT%\export\naboo_theed" "%OUT%" naboo_theed --quality 0 --texbudget 2500
if errorlevel 1 goto fail
move /y "%OUT%\katarn_hd.shader" "%ROOT%\home\MD\shaders\katarn_hd.shader" >nul
echo katarn_hd> "%ROOT%\home\MD\shaders\shaderlist.txt"
echo.
echo Compiling the collision BSP (WzMap)...
WzMap.exe -game ja -fs_basepath "%GAMEDATA%" -fs_basepath "%ROOT%\home" -fs_game MD -meta "%OUT%\naboo_theed.map" > "%OUT%\naboo_theed_bsp_log.txt" 2>&1
if not exist "%OUT%\naboo_theed.bsp" goto fail
echo.
echo Done. Start bin\KatarnEngine_NabooTheed.bat
pause
exit /b 0
:fail
echo.
echo Something went wrong - send me the logs in %OUT%
pause
exit /b 1
