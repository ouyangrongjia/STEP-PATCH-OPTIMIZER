param(
    [Parameter(Mandatory=$true)][string]$StepPath,
    [Parameter(Mandatory=$true)][string]$OutputDir,
    [Parameter(Mandatory=$true)][string]$CreoRoot,
    [int]$TimeoutSeconds=240,
    [string]$ExistingImportDirectory=''
)
$ErrorActionPreference='Stop'
$repoRoot=Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$existingRunner=Join-Path $repoRoot 'scripts/run_creo_step_diagnostic.ps1'
$parseTokens=$null
$parseErrors=$null
$ast=[System.Management.Automation.Language.Parser]::ParseFile($existingRunner,[ref]$parseTokens,[ref]$parseErrors)
if ($parseErrors.Count) { throw 'Cannot parse the existing Creo diagnostic runner' }
# Reuse its function definitions only, avoiding the default run-file digest writer.
$definitions=$ast.FindAll({param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst]},$false)
foreach ($definition in $definitions) { Invoke-Expression $definition.Extent.Text }
$OutputDir=[System.IO.Path]::GetFullPath($OutputDir)
$stepAbsolute=[System.IO.Path]::GetFullPath($StepPath)
$stageDir=Join-Path $OutputDir 'stage'
$importDir=Join-Path $OutputDir 'import'
$checkDir=Join-Path $OutputDir 'modelcheck'
$logDir=Join-Path $OutputDir 'logs'
foreach ($directory in @($OutputDir,$stageDir,$importDir,$checkDir,$logDir)) { New-Item -ItemType Directory -Path $directory -Force | Out-Null }
$started=Get-Date
$result=[ordered]@{started_at=$started.ToString('o');step=$stepAbsolute;creo_root=$CreoRoot;status='Running';validated=$false}
try {
    if (-not (Test-CreoRoot $CreoRoot)) { throw 'Creo installation is incomplete' }
    Set-CreoBatchEnvironment $CreoRoot
    $common=Join-Path $CreoRoot 'Common Files'
    $batch=Join-Path $common 'x86e_win64/obj/dbatchc.exe'
    if ($ExistingImportDirectory) {
        $parts=@(Get-ChildItem -LiteralPath $ExistingImportDirectory -File | Where-Object { $_.Name -match '\.prt\.\d+$' } | ForEach-Object FullName)
        $result.existing_import_directory=[System.IO.Path]::GetFullPath($ExistingImportDirectory)
        $assemblyRoot=$ExistingImportDirectory
    } else {
        $staged=Join-Path $stageDir 'candidate.step'
        Copy-Item -LiteralPath $stepAbsolute -Destination $staged
        $dxc=Join-Path $stageDir 'import.dxc'
        New-DxcFile -Path $dxc -TtdPath (Join-Path $common 'text/ttds/step_3d_import.ttd') -TaskName 'STEP 3D Import' -ObjectPath $staged -TaskOutputDir $importDir -Syntax 'ObjectText'
        $result.import=Invoke-ProcessWithTimeout -FilePath $batch -Arguments @('-nographics','-process',$dxc) -WorkingDirectory $OutputDir -LogPrefix 'import' -Timeout $TimeoutSeconds -LogDirectory $logDir
        $parts=@(Find-CreoPrtFiles -Roots @($OutputDir,$importDir,$stageDir) -Since $started)
        $assemblyRoot=$importDir
    }
    $result.generated_parts=$parts
    if ($parts.Count) {
        $assemblies=@(Get-ChildItem -LiteralPath $assemblyRoot -File | Where-Object { $_.Name -match '^candidate\.asm\.\d+$' } | ForEach-Object FullName)
        $result.root_assemblies=$assemblies
        $result.component_checks=@()
        $index=0
        foreach ($object in @($parts)+@($assemblies)) {
            $objectOutput=Join-Path $checkDir "component_$index"
            New-Item -ItemType Directory -Path $objectOutput -Force | Out-Null
            $dxc=Join-Path $stageDir "modelcheck_$index.dxc"
            New-DxcFile -Path $dxc -TtdPath (Join-Path $common 'text/ttds/modelcheck.ttd') -TaskName 'ModelCHECK' -ObjectPath $object -TaskOutputDir $objectOutput -Syntax 'ObjectText'
            $command=Invoke-ProcessWithTimeout -FilePath $batch -Arguments @('-nographics','-process',$dxc) -WorkingDirectory $OutputDir -LogPrefix "modelcheck_$index" -Timeout $TimeoutSeconds -LogDirectory $logDir
            $reports=@(Find-NewFiles -Roots @($objectOutput) -Since $started -Extensions @('.xml','.html','.htm','.txt','.log'))
            $summary=Get-ModelCheckSummary -ReportFiles $reports
            $result.component_checks+=@([ordered]@{object=$object;command=$command;summary=$summary})
            $index++
        }
        $result.status='ReportReady'
        $result.validated=@($result.component_checks | Where-Object { -not $_.summary.diagnostic_passed }).Count -eq 0
    } else { $result.status='NoImportedPart' }
} catch {
    $result.status='ExecutionError'
    $result.error=$_.Exception.Message
}
$result.ended_at=(Get-Date).ToString('o')
$result | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath (Join-Path $OutputDir 'result.json') -Encoding utf8
Write-Output "$($result.status); validated=$($result.validated)"
