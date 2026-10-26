#ifndef LIFTER_AST_PASS_PIPELINE_H_
#define LIFTER_AST_PASS_PIPELINE_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
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
  bool returns_v0 = false;
  bool returns_f0 = false;
  CType return_type = CType::Void();
  bool has_goto_targets = false;
  const absl::flat_hash_map<std::string, int>* function_parameter_counts = nullptr;
  std::vector<CParameter>* mutable_parameters = nullptr;
};

// Polymorphic base class for post-structuring C AST transformation passes.
class AstPass {
 public:
  virtual ~AstPass() = default;

  // Human-readable pass identifier.
  virtual std::string_view Name() const = 0;

  // Executes the transformation pass on `body`. Returns true if the AST was modified.
  virtual bool Run(CompoundStatement* body, const AstPassContext& ctx) = 0;
};

// Adapter class that wraps a callable as an `AstPass`.
class FunctionAstPass : public AstPass {
 public:
  using Callback = std::function<void(CompoundStatement*, const AstPassContext&)>;

  FunctionAstPass(std::string name, Callback callback)
      : name_(std::move(name)), callback_(std::move(callback)) {}

  std::string_view Name() const override { return name_; }

  bool Run(CompoundStatement* body, const AstPassContext& ctx) override;

 private:
  std::string name_;
  Callback callback_;
};

// Composite `AstPass` that executes a group of child passes in a bounded fixed-point loop until
// an iteration produces no AST change or `max_iterations` is reached.
class FixedPointAstPassGroup : public AstPass {
 public:
  static constexpr int kDefaultMaxIterations = 4;

  explicit FixedPointAstPassGroup(std::string name, int max_iterations = kDefaultMaxIterations)
      : name_(std::move(name)), max_iterations_(max_iterations) {}

  void AddPass(std::unique_ptr<AstPass> pass);

  absl::Span<const std::unique_ptr<AstPass>> Passes() const { return passes_; }
  int LastIterationsExecuted() const { return last_iterations_executed_; }

  std::string_view Name() const override { return name_; }
  bool Run(CompoundStatement* body, const AstPassContext& ctx) override;

 private:
  std::string name_;
  int max_iterations_ = kDefaultMaxIterations;
  int last_iterations_executed_ = 0;
  std::vector<std::unique_ptr<AstPass>> passes_;
};

// Orchestrates post-structuring C AST rewrite passes with bounded fixed-point convergence.
class AstPassPipeline {
 public:
  static constexpr int kDefaultMaxIterations = FixedPointAstPassGroup::kDefaultMaxIterations;

  AstPassPipeline() = default;

  // Appends a pass to the pipeline.
  void AddPass(std::unique_ptr<AstPass> pass);

  absl::Span<const std::unique_ptr<AstPass>> Passes() const { return passes_; }

  // Executes all registered passes in sequence on `body`. Returns true if any pass modified `body`.
  bool RunOnBody(CompoundStatement* body, const AstPassContext& ctx);

  // Constructs the default post-structuring AST optimization pipeline.
  static AstPassPipeline BuildDefaultPipeline();

  // Runs `passes` in a loop until a full iteration produces no AST change or `max_iterations` is
  // reached. Returns the number of iterations executed.
  static int RunToFixedPoint(CompoundStatement* body, const AstPassContext& ctx,
                             absl::Span<AstPass* const> passes,
                             int max_iterations = kDefaultMaxIterations);

  // Runs the full post-structuring AST optimization pipeline on `body`.
  static void Run(CompoundStatement* body, const AstPassContext& ctx);

 private:
  std::vector<std::unique_ptr<AstPass>> passes_;
};

}  // namespace rom_nom_nom

#endif  // LIFTER_AST_PASS_PIPELINE_H_
