#!/usr/bin/env python3
"""
Splat runner action for Bazel.
Splits an N64 ROM into Bazel-managed output directories:
  - out_asm: generated .s assembly files and macros
  - out_build: linker script (.ld), undefined symbols and funcs
  - out_assets: extracted binary assets
"""

import argparse
import os
import shutil
import sys
from pathlib import Path
import yaml
from splat.scripts import split


def main():
    parser = argparse.ArgumentParser(description="Run splat to generate assembly, linker scripts, and assets.")
    parser.add_argument("--config", required=True, type=Path, help="Input splat YAML config")
    parser.add_argument("--rom", required=True, type=Path, help="Input target ROM")
    parser.add_argument("--symbols", required=True, type=Path, help="Symbols text file")
    parser.add_argument("--out-asm", required=True, type=Path, help="Output asm directory")
    parser.add_argument("--out-build", required=True, type=Path, help="Output build directory")
    parser.add_argument("--out-assets", required=True, type=Path, help="Output assets directory")
    parser.add_argument("--src-dir", type=Path, default=None, help="Root of source directory")
    parser.add_argument("--extensions-dir", type=Path, default=None, help="Splat extensions directory")
    args = parser.parse_args()

    # Determine repo root
    repo_root = Path(
        os.environ.get("BUILD_WORKSPACE_DIRECTORY") or Path(__file__).resolve().parent.parent
    ).resolve()

    # Recreate clean output directories
    for d in [args.out_asm, args.out_build, args.out_assets]:
        if d.exists():
            shutil.rmtree(d)
        d.mkdir(parents=True, exist_ok=True)

    with open(args.config, "r", encoding="utf-8") as f:
        cfg = yaml.safe_load(f)

    options = cfg.setdefault("options", {})
    basename = options.get("basename", args.config.stem)

    options["asm_path"] = str(args.out_asm.resolve())
    options["build_path"] = str(args.out_build.resolve())
    options["asset_path"] = str(args.out_assets.resolve())
    options["generated_asm_macros_directory"] = str(args.out_asm.resolve())
    options["ld_script_path"] = str((args.out_build / f"{basename}.ld").resolve())
    options["elf_path"] = str((args.out_build / f"{basename}.elf").resolve())
    options["target_path"] = str(args.rom.resolve())
    options["symbol_addrs_path"] = [str(args.symbols.resolve())]
    options["undefined_funcs_auto_path"] = str((args.out_build / "undefined_funcs_auto.txt").resolve())
    options["undefined_syms_auto_path"] = str((args.out_build / "undefined_syms_auto.txt").resolve())
    options["base_path"] = str(repo_root)

    if args.extensions_dir and args.extensions_dir.exists():
        options["extensions_path"] = str(args.extensions_dir.resolve())
    elif (repo_root / "tools" / "splat_ext").exists():
        options["extensions_path"] = str((repo_root / "tools" / "splat_ext").resolve())

    if args.src_dir and args.src_dir.exists():
        options["src_path"] = str(args.src_dir.resolve())
    elif "src_path" in options:
        options["src_path"] = str((repo_root / options["src_path"]).resolve())

    options["cache_path"] = str((args.out_build / ".splache").resolve())

    tmp_config = args.out_build / f"{basename}_splat.yaml"
    with open(tmp_config, "w", encoding="utf-8") as f:
        yaml.dump(cfg, f)

    split.main([tmp_config], modes=["all"], verbose=False, use_cache=False)

    # Post-process generated macro.inc to strip directives unsupported by authentic 1996 GAS
    macro_inc = args.out_asm / "macro.inc"
    if macro_inc.exists():
        lines = macro_inc.read_text(encoding="utf-8").splitlines(keepends=True)
        filtered_lines = [l for l in lines if not l.strip().startswith(".internal")]
        macro_inc.write_text("".join(filtered_lines), encoding="utf-8")


if __name__ == "__main__":
    main()
