## Philosophy

The build orchestrator driving the retail KMC GCC 2.7.2 / SN64 toolchain.
Compiles C sources and assembles split assembly into linked ELFs and runnable
N64 `.z64` ROMs.

## Invariants

1. Only allowed to depend on header files in `core/` and `builder/`.
2. **Configuration Driven**: Module compiler flags, include paths, and linker
   scripts must be derived strictly from `config/<game>.textproto`.
