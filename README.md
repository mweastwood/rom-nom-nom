# rom-nom-nom

Playground for from-scratch decompilation and ROM patches for N64 games.

## Repository Layout

```text
rom-nom-nom/
├── .gitignore               # Strict ignore rules for ROMs, ASM, and build dumps
├── MODULE.bazel             # Bazel module configuration
├── BUILD.bazel              # Top-level Bazel build targets
├── requirements.txt         # Python tools (splat, spimdisasm, m2c, etc.)
├── roms/                    # Local baseroms (untracked)
│   ├── harvest-moon-64.z64
│   └── ogre-battle-64.z64
├── splat/                   # Splat YAML split configurations (tracked)
│   ├── BUILD.bazel
│   ├── harvest-moon-64.yaml
│   └── ogre-battle-64.yaml
├── symbols/                 # Symbol and reloc address maps (tracked)
│   ├── BUILD.bazel
│   ├── harvest-moon-64/
│   └── ogre-battle-64/
├── src/                     # C/C++ source and header files (tracked)
│   ├── harvest-moon-64/
│   └── ogre-battle-64/
├── asm/                     # Disassembled assembly (untracked)
│   ├── harvest-moon-64/
│   └── ogre-battle-64/
├── build/                   # Compiled ELF, linker scripts, and temp objects (untracked)
│   ├── harvest-moon-64/
│   └── ogre-battle-64/
├── patches/                 # Distributable BPS patch files (tracked)
│   ├── harvest-moon-64/
│   └── ogre-battle-64/
└── tools/                   # Helper scripts and reproducible tools (tracked)
    ├── BUILD.bazel
    ├── create_bps.py        # BPS patch generator (py_binary)
    └── split.py             # Reproducible ROM splitter & SHA-1 verifier (py_binary)
```

## Quick Start

### 1. Prerequisites

- **Bazel 9+**: Installed and available in PATH
- **GNU MIPS Toolchain**: `gcc-mips-linux-gnu`, `binutils-mips-linux-gnu`
- **Python 3.10+ virtualenv**:
  ```bash
  python3 -m venv .venv
  source .venv/bin/activate
  pip install -r requirements.txt
  ```

### 2. Disassembling / Splitting a ROM

Using **Bazel**:

```bash
# Split Harvest Moon 64
bazel run //:split_harvest_moon_64

# Split Ogre Battle 64
bazel run //:split_ogre_battle_64

# Or pass custom arguments through the tools:split target
bazel run //tools:split -- harvest-moon-64
```

Alternatively, you can run the script directly:
```bash
python3 tools/split.py harvest-moon-64
```

The split runner automatically verifies the target ROM's presence and SHA-1 checksum against the configuration before executing splat.

### 3. Decompiling Functions to C/C++

Use `m2c` from `.venv` to assist with decompiling target functions from assembly:

```bash
.venv/bin/m2c -t mips-gcc-c -f <function_name> asm/<game>/<file>.s
```

Write and refine the C implementation and headers in `src/<game>/`.

### 4. Creating Distribution Patches

Using Bazel to invoke the BPS patch generator:

```bash
bazel run //tools:create_bps -- create roms/harvest-moon-64.z64 build/harvest-moon-64/harvest-moon-64.z64 patches/harvest-moon-64/harvest-moon-64.bps
```
