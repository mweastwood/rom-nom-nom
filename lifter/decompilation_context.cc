#include "lifter/decompilation_context.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "core/control_flow_graph.h"
#include "core/jump_table.h"
#include "lifter/stack_frame_analyzer.h"

namespace rom_nom_nom {

StackFrameLayout::StackFrameLayout(StackFrameAnalysis analysis) : analysis_(std::move(analysis)) {}

StackFrameLayout StackFrameLayout::AnalyzeFromCfg(const ControlFlowGraph& cfg) {
  return StackFrameLayout(StackFrameAnalyzer::Analyze(cfg));
}

int32_t StackFrameLayout::FpRegSize() const {
  absl::flat_hash_set<int> even_fpr_pairs;
  for (const auto& [fpr, offset] : analysis_.prologue.saved_fpr_offsets) {
    (void)offset;
    even_fpr_pairs.insert(static_cast<int>(fpr) & ~1);
  }
  return static_cast<int32_t>(even_fpr_pairs.size() * 8);
}

bool StackFrameLayout::IsCalleeSavedSpillOffset(int32_t sp_offset) const {
  for (const auto& [reg, offset] : analysis_.prologue.saved_reg_offsets) {
    if (offset == sp_offset) return true;
  }
  for (const auto& [fpr, offset] : analysis_.prologue.saved_fpr_offsets) {
    if (offset == sp_offset || offset + 4 == sp_offset) return true;
  }
  return false;
}

DecompilationContext::DecompilationContext(const LoadedFunction& loaded_function,
                                           InterProceduralAbiContext abi_context)
    : loaded_function_(loaded_function), abi_context_(abi_context) {}

absl::Status DecompilationContext::BuildCfg() {
  if (loaded_function_.instructions.empty()) {
    return absl::InvalidArgumentError("DecompilationContext: No instructions to decompile.");
  }
  jump_tables_.clear();
  if (loaded_function_.memory_reader) {
    jump_tables_ = JumpTableDetector::DetectJumpTables(loaded_function_.instructions,
                                                       loaded_function_.memory_reader);
  }
  auto cfg_or = ControlFlowGraph::Build(loaded_function_.instructions, jump_tables_);
  if (!cfg_or.ok()) {
    return cfg_or.status();
  }
  cfg_ = std::move(*cfg_or);
  cfg_analyses_.SetCfg(&(*cfg_));
  cfg_analyses_.InvalidateAll();
  frame_layout_.reset();
  return absl::OkStatus();
}

const StackFrameLayout& DecompilationContext::FrameLayout() {
  if (!frame_layout_.has_value() && cfg_.has_value()) {
    frame_layout_ = StackFrameLayout::AnalyzeFromCfg(*cfg_);
  }
  return *frame_layout_;
}

}  // namespace rom_nom_nom
