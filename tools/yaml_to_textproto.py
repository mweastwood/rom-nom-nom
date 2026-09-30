#!/usr/bin/env python3
"""
Converts legacy Splat YAML split configurations into SplitConfig .textproto format.
"""

import argparse
import sys
from pathlib import Path
import yaml

TYPE_MAP = {
    "header": "SEGMENT_HEADER",
    "bin": "SEGMENT_BIN",
    "code": "SEGMENT_CODE",
    "data": "SEGMENT_DATA",
    "rodata": "SEGMENT_RODATA",
    "bss": "SEGMENT_BSS",
}

SUB_TYPE_MAP = {
    "asm": "SUBSEGMENT_ASM",
    "c": "SUBSEGMENT_C",
    "hasm": "SUBSEGMENT_HASM",
    "data": "SUBSEGMENT_DATA",
    "rodata": "SUBSEGMENT_RODATA",
    "bin": "SUBSEGMENT_BIN",
    "bss": "SUBSEGMENT_BSS",
}


def convert_splat_yaml(yaml_path: Path) -> str:
    with open(yaml_path, "r", encoding="utf-8") as f:
        data = yaml.safe_load(f)

    lines = []
    lines.append(f'game_name: "{data.get("name", "")}"')
    lines.append(f'sha1: "{data.get("sha1", "")}"')
    opts = data.get("options", {})
    basename = opts.get("basename", yaml_path.stem)
    lines.append(f'basename: "{basename}"')

    for sym_path in opts.get("symbol_addrs_path", []):
        sym_p = Path(sym_path)
        if sym_p.suffix == ".txt":
            sym_p = sym_p.with_suffix(".textproto")
        lines.append(f'symbol_files: "{sym_p}"')

    # Compiler flags
    c_flags = data.get("c_flags", {})
    for mod, flags in sorted(c_flags.items()):
        lines.append(f'c_flags {{\n  key: "{mod}"\n  value {{\n' +
                     "\n".join(f'    flags: "{flag}"' for flag in flags) +
                     "\n  }\n}")

    segments_raw = data.get("segments", [])
    
    # Pre-pass: collect segment boundaries
    seg_starts = []
    for seg in segments_raw:
        if isinstance(seg, list):
            seg_starts.append(seg[0])
        elif isinstance(seg, dict):
            seg_starts.append(seg.get("start", 0))

    for idx, seg in enumerate(segments_raw):
        if isinstance(seg, list):
            if len(seg) == 1:
                # End of ROM marker e.g. [0x1000000]
                continue
            seg_start = seg[0]
            seg_type = TYPE_MAP.get(seg[1], "SEGMENT_BIN")
            seg_name = seg[2] if len(seg) > 2 else f"{seg_start:X}"
            next_start = seg_starts[idx + 1] if idx + 1 < len(seg_starts) else 0

            lines.append("segments {")
            lines.append(f'  name: "{seg_name}"')
            lines.append(f"  type: {seg_type}")
            lines.append(f"  rom_start: 0x{seg_start:X}")
            if next_start:
                lines.append(f"  rom_end: 0x{next_start:X}")
            lines.append("}")

        elif isinstance(seg, dict):
            s_name = seg.get("name", "")
            s_type_str = seg.get("type", "bin")
            s_type = TYPE_MAP.get(s_type_str, "SEGMENT_BIN")
            s_start = seg.get("start", 0)
            s_end = seg.get("end", 0)
            if not s_end and idx + 1 < len(seg_starts):
                s_end = seg_starts[idx + 1]

            s_vram = seg.get("vram", 0)
            s_bss = seg.get("bss_size", 0)
            if not s_name:
                s_name = f"{s_start:X}"

            lines.append("segments {")
            lines.append(f'  name: "{s_name}"')
            lines.append(f"  type: {s_type}")
            lines.append(f"  rom_start: 0x{s_start:X}")
            if s_end:
                lines.append(f"  rom_end: 0x{s_end:X}")
            if s_vram:
                lines.append(f"  vram: 0x{s_vram:X}")
            if s_bss:
                lines.append(f"  bss_size: 0x{s_bss:X}")

            # Subsegments
            subsegments_raw = seg.get("subsegments", [])
            for s_idx, sub in enumerate(subsegments_raw):
                if isinstance(sub, list):
                    sub_start = sub[0]
                    sub_t_str = sub[1] if len(sub) > 1 else "asm"
                    sub_t = SUB_TYPE_MAP.get(sub_t_str, "SUBSEGMENT_ASM")
                    sub_n = sub[2] if len(sub) > 2 else f"{sub_start:X}"
                    sub_vram = sub[3] if len(sub) > 3 and isinstance(sub[3], int) else 0

                    sub_line = f'  subsegments {{ rom_start: 0x{sub_start:X} type: {sub_t} name: "{sub_n}"'
                    if sub_vram:
                        sub_line += f" vram: 0x{sub_vram:X}"
                    sub_line += " }"
                    lines.append(sub_line)

                elif isinstance(sub, dict):
                    sub_t_str = sub.get("type", "bss")
                    sub_t = SUB_TYPE_MAP.get(sub_t_str, "SUBSEGMENT_BSS")
                    sub_start = sub.get("start", 0)
                    sub_n = sub.get("name", "")
                    if not sub_n:
                        # Follow splat convention: name bss subsegment after segment end
                        sub_n = f"{s_end:X}" if s_end else "bss"
                    sub_vram = sub.get("vram", 0)

                    sub_line = f'  subsegments {{ type: {sub_t} name: "{sub_n}"'
                    if sub_start:
                        sub_line += f" rom_start: 0x{sub_start:X}"
                    if sub_vram:
                        sub_line += f" vram: 0x{sub_vram:X}"
                    sub_line += " }"
                    lines.append(sub_line)

            lines.append("}")

    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description="Convert Splat YAML to SplitConfig .textproto")
    parser.add_argument("yaml", type=Path, help="Input Splat YAML file")
    parser.add_argument("-o", "--output", type=Path, help="Output .textproto path")
    args = parser.parse_args()

    content = convert_splat_yaml(args.yaml)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(content, encoding="utf-8")
        print(f"Wrote {args.output}")
    else:
        sys.stdout.write(content)


if __name__ == "__main__":
    main()
