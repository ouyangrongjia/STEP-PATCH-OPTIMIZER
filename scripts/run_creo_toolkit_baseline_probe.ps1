param(
    [string]$StepPath = "",
    [switch]$UseLatestAppliedStep,
    [string]$CreoRoot = "",
    [string]$OutputDir = "",
    [string]$Configuration = "Release",
    [int]$TimeoutSeconds = 900,
    [switch]$BuildOnly,
    [switch]$SkipRun
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

function Convert-ToRepoAbsolutePath {
    param([Parameter(Mandatory = $true)][string]$PathValue)

    if ([System.IO.Path]::IsPathRooted($PathValue)) {
        return [System.IO.Path]::GetFullPath($PathValue)
    }

    return [System.IO.Path]::GetFullPath((Join-Path $repoRoot $PathValue))
}

function Test-CreoToolkitRoot {
    param([Parameter(Mandatory = $true)][string]$CandidateRoot)

    if (-not $CandidateRoot) {
        return $false
    }

    $root = [System.IO.Path]::GetFullPath($CandidateRoot)
    $commonFiles = Join-Path $root "Common Files"
    $parametricBat = Join-Path $root "Parametric\bin\parametric.bat"
    $proToolkitHeader = Join-Path $commonFiles "protoolkit\includes\ProToolkit.h"
    $proToolkitLib = Join-Path $commonFiles "protoolkit\x86e_win64\obj\protkmd_NU.lib"
    $proToolkitFallbackLib = Join-Path $commonFiles "protoolkit\x86e_win64\obj\protoolkit_NU.lib"
    $proCommMsg = Join-Path $commonFiles "x86e_win64\obj\pro_comm_msg.exe"

    return (Test-Path -LiteralPath $parametricBat) -and
        (Test-Path -LiteralPath $proToolkitHeader) -and
        ((Test-Path -LiteralPath $proToolkitLib) -or (Test-Path -LiteralPath $proToolkitFallbackLib)) -and
        (Test-Path -LiteralPath $proCommMsg)
}

function Normalize-CreoRootCandidate {
    param([Parameter(Mandatory = $true)][string]$CandidateRoot)

    if (-not $CandidateRoot) {
        return ""
    }

    $full = [System.IO.Path]::GetFullPath($CandidateRoot)
    if ((Split-Path -Leaf $full) -eq "Common Files") {
        return [System.IO.Path]::GetFullPath((Split-Path -Parent $full))
    }

    return $full
}

function Find-CreoToolkitRoot {
    param([string]$PreferredRoot)

    $candidates = New-Object System.Collections.Generic.List[string]
    foreach ($value in @($PreferredRoot, $env:CREO_ROOT, $env:CREO_DIRECTORY)) {
        if ($value) {
            $candidates.Add((Normalize-CreoRootCandidate -CandidateRoot $value))
        }
    }

    foreach ($base in @("E:\Proe", "E:\Preo", "C:\Program Files\PTC", "C:\Program Files\PTC\Creo")) {
        if (-not (Test-Path -LiteralPath $base)) {
            continue
        }

        $candidates.Add((Normalize-CreoRootCandidate -CandidateRoot $base))
        Get-ChildItem -LiteralPath $base -Directory -ErrorAction SilentlyContinue |
            ForEach-Object {
                $candidates.Add((Normalize-CreoRootCandidate -CandidateRoot $_.FullName))
                Get-ChildItem -LiteralPath $_.FullName -Directory -ErrorAction SilentlyContinue |
                    ForEach-Object {
                        $candidates.Add((Normalize-CreoRootCandidate -CandidateRoot $_.FullName))
                    }
            }
    }

    foreach ($candidate in ($candidates | Where-Object { $_ } | Select-Object -Unique)) {
        if (Test-CreoToolkitRoot -CandidateRoot $candidate) {
            return $candidate
        }
    }

    throw "Creo Pro/TOOLKIT root was not found. Pass -CreoRoot or set CREO_ROOT/CREO_DIRECTORY."
}

function Find-LatestAppliedStep {
    $root = Join-Path $repoRoot "data\baseline_runs"
    if (-not (Test-Path -LiteralPath $root)) {
        throw "No baseline_runs directory exists. Pass -StepPath explicitly."
    }

    $latest = Get-ChildItem -LiteralPath $root -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like "*_applied.stp" -or $_.Name -like "*_applied.step" } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1

    if (-not $latest) {
        throw "No *_applied.stp or *_applied.step file was found under data\baseline_runs. Pass -StepPath explicitly."
    }

    return $latest.FullName
}

function Invoke-NativeCommand {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code $LASTEXITCODE`: $FilePath $($Arguments -join ' ')"
    }
}

function Invoke-ProcessWithTimeout {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][string]$LogPrefix,
        [Parameter(Mandatory = $true)][int]$Timeout
    )

    $stdoutPath = Join-Path $WorkingDirectory "$LogPrefix.stdout.txt"
    $stderrPath = Join-Path $WorkingDirectory "$LogPrefix.stderr.txt"
    if (Test-Path -LiteralPath $stdoutPath) {
        Remove-Item -LiteralPath $stdoutPath -Force
    }
    if (Test-Path -LiteralPath $stderrPath) {
        Remove-Item -LiteralPath $stderrPath -Force
    }

    $process = Start-Process `
        -FilePath $FilePath `
        -ArgumentList $Arguments `
        -WorkingDirectory $WorkingDirectory `
        -RedirectStandardOutput $stdoutPath `
        -RedirectStandardError $stderrPath `
        -WindowStyle Hidden `
        -PassThru

    $completed = $process.WaitForExit($Timeout * 1000)
    if (-not $completed) {
        try {
            $process.Kill()
        } catch {
        }
        throw "Creo Toolkit baseline probe timed out after $Timeout seconds. Stdout: $stdoutPath Stderr: $stderrPath"
    }

    return [ordered]@{
        exit_code = $process.ExitCode
        stdout_path = $stdoutPath
        stderr_path = $stderrPath
    }
}

$resolvedCreoRoot = Find-CreoToolkitRoot -PreferredRoot $CreoRoot

if (-not $StepPath) {
    if (-not $UseLatestAppliedStep) {
        $UseLatestAppliedStep = $true
    }
    $StepPath = Find-LatestAppliedStep
} else {
    $StepPath = Convert-ToRepoAbsolutePath -PathValue $StepPath
}

if (-not (Test-Path -LiteralPath $StepPath)) {
    throw "StepPath does not exist: $StepPath"
}

if (-not $OutputDir) {
    $OutputDir = Join-Path $repoRoot "data\baseline_runs\creo_toolkit_baseline_probe"
} else {
    $OutputDir = Convert-ToRepoAbsolutePath -PathValue $OutputDir
}

$sourceDir = Join-Path $repoRoot "tools\creo_toolkit_baseline_probe"
$buildDir = Join-Path $repoRoot "build\creo_toolkit_baseline_probe"
$stageDir = Join-Path $OutputDir "stage"
$modelcheckOutputDir = Join-Path $OutputDir "modelcheck"
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
New-Item -ItemType Directory -Force -Path $stageDir | Out-Null
New-Item -ItemType Directory -Force -Path $modelcheckOutputDir | Out-Null

$stagedStep = Join-Path $stageDir ("input" + [System.IO.Path]::GetExtension($StepPath).ToLowerInvariant())
Copy-Item -LiteralPath $StepPath -Destination $stagedStep -Force

Invoke-NativeCommand `
    -FilePath "cmake" `
    -Arguments @("-S", $sourceDir, "-B", $buildDir, "-DCREO_ROOT=$resolvedCreoRoot") `
    -WorkingDirectory $repoRoot
Invoke-NativeCommand `
    -FilePath "cmake" `
    -Arguments @("--build", $buildDir, "--config", $Configuration) `
    -WorkingDirectory $repoRoot

$exe = Get-ChildItem -LiteralPath $buildDir -Recurse -File -Filter "creo_toolkit_baseline_probe.exe" |
    Where-Object { $_.FullName -match [regex]::Escape($Configuration) -or $Configuration -eq "" } |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if (-not $exe) {
    $exe = Get-ChildItem -LiteralPath $buildDir -Recurse -File -Filter "creo_toolkit_baseline_probe.exe" |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
}
if (-not $exe) {
    throw "creo_toolkit_baseline_probe.exe was not built under $buildDir"
}

$resultPath = Join-Path $OutputDir "creo_toolkit_baseline_result.json"
$exportStepBase = Join-Path $OutputDir "creo_toolkit_import_export"

if ($BuildOnly -or $SkipRun) {
    Write-Host "Creo Toolkit baseline probe built: $($exe.FullName)"
    Write-Host "Staged STEP: $stagedStep"
    exit 0
}

$commonFiles = Join-Path $resolvedCreoRoot "Common Files"
$parametricBat = Join-Path $resolvedCreoRoot "Parametric\bin\parametric.bat"
$creoCommand = "`"$parametricBat`" -g:no_graphics -i:rpc_input"
$textPath = ""
$env:PRO_COMM_MSG_EXE = Join-Path $commonFiles "x86e_win64\obj\pro_comm_msg.exe"
$env:PATH = "$(Join-Path $commonFiles 'bin');$(Join-Path $commonFiles 'x86e_win64\lib');$(Join-Path $commonFiles 'protoolkit\x86e_win64\obj');$env:PATH"

$probeArgs = @(
    "--step", $stagedStep,
    "--creo-command", $creoCommand,
    "--output-dir", $OutputDir,
    "--modelcheck-output-dir", $modelcheckOutputDir,
    "--export-step-base", $exportStepBase,
    "--result", $resultPath,
    "--model-name", "spo_tk_baseline"
)
if ($textPath) {
    $probeArgs += @("--text-path", $textPath)
}

$run = Invoke-ProcessWithTimeout `
    -FilePath $exe.FullName `
    -Arguments $probeArgs `
    -WorkingDirectory $OutputDir `
    -LogPrefix "creo_toolkit_baseline" `
    -Timeout $TimeoutSeconds

Write-Host "Creo Toolkit baseline probe exit code: $($run.exit_code)"
Write-Host "Result JSON: $resultPath"
Write-Host "Stdout: $($run.stdout_path)"
Write-Host "Stderr: $($run.stderr_path)"

if ($run.exit_code -ne 0) {
    exit $run.exit_code
}
