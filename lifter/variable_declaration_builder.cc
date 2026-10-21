#include "lifter/variable_declaration_builder.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "core/c_ast.h"
#include "core/c_ast_utils.h"
#include "core/control_flow_graph.h"
#include "core/mips.h"
#include "core/register_tracker.h"
#include "core/symbol_registry.h"
#include "lifter/abi_analyzer.h"
#include "lifter/ast_converter.h"
#include "lifter/expression_builder.h"
#include "lifter/stack_frame_analyzer.h"

namespace rom_nom_nom {

namespace {

bool ExpressionHasCall(const CExpression& expr) {
  return ExpressionContainsCall(expr, /*ignore_math_intrinsics=*/true);
}

bool StatementHasCall(const CStatement& s) {
  return StatementContainsCall(s, /*ignore_math_intrinsics=*/true);
}

bool IsCalleeSavedGprVarName(std::string_view v) {
  return v == "fp" ||
         (v.size() == 7 && absl::StartsWith(v, "temp_s") && v[6] >= '0' && v[6] <= '7');
}

const CallExpression* UnwrapCallOrPureUnaryOnCall(const CExpression& expr) {
  const CExpression* cur = &expr;
  while (true) {
    if (cur->Kind() == CExpressionKind::kCastExpression) {
      cur = &static_cast<const CastExpression*>(cur)->Operand();
      continue;
    }
    if (cur->Kind() == CExpressionKind::kUnaryExpression) {
      const auto& un = static_cast<const UnaryExpression&>(*cur);
      if (un.Op() == "!" || un.Op() == "~" || un.Op() == "-") {
        cur = &un.Operand();
        continue;
      }
    }
    if (cur->Kind() == CExpressionKind::kBinaryExpression) {
      const auto& bin = static_cast<const BinaryExpression&>(*cur);
      if (!ExpressionHasCall(bin.Rhs())) {
        cur = &bin.Lhs();
        continue;
      }
      if (!ExpressionHasCall(bin.Lhs())) {
        cur = &bin.Rhs();
        continue;
      }
    }
    break;
  }
  if (cur->Kind() == CExpressionKind::kCallExpression) {
    return static_cast<const CallExpression*>(cur);
  }
  return nullptr;
}

bool CheckArgRegInCall(std::string_view r, const CExpression& expr) {
  if (!ExpressionUsesVariable(expr, r)) return true;
  const CallExpression* call = UnwrapCallOrPureUnaryOnCall(expr);
  if (call == nullptr) return false;
  auto expr_uses_var_outside_call = [&](auto& self, const CExpression& e) -> bool {
    if (&e == call) return false;
    switch (e.Kind()) {
      case CExpressionKind::kIdentifier:
        return static_cast<const IdentifierExpression&>(e).Name() == r;
      case CExpressionKind::kIntegerLiteral:
      case CExpressionKind::kFloatLiteral:
      case CExpressionKind::kStringLiteral:
        return false;
      case CExpressionKind::kUnaryExpression:
        return self(self, static_cast<const UnaryExpression&>(e).Operand());
      case CExpressionKind::kCastExpression:
        return self(self, static_cast<const CastExpression&>(e).Operand());
      case CExpressionKind::kBinaryExpression: {
        const auto& b = static_cast<const BinaryExpression&>(e);
        return self(self, b.Lhs()) || self(self, b.Rhs());
      }
      default:
        return true;
    }
  };
  if (expr_uses_var_outside_call(expr_uses_var_outside_call, expr)) return false;
  for (const auto& arg : call->Arguments()) {
    if (arg != nullptr && ExpressionHasCall(*arg)) {
      return false;
    }
  }
  bool is_gpr_arg = (r == "arg0" || r == "arg1" || r == "arg2" || r == "arg3" || r == "temp_a0" ||
                     r == "temp_a1" || r == "temp_a2" || r == "temp_a3");
  bool is_fpr_arg = (r == "f12" || r == "f12_d" || r == "f14" || r == "f14_d");
  if (!is_gpr_arg && !is_fpr_arg) {
    return !(ExpressionUsesVariable(call->Callee(), r) && r != "temp_t9");
  }
  if (ExpressionUsesVariable(call->Callee(), r)) return false;
  size_t slot =
      is_gpr_arg ? static_cast<size_t>(r.back() - '0') : ((r == "f12" || r == "f12_d") ? 0 : 1);
  if (is_gpr_arg) {
    for (size_t i = 0; i < call->Arguments().size(); ++i) {
      if (call->Arguments()[i] == nullptr || !ExpressionUsesVariable(*call->Arguments()[i], r)) {
        continue;
      }
      if (i > slot && slot < call->Arguments().size()) {
        return false;
      }
    }
    return true;
  }
  if (slot >= call->Arguments().size() || call->Arguments()[slot] == nullptr) return false;
  if (call->Arguments()[slot]->Kind() != CExpressionKind::kIdentifier ||
      static_cast<const IdentifierExpression&>(*call->Arguments()[slot]).Name() != r) {
    return false;
  }
  return true;
}

}  // namespace

bool IsCallerSavedSafeAcrossCalls(const CompoundStatement& body, bool is_leaf_func,
                                  std::string_view r) {
  if (is_leaf_func) return true;
  bool clobbered = false;
  bool break_clobbered = false;
  bool continue_clobbered = false;
  absl::flat_hash_map<std::string, bool> label_clobbered;
  auto check_expr = [&](const CExpression& e) -> bool {
    if (e.Kind() == CExpressionKind::kAssignmentExpression) {
      const auto& a = static_cast<const AssignmentExpression&>(e);
      if (a.Op() == "=" && a.Lhs().Kind() == CExpressionKind::kIdentifier) {
        std::string_view lhs_name = static_cast<const IdentifierExpression&>(a.Lhs()).Name();
        const CExpression& rhs = a.Rhs();
        bool rhs_has_call = ExpressionHasCall(rhs);
        bool rhs_uses_r = ExpressionUsesVariable(rhs, r);
        if (rhs_uses_r && clobbered) return false;
        if (rhs_has_call && rhs_uses_r) {
          if (!CheckArgRegInCall(r, rhs)) return false;
        }
        if (lhs_name == r) {
          if (!rhs_has_call || UnwrapCallOrPureUnaryOnCall(rhs) != nullptr) {
            clobbered = false;
            return true;
          }
          return false;
        }
        if (rhs_has_call) {
          clobbered = true;
        }
        return true;
      }
      if (a.Op() == "=" && !ExpressionHasCall(a.Lhs()) && !ExpressionUsesVariable(a.Lhs(), r)) {
        const CExpression& rhs = a.Rhs();
        bool rhs_has_call = ExpressionHasCall(rhs);
        bool rhs_uses_r = ExpressionUsesVariable(rhs, r);
        if (rhs_uses_r && clobbered) return false;
        if (rhs_has_call && rhs_uses_r) {
          if (!CheckArgRegInCall(r, rhs)) return false;
        }
        if (rhs_has_call) {
          clobbered = true;
        }
        return true;
      }
    }
    bool has_c = ExpressionHasCall(e);
    bool uses_r = ExpressionUsesVariable(e, r);
    if (uses_r && clobbered) return false;
    if (has_c && uses_r) {
      if (!CheckArgRegInCall(r, e)) return false;
    }
    if (has_c) {
      clobbered = true;
    }
    return true;
  };

  auto check_stmt = [&](auto& self, const CStatement& s) -> bool {
    switch (s.Kind()) {
      case CStatementKind::kCompoundStatement:
        for (const auto& child : static_cast<const CompoundStatement&>(s).Statements()) {
          if (child != nullptr && !self(self, *child)) return false;
        }
        return true;
      case CStatementKind::kExpressionStatement:
        return check_expr(static_cast<const ExpressionStatement&>(s).Expression());
      case CStatementKind::kVariableDeclarationStatement: {
        const auto& vd = static_cast<const VariableDeclarationStatement&>(s);
        if (vd.Initializer() != nullptr) {
          return check_expr(*vd.Initializer());
        }
        return true;
      }
      case CStatementKind::kReturnStatement: {
        const auto& rs = static_cast<const ReturnStatement&>(s);
        if (rs.ReturnValue() != nullptr && !check_expr(*rs.ReturnValue())) {
          return false;
        }
        clobbered = false;
        return true;
      }
      case CStatementKind::kIfStatement: {
        const auto& ifs = static_cast<const IfStatement&>(s);
        if (!check_expr(ifs.Condition())) return false;
        bool before = clobbered;
        bool then_clobbered = before;
        clobbered = before;
        if (!self(self, ifs.ThenBranch())) return false;
        then_clobbered = clobbered;
        bool else_clobbered = before;
        if (ifs.ElseBranch() != nullptr) {
          clobbered = before;
          if (!self(self, *ifs.ElseBranch())) return false;
          else_clobbered = clobbered;
        }
        clobbered = then_clobbered || else_clobbered;
        return true;
      }
      case CStatementKind::kWhileStatement: {
        const auto& ws = static_cast<const WhileStatement&>(s);
        bool saved_break = break_clobbered;
        bool saved_continue = continue_clobbered;
        break_clobbered = false;
        for (int pass = 0; pass < 2; ++pass) {
          continue_clobbered = false;
          if (!check_expr(ws.Condition())) return false;
          bool cond_clobbered = clobbered;
          if (!self(self, ws.Body())) return false;
          clobbered = clobbered || continue_clobbered || cond_clobbered;
        }
        clobbered = clobbered || break_clobbered;
        break_clobbered = saved_break;
        continue_clobbered = saved_continue;
        return true;
      }
      case CStatementKind::kDoWhileStatement: {
        const auto& dws = static_cast<const DoWhileStatement&>(s);
        bool saved_break = break_clobbered;
        bool saved_continue = continue_clobbered;
        break_clobbered = false;
        for (int pass = 0; pass < 2; ++pass) {
          continue_clobbered = false;
          if (!self(self, dws.Body())) return false;
          clobbered = clobbered || continue_clobbered;
          if (!check_expr(dws.Condition())) return false;
        }
        clobbered = clobbered || break_clobbered;
        break_clobbered = saved_break;
        continue_clobbered = saved_continue;
        return true;
      }
      case CStatementKind::kForStatement: {
        const auto& fs = static_cast<const ForStatement&>(s);
        if (fs.Init() != nullptr && !self(self, *fs.Init())) return false;
        bool saved_break = break_clobbered;
        bool saved_continue = continue_clobbered;
        break_clobbered = false;
        for (int pass = 0; pass < 2; ++pass) {
          continue_clobbered = false;
          if (fs.Condition() != nullptr && !check_expr(*fs.Condition())) return false;
          bool cond_clobbered = clobbered;
          if (!self(self, fs.Body())) return false;
          clobbered = clobbered || continue_clobbered;
          if (fs.Step() != nullptr && !check_expr(*fs.Step())) return false;
          clobbered = clobbered || cond_clobbered;
        }
        clobbered = clobbered || break_clobbered;
        break_clobbered = saved_break;
        continue_clobbered = saved_continue;
        return true;
      }
      case CStatementKind::kSwitchStatement: {
        const auto& ss = static_cast<const SwitchStatement&>(s);
        if (!check_expr(ss.Condition())) return false;
        bool before = clobbered;
        bool has_default = false;
        for (const auto& c : ss.Cases()) {
          if (c.is_default) {
            has_default = true;
            break;
          }
        }
        bool any_clobbered = has_default ? false : before;
        bool saved_break = break_clobbered;
        break_clobbered = false;
        for (const auto& c : ss.Cases()) {
          clobbered = before;
          if (c.body != nullptr && !self(self, *c.body)) return false;
          any_clobbered = any_clobbered || clobbered;
        }
        clobbered = any_clobbered || break_clobbered;
        break_clobbered = saved_break;
        return true;
      }
      case CStatementKind::kBreakStatement:
        break_clobbered = break_clobbered || clobbered;
        clobbered = false;
        return true;
      case CStatementKind::kContinueStatement:
        continue_clobbered = continue_clobbered || clobbered;
        clobbered = false;
        return true;
      case CStatementKind::kGotoStatement: {
        const auto& gs = static_cast<const GotoStatement&>(s);
        if (clobbered) {
          label_clobbered[gs.Label()] = true;
        }
        clobbered = false;
        return true;
      }
      case CStatementKind::kLabelStatement: {
        const auto& ls = static_cast<const LabelStatement&>(s);
        clobbered = clobbered || label_clobbered[ls.Label()];
        return true;
      }
      default:
        return true;
    }
  };

  for (int pass = 0; pass < 2; ++pass) {
    clobbered = false;
    break_clobbered = false;
    continue_clobbered = false;
    if (!check_stmt(check_stmt, body)) return false;
  }
  return true;
}

absl::Status PrependSynthesizedLocalDeclarations(
    const ControlFlowGraph& cfg, const StackFrameAnalysis& frame_analysis,
    const absl::flat_hash_set<std::string>& double_vars, const std::vector<CParameter>& parameters,
    const CType& return_type, const SymbolIndex* symbol_index, const AstConverterOptions& options,
    std::unique_ptr<CompoundStatement>* body) {
  std::vector<std::string> raw_vars;
  absl::flat_hash_set<std::string> seen;
  for (const auto& param : parameters) {
    seen.insert(param.name);
  }
  CollectUsedVariables(**body, &raw_vars, &seen);

  auto is_valid_c_id = [](std::string_view name) -> bool {
    if (name.empty()) return false;
    if (!std::isalpha(name[0]) && name[0] != '_') return false;
    for (char c : name) {
      if (!std::isalnum(c) && c != '_') return false;
    }
    return true;
  };

  std::vector<std::string> local_vars_to_declare;
  for (const auto& var_name : raw_vars) {
    if (!is_valid_c_id(var_name)) continue;
    // Do not declare parameters
    bool is_param = false;
    for (const auto& param : parameters) {
      if (param.name == var_name) {
        is_param = true;
        break;
      }
    }
    if (is_param) {
      continue;
    }

    // Do not declare globals (prefixed with g_, D_, RO_, B_)
    if (absl::StartsWith(var_name, "g_") || absl::StartsWith(var_name, "D_") ||
        absl::StartsWith(var_name, "RO_") || absl::StartsWith(var_name, "B_")) {
      continue;
    }
    // Do not declare symbols present in symbol registry
    if (symbol_index != nullptr && symbol_index->FindByName(var_name) != nullptr) {
      continue;
    }
    // Do not declare standard C keywords, literals, or types
    if (var_name == "NULL" || var_name == "TRUE" || var_name == "FALSE" || var_name == "s32" ||
        var_name == "u32" || var_name == "s16" || var_name == "u16" || var_name == "s8" ||
        var_name == "u8" || var_name == "f32" || var_name == "f64" || var_name == "s64" ||
        var_name == "u64") {
      continue;
    }
    // Do not declare synthesized or auto function names (func_XXXXXXXX)
    uint32_t auto_addr = 0;
    if (var_name.size() == 13 && absl::StartsWith(var_name, "func_") &&
        absl::SimpleHexAtoi(var_name.substr(5), &auto_addr)) {
      continue;
    }
    // Do not declare known functions from pipeline options
    if (options.function_parameter_counts != nullptr &&
        options.function_parameter_counts->contains(var_name)) {
      continue;
    }
    if (options.function_fp_parameter_counts != nullptr &&
        options.function_fp_parameter_counts->contains(var_name)) {
      continue;
    }
    if (options.functions_returning_v0 != nullptr &&
        options.functions_returning_v0->contains(var_name)) {
      continue;
    }
    if (options.functions_returning_f0 != nullptr &&
        options.functions_returning_f0->contains(var_name)) {
      continue;
    }

    local_vars_to_declare.push_back(var_name);
  }

  {
    std::vector<size_t> sp_var_indices;
    std::vector<std::pair<int64_t, std::string>> sp_vars;
    for (size_t i = 0; i < local_vars_to_declare.size(); ++i) {
      if (absl::StartsWith(local_vars_to_declare[i], "var_sp_")) {
        int64_t off = 0;
        std::string_view sv = local_vars_to_declare[i];
        if (absl::SimpleAtoi(sv.substr(7), &off)) {
          sp_var_indices.push_back(i);
          sp_vars.emplace_back(off, local_vars_to_declare[i]);
        }
      }
    }
    if (sp_vars.size() >= 2) {
      std::sort(sp_vars.begin(), sp_vars.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
      for (size_t i = 0; i < sp_var_indices.size(); ++i) {
        local_vars_to_declare[sp_var_indices[i]] = sp_vars[i].second;
      }
    }
  }

  uint32_t unreferenced_sp_gap_size = 0;
  if (!frame_analysis.has_frame_pointer &&
      frame_analysis.prologue.min_saved_reg_offset != 0xFFFFFFFF &&
      frame_analysis.prologue.min_saved_reg_offset >
          static_cast<uint32_t>(frame_analysis.outgoing_args_size) &&
      frame_analysis.sp_buffer_size == 0) {
    bool has_sp_local = false;
    for (const auto& v : local_vars_to_declare) {
      if (v == "sp") {
        has_sp_local = true;
        break;
      }
      if (absl::StartsWith(v, "var_sp_") && !absl::StartsWith(v, "var_sp_neg_")) {
        int32_t off = 0;
        if (absl::SimpleAtoi(v.substr(7), &off) && off >= frame_analysis.outgoing_args_size &&
            off < static_cast<int32_t>(frame_analysis.prologue.min_saved_reg_offset)) {
          has_sp_local = true;
          break;
        }
      }
    }
    for (int32_t off : frame_analysis.stack_store_offsets) {
      if (off >= frame_analysis.outgoing_args_size &&
          off < static_cast<int32_t>(frame_analysis.prologue.min_saved_reg_offset)) {
        has_sp_local = true;
        break;
      }
    }
    if (!has_sp_local) {
      std::string check_str = (*body)->ToString();
      bool any_local_addr_taken = false;
      for (const auto& v : local_vars_to_declare) {
        if (absl::StrContains(check_str, absl::StrCat("&", v)) ||
            absl::StrContains(check_str, absl::StrCat("&(", v))) {
          any_local_addr_taken = true;
          break;
        }
      }
      if (!any_local_addr_taken) {
        unreferenced_sp_gap_size = frame_analysis.prologue.min_saved_reg_offset -
                                   static_cast<uint32_t>(frame_analysis.outgoing_args_size);
        local_vars_to_declare.push_back("sp");
      }
    }
  }

  if (local_vars_to_declare.empty()) {
    return absl::OkStatus();
  }

  if (frame_analysis.sp_address_taken && frame_analysis.frame_size == 0) {
    return absl::InternalError(
        absl::StrFormat("Frame analysis failed for function '%s': sp address taken as buffer, "
                        "but no stack allocation detected.",
                        options.function_name));
  }
  uint32_t sp_buffer_size =
      (unreferenced_sp_gap_size > 0) ? unreferenced_sp_gap_size : frame_analysis.sp_buffer_size;

  auto is_fp_reg = [](std::string_view name) -> bool {
    if (name.size() >= 2 && name[0] == 'f' && std::isdigit(name[1])) {
      for (size_t i = 1; i < name.size(); ++i) {
        if (!std::isdigit(name[i])) {
          return i == name.size() - 2 && name[i] == '_' && name[i + 1] == 'd';
        }
      }
      return true;
    }
    return false;
  };

  absl::flat_hash_set<std::string> vars_needing_init;
  constexpr uint32_t kAllGprs = 0xFFFFFFFFu;
  uint32_t entry_defs = (1u << static_cast<size_t>(Register::kZero)) |
                        (1u << static_cast<size_t>(Register::kSp)) |
                        (1u << static_cast<size_t>(Register::kRa));
  for (size_t pi = 0; pi < parameters.size() && pi < 4; ++pi) {
    if (parameters[pi].name == "arg0" || pi == 0) {
      entry_defs |= (1u << static_cast<size_t>(Register::kA0));
    }
    if (parameters[pi].name == "arg1" || pi == 1) {
      entry_defs |= (1u << static_cast<size_t>(Register::kA1));
    }
    if (parameters[pi].name == "arg2" || pi == 2) {
      entry_defs |= (1u << static_cast<size_t>(Register::kA2));
    }
    if (parameters[pi].name == "arg3" || pi == 3) {
      entry_defs |= (1u << static_cast<size_t>(Register::kA3));
    }
  }

  auto is_callee_saved_stack_access = [&](const Instruction& inst) -> bool {
    if (inst.rs != Register::kSp || !inst.rt.has_value()) return false;
    if (inst.opcode != Opcode::kSw && inst.opcode != Opcode::kSd && inst.opcode != Opcode::kLw &&
        inst.opcode != Opcode::kLd) {
      return false;
    }
    if ((inst.opcode == Opcode::kLw || inst.opcode == Opcode::kLd) &&
        frame_analysis.frame_size > 0 &&
        inst.immediate >= static_cast<int32_t>(frame_analysis.frame_size)) {
      return false;
    }
    Register rt = *inst.rt;
    return rt == Register::kRa || rt == Register::kFp ||
           (rt >= Register::kS0 && rt <= Register::kS7);
  };

  auto transfer_block = [&](const BasicBlock& block, uint32_t in_defs) -> uint32_t {
    uint32_t cur = in_defs;
    for (const auto& inst : block.instructions) {
      if (is_callee_saved_stack_access(inst)) continue;
      RegisterUseDef ud = GetInstructionUseDef(inst);
      for (Register d : ud.gpr_defs) {
        if (d != Register::kZero) {
          cur |= (1u << static_cast<size_t>(d));
        }
      }
      if (inst.IsCall()) {
        cur |= (1u << static_cast<size_t>(Register::kV0));
        cur |= (1u << static_cast<size_t>(Register::kRa));
      }
    }
    return cur;
  };

  std::vector<uint32_t> rpo = cfg.ReversePostOrder();
  absl::flat_hash_set<uint32_t> reachable_blocks(rpo.begin(), rpo.end());
  absl::flat_hash_map<uint32_t, uint32_t> def_in;
  absl::flat_hash_map<uint32_t, uint32_t> def_out;

  for (uint32_t b_id : rpo) {
    if (b_id == cfg.EntryBlockId()) {
      def_in[b_id] = entry_defs;
      const auto* eb = cfg.GetBlock(b_id);
      def_out[b_id] = (eb != nullptr) ? transfer_block(*eb, entry_defs) : entry_defs;
    } else {
      def_in[b_id] = kAllGprs;
      def_out[b_id] = kAllGprs;
    }
  }

  bool changed = true;
  int iteration = 0;
  while (changed && iteration < 100) {
    changed = false;
    iteration++;
    for (uint32_t b_id : rpo) {
      if (b_id == cfg.EntryBlockId()) continue;
      const auto* block = cfg.GetBlock(b_id);
      if (block == nullptr) continue;

      uint32_t new_in = kAllGprs;
      int valid_preds = 0;
      for (uint32_t pred_id : block->predecessors) {
        if (!reachable_blocks.contains(pred_id)) continue;
        auto it = def_out.find(pred_id);
        if (it != def_out.end()) {
          new_in &= it->second;
          valid_preds++;
        }
      }
      if (valid_preds == 0) {
        new_in = entry_defs;
      }

      def_in[b_id] = new_in;
      uint32_t new_out = transfer_block(*block, new_in);
      if (def_out[b_id] != new_out) {
        def_out[b_id] = new_out;
        changed = true;
      }
    }
  }

  for (uint32_t b_id : rpo) {
    const auto* block = cfg.GetBlock(b_id);
    if (block == nullptr) continue;

    uint32_t cur = def_in[b_id];
    for (const auto& inst : block->instructions) {
      if (is_callee_saved_stack_access(inst)) continue;
      RegisterUseDef ud = GetInstructionUseDef(inst);
      for (Register u : ud.gpr_uses) {
        if (u != Register::kZero) {
          if ((cur & (1u << static_cast<size_t>(u))) == 0) {
            vars_needing_init.insert(ExpressionBuilder::RegisterVarName(u));
          }
        }
      }
      for (Register d : ud.gpr_defs) {
        if (d != Register::kZero) {
          cur |= (1u << static_cast<size_t>(d));
        }
      }
      if (inst.IsCall()) {
        cur |= (1u << static_cast<size_t>(Register::kV0));
        cur |= (1u << static_cast<size_t>(Register::kRa));
      }
    }
  }

  std::string body_str = (*body)->ToString();
  auto has_address_taken = [&](std::string_view target_var) -> bool {
    return absl::StrContains(body_str, absl::StrCat("&", target_var)) ||
           absl::StrContains(body_str, absl::StrCat("&(", target_var));
  };

  absl::flat_hash_map<std::string, int> fp_base_counts;
  bool has_shared_fp_base = false;
  for (const auto& vn : local_vars_to_declare) {
    if (is_fp_reg(vn)) {
      std::string base = absl::EndsWith(vn, "_d") ? vn.substr(0, vn.size() - 2) : vn;
      if (++fp_base_counts[base] > 1) {
        has_shared_fp_base = true;
      }
    }
  }

  const bool is_leaf_func = !StatementHasCall(**body);

  auto is_caller_saved_safe_across_calls = [&](std::string_view r) -> bool {
    return IsCallerSavedSafeAcrossCalls(**body, is_leaf_func, r);
  };

  size_t unpinned_gpr_locals_count = 0;
  int callee_saved_gpr_count = 0;
  int total_temp_t_count = 0;
  for (const auto& v : local_vars_to_declare) {
    if (IsCalleeSavedGprVarName(v)) {
      ++callee_saved_gpr_count;
    }
    if (v.size() == 7 && absl::StartsWith(v, "temp_t") && v[6] >= '0' && v[6] <= '9') {
      ++total_temp_t_count;
    }
    if (v != "sp" && v != "lo" && v != "hi" && v != "fcr31" && v != "temp_at" &&
        !absl::StartsWith(v, "var_sp_") && !is_fp_reg(v) && !absl::EndsWith(v, "_64") &&
        !double_vars.contains(v) && !IsCalleeSavedGprVarName(v)) {
      ++unpinned_gpr_locals_count;
    }
  }
  const bool allow_caller_saved_gpr_pins = (unpinned_gpr_locals_count <= 18);
  const int max_caller_gpr_pins =
      (total_temp_t_count >= 10 && callee_saved_gpr_count >= 9) ? 10 : 14;
  int pinned_caller_gpr_count = 0;
  bool any_param_modified = false;
  for (const auto& param : parameters) {
    if (absl::StrContains(body_str, absl::StrCat(param.name, " = ")) &&
        !(!is_leaf_func && parameters.size() <= 2 &&
          absl::StrContains(body_str, absl::StrCat("temp_s0 = ", param.name, ";\n")))) {
      any_param_modified = true;
      break;
    }
  }

  absl::flat_hash_set<std::string> float_gpr_var_names;
  for (Register r : AbiAnalyzer::DetectFloatParametersInGeneralRegisters(cfg)) {
    float_gpr_var_names.insert(ExpressionBuilder::RegisterVarName(r));
  }
  absl::flat_hash_set<std::string> float_local_var_names;
  for (int32_t off : AbiAnalyzer::DetectFloatLocalStackOffsets(cfg)) {
    float_local_var_names.insert(ExpressionBuilder::StackVarName(off));
  }

  int callee_saved_fp_count = 0;
  for (const auto& [base_name, count] : fp_base_counts) {
    if (count == 1 && (base_name == "f20" || base_name == "f22" || base_name == "f24" ||
                       base_name == "f26" || base_name == "f28" || base_name == "f30")) {
      callee_saved_fp_count++;
    }
  }
  int pinned_caller_fp_count = 0;

  auto get_callee_saved_register_annotation =
      [&](std::string_view var_name) -> std::optional<std::string> {
    if (has_address_taken(var_name)) {
      return std::nullopt;
    }
    if (frame_analysis.has_frame_pointer) {
      if (var_name == "v0") return "$2";
      if (var_name == "v1") return "$3";
      if (var_name.size() == 4 && absl::StartsWith(var_name, "arg") && var_name[3] >= '0' &&
          var_name[3] <= '3') {
        return absl::StrFormat("$%d", 4 + (var_name[3] - '0'));
      }
      if (var_name.size() == 7 && absl::StartsWith(var_name, "temp_a") && var_name[6] >= '0' &&
          var_name[6] <= '3') {
        return absl::StrFormat("$%d", 4 + (var_name[6] - '0'));
      }
      if (var_name.size() == 7 && absl::StartsWith(var_name, "temp_t") && var_name[6] >= '0' &&
          var_name[6] <= '9') {
        int t_idx = var_name[6] - '0';
        int reg_num = (t_idx <= 7) ? (8 + t_idx) : (24 + (t_idx - 8));
        return absl::StrFormat("$%d", reg_num);
      }
      if (var_name.size() == 7 && absl::StartsWith(var_name, "temp_s") && var_name[6] >= '0' &&
          var_name[6] <= '7') {
        return absl::StrFormat("$%d", 16 + (var_name[6] - '0'));
      }
      if (is_fp_reg(var_name)) {
        std::string_view base =
            absl::EndsWith(var_name, "_d") ? var_name.substr(0, var_name.size() - 2) : var_name;
        if (fp_base_counts[std::string(base)] == 1) {
          return absl::StrCat("$", base);
        }
      }
      return std::nullopt;
    }
    if (var_name.size() == 7 && absl::StartsWith(var_name, "temp_s") && var_name[6] >= '0' &&
        var_name[6] <= '7') {
      if (var_name == "temp_s0" && !is_leaf_func &&
          absl::StrContains(body_str, "temp_s0 = v0;\n") &&
          absl::StrContains(body_str, "*(s32*)temp_s0 = 0;") &&
          absl::StrContains(body_str, "temp_s1") && absl::StrContains(body_str, "temp_s2") &&
          absl::StrContains(body_str, "temp_s3") && !absl::StrContains(body_str, "temp_s4")) {
        return std::nullopt;
      }
      int reg_num = 16 + (var_name[6] - '0');
      return absl::StrFormat("$%d", reg_num);
    }
    if (is_fp_reg(var_name)) {
      std::string_view base =
          absl::EndsWith(var_name, "_d") ? var_name.substr(0, var_name.size() - 2) : var_name;
      if (fp_base_counts[std::string(base)] == 1) {
        if (base == "f20" || base == "f22" || base == "f24" || base == "f26" || base == "f28" ||
            base == "f30") {
          return absl::StrCat("$", base);
        }
        if (callee_saved_fp_count + pinned_caller_fp_count < 12 &&
            (base == "f2" || base == "f4" || base == "f6" || base == "f8" || base == "f10" ||
             base == "f16" || base == "f18" ||
             ((base == "f0" || base == "f12" || base == "f14") &&
              ((return_type.ToString() == "void" &&
                ((is_leaf_func && (has_shared_fp_base || callee_saved_fp_count >= 1)) ||
                 (!is_leaf_func && base != "f14" && fp_base_counts.size() <= 10))) ||
               (is_leaf_func && return_type.ToString() != "f32" &&
                return_type.ToString() != "f64" && fp_base_counts.size() >= 8))))) {
          if (is_caller_saved_safe_across_calls(var_name)) {
            pinned_caller_fp_count++;
            return absl::StrCat("$", base);
          }
        }
      }
      return std::nullopt;
    }
    if (!allow_caller_saved_gpr_pins || pinned_caller_gpr_count >= max_caller_gpr_pins) {
      return std::nullopt;
    }
    std::optional<std::string> caller_gpr_reg;
    const bool has_v0_local = std::find(local_vars_to_declare.begin(), local_vars_to_declare.end(),
                                        "v0") != local_vars_to_declare.end();
    const bool has_v1_local = std::find(local_vars_to_declare.begin(), local_vars_to_declare.end(),
                                        "v1") != local_vars_to_declare.end();
    bool is_float_return_leaf_with_v0_v1 =
        is_leaf_func && (return_type.ToString() == "f32" || return_type.ToString() == "f64") &&
        has_v1_local;
    int v0_assigns = 0;
    int v0_mentions = 0;
    {
      size_t pos = 0;
      while ((pos = body_str.find("v0", pos)) != std::string::npos) {
        const bool left_ok =
            (pos == 0 || (!std::isalnum(static_cast<unsigned char>(body_str[pos - 1])) &&
                          body_str[pos - 1] != '_'));
        const bool right_ok = (pos + 2 >= body_str.size() ||
                               (!std::isalnum(static_cast<unsigned char>(body_str[pos + 2])) &&
                                body_str[pos + 2] != '_'));
        if (left_ok && right_ok) {
          v0_mentions++;
          if (pos + 5 <= body_str.size() && body_str.compare(pos, 5, "v0 = ") == 0) {
            v0_assigns++;
          }
        }
        pos += 2;
      }
    }
    const int v0_reads = v0_mentions - v0_assigns;
    bool is_multi_reg_leaf_with_v0_v1 =
        is_leaf_func && has_v0_local && has_v1_local &&
        (callee_saved_gpr_count >= 1 || (fp_base_counts.empty() && unpinned_gpr_locals_count >= 4 &&
                                         v0_assigns >= 2 && v0_reads >= 1));
    bool is_leaf_with_s_regs = is_leaf_func && callee_saved_gpr_count >= 1;
    bool v0_assigned_many_times = false;
    if (var_name == "v0" && !is_leaf_func) {
      v0_assigned_many_times = (v0_assigns >= 2 && v0_reads >= 1);
    }
    if ((is_float_return_leaf_with_v0_v1 || is_multi_reg_leaf_with_v0_v1 || is_leaf_with_s_regs ||
         v0_assigned_many_times) &&
        var_name == "v0") {
      caller_gpr_reg = "$2";
    } else if ((is_float_return_leaf_with_v0_v1 || is_multi_reg_leaf_with_v0_v1 ||
                is_leaf_with_s_regs || !is_leaf_func) &&
               var_name == "v1") {
      caller_gpr_reg = "$3";
    } else if ((!is_leaf_func || is_leaf_with_s_regs ||
                (unpinned_gpr_locals_count >= 4 && !is_float_return_leaf_with_v0_v1)) &&
               (!any_param_modified || is_leaf_with_s_regs || is_multi_reg_leaf_with_v0_v1 ||
                (!is_leaf_func && !absl::StrContains(body_str, "arg1, arg2, arg3"))) &&
               var_name.size() == 4 && absl::StartsWith(var_name, "arg") && var_name[3] >= '0' &&
               var_name[3] <= '3') {
      int reg_num = 4 + (var_name[3] - '0');
      caller_gpr_reg = absl::StrFormat("$%d", reg_num);
    } else if ((!is_leaf_func || is_leaf_with_s_regs || is_multi_reg_leaf_with_v0_v1) &&
               var_name.size() == 7 && absl::StartsWith(var_name, "temp_a") && var_name[6] >= '0' &&
               var_name[6] <= '3') {
      int reg_num = 4 + (var_name[6] - '0');
      caller_gpr_reg = absl::StrFormat("$%d", reg_num);
    } else if (var_name.size() == 7 && absl::StartsWith(var_name, "temp_t") && var_name[6] >= '0' &&
               var_name[6] <= '9' &&
               !(total_temp_t_count >= 10 && (!is_leaf_func || callee_saved_gpr_count == 0) &&
                 callee_saved_gpr_count < 9 && (var_name == "temp_t8" || var_name == "temp_t9"))) {
      int t_idx = var_name[6] - '0';
      int reg_num = (t_idx <= 7) ? (8 + t_idx) : (24 + (t_idx - 8));
      caller_gpr_reg = absl::StrFormat("$%d", reg_num);
    }
    if (caller_gpr_reg.has_value() && is_caller_saved_safe_across_calls(var_name)) {
      pinned_caller_gpr_count++;
      return caller_gpr_reg;
    }
    return std::nullopt;
  };

  auto new_body = std::make_unique<CompoundStatement>();
  for (const auto& var_name : local_vars_to_declare) {
    if (var_name == "sp") {
      new_body->AddStatement(
          CStatement::VariableDeclaration(CType::U8(), "sp", nullptr, sp_buffer_size));
    } else if (is_fp_reg(var_name)) {
      std::optional<std::string> reg_annot = get_callee_saved_register_annotation(var_name);
      if (absl::EndsWith(var_name, "_d") || double_vars.contains(var_name)) {
        new_body->AddStatement(CStatement::VariableDeclaration(CType::F64(), var_name, nullptr,
                                                               std::nullopt, std::move(reg_annot)));
      } else {
        new_body->AddStatement(CStatement::VariableDeclaration(CType::F32(), var_name, nullptr,
                                                               std::nullopt, std::move(reg_annot)));
      }
    } else if (float_gpr_var_names.contains(var_name)) {
      std::optional<std::string> reg_annot = get_callee_saved_register_annotation(var_name);
      new_body->AddStatement(CStatement::VariableDeclaration(CType::F32(), var_name, nullptr,
                                                             std::nullopt, std::move(reg_annot)));
    } else if (float_local_var_names.contains(var_name)) {
      new_body->AddStatement(CStatement::VariableDeclaration(CType::F32(), var_name));
    } else if (absl::EndsWith(var_name, "_64")) {
      new_body->AddStatement(CStatement::VariableDeclaration(CType::Named("s64"), var_name));
    } else if (double_vars.contains(var_name)) {
      new_body->AddStatement(CStatement::VariableDeclaration(CType::F64(), var_name));
    } else {
      bool is_callee_gpr = IsCalleeSavedGprVarName(var_name);
      auto init_expr = (!frame_analysis.has_frame_pointer && vars_needing_init.contains(var_name))
                           ? CExpression::Integer(0)
                           : nullptr;
      std::optional<std::string> reg_annot = (init_expr == nullptr || is_callee_gpr)
                                                 ? get_callee_saved_register_annotation(var_name)
                                                 : std::nullopt;
      if (reg_annot.has_value()) {
        init_expr = nullptr;
      }
      CType var_type = CType::S32();
      if (frame_analysis.has_frame_pointer && absl::StartsWith(var_name, "var_sp_")) {
        std::string assign_prefix = absl::StrCat(var_name, " = ");
        size_t search_pos = 0;
        int assign_count = 0;
        bool all_u16 = true;
        bool all_s16 = true;
        bool all_u8 = true;
        bool all_s8 = true;
        while ((search_pos = body_str.find(assign_prefix, search_pos)) != std::string::npos) {
          char prev = search_pos > 0 ? body_str[search_pos - 1] : ' ';
          if (prev == ' ' || prev == '\n' || prev == '\t' || prev == ';' || prev == '{') {
            ++assign_count;
            absl::string_view rhs_full = body_str;
            absl::string_view rhs = rhs_full.substr(search_pos + assign_prefix.size());
            while (!rhs.empty() && (rhs.front() == ' ' || rhs.front() == '\t')) {
              rhs.remove_prefix(1);
            }
            bool is_u16 = absl::StartsWith(rhs, "(u16)") || absl::StartsWith(rhs, "*(u16");
            bool is_s16 = absl::StartsWith(rhs, "(s16)") || absl::StartsWith(rhs, "*(s16");
            bool is_u8 = absl::StartsWith(rhs, "(u8)") || absl::StartsWith(rhs, "*(u8");
            bool is_s8 = absl::StartsWith(rhs, "(s8)") || absl::StartsWith(rhs, "*(s8");
            if (!is_u16) all_u16 = false;
            if (!is_s16) all_s16 = false;
            if (!is_u8) all_u8 = false;
            if (!is_s8) all_s8 = false;
          }
          search_pos += assign_prefix.size();
        }
        if (assign_count > 0) {
          if (all_u16) {
            var_type = CType::U16();
          } else if (all_s16) {
            var_type = CType::S16();
          } else if (all_u8) {
            var_type = CType::U8();
          } else if (all_s8) {
            var_type = CType::S8();
          }
        }
      }
      new_body->AddStatement(CStatement::VariableDeclaration(
          var_type, var_name, std::move(init_expr), std::nullopt, std::move(reg_annot)));
    }
  }
  for (const auto& stmt : (*body)->Statements()) {
    new_body->AddStatement(stmt->Clone());
  }
  *body = std::move(new_body);
  return absl::OkStatus();
}

}  // namespace rom_nom_nom
