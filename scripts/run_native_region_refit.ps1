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
if ($Layout -eq "fixed" -and $PatchCount -le 0) {
    throw "Fixed layout requires -PatchCount greater than zero."
}
if ([int]$ConstraintsOnly.IsPresent + [int]$SamplingOnly.IsPresent +
        [int]$LayoutOnly.IsPresent + [int]$FitOnly.IsPresent -gt 1) {
    throw "Stage diagnostic switches cannot be used together."
}

$candidateToken = if ($PSCmdlet.ParameterSetName -eq "Candidate") { [string]$CandidateId } else { "faces" }
$variant = if ($ConstraintsOnly) { "constraints" } elseif ($SamplingOnly) { "sampling" } elseif ($LayoutOnly) { "layout" } elseif ($FitOnly) { "fit" } else { $Layout }
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
$baseArguments += @("--layout", $Layout)
if ($PatchCount -gt 0) {
    $baseArguments += @("--patch-count", [string]$PatchCount)
}
if ($LockedEdgeIds.Count -gt 0) {
    $baseArguments += @("--locked-edges", ($LockedEdgeIds -join ","))
}
$baseArguments += @(
    "--wall-time-ms", [string]$WallTimeMilliseconds,
    "--max-memory-mib", [string]$MaximumMemoryMiB)
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
            $performanceRuns += Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json
        }
    }
    $cold = @($performanceRuns | Where-Object run_identity -eq "cold") | Select-Object -First 1
    $warm = @($performanceRuns | Where-Object { $_.run_identity -like "warm-*" })
    $signatures = @($performanceRuns | ForEach-Object {
        "$($_.status)|$($_.selected_layout_hash)|$($_.selected_patch_count)|$($_.geometry_metrics | ConvertTo-Json -Compress)"
    } | Select-Object -Unique)
    $consistencyPassed = $performanceRuns.Count -eq 3 -and $signatures.Count -eq 1
    $coldBudgetPassed = $null -ne $cold -and [double]$cold.core_total_ms -le 900000.0
    $warmBudgetPassed = $warm.Count -eq 2 -and @($warm | Where-Object { [double]$_.core_total_ms -gt 300000.0 }).Count -eq 0
    $memoryBudgetBytes = [uint64]$MaximumMemoryMiB * [uint64]1024 * [uint64]1024
    $memoryPassed = $performanceRuns.Count -eq 3 -and @($performanceRuns | Where-Object {
        [uint64]$_.peak_working_set_bytes -gt $memoryBudgetBytes -or
        [uint64]$_.peak_private_bytes -gt $memoryBudgetBytes
    }).Count -eq 0
    $fitCountPassed = $performanceRuns.Count -eq 3 -and @($performanceRuns | Where-Object {
        [int]$_.full_fit_count -gt 3
    }).Count -eq 0
    $algorithmPassed = $runs.Count -eq 3 -and @($runs | Where-Object exit_code -ne 0).Count -eq 0
    $performancePassed = $algorithmPassed -and $coldBudgetPassed -and $warmBudgetPassed -and
        $memoryPassed -and $fitCountPassed -and $consistencyPassed
    [ordered]@{
        schema_version = 1
        passed = $performancePassed
        cold_definition = "Fresh unique algorithm cache; operating-system file cache is not cleared."
        limits = [ordered]@{
            cold_wall_ms = 900000
            warm_wall_ms = 300000
            memory_bytes = $memoryBudgetBytes
            maximum_full_fits = 3
        }
        gates = [ordered]@{
            algorithm = $algorithmPassed
            cold_wall = $coldBudgetPassed
            warm_wall = $warmBudgetPassed
            memory = $memoryPassed
            full_fit_count = $fitCountPassed
            deterministic_consistency = $consistencyPassed
        }
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
$fittedPatchNetwork = Join-Path $context.directories.artifacts "fit\fitted_patch_network.step"
if (Test-Path -LiteralPath $fittedPatchNetwork -PathType Leaf) {
    $artifacts["fitted_patch_network_step"] = $fittedPatchNetwork
}
$finalSewn = Join-Path $context.directories.artifacts "assembly\final_sewn.step"
if (Test-Path -LiteralPath $finalSewn -PathType Leaf) {
    $artifacts["final_sewn_step"] = $finalSewn
}

$evidenceDefinitions = @(
    [ordered]@{ name = "adaptive_sampling_stl"; path = $samplingStl; stage = "Sampling"; required = $true },
    [ordered]@{ name = "selected_layout_visualization"; path = $layoutSvg; stage = "Layout"; required = $true },
    [ordered]@{ name = "final_sewn_step"; path = $finalSewn; stage = "StepRoundtrip"; required = $true }
)
$evidence = @()
foreach ($definition in $evidenceDefinitions) {
    $exists = Test-Path -LiteralPath $definition.path -PathType Leaf
    $evidence += [ordered]@{
        name = $definition.name
        path = if ($exists) { [System.IO.Path]::GetRelativePath($context.output_dir, $definition.path) } else { "" }
        sha256 = if ($exists) { (Get-FileHash -LiteralPath $definition.path -Algorithm SHA256).Hash } else { "" }
        generated_stage = $definition.stage
        required = $definition.required
        validated = $exists -and ($definition.name -ne "final_sewn_step" -or $primaryRun.exit_code -eq 0)
    }
}
[ordered]@{
    schema_version = 1
    run_id = $context.run_id
    evidence = $evidence
} | ConvertTo-Json -Depth 10 | Set-Content `
    -LiteralPath (Join-Path $context.directories.diagnostics "evidence_manifest.json") `
    -Encoding UTF8

Complete-SpoRun -Context $context -Status $status -Message $message -Artifacts $artifacts
Write-Output $context.output_dir
exit $(if ($status -eq "succeeded") { 0 } else { 1 })
