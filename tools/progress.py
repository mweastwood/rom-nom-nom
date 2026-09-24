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


def load_symbols_file(game: str) -> dict[str, int]:
    """Load manually defined symbols and their VRAM addresses from symbols/<game>.txt."""
    sym_path = REPO_ROOT / "symbols" / f"{game}.txt"
    if not sym_path.exists():
        return {}
    defs = {}
    for line in sym_path.read_text(encoding="utf-8").splitlines():
        line = re.sub(r"//.*", "", line).strip()
        m = re.match(r"^([a-zA-Z0-9_]+)\s*=\s*(0x[0-9a-fA-F]+)\s*;", line)
        if m:
            defs[m.group(1)] = int(m.group(2), 16)
    return defs


def parse_data_symbols(game: str, defined_symbols: dict[str, int] | None = None) -> dict[str, dict]:
    """Parse all data, rodata, and bss symbols from disassembled assembly files."""
    asm_dirs = [
        REPO_ROOT / "bazel-bin" / "asm" / game / "data",
        REPO_ROOT / "asm" / game / "data",
    ]
    d_dir = next((d for d in asm_dirs if d.exists()), None)
    if not d_dir:
        return {}

    symbols = {}
    for p in sorted(d_dir.glob("*.s")):
        section = "bss" if "bss" in p.name else "data"
        content = p.read_text(encoding="utf-8", errors="ignore")
        for line in content.splitlines():
            line = line.strip()
            m_dl = re.match(r"^dlabel\s+([A-Za-z0-9_]+)", line)
            if m_dl:
                sname = m_dl.group(1)
                is_named = (
                    (defined_symbols and sname in defined_symbols)
                    or (not sname.startswith("D_") and not sname.startswith("jtbl_"))
                )
                vram = None
                if defined_symbols and sname in defined_symbols:
                    vram = defined_symbols[sname]
                elif sname.startswith("D_"):
                    try:
                        vram = int(sname[2:], 16)
                    except ValueError:
                        pass
                symbols[sname] = {
                    "name": sname,
                    "section": section,
                    "file": p.name,
                    "is_named": is_named,
                    "vram": vram,
                }
    return symbols


def parse_c_declarations(
    game: str, modules: dict | None = None, defined_symbols: dict[str, int] | None = None
) -> tuple[list[dict], dict[str, dict]]:
    """Parse struct definitions and global variable extern declarations from headers and C sources."""
    src_dirs = [REPO_ROOT / "src" / "c" / game, REPO_ROOT / "src" / game]
    s_dir = next((d for d in src_dirs if d.exists()), None)
    if not s_dir:
        return [], {}

    structs = []
    globals_map = {}

    def get_file_module(file_name: str) -> str:
        stem = Path(file_name).stem
        if stem == "game_time":
            return "time"
        if modules and stem in modules:
            return stem
        m = re.match(r"^([A-Za-z0-9_]+)_segment_", stem)
        if m and modules and m.group(1) in modules:
            return m.group(1)
        return stem

    for p in sorted(s_dir.glob("*.[ch]")):
        content = p.read_text(encoding="utf-8", errors="ignore")
        content = re.sub(r"/\*.*?\*/", "", content, flags=re.S)
        content = re.sub(r"//.*$", "", content, flags=re.M)
        is_header = p.suffix == ".h"
        mod_name = get_file_module(p.name)

        # 1. Parse Struct Definitions
        struct_pattern = re.compile(
            r"(?:typedef\s+)?struct\s*([A-Za-z0-9_]*)\s*\{([^}]+)\}\s*([A-Za-z0-9_]*);",
            re.S,
        )
        for m in struct_pattern.finditer(content):
            tag = m.group(1).strip()
            body = m.group(2).strip()
            alias = m.group(3).strip()
            sname = alias if alias else tag
            if not sname:
                continue

            raw_fields = [f.strip() for f in body.split(";") if f.strip()]
            fields = []
            for rf in raw_fields:
                is_unk = bool(re.search(r"\b(unk|pad)[0-9A-Za-z_]*\b", rf, re.I))
                fields.append({"raw": rf, "is_unknown": is_unk})

            named_cnt = sum(1 for f in fields if not f["is_unknown"])
            structs.append({
                "name": sname,
                "tag": tag,
                "alias": alias,
                "file": p.name,
                "module": mod_name,
                "is_header": is_header,
                "total_fields": len(fields),
                "named_fields": named_cnt,
                "unknown_fields": len(fields) - named_cnt,
                "fields": fields,
            })

        # 2. Parse Extern Globals
        # Function pointers: extern void (*g_idle_callback)(void);
        fn_ptr_pat = re.compile(
            r"extern\s+([A-Za-z0-9_*\s]+?)\s*\(\s*\*\s*([A-Za-z0-9_]+)\s*\)\s*\([^;]*?\)\s*;",
            re.M,
        )
        for m in fn_ptr_pat.finditer(content):
            ret_type = m.group(1).strip()
            sym_name = m.group(2).strip()
            vram = None
            if defined_symbols and sym_name in defined_symbols:
                vram = defined_symbols[sym_name]
            elif sym_name.startswith("D_"):
                try:
                    vram = int(sym_name[2:], 16)
                except ValueError:
                    pass
            elif sym_name == "osTvType":
                vram = 0x80000300
            g_item = {
                "name": sym_name,
                "type": f"{ret_type} (*)(...)",
                "file": p.name,
                "module": mod_name,
                "is_header": is_header,
                "alias": None,
                "vram": vram,
                "in_symbols_file": bool(defined_symbols and sym_name in defined_symbols),
            }
            if sym_name not in globals_map or is_header:
                globals_map[sym_name] = g_item

        # Variable declarations: extern [volatile] Type [*] name [array];
        var_pat = re.compile(
            r"extern\s+(?:const\s+|volatile\s+)?([A-Za-z0-9_*]+(?:\s*\*+)?)\s+([A-Za-z0-9_]+)(\s*\[[^;\]]*\])*\s*;",
            re.M,
        )
        for m in var_pat.finditer(content):
            raw_type = m.group(1).strip()
            sym_name = m.group(2).strip()
            arr = m.group(3) or ""
            if "(" in raw_type or "(" in sym_name:
                continue
            vram = None
            if defined_symbols and sym_name in defined_symbols:
                vram = defined_symbols[sym_name]
            elif sym_name.startswith("D_"):
                try:
                    vram = int(sym_name[2:], 16)
                except ValueError:
                    pass
            elif sym_name == "osTvType":
                vram = 0x80000300
            g_item = {
                "name": sym_name,
                "type": f"{raw_type}{arr.strip()}",
                "file": p.name,
                "module": mod_name,
                "is_header": is_header,
                "alias": None,
                "vram": vram,
                "in_symbols_file": bool(defined_symbols and sym_name in defined_symbols),
            }
            if sym_name not in globals_map or is_header:
                globals_map[sym_name] = g_item

    # Second pass: Associate #define aliases with globals
    alias_pat = re.compile(
        r"#define\s+([A-Za-z0-9_]+)\s+([A-Za-z0-9_]+)",
        re.M,
    )
    for p in sorted(s_dir.glob("*.[ch]")):
        content = p.read_text(encoding="utf-8", errors="ignore")
        for m in alias_pat.finditer(content):
            alias_name = m.group(1).strip()
            target_sym = m.group(2).strip()
            if target_sym in globals_map and not globals_map[target_sym].get("alias"):
                globals_map[target_sym]["alias"] = alias_name

    return structs, globals_map


def get_module_data_refs(
    game: str, mod_name: str, defined_symbols: dict[str, int] | None = None
) -> set[str]:
    """Find all global data/bss symbols referenced in a module's assembly and C files."""
    paths = []
    p1 = REPO_ROOT / "bazel-bin" / "asm" / game / f"{mod_name}.s"
    if p1.exists():
        paths.append(p1)
    p2 = REPO_ROOT / "bazel-bin" / "asm" / game / "nonmatchings" / mod_name
    if p2.exists():
        paths.extend(p2.glob("*.s"))
    p3 = REPO_ROOT / "src" / "c" / game / f"{mod_name}.c"
    if p3.exists():
        paths.append(p3)

    refs = set()
    for p in paths:
        content = p.read_text(encoding="utf-8", errors="ignore")
        for m in re.finditer(r"\b(D_[0-9A-Fa-f]{8})\b", content):
            refs.add(m.group(1))
        if defined_symbols:
            for sname in defined_symbols:
                if re.search(r"\b" + re.escape(sname) + r"\b", content):
                    refs.add(sname)
    return refs


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
        for s_path in sorted(asm_dir.rglob("*.s")):
            if (
                s_path.name.startswith("header")
                or s_path.name.startswith("1000")
                or "data" in s_path.parts
                or s_path.name.endswith(".data.s")
                or s_path.name.endswith(".bss.s")
            ):
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
                    if cur_func not in decompiled_func_names:
                        end_rom = (last_rom + 4) if last_rom is not None else start_rom
                        size = (end_rom - start_rom) if (start_rom is not None and end_rom is not None) else 0
                        sub = find_subseg_by_rom(start_rom) if start_rom is not None else None
                        fallback_mod = sub["module"] if sub else (s_path.parent.name if s_path.parent.name != "asm" else s_path.stem)
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
            else:
                sub_name = finfo.get("subseg")
                for s in modules[m_name]["subsegments"]:
                    if s["name"] == sub_name and s["type"] == "c":
                        modules[m_name]["decompiled_bytes"] -= finfo["size"]
                        break

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


def print_data_and_structs_summary(
    data_symbols: dict,
    structs: list,
    globals_map: dict,
    defined_symbols: dict[str, int] | None = None,
    game: str = "harvest-moon-64",
):
    """Print high-level statistics for global variables and data structures."""
    if not data_symbols and not structs and not globals_map:
        return

    hdr_globals = sum(1 for g in globals_map.values() if g["is_header"])
    src_globals = sum(1 for g in globals_map.values() if not g["is_header"])
    total_data_syms = len(data_symbols)
    total_data_cnt = sum(1 for s in data_symbols.values() if s["section"] == "data")
    total_bss_cnt = sum(1 for s in data_symbols.values() if s["section"] == "bss")
    named_syms_cnt = sum(1 for s in data_symbols.values() if s.get("is_named"))
    unnamed_cnt = max(0, total_data_syms - named_syms_cnt)

    hdr_structs = sum(1 for s in structs if s["is_header"])
    src_structs = sum(1 for s in structs if not s["is_header"])
    total_fields = sum(s["total_fields"] for s in structs)
    named_fields = sum(s["named_fields"] for s in structs)
    struct_fidelity = (named_fields / total_fields * 100.0) if total_fields else 0.0

    print("\n=== Global Variables (.data & .bss) ===")
    if total_data_syms > 0:
        named_pct = (named_syms_cnt / total_data_syms * 100.0)
        hdr_pct = (hdr_globals / total_data_syms * 100.0)
        src_pct = (src_globals / total_data_syms * 100.0)
        und_pct = (unnamed_cnt / total_data_syms * 100.0)
        print(f"Total Global Symbols:       {total_data_syms:6,d} symbols (.data: {total_data_cnt:,}, .bss: {total_bss_cnt:,})")
        print(f"Named in symbols/{game}.txt:{named_syms_cnt:6,d} symbols ({named_pct:5.1f}%)")
        print(f"Declared in C Headers:      {hdr_globals:6,d} symbols ({hdr_pct:5.1f}%)")
        migration_note = "  [Candidates for header migration]" if src_globals > 0 else ""
        print(f"Declared in C Sources:      {src_globals:6,d} symbols ({src_pct:5.1f}%){migration_note}")
        print(f"Auto-Generated (D_XXXXXXXX):{unnamed_cnt:6,d} symbols ({und_pct:5.1f}%)")
    else:
        print(f"Declared in C Headers:      {hdr_globals:6,d} symbols")
        print(f"Declared in C Sources:      {src_globals:6,d} symbols")

    print("\n=== Data Structures (Structs) ===")
    src_struct_note = f" ({hdr_structs} in headers, {src_structs} in C sources)" if src_structs > 0 else f" ({hdr_structs} in headers)"
    print(f"Total Structs Defined:      {len(structs):6,d} structs{src_struct_note}")
    if total_fields > 0:
        print(f"Field Reverse-Engineering:  {named_fields:6,d} / {total_fields:,} named fields ({struct_fidelity:5.1f}% identified)")


def print_structs_catalog(structs: list[dict]):
    """Print full catalog of defined structs across the codebase."""
    print("\n=== Data Structures Catalog ===")
    if not structs:
        print("No structs defined.")
        return

    hdr = f"{'STRUCT':24s} {'FILE':16s} {'MODULE':12s} {'FIELDS (NAMED/TOTAL)':22s} {'STATUS'}"
    print(hdr)
    print("-" * len(hdr))

    for s in sorted(structs, key=lambda x: (0 if x["is_header"] else 1, x["name"])):
        status = "In header" if s["is_header"] else f"MOVE TO {s['module']}.h"
        f_str = f"{s['named_fields']:2d} / {s['total_fields']:2d}"
        print(f"{s['name']:24s} {s['file']:16s} {s['module']:12s} {f_str:^22s} {status}")
    print(f"\nTotal structs: {len(structs)}")


def print_globals_catalog(
    globals_map: dict[str, dict],
    data_symbols: dict[str, dict],
    defined_symbols: dict[str, int] | None = None,
    game: str = "harvest-moon-64",
):
    """Print full catalog of declared global variables."""
    print("\n=== Declared Global Variables ===")
    if not globals_map:
        print("No globals declared.")
        return

    hdr = f"{'SYMBOL':28s} {'VRAM':12s} {'TYPE':22s} {'FILE':14s} {'MODULE':10s} {'STATUS'}"
    print(hdr)
    print("-" * len(hdr))

    for g in sorted(globals_map.values(), key=lambda x: (0 if x["is_header"] else 1, x["name"])):
        vram_str = f"0x{g['vram']:08X}" if g.get("vram") else "-"
        if defined_symbols and g["name"] in defined_symbols:
            status = f"symbols/{game}.txt"
        elif g["is_header"]:
            status = "header"
        else:
            status = f"MOVE TO {g['module']}.h"
        print(f"{g['name']:28s} {vram_str:12s} {g['type']:22s} {g['file']:14s} {g['module']:10s} {status}")

    hdr_cnt = sum(1 for g in globals_map.values() if g["is_header"])
    src_cnt = sum(1 for g in globals_map.values() if not g["is_header"])
    named_in_file = sum(1 for g in globals_map.values() if defined_symbols and g["name"] in defined_symbols)
    print(
        f"\nTotal declared globals: {len(globals_map)} ({named_in_file} defined in symbols/{game}.txt, {hdr_cnt} in headers, {src_cnt} in C sources)"
    )
    if data_symbols:
        unnamed = sum(1 for s in data_symbols.values() if not s.get("is_named"))
        print(f"Auto-generated symbols in ROM assembly: {unnamed:,} (D_XXXXXXXX)")


def print_module_detail(
    mod_name: str,
    modules: dict,
    all_funcs: dict,
    subsegs: list,
    game: str,
    structs: list | None = None,
    globals_map: dict | None = None,
    data_symbols: dict | None = None,
    defined_symbols: dict[str, int] | None = None,
):
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

    # Structs section for this module
    if structs is not None:
        mod_structs = [s for s in structs if s["module"] == mod_name]
        print("\nData Structures (Structs):")
        if mod_structs:
            hdr_s = f"  {'STRUCT':24s} {'FILE':16s} {'FIELDS (NAMED/TOTAL)':22s} {'STATUS'}"
            print(hdr_s)
            print("  " + "-" * (len(hdr_s) - 2))
            for s in mod_structs:
                status = "In header" if s["is_header"] else f"MOVE TO {mod_name}.h"
                f_str = f"{s['named_fields']:2d} / {s['total_fields']:2d}"
                print(f"  {s['name']:24s} {s['file']:16s} {f_str:^22s} {status}")
        else:
            print("  None defined.")

    # Globals section for this module
    if globals_map is not None:
        mod_refs = get_module_data_refs(game, mod_name, defined_symbols)
        mod_hdr_globals = [g for g in globals_map.values() if g["module"] == mod_name and g["is_header"]]
        mod_src_globals = [g for g in globals_map.values() if g["module"] == mod_name and not g["is_header"]]
        undeclared_refs = sorted(
            [r for r in mod_refs if r not in globals_map and (not defined_symbols or r not in defined_symbols)]
        )

        print("\nGlobal Variables:")
        print(f"  Declared in Header ({len(mod_hdr_globals)}):")
        if mod_hdr_globals:
            for g in sorted(mod_hdr_globals, key=lambda x: x["name"])[:10]:
                vram_str = f" [0x{g['vram']:08X}]" if g.get("vram") else ""
                disp_sym = g["name"] + vram_str
                print(f"    {disp_sym:36s} {g['type']}")
            if len(mod_hdr_globals) > 10:
                print(f"    ... and {len(mod_hdr_globals) - 10} more")
        else:
            print("    None declared in header.")

        if mod_src_globals:
            print(f"\n  Migration Candidates in Source ({len(mod_src_globals)} in {mod_name}.c -> move to {mod_name}.h):")
            for g in sorted(mod_src_globals, key=lambda x: x["name"])[:10]:
                vram_str = f" [0x{g['vram']:08X}]" if g.get("vram") else ""
                disp_sym = g["name"] + vram_str
                print(f"    {disp_sym:36s} {g['type']}")
            if len(mod_src_globals) > 10:
                print(f"    ... and {len(mod_src_globals) - 10} more")

        if undeclared_refs:
            print(f"\n  Referenced Undeclared Symbols in Assembly ({len(undeclared_refs)}):")
            disp_refs = ", ".join(undeclared_refs[:8])
            if len(undeclared_refs) > 8:
                disp_refs += f", ... (+{len(undeclared_refs) - 8} more)"
            print(f"    {disp_refs}")

    print("\nWorkflow Guidance:")
    print(f"  1. Define structs & globals: src/c/{game}/{mod['name']}.h")
    print(f"  2. Add C implementation:     src/c/{game}/{mod['name']}.c")
    print(f"  3. Include module header:    #include \"{mod['name']}.h\"")
    print("  4. Check bit-exact diff:     bazel run //:diff -- <func_name>")
    print("  5. Generate C draft context: bazel run //:m2c -- <func_name>")
    print()


def main():
    parser = argparse.ArgumentParser(
        description="Calculate decompilation progress, detect module names, and track function-level roadmap."
    )
    parser.add_argument("--game", default="harvest-moon-64", help="Game identifier.")
    parser.add_argument("--module", "-m", help="Display detailed function breakdown for a specific module.")
    parser.add_argument("--ready", "-r", action="store_true", help="List all functions ready to decompile.")
    parser.add_argument(
        "--structs", "-s", action="store_true", help="List all defined structs and their header migration status."
    )
    parser.add_argument(
        "--globals", "-g", action="store_true", help="List all declared and undeclared global variables."
    )
    parser.add_argument("--mermaid", action="store_true", help="Output Mermaid dependency graph.")
    parser.add_argument("--quiet", action="store_true", help="Hide tables and show summary only.")
    args = parser.parse_args()

    game_info, subsegs, symbols = load_game_config(args.game)
    if not game_info:
        return 1

    defined_symbols = load_symbols_file(args.game)
    all_funcs = parse_codebase_and_functions(args.game, subsegs, symbols)
    modules = group_by_logical_module(subsegs, all_funcs, args.game)
    data_symbols = parse_data_symbols(args.game, defined_symbols)
    structs, globals_map = parse_c_declarations(args.game, modules, defined_symbols)

    if args.structs:
        print_structs_catalog(structs)
        return 0

    if args.globals:
        print_globals_catalog(globals_map, data_symbols, defined_symbols, args.game)
        return 0

    if args.module:
        print_module_detail(
            args.module, modules, all_funcs, subsegs, args.game, structs, globals_map, data_symbols, defined_symbols
        )
        return 0

    if args.mermaid:
        print(generate_mermaid_dag(modules))
        return 0

    c_bytes = sum(m["decompiled_bytes"] for m in modules.values())
    total_bytes = game_info["total_text"]
    asm_bytes = total_bytes - c_bytes
    game_info["c_bytes"] = c_bytes
    game_info["asm_bytes"] = asm_bytes
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

    if not args.quiet:
        print_data_and_structs_summary(data_symbols, structs, globals_map, defined_symbols, args.game)

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
