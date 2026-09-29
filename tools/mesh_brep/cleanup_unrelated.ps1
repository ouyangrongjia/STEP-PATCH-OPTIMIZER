param(
    [Parameter(Mandatory=$true)][string]$Plan,
    [switch]$Execute
)
# Prepared for manual review after automated execution was denied. Defaults to
# listing exact targets; execution must be explicitly selected by the operator.
$ErrorActionPreference='Stop'
$record=Get-Content -LiteralPath $Plan -Raw | ConvertFrom-Json
$runRoot=(Resolve-Path -LiteralPath $record.scope).Path.TrimEnd('\')
$prefix=$runRoot+'\'
$targets=@()
foreach ($relative in $record.targets) {
    if ([System.IO.Path]::IsPathRooted($relative)) { throw 'Plan must use relative target names' }
    $path=[System.IO.Path]::GetFullPath((Join-Path $runRoot $relative))
    if (-not $path.StartsWith($prefix,[System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Target escapes the declared run directory: $relative"
    }
    if (-not (Test-Path -LiteralPath $path)) { continue }
    $item=Get-Item -LiteralPath $path -Force
    $ancestors=@($item)
    $ancestor=if ($item.PSIsContainer) { $item.Parent } else { $item.Directory }
    while ($ancestor) {
        $ancestors+=,$ancestor; $ancestor=$ancestor.Parent
    }
    foreach ($check in $ancestors) {
        if ($check.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
            throw "Reparse point in cleanup ancestry: $($check.FullName)"
        }
    }
    $children=if ($item.PSIsContainer) { @(Get-ChildItem -LiteralPath $path -Recurse -Force) } else { @() }
    foreach ($check in $children) {
        if ($check.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
            throw "Reparse point in cleanup target: $($check.FullName)"
        }
    }
    $targets+=,$path
}
$targets | ForEach-Object { Write-Output $_ }
if ($Execute) {
    foreach ($target in $targets) { Remove-Item -LiteralPath $target -Recurse -Force }
} else {
    Write-Output 'Review only. Nothing deleted. Use -Execute only after reviewing these paths.'
}
