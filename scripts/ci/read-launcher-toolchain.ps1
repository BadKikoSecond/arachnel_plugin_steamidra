#Requires -Version 5.1
param([string]$EnvFile = (Join-Path $PSScriptRoot "launcher-toolchain.env"))

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
if (-not (Test-Path -LiteralPath $EnvFile)) { throw "Missing toolchain lock: $EnvFile" }

Get-Content -LiteralPath $EnvFile | ForEach-Object {
    $line = $_.Trim()
    if (-not $line -or $line.StartsWith('#')) { return }
    $eq = $line.IndexOf('=')
    if ($eq -lt 1) { return }
    Set-Item -Path "env:$($line.Substring(0, $eq).Trim())" -Value $line.Substring($eq + 1).Trim()
}
