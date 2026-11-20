@echo off
rem ===========================================================================
rem  HEAVY RAIN VR MOD - UNINSTALL
rem
rem  Moves the mod's dinput8.dll (and, if you want, hrvr.ini and hrvr_logs) to
rem  the Recycle Bin and puts the game's Display mode back to what it was
rem  before the install. Nothing is deleted for good; your saves and the game's
rem  files are left alone.
rem
rem  The work is done by tools\hrvr_setup.ps1 - read it to see every step.
rem ===========================================================================
setlocal
rem (run from the game folder: a folder that is a window's current folder cannot be moved to the Recycle Bin)
cd /d "%~dp0.."
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\hrvr_setup.ps1" -Mode Uninstall & exit /b 0
