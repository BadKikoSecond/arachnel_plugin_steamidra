# Fetch/vendor Windows third_party bits for self-contained SteaMidra builds.
# Depot download uses linked depotkit - no DepotDownloaderMod / .NET runtime.
param([string]$Os = "windows")
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Dest = Join-Path $Root "third_party\$Os"
New-Item -ItemType Directory -Force -Path (Join-Path $Dest "LumaCore") | Out-Null

Write-Host "==> third_party/$Os"
Write-Host "Place LumaCore release DLLs under third_party/windows/LumaCore/ (or let worker download at runtime)."
Write-Host "OK $Dest"
