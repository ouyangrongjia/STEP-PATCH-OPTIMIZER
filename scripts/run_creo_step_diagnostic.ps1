param(
    [Parameter(Mandatory = $true)]
    [string]$StepPath,
    [string]$CreoRoot = "",
    [string]$OutputDir = "",
    [int]$TimeoutSeconds = 600,
    [ValidateSet("ObjectText", "ObjectPathAttribute")]
    [string]$DxcObjectSyntax = "ObjectText",
    [switch]$SkipModelCheck
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

function Get-XmlEscapedText {
    param([Parameter(Mandatory = $true)][string]$Value)
    return [System.Security.SecurityElement]::Escape($Value)
}

function Test-CreoRoot {
    param([Parameter(Mandatory = $true)][string]$CandidateRoot)

    if (-not $CandidateRoot) {
        return $false
    }

    $root = [System.IO.Path]::GetFullPath($CandidateRoot)
    $commonFiles = Join-Path $root "Common Files"
    $parametricBat = Join-Path $root "Parametric\bin\parametric.bat"
    $stepTtd = Join-Path $commonFiles "text\ttds\step_3d_import.ttd"
    $modelCheckTtd = Join-Path $commonFiles "text\ttds\modelcheck.ttd"
    $dbatchc = Join-Path $commonFiles "x86e_win64\obj\dbatchc.exe"

    return (Test-Path -LiteralPath $parametricBat) -and
        (Test-Path -LiteralPath $stepTtd) -and
        (Test-Path -LiteralPath $modelCheckTtd) -and
        (Test-Path -LiteralPath $dbatchc)
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

function Find-CreoRoot {
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
        if (Test-CreoRoot -CandidateRoot $candidate) {
            return $candidate
        }
    }

    throw "Creo root was not found. Pass -CreoRoot or set CREO_ROOT/CREO_DIRECTORY."
}

function Set-CreoBatchEnvironment {
    param([Parameter(Mandatory = $true)][string]$Root)

    $commonFiles = Join-Path $Root "Common Files"
    $parametric = Join-Path $Root "Parametric"
    $machine = "x86e_win64"

    $env:CREO_DIRECTORY = $Root
    $env:PRO_DIRECTORY = $commonFiles
    $env:CREOAPP_DIRECTORY = $parametric
    $env:PRO_MACHINE_TYPE = $machine
    $env:DB_CLIENT_DIRECTORY = $commonFiles
    $env:DBC_TEXT_RESOURCE = Join-Path $commonFiles "text\resource"
    $env:DBC_UITOOLS_RESOURCE = Join-Path $commonFiles "proe\uitools\text\resource"
    $env:MOZILLA_FIVE_HOME = Join-Path $commonFiles "$machine\obj\MOZILLA"
    $env:NMSD_PATH = Join-Path $commonFiles "$machine\nms\nmsd.exe"
    $env:PROE_START = Join-Path $parametric "bin\parametric.bat"
    $env:PROE_STARTUP_EXE = Join-Path $parametric "bin\parametric.exe"
    $env:PROE_STARTUP_PSF = Join-Path $parametric "bin\parametric.psf"
    $env:PRO_COMM_MSG_EXE = Join-Path $commonFiles "$machine\obj\pro_comm_msg.exe"
    $env:Path = "$(Join-Path $commonFiles 'bin');$(Join-Path $commonFiles "$machine\lib");$env:Path"
}

function New-DxcFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$TtdPath,
        [Parameter(Mandatory = $true)][string]$TaskName,
        [Parameter(Mandatory = $true)][string]$ObjectPath,
        [Parameter(Mandatory = $true)][string]$TaskOutputDir,
        [Parameter(Mandatory = $true)][string]$Syntax
    )

    $escapedTtd = Get-XmlEscapedText -Value $TtdPath
    $escapedObject = Get-XmlEscapedText -Value $ObjectPath
    $escapedOutput = Get-XmlEscapedText -Value $TaskOutputDir
    $escapedName = Get-XmlEscapedText -Value $TaskName

    if ($Syntax -eq "ObjectPathAttribute") {
        $objectNode = "    <Object Path=`"$escapedObject`"/>"
    } else {
        $objectNode = "    <Object>$escapedObject</Object>"
    }

    $content = @"
<?xml version="1.0" encoding="UTF-8"?>
<DXC>
  <WINDCHILL/>
  <Group TTD="$escapedTtd" DSQM="_LOCAL" Name="$escapedName" Output="2" OutputDir="$escapedOutput" VaultResults="0" PrimaryContent="0">
$objectNode
  </Group>
</DXC>
"@

    Set-Content -LiteralPath $Path -Value $content -Encoding UTF8
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

    $startedAt = Get-Date
    $process = Start-Process `
        -FilePath $FilePath `
        -ArgumentList $Arguments `
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

    $endedAt = Get-Date
    $exitCode = $null
    if (-not $timedOut) {
        $exitCode = $process.ExitCode
    }

    return [ordered]@{
        file_path = $FilePath
        arguments = $Arguments
        working_directory = $WorkingDirectory
        started_at = $startedAt.ToString("o")
        ended_at = $endedAt.ToString("o")
        timeout_seconds = $Timeout
        timed_out = $timedOut
        exit_code = $exitCode
        stdout_path = $stdoutPath
        stderr_path = $stderrPath
    }
}

function Find-NewFiles {
    param(
        [Parameter(Mandatory = $true)][string[]]$Roots,
        [Parameter(Mandatory = $true)][datetime]$Since,
        [Parameter(Mandatory = $true)][string[]]$Extensions
    )

    $paths = New-Object System.Collections.Generic.List[string]
    foreach ($root in $Roots) {
        if (-not (Test-Path -LiteralPath $root)) {
            continue
        }

        Get-ChildItem -LiteralPath $root -Recurse -File -ErrorAction SilentlyContinue |
            Where-Object {
                $_.LastWriteTime -ge $Since.AddSeconds(-2) -and
                ($Extensions -contains $_.Extension.ToLowerInvariant())
            } |
            Sort-Object LastWriteTime, FullName |
            ForEach-Object {
                $paths.Add($_.FullName)
            }
    }

    return @($paths | Select-Object -Unique)
}

function Find-CreoPrtFiles {
    param(
        [Parameter(Mandatory = $true)][string[]]$Roots,
        [Parameter(Mandatory = $true)][datetime]$Since
    )

    $paths = New-Object System.Collections.Generic.List[string]
    foreach ($root in $Roots) {
        if (-not (Test-Path -LiteralPath $root)) {
            continue
        }

        foreach ($pattern in @("*.prt", "*.prt.*")) {
            Get-ChildItem -LiteralPath $root -Recurse -File -Filter $pattern -ErrorAction SilentlyContinue |
                Where-Object { $_.LastWriteTime -ge $Since.AddSeconds(-2) } |
                Sort-Object LastWriteTime, FullName |
                ForEach-Object {
                    $paths.Add($_.FullName)
                }
        }
    }

    return @($paths | Select-Object -Unique)
}

function Convert-ModelCheckCheck {
    param([Parameter(Mandatory = $true)]$Check)

    return [ordered]@{
        name = [string]$Check.name
        status = [string]$Check.stat
        description = [string]$Check.desc
        message = [string]$Check.msg
        answer = [string]$Check.ans
        item_count = @($Check.item).Count
    }
}

function Get-ModelCheckSummary {
    param([Parameter(Mandatory = $true)][string[]]$ReportFiles)

    $summary = [ordered]@{
        parsed = $false
        xml_path = ""
        parse_error = ""
        diagnostic_passed = $false
        pass_count = 0
        info_count = 0
        warning_count = 0
        error_count = 0
        key_checks = @()
        failed_checks = @()
        warning_checks = @()
        import_validation = [ordered]@{}
    }

    $xmlReports = @(
        $ReportFiles |
            Where-Object { [System.IO.Path]::GetExtension($_).ToLowerInvariant() -eq ".xml" } |
            Select-Object -First 1
    )
    if ($xmlReports.Count -eq 0) {
        return $summary
    }

    $summary.xml_path = $xmlReports[0]
    try {
        [xml]$doc = Get-Content -Raw -LiteralPath $summary.xml_path
        $checks = @($doc.mc_report.mc_checks.check)

        foreach ($group in ($checks | Group-Object stat)) {
            switch ($group.Name) {
                "PASS" { $summary.pass_count = $group.Count }
                "INFO" { $summary.info_count = $group.Count }
                "WARNING" { $summary.warning_count = $group.Count }
                "ERROR" { $summary.error_count = $group.Count }
            }
        }

        $summary.failed_checks = @(
            $checks |
                Where-Object { $_.stat -eq "ERROR" } |
                ForEach-Object { Convert-ModelCheckCheck -Check $_ }
        )
        $summary.warning_checks = @(
            $checks |
                Where-Object { $_.stat -eq "WARNING" } |
                ForEach-Object { Convert-ModelCheckCheck -Check $_ }
        )

        $keyNames = @(
            "GEOM_CHECKS",
            "SHORT_EDGES",
            "IMPORT_FEAT",
            "ACCURACY_INFO",
            "PARAM_INFO",
            "SRF_EDGES"
        )
        $summary.key_checks = @(
            foreach ($keyName in $keyNames) {
                $check = $checks | Where-Object { $_.name -eq $keyName } | Select-Object -First 1
                if ($check) {
                    Convert-ModelCheckCheck -Check $check
                }
            }
        )

        $expectedImportValidationKeys = @(
            "PTC_VAL_IMP_SCORE",
            "PTC_VAL_IMP_PART_STATUS",
            "PTC_MP_VAL_IMP_AREA"
        )
        $paramInfo = $checks | Where-Object { $_.name -eq "PARAM_INFO" } | Select-Object -First 1
        foreach ($item in @($paramInfo.item)) {
            $key = [string]$item.info1
            if (-not $key) {
                continue
            }
            if (($expectedImportValidationKeys -contains $key) -or
                $key.StartsWith("PTC_VAL_IMP_") -or
                $key.StartsWith("PTC_MP_VAL_IMP_")) {
                $summary.import_validation[$key] = [string]$item.info2
            }
        }

        $summary.diagnostic_passed = ($summary.error_count -eq 0)
        $summary.parsed = $true
    } catch {
        $summary.parse_error = $_.Exception.Message
    }

    return $summary
}

function Write-ResultJson {
    param(
        [Parameter(Mandatory = $true)][System.Collections.IDictionary]$Result,
        [Parameter(Mandatory = $true)][string]$Path
    )

    $Result | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $Path -Encoding UTF8
}

$stepAbsolute = Convert-ToRepoAbsolutePath -PathValue $StepPath
if (-not (Test-Path -LiteralPath $stepAbsolute)) {
    throw "StepPath does not exist: $stepAbsolute"
}

if (-not $OutputDir) {
    $OutputDir = Join-Path $repoRoot "data\baseline_runs\creo_step_diagnostic"
} else {
    $OutputDir = Convert-ToRepoAbsolutePath -PathValue $OutputDir
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

$stageDir = Join-Path $OutputDir "stage"
$importOutputDir = Join-Path $OutputDir "step_import"
$modelCheckOutputDir = Join-Path $OutputDir "modelcheck"
New-Item -ItemType Directory -Force -Path $stageDir | Out-Null
New-Item -ItemType Directory -Force -Path $importOutputDir | Out-Null
New-Item -ItemType Directory -Force -Path $modelCheckOutputDir | Out-Null
$env:MC_BATCH_REPORT_DIR = $modelCheckOutputDir

$resultPath = Join-Path $OutputDir "creo_step_diagnostic_result.json"
$startedAt = Get-Date
$result = [ordered]@{
    captured = $true
    diagnostic_tool = "Creo Distributed Batch + ModelCHECK"
    status = "Started"
    success = $false
    step_input_original = $stepAbsolute
    step_input_staged = ""
    creo_root = ""
    dbatchc_path = ""
    dxc_object_syntax = $DxcObjectSyntax
    output_dir = $OutputDir
    result_json = $resultPath
    import = [ordered]@{
        attempted = $false
        dxc_path = ""
        command = $null
        generated_prt = $false
        prt_path = ""
        generated_prt_candidates = @()
    }
    modelcheck = [ordered]@{
        attempted = $false
        skipped_reason = ""
        dxc_path = ""
        command = $null
        report_files = @()
        diagnostic_passed = $false
        summary = [ordered]@{
            parsed = $false
            xml_path = ""
            parse_error = ""
            diagnostic_passed = $false
            pass_count = 0
            info_count = 0
            warning_count = 0
            error_count = 0
            key_checks = @()
            failed_checks = @()
            warning_checks = @()
            import_validation = [ordered]@{}
        }
    }
    notes = @()
}

try {
    $resolvedCreoRoot = Find-CreoRoot -PreferredRoot $CreoRoot
    Set-CreoBatchEnvironment -Root $resolvedCreoRoot

    $commonFiles = Join-Path $resolvedCreoRoot "Common Files"
    $dbatchc = Join-Path $commonFiles "x86e_win64\obj\dbatchc.exe"
    $stepTtd = Join-Path $commonFiles "text\ttds\step_3d_import.ttd"
    $modelCheckTtd = Join-Path $commonFiles "text\ttds\modelcheck.ttd"

    $result.creo_root = $resolvedCreoRoot
    $result.dbatchc_path = $dbatchc

    $stagedStep = Join-Path $stageDir ("input" + [System.IO.Path]::GetExtension($stepAbsolute).ToLowerInvariant())
    Copy-Item -LiteralPath $stepAbsolute -Destination $stagedStep -Force
    $result.step_input_staged = $stagedStep

    $importDxc = Join-Path $stageDir "step_import.dxc"
    New-DxcFile `
        -Path $importDxc `
        -TtdPath $stepTtd `
        -TaskName "STEP 3D Import" `
        -ObjectPath $stagedStep `
        -TaskOutputDir $importOutputDir `
        -Syntax $DxcObjectSyntax

    $result.import.attempted = $true
    $result.import.dxc_path = $importDxc
    $result.import.command = Invoke-ProcessWithTimeout `
        -FilePath $dbatchc `
        -Arguments @("-nographics", "-process", $importDxc) `
        -WorkingDirectory $OutputDir `
        -LogPrefix "step_import" `
        -Timeout $TimeoutSeconds

    $prtCandidates = @(Find-CreoPrtFiles `
        -Roots @($OutputDir, $stageDir, $importOutputDir) `
        -Since $startedAt)
    $result.import.generated_prt_candidates = @($prtCandidates)
    if ($prtCandidates.Count -gt 0) {
        $result.import.generated_prt = $true
        $result.import.prt_path = $prtCandidates[0]
    }

    if ($result.import.command.timed_out) {
        $result.status = "CreoStepImportTimedOut"
        $result.notes += "STEP import did not finish before TimeoutSeconds."
    } elseif (-not $result.import.generated_prt) {
        $result.status = "CreoStepImportNoPrt"
        $result.notes += "STEP import finished or returned, but no .prt was found in the diagnostic output roots."
    } elseif ($SkipModelCheck) {
        $result.status = "CreoStepImportOnly"
        $result.success = $true
        $result.modelcheck.skipped_reason = "SkipModelCheck"
    } else {
        $modelCheckDxc = Join-Path $stageDir "modelcheck.dxc"
        New-DxcFile `
            -Path $modelCheckDxc `
            -TtdPath $modelCheckTtd `
            -TaskName "ModelCHECK" `
            -ObjectPath $result.import.prt_path `
            -TaskOutputDir $modelCheckOutputDir `
            -Syntax $DxcObjectSyntax

        $result.modelcheck.attempted = $true
        $result.modelcheck.dxc_path = $modelCheckDxc
        $result.modelcheck.command = Invoke-ProcessWithTimeout `
            -FilePath $dbatchc `
            -Arguments @("-nographics", "-process", $modelCheckDxc) `
            -WorkingDirectory $OutputDir `
            -LogPrefix "modelcheck" `
            -Timeout $TimeoutSeconds

        $reportFiles = Find-NewFiles `
            -Roots @($OutputDir, $modelCheckOutputDir) `
            -Since $startedAt `
            -Extensions @(".html", ".htm", ".xml", ".txt", ".csv", ".log")
        $result.modelcheck.report_files = @(
            $reportFiles |
                Where-Object {
                    $_ -ne (Join-Path $OutputDir "step_import.stdout.txt") -and
                    $_ -ne (Join-Path $OutputDir "step_import.stderr.txt") -and
                    $_ -ne (Join-Path $OutputDir "modelcheck.stdout.txt") -and
                    $_ -ne (Join-Path $OutputDir "modelcheck.stderr.txt")
                }
        )

        if ($result.modelcheck.command.timed_out) {
            $result.status = "CreoModelCheckTimedOut"
            $result.notes += "ModelCHECK did not finish before TimeoutSeconds."
        } elseif ($result.modelcheck.report_files.Count -eq 0) {
            $result.status = "CreoModelCheckNoReport"
            $result.notes += "ModelCHECK returned, but no report-like file was found in the diagnostic output roots."
        } else {
            $result.modelcheck.summary = Get-ModelCheckSummary -ReportFiles $result.modelcheck.report_files
            $result.modelcheck.diagnostic_passed = [bool]$result.modelcheck.summary.diagnostic_passed
            if ($result.modelcheck.summary.parsed -and -not $result.modelcheck.diagnostic_passed) {
                $result.notes += "ModelCHECK report was generated, but diagnostic_passed=false. Inspect modelcheck.summary for failing checks."
            }
            $result.status = "CreoModelCheckReportReady"
            $result.success = $true
        }
    }
} catch {
    $result.status = "CreoStepDiagnosticError"
    $result.error = $_.Exception.Message
    $result.notes += "The runner failed before completing the configured diagnostic chain."
    Write-ResultJson -Result $result -Path $resultPath
    throw
}

Write-ResultJson -Result $result -Path $resultPath

Write-Host "Creo STEP diagnostic status: $($result.status)"
Write-Host "Result JSON: $resultPath"
if ($result.import.prt_path) {
    Write-Host "Imported PRT: $($result.import.prt_path)"
}
if ($result.modelcheck.report_files.Count -gt 0) {
    Write-Host "ModelCHECK report files:"
    $result.modelcheck.report_files | ForEach-Object { Write-Host "  $_" }
}

if (-not $result.success) {
    exit 1
}
