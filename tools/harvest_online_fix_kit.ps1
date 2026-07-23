#Requires -Version 5.1
# Populate online_fix_kit/ with SteamFix64.dll + winmm.dll for packaging.
param(
    [string]$DestDir = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
if (-not $DestDir) {
    $DestDir = Join-Path $Root "online_fix_kit"
}

New-Item -ItemType Directory -Force -Path $DestDir | Out-Null
$needSteam = -not (Test-Path (Join-Path $DestDir "SteamFix64.dll"))
$needWinmm = -not (Test-Path (Join-Path $DestDir "winmm.dll"))
if (-not $needSteam -and -not $needWinmm) {
    Write-Host "online_fix_kit already present"
    exit 0
}

$roots = @(
    "D:\HydraGames",
    "D:\ArachnelGame",
    "C:\Games\Arachnel",
    "D:\Games\Arachnel",
    (Join-Path $env:USERPROFILE "Games")
)
if ($env:APPDATA) {
    $roots += (Join-Path $env:APPDATA "Arachnel\Arachnel\library")
}

function Find-NamedFile([string]$Name, [string[]]$SearchRoots) {
    foreach ($root in $SearchRoots) {
        if (-not (Test-Path -LiteralPath $root)) { continue }
        $direct = Join-Path $root $Name
        if (Test-Path -LiteralPath $direct) { return $direct }
        $hit = Get-ChildItem -LiteralPath $root -Filter $Name -Recurse -File -ErrorAction SilentlyContinue |
            Where-Object {
                $_.FullName -notmatch '\\Engine\\|\\\.depotdownloader\\|\\staging\\'
            } |
            Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    return $null
}

$steam = Find-NamedFile "SteamFix64.dll" $roots
$winmm = Find-NamedFile "winmm.dll" $roots
if (-not $steam -or -not $winmm) {
    Write-Warning "Could not harvest SteamFix64.dll / winmm.dll into online_fix_kit"
    exit 0
}

Copy-Item -Force -LiteralPath $steam -Destination (Join-Path $DestDir "SteamFix64.dll")
Copy-Item -Force -LiteralPath $winmm -Destination (Join-Path $DestDir "winmm.dll")
$txt = Join-Path $DestDir "winmm.txt"
if (-not (Test-Path $txt)) {
    Set-Content -Path $txt -Value "SteamFix64.dll`n" -NoNewline
}
Write-Host "Harvested online_fix_kit from FreeTP/Hydra installs"
Get-ChildItem $DestDir | ForEach-Object { Write-Host ("  {0} ({1})" -f $_.Name, $_.Length) }
