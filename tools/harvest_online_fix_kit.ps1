#Requires -Version 5.1
# Populate online_fix_kit/windows/ with FreeTP SteamFix64 + winmm for packaging.
param(
    [string]$DestDir = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
if (-not $DestDir) {
    $DestDir = Join-Path $Root "online_fix_kit\windows"
}

New-Item -ItemType Directory -Force -Path $DestDir | Out-Null
$x86Dir = Join-Path $DestDir "x86_optional"
New-Item -ItemType Directory -Force -Path $x86Dir | Out-Null

$needSteam = -not (Test-Path (Join-Path $DestDir "SteamFix64.dll"))
$needWinmm = -not (Test-Path (Join-Path $DestDir "winmm.dll"))
if (-not $needSteam -and -not $needWinmm) {
    Write-Host "online_fix_kit/windows already has SteamFix64 + winmm"
} else {
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
        Write-Warning "Could not harvest SteamFix64.dll / winmm.dll into online_fix_kit/windows"
    } else {
        Copy-Item -Force -LiteralPath $steam -Destination (Join-Path $DestDir "SteamFix64.dll")
        Copy-Item -Force -LiteralPath $winmm -Destination (Join-Path $DestDir "winmm.dll")
        $txt = Join-Path $DestDir "winmm.txt"
        if (-not (Test-Path $txt)) {
            Set-Content -Path $txt -Value "SteamFix64.dll`n" -NoNewline
        }
        Write-Host "Harvested SteamFix64 + winmm from FreeTP/Hydra installs"
    }

    $fix32 = Find-NamedFile "SteamFix32.dll" $roots
    $winmm32 = Find-NamedFile "winmm32.dll" $roots
    if (-not $winmm32) {
        # Some FreeTP trees only have an x86 winmm.dll next to SteamFix32.
        $cand = Find-NamedFile "winmm.dll" $roots
        if ($cand -and $fix32) { $winmm32 = $cand }
    }
    if ($fix32) {
        Copy-Item -Force -LiteralPath $fix32 -Destination (Join-Path $x86Dir "SteamFix32.dll")
    }
    if ($winmm32) {
        Copy-Item -Force -LiteralPath $winmm32 -Destination (Join-Path $x86Dir "winmm32.dll")
    }
}

# Refresh zip used by packaging (no unpacked 32-bit PE in the .arach tree).
$zipPath = Join-Path $DestDir "x86_optional.zip"
$sf32 = Join-Path $x86Dir "SteamFix32.dll"
$wm32 = Join-Path $x86Dir "winmm32.dll"
if ((Test-Path $sf32) -and (Test-Path $wm32)) {
    if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
    Push-Location $x86Dir
    try {
        Compress-Archive -LiteralPath @("SteamFix32.dll", "winmm32.dll") -DestinationPath $zipPath -Force
    } finally {
        Pop-Location
    }
    Write-Host "Updated $zipPath"
}

Get-ChildItem $DestDir -File | ForEach-Object { Write-Host ("  {0} ({1})" -f $_.Name, $_.Length) }
