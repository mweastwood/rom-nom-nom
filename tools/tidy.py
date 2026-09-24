#!/usr/bin/env python3
"""Run clang-tidy on C/C++ files to verify Google style and naming conventions."""

import argparse
import os
from pathlib import Path
import re
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


def check_naming_integrity(files):
    """Enforces clean-room naming integrity:
    1. Prohibits __attribute__((alias(...))) across the codebase.
    2. Prohibits function definitions starting with func_800... in C/C++ sources.
    3. Prohibits any reference to default labels (D_XXXXXXXX or func_XXXXXXXX)
       when a human-readable symbol has been registered in symbols/*.txt.
    """
    errors = []

    symbols_dir = REPO_ROOT / "symbols"
    named_syms = {}
    if symbols_dir.exists():
        for sym_file in symbols_dir.glob("*.txt"):
            with open(sym_file, "r", encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    m = re.match(r"^([A-Za-z0-9_]+)\s*=\s*(0x[0-9A-Fa-f]+);", line)
                    if m:
                        name, addr_str = m.group(1), m.group(2)
                        addr = int(addr_str, 16)
                        d_sym = f"D_{addr:08X}"
                        func_sym = f"func_{addr:08X}"
                        if name != d_sym and not name.startswith("D_") and name != func_sym and not name.startswith("func_"):
                            named_syms[d_sym] = (name, sym_file.name)
                            named_syms[func_sym] = (name, sym_file.name)

    alias_pattern = re.compile(r"__attribute__\s*\(\s*\(\s*alias\s*\(")
    func_def_pattern = re.compile(r"^[a-zA-Z0-9_* ]+\s+(func_[0-9A-Fa-f]+)\s*\([^;]*\)\s*\{", re.MULTILINE)

    for f in files:
        if not f.exists():
            continue
        try:
            content = f.read_text(encoding="utf-8", errors="replace")
        except Exception:
            continue

        try:
            rel_path = f.relative_to(REPO_ROOT)
        except ValueError:
            rel_path = f

        # 1. Alias check
        if alias_pattern.search(content):
            errors.append(f"{rel_path}: uses __attribute__((alias(...))), which is prohibited. Define functions directly under their readable name and register them in symbols/<game>.txt.")

        # 2. Decompiled function definition check
        if f.suffix in {".c", ".cc", ".cpp"} and "mocks" not in f.parts:
            for match in func_def_pattern.finditer(content):
                sym = match.group(1)
                errors.append(f"{rel_path}: defines function with default label '{sym}'. Decompiled functions must use Google Style CamelCase and be mapped in symbols/<game>.txt.")

        # 3. Reference to renamed symbols check
        for default_sym, (clean_name, sym_file) in named_syms.items():
            if default_sym in content:
                for line_idx, line in enumerate(content.splitlines(), start=1):
                    if default_sym in line:
                        errors.append(f"{rel_path}:{line_idx}: references deprecated label '{default_sym}'. Use readable name '{clean_name}' (defined in symbols/{sym_file}).")

    return errors


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
    naming_errors = check_naming_integrity(target_files)
    if naming_errors:
        has_errors = True
        print("\n=== Naming & Symbol Integrity Check Failures ===")
        for err in naming_errors:
            print(f"[FAIL] {err}", file=sys.stderr)

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
