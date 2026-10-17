@echo off
rem ============================================================
rem  Ra3-Copilot one-click build (x86 target: 32-bit game)
rem  Output: ra3copilot.dll + injector.exe
rem  NOTE: pure ASCII only (GBK codepage safety).
rem  NOTE: self-contained toolchain env - does NOT call vcvars32.bat,
rem        because reg.exe (used by vcvars for SDK probing) is blocked
rem        by this machine's security policy.
rem ============================================================
setlocal
cd /d "%~dp0"

set "MSVC=C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Tools\MSVC\14.29.30133"
set "SDK=C:\Program Files (x86)\Windows Kits\10"
set "SDKV=10.0.19041.0"

if not exist "%MSVC%\bin\Hostx86\x86\cl.exe" (
    echo [x] cl.exe not found under MSVC dir
    exit /b 1
)

set "PATH=%MSVC%\bin\Hostx86\x86;%PATH%"
set "INCLUDE=%MSVC%\include;%SDK%\Include\%SDKV%\um;%SDK%\Include\%SDKV%\shared;%SDK%\Include\%SDKV%\ucrt;%SDK%\Include\%SDKV%\winrt"
set "LIB=%MSVC%\lib\x86;%SDK%\Lib\%SDKV%\um\x86;%SDK%\Lib\%SDKV%\ucrt\x86"

del /q *.obj >nul 2>&1
if exist obj_mh rmdir /s /q obj_mh >nul 2>&1
mkdir obj_mh >nul 2>&1

echo [1/4] MASM assembling stubs.asm
ml /nologo /c /coff src\stubs.asm
if errorlevel 1 (
    echo [x] ml failed
    exit /b 1
)

echo [2/4] Building MinHook (isolated obj dir, avoids hook.obj clash)
cl /nologo /O2 /c /Fo:obj_mh\ ^
    /I third_party\minhook-1.3.3\include /I third_party\minhook-1.3.3\src ^
    third_party\minhook-1.3.3\src\buffer.c third_party\minhook-1.3.3\src\hook.c ^
    third_party\minhook-1.3.3\src\trampoline.c third_party\minhook-1.3.3\src\hde\hde32.c
if errorlevel 1 (
    echo [x] MinHook build failed
    exit /b 1
)

echo [3/4] Building ra3copilot.dll
if not exist build_out mkdir build_out >nul 2>&1
cl /nologo /O2 /EHsc /W3 /utf-8 /LD /Fe:build_out\ra3copilot.dll ^
    src\dllmain.cpp src\overlay.cpp src\features.cpp src\actions.cpp src\battlefield.cpp src\aicmd.cpp src\symres.cpp src\hook.cpp src\httpapi.cpp stubs.obj ^
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
    echo [!] root ra3copilot.dll is locked by preview/AV.
    echo     Fresh build is at build_out\ra3copilot.dll - copy it manually.
)
copy /Y build_out\ra3copilot.map ra3copilot.map >nul 2>&1

echo [4/4] Building injector.exe
cl /nologo /O2 /EHsc /W3 /utf-8 /Fe:injector.exe src\injector.cpp ^
    /link shell32.lib user32.lib advapi32.lib ws2_32.lib
if errorlevel 1 (
    echo [x] injector build failed
    exit /b 1
)

del /q *.obj >nul 2>&1
if exist obj_mh rmdir /s /q obj_mh >nul 2>&1
echo.
echo BUILD_OK
echo   ra3copilot.dll  - injected module
echo   injector.exe    - launcher (start game first, then run this)
exit /b 0
