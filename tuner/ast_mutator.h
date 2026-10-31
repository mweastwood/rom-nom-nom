#ifndef TUNER_AST_MUTATOR_H_
#define TUNER_AST_MUTATOR_H_

#include <string>
#include <utility>
#include <vector>

#include "absl/types/span.h"
#include "core/c_ast.h"
#include "core/game_config.pb.h"

namespace rom_nom_nom {

struct DiffDiagnostic {
  // Relative positions [0.0, 1.0] in the function where non-exact instruction rows occur.
  std::vector<double> mismatch_relative_positions;

  // Ordered pairs of `(compiled_reg_annotation, target_reg_annotation)` observed on aligned
  // instructions with matching opcodes (e.g. `("$2", "$3")`, `("$8", "$9")`, `("$16", "$17")`).
  std::vector<std::pair<std::string, std::string>> transposed_register_pairs;

  // True if adjacent instructions in compiled vs target have transposed opcodes (scheduling swap).
  bool has_adjacent_insn_swap = false;

  // True if a branch/jump/return has a `nop` delay slot in compiled vs a non-nop instruction in
  // target (or vice versa).
  bool has_unfilled_delay_slot = false;

  // True if compiled has an extra or missing register-to-register move instruction.
  bool has_extra_or_missing_move = false;
};

// Discovers and applies semantics-preserving AST mutations to a `FunctionDeclaration`
// (`core/c_ast.h`) to explore GCC 2.7.2 register allocation, instruction selection, and branch
// polarity variations.
class AstMutator {
 public:
  // Enumerates all valid single-step AST mutations applicable to `function`. When `diff_diagnostic`
  // is provided, synthesizes targeted register-transposition mutations and ranks mutations by
  // symptom relevance and proximity to instruction diff hotspots.
  static std::vector<AstMutationStep> DiscoverMutations(
      const FunctionDeclaration& function, size_t max_mutations = 64,
      const DiffDiagnostic* diff_diagnostic = nullptr);

  // Returns multi-step canonicalization preset sequences to evaluate as candidate starting points.
  static std::vector<std::vector<AstMutationStep>> GenerateCanonicalPresets(
      const FunctionDeclaration& function);

  // Applies a single `AstMutationStep` to a clone of `function` and returns the mutated AST.
  static FunctionDeclaration ApplyMutation(const FunctionDeclaration& function,
                                           const AstMutationStep& step);

  // Applies a sequence of `AstMutationStep`s in order to a clone of `function`.
  static FunctionDeclaration ApplyMutations(const FunctionDeclaration& function,
                                            absl::Span<const AstMutationStep> steps);

  // Returns a human-readable description of a mutation step.
  static std::string DescribeStep(const AstMutationStep& step);

  // Prunes unused local declarations and dead variable assignments.
  static std::unique_ptr<CompoundStatement> PruneUnusedLocalDeclarations(
      const CompoundStatement& body, bool preserve_trailing_return = true);
};

}  // namespace rom_nom_nom

#endif  // TUNER_AST_MUTATOR_H_
