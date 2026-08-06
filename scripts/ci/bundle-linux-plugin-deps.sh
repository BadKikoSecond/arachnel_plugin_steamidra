#!/usr/bin/env bash
# Ship a libcurl that matches AppImage-era OpenSSL (3.0.x), not the host distro.
#
# Arachnel AppImage puts its libssl on LD_LIBRARY_PATH. Rolling hosts (CachyOS,
# Arch, Fedora) often ship a newer libcurl that needs OPENSSL_3.2+ — dlopen then
# fails while FreeTP (no curl) still loads. Bundle jammy-era curl next to the
# plugin and set RUNPATH=$ORIGIN so Arachnel can preload it before the .so.
set -euo pipefail

SO="${1:?usage: bundle-linux-plugin-deps.sh <libsteamidra_plugin.so>}"
DIR="$(cd "$(dirname "${SO}")" && pwd)"
SO="${DIR}/$(basename "${SO}")"

if [[ ! -f "${SO}" ]]; then
  echo "missing plugin library: ${SO}" >&2
  exit 1
fi

if ! command -v patchelf >/dev/null 2>&1; then
  echo "patchelf is required" >&2
  exit 1
fi

copy_dep() {
  local name="$1"
  local src
  src="$(ldd "${SO}" | awk -v n="${name}" '$1 == n { print $3; exit }')"
  if [[ -z "${src}" || "${src}" == "not" || ! -f "${src}" ]]; then
    echo "WARNING: could not resolve ${name} via ldd" >&2
    return 0
  fi
  cp -aL "${src}" "${DIR}/${name}"
  echo "bundled ${name} <- ${src}"
}

# Prefer the build-image curl (CI should be ubuntu:22.04 so OPENSSL_3.0 matches
# the Arachnel AppImage). Do not ship libssl/libcrypto - reuse AppImage's.
copy_dep libcurl.so.4

patchelf --set-rpath '$ORIGIN' "${SO}"
if [[ -f "${DIR}/libcurl.so.4" ]]; then
  patchelf --set-rpath '$ORIGIN' "${DIR}/libcurl.so.4" || true
fi

echo "RUNPATH=$(patchelf --print-rpath "${SO}")"
