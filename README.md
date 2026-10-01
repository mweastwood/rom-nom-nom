# rom-nom-nom

Playground for from-scratch decompilation and ROM patches for N64 games.

## Repository Layout

```text
rom-nom-nom/
├── .gitignore               # Strict ignore rules for ROMs, ASM, and build dumps
├── MODULE.bazel             # Bazel module configuration
├── BUILD.bazel              # Top-level Bazel build targets
├── requirements.txt         # Python tools (spimdisasm, m2c, etc.)
├── rules/                   # Starlark rules for hermetic N64 ROM builds (tracked)
│   └── defs.bzl             # splitter_split, n64_rom, and n64_rom_bitexact_test
├── roms/                    # Local baseroms (binaries untracked, BUILD tracked)
│   ├── BUILD.bazel
│   ├── harvest-moon-64.z64
│   └── ogre-battle-64.z64
├── config/                  # Split configurations (tracked)
│   ├── BUILD.bazel
│   ├── harvest-moon-64.textproto
│   └── ogre-battle-64.textproto
├── macros/                  # Hardware hazard and assembler compatibility macros (tracked)
│   ├── BUILD.bazel
│   └── harvest-moon-64_macro.inc
├── symbols/                 # Symbol and reloc address maps (tracked)
│   ├── BUILD.bazel
│   ├── harvest-moon-64.textproto
│   └── ogre-battle-64.textproto
├── src/                     # C/C++ source and header files (tracked)
│   ├── c/harvest-moon-64/
│   ├── cc/harvest-moon-64/
│   └── ogre-battle-64/
├── splitter/                # Fast hermetic N64 binary splitter & disassembler (tracked)
└── tools/                   # Helper scripts and reproducible tools (tracked)
    ├── BUILD.bazel
    ├── build_rom.py         # Bazel ROM build & bit-exact verification action
    ├── install_toolchain.py # GCC 2.7.2 installer
    └── format.py            # Codebase formatter
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

### 2. Building ROMs with Bazel

All assembly files and linker scripts are treated as **generated artifacts** by Bazel. The in-house `//splitter` tool is executed hermetically as a Bazel action whenever split configs (`.textproto`) or symbols change.

```bash
# Build matching ROM for Harvest Moon 64
bazel build //:harvest_moon_64_rom

# Build matching ROM for Ogre Battle 64
bazel build //:ogre_battle_64_rom

# Build all ROMs
bazel build //...
```

The resulting ROMs are output to:
- `bazel-bin/harvest-moon-64.z64`
- `bazel-bin/ogre-battle-64.z64`

### 3. Generated Disassembly

Because assembly is managed by Bazel, the generated `.s` files and linker scripts are located in:
- `bazel-bin/asm/harvest-moon-64/`
- `bazel-bin/asm/ogre-battle-64/`

You can inspect, search, and view them directly in your editor at those paths.

### 4. Running Bit-Exact Matching Tests

```bash
# Run bit-exact ROM match tests
bazel test //:harvest_moon_64_rom_bitexact_test
bazel test //:ogre_battle_64_rom_bitexact_test

# Run all test suites across the repository
bazel test //...
```

### 5. Formatting Code

```bash
bazel run //:format
```

### 6. Git Hooks (Automatic Code Formatting)

A pre-commit hook is provided in `.githooks/` to ensure all staged C and C++ files are automatically formatted before every commit:

```bash
git config core.hooksPath .githooks
```
