@echo off
rem ============================================================
rem  RA3-Copilot - one-click deploy (x86 injector)
rem
rem  IMPORTANT: injection MUST go through the 32-bit injector.exe.
rem  The game is a 32-bit process; a 64-bit CreateRemoteThread cannot
rem  start a thread in a 32-bit process (WOW64 boundary). python is
rem  used for diagnostics only (ReadProcessMemory works across bitness).
rem
rem  Python lookup: RA3_PY env var > python on PATH > known fallback.
rem  Keep this file pure ASCII + CRLF.
rem ============================================================
setlocal
cd /d "%~dp0"

net session >nul 2>&1
if errorlevel 1 (
    echo [*] Requesting administrator rights...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

echo [1/2] Injecting via x86 injector.exe ...
"%~dp0injector.exe"
echo.

echo [2/2] Diagnostics ...
if defined RA3_PY set "PY=%RA3_PY%"
if not defined PY (
    where python >nul 2>&1 && set "PY=python"
)
if not defined PY (
    where py >nul 2>&1 && set "PY=py"
)
if defined PY (
    "%PY%" "%~dp0tools\inject_and_probe.py" --no-inject
) else (
    echo [!] python not found - diagnostics skipped ^(set RA3_PY to override^)
    del "%~dp0tools\inject_probe_result.txt" >nul 2>&1
)
echo.

echo ================ RESULT ================
type "%~dp0tools\inject_probe_result.txt" 2>nul
echo ========================================
echo.
echo Back in game: press INSERT to show/hide the overlay menu.
echo (the menu is drawn INSIDE the game frame via MinHook inline hooks on d3d9
echo  Present/EndScene - it is not a separate window and does not take focus.)
echo.
pause
