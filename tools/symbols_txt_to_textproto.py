#!/usr/bin/env python3
"""
Converts legacy symbols.txt into pure SymbolRegistry .textproto format.
"""

import argparse
import re
import sys
from pathlib import Path


def convert_symbols_txt(txt_path: Path) -> str:
    lines = []
    seen_addresses = set()
    seen_names = set()
    entries = []

    with open(txt_path, "r", encoding="utf-8") as f:
        for line in f:
            clean = line.strip()
            if not clean or clean.startswith("//"):
                continue

            comment = ""
            if "//" in clean:
                code_part, comment = clean.split("//", 1)
                clean = code_part.strip()
                comment = comment.strip()

            m = re.match(r"^([a-zA-Z0-9_\.]+)\s*=\s*(0x[0-9a-fA-F]+|[0-9]+)\s*;", clean)
            if not m:
                continue

            name = m.group(1)
            raw_addr = m.group(2)
            addr = int(raw_addr, 16) if raw_addr.lower().startswith("0x") else int(raw_addr)

            if addr in seen_addresses or name in seen_names:
                continue
            seen_addresses.add(addr)
            seen_names.add(name)

            # Determine type
            sym_type = "SYMBOL_FUNC"
            if "type:data" in comment:
                sym_type = "SYMBOL_DATA"
            elif "type:rodata" in comment:
                sym_type = "SYMBOL_RODATA"
            elif "type:bss" in comment:
                sym_type = "SYMBOL_BSS"
            elif "type:label" in comment or name.startswith(".L"):
                sym_type = "SYMBOL_LABEL"
            elif "type:func" in comment:
                sym_type = "SYMBOL_FUNC"
            elif name.startswith("D_") or name.startswith("g_"):
                sym_type = "SYMBOL_DATA"

            # Parse size
            size = 0
            size_m = re.search(r"size:(0x[0-9a-fA-F]+|[0-9]+)", comment)
            if size_m:
                raw_size = size_m.group(1)
                size = int(raw_size, 16) if raw_size.lower().startswith("0x") else int(raw_size)

            # Parse absolute
            is_absolute = False
            if re.search(r"absolute:(true|1)", comment, re.IGNORECASE):
                is_absolute = True

            entries.append((addr, name, sym_type, size, is_absolute))

    # Sort entries by address ascending
    entries.sort(key=lambda e: e[0])

    out = []
    for addr, name, sym_type, size, is_absolute in entries:
        out.append("entries {")
        out.append(f'  name: "{name}"')
        out.append(f"  address: 0x{addr:08X}")
        out.append(f"  type: {sym_type}")
        if size > 0:
            out.append(f"  size: 0x{size:X}")
        if is_absolute:
            out.append("  is_absolute: true")
        out.append("}")

    return "\n".join(out) + "\n"


def main():
    parser = argparse.ArgumentParser(description="Convert symbols.txt to SymbolRegistry .textproto")
    parser.add_argument("symbols_txt", type=Path, help="Input symbols.txt file")
    parser.add_argument("-o", "--output", type=Path, help="Output .textproto path")
    args = parser.parse_args()

    content = convert_symbols_txt(args.symbols_txt)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(content, encoding="utf-8")
        print(f"Wrote {args.output}")
    else:
        sys.stdout.write(content)


if __name__ == "__main__":
    main()
