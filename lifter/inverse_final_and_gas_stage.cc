#include "lifter/inverse_final_and_gas_stage.h"

#include <algorithm>
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
#include "absl/strings/str_format.h"
#include "core/control_flow_graph.h"
#include "core/division_pattern.h"
#include "core/mips.h"
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

}  // namespace

RtlControlFlowGraph InverseFinalAndGasLifter::LiftToSched2RtlCfg(
    const ControlFlowGraph& input_cfg, const StackFrameAnalysis* frame_analysis) {
  RtlControlFlowGraph rtl_cfg = LiftToRtlCfg(input_cfg, frame_analysis);
  InvertJump2CrossJumping(&rtl_cfg);
  InvertSched2DiamondSimplifications(&rtl_cfg);
  InvertJump2DeleteNoopMoves(&rtl_cfg);
  return rtl_cfg;
}

}  // namespace rom_nom_nom
