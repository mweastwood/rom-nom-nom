## Philosophy

The clean-room ROM extraction and disassembly engine. Slices the retail ROM
binary into reproducible assembly files, data blobs, and linker scripts driven
purely by `config/<game>.textproto`.

## Invariants

1. Only allowed to depend on header files in `core/` and `splitter/`.
2. **Determinism**: Splitting the same ROM with the same configuration must
   produce byte-for-byte identical output files.
3. **Relocation Fidelity**: Linker scripts and assembly files must preserve
   exact symbol addresses and hardware entrypoint layouts.
