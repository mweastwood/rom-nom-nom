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

}  // namespace rom_nom_nom
