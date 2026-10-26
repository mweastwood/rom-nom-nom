#ifndef LIFTER_AST_TYPE_INFERENCE_PASS_H_
#define LIFTER_AST_TYPE_INFERENCE_PASS_H_

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/types/span.h"
#include "core/c_ast.h"
#include "core/control_flow_graph.h"
#include "core/symbol_registry.h"
#include "lifter/ast_converter.h"
#include "lifter/ast_pass_pipeline.h"

namespace rom_nom_nom {

// Infers function return flags, parameter lists, and return types from the CFG, ABI analysis, and
// lifted C AST body.
class FunctionSignatureInference {
 public:
  static void DetermineReturnFlags(const ControlFlowGraph& cfg, const SymbolIndex* symbol_index,
                                   const AstConverterOptions& options, bool* returns_v0,
                                   bool* returns_f0);

  static std::vector<CParameter> BuildFunctionParameters(const ControlFlowGraph& cfg,
                                                         const AstConverterOptions& options);

  static CType InferReturnType(const ControlFlowGraph& cfg, const AstConverterOptions& options,
                               bool returns_v0, bool returns_f0, const CompoundStatement& body);
};

// Infers narrow unsigned integer types (`u8` / `u16`) for conditionally-stepped local variables
// whose reads are exclusively masked by `0xFF` or `0xFFFF`, and rewrites `body` to strip redundant
// narrow masks and fold `(u32)((x & mask) ^ C) < 1` return idioms into `(x & mask) == C`.
absl::flat_hash_map<std::string, CType> InferAndPromoteNarrowCondSteppedVars(
    absl::Span<const std::string> candidate_vars, std::unique_ptr<CompoundStatement>* body);

// Concrete `AstPass` that promotes narrow conditional-step variables (`u8`/`u16`) and rewrites
// narrow return idioms and post-stepped masks in a `CompoundStatement`.
class ReturnAndGlobalTypePromotionPass : public AstPass {
 public:
  explicit ReturnAndGlobalTypePromotionPass(
      std::vector<std::string> candidate_vars = {},
      absl::flat_hash_map<std::string, CType>* out_promoted_types = nullptr)
      : candidate_vars_(std::move(candidate_vars)), out_promoted_types_(out_promoted_types) {}

  std::string_view Name() const override { return "ReturnAndGlobalTypePromotion"; }

  bool Run(CompoundStatement* body, const AstPassContext& ctx) override;

 private:
  std::vector<std::string> candidate_vars_;
  absl::flat_hash_map<std::string, CType>* out_promoted_types_ = nullptr;
};

// Concrete `AstPass` that partitions independent, non-overlapping basic-block-local def-use webs
// of unpinned caller-saved scratch registers (`v0`, `v1`, `temp_t0..temp_t9`, `temp_a0..temp_a3`,
// `temp_at`) into distinct local variables (`v0_2`, `temp_t0_2`, ...) so GCC 2.7.2 `local-alloc.c`
// (`block_alloc`) sees `reg_basic_block >= 0 && reg_n_deaths == 1` and allocates them in `.lreg`.
class ScratchRegisterWebSplitterPass : public AstPass {
 public:
  explicit ScratchRegisterWebSplitterPass(
      std::vector<std::string> candidate_vars = {},
      absl::flat_hash_map<std::string, std::vector<std::string>>* out_split_vars = nullptr)
      : candidate_vars_(std::move(candidate_vars)), out_split_vars_(out_split_vars) {}

  std::string_view Name() const override { return "ScratchRegisterWebSplitter"; }

  bool Run(CompoundStatement* body, const AstPassContext& ctx) override;

 private:
  std::vector<std::string> candidate_vars_;
  absl::flat_hash_map<std::string, std::vector<std::string>>* out_split_vars_ = nullptr;
};

}  // namespace rom_nom_nom

#endif  // LIFTER_AST_TYPE_INFERENCE_PASS_H_
