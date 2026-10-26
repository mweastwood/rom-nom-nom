#include "lifter/cop1_lifter.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/strings/str_format.h"
#include "core/mips.h"
#include "core/register_tracker.h"
#include "lifter/expression_builder.h"

namespace rom_nom_nom {
namespace {

std::unique_ptr<LiftedExpression> LiftFpRegister(FpRegister reg, bool is_double = false) {
  return LiftedExpression::Variable(ExpressionBuilder::FpRegisterVarName(reg, is_double));
}

bool IsSpOffset(const Cop1LiftState& state, int32_t offset) {
  if (!state.sp_address_taken) return false;
  if (offset < state.sp_base_offset) return false;
  if (state.sp_buffer_size > 0 && (offset - state.sp_base_offset) >= state.sp_buffer_size) {
    return false;
  }
  return true;
}

std::unique_ptr<LiftedExpression> MakeSpAddr(const Cop1LiftState& state, int32_t offset) {
  int32_t eff_offset = offset - state.sp_base_offset;
  if (eff_offset == 0) {
    return LiftedExpression::Unary("(s32)", LiftedExpression::Variable("sp"));
  }
  if (eff_offset > 0) {
    return LiftedExpression::Binary(
        "+", LiftedExpression::Unary("(s32)", LiftedExpression::Variable("sp")),
        LiftedExpression::Integer(eff_offset));
  }
  return LiftedExpression::Binary(
      "-", LiftedExpression::Unary("(s32)", LiftedExpression::Variable("sp")),
      LiftedExpression::Integer(-eff_offset));
}

std::unique_ptr<LiftedExpression> MakeRegOffsetExpr(const Cop1LiftState& state, Register r,
                                                    int16_t imm) {
  auto base = ExpressionBuilder::LiftRegisterOrConstant(r, state.tracker, state.has_frame_pointer);
  if (imm == 0) {
    return base;
  }
  if (imm < 0) {
    return LiftedExpression::Binary("-", std::move(base), LiftedExpression::Integer(-imm));
  }
  return LiftedExpression::Binary("+", std::move(base), LiftedExpression::Integer(imm));
}

bool IsActiveFloatGpr(const Cop1LiftState& state, Register r) {
  if (state.ctx.float_parameters_in_general_registers == nullptr ||
      !state.ctx.float_parameters_in_general_registers->contains(r)) {
    return false;
  }
  if (r == Register::kA0 || r == Register::kA1 || r == Register::kA2 || r == Register::kA3) {
    return !state.gprs_defined_in_block.contains(r);
  }
  return true;
}

}  // namespace

std::optional<uint32_t> ExtractMtc1ConstantBits(Register rt, const RegisterTracker& tracker) {
  if (rt == Register::kZero) {
    return 0u;
  }
  TrackedValue tv = tracker.GetRegisterValue(rt);
  if (tv.kind == ValueKind::kConstant || tv.kind == ValueKind::kSymbolAddress) {
    return tv.constant_value;
  }
  if (tv.kind == ValueKind::kSymbolHi) {
    return tv.symbol_hi;
  }
  return std::nullopt;
}

void InvalidateNonMtc1FprConstantBits(const Instruction& inst,
                                      std::optional<uint32_t>* fpr_const_bits) {
  if (inst.opcode == Opcode::kMtc1 || fpr_const_bits == nullptr) {
    return;
  }
  RegisterUseDef ud = GetInstructionUseDef(inst);
  for (FpRegister d : ud.fpr_defs) {
    int f_idx = static_cast<int>(d);
    fpr_const_bits[f_idx] = std::nullopt;
    if ((f_idx % 2) == 0 && f_idx + 1 < 32) {
      fpr_const_bits[f_idx + 1] = std::nullopt;
    }
  }
  if (inst.IsCall()) {
    for (int f = 0; f < 20; ++f) {
      fpr_const_bits[f] = std::nullopt;
    }
  }
}

void StepFprConstantBits(const Instruction& inst, const RegisterTracker& tracker,
                         std::optional<uint32_t>* fpr_const_bits) {
  if (fpr_const_bits == nullptr) {
    return;
  }
  if (inst.opcode == Opcode::kMtc1) {
    if (inst.fs.has_value() && inst.rt.has_value()) {
      fpr_const_bits[static_cast<int>(*inst.fs)] = ExtractMtc1ConstantBits(*inst.rt, tracker);
    }
    return;
  }
  InvalidateNonMtc1FprConstantBits(inst, fpr_const_bits);
}

void LiftCop1MemoryAndMoveInstruction(const Instruction& inst, size_t i, Cop1LiftState& state) {
  const auto& ctx = state.ctx;
  const auto& instructions_to_process = state.instructions_to_process;
  const size_t loop_end = state.loop_end;
  const bool has_frame_pointer = state.has_frame_pointer;
  const uint32_t frame_size = state.frame_size;
  auto& tracker = state.tracker;
  auto& skipped_instruction_indices = state.skipped_instruction_indices;
  auto* fpr_const_bits = state.fpr_const_bits;
  auto* pending_int_to_fp = state.pending_int_to_fp;
  auto* pending_fp_to_int = state.pending_fp_to_int;
  auto& statements = state.statements;

  switch (inst.opcode) {
    case Opcode::kLwc1:
      if (inst.ft.has_value() && inst.rs.has_value()) {
        bool is_stack_base =
            (*inst.rs == Register::kSp) || (has_frame_pointer && *inst.rs == Register::kFp);
        if (is_stack_base && ctx.saved_fpr_offsets != nullptr) {
          auto it = ctx.saved_fpr_offsets->find(*inst.ft);
          if (it != ctx.saved_fpr_offsets->end() && it->second == inst.immediate) {
            break;
          }
        }
        int ft_idx = static_cast<int>(*inst.ft);
        bool only_used_by_int_to_fp_cvt = false;
        for (size_t look = i + 1; look < loop_end; ++look) {
          const Instruction& future_inst = instructions_to_process[look];
          if ((future_inst.opcode == Opcode::kCvtSW || future_inst.opcode == Opcode::kCvtDW) &&
              future_inst.fs == *inst.ft) {
            bool reused_after_cvt = false;
            if (future_inst.fd != *inst.ft) {
              for (size_t k = look + 1; k < loop_end; ++k) {
                RegisterUseDef kud = GetInstructionUseDef(instructions_to_process[k]);
                for (FpRegister u : kud.fpr_uses) {
                  if (u == *inst.ft || (static_cast<int>(u) & ~1) == (ft_idx & ~1)) {
                    reused_after_cvt = true;
                    break;
                  }
                }
                if (reused_after_cvt) break;
                bool redefined = false;
                for (FpRegister d : kud.fpr_defs) {
                  if (d == *inst.ft || (static_cast<int>(d) & ~1) == (ft_idx & ~1)) {
                    redefined = true;
                    break;
                  }
                }
                if (redefined || instructions_to_process[k].IsCall()) break;
              }
            }
            only_used_by_int_to_fp_cvt = !reused_after_cvt;
            break;
          }
          RegisterUseDef fud = GetInstructionUseDef(future_inst);
          bool touches_ft_or_rs = false;
          for (FpRegister u : fud.fpr_uses) {
            if (u == *inst.ft || (static_cast<int>(u) & ~1) == (ft_idx & ~1)) {
              touches_ft_or_rs = true;
            }
          }
          for (FpRegister d : fud.fpr_defs) {
            if (d == *inst.ft || (static_cast<int>(d) & ~1) == (ft_idx & ~1)) {
              touches_ft_or_rs = true;
            }
          }
          for (Register d : fud.gpr_defs) {
            if (d == *inst.rs) {
              touches_ft_or_rs = true;
            }
          }
          if (touches_ft_or_rs || future_inst.IsCall() || future_inst.IsStore()) {
            break;
          }
        }
        if (only_used_by_int_to_fp_cvt) {
          if (is_stack_base) {
            int32_t offset = inst.immediate;
            if (IsSpOffset(state, offset)) {
              pending_int_to_fp[ft_idx] = LiftedExpression::Load("s32", MakeSpAddr(state, offset));
            } else {
              uint32_t base_offset = frame_size + 16;
              int arg_idx = -1;
              if (offset >= static_cast<int32_t>(base_offset)) {
                arg_idx = 4 + (offset - static_cast<int32_t>(base_offset)) / 4;
              }
              if (arg_idx >= 0) {
                pending_int_to_fp[ft_idx] =
                    LiftedExpression::Variable(absl::StrFormat("arg%d", arg_idx));
              } else {
                int32_t base_dword_offset = offset & ~7;
                int32_t byte_in_dword = offset & 7;
                if (!state.local_stack_word_stores.contains(offset) &&
                    state.local_stack_dword_stores.contains(base_dword_offset)) {
                  auto base_var = LiftedExpression::Variable(
                      ExpressionBuilder::StackVarName(base_dword_offset));
                  auto base_u64 = state.local_stack_fp_dword_stores.contains(base_dword_offset)
                                      ? LiftedExpression::Load("u64", LiftedExpression::Unary(
                                                                          "&", std::move(base_var)))
                                      : LiftedExpression::Unary("(u64)", base_var->Clone());
                  if (byte_in_dword == 4) {
                    pending_int_to_fp[ft_idx] = LiftedExpression::Unary(
                        "(s32)", state.local_stack_fp_dword_stores.contains(base_dword_offset)
                                     ? std::move(base_u64)
                                     : std::move(base_var));
                  } else {
                    pending_int_to_fp[ft_idx] = LiftedExpression::Unary(
                        "(s32)", LiftedExpression::Binary(
                                     ">>", std::move(base_u64),
                                     LiftedExpression::Integer((4 - byte_in_dword) * 8)));
                  }
                } else {
                  pending_int_to_fp[ft_idx] =
                      LiftedExpression::Variable(ExpressionBuilder::StackVarName(offset));
                }
              }
            }
          } else {
            pending_int_to_fp[ft_idx] =
                LiftedExpression::Load("s32", MakeRegOffsetExpr(state, *inst.rs, inst.immediate));
          }
          fpr_const_bits[ft_idx] = std::nullopt;
          break;
        }

        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = ExpressionBuilder::FpRegisterVarName(*inst.ft);
        if (is_stack_base) {
          int32_t offset = inst.immediate;
          if (IsSpOffset(state, offset)) {
            statement.expression = LiftedExpression::Load("f32", MakeSpAddr(state, offset));
          } else {
            uint32_t base_offset = frame_size + 16;
            int arg_idx = -1;
            if (offset >= static_cast<int32_t>(base_offset)) {
              arg_idx = 4 + (offset - static_cast<int32_t>(base_offset)) / 4;
            }
            if (arg_idx >= 0) {
              if (ctx.double_stack_param_slots != nullptr) {
                if ((arg_idx % 2 == 0) && ctx.double_stack_param_slots->contains(arg_idx) &&
                    (ft_idx % 2 == 1)) {
                  break;
                }
                if ((arg_idx % 2 == 1) && ctx.double_stack_param_slots->contains(arg_idx - 1) &&
                    (ft_idx % 2 == 0)) {
                  statement.destination_variable =
                      ExpressionBuilder::FpRegisterVarName(*inst.ft, /*is_double=*/true);
                  statement.expression =
                      LiftedExpression::Variable(absl::StrFormat("arg%d", arg_idx - 1));
                  statements.push_back(std::move(statement));
                  break;
                }
              }
              std::string arg_name = absl::StrFormat("arg%d", arg_idx);
              if (ctx.float_stack_param_slots != nullptr &&
                  ctx.float_stack_param_slots->contains(arg_idx)) {
                statement.expression = LiftedExpression::Variable(arg_name);
              } else {
                statement.expression =
                    LiftedExpression::Unary("*(f32*)&", LiftedExpression::Variable(arg_name));
              }
            } else {
              int32_t base_dword_offset = offset & ~7;
              int32_t byte_in_dword = offset & 7;
              if (!state.local_stack_word_stores.contains(offset) &&
                  state.local_stack_dword_stores.contains(base_dword_offset)) {
                auto base_addr = LiftedExpression::Unary(
                    "&",
                    LiftedExpression::Variable(ExpressionBuilder::StackVarName(base_dword_offset)));
                if (byte_in_dword == 0) {
                  statement.expression = LiftedExpression::Load("f32", std::move(base_addr));
                } else {
                  statement.expression = LiftedExpression::Load(
                      "f32", LiftedExpression::Binary(
                                 "+", LiftedExpression::Unary("(u8*)", std::move(base_addr)),
                                 LiftedExpression::Integer(byte_in_dword)));
                }
              } else if (ctx.float_local_stack_offsets != nullptr &&
                         ctx.float_local_stack_offsets->contains(offset)) {
                statement.expression =
                    LiftedExpression::Variable(ExpressionBuilder::StackVarName(offset));
              } else {
                statement.expression = LiftedExpression::Load(
                    "f32",
                    LiftedExpression::Unary(
                        "&", LiftedExpression::Variable(ExpressionBuilder::StackVarName(offset))));
              }
            }
          }
        } else {
          const char* fp_load_type =
              (state.sp_alloc_idx.has_value() && i < *state.sp_alloc_idx) ? "const f32" : "f32";
          statement.expression = LiftedExpression::Load(
              fp_load_type, MakeRegOffsetExpr(state, *inst.rs, inst.immediate));
        }
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kSwc1:
      if (inst.ft.has_value() && inst.rs.has_value()) {
        int ft_idx = static_cast<int>(*inst.ft);
        bool is_int_trunc_store =
            (ft_idx >= 0 && ft_idx < 32 && pending_fp_to_int[ft_idx] != nullptr);
        auto val_expr =
            is_int_trunc_store ? std::move(pending_fp_to_int[ft_idx]) : LiftFpRegister(*inst.ft);
        const char* store_type = is_int_trunc_store ? "s32" : "f32";
        LiftedStatement statement;
        statement.kind = StatementKind::kStore;
        statement.store_type = store_type;
        bool is_stack_base =
            (*inst.rs == Register::kSp) || (has_frame_pointer && *inst.rs == Register::kFp);
        if (is_stack_base) {
          if (ctx.saved_fpr_offsets != nullptr) {
            auto it = ctx.saved_fpr_offsets->find(*inst.ft);
            if (it != ctx.saved_fpr_offsets->end() && it->second == inst.immediate) {
              break;
            }
          }
          if (IsSpOffset(state, inst.immediate)) {
            statement.destination_address = MakeSpAddr(state, inst.immediate);
            statement.expression = std::move(val_expr);
          } else {
            uint32_t base_offset = frame_size + 16;
            int arg_idx = -1;
            if (inst.immediate >= static_cast<int32_t>(base_offset)) {
              int candidate_idx = 4 + (inst.immediate - static_cast<int32_t>(base_offset)) / 4;
              if (ctx.float_stack_param_slots != nullptr &&
                  ctx.float_stack_param_slots->contains(candidate_idx)) {
                arg_idx = candidate_idx;
              }
            }
            if (arg_idx >= 0) {
              std::string arg_name = absl::StrFormat("arg%d", arg_idx);
              statement.kind = StatementKind::kAssignment;
              statement.destination_variable = arg_name;
              statement.expression = std::move(val_expr);
            } else if (is_int_trunc_store ||
                       (ctx.float_local_stack_offsets != nullptr &&
                        ctx.float_local_stack_offsets->contains(inst.immediate))) {
              statement.kind = StatementKind::kAssignment;
              statement.destination_variable = ExpressionBuilder::StackVarName(inst.immediate);
              statement.expression = std::move(val_expr);
            } else {
              statement.destination_address = LiftedExpression::Unary(
                  "&", LiftedExpression::Variable(ExpressionBuilder::StackVarName(inst.immediate)));
              statement.expression = std::move(val_expr);
            }
          }
        } else {
          statement.destination_address = MakeRegOffsetExpr(state, *inst.rs, inst.immediate);
          statement.expression = std::move(val_expr);
        }
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kLdc1:
      if (inst.ft.has_value() && inst.rs.has_value()) {
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable =
            ExpressionBuilder::FpRegisterVarName(*inst.ft, /*is_double=*/true);
        bool is_stack_base =
            (*inst.rs == Register::kSp) || (has_frame_pointer && *inst.rs == Register::kFp);
        if (is_stack_base) {
          int32_t offset = inst.immediate;
          if (ctx.saved_fpr_offsets != nullptr && ctx.saved_fpr_offsets->contains(*inst.ft) &&
              ctx.saved_fpr_offsets->at(*inst.ft) == offset) {
            break;
          }
          if (IsSpOffset(state, offset)) {
            statement.expression = LiftedExpression::Load("f64", MakeSpAddr(state, offset));
          } else {
            uint32_t base_offset = frame_size + 16;
            int arg_idx = -1;
            if (offset >= static_cast<int32_t>(base_offset)) {
              arg_idx = 4 + (offset - static_cast<int32_t>(base_offset)) / 4;
            }
            if (arg_idx >= 0) {
              std::string arg_name = absl::StrFormat("arg%d", arg_idx);
              if (ctx.double_stack_param_slots != nullptr &&
                  ctx.double_stack_param_slots->contains(arg_idx)) {
                statement.expression = LiftedExpression::Variable(arg_name);
              } else {
                statement.expression =
                    LiftedExpression::Unary("*(f64*)&", LiftedExpression::Variable(arg_name));
              }
            } else if (ctx.double_parameter_in_a2_a3 &&
                       offset == static_cast<int32_t>(frame_size + 8)) {
              statement.expression =
                  LiftedExpression::Variable(ExpressionBuilder::StackVarName(offset));
            } else {
              statement.expression = LiftedExpression::Load(
                  "f64",
                  LiftedExpression::Unary(
                      "&", LiftedExpression::Variable(ExpressionBuilder::StackVarName(offset))));
            }
          }
        } else {
          statement.expression =
              LiftedExpression::Load("f64", MakeRegOffsetExpr(state, *inst.rs, inst.immediate));
        }
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kSdc1:
      if (inst.ft.has_value() && inst.rs.has_value()) {
        LiftedStatement statement;
        statement.kind = StatementKind::kStore;
        statement.store_type = "f64";
        bool is_stack_base =
            (*inst.rs == Register::kSp) || (has_frame_pointer && *inst.rs == Register::kFp);
        if (is_stack_base) {
          int32_t offset = inst.immediate;
          if (ctx.saved_fpr_offsets != nullptr && ctx.saved_fpr_offsets->contains(*inst.ft) &&
              ctx.saved_fpr_offsets->at(*inst.ft) == offset) {
            break;
          }
          if (IsSpOffset(state, inst.immediate)) {
            statement.destination_address = MakeSpAddr(state, inst.immediate);
          } else {
            uint32_t base_offset = frame_size + 16;
            int arg_idx = -1;
            if (inst.immediate >= static_cast<int32_t>(base_offset)) {
              int candidate_idx = 4 + (inst.immediate - static_cast<int32_t>(base_offset)) / 4;
              if (ctx.double_stack_param_slots != nullptr &&
                  ctx.double_stack_param_slots->contains(candidate_idx)) {
                arg_idx = candidate_idx;
              }
            }
            if (arg_idx >= 0) {
              statement.kind = StatementKind::kAssignment;
              statement.destination_variable = absl::StrFormat("arg%d", arg_idx);
            } else {
              statement.destination_address = LiftedExpression::Unary(
                  "&", LiftedExpression::Variable(ExpressionBuilder::StackVarName(inst.immediate)));
            }
          }
        } else {
          statement.destination_address = MakeRegOffsetExpr(state, *inst.rs, inst.immediate);
        }
        statement.expression = LiftFpRegister(*inst.ft, /*is_double=*/true);
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kMfc1:
      if (inst.rt.has_value() && *inst.rt != Register::kZero && inst.fs.has_value()) {
        int fs_idx = static_cast<int>(*inst.fs);
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = ExpressionBuilder::RegisterVarName(*inst.rt);
        bool is_arg_reg =
            (*inst.rt == Register::kA1 || *inst.rt == Register::kA2 || *inst.rt == Register::kA3);
        if (pending_fp_to_int[fs_idx] != nullptr) {
          statement.expression = std::move(pending_fp_to_int[fs_idx]);
        } else if (ctx.float_parameters_in_general_registers != nullptr &&
                   ctx.float_parameters_in_general_registers->contains(*inst.rt) && !is_arg_reg) {
          statement.expression = LiftFpRegister(*inst.fs, /*is_double=*/false);
        } else {
          statement.expression =
              LiftedExpression::Unary("*(s32*)&", LiftFpRegister(*inst.fs, /*is_double=*/false));
        }
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kDmfc1:
      if (inst.rt.has_value() && *inst.rt != Register::kZero && inst.fs.has_value()) {
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = ExpressionBuilder::RegisterVarName(*inst.rt);
        statement.expression =
            LiftedExpression::Unary("*(s64*)&", LiftFpRegister(*inst.fs, /*is_double=*/true));
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kMtc1:
      if (inst.fs.has_value() && inst.rt.has_value()) {
        int fs_idx = static_cast<int>(*inst.fs);
        bool only_used_by_int_to_fp_cvt = false;
        bool rt_modified_before_cvt = false;
        size_t cvt_look_idx = 0;
        if (*inst.rt != Register::kZero) {
          for (size_t look = i + 1; look < loop_end; ++look) {
            const Instruction& future_inst = instructions_to_process[look];
            if ((future_inst.opcode == Opcode::kCvtSW || future_inst.opcode == Opcode::kCvtDW) &&
                future_inst.fs == *inst.fs && future_inst.fd.has_value()) {
              bool reused_after_cvt = false;
              if (future_inst.fd != *inst.fs) {
                for (size_t k = look + 1; k < loop_end; ++k) {
                  RegisterUseDef kud = GetInstructionUseDef(instructions_to_process[k]);
                  for (FpRegister u : kud.fpr_uses) {
                    if (u == *inst.fs || (static_cast<int>(u) & ~1) == (fs_idx & ~1)) {
                      reused_after_cvt = true;
                      break;
                    }
                  }
                  if (reused_after_cvt) break;
                  bool redefined = false;
                  for (FpRegister d : kud.fpr_defs) {
                    if (d == *inst.fs || (static_cast<int>(d) & ~1) == (fs_idx & ~1)) {
                      redefined = true;
                      break;
                    }
                  }
                  if (redefined || instructions_to_process[k].IsCall()) break;
                }
              }
              only_used_by_int_to_fp_cvt = !reused_after_cvt;
              cvt_look_idx = look;
              break;
            }
            RegisterUseDef fud = GetInstructionUseDef(future_inst);
            bool touches_fs = false;
            for (FpRegister u : fud.fpr_uses) {
              if (u == *inst.fs || (static_cast<int>(u) & ~1) == (fs_idx & ~1)) {
                touches_fs = true;
              }
            }
            for (FpRegister d : fud.fpr_defs) {
              if (d == *inst.fs || (static_cast<int>(d) & ~1) == (fs_idx & ~1)) {
                touches_fs = true;
              }
            }
            for (Register d : fud.gpr_defs) {
              if (d == *inst.rt) {
                rt_modified_before_cvt = true;
              }
            }
            if (touches_fs || future_inst.IsCall()) {
              break;
            }
          }
        }
        if (only_used_by_int_to_fp_cvt && !rt_modified_before_cvt) {
          pending_int_to_fp[fs_idx] = ExpressionBuilder::LiftRegisterOrConstant(*inst.rt, tracker);
          fpr_const_bits[fs_idx] = std::nullopt;
          break;
        }
        if (only_used_by_int_to_fp_cvt && rt_modified_before_cvt) {
          const Instruction& cvt_inst = instructions_to_process[cvt_look_idx];
          FpRegister target_fd = *cvt_inst.fd;
          int target_fd_idx = static_cast<int>(target_fd);
          bool fd_clobbered_between = false;
          absl::flat_hash_set<size_t> folded_staging_indices;
          for (size_t m = i + 1; m < cvt_look_idx; ++m) {
            if (skipped_instruction_indices.contains(m) || folded_staging_indices.contains(m)) {
              continue;
            }
            const Instruction& mid_inst = instructions_to_process[m];
            if (mid_inst.opcode == Opcode::kMtc1 && mid_inst.fs == target_fd &&
                mid_inst.rt.has_value() && *mid_inst.rt != Register::kZero) {
              bool staged_cvt_found = false;
              for (size_t m2 = m + 1; m2 < cvt_look_idx; ++m2) {
                const Instruction& m2_inst = instructions_to_process[m2];
                if ((m2_inst.opcode == Opcode::kCvtSW || m2_inst.opcode == Opcode::kCvtDW) &&
                    m2_inst.fs == target_fd && m2_inst.fd.has_value() && *m2_inst.fd != target_fd) {
                  folded_staging_indices.insert(m2);
                  staged_cvt_found = true;
                  break;
                }
                RegisterUseDef m2_ud = GetInstructionUseDef(m2_inst);
                bool touches_target_or_rt = false;
                for (FpRegister u : m2_ud.fpr_uses) {
                  if (u == target_fd || (static_cast<int>(u) & ~1) == (target_fd_idx & ~1)) {
                    touches_target_or_rt = true;
                  }
                }
                for (FpRegister d : m2_ud.fpr_defs) {
                  if (d == target_fd || (static_cast<int>(d) & ~1) == (target_fd_idx & ~1)) {
                    touches_target_or_rt = true;
                  }
                }
                for (Register d : m2_ud.gpr_defs) {
                  if (d == *mid_inst.rt) {
                    touches_target_or_rt = true;
                  }
                }
                if (touches_target_or_rt || m2_inst.IsCall()) break;
              }
              if (staged_cvt_found) {
                continue;
              }
            }
            RegisterUseDef mud = GetInstructionUseDef(mid_inst);
            for (FpRegister u : mud.fpr_uses) {
              if (u == target_fd || (static_cast<int>(u) & ~1) == (target_fd_idx & ~1)) {
                fd_clobbered_between = true;
              }
            }
            for (FpRegister d : mud.fpr_defs) {
              if (d == target_fd || (static_cast<int>(d) & ~1) == (target_fd_idx & ~1)) {
                fd_clobbered_between = true;
              }
            }
            if (fd_clobbered_between) break;
          }
          if (!fd_clobbered_between) {
            bool is_double_cvt = (cvt_inst.opcode == Opcode::kCvtDW);
            LiftedStatement statement;
            statement.kind = StatementKind::kAssignment;
            statement.destination_variable =
                ExpressionBuilder::FpRegisterVarName(target_fd, is_double_cvt);
            statement.expression = LiftedExpression::Unary(
                is_double_cvt ? "(f64)" : "(f32)",
                ExpressionBuilder::LiftRegisterOrConstant(*inst.rt, tracker));
            statements.push_back(std::move(statement));
            skipped_instruction_indices.insert(cvt_look_idx);
            fpr_const_bits[fs_idx] = std::nullopt;
            fpr_const_bits[target_fd_idx] = std::nullopt;
            break;
          }
        }

        if (ctx.double_parameter_in_a2_a3) {
          if (*inst.rt == Register::kA2 && !state.gprs_defined_in_block.contains(Register::kA2) &&
              (fs_idx % 2 == 1)) {
            fpr_const_bits[fs_idx] = std::nullopt;
            break;
          }
          if (*inst.rt == Register::kA3 && !state.gprs_defined_in_block.contains(Register::kA3) &&
              (fs_idx % 2 == 0)) {
            fpr_const_bits[fs_idx] = std::nullopt;
            LiftedStatement statement;
            statement.kind = StatementKind::kAssignment;
            statement.destination_variable =
                ExpressionBuilder::FpRegisterVarName(*inst.fs, /*is_double=*/true);
            statement.expression = LiftedExpression::Variable("arg2");
            statements.push_back(std::move(statement));
            break;
          }
        }

        if ((fs_idx % 2 == 0) && i + 1 < loop_end &&
            (*inst.rt == Register::kS1 || *inst.rt == Register::kS3 || *inst.rt == Register::kS5 ||
             *inst.rt == Register::kS7)) {
          size_t next_idx = i + 1;
          if (next_idx + 1 < loop_end && instructions_to_process[next_idx].IsNop()) {
            ++next_idx;
          }
          const Instruction& next_inst = instructions_to_process[next_idx];
          Register even_gpr = static_cast<Register>(static_cast<int>(*inst.rt) - 1);
          FpRegister odd_fpr = static_cast<FpRegister>(fs_idx + 1);
          if (next_inst.opcode == Opcode::kMtc1 && next_inst.fs == odd_fpr &&
              next_inst.rt == even_gpr) {
            fpr_const_bits[fs_idx] = std::nullopt;
            fpr_const_bits[fs_idx + 1] = std::nullopt;
            LiftedStatement statement;
            statement.kind = StatementKind::kAssignment;
            statement.destination_variable =
                ExpressionBuilder::FpRegisterVarName(*inst.fs, /*is_double=*/true);
            statement.expression =
                LiftedExpression::Variable(ExpressionBuilder::RegisterVarName(even_gpr));
            statements.push_back(std::move(statement));
            skipped_instruction_indices.insert(next_idx);
            break;
          }
        }

        std::optional<uint32_t> const_bits = ExtractMtc1ConstantBits(*inst.rt, tracker);
        fpr_const_bits[fs_idx] = const_bits;
        const bool has_val_bits = const_bits.has_value();
        const uint32_t val_bits = const_bits.value_or(0);

        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable =
            ExpressionBuilder::FpRegisterVarName(*inst.fs, /*is_double=*/false);
        if (*inst.rt == Register::kZero) {
          statement.expression = LiftedExpression::Variable("0.0f");
        } else if (IsActiveFloatGpr(state, *inst.rt)) {
          statement.expression =
              LiftedExpression::Variable(ExpressionBuilder::RegisterVarName(*inst.rt));
        } else if (*inst.rt == Register::kAt && has_val_bits && (val_bits & 0x7F800000u) != 0 &&
                   (val_bits & 0x7F800000u) != 0x7F800000u) {
          float f_val = 0.0f;
          std::memcpy(&f_val, &val_bits, sizeof(f_val));
          std::string f_str = absl::StrFormat("%.9g", f_val);
          if (f_str.find('.') == std::string::npos && f_str.find('e') == std::string::npos) {
            f_str += ".0f";
          } else {
            f_str += "f";
          }
          statement.expression = LiftedExpression::Variable(f_str);
        } else {
          statement.expression = LiftedExpression::Unary(
              "*(f32*)&", LiftedExpression::Variable(ExpressionBuilder::RegisterVarName(*inst.rt)));
        }
        statements.push_back(std::move(statement));

        int even_idx = fs_idx & ~1;
        int odd_idx = even_idx + 1;
        if (fpr_const_bits[even_idx].has_value() && fpr_const_bits[odd_idx].has_value()) {
          uint64_t d_bits =
              (static_cast<uint64_t>(*fpr_const_bits[odd_idx]) << 32) | *fpr_const_bits[even_idx];
          double d_val = 0.0;
          std::memcpy(&d_val, &d_bits, sizeof(d_val));
          if (std::isfinite(d_val)) {
            LiftedStatement d_stmt;
            d_stmt.kind = StatementKind::kAssignment;
            d_stmt.destination_variable = ExpressionBuilder::FpRegisterVarName(
                static_cast<FpRegister>(even_idx), /*is_double=*/true);
            if (d_val == 0.0) {
              d_stmt.expression = LiftedExpression::Variable("0.0");
            } else {
              std::string d_str = absl::StrFormat("%.17g", d_val);
              if (d_str.find('.') == std::string::npos && d_str.find('e') == std::string::npos) {
                d_str += ".0";
              }
              d_stmt.expression = LiftedExpression::Variable(d_str);
            }
            statements.push_back(std::move(d_stmt));
          }
        }
      }
      break;

    case Opcode::kDmtc1:
      if (inst.fs.has_value() && inst.rt.has_value()) {
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable =
            ExpressionBuilder::FpRegisterVarName(*inst.fs, /*is_double=*/true);
        if (*inst.rt == Register::kZero) {
          statement.expression = LiftedExpression::Variable("0.0");
        } else {
          statement.expression = LiftedExpression::Unary(
              "*(f64*)&", LiftedExpression::Variable(ExpressionBuilder::RegisterVarName(*inst.rt)));
        }
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kCfc1:
      if (inst.rt.has_value() && *inst.rt != Register::kZero && inst.fs.has_value()) {
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = ExpressionBuilder::RegisterVarName(*inst.rt);
        int ctrl = static_cast<int>(*inst.fs);
        if (ctrl == 31) {
          statement.expression = LiftedExpression::Variable("fcr31");
        } else {
          statement.expression = LiftedExpression::Integer(0x00000B00, true);
        }
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kCtc1:
      if (inst.fs.has_value() && inst.rt.has_value()) {
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = "fcr31";
        statement.expression = ExpressionBuilder::LiftRegisterOrConstant(*inst.rt, tracker);
        statements.push_back(std::move(statement));
      }
      break;

    default:
      break;
  }
}

void LiftCop1ArithmeticAndConvertInstruction(const Instruction& inst, size_t i,
                                             Cop1LiftState& state) {
  const auto& instructions_to_process = state.instructions_to_process;
  const size_t loop_end = state.loop_end;
  auto* pending_int_to_fp = state.pending_int_to_fp;
  auto* pending_fp_to_int = state.pending_fp_to_int;
  auto& statements = state.statements;

  switch (inst.opcode) {
    case Opcode::kAddS:
    case Opcode::kSubS:
    case Opcode::kMulS:
    case Opcode::kDivS:
    case Opcode::kAddD:
    case Opcode::kSubD:
    case Opcode::kMulD:
    case Opcode::kDivD:
      if (inst.fd.has_value() && inst.fs.has_value() && inst.ft.has_value()) {
        bool is_double = (inst.opcode == Opcode::kAddD || inst.opcode == Opcode::kSubD ||
                          inst.opcode == Opcode::kMulD || inst.opcode == Opcode::kDivD);
        const char* op = "+";
        if (inst.opcode == Opcode::kSubS || inst.opcode == Opcode::kSubD) op = "-";
        if (inst.opcode == Opcode::kMulS || inst.opcode == Opcode::kMulD) op = "*";
        if (inst.opcode == Opcode::kDivS || inst.opcode == Opcode::kDivD) op = "/";
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = ExpressionBuilder::FpRegisterVarName(*inst.fd, is_double);
        statement.expression = LiftedExpression::Binary(op, LiftFpRegister(*inst.fs, is_double),
                                                        LiftFpRegister(*inst.ft, is_double));
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kSqrtS:
    case Opcode::kSqrtD:
    case Opcode::kAbsS:
    case Opcode::kAbsD:
    case Opcode::kRoundWS:
    case Opcode::kRoundWD:
    case Opcode::kCeilWS:
    case Opcode::kCeilWD:
    case Opcode::kFloorWS:
    case Opcode::kFloorWD:
      if (inst.fd.has_value() && inst.fs.has_value()) {
        bool is_double = (inst.opcode == Opcode::kSqrtD || inst.opcode == Opcode::kAbsD ||
                          inst.opcode == Opcode::kRoundWD || inst.opcode == Opcode::kCeilWD ||
                          inst.opcode == Opcode::kFloorWD);
        const char* fn = "N64Sqrtf";
        if (inst.opcode == Opcode::kSqrtD) fn = "N64Sqrt";
        if (inst.opcode == Opcode::kAbsS) fn = "fabsf";
        if (inst.opcode == Opcode::kAbsD) fn = "fabs";
        if (inst.opcode == Opcode::kRoundWS) fn = "roundf";
        if (inst.opcode == Opcode::kRoundWD) fn = "round";
        if (inst.opcode == Opcode::kCeilWS) fn = "ceilf";
        if (inst.opcode == Opcode::kCeilWD) fn = "ceil";
        if (inst.opcode == Opcode::kFloorWS) fn = "floorf";
        if (inst.opcode == Opcode::kFloorWD) fn = "floor";
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = ExpressionBuilder::FpRegisterVarName(*inst.fd, is_double);
        std::vector<std::unique_ptr<LiftedExpression>> args;
        args.push_back(LiftFpRegister(*inst.fs, is_double));
        statement.expression = LiftedExpression::Call(fn, std::move(args));
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kMovS:
    case Opcode::kMovD:
      if (inst.fd.has_value() && inst.fs.has_value()) {
        bool is_double = (inst.opcode == Opcode::kMovD);
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = ExpressionBuilder::FpRegisterVarName(*inst.fd, is_double);
        statement.expression = LiftFpRegister(*inst.fs, is_double);
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kNegS:
    case Opcode::kNegD:
      if (inst.fd.has_value() && inst.fs.has_value()) {
        bool is_double = (inst.opcode == Opcode::kNegD);
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = ExpressionBuilder::FpRegisterVarName(*inst.fd, is_double);
        statement.expression = LiftedExpression::Unary("-", LiftFpRegister(*inst.fs, is_double));
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kCvtSD:
      if (inst.fd.has_value() && inst.fs.has_value()) {
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable =
            ExpressionBuilder::FpRegisterVarName(*inst.fd, /*is_double=*/false);
        statement.expression =
            LiftedExpression::Unary("(f32)", LiftFpRegister(*inst.fs, /*is_double=*/true));
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kCvtSW:
      if (inst.fd.has_value() && inst.fs.has_value()) {
        int fs_idx = static_cast<int>(*inst.fs);
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable =
            ExpressionBuilder::FpRegisterVarName(*inst.fd, /*is_double=*/false);
        if (pending_int_to_fp[fs_idx] != nullptr) {
          statement.expression =
              LiftedExpression::Unary("(f32)", std::move(pending_int_to_fp[fs_idx]));
        } else {
          statement.expression = LiftedExpression::Unary(
              "(f32)*(s32*)&", LiftFpRegister(*inst.fs, /*is_double=*/false));
        }
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kCvtDS:
      if (inst.fd.has_value() && inst.fs.has_value()) {
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable =
            ExpressionBuilder::FpRegisterVarName(*inst.fd, /*is_double=*/true);
        statement.expression =
            LiftedExpression::Unary("(f64)", LiftFpRegister(*inst.fs, /*is_double=*/false));
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kCvtDW:
      if (inst.fd.has_value() && inst.fs.has_value()) {
        int fs_idx = static_cast<int>(*inst.fs);
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable =
            ExpressionBuilder::FpRegisterVarName(*inst.fd, /*is_double=*/true);
        if (pending_int_to_fp[fs_idx] != nullptr) {
          statement.expression =
              LiftedExpression::Unary("(f64)", std::move(pending_int_to_fp[fs_idx]));
        } else {
          statement.expression = LiftedExpression::Unary(
              "(f64)*(s32*)&", LiftFpRegister(*inst.fs, /*is_double=*/false));
        }
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kCvtWS:
    case Opcode::kCvtWD:
    case Opcode::kTruncWS:
    case Opcode::kTruncWD:
      if (inst.fd.has_value() && inst.fs.has_value()) {
        int fd_idx = static_cast<int>(*inst.fd);
        int fs_idx = static_cast<int>(*inst.fs);
        bool src_is_double = (inst.opcode == Opcode::kCvtWD || inst.opcode == Opcode::kTruncWD);
        bool only_used_by_mfc1_or_swc1 = false;
        for (size_t look = i + 1; look < loop_end; ++look) {
          const Instruction& future_inst = instructions_to_process[look];
          if ((future_inst.opcode == Opcode::kMfc1 && future_inst.fs == *inst.fd &&
               future_inst.rt.has_value() && *future_inst.rt != Register::kZero) ||
              (future_inst.opcode == Opcode::kSwc1 && future_inst.ft == *inst.fd)) {
            bool reused_after_consumer = false;
            for (size_t k = look + 1; k < loop_end; ++k) {
              RegisterUseDef kud = GetInstructionUseDef(instructions_to_process[k]);
              for (FpRegister u : kud.fpr_uses) {
                if (u == *inst.fd || (static_cast<int>(u) & ~1) == (fd_idx & ~1)) {
                  reused_after_consumer = true;
                  break;
                }
              }
              if (reused_after_consumer) break;
              bool redefined = false;
              for (FpRegister d : kud.fpr_defs) {
                if (d == *inst.fd || (static_cast<int>(d) & ~1) == (fd_idx & ~1)) {
                  redefined = true;
                  break;
                }
              }
              if (redefined || instructions_to_process[k].IsCall()) break;
            }
            only_used_by_mfc1_or_swc1 = !reused_after_consumer;
            break;
          }
          RegisterUseDef fud = GetInstructionUseDef(future_inst);
          bool touches_fd = false;
          for (FpRegister u : fud.fpr_uses) {
            if (u == *inst.fd || (static_cast<int>(u) & ~1) == (fd_idx & ~1)) {
              touches_fd = true;
            }
          }
          for (FpRegister d : fud.fpr_defs) {
            if (d == *inst.fd || (static_cast<int>(d) & ~1) == (fd_idx & ~1) || d == *inst.fs ||
                (static_cast<int>(d) & ~1) == (fs_idx & ~1)) {
              touches_fd = true;
            }
          }
          if (touches_fd || future_inst.IsCall()) {
            break;
          }
        }
        if (only_used_by_mfc1_or_swc1) {
          pending_fp_to_int[fd_idx] =
              LiftedExpression::Unary("(s32)", LiftFpRegister(*inst.fs, src_is_double));
          break;
        }
        LiftedStatement statement;
        statement.kind = StatementKind::kStore;
        statement.store_type = "s32";
        statement.destination_address = LiftedExpression::Unary(
            "&", LiftedExpression::Variable(
                     ExpressionBuilder::FpRegisterVarName(*inst.fd, /*is_double=*/false)));
        statement.expression =
            LiftedExpression::Unary("(s32)", LiftFpRegister(*inst.fs, src_is_double));
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kCvtLS:
    case Opcode::kCvtLD:
      if (inst.fd.has_value() && inst.fs.has_value()) {
        LiftedStatement statement;
        statement.kind = StatementKind::kStore;
        statement.store_type = "s64";
        statement.destination_address = LiftedExpression::Unary(
            "&", LiftedExpression::Variable(
                     ExpressionBuilder::FpRegisterVarName(*inst.fd, /*is_double=*/true)));
        bool src_is_double = (inst.opcode == Opcode::kCvtLD);
        statement.expression =
            LiftedExpression::Unary("(s64)", LiftFpRegister(*inst.fs, src_is_double));
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kCvtSL:
      if (inst.fd.has_value() && inst.fs.has_value()) {
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable =
            ExpressionBuilder::FpRegisterVarName(*inst.fd, /*is_double=*/false);
        statement.expression =
            LiftedExpression::Unary("(f32)*(s64*)&", LiftFpRegister(*inst.fs, /*is_double=*/true));
        statements.push_back(std::move(statement));
      }
      break;

    case Opcode::kCvtDL:
      if (inst.fd.has_value() && inst.fs.has_value()) {
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable =
            ExpressionBuilder::FpRegisterVarName(*inst.fd, /*is_double=*/true);
        statement.expression =
            LiftedExpression::Unary("(f64)*(s64*)&", LiftFpRegister(*inst.fs, /*is_double=*/true));
        statements.push_back(std::move(statement));
      }
      break;

    default:
      break;
  }
}

void LiftCop1ComparisonInstruction(const Instruction& inst,
                                   std::vector<LiftedStatement>& statements) {
  switch (inst.opcode) {
    case Opcode::kCFS:
    case Opcode::kCSfS:
    case Opcode::kCFD:
    case Opcode::kCSfD: {
      LiftedStatement statement;
      statement.kind = StatementKind::kAssignment;
      statement.destination_variable = "fcond";
      statement.expression = LiftedExpression::Integer(0);
      statements.push_back(std::move(statement));
      break;
    }

    case Opcode::kCEqS:
    case Opcode::kCSeqS:
    case Opcode::kCUeqS:
    case Opcode::kCNglS:
    case Opcode::kCUnS:
    case Opcode::kCLtS:
    case Opcode::kCOltS:
    case Opcode::kCUltS:
    case Opcode::kCNgeS:
    case Opcode::kCLeS:
    case Opcode::kCOleS:
    case Opcode::kCUleS:
    case Opcode::kCNgtS:
    case Opcode::kCNgleS:
    case Opcode::kCEqD:
    case Opcode::kCSeqD:
    case Opcode::kCUeqD:
    case Opcode::kCNglD:
    case Opcode::kCUnD:
    case Opcode::kCLtD:
    case Opcode::kCOltD:
    case Opcode::kCUltD:
    case Opcode::kCNgeD:
    case Opcode::kCLeD:
    case Opcode::kCOleD:
    case Opcode::kCUleD:
    case Opcode::kCNgtD:
    case Opcode::kCNgleD:
      if (inst.fs.has_value() && inst.ft.has_value()) {
        bool is_double = IsDoubleComparisonOp(inst.opcode);
        const char* op = "==";
        if (inst.opcode == Opcode::kCLtS || inst.opcode == Opcode::kCOltS ||
            inst.opcode == Opcode::kCUltS || inst.opcode == Opcode::kCNgeS ||
            inst.opcode == Opcode::kCLtD || inst.opcode == Opcode::kCOltD ||
            inst.opcode == Opcode::kCUltD || inst.opcode == Opcode::kCNgeD) {
          op = "<";
        } else if (inst.opcode == Opcode::kCLeS || inst.opcode == Opcode::kCOleS ||
                   inst.opcode == Opcode::kCUleS || inst.opcode == Opcode::kCNgtS ||
                   inst.opcode == Opcode::kCNgleS || inst.opcode == Opcode::kCLeD ||
                   inst.opcode == Opcode::kCOleD || inst.opcode == Opcode::kCUleD ||
                   inst.opcode == Opcode::kCNgtD || inst.opcode == Opcode::kCNgleD) {
          op = "<=";
        }
        LiftedStatement statement;
        statement.kind = StatementKind::kAssignment;
        statement.destination_variable = "fcond";
        statement.expression = LiftedExpression::Binary(op, LiftFpRegister(*inst.fs, is_double),
                                                        LiftFpRegister(*inst.ft, is_double));
        statements.push_back(std::move(statement));
      }
      break;

    default:
      break;
  }
}

}  // namespace rom_nom_nom
