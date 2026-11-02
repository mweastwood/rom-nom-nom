#include "lifter/inverse_rtl_to_c_ast_stage.h"

#include <utility>

#include "absl/status/status.h"
#include "core/c_ast.h"
#include "lifter/ast_converter.h"
#include "lifter/cfg_analysis_manager.h"
#include "lifter/control_flow_structurer.h"
#include "lifter/decompilation_context.h"
#include "lifter/dominator_tree.h"
#include "lifter/loop_analyzer.h"

namespace rom_nom_nom {

absl::Status InverseRtlToCAstLifter::LiftRtlToCAst(DecompilationContext* context) {
  if (context == nullptr || !context->HasCfg()) {
    return absl::FailedPreconditionError("InverseRtlToCAstStage: CFG not initialized.");
  }

  // 1. Invert `stmt.c` (`expand_start_cond`, `expand_start_loop`, `expand_start_case`) and
  // `expr.c:do_jump` into a hierarchical `StructuredRegion` tree.
  auto& cfg_analyses = context->CfgAnalyses();
  const DominatorTree& dom_tree = cfg_analyses.GetDominatorTree();
  const DominatorTree& post_dom_tree = cfg_analyses.GetPostDominatorTree();
  const LoopInfo& loop_info = cfg_analyses.GetLoopInfo();

  context->SetRootRegion(
      ControlFlowStructurer::Structure(context->Cfg(), dom_tree, post_dom_tree, loop_info));
  if (!context->HasRootRegion()) {
    return absl::InternalError("InverseRtlToCAstStage: Failed to structure control flow graph.");
  }

  // 2. Invert `function.c` (`expand_function_start`, `assign_parms`, `expand_function_end`),
  // `stmt.c` (`expand_decl`, `expand_return`), `expr.c` (`expand_expr`, `expand_assignment`,
  // `store_expr`, `do_store_flag`), and `calls.c` (`expand_call`) into a complete
  // `FunctionDeclaration` C AST via `AstConverter` and `AstPassPipeline`.
  const InterProceduralAbiContext& abi_ctx = context->AbiContext();
  AstConverterOptions converter_opts;
  converter_opts.function_name = context->FunctionName();
  converter_opts.game_config = context->Config();
  converter_opts.function_parameter_counts = abi_ctx.function_parameter_counts;
  converter_opts.function_fp_parameter_counts = abi_ctx.function_fp_parameter_counts;
  converter_opts.functions_returning_v0 = abi_ctx.functions_returning_v0;
  converter_opts.functions_returning_f0 = abi_ctx.functions_returning_f0;

  auto ast_or = AstConverter::Convert(context->Cfg(), context->RootRegion(), context->Symbols(),
                                      converter_opts);
  if (!ast_or.ok()) {
    return ast_or.status();
  }
  context->SetAst(std::move(*ast_or));
  return absl::OkStatus();
}

}  // namespace rom_nom_nom
