#ifndef LIFTER_ABI_ANALYZER_H_
#define LIFTER_ABI_ANALYZER_H_

#include <cstdint>
#include <string>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/types/span.h"
#include "core/control_flow_graph.h"
#include "core/mips.h"
#include "core/symbol_registry.h"

namespace rom_nom_nom {

// Information about return types and FP argument precision of calls within a function.
struct CallReturnInfo {
  bool returns_s64 = false;
  bool returns_f0 = false;
  bool returns_f0_double = false;
  bool returns_v0 = false;
  bool arg_f12_double = false;
  bool arg_f14_double = false;
};

// Analyzes N64 O32 ABI parameter liveness, return value conventions, and floating-point precision.
class AbiAnalyzer {
 public:
  // Returns true if the given GPR is used before definition on any path from the CFG entry block.
  static bool IsRegisterLiveIn(const ControlFlowGraph& cfg, Register reg);

  // Returns true if the given FPR is used before definition on any path from the CFG entry block.
  static bool IsFpRegisterLiveIn(const ControlFlowGraph& cfg, FpRegister reg);

  // Analyzes register and stack usage across instructions to determine the number of integer/stack
  // parameters expected by a function (via use-before-definition).
  static int DetermineParameterCount(absl::Span<const Instruction> instructions);

  // Analyzes register usage across instructions to determine the number of float parameters ($f12,
  // $f14) expected by a function (via use-before-definition).
  static int DetermineFpParameterCount(absl::Span<const Instruction> instructions);

  // Computes return types and FP argument widths of calls across basic blocks in a CFG.
  static absl::flat_hash_map<uint32_t, CallReturnInfo> ComputeCallReturnInfo(
      const ControlFlowGraph& cfg, bool function_returns_v0 = false,
      bool function_returns_f0 = false);

  // Returns true if `op` consumes its floating-point source operands as double-precision values.
  static bool IsDoublePrecisionUseOp(Opcode op);

  // Scans instructions starting at `start_idx` for post-call uses of `$v0`, `$v1`, and `$f0`.
  // When `stop_at_branches` is true, stops at unconditional jumps/returns (inspecting their delay
  // slot) in addition to subsequent calls.
  static CallReturnInfo ScanCallReturnUsage(absl::Span<const Instruction> instructions,
                                            size_t start_idx, bool stop_at_branches = false);

  // Identifies double-precision float variables used in the CFG.
  static absl::flat_hash_set<std::string> IdentifyDoubleVariables(const ControlFlowGraph& cfg);

  // Determines whether an FP parameter register (e.g. $f12 or $f14) expects double precision.
  static bool IsFpParameterDouble(const ControlFlowGraph& cfg, FpRegister reg);

  // Analyzes the CFG to determine if the function returns a value in $v0.
  static bool DetermineReturnsV0(
      const ControlFlowGraph& cfg, const SymbolIndex* symbol_index = nullptr,
      const absl::flat_hash_set<std::string>* functions_returning_v0 = nullptr);

  // Analyzes the CFG to determine if the function returns a float value in $f0.
  static bool DetermineReturnsF0(const ControlFlowGraph& cfg);
  static bool DetermineReturnsF0Double(const ControlFlowGraph& cfg);

  // Detects callee-saved registers ($s0-$s7, $fp) that are used before definition in the
  // function body.
  static absl::flat_hash_set<Register> DetectUninitializedCalleeSavedRegisters(
      const ControlFlowGraph& cfg);

  // Detects incoming function parameters in GPRs ($a1-$a3) and local carrier GPRs that represent
  // float values (e.g. when preceding parameters are integer/pointer or $f12 is used, and the
  // parameter is moved via mtc1 without integer use or cvt.s.w/cvt.d.w conversion).
  static absl::flat_hash_set<Register> DetectFloatParametersInGeneralRegisters(
      const ControlFlowGraph& cfg);

  // Detects incoming stack parameter slots (slot >= 4, corresponding to arg4+) that represent
  // single-precision float parameters (f32).
  static absl::flat_hash_set<int> DetectFloatStackParameterSlots(const ControlFlowGraph& cfg);

  // Detects local stack frame offsets (0 <= offset < frame_size) that are used exclusively as
  // single-precision float local variables (var_sp_<offset>).
  static absl::flat_hash_set<int32_t> DetectFloatLocalStackOffsets(const ControlFlowGraph& cfg);

  // Maps an ABI register to its clean variable name (e.g. "v0", "arg0", "temp_t0").
  static std::string RegisterVarName(Register reg);

  // Maps a COP1 floating point register to its clean variable name (e.g. "f0", "f12", or "f0_d" if
  // double).
  static std::string FpRegisterVarName(FpRegister reg, bool is_double = false);
};

}  // namespace rom_nom_nom

#endif  // LIFTER_ABI_ANALYZER_H_
