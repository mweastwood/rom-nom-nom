#ifndef LIFTER_CFG_PASS_PIPELINE_H_
#define LIFTER_CFG_PASS_PIPELINE_H_

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

#include "core/control_flow_graph.h"
#include "lifter/cfg_analysis_manager.h"

namespace rom_nom_nom {

// Abstract base class for CFG-level transformation passes (e.g. delay-slot unscheduling and
// cross-jump tail duplication).
class CfgPass {
 public:
  virtual ~CfgPass() = default;
  virtual std::string_view Name() const = 0;

  // Executes the pass on `cfg`, querying cached analyses via `analyses` as needed.
  // Returns true if `cfg` was modified.
  virtual bool Run(ControlFlowGraph* cfg, CfgAnalysisManager* analyses) = 0;
};

// Orchestrates an ordered sequence of `CfgPass` objects, automatically invalidating cached
// analyses in `CfgAnalysisManager` whenever a pass reports a CFG mutation.
class CfgPassPipeline {
 public:
  CfgPassPipeline() = default;

  void AddPass(std::unique_ptr<CfgPass> pass);

  // Runs all registered passes once in order. Returns true if any pass modified `cfg`.
  bool Run(ControlFlowGraph* cfg, CfgAnalysisManager* analyses) const;

  // Runs all registered passes repeatedly up to `max_iterations` (or until a fixed point is
  // reached). Returns true if any pass modified `cfg`.
  bool RunToFixedPoint(ControlFlowGraph* cfg, CfgAnalysisManager* analyses,
                       int max_iterations = 2) const;

  size_t PassCount() const { return passes_.size(); }
  const std::vector<std::unique_ptr<CfgPass>>& Passes() const { return passes_; }

 private:
  std::vector<std::unique_ptr<CfgPass>> passes_;
};

}  // namespace rom_nom_nom

#endif  // LIFTER_CFG_PASS_PIPELINE_H_
