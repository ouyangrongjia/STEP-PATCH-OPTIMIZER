[CmdletBinding(DefaultParameterSetName = "Candidate")]
param(
    [Parameter(Mandatory = $true)][string]$SourceStep,
    [Parameter(Mandatory = $true, ParameterSetName = "Candidate")][int]$CandidateId,
    [Parameter(Mandatory = $true, ParameterSetName = "Faces")][long[]]$FaceIds,
    [ValidateSet("legacy", "target", "adaptive")][string]$PatchMode = "legacy",
    [int]$RequestedPatchCount = 0,
    [int]$PatchCountSearchRadius = 0,
    [ValidateSet("minimum-qualified")][string]$AdaptiveObjective = "minimum-qualified",
    [int]$AdaptiveSafetyCap = 12,
    [ValidateSet("adaptive", "fixed")][string]$Layout = "adaptive",
    [int]$PatchCount = 0,
    [ValidateSet("adaptive-occt", "quality-balanced", "uv-uniform")][string]$SamplingMode = "adaptive-occt",
    [long]$TargetSampleTriangles = 80000,
    [ValidateSet("adaptive-occt", "quality-balanced", "uv-uniform")][string]$LayoutCarrierSamplingMode = "quality-balanced",
    [long]$TargetLayoutCarrierTriangles = 80000,
    [int]$MaximumFullFitCount = 3,
    [switch]$BestEffortThroughSewing,
    [switch]$ForceDiagnosticSewing,
    [long[]]$LockedEdgeIds = @(),
    [long]$WallTimeMilliseconds = 900000,
    [long]$InputReadBudgetMilliseconds = 60000,
    [long]$MaximumMemoryMiB = 8192,
    [ValidateSet("disabled", "read-write", "read-only", "refresh")][string]$CachePolicy = "disabled",
    [string]$CacheDir = "",
    [string]$OutputDir = "",
    [switch]$PerformanceAcceptance,
    [switch]$ConstraintsOnly,
    [switch]$SamplingOnly,
    [switch]$LayoutOnly,
    [switch]$FitOnly
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "run_artifacts.ps1")

$sourcePath = [System.IO.Path]::GetFullPath($SourceStep)
if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
    throw "STEP source does not exist: $sourcePath"
}
if ($PatchMode -eq "legacy" -and $Layout -eq "fixed" -and $PatchCount -le 0) {
    throw "Fixed layout requires -PatchCount greater than zero."
}
if ($PatchMode -eq "target" -and ($RequestedPatchCount -le 0 -or $PatchCountSearchRadius -lt 0)) {
    throw "Target patch mode requires positive -RequestedPatchCount and non-negative -PatchCountSearchRadius."
}
if ($PatchMode -eq "adaptive" -and $AdaptiveSafetyCap -le 0) {
    throw "Adaptive patch mode requires positive -AdaptiveSafetyCap."
}
if ($TargetSampleTriangles -le 0 -or $MaximumFullFitCount -le 0) {
    throw "Sampling target and maximum full fit count must be positive."
}
if ([int]$ConstraintsOnly.IsPresent + [int]$SamplingOnly.IsPresent +
        [int]$LayoutOnly.IsPresent + [int]$FitOnly.IsPresent -gt 1) {
    throw "Stage diagnostic switches cannot be used together."
}

$candidateToken = if ($PSCmdlet.ParameterSetName -eq "Candidate") { [string]$CandidateId } else { "faces" }
$variant = if ($ConstraintsOnly) { "constraints" } elseif ($SamplingOnly) { "sampling" } elseif ($LayoutOnly) { "layout" } elseif ($FitOnly) { "fit" } elseif ($PatchMode -ne "legacy") { $PatchMode } else { $Layout }
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

$baseArguments = @($sourcePath, $context.output_dir)
if ($PSCmdlet.ParameterSetName -eq "Candidate") {
    $baseArguments += @("--candidate-id", [string]$CandidateId)
} else {
    $baseArguments += @("--faces", ($FaceIds -join ","))
}
$baseArguments += if ($PatchMode -eq "legacy") {
    @("--layout", $Layout)
} else {
    @("--patch-mode", $PatchMode)
}
if ($PatchMode -eq "target") {
    $baseArguments += @(
        "--requested-patch-count", [string]$RequestedPatchCount,
        "--patch-count-search-radius", [string]$PatchCountSearchRadius)
} elseif ($PatchMode -eq "adaptive") {
    $baseArguments += @(
        "--adaptive-objective", $AdaptiveObjective,
        "--adaptive-safety-cap", [string]$AdaptiveSafetyCap)
} elseif ($PatchCount -gt 0) {
    $baseArguments += @("--patch-count", [string]$PatchCount)
}
if ($LockedEdgeIds.Count -gt 0) {
    $baseArguments += @("--locked-edges", ($LockedEdgeIds -join ","))
}
$baseArguments += @(
    "--sampling-mode", $SamplingMode,
    "--target-sample-triangles", [string]$TargetSampleTriangles,
    "--layout-carrier-sampling-mode", $LayoutCarrierSamplingMode,
    "--target-layout-carrier-triangles", [string]$TargetLayoutCarrierTriangles,
    "--maximum-full-fit-count", [string]$MaximumFullFitCount,
    "--wall-time-ms", [string]$WallTimeMilliseconds,
    "--input-read-budget-ms", [string]$InputReadBudgetMilliseconds,
    "--max-memory-mib", [string]$MaximumMemoryMiB)
if ($BestEffortThroughSewing) {
    $baseArguments += "--best-effort-through-sewing"
}
if ($ForceDiagnosticSewing) {
    $baseArguments += "--force-diagnostic-sewing"
}
if ($ConstraintsOnly) {
    $baseArguments += "--constraints-only"
} elseif ($SamplingOnly) {
    $baseArguments += "--sampling-only"
} elseif ($LayoutOnly) {
    $baseArguments += "--layout-only"
} elseif ($FitOnly) {
    $baseArguments += "--fit-only"
}

if ($PerformanceAcceptance) {
    if ($CachePolicy -notin @("disabled", "read-write")) {
        throw "Performance acceptance requires a writable cold/warm cache."
    }
    $CachePolicy = "read-write"
    if (-not $CacheDir) {
        $CacheDir = Join-Path $repoRoot "data\cache\native-region-refit\$($context.run_id)"
    }
    $CacheDir = [System.IO.Path]::GetFullPath($CacheDir)
    if (Test-Path -LiteralPath $CacheDir) {
        if (@(Get-ChildItem -LiteralPath $CacheDir -Force).Count -gt 0) {
            throw "Cold performance cache directory must be uniquely empty: $CacheDir"
        }
    } else {
        New-Item -ItemType Directory -Path $CacheDir -Force | Out-Null
    }
} elseif ($CachePolicy -ne "disabled") {
    if (-not $CacheDir) {
        throw "Non-disabled cache policy requires -CacheDir."
    }
    $CacheDir = [System.IO.Path]::GetFullPath($CacheDir)
}

if ($CachePolicy -ne "disabled") {
    $baseArguments += @("--cache-dir", $CacheDir, "--cache-policy", $CachePolicy)
}
$baseArguments += @("--commit", [string]$context.git.commit)

function Invoke-NativeRegionRefitProbe {
    param(
        [Parameter(Mandatory = $true)][string]$Identity,
        [Parameter(Mandatory = $true)][string]$LogDirectory,
        [switch]$PerformanceOnly
    )

    New-Item -ItemType Directory -Path $LogDirectory -Force | Out-Null
    $stdout = Join-Path $LogDirectory "probe.stdout.log"
    $stderr = Join-Path $LogDirectory "probe.stderr.log"
    $arguments = @($baseArguments) + @("--run-identity", $Identity)
    $runWallBudgetMilliseconds = $WallTimeMilliseconds
    if ($PerformanceOnly) {
        $arguments += "--performance-only"
        if ($PerformanceAcceptance) {
            $runWallBudgetMilliseconds = [Math]::Min($runWallBudgetMilliseconds, 300000L)
            $arguments += @("--wall-time-ms", [string]$runWallBudgetMilliseconds)
        }
    }
    $started = Get-Date
    $process = Start-Process `
        -FilePath $probe `
        -ArgumentList $arguments `
        -WorkingDirectory $repoRoot `
        -NoNewWindow `
        -PassThru `
        -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr
    $timeoutMilliseconds = [Math]::Min(
        [int]::MaxValue,
        [long]$runWallBudgetMilliseconds + 30000L)
    $completed = $process.WaitForExit([int]$timeoutMilliseconds)
    if (-not $completed) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        $exitCode = 124
        Add-Content -LiteralPath $stderr -Encoding UTF8 -Value "Probe process exceeded the hard script timeout."
    } else {
        $process.WaitForExit()
        $process.Refresh()
        $exitCode = $process.ExitCode
    }
    return [pscustomobject]@{
        identity = $Identity
        exit_code = $exitCode
        wall_ms = [long](((Get-Date) - $started).TotalMilliseconds)
        stdout = $stdout
        stderr = $stderr
    }
}

$runs = @()
if ($PerformanceAcceptance) {
    $runs += Invoke-NativeRegionRefitProbe `
        -Identity "cold" `
        -LogDirectory (Join-Path $context.directories.logs "cold")
    $runs += Invoke-NativeRegionRefitProbe `
        -Identity "warm-1" `
        -LogDirectory (Join-Path $context.directories.logs "warm\1") `
        -PerformanceOnly
    $runs += Invoke-NativeRegionRefitProbe `
        -Identity "warm-2" `
        -LogDirectory (Join-Path $context.directories.logs "warm\2") `
        -PerformanceOnly

    $performanceRuns = @()
    foreach ($run in $runs) {
        $path = Join-Path $context.directories.diagnostics "performance_$($run.identity).json"
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            $performanceRun = Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json
            $performanceRun | Add-Member -NotePropertyName process_wall_ms -NotePropertyValue $run.wall_ms
            $performanceRun | Add-Member -NotePropertyName process_exit_code -NotePropertyValue $run.exit_code
            $performanceRuns += $performanceRun
        }
    }
    $cold = @($performanceRuns | Where-Object run_identity -eq "cold") | Select-Object -First 1
    $warm = @($performanceRuns | Where-Object { $_.run_identity -like "warm-*" })
    $signatures = @($performanceRuns | ForEach-Object {
        "$($_.status)|$($_.selected_layout_hash)|$($_.selected_patch_count)|$($_.geometry_metrics | ConvertTo-Json -Compress)"
    } | Select-Object -Unique)
    $consistencyPassed = $performanceRuns.Count -eq 3 -and $signatures.Count -eq 1
    $coldBudgetPassed = $null -ne $cold -and [double]$cold.process_wall_ms -le 900000.0
    $warmBudgetPassed = $warm.Count -eq 2 -and @($warm | Where-Object {
        [double]$_.process_wall_ms -gt 300000.0
    }).Count -eq 0
    $memoryBudgetBytes = [uint64]$MaximumMemoryMiB * [uint64]1024 * [uint64]1024
    $memoryPassed = $performanceRuns.Count -eq 3 -and @($performanceRuns | Where-Object {
        [uint64]$_.peak_working_set_bytes -gt $memoryBudgetBytes -or
        [uint64]$_.peak_private_bytes -gt $memoryBudgetBytes
    }).Count -eq 0
    $fitCountPassed = $performanceRuns.Count -eq 3 -and @($performanceRuns | Where-Object {
        [int]$_.full_fit_count -gt 3
    }).Count -eq 0
    $stageMeasurements = [ordered]@{}
    $stageViolations = @()
    if ($null -ne $cold) {
        function Get-ColdStageWallMilliseconds {
            param([Parameter(Mandatory = $true)][string[]]$StageNames)
            return [double](@($cold.stages | Where-Object { $_.stage -in $StageNames } |
                Measure-Object -Property wall_ms -Sum).Sum)
        }

        $stageMeasurements["input_and_constraints_ms"] = [double]$cold.input_read_ms +
            [double]$cold.candidate_resolution_ms + (Get-ColdStageWallMilliseconds @("Constraints"))
        $stageMeasurements["sampling_ms"] = Get-ColdStageWallMilliseconds @("Sampling")
        $stageMeasurements["layout_ms"] = Get-ColdStageWallMilliseconds @(
            "CurveNetwork", "Layout")
        $fitAttemptWalls = @($cold.fit.attempts | ForEach-Object { [double]$_.wall_ms })
        $stageMeasurements["maximum_single_fit_ms"] = if ($fitAttemptWalls.Count -gt 0) {
            [double](($fitAttemptWalls | Measure-Object -Maximum).Maximum)
        } else { 0.0 }
        $stageMeasurements["cumulative_fit_ms"] = [double](($fitAttemptWalls | Measure-Object -Sum).Sum)
        $stageMeasurements["retrim_ms"] = Get-ColdStageWallMilliseconds @("Retrim")
        $stageMeasurements["final_validation_ms"] = Get-ColdStageWallMilliseconds @(
            "Sewing", "StrictTopology", "StepRoundtrip")

        $stageLimits = [ordered]@{
            input_and_constraints_ms = 10000
            sampling_ms = 90000
            layout_ms = 60000
            maximum_single_fit_ms = 180000
            cumulative_fit_ms = 450000
            retrim_ms = 90000
            final_validation_ms = 180000
        }
        foreach ($entry in $stageLimits.GetEnumerator()) {
            if ([double]$stageMeasurements[$entry.Key] -gt [double]$entry.Value) {
                $stageViolations += [ordered]@{
                    stage = $entry.Key
                    actual_ms = [double]$stageMeasurements[$entry.Key]
                    limit_ms = [double]$entry.Value
                }
            }
        }
    }
    $stageBudgetsPassed = $null -ne $cold -and $stageViolations.Count -eq 0
    $algorithmPassed = $runs.Count -eq 3 -and @($runs | Where-Object exit_code -ne 0).Count -eq 0
    $performancePassed = $algorithmPassed -and $coldBudgetPassed -and $warmBudgetPassed -and
        $memoryPassed -and $fitCountPassed -and $consistencyPassed -and $stageBudgetsPassed
    [ordered]@{
        schema_version = 1
        passed = $performancePassed
        cold_definition = "Fresh unique algorithm cache; operating-system file cache is not cleared."
        limits = [ordered]@{
            cold_wall_ms = 900000
            warm_wall_ms = 300000
            memory_bytes = $memoryBudgetBytes
            maximum_full_fits = 3
            stages_ms = [ordered]@{
                input_and_constraints = 10000
                sampling = 90000
                layout = 60000
                maximum_single_fit = 180000
                cumulative_fit = 450000
                retrim = 90000
                final_validation = 180000
            }
        }
        gates = [ordered]@{
            algorithm = $algorithmPassed
            cold_wall = $coldBudgetPassed
            warm_wall = $warmBudgetPassed
            memory = $memoryPassed
            full_fit_count = $fitCountPassed
            deterministic_consistency = $consistencyPassed
            stage_budgets = $stageBudgetsPassed
        }
        stage_budget_measurements = $stageMeasurements
        stage_budget_violations = $stageViolations
        runs = $performanceRuns
    } | ConvertTo-Json -Depth 100 | Set-Content `
        -LiteralPath (Join-Path $context.directories.diagnostics "performance_report.json") `
        -Encoding UTF8
} else {
    $runs += Invoke-NativeRegionRefitProbe `
        -Identity "single" `
        -LogDirectory $context.directories.logs
    $performancePassed = $runs[0].exit_code -eq 0
}

$primaryRun = $runs[0]
$status = if ($primaryRun.exit_code -eq 0 -and $performancePassed) { "succeeded" } else { "failed" }
$message = if ($status -eq "succeeded") {
    "Native regional refit probe and requested performance gates completed."
} else {
    "Native regional refit or performance acceptance failed; inspect diagnostics and logs."
}
$artifacts = [ordered]@{}
$samplingStl = Join-Path $context.directories.artifacts "sampling\adaptive_reference.stl"
if (Test-Path -LiteralPath $samplingStl -PathType Leaf) {
    $artifacts["adaptive_sampling_stl"] = $samplingStl
}
$layoutCarrierStl = Join-Path $context.directories.artifacts "sampling\layout_carrier.stl"
if (Test-Path -LiteralPath $layoutCarrierStl -PathType Leaf) {
    $artifacts["layout_carrier_stl"] = $layoutCarrierStl
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
$curveNetworkJson = Join-Path $context.directories.artifacts "curve_network\selected_curve_network.json"
$curveNetworkPly = Join-Path $context.directories.artifacts "curve_network\selected_curve_network.ply"
if (Test-Path -LiteralPath $curveNetworkJson -PathType Leaf) {
    $artifacts["selected_curve_network_json"] = $curveNetworkJson
}
if (Test-Path -LiteralPath $curveNetworkPly -PathType Leaf) {
    $artifacts["selected_curve_network_ply"] = $curveNetworkPly
}
$fittedPatchNetwork = Join-Path $context.directories.artifacts "fit\fitted_patch_network.step"
if (Test-Path -LiteralPath $fittedPatchNetwork -PathType Leaf) {
    $artifacts["fitted_patch_network_step"] = $fittedPatchNetwork
}
$bestEffortPatchNetwork = Join-Path $context.directories.artifacts "fit\best_effort_fitted_patch_network.step"
if (Test-Path -LiteralPath $bestEffortPatchNetwork -PathType Leaf) {
    $artifacts["best_effort_fitted_patch_network_step"] = $bestEffortPatchNetwork
}
$cadCheckCandidate = Join-Path $context.directories.artifacts "brep\cad_check_candidate.step"
if (Test-Path -LiteralPath $cadCheckCandidate -PathType Leaf) {
    $artifacts["cad_check_candidate_step"] = $cadCheckCandidate
}
$sewnAttempt = Join-Path $context.directories.artifacts "brep\sewn_attempt.step"
if (Test-Path -LiteralPath $sewnAttempt -PathType Leaf) {
    $artifacts["sewn_attempt_step"] = $sewnAttempt
}
$finalArtifactStatus = Join-Path $context.directories.artifacts "final\artifact_status.json"
$finalSewn = Join-Path $context.directories.artifacts "final\final_sewn.step"
$diagnosticSewn = Join-Path $context.directories.artifacts "final\diagnostic_sewn.step"
if (Test-Path -LiteralPath $finalArtifactStatus -PathType Leaf) {
    $artifacts["final_artifact_status_json"] = $finalArtifactStatus
}
if (Test-Path -LiteralPath $finalSewn -PathType Leaf) {
    $artifacts["final_sewn_step"] = $finalSewn
}
if (Test-Path -LiteralPath $diagnosticSewn -PathType Leaf) {
    $artifacts["diagnostic_sewn_step"] = $diagnosticSewn
}

$creoScript = Join-Path $PSScriptRoot "run_creo_step_diagnostic.ps1"
$creoOutput = Join-Path $context.output_dir "external_cad\creo"
$externalCadStep = if (Test-Path -LiteralPath $finalSewn -PathType Leaf) {
    $finalSewn
} elseif (Test-Path -LiteralPath $diagnosticSewn -PathType Leaf) {
    $diagnosticSewn
} else {
    $null
}
if ($null -ne $externalCadStep -and
    (Get-Item -LiteralPath $externalCadStep).Length -gt 0 -and
    (Test-Path -LiteralPath $creoScript -PathType Leaf)) {
    New-Item -ItemType Directory -Path $creoOutput -Force | Out-Null
    $nmsdBefore = @(
        Get-Process -Name "nmsd" -ErrorAction SilentlyContinue |
            Select-Object -ExpandProperty Id)
    $powershellExecutable = (Get-Process -Id $PID).Path
    $creoProcess = Start-Process `
        -FilePath $powershellExecutable `
        -ArgumentList @(
            "-NoProfile",
            "-ExecutionPolicy", "Bypass",
            "-File", $creoScript,
            "-StepPath", $externalCadStep,
            "-OutputDir", $creoOutput) `
        -WorkingDirectory $repoRoot `
        -WindowStyle Hidden `
        -PassThru
    $creoProcess.WaitForExit()
    $creoProcess.Refresh()
    $creoExitCode = [int]$creoProcess.ExitCode
    $creoProcess.Dispose()
    $artifacts["creo_diagnostic_directory"] = $creoOutput
    $artifacts["creo_diagnostic_exit_code"] = $creoExitCode
    $newNmsdIds = @(
        Get-Process -Name "nmsd" -ErrorAction SilentlyContinue |
            Where-Object { $_.Id -notin $nmsdBefore } |
            Select-Object -ExpandProperty Id)
    if ($newNmsdIds.Count -gt 0) {
        $creoDescendantStopTimeoutSeconds = 10
        try {
            $newNmsdProcesses = @(Get-Process -Id $newNmsdIds -ErrorAction Stop)
            $newNmsdProcesses | Stop-Process -Force -ErrorAction Stop
            $newNmsdProcesses | Wait-Process `
                -Timeout $creoDescendantStopTimeoutSeconds `
                -ErrorAction Stop
        } catch {
            Write-Warning "Creo name-server descendants could not be stopped before evidence finalization."
        }
    }
}

Complete-SpoRun -Context $context -Status $status -Message $message -Artifacts $artifacts
$performancePath = Join-Path $context.directories.diagnostics "performance_report.json"
$runJsonPath = Join-Path $context.output_dir "run.json"
if ((Test-Path -LiteralPath $performancePath -PathType Leaf) -and
    (Test-Path -LiteralPath $runJsonPath -PathType Leaf)) {
    $performance = Get-Content -LiteralPath $performancePath -Raw -Encoding UTF8 | ConvertFrom-Json
    $runJson = Get-Content -LiteralPath $runJsonPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $runJson | Add-Member -NotePropertyName diagnostic_pipeline_completed `
        -NotePropertyValue ([bool]$performance.diagnostic_pipeline_completed) -Force
    $runJson | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $runJsonPath -Encoding UTF8
}
$manifestPath = Join-Path $context.output_dir "evidence_manifest.json"
$requiredDiagnosticNames = @(
    "sampling_report.json",
    "curve_network.json",
    "layout_report.json",
    "fit_report.json",
    "edge_consistency_report.json",
    "triangulation_report.json",
    "retrim_report.json",
    "sewing_report.json",
    "strict_topology_report.json",
    "output_complexity_report.json",
    "performance_report.json")
function Get-SharedFileSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    $stream = [System.IO.FileStream]::new(
        $Path,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::ReadWrite)
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        return [System.BitConverter]::ToString(
            $sha256.ComputeHash($stream)).Replace("-", "")
    } finally {
        $sha256.Dispose()
        $stream.Dispose()
    }
}
$evidence = @(
    Get-ChildItem -LiteralPath $context.output_dir -Recurse -File |
        Where-Object { $_.FullName -ne $manifestPath } |
        Sort-Object FullName |
        ForEach-Object {
            $relativePath = [System.IO.Path]::GetRelativePath($context.output_dir, $_.FullName)
            [ordered]@{
                name = $relativePath.Replace("\", "/")
                path = $relativePath
                sha256 = Get-SharedFileSha256 -Path $_.FullName
                generated_stage = if ($relativePath -like "external_cad*") {
                    "ExternalCad"
                } elseif ($relativePath -like "artifacts*") {
                    "Artifact"
                } elseif ($relativePath -like "logs*") {
                    "Log"
                } else {
                    "Run"
                }
                required = $false
                validated = ($status -eq "succeeded")
            }
        }
)
[ordered]@{
    schema_version = 2
    run_id = $context.run_id
    manifest_self_excluded = $true
    expected_diagnostics = $requiredDiagnosticNames
    evidence = $evidence
} | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
Write-Output $context.output_dir
exit $(if ($status -eq "succeeded") { 0 } else { 1 })
