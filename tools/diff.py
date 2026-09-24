#!/usr/bin/env python3
"""Interactive assembly difference tool for N64 decompilation.

Compares the target original ROM/assembly instructions against recompiled C
object code side-by-side with color-coded diff highlighting.
"""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

if "BUILD_WORKSPACE_DIRECTORY" in os.environ:
    REPO_ROOT = Path(os.environ["BUILD_WORKSPACE_DIRECTORY"])
else:
    REPO_ROOT = Path(__file__).resolve().parent.parent

# ANSI Colors
COLOR_RESET = "\033[0m"
COLOR_GREEN = "\033[32m"
COLOR_YELLOW = "\033[33m"
COLOR_RED = "\033[31m"
COLOR_CYAN = "\033[36m"
COLOR_DIM = "\033[2m"
COLOR_BOLD = "\033[1m"


def find_tool(name: str) -> str:
    res = subprocess.run(["which", name], capture_output=True, text=True)
    if res.returncode == 0:
        return res.stdout.strip()
    return name


def load_symbol_table(game: str) -> dict[str, int]:
    """Load symbol addresses from built ELF or symbol file."""
    elf_path = REPO_ROOT / "bazel-bin" / f"{game}.elf"
    sym_map = {}

    if elf_path.exists():
        objdump = find_tool("mips-linux-gnu-objdump")
        res = subprocess.run([objdump, "-t", str(elf_path)], capture_output=True, text=True)
        if res.returncode == 0:
            for line in res.stdout.splitlines():
                parts = line.split()
                if len(parts) >= 6 and len(parts[0]) == 8:
                    try:
                        sym_map[parts[-1]] = int(parts[0], 16)
                    except ValueError:
                        pass
            return sym_map

    symbols_file = REPO_ROOT / "symbols" / f"{game}.txt"
    if symbols_file.exists():
        for line in symbols_file.read_text(encoding="utf-8").splitlines():
            line = line.split("//")[0].strip()
            if "=" in line:
                sym, val = line.split("=", 1)
                sym = sym.strip()
                val = val.strip().rstrip(";")
                try:
                    sym_map[sym] = int(val, 16 if "0x" in val else 10)
                except ValueError:
                    pass

    return sym_map


def get_symbol_info_from_elf(elf_path: Path, func_name: str):
    """Retrieve VRAM address and size for a function from the built ELF symbol table."""
    if not elf_path.exists():
        return None

    objdump = find_tool("mips-linux-gnu-objdump")
    res = subprocess.run([objdump, "-t", str(elf_path)], capture_output=True, text=True)
    if res.returncode != 0:
        return None

    pattern = re.compile(
        r"^([0-9a-fA-F]{8})\s+[g|l]\s+[F|O]\s+\S+\s+([0-9a-fA-F]{8})\s+"
        + re.escape(func_name)
        + r"$"
    )
    for line in res.stdout.splitlines():
        m = pattern.match(line)
        if m:
            vram = int(m.group(1), 16)
            size = int(m.group(2), 16)
            return vram, size
    return None


def get_vram_and_rom_offset(game: str, vram: int):
    """Convert VRAM address to ROM offset based on game segment mapping."""
    if game == "harvest-moon-64":
        return vram - 0x80025C50 + 0x1050
    elif game == "ogre-battle-64":
        return vram - 0x80070C60 + 0x1060
    return None


def find_function_in_c(game: str, func_name: str) -> tuple[Path, str]:
    """Find the C source file defining or declaring the given function."""
    src_dir = REPO_ROOT / "src" / "c" / game
    for cf in sorted(src_dir.glob("*.c")):
        content = cf.read_text(encoding="utf-8")
        # Reverse alias: func_800XXXXX(...) __attribute__((alias("MyFunction")));
        rev_pattern = re.compile(
            r"\b" + re.escape(func_name) + r"\b[^\n;]*alias\([\"']([A-Za-z0-9_]+)[\"']\)"
        )
        rev = rev_pattern.search(content)
        if rev:
            return cf, rev.group(1)
        # Check alias declarations
        alias_pattern = re.compile(
            r"\b([A-Za-z0-9_]+)\b[^\n;]*alias\([\"']" + re.escape(func_name) + r"[\"']\)"
        )
        m = alias_pattern.search(content)
        if m:
            return cf, func_name
        if re.search(r"\b" + re.escape(func_name) + r"\b\s*\(", content):
            return cf, func_name
    return None, func_name


def find_function_in_asm(game: str, func_name: str):
    """Find target function in generated asm files."""
    asm_dirs = [
        REPO_ROOT / "bazel-bin" / "asm" / game,
        REPO_ROOT / "asm" / game,
    ]
    for d in asm_dirs:
        if not d.exists():
            continue
        for sf in sorted(d.glob("*.s")):
            content = sf.read_text(encoding="utf-8")
            m = re.search(
                r"glabel\s+" + re.escape(func_name) + r"\n(.*?)\nendlabel", content, re.DOTALL
            )
            if m:
                return sf, m.group(1)
    return None, None


def disassemble_target_from_rom(rom_path: Path, rom_offset: int, size: int):
    """Disassemble target instructions directly from the original ROM."""
    if not rom_path.exists() or size <= 0:
        return []

    rom_bytes = rom_path.read_bytes()[rom_offset : rom_offset + size]
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        f.write(rom_bytes)
        temp_bin = Path(f.name)

    objdump = find_tool("mips-linux-gnu-objdump")
    try:
        res = subprocess.run(
            [objdump, "-b", "binary", "-m", "mips:4300", "-EB", "-Dz", str(temp_bin)],
            capture_output=True,
            text=True,
            check=True,
        )
        instructions = []
        for line in res.stdout.splitlines():
            if "\t" in line:
                parts = line.split("\t", 2)
                if len(parts) >= 3:
                    instructions.append(parts[2].strip())
        return instructions
    finally:
        temp_bin.unlink(missing_ok=True)


def load_c_flags(game: str, stem: str) -> tuple[list[str], list[str]]:
    """Load compiler and assembler flags for a specific C file from splat config."""
    yaml_path = REPO_ROOT / "splat" / f"{game}.yaml"
    opt_flags = ["-O2", "-mips2", "-Wa,-O1"]
    if yaml_path.exists():
        try:
            import yaml
            cfg = yaml.safe_load(yaml_path.read_text(encoding="utf-8"))
            c_flags = cfg.get("c_flags", {})
            opt_flags = c_flags.get(stem, c_flags.get("default", opt_flags))
        except Exception:
            pass
    gcc_flags = [f for f in opt_flags if not f.startswith("-Wa,")]
    as_flags = [f[4:] for f in opt_flags if f.startswith("-Wa,")]
    if not as_flags:
        as_flags = ["-O1"]
    return gcc_flags, as_flags


def compile_and_disassemble_c(c_file: Path, func_name: str, game: str, sym_map: dict[str, int]):
    """Compile a single C file with GCC 2.7.2, resolve relocations, and extract instructions."""
    gcc_bin = REPO_ROOT / "toolchain" / "gcc-2.7.2" / "gcc"
    if not gcc_bin.exists():
        print(f"Error: GCC 2.7.2 not found at {gcc_bin}", file=sys.stderr)
        return []

    as_bin = find_tool("mips-linux-gnu-as")
    objdump_bin = find_tool("mips-linux-gnu-objdump")

    asm_dir = REPO_ROOT / "bazel-bin" / "asm" / game
    if not asm_dir.exists():
        asm_dir = REPO_ROOT / "asm" / game

    gcc_flags, as_flags = load_c_flags(game, c_file.stem)

    with tempfile.TemporaryDirectory() as tmp_dir:
        tmp_path = Path(tmp_dir)
        temp_s = tmp_path / "temp.s"
        temp_o = tmp_path / "temp.o"

        gcc_cmd = [
            str(gcc_bin),
            f"-B{gcc_bin.parent}/",
            "-x",
            "c",
            "-S",
            *gcc_flags,
            "-G",
            "0",
            f"-I{REPO_ROOT}",
            f"-I{REPO_ROOT / 'bazel-bin'}",
            f"-I{asm_dir}",
            f"-I{REPO_ROOT / 'src' / 'c' / game}",
            str(c_file),
            "-o",
            str(temp_s),
        ]
        res = subprocess.run(gcc_cmd, cwd=REPO_ROOT, capture_output=True, text=True)
        if res.returncode != 0:
            print(f"Compilation error:\n{res.stderr}", file=sys.stderr)
            return []

        macro_inc = asm_dir / "macro.inc"
        as_cmd = [
            as_bin,
            "-march=vr4300",
            "-mabi=32",
            "-EB",
            "-G",
            "0",
            *as_flags,
            f"-I{REPO_ROOT}",
            f"-I{REPO_ROOT / 'bazel-bin'}",
            f"-I{asm_dir}",
            f"-I{asm_dir.parent}",
            f"-I{asm_dir.parent.parent}",
        ]
        if macro_inc.exists():
            as_cmd.append(str(macro_inc))
        as_cmd.extend([str(temp_s), "-o", str(temp_o)])
        res = subprocess.run(as_cmd, cwd=REPO_ROOT, capture_output=True, text=True)
        if res.returncode != 0:
            print(f"Assembler error:\n{res.stderr}", file=sys.stderr)
            return []

        objdump_cmd = [objdump_bin, "-drz", str(temp_o)]
        res = subprocess.run(objdump_cmd, capture_output=True, text=True)
        if res.returncode != 0:
            return []

        # Parse instructions and pair with any relocations on the next line
        in_func = False
        raw_items = []
        target_label = f"<{func_name}>:"

        for line in res.stdout.splitlines():
            if target_label in line:
                in_func = True
                continue
            if in_func:
                if not line.strip() or (">:" in line and target_label not in line):
                    break
                if "R_MIPS" in line:
                    if raw_items:
                        raw_items[-1]["relocs"].append(line.strip())
                elif "\t" in line:
                    parts = line.split("\t", 2)
                    if len(parts) >= 3:
                        raw_items.append({"instr": parts[2].strip(), "relocs": []})

        # Apply relocations to instructions where symbols are known
        resolved_instructions = []
        for item in raw_items:
            instr = item["instr"]
            for r in item["relocs"]:
                # Example: 28: R_MIPS_HI16 D_801C3F00
                m_rel = re.search(r"R_MIPS_(\w+)\s+([A-Za-z0-9_]+)", r)
                if not m_rel:
                    continue
                rtype, rsym = m_rel.group(1), m_rel.group(2)
                if rsym in sym_map:
                    sym_val = sym_map[rsym]
                    if rtype == "HI16":
                        hi_val = (sym_val + 0x8000) >> 16
                        instr = re.sub(r"0x0", f"0x{hi_val:x}", instr)
                    elif rtype == "LO16":
                        lo_val = sym_val & 0xFFFF
                        if lo_val >= 0x8000:
                            lo_val -= 0x10000
                        # e.g. sh zero,36(at) -> sh zero, (lo + 36)(at)
                        m_mem = re.search(r"([-\d]+)\((\w+)\)", instr)
                        if m_mem:
                            addend = int(m_mem.group(1))
                            reg = m_mem.group(2)
                            actual_disp = lo_val + addend
                            instr = re.sub(
                                r"([-\d]+)\(" + reg + r"\)", f"{actual_disp}({reg})", instr
                            )
            resolved_instructions.append(instr)

        return resolved_instructions


def normalize_instruction(instr: str) -> str:
    """Normalize whitespace, aliases, and branch target formatting for comparison."""
    instr = re.sub(r"\s+", " ", instr.strip())
    # Move aliases
    instr = re.sub(r"^daddu\s+([^,]+),\s*([^,]+),\s*\$0$", r"move \1,\2", instr)
    instr = re.sub(r"^addu\s+([^,]+),\s*([^,]+),\s*\$0$", r"move \1,\2", instr)
    instr = re.sub(r"^or\s+([^,]+),\s*([^,]+),\s*\$0$", r"move \1,\2", instr)
    instr = re.sub(r"\s*,\s*", ",", instr)

    # Branch target label normalization:
    # 1. With offset: e.g. "beqz v0,4c0 <MessageClipSpan+0x18>" -> "beqz v0,0x18"
    #                      "bc1f a94 <AudioVoiceSetPitch+0x94>" -> "bc1f 0x94"
    instr = re.sub(
        r"([,\s])[0-9a-fA-F]+\s*<[^>]*\+0x([0-9a-fA-F]+)>",
        lambda m: f"{m.group(1)}0x{int(m.group(2), 16):x}",
        instr,
    )
    # 2. To function entry: e.g. "bnez v0,4a8 <MessageClipSpan>" -> "bnez v0,0x0"
    instr = re.sub(r"([,\s])[0-9a-fA-F]+\s*<[A-Za-z0-9_]+>", r"\g<1>0x0", instr)
    # 3. Plain hex address: e.g. "beqz v0,0x18"
    instr = re.sub(r",\s*0x([0-9a-fA-F]+)", lambda m: f",0x{int(m.group(1), 16):x}", instr)
    return instr



def run_diff(func_name: str, game: str = "harvest-moon-64", use_color: bool = True):
    elf_path = REPO_ROOT / "bazel-bin" / f"{game}.elf"
    rom_path = REPO_ROOT / "roms" / f"{game}.z64"
    sym_map = load_symbol_table(game)

    # 1. Determine target instructions
    target_instructions = []
    symbol_info = get_symbol_info_from_elf(elf_path, func_name)

    if symbol_info:
        vram, size = symbol_info
        rom_offset = get_vram_and_rom_offset(game, vram)
        if rom_offset is not None and size > 0:
            target_instructions = disassemble_target_from_rom(rom_path, rom_offset, size)

    if not target_instructions:
        asm_file, asm_body = find_function_in_asm(game, func_name)
        if asm_body:
            for l in asm_body.splitlines():
                parts = l.strip().split("*/")
                if len(parts) >= 2:
                    target_instructions.append(parts[1].strip())

    if not target_instructions:
        print(
            f"Error: Could not locate target instructions for function '{func_name}'.",
            file=sys.stderr,
        )
        return False

    # 2. Find and compile current C code
    c_file, actual_func = find_function_in_c(game, func_name)
    current_instructions = []
    if c_file:
        current_instructions = compile_and_disassemble_c(c_file, actual_func, game, sym_map)

    # 3. Print Header
    c_info = str(c_file.relative_to(REPO_ROOT)) if c_file else "Not yet in C"
    print(f"\nFunction: {COLOR_BOLD}{func_name}{COLOR_RESET} ({game})")
    print(f"Source:   {c_info}")
    print(f"Target:   {len(target_instructions)} instructions")
    print(f"Current:  {len(current_instructions)} instructions\n")

    col_width = 44
    hdr_target = "TARGET (Original ROM)".ljust(col_width)
    hdr_curr = "CURRENT (Compiled C)".ljust(col_width)
    sep = "=" * col_width + "   " + "=" * col_width

    print(f"{COLOR_DIM}{hdr_target}   {hdr_curr}{COLOR_RESET}")
    print(f"{COLOR_DIM}{sep}{COLOR_RESET}")

    max_len = max(len(target_instructions), len(current_instructions))
    matching_count = 0

    for i in range(max_len):
        offset_str = f"0x{i * 4:03X}: "
        target_raw = target_instructions[i] if i < len(target_instructions) else ""
        current_raw = current_instructions[i] if i < len(current_instructions) else ""

        target_norm = normalize_instruction(target_raw)
        current_norm = normalize_instruction(current_raw)

        target_col = (offset_str + target_norm).ljust(col_width)
        current_col = (offset_str + current_norm).ljust(col_width)

        if not target_raw:
            print(f"{' ' * col_width} | {COLOR_RED}{current_col}{COLOR_RESET} [EXTRA]")
        elif not current_raw:
            print(f"{COLOR_RED}{target_col}{COLOR_RESET} | {' ' * col_width} [MISSING]")
        elif target_norm == current_norm:
            matching_count += 1
            print(f"{COLOR_GREEN}{target_col} | {current_col} [OK]{COLOR_RESET}")
        else:
            print(f"{COLOR_YELLOW}{target_col} | {COLOR_RED}{current_col}{COLOR_RESET} [DIFF]")

    print(f"{COLOR_DIM}{sep}{COLOR_RESET}")
    pct = (matching_count / len(target_instructions) * 100.0) if target_instructions else 0.0

    if matching_count == len(target_instructions) == len(current_instructions):
        print(
            f"\n{COLOR_GREEN}{COLOR_BOLD}*** [MATCH 100%] {matching_count}/{len(target_instructions)} instructions match bit-exact! ***{COLOR_RESET}\n"
        )
        return True
    else:
        print(
            f"\n{COLOR_YELLOW}Match: {matching_count}/{len(target_instructions)} instructions ({pct:.1f}%){COLOR_RESET}\n"
        )
        return False


def main():
    parser = argparse.ArgumentParser(description="Assembly differ for N64 decompilation.")
    parser.add_argument("function", help="Target function name (e.g. MessageInit or func_800266C0)")
    parser.add_argument("--game", default="harvest-moon-64", help="Game identifier.")
    parser.add_argument(
        "--watch",
        action="store_true",
        help="Watch source file and re-diff automatically on save.",
    )
    args = parser.parse_args()

    if not args.watch:
        success = run_diff(args.function, args.game)
        return 0 if success else 1

    print(f"Watching for changes to function '{args.function}'... (Press Ctrl+C to stop)")
    last_mtime = 0
    c_file, _ = find_function_in_c(args.game, args.function)

    try:
        while True:
            current_mtime = c_file.stat().st_mtime if c_file and c_file.exists() else 0
            if current_mtime != last_mtime:
                print("\033[H\033[J", end="")
                run_diff(args.function, args.game)
                last_mtime = current_mtime
            time.sleep(0.5)
    except KeyboardInterrupt:
        print("\nStopped watcher.")
        return 0


if __name__ == "__main__":
    sys.exit(main())
