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
$validationSamples = 192
$boundarySamplesPerEdge = 32
$jacobianGridSize = 16
$tolerance3d = 0.03
$normalToleranceDegrees = 15.0
$strategies = @(
    [pscustomobject]@{ name = "area_weighted"; variant = "nsr-1.2-area-weighted" },
    [pscustomobject]@{ name = "face_stratified_area_weighted"; variant = "nsr-1.2-face-stratified-area-weighted" }
)

$comparisonRows = @()
$strategyRunDirectories = @()
$createdRunDirectories = @()
$fixedConfigurationJson = $null
try {
    foreach ($strategy in $strategies) {
        $parameters = [ordered]@{
            Preset = $Preset
            SourceStep = $SourceStep
            CandidateId = $CandidateId
            SamplingStrategy = $strategy.name
            TrainingSamples = $trainingSamples
            ValidationSamples = $validationSamples
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
            -Variant $strategy.variant `
            -ScriptPath $PSCommandPath `
            -Parameters $parameters
        $createdRunDirectories += $runContext.output_dir
        $strategyRunDirectories += $runContext.output_dir

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

        try {
            $probeArguments = @(
                "--source-step", $SourceStep,
                "--candidate-id", ([string]$CandidateId),
                "--output-patch", $patchPath,
                "--report", $reportPath,
                "--sampling-strategy", $strategy.name,
                "--training-samples", ([string]$trainingSamples),
                "--validation-samples", ([string]$validationSamples),
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
                    throw "Strategy $($strategy.name) is missing required artifact: $requiredPath"
                }
            }

            & $stepStatsExe $patchPath 1> $stepStatsStdoutPath 2> $stepStatsStderrPath
            $stepStatsExitCode = $LASTEXITCODE
            if ($stepStatsExitCode -ne 0) {
                throw "step_stats failed for strategy $($strategy.name) with exit code $stepStatsExitCode"
            }

            $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
            $configuration = Get-RequiredProperty -Object $report -Name "configuration"
            if ([string]$configuration.sampling_strategy -ne $strategy.name -or
                [int]$configuration.training_sample_count -ne $trainingSamples -or
                [int]$configuration.validation_sample_count -ne $validationSamples -or
                [int]$configuration.boundary_samples_per_edge -ne $boundarySamplesPerEdge -or
                [int]$configuration.jacobian_grid_size -ne $jacobianGridSize -or
                [double]$configuration.tolerance3d -ne $tolerance3d -or
                [double]$configuration.normal_tolerance_degrees -ne $normalToleranceDegrees -or
                [int]$configuration.plate_degree -ne 3 -or
                [int]$configuration.points_on_curve -ne 24 -or
                [int]$configuration.iterations -ne 3 -or
                [int]$configuration.max_degree -ne 8 -or
                [int]$configuration.max_segments -ne 12) {
                throw "Strategy $($strategy.name) report does not match the fixed NSR-1.2 configuration."
            }
            $fixedConfiguration = [ordered]@{}
            foreach ($property in $configuration.PSObject.Properties) {
                if ($property.Name -ne "sampling_strategy") {
                    $fixedConfiguration[$property.Name] = $property.Value
                }
            }
            $currentFixedConfigurationJson = $fixedConfiguration | ConvertTo-Json -Compress
            if ($null -eq $fixedConfigurationJson) {
                $fixedConfigurationJson = $currentFixedConfigurationJson
            } elseif ($currentFixedConfigurationJson -ne $fixedConfigurationJson) {
                throw "Strategy $($strategy.name) changed a non-strategy SurfaceRefitOptions value."
            }
            if (-not $report.diagnostics.csv_written -or
                -not $report.diagnostics.obj_written -or
                -not $report.diagnostics.normal_csv_written) {
                throw "Strategy $($strategy.name) did not finish all diagnostic files."
            }
            if (-not $report.brep.step_roundtrip_read_success -or
                -not $report.brep.step_roundtrip_brep_check_performed) {
                throw "Strategy $($strategy.name) did not finish STEP roundtrip diagnostics."
            }
            if ($report.patch_written -and -not $report.patch_acceptable -and -not $report.diagnostic_patch) {
                throw "Strategy $($strategy.name) failed to label its unacceptable patch as diagnostic."
            }

            $perFace = @($report.sampling.per_face)
            $trainingMinimum = ($perFace | Measure-Object training_count -Minimum).Minimum
            $trainingMaximum = ($perFace | Measure-Object training_count -Maximum).Maximum
            $trainingZeroCount = @($perFace | Where-Object { [int]$_.training_count -eq 0 }).Count
            $failedGates = @($report.failed_gates) -join ";"
            $jacobianPassed =
                [int]$report.jacobian.sample_count -gt 0 -and
                [int]$report.jacobian.near_singular_count -eq 0 -and
                [int]$report.jacobian.flipped_pair_count -eq 0
            $comparisonRows += [pscustomobject]([ordered]@{
                strategy = $strategy.name
                success = [bool]$report.patch_acceptable
                fit_success = [bool]$report.fit_success
                failed_gates = $failedGates
                boundary_max = $report.boundary_error.max
                boundary_p95 = $report.boundary_error.p95
                boundary_rms = $report.boundary_error.rms
                boundary_over_tolerance = $report.boundary_error.over_tolerance_count
                corner_max = $report.corner_error.max
                corner_p95 = $report.corner_error.p95
                corner_rms = $report.corner_error.rms
                corner_over_tolerance = $report.corner_error.over_tolerance_count
                interior_max = $report.interior_validation_error.max
                interior_p95 = $report.interior_validation_error.p95
                interior_rms = $report.interior_validation_error.rms
                interior_over_tolerance = $report.interior_validation_error.over_tolerance_count
                normal_max_degrees = $report.normal_validation_error.max_angle_degrees
                normal_p95_degrees = $report.normal_validation_error.p95_angle_degrees
                normal_rms_degrees = $report.normal_validation_error.rms_angle_degrees
                normal_over_tolerance = $report.normal_validation_error.over_tolerance_count
                per_face_training_min = $trainingMinimum
                per_face_training_max = $trainingMaximum
                per_face_training_zero_count = $trainingZeroCount
                u_degree = $report.surface_complexity.u_degree
                v_degree = $report.surface_complexity.v_degree
                total_poles = $report.surface_complexity.total_poles
                u_segments = $report.surface_complexity.u_segments
                v_segments = $report.surface_complexity.v_segments
                total_segments = $report.surface_complexity.total_segments
                jacobian_passed = $jacobianPassed
                jacobian_min_normalized = $report.jacobian.min_normalized
                jacobian_near_singular = $report.jacobian.near_singular_count
                jacobian_flipped_pairs = $report.jacobian.flipped_pair_count
                filling_face_brep_check_valid = [bool]$report.brep.filling_face_brep_check_valid
                oriented_face_brep_check_valid = [bool]$report.brep.oriented_face_brep_check_valid
                step_roundtrip_brep_check_valid = [bool]$report.brep.step_roundtrip_brep_check_valid
                step_roundtrip_face_count = $report.brep.step_roundtrip_face_count
                step_roundtrip_edge_count = $report.brep.step_roundtrip_edge_count
                duration_ms = $stopwatch.ElapsedMilliseconds
                probe_exit_code = $probeExitCode
                step_stats_exit_code = $stepStatsExitCode
                run_directory = $runContext.output_dir
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
                -Message "NSR-1.2 $($strategy.name) completed; patch_acceptable=$($report.patch_acceptable); failed_gates=$failedGates." `
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
            Complete-SpoRun `
                -Context $runContext `
                -Status "failed" `
                -Message $_.Exception.Message `
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
            throw
        }
    }

    $comparisonContext = New-SpoRunContext `
        -RepoRoot $repoRoot `
        -Workflow "native-surface-fit-comparison" `
        -SourcePath $SourceStep `
        -CandidateId ([string]$CandidateId) `
        -Variant "nsr-1.2-sampling-ab" `
        -ScriptPath $PSCommandPath `
        -Parameters ([ordered]@{
            strategy_run_directories = $strategyRunDirectories
            training_samples = $trainingSamples
            validation_samples = $validationSamples
            boundary_samples_per_edge = $boundarySamplesPerEdge
            jacobian_grid_size = $jacobianGridSize
            tolerance3d = $tolerance3d
            normal_tolerance_degrees = $normalToleranceDegrees
        })
    $createdRunDirectories += $comparisonContext.output_dir
    $comparisonCsv = Join-Path $comparisonContext.directories.artifacts "native_surface_fit_sampling_ab.csv"
    $comparisonMarkdown = Join-Path $comparisonContext.directories.artifacts "native_surface_fit_sampling_ab.md"
    $comparisonRows | Export-Csv -LiteralPath $comparisonCsv -NoTypeInformation -Encoding UTF8

    $markdown = @(
        "# NSR-1.2 Native Surface Fit Sampling A/B",
        "",
        "Only the deterministic training sampling allocation strategy changes; the total training count and all solver/validation settings remain fixed.",
        "",
        "| strategy | acceptable | failed_gates | boundary max/p95/RMS/over | corner max/p95/RMS/over | interior max/p95/RMS/over | normal max/p95/RMS/over | training min/max/zero | degree | poles | segments | Jacobian | BRep filling/oriented/roundtrip | roundtrip faces/edges | duration ms |",
        "|---|---:|---|---|---|---|---|---|---|---:|---|---:|---|---|---:|"
    )
    foreach ($row in $comparisonRows) {
        $markdown += "| $($row.strategy) | $($row.success) | $($row.failed_gates) | $($row.boundary_max)/$($row.boundary_p95)/$($row.boundary_rms)/$($row.boundary_over_tolerance) | $($row.corner_max)/$($row.corner_p95)/$($row.corner_rms)/$($row.corner_over_tolerance) | $($row.interior_max)/$($row.interior_p95)/$($row.interior_rms)/$($row.interior_over_tolerance) | $($row.normal_max_degrees)/$($row.normal_p95_degrees)/$($row.normal_rms_degrees)/$($row.normal_over_tolerance) | $($row.per_face_training_min)/$($row.per_face_training_max)/$($row.per_face_training_zero_count) | $($row.u_degree)x$($row.v_degree) | $($row.total_poles) | $($row.u_segments)x$($row.v_segments) ($($row.total_segments)) | $($row.jacobian_passed) | $($row.filling_face_brep_check_valid)/$($row.oriented_face_brep_check_valid)/$($row.step_roundtrip_brep_check_valid) | $($row.step_roundtrip_face_count)/$($row.step_roundtrip_edge_count) | $($row.duration_ms) |"
    }
    $markdown | Set-Content -LiteralPath $comparisonMarkdown -Encoding UTF8

    Complete-SpoRun `
        -Context $comparisonContext `
        -Status "completed" `
        -Message "Generated the fixed 96-point NSR-1.2 sampling allocation comparison." `
        -ResolvedCandidateId ([string]$CandidateId) `
        -Artifacts ([ordered]@{
            comparison_csv = $comparisonCsv
            comparison_markdown = $comparisonMarkdown
            strategy_run_directories = $strategyRunDirectories
        }) `
        -Stages ([ordered]@{ comparison = [ordered]@{ strategy_count = $comparisonRows.Count; passed = $true } })

    Write-Host "Strategy run directories:"
    $strategyRunDirectories | ForEach-Object { Write-Host "  $_" }
    Write-Host "Comparison run directory: $($comparisonContext.output_dir)"
    Write-Host "Comparison table: $comparisonMarkdown"
} catch {
    $runsRoot = [System.IO.Path]::GetFullPath((Join-Path $repoRoot "data\runs"))
    $runsRootPrefix = $runsRoot + [System.IO.Path]::DirectorySeparatorChar
    $resolvedRunDirectories = foreach ($runDirectory in @($createdRunDirectories | Select-Object -Unique)) {
        $resolvedRunDirectory = [System.IO.Path]::GetFullPath($runDirectory)
        if (-not $resolvedRunDirectory.StartsWith(
            $runsRootPrefix,
            [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to remove an incomplete run outside data/runs: $resolvedRunDirectory"
        }
        $resolvedRunDirectory
    }
    foreach ($resolvedRunDirectory in $resolvedRunDirectories) {
        if (Test-Path -LiteralPath $resolvedRunDirectory -PathType Container) {
            Remove-Item -LiteralPath $resolvedRunDirectory -Recurse -Force
        }
    }
    throw
}
