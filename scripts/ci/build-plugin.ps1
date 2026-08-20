#Requires -Version 5.1
param([string]$BuildDir = "build-ci-win")

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
. (Join-Path $PSScriptRoot "read-launcher-toolchain.ps1")

$BuildPath = Join-Path $Root $BuildDir
$SdkDir = if ($env:ARACHNEL_SDK_DIR) { $env:ARACHNEL_SDK_DIR } else { Join-Path $Root ".ci\arachnel-sdk" }
$DepotkitDir = if ($env:DEPOTKIT_DIR) { $env:DEPOTKIT_DIR } else { Join-Path $Root ".ci\depotkit" }
$QtRoot = if ($env:QT_INSTALL_DIR) { $env:QT_INSTALL_DIR } else { Join-Path $Root ".ci\qt" }
$QtVersion = $env:QT_VERSION
$QtArch = $env:QT_WINDOWS_ARCH
$QtKit = if ($env:QT_WINDOWS_KIT) { $env:QT_WINDOWS_KIT } else {
    if ($QtArch -match 'mingw') { 'mingw_64' } else { 'msvc2022_64' }
}
$QtPrefix = Join-Path $QtRoot "$QtVersion\$QtKit"
$DistDir = Join-Path $Root "dist\windows"
$useMingw = $QtKit -match 'mingw'

Write-Host "Toolchain: Qt $QtVersion $QtArch ($QtKit), SDK $($env:ARACHNEL_SDK_REF)"

if ($env:CI_COMMIT_TAG) {
    python (Join-Path $Root "scripts\ci\set_plugin_version.py") $env:CI_COMMIT_TAG
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if (-not (Test-Path (Join-Path $SdkDir "cmake\ArachnelPluginSdk.cmake"))) {
    git clone --depth 1 --branch $env:ARACHNEL_SDK_REF https://github.com/BadKiko/Arachnel.git $SdkDir
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$depotkitUrl = if ($env:DEPOTKIT_GIT_URL) { $env:DEPOTKIT_GIT_URL } else { "https://github.com/BadKiko/depotkit.git" }
$depotkitRef = if ($env:DEPOTKIT_REF) { $env:DEPOTKIT_REF } else { "main" }
if (-not (Test-Path (Join-Path $DepotkitDir "CMakeLists.txt"))) {
    if (Test-Path $DepotkitDir) { Remove-Item -Recurse -Force $DepotkitDir }
    git clone --depth 1 --branch $depotkitRef $depotkitUrl $DepotkitDir
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

function Find-MingwBin {
    param([string]$Prefix, [string]$InstallRoot)
    $candidates = @(
        (Join-Path $InstallRoot "Tools\mingw1310_64\bin"),
        (Join-Path $InstallRoot "Tools\mingw1120_64\bin"),
        (Join-Path (Split-Path -Parent (Split-Path -Parent $Prefix)) "Tools\mingw1310_64\bin")
    )
    foreach ($bin in $candidates) {
        if (Test-Path -LiteralPath (Join-Path $bin "g++.exe")) { return $bin }
    }
    $fromPath = Get-Command g++.exe -ErrorAction SilentlyContinue
    if ($fromPath) { return (Split-Path -Parent $fromPath.Source) }
    return $null
}

if (-not (Test-Path (Join-Path $QtPrefix "lib\cmake\Qt6\Qt6Config.cmake"))) {
    $AqtVenv = Join-Path $Root ".ci\aqt-venv"
    $AqtPython = Join-Path $AqtVenv "Scripts\python.exe"
    if (-not (Test-Path $AqtPython)) { python -m venv $AqtVenv }
    & $AqtPython -m pip install --upgrade pip "git+https://github.com/miurahr/aqtinstall.git"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $moduleArgs = @()
    foreach ($module in ($env:QT_MODULES -split '\s+')) {
        if ($module) { $moduleArgs += @("-m", $module) }
    }
    & $AqtPython -m aqt install-qt windows desktop $QtVersion $QtArch @moduleArgs -O $QtRoot
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    if ($useMingw) {
        & $AqtPython -m aqt install-tool windows desktop tools_mingw1310 -O $QtRoot
    }
}

if (-not (Test-Path (Join-Path $QtPrefix "lib\cmake\Qt6\Qt6Config.cmake"))) {
    throw "Qt kit not found at $QtPrefix"
}

& (Join-Path $Root "tools\fetch_third_party.ps1") windows
& (Join-Path $Root "tools\harvest_online_fix_kit.ps1")
# Best-effort OF.me harvest for linux kit (used by universal merge); ignore failures.
try { & (Join-Path $Root "tools\harvest_onlinefix_me_kit.ps1") } catch { Write-Warning $_ }

$configureArgs = @(
    "-S", $Root,
    "-B", $BuildPath,
    "-DCMAKE_PREFIX_PATH=$QtPrefix",
    "-DARACHNEL_SDK_DIR=$SdkDir",
    "-DDEPOTKIT_DIR=$DepotkitDir",
    "-DCMAKE_BUILD_TYPE=Release"
)

if ($useMingw) {
    $mingwBin = Find-MingwBin -Prefix $QtPrefix -InstallRoot $QtRoot
    if (-not $mingwBin) { throw "MinGW bin not found (expected Tools\mingw1310_64\bin next to Qt)" }
    $env:Path = "$mingwBin;D:\Qt\Tools\Ninja;$env:Path"
    $gcc = (Join-Path $mingwBin "gcc.exe") -replace '\\', '/'
    $gxx = (Join-Path $mingwBin "g++.exe") -replace '\\', '/'
    Write-Host "==> Configure (MinGW/Ninja) $QtPrefix"
    $configureArgs += @(
        "-G", "Ninja",
        "-DCMAKE_C_COMPILER=$gcc",
        "-DCMAKE_CXX_COMPILER=$gxx"
    )
} else {
    Write-Host "==> Configure (MSVC 2022 x64)"
    $configureArgs += @("-G", "Visual Studio 17 2022", "-A", "x64")
}

& cmake @configureArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($useMingw) {
    & cmake --build $BuildPath --target steamidra_arach -j $env:NUMBER_OF_PROCESSORS
} else {
    & cmake --build $BuildPath --config Release --target steamidra_arach -j $env:NUMBER_OF_PROCESSORS
}
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$ArachPath = Join-Path $BuildPath "dist\steamidra.arach"
if (-not (Test-Path $ArachPath)) { throw "Expected artifact not found: $ArachPath" }
New-Item -ItemType Directory -Force $DistDir | Out-Null
Copy-Item $ArachPath (Join-Path $DistDir "steam.arach") -Force
Write-Host "Done: $(Join-Path $DistDir 'steam.arach')"
