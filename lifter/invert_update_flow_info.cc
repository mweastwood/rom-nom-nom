#include "lifter/invert_update_flow_info.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/btree_set.h"
#include "absl/status/statusor.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "lifter/reconstruct_hard_reg_liveness.h"
#include "lifter/rtl_liveness.h"

namespace rom_nom_nom::lifter {
namespace {

constexpr int kMipsZeroRegno = 0;
constexpr int kMipsLastCallerSavedLowGpr = 15;
constexpr int kMipsFirstCallerSavedHighGpr = 24;
constexpr int kMipsSpRegno = 29;
constexpr int kMipsFpRegno = 30;
constexpr int kMipsRaRegno = 31;
constexpr int kMipsFirstFpr = 32;
constexpr int kMipsLastCallerSavedFpr = 51;
constexpr int kMipsLastFpr = 63;
constexpr int kMipsHiRegno = 64;

bool IsCallUsedMipsHardReg(int regno) {
  // Exact `CALL_USED_REGISTERS` from `config/mips/mips.h:1203-1210`.
  if (regno < 0 || regno >= kMipsFirstPseudoRegister) {
    return false;
  }
  return (regno <= kMipsLastCallerSavedLowGpr) ||
         (regno >= kMipsFirstCallerSavedHighGpr && regno <= kMipsSpRegno) ||
         (regno == kMipsRaRegno) || (regno >= kMipsFirstFpr && regno <= kMipsLastCallerSavedFpr) ||
         (regno >= kMipsHiRegno && regno <= kMipsStatusRegnum);
}

bool IsMipsLargeInt(int64_t val) {
  // Mirrors `config/mips/mips.c:416` (`large_int`): does not fit in signed
  // 16-bit, unsigned 16-bit, or a bare upper 16-bit `lui` constant.
  const uint32_t u32 = static_cast<uint32_t>(val);
  const bool fits_signed16 = (val >= -32768 && val <= 32767);
  const bool fits_unsigned16 = (val >= 0 && val <= 65535);
  const bool fits_lui = ((u32 & 0x0000ffffu) == 0u);
  return !fits_signed16 && !fits_unsigned16 && !fits_lui;
}

bool ContainsUnknownOrMalformedOp(const RtlExpr& expr, size_t num_blocks,
                                  bool allow_return = false) {
  if (expr.op == RtlOp::kUnknown) {
    return true;
  }
  if (expr.op == RtlOp::kReg && expr.RegNo() < 0) {
    return true;
  }
  if (expr.op == RtlOp::kReturn && !allow_return) {
    return true;
  }
  if (expr.op == RtlOp::kSequence) {
    if (expr.children.size() != 2 || expr.children[0].op != RtlOp::kJumpInsn ||
        expr.children[1].op != RtlOp::kInsn || expr.children[1].children.empty()) {
      return true;
    }
  }
  if (expr.op == RtlOp::kAddrVec || expr.op == RtlOp::kAddrDiffVec) {
    for (const RtlExpr& child : expr.children) {
      if (child.op == RtlOp::kLabelRef && !child.str_val.empty() && child.str_val[0] == 'L' &&
          child.str_val.size() > 1) {
        size_t idx = 0;
        bool all_digits = true;
        for (size_t k = 1; k < child.str_val.size(); ++k) {
          const char c = child.str_val[k];
          if (c < '0' || c > '9') {
            all_digits = false;
            break;
          }
          idx = idx * 10 + static_cast<size_t>(c - '0');
        }
        if (all_digits && idx >= num_blocks) {
          return true;
        }
      }
    }
  }
  const bool child_allow_return = (expr.op == RtlOp::kParallel);
  for (const RtlExpr& child : expr.children) {
    if (ContainsUnknownOrMalformedOp(child, num_blocks, child_allow_return)) {
      return true;
    }
  }
  return false;
}

bool ValidateCfgForUpdateFlowInfo(const RtlControlFlowGraph& cfg) {
  if (cfg.rpo_blocks.empty()) {
    return false;
  }
  const size_t n_blocks = cfg.rpo_blocks.size();
  for (size_t b = 0; b < n_blocks; ++b) {
    const RtlBasicBlock& blk = cfg.rpo_blocks[b];
    if (blk.id != static_cast<uint32_t>(b)) {
      return false;
    }
    for (size_t succ : blk.successors) {
      if (succ >= n_blocks) {
        return false;
      }
    }
    for (size_t target : blk.addr_vec_targets) {
      if (target >= n_blocks) {
        return false;
      }
    }
    for (const std::string& pat : blk.instructions) {
      if (pat.empty()) {
        return false;
      }
      auto parsed_or = ParsePatternAndNotes(pat);
      if (!parsed_or.ok()) {
        return false;
      }
      if (ContainsUnknownOrMalformedOp(parsed_or->pattern, n_blocks)) {
        return false;
      }
      for (const RtlRegNote& note : parsed_or->reg_notes) {
        if (ContainsUnknownOrMalformedOp(note.datum, n_blocks)) {
          return false;
        }
      }
      for (const RtlExpr& fu : parsed_or->call_fusage) {
        if (ContainsUnknownOrMalformedOp(fu, n_blocks)) {
          return false;
        }
      }
    }
    for (const RtlInsn& insn : blk.rtl_insns) {
      for (const RtlExpr& fu : insn.call_fusage) {
        if (ContainsUnknownOrMalformedOp(fu, n_blocks)) {
          return false;
        }
      }
    }
  }
  for (const auto& [regno, hr] : cfg.sideband.reg_renumber) {
    if (regno < kMipsFirstPseudoRegister || hr < -1 || hr >= kMipsFirstPseudoRegister) {
      return false;
    }
  }
  for (const auto& [regno, _] : cfg.sideband.reg_n_refs) {
    if (regno < 0) return false;
  }
  for (const auto& [regno, _] : cfg.sideband.reg_n_sets) {
    if (regno < 0) return false;
  }
  for (const auto& [regno, _] : cfg.sideband.reg_n_deaths) {
    if (regno < 0) return false;
  }
  for (const auto& [regno, _] : cfg.sideband.reg_live_length) {
    if (regno < 0) return false;
  }
  for (const auto& [regno, _] : cfg.sideband.reg_n_calls_crossed) {
    if (regno < 0) return false;
  }
  for (const auto& [b_idx, live_set] : cfg.sideband.basic_block_live_at_start) {
    if (b_idx < 0 || static_cast<size_t>(b_idx) >= n_blocks) {
      return false;
    }
    for (int r : live_set) {
      if (r < 0) {
        return false;
      }
    }
  }
  for (int r : cfg.sideband.pointer_regnos) {
    if (r < 0) {
      return false;
    }
  }
  for (int r : cfg.sideband.saved_gpr_regnos) {
    if (r < 0 || r >= kMipsFirstPseudoRegister) {
      return false;
    }
  }
  for (int r : cfg.sideband.saved_fpr_regnos) {
    if (r < 0 || r >= kMipsFirstPseudoRegister) {
      return false;
    }
  }
  return true;
}

// Extracts the underlying register number from an `SImode` `register_operand`
// (`(reg:SI R)` or `(subreg:SI (reg ...) W)`, per `mips.md:3209`).
std::optional<int> ExtractSiRegisterOperandRegno(const RtlExpr& expr) {
  if (expr.mode != RtlMachineMode::kSi) {
    return std::nullopt;
  }
  if (expr.op == RtlOp::kReg) {
    return expr.RegNo();
  }
  if (expr.op == RtlOp::kSubreg && !expr.children.empty() && expr.children[0].op == RtlOp::kReg) {
    return expr.children[0].RegNo();
  }
  return std::nullopt;
}

bool SameSiRegisterOperand(const RtlExpr& a, const RtlExpr& b) {
  if (a.op != b.op || a.mode != RtlMachineMode::kSi || b.mode != RtlMachineMode::kSi) {
    return false;
  }
  if (a.op == RtlOp::kReg) {
    return a.RegNo() == b.RegNo();
  }
  if (a.op == RtlOp::kSubreg && !a.children.empty() && !b.children.empty() &&
      a.children[0].op == RtlOp::kReg && b.children[0].op == RtlOp::kReg) {
    return a.int_val == b.int_val && a.children[0].RegNo() == b.children[0].RegNo() &&
           a.children[0].mode == b.children[0].mode;
  }
  return false;
}

bool MatchSplitLargeIntPair(const RtlPatternAndNotes& first, const RtlPatternAndNotes& second,
                            RtlExpr* out_dest_op, int* out_dest_regno, int64_t* out_combined_val) {
  // Match `first`: `(set OP0 (const_int HI))` (`config/mips/mips.md:3208-3221`).
  if (first.pattern.op != RtlOp::kSet || first.pattern.children.size() != 2) {
    return false;
  }
  const RtlExpr& dst1 = first.pattern.children[0];
  const RtlExpr& src1 = first.pattern.children[1];
  const std::optional<int> dst1_regno = ExtractSiRegisterOperandRegno(dst1);
  if (!dst1_regno.has_value() || src1.op != RtlOp::kConstInt) {
    return false;
  }

  // Match `second`: `(set OP0 (ior:SI OP0 (const_int LO)))`.
  if (second.pattern.op != RtlOp::kSet || second.pattern.children.size() != 2) {
    return false;
  }
  const RtlExpr& dst2 = second.pattern.children[0];
  const RtlExpr& src2 = second.pattern.children[1];
  if (!SameSiRegisterOperand(dst1, dst2) || src2.op != RtlOp::kIor ||
      src2.mode != RtlMachineMode::kSi || src2.children.size() != 2) {
    return false;
  }
  if (!SameSiRegisterOperand(dst1, src2.children[0]) || src2.children[1].op != RtlOp::kConstInt) {
    return false;
  }

  const uint32_t hi_u32 = static_cast<uint32_t>(src1.int_val);
  const uint32_t lo_u32 = static_cast<uint32_t>(src2.children[1].int_val);
  if ((hi_u32 & 0x0000ffffu) != 0u || (lo_u32 & 0xffff0000u) != 0u) {
    return false;
  }
  const int64_t combined = static_cast<int32_t>(hi_u32 | lo_u32);
  if (!IsMipsLargeInt(combined)) {
    return false;
  }

  *out_dest_op = dst2;
  *out_dest_regno = *dst1_regno;
  *out_combined_val = combined;
  return true;
}

}  // namespace

RtlControlFlowGraph InvertUpdateFlowInfo(const RtlControlFlowGraph& scheduled_cfg) {
  if (!ValidateCfgForUpdateFlowInfo(scheduled_cfg)) {
    return {};
  }

  RtlControlFlowGraph out = scheduled_cfg;
  for (RtlBasicBlock& blk : out.rpo_blocks) {
    const size_t n = blk.instructions.size();
    if (n < 2) {
      continue;
    }

    std::vector<RtlPatternAndNotes> parsed(n);
    std::vector<bool> ok(n, false);
    for (size_t i = 0; i < n; ++i) {
      auto p = ParsePatternAndNotes(blk.instructions[i]);
      if (p.ok()) {
        if (i < blk.rtl_insns.size() && blk.rtl_insns[i].has_explicit_call_fusage &&
            !p->has_explicit_call_fusage) {
          p->has_explicit_call_fusage = true;
          p->call_fusage = blk.rtl_insns[i].call_fusage;
        }
        parsed[i] = std::move(*p);
        ok[i] = true;
      }
    }

    auto is_scheduling_barrier = [&](size_t idx) -> bool {
      const std::optional<RtlLoopNoteKind> loop_kind =
          (idx < blk.rtl_insns.size() && blk.rtl_insns[idx].loop_note_kind.has_value())
              ? blk.rtl_insns[idx].loop_note_kind
              : ParseRtlLoopNoteKind(blk.instructions[idx]);
      if (loop_kind == RtlLoopNoteKind::kLoopBeg || loop_kind == RtlLoopNoteKind::kLoopEnd) {
        return true;
      }
      if (!ok[idx]) {
        return true;
      }
      bool barrier = false;
      parsed[idx].pattern.Visit([&](const RtlExpr& e) {
        if (e.op == RtlOp::kAsmInput || e.op == RtlOp::kUnspecVolatile ||
            (e.op == RtlOp::kAsmOperands && e.flags.volatil) ||
            (e.op == RtlOp::kUnknown && e.raw_op == "trap_if")) {
          barrier = true;
        }
      });
      return barrier;
    };

    auto insn_mentions_or_clobbers_reg = [&](size_t idx, int regno) -> bool {
      if (is_scheduling_barrier(idx)) {
        return true;
      }
      bool hit = false;
      auto check_expr = [&](const RtlExpr& e) {
        if (e.IsReg() && e.RegNo() == regno) {
          hit = true;
        }
      };
      parsed[idx].pattern.Visit(check_expr);
      for (const RtlRegNote& note : parsed[idx].reg_notes) {
        note.datum.Visit(check_expr);
      }
      for (const RtlExpr& fu : parsed[idx].call_fusage) {
        fu.Visit(check_expr);
      }
      if (!hit && regno < kMipsFirstPseudoRegister && IsCallPattern(parsed[idx].pattern) &&
          IsCallUsedMipsHardReg(regno)) {
        hit = true;
      }
      return hit;
    };

    std::vector<bool> removed_first(n, false);
    bool modified_block = false;
    for (size_t i = 0; i < n; ++i) {
      if (removed_first[i] || !ok[i]) {
        continue;
      }
      if (parsed[i].pattern.op != RtlOp::kSet || parsed[i].pattern.children.size() != 2 ||
          parsed[i].pattern.SetSrc().op != RtlOp::kConstInt) {
        continue;
      }
      const std::optional<int> target_regno =
          ExtractSiRegisterOperandRegno(parsed[i].pattern.SetDest());
      if (!target_regno.has_value()) {
        continue;
      }
      for (size_t j = i + 1; j < n; ++j) {
        if (removed_first[j] || !ok[j]) {
          break;
        }
        RtlExpr dest_op;
        int dest_regno = -1;
        int64_t combined_val = 0;
        if (MatchSplitLargeIntPair(parsed[i], parsed[j], &dest_op, &dest_regno, &combined_val)) {
          RtlPatternAndNotes merged;
          merged.bracketed_notes = parsed[j].bracketed_notes;
          merged.pattern = RtlExpr::MakeSet(dest_op, RtlExpr::MakeConstInt(combined_val));
          for (const RtlRegNote& note : parsed[i].reg_notes) {
            if (note.kind == RtlMachineMode::kRegDead && note.datum.op == RtlOp::kReg &&
                note.datum.RegNo() == dest_regno) {
              continue;
            }
            merged.reg_notes.push_back(note);
          }
          for (const RtlRegNote& note : parsed[j].reg_notes) {
            merged.reg_notes.push_back(note);
          }
          parsed[j] = std::move(merged);
          removed_first[i] = true;
          auto sets_it = out.sideband.reg_n_sets.find(dest_regno);
          if (sets_it != out.sideband.reg_n_sets.end() && sets_it->second > 1) {
            sets_it->second -= 1;
          }
          modified_block = true;
          break;
        }
        if (insn_mentions_or_clobbers_reg(j, *target_regno)) {
          break;
        }
      }
    }

    if (modified_block) {
      std::vector<std::string> new_insns;
      std::vector<std::optional<std::vector<RtlExpr>>> kept_fusage;
      new_insns.reserve(n);
      kept_fusage.reserve(n);
      for (size_t i = 0; i < n; ++i) {
        if (removed_first[i]) {
          continue;
        }
        if (ok[i]) {
          new_insns.push_back(FormatGccPrintRtlPatternAndNotes(
              parsed[i].pattern, parsed[i].reg_notes, parsed[i].bracketed_notes,
              /*call_fusage=*/{}));
          if (parsed[i].has_explicit_call_fusage) {
            kept_fusage.push_back(parsed[i].call_fusage);
          } else {
            kept_fusage.push_back(std::nullopt);
          }
        } else {
          new_insns.push_back(blk.instructions[i]);
          kept_fusage.push_back(std::nullopt);
        }
      }
      blk.instructions = std::move(new_insns);
      blk.SyncRtlInsnsFromInstructions();
      for (size_t k = 0; k < blk.rtl_insns.size() && k < kept_fusage.size(); ++k) {
        if (kept_fusage[k].has_value() && !blk.rtl_insns[k].has_explicit_call_fusage) {
          blk.rtl_insns[k].has_explicit_call_fusage = true;
          blk.rtl_insns[k].call_fusage = std::move(*kept_fusage[k]);
        }
      }
    }
  }

  return out;
}

namespace {

bool IsFixedForSched1Deaths(int regno) {
  // Mirrors `sched.c:2842-2854` (`attach_deaths`): fixed hard registers (`$0`/argp,
  // `$sp`, `$fp`) never receive `REG_DEAD` notes in pre-reload `sched1`.
  return regno == kMipsZeroRegno || regno == kMipsSpRegno || regno == kMipsFpRegno;
}

int Sched1RegSpan(int regno, RtlMachineMode mode, bool gp64, bool fp64) {
  if (regno >= 0 && regno < kMipsFirstPseudoRegister) {
    return std::max(1, MipsHardRegNregs(regno, mode, gp64, fp64));
  }
  return 1;
}

int ModeByteSizeForSched1(RtlMachineMode mode, bool gp64) {
  switch (mode) {
    case RtlMachineMode::kQi:
      return 1;
    case RtlMachineMode::kHi:
      return 2;
    case RtlMachineMode::kSi:
    case RtlMachineMode::kSf:
      return 4;
    case RtlMachineMode::kDi:
    case RtlMachineMode::kDf:
      return 8;
    case RtlMachineMode::kTi:
      return 16;
    default:
      return gp64 ? 8 : 4;
  }
}

RtlMachineMode SingleWordModeForHardReg(int regno, bool gp64, bool fp64) {
  if (regno >= kMipsFirstFpr && regno <= kMipsLastFpr) {
    return fp64 ? RtlMachineMode::kDf : RtlMachineMode::kSf;
  }
  return gp64 ? RtlMachineMode::kDi : RtlMachineMode::kSi;
}

struct Sched1InsnRegSummary {
  // Ordered `(regno, mode, flags)` read occurrences (matching `attach_deaths` traversal).
  std::vector<RtlExpr> read_regs;
  absl::btree_set<int> read_words;
  absl::btree_set<int> full_def_words;
  absl::btree_set<int> partial_def_words;
  absl::btree_set<int> unused_words;
  bool is_call = false;
  bool is_note_or_pseudo = false;
};

Sched1InsnRegSummary SummarizeInsnForSched1Deaths(const RtlPatternAndNotes& pn, bool gp64,
                                                  bool fp64) {
  Sched1InsnRegSummary out;
  const RtlExpr& pat = pn.pattern;
  if (pat.op == RtlOp::kNote || pat.op == RtlOp::kAddrVec || pat.op == RtlOp::kAddrDiffVec) {
    out.is_note_or_pseudo = true;
    return out;
  }
  out.is_call = IsCallPattern(pat);

  auto add_read = [&](const RtlExpr& reg_expr) {
    const int r = reg_expr.RegNo();
    if (r < 0) {
      return;
    }
    const int span = Sched1RegSpan(r, reg_expr.mode, gp64, fp64);
    for (int k = 0; k < span; ++k) {
      out.read_words.insert(r + k);
    }
    out.read_regs.push_back(reg_expr);
  };

  auto add_full_def = [&](const RtlExpr& dst_expr) {
    // Mirrors `sched.c:2320-2366` (`sched_note_set`): full-register `SET`/`CLOBBER`
    // destinations make the reg live on the forward pass (`death == 0`) AND kill it
    // on the backward pass (`death == 1`); partial `SUBREG`/`STRICT_LOW_PART`/`*_EXTRACT`
    // destinations still mark the inner register live on the forward pass (`sched.c:3543`)
    // while suppressing killing on the backward pass (`sched.c:3783`).
    const RtlExpr* cur = &dst_expr;
    bool partial = false;
    while (cur->IsSubreg() || cur->op == RtlOp::kStrictLowPart || cur->op == RtlOp::kZeroExtract ||
           cur->op == RtlOp::kSignExtract) {
      if (!cur->IsSubreg()) {
        partial = true;
      } else if (!cur->children.empty()) {
        const int inner_bytes = ModeByteSizeForSched1(cur->children[0].mode, gp64);
        const int outer_bytes = ModeByteSizeForSched1(cur->mode, gp64);
        const int word_bytes = gp64 ? 8 : 4;
        const int inner_words = (inner_bytes + word_bytes - 1) / word_bytes;
        const int outer_words = (outer_bytes + word_bytes - 1) / word_bytes;
        if (inner_words > outer_words) {
          partial = true;
        }
      }
      if (cur->children.empty()) {
        return;
      }
      cur = &cur->children[0];
    }
    if (cur->IsReg() && cur->RegNo() >= 0) {
      const int r = cur->RegNo();
      const int span = Sched1RegSpan(r, cur->mode, gp64, fp64);
      for (int k = 0; k < span; ++k) {
        if (partial) {
          out.partial_def_words.insert(r + k);
        } else {
          out.full_def_words.insert(r + k);
        }
      }
    }
  };

  auto walk_reads = [&](const auto& self, const RtlExpr& x, bool set_p) -> void {
    switch (x.op) {
      case RtlOp::kConstInt:
      case RtlOp::kConstDouble:
      case RtlOp::kConst:
      case RtlOp::kLabelRef:
      case RtlOp::kSymbolRef:
      case RtlOp::kPc:
      case RtlOp::kCc0:
      case RtlOp::kAddrVec:
      case RtlOp::kAddrDiffVec:
      case RtlOp::kAsmInput:
      case RtlOp::kNote:
      case RtlOp::kScratch:
        return;
      case RtlOp::kReg:
        if (!set_p) {
          add_read(x);
        }
        return;
      case RtlOp::kMem:
        if (!x.children.empty()) {
          self(self, x.children[0], false);
        }
        return;
      case RtlOp::kSubreg:
      case RtlOp::kStrictLowPart:
        if (!x.children.empty()) {
          self(self, x.children[0], set_p);
        }
        return;
      case RtlOp::kZeroExtract:
      case RtlOp::kSignExtract:
        if (!x.children.empty()) {
          self(self, x.children[0], set_p);
        }
        for (size_t k = 1; k < x.children.size(); ++k) {
          self(self, x.children[k], false);
        }
        return;
      case RtlOp::kSet:
        if (x.children.size() >= 2) {
          self(self, x.SetSrc(), false);
          self(self, x.SetDest(), true);
        }
        return;
      case RtlOp::kClobber:
        if (!x.children.empty() && x.children[0].IsMem() && !x.children[0].children.empty()) {
          self(self, x.children[0].children[0], false);
        }
        return;
      default:
        for (const RtlExpr& ch : x.children) {
          self(self, ch, false);
        }
        return;
    }
  };

  if (pat.IsParallel()) {
    for (const RtlExpr& elem : pat.children) {
      if ((elem.op == RtlOp::kSet || elem.op == RtlOp::kClobber) && !elem.children.empty()) {
        add_full_def(elem.children[0]);
      }
      walk_reads(walk_reads, elem, false);
    }
  } else {
    if ((pat.op == RtlOp::kSet || pat.op == RtlOp::kClobber) && !pat.children.empty()) {
      add_full_def(pat.children[0]);
    }
    walk_reads(walk_reads, pat, false);
  }
  for (const RtlExpr& fu : pn.call_fusage) {
    if (fu.op == RtlOp::kUse && !fu.children.empty()) {
      walk_reads(walk_reads, fu.children[0], false);
    }
  }
  for (const RtlRegNote& note : pn.reg_notes) {
    if (note.kind == RtlMachineMode::kRegUnused && note.datum.IsReg() && note.datum.RegNo() >= 0) {
      const int r = note.datum.RegNo();
      const int span = Sched1RegSpan(r, note.datum.mode, gp64, fp64);
      for (int k = 0; k < span; ++k) {
        out.unused_words.insert(r + k);
      }
    }
  }
  return out;
}

}  // namespace

void RestorePreSched1FlowLiveness(RtlControlFlowGraph* cfg, const CompilationProfile& profile) {
  if (cfg == nullptr || cfg->rpo_blocks.empty()) {
    return;
  }
  bool gp64 = false;
  bool fp64 = false;
  DetectMipsRegisterWidthFlags(*cfg, &profile, &gp64, &fp64);

  // Compute exact `flow.c` `basic_block_live_at_start` on a scratch copy without mutating
  // `cfg`'s `REG_UNUSED` notes, `(use (reg/i ...))` notes, or sideband stats (`sched.c:3573`).
  RtlControlFlowGraph live_cfg = *cfg;
  ReconstructRtlLiveness(&live_cfg, profile,
                         {.reload_completed = false,
                          .update_reg_notes = false,
                          .preserve_phantom_pseudos = true,
                          .apply_post_sched_and_equiv_live_length = false,
                          .include_call_reg_notes = false});

  // Reconcile per-block `REG_DEAD` notes strictly to match `sched.c:3540-3611` +
  // `sched.c:2715-2728, 2842-2905, 3967-3968` (`dead_notes` conservation), leaving
  // `REG_UNUSED` and all non-liveness notes untouched.
  for (size_t b = 0; b < cfg->rpo_blocks.size(); ++b) {
    RtlBasicBlock& blk = cfg->rpo_blocks[b];
    const size_t n = blk.instructions.size();
    if (n == 0) {
      continue;
    }

    std::vector<RtlPatternAndNotes> parsed(n);
    std::vector<bool> ok(n, false);
    std::vector<Sched1InsnRegSummary> sums(n);
    std::vector<std::optional<std::vector<RtlExpr>>> saved_fusage(n);
    for (size_t i = 0; i < n; ++i) {
      if (i < blk.rtl_insns.size() && blk.rtl_insns[i].has_explicit_call_fusage) {
        saved_fusage[i] = blk.rtl_insns[i].call_fusage;
      }
      auto p = ParsePatternAndNotes(blk.instructions[i]);
      if (!p.ok()) {
        continue;
      }
      if (saved_fusage[i].has_value() && !p->has_explicit_call_fusage) {
        p->has_explicit_call_fusage = true;
        p->call_fusage = *saved_fusage[i];
      }
      sums[i] = SummarizeInsnForSched1Deaths(*p, gp64, fp64);
      parsed[i] = std::move(*p);
      ok[i] = true;
    }

    // Step A: Strip any `REG_DEAD` note that `sched.c:2842-2860` (`attach_deaths`) can never
    // re-attach (fixed reg, unread reg, `dead_or_set_p` reg set/clobbered by the same insn,
    // reg read again later in the same block before redefinition, or duplicate `REG_DEAD`).
    bool block_changed = false;
    for (size_t i = 0; i < n; ++i) {
      if (!ok[i] || sums[i].is_note_or_pseudo || sums[i].is_call) {
        continue;
      }
      absl::btree_set<int> seen_dead_words;
      std::vector<RtlRegNote> kept_notes;
      kept_notes.reserve(parsed[i].reg_notes.size());
      bool insn_changed = false;
      for (RtlRegNote note : parsed[i].reg_notes) {
        if (note.kind != RtlMachineMode::kRegDead || !note.datum.IsReg() ||
            note.datum.RegNo() < 0) {
          kept_notes.push_back(std::move(note));
          continue;
        }
        const int r = note.datum.RegNo();
        // `mips.h:1255-1258`: `HARD_REGNO_NREGS(r, VOIDmode) == 0`, so a `VOIDmode` (`kNone`/
        // `kVoid`) hard-reg `REG_DEAD` note never clears `bb_live_regs` in `sched.c:3589-3600`
        // and triggers `sched.c:3968` `abort()`. Repair missing mode from the matching read.
        const auto is_void_mode = [](const RtlExpr& e) {
          return (e.mode == RtlMachineMode::kNone || e.mode == RtlMachineMode::kVoid) &&
                 e.raw_mode.empty();
        };
        if (r < kMipsFirstPseudoRegister && is_void_mode(note.datum)) {
          for (const RtlExpr& rr : sums[i].read_regs) {
            if (rr.RegNo() == r && !is_void_mode(rr)) {
              note.datum.mode = rr.mode;
              note.datum.raw_mode = rr.raw_mode;
              insn_changed = true;
              break;
            }
          }
        }
        const int span = Sched1RegSpan(r, note.datum.mode, gp64, fp64);
        bool valid = !IsFixedForSched1Deaths(r) &&
                     !(r < kMipsFirstPseudoRegister && is_void_mode(note.datum));
        for (int k = 0; k < span && valid; ++k) {
          const int w = r + k;
          if (seen_dead_words.contains(w) || !sums[i].read_words.contains(w) ||
              sums[i].full_def_words.contains(w) || sums[i].unused_words.contains(w)) {
            valid = false;
            break;
          }
          // Check if `w` is read again later in block `b` before being full-redefined.
          for (size_t j = i + 1; j < n; ++j) {
            if (!ok[j] || sums[j].is_note_or_pseudo) {
              continue;
            }
            if (sums[j].read_words.contains(w)) {
              valid = false;
              break;
            }
            if (sums[j].full_def_words.contains(w) ||
                (sums[j].is_call && w < kMipsFirstPseudoRegister && IsCallUsedMipsHardReg(w))) {
              break;
            }
          }
        }
        if (valid) {
          for (int k = 0; k < span; ++k) {
            seen_dead_words.insert(r + k);
          }
          kept_notes.push_back(note);
        } else {
          insn_changed = true;
        }
      }
      if (insn_changed) {
        parsed[i].reg_notes = std::move(kept_notes);
        block_changed = true;
      }
    }

    // Step B: Simulate `sched.c:3540-3609` forward `bb_live` pass from `basic_block_live_at_start`
    // using Step A's cleaned `REG_DEAD` and existing `REG_UNUSED` notes to obtain `bb_live_end`.
    absl::btree_set<int> bb_live;
    if (const auto it = live_cfg.sideband.basic_block_live_at_start.find(static_cast<int>(b));
        it != live_cfg.sideband.basic_block_live_at_start.end()) {
      bb_live.insert(it->second.begin(), it->second.end());
    }
    for (size_t i = 0; i < n; ++i) {
      if (!ok[i] || sums[i].is_note_or_pseudo) {
        continue;
      }
      for (int w : sums[i].full_def_words) {
        bb_live.insert(w);
      }
      for (int w : sums[i].partial_def_words) {
        bb_live.insert(w);
      }
      for (const RtlRegNote& note : parsed[i].reg_notes) {
        if ((note.kind == RtlMachineMode::kRegDead || note.kind == RtlMachineMode::kRegUnused) &&
            note.datum.IsReg() && note.datum.RegNo() >= 0) {
          const int r = note.datum.RegNo();
          const int span = Sched1RegSpan(r, note.datum.mode, gp64, fp64);
          for (int k = 0; k < span; ++k) {
            bb_live.erase(r + k);
          }
        }
      }
    }

    // Step C: Backward pass from `bb_live` (`sched.c:3764-3882`): add any missing `REG_DEAD`
    // note that `attach_deaths` will demand (`sched.c:2728`).
    for (size_t rev = n; rev > 0; --rev) {
      const size_t i = rev - 1;
      if (!ok[i] || sums[i].is_note_or_pseudo) {
        continue;
      }
      for (int w : sums[i].full_def_words) {
        bb_live.erase(w);
      }
      if (sums[i].is_call) {
        for (int r = 0; r < kMipsFirstPseudoRegister; ++r) {
          if (IsCallUsedMipsHardReg(r)) {
            bb_live.erase(r);
          }
        }
        bb_live.insert(sums[i].read_words.begin(), sums[i].read_words.end());
        continue;
      }

      absl::btree_set<int> existing_dead_words;
      for (const RtlRegNote& note : parsed[i].reg_notes) {
        if (note.kind == RtlMachineMode::kRegDead && note.datum.IsReg() &&
            note.datum.RegNo() >= 0) {
          const int r = note.datum.RegNo();
          const int span = Sched1RegSpan(r, note.datum.mode, gp64, fp64);
          for (int k = 0; k < span; ++k) {
            existing_dead_words.insert(r + k);
          }
        }
      }
      const absl::btree_set<int> old_live = bb_live;
      for (const RtlExpr& reg_expr : sums[i].read_regs) {
        const int r = reg_expr.RegNo();
        if (r < 0 || IsFixedForSched1Deaths(r)) {
          continue;
        }
        const int span = Sched1RegSpan(r, reg_expr.mode, gp64, fp64);
        bool all_dead = true;
        bool any_missing_dead = false;
        for (int k = 0; k < span; ++k) {
          const int w = r + k;
          const bool dead_here = !old_live.contains(w) && !sums[i].full_def_words.contains(w) &&
                                 !sums[i].unused_words.contains(w);
          if (!dead_here) {
            all_dead = false;
          } else if (!existing_dead_words.contains(w)) {
            any_missing_dead = true;
          }
        }
        if (!any_missing_dead) {
          continue;
        }
        if (all_dead) {
          bool overlap = false;
          for (int k = 0; k < span; ++k) {
            if (existing_dead_words.contains(r + k)) {
              overlap = true;
              break;
            }
          }
          bool replaced_partial = false;
          if (overlap && r < kMipsFirstPseudoRegister && span > 1) {
            // `flow.c:2537` / `sched.c:2888-2889`: if all words of multi-word
            // hard reg `reg_expr` die here and existing `REG_DEAD` notes only
            // cover a strict subset of `[r, r + span)`, replace those partial
            // notes with the full `(REG_DEAD reg_expr)` so `dead_notes` word
            // count stays conserved (`sched.c:2746-2753`).
            bool all_overlapping_contained = true;
            for (const RtlRegNote& note : parsed[i].reg_notes) {
              if (note.kind != RtlMachineMode::kRegDead || !note.datum.IsReg() ||
                  note.datum.RegNo() < 0) {
                continue;
              }
              const int nr = note.datum.RegNo();
              const int nspan = Sched1RegSpan(nr, note.datum.mode, gp64, fp64);
              const bool overlaps_range = (nr < r + span) && (r < nr + nspan);
              if (overlaps_range && (nr < r || nr + nspan > r + span)) {
                all_overlapping_contained = false;
                break;
              }
            }
            if (all_overlapping_contained) {
              parsed[i].reg_notes.erase(
                  std::remove_if(parsed[i].reg_notes.begin(), parsed[i].reg_notes.end(),
                                 [&](const RtlRegNote& note) {
                                   if (note.kind != RtlMachineMode::kRegDead ||
                                       !note.datum.IsReg() || note.datum.RegNo() < 0) {
                                     return false;
                                   }
                                   const int nr = note.datum.RegNo();
                                   const int nspan = Sched1RegSpan(nr, note.datum.mode, gp64, fp64);
                                   return nr >= r && nr + nspan <= r + span;
                                 }),
                  parsed[i].reg_notes.end());
              parsed[i].reg_notes.insert(parsed[i].reg_notes.begin(),
                                         RtlRegNote{.kind = RtlMachineMode::kRegDead,
                                                    .list_op = RtlOp::kExprList,
                                                    .datum = reg_expr});
              for (int k = 0; k < span; ++k) {
                existing_dead_words.insert(r + k);
              }
              block_changed = true;
              replaced_partial = true;
            }
          }
          if (!overlap) {
            parsed[i].reg_notes.insert(parsed[i].reg_notes.begin(),
                                       RtlRegNote{.kind = RtlMachineMode::kRegDead,
                                                  .list_op = RtlOp::kExprList,
                                                  .datum = reg_expr});
            for (int k = 0; k < span; ++k) {
              existing_dead_words.insert(r + k);
            }
            block_changed = true;
          } else if (!replaced_partial && r < kMipsFirstPseudoRegister && span > 1) {
            for (int k = span - 1; k >= 0; --k) {
              const int w = r + k;
              if (!old_live.contains(w) && !sums[i].full_def_words.contains(w) &&
                  !sums[i].unused_words.contains(w) && !existing_dead_words.contains(w)) {
                parsed[i].reg_notes.insert(
                    parsed[i].reg_notes.begin(),
                    RtlRegNote{
                        .kind = RtlMachineMode::kRegDead,
                        .list_op = RtlOp::kExprList,
                        .datum = RtlExpr::MakeReg(SingleWordModeForHardReg(w, gp64, fp64), w)});
                existing_dead_words.insert(w);
                block_changed = true;
              }
            }
          }
        } else if (r < kMipsFirstPseudoRegister && span > 1) {
          for (int k = span - 1; k >= 0; --k) {
            const int w = r + k;
            if (!old_live.contains(w) && !sums[i].full_def_words.contains(w) &&
                !sums[i].unused_words.contains(w) && !existing_dead_words.contains(w)) {
              parsed[i].reg_notes.insert(
                  parsed[i].reg_notes.begin(),
                  RtlRegNote{
                      .kind = RtlMachineMode::kRegDead,
                      .list_op = RtlOp::kExprList,
                      .datum = RtlExpr::MakeReg(SingleWordModeForHardReg(w, gp64, fp64), w)});
              existing_dead_words.insert(w);
              block_changed = true;
            }
          }
        }
      }
      bb_live.insert(sums[i].read_words.begin(), sums[i].read_words.end());
    }

    if (block_changed) {
      for (size_t i = 0; i < n; ++i) {
        if (ok[i] && !sums[i].is_note_or_pseudo && !sums[i].is_call) {
          blk.instructions[i] = FormatGccPrintRtlPatternAndNotes(
              parsed[i].pattern, parsed[i].reg_notes, parsed[i].bracketed_notes,
              /*call_fusage=*/{});
        }
      }
      blk.SyncRtlInsnsFromInstructions();
      for (size_t i = 0; i < blk.rtl_insns.size() && i < saved_fusage.size(); ++i) {
        if (saved_fusage[i].has_value() && !blk.rtl_insns[i].has_explicit_call_fusage) {
          blk.rtl_insns[i].has_explicit_call_fusage = true;
          blk.rtl_insns[i].call_fusage = std::move(*saved_fusage[i]);
        }
      }
    }
  }

  CanonicalizeCfgLivenessNoteOrder(cfg, profile, /*sched_note_order=*/false,
                                   /*reload_completed=*/false);
}

}  // namespace rom_nom_nom::lifter
