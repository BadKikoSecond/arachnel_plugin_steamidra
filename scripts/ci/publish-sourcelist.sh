#!/usr/bin/env bash
# Local/manual helper. Release CI triggers arachnel-plugins-sourcelist instead
# (see publish-sourcelist job → downstream ingest-plugin).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${ROOT}"

# shellcheck disable=SC1091
source "${ROOT}/scripts/ci/read-launcher-toolchain.sh"

ARACH="${1:-}"
if [[ -z "${ARACH}" || ! -f "${ARACH}" ]]; then
  echo "usage: publish-sourcelist.sh path/to/plugin.arach" >&2
  echo "Release pipelines trigger BadKiko/arachnel-plugins-sourcelist (no SOURCELIST_PUSH_TOKEN)." >&2
  exit 1
fi

echo "For releases, CI triggers the sourcelist ingest-plugin job." >&2
echo "Manual ingest: clone arachnel-plugins-sourcelist and run tools/ingest_plugin_build.py" >&2
exit 1
