param(
    [string]$Preset = "windows-msvc-debug"
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

$env:Path = "C:\Program Files\CMake\bin;C:\Users\27836\vcpkg;" + $env:Path
cmake --build --preset $Preset --target spo_tests
ctest --preset $Preset --output-on-failure --timeout 30
