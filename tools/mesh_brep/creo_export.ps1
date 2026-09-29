param(
    [Parameter(Mandatory=$true)][string]$Part,
    [Parameter(Mandatory=$true)][string]$OutputDir,
    [Parameter(Mandatory=$true)][string]$CreoRoot,
    [int]$TimeoutSeconds=240
)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$tokens=$null; $errors=$null
$ast=[System.Management.Automation.Language.Parser]::ParseFile((Join-Path $root 'scripts/run_creo_step_diagnostic.ps1'),[ref]$tokens,[ref]$errors)
if ($errors.Count) { throw 'Cannot parse the existing Creo runner' }
foreach ($definition in $ast.FindAll({param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst]},$false)) {
    Invoke-Expression $definition.Extent.Text
}
$OutputDir=[System.IO.Path]::GetFullPath($OutputDir)
if (Test-Path -LiteralPath $OutputDir) { throw 'Refusing to overwrite an export audit' }
New-Item -ItemType Directory -Path $OutputDir | Out-Null
Set-CreoBatchEnvironment $CreoRoot
$common=Join-Path $CreoRoot 'Common Files'
$dxc=Join-Path $OutputDir 'export.dxc'
New-DxcFile -Path $dxc -TtdPath (Join-Path $common 'text/ttds/step_3d_export.ttd') -TaskName 'STEP 3D Export' -ObjectPath ([System.IO.Path]::GetFullPath($Part)) -TaskOutputDir $OutputDir -Syntax 'ObjectText'
$result=Invoke-ProcessWithTimeout -FilePath (Join-Path $common 'x86e_win64/obj/dbatchc.exe') -Arguments @('-nographics','-process',$dxc) -WorkingDirectory $OutputDir -LogPrefix 'export' -Timeout $TimeoutSeconds -LogDirectory $OutputDir
$result | ConvertTo-Json -Depth 10 | Set-Content -Encoding utf8 -LiteralPath (Join-Path $OutputDir 'export.json')
if ($result.timed_out -or $result.exit_code -ne 0) { throw 'Creo STEP export failed' }
