param(
    [string]$PluginDir = ""
)

$ErrorActionPreference = "Stop"

function Fail($Message) {
    Write-Error $Message
    exit 1
}

if (!$PluginDir) {
    $PluginDir = Join-Path $env:ProgramData "obs-studio\plugins\virtuedit-obs"
}

$PluginDll = Join-Path $PluginDir "bin\64bit\virtuedit-obs.dll"
$DataDir = Join-Path $PluginDir "data"
$FfmpegExe = Join-Path $DataDir "ffmpeg.exe"
$EnglishLocale = Join-Path $DataDir "locale\en-US.ini"
$KoreanLocale = Join-Path $DataDir "locale\ko-KR.ini"
$License = Join-Path $PluginDir "LICENSE.txt"
$ThirdPartyLicenses = Join-Path $PluginDir "THIRD_PARTY_LICENSES.txt"
$SourceCodeInfo = Join-Path $PluginDir "SOURCE_CODE.txt"
$Lgpl21 = Join-Path $PluginDir "licenses\LGPL-2.1.txt"
$Lgpl30 = Join-Path $PluginDir "licenses\LGPL-3.0.txt"

Write-Host "==> Checking Windows plugin install"
Write-Host "Plugin folder:"
Write-Host "  $PluginDir"

if (!(Test-Path -LiteralPath $PluginDir -PathType Container)) {
    Fail "plugin folder not found"
}
if (!(Test-Path -LiteralPath $PluginDll -PathType Leaf)) {
    Fail "plugin DLL missing: $PluginDll"
}
if (!(Test-Path -LiteralPath $DataDir -PathType Container)) {
    Fail "data folder missing: $DataDir"
}
if (!(Test-Path -LiteralPath $EnglishLocale -PathType Leaf)) {
    Fail "en-US locale missing: $EnglishLocale"
}
if (!(Test-Path -LiteralPath $KoreanLocale -PathType Leaf)) {
    Fail "ko-KR locale missing: $KoreanLocale"
}
if (!(Test-Path -LiteralPath $License -PathType Leaf)) {
    Fail "GPL license missing: $License"
}
if (!(Test-Path -LiteralPath $ThirdPartyLicenses -PathType Leaf)) {
    Fail "third-party notices missing: $ThirdPartyLicenses"
}
if (!(Test-Path -LiteralPath $SourceCodeInfo -PathType Leaf)) {
    Fail "source information missing: $SourceCodeInfo"
}
if (!(Test-Path -LiteralPath $Lgpl21 -PathType Leaf)) {
    Fail "LGPL-2.1 license missing: $Lgpl21"
}
if (!(Test-Path -LiteralPath $Lgpl30 -PathType Leaf)) {
    Fail "LGPL-3.0 license missing: $Lgpl30"
}
if (!(Test-Path -LiteralPath $FfmpegExe -PathType Leaf)) {
    Fail "bundled ffmpeg.exe missing: $FfmpegExe"
}

$FfmpegVersion = & $FfmpegExe -version 2>$null
if ($LASTEXITCODE -ne 0) {
    Fail "bundled ffmpeg.exe could not run"
}
if ($FfmpegVersion -match "--enable-gpl") {
    Fail "bundled ffmpeg.exe was built with --enable-gpl"
}

$DllInfo = Get-Item -LiteralPath $PluginDll
$FfmpegInfo = Get-Item -LiteralPath $FfmpegExe
if ($DllInfo.Length -le 0) {
    Fail "plugin DLL is zero bytes"
}
if ($FfmpegInfo.Length -le 0) {
    Fail "ffmpeg.exe is zero bytes"
}

Write-Host "Plugin DLL:"
Write-Host "  $PluginDll"
Write-Host "Bundled ffmpeg:"
Write-Host "  $FfmpegExe"
Write-Host "Windows plugin install verification passed."
