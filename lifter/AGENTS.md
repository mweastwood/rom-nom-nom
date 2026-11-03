## Philosophy

The native clean-room MIPS-to-C inverse compilation engine. Reconstructs C ASTs
by authentically inverting each of KMC GCC 2.7.2's 14 compilation stages in
reverse chronological order:

`Binary (.o)` -> `.12.dbr` -> `.11.jump2` -> `.10.sched2` -> `.09.greg` ->
`.08.lreg` -> `.07.sched` -> `.06.combine` -> `.05.flow` -> `.04.cse2` ->
`.03.loop` -> `.02.cse` -> `.01.jump` -> `.00.rtl` -> `C AST`

## Invariants

1. **Allowed Dependencies**:
   - Only allowed to depend on header files in `core/` and `lifter/`.
2. **Clean-Room & GCC 2.7.2 Grounding**:
   - All stage inversions must be derived from original GCC 2.7.2 source logic
     (`final.c`, `reorg.c`, `jump.c`, `sched.c`, `global.c`, `reload1.c`,
     `local-alloc.c`, `combine.c`, `flow.c`, `cse.c`, `loop.c`, `stmt.c`,
     `expr.c`, and `config/mips/mips.md`) and hardware documentation.
3. **Purely Static & Deterministic ($O(1)$ per Function)**:
   - `lifter/` must never spawn compiler subprocesses or perform compile-and-check
     search.
4. **Google Style**:
   - All lifter C++ code and emitted C identifiers must adhere to Google Style
     (`CamelCase` functions, `lower_snake_case` variables, `kCamelCase`
     constants/enums).
5. **Strict Stage Isolation (Markov Property / No Side Channels)**:
   - Stage 0 (`InverseFinalAndGasStage`) lifts the machine `ControlFlowGraph`
     into the `.12.dbr` `RtlControlFlowGraph`.
   - Stages 1 through 12 must operate **exclusively** as pure transformations
     `RtlControlFlowGraph -> RtlControlFlowGraph` on the output of the
     immediately preceding stage. They must **never** read or mutate the raw
     machine `ControlFlowGraph` (`context->Cfg()`) or pass side-channel hints
     around intermediate RTL stages.
   - Stage 13 (`InverseRtlToCAstStage`) must synthesize the `CFunction` AST
     **exclusively** from Stage 12's recovered `.00.rtl` `RtlControlFlowGraph`
     (never bypassing `.00.rtl` via legacy direct machine-CFG lifting).
6. **Strict Stage Responsibility (No Cross-Stage Bleeding or No-Op Stubs)**:
   - Each stage $g_k$ must invert **only** the transformations performed by its
     corresponding GCC 2.7.2 pass $f_k$:
     - Do not implement another stage's responsibility early or late (e.g.,
       `InverseGregReloadStage` must only undo `global.c`/`reload1.c` hard
       register assignment, caller-save spills, and frame pointer elimination—never
       `combine`, `cse`, or `loop` transformations).
     - No stage may be left as a no-op pass-through when the corresponding GCC
       pass transforms the RTL.
7. **Zero Overspecialization / Zero Test-Corpus Cheating**:
   - Never hardcode register numbers, stack offsets, constant literals, loop
     bounds, or instruction sequences tailored to specific test cases in
     `core/round_trip_test_cases.h`.
   - Every transformation rule must be general over arbitrary registers, modes,
     expressions, and CFG topologies, directly mirroring the general algebraic
     or dataflow rule in GCC 2.7.2.
8. **Uncompromised Exact RTL Verification**:
   - Never weaken `RtlControlFlowGraph::CompareAgainst`, `CanonicalizeRtlPattern`,
     or `GccRtlDumpInspector::BuildCanonicalCfgFromRtlDump` to make tests pass.
   - Specifically, never wildcard pseudo/hard registers or stack addresses,
     never sort instructions within basic blocks, never strip RTL flags (`/v`,
     `/s`, `/i`, `/u`, `/f`), `REG_NOTES`, `NOTE_INSN_LOOP_*` notes,
     `(sequence [...])` bundles, `(use ...)`/`(clobber ...)`/`(return)` insns,
     or `(label_ref L<k>)` targets, and never coalesce or reorder basic blocks
     in the dump inspector.
