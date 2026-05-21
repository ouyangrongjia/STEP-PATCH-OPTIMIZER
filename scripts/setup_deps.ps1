param(
    [string]$VcpkgRoot = $(if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { Join-Path $env:USERPROFILE "vcpkg" }),
    [string]$Triplet = "x64-windows"
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$env:VCPKG_ROOT = $VcpkgRoot
[Environment]::SetEnvironmentVariable("VCPKG_ROOT", $VcpkgRoot, "User")
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
