function ConvertTo-SpoRunToken {
    param(
        [AllowEmptyString()][string]$Value,
        [Parameter(Mandatory = $true)][string]$Fallback
    )

    $token = if ($Value) { $Value.Trim() } else { "" }
    $token = [regex]::Replace($token, '[<>:"/\\|?*\x00-\x1F]', '_')
    $token = [regex]::Replace($token, '\s+', '_')
    $token = [regex]::Replace($token, '_{2,}', '_')
    $token = $token.Trim([char[]]" ._")
    if (-not $token) {
        return $Fallback
    }
    return $token
}

function Format-SpoCandidateToken {
    param([AllowEmptyString()][string]$CandidateId)

    if (-not $CandidateId -or $CandidateId -eq "na" -or $CandidateId -eq "none") {
        return "cand-na"
    }
    if ($CandidateId -eq "auto") {
        return "cand-auto"
    }

    $numericId = 0
    if ([int]::TryParse($CandidateId, [ref]$numericId)) {
        return "cand-{0:D4}" -f $numericId
    }

    return "cand-$(ConvertTo-SpoRunToken -Value $CandidateId -Fallback 'unknown')"
}

function ConvertTo-SpoRunParameters {
    param([System.Collections.IDictionary]$Parameters)

    $result = [ordered]@{}
    if (-not $Parameters) {
        return $result
    }

    foreach ($key in $Parameters.Keys) {
        $value = $Parameters[$key]
        if ($value -is [System.Management.Automation.SwitchParameter]) {
            $result[[string]$key] = [bool]$value
        } elseif ($value -is [System.Collections.IEnumerable] -and $value -isnot [string]) {
            $result[[string]$key] = @($value)
        } else {
            $result[[string]$key] = $value
        }
    }
    return $result
}

function Get-SpoEffectiveScriptParameters {
    param([Parameter(Mandatory = $true)][string[]]$ParameterNames)

    $result = [ordered]@{}
    foreach ($name in $ParameterNames) {
        $variable = Get-Variable -Name $name -Scope 1 -ErrorAction SilentlyContinue
        if ($variable) {
            $result[$name] = $variable.Value
        }
    }
    return $result
}

function Get-SpoGitSnapshot {
    param([Parameter(Mandatory = $true)][string]$RepoRoot)

    $commit = ""
    $branch = ""
    $dirty = $false
    try {
        $commit = [string]((& git -C $RepoRoot rev-parse HEAD 2>$null | Select-Object -First 1))
        $branch = [string]((& git -C $RepoRoot branch --show-current 2>$null | Select-Object -First 1))
        $dirty = @(& git -C $RepoRoot status --porcelain 2>$null).Count -gt 0
    } catch {
        # A missing Git executable should not prevent a diagnostic run.
    }

    return [ordered]@{
        commit = $commit.Trim()
        branch = $branch.Trim()
        dirty = $dirty
    }
}

function Get-SpoSourceSnapshot {
    param([AllowEmptyString()][string]$SourcePath)

    $absolutePath = ""
    $exists = $false
    $sha256 = ""
    if ($SourcePath) {
        $absolutePath = [System.IO.Path]::GetFullPath($SourcePath)
        $exists = Test-Path -LiteralPath $absolutePath -PathType Leaf
        if ($exists) {
            $stream = [System.IO.File]::OpenRead($absolutePath)
            try {
                $hasher = [System.Security.Cryptography.SHA256]::Create()
                try {
                    $hashBytes = $hasher.ComputeHash($stream)
                    $sha256 = ([System.BitConverter]::ToString($hashBytes)).Replace("-", "")
                } finally {
                    $hasher.Dispose()
                }
            } finally {
                $stream.Dispose()
            }
        }
    }

    return [ordered]@{
        path = $absolutePath
        exists = $exists
        sha256 = $sha256
    }
}

function Write-SpoRunFiles {
    param([Parameter(Mandatory = $true)]$Context)

    $runJsonPath = Join-Path $Context.output_dir "run.json"
    $runMarkdownPath = Join-Path $Context.output_dir "RUN.md"
    $Context | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $runJsonPath -Encoding UTF8

    $lines = @(
        "# Run $($Context.run_id)",
        "",
        "| Field | Value |",
        "|---|---|",
        "| status | $($Context.status) |",
        "| workflow | $($Context.workflow) |",
        "| model | $($Context.model) |",
        "| candidate | $($Context.candidate_token) |",
        "| resolved_candidate | $($Context.resolved_candidate_token) |",
        "| variant | $($Context.variant) |",
        "| started_at | $($Context.started_at) |",
        "| ended_at | $($Context.ended_at) |",
        "| git_commit | $($Context.git.commit) |",
        "| git_branch | $($Context.git.branch) |",
        "| git_dirty | $($Context.git.dirty) |",
        "| source | $($Context.source.path) |",
        "| source_sha256 | $($Context.source.sha256) |",
        "| script | $($Context.script_path) |",
        "",
        "## Directories",
        "",
        "- artifacts: $($Context.directories.artifacts)",
        "- diagnostics: $($Context.directories.diagnostics)",
        "- logs: $($Context.directories.logs)",
        "",
        "## Message",
        "",
        "$($Context.message)",
        "",
        "## Artifacts",
        ""
    )

    if ($Context.artifacts.Count -eq 0) {
        $lines += "No final artifacts have been registered yet."
    } else {
        foreach ($key in $Context.artifacts.Keys) {
            $lines += "- ${key}: $($Context.artifacts[$key])"
        }
    }

    $lines += @(
        "",
        "## Parameters",
        "",
        '```json',
        ($Context.parameters | ConvertTo-Json -Depth 10),
        '```'
    )
    $lines | Set-Content -LiteralPath $runMarkdownPath -Encoding UTF8
}

function New-SpoRunContext {
    param(
        [Parameter(Mandatory = $true)][string]$RepoRoot,
        [Parameter(Mandatory = $true)][string]$Workflow,
        [AllowEmptyString()][string]$SourcePath = "",
        [AllowEmptyString()][string]$CandidateId = "na",
        [AllowEmptyString()][string]$Variant = "default",
        [AllowEmptyString()][string]$OutputDir = "",
        [AllowEmptyString()][string]$DefaultRunRoot = "",
        [Parameter(Mandatory = $true)][string]$ScriptPath,
        [System.Collections.IDictionary]$Parameters
    )

    $workflowToken = ConvertTo-SpoRunToken -Value $Workflow -Fallback "run"
    $modelName = if ($SourcePath) {
        [System.IO.Path]::GetFileNameWithoutExtension($SourcePath)
    } else {
        "repository"
    }
    $modelToken = ConvertTo-SpoRunToken -Value $modelName -Fallback "model"
    $candidateToken = Format-SpoCandidateToken -CandidateId $CandidateId
    $variantToken = ConvertTo-SpoRunToken -Value $Variant -Fallback "default"
    $timestamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
    $runId = "${timestamp}__${workflowToken}__${modelToken}__${candidateToken}__${variantToken}"

    if ($OutputDir) {
        $resolvedOutputDir = if ([System.IO.Path]::IsPathRooted($OutputDir)) {
            [System.IO.Path]::GetFullPath($OutputDir)
        } else {
            [System.IO.Path]::GetFullPath((Join-Path $RepoRoot $OutputDir))
        }
    } elseif ($DefaultRunRoot) {
        $resolvedOutputDir = Join-Path (
            [System.IO.Path]::GetFullPath($DefaultRunRoot)) $runId
    } else {
        $resolvedOutputDir = Join-Path $RepoRoot "data\runs\$runId"
    }

    $existingRunManifest = Join-Path $resolvedOutputDir "run.json"
    if (Test-Path -LiteralPath $existingRunManifest) {
        throw "Run directory already contains run.json; choose a new -OutputDir: $resolvedOutputDir"
    }

    $artifactsDir = Join-Path $resolvedOutputDir "artifacts"
    $diagnosticsDir = Join-Path $resolvedOutputDir "diagnostics"
    $logsDir = Join-Path $resolvedOutputDir "logs"
    foreach ($directory in @($resolvedOutputDir, $artifactsDir, $diagnosticsDir, $logsDir)) {
        New-Item -ItemType Directory -Force -Path $directory | Out-Null
    }

    $context = [pscustomobject]([ordered]@{
        schema_version = 1
        run_id = $runId
        workflow = $workflowToken
        model = $modelToken
        candidate_id = $CandidateId
        candidate_token = $candidateToken
        resolved_candidate_id = if ($candidateToken -eq "cand-auto") { "" } else { $CandidateId }
        resolved_candidate_token = if ($candidateToken -eq "cand-auto") { "" } else { $candidateToken }
        variant = $variantToken
        status = "running"
        message = "Run started."
        started_at = (Get-Date).ToString("o")
        ended_at = ""
        duration_ms = $null
        output_dir = $resolvedOutputDir
        script_path = [System.IO.Path]::GetFullPath($ScriptPath)
        git = Get-SpoGitSnapshot -RepoRoot $RepoRoot
        source = Get-SpoSourceSnapshot -SourcePath $SourcePath
        parameters = ConvertTo-SpoRunParameters -Parameters $Parameters
        directories = [ordered]@{
            artifacts = $artifactsDir
            diagnostics = $diagnosticsDir
            logs = $logsDir
        }
        stages = [ordered]@{}
        artifacts = [ordered]@{}
    })
    Write-SpoRunFiles -Context $context
    return $context
}

function Complete-SpoRun {
    param(
        [Parameter(Mandatory = $true)]$Context,
        [Parameter(Mandatory = $true)][string]$Status,
        [AllowEmptyString()][string]$Message = "",
        [AllowEmptyString()][string]$ResolvedCandidateId = "",
        [System.Collections.IDictionary]$Artifacts,
        [System.Collections.IDictionary]$Stages
    )

    $endedAt = Get-Date
    $startedAt = [DateTimeOffset]::Parse($Context.started_at)
    $Context.status = $Status
    $Context.message = $Message
    $Context.ended_at = $endedAt.ToString("o")
    $Context.duration_ms = [long](($endedAt - $startedAt.LocalDateTime).TotalMilliseconds)

    if ($ResolvedCandidateId) {
        $Context.resolved_candidate_id = $ResolvedCandidateId
        $Context.resolved_candidate_token = Format-SpoCandidateToken -CandidateId $ResolvedCandidateId
    }

    if ($Artifacts) {
        foreach ($key in $Artifacts.Keys) {
            $Context.artifacts[[string]$key] = $Artifacts[$key]
        }
    }
    if ($Stages) {
        foreach ($key in $Stages.Keys) {
            $Context.stages[[string]$key] = $Stages[$key]
        }
    }
    Write-SpoRunFiles -Context $Context
}
