param(
    [string]$BuildDir = "",
    [string]$Configuration = "Release",
    [string]$ObsSourceDir = $env:OBS_SOURCE_DIR,
    [string]$ObsLibDir = $env:OBS_LIB_DIR,
    [string]$SimdeIncludeDir = $env:SIMDE_INCLUDE_DIR,
    [string]$CmakePrefixPath = $env:CMAKE_PREFIX_PATH
)

$ErrorActionPreference = "Stop"

$RootDir = Resolve-Path (Join-Path $PSScriptRoot "..")
if (!$BuildDir) {
    $BuildDir = Join-Path $RootDir "build-windows"
}

if (!$ObsSourceDir -or !(Test-Path (Join-Path $ObsSourceDir "libobs/obs-module.h"))) {
    throw "OBS_SOURCE_DIR must point to an OBS Studio source checkout."
}

if (!$ObsLibDir -or !(Test-Path (Join-Path $ObsLibDir "obs.lib"))) {
    throw "OBS_LIB_DIR must point to a directory containing obs.lib."
}

if (!$SimdeIncludeDir -or !(Test-Path (Join-Path $SimdeIncludeDir "simde/x86/sse2.h"))) {
    throw "SIMDE_INCLUDE_DIR must point to a directory containing simde/x86/sse2.h."
}

$cmakeArgs = @(
    "-S", $RootDir,
    "-B", $BuildDir,
    "-G", "Visual Studio 17 2022",
    "-A", "x64",
    "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
    "-DOBS_SOURCE_DIR=$ObsSourceDir",
    "-DOBS_LIB_DIR=$ObsLibDir",
    "-DSIMDE_INCLUDE_DIR=$SimdeIncludeDir"
)

if ($CmakePrefixPath) {
    $cmakeArgs += "-DCMAKE_PREFIX_PATH=$CmakePrefixPath"
}

& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) {
    throw "CMake configure failed."
}

& cmake --build $BuildDir --config $Configuration
if ($LASTEXITCODE -ne 0) {
    throw "CMake build failed."
}

Write-Host "Built Windows plugin in $BuildDir ($Configuration)."
