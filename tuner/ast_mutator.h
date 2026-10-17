#ifndef TUNER_AST_MUTATOR_H_
#define TUNER_AST_MUTATOR_H_

#include <string>
#include <vector>

#include "absl/types/span.h"
#include "core/c_ast.h"
#include "core/game_config.pb.h"

namespace rom_nom_nom {

// Discovers and applies semantics-preserving AST mutations to a `FunctionDeclaration`
// (`core/c_ast.h`) to explore GCC 2.7.2 register allocation, instruction selection, and branch
// polarity variations.
class AstMutator {
 public:
  // Enumerates all valid single-step AST mutations applicable to `function`.
  static std::vector<AstMutationStep> DiscoverMutations(const FunctionDeclaration& function,
                                                        size_t max_mutations = 64);

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
};

}  // namespace rom_nom_nom

#endif  // TUNER_AST_MUTATOR_H_
