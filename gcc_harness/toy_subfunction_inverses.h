#ifndef GCC_HARNESS_TOY_SUBFUNCTION_INVERSES_H_
#define GCC_HARNESS_TOY_SUBFUNCTION_INVERSES_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "absl/types/span.h"
#include "core/rtl_cfg.h"
#include "gcc_harness/subfunction_inverse_provider.h"

namespace rom_nom_nom::gcc_harness {

// Miniature synthetic right-inverse fixture implementing `SubfunctionInverseProvider`
// for the 26 individual KMC GCC 2.7.2 (`cc1`) and GNU Binutils 2.6 (`as`) C
// functions $f_i$ exposed in `FunctionHooks`.
//
// IMPORTANT: `ToySubfunctionInverses` exists **strictly as a miniature
// synthetic fixture for self-testing `gcc_harness` mechanics** (`GccContext`,
// `FunctionHooks`, and `RightInverseVerifier` round-trip execution) on small
// unit-test inputs. It is NOT used for production lifting; production inverse
// stages live in `lifter/` and plug into `RightInverseVerifier` via the
// `SubfunctionInverseProvider` interface.
class ToySubfunctionInverses : public SubfunctionInverseProvider {
 public:
  ToySubfunctionInverses() = default;
  ~ToySubfunctionInverses() override = default;

  // ---------------------------------------------------------------------------
  // Stage 0: Binutils 2.6 GAS (`tc-mips.c`) & GCC 2.7.2 Final (`final.c`, `mips.c`)
  // ---------------------------------------------------------------------------

  std::string InvertGasMipsIp(uint32_t word_x) const override;
  std::string InvertGasMacro(absl::Span<const uint32_t> macro_words_x) const override;
  std::vector<std::string> InvertGasAppendInsn(absl::Span<const uint32_t> words_x) const override;
  std::string InvertFinalScanInsn(std::string_view asm_line_x) const override;
  PrologueEpilogueFrameSpec InvertFunctionPrologueEpilogue(
      std::string_view asm_text_x) const override;
  RtlControlFlowGraph InvertFinalAndGasFunction(
      absl::Span<const uint32_t> machine_words_x) const override;

  // ---------------------------------------------------------------------------
  // Stage 1: Delayed-Branch Scheduling (`reorg.c`)
  // ---------------------------------------------------------------------------

  RtlControlFlowGraph InvertMakeReturnInsns(const RtlControlFlowGraph& x) const override;
  RtlControlFlowGraph InvertRelaxDelaySlots(const RtlControlFlowGraph& x) const override;
  RtlControlFlowGraph InvertFillEagerDelaySlots(const RtlControlFlowGraph& x) const override;
  RtlControlFlowGraph InvertFillSimpleDelaySlots(const RtlControlFlowGraph& x) const override;
  RtlControlFlowGraph InvertDbrSchedule(const RtlControlFlowGraph& x) const override;

  // ---------------------------------------------------------------------------
  // Stage 2: Post-Reload Cross-Jumping (`jump.c`)
  // ---------------------------------------------------------------------------

  RtlControlFlowGraph InvertDoCrossJump(const RtlControlFlowGraph& x) const override;

  // ---------------------------------------------------------------------------
  // Stage 3 & Stage 6: Instruction Scheduling (`sched.c`)
  // ---------------------------------------------------------------------------

  RtlControlFlowGraph InvertScheduleBlock(const RtlControlFlowGraph& x) const override;

  // ---------------------------------------------------------------------------
  // Stage 4 & Stage 5: Register Allocation & Reload (`reload1.c`, `local-alloc.c`, `global.c`)
  // ---------------------------------------------------------------------------

  RtlControlFlowGraph InvertAlterReg(const RtlControlFlowGraph& x, int pseudo_regno) const override;
  using SubfunctionInverseProvider::InvertAlterReg;
  RtlControlFlowGraph InvertBlockAlloc(const RtlControlFlowGraph& x) const override;
  RtlControlFlowGraph InvertUpdateEquivRegs(const RtlControlFlowGraph& x) const override;

  // ---------------------------------------------------------------------------
  // Stage 7: Instruction Combination (`combine.c`)
  // ---------------------------------------------------------------------------

  std::string InvertSimplifyRtx(std::string_view simplified_rtx_x) const override;
  SubstInverseSpec InvertSubst(std::string_view subst_rtx_x) const override;
  RtlControlFlowGraph InvertTryCombine(const RtlControlFlowGraph& x) const override;

  // ---------------------------------------------------------------------------
  // Stage 8: Flow & Life Analysis (`flow.c`)
  // ---------------------------------------------------------------------------

  RtlControlFlowGraph InvertFindBasicBlocks(const RtlControlFlowGraph& x) const override;
  RtlControlFlowGraph InvertPropagateBlock(const RtlControlFlowGraph& x) const override;

  // ---------------------------------------------------------------------------
  // Stage 9 & Stage 11: Common Subexpression Elimination (`cse.c`)
  // ---------------------------------------------------------------------------

  std::string InvertFoldRtx(std::string_view folded_rtx_x) const override;
  RtlControlFlowGraph InvertCseBasicBlock(const RtlControlFlowGraph& x) const override;

  // ---------------------------------------------------------------------------
  // Stage 10: Loop Optimization (`loop.c`)
  // ---------------------------------------------------------------------------

  RtlControlFlowGraph InvertScanLoop(const RtlControlFlowGraph& x) const override;
  RtlControlFlowGraph InvertStrengthReduce(const RtlControlFlowGraph& x) const override;

  // ---------------------------------------------------------------------------
  // Stage 12: Initial Jump Optimization (`jump.c`)
  // ---------------------------------------------------------------------------

  RtlControlFlowGraph InvertJumpRedirect(const RtlControlFlowGraph& x) const override;
  RtlControlFlowGraph InvertJump1Optimize(const RtlControlFlowGraph& x) const override;

  // ---------------------------------------------------------------------------
  // Stage 13: Tree -> RTL Expansion (`function.c`, `stmt.c`, `expr.c`, `calls.c`)
  // ---------------------------------------------------------------------------

  RtlControlFlowGraph InvertInstantiateVirtualRegs(const RtlControlFlowGraph& x) const override;
  absl::StatusOr<ExpandFunctionEndInput> InvertExpandFunctionEnd(
      const RtlControlFlowGraph& x) const override;
  absl::StatusOr<ExpandReturnInput> InvertExpandReturn(const RtlControlFlowGraph& x) const override;
  absl::StatusOr<ExpandExprInput> InvertExpandExpr(const RtlControlFlowGraph& x) const override;
  absl::StatusOr<ExpandAssignmentInput> InvertExpandAssignment(
      const RtlControlFlowGraph& x) const override;
  absl::StatusOr<DoStoreFlagInput> InvertDoStoreFlag(const RtlControlFlowGraph& x) const override;
  absl::StatusOr<DoJumpInput> InvertDoJump(const RtlControlFlowGraph& x) const override;
  absl::StatusOr<ExpandCallInput> InvertExpandCall(const RtlControlFlowGraph& x) const override;
  absl::StatusOr<ExpandCondInput> InvertExpandCond(const RtlControlFlowGraph& x) const override;
  absl::StatusOr<ExpandLoopInput> InvertExpandLoop(const RtlControlFlowGraph& x) const override;
  absl::StatusOr<ExpandCaseInput> InvertExpandCase(const RtlControlFlowGraph& x) const override;
  absl::StatusOr<ExpandDeclInput> InvertExpandDecl(const RtlControlFlowGraph& x) const override;
  absl::StatusOr<FunctionDeclaration> InvertExpandFunctionStart(
      const RtlControlFlowGraph& x) const override;
};

}  // namespace rom_nom_nom::gcc_harness

#endif  // GCC_HARNESS_TOY_SUBFUNCTION_INVERSES_H_
