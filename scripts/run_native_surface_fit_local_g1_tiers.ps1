param(
    [string]$Preset = "windows-msvc-debug",
    [Parameter(Mandatory = $true)][string]$SourceStep,
    [Parameter(Mandatory = $true)][ValidateRange(0, [int]::MaxValue)][int]$CandidateId
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot
. (Join-Path $PSScriptRoot "run_artifacts.ps1")

function Convert-ToRepoAbsolutePath {
    param([Parameter(Mandatory = $true)][string]$PathValue)

    if ([System.IO.Path]::IsPathRooted($PathValue)) {
        return [System.IO.Path]::GetFullPath($PathValue)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $repoRoot $PathValue))
}

function Find-DebugTool {
    param([Parameter(Mandatory = $true)][string]$Name)

    foreach ($candidate in @(
        (Join-Path $repoRoot "build\$Preset\Debug\$Name.exe"),
        (Join-Path $repoRoot "build\$Preset\$Name.exe")
    )) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }
    throw "$Name.exe was not found under build\$Preset after build."
}

function Get-RequiredProperty {
    param(
        [Parameter(Mandatory = $true)]$Object,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) {
        throw "Probe report is missing required property: $Name"
    }
    return $property.Value
}

function New-FailureComparisonRow {
    param(
        [Parameter(Mandatory = $true)][int]$ConfiguredG1Count,
        [Parameter(Mandatory = $true)][string]$RunDirectory,
        [Parameter(Mandatory = $true)][string]$ErrorMessage
    )

    return [pscustomobject]([ordered]@{
        configured_g1_count = $ConfiguredG1Count
        actual_g1_count = $null
        g0_training_constraints = $null
        total_training_constraints = $null
        patch_acceptable = $false
        fit_success = $false
        failed_gates = "RunnerFailure"
        boundary_max = $null; boundary_p95 = $null; boundary_rms = $null; boundary_over_tolerance = $null
        corner_max = $null; corner_p95 = $null; corner_rms = $null; corner_over_tolerance = $null
        selection_interior_max = $null; selection_interior_p95 = $null; selection_interior_rms = $null; selection_interior_over_tolerance = $null
        selection_normal_max_degrees = $null; selection_normal_p95_degrees = $null; selection_normal_rms_degrees = $null; selection_normal_over_tolerance = $null
        holdout_interior_max = $null; holdout_interior_p95 = $null; holdout_interior_rms = $null; holdout_interior_over_tolerance = $null
        holdout_normal_max_degrees = $null; holdout_normal_p95_degrees = $null; holdout_normal_rms_degrees = $null; holdout_normal_over_tolerance = $null
        per_face_training_min = $null; per_face_training_max = $null; per_face_training_zero_count = $null
        u_degree = $null; v_degree = $null; total_poles = $null
        u_segments = $null; v_segments = $null; total_segments = $null
        jacobian_passed = $false; jacobian_min_normalized = $null; jacobian_near_singular = $null; jacobian_flipped_pairs = $null
        filling_face_brep_check_valid = $false; oriented_face_brep_check_valid = $false; step_roundtrip_brep_check_valid = $false
        step_roundtrip_face_count = $null; step_roundtrip_edge_count = $null
        training_selection_overlap_count = $null; training_holdout_overlap_count = $null; selection_holdout_overlap_count = $null
        duration_ms = $null; probe_exit_code = $null; step_stats_exit_code = $null
        run_directory = $RunDirectory
        runner_error = $ErrorMessage
    })
}

$SourceStep = Convert-ToRepoAbsolutePath -PathValue $SourceStep
if (-not (Test-Path -LiteralPath $SourceStep -PathType Leaf)) {
    throw "SourceStep does not exist: $SourceStep"
}

$vcpkgRoot = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { Join-Path $env:USERPROFILE "vcpkg" }
$env:VCPKG_ROOT = $vcpkgRoot
$env:Path = "C:\Program Files\CMake\bin;$vcpkgRoot;" + $env:Path

& cmake --build --preset $Preset --target native_surface_fit_probe step_stats -- /m:1
if ($LASTEXITCODE -ne 0) {
    throw "Native surface probe tools build failed with exit code $LASTEXITCODE"
}
$probeExe = Find-DebugTool -Name "native_surface_fit_probe"
$stepStatsExe = Find-DebugTool -Name "step_stats"

$trainingSamples = 96
$selectionSamples = 192
$holdoutSamples = 192
$boundarySamplesPerEdge = 32
$jacobianGridSize = 16
$tolerance3d = 0.03
$normalToleranceDegrees = 15.0
$g1Tiers = @(0, 1, 4, 8)

$comparisonRows = @()
$tierRunDirectories = @()
$runnerFailures = @()
$fixedConfigurationJson = $null
$previousSelectedTrainingIndices = @()

foreach ($configuredG1Count in $g1Tiers) {
    $variant = "nsr-1.3-local-g1-$configuredG1Count"
    $runContext = $null
    $runDirectory = ""
    try {
        $parameters = [ordered]@{
            Preset = $Preset
            SourceStep = $SourceStep
            CandidateId = $CandidateId
            SamplingStrategy = "face_stratified_area_weighted"
            TrainingSamples = $trainingSamples
            SelectionSamples = $selectionSamples
            HoldoutSamples = $holdoutSamples
            LocalG1Count = $configuredG1Count
            BoundarySamplesPerEdge = $boundarySamplesPerEdge
            JacobianGridSize = $jacobianGridSize
            Tolerance3d = $tolerance3d
            NormalToleranceDegrees = $normalToleranceDegrees
        }
        $runContext = New-SpoRunContext `
            -RepoRoot $repoRoot `
            -Workflow "native-surface-fit" `
            -SourcePath $SourceStep `
            -CandidateId ([string]$CandidateId) `
            -Variant $variant `
            -ScriptPath $PSCommandPath `
            -Parameters $parameters
        $runDirectory = $runContext.output_dir
        $tierRunDirectories += $runDirectory

        $candidateToken = "{0:D4}" -f $CandidateId
        $patchPath = Join-Path $runContext.directories.artifacts "candidate_${candidateToken}_native_surface_patch.stp"
        $reportPath = Join-Path $runContext.output_dir "native_surface_fit_report.json"
        $diagnosticDirectory = Join-Path $runContext.directories.diagnostics "native_surface_fit"
        $distanceCsvPath = Join-Path $diagnosticDirectory "surface_error_samples.csv"
        $distanceObjPath = Join-Path $diagnosticDirectory "surface_error_vectors.obj"
        $normalCsvPath = Join-Path $diagnosticDirectory "surface_normal_error_samples.csv"
        $stdoutPath = Join-Path $runContext.directories.logs "native_surface_fit_probe.stdout.txt"
        $stderrPath = Join-Path $runContext.directories.logs "native_surface_fit_probe.stderr.txt"
        $stepStatsStdoutPath = Join-Path $runContext.directories.logs "step_stats.stdout.txt"
        $stepStatsStderrPath = Join-Path $runContext.directories.logs "step_stats.stderr.txt"

        $probeArguments = @(
            "--source-step", $SourceStep,
            "--candidate-id", ([string]$CandidateId),
            "--output-patch", $patchPath,
            "--report", $reportPath,
            "--sampling-strategy", "face_stratified_area_weighted",
            "--training-samples", ([string]$trainingSamples),
            "--selection-samples", ([string]$selectionSamples),
            "--holdout-samples", ([string]$holdoutSamples),
            "--local-g1-count", ([string]$configuredG1Count),
            "--boundary-samples-per-edge", ([string]$boundarySamplesPerEdge),
            "--jacobian-grid-size", ([string]$jacobianGridSize),
            "--tolerance3d", ([string]$tolerance3d),
            "--normal-tolerance-degrees", ([string]$normalToleranceDegrees)
        )

        $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
        & $probeExe @probeArguments 1> $stdoutPath 2> $stderrPath
        $probeExitCode = $LASTEXITCODE
        $stopwatch.Stop()

        foreach ($requiredPath in @(
            $reportPath,
            $patchPath,
            $distanceCsvPath,
            $distanceObjPath,
            $normalCsvPath,
            $stdoutPath,
            $stderrPath
        )) {
            if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
                throw "Local G1 tier $configuredG1Count is missing required artifact: $requiredPath"
            }
        }

        & $stepStatsExe $patchPath 1> $stepStatsStdoutPath 2> $stepStatsStderrPath
        $stepStatsExitCode = $LASTEXITCODE
        if ($stepStatsExitCode -ne 0) {
            throw "step_stats failed for local G1 tier $configuredG1Count with exit code $stepStatsExitCode"
        }

        $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
        $configuration = Get-RequiredProperty -Object $report -Name "configuration"
        $constraints = Get-RequiredProperty -Object $report -Name "constraints"
        $sampling = Get-RequiredProperty -Object $report -Name "sampling"
        if ([string]$configuration.sampling_strategy -ne "face_stratified_area_weighted" -or
            [int]$configuration.training_sample_count -ne $trainingSamples -or
            [int]$configuration.selection_sample_count -ne $selectionSamples -or
            [int]$configuration.holdout_sample_count -ne $holdoutSamples -or
            [int]$configuration.configured_local_g1_count -ne $configuredG1Count -or
            [int]$configuration.boundary_samples_per_edge -ne $boundarySamplesPerEdge -or
            [int]$configuration.jacobian_grid_size -ne $jacobianGridSize -or
            [double]$configuration.tolerance3d -ne $tolerance3d -or
            [double]$configuration.normal_tolerance_degrees -ne $normalToleranceDegrees -or
            [int]$configuration.plate_degree -ne 3 -or
            [int]$configuration.points_on_curve -ne 24 -or
            [int]$configuration.iterations -ne 3 -or
            [int]$configuration.max_degree -ne 8 -or
            [int]$configuration.max_segments -ne 12) {
            throw "Local G1 tier $configuredG1Count report does not match the fixed NSR-1.3 configuration."
        }

        $fixedConfiguration = [ordered]@{}
        foreach ($property in $configuration.PSObject.Properties) {
            if ($property.Name -ne "configured_local_g1_count") {
                $fixedConfiguration[$property.Name] = $property.Value
            }
        }
        $currentFixedConfigurationJson = $fixedConfiguration | ConvertTo-Json -Compress
        if ($null -eq $fixedConfigurationJson) {
            $fixedConfigurationJson = $currentFixedConfigurationJson
        } elseif ($currentFixedConfigurationJson -ne $fixedConfigurationJson) {
            throw "Local G1 tier $configuredG1Count changed a non-G1 SurfaceRefitOptions value."
        }

        if (-not [bool]$sampling.sample_sets_disjoint -or
            [int]$sampling.training_selection_overlap_count -ne 0 -or
            [int]$sampling.training_holdout_overlap_count -ne 0 -or
            [int]$sampling.selection_holdout_overlap_count -ne 0) {
            throw "Local G1 tier $configuredG1Count has overlapping training, selection, or holdout samples."
        }
        if ([int]$constraints.configured_g1_count -ne $configuredG1Count -or
            [int]$constraints.actual_g1_count -ne $configuredG1Count -or
            [int]$constraints.g0_training_constraint_count -ne ($trainingSamples - $configuredG1Count) -or
            [int]$constraints.total_training_constraint_count -ne $trainingSamples) {
            throw "Local G1 tier $configuredG1Count has inconsistent configured/actual G1 or total constraint counts."
        }
        if (@($constraints.local_g1_constraints).Count -ne $configuredG1Count) {
            throw "Local G1 tier $configuredG1Count has an incomplete selected-constraint report."
        }
        $selectedTrainingIndices = @($constraints.local_g1_constraints | ForEach-Object { [int]$_.training_index })
        for ($index = 0; $index -lt $previousSelectedTrainingIndices.Count; ++$index) {
            if ($selectedTrainingIndices[$index] -ne $previousSelectedTrainingIndices[$index]) {
                throw "Local G1 tier $configuredG1Count is not nested with the preceding tier."
            }
        }
        $previousSelectedTrainingIndices = $selectedTrainingIndices

        if (-not $report.diagnostics.csv_written -or
            -not $report.diagnostics.obj_written -or
            -not $report.diagnostics.normal_csv_written) {
            throw "Local G1 tier $configuredG1Count did not finish all diagnostic files."
        }
        if (-not $report.brep.step_roundtrip_read_success -or
            -not $report.brep.step_roundtrip_brep_check_performed) {
            throw "Local G1 tier $configuredG1Count did not finish STEP roundtrip diagnostics."
        }
        if ($report.patch_written -and -not $report.patch_acceptable -and -not $report.diagnostic_patch) {
            throw "Local G1 tier $configuredG1Count failed to label its unacceptable patch as diagnostic."
        }

        $perFace = @($sampling.per_face)
        $trainingMinimum = ($perFace | Measure-Object training_count -Minimum).Minimum
        $trainingMaximum = ($perFace | Measure-Object training_count -Maximum).Maximum
        $trainingZeroCount = @($perFace | Where-Object { [int]$_.training_count -eq 0 }).Count
        $failedGates = @($report.failed_gates) -join ";"
        $jacobianPassed =
            [int]$report.jacobian.sample_count -gt 0 -and
            [int]$report.jacobian.near_singular_count -eq 0 -and
            [int]$report.jacobian.flipped_pair_count -eq 0
        $comparisonRows += [pscustomobject]([ordered]@{
            configured_g1_count = $configuredG1Count
            actual_g1_count = [int]$constraints.actual_g1_count
            g0_training_constraints = [int]$constraints.g0_training_constraint_count
            total_training_constraints = [int]$constraints.total_training_constraint_count
            patch_acceptable = [bool]$report.patch_acceptable
            fit_success = [bool]$report.fit_success
            failed_gates = $failedGates
            boundary_max = $report.boundary_error.max; boundary_p95 = $report.boundary_error.p95; boundary_rms = $report.boundary_error.rms; boundary_over_tolerance = $report.boundary_error.over_tolerance_count
            corner_max = $report.corner_error.max; corner_p95 = $report.corner_error.p95; corner_rms = $report.corner_error.rms; corner_over_tolerance = $report.corner_error.over_tolerance_count
            selection_interior_max = $report.interior_selection_error.max; selection_interior_p95 = $report.interior_selection_error.p95; selection_interior_rms = $report.interior_selection_error.rms; selection_interior_over_tolerance = $report.interior_selection_error.over_tolerance_count
            selection_normal_max_degrees = $report.normal_selection_error.max_angle_degrees; selection_normal_p95_degrees = $report.normal_selection_error.p95_angle_degrees; selection_normal_rms_degrees = $report.normal_selection_error.rms_angle_degrees; selection_normal_over_tolerance = $report.normal_selection_error.over_tolerance_count
            holdout_interior_max = $report.interior_holdout_error.max; holdout_interior_p95 = $report.interior_holdout_error.p95; holdout_interior_rms = $report.interior_holdout_error.rms; holdout_interior_over_tolerance = $report.interior_holdout_error.over_tolerance_count
            holdout_normal_max_degrees = $report.normal_holdout_error.max_angle_degrees; holdout_normal_p95_degrees = $report.normal_holdout_error.p95_angle_degrees; holdout_normal_rms_degrees = $report.normal_holdout_error.rms_angle_degrees; holdout_normal_over_tolerance = $report.normal_holdout_error.over_tolerance_count
            per_face_training_min = $trainingMinimum; per_face_training_max = $trainingMaximum; per_face_training_zero_count = $trainingZeroCount
            u_degree = $report.surface_complexity.u_degree; v_degree = $report.surface_complexity.v_degree; total_poles = $report.surface_complexity.total_poles
            u_segments = $report.surface_complexity.u_segments; v_segments = $report.surface_complexity.v_segments; total_segments = $report.surface_complexity.total_segments
            jacobian_passed = $jacobianPassed; jacobian_min_normalized = $report.jacobian.min_normalized; jacobian_near_singular = $report.jacobian.near_singular_count; jacobian_flipped_pairs = $report.jacobian.flipped_pair_count
            filling_face_brep_check_valid = [bool]$report.brep.filling_face_brep_check_valid
            oriented_face_brep_check_valid = [bool]$report.brep.oriented_face_brep_check_valid
            step_roundtrip_brep_check_valid = [bool]$report.brep.step_roundtrip_brep_check_valid
            step_roundtrip_face_count = $report.brep.step_roundtrip_face_count; step_roundtrip_edge_count = $report.brep.step_roundtrip_edge_count
            training_selection_overlap_count = $sampling.training_selection_overlap_count; training_holdout_overlap_count = $sampling.training_holdout_overlap_count; selection_holdout_overlap_count = $sampling.selection_holdout_overlap_count
            duration_ms = $stopwatch.ElapsedMilliseconds; probe_exit_code = $probeExitCode; step_stats_exit_code = $stepStatsExitCode
            run_directory = $runDirectory
            runner_error = ""
        })

        $runStatus = if ($report.patch_acceptable -and $probeExitCode -eq 0) {
            "passed"
        } elseif ($report.patch_written) {
            "failed_quality_gate"
        } else {
            "failed"
        }
        Complete-SpoRun `
            -Context $runContext `
            -Status $runStatus `
            -Message "NSR-1.3 local G1=$configuredG1Count completed; patch_acceptable=$($report.patch_acceptable); failed_gates=$failedGates." `
            -ResolvedCandidateId ([string]$CandidateId) `
            -Artifacts ([ordered]@{
                patch_step = $patchPath
                fit_report = $reportPath
                surface_error_samples_csv = $distanceCsvPath
                surface_error_vectors_obj = $distanceObjPath
                surface_normal_error_samples_csv = $normalCsvPath
                stdout_log = $stdoutPath
                stderr_log = $stderrPath
                step_stats_stdout = $stepStatsStdoutPath
                step_stats_stderr = $stepStatsStderrPath
            }) `
            -Stages ([ordered]@{
                native_surface_fit_probe = [ordered]@{
                    exit_code = $probeExitCode
                    fit_success = [bool]$report.fit_success
                    patch_acceptable = [bool]$report.patch_acceptable
                    failed_gates = @($report.failed_gates)
                    configured_g1_count = $configuredG1Count
                    actual_g1_count = [int]$constraints.actual_g1_count
                    duration_ms = $stopwatch.ElapsedMilliseconds
                }
                step_stats = [ordered]@{
                    exit_code = $stepStatsExitCode
                    face_count = $report.brep.step_roundtrip_face_count
                    edge_count = $report.brep.step_roundtrip_edge_count
                    brep_check_valid = [bool]$report.brep.step_roundtrip_brep_check_valid
                }
            })
    } catch {
        $failureMessage = $_.Exception.Message
        $runnerFailures += "G1=${configuredG1Count}: $failureMessage"
        $comparisonRows += New-FailureComparisonRow `
            -ConfiguredG1Count $configuredG1Count `
            -RunDirectory $runDirectory `
            -ErrorMessage $failureMessage
        if ($null -ne $runContext) {
            Complete-SpoRun `
                -Context $runContext `
                -Status "failed" `
                -Message $failureMessage `
                -ResolvedCandidateId ([string]$CandidateId) `
                -Artifacts ([ordered]@{
                    patch_step = $patchPath
                    fit_report = $reportPath
                    surface_error_samples_csv = $distanceCsvPath
                    surface_error_vectors_obj = $distanceObjPath
                    surface_normal_error_samples_csv = $normalCsvPath
                    stdout_log = $stdoutPath
                    stderr_log = $stderrPath
                    step_stats_stdout = $stepStatsStdoutPath
                    step_stats_stderr = $stepStatsStderrPath
                })
        }
    }
}

$comparisonContext = New-SpoRunContext `
    -RepoRoot $repoRoot `
    -Workflow "native-surface-fit-comparison" `
    -SourcePath $SourceStep `
    -CandidateId ([string]$CandidateId) `
    -Variant "nsr-1.3-local-g1-tiers" `
    -ScriptPath $PSCommandPath `
    -Parameters ([ordered]@{
        tier_run_directories = $tierRunDirectories
        configured_g1_counts = $g1Tiers
        training_samples = $trainingSamples
        selection_samples = $selectionSamples
        holdout_samples = $holdoutSamples
        boundary_samples_per_edge = $boundarySamplesPerEdge
        jacobian_grid_size = $jacobianGridSize
        tolerance3d = $tolerance3d
        normal_tolerance_degrees = $normalToleranceDegrees
        runner_failures = $runnerFailures
    })
$comparisonCsv = Join-Path $comparisonContext.directories.artifacts "native_surface_fit_local_g1_tiers.csv"
$comparisonMarkdown = Join-Path $comparisonContext.directories.artifacts "native_surface_fit_local_g1_tiers.md"
$comparisonRows | Sort-Object configured_g1_count | Export-Csv -LiteralPath $comparisonCsv -NoTypeInformation -Encoding UTF8

$markdown = @(
    "# NSR-1.3 Native Surface Fit Local G1 Tiers",
    "",
    "Training, selection, holdout, boundary, and solver settings are fixed. Only the configured local G1 count changes.",
    "",
    "| G1 configured/actual | acceptable | failed_gates | boundary max/p95/RMS/over | corner max/p95/RMS/over | selection interior max/p95/RMS/over | selection normal max/p95/RMS/over | holdout interior max/p95/RMS/over | holdout normal max/p95/RMS/over | training min/max/zero | degree | poles | segments | Jacobian | BRep filling/oriented/roundtrip | roundtrip faces/edges | overlaps T-S/T-H/S-H | duration ms | runner error |",
    "|---|---:|---|---|---|---|---|---|---|---|---|---:|---|---:|---|---|---|---:|---|"
)
foreach ($row in @($comparisonRows | Sort-Object configured_g1_count)) {
    $markdown += "| $($row.configured_g1_count)/$($row.actual_g1_count) | $($row.patch_acceptable) | $($row.failed_gates) | $($row.boundary_max)/$($row.boundary_p95)/$($row.boundary_rms)/$($row.boundary_over_tolerance) | $($row.corner_max)/$($row.corner_p95)/$($row.corner_rms)/$($row.corner_over_tolerance) | $($row.selection_interior_max)/$($row.selection_interior_p95)/$($row.selection_interior_rms)/$($row.selection_interior_over_tolerance) | $($row.selection_normal_max_degrees)/$($row.selection_normal_p95_degrees)/$($row.selection_normal_rms_degrees)/$($row.selection_normal_over_tolerance) | $($row.holdout_interior_max)/$($row.holdout_interior_p95)/$($row.holdout_interior_rms)/$($row.holdout_interior_over_tolerance) | $($row.holdout_normal_max_degrees)/$($row.holdout_normal_p95_degrees)/$($row.holdout_normal_rms_degrees)/$($row.holdout_normal_over_tolerance) | $($row.per_face_training_min)/$($row.per_face_training_max)/$($row.per_face_training_zero_count) | $($row.u_degree)x$($row.v_degree) | $($row.total_poles) | $($row.u_segments)x$($row.v_segments) ($($row.total_segments)) | $($row.jacobian_passed) | $($row.filling_face_brep_check_valid)/$($row.oriented_face_brep_check_valid)/$($row.step_roundtrip_brep_check_valid) | $($row.step_roundtrip_face_count)/$($row.step_roundtrip_edge_count) | $($row.training_selection_overlap_count)/$($row.training_holdout_overlap_count)/$($row.selection_holdout_overlap_count) | $($row.duration_ms) | $($row.runner_error) |"
}
if ($runnerFailures.Count -gt 0) {
    $markdown += ""
    $markdown += "## Runner failures"
    $markdown += ""
    foreach ($failure in $runnerFailures) {
        $markdown += "- $failure"
    }
}
$markdown | Set-Content -LiteralPath $comparisonMarkdown -Encoding UTF8

$comparisonStatus = if ($runnerFailures.Count -eq 0) { "completed" } else { "failed" }
Complete-SpoRun `
    -Context $comparisonContext `
    -Status $comparisonStatus `
    -Message "Generated the fixed 96-point NSR-1.3 local G1 comparison; runner_failures=$($runnerFailures.Count)." `
    -ResolvedCandidateId ([string]$CandidateId) `
    -Artifacts ([ordered]@{
        comparison_csv = $comparisonCsv
        comparison_markdown = $comparisonMarkdown
        tier_run_directories = $tierRunDirectories
    }) `
    -Stages ([ordered]@{
        comparison = [ordered]@{
            tier_count = $comparisonRows.Count
            runner_failure_count = $runnerFailures.Count
            passed = $runnerFailures.Count -eq 0
        }
    })

Write-Host "Tier run directories:"
$tierRunDirectories | ForEach-Object { Write-Host "  $_" }
Write-Host "Comparison run directory: $($comparisonContext.output_dir)"
Write-Host "Comparison table: $comparisonMarkdown"

if ($runnerFailures.Count -gt 0) {
    throw "NSR-1.3 completed with runner failures; artifacts and comparison were retained: $($runnerFailures -join ' | ')"
}
