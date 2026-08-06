#!/usr/bin/env bash
set -euo pipefail

SO="${1:?usage: linux-runtime-bundle.sh <plugin.so> [plugin-dir]}"
DIR="${2:-$(cd "$(dirname "${SO}")" && pwd)}"
SO="${DIR}/$(basename "${SO}")"

if [[ ! -f "${SO}" ]]; then
  echo "missing plugin library: ${SO}" >&2
  exit 1
fi
if ! command -v patchelf >/dev/null 2>&1; then
  echo "patchelf is required" >&2
  exit 1
fi

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

is_bundle_candidate() {
  local name="$1"
  if is_allowed_system_lib "${name}"; then
    return 1
  fi
  case "${name}" in
    libQt6*.so.*)
      # Qt comes from Arachnel runtime/AppImage.
      return 1
      ;;
  esac
  return 0
}

bundle_deps() {
  local src="$1"
  ldd "${src}" | while IFS= read -r line; do
    [[ "${line}" == *"=>"* ]] || continue
    local name path
    name="$(awk '{print $1}' <<<"${line}")"
    path="$(awk '{print $3}' <<<"${line}")"
    [[ -n "${name}" ]] || continue
    if [[ "${path}" == "not" || -z "${path}" ]]; then
      continue
    fi
    [[ -f "${path}" ]] || continue
    if ! is_bundle_candidate "${name}"; then
      continue
    fi
    cp -aL "${path}" "${DIR}/${name}"
    echo "bundled ${name} <- ${path}"
  done
}

bundle_deps "${SO}"

# Also patch bundled shared libs to load peers from plugin dir.
shopt -s nullglob
for dep in "${DIR}"/lib*.so*; do
  [[ -f "${dep}" ]] || continue
  patchelf --set-rpath '$ORIGIN' "${dep}" || true
done
patchelf --set-rpath '$ORIGIN' "${SO}"
echo "RUNPATH=$(patchelf --print-rpath "${SO}")"
