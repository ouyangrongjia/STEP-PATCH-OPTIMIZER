param(
    [string]$VcpkgRoot = "C:\Users\27836\vcpkg",
    [string]$Triplet = "x64-windows"
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$vcpkgExe = Join-Path $VcpkgRoot "vcpkg.exe"
$bootstrap = Join-Path $VcpkgRoot "bootstrap-vcpkg.bat"

if (-not (Test-Path $VcpkgRoot)) {
    throw "vcpkg directory not found: $VcpkgRoot"
}

if (-not (Test-Path $vcpkgExe)) {
    if (-not (Test-Path $bootstrap)) {
        throw "bootstrap-vcpkg.bat not found: $bootstrap"
    }
    & $bootstrap
}

& $vcpkgExe install --triplet $Triplet --x-manifest-root=$repoRoot
