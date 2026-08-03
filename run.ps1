#Requires -Version 5.1
param(
    [string]$BuildDir = "build-win",
    [string]$ArachnelSdkDir = $(if ($env:ARACHNEL_SDK_DIR) { $env:ARACHNEL_SDK_DIR } else { "" }),
    [string]$DepotkitDir = $(if ($env:DEPOTKIT_DIR) { $env:DEPOTKIT_DIR } else { "" })
)

$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot

# Optional: LumaCore / other injectors (depotkit is linked at build time).
if (-not (Test-Path (Join-Path $Root "third_party\windows"))) {
    Write-Host "Preparing third_party (windows)…"
    & (Join-Path $Root "tools\fetch_third_party.ps1")
}

function Find-QtPrefix {
    if ($env:CMAKE_PREFIX_PATH) { return $env:CMAKE_PREFIX_PATH }
    foreach ($root in @("D:\Qt", "C:\Qt")) {
        if (-not (Test-Path $root)) { continue }
        $kits = Get-ChildItem $root -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^\d' } |
            Sort-Object Name -Descending
        foreach ($ver in $kits) {
            $candidates = Get-ChildItem $ver.FullName -Directory -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -match 'mingw' }
            foreach ($kit in $candidates) {
                if (Test-Path (Join-Path $kit.FullName "lib\cmake\Qt6\Qt6Config.cmake")) {
                    return $kit.FullName
                }
            }
        }
    }
    return $null
}

if (-not $ArachnelSdkDir) {
    foreach ($candidate in @(
            (Join-Path $Root "..\Arachnel"),
            (Join-Path $Root "..\..\Work\Arachnel"),
            "D:\Work\Arachnel"
        )) {
        if (Test-Path (Join-Path $candidate "cmake\ArachnelPluginSdk.cmake")) {
            $ArachnelSdkDir = (Resolve-Path $candidate).Path
            break
        }
    }
}

if (-not $ArachnelSdkDir -or -not (Test-Path (Join-Path $ArachnelSdkDir "cmake\ArachnelPluginSdk.cmake"))) {
    Write-Error "Set ARACHNEL_SDK_DIR to an Arachnel checkout (contains cmake/ArachnelPluginSdk.cmake)."
}

if (-not $DepotkitDir) {
    foreach ($candidate in @(
            (Join-Path $Root "..\depotkit"),
            "D:\PetWork\depotkit"
        )) {
        if (Test-Path (Join-Path $candidate "CMakeLists.txt")) {
            $DepotkitDir = (Resolve-Path $candidate).Path
            break
        }
    }
}
if (-not $DepotkitDir -or -not (Test-Path (Join-Path $DepotkitDir "CMakeLists.txt"))) {
    Write-Error "Set DEPOTKIT_DIR to a depotkit checkout (https://github.com/BadKiko/depotkit)."
}

$qtPrefix = Find-QtPrefix
if (-not $qtPrefix) {
    Write-Error "Qt 6 kit not found. Set CMAKE_PREFIX_PATH, e.g. D:\Qt\6.11.1\mingw_64"
}

$env:Path = "D:\Qt\Tools\mingw1310_64\bin;D:\Qt\Tools\Ninja;$env:Path"
$buildPath = Join-Path $Root $BuildDir
if (-not (Test-Path $buildPath)) {
    cmake -S $Root -B $buildPath -G Ninja `
        -DCMAKE_BUILD_TYPE=RelWithDebInfo `
        -DCMAKE_PREFIX_PATH="$qtPrefix" `
        -DARACHNEL_SDK_DIR="$ArachnelSdkDir" `
        -DDEPOTKIT_DIR="$DepotkitDir"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

cmake --build $buildPath --target steamidra_arach
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$bundle = Join-Path $buildPath "plugin-bundle"
# POST_BUILD may skip when the DLL is unchanged — always refresh loose assets.
Copy-Item -Force (Join-Path $Root "plugin.json") (Join-Path $bundle "plugin.json")
# Runtime is C++ only — do not ship Python worker.
if (Test-Path (Join-Path $bundle "worker")) {
    Remove-Item -Recurse -Force (Join-Path $bundle "worker")
}
if (Test-Path (Join-Path $Root "config")) {
    Copy-Item -Recurse -Force (Join-Path $Root "config") (Join-Path $bundle "config")
}

# Arachnel AppDataLocation = %APPDATA%\<Org>\<App> after configureApplicationIdentity.
$destCandidates = @(
    (Join-Path $env:APPDATA "Arachnel\Arachnel\plugins\steamidra"),
    (Join-Path $env:APPDATA "Arachnel\plugins\steamidra")
)
$dest = $destCandidates[0]
New-Item -ItemType Directory -Force -Path (Split-Path $dest) | Out-Null
if (Test-Path (Join-Path $bundle "steamidra_plugin.dll")) {
    # Keep local catalog cache + settings across reinstalls (20MB relay fetch).
    $keepDir = Join-Path $env:TEMP ("arachnel-steamidra-keep-" + [guid]::NewGuid().ToString("n"))
    New-Item -ItemType Directory -Force -Path $keepDir | Out-Null
    foreach ($name in @("games-manifest-catalog.json", "settings.json")) {
        foreach ($probe in $destCandidates) {
            $srcKeep = Join-Path $probe $name
            if (Test-Path $srcKeep) {
                Copy-Item -Force $srcKeep (Join-Path $keepDir $name)
                break
            }
        }
    }
    $kitSrc = $null
    foreach ($probe in $destCandidates) {
        $cand = Join-Path $probe "online_fix_kit"
        if (Test-Path $cand) { $kitSrc = $cand; break }
    }
    # Only keep an older install kit when the build bundle is incomplete.
    # Otherwise restoring it would wipe SteamFix32 / winmm32 / steamless from the new build.
    $bundleKit = Join-Path $bundle "online_fix_kit"
    $bundleKitOk = (Test-Path (Join-Path $bundleKit "SteamFix64.dll")) -and `
        (Test-Path (Join-Path $bundleKit "winmm.dll")) -and `
        (Test-Path (Join-Path $bundleKit "SteamFix32.dll")) -and `
        (Test-Path (Join-Path $bundleKit "winmm32.dll")) -and `
        (Test-Path (Join-Path $bundleKit "steamless\Steamless.CLI.exe"))
    if ($kitSrc -and -not $bundleKitOk) {
        Copy-Item -Recurse -Force $kitSrc (Join-Path $keepDir "online_fix_kit")
    }
    foreach ($probe in $destCandidates) {
        $replaced = $false
        if (Test-Path $probe) {
            try {
                Remove-Item -Recurse -Force $probe -ErrorAction Stop
                $replaced = $true
            } catch {
                Write-Host "Plugin folder locked at $probe (close Arachnel for a clean wipe). Overwriting files in place…"
            }
        }
        New-Item -ItemType Directory -Force -Path $probe | Out-Null
        Copy-Item -Path (Join-Path $bundle "*") -Destination $probe -Recurse -Force
        foreach ($name in @("games-manifest-catalog.json", "settings.json")) {
            $kept = Join-Path $keepDir $name
            if (Test-Path $kept) {
                Copy-Item -Force $kept (Join-Path $probe $name)
            }
        }
        $kitKept = Join-Path $keepDir "online_fix_kit"
        if (Test-Path $kitKept) {
            Copy-Item -Recurse -Force $kitKept (Join-Path $probe "online_fix_kit")
        }
        $ver = (Get-Content (Join-Path $probe "plugin.json") -Raw | ConvertFrom-Json).version
        if ($replaced) {
            Write-Host "Reinstalled steamidra v$ver -> $probe"
        } else {
            Write-Host "Updated steamidra v$ver in place -> $probe (restart Arachnel to unload old DLL)"
        }
    }
    Remove-Item -Recurse -Force $keepDir -ErrorAction SilentlyContinue
}

$arach = Join-Path $buildPath "dist\steamidra.arach"
$winArach = Join-Path $buildPath "dist\steamidra-windows.arach"
if (Test-Path $arach) {
    Copy-Item -Force $arach $winArach
    Write-Host "Done. .arach: $arach (also $winArach for sourcelist)"
} else {
    Write-Host "Done."
}
