#!/usr/bin/env bash
# Fetch/vendor OS-specific third_party bits for self-contained SteaMidra builds.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OS="${1:-linux}"
DEST="$ROOT/third_party/$OS"
mkdir -p "$DEST/DDMod" "$DEST/dotnet"

echo "==> third_party/$OS"

if [[ "$OS" == "linux" ]]; then
  mkdir -p "$DEST/SLSsteam"
  if [[ ! -f "$DEST/SLSsteam/SLSsteam.so" ]]; then
    echo "Downloading AceSLS SLSsteam…"
    API="https://api.github.com/repos/AceSLS/SLSsteam/releases/latest"
    URL="$(python3 - <<'PY'
import json,urllib.request
req=urllib.request.Request("https://api.github.com/repos/AceSLS/SLSsteam/releases/latest",
  headers={"User-Agent":"Arachnel-SteaMidra","Accept":"application/vnd.github+json"})
rel=json.load(urllib.request.urlopen(req,timeout=60))
url=None
for a in rel.get("assets") or []:
  n=a.get("name") or ""
  if "Any" in n and (n.endswith(".7z") or n.endswith(".zip")):
    url=a.get("browser_download_url"); break
  if n.endswith(".zip") and not url:
    url=a.get("browser_download_url")
print(url or "")
PY
)"
    [[ -n "$URL" ]] || { echo "No SLSsteam asset"; exit 1; }
    TMP="$(mktemp -d)"
    curl -fsSL -o "$TMP/sls.bin" "$URL"
    if file "$TMP/sls.bin" | grep -qi zip; then
      unzip -qo "$TMP/sls.bin" -d "$TMP/out"
    else
      7z x "$TMP/sls.bin" "-o$TMP/out" -y >/dev/null
    fi
    find "$TMP/out" -name 'SLSsteam.so' -o -name 'library-inject.so' | while read -r f; do
      cp -a "$f" "$DEST/SLSsteam/"
    done
    rm -rf "$TMP"
  fi
  if [[ ! -x "$DEST/dotnet/dotnet" ]]; then
    curl -fsSL https://dot.net/v1/dotnet-install.sh -o /tmp/dotnet-install-steamidra.sh
    bash /tmp/dotnet-install-steamidra.sh --channel 9.0 --runtime dotnet --install-dir "$DEST/dotnet"
  fi
elif [[ "$OS" == "windows" ]]; then
  mkdir -p "$DEST/LumaCore"
  if [[ ! -x "$DEST/dotnet/dotnet.exe" ]] && [[ ! -x "$DEST/dotnet/dotnet" ]]; then
    curl -fsSL https://dot.net/v1/dotnet-install.sh -o /tmp/dotnet-install-steamidra.sh
    # Cross-fetch Windows runtime from Linux hosts when packaging for Win
    bash /tmp/dotnet-install-steamidra.sh --channel 9.0 --runtime dotnet \
      --os win --arch x64 --install-dir "$DEST/dotnet"
  fi
  echo "Place LumaCore release DLLs under third_party/windows/LumaCore/ (or let worker download at runtime)."
else
  echo "Usage: $0 linux|windows"; exit 2
fi

if [[ ! -f "$DEST/DDMod/DepotDownloaderMod.dll" ]]; then
  if [[ -f "$ROOT/third_party/DDMod/DepotDownloaderMod.dll" ]]; then
    cp -a "$ROOT/third_party/DDMod/"* "$DEST/DDMod/"
  elif [[ -f "$ROOT/third_party/linux/DDMod/DepotDownloaderMod.dll" ]]; then
    cp -a "$ROOT/third_party/linux/DDMod/"* "$DEST/DDMod/"
  else
    echo "Downloading DepotDownloaderMod 3.4.0.2…"
    TMP_DD="$(mktemp -d)"
    curl -fL --retry 5 --retry-all-errors \
      -o "$TMP_DD/Release.rar" \
      "https://github.com/SteamAutoCracks/DepotDownloaderMod/releases/download/DepotDownloaderMod_3.4.0.2/Release.rar"
    7z x "$TMP_DD/Release.rar" "-o$TMP_DD/out" -y >/dev/null
    DD_DLL="$(find "$TMP_DD/out" -type f -name DepotDownloaderMod.dll -print -quit)"
    [[ -n "$DD_DLL" ]] || { echo "DepotDownloaderMod.dll missing in release"; exit 1; }
    cp -a "$(dirname "$DD_DLL")/." "$DEST/DDMod/"
    rm -rf "$TMP_DD"
  fi
fi

echo "OK $DEST"
du -sh "$DEST"/* 2>/dev/null || true
