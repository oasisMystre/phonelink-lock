#Requires -Version 5.1
<#
.SYNOPSIS
    Configures and builds PhoneLinkLock.

.PARAMETER Config
    Build configuration: Release (default) or Debug.

.EXAMPLE
    .\build.ps1
    .\build.ps1 -Config Debug
#>
param(
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $scriptDir

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    $vswherePath = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswherePath) {
        $vsPath = & $vswherePath -latest -requires Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath
        if ($vsPath) {
            $cmakePath = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
            if (Test-Path $cmakePath) {
                $env:PATH = "$cmakePath;" + $env:PATH
            }
        }
    }
}

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Write-Error "cmake was not found on PATH. Install CMake and/or run this from a 'Developer PowerShell for VS' so the MSVC toolchain is on PATH too."
    exit 1
}

# Prefer VS2022, fall back to VS2019 if that's what's installed.
$generator = "Visual Studio 17 2022"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
    $vsVersion = & $vswhere -latest -property catalog_productLineVersion 2>$null
    if ($vsVersion -eq "2019") { $generator = "Visual Studio 16 2019" }
}

Write-Host "=== Configuring ($Config) using '$generator' ===" -ForegroundColor Cyan
cmake -B build -G $generator -A x64
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }

Write-Host "=== Building ($Config) ===" -ForegroundColor Cyan
cmake --build build --config $Config
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

$exePath = Join-Path $scriptDir "build\$Config\PhoneLinkLock.exe"
if (Test-Path $exePath) {
    Write-Host ""
    Write-Host "=== Build succeeded ===" -ForegroundColor Green
    Write-Host "Executable: $exePath"
} else {
    throw "Build reported success but the .exe was not found at: $exePath"
}

if ($Config -eq "Release") {
    Write-Host "=== Building MSI Installer (Release only) ===" -ForegroundColor Cyan
    wix build phonelinklock.wxs -o build\PhoneLinkLock.msi
    if ($LASTEXITCODE -eq 0) {
        Write-Host "Built MSI: build\PhoneLinkLock.msi" -ForegroundColor Green
    } else {
        Write-Host "Failed to build MSI (ensure WiX v4 is installed)" -ForegroundColor Yellow
    }

    Write-Host "=== Building Setup Wrapper (Release only) ===" -ForegroundColor Cyan
    if (Test-Path "build_setup.ps1") {
        .\build_setup.ps1
    }
}
