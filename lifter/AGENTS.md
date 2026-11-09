## Philosophy

The native clean-room MIPS-to-C inverse compilation engine. Reconstructs C ASTs
by authentically inverting each of KMC GCC 2.7.2's 14 compilation stages in
reverse chronological order:

`Binary (.o)` -> `.12.dbr` -> `.11.jump2` -> `.10.sched2` -> `.09.greg` ->
`.08.lreg` -> `.07.sched` -> `.06.combine` -> `.05.flow` -> `.04.cse2` ->
`.03.loop` -> `.02.cse` -> `.01.jump` -> `.00.rtl` -> `C AST`

## Invariants

1. **Allowed Dependencies**:
   - Production library files in `lifter/` are only allowed to depend on header
     files in `core/` and `lifter/` (plus standard library and Abseil).
   - Unit test files in `lifter/` (`lifter/*_test.cc`) may additionally depend
     on `gcc_harness/` (`testonly = True`) to verify the right-inverse law
     $f_i(g_i(x)) = x$ against real in-process KMC GCC 2.7.2 and Binutils 2.6
     GAS passes.
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
8. **Uncompromised Exact RTL & Right-Inverse Verification**:
   - Never weaken `RtlControlFlowGraph::CompareAgainst`, `CanonicalizeRtlPattern`,
     or in-process `gcc_harness` right-inverse verification ($f_i(g_i(x)) = x$) to
     make tests pass.
   - Specifically, never wildcard pseudo/hard registers or stack addresses,
     never sort instructions within basic blocks, never strip RTL flags (`/v`,
     `/s`, `/i`, `/u`, `/f`), `REG_NOTES`, `NOTE_INSN_LOOP_*` notes,
     `(sequence [...])` bundles, `(use ...)`/`(clobber ...)`/`(return)` insns,
     or `(label_ref L<k>)` targets, and never coalesce or reorder basic blocks.
9. **Self-Contained Subfunction Inverse Modules & Thin Pass Stitchers**:
   - Each `lifter/invert_<func>.{h,cc}` module must be completely self-contained
     for its GCC/GAS subfunction $f_{s,j}$: it must provide both any
     element-level inverse and the full stream/function-level transformation
     needed so its output matches the exact input domain of the upstream
     subfunction $f_{s,j-1}$.
   - **Pre-Image Fidelity When $f_{s,j}$ Is Many-to-One**: When designing
     $g_{s,j}(x_j)$, do not merely pick an arbitrary synthetic pre-image that
     satisfies $f_{s,j}(g_{s,j}(x_j)) = x_j$ in isolation—ensure $g_{s,j}(x_j)$
     produces a pre-image in the canonical structural form of $x_{j-1}$ (the
     image of the preceding forward subfunction $f_{s,j-1}$, preserving block
     layout, barrier placement, and sideband indexing) so upstream inverse
     modules $g_{s,j-1}$ do not encounter out-of-distribution CFG/RTL shapes
     when stitched together.
   - Never defer stream-level lifting, hazard-`nop` stripping, macro collapsing,
     epilogue extraction, or CFG reconstruction logic to a pass stitcher or hide
     it inside `gcc_harness/right_inverse_verifier.cc`.
   - Every pass stitcher (`lifter/pass00_...` through `lifter/pass13_...`) must
     be a thin composition (~30–50 lines) that simply chains its constituent
     `invert_<func>` functions in reverse compilation order with zero
     pass-internal lifting logic or duplicated helpers.
