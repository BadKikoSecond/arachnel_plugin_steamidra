#!/usr/bin/env bash
set -euo pipefail

SO="${1:?usage: check-linux-runtime.sh <plugin.so> [plugin-dir]}"
DIR="${2:-$(cd "$(dirname "${SO}")" && pwd)}"
SO="${DIR}/$(basename "${SO}")"

if [[ ! -f "${SO}" ]]; then
  echo "missing plugin library: ${SO}" >&2
  exit 1
fi

need_tools=(ldd readelf python3)
for tool in "${need_tools[@]}"; do
  command -v "${tool}" >/dev/null 2>&1 || {
    echo "missing required tool: ${tool}" >&2
    exit 1
  }
done

is_allowed_system_lib() {
  local name="$1"
  case "${name}" in
    linux-vdso.so.*|ld-linux-*.so.*|libc.so.*|libm.so.*|libpthread.so.*|libdl.so.*|librt.so.*|libgcc_s.so.*|libstdc++.so.*|libresolv.so.*|libnsl.so.*|libutil.so.*|libz.so.*)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

rpath="$(readelf -d "${SO}" | awk -F'[][]' '/RUNPATH|RPATH/ { print $2; exit }')"
if [[ "${rpath}" != *'$ORIGIN'* ]]; then
  echo "RUNPATH check failed for ${SO}: expected \$ORIGIN, got '${rpath:-<empty>}'" >&2
  exit 1
fi

ldd_output="$(ldd "${SO}")"
echo "${ldd_output}"

missing=()
while IFS= read -r line; do
  [[ "${line}" == *"=> not found"* ]] || continue
  lib="$(awk '{print $1}' <<<"${line}")"
  [[ -n "${lib}" ]] && missing+=("${lib}")
done <<< "${ldd_output}"
if [[ "${#missing[@]}" -gt 0 ]]; then
  echo "missing runtime dependencies: ${missing[*]}" >&2
  exit 1
fi

violations=()
while IFS= read -r line; do
  [[ "${line}" == *"=>"* ]] || continue
  lib="$(awk '{print $1}' <<<"${line}")"
  path="$(awk '{print $3}' <<<"${line}")"
  [[ -n "${lib}" && -n "${path}" && "${path}" != "not" ]] || continue
  if [[ -f "${DIR}/${lib}" ]]; then
    continue
  fi
  if [[ "${lib}" == libQt6*.so.* ]]; then
    continue
  fi
  if is_allowed_system_lib "${lib}"; then
    continue
  fi
  violations+=("${lib} -> ${path}")
done <<< "${ldd_output}"
if [[ "${#violations[@]}" -gt 0 ]]; then
  printf 'non-bundled runtime deps detected:\n' >&2
  printf '  %s\n' "${violations[@]}" >&2
  exit 1
fi

python3 - "${SO}" <<'PY'
import ctypes
import sys

so_path = sys.argv[1]
ctypes.CDLL(so_path, mode=ctypes.RTLD_GLOBAL)
print(f"smoke-load ok: {so_path}")
PY
