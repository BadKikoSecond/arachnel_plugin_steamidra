#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
export ARACHNEL_SDK_DIR="${ARACHNEL_SDK_DIR:-$ROOT/../Arachnel}"
if [[ ! -f "$ROOT/third_party/linux/DDMod/DepotDownloaderMod.dll" ]] \
  || [[ ! -x "$ROOT/third_party/linux/dotnet/dotnet" ]]; then
  echo "Fetching third_party (linux)…"
  "$ROOT/tools/fetch_third_party.sh" linux
fi
BUILD="$ROOT/build"
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$BUILD" --target steamidra_arach
BUNDLE="$BUILD/plugin-bundle"
# POST_BUILD may skip when the DLL is unchanged — always refresh loose assets.
cp -f "$ROOT/plugin.json" "$BUNDLE/plugin.json"
rm -rf "$BUNDLE/worker"
cp -a "$ROOT/worker" "$BUNDLE/worker"
if [[ -d "$ROOT/config" ]]; then
  cp -a "$ROOT/config/." "$BUNDLE/config/"
fi
DEST="${XDG_DATA_HOME:-$HOME/.local/share}/PetWork/Arachnel/plugins/steamidra"
rm -rf "$DEST"
mkdir -p "$DEST"
cp -a "$BUNDLE/." "$DEST/"
VER="$(python3 -c "import json; print(json.load(open('$DEST/plugin.json'))['version'])" 2>/dev/null || echo "?")"
echo "Reinstalled steamidra v$VER -> $DEST"
if [[ -f "$BUILD/dist/steamidra.arach" ]]; then
  echo "Package: $BUILD/dist/steamidra.arach ($(du -h "$BUILD/dist/steamidra.arach" | awk '{print $1}'))"
fi
