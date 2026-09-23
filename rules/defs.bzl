"""
Bazel rules for N64 decompilation:
  - splat_split: Generates assembly, linker script, and binary assets
  - n64_rom: Builds .z64 ROM and .elf binary from C sources + generated asm
  - n64_rom_bitexact_test: Verifies bit-for-byte exact match against original ROM
  - n64_game: Macro that defines <game>_rom and <game>_rom_bitexact_test
"""

def _splat_split_impl(ctx):
    game = ctx.attr.game
    asm_dir = ctx.actions.declare_directory("asm/" + game)
    build_dir = ctx.actions.declare_directory("build/" + game)
    assets_dir = ctx.actions.declare_directory("assets/" + game)

    args = ctx.actions.args()
    args.add("--config", ctx.file.config)
    args.add("--rom", ctx.file.rom)
    args.add("--symbols", ctx.file.symbols)
    args.add("--out-asm", asm_dir.path)
    args.add("--out-build", build_dir.path)
    args.add("--out-assets", assets_dir.path)

    ctx.actions.run(
        inputs = [ctx.file.config, ctx.file.rom, ctx.file.symbols] + ctx.files.srcs,
        outputs = [asm_dir, build_dir, assets_dir],
        executable = ctx.executable._splat_runner,
        arguments = [args],
        mnemonic = "SplatSplit",
        execution_requirements = {"no-sandbox": "1"},
    )

    return [
        DefaultInfo(files = depset([asm_dir, build_dir, assets_dir])),
        OutputGroupInfo(
            asm = depset([asm_dir]),
            build = depset([build_dir]),
            assets = depset([assets_dir]),
        ),
    ]

splat_split = rule(
    implementation = _splat_split_impl,
    attrs = {
        "game": attr.string(mandatory = True),
        "config": attr.label(mandatory = True, allow_single_file = [".yaml"]),
        "symbols": attr.label(mandatory = True, allow_single_file = [".txt"]),
        "rom": attr.label(mandatory = True, allow_single_file = [".z64"]),
        "srcs": attr.label_list(allow_files = True, default = []),
        "_splat_runner": attr.label(
            default = "//tools:splat_runner",
            executable = True,
            cfg = "exec",
        ),
    },
)

def _n64_rom_impl(ctx):
    game = ctx.attr.game
    out_rom = ctx.actions.declare_file(game + ".z64")
    out_elf = ctx.actions.declare_file(game + ".elf")

    split_target = ctx.attr.split
    asm_dir = split_target[OutputGroupInfo].asm.to_list()[0]
    build_dir = split_target[OutputGroupInfo].build.to_list()[0]
    assets_dir = split_target[OutputGroupInfo].assets.to_list()[0]

    args = ctx.actions.args()
    args.add("--game", game)
    args.add("--config", ctx.file.config)
    args.add("--symbols", ctx.file.symbols)
    args.add("--asm-dir", asm_dir.path)
    args.add("--build-dir", build_dir.path)
    args.add("--assets-dir", assets_dir.path)
    args.add("--out-elf", out_elf.path)
    args.add("--out-rom", out_rom.path)
    args.add("--toolchain", ctx.attr.toolchain)

    ctx.actions.run(
        inputs = [ctx.file.config, ctx.file.symbols, asm_dir, build_dir, assets_dir] + ctx.files.srcs,
        outputs = [out_rom, out_elf],
        executable = ctx.executable._build_rom,
        arguments = [args],
        mnemonic = "N64Rom",
        execution_requirements = {"no-sandbox": "1"},
    )

    return [
        DefaultInfo(
            files = depset([out_rom]),
            runfiles = ctx.runfiles(files = [out_rom, out_elf]),
        ),
        OutputGroupInfo(
            rom = depset([out_rom]),
            elf = depset([out_elf]),
        ),
    ]

n64_rom = rule(
    implementation = _n64_rom_impl,
    attrs = {
        "game": attr.string(mandatory = True),
        "config": attr.label(mandatory = True, allow_single_file = [".yaml"]),
        "symbols": attr.label(mandatory = True, allow_single_file = [".txt"]),
        "split": attr.label(mandatory = True, providers = [OutputGroupInfo]),
        "srcs": attr.label_list(allow_files = True, default = []),
        "toolchain": attr.string(default = "original"),
        "_build_rom": attr.label(
            default = "//tools:build_rom",
            executable = True,
            cfg = "exec",
        ),
    },
)

def _n64_rom_bitexact_test_impl(ctx):
    rom = ctx.file.rom
    target_rom = ctx.file.target_rom
    config = ctx.file.config

    script = ctx.actions.declare_file(ctx.label.name + ".sh")
    script_content = """#!/usr/bin/env bash
set -euo pipefail

# Resolve file paths in runfiles
TARGET_ROM="{target_rom}"
BUILT_ROM="{built_rom}"

if [[ -f "$TARGET_ROM" ]]; then
    ACTUAL_TARGET="$TARGET_ROM"
elif [[ -f "${{RUNFILES_DIR:-.default_runfiles}}/$TARGET_ROM" ]]; then
    ACTUAL_TARGET="${{RUNFILES_DIR:-.default_runfiles}}/$TARGET_ROM"
else
    # Fallback to search in runfiles
    ACTUAL_TARGET=$(find . -name "$(basename "$TARGET_ROM")" | head -n 1)
fi

if [[ -f "$BUILT_ROM" ]]; then
    ACTUAL_BUILT="$BUILT_ROM"
elif [[ -f "${{RUNFILES_DIR:-.default_runfiles}}/$BUILT_ROM" ]]; then
    ACTUAL_BUILT="${{RUNFILES_DIR:-.default_runfiles}}/$BUILT_ROM"
else
    ACTUAL_BUILT=$(find . -name "$(basename "$BUILT_ROM")" | head -n 1)
fi

python3 -c "
import hashlib, sys
from pathlib import Path

target_p = Path('$ACTUAL_TARGET')
built_p = Path('$ACTUAL_BUILT')

orig = target_p.read_bytes()
built = built_p.read_bytes()

target_sha1 = hashlib.sha1(orig).hexdigest()
built_sha1 = hashlib.sha1(built).hexdigest()

print(f'Target ROM SHA-1: {{target_sha1}}')
print(f'Built  ROM SHA-1: {{built_sha1}}')

if orig != built:
    diff_count = sum(1 for a, b in zip(orig, built) if a != b)
    diff_count += abs(len(orig) - len(built))
    print(f'[FAIL] Byte mismatch: {{diff_count}} differing bytes!')
    sys.exit(1)

print('[SUCCESS] 100% byte-for-byte exact match verified!')
"
""".format(
        target_rom = target_rom.short_path,
        built_rom = rom.short_path,
    )

    ctx.actions.write(
        output = script,
        content = script_content,
        is_executable = True,
    )

    return [
        DefaultInfo(
            executable = script,
            runfiles = ctx.runfiles(files = [rom, target_rom, config]),
        ),
    ]

n64_rom_bitexact_test = rule(
    implementation = _n64_rom_bitexact_test_impl,
    test = True,
    attrs = {
        "rom": attr.label(mandatory = True, allow_single_file = [".z64"]),
        "target_rom": attr.label(mandatory = True, allow_single_file = [".z64"]),
        "config": attr.label(mandatory = True, allow_single_file = [".yaml"]),
    },
)

def n64_game(name, game, config, symbols, rom, srcs = []):
    """Macro to instantiate an N64 game with pure Bazel generated assembly pipeline."""
    split_name = name + "_split"
    rom_name = name + "_rom"
    test_name = name + "_rom_bitexact_test"

    splat_split(
        name = split_name,
        game = game,
        config = config,
        symbols = symbols,
        rom = rom,
        srcs = srcs,
    )

    n64_rom(
        name = rom_name,
        game = game,
        config = config,
        symbols = symbols,
        split = ":" + split_name,
        srcs = srcs,
    )

    n64_rom_bitexact_test(
        name = test_name,
        rom = ":" + rom_name,
        target_rom = rom,
        config = config,
    )
