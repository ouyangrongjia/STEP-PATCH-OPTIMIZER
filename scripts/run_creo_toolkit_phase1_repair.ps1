param(
    [string]$Preset = "windows-msvc-debug",
    [ValidateSet("A0", "B1", "B2", "B2.0", "B2.1", "B2.2", "B2.3")]
    [string]$Experiment = "A0",
    [string]$SourceStep = "",
    [string]$CandidateId = "auto",
    [string]$CreoRoot = "",
    [string]$OutputDir = "",
    [string]$Configuration = "Release",
    [string]$WrapCore = "E:\Geomagic Wrap\wrapCore.exe",
    [int]$TimeoutSeconds = 1800,
    [int]$CreoTimeoutSeconds = 900,
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

function Find-DefaultSourceStep {
    $roots = @(
        (Join-Path $repoRoot "data\stp"),
        (Join-Path $repoRoot "data\samples")
    )

    $candidates = @()
    foreach ($root in $roots) {
        if (-not (Test-Path -LiteralPath $root)) {
            continue
        }

        $candidates += Get-ChildItem -LiteralPath $root -Recurse -File |
            Where-Object {
                $extension = $_.Extension.ToLowerInvariant()
                $extension -eq ".stp" -or $extension -eq ".step"
            }
    }

    $candidate = $candidates |
        Sort-Object FullName |
        Select-Object -First 1

    if ($null -eq $candidate) {
        return ""
    }

    return $candidate.FullName
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

function Invoke-NativeCommand {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory
    )

    Write-Host ""
    Write-Host "==> $FilePath $($Arguments -join ' ')"
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

    New-Item -ItemType Directory -Force -Path $WorkingDirectory | Out-Null
    $stdoutPath = Join-Path $WorkingDirectory "$LogPrefix.stdout.txt"
    $stderrPath = Join-Path $WorkingDirectory "$LogPrefix.stderr.txt"
    if (Test-Path -LiteralPath $stdoutPath) {
        Remove-Item -LiteralPath $stdoutPath -Force
    }
    if (Test-Path -LiteralPath $stderrPath) {
        Remove-Item -LiteralPath $stderrPath -Force
    }

    $argumentLine = ($Arguments | ForEach-Object {
            $argument = [string]$_
            if ($argument -match '[\s"]') {
                '"' + ($argument -replace '"', '\"') + '"'
            } else {
                $argument
            }
        }) -join " "

    $process = Start-Process `
        -FilePath $FilePath `
        -ArgumentList $argumentLine `
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
        throw "$LogPrefix timed out after $Timeout seconds. Stdout: $stdoutPath Stderr: $stderrPath"
    }
    $process.Refresh()
    $exitCode = $process.ExitCode
    if ($null -eq $exitCode) {
        $exitCode = 0
    }

    return [ordered]@{
        exit_code = $exitCode
        stdout_path = $stdoutPath
        stderr_path = $stderrPath
    }
}

function Find-BuiltExe {
    param([Parameter(Mandatory = $true)][string]$ExeName)

    $candidates = @(
        (Join-Path $repoRoot "build\$Preset\Debug\$ExeName"),
        (Join-Path $repoRoot "build\$Preset\$ExeName")
    )

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }

    $found = Get-ChildItem -LiteralPath (Join-Path $repoRoot "build\$Preset") -Recurse -File -Filter $ExeName -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($found) {
        return $found.FullName
    }

    throw "$ExeName was not found under build\$Preset after build."
}

function Find-CreoToolkitProbeExe {
    param(
        [Parameter(Mandatory = $true)][string]$BuildDir,
        [Parameter(Mandatory = $true)][string]$ConfigurationName
    )

    $exe = Get-ChildItem -LiteralPath $BuildDir -Recurse -File -Filter "creo_toolkit_phase1_repair_probe.exe" |
        Where-Object { $_.FullName -match [regex]::Escape($ConfigurationName) -or $ConfigurationName -eq "" } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if (-not $exe) {
        $exe = Get-ChildItem -LiteralPath $BuildDir -Recurse -File -Filter "creo_toolkit_phase1_repair_probe.exe" |
            Sort-Object LastWriteTime -Descending |
            Select-Object -First 1
    }
    if (-not $exe) {
        throw "creo_toolkit_phase1_repair_probe.exe was not built under $BuildDir"
    }
    return $exe.FullName
}

function Find-ExportedStep {
    param([Parameter(Mandatory = $true)][string]$ExportStepBase)

    $direct = @("$ExportStepBase.stp", "$ExportStepBase.step")
    foreach ($candidate in $direct) {
        if (Test-Path -LiteralPath $candidate) {
            return [System.IO.Path]::GetFullPath($candidate)
        }
    }

    $parent = Split-Path -Parent $ExportStepBase
    $leaf = Split-Path -Leaf $ExportStepBase
    $found = Get-ChildItem -LiteralPath $parent -File -ErrorAction SilentlyContinue |
        Where-Object {
            ($_.Extension.ToLowerInvariant() -eq ".stp" -or $_.Extension.ToLowerInvariant() -eq ".step") -and
            $_.BaseName.StartsWith($leaf, [System.StringComparison]::OrdinalIgnoreCase)
        } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1

    if ($found) {
        return $found.FullName
    }

    return ""
}

function Get-FileEvidence {
    param([string]$PathValue)

    if (-not $PathValue -or -not (Test-Path -LiteralPath $PathValue)) {
        return [ordered]@{
            path = $PathValue
            exists = $false
        }
    }

    $item = Get-Item -LiteralPath $PathValue
    return [ordered]@{
        path = $item.FullName
        exists = $true
        length = $item.Length
        last_write_time = $item.LastWriteTime.ToString("o")
        sha256 = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash
    }
}

function Convert-StepStatsTextToObject {
    param([string]$Text)

    $result = [ordered]@{}
    foreach ($line in ($Text -split "`r?`n")) {
        if ($line -match "^([A-Za-z_]+):\s*(.*)$") {
            $key = $matches[1]
            $value = $matches[2].Trim()
            $number = 0
            if ([int]::TryParse($value, [ref]$number)) {
                $result[$key] = $number
            } else {
                $result[$key] = $value
            }
        }
    }
    return $result
}

function Get-ModelCheckMetrics {
    param([string]$DiagnosticJsonPath)

    if (-not $DiagnosticJsonPath -or -not (Test-Path -LiteralPath $DiagnosticJsonPath)) {
        return [ordered]@{
            parsed = $false
        }
    }

    $json = Get-Content -LiteralPath $DiagnosticJsonPath -Raw | ConvertFrom-Json
    $shortEdges = $null
    $geomChecks = $null
    if ($json.key_checks) {
        $shortEdges = $json.key_checks | Where-Object { $_.name -eq "SHORT_EDGES" } | Select-Object -First 1
        $geomChecks = $json.key_checks | Where-Object { $_.name -eq "GEOM_CHECKS" } | Select-Object -First 1
    }

    $partStatus = $null
    $impScore = $null
    if ($json.import_validation) {
        $partStatus = $json.import_validation.PTC_VAL_IMP_PART_STATUS
        $impScore = $json.import_validation.PTC_VAL_IMP_SCORE
    }

    return [ordered]@{
        parsed = $true
        diagnostic_passed = $json.diagnostic_passed
        error_count = $json.error_count
        warning_count = $json.warning_count
        part_status = $partStatus
        import_score = $impScore
        geom_checks_status = if ($geomChecks) { $geomChecks.status } else { $null }
        short_edges_status = if ($shortEdges) { $shortEdges.status } else { $null }
        short_edge_item_count = if ($shortEdges -and $shortEdges.PSObject.Properties.Name -contains "item_count") { $shortEdges.item_count } else { $null }
    }
}

if (-not $SourceStep) {
    $SourceStep = Find-DefaultSourceStep
    if (-not $SourceStep) {
        throw "No STEP/STP sample found under data\stp or data\samples. Pass -SourceStep."
    }
} else {
    $SourceStep = Convert-ToRepoAbsolutePath -PathValue $SourceStep
}

if (-not (Test-Path -LiteralPath $SourceStep)) {
    throw "SourceStep does not exist: $SourceStep"
}

if (-not $OutputDir) {
    $OutputDir = Join-Path $repoRoot ("data\baseline_runs\creo_toolkit_phase1_repair\" + (Get-Date -Format "yyyyMMdd_HHmmss"))
} else {
    $OutputDir = Convert-ToRepoAbsolutePath -PathValue $OutputDir
}

$resolvedCreoRoot = Find-CreoToolkitRoot -PreferredRoot $CreoRoot
$geomagicRunDir = Join-Path $OutputDir "geomagic_generation"
$inputsDir = Join-Path $OutputDir "phase1_inputs"
$creoRunDir = Join-Path $OutputDir "creo_toolkit"
$occtRunDir = Join-Path $OutputDir "occt_validation"
$modelcheckParseDir = Join-Path $OutputDir "modelcheck_parse"
foreach ($dir in @($OutputDir, $geomagicRunDir, $inputsDir, $creoRunDir, $occtRunDir, $modelcheckParseDir)) {
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
}

$vcpkgRoot = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { Join-Path $env:USERPROFILE "vcpkg" }
$env:VCPKG_ROOT = $vcpkgRoot
$env:Path = "C:\Program Files\CMake\bin;$vcpkgRoot;" + $env:Path

Invoke-NativeCommand -FilePath "cmake" -Arguments @("--build", "--preset", $Preset, "--target", "corner_baseline_probe") -WorkingDirectory $repoRoot
Invoke-NativeCommand -FilePath "cmake" -Arguments @("--build", "--preset", $Preset, "--target", "creo_phase1_input_exporter") -WorkingDirectory $repoRoot
Invoke-NativeCommand -FilePath "cmake" -Arguments @("--build", "--preset", $Preset, "--target", "step_stats") -WorkingDirectory $repoRoot
Invoke-NativeCommand -FilePath "cmake" -Arguments @("--build", "--preset", $Preset, "--target", "strict_topology_gate_probe") -WorkingDirectory $repoRoot

$toolkitSourceDir = Join-Path $repoRoot "tools\creo_toolkit_phase1_repair_probe"
$toolkitBuildDir = Join-Path $repoRoot "build\creo_toolkit_phase1_repair_probe"
Invoke-NativeCommand -FilePath "cmake" -Arguments @("-S", $toolkitSourceDir, "-B", $toolkitBuildDir, "-DCREO_ROOT=$resolvedCreoRoot") -WorkingDirectory $repoRoot
Invoke-NativeCommand -FilePath "cmake" -Arguments @("--build", $toolkitBuildDir, "--config", $Configuration) -WorkingDirectory $repoRoot

if ($BuildOnly -or $SkipRun) {
    Write-Host "Creo Toolkit phase1 probe built under: $toolkitBuildDir"
    Write-Host "Output directory prepared: $OutputDir"
    exit 0
}

if (-not (Test-Path -LiteralPath $WrapCore)) {
    throw "wrapCore was not found: $WrapCore"
}

$gateScript = Join-Path $PSScriptRoot "run_corner_baseline_gate.ps1"
$baselineReport = Join-Path $geomagicRunDir "baseline_report.json"
$baselineArgs = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", $gateScript,
    "-Preset", $Preset,
    "-Experiment", $Experiment,
    "-SourceStep", $SourceStep,
    "-CandidateId", $CandidateId,
    "-OutputDir", $geomagicRunDir,
    "-Report", $baselineReport,
    "-WrapCore", $WrapCore,
    "-TimeoutSeconds", "$TimeoutSeconds",
    "-RealGeomagic",
    "-AllowQualityGateFailure"
)

$geomagicGenerationRun = Invoke-ProcessWithTimeout `
    -FilePath "powershell" `
    -Arguments $baselineArgs `
    -WorkingDirectory $repoRoot `
    -LogPrefix "phase1_geomagic_generation" `
    -Timeout ($TimeoutSeconds + 300)

if (-not (Test-Path -LiteralPath $baselineReport)) {
    throw "Fresh Geomagic generation did not produce baseline report: $baselineReport"
}

$baseline = Get-Content -LiteralPath $baselineReport -Raw | ConvertFrom-Json
$resolvedCandidateId = [int]$baseline.candidate_id
$freshPatchPath = ([string]$baseline.patch_step_path.path).Trim()
if ((-not $freshPatchPath -or -not (Test-Path -LiteralPath $freshPatchPath)) -and $baseline.geomagic.output_step_path) {
    $freshPatchPath = ([string]$baseline.geomagic.output_step_path).Trim()
}
if ($freshPatchPath) {
    $freshPatchPath = [System.IO.Path]::GetFullPath($freshPatchPath)
}
if (-not $freshPatchPath -or -not (Test-Path -LiteralPath $freshPatchPath)) {
    $sourceStem = [System.IO.Path]::GetFileNameWithoutExtension($SourceStep)
    $candidatePadded = "{0:D4}" -f $resolvedCandidateId
    $candidatePatch = Join-Path $repoRoot "data\crop_stp\$sourceStem\${sourceStem}_candidate_${candidatePadded}.stp"
    if (Test-Path -LiteralPath $candidatePatch) {
        $freshPatchPath = [System.IO.Path]::GetFullPath($candidatePatch)
    }
}
if (-not $freshPatchPath -or -not (Test-Path -LiteralPath $freshPatchPath)) {
    throw "Fresh Geomagic generation did not produce patch_step_path. Baseline exit code: $($geomagicGenerationRun.exit_code). Report: $baselineReport"
}

$geomagicPatch = Join-Path $inputsDir "geomagic_patch.stp"
Copy-Item -LiteralPath $freshPatchPath -Destination $geomagicPatch -Force

$baseRemoved = Join-Path $inputsDir "base_removed_candidate.stp"
$baseRemovedManifest = Join-Path $inputsDir "base_removed_candidate_manifest.json"
$inputExporterExe = Find-BuiltExe -ExeName "creo_phase1_input_exporter.exe"
Invoke-NativeCommand `
    -FilePath $inputExporterExe `
    -Arguments @(
        "--source-step", $SourceStep,
        "--candidate-id", "$resolvedCandidateId",
        "--base-removed-output", $baseRemoved,
        "--manifest", $baseRemovedManifest
    ) `
    -WorkingDirectory $repoRoot

$commonFiles = Join-Path $resolvedCreoRoot "Common Files"
$parametricBat = Join-Path $resolvedCreoRoot "Parametric\bin\parametric.bat"
$creoCommand = "`"$parametricBat`" -g:no_graphics -i:rpc_input"
$env:PRO_COMM_MSG_EXE = Join-Path $commonFiles "x86e_win64\obj\pro_comm_msg.exe"
$env:PATH = "$(Join-Path $commonFiles 'bin');$(Join-Path $commonFiles 'x86e_win64\lib');$(Join-Path $commonFiles 'protoolkit\x86e_win64\obj');$env:PATH"

$toolkitProbeExe = Find-CreoToolkitProbeExe -BuildDir $toolkitBuildDir -ConfigurationName $Configuration
$modelcheckOutputDir = Join-Path $creoRunDir "modelcheck"
New-Item -ItemType Directory -Force -Path $modelcheckOutputDir | Out-Null
$toolkitResultPath = Join-Path $creoRunDir "creo_toolkit_phase1_result.json"
$exportStepBase = Join-Path $creoRunDir "creo_phase1_merged_export"
$probeArgs = @(
    "--base-removed-step", $baseRemoved,
    "--geomagic-patch-step", $geomagicPatch,
    "--creo-command", $creoCommand,
    "--output-dir", $creoRunDir,
    "--modelcheck-output-dir", $modelcheckOutputDir,
    "--export-step-base", $exportStepBase,
    "--result", $toolkitResultPath,
    "--model-name", "spo_phase1_base"
)

$creoRun = Invoke-ProcessWithTimeout `
    -FilePath $toolkitProbeExe `
    -Arguments $probeArgs `
    -WorkingDirectory $creoRunDir `
    -LogPrefix "creo_toolkit_phase1" `
    -Timeout $CreoTimeoutSeconds

$exportedStep = Find-ExportedStep -ExportStepBase $exportStepBase

$modelcheckXml = Get-ChildItem -LiteralPath $modelcheckOutputDir -Recurse -File -Filter "*.xml" -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
$modelcheckParseRun = $null
$modelcheckDiagnosticJson = Join-Path $modelcheckParseDir "creo_step_diagnostic_result.json"
if ($modelcheckXml) {
    $modelcheckParseRun = Invoke-ProcessWithTimeout `
        -FilePath "powershell" `
        -Arguments @(
            "-NoProfile",
            "-ExecutionPolicy", "Bypass",
            "-File", (Join-Path $PSScriptRoot "run_creo_step_diagnostic.ps1"),
            "-ParseModelCheckOnly",
            "-ModelCheckXmlPath", $modelcheckXml.FullName,
            "-OutputDir", $modelcheckParseDir
        ) `
        -WorkingDirectory $repoRoot `
        -LogPrefix "modelcheck_parse" `
        -Timeout 300
}

$stepStatsRun = $null
$stepStats = [ordered]@{ success = $false }
$strictGateRun = $null
$strictGateReportPath = Join-Path $occtRunDir "strict_topology_gate_result.json"
$strictGate = $null
if ($exportedStep) {
    $stepStatsExe = Find-BuiltExe -ExeName "step_stats.exe"
    $stepStatsRun = Invoke-ProcessWithTimeout `
        -FilePath $stepStatsExe `
        -Arguments @($exportedStep) `
        -WorkingDirectory $occtRunDir `
        -LogPrefix "step_stats" `
        -Timeout 300
    $stepStatsText = Get-Content -LiteralPath $stepStatsRun.stdout_path -Raw
    $stepStats = Convert-StepStatsTextToObject -Text $stepStatsText

    $strictProbeExe = Find-BuiltExe -ExeName "strict_topology_gate_probe.exe"
    $strictGateRun = Invoke-ProcessWithTimeout `
        -FilePath $strictProbeExe `
        -Arguments @(
            "--before-step", $SourceStep,
            "--after-step", $exportedStep,
            "--temporary-step", (Join-Path $occtRunDir "strict_topology_gate_roundtrip.stp"),
            "--report", $strictGateReportPath
        ) `
        -WorkingDirectory $occtRunDir `
        -LogPrefix "StrictTopologyGate" `
        -Timeout 300
    if (Test-Path -LiteralPath $strictGateReportPath) {
        $strictGate = Get-Content -LiteralPath $strictGateReportPath -Raw | ConvertFrom-Json
    }
}

$toolkitResult = if (Test-Path -LiteralPath $toolkitResultPath) {
    Get-Content -LiteralPath $toolkitResultPath -Raw | ConvertFrom-Json
} else {
    $null
}
$modelcheckMetrics = Get-ModelCheckMetrics -DiagnosticJsonPath $modelcheckDiagnosticJson
$strictGatePassed = $false
if ($strictGate) {
    $strictGatePassed = [bool]$strictGate.passed
}

$stepStatsSuccess = $false
if ($stepStats.Contains("success")) {
    $stepStatsSuccess = ($stepStats["success"] -eq 1 -or $stepStats["success"] -eq "1" -or $stepStats["success"] -eq $true)
}
$stepStatsSolidCount = if ($stepStats.Contains("solids")) { [int]$stepStats["solids"] } else { 0 }
$overallPassed = ($creoRun.exit_code -eq 0) -and
    $exportedStep -and
    $stepStatsSuccess -and
    ($stepStatsSolidCount -gt 0) -and
    $strictGatePassed

$summaryPath = Join-Path $OutputDir "phase1_result.json"
$summary = [ordered]@{
    tool = "run_creo_toolkit_phase1_repair"
    status = if ($overallPassed) { "Phase1Passed" } else { "Phase1Failed" }
    overall_passed = [bool]$overallPassed
    created_at = (Get-Date).ToString("o")
    note = "This run forces fresh Geomagic generation and does not use a previously merged STEP as phase 1 input."
    flow = @(
        "Build corner_baseline_probe, creo_phase1_input_exporter, step_stats, strict_topology_gate_probe, and Creo Toolkit phase1 probe.",
        "Run run_corner_baseline_gate.ps1 -RealGeomagic to create a fresh Geomagic patch.",
        "Copy the fresh patch to phase1_inputs\geomagic_patch.stp.",
        "Export phase1_inputs\base_removed_candidate.stp by deleting the same candidate faces from the source STEP.",
        "Import base_removed_candidate.stp in Creo, create an import feature from geomagic_patch.stp with ProImportfeatAttr.join_surfaces=1 and attempt_make_solid=1.",
        "Regenerate, save, run ProModelcheckExecute, export STEP with ProIntf3DFileWriteWithDefaultProfile(PRO_INTF_EXPORT_STEP).",
        "Parse ModelCHECK XML, run OCCT step_stats, and run StrictTopologyGate with watertight/roundtrip requirements."
    )
    source_files = [ordered]@{
        source_step = Get-FileEvidence -PathValue $SourceStep
        fresh_geomagic_patch_source = Get-FileEvidence -PathValue $freshPatchPath
        geomagic_patch_input = Get-FileEvidence -PathValue $geomagicPatch
        base_removed_candidate_input = Get-FileEvidence -PathValue $baseRemoved
        exported_creo_step = Get-FileEvidence -PathValue $exportedStep
    }
    parameters = [ordered]@{
        preset = $Preset
        experiment = $Experiment
        requested_candidate_id = $CandidateId
        resolved_candidate_id = $resolvedCandidateId
        creo_root = $resolvedCreoRoot
        wrap_core = $WrapCore
    }
    run_artifacts = [ordered]@{
        output_dir = $OutputDir
        baseline_report = $baselineReport
        base_removed_manifest = $baseRemovedManifest
        toolkit_result = $toolkitResultPath
        modelcheck_xml = if ($modelcheckXml) { $modelcheckXml.FullName } else { "" }
        modelcheck_parse_result = if (Test-Path -LiteralPath $modelcheckDiagnosticJson) { $modelcheckDiagnosticJson } else { "" }
        step_stats_stdout = if ($stepStatsRun) { $stepStatsRun.stdout_path } else { "" }
        strict_topology_gate_report = if (Test-Path -LiteralPath $strictGateReportPath) { $strictGateReportPath } else { "" }
    }
    exit_codes = [ordered]@{
        geomagic_generation = $geomagicGenerationRun.exit_code
        creo_toolkit_phase1 = $creoRun.exit_code
        modelcheck_parse = if ($modelcheckParseRun) { $modelcheckParseRun.exit_code } else { $null }
        step_stats = if ($stepStatsRun) { $stepStatsRun.exit_code } else { $null }
        strict_topology_gate = if ($strictGateRun) { $strictGateRun.exit_code } else { $null }
    }
    metrics = [ordered]@{
        baseline_stage = $baseline.stage
        baseline_success = $baseline.success
        toolkit_stage1_acceptance_passed = if ($toolkitResult) { $toolkitResult.stage1_toolkit_acceptance_passed } else { $false }
        toolkit_modelcheck_errors = if ($toolkitResult) { $toolkitResult.modelcheck_errors } else { $null }
        toolkit_modelcheck_warnings = if ($toolkitResult) { $toolkitResult.modelcheck_warnings } else { $null }
        toolkit_import_validation_score = if ($toolkitResult) { $toolkitResult.import_validation_score } else { "" }
        modelcheck = $modelcheckMetrics
        step_stats = $stepStats
        strict_topology_gate = $strictGate
    }
}

$summary | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $summaryPath -Encoding UTF8

Write-Host ""
Write-Host "Creo Toolkit phase 1 result: $summaryPath"
Write-Host "Overall passed: $overallPassed"
Write-Host "Candidate id: $resolvedCandidateId"
Write-Host "Base removed candidate: $baseRemoved"
Write-Host "Geomagic patch input: $geomagicPatch"
Write-Host "Creo exported STEP: $exportedStep"
Write-Host "StrictTopologyGate report: $strictGateReportPath"

if (-not $overallPassed) {
    exit 20
}
