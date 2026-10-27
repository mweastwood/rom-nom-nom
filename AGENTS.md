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

3. **Lifted C ROM (100% Behavioral Equivalence & Pure Static Recovery)**:
   - The whole-game lifted C ROM (`bazel test //:<game>_lifted_c_equivalence_test`)
     is produced strictly by static analysis (`lifter/`) compiled with baseline
     module flags from `config/<game>.textproto`—never contaminated by `tuner/`
     caches.
   - It must achieve **100% behavioral equivalence** across the differential
     fuzzer suite and preserve structural ROM integrity (valid N64 header magic,
     entrypoint, and bit-exact downstream asset segments under code shifts).
   - The lifter itself must be completely general, such that adding a new ROM
     immediately achieves 100% behavioral equivalence.

4. **Tuned C ROM (Monotonic Structural & Bit-Exact Convergence)**:
   - The tuned C ROM (`bazel test //:<game>_tuned_c_equivalence_test`) applies
     compile-in-the-loop AST and flag search (`tuner/`) on top of `lifter/`.
   - It must maintain **100% behavioral equivalence** and **monotonically
     preserve or improve** both per-function structural isomorphism and raw
     instruction alignment over the Lifted C ROM (never regressing stack-frame,
     saved-register, or CFG matches for accidental instruction hits).

5. **Thorough Unit Testing**:
   - All changes should be accompanied with unit tests to verify that
     improvements do not regress.
   - Tautological unit tests are unnecessary (e.g., testing the contents of a
     map or anything that is blindingly obvious from the code).
