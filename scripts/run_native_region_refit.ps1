[CmdletBinding(DefaultParameterSetName = "Candidate")]
param(
    [Parameter(Mandatory = $true)][string]$SourceStep,
    [Parameter(Mandatory = $true, ParameterSetName = "Candidate")][int]$CandidateId,
    [Parameter(Mandatory = $true, ParameterSetName = "Faces")][long[]]$FaceIds,
    [ValidateSet("adaptive", "fixed")][string]$Layout = "adaptive",
    [int]$PatchCount = 0,
    [long[]]$LockedEdgeIds = @(),
    [long]$WallTimeMilliseconds = 900000,
    [long]$MaximumMemoryMiB = 8192,
    [string]$OutputDir = "",
    [switch]$ConstraintsOnly,
    [switch]$SamplingOnly,
    [switch]$LayoutOnly
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "run_artifacts.ps1")

$sourcePath = [System.IO.Path]::GetFullPath($SourceStep)
if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
    throw "STEP source does not exist: $sourcePath"
}
if ($Layout -eq "fixed" -and $PatchCount -le 0) {
    throw "Fixed layout requires -PatchCount greater than zero."
}
if ([int]$ConstraintsOnly.IsPresent + [int]$SamplingOnly.IsPresent + [int]$LayoutOnly.IsPresent -gt 1) {
    throw "Stage diagnostic switches cannot be used together."
}

$candidateToken = if ($PSCmdlet.ParameterSetName -eq "Candidate") { [string]$CandidateId } else { "faces" }
$variant = if ($ConstraintsOnly) { "constraints" } elseif ($SamplingOnly) { "sampling" } elseif ($LayoutOnly) { "layout" } else { $Layout }
$context = New-SpoRunContext `
    -RepoRoot $repoRoot `
    -Workflow "native-region-refit" `
    -SourcePath $sourcePath `
    -CandidateId $candidateToken `
    -Variant $variant `
    -OutputDir $OutputDir `
    -ScriptPath $PSCommandPath `
    -Parameters $PSBoundParameters

$probe = Join-Path $repoRoot "build\windows-msvc-debug\Debug\native_region_refit_probe.exe"
if (-not (Test-Path -LiteralPath $probe -PathType Leaf)) {
    throw "Probe is not built: $probe"
}

$arguments = @($sourcePath, $context.output_dir)
if ($PSCmdlet.ParameterSetName -eq "Candidate") {
    $arguments += @("--candidate-id", [string]$CandidateId)
} else {
    $arguments += @("--faces", ($FaceIds -join ","))
}
$arguments += @("--layout", $Layout)
if ($PatchCount -gt 0) {
    $arguments += @("--patch-count", [string]$PatchCount)
}
if ($LockedEdgeIds.Count -gt 0) {
    $arguments += @("--locked-edges", ($LockedEdgeIds -join ","))
}
$arguments += @(
    "--wall-time-ms", [string]$WallTimeMilliseconds,
    "--max-memory-mib", [string]$MaximumMemoryMiB)
if ($ConstraintsOnly) {
    $arguments += "--constraints-only"
} elseif ($SamplingOnly) {
    $arguments += "--sampling-only"
} elseif ($LayoutOnly) {
    $arguments += "--layout-only"
}

$stdout = Join-Path $context.directories.logs "probe.stdout.log"
$stderr = Join-Path $context.directories.logs "probe.stderr.log"
$process = Start-Process `
    -FilePath $probe `
    -ArgumentList $arguments `
    -WorkingDirectory $repoRoot `
    -NoNewWindow `
    -Wait `
    -PassThru `
    -RedirectStandardOutput $stdout `
    -RedirectStandardError $stderr

$status = if ($process.ExitCode -eq 0) { "succeeded" } else { "failed" }
$message = if ($process.ExitCode -eq 0) {
    "Native regional refit probe completed."
} else {
    "Native regional refit probe failed with exit code $($process.ExitCode)."
}
$artifacts = [ordered]@{}
$samplingStl = Join-Path $context.directories.artifacts "sampling\adaptive_reference.stl"
if (Test-Path -LiteralPath $samplingStl -PathType Leaf) {
    $artifacts["adaptive_sampling_stl"] = $samplingStl
}
$layoutJson = Join-Path $context.directories.artifacts "layout\selected_layout.json"
$layoutPly = Join-Path $context.directories.artifacts "layout\selected_layout.ply"
$layoutSvg = Join-Path $context.directories.artifacts "layout\selected_layout.svg"
if (Test-Path -LiteralPath $layoutJson -PathType Leaf) {
    $artifacts["selected_layout_json"] = $layoutJson
}
if (Test-Path -LiteralPath $layoutPly -PathType Leaf) {
    $artifacts["selected_layout_ply"] = $layoutPly
}
if (Test-Path -LiteralPath $layoutSvg -PathType Leaf) {
    $artifacts["selected_layout_svg"] = $layoutSvg
}
Complete-SpoRun -Context $context -Status $status -Message $message -Artifacts $artifacts
Write-Output $context.output_dir
exit $process.ExitCode
