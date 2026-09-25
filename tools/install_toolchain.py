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
BINUTILS_26_DIR = TOOLCHAIN_DIR / "binutils-2.6"
BINUTILS_26_URL = "https://github.com/decompals/mips-binutils-2.6/releases/latest/download/binutils-2.6-linux.tar.gz"


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


def get_kmc_as(require_installed: bool = True) -> Path:
    """Get path to authentic KMC binutils 2.6 assembler binary."""
    as_bin = BINUTILS_26_DIR / "as"
    if require_installed and not as_bin.exists():
        raise FileNotFoundError(
            f"KMC binutils 2.6 assembler not found at {as_bin}.\n"
            "Please run 'bazel run //tools:install_toolchain' to install it."
        )
    return as_bin


def install_binutils_26() -> Path:
    """Explicitly download and install KMC binutils 2.6 into toolchain/binutils-2.6."""
    as_bin = BINUTILS_26_DIR / "as"
    print(f"[INFO] Installing KMC binutils 2.6 toolchain to {BINUTILS_26_DIR}...")
    BINUTILS_26_DIR.mkdir(parents=True, exist_ok=True)
    req = urllib.request.Request(BINUTILS_26_URL, headers={"User-Agent": "rom-nom-nom-builder"})
    with urllib.request.urlopen(req) as resp:
        content = resp.read()
    with tarfile.open(fileobj=io.BytesIO(content), mode="r:gz") as tar:
        tar.extractall(path=BINUTILS_26_DIR)
    as_bin.chmod(0o755)

    # Link authentic assembler into gcc-2.7.2/as
    GCC_272_DIR.mkdir(parents=True, exist_ok=True)
    as_sym = GCC_272_DIR / "as"
    if as_sym.is_symlink() or as_sym.exists():
        as_sym.unlink()
    try:
        os.symlink(as_bin, as_sym)
    except OSError:
        pass
    print(f"[SUCCESS] KMC binutils 2.6 toolchain ready at {BINUTILS_26_DIR}")
    return as_bin


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

    install_binutils_26()
    print(f"[SUCCESS] GCC 2.7.2 toolchain ready at {GCC_272_DIR}")
    return gcc_bin


def main():
    target = sys.argv[1] if len(sys.argv) > 1 else "all"
    if target in ("gcc-2.7.2", "all"):
        gcc_bin = install_gcc_272()
        print(f"Installed GCC: {gcc_bin}")
    elif target in ("binutils-2.6", "as"):
        as_bin = install_binutils_26()
        print(f"Installed AS: {as_bin}")
    else:
        print(f"Unknown toolchain target: {target}")
        sys.exit(1)


if __name__ == "__main__":
    main()
