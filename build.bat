@echo off
setlocal enabledelayedexpansion

REM PhoneLinkLock build script
REM Usage: build.bat [Debug|Release]   (default: Release)

set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release

set SCRIPT_DIR=%~dp0
cd /d "%SCRIPT_DIR%"

where cmake >nul 2>nul
if errorlevel 1 (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "!VSWHERE!" (
        for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -requires Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath`) do (
            set "VS_PATH=%%i"
        )
        if defined VS_PATH (
            set "PATH=!VS_PATH!\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;!PATH!"
        )
    )
)

where cmake >nul 2>nul
if errorlevel 1 (
    echo [ERROR] cmake was not found on PATH. Install CMake and/or run this
    echo         from a "Developer PowerShell/Command Prompt for VS" so both
    echo         cmake and the MSVC toolchain are available.
    exit /b 1
)

echo === Configuring (%CONFIG%) ===
cmake -B build -G "Visual Studio 17 2022" -A x64
if errorlevel 1 (
    echo [ERROR] CMake configure failed.
    exit /b 1
)

echo === Building (%CONFIG%) ===
cmake --build build --config %CONFIG%
if errorlevel 1 (
    echo [ERROR] Build failed.
    exit /b 1
)

set EXE_PATH=%SCRIPT_DIR%build\%CONFIG%\PhoneLinkLock.exe
if exist "%EXE_PATH%" (
    echo.
    echo === Build succeeded ===
    echo Executable: %EXE_PATH%
) else (
    echo [ERROR] Build reported success but the .exe was not found at:
    echo         %EXE_PATH%
    exit /b 1
)

endlocal
