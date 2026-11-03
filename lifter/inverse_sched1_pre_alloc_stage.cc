#include "lifter/inverse_sched1_pre_alloc_stage.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/match.h"
#include "absl/strings/str_format.h"
#include "core/mips.h"
#include "lifter/rtl_cfg.h"
#include "lifter/rtl_cfg_builder.h"
#include "lifter/rtl_def_use_analysis.h"

namespace rom_nom_nom {

void InverseSched1PreAllocLifter::InvertSchedToCombineRtlCfg(RtlControlFlowGraph* cfg) {
  if (cfg == nullptr) {
    return;
  }
  cfg->pass = RtlPassKind::kCombine;
  if (cfg->rpo_blocks.empty()) {
    return;
  }

  for (auto& blk : cfg->rpo_blocks) {
    const size_t n = blk.rtl_insns.size();
    if (n < 2) {
      continue;
    }

    std::vector<RtlInsnDefUse> parsed(n);
    for (size_t i = 0; i < n; ++i) {
      parsed[i] = AnalyzeRtlInsnDefUse(blk.rtl_insns[i], nullptr);
    }

    // Invert `sched.c:schedule_insns` (`0x08181892`–`0x0818196d`, when `reload_completed == 0`)
    // calling `try_split(PATTERN(insn), insn, 1)` -> `insn-emit.c:gen_split_156` (`0x081a1d49`):
    //   (define_split
    //     [(set (match_operand:SI 0 "register_operand" "")
    //           (match_operand:SI 1 "large_int" ""))]
    //     "!TARGET_DEBUG_D_MODE"
    //     [(set (match_dup 0) (match_dup 2))
    //      (set (match_dup 0) (ior:SI (match_dup 0) (match_dup 3)))])
    // where `mips.c:large_int` (`0x081c7422`) splits any 32-bit `CONST_INT` with both non-zero
    // upper 16 bits (`lui $r, hi16`) and non-zero lower 16 bits (`ori $r, $r, lo16`).
    std::vector<bool> keep(n, true);
    bool modified = false;
    for (size_t i = 0; i < n; ++i) {
      if (!keep[i]) {
        continue;
      }
      const RtlInsn& hi_rtl = blk.rtl_insns[i];
      if (hi_rtl.idiom_kind != RtlFusedIdiomKind::kSingleInstruction ||
          hi_rtl.machine_instructions.size() != 1) {
        continue;
      }
      const Instruction& hi_inst = hi_rtl.machine_instructions[0];
      if (hi_inst.opcode != Opcode::kLui || !hi_inst.rt.has_value() ||
          *hi_inst.rt == Register::kZero || hi_inst.immediate == 0) {
        continue;
      }

      const Register reg = *hi_inst.rt;
      const std::string reg_name = GprName(reg);
      for (size_t j = i + 1; j < n; ++j) {
        if (!keep[j]) {
          continue;
        }
        const RtlInsn& lo_rtl = blk.rtl_insns[j];
        if (lo_rtl.idiom_kind == RtlFusedIdiomKind::kSingleInstruction &&
            lo_rtl.machine_instructions.size() == 1) {
          const Instruction& lo_inst = lo_rtl.machine_instructions[0];
          if (lo_inst.opcode == Opcode::kOri && lo_inst.rt == reg && lo_inst.rs == reg &&
              lo_inst.immediate != 0) {
            blk.rtl_insns[i].idiom_kind = RtlFusedIdiomKind::kSplitLargeIntHiLo;
            blk.rtl_insns[i].machine_instructions.push_back(lo_inst);
            const uint32_t hi_bits = static_cast<uint32_t>(static_cast<uint16_t>(hi_inst.immediate))
                                     << 16;
            const uint32_t lo_bits =
                static_cast<uint32_t>(static_cast<uint16_t>(lo_inst.immediate));
            const int32_t combined_imm = static_cast<int32_t>(hi_bits | lo_bits);
            const std::string fused_sexpr =
                absl::StrFormat("(set (reg:SI %d %s) (const_int %d))", static_cast<int>(reg),
                                reg_name, combined_imm);
            blk.rtl_insns[i].rtl_sexpr_pattern = fused_sexpr;
            if (absl::StartsWith(blk.rtl_insns[i].canonical_pattern, "(")) {
              blk.rtl_insns[i].canonical_pattern = fused_sexpr;
            }
            keep[j] = false;
            modified = true;
            break;
          }
        }
        if (parsed[j].is_call || HasRegItem(parsed[j].uses, reg_name) ||
            HasRegItem(parsed[j].defs, reg_name)) {
          break;
        }
      }
    }

    if (!modified) {
      continue;
    }

    std::vector<RtlInsn> new_insns;
    std::vector<std::string> new_pats;
    new_insns.reserve(n);
    new_pats.reserve(n);
    for (size_t i = 0; i < n; ++i) {
      if (!keep[i]) {
        continue;
      }
      new_pats.push_back(blk.rtl_insns[i].canonical_pattern);
      new_insns.push_back(std::move(blk.rtl_insns[i]));
    }
    blk.rtl_insns = std::move(new_insns);
    blk.instructions = std::move(new_pats);
  }
}

}  // namespace rom_nom_nom
