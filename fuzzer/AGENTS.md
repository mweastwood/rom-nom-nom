## Philosophy

The semantic equivalence testing engine. Emulates target retail MIPS code and
candidate recompiled C code across hundreds of randomized and boundary test
vectors to mathematically prove behavioral equivalence.

## Invariants

1. Only allowed to depend on header files in `core/` and `fuzzer/`.
2. **Deterministic Execution**: Given the same PRNG seed and input matrices,
   test runs must produce identical results across runs and host architectures.
3. **Fail Loudly**: Any unhandled divergence in execution status, unexpected
   infinite loops, or memory write mismatches must fail the test immediately.
