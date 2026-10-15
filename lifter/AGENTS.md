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
