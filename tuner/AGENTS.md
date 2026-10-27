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

3. **Lexicographical Structural & Bit-Exact Monotonicity**:
   - Candidate scoring (`CandidateScore::IsBetterThan`) follows a strict
     hierarchy:
     1. Bit-exact match (`raw_bit_exact` / `normalized_exact`).
     2. Structural isomorphism (`frame_size_matches`, `saved_registers_match`,
        `cfg_topology_matches`).
     3. Net bit-exact and normalized instruction alignment penalized by size
        delta.
   - A non-bit-exact candidate must **never** be accepted if it regresses any
     structural isomorphism dimension of the baseline lifted function.

4. **Separation of Concerns (Lifter vs. Tuner)**:
   - While `lifter/` statically recovers observable control flow, types, and
     live ranges (~90%+ of structural isomorphism), compilation under GCC 2.7.2
     `-O2` is many-to-one on a ~7–10% tail where small AST variations flip
     `reload` stack-spill slots ($\pm 8$ bytes), marginal `$s0`–`$s7`
     allocations, or `jump` cross-jumping thresholds.
   - `tuner/` owns both:
     - **Bridging that non-injective structural tail** via compile-in-the-loop
       AST mutations (temp inlining/splitting, declaration reordering, register
       copy propagation), and
     - **Closing the final bit-exactness gap** on structurally isomorphic
       functions (operand commutation, statement swapping, condition inversion,
       and assembler/codegen flags).
