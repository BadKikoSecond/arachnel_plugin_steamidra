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
[[ -z "${CI_COMMIT_TAG:-}" ]] || python3 scripts/ci/set_plugin_version.py "${CI_COMMIT_TAG}"

if [[ ! -f "${SDK_DIR}/cmake/ArachnelPluginSdk.cmake" ]]; then
  rm -rf "${SDK_DIR}"
  git clone --depth 1 --branch "${ARACHNEL_SDK_REF}" \
    https://github.com/BadKiko/Arachnel.git "${SDK_DIR}"
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
cmake -S "${ROOT}" -B "${BUILD}" -G Ninja \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DCMAKE_PREFIX_PATH="${QT_PATH}" \
  -DARACHNEL_SDK_DIR="${SDK_DIR}"
cmake --build "${BUILD}" --target steamidra_arach -j"$(nproc)"

mkdir -p "${DIST}"
cp -f "${BUILD}/dist/steamidra.arach" "${DIST}/steam.arach"
echo "Done: ${DIST}/steam.arach"
