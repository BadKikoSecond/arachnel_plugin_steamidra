#Requires -Version 5.1
param([string]$BuildDir = "build-ci-win")

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
. (Join-Path $PSScriptRoot "read-launcher-toolchain.ps1")

$BuildPath = Join-Path $Root $BuildDir
$SdkDir = if ($env:ARACHNEL_SDK_DIR) { $env:ARACHNEL_SDK_DIR } else { Join-Path $Root ".ci\arachnel-sdk" }
$QtRoot = if ($env:QT_INSTALL_DIR) { $env:QT_INSTALL_DIR } else { Join-Path $Root ".ci\qt" }
$QtPrefix = Join-Path $QtRoot "$($env:QT_VERSION)\msvc2022_64"
$DistDir = Join-Path $Root "dist\windows"

if ($env:CI_COMMIT_TAG) {
    python (Join-Path $Root "scripts\ci\set_plugin_version.py") $env:CI_COMMIT_TAG
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if (-not (Test-Path (Join-Path $SdkDir "cmake\ArachnelPluginSdk.cmake"))) {
    git clone --depth 1 --branch $env:ARACHNEL_SDK_REF https://github.com/BadKiko/Arachnel.git $SdkDir
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if (-not (Test-Path (Join-Path $QtPrefix "lib\cmake\Qt6\Qt6Config.cmake"))) {
    $AqtVenv = Join-Path $Root ".ci\aqt-venv"
    $AqtPython = Join-Path $AqtVenv "Scripts\python.exe"
    if (-not (Test-Path $AqtPython)) { python -m venv $AqtVenv }
    & $AqtPython -m pip install --upgrade pip aqtinstall
    $moduleArgs = @()
    foreach ($module in ($env:QT_MODULES -split '\s+')) {
        if ($module) { $moduleArgs += @("-m", $module) }
    }
    & $AqtPython -m aqt install-qt windows desktop $env:QT_VERSION $env:QT_WINDOWS_ARCH @moduleArgs -O $QtRoot
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

& (Join-Path $Root "tools\fetch_third_party.ps1") windows
cmake -S $Root -B $BuildPath -G "Visual Studio 17 2022" -A x64 `
    -DCMAKE_PREFIX_PATH="$QtPrefix" `
    -DARACHNEL_SDK_DIR="$SdkDir"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build $BuildPath --config Release --target steamidra_arach -j $env:NUMBER_OF_PROCESSORS
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$ArachPath = Join-Path $BuildPath "dist\steamidra.arach"
if (-not (Test-Path $ArachPath)) { throw "Expected artifact not found: $ArachPath" }
New-Item -ItemType Directory -Force $DistDir | Out-Null
Copy-Item $ArachPath (Join-Path $DistDir "steam.arach") -Force
Write-Host "Done: $(Join-Path $DistDir 'steam.arach')"
