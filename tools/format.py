#!/usr/bin/env python3
"""Format or verify C/C++ files using clang-format with Google style."""

import argparse
import os
from pathlib import Path
import subprocess
import sys

if "BUILD_WORKSPACE_DIRECTORY" in os.environ:
    REPO_ROOT = Path(os.environ["BUILD_WORKSPACE_DIRECTORY"])
else:
    REPO_ROOT = Path(__file__).resolve().parent.parent

SEARCH_DIRS = ["src", "mocks", "tests"]
EXTENSIONS = {".c", ".h", ".cc", ".cpp"}


def find_files():
    files = []
    for d in SEARCH_DIRS:
        dir_path = REPO_ROOT / d
        if not dir_path.exists():
            continue
        for root, _, filenames in os.walk(dir_path):
            for f in filenames:
                p = Path(root) / f
                if p.suffix in EXTENSIONS:
                    files.append(p)
    return sorted(files)


def main():
    parser = argparse.ArgumentParser(description="Run clang-format on codebase")
    parser.add_argument("--check", action="store_true", help="Check formatting without writing changes")
    args = parser.parse_args()

    files = find_files()
    if not files:
        print("No C/C++ files found.")
        return 0

    clang_format = "clang-format"
    cmd = [clang_format]

    if args.check:
        cmd.extend(["--dry-run", "--Werror"])
    else:
        cmd.append("-i")

    cmd.extend([str(f) for f in files])
    try:
        subprocess.check_call(cmd, cwd=REPO_ROOT)
        action = "verified" if args.check else "formatted"
        print(f"Successfully {action} {len(files)} files.")
        return 0
    except subprocess.CalledProcessError as e:
        print(f"clang-format failed with exit code {e.returncode}", file=sys.stderr)
        return e.returncode


if __name__ == "__main__":
    sys.exit(main())
