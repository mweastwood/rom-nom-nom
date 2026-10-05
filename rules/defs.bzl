"""
Bazel rules for N64 decompilation:
  - splitter_split: Generates assembly, linker script, and binary assets via //splitter:splitter
  - n64_rom: Builds .z64 ROM and .elf binary from C sources + generated asm
  - n64_rom_bitexact_test: Verifies bit-for-byte exact match against original ROM
  - n64_game: Macro that defines <game>_rom and <game>_rom_bitexact_test
  - lifter_lift: Lifts functions/modules into clean C implementation files via //lifter:lifter
"""

def _splitter_split_impl(ctx):
    game = ctx.attr.game
    asm_dir = ctx.actions.declare_directory("asm/" + game)
    build_dir = ctx.actions.declare_directory("build/" + game)
    assets_dir = ctx.actions.declare_directory("assets/" + game)

    args = ctx.actions.args()
    args.add("--config", ctx.file.config)
    args.add("--rom", ctx.file.rom)
    args.add("--symbols", ctx.file.symbols)
    args.add("--out_dir", build_dir.path)
    args.add("--asm_out_dir", asm_dir.path)
    args.add("--assets_out_dir", assets_dir.path)

    ctx.actions.run(
        inputs = [ctx.file.config, ctx.file.rom, ctx.file.symbols] + ctx.files.srcs,
        outputs = [asm_dir, build_dir, assets_dir],
        executable = ctx.executable._splitter,
        arguments = [args],
        mnemonic = "Splitter",
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

splitter_split = rule(
    implementation = _splitter_split_impl,
    attrs = {
        "game": attr.string(mandatory = True),
        "config": attr.label(mandatory = True, allow_single_file = [".textproto"]),
        "symbols": attr.label(mandatory = True, allow_single_file = [".textproto"]),
        "rom": attr.label(mandatory = True, allow_single_file = [".z64"]),
        "srcs": attr.label_list(allow_files = True, default = []),
        "_splitter": attr.label(
            default = "//splitter:splitter",
            executable = True,
            cfg = "exec",
        ),
    },
)


def _n64_rom_impl(ctx):
    game = ctx.attr.game
    out_name = ctx.attr.out_name if ctx.attr.out_name else game
    out_rom = ctx.actions.declare_file(out_name + ".z64")
    out_elf = ctx.actions.declare_file(out_name + ".elf")

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

    inputs = [ctx.file.config, ctx.file.symbols, asm_dir, build_dir, assets_dir] + ctx.files.srcs
    if ctx.attr.src_dir:
        src_dir_file = ctx.file.src_dir
        args.add("--src-dir", src_dir_file.path)
        inputs.append(src_dir_file)
    if ctx.attr.prefer_c:
        args.add("--prefer-c")

    ctx.actions.run(
        inputs = inputs,
        outputs = [out_rom, out_elf],
        executable = ctx.executable._build_rom,
        arguments = [args],
        mnemonic = "N64Rom",
        execution_requirements = {"no-sandbox": "1"},
        use_default_shell_env = True,
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
        "config": attr.label(mandatory = True, allow_single_file = [".textproto"]),
        "symbols": attr.label(mandatory = True, allow_single_file = [".textproto"]),
        "split": attr.label(mandatory = True, providers = [OutputGroupInfo]),
        "srcs": attr.label_list(allow_files = True, default = []),
        "src_dir": attr.label(allow_single_file = True),
        "out_name": attr.string(default = ""),
        "prefer_c": attr.bool(default = False),
        "toolchain": attr.string(default = "original"),
        "_build_rom": attr.label(
            default = "//builder:builder",
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
        "config": attr.label(mandatory = True, allow_single_file = [".textproto"]),
    },
)

def _n64_lifted_c_rom_diff_test_impl(ctx):
    candidate_rom = ctx.file.candidate_rom
    base_rom = ctx.file.base_rom
    config = ctx.file.config
    differ = ctx.executable._differ

    script = ctx.actions.declare_file(ctx.label.name + ".sh")

    script_content = """#!/usr/bin/env bash
set -euo pipefail

BASE_ROM="{base_rom}"
CANDIDATE_ROM="{candidate_rom}"
CONFIG_FILE="{config}"
DIFFER_BIN="{differ}"

resolve_path() {{
    local path="$1"
    if [[ -f "$path" ]]; then
        echo "$path"
    elif [[ -f "${{RUNFILES_DIR:-.default_runfiles}}/$path" ]]; then
        echo "${{RUNFILES_DIR:-.default_runfiles}}/$path"
    else
        find . -name "$(basename "$path")" | head -n 1
    fi
}}

ACTUAL_BASE=$(resolve_path "$BASE_ROM")
ACTUAL_CANDIDATE=$(resolve_path "$CANDIDATE_ROM")
ACTUAL_CONFIG=$(resolve_path "$CONFIG_FILE")
ACTUAL_DIFFER=$(resolve_path "$DIFFER_BIN")

"$ACTUAL_DIFFER" \
    --base_rom="$ACTUAL_BASE" \
    --candidate_rom="$ACTUAL_CANDIDATE" \
    --config="$ACTUAL_CONFIG" \
    --nocolor
""".format(
        base_rom = base_rom.short_path,
        candidate_rom = candidate_rom.short_path,
        config = config.short_path,
        differ = differ.short_path,
    )

    ctx.actions.write(
        output = script,
        content = script_content,
        is_executable = True,
    )

    return [
        DefaultInfo(
            executable = script,
            runfiles = ctx.runfiles(files = [candidate_rom, base_rom, config, differ]).merge(
                ctx.attr._differ[DefaultInfo].default_runfiles,
            ),
        ),
    ]

n64_lifted_c_rom_diff_test = rule(
    implementation = _n64_lifted_c_rom_diff_test_impl,
    test = True,
    attrs = {
        "candidate_rom": attr.label(mandatory = True, allow_single_file = [".z64"]),
        "base_rom": attr.label(mandatory = True, allow_single_file = [".z64"]),
        "config": attr.label(mandatory = True, allow_single_file = [".textproto"]),
        "_differ": attr.label(
            default = Label("//differ:differ"),
            executable = True,
            cfg = "exec",
        ),
    },
)

n64_lifted_c_rom_test = n64_lifted_c_rom_diff_test

def _n64_lifted_c_equivalence_test_impl(ctx):
    candidate_rom = ctx.file.candidate_rom
    base_rom = ctx.file.base_rom
    config = ctx.file.config
    symbols = ctx.file.symbols
    fuzzer = ctx.executable._fuzzer
    game = ctx.attr.game

    script = ctx.actions.declare_file(ctx.label.name + ".sh")

    script_content = """#!/usr/bin/env bash
set -euo pipefail

BASE_ROM="{base_rom}"
CANDIDATE_ROM="{candidate_rom}"
CONFIG_FILE="{config}"
SYMBOLS_FILE="{symbols}"
FUZZER_BIN="{fuzzer}"
GAME="{game}"

resolve_path() {{
    local path="$1"
    if [[ -f "$path" ]]; then
        echo "$path"
    elif [[ -f "${{RUNFILES_DIR:-.default_runfiles}}/$path" ]]; then
        echo "${{RUNFILES_DIR:-.default_runfiles}}/$path"
    elif [[ -n "${{TEST_SRCDIR:-}}" && -f "$TEST_SRCDIR/$path" ]]; then
        echo "$TEST_SRCDIR/$path"
    elif [[ -n "${{TEST_SRCDIR:-}}" && -f "$TEST_SRCDIR/_main/$path" ]]; then
        echo "$TEST_SRCDIR/_main/$path"
    else
        find . -name "$(basename "$path")" | head -n 1
    fi
}}

ACTUAL_BASE=$(resolve_path "$BASE_ROM")
ACTUAL_CANDIDATE=$(resolve_path "$CANDIDATE_ROM")
ACTUAL_CONFIG=$(resolve_path "$CONFIG_FILE")
ACTUAL_SYMBOLS=$(resolve_path "$SYMBOLS_FILE")
ACTUAL_FUZZER=$(resolve_path "$FUZZER_BIN")

CANDIDATE_ELF="${{CANDIDATE_ROM%.z64}}.elf"
ACTUAL_CANDIDATE_ELF=$(resolve_path "$CANDIDATE_ELF" || true)

EXTRA_FLAGS=""
if [[ -n "$ACTUAL_CANDIDATE_ELF" && -f "$ACTUAL_CANDIDATE_ELF" ]]; then
    EXTRA_FLAGS="--candidate_elf=$ACTUAL_CANDIDATE_ELF"
fi

"$ACTUAL_FUZZER" \
    --game="$GAME" \
    --base_rom="$ACTUAL_BASE" \
    --candidate_rom="$ACTUAL_CANDIDATE" \
    --config="$ACTUAL_CONFIG" \
    --symbols="$ACTUAL_SYMBOLS" \
    $EXTRA_FLAGS \
    --nocolor
""".format(
        game = game,
        base_rom = base_rom.short_path,
        candidate_rom = candidate_rom.short_path,
        config = config.short_path,
        symbols = symbols.short_path,
        fuzzer = fuzzer.short_path,
    )

    ctx.actions.write(
        output = script,
        content = script_content,
        is_executable = True,
    )

    runfiles = ctx.runfiles(files = [candidate_rom, base_rom, config, symbols, fuzzer]).merge(
        ctx.attr._fuzzer[DefaultInfo].default_runfiles,
    )
    if DefaultInfo in ctx.attr.candidate_rom:
        runfiles = runfiles.merge(ctx.attr.candidate_rom[DefaultInfo].default_runfiles)

    return [
        DefaultInfo(
            executable = script,
            runfiles = runfiles,
        ),
    ]

n64_lifted_c_equivalence_test = rule(
    implementation = _n64_lifted_c_equivalence_test_impl,
    test = True,
    attrs = {
        "game": attr.string(default = "harvest-moon-64"),
        "candidate_rom": attr.label(mandatory = True, allow_single_file = [".z64"]),
        "base_rom": attr.label(mandatory = True, allow_single_file = [".z64"]),
        "config": attr.label(mandatory = True, allow_single_file = [".textproto"]),
        "symbols": attr.label(mandatory = True, allow_single_file = [".textproto"]),
        "_fuzzer": attr.label(
            default = Label("//fuzzer:fuzzer"),
            executable = True,
            cfg = "exec",
        ),
    },
)

def n64_game(name, game, config, symbols, rom, srcs = []):
    """Macro to instantiate an N64 game with pure Bazel generated assembly pipeline."""
    split_name = name + "_split"
    assembly_rom_name = name + "_assembly_rom"
    assembly_test_name = name + "_assembly_rom_bitexact_test"

    splitter_split(
        name = split_name,
        game = game,
        config = config,
        symbols = symbols,
        rom = rom,
        srcs = srcs,
    )

    # Stage 2: Pure assembly ROM (built strictly from disassembled assembly, no C files)
    n64_rom(
        name = assembly_rom_name,
        game = game,
        config = config,
        symbols = symbols,
        split = ":" + split_name,
        out_name = game,
    )

    n64_rom_bitexact_test(
        name = assembly_test_name,
        rom = ":" + assembly_rom_name,
        target_rom = rom,
        config = config,
    )

    # Backward-compatibility aliases
    native.alias(
        name = name + "_rom",
        actual = ":" + assembly_rom_name,
    )
    native.test_suite(
        name = name + "_rom_bitexact_test",
        tests = [":" + assembly_test_name],
    )

    # Equivalence verification test via differential fuzzer
    equivalence_test_name = name + "_equivalence_test"
    n64_lifted_c_equivalence_test(
        name = equivalence_test_name,
        game = game,
        candidate_rom = ":" + assembly_rom_name,
        base_rom = rom,
        config = config,
        symbols = symbols,
    )

    # Stage 3: Whole-game lifted C pipeline, lifted C ROM, and validation test
    lifted_c_name = name + "_lifted_c"
    lifted_c_rom_name = name + "_lifted_c_rom"
    lifted_c_diff_test_name = name + "_lifted_c_rom_diff_test"

    lifter_lift_game(
        name = lifted_c_name,
        game = game,
        config = config,
        symbols = symbols,
        rom = rom,
        split = ":" + split_name,
    )

    # Backward-compatible alias for lifted directory
    native.alias(
        name = name + "_lifted",
        actual = ":" + lifted_c_name,
    )

    n64_rom(
        name = lifted_c_rom_name,
        game = game,
        config = config,
        symbols = symbols,
        split = ":" + split_name,
        src_dir = ":" + lifted_c_name,
        out_name = game + "_lifted_c",
        prefer_c = True,
    )

    # Backward-compatible alias for lifted ROM
    native.alias(
        name = name + "_lifted_rom",
        actual = ":" + lifted_c_rom_name,
    )

    n64_lifted_c_rom_diff_test(
        name = lifted_c_diff_test_name,
        candidate_rom = ":" + lifted_c_rom_name,
        base_rom = rom,
        config = config,
    )

    # Backward-compatible alias for lifted diff test
    native.test_suite(
        name = name + "_lifted_c_rom_test",
        tests = [":" + lifted_c_diff_test_name],
    )

    n64_lifted_c_equivalence_test(
        name = name + "_lifted_c_equivalence_test",
        game = game,
        candidate_rom = ":" + lifted_c_rom_name,
        base_rom = rom,
        config = config,
        symbols = symbols,
    )

def _lifter_lift_game_impl(ctx):
    out_dir = ctx.actions.declare_directory("lifted/" + ctx.attr.game)

    args = ctx.actions.args()
    args.add("--game=" + ctx.attr.game)
    args.add("--config=" + ctx.file.config.path)
    args.add("--symbols=" + ctx.file.symbols.path)
    args.add("--rom=" + ctx.file.rom.path)
    args.add("--output_dir=" + out_dir.path)
    if not ctx.attr.format:
        args.add("--noformat")

    inputs = [ctx.file.config, ctx.file.symbols, ctx.file.rom]
    if ctx.attr.split:
        asm_dir = ctx.attr.split[OutputGroupInfo].asm.to_list()[0]
        inputs.append(asm_dir)
        args.add("--asm_dir=" + asm_dir.path)

    ctx.actions.run(
        inputs = inputs,
        outputs = [out_dir],
        executable = ctx.executable._lifter,
        arguments = [args],
        mnemonic = "LifterGame",
        execution_requirements = {"no-sandbox": "1"},
    )

    return [
        DefaultInfo(files = depset([out_dir])),
    ]

lifter_lift_game = rule(
    implementation = _lifter_lift_game_impl,
    attrs = {
        "game": attr.string(mandatory = True),
        "config": attr.label(mandatory = True, allow_single_file = [".textproto"]),
        "symbols": attr.label(mandatory = True, allow_single_file = [".textproto"]),
        "rom": attr.label(mandatory = True, allow_single_file = [".z64"]),
        "split": attr.label(mandatory = False, providers = [OutputGroupInfo]),
        "format": attr.bool(default = True),
        "_lifter": attr.label(
            default = "//lifter:lifter",
            executable = True,
            cfg = "exec",
        ),
    },
)
