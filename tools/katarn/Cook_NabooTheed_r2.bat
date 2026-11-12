@echo off
rem Naboo Theed for rend2 (and KatarnEngine): cooks export\naboo_theed straight into GameData\MD as naboo_theed_r2
rem   MD\maps\naboo_theed_r2.kworld   - the visible world: clusters + LODs, baked light per vertex, light grid
rem   MD\maps\naboo_theed_r2.bsp      - collision, sky box, spawn point (WzMap)
rem   MD\textures\kw_naboo_theed_r2\  - textures (.dds) and the sky box faces
rem   MD\shaders\kw_naboo_theed_r2.shader - the materials (rend2 keywords, edit freely)
rem In game (rend2 with kworld support): devmap naboo_theed_r2
rem Same lighter build as Cook_NabooTheed.bat (no crates / boxes / fences, no fire lights, ground leaves at 20%).
setlocal
cd /d "%~dp0"
for %%I in ("%~dp0..") do set "ROOT=%%~fI"
set "GAMEDATA=D:\Steam\steamapps\common\Jedi Academy\GameData"
set "MD=%GAMEDATA%\MD"
set "OUT=%MD%\maps"
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%MD%\shaders" mkdir "%MD%\shaders"
echo Cooking Naboo Theed for rend2 (cook + light bake, this takes a while)...
katarn_cook.exe "%ROOT%\export\naboo_theed" "%OUT%" naboo_theed_r2 --quality 0 --texbudget 2500 --collision-budget 500000 --skip "HeightFog,Sky_Cottonball,FX_,fxsrc_,DroidWreck,AATWreck,AATDebris,RubblePile,BD_FogBillboard,InvalidateLightPlane,WindowEnlightenPlane,Container_,Crate_,^Box_M,O_NAB_BoxPile,O_NAB_BoxMarketStand,Box_M_02_Pile,O_NAB_FenceModularSet,O_NAB_FenceMetal,O_NAB_FenceCloth" --skiplight "FX_WreckFire,Sphere" --thin "GroundLeaves=0.2"
if errorlevel 1 goto fail
move /y "%OUT%\katarn_hd.shader" "%MD%\shaders\katarn_hd.shader" >nul
echo.
echo Compiling the collision BSP (WzMap)...
WzMap.exe -game ja -fs_basepath "%GAMEDATA%" -fs_game MD -meta -notjunc -nosubdivide "%OUT%\naboo_theed_r2.map" > "%OUT%\naboo_theed_r2_bsp_log.txt" 2>&1
if not exist "%OUT%\naboo_theed_r2.bsp" goto fail
echo.
echo Done. In game: devmap naboo_theed_r2
pause
exit /b 0
:fail
echo.
echo Something went wrong - send me the logs in %OUT% (naboo_theed_r2_cook_log.txt, naboo_theed_r2_bsp_log.txt)
pause
exit /b 1
