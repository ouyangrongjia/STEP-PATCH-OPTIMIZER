param(
    [string]$Preset = "windows-msvc-debug"
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repoRoot "build\windows-msvc-debug\Debug\step-patch-optimizer.exe"

if (-not (Test-Path $exe)) {
    & (Join-Path $PSScriptRoot "build_debug.ps1") -Preset $Preset
}

& $exe
