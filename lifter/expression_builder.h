#ifndef LIFTER_EXPRESSION_BUILDER_H_
#define LIFTER_EXPRESSION_BUILDER_H_

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/types/span.h"
#include "core/control_flow_graph.h"
#include "core/mips.h"
#include "core/register_tracker.h"
#include "core/symbol_registry.h"
#include "lifter/abi_analyzer.h"
#include "lifter/branch_condition_builder.h"
#include "lifter/symbol_folder.h"

namespace rom_nom_nom {

// Classification of lifted expression types.
enum class ExpressionKind {
  kIntegerLiteral,
  kVariable,
  kGlobalRef,
  kUnaryOp,
  kCast,
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
  std::unique_ptr<LiftedExpression> Clone() const;

  // Returns true if the integer literal should be formatted in hexadecimal.
  // Values with bit 31 set represent either 32-bit bitmasks, addresses, or sign-extended
  // negative numbers that must be formatted in hex to prevent unsigned decimal overflow in C89.
  // Explicitly requested hex values format as hex when multi-digit (> 9).
  static bool RequiresHex(uint32_t val, bool explicit_hex = false) {
    if ((val & 0x80000000u) != 0) {
      return true;
    }
    return explicit_hex && val > 9;
  }

  static std::unique_ptr<LiftedExpression> Integer(uint32_t val, bool hex = false);
  static std::unique_ptr<LiftedExpression> Variable(std::string name);
  static std::unique_ptr<LiftedExpression> GlobalRef(std::string name);
  static std::unique_ptr<LiftedExpression> Unary(std::string op,
                                                 std::unique_ptr<LiftedExpression> operand);
  static std::unique_ptr<LiftedExpression> Cast(std::string type,
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
  kUnhandled,
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

// Per-function and per-module context for lifting basic blocks and instruction sequences.
struct FunctionLiftContext {
  const SymbolIndex* symbol_index = nullptr;
  const GameConfig* game_config = nullptr;
  const absl::flat_hash_map<std::string, int>* function_parameter_counts = nullptr;
  const absl::flat_hash_map<std::string, int>* function_fp_parameter_counts = nullptr;
  uint32_t frame_size = 0;
  const absl::flat_hash_set<std::string>* functions_returning_v0 = nullptr;
  const absl::flat_hash_set<std::string>* functions_returning_f0 = nullptr;
  bool has_frame_pointer = false;
  bool has_calls = false;
  bool sp_address_taken = false;
  const absl::flat_hash_map<Register, int32_t>* saved_reg_offsets = nullptr;
  const absl::flat_hash_map<FpRegister, int32_t>* saved_fpr_offsets = nullptr;
  const std::array<std::optional<uint32_t>, 32>* initial_fpr_const_bits = nullptr;
  const RegisterTracker* initial_tracker = nullptr;
  const absl::flat_hash_map<uint32_t, CallReturnInfo>* call_return_info = nullptr;
  const absl::flat_hash_set<int32_t>* function_stack_stores = nullptr;
  const absl::flat_hash_set<Register>* float_parameters_in_general_registers = nullptr;
  const absl::flat_hash_set<int>* float_stack_param_slots = nullptr;
  const absl::flat_hash_set<int32_t>* float_local_stack_offsets = nullptr;
  uint32_t outgoing_args_size = 0;
  int32_t sp_buffer_size = 0;
  const ControlFlowGraph* cfg = nullptr;
  uint32_t current_block_id = 0;
};

// Lifts instructions within basic blocks into structured statements and expressions.
class ExpressionBuilder {
 public:
  using FoldedComparison = ::rom_nom_nom::FoldedComparison;

  ExpressionBuilder() = default;

  // Lifts a basic block into a sequence of high-level statements using a FunctionLiftContext.
  static std::vector<LiftedStatement> LiftBlock(const BasicBlock& block,
                                                const FunctionLiftContext& ctx,
                                                bool omit_last_instruction = false);

  // Lifts a basic block into a sequence of high-level statements.
  static std::vector<LiftedStatement> LiftBlock(
      const BasicBlock& block, const SymbolIndex* symbol_index = nullptr,
      const GameConfig* game_config = nullptr,
      const absl::flat_hash_map<std::string, int>* function_parameter_counts = nullptr,
      const absl::flat_hash_map<std::string, int>* function_fp_parameter_counts = nullptr,
      bool omit_last_instruction = false, uint32_t frame_size = 0,
      const absl::flat_hash_set<std::string>* functions_returning_v0 = nullptr,
      const absl::flat_hash_set<std::string>* functions_returning_f0 = nullptr,
      bool has_frame_pointer = false, bool has_calls = false, bool sp_address_taken = false,
      const absl::flat_hash_map<Register, int32_t>* saved_reg_offsets = nullptr,
      const absl::flat_hash_map<FpRegister, int32_t>* saved_fpr_offsets = nullptr,
      const std::array<std::optional<uint32_t>, 32>* initial_fpr_const_bits = nullptr,
      const RegisterTracker* initial_tracker = nullptr,
      const absl::flat_hash_map<uint32_t, CallReturnInfo>* call_return_info = nullptr,
      const absl::flat_hash_set<int32_t>* function_stack_stores = nullptr);

  // Lifts a raw sequence of instructions into high-level statements using a FunctionLiftContext.
  static std::vector<LiftedStatement> LiftInstructions(absl::Span<const Instruction> instructions,
                                                       const FunctionLiftContext& ctx,
                                                       bool omit_last_instruction = false);

  // Lifts a raw sequence of instructions into high-level statements.
  static std::vector<LiftedStatement> LiftInstructions(
      absl::Span<const Instruction> instructions, const SymbolIndex* symbol_index = nullptr,
      const GameConfig* game_config = nullptr,
      const absl::flat_hash_map<std::string, int>* function_parameter_counts = nullptr,
      const absl::flat_hash_map<std::string, int>* function_fp_parameter_counts = nullptr,
      bool omit_last_instruction = false, uint32_t frame_size = 0,
      const absl::flat_hash_set<std::string>* functions_returning_v0 = nullptr,
      const absl::flat_hash_set<std::string>* functions_returning_f0 = nullptr,
      bool has_frame_pointer = false, bool has_calls = false, bool sp_address_taken = false,
      const absl::flat_hash_map<Register, int32_t>* saved_reg_offsets = nullptr,
      const absl::flat_hash_map<FpRegister, int32_t>* saved_fpr_offsets = nullptr,
      const std::array<std::optional<uint32_t>, 32>* initial_fpr_const_bits = nullptr,
      const RegisterTracker* initial_tracker = nullptr,
      const absl::flat_hash_map<uint32_t, CallReturnInfo>* call_return_info = nullptr,
      const absl::flat_hash_set<int32_t>* function_stack_stores = nullptr);

  // Computes return types of calls across basic blocks in a CFG.
  static absl::flat_hash_map<uint32_t, CallReturnInfo> ComputeCallReturnInfo(
      const ControlFlowGraph& cfg, bool function_returns_v0 = false,
      bool function_returns_f0 = false);

  // Analyzes register usage across instructions to determine the number of parameters ($a0-$a3)
  // expected by a function (via use-before-definition).
  static int DetermineParameterCount(absl::Span<const Instruction> instructions);

  // Analyzes register usage across instructions to determine the number of float parameters ($f12,
  // $f14) expected by a function (via use-before-definition).
  static int DetermineFpParameterCount(absl::Span<const Instruction> instructions);

  // Analyzes the CFG to determine if the function returns a value in $v0.
  // Returns true if there is an unconsumed definition of $v0 that reaches a return instruction.
  static bool DetermineReturnsV0(
      const ControlFlowGraph& cfg, const SymbolIndex* symbol_index = nullptr,
      const absl::flat_hash_set<std::string>* functions_returning_v0 = nullptr);

  // Analyzes the CFG to determine if the function returns a float value in $f0.
  static bool DetermineReturnsF0(const ControlFlowGraph& cfg);
  static bool DetermineReturnsF0Double(const ControlFlowGraph& cfg);

  // Detects callee-saved registers ($s0-$s7, $fp) that are used before definition in the
  // function body (excluding standard prologue spills like `sw $sX, offset($sp)`).
  // These represent uninitialized local variables in the original compiled binary.
  static absl::flat_hash_set<Register> DetectUninitializedCalleeSavedRegisters(
      const ControlFlowGraph& cfg);

  // Maps an ABI register to its clean variable name (e.g. "v0", "arg0", "temp_t0").
  static std::string RegisterVarName(Register reg);

  // Maps a COP1 floating point register to its clean variable name (e.g. "f0", "f12", or "f0_d" if
  // double).
  static std::string FpRegisterVarName(FpRegister reg, bool is_double = false);

  // Maps a local stack frame offset to its variable name (e.g. "var_sp_20").
  static std::string StackVarName(int32_t offset);

  // Finds a comparison instruction in `instructions` that is folded into a subsequent conditional
  // branch. Returns std::nullopt if no instruction is folded.
  static std::optional<FoldedComparison> FindFoldedComparison(
      absl::Span<const Instruction> instructions);

  // Lifts a folded symbol access into one or more high-level statements.
  static void LiftFoldedAccess(const FoldedSymbolAccess& lo, const Instruction& inst,
                               RegisterTracker& tracker, std::vector<LiftedStatement>& statements);

  static std::unique_ptr<LiftedExpression> LiftRegisterOrConstant(Register reg,
                                                                  const RegisterTracker& tracker,
                                                                  bool has_frame_pointer = false);
};

}  // namespace rom_nom_nom

#endif  // LIFTER_EXPRESSION_BUILDER_H_
