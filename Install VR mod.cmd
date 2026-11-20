@echo off
rem ===========================================================================
rem  HEAVY RAIN VR MOD - INSTALL (run it once)
rem
rem  Put this whole HR_VR_MOD folder inside your Heavy Rain folder (the one
rem  containing HeavyRain.exe) and run this file. It copies the mod
rem  (dinput8.dll) next to HeavyRain.exe and sets the game's graphics setting
rem  Display mode to Windowed (the mod writes its own hrvr.ini at the first start).
rem  After that just start SteamVR and the game as usual.
rem
rem  The work is done by tools\hrvr_setup.ps1 - read it to see every step.
rem ===========================================================================
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\hrvr_setup.ps1" -Mode Install & exit /b 0
