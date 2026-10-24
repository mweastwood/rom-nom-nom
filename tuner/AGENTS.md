## Philosophy

Automated bit-exactness search and compiler/AST tuning engine for lifted N64 C code.

## Invariants

1. **Semantic Preservation**:
   - Every AST mutation and compiler flag candidate explored by `tuner` must
     preserve program semantics (verified via structural AST invariants and
     optional `DifferentialFuzzer` equivalence checking).

2. **Deterministic O(1) Cache Application**:
   - When a per-function tuning cache entry matches `sha1(target_raw_words +
     baseline_c_code)`, `tuner` applies the cached AST mutation recipe and
     compiler flags deterministically in $O(1)$ without spawning compiler
     subprocesses.

3. **Monotonic Improvement**:
   - Candidate variations are only accepted when they strictly improve
     instruction alignment and bit-exactness score over the baseline.

4. **Separation of Concerns (Lifter vs. Tuner)**:
   - `tuner` owns compiler-specific codegen exploration (GCC 2.7.2 local
     declaration ordering, subexpression extraction vs. inlining, commutative
     operand swapping, condition polarity inversion, and per-function compiler
     flags).
   - `tuner` is designed to close the final bit-exactness gap on functions that
     are already structurally isomorphic to the retail binary (matching stack
     frame size, callee-saved register set, and basic-block/branch topology);
     fundamental control-flow, stack-frame, or type-recovery gaps belong in
     `lifter/`.
