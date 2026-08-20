#!/usr/bin/env bash
# Merge Online-Fix.me fix archives into online_fix_kit/linux/.
# Usage:
#   ./tools/merge_onlinefix_me_archives.sh /path/to/fix_rars_or_dirs
# Archives are often password-protected with: online-fix.me
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${ONLINEFIX_KIT_DEST:-${ROOT}/online_fix_kit/linux}"
SRC="${1:-}"
if [[ -z "${SRC}" || ! -e "${SRC}" ]]; then
  echo "Usage: $0 <dir-with-fix-archives-or-extracted-folders>" >&2
  exit 2
fi

WORKDIR="$(mktemp -d /tmp/ofme_merge.XXXXXX)"
trap 'rm -rf "${WORKDIR}"' EXIT
mkdir -p "${DEST}" "${WORKDIR}/extracted"

NAMES=(
  OnlineFix64.dll OnlineFix32.dll OnlineFix.dll OnlineFix.ini dlllist.txt
  SteamOverlay64.dll SteamOverlay32.dll StubDRM64.dll StubDRM32.dll
)

extract_one() {
  local archive="$1"
  local out="$2"
  mkdir -p "${out}"
  if command -v 7z >/dev/null 2>&1; then
    7z x -y -p"online-fix.me" "-o${out}" "${archive}" >/dev/null 2>&1 \
      || 7z x -y "-o${out}" "${archive}" >/dev/null 2>&1 || return 1
  elif command -v unrar >/dev/null 2>&1; then
    unrar x -o+ -p"online-fix.me" "${archive}" "${out}/" >/dev/null 2>&1 \
      || unrar x -o+ "${archive}" "${out}/" >/dev/null 2>&1 || return 1
  else
    echo "Need 7z or unrar to extract ${archive}" >&2
    return 1
  fi
}

copied=0
# Extract archives under SRC
while IFS= read -r -d '' archive; do
  base="$(basename "${archive}")"
  out="${WORKDIR}/extracted/${base}"
  echo "Extract: ${archive}"
  extract_one "${archive}" "${out}" || echo "  skip (extract failed)"
done < <(find "${SRC}" -type f \( -iname '*.rar' -o -iname '*.zip' -o -iname '*.7z' \) -print0 2>/dev/null)

# Also scan already-extracted trees
SCAN_ROOTS=("${SRC}" "${WORKDIR}/extracted")
for name in "${NAMES[@]}"; do
  best=""
  best_size=0
  for root in "${SCAN_ROOTS[@]}"; do
    while IFS= read -r -d '' hit; do
      sz="$(stat -c%s "${hit}" 2>/dev/null || echo 0)"
      # Prefer larger / newer-ish by size as a crude consensus for shared DLLs.
      if [[ "${sz}" -ge "${best_size}" ]]; then
        best="${hit}"
        best_size="${sz}"
      fi
    done < <(find "${root}" -type f -iname "${name}" -print0 2>/dev/null)
  done
  if [[ -n "${best}" ]]; then
    cp -f "${best}" "${DEST}/${name}"
    echo "  ${name} <- ${best} (${best_size} bytes)"
    copied=$((copied + 1))
  fi
done

if [[ ! -f "${DEST}/dlllist.txt" ]]; then
  {
    for n in OnlineFix64.dll SteamOverlay64.dll StubDRM64.dll; do
      [[ -f "${DEST}/${n}" ]] && printf '%s\n' "${n}"
    done
  } >"${DEST}/dlllist.txt"
fi
if [[ ! -f "${DEST}/OnlineFix.ini" ]]; then
  cat >"${DEST}/OnlineFix.ini" <<'EOF'
[Main]
FakeAppId=480
EOF
fi

echo "Merged ${copied} kit files into ${DEST}"
ls -la "${DEST}"
if [[ ! -f "${DEST}/OnlineFix64.dll" && ! -f "${DEST}/OnlineFix.dll" ]]; then
  echo "WARNING: OnlineFix64.dll still missing — drop OF.me *fix* archives into the source dir and re-run." >&2
  exit 1
fi
