param(
    [string]$Preset = "windows-msvc-debug",
    [switch]$Gui,
    [switch]$StepStats,
    [string]$StepStatsPath = "",
    [switch]$RealGeomagic,
    [switch]$SkipDocsSyncCheck
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

function Get-ChangedFiles {
    $files = @()
    $files += git diff --name-only
    $files += git diff --cached --name-only
    $files += git ls-files --others --exclude-standard
    return $files | Where-Object { $_ } | Sort-Object -Unique
}

function Test-DocsSync {
    param([Parameter(Mandatory = $true)][string[]]$ChangedFiles)

    if ($SkipDocsSyncCheck -or $ChangedFiles.Count -eq 0) {
        return
    }

    $requiredDocs = @(
        "docs/implementation_status.md",
        "docs/TODO.md",
        "docs/geomagic_patch_workflow.md"
    )

    $docChanged = $false
    foreach ($file in $ChangedFiles) {
        $normalized = $file.Replace("\", "/")
        if ($requiredDocs -contains $normalized) {
            $docChanged = $true
            break
        }
    }

    $watchedPrefixes = @(
        "src/app/",
        "src/gui/",
        "src/patch/",
        "src/external/geomagic/",
        "scripts/geomagic_wrap/"
    )
    $watchedExact = @(
        "CMakeLists.txt",
        "scripts/run_geomagic_patch.ps1",
        "scripts/verify_spo.ps1"
    )

    $needsDocs = $false
    foreach ($file in $ChangedFiles) {
        $normalized = $file.Replace("\", "/")

        if ($watchedExact -contains $normalized) {
            $needsDocs = $true
            break
        }

        foreach ($prefix in $watchedPrefixes) {
            if ($normalized.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
                $needsDocs = $true
                break
            }
        }

        if ($needsDocs) {
            break
        }

        if ($normalized.StartsWith("tests/test_geomagic", [System.StringComparison]::OrdinalIgnoreCase) -or
            $normalized.StartsWith("tests/test_patch", [System.StringComparison]::OrdinalIgnoreCase)) {
            $needsDocs = $true
            break
        }
    }

    if ($needsDocs -and -not $docChanged) {
        throw "Docs sync check failed: Geomagic/Patch code or repo scripts changed, but docs/implementation_status.md, docs/TODO.md, or docs/geomagic_patch_workflow.md was not updated. Update docs or rerun with -SkipDocsSyncCheck."
    }
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

$changedFiles = @(Get-ChangedFiles)
Test-DocsSync -ChangedFiles $changedFiles

$vcpkgRoot = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { Join-Path $env:USERPROFILE "vcpkg" }
$env:VCPKG_ROOT = $vcpkgRoot
$env:Path = "C:\Program Files\CMake\bin;$vcpkgRoot;" + $env:Path

Invoke-Native -FilePath "cmake" -Arguments @("--preset", $Preset)
Invoke-Native -FilePath "cmake" -Arguments @("--build", "--preset", $Preset, "--target", "spo_tests")
Invoke-Native -FilePath "cmake" -Arguments @("--build", "--preset", $Preset, "--target", "patch_apply_probe")
Invoke-Native -FilePath "cmake" -Arguments @("--build", "--preset", $Preset, "--target", "corner_baseline_probe")

$oldRealGeomagic = $env:SPO_ENABLE_REAL_GEOMAGIC_TESTS
try {
    if ($RealGeomagic) {
        $env:SPO_ENABLE_REAL_GEOMAGIC_TESTS = "1"
    } else {
        $env:SPO_ENABLE_REAL_GEOMAGIC_TESTS = "0"
    }

    Invoke-Native -FilePath "ctest" -Arguments @("--preset", $Preset, "-R", "spo_tests", "--output-on-failure")
} finally {
    if ($null -eq $oldRealGeomagic) {
        Remove-Item Env:SPO_ENABLE_REAL_GEOMAGIC_TESTS -ErrorAction SilentlyContinue
    } else {
        $env:SPO_ENABLE_REAL_GEOMAGIC_TESTS = $oldRealGeomagic
    }
}

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
