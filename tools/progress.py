#!/usr/bin/env python3
"""Calculate decompilation progress, detect module names, and track function-level roadmap."""

import argparse
import os
from pathlib import Path
import re
import sys
import yaml

if "BUILD_WORKSPACE_DIRECTORY" in os.environ:
    REPO_ROOT = Path(os.environ["BUILD_WORKSPACE_DIRECTORY"])
else:
    REPO_ROOT = Path(__file__).resolve().parent.parent


def parse_assert_strings(game: str) -> dict[str, str]:
    """Scan data/rodata assembly files to map assert symbols to original module names."""
    assert_map = {}
    asm_dirs = [
        REPO_ROOT / "bazel-bin" / "asm" / game / "data",
        REPO_ROOT / "asm" / game / "data",
    ]
    for d in asm_dirs:
        if not d.exists():
            continue
        for p in d.glob("*.s"):
            content = p.read_text(encoding="utf-8", errors="ignore")
            for m in re.finditer(
                r'dlabel\s+([A-Za-z0-9_]+)\s+(?:/\*[^*]+\*/\s*)?\.asciz\s+\"([^\"*]+/)?([^/\\\"]+)\.c\"',
                content,
            ):
                assert_map[m.group(1)] = m.group(3)

    return assert_map


def load_game_config(game: str):
    """Load splat config, symbol table, and identify code subsegments."""
    config_file = REPO_ROOT / "splat" / f"{game}.yaml"
    if not config_file.exists():
        print(f"Error: Splat config not found: {config_file}", file=sys.stderr)
        return None, None, None

    data = yaml.safe_load(config_file.read_text(encoding="utf-8"))
    options = data.get("options", {})

    # 1. Parse symbol addresses dynamically from symbol file(s)
    symbols = {}
    sym_paths = options.get("symbol_addrs_path", [f"symbols/{game}.txt"])
    if isinstance(sym_paths, str):
        sym_paths = [sym_paths]
    for sp in sym_paths:
        p = REPO_ROOT / sp
        if p.exists():
            for line in p.read_text(encoding="utf-8", errors="ignore").splitlines():
                m = re.match(r"^\s*([A-Za-z0-9_]+)\s*=\s*(0x[0-9A-Fa-f]+)\s*;", line)
                if m:
                    symbols[m.group(1)] = int(m.group(2), 16)

    # 2. Extract code subsegments across all code segments
    subsegs = []
    c_bytes = 0
    asm_bytes = 0

    for seg in data.get("segments", []):
        if not (isinstance(seg, dict) and "subsegments" in seg):
            continue

        raw_subsegs = seg["subsegments"]
        seg_start = seg.get("start", 0)
        seg_vram = seg.get("vram", 0)
        vram_base = seg_vram - seg_start

        # Identify where code ends (.text) before data/rodata/bss
        text_end = None
        for s in raw_subsegs:
            if isinstance(s, list) and len(s) >= 2 and s[1] in ("data", "rodata", "bss"):
                text_end = s[0]
                break
            elif isinstance(s, dict) and s.get("type") in ("data", "rodata", "bss"):
                text_end = s.get("start")
                break

        if text_end is None:
            if raw_subsegs and isinstance(raw_subsegs[-1], list):
                text_end = raw_subsegs[-1][0]
            elif raw_subsegs and isinstance(raw_subsegs[-1], dict):
                text_end = raw_subsegs[-1].get("start", 0)
            else:
                text_end = 0

        for i, s in enumerate(raw_subsegs):
            if not (isinstance(s, list) and len(s) >= 2):
                continue
            start, stype = s[0], s[1]
            if start >= text_end or stype not in ("c", "asm", "hasm"):
                continue

            next_start = text_end
            for j in range(i + 1, len(raw_subsegs)):
                ns = raw_subsegs[j]
                if isinstance(ns, list) and len(ns) >= 1:
                    next_start = ns[0]
                    break
                elif isinstance(ns, dict) and "start" in ns:
                    next_start = ns["start"]
                    break

            size = next_start - start
            name = s[2] if len(s) >= 3 else f"{start:X}"
            vram = vram_base + start
            end_vram = vram_base + next_start

            item = {
                "name": name,
                "start": start,
                "end": next_start,
                "size": size,
                "vram": vram,
                "end_vram": end_vram,
                "vram_base": vram_base,
                "type": stype,
                "module": name,
                "funcs": [],
                "outgoing": set(),
            }
            subsegs.append(item)

            if stype == "c":
                c_bytes += size
            elif stype in ("asm", "hasm"):
                asm_bytes += size

    # 3. Dynamic module detection from assert strings, splat names, and adjacent C modules
    assert_map = parse_assert_strings(game)
    asm_dirs = [REPO_ROOT / "bazel-bin" / "asm" / game, REPO_ROOT / "asm" / game]
    asm_dir = next((d for d in asm_dirs if d.exists()), None)
    src_dirs = [REPO_ROOT / "src" / "c" / game, REPO_ROOT / "src" / game]
    c_dir = next((d for d in src_dirs if d.exists()), None)

    for i, s in enumerate(subsegs):
        name = s["name"]
        is_hex = bool(re.match(r"^[0-9A-Fa-f]+$", name))
        if not is_hex:
            s["module"] = name
        else:
            s_file = (asm_dir / f"{name}.s") if asm_dir else None
            content = s_file.read_text(encoding="utf-8", errors="ignore") if (s_file and s_file.exists()) else ""
            found_mod = None
            for sym, m_name in assert_map.items():
                if sym in content:
                    found_mod = m_name
                    break

            if found_mod:
                s["module"] = found_mod
            elif i > 0 and subsegs[i - 1]["name"] in ("main", "message"):
                # Contiguous asm tail of a partially decompiled C module (e.g. 17B0 in main, 1E830 in message)
                s["module"] = subsegs[i - 1]["module"]
            elif len(subsegs) > 1 and i == len(subsegs) - 1 and len(re.findall(r"\bjal\s+os", content)) > 5:
                # Trailing Libultra OS SDK section
                s["module"] = "libultra"
            else:
                s["module"] = name

    game_info = {
        "game": game,
        "name": data.get("name", game),
        "options": options,
        "c_bytes": c_bytes,
        "asm_bytes": asm_bytes,
        "total_text": c_bytes + asm_bytes,
    }
    return game_info, subsegs, symbols


def is_sdk_routine(callee: str, callee_vram: int | None = None, sdk_ranges: list | None = None) -> bool:
    """Check if callee is an N64 OS / Libultra / SDK function."""
    if (
        callee.startswith("os")
        or callee.startswith("leo")
        or callee.startswith("al")
        or callee.startswith("__")
        or callee in ("bzero", "bcopy", "bcmp", "memcpy", "memset", "strlen", "strcpy", "strcmp")
    ):
        return True
    if sdk_ranges and callee_vram is not None:
        for start, end in sdk_ranges:
            if start <= callee_vram < end:
                return True
    return False


def parse_codebase_and_functions(game: str, subsegs: list, symbols: dict):
    """Parse all C sources and ASM files to extract function catalog and call graph."""
    def find_subseg_by_rom(rom_addr: int):
        for s in subsegs:
            if s["start"] <= rom_addr < s["end"]:
                return s
        return None

    def find_subseg_by_vram(vram: int):
        for s in subsegs:
            if s["vram"] <= vram < s["end_vram"]:
                return s
        return None

    # Identify SDK VRAM ranges
    sdk_ranges = [
        (s["vram"], s["end_vram"])
        for s in subsegs
        if s["module"] == "libultra" or "libultra" in s["name"].lower()
    ]

    c_funcs = {}
    decompiled_func_names = set()

    # 1. Parse C source files
    src_dirs = [REPO_ROOT / "src" / "c" / game, REPO_ROOT / "src" / game]
    c_dir = next((d for d in src_dirs if d.exists()), None)

    if c_dir:
        for c_file in sorted(c_dir.glob("*.c")):
            content = c_file.read_text(encoding="utf-8", errors="ignore")
            mod_stem = c_file.stem
            m_seg = re.match(r"^([a-zA-Z0-9_]+)_segment_", mod_stem)
            file_mod = m_seg.group(1) if m_seg else mod_stem

            # Parse aliases: func_800XXXXX(...) __attribute__((alias("MyFunction")));
            for m in re.finditer(
                r"([A-Za-z0-9_]+)\s*\([^)]*\)\s*__attribute__\s*\(\s*\(\s*alias\s*\(\s*\"([^\"]+)\"\s*\)\s*\)\s*\)",
                content,
            ):
                rom_sym = m.group(1)
                c_name = m.group(2)
                decompiled_func_names.add(rom_sym)
                decompiled_func_names.add(c_name)

                if rom_sym.startswith("func_"):
                    try:
                        vram = int(rom_sym[5:], 16)
                    except ValueError:
                        vram = 0
                else:
                    vram = symbols.get(rom_sym, 0)

                sub = find_subseg_by_vram(vram) if vram else None
                rom_offset = (vram - sub["vram_base"]) if (sub and vram) else 0

                c_funcs[rom_sym] = {
                    "name": rom_sym,
                    "c_name": c_name,
                    "module": sub["module"] if sub else file_mod,
                    "file": c_file.name,
                    "vram": vram,
                    "rom_start": rom_offset,
                    "rom_end": 0,
                    "size": 0,
                    "is_decompiled": True,
                    "calls": [],
                }

            # Parse direct C function definitions
            for m in re.finditer(
                r"^(?:void|s32|u32|s16|u16|s8|u8|int|f32|f64|void\*|u8\*|s32\*)\s+([A-Za-z0-9_]+)\s*\([^;]*?\)\s*\{",
                content,
                re.M,
            ):
                fn_name = m.group(1)
                if fn_name not in c_funcs and fn_name not in decompiled_func_names:
                    decompiled_func_names.add(fn_name)
                    if fn_name.startswith("func_"):
                        try:
                            vram = int(fn_name[5:], 16)
                        except ValueError:
                            vram = 0
                    else:
                        vram = symbols.get(fn_name, 0)

                    sub = find_subseg_by_vram(vram) if vram else None
                    rom_offset = (vram - sub["vram_base"]) if (sub and vram) else 0

                    c_funcs[fn_name] = {
                        "name": fn_name,
                        "c_name": fn_name,
                        "module": sub["module"] if sub else file_mod,
                        "file": c_file.name,
                        "vram": vram,
                        "rom_start": rom_offset,
                        "rom_end": 0,
                        "size": 0,
                        "is_decompiled": True,
                        "calls": [],
                    }

    # 2. Parse ASM files
    asm_dirs = [REPO_ROOT / "bazel-bin" / "asm" / game, REPO_ROOT / "asm" / game]
    asm_dir = next((d for d in asm_dirs if d.exists()), None)

    asm_funcs = {}
    if asm_dir:
        for s_path in sorted(asm_dir.glob("*.s")):
            if s_path.name.startswith("header") or s_path.name.startswith("1000") or s_path.parent.name == "data":
                continue

            content = s_path.read_text(encoding="utf-8", errors="ignore")
            cur_func = None
            start_rom = None
            vram = None
            last_rom = None
            calls = []

            for line in content.splitlines():
                line_s = line.strip()

                m_g = re.match(r"^glabel\s+([A-Za-z0-9_]+)", line_s)
                if m_g:
                    cur_func = m_g.group(1)
                    start_rom = None
                    vram = None
                    last_rom = None
                    calls = []
                    continue

                m_off = re.search(r"/\*\s*([0-9A-Fa-f]+)\s+([0-9A-Fa-f]+)", line_s)
                if m_off and cur_func:
                    rom_val = int(m_off.group(1), 16)
                    if start_rom is None:
                        start_rom = rom_val
                        vram = int(m_off.group(2), 16)
                    last_rom = rom_val

                    m_jal = re.search(r"\bjal\s+([A-Za-z0-9_]+)", line_s)
                    if m_jal:
                        calls.append(m_jal.group(1))

                m_end = re.match(r"^endlabel\s+([A-Za-z0-9_]+)", line_s)
                if m_end and cur_func:
                    end_rom = (last_rom + 4) if last_rom is not None else start_rom
                    size = (end_rom - start_rom) if (start_rom is not None and end_rom is not None) else 0
                    sub = find_subseg_by_rom(start_rom) if start_rom is not None else None
                    fallback_mod = sub["module"] if sub else s_path.stem
                    asm_funcs[cur_func] = {
                        "name": cur_func,
                        "c_name": None,
                        "module": fallback_mod,
                        "file": s_path.name,
                        "vram": vram,
                        "rom_start": start_rom,
                        "rom_end": end_rom,
                        "size": size,
                        "is_decompiled": False,
                        "calls": calls,
                    }
                    cur_func = None

    all_funcs = {}
    for fn, finfo in c_funcs.items():
        if finfo["rom_start"]:
            sub = find_subseg_by_rom(finfo["rom_start"])
            if sub:
                finfo["subseg"] = sub["name"]
                sub["funcs"].append(fn)
        all_funcs[fn] = finfo

    for fn, finfo in asm_funcs.items():
        sub = find_subseg_by_rom(finfo["rom_start"]) if finfo["rom_start"] is not None else None
        if sub:
            finfo["subseg"] = sub["name"]
            sub["funcs"].append(fn)
        else:
            finfo["subseg"] = finfo["file"]
        all_funcs[fn] = finfo

    # Resolve call dependencies and readiness
    for fn, finfo in all_funcs.items():
        unresolved = []
        for target in finfo["calls"]:
            if target in decompiled_func_names:
                continue
            callee_vram = None
            if target.startswith("func_"):
                try:
                    callee_vram = int(target[5:], 16)
                except ValueError:
                    pass
            elif target in symbols:
                callee_vram = symbols[target]

            if is_sdk_routine(target, callee_vram, sdk_ranges):
                continue
            unresolved.append(target)

        finfo["unresolved"] = sorted(set(unresolved))
        finfo["is_ready"] = (not finfo["is_decompiled"]) and (len(finfo["unresolved"]) == 0)

    return all_funcs


def group_by_logical_module(subsegs: list, all_funcs: dict, game: str):
    """Aggregate subsegments and functions into logical modules."""
    modules = {}

    for s in subsegs:
        m_name = s["module"]
        if m_name not in modules:
            modules[m_name] = {
                "name": m_name,
                "rom_start": s["start"],
                "rom_end": s["end"],
                "total_bytes": 0,
                "decompiled_bytes": 0,
                "total_funcs": 0,
                "decompiled_funcs": 0,
                "subsegments": [],
                "files": [],
                "outgoing": set(),
                "funcs": [],
            }
        mod = modules[m_name]
        mod["total_bytes"] += s["size"]
        if s["type"] == "c":
            mod["decompiled_bytes"] += s["size"]
        mod["rom_start"] = min(mod["rom_start"], s["start"])
        mod["rom_end"] = max(mod["rom_end"], s["end"])
        mod["subsegments"].append(s)

    # Map existing C files to modules
    src_dirs = [REPO_ROOT / "src" / "c" / game, REPO_ROOT / "src" / game]
    c_dir = next((d for d in src_dirs if d.exists()), None)
    if c_dir:
        for c_file in sorted(c_dir.glob("*.c")):
            stem = c_file.stem
            m_seg = re.match(r"^([a-zA-Z0-9_]+)_segment_", stem)
            target_mod = m_seg.group(1) if m_seg else stem
            if target_mod not in modules:
                for fn, finfo in all_funcs.items():
                    if finfo.get("file") == c_file.name:
                        target_mod = finfo.get("module")
                        break
            if target_mod and target_mod in modules:
                if c_file.name not in modules[target_mod]["files"]:
                    modules[target_mod]["files"].append(c_file.name)

    # Attach functions to modules
    for fn, finfo in all_funcs.items():
        m_name = finfo.get("module")
        if m_name in modules:
            modules[m_name]["funcs"].append(finfo)
            modules[m_name]["total_funcs"] += 1
            if finfo["is_decompiled"]:
                modules[m_name]["decompiled_funcs"] += 1

            for callee in finfo["calls"]:
                if callee.startswith("func_"):
                    try:
                        tvram = int(callee[5:], 16)
                        for s in subsegs:
                            if s["vram"] <= tvram < s["end_vram"]:
                                dst_mod = s["module"]
                                if dst_mod != m_name and dst_mod != "libultra":
                                    modules[m_name]["outgoing"].add(dst_mod)
                                break
                    except ValueError:
                        pass

    # Status assignment
    for m_name, mod in modules.items():
        mod["outgoing"].discard(m_name)
        mod["outgoing"].discard("libultra")
        pct = (mod["decompiled_bytes"] / mod["total_bytes"] * 100.0) if mod["total_bytes"] > 0 else 0.0
        mod["pct"] = pct

        if m_name == "libultra":
            mod["status"] = "[SDK]"
        elif mod["decompiled_bytes"] == mod["total_bytes"] and mod["total_bytes"] > 0:
            mod["status"] = "[DONE]"
        elif mod["decompiled_bytes"] > 0:
            mod["status"] = "[IN PROGRESS]"
        else:
            mod["status"] = None

    for m_name, mod in modules.items():
        if mod["status"] is None:
            blocking = [
                dep for dep in mod["outgoing"]
                if dep in modules and modules[dep]["status"] != "[DONE]" and dep != "libultra"
            ]
            mod["status"] = "[READY]" if len(blocking) == 0 else "[BLOCKED]"

    return modules


def render_progress_bar(percentage: float, width: int = 50) -> str:
    filled = int(width * (percentage / 100.0))
    bar = "=" * filled + "-" * (width - filled)
    return f"[{bar}] {percentage:5.2f}%"


def generate_mermaid_dag(modules: dict) -> str:
    """Generate a Mermaid directed graph of module dependencies."""
    lines = ["```mermaid", "graph TD"]
    for m_name, mod in sorted(modules.items(), key=lambda kv: kv[1]["rom_start"]):
        size_kb = mod["total_bytes"] / 1024
        label = f'"{m_name}\\n({size_kb:.1f} KB)"'

        status = mod["status"]
        if status == "[DONE]":
            lines.append(f"    {m_name}[{label}]:::done")
        elif status == "[IN PROGRESS]":
            lines.append(f"    {m_name}[{label}]:::inprogress")
        elif status == "[READY]":
            lines.append(f"    {m_name}[{label}]:::ready")
        elif status == "[SDK]":
            lines.append(f"    {m_name}[{label}]:::sdk")
        else:
            lines.append(f"    {m_name}[{label}]:::blocked")

    for m_name, mod in modules.items():
        for dep in sorted(mod["outgoing"]):
            if dep in modules:
                lines.append(f"    {m_name} --> {dep}")

    lines.append("    classDef done fill:#2da44e,stroke:#2da44e,color:#fff;")
    lines.append("    classDef inprogress fill:#dbab09,stroke:#dbab09,color:#fff;")
    lines.append("    classDef ready fill:#1f6feb,stroke:#1f6feb,color:#fff;")
    lines.append("    classDef blocked fill:#8b949e,stroke:#6e7681,color:#fff;")
    lines.append("    classDef sdk fill:#6e7681,stroke:#6e7681,color:#fff;")
    lines.append("```")
    return "\n".join(lines)


def format_blocking_target(callee: str, all_funcs: dict, subsegs: list) -> str:
    """Format a blocking callee as module:function."""
    if callee in all_funcs and all_funcs[callee].get("module"):
        return f"{all_funcs[callee]['module']}:{callee}"
    if callee.startswith("func_"):
        try:
            tvram = int(callee[5:], 16)
            for s in subsegs:
                if s["vram"] <= tvram < s["end_vram"]:
                    return f"{s['module']}:{callee}"
        except ValueError:
            pass
    return callee


def print_module_detail(mod_name: str, modules: dict, all_funcs: dict, subsegs: list, game: str):
    """Print full function-by-function breakdown for a specific module."""
    if mod_name not in modules:
        print(f"Error: Module '{mod_name}' not found.", file=sys.stderr)
        print(f"Available modules: {', '.join(sorted(modules.keys()))}", file=sys.stderr)
        return

    mod = modules[mod_name]

    print(f"\n=== Module Detail: {mod_name} ===")
    print(f"ROM Range:      0x{mod['rom_start']:05X} - 0x{mod['rom_end']:05X}")
    print(f"Code Size:      {mod['total_bytes']:,} bytes ({mod['pct']:5.1f}% decompiled)")
    print(f"Functions:      {mod['decompiled_funcs']} matching / {mod['total_funcs']} total")
    print(f"Existing Files: {', '.join(mod['files']) if mod['files'] else 'None'}")
    print()

    print("Subsegments in Splat:")
    for s in mod["subsegments"]:
        stype = "[MATCH]" if s["type"] == "c" else "[ASM]"
        print(f"  {stype:7s} {s['name']:32s} [0x{s['start']:05X} - 0x{s['end']:05X}] {s['size']:6,d} B")

    print("\nFunctions in Module:")
    hdr = f"{'FUNCTION':28s} {'ROM RANGE':21s} {'SIZE':>8s}  {'STATUS'}"
    print(hdr)
    print("-" * 80)

    funcs_in_mod = sorted(mod["funcs"], key=lambda f: f["rom_start"] if f["rom_start"] else 0)
    for f in funcs_in_mod:
        if f["is_decompiled"]:
            status = "DONE"
        elif f["is_ready"]:
            status = "READY"
        else:
            unres_items = [format_blocking_target(u, all_funcs, subsegs) for u in f["unresolved"][:3]]
            unres = ", ".join(unres_items)
            if len(f["unresolved"]) > 3:
                unres += f" (+{len(f['unresolved']) - 3} more)"
            status = f"BLOCKED ({unres})"

        r_range = f"0x{f['rom_start']:05X} - 0x{f['rom_end']:05X}" if f["rom_start"] else "-"
        size_str = f"{f['size']:,} B" if f["size"] else "-"
        disp_name = f["c_name"] if f["c_name"] else f["name"]

        print(f"{disp_name:28s} {r_range:21s} {size_str:>8s}  {status}")

    print("\nWorkflow Guidance:")
    print(f"  1. Add C implementation:     src/c/{game}/{mod['name']}.c")
    print(f"  2. Include module header:    #include \"{mod['name']}.h\"")
    print("  3. Check bit-exact diff:     bazel run //:diff -- <func_name>")
    print("  4. Generate C draft context: bazel run //:m2c -- <func_name>")
    print()


def main():
    parser = argparse.ArgumentParser(
        description="Calculate decompilation progress, detect module names, and track function-level roadmap."
    )
    parser.add_argument("--game", default="harvest-moon-64", help="Game identifier.")
    parser.add_argument("--module", "-m", help="Display detailed function breakdown for a specific module.")
    parser.add_argument("--ready", "-r", action="store_true", help="List all functions ready to decompile.")
    parser.add_argument("--mermaid", action="store_true", help="Output Mermaid dependency graph.")
    parser.add_argument("--quiet", action="store_true", help="Hide tables and show summary only.")
    args = parser.parse_args()

    game_info, subsegs, symbols = load_game_config(args.game)
    if not game_info:
        return 1

    all_funcs = parse_codebase_and_functions(args.game, subsegs, symbols)
    modules = group_by_logical_module(subsegs, all_funcs, args.game)

    if args.module:
        print_module_detail(args.module, modules, all_funcs, subsegs, args.game)
        return 0

    if args.mermaid:
        print(generate_mermaid_dag(modules))
        return 0

    total_bytes = game_info["total_text"]
    c_bytes = game_info["c_bytes"]
    pct = (c_bytes / total_bytes * 100.0) if total_bytes > 0 else 0.0

    total_funcs = len(all_funcs)
    decomp_funcs = sum(1 for f in all_funcs.values() if f["is_decompiled"])
    asm_funcs = total_funcs - decomp_funcs

    print(f"\n=== Decompilation Progress: {game_info['name']} ===")
    print(f"Total .text Executable Code:  {total_bytes:10,d} bytes  ({total_funcs:5,d} functions)")
    print(f"Decompiled to Matching C:     {c_bytes:10,d} bytes  ({decomp_funcs:5,d} functions)")
    print(f"Remaining in Assembly:        {game_info['asm_bytes']:10,d} bytes  ({asm_funcs:5,d} functions)")
    print()
    print("Overall .text Progress:")
    print(render_progress_bar(pct, 50))

    if args.ready:
        print("\n=== All Functions Ready to Decompile ===")
        ready_funcs = [f for f in all_funcs.values() if f.get("is_ready")]
        ready_funcs.sort(key=lambda f: (f["module"], f["size"], f["rom_start"]))

        hdr = f"{'FUNCTION':18s} {'MODULE':16s} {'ROM RANGE':21s} {'SIZE':>8s}  {'STATUS'}"
        print(hdr)
        print("-" * len(hdr))
        for f in ready_funcs:
            r_range = f"0x{f['rom_start']:05X} - 0x{f['rom_end']:05X}" if f["rom_start"] else "-"
            size_str = f"{f['size']:,} B" if f["size"] else "-"
            print(f"{f['name']:18s} {f['module']:16s} {r_range:21s} {size_str:>8s}  READY")
        print(f"\nTotal ready functions: {len(ready_funcs)}")
        return 0

    if not args.quiet:
        print("\n=== Module Roadmap ===")
        hdr = f"{'MODULE':15s} {'BYTES':27s} {'FUNCS':20s} {'FILES'}"
        print(hdr)
        print("-" * 80)

        for m_name, mod in sorted(modules.items(), key=lambda kv: kv[1]["rom_start"]):
            c_b = mod["decompiled_bytes"]
            t_b = mod["total_bytes"]
            b_pct = (c_b / t_b * 100.0) if t_b else 0.0

            c_f = mod["decompiled_funcs"]
            t_f = mod["total_funcs"]
            f_pct = (c_f / t_f * 100.0) if t_f else 0.0

            b_str = f"{c_b:>7,d} / {t_b:>7,d} ({b_pct:5.1f}%)"
            f_str = f"{c_f:>4d} / {t_f:>4d} ({f_pct:5.1f}%)"
            files_str = ", ".join(mod["files"]) if mod["files"] else "-"

            print(f"{m_name:15s} {b_str:27s} {f_str:20s} {files_str}")

        # Top Recommended Functions to Decompile Next
        print("\n=== Next Functions to Decompile ===")
        funcs_by_mod = {}
        for f in all_funcs.values():
            if not f.get("is_decompiled"):
                funcs_by_mod.setdefault(f.get("module"), []).append(f)

        for m_name in funcs_by_mod:
            # Sort: READY first (by size ascending), then BLOCKED
            funcs_by_mod[m_name].sort(key=lambda f: (
                0 if f.get("is_ready") else 1,
                f["size"] if f["size"] else 999999,
            ))

        # Dynamically prioritize modules: in-progress first, then ready, then others
        in_progress = [
            m for m in sorted(modules.values(), key=lambda x: x["rom_start"])
            if 0 < m["decompiled_bytes"] < m["total_bytes"] and m["name"] in funcs_by_mod
        ]
        ready_mods = [
            m for m in sorted(modules.values(), key=lambda x: x["rom_start"])
            if m["decompiled_bytes"] == 0
            and m["name"] in funcs_by_mod
            and any(f.get("is_ready") for f in funcs_by_mod[m["name"]])
        ]
        other_mods = [
            m for m in sorted(modules.values(), key=lambda x: x["rom_start"])
            if m not in in_progress and m not in ready_mods and m["name"] in funcs_by_mod
        ]
        priority_mods = (in_progress + ready_mods + other_mods)[:5]

        for mod in priority_mods:
            p_mod = mod["name"]
            print(f"\n  Module: {p_mod}")
            f_hdr = f"    {'FUNCTION':18s} {'ROM RANGE':21s} {'SIZE':>8s}  {'STATUS'}"
            print(f_hdr)
            print("    " + "-" * (len(f_hdr) - 4))
            for f in funcs_by_mod[p_mod][:5]:
                r_range = f"0x{f['rom_start']:05X} - 0x{f['rom_end']:05X}" if f["rom_start"] else "-"
                size_str = f"{f['size']:,} B" if f["size"] else "-"
                if f.get("is_ready"):
                    status = "READY"
                else:
                    unres_items = [
                        format_blocking_target(u, all_funcs, subsegs)
                        for u in f["unresolved"][:2]
                    ]
                    unres = ", ".join(unres_items)
                    if len(f["unresolved"]) > 2:
                        unres += f" (+{len(f['unresolved']) - 2} more)"
                    status = f"BLOCKED ({unres})"
                print(f"    {f['name']:18s} {r_range:21s} {size_str:>8s}  {status}")

        print("\nWorkflow Tips:")
        print("  - To decompile next, add matching C functions into src/c/<game>/<module>.c.")
        print("  - Check bit-exact diff:      bazel run //:diff -- <func_name>")
        print("  - Generate initial C draft:  bazel run //:m2c -- <func_name>")
        print("  - View module details:       bazel run //:progress -- -m <module>")
        print()

    return 0


if __name__ == "__main__":
    sys.exit(main())
