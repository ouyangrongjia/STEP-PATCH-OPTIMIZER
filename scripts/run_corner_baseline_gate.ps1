param(
    [string]$Preset = "windows-msvc-debug",
    [string]$SourceStep = "",
    [string]$CandidateId = "auto",
    [string]$Patch = "",
    [string]$OutputDir = "",
    [string]$Report = "",
    [string]$WrapCore = "E:\Geomagic Wrap\wrapCore.exe",
    [int]$TimeoutSeconds = 1800,
    [switch]$RealGeomagic,
    [switch]$AllowQualityGateFailure
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

function Find-DefaultSourceStep {
    $roots = @(
        (Join-Path $repoRoot "data\stp"),
        (Join-Path $repoRoot "data\samples")
    )

    $candidates = @()
    foreach ($root in $roots) {
        if (-not (Test-Path $root)) {
            continue
        }

        $candidates += Get-ChildItem -LiteralPath $root -Recurse -File |
            Where-Object {
                $extension = $_.Extension.ToLowerInvariant()
                $extension -eq ".stp" -or $extension -eq ".step"
            }
    }

    $candidate = $candidates |
        Sort-Object FullName |
        Select-Object -First 1

    if ($null -eq $candidate) {
        return ""
    }

    return $candidate.FullName
}

function Find-CornerBaselineProbeExe {
    $candidates = @(
        (Join-Path $repoRoot "build\$Preset\Debug\corner_baseline_probe.exe"),
        (Join-Path $repoRoot "build\$Preset\corner_baseline_probe.exe")
    )

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    throw "corner_baseline_probe.exe was not found under build\$Preset after build."
}

function Find-ExistingPatchForCandidate {
    param(
        [Parameter(Mandatory = $true)][string]$SourceStepPath,
        [Parameter(Mandatory = $true)][int]$ResolvedCandidateId
    )

    $stem = [System.IO.Path]::GetFileNameWithoutExtension($SourceStepPath)
    $padded = "{0:D4}" -f $ResolvedCandidateId
    $baseName = "${stem}_candidate_${padded}"

    $roots = @(
        (Join-Path $repoRoot "data\crop_stp\$stem"),
        (Join-Path $repoRoot "data\crop_igs\$stem")
    )
    $extensions = @(".stp", ".step", ".igs", ".iges")

    foreach ($root in $roots) {
        foreach ($extension in $extensions) {
            $candidate = Join-Path $root "${baseName}${extension}"
            if (Test-Path $candidate) {
                return [System.IO.Path]::GetFullPath($candidate)
            }
        }
    }

    return ""
}

function Test-AllowableBaselineFailure {
    param([Parameter(Mandatory = $true)][string]$ReportPath)

    if (-not (Test-Path $ReportPath)) {
        return $false
    }

    $json = Get-Content -LiteralPath $ReportPath -Raw | ConvertFrom-Json
    if ($json.stage -ne "failed_gate") {
        return $false
    }
    if (-not $json.patch_apply.gate_passed) {
        return $false
    }
    if (-not $json.commercial_cad_like_quality_gate.evaluated) {
        return $false
    }
    if ($json.commercial_cad_like_quality_gate.passed) {
        return $false
    }

    return $true
}

$vcpkgRoot = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { Join-Path $env:USERPROFILE "vcpkg" }
$env:VCPKG_ROOT = $vcpkgRoot
$env:Path = "C:\Program Files\CMake\bin;$vcpkgRoot;" + $env:Path

if (-not $SourceStep) {
    $SourceStep = Find-DefaultSourceStep
    if (-not $SourceStep) {
        Write-Host "SKIPPED: no STEP/STP sample found under data\stp or data\samples."
        exit 0
    }
} else {
    $SourceStep = Convert-ToRepoAbsolutePath -PathValue $SourceStep
}

if (-not (Test-Path $SourceStep)) {
    throw "SourceStep does not exist: $SourceStep"
}

if (-not $OutputDir) {
    $OutputDir = Join-Path $repoRoot "data\baseline_runs\scripted_a0_gate"
} else {
    $OutputDir = Convert-ToRepoAbsolutePath -PathValue $OutputDir
}

if (-not $Report) {
    $Report = Join-Path $OutputDir "baseline_report.json"
} else {
    $Report = Convert-ToRepoAbsolutePath -PathValue $Report
}

if ($Patch) {
    $Patch = Convert-ToRepoAbsolutePath -PathValue $Patch
    if (-not (Test-Path $Patch)) {
        throw "Patch does not exist: $Patch"
    }
}

$candidateIdValue = 0
$candidateIdIsNumeric = [int]::TryParse($CandidateId, [ref]$candidateIdValue)
if (-not $Patch -and -not $RealGeomagic -and $candidateIdIsNumeric) {
    $Patch = Find-ExistingPatchForCandidate -SourceStepPath $SourceStep -ResolvedCandidateId $candidateIdValue
}

if (-not $Patch -and -not $RealGeomagic) {
    Write-Host "SKIPPED: no existing patch found. Pass -Patch, numeric -CandidateId with an existing staged patch, or -RealGeomagic."
    exit 0
}

if ($RealGeomagic -and -not $Patch -and -not (Test-Path $WrapCore)) {
    throw "wrapCore was not found: $WrapCore"
}

Invoke-Native -FilePath "cmake" -Arguments @("--build", "--preset", $Preset, "--target", "corner_baseline_probe")
$probeExe = Find-CornerBaselineProbeExe

$probeArgs = @(
    "--source-step", $SourceStep,
    "--candidate-id", $CandidateId,
    "--output-dir", $OutputDir,
    "--report", $Report
)

if ($Patch) {
    $probeArgs += @("--patch", $Patch)
} else {
    $probeArgs += @(
        "--wrap-core", $WrapCore,
        "--timeout-seconds", "$TimeoutSeconds"
    )
}

Write-Host ""
Write-Host "==> $probeExe $($probeArgs -join ' ')"
& $probeExe @probeArgs
$probeExitCode = $LASTEXITCODE

if ($probeExitCode -ne 0) {
    if ($AllowQualityGateFailure -and (Test-AllowableBaselineFailure -ReportPath $Report)) {
        Write-Warning "Baseline pipeline completed but CommercialCadLikeQualityGate failed; report preserved at $Report"
        exit 0
    }

    throw "$probeExe failed with exit code $probeExitCode"
}

Write-Host ""
Write-Host "corner baseline gate completed: $Report"
