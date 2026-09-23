#!/usr/bin/env python3
"""Unified MIPS-to-C decompiler runner with automatic on-the-fly context generation.

Preprocesses all project headers and types dynamically, locates the target function
in the generated assembly files, and invokes m2c to produce formatted C drafts.
"""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

if "BUILD_WORKSPACE_DIRECTORY" in os.environ:
    REPO_ROOT = Path(os.environ["BUILD_WORKSPACE_DIRECTORY"])
else:
    REPO_ROOT = Path(__file__).resolve().parent.parent


def find_m2c() -> str:
    """Locate the m2c binary in PATH or ~/.local/bin."""
    res = subprocess.run(["which", "m2c"], capture_output=True, text=True)
    if res.returncode == 0:
        return res.stdout.strip()
    user_m2c = Path.home() / ".local" / "bin" / "m2c"
    if user_m2c.exists() and os.access(user_m2c, os.X_OK):
        return str(user_m2c)
    return None


def generate_context_string(game: str = "harvest-moon-64") -> str:
    """Preprocess all project headers on the fly into a consolidated context string."""
    src_dir = REPO_ROOT / "src" / "c" / game
    ultra_mock = REPO_ROOT / "mocks" / "ultra64" / "ultra64_mock.h"

    lines = [
        "#define _LANGUAGE_C 1",
        "#define TARGET_N64 1",
    ]

    common_h = src_dir / "common.h"
    if common_h.exists():
        lines.append(f'#include "{common_h.resolve()}"')

    if ultra_mock.exists():
        lines.append(f'#include "{ultra_mock.resolve()}"')

    for h in sorted(src_dir.glob("*.h")):
        if h != common_h:
            lines.append(f'#include "{h.resolve()}"')

    with tempfile.NamedTemporaryFile(suffix=".c", mode="w", delete=False) as f:
        f.write("\n".join(lines) + "\n")
        temp_input = Path(f.name)

    cmd = [
        "gcc",
        "-E",
        "-P",
        "-ffreestanding",
        f"-I{REPO_ROOT}",
        f"-I{src_dir}",
        f"-I{REPO_ROOT / 'mocks' / 'ultra64'}",
        str(temp_input),
    ]

    try:
        res = subprocess.run(cmd, capture_output=True, text=True, check=True)
        return res.stdout
    except subprocess.CalledProcessError as e:
        print(f"Error generating context: {e.stderr}", file=sys.stderr)
        return ""
    finally:
        temp_input.unlink(missing_ok=True)


def find_function_asm(game: str, func_name: str) -> tuple[Path, str]:
    """Find which assembly file contains the target function label."""
    asm_dirs = [
        REPO_ROOT / "bazel-bin" / "asm" / game,
        REPO_ROOT / "asm" / game,
    ]
    pattern = re.compile(r"glabel\s+" + re.escape(func_name) + r"\b")

    for d in asm_dirs:
        if not d.exists():
            continue
        for sf in sorted(d.glob("*.s")):
            content = sf.read_text(encoding="utf-8")
            if pattern.search(content):
                return sf, content

    return None, None


def format_c_code(code: str) -> str:
    """Format C code using clang-format if available."""
    res = subprocess.run(["which", "clang-format"], capture_output=True, text=True)
    if res.returncode != 0:
        return code

    try:
        proc = subprocess.run(
            ["clang-format", "--style=file"],
            input=code,
            capture_output=True,
            text=True,
            cwd=REPO_ROOT,
        )
        if proc.returncode == 0:
            return proc.stdout
    except Exception:
        pass
    return code


def main():
    parser = argparse.ArgumentParser(
        description="Decompile a function from MIPS assembly to C using m2c with automatic on-the-fly context."
    )
    parser.add_argument("function", nargs="?", help="Function name to decompile (e.g. func_800266C0)")
    parser.add_argument("--game", default="harvest-moon-64", help="Game identifier.")
    parser.add_argument(
        "--dump-context",
        action="store_true",
        help="Print the generated C context to stdout (e.g. for decomp.me) and exit.",
    )
    parser.add_argument(
        "--target",
        default="mips-gcc-c",
        help="m2c target architecture/compiler triple (default: mips-gcc-c).",
    )
    args = parser.parse_args()

    # 1. Handle --dump-context
    if args.dump_context:
        ctx = generate_context_string(args.game)
        if not ctx:
            return 1
        sys.stdout.write(ctx)
        return 0

    if not args.function:
        parser.print_help()
        return 1

    # 2. Check m2c installation
    m2c_bin = find_m2c()
    if not m2c_bin:
        print(
            "Error: m2c not found in PATH or ~/.local/bin.\n"
            "Please install it via: pip install git+https://github.com/matt-kempster/m2c.git",
            file=sys.stderr,
        )
        return 1

    # 3. Locate target function in assembly files
    asm_file, _ = find_function_asm(args.game, args.function)
    if not asm_file:
        print(
            f"Error: Function '{args.function}' not found in assembly directories for {args.game}.\n"
            f"Make sure you have run 'bazel build //:harvest_moon_64_rom' so asm files are generated.",
            file=sys.stderr,
        )
        return 1

    # 4. Generate on-the-fly context in temporary file
    ctx_text = generate_context_string(args.game)
    with tempfile.NamedTemporaryFile(suffix=".c", mode="w", delete=False) as f:
        f.write(ctx_text)
        temp_ctx = Path(f.name)

    try:
        # 5. Run m2c
        cmd = [
            m2c_bin,
            "-t",
            args.target,
            "--context",
            str(temp_ctx),
            "-f",
            args.function,
            str(asm_file),
        ]
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            print(f"m2c error:\n{res.stderr}", file=sys.stderr)
            return res.returncode

        # 6. Format output with clang-format
        formatted = format_c_code(res.stdout)
        print(formatted)
        return 0
    finally:
        temp_ctx.unlink(missing_ok=True)
        # Clean up any m2c cache file
        cache_file = temp_ctx.with_suffix(".c.m2c")
        if cache_file.exists():
            cache_file.unlink(missing_ok=True)


if __name__ == "__main__":
    sys.exit(main())
