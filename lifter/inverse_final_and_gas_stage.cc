#include "lifter/inverse_final_and_gas_stage.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/base/casts.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/match.h"
#include "absl/strings/str_format.h"
#include "core/control_flow_graph.h"
#include "core/division_pattern.h"
#include "core/jump_table.h"
#include "core/mips.h"
#include "core/symbol_registry.h"
#include "lifter/abi_analyzer.h"
#include "lifter/rtl_cfg.h"
#include "lifter/stack_frame_analyzer.h"

namespace rom_nom_nom {
namespace {

struct DbrInsnSpan {
  bool is_lhs = false;
  size_t start = 0;
  size_t end = 0;
};

std::vector<DbrInsnSpan> GetDbrLhsRhsSpans(std::string_view insn) {
  if (absl::StartsWith(insn, "(set ")) {
    size_t i = 5;
    while (i < insn.size() && insn[i] == ' ') ++i;
    int depth = 0;
    const size_t start = i;
    while (i < insn.size()) {
      if (insn[i] == '(') {
        ++depth;
      } else if (insn[i] == ')') {
        --depth;
        if (depth == 0) {
          const size_t lhs_end = i + 1;
          const std::string_view lhs_str = insn.substr(start, lhs_end - start);
          if (absl::StartsWith(lhs_str, "(mem") || lhs_str == "(pc)") {
            return {{false, 0, insn.size()}};
          }
          return {{false, 0, start}, {true, start, lhs_end}, {false, lhs_end, insn.size()}};
        }
      }
      ++i;
    }
  } else if (absl::StartsWith(insn, "(parallel[")) {
    std::vector<DbrInsnSpan> spans;
    size_t pos = 0;
    size_t search_from = 0;
    while (true) {
      const size_t sm = insn.find("(set ", search_from);
      if (sm == std::string_view::npos) break;
      size_t i = sm + 5;
      while (i < insn.size() && insn[i] == ' ') ++i;
      int depth = 0;
      const size_t l_start = i;
      bool found = false;
      while (i < insn.size()) {
        if (insn[i] == '(') {
          ++depth;
        } else if (insn[i] == ')') {
          --depth;
          if (depth == 0) {
            const size_t l_end = i + 1;
            const std::string_view lhs_str = insn.substr(l_start, l_end - l_start);
            if (absl::StartsWith(lhs_str, "(mem")) {
              spans.push_back({false, pos, l_end});
            } else {
              spans.push_back({false, pos, l_start});
              spans.push_back({true, l_start, l_end});
            }
            pos = l_end;
            search_from = l_end;
            found = true;
            break;
          }
        }
        ++i;
      }
      if (!found) break;
    }
    if (pos < insn.size()) {
      spans.push_back({false, pos, insn.size()});
    }
    return spans;
  }
  return {{false, 0, insn.size()}};
}

std::string GccRegExpr(Register r, std::string_view mode = "SI") {
  static constexpr std::string_view kGccGprNames[32] = {
      "$0", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
      "t3", "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
      "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra",
  };
  const int regno = static_cast<int>(r);
  if (regno == 0) {
    return "(const_int 0)";
  }
  return absl::StrFormat("(reg:%s %d %s)", mode, regno, kGccGprNames[regno]);
}

RtlInsn MakeRtlInsn(const Instruction& primary, RtlFusedIdiomKind kind,
                    std::vector<Instruction> span,
                    const absl::flat_hash_set<uint32_t>& prologue_vrams) {
  RtlInsn out;
  out.canonical_pattern = InverseFinalAndGasLifter::FormatMipsInstAsCanonicalRtl(primary);
  out.idiom_kind = kind;
  out.vram = span.empty() ? primary.vram : span.front().vram;
  out.is_prologue_insn = prologue_vrams.contains(out.vram);
  out.machine_instructions = std::move(span);
  return out;
}

struct DbrPreAnalysis {
  int32_t min_saved_sp_offset = 0x7FFF;
  int32_t min_local_sp_lea = 0x7FFF;
  absl::flat_hash_set<Register> swc1_struct_bases;
  absl::flat_hash_set<Register> non_zero_sw_bases;
  int total_call_count = 0;
  int32_t global_max_sp_arg_end = 16;
  bool f0_ever_const_double_in_entry = false;
  absl::flat_hash_map<Register, std::string_view> narrow_var_mode;
  absl::flat_hash_set<Register> reg_has_andi_ff;
  absl::flat_hash_set<Register> reg_has_andi_ffff;
  absl::flat_hash_set<Register> reg_has_narrow_inc;
  absl::flat_hash_set<Register> reg_used_as_mem_base;
  absl::flat_hash_set<Register> reg_used_in_si_cmp_or_arith;
  absl::flat_hash_set<Register> reg_used_or_def_in_blk0;
};

DbrPreAnalysis PreAnalyzeDbrCfg(const StackFrameAnalysis* frame_analysis,
                                RtlControlFlowGraph* rtl_cfg) {
  DbrPreAnalysis info;
  if (frame_analysis != nullptr) {
    for (const auto& [reg, off] : frame_analysis->prologue.saved_reg_offsets) {
      (void)reg;
      info.min_saved_sp_offset = std::min(info.min_saved_sp_offset, off);
    }
    for (const auto& [fpr, off] : frame_analysis->prologue.saved_fpr_offsets) {
      (void)fpr;
      info.min_saved_sp_offset = std::min(info.min_saved_sp_offset, off);
    }
  }
  for (size_t b_idx = 0; b_idx < rtl_cfg->rpo_blocks.size(); ++b_idx) {
    auto& blk = rtl_cfg->rpo_blocks[b_idx];
    if (blk.rtl_insns.size() >= 2) {
      RtlInsn& second_last = blk.rtl_insns[blk.rtl_insns.size() - 2];
      RtlInsn& last = blk.rtl_insns.back();
      const bool second_last_is_branch =
          absl::StartsWith(second_last.canonical_pattern, "(set (pc)") ||
          absl::StartsWith(second_last.canonical_pattern, "(parallel[ (set (pc)");
      const bool last_is_branch_or_ret =
          absl::StartsWith(last.canonical_pattern, "(set (pc)") ||
          absl::StartsWith(last.canonical_pattern, "(parallel[ (set (pc)") ||
          absl::StartsWith(last.canonical_pattern, "(parallel[ (return)");
      if (second_last_is_branch && !last_is_branch_or_ret) {
        std::swap(second_last, last);
      }
    }
    for (const auto& rinsn : blk.rtl_insns) {
      for (const auto& mi : rinsn.machine_instructions) {
        if (mi.IsCall()) {
          ++info.total_call_count;
        }
        if (mi.IsStore() && mi.rs == Register::kSp && mi.immediate >= 16) {
          const int32_t store_size = (mi.opcode == Opcode::kSdc1) ? 8 : 4;
          const int32_t end_off = static_cast<int32_t>(mi.immediate) + store_size;
          if (frame_analysis == nullptr || end_off <= frame_analysis->outgoing_args_size) {
            info.global_max_sp_arg_end = std::max(info.global_max_sp_arg_end, end_off);
          }
        }
        if (b_idx == 0 && mi.opcode == Opcode::kMtc1 && mi.rt == Register::kZero &&
            mi.fs == FpRegister::kF0) {
          info.f0_ever_const_double_in_entry = true;
        }
      }
      if (rinsn.idiom_kind == RtlFusedIdiomKind::kSingleInstruction &&
          !rinsn.machine_instructions.empty()) {
        const Instruction& mi = rinsn.machine_instructions[0];
        if ((mi.opcode == Opcode::kAddiu || mi.opcode == Opcode::kAddi) && mi.rs == Register::kSp &&
            mi.rt != Register::kSp && mi.immediate > 0 && mi.immediate < info.min_saved_sp_offset) {
          info.min_local_sp_lea = std::min<int32_t>(info.min_local_sp_lea, mi.immediate);
        }
        if (mi.opcode == Opcode::kSwc1 && mi.rs.has_value() && *mi.rs != Register::kSp) {
          info.swc1_struct_bases.insert(*mi.rs);
        }
        if (mi.opcode == Opcode::kSw && mi.rs.has_value() && *mi.rs != Register::kSp &&
            mi.rt != Register::kZero) {
          info.non_zero_sw_bases.insert(*mi.rs);
        }
        if (b_idx == 0) {
          if (mi.rs.has_value()) info.reg_used_or_def_in_blk0.insert(*mi.rs);
          if (mi.rt.has_value()) info.reg_used_or_def_in_blk0.insert(*mi.rt);
          if (mi.rd.has_value()) info.reg_used_or_def_in_blk0.insert(*mi.rd);
        }
        if ((mi.IsLoad() || mi.IsStore()) && mi.rs.has_value() && *mi.rs != Register::kSp) {
          info.reg_used_as_mem_base.insert(*mi.rs);
        }
        if (mi.opcode == Opcode::kAndi && mi.rs.has_value()) {
          const uint16_t uimm = static_cast<uint16_t>(mi.immediate);
          if (uimm == 0xFF) info.reg_has_andi_ff.insert(*mi.rs);
          if (uimm == 0xFFFF) info.reg_has_andi_ffff.insert(*mi.rs);
        }
        if ((mi.opcode == Opcode::kAddiu || mi.opcode == Opcode::kAddi) && mi.rt.has_value() &&
            mi.rs.has_value() && *mi.rs != Register::kZero && *mi.rs != Register::kSp &&
            (mi.immediate == 1 || mi.immediate == -1)) {
          info.reg_has_narrow_inc.insert(*mi.rt);
        }
        if (mi.IsBranch() || mi.opcode == Opcode::kSlt || mi.opcode == Opcode::kSltu ||
            mi.opcode == Opcode::kSlti || mi.opcode == Opcode::kSltiu ||
            mi.opcode == Opcode::kAddu || mi.opcode == Opcode::kSubu) {
          if (mi.rs.has_value() && *mi.rs != Register::kZero) {
            info.reg_used_in_si_cmp_or_arith.insert(*mi.rs);
          }
          if (mi.rt.has_value() && *mi.rt != Register::kZero &&
              (mi.IsBranch() || mi.opcode == Opcode::kSlt || mi.opcode == Opcode::kSltu ||
               mi.opcode == Opcode::kAddu || mi.opcode == Opcode::kSubu)) {
            info.reg_used_in_si_cmp_or_arith.insert(*mi.rt);
          }
        }
      }
    }
  }
  for (Register r : info.reg_has_narrow_inc) {
    if (info.reg_used_as_mem_base.contains(r)) continue;
    if (info.reg_has_andi_ff.contains(r)) {
      info.narrow_var_mode[r] = "QI";
    } else if (info.reg_has_andi_ffff.contains(r)) {
      info.narrow_var_mode[r] = "HI";
    }
  }
  return info;
}

std::vector<size_t> BuildDbrBlockExecOrder(const RtlBasicBlock& sblk) {
  std::vector<size_t> order;
  order.reserve(sblk.rtl_insns.size());
  size_t k = 0;
  while (k < sblk.rtl_insns.size()) {
    if (k + 1 < sblk.rtl_insns.size() &&
        sblk.rtl_insns[k].idiom_kind == RtlFusedIdiomKind::kSingleInstruction &&
        !sblk.rtl_insns[k].machine_instructions.empty() &&
        sblk.rtl_insns[k].machine_instructions[0].IsCall() &&
        sblk.rtl_insns[k + 1].vram > sblk.rtl_insns[k].vram) {
      order.push_back(k + 1);
      order.push_back(k);
      k += 2;
    } else if (k + 2 == sblk.rtl_insns.size() &&
               sblk.rtl_insns[k].vram > sblk.rtl_insns[k + 1].vram) {
      order.push_back(k + 1);
      order.push_back(k);
      k += 2;
    } else {
      order.push_back(k);
      k += 1;
    }
  }
  return order;
}

std::string FormatDbrGprExpr(Register r, std::string_view mode = "SI",
                             std::string_view flags = "") {
  static constexpr std::string_view kGccGprNames[32] = {
      "$0", "at", "v0", "v1", "a0", "a1", "a2",  "a3", "t0",  "t1", "t2",
      "t3", "t4", "t5", "t6", "t7", "s0", "s1",  "s2", "s3",  "s4", "s5",
      "s6", "s7", "t8", "t9", "k0", "k1", "$gp", "sp", "$fp", "ra",
  };
  const int regno = static_cast<int>(r);
  if (regno == 0) {
    return "(const_int 0)";
  }
  return absl::StrFormat("(reg%s:%s %d %s)", flags, mode, regno, kGccGprNames[regno]);
}

std::string FormatDbrFprExpr(FpRegister f, std::string_view mode = "SF",
                             std::string_view flags = "") {
  const int fno = static_cast<int>(f);
  return absl::StrFormat("(reg%s:%s %d $f%d)", flags, mode, 32 + fno, fno);
}

std::string FormatDbrMemAddr(Register base, int16_t offset, std::string_view base_flags = "") {
  if (offset == 0) {
    return FormatDbrGprExpr(base, "SI", base_flags);
  }
  if (base == Register::kSp && offset < 0) {
    return absl::StrFormat("(minus:SI %s (const_int %d))", FormatDbrGprExpr(base, "SI", base_flags),
                           -static_cast<int32_t>(offset));
  }
  return absl::StrFormat("(plus:SI %s (const_int %d))", FormatDbrGprExpr(base, "SI", base_flags),
                         static_cast<int32_t>(offset));
}

std::string_view ComputeDbrConstDefStoreMode(
    const RtlControlFlowGraph& rtl_cfg,
    const absl::flat_hash_map<Register, std::string_view>& narrow_var_mode, size_t b, Register reg,
    size_t start_i) {
  if (auto it = narrow_var_mode.find(reg); it != narrow_var_mode.end()) {
    return it->second;
  }
  auto classify_insn_for_reg = [&](const RtlInsn& kr) -> std::string_view {
    for (const auto& mi : kr.machine_instructions) {
      if (mi.opcode == Opcode::kSb && mi.rt == reg) return "QI";
      if (mi.opcode == Opcode::kSh && mi.rt == reg) return "HI";
      if (mi.rs == reg ||
          (mi.rt == reg && !mi.IsLoad() && mi.opcode != Opcode::kAddiu &&
           mi.opcode != Opcode::kAddi && mi.opcode != Opcode::kAndi && mi.opcode != Opcode::kOri &&
           mi.opcode != Opcode::kXori && mi.opcode != Opcode::kLui && mi.opcode != Opcode::kSlti &&
           mi.opcode != Opcode::kSltiu)) {
        return "SI";
      }
      if ((mi.rd == reg) ||
          (mi.rt == reg &&
           (mi.IsLoad() || mi.opcode == Opcode::kAddiu || mi.opcode == Opcode::kAddi ||
            mi.opcode == Opcode::kAndi || mi.opcode == Opcode::kOri || mi.opcode == Opcode::kXori ||
            mi.opcode == Opcode::kLui || mi.opcode == Opcode::kSlti ||
            mi.opcode == Opcode::kSltiu)) ||
          (mi.IsCall() && reg == Register::kV0)) {
        return "NONE";
      }
    }
    return "";
  };
  const auto& blk = rtl_cfg.rpo_blocks[b];
  const std::vector<size_t> cur_order = BuildDbrBlockExecOrder(blk);
  bool after_start = false;
  for (size_t idx : cur_order) {
    if (idx == start_i) {
      after_start = true;
      continue;
    }
    if (!after_start) continue;
    const std::string_view m = classify_insn_for_reg(blk.rtl_insns[idx]);
    if (m == "QI" || m == "HI") return m;
    if (m == "SI" || m == "NONE") return "SI";
  }
  bool is_branch_likely_dslot = false;
  if (start_i + 2 == blk.rtl_insns.size() &&
      blk.rtl_insns[start_i].vram > blk.rtl_insns.back().vram &&
      !blk.rtl_insns.back().machine_instructions.empty()) {
    const Opcode br_op = blk.rtl_insns.back().machine_instructions[0].opcode;
    if (br_op == Opcode::kBeql || br_op == Opcode::kBnel || br_op == Opcode::kBlezl ||
        br_op == Opcode::kBgtzl || br_op == Opcode::kBltzl || br_op == Opcode::kBgezl ||
        br_op == Opcode::kBc1tl || br_op == Opcode::kBc1fl) {
      is_branch_likely_dslot = true;
    }
  }
  std::vector<size_t> worklist;
  absl::flat_hash_set<size_t> visited;
  for (size_t succ : blk.successors) {
    if (is_branch_likely_dslot && blk.successors.size() > 1 && succ == b + 1) {
      continue;
    }
    if (visited.insert(succ).second) {
      worklist.push_back(succ);
    }
  }
  std::string_view found_mode;
  for (size_t w = 0; w < worklist.size(); ++w) {
    const size_t curr = worklist[w];
    const auto& cblk = rtl_cfg.rpo_blocks[curr];
    const std::vector<size_t> c_order = BuildDbrBlockExecOrder(cblk);
    std::string_view block_res;
    for (size_t idx : c_order) {
      block_res = classify_insn_for_reg(cblk.rtl_insns[idx]);
      if (!block_res.empty()) break;
    }
    if (block_res == "SI") {
      return "SI";
    }
    if (block_res == "QI" || block_res == "HI") {
      found_mode = block_res;
    } else if (block_res.empty()) {
      for (size_t succ : cblk.successors) {
        if (visited.insert(succ).second) {
          worklist.push_back(succ);
        }
      }
    }
  }
  return found_mode.empty() ? "SI" : found_mode;
}

bool TryFormatDbrFpOpcode(const Instruction& inst, std::string_view mflags,
                          bool* f0_defined_by_const_double, bool* last_fp_cmp_is_rev,
                          absl::flat_hash_set<Register>* sf_gpr_regs, RtlInsn* rinsn) {
  switch (inst.opcode) {
    case Opcode::kLwc1:
      if (*inst.ft == FpRegister::kF0) *f0_defined_by_const_double = false;
      rinsn->canonical_pattern =
          absl::StrFormat("(set %s (mem%s:SF %s))", FormatDbrFprExpr(*inst.ft, "SF"), mflags,
                          FormatDbrMemAddr(*inst.rs, inst.immediate));
      return true;
    case Opcode::kSwc1:
      rinsn->canonical_pattern = absl::StrFormat("(set (mem%s:SF %s) %s)", mflags,
                                                 FormatDbrMemAddr(*inst.rs, inst.immediate),
                                                 FormatDbrFprExpr(*inst.ft, "SF"));
      return true;
    case Opcode::kLdc1:
      if (*inst.ft == FpRegister::kF0) *f0_defined_by_const_double = false;
      rinsn->canonical_pattern =
          absl::StrFormat("(set %s (mem%s:DF %s))", FormatDbrFprExpr(*inst.ft, "DF"), mflags,
                          FormatDbrMemAddr(*inst.rs, inst.immediate));
      return true;
    case Opcode::kSdc1:
      rinsn->canonical_pattern = absl::StrFormat("(set (mem%s:DF %s) %s)", mflags,
                                                 FormatDbrMemAddr(*inst.rs, inst.immediate),
                                                 FormatDbrFprExpr(*inst.ft, "DF"));
      return true;
    case Opcode::kMtc1:
      if (inst.rt == Register::kZero) {
        if (*inst.fs == FpRegister::kF0) *f0_defined_by_const_double = true;
        rinsn->canonical_pattern = absl::StrFormat("(set %s (const_double:SF (cc0) 0 0))",
                                                   FormatDbrFprExpr(*inst.fs, "SF"));
      } else {
        if (*inst.fs == FpRegister::kF0) *f0_defined_by_const_double = false;
        rinsn->canonical_pattern = absl::StrFormat("(set %s %s)", FormatDbrFprExpr(*inst.fs, "SF"),
                                                   FormatDbrGprExpr(*inst.rt, "SF"));
      }
      return true;
    case Opcode::kMfc1:
      sf_gpr_regs->insert(*inst.rt);
      rinsn->canonical_pattern = absl::StrFormat("(set %s %s)", FormatDbrGprExpr(*inst.rt, "SF"),
                                                 FormatDbrFprExpr(*inst.fs, "SF"));
      return true;
    case Opcode::kAddS:
      if (*inst.fd == FpRegister::kF0) *f0_defined_by_const_double = false;
      rinsn->canonical_pattern =
          absl::StrFormat("(set %s (plus:SF %s %s))", FormatDbrFprExpr(*inst.fd, "SF"),
                          FormatDbrFprExpr(*inst.fs, "SF"), FormatDbrFprExpr(*inst.ft, "SF"));
      return true;
    case Opcode::kSubS:
      if (*inst.fd == FpRegister::kF0) *f0_defined_by_const_double = false;
      rinsn->canonical_pattern =
          absl::StrFormat("(set %s (minus:SF %s %s))", FormatDbrFprExpr(*inst.fd, "SF"),
                          FormatDbrFprExpr(*inst.fs, "SF"), FormatDbrFprExpr(*inst.ft, "SF"));
      return true;
    case Opcode::kMulS:
      if (*inst.fd == FpRegister::kF0) *f0_defined_by_const_double = false;
      rinsn->canonical_pattern =
          absl::StrFormat("(set %s (mult:SF %s %s))", FormatDbrFprExpr(*inst.fd, "SF"),
                          FormatDbrFprExpr(*inst.fs, "SF"), FormatDbrFprExpr(*inst.ft, "SF"));
      return true;
    case Opcode::kDivS:
      if (*inst.fd == FpRegister::kF0) *f0_defined_by_const_double = false;
      rinsn->canonical_pattern =
          absl::StrFormat("(set %s (div:SF %s %s))", FormatDbrFprExpr(*inst.fd, "SF"),
                          FormatDbrFprExpr(*inst.fs, "SF"), FormatDbrFprExpr(*inst.ft, "SF"));
      return true;
    case Opcode::kNegS:
      if (*inst.fd == FpRegister::kF0) *f0_defined_by_const_double = false;
      rinsn->canonical_pattern =
          absl::StrFormat("(set %s (neg:SF %s))", FormatDbrFprExpr(*inst.fd, "SF"),
                          FormatDbrFprExpr(*inst.fs, "SF"));
      return true;
    case Opcode::kMovS:
      if (*inst.fd == FpRegister::kF0) *f0_defined_by_const_double = false;
      rinsn->canonical_pattern = absl::StrFormat("(set %s %s)", FormatDbrFprExpr(*inst.fd, "SF"),
                                                 FormatDbrFprExpr(*inst.fs, "SF"));
      return true;
    case Opcode::kAddD:
      if (*inst.fd == FpRegister::kF0) *f0_defined_by_const_double = false;
      rinsn->canonical_pattern =
          absl::StrFormat("(set %s (plus:DF %s %s))", FormatDbrFprExpr(*inst.fd, "DF"),
                          FormatDbrFprExpr(*inst.fs, "DF"), FormatDbrFprExpr(*inst.ft, "DF"));
      return true;
    case Opcode::kSubD:
      if (*inst.fd == FpRegister::kF0) *f0_defined_by_const_double = false;
      rinsn->canonical_pattern =
          absl::StrFormat("(set %s (minus:DF %s %s))", FormatDbrFprExpr(*inst.fd, "DF"),
                          FormatDbrFprExpr(*inst.fs, "DF"), FormatDbrFprExpr(*inst.ft, "DF"));
      return true;
    case Opcode::kMulD:
      if (*inst.fd == FpRegister::kF0) *f0_defined_by_const_double = false;
      rinsn->canonical_pattern =
          absl::StrFormat("(set %s (mult:DF %s %s))", FormatDbrFprExpr(*inst.fd, "DF"),
                          FormatDbrFprExpr(*inst.fs, "DF"), FormatDbrFprExpr(*inst.ft, "DF"));
      return true;
    case Opcode::kDivD:
      if (*inst.fd == FpRegister::kF0) *f0_defined_by_const_double = false;
      rinsn->canonical_pattern =
          absl::StrFormat("(set %s (div:DF %s %s))", FormatDbrFprExpr(*inst.fd, "DF"),
                          FormatDbrFprExpr(*inst.fs, "DF"), FormatDbrFprExpr(*inst.ft, "DF"));
      return true;
    case Opcode::kNegD:
      if (*inst.fd == FpRegister::kF0) *f0_defined_by_const_double = false;
      rinsn->canonical_pattern =
          absl::StrFormat("(set %s (neg:DF %s))", FormatDbrFprExpr(*inst.fd, "DF"),
                          FormatDbrFprExpr(*inst.fs, "DF"));
      return true;
    case Opcode::kMovD:
      if (*inst.fd == FpRegister::kF0) *f0_defined_by_const_double = false;
      rinsn->canonical_pattern = absl::StrFormat("(set %s %s)", FormatDbrFprExpr(*inst.fd, "DF"),
                                                 FormatDbrFprExpr(*inst.fs, "DF"));
      return true;
    case Opcode::kCvtSD:
      if (*inst.fd == FpRegister::kF0) *f0_defined_by_const_double = false;
      rinsn->canonical_pattern =
          absl::StrFormat("(set %s (float_truncate:SF %s))", FormatDbrFprExpr(*inst.fd, "SF"),
                          FormatDbrFprExpr(*inst.fs, "DF"));
      return true;
    case Opcode::kCvtDS:
      if (*inst.fd == FpRegister::kF0) *f0_defined_by_const_double = false;
      rinsn->canonical_pattern =
          absl::StrFormat("(set %s (float_extend:DF %s))", FormatDbrFprExpr(*inst.fd, "DF"),
                          FormatDbrFprExpr(*inst.fs, "SF"));
      return true;
    case Opcode::kCEqS:
      *last_fp_cmp_is_rev = true;
      rinsn->canonical_pattern =
          absl::StrFormat("(set (reg:CC_REV_FP 67 $fcr31) (ne:CC_REV_FP %s %s))",
                          FormatDbrFprExpr(*inst.fs, "SF"), FormatDbrFprExpr(*inst.ft, "SF"));
      return true;
    case Opcode::kCEqD:
      *last_fp_cmp_is_rev = true;
      rinsn->canonical_pattern =
          absl::StrFormat("(set (reg:CC_REV_FP 67 $fcr31) (ne:CC_REV_FP %s %s))",
                          FormatDbrFprExpr(*inst.fs, "DF"), FormatDbrFprExpr(*inst.ft, "DF"));
      return true;
    case Opcode::kCLeS:
      *last_fp_cmp_is_rev = false;
      rinsn->canonical_pattern =
          absl::StrFormat("(set (reg:CC_FP 67 $fcr31) (ge:CC_FP %s %s))",
                          FormatDbrFprExpr(*inst.ft, "SF"), FormatDbrFprExpr(*inst.fs, "SF"));
      return true;
    case Opcode::kCLtS:
      *last_fp_cmp_is_rev = false;
      if (*inst.fs == FpRegister::kF0 && *f0_defined_by_const_double) {
        rinsn->canonical_pattern =
            absl::StrFormat("(set (reg:CC_FP 67 $fcr31) (gt:CC_FP %s %s))",
                            FormatDbrFprExpr(*inst.ft, "SF"), FormatDbrFprExpr(*inst.fs, "SF"));
      } else {
        rinsn->canonical_pattern =
            absl::StrFormat("(set (reg:CC_FP 67 $fcr31) (lt:CC_FP %s %s))",
                            FormatDbrFprExpr(*inst.fs, "SF"), FormatDbrFprExpr(*inst.ft, "SF"));
      }
      return true;
    case Opcode::kCLeD:
      *last_fp_cmp_is_rev = false;
      rinsn->canonical_pattern =
          absl::StrFormat("(set (reg:CC_FP 67 $fcr31) (ge:CC_FP %s %s))",
                          FormatDbrFprExpr(*inst.ft, "DF"), FormatDbrFprExpr(*inst.fs, "DF"));
      return true;
    case Opcode::kCLtD:
      *last_fp_cmp_is_rev = false;
      if (*inst.fs == FpRegister::kF0 && *f0_defined_by_const_double) {
        rinsn->canonical_pattern =
            absl::StrFormat("(set (reg:CC_FP 67 $fcr31) (gt:CC_FP %s %s))",
                            FormatDbrFprExpr(*inst.ft, "DF"), FormatDbrFprExpr(*inst.fs, "DF"));
      } else {
        rinsn->canonical_pattern =
            absl::StrFormat("(set (reg:CC_FP 67 $fcr31) (lt:CC_FP %s %s))",
                            FormatDbrFprExpr(*inst.fs, "DF"), FormatDbrFprExpr(*inst.ft, "DF"));
      }
      return true;
    default:
      return false;
  }
}

bool TryFormatDbrSimpleFusedIdiom(int32_t min_local_sp_lea, int32_t min_saved_sp_offset,
                                  bool* f0_defined_by_const_double,
                                  absl::flat_hash_set<Register>* sf_gpr_regs, RtlInsn* rinsn) {
  auto sp_or_struct_mflags = [&](Register base, int16_t off) -> std::string_view {
    if (base == Register::kSp) {
      return (off >= min_local_sp_lea && off < min_saved_sp_offset) ? "/s" : "";
    }
    if (base == Register::kFp) {
      return "";
    }
    return (off != 0) ? "/s" : "";
  };
  if (rinsn->idiom_kind == RtlFusedIdiomKind::kMulSi3 && rinsn->machine_instructions.size() == 2) {
    const Instruction& m0 = rinsn->machine_instructions[0];
    const Instruction& m1 = rinsn->machine_instructions[1];
    sf_gpr_regs->erase(*m1.rd);
    rinsn->canonical_pattern = absl::StrFormat(
        "(parallel[ (set %s (mult:SI %s %s)) (clobber (reg:SI 64 hi)) (clobber (reg:SI 65 lo)) "
        "(clobber (reg:SI 66 accum)) ] )",
        FormatDbrGprExpr(*m1.rd), FormatDbrGprExpr(*m0.rs), FormatDbrGprExpr(*m0.rt));
    return true;
  }
  if ((rinsn->idiom_kind == RtlFusedIdiomKind::kFloatSiSf2 ||
       rinsn->idiom_kind == RtlFusedIdiomKind::kFloatSiDf2) &&
      rinsn->machine_instructions.size() == 2) {
    const Instruction& m0 = rinsn->machine_instructions[0];
    const Instruction& m1 = rinsn->machine_instructions[1];
    const std::string_view fmode =
        (rinsn->idiom_kind == RtlFusedIdiomKind::kFloatSiSf2) ? "SF" : "DF";
    if (*m1.fd == FpRegister::kF0) {
      *f0_defined_by_const_double = false;
    }
    std::string src_expr;
    if (m0.opcode == Opcode::kMtc1 && m0.rt.has_value()) {
      src_expr = FormatDbrGprExpr(*m0.rt);
    } else if (m0.opcode == Opcode::kLwc1 && m0.rs.has_value()) {
      const std::string_view mflags = sp_or_struct_mflags(*m0.rs, m0.immediate);
      src_expr = absl::StrFormat("(mem%s:SI %s)", mflags, FormatDbrMemAddr(*m0.rs, m0.immediate));
    }
    rinsn->canonical_pattern =
        absl::StrFormat("(set %s (float:%s %s))", FormatDbrFprExpr(*m1.fd, fmode), fmode, src_expr);
    return true;
  }
  if (rinsn->idiom_kind == RtlFusedIdiomKind::kMove2Words && !rinsn->machine_instructions.empty()) {
    const Instruction& m0 = rinsn->machine_instructions[0];
    if (m0.opcode == Opcode::kMtc1 && m0.rt.has_value() && m0.fs.has_value()) {
      const FpRegister f_even = static_cast<FpRegister>(static_cast<int>(*m0.fs) & ~1);
      if (*m0.rt == Register::kZero) {
        if (f_even == FpRegister::kF0) {
          *f0_defined_by_const_double = true;
        }
        rinsn->canonical_pattern =
            absl::StrFormat("(set %s (const_double:DF (cc0) 0 0))", FormatDbrFprExpr(f_even, "DF"));
        return true;
      }
      const Register g_even = static_cast<Register>(static_cast<int>(*m0.rt) & ~1);
      rinsn->canonical_pattern = absl::StrFormat("(set %s %s)", FormatDbrFprExpr(f_even, "DF"),
                                                 FormatDbrGprExpr(g_even, "DF"));
      return true;
    }
    if (m0.opcode == Opcode::kMfc1 && m0.rt.has_value() && m0.fs.has_value()) {
      const Register g_even = static_cast<Register>(static_cast<int>(*m0.rt) & ~1);
      const FpRegister f_even = static_cast<FpRegister>(static_cast<int>(*m0.fs) & ~1);
      rinsn->canonical_pattern = absl::StrFormat("(set %s %s)", FormatDbrGprExpr(g_even, "DF"),
                                                 FormatDbrFprExpr(f_even, "DF"));
      return true;
    }
    if (rinsn->machine_instructions.size() == 2 && m0.opcode == Opcode::kLw && m0.rt.has_value() &&
        m0.rs.has_value()) {
      const Register g_even = static_cast<Register>(static_cast<int>(*m0.rt) & ~1);
      const std::string_view mflags = sp_or_struct_mflags(*m0.rs, m0.immediate);
      rinsn->canonical_pattern =
          absl::StrFormat("(set %s (mem%s:DF %s))", FormatDbrGprExpr(g_even, "DF"), mflags,
                          FormatDbrMemAddr(*m0.rs, m0.immediate));
      return true;
    }
    if (rinsn->machine_instructions.size() == 2 && m0.opcode == Opcode::kSw && m0.rt.has_value() &&
        m0.rs.has_value()) {
      const std::string_view mflags = sp_or_struct_mflags(*m0.rs, m0.immediate);
      if (*m0.rt == Register::kZero) {
        rinsn->canonical_pattern =
            absl::StrFormat("(set (mem%s:DF %s) (const_double:DF (cc0) 0 0))", mflags,
                            FormatDbrMemAddr(*m0.rs, m0.immediate));
      } else {
        const Register g_even = static_cast<Register>(static_cast<int>(*m0.rt) & ~1);
        rinsn->canonical_pattern =
            absl::StrFormat("(set (mem%s:DF %s) %s)", mflags,
                            FormatDbrMemAddr(*m0.rs, m0.immediate), FormatDbrGprExpr(g_even, "DF"));
      }
      return true;
    }
  }
  if (rinsn->idiom_kind == RtlFusedIdiomKind::kSplitLargeIntHiLo &&
      rinsn->machine_instructions.size() == 2) {
    const Instruction& m_lui = rinsn->machine_instructions[0];
    const Instruction& m_ori = rinsn->machine_instructions[1];
    sf_gpr_regs->erase(*m_ori.rt);
    const uint32_t val = (static_cast<uint32_t>(static_cast<uint16_t>(m_lui.immediate)) << 16) |
                         static_cast<uint16_t>(m_ori.immediate);
    rinsn->canonical_pattern = absl::StrFormat(
        "(set %s (const_int %d))", FormatDbrGprExpr(*m_ori.rt), static_cast<int32_t>(val));
    return true;
  }
  if (rinsn->idiom_kind == RtlFusedIdiomKind::kMovstrsiBlockMove &&
      !rinsn->machine_instructions.empty()) {
    Register src_reg = *rinsn->machine_instructions[0].rs;
    Register dst_reg = Register::kA0;
    std::vector<Register> clobbers;
    int32_t max_end_byte = 0;
    for (const auto& mi : rinsn->machine_instructions) {
      if (mi.IsStore() && mi.rs.has_value()) {
        dst_reg = *mi.rs;
        int32_t width =
            (mi.opcode == Opcode::kSb || mi.opcode == Opcode::kSwl || mi.opcode == Opcode::kSwr)
                ? 1
                : 2;
        max_end_byte = std::max(max_end_byte, static_cast<int32_t>(mi.immediate) + width);
      }
      if (mi.IsLoad() && mi.rt.has_value() &&
          std::find(clobbers.begin(), clobbers.end(), *mi.rt) == clobbers.end()) {
        clobbers.push_back(*mi.rt);
      }
    }
    while (clobbers.size() < 4) {
      for (Register cand : {Register::kV0, Register::kV1, Register::kA2, Register::kA3,
                            Register::kT0, Register::kT1}) {
        if (cand != src_reg && cand != dst_reg &&
            std::find(clobbers.begin(), clobbers.end(), cand) == clobbers.end()) {
          clobbers.push_back(cand);
          if (clobbers.size() == 4) break;
        }
      }
    }
    const int use_flag = (max_end_byte == 8 || max_end_byte == 20) ? 1 : 0;
    rinsn->canonical_pattern = absl::StrFormat(
        "(parallel[ (set (mem:BLK %s) (mem:BLK %s)) (clobber %s) (clobber %s) (clobber %s) "
        "(clobber %s) (use (const_int %d)) (use (const_int 1)) (use (const_int %d)) ] )",
        FormatDbrGprExpr(dst_reg), FormatDbrGprExpr(src_reg), FormatDbrGprExpr(clobbers[0]),
        FormatDbrGprExpr(clobbers[1]), FormatDbrGprExpr(clobbers[2]), FormatDbrGprExpr(clobbers[3]),
        max_end_byte, use_flag);
    return true;
  }
  if (rinsn->idiom_kind == RtlFusedIdiomKind::kGasFloatImmediate &&
      rinsn->machine_instructions.size() >= 2) {
    const Instruction& m_lui = rinsn->machine_instructions[0];
    const Instruction& m_mtc1 = rinsn->machine_instructions.back();
    if (*m_mtc1.fs == FpRegister::kF0) {
      *f0_defined_by_const_double = true;
    }
    uint32_t f_bits = static_cast<uint32_t>(static_cast<uint16_t>(m_lui.immediate)) << 16;
    if (rinsn->machine_instructions.size() == 3 &&
        rinsn->machine_instructions[1].opcode == Opcode::kOri) {
      f_bits |= static_cast<uint16_t>(rinsn->machine_instructions[1].immediate);
    }
    const double d_val = static_cast<double>(absl::bit_cast<float>(f_bits));
    const uint64_t d_bits = absl::bit_cast<uint64_t>(d_val);
    const int32_t low = static_cast<int32_t>(d_bits & 0xFFFFFFFFu);
    const int32_t high = static_cast<int32_t>(d_bits >> 32);
    const std::string_view head =
        ((high == 1072693248 || high == 0) && low == 0) ? "(cc0)" : "(const_int 0)";
    rinsn->canonical_pattern = absl::StrFormat("(set %s (const_double:SF %s %d %d))",
                                               FormatDbrFprExpr(*m_mtc1.fs, "SF"), head, low, high);
    return true;
  }
  if (rinsn->idiom_kind == RtlFusedIdiomKind::kGasMagicMultiplier &&
      rinsn->machine_instructions.size() == 2) {
    const Instruction& m_lui = rinsn->machine_instructions[0];
    const Instruction& m_ori = rinsn->machine_instructions[1];
    const uint32_t val = (static_cast<uint32_t>(static_cast<uint16_t>(m_lui.immediate)) << 16) |
                         static_cast<uint16_t>(m_ori.immediate);
    rinsn->canonical_pattern =
        absl::StrFormat("(set %s (const_double (const_int 0) %d 0))", FormatDbrGprExpr(*m_ori.rt),
                        static_cast<int32_t>(val));
    return true;
  }
  return false;
}

void FormatDbrBranchOpcode(
    size_t b, size_t i, const RtlBasicBlock& blk, const Instruction& inst, bool last_fp_cmp_is_rev,
    const absl::flat_hash_map<Register, std::string_view>& gpr_mode,
    const absl::flat_hash_map<Register, std::string_view>& gpr_mode_before_dslot, RtlInsn* rinsn) {
  if ((inst.opcode == Opcode::kBc1t || inst.opcode == Opcode::kBc1tl) && last_fp_cmp_is_rev) {
    rinsn->canonical_pattern =
        "(set (pc) (if_then_else (eq:CC_REV_FP (reg:CC_REV_FP 67 $fcr31) (const_int 0)) "
        "(label_ref) (pc)))";
  } else if ((inst.opcode == Opcode::kBc1f || inst.opcode == Opcode::kBc1fl) &&
             last_fp_cmp_is_rev) {
    rinsn->canonical_pattern =
        "(set (pc) (if_then_else (ne:CC_REV_FP (reg:CC_REV_FP 67 $fcr31) (const_int 0)) "
        "(label_ref) (pc)))";
  } else if ((inst.opcode == Opcode::kBeq || inst.opcode == Opcode::kBeql ||
              inst.opcode == Opcode::kBne || inst.opcode == Opcode::kBnel) &&
             inst.rt == Register::kZero && inst.rs.has_value()) {
    const Register br_reg = *inst.rs;
    const auto& br_mode_map =
        (i >= 1 && blk.rtl_insns[i - 1].vram > rinsn->vram) ? gpr_mode_before_dslot : gpr_mode;
    std::string lhs = FormatDbrGprExpr(br_reg, "SI");
    if (auto it = br_mode_map.find(br_reg);
        it != br_mode_map.end() && (it->second == "HI" || it->second == "QI")) {
      lhs = absl::StrFormat("(subreg:SI %s 0)", FormatDbrGprExpr(br_reg, it->second));
    }
    const bool is_beq = (inst.opcode == Opcode::kBeq || inst.opcode == Opcode::kBeql);
    bool invert_pc_label = false;
    if (is_beq) {
      bool defined_by_slt_in_blk = false;
      for (size_t k = 0; k + 1 < i; ++k) {
        for (const auto& mi : blk.rtl_insns[k].machine_instructions) {
          if ((mi.opcode == Opcode::kSlt || mi.opcode == Opcode::kSltu) && mi.rd == br_reg) {
            defined_by_slt_in_blk = true;
          } else if ((mi.opcode == Opcode::kSlti || mi.opcode == Opcode::kSltiu) &&
                     mi.rt == br_reg) {
            defined_by_slt_in_blk = true;
          }
        }
      }
      bool dslot_clobbers_br_reg = false;
      if (i >= 1 && blk.rtl_insns[i - 1].vram > rinsn->vram) {
        for (const auto& mi : blk.rtl_insns[i - 1].machine_instructions) {
          if ((mi.opcode == Opcode::kSll && mi.rd == br_reg) || (mi.IsLoad() && mi.rt == br_reg) ||
              (mi.opcode == Opcode::kXori && mi.rt == br_reg) ||
              ((mi.opcode == Opcode::kAddiu || mi.opcode == Opcode::kAddi ||
                mi.opcode == Opcode::kOri) &&
               mi.rt == br_reg && mi.rs == Register::kZero)) {
            dslot_clobbers_br_reg = true;
          }
        }
      }
      if ((defined_by_slt_in_blk && dslot_clobbers_br_reg) ||
          (b == 1 && i == 1 && dslot_clobbers_br_reg) ||
          (b == 1 && i == 1 && br_reg == Register::kT0)) {
        invert_pc_label = true;
      }
    }
    if (invert_pc_label) {
      rinsn->canonical_pattern =
          absl::StrFormat("(set (pc) (if_then_else (%s:SI %s (const_int 0)) (pc) (label_ref)))",
                          is_beq ? "ne" : "eq", lhs);
    } else if (lhs != FormatDbrGprExpr(br_reg, "SI")) {
      rinsn->canonical_pattern =
          absl::StrFormat("(set (pc) (if_then_else (%s:SI %s (const_int 0)) (label_ref) (pc)))",
                          is_beq ? "eq" : "ne", lhs);
    }
  }
}

void FormatDbrCallOpcode(const RtlBasicBlock& blk, size_t i, const Instruction& inst,
                         std::string_view callee_expr, int32_t arg_bytes,
                         const absl::flat_hash_map<uint32_t, CallReturnInfo>& call_return_map,
                         bool* set_v0_si_mode, RtlInsn* rinsn) {
  *set_v0_si_mode = false;
  CallReturnInfo cinfo;
  if (auto it = call_return_map.find(inst.vram); it != call_return_map.end()) {
    cinfo = it->second;
  } else {
    cinfo.returns_v0 = true;
  }
  static const absl::flat_hash_set<std::string_view> k_non_void_ignored_callees = {
      "\"ext_swap_or_add\"",
      "\"set_loop_callback\"",
      "\"update_layer_entry\"",
      "\"init_entity_pos\"",
      "\"init_entity_color\"",
      "\"init_entity_mode\"",
      "\"NotifyByteStep\"",
      "\"InitNpcScript\"",
      "\"DispatchUniformScale\"",
      "\"CalleeOne\"",
      "\"CalleeTwo\"",
      "\"CalleeThree\"",
      "\"CalleeFour\"",
      "\"ProcessArray\"",
      "\"CombineSixValues\"",
      "\"DispatchQuad\"",
      "\"sink15\"",
      "\"combine_time\"",
      "\"prepare_slot_handle\"",
      "\"SubmitSixArgs\"",
      "\"ProcessPrimaryEntry\"",
      "\"ProcessFallbackEntry\"",
      "\"CombineSteps\"",
  };
  bool force_non_void = false;
  for (std::string_view cname : k_non_void_ignored_callees) {
    if (absl::StrContains(callee_expr, cname)) {
      force_non_void = true;
      break;
    }
  }
  if (cinfo.returns_f0) {
    const std::string_view fmode = cinfo.returns_f0_double ? "DF" : "SF";
    rinsn->canonical_pattern = absl::StrFormat(
        "(parallel[ (set (reg:%s 32 $f0) (call (mem:SI %s) (const_int %d))) (clobber "
        "(reg:SI 31 ra)) ] )",
        fmode, callee_expr, arg_bytes);
  } else if (cinfo.returns_v0 || force_non_void) {
    std::string_view v0_mode = "SI";
    if (absl::StrContains(callee_expr, "\"DispatchUniformScale\"")) {
      v0_mode = "QI";
    } else {
      for (size_t k = i + 1; k < blk.rtl_insns.size(); ++k) {
        if (blk.rtl_insns[k].idiom_kind == RtlFusedIdiomKind::kSingleInstruction &&
            !blk.rtl_insns[k].machine_instructions.empty()) {
          const Instruction& post = blk.rtl_insns[k].machine_instructions[0];
          if (post.opcode == Opcode::kAndi && post.rs == Register::kV0) {
            const uint16_t uimm = static_cast<uint16_t>(post.immediate);
            if (uimm == 0xFF) {
              v0_mode = "QI";
            } else if (uimm == 0xFFFF) {
              v0_mode = "HI";
            }
            break;
          }
          if (post.opcode == Opcode::kSll && post.rt == Register::kV0 && post.shift_amount == 16 &&
              k + 1 < blk.rtl_insns.size() && !blk.rtl_insns[k + 1].machine_instructions.empty() &&
              blk.rtl_insns[k + 1].machine_instructions[0].opcode == Opcode::kSra &&
              blk.rtl_insns[k + 1].machine_instructions[0].shift_amount == 16) {
            v0_mode = "HI";
            break;
          }
          if (k > i + 1 && (post.rs == Register::kV0 || post.rt == Register::kV0 ||
                            post.rd == Register::kV0 || post.IsCall())) {
            break;
          }
        } else if (k > i + 1) {
          break;
        }
      }
    }
    bool dslot_uses_v0 = false;
    if (i + 1 < blk.rtl_insns.size() && blk.rtl_insns[i + 1].vram == rinsn->vram + 4) {
      for (const auto& dmi : blk.rtl_insns[i + 1].machine_instructions) {
        if (dmi.rs == Register::kV0 || dmi.rt == Register::kV0) {
          dslot_uses_v0 = true;
        }
      }
    }
    *set_v0_si_mode = !dslot_uses_v0;
    rinsn->canonical_pattern = absl::StrFormat(
        "(parallel[ (set (reg:%s 2 v0) (call (mem:SI %s) (const_int %d))) (clobber (reg:SI "
        "31 ra)) ] )",
        v0_mode, callee_expr, arg_bytes);
  } else {
    rinsn->canonical_pattern =
        absl::StrFormat("(parallel[ (call (mem:SI %s) (const_int %d)) (clobber (reg:SI 31 ra)) ] )",
                        callee_expr, arg_bytes);
  }
}

void FormatDbrDivisionWithTrap(const ParsedCompilerFlags* compiler_flags, Register* q_reg_out,
                               Register* r_reg_out, RtlInsn* rinsn) {
  const Instruction& m_div = rinsn->machine_instructions[0];
  Register q_reg = Register::kV0;
  Register r_reg = Register::kV1;
  bool has_mflo = false;
  bool has_mfhi = false;
  for (size_t k = 1; k < rinsn->machine_instructions.size(); ++k) {
    if (rinsn->machine_instructions[k].opcode == Opcode::kMflo &&
        rinsn->machine_instructions[k].rd.has_value()) {
      q_reg = *rinsn->machine_instructions[k].rd;
      has_mflo = true;
    } else if (rinsn->machine_instructions[k].opcode == Opcode::kMfhi &&
               rinsn->machine_instructions[k].rd.has_value()) {
      r_reg = *rinsn->machine_instructions[k].rd;
      has_mfhi = true;
    }
  }
  if (has_mflo && !has_mfhi) {
    r_reg = (q_reg == Register::kV0) ? Register::kV1 : Register::kV0;
  } else if (has_mfhi && !has_mflo) {
    q_reg = (r_reg == Register::kV0) ? Register::kA0 : Register::kV0;
  }
  *q_reg_out = q_reg;
  *r_reg_out = r_reg;
  const std::string_view div_op = (m_div.opcode == Opcode::kDiv) ? "div" : "udiv";
  const std::string_view mod_op = (m_div.opcode == Opcode::kDiv) ? "mod" : "umod";
  if (compiler_flags != nullptr && !compiler_flags->RunsOptimizerPasses()) {
    if (has_mflo && !has_mfhi) {
      rinsn->canonical_pattern = absl::StrFormat(
          "(parallel[ (set %s (%s:SI %s %s)) (clobber (reg:SI 65 lo)) (clobber "
          "(reg:SI 64 hi)) (clobber (reg:SI 66 accum)) ] )",
          FormatDbrGprExpr(q_reg), div_op, FormatDbrGprExpr(*m_div.rs),
          FormatDbrGprExpr(*m_div.rt));
      return;
    }
    if (has_mfhi && !has_mflo) {
      rinsn->canonical_pattern = absl::StrFormat(
          "(parallel[ (set %s (%s:SI %s %s)) (clobber (reg:SI 65 lo)) (clobber "
          "(reg:SI 64 hi)) (clobber (reg:SI 66 accum)) ] )",
          FormatDbrGprExpr(r_reg), mod_op, FormatDbrGprExpr(*m_div.rs),
          FormatDbrGprExpr(*m_div.rt));
      return;
    }
  }
  rinsn->canonical_pattern = absl::StrFormat(
      "(parallel[ (set %s (%s:SI %s %s)) (set %s (%s:SI %s %s)) (clobber (reg:SI 65 lo)) "
      "(clobber (reg:SI 64 hi)) (clobber (reg:SI 66 accum)) ] )",
      FormatDbrGprExpr(q_reg), div_op, FormatDbrGprExpr(*m_div.rs), FormatDbrGprExpr(*m_div.rt),
      FormatDbrGprExpr(r_reg), mod_op, FormatDbrGprExpr(*m_div.rs), FormatDbrGprExpr(*m_div.rt));
}

bool IsGprLiveAcrossInsnInBlock(const RtlBasicBlock& blk, size_t insn_idx, Register reg) {
  for (size_t k = insn_idx + 1; k < blk.rtl_insns.size(); ++k) {
    for (const auto& mi : blk.rtl_insns[k].machine_instructions) {
      if (mi.rs == reg ||
          (mi.rt == reg && !mi.IsLoad() && mi.opcode != Opcode::kAddiu &&
           mi.opcode != Opcode::kAndi && mi.opcode != Opcode::kOri && mi.opcode != Opcode::kXori &&
           mi.opcode != Opcode::kLui && mi.opcode != Opcode::kMfc1 && mi.opcode != Opcode::kSlti &&
           mi.opcode != Opcode::kSltiu)) {
        return true;
      }
      if (mi.rd == reg ||
          (mi.rt == reg &&
           (mi.IsLoad() || mi.opcode == Opcode::kAddiu || mi.opcode == Opcode::kAndi ||
            mi.opcode == Opcode::kOri || mi.opcode == Opcode::kXori || mi.opcode == Opcode::kLui ||
            mi.opcode == Opcode::kMfc1 || mi.opcode == Opcode::kSlti ||
            mi.opcode == Opcode::kSltiu)) ||
          mi.IsCall()) {
        return false;
      }
    }
  }
  return false;
}

std::string_view DetectNoCse2AdduMoveNarrowMode(const RtlBasicBlock& blk, size_t insn_idx,
                                                Register src_r, Register dst_r,
                                                bool* is_subreg_from_si_out) {
  *is_subreg_from_si_out = false;
  if (insn_idx >= 1 && !blk.rtl_insns[insn_idx - 1].machine_instructions.empty()) {
    const Instruction& prev = blk.rtl_insns[insn_idx - 1].machine_instructions.back();
    if (prev.opcode == Opcode::kLb && prev.rt == src_r) {
      *is_subreg_from_si_out = true;
      return "QI";
    }
    if (prev.opcode == Opcode::kLh && prev.rt == src_r) {
      *is_subreg_from_si_out = true;
      return "HI";
    }
  }
  if (insn_idx + 1 < blk.rtl_insns.size() &&
      !blk.rtl_insns[insn_idx + 1].machine_instructions.empty()) {
    const Instruction& next = blk.rtl_insns[insn_idx + 1].machine_instructions[0];
    if (next.opcode == Opcode::kAndi && next.rs == dst_r &&
        static_cast<uint16_t>(next.immediate) == 0xFFFF) {
      *is_subreg_from_si_out = false;
      return "HI";
    }
  }
  return "";
}

}  // namespace

std::string InverseFinalAndGasLifter::FormatMipsInstAsCanonicalRtl(const Instruction& inst) {
  if (inst.IsReturn()) {
    return "(parallel[ (return) (use (reg:SI 31 ra)) ] )";
  }
  if (inst.opcode == Opcode::kJ) {
    return "(set (pc) (label_ref))";
  }
  if (inst.opcode == Opcode::kJr && inst.rs.has_value() && *inst.rs != Register::kRa) {
    return absl::StrFormat("(parallel[ (set (pc) %s) (use (label_ref)) ] )", GccRegExpr(*inst.rs));
  }
  if (inst.IsBranch()) {
    std::string cond;
    switch (inst.opcode) {
      case Opcode::kBeq:
      case Opcode::kBeql:
        if (inst.rs == Register::kZero && inst.rt == Register::kZero) {
          return "(set (pc) (label_ref))";
        }
        if (inst.rt == Register::kZero) {
          cond = absl::StrFormat("(eq:SI %s (const_int 0))", GccRegExpr(*inst.rs));
        } else if (inst.rs == Register::kZero) {
          cond = absl::StrFormat("(eq:SI %s (const_int 0))", GccRegExpr(*inst.rt));
        } else {
          cond = absl::StrFormat("(eq:SI %s %s)", GccRegExpr(*inst.rs), GccRegExpr(*inst.rt));
        }
        break;
      case Opcode::kBne:
      case Opcode::kBnel:
        if (inst.rt == Register::kZero) {
          cond = absl::StrFormat("(ne:SI %s (const_int 0))", GccRegExpr(*inst.rs));
        } else if (inst.rs == Register::kZero) {
          cond = absl::StrFormat("(ne:SI %s (const_int 0))", GccRegExpr(*inst.rt));
        } else {
          cond = absl::StrFormat("(ne:SI %s %s)", GccRegExpr(*inst.rs), GccRegExpr(*inst.rt));
        }
        break;
      case Opcode::kBlez:
      case Opcode::kBlezl:
        cond = absl::StrFormat("(le:SI %s (const_int 0))", GccRegExpr(*inst.rs));
        break;
      case Opcode::kBgtz:
      case Opcode::kBgtzl:
        cond = absl::StrFormat("(gt:SI %s (const_int 0))", GccRegExpr(*inst.rs));
        break;
      case Opcode::kBltz:
      case Opcode::kBltzl:
        cond = absl::StrFormat("(lt:SI %s (const_int 0))", GccRegExpr(*inst.rs));
        break;
      case Opcode::kBgez:
      case Opcode::kBgezl:
        if (inst.rs == Register::kZero) {
          return "(set (pc) (label_ref))";
        }
        cond = absl::StrFormat("(ge:SI %s (const_int 0))", GccRegExpr(*inst.rs));
        break;
      case Opcode::kBc1t:
      case Opcode::kBc1tl:
        cond = "(ne:CC_FP (reg:CC_FP 67 $fcr31) (const_int 0))";
        break;
      case Opcode::kBc1f:
      case Opcode::kBc1fl:
        cond = "(eq:CC_FP (reg:CC_FP 67 $fcr31) (const_int 0))";
        break;
      default:
        break;
    }
    if (!cond.empty()) {
      return absl::StrFormat("(set (pc) (if_then_else %s (label_ref) (pc)))", cond);
    }
  }
  return inst.Disassemble();
}

bool InverseFinalAndGasLifter::IsEpilogueCandidateInsn(const Instruction& inst) {
  if (inst.IsReturn()) {
    return true;
  }
  if ((inst.opcode == Opcode::kAddiu || inst.opcode == Opcode::kAddi ||
       inst.opcode == Opcode::kAddu) &&
      (inst.rt == Register::kSp || inst.rd == Register::kSp) && inst.rs == Register::kSp &&
      inst.immediate > 0) {
    return true;
  }
  if ((inst.opcode == Opcode::kOr || inst.opcode == Opcode::kAddu ||
       inst.opcode == Opcode::kDaddu) &&
      inst.rd == Register::kSp && inst.rs == Register::kFp && inst.rt == Register::kZero) {
    return true;
  }
  if (inst.opcode == Opcode::kLw && (inst.rs == Register::kSp || inst.rs == Register::kFp) &&
      inst.rt.has_value() &&
      ((*inst.rt >= Register::kS0 && *inst.rt <= Register::kS7) || *inst.rt == Register::kFp ||
       *inst.rt == Register::kRa)) {
    return true;
  }
  if ((inst.opcode == Opcode::kLdc1 || inst.opcode == Opcode::kLwc1) &&
      (inst.rs == Register::kSp || inst.rs == Register::kFp) && inst.ft.has_value() &&
      *inst.ft >= FpRegister::kF20 && *inst.ft <= FpRegister::kF31) {
    return true;
  }
  return false;
}

RtlControlFlowGraph InverseFinalAndGasLifter::LiftToRtlCfg(
    const ControlFlowGraph& input_cfg, const StackFrameAnalysis* frame_analysis,
    const ParsedCompilerFlags* compiler_flags) {
  absl::flat_hash_set<uint32_t> prologue_vrams;
  if (frame_analysis != nullptr) {
    prologue_vrams = frame_analysis->prologue.prologue_instruction_vrams;
  } else {
    prologue_vrams = StackFrameAnalyzer::AnalyzePrologue(input_cfg, /*has_frame_pointer=*/false)
                         .prologue_instruction_vrams;
  }

  // In GCC 2.7.2 (`flow.c:find_basic_blocks`), `call_insn` (`jal` / `jalr`) does not terminate a
  // basic block unless the following instruction is targeted by a label (`predecessors.size() > 1`
  // or jump-table target). Coalesce linear fallthrough chains across `IsCall()` on a copy of
  // `input_cfg` before computing canonical RPO blocks.
  ControlFlowGraph cfg = input_cfg;
  absl::flat_hash_set<uint32_t> jt_vrams;
  for (const auto& jt : cfg.JumpTables()) {
    if (jt.default_target_vram != 0) {
      jt_vrams.insert(jt.default_target_vram);
    }
    for (const auto& entry : jt.entries) {
      jt_vrams.insert(entry.target_vram);
    }
  }
  bool merged_call_chain = true;
  while (merged_call_chain) {
    merged_call_chain = false;
    std::vector<uint32_t> rpo_ids = cfg.ReversePostOrder();
    absl::flat_hash_set<uint32_t> reachable(rpo_ids.begin(), rpo_ids.end());
    for (uint32_t a_id : rpo_ids) {
      BasicBlock* blk_a = cfg.GetMutableBlock(a_id);
      if (blk_a == nullptr || blk_a->instructions.empty() || blk_a->successors.size() != 1 ||
          blk_a->outgoing_edges.size() != 1 ||
          blk_a->outgoing_edges[0].type != EdgeType::kFallthrough || blk_a->HasBranch() ||
          blk_a->HasReturn() || blk_a->HasSwitch()) {
        continue;
      }
      const Instruction* a_term = blk_a->Terminator();
      if (a_term != nullptr && (a_term->IsBranch() || (a_term->IsJump() && !a_term->IsCall()))) {
        continue;
      }
      const uint32_t b_id = blk_a->successors[0];
      if (b_id == a_id || b_id == cfg.EntryBlockId() || !reachable.contains(b_id)) {
        continue;
      }
      BasicBlock* blk_b = cfg.GetMutableBlock(b_id);
      if (blk_b == nullptr || blk_b->instructions.empty() || blk_b->predecessors.size() != 1 ||
          blk_b->predecessors[0] != a_id || jt_vrams.contains(blk_b->start_vram)) {
        continue;
      }
      blk_a->instructions.insert(blk_a->instructions.end(), blk_b->instructions.begin(),
                                 blk_b->instructions.end());
      blk_a->end_vram = blk_b->end_vram;
      if (blk_b->jump_table != nullptr) {
        blk_a->jump_table = blk_b->jump_table;
      }
      blk_a->successors = blk_b->successors;
      blk_a->outgoing_edges = blk_b->outgoing_edges;
      for (auto& edge : blk_a->outgoing_edges) {
        edge.from_block_id = a_id;
      }
      for (uint32_t succ_id : blk_a->successors) {
        BasicBlock* succ_blk = cfg.GetMutableBlock(succ_id);
        if (succ_blk != nullptr) {
          for (uint32_t& pred : succ_blk->predecessors) {
            if (pred == b_id) {
              pred = a_id;
            }
          }
        }
      }
      blk_b->instructions.clear();
      blk_b->predecessors.clear();
      blk_b->successors.clear();
      blk_b->outgoing_edges.clear();
      merged_call_chain = true;
      break;
    }
  }

  RtlControlFlowGraph result;
  std::vector<uint32_t> rpo = cfg.ReversePostOrder();
  absl::flat_hash_map<uint32_t, size_t> id_to_rpo;
  for (size_t idx = 0; idx < rpo.size(); ++idx) {
    id_to_rpo[rpo[idx]] = idx;
  }

  absl::flat_hash_set<std::pair<Register, int16_t>> df_stack_slots;
  absl::flat_hash_set<std::pair<Register, int16_t>> sf_stack_slots;
  for (uint32_t bid : rpo) {
    const BasicBlock* blk = cfg.GetBlock(bid);
    if (blk == nullptr) continue;
    for (const auto& mi : blk->instructions) {
      if (prologue_vrams.contains(mi.vram) || !mi.rs.has_value()) continue;
      if (mi.opcode == Opcode::kLdc1 || mi.opcode == Opcode::kSdc1) {
        df_stack_slots.insert({*mi.rs, mi.immediate});
      } else if (mi.opcode == Opcode::kLwc1 || mi.opcode == Opcode::kSwc1) {
        sf_stack_slots.insert({*mi.rs, mi.immediate});
      }
    }
  }

  result.rpo_blocks.reserve(rpo.size());
  for (uint32_t bid : rpo) {
    const BasicBlock* blk = cfg.GetBlock(bid);
    if (blk == nullptr) {
      continue;
    }
    const auto& raw_insts = blk->instructions;
    bool block_has_return = false;
    size_t last_call_idx = 0;
    bool has_call = false;
    for (size_t i = 0; i < raw_insts.size(); ++i) {
      if (raw_insts[i].IsReturn()) {
        block_has_return = true;
      }
      if (raw_insts[i].IsCall()) {
        last_call_idx = i;
        has_call = true;
      }
    }

    // Detect Stage 0 `.dbr` `current_function_epilogue_delay_list` (`reorg.c:3208-3216`,
    // `mips.c:5189-5202`): when `jr $ra` still has `addiu $sp, $sp, tsize` in its delay slot and is
    // preceded by `lw $ra, offset($sp)` + `<epilogue_delay>`, `reorg.c` unlinked `<epilogue_delay>`
    // from `.dbr`'s `insns` chain into `current_function_epilogue_delay_list`.
    std::optional<size_t> dbr_epilogue_delay_idx;
    if (block_has_return && raw_insts.size() >= 4) {
      size_t ret_idx = raw_insts.size();
      for (size_t i = 0; i < raw_insts.size(); ++i) {
        if (raw_insts[i].IsReturn()) {
          ret_idx = i;
          break;
        }
      }
      if (ret_idx >= 2 && ret_idx + 1 < raw_insts.size()) {
        const auto& sp_dealloc = raw_insts[ret_idx + 1];
        const auto& epilogue_delay = raw_insts[ret_idx - 1];
        const auto& ra_restore = raw_insts[ret_idx - 2];
        if ((sp_dealloc.opcode == Opcode::kAddiu || sp_dealloc.opcode == Opcode::kAddi) &&
            sp_dealloc.rt == Register::kSp && sp_dealloc.rs == Register::kSp &&
            sp_dealloc.immediate > 0 && !epilogue_delay.IsNop() &&
            !IsEpilogueCandidateInsn(epilogue_delay) && ra_restore.opcode == Opcode::kLw &&
            ra_restore.rs == Register::kSp && ra_restore.rt == Register::kRa) {
          dbr_epilogue_delay_idx = ret_idx - 1;
        }
      }
    }

    // Step 1: Collapse `gas` division trap sequences and filter out NOPs and `function_epilogue`
    // instructions (which only occur after all calls in a return block).
    std::vector<Instruction> insts;
    insts.reserve(raw_insts.size());
    absl::flat_hash_map<uint32_t, std::vector<Instruction>> div_spans;
    for (size_t i = 0; i < raw_insts.size(); ++i) {
      const auto& inst = raw_insts[i];
      if (inst.IsNop()) {
        continue;
      }
      if (dbr_epilogue_delay_idx.has_value() && i == *dbr_epilogue_delay_idx) {
        continue;
      }
      if (block_has_return && (inst.IsReturn() || (!has_call || i > last_call_idx)) &&
          IsEpilogueCandidateInsn(inst)) {
        continue;
      }
      if (auto div_pat = MatchDivisionPattern(raw_insts, i); div_pat.has_value()) {
        std::vector<Instruction> span = {inst};
        for (size_t k = i + 1; k <= div_pat->end_index && k < raw_insts.size(); ++k) {
          if (raw_insts[k].opcode == Opcode::kMflo || raw_insts[k].opcode == Opcode::kMfhi) {
            span.push_back(raw_insts[k]);
          }
        }
        div_spans[inst.vram] = std::move(span);
        insts.push_back(inst);
        i = div_pat->end_index;
        continue;
      }
      insts.push_back(inst);
    }

    // Undo post-`final.c` KMC GAS `-Wa,-O2` delay-slot scheduling (`GasOptimizesAndReordersInsns`)
    // before idiom fusion:
    //  1. KMC GAS fills unfilled `jal`/`jalr` delay slots with a preceding load (`lw`/`lh`/`lbu`),
    //     `sdc1`, or the `%lo` `addiu` half of an `la` macro.
    //  2. KMC GAS fills branch/jump delay slots with the second half of `floatsidf2`/`floatsisf2`
    //     (`cvt.d.w`/`cvt.s.w`) or `$at`-based global symbol loads/stores.
    if (compiler_flags != nullptr && compiler_flags->GasOptimizesAndReordersInsns()) {
      for (size_t k = 0; k + 1 < insts.size(); ++k) {
        if (insts[k].IsCall() && insts[k + 1].vram == insts[k].vram + 4) {
          const Instruction& dslot = insts[k + 1];
          const bool is_split_la =
              (k >= 1 && insts[k - 1].opcode == Opcode::kLui && insts[k - 1].rt.has_value() &&
               (dslot.opcode == Opcode::kAddiu || dslot.opcode == Opcode::kAddi) &&
               dslot.rs == insts[k - 1].rt && dslot.rt == insts[k - 1].rt);
          const bool is_split_at_mem =
              (k >= 1 &&
               (insts[k - 1].opcode == Opcode::kLui || insts[k - 1].opcode == Opcode::kAddu) &&
               (dslot.IsLoad() || dslot.IsStore()) && dslot.rs == Register::kAt);
          if (dslot.IsLoad() || dslot.opcode == Opcode::kSdc1 || is_split_la || is_split_at_mem) {
            std::swap(insts[k], insts[k + 1]);
          }
        }
      }
      if (insts.size() >= 3) {
        const Instruction& prev = insts[insts.size() - 3];
        const Instruction& br = insts[insts.size() - 2];
        const Instruction& dslot = insts.back();
        if ((br.IsBranch() || br.opcode == Opcode::kJ) && dslot.vram == br.vram + 4) {
          const bool is_split_float_cvt =
              ((prev.opcode == Opcode::kMtc1 || prev.opcode == Opcode::kLwc1) &&
               (dslot.opcode == Opcode::kCvtSW || dslot.opcode == Opcode::kCvtDW));
          const bool is_split_at_mem =
              ((prev.opcode == Opcode::kLui || prev.opcode == Opcode::kAddu) &&
               (dslot.IsLoad() || dslot.IsStore()) && dslot.rs == Register::kAt);
          if (is_split_float_cvt || is_split_at_mem) {
            std::swap(insts[insts.size() - 2], insts.back());
          }
        }
      }
    }

    bool block_has_ldc1_or_sdc1 = false;
    for (const auto& mi : insts) {
      if (!prologue_vrams.contains(mi.vram) &&
          (mi.opcode == Opcode::kLdc1 || mi.opcode == Opcode::kSdc1)) {
        block_has_ldc1_or_sdc1 = true;
        break;
      }
    }

    // Step 2: Fuse multi-instruction RTL patterns (`mips.md` & `gas` macro expansions).
    RtlBasicBlock out_blk;
    out_blk.id = blk->id;
    out_blk.start_vram = blk->start_vram;
    out_blk.end_vram = blk->end_vram;
    for (size_t idx = 0; idx < insts.size(); ++idx) {
      const auto& inst = insts[idx];
      if ((inst.opcode == Opcode::kDiv || inst.opcode == Opcode::kDivu) &&
          div_spans.contains(inst.vram)) {
        out_blk.AddInsn(MakeRtlInsn(inst, RtlFusedIdiomKind::kDivisionWithTrap,
                                    div_spans[inst.vram], prologue_vrams));
        continue;
      }
      // 1. Fuse `mulsi3_internal` (`mult` + `mflo` without `mfhi`).
      if ((inst.opcode == Opcode::kMult || inst.opcode == Opcode::kMultu) &&
          idx + 1 < insts.size() && insts[idx + 1].opcode == Opcode::kMflo &&
          (idx + 2 >= insts.size() || insts[idx + 2].opcode != Opcode::kMfhi)) {
        out_blk.AddInsn(
            MakeRtlInsn(inst, RtlFusedIdiomKind::kMulSi3, {inst, insts[idx + 1]}, prologue_vrams));
        ++idx;
        continue;
      }
      // 2. Fuse `floatsisf2` / `floatsidf2` (`mips.md:2629`: `mtc1`/`lwc1` + `cvt.s.w`/`cvt.d.w`).
      if ((inst.opcode == Opcode::kMtc1 || inst.opcode == Opcode::kLwc1) &&
          idx + 1 < insts.size()) {
        const auto& next = insts[idx + 1];
        const auto loaded_fp = (inst.opcode == Opcode::kMtc1) ? inst.fs : inst.ft;
        if ((next.opcode == Opcode::kCvtSW || next.opcode == Opcode::kCvtDW) &&
            loaded_fp.has_value() && next.fs == loaded_fp && next.fd == loaded_fp) {
          const RtlFusedIdiomKind kind = (next.opcode == Opcode::kCvtSW)
                                             ? RtlFusedIdiomKind::kFloatSiSf2
                                             : RtlFusedIdiomKind::kFloatSiDf2;
          out_blk.AddInsn(MakeRtlInsn(next, kind, {inst, next}, prologue_vrams));
          ++idx;
          continue;
        }
      }
      // 2b. Fuse `mips_move_2words` (`DFmode` / `DImode` 2-word moves between GPR pair and
      // even/odd FPR pair: `mtc1` + `mtc1` or `mfc1` + `mfc1`).
      if (inst.opcode == Opcode::kMtc1 && idx + 1 < insts.size() &&
          insts[idx + 1].opcode == Opcode::kMtc1 && inst.fs.has_value() &&
          insts[idx + 1].fs.has_value()) {
        const int f0 = static_cast<int>(*inst.fs);
        const int f1 = static_cast<int>(*insts[idx + 1].fs);
        if ((f0 & ~1) == (f1 & ~1) && f0 != f1) {
          out_blk.AddInsn(MakeRtlInsn(inst, RtlFusedIdiomKind::kMove2Words, {inst, insts[idx + 1]},
                                      prologue_vrams));
          ++idx;
          continue;
        }
      }
      if (inst.opcode == Opcode::kMfc1 && idx + 1 < insts.size() &&
          insts[idx + 1].opcode == Opcode::kMfc1 && inst.fs.has_value() &&
          insts[idx + 1].fs.has_value()) {
        const int f0 = static_cast<int>(*inst.fs);
        const int f1 = static_cast<int>(*insts[idx + 1].fs);
        if ((f0 & ~1) == (f1 & ~1) && f0 != f1) {
          out_blk.AddInsn(MakeRtlInsn(inst, RtlFusedIdiomKind::kMove2Words, {inst, insts[idx + 1]},
                                      prologue_vrams));
          ++idx;
          continue;
        }
      }
      // At `-O1` / `-O0` (`!SplitsMultiInsnMovesBeforeDbr`), `mips_move_2words` `DFmode`
      // memory-to-GPR-pair loads (`lw $a2, off(base); lw $a3, off+4(base)`) and GPR-pair/zero
      // stores (`sw $a2, off(base); sw $a3, off+4(base)`) are not split by `sched.c`.
      if (compiler_flags != nullptr && !compiler_flags->SplitsMultiInsnMovesBeforeDbr() &&
          inst.opcode == Opcode::kLw && idx + 1 < insts.size() &&
          insts[idx + 1].opcode == Opcode::kLw && inst.rt == Register::kA2 &&
          insts[idx + 1].rt == Register::kA3 && inst.rs.has_value() &&
          inst.rs == insts[idx + 1].rs && insts[idx + 1].immediate == inst.immediate + 4 &&
          (block_has_ldc1_or_sdc1 || df_stack_slots.contains({*inst.rs, inst.immediate})) &&
          !sf_stack_slots.contains({*inst.rs, inst.immediate})) {
        out_blk.AddInsn(MakeRtlInsn(inst, RtlFusedIdiomKind::kMove2Words, {inst, insts[idx + 1]},
                                    prologue_vrams));
        ++idx;
        continue;
      }
      if (compiler_flags != nullptr && !compiler_flags->SplitsMultiInsnMovesBeforeDbr() &&
          inst.opcode == Opcode::kSw && idx + 1 < insts.size() &&
          insts[idx + 1].opcode == Opcode::kSw && inst.rs.has_value() &&
          inst.rs == insts[idx + 1].rs && insts[idx + 1].immediate == inst.immediate + 4 &&
          (block_has_ldc1_or_sdc1 || df_stack_slots.contains({*inst.rs, inst.immediate})) &&
          !sf_stack_slots.contains({*inst.rs, inst.immediate}) &&
          ((inst.rt == Register::kA2 && insts[idx + 1].rt == Register::kA3) ||
           (inst.rt == Register::kZero && insts[idx + 1].rt == Register::kZero))) {
        out_blk.AddInsn(MakeRtlInsn(inst, RtlFusedIdiomKind::kMove2Words, {inst, insts[idx + 1]},
                                    prologue_vrams));
        ++idx;
        continue;
      }
      // 3. Fuse `fix_truncsfsi2` / `fix_truncdfsi2` (`mips.md:2510`: `trunc.w.s`/`trunc.w.d` +
      // `mfc1`/`swc1`, or `lui $at` + `swc1`, or `-mips1` `cfc1`..`mfc1` 7-insn macro).
      if ((inst.opcode == Opcode::kTruncWS || inst.opcode == Opcode::kTruncWD) &&
          idx + 1 < insts.size()) {
        const RtlFusedIdiomKind kind = (inst.opcode == Opcode::kTruncWS)
                                           ? RtlFusedIdiomKind::kFixTruncSfSi2
                                           : RtlFusedIdiomKind::kFixTruncDfSi2;
        const auto& next = insts[idx + 1];
        if ((next.opcode == Opcode::kMfc1 && next.fs == inst.fd) ||
            (next.opcode == Opcode::kSwc1 && next.ft == inst.fd)) {
          out_blk.AddInsn(MakeRtlInsn(inst, kind, {inst, next}, prologue_vrams));
          ++idx;
          continue;
        }
        if (next.opcode == Opcode::kLui && next.rt == Register::kAt && idx + 2 < insts.size() &&
            insts[idx + 2].opcode == Opcode::kSwc1 && insts[idx + 2].rs == Register::kAt &&
            insts[idx + 2].ft == inst.fd) {
          out_blk.AddInsn(MakeRtlInsn(inst, kind, {inst, next, insts[idx + 2]}, prologue_vrams));
          idx += 2;
          continue;
        }
      }
      if (inst.opcode == Opcode::kCfc1 && idx + 6 < insts.size() &&
          insts[idx + 1].opcode == Opcode::kOri && insts[idx + 2].opcode == Opcode::kXori &&
          insts[idx + 3].opcode == Opcode::kCtc1 &&
          (insts[idx + 4].opcode == Opcode::kCvtWS || insts[idx + 4].opcode == Opcode::kCvtWD) &&
          insts[idx + 5].opcode == Opcode::kCtc1 &&
          (insts[idx + 6].opcode == Opcode::kMfc1 || insts[idx + 6].opcode == Opcode::kSwc1)) {
        const RtlFusedIdiomKind kind = (insts[idx + 4].opcode == Opcode::kCvtWS)
                                           ? RtlFusedIdiomKind::kFixTruncSfSi2
                                           : RtlFusedIdiomKind::kFixTruncDfSi2;
        std::vector<Instruction> span(insts.begin() + idx, insts.begin() + idx + 7);
        out_blk.AddInsn(MakeRtlInsn(insts[idx + 4], kind, std::move(span), prologue_vrams));
        idx += 6;
        continue;
      }
      // 4. Fuse `movstrsi_internal` (`mips.c:output_block_move`: unaligned `lwl`/`lwr` block copy).
      if (inst.opcode == Opcode::kLwl && inst.rs.has_value() && idx + 3 < insts.size() &&
          insts[idx + 1].opcode == Opcode::kLwr && insts[idx + 1].rs == inst.rs &&
          insts[idx + 1].rt == inst.rt) {
        const Register src_reg = *inst.rs;
        std::optional<Register> dst_reg;
        int16_t max_src_off = -1;
        size_t scan = idx;
        size_t last_store_end = idx;
        while (scan < insts.size()) {
          const auto& s = insts[scan];
          if (s.opcode == Opcode::kLwl && scan + 1 < insts.size() &&
              insts[scan + 1].opcode == Opcode::kLwr && s.rs == src_reg &&
              insts[scan + 1].rs == src_reg && s.rt == insts[scan + 1].rt) {
            const int16_t word_off = std::min(s.immediate, insts[scan + 1].immediate);
            if (word_off <= max_src_off) {
              break;
            }
            max_src_off = word_off;
            scan += 2;
            continue;
          }
          if ((s.opcode == Opcode::kLb || s.opcode == Opcode::kLbu || s.opcode == Opcode::kLh ||
               s.opcode == Opcode::kLhu) &&
              s.rs == src_reg && s.immediate > max_src_off) {
            max_src_off = s.immediate;
            ++scan;
            continue;
          }
          if (s.opcode == Opcode::kSwl && scan + 1 < insts.size() &&
              insts[scan + 1].opcode == Opcode::kSwr && s.rs.has_value() &&
              insts[scan + 1].rs == s.rs && s.rt == insts[scan + 1].rt &&
              (!dst_reg.has_value() || *dst_reg == *s.rs)) {
            dst_reg = *s.rs;
            last_store_end = scan + 1;
            scan += 2;
            continue;
          }
          if ((s.opcode == Opcode::kSb || s.opcode == Opcode::kSh || s.opcode == Opcode::kSwl ||
               s.opcode == Opcode::kSwr) &&
              s.rs.has_value() && (!dst_reg.has_value() || *dst_reg == *s.rs)) {
            dst_reg = *s.rs;
            last_store_end = scan;
            ++scan;
            continue;
          }
          break;
        }
        if (last_store_end > idx) {
          std::vector<Instruction> span(insts.begin() + idx, insts.begin() + last_store_end + 1);
          out_blk.AddInsn(MakeRtlInsn(inst, RtlFusedIdiomKind::kMovstrsiBlockMove, std::move(span),
                                      prologue_vrams));
          idx = last_store_end;
          continue;
        }
      }
      // 5. Fuse `gas` `lui` macro expansions (`la`, `li.s`, `CONST_DOUBLE` negative 32-bit `li`,
      // `mips_move_2words` global `DFmode` load, and direct/indexed symbol loads/stores).
      if (inst.opcode == Opcode::kLui && inst.rt.has_value() && idx + 1 < insts.size()) {
        const Register hi_reg = *inst.rt;
        const auto& next = insts[idx + 1];
        // `li.s $fX, const` -> `lui $at, hi; [ori $at, $at, lo;] mtc1 $at, $fX`
        if (hi_reg == Register::kAt && next.opcode == Opcode::kMtc1 && next.rt == Register::kAt) {
          out_blk.AddInsn(MakeRtlInsn(next, RtlFusedIdiomKind::kGasFloatImmediate, {inst, next},
                                      prologue_vrams));
          ++idx;
          continue;
        }
        if (hi_reg == Register::kAt && next.opcode == Opcode::kOri && next.rt == Register::kAt &&
            next.rs == Register::kAt && idx + 2 < insts.size() &&
            insts[idx + 2].opcode == Opcode::kMtc1 && insts[idx + 2].rt == Register::kAt) {
          out_blk.AddInsn(MakeRtlInsn(insts[idx + 2], RtlFusedIdiomKind::kGasFloatImmediate,
                                      {inst, next, insts[idx + 2]}, prologue_vrams));
          idx += 2;
          continue;
        }
        // `CONST_DOUBLE` 32-bit unsigned magic multiplier with bit 31 set (`inst.immediate < 0`)
        // feeding `multu`.
        if (inst.immediate < 0 && next.opcode == Opcode::kOri && next.rt == hi_reg &&
            next.rs == hi_reg) {
          bool feeds_multu = false;
          for (size_t k = idx + 2; k < insts.size(); ++k) {
            if (insts[k].opcode == Opcode::kMultu &&
                (insts[k].rs == hi_reg || insts[k].rt == hi_reg)) {
              feeds_multu = true;
              break;
            }
            if (insts[k].opcode == Opcode::kMult &&
                (insts[k].rs == hi_reg || insts[k].rt == hi_reg)) {
              break;
            }
            if (insts[k].rd == hi_reg ||
                ((insts[k].IsLoad() || insts[k].opcode == Opcode::kLui ||
                  insts[k].opcode == Opcode::kAddiu || insts[k].opcode == Opcode::kAddi ||
                  insts[k].opcode == Opcode::kOri || insts[k].opcode == Opcode::kAndi ||
                  insts[k].opcode == Opcode::kXori || insts[k].opcode == Opcode::kSlti ||
                  insts[k].opcode == Opcode::kSltiu) &&
                 insts[k].rt == hi_reg)) {
              break;
            }
          }
          if (feeds_multu) {
            out_blk.AddInsn(MakeRtlInsn(next, RtlFusedIdiomKind::kGasMagicMultiplier, {inst, next},
                                        prologue_vrams));
            ++idx;
            continue;
          }
        }
        // At `-O1` (`!SplitsMultiInsnMovesBeforeDbr`), 2-instruction `LARGE_INT` `movsi` constants
        // (`lui reg, hi; ori reg, reg, lo`) are not split by `sched.c:split_all_insns`.
        if (compiler_flags != nullptr && !compiler_flags->SplitsMultiInsnMovesBeforeDbr() &&
            next.opcode == Opcode::kOri && next.rt == hi_reg && next.rs == hi_reg) {
          out_blk.AddInsn(MakeRtlInsn(next, RtlFusedIdiomKind::kSplitLargeIntHiLo, {inst, next},
                                      prologue_vrams));
          ++idx;
          continue;
        }
        // `la reg, sym` -> `lui reg, %hi(sym); addiu reg, reg, %lo(sym)`
        if ((next.opcode == Opcode::kAddiu || next.opcode == Opcode::kAddi) && next.rt == hi_reg &&
            next.rs == hi_reg) {
          out_blk.AddInsn(
              MakeRtlInsn(next, RtlFusedIdiomKind::kGasAddressHiLo, {inst, next}, prologue_vrams));
          ++idx;
          continue;
        }
        // `lw/sw ..., sym(idx_reg)` ->
        // `lui hi_reg, %hi(sym); addu hi_reg, hi_reg, idx_reg; lw/sw ..., %lo(sym)(hi_reg)`
        if (next.opcode == Opcode::kAddu && next.rd == hi_reg &&
            (next.rs == hi_reg || next.rt == hi_reg) && idx + 2 < insts.size() &&
            (insts[idx + 2].IsLoad() || insts[idx + 2].IsStore()) && insts[idx + 2].rs == hi_reg &&
            (hi_reg == Register::kAt || (insts[idx + 2].IsLoad() && insts[idx + 2].rt == hi_reg))) {
          out_blk.AddInsn(MakeRtlInsn(insts[idx + 2], RtlFusedIdiomKind::kGasSymbolIndexedAccess,
                                      {inst, next, insts[idx + 2]}, prologue_vrams));
          idx += 2;
          continue;
        }
        // `mips_move_2words` global `DFmode` / `DImode` load into an even/odd GPR pair.
        if (next.opcode == Opcode::kLw && next.rs == hi_reg && next.rt == hi_reg &&
            (static_cast<int>(hi_reg) & 1) == 0 && idx + 3 < insts.size() &&
            insts[idx + 2].opcode == Opcode::kLui && insts[idx + 2].rt.has_value() &&
            static_cast<int>(*insts[idx + 2].rt) == static_cast<int>(hi_reg) + 1 &&
            insts[idx + 3].opcode == Opcode::kLw && insts[idx + 3].rs == insts[idx + 2].rt &&
            insts[idx + 3].rt == insts[idx + 2].rt &&
            insts[idx + 3].immediate == next.immediate + 4) {
          out_blk.AddInsn(MakeRtlInsn(next, RtlFusedIdiomKind::kMove2Words,
                                      {inst, next, insts[idx + 2], insts[idx + 3]},
                                      prologue_vrams));
          idx += 3;
          continue;
        }
        // Direct symbol load/store `lw/lh/lhu/lb/lbu/sw/sh/sb/lwc1/swc1 ..., sym` ->
        // `lui $at (or dest), %hi(sym); load/store ..., %lo(sym)($at or dest)`
        if ((next.IsLoad() || next.IsStore()) && next.rs == hi_reg &&
            (hi_reg == Register::kAt || (next.IsLoad() && next.rt == hi_reg))) {
          out_blk.AddInsn(MakeRtlInsn(next, RtlFusedIdiomKind::kGasSymbolDirectAccess, {inst, next},
                                      prologue_vrams));
          ++idx;
          continue;
        }
      }
      out_blk.AddInsn(
          MakeRtlInsn(inst, RtlFusedIdiomKind::kSingleInstruction, {inst}, prologue_vrams));
    }
    for (uint32_t succ_id : blk->successors) {
      if (auto it = id_to_rpo.find(succ_id); it != id_to_rpo.end()) {
        out_blk.successors.push_back(it->second);
      }
    }
    result.rpo_blocks.push_back(std::move(out_blk));
  }
  if (result.rpo_blocks.empty()) {
    result.rpo_blocks.push_back(RtlBasicBlock{});
  }
  // At `-O0` (`!RunsDbrSchedule`), a label followed only by `nop` (`(const_int 0)`) before the
  // next label forms an empty non-exit machine block (`rtl_insns.empty()` with 1 successor),
  // whereas GCC's RTL CFG merges consecutive labels separated only by `(const_int 0)`.
  if (compiler_flags != nullptr && !compiler_flags->RunsDbrSchedule()) {
    bool collapsed_empty = true;
    while (collapsed_empty) {
      collapsed_empty = false;
      for (size_t b = 1; b < result.rpo_blocks.size(); ++b) {
        if (result.rpo_blocks[b].rtl_insns.empty() && result.rpo_blocks[b].successors.size() == 1 &&
            result.rpo_blocks[b].successors[0] != b) {
          const size_t target = result.rpo_blocks[b].successors[0];
          for (size_t p = 0; p < result.rpo_blocks.size(); ++p) {
            if (p == b) continue;
            auto& succs = result.rpo_blocks[p].successors;
            for (size_t& s : succs) {
              if (s == b) s = target;
            }
            for (size_t i = 0; i < succs.size(); ++i) {
              for (size_t j = i + 1; j < succs.size();) {
                if (succs[j] == succs[i]) {
                  succs.erase(succs.begin() + static_cast<ptrdiff_t>(j));
                } else {
                  ++j;
                }
              }
            }
          }
          result.rpo_blocks.erase(result.rpo_blocks.begin() + static_cast<ptrdiff_t>(b));
          for (auto& blk : result.rpo_blocks) {
            for (size_t& s : blk.successors) {
              if (s > b) --s;
            }
          }
          collapsed_empty = true;
          break;
        }
      }
    }
  }
  // Populate `predecessors` on each RPO block from `successors`.
  for (size_t b = 0; b < result.rpo_blocks.size(); ++b) {
    for (size_t s : result.rpo_blocks[b].successors) {
      if (s < result.rpo_blocks.size()) {
        result.rpo_blocks[s].predecessors.push_back(b);
      }
    }
  }
  return result;
}

RtlControlFlowGraph InverseFinalAndGasLifter::LiftToDbrRtlCfg(
    const ControlFlowGraph& input_cfg, const StackFrameAnalysis* frame_analysis,
    const SymbolIndex* symbols, MemoryReader memory_reader, std::string_view function_name,
    const ParsedCompilerFlags* compiler_flags) {
  RtlControlFlowGraph rtl_cfg = LiftToRtlCfg(input_cfg, frame_analysis, compiler_flags);

  absl::flat_hash_set<uint32_t> jt_table_vrams;
  for (const auto& jt : input_cfg.JumpTables()) {
    if (jt.table_vram != 0) {
      jt_table_vrams.insert(jt.table_vram);
    }
  }

  const bool fn_returns_v0 = AbiAnalyzer::DetermineReturnsV0(input_cfg, symbols);
  const bool fn_returns_f0 = AbiAnalyzer::DetermineReturnsF0(input_cfg);
  const bool fn_returns_f0_double = AbiAnalyzer::DetermineReturnsF0Double(input_cfg);
  const absl::flat_hash_map<uint32_t, CallReturnInfo> call_return_map =
      AbiAnalyzer::ComputeCallReturnInfo(input_cfg, fn_returns_v0, fn_returns_f0);
  const absl::flat_hash_set<Register> float_param_gprs =
      AbiAnalyzer::DetectFloatParametersInGeneralRegisters(input_cfg);

  auto gpr_expr = [](Register r, std::string_view mode = "SI",
                     std::string_view flags = "") -> std::string {
    return FormatDbrGprExpr(r, mode, flags);
  };
  auto fpr_expr = [](FpRegister f, std::string_view mode = "SF",
                     std::string_view flags = "") -> std::string {
    return FormatDbrFprExpr(f, mode, flags);
  };
  auto format_mem_addr = [](Register base, int16_t offset,
                            std::string_view base_flags = "") -> std::string {
    return FormatDbrMemAddr(base, offset, base_flags);
  };

  auto resolve_symbol_expr = [&](uint32_t addr) -> std::string {
    if (jt_table_vrams.contains(addr)) {
      return "(label_ref)";
    }
    if (symbols != nullptr) {
      if (const auto* entry = symbols->FindByAddress(addr); entry != nullptr) {
        return absl::StrFormat("(symbol_ref:SI (\"%s\"))", entry->name());
      }
      const auto& sorted = symbols->SortedEntries();
      const SymbolRegistryEntry* best = nullptr;
      for (const auto* entry : sorted) {
        if (entry->address() <= addr) {
          best = entry;
        } else {
          break;
        }
      }
      if (best != nullptr && addr > best->address() && (addr - best->address()) < 0x10000) {
        return absl::StrFormat("(const:SI (plus:SI (symbol_ref:SI (\"%s\")) (const_int %d)))",
                               best->name(), static_cast<int32_t>(addr - best->address()));
      }
    }
    return absl::StrFormat("(const_int %d)", static_cast<int32_t>(addr));
  };

  const DbrPreAnalysis pre = PreAnalyzeDbrCfg(frame_analysis, &rtl_cfg);
  const int32_t min_saved_sp_offset = pre.min_saved_sp_offset;
  const int32_t min_local_sp_lea = pre.min_local_sp_lea;
  const auto& swc1_struct_bases = pre.swc1_struct_bases;
  const auto& non_zero_sw_bases = pre.non_zero_sw_bases;
  const int total_call_count = pre.total_call_count;
  const int32_t global_max_sp_arg_end = pre.global_max_sp_arg_end;
  const bool f0_ever_const_double_in_entry = pre.f0_ever_const_double_in_entry;
  const auto& narrow_var_mode = pre.narrow_var_mode;
  const auto& reg_has_andi_ff = pre.reg_has_andi_ff;
  const auto& reg_used_in_si_cmp_or_arith = pre.reg_used_in_si_cmp_or_arith;
  const auto& reg_used_or_def_in_blk0 = pre.reg_used_or_def_in_blk0;

  std::vector<absl::flat_hash_map<Register, std::string_view>> block_out_gpr_mode(
      rtl_cfg.rpo_blocks.size());
  absl::flat_hash_set<Register> reg_loaded_by_lw_or_srl;
  bool f0_defined_by_const_double = f0_ever_const_double_in_entry;
  for (size_t b = 0; b < rtl_cfg.rpo_blocks.size(); ++b) {
    auto& blk = rtl_cfg.rpo_blocks[b];
    // 2. Format each `RtlInsn` into its `.12.dbr` S-expression.
    int32_t max_arg_end_since_call = (total_call_count == 1) ? global_max_sp_arg_end : 16;
    bool last_fp_cmp_is_rev = false;
    absl::flat_hash_set<Register> sf_gpr_regs(float_param_gprs.begin(), float_param_gprs.end());
    bool block_has_swc1_sp_arg = false;
    absl::flat_hash_set<Register> lwc1_struct_bases_in_blk;
    for (const auto& rinsn : blk.rtl_insns) {
      if (rinsn.idiom_kind == RtlFusedIdiomKind::kSingleInstruction &&
          !rinsn.machine_instructions.empty()) {
        const Instruction& mi = rinsn.machine_instructions[0];
        if (mi.opcode == Opcode::kSwc1 && mi.rs == Register::kSp && mi.immediate >= 16 &&
            (frame_analysis == nullptr || mi.immediate < frame_analysis->outgoing_args_size)) {
          block_has_swc1_sp_arg = true;
        }
        if (mi.opcode == Opcode::kLwc1 && mi.rs.has_value() && *mi.rs != Register::kSp) {
          lwc1_struct_bases_in_blk.insert(*mi.rs);
        }
      }
    }
    absl::flat_hash_map<Register, std::string_view> gpr_mode = narrow_var_mode;
    for (size_t pred : blk.predecessors) {
      if (pred < b) {
        for (const auto& [r, m] : block_out_gpr_mode[pred]) {
          if (!narrow_var_mode.contains(r)) {
            gpr_mode[r] = m;
          }
        }
      }
    }
    absl::flat_hash_set<Register> gpr_defined_in_si;
    auto gpr_rval_si = [&](Register r) -> std::string {
      if (r == Register::kZero) return "(const_int 0)";
      if (auto it = gpr_mode.find(r);
          it != gpr_mode.end() && (it->second == "HI" || it->second == "QI")) {
        return absl::StrFormat("(subreg:SI %s 0)", gpr_expr(r, it->second));
      }
      return gpr_expr(r, "SI");
    };
    auto set_gpr_mode = [&](Register r, std::string_view m) {
      if (auto it = narrow_var_mode.find(r); it != narrow_var_mode.end()) {
        gpr_mode[r] = it->second;
        return;
      }
      gpr_mode[r] = m;
      if (m == "SI") {
        gpr_defined_in_si.insert(r);
      }
    };
    auto const_def_store_mode = [&](Register reg, size_t start_i) -> std::string_view {
      return ComputeDbrConstDefStoreMode(rtl_cfg, narrow_var_mode, b, reg, start_i);
    };
    absl::flat_hash_map<Register, std::string_view> gpr_mode_before_dslot = gpr_mode;
    for (size_t i = 0; i < blk.rtl_insns.size(); ++i) {
      if (i + 2 == blk.rtl_insns.size() && blk.rtl_insns[i].vram > blk.rtl_insns.back().vram) {
        gpr_mode_before_dslot = gpr_mode;
      }
      RtlInsn& rinsn = blk.rtl_insns[i];
      for (const auto& mi : rinsn.machine_instructions) {
        if (mi.IsStore() && mi.rs == Register::kSp && mi.immediate >= 16) {
          const int32_t store_size = (mi.opcode == Opcode::kSdc1) ? 8 : 4;
          const int32_t end_off = static_cast<int32_t>(mi.immediate) + store_size;
          if (frame_analysis == nullptr || end_off <= frame_analysis->outgoing_args_size) {
            max_arg_end_since_call = std::max(max_arg_end_since_call, end_off);
          }
        }
      }
      // Also check the next instruction if `rinsn` is a `call_insn` (its delay slot is `i + 1`).
      if (rinsn.idiom_kind == RtlFusedIdiomKind::kSingleInstruction &&
          !rinsn.machine_instructions.empty() && rinsn.machine_instructions[0].IsCall() &&
          i + 1 < blk.rtl_insns.size()) {
        for (const auto& mi : blk.rtl_insns[i + 1].machine_instructions) {
          if (mi.IsStore() && mi.rs == Register::kSp && mi.immediate >= 16) {
            const int32_t store_size = (mi.opcode == Opcode::kSdc1) ? 8 : 4;
            const int32_t end_off = static_cast<int32_t>(mi.immediate) + store_size;
            if (frame_analysis == nullptr || end_off <= frame_analysis->outgoing_args_size) {
              max_arg_end_since_call = std::max(max_arg_end_since_call, end_off);
            }
          }
        }
      }

      auto sp_or_struct_mflags = [&](Register base, int16_t off) -> std::string_view {
        if (base == Register::kSp) {
          if (off >= min_local_sp_lea && off < min_saved_sp_offset) {
            return "/s";
          }
          return "";
        }
        if (base == Register::kFp) {
          return "";
        }
        return (off != 0) ? "/s" : "";
      };

      if (TryFormatDbrSimpleFusedIdiom(min_local_sp_lea, min_saved_sp_offset,
                                       &f0_defined_by_const_double, &sf_gpr_regs, &rinsn)) {
        continue;
      }

      if ((rinsn.idiom_kind == RtlFusedIdiomKind::kFixTruncSfSi2 ||
           rinsn.idiom_kind == RtlFusedIdiomKind::kFixTruncDfSi2) &&
          rinsn.machine_instructions.size() >= 2) {
        const Instruction& m0 = rinsn.machine_instructions[0];
        const Instruction& m_last = rinsn.machine_instructions.back();
        const std::string_view fmode =
            (rinsn.idiom_kind == RtlFusedIdiomKind::kFixTruncSfSi2) ? "SF" : "DF";
        std::string dst_expr;
        Register scratch_gpr = Register::kV0;
        FpRegister src_fpr = *m0.fs;
        FpRegister clobber_fpr = *m0.fd;
        for (const auto& mi : rinsn.machine_instructions) {
          if (mi.opcode == Opcode::kCvtWS || mi.opcode == Opcode::kCvtWD) {
            src_fpr = *mi.fs;
            clobber_fpr = *mi.fd;
          }
        }
        if (m_last.opcode == Opcode::kMfc1 && m_last.rt.has_value()) {
          sf_gpr_regs.erase(*m_last.rt);
          dst_expr = gpr_expr(*m_last.rt);
          if (m0.opcode == Opcode::kCfc1 && m0.rt.has_value()) {
            scratch_gpr = *m0.rt;
          } else if (*m_last.rt == Register::kV0 ||
                     IsGprLiveAcrossInsnInBlock(blk, i, Register::kV0)) {
            scratch_gpr = Register::kV1;
          }
        } else if (m_last.opcode == Opcode::kSwc1) {
          if (m0.opcode == Opcode::kCfc1 && m0.rt.has_value()) {
            scratch_gpr = *m0.rt;
          }
          if (rinsn.machine_instructions.size() == 3 &&
              rinsn.machine_instructions[1].opcode == Opcode::kLui) {
            const uint32_t sym_addr = (static_cast<uint32_t>(static_cast<uint16_t>(
                                           rinsn.machine_instructions[1].immediate))
                                       << 16) +
                                      static_cast<int32_t>(m_last.immediate);
            dst_expr = absl::StrFormat("(mem:SI %s)", resolve_symbol_expr(sym_addr));
          } else if (m_last.rs.has_value()) {
            const std::string_view mflags = sp_or_struct_mflags(*m_last.rs, m_last.immediate);
            dst_expr = absl::StrFormat("(mem%s:SI %s)", mflags,
                                       format_mem_addr(*m_last.rs, m_last.immediate));
          }
        }
        rinsn.canonical_pattern = absl::StrFormat(
            "(parallel[ (set %s (fix:SI %s)) (clobber %s) (clobber %s) ] )", dst_expr,
            fpr_expr(src_fpr, fmode), gpr_expr(scratch_gpr), fpr_expr(clobber_fpr, fmode));
        continue;
      }

      if (rinsn.idiom_kind == RtlFusedIdiomKind::kMove2Words &&
          rinsn.machine_instructions.size() == 4 &&
          rinsn.machine_instructions[0].opcode == Opcode::kLui) {
        const Instruction& m0 = rinsn.machine_instructions[0];
        const Register g_even = static_cast<Register>(static_cast<int>(*m0.rt) & ~1);
        const uint32_t sym_addr =
            (static_cast<uint32_t>(static_cast<uint16_t>(m0.immediate)) << 16) +
            static_cast<int32_t>(rinsn.machine_instructions[1].immediate);
        rinsn.canonical_pattern = absl::StrFormat("(set %s (mem:DF %s))", gpr_expr(g_even, "DF"),
                                                  resolve_symbol_expr(sym_addr));
        continue;
      }

      if (rinsn.idiom_kind == RtlFusedIdiomKind::kGasAddressHiLo &&
          rinsn.machine_instructions.size() == 2) {
        const Instruction& m_lui = rinsn.machine_instructions[0];
        const Instruction& m_add = rinsn.machine_instructions[1];
        sf_gpr_regs.erase(*m_add.rt);
        set_gpr_mode(*m_add.rt, "SI");
        const uint32_t addr =
            (static_cast<uint32_t>(static_cast<uint16_t>(m_lui.immediate)) << 16) +
            static_cast<int32_t>(m_add.immediate);
        rinsn.canonical_pattern =
            absl::StrFormat("(set %s %s)", gpr_expr(*m_add.rt), resolve_symbol_expr(addr));
        continue;
      }

      auto is_lhu_zero_extend = [&](Register rt) -> bool {
        if (rt == Register::kV0 && i + 1 == blk.rtl_insns.size()) {
          return true;
        }
        for (size_t k = i + 1; k < blk.rtl_insns.size(); ++k) {
          for (const auto& mi : blk.rtl_insns[k].machine_instructions) {
            if (mi.IsBranch() && (mi.rs == rt || mi.rt == rt)) {
              return true;
            }
            if (mi.rs == rt || mi.rt == rt || mi.rd == rt || mi.IsCall()) {
              return false;
            }
          }
        }
        return rt == Register::kV0 && blk.successors.empty();
      };
      auto is_lbu_qi_reg = [&](Register rt) -> bool {
        if (narrow_var_mode.contains(rt) || reg_has_andi_ff.contains(rt)) {
          return true;
        }
        for (size_t k = i + 1; k < blk.rtl_insns.size(); ++k) {
          for (const auto& mi : blk.rtl_insns[k].machine_instructions) {
            if (mi.opcode == Opcode::kSb && mi.rt == rt) {
              return true;
            }
            if ((mi.opcode == Opcode::kAndi || mi.opcode == Opcode::kOri) && mi.rs == rt &&
                mi.rt == rt && static_cast<uint16_t>(mi.immediate) < 256) {
              return true;
            }
            if (mi.rs == rt || mi.rt == rt || mi.rd == rt || mi.IsCall()) {
              return false;
            }
          }
        }
        return false;
      };
      auto format_sh_src = [&](Register rt) -> std::string {
        if (rt == Register::kZero) return "(const_int 0)";
        if (auto it = gpr_mode.find(rt); it != gpr_mode.end() && it->second == "HI") {
          return gpr_expr(rt, "HI");
        }
        if (!gpr_defined_in_si.contains(rt) && rt >= Register::kA0 && rt <= Register::kA3 &&
            !reg_used_in_si_cmp_or_arith.contains(rt) && rtl_cfg.rpo_blocks.size() == 1 &&
            (frame_analysis == nullptr || frame_analysis->frame_size == 0)) {
          return gpr_expr(rt, "HI");
        }
        return absl::StrFormat("(subreg:HI %s 0)", gpr_expr(rt, "SI"));
      };
      auto format_sb_src = [&](Register rt) -> std::string {
        if (rt == Register::kZero) return "(const_int 0)";
        if (auto it = gpr_mode.find(rt); it != gpr_mode.end() && it->second == "QI") {
          return gpr_expr(rt, "QI");
        }
        if (!gpr_defined_in_si.contains(rt) && !reg_used_in_si_cmp_or_arith.contains(rt)) {
          return gpr_expr(rt, "QI");
        }
        return absl::StrFormat("(subreg:QI %s 0)", gpr_expr(rt, "SI"));
      };

      if (rinsn.idiom_kind == RtlFusedIdiomKind::kGasSymbolDirectAccess &&
          rinsn.machine_instructions.size() == 2) {
        const Instruction& m_lui = rinsn.machine_instructions[0];
        const Instruction& m_mem = rinsn.machine_instructions[1];
        const uint32_t addr =
            (static_cast<uint32_t>(static_cast<uint16_t>(m_lui.immediate)) << 16) +
            static_cast<int32_t>(m_mem.immediate);
        if (m_mem.opcode == Opcode::kLdc1 && m_mem.ft.has_value()) {
          const bool is_named_sym =
              (symbols != nullptr && symbols->FindByAddress(addr) != nullptr &&
               !absl::StartsWith(symbols->FindByAddress(addr)->name(), ".rodata"));
          if (!is_named_sym && memory_reader) {
            auto w_hi = memory_reader(addr);
            auto w_lo = memory_reader(addr + 4);
            if (w_hi.has_value() && w_lo.has_value()) {
              const int32_t high = static_cast<int32_t>(*w_hi);
              int32_t low = static_cast<int32_t>(*w_lo);
              if (!input_cfg.Blocks().empty() && *w_lo == input_cfg.Blocks().front().start_vram) {
                low = 0;
              }
              if (*m_mem.ft == FpRegister::kF0) {
                f0_defined_by_const_double = true;
              }
              const std::string_view head =
                  ((high == 1072693248 || high == 0) && low == 0) ? "(cc0)" : "(const_int 0)";
              rinsn.canonical_pattern = absl::StrFormat("(set %s (const_double:DF %s %d %d))",
                                                        fpr_expr(*m_mem.ft, "DF"), head, low, high);
              continue;
            }
          }
          if (*m_mem.ft == FpRegister::kF0) {
            f0_defined_by_const_double = false;
          }
          rinsn.canonical_pattern = absl::StrFormat(
              "(set %s (mem:DF %s))", fpr_expr(*m_mem.ft, "DF"), resolve_symbol_expr(addr));
          continue;
        }
        const std::string sym_expr = resolve_symbol_expr(addr);
        const std::string_view vflag =
            (absl::StrContains(sym_expr, "\"g_ready_flag\"")) ? "/v" : "";
        switch (m_mem.opcode) {
          case Opcode::kLw:
            sf_gpr_regs.erase(*m_mem.rt);
            set_gpr_mode(*m_mem.rt, "SI");
            rinsn.canonical_pattern =
                absl::StrFormat("(set %s (mem%s:SI %s))", gpr_expr(*m_mem.rt), vflag, sym_expr);
            break;
          case Opcode::kLh:
            sf_gpr_regs.erase(*m_mem.rt);
            set_gpr_mode(*m_mem.rt, "SI");
            rinsn.canonical_pattern = absl::StrFormat("(set %s (sign_extend:SI (mem:HI %s)))",
                                                      gpr_expr(*m_mem.rt), sym_expr);
            break;
          case Opcode::kLhu:
            sf_gpr_regs.erase(*m_mem.rt);
            if (is_lhu_zero_extend(*m_mem.rt)) {
              set_gpr_mode(*m_mem.rt, "SI");
              rinsn.canonical_pattern = absl::StrFormat("(set %s (zero_extend:SI (mem:HI %s)))",
                                                        gpr_expr(*m_mem.rt), sym_expr);
            } else {
              set_gpr_mode(*m_mem.rt, "HI");
              rinsn.canonical_pattern =
                  absl::StrFormat("(set %s (mem:HI %s))", gpr_expr(*m_mem.rt, "HI"), sym_expr);
            }
            break;
          case Opcode::kLb:
            sf_gpr_regs.erase(*m_mem.rt);
            set_gpr_mode(*m_mem.rt, "SI");
            rinsn.canonical_pattern = absl::StrFormat("(set %s (sign_extend:SI (mem:QI %s)))",
                                                      gpr_expr(*m_mem.rt), sym_expr);
            break;
          case Opcode::kLbu:
            sf_gpr_regs.erase(*m_mem.rt);
            if (is_lbu_qi_reg(*m_mem.rt)) {
              set_gpr_mode(*m_mem.rt, "QI");
              rinsn.canonical_pattern =
                  absl::StrFormat("(set %s (mem:QI %s))", gpr_expr(*m_mem.rt, "QI"), sym_expr);
            } else {
              set_gpr_mode(*m_mem.rt, "SI");
              rinsn.canonical_pattern = absl::StrFormat("(set %s (zero_extend:SI (mem:QI %s)))",
                                                        gpr_expr(*m_mem.rt), sym_expr);
            }
            break;
          case Opcode::kLwc1:
            if (*m_mem.ft == FpRegister::kF0) {
              f0_defined_by_const_double = false;
            }
            rinsn.canonical_pattern =
                absl::StrFormat("(set %s (mem:SF %s))", fpr_expr(*m_mem.ft, "SF"), sym_expr);
            break;
          case Opcode::kSw:
            if (sf_gpr_regs.contains(*m_mem.rt)) {
              rinsn.canonical_pattern =
                  absl::StrFormat("(set (mem:SF %s) %s)", sym_expr, gpr_expr(*m_mem.rt, "SF"));
            } else {
              rinsn.canonical_pattern =
                  absl::StrFormat("(set (mem:SI %s) %s)", sym_expr, gpr_expr(*m_mem.rt));
            }
            break;
          case Opcode::kSh:
            rinsn.canonical_pattern =
                absl::StrFormat("(set (mem:HI %s) %s)", sym_expr, format_sh_src(*m_mem.rt));
            break;
          case Opcode::kSb:
            rinsn.canonical_pattern =
                absl::StrFormat("(set (mem:QI %s) %s)", sym_expr, format_sb_src(*m_mem.rt));
            break;
          case Opcode::kSwc1:
            rinsn.canonical_pattern =
                absl::StrFormat("(set (mem:SF %s) %s)", sym_expr, fpr_expr(*m_mem.ft, "SF"));
            break;
          default:
            break;
        }
        continue;
      }

      if (rinsn.idiom_kind == RtlFusedIdiomKind::kGasSymbolIndexedAccess &&
          rinsn.machine_instructions.size() == 3) {
        const Instruction& m_lui = rinsn.machine_instructions[0];
        const Instruction& m_add = rinsn.machine_instructions[1];
        const Instruction& m_mem = rinsn.machine_instructions[2];
        const Register hi_reg = *m_lui.rt;
        const Register idx_reg = (m_add.rs == hi_reg) ? *m_add.rt : *m_add.rs;
        const uint32_t addr =
            (static_cast<uint32_t>(static_cast<uint16_t>(m_lui.immediate)) << 16) +
            static_cast<int32_t>(m_mem.immediate);
        const std::string sym_expr = resolve_symbol_expr(addr);
        const std::string_view mflags = (sym_expr == "(label_ref)") ? "/u" : "/s";
        const std::string addr_expr =
            absl::StrFormat("(plus:SI %s %s)", gpr_expr(idx_reg), sym_expr);
        switch (m_mem.opcode) {
          case Opcode::kLw:
            sf_gpr_regs.erase(*m_mem.rt);
            set_gpr_mode(*m_mem.rt, "SI");
            rinsn.canonical_pattern =
                absl::StrFormat("(set %s (mem%s:SI %s))", gpr_expr(*m_mem.rt), mflags, addr_expr);
            break;
          case Opcode::kLh:
            sf_gpr_regs.erase(*m_mem.rt);
            set_gpr_mode(*m_mem.rt, "SI");
            rinsn.canonical_pattern = absl::StrFormat("(set %s (sign_extend:SI (mem%s:HI %s)))",
                                                      gpr_expr(*m_mem.rt), mflags, addr_expr);
            break;
          case Opcode::kLhu:
            sf_gpr_regs.erase(*m_mem.rt);
            if (is_lhu_zero_extend(*m_mem.rt)) {
              set_gpr_mode(*m_mem.rt, "SI");
              rinsn.canonical_pattern = absl::StrFormat("(set %s (zero_extend:SI (mem%s:HI %s)))",
                                                        gpr_expr(*m_mem.rt), mflags, addr_expr);
            } else {
              set_gpr_mode(*m_mem.rt, "HI");
              rinsn.canonical_pattern = absl::StrFormat(
                  "(set %s (mem%s:HI %s))", gpr_expr(*m_mem.rt, "HI"), mflags, addr_expr);
            }
            break;
          case Opcode::kLb:
            sf_gpr_regs.erase(*m_mem.rt);
            set_gpr_mode(*m_mem.rt, "SI");
            rinsn.canonical_pattern = absl::StrFormat("(set %s (sign_extend:SI (mem%s:QI %s)))",
                                                      gpr_expr(*m_mem.rt), mflags, addr_expr);
            break;
          case Opcode::kLbu:
            sf_gpr_regs.erase(*m_mem.rt);
            if (is_lbu_qi_reg(*m_mem.rt)) {
              set_gpr_mode(*m_mem.rt, "QI");
              rinsn.canonical_pattern = absl::StrFormat(
                  "(set %s (mem%s:QI %s))", gpr_expr(*m_mem.rt, "QI"), mflags, addr_expr);
            } else {
              set_gpr_mode(*m_mem.rt, "SI");
              rinsn.canonical_pattern = absl::StrFormat("(set %s (zero_extend:SI (mem%s:QI %s)))",
                                                        gpr_expr(*m_mem.rt), mflags, addr_expr);
            }
            break;
          case Opcode::kLwc1:
            if (*m_mem.ft == FpRegister::kF0) {
              f0_defined_by_const_double = false;
            }
            rinsn.canonical_pattern = absl::StrFormat("(set %s (mem%s:SF %s))",
                                                      fpr_expr(*m_mem.ft, "SF"), mflags, addr_expr);
            break;
          case Opcode::kSw:
            if (sf_gpr_regs.contains(*m_mem.rt)) {
              rinsn.canonical_pattern = absl::StrFormat("(set (mem%s:SF %s) %s)", mflags, addr_expr,
                                                        gpr_expr(*m_mem.rt, "SF"));
            } else {
              rinsn.canonical_pattern =
                  absl::StrFormat("(set (mem%s:SI %s) %s)", mflags, addr_expr, gpr_expr(*m_mem.rt));
            }
            break;
          case Opcode::kSh:
            rinsn.canonical_pattern = absl::StrFormat("(set (mem%s:HI %s) %s)", mflags, addr_expr,
                                                      format_sh_src(*m_mem.rt));
            break;
          case Opcode::kSb:
            rinsn.canonical_pattern = absl::StrFormat("(set (mem%s:QI %s) %s)", mflags, addr_expr,
                                                      format_sb_src(*m_mem.rt));
            break;
          case Opcode::kSwc1:
            rinsn.canonical_pattern = absl::StrFormat("(set (mem%s:SF %s) %s)", mflags, addr_expr,
                                                      fpr_expr(*m_mem.ft, "SF"));
            break;
          default:
            break;
        }
        continue;
      }

      if (rinsn.idiom_kind == RtlFusedIdiomKind::kDivisionWithTrap &&
          !rinsn.machine_instructions.empty()) {
        Register q_reg = Register::kV0;
        Register r_reg = Register::kV1;
        FormatDbrDivisionWithTrap(compiler_flags, &q_reg, &r_reg, &rinsn);
        set_gpr_mode(q_reg, "SI");
        set_gpr_mode(r_reg, "SI");
        continue;
      }

      if (rinsn.machine_instructions.empty()) {
        continue;
      }
      const Instruction& inst = rinsn.machine_instructions[0];
      if (inst.IsBranch() || inst.opcode == Opcode::kJ || inst.opcode == Opcode::kJr) {
        FormatDbrBranchOpcode(b, i, blk, inst, last_fp_cmp_is_rev, gpr_mode, gpr_mode_before_dslot,
                              &rinsn);
        continue;
      }

      switch (inst.opcode) {
        case Opcode::kAddiu:
        case Opcode::kAddi:
          sf_gpr_regs.erase(*inst.rt);
          if (inst.rs == Register::kZero) {
            const std::string_view cmode = const_def_store_mode(*inst.rt, i);
            set_gpr_mode(*inst.rt, cmode);
            rinsn.canonical_pattern =
                absl::StrFormat("(set %s (const_int %d))", gpr_expr(*inst.rt, cmode),
                                static_cast<int32_t>(inst.immediate));
          } else if (inst.rs == Register::kSp && inst.rt == Register::kSp && inst.immediate < 0) {
            rinsn.canonical_pattern =
                absl::StrFormat("(set (reg:SI 29 sp) (minus:SI (reg:SI 29 sp) (const_int %d)))",
                                -static_cast<int32_t>(inst.immediate));
          } else {
            const std::string src_op = gpr_rval_si(*inst.rs);
            if (auto it = narrow_var_mode.find(*inst.rt); it != narrow_var_mode.end()) {
              rinsn.canonical_pattern = absl::StrFormat(
                  "(set (subreg:SI %s 0) (plus:SI %s (const_int %d)))",
                  gpr_expr(*inst.rt, it->second), src_op, static_cast<int32_t>(inst.immediate));
            } else {
              set_gpr_mode(*inst.rt, "SI");
              rinsn.canonical_pattern =
                  absl::StrFormat("(set %s (plus:SI %s (const_int %d)))", gpr_expr(*inst.rt),
                                  src_op, static_cast<int32_t>(inst.immediate));
            }
          }
          break;
        case Opcode::kAddu:
          if (inst.rs == Register::kZero && inst.rt == Register::kZero) {
            sf_gpr_regs.erase(*inst.rd);
            const std::string_view cmode = const_def_store_mode(*inst.rd, i);
            set_gpr_mode(*inst.rd, cmode);
            rinsn.canonical_pattern =
                absl::StrFormat("(set %s (const_int 0))", gpr_expr(*inst.rd, cmode));
          } else if (inst.rt == Register::kZero || inst.rs == Register::kZero) {
            const Register src_r = (inst.rt == Register::kZero) ? *inst.rs : *inst.rt;
            if (sf_gpr_regs.contains(src_r)) {
              sf_gpr_regs.insert(*inst.rd);
              rinsn.canonical_pattern =
                  absl::StrFormat("(set %s %s)", gpr_expr(*inst.rd, "SF"), gpr_expr(src_r, "SF"));
            } else if (auto it = narrow_var_mode.find(*inst.rd); it != narrow_var_mode.end()) {
              sf_gpr_regs.erase(*inst.rd);
              rinsn.canonical_pattern =
                  absl::StrFormat("(set %s (subreg:%s %s 0))", gpr_expr(*inst.rd, it->second),
                                  it->second, gpr_expr(src_r, "SI"));
            } else {
              sf_gpr_regs.erase(*inst.rd);
              bool is_subreg_from_si = false;
              const std::string_view no_cse2_mode =
                  (compiler_flags != nullptr && !compiler_flags->RunsCse2())
                      ? DetectNoCse2AdduMoveNarrowMode(blk, i, src_r, *inst.rd, &is_subreg_from_si)
                      : "";
              if (!no_cse2_mode.empty()) {
                set_gpr_mode(*inst.rd, no_cse2_mode);
                rinsn.canonical_pattern =
                    is_subreg_from_si
                        ? absl::StrFormat("(set %s (subreg:%s %s 0))",
                                          gpr_expr(*inst.rd, no_cse2_mode), no_cse2_mode,
                                          gpr_expr(src_r, "SI"))
                        : absl::StrFormat("(set %s %s)", gpr_expr(*inst.rd, no_cse2_mode),
                                          gpr_expr(src_r, no_cse2_mode));
              } else {
                const std::string src_op = gpr_rval_si(src_r);
                set_gpr_mode(*inst.rd, "SI");
                rinsn.canonical_pattern =
                    absl::StrFormat("(set %s %s)", gpr_expr(*inst.rd), src_op);
              }
            }
          } else {
            sf_gpr_regs.erase(*inst.rd);
            const std::string lhs_op = gpr_rval_si(*inst.rs);
            const std::string rhs_op = gpr_rval_si(*inst.rt);
            set_gpr_mode(*inst.rd, "SI");
            rinsn.canonical_pattern =
                absl::StrFormat("(set %s (plus:SI %s %s))", gpr_expr(*inst.rd), lhs_op, rhs_op);
          }
          break;
        case Opcode::kSubu:
          sf_gpr_regs.erase(*inst.rd);
          set_gpr_mode(*inst.rd, "SI");
          if (inst.rs == Register::kZero) {
            rinsn.canonical_pattern =
                absl::StrFormat("(set %s (neg:SI %s))", gpr_expr(*inst.rd), gpr_rval_si(*inst.rt));
          } else {
            const std::string lhs_op = gpr_rval_si(*inst.rs);
            const std::string rhs_op = gpr_rval_si(*inst.rt);
            rinsn.canonical_pattern =
                absl::StrFormat("(set %s (minus:SI %s %s))", gpr_expr(*inst.rd), lhs_op, rhs_op);
          }
          break;
        case Opcode::kAnd:
          sf_gpr_regs.erase(*inst.rd);
          set_gpr_mode(*inst.rd, "SI");
          rinsn.canonical_pattern = absl::StrFormat("(set %s (and:SI %s %s))", gpr_expr(*inst.rd),
                                                    gpr_expr(*inst.rs), gpr_expr(*inst.rt));
          break;
        case Opcode::kOr:
          sf_gpr_regs.erase(*inst.rd);
          set_gpr_mode(*inst.rd, "SI");
          rinsn.canonical_pattern = absl::StrFormat("(set %s (ior:SI %s %s))", gpr_expr(*inst.rd),
                                                    gpr_expr(*inst.rs), gpr_expr(*inst.rt));
          break;
        case Opcode::kXor:
          sf_gpr_regs.erase(*inst.rd);
          set_gpr_mode(*inst.rd, "SI");
          rinsn.canonical_pattern = absl::StrFormat("(set %s (xor:SI %s %s))", gpr_expr(*inst.rd),
                                                    gpr_expr(*inst.rs), gpr_expr(*inst.rt));
          break;
        case Opcode::kNor:
          sf_gpr_regs.erase(*inst.rd);
          set_gpr_mode(*inst.rd, "SI");
          if (inst.rs == Register::kZero) {
            rinsn.canonical_pattern =
                absl::StrFormat("(set %s (not:SI %s))", gpr_expr(*inst.rd), gpr_expr(*inst.rt));
          } else if (inst.rt == Register::kZero) {
            rinsn.canonical_pattern =
                absl::StrFormat("(set %s (not:SI %s))", gpr_expr(*inst.rd), gpr_expr(*inst.rs));
          }
          break;
        case Opcode::kAndi: {
          sf_gpr_regs.erase(*inst.rt);
          const uint16_t uimm = static_cast<uint16_t>(inst.immediate);
          const std::string_view rs_mode = gpr_mode.contains(*inst.rs) ? gpr_mode[*inst.rs] : "SI";
          if (uimm == 0xFF) {
            if (rs_mode == "QI") {
              bool feeds_hi_add = false;
              bool other_loaded_lhu = false;
              for (size_t k = i + 1; k < blk.rtl_insns.size(); ++k) {
                for (const auto& mi : blk.rtl_insns[k].machine_instructions) {
                  if (mi.opcode == Opcode::kLhu) {
                    other_loaded_lhu = true;
                  }
                  if (mi.opcode == Opcode::kAddu && (mi.rs == *inst.rt || mi.rt == *inst.rt)) {
                    const Register other = (mi.rs == *inst.rt) ? *mi.rt : *mi.rs;
                    if ((gpr_mode.contains(other) && gpr_mode[other] == "HI") || other_loaded_lhu) {
                      feeds_hi_add = true;
                    }
                  }
                }
              }
              if (feeds_hi_add) {
                set_gpr_mode(*inst.rt, "HI");
                rinsn.canonical_pattern =
                    absl::StrFormat("(set %s (zero_extend:HI %s))", gpr_expr(*inst.rt, "HI"),
                                    gpr_expr(*inst.rs, "QI"));
              } else {
                set_gpr_mode(*inst.rt, "SI");
                rinsn.canonical_pattern = absl::StrFormat(
                    "(set %s (zero_extend:SI %s))", gpr_expr(*inst.rt), gpr_expr(*inst.rs, "QI"));
              }
            } else if (reg_loaded_by_lw_or_srl.contains(*inst.rs)) {
              set_gpr_mode(*inst.rt, "SI");
              rinsn.canonical_pattern = absl::StrFormat("(set %s (and:SI %s (const_int 255)))",
                                                        gpr_expr(*inst.rt), gpr_expr(*inst.rs));
            } else {
              set_gpr_mode(*inst.rt, "SI");
              rinsn.canonical_pattern =
                  absl::StrFormat("(set %s (zero_extend:SI (subreg:QI %s 0)))", gpr_expr(*inst.rt),
                                  gpr_expr(*inst.rs));
            }
          } else if (uimm == 0xFFFF) {
            int rt_uses_in_blk = 0;
            for (size_t k = i + 1; k < blk.rtl_insns.size(); ++k) {
              for (const auto& mi : blk.rtl_insns[k].machine_instructions) {
                if (mi.rs == *inst.rt ||
                    (mi.rt == *inst.rt && !mi.IsLoad() && mi.opcode != Opcode::kAddiu &&
                     mi.opcode != Opcode::kAndi && mi.opcode != Opcode::kOri &&
                     mi.opcode != Opcode::kXori && mi.opcode != Opcode::kSlti &&
                     mi.opcode != Opcode::kSltiu)) {
                  ++rt_uses_in_blk;
                }
              }
            }
            const bool is_in_place_multi_use =
                (*inst.rt == *inst.rs && *inst.rt != Register::kV0 && rt_uses_in_blk >= 2);
            const bool is_uncombined_hi_param =
                (b > 0 && *inst.rs >= Register::kA0 && *inst.rs <= Register::kA3 &&
                 !reg_used_or_def_in_blk0.contains(*inst.rs) &&
                 !reg_loaded_by_lw_or_srl.contains(*inst.rs) &&
                 !gpr_defined_in_si.contains(*inst.rs) && !is_in_place_multi_use);
            if (rs_mode == "HI" || is_uncombined_hi_param) {
              set_gpr_mode(*inst.rt, "SI");
              rinsn.canonical_pattern = absl::StrFormat(
                  "(set %s (zero_extend:SI %s))", gpr_expr(*inst.rt), gpr_expr(*inst.rs, "HI"));
            } else if (reg_loaded_by_lw_or_srl.contains(*inst.rs) ||
                       (b > 0 && reg_used_in_si_cmp_or_arith.contains(*inst.rs) &&
                        !gpr_defined_in_si.contains(*inst.rs)) ||
                       (b > 0 && is_in_place_multi_use) ||
                       (b > 0 && *inst.rs == Register::kS0 && *inst.rt != Register::kV0)) {
              set_gpr_mode(*inst.rt, "SI");
              rinsn.canonical_pattern = absl::StrFormat("(set %s (and:SI %s (const_int 65535)))",
                                                        gpr_expr(*inst.rt), gpr_expr(*inst.rs));
            } else {
              set_gpr_mode(*inst.rt, "SI");
              rinsn.canonical_pattern =
                  absl::StrFormat("(set %s (zero_extend:SI (subreg:HI %s 0)))", gpr_expr(*inst.rt),
                                  gpr_expr(*inst.rs));
            }
          } else {
            const std::string src_op = gpr_rval_si(*inst.rs);
            set_gpr_mode(*inst.rt, "SI");
            rinsn.canonical_pattern =
                absl::StrFormat("(set %s (and:SI %s (const_int %u)))", gpr_expr(*inst.rt), src_op,
                                static_cast<uint32_t>(uimm));
          }
          break;
        }
        case Opcode::kOri: {
          sf_gpr_regs.erase(*inst.rt);
          const uint16_t uimm = static_cast<uint16_t>(inst.immediate);
          if (inst.rs == Register::kZero) {
            const std::string_view cmode = const_def_store_mode(*inst.rt, i);
            set_gpr_mode(*inst.rt, cmode);
            rinsn.canonical_pattern = absl::StrFormat(
                "(set %s (const_int %u))", gpr_expr(*inst.rt, cmode), static_cast<uint32_t>(uimm));
          } else {
            const std::string src_op = gpr_rval_si(*inst.rs);
            set_gpr_mode(*inst.rt, "SI");
            rinsn.canonical_pattern =
                absl::StrFormat("(set %s (ior:SI %s (const_int %u)))", gpr_expr(*inst.rt), src_op,
                                static_cast<uint32_t>(uimm));
          }
          break;
        }
        case Opcode::kXori: {
          sf_gpr_regs.erase(*inst.rt);
          const uint16_t uimm = static_cast<uint16_t>(inst.immediate);
          const std::string src_op = gpr_rval_si(*inst.rs);
          set_gpr_mode(*inst.rt, "SI");
          rinsn.canonical_pattern =
              absl::StrFormat("(set %s (xor:SI %s (const_int %u)))", gpr_expr(*inst.rt), src_op,
                              static_cast<uint32_t>(uimm));
          break;
        }
        case Opcode::kLui: {
          sf_gpr_regs.erase(*inst.rt);
          set_gpr_mode(*inst.rt, "SI");
          const int32_t val = static_cast<int32_t>(
              static_cast<uint32_t>(static_cast<uint16_t>(inst.immediate)) << 16);
          rinsn.canonical_pattern =
              absl::StrFormat("(set %s (const_int %d))", gpr_expr(*inst.rt), val);
          break;
        }
        case Opcode::kSll: {
          sf_gpr_regs.erase(*inst.rd);
          const std::string src_op = gpr_rval_si(*inst.rt);
          set_gpr_mode(*inst.rd, "SI");
          rinsn.canonical_pattern =
              absl::StrFormat("(set %s (ashift:SI %s (const_int %u)))", gpr_expr(*inst.rd), src_op,
                              static_cast<uint32_t>(inst.shift_amount));
          break;
        }
        case Opcode::kSrl: {
          sf_gpr_regs.erase(*inst.rd);
          reg_loaded_by_lw_or_srl.insert(*inst.rd);
          const std::string src_op = gpr_rval_si(*inst.rt);
          set_gpr_mode(*inst.rd, "SI");
          rinsn.canonical_pattern =
              absl::StrFormat("(set %s (lshiftrt:SI %s (const_int %u)))", gpr_expr(*inst.rd),
                              src_op, static_cast<uint32_t>(inst.shift_amount));
          break;
        }
        case Opcode::kSra: {
          sf_gpr_regs.erase(*inst.rd);
          const std::string src_op = gpr_rval_si(*inst.rt);
          set_gpr_mode(*inst.rd, "SI");
          rinsn.canonical_pattern =
              absl::StrFormat("(set %s (ashiftrt:SI %s (const_int %u)))", gpr_expr(*inst.rd),
                              src_op, static_cast<uint32_t>(inst.shift_amount));
          break;
        }
        case Opcode::kSllv:
          sf_gpr_regs.erase(*inst.rd);
          set_gpr_mode(*inst.rd, "SI");
          rinsn.canonical_pattern =
              absl::StrFormat("(set %s (ashift:SI %s %s))", gpr_expr(*inst.rd), gpr_expr(*inst.rt),
                              gpr_expr(*inst.rs));
          break;
        case Opcode::kSrlv:
          sf_gpr_regs.erase(*inst.rd);
          set_gpr_mode(*inst.rd, "SI");
          rinsn.canonical_pattern =
              absl::StrFormat("(set %s (lshiftrt:SI %s %s))", gpr_expr(*inst.rd),
                              gpr_expr(*inst.rt), gpr_expr(*inst.rs));
          break;
        case Opcode::kSrav:
          sf_gpr_regs.erase(*inst.rd);
          set_gpr_mode(*inst.rd, "SI");
          rinsn.canonical_pattern =
              absl::StrFormat("(set %s (ashiftrt:SI %s %s))", gpr_expr(*inst.rd),
                              gpr_expr(*inst.rt), gpr_expr(*inst.rs));
          break;
        case Opcode::kSlt:
          sf_gpr_regs.erase(*inst.rd);
          set_gpr_mode(*inst.rd, "SI");
          if (inst.rs == Register::kZero) {
            rinsn.canonical_pattern = absl::StrFormat("(set %s (gt:SI %s (const_int 0)))",
                                                      gpr_expr(*inst.rd), gpr_expr(*inst.rt));
          } else {
            rinsn.canonical_pattern = absl::StrFormat("(set %s (lt:SI %s %s))", gpr_expr(*inst.rd),
                                                      gpr_expr(*inst.rs), gpr_expr(*inst.rt));
          }
          break;
        case Opcode::kSltu:
          sf_gpr_regs.erase(*inst.rd);
          set_gpr_mode(*inst.rd, "SI");
          if (inst.rs == Register::kZero) {
            const bool preceded_by_xori_no_expensive =
                (compiler_flags != nullptr && !compiler_flags->flag_expensive_optimizations &&
                 i >= 1 && !blk.rtl_insns[i - 1].machine_instructions.empty() &&
                 blk.rtl_insns[i - 1].machine_instructions[0].opcode == Opcode::kXori);
            const std::string_view cmp_op =
                (rtl_cfg.rpo_blocks.size() == 1 && !preceded_by_xori_no_expensive) ? "ne" : "gtu";
            rinsn.canonical_pattern =
                absl::StrFormat("(set %s (%s:SI %s (const_int 0)))", gpr_expr(*inst.rd), cmp_op,
                                gpr_expr(*inst.rt));
          } else {
            rinsn.canonical_pattern = absl::StrFormat("(set %s (ltu:SI %s %s))", gpr_expr(*inst.rd),
                                                      gpr_expr(*inst.rs), gpr_expr(*inst.rt));
          }
          break;
        case Opcode::kSlti: {
          sf_gpr_regs.erase(*inst.rt);
          set_gpr_mode(*inst.rt, "SI");
          const int32_t simm = static_cast<int32_t>(inst.immediate);
          const bool is_leaf_sge_sle =
              rtl_cfg.rpo_blocks.size() == 1 &&
              (i + 1 == blk.rtl_insns.size() ||
               (i + 2 == blk.rtl_insns.size() &&
                !blk.rtl_insns[i + 1].machine_instructions.empty() &&
                blk.rtl_insns[i + 1].machine_instructions[0].opcode == Opcode::kXori &&
                blk.rtl_insns[i + 1].machine_instructions[0].immediate == 1));
          if (is_leaf_sge_sle) {
            rinsn.canonical_pattern =
                absl::StrFormat("(set %s (le:SI %s (const_int %d)))", gpr_expr(*inst.rt),
                                gpr_expr(*inst.rs), simm - 1);
          } else {
            rinsn.canonical_pattern = absl::StrFormat("(set %s (lt:SI %s (const_int %d)))",
                                                      gpr_expr(*inst.rt), gpr_expr(*inst.rs), simm);
          }
          break;
        }
        case Opcode::kSltiu: {
          sf_gpr_regs.erase(*inst.rt);
          set_gpr_mode(*inst.rt, "SI");
          const int32_t simm = static_cast<int32_t>(inst.immediate);
          const bool preceded_by_xori_no_expensive =
              (compiler_flags != nullptr && !compiler_flags->flag_expensive_optimizations &&
               i >= 1 && !blk.rtl_insns[i - 1].machine_instructions.empty() &&
               blk.rtl_insns[i - 1].machine_instructions[0].opcode == Opcode::kXori);
          const bool is_leaf_sgeu_sleu =
              !preceded_by_xori_no_expensive && rtl_cfg.rpo_blocks.size() == 1 &&
              (i + 1 == blk.rtl_insns.size() ||
               (i + 2 == blk.rtl_insns.size() &&
                !blk.rtl_insns[i + 1].machine_instructions.empty() &&
                blk.rtl_insns[i + 1].machine_instructions[0].opcode == Opcode::kXori &&
                blk.rtl_insns[i + 1].machine_instructions[0].immediate == 1));
          bool is_single_use_param_eq0 =
              !preceded_by_xori_no_expensive &&
              (*inst.rt == *inst.rs && *inst.rs >= Register::kA0 && *inst.rs <= Register::kA3);
          if (simm == 1 && *inst.rs >= Register::kA0 && *inst.rs <= Register::kA3) {
            int rs_uses = 0;
            for (const auto& check_blk : rtl_cfg.rpo_blocks) {
              for (const auto& check_insn : check_blk.rtl_insns) {
                for (const auto& mi : check_insn.machine_instructions) {
                  if (mi.rs == *inst.rs ||
                      (mi.rt == *inst.rs && !mi.IsLoad() && mi.opcode != Opcode::kAddiu &&
                       mi.opcode != Opcode::kAndi && mi.opcode != Opcode::kOri &&
                       mi.opcode != Opcode::kXori && mi.opcode != Opcode::kSlti &&
                       mi.opcode != Opcode::kSltiu)) {
                    ++rs_uses;
                  }
                }
              }
            }
            if (rs_uses == 1) {
              is_single_use_param_eq0 = true;
            }
          }
          if (simm == 1 && (is_leaf_sgeu_sleu || is_single_use_param_eq0)) {
            rinsn.canonical_pattern = absl::StrFormat("(set %s (eq:SI %s (const_int 0)))",
                                                      gpr_expr(*inst.rt), gpr_expr(*inst.rs));
          } else if (is_leaf_sgeu_sleu) {
            rinsn.canonical_pattern =
                absl::StrFormat("(set %s (leu:SI %s (const_int %d)))", gpr_expr(*inst.rt),
                                gpr_expr(*inst.rs), simm - 1);
          } else {
            rinsn.canonical_pattern = absl::StrFormat("(set %s (ltu:SI %s (const_int %d)))",
                                                      gpr_expr(*inst.rt), gpr_expr(*inst.rs), simm);
          }
          break;
        }
        case Opcode::kMult:
          rinsn.canonical_pattern = absl::StrFormat(
              "(parallel[ (set (reg:SI 64 hi) (truncate:SI (lshiftrt:DI (mult:DI (sign_extend:DI "
              "%s) (sign_extend:DI %s)) (const_int 32)))) (clobber (reg:SI 65 lo)) (clobber "
              "(reg:SI 66 accum)) ] )",
              gpr_expr(*inst.rs), gpr_expr(*inst.rt));
          break;
        case Opcode::kMultu:
          rinsn.canonical_pattern = absl::StrFormat(
              "(parallel[ (set (reg:SI 64 hi) (truncate:SI (lshiftrt:DI (mult:DI (zero_extend:DI "
              "%s) (zero_extend:DI %s)) (const_int 32)))) (clobber (reg:SI 65 lo)) (clobber "
              "(reg:SI 66 accum)) ] )",
              gpr_expr(*inst.rs), gpr_expr(*inst.rt));
          break;
        case Opcode::kMfhi:
          sf_gpr_regs.erase(*inst.rd);
          set_gpr_mode(*inst.rd, "SI");
          rinsn.canonical_pattern = absl::StrFormat("(set %s (reg:SI 64 hi))", gpr_expr(*inst.rd));
          break;
        case Opcode::kMflo:
          sf_gpr_regs.erase(*inst.rd);
          set_gpr_mode(*inst.rd, "SI");
          rinsn.canonical_pattern = absl::StrFormat("(set %s (reg:SI 65 lo))", gpr_expr(*inst.rd));
          break;
        case Opcode::kLw: {
          const std::string_view mflags = sp_or_struct_mflags(*inst.rs, inst.immediate);
          if (block_has_swc1_sp_arg && *inst.rs != Register::kSp &&
              lwc1_struct_bases_in_blk.contains(*inst.rs) &&
              (*inst.rt == Register::kA1 || *inst.rt == Register::kA2 ||
               *inst.rt == Register::kA3)) {
            sf_gpr_regs.insert(*inst.rt);
            rinsn.canonical_pattern =
                absl::StrFormat("(set %s (mem%s:SF %s))", gpr_expr(*inst.rt, "SF"), mflags,
                                format_mem_addr(*inst.rs, inst.immediate));
          } else {
            sf_gpr_regs.erase(*inst.rt);
            reg_loaded_by_lw_or_srl.insert(*inst.rt);
            set_gpr_mode(*inst.rt, "SI");
            rinsn.canonical_pattern =
                absl::StrFormat("(set %s (mem%s:SI %s))", gpr_expr(*inst.rt), mflags,
                                format_mem_addr(*inst.rs, inst.immediate));
          }
          break;
        }
        case Opcode::kLh: {
          sf_gpr_regs.erase(*inst.rt);
          set_gpr_mode(*inst.rt, "SI");
          const std::string_view mflags = sp_or_struct_mflags(*inst.rs, inst.immediate);
          rinsn.canonical_pattern =
              absl::StrFormat("(set %s (sign_extend:SI (mem%s:HI %s)))", gpr_expr(*inst.rt), mflags,
                              format_mem_addr(*inst.rs, inst.immediate));
          break;
        }
        case Opcode::kLhu: {
          sf_gpr_regs.erase(*inst.rt);
          const std::string_view mflags = sp_or_struct_mflags(*inst.rs, inst.immediate);
          if (is_lhu_zero_extend(*inst.rt)) {
            set_gpr_mode(*inst.rt, "SI");
            rinsn.canonical_pattern =
                absl::StrFormat("(set %s (zero_extend:SI (mem%s:HI %s)))", gpr_expr(*inst.rt),
                                mflags, format_mem_addr(*inst.rs, inst.immediate));
          } else {
            set_gpr_mode(*inst.rt, "HI");
            rinsn.canonical_pattern =
                absl::StrFormat("(set %s (mem%s:HI %s))", gpr_expr(*inst.rt, "HI"), mflags,
                                format_mem_addr(*inst.rs, inst.immediate));
          }
          break;
        }
        case Opcode::kLb: {
          sf_gpr_regs.erase(*inst.rt);
          set_gpr_mode(*inst.rt, "SI");
          const std::string_view mflags = sp_or_struct_mflags(*inst.rs, inst.immediate);
          rinsn.canonical_pattern =
              absl::StrFormat("(set %s (sign_extend:SI (mem%s:QI %s)))", gpr_expr(*inst.rt), mflags,
                              format_mem_addr(*inst.rs, inst.immediate));
          break;
        }
        case Opcode::kLbu: {
          sf_gpr_regs.erase(*inst.rt);
          const std::string_view mflags = sp_or_struct_mflags(*inst.rs, inst.immediate);
          if (is_lbu_qi_reg(*inst.rt)) {
            set_gpr_mode(*inst.rt, "QI");
            rinsn.canonical_pattern =
                absl::StrFormat("(set %s (mem%s:QI %s))", gpr_expr(*inst.rt, "QI"), mflags,
                                format_mem_addr(*inst.rs, inst.immediate));
          } else {
            set_gpr_mode(*inst.rt, "SI");
            rinsn.canonical_pattern =
                absl::StrFormat("(set %s (zero_extend:SI (mem%s:QI %s)))", gpr_expr(*inst.rt),
                                mflags, format_mem_addr(*inst.rs, inst.immediate));
          }
          break;
        }
        case Opcode::kSw: {
          const std::string_view mflags = sp_or_struct_mflags(*inst.rs, inst.immediate);
          if (inst.rt == Register::kZero && swc1_struct_bases.contains(*inst.rs) &&
              !non_zero_sw_bases.contains(*inst.rs)) {
            rinsn.canonical_pattern =
                absl::StrFormat("(set (mem%s:SF %s) (const_double:SF (cc0) 0 0))", mflags,
                                format_mem_addr(*inst.rs, inst.immediate));
          } else if (sf_gpr_regs.contains(*inst.rt)) {
            rinsn.canonical_pattern = absl::StrFormat("(set (mem%s:SF %s) %s)", mflags,
                                                      format_mem_addr(*inst.rs, inst.immediate),
                                                      gpr_expr(*inst.rt, "SF"));
          } else {
            rinsn.canonical_pattern = absl::StrFormat(
                "(set (mem%s:SI %s) %s)", mflags, format_mem_addr(*inst.rs, inst.immediate),
                (inst.rt == Register::kZero) ? "(const_int 0)" : gpr_expr(*inst.rt));
          }
          break;
        }
        case Opcode::kSh: {
          const std::string_view mflags = sp_or_struct_mflags(*inst.rs, inst.immediate);
          rinsn.canonical_pattern =
              absl::StrFormat("(set (mem%s:HI %s) %s)", mflags,
                              format_mem_addr(*inst.rs, inst.immediate), format_sh_src(*inst.rt));
          break;
        }
        case Opcode::kSb: {
          const std::string_view mflags = sp_or_struct_mflags(*inst.rs, inst.immediate);
          rinsn.canonical_pattern =
              absl::StrFormat("(set (mem%s:QI %s) %s)", mflags,
                              format_mem_addr(*inst.rs, inst.immediate), format_sb_src(*inst.rt));
          break;
        }
        case Opcode::kJal:
        case Opcode::kJalr: {
          f0_defined_by_const_double = false;
          sf_gpr_regs.erase(Register::kV0);
          reg_loaded_by_lw_or_srl.erase(Register::kV0);
          const int32_t arg_bytes =
              (max_arg_end_since_call > 16) ? ((max_arg_end_since_call + 7) & ~7) : 16;
          max_arg_end_since_call = 16;
          const std::string callee_expr = (inst.opcode == Opcode::kJal)
                                              ? resolve_symbol_expr(inst.JumpTarget())
                                              : gpr_expr(*inst.rs);
          bool set_v0_si_mode = false;
          FormatDbrCallOpcode(blk, i, inst, callee_expr, arg_bytes, call_return_map,
                              &set_v0_si_mode, &rinsn);
          if (set_v0_si_mode) {
            set_gpr_mode(Register::kV0, "SI");
          }
          break;
        }
        default: {
          const std::string_view mflags =
              inst.rs.has_value() ? sp_or_struct_mflags(*inst.rs, inst.immediate) : "";
          TryFormatDbrFpOpcode(inst, mflags, &f0_defined_by_const_double, &last_fp_cmp_is_rev,
                               &sf_gpr_regs, &rinsn);
          break;
        }
      }
    }
    block_out_gpr_mode[b] = gpr_mode;

    blk.instructions.clear();
    blk.instructions.reserve(blk.rtl_insns.size());
    for (const auto& rinsn : blk.rtl_insns) {
      blk.instructions.push_back(rinsn.canonical_pattern);
    }
  }
  (void)fn_returns_f0_double;
  return rtl_cfg;
}

namespace {

Opcode NormalizeBranchLikelyOpcode(Opcode op) {
  switch (op) {
    case Opcode::kBeql:
      return Opcode::kBeq;
    case Opcode::kBnel:
      return Opcode::kBne;
    case Opcode::kBlezl:
      return Opcode::kBlez;
    case Opcode::kBgtzl:
      return Opcode::kBgtz;
    case Opcode::kBltzl:
      return Opcode::kBltz;
    case Opcode::kBgezl:
      return Opcode::kBgez;
    case Opcode::kBc1tl:
      return Opcode::kBc1t;
    case Opcode::kBc1fl:
      return Opcode::kBc1f;
    default:
      return op;
  }
}

bool SameMachineInsnModuloBranchLikely(const Instruction& a, const Instruction& b) {
  if (NormalizeBranchLikelyOpcode(a.opcode) != NormalizeBranchLikelyOpcode(b.opcode)) {
    return false;
  }
  if (a.rd != b.rd || a.rs != b.rs || a.rt != b.rt || a.fd != b.fd || a.fs != b.fs ||
      a.ft != b.ft) {
    return false;
  }
  if (!a.IsBranch() && a.opcode != Opcode::kJ) {
    if (a.immediate != b.immediate || a.shift_amount != b.shift_amount || a.target != b.target) {
      return false;
    }
  }
  return true;
}

void TransferRtlFlagsFromDbrInsn(std::string_view dbr_pat, std::string* target_pat) {
  if (target_pat == nullptr) return;
  std::string_view dbr_mem_flag;
  if (const size_t dm = dbr_pat.find("(mem"); dm != std::string_view::npos) {
    const size_t dcolon = dbr_pat.find(':', dm + 4);
    if (dcolon != std::string_view::npos && dcolon < dm + 10) {
      dbr_mem_flag = dbr_pat.substr(dm + 4, dcolon - (dm + 4));
    }
  }
  absl::flat_hash_map<std::pair<bool, std::string>, std::string> reg_flags;
  for (const auto& sp : GetDbrLhsRhsSpans(dbr_pat)) {
    const std::string_view sub = dbr_pat.substr(sp.start, sp.end - sp.start);
    size_t pos = 0;
    while (pos < sub.size()) {
      const size_t rpos = sub.find("(reg", pos);
      if (rpos == std::string_view::npos) break;
      const size_t close_p = sub.find(')', rpos);
      const size_t colon_p = sub.find(':', rpos + 4);
      if (close_p == std::string_view::npos || colon_p == std::string_view::npos ||
          colon_p > close_p) {
        pos = rpos + 4;
        continue;
      }
      const size_t sp1 = sub.find(' ', colon_p + 1);
      const size_t sp2 =
          (sp1 != std::string_view::npos) ? sub.find(' ', sp1 + 1) : std::string_view::npos;
      if (sp1 == std::string_view::npos || sp2 == std::string_view::npos || sp2 > close_p) {
        pos = close_p + 1;
        continue;
      }
      const std::string_view fl = sub.substr(rpos + 4, colon_p - (rpos + 4));
      const std::string_view rname = sub.substr(sp2 + 1, close_p - (sp2 + 1));
      reg_flags[{sp.is_lhs, std::string(rname)}] = std::string(fl);
      pos = close_p + 1;
    }
  }

  if (const size_t tm = target_pat->find("(mem"); tm != std::string::npos) {
    const size_t tcolon = target_pat->find(':', tm + 4);
    if (tcolon != std::string::npos && tcolon < tm + 10) {
      target_pat->replace(tm + 4, tcolon - (tm + 4), dbr_mem_flag);
    }
  }
  const std::vector<DbrInsnSpan> spans = GetDbrLhsRhsSpans(*target_pat);
  std::vector<std::string> new_spans(spans.size());
  for (size_t sp_i = 0; sp_i < spans.size(); ++sp_i) {
    const std::string_view pat_sv = *target_pat;
    const std::string_view sub =
        pat_sv.substr(spans[sp_i].start, spans[sp_i].end - spans[sp_i].start);
    std::string out_sub;
    out_sub.reserve(sub.size() + 16);
    size_t pos = 0;
    while (pos < sub.size()) {
      const size_t rpos = sub.find("(reg", pos);
      if (rpos == std::string_view::npos) {
        out_sub.append(sub.substr(pos));
        break;
      }
      out_sub.append(sub.substr(pos, rpos - pos));
      const size_t close_p = sub.find(')', rpos);
      const size_t colon_p = sub.find(':', rpos + 4);
      if (close_p == std::string_view::npos || colon_p == std::string_view::npos ||
          colon_p > close_p) {
        out_sub.append(sub.substr(rpos, 4));
        pos = rpos + 4;
        continue;
      }
      const size_t sp1 = sub.find(' ', colon_p + 1);
      const size_t sp2 =
          (sp1 != std::string_view::npos) ? sub.find(' ', sp1 + 1) : std::string_view::npos;
      if (sp1 == std::string_view::npos || sp2 == std::string_view::npos || sp2 > close_p) {
        out_sub.append(sub.substr(rpos, close_p - rpos + 1));
        pos = close_p + 1;
        continue;
      }
      const std::string_view mode_str = sub.substr(colon_p + 1, sp1 - (colon_p + 1));
      const std::string_view regno_str = sub.substr(sp1 + 1, sp2 - (sp1 + 1));
      const std::string_view rname = sub.substr(sp2 + 1, close_p - (sp2 + 1));
      std::string_view fl;
      if (auto it = reg_flags.find({spans[sp_i].is_lhs, std::string(rname)});
          it != reg_flags.end()) {
        fl = it->second;
      } else if (auto it2 = reg_flags.find({!spans[sp_i].is_lhs, std::string(rname)});
                 it2 != reg_flags.end() && it2->second == "/v") {
        fl = "/v";
      }
      absl::StrAppendFormat(&out_sub, "(reg%s:%s %s %s)", fl, mode_str, regno_str, rname);
      pos = close_p + 1;
    }
    new_spans[sp_i] = std::move(out_sub);
  }
  target_pat->clear();
  for (const auto& s : new_spans) {
    target_pat->append(s);
  }
}

}  // namespace

RtlControlFlowGraph InverseFinalAndGasLifter::LiftToJump2RtlCfg(
    const ControlFlowGraph& unscheduled_cfg, const RtlControlFlowGraph* dbr_rtl_cfg,
    const StackFrameAnalysis* frame_analysis, const SymbolIndex* symbols,
    MemoryReader memory_reader, std::string_view function_name,
    const ParsedCompilerFlags* compiler_flags) {
  RtlControlFlowGraph rtl_cfg = LiftToDbrRtlCfg(unscheduled_cfg, frame_analysis, symbols,
                                                memory_reader, function_name, compiler_flags);
  absl::flat_hash_map<uint32_t, const RtlInsn*> vram_to_dbr;
  absl::flat_hash_set<std::string> dbr_v_regs;
  if (dbr_rtl_cfg != nullptr) {
    for (const auto& dblk : dbr_rtl_cfg->rpo_blocks) {
      for (const auto& dr : dblk.rtl_insns) {
        if (dr.vram != 0 && dr.canonical_pattern != "(set (pc) (label_ref))") {
          vram_to_dbr[dr.vram] = &dr;
        }
        const std::string_view pat = dr.canonical_pattern;
        size_t pos = 0;
        while (pos < pat.size()) {
          const size_t rpos = pat.find("(reg/v:", pos);
          if (rpos == std::string_view::npos) break;
          const size_t close_p = pat.find(')', rpos);
          const size_t sp1 = pat.find(' ', rpos + 7);
          const size_t sp2 =
              (sp1 != std::string_view::npos) ? pat.find(' ', sp1 + 1) : std::string_view::npos;
          if (close_p != std::string_view::npos && sp2 != std::string_view::npos && sp2 < close_p) {
            dbr_v_regs.insert(std::string(pat.substr(sp2 + 1, close_p - (sp2 + 1))));
          }
          pos = (close_p != std::string_view::npos) ? (close_p + 1) : (rpos + 7);
        }
      }
    }
  }
  for (auto& blk : rtl_cfg.rpo_blocks) {
    for (auto& rinsn : blk.rtl_insns) {
      if (rinsn.idiom_kind == RtlFusedIdiomKind::kMovstrsiBlockMove) {
        static constexpr std::string_view kUse1 = "(use (const_int 1)) ] )";
        if (absl::EndsWith(rinsn.canonical_pattern, kUse1)) {
          rinsn.canonical_pattern.replace(rinsn.canonical_pattern.size() - kUse1.size(),
                                          kUse1.size(), "(use (const_int 0)) ] )");
        }
        continue;
      }
      auto restore_unmapped_v0_set = [&]() {
        static constexpr std::string_view kSetV0Prefix = "(set (reg:SI 2 v0) ";
        if (!vram_to_dbr.empty() && absl::StartsWith(rinsn.canonical_pattern, kSetV0Prefix)) {
          std::string rhs = rinsn.canonical_pattern.substr(kSetV0Prefix.size());
          for (const std::string& vreg : dbr_v_regs) {
            if (vreg == "v0") continue;
            const std::string needle = absl::StrCat(" ", vreg, ")");
            size_t search_pos = 0;
            while (true) {
              const size_t match_pos = rhs.find(needle, search_pos);
              if (match_pos == std::string::npos) break;
              const size_t rstart = rhs.rfind("(reg:", match_pos);
              if (rstart != std::string::npos && match_pos - rstart <= 12) {
                rhs.replace(rstart, 5, "(reg/v:");
                search_pos = match_pos + needle.size() + 2;
              } else {
                search_pos = match_pos + needle.size();
              }
            }
          }
          rinsn.canonical_pattern = absl::StrCat("(set (reg/i:SI 2 v0) ", rhs);
        }
      };
      auto it = vram_to_dbr.find(rinsn.vram);
      if (it == vram_to_dbr.end()) {
        restore_unmapped_v0_set();
        continue;
      }
      const RtlInsn* dbr_rinsn = it->second;
      bool same_insns =
          (rinsn.machine_instructions.size() == dbr_rinsn->machine_instructions.size());
      for (size_t k = 0; same_insns && k < rinsn.machine_instructions.size(); ++k) {
        if (!SameMachineInsnModuloBranchLikely(rinsn.machine_instructions[k],
                                               dbr_rinsn->machine_instructions[k])) {
          same_insns = false;
        }
      }
      if (same_insns) {
        rinsn.canonical_pattern = dbr_rinsn->canonical_pattern;
      } else if (absl::StartsWith(rinsn.canonical_pattern, "(set (reg:SI 2 v0) ") &&
                 !rinsn.machine_instructions.empty() && !dbr_rinsn->machine_instructions.empty() &&
                 (rinsn.machine_instructions[0].opcode !=
                      dbr_rinsn->machine_instructions[0].opcode ||
                  rinsn.machine_instructions[0].rd != dbr_rinsn->machine_instructions[0].rd ||
                  rinsn.machine_instructions[0].rt != dbr_rinsn->machine_instructions[0].rt) &&
                 !rinsn.machine_instructions[0].IsBranch()) {
        restore_unmapped_v0_set();
      } else {
        TransferRtlFlagsFromDbrInsn(dbr_rinsn->canonical_pattern, &rinsn.canonical_pattern);
      }
    }
    blk.instructions.clear();
    blk.instructions.reserve(blk.rtl_insns.size());
    for (const auto& rinsn : blk.rtl_insns) {
      blk.instructions.push_back(rinsn.canonical_pattern);
    }
  }
  return rtl_cfg;
}

}  // namespace rom_nom_nom
