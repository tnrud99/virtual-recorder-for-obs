param(
    [Parameter(Mandatory = $true)]
    [string]$DllPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputLib,

    [Parameter(Mandatory = $true)]
    [string]$LibraryName
)

$ErrorActionPreference = "Stop"

if (!(Test-Path $DllPath)) {
    throw "DLL not found: $DllPath"
}

$outputDir = Split-Path -Parent $OutputLib
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

$defPath = [System.IO.Path]::ChangeExtension($OutputLib, ".def")
$exports = & dumpbin /nologo /exports $DllPath
if ($LASTEXITCODE -ne 0) {
    throw "dumpbin failed for $DllPath"
}

$names = @()
foreach ($line in $exports) {
    if ($line -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)') {
        $name = $Matches[1]
        if ($name -and $name -ne "[NONAME]" -and !$name.StartsWith("Summary")) {
            $names += $name
        }
    }
}

if ($names.Count -eq 0) {
    throw "No exported symbols found in $DllPath"
}

@(
    "LIBRARY $LibraryName"
    "EXPORTS"
) + ($names | Sort-Object -Unique | ForEach-Object { "    $_" }) |
    Set-Content -Encoding ASCII -Path $defPath

& lib /nologo /machine:x64 /def:$defPath /out:$OutputLib
if ($LASTEXITCODE -ne 0) {
    throw "lib.exe failed for $DllPath"
}

Write-Host "Created import library: $OutputLib"
