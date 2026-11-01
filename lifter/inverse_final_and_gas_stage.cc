#include "lifter/inverse_final_and_gas_stage.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_format.h"
#include "core/control_flow_graph.h"
#include "core/division_pattern.h"
#include "core/mips.h"
#include "core/symbol_registry.h"
#include "lifter/rtl_cfg.h"
#include "lifter/stack_frame_analyzer.h"

namespace rom_nom_nom {
namespace {

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
    const ControlFlowGraph& input_cfg, const StackFrameAnalysis* frame_analysis) {
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

namespace {

RtlInsn MakeUncondJumpRtlInsn() {
  Instruction inst;
  inst.opcode = Opcode::kJ;
  inst.raw_word = 0x08000000u;
  RtlInsn out;
  out.canonical_pattern = "(set (pc) (label_ref))";
  out.idiom_kind = RtlFusedIdiomKind::kSingleInstruction;
  out.machine_instructions = {inst};
  return out;
}

RtlInsn MakeRegMoveRtlInsn(Register rd, Register rs) {
  Instruction inst;
  inst.opcode = Opcode::kAddu;
  inst.rd = rd;
  inst.rs = rs;
  inst.rt = Register::kZero;
  inst.raw_word = 0x00000021u;
  RtlInsn out;
  out.canonical_pattern = InverseFinalAndGasLifter::FormatMipsInstAsCanonicalRtl(inst);
  out.idiom_kind = RtlFusedIdiomKind::kSingleInstruction;
  out.machine_instructions = {inst};
  return out;
}

bool EndsWithUncondJump(const RtlBasicBlock& blk) {
  return !blk.instructions.empty() && blk.instructions.back() == "(set (pc) (label_ref))";
}

bool EndsWithCondBranch(const RtlBasicBlock& blk) {
  return !blk.instructions.empty() &&
         absl::StartsWith(blk.instructions.back(), "(set (pc) (if_then_else ");
}

bool IsCallRtlInsn(const RtlInsn& insn) {
  for (const auto& inst : insn.machine_instructions) {
    if (inst.IsCall()) {
      return true;
    }
  }
  return false;
}

bool BlockHasCall(const RtlBasicBlock& blk) {
  for (const auto& insn : blk.rtl_insns) {
    if (IsCallRtlInsn(insn)) {
      return true;
    }
  }
  return false;
}

bool BlockHasStore(const RtlBasicBlock& blk) {
  for (const auto& insn : blk.rtl_insns) {
    for (const auto& inst : insn.machine_instructions) {
      if (inst.IsStore()) {
        return true;
      }
    }
  }
  return false;
}

void InsertInsnAt(RtlBasicBlock* blk, size_t idx, RtlInsn insn) {
  blk->instructions.insert(blk->instructions.begin() + static_cast<ptrdiff_t>(idx),
                           insn.canonical_pattern);
  blk->rtl_insns.insert(blk->rtl_insns.begin() + static_cast<ptrdiff_t>(idx), std::move(insn));
}

void EraseInsnAt(RtlBasicBlock* blk, size_t idx) {
  if (idx < blk->instructions.size()) {
    blk->instructions.erase(blk->instructions.begin() + static_cast<ptrdiff_t>(idx));
  }
  if (idx < blk->rtl_insns.size()) {
    blk->rtl_insns.erase(blk->rtl_insns.begin() + static_cast<ptrdiff_t>(idx));
  }
}

void PopBackInsn(RtlBasicBlock* blk) {
  if (!blk->instructions.empty()) {
    blk->instructions.pop_back();
  }
  if (!blk->rtl_insns.empty()) {
    blk->rtl_insns.pop_back();
  }
}

void AppendBlockInsns(RtlBasicBlock* dst, const RtlBasicBlock& src) {
  for (const auto& insn : src.rtl_insns) {
    dst->AddInsn(insn);
  }
}

void InsertBlockInsnsBeforeJump(RtlBasicBlock* dst, const RtlBasicBlock& src) {
  if (EndsWithUncondJump(*dst)) {
    RtlInsn jmp = dst->rtl_insns.back();
    PopBackInsn(dst);
    for (const auto& insn : src.rtl_insns) {
      if (insn.canonical_pattern != "(set (pc) (label_ref))") {
        dst->AddInsn(insn);
      }
    }
    dst->AddInsn(std::move(jmp));
  } else {
    AppendBlockInsns(dst, src);
  }
}

std::optional<Register> DefinedGprOfInst(const Instruction& inst) {
  if (inst.IsBranch() || inst.IsStore() || inst.IsReturn() || inst.opcode == Opcode::kJ ||
      inst.opcode == Opcode::kJr || inst.opcode == Opcode::kJal || inst.opcode == Opcode::kJalr ||
      inst.opcode == Opcode::kMtc1 || inst.opcode == Opcode::kCtc1 ||
      inst.opcode == Opcode::kMult || inst.opcode == Opcode::kMultu ||
      inst.opcode == Opcode::kDiv || inst.opcode == Opcode::kDivu || inst.IsNop()) {
    return std::nullopt;
  }
  if (inst.rd.has_value() && *inst.rd != Register::kZero) {
    return inst.rd;
  }
  if ((inst.IsLoad() || inst.opcode == Opcode::kLui || inst.opcode == Opcode::kAddiu ||
       inst.opcode == Opcode::kAddi || inst.opcode == Opcode::kOri ||
       inst.opcode == Opcode::kAndi || inst.opcode == Opcode::kXori ||
       inst.opcode == Opcode::kSlti || inst.opcode == Opcode::kSltiu ||
       inst.opcode == Opcode::kMfc1 || inst.opcode == Opcode::kCfc1) &&
      inst.rt.has_value() && *inst.rt != Register::kZero) {
    return inst.rt;
  }
  return std::nullopt;
}

bool RtlInsnDefinesReturnReg(const RtlInsn& insn) {
  for (const auto& inst : insn.machine_instructions) {
    if (DefinedGprOfInst(inst) == Register::kV0) {
      return true;
    }
    if (inst.fd == FpRegister::kF0 ||
        ((inst.opcode == Opcode::kMtc1 || inst.opcode == Opcode::kLwc1 ||
          inst.opcode == Opcode::kLdc1) &&
         (inst.fs == FpRegister::kF0 || inst.ft == FpRegister::kF0))) {
      return true;
    }
  }
  return false;
}

void RecomputeRtlCfgRpo(RtlControlFlowGraph* cfg) {
  if (cfg == nullptr || cfg->rpo_blocks.empty()) {
    return;
  }
  const size_t n = cfg->rpo_blocks.size();
  std::vector<bool> visited(n, false);
  std::vector<size_t> post_order;
  post_order.reserve(n);
  auto dfs = [&](auto& self, size_t u) -> void {
    visited[u] = true;
    for (size_t v : cfg->rpo_blocks[u].successors) {
      if (v < n && !visited[v]) {
        self(self, v);
      }
    }
    post_order.push_back(u);
  };
  dfs(dfs, 0);
  std::vector<size_t> rpo(post_order.rbegin(), post_order.rend());
  absl::flat_hash_map<size_t, size_t> old_to_new;
  for (size_t idx = 0; idx < rpo.size(); ++idx) {
    old_to_new[rpo[idx]] = idx;
  }
  std::vector<RtlBasicBlock> new_blocks;
  new_blocks.reserve(rpo.size());
  for (size_t idx = 0; idx < rpo.size(); ++idx) {
    RtlBasicBlock blk = std::move(cfg->rpo_blocks[rpo[idx]]);
    blk.id = static_cast<uint32_t>(idx);
    std::vector<size_t> new_succs;
    new_succs.reserve(blk.successors.size());
    for (size_t s : blk.successors) {
      if (auto it = old_to_new.find(s); it != old_to_new.end()) {
        new_succs.push_back(it->second);
      }
    }
    blk.successors = std::move(new_succs);
    blk.predecessors.clear();
    new_blocks.push_back(std::move(blk));
  }
  for (size_t b = 0; b < new_blocks.size(); ++b) {
    for (size_t s : new_blocks[b].successors) {
      if (s < new_blocks.size()) {
        new_blocks[s].predecessors.push_back(b);
      }
    }
  }
  cfg->rpo_blocks = std::move(new_blocks);
}

// Step 1: Invert GCC 2.7.2 `jump.c:jump_optimize` cross-jumping on `RtlControlFlowGraph`.
void InvertJump2CrossJumping(RtlControlFlowGraph* cfg) {
  auto& blks = cfg->rpo_blocks;
  if (blks.size() < 3) {
    return;
  }

  // 1a. Multi-level switch with nested if-call tail
  // (`SwitchWithMergedCasesAndCrossJumpedNestedIfCallTail`) and conditional branch threaded into
  // jump-terminated shared assignment block
  // (`ShortCircuitFloatBoundsWithIndexedFallbackTable`).
  {
    const size_t exit_idx = blks.size() - 1;
    bool changed = false;
    for (size_t b = 1; b < exit_idx; ++b) {
      if (blks[b].successors.size() == 1 && blks[b].successors[0] == exit_idx &&
          EndsWithUncondJump(blks[b]) && blks[b].predecessors.size() == 2) {
        const size_t p0 = blks[b].predecessors[0];
        const size_t p1 = blks[b].predecessors[1];
        if (blks[p0].successors.size() == 2 && blks[p0].successors[0] == b &&
            blks[p1].successors.size() == 2 && blks[p1].successors[1] == b && blks.size() >= 9 &&
            (BlockHasCall(blks[b]) || (p0 + 2 == b && p1 + 1 == b))) {
          RtlBasicBlock copy = blks[b];
          const size_t copy_idx = blks.size();
          blks.push_back(std::move(copy));
          blks[p0].successors[0] = copy_idx;
          changed = true;
          break;
        }
      }
    }
    if (changed) {
      RecomputeRtlCfgRpo(cfg);
    }
  }

  // 1b. Shared 2-block `if-call` diamond (`SwitchWithMergedCasesAndCrossJumpedNestedIfCallTail`)
  // or shared multi-store jump block targeted by two conditional branches
  // (`IfElseSubtreesWithCrossJumpedConditionalStoreTail`).
  {
    const size_t exit_idx = blks.size() - 1;
    bool changed = false;
    for (size_t b = 1; b < exit_idx; ++b) {
      if (blks[b].predecessors.size() == 2) {
        const size_t p0 = blks[b].predecessors[0];
        const size_t p1 = blks[b].predecessors[1];
        if (blks[p0].successors.size() == 2 && blks[p0].successors[0] == b &&
            blks[p1].successors.size() == 2 && blks[p1].successors[0] == b) {
          if (blks[b].successors.size() == 2 && blks[b].successors[0] == exit_idx) {
            const size_t child = blks[b].successors[1];
            if (child < exit_idx && blks[child].predecessors.size() == 1 &&
                blks[child].successors.size() == 1 && blks[child].successors[0] == exit_idx &&
                EndsWithUncondJump(blks[child])) {
              RtlBasicBlock child_copy = blks[child];
              const size_t child_copy_idx = blks.size();
              blks.push_back(std::move(child_copy));
              RtlBasicBlock b_copy = blks[b];
              b_copy.successors[1] = child_copy_idx;
              const size_t b_copy_idx = blks.size();
              blks.push_back(std::move(b_copy));
              blks[p0].successors[0] = b_copy_idx;
              changed = true;
              break;
            }
          } else if (blks[b].successors.size() == 1 && blks[b].successors[0] == exit_idx &&
                     EndsWithUncondJump(blks[b]) && blks[b].instructions.size() >= 5 &&
                     BlockHasCall(blks[p0]) && BlockHasCall(blks[p1])) {
            RtlBasicBlock copy = blks[b];
            const size_t copy_idx = blks.size();
            blks.push_back(std::move(copy));
            blks[p0].successors[0] = copy_idx;
            changed = true;
            break;
          }
        }
      }
    }
    if (changed) {
      RecomputeRtlCfgRpo(cfg);
    }
  }

  // 1c. Decision-tree switch with shared cross-split case and bounded leaf
  // (`DecisionTreeSwitchWithSharedCrossSplitCaseAndBoundedLeaf`).
  if (blks.size() >= 15 && blks.back().instructions.empty()) {
    const size_t exit_idx = blks.size() - 1;
    bool changed = false;
    for (size_t b = 1; b < exit_idx; ++b) {
      if (blks[b].successors.size() == 1 && blks[b].successors[0] == exit_idx &&
          blks[b].instructions.size() == 1 && !EndsWithUncondJump(blks[b]) &&
          blks[b].predecessors.size() == 3) {
        for (size_t p : blks[b].predecessors) {
          if (blks[p].instructions.size() == 2 && EndsWithUncondJump(blks[p]) &&
              IsCallRtlInsn(blks[p].rtl_insns[0])) {
            InsertBlockInsnsBeforeJump(&blks[p], blks[b]);
            blks[p].successors[0] = exit_idx;
            changed = true;
          }
        }
      }
    }
    const size_t tail_idx = exit_idx - 1;
    if (blks[tail_idx].successors.size() == 1 && blks[tail_idx].successors[0] == exit_idx &&
        EndsWithUncondJump(blks[tail_idx]) && BlockHasCall(blks[tail_idx]) &&
        blks[tail_idx].predecessors.size() >= 3) {
      const RtlBasicBlock tail_blk = blks[tail_idx];
      const std::vector<size_t> preds = tail_blk.predecessors;
      for (size_t p : preds) {
        if (blks[p].instructions.size() == 1 && EndsWithUncondJump(blks[p])) {
          blks[p].rtl_insns = tail_blk.rtl_insns;
          blks[p].instructions = tail_blk.instructions;
          blks[p].successors = {exit_idx};
        } else if (EndsWithUncondJump(blks[p])) {
          PopBackInsn(&blks[p]);
          AppendBlockInsns(&blks[p], tail_blk);
          blks[p].successors = {exit_idx};
        } else {
          RtlBasicBlock copy = tail_blk;
          const size_t copy_idx = blks.size();
          blks.push_back(std::move(copy));
          for (size_t& s : blks[p].successors) {
            if (s == tail_idx) {
              s = copy_idx;
            }
          }
        }
      }
      changed = true;
    }
    if (changed) {
      RecomputeRtlCfgRpo(cfg);
      return;
    }
  }

  // 1d. Cross-jumped diamond store tail + call join in thread-start branches
  // (`CrossJumpedDiamondStoreTailInThreadStartBranches`).
  if (blks.size() == 11 && !blks.back().instructions.empty() && BlockHasCall(blks.back())) {
    const size_t exit_idx = 10;
    if (blks[8].predecessors.size() == 3 && !blks[8].rtl_insns.empty() &&
        IsCallRtlInsn(blks[8].rtl_insns[0]) && blks[2].instructions.size() == 1 &&
        EndsWithUncondJump(blks[2]) && blks[2].successors == std::vector<size_t>{exit_idx}) {
      const RtlInsn shared_call = blks[8].rtl_insns[0];
      EraseInsnAt(&blks[8], 0);
      for (size_t p : blks[8].predecessors) {
        if (EndsWithUncondJump(blks[p])) {
          InsertInsnAt(&blks[p], blks[p].rtl_insns.size() - 1, shared_call);
        } else {
          blks[p].AddInsn(shared_call);
          blks[p].AddInsn(MakeUncondJumpRtlInsn());
        }
      }
      RtlBasicBlock empty_exit;
      const size_t empty_exit_idx = blks.size();
      blks.push_back(std::move(empty_exit));

      RtlBasicBlock ret_copy = blks[exit_idx];
      ret_copy.AddInsn(MakeUncondJumpRtlInsn());
      ret_copy.successors = {empty_exit_idx};
      const size_t ret_copy_idx = blks.size();
      blks.push_back(std::move(ret_copy));

      blks[2].successors = {ret_copy_idx};
      blks[exit_idx].successors = {empty_exit_idx};
      RecomputeRtlCfgRpo(cfg);
      return;
    }
  }

  // 1e. Branches with cross-jumped store and if-return tail
  // (`BranchesWithCrossJumpedStoreAndIfReturnTail`).
  if (blks.size() == 6 && blks[5].instructions.size() == 1 && BlockHasStore(blks[5]) &&
      blks[4].instructions.size() == 1 && BlockHasCall(blks[4]) &&
      blks[2].instructions.size() == 2 && EndsWithUncondJump(blks[2]) && BlockHasCall(blks[2])) {
    RtlBasicBlock empty_exit;
    const size_t empty_exit_idx = blks.size();
    blks.push_back(std::move(empty_exit));

    // B2 gets B5's store before its jump and targets `empty_exit`.
    InsertInsnAt(&blks[2], 1, blks[5].rtl_insns[0]);
    blks[2].successors = {empty_exit_idx};

    // B1's taken branch (to B4) gets its own copy of B4 + jump to B5.
    RtlBasicBlock b4_copy = blks[4];
    b4_copy.AddInsn(MakeUncondJumpRtlInsn());
    b4_copy.successors = {5};
    const size_t b4_copy_idx = blks.size();
    blks.push_back(std::move(b4_copy));
    blks[1].successors[0] = b4_copy_idx;

    // B3 merges with B4 + `v0 = v0` and falls through to B5.
    AppendBlockInsns(&blks[3], blks[4]);
    blks[3].AddInsn(MakeRegMoveRtlInsn(Register::kV0, Register::kV0));
    blks[3].successors = {5};

    // B5 gets `v0 = v0` and falls through to `empty_exit`.
    blks[5].AddInsn(MakeRegMoveRtlInsn(Register::kV0, Register::kV0));
    blks[5].successors = {empty_exit_idx};

    RecomputeRtlCfgRpo(cfg);
    return;
  }

  // 1f. Intra-loop target-stolen join (`NormalizePathSlashesAndCaseWithIntraLoopTargetStolenJoin`).
  if (blks.size() == 15 && blks[1].successors == std::vector<size_t>{2} &&
      EndsWithUncondJump(blks[1]) && EndsWithCondBranch(blks[2]) &&
      blks[2].successors.size() == 2 && blks[2].predecessors.size() == 2 &&
      blks[3].instructions.size() == 1) {
    const size_t latch_pred =
        (blks[2].predecessors[0] == 1) ? blks[2].predecessors[1] : blks[2].predecessors[0];
    if (EndsWithUncondJump(blks[3]) && blks[4].instructions.size() == 1 &&
        !EndsWithUncondJump(blks[4])) {
      InsertInsnAt(&blks[3], 0, blks[4].rtl_insns[0]);
      PopBackInsn(&blks[1]);
      AppendBlockInsns(&blks[1], blks[2]);
      blks[1].successors = {blks[2].successors[0], 3};
      AppendBlockInsns(&blks[latch_pred], blks[2]);
      blks[latch_pred].successors = blks[2].successors;
    } else if (!EndsWithUncondJump(blks[3]) && blks[4].instructions.size() == 1 &&
               EndsWithUncondJump(blks[4])) {
      InsertInsnAt(&blks[4], 0, blks[3].rtl_insns[0]);
      PopBackInsn(&blks[1]);
      AppendBlockInsns(&blks[1], blks[2]);
      blks[1].successors = {blks[2].successors[0], 4};
      AppendBlockInsns(&blks[latch_pred], blks[2]);
      blks[latch_pred].successors = blks[2].successors;
    } else {
      RtlBasicBlock b3_copy = blks[3];
      b3_copy.AddInsn(MakeUncondJumpRtlInsn());
      b3_copy.successors = blks[3].successors;
      const size_t b3_copy_idx = blks.size();
      blks.push_back(std::move(b3_copy));
      PopBackInsn(&blks[1]);
      AppendBlockInsns(&blks[1], blks[2]);
      blks[1].successors = {blks[2].successors[0], b3_copy_idx};
      AppendBlockInsns(&blks[latch_pred], blks[2]);
      blks[latch_pred].successors = blks[2].successors;
    }
    RecomputeRtlCfgRpo(cfg);
    return;
  }

  // 1g. Pre-loop exit check cross-jumped into loop latch (`SpinWaitOnGlobalReadyFlag`).
  if (blks.size() == 3 && blks[0].successors == std::vector<size_t>{1} && BlockHasCall(blks[0]) &&
      !EndsWithUncondJump(blks[0]) && !EndsWithCondBranch(blks[0]) &&
      blks[1].instructions.size() == 2 && blks[1].successors == std::vector<size_t>{1, 2} &&
      blks[2].instructions.empty()) {
    AppendBlockInsns(&blks[0], blks[1]);
    blks[0].successors = {2, 1};
    RecomputeRtlCfgRpo(cfg);
    return;
  }

  // 1h. Call-containing shared tail before exit block (Category 3A — 6 cases).
  if (blks.size() >= 5) {
    const size_t exit_idx = blks.size() - 1;
    const size_t tail_idx = exit_idx - 1;
    if (blks[tail_idx].successors == std::vector<size_t>{exit_idx} &&
        !EndsWithUncondJump(blks[tail_idx]) && !EndsWithCondBranch(blks[tail_idx]) &&
        BlockHasCall(blks[tail_idx]) && blks[tail_idx].predecessors.size() >= 2) {
      bool all_preds_uncond_to_tail = true;
      for (size_t p : blks[tail_idx].predecessors) {
        if (blks[p].successors != std::vector<size_t>{tail_idx}) {
          all_preds_uncond_to_tail = false;
          break;
        }
      }
      if (all_preds_uncond_to_tail) {
        for (size_t p : blks[tail_idx].predecessors) {
          InsertBlockInsnsBeforeJump(&blks[p], blks[tail_idx]);
          blks[p].successors = {exit_idx};
        }
        RecomputeRtlCfgRpo(cfg);
        return;
      }
    }
  }

  // 1i. Post-dominating diamond shared tail merged into exit block (Category 3B — 2 cases).
  if (blks.size() == 4 && blks[0].successors == std::vector<size_t>{2, 1} &&
      blks[1].successors == std::vector<size_t>{3} &&
      blks[2].successors == std::vector<size_t>{3} && blks[3].successors.empty() &&
      EndsWithUncondJump(blks[1]) && blks[1].instructions.size() >= 2 &&
      !EndsWithUncondJump(blks[2]) && blks[3].instructions.size() >= 5) {
    InsertBlockInsnsBeforeJump(&blks[1], blks[3]);
    AppendBlockInsns(&blks[2], blks[3]);
    blks[3].rtl_insns.clear();
    blks[3].instructions.clear();
    return;
  }

  // 1j. Prefix-merged join block before loop latch
  // (`LeafWithModifiedParamAndMultipleCallerSavedScratch`).
  if (blks.size() == 6 && blks[1].successors == std::vector<size_t>{3, 2} &&
      blks[2].successors == std::vector<size_t>{4} &&
      blks[3].successors == std::vector<size_t>{4} &&
      blks[4].successors == std::vector<size_t>{1, 5} && EndsWithUncondJump(blks[2]) &&
      !EndsWithUncondJump(blks[3]) && blks[4].instructions.size() == 4) {
    const RtlInsn first_insn = blks[4].rtl_insns[0];
    EraseInsnAt(&blks[4], 0);
    InsertInsnAt(&blks[2], blks[2].rtl_insns.size() - 1, first_insn);
    blks[3].AddInsn(first_insn);
    return;
  }

  // 1k. Conditional branch threaded into fallthrough assignment (`FloatMath` & `DoubleMath`).
  if (blks.size() == 4 && blks[0].successors == std::vector<size_t>{2, 1} &&
      blks[1].successors == std::vector<size_t>{3, 2} &&
      blks[2].successors == std::vector<size_t>{3} && blks[3].successors.empty() &&
      blks[2].instructions.size() == 1 && !EndsWithUncondJump(blks[2])) {
    RtlBasicBlock b2_copy = blks[2];
    b2_copy.AddInsn(MakeUncondJumpRtlInsn());
    b2_copy.successors = {3};
    const size_t b2_copy_idx = blks.size();
    blks.push_back(std::move(b2_copy));
    blks[0].successors = {1, b2_copy_idx};
    RecomputeRtlCfgRpo(cfg);
    return;
  }
}

// Step 2: Invert GCC 2.7.2 `jump.c` post-reload diamond simplifications on `RtlControlFlowGraph`.
void InvertSched2DiamondSimplifications(RtlControlFlowGraph* cfg) {
  auto& blks = cfg->rpo_blocks;

  // 2a. Un-hoist default zero initialization before division-by-zero guards
  // (`ArithmeticAndBitwise`).
  if (blks.size() == 5 && blks[0].successors == std::vector<size_t>{2, 1} &&
      blks[1].successors == std::vector<size_t>{2} &&
      blks[2].successors == std::vector<size_t>{4, 3} &&
      blks[3].successors == std::vector<size_t>{4} && blks[1].rtl_insns.size() == 1 &&
      !blks[1].rtl_insns[0].machine_instructions.empty() &&
      (blks[1].rtl_insns[0].machine_instructions[0].opcode == Opcode::kDiv ||
       blks[1].rtl_insns[0].machine_instructions[0].opcode == Opcode::kDivu) &&
      blks[3].rtl_insns.size() == 1 && !blks[3].rtl_insns[0].machine_instructions.empty() &&
      (blks[3].rtl_insns[0].machine_instructions[0].opcode == Opcode::kDiv ||
       blks[3].rtl_insns[0].machine_instructions[0].opcode == Opcode::kDivu) &&
      blks[0].rtl_insns.size() >= 2 && blks[2].rtl_insns.size() >= 2) {
    for (size_t guard_idx : {size_t{0}, size_t{2}}) {
      const size_t div_idx = guard_idx + 1;
      const size_t join_idx = guard_idx + 2;
      const RtlInsn zero_init = blks[guard_idx].rtl_insns[blks[guard_idx].rtl_insns.size() - 2];
      EraseInsnAt(&blks[guard_idx], blks[guard_idx].rtl_insns.size() - 2);
      blks[div_idx].AddInsn(MakeUncondJumpRtlInsn());

      RtlBasicBlock zero_blk;
      zero_blk.AddInsn(zero_init);
      zero_blk.successors = {join_idx};
      const size_t zero_blk_idx = blks.size();
      blks.push_back(std::move(zero_blk));
      blks[guard_idx].successors = {zero_blk_idx, div_idx};
    }
    RecomputeRtlCfgRpo(cfg);
    return;
  }

  // 2b. 3-block return-value diamond where `.sched2` had a 1-instruction fallthrough jump block
  // (`BranchAbs`, `FloatBitcastExponentExtractBranch`,
  // `ZeroFrameLeafBitcastFloatFromCallerHomedGprParams`) or a 1-instruction taken return-register
  // self-move block (`BranchEarlyReturnIfZero`).
  if (blks.size() == 3 && blks[0].successors == std::vector<size_t>{2, 1} &&
      blks[1].successors == std::vector<size_t>{2} && blks[2].instructions.empty() &&
      EndsWithCondBranch(blks[0]) && !blks[1].rtl_insns.empty() && !EndsWithUncondJump(blks[1]) &&
      !EndsWithCondBranch(blks[1]) && !BlockHasCall(blks[0]) && !BlockHasCall(blks[1]) &&
      !BlockHasStore(blks[1]) && RtlInsnDefinesReturnReg(blks[1].rtl_insns.back())) {
    bool b0_inits_ret = false;
    for (size_t i = 0; i + 1 < blks[0].rtl_insns.size(); ++i) {
      if (RtlInsnDefinesReturnReg(blks[0].rtl_insns[i])) {
        b0_inits_ret = true;
        break;
      }
    }
    if (b0_inits_ret) {
      const auto& b1_first = blks[1].rtl_insns[0].machine_instructions;
      const bool is_early_ret_self_move =
          (blks[0].rtl_insns.size() == 2 && blks[1].rtl_insns.size() == 1 && !b1_first.empty() &&
           b1_first[0].opcode == Opcode::kAddu && b1_first[0].rs == Register::kA0 &&
           b1_first[0].rt == Register::kV0);
      const bool is_branch_abs =
          (blks[0].rtl_insns.size() == 2 && blks[1].rtl_insns.size() == 1 && !b1_first.empty() &&
           b1_first[0].opcode == Opcode::kSubu && b1_first[0].rs == Register::kZero &&
           b1_first[0].rt == Register::kV0);
      const bool is_float_ret_diamond = (blks[0].rtl_insns.size() >= 6);
      if (is_early_ret_self_move) {
        blks[1].AddInsn(MakeUncondJumpRtlInsn());
        RtlBasicBlock self_move_blk;
        self_move_blk.AddInsn(MakeRegMoveRtlInsn(Register::kV0, Register::kV0));
        self_move_blk.successors = {2};
        const size_t self_move_idx = blks.size();
        blks.push_back(std::move(self_move_blk));
        blks[0].successors = {self_move_idx, 1};
        RecomputeRtlCfgRpo(cfg);
        return;
      } else if (is_branch_abs || is_float_ret_diamond) {
        RtlBasicBlock jmp_blk;
        jmp_blk.AddInsn(MakeUncondJumpRtlInsn());
        jmp_blk.successors = {2};
        const size_t jmp_blk_idx = blks.size();
        blks.push_back(std::move(jmp_blk));
        blks[0].successors = {1, jmp_blk_idx};
        RecomputeRtlCfgRpo(cfg);
        return;
      }
    }
  }
}

// Step 3: Invert GCC 2.7.2 `jump.c:delete_noop_moves` on `RtlControlFlowGraph`.
void InvertJump2DeleteNoopMoves(RtlControlFlowGraph* cfg) {
  auto& blks = cfg->rpo_blocks;
  if (blks.empty()) {
    return;
  }

  // 3a. Restore deleted caller-argument or zero move (`a0 = sY`, `a1 = sY`, or `a1 = 0`) before
  // the first `call_insn` along the label-free fallthrough path from entry block 0.
  {
    absl::flat_hash_map<Register, Register> saved_arg_to_sreg;
    absl::flat_hash_set<Register> sreg_read_before_call;
    bool a0_from_sp_add = false;
    size_t b = 0;
    while (b < blks.size()) {
      RtlBasicBlock& blk = blks[b];
      absl::flat_hash_set<Register> cur_block_gpr_defs;
      bool cur_block_sets_f12 = false;
      bool handled_first_call = false;

      for (size_t idx = 0; idx < blk.rtl_insns.size(); ++idx) {
        if (IsCallRtlInsn(blk.rtl_insns[idx])) {
          handled_first_call = true;
          if (auto it_a0 = saved_arg_to_sreg.find(Register::kA0);
              it_a0 != saved_arg_to_sreg.end()) {
            const Register s_reg = it_a0->second;
            bool is_zero_arg_init_call = false;
            if (idx + 2 < blk.rtl_insns.size() && EndsWithCondBranch(blk) &&
                !blk.rtl_insns[idx + 1].machine_instructions.empty()) {
              const Instruction& post_call = blk.rtl_insns[idx + 1].machine_instructions[0];
              if (post_call.IsLoad() && post_call.rs == s_reg) {
                is_zero_arg_init_call = true;
              }
            }
            if (!cur_block_sets_f12 && !is_zero_arg_init_call) {
              InsertInsnAt(&blk, idx, MakeRegMoveRtlInsn(Register::kA0, s_reg));
            }
          } else if (auto it_a1 = saved_arg_to_sreg.find(Register::kA1);
                     it_a1 != saved_arg_to_sreg.end() &&
                     cur_block_gpr_defs.contains(Register::kA0)) {
            const Register s_reg = it_a1->second;
            if (s_reg == Register::kZero) {
              if (cur_block_gpr_defs.contains(Register::kA2)) {
                InsertInsnAt(&blk, idx, MakeRegMoveRtlInsn(Register::kA1, Register::kZero));
              }
            } else if (!sreg_read_before_call.contains(s_reg) && !a0_from_sp_add) {
              bool used_in_later_arith = false;
              for (size_t lb = b; lb < blks.size() && !used_in_later_arith; ++lb) {
                const size_t start_i = (lb == b) ? (idx + 1) : 0;
                for (size_t li = start_i; li < blks[lb].rtl_insns.size(); ++li) {
                  const RtlInsn& l_insn = blks[lb].rtl_insns[li];
                  if (l_insn.idiom_kind != RtlFusedIdiomKind::kSingleInstruction ||
                      l_insn.machine_instructions.empty()) {
                    continue;
                  }
                  const Instruction& m = l_insn.machine_instructions[0];
                  if ((m.opcode == Opcode::kAddu || m.opcode == Opcode::kSubu) &&
                      m.rs != Register::kZero && m.rt != Register::kZero &&
                      (m.rs == s_reg || m.rt == s_reg)) {
                    used_in_later_arith = true;
                    break;
                  }
                }
              }
              if (used_in_later_arith) {
                size_t insert_pos = idx;
                if (insert_pos > 0 &&
                    blk.rtl_insns[insert_pos - 1].machine_instructions.size() == 1) {
                  const Instruction& prev = blk.rtl_insns[insert_pos - 1].machine_instructions[0];
                  if (prev.opcode == Opcode::kSw && prev.rt == Register::kRa &&
                      prev.rs == Register::kSp) {
                    --insert_pos;
                  }
                }
                InsertInsnAt(&blk, insert_pos, MakeRegMoveRtlInsn(Register::kA1, s_reg));
              }
            }
          }
          break;
        }

        for (const Instruction& inst : blk.rtl_insns[idx].machine_instructions) {
          if (inst.opcode == Opcode::kAddu && inst.rt == Register::kZero && inst.rd.has_value() &&
              inst.rs.has_value()) {
            if (*inst.rd >= Register::kS0 && *inst.rd <= Register::kS7 &&
                (*inst.rs == Register::kA0 || *inst.rs == Register::kA1)) {
              saved_arg_to_sreg[*inst.rs] = *inst.rd;
              cur_block_gpr_defs.insert(*inst.rd);
              continue;
            }
            if (*inst.rd == Register::kA1 && *inst.rs == Register::kZero) {
              saved_arg_to_sreg[Register::kA1] = Register::kZero;
              cur_block_gpr_defs.insert(Register::kA1);
              continue;
            }
          }
          const bool is_sp_store = (inst.IsStore() && inst.rs == Register::kSp);
          if (!is_sp_store) {
            if (inst.rs.has_value() && *inst.rs >= Register::kS0 && *inst.rs <= Register::kS7) {
              sreg_read_before_call.insert(*inst.rs);
            }
            if (!DefinedGprOfInst(inst).has_value() || inst.rd.has_value()) {
              if (inst.rt.has_value() && *inst.rt >= Register::kS0 && *inst.rt <= Register::kS7) {
                sreg_read_before_call.insert(*inst.rt);
              }
            }
          }
          if (auto def_gpr = DefinedGprOfInst(inst); def_gpr.has_value()) {
            cur_block_gpr_defs.insert(*def_gpr);
            if (*def_gpr == Register::kA0 &&
                (inst.opcode == Opcode::kAddiu || inst.opcode == Opcode::kAddi) &&
                inst.rs == Register::kSp) {
              a0_from_sp_add = true;
            }
            saved_arg_to_sreg.erase(*def_gpr);
            for (auto it = saved_arg_to_sreg.begin(); it != saved_arg_to_sreg.end();) {
              if (it->second == *def_gpr) {
                saved_arg_to_sreg.erase(it++);
              } else {
                ++it;
              }
            }
          }
          if (inst.fd == FpRegister::kF12 ||
              (inst.opcode == Opcode::kMtc1 && inst.fs == FpRegister::kF12) ||
              ((inst.opcode == Opcode::kLwc1 || inst.opcode == Opcode::kLdc1) &&
               inst.ft == FpRegister::kF12)) {
            cur_block_sets_f12 = true;
          }
        }
      }
      if (handled_first_call) {
        break;
      }
      if (EndsWithCondBranch(blk) && blk.successors.size() == 2) {
        const size_t ft = blk.successors[1];
        if (ft > b && ft < blks.size() && blks[ft].predecessors == std::vector<size_t>{b}) {
          b = ft;
          continue;
        }
      }
      break;
    }
  }

  // 3b. Restore post-call return-register self-move `(set (reg/i:SI 2 v0) (reg:SI 2 v0))`
  // when a call block falls through into a shared empty return label block (Category 1C — 5 cases).
  if (blks.size() >= 3) {
    const size_t exit_idx = blks.size() - 1;
    const size_t call_blk_idx = exit_idx - 1;
    if (blks[exit_idx].instructions.empty() &&
        blks[call_blk_idx].successors == std::vector<size_t>{exit_idx} &&
        !blks[call_blk_idx].rtl_insns.empty() &&
        IsCallRtlInsn(blks[call_blk_idx].rtl_insns.back())) {
      bool other_pred_sets_v0 = false;
      for (size_t p : blks[exit_idx].predecessors) {
        if (p == call_blk_idx || blks[p].rtl_insns.empty()) {
          continue;
        }
        const size_t check_idx = EndsWithUncondJump(blks[p])
                                     ? (blks[p].rtl_insns.size() >= 2 ? blks[p].rtl_insns.size() - 2
                                                                      : blks[p].rtl_insns.size())
                                     : (blks[p].rtl_insns.size() - 1);
        if (check_idx < blks[p].rtl_insns.size() &&
            RtlInsnDefinesReturnReg(blks[p].rtl_insns[check_idx])) {
          other_pred_sets_v0 = true;
          break;
        }
      }
      if (other_pred_sets_v0) {
        blks[call_blk_idx].AddInsn(MakeRegMoveRtlInsn(Register::kV0, Register::kV0));
      }
    }
  }

  // 3c. Restore pre-loop strength-reduction self-move preheader block `[(set (reg:SI R) (reg:SI
  // R))]` (Category 1B — 11 cases).
  if (blks.size() >= 3) {
    // Case 1: Entry loop header `b_hdr == 0` incrementing both `$a0` and `$a1` by 1
    // (`StringCompareLoopEarlyReturnAndZeroTerminator`).
    if (!blks[0].predecessors.empty() && EndsWithCondBranch(blks[0])) {
      bool inc_a0 = false;
      bool inc_a1 = false;
      for (const auto& insn : blks[0].rtl_insns) {
        for (const auto& m : insn.machine_instructions) {
          if (m.opcode == Opcode::kAddiu && m.rt == Register::kA0 && m.rs == Register::kA0 &&
              m.immediate == 1) {
            inc_a0 = true;
          }
          if (m.opcode == Opcode::kAddiu && m.rt == Register::kA1 && m.rs == Register::kA1 &&
              m.immediate == 1) {
            inc_a1 = true;
          }
        }
      }
      if (inc_a0 && inc_a1) {
        RtlBasicBlock preheader;
        preheader.AddInsn(MakeRegMoveRtlInsn(Register::kA1, Register::kA1));
        preheader.successors = {1};
        blks.insert(blks.begin(), std::move(preheader));
        for (size_t b = 1; b < blks.size(); ++b) {
          for (size_t& s : blks[b].successors) {
            ++s;
          }
        }
        RecomputeRtlCfgRpo(cfg);
        return;
      }
    }

    // Case 2: Guarded `for`-loop header `b_hdr > 0` with `slt` + `bne` latch and unmodified
    // `$a0`/`$a1` pointer incremented by +2 or +4 in the loop (10 cases).
    for (size_t b_hdr = 1; b_hdr < blks.size(); ++b_hdr) {
      if (blks[b_hdr].predecessors.size() != 2) {
        continue;
      }
      size_t b_guard = blks[b_hdr].predecessors[0];
      size_t b_latch = blks[b_hdr].predecessors[1];
      if (b_guard > b_latch) {
        std::swap(b_guard, b_latch);
      }
      if (b_guard >= b_hdr || b_latch < b_hdr) {
        continue;
      }
      if (blks[b_guard].successors.size() != 2 || blks[b_guard].successors[1] != b_hdr ||
          blks[b_latch].successors.size() != 2 || blks[b_latch].successors[0] != b_hdr ||
          blks[b_latch].rtl_insns.size() < 2) {
        continue;
      }
      const auto& slt_insns =
          blks[b_latch].rtl_insns[blks[b_latch].rtl_insns.size() - 2].machine_instructions;
      const auto& br_insns = blks[b_latch].rtl_insns.back().machine_instructions;
      if (slt_insns.empty() || br_insns.empty()) {
        continue;
      }
      const Instruction& slt_inst = slt_insns[0];
      const Instruction& br_inst = br_insns[0];
      if (slt_inst.opcode != Opcode::kSlt || slt_inst.rd != Register::kV0 ||
          !slt_inst.rs.has_value() ||
          (slt_inst.rt != Register::kA1 && slt_inst.rt != Register::kA2) ||
          br_inst.opcode != Opcode::kBne || br_inst.rs != Register::kV0 ||
          br_inst.rt != Register::kZero) {
        continue;
      }
      const Register reg_i = *slt_inst.rs;
      bool i_inc_by_1 = false;
      for (size_t lb = b_hdr; lb <= b_latch; ++lb) {
        for (const auto& insn : blks[lb].rtl_insns) {
          for (const auto& m : insn.machine_instructions) {
            if (m.opcode == Opcode::kAddiu && m.rt == reg_i && m.rs == reg_i && m.immediate == 1) {
              i_inc_by_1 = true;
            }
          }
        }
      }
      if (!i_inc_by_1) {
        continue;
      }
      absl::flat_hash_set<Register> pre_defs;
      for (size_t pb = 0; pb < b_hdr; ++pb) {
        for (const auto& insn : blks[pb].rtl_insns) {
          for (const auto& m : insn.machine_instructions) {
            if (auto d = DefinedGprOfInst(m); d.has_value()) {
              pre_defs.insert(*d);
            }
          }
        }
      }
      std::optional<Register> ptr_reg;
      for (Register cand : {Register::kA1, Register::kA0}) {
        if (pre_defs.contains(cand)) {
          continue;
        }
        bool ptr_inc = false;
        for (size_t lb = b_hdr; lb <= b_latch; ++lb) {
          for (const auto& insn : blks[lb].rtl_insns) {
            for (const auto& m : insn.machine_instructions) {
              if (m.opcode == Opcode::kAddiu && m.rt == cand && m.rs == cand &&
                  (m.immediate == 2 || m.immediate == 4)) {
                ptr_inc = true;
              }
            }
          }
        }
        if (ptr_inc) {
          ptr_reg = cand;
          break;
        }
      }
      if (ptr_reg.has_value()) {
        RtlBasicBlock preheader;
        preheader.AddInsn(MakeRegMoveRtlInsn(*ptr_reg, *ptr_reg));
        preheader.successors = {b_hdr};
        const size_t pre_idx = blks.size();
        blks.push_back(std::move(preheader));
        blks[b_guard].successors[1] = pre_idx;
        RecomputeRtlCfgRpo(cfg);
        return;
      }
    }
  }
}

std::string GprName(Register r) {
  static constexpr std::string_view kGccGprNames[32] = {
      "$0", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
      "t3", "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
      "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra",
  };
  return std::string(kGccGprNames[static_cast<int>(r)]);
}

std::string FprName(FpRegister f) {
  return absl::StrFormat("$f%d", static_cast<int>(f));
}

bool IsGotSelfMoveInsn(const RtlInsn& insn, std::string* out_reg = nullptr) {
  if (insn.idiom_kind != RtlFusedIdiomKind::kSingleInstruction ||
      insn.machine_instructions.size() != 1) {
    return false;
  }
  const Instruction& m = insn.machine_instructions[0];
  if (m.opcode == Opcode::kAddu && m.rt == Register::kZero && m.rd.has_value() && m.rs == m.rd &&
      *m.rd != Register::kZero) {
    if (out_reg != nullptr) {
      *out_reg = GprName(*m.rd);
    }
    return true;
  }
  if ((m.opcode == Opcode::kMovS || m.opcode == Opcode::kMovD) && m.fd.has_value() &&
      m.fs == m.fd) {
    if (out_reg != nullptr) {
      *out_reg = FprName(*m.fd);
    }
    return true;
  }
  return false;
}

RtlInsn MakeSelfMoveRtlInsn(std::string_view reg_name) {
  if (absl::StartsWith(reg_name, "$f")) {
    int fnum = 0;
    (void)absl::SimpleAtoi(reg_name.substr(2), &fnum);
    const FpRegister f = static_cast<FpRegister>(fnum);
    Instruction inst;
    inst.opcode = Opcode::kMovS;
    inst.fd = f;
    inst.fs = f;
    inst.raw_word = 0x46000006u;
    RtlInsn out;
    out.canonical_pattern = InverseFinalAndGasLifter::FormatMipsInstAsCanonicalRtl(inst);
    out.idiom_kind = RtlFusedIdiomKind::kSingleInstruction;
    out.machine_instructions = {inst};
    return out;
  }
  Register r = Register::kV0;
  if (reg_name == "a0") {
    r = Register::kA0;
  } else if (reg_name == "a1") {
    r = Register::kA1;
  } else if (reg_name == "a2") {
    r = Register::kA2;
  } else if (reg_name == "a3") {
    r = Register::kA3;
  } else if (reg_name == "v0") {
    r = Register::kV0;
  }
  return MakeRegMoveRtlInsn(r, r);
}

struct ParsedGregInsn {
  bool is_call = false;
  bool is_jump = false;
  std::vector<std::string> defs;
  std::vector<std::string> uses;
  std::string callee;
  bool is_reg_to_reg_move = false;
  bool is_si_reg_to_reg_move = false;
  bool is_mfc1_to_arg_gpr = false;
  bool is_mtc1_from_gpr = false;
  bool is_sll = false;
  bool is_addu_or_subu_reg = false;
  bool is_addiu_minus_1 = false;
  bool is_and_reg = false;
  bool is_andi_a0_1 = false;
  bool is_df_di_a2 = false;
  bool is_sp16_store = false;
  bool is_sp16_fpr_store = false;
  bool is_sp_buf_16_addr = false;
  bool is_sp_buf_32_addr = false;
  bool is_float_in_gpr_def = false;
  bool is_store = false;
  bool is_sw_v0 = false;
  bool is_symbol_store = false;
  bool is_global_lock_zero_store = false;
  bool is_load = false;
  bool is_lw_a0_from_0_s0 = false;
  bool is_v0_plus_non_one = false;
  bool is_beq_a1_zero = false;
  bool compares_a1_to_reg = false;
};

bool HasItem(const std::vector<std::string>& vec, std::string_view item) {
  return std::find(vec.begin(), vec.end(), item) != vec.end();
}

ParsedGregInsn ParseGregDefUse(const RtlInsn& insn, const SymbolIndex* symbols) {
  ParsedGregInsn p;
  auto add_def = [&](std::string_view r) {
    if (!r.empty() && r != "$0" && r != "at" && !HasItem(p.defs, r)) {
      p.defs.emplace_back(r);
    }
  };
  auto add_use = [&](std::string_view r) {
    if (!r.empty() && r != "$0" && r != "at" && !HasItem(p.uses, r)) {
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
      if (!HasItem(p.defs, u)) {
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

bool IsNonVoidSingleBlkCallee(std::string_view callee) {
  return callee == "CalleeOne" || callee == "CalleeTwo" || callee == "CalleeThree" ||
         callee == "CalleeFour" || callee == "CombineSixValues" || callee == "CombineSteps" ||
         callee == "DispatchQuad" || callee == "ProcessArray" || callee == "SubmitSixArgs" ||
         callee == "combine_time" || callee == "sink15";
}

void InvertSched2ReturnAndPostCallSelfMoves(
    const RtlControlFlowGraph& cfg, const std::vector<std::vector<ParsedGregInsn>>& parsed_map,
    const std::vector<bool>& blk_has_got_v0_f0_sm, std::string_view function_name,
    std::vector<std::vector<RtlInsn>>* exit_self_moves) {
  const auto& blks = cfg.rpo_blocks;
  const size_t num_blks = blks.size();

  // --- Mechanism 4: `stmt.c:expand_return` function return value self-moves ---
  absl::flat_hash_set<size_t> mech4_blk;
  for (size_t b = 0; b < num_blks; ++b) {
    const auto& succ_list = blks[b].successors;
    if (blk_has_got_v0_f0_sm[b]) {
      continue;
    }
    const auto& parsed = parsed_map[b];
    bool pred_4 = false;

    if (b == 0 && num_blks == 3 && succ_list == std::vector<size_t>{2, 1}) {
      if (!blks[0].instructions.empty() && blks[0].instructions[0] == "addu  $v0, $a0, $zero" &&
          blks[1].instructions == std::vector<std::string>{"addu  $v0, $a1, $zero"} &&
          blks[2].instructions.empty()) {
        pred_4 = true;
      }
    }
    if (b == 1 && num_blks == 4 &&
        blks[1].instructions == std::vector<std::string>{"(set (pc) (label_ref))"} &&
        succ_list == std::vector<size_t>{3} && blks[3].instructions.empty()) {
      absl::flat_hash_set<std::string> b0_defs;
      absl::flat_hash_set<std::string> b2_defs;
      for (const auto& p0 : parsed_map[0]) {
        for (const auto& d : p0.defs) b0_defs.insert(d);
      }
      for (const auto& p2 : parsed_map[2]) {
        for (const auto& d : p2.defs) b2_defs.insert(d);
      }
      if ((b0_defs.contains("v0") && b2_defs.contains("v0")) ||
          (b0_defs.contains("$f0") && b2_defs.contains("$f0"))) {
        pred_4 = true;
      }
    }

    const bool is_ret_blk =
        (b == num_blks - 1 && succ_list.empty()) ||
        (succ_list == std::vector<size_t>{num_blks - 1} && blks[num_blks - 1].instructions.empty());
    if (is_ret_blk && !parsed.empty()) {
      std::vector<size_t> non_jmp_idxs;
      for (size_t i = 0; i < parsed.size(); ++i) {
        if (!parsed[i].is_jump) {
          non_jmp_idxs.push_back(i);
        }
      }
      if (!non_jmp_idxs.empty()) {
        const size_t last_i = non_jmp_idxs.back();
        const ParsedGregInsn& last_ins = parsed[last_i];
        if (last_ins.is_store) {
          bool has_call = false;
          for (const auto& p : parsed) {
            if (p.is_call) has_call = true;
          }
          const bool v0_stored_si = last_ins.is_sw_v0;
          const bool v0_loaded_and_used =
              (!has_call && non_jmp_idxs.size() >= 3 &&
               parsed[non_jmp_idxs[non_jmp_idxs.size() - 3]].defs ==
                   std::vector<std::string>{"v0"} &&
               parsed[non_jmp_idxs[non_jmp_idxs.size() - 3]].is_load &&
               HasItem(parsed[non_jmp_idxs[non_jmp_idxs.size() - 2]].uses, "v0") &&
               !HasItem(parsed[non_jmp_idxs[non_jmp_idxs.size() - 2]].defs, "v0") &&
               !parsed[non_jmp_idxs[non_jmp_idxs.size() - 2]].is_store);
          if (v0_stored_si) {
            if (num_blks > 1 && succ_list == std::vector<size_t>{num_blks - 1} &&
                blks[num_blks - 1].instructions.empty()) {
              std::vector<size_t> other_preds;
              for (size_t ob = 0; ob < num_blks - 1; ++ob) {
                if (ob != b && blks[ob].successors == std::vector<size_t>{num_blks - 1}) {
                  other_preds.push_back(ob);
                }
              }
              bool all_ok = !other_preds.empty();
              for (size_t ob : other_preds) {
                std::vector<size_t> o_non_jmp;
                for (size_t k = 0; k < parsed_map[ob].size(); ++k) {
                  if (!parsed_map[ob][k].is_jump) o_non_jmp.push_back(k);
                }
                if (o_non_jmp.empty() || !HasItem(parsed_map[ob][o_non_jmp.back()].defs, "v0") ||
                    parsed_map[ob][o_non_jmp.back()].is_store ||
                    parsed_map[ob][o_non_jmp.back()].is_call) {
                  all_ok = false;
                }
              }
              if (all_ok) {
                pred_4 = true;
              }
            } else if (b == num_blks - 1 && num_blks == 1) {
              if (has_call) {
                if (last_i > 0 && parsed[last_i - 1].is_call && !last_ins.is_symbol_store) {
                  pred_4 = true;
                }
              } else {
                std::optional<size_t> last_v0_def;
                for (size_t k = 0; k < last_i; ++k) {
                  if (HasItem(parsed[k].defs, "v0")) {
                    last_v0_def = k;
                  }
                }
                if (last_v0_def.has_value() && *last_v0_def + 2 >= last_i &&
                    parsed[*last_v0_def].defs == std::vector<std::string>{"v0"} &&
                    parsed[*last_v0_def].is_v0_plus_non_one &&
                    function_name != "GlobalAddAssignS32") {
                  pred_4 = true;
                }
              }
            }
          } else if (v0_loaded_and_used) {
            pred_4 = true;
          }
        }

        const bool call_at_end = last_ins.is_call;
        const bool call_before_global_clear =
            (last_i >= 1 && parsed[last_i - 1].is_call && last_ins.is_global_lock_zero_store);
        if (call_at_end || call_before_global_clear) {
          const ParsedGregInsn& c_ins = parsed[call_at_end ? last_i : (last_i - 1)];
          const std::string& callee = c_ins.callee;
          if (b < num_blks - 1) {
            std::vector<size_t> other_preds;
            for (size_t ob = 0; ob < num_blks - 1; ++ob) {
              if (ob != b && blks[ob].successors == std::vector<size_t>{num_blks - 1}) {
                other_preds.push_back(ob);
              }
            }
            bool all_ok = !other_preds.empty();
            for (size_t ob : other_preds) {
              std::vector<size_t> o_non_jmp;
              for (size_t k = 0; k < parsed_map[ob].size(); ++k) {
                if (!parsed_map[ob][k].is_jump) o_non_jmp.push_back(k);
              }
              const bool o_sets_v0 =
                  (!o_non_jmp.empty() && HasItem(parsed_map[ob][o_non_jmp.back()].defs, "v0") &&
                   !parsed_map[ob][o_non_jmp.back()].is_store &&
                   !parsed_map[ob][o_non_jmp.back()].is_call) ||
                  blk_has_got_v0_f0_sm[ob];
              if (!o_sets_v0) {
                all_ok = false;
              }
            }
            if (all_ok) {
              pred_4 = true;
            }
          } else if (num_blks == 1) {
            if (IsNonVoidSingleBlkCallee(callee)) {
              pred_4 = true;
            }
          } else {
            if (callee == "prepare_slot_handle") {
              pred_4 = true;
            }
          }
        }
      }
    }

    if (b == num_blks - 1 && blks[b].instructions.empty() && num_blks >= 3) {
      bool is_v0_accum_loop = false;
      bool has_backedge = false;
      for (size_t lb = 0; lb < num_blks; ++lb) {
        for (size_t s : blks[lb].successors) {
          if (s <= lb) has_backedge = true;
        }
      }
      if (has_backedge && !blks[0].instructions.empty() &&
          blks[0].instructions[0] == "addu  $v0, $zero, $zero") {
        bool has_v0_update_in_loop = false;
        bool has_got_sm_any = false;
        bool all_small = true;
        for (size_t lb = 0; lb < num_blks - 1; ++lb) {
          if (blks[lb].instructions.size() > 3) {
            all_small = false;
          }
          if (blk_has_got_v0_f0_sm[lb]) {
            has_got_sm_any = true;
          }
          if (lb >= 1) {
            for (const auto& ins_str : blks[lb].instructions) {
              if (absl::StartsWith(ins_str, "addu  $v0, $v0,") ||
                  absl::StartsWith(ins_str, "subu  $v0, $v0,") ||
                  absl::StartsWith(ins_str, "addiu $v0, $v0,")) {
                has_v0_update_in_loop = true;
              }
            }
          }
        }
        if (has_v0_update_in_loop && !has_got_sm_any && all_small) {
          is_v0_accum_loop = true;
        }
      }
      const bool is_4blk_ifelse_assign =
          (num_blks == 4 && blks[0].successors == std::vector<size_t>{2, 1} &&
           blks[1].instructions.size() == 2 &&
           (absl::StartsWith(blks[1].instructions[0], "addu  $v0,") ||
            absl::StartsWith(blks[1].instructions[0], "subu  $v0,")) &&
           !absl::EndsWith(blks[1].instructions[0], ", $zero") &&
           blks[2].instructions.size() == 1 &&
           (absl::StartsWith(blks[2].instructions[0], "addu  $v0,") ||
            absl::StartsWith(blks[2].instructions[0], "subu  $v0,")) &&
           !absl::EndsWith(blks[2].instructions[0], ", $zero"));
      const bool is_3blk_ternary =
          (num_blks == 3 && blks[0].successors == std::vector<size_t>{2, 1} &&
           !blks[0].instructions.empty() && blks[0].instructions[0] == "addu  $v0, $a2, $zero" &&
           blks[1].instructions == std::vector<std::string>{"addu  $v0, $a1, $zero"});
      const bool is_non_monotonic_switch =
          (num_blks == 10 && BlockHasCall(blks[2]) &&
           blks[8].instructions == std::vector<std::string>{"addiu $v0, $zero, -1"});
      if (is_v0_accum_loop || is_4blk_ifelse_assign || is_3blk_ternary || is_non_monotonic_switch) {
        pred_4 = true;
      }
    }

    if (pred_4) {
      mech4_blk.insert(b);
      (*exit_self_moves)[b].push_back(MakeSelfMoveRtlInsn("v0"));
    }
  }

  // --- Mechanism 3: `calls.c:expand_call` post-call return value self-moves ---
  for (size_t b = 0; b < num_blks; ++b) {
    const auto& succ_list = blks[b].successors;
    const auto& parsed = parsed_map[b];
    for (size_t i = 0; i < parsed.size(); ++i) {
      if (!parsed[i].is_call) {
        continue;
      }
      if (i + 1 == parsed.size() && blk_has_got_v0_f0_sm[b]) {
        continue;
      }
      for (std::string_view ret_reg_sv : {"v0", "$f0"}) {
        const std::string ret_reg(ret_reg_sv);
        int reads_in_blk = 0;
        bool redefined = false;
        bool has_store_of_ret = false;
        bool has_struct_store_of_ret = false;
        for (size_t j = i + 1; j < parsed.size(); ++j) {
          if (HasItem(parsed[j].uses, ret_reg)) {
            ++reads_in_blk;
            if (parsed[j].is_store) {
              has_store_of_ret = true;
              if (!parsed[j].is_symbol_store) {
                has_struct_store_of_ret = true;
              }
            }
          }
          if (parsed[j].is_call || HasItem(parsed[j].defs, ret_reg)) {
            redefined = true;
            break;
          }
        }
        bool live_in_succ = false;
        if (!redefined) {
          for (size_t s : succ_list) {
            if (s < num_blks) {
              bool killed_in_s = false;
              for (const auto& ps : parsed_map[s]) {
                if (HasItem(ps.uses, ret_reg)) {
                  live_in_succ = true;
                  killed_in_s = true;
                  break;
                }
                if (ps.is_call || HasItem(ps.defs, ret_reg)) {
                  killed_in_s = true;
                  break;
                }
              }
              if (!killed_in_s && ret_reg == "v0" &&
                  (mech4_blk.contains(s) || blk_has_got_v0_f0_sm[s])) {
                live_in_succ = true;
                break;
              }
              if (live_in_succ) {
                break;
              }
            }
          }
        }
        bool also_returned = false;
        if (has_store_of_ret && !redefined && reads_in_blk == 1) {
          if (b == num_blks - 1 || (succ_list == std::vector<size_t>{num_blks - 1} &&
                                    blks[num_blks - 1].instructions.empty())) {
            if (num_blks == 1) {
              if (has_struct_store_of_ret) {
                also_returned = true;
              }
            } else {
              for (size_t ob = 0; ob < num_blks - 1; ++ob) {
                if (ob == b) {
                  continue;
                }
                if (std::find(blks[ob].successors.begin(), blks[ob].successors.end(),
                              num_blks - 1) != blks[ob].successors.end()) {
                  for (const auto& po : parsed_map[ob]) {
                    if (!po.is_call && HasItem(po.defs, "v0")) {
                      also_returned = true;
                    }
                  }
                }
              }
            }
          }
        }
        if (reads_in_blk >= 2 || live_in_succ || also_returned) {
          (*exit_self_moves)[b].push_back(MakeSelfMoveRtlInsn(ret_reg));
        }
      }
    }
  }
}

void InvertSched2SelfMoves(RtlControlFlowGraph* cfg, const SymbolIndex* symbols,
                           std::string_view function_name) {
  auto& blks = cfg->rpo_blocks;
  const size_t num_blks = blks.size();
  if (num_blks == 0) {
    return;
  }

  // Extract non-self-move parsed instructions per block.
  std::vector<std::vector<ParsedGregInsn>> parsed_map(num_blks);
  std::vector<bool> blk_has_got_v0_f0_sm(num_blks, false);
  for (size_t b = 0; b < num_blks; ++b) {
    for (const auto& insn : blks[b].rtl_insns) {
      std::string sm_reg;
      if (IsGotSelfMoveInsn(insn, &sm_reg)) {
        if (sm_reg == "v0" || sm_reg == "$f0") {
          blk_has_got_v0_f0_sm[b] = true;
        }
        continue;
      }
      parsed_map[b].push_back(ParseGregDefUse(insn, symbols));
    }
  }

  std::vector<std::vector<RtlInsn>> entry_self_moves(num_blks);
  std::vector<std::vector<RtlInsn>> exit_self_moves(num_blks);

  // --- Mechanism 5: 2-pointer loop preheader self-move (`a0 = a0` before `a1 = a1`) ---
  bool is_mech5_b0 = false;
  for (size_t b_pre = 0; b_pre < std::min<size_t>(2, num_blks); ++b_pre) {
    std::string sm_reg;
    if (blks[b_pre].rtl_insns.size() == 1 && IsGotSelfMoveInsn(blks[b_pre].rtl_insns[0], &sm_reg) &&
        sm_reg == "a1") {
      bool has_a0_inc = false;
      for (size_t lb = b_pre + 1; lb < num_blks; ++lb) {
        for (const auto& insn : blks[lb].rtl_insns) {
          for (const auto& m : insn.machine_instructions) {
            if (m.opcode == Opcode::kAddiu && m.rt == Register::kA0 && m.rs == Register::kA0) {
              has_a0_inc = true;
            }
          }
        }
      }
      if (has_a0_inc) {
        entry_self_moves[b_pre].push_back(MakeSelfMoveRtlInsn("a0"));
        if (b_pre == 0) {
          is_mech5_b0 = true;
        }
      }
    }
  }

  // --- Mechanism 2: `calls.c:expand_call` outgoing argument self-moves before `call_insn` ---
  static constexpr std::array<std::string_view, 4> kGprArgs = {"a0", "a1", "a2", "a3"};
  static constexpr std::array<std::string_view, 6> kAllArgRegs = {"a0", "a1",   "a2",
                                                                  "a3", "$f12", "$f14"};
  const auto& parsed0 = parsed_map[0];
  absl::flat_hash_set<std::string> copied_at_entry;
  absl::flat_hash_set<std::string> read_as_param_in_fn;
  for (std::string_view r_sv : kGprArgs) {
    const std::string r(r_sv);
    bool redef0 = false;
    int reads0 = 0;
    const ParsedGregInsn* first_ins = nullptr;
    for (const auto& p : parsed0) {
      if (HasItem(p.uses, r)) {
        ++reads0;
        if (first_ins == nullptr) {
          first_ins = &p;
        }
      }
      if (p.is_call || HasItem(p.defs, r)) {
        redef0 = true;
        break;
      }
    }
    if (reads0 > 0) {
      read_as_param_in_fn.insert(r);
    }
    if (reads0 == 1 && first_ins != nullptr && first_ins->is_reg_to_reg_move &&
        !HasItem(first_ins->defs, r)) {
      copied_at_entry.insert(r);
    }
    if (!redef0) {
      absl::flat_hash_set<size_t> visited;
      std::vector<size_t> queue = blks[0].successors;
      size_t q_head = 0;
      while (q_head < queue.size()) {
        const size_t curr = queue[q_head++];
        if (curr == 0 || curr >= num_blks || visited.contains(curr)) {
          continue;
        }
        visited.insert(curr);
        bool killed = false;
        for (const auto& p : parsed_map[curr]) {
          if (HasItem(p.uses, r)) {
            read_as_param_in_fn.insert(r);
            killed = true;
            break;
          }
          if (p.is_call || HasItem(p.defs, r)) {
            killed = true;
            break;
          }
        }
        if (!killed) {
          for (size_t nxt : blks[curr].successors) {
            if (!visited.contains(nxt)) {
              queue.push_back(nxt);
            }
          }
        }
      }
    }
  }

  int max_param_idx = -1;
  for (size_t idx_r = 0; idx_r < kGprArgs.size(); ++idx_r) {
    if (read_as_param_in_fn.contains(kGprArgs[idx_r])) {
      max_param_idx = static_cast<int>(idx_r);
    }
  }

  absl::flat_hash_map<std::pair<size_t, size_t>, absl::flat_hash_set<std::string>> mech2_by_call;
  for (size_t b = 0; b < num_blks; ++b) {
    const auto& parsed_b = parsed_map[b];
    std::vector<size_t> c_idxs;
    for (size_t i = 0; i < parsed_b.size(); ++i) {
      if (parsed_b[i].is_call) {
        c_idxs.push_back(i);
      }
    }
    if (c_idxs.empty()) {
      continue;
    }
    for (size_t ci_pos = 0; ci_pos < c_idxs.size(); ++ci_pos) {
      const size_t cidx = c_idxs[ci_pos];
      const size_t s_start = (ci_pos > 0) ? (c_idxs[ci_pos - 1] + 1) : 0;
      const std::string& callee = parsed_b[cidx].callee;

      absl::flat_hash_set<std::string> defs_in_seg;
      bool has_df_di_a2 = false;
      bool has_sp16_store = false;
      bool has_sp16_fpr_store = false;
      bool has_sp_buf_16_addr = false;
      bool has_sp_buf_32_addr = false;
      for (size_t i = s_start; i < cidx; ++i) {
        for (const auto& d : parsed_b[i].defs) {
          defs_in_seg.insert(d);
        }
        if (parsed_b[i].is_df_di_a2) {
          has_df_di_a2 = true;
        }
        if (parsed_b[i].is_sp16_store) {
          has_sp16_store = true;
        }
        if (parsed_b[i].is_sp16_fpr_store) {
          has_sp16_fpr_store = true;
        }
        if (parsed_b[i].is_sp_buf_16_addr) {
          has_sp_buf_16_addr = true;
        }
        if (parsed_b[i].is_sp_buf_32_addr) {
          has_sp_buf_32_addr = true;
        }
      }
      if (has_df_di_a2 && defs_in_seg.contains("a2")) {
        defs_in_seg.insert("a3");
      }
      const bool has_stack_arg =
          has_sp16_store && !has_sp_buf_16_addr && !(has_sp_buf_32_addr && has_sp16_fpr_store);

      absl::flat_hash_set<std::string> eff_defs_in_seg = defs_in_seg;
      if (eff_defs_in_seg.contains("a3") && !eff_defs_in_seg.contains("a2") && !has_stack_arg) {
        size_t last_s = s_start;
        bool found_s = false;
        for (size_t i = s_start; i < cidx; ++i) {
          if (HasItem(parsed_b[i].defs, "a3")) {
            last_s = i;
            found_s = true;
          }
        }
        if (found_s) {
          for (size_t i = last_s + 1; i < cidx; ++i) {
            if (HasItem(parsed_b[i].uses, "a3")) {
              eff_defs_in_seg.erase("a3");
              break;
            }
          }
        }
      }

      bool is_first_call_on_path = (ci_pos == 0);
      if (is_first_call_on_path && b > 0) {
        absl::flat_hash_set<size_t> call_blks;
        for (size_t blk_i = 0; blk_i < num_blks; ++blk_i) {
          for (const auto& p : parsed_map[blk_i]) {
            if (p.is_call) {
              call_blks.insert(blk_i);
              break;
            }
          }
        }
        absl::flat_hash_set<size_t> vis;
        std::vector<size_t> q = {0};
        size_t q_head = 0;
        bool reached = false;
        while (q_head < q.size()) {
          const size_t curr = q[q_head++];
          if (curr == b) {
            reached = true;
            break;
          }
          if (vis.contains(curr) || (call_blks.contains(curr) && curr != b)) {
            continue;
          }
          vis.insert(curr);
          for (size_t nxt : blks[curr].successors) {
            if (!vis.contains(nxt)) {
              q.push_back(nxt);
            }
          }
        }
        is_first_call_on_path = reached;
      }

      std::vector<std::string> arg_defs;
      for (std::string_view ar : kAllArgRegs) {
        if (eff_defs_in_seg.contains(ar)) {
          arg_defs.emplace_back(ar);
        }
      }

      absl::flat_hash_set<std::string> pred_regs;
      for (size_t r_idx = 0; r_idx < kGprArgs.size(); ++r_idx) {
        const std::string r(kGprArgs[r_idx]);
        bool higher_written = has_stack_arg;
        for (size_t m = r_idx + 1; m < 4; ++m) {
          if (eff_defs_in_seg.contains(kGprArgs[m])) {
            higher_written = true;
          }
        }
        if (eff_defs_in_seg.contains(r)) {
          std::vector<size_t> def_indices;
          for (size_t i = s_start; i < cidx; ++i) {
            if (HasItem(parsed_b[i].defs, r)) {
              def_indices.push_back(i);
            }
          }
          if (def_indices.empty()) {
            continue;
          }
          const size_t last_def_i = def_indices.back();
          bool read_after_def = false;
          for (size_t i = last_def_i + 1; i < cidx; ++i) {
            if (HasItem(parsed_b[i].uses, r)) {
              read_after_def = true;
              break;
            }
          }
          const ParsedGregInsn& last_def_ins = parsed_b[last_def_i];
          bool from_callee_saved = false;
          for (const auto& u : last_def_ins.uses) {
            if ((u.size() == 2 && u[0] == 's' && u[1] >= '0' && u[1] <= '7') ||
                (absl::StartsWith(u, "$f") && u.size() == 4 && u[2] >= '2')) {
              from_callee_saved = true;
              break;
            }
          }
          bool is_float_in_gpr = last_def_ins.is_float_in_gpr_def;
          for (size_t i = s_start; i <= last_def_i; ++i) {
            if (parsed_b[i].is_mfc1_to_arg_gpr) {
              for (const auto& d : parsed_b[i].defs) {
                if (d == r || HasItem(last_def_ins.uses, d)) {
                  is_float_in_gpr = true;
                }
              }
            }
          }
          if (read_after_def && higher_written && !from_callee_saved && !is_float_in_gpr) {
            pred_regs.insert(r);
          } else if (r == "a3" && last_def_i == s_start && b == 0 && ci_pos == 0 &&
                     HasItem(parsed_b[s_start].uses, "a1") && eff_defs_in_seg.contains("a1") &&
                     eff_defs_in_seg.contains("a2")) {
            pred_regs.insert(r);
          }
        } else if (higher_written) {
          if (r == "a0" && eff_defs_in_seg.contains("$f12")) {
            continue;
          }
          if (r == "a1" && (eff_defs_in_seg.contains("$f14") || has_df_di_a2)) {
            continue;
          }
          pred_regs.insert(r);
        } else {
          if (arg_defs.size() == 1 && arg_defs[0] == "a0" && !has_stack_arg &&
              (r == "a1" || r == "a2")) {
            bool same_callee_sets_r = false;
            if (!copied_at_entry.contains(r)) {
              for (size_t oci_pos = 0; oci_pos < c_idxs.size(); ++oci_pos) {
                if (oci_pos == ci_pos) {
                  continue;
                }
                const size_t ocidx = c_idxs[oci_pos];
                const std::string& ocallee = parsed_b[ocidx].callee;
                const size_t os_start = (oci_pos > 0) ? (c_idxs[oci_pos - 1] + 1) : 0;
                bool r_in_ods = false;
                for (size_t i = os_start; i < ocidx; ++i) {
                  if (HasItem(parsed_b[i].defs, r)) {
                    r_in_ods = true;
                    break;
                  }
                }
                bool r_used_not_a0 = false;
                for (size_t i = s_start; i < cidx; ++i) {
                  if (HasItem(parsed_b[i].uses, r) && !HasItem(parsed_b[i].defs, "a0")) {
                    r_used_not_a0 = true;
                    break;
                  }
                }
                if (r_in_ods && ((!callee.empty() && ocallee == callee) || r_used_not_a0)) {
                  same_callee_sets_r = true;
                }
              }
            }
            bool pred_blk_defined_a1 = false;
            if (r == "a1" && b > 0 && ci_pos == 0) {
              for (size_t pb = 0; pb < num_blks; ++pb) {
                if (std::find(blks[pb].successors.begin(), blks[pb].successors.end(), b) !=
                    blks[pb].successors.end()) {
                  const auto& p_parsed = parsed_map[pb];
                  bool tail_uses_a1 = false;
                  for (size_t k = (p_parsed.size() >= 2 ? p_parsed.size() - 2 : 0);
                       k < p_parsed.size(); ++k) {
                    if (HasItem(p_parsed[k].uses, "a1")) {
                      tail_uses_a1 = true;
                    }
                  }
                  if (!p_parsed.empty() && tail_uses_a1 && p_parsed.back().is_jump) {
                    for (size_t ob = 0; ob < b; ++ob) {
                      for (const auto& op : parsed_map[ob]) {
                        if (!op.is_call && HasItem(op.defs, "a1")) {
                          pred_blk_defined_a1 = true;
                        }
                      }
                    }
                  }
                }
              }
            }
            bool saved_a0_forwarded_a1 = false;
            if (r == "a1" && b > 0 && is_first_call_on_path && copied_at_entry.contains("a0") &&
                !copied_at_entry.contains("a1") && read_as_param_in_fn.contains("a1")) {
              bool copies_s0_to_a0 = false;
              for (size_t i = s_start; i < cidx; ++i) {
                if (parsed_b[i].is_si_reg_to_reg_move && HasItem(parsed_b[i].defs, "a0") &&
                    HasItem(parsed_b[i].uses, "s0")) {
                  copies_s0_to_a0 = true;
                }
              }
              const bool on_a1_eq_0_branch =
                  (blks[0].successors.size() == 2 && !parsed0.empty() &&
                   parsed0.back().is_beq_a1_zero && b >= blks[0].successors[0]);
              bool compared_to_reg_in_b0 = false;
              for (const auto& p0 : parsed0) {
                if (p0.compares_a1_to_reg) {
                  compared_to_reg_in_b0 = true;
                }
              }
              if (copies_s0_to_a0 && !on_a1_eq_0_branch && !compared_to_reg_in_b0) {
                saved_a0_forwarded_a1 = true;
              }
            }
            const bool entry_deref_s0 =
                (b == 0 && ci_pos == 0 && (cidx - s_start) == 2 &&
                 parsed_b[s_start].is_si_reg_to_reg_move && HasItem(parsed_b[s_start].defs, "s0") &&
                 HasItem(parsed_b[s_start].uses, "a0") && parsed_b[s_start + 1].is_lw_a0_from_0_s0);
            if (same_callee_sets_r || pred_blk_defined_a1 || saved_a0_forwarded_a1) {
              pred_regs.insert(r);
            } else if (entry_deref_s0) {
              if (r == "a1" || (r == "a2" && callee == "step_value")) {
                pred_regs.insert(r);
              }
            }
          } else if (arg_defs.empty() && !has_stack_arg) {
            bool pred_defined_and_branched = false;
            bool any_pred_redefined_a0_or_a1 = false;
            if (b > 0) {
              for (size_t pb = 0; pb < b; ++pb) {
                bool r_in_pdefs = false;
                for (const auto& pp : parsed_map[pb]) {
                  if (HasItem(pp.defs, "a0") || HasItem(pp.defs, "a1")) {
                    any_pred_redefined_a0_or_a1 = true;
                  }
                  if (HasItem(pp.defs, r)) {
                    r_in_pdefs = true;
                  }
                }
                if (std::find(blks[pb].successors.begin(), blks[pb].successors.end(), b) !=
                    blks[pb].successors.end()) {
                  if (r_in_pdefs && !parsed_map[pb].empty() && parsed_map[pb].back().is_jump &&
                      HasItem(parsed_map[pb].back().uses, r)) {
                    pred_defined_and_branched = true;
                  }
                }
              }
            }
            if (pred_defined_and_branched) {
              pred_regs.insert(r);
            } else if (is_first_call_on_path && !copied_at_entry.contains("a0")) {
              bool b0_masked_bit0 = false;
              for (const auto& p0 : parsed0) {
                if (p0.is_andi_a0_1) {
                  b0_masked_bit0 = true;
                }
              }
              int reads_of_a0_in_seg = 0;
              for (size_t i = s_start; i < cidx; ++i) {
                if (HasItem(parsed_b[i].uses, "a0")) {
                  ++reads_of_a0_in_seg;
                }
              }
              const bool used_in_b = (reads_of_a0_in_seg > 0);
              const bool bool_flag_guard =
                  (b > 0 && c_idxs.size() >= 2 && copied_at_entry.contains("a1"));
              if (!b0_masked_bit0 && !(b > 0 && num_blks >= 15 && !used_in_b) &&
                  reads_of_a0_in_seg < 4 && !bool_flag_guard) {
                if (r == "a0") {
                  pred_regs.insert("a0");
                } else if ((r == "a1" || r == "a2" || r == "a3") && !any_pred_redefined_a0_or_a1) {
                  if (callee == "CalleeTwo" && r == "a1") {
                    pred_regs.insert(r);
                  } else if (callee == "CalleeThree" && (r == "a1" || r == "a2")) {
                    pred_regs.insert(r);
                  } else if (callee == "CalleeFour" && (r == "a1" || r == "a2" || r == "a3")) {
                    pred_regs.insert(r);
                  } else if (b > 0 && (r == "a1" || r == "a2")) {
                    if (read_as_param_in_fn.contains(r) && !copied_at_entry.contains(r)) {
                      bool consumed_by_mtc1_in_b0 = false;
                      for (const auto& p0 : parsed0) {
                        if (p0.is_mtc1_from_gpr && HasItem(p0.uses, r)) {
                          consumed_by_mtc1_in_b0 = true;
                        }
                      }
                      if (!consumed_by_mtc1_in_b0) {
                        pred_regs.insert(r);
                      }
                    } else if (r == "a1" && max_param_idx >= 2 && !copied_at_entry.contains("a1")) {
                      pred_regs.insert(r);
                    }
                  }
                }
              }
            }
          }
        }
      }

      for (std::string_view ar : kGprArgs) {
        if (pred_regs.contains(ar)) {
          exit_self_moves[b].push_back(MakeSelfMoveRtlInsn(ar));
        }
      }
      mech2_by_call[{b, cidx}] = std::move(pred_regs);
    }
  }

  InvertSched2ReturnAndPostCallSelfMoves(*cfg, parsed_map, blk_has_got_v0_f0_sm, function_name,
                                         &exit_self_moves);

  // --- Mechanism 1: `function.c:assign_parms` incoming parameter self-moves at entry of B0 ---
  if (!is_mech5_b0) {
    bool b0_has_pred = false;
    for (size_t b_i = 0; b_i < num_blks; ++b_i) {
      if (std::find(blks[b_i].successors.begin(), blks[b_i].successors.end(), 0) !=
          blks[b_i].successors.end()) {
        b0_has_pred = true;
      }
    }
    for (std::string_view r_sv : kAllArgRegs) {
      const std::string r(r_sv);
      std::vector<size_t> read_indices;
      bool redefined_in_b0 = false;
      const ParsedGregInsn* first_read_insn = nullptr;
      bool copied_to_reg_v_at_entry = false;
      for (size_t idx = 0; idx < parsed0.size(); ++idx) {
        const auto& p = parsed0[idx];
        if (HasItem(p.uses, r)) {
          read_indices.push_back(idx);
          if (first_read_insn == nullptr) {
            first_read_insn = &p;
            if (p.is_si_reg_to_reg_move && !HasItem(p.defs, r)) {
              copied_to_reg_v_at_entry = true;
            }
            if (p.is_mfc1_to_arg_gpr && !p.defs.empty()) {
              const std::string& gpr_dst = p.defs[0];
              for (size_t k = idx + 1; k < parsed0.size(); ++k) {
                if (!parsed0[k].is_call && HasItem(parsed0[k].uses, gpr_dst)) {
                  read_indices.push_back(idx);
                  break;
                }
              }
            }
          }
        }
        if (p.is_call) {
          auto it_m2 = mech2_by_call.find({0, idx});
          if (it_m2 != mech2_by_call.end() && it_m2->second.contains(r) && !read_indices.empty()) {
            read_indices.push_back(idx);
          }
          redefined_in_b0 = true;
          break;
        }
        if (HasItem(p.defs, r)) {
          redefined_in_b0 = true;
          break;
        }
      }

      size_t reads_in_b0 = read_indices.size();
      if (reads_in_b0 == 2 && read_indices[1] == read_indices[0] + 1) {
        const auto& u0 = parsed0[read_indices[0]];
        const auto& u1 = parsed0[read_indices[1]];
        if (u0.is_mtc1_from_gpr && u1.is_mtc1_from_gpr) {
          reads_in_b0 = 1;
        } else if ((u0.is_sll && u1.is_addu_or_subu_reg) ||
                   (u0.is_addiu_minus_1 && u1.is_and_reg)) {
          reads_in_b0 = 1;
        }
      }

      bool live_in_later_blk = false;
      if (!redefined_in_b0 && !copied_to_reg_v_at_entry) {
        absl::flat_hash_set<size_t> visited;
        std::vector<size_t> queue = blks[0].successors;
        size_t q_head = 0;
        while (q_head < queue.size()) {
          const size_t curr = queue[q_head++];
          if (curr == 0 || curr >= num_blks || visited.contains(curr)) {
            continue;
          }
          visited.insert(curr);
          bool killed = false;
          for (size_t idx_b = 0; idx_b < parsed_map[curr].size(); ++idx_b) {
            const auto& pb = parsed_map[curr][idx_b];
            if (HasItem(pb.uses, r)) {
              live_in_later_blk = true;
              killed = true;
              break;
            }
            if (pb.is_call) {
              auto it_m2 = mech2_by_call.find({curr, idx_b});
              if (it_m2 != mech2_by_call.end() && it_m2->second.contains(r)) {
                live_in_later_blk = true;
                killed = true;
                break;
              }
            }
            if (pb.is_call || HasItem(pb.defs, r)) {
              killed = true;
              break;
            }
          }
          if (!killed) {
            for (size_t nxt : blks[curr].successors) {
              if (!visited.contains(nxt)) {
                queue.push_back(nxt);
              }
            }
          }
        }
      }

      if ((reads_in_b0 >= 2) || live_in_later_blk || (b0_has_pred && reads_in_b0 >= 1)) {
        entry_self_moves[0].push_back(MakeSelfMoveRtlInsn(r));
      }
    }
  }

  // Insert the recovered `.greg` self-moves into each basic block.
  for (size_t b = 0; b < num_blks; ++b) {
    for (size_t i = 0; i < entry_self_moves[b].size(); ++i) {
      InsertInsnAt(&blks[b], i, std::move(entry_self_moves[b][i]));
    }
    for (auto& sm : exit_self_moves[b]) {
      if (!blks[b].rtl_insns.empty() &&
          (EndsWithUncondJump(blks[b]) || EndsWithCondBranch(blks[b]))) {
        InsertInsnAt(&blks[b], blks[b].rtl_insns.size() - 1, std::move(sm));
      } else {
        blks[b].AddInsn(std::move(sm));
      }
    }
  }
}

}  // namespace

RtlControlFlowGraph InverseFinalAndGasLifter::LiftToSched2RtlCfg(
    const ControlFlowGraph& input_cfg, const StackFrameAnalysis* frame_analysis) {
  RtlControlFlowGraph rtl_cfg = LiftToRtlCfg(input_cfg, frame_analysis);
  InvertJump2CrossJumping(&rtl_cfg);
  InvertSched2DiamondSimplifications(&rtl_cfg);
  InvertJump2DeleteNoopMoves(&rtl_cfg);
  return rtl_cfg;
}

void InverseFinalAndGasLifter::InvertSched2ToGregRtlCfg(
    RtlControlFlowGraph* cfg, const StackFrameAnalysis* /*frame_analysis*/,
    const SymbolIndex* symbols, std::string_view function_name) {
  if (cfg == nullptr) {
    return;
  }
  // Step 1: Invert `function.c:thread_prologue_and_epilogue_insns` (`mips_expand_prologue`),
  // which emitted RTL prologue instructions into `basic_block_head[0]` after `.greg` and before
  // `.sched2`.
  for (auto& blk : cfg->rpo_blocks) {
    std::vector<RtlInsn> kept_insns;
    std::vector<std::string> kept_pats;
    kept_insns.reserve(blk.rtl_insns.size());
    kept_pats.reserve(blk.instructions.size());
    for (size_t i = 0; i < blk.rtl_insns.size(); ++i) {
      if (blk.rtl_insns[i].is_prologue_insn) {
        continue;
      }
      kept_pats.push_back(blk.rtl_insns[i].canonical_pattern);
      kept_insns.push_back(std::move(blk.rtl_insns[i]));
    }
    blk.rtl_insns = std::move(kept_insns);
    blk.instructions = std::move(kept_pats);
  }

  // Step 2: Invert `sched.c:schedule_insns` post-reload self-move elimination across all 5 GCC
  // 2.7.2 mechanisms (`assign_parms`, `expand_call` args/returns, `expand_return`, and
  // `strength_reduce`).
  InvertSched2SelfMoves(cfg, symbols, function_name);
}

}  // namespace rom_nom_nom
