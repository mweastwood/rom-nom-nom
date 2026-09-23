#!/usr/bin/env python3
"""Run clang-tidy on C/C++ files to verify Google style and naming conventions."""

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


def get_compile_args(file_path: Path):
    includes = [
        f"-I{REPO_ROOT}",
        f"-I{REPO_ROOT / 'src' / 'c' / 'harvest-moon-64'}",
        f"-I{REPO_ROOT / 'src' / 'cc' / 'harvest-moon-64'}",
        f"-I{REPO_ROOT / 'mocks' / 'ultra64'}",
    ]
    # Check if googletest headers are available
    gtest_candidates = [
        REPO_ROOT / "bazel-rom-nom-nom" / "external" / "googletest+" / "googletest" / "include",
    ]
    symlink = REPO_ROOT / "bazel-rom-nom-nom"
    if symlink.is_symlink():
        output_base = symlink.resolve().parent.parent
        for p in (output_base / "external").glob("*googletest*/googletest/include"):
            gtest_candidates.append(p)

    for gtest_dir in gtest_candidates:
        if gtest_dir.exists():
            includes.append(f"-I{gtest_dir}")
            break

    is_cpp = (
        file_path.suffix in {".cc", ".cpp"}
        or "src/cc" in str(file_path)
        or "tests" in str(file_path)
    )

    if is_cpp:
        args = ["-x", "c++", "-std=c++20"] if file_path.suffix == ".h" else ["-std=c++20"]
    else:
        args = ["-x", "c", "-ffreestanding"] if file_path.suffix == ".h" else ["-ffreestanding"]

    return args + includes


def main():
    parser = argparse.ArgumentParser(description="Run clang-tidy to verify Google style conventions.")
    parser.add_argument("--fix", action="store_true", help="Automatically apply clang-tidy fixes.")
    parser.add_argument("--check", action="store_true", help="Fail if any warning/error is detected.")
    parser.add_argument("files", nargs="*", type=Path, help="Specific files to check (optional).")
    args = parser.parse_args()

    tidy_bin = subprocess.run(["which", "clang-tidy"], capture_output=True, text=True)
    if tidy_bin.returncode != 0:
        print("[ERROR] clang-tidy not found in PATH.", file=sys.stderr)
        return 1
    clang_tidy = tidy_bin.stdout.strip()

    target_files = [f.resolve() for f in args.files] if args.files else find_files()
    if not target_files:
        print("No C/C++ files to check.")
        return 0

    has_errors = False
    print(f"=== Running clang-tidy on {len(target_files)} files ===")
    for f in target_files:
        extra_args = get_compile_args(f)
        cmd = [clang_tidy]
        if args.fix:
            cmd.append("-fix")
        if args.check:
            cmd.append("-warnings-as-errors=*")
        cmd.append(str(f))
        cmd.append("--")
        cmd.extend(extra_args)

        res = subprocess.run(cmd, cwd=REPO_ROOT, capture_output=True, text=True)
        if res.returncode != 0 or "error:" in res.stderr or "error:" in res.stdout:
            has_errors = True
            print(f"\n[FAIL] {f.relative_to(REPO_ROOT)}:")
            if res.stdout.strip():
                print(res.stdout.strip())
            if res.stderr.strip():
                print(res.stderr.strip(), file=sys.stderr)
        elif res.stdout.strip():
            if args.check:
                has_errors = True
                print(f"\n[WARN] {f.relative_to(REPO_ROOT)}:")
                print(res.stdout.strip())

    if has_errors:
        print("\n[ERROR] clang-tidy found style/naming violations.", file=sys.stderr)
        return 1

    print("[SUCCESS] All files conform to Google style conventions.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
