# Fetch/vendor Windows third_party bits for self-contained SteaMidra builds.
# Depot download uses linked depotkit. No LumaCore / DepotDownloaderMod.
param([string]$Os = "windows")
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Dest = Join-Path $Root "third_party\$Os"
New-Item -ItemType Directory -Force -Path $Dest | Out-Null

Write-Host "==> third_party/$Os (nothing required on Windows beyond online_fix_kit)"
Write-Host "OK $Dest"
