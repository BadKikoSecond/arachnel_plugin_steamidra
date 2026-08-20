#!/usr/bin/env bash
set -euo pipefail

# Backward-compatible wrapper (older CI/local scripts call this name).
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "${ROOT}/linux-runtime-bundle.sh" "$@"
