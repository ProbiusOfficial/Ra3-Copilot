@echo off
rem ============================================================
rem  Reload the copilot DLL into an ALREADY-RUNNING game that
rem  was started elevated. Must be run as Administrator.
rem  (No rebuild: uses the ra3copilot.dll produced by build.bat)
rem ============================================================
setlocal
cd /d "%~dp0"

echo [1/2] unload...
injector.exe --unload
if errorlevel 1 echo [i] nothing to unload (or unload refused) - continuing

echo [2/2] inject...
injector.exe
if errorlevel 1 (
    echo [x] inject failed
    pause
    exit /b 1
)

echo.
echo RELOAD_OK
pause
