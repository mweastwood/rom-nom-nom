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
5. **Generality & Anti-Overfitting**:
   - Lifter passes must only recover general, standard C abstractions (clean
     control-flow structuring, stack-frame and callee-saved register fidelity,
     type width recovery, and redundant cast/pointer arithmetic simplification).
   - Never encode brittle, function-specific heuristics or GCC 2.7.2 register
     allocation quirks into the lifter to chase raw Myers LCS percentage.
   - Structural readiness for downstream bit-exact tuning is measured by
     per-function structural invariants (matching stack-frame size, saved
     register set, and basic-block/branch topology), leaving compiler-specific
     register allocation, expression evaluation order, and instruction
     scheduling perturbations to `tuner/`.
