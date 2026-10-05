#ifndef LIFTER_EXPRESSION_BUILDER_H_
#define LIFTER_EXPRESSION_BUILDER_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/types/span.h"
#include "core/mips.h"
#include "lifter/control_flow_graph.h"
#include "lifter/register_tracker.h"
#include "lifter/symbol_folder.h"
#include "splitter/symbol_registry.h"

namespace rom_nom_nom {

// Classification of lifted expression types.
enum class ExpressionKind {
  kIntegerLiteral,
  kVariable,
  kGlobalRef,
  kUnaryOp,
  kBinaryOp,
  kMemoryLoad,
  kFunctionCall,
};

// Represents an expression lifted from instructions.
struct LiftedExpression {
  ExpressionKind kind = ExpressionKind::kVariable;

  // For kIntegerLiteral
  uint32_t int_val = 0;
  bool is_hex = false;

  // For kVariable or kGlobalRef
  std::string name;

  // For kUnaryOp or kBinaryOp
  std::string op;

  // Sub-expressions:
  // - kUnaryOp: [0] = operand
  // - kBinaryOp: [0] = lhs, [1] = rhs
  // - kMemoryLoad: [0] = address expression
  // - kFunctionCall: arguments
  std::vector<std::unique_ptr<LiftedExpression>> args;

  std::string ToString() const;

  static std::unique_ptr<LiftedExpression> Integer(uint32_t val, bool hex = false);
  static std::unique_ptr<LiftedExpression> Variable(std::string name);
  static std::unique_ptr<LiftedExpression> GlobalRef(std::string name);
  static std::unique_ptr<LiftedExpression> Unary(std::string op,
                                                 std::unique_ptr<LiftedExpression> operand);
  static std::unique_ptr<LiftedExpression> Binary(std::string op,
                                                  std::unique_ptr<LiftedExpression> lhs,
                                                  std::unique_ptr<LiftedExpression> rhs);
  static std::unique_ptr<LiftedExpression> Load(std::string type,
                                                std::unique_ptr<LiftedExpression> addr);
  static std::unique_ptr<LiftedExpression> Call(
      std::string func_name, std::vector<std::unique_ptr<LiftedExpression>> arguments);
};

// Classification of lifted statements.
enum class StatementKind {
  kAssignment,
  kStore,
  kCall,
  kReturn,
};

// Represents a statement lifted from instructions.
struct LiftedStatement {
  StatementKind kind = StatementKind::kAssignment;

  // For kAssignment: destination variable name
  std::string destination_variable;

  // For kStore: destination memory address expression and store width/type (e.g. "s32", "s16",
  // "s8")
  std::unique_ptr<LiftedExpression> destination_address;
  std::string store_type = "s32";

  // Expression value:
  // - kAssignment: rhs
  // - kStore: value to store
  // - kCall: call expression
  // - kReturn: return value expression (optional)
  std::unique_ptr<LiftedExpression> expression;

  std::string ToString() const;
};

// Lifts instructions within basic blocks into structured statements and expressions.
class ExpressionBuilder {
 public:
  ExpressionBuilder() = default;

  // Lifts a basic block into a sequence of high-level statements.
  static std::vector<LiftedStatement> LiftBlock(
      const BasicBlock& block, const SymbolIndex* symbol_index = nullptr,
      const SplitConfig* split_config = nullptr,
      const absl::flat_hash_map<std::string, int>* function_parameter_counts = nullptr);

  // Lifts a raw sequence of instructions into high-level statements.
  static std::vector<LiftedStatement> LiftInstructions(
      absl::Span<const Instruction> instructions, const SymbolIndex* symbol_index = nullptr,
      const SplitConfig* split_config = nullptr,
      const absl::flat_hash_map<std::string, int>* function_parameter_counts = nullptr);

  // Analyzes register usage across instructions to determine the number of parameters ($a0-$a3)
  // expected by a function (via use-before-definition).
  static int DetermineParameterCount(absl::Span<const Instruction> instructions);

  // Analyzes the CFG to determine if the function returns a value in $v0.
  // Returns true if there is an unconsumed definition of $v0 that reaches a return instruction.
  static bool DetermineReturnsV0(const ControlFlowGraph& cfg);

  // Maps an ABI register to its clean variable name (e.g. "v0", "arg0", "temp_t0").
  static std::string RegisterVarName(Register reg);

  // Information about a comparison instruction folded into a subsequent conditional branch.
  struct FoldedComparison {
    size_t instruction_index = 0;
    bool can_suppress_statement = false;
  };

  // Finds a comparison instruction in `instructions` that is folded into a subsequent conditional
  // branch. Returns std::nullopt if no instruction is folded.
  static std::optional<FoldedComparison> FindFoldedComparison(
      absl::Span<const Instruction> instructions);

 private:
  static std::unique_ptr<LiftedExpression> LiftRegisterOrConstant(Register reg,
                                                                  const RegisterTracker& tracker);
};

}  // namespace rom_nom_nom

#endif  // LIFTER_EXPRESSION_BUILDER_H_
