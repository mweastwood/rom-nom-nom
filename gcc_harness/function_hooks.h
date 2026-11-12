#ifndef GCC_HARNESS_FUNCTION_HOOKS_H_
#define GCC_HARNESS_FUNCTION_HOOKS_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/types/span.h"
#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "gcc_harness/gcc_context.h"

namespace rom_nom_nom::gcc_harness {

// Identifies each of the 27 subfunctions (30 boundary transitions including
// multi-pass CSE/Sched/Jump instances) across KMC GCC 2.7.2 (`cc1`) and GNU
// Binutils 2.6 (`as`) whose exact boundary (`pre_state` -> `post_state`) is
// captured during forward compilation tracing.
enum class SubfunctionId {
  // Stage 13 (`function.c:instantiate_virtual_regs`, `.00.rtl` virtual-reg instantiation):
  kInstantiateVirtualRegs,

  // Stage 12 (`jump1`, `.00.rtl` -> `.01.jump`):
  kJumpRedirect,
  kJump1Optimize,

  // Stage 11 (`cse1`, `.01.jump` -> `.02.cse`):
  kFoldRtx,
  kCseBasicBlockCse1,

  // Stage 10 (`loop`, `.02.cse` -> `.03.loop`):
  kScanLoop,
  kStrengthReduce,

  // Stage 9 (`cse2`, `.03.loop` -> `.04.cse2`):
  kCseBasicBlockCse2,

  // Stage 8 (`flow`, `.04.cse2` -> `.05.flow`):
  kFindBasicBlocks,
  kPropagateBlock,

  // Stage 7 (`combine`, `.05.flow` -> `.06.combine`):
  kSimplifyRtx,
  kSubst,
  kTryCombine,

  // Stage 6 (`sched1`, `.06.combine` -> `.07.sched`):
  kScheduleBlockSched1,

  // Stage 5 (`local_alloc`, `.07.sched` -> `.08.lreg`):
  kUpdateEquivRegs,
  kBlockAlloc,

  // Stage 4 (`global_alloc` + `reload`, `.08.lreg` -> `.09.greg`):
  kGlobalAlloc,
  kReload,
  kAlterReg,

  // Stage 3 (`sched2`, `.09.greg` -> `.10.sched2`):
  kScheduleBlockSched2,

  // Stage 2 (`jump2`, `.10.sched2` -> `.11.jump2`):
  kDoCrossJump,

  // Stage 1 (`dbr_schedule`, `.11.jump2` -> `.12.dbr`):
  kFillSimpleDelaySlots,
  kFillEagerDelaySlots,
  kRelaxDelaySlots,
  kMakeReturnInsns,

  // Stage 0 (`final` + `GAS`, `.12.dbr` -> `.s` -> `.o`):
  kFunctionPrologueEpilogue,
  kFinalScanInsn,
  kGasMipsIp,
  kGasMacro,
  kGasAppendInsn,
};

// Returns a human-readable name for `id`.
std::string_view SubfunctionIdName(SubfunctionId id);

// Returns the ordered sequence of all 30 `SubfunctionId`s in forward
// compilation order (`kInstantiateVirtualRegs` through `kGasAppendInsn`).
absl::Span<const SubfunctionId> AllSubfunctionIds();

// Captures the exact input (`pre_state`) and output (`post_state`) of a single
// GCC 2.7.2 / Binutils 2.6 GAS subfunction boundary during in-process forward
// compilation.
struct SubfunctionBoundarySnapshot {
  SubfunctionId subfunction_id = SubfunctionId::kJump1Optimize;
  int optimize_level = 2;
  std::vector<std::string> c_flags;
  absl::flat_hash_map<std::string, uint32_t> gp_symbol_sizes;
  RtlControlFlowGraph pre_state;
  RtlControlFlowGraph post_state;

  // Populated for Stage 0 (`final` / `GAS`) boundaries:
  std::vector<std::string> pre_asm_lines;
  std::vector<std::string> post_asm_lines;
  std::vector<uint32_t> post_machine_words;
};

// Complete in-process forward compilation trace for a single C function from
// `.00.rtl` through `.12.dbr`, `.s`, and 32-bit big-endian MIPS machine words.
struct FunctionCompilationTrace {
  std::string test_name;
  std::string function_name;
  int optimize_level = 2;
  std::vector<std::string> c_flags;
  absl::flat_hash_map<std::string, uint32_t> gp_symbol_sizes;

  // Pass-level canonical CFGs (`.00.rtl` through `.12.dbr`), each carrying its
  // populated `RtlSidebandState`.
  absl::flat_hash_map<RtlPassKind, RtlControlFlowGraph> pass_snapshots;

  // Ordered sequence of subfunction boundary snapshots (`pre_state` and
  // `post_state`) recorded across the pipeline.
  std::vector<SubfunctionBoundarySnapshot> subfunction_snapshots;

  // Stage 0 (`final` + `GAS`) outputs:
  std::string final_asm_text;
  std::vector<std::string> final_insn_asm_lines;
  std::vector<uint32_t> final_machine_words;
  Stage0FunctionInput stage0_input;
};

struct CrossJumpSearchResult {
  Rtx e1 = nullptr;
  Rtx e2 = nullptr;
};

// Direct in-process C++ hooks for KMC GCC 2.7.2 (`cc1`) and GNU Binutils 2.6
// (`as`) passes and internal static pass helper functions $f$.
class FunctionHooks {
 public:
  explicit FunctionHooks(GccContext* ctx);

  // ---------------------------------------------------------------------------
  // Stage 0: Final Assembly Emission (`final.c`, `mips.c`) & GAS (`tc-mips.c`)
  // ---------------------------------------------------------------------------
  std::string RunFinal(Rtx first = nullptr, bool include_prologue_epilogue = true);
  std::string RunFinalScanInsn(Rtx insn, int prescan = 0, Rtx* next_insn_out = nullptr);
  void RunShortenBranches(Rtx first = nullptr);
  std::string RunFunctionPrologue(int frame_size = 0, uint32_t saved_gpr_mask = 0,
                                  uint32_t saved_fpr_mask = 0);
  std::string RunFunctionEpilogue(int frame_size = 0, uint32_t saved_gpr_mask = 0,
                                  uint32_t saved_fpr_mask = 0);
  std::string RunFunctionPrologueEpilogue(int frame_size = 0, uint32_t saved_gpr_mask = 0,
                                          uint32_t saved_fpr_mask = 0);

  // Applies a module-level `CompilationProfile` (`optimize_level` and `c_flags`,
  // including `-Wa,-g` / `-Wa,-O2` GAS options and `-G` threshold) to in-process
  // GCC 2.7.2 and Binutils 2.6 globals.
  void ApplyCompilationProfile(const CompilationProfile& profile);

  // Checks that every materialized `INSN`/`JUMP_INSN`/`CALL_INSN` in `ctx_` is
  // recognizable by GCC 2.7.2 `recog` under the active compilation profile
  // (preventing `fatal_insn` `abort()`s on invalid/adversarial lifted RTL).
  bool ValidateMaterializedInsnsRecognizable(std::string* out_bad_insn = nullptr) const;

  // Assembles one or more MIPS assembly lines in-process via Binutils 2.6 GAS
  // (`md_assemble`) and returns the emitted 32-bit big-endian machine words.
  std::vector<uint32_t> RunGasMdAssemble(std::string_view asm_text);

  // Assembles MIPS assembly text in-process via Binutils 2.6 GAS (`md_assemble`)
  // and captures `.text` machine words, `fixS` relocations, `.rdata`/`.rodata`
  // switch jump tables, and constant-pool entries (`$LC*` and `li.s`/`li.d`
  // literals).
  Stage0FunctionInput RunGasMdAssembleWithMetadata(std::string_view asm_text);

  // Parses a single MIPS instruction in-process via GAS `mips_ip` (`tc-mips.c`)
  // and returns the raw 32-bit opcode word (`insn.insn_opcode`).
  uint32_t RunGasMipsIp(std::string_view insn_asm_line);

  // Expands a MIPS macro pseudo-instruction (e.g. `li`, `la`, `move`, `subu`)
  // in-process via GAS `mips_ip` + `macro` and returns the emitted 32-bit
  // big-endian machine words.
  std::vector<uint32_t> RunGasMacro(std::string_view macro_asm_line);

  // Encodes a single native MIPS instruction in-process via GAS `mips_ip` +
  // `append_insn` (`.set noreorder`) and returns the 32-bit machine word.
  uint32_t RunGasAppendInsn(std::string_view insn_asm_line);

  // Encodes a sequence of native MIPS instructions in-process via GAS `mips_ip`
  // + `append_insn` in `.set reorder` mode (`mips_noreorder = 0`), exercising
  // `append_insn`'s delay-slot NOP insertion and instruction-swapping logic.
  std::vector<uint32_t> RunGasAppendInsnReorder(absl::Span<const std::string> insn_asm_lines);

  // ---------------------------------------------------------------------------
  // Stage 1: Delayed-Branch Scheduling (`reorg.c`)
  // ---------------------------------------------------------------------------
  void RunDbrSchedule(Rtx first = nullptr);
  void RunFillSimpleDelaySlots(int non_jumps_p, Rtx first = nullptr);
  void RunFillEagerDelaySlots(Rtx first = nullptr);
  Rtx RunFillSlotsFromThread(Rtx insn, Rtx condition, Rtx thread, Rtx opposite_thread, int likely,
                             int thread_if_true, int own_thread, int own_opposite_thread,
                             int slots_to_fill, int* pslots_filled);
  void RunRelaxDelaySlots(Rtx first = nullptr);
  void RunMakeReturnInsns(Rtx first = nullptr);
  Rtx RunFindEndLabel(Rtx first = nullptr);

  // ---------------------------------------------------------------------------
  // Stage 2 & Stage 12: Jump Optimization & Cross-Jumping (`jump.c`)
  // ---------------------------------------------------------------------------
  void RunJumpOptimize(int cross_jump, int noop_moves, int after_regscan, Rtx first = nullptr);
  CrossJumpSearchResult RunFindCrossJump(Rtx e1, Rtx e2, int minimum);
  void RunDoCrossJump(Rtx insn, Rtx newjpos, Rtx newlpos);
  int RunRedirectJump(Rtx jump, Rtx nlabel);
  int RunInvertJump(Rtx jump, Rtx nlabel);

  // ---------------------------------------------------------------------------
  // Stage 3 & Stage 6: Instruction Scheduling (`sched.c`)
  // ---------------------------------------------------------------------------
  void RunScheduleInsns(Rtx first = nullptr);
  void RunScheduleBlock(int bb_index, Rtx first = nullptr);

  // ---------------------------------------------------------------------------
  // Stage 4 & Stage 5: Register Allocation & Reload (`local-alloc.c`,
  // `global.c`, `reload1.c`)
  // ---------------------------------------------------------------------------
  void RunLocalAlloc(Rtx first = nullptr);
  void RunBlockAlloc(int bb_index, Rtx first = nullptr);
  void RunUpdateEquivRegs(Rtx first = nullptr);
  int RunGlobalAlloc(Rtx first = nullptr);
  int RunReload(Rtx first = nullptr, int global = 1);
  void RunAlterReg(int regno, int from_reg);

  // ---------------------------------------------------------------------------
  // Stage 7: Instruction Combination (`combine.c`)
  // ---------------------------------------------------------------------------
  void RunCombineInstructions(Rtx first = nullptr, int nregs = 0);
  Rtx RunTryCombine(Rtx i3, Rtx i2, Rtx i1 = nullptr);
  Rtx RunSubst(Rtx x, Rtx from, Rtx to, int in_dest = 0, int unique_copy = 0);
  Rtx RunSimplifyRtx(Rtx x, int op0_mode = kVoidMode, int last = 0, int in_dest = 0);

  // ---------------------------------------------------------------------------
  // Stage 8: Flow & Life Analysis (`flow.c`)
  // ---------------------------------------------------------------------------
  void RunFlowAnalysis(Rtx first = nullptr, int nregs = 0);
  void RunFindBasicBlocks(Rtx first = nullptr, Rtx nonlocal_label_list = nullptr);
  void RunLifeAnalysis(Rtx first = nullptr, int nregs = 0);
  void RunPropagateBlock(Rtx old_live, Rtx first_insn, Rtx last_insn, int final_pass,
                         Rtx significant, int bnum);

  // ---------------------------------------------------------------------------
  // Stage 9 & Stage 11: Common Subexpression Elimination (`cse.c`)
  // ---------------------------------------------------------------------------
  int RunCseMain(Rtx first = nullptr, int nregs = 0, int after_loop = 0);
  Rtx RunCseBasicBlock(Rtx from, Rtx to, void* next_branch, int around_loop);
  Rtx RunFoldRtx(Rtx x, Rtx insn = nullptr);

  // ---------------------------------------------------------------------------
  // Stage 10: Loop Optimization (`loop.c`)
  // ---------------------------------------------------------------------------
  void RunLoopOptimize(Rtx first = nullptr, int unroll_p = 0);
  void RunScanLoop(Rtx loop_start = nullptr, Rtx end = nullptr, int nregs = 0);
  void RunStrengthReduce(Rtx scan_start = nullptr, Rtx end = nullptr, Rtx loop_top = nullptr,
                         int insn_count = 0, Rtx loop_start = nullptr, Rtx loop_end = nullptr);

  // ---------------------------------------------------------------------------
  // Stage 13: Tree -> RTL Expansion (`function.c`, `stmt.c`, `expr.c`, `calls.c`)
  // ---------------------------------------------------------------------------
  RtlControlFlowGraph RunInstantiateVirtualRegs(GccContext& ctx,
                                                const RtlControlFlowGraph& pre_cfg);
  RtlControlFlowGraph RunInstantiateVirtualRegs(const RtlControlFlowGraph& pre_cfg);

  RtlControlFlowGraph RunExpandFunctionEnd(GccContext& ctx, const RtlControlFlowGraph& body_cfg,
                                           const FunctionDeclaration& func_decl);
  RtlControlFlowGraph RunExpandFunctionEnd(const RtlControlFlowGraph& body_cfg,
                                           const FunctionDeclaration& func_decl);
  RtlControlFlowGraph RunExpandFunctionEnd(GccContext& ctx, const RtlControlFlowGraph& body_cfg);
  RtlControlFlowGraph RunExpandFunctionEnd(const RtlControlFlowGraph& body_cfg);

  RtlControlFlowGraph RunExpandReturn(GccContext& ctx, const ReturnStatement& ret_stmt,
                                      const FunctionDeclaration& func_decl);
  RtlControlFlowGraph RunExpandReturn(const ReturnStatement& ret_stmt,
                                      const FunctionDeclaration& func_decl);

  RtlControlFlowGraph RunExpandExpr(GccContext& ctx, const CExpression& expr,
                                    const FunctionDeclaration& func_decl,
                                    int target_pseudo_regno = 0);
  RtlControlFlowGraph RunExpandExpr(const CExpression& expr, const FunctionDeclaration& func_decl,
                                    int target_pseudo_regno = 0);

  RtlControlFlowGraph RunExpandAssignment(GccContext& ctx, const CExpression& lhs,
                                          const CExpression& rhs,
                                          const FunctionDeclaration& func_decl);
  RtlControlFlowGraph RunExpandAssignment(const CExpression& lhs, const CExpression& rhs,
                                          const FunctionDeclaration& func_decl);

  RtlControlFlowGraph RunDoStoreFlag(GccContext& ctx, const CExpression& comparison_expr,
                                     const FunctionDeclaration& func_decl,
                                     int target_pseudo_regno = 0, int only_cheap = 0);
  RtlControlFlowGraph RunDoStoreFlag(const CExpression& comparison_expr,
                                     const FunctionDeclaration& func_decl,
                                     int target_pseudo_regno = 0, int only_cheap = 0);

  RtlControlFlowGraph RunDoJump(GccContext& ctx, const CExpression& cond_expr,
                                const FunctionDeclaration& func_decl, bool jump_if_true = false,
                                bool jump_if_false = true);
  RtlControlFlowGraph RunDoJump(const CExpression& cond_expr, const FunctionDeclaration& func_decl,
                                bool jump_if_true = false, bool jump_if_false = true);

  RtlControlFlowGraph RunExpandCall(GccContext& ctx, const CallExpression& call_expr,
                                    const FunctionDeclaration& caller_decl,
                                    const CType& return_type = CType::S32(),
                                    int target_pseudo_regno = 0, bool ignore_result = false);
  RtlControlFlowGraph RunExpandCall(const CallExpression& call_expr,
                                    const FunctionDeclaration& caller_decl,
                                    const CType& return_type = CType::S32(),
                                    int target_pseudo_regno = 0, bool ignore_result = false);

  RtlControlFlowGraph RunExpandCond(GccContext& ctx, const IfStatement& if_stmt,
                                    const FunctionDeclaration& func_decl);
  RtlControlFlowGraph RunExpandCond(const IfStatement& if_stmt,
                                    const FunctionDeclaration& func_decl);

  RtlControlFlowGraph RunExpandLoop(GccContext& ctx, const CStatement& loop_stmt,
                                    const FunctionDeclaration& func_decl);
  RtlControlFlowGraph RunExpandLoop(const CStatement& loop_stmt,
                                    const FunctionDeclaration& func_decl);

  RtlControlFlowGraph RunExpandCase(GccContext& ctx, const SwitchStatement& switch_stmt,
                                    const FunctionDeclaration& func_decl);
  RtlControlFlowGraph RunExpandCase(const SwitchStatement& switch_stmt,
                                    const FunctionDeclaration& func_decl);

  RtlControlFlowGraph RunExpandDecl(GccContext& ctx, const VariableDeclarationStatement& var_decl,
                                    const FunctionDeclaration& func_decl);
  RtlControlFlowGraph RunExpandDecl(const VariableDeclarationStatement& var_decl,
                                    const FunctionDeclaration& func_decl);

  RtlControlFlowGraph RunExpandFunctionStart(GccContext& ctx, const FunctionDeclaration& func_decl);
  RtlControlFlowGraph RunExpandFunctionStart(const FunctionDeclaration& func_decl);

  // ---------------------------------------------------------------------------
  // In-Process Forward Compilation Tracer (`TraceRtlPipeline` &
  // `TraceForwardCompilation`)
  // ---------------------------------------------------------------------------

  // Compiles `c_source` for `function_name` in-process using the KMC GCC 2.7.2
  // C frontend (`yyparse`) and returns the initial `.00.rtl` canonical CFG.
  RtlControlFlowGraph CompileCSourceToInitialRtlCfg(std::string_view c_source,
                                                    std::string_view function_name,
                                                    absl::Span<const std::string> c_flags = {
                                                        "-O2"});

  // Materializes `pre_state` into `ctx_`, executes the forward hook for
  // `subfunction_id`, and returns the resulting `SubfunctionBoundarySnapshot`
  // (`pre_state`, `post_state`, and Stage 0 assembly / machine words).
  SubfunctionBoundarySnapshot ExecuteSubfunctionBoundary(
      SubfunctionId subfunction_id, const RtlControlFlowGraph& pre_state,
      absl::Span<const std::string> pre_asm_lines = {}, int optimize_level = 2,
      absl::Span<const std::string> c_flags = {},
      const absl::flat_hash_map<std::string, uint32_t>& gp_symbol_sizes = {});

  // Convenience overload re-executing the subfunction boundary described by
  // `snap_template` (including its `subfunction_id`, `optimize_level`, and
  // `c_flags`) on a candidate `candidate_pre_state` (and optional
  // `candidate_pre_asm_lines`).
  SubfunctionBoundarySnapshot ExecuteSubfunctionBoundary(
      const SubfunctionBoundarySnapshot& snap_template,
      const RtlControlFlowGraph& candidate_pre_state,
      absl::Span<const std::string> candidate_pre_asm_lines = {});

  // Steps 100% in-process from `rtl_00_cfg` (`.00.rtl`) through every GCC 2.7.2
  // RTL pass (`.01.jump` .. `.12.dbr`), `final`, and Binutils 2.6 GAS,
  // capturing all 13 `pass_snapshots` and all 30 `SubfunctionBoundarySnapshot`s.
  FunctionCompilationTrace TraceRtlPipeline(
      const RtlControlFlowGraph& rtl_00_cfg, int optimize_level = 2,
      std::string_view function_name = "harness_func", std::string_view test_name = "",
      absl::Span<const std::string> c_flags = {},
      const absl::flat_hash_map<std::string, uint32_t>& gp_symbol_sizes = {});

  // Compiles `c_source` for `function_name` 100% in-process and traces its
  // forward execution across all 13 RTL passes, 26 subfunctions, `final`, and
  // GAS.
  FunctionCompilationTrace TraceForwardCompilation(std::string_view c_source,
                                                   std::string_view function_name,
                                                   absl::Span<const std::string> c_flags = {"-O2"},
                                                   std::string_view test_name = "");

  // Convenience overload tracing a `RoundTripTestCase` in-process.
  FunctionCompilationTrace TraceForwardCompilation(const RoundTripTestCase& test_case);

 private:
  void PrepareReorgState(Rtx first);
  absl::flat_hash_map<int, absl::flat_hash_set<int>> CapturePostReorgLiveAtStart(Rtx first);
  void PrepareSchedState(Rtx first);
  void PrepareLocalAllocState(Rtx first);
  void PrepareCombineState(Rtx first);
  void PrepareLoopState(Rtx first);
  void ExecuteSchedPassGranular(Rtx first);
  void ExecuteCombinePassGranular(Rtx first, int nregs);

  GccContext* ctx_;
  bool in_sched_pass_ = false;
  bool in_local_alloc_pass_ = false;
  bool in_combine_pass_ = false;
  bool in_loop_pass_ = false;
  absl::flat_hash_map<int, absl::flat_hash_set<int>> last_reorg_post_live_;
};

}  // namespace rom_nom_nom::gcc_harness

#endif  // GCC_HARNESS_FUNCTION_HOOKS_H_
