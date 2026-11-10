@echo off
rem Exports Naboo_Theed.blend for KatarnEngine (nothing in the .blend is changed or saved).
rem Output: K:\Projects\KatarnEngine\export\naboo_theed
setlocal
set "ROOT=%~dp0.."
set "BLEND=%ROOT%\Naboo_Theed.blend"
set "OUT=%ROOT%\export\naboo_theed"
set "BL="
for %%P in (
  "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe"
  "C:\Program Files\Blender Foundation\Blender 5.1\blender.exe"
  "C:\Program Files\Blender Foundation\Blender 5.0\blender.exe"
  "C:\Program Files (x86)\Steam\steamapps\common\Blender\blender.exe"
  "D:\Steam\steamapps\common\Blender\blender.exe"
  "D:\SteamLibrary\steamapps\common\Blender\blender.exe"
) do if exist %%P set "BL=%%~P"
if not defined BL for /f "delims=" %%B in ('where blender 2^>nul') do if not defined BL set "BL=%%B"
if not defined BL (
  echo Could not find blender.exe. Drag blender.exe onto this window and press Enter:
  set /p "BL="
)
set "BL=%BL:"=%"
echo Blender: %BL%
echo Exporting %BLEND%
echo        to %OUT%
"%BL%" -b "%BLEND%" -P "%~dp0katarn_export.py" -- "%OUT%"
echo.
echo Finished. The summary is in %OUT%\export_log.txt
pause
