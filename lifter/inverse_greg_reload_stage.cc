#include "lifter/inverse_greg_reload_stage.h"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
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

}  // namespace

void InverseGregReloadLifter::InvertGregToLregRtlCfg(RtlControlFlowGraph* cfg,
                                                     const StackFrameAnalysis* /*frame_analysis*/) {
  if (cfg == nullptr || cfg->rpo_blocks.empty()) {
    return;
  }

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
      if (m.opcode == Opcode::kMtc1 && m.rt.has_value() && m.fs.has_value() &&
          (*m.rt == Register::kA1 || *m.rt == Register::kA2 || *m.rt == Register::kA3) &&
          static_cast<int>(*m.fs) < 20 && *m.fs != FpRegister::kF12 && *m.fs != FpRegister::kF14) {
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
          keep[i] = false;
          continue;
        }
      }

      // Category 1C: `SFmode` scratch FPR (`$f0`..`$f18`) to GPR (`$a1`..`$a3`) output reload
      // (`RELOAD_FOR_OUTPUT` `mfc1 $aY, $fX` after an FP `'=f'`-constraint instruction).
      if (m.opcode == Opcode::kMfc1 && m.rt.has_value() && m.fs.has_value() &&
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
            keep[i] = false;
            continue;
          }
        }
      }

      // Category 2: `smulsi3_highpart` / `umulsi3_highpart` (`'=h'` constraint) `mfhi` output
      // reload (`(set (reg:SI R) (reg:SI 64 hi))`).
      if (m.opcode == Opcode::kMfhi && blk_has_mult && !blk_has_standalone_mflo) {
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
      new_pats.push_back(blks[b].rtl_insns[i].canonical_pattern);
      new_insns.push_back(std::move(blks[b].rtl_insns[i]));
    }
    blks[b].rtl_insns = std::move(new_insns);
    blks[b].instructions = std::move(new_pats);
  }
}

}  // namespace rom_nom_nom
