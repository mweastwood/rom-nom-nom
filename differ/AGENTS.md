## Philosophy

The verification and visualization engine. Compares target retail ROM binaries
and instruction streams against recompiled candidates to prove bit-exact or
shift-aligned equivalence.

## Invariants

1. Only allowed to depend on header files in `core/` and `differ/`.
