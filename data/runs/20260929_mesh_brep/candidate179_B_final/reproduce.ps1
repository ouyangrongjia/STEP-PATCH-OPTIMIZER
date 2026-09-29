# Delivery template: report_compact.py places this beside the input directory.
param(
    [string]$Python='python',
    [string]$CodeRoot='',
    [string]$OutputDir='',
    [switch]$Assemble
)
$ErrorActionPreference='Stop'
if (-not $CodeRoot) { $CodeRoot=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../../../..')).Path }
if (-not $OutputDir) { $OutputDir=Join-Path $PSScriptRoot 'reproduced' }
$OutputDir=[System.IO.Path]::GetFullPath($OutputDir)
if (Test-Path -LiteralPath $OutputDir) { throw 'Output directory already exists; choose a new -OutputDir' }
$stl=Join-Path $PSScriptRoot 'input/input.stl'
$layout=Join-Path $PSScriptRoot 'input/layout.json'
Push-Location -LiteralPath $CodeRoot
try {
    & $Python -B -m tools.mesh_brep.compact --stl $stl --layout $layout --output (Join-Path $OutputDir 'mesh')
    if ($LASTEXITCODE -ne 0) { throw 'Mesh-only reconstruction failed' }
    if ($Assemble) {
        $reference=Join-Path $CodeRoot 'data/runs/20260929_mesh_brep/baseline_cold'
        & $Python -B -m tools.mesh_brep.attach_boundary --mesh-result (Join-Path $OutputDir 'mesh') --region (Join-Path $reference 'original_region.step') --output (Join-Path $OutputDir 'interface')
        if ($LASTEXITCODE -ne 0) { throw 'CAD boundary attachment failed' }
        & $Python -B -m tools.mesh_brep.assemble_candidate --audit (Join-Path $reference 'input_audit.json') --patch (Join-Path $OutputDir 'interface/network.step') --output (Join-Path $OutputDir 'whole')
        if ($LASTEXITCODE -ne 0) { throw 'Whole-model assembly failed' }
    }
} finally { Pop-Location }
