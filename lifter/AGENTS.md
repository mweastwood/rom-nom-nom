## Philosophy

The native clean-room MIPS-to-C decompilation engine. Reconstructs control
flow, types, stack variables, and expressions from raw assembly into readable,
standard C code targeting GCC 2.7.2.

## Invariants

1. Only allowed to depend on header files in `core/` and `lifter/`.
2. **Clean-Room Derivation**: Code structure and types must be derived solely
   from local assembly analysis, control-flow graphs, and ROM data.
3. **Re-compilable C**: Generated C code must compile cleanly.
4. **Google Style**: Emitted functions and types must adhere to Google Style
   (`CamelCase` functions, `lower_snake_case` variables, `kCamelCase`
   constants/enums).
5. **Purely Static & Deterministic ($O(1)$ per Function)**:
   - `lifter/` must never spawn compiler subprocesses or perform compile-and-check
     search.
6. **Observable Recovery vs. Compiler-Threshold Guessing**:
   - `lifter/` owns the recovery of structure that is **explicitly observable in
     the target binary**:
     - Structured control flow (`if`/`else`, `while`/`do-while`, `switch`,
       short-circuit `&&`/`||`) without unstructured `goto` fallbacks or
       materialized boolean temporaries (`v0 = (a < b); if (v0 != 0)`).
     - Hardware-accurate narrow type inference (`s8`/`u8`/`s16`/`u16`) without
       redundant `(u8)`/`(u16)` casts when `lbu`/`lhu` already zero-extend.
     - Variable live ranges across `jal` calls that reflect the binary's actual
       callee-saved (`$s0`–`$s7`) vs. caller-saved (`$t0`–`$t9`) usage.
   - Do **not** encode brittle static guesses for non-injective GCC 2.7.2
     thresholds (such as whether GCC's `reload` pass allocates an extra $\pm 8$-byte
     stack spill slot or how `local-alloc` breaks ties between declarations);
     those require compile-in-the-loop verification in `tuner/`.
7. **Pass Modularity**:
   - Keep AST and CFG passes small, single-responsibility, and compositional.
     Avoid monolithic multi-hundred-line pattern matchers.
