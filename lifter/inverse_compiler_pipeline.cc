#include "lifter/inverse_compiler_pipeline.h"

#include <array>
#include <cstddef>
#include <memory>
#include <utility>

#include "absl/status/status.h"
#include "absl/types/span.h"
#include "lifter/ast_converter.h"
#include "lifter/cfg_de_reorg.h"
#include "lifter/control_flow_structurer.h"
#include "lifter/decompilation_context.h"
#include "lifter/dominator_tree.h"
#include "lifter/inverse_final_and_gas_stage.h"
#include "lifter/loop_analyzer.h"
#include "lifter/rtl_cfg.h"

namespace rom_nom_nom {
namespace {

constexpr std::array<InverseStageDescriptor, 14> kStageDescriptors = {{
    {InverseStageId::kStage00FinalAndGas, 0, 13, RtlPassKind::kDbr, std::nullopt,
     "InverseFinalAndGasStage", "GCCStage13_FinalAndGas (.dbr -> .o)"},
    {InverseStageId::kStage01DbrDelaySlot, 1, 12, RtlPassKind::kJump2, RtlPassKind::kDbr,
     "InverseDbrDelaySlotStage", "GCCStage12_DbrReorg (.jump2 -> .dbr)"},
    {InverseStageId::kStage02Jump2CrossJump, 2, 11, RtlPassKind::kSched2, RtlPassKind::kJump2,
     "InverseJump2CrossJumpStage", "GCCStage11_Jump2CrossJump (.sched2 -> .jump2)"},
    {InverseStageId::kStage03Sched2PostReload, 3, 10, RtlPassKind::kGreg, RtlPassKind::kSched2,
     "InverseSched2PostReloadStage", "GCCStage10_Sched2 (.greg -> .sched2)"},
    {InverseStageId::kStage04GregReload, 4, 9, RtlPassKind::kLreg, RtlPassKind::kGreg,
     "InverseGregReloadStage", "GCCStage9_GregReload (.lreg -> .greg)"},
    {InverseStageId::kStage05LregLocalAlloc, 5, 8, RtlPassKind::kSched, RtlPassKind::kLreg,
     "InverseLregLocalAllocStage", "GCCStage8_LregLocalAlloc (.sched -> .lreg)"},
    {InverseStageId::kStage06Sched1PreAlloc, 6, 7, RtlPassKind::kCombine, RtlPassKind::kSched,
     "InverseSched1PreAllocStage", "GCCStage7_Sched1 (.combine -> .sched)"},
    {InverseStageId::kStage07Combine, 7, 6, RtlPassKind::kFlow, RtlPassKind::kCombine,
     "InverseCombineStage", "GCCStage6_Combine (.flow -> .combine)"},
    {InverseStageId::kStage08FlowLifeAnalysis, 8, 5, RtlPassKind::kCse2, RtlPassKind::kFlow,
     "InverseFlowLifeAnalysisStage", "GCCStage5_Flow (.cse2 -> .flow)"},
    {InverseStageId::kStage09Cse2PostLoop, 9, 4, RtlPassKind::kLoop, RtlPassKind::kCse2,
     "InverseCse2PostLoopStage", "GCCStage4_Cse2 (.loop -> .cse2)"},
    {InverseStageId::kStage10LoopOpt, 10, 3, RtlPassKind::kCse, RtlPassKind::kLoop,
     "InverseLoopOptStage", "GCCStage3_LoopOpt (.cse -> .loop)"},
    {InverseStageId::kStage11Cse1BasicBlock, 11, 2, RtlPassKind::kJump, RtlPassKind::kCse,
     "InverseCse1BasicBlockStage", "GCCStage2_Cse1 (.jump -> .cse)"},
    {InverseStageId::kStage12Jump1CfgCleanup, 12, 1, RtlPassKind::kRtl, RtlPassKind::kJump,
     "InverseJump1CfgCleanupStage", "GCCStage1_Jump1AndRtl (.rtl -> .jump)"},
    {InverseStageId::kStage13RtlToCAst, 13, 0, std::nullopt, RtlPassKind::kRtl,
     "InverseRtlToCAstStage", "GCCStage0_TreeToRtl (C AST -> .rtl)"},
}};

}  // namespace

const InverseStageDescriptor& InverseCompilerStage::Descriptor() const {
  return InverseCompilerPipeline::GetStageDescriptor(Id());
}

absl::Status InverseFinalAndGasStage::Run(DecompilationContext* context) {
  if (context == nullptr) {
    return absl::InvalidArgumentError("InverseFinalAndGasStage: null DecompilationContext.");
  }
  if (!context->HasCfg()) {
    if (absl::Status status = context->BuildCfg(); !status.ok()) {
      return status;
    }
  }
  context->LiftCfgToRtl();
  return absl::OkStatus();
}

absl::Status InverseDbrDelaySlotStage::Run(DecompilationContext* context) {
  if (context == nullptr || !context->HasCfg()) {
    return absl::FailedPreconditionError("InverseDbrDelaySlotStage: CFG not initialized.");
  }
  // Produce the canonical `.jump2` RtlCfg with all delay slots unscheduled (`BuildStage1Pipeline`)
  // and contiguous fallthrough blocks coalesced.
  ControlFlowGraph stage1_cfg = context->Cfg();
  CfgAnalysisManager stage1_analyses(&stage1_cfg);
  CfgDeReorg::UnscheduleDelaySlotsOnly(&stage1_cfg, &stage1_analyses);

  CfgPassPipeline pre_cross_jump_pipeline;
  pre_cross_jump_pipeline.AddPass(std::make_unique<SimpleDelaySlotUnschedulerPass>());
  pre_cross_jump_pipeline.AddPass(std::make_unique<TargetHeaderDelaySlotReconcilerPass>());
  pre_cross_jump_pipeline.AddPass(std::make_unique<RelaxDelaySlotUnschedulerPass>());
  pre_cross_jump_pipeline.AddPass(std::make_unique<EagerThreadDelaySlotUnschedulerPass>());
  pre_cross_jump_pipeline.Run(context->MutableCfg(), &context->CfgAnalyses());
  context->InvalidateFrameLayout();
  context->SetRtlCfg(
      InverseFinalAndGasLifter::LiftToRtlCfg(stage1_cfg, &context->FrameLayout().RawAnalysis()));
  return absl::OkStatus();
}

absl::Status InverseJump2CrossJumpStage::Run(DecompilationContext* context) {
  if (context == nullptr || !context->HasCfg()) {
    return absl::FailedPreconditionError("InverseJump2CrossJumpStage: CFG not initialized.");
  }
  CfgPassPipeline cross_jump_pipeline;
  cross_jump_pipeline.AddPass(std::make_unique<CrossJumpTailDuplicatorPass>());
  cross_jump_pipeline.AddPass(std::make_unique<SimpleDelaySlotUnschedulerPass>());
  cross_jump_pipeline.AddPass(std::make_unique<EagerThreadDelaySlotUnschedulerPass>(
      EagerThreadUnscheduleMode::kPostCrossJump));
  cross_jump_pipeline.Run(context->MutableCfg(), &context->CfgAnalyses());
  context->InvalidateFrameLayout();

  ControlFlowGraph stage2_cfg = context->Cfg();
  CfgAnalysisManager stage2_analyses(&stage2_cfg);
  CfgDeReorg::UnscheduleDelaySlotsOnly(&stage2_cfg, &stage2_analyses);
  context->SetRtlCfg(InverseFinalAndGasLifter::LiftToSched2RtlCfg(
      stage2_cfg, &context->FrameLayout().RawAnalysis()));
  return absl::OkStatus();
}

absl::Status InverseSched2PostReloadStage::Run(DecompilationContext* context) {
  if (context == nullptr || !context->HasRtlCfg()) {
    return absl::FailedPreconditionError("InverseSched2PostReloadStage: RtlCfg not initialized.");
  }
  InverseFinalAndGasLifter::InvertSched2ToGregRtlCfg(context->MutableRtlCfg(),
                                                     &context->FrameLayout().RawAnalysis(),
                                                     context->Symbols(), context->FunctionName());
  return absl::OkStatus();
}

absl::Status InverseGregReloadStage::Run(DecompilationContext* context) {
  if (context == nullptr || !context->HasRtlCfg()) {
    return absl::FailedPreconditionError("InverseGregReloadStage: RtlCfg not initialized.");
  }
  return absl::OkStatus();
}

absl::Status InverseLregLocalAllocStage::Run(DecompilationContext* context) {
  if (context == nullptr || !context->HasRtlCfg()) {
    return absl::FailedPreconditionError("InverseLregLocalAllocStage: RtlCfg not initialized.");
  }
  return absl::OkStatus();
}

absl::Status InverseSched1PreAllocStage::Run(DecompilationContext* context) {
  if (context == nullptr || !context->HasRtlCfg()) {
    return absl::FailedPreconditionError("InverseSched1PreAllocStage: RtlCfg not initialized.");
  }
  return absl::OkStatus();
}

absl::Status InverseCombineStage::Run(DecompilationContext* context) {
  if (context == nullptr || !context->HasRtlCfg()) {
    return absl::FailedPreconditionError("InverseCombineStage: RtlCfg not initialized.");
  }
  return absl::OkStatus();
}

absl::Status InverseFlowLifeAnalysisStage::Run(DecompilationContext* context) {
  if (context == nullptr || !context->HasRtlCfg()) {
    return absl::FailedPreconditionError("InverseFlowLifeAnalysisStage: RtlCfg not initialized.");
  }
  return absl::OkStatus();
}

absl::Status InverseCse2PostLoopStage::Run(DecompilationContext* context) {
  if (context == nullptr || !context->HasRtlCfg()) {
    return absl::FailedPreconditionError("InverseCse2PostLoopStage: RtlCfg not initialized.");
  }
  return absl::OkStatus();
}

absl::Status InverseLoopOptStage::Run(DecompilationContext* context) {
  if (context == nullptr || !context->HasRtlCfg()) {
    return absl::FailedPreconditionError("InverseLoopOptStage: RtlCfg not initialized.");
  }
  return absl::OkStatus();
}

absl::Status InverseCse1BasicBlockStage::Run(DecompilationContext* context) {
  if (context == nullptr || !context->HasRtlCfg()) {
    return absl::FailedPreconditionError("InverseCse1BasicBlockStage: RtlCfg not initialized.");
  }
  return absl::OkStatus();
}

absl::Status InverseJump1CfgCleanupStage::Run(DecompilationContext* context) {
  if (context == nullptr || !context->HasRtlCfg()) {
    return absl::FailedPreconditionError("InverseJump1CfgCleanupStage: RtlCfg not initialized.");
  }
  return absl::OkStatus();
}

absl::Status InverseRtlToCAstStage::Run(DecompilationContext* context) {
  if (context == nullptr || !context->HasCfg()) {
    return absl::FailedPreconditionError("InverseRtlToCAstStage: CFG not initialized.");
  }
  auto& cfg_analyses = context->CfgAnalyses();
  const DominatorTree& dom_tree = cfg_analyses.GetDominatorTree();
  const DominatorTree& post_dom_tree = cfg_analyses.GetPostDominatorTree();
  const LoopInfo& loop_info = cfg_analyses.GetLoopInfo();

  context->SetRootRegion(
      ControlFlowStructurer::Structure(context->Cfg(), dom_tree, post_dom_tree, loop_info));
  if (!context->HasRootRegion()) {
    return absl::InternalError("InverseRtlToCAstStage: Failed to structure control flow graph.");
  }

  AstConverterOptions converter_opts;
  converter_opts.function_name = context->FunctionName();
  converter_opts.game_config = context->Config();
  converter_opts.function_parameter_counts = context->AbiContext().function_parameter_counts;
  converter_opts.function_fp_parameter_counts = context->AbiContext().function_fp_parameter_counts;
  converter_opts.functions_returning_v0 = context->AbiContext().functions_returning_v0;
  converter_opts.functions_returning_f0 = context->AbiContext().functions_returning_f0;

  auto ast_or = AstConverter::Convert(context->Cfg(), context->RootRegion(), context->Symbols(),
                                      converter_opts);
  if (!ast_or.ok()) {
    return ast_or.status();
  }
  context->SetAst(std::move(*ast_or));
  return absl::OkStatus();
}

void InverseCompilerPipeline::AddStage(std::unique_ptr<InverseCompilerStage> stage) {
  if (stage != nullptr) {
    stages_.push_back(std::move(stage));
  }
}

absl::Span<const InverseStageDescriptor> InverseCompilerPipeline::AllStageDescriptors() {
  return absl::MakeConstSpan(kStageDescriptors);
}

absl::Span<const InverseStageDescriptor> InverseCompilerPipeline::RtlStageDescriptors() {
  return absl::MakeConstSpan(kStageDescriptors.data(), 13);
}

const InverseStageDescriptor& InverseCompilerPipeline::GetStageDescriptor(InverseStageId id) {
  const size_t idx = static_cast<size_t>(id);
  return kStageDescriptors[idx < kStageDescriptors.size() ? idx : 0];
}

InverseCompilerPipeline InverseCompilerPipeline::BuildDefaultPipeline() {
  InverseCompilerPipeline pipeline;
  pipeline.AddStage(std::make_unique<InverseFinalAndGasStage>());
  pipeline.AddStage(std::make_unique<InverseDbrDelaySlotStage>());
  pipeline.AddStage(std::make_unique<InverseJump2CrossJumpStage>());
  pipeline.AddStage(std::make_unique<InverseSched2PostReloadStage>());
  pipeline.AddStage(std::make_unique<InverseGregReloadStage>());
  pipeline.AddStage(std::make_unique<InverseLregLocalAllocStage>());
  pipeline.AddStage(std::make_unique<InverseSched1PreAllocStage>());
  pipeline.AddStage(std::make_unique<InverseCombineStage>());
  pipeline.AddStage(std::make_unique<InverseFlowLifeAnalysisStage>());
  pipeline.AddStage(std::make_unique<InverseCse2PostLoopStage>());
  pipeline.AddStage(std::make_unique<InverseLoopOptStage>());
  pipeline.AddStage(std::make_unique<InverseCse1BasicBlockStage>());
  pipeline.AddStage(std::make_unique<InverseJump1CfgCleanupStage>());
  pipeline.AddStage(std::make_unique<InverseRtlToCAstStage>());
  return pipeline;
}

absl::Status InverseCompilerPipeline::Run(DecompilationContext* context,
                                          StageSnapshotCallback snapshot_cb) const {
  if (context == nullptr) {
    return absl::InvalidArgumentError("InverseCompilerPipeline: null DecompilationContext.");
  }
  for (const auto& stage : stages_) {
    if (absl::Status status = stage->Run(context); !status.ok()) {
      return status;
    }
    if (snapshot_cb) {
      snapshot_cb(stage->Descriptor(), *context);
    }
  }
  return absl::OkStatus();
}

}  // namespace rom_nom_nom
