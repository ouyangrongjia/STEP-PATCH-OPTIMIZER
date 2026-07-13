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

function Find-NativeSurfaceFitProbeExe {
    $candidates = @(
        (Join-Path $repoRoot "build\$Preset\Debug\native_surface_fit_probe.exe"),
        (Join-Path $repoRoot "build\$Preset\native_surface_fit_probe.exe")
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }
    throw "native_surface_fit_probe.exe was not found under build\$Preset after build."
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

& cmake --build --preset $Preset --target native_surface_fit_probe
if ($LASTEXITCODE -ne 0) {
    throw "native_surface_fit_probe build failed with exit code $LASTEXITCODE"
}
$probeExe = Find-NativeSurfaceFitProbeExe

$validationSamples = 192
$boundarySamplesPerEdge = 32
$jacobianGridSize = 16
$tolerance3d = 0.03
$normalToleranceDegrees = 15.0
$tiers = @(
    [pscustomobject]@{ name = "sparse"; training = 24 },
    [pscustomobject]@{ name = "medium"; training = 48 },
    [pscustomobject]@{ name = "dense"; training = 96 }
)

$comparisonRows = @()
$tierRunDirectories = @()
$createdRunDirectories = @()
$fixedConfigurationJson = $null
try {
foreach ($tier in $tiers) {
    $parameters = [ordered]@{
        Preset = $Preset
        SourceStep = $SourceStep
        CandidateId = $CandidateId
        Tier = $tier.name
        TrainingSamples = $tier.training
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
        -Variant "nsr-1.1-$($tier.name)" `
        -ScriptPath $PSCommandPath `
        -Parameters $parameters
    $createdRunDirectories += $runContext.output_dir
    $tierRunDirectories += $runContext.output_dir

    $candidateToken = "{0:D4}" -f $CandidateId
    $patchPath = Join-Path $runContext.directories.artifacts "candidate_${candidateToken}_native_surface_patch.stp"
    $reportPath = Join-Path $runContext.output_dir "native_surface_fit_report.json"
    $csvPath = Join-Path $runContext.directories.diagnostics "native_surface_fit\surface_error_samples.csv"
    $objPath = Join-Path $runContext.directories.diagnostics "native_surface_fit\surface_error_vectors.obj"
    $stdoutPath = Join-Path $runContext.directories.logs "native_surface_fit_probe.stdout.txt"
    $stderrPath = Join-Path $runContext.directories.logs "native_surface_fit_probe.stderr.txt"

    try {
        $probeArguments = @(
            "--source-step", $SourceStep,
            "--candidate-id", ([string]$CandidateId),
            "--output-patch", $patchPath,
            "--report", $reportPath,
            "--training-samples", ([string]$tier.training),
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

        foreach ($requiredPath in @($reportPath, $patchPath, $csvPath, $objPath, $stdoutPath, $stderrPath)) {
            if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
                throw "Tier $($tier.name) is missing required artifact: $requiredPath"
            }
        }

        $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
        $configuration = Get-RequiredProperty -Object $report -Name "configuration"
        if ([int]$configuration.training_sample_count -ne $tier.training -or
            [int]$configuration.validation_sample_count -ne $validationSamples -or
            [int]$configuration.boundary_samples_per_edge -ne $boundarySamplesPerEdge -or
            [int]$configuration.jacobian_grid_size -ne $jacobianGridSize -or
            [double]$configuration.tolerance3d -ne $tolerance3d -or
            [double]$configuration.normal_tolerance_degrees -ne $normalToleranceDegrees) {
            throw "Tier $($tier.name) report does not match the fixed NSR-1.1 experiment configuration."
        }
        $fixedConfiguration = [ordered]@{}
        foreach ($property in $configuration.PSObject.Properties) {
            if ($property.Name -ne "training_sample_count") {
                $fixedConfiguration[$property.Name] = $property.Value
            }
        }
        $currentFixedConfigurationJson = $fixedConfiguration | ConvertTo-Json -Compress
        if ($null -eq $fixedConfigurationJson) {
            $fixedConfigurationJson = $currentFixedConfigurationJson
        } elseif ($currentFixedConfigurationJson -ne $fixedConfigurationJson) {
            throw "Tier $($tier.name) changed a non-training solver configuration value."
        }
        if (-not $report.diagnostics.csv_written -or -not $report.diagnostics.obj_written) {
            throw "Tier $($tier.name) did not finish both diagnostic files."
        }
        if ($report.patch_written -and -not $report.fit_success -and -not $report.diagnostic_patch) {
            throw "Tier $($tier.name) failed to label its failed-fit patch as diagnostic."
        }

        $failedGates = @($report.failed_gates) -join ";"
        $jacobianPassed =
            [int]$report.jacobian.sample_count -gt 0 -and
            [int]$report.jacobian.near_singular_count -eq 0 -and
            [int]$report.jacobian.flipped_pair_count -eq 0
        $comparisonRows += [pscustomobject]([ordered]@{
            tier = $tier.name
            training_samples = $tier.training
            success = [bool]$report.fit_success
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
            brep_check_valid = [bool]$report.brep.brep_check_valid
            duration_ms = $stopwatch.ElapsedMilliseconds
            probe_exit_code = $probeExitCode
            run_directory = $runContext.output_dir
        })

        $runStatus = if ($probeExitCode -eq 0 -and $report.fit_success) {
            "passed"
        } elseif ($report.patch_written -and -not $report.fit_success) {
            "failed_quality_gate"
        } else {
            "failed"
        }
        Complete-SpoRun `
            -Context $runContext `
            -Status $runStatus `
            -Message "NSR-1.1 $($tier.name) tier completed; fit_success=$($report.fit_success); failed_gates=$failedGates." `
            -ResolvedCandidateId ([string]$CandidateId) `
            -Artifacts ([ordered]@{
                patch_step = $patchPath
                fit_report = $reportPath
                surface_error_samples_csv = $csvPath
                surface_error_vectors_obj = $objPath
                stdout_log = $stdoutPath
                stderr_log = $stderrPath
            }) `
            -Stages ([ordered]@{
                native_surface_fit_probe = [ordered]@{
                    exit_code = $probeExitCode
                    fit_success = [bool]$report.fit_success
                    failed_gates = @($report.failed_gates)
                    duration_ms = $stopwatch.ElapsedMilliseconds
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
                surface_error_samples_csv = $csvPath
                surface_error_vectors_obj = $objPath
                stdout_log = $stdoutPath
                stderr_log = $stderrPath
            })
        throw
    }
}

$comparisonContext = New-SpoRunContext `
    -RepoRoot $repoRoot `
    -Workflow "native-surface-fit-comparison" `
    -SourcePath $SourceStep `
    -CandidateId ([string]$CandidateId) `
    -Variant "nsr-1.1-three-tier" `
    -ScriptPath $PSCommandPath `
    -Parameters ([ordered]@{
        tier_run_directories = $tierRunDirectories
        validation_samples = $validationSamples
        boundary_samples_per_edge = $boundarySamplesPerEdge
        jacobian_grid_size = $jacobianGridSize
        tolerance3d = $tolerance3d
        normal_tolerance_degrees = $normalToleranceDegrees
    })
$createdRunDirectories += $comparisonContext.output_dir
$comparisonCsv = Join-Path $comparisonContext.directories.artifacts "native_surface_fit_tier_comparison.csv"
$comparisonMarkdown = Join-Path $comparisonContext.directories.artifacts "native_surface_fit_tier_comparison.md"
$comparisonRows | Export-Csv -LiteralPath $comparisonCsv -NoTypeInformation -Encoding UTF8

$markdown = @(
    "# NSR-1.1 Native Surface Fit Tier Comparison",
    "",
    "Only training sample count changes across tiers.",
    "",
    "| tier | success | failed_gates | boundary max / p95 / RMS / over | corner max / p95 / RMS / over | interior max / p95 / RMS / over | normal max / p95 | degree | poles | segments | Jacobian | BRepCheck | duration ms |",
    "|---|---:|---|---|---|---|---|---|---:|---|---|---:|---:|"
)
foreach ($row in $comparisonRows) {
    $markdown += "| $($row.tier) | $($row.success) | $($row.failed_gates) | $($row.boundary_max) / $($row.boundary_p95) / $($row.boundary_rms) / $($row.boundary_over_tolerance) | $($row.corner_max) / $($row.corner_p95) / $($row.corner_rms) / $($row.corner_over_tolerance) | $($row.interior_max) / $($row.interior_p95) / $($row.interior_rms) / $($row.interior_over_tolerance) | $($row.normal_max_degrees) / $($row.normal_p95_degrees) | $($row.u_degree)x$($row.v_degree) | $($row.total_poles) | $($row.u_segments)x$($row.v_segments) ($($row.total_segments)) | $($row.jacobian_passed) | $($row.brep_check_valid) | $($row.duration_ms) |"
}
$markdown | Set-Content -LiteralPath $comparisonMarkdown -Encoding UTF8

Complete-SpoRun `
    -Context $comparisonContext `
    -Status "completed" `
    -Message "Generated the fixed sparse/medium/dense NSR-1.1 comparison." `
    -ResolvedCandidateId ([string]$CandidateId) `
    -Artifacts ([ordered]@{
        comparison_csv = $comparisonCsv
        comparison_markdown = $comparisonMarkdown
        tier_run_directories = $tierRunDirectories
    }) `
    -Stages ([ordered]@{ comparison = [ordered]@{ tier_count = $comparisonRows.Count; passed = $true } })

Write-Host "Tier run directories:"
$tierRunDirectories | ForEach-Object { Write-Host "  $_" }
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
