param(
    [string]$BuildDir = "",
    [string]$Configuration = "Release",
    [string]$PackageRoot = "",
    [string]$ZipPath = "",
    [string]$FfmpegExe = ""
)

$ErrorActionPreference = "Stop"

$RootDir = Resolve-Path (Join-Path $PSScriptRoot "..")
if (!$BuildDir) {
    $BuildDir = Join-Path $RootDir "build-windows"
}
if (!$PackageRoot) {
    $PackageRoot = Join-Path $RootDir "release/windows"
}
if (!$ZipPath) {
    $ZipPath = Join-Path $PackageRoot "VirtuEdit-OBS-Plugin-Windows-x64.zip"
}

$candidateDlls = @(
    (Join-Path $BuildDir "$Configuration/virtuedit-obs.dll"),
    (Join-Path $BuildDir "virtuedit-obs.dll")
)
$PluginDll = $candidateDlls | Where-Object { Test-Path $_ } | Select-Object -First 1
if (!$PluginDll) {
    throw "virtuedit-obs.dll was not found under $BuildDir."
}

$StageDir = Join-Path $PackageRoot "VirtuEdit-OBS-Plugin-Windows-x64"
$PackageDir = Join-Path $StageDir "virtuedit-obs"
if (Test-Path $StageDir) {
    Remove-Item -Recurse -Force $StageDir
}
New-Item -ItemType Directory -Force -Path (Join-Path $PackageDir "bin/64bit") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $PackageDir "data") | Out-Null

Copy-Item (Join-Path $RootDir "packaging/windows/Install-Windows.bat") (Join-Path $StageDir "Install-Windows.bat")
Copy-Item (Join-Path $RootDir "packaging/windows/Uninstall-Windows.bat") (Join-Path $StageDir "Uninstall-Windows.bat")
Copy-Item (Join-Path $RootDir "packaging/windows/INSTALL.txt") (Join-Path $StageDir "How to Install.txt")
Copy-Item $PluginDll (Join-Path $PackageDir "bin/64bit/virtuedit-obs.dll")
Copy-Item -Recurse (Join-Path $RootDir "data/*") (Join-Path $PackageDir "data")
Copy-Item (Join-Path $RootDir "LICENSE") (Join-Path $PackageDir "LICENSE.txt")
Copy-Item (Join-Path $RootDir "THIRD_PARTY_LICENSES") (Join-Path $PackageDir "THIRD_PARTY_LICENSES.txt")
Copy-Item (Join-Path $RootDir "SOURCE_CODE.md") (Join-Path $PackageDir "SOURCE_CODE.txt")
New-Item -ItemType Directory -Force -Path (Join-Path $PackageDir "licenses") | Out-Null
Copy-Item (Join-Path $RootDir "licenses/LGPL-2.1.txt") (Join-Path $PackageDir "licenses/LGPL-2.1.txt")
Copy-Item (Join-Path $RootDir "licenses/LGPL-3.0.txt") (Join-Path $PackageDir "licenses/LGPL-3.0.txt")
Copy-Item (Join-Path $RootDir "packaging/windows/INSTALL.txt") (Join-Path $PackageDir "INSTALL.txt")

if ($FfmpegExe) {
    if (!(Test-Path $FfmpegExe)) {
        throw "ffmpeg.exe not found: $FfmpegExe"
    }
    Copy-Item $FfmpegExe (Join-Path $PackageDir "data/ffmpeg.exe")
} else {
    Write-Warning "No ffmpeg.exe was provided. Recording post-processing will fail unless system ffmpeg fallback is enabled."
}

if (Test-Path $ZipPath) {
    Remove-Item -Force $ZipPath
}
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $ZipPath) | Out-Null
Compress-Archive -Path (Join-Path $StageDir "*") -DestinationPath $ZipPath -Force

Write-Host "Packaged Windows ZIP: $ZipPath"
