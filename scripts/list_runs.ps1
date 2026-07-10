param(
    [string]$Root = "",
    [string]$Workflow = "",
    [string]$Model = "",
    [string]$CandidateId = "",
    [string]$Status = "",
    [int]$Latest = 0,
    [switch]$Json
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "run_artifacts.ps1")

if (-not $Root) {
    $Root = Join-Path $repoRoot "data\runs"
} elseif (-not [System.IO.Path]::IsPathRooted($Root)) {
    $Root = Join-Path $repoRoot $Root
}
$Root = [System.IO.Path]::GetFullPath($Root)

if (-not (Test-Path -LiteralPath $Root)) {
    Write-Host "No run directory exists: $Root"
    exit 0
}

$candidateToken = if ($CandidateId) { Format-SpoCandidateToken -CandidateId $CandidateId } else { "" }
$runs = @(
    Get-ChildItem -LiteralPath $Root -Recurse -File -Filter "run.json" -ErrorAction SilentlyContinue |
        ForEach-Object {
            try {
                Get-Content -Raw -LiteralPath $_.FullName | ConvertFrom-Json
            } catch {
                Write-Warning "Skipping invalid run manifest: $($_.FullName)"
            }
        } |
        Where-Object {
            (-not $Workflow -or $_.workflow -like "*$Workflow*") -and
            (-not $Model -or $_.model -like "*$Model*") -and
            (-not $candidateToken -or $_.candidate_token -eq $candidateToken -or $_.resolved_candidate_token -eq $candidateToken) -and
            (-not $Status -or $_.status -like "*$Status*")
        } |
        Sort-Object { [DateTimeOffset]::Parse($_.started_at) } -Descending
)

if ($Latest -gt 0) {
    $runs = @($runs | Select-Object -First $Latest)
}

if ($Json) {
    ConvertTo-Json -InputObject @($runs) -Depth 20
    exit 0
}

$runs |
    Select-Object `
        @{Name = "StartedAt"; Expression = { $_.started_at }},
        @{Name = "Status"; Expression = { $_.status }},
        @{Name = "Workflow"; Expression = { $_.workflow }},
        @{Name = "Model"; Expression = { $_.model }},
        @{Name = "Candidate"; Expression = { if ($_.resolved_candidate_token) { $_.resolved_candidate_token } else { $_.candidate_token } }},
        @{Name = "Variant"; Expression = { $_.variant }},
        @{Name = "Git"; Expression = { if ($_.git.commit) { $_.git.commit.Substring(0, [Math]::Min(8, $_.git.commit.Length)) } else { "" } }},
        @{Name = "OutputDir"; Expression = { $_.output_dir }} |
    Format-Table -AutoSize
