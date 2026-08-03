#!/usr/bin/env bash
# Fetch/vendor OS-specific third_party bits for self-contained SteaMidra builds.
# Depot download uses linked depotkit - no DepotDownloaderMod / .NET runtime.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OS="${1:-linux}"
DEST="$ROOT/third_party/$OS"

echo "==> third_party/$OS"

if [[ "$OS" == "linux" ]]; then
  mkdir -p "$DEST/SLSsteam"
  if [[ ! -f "$DEST/SLSsteam/SLSsteam.so" ]]; then
    echo "Downloading AceSLS SLSsteam…"
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
    mkdir -p "$TMP/out"
    curl -fL --retry 5 --retry-all-errors -o "$TMP/sls.bin" "$URL"
    if ! 7z x "$TMP/sls.bin" "-o$TMP/out" -y >/dev/null; then
      if file "$TMP/sls.bin" | grep -qiE 'Zip archive|zip compressed data'; then
        unzip -qo "$TMP/sls.bin" -d "$TMP/out"
      else
        echo "Failed to extract SLSsteam from $URL"
        file "$TMP/sls.bin" || true
        ls -lh "$TMP/sls.bin" || true
        exit 1
      fi
    fi
    mapfile -t _sls_files < <(find "$TMP/out" \( -name 'SLSsteam.so' -o -name 'library-inject.so' \) -type f)
    if [[ "${#_sls_files[@]}" -eq 0 ]]; then
      echo "SLSsteam.so not found in archive"
      find "$TMP/out" -maxdepth 3 -type f | head -n 40 || true
      exit 1
    fi
    for f in "${_sls_files[@]}"; do
      cp -a "$f" "$DEST/SLSsteam/"
    done
    rm -rf "$TMP"
  fi
elif [[ "$OS" == "windows" ]]; then
  mkdir -p "$DEST"
  echo "Nothing required under third_party/windows (depotkit is linked; no LumaCore)."
else
  echo "Usage: $0 linux|windows"; exit 2
fi

echo "OK $DEST"
du -sh "$DEST"/* 2>/dev/null || true
