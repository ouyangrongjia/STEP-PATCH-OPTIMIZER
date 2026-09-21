param(
    [Parameter(Mandatory=$true)][string]$PythonExe,
    [Parameter(Mandatory=$true)][string]$SourceStep,
    [Parameter(Mandatory=$true)][string]$BaselineDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$CreoRoot=''
)
$ErrorActionPreference='Stop'
$repoRoot=Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location -LiteralPath $repoRoot
foreach ($name in @('PythonExe','SourceStep','BaselineDirectory','OutputDirectory')) {
    $value=Get-Variable -Name $name -ValueOnly
    Set-Variable -Name $name -Value ([System.IO.Path]::GetFullPath($value))
}
if (Test-Path -LiteralPath (Join-Path $OutputDirectory 'reference.npz')) { throw 'Use a new output directory; existing experiment inputs are immutable.' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
function Invoke-ResearchStage {
    param([string]$Name,[string]$Script,[string[]]$StageArguments)
    & $PythonExe (Join-Path $PSScriptRoot $Script) @StageArguments > (Join-Path $OutputDirectory "$Name.log") 2>&1
    if ($LASTEXITCODE -ne 0) { throw "$Name failed; see $OutputDirectory/$Name.log" }
}
Invoke-ResearchStage 'tests' 'test_numerics.py' @()
Invoke-ResearchStage 'prepare' 'prepare.py' @('--source',$SourceStep,'--baseline',$BaselineDirectory,'--output',$OutputDirectory)
Invoke-ResearchStage 'baseline_audit' 'baseline_audit.py' @('--run',$OutputDirectory,'--baseline',$BaselineDirectory)
Invoke-ResearchStage 'fit' 'fit_graph.py' @('--run',$OutputDirectory,'--baseline',$BaselineDirectory)
Invoke-ResearchStage 'build_initial' 'build_faces.py' @('--run',$OutputDirectory,'--baseline',$BaselineDirectory)
Invoke-ResearchStage 'refine' 'refine_extrapolation.py' @('--run',$OutputDirectory)
Invoke-ResearchStage 'build_faces' 'build_faces.py' @('--run',$OutputDirectory,'--baseline',$BaselineDirectory)
foreach ($name in @('g17_data','g17_outer')) {
    Invoke-ResearchStage "crop_$name" 'crop_supports.py' @('--run',$OutputDirectory,'--name',$name)
}
Invoke-ResearchStage 'evaluate' 'evaluate.py' @('--run',$OutputDirectory,'--baseline',$BaselineDirectory)
Invoke-ResearchStage 'diagnose' 'diagnose.py' @('--run',$OutputDirectory,'--baseline',$BaselineDirectory)
Invoke-ResearchStage 'boundary_audit' 'boundary_audit.py' @('--source',$SourceStep,'--run',$OutputDirectory,'--baseline',$BaselineDirectory)
Invoke-ResearchStage 'partition' 'partition_audit.py' @('--run',$OutputDirectory)
Invoke-ResearchStage 'integrate' 'integrate.py' @('--source',$SourceStep,'--run',$OutputDirectory,'--names','g17_data','g25_strict_outer','g17_outer_cropped')
foreach ($name in @('g17_data_cropped','g17_outer_cropped')) {
    Invoke-ResearchStage "render_$name" 'render.py' @('--run',$OutputDirectory,'--baseline',$BaselineDirectory,'--candidate',$name,'--label',"$name : 4 faces / 446 poles")
}
Invoke-ResearchStage 'plots' 'plots.py' @('--run',$OutputDirectory)
if ($CreoRoot) {
    foreach ($name in @('g17_data','g17_outer_cropped')) {
        & (Join-Path $PSScriptRoot 'run_creo.ps1') -StepPath (Join-Path $OutputDirectory "$name/whole_model_diagnostic.step") -OutputDir (Join-Path $OutputDirectory "$name/creo") -CreoRoot $CreoRoot
    }
}
Invoke-ResearchStage 'summarize' 'summarize.py' @('--run',$OutputDirectory,'--baseline',$BaselineDirectory)
Write-Output "Completed research diagnostics: $OutputDirectory/RUN.md"
