#!/usr/bin/env python3
"""
Toolchain Installation Script for N64 Decompilation.

Downloads and extracts legacy compiler toolchains (such as GCC 2.7.2)
into the `toolchain/` directory.
"""

import io
import os
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

if "BUILD_WORKSPACE_DIRECTORY" in os.environ:
    REPO_ROOT = Path(os.environ["BUILD_WORKSPACE_DIRECTORY"])
else:
    REPO_ROOT = Path(__file__).resolve().parent.parent

TOOLCHAIN_DIR = REPO_ROOT / "toolchain"
GCC_272_DIR = TOOLCHAIN_DIR / "gcc-2.7.2"
GCC_272_URL = "https://github.com/decompals/mips-gcc-2.7.2/releases/download/v0.1/gcc-2.7.2-linux.tar.gz"


def find_tool(name: str) -> str:
    """Find a tool executable in PATH."""
    res = subprocess.run(["which", name], capture_output=True, text=True)
    if res.returncode == 0:
        return res.stdout.strip()
    return name


def get_gcc_272(require_installed: bool = True) -> Path:
    """Get path to GCC 2.7.2 binary without auto-installing."""
    gcc_bin = GCC_272_DIR / "gcc"
    if require_installed and not gcc_bin.exists():
        raise FileNotFoundError(
            f"GCC 2.7.2 toolchain not found at {gcc_bin}.\n"
            "Please run 'bazel run //tools:install_toolchain' to install it."
        )
    return gcc_bin


def install_gcc_272() -> Path:
    """Explicitly download and install GCC 2.7.2 into toolchain/gcc-2.7.2."""
    gcc_bin = GCC_272_DIR / "gcc"
    print(f"[INFO] Installing GCC 2.7.2 toolchain to {GCC_272_DIR}...")
    GCC_272_DIR.mkdir(parents=True, exist_ok=True)
    req = urllib.request.Request(GCC_272_URL, headers={"User-Agent": "rom-nom-nom-builder"})
    with urllib.request.urlopen(req) as resp:
        content = resp.read()
    with tarfile.open(fileobj=io.BytesIO(content), mode="r:gz") as tar:
        tar.extractall(path=GCC_272_DIR)

    as_sym = GCC_272_DIR / "as"
    if not as_sym.exists():
        system_as = find_tool("mips-linux-gnu-as")
        try:
            os.symlink(system_as, as_sym)
        except OSError:
            pass
    print(f"[SUCCESS] GCC 2.7.2 toolchain ready at {GCC_272_DIR}")
    return gcc_bin


def main():
    target = sys.argv[1] if len(sys.argv) > 1 else "gcc-2.7.2"
    if target in ("gcc-2.7.2", "all"):
        gcc_bin = install_gcc_272()
        print(f"Installed: {gcc_bin}")
    else:
        print(f"Unknown toolchain target: {target}")
        sys.exit(1)


if __name__ == "__main__":
    main()
