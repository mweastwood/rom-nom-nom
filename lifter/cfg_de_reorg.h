#ifndef LIFTER_CFG_DE_REORG_H_
#define LIFTER_CFG_DE_REORG_H_

#include <string_view>

#include "core/control_flow_graph.h"
#include "lifter/cfg_analysis_manager.h"
#include "lifter/cfg_pass_pipeline.h"

namespace rom_nom_nom {

// Inverse of GCC 2.7.2 `reorg.c:fill_simple_delay_slots`.
// Hoists same-block multi-instruction pairs (unaligned memory accesses `lwl`/`lwr`, `swl`/`swr` or
// multiply `mult` + `mflo`/`mfhi`) that were split across a branch or jump delay slot.
class SimpleDelaySlotUnschedulerPass : public CfgPass {
 public:
  std::string_view Name() const override { return "SimpleDelaySlotUnschedulerPass"; }
  bool Run(ControlFlowGraph* cfg, CfgAnalysisManager* analyses) override;
};

// Inverse of GCC 2.7.2 `reorg.c:fill_eager_delay_slots` (`fill_slots_from_thread`).
// Reconstructs target-stolen branch-likely (`bnezl`/`beqzl`/`bc1tl`/`bc1fl`) delay slots and
// fallthrough-stolen non-likely branch delay slots onto their destination blocks when the defined
// registers are dead along the opposite edge.
class EagerThreadDelaySlotUnschedulerPass : public CfgPass {
 public:
  std::string_view Name() const override { return "EagerThreadDelaySlotUnschedulerPass"; }
  bool Run(ControlFlowGraph* cfg, CfgAnalysisManager* analyses) override;
};

// Inverse of GCC 2.7.2 `reorg.c:relax_delay_slots`.
// Reconnects branches whose delay slot stole the first instruction of a shared target (`target +
// 4`) back to the original single-instruction header block.
class RelaxDelaySlotUnschedulerPass : public CfgPass {
 public:
  std::string_view Name() const override { return "RelaxDelaySlotUnschedulerPass"; }
  bool Run(ControlFlowGraph* cfg, CfgAnalysisManager* analyses) override;
};

// Inverse of GCC 2.7.2 `jump.c:find_cross_jump` / `do_cross_jump` (`toplev.c:3142`, post-reload
// `jump_optimize(..., cross_jump=1, ...)`).
class CrossJumpTailDuplicatorPass : public CfgPass {
 public:
  std::string_view Name() const override { return "CrossJumpTailDuplicatorPass"; }
  bool Run(ControlFlowGraph* cfg, CfgAnalysisManager* analyses) override;
};

// Orchestrates the inverse `reorg.c` + `jump2` CFG pass pipeline prior to dominator analysis, loop
// analysis, and region structuring.
class CfgDeReorg {
 public:
  static CfgPassPipeline BuildDefaultPipeline();
  static void UnscheduleDelaySlots(ControlFlowGraph* cfg, CfgAnalysisManager* analyses = nullptr);
};

}  // namespace rom_nom_nom

#endif  // LIFTER_CFG_DE_REORG_H_
