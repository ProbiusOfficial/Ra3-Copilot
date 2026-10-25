@echo off
rem ============================================================
rem  Ra3-Copilot one-click build (x86 target: 32-bit game)
rem  Output:
rem    build_out\Ra3Copilot.exe  - single-file release (launcher + DLL + MCP)
rem    ra3copilot.dll            - injected module (dev / hot reload)
rem    injector.exe              - command-line injector (dev)
rem  NOTE: pure ASCII only (GBK codepage safety).
rem  NOTE: self-contained toolchain env - does NOT call vcvars32.bat,
rem        because reg.exe (used by vcvars for SDK probing) is blocked
rem        by this machine's security policy.
rem ============================================================
setlocal
cd /d "%~dp0"

rem CI sets RA3_TOOLCHAIN_READY=1 after vcvars; then the pinned paths below are skipped.
if "%RA3_TOOLCHAIN_READY%"=="1" goto toolchain_ok

set "MSVC=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207"
set "SDK=C:\Program Files (x86)\Windows Kits\10"
set "SDKV=10.0.22621.0"

if not exist "%MSVC%\bin\Hostx86\x86\cl.exe" (
    echo [x] cl.exe not found under MSVC dir
    exit /b 1
)
if not exist "%SDK%\bin\%SDKV%\x86\rc.exe" (
    echo [x] rc.exe not found under SDK dir
    exit /b 1
)

set "PATH=%SDK%\bin\%SDKV%\x86;%MSVC%\bin\Hostx86\x86;%PATH%"
set "INCLUDE=%MSVC%\include;%SDK%\Include\%SDKV%\um;%SDK%\Include\%SDKV%\shared;%SDK%\Include\%SDKV%\ucrt;%SDK%\Include\%SDKV%\winrt"
set "LIB=%MSVC%\lib\x86;%SDK%\Lib\%SDKV%\um\x86;%SDK%\Lib\%SDKV%\ucrt\x86"

:toolchain_ok

del /q *.obj >nul 2>&1
if exist obj_mh rmdir /s /q obj_mh >nul 2>&1
mkdir obj_mh >nul 2>&1
if not exist build_out mkdir build_out >nul 2>&1

echo [1/5] MASM assembling stubs.asm
ml /nologo /c /coff src\stubs.asm
if errorlevel 1 (
    echo [x] ml failed
    exit /b 1
)

echo [2/5] Building MinHook (isolated obj dir, avoids hook.obj clash)
cl /nologo /O2 /MT /c /Fo:obj_mh\ ^
    /I third_party\minhook-1.3.3\include /I third_party\minhook-1.3.3\src ^
    third_party\minhook-1.3.3\src\buffer.c third_party\minhook-1.3.3\src\hook.c ^
    third_party\minhook-1.3.3\src\trampoline.c third_party\minhook-1.3.3\src\hde\hde32.c
if errorlevel 1 (
    echo [x] MinHook build failed
    exit /b 1
)

echo [3/5] Building ra3copilot.dll
cl /nologo /O2 /MT /EHsc /W3 /utf-8 /LD /Fe:build_out\ra3copilot.dll ^
    src\dllmain.cpp src\overlay.cpp src\features.cpp src\actions.cpp src\protocol.cpp src\battlefield.cpp src\aicmd.cpp src\symres.cpp src\hook.cpp src\httpapi.cpp src\scriptobj.cpp stubs.obj ^
    obj_mh\buffer.obj obj_mh\hook.obj obj_mh\trampoline.obj obj_mh\hde32.obj ^
    imgui\imgui.cpp imgui\imgui_draw.cpp imgui\imgui_tables.cpp imgui\imgui_widgets.cpp ^
    imgui\backends\imgui_impl_dx9.cpp imgui\backends\imgui_impl_win32.cpp ^
    /I imgui /I imgui\backends /I src /I third_party\minhook-1.3.3\include /I third_party\minhook-1.3.3\src ^
    /link d3d9.lib user32.lib gdi32.lib dwmapi.lib ws2_32.lib /MAP:build_out\ra3copilot.map
if errorlevel 1 (
    echo [x] DLL build failed
    exit /b 1
)
rem copy out of build_out (root copy may be briefly locked by preview/AV handles)
copy /Y build_out\ra3copilot.dll ra3copilot.dll >nul 2>&1
if errorlevel 1 (
    echo *** DLL_STALE *** root ra3copilot.dll NOT updated - locked by running game or AV.
    echo     Run injector.exe --unload first, then copy build_out\ra3copilot.dll manually.
)
copy /Y build_out\ra3copilot.map ra3copilot.map >nul 2>&1

echo [4/5] Building Ra3Copilot.exe (single-file launcher)
rc /nologo /fo build_out\launcher.res res\launcher.rc
if errorlevel 1 (
    echo [x] resource compile failed
    exit /b 1
)
cl /nologo /O2 /MT /EHsc /W3 /utf-8 /DUNICODE /D_UNICODE ^
    /Fe:build_out\Ra3Copilot.exe src\launcher.cpp src\injectcore.cpp build_out\launcher.res ^
    imgui\imgui.cpp imgui\imgui_draw.cpp imgui\imgui_tables.cpp imgui\imgui_widgets.cpp ^
    imgui\backends\imgui_impl_dx9.cpp imgui\backends\imgui_impl_win32.cpp ^
    /I src /I res /I imgui /I imgui\backends ^
    /link /SUBSYSTEM:WINDOWS /MANIFEST:NO shell32.lib comdlg32.lib advapi32.lib user32.lib gdi32.lib d3d9.lib ws2_32.lib
if errorlevel 1 (
    echo [x] launcher build failed
    exit /b 1
)
copy /Y build_out\Ra3Copilot.exe Ra3Copilot.exe >nul 2>&1
if errorlevel 1 (
    echo *** LAUNCHER_STALE *** root Ra3Copilot.exe NOT updated - a running instance holds it.
    echo     Close Ra3Copilot.exe first, then copy build_out\Ra3Copilot.exe manually.
)
rem injector.exe / hot_reload.bat 读的是根目录这份 DLL；不拷就会一直注入旧版
copy /Y build_out\ra3copilot.dll ra3copilot.dll >nul 2>&1

echo [5/5] Building injector.exe
cl /nologo /O2 /MT /EHsc /W3 /utf-8 /Fe:injector.exe src\injector.cpp ^
    /link shell32.lib user32.lib advapi32.lib ws2_32.lib
if errorlevel 1 (
    echo [x] injector build failed
    exit /b 1
)

del /q *.obj >nul 2>&1
if exist obj_mh rmdir /s /q obj_mh >nul 2>&1
echo.
echo BUILD_OK
echo   build_out\Ra3Copilot.exe  - single-file release for players
echo   ra3copilot.dll            - injected module (dev)
echo   injector.exe              - command-line injector (dev)
exit /b 0
