#include "lifter/cfg_pass_pipeline.h"

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "core/control_flow_graph.h"
#include "core/mips.h"
#include "gtest/gtest.h"
#include "lifter/cfg_analysis_manager.h"

namespace rom_nom_nom {
namespace {

TEST(CfgAnalysisManagerTest, ComputesAndCachesDominatorsLoopsAndLiveness) {
  // 0x80000000: addiu $v0, $a0, 1
  // 0x80000004: beq $v0, $zero, 0x80000014
  // 0x80000008: nop
  // 0x8000000C: addiu $v0, $v0, 2
  // 0x80000010: nop
  // 0x80000014: jr $ra
  // 0x80000018: nop
  std::vector<uint32_t> words = {
      0x24820001,  // addiu $v0, $a0, 1
      0x10400003,  // beq $v0, $zero, 0x80000014
      0x00000000,  // nop
      0x24420002,  // addiu $v0, $v0, 2
      0x00000000,  // nop
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);

  CfgAnalysisManager analyses(&cfg);
  EXPECT_FALSE(analyses.HasCachedDominatorTree());
  EXPECT_FALSE(analyses.HasCachedPostDominatorTree());
  EXPECT_FALSE(analyses.HasCachedLoopInfo());
  EXPECT_FALSE(analyses.HasCachedLivenessInfo());

  const auto& dom = analyses.GetDominatorTree();
  EXPECT_TRUE(analyses.HasCachedDominatorTree());
  EXPECT_TRUE(dom.Dominates(0, 1));
  EXPECT_TRUE(dom.Dominates(0, 2));

  const auto& liveness = analyses.GetLivenessInfo();
  EXPECT_TRUE(analyses.HasCachedLivenessInfo());
  EXPECT_TRUE(liveness.IsGprLiveIn(0, Register::kA0));
  EXPECT_TRUE(liveness.IsGprLiveOut(0, Register::kV0));
  EXPECT_TRUE(liveness.IsGprLiveIn(1, Register::kV0));

  analyses.InvalidateAll();
  EXPECT_FALSE(analyses.HasCachedDominatorTree());
  EXPECT_FALSE(analyses.HasCachedLivenessInfo());
}

TEST(CfgPassPipelineTest, InvalidatesAnalysesOnlyWhenPassMutatesCfg) {
  std::vector<uint32_t> words = {
      0x24820001,  // addiu $v0, $a0, 1
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  auto insts = *DecodeSequence(words, 0x80000000);
  auto cfg = *ControlFlowGraph::Build(insts);

  class NoOpPass : public CfgPass {
   public:
    std::string_view Name() const override { return "NoOpPass"; }
    bool Run(ControlFlowGraph* /*cfg*/, CfgAnalysisManager* analyses) override {
      (void)analyses->GetDominatorTree();
      return false;
    }
  };

  class MutatingPass : public CfgPass {
   public:
    explicit MutatingPass(int* runs) : runs_(runs) {}
    std::string_view Name() const override { return "MutatingPass"; }
    bool Run(ControlFlowGraph* /*cfg*/, CfgAnalysisManager* analyses) override {
      (void)analyses->GetDominatorTree();
      ++(*runs_);
      return *runs_ == 1;
    }

   private:
    int* runs_;
  };

  CfgAnalysisManager analyses(&cfg);
  CfgPassPipeline noop_pipeline;
  noop_pipeline.AddPass(std::make_unique<NoOpPass>());
  EXPECT_FALSE(noop_pipeline.Run(&cfg, &analyses));
  EXPECT_TRUE(analyses.HasCachedDominatorTree());

  int runs = 0;
  CfgPassPipeline mutating_pipeline;
  mutating_pipeline.AddPass(std::make_unique<MutatingPass>(&runs));
  EXPECT_TRUE(mutating_pipeline.RunToFixedPoint(&cfg, &analyses, /*max_iterations=*/3));
  EXPECT_EQ(runs, 2);
}

}  // namespace
}  // namespace rom_nom_nom
