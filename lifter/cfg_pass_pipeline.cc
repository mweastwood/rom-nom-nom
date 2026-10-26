#include "lifter/cfg_pass_pipeline.h"

#include <memory>
#include <utility>

#include "core/control_flow_graph.h"
#include "lifter/cfg_analysis_manager.h"

namespace rom_nom_nom {

void CfgPassPipeline::AddPass(std::unique_ptr<CfgPass> pass) {
  if (pass != nullptr) {
    passes_.push_back(std::move(pass));
  }
}

bool CfgPassPipeline::Run(ControlFlowGraph* cfg, CfgAnalysisManager* analyses) const {
  if (cfg == nullptr) return false;
  CfgAnalysisManager local_analyses(cfg);
  if (analyses == nullptr) {
    analyses = &local_analyses;
  } else {
    analyses->SetCfg(cfg);
  }

  bool any_changed = false;
  for (const auto& pass : passes_) {
    if (pass->Run(cfg, analyses)) {
      analyses->InvalidateAll();
      any_changed = true;
    }
  }
  return any_changed;
}

bool CfgPassPipeline::RunToFixedPoint(ControlFlowGraph* cfg, CfgAnalysisManager* analyses,
                                      int max_iterations) const {
  if (cfg == nullptr || max_iterations <= 0) return false;
  bool any_changed = false;
  for (int iter = 0; iter < max_iterations; ++iter) {
    if (!Run(cfg, analyses)) {
      break;
    }
    any_changed = true;
  }
  return any_changed;
}

}  // namespace rom_nom_nom
