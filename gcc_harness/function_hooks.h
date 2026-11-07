#ifndef GCC_HARNESS_FUNCTION_HOOKS_H_
#define GCC_HARNESS_FUNCTION_HOOKS_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "absl/types/span.h"
#include "gcc_harness/gcc_context.h"

namespace rom_nom_nom::gcc_harness {

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
  std::string RunFinal(Rtx first = nullptr);
  std::string RunFinalScanInsn(Rtx insn, int prescan = 0);
  void RunShortenBranches(Rtx first = nullptr);
  std::string RunFunctionPrologue(int frame_size = 0, uint32_t saved_gpr_mask = 0);
  std::string RunFunctionEpilogue(int frame_size = 0, uint32_t saved_gpr_mask = 0);
  std::string RunFunctionPrologueEpilogue(int frame_size = 0, uint32_t saved_gpr_mask = 0);

  // Assembles one or more MIPS assembly lines in-process via Binutils 2.6 GAS
  // (`md_assemble`) and returns the emitted 32-bit big-endian machine words.
  std::vector<uint32_t> RunGasMdAssemble(std::string_view asm_text);

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

 private:
  void PrepareReorgState(Rtx first);
  void PrepareSchedState(Rtx first);
  void PrepareLocalAllocState(Rtx first);
  void PrepareCombineState(Rtx first);
  void PrepareLoopState(Rtx first);

  GccContext* ctx_;
};

}  // namespace rom_nom_nom::gcc_harness

#endif  // GCC_HARNESS_FUNCTION_HOOKS_H_
