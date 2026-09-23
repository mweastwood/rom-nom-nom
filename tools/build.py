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

if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.install_toolchain import GCC_272_DIR, get_gcc_272

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




def compile_with_gcc_272(src_path: Path, target_obj: Path, game_name: str, as_bin: str):
    """Compile C/C++ source using GCC 2.7.2 and post-process assembly for exact hardware match."""
    gcc_bin = get_gcc_272(require_installed=True)
    temp_s = target_obj.with_suffix(".s272")

    # Run GCC 2.7.2 in C mode to compile functions
    cmd = [
        str(gcc_bin), f"-B{GCC_272_DIR}/", "-x", "c", "-S", "-O0",
        f"-I{REPO_ROOT}",
        f"-I{ASM_DIR / game_name}",
        f"-I{SRC_DIR / game_name}",
        str(src_path), "-o", str(temp_s)
    ]
    subprocess.check_call(cmd, cwd=REPO_ROOT)

    with open(temp_s, "r", encoding="utf-8") as f:
        lines = f.readlines()

    new_lines = []
    i = 0
    while i < len(lines):
        line = lines[i]
        m_la = re.match(r"^\tla\t(\$[a-z0-9]+),([A-Za-z0-9_]+)\s*$", line)
        m_sw = re.match(r"^\tsw\t(\$[a-z0-9]+),([A-Za-z0-9_]+)\s*$", line)

        if m_la and i + 1 < len(lines) and lines[i+1].startswith("\tjal\t"):
            reg, sym = m_la.group(1), m_la.group(2)
            jal_line = lines[i+1]
            new_lines.append("\t.set noreorder\n")
            new_lines.append(f"\tlui\t{reg},%hi({sym})\n")
            new_lines.append(jal_line)
            new_lines.append(f"\taddiu\t{reg},{reg},%lo({sym})\n")
            new_lines.append("\t.set reorder\n")
            i += 2
            continue
        elif m_sw and i + 1 < len(lines) and lines[i+1].startswith("\tjal\t"):
            reg, sym = m_sw.group(1), m_sw.group(2)
            jal_line = lines[i+1]
            new_lines.append("\t.set noreorder\n")
            new_lines.append(f"\tlui\t$at,%hi({sym})\n")
            new_lines.append(jal_line)
            new_lines.append(f"\tsw\t{reg},%lo({sym})($at)\n")
            new_lines.append("\t.set reorder\n")
            i += 2
            continue

        # Replace pseudo move with hardware addu
        line = re.sub(r"\bmove\s+(\$[a-z0-9]+)\s*,\s*(\$[a-z0-9]+)", r"addu \1, \2, $0", line)
        new_lines.append(line)
        i += 1

    with open(temp_s, "w", encoding="utf-8") as f:
        f.writelines(new_lines)

    as_cmd = [
        as_bin, "-march=vr4300", "-mabi=32", "-EB",
        f"-I{ASM_DIR / game_name}",
        str(temp_s), "-o", str(target_obj)
    ]
    subprocess.check_call(as_cmd, cwd=REPO_ROOT)
    temp_s.unlink(missing_ok=True)


def build_and_verify(game_name: str, toolchain: str = "original", is_test: bool = False) -> bool:
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
    elf_file = game_build_dir / f"{game_name}.{toolchain}.elf"
    out_rom = game_build_dir / f"{game_name}.{toolchain}.z64"
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

    toolchain_defs = []
    if toolchain == "modern":
        toolchain_defs = ["-DMODERN_TOOLCHAIN=1", "-DNON_MATCHING=1"]
    else:
        toolchain_defs = ["-DORIGINAL_TOOLCHAIN=1"]

    # 3. Compile / Assemble each object
    print(f"=== Building {config_data.get('name', game_name)} [{toolchain} toolchain] ({len(needed_objs)} objects) ===")
    for rel_obj in needed_objs:
        target_obj = game_build_dir / rel_obj
        target_obj.parent.mkdir(parents=True, exist_ok=True)

        base_no_ext = rel_obj[:-2]
        cc_src = REPO_ROOT / f"{base_no_ext}.cc"
        cpp_src = REPO_ROOT / f"{base_no_ext}.cpp"
        c_src = REPO_ROOT / f"{base_no_ext}.c"
        s_src = REPO_ROOT / f"{base_no_ext}.s"
        bin_src = REPO_ROOT / f"{base_no_ext}.bin"

        src_to_compile = None
        if cc_src.exists():
            src_to_compile = cc_src
        elif cpp_src.exists():
            src_to_compile = cpp_src
        elif c_src.exists():
            src_to_compile = c_src

        if src_to_compile is not None:
            if toolchain == "original":
                compile_with_gcc_272(src_to_compile, target_obj, game_name, as_bin)
            else:
                compiler = gpp_bin if (cc_src.exists() or cpp_src.exists()) else gcc_bin
                cmd = [
                    compiler, "-c", "-march=vr4300", "-mabi=32", "-EB",
                    "-fno-PIC", "-mno-abicalls", "-ffreestanding",
                    f"-I{REPO_ROOT}",
                    f"-I{ASM_DIR / game_name}",
                    f"-I{SRC_DIR / game_name}",
                ] + toolchain_defs + [str(src_to_compile), "-o", str(target_obj)]
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
    print(f"\nBuilt ROM ({toolchain}): {out_rom.name}")
    print(f"Built ROM SHA-1:    {built_sha1}")

    if toolchain == "modern":
        print(f"[SUCCESS] Modern ROM built successfully!")
        return True

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
        print(f"Usage: {sys.argv[0]} <name-of-game> [--toolchain=modern|original] [--test]")
        sys.exit(0 if len(sys.argv) >= 2 and sys.argv[1] in ("-h", "--help") else 1)

    game_name = sys.argv[1]
    if game_name.endswith(".yaml"):
        game_name = game_name[:-5]
    if game_name.endswith(".z64"):
        game_name = game_name[:-4]

    toolchain = "original"
    is_test = False

    for arg in sys.argv[2:]:
        if arg == "--test":
            is_test = True
        elif arg.startswith("--toolchain="):
            toolchain = arg.split("=", 1)[1]
        elif arg == "--modern":
            toolchain = "modern"
        elif arg == "--original":
            toolchain = "original"

    success = build_and_verify(game_name, toolchain=toolchain, is_test=is_test)
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
