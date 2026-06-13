param(
    [string]$EnvName = "spo-global-chain",
    [string]$Conda = "D:\miniconda\Scripts\conda.exe"
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$scriptPath = Join-Path $repoRoot "scripts\global_chain_cut_cli.py"

if (-not (Test-Path $Conda)) {
    $condaCommand = Get-Command conda -ErrorAction SilentlyContinue
    if ($null -eq $condaCommand) {
        throw "conda was not found. Pass -Conda <path-to-conda.exe>."
    }
    $Conda = $condaCommand.Source
}

if (-not (Test-Path $scriptPath)) {
    throw "Global Cut Chain CLI script was not found: $scriptPath"
}

$envList = & $Conda env list
$envExists = $false
foreach ($line in $envList) {
    if ($line -match "^\s*$([regex]::Escape($EnvName))\s+") {
        $envExists = $true
        break
    }
}

if (-not $envExists) {
    & $Conda create -y -n $EnvName python=3.11 "numpy<2" rtree -c conda-forge
    if ($LASTEXITCODE -ne 0) {
        throw "conda create failed with exit code $LASTEXITCODE"
    }
} else {
    & $Conda install -y -n $EnvName python=3.11 "numpy<2" rtree -c conda-forge
    if ($LASTEXITCODE -ne 0) {
        throw "conda install failed with exit code $LASTEXITCODE"
    }
}

& $Conda run -n $EnvName python -m pip install --upgrade --force-reinstall --no-deps "trimesh==4.5.3" "triangle==20250106"
if ($LASTEXITCODE -ne 0) {
    throw "pip install Global Cut Chain Python packages failed with exit code $LASTEXITCODE"
}

& $Conda run -n $EnvName python $scriptPath --self-test
if ($LASTEXITCODE -ne 0) {
    throw "Global Cut Chain CLI self-test failed with exit code $LASTEXITCODE"
}

$python = (& $Conda run -n $EnvName python -c "import sys; print(sys.executable)").Trim()
Write-Host "Global Cut Chain Python environment is ready."
Write-Host "SPO_GLOBAL_CHAIN_PYTHON=$python"
