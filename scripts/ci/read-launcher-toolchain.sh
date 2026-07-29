#!/usr/bin/env bash
set -euo pipefail

# Never use $1 here: callers often `source` this while $1 is a .arach (zip).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENV_FILE="${TOOLCHAIN_ENV_FILE:-${SCRIPT_DIR}/launcher-toolchain.env}"
[[ -f "${ENV_FILE}" ]] || { echo "Missing toolchain lock: ${ENV_FILE}" >&2; exit 1; }

while IFS= read -r line || [[ -n "${line}" ]]; do
  line="${line%%#*}"
  line="${line#"${line%%[![:space:]]*}"}"
  line="${line%"${line##*[![:space:]]}"}"
  line="${line//$'\r'/}"
  [[ -z "${line}" ]] && continue
  [[ "${line}" == *=* ]] || continue
  key="${line%%=*}"
  value="${line#*=}"
  key="${key%"${key##*[![:space:]]}"}"
  key="${key//$'\r'/}"
  value="${value//$'\r'/}"
  if [[ "${value}" == \"*\" ]]; then
    value="${value:1:${#value}-2}"
  elif [[ "${value}" == \'*\' ]]; then
    value="${value:1:${#value}-2}"
  fi
  [[ "${key}" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]] || continue
  export "${key}=${value}"
done < "${ENV_FILE}"
