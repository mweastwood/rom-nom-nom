#include "lifter/rtl_def_use_analysis.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/strings/match.h"
#include "core/mips.h"
#include "core/symbol_registry.h"
#include "lifter/rtl_cfg.h"
#include "lifter/rtl_cfg_builder.h"

namespace rom_nom_nom {

bool HasRegItem(const std::vector<std::string>& vec, std::string_view item) {
  return std::find(vec.begin(), vec.end(), item) != vec.end();
}

RtlInsnDefUse AnalyzeRtlInsnDefUse(const RtlInsn& insn, const SymbolIndex* symbols) {
  RtlInsnDefUse p;
  auto add_def = [&](std::string_view r) {
    if (!r.empty() && r != "$0" && r != "at" && !HasRegItem(p.defs, r)) {
      p.defs.emplace_back(r);
    }
  };
  auto add_use = [&](std::string_view r) {
    if (!r.empty() && r != "$0" && r != "at" && !HasRegItem(p.uses, r)) {
      p.uses.emplace_back(r);
    }
  };

  if (IsCallRtlInsn(insn)) {
    p.is_call = true;
    add_def("v0");
    add_def("$f0");
    for (const auto& m : insn.machine_instructions) {
      if (m.opcode == Opcode::kJal && symbols != nullptr) {
        if (const auto* entry = symbols->FindByAddress(m.JumpTarget()); entry != nullptr) {
          p.callee = entry->name();
        }
      }
      if (m.opcode == Opcode::kJalr && m.rs.has_value()) {
        add_use(GprName(*m.rs));
      }
    }
    return p;
  }

  if (absl::StartsWith(insn.canonical_pattern, "(set (pc)") ||
      absl::StartsWith(insn.canonical_pattern, "(parallel[ (set (pc)")) {
    p.is_jump = true;
    if (!insn.machine_instructions.empty()) {
      const Instruction& m = insn.machine_instructions[0];
      if (m.rs.has_value()) {
        add_use(GprName(*m.rs));
      }
      if (m.rt.has_value()) {
        add_use(GprName(*m.rt));
      }
      if ((m.opcode == Opcode::kBeq || m.opcode == Opcode::kBeql) &&
          ((m.rs == Register::kA1 && m.rt == Register::kZero) ||
           (m.rt == Register::kA1 && m.rs == Register::kZero))) {
        p.is_beq_a1_zero = true;
      }
      if ((m.opcode == Opcode::kBeq || m.opcode == Opcode::kBne || m.opcode == Opcode::kBeql ||
           m.opcode == Opcode::kBnel) &&
          (m.rs == Register::kA1 || m.rt == Register::kA1) && m.rs != Register::kZero &&
          m.rt != Register::kZero) {
        p.compares_a1_to_reg = true;
      }
    }
    return p;
  }

  if (insn.idiom_kind == RtlFusedIdiomKind::kMove2Words && !insn.machine_instructions.empty()) {
    const Instruction& m0 = insn.machine_instructions[0];
    if (m0.opcode == Opcode::kMtc1 && m0.rt.has_value() && m0.fs.has_value()) {
      const Register g_even = static_cast<Register>(static_cast<int>(*m0.rt) & ~1);
      const FpRegister f_even = static_cast<FpRegister>(static_cast<int>(*m0.fs) & ~1);
      add_use(GprName(g_even));
      add_def(FprName(f_even));
      p.is_reg_to_reg_move = true;
      if (g_even == Register::kA2) {
        p.is_df_di_a2 = true;
      }
    } else if (m0.opcode == Opcode::kMfc1 && m0.rt.has_value() && m0.fs.has_value()) {
      const Register g_even = static_cast<Register>(static_cast<int>(*m0.rt) & ~1);
      const FpRegister f_even = static_cast<FpRegister>(static_cast<int>(*m0.fs) & ~1);
      add_def(GprName(g_even));
      add_use(FprName(f_even));
      p.is_reg_to_reg_move = true;
      if (g_even == Register::kA2) {
        p.is_df_di_a2 = true;
      }
    } else if (m0.opcode == Opcode::kLui && m0.rt.has_value()) {
      const Register g_even = static_cast<Register>(static_cast<int>(*m0.rt) & ~1);
      add_def(GprName(g_even));
      p.is_load = true;
      if (g_even == Register::kA2) {
        p.is_df_di_a2 = true;
      }
    }
    return p;
  }

  if (insn.idiom_kind == RtlFusedIdiomKind::kMovstrsiBlockMove) {
    for (const auto& m : insn.machine_instructions) {
      if ((m.IsLoad() || m.IsStore()) && m.rs.has_value()) {
        add_use(GprName(*m.rs));
      }
      if (m.IsLoad() && m.rt.has_value()) {
        add_def(GprName(*m.rt));
      }
    }
    p.is_store = true;
    return p;
  }

  if (insn.idiom_kind == RtlFusedIdiomKind::kFixTruncSfSi2 ||
      insn.idiom_kind == RtlFusedIdiomKind::kFixTruncDfSi2) {
    for (const auto& m : insn.machine_instructions) {
      if ((m.opcode == Opcode::kTruncWS || m.opcode == Opcode::kTruncWD ||
           m.opcode == Opcode::kCvtWS || m.opcode == Opcode::kCvtWD) &&
          m.fs.has_value() && m.fd.has_value()) {
        add_use(FprName(*m.fs));
        add_def(FprName(*m.fd));
      } else if (m.opcode == Opcode::kMfc1 && m.rt.has_value()) {
        add_def(GprName(*m.rt));
      } else if (m.opcode == Opcode::kSwc1 && m.rs.has_value()) {
        add_use(GprName(*m.rs));
        p.is_store = true;
        if (insn.machine_instructions.size() == 3 &&
            insn.machine_instructions[1].opcode == Opcode::kLui) {
          p.is_symbol_store = true;
        }
      } else if ((m.opcode == Opcode::kCfc1 || m.opcode == Opcode::kXori) && m.rt.has_value()) {
        add_def(GprName(*m.rt));
      }
    }
    return p;
  }

  for (const Instruction& m : insn.machine_instructions) {
    std::vector<std::string> m_uses;
    std::vector<std::string> m_defs;
    if (m.IsLoad()) {
      if (m.rs.has_value()) {
        m_uses.push_back(GprName(*m.rs));
      }
      if (m.opcode == Opcode::kLwc1 || m.opcode == Opcode::kLdc1) {
        if (m.ft.has_value()) {
          const FpRegister f = (m.opcode == Opcode::kLdc1)
                                   ? static_cast<FpRegister>(static_cast<int>(*m.ft) & ~1)
                                   : *m.ft;
          m_defs.push_back(FprName(f));
        }
      } else if (m.rt.has_value()) {
        m_defs.push_back(GprName(*m.rt));
      }
    } else if (m.IsStore()) {
      if (m.opcode == Opcode::kSwc1 || m.opcode == Opcode::kSdc1) {
        if (m.ft.has_value()) {
          const FpRegister f = (m.opcode == Opcode::kSdc1)
                                   ? static_cast<FpRegister>(static_cast<int>(*m.ft) & ~1)
                                   : *m.ft;
          m_uses.push_back(FprName(f));
        }
      } else if (m.rt.has_value()) {
        m_uses.push_back(GprName(*m.rt));
      }
      if (m.rs.has_value()) {
        m_uses.push_back(GprName(*m.rs));
      }
    } else if (m.opcode == Opcode::kLui) {
      if (m.rt.has_value()) {
        m_defs.push_back(GprName(*m.rt));
      }
    } else if (m.opcode == Opcode::kAddiu || m.opcode == Opcode::kAddi ||
               m.opcode == Opcode::kOri || m.opcode == Opcode::kAndi || m.opcode == Opcode::kXori ||
               m.opcode == Opcode::kSlti || m.opcode == Opcode::kSltiu) {
      if (m.rs.has_value()) {
        m_uses.push_back(GprName(*m.rs));
      }
      if (m.rt.has_value()) {
        m_defs.push_back(GprName(*m.rt));
      }
    } else if (m.opcode == Opcode::kSll || m.opcode == Opcode::kSrl || m.opcode == Opcode::kSra) {
      if (m.rt.has_value()) {
        m_uses.push_back(GprName(*m.rt));
      }
      if (m.rd.has_value()) {
        m_defs.push_back(GprName(*m.rd));
      }
    } else if (m.opcode == Opcode::kAddu || m.opcode == Opcode::kSubu || m.opcode == Opcode::kAnd ||
               m.opcode == Opcode::kOr || m.opcode == Opcode::kXor || m.opcode == Opcode::kNor ||
               m.opcode == Opcode::kSlt || m.opcode == Opcode::kSltu || m.opcode == Opcode::kSllv ||
               m.opcode == Opcode::kSrlv || m.opcode == Opcode::kSrav) {
      if (m.rs.has_value()) {
        m_uses.push_back(GprName(*m.rs));
      }
      if (m.rt.has_value()) {
        m_uses.push_back(GprName(*m.rt));
      }
      if (m.rd.has_value()) {
        m_defs.push_back(GprName(*m.rd));
      }
    } else if (m.opcode == Opcode::kMult || m.opcode == Opcode::kMultu ||
               m.opcode == Opcode::kDiv || m.opcode == Opcode::kDivu) {
      if (m.rs.has_value()) {
        m_uses.push_back(GprName(*m.rs));
      }
      if (m.rt.has_value()) {
        m_uses.push_back(GprName(*m.rt));
      }
    } else if (m.opcode == Opcode::kMflo || m.opcode == Opcode::kMfhi) {
      if (m.rd.has_value()) {
        m_defs.push_back(GprName(*m.rd));
      }
    } else if (m.opcode == Opcode::kMtc1) {
      if (m.rt.has_value()) {
        m_uses.push_back(GprName(*m.rt));
      }
      if (m.fs.has_value()) {
        m_defs.push_back(FprName(*m.fs));
      }
    } else if (m.opcode == Opcode::kMfc1) {
      if (m.fs.has_value()) {
        m_uses.push_back(FprName(*m.fs));
      }
      if (m.rt.has_value()) {
        m_defs.push_back(GprName(*m.rt));
      }
    } else if (m.opcode == Opcode::kAddS || m.opcode == Opcode::kSubS ||
               m.opcode == Opcode::kMulS || m.opcode == Opcode::kDivS ||
               m.opcode == Opcode::kAddD || m.opcode == Opcode::kSubD ||
               m.opcode == Opcode::kMulD || m.opcode == Opcode::kDivD) {
      if (m.fs.has_value()) {
        m_uses.push_back(FprName(*m.fs));
      }
      if (m.ft.has_value()) {
        m_uses.push_back(FprName(*m.ft));
      }
      if (m.fd.has_value()) {
        m_defs.push_back(FprName(*m.fd));
      }
    } else if (m.opcode == Opcode::kMovS || m.opcode == Opcode::kMovD ||
               m.opcode == Opcode::kNegS || m.opcode == Opcode::kNegD ||
               m.opcode == Opcode::kCvtSD || m.opcode == Opcode::kCvtDS ||
               m.opcode == Opcode::kCvtSW || m.opcode == Opcode::kCvtDW) {
      if (m.fs.has_value()) {
        m_uses.push_back(FprName(*m.fs));
      }
      if (m.fd.has_value()) {
        m_defs.push_back(FprName(*m.fd));
      }
    } else if (m.opcode == Opcode::kCEqS || m.opcode == Opcode::kCLtS ||
               m.opcode == Opcode::kCLeS || m.opcode == Opcode::kCEqD ||
               m.opcode == Opcode::kCLtD || m.opcode == Opcode::kCLeD) {
      if (m.fs.has_value()) {
        m_uses.push_back(FprName(*m.fs));
      }
      if (m.ft.has_value()) {
        m_uses.push_back(FprName(*m.ft));
      }
    }

    for (const auto& u : m_uses) {
      if (!HasRegItem(p.defs, u)) {
        add_use(u);
      }
    }
    for (const auto& d : m_defs) {
      add_def(d);
    }
  }

  if (!insn.machine_instructions.empty()) {
    const Instruction& last_m = insn.machine_instructions.back();
    if (last_m.IsStore()) {
      p.is_store = true;
      if (last_m.opcode == Opcode::kSw && last_m.rt == Register::kV0) {
        p.is_sw_v0 = true;
      }
      if (insn.idiom_kind == RtlFusedIdiomKind::kGasSymbolDirectAccess ||
          insn.idiom_kind == RtlFusedIdiomKind::kGasSymbolIndexedAccess) {
        p.is_symbol_store = true;
        if (insn.idiom_kind == RtlFusedIdiomKind::kGasSymbolDirectAccess &&
            last_m.opcode == Opcode::kSw && last_m.rt == Register::kZero) {
          p.is_global_lock_zero_store = true;
        }
      }
      if (last_m.rs == Register::kSp && last_m.immediate == 16) {
        p.is_sp16_store = true;
        if (last_m.opcode == Opcode::kSwc1 || last_m.opcode == Opcode::kSdc1) {
          p.is_sp16_fpr_store = true;
        }
      }
    }
    if (last_m.IsLoad() && insn.idiom_kind != RtlFusedIdiomKind::kFloatSiSf2 &&
        insn.idiom_kind != RtlFusedIdiomKind::kFloatSiDf2) {
      p.is_load = true;
    }
  }

  if (insn.idiom_kind == RtlFusedIdiomKind::kSingleInstruction &&
      insn.machine_instructions.size() == 1) {
    const Instruction& m = insn.machine_instructions[0];
    if (m.opcode == Opcode::kAddu && m.rt == Register::kZero && m.rd.has_value() &&
        m.rs.has_value() && *m.rd != Register::kZero && *m.rs != Register::kZero &&
        *m.rd != *m.rs) {
      p.is_reg_to_reg_move = true;
      p.is_si_reg_to_reg_move = true;
    }
    if ((m.opcode == Opcode::kMovS || m.opcode == Opcode::kMovD) && m.fd.has_value() &&
        m.fs.has_value() && *m.fd != *m.fs) {
      p.is_reg_to_reg_move = true;
    }
    if (m.opcode == Opcode::kMtc1 && m.rt.has_value() && m.fs.has_value() &&
        *m.rt != Register::kZero) {
      p.is_reg_to_reg_move = true;
      p.is_mtc1_from_gpr = true;
    }
    if (m.opcode == Opcode::kMfc1 && m.rt.has_value() && m.fs.has_value()) {
      p.is_reg_to_reg_move = true;
      p.is_float_in_gpr_def = true;
      if (*m.rt >= Register::kA0 && *m.rt <= Register::kA3) {
        p.is_mfc1_to_arg_gpr = true;
      }
    }
    if (m.opcode == Opcode::kSll) {
      p.is_sll = true;
    }
    if ((m.opcode == Opcode::kAddu || m.opcode == Opcode::kSubu) && m.rs != Register::kZero &&
        m.rt != Register::kZero) {
      p.is_addu_or_subu_reg = true;
      if (m.opcode == Opcode::kAddu && m.rd == Register::kV0) {
        p.is_v0_plus_non_one = true;
      }
    }
    if (m.opcode == Opcode::kAddiu || m.opcode == Opcode::kAddi) {
      if (m.immediate == -1) {
        p.is_addiu_minus_1 = true;
      }
      if (m.rt == Register::kV0 && m.rs != Register::kZero && m.immediate != 0 &&
          m.immediate != 1) {
        p.is_v0_plus_non_one = true;
      }
      if (m.rs == Register::kSp && m.rt.has_value() && *m.rt >= Register::kA0 &&
          *m.rt <= Register::kA3) {
        if (m.immediate == 16) {
          p.is_sp_buf_16_addr = true;
        }
        if (m.immediate == 32) {
          p.is_sp_buf_32_addr = true;
        }
      }
    }
    if (m.opcode == Opcode::kAnd) {
      p.is_and_reg = true;
    }
    if (m.opcode == Opcode::kAndi && m.rs == Register::kA0 && m.immediate == 1) {
      p.is_andi_a0_1 = true;
    }
    if (m.opcode == Opcode::kLw && m.rt == Register::kA0 && m.rs == Register::kS0 &&
        m.immediate == 0) {
      p.is_lw_a0_from_0_s0 = true;
    }
    if ((m.opcode == Opcode::kSlt || m.opcode == Opcode::kSltu) &&
        (m.rs == Register::kA1 || m.rt == Register::kA1) && m.rs != Register::kZero &&
        m.rt != Register::kZero) {
      p.compares_a1_to_reg = true;
    }
  }
  return p;
}

bool IsRegLiveInSuccessors(const RtlControlFlowGraph& cfg,
                           const std::vector<std::vector<RtlInsnDefUse>>& parsed_map,
                           size_t start_blk, std::string_view reg_name) {
  const size_t num_blks = cfg.rpo_blocks.size();
  absl::flat_hash_set<size_t> visited;
  std::vector<size_t> queue = cfg.rpo_blocks[start_blk].successors;
  size_t q_head = 0;
  while (q_head < queue.size()) {
    const size_t curr = queue[q_head++];
    if (curr >= num_blks || visited.contains(curr)) {
      continue;
    }
    visited.insert(curr);
    bool killed = false;
    for (const auto& p : parsed_map[curr]) {
      if (HasRegItem(p.uses, reg_name)) {
        return true;
      }
      if (p.is_call || HasRegItem(p.defs, reg_name)) {
        killed = true;
        break;
      }
    }
    if (!killed) {
      for (size_t nxt : cfg.rpo_blocks[curr].successors) {
        if (!visited.contains(nxt)) {
          queue.push_back(nxt);
        }
      }
    }
  }
  return false;
}

}  // namespace rom_nom_nom
