param(
    [string]$Preset = "windows-msvc-debug",
    [string]$SourceStep = "",
    [string]$CandidateId = "auto",
    [string]$OutputDir = "",
    [string]$WrapCore = "E:\Geomagic Wrap\wrapCore.exe",
    [string]$CreoRoot = "",
    [int]$TimeoutSeconds = 1800,
    [int]$CreoTimeoutSeconds = 900,
    [double]$G1Tolerance3d = 0.03,
    [double]$G1ToleranceAngle = 0.01,
    [int]$CornerFeatureSamples = 96,
    [int]$CandidateOverCoverSamples = 64,
    [double]$CandidateOverCoverWidth = 0.25,
    [int]$CandidateOverCoverRings = 3,
    [double]$CandidateOverCoverMiterMaxScale = 1.25,
    [switch]$EnableAuxiliarySupportCollar,
    [int]$SupportCollarSamples = 64,
    [double]$SupportCollarWidth = 0.05,
    [int]$SupportCollarRings = 1,
    [double]$SupportCollarUnderCover = 0.0,
    [switch]$SkipCreo
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

function Convert-ArgumentsToLine {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)

    return (($Arguments | ForEach-Object {
                $argument = [string]$_
                if ($argument -match '[\s"]') {
                    '"' + ($argument -replace '"', '\"') + '"'
                } else {
                    $argument
                }
            }) -join " ")
}

function Find-DefaultSourceStep {
    $roots = @(
        (Join-Path $repoRoot "data\stp"),
        (Join-Path $repoRoot "data\samples")
    )
    foreach ($root in $roots) {
        if (-not (Test-Path -LiteralPath $root)) {
            continue
        }
        $candidate = Get-ChildItem -LiteralPath $root -Recurse -File |
            Where-Object {
                $extension = $_.Extension.ToLowerInvariant()
                $extension -eq ".stp" -or $extension -eq ".step"
            } |
            Sort-Object FullName |
            Select-Object -First 1
        if ($candidate) {
            return $candidate.FullName
        }
    }

    return ""
}

function Find-BuiltExe {
    param([Parameter(Mandatory = $true)][string]$ExeName)

    $candidates = @(
        (Join-Path $repoRoot "build\$Preset\Debug\$ExeName"),
        (Join-Path $repoRoot "build\$Preset\$ExeName")
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) {
            return [System.IO.Path]::GetFullPath($candidate)
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

function Invoke-BuildTarget {
    param([Parameter(Mandatory = $true)][string]$Target)

    Write-Host ""
    Write-Host "==> cmake --build --preset $Preset --target $Target -- /m:1"
    & cmake --build --preset $Preset --target $Target -- /m:1
    if ($LASTEXITCODE -ne 0) {
        throw "Build target failed: $Target"
    }
}

function Invoke-LoggedProcess {
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

    $startedAt = Get-Date
    $argumentLine = Convert-ArgumentsToLine -Arguments $Arguments
    Write-Host ""
    Write-Host "==> $FilePath $argumentLine"
    $process = Start-Process `
        -FilePath $FilePath `
        -ArgumentList $argumentLine `
        -WorkingDirectory $WorkingDirectory `
        -RedirectStandardOutput $stdoutPath `
        -RedirectStandardError $stderrPath `
        -WindowStyle Hidden `
        -PassThru

    $completed = $process.WaitForExit($Timeout * 1000)
    $timedOut = -not $completed
    if ($timedOut) {
        try {
            $process.Kill()
        } catch {
        }
    }
    $process.Refresh()
    $exitCode = $null
    if (-not $timedOut) {
        try {
            $process.WaitForExit()
            $exitCode = $process.ExitCode
        } catch {
            $exitCode = $null
        }
        if ($null -eq $exitCode) {
            $exitCode = 0
        }
    }

    return [ordered]@{
        file_path = $FilePath
        arguments = @($Arguments)
        working_directory = $WorkingDirectory
        started_at = $startedAt.ToString("o")
        ended_at = (Get-Date).ToString("o")
        timeout_seconds = $Timeout
        timed_out = $timedOut
        exit_code = $exitCode
        stdout_path = $stdoutPath
        stderr_path = $stderrPath
    }
}

function Read-JsonFile {
    param([string]$PathValue)

    if (-not $PathValue -or -not (Test-Path -LiteralPath $PathValue)) {
        return $null
    }

    return [System.IO.File]::ReadAllText($PathValue, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
}

function Get-FileSha256 {
    param([Parameter(Mandatory = $true)][string]$PathValue)

    if (Get-Command Get-FileHash -ErrorAction SilentlyContinue) {
        return (Get-FileHash -LiteralPath $PathValue -Algorithm SHA256).Hash
    }

    $stream = [System.IO.File]::OpenRead($PathValue)
    try {
        $sha256 = [System.Security.Cryptography.SHA256]::Create()
        try {
            $hash = $sha256.ComputeHash($stream)
            return (($hash | ForEach-Object { $_.ToString("x2") }) -join "").ToUpperInvariant()
        } finally {
            if ($sha256) {
                $sha256.Dispose()
            }
        }
    } finally {
        $stream.Dispose()
    }
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
        sha256 = Get-FileSha256 -PathValue $item.FullName
    }
}

function Get-CreoVariantExportedStep {
    param($Variant)

    if ($Variant -and $Variant.artifacts -and $Variant.artifacts.exported_step) {
        return [string]$Variant.artifacts.exported_step
    }

    return ""
}

function Get-ObjectValue {
    param(
        $Object,
        [Parameter(Mandatory = $true)][string]$Name
    )

    if ($null -eq $Object) {
        return $null
    }
    $property = $Object.PSObject.Properties[$Name]
    if ($property) {
        return $property.Value
    }
    return $null
}

function Convert-StepStatsTextToObject {
    param([string]$Text)

    $result = [ordered]@{}
    foreach ($line in ($Text -split "`r?`n")) {
        if ($line -match "^([A-Za-z_]+):\s*(.*)$") {
            $key = $Matches[1]
            $value = $Matches[2].Trim()
            $intValue = 0
            if ([int]::TryParse($value, [ref]$intValue)) {
                $result[$key] = $intValue
            } else {
                $result[$key] = $value
            }
        }
    }
    return $result
}

function Resolve-PathFromJsonValue {
    param([string]$PathValue)

    if (-not $PathValue) {
        return ""
    }

    $trimmed = $PathValue.Trim()
    if (-not $trimmed) {
        return ""
    }
    if ([System.IO.Path]::IsPathRooted($trimmed)) {
        return [System.IO.Path]::GetFullPath($trimmed)
    }

    return [System.IO.Path]::GetFullPath((Join-Path $repoRoot $trimmed))
}

function Get-PatchStepFromBaseline {
    param($Baseline)

    $path = ""
    if ($Baseline -and $Baseline.patch_step_path) {
        $path = Resolve-PathFromJsonValue -PathValue ([string]$Baseline.patch_step_path.path)
    }
    if ((-not $path -or -not (Test-Path -LiteralPath $path)) -and $Baseline -and $Baseline.geomagic) {
        $path = Resolve-PathFromJsonValue -PathValue ([string]$Baseline.geomagic.output_step_path)
    }
    return $path
}

function Get-AppliedStepFromBaseline {
    param($Baseline)

    if ($Baseline -and $Baseline.applied_step_export) {
        return Resolve-PathFromJsonValue -PathValue ([string]$Baseline.applied_step_export.path)
    }
    return ""
}

function Get-KeyCheck {
    param(
        $Summary,
        [Parameter(Mandatory = $true)][string]$Name
    )

    if ($null -eq $Summary) {
        return $null
    }
    foreach ($groupName in @("key_checks", "failed_checks", "warning_checks")) {
        foreach ($check in @((Get-ObjectValue -Object $Summary -Name $groupName))) {
            if (([string](Get-ObjectValue -Object $check -Name "name")) -eq $Name) {
                return $check
            }
        }
    }
    return $null
}

function Get-CreoMetrics {
    param([string]$DiagnosticJsonPath)

    $diagnostic = Read-JsonFile -PathValue $DiagnosticJsonPath
    if ($null -eq $diagnostic) {
        return [ordered]@{
            parsed = $false
        }
    }

    $summary = $diagnostic.modelcheck.summary
    $shortEdges = Get-KeyCheck -Summary $summary -Name "SHORT_EDGES"
    $geomChecks = Get-KeyCheck -Summary $summary -Name "GEOM_CHECKS"
    $importFeat = Get-KeyCheck -Summary $summary -Name "IMPORT_FEAT"
    $importValidation = $summary.import_validation

    return [ordered]@{
        parsed = $true
        status = $diagnostic.status
        runner_success = $diagnostic.success
        diagnostic_passed = $diagnostic.modelcheck.diagnostic_passed
        error_count = $summary.error_count
        warning_count = $summary.warning_count
        import_score = if ($importValidation) { $importValidation.PTC_VAL_IMP_SCORE } else { $null }
        part_status = if ($importValidation) { $importValidation.PTC_VAL_IMP_PART_STATUS } else { $null }
        geom_checks_status = if ($geomChecks) { $geomChecks.status } else { $null }
        geom_checks_item_count = if ($geomChecks) { $geomChecks.item_count } else { $null }
        short_edges_status = if ($shortEdges) { $shortEdges.status } else { $null }
        short_edges_item_count = if ($shortEdges) { $shortEdges.item_count } else { $null }
        import_feat_status = if ($importFeat) { $importFeat.status } else { $null }
        result_json = $DiagnosticJsonPath
    }
}

function Invoke-StepStats {
    param(
        [Parameter(Mandatory = $true)][string]$StepPath,
        [Parameter(Mandatory = $true)][string]$OutputDir,
        [Parameter(Mandatory = $true)][string]$Prefix
    )

    $exe = Find-BuiltExe -ExeName "step_stats.exe"
    $run = Invoke-LoggedProcess `
        -FilePath $exe `
        -Arguments @($StepPath) `
        -WorkingDirectory $OutputDir `
        -LogPrefix $Prefix `
        -Timeout 300
    $stats = [ordered]@{}
    if (Test-Path -LiteralPath $run.stdout_path) {
        $stats = Convert-StepStatsTextToObject -Text (Get-Content -LiteralPath $run.stdout_path -Raw)
    }
    return [ordered]@{
        run = $run
        stats = $stats
    }
}

function Invoke-StrictGate {
    param(
        [Parameter(Mandatory = $true)][string]$BeforeStep,
        [Parameter(Mandatory = $true)][string]$AfterStep,
        [Parameter(Mandatory = $true)][string]$OutputDir,
        [Parameter(Mandatory = $true)][string]$Prefix
    )

    $exe = Find-BuiltExe -ExeName "strict_topology_gate_probe.exe"
    $reportPath = Join-Path $OutputDir "$Prefix.json"
    $run = Invoke-LoggedProcess `
        -FilePath $exe `
        -Arguments @(
            "--before-step", $BeforeStep,
            "--after-step", $AfterStep,
            "--temporary-step", (Join-Path $OutputDir "$Prefix.roundtrip.stp"),
            "--report", $reportPath
        ) `
        -WorkingDirectory $OutputDir `
        -LogPrefix $Prefix `
        -Timeout 300
    return [ordered]@{
        run = $run
        report = Read-JsonFile -PathValue $reportPath
        report_path = $reportPath
    }
}

function Invoke-CreoDiagnostic {
    param(
        [Parameter(Mandatory = $true)][string]$StepPath,
        [Parameter(Mandatory = $true)][string]$OutputDir,
        [string]$BaselineReportPath = ""
    )

    if ($SkipCreo) {
        return [ordered]@{
            skipped = $true
            reason = "SkipCreo"
        }
    }

    $args = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", (Join-Path $PSScriptRoot "run_creo_step_diagnostic.ps1"),
        "-StepPath", $StepPath,
        "-OutputDir", $OutputDir,
        "-TimeoutSeconds", "$CreoTimeoutSeconds"
    )
    if ($CreoRoot) {
        $args += @("-CreoRoot", $CreoRoot)
    }
    if ($BaselineReportPath) {
        $args += @("-BaselineReportPath", $BaselineReportPath)
    }

    $run = Invoke-LoggedProcess `
        -FilePath "powershell" `
        -Arguments $args `
        -WorkingDirectory $repoRoot `
        -LogPrefix "creo_step_diagnostic" `
        -Timeout ($CreoTimeoutSeconds + 120)
    $jsonPath = Join-Path $OutputDir "creo_step_diagnostic_result.json"
    return [ordered]@{
        skipped = $false
        run = $run
        result_json = $jsonPath
        metrics = Get-CreoMetrics -DiagnosticJsonPath $jsonPath
    }
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

function Find-CreoToolkitProbeExe {
    param(
        [Parameter(Mandatory = $true)][string]$BuildDir
    )

    $exe = Get-ChildItem -LiteralPath $BuildDir -Recurse -File -Filter "creo_toolkit_phase1_repair_probe.exe" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if (-not $exe) {
        throw "creo_toolkit_phase1_repair_probe.exe was not built under $BuildDir"
    }
    return $exe.FullName
}

function Find-ExportedStep {
    param([Parameter(Mandatory = $true)][string]$ExportStepBase)

    foreach ($candidate in @("$ExportStepBase.stp", "$ExportStepBase.step")) {
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

function Invoke-ModelcheckParseOnly {
    param(
        [Parameter(Mandatory = $true)][string]$ModelCheckDir,
        [Parameter(Mandatory = $true)][string]$OutputDir,
        [string]$BaselineReportPath = ""
    )

    New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

    $xml = Get-ChildItem -LiteralPath $ModelCheckDir -Recurse -File -Filter "*.xml" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if (-not $xml) {
        return [ordered]@{
            parsed = $false
            skipped_reason = "No ModelCHECK XML was produced."
        }
    }

    $args = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", (Join-Path $PSScriptRoot "run_creo_step_diagnostic.ps1"),
        "-ParseModelCheckOnly",
        "-ModelCheckXmlPath", $xml.FullName,
        "-OutputDir", $OutputDir
    )
    if ($BaselineReportPath) {
        $args += @("-BaselineReportPath", $BaselineReportPath)
    }

    $run = Invoke-LoggedProcess `
        -FilePath "powershell" `
        -Arguments $args `
        -WorkingDirectory $OutputDir `
        -LogPrefix "modelcheck_parse" `
        -Timeout 300
    $jsonPath = Join-Path $OutputDir "creo_step_diagnostic_result.json"
    return [ordered]@{
        parsed = Test-Path -LiteralPath $jsonPath
        modelcheck_xml = $xml.FullName
        run = $run
        result_json = $jsonPath
        metrics = Get-CreoMetrics -DiagnosticJsonPath $jsonPath
    }
}

function New-CreoToolkitMetrics {
    param($Variant)

    $stepStats = if ($Variant -and $Variant.step_stats) { $Variant.step_stats.stats } else { $null }
    $strict = if ($Variant -and $Variant.strict_topology_gate) { $Variant.strict_topology_gate.report } else { $null }
    $modelcheck = if ($Variant -and $Variant.modelcheck_parse -and $Variant.modelcheck_parse.metrics) { $Variant.modelcheck_parse.metrics } else { $null }
    $toolkit = if ($Variant -and $Variant.toolkit_result) { $Variant.toolkit_result } else { $null }

    return [ordered]@{
        toolkit_runner_success = if ($toolkit) { $toolkit.runner_success } else { $null }
        toolkit_stage1_acceptance_passed = if ($toolkit) { $toolkit.stage1_toolkit_acceptance_passed } else { $null }
        toolkit_import_validation_score = if ($toolkit) { $toolkit.import_validation_score } else { $null }
        toolkit_modelcheck_errors = if ($toolkit) { $toolkit.modelcheck_errors } else { $null }
        toolkit_modelcheck_warnings = if ($toolkit) { $toolkit.modelcheck_warnings } else { $null }
        step_stats_success = if ($stepStats) { $stepStats.success } else { $null }
        step_stats_solids = if ($stepStats) { $stepStats.solids } else { $null }
        step_stats_shells = if ($stepStats) { $stepStats.shells } else { $null }
        step_stats_faces = if ($stepStats) { $stepStats.faces } else { $null }
        step_stats_brep_check_valid = if ($stepStats) { $stepStats.brep_check_valid } else { $null }
        strict_topology_passed = if ($strict) { $strict.passed } else { $null }
        strict_after_free_edges = if ($strict) { $strict.after_free_edges } else { $null }
        strict_after_multiple_edges = if ($strict) { $strict.after_multiple_edges } else { $null }
        strict_after_brep_check_valid = if ($strict) { $strict.after_brep_check_valid } else { $null }
        creo_diagnostic_passed = if ($modelcheck) { $modelcheck.diagnostic_passed } else { $null }
        creo_error_count = if ($modelcheck) { $modelcheck.error_count } else { $null }
        creo_warning_count = if ($modelcheck) { $modelcheck.warning_count } else { $null }
        creo_part_status = if ($modelcheck) { $modelcheck.part_status } else { $null }
        creo_import_score = if ($modelcheck) { $modelcheck.import_score } else { $null }
        creo_short_edges_item_count = if ($modelcheck) { $modelcheck.short_edges_item_count } else { $null }
    }
}

function Invoke-CreoToolkitMergeVariant {
    param(
        [Parameter(Mandatory = $true)][string]$RouteDir,
        [Parameter(Mandatory = $true)][string]$RouteName,
        [string]$BaseRemovedStep,
        [string]$PatchStep,
        [string]$ResolvedCreoRoot,
        [string]$ToolkitProbeExe,
        [string]$BaselineReportPath = ""
    )

    $variantDir = Join-Path $RouteDir "creo_toolkit_sewing"
    $modelcheckDir = Join-Path $variantDir "modelcheck"
    $parseDir = Join-Path $variantDir "modelcheck_parse"
    $validationDir = Join-Path $variantDir "validation"
    foreach ($dir in @($variantDir, $modelcheckDir, $parseDir, $validationDir)) {
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
    }

    if ($SkipCreo) {
        return [ordered]@{
            skipped = $true
            skipped_reason = "SkipCreo"
        }
    }
    if (-not $ResolvedCreoRoot -or -not $ToolkitProbeExe -or -not (Test-Path -LiteralPath $ToolkitProbeExe)) {
        return [ordered]@{
            skipped = $true
            skipped_reason = "Creo Toolkit probe was not built or Creo root was not resolved."
        }
    }
    if (-not $BaseRemovedStep -or -not (Test-Path -LiteralPath $BaseRemovedStep)) {
        return [ordered]@{
            skipped = $true
            skipped_reason = "Base removed candidate STEP was not produced."
            base_removed = Get-FileEvidence -PathValue $BaseRemovedStep
        }
    }
    if (-not $PatchStep -or -not (Test-Path -LiteralPath $PatchStep)) {
        return [ordered]@{
            skipped = $true
            skipped_reason = "Patch STEP was not produced."
            patch_step = Get-FileEvidence -PathValue $PatchStep
        }
    }

    $commonFiles = Join-Path $ResolvedCreoRoot "Common Files"
    $parametricBat = Join-Path $ResolvedCreoRoot "Parametric\bin\parametric.bat"
    $creoCommand = "`"$parametricBat`" -g:no_graphics -i:rpc_input"
    $env:PRO_COMM_MSG_EXE = Join-Path $commonFiles "x86e_win64\obj\pro_comm_msg.exe"
    $env:PATH = "$(Join-Path $commonFiles 'bin');$(Join-Path $commonFiles 'x86e_win64\lib');$(Join-Path $commonFiles 'protoolkit\x86e_win64\obj');$env:PATH"

    $toolkitResultPath = Join-Path $variantDir "creo_toolkit_result.json"
    $exportStepBase = Join-Path $variantDir "${RouteName}_creo_merged_export"
    $run = Invoke-LoggedProcess `
        -FilePath $ToolkitProbeExe `
        -Arguments @(
            "--base-removed-step", $BaseRemovedStep,
            "--geomagic-patch-step", $PatchStep,
            "--creo-command", $creoCommand,
            "--output-dir", $variantDir,
            "--modelcheck-output-dir", $modelcheckDir,
            "--export-step-base", $exportStepBase,
            "--result", $toolkitResultPath,
            "--model-name", "spo_${RouteName}"
        ) `
        -WorkingDirectory $variantDir `
        -LogPrefix "${RouteName}_creo_toolkit_sewing" `
        -Timeout $CreoTimeoutSeconds
    $exportedStep = Find-ExportedStep -ExportStepBase $exportStepBase
    $stepStats = $null
    $strictGate = $null
    if ($exportedStep -and (Test-Path -LiteralPath $exportedStep)) {
        $stepStats = Invoke-StepStats -StepPath $exportedStep -OutputDir $validationDir -Prefix "${RouteName}_creo_step_stats"
        $strictGate = Invoke-StrictGate -BeforeStep $SourceStep -AfterStep $exportedStep -OutputDir $validationDir -Prefix "${RouteName}_creo_strict_topology_gate"
    }
    $parse = Invoke-ModelcheckParseOnly -ModelCheckDir $modelcheckDir -OutputDir $parseDir -BaselineReportPath $BaselineReportPath
    $toolkitResult = Read-JsonFile -PathValue $toolkitResultPath

    $variant = [ordered]@{
        skipped = $false
        source_files = [ordered]@{
            source_step = Get-FileEvidence -PathValue $SourceStep
            base_removed_candidate = Get-FileEvidence -PathValue $BaseRemovedStep
            patch_step = Get-FileEvidence -PathValue $PatchStep
            exported_creo_step = Get-FileEvidence -PathValue $exportedStep
        }
        artifacts = [ordered]@{
            route_dir = $variantDir
            toolkit_result = $toolkitResultPath
            modelcheck_dir = $modelcheckDir
            exported_step = $exportedStep
        }
        commands = [ordered]@{
            creo_toolkit_sewing = $run
        }
        toolkit_result = $toolkitResult
        step_stats = $stepStats
        strict_topology_gate = $strictGate
        modelcheck_parse = $parse
    }
    $variant.metrics = New-CreoToolkitMetrics -Variant $variant
    $variant.status = if ($variant.metrics.strict_topology_passed -and $variant.metrics.step_stats_solids -gt 0) { "Passed" } else { "Failed" }
    return $variant
}

function Invoke-PostValidation {
    param(
        [Parameter(Mandatory = $true)][string]$RouteDir,
        [Parameter(Mandatory = $true)][string]$RouteName,
        [string]$AppliedStepPath,
        [string]$BaselineReportPath = ""
    )

    $validationDir = Join-Path $RouteDir "validation"
    New-Item -ItemType Directory -Force -Path $validationDir | Out-Null
    if (-not $AppliedStepPath -or -not (Test-Path -LiteralPath $AppliedStepPath)) {
        return [ordered]@{
            attempted = $false
            skipped_reason = "No applied STEP was produced."
            applied_step = Get-FileEvidence -PathValue $AppliedStepPath
        }
    }

    $stepStats = Invoke-StepStats -StepPath $AppliedStepPath -OutputDir $validationDir -Prefix "${RouteName}_step_stats"
    $strictGate = Invoke-StrictGate -BeforeStep $SourceStep -AfterStep $AppliedStepPath -OutputDir $validationDir -Prefix "${RouteName}_strict_topology_gate"
    $creo = Invoke-CreoDiagnostic -StepPath $AppliedStepPath -OutputDir (Join-Path $validationDir "creo") -BaselineReportPath $BaselineReportPath

    return [ordered]@{
        attempted = $true
        applied_step = Get-FileEvidence -PathValue $AppliedStepPath
        step_stats = $stepStats
        strict_topology_gate = $strictGate
        creo = $creo
    }
}

function New-RouteMetrics {
    param(
        $Baseline,
        $Validation
    )

    $stepStats = if ($Validation -and $Validation.step_stats) { $Validation.step_stats.stats } else { $null }
    $strict = if ($Validation -and $Validation.strict_topology_gate) { $Validation.strict_topology_gate.report } else { $null }
    $creo = if ($Validation -and $Validation.creo -and $Validation.creo.metrics) { $Validation.creo.metrics } else { $null }

    return [ordered]@{
        baseline_stage = if ($Baseline) { $Baseline.stage } else { $null }
        baseline_overall_success = if ($Baseline) { $Baseline.overall_success } else { $null }
        patch_apply_success = if ($Baseline -and $Baseline.patch_apply) { $Baseline.patch_apply.success } else { $null }
        patch_apply_gate_passed = if ($Baseline -and $Baseline.patch_apply) { $Baseline.patch_apply.gate_passed } else { $null }
        patch_apply_gate_after_free_edges = if ($Baseline -and $Baseline.patch_apply) { $Baseline.patch_apply.gate_after_free_edges } else { $null }
        patch_apply_gate_after_multiple_edges = if ($Baseline -and $Baseline.patch_apply) { $Baseline.patch_apply.gate_after_multiple_edges } else { $null }
        used_original_boundary_surface_retrim = if ($Baseline -and $Baseline.patch_apply) { $Baseline.patch_apply.used_original_boundary_surface_retrim } else { $null }
        attempted_multi_surface_boundary_shell = if ($Baseline -and $Baseline.patch_apply) { $Baseline.patch_apply.attempted_multi_surface_boundary_shell } else { $null }
        used_multi_surface_boundary_shell = if ($Baseline -and $Baseline.patch_apply) { $Baseline.patch_apply.used_multi_surface_boundary_shell } else { $null }
        multi_surface_boundary_sample_count = if ($Baseline -and $Baseline.patch_apply) { $Baseline.patch_apply.multi_surface_boundary_sample_count } else { $null }
        multi_surface_projected_sample_count = if ($Baseline -and $Baseline.patch_apply) { $Baseline.patch_apply.multi_surface_projected_sample_count } else { $null }
        multi_surface_failed_projection_count = if ($Baseline -and $Baseline.patch_apply) { $Baseline.patch_apply.multi_surface_failed_projection_count } else { $null }
        multi_surface_max_projection_distance = if ($Baseline -and $Baseline.patch_apply) { $Baseline.patch_apply.multi_surface_max_projection_distance } else { $null }
        multi_surface_built_face_count = if ($Baseline -and $Baseline.patch_apply) { $Baseline.patch_apply.multi_surface_built_face_count } else { $null }
        multi_surface_closed_wire_count = if ($Baseline -and $Baseline.patch_apply) { $Baseline.patch_apply.multi_surface_closed_wire_count } else { $null }
        multi_surface_open_wire_count = if ($Baseline -and $Baseline.patch_apply) { $Baseline.patch_apply.multi_surface_open_wire_count } else { $null }
        multi_surface_failed_patch_face_index = if ($Baseline -and $Baseline.patch_apply) { $Baseline.patch_apply.multi_surface_failed_patch_face_index } else { $null }
        multi_surface_failed_face_edge_count = if ($Baseline -and $Baseline.patch_apply) { $Baseline.patch_apply.multi_surface_failed_face_edge_count } else { $null }
        retrim_pcurve_attempt_count = if ($Baseline -and $Baseline.patch_apply) { $Baseline.patch_apply.retrim_boundary_edge_pcurve_rebuild_attempt_count } else { $null }
        retrim_pcurve_success_count = if ($Baseline -and $Baseline.patch_apply) { $Baseline.patch_apply.retrim_boundary_edge_pcurve_rebuild_success_count } else { $null }
        retrim_pcurve_failure_count = if ($Baseline -and $Baseline.patch_apply) { $Baseline.patch_apply.retrim_boundary_edge_pcurve_rebuild_failure_count } else { $null }
        retrim_same_parameter_failure_count = if ($Baseline -and $Baseline.patch_apply) { $Baseline.patch_apply.retrim_boundary_edge_same_parameter_failure_count } else { $null }
        retrim_max_same_parameter_deviation = if ($Baseline -and $Baseline.patch_apply) { $Baseline.patch_apply.retrim_boundary_edge_max_same_parameter_deviation } else { $null }
        boundary_gap_max = if ($Baseline -and $Baseline.patch_apply -and $Baseline.patch_apply.trim_diagnostics) { $Baseline.patch_apply.trim_diagnostics.boundary_gap_max } else { $null }
        internal_seam_gap_max = if ($Baseline -and $Baseline.patch_apply -and $Baseline.patch_apply.trim_diagnostics) { $Baseline.patch_apply.trim_diagnostics.internal_seam_gap_max } else { $null }
        step_stats_success = if ($stepStats) { $stepStats.success } else { $null }
        step_stats_solids = if ($stepStats) { $stepStats.solids } else { $null }
        step_stats_shells = if ($stepStats) { $stepStats.shells } else { $null }
        step_stats_faces = if ($stepStats) { $stepStats.faces } else { $null }
        strict_topology_passed = if ($strict) { $strict.passed } else { $null }
        strict_after_free_edges = if ($strict) { $strict.after_free_edges } else { $null }
        strict_after_multiple_edges = if ($strict) { $strict.after_multiple_edges } else { $null }
        strict_after_brep_check_valid = if ($strict) { $strict.after_brep_check_valid } else { $null }
        strict_roundtrip_free_edges = if ($strict) { $strict.roundtrip_free_edges } else { $null }
        creo_runner_success = if ($creo) { $creo.runner_success } else { $null }
        creo_diagnostic_passed = if ($creo) { $creo.diagnostic_passed } else { $null }
        creo_error_count = if ($creo) { $creo.error_count } else { $null }
        creo_warning_count = if ($creo) { $creo.warning_count } else { $null }
        creo_part_status = if ($creo) { $creo.part_status } else { $null }
        creo_import_score = if ($creo) { $creo.import_score } else { $null }
        creo_short_edges_item_count = if ($creo) { $creo.short_edges_item_count } else { $null }
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
    $OutputDir = Join-Path $repoRoot ("data\baseline_runs\boundary_trim_fill_experiments\" + (Get-Date -Format "yyyyMMdd_HHmmss"))
} else {
    $OutputDir = Convert-ToRepoAbsolutePath -PathValue $OutputDir
}
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

$vcpkgRoot = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { Join-Path $env:USERPROFILE "vcpkg" }
$env:VCPKG_ROOT = $vcpkgRoot
$env:Path = "C:\Program Files\CMake\bin;$vcpkgRoot;" + $env:Path

foreach ($target in @(
        "g1_boundary_fill_patch_exporter",
        "step_unit_normalizer",
        "creo_phase1_input_exporter",
        "corner_baseline_probe",
        "step_stats",
        "strict_topology_gate_probe"
    )) {
    Invoke-BuildTarget -Target $target
}

$resolvedCreoRoot = ""
$toolkitProbeExe = ""
if (-not $SkipCreo) {
    $resolvedCreoRoot = Find-CreoToolkitRoot -PreferredRoot $CreoRoot
    $toolkitSourceDir = Join-Path $repoRoot "tools\creo_toolkit_phase1_repair_probe"
    $toolkitBuildDir = Join-Path $repoRoot "build\creo_toolkit_phase1_repair_probe"
    Write-Host ""
    Write-Host "==> cmake -S $toolkitSourceDir -B $toolkitBuildDir -DCREO_ROOT=$resolvedCreoRoot"
    & cmake -S $toolkitSourceDir -B $toolkitBuildDir "-DCREO_ROOT=$resolvedCreoRoot"
    if ($LASTEXITCODE -ne 0) {
        throw "Configure failed for Creo Toolkit phase1 repair probe."
    }
    Write-Host ""
    Write-Host "==> cmake --build $toolkitBuildDir --config Release"
    & cmake --build $toolkitBuildDir --config Release
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed for Creo Toolkit phase1 repair probe."
    }
    $toolkitProbeExe = Find-CreoToolkitProbeExe -BuildDir $toolkitBuildDir
}

$cornerProbeExe = Find-BuiltExe -ExeName "corner_baseline_probe.exe"
$g1ExporterExe = Find-BuiltExe -ExeName "g1_boundary_fill_patch_exporter.exe"
$normalizerExe = Find-BuiltExe -ExeName "step_unit_normalizer.exe"
$inputExporterExe = Find-BuiltExe -ExeName "creo_phase1_input_exporter.exe"

$resultPath = Join-Path $OutputDir "boundary_trim_fill_experiment_result.json"
$result = [ordered]@{
    tool = "run_boundary_trim_fill_experiments"
    created_at = (Get-Date).ToString("o")
    status = "Started"
    source_files = [ordered]@{
        source_step = Get-FileEvidence -PathValue $SourceStep
    }
    parameters = [ordered]@{
        preset = $Preset
        requested_candidate_id = $CandidateId
        wrap_core = $WrapCore
        creo_root = if ($resolvedCreoRoot) { $resolvedCreoRoot } else { $CreoRoot }
        g1_tolerance3d = $G1Tolerance3d
        g1_tolerance_angle = $G1ToleranceAngle
        corner_feature_samples = $CornerFeatureSamples
        candidate_over_cover_samples = $CandidateOverCoverSamples
        candidate_over_cover_width = $CandidateOverCoverWidth
        candidate_over_cover_rings = $CandidateOverCoverRings
        candidate_over_cover_miter_max_scale = $CandidateOverCoverMiterMaxScale
        enable_auxiliary_support_collar = [bool]$EnableAuxiliarySupportCollar
        support_collar_samples = $SupportCollarSamples
        support_collar_width = $SupportCollarWidth
        support_collar_rings = $SupportCollarRings
        support_collar_under_cover = $SupportCollarUnderCover
        skip_creo = [bool]$SkipCreo
    }
    flow = @(
        "Route 1: build a fresh OCCT BRepFill_Filling patch constrained by the original CAD boundary and adjacent support faces with G1 constraints.",
        "Route 1: feed that patch into PatchReplacementCommand, then export the applied STEP only if internal StrictTopologyGate passes.",
        "Route 2: freshly generate a Geomagic patch from B1 dense edge sampling plus candidate/source-face parallel over-cover, align it to the original candidate bbox in the base-removed STEP coordinate space, then re-apply with strict original-boundary re-trim. A narrow adjacent-face support collar is added only when -EnableAuxiliarySupportCollar is set.",
        "Both OCCT-sewn routes: run OCCT step_stats, independent StrictTopologyGate, and Creo Distributed Batch + ModelCHECK on the exported applied STEP when it exists.",
        "Both Creo-sewn routes: import the same base_removed_candidate STEP and each route patch into Creo Toolkit with join_surfaces=1 and attempt_make_solid=1, then export and validate the Creo result."
    )
    routes = [ordered]@{}
    shared_inputs = [ordered]@{}
    artifact_retention_policy = [ordered]@{
        preserve_failed_run_artifacts = $true
        applies_when_modelcheck_fails = $true
        applies_when_occt_gate_fails = $true
        required_artifacts = @(
            "generated patch STEP files",
            "base_removed_candidate.stp",
            "normalized patch STEP files",
            "OCCT applied/stitched STEP files when produced",
            "Creo exported stitched STEP files when produced"
        )
        note = "Do not delete run output only because ModelCHECK, StrictTopologyGate, or Creo solid acceptance failed."
    }
    preserved_artifacts = [ordered]@{}
    file_provenance = [ordered]@{
        source_step_generated_this_run = $false
        source_step = $SourceStep
        requested_candidate_id = $CandidateId
        fresh_geomagic_patch_generated_this_run = $false
        fresh_geomagic_patch = ""
        base_removed_candidate_generated_this_run = $false
        base_removed_candidate = ""
        normalized_patch_generated_this_run = $false
        normalized_patch = ""
        resolved_candidate_id = $CandidateId
    }
}

$route1Dir = Join-Path $OutputDir "route1_g1_boundary_fill"
New-Item -ItemType Directory -Force -Path $route1Dir | Out-Null
$route1Patch = Join-Path $route1Dir "g1_boundary_fill_patch.stp"
$route1Manifest = Join-Path $route1Dir "g1_boundary_fill_manifest.json"
$route1G1Run = Invoke-LoggedProcess `
    -FilePath $g1ExporterExe `
    -Arguments @(
        "--source-step", $SourceStep,
        "--candidate-id", $CandidateId,
        "--output-patch", $route1Patch,
        "--manifest", $route1Manifest,
        "--tolerance3d", "$G1Tolerance3d"
    ) `
    -WorkingDirectory $route1Dir `
    -LogPrefix "g1_boundary_fill_patch_exporter" `
    -Timeout 600
$route1ManifestJson = Read-JsonFile -PathValue $route1Manifest
$route1CandidateId = if ($route1ManifestJson -and $route1ManifestJson.candidate_id -ne $null) {
    [string]$route1ManifestJson.candidate_id
} else {
    $CandidateId
}

$route1ApplyReport = Join-Path $route1Dir "apply\baseline_report.json"
$route1ApplyRun = $null
$route1Baseline = $null
if (Test-Path -LiteralPath $route1Patch) {
    $route1ApplyDir = Split-Path -Parent $route1ApplyReport
    New-Item -ItemType Directory -Force -Path $route1ApplyDir | Out-Null
    $route1ApplyRun = Invoke-LoggedProcess `
        -FilePath $cornerProbeExe `
        -Arguments @(
            "--source-step", $SourceStep,
            "--candidate-id", $route1CandidateId,
            "--patch", $route1Patch,
            "--output-dir", $route1ApplyDir,
            "--report", $route1ApplyReport
        ) `
        -WorkingDirectory $route1ApplyDir `
        -LogPrefix "route1_apply" `
        -Timeout $TimeoutSeconds
    $route1Baseline = Read-JsonFile -PathValue $route1ApplyReport
}
$route1AppliedStep = Get-AppliedStepFromBaseline -Baseline $route1Baseline
$route1Validation = Invoke-PostValidation -RouteDir $route1Dir -RouteName "route1" -AppliedStepPath $route1AppliedStep -BaselineReportPath $route1ApplyReport
$result.routes.route1_g1_boundary_fill = [ordered]@{
    status = if ($route1Validation.attempted -and $route1Validation.strict_topology_gate.report.passed -and ($SkipCreo -or $route1Validation.creo.metrics.diagnostic_passed)) { "Passed" } else { "Failed" }
    flow = @(
        "Generate G1 boundary-constrained fill patch directly from source STEP original boundary.",
        "Apply generated patch through current PatchReplacementCommand.",
        "Validate exported applied STEP with OCCT stats, independent StrictTopologyGate, and Creo ModelCHECK."
    )
    source_files = [ordered]@{
        source_step = Get-FileEvidence -PathValue $SourceStep
        generated_g1_patch = Get-FileEvidence -PathValue $route1Patch
        applied_step = Get-FileEvidence -PathValue $route1AppliedStep
    }
    artifacts = [ordered]@{
        route_dir = $route1Dir
        g1_manifest = $route1Manifest
        baseline_report = $route1ApplyReport
    }
    commands = [ordered]@{
        g1_patch_export = $route1G1Run
        apply = $route1ApplyRun
    }
    g1_patch = $route1ManifestJson
    baseline = $route1Baseline
    validation = $route1Validation
    metrics = New-RouteMetrics -Baseline $route1Baseline -Validation $route1Validation
}

$route2Dir = Join-Path $OutputDir "route2_expanded_strict_trim"
$route2GenerationDir = Join-Path $route2Dir "fresh_geomagic_generation"
$route2ApplyDir = Join-Path $route2Dir "apply_normalized_patch"
New-Item -ItemType Directory -Force -Path $route2GenerationDir | Out-Null
New-Item -ItemType Directory -Force -Path $route2ApplyDir | Out-Null
$route2GenerationReport = Join-Path $route2GenerationDir "baseline_report.json"
$route2ApplyReport = Join-Path $route2ApplyDir "baseline_report.json"
$route2GenRun = $null
$route2BaselineGeneration = $null
$route2RawPatch = ""
$route2NormalizedPatch = Join-Path $route2Dir "normalized_patch_mm.stp"
$route2NormalizeManifest = Join-Path $route2Dir "normalized_patch_mm_manifest.json"
$route2NormalizeRun = $null
$route2ApplyRun = $null
$route2BaselineApply = $null
$baseRemoved = ""
$baseRemovedManifest = ""

$route2SupportCollarArgs = @()
if ($EnableAuxiliarySupportCollar) {
    $route2SupportCollarArgs = @(
        "--b2-adjacent-face-support-collar",
        "--support-collar-samples", "$SupportCollarSamples",
        "--support-collar-width", "$SupportCollarWidth",
        "--support-collar-rings", "$SupportCollarRings",
        "--adaptive-support-collar-width",
        "--support-collar-under-cover", "$SupportCollarUnderCover"
    )
}

if (Test-Path -LiteralPath $WrapCore) {
    $route2GenerationArgs = @(
        "--source-step", $SourceStep,
        "--candidate-id", $CandidateId,
        "--output-dir", $route2GenerationDir,
        "--report", $route2GenerationReport,
        "--wrap-core", $WrapCore,
        "--timeout-seconds", "$TimeoutSeconds",
        "--b1-corner-feature-sampling",
        "--corner-feature-samples", "$CornerFeatureSamples",
        "--candidate-surface-over-cover",
        "--candidate-over-cover-samples", "$CandidateOverCoverSamples",
        "--candidate-over-cover-width", "$CandidateOverCoverWidth",
        "--candidate-over-cover-rings", "$CandidateOverCoverRings",
        "--candidate-over-cover-miter-max-scale", "$CandidateOverCoverMiterMaxScale"
    )
    $route2GenerationArgs += $route2SupportCollarArgs
    $route2GenerationArgs += @(
        "--allow-high-risk-patch-preview",
        "--strict-original-boundary-retrim"
    )
    $route2GenRun = Invoke-LoggedProcess `
        -FilePath $cornerProbeExe `
        -Arguments $route2GenerationArgs `
        -WorkingDirectory $route2GenerationDir `
        -LogPrefix "route2_fresh_geomagic_generation" `
        -Timeout ($TimeoutSeconds + 300)
    $route2BaselineGeneration = Read-JsonFile -PathValue $route2GenerationReport
    $route2RawPatch = Get-PatchStepFromBaseline -Baseline $route2BaselineGeneration
    if ($route2RawPatch -and (Test-Path -LiteralPath $route2RawPatch)) {
        $result.file_provenance["fresh_geomagic_patch_generated_this_run"] = $true
        $result.file_provenance["fresh_geomagic_patch"] = $route2RawPatch
    }
}

$route2CandidateId = if ($route2BaselineGeneration -and $route2BaselineGeneration.candidate_id -ne $null) {
    [string]$route2BaselineGeneration.candidate_id
} else {
    $CandidateId
}

if ($route2RawPatch -and (Test-Path -LiteralPath $route2RawPatch)) {
    $sharedInputsDir = Join-Path $OutputDir "shared_inputs"
    New-Item -ItemType Directory -Force -Path $sharedInputsDir | Out-Null
    $candidateForBaseRemoved = $route2CandidateId
    if (-not $candidateForBaseRemoved -or $candidateForBaseRemoved -eq "auto") {
        $candidateForBaseRemoved = $route1CandidateId
    }
    if (-not $candidateForBaseRemoved) {
        $candidateForBaseRemoved = $CandidateId
    }

    $baseRemoved = Join-Path $sharedInputsDir "base_removed_candidate.stp"
    $baseRemovedManifest = Join-Path $sharedInputsDir "base_removed_candidate_manifest.json"
    $baseRemovedRun = Invoke-LoggedProcess `
        -FilePath $inputExporterExe `
        -Arguments @(
            "--source-step", $SourceStep,
            "--candidate-id", "$candidateForBaseRemoved",
            "--base-removed-output", $baseRemoved,
            "--manifest", $baseRemovedManifest
        ) `
        -WorkingDirectory $sharedInputsDir `
        -LogPrefix "base_removed_candidate_export" `
        -Timeout 600
    $baseRemovedManifestJson = Read-JsonFile -PathValue $baseRemovedManifest
    if ($baseRemovedManifestJson -and $baseRemovedManifestJson.candidate_id -ne $null) {
        $route2CandidateId = [string]$baseRemovedManifestJson.candidate_id
        $route1CandidateId = [string]$baseRemovedManifestJson.candidate_id
    }
    $result.file_provenance["base_removed_candidate_generated_this_run"] = Test-Path -LiteralPath $baseRemoved
    $result.file_provenance["base_removed_candidate"] = $baseRemoved
    $result.file_provenance["resolved_candidate_id"] = $route2CandidateId
    $result.shared_inputs = [ordered]@{
        source_step = Get-FileEvidence -PathValue $SourceStep
        base_removed_candidate = Get-FileEvidence -PathValue $baseRemoved
        base_removed_manifest = $baseRemovedManifest
        base_removed_export_command = $baseRemovedRun
        resolved_candidate_id = $route2CandidateId
    }

    $route2NormalizeRun = Invoke-LoggedProcess `
        -FilePath $normalizerExe `
        -Arguments @(
            "--input", $route2RawPatch,
            "--output", $route2NormalizedPatch,
            "--manifest", $route2NormalizeManifest,
            "--target-step", $baseRemoved,
            "--source-step", $SourceStep,
            "--candidate-id", "$route2CandidateId"
        ) `
        -WorkingDirectory $route2Dir `
        -LogPrefix "route2_step_unit_normalizer" `
        -Timeout 600
}

if (Test-Path -LiteralPath $route2NormalizedPatch) {
    $route2ApplyArgs = @(
        "--source-step", $SourceStep,
        "--candidate-id", $route2CandidateId,
        "--patch", $route2NormalizedPatch,
        "--output-dir", $route2ApplyDir,
        "--report", $route2ApplyReport,
        "--b1-corner-feature-sampling",
        "--corner-feature-samples", "$CornerFeatureSamples",
        "--candidate-surface-over-cover",
        "--candidate-over-cover-samples", "$CandidateOverCoverSamples",
        "--candidate-over-cover-width", "$CandidateOverCoverWidth",
        "--candidate-over-cover-rings", "$CandidateOverCoverRings",
        "--candidate-over-cover-miter-max-scale", "$CandidateOverCoverMiterMaxScale"
    )
    $route2ApplyArgs += $route2SupportCollarArgs
    $route2ApplyArgs += @(
        "--allow-high-risk-patch-preview",
        "--strict-original-boundary-retrim"
    )
    $route2ApplyRun = Invoke-LoggedProcess `
        -FilePath $cornerProbeExe `
        -Arguments $route2ApplyArgs `
        -WorkingDirectory $route2ApplyDir `
        -LogPrefix "route2_apply_normalized_patch" `
        -Timeout $TimeoutSeconds
    $route2BaselineApply = Read-JsonFile -PathValue $route2ApplyReport
}

$route2AppliedStep = Get-AppliedStepFromBaseline -Baseline $route2BaselineApply
$route2Validation = Invoke-PostValidation -RouteDir $route2Dir -RouteName "route2" -AppliedStepPath $route2AppliedStep -BaselineReportPath $route2ApplyReport
$result.routes.route2_expanded_strict_trim = [ordered]@{
    status = if ($route2Validation.attempted -and $route2Validation.strict_topology_gate.report.passed -and ($SkipCreo -or $route2Validation.creo.metrics.diagnostic_passed)) { "Passed" } else { "Failed" }
    flow = @(
        "Freshly generate Geomagic STEP patch from B1 feature-edge samples and candidate/source-face parallel over-cover; optionally include a narrow auxiliary adjacent-face support band when requested.",
        "Normalize the fresh Geomagic STEP through OCCT STEP read/write to produce normalized_patch_mm.stp and align STEP units with the project/original STEP path.",
        "Apply the normalized patch through strict original-boundary re-trim and current PatchReplacementCommand.",
        "Validate exported applied STEP with OCCT stats, independent StrictTopologyGate, and Creo ModelCHECK."
    )
    source_files = [ordered]@{
        source_step = Get-FileEvidence -PathValue $SourceStep
        fresh_geomagic_patch = Get-FileEvidence -PathValue $route2RawPatch
        normalized_patch_mm = Get-FileEvidence -PathValue $route2NormalizedPatch
        applied_step = Get-FileEvidence -PathValue $route2AppliedStep
    }
    artifacts = [ordered]@{
        route_dir = $route2Dir
        generation_report = $route2GenerationReport
        normalize_manifest = $route2NormalizeManifest
        apply_report = $route2ApplyReport
    }
    commands = [ordered]@{
        fresh_geomagic_generation = $route2GenRun
        normalize_patch = $route2NormalizeRun
        apply = $route2ApplyRun
    }
    generation_baseline = $route2BaselineGeneration
    normalize_manifest = Read-JsonFile -PathValue $route2NormalizeManifest
    apply_baseline = $route2BaselineApply
    validation = $route2Validation
    metrics = New-RouteMetrics -Baseline $route2BaselineApply -Validation $route2Validation
}

$result.file_provenance["normalized_patch_generated_this_run"] = Test-Path -LiteralPath $route2NormalizedPatch
$result.file_provenance["normalized_patch"] = $route2NormalizedPatch

$route1Creo = Invoke-CreoToolkitMergeVariant `
    -RouteDir $route1Dir `
    -RouteName "route1_g1" `
    -BaseRemovedStep $baseRemoved `
    -PatchStep $route1Patch `
    -ResolvedCreoRoot $resolvedCreoRoot `
    -ToolkitProbeExe $toolkitProbeExe `
    -BaselineReportPath $route1ApplyReport
$result.routes.route1_g1_boundary_fill_creo_sewing = [ordered]@{
    status = if ($route1Creo.skipped) { "Skipped" } else { $route1Creo.status }
    flow = @(
        "Use the same base_removed_candidate STEP exported from the original source/candidate.",
        "Use the route 1 G1 boundary-constrained patch as the Creo import feature input.",
        "Run ProImportfeatCreate with join_surfaces=1 and attempt_make_solid=1, then regenerate, ModelCHECK, export STEP, step_stats and StrictTopologyGate."
    )
    result = $route1Creo
    metrics = if ($route1Creo.metrics) { $route1Creo.metrics } else { $null }
}

$route2Creo = Invoke-CreoToolkitMergeVariant `
    -RouteDir $route2Dir `
    -RouteName "route2_aligned" `
    -BaseRemovedStep $baseRemoved `
    -PatchStep $route2NormalizedPatch `
    -ResolvedCreoRoot $resolvedCreoRoot `
    -ToolkitProbeExe $toolkitProbeExe `
    -BaselineReportPath $route2ApplyReport
$result.routes.route2_expanded_strict_trim_creo_sewing = [ordered]@{
    status = if ($route2Creo.skipped) { "Skipped" } else { $route2Creo.status }
    flow = @(
        "Use the same base_removed_candidate STEP exported from the original source/candidate.",
        "Use route 2 normalized_patch_mm.stp after explicit candidate-bbox scale/coordinate alignment.",
        "Run ProImportfeatCreate with join_surfaces=1 and attempt_make_solid=1, then regenerate, ModelCHECK, export STEP, step_stats and StrictTopologyGate."
    )
    result = $route2Creo
    metrics = if ($route2Creo.metrics) { $route2Creo.metrics } else { $null }
}

$result.preserved_artifacts = [ordered]@{
    output_dir = $OutputDir
    result_json = $resultPath
    source_step = Get-FileEvidence -PathValue $SourceStep
    patch_steps = [ordered]@{
        route1_g1_patch = Get-FileEvidence -PathValue $route1Patch
        route2_fresh_geomagic_patch = Get-FileEvidence -PathValue $route2RawPatch
        route2_normalized_patch_mm = Get-FileEvidence -PathValue $route2NormalizedPatch
    }
    base_removed = [ordered]@{
        base_removed_candidate = Get-FileEvidence -PathValue $baseRemoved
        base_removed_manifest = Get-FileEvidence -PathValue $baseRemovedManifest
    }
    stitched_steps = [ordered]@{
        route1_occt_applied_step = Get-FileEvidence -PathValue $route1AppliedStep
        route2_occt_applied_step = Get-FileEvidence -PathValue $route2AppliedStep
        route1_creo_exported_step = Get-FileEvidence -PathValue (Get-CreoVariantExportedStep -Variant $route1Creo)
        route2_creo_exported_step = Get-FileEvidence -PathValue (Get-CreoVariantExportedStep -Variant $route2Creo)
    }
    manifests_and_reports = [ordered]@{
        route1_g1_manifest = Get-FileEvidence -PathValue $route1Manifest
        route1_apply_report = Get-FileEvidence -PathValue $route1ApplyReport
        route2_generation_report = Get-FileEvidence -PathValue $route2GenerationReport
        route2_normalize_manifest = Get-FileEvidence -PathValue $route2NormalizeManifest
        route2_apply_report = Get-FileEvidence -PathValue $route2ApplyReport
    }
}

$routeStatuses = @(
    $result.routes.route1_g1_boundary_fill.status,
    $result.routes.route2_expanded_strict_trim.status,
    $result.routes.route1_g1_boundary_fill_creo_sewing.status,
    $result.routes.route2_expanded_strict_trim_creo_sewing.status
)
$nonSkippedStatuses = @($routeStatuses | Where-Object { $_ -ne "Skipped" })
$result.status = if ($nonSkippedStatuses -contains "Passed") { "AtLeastOneRoutePassed" } else { "AllRoutesFailed" }
$result | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $resultPath -Encoding UTF8

Write-Host ""
Write-Host "Boundary trim/fill experiment result: $resultPath"
Write-Host "Route 1 status: $($result.routes.route1_g1_boundary_fill.status)"
Write-Host "Route 2 status: $($result.routes.route2_expanded_strict_trim.status)"
Write-Host "Route 1 Creo sewing status: $($result.routes.route1_g1_boundary_fill_creo_sewing.status)"
Write-Host "Route 2 Creo sewing status: $($result.routes.route2_expanded_strict_trim_creo_sewing.status)"
Write-Host "Source STEP: $SourceStep"
Write-Host "Artifact retention: output directory is preserved even when ModelCHECK or gate validation fails."

if ($result.status -eq "AllRoutesFailed") {
    exit 20
}
