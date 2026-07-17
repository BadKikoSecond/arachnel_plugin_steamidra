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
  $commonDd = Join-Path $Root "third_party\DDMod"
  $linuxDd = Join-Path $Root "third_party\linux\DDMod"
  if (Test-Path (Join-Path $commonDd "DepotDownloaderMod.dll")) {
    Copy-Item -Recurse -Force (Join-Path $commonDd "*") (Join-Path $Dest "DDMod")
  } elseif (Test-Path (Join-Path $linuxDd "DepotDownloaderMod.dll")) {
    Copy-Item -Recurse -Force (Join-Path $linuxDd "*") (Join-Path $Dest "DDMod")
  } else {
    Write-Host "Downloading DepotDownloaderMod 3.4.0.2…"
    $archive = Join-Path $env:TEMP "DepotDownloaderMod-3.4.0.2.rar"
    $url = "https://github.com/SteamAutoCracks/DepotDownloaderMod/releases/download/DepotDownloaderMod_3.4.0.2/Release.rar"
    for ($attempt = 1; $attempt -le 5; $attempt++) {
      try {
        Invoke-WebRequest -Uri $url -OutFile $archive
        break
      } catch {
        if ($attempt -eq 5) { throw }
        Start-Sleep -Seconds (2 * $attempt)
      }
    }
    $extract = Join-Path $env:TEMP ("ddmod-" + [guid]::NewGuid().ToString("n"))
    New-Item -ItemType Directory -Force $extract | Out-Null
    $sevenZip = "${env:ProgramFiles}\7-Zip\7z.exe"
    if (-not (Test-Path $sevenZip)) { throw "7-Zip is required to extract DepotDownloaderMod" }
    & $sevenZip x $archive "-o$extract" -y | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Failed to extract DepotDownloaderMod" }
    $found = Get-ChildItem $extract -Recurse -Filter "DepotDownloaderMod.dll" | Select-Object -First 1
    if (-not $found) { throw "DepotDownloaderMod.dll missing in release" }
    Copy-Item -Recurse -Force (Join-Path $found.DirectoryName "*") (Join-Path $Dest "DDMod")
    Remove-Item -Recurse -Force $extract
  }
}

Write-Host "Place LumaCore release DLLs under third_party/windows/LumaCore/ (or let worker download at runtime)."
Write-Host "OK $Dest"
