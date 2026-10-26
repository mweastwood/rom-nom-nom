#ifndef LIFTER_DECOMPILATION_CONTEXT_H_
#define LIFTER_DECOMPILATION_CONTEXT_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "core/c_ast.h"
#include "core/control_flow_graph.h"
#include "core/jump_table.h"
#include "core/mips.h"
#include "lifter/cfg_analysis_manager.h"
#include "lifter/control_flow_structurer.h"
#include "lifter/function_loader.h"
#include "lifter/stack_frame_analyzer.h"

namespace rom_nom_nom {

// Models GCC 2.7.2's MIPS stack frame geometry (`config/mips/mips.c:compute_frame_size`,
// `struct mips_frame_info`).
//
// In GCC 2.7.2, `compute_frame_size(size)` computes:
//   `args_size`     : outgoing argument build area `[0, args_size)` (at least 16 bytes when calls)
//   `var_size`      : local variable / stack buffer area `[args_size, args_size + var_size)`
//   `gp_reg_size`   : saved GPR area `[gp_sp_offset - gp_reg_size + 4, gp_sp_offset]`
//   `fp_reg_size`   : saved FPR area `[fp_sp_offset - fp_reg_size + 8, fp_sp_offset]`
//   `total_size`    : 8-byte aligned total frame size allocated via `addiu $sp, $sp, -total_size`
//   `caller_homing` : caller-allocated 16-byte register argument homing slots
//                     `[total_size, total_size + 16)`
class StackFrameLayout {
 public:
  StackFrameLayout() = default;
  explicit StackFrameLayout(StackFrameAnalysis analysis);

  static StackFrameLayout AnalyzeFromCfg(const ControlFlowGraph& cfg);

  int32_t TotalFrameSize() const { return analysis_.frame_size; }
  int32_t OutgoingArgsSize() const { return analysis_.outgoing_args_size; }
  int32_t GpRegSize() const {
    return static_cast<int32_t>(analysis_.prologue.saved_reg_offsets.size() * 4);
  }
  int32_t GpRegRounded() const { return (GpRegSize() + 7) & ~7; }
  int32_t FpRegSize() const;
  int32_t FpRegRounded() const { return (FpRegSize() + 7) & ~7; }
  int32_t LocalVarRegionStart() const { return analysis_.outgoing_args_size; }
  int32_t LocalVarRegionEnd() const {
    if (analysis_.prologue.min_saved_reg_offset != 0xFFFFFFFF &&
        static_cast<int32_t>(analysis_.prologue.min_saved_reg_offset) <= analysis_.frame_size) {
      return static_cast<int32_t>(analysis_.prologue.min_saved_reg_offset);
    }
    return analysis_.frame_size;
  }
  int32_t LocalVarSize() const {
    int32_t end = LocalVarRegionEnd();
    int32_t start = LocalVarRegionStart();
    return (end > start) ? (end - start) : 0;
  }
  // Exact `var_size` component in GCC 2.7.2 `compute_frame_size`:
  // `total_size = var_size + args_size + gp_reg_rounded + MIPS_STACK_ALIGN(fp_reg_size)`.
  int32_t ExactVarSize() const {
    if (analysis_.frame_size <= 0) return 0;
    int32_t start = (analysis_.outgoing_args_size + 7) & ~7;
    int32_t end = LocalVarRegionEnd() & ~7;
    return (end > start) ? (end - start) : 0;
  }
  int32_t SpBufferSize() const { return analysis_.sp_buffer_size; }
  int32_t CallerHomingStart() const { return analysis_.frame_size; }

  bool HasFramePointer() const { return analysis_.has_frame_pointer; }
  bool HasCalls() const { return analysis_.has_calls; }
  bool IsSpAddressTaken() const { return analysis_.sp_address_taken; }

  bool IsOutgoingArgOffset(int32_t sp_offset) const {
    return analysis_.has_calls && sp_offset >= 0 && sp_offset < analysis_.outgoing_args_size;
  }

  bool IsLocalVarOffset(int32_t sp_offset) const {
    return sp_offset >= LocalVarRegionStart() && sp_offset < LocalVarRegionEnd();
  }

  bool IsCalleeSavedSpillOffset(int32_t sp_offset) const;

  bool IsCallerHomedArgOffset(int32_t sp_offset) const {
    int32_t start = CallerHomingStart();
    return sp_offset >= start && sp_offset < start + 16;
  }

  const absl::flat_hash_map<Register, int32_t>& SavedGprOffsets() const {
    return analysis_.prologue.saved_reg_offsets;
  }
  const absl::flat_hash_map<FpRegister, int32_t>& SavedFprOffsets() const {
    return analysis_.prologue.saved_fpr_offsets;
  }

  const StackFrameAnalysis& RawAnalysis() const { return analysis_; }
  StackFrameAnalysis& MutableRawAnalysis() { return analysis_; }

 private:
  StackFrameAnalysis analysis_;
};

// Optional inter-procedural ABI tables shared across functions in a translation unit.
struct InterProceduralAbiContext {
  const absl::flat_hash_map<std::string, int>* function_parameter_counts = nullptr;
  const absl::flat_hash_map<std::string, int>* function_fp_parameter_counts = nullptr;
  const absl::flat_hash_set<std::string>* functions_returning_v0 = nullptr;
  const absl::flat_hash_set<std::string>* functions_returning_f0 = nullptr;
};

// First-class state object representing a single function's journey through the inverse-compiler
// pipeline (mirroring GCC 2.7.2's per-function state in `toplev.c:rest_of_compilation`).
class DecompilationContext {
 public:
  explicit DecompilationContext(const LoadedFunction& loaded_function,
                                InterProceduralAbiContext abi_context = {});

  const LoadedFunction& Function() const { return loaded_function_; }
  const std::string& FunctionName() const { return loaded_function_.name; }
  uint32_t Vram() const { return loaded_function_.vram; }
  const SymbolIndex* Symbols() const { return loaded_function_.symbol_index; }
  const GameConfig* Config() const { return loaded_function_.game_config; }
  const InterProceduralAbiContext& AbiContext() const { return abi_context_; }

  // Stage 1: Build the ControlFlowGraph and detect jump tables from raw instructions.
  absl::Status BuildCfg();

  bool HasCfg() const { return cfg_.has_value(); }
  const ControlFlowGraph& Cfg() const { return *cfg_; }
  ControlFlowGraph* MutableCfg() { return cfg_.has_value() ? &(*cfg_) : nullptr; }
  const std::vector<JumpTable>& JumpTables() const { return jump_tables_; }
  CfgAnalysisManager& CfgAnalyses() { return cfg_analyses_; }

  // Stage 2: Analyze or retrieve the GCC 2.7.2 stack frame layout (`mips.c:compute_frame_size`).
  const StackFrameLayout& FrameLayout();
  void InvalidateFrameLayout() { frame_layout_.reset(); }
  void InvalidateCfgAnalyses() {
    cfg_analyses_.InvalidateAll();
    frame_layout_.reset();
  }

  // Stage 3: Store and access the structured control-flow region hierarchy.
  bool HasRootRegion() const { return root_region_ != nullptr; }
  const StructuredRegion& RootRegion() const { return *root_region_; }
  StructuredRegion* MutableRootRegion() { return root_region_.get(); }
  void SetRootRegion(std::unique_ptr<StructuredRegion> root_region) {
    root_region_ = std::move(root_region);
  }

  // Stage 4: Store and access the lifted C AST `FunctionDeclaration`.
  bool HasAst() const { return ast_.has_value(); }
  const FunctionDeclaration& Ast() const { return *ast_; }
  FunctionDeclaration* MutableAst() { return ast_.has_value() ? &(*ast_) : nullptr; }
  void SetAst(FunctionDeclaration ast) { ast_ = std::move(ast); }
  std::unique_ptr<FunctionDeclaration> TakeAst() {
    if (!ast_.has_value()) return nullptr;
    auto ptr = std::make_unique<FunctionDeclaration>(std::move(*ast_));
    ast_.reset();
    return ptr;
  }

 private:
  const LoadedFunction& loaded_function_;
  InterProceduralAbiContext abi_context_;
  std::vector<JumpTable> jump_tables_;
  std::optional<ControlFlowGraph> cfg_;
  CfgAnalysisManager cfg_analyses_;
  std::optional<StackFrameLayout> frame_layout_;
  std::unique_ptr<StructuredRegion> root_region_;
  std::optional<FunctionDeclaration> ast_;
};

}  // namespace rom_nom_nom

#endif  // LIFTER_DECOMPILATION_CONTEXT_H_
