:: put gcc in PATH
@echo off
set W64DEVKIT_X64_BIN_PATH=C:\progs\w64devkit-x64\bin
set PATH=%W64DEVKIT_X64_BIN_PATH%;%PATH%

@echo on
:: project setup
set PROJECT_NAME=cuikcap
set PROJECT_ROOT=C:\dev\%PROJECT_NAME%
set BUILD_OUT_PATH=%PROJECT_ROOT%\build\windows-debug-64
if not exist %BUILD_OUT_PATH% mkdir %BUILD_OUT_PATH%

set USE_ANALYZER=0
set FTIME_REPORT_FLAG=-ftime-report

set "ANALYZER_FLAGS="
if "%USE_ANALYZER%"=="1" (
    set "ANALYZER_FLAGS=%FTIME_REPORT_FLAG% -fanalyzer -fanalyzer-checker=taint -Wno-analyzer-unsafe-call-within-signal-handler"
)

gcc -nostdlib -fno-builtin %PROJECT_ROOT%\main.c ^
    -std=c99 ^
    -Wall ^
    -Wextra ^
    -Werror ^
    -Wpedantic ^
    -Wshadow ^
    -Wconversion ^
    -Wnull-dereference ^
    -Wswitch-enum ^
    -Wstrict-prototypes ^
    -Wduplicated-cond ^
    -Wduplicated-branches ^
    -Wlogical-op ^
    -Wc++-compat ^
    -g3 ^
    %ANALYZER_FLAGS% ^
    -pipe ^
    -O0 ^
-o %BUILD_OUT_PATH%\cuikcap.exe -lgdi32 -luser32 -lkernel32

if %ERRORLEVEL% neq 0 (
    echo Build failed.
    exit /b %ERRORLEVEL%
) else (
    echo Build succeeded.
    exit /b 0
)