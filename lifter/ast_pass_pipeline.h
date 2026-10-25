#ifndef LIFTER_AST_PASS_PIPELINE_H_
#define LIFTER_AST_PASS_PIPELINE_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/types/span.h"
#include "core/c_ast.h"

namespace rom_nom_nom {

// Context provided to AST post-processing passes.
struct AstPassContext {
  const std::vector<CParameter>& parameters;
  uint32_t frame_size = 0;
  bool has_frame_pointer = false;
  CType return_type = CType::Void();
  bool has_goto_targets = false;
  const absl::flat_hash_map<std::string, int>* function_parameter_counts = nullptr;
  std::vector<CParameter>* mutable_parameters = nullptr;
};

// Descriptor for a single AST transformation pass.
struct AstPass {
  std::string_view name;
  void (*run)(CompoundStatement* body, const AstPassContext& ctx) = nullptr;
};

// Orchestrates post-structuring C AST rewrite passes with bounded fixed-point convergence.
class AstPassPipeline {
 public:
  static constexpr int kDefaultMaxIterations = 4;

  // Runs `passes` in a loop until a full iteration produces no AST change or `max_iterations` is
  // reached. Returns the number of iterations executed.
  static int RunToFixedPoint(CompoundStatement* body, const AstPassContext& ctx,
                             absl::Span<const AstPass> passes,
                             int max_iterations = kDefaultMaxIterations);

  // Runs the full post-structuring AST optimization pipeline on `body`.
  static void Run(CompoundStatement* body, const AstPassContext& ctx);
};

}  // namespace rom_nom_nom

#endif  // LIFTER_AST_PASS_PIPELINE_H_
