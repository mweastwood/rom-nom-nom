#ifndef LIFTER_AST_CONVERTER_H_
#define LIFTER_AST_CONVERTER_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "core/c_ast.h"
#include "core/mips.h"
#include "lifter/control_flow_graph.h"
#include "lifter/control_flow_structurer.h"
#include "lifter/expression_builder.h"
#include "splitter/symbol_registry.h"

namespace rom_nom_nom {

class SplitConfig;

// Configuration options for AST conversion.
struct AstConverterOptions {
  std::string function_name = "func";
  std::optional<CType> return_type;
  std::vector<CParameter> parameters;
  bool is_static = false;
  const SplitConfig* split_config = nullptr;
  const absl::flat_hash_map<std::string, int>* function_parameter_counts = nullptr;
};

// Converts structured control flow regions and lifted basic blocks into a C FunctionDeclaration
// AST.
class AstConverter {
 public:
  // Converts a structured CFG into a complete C FunctionDeclaration AST.
  static FunctionDeclaration Convert(const ControlFlowGraph& cfg,
                                     const StructuredRegion& root_region,
                                     const SymbolIndex* symbol_index = nullptr,
                                     const AstConverterOptions& options = {});

  // Converts a single LiftedExpression to a CExpression.
  static std::unique_ptr<CExpression> ConvertExpression(const LiftedExpression& expr);

  // Converts a single LiftedStatement to a CStatement.
  static std::unique_ptr<CStatement> ConvertStatement(const LiftedStatement& stmt);

  // Extracts the boolean condition CExpression from a basic block ending in a conditional branch.
  static std::unique_ptr<CExpression> ExtractBranchCondition(const BasicBlock& block,
                                                             bool invert_condition);
};

}  // namespace rom_nom_nom

#endif  // LIFTER_AST_CONVERTER_H_
