# `gcc_harness/` — In-Process KMC GCC 2.7.2 & GAS 2.6 Execution & Right-Inverse Verification Harness

## Purpose

`gcc_harness/` provides a `testonly = True` in-process execution and verification harness for all 26 individual KMC GCC 2.7.2 (`cc1`) and GNU Binutils 2.6 (`as`) C functions $f_i$, together with a lossless bidirectional bridge between `RtlControlFlowGraph` (`//core:rtl_cfg`) and GCC's in-memory `rtx` instruction chain and a standalone reference suite of 26 1-to-1 mathematical right-inverse functions $g_i$ in `SubfunctionInverses`.

## Core Invariants

1. **Mathematical Right-Inverse Law ($f_i(g_i(x)) = x$)**:
   - Forward compiler passes and helper functions $f_i: A \to B$ are frequently many-to-one (lossy).
   - Each inverse function $g_i: B \to A$ in `SubfunctionInverses` is verified by starting from a valid target-domain artifact $x \in \operatorname{Im}(f_i) \subseteq B$, computing $y = g_i(x) \in A$, and executing the genuine GCC/GAS forward function $f_i(y)$ in-process via `FunctionHooks` to verify:
     $$f_i(g_i(x)) = x$$
   - Tests must **never** assert the left-inverse $g_i(f_i(x)) = x$ on lossy passes.

2. **1-to-1 Subfunction Granularity & Zero Secret Massaging**:
   - Every individual GCC 2.7.2 and Binutils 2.6 C function exposed in `FunctionHooks` has a corresponding 1-to-1 inverse method in `SubfunctionInverses` (`subfunction_inverses.{h,cc}`) and a 1-to-1 verifier method in `RightInverseVerifier` (`right_inverse_verifier.{h,cc}`).
   - `GccContext`, `FunctionHooks`, and `RightInverseVerifier` perform **zero** post-pass normalization, **zero** secret mode or `(return)` coercions, and **zero** secret `(use (reg/i:SI 2 v0))` instruction injections/deletions.

3. **GPL & Clean-Room Isolation**:
   - GPL source code (`toolchain/gcc-2.7.2-src/` and `toolchain/binutils-2.6-src/`) and static archives (`toolchain/libgcc272_harness.a` and `toolchain/libgas26_harness.a`) are gitignored and built locally via `gcc_harness/setup_sources.sh` (`tools/setup_gcc_harness.sh`).
   - Every target in `gcc_harness/BUILD.bazel` is marked `testonly = True`.
   - Production targets in `lifter/` must **never** depend on `gcc_harness/`, and `gcc_harness/` has **zero** dependencies on `//lifter:*` targets (sharing the unified `RtlControlFlowGraph` IR and RTL S-expression utilities in `//core:rtl_cfg` and `//core:rtl_sexpr_utils`).

4. **Zero Stubs or Fake Fallbacks**:
   - `GccContext` initializes the complete GCC 2.7.2 backend state (obstacks, RTL tables, register sets, optabs, recognizer, loop tables, alias analysis, and a synthetic `FUNCTION_DECL` with `RESULT_DECL` and `BLOCK`) and Binutils 2.6 GAS state (`bfd_init`, `output_file_create("/dev/null")`, `subsegs_begin`, `frag_init`, `md_begin`, `target_big_endian = 1`).
   - Every method in `FunctionHooks` executes the genuine GCC 2.7.2 or GAS 2.6 C function in-process.
