@echo off
rem Re-exports Naboo_Theed.blend and cooks it again (export ~2 min, cook ~30-40 min).
rem Use this after a KatarnEngine update that changes the exporter or the cooker.
setlocal
cd /d "%~dp0"
echo ==== 1/2 Export from Blender ====
echo. | call "%~dp0Export_NabooTheed.bat"
echo.
echo ==== 2/2 Cook ====
call "%~dp0Cook_NabooTheed.bat"
