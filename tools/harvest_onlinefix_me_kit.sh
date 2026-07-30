#!/usr/bin/env bash
# Populate online_fix_kit/linux/ with online-fix.me layout for Proton packaging.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${1:-${ROOT}/online_fix_kit/linux}"
mkdir -p "${DEST}"

need_of=1
if [[ -f "${DEST}/OnlineFix64.dll" || -f "${DEST}/OnlineFix.dll" ]]; then
  if [[ -f "${DEST}/dlllist.txt" ]]; then
    echo "online_fix_kit/linux already has OnlineFix + dlllist.txt"
    exit 0
  fi
  need_of=0
fi

roots=(
  "${HOME}/Games"
  "${HOME}/Games/Online-Fix"
  "${HOME}/.var/app/org.badkiko.sofl/data/game"
  "/mnt/Games"
  "/mnt/game"
)
if [[ -n "${XDG_DATA_HOME:-}" ]]; then
  roots+=("${XDG_DATA_HOME}/Arachnel/Arachnel/library" "${XDG_DATA_HOME}/Arachnel/library")
else
  roots+=("${HOME}/.local/share/Arachnel/Arachnel/library")
fi
# Extra roots from env (colon-separated).
if [[ -n "${ONLINEFIX_ME_SEARCH_ROOTS:-}" ]]; then
  IFS=':' read -r -a extra <<< "${ONLINEFIX_ME_SEARCH_ROOTS}"
  roots+=("${extra[@]}")
fi

find_named() {
  local name="$1"
  local root hit
  for root in "${roots[@]}"; do
    [[ -d "${root}" ]] || continue
    if [[ -f "${root}/${name}" ]]; then
      printf '%s\n' "${root}/${name}"
      return 0
    fi
    hit="$(find "${root}" -type f -name "${name}" \
      ! -path '*/Engine/*' ! -path '*/.depotdownloader/*' ! -path '*/staging/*' \
      2>/dev/null | head -n1 || true)"
    if [[ -n "${hit}" ]]; then
      printf '%s\n' "${hit}"
      return 0
    fi
  done
  return 1
}

names=(
  OnlineFix64.dll OnlineFix32.dll OnlineFix.dll OnlineFix.ini dlllist.txt
  SteamOverlay64.dll SteamOverlay32.dll StubDRM64.dll StubDRM32.dll
)
copied=0
for name in "${names[@]}"; do
  dest="${DEST}/${name}"
  [[ -f "${dest}" ]] && continue
  src="$(find_named "${name}" || true)"
  [[ -n "${src}" ]] || continue
  cp -f "${src}" "${dest}"
  echo "  ${name} <- ${src}"
  copied=$((copied + 1))
done

if [[ ! -f "${DEST}/OnlineFix64.dll" && ! -f "${DEST}/OnlineFix.dll" ]]; then
  echo "WARNING: could not harvest OnlineFix64.dll into online_fix_kit/linux (supply OF.me binaries manually)" >&2
  exit 0
fi

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

echo "Harvested online-fix.me kit into online_fix_kit/linux (${copied} new files)"
ls -la "${DEST}"
