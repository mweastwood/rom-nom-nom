## Philosophy

A clean-room reverse-engineering, automated decompilation, and semantic
verification platform for Nintendo 64 software.

## Invariants

0. **Clean-Room Decompilation & External Reference Policy**:
   - Contributors and AI agents are **strictly forbidden** from viewing,
     querying, citing, referencing, or deriving code, structures, or symbols
     from other third-party decompilation projects.
   - All decompilation must be derived solely from original retail ROM
     binaries, local disassembly, official hardware documentation, and
     independent analysis.

1. **Google Style Guide Compliance**:
   - All C/C++ code must follow Google Style:
     - **Functions**: `CamelCase` without underscores (`MessageInit`, `AudioUpdate`).
     - **Variables & Members**: `lower_snake_case` (`box_index`, `bank_id`).
     - **Constants & Enums**: `kCamelCase` with `k` prefix (`kIdle`, `kMaxSteps`).
     - **Macros**: `UPPER_SNAKE_CASE` (`TEST`, `TEST_F`).

2. **Split Assembly ROM (100% Bit-Exact Match)**:
   - Reassembling split assembly files and manual C modules must produce a
     byte-for-byte SHA-1 bit-exact match against the original retail ROM
     (`bazel test //:<game>_assembly_rom_bitexact_test`).

3. **Lifted C ROM (100% Behavioral Equivalence)**:
   - The whole-game lifted C ROM is not required to be bit-exact to the retail
     binary, but it **must achieve 100% behavioral equivalence** across the
     differential fuzzer suite (`bazel test //:<game>_lifted_c_equivalence_test`).
   - Structural integrity must be preserved: valid N64 header magic,
     entrypoint, and bit-exact downstream asset segments when code expansion
     shifts occur.
