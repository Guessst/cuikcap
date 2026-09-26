:: setup MSVC environment
@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

@echo on
:: project setup
set PROJECT_NAME=cuikcap
set PROJECT_ROOT=C:\dev\%PROJECT_NAME%
set BUILD_OUT_PATH=%PROJECT_ROOT%\build\windows-release-msvc-64
if not exist "%BUILD_OUT_PATH%" mkdir "%BUILD_OUT_PATH%"

cl "%PROJECT_ROOT%\main.c" ^
    /nologo ^
    /std:c99 ^
    /W4 ^
    /O1 ^
    /GL ^
    /Gy ^
    /Gw ^
    /GS- ^
    /Zc:inline ^
    /Fe"%BUILD_OUT_PATH%\cuikcap.exe" ^
    /Fo"%BUILD_OUT_PATH%\\" ^
    /link ^
    /LTCG ^
    /NODEFAULTLIB ^
    /ENTRY:mainCRTStartup ^
    /SUBSYSTEM:WINDOWS ^
    /OPT:REF ^
    /OPT:ICF ^
    /MERGE:.rdata=.text ^
    /MERGE:.pdata=.text ^
    kernel32.lib user32.lib gdi32.lib

if %ERRORLEVEL% neq 0 (
    echo Build failed.
    exit /b %ERRORLEVEL%
) else (
    echo Build succeeded.
    exit /b 0
)