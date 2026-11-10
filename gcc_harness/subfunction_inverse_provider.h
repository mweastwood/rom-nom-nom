#ifndef GCC_HARNESS_SUBFUNCTION_INVERSE_PROVIDER_H_
#define GCC_HARNESS_SUBFUNCTION_INVERSE_PROVIDER_H_

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "core/c_ast.h"
#include "core/rtl_cfg.h"

namespace rom_nom_nom::gcc_harness {

using PrologueEpilogueFrameSpec = ::rom_nom_nom::PrologueEpilogueFrameSpec;
using SubstInverseSpec = ::rom_nom_nom::SubstInverseSpec;
using SubstPreImage = ::rom_nom_nom::SubstPreImage;

// Input bundle recovered by `InvertExpandFunctionEnd` for
// `function.c:expand_function_end`.
struct ExpandFunctionEndInput {
  RtlControlFlowGraph body_cfg;
  FunctionDeclaration func_decl =
      FunctionDeclaration(CType::S32(), "harness_func", {}, std::make_unique<CompoundStatement>());
};

// Input bundle recovered by `InvertExpandReturn` for `stmt.c:expand_return`.
struct ExpandReturnInput {
  ReturnStatement ret_stmt;
  FunctionDeclaration func_decl =
      FunctionDeclaration(CType::S32(), "harness_func", {}, std::make_unique<CompoundStatement>());
};

// Input bundle recovered by `InvertExpandExpr` for `expr.c:expand_expr`.
struct ExpandExprInput {
  std::unique_ptr<CExpression> expr;
  FunctionDeclaration func_decl =
      FunctionDeclaration(CType::S32(), "harness_func", {}, std::make_unique<CompoundStatement>());
  int target_pseudo_regno = 0;
};

// Input bundle recovered by `InvertExpandAssignment` for
// `expr.c:expand_assignment`.
struct ExpandAssignmentInput {
  std::unique_ptr<CExpression> lhs;
  std::unique_ptr<CExpression> rhs;
  FunctionDeclaration func_decl =
      FunctionDeclaration(CType::S32(), "harness_func", {}, std::make_unique<CompoundStatement>());
};

// Input bundle recovered by `InvertDoStoreFlag` for `expr.c:do_store_flag`.
struct DoStoreFlagInput {
  std::unique_ptr<CExpression> comparison_expr;
  FunctionDeclaration func_decl =
      FunctionDeclaration(CType::S32(), "harness_func", {}, std::make_unique<CompoundStatement>());
  int target_pseudo_regno = 0;
  int only_cheap = 0;
};

// Input bundle recovered by `InvertDoJump` for `expr.c:do_jump`.
struct DoJumpInput {
  std::unique_ptr<CExpression> cond_expr;
  FunctionDeclaration func_decl =
      FunctionDeclaration(CType::S32(), "harness_func", {}, std::make_unique<CompoundStatement>());
  bool jump_if_true = false;
  bool jump_if_false = true;
};

// Input bundle recovered by `InvertExpandCall` for `calls.c:expand_call`.
struct ExpandCallInput {
  CallExpression call_expr = CallExpression(CExpression::Identifier("callee"), {});
  FunctionDeclaration caller_decl =
      FunctionDeclaration(CType::S32(), "harness_func", {}, std::make_unique<CompoundStatement>());
  CType return_type = CType::S32();
  int target_pseudo_regno = 0;
  bool ignore_result = false;
};

// Input bundle recovered by `InvertExpandCond` for
// `stmt.c:expand_start_cond` / `expand_start_else` / `expand_end_cond`.
struct ExpandCondInput {
  IfStatement if_stmt = IfStatement(CExpression::Integer(1), std::make_unique<CompoundStatement>());
  FunctionDeclaration func_decl =
      FunctionDeclaration(CType::S32(), "harness_func", {}, std::make_unique<CompoundStatement>());
};

// Input bundle recovered by `InvertExpandLoop` for
// `stmt.c:expand_start_loop` / `expand_exit_loop_if_false` / `expand_end_loop`.
struct ExpandLoopInput {
  std::unique_ptr<CStatement> loop_stmt;
  FunctionDeclaration func_decl =
      FunctionDeclaration(CType::S32(), "harness_func", {}, std::make_unique<CompoundStatement>());
};

// Input bundle recovered by `InvertExpandCase` for
// `stmt.c:expand_start_case` / `pushcase` / `expand_end_case`.
struct ExpandCaseInput {
  SwitchStatement switch_stmt = SwitchStatement(CExpression::Integer(0), {});
  FunctionDeclaration func_decl =
      FunctionDeclaration(CType::S32(), "harness_func", {}, std::make_unique<CompoundStatement>());
};

// Input bundle recovered by `InvertExpandDecl` for `stmt.c:expand_decl`.
struct ExpandDeclInput {
  VariableDeclarationStatement var_decl =
      VariableDeclarationStatement(CType::S32(), "local_var", nullptr);
  FunctionDeclaration func_decl =
      FunctionDeclaration(CType::S32(), "harness_func", {}, std::make_unique<CompoundStatement>());
};

// Abstract interface providing 1-to-1 mathematical right-inverses $g_i$ for
// each of the 26 individual KMC GCC 2.7.2 (`cc1`) and GNU Binutils 2.6 (`as`)
// C functions $f_i$ exposed in `FunctionHooks`.
//
// `RightInverseVerifier` depends only on `SubfunctionInverseProvider`, allowing
// both the miniature synthetic self-test fixture (`ToySubfunctionInverses`) and
// the production `lifter/` stage/subfunction inverses to be verified directly
// against the genuine in-process GCC 2.7.2 and Binutils 2.6 functions.
class SubfunctionInverseProvider {
 public:
  virtual ~SubfunctionInverseProvider() = default;

  // ---------------------------------------------------------------------------
  // Stage 0: Binutils 2.6 GAS (`tc-mips.c`) & GCC 2.7.2 Final (`final.c`, `mips.c`)
  // ---------------------------------------------------------------------------

  // 1. Inverse of `tc-mips.c:mips_ip`: decodes a 32-bit big-endian MIPS
  // instruction word `word_x` into its canonical GAS assembly line `y`.
  virtual std::string InvertGasMipsIp(uint32_t /*word_x*/) const { return {}; }

  // 2. Inverse of `tc-mips.c:macro`: collapses a multi-word GAS macro expansion
  // `macro_words_x` (e.g. `lui` + `ori` / `addiu`) back into its single macro
  // assembly line `y` (e.g. `li $2,0x80012345`).
  virtual std::string InvertGasMacro(absl::Span<const uint32_t> /*macro_words_x*/) const {
    return {};
  }

  // 3. Inverse of `tc-mips.c:append_insn` (in `.set reorder` mode): recovers
  // the pre-`append_insn` sequence of assembly lines `y` from the emitted
  // machine words `words_x`.
  virtual std::vector<std::string> InvertGasAppendInsn(
      absl::Span<const uint32_t> /*words_x*/) const {
    return {};
  }

  // 4. Inverse of `final.c:final_scan_insn`: lifts a single MIPS assembly
  // instruction emitted by `final_scan_insn` back into its RTL pattern `y`.
  virtual std::string InvertFinalScanInsn(std::string_view /*asm_line_x*/) const { return {}; }

  // 5. Inverse of `mips.c:function_prologue` + `mips.c:function_epilogue`:
  // recovers `(var_size, saved_gpr_mask)` from emitted prologue/epilogue
  // assembly text `asm_text_x`.
  virtual PrologueEpilogueFrameSpec InvertFunctionPrologueEpilogue(
      std::string_view /*asm_text_x*/) const {
    return PrologueEpilogueFrameSpec{.var_size = -1, .saved_gpr_mask = 0};
  }

  // Whole-stage inverse for `final` + `md_assemble`: lifts a sequence of
  // 32-bit machine words `machine_words_x` into its `.dbr` `RtlControlFlowGraph`.
  virtual RtlControlFlowGraph InvertFinalAndGasFunction(
      absl::Span<const uint32_t> /*machine_words_x*/) const {
    return {};
  }

  // ---------------------------------------------------------------------------
  // Stage 1: Delayed-Branch Scheduling (`reorg.c`)
  // ---------------------------------------------------------------------------

  // 6. Inverse of `reorg.c:make_return_insns` + `find_end_label`.
  virtual RtlControlFlowGraph InvertMakeReturnInsns(const RtlControlFlowGraph& /*x*/) const {
    return {};
  }

  // 7. Inverse of `reorg.c:relax_delay_slots`.
  virtual RtlControlFlowGraph InvertRelaxDelaySlots(const RtlControlFlowGraph& /*x*/) const {
    return {};
  }

  // 8. Inverse of `reorg.c:fill_eager_delay_slots` + `fill_slots_from_thread`.
  virtual RtlControlFlowGraph InvertFillEagerDelaySlots(const RtlControlFlowGraph& /*x*/) const {
    return {};
  }

  // 9. Inverse of `reorg.c:fill_simple_delay_slots`.
  virtual RtlControlFlowGraph InvertFillSimpleDelaySlots(const RtlControlFlowGraph& /*x*/) const {
    return {};
  }

  // 10. Inverse of `reorg.c:dbr_schedule`.
  virtual RtlControlFlowGraph InvertDbrSchedule(const RtlControlFlowGraph& /*x*/) const {
    return {};
  }

  // ---------------------------------------------------------------------------
  // Stage 2: Post-Reload Cross-Jumping (`jump.c`)
  // ---------------------------------------------------------------------------

  // 11. Inverse of `jump.c:do_cross_jump` + `find_cross_jump`.
  virtual RtlControlFlowGraph InvertDoCrossJump(const RtlControlFlowGraph& /*x*/) const {
    return {};
  }

  // ---------------------------------------------------------------------------
  // Stage 3 & Stage 6: Instruction Scheduling (`sched.c`)
  // ---------------------------------------------------------------------------

  // 12. Inverse of `sched.c:schedule_block` / `schedule_insns`.
  virtual RtlControlFlowGraph InvertScheduleBlock(const RtlControlFlowGraph& /*x*/) const {
    return {};
  }

  // ---------------------------------------------------------------------------
  // Stage 4 & Stage 5: Register Allocation & Reload (`reload1.c`, `local-alloc.c`, `global.c`)
  // ---------------------------------------------------------------------------

  // 13a. Inverse of `reload1.c:alter_reg`.
  virtual RtlControlFlowGraph InvertAlterReg(const RtlControlFlowGraph& /*x*/,
                                             int /*pseudo_regno*/) const {
    return {};
  }
  RtlControlFlowGraph InvertAlterReg(const RtlControlFlowGraph& x) const {
    return InvertAlterReg(x, 72);
  }

  // 13b. Inverse of `reload1.c:reload`.
  virtual RtlControlFlowGraph InvertReload(const RtlControlFlowGraph& /*x*/) const { return {}; }

  // 13c. Inverse of `global.c:global_alloc`.
  virtual RtlControlFlowGraph InvertGlobalAlloc(const RtlControlFlowGraph& /*x*/) const {
    return {};
  }

  // 14. Inverse of `local-alloc.c:block_alloc`.
  virtual RtlControlFlowGraph InvertBlockAlloc(const RtlControlFlowGraph& /*x*/) const {
    return {};
  }

  // 15. Inverse of `local-alloc.c:update_equiv_regs`.
  virtual RtlControlFlowGraph InvertUpdateEquivRegs(const RtlControlFlowGraph& /*x*/) const {
    return {};
  }

  // ---------------------------------------------------------------------------
  // Stage 7: Instruction Combination (`combine.c`)
  // ---------------------------------------------------------------------------

  // 16. Inverse of `combine.c:simplify_rtx`.
  virtual std::string InvertSimplifyRtx(std::string_view /*simplified_rtx_x*/) const { return {}; }

  // 17. Inverse of `combine.c:subst`.
  virtual SubstInverseSpec InvertSubst(std::string_view /*subst_rtx_x*/) const { return {}; }

  // 18. Inverse of `combine.c:try_combine` / `combine_instructions`.
  virtual RtlControlFlowGraph InvertTryCombine(const RtlControlFlowGraph& /*x*/) const {
    return {};
  }

  // ---------------------------------------------------------------------------
  // Stage 8: Flow & Life Analysis (`flow.c`)
  // ---------------------------------------------------------------------------

  // 19. Inverse of `flow.c:find_basic_blocks`.
  virtual RtlControlFlowGraph InvertFindBasicBlocks(const RtlControlFlowGraph& /*x*/) const {
    return {};
  }

  // 20. Inverse of `flow.c:propagate_block` / `life_analysis` / `flow_analysis`.
  virtual RtlControlFlowGraph InvertPropagateBlock(const RtlControlFlowGraph& /*x*/) const {
    return {};
  }

  // ---------------------------------------------------------------------------
  // Stage 9 & Stage 11: Common Subexpression Elimination (`cse.c`)
  // ---------------------------------------------------------------------------

  // 21. Inverse of `cse.c:fold_rtx`.
  virtual std::string InvertFoldRtx(std::string_view /*folded_rtx_x*/) const { return {}; }

  // 22. Inverse of `cse.c:cse_basic_block` / `cse_main`.
  virtual RtlControlFlowGraph InvertCseBasicBlock(const RtlControlFlowGraph& /*x*/) const {
    return {};
  }

  // ---------------------------------------------------------------------------
  // Stage 10: Loop Optimization (`loop.c`)
  // ---------------------------------------------------------------------------

  // 23. Inverse of `loop.c:scan_loop`.
  virtual RtlControlFlowGraph InvertScanLoop(const RtlControlFlowGraph& /*x*/) const { return {}; }

  // 24. Inverse of `loop.c:strength_reduce`.
  virtual RtlControlFlowGraph InvertStrengthReduce(const RtlControlFlowGraph& /*x*/) const {
    return {};
  }

  // ---------------------------------------------------------------------------
  // Stage 12: Initial Jump Optimization (`jump.c`)
  // ---------------------------------------------------------------------------

  // 25. Inverse of `jump.c:redirect_jump` / `invert_jump`.
  virtual RtlControlFlowGraph InvertJumpRedirect(const RtlControlFlowGraph& /*x*/) const {
    return {};
  }

  // 26. Inverse of `jump.c:jump_optimize` (`cross_jump=0, noop_moves=0`).
  virtual RtlControlFlowGraph InvertJump1Optimize(const RtlControlFlowGraph& /*x*/) const {
    return {};
  }

  // ---------------------------------------------------------------------------
  // Stage 13: Tree -> RTL Expansion (`function.c`, `stmt.c`, `expr.c`, `calls.c`)
  // ---------------------------------------------------------------------------

  // 27. Inverse of `function.c:instantiate_virtual_regs`.
  virtual RtlControlFlowGraph InvertInstantiateVirtualRegs(const RtlControlFlowGraph& /*x*/) const {
    return {};
  }

  // 28. Inverse of `function.c:expand_function_end`.
  virtual absl::StatusOr<ExpandFunctionEndInput> InvertExpandFunctionEnd(
      const RtlControlFlowGraph& /*x*/) const {
    return absl::UnimplementedError("InvertExpandFunctionEnd not implemented");
  }

  // 29. Inverse of `stmt.c:expand_return`.
  virtual absl::StatusOr<ExpandReturnInput> InvertExpandReturn(
      const RtlControlFlowGraph& /*x*/) const {
    return absl::UnimplementedError("InvertExpandReturn not implemented");
  }

  // 30. Inverse of `expr.c:expand_expr`.
  virtual absl::StatusOr<ExpandExprInput> InvertExpandExpr(const RtlControlFlowGraph& /*x*/) const {
    return absl::UnimplementedError("InvertExpandExpr not implemented");
  }

  // 31. Inverse of `expr.c:expand_assignment`.
  virtual absl::StatusOr<ExpandAssignmentInput> InvertExpandAssignment(
      const RtlControlFlowGraph& /*x*/) const {
    return absl::UnimplementedError("InvertExpandAssignment not implemented");
  }

  // 32. Inverse of `expr.c:do_store_flag`.
  virtual absl::StatusOr<DoStoreFlagInput> InvertDoStoreFlag(
      const RtlControlFlowGraph& /*x*/) const {
    return absl::UnimplementedError("InvertDoStoreFlag not implemented");
  }

  // 33. Inverse of `expr.c:do_jump`.
  virtual absl::StatusOr<DoJumpInput> InvertDoJump(const RtlControlFlowGraph& /*x*/) const {
    return absl::UnimplementedError("InvertDoJump not implemented");
  }

  // 34. Inverse of `calls.c:expand_call`.
  virtual absl::StatusOr<ExpandCallInput> InvertExpandCall(const RtlControlFlowGraph& /*x*/) const {
    return absl::UnimplementedError("InvertExpandCall not implemented");
  }

  // 35. Inverse of `stmt.c:expand_start_cond` / `expand_start_else` /
  // `expand_end_cond`.
  virtual absl::StatusOr<ExpandCondInput> InvertExpandCond(const RtlControlFlowGraph& /*x*/) const {
    return absl::UnimplementedError("InvertExpandCond not implemented");
  }

  // 36. Inverse of `stmt.c:expand_start_loop` / `expand_exit_loop_if_false` /
  // `expand_end_loop`.
  virtual absl::StatusOr<ExpandLoopInput> InvertExpandLoop(const RtlControlFlowGraph& /*x*/) const {
    return absl::UnimplementedError("InvertExpandLoop not implemented");
  }

  // 37. Inverse of `stmt.c:expand_start_case` / `pushcase` / `expand_end_case`.
  virtual absl::StatusOr<ExpandCaseInput> InvertExpandCase(const RtlControlFlowGraph& /*x*/) const {
    return absl::UnimplementedError("InvertExpandCase not implemented");
  }

  // 38. Inverse of `stmt.c:expand_decl`.
  virtual absl::StatusOr<ExpandDeclInput> InvertExpandDecl(const RtlControlFlowGraph& /*x*/) const {
    return absl::UnimplementedError("InvertExpandDecl not implemented");
  }

  // 39. Inverse of `function.c:expand_function_start` + `assign_parms`.
  virtual absl::StatusOr<FunctionDeclaration> InvertExpandFunctionStart(
      const RtlControlFlowGraph& /*x*/) const {
    return absl::UnimplementedError("InvertExpandFunctionStart not implemented");
  }
};

}  // namespace rom_nom_nom::gcc_harness

#endif  // GCC_HARNESS_SUBFUNCTION_INVERSE_PROVIDER_H_
