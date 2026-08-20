#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WIN_ARCH="${1:-${ROOT}/dist/windows/steam.arach}"
LIN_ARCH="${2:-${ROOT}/dist/linux/steam.arach}"
OUT_ARCH="${3:-${ROOT}/dist/universal/steam.arach}"
STAGING="${ROOT}/dist/universal/.staging"

rm -rf "${STAGING}"
mkdir -p "${STAGING}/win" "${STAGING}/linux" "${STAGING}/merged/steamidra" "${STAGING}/linux_kit"
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

# Per-OS Online-Fix kits:
# - windows/ stays flat (needed immediately on Windows hosts)
# - linux kit is shipped as linux.zip only — no loose OnlineFix*/SteamFix PE for Defender
#   to scan when the universal .arach is installed under %AppData% on Windows.
rm -rf "${MERGED}/online_fix_kit"
mkdir -p "${MERGED}/online_fix_kit/windows"
if [[ -d "${WIN}/online_fix_kit" ]]; then
  if [[ -d "${WIN}/online_fix_kit/windows" ]]; then
    cp -a "${WIN}/online_fix_kit/windows/." "${MERGED}/online_fix_kit/windows/"
  else
    cp -a "${WIN}/online_fix_kit/." "${MERGED}/online_fix_kit/windows/"
  fi
fi
# Drop any accidental loose linux PE / nested dirs from the Windows tree copy.
rm -rf "${MERGED}/online_fix_kit/linux"
rm -f "${MERGED}/online_fix_kit/linux.zip"
# Never leave OF.me names at the kit root (Windows flat packages shouldn't have them).
rm -f "${MERGED}/online_fix_kit/"OnlineFix*.dll \
  "${MERGED}/online_fix_kit/"SteamOverlay*.dll \
  "${MERGED}/online_fix_kit/"StubDRM*.dll \
  "${MERGED}/online_fix_kit/OnlineFix.ini" \
  "${MERGED}/online_fix_kit/dlllist.txt" 2>/dev/null || true

if [[ -d "${LIN}/online_fix_kit" ]]; then
  rm -rf "${STAGING}/linux_kit"
  mkdir -p "${STAGING}/linux_kit"
  if [[ -d "${LIN}/online_fix_kit/linux" ]]; then
    cp -a "${LIN}/online_fix_kit/linux/." "${STAGING}/linux_kit/"
  else
    cp -a "${LIN}/online_fix_kit/." "${STAGING}/linux_kit/"
  fi
  # Strip READMEs / nested windows copies if a mis-packaged linux tree included them.
  rm -rf "${STAGING}/linux_kit/windows" "${STAGING}/linux_kit/linux" \
    "${STAGING}/linux_kit/x86_optional"
  (cd "${STAGING}/linux_kit" && zip -qr "${MERGED}/online_fix_kit/linux.zip" .)
fi

kit_ok_windows=0
kit_ok_linux=0
if [[ -f "${MERGED}/online_fix_kit/windows/SteamFix64.dll" \
   && -f "${MERGED}/online_fix_kit/windows/winmm.dll" ]]; then
  kit_ok_windows=1
fi
if [[ -f "${MERGED}/online_fix_kit/linux.zip" ]]; then
  kit_ok_linux=1
fi
if [[ "${kit_ok_windows}" -ne 1 ]]; then
  echo "WARNING: universal bundle missing online_fix_kit/windows SteamFix64+winmm" >&2
fi
if [[ "${kit_ok_linux}" -ne 1 ]]; then
  echo "WARNING: universal bundle missing online_fix_kit/linux.zip" >&2
fi
# Sanity: no loose OF.me PE in the universal tree (Windows Defender bait).
if find "${MERGED}/online_fix_kit" -type f \
  \( -iname 'OnlineFix*.dll' -o -iname 'SteamOverlay*.dll' -o -iname 'StubDRM*.dll' \) \
  2>/dev/null | grep -q .; then
  echo "ERROR: universal online_fix_kit still has loose OF.me PE files" >&2
  find "${MERGED}/online_fix_kit" -type f \
    \( -iname 'OnlineFix*.dll' -o -iname 'SteamOverlay*.dll' -o -iname 'StubDRM*.dll' \) >&2
  exit 1
fi

shopt -s nullglob
for so in "${LIN}"/lib*.so*; do
  [[ -f "${so}" ]] || continue
  cp -a "${so}" "${MERGED}/"
done
if command -v patchelf >/dev/null 2>&1 && [[ -f "${MERGED}/libsteamidra_plugin.so" ]]; then
  patchelf --set-rpath '$ORIGIN' "${MERGED}/libsteamidra_plugin.so" || true
  for so in "${MERGED}"/lib*.so*; do
    [[ -f "${so}" ]] || continue
    patchelf --set-rpath '$ORIGIN' "${so}" || true
  done
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
