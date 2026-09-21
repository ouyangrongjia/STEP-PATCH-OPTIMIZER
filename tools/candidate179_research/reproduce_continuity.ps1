param(
    [Parameter(Mandatory=$true)][string]$Python,
    [Parameter(Mandatory=$true)][string]$SourceStep,
    [Parameter(Mandatory=$true)][string]$OutputDir,
    [string]$ReferenceRun='data/runs/20260921_candidate179_continuity',
    [string]$CreoRoot=''
)
$ErrorActionPreference='Stop'
$repoRoot=Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Push-Location $repoRoot
try {
    $reference=(Resolve-Path -LiteralPath $ReferenceRun).Path
    $source=(Resolve-Path -LiteralPath $SourceStep).Path
    $out=if([IO.Path]::IsPathRooted($OutputDir)) { [IO.Path]::GetFullPath($OutputDir) } else { [IO.Path]::GetFullPath((Join-Path $repoRoot $OutputDir)) }
    if(Test-Path -LiteralPath $out) { throw 'Output directory exists; choose a new run directory.' }
    New-Item -ItemType Directory -Path $out | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $out 'control') | Out-Null
    foreach($name in @('protocol.json','reference.npz','original_region.step','input_audit.json','dependencies.json')) {
        Copy-Item -LiteralPath (Join-Path $reference $name) -Destination (Join-Path $out $name)
    }
    foreach($name in @('patch_faces.step','support.step','whole_model_diagnostic.step','fit.json')) {
        Copy-Item -LiteralPath (Join-Path $reference "control/$name") -Destination (Join-Path $out "control/$name")
    }
    $audit=Get-Content -Raw (Join-Path $out 'input_audit.json')|ConvertFrom-Json
    $audit.source=$source
    $audit|ConvertTo-Json -Depth 30|Set-Content -LiteralPath (Join-Path $out 'input_audit.json') -Encoding utf8
    $versionCode='import json,sys,platform; from pathlib import Path; from importlib.metadata import version; p=Path(sys.argv[1]); keys=json.loads((p/"dependencies.json").read_text(encoding="utf-8-sig")); (p/"dependencies.json").write_text(json.dumps({k:version(k) for k in keys},indent=2),encoding="utf-8"); (p/"runtime.json").write_text(json.dumps(dict(python=sys.version,executable=sys.executable,platform=platform.platform()),indent=2),encoding="utf-8")'
    & $Python -c $versionCode $out
    if($LASTEXITCODE -ne 0) { throw 'Required experiment dependencies are unavailable in the selected Python.' }
    @{reproduction_of=$reference;source=$source;code_revision=(& git rev-parse HEAD);branch=(& git branch --show-current);started_at=(Get-Date -Format o)} |
        ConvertTo-Json|Set-Content -LiteralPath (Join-Path $out 'source_state.json') -Encoding utf8
    function Invoke-Research([string]$Script,[string[]]$Extra=@()) {
        & $Python (Join-Path $PSScriptRoot $Script) --run $out @Extra *> (Join-Path $out ($Script.Replace('.py','.log')))
        if($LASTEXITCODE -ne 0) { throw "Research command failed: $Script; see its log in $out" }
    }
    Invoke-Research 'continuity_corners.py'
    Invoke-Research 'continuity_diagnose.py'
    Invoke-Research 'continuity_fit.py'
    Invoke-Research 'designer_audit.py'
    Invoke-Research 'continuity_attribution.py'
    Invoke-Research 'integrate.py' @('--source',$source,'--names','aligned_inner_cubic','linear_xy_soft','--patch-file','patch_faces.step')
    Invoke-Research 'integrated_boundary_audit.py' @('--names','control','aligned_inner_cubic','linear_xy_soft')
    Invoke-Research 'continuity_validate.py' @('--stage','geometry')
    Invoke-Research 'continuity_validate.py' @('--stage','confirmation')
    Invoke-Research 'continuity_reverse_confirmation.py'
    Invoke-Research 'continuity_local.py'
    Invoke-Research 'continuity_local.py' @('--confirmed-worst')
    if($CreoRoot) {
        $cases=@(@{step=$source;folder='original_control/creo'},
            @{step=(Join-Path $out 'control/whole_model_diagnostic.step');folder='control/creo'},
            @{step=(Join-Path $out 'aligned_inner_cubic/whole_model_diagnostic.step');folder='aligned_inner_cubic/creo'})
        foreach($case in $cases) {
            $folder=Join-Path $out $case.folder
            & (Join-Path $PSScriptRoot 'run_creo.ps1') -StepPath $case.step -OutputDir $folder -CreoRoot $CreoRoot
            $result=Get-Content -Raw (Join-Path $folder 'result.json')|ConvertFrom-Json
            if($result.status -ne 'ReportReady') { throw "Creo execution failed in $folder" }
            # A valid negative ModelCHECK report is evidence, not a runner error.
        }
    }
    Invoke-Research 'continuity_render.py'
    Invoke-Research 'continuity_collect.py'
    Write-Output "Reproduction completed in $out; examine run.json for actual acceptance status."
} finally { Pop-Location }
