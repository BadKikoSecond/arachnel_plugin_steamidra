#!/usr/bin/env bash
# Local/manual helper. Release CI notifies BadKikoSecond/arachnel_plugins_sourcelist instead
# (see the publish-sourcelist job in .github/workflows/release.yml).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${ROOT}"

# shellcheck disable=SC1091
source "${ROOT}/scripts/ci/read-launcher-toolchain.sh"

ARACH="${1:-}"
if [[ -z "${ARACH}" || ! -f "${ARACH}" ]]; then
  echo "usage: publish-sourcelist.sh path/to/plugin.arach" >&2
  echo "Release workflow notifies BadKikoSecond/arachnel_plugins_sourcelist (secret SOURCELIST_DISPATCH_TOKEN, optional)." >&2
  exit 1
fi

echo "For releases, CI dispatches the sourcelist ingest workflow (the sourcelist also polls releases)." >&2
echo "Manual ingest: clone arachnel_plugins_sourcelist and run tools/ingest_plugin_build.py" >&2
exit 1
