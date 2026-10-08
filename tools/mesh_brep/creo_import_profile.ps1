param(
    [Parameter(Mandatory=$true)][string]$Step,
    [Parameter(Mandatory=$true)][string]$OutputDir,
    [Parameter(Mandatory=$true)][string]$CreoRoot,
    [Parameter(Mandatory=$true)][string]$Profile,
    [int]$TimeoutSeconds=240
)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$tokens=$null; $errors=$null
$ast=[System.Management.Automation.Language.Parser]::ParseFile((Join-Path $root 'scripts/run_creo_step_diagnostic.ps1'),[ref]$tokens,[ref]$errors)
if ($errors.Count) { throw 'Cannot parse existing Creo runner' }
foreach ($definition in $ast.FindAll({param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst]},$false)) {
    Invoke-Expression $definition.Extent.Text
}
$OutputDir=[IO.Path]::GetFullPath($OutputDir)
$Step=[IO.Path]::GetFullPath($Step)
$Profile=[IO.Path]::GetFullPath($Profile)
if (Test-Path -LiteralPath $OutputDir) { throw 'Refusing to overwrite an import audit' }
New-Item -ItemType Directory -Path $OutputDir | Out-Null
Copy-Item -LiteralPath $Profile -Destination (Join-Path $OutputDir 'profile.dip')
$profileCopy=Join-Path $OutputDir 'profile.dip'
Set-CreoBatchEnvironment $CreoRoot
$env:PTC_WF_ROOT=Join-Path $OutputDir 'dsm_cache'
$common=Join-Path $CreoRoot 'Common Files'
$import=Join-Path $OutputDir 'import'; New-Item -ItemType Directory -Path $import | Out-Null
$dxc=Join-Path $OutputDir 'import.dxc'
New-DxcFile -Path $dxc -TtdPath (Join-Path $common 'text/ttds/step_3d_import.ttd') -TaskName 'STEP 3D Import' -ObjectPath $Step -TaskOutputDir $import -Syntax 'ObjectText'
[xml]$task=Get-Content -LiteralPath $dxc -Raw
foreach ($file in @($profileCopy)) {
    $node=$task.CreateElement('ConfigFile'); $node.InnerText=$file
    [void]$task.DXC.Group.AppendChild($node)
}
$task.Save($dxc)
$result=Invoke-ProcessWithTimeout -FilePath (Join-Path $common 'x86e_win64/obj/dbatchc.exe') -Arguments @('-nographics','-process',$dxc) -WorkingDirectory $OutputDir -LogPrefix 'import' -Timeout $TimeoutSeconds -LogDirectory $OutputDir
$result | ConvertTo-Json -Depth 10 | Set-Content -Encoding utf8 -LiteralPath (Join-Path $OutputDir 'import_process.json')
if ($result.timed_out -or ($null -ne $result.exit_code -and $result.exit_code -ne 0)) { throw 'Creo import process failed' }
$part=Get-ChildItem -LiteralPath $import -Filter '*.prt.*' | Select-Object -First 1
if (-not $part) { throw 'No Creo part was generated' }
$checks=Join-Path $OutputDir 'modelcheck'; New-Item -ItemType Directory -Path $checks | Out-Null
$dxc=Join-Path $OutputDir 'modelcheck.dxc'
New-DxcFile -Path $dxc -TtdPath (Join-Path $common 'text/ttds/modelcheck.ttd') -TaskName 'ModelCHECK' -ObjectPath $part.FullName -TaskOutputDir $checks -Syntax 'ObjectText'
$result=Invoke-ProcessWithTimeout -FilePath (Join-Path $common 'x86e_win64/obj/dbatchc.exe') -Arguments @('-nographics','-process',$dxc) -WorkingDirectory $OutputDir -LogPrefix 'modelcheck' -Timeout $TimeoutSeconds -LogDirectory $OutputDir
$result | ConvertTo-Json -Depth 10 | Set-Content -Encoding utf8 -LiteralPath (Join-Path $OutputDir 'modelcheck_process.json')
if ($result.timed_out -or ($null -ne $result.exit_code -and $result.exit_code -ne 0)) { throw 'Creo ModelCHECK process failed' }
if (-not (Get-ChildItem -LiteralPath $checks -Filter '*.p.xml' -File)) { throw 'Creo produced no ModelCHECK report' }
