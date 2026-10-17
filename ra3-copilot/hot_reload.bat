@echo off
rem ============================================================
rem  Ra3-Copilot hot reload: unload in-game DLL -> rebuild -> inject
rem  Run while the game is in a match. No game restart needed.
rem  NOTE: pure ASCII only (GBK codepage safety).
rem ============================================================
setlocal
cd /d "%~dp0"

echo [1/3] unload running instance (no-op if not loaded)...
injector.exe --unload
if errorlevel 1 (
    echo [x] unload failed - aborting. If unload was aborted in-game,
    echo     check ra3copilot_log.txt for the [unload] lines.
    exit /b 1
)

echo [2/3] build...
call build.bat
if errorlevel 1 (
    echo [x] build failed
    exit /b 1
)

echo [3/3] inject...
injector.exe
if errorlevel 1 (
    echo [x] inject failed
    exit /b 1
)

echo.
echo HOT_RELOAD_OK
exit /b 0
