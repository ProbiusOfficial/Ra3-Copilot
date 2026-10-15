@echo off
rem Build the tiny GUI clicker (x86 or x64 - builds for the host arch, fine either way).
rem Pure ASCII + CRLF only.
setlocal
cd /d "%~dp0"

set "MSVC=C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Tools\MSVC\14.29.30133"
set "SDK=C:\Program Files (x86)\Windows Kits\10"
set "SDKV=10.0.19041.0"

if not exist "%MSVC%\bin\Hostx86\x86\cl.exe" (
    echo [x] cl.exe not found
    exit /b 1
)

set "PATH=%MSVC%\bin\Hostx86\x86;%PATH%"
set "INCLUDE=%MSVC%\include;%SDK%\Include\%SDKV%\um;%SDK%\Include\%SDKV%\shared;%SDK%\Include\%SDKV%\ucrt"
set "LIB=%MSVC%\lib\x86;%SDK%\Lib\%SDKV%\um\x86;%SDK%\Lib\%SDKV%\ucrt\x86"

cl /nologo /O2 /EHsc /W3 /utf-8 /Fe:click_btn.exe click_btn.cpp /link user32.lib shell32.lib
if errorlevel 1 (
    echo [x] build failed
    exit /b 1
)
del /q *.obj >nul 2>&1
echo BUILD_OK click_btn.exe
exit /b 0
