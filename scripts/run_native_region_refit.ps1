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
    [ValidateSet("geomagic-equivalent", "feature-preserving")]
    [string]$FittingMode = "feature-preserving",
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
$nativeRegionalRefitRunRoot =
    "D:\pyProject\step-patch-optimizer-worktrees\native-regional-refit\data\runs"
$nativeRegionalRefitStagingRoot =
    Join-Path $repoRoot "data\.native-region-refit-staging"
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
    -OutputDir "" `
    -DefaultRunRoot $nativeRegionalRefitStagingRoot `
    -ScriptPath $PSCommandPath `
    -Parameters $PSBoundParameters
$stagingDirectory = [System.IO.Path]::GetFullPath($context.output_dir)
$finalRunDirectory = if ($OutputDir) {
    if ([System.IO.Path]::IsPathRooted($OutputDir)) {
        [System.IO.Path]::GetFullPath($OutputDir)
    } else {
        [System.IO.Path]::GetFullPath((Join-Path $repoRoot $OutputDir))
    }
} else {
    Join-Path $nativeRegionalRefitRunRoot $context.run_id
}
$publishedFinalDirectory = Join-Path $finalRunDirectory "final"
$runFailure = $null
$publishFailure = $null
$scriptExitCode = 1

try {
if (Test-Path -LiteralPath $finalRunDirectory) {
    throw "Final run directory already exists; refusing to overwrite it: $finalRunDirectory"
}

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
    "--fitting-mode", $FittingMode,
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
$layoutJson = Join-Path $context.directories.artifacts "layout\selected_parameter_domain.json"
$layoutPly = Join-Path $context.directories.artifacts "layout\selected_parameter_domain.ply"
$layoutSvg = Join-Path $context.directories.artifacts "layout\selected_parameter_domain.svg"
if (Test-Path -LiteralPath $layoutJson -PathType Leaf) {
    $artifacts["selected_parameter_domain_json"] = $layoutJson
}
if (Test-Path -LiteralPath $layoutPly -PathType Leaf) {
    $artifacts["selected_parameter_domain_ply"] = $layoutPly
}
if (Test-Path -LiteralPath $layoutSvg -PathType Leaf) {
    $artifacts["selected_parameter_domain_svg"] = $layoutSvg
}
$curveNetworkJson = Join-Path $context.directories.artifacts "curve_network\selected_curve_network.json"
$curveNetworkPly = Join-Path $context.directories.artifacts "curve_network\selected_curve_network.ply"
$curveNetworkStep = Join-Path $context.directories.artifacts "curve_network\selected_curve_network.step"
if (Test-Path -LiteralPath $curveNetworkJson -PathType Leaf) {
    $artifacts["selected_curve_network_json"] = $curveNetworkJson
}
if (Test-Path -LiteralPath $curveNetworkPly -PathType Leaf) {
    $artifacts["selected_curve_network_ply"] = $curveNetworkPly
}
if (Test-Path -LiteralPath $curveNetworkStep -PathType Leaf) {
    $artifacts["selected_curve_network_step"] = $curveNetworkStep
}
$fittedPatchNetwork = Join-Path $context.directories.artifacts "fit\fitted_support_surfaces.step"
if (Test-Path -LiteralPath $fittedPatchNetwork -PathType Leaf) {
    $artifacts["fitted_support_surfaces_step"] = $fittedPatchNetwork
}
$bestEffortPatchNetwork = Join-Path $context.directories.artifacts "fit\best_effort_support_surfaces.step"
if (Test-Path -LiteralPath $bestEffortPatchNetwork -PathType Leaf) {
    $artifacts["best_effort_support_surfaces_step"] = $bestEffortPatchNetwork
}
$replacementQuilt = Join-Path $context.directories.artifacts "brep\replacement_quilt.step"
if (Test-Path -LiteralPath $replacementQuilt -PathType Leaf) {
    $artifacts["replacement_quilt_step"] = $replacementQuilt
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

function Get-OptionalJson {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $null
    }
    try {
        return Get-Content -LiteralPath $Path -Raw -Encoding UTF8 |
            ConvertFrom-Json
    } catch {
        return $null
    }
}

function Copy-PublishedFile {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination
    )

    if (-not (Test-Path -LiteralPath $Source -PathType Leaf) -or
        (Get-Item -LiteralPath $Source).Length -le 0) {
        return $false
    }
    Copy-Item -LiteralPath $Source -Destination $Destination
    return $true
}

function Publish-NativeRegionRefitFinal {
    param(
        [Parameter(Mandatory = $true)][string]$StagingDirectory,
        [Parameter(Mandatory = $true)][string]$FinalRunDirectory,
        [Parameter(Mandatory = $true)]$Context
    )

    if (Test-Path -LiteralPath $FinalRunDirectory) {
        throw "Final run directory already exists; refusing to overwrite it: $FinalRunDirectory"
    }
    $finalParent = [System.IO.Path]::GetFullPath(
        (Split-Path -Parent $FinalRunDirectory))
    New-Item -ItemType Directory -Path $finalParent -Force | Out-Null
    $transactionDirectory = [System.IO.Path]::GetFullPath(
        "$FinalRunDirectory.__publishing__$([guid]::NewGuid().ToString('N'))")
    $expectedParentPrefix = $finalParent.TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar) +
        [System.IO.Path]::DirectorySeparatorChar
    if (-not $transactionDirectory.StartsWith(
            $expectedParentPrefix,
            [System.StringComparison]::OrdinalIgnoreCase) -or
        -not ([System.IO.Path]::GetFullPath($FinalRunDirectory)).StartsWith(
            $expectedParentPrefix,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Publishing paths escaped the intended run parent."
    }
    $packageFinalDirectory = Join-Path $transactionDirectory "final"
    New-Item -ItemType Directory -Path $packageFinalDirectory -Force |
        Out-Null

    try {
        $stagedArtifactStatus = Get-OptionalJson -Path (
            Join-Path $StagingDirectory "artifacts\final\artifact_status.json")
        $creoResult = Get-OptionalJson -Path (
            Join-Path $StagingDirectory "external_cad\creo\creo_step_diagnostic_result.json")
        $creoAttempted = $null -ne $creoResult
        $creoPassed = $creoAttempted -and
            [bool]$creoResult.import.generated_prt -and
            [bool]$creoResult.modelcheck.diagnostic_passed
        $productGatesPassedBeforeCreo = $null -ne $stagedArtifactStatus -and
            [bool]$stagedArtifactStatus.product_validated

        $stagedFinalSewn = Join-Path $StagingDirectory (
            "artifacts\final\final_sewn.step")
        $stagedDiagnosticSewn = Join-Path $StagingDirectory (
            "artifacts\final\diagnostic_sewn.step")
        $hasStagedFinalSewn =
            Test-Path -LiteralPath $stagedFinalSewn -PathType Leaf
        $hasStagedDiagnosticSewn =
            Test-Path -LiteralPath $stagedDiagnosticSewn -PathType Leaf
        if ($hasStagedFinalSewn -and $hasStagedDiagnosticSewn) {
            throw "Staging output contains both final_sewn.step and diagnostic_sewn.step."
        }
        $sewnSource = if ($hasStagedDiagnosticSewn) {
            $stagedDiagnosticSewn
        } elseif ($hasStagedFinalSewn) {
            $stagedFinalSewn
        } else {
            $null
        }
        $constructibleGeometry = $null -ne $sewnSource -and
            (Get-Item -LiteralPath $sewnSource).Length -gt 0
        $productValidated = $constructibleGeometry -and
            $productGatesPassedBeforeCreo -and $creoPassed
        $finalStatus = if ($productValidated) {
            "ProductValidated"
        } elseif ($constructibleGeometry) {
            "DiagnosticOnly"
        } else {
            "Unavailable"
        }
        $sewnName = if ($productValidated) {
            "final_sewn.step"
        } elseif ($constructibleGeometry) {
            "diagnostic_sewn.step"
        } else {
            $null
        }

        $publishSources = [ordered]@{
            "curve_network.json" = @(
                (Join-Path $StagingDirectory (
                    "artifacts\curve_network\selected_curve_network.json")),
                (Join-Path $StagingDirectory "diagnostics\curve_network.json"))
            "curve_network.step" = @(
                (Join-Path $StagingDirectory (
                    "artifacts\curve_network\selected_curve_network.step")))
            "untrimmed_support_surfaces.step" = @(
                (Join-Path $StagingDirectory (
                    "artifacts\fit\fitted_support_surfaces.step")),
                (Join-Path $StagingDirectory (
                    "artifacts\fit\best_effort_support_surfaces.step")))
            "trimmed_patch_faces.step" = @(
                (Join-Path $StagingDirectory (
                    "artifacts\brep\replacement_quilt.step")))
        }
        foreach ($entry in $publishSources.GetEnumerator()) {
            foreach ($source in $entry.Value) {
                if (Copy-PublishedFile `
                        -Source $source `
                        -Destination (Join-Path $packageFinalDirectory $entry.Key)) {
                    break
                }
            }
        }
        if ($constructibleGeometry) {
            Copy-PublishedFile `
                -Source $sewnSource `
                -Destination (Join-Path $packageFinalDirectory $sewnName) |
                Out-Null
        }

        $publishedSewnPath = if ($sewnName) {
            Join-Path $packageFinalDirectory $sewnName
        } else {
            $null
        }
        $firstProductFailureGate = if ($productValidated) {
            $null
        } elseif ($productGatesPassedBeforeCreo -and -not $creoPassed) {
            "Creo"
        } elseif ($null -ne $stagedArtifactStatus) {
            $stagedArtifactStatus.first_product_failure_gate
        } else {
            $null
        }
        $unavailableReason = if ($constructibleGeometry) {
            $null
        } elseif ($null -ne $stagedArtifactStatus) {
            $stagedArtifactStatus.unavailable_reason
        } else {
            "The probe did not produce constructible sewn geometry."
        }
        $publishedArtifactStatus = [ordered]@{
            schema_version = 2
            status = $finalStatus
            product_gates_passed_before_creo =
                $productGatesPassedBeforeCreo
            creo_attempted = $creoAttempted
            creo_validation_passed = $creoPassed
            product_validated = $productValidated
            diagnostic_only =
                $constructibleGeometry -and -not $productValidated
            can_apply = $productValidated
            constructible_geometry = $constructibleGeometry
            sewn_step = $sewnName
            sha256 = if ($publishedSewnPath) {
                Get-SharedFileSha256 -Path $publishedSewnPath
            } else {
                $null
            }
            sewing_tolerance_mm = 0.001
            first_product_failure_gate = $firstProductFailureGate
            unavailable_reason = $unavailableReason
            message = if ($null -ne $stagedArtifactStatus) {
                $stagedArtifactStatus.message
            } else {
                $Context.message
            }
        }
        $publishedArtifactStatus | ConvertTo-Json -Depth 20 |
            Set-Content -LiteralPath (
                Join-Path $packageFinalDirectory "artifact_status.json") `
                -Encoding UTF8

        $performancePath = Get-ChildItem `
            -LiteralPath (Join-Path $StagingDirectory "diagnostics") `
            -Filter "performance_*.json" -File -ErrorAction SilentlyContinue |
            Sort-Object Name | Select-Object -First 1 -ExpandProperty FullName
        $validationSummary = [ordered]@{
            schema_version = 1
            run_id = $Context.run_id
            status = $finalStatus
            product_validated = $productValidated
            can_apply = $productValidated
            curve_network = Get-OptionalJson -Path (
                Join-Path $StagingDirectory "diagnostics\curve_network.json")
            layout = Get-OptionalJson -Path (
                Join-Path $StagingDirectory "diagnostics\layout_report.json")
            fitting = Get-OptionalJson -Path (
                Join-Path $StagingDirectory "diagnostics\fit_report.json")
            brep = [ordered]@{
                retrim = Get-OptionalJson -Path (
                    Join-Path $StagingDirectory "diagnostics\retrim_report.json")
                edge_consistency = Get-OptionalJson -Path (
                    Join-Path $StagingDirectory (
                        "diagnostics\edge_consistency_report.json"))
                triangulation = Get-OptionalJson -Path (
                    Join-Path $StagingDirectory (
                        "diagnostics\triangulation_report.json"))
                output_complexity = Get-OptionalJson -Path (
                    Join-Path $StagingDirectory (
                        "diagnostics\output_complexity_report.json"))
                step_face_lineage = Get-OptionalJson -Path (
                    Join-Path $StagingDirectory (
                        "diagnostics\step_face_lineage_report.json"))
            }
            sewing = Get-OptionalJson -Path (
                Join-Path $StagingDirectory "diagnostics\sewing_report.json")
            step_roundtrip = Get-OptionalJson -Path (
                Join-Path $StagingDirectory (
                    "diagnostics\strict_topology_report.json"))
            performance = if ($performancePath) {
                Get-OptionalJson -Path $performancePath
            } else {
                $null
            }
            creo = [ordered]@{
                attempted = $creoAttempted
                captured = if ($creoAttempted) {
                    [bool]$creoResult.captured
                } else {
                    $false
                }
                generated_prt = if ($creoAttempted) {
                    [bool]$creoResult.import.generated_prt
                } else {
                    $false
                }
                modelcheck_passed = if ($creoAttempted) {
                    [bool]$creoResult.modelcheck.diagnostic_passed
                } else {
                    $false
                }
                validation_passed = $creoPassed
                status = if ($creoAttempted) {
                    $creoResult.status
                } else {
                    "NotAttempted"
                }
                import_validation = if ($creoAttempted) {
                    $creoResult.modelcheck.summary.import_validation
                } else {
                    $null
                }
            }
        }
        $validationSummary | ConvertTo-Json -Depth 100 |
            Set-Content -LiteralPath (
                Join-Path $packageFinalDirectory "validation_summary.json") `
                -Encoding UTF8

        $publishedRun = Get-OptionalJson -Path (
            Join-Path $StagingDirectory "run.json")
        if ($null -eq $publishedRun) {
            $publishedRun = $Context
        }
        $publishedRun.output_dir = $FinalRunDirectory
        $publishedRun.directories = [ordered]@{
            final = Join-Path $FinalRunDirectory "final"
        }
        $publishedRun.artifacts = [ordered]@{}
        foreach ($file in Get-ChildItem `
                -LiteralPath $packageFinalDirectory -File |
                Sort-Object Name) {
            $publishedRun.artifacts[$file.BaseName] = $file.Name
        }
        $publishedRun | ConvertTo-Json -Depth 100 |
            Set-Content -LiteralPath (
                Join-Path $packageFinalDirectory "run.json") -Encoding UTF8

        $runMarkdown = @(
            "# Run $($Context.run_id)",
            "",
            "| 字段 | 值 |",
            "|---|---|",
            "| 状态 | $finalStatus |",
            "| 产品门在 Creo 前通过 | $productGatesPassedBeforeCreo |",
            "| Creo 验证通过 | $creoPassed |",
            "| 可 Apply | $productValidated |",
            "| 固定诊断缝合容差 | 0.001 mm |",
            "| 最终目录 | $(Join-Path $FinalRunDirectory 'final') |",
            "",
            "## 说明",
            "",
            "$($Context.message)",
            "",
            "## 发布文件",
            ""
        )
        foreach ($file in Get-ChildItem `
                -LiteralPath $packageFinalDirectory -File |
                Sort-Object Name) {
            $runMarkdown += "- $($file.Name)"
        }
        $runMarkdown | Set-Content -LiteralPath (
            Join-Path $packageFinalDirectory "RUN.md") -Encoding UTF8

        $manifestPath = Join-Path $packageFinalDirectory (
            "evidence_manifest.json")
        $evidence = @(
            Get-ChildItem -LiteralPath $packageFinalDirectory -File |
                Where-Object { $_.FullName -ne $manifestPath } |
                Sort-Object Name |
                ForEach-Object {
                    $role = switch ($_.Name) {
                        "run.json" { "RunMetadata" }
                        "RUN.md" { "RunSummary" }
                        "artifact_status.json" { "ArtifactStatus" }
                        "validation_summary.json" { "ValidationSummary" }
                        "curve_network.json" { "CurveNetworkData" }
                        "curve_network.step" { "CurveNetworkGeometry" }
                        "untrimmed_support_surfaces.step" {
                            "UntrimmedSupportSurfaces"
                        }
                        "trimmed_patch_faces.step" {
                            "TrimmedReplacementFaces"
                        }
                        "final_sewn.step" { "ProductSewnShape" }
                        "diagnostic_sewn.step" { "DiagnosticSewnShape" }
                        default { "Unexpected" }
                    }
                    [ordered]@{
                        name = $_.Name
                        role = $role
                        size_bytes = [uint64]$_.Length
                        sha256 = Get-SharedFileSha256 -Path $_.FullName
                    }
                }
        )
        [ordered]@{
            schema_version = 3
            run_id = $Context.run_id
            manifest_self_excluded = $true
            evidence = $evidence
        } | ConvertTo-Json -Depth 20 |
            Set-Content -LiteralPath $manifestPath -Encoding UTF8

        Move-Item `
            -LiteralPath $transactionDirectory `
            -Destination $FinalRunDirectory
    } catch {
        if (Test-Path -LiteralPath $transactionDirectory) {
            Remove-Item -LiteralPath $transactionDirectory -Recurse -Force
        }
        throw
    }
    return Join-Path $FinalRunDirectory "final"
}

$scriptExitCode = if ($status -eq "succeeded") { 0 } else { 1 }
} catch {
    $runFailure = $_
    try {
        Complete-SpoRun `
            -Context $context `
            -Status "failed" `
            -Message ("Native regional refit wrapper failed: " + $_.Exception.Message) `
            -Artifacts ([ordered]@{})
    } catch {
        $context.status = "failed"
        $context.message = "Native regional refit wrapper failed before run metadata could be finalized."
    }
} finally {
    try {
        $publishedFinalDirectory = Publish-NativeRegionRefitFinal `
            -StagingDirectory $stagingDirectory `
            -FinalRunDirectory $finalRunDirectory `
            -Context $context
    } catch {
        $publishFailure = $_
    }
    $resolvedStagingRoot = [System.IO.Path]::GetFullPath(
        $nativeRegionalRefitStagingRoot).TrimEnd(
            [System.IO.Path]::DirectorySeparatorChar) +
        [System.IO.Path]::DirectorySeparatorChar
    if (-not $stagingDirectory.StartsWith(
            $resolvedStagingRoot,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        if ($null -eq $publishFailure) {
            $publishFailure =
                "Staging cleanup target escaped the configured staging root."
        }
    } elseif (Test-Path -LiteralPath $stagingDirectory) {
        Remove-Item -LiteralPath $stagingDirectory -Recurse -Force
    }
}

if ($null -ne $publishFailure) {
    Write-Error ("Final publication failed: " + $publishFailure)
    exit 1
}
if ($null -ne $runFailure) {
    Write-Error ("Native regional refit failed: " + $runFailure)
    exit 1
}
Write-Output $publishedFinalDirectory
exit $scriptExitCode
