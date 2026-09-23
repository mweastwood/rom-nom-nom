#!/usr/bin/env bash
set -euo pipefail

# If invoked via 'bazel run', use the workspace directory
if [ -n "${BUILD_WORKSPACE_DIRECTORY:-}" ]; then
    REPO_ROOT="${BUILD_WORKSPACE_DIRECTORY}"
else
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
fi

# Use python from .venv if present, otherwise default to python3
if [ -f "${REPO_ROOT}/.venv/bin/python3" ]; then
    PYTHON="${REPO_ROOT}/.venv/bin/python3"
else
    PYTHON="python3"
fi

exec "${PYTHON}" "${REPO_ROOT}/tools/split.py" "$@"
