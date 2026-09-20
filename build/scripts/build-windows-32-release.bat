@echo off
set W64DEVKIT_X86_BIN_PATH=C:\progs\w64devkit-x86\bin
set PATH=%W64DEVKIT_X86_BIN_PATH%;%PATH%

@echo on

:: project setup
set PROJECT_NAME=cuikcap
set PROJECT_ROOT=C:\dev\%PROJECT_NAME%
set BUILD_OUT_PATH=%PROJECT_ROOT%\build\windows-release-32
if not exist %BUILD_OUT_PATH% mkdir %BUILD_OUT_PATH%

gcc -nostdlib -fno-builtin %PROJECT_ROOT%\main.c ^
    -std=c99 ^
    -pipe ^
    -Os ^
    -s ^
    -fno-ident ^
    -ffunction-sections ^
    -fdata-sections ^
    -Wl,--gc-sections ^
    -Wl,--build-id=none ^
    -m32 ^
-o %BUILD_OUT_PATH%\cuikcap32.exe -lgdi32 -luser32 -lkernel32
if %ERRORLEVEL% neq 0 (
    echo Build failed.
) else (
    echo Build succeeded.
)