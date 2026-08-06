#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WIN_ARCH="${1:-${ROOT}/dist/windows/steam.arach}"
LIN_ARCH="${2:-${ROOT}/dist/linux/steam.arach}"
OUT_ARCH="${3:-${ROOT}/dist/universal/steam.arach}"
STAGING="${ROOT}/dist/universal/.staging"

rm -rf "${STAGING}"
mkdir -p "${STAGING}/win" "${STAGING}/linux" "${STAGING}/merged/steamidra"
unzip -q "${WIN_ARCH}" -d "${STAGING}/win"
unzip -q "${LIN_ARCH}" -d "${STAGING}/linux"

find_root() {
  local base="$1"
  [[ -f "${base}/plugin.json" ]] && { printf '%s\n' "${base}"; return; }
  local child
  for child in "${base}"/*; do
    [[ -d "${child}" && -f "${child}/plugin.json" ]] && {
      printf '%s\n' "${child}"
      return
    }
  done
  return 1
}

WIN="$(find_root "${STAGING}/win")"
LIN="$(find_root "${STAGING}/linux")"
MERGED="${STAGING}/merged/steamidra"
cp -a "${WIN}/." "${MERGED}/"
rm -rf "${MERGED}/Release" "${MERGED}/Debug" "${MERGED}/RelWithDebInfo" "${MERGED}/MinSizeRel"

# Keep platform runtimes separate; C++ plugin selects the current OS.
rm -rf "${MERGED}/third_party"
mkdir -p "${MERGED}/third_party/windows" "${MERGED}/third_party/linux"
cp -a "${WIN}/third_party/." "${MERGED}/third_party/windows/"
cp -a "${LIN}/third_party/." "${MERGED}/third_party/linux/"
[[ ! -f "${MERGED}/third_party/README.md" ]] || \
  cp -a "${WIN}/third_party/README.md" "${MERGED}/third_party/README.md"

# Prefer Windows-harvested SteamFix kit (Linux CI usually has none).
if [[ -f "${WIN}/online_fix_kit/SteamFix64.dll" && -f "${WIN}/online_fix_kit/winmm.dll" ]]; then
  rm -rf "${MERGED}/online_fix_kit"
  mkdir -p "${MERGED}/online_fix_kit"
  cp -a "${WIN}/online_fix_kit/." "${MERGED}/online_fix_kit/"
elif [[ -f "${LIN}/online_fix_kit/SteamFix64.dll" && -f "${LIN}/online_fix_kit/winmm.dll" ]]; then
  rm -rf "${MERGED}/online_fix_kit"
  mkdir -p "${MERGED}/online_fix_kit"
  cp -a "${LIN}/online_fix_kit/." "${MERGED}/online_fix_kit/"
fi
if [[ ! -f "${MERGED}/online_fix_kit/SteamFix64.dll" ]]; then
  echo "WARNING: universal bundle missing online_fix_kit/SteamFix64.dll" >&2
fi

[[ ! -f "${LIN}/libsteamidra_plugin.so" ]] || \
  cp -a "${LIN}/libsteamidra_plugin.so" "${MERGED}/"
# Linux-only: jammy-era libcurl beside the .so (matches AppImage OpenSSL 3.0).
[[ ! -f "${LIN}/libcurl.so.4" ]] || cp -a "${LIN}/libcurl.so.4" "${MERGED}/"
if command -v patchelf >/dev/null 2>&1 && [[ -f "${MERGED}/libsteamidra_plugin.so" ]]; then
  patchelf --set-rpath '$ORIGIN' "${MERGED}/libsteamidra_plugin.so" || true
  [[ ! -f "${MERGED}/libcurl.so.4" ]] || patchelf --set-rpath '$ORIGIN' "${MERGED}/libcurl.so.4" || true
fi

required=(
  "${MERGED}/plugin.json"
  "${MERGED}/steamidra_plugin.dll"
  "${MERGED}/libsteamidra_plugin.so"
)
for path in "${required[@]}"; do
  [[ -f "${path}" ]] || { echo "Universal bundle missing: ${path}" >&2; exit 1; }
done

mkdir -p "$(dirname "${OUT_ARCH}")"
rm -f "${OUT_ARCH}"
(cd "${STAGING}/merged" && zip -qr "${OUT_ARCH}" steamidra)
echo "Universal bundle: ${OUT_ARCH}"
