@echo off
rem Sync freshly built DLL from build_out to project root.
rem Use this after closing the game (the running game locks ra3copilot.dll).
setlocal
cd /d "%~dp0"
if not exist build_out\ra3copilot.dll (
    echo [x] build_out\ra3copilot.dll not found - run build.bat first
    pause
    exit /b 1
)
copy /Y build_out\ra3copilot.dll ra3copilot.dll >nul
if errorlevel 1 (
    echo [x] ra3copilot.dll is locked. Close the game ^(and any preview^) and retry.
    pause
    exit /b 1
)
copy /Y build_out\ra3copilot.map ra3copilot.map >nul 2>&1
echo [ok] ra3copilot.dll updated. Now run deploy.bat to inject.
pause
