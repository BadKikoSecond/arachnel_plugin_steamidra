#Requires -Version 5.1
<#
.SYNOPSIS
  Run SteaMidra install with a newline progress log (one tick per line).

.EXAMPLE
  .\tools\install_logged.ps1 -AppId 4704690 -Title MECCHA -Target "D:\Games\ArachnelTest\MECCHA"
  .\tools\install_logged.ps1 -AppId 4704690 -Title MECCHA -Target "D:\Games\ArachnelTest\MECCHA" -Force
#>
param(
    [Parameter(Mandatory = $true)][string]$AppId,
    [string]$Title = "App",
    [Parameter(Mandatory = $true)][string]$Target,
    [switch]$Force,
    [string]$LogPath = ""
)

$ErrorActionPreference = "Stop"
$PluginRoot = Split-Path $PSScriptRoot -Parent
$Worker = Join-Path $PluginRoot "worker\steamidra_worker.py"
if (-not (Test-Path $Worker)) {
    throw "Worker not found: $Worker"
}
$Settings = Join-Path $PluginRoot "settings.json"
if (-not $LogPath) {
    $LogPath = Join-Path (Get-Location) "progress-$AppId.log"
}

$pyArgs = @(
    $Worker,
    "--progress-log", $LogPath,
    "install",
    "--appid", $AppId,
    "--title", $Title,
    "--target", $Target
)
if (Test-Path $Settings) { $pyArgs += @("--settings", $Settings) }
if ($Force) { $pyArgs += "--force" }

Write-Host "Progress log (one line per tick): $LogPath"
Write-Host "python $($pyArgs -join ' ')"
& python @pyArgs
$code = $LASTEXITCODE
Write-Host ""
Write-Host "---- last 40 log lines ----"
if (Test-Path $LogPath) {
    Get-Content $LogPath -Tail 40 -Encoding utf8
}
exit $code
