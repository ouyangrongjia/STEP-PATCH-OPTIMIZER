param(
    [ValidateSet('FitSet', 'Replay', 'Full', 'RefreshCache')]
    [string]$Tier = 'FitSet',
    [string]$BaselinePath = 'data/runs/20260715_surface_reduction_candidate_0179_hybrid/performance_baseline.json',
    [string]$CacheDirectory = 'data/cache/surface-reduction/v1',
    [string]$OutputDirectory = 'data/runs/20260715_surface_reduction_candidate_0179_hybrid/performance',
    [int]$Workers = 4,
    [int]$MaximumFitEvaluations = 512,
    [int]$MaximumProposalGroupSize = 2
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$baselineFile = Join-Path $repo $BaselinePath
$baseline = Get-Content -LiteralPath $baselineFile -Raw | ConvertFrom-Json
$sourceStep = Join-Path $repo $baseline.source_step
$scanStl = Join-Path $repo $baseline.scan_stl
$cachePath = Join-Path $repo $CacheDirectory
$outputPath = Join-Path $repo $OutputDirectory
$probe = Join-Path $repo 'build/windows-msvc-debug/Debug/surface_reduction_probe.exe'

function Assert-EqualHash([string]$Path, [string]$Expected) {
    $actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    if ($actual -ne $Expected) {
        throw "输入文件哈希不匹配：$Path，预期 $Expected，实际 $actual"
    }
}

function Get-AlgorithmFingerprint {
    $files = @(
        'src/reduction/SurfaceReductionEngine.h',
        'src/reduction/SurfaceReductionEngine.cpp',
        'src/reduction/RestrictedBSplinePatchFitter.h',
        'src/reduction/RestrictedBSplinePatchFitter.cpp',
        'src/reduction/StlHybridParameterization.h',
        'src/reduction/StlHybridParameterization.cpp',
        'src/reduction/SurfaceReductionFitCache.h',
        'src/reduction/SurfaceReductionFitCache.cpp',
        'src/stl/StlRegionIndex.h',
        'src/stl/StlRegionIndex.cpp',
        'src/stl/StlRegionExtractor.h',
        'src/stl/StlRegionExtractor.cpp',
        'CMakeLists.txt'
    )
    $lines = foreach ($file in $files) {
        $path = Join-Path $repo $file
        '{0}:{1}' -f $file, (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    }
    $bytes = [Text.Encoding]::UTF8.GetBytes(($lines -join "`n"))
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([Convert]::ToHexString($sha.ComputeHash($bytes))) }
    finally { $sha.Dispose() }
}

function Invoke-Probe(
    [string]$Name,
    [string]$CacheMode,
    [object[]]$SelectionArguments,
    [double]$TargetRatio,
    [bool]$LocalFitOnly = $false
) {
    $json = Join-Path $outputPath "$Name.json"
    $step = Join-Path $outputPath "$Name.step"
    $log = Join-Path $outputPath "$Name.log"
    $arguments = @(
        $sourceStep,
        $step,
        '--source-stl', $scanStl,
        '--target-ratio', ([string]::Format([Globalization.CultureInfo]::InvariantCulture, '{0}', $TargetRatio)),
        '--workers', $Workers,
        '--maximum-fit-evaluations', $MaximumFitEvaluations,
        '--maximum-proposal-group-size', $MaximumProposalGroupSize,
        '--report-json', $json
    )
    if ($CacheMode -ne 'disabled') {
        $arguments += @(
            '--cache-mode', $CacheMode,
            '--cache-dir', $cachePath,
            '--algorithm-fingerprint', $script:algorithmFingerprint
        )
    }
    if ($LocalFitOnly) { $arguments += '--local-fit-only' }
    $arguments += $SelectionArguments
    $started = [Diagnostics.Stopwatch]::StartNew()
    & $probe @arguments *>&1 | Tee-Object -LiteralPath $log
    $exitCode = $LASTEXITCODE
    $started.Stop()
    if (-not (Test-Path -LiteralPath $json)) {
        throw "probe 未生成结构化报告：$json"
    }
    [pscustomobject]@{
        ExitCode = $exitCode
        ElapsedSeconds = $started.Elapsed.TotalSeconds
        Report = Get-Content -LiteralPath $json -Raw | ConvertFrom-Json
        JsonPath = $json
    }
}

function Normalize-Groups($Groups) {
    @($Groups | ForEach-Object {
        (@($_ | ForEach-Object { [int]$_ }) | Sort-Object) -join '+'
    } | Sort-Object)
}

function Assert-FullBaseline($Run, [double]$MaximumSeconds) {
    if ($Run.ExitCode -ne 0) { throw "完整回归返回 $($Run.ExitCode)：$($Run.Report.message)" }
    if ($Run.Report.input_faces -ne $baseline.selection.input_face_count -or
        $Run.Report.output_faces -ne $baseline.selection.output_face_count) {
        throw "面数基线不一致：$($Run.Report.input_faces)→$($Run.Report.output_faces)"
    }
    $expected = Normalize-Groups $baseline.accepted_groups
    $actual = Normalize-Groups @($Run.Report.output_groups | Where-Object { $_.Count -gt 1 })
    if (($expected -join ',') -ne ($actual -join ',')) {
        throw "接受组基线不一致：预期 $($expected -join ',')，实际 $($actual -join ',')"
    }
    if (-not $Run.Report.all_quality_gates_passed -or -not $Run.Report.brep_check_valid -or
        -not $Run.Report.step_roundtrip_ok -or $Run.Report.after_free_edges -ne 0 -or
        $Run.Report.after_multiple_edges -ne 0) {
        throw '全局拓扑、BRepCheck 或 STEP 回读质量门失败。'
    }
    if ($Run.Report.max_distance -gt $baseline.quality_limits.max_distance_mm -or
        $Run.Report.max_normal_degrees -gt $baseline.quality_limits.max_normal_degrees -or
        $Run.Report.max_smooth_seam_degrees -gt $baseline.quality_limits.max_smooth_seam_degrees) {
        throw '几何质量指标超过 candidate 基线。'
    }
    if ($Run.ElapsedSeconds -gt $MaximumSeconds) {
        throw "性能门失败：$([math]::Round($Run.ElapsedSeconds, 2)) 秒 > $MaximumSeconds 秒"
    }
}

Assert-EqualHash $sourceStep $baseline.source_step_sha256
Assert-EqualHash $scanStl $baseline.scan_stl_sha256
New-Item -ItemType Directory -Force -Path $outputPath | Out-Null
New-Item -ItemType Directory -Force -Path $cachePath | Out-Null
cmake --build --preset windows-msvc-debug --target surface_reduction_probe
if ($LASTEXITCODE -ne 0) { throw 'surface_reduction_probe 构建失败。' }
$script:algorithmFingerprint = Get-AlgorithmFingerprint

switch ($Tier) {
    'FitSet' {
        $selectionArguments = @()
        foreach ($group in $baseline.accepted_groups) {
            $selectionArguments += @('--local-fit-group', ((@($group) | ForEach-Object { [string]$_ }) -join ','))
        }
        foreach ($failure in $baseline.representative_failures) {
            $selectionArguments += @('--local-fit-group', ((@($failure.faces) | ForEach-Object { [string]$_ }) -join ','))
        }
        $run = Invoke-Probe 'fit-set' 'disabled' $selectionArguments 0.5 $true
        if ($run.ExitCode -ne 0) { throw "代表分组批量拟合返回 $($run.ExitCode)。" }
        $expectedAccepted = Normalize-Groups $baseline.accepted_groups
        $actualAccepted = Normalize-Groups $run.Report.output_groups
        if (($expectedAccepted -join ',') -ne ($actualAccepted -join ',')) {
            throw "代表接受组不一致：预期 $($expectedAccepted -join ',')，实际 $($actualAccepted -join ',')"
        }
        foreach ($failure in $baseline.representative_failures) {
            $expectedFaces = ((@($failure.faces) | ForEach-Object { [int]$_ }) | Sort-Object) -join '+'
            $matched = @($run.Report.rejected_groups | Where-Object {
                (((@($_.faces) | ForEach-Object { [int]$_ }) | Sort-Object) -join '+') -eq $expectedFaces
            })
            if ($matched.Count -ne 1 -or $matched[0].stage -ne $failure.stage) {
                throw "代表拒绝组未在预期阶段失败：$expectedFaces/$($failure.stage)"
            }
        }
    }
    'Replay' {
        $run = Invoke-Probe 'replay' 'read-only-required' @('--candidate-id', [string]$baseline.candidate_id) $baseline.selection.target_ratio
        Assert-FullBaseline $run $baseline.quality_limits.max_replay_seconds
        if ($run.Report.persistent_cache_required_miss) { throw '热回放存在必需缓存缺失。' }
    }
    'Full' {
        $run = Invoke-Probe 'full' 'disabled' @('--candidate-id', [string]$baseline.candidate_id) $baseline.selection.target_ratio
        Assert-FullBaseline $run $baseline.quality_limits.max_full_seconds
    }
    'RefreshCache' {
        $run = Invoke-Probe 'refresh-cache' 'refresh' @('--candidate-id', [string]$baseline.candidate_id) $baseline.selection.target_ratio
        Assert-FullBaseline $run $baseline.quality_limits.max_full_seconds
        if ($run.Report.persistent_cache_writes -le 0) { throw '刷新缓存没有写入任何完整局部拟合项。' }
    }
}

Write-Host "分层回归通过：Tier=$Tier，算法指纹=$script:algorithmFingerprint"
