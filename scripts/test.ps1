param(
    [string]$Preset = "windows-msvc-debug",
    [string]$OutputDir = ""
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot
. (Join-Path $PSScriptRoot "run_artifacts.ps1")

$runContext = New-SpoRunContext `
    -RepoRoot $repoRoot `
    -Workflow "unit-tests" `
    -CandidateId "na" `
    -Variant $Preset `
    -OutputDir $OutputDir `
    -ScriptPath $PSCommandPath `
    -Parameters (Get-SpoEffectiveScriptParameters -ParameterNames $MyInvocation.MyCommand.Parameters.Keys)
$OutputDir = $runContext.output_dir
$buildLog = Join-Path $runContext.directories.logs "build.log"
$ctestLog = Join-Path $runContext.directories.logs "ctest.log"

function Invoke-LoggedNative {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$LogPath
    )

    & $FilePath @Arguments 2>&1 |
        Tee-Object -FilePath $LogPath |
        ForEach-Object { Write-Host $_ }
    return $LASTEXITCODE
}

$vcpkgRoot = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { Join-Path $env:USERPROFILE "vcpkg" }
$env:VCPKG_ROOT = $vcpkgRoot
$env:Path = "C:\Program Files\CMake\bin;$vcpkgRoot;" + $env:Path
$buildExitCode = Invoke-LoggedNative `
    -FilePath "cmake" `
    -Arguments @("--build", "--preset", $Preset, "--target", "spo_tests") `
    -LogPath $buildLog
if ($buildExitCode -ne 0) {
    Complete-SpoRun `
        -Context $runContext `
        -Status "failed" `
        -Message "spo_tests build failed with exit code $buildExitCode." `
        -Artifacts ([ordered]@{ build_log = $buildLog }) `
        -Stages ([ordered]@{ build = [ordered]@{ exit_code = $buildExitCode; passed = $false } })
    exit $buildExitCode
}

$ctestExitCode = Invoke-LoggedNative `
    -FilePath "ctest" `
    -Arguments @("--preset", $Preset, "--output-on-failure", "--timeout", "300") `
    -LogPath $ctestLog
$stages = [ordered]@{
    build = [ordered]@{ exit_code = $buildExitCode; passed = $true }
    ctest = [ordered]@{ exit_code = $ctestExitCode; passed = ($ctestExitCode -eq 0) }
}
$artifacts = [ordered]@{
    build_log = $buildLog
    ctest_log = $ctestLog
}
if ($ctestExitCode -ne 0) {
    Complete-SpoRun `
        -Context $runContext `
        -Status "failed" `
        -Message "ctest failed with exit code $ctestExitCode." `
        -Artifacts $artifacts `
        -Stages $stages
    exit $ctestExitCode
}

Complete-SpoRun `
    -Context $runContext `
    -Status "passed" `
    -Message "spo_tests build and ctest passed." `
    -Artifacts $artifacts `
    -Stages $stages

Write-Host "Test run artifacts: $OutputDir"
