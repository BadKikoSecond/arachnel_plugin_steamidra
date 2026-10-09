#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck disable=SC1091
source "${ROOT}/scripts/ci/read-launcher-toolchain.sh"

BUILD="${BUILD_DIR:-${ROOT}/build-linux}"
SDK_DIR="${ARACHNEL_SDK_DIR:-${ROOT}/.ci/arachnel-sdk}"
QT_PATH="${QT_INSTALL_DIR:-${ROOT}/.ci/qt/${QT_VERSION}/gcc_64}"
DIST="${ROOT}/dist/linux"

cd "${ROOT}"
RELEASE_TAG="${RELEASE_TAG:-${CI_COMMIT_TAG:-}}"
[[ -z "${RELEASE_TAG}" ]] || python3 scripts/ci/set_plugin_version.py "${RELEASE_TAG}"

if [[ ! -f "${SDK_DIR}/cmake/ArachnelPluginSdk.cmake" ]]; then
  rm -rf "${SDK_DIR}"
  git clone --depth 1 --branch "${ARACHNEL_SDK_REF}" \
    https://github.com/BadKiko/Arachnel.git "${SDK_DIR}"
fi

DEPOTKIT_DIR="${DEPOTKIT_DIR:-${ROOT}/.ci/depotkit}"
if [[ ! -f "${DEPOTKIT_DIR}/CMakeLists.txt" ]]; then
  rm -rf "${DEPOTKIT_DIR}"
  git clone --depth 1 --branch "${DEPOTKIT_REF:-main}" \
    "${DEPOTKIT_GIT_URL:-https://github.com/BadKiko/depotkit.git}" "${DEPOTKIT_DIR}"
fi

if [[ ! -f "${QT_PATH}/lib/cmake/Qt6/Qt6Config.cmake" ]]; then
  AQT_VENV="${ROOT}/.ci/aqt-venv"
  [[ -x "${AQT_VENV}/bin/aqt" ]] || {
    python3 -m venv "${AQT_VENV}"
    "${AQT_VENV}/bin/pip" install --upgrade pip "git+https://github.com/miurahr/aqtinstall.git"
  }
  # shellcheck disable=SC2206
  MODULE_ARGS=(${QT_MODULES})
  "${AQT_VENV}/bin/aqt" install-qt linux desktop "${QT_VERSION}" "${QT_LINUX_ARCH}" \
    $(printf -- '-m %s ' "${MODULE_ARGS[@]}") \
    -O "$(dirname "$(dirname "${QT_PATH}")")"
fi

bash tools/fetch_third_party.sh linux
# Best-effort OF.me harvest into online_fix_kit/linux (falls back to SteamFix x64 in CMake).
bash tools/harvest_onlinefix_me_kit.sh || true
cmake -S "${ROOT}" -B "${BUILD}" -G Ninja \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DCMAKE_PREFIX_PATH="${QT_PATH}" \
  -DARACHNEL_SDK_DIR="${SDK_DIR}" \
  -DDEPOTKIT_DIR="${DEPOTKIT_DIR}"
cmake --build "${BUILD}" --target steamidra_arach -j"$(nproc)"
bash "${ROOT}/scripts/ci/check-linux-runtime.sh" "${BUILD}/plugin-bundle/libsteamidra_plugin.so"

# The host looks these up by name; a missing export silently drops back to the slow catalog_json
# path (extension) or refuses to load the plugin (API 4), so fail the build instead.
EXPORTS="$(nm -D --defined-only "${BUILD}/plugin-bundle/libsteamidra_plugin.so" | awk '{print $3}')"
for sym in arachnel_plugin_api_version arachnel_plugin_create arachnel_plugin_destroy \
           arachnel_plugin_catalog_json arachnel_plugin_catalog_json_free \
           arachnel_plugin_source_ext_version arachnel_plugin_catalog_source \
           arachnel_plugin_normalize_rows arachnel_plugin_source_free; do
  grep -qx "${sym}" <<<"${EXPORTS}" || { echo "missing plugin export: ${sym}" >&2; exit 1; }
done
echo "plugin exports OK"

mkdir -p "${DIST}"
cp -f "${BUILD}/dist/steamidra.arach" "${DIST}/steam.arach"
echo "Done: ${DIST}/steam.arach"
