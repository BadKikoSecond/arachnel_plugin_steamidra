# Fetch/vendor Windows third_party bits for self-contained SteaMidra builds.
param([string]$Os = "windows")
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Dest = Join-Path $Root "third_party\$Os"
New-Item -ItemType Directory -Force -Path (Join-Path $Dest "DDMod") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $Dest "dotnet") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $Dest "LumaCore") | Out-Null

Write-Host "==> third_party/$Os"

$dotnet = Join-Path $Dest "dotnet\dotnet.exe"
if (-not (Test-Path $dotnet)) {
  $script = Join-Path $env:TEMP "dotnet-install.ps1"
  Invoke-WebRequest -Uri "https://dot.net/v1/dotnet-install.ps1" -OutFile $script
  & $script -Channel 9.0 -Runtime dotnet -InstallDir (Join-Path $Dest "dotnet")
}

$dd = Join-Path $Dest "DDMod\DepotDownloaderMod.dll"
if (-not (Test-Path $dd)) {
  $linuxDd = Join-Path $Root "third_party\linux\DDMod"
  if (Test-Path (Join-Path $linuxDd "DepotDownloaderMod.dll")) {
    Copy-Item -Recurse -Force (Join-Path $linuxDd "*") (Join-Path $Dest "DDMod")
  } else {
    throw "Missing DepotDownloaderMod.dll — copy into third_party/$Os/DDMod/"
  }
}

Write-Host "Place LumaCore release DLLs under third_party/windows/LumaCore/ (or let worker download at runtime)."
Write-Host "OK $Dest"
