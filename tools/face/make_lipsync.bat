@echo off
rem Make .lip lipsync files for voice lines, for the face system (docs/FACE_SYSTEM.md).
rem Needs Rhubarb Lip Sync (https://github.com/DanielSWolf/rhubarb-lip-sync/releases):
rem   unzip it and put rhubarb.exe (with its res folder) next to this file.
rem For .mp3 lines it also needs ffmpeg.exe next to this file (or on PATH); .wav and .ogg work directly.
rem
rem Usage: drag a folder of voice lines onto this file, or:  make_lipsync.bat "C:\path\to\sound\cine"
rem Each line.mp3 / line.wav / line.ogg gets a line.lip beside it. Put the .lip files in a pk3 at the
rem same path as the sound (e.g. sound/cine/dooku.lip next to sound/cine/dooku.mp3).
setlocal enabledelayedexpansion
set "HERE=%~dp0"
set "DIR=%~1"
if "%DIR%"=="" set "DIR=%CD%"
if not exist "%HERE%rhubarb.exe" (
  echo rhubarb.exe not found next to this script. Download Rhubarb Lip Sync and unzip it here.
  pause & exit /b 1
)
for %%F in ("%DIR%\*.wav" "%DIR%\*.ogg") do (
  echo %%~nxF
  "%HERE%rhubarb.exe" -q -f tsv --extendedShapes GHX -o "%%~dpnF.lip" "%%F"
)
for %%F in ("%DIR%\*.mp3") do (
  echo %%~nxF
  set "TMPW=%TEMP%\lipsync_%%~nF.wav"
  ffmpeg -loglevel error -y -i "%%F" -ac 1 -ar 22050 "!TMPW!" || "%HERE%ffmpeg.exe" -loglevel error -y -i "%%F" -ac 1 -ar 22050 "!TMPW!"
  "%HERE%rhubarb.exe" -q -f tsv --extendedShapes GHX -o "%%~dpnF.lip" "!TMPW!"
  del "!TMPW!" 2>nul
)
echo Done.
pause
