#!/usr/bin/env bash
# Publish this plugin build into arachnel-plugins-sourcelist (schema v2 builds[]).
# Requires: SOURCELIST_PUSH_TOKEN (project/group token with write_repository on sourcelist)
# Optional: DOWNLOAD_URL (defaults to GitLab generic package URL for this tag)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${ROOT}"

# shellcheck disable=SC1091
source "${ROOT}/scripts/ci/launcher-toolchain.env"

ARACH="${1:-}"
if [[ -z "${ARACH}" || ! -f "${ARACH}" ]]; then
  echo "usage: publish-sourcelist.sh path/to/plugin.arach" >&2
  exit 1
fi

if [[ -z "${SOURCELIST_PUSH_TOKEN:-}" ]]; then
  echo "SOURCELIST_PUSH_TOKEN is not set; skip sourcelist publish" >&2
  exit 0
fi

DOWNLOAD_URL="${DOWNLOAD_URL:-}"
MIN_ARACHNEL="${MIN_ARACHNEL:-0.1.34}"
MAX_ARACHNEL="${MAX_ARACHNEL:-}"
ABI_TOKEN="${ARACHNEL_SDK_REF:-main}"

SOURCELIST_HOST="${SOURCELIST_HOST:-${CI_SERVER_HOST:-gitlab.com}}"
SOURCELIST_PATH="${SOURCELIST_PATH:-BadKiko/arachnel-plugins-sourcelist}"
WORK="$(mktemp -d)"
cleanup() { rm -rf "${WORK}"; }
trap cleanup EXIT

git clone --depth 1 \
  "https://oauth2:${SOURCELIST_PUSH_TOKEN}@${SOURCELIST_HOST}/${SOURCELIST_PATH}.git" \
  "${WORK}/sourcelist"

ARGS=(
  --arach "${ARACH}"
  --min-arachnel "${MIN_ARACHNEL}"
  --abi-token "${ABI_TOKEN}"
)
if [[ -n "${DOWNLOAD_URL}" ]]; then
  ARGS+=(--url "${DOWNLOAD_URL}")
fi
# Clear upper bound unless explicitly set.
ARGS+=(--max-arachnel "${MAX_ARACHNEL}")

python3 "${WORK}/sourcelist/tools/ingest_plugin_build.py" "${ARGS[@]}"

cd "${WORK}/sourcelist"
git config user.email "ci@gitlab.com"
git config user.name "Arachnel Plugin CI"
if git diff --quiet -- plugins.json; then
  echo "plugins.json unchanged"
  exit 0
fi
git add plugins.json
PLUGIN_NAME="$(basename "${ARACH}")"
git commit -m "ci: ingest ${PLUGIN_NAME} ${CI_COMMIT_TAG:-build} into plugins.json"
git push "https://oauth2:${SOURCELIST_PUSH_TOKEN}@${SOURCELIST_HOST}/${SOURCELIST_PATH}.git" "HEAD:main"
echo "Published to sourcelist"
