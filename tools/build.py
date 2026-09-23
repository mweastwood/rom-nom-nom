#!/usr/bin/env python3
"""
N64 ROM Builder and Byte-Matching Verification Tool.

Compiles C++, assembles MIPS assembly, converts binary assets,
generates linker symbol scripts, links the final ELF, extracts the ROM,
and verifies byte-for-byte matching against the target ROM and expected SHA-1.
"""

import hashlib
import os
import re
import subprocess
import sys
from pathlib import Path
import spimdisasm
import yaml

if "BUILD_WORKSPACE_DIRECTORY" in os.environ:
    REPO_ROOT = Path(os.environ["BUILD_WORKSPACE_DIRECTORY"])
else:
    REPO_ROOT = Path(__file__).resolve().parent.parent

SPLAT_DIR = REPO_ROOT / "splat"
SYMBOLS_DIR = REPO_ROOT / "symbols"
SRC_DIR = REPO_ROOT / "src"
ASM_DIR = REPO_ROOT / "asm"
ASSETS_DIR = REPO_ROOT / "assets"
BUILD_DIR = REPO_ROOT / "build"
ROMS_DIR = REPO_ROOT / "roms"
VENV_BIN = REPO_ROOT / ".venv" / "bin"


def compute_sha1(file_path: Path) -> str:
    """Compute sha1 hash of a file."""
    h = hashlib.sha1()
    with open(file_path, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest().lower()


def find_tool(name: str) -> str:
    """Find a tool executable in virtualenv, PATH, or MIPS prefix."""
    # Check venv first
    venv_tool = VENV_BIN / name
    if venv_tool.exists() and os.access(venv_tool, os.X_OK):
        return str(venv_tool)
    # Check system
    res = subprocess.run(["which", name], capture_output=True, text=True)
    if res.returncode == 0:
        return res.stdout.strip()
    return name


def ensure_split(game_name: str, config_path: Path):
    """Ensure splat split has been performed so LD script and ASM exist."""
    ld_path = BUILD_DIR / game_name / f"{game_name}.ld"
    if not ld_path.exists():
        print(f"[{game_name}] Linker script not found at {ld_path}. Running split first...")
        splat_exe = find_tool("splat")
        rel_config = config_path.relative_to(REPO_ROOT)
        subprocess.check_call([splat_exe, "split", str(rel_config)], cwd=REPO_ROOT)


def generate_symbols_ld(game_name: str, config_data: dict, out_file: Path):
    """Generate linker script for hardware registers, libultra, and custom symbols."""
    ctx = spimdisasm.common.Context()
    opts = config_data.get("options", {})

    if opts.get("hardware_regs", False):
        ctx.globalSegment.fillHardwareRegs(True)
    if opts.get("libultra_symbols", False):
        ctx.globalSegment.fillLibultraSymbols()

    symbols = {}
    for vram, sym in ctx.globalSegment.symbols.items():
        symbols[sym.name] = vram

    # Read game-specific symbols.txt
    symbols_file = SYMBOLS_DIR / f"{game_name}.txt"
    if symbols_file.exists():
        with open(symbols_file, "r", encoding="utf-8") as f:
            for line in f:
                # Strip comments
                clean = re.sub(r"//.*", "", line).strip()
                m = re.match(r"^([a-zA-Z0-9_]+)\s*=\s*(0x[0-9a-fA-F]+)\s*;", clean)
                if m:
                    sym_name = m.group(1)
                    vram_val = int(m.group(2), 16)
                    symbols[sym_name] = vram_val

    out_file.parent.mkdir(parents=True, exist_ok=True)
    with open(out_file, "w", encoding="utf-8") as f:
        f.write("/* Auto-generated hardware registers and global symbol definitions */\n")
        for sym_name, vram_val in sorted(symbols.items()):
            f.write(f"{sym_name} = 0x{vram_val:08X};\n")


def build_and_verify(game_name: str, is_test: bool = False) -> bool:
    config_path = SPLAT_DIR / f"{game_name}.yaml"
    if not config_path.exists():
        print(f"Error: Splat config not found: {config_path}")
        return False

    with open(config_path, "r", encoding="utf-8") as f:
        config_data = yaml.safe_load(f)

    expected_sha1 = config_data.get("sha1", "").lower()
    options = config_data.get("options", {})
    target_rel = options.get("target_path", f"roms/{game_name}.z64")
    rom_path = (REPO_ROOT / target_rel).resolve()

    ensure_split(game_name, config_path)

    game_build_dir = BUILD_DIR / game_name
    ld_script = game_build_dir / f"{game_name}.ld"
    elf_file = game_build_dir / f"{game_name}.elf"
    out_rom = game_build_dir / f"{game_name}.z64"
    symbols_ld = game_build_dir / "symbols.ld"

    # 1. Generate linker symbol script
    generate_symbols_ld(game_name, config_data, symbols_ld)

    # 2. Parse required objects from LD script
    ld_content = ld_script.read_text(encoding="utf-8")
    obj_pattern = rf"build/{re.escape(game_name)}/([\w./\-]+/[^\s()]+?\.o)"
    needed_objs = sorted(set(re.findall(obj_pattern, ld_content)))

    as_bin = find_tool("mips-linux-gnu-as")
    gpp_bin = find_tool("mips-linux-gnu-g++")
    gcc_bin = find_tool("mips-linux-gnu-gcc")
    objcopy_bin = find_tool("mips-linux-gnu-objcopy")
    ld_bin = find_tool("mips-linux-gnu-ld")

    # 3. Compile / Assemble each object
    print(f"=== Building {config_data.get('name', game_name)} ({len(needed_objs)} objects) ===")
    for rel_obj in needed_objs:
        target_obj = game_build_dir / rel_obj
        target_obj.parent.mkdir(parents=True, exist_ok=True)

        base_no_ext = rel_obj[:-2]
        cc_src = REPO_ROOT / f"{base_no_ext}.cc"
        cpp_src = REPO_ROOT / f"{base_no_ext}.cpp"
        c_src = REPO_ROOT / f"{base_no_ext}.c"
        s_src = REPO_ROOT / f"{base_no_ext}.s"
        bin_src = REPO_ROOT / f"{base_no_ext}.bin"

        if cc_src.exists():
            cmd = [
                gpp_bin, "-c", "-march=vr4300", "-mabi=32", "-EB",
                "-fno-PIC", "-mno-abicalls",
                f"-I{REPO_ROOT}",
                f"-I{ASM_DIR / game_name}",
                f"-I{SRC_DIR / game_name}",
                str(cc_src), "-o", str(target_obj)
            ]
            subprocess.check_call(cmd, cwd=REPO_ROOT)
        elif cpp_src.exists():
            cmd = [
                gpp_bin, "-c", "-march=vr4300", "-mabi=32", "-EB",
                "-fno-PIC", "-mno-abicalls",
                f"-I{REPO_ROOT}",
                f"-I{ASM_DIR / game_name}",
                f"-I{SRC_DIR / game_name}",
                str(cpp_src), "-o", str(target_obj)
            ]
            subprocess.check_call(cmd, cwd=REPO_ROOT)
        elif c_src.exists():
            cmd = [
                gcc_bin, "-c", "-march=vr4300", "-mabi=32", "-EB",
                "-fno-PIC", "-mno-abicalls",
                f"-I{REPO_ROOT}",
                f"-I{ASM_DIR / game_name}",
                f"-I{SRC_DIR / game_name}",
                str(c_src), "-o", str(target_obj)
            ]
            subprocess.check_call(cmd, cwd=REPO_ROOT)
        elif s_src.exists():
            cmd = [
                as_bin, "-march=vr4300", "-mabi=32", "-EB",
                f"-I{ASM_DIR / game_name}",
                f"-I{game_build_dir}",
                str(s_src), "-o", str(target_obj)
            ]
            subprocess.check_call(cmd, cwd=REPO_ROOT)
        elif bin_src.exists():
            cmd = [
                objcopy_bin, "-I", "binary", "-O", "elf32-tradbigmips", "-B", "mips",
                str(bin_src), str(target_obj)
            ]
            subprocess.check_call(cmd, cwd=REPO_ROOT)
        else:
            raise FileNotFoundError(f"Source file not found for object: {rel_obj}")

    # 4. Link
    print("Linking ELF...")
    ld_cmd = [ld_bin, "-T", str(symbols_ld)]
    
    undef_syms = game_build_dir / "undefined_syms_auto.txt"
    if undef_syms.exists():
        ld_cmd += ["-T", str(undef_syms)]

    undef_funcs = game_build_dir / "undefined_funcs_auto.txt"
    if undef_funcs.exists():
        ld_cmd += ["-T", str(undef_funcs)]

    ld_cmd += [
        "-T", str(ld_script),
        "--no-check-sections",
        "-o", str(elf_file)
    ]
    subprocess.check_call(ld_cmd, cwd=REPO_ROOT)

    # 5. Extract ROM binary
    print("Extracting ROM binary...")
    subprocess.check_call([objcopy_bin, "-O", "binary", str(elf_file), str(out_rom)], cwd=REPO_ROOT)

    # 6. Verify Matching
    built_sha1 = compute_sha1(out_rom)
    print(f"\nBuilt ROM SHA-1:    {built_sha1}")
    print(f"Expected ROM SHA-1: {expected_sha1}")

    sha1_matches = (built_sha1 == expected_sha1)
    byte_matches = True

    if rom_path.exists():
        orig_bytes = rom_path.read_bytes()
        built_bytes = out_rom.read_bytes()
        if orig_bytes != built_bytes:
            byte_matches = False
            diff_count = sum(1 for a, b in zip(orig_bytes, built_bytes) if a != b)
            diff_count += abs(len(orig_bytes) - len(built_bytes))
            print(f"[ERROR] Byte mismatch with input ROM ({diff_count} differing bytes)!")
        else:
            print("[SUCCESS] Byte-for-byte exact match against input ROM verified!")
    else:
        print("[INFO] Input ROM not present locally; verified against pinned SHA-1 hash.")

    if sha1_matches and byte_matches:
        print(f"\n*** [MATCH OK] {game_name} matches 100% byte-for-byte! ***\n")
        return True
    else:
        print(f"\n*** [MATCH FAILED] {game_name} does not match! ***\n")
        return False


def main():
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        print(f"Usage: {sys.argv[0]} <name-of-game> [--test]")
        sys.exit(0 if len(sys.argv) >= 2 and sys.argv[1] in ("-h", "--help") else 1)

    game_name = sys.argv[1]
    if game_name.endswith(".yaml"):
        game_name = game_name[:-5]
    if game_name.endswith(".z64"):
        game_name = game_name[:-4]

    is_test = "--test" in sys.argv[2:]

    success = build_and_verify(game_name, is_test=is_test)
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
