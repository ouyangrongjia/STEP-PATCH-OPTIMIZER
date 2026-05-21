param(
    [string]$VcpkgRoot = $(if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { Join-Path $env:USERPROFILE "vcpkg" }),
    [string]$Triplet = "x64-windows",
    [switch]$SkipSystemInstall,
    [switch]$SkipBuild,
    [switch]$RunGui
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

function Test-Command {
    param([string]$Name)
    return $null -ne (Get-Command $Name -ErrorAction SilentlyContinue)
}

function Update-CurrentPath {
    $machinePath = [Environment]::GetEnvironmentVariable("Path", "Machine")
    $userPath = [Environment]::GetEnvironmentVariable("Path", "User")
    $env:Path = "$machinePath;$userPath;$env:Path"
}

function Install-WingetPackage {
    param(
        [string]$Id,
        [string]$Name,
        [string[]]$ExtraArgs = @()
    )

    if (-not (Test-Command "winget")) {
        throw "winget is not available. Install Git, CMake, and Visual Studio Build Tools manually, then rerun this script."
    }

    Write-Host "Installing $Name with winget..."
    $args = @(
        "install", "--id", $Id, "-e",
        "--accept-package-agreements",
        "--accept-source-agreements"
    ) + $ExtraArgs
    & winget @args
    Update-CurrentPath
}

function Test-MsvcBuildTools {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        return $false
    }

    $installPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    return -not [string]::IsNullOrWhiteSpace($installPath)
}

if (-not $SkipSystemInstall) {
    if (-not (Test-Command "git")) {
        Install-WingetPackage -Id "Git.Git" -Name "Git"
    }

    if (-not (Test-Command "cmake")) {
        Install-WingetPackage -Id "Kitware.CMake" -Name "CMake"
    }

    if (-not (Test-MsvcBuildTools)) {
        Install-WingetPackage `
            -Id "Microsoft.VisualStudio.2022.BuildTools" `
            -Name "Visual Studio 2022 Build Tools" `
            -ExtraArgs @(
                "--override",
                "--wait --quiet --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --norestart"
            )
    }
}

if (-not (Test-Path $VcpkgRoot)) {
    if (-not (Test-Command "git")) {
        throw "Git is required to clone vcpkg."
    }

    Write-Host "Cloning vcpkg to $VcpkgRoot..."
    git clone https://github.com/microsoft/vcpkg.git $VcpkgRoot
}

$bootstrap = Join-Path $VcpkgRoot "bootstrap-vcpkg.bat"
if (-not (Test-Path $bootstrap)) {
    throw "Invalid vcpkg directory: $VcpkgRoot"
}

$env:VCPKG_ROOT = $VcpkgRoot
[Environment]::SetEnvironmentVariable("VCPKG_ROOT", $VcpkgRoot, "User")
$env:Path = "C:\Program Files\CMake\bin;$VcpkgRoot;$env:Path"

& (Join-Path $PSScriptRoot "setup_deps.ps1") -VcpkgRoot $VcpkgRoot -Triplet $Triplet

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot "build_debug.ps1")
    & (Join-Path $PSScriptRoot "test.ps1")
}

if ($RunGui) {
    & (Join-Path $PSScriptRoot "run_gui.ps1")
}
