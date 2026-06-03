param(
    [Parameter(Mandatory = $true)][string]$InputStl,
    [string]$OutputStep = "",
    [string]$WrapCorePath = "E:\Geomagic Wrap\wrapCore.exe",
    [string]$ScriptPath = "",
    [string]$Preset = "windows-msvc-debug",
    [switch]$StrictPatchTarget,
    [switch]$NoStepStats
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

function Convert-ToRepoAbsolutePath {
    param([Parameter(Mandatory = $true)][string]$PathValue)

    if ([System.IO.Path]::IsPathRooted($PathValue)) {
        return [System.IO.Path]::GetFullPath($PathValue)
    }

    return [System.IO.Path]::GetFullPath((Join-Path $repoRoot $PathValue))
}

function Get-DefaultOutputStep {
    param([Parameter(Mandatory = $true)][string]$InputStlPath)

    $cropStlRoot = [System.IO.Path]::GetFullPath((Join-Path $repoRoot "data\crop_stl"))
    $cropStpRoot = [System.IO.Path]::GetFullPath((Join-Path $repoRoot "data\crop_stp"))

    if ($InputStlPath.StartsWith($cropStlRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        $relative = $InputStlPath.Substring($cropStlRoot.Length) -replace '^[\\/]+', ''
        $relativeStep = [System.IO.Path]::ChangeExtension($relative, ".stp")
        return [System.IO.Path]::GetFullPath((Join-Path $cropStpRoot $relativeStep))
    }

    return [System.IO.Path]::ChangeExtension($InputStlPath, ".stp")
}

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

$inputStlPath = Convert-ToRepoAbsolutePath -PathValue $InputStl
if (-not (Test-Path $inputStlPath)) {
    throw "Input STL does not exist: $inputStlPath"
}

if (-not $ScriptPath) {
    $ScriptPath = Join-Path $repoRoot "scripts\geomagic_wrap\autosurface_pipeline.py"
}

$scriptAbsPath = Convert-ToRepoAbsolutePath -PathValue $ScriptPath
if (-not (Test-Path $scriptAbsPath)) {
    throw "Geomagic script does not exist: $scriptAbsPath"
}

$wrapCoreAbsPath = Convert-ToRepoAbsolutePath -PathValue $WrapCorePath
if (-not (Test-Path $wrapCoreAbsPath)) {
    throw "wrapCore.exe does not exist: $wrapCoreAbsPath"
}

$outputStepPath = if ($OutputStep) {
    Convert-ToRepoAbsolutePath -PathValue $OutputStep
} else {
    Get-DefaultOutputStep -InputStlPath $inputStlPath
}

if (-not [System.IO.Path]::GetExtension($outputStepPath)) {
    $outputStepPath = "$outputStepPath.stp"
}

$outputDir = [System.IO.Path]::GetDirectoryName($outputStepPath)
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

$outputStem = [System.IO.Path]::GetFileNameWithoutExtension($outputStepPath)
$fitRegionLog = Join-Path $outputDir "$($outputStem)_fit_region.log"
$igesSidecar = Join-Path $outputDir "$($outputStem)_autosurface.igs"
$strictPatchTargetValue = if ($StrictPatchTarget) { "1" } else { "0" }

Write-Host "FIT_REGION_INPUT=$inputStlPath"
Write-Host "FIT_REGION_OUTPUT=$outputStepPath"
Write-Host "FIT_REGION_STRICT_PATCH_TARGET=$strictPatchTargetValue"
Write-Host "FIT_REGION_LOG_FILE=$fitRegionLog"
Write-Host "IGES sidecar=$igesSidecar"
Write-Host "script=$scriptAbsPath"
Write-Host "wrapCore=$wrapCoreAbsPath"

$oldInput = $env:FIT_REGION_INPUT
$oldOutput = $env:FIT_REGION_OUTPUT
$oldStrict = $env:FIT_REGION_STRICT_PATCH_TARGET
$oldLogFile = $env:FIT_REGION_LOG_FILE

try {
    $env:FIT_REGION_INPUT = $inputStlPath
    $env:FIT_REGION_OUTPUT = $outputStepPath
    $env:FIT_REGION_STRICT_PATCH_TARGET = $strictPatchTargetValue
    $env:FIT_REGION_LOG_FILE = $fitRegionLog

    Write-Host ""
    Write-Host "==> $wrapCoreAbsPath --script $scriptAbsPath"
    & $wrapCoreAbsPath --script $scriptAbsPath
    $wrapCoreExitCode = $LASTEXITCODE
} finally {
    if ($null -eq $oldInput) {
        Remove-Item Env:FIT_REGION_INPUT -ErrorAction SilentlyContinue
    } else {
        $env:FIT_REGION_INPUT = $oldInput
    }

    if ($null -eq $oldOutput) {
        Remove-Item Env:FIT_REGION_OUTPUT -ErrorAction SilentlyContinue
    } else {
        $env:FIT_REGION_OUTPUT = $oldOutput
    }

    if ($null -eq $oldStrict) {
        Remove-Item Env:FIT_REGION_STRICT_PATCH_TARGET -ErrorAction SilentlyContinue
    } else {
        $env:FIT_REGION_STRICT_PATCH_TARGET = $oldStrict
    }

    if ($null -eq $oldLogFile) {
        Remove-Item Env:FIT_REGION_LOG_FILE -ErrorAction SilentlyContinue
    } else {
        $env:FIT_REGION_LOG_FILE = $oldLogFile
    }
}

Write-Host ""
Write-Host "wrapCore exit code: $wrapCoreExitCode"

if (-not (Test-Path $outputStepPath)) {
    Write-Host "STEP output was not created: $outputStepPath"
    if (Test-Path $fitRegionLog) {
        Write-Host ""
        Write-Host "Last fit_region log lines:"
        Get-Content -Path $fitRegionLog -Tail 120
    }
    exit 1
}

Write-Host "STEP output: $outputStepPath"
if (Test-Path $fitRegionLog) {
    Write-Host "fit_region log: $fitRegionLog"
}
if (Test-Path $igesSidecar) {
    Write-Host "IGES sidecar: $igesSidecar"
}

if (-not $NoStepStats) {
    $vcpkgRoot = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { Join-Path $env:USERPROFILE "vcpkg" }
    $env:VCPKG_ROOT = $vcpkgRoot
    $env:Path = "C:\Program Files\CMake\bin;$vcpkgRoot;" + $env:Path

    Invoke-Native -FilePath "cmake" -Arguments @("--build", "--preset", $Preset, "--target", "step_stats")
    $stepStatsExe = Find-StepStatsExe
    Invoke-Native -FilePath $stepStatsExe -Arguments @($outputStepPath)
}

if ($wrapCoreExitCode -ne 0) {
    Write-Host "wrapCore returned a non-zero exit code, but STEP output exists. Treating this as success, matching backend behavior."
}
