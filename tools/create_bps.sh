#!/usr/bin/env bash
set -euo pipefail

if [ -n "${BUILD_WORKSPACE_DIRECTORY:-}" ]; then
    REPO_ROOT="${BUILD_WORKSPACE_DIRECTORY}"
else
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
fi

if [ -f "${REPO_ROOT}/.venv/bin/python3" ]; then
    PYTHON="${REPO_ROOT}/.venv/bin/python3"
else
    PYTHON="python3"
fi

exec "${PYTHON}" "${REPO_ROOT}/tools/create_bps.py" "$@"
