#!/usr/bin/env python3
"""Calculate and display decompilation progress for N64 games."""

import argparse
import os
from pathlib import Path
import sys
import yaml

if "BUILD_WORKSPACE_DIRECTORY" in os.environ:
    REPO_ROOT = Path(os.environ["BUILD_WORKSPACE_DIRECTORY"])
else:
    REPO_ROOT = Path(__file__).resolve().parent.parent


def get_game_progress(game: str):
    config_file = REPO_ROOT / "splat" / f"{game}.yaml"
    if not config_file.exists():
        print(f"Error: Splat config not found: {config_file}", file=sys.stderr)
        return None

    data = yaml.safe_load(config_file.read_text())
    main_seg = None
    for seg in data.get("segments", []):
        if isinstance(seg, dict) and seg.get("name") == "main":
            main_seg = seg
            break

    if not main_seg or "subsegments" not in main_seg:
        print(f"Error: 'main' segment not found in {config_file}", file=sys.stderr)
        return None

    subsegs = main_seg["subsegments"]
    text_end = None
    for s in subsegs:
        if isinstance(s, list) and len(s) >= 2:
            if s[1] in ("data", "rodata", "bss"):
                text_end = s[0]
                break

    if text_end is None:
        text_end = subsegs[-1][0] if isinstance(subsegs[-1], list) else 0

    c_files = []
    c_bytes = 0
    asm_bytes = 0

    for i in range(len(subsegs)):
        s = subsegs[i]
        if not (isinstance(s, list) and len(s) >= 2):
            continue
        start = s[0]
        stype = s[1]
        if start >= text_end:
            break

        next_start = text_end
        for j in range(i + 1, len(subsegs)):
            next_s = subsegs[j]
            if isinstance(next_s, list) and len(next_s) >= 1:
                next_start = next_s[0]
                break
            elif isinstance(next_s, dict) and "start" in next_s:
                next_start = next_s["start"]
                break

        size = next_start - start
        name = s[2] if len(s) >= 3 else f"0x{start:X}"

        if stype == "c":
            c_bytes += size
            c_files.append((name, start, next_start, size))
        elif stype in ("asm", "hasm"):
            asm_bytes += size

    total_text = c_bytes + asm_bytes
    return {
        "game": game,
        "name": data.get("name", game),
        "c_files": c_files,
        "c_bytes": c_bytes,
        "asm_bytes": asm_bytes,
        "total_text": total_text,
    }


def render_progress_bar(percentage: float, width: int = 40) -> str:
    filled = int(width * (percentage / 100.0))
    bar = "=" * filled + "-" * (width - filled)
    return f"[{bar}] {percentage:5.2f}%"


def main():
    parser = argparse.ArgumentParser(description="Calculate decompilation progress.")
    parser.add_argument("--game", default="harvest-moon-64", help="Game identifier.")
    parser.add_argument("--verbose", "-v", action="store_true", help="List all decompiled C files.")
    args = parser.parse_args()

    prog = get_game_progress(args.game)
    if not prog:
        return 1

    total = prog["total_text"]
    c_bytes = prog["c_bytes"]
    pct = (c_bytes / total * 100.0) if total > 0 else 0.0

    print(f"\n=== Decompilation Progress: {prog['name']} ===")
    print(f"Total .text Executable Code:  {total:10,d} bytes")
    print(f"Decompiled to Matching C:     {c_bytes:10,d} bytes")
    print(f"Remaining in Assembly:        {prog['asm_bytes']:10,d} bytes")
    print()
    print("Overall .text Progress:")
    print(render_progress_bar(pct, 50))

    if args.verbose or True:
        print("\nDecompiled Modules:")
        for name, start, end, size in prog["c_files"]:
            print(f"  - {name:20s} [0x{start:05X} - 0x{end:05X}] {size:6,d} B ({size/1024:4.1f} KB)")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
