#include "lifter/inverse_compiler_pipeline.h"

#include <array>
#include <cstddef>
#include <memory>
#include <utility>

#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/types/span.h"
#include "lifter/ast_converter.h"
#include "lifter/cfg_de_reorg.h"
#include "lifter/control_flow_structurer.h"
#include "lifter/decompilation_context.h"
#include "lifter/dominator_tree.h"
#include "lifter/inverse_combine_stage.h"
#include "lifter/inverse_cse1_basic_block_stage.h"
#include "lifter/inverse_cse2_post_loop_stage.h"
#include "lifter/inverse_final_and_gas_stage.h"
#include "lifter/inverse_flow_life_analysis_stage.h"
#include "lifter/inverse_greg_reload_stage.h"
#include "lifter/inverse_jump1_cfg_cleanup_stage.h"
#include "lifter/inverse_jump2_cross_jump_stage.h"
#include "lifter/inverse_loop_opt_stage.h"
#include "lifter/inverse_rtl_to_c_ast_stage.h"
#include "lifter/inverse_sched1_pre_alloc_stage.h"
#include "lifter/inverse_sched2_post_reload_stage.h"
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
  if (!context->Flags().RunsDbrSchedule() && !context->Flags().GasOptimizesAndReordersInsns()) {
    return absl::OkStatus();
  }
  // Produce the canonical `.jump2` RtlCfg with all delay slots unscheduled (`BuildStage1Pipeline`)
  // and contiguous fallthrough blocks coalesced.
  ControlFlowGraph stage1_cfg = context->Cfg();
  CfgAnalysisManager stage1_analyses(&stage1_cfg);
  stage1_analyses.SetFlags(context->Flags());
  CfgDeReorg::UnscheduleDelaySlotsOnly(&stage1_cfg, &stage1_analyses);

  CfgPassPipeline pre_cross_jump_pipeline;
  pre_cross_jump_pipeline.AddPass(std::make_unique<SimpleDelaySlotUnschedulerPass>());
  pre_cross_jump_pipeline.AddPass(std::make_unique<TargetHeaderDelaySlotReconcilerPass>());
  pre_cross_jump_pipeline.AddPass(std::make_unique<RelaxDelaySlotUnschedulerPass>());
  pre_cross_jump_pipeline.AddPass(std::make_unique<EagerThreadDelaySlotUnschedulerPass>());
  pre_cross_jump_pipeline.Run(context->MutableCfg(), &context->CfgAnalyses());
  context->InvalidateFrameLayout();
  const RtlControlFlowGraph dbr_rtl_cfg = context->RtlCfg();
  context->SetRtlCfg(InverseFinalAndGasLifter::LiftToJump2RtlCfg(
      stage1_cfg, &dbr_rtl_cfg, &context->FrameLayout().RawAnalysis(), context->Symbols(),
      context->Function().memory_reader, context->FunctionName(), &context->Flags()));
  return absl::OkStatus();
}

absl::Status InverseJump2CrossJumpStage::Run(DecompilationContext* context) {
  if (context == nullptr || !context->HasCfg()) {
    return absl::FailedPreconditionError("InverseJump2CrossJumpStage: CFG not initialized.");
  }
  if (!context->Flags().RunsOptimizerPasses()) {
    return absl::OkStatus();
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
  stage2_analyses.SetFlags(context->Flags());
  CfgDeReorg::UnscheduleDelaySlotsOnly(&stage2_cfg, &stage2_analyses);
  const RtlControlFlowGraph jump2_rtl_cfg = context->RtlCfg();
  context->SetRtlCfg(InverseJump2CrossJumpLifter::LiftToSched2RtlCfg(
      stage2_cfg, &context->FrameLayout().RawAnalysis(), &jump2_rtl_cfg, context->Symbols(),
      context->Function().memory_reader, context->FunctionName(), &context->Flags()));
  return absl::OkStatus();
}

absl::Status InverseSched2PostReloadStage::Run(DecompilationContext* context) {
  if (context == nullptr || !context->HasRtlCfg()) {
    return absl::FailedPreconditionError("InverseSched2PostReloadStage: RtlCfg not initialized.");
  }
  InverseSched2PostReloadLifter::InvertSched2ToGregRtlCfg(
      context->MutableRtlCfg(), &context->FrameLayout().RawAnalysis(), context->Symbols(),
      context->FunctionName(), &context->Flags());
  return absl::OkStatus();
}

absl::Status InverseGregReloadStage::Run(DecompilationContext* context) {
  if (context == nullptr || !context->HasRtlCfg()) {
    return absl::FailedPreconditionError("InverseGregReloadStage: RtlCfg not initialized.");
  }
  InverseSched2PostReloadLifter::RestoreGregSelfMovesAndFormatAsMipsRtl(
      context->MutableRtlCfg(), context->Symbols(), context->FunctionName(), &context->Flags());
  InverseGregReloadLifter::InvertGregToLregRtlCfg(
      context->MutableRtlCfg(), &context->FrameLayout().RawAnalysis(), &context->Flags());
  return absl::OkStatus();
}

absl::Status InverseLregLocalAllocStage::Run(DecompilationContext* context) {
  if (context == nullptr || !context->HasRtlCfg()) {
    return absl::FailedPreconditionError("InverseLregLocalAllocStage: RtlCfg not initialized.");
  }
  RtlControlFlowGraph* cfg = context->MutableRtlCfg();
  cfg->pass = RtlPassKind::kSched;
  // Invert `local-alloc.c:local_alloc` (`.07.sched` -> `.08.lreg`): classify register webs into
  // single-basic-block local allocations (`reg_basic_block[r] >= 0`) vs. multi-block global
  // allocations (`reg_basic_block[r] == REG_BLOCK_GLOBAL`).
  std::array<int, 64> first_block;
  first_block.fill(-1);
  std::array<bool, 64> is_multi_block{};
  for (size_t b = 0; b < cfg->rpo_blocks.size(); ++b) {
    for (const RtlInsn& rinsn : cfg->rpo_blocks[b].rtl_insns) {
      for (const Instruction& mi : rinsn.machine_instructions) {
        for (const std::optional<Register>& reg_opt : {mi.rs, mi.rt, mi.rd}) {
          if (!reg_opt.has_value()) {
            continue;
          }
          const size_t r = static_cast<size_t>(*reg_opt);
          if (r == 0 || r >= first_block.size()) {
            continue;
          }
          if (first_block[r] == -1) {
            first_block[r] = static_cast<int>(b);
          } else if (first_block[r] != static_cast<int>(b)) {
            is_multi_block[r] = true;
          }
        }
      }
    }
  }
  size_t local_count = 0;
  size_t global_count = 0;
  for (size_t r = 1; r < first_block.size(); ++r) {
    if (first_block[r] != -1) {
      if (is_multi_block[r]) {
        ++global_count;
      } else {
        ++local_count;
      }
    }
  }
  cfg->local_alloc_pseudo_reg_count = local_count;
  cfg->global_alloc_pseudo_reg_count = global_count;
  return absl::OkStatus();
}

absl::Status InverseSched1PreAllocStage::Run(DecompilationContext* context) {
  if (context == nullptr || !context->HasRtlCfg()) {
    return absl::FailedPreconditionError("InverseSched1PreAllocStage: RtlCfg not initialized.");
  }
  InverseSched1PreAllocLifter::InvertSchedToCombineRtlCfg(context->MutableRtlCfg());
  return absl::OkStatus();
}

absl::Status InverseCombineStage::Run(DecompilationContext* context) {
  if (context == nullptr || !context->HasRtlCfg()) {
    return absl::FailedPreconditionError("InverseCombineStage: RtlCfg not initialized.");
  }
  RtlControlFlowGraph* cfg = context->MutableRtlCfg();
  if (context->Flags().RunsOptimizerPasses()) {
    for (auto& blk : cfg->rpo_blocks) {
      for (size_t i = 0; i < blk.rtl_insns.size(); ++i) {
        if (!blk.rtl_insns[i].machine_instructions.empty() &&
            absl::StartsWith(blk.rtl_insns[i].canonical_pattern, "(") &&
            !absl::StartsWith(blk.rtl_insns[i].canonical_pattern, "(set (pc)") &&
            !absl::StartsWith(blk.rtl_insns[i].canonical_pattern, "(parallel[ (set (pc)")) {
          const auto& m_insts = blk.rtl_insns[i].machine_instructions;
          const RtlFusedIdiomKind kind = blk.rtl_insns[i].idiom_kind;
          const Instruction* rep_inst = &m_insts.front();
          if (kind == RtlFusedIdiomKind::kFloatSiSf2 || kind == RtlFusedIdiomKind::kFloatSiDf2 ||
              kind == RtlFusedIdiomKind::kGasFloatImmediate ||
              kind == RtlFusedIdiomKind::kGasMagicMultiplier ||
              kind == RtlFusedIdiomKind::kGasAddressHiLo ||
              kind == RtlFusedIdiomKind::kGasSymbolIndexedAccess ||
              kind == RtlFusedIdiomKind::kGasSymbolDirectAccess) {
            rep_inst = &m_insts.back();
          } else if (kind == RtlFusedIdiomKind::kMove2Words && m_insts.size() == 4) {
            rep_inst = &m_insts[1];
          } else if ((kind == RtlFusedIdiomKind::kFixTruncSfSi2 ||
                      kind == RtlFusedIdiomKind::kFixTruncDfSi2) &&
                     m_insts.size() == 7) {
            rep_inst = &m_insts[4];
          }
          blk.rtl_insns[i].canonical_pattern =
              InverseFinalAndGasLifter::FormatMipsInstAsCanonicalRtl(*rep_inst);
        }
        if (i < blk.instructions.size()) {
          blk.instructions[i] = blk.rtl_insns[i].canonical_pattern;
        }
      }
    }
  }
  InverseCombineLifter::InvertCombineToFlowRtlCfg(cfg, &context->Flags());
  for (auto& blk : cfg->rpo_blocks) {
    for (size_t i = 0; i < blk.rtl_insns.size(); ++i) {
      if (!blk.rtl_insns[i].rtl_sexpr_pattern.empty()) {
        blk.rtl_insns[i].canonical_pattern = blk.rtl_insns[i].rtl_sexpr_pattern;
      }
      if (i < blk.instructions.size()) {
        blk.instructions[i] = blk.rtl_insns[i].canonical_pattern;
      }
    }
  }
  return absl::OkStatus();
}

absl::Status InverseFlowLifeAnalysisStage::Run(DecompilationContext* context) {
  if (context == nullptr || !context->HasRtlCfg()) {
    return absl::FailedPreconditionError("InverseFlowLifeAnalysisStage: RtlCfg not initialized.");
  }
  RtlControlFlowGraph* cfg = context->MutableRtlCfg();
  for (auto& blk : cfg->rpo_blocks) {
    for (size_t i = 0; i < blk.rtl_insns.size(); ++i) {
      if (blk.rtl_insns[i].rtl_sexpr_pattern.empty() &&
          absl::StartsWith(blk.rtl_insns[i].canonical_pattern, "(")) {
        blk.rtl_insns[i].rtl_sexpr_pattern = blk.rtl_insns[i].canonical_pattern;
      }
      if (!blk.rtl_insns[i].machine_instructions.empty() &&
          absl::StartsWith(blk.rtl_insns[i].canonical_pattern, "(") &&
          !absl::StartsWith(blk.rtl_insns[i].canonical_pattern, "(set (pc)") &&
          !absl::StartsWith(blk.rtl_insns[i].canonical_pattern, "(parallel[ (set (pc)")) {
        const auto& m_insts = blk.rtl_insns[i].machine_instructions;
        const RtlFusedIdiomKind kind = blk.rtl_insns[i].idiom_kind;
        const Instruction* rep_inst = &m_insts.front();
        if (kind == RtlFusedIdiomKind::kFloatSiSf2 || kind == RtlFusedIdiomKind::kFloatSiDf2 ||
            kind == RtlFusedIdiomKind::kGasFloatImmediate ||
            kind == RtlFusedIdiomKind::kGasMagicMultiplier ||
            kind == RtlFusedIdiomKind::kGasAddressHiLo ||
            kind == RtlFusedIdiomKind::kGasSymbolIndexedAccess ||
            kind == RtlFusedIdiomKind::kGasSymbolDirectAccess) {
          rep_inst = &m_insts.back();
        } else if (kind == RtlFusedIdiomKind::kMove2Words && m_insts.size() == 4) {
          rep_inst = &m_insts[1];
        } else if ((kind == RtlFusedIdiomKind::kFixTruncSfSi2 ||
                    kind == RtlFusedIdiomKind::kFixTruncDfSi2) &&
                   m_insts.size() == 7) {
          rep_inst = &m_insts[4];
        }
        blk.rtl_insns[i].canonical_pattern =
            InverseFinalAndGasLifter::FormatMipsInstAsCanonicalRtl(*rep_inst);
      } else if (blk.rtl_insns[i].is_synthetic_rtl_expansion &&
                 blk.rtl_insns[i].machine_instructions.empty()) {
        blk.rtl_insns[i].canonical_pattern = "addu  $zero, $zero, $zero";
      }
      if (i < blk.instructions.size()) {
        blk.instructions[i] = blk.rtl_insns[i].canonical_pattern;
      }
    }
  }
  InverseFlowLifeAnalysisLifter::InvertFlowToCse2RtlCfg(cfg, &context->Flags());
  for (auto& blk : cfg->rpo_blocks) {
    for (size_t i = 0; i < blk.rtl_insns.size(); ++i) {
      if (!blk.rtl_insns[i].rtl_sexpr_pattern.empty()) {
        blk.rtl_insns[i].canonical_pattern = blk.rtl_insns[i].rtl_sexpr_pattern;
      }
      if (i < blk.instructions.size()) {
        blk.instructions[i] = blk.rtl_insns[i].canonical_pattern;
      }
    }
  }
  return absl::OkStatus();
}

absl::Status InverseCse2PostLoopStage::Run(DecompilationContext* context) {
  if (context == nullptr || !context->HasRtlCfg()) {
    return absl::FailedPreconditionError("InverseCse2PostLoopStage: RtlCfg not initialized.");
  }
  RtlControlFlowGraph* cfg = context->MutableRtlCfg();
  for (auto& blk : cfg->rpo_blocks) {
    for (size_t i = 0; i < blk.rtl_insns.size(); ++i) {
      if (!blk.rtl_insns[i].machine_instructions.empty() &&
          absl::StartsWith(blk.rtl_insns[i].canonical_pattern, "(") &&
          !absl::StartsWith(blk.rtl_insns[i].canonical_pattern, "(set (pc)") &&
          !absl::StartsWith(blk.rtl_insns[i].canonical_pattern, "(parallel[ (set (pc)")) {
        const auto& m_insts = blk.rtl_insns[i].machine_instructions;
        const RtlFusedIdiomKind kind = blk.rtl_insns[i].idiom_kind;
        const Instruction* rep_inst = &m_insts.front();
        if (kind == RtlFusedIdiomKind::kFloatSiSf2 || kind == RtlFusedIdiomKind::kFloatSiDf2 ||
            kind == RtlFusedIdiomKind::kGasFloatImmediate ||
            kind == RtlFusedIdiomKind::kGasMagicMultiplier ||
            kind == RtlFusedIdiomKind::kGasAddressHiLo ||
            kind == RtlFusedIdiomKind::kGasSymbolIndexedAccess ||
            kind == RtlFusedIdiomKind::kGasSymbolDirectAccess) {
          rep_inst = &m_insts.back();
        } else if (kind == RtlFusedIdiomKind::kMove2Words && m_insts.size() == 4) {
          rep_inst = &m_insts[1];
        } else if ((kind == RtlFusedIdiomKind::kFixTruncSfSi2 ||
                    kind == RtlFusedIdiomKind::kFixTruncDfSi2) &&
                   m_insts.size() == 7) {
          rep_inst = &m_insts[4];
        }
        blk.rtl_insns[i].canonical_pattern =
            InverseFinalAndGasLifter::FormatMipsInstAsCanonicalRtl(*rep_inst);
      } else if (blk.rtl_insns[i].is_synthetic_rtl_expansion &&
                 blk.rtl_insns[i].machine_instructions.empty()) {
        blk.rtl_insns[i].canonical_pattern = "addu  $zero, $zero, $zero";
      }
      if (i < blk.instructions.size()) {
        blk.instructions[i] = blk.rtl_insns[i].canonical_pattern;
      }
    }
  }
  InverseCse2PostLoopLifter::InvertCse2ToLoopRtlCfg(cfg, &context->Flags());
  for (auto& blk : cfg->rpo_blocks) {
    for (size_t i = 0; i < blk.rtl_insns.size(); ++i) {
      if (!blk.rtl_insns[i].rtl_sexpr_pattern.empty()) {
        blk.rtl_insns[i].canonical_pattern = blk.rtl_insns[i].rtl_sexpr_pattern;
      }
      if (i < blk.instructions.size()) {
        blk.instructions[i] = blk.rtl_insns[i].canonical_pattern;
      }
    }
  }
  return absl::OkStatus();
}

absl::Status InverseLoopOptStage::Run(DecompilationContext* context) {
  if (context == nullptr || !context->HasRtlCfg()) {
    return absl::FailedPreconditionError("InverseLoopOptStage: RtlCfg not initialized.");
  }
  RtlControlFlowGraph* cfg = context->MutableRtlCfg();
  for (auto& blk : cfg->rpo_blocks) {
    for (size_t i = 0; i < blk.rtl_insns.size(); ++i) {
      if (!blk.rtl_insns[i].machine_instructions.empty() &&
          absl::StartsWith(blk.rtl_insns[i].canonical_pattern, "(") &&
          !absl::StartsWith(blk.rtl_insns[i].canonical_pattern, "(set (pc)") &&
          !absl::StartsWith(blk.rtl_insns[i].canonical_pattern, "(parallel[ (set (pc)")) {
        const auto& m_insts = blk.rtl_insns[i].machine_instructions;
        const RtlFusedIdiomKind kind = blk.rtl_insns[i].idiom_kind;
        const Instruction* rep_inst = &m_insts.front();
        if (kind == RtlFusedIdiomKind::kFloatSiSf2 || kind == RtlFusedIdiomKind::kFloatSiDf2 ||
            kind == RtlFusedIdiomKind::kGasFloatImmediate ||
            kind == RtlFusedIdiomKind::kGasMagicMultiplier ||
            kind == RtlFusedIdiomKind::kGasAddressHiLo ||
            kind == RtlFusedIdiomKind::kGasSymbolIndexedAccess ||
            kind == RtlFusedIdiomKind::kGasSymbolDirectAccess) {
          rep_inst = &m_insts.back();
        } else if (kind == RtlFusedIdiomKind::kMove2Words && m_insts.size() == 4) {
          rep_inst = &m_insts[1];
        } else if ((kind == RtlFusedIdiomKind::kFixTruncSfSi2 ||
                    kind == RtlFusedIdiomKind::kFixTruncDfSi2) &&
                   m_insts.size() == 7) {
          rep_inst = &m_insts[4];
        }
        blk.rtl_insns[i].canonical_pattern =
            InverseFinalAndGasLifter::FormatMipsInstAsCanonicalRtl(*rep_inst);
      } else if (blk.rtl_insns[i].is_synthetic_rtl_expansion &&
                 blk.rtl_insns[i].machine_instructions.empty()) {
        blk.rtl_insns[i].canonical_pattern = "addu  $zero, $zero, $zero";
      }
      if (i < blk.instructions.size()) {
        blk.instructions[i] = blk.rtl_insns[i].canonical_pattern;
      }
    }
  }
  InverseLoopOptLifter::InvertLoopToCseRtlCfg(cfg);
  for (auto& blk : cfg->rpo_blocks) {
    for (size_t i = 0; i < blk.rtl_insns.size(); ++i) {
      if (!blk.rtl_insns[i].rtl_sexpr_pattern.empty()) {
        blk.rtl_insns[i].canonical_pattern = blk.rtl_insns[i].rtl_sexpr_pattern;
      }
      if (i < blk.instructions.size()) {
        blk.instructions[i] = blk.rtl_insns[i].canonical_pattern;
      }
    }
  }
  return absl::OkStatus();
}

absl::Status InverseCse1BasicBlockStage::Run(DecompilationContext* context) {
  if (context == nullptr || !context->HasRtlCfg()) {
    return absl::FailedPreconditionError("InverseCse1BasicBlockStage: RtlCfg not initialized.");
  }
  RtlControlFlowGraph* cfg = context->MutableRtlCfg();
  if (!context->Flags().RunsOptimizerPasses()) {
    cfg->pass = RtlPassKind::kJump;
    return absl::OkStatus();
  }
  for (auto& blk : cfg->rpo_blocks) {
    for (size_t i = 0; i < blk.rtl_insns.size(); ++i) {
      if (!blk.rtl_insns[i].machine_instructions.empty() &&
          absl::StartsWith(blk.rtl_insns[i].canonical_pattern, "(") &&
          !absl::StartsWith(blk.rtl_insns[i].canonical_pattern, "(set (pc)") &&
          !absl::StartsWith(blk.rtl_insns[i].canonical_pattern, "(parallel[ (set (pc)")) {
        const auto& m_insts = blk.rtl_insns[i].machine_instructions;
        const RtlFusedIdiomKind kind = blk.rtl_insns[i].idiom_kind;
        const Instruction* rep_inst = &m_insts.front();
        if (kind == RtlFusedIdiomKind::kFloatSiSf2 || kind == RtlFusedIdiomKind::kFloatSiDf2 ||
            kind == RtlFusedIdiomKind::kGasFloatImmediate ||
            kind == RtlFusedIdiomKind::kGasMagicMultiplier ||
            kind == RtlFusedIdiomKind::kGasAddressHiLo ||
            kind == RtlFusedIdiomKind::kGasSymbolIndexedAccess ||
            kind == RtlFusedIdiomKind::kGasSymbolDirectAccess) {
          rep_inst = &m_insts.back();
        } else if (kind == RtlFusedIdiomKind::kMove2Words && m_insts.size() == 4) {
          rep_inst = &m_insts[1];
        } else if ((kind == RtlFusedIdiomKind::kFixTruncSfSi2 ||
                    kind == RtlFusedIdiomKind::kFixTruncDfSi2) &&
                   m_insts.size() == 7) {
          rep_inst = &m_insts[4];
        }
        blk.rtl_insns[i].canonical_pattern =
            InverseFinalAndGasLifter::FormatMipsInstAsCanonicalRtl(*rep_inst);
      } else if (blk.rtl_insns[i].is_synthetic_rtl_expansion &&
                 blk.rtl_insns[i].machine_instructions.empty()) {
        blk.rtl_insns[i].canonical_pattern = "addu  $zero, $zero, $zero";
      }
      if (i < blk.instructions.size()) {
        blk.instructions[i] = blk.rtl_insns[i].canonical_pattern;
      }
    }
  }
  InverseCse1BasicBlockLifter::InvertCseToJumpRtlCfg(cfg);
  for (auto& blk : cfg->rpo_blocks) {
    for (size_t i = 0; i < blk.rtl_insns.size(); ++i) {
      if (!blk.rtl_insns[i].rtl_sexpr_pattern.empty()) {
        blk.rtl_insns[i].canonical_pattern = blk.rtl_insns[i].rtl_sexpr_pattern;
      }
      if (i < blk.instructions.size()) {
        blk.instructions[i] = blk.rtl_insns[i].canonical_pattern;
      }
    }
  }
  return absl::OkStatus();
}

absl::Status InverseJump1CfgCleanupStage::Run(DecompilationContext* context) {
  if (context == nullptr || !context->HasRtlCfg()) {
    return absl::FailedPreconditionError("InverseJump1CfgCleanupStage: RtlCfg not initialized.");
  }
  RtlControlFlowGraph* cfg = context->MutableRtlCfg();
  for (auto& blk : cfg->rpo_blocks) {
    for (size_t i = 0; i < blk.rtl_insns.size(); ++i) {
      if (!blk.rtl_insns[i].machine_instructions.empty() &&
          absl::StartsWith(blk.rtl_insns[i].canonical_pattern, "(") &&
          !absl::StartsWith(blk.rtl_insns[i].canonical_pattern, "(set (pc)") &&
          !absl::StartsWith(blk.rtl_insns[i].canonical_pattern, "(parallel[ (set (pc)")) {
        const auto& m_insts = blk.rtl_insns[i].machine_instructions;
        const RtlFusedIdiomKind kind = blk.rtl_insns[i].idiom_kind;
        const Instruction* rep_inst = &m_insts.front();
        if (kind == RtlFusedIdiomKind::kFloatSiSf2 || kind == RtlFusedIdiomKind::kFloatSiDf2 ||
            kind == RtlFusedIdiomKind::kGasFloatImmediate ||
            kind == RtlFusedIdiomKind::kGasMagicMultiplier ||
            kind == RtlFusedIdiomKind::kGasAddressHiLo ||
            kind == RtlFusedIdiomKind::kGasSymbolIndexedAccess ||
            kind == RtlFusedIdiomKind::kGasSymbolDirectAccess) {
          rep_inst = &m_insts.back();
        } else if (kind == RtlFusedIdiomKind::kMove2Words && m_insts.size() == 4) {
          rep_inst = &m_insts[1];
        } else if ((kind == RtlFusedIdiomKind::kFixTruncSfSi2 ||
                    kind == RtlFusedIdiomKind::kFixTruncDfSi2) &&
                   m_insts.size() == 7) {
          rep_inst = &m_insts[4];
        }
        blk.rtl_insns[i].canonical_pattern =
            InverseFinalAndGasLifter::FormatMipsInstAsCanonicalRtl(*rep_inst);
      } else if (blk.rtl_insns[i].is_synthetic_rtl_expansion &&
                 blk.rtl_insns[i].machine_instructions.empty()) {
        blk.rtl_insns[i].canonical_pattern = "addu  $zero, $zero, $zero";
      }
      if (i < blk.instructions.size()) {
        blk.instructions[i] = blk.rtl_insns[i].canonical_pattern;
      }
    }
  }
  InverseJump1CfgCleanupLifter::InvertJumpToRtlCfg(cfg);
  return absl::OkStatus();
}

absl::Status InverseRtlToCAstStage::Run(DecompilationContext* context) {
  return InverseRtlToCAstLifter::LiftRtlToCAst(context);
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
