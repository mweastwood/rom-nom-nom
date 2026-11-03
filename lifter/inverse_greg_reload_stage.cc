#include "lifter/inverse_greg_reload_stage.h"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_format.h"
#include "core/mips.h"
#include "lifter/rtl_cfg.h"
#include "lifter/rtl_cfg_builder.h"
#include "lifter/rtl_def_use_analysis.h"
#include "lifter/stack_frame_analyzer.h"

namespace rom_nom_nom {
namespace {

bool IsSingleFpArithOrCmpConsumer(const RtlInsn& insn) {
  if (insn.idiom_kind != RtlFusedIdiomKind::kSingleInstruction ||
      insn.machine_instructions.size() != 1) {
    return false;
  }
  const Opcode op = insn.machine_instructions[0].opcode;
  return op == Opcode::kAddS || op == Opcode::kSubS || op == Opcode::kMulS || op == Opcode::kDivS ||
         op == Opcode::kCEqS || op == Opcode::kCLtS || op == Opcode::kCLeS;
}

bool IsSingleFpOutputReloadProducer(const RtlInsn& insn) {
  if (insn.idiom_kind == RtlFusedIdiomKind::kFloatSiSf2) {
    return true;
  }
  if (insn.idiom_kind != RtlFusedIdiomKind::kSingleInstruction ||
      insn.machine_instructions.size() != 1) {
    return false;
  }
  const Opcode op = insn.machine_instructions[0].opcode;
  return op == Opcode::kAddS || op == Opcode::kSubS || op == Opcode::kMulS || op == Opcode::kDivS ||
         op == Opcode::kNegS || op == Opcode::kCvtSD || op == Opcode::kCvtSW;
}

bool FunctionUsesAllNineCalleeSavedGprs(const RtlControlFlowGraph& cfg) {
  absl::flat_hash_set<Register> used_sregs;
  for (const auto& blk : cfg.rpo_blocks) {
    for (const auto& insn : blk.rtl_insns) {
      for (const auto& m : insn.machine_instructions) {
        for (const auto& r_opt : {m.rd, m.rs, m.rt}) {
          if (!r_opt.has_value()) {
            continue;
          }
          const Register r = *r_opt;
          if ((r >= Register::kS0 && r <= Register::kS7) || r == Register::kFp) {
            used_sregs.insert(r);
          }
        }
      }
    }
  }
  return used_sregs.size() == 9;
}

void ReplaceAllSubstrs(std::string* s, std::string_view from, std::string_view to) {
  if (s == nullptr || from.empty()) {
    return;
  }
  size_t pos = 0;
  while ((pos = s->find(from, pos)) != std::string::npos) {
    s->replace(pos, from.size(), to);
    pos += to.size();
  }
}

void InvertEliminatedFrameAddresses(std::string* pat, int32_t frame_size) {
  if (pat == nullptr || frame_size <= 0) {
    return;
  }
  for (std::string_view base_prefix :
       {"(plus:SI (reg:SI 29 sp) (const_int ", "(plus:SI (reg:SI 30 $fp) (const_int "}) {
    size_t pos = 0;
    while ((pos = pat->find(base_prefix, pos)) != std::string::npos) {
      const size_t num_start = pos + base_prefix.size();
      const size_t close_pos = pat->find("))", num_start);
      if (close_pos == std::string::npos) {
        break;
      }
      int32_t off = 0;
      const std::string_view pat_sv = *pat;
      const std::string_view num_sv = pat_sv.substr(num_start, close_pos - num_start);
      if (absl::SimpleAtoi(num_sv, &off) && off >= frame_size) {
        const std::string rep =
            (off == frame_size)
                ? "(reg:SI 0 $0)"
                : absl::StrFormat("(plus:SI (reg:SI 0 $0) (const_int %d))", off - frame_size);
        pat->replace(pos, (close_pos + 2) - pos, rep);
        pos += rep.size();
      } else {
        pos = close_pos + 2;
      }
    }
  }
}

}  // namespace

void InverseGregReloadLifter::InvertGregToLregRtlCfg(RtlControlFlowGraph* cfg,
                                                     const StackFrameAnalysis* frame_analysis,
                                                     const ParsedCompilerFlags* flags) {
  if (cfg == nullptr || cfg->rpo_blocks.empty()) {
    return;
  }
  cfg->pass = RtlPassKind::kLreg;
  const bool runs_opt = (flags == nullptr || flags->RunsOptimizerPasses());
  const int32_t frame_size = (frame_analysis != nullptr) ? frame_analysis->frame_size : 0;

  auto& blks = cfg->rpo_blocks;
  const size_t num_blks = blks.size();
  std::vector<std::vector<RtlInsnDefUse>> parsed_map(num_blks);
  for (size_t b = 0; b < num_blks; ++b) {
    parsed_map[b].reserve(blks[b].rtl_insns.size());
    for (const auto& insn : blks[b].rtl_insns) {
      parsed_map[b].push_back(AnalyzeRtlInsnDefUse(insn, nullptr));
    }
  }

  const bool has_spilled_sregs = FunctionUsesAllNineCalleeSavedGprs(*cfg);

  for (size_t b = 0; b < num_blks; ++b) {
    const auto& r_insns = blks[b].rtl_insns;
    const auto& parsed_b = parsed_map[b];
    const size_t n = r_insns.size();

    bool blk_has_mult = false;
    bool blk_has_standalone_mflo = false;
    for (const auto& insn : r_insns) {
      if (insn.idiom_kind != RtlFusedIdiomKind::kSingleInstruction ||
          insn.machine_instructions.size() != 1) {
        continue;
      }
      const Opcode op = insn.machine_instructions[0].opcode;
      if (op == Opcode::kMult || op == Opcode::kMultu) {
        blk_has_mult = true;
      } else if (op == Opcode::kMflo) {
        blk_has_standalone_mflo = true;
      }
    }

    std::vector<bool> keep(n, true);
    for (size_t i = 0; i < n; ++i) {
      const RtlInsn& insn = r_insns[i];

      // Category 1A: `DFmode` GPR-pair (`$s0`..`$s7`) to scratch FPR (`$f0`..`$f18`) input reload
      // (`reload1.c:emit_reload_insns` for `divdf3` / `adddf3` / `subdf3` / `muldf3` `'f'`
      // constraint).
      if (insn.idiom_kind == RtlFusedIdiomKind::kMove2Words && !insn.machine_instructions.empty()) {
        const Instruction& m0 = insn.machine_instructions[0];
        if (m0.opcode == Opcode::kMtc1 && m0.rt.has_value() && m0.fs.has_value()) {
          const Register g_even = static_cast<Register>(static_cast<int>(*m0.rt) & ~1);
          const FpRegister f_even = static_cast<FpRegister>(static_cast<int>(*m0.fs) & ~1);
          if (g_even >= Register::kS0 && g_even <= Register::kS7 && static_cast<int>(f_even) < 20 &&
              f_even != FpRegister::kF12 && f_even != FpRegister::kF14) {
            keep[i] = false;
            continue;
          }
        }
      }

      if (insn.idiom_kind != RtlFusedIdiomKind::kSingleInstruction ||
          insn.machine_instructions.size() != 1) {
        continue;
      }
      const Instruction& m = insn.machine_instructions[0];

      // Category 1B: `SFmode` GPR (`$a1`..`$a3`) to scratch FPR (`$f0`..`$f18`) input reload
      // (`RELOAD_FOR_INPUT` `mtc1 $aY, $fX` before an FP `'f'`-constraint instruction).
      if (runs_opt && m.opcode == Opcode::kMtc1 && m.rt.has_value() && m.fs.has_value() &&
          (*m.rt == Register::kA1 || *m.rt == Register::kA2 || *m.rt == Register::kA3) &&
          static_cast<int>(*m.fs) < 20 && *m.fs != FpRegister::kF14) {
        const std::string fpr_name = FprName(*m.fs);
        size_t use_count = 0;
        size_t first_use_idx = n;
        bool killed_in_blk = false;
        for (size_t k = i + 1; k < n; ++k) {
          if (HasRegItem(parsed_b[k].uses, fpr_name)) {
            if (use_count == 0) {
              first_use_idx = k;
            }
            ++use_count;
          }
          if (parsed_b[k].is_call || HasRegItem(parsed_b[k].defs, fpr_name)) {
            killed_in_blk = true;
            break;
          }
        }
        if (use_count == 1 && first_use_idx < n &&
            IsSingleFpArithOrCmpConsumer(r_insns[first_use_idx]) &&
            (killed_in_blk || !IsRegLiveInSuccessors(*cfg, parsed_map, b, fpr_name))) {
          const std::string gpr_sf =
              absl::StrFormat("(reg:SF %d %s)", static_cast<int>(*m.rt), GprName(*m.rt));
          const std::string fpr_sf =
              absl::StrFormat("(reg:SF %d %s)", 32 + static_cast<int>(*m.fs), fpr_name);
          const std::string fpr_v_sf =
              absl::StrFormat("(reg/v:SF %d %s)", 32 + static_cast<int>(*m.fs), fpr_name);
          ReplaceAllSubstrs(&blks[b].rtl_insns[first_use_idx].rtl_sexpr_pattern, fpr_sf, gpr_sf);
          ReplaceAllSubstrs(&blks[b].rtl_insns[first_use_idx].rtl_sexpr_pattern, fpr_v_sf, gpr_sf);
          keep[i] = false;
          continue;
        }
      }

      // Category 1C: `SFmode` scratch FPR (`$f0`..`$f18`) to GPR (`$a1`..`$a3`) output reload
      // (`RELOAD_FOR_OUTPUT` `mfc1 $aY, $fX` after an FP `'=f'`-constraint instruction).
      if (runs_opt && m.opcode == Opcode::kMfc1 && m.rt.has_value() && m.fs.has_value() &&
          (*m.rt == Register::kA1 || *m.rt == Register::kA2 || *m.rt == Register::kA3) &&
          static_cast<int>(*m.fs) < 20) {
        const std::string fpr_name = FprName(*m.fs);
        std::optional<size_t> def_idx;
        bool used_between = false;
        for (size_t k = i; k > 0; --k) {
          const size_t prev = k - 1;
          if (parsed_b[prev].is_call) {
            break;
          }
          if (HasRegItem(parsed_b[prev].defs, fpr_name)) {
            def_idx = prev;
            break;
          }
          if (HasRegItem(parsed_b[prev].uses, fpr_name)) {
            used_between = true;
            break;
          }
        }
        if (def_idx.has_value() && !used_between &&
            IsSingleFpOutputReloadProducer(r_insns[*def_idx])) {
          bool used_after_in_blk = false;
          bool killed_after_in_blk = false;
          for (size_t k = i + 1; k < n; ++k) {
            if (HasRegItem(parsed_b[k].uses, fpr_name)) {
              used_after_in_blk = true;
              break;
            }
            if (parsed_b[k].is_call || HasRegItem(parsed_b[k].defs, fpr_name)) {
              killed_after_in_blk = true;
              break;
            }
          }
          if (!used_after_in_blk &&
              (killed_after_in_blk || !IsRegLiveInSuccessors(*cfg, parsed_map, b, fpr_name))) {
            const std::string set_gpr_sf =
                absl::StrFormat("(set (reg:SF %d %s)", static_cast<int>(*m.rt), GprName(*m.rt));
            const std::string set_fpr_sf =
                absl::StrFormat("(set (reg:SF %d %s)", 32 + static_cast<int>(*m.fs), fpr_name);
            const std::string set_fpr_v_sf =
                absl::StrFormat("(set (reg/v:SF %d %s)", 32 + static_cast<int>(*m.fs), fpr_name);
            ReplaceAllSubstrs(&blks[b].rtl_insns[*def_idx].rtl_sexpr_pattern, set_fpr_sf,
                              set_gpr_sf);
            ReplaceAllSubstrs(&blks[b].rtl_insns[*def_idx].rtl_sexpr_pattern, set_fpr_v_sf,
                              set_gpr_sf);
            keep[i] = false;
            continue;
          }
        }
      }

      // Category 2: `smulsi3_highpart` / `umulsi3_highpart` (`'=h'` constraint) `mfhi` output
      // reload (`(set (reg:SI R) (reg:SI 64 hi))`).
      if (m.opcode == Opcode::kMfhi && blk_has_mult && !blk_has_standalone_mflo) {
        if (m.rd.has_value()) {
          const std::string set_rd = absl::StrFormat("(set (reg:SI %d %s) (truncate:SI",
                                                     static_cast<int>(*m.rd), GprName(*m.rd));
          for (size_t k = i; k > 0; --k) {
            std::string& prev_sexpr = blks[b].rtl_insns[k - 1].rtl_sexpr_pattern;
            if (prev_sexpr.find("(set (reg:SI 64 hi) (truncate:SI") != std::string::npos) {
              ReplaceAllSubstrs(&prev_sexpr, "(set (reg:SI 64 hi) (truncate:SI", set_rd);
              ReplaceAllSubstrs(&prev_sexpr, "(clobber (reg:SI 64 hi))", "(clobber (scratch:SI))");
              break;
            }
          }
        }
        keep[i] = false;
        continue;
      }

      // Category 3: Pseudo-register stack spill stores (`sw $v1, offset($sp)`) and reload loads
      // (`lw $v1, offset($sp)`) emitted by `reload1.c:alter_reg` / `emit_reload_insns` when all 9
      // callee-saved GPRs (`$s0`..`$s7`, `$fp`) are exhausted.
      if (has_spilled_sregs && (m.opcode == Opcode::kSw || m.opcode == Opcode::kLw) &&
          m.rs == Register::kSp && m.rt == Register::kV1) {
        keep[i] = false;
        continue;
      }
    }

    std::vector<RtlInsn> new_insns;
    std::vector<std::string> new_pats;
    new_insns.reserve(n);
    new_pats.reserve(n);
    for (size_t i = 0; i < n; ++i) {
      if (!keep[i]) {
        continue;
      }
      if (flags != nullptr) {
        if (!blks[b].rtl_insns[i].rtl_sexpr_pattern.empty()) {
          std::string& sexpr = blks[b].rtl_insns[i].rtl_sexpr_pattern;
          if (has_spilled_sregs && !blks[b].rtl_insns[i].machine_instructions.empty()) {
            const Instruction& m_kept = blks[b].rtl_insns[i].machine_instructions[0];
            if ((m_kept.opcode == Opcode::kSw || m_kept.opcode == Opcode::kLw) &&
                m_kept.rs == Register::kSp && m_kept.rt.has_value() &&
                *m_kept.rt >= Register::kA0 && *m_kept.rt <= Register::kA3) {
              const std::string mem_sp = absl::StrFormat(
                  "(mem:SI (plus:SI (reg:SI 29 sp) (const_int %d)))", m_kept.immediate);
              ReplaceAllSubstrs(&sexpr, mem_sp, "(reg:SI 3 v1)");
            }
          }
          InvertEliminatedFrameAddresses(&sexpr, frame_size);
          blks[b].rtl_insns[i].canonical_pattern = sexpr;
        } else if (!blks[b].rtl_insns[i].machine_instructions.empty()) {
          const Instruction& m = blks[b].rtl_insns[i].machine_instructions[0];
          if (m.opcode == Opcode::kAddu && m.rd.has_value() && m.rs.has_value() &&
              m.rt == Register::kZero) {
            blks[b].rtl_insns[i].canonical_pattern =
                absl::StrFormat("(set (reg:SI %d %s) (reg:SI %d %s))", static_cast<int>(*m.rd),
                                GprName(*m.rd), static_cast<int>(*m.rs), GprName(*m.rs));
          } else if (m.opcode == Opcode::kMovS && m.fd.has_value() && m.fs.has_value()) {
            blks[b].rtl_insns[i].canonical_pattern =
                absl::StrFormat("(set (reg:SF %d %s) (reg:SF %d %s))", 32 + static_cast<int>(*m.fd),
                                FprName(*m.fd), 32 + static_cast<int>(*m.fs), FprName(*m.fs));
          }
        }
      }
      new_pats.push_back(blks[b].rtl_insns[i].canonical_pattern);
      new_insns.push_back(std::move(blks[b].rtl_insns[i]));
    }
    blks[b].rtl_insns = std::move(new_insns);
    blks[b].instructions = std::move(new_pats);
  }
}

}  // namespace rom_nom_nom
