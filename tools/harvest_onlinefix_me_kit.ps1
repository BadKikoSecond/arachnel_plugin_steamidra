#Requires -Version 5.1
# Populate online_fix_kit/linux/ with online-fix.me layout for Proton packaging.
param(
    [string]$DestDir = "",
    [string[]]$SearchRoots = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
if (-not $DestDir) {
    $DestDir = Join-Path $Root "online_fix_kit\linux"
}
New-Item -ItemType Directory -Force -Path $DestDir | Out-Null

$required = @("OnlineFix64.dll", "dlllist.txt")
$optional = @(
    "OnlineFix32.dll",
    "OnlineFix.dll",
    "OnlineFix.ini",
    "SteamOverlay64.dll",
    "SteamOverlay32.dll",
    "StubDRM64.dll",
    "StubDRM32.dll"
)

$haveAll = $true
foreach ($name in $required) {
    if (-not (Test-Path (Join-Path $DestDir $name))) { $haveAll = $false; break }
}
if ($haveAll) {
    Write-Host "online_fix_kit/linux already has OnlineFix64 + dlllist.txt"
    exit 0
}

$roots = [System.Collections.Generic.List[string]]::new()
foreach ($r in $SearchRoots) { if ($r) { [void]$roots.Add($r) } }
foreach ($r in @(
        (Join-Path $env:USERPROFILE "Games"),
        (Join-Path $env:USERPROFILE "Games\Online-Fix"),
        "D:\Games\Online-Fix",
        "C:\Games\Online-Fix",
        "D:\Online-Fix",
        "C:\Online-Fix"
    )) {
    [void]$roots.Add($r)
}
if ($env:APPDATA) {
    [void]$roots.Add((Join-Path $env:APPDATA "Arachnel\Arachnel\library"))
}

function Find-NamedFile([string]$Name, [string[]]$SearchRoots) {
    foreach ($root in $SearchRoots) {
        if (-not (Test-Path -LiteralPath $root)) { continue }
        $direct = Join-Path $root $Name
        if (Test-Path -LiteralPath $direct) { return $direct }
        $hit = Get-ChildItem -LiteralPath $root -Filter $Name -Recurse -File -ErrorAction SilentlyContinue |
            Where-Object {
                $_.FullName -notmatch '\\Engine\\|\\\.depotdownloader\\|\\staging\\|\\steamless\\'
            } |
            Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    return $null
}

$copied = 0
foreach ($name in ($required + $optional)) {
    $dest = Join-Path $DestDir $name
    if (Test-Path $dest) { continue }
    $src = Find-NamedFile $name $roots.ToArray()
    if (-not $src) { continue }
    Copy-Item -Force -LiteralPath $src -Destination $dest
    Write-Host "  $name <- $src"
    $copied++
}

if (-not (Test-Path (Join-Path $DestDir "OnlineFix64.dll")) -and
    -not (Test-Path (Join-Path $DestDir "OnlineFix.dll"))) {
    Write-Warning "Could not harvest OnlineFix64.dll into online_fix_kit/linux (supply OF.me binaries manually)"
    exit 0
}

if (-not (Test-Path (Join-Path $DestDir "dlllist.txt"))) {
    $lines = @()
    foreach ($n in @("OnlineFix64.dll", "SteamOverlay64.dll", "StubDRM64.dll")) {
        if (Test-Path (Join-Path $DestDir $n)) { $lines += $n }
    }
    if ($lines.Count -gt 0) {
        Set-Content -Path (Join-Path $DestDir "dlllist.txt") -Value ($lines -join "`r`n")
    }
}

if (-not (Test-Path (Join-Path $DestDir "OnlineFix.ini"))) {
    @"
[Main]
FakeAppId=480
"@ | Set-Content -Path (Join-Path $DestDir "OnlineFix.ini")
}

Write-Host "Harvested online-fix.me kit into online_fix_kit/linux ($copied new files)"
Get-ChildItem $DestDir -File | ForEach-Object { Write-Host ("  {0} ({1})" -f $_.Name, $_.Length) }
