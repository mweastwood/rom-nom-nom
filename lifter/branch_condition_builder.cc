#include "lifter/branch_condition_builder.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "absl/container/flat_hash_set.h"
#include "absl/strings/str_cat.h"
#include "absl/types/span.h"
#include "core/c_ast.h"
#include "core/control_flow_graph.h"
#include "core/mips.h"
#include "core/register_tracker.h"
#include "lifter/abi_analyzer.h"

namespace rom_nom_nom {

namespace {

std::unique_ptr<CExpression> RegExpr(Register reg) {
  if (reg == Register::kZero) {
    return CExpression::Integer(0);
  }
  return CExpression::Identifier(AbiAnalyzer::RegisterVarName(reg));
}

bool IsFpuCompareOpcode(Opcode op) {
  return op == Opcode::kCEqS || op == Opcode::kCEqD || op == Opcode::kCSeqS ||
         op == Opcode::kCSeqD || op == Opcode::kCUeqS || op == Opcode::kCUeqD ||
         op == Opcode::kCNglS || op == Opcode::kCNglD || op == Opcode::kCUnS ||
         op == Opcode::kCUnD || op == Opcode::kCLtS || op == Opcode::kCLtD ||
         op == Opcode::kCOltS || op == Opcode::kCOltD || op == Opcode::kCUltS ||
         op == Opcode::kCUltD || op == Opcode::kCNgeS || op == Opcode::kCNgeD ||
         op == Opcode::kCLeS || op == Opcode::kCLeD || op == Opcode::kCOleS ||
         op == Opcode::kCOleD || op == Opcode::kCUleS || op == Opcode::kCUleD ||
         op == Opcode::kCNgtS || op == Opcode::kCNgtD || op == Opcode::kCNgleS ||
         op == Opcode::kCNgleD;
}

bool ExpressionReferencesIdentifier(const CExpression& expr, std::string_view name) {
  switch (expr.Kind()) {
    case CExpressionKind::kIdentifier:
      return static_cast<const IdentifierExpression&>(expr).Name() == name;
    case CExpressionKind::kUnaryExpression:
      return ExpressionReferencesIdentifier(static_cast<const UnaryExpression&>(expr).Operand(),
                                            name);
    case CExpressionKind::kBinaryExpression: {
      const auto& bin = static_cast<const BinaryExpression&>(expr);
      return ExpressionReferencesIdentifier(bin.Lhs(), name) ||
             ExpressionReferencesIdentifier(bin.Rhs(), name);
    }
    case CExpressionKind::kAssignmentExpression: {
      const auto& assign = static_cast<const AssignmentExpression&>(expr);
      return ExpressionReferencesIdentifier(assign.Lhs(), name) ||
             ExpressionReferencesIdentifier(assign.Rhs(), name);
    }
    case CExpressionKind::kCastExpression:
      return ExpressionReferencesIdentifier(static_cast<const CastExpression&>(expr).Operand(),
                                            name);
    case CExpressionKind::kTernaryExpression: {
      const auto& tern = static_cast<const TernaryExpression&>(expr);
      return ExpressionReferencesIdentifier(tern.Condition(), name) ||
             ExpressionReferencesIdentifier(tern.TrueExpression(), name) ||
             ExpressionReferencesIdentifier(tern.FalseExpression(), name);
    }
    case CExpressionKind::kCallExpression: {
      const auto& call = static_cast<const CallExpression&>(expr);
      if (ExpressionReferencesIdentifier(call.Callee(), name)) return true;
      for (const auto& arg : call.Arguments()) {
        if (arg != nullptr && ExpressionReferencesIdentifier(*arg, name)) return true;
      }
      return false;
    }
    default:
      return false;
  }
}

}  // namespace

std::optional<FoldedComparison> BranchConditionBuilder::FindFoldedComparison(
    absl::Span<const Instruction> instructions) {
  if (instructions.size() < 2) {
    return std::nullopt;
  }

  // 1. Find the terminating branch instruction (skipping trailing delay slot if present)
  int branch_idx = -1;
  for (int i = static_cast<int>(instructions.size()) - 1; i >= 0; --i) {
    if (instructions[i].IsBranch()) {
      branch_idx = i;
      break;
    }
  }

  if (branch_idx < 0) {
    return std::nullopt;
  }

  const auto& branch = instructions[branch_idx];
  if (branch.opcode == Opcode::kBc1t || branch.opcode == Opcode::kBc1tl ||
      branch.opcode == Opcode::kBc1f || branch.opcode == Opcode::kBc1fl) {
    int def_idx = -1;
    for (int i = branch_idx - 1; i >= 0; --i) {
      const auto& op = instructions[i].opcode;
      if (op == Opcode::kCEqS || op == Opcode::kCEqD || op == Opcode::kCSeqS ||
          op == Opcode::kCSeqD || op == Opcode::kCUeqS || op == Opcode::kCUeqD ||
          op == Opcode::kCNglS || op == Opcode::kCNglD || op == Opcode::kCUnS ||
          op == Opcode::kCUnD || op == Opcode::kCLtS || op == Opcode::kCLtD ||
          op == Opcode::kCOltS || op == Opcode::kCOltD || op == Opcode::kCUltS ||
          op == Opcode::kCUltD || op == Opcode::kCNgeS || op == Opcode::kCNgeD ||
          op == Opcode::kCLeS || op == Opcode::kCLeD || op == Opcode::kCOleS ||
          op == Opcode::kCOleD || op == Opcode::kCUleS || op == Opcode::kCUleD ||
          op == Opcode::kCNgtS || op == Opcode::kCNgtD || op == Opcode::kCNgleS ||
          op == Opcode::kCNgleD) {
        def_idx = i;
        break;
      }
      if (op == Opcode::kCtc1) {
        break;
      }
    }
    if (def_idx >= 0) {
      const auto& def_inst = instructions[def_idx];
      RegisterUseDef def_ud = GetInstructionUseDef(def_inst);
      bool hazard = false;
      for (int i = def_idx + 1; i < branch_idx; ++i) {
        RegisterUseDef mid_ud = GetInstructionUseDef(instructions[i]);
        for (FpRegister input_reg : def_ud.fpr_uses) {
          for (FpRegister mid_def : mid_ud.fpr_defs) {
            if (mid_def == input_reg) {
              hazard = true;
              break;
            }
          }
          if (hazard) break;
        }
        if (hazard) break;
      }
      if (!hazard) {
        FoldedComparison folded;
        folded.instruction_index = static_cast<size_t>(def_idx);
        folded.can_suppress_statement = true;
        return folded;
      }
    }
    return std::nullopt;
  }

  Register rs = branch.rs.value_or(Register::kZero);
  Register rt = branch.rt.value_or(Register::kZero);

  // 2. Identify condition register tested against zero
  std::optional<Register> cond_reg;
  switch (branch.opcode) {
    case Opcode::kBne:
    case Opcode::kBnel:
    case Opcode::kBeq:
    case Opcode::kBeql:
      if (rt == Register::kZero && rs != Register::kZero) {
        cond_reg = rs;
      } else if (rs == Register::kZero && rt != Register::kZero) {
        cond_reg = rt;
      }
      break;

    case Opcode::kBgtz:
    case Opcode::kBgtzl:
    case Opcode::kBgez:
    case Opcode::kBgezl:
    case Opcode::kBltz:
    case Opcode::kBltzl:
    case Opcode::kBlez:
    case Opcode::kBlezl:
      if (rs != Register::kZero) {
        cond_reg = rs;
      }
      break;

    default:
      break;
  }

  if (!cond_reg.has_value()) {
    return std::nullopt;
  }

  // 3. Scan backward from branch_idx - 1 for reaching definition of cond_reg
  int def_idx = -1;
  for (int i = branch_idx - 1; i >= 0; --i) {
    RegisterUseDef ud = GetInstructionUseDef(instructions[i]);
    bool defs_cond = false;
    for (Register r : ud.gpr_defs) {
      if (r == *cond_reg) {
        defs_cond = true;
        break;
      }
    }
    if (defs_cond) {
      def_idx = i;
      break;
    }
  }

  if (def_idx < 0) {
    return std::nullopt;
  }

  const auto& def_inst = instructions[def_idx];

  // Verify that def_inst is a comparison/test opcode that defines cond_reg
  switch (def_inst.opcode) {
    case Opcode::kSlt:
    case Opcode::kSltu:
    case Opcode::kXor:
    case Opcode::kAnd:
      if (!def_inst.rd.has_value() || *def_inst.rd != *cond_reg) {
        return std::nullopt;
      }
      break;

    case Opcode::kSlti:
    case Opcode::kSltiu:
    case Opcode::kXori:
    case Opcode::kAndi:
      if (!def_inst.rt.has_value() || *def_inst.rt != *cond_reg) {
        return std::nullopt;
      }
      break;

    default:
      return std::nullopt;
  }

  // 4. Hazard check: no instruction between def_idx and branch_idx clobbers inputs of def_inst,
  // or uses or clobbers cond_reg
  RegisterUseDef def_ud = GetInstructionUseDef(def_inst);
  for (int i = def_idx + 1; i < branch_idx; ++i) {
    RegisterUseDef mid_ud = GetInstructionUseDef(instructions[i]);
    for (Register input_reg : def_ud.gpr_uses) {
      for (Register mid_def : mid_ud.gpr_defs) {
        if (mid_def == input_reg) {
          return std::nullopt;
        }
      }
    }
    for (Register mid_use : mid_ud.gpr_uses) {
      if (mid_use == *cond_reg) {
        return std::nullopt;
      }
    }
    for (Register mid_def : mid_ud.gpr_defs) {
      if (mid_def == *cond_reg) {
        return std::nullopt;
      }
    }
  }

  // 5. Reject folding if a non-likely branch delay slot defines any input of def_inst (other than
  // cond_reg)
  if (!branch.IsBranchLikely() && branch_idx + 1 < static_cast<int>(instructions.size())) {
    RegisterUseDef delay_ud = GetInstructionUseDef(instructions[branch_idx + 1]);
    for (Register input_reg : def_ud.gpr_uses) {
      if (input_reg == *cond_reg) continue;
      for (Register delay_def : delay_ud.gpr_defs) {
        if (delay_def == input_reg) {
          return std::nullopt;
        }
      }
    }
  }

  // 6. Check if cond_reg is an input to def_inst (in-place modification / self-clobbering)
  bool cond_is_input = false;
  for (Register r : def_ud.gpr_uses) {
    if (r == *cond_reg) {
      cond_is_input = true;
      break;
    }
  }

  bool delay_clobbers_cond = false;
  if (branch_idx + 1 < static_cast<int>(instructions.size())) {
    const auto& delay = instructions[branch_idx + 1];
    if (delay.IsNop() && delay.unscheduled_clobber_reg == *cond_reg) {
      delay_clobbers_cond = true;
    } else {
      RegisterUseDef delay_ud = GetInstructionUseDef(delay);
      bool delay_uses_cond = false;
      for (Register r : delay_ud.gpr_uses) {
        if (r == *cond_reg) {
          delay_uses_cond = true;
          break;
        }
      }
      bool delay_defs_cond = false;
      for (Register r : delay_ud.gpr_defs) {
        if (r == *cond_reg) {
          delay_defs_cond = true;
          break;
        }
      }
      if (!delay_uses_cond && delay_defs_cond) {
        delay_clobbers_cond = true;
      }
    }
  }

  // If def_inst mutates cond_reg in place (cond_is_input) and cond_reg is not $at,
  // we can only fold and suppress def_inst if the delay slot immediately clobbers cond_reg.
  // Otherwise, def_inst is an in-place variable mutation that must be preserved as a
  // statement and cannot be folded into the branch condition.
  if (cond_is_input && *cond_reg != Register::kAt && !delay_clobbers_cond) {
    return std::nullopt;
  }

  // Determine whether emitting def_inst as a statement can be safely suppressed
  bool can_suppress = false;
  if (*cond_reg == Register::kAt || delay_clobbers_cond ||
      (*cond_reg == Register::kV1 &&
       (def_inst.opcode == Opcode::kSlt || def_inst.opcode == Opcode::kSltu ||
        def_inst.opcode == Opcode::kSlti || def_inst.opcode == Opcode::kSltiu))) {
    can_suppress = true;
  }

  return FoldedComparison{static_cast<size_t>(def_idx), can_suppress};
}

std::unique_ptr<CExpression> BranchConditionBuilder::ExtractBranchCondition(const BasicBlock& block,
                                                                            bool invert_condition) {
  // Find the branch instruction (skipping trailing delay slot if present)
  const Instruction* branch_inst = nullptr;
  for (auto it = block.instructions.rbegin(); it != block.instructions.rend(); ++it) {
    if (it->IsBranch()) {
      branch_inst = &(*it);
      break;
    }
  }

  if (branch_inst == nullptr) {
    return CExpression::Integer(invert_condition ? 0 : 1);
  }

  // Check if this block's branch folds a reaching comparison instruction
  auto folded_comp = FindFoldedComparison(block.instructions);
  if (folded_comp.has_value()) {
    const auto& def_inst = block.instructions[folded_comp->instruction_index];

    bool branch_taken_on_nonzero = false;
    switch (branch_inst->opcode) {
      case Opcode::kBne:
      case Opcode::kBnel:
      case Opcode::kBgtz:
      case Opcode::kBgtzl:
        branch_taken_on_nonzero = true;
        break;
      case Opcode::kBeq:
      case Opcode::kBeql:
      case Opcode::kBlez:
      case Opcode::kBlezl:
        branch_taken_on_nonzero = false;
        break;
      case Opcode::kBc1t:
      case Opcode::kBc1tl:
        branch_taken_on_nonzero = true;
        break;
      case Opcode::kBc1f:
      case Opcode::kBc1fl:
        branch_taken_on_nonzero = false;
        break;
      default:
        break;
    }

    bool want_true = branch_taken_on_nonzero ^ invert_condition;

    switch (def_inst.opcode) {
      case Opcode::kSlt: {
        std::string op = want_true ? "<" : ">=";
        Register def_rs = def_inst.rs.value_or(Register::kZero);
        Register def_rt = def_inst.rt.value_or(Register::kZero);
        return CExpression::Binary(op, RegExpr(def_rs), RegExpr(def_rt));
      }

      case Opcode::kSlti: {
        std::string op = want_true ? "<" : ">=";
        Register def_rs = def_inst.rs.value_or(Register::kZero);
        return CExpression::Binary(op, RegExpr(def_rs), CExpression::Integer(def_inst.immediate));
      }

      case Opcode::kSltu: {
        std::string op = want_true ? "<" : ">=";
        Register def_rs = def_inst.rs.value_or(Register::kZero);
        Register def_rt = def_inst.rt.value_or(Register::kZero);
        return CExpression::Binary(op, CExpression::Cast(CType::U32(), RegExpr(def_rs)),
                                   CExpression::Cast(CType::U32(), RegExpr(def_rt)));
      }

      case Opcode::kSltiu: {
        std::string op = want_true ? "<" : ">=";
        Register def_rs = def_inst.rs.value_or(Register::kZero);
        uint32_t uimm = static_cast<uint32_t>(def_inst.immediate);
        return CExpression::Binary(
            op, CExpression::Cast(CType::U32(), RegExpr(def_rs)),
            CExpression::Integer(uimm, /*is_hex=*/uimm > 0xFFFF, /*is_unsigned=*/true));
      }

      case Opcode::kXor: {
        std::string op = want_true ? "!=" : "==";
        Register def_rs = def_inst.rs.value_or(Register::kZero);
        Register def_rt = def_inst.rt.value_or(Register::kZero);
        return CExpression::Binary(op, RegExpr(def_rs), RegExpr(def_rt));
      }

      case Opcode::kXori: {
        std::string op = want_true ? "!=" : "==";
        Register def_rs = def_inst.rs.value_or(Register::kZero);
        uint32_t uimm = def_inst.UnsignedImmediate();
        return CExpression::Binary(op, RegExpr(def_rs),
                                   CExpression::Integer(uimm, /*is_hex=*/uimm > 0xFFFF));
      }

      case Opcode::kAndi: {
        std::string op = want_true ? "!=" : "==";
        Register def_rs = def_inst.rs.value_or(Register::kZero);
        uint32_t uimm = def_inst.UnsignedImmediate();
        auto bit_and = CExpression::Binary("&", RegExpr(def_rs),
                                           CExpression::Integer(uimm, /*is_hex=*/uimm > 0xFFFF));
        return CExpression::Binary(op, std::move(bit_and), CExpression::Integer(0));
      }

      case Opcode::kAnd: {
        std::string op = want_true ? "!=" : "==";
        Register def_rs = def_inst.rs.value_or(Register::kZero);
        Register def_rt = def_inst.rt.value_or(Register::kZero);
        auto bit_and = CExpression::Binary("&", RegExpr(def_rs), RegExpr(def_rt));
        return CExpression::Binary(op, std::move(bit_and), CExpression::Integer(0));
      }

      case Opcode::kCEqS:
      case Opcode::kCEqD:
      case Opcode::kCSeqS:
      case Opcode::kCSeqD:
      case Opcode::kCUeqS:
      case Opcode::kCUeqD:
      case Opcode::kCNglS:
      case Opcode::kCNglD:
      case Opcode::kCUnS:
      case Opcode::kCUnD: {
        bool is_double = (def_inst.opcode == Opcode::kCEqD || def_inst.opcode == Opcode::kCSeqD ||
                          def_inst.opcode == Opcode::kCUeqD || def_inst.opcode == Opcode::kCNglD ||
                          def_inst.opcode == Opcode::kCUnD);
        std::string op = want_true ? "==" : "!=";
        return CExpression::Binary(op,
                                   CExpression::Identifier(AbiAnalyzer::FpRegisterVarName(
                                       def_inst.fs.value_or(FpRegister::kF0), is_double)),
                                   CExpression::Identifier(AbiAnalyzer::FpRegisterVarName(
                                       def_inst.ft.value_or(FpRegister::kF0), is_double)));
      }

      case Opcode::kCLtS:
      case Opcode::kCLtD:
      case Opcode::kCOltS:
      case Opcode::kCOltD:
      case Opcode::kCUltS:
      case Opcode::kCUltD:
      case Opcode::kCNgeS:
      case Opcode::kCNgeD: {
        bool is_double = (def_inst.opcode == Opcode::kCLtD || def_inst.opcode == Opcode::kCOltD ||
                          def_inst.opcode == Opcode::kCUltD || def_inst.opcode == Opcode::kCNgeD);
        auto expr = CExpression::Binary("<",
                                        CExpression::Identifier(AbiAnalyzer::FpRegisterVarName(
                                            def_inst.fs.value_or(FpRegister::kF0), is_double)),
                                        CExpression::Identifier(AbiAnalyzer::FpRegisterVarName(
                                            def_inst.ft.value_or(FpRegister::kF0), is_double)));
        if (!want_true) {
          return CExpression::Unary("!", std::move(expr));
        }
        return expr;
      }

      case Opcode::kCLeS:
      case Opcode::kCLeD:
      case Opcode::kCOleS:
      case Opcode::kCOleD:
      case Opcode::kCUleS:
      case Opcode::kCUleD:
      case Opcode::kCNgtS:
      case Opcode::kCNgtD:
      case Opcode::kCNgleS:
      case Opcode::kCNgleD: {
        bool is_double = (def_inst.opcode == Opcode::kCLeD || def_inst.opcode == Opcode::kCOleD ||
                          def_inst.opcode == Opcode::kCUleD || def_inst.opcode == Opcode::kCNgtD ||
                          def_inst.opcode == Opcode::kCNgleD);
        auto expr = CExpression::Binary("<=",
                                        CExpression::Identifier(AbiAnalyzer::FpRegisterVarName(
                                            def_inst.fs.value_or(FpRegister::kF0), is_double)),
                                        CExpression::Identifier(AbiAnalyzer::FpRegisterVarName(
                                            def_inst.ft.value_or(FpRegister::kF0), is_double)));
        if (!want_true) {
          return CExpression::Unary("!", std::move(expr));
        }
        return expr;
      }

      default:
        break;
    }
  }

  Register rs = branch_inst->rs.value_or(Register::kZero);
  Register rt = branch_inst->rt.value_or(Register::kZero);

  switch (branch_inst->opcode) {
    case Opcode::kBeq:
    case Opcode::kBeql: {
      std::string op = invert_condition ? "!=" : "==";
      return CExpression::Binary(op, RegExpr(rs), RegExpr(rt));
    }

    case Opcode::kBne:
    case Opcode::kBnel: {
      std::string op = invert_condition ? "==" : "!=";
      return CExpression::Binary(op, RegExpr(rs), RegExpr(rt));
    }

    case Opcode::kBgtz:
    case Opcode::kBgtzl: {
      std::string op = invert_condition ? "<=" : ">";
      return CExpression::Binary(op, RegExpr(rs), CExpression::Integer(0));
    }

    case Opcode::kBgez:
    case Opcode::kBgezl: {
      std::string op = invert_condition ? "<" : ">=";
      return CExpression::Binary(op, RegExpr(rs), CExpression::Integer(0));
    }

    case Opcode::kBltz:
    case Opcode::kBltzl: {
      std::string op = invert_condition ? ">=" : "<";
      return CExpression::Binary(op, RegExpr(rs), CExpression::Integer(0));
    }

    case Opcode::kBlez:
    case Opcode::kBlezl: {
      std::string op = invert_condition ? ">" : "<=";
      return CExpression::Binary(op, RegExpr(rs), CExpression::Integer(0));
    }

    case Opcode::kBc1t:
    case Opcode::kBc1tl:
      if (invert_condition) {
        return CExpression::Unary("!", CExpression::Identifier("fcond"));
      }
      return CExpression::Identifier("fcond");

    case Opcode::kBc1f:
    case Opcode::kBc1fl:
      if (invert_condition) {
        return CExpression::Identifier("fcond");
      }
      return CExpression::Unary("!", CExpression::Identifier("fcond"));

    default:
      break;
  }

  return CExpression::Integer(invert_condition ? 0 : 1);
}

bool BranchConditionBuilder::ShouldDecoupleBranchConditionDelaySlot(const BasicBlock& cond_block) {
  if (cond_block.instructions.size() < 2) {
    return false;
  }
  const auto* term = cond_block.Terminator();
  if (term == nullptr || !term->IsBranch() || term->IsBranchLikely()) {
    return false;
  }
  const auto& delay_inst = cond_block.instructions.back();
  if (delay_inst.IsNop()) {
    return false;
  }

  RegisterUseDef delay_ud = GetInstructionUseDef(delay_inst);
  const bool delay_modifies_fcond =
      IsFpuCompareOpcode(delay_inst.opcode) || delay_inst.opcode == Opcode::kCtc1;
  if (delay_ud.gpr_defs.empty() && delay_ud.fpr_defs.empty() && !delay_modifies_fcond) {
    return false;
  }

  auto extracted_cond = ExtractBranchCondition(cond_block, /*invert_condition=*/false);
  if (extracted_cond != nullptr) {
    if (delay_modifies_fcond && ExpressionReferencesIdentifier(*extracted_cond, "fcond")) {
      return true;
    }
    for (Register r : delay_ud.gpr_defs) {
      if (r != Register::kZero &&
          ExpressionReferencesIdentifier(*extracted_cond, AbiAnalyzer::RegisterVarName(r))) {
        return true;
      }
    }
    for (FpRegister f : delay_ud.fpr_defs) {
      std::string fp_name = AbiAnalyzer::FpRegisterVarName(f);
      if (ExpressionReferencesIdentifier(*extracted_cond, fp_name) ||
          ExpressionReferencesIdentifier(*extracted_cond, absl::StrCat(fp_name, "_d"))) {
        return true;
      }
    }
  }

  absl::flat_hash_set<Register> cond_gprs;
  absl::flat_hash_set<FpRegister> cond_fprs;
  if (term->rs.has_value() && *term->rs != Register::kZero) {
    cond_gprs.insert(*term->rs);
  }
  if (term->rt.has_value() && *term->rt != Register::kZero) {
    cond_gprs.insert(*term->rt);
  }
  auto folded_comp = FindFoldedComparison(cond_block.instructions);
  if (folded_comp.has_value() && folded_comp->instruction_index < cond_block.instructions.size()) {
    const auto& def_inst = cond_block.instructions[folded_comp->instruction_index];
    RegisterUseDef def_ud = GetInstructionUseDef(def_inst);
    for (Register r : def_ud.gpr_uses) {
      if (r != Register::kZero) {
        cond_gprs.insert(r);
      }
    }
    for (FpRegister f : def_ud.fpr_uses) {
      cond_fprs.insert(f);
    }
  }
  bool has_call_in_block = false;
  for (const auto& inst : cond_block.instructions) {
    if (inst.IsCall()) {
      has_call_in_block = true;
      break;
    }
  }
  for (Register r : delay_ud.gpr_defs) {
    if (cond_gprs.contains(r)) {
      return true;
    }
    if (has_call_in_block && r >= Register::kA0 && r <= Register::kA3) {
      return true;
    }
  }
  for (FpRegister f : delay_ud.fpr_defs) {
    if (cond_fprs.contains(f)) {
      return true;
    }
    if (has_call_in_block && f >= FpRegister::kF12 && f <= FpRegister::kF15) {
      return true;
    }
  }
  return false;
}

bool BranchConditionBuilder::ShouldDecoupleBranchConditionDelaySlot(const ControlFlowGraph& cfg,
                                                                    uint32_t cond_block_id) {
  const auto* cond_block = cfg.GetBlock(cond_block_id);
  if (cond_block == nullptr) {
    return false;
  }
  if (ShouldDecoupleBranchConditionDelaySlot(*cond_block)) {
    return true;
  }
  if (cond_block->instructions.size() < 2) {
    return false;
  }
  const auto* term = cond_block->Terminator();
  const auto& delay_inst = cond_block->instructions.back();
  if (term == nullptr || !term->IsBranch() || term->IsBranchLikely() || delay_inst.IsNop()) {
    return false;
  }
  bool only_condition_instructions = (cond_block->instructions.size() == 2);
  if (!only_condition_instructions && cond_block->instructions.size() == 3) {
    auto folded_comp = FindFoldedComparison(cond_block->instructions);
    if (folded_comp.has_value() && folded_comp->instruction_index == 0 &&
        folded_comp->can_suppress_statement) {
      only_condition_instructions = true;
    }
  }
  if (!only_condition_instructions) {
    return false;
  }
  RegisterUseDef delay_ud = GetInstructionUseDef(delay_inst);
  bool defines_arg_reg = false;
  for (Register r : delay_ud.gpr_defs) {
    if (r >= Register::kA0 && r <= Register::kA3) {
      defines_arg_reg = true;
      break;
    }
  }
  for (FpRegister f : delay_ud.fpr_defs) {
    if (f >= FpRegister::kF12 && f <= FpRegister::kF15) {
      defines_arg_reg = true;
      break;
    }
  }
  if (!defines_arg_reg) {
    return false;
  }
  bool pred_has_call = false;
  for (uint32_t pred_id : cond_block->predecessors) {
    const auto* pred_block = cfg.GetBlock(pred_id);
    if (pred_block != nullptr) {
      for (const auto& inst : pred_block->instructions) {
        if (inst.IsCall()) {
          pred_has_call = true;
          break;
        }
      }
    }
    if (pred_has_call) break;
  }
  if (pred_has_call) {
    for (uint32_t succ_id : cond_block->successors) {
      const auto* succ_block = cfg.GetBlock(succ_id);
      if (succ_block != nullptr) {
        for (const auto& inst : succ_block->instructions) {
          if (inst.IsCall()) {
            return true;
          }
        }
      }
    }
  }
  return false;
}

bool BranchConditionBuilder::ShouldDecoupleLoopConditionDelaySlot(const BasicBlock& cond_block,
                                                                  const BasicBlock* header_block) {
  if (cond_block.instructions.size() < 2) {
    return false;
  }
  const auto* term = cond_block.Terminator();
  if (term == nullptr || !term->IsBranch() || term->IsBranchLikely()) {
    return false;
  }
  const auto& delay_inst = cond_block.instructions.back();
  if (delay_inst.IsNop()) {
    return false;
  }

  RegisterUseDef delay_ud = GetInstructionUseDef(delay_inst);
  if (delay_ud.gpr_defs.empty()) {
    return false;
  }

  // Check: Are all registers defined by delay_inst killed at header_block before being used?
  if (header_block != nullptr) {
    bool all_killed = true;
    for (Register def_reg : delay_ud.gpr_defs) {
      if (def_reg == Register::kZero) continue;
      bool killed = false;
      for (size_t idx = 0; idx < header_block->instructions.size(); ++idx) {
        const auto& inst = header_block->instructions[idx];
        if (inst.IsCall()) {
          if (idx + 1 < header_block->instructions.size()) {
            RegisterUseDef delay_inst_ud =
                GetInstructionUseDef(header_block->instructions[idx + 1]);
            bool delay_used = false;
            for (Register u : delay_inst_ud.gpr_uses) {
              if (u == def_reg) {
                delay_used = true;
                break;
              }
            }
            if (delay_used) break;
            bool delay_defined = false;
            for (Register d : delay_inst_ud.gpr_defs) {
              if (d == def_reg) {
                delay_defined = true;
                break;
              }
            }
            if (delay_defined) {
              killed = true;
              break;
            }
          }
          RegisterUseDef call_ud = GetInstructionUseDef(inst);
          bool call_used = false;
          for (Register u : call_ud.gpr_uses) {
            if (u == def_reg) {
              call_used = true;
              break;
            }
          }
          if (call_used) break;
          if (def_reg == Register::kV0 || def_reg == Register::kV1 || def_reg == Register::kAt ||
              (def_reg >= Register::kT0 && def_reg <= Register::kT7) || def_reg == Register::kT8 ||
              def_reg == Register::kT9) {
            killed = true;
            break;
          }
          if (idx + 1 < header_block->instructions.size()) {
            ++idx;
          }
          continue;
        }
        RegisterUseDef inst_ud = GetInstructionUseDef(inst);
        bool used = false;
        for (Register u : inst_ud.gpr_uses) {
          if (u == def_reg) {
            used = true;
            break;
          }
        }
        if (used) {
          break;
        }
        bool defined = false;
        for (Register d : inst_ud.gpr_defs) {
          if (d == def_reg) {
            defined = true;
            break;
          }
        }
        if (defined) {
          killed = true;
          break;
        }
      }
      if (!killed) {
        all_killed = false;
        break;
      }
    }
    if (all_killed) {
      return true;
    }
  }

  return false;
}

bool BranchConditionBuilder::ShouldDecoupleLoopConditionDelaySlot(const ControlFlowGraph& cfg,
                                                                  uint32_t cond_block_id,
                                                                  uint32_t header_block_id) {
  const auto* cond_block = cfg.GetBlock(cond_block_id);
  if (cond_block == nullptr) {
    return false;
  }
  return ShouldDecoupleLoopConditionDelaySlot(*cond_block, cfg.GetBlock(header_block_id));
}

}  // namespace rom_nom_nom
