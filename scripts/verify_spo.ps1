param(
    [string]$Preset = "windows-msvc-debug",
    [switch]$Gui,
    [switch]$StepStats,
    [string]$StepStatsPath = "",
    [switch]$RealGeomagic
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    Write-Host ""
    Write-Host "==> $FilePath $($Arguments -join ' ')"
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath failed with exit code $LASTEXITCODE"
    }
}

function Convert-ToRepoAbsolutePath {
    param([Parameter(Mandatory = $true)][string]$PathValue)

    if ([System.IO.Path]::IsPathRooted($PathValue)) {
        return [System.IO.Path]::GetFullPath($PathValue)
    }

    return [System.IO.Path]::GetFullPath((Join-Path $repoRoot $PathValue))
}

function Find-StepStatsExe {
    $candidates = @(
        (Join-Path $repoRoot "build\$Preset\Debug\step_stats.exe"),
        (Join-Path $repoRoot "build\$Preset\step_stats.exe")
    )

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    throw "step_stats.exe was not found under build\$Preset after build."
}

$vcpkgRoot = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { Join-Path $env:USERPROFILE "vcpkg" }
$env:VCPKG_ROOT = $vcpkgRoot
$env:Path = "C:\Program Files\CMake\bin;$vcpkgRoot;" + $env:Path

Invoke-Native -FilePath "cmake" -Arguments @("--preset", $Preset)
Invoke-Native -FilePath "powershell" -Arguments @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", (Join-Path $repoRoot "scripts\test.ps1"),
    "-Preset", $Preset
)
Invoke-Native -FilePath "cmake" -Arguments @("--build", "--preset", $Preset, "--target", "patch_apply_probe")
Invoke-Native -FilePath "cmake" -Arguments @("--build", "--preset", $Preset, "--target", "corner_baseline_probe")

if ($RealGeomagic) {
    Invoke-Native -FilePath "powershell" -Arguments @(
        "-ExecutionPolicy", "Bypass",
        "-File", (Join-Path $repoRoot "scripts\run_corner_baseline_gate.ps1"),
        "-Preset", $Preset,
        "-RealGeomagic",
        "-AllowQualityGateFailure"
    )
}

if ($Gui) {
    Invoke-Native -FilePath "cmake" -Arguments @("--build", "--preset", $Preset, "--target", "step-patch-optimizer")
}

if ($StepStats -or $StepStatsPath) {
    if (-not $StepStatsPath) {
        throw "-StepStats requires -StepStatsPath."
    }

    $stepStatsInput = Convert-ToRepoAbsolutePath -PathValue $StepStatsPath
    if (-not (Test-Path $stepStatsInput)) {
        throw "StepStatsPath does not exist: $stepStatsInput"
    }

    Invoke-Native -FilePath "cmake" -Arguments @("--build", "--preset", $Preset, "--target", "step_stats")
    $stepStatsExe = Find-StepStatsExe
    Invoke-Native -FilePath $stepStatsExe -Arguments @($stepStatsInput)
}

Invoke-Native -FilePath "git" -Arguments @("diff", "--check")

Write-Host ""
Write-Host "verify_spo completed."
