## Philosophy

The build orchestrator driving the retail KMC GCC 2.7.2 / SN64 toolchain.
Compiles C sources and assembles split assembly into linked ELFs and runnable
N64 `.z64` ROMs.

## Invariants

1. Only allowed to depend on header files in `core/` and `builder/`.
2. **Configuration & Stage Isolation**:
   - Module compiler flags, include paths, and linker scripts must be derived
     strictly from `config/<game>.textproto`.
   - `builder/` must never implicitly load `config/<game>.tuning.textproto`
     during Stage 2 (`assembly_rom`) or Stage 3 (`lifted_c_rom`) builds. Tuning
     flag overrides are applied only when explicitly provided via
     `src_dir/tuned_c_flags.textproto` (Stage 4 `tuned_c_rom`) or an explicit
     `tuning_cache_file` field in `GameConfig`.
