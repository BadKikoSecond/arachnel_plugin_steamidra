#!/usr/bin/env bash
set -euo pipefail

ENV_FILE="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/launcher-toolchain.env}"
[[ -f "${ENV_FILE}" ]] || { echo "Missing toolchain lock: ${ENV_FILE}" >&2; exit 1; }

while IFS= read -r line || [[ -n "${line}" ]]; do
  line="${line%%#*}"
  line="${line#"${line%%[![:space:]]*}"}"
  line="${line%"${line##*[![:space:]]}"}"
  [[ -z "${line}" ]] && continue
  [[ "${line}" == *=* ]] || continue
  key="${line%%=*}"
  value="${line#*=}"
  key="${key%"${key##*[![:space:]]}"}"
  if [[ "${value}" == \"*\" ]]; then
    value="${value:1:${#value}-2}"
  elif [[ "${value}" == \'*\' ]]; then
    value="${value:1:${#value}-2}"
  fi
  export "${key}=${value}"
done < "${ENV_FILE}"
