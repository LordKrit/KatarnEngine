@echo off
rem Starts a local KatarnEngine MP game on Naboo Theed (free for all). Add bots from the console: addbot <name>
call "%~dp0KatarnEngine_MP.bat" +set g_gametype 0 +set sv_pure 0 +devmap naboo_theed %*
