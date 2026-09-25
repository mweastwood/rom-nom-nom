#!/usr/bin/env python3
"""
N64 ROM Builder and Bit-Exact Verification Tool for Bazel.

Compiles C sources using legacy or modern toolchain,
assembles MIPS assembly from Bazel-generated asm directories,
converts binary assets, links the final ELF, extracts the ROM,
and optionally verifies bit-for-byte matching.
"""

import argparse
import hashlib
import os
import re
import shutil
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


def compute_sha1(file_path: Path) -> str:
    h = hashlib.sha1()
    with open(file_path, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest().lower()


def find_tool(name: str) -> str:
    venv_tool = REPO_ROOT / ".venv" / "bin" / name
    if venv_tool.exists() and os.access(venv_tool, os.X_OK):
        return str(venv_tool)
    res = subprocess.run(["which", name], capture_output=True, text=True)
    if res.returncode == 0:
        return res.stdout.strip()
    return name


def generate_symbols_ld(symbols_file: Path, config_data: dict, out_file: Path):
    ctx = spimdisasm.common.Context()
    opts = config_data.get("options", {})

    if opts.get("hardware_regs", False):
        ctx.globalSegment.fillHardwareRegs(True)
    if opts.get("libultra_symbols", False):
        ctx.globalSegment.fillLibultraSymbols()

    symbols = {}
    for vram, sym in ctx.globalSegment.symbols.items():
        symbols[sym.name] = vram

    if symbols_file.exists():
        with open(symbols_file, "r", encoding="utf-8") as f:
            for line in f:
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


def compile_with_gcc_272(
    src_path: Path,
    target_obj: Path,
    asm_dir: Path,
    as_bin: str,
    opt_flags: list,
    extra_includes: list,
):
    gcc_bin = get_gcc_272(require_installed=True)
    temp_s = target_obj.with_suffix(".s272")

    gcc_flags = [f for f in opt_flags if not f.startswith("-Wa,")]
    as_extra_flags = [f[4:] for f in opt_flags if f.startswith("-Wa,")]

    cmd = [
        str(gcc_bin),
        f"-B{GCC_272_DIR}/",
        "-x",
        "c",
        "-S",
        *gcc_flags,
        "-G",
        "0",
        f"-I{REPO_ROOT}",
        f"-I{asm_dir}",
    ]
    for inc in extra_includes:
        cmd.append(f"-I{inc}")
    cmd.extend([str(src_path), "-o", str(temp_s)])
    subprocess.check_call(cmd, cwd=REPO_ROOT)

    macro_inc = asm_dir / "macro.inc"
    as_cmd = [
        as_bin,
        "-march=vr4300",
        "-mabi=32",
        "-EB",
        "-G",
        "0",
        *as_extra_flags,
        f"-I{asm_dir}",
        f"-I{asm_dir.parent}",
        f"-I{asm_dir.parent.parent}",
    ]
    if macro_inc.exists():
        as_cmd.append(str(macro_inc))
    as_cmd.extend([str(temp_s), "-o", str(target_obj)])
    subprocess.check_call(as_cmd, cwd=REPO_ROOT)
    temp_s.unlink(missing_ok=True)


def build_rom(
    game_name: str,
    config_path: Path,
    symbols_path: Path,
    asm_dir: Path,
    build_dir: Path,
    assets_dir: Path,
    out_elf: Path,
    out_rom: Path,
    toolchain: str = "original",
    verify_rom: Path = None,
    is_test: bool = False,
) -> bool:
    with open(config_path, "r", encoding="utf-8") as f:
        config_data = yaml.safe_load(f)

    expected_sha1 = config_data.get("sha1", "").lower()
    ld_script = build_dir / f"{game_name}.ld"
    if not ld_script.exists():
        candidates = list(build_dir.glob("*.ld"))
        if candidates:
            ld_script = candidates[0]
        else:
            raise FileNotFoundError(f"Linker script not found in {build_dir}")

    # Isolated object working directory next to out_elf
    obj_dir = out_elf.parent / f"_{game_name}_objs"
    if obj_dir.exists():
        shutil.rmtree(obj_dir)
    obj_dir.mkdir(parents=True, exist_ok=True)

    # 1. Generate symbols.ld in obj_dir
    symbols_ld = obj_dir / "symbols.ld"
    generate_symbols_ld(symbols_path, config_data, symbols_ld)

    # 2. Parse needed objects and rewrite LD script
    ld_content = ld_script.read_text(encoding="utf-8")
    needed_obj_matches = sorted(set(re.findall(r"([^\s()]+?\.o)", ld_content)))

    # Map object names to destination paths in obj_dir
    obj_map = {}
    for obj_match in needed_obj_matches:
        obj_name = Path(obj_match).name
        obj_map[obj_match] = (obj_dir / obj_name).resolve()

    # Rewrite linker script pointing to obj_dir
    def replace_obj_path(m):
        full_match = m.group(1)
        if full_match in obj_map:
            return str(obj_map[full_match])
        return str(obj_dir / Path(full_match).name)

    rewritten_ld_content = re.sub(r"([^\s()]+\.o)", replace_obj_path, ld_content)
    rewritten_ld_path = obj_dir / f"{game_name}.ld"
    rewritten_ld_path.write_text(rewritten_ld_content, encoding="utf-8")

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

    extra_includes = [
        REPO_ROOT / "src" / game_name,
        REPO_ROOT / "src" / "c" / game_name,
        REPO_ROOT / "src" / "cc" / game_name,
    ]

    print(f"=== Building {config_data.get('name', game_name)} [{toolchain} toolchain] ({len(obj_map)} objects) ===")
    for obj_match, target_obj in obj_map.items():
        stem = target_obj.stem

        c_candidates = list((REPO_ROOT / "src").glob(f"**/{stem}.c"))
        cc_candidates = list((REPO_ROOT / "src").glob(f"**/{stem}.cc")) + list((REPO_ROOT / "src").glob(f"**/{stem}.cpp"))
        s_candidates = list(asm_dir.glob(f"**/{stem}.s"))
        bin_candidates = list(assets_dir.glob(f"**/{stem}.bin"))

        if c_candidates or cc_candidates:
            src_file = c_candidates[0] if c_candidates else cc_candidates[0]
            if toolchain == "original":
                c_flags_cfg = config_data.get("c_flags", {})
                file_opt = c_flags_cfg.get(src_file.stem, c_flags_cfg.get("default", ["-O2", "-mips2", "-Wa,-O1"]))
                compile_with_gcc_272(src_file, target_obj, asm_dir, as_bin, file_opt, extra_includes)
            else:
                compiler = gpp_bin if cc_candidates else gcc_bin
                cmd = [
                    compiler,
                    "-c",
                    "-march=vr4300",
                    "-mabi=32",
                    "-EB",
                    "-fno-PIC",
                    "-mno-abicalls",
                    "-ffreestanding",
                    f"-I{REPO_ROOT}",
                    f"-I{asm_dir}",
                ]
                for inc in extra_includes:
                    cmd.append(f"-I{inc}")
                cmd.extend(toolchain_defs)
                cmd.extend([str(src_file), "-o", str(target_obj)])
                subprocess.check_call(cmd, cwd=REPO_ROOT)
        elif s_candidates:
            s_src = s_candidates[0]
            cmd = [
                as_bin,
                "-march=vr4300",
                "-mabi=32",
                "-EB",
                f"-I{asm_dir}",
                f"-I{build_dir}",
                str(s_src),
                "-o",
                str(target_obj),
            ]
            subprocess.check_call(cmd, cwd=REPO_ROOT)
        elif bin_candidates:
            bin_src = bin_candidates[0]
            cmd = [
                objcopy_bin,
                "-I",
                "binary",
                "-O",
                "elf32-tradbigmips",
                "-B",
                "mips",
                str(bin_src),
                str(target_obj),
            ]
            subprocess.check_call(cmd, cwd=REPO_ROOT)
        else:
            raise FileNotFoundError(f"Source file not found for object: {obj_match}")

    # 3. Link ELF
    print("Linking ELF...")
    out_elf.parent.mkdir(parents=True, exist_ok=True)
    ld_cmd = [ld_bin, "-T", str(symbols_ld)]

    undef_syms = build_dir / "undefined_syms_auto.txt"
    if undef_syms.exists():
        ld_cmd += ["-T", str(undef_syms)]

    undef_funcs = build_dir / "undefined_funcs_auto.txt"
    if undef_funcs.exists():
        ld_cmd += ["-T", str(undef_funcs)]

    ld_cmd += ["-T", str(rewritten_ld_path), "--no-check-sections", "-o", str(out_elf)]
    subprocess.check_call(ld_cmd, cwd=REPO_ROOT)

    # 4. Extract ROM
    print("Extracting ROM binary...")
    out_rom.parent.mkdir(parents=True, exist_ok=True)
    subprocess.check_call([objcopy_bin, "-O", "binary", str(out_elf), str(out_rom)], cwd=REPO_ROOT)

    built_sha1 = compute_sha1(out_rom)
    print(f"\nBuilt ROM: {out_rom.name}")
    print(f"Built ROM SHA-1:    {built_sha1}")

    if toolchain == "modern":
        print("[SUCCESS] Modern ROM built successfully!")
        return True

    print(f"Expected ROM SHA-1: {expected_sha1}")
    sha1_matches = (built_sha1 == expected_sha1)
    byte_matches = True

    target_rom_candidate = verify_rom or (REPO_ROOT / config_data.get("options", {}).get("target_path", ""))
    if target_rom_candidate and Path(target_rom_candidate).exists():
        orig_bytes = Path(target_rom_candidate).read_bytes()
        built_bytes = out_rom.read_bytes()
        if orig_bytes != built_bytes:
            byte_matches = False
            diff_count = sum(1 for a, b in zip(orig_bytes, built_bytes) if a != b)
            diff_count += abs(len(orig_bytes) - len(built_bytes))
            print(f"[ERROR] Byte mismatch with target ROM ({diff_count} differing bytes)!")
            diffs = [(i, orig_bytes[i], built_bytes[i]) for i in range(min(len(orig_bytes), len(built_bytes))) if orig_bytes[i] != built_bytes[i]]
            for i, o, b in diffs[:20]:
                print(f"  ROM diff at 0x{i:06X}: target={o:02X} built={b:02X}")
        else:
            print("[SUCCESS] Byte-for-byte exact match against target ROM verified!")

    if sha1_matches and byte_matches:
        print(f"\n*** [MATCH OK] {game_name} matches 100% byte-for-byte! ***\n")
        return True
    else:
        print(f"\n*** [MATCH FAILED] {game_name} does not match! ***\n")
        if is_test:
            sys.exit(1)
        return False


def main():
    parser = argparse.ArgumentParser(description="Build N64 ROM from generated assembly and C sources.")
    parser.add_argument("--game", required=True, help="Game name, e.g. harvest-moon-64")
    parser.add_argument("--config", required=True, type=Path, help="Path to splat YAML config")
    parser.add_argument("--symbols", required=True, type=Path, help="Path to symbols file")
    parser.add_argument("--asm-dir", required=True, type=Path, help="Generated assembly directory")
    parser.add_argument("--build-dir", required=True, type=Path, help="Generated build directory (with LD script)")
    parser.add_argument("--assets-dir", required=True, type=Path, help="Generated assets directory")
    parser.add_argument("--out-elf", required=True, type=Path, help="Output ELF file path")
    parser.add_argument("--out-rom", required=True, type=Path, help="Output ROM (.z64) file path")
    parser.add_argument("--toolchain", default="original", choices=["original", "modern"], help="Toolchain to use")
    parser.add_argument("--verify-rom", type=Path, default=None, help="Original ROM to verify match against")
    parser.add_argument("--test", action="store_true", help="Fail with non-zero exit code if not byte-exact match")
    args = parser.parse_args()

    success = build_rom(
        game_name=args.game,
        config_path=args.config,
        symbols_path=args.symbols,
        asm_dir=args.asm_dir,
        build_dir=args.build_dir,
        assets_dir=args.assets_dir,
        out_elf=args.out_elf,
        out_rom=args.out_rom,
        toolchain=args.toolchain,
        verify_rom=args.verify_rom,
        is_test=args.test,
    )
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
