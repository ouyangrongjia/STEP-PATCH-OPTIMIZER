param(
    [string]$OutputCsv = "data/runs/native_region_refit_scale_benchmark.csv",
    [long]$SingleFitBudgetMilliseconds = 180000
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$outputPath = if ([System.IO.Path]::IsPathRooted($OutputCsv)) {
    $OutputCsv
} else {
    Join-Path $repoRoot $OutputCsv
}

Push-Location $repoRoot
try {
    cmake --build --preset windows-msvc-debug --target native_region_refit_benchmark --parallel 8
    if ($LASTEXITCODE -ne 0) { throw "原生区域重拟合规模基准构建失败。" }
    $executable = Join-Path $repoRoot "build/windows-msvc-debug/Debug/native_region_refit_benchmark.exe"
    & $executable $outputPath $SingleFitBudgetMilliseconds
    if ($LASTEXITCODE -ne 0) { throw "原生区域重拟合规模基准运行失败。" }
    Write-Output $outputPath
} finally {
    Pop-Location
}
