#include "lifter/invert_schedule_block.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/btree_map.h"
#include "absl/container/btree_set.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/types/span.h"
#include "core/compiler_flags.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "lifter/mips_idiom_utils.h"
#include "lifter/post_reload_noop_moves.h"
#include "lifter/reconstruct_hard_reg_liveness.h"
#include "lifter/rtl_liveness.h"
#include "lifter/sched_model.h"
#include "lifter/sched_post_reload_projection.h"

namespace rom_nom_nom::lifter {
namespace {

// Architectural MIPS hard register numbers used in `mips.c` prologue/epilogue
// generation (`GP_ARG_FIRST..GP_ARG_LAST`, `MIPS_TEMP1_REGNUM`,
// `MIPS_TEMP2_REGNUM`, `STACK_POINTER_REGNUM`, `FRAME_POINTER_REGNUM`,
// `GP_REG_FIRST + 31`).
constexpr int kMipsFirstArgGpr = 4;
constexpr int kMipsLastArgGpr = 7;
constexpr int kMipsTemp1Regno = 12;
constexpr int kMipsTemp2Regno = 13;
constexpr int kMipsSpRegno = 29;
constexpr int kMipsFpRegno = 30;
constexpr int kMipsRaRegno = 31;
constexpr int kMipsFirstCalleeSavedGpr = 16;
constexpr int kMipsLastCalleeSavedGpr = 23;
constexpr int kMipsFirstFpr = 32;
constexpr int kMipsLastFpr = 63;
constexpr int kMipsFirstCalleeSavedFpr = 52;
constexpr int kMipsHiRegno = 64;
constexpr int kMipsAccumRegno = 66;

int HardRegSpanCount(int regno, RtlMachineMode mode, bool gp64, bool fp64) {
  if (regno >= 0 && regno < kMipsFirstPseudoRegister) {
    return MipsHardRegNregs(regno, mode, gp64, fp64);
  }
  return 1;
}

void InsertRegSpan(int regno, RtlMachineMode mode, bool gp64, bool fp64,
                   absl::flat_hash_set<int>* set) {
  if (regno < 0) {
    return;
  }
  const int count = HardRegSpanCount(regno, mode, gp64, fp64);
  for (int i = 0; i < count; ++i) {
    set->insert(regno + i);
  }
}

bool IsReturnInternalExpr(const RtlExpr& pat) {
  return pat.op == RtlOp::kParallel && !pat.children.empty() &&
         pat.children[0].op == RtlOp::kReturn;
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
      if (child.op == RtlOp::kLabelRef && !child.str_val.empty()) {
        if (child.str_val[0] == 'L' && child.str_val.size() > 1) {
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
  }
  const bool child_allow_return = (expr.op == RtlOp::kParallel);
  for (const RtlExpr& child : expr.children) {
    if (ContainsUnknownOrMalformedOp(child, num_blocks, child_allow_return)) {
      return true;
    }
  }
  return false;
}

bool ValidateScheduledCfg(const RtlControlFlowGraph& cfg) {
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

void CollectReadRegsFromExpr(const RtlExpr& expr, bool gp64, bool fp64,
                             absl::flat_hash_set<int>* reads) {
  if (expr.op == RtlOp::kReg) {
    InsertRegSpan(expr.RegNo(), expr.mode, gp64, fp64, reads);
    return;
  }
  if (expr.op == RtlOp::kSubreg && !expr.children.empty() && expr.children[0].op == RtlOp::kReg) {
    InsertRegSpan(expr.children[0].RegNo(), expr.mode, gp64, fp64, reads);
    return;
  }
  for (const RtlExpr& child : expr.children) {
    CollectReadRegsFromExpr(child, gp64, fp64, reads);
  }
}

void CollectReadWriteAndMemFromDestination(const RtlExpr& dst, bool gp64, bool fp64,
                                           absl::flat_hash_set<int>* reads,
                                           absl::flat_hash_set<int>* writes, bool* writes_mem,
                                           bool* has_side_effects) {
  const RtlExpr* cur = &dst;
  while (cur->op == RtlOp::kSubreg || cur->op == RtlOp::kStrictLowPart ||
         cur->op == RtlOp::kZeroExtract || cur->op == RtlOp::kSignExtract) {
    if ((cur->op == RtlOp::kZeroExtract || cur->op == RtlOp::kSignExtract) &&
        cur->children.size() >= 3) {
      CollectReadRegsFromExpr(cur->children[1], gp64, fp64, reads);
      CollectReadRegsFromExpr(cur->children[2], gp64, fp64, reads);
    }
    if (cur->children.empty()) {
      return;
    }
    cur = &cur->children[0];
  }
  if (cur->op == RtlOp::kReg) {
    InsertRegSpan(cur->RegNo(), cur->mode, gp64, fp64, writes);
  } else if (cur->op == RtlOp::kMem) {
    *writes_mem = true;
    if (cur->flags.volatil) {
      *has_side_effects = true;
    }
    if (!cur->children.empty()) {
      CollectReadRegsFromExpr(cur->children[0], gp64, fp64, reads);
    }
  } else {
    CollectReadRegsFromExpr(*cur, gp64, fp64, reads);
  }
}

void AnalyzeSourceExpr(const RtlExpr& expr, bool gp64, bool fp64, absl::flat_hash_set<int>* reads,
                       bool* reads_mem, bool* has_side_effects) {
  if (expr.op == RtlOp::kMem) {
    *reads_mem = true;
    if (expr.flags.volatil) {
      *has_side_effects = true;
    }
  }
  if (expr.op == RtlOp::kCall || expr.op == RtlOp::kAsmInput || expr.op == RtlOp::kAsmOperands ||
      expr.op == RtlOp::kUnspec || expr.op == RtlOp::kUnspecVolatile ||
      expr.op == RtlOp::kUnalignedLoad || expr.op == RtlOp::kUnalignedStore) {
    *has_side_effects = true;
  }
  if (expr.op == RtlOp::kReg) {
    InsertRegSpan(expr.RegNo(), expr.mode, gp64, fp64, reads);
    return;
  }
  for (const RtlExpr& child : expr.children) {
    AnalyzeSourceExpr(child, gp64, fp64, reads, reads_mem, has_side_effects);
  }
}

struct ParsedBlockInsn {
  RtlPatternAndNotes parsed;
  std::optional<RtlInsn> orig_rtl_insn;
  absl::flat_hash_set<int> read_regs;
  absl::flat_hash_set<int> write_regs;
  bool reads_mem = false;
  bool writes_mem = false;
  bool has_side_effects = false;
};

void AnalyzeStatementNode(const RtlExpr& stmt, bool gp64, bool fp64, ParsedBlockInsn* info) {
  if (stmt.op == RtlOp::kSet && stmt.children.size() == 2) {
    if (stmt.children[0].op == RtlOp::kPc) {
      info->has_side_effects = true;
    } else {
      CollectReadWriteAndMemFromDestination(stmt.children[0], gp64, fp64, &info->read_regs,
                                            &info->write_regs, &info->writes_mem,
                                            &info->has_side_effects);
    }
    AnalyzeSourceExpr(stmt.children[1], gp64, fp64, &info->read_regs, &info->reads_mem,
                      &info->has_side_effects);
    return;
  }
  if (stmt.op == RtlOp::kClobber && !stmt.children.empty()) {
    CollectReadWriteAndMemFromDestination(stmt.children[0], gp64, fp64, &info->read_regs,
                                          &info->write_regs, &info->writes_mem,
                                          &info->has_side_effects);
    return;
  }
  if (stmt.op == RtlOp::kUse && !stmt.children.empty()) {
    AnalyzeSourceExpr(stmt.children[0], gp64, fp64, &info->read_regs, &info->reads_mem,
                      &info->has_side_effects);
    return;
  }
  if (stmt.op == RtlOp::kParallel) {
    for (const RtlExpr& elem : stmt.children) {
      AnalyzeStatementNode(elem, gp64, fp64, info);
    }
    return;
  }
  info->has_side_effects = true;
  AnalyzeSourceExpr(stmt, gp64, fp64, &info->read_regs, &info->reads_mem, &info->has_side_effects);
}

ParsedBlockInsn BuildParsedBlockInsn(RtlPatternAndNotes parsed, bool gp64, bool fp64,
                                     std::optional<RtlInsn> orig_insn = std::nullopt) {
  ParsedBlockInsn info;
  info.parsed = std::move(parsed);
  info.orig_rtl_insn = std::move(orig_insn);
  AnalyzeStatementNode(info.parsed.pattern, gp64, fp64, &info);
  return info;
}

bool FusageListReadsReg(absl::Span<const RtlExpr> call_fusage, int regno, bool gp64, bool fp64) {
  for (const RtlExpr& usage_elem : call_fusage) {
    if (usage_elem.op == RtlOp::kUse) {
      absl::flat_hash_set<int> fusage_reads;
      CollectReadRegsFromExpr(usage_elem, gp64, fp64, &fusage_reads);
      if (fusage_reads.contains(regno)) {
        return true;
      }
    }
  }
  return false;
}

bool CallInsnReadsArgReg(const ParsedBlockInsn& call_insn, int regno, bool gp64, bool fp64) {
  if (call_insn.read_regs.contains(regno)) {
    return true;
  }
  if (call_insn.orig_rtl_insn.has_value() && call_insn.orig_rtl_insn->has_explicit_call_fusage) {
    return FusageListReadsReg(call_insn.orig_rtl_insn->call_fusage, regno, gp64, fp64);
  }
  if (call_insn.parsed.has_explicit_call_fusage) {
    return FusageListReadsReg(call_insn.parsed.call_fusage, regno, gp64, fp64);
  }
  return false;
}

bool CallInsnSetsReturnReg(const ParsedBlockInsn& call_insn, int regno, bool gp64, bool fp64) {
  absl::flat_hash_set<int> ret_regs;
  auto collect_set_dest = [&](const RtlExpr& stmt) {
    if (stmt.op == RtlOp::kSet && stmt.children.size() == 2) {
      const RtlExpr* dst = &stmt.children[0];
      while (dst->op == RtlOp::kSubreg && !dst->children.empty()) {
        dst = &dst->children[0];
      }
      if (dst->op == RtlOp::kReg) {
        InsertRegSpan(dst->RegNo(), dst->mode, gp64, fp64, &ret_regs);
      }
    }
  };
  const RtlExpr& pat = call_insn.parsed.pattern;
  if (pat.op == RtlOp::kParallel) {
    for (const RtlExpr& elem : pat.children) {
      collect_set_dest(elem);
    }
  } else {
    collect_set_dest(pat);
  }
  return ret_regs.contains(regno);
}

// Returns true if `insn` is a single-set non-volatile memory load into a scalar
// integer or floating-point register `out_dest_regno` (not `$sp` or `$fp`).
bool IsSchedulableLoadInsn(const ParsedBlockInsn& insn, int* out_dest_regno) {
  if (insn.has_side_effects || insn.writes_mem || !insn.reads_mem) {
    return false;
  }
  const RtlExpr& pat = insn.parsed.pattern;
  if (pat.op != RtlOp::kSet || pat.children.size() != 2) {
    return false;
  }
  const RtlExpr& dst = pat.children[0];
  if (dst.op != RtlOp::kReg) {
    return false;
  }
  const int dst_regno = dst.RegNo();
  if (dst_regno < 0 || dst_regno == kMipsSpRegno || dst_regno == kMipsFpRegno ||
      (dst_regno >= kMipsHiRegno && dst_regno <= kMipsAccumRegno)) {
    return false;
  }
  const RtlExpr* src = &pat.children[1];
  if ((src->op == RtlOp::kSignExtend || src->op == RtlOp::kZeroExtend) &&
      src->children.size() == 1) {
    src = &src->children[0];
  }
  if (src->op != RtlOp::kMem || src->flags.volatil || src->children.size() != 1) {
    return false;
  }
  *out_dest_regno = dst_regno;
  return true;
}

bool IsDirectSymbolLoadInsn(const ParsedBlockInsn& insn) {
  const RtlExpr& pat = insn.parsed.pattern;
  if (pat.op != RtlOp::kSet || pat.children.size() != 2) {
    return false;
  }
  const RtlExpr* src = &pat.children[1];
  if ((src->op == RtlOp::kSignExtend || src->op == RtlOp::kZeroExtend) &&
      src->children.size() == 1) {
    src = &src->children[0];
  }
  if (src->op != RtlOp::kMem || src->children.size() != 1) {
    return false;
  }
  const RtlExpr& addr = src->children[0];
  return addr.op == RtlOp::kSymbolRef || addr.op == RtlOp::kConst;
}

bool IsConstIntMoveInsn(const ParsedBlockInsn& insn) {
  const RtlExpr& pat = insn.parsed.pattern;
  return pat.op == RtlOp::kSet && pat.children.size() == 2 && pat.children[0].op == RtlOp::kReg &&
         pat.children[1].op == RtlOp::kConstInt;
}

bool IsCalleeSavedMipsHardReg(int regno) {
  return (regno >= kMipsFirstCalleeSavedGpr && regno <= kMipsLastCalleeSavedGpr) ||
         regno == kMipsFpRegno || (regno >= kMipsFirstCalleeSavedFpr && regno <= kMipsLastFpr);
}

// Checks `mips.c:large_int(val)`: true when `(val & 0xffff0000) != 0`,
// `(val & 0xffff0000) != 0xffff0000`, and `(val & 0x0000ffff) != 0`.
bool IsMipsLargeInt(int64_t val) {
  const uint32_t u32 = static_cast<uint32_t>(val);
  const uint32_t high = u32 & 0xffff0000u;
  const uint32_t low = u32 & 0x0000ffffu;
  return high != 0u && high != 0xffff0000u && low != 0u;
}

bool HasRegDeadNoteFor(const ParsedBlockInsn& insn, int regno) {
  for (const RtlRegNote& note : insn.parsed.reg_notes) {
    if (note.kind == RtlMachineMode::kRegDead && note.datum.op == RtlOp::kReg &&
        note.datum.RegNo() == regno) {
      return true;
    }
  }
  return false;
}

bool IsSingleCycleIntAluExpr(const RtlExpr& expr) {
  switch (expr.op) {
    case RtlOp::kReg: {
      const int r = expr.RegNo();
      // FPRs (`32..63`) and HI/LO (`64..66`) have multi-cycle `xfer`/`hilo`
      // latencies in `mips.md`.
      return r >= 0 && (r < kMipsFirstFpr || r > kMipsAccumRegno);
    }
    case RtlOp::kSubreg:
      return !expr.children.empty() && IsSingleCycleIntAluExpr(expr.children[0]);
    case RtlOp::kConstInt:
      return !IsMipsLargeInt(expr.int_val);
    case RtlOp::kSymbolRef:
    case RtlOp::kLabelRef:
    case RtlOp::kConst:
    case RtlOp::kHigh:
      return true;
    case RtlOp::kLoSum:
    case RtlOp::kPlus:
    case RtlOp::kMinus:
    case RtlOp::kAnd:
    case RtlOp::kIor:
    case RtlOp::kXor:
    case RtlOp::kNot:
    case RtlOp::kNeg:
    case RtlOp::kAshift:
    case RtlOp::kAshiftrt:
    case RtlOp::kLshiftrt:
    case RtlOp::kSignExtend:
    case RtlOp::kZeroExtend:
    case RtlOp::kTruncate:
    case RtlOp::kEq:
    case RtlOp::kNe:
    case RtlOp::kLt:
    case RtlOp::kLe:
    case RtlOp::kGt:
    case RtlOp::kGe:
    case RtlOp::kLtu:
    case RtlOp::kLeu:
    case RtlOp::kGtu:
    case RtlOp::kGeu: {
      if (expr.mode == RtlMachineMode::kSf || expr.mode == RtlMachineMode::kDf ||
          expr.mode == RtlMachineMode::kDi) {
        return false;
      }
      for (const RtlExpr& child : expr.children) {
        if (!IsSingleCycleIntAluExpr(child)) {
          return false;
        }
      }
      return true;
    }
    default:
      return false;
  }
}

// Returns true if `insn` is a single-set 1-cycle integer ALU or register-move
// instruction writing `out_dest_regno` (not `$sp`).
bool IsSingleCycleIntAluInsn(const ParsedBlockInsn& insn, int* out_dest_regno) {
  if (insn.has_side_effects || insn.reads_mem || insn.writes_mem) {
    return false;
  }
  const RtlExpr& pat = insn.parsed.pattern;
  if (pat.op != RtlOp::kSet || pat.children.size() != 2) {
    return false;
  }
  const RtlExpr& dst = pat.children[0];
  if (dst.op != RtlOp::kReg || dst.mode == RtlMachineMode::kDi || dst.mode == RtlMachineMode::kSf ||
      dst.mode == RtlMachineMode::kDf) {
    return false;
  }
  const int dst_regno = dst.RegNo();
  if (dst_regno < 0 || dst_regno == kMipsSpRegno ||
      (dst_regno >= kMipsFirstFpr && dst_regno <= kMipsAccumRegno)) {
    return false;
  }
  if (!IsSingleCycleIntAluExpr(pat.children[1])) {
    return false;
  }
  *out_dest_regno = dst_regno;
  return true;
}

bool SetsAreDisjoint(const absl::flat_hash_set<int>& a, const absl::flat_hash_set<int>& b) {
  for (int x : a) {
    if (b.contains(x)) {
      return false;
    }
  }
  return true;
}

bool IsRealConsumerInsn(const ParsedBlockInsn& insn) {
  const RtlOp op = insn.parsed.pattern.op;
  if (op == RtlOp::kUse || op == RtlOp::kClobber || op == RtlOp::kReturn ||
      IsReturnInternalExpr(insn.parsed.pattern) || op == RtlOp::kNote || op == RtlOp::kCodeLabel ||
      op == RtlOp::kBarrier || op == RtlOp::kSequence || op == RtlOp::kCall) {
    return false;
  }
  if (op == RtlOp::kSet && !insn.parsed.pattern.children.empty() &&
      insn.parsed.pattern.children[0].op == RtlOp::kPc) {
    return false;
  }
  return true;
}

// Verifies that no instruction after `consumer_idx` in `insns` has a
// `sched_analyze` dependence (`LOG_LINKS` edge) directly to `load_insn` or
// `alu_insn` that would keep `INSN_REF_COUNT` nonzero when `insns[consumer_idx]`
// is scheduled in reverse order.
bool HasNoLaterDirectDependents(absl::Span<const ParsedBlockInsn> insns, size_t consumer_idx,
                                const ParsedBlockInsn& load_insn, int load_regno,
                                const ParsedBlockInsn& alu_insn, int alu_regno) {
  const ParsedBlockInsn& consumer = insns[consumer_idx];
  const bool load_redefined_at_consumer = consumer.write_regs.contains(load_regno);
  const bool alu_redefined_at_consumer = consumer.write_regs.contains(alu_regno);

  for (size_t k = consumer_idx + 1; k < insns.size(); ++k) {
    const ParsedBlockInsn& later = insns[k];
    if (later.writes_mem || later.has_side_effects) {
      const RtlOp op = later.parsed.pattern.op;
      const bool is_pc_set = op == RtlOp::kSet && !later.parsed.pattern.children.empty() &&
                             later.parsed.pattern.children[0].op == RtlOp::kPc;
      if (op != RtlOp::kUse && op != RtlOp::kReturn &&
          !IsReturnInternalExpr(later.parsed.pattern) && !is_pc_set) {
        return false;
      }
    }
    if (!load_redefined_at_consumer && later.read_regs.contains(load_regno)) {
      return false;
    }
    if (!alu_redefined_at_consumer && later.read_regs.contains(alu_regno)) {
      return false;
    }
    if (!SetsAreDisjoint(later.write_regs, load_insn.read_regs) ||
        !SetsAreDisjoint(later.write_regs, load_insn.write_regs) ||
        !SetsAreDisjoint(later.write_regs, alu_insn.read_regs) ||
        !SetsAreDisjoint(later.write_regs, alu_insn.write_regs)) {
      return false;
    }
  }
  return true;
}

bool IsCallExpr(const RtlExpr& pat) {
  if (pat.op == RtlOp::kCall) {
    return true;
  }
  for (const RtlExpr& child : pat.children) {
    if (IsCallExpr(child)) {
      return true;
    }
  }
  return false;
}

bool IsSimpleRegMemStoreInsn(const ParsedBlockInsn& insn, int* out_src_regno) {
  if (insn.has_side_effects || insn.reads_mem || !insn.writes_mem) {
    return false;
  }
  const RtlExpr& pat = insn.parsed.pattern;
  if (pat.op != RtlOp::kSet || pat.children.size() != 2) {
    return false;
  }
  const RtlExpr& dst = pat.children[0];
  const RtlExpr& src = pat.children[1];
  if (dst.op != RtlOp::kMem || dst.flags.volatil || dst.children.size() != 1 ||
      src.op != RtlOp::kReg) {
    return false;
  }
  *out_src_regno = src.RegNo();
  return *out_src_regno >= 0;
}

bool IsMultiCycleFpAluInsn(const ParsedBlockInsn& insn, int* out_dst_regno) {
  if (insn.has_side_effects || insn.reads_mem || insn.writes_mem) {
    return false;
  }
  const RtlExpr& pat = insn.parsed.pattern;
  if (pat.op != RtlOp::kSet || pat.children.size() != 2) {
    return false;
  }
  const RtlExpr& dst = pat.children[0];
  const RtlExpr& src = pat.children[1];
  if (dst.op != RtlOp::kReg ||
      (dst.mode != RtlMachineMode::kSf && dst.mode != RtlMachineMode::kDf)) {
    return false;
  }
  switch (src.op) {
    case RtlOp::kPlus:
    case RtlOp::kMinus:
    case RtlOp::kMult:
    case RtlOp::kDiv:
    case RtlOp::kNeg:
    case RtlOp::kAbs:
    case RtlOp::kSqrt:
      break;
    default:
      return false;
  }
  *out_dst_regno = dst.RegNo();
  return *out_dst_regno >= 0;
}

// Reverses `schedule_block` hoisting/interleaving of multi-cycle FP arithmetic
// producers and outgoing call-argument 1-cycle ALU setup instructions across
// memory stores preceding a `CALL_INSN` (covering both R4000 full hoisting
// ahead of all stores and R3000/-mips1 interleaving of FP producers with
// independent stores).
bool UnscheduleHoistedFpStoresAndCallArgs(std::vector<ParsedBlockInsn>* insns_ptr, bool gp64,
                                          bool fp64) {
  std::vector<ParsedBlockInsn>& insns = *insns_ptr;
  bool any_modified = false;

  for (size_t call_idx = 0; call_idx < insns.size(); ++call_idx) {
    if (!IsCallExpr(insns[call_idx].parsed.pattern) || call_idx < 3) {
      continue;
    }

    size_t trailing_store_start = call_idx;
    while (trailing_store_start > 0) {
      int store_src_reg = -1;
      if (!IsSimpleRegMemStoreInsn(insns[trailing_store_start - 1], &store_src_reg)) {
        break;
      }
      --trailing_store_start;
    }
    if (call_idx - trailing_store_start < 2) {
      continue;
    }

    // Scan backward across the contiguous pre-call region of simple stores,
    // multi-cycle FP producers, and 1-cycle integer ALU instructions.
    size_t window_start = trailing_store_start;
    while (window_start > 0) {
      const ParsedBlockInsn& cand = insns[window_start - 1];
      int reg = -1;
      if (IsSimpleRegMemStoreInsn(cand, &reg) || IsMultiCycleFpAluInsn(cand, &reg) ||
          IsSingleCycleIntAluInsn(cand, &reg)) {
        --window_start;
      } else {
        break;
      }
    }
    if (window_start == trailing_store_start) {
      continue;
    }

    // Validate `[window_start .. call_idx - 1]`, advancing `window_start`
    // past any unrelated earlier instructions until the window is self-contained.
    bool window_valid = false;
    std::vector<int> kind_in_window;  // 0 = store, 1 = fp_prod, 2 = int_alu
    std::vector<int> fp_consumer_store;
    while (window_start < trailing_store_start) {
      const size_t win_len = call_idx - window_start;
      kind_in_window.assign(win_len, -1);
      fp_consumer_store.assign(win_len, -1);
      bool ok = true;
      int num_stores = 0;
      int num_hoisted_fp_producers = 0;

      for (size_t h = window_start; h < call_idx; ++h) {
        const ParsedBlockInsn& cand = insns[h];
        int store_src = -1;
        int fp_dst = -1;
        int alu_dst = -1;

        if (IsSimpleRegMemStoreInsn(cand, &store_src)) {
          kind_in_window[h - window_start] = 0;
          ++num_stores;
        } else if (IsMultiCycleFpAluInsn(cand, &fp_dst)) {
          kind_in_window[h - window_start] = 1;
          for (size_t k = window_start; k < call_idx; ++k) {
            if (k != h && !SetsAreDisjoint(cand.read_regs, insns[k].write_regs)) {
              ok = false;
              break;
            }
          }
          if (!ok) {
            break;
          }
          int matched_store = -1;
          for (size_t k = window_start; k < call_idx; ++k) {
            if (k == h) {
              continue;
            }
            if (insns[k].write_regs.contains(fp_dst)) {
              ok = false;
              break;
            }
            if (insns[k].read_regs.contains(fp_dst)) {
              int k_store_src = -1;
              if (k > h && matched_store == -1 && HasRegDeadNoteFor(insns[k], fp_dst) &&
                  IsSimpleRegMemStoreInsn(insns[k], &k_store_src) && k_store_src == fp_dst) {
                matched_store = static_cast<int>(k);
              } else {
                ok = false;
                break;
              }
            }
          }
          if (!ok || matched_store == -1) {
            ok = false;
            break;
          }
          fp_consumer_store[h - window_start] = matched_store;
          if (matched_store > static_cast<int>(h) + 1) {
            ++num_hoisted_fp_producers;
          }
        } else if (IsSingleCycleIntAluInsn(cand, &alu_dst)) {
          kind_in_window[h - window_start] = 2;
          if (!CallInsnReadsArgReg(insns[call_idx], alu_dst, gp64, fp64)) {
            ok = false;
            break;
          }
          for (size_t k = window_start; k < call_idx; ++k) {
            if (k == h) {
              continue;
            }
            if (!SetsAreDisjoint(cand.read_regs, insns[k].write_regs) ||
                insns[k].read_regs.contains(alu_dst) || insns[k].write_regs.contains(alu_dst)) {
              ok = false;
              break;
            }
          }
          if (!ok) {
            break;
          }
        } else {
          ok = false;
          break;
        }
      }

      if (ok && num_stores >= 2 && num_hoisted_fp_producers > 0) {
        // Verify that every store's address and value registers are not written
        // anywhere in `[window_start .. call_idx - 1]` except by that store's
        // matched `MultiCycleFpAluInsn` producer.
        for (size_t s = window_start; s < call_idx && ok; ++s) {
          if (kind_in_window[s - window_start] != 0) {
            continue;
          }
          for (size_t k = window_start; k < call_idx; ++k) {
            if (k == s) {
              continue;
            }
            if (kind_in_window[k - window_start] == 1 &&
                fp_consumer_store[k - window_start] == static_cast<int>(s)) {
              // Address registers of `insns[s]` must still be disjoint from `insns[k].write_regs`.
              int s_src = -1;
              IsSimpleRegMemStoreInsn(insns[s], &s_src);
              for (int r : insns[s].read_regs) {
                if (r != s_src && insns[k].write_regs.contains(r)) {
                  ok = false;
                  break;
                }
              }
              if (!ok) {
                break;
              }
              continue;
            }
            if (!SetsAreDisjoint(insns[s].read_regs, insns[k].write_regs)) {
              ok = false;
              break;
            }
          }
        }
      } else {
        ok = false;
      }

      if (ok) {
        window_valid = true;
        break;
      }
      ++window_start;
    }

    if (!window_valid) {
      continue;
    }

    std::vector<size_t> new_order;
    new_order.reserve(call_idx - window_start);
    for (size_t s = window_start; s < call_idx; ++s) {
      if (kind_in_window[s - window_start] != 0) {
        continue;
      }
      for (size_t h = window_start; h < call_idx; ++h) {
        if (kind_in_window[h - window_start] == 1 &&
            fp_consumer_store[h - window_start] == static_cast<int>(s)) {
          new_order.push_back(h);
        }
      }
      new_order.push_back(s);
    }
    for (size_t h = window_start; h < call_idx; ++h) {
      if (kind_in_window[h - window_start] == 2) {
        new_order.push_back(h);
      }
    }

    bool changed = false;
    for (size_t i = 0; i < new_order.size(); ++i) {
      if (new_order[i] != window_start + i) {
        changed = true;
        break;
      }
    }
    if (!changed) {
      continue;
    }

    std::vector<ParsedBlockInsn> reordered;
    reordered.reserve(new_order.size());
    for (size_t idx : new_order) {
      reordered.push_back(std::move(insns[idx]));
    }
    for (size_t i = 0; i < reordered.size(); ++i) {
      insns[window_start + i] = std::move(reordered[i]);
    }
    any_modified = true;
  }

  return any_modified;
}

// Reverses `schedule_block` sinking of a callee-saved multi-cycle FP ALU
// instruction across an earlier `CALL_INSN` into the argument setup of a later
// `CALL_INSN`. In GCC 2.7.2 (`sched.c:1704`), `sched_analyze_1` checks
// `call_used_regs[i]` instead of `call_used_regs[regno + i]` (where `i = 0` has
// `call_used_regs[0] == 1`), attaching a spurious `REG_DEP_ANTI` edge to
// `last_function_call` whenever the instruction starts after `CALL_INSN #1`.
bool UnscheduleCalleeSavedFpAluSunkAcrossCall(std::vector<ParsedBlockInsn>* insns_ptr, bool gp64,
                                              bool fp64) {
  std::vector<ParsedBlockInsn>& insns = *insns_ptr;
  bool any_modified = false;

  for (size_t idx = 1; idx + 2 < insns.size(); ++idx) {
    const ParsedBlockInsn& cand = insns[idx];
    int fp_dst = -1;
    int alu_dst = -1;
    if (!IsMultiCycleFpAluInsn(cand, &fp_dst) || cand.read_regs.empty() ||
        cand.write_regs.empty() || !IsSingleCycleIntAluInsn(insns[idx + 1], &alu_dst)) {
      continue;
    }
    bool all_callee_saved = true;
    for (int r : cand.write_regs) {
      if (!IsCalleeSavedMipsHardReg(r)) {
        all_callee_saved = false;
        break;
      }
    }
    for (int r : cand.read_regs) {
      if (!IsCalleeSavedMipsHardReg(r)) {
        all_callee_saved = false;
        break;
      }
    }
    if (!all_callee_saved) {
      continue;
    }
    if (!SetsAreDisjoint(cand.read_regs, insns[idx + 1].read_regs) ||
        !SetsAreDisjoint(cand.read_regs, insns[idx + 1].write_regs) ||
        !SetsAreDisjoint(cand.write_regs, insns[idx + 1].read_regs) ||
        !SetsAreDisjoint(cand.write_regs, insns[idx + 1].write_regs)) {
      continue;
    }
    if (!IsCallExpr(insns[idx + 2].parsed.pattern) ||
        !CallInsnReadsArgReg(insns[idx + 2], alu_dst, gp64, fp64)) {
      continue;
    }

    size_t earliest_valid_call = idx;
    for (size_t back = idx; back > 0; --back) {
      const ParsedBlockInsn& prev = insns[back - 1];
      if (!SetsAreDisjoint(prev.read_regs, cand.write_regs) ||
          !SetsAreDisjoint(prev.write_regs, cand.read_regs) ||
          !SetsAreDisjoint(prev.write_regs, cand.write_regs)) {
        break;
      }
      bool note_conflict = false;
      for (const RtlRegNote& note : prev.parsed.reg_notes) {
        if (note.datum.op == RtlOp::kReg && (cand.read_regs.contains(note.datum.RegNo()) ||
                                             cand.write_regs.contains(note.datum.RegNo()))) {
          note_conflict = true;
          break;
        }
      }
      if (note_conflict) {
        break;
      }
      if (IsCallExpr(prev.parsed.pattern)) {
        earliest_valid_call = back - 1;
      }
    }

    if (earliest_valid_call < idx) {
      ParsedBlockInsn hoisted = std::move(insns[idx]);
      for (size_t m = idx; m > earliest_valid_call; --m) {
        insns[m] = std::move(insns[m - 1]);
      }
      insns[earliest_valid_call] = std::move(hoisted);
      any_modified = true;
    }
  }

  return any_modified;
}

// Reverses `sched2` (`schedule_block`) latency-hiding sinks of `reload`-emitted
// `=x` (`HI`/`LO`) output reloads `(set (reg:M gpr) (reg:M 64/65))` (and any
// immediately adjacent dying stack spill store `(set (mem:M stack_slot) (reg:M gpr))`)
// back immediately after the `mult` `PARALLEL` that defined `HI`/`LO`.
bool UnscheduleDelayedHiLoReloads(std::vector<ParsedBlockInsn>* insns_ptr,
                                  bool frame_pointer_needed) {
  std::vector<ParsedBlockInsn>& insns = *insns_ptr;
  bool any_modified = false;

  auto extract_stack_mem_range = [&](const RtlExpr& mem, int* out_base, int64_t* out_off,
                                     int64_t* out_sz) -> bool {
    if (mem.op != RtlOp::kMem || mem.flags.volatil || mem.children.size() != 1) {
      return false;
    }
    int64_t sz = 0;
    switch (mem.mode) {
      case RtlMachineMode::kQi:
        sz = 1;
        break;
      case RtlMachineMode::kHi:
        sz = 2;
        break;
      case RtlMachineMode::kSi:
      case RtlMachineMode::kSf:
        sz = 4;
        break;
      case RtlMachineMode::kDi:
      case RtlMachineMode::kDf:
        sz = 8;
        break;
      default:
        return false;
    }
    const RtlExpr& addr = mem.children[0];
    int base = -1;
    int64_t off = 0;
    if (addr.op == RtlOp::kReg) {
      base = addr.RegNo();
    } else if (addr.op == RtlOp::kPlus && addr.children.size() == 2 &&
               addr.children[0].op == RtlOp::kReg && addr.children[1].op == RtlOp::kConstInt) {
      base = addr.children[0].RegNo();
      off = addr.children[1].int_val;
    } else {
      return false;
    }
    const int stack_base = frame_pointer_needed ? kMipsFpRegno : kMipsSpRegno;
    if (base != kMipsSpRegno && base != stack_base) {
      return false;
    }
    *out_base = base;
    *out_off = off;
    *out_sz = sz;
    return true;
  };

  auto has_only_dead_note_for = [](const RtlPatternAndNotes& p, int regno) -> bool {
    if (p.reg_notes.size() != 1) {
      return false;
    }
    const RtlRegNote& n = p.reg_notes[0];
    return n.kind == RtlMachineMode::kRegDead && n.datum.op == RtlOp::kReg &&
           !n.datum.IsArgPointerReg() && n.datum.RegNo() == regno;
  };

  for (size_t i = 0; i + 2 < insns.size(); ++i) {
    const RtlExpr& mul_pat = insns[i].parsed.pattern;
    if (mul_pat.op != RtlOp::kParallel || mul_pat.children.size() < 2) {
      continue;
    }
    const RtlExpr& elem0 = mul_pat.children[0];
    if (elem0.op != RtlOp::kSet || elem0.children.size() != 2 ||
        elem0.children[0].op != RtlOp::kReg || elem0.children[0].IsArgPointerReg()) {
      continue;
    }
    const int md_reg = elem0.children[0].RegNo();
    const RtlMachineMode md_mode = elem0.children[0].mode;
    const RtlOp rhs_op = elem0.children[1].op;
    const bool valid_mul =
        (md_reg == 64 && md_mode == RtlMachineMode::kDi && rhs_op == RtlOp::kMult) ||
        (md_reg == 65 && md_mode == RtlMachineMode::kSi && rhs_op == RtlOp::kMult) ||
        (md_reg == 64 && md_mode == RtlMachineMode::kSi && rhs_op == RtlOp::kTruncate);
    if (!valid_mul) {
      continue;
    }

    for (size_t mv_idx = i + 1; mv_idx < insns.size(); ++mv_idx) {
      const RtlExpr& mv_pat = insns[mv_idx].parsed.pattern;
      if (mv_pat.op == RtlOp::kSet && mv_pat.children.size() == 2 &&
          mv_pat.children[0].op == RtlOp::kReg && !mv_pat.children[0].IsArgPointerReg() &&
          mv_pat.children[1].op == RtlOp::kReg && !mv_pat.children[1].IsArgPointerReg() &&
          mv_pat.children[0].mode == mv_pat.children[1].mode) {
        const int gpr = mv_pat.children[0].RegNo();
        const int src_md = mv_pat.children[1].RegNo();
        const RtlMachineMode mv_mode = mv_pat.children[0].mode;
        const bool md_match =
            (src_md == md_reg && mv_mode == md_mode) ||
            (md_reg == 64 && md_mode == RtlMachineMode::kSi && rhs_op == RtlOp::kTruncate &&
             src_md == 65 && mv_mode == RtlMachineMode::kSi);
        if (md_match && gpr >= 1 && gpr < 32 && gpr != kMipsSpRegno &&
            (!frame_pointer_needed || gpr != kMipsFpRegno) &&
            has_only_dead_note_for(insns[mv_idx].parsed, src_md)) {
          if (mv_idx == i + 1) {
            break;
          }
          bool can_hoist_mv = true;
          for (size_t k = i + 1; k < mv_idx; ++k) {
            if (insns[k].has_side_effects ||
                !SetsAreDisjoint(insns[k].read_regs, insns[mv_idx].write_regs) ||
                !SetsAreDisjoint(insns[k].write_regs, insns[mv_idx].read_regs) ||
                !SetsAreDisjoint(insns[k].write_regs, insns[mv_idx].write_regs)) {
              can_hoist_mv = false;
              break;
            }
            for (const RtlRegNote& note : insns[k].parsed.reg_notes) {
              if (note.datum.op == RtlOp::kReg &&
                  (insns[mv_idx].read_regs.contains(note.datum.RegNo()) ||
                   insns[mv_idx].write_regs.contains(note.datum.RegNo()))) {
                can_hoist_mv = false;
                break;
              }
            }
            if (!can_hoist_mv) {
              break;
            }
          }
          if (can_hoist_mv) {
            bool hoist_adj_store = false;
            int st_base = -1;
            int64_t st_off = 0;
            int64_t st_sz = 0;
            if (mv_idx + 1 < insns.size()) {
              const RtlExpr& st_pat = insns[mv_idx + 1].parsed.pattern;
              if (st_pat.op == RtlOp::kSet && st_pat.children.size() == 2 &&
                  st_pat.children[1].op == RtlOp::kReg && !st_pat.children[1].IsArgPointerReg() &&
                  st_pat.children[1].RegNo() == gpr && st_pat.children[1].mode == mv_mode &&
                  extract_stack_mem_range(st_pat.children[0], &st_base, &st_off, &st_sz) &&
                  has_only_dead_note_for(insns[mv_idx + 1].parsed, gpr)) {
                hoist_adj_store = true;
                for (size_t k = i + 1; k < mv_idx && hoist_adj_store; ++k) {
                  if (!SetsAreDisjoint(insns[k].write_regs, insns[mv_idx + 1].read_regs)) {
                    hoist_adj_store = false;
                    break;
                  }
                  if (insns[k].reads_mem || insns[k].writes_mem) {
                    insns[k].parsed.pattern.Visit([&](const RtlExpr& n) {
                      if (n.op == RtlOp::kMem) {
                        int k_base = -1;
                        int64_t k_off = 0;
                        int64_t k_sz = 0;
                        if (extract_stack_mem_range(n, &k_base, &k_off, &k_sz)) {
                          if (k_base != st_base ||
                              (k_off + k_sz > st_off && st_off + st_sz > k_off)) {
                            hoist_adj_store = false;
                          }
                        } else if (insns[k].writes_mem || !n.flags.unchanging) {
                          hoist_adj_store = false;
                        }
                      }
                    });
                  }
                }
              }
            }
            const size_t hoist_end = hoist_adj_store ? (mv_idx + 2) : (mv_idx + 1);
            std::rotate(insns.begin() + static_cast<ptrdiff_t>(i + 1),
                        insns.begin() + static_cast<ptrdiff_t>(mv_idx),
                        insns.begin() + static_cast<ptrdiff_t>(hoist_end));
            any_modified = true;
          }
          break;
        }
      }
      if (insns[mv_idx].has_side_effects || insns[mv_idx].read_regs.contains(64) ||
          insns[mv_idx].read_regs.contains(65) || insns[mv_idx].read_regs.contains(66) ||
          insns[mv_idx].write_regs.contains(64) || insns[mv_idx].write_regs.contains(65) ||
          insns[mv_idx].write_regs.contains(66)) {
        break;
      }
    }
  }

  return any_modified;
}

// Reverses `sched2` hoisting of a 1-cycle callee-saved GPR ALU instruction
// (e.g., a loop induction increment `addiu $s0, $s0, 1`) up across post-call
// `caller-save` restore consumers to immediately follow the `caller-save` stack
// restore loads after a `CALL_INSN`. In `caller-save.c:restore_referenced_regs`,
// restores are emitted lazily immediately before the first post-call instruction
// referencing each saved register; leaving a hoisted callee-saved ALU ahead of
// all restore consumers causes `caller-save` on replay to emit its restores
// after that ALU, shifting `INSN_LUID` tie-break order in `sched2`.
bool UnschedulePostCallHoistedCalleeSavedAlu(std::vector<ParsedBlockInsn>* insns_ptr,
                                             bool frame_pointer_needed) {
  std::vector<ParsedBlockInsn>& insns = *insns_ptr;
  bool any_modified = false;

  auto is_stack_load_into_non_callee_saved = [&](const ParsedBlockInsn& insn,
                                                 int* out_dst) -> bool {
    int dst = -1;
    if (!IsSchedulableLoadInsn(insn, &dst) || IsCalleeSavedMipsHardReg(dst) ||
        dst == kMipsSpRegno || dst == kMipsFpRegno) {
      return false;
    }
    const RtlExpr& mem = insn.parsed.pattern.children[1];
    if (mem.op != RtlOp::kMem || mem.flags.volatil || mem.children.size() != 1) {
      return false;
    }
    const RtlExpr& addr = mem.children[0];
    int base = -1;
    int64_t off = 0;
    if (addr.op == RtlOp::kReg) {
      base = addr.RegNo();
    } else if (addr.op == RtlOp::kPlus && addr.children.size() == 2 &&
               addr.children[0].op == RtlOp::kReg && addr.children[1].op == RtlOp::kConstInt) {
      base = addr.children[0].RegNo();
      off = addr.children[1].int_val;
    } else {
      return false;
    }
    const int stack_base = frame_pointer_needed ? kMipsFpRegno : kMipsSpRegno;
    if ((base != kMipsSpRegno && base != stack_base) || off < 16) {
      return false;
    }
    *out_dst = dst;
    return true;
  };

  auto insn_mentions_reg_in_notes = [](const ParsedBlockInsn& insn,
                                       const absl::flat_hash_set<int>& regs) -> bool {
    for (const RtlRegNote& note : insn.parsed.reg_notes) {
      if (note.datum.op == RtlOp::kReg && !note.datum.IsArgPointerReg() &&
          regs.contains(note.datum.RegNo())) {
        return true;
      }
    }
    return false;
  };

  for (size_t call_idx = 0; call_idx + 3 < insns.size(); ++call_idx) {
    if (!IsCallExpr(insns[call_idx].parsed.pattern)) {
      continue;
    }
    absl::flat_hash_set<int> restored_regs;
    size_t restore_end = call_idx + 1;
    while (restore_end < insns.size()) {
      int r_dst = -1;
      if (!is_stack_load_into_non_callee_saved(insns[restore_end], &r_dst)) {
        break;
      }
      for (int w : insns[restore_end].write_regs) {
        restored_regs.insert(w);
      }
      ++restore_end;
    }
    if (restore_end == call_idx + 1 || restore_end + 1 >= insns.size()) {
      continue;
    }

    int alu_dst = -1;
    const ParsedBlockInsn& alu_insn = insns[restore_end];
    if (!IsSingleCycleIntAluInsn(alu_insn, &alu_dst) || IsConstIntMoveInsn(alu_insn) ||
        alu_insn.read_regs.empty()) {
      continue;
    }
    bool all_callee_saved = true;
    absl::flat_hash_set<int> alu_regs;
    for (int r : alu_insn.read_regs) {
      if (!IsCalleeSavedMipsHardReg(r) || (frame_pointer_needed && r == kMipsFpRegno)) {
        all_callee_saved = false;
        break;
      }
      alu_regs.insert(r);
    }
    for (int r : alu_insn.write_regs) {
      if (!IsCalleeSavedMipsHardReg(r) || (frame_pointer_needed && r == kMipsFpRegno)) {
        all_callee_saved = false;
        break;
      }
      alu_regs.insert(r);
    }
    if (!all_callee_saved || insn_mentions_reg_in_notes(alu_insn, alu_regs)) {
      continue;
    }

    // Check if `restore_end + 1` is an immediately adjacent single-use stack
    // load (e.g., loop-bound input reload) hoisted to feed the same consumer.
    int opt_load_dst = -1;
    const bool has_opt_load =
        (restore_end + 2 < insns.size()) &&
        is_stack_load_into_non_callee_saved(insns[restore_end + 1], &opt_load_dst) &&
        !restored_regs.contains(opt_load_dst) && !alu_regs.contains(opt_load_dst) &&
        SetsAreDisjoint(insns[restore_end + 1].read_regs, alu_regs);

    std::optional<size_t> first_dep;
    bool can_sink_alu = true;
    bool saw_restored_consumer = false;
    for (size_t k = restore_end + 1; k < insns.size(); ++k) {
      if (!SetsAreDisjoint(insns[k].read_regs, alu_regs) ||
          !SetsAreDisjoint(insns[k].write_regs, alu_regs) ||
          insn_mentions_reg_in_notes(insns[k], alu_regs)) {
        first_dep = k;
        break;
      }
      if (insns[k].has_side_effects || IsCallExpr(insns[k].parsed.pattern)) {
        can_sink_alu = false;
        break;
      }
      if (!SetsAreDisjoint(insns[k].read_regs, restored_regs)) {
        saw_restored_consumer = true;
      }
    }
    if (!can_sink_alu || !saw_restored_consumer || !first_dep.has_value() ||
        !IsRealConsumerInsn(insns[*first_dep]) || !insns[*first_dep].read_regs.contains(alu_dst)) {
      continue;
    }

    bool opt_load_dead_at_consumer = HasRegDeadNoteFor(insns[*first_dep], opt_load_dst) ||
                                     insns[*first_dep].write_regs.contains(opt_load_dst);
    if (!opt_load_dead_at_consumer && has_opt_load && opt_load_dst != 2 && opt_load_dst != 3 &&
        *first_dep + 1 < insns.size()) {
      bool read_before_call = false;
      for (size_t k = 0; k < call_idx; ++k) {
        if (insns[k].read_regs.contains(opt_load_dst)) {
          read_before_call = true;
          break;
        }
        if (insns[k].write_regs.contains(opt_load_dst)) {
          break;
        }
      }
      if (!read_before_call) {
        opt_load_dead_at_consumer = true;
      }
    }

    bool sink_pair_with_load = false;
    if (has_opt_load && *first_dep > restore_end + 2 &&
        insns[*first_dep].read_regs.contains(opt_load_dst) && opt_load_dead_at_consumer) {
      absl::flat_hash_set<int> load_regs = insns[restore_end + 1].write_regs;
      sink_pair_with_load = true;
      for (size_t k = restore_end + 2; k < *first_dep; ++k) {
        if (insns[k].writes_mem || !SetsAreDisjoint(insns[k].read_regs, load_regs) ||
            !SetsAreDisjoint(insns[k].write_regs, load_regs) ||
            !SetsAreDisjoint(insns[k].write_regs, insns[restore_end + 1].read_regs) ||
            insn_mentions_reg_in_notes(insns[k], load_regs)) {
          sink_pair_with_load = false;
          break;
        }
      }
      for (size_t k = *first_dep + 1; k < insns.size() && sink_pair_with_load; ++k) {
        if (insns[k].write_regs.contains(opt_load_dst)) {
          break;
        }
        if (insns[k].read_regs.contains(opt_load_dst)) {
          sink_pair_with_load = false;
          break;
        }
      }
    }

    if (sink_pair_with_load) {
      std::rotate(insns.begin() + static_cast<ptrdiff_t>(restore_end),
                  insns.begin() + static_cast<ptrdiff_t>(restore_end + 2),
                  insns.begin() + static_cast<ptrdiff_t>(*first_dep));
      any_modified = true;
    } else if (*first_dep > restore_end + 1) {
      size_t sink_target = *first_dep;
      int prev_load_dst = -1;
      if (sink_target > restore_end + 1 &&
          is_stack_load_into_non_callee_saved(insns[sink_target - 1], &prev_load_dst) &&
          insns[*first_dep].read_regs.contains(prev_load_dst)) {
        --sink_target;
      }
      if (sink_target > restore_end + 1) {
        std::rotate(insns.begin() + static_cast<ptrdiff_t>(restore_end),
                    insns.begin() + static_cast<ptrdiff_t>(restore_end + 1),
                    insns.begin() + static_cast<ptrdiff_t>(sink_target));
        any_modified = true;
      }
    }
  }

  return any_modified;
}

// Reverses `sched2` sinking of `$sp`-based stack stores (`[sp + off]`,
// `off >= 16`) away from their defining 1-cycle integer ALU instructions in a
// pre-call argument-setup sequence (`call_arg_bytes > 16`) when `reload`
// output-spilled at least one outgoing stack-argument expression to
// `[sp + spill_off]` (`spill_off >= call_arg_bytes`) and inherited the reload
// register for `[sp + arg_off]` (`16 <= arg_off < call_arg_bytes`,
// `reload.c:find_equiv_reg` at `reload.c:5651`). Hoisting those sunk stack
// stores back immediately after their defining ALU instructions restores the
// pre-`sched2` (`kAlterReg.post_state`) instruction and `INSN_LUID` order.
bool UnscheduleSunkPreCallStackStores(std::vector<ParsedBlockInsn>* insns_ptr,
                                      bool frame_pointer_needed) {
  std::vector<ParsedBlockInsn>& insns = *insns_ptr;
  bool any_modified = false;

  auto mode_byte_size = [](RtlMachineMode mode) -> int64_t {
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
      default:
        return 8;
    }
  };

  auto extract_call_arg_bytes = [](const RtlExpr& pat) -> int64_t {
    const RtlExpr* call_node = nullptr;
    if (pat.op == RtlOp::kCall) {
      call_node = &pat;
    } else if (pat.op == RtlOp::kSet && pat.children.size() == 2 &&
               pat.children[1].op == RtlOp::kCall) {
      call_node = &pat.children[1];
    } else if (pat.op == RtlOp::kParallel && !pat.children.empty()) {
      const RtlExpr& c0 = pat.children[0];
      if (c0.op == RtlOp::kCall) {
        call_node = &c0;
      } else if (c0.op == RtlOp::kSet && c0.children.size() == 2 &&
                 c0.children[1].op == RtlOp::kCall) {
        call_node = &c0.children[1];
      }
    }
    if (call_node != nullptr && call_node->children.size() >= 2 &&
        call_node->children[1].op == RtlOp::kConstInt) {
      return call_node->children[1].int_val;
    }
    return 0;
  };

  auto match_sp_addr = [](const RtlExpr& addr, int64_t* out_off) -> bool {
    if (addr.op == RtlOp::kReg && addr.RegNo() == kMipsSpRegno) {
      *out_off = 0;
      return true;
    }
    if (addr.op == RtlOp::kPlus && addr.children.size() == 2 &&
        addr.children[0].op == RtlOp::kReg && addr.children[0].RegNo() == kMipsSpRegno &&
        addr.children[1].op == RtlOp::kConstInt && addr.children[1].int_val >= 0) {
      *out_off = addr.children[1].int_val;
      return true;
    }
    return false;
  };

  auto match_sp_reg_store = [&](const ParsedBlockInsn& insn, int64_t* out_off, int64_t* out_size,
                                int* out_src_reg) -> bool {
    if (insn.has_side_effects || insn.reads_mem || !insn.writes_mem) {
      return false;
    }
    const RtlExpr& pat = insn.parsed.pattern;
    if (pat.op != RtlOp::kSet || pat.children.size() != 2) {
      return false;
    }
    const RtlExpr& dst = pat.children[0];
    const RtlExpr& src = pat.children[1];
    if (dst.op != RtlOp::kMem || dst.flags.volatil || dst.children.size() != 1 ||
        src.op != RtlOp::kReg || src.IsArgPointerReg() || dst.mode != src.mode) {
      return false;
    }
    if (!match_sp_addr(dst.children[0], out_off)) {
      return false;
    }
    *out_size = mode_byte_size(dst.mode);
    *out_src_reg = src.RegNo();
    return true;
  };

  auto insn_mentions_reg_in_notes = [](const ParsedBlockInsn& insn, int regno) -> bool {
    for (const RtlRegNote& note : insn.parsed.reg_notes) {
      if (note.datum.op == RtlOp::kReg && !note.datum.IsArgPointerReg() &&
          note.datum.RegNo() == regno) {
        return true;
      }
    }
    return false;
  };

  struct StoreCand {
    size_t idx = 0;
    int64_t off = 0;
    int64_t size = 4;
  };

  auto ranges_disjoint = [](int64_t a_off, int64_t a_sz, int64_t b_off, int64_t b_sz) -> bool {
    return a_off + a_sz <= b_off || b_off + b_sz <= a_off;
  };

  auto extract_general_varying_ptr_base = [&](const RtlExpr& addr) -> int {
    if (addr.op == RtlOp::kLoSum || addr.op == RtlOp::kSymbolRef || addr.op == RtlOp::kLabelRef ||
        addr.op == RtlOp::kConst || addr.op == RtlOp::kConstInt) {
      return -1;
    }
    const RtlExpr* base = &addr;
    if (addr.op == RtlOp::kPlus && addr.children.size() == 2 &&
        addr.children[1].op == RtlOp::kConstInt) {
      base = &addr.children[0];
    }
    if (base->op != RtlOp::kReg || base->IsArgPointerReg()) {
      return -1;
    }
    const int breg = base->RegNo();
    if (breg == kMipsSpRegno || breg == kMipsTemp2Regno ||
        (breg == kMipsFpRegno && frame_pointer_needed)) {
      return -1;
    }
    return (breg >= 1 && breg < kMipsFirstFpr) ? breg : -1;
  };

  auto find_last_prologue_save_idx = [&](size_t limit) -> std::optional<size_t> {
    std::optional<size_t> last_prologue_save_idx;
    bool saw_sp_decrement = false;
    absl::flat_hash_set<int> written_so_far;
    for (size_t i = 0; i < limit; ++i) {
      const RtlExpr& pat = insns[i].parsed.pattern;
      if (IsCallExpr(pat) || pat.op == RtlOp::kReturn ||
          (pat.op == RtlOp::kSet && !pat.children.empty() && pat.children[0].op == RtlOp::kPc)) {
        break;
      }
      if (!saw_sp_decrement && pat.op == RtlOp::kSet && pat.children.size() == 2 &&
          pat.children[0].op == RtlOp::kReg && pat.children[0].RegNo() == kMipsSpRegno &&
          (pat.children[1].op == RtlOp::kMinus || pat.children[1].op == RtlOp::kPlus) &&
          pat.children[1].children.size() == 2 && pat.children[1].children[0].op == RtlOp::kReg &&
          pat.children[1].children[0].RegNo() == kMipsSpRegno) {
        saw_sp_decrement = true;
        written_so_far.insert(kMipsSpRegno);
        continue;
      }
      if (saw_sp_decrement && insns[i].write_regs.contains(kMipsSpRegno)) {
        break;
      }
      int64_t sv_off = 0;
      int64_t sv_sz = 0;
      int sv_reg = -1;
      if (saw_sp_decrement && match_sp_reg_store(insns[i], &sv_off, &sv_sz, &sv_reg) &&
          (sv_reg == kMipsRaRegno || IsCalleeSavedMipsHardReg(sv_reg)) &&
          !written_so_far.contains(sv_reg)) {
        last_prologue_save_idx = i;
      }
      for (int w : insns[i].write_regs) {
        written_so_far.insert(w);
      }
    }
    return last_prologue_save_idx;
  };

  auto compute_unchanging_ptr_regs_per_insn =
      [&](size_t limit) -> std::vector<absl::flat_hash_set<int>> {
    const std::optional<size_t> last_prologue_save_idx = find_last_prologue_save_idx(limit);

    std::vector<absl::flat_hash_set<int>> per_insn(limit);
    absl::flat_hash_set<int> cur_unchanging;
    auto scan_read_expr = [&](auto& self, const RtlExpr& expr, bool before_prologue_save) -> void {
      if (expr.op == RtlOp::kMem && !expr.children.empty()) {
        const int breg = extract_general_varying_ptr_base(expr.children[0]);
        if (!expr.flags.volatil && breg != -1 && (before_prologue_save || expr.flags.unchanging)) {
          cur_unchanging.insert(breg);
        }
        self(self, expr.children[0], before_prologue_save);
        return;
      }
      for (const RtlExpr& ch : expr.children) {
        self(self, ch, before_prologue_save);
      }
    };
    auto scan_stmt = [&](auto& self, const RtlExpr& stmt, bool before_prologue_save) -> void {
      if (stmt.op == RtlOp::kSet && stmt.children.size() == 2) {
        if (stmt.children[0].op == RtlOp::kMem && !stmt.children[0].children.empty()) {
          const int w_breg = extract_general_varying_ptr_base(stmt.children[0].children[0]);
          if (w_breg != -1) {
            cur_unchanging.erase(w_breg);
          }
          scan_read_expr(scan_read_expr, stmt.children[0].children[0], before_prologue_save);
        }
        scan_read_expr(scan_read_expr, stmt.children[1], before_prologue_save);
        return;
      }
      if (stmt.op == RtlOp::kParallel) {
        for (const RtlExpr& elem : stmt.children) {
          self(self, elem, before_prologue_save);
        }
        return;
      }
      if (stmt.op == RtlOp::kClobber) {
        return;
      }
      scan_read_expr(scan_read_expr, stmt, before_prologue_save);
    };

    for (size_t i = 0; i < limit; ++i) {
      const bool before_prologue_save =
          last_prologue_save_idx.has_value() && i < *last_prologue_save_idx;
      const RtlExpr& pat = insns[i].parsed.pattern;
      scan_stmt(scan_stmt, pat, before_prologue_save);
      per_insn[i] = cur_unchanging;

      int preserved_dst = -1;
      if (pat.op == RtlOp::kSet && pat.children.size() == 2 && pat.children[0].op == RtlOp::kReg &&
          !pat.children[0].IsArgPointerReg()) {
        const int dst_reg = pat.children[0].RegNo();
        const RtlExpr& rhs = pat.children[1];
        int src_ptr_reg = -1;
        if (rhs.op == RtlOp::kReg && !rhs.IsArgPointerReg()) {
          src_ptr_reg = rhs.RegNo();
        } else if (rhs.op == RtlOp::kPlus && rhs.children.size() == 2 &&
                   rhs.children[0].op == RtlOp::kReg && !rhs.children[0].IsArgPointerReg() &&
                   rhs.children[1].op == RtlOp::kConstInt) {
          src_ptr_reg = rhs.children[0].RegNo();
        }
        if (dst_reg >= 1 && dst_reg < kMipsFirstFpr && cur_unchanging.contains(src_ptr_reg)) {
          cur_unchanging.insert(dst_reg);
          preserved_dst = dst_reg;
        }
      }
      for (int w : insns[i].write_regs) {
        if (w != preserved_dst) {
          cur_unchanging.erase(w);
        }
      }
      if (IsCallExpr(pat)) {
        for (int r = 1; r < kMipsFirstFpr; ++r) {
          if (!IsCalleeSavedMipsHardReg(r)) {
            cur_unchanging.erase(r);
          }
        }
      }
    }
    return per_insn;
  };

  auto all_read_mems_disjoint_from_stores = [&](auto& self, const RtlExpr& expr,
                                                const absl::flat_hash_set<int>& unchanging_regs,
                                                const std::vector<StoreCand>& stores) -> bool {
    if (expr.op == RtlOp::kMem) {
      if (expr.flags.volatil || expr.children.size() != 1) {
        return false;
      }
      const int breg = extract_general_varying_ptr_base(expr.children[0]);
      const bool is_unchanging_read =
          expr.flags.unchanging || (breg != -1 && unchanging_regs.contains(breg));
      if (!is_unchanging_read) {
        int64_t r_off = 0;
        if (!match_sp_addr(expr.children[0], &r_off)) {
          return false;
        }
        const int64_t r_sz = mode_byte_size(expr.mode);
        for (const StoreCand& st : stores) {
          if (!ranges_disjoint(r_off, r_sz, st.off, st.size)) {
            return false;
          }
        }
      }
      return self(self, expr.children[0], unchanging_regs, stores);
    }
    for (const RtlExpr& ch : expr.children) {
      if (!self(self, ch, unchanging_regs, stores)) {
        return false;
      }
    }
    return true;
  };

  size_t seg_start = 0;
  for (size_t idx = 0; idx < insns.size(); ++idx) {
    if (!IsCallExpr(insns[idx].parsed.pattern)) {
      continue;
    }
    const size_t call_idx = idx;
    const int64_t call_arg_bytes = extract_call_arg_bytes(insns[call_idx].parsed.pattern);
    if (call_arg_bytes <= 16) {
      seg_start = call_idx + 1;
      continue;
    }

    auto collect_hoistable_stores = [&](size_t d, std::vector<StoreCand>* out_stores,
                                        bool* out_has_spill_plus_arg) -> bool {
      out_stores->clear();
      *out_has_spill_plus_arg = false;
      int r = -1;
      if (!IsSingleCycleIntAluInsn(insns[d], &r) || IsConstIntMoveInsn(insns[d]) || r < 1 ||
          r >= kMipsFirstFpr || r == kMipsSpRegno || (frame_pointer_needed && r == kMipsFpRegno)) {
        return false;
      }

      bool saw_spill_store = false;
      bool saw_arg_store = false;
      for (size_t k = d + 1; k < call_idx; ++k) {
        int64_t st_off = 0;
        int64_t st_sz = 0;
        int st_reg = -1;
        if (match_sp_reg_store(insns[k], &st_off, &st_sz, &st_reg) && st_reg == r) {
          if (st_off < 16) {
            return false;
          }
          if (st_off >= call_arg_bytes) {
            saw_spill_store = true;
          } else {
            saw_arg_store = true;
            if (saw_spill_store) {
              *out_has_spill_plus_arg = true;
            }
          }
          out_stores->push_back(StoreCand{k, st_off, st_sz});
          continue;
        }
        if (insns[k].write_regs.contains(r)) {
          break;
        }
        if (insns[k].read_regs.contains(r)) {
          return false;
        }
      }

      if (!saw_arg_store || out_stores->empty() ||
          out_stores->back().idx <= d + out_stores->size()) {
        return false;
      }

      const size_t last_st_idx = out_stores->back().idx;
      size_t st_pos = 0;
      for (size_t k = d + 1; k < last_st_idx; ++k) {
        if (st_pos < out_stores->size() && (*out_stores)[st_pos].idx == k) {
          ++st_pos;
          continue;
        }
        if (insns[k].has_side_effects || insns[k].read_regs.contains(r) ||
            insns[k].write_regs.contains(r) || insns[k].write_regs.contains(kMipsSpRegno) ||
            insn_mentions_reg_in_notes(insns[k], r)) {
          return false;
        }
        if (insns[k].writes_mem) {
          int64_t k_off = 0;
          int64_t k_sz = 0;
          int k_reg = -1;
          if (!match_sp_reg_store(insns[k], &k_off, &k_sz, &k_reg)) {
            return false;
          }
          for (const StoreCand& st : *out_stores) {
            if (!ranges_disjoint(k_off, k_sz, st.off, st.size)) {
              return false;
            }
          }
        }
        if (insns[k].reads_mem) {
          const std::vector<absl::flat_hash_set<int>> unchanging_per_insn =
              compute_unchanging_ptr_regs_per_insn(call_idx);
          if (!all_read_mems_disjoint_from_stores(all_read_mems_disjoint_from_stores,
                                                  insns[k].parsed.pattern, unchanging_per_insn[k],
                                                  *out_stores)) {
            return false;
          }
        }
      }
      return true;
    };

    bool segment_triggered = false;
    for (size_t d = seg_start; d < call_idx; ++d) {
      std::vector<StoreCand> stores;
      bool has_spill_plus_arg = false;
      if (collect_hoistable_stores(d, &stores, &has_spill_plus_arg) && has_spill_plus_arg) {
        segment_triggered = true;
        break;
      }
    }

    if (segment_triggered) {
      std::optional<size_t> max_hoisted_store_idx;
      for (size_t d = seg_start; d < call_idx; ++d) {
        std::vector<StoreCand> stores;
        bool has_spill_plus_arg = false;
        if (!collect_hoistable_stores(d, &stores, &has_spill_plus_arg)) {
          continue;
        }
        for (size_t j = 0; j < stores.size(); ++j) {
          const size_t dst_idx = d + 1 + j;
          const size_t src_idx = stores[j].idx;
          if (src_idx > dst_idx) {
            std::rotate(insns.begin() + static_cast<ptrdiff_t>(dst_idx),
                        insns.begin() + static_cast<ptrdiff_t>(src_idx),
                        insns.begin() + static_cast<ptrdiff_t>(src_idx + 1));
            any_modified = true;
            max_hoisted_store_idx = max_hoisted_store_idx.has_value()
                                        ? std::max(*max_hoisted_store_idx, dst_idx)
                                        : dst_idx;
          }
        }
        d += stores.size();
      }

      // In `sched.c:1740`, `sched_analyze_1` flushes `pending_read_insns` /
      // `pending_write_insns` whenever a `MEM` write observes
      // `pending_lists_length > 32`, setting `last_pending_memory_flush` so no
      // subsequent `MEM` read (`sched.c:1930-1931`, even `RTX_UNCHANGING_P`
      // `mem/u`) can hoist above that write. Conversely, `MEM` reads in
      // `sched_analyze_2` (`sched.c:1935`) never trigger `flush_pending_lists`.
      // When hoisting pre-call stack stores back above an intervening `MEM`
      // read pushes `pending_lists_length > 32` at one of those stores, any
      // prologue-hoisted `mem/u` loads (`l_idx < last_prologue_save_idx`) whose
      // first consumer lies past `max_hoisted_store_idx` must have originated
      // below those stores in `.09.greg`; sink them back before their consumers.
      if (seg_start == 0 && max_hoisted_store_idx.has_value()) {
        int pending_lists_len = 0;
        bool overflowed_pending_lists = false;
        auto count_expr_mems = [&](auto& self, const RtlExpr& e) -> int {
          int cnt = (e.op == RtlOp::kMem) ? 1 : 0;
          for (const RtlExpr& ch : e.children) {
            cnt += self(self, ch);
          }
          return cnt;
        };
        for (size_t i = 0; i < call_idx; ++i) {
          if (insns[i].writes_mem && pending_lists_len > 32) {
            overflowed_pending_lists = true;
            break;
          }
          pending_lists_len += count_expr_mems(count_expr_mems, insns[i].parsed.pattern);
          for (const RtlRegNote& note : insns[i].parsed.reg_notes) {
            if (note.kind == RtlMachineMode::kRegDead && note.datum.op == RtlOp::kMem) {
              ++pending_lists_len;
            }
          }
        }

        std::optional<size_t> last_prologue_save = find_last_prologue_save_idx(call_idx);
        if (overflowed_pending_lists && last_prologue_save.has_value() &&
            *last_prologue_save < *max_hoisted_store_idx) {
          std::optional<size_t> last_arg_store_idx;
          for (size_t k = seg_start; k < call_idx; ++k) {
            int64_t st_off = 0;
            int64_t st_sz = 0;
            int st_reg = -1;
            if (match_sp_reg_store(insns[k], &st_off, &st_sz, &st_reg) && st_off >= 16 &&
                st_off < call_arg_bytes) {
              last_arg_store_idx = k;
            }
          }
          size_t pair_sink_pos = last_arg_store_idx.has_value() ? (*last_arg_store_idx + 1)
                                                                : (*max_hoisted_store_idx + 1);

          for (size_t l_idx = 0; l_idx < *last_prologue_save;) {
            int load_dst = -1;
            if (!IsSchedulableLoadInsn(insns[l_idx], &load_dst) || load_dst < 1 ||
                load_dst >= kMipsFirstFpr || load_dst == kMipsSpRegno ||
                (frame_pointer_needed && load_dst == kMipsFpRegno)) {
              ++l_idx;
              continue;
            }
            RtlExpr* load_rhs = &insns[l_idx].parsed.pattern.children[1];
            if ((load_rhs->op == RtlOp::kSignExtend || load_rhs->op == RtlOp::kZeroExtend) &&
                load_rhs->children.size() == 1) {
              load_rhs = &load_rhs->children[0];
            }
            if (load_rhs->op != RtlOp::kMem || !load_rhs->flags.unchanging ||
                load_rhs->flags.volatil || load_rhs->children.size() != 1) {
              ++l_idx;
              continue;
            }
            const int breg = extract_general_varying_ptr_base(load_rhs->children[0]);
            if (breg == -1 || breg == load_dst) {
              ++l_idx;
              continue;
            }

            std::optional<size_t> consumer_idx;
            bool blocked = false;
            for (size_t k = l_idx + 1; k < call_idx; ++k) {
              if (insns[k].read_regs.contains(load_dst)) {
                consumer_idx = k;
                break;
              }
              if (insns[k].has_side_effects || insns[k].write_regs.contains(load_dst) ||
                  insns[k].write_regs.contains(breg) ||
                  insn_mentions_reg_in_notes(insns[k], load_dst)) {
                blocked = true;
                break;
              }
              if (insns[k].writes_mem) {
                int64_t k_off = 0;
                int64_t k_sz = 0;
                int k_reg = -1;
                if (!match_sp_reg_store(insns[k], &k_off, &k_sz, &k_reg)) {
                  blocked = true;
                  break;
                }
              }
            }
            if (blocked || !consumer_idx.has_value() || *consumer_idx <= *max_hoisted_store_idx) {
              ++l_idx;
              continue;
            }

            bool sink_with_callee_saved_alu = false;
            if (last_arg_store_idx.has_value() && *consumer_idx <= *last_arg_store_idx &&
                pair_sink_pos <= call_idx) {
              int alu_dst = -1;
              if (IsCalleeSavedMipsHardReg(load_dst) &&
                  IsSingleCycleIntAluInsn(insns[*consumer_idx], &alu_dst) && alu_dst == load_dst &&
                  insns[*consumer_idx].read_regs.size() == 1 &&
                  insns[*consumer_idx].write_regs.size() == 1) {
                sink_with_callee_saved_alu = true;
                for (size_t k = *consumer_idx + 1; k < pair_sink_pos; ++k) {
                  if (insns[k].has_side_effects || insns[k].read_regs.contains(load_dst) ||
                      insns[k].write_regs.contains(load_dst) ||
                      insns[k].write_regs.contains(breg) ||
                      insn_mentions_reg_in_notes(insns[k], load_dst)) {
                    sink_with_callee_saved_alu = false;
                    break;
                  }
                  if (insns[k].writes_mem) {
                    int64_t k_off = 0;
                    int64_t k_sz = 0;
                    int k_reg = -1;
                    if (!match_sp_reg_store(insns[k], &k_off, &k_sz, &k_reg)) {
                      sink_with_callee_saved_alu = false;
                      break;
                    }
                  }
                }
              }
              if (!sink_with_callee_saved_alu) {
                ++l_idx;
                continue;
              }
            }

            const size_t check_breg_from =
                sink_with_callee_saved_alu ? pair_sink_pos : *consumer_idx;
            bool breg_used_after_consumer = false;
            for (size_t k = check_breg_from; k < call_idx; ++k) {
              if (insns[k].read_regs.contains(breg) || insns[k].write_regs.contains(breg)) {
                breg_used_after_consumer = true;
                break;
              }
            }
            if (!breg_used_after_consumer) {
              for (size_t m = l_idx + 1; m < check_breg_from; ++m) {
                if (!insns[m].read_regs.contains(breg)) {
                  continue;
                }
                auto& m_notes = insns[m].parsed.reg_notes;
                for (auto it = m_notes.begin(); it != m_notes.end(); ++it) {
                  if (it->kind == RtlMachineMode::kRegDead && it->datum.op == RtlOp::kReg &&
                      !it->datum.IsArgPointerReg() && it->datum.RegNo() == breg) {
                    RtlRegNote moved_note = *it;
                    m_notes.erase(it);
                    if (!HasRegDeadNoteFor(insns[l_idx], breg)) {
                      insns[l_idx].parsed.reg_notes.push_back(std::move(moved_note));
                    }
                    break;
                  }
                }
              }
            }

            if (sink_with_callee_saved_alu) {
              std::rotate(insns.begin() + static_cast<ptrdiff_t>(*consumer_idx),
                          insns.begin() + static_cast<ptrdiff_t>(*consumer_idx + 1),
                          insns.begin() + static_cast<ptrdiff_t>(pair_sink_pos));
              if (last_arg_store_idx.has_value() && *last_arg_store_idx > *consumer_idx &&
                  *last_arg_store_idx < pair_sink_pos) {
                --(*last_arg_store_idx);
              }
              std::rotate(insns.begin() + static_cast<ptrdiff_t>(l_idx),
                          insns.begin() + static_cast<ptrdiff_t>(l_idx + 1),
                          insns.begin() + static_cast<ptrdiff_t>(pair_sink_pos - 1));
              --(*last_prologue_save);
              --(*max_hoisted_store_idx);
              if (last_arg_store_idx.has_value() && *last_arg_store_idx > l_idx &&
                  *last_arg_store_idx < pair_sink_pos - 1) {
                --(*last_arg_store_idx);
              }
            } else {
              std::rotate(insns.begin() + static_cast<ptrdiff_t>(l_idx),
                          insns.begin() + static_cast<ptrdiff_t>(l_idx + 1),
                          insns.begin() + static_cast<ptrdiff_t>(*consumer_idx));
              --(*last_prologue_save);
              --(*max_hoisted_store_idx);
              if (last_arg_store_idx.has_value() && *last_arg_store_idx > l_idx &&
                  *last_arg_store_idx < *consumer_idx) {
                --(*last_arg_store_idx);
              }
              if (pair_sink_pos > l_idx && pair_sink_pos < *consumer_idx) {
                --pair_sink_pos;
              }
            }
            any_modified = true;
          }
        }
      }
    }

    seg_start = call_idx + 1;
  }

  return any_modified;
}

void UnscheduleBlockInstructions(RtlBasicBlock* blk, bool gp64, bool fp64, bool is_sched2 = false,
                                 bool frame_pointer_needed = false, bool runs_sched1 = false) {
  const size_t n = blk->instructions.size();
  if (n < 3) {
    return;
  }

  std::vector<ParsedBlockInsn> insns;
  insns.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    auto parsed_or = ParsePatternAndNotes(blk->instructions[i]);
    if (!parsed_or.ok()) {
      return;
    }
    std::optional<RtlInsn> orig_insn;
    if (i < blk->rtl_insns.size()) {
      orig_insn = blk->rtl_insns[i];
    }
    insns.push_back(BuildParsedBlockInsn(std::move(*parsed_or), gp64, fp64, std::move(orig_insn)));
  }

  bool modified = false;
  if (is_sched2 && UnscheduleDelayedHiLoReloads(&insns, frame_pointer_needed)) {
    modified = true;
  }
  if (is_sched2 && UnschedulePostCallHoistedCalleeSavedAlu(&insns, frame_pointer_needed)) {
    modified = true;
  }
  if (is_sched2 && UnscheduleSunkPreCallStackStores(&insns, frame_pointer_needed)) {
    modified = true;
  }
  if (UnscheduleHoistedFpStoresAndCallArgs(&insns, gp64, fp64)) {
    modified = true;
  }
  if (UnscheduleCalleeSavedFpAluSunkAcrossCall(&insns, gp64, fp64)) {
    modified = true;
  }
  for (size_t i = 0; i + 2 < insns.size(); ++i) {
    int load_regno = -1;
    int alu_regno = -1;
    if (!IsSchedulableLoadInsn(insns[i], &load_regno) ||
        !IsSingleCycleIntAluInsn(insns[i + 1], &alu_regno) || IsDirectSymbolLoadInsn(insns[i]) ||
        IsConstIntMoveInsn(insns[i + 1])) {
      continue;
    }

    // `insns[i]` (load) and `insns[i + 1]` (1-cycle ALU) must be completely
    // register-disjoint so there is no data/anti/output dependence between them
    // and no shared `REG_DEAD` note that would shift between them.
    if (!SetsAreDisjoint(insns[i].read_regs, insns[i + 1].read_regs) ||
        !SetsAreDisjoint(insns[i].read_regs, insns[i + 1].write_regs) ||
        !SetsAreDisjoint(insns[i].write_regs, insns[i + 1].read_regs) ||
        !SetsAreDisjoint(insns[i].write_regs, insns[i + 1].write_regs)) {
      continue;
    }

    const ParsedBlockInsn& consumer = insns[i + 2];
    if (!IsRealConsumerInsn(consumer)) {
      continue;
    }

    bool should_swap = false;
    // Case 1: `insns[i + 2]` directly consumes both `load_regno` and `alu_regno`.
    if (consumer.read_regs.contains(load_regno) && consumer.read_regs.contains(alu_regno) &&
        HasNoLaterDirectDependents(insns, i + 2, insns[i], load_regno, insns[i + 1], alu_regno)) {
      should_swap = true;
    } else if (i + 3 < insns.size()) {
      // Case 2: `insns[i + 2]` is a 1-cycle ALU consumer of `load_regno`
      // producing `mid_regno` (where `load_regno` dies or is overwritten at
      // `insns[i + 2]`), and `insns[i + 3]` consumes both `mid_regno` and
      // `alu_regno`.
      int mid_regno = -1;
      if (consumer.read_regs.contains(load_regno) && !consumer.read_regs.contains(alu_regno) &&
          (consumer.write_regs.contains(load_regno) || HasRegDeadNoteFor(consumer, load_regno)) &&
          IsSingleCycleIntAluInsn(consumer, &mid_regno) &&
          SetsAreDisjoint(consumer.read_regs, insns[i + 1].write_regs) &&
          SetsAreDisjoint(consumer.write_regs, insns[i + 1].read_regs) &&
          SetsAreDisjoint(consumer.write_regs, insns[i + 1].write_regs) &&
          IsRealConsumerInsn(insns[i + 3]) && insns[i + 3].read_regs.contains(mid_regno) &&
          insns[i + 3].read_regs.contains(alu_regno) &&
          HasNoLaterDirectDependents(insns, i + 3, insns[i], load_regno, insns[i + 1], alu_regno)) {
        should_swap = true;
      }
    }

    if (should_swap && is_sched2 && runs_sched1 && load_regno >= 2 && load_regno <= 25 &&
        !insns[i].read_regs.contains(kMipsSpRegno) &&
        !(frame_pointer_needed && insns[i].read_regs.contains(kMipsFpRegno)) &&
        !consumer.write_regs.contains(load_regno)) {
      auto gpr_alloc_ord = [](int hr) -> int {
        static constexpr std::array<int, 16> kCallerSavedGprOrder = {
            2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25};
        for (size_t k = 0; k < kCallerSavedGprOrder.size(); ++k) {
          if (kCallerSavedGprOrder[k] == hr) {
            return static_cast<int>(k);
          }
        }
        return 100;
      };
      const int load_ord = gpr_alloc_ord(load_regno);
      for (const RtlRegNote& n : insns[i + 1].parsed.reg_notes) {
        if (n.kind == RtlMachineMode::kRegDead && n.datum.op == RtlOp::kReg) {
          const int dr = n.datum.RegNo();
          if (dr >= 2 && dr <= 25 && !insns[i + 1].write_regs.contains(dr) &&
              gpr_alloc_ord(dr) < load_ord) {
            should_swap = false;
            break;
          }
        }
      }
    }

    if (should_swap) {
      std::swap(insns[i], insns[i + 1]);
      modified = true;
      ++i;  // Advance past the swapped pair.
    }
  }

  if (modified) {
    bool all_have_orig_rtl = true;
    for (size_t i = 0; i < insns.size(); ++i) {
      blk->instructions[i] = FormatGccPrintRtlPatternAndNotes(
          insns[i].parsed.pattern, insns[i].parsed.reg_notes, insns[i].parsed.bracketed_notes,
          /*call_fusage=*/{});
      if (!insns[i].orig_rtl_insn.has_value()) {
        all_have_orig_rtl = false;
      }
    }
    if (all_have_orig_rtl && blk->rtl_insns.size() == insns.size()) {
      for (size_t i = 0; i < insns.size(); ++i) {
        RtlInsn moved = std::move(*insns[i].orig_rtl_insn);
        moved.canonical_pattern = blk->instructions[i];
        blk->rtl_insns[i] = std::move(moved);
      }
    } else {
      blk->SyncRtlInsnsFromInstructions();
      for (size_t i = 0; i < blk->rtl_insns.size() && i < insns.size(); ++i) {
        if (insns[i].parsed.has_explicit_call_fusage &&
            !blk->rtl_insns[i].has_explicit_call_fusage) {
          blk->rtl_insns[i].has_explicit_call_fusage = true;
          blk->rtl_insns[i].call_fusage = insns[i].parsed.call_fusage;
        }
      }
    }
  }
}

bool IsPrologueSpDecrement(const RtlExpr& pat, int* out_tsize) {
  if (pat.op != RtlOp::kSet || pat.children.size() != 2) {
    return false;
  }
  const RtlExpr& dst = pat.children[0];
  const RtlExpr& src = pat.children[1];
  if (dst.op != RtlOp::kReg || dst.RegNo() != kMipsSpRegno || src.children.size() != 2 ||
      src.children[0].op != RtlOp::kReg || src.children[0].RegNo() != kMipsSpRegno) {
    return false;
  }
  if (src.op == RtlOp::kMinus) {
    if (src.children[1].op == RtlOp::kConstInt && src.children[1].int_val > 0) {
      *out_tsize = static_cast<int>(src.children[1].int_val);
      return true;
    }
    return src.children[1].op == RtlOp::kReg && src.children[1].RegNo() == kMipsTemp1Regno;
  }
  if (src.op == RtlOp::kPlus && src.children[1].op == RtlOp::kConstInt &&
      src.children[1].int_val < 0) {
    *out_tsize = static_cast<int>(-src.children[1].int_val);
    return true;
  }
  return false;
}

bool HasOnlyFlowLivenessNotes(const RtlPatternAndNotes& pn) {
  for (const RtlRegNote& note : pn.reg_notes) {
    if (note.kind != RtlMachineMode::kRegDead && note.kind != RtlMachineMode::kRegUnused) {
      return false;
    }
  }
  return true;
}

bool MatchConstIntSetToReg(const RtlExpr& pat, int regno, int64_t* out_val) {
  if (pat.op != RtlOp::kSet || pat.children.size() != 2) {
    return false;
  }
  const RtlExpr& dst = pat.children[0];
  const RtlExpr& src = pat.children[1];
  if (dst.op != RtlOp::kReg || dst.RegNo() != regno || dst.flags.volatil) {
    return false;
  }
  if (src.op == RtlOp::kConstInt) {
    *out_val = src.int_val;
    return true;
  }
  if ((src.op == RtlOp::kIor || src.op == RtlOp::kPlus) && src.mode == RtlMachineMode::kSi &&
      src.children.size() == 2 && src.children[0].op == RtlOp::kReg &&
      src.children[0].RegNo() == 0 && src.children[1].op == RtlOp::kConstInt) {
    *out_val = src.children[1].int_val;
    return true;
  }
  return false;
}

bool MatchRegIorConstInt(const RtlExpr& pat, int regno, int64_t* out_lo) {
  if (pat.op != RtlOp::kSet || pat.children.size() != 2) {
    return false;
  }
  const RtlExpr& dst = pat.children[0];
  const RtlExpr& src = pat.children[1];
  if (dst.op != RtlOp::kReg || dst.RegNo() != regno || dst.flags.volatil || src.op != RtlOp::kIor ||
      src.children.size() != 2 || src.children[0].op != RtlOp::kReg ||
      src.children[0].RegNo() != regno || src.children[1].op != RtlOp::kConstInt) {
    return false;
  }
  *out_lo = src.children[1].int_val;
  return true;
}

// Matches `< 4`-byte by-value struct/union argument right-shifts emitted by
// `mips.c:function_arg` + `mips_expand_prologue` (`mips.c:2968-2978, 4976-4992`):
//   `(set (reg:SI r) (ashiftrt:SI (reg:SI r) (const_int 24|16|8)))`
bool MatchPrologueStructArgShift(const RtlExpr& pat, int* out_regno,
                                 MipsPrologueArgSpec* out_spec) {
  if (pat.op != RtlOp::kSet || pat.children.size() != 2) {
    return false;
  }
  const RtlExpr& dst = pat.children[0];
  const RtlExpr& src = pat.children[1];
  if (dst.op != RtlOp::kReg || dst.mode != RtlMachineMode::kSi || dst.flags.volatil ||
      dst.flags.in_struct || dst.flags.integrated) {
    return false;
  }
  const int r = dst.RegNo();
  if (r < kMipsFirstArgGpr || r > kMipsLastArgGpr) {
    return false;
  }
  if (src.op != RtlOp::kAshiftrt || src.mode != RtlMachineMode::kSi || src.children.size() != 2 ||
      src.children[0].op != RtlOp::kReg || src.children[0].mode != RtlMachineMode::kSi ||
      src.children[0].RegNo() != r || src.children[0].flags.volatil ||
      src.children[0].flags.in_struct || src.children[0].flags.integrated ||
      src.children[1].op != RtlOp::kConstInt) {
    return false;
  }
  constexpr int kGccQiMode = 1;
  constexpr int kGccHiMode = 2;
  constexpr int kGccBlkMode = 26;
  switch (src.children[1].int_val) {
    case 24:
      *out_spec = MipsPrologueArgSpec{
          .kind = MipsPrologueArgKind::kStructOrUnion,
          .byte_size = 1,
          .align_bits = 8,
          .gcc_mode = kGccQiMode,
      };
      break;
    case 16:
      *out_spec = MipsPrologueArgSpec{
          .kind = MipsPrologueArgKind::kStructOrUnion,
          .byte_size = 2,
          .align_bits = 16,
          .gcc_mode = kGccHiMode,
      };
      break;
    case 8:
      *out_spec = MipsPrologueArgSpec{
          .kind = MipsPrologueArgKind::kStructOrUnion,
          .byte_size = 3,
          .align_bits = 8,
          .gcc_mode = kGccBlkMode,
      };
      break;
    default:
      return false;
  }
  *out_regno = r;
  return true;
}

// Matches a variadic GP argument home store emitted by `mips_expand_prologue`
// (`mips.c:4998-5020`) prior to `$sp` decrement:
//   `(set (mem:SI <sp + 4*(r-4)>) (reg:SI r))` for `r` in `4..7`.
bool MatchPrologueVariadicArgStore(const RtlExpr& pat, int* out_regno) {
  if (pat.op != RtlOp::kSet || pat.children.size() != 2) {
    return false;
  }
  const RtlExpr& dst = pat.children[0];
  const RtlExpr& src = pat.children[1];
  if (dst.op != RtlOp::kMem || dst.children.size() != 1 || dst.flags.volatil ||
      dst.flags.in_struct || src.op != RtlOp::kReg || src.flags.volatil || src.flags.in_struct ||
      dst.mode != src.mode ||
      (dst.mode != RtlMachineMode::kSi && dst.mode != RtlMachineMode::kDi)) {
    return false;
  }
  const int r = src.RegNo();
  if (r < kMipsFirstArgGpr || r > kMipsLastArgGpr) {
    return false;
  }
  const int word_bytes = (dst.mode == RtlMachineMode::kDi) ? 8 : 4;
  const int expected_offset = (r - kMipsFirstArgGpr) * word_bytes;
  const RtlExpr& addr = dst.children[0];
  if (expected_offset == 0) {
    if (addr.op != RtlOp::kReg || addr.RegNo() != kMipsSpRegno) {
      return false;
    }
  } else {
    if (addr.op != RtlOp::kPlus || addr.children.size() != 2 ||
        addr.children[0].op != RtlOp::kReg || addr.children[0].RegNo() != kMipsSpRegno ||
        addr.children[1].op != RtlOp::kConstInt || addr.children[1].int_val != expected_offset) {
      return false;
    }
  }
  *out_regno = r;
  return true;
}

bool MatchPrologueRegPlusSp(const RtlExpr& pat, int dst_regno, int other_regno) {
  if (pat.op != RtlOp::kSet || pat.children.size() != 2) {
    return false;
  }
  const RtlExpr& dst = pat.children[0];
  const RtlExpr& src = pat.children[1];
  if (dst.op != RtlOp::kReg || dst.RegNo() != dst_regno || dst.flags.volatil ||
      src.op != RtlOp::kPlus || src.children.size() != 2 || src.children[0].op != RtlOp::kReg ||
      src.children[1].op != RtlOp::kReg) {
    return false;
  }
  const int r0 = src.children[0].RegNo();
  const int r1 = src.children[1].RegNo();
  return (r0 == other_regno && r1 == kMipsSpRegno) || (r0 == kMipsSpRegno && r1 == other_regno);
}

bool MatchPrologueSaveStoreWithBase(const RtlExpr& pat, bool has_active_base13_offset,
                                    int active_base13_offset, int tsize, int* out_saved_regno,
                                    int* out_save_offset) {
  if (pat.op != RtlOp::kSet || pat.children.size() != 2) {
    return false;
  }
  const RtlExpr& dst = pat.children[0];
  const RtlExpr& src = pat.children[1];
  if (dst.op != RtlOp::kMem || dst.children.size() != 1 || dst.flags.volatil ||
      dst.flags.in_struct || src.op != RtlOp::kReg || src.flags.volatil || src.flags.in_struct) {
    return false;
  }
  const int regno = src.RegNo();
  const bool is_callee_saved_gpr =
      (regno >= kMipsFirstCalleeSavedGpr && regno <= kMipsLastCalleeSavedGpr) ||
      regno == kMipsFpRegno || regno == kMipsRaRegno;
  const bool is_callee_saved_fpr = (regno >= kMipsFirstCalleeSavedFpr && regno <= kMipsLastFpr);
  if (!is_callee_saved_gpr && !is_callee_saved_fpr) {
    return false;
  }

  const RtlExpr& addr = dst.children[0];
  if (addr.op == RtlOp::kReg) {
    if (addr.RegNo() == kMipsSpRegno) {
      *out_saved_regno = regno;
      *out_save_offset = 0;
      return true;
    }
    if (addr.RegNo() == kMipsTemp2Regno && has_active_base13_offset && active_base13_offset >= 0 &&
        active_base13_offset < tsize) {
      *out_saved_regno = regno;
      *out_save_offset = active_base13_offset;
      return true;
    }
    return false;
  }
  if (addr.op == RtlOp::kPlus && addr.children.size() == 2 && addr.children[0].op == RtlOp::kReg &&
      addr.children[1].op == RtlOp::kConstInt) {
    const int base_reg = addr.children[0].RegNo();
    const int disp = static_cast<int>(addr.children[1].int_val);
    if (base_reg == kMipsSpRegno && disp >= 0 && disp < tsize) {
      *out_saved_regno = regno;
      *out_save_offset = disp;
      return true;
    }
    if (base_reg == kMipsTemp2Regno && has_active_base13_offset) {
      const int eff_offset = active_base13_offset + disp;
      if (eff_offset >= 0 && eff_offset < tsize) {
        *out_saved_regno = regno;
        *out_save_offset = eff_offset;
        return true;
      }
    }
  }
  return false;
}

bool IsPrologueFpSetup(const RtlExpr& pat) {
  return pat.op == RtlOp::kSet && pat.children.size() == 2 && pat.children[0].op == RtlOp::kReg &&
         pat.children[0].RegNo() == kMipsFpRegno && !pat.children[0].flags.volatil &&
         pat.children[1].op == RtlOp::kReg && pat.children[1].RegNo() == kMipsSpRegno;
}

bool IsJumpOrReturnExpr(const RtlExpr& pat) {
  if (pat.op == RtlOp::kReturn) {
    return true;
  }
  if (pat.op == RtlOp::kSet && !pat.children.empty() && pat.children[0].op == RtlOp::kPc) {
    return true;
  }
  if (pat.op == RtlOp::kSequence && !pat.children.empty() &&
      pat.children[0].op == RtlOp::kJumpInsn) {
    return true;
  }
  if (pat.op == RtlOp::kParallel) {
    for (const RtlExpr& elem : pat.children) {
      if (IsJumpOrReturnExpr(elem)) {
        return true;
      }
    }
  }
  return false;
}

bool IsUnconditionalJumpOrReturnExpr(const RtlExpr& pat) {
  if (pat.op == RtlOp::kReturn) {
    return true;
  }
  if (pat.op == RtlOp::kSet && pat.children.size() == 2 && pat.children[0].op == RtlOp::kPc &&
      pat.children[1].op != RtlOp::kIfThenElse) {
    return true;
  }
  if (pat.op == RtlOp::kSequence && !pat.children.empty() &&
      pat.children[0].op == RtlOp::kJumpInsn && !pat.children[0].children.empty()) {
    return IsUnconditionalJumpOrReturnExpr(pat.children[0].children[0]);
  }
  if (pat.op == RtlOp::kParallel) {
    for (const RtlExpr& elem : pat.children) {
      if (IsUnconditionalJumpOrReturnExpr(elem)) {
        return true;
      }
    }
  }
  return false;
}

int64_t ScalarMemModeByteSize(RtlMachineMode mode) {
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
    default:
      return 0;
  }
}

bool ByteRangesDisjoint(int64_t off1, int64_t sz1, int64_t off2, int64_t sz2) {
  return (off1 + sz1 <= off2) || (off2 + sz2 <= off1);
}

bool ExtractStackBaseDispMem(const RtlExpr& mem, int* out_base, int64_t* out_off,
                             int64_t* out_size) {
  if (mem.op != RtlOp::kMem || mem.flags.volatil || mem.children.size() != 1) {
    return false;
  }
  const int64_t sz = ScalarMemModeByteSize(mem.mode);
  if (sz <= 0) {
    return false;
  }
  const RtlExpr& addr = mem.children[0];
  int base = -1;
  int64_t off = 0;
  if (addr.op == RtlOp::kReg) {
    base = addr.RegNo();
  } else if (addr.op == RtlOp::kPlus && addr.children.size() == 2 &&
             addr.children[0].op == RtlOp::kReg && addr.children[1].op == RtlOp::kConstInt) {
    base = addr.children[0].RegNo();
    off = addr.children[1].int_val;
  } else {
    return false;
  }
  if ((base != kMipsSpRegno && base != kMipsFpRegno) || off < 0) {
    return false;
  }
  *out_base = base;
  *out_off = off;
  *out_size = sz;
  return true;
}

bool ExprHasOverlappingStackRead(const RtlExpr& expr, bool in_dest, int target_base,
                                 int64_t target_off, int64_t target_sz) {
  if (expr.op == RtlOp::kSet && expr.children.size() == 2) {
    const bool dest_is_mem = (expr.children[0].op == RtlOp::kMem);
    if (dest_is_mem && !expr.children[0].children.empty()) {
      if (ExprHasOverlappingStackRead(expr.children[0].children[0], false, target_base, target_off,
                                      target_sz)) {
        return true;
      }
    } else if (ExprHasOverlappingStackRead(expr.children[0], true, target_base, target_off,
                                           target_sz)) {
      return true;
    }
    return ExprHasOverlappingStackRead(expr.children[1], false, target_base, target_off, target_sz);
  }
  if (expr.op == RtlOp::kParallel) {
    for (const RtlExpr& elem : expr.children) {
      if (ExprHasOverlappingStackRead(elem, false, target_base, target_off, target_sz)) {
        return true;
      }
    }
    return false;
  }
  if (!in_dest && expr.op == RtlOp::kMem) {
    int base = -1;
    int64_t off = 0;
    int64_t sz = 0;
    if (ExtractStackBaseDispMem(expr, &base, &off, &sz)) {
      if (base == target_base && !ByteRangesDisjoint(off, sz, target_off, target_sz)) {
        return true;
      }
    } else if (expr.flags.volatil) {
      return true;
    }
  }
  for (const RtlExpr& child : expr.children) {
    if (ExprHasOverlappingStackRead(child, in_dest, target_base, target_off, target_sz)) {
      return true;
    }
  }
  return false;
}

// In `reload1.c`, when a dying pseudo spilled to stack slot `S` is written via
// reload register `R` (`(set S R)`) and the final consumer `I_use` in the same
// exit block inherits `R` directly instead of reloading `S`, `I_use` retains
// `(expr_list:REG_DEAD S ...)` in `.09.greg` and `.10.sched2`. During `sched2`,
// `sched_analyze_insn` (`sched.c:2095-2102`) passes every `REG_DEAD` datum to
// `sched_analyze_2`, creating a true memory dependence `I_use -> (set S R)` that
// prevents `schedule_select` from sinking the `memory`-unit store past `I_use`.
void RestoreInheritedSpillSlotDeadNotes(RtlControlFlowGraph* cfg, bool gp64, bool fp64) {
  std::vector<std::vector<ParsedBlockInsn>> block_insns(cfg->rpo_blocks.size());
  for (size_t b_idx = 0; b_idx < cfg->rpo_blocks.size(); ++b_idx) {
    const RtlBasicBlock& blk = cfg->rpo_blocks[b_idx];
    block_insns[b_idx].reserve(blk.instructions.size());
    for (const std::string& pat_str : blk.instructions) {
      auto parsed = ParsePatternAndNotes(pat_str);
      if (!parsed.ok()) {
        return;
      }
      block_insns[b_idx].push_back(BuildParsedBlockInsn(std::move(*parsed), gp64, fp64));
    }
  }

  // Per `global.c:find_reg` (`lines 1060-1210`) and `reload1.c:2352`
  // (`alter_reg`), GCC allocates callee-saved registers before spilling a
  // pseudo to a stack slot. A call-free function that uses no callee-saved
  // GPRs (`$16..$23`) cannot have an `alter_reg` GPR spill slot (stack stores
  // there are user locals/aggregates, not spilled pseudos).
  bool has_any_call = false;
  bool uses_callee_saved_gpr = false;
  for (const auto& insns : block_insns) {
    for (const ParsedBlockInsn& insn : insns) {
      if (IsCallExpr(insn.parsed.pattern)) {
        has_any_call = true;
      }
      for (int r : insn.write_regs) {
        if (r >= 16 && r <= 23) {
          uses_callee_saved_gpr = true;
        }
      }
      for (int r : insn.read_regs) {
        if (r >= 16 && r <= 23) {
          uses_callee_saved_gpr = true;
        }
      }
    }
  }

  for (size_t b_idx = 0; b_idx < cfg->rpo_blocks.size(); ++b_idx) {
    RtlBasicBlock& blk = cfg->rpo_blocks[b_idx];
    if (!blk.successors.empty()) {
      continue;
    }
    auto& insns = block_insns[b_idx];
    absl::flat_hash_set<int> defined_in_block;
    for (size_t i = 0; i < insns.size(); ++i) {
      const RtlExpr& pat = insns[i].parsed.pattern;
      int base = -1;
      int64_t off = 0;
      int64_t sz = 0;
      // `function.c:667-735` (`assign_stack_local`) + `reload1.c:2352`
      // (`alter_reg`): compiler-allocated spill slots never carry
      // `MEM_IN_STRUCT_P` (`mem/s`) or `MEM_VOLATILE_P` (`mem/v`).
      if (pat.op == RtlOp::kSet && pat.children.size() == 2 && pat.children[1].op == RtlOp::kReg &&
          !pat.children[0].flags.in_struct && !pat.children[0].flags.volatil &&
          ExtractStackBaseDispMem(pat.children[0], &base, &off, &sz)) {
        const int src_reg = pat.children[1].RegNo();
        if (src_reg >= 1 && src_reg < kMipsHiRegno && src_reg != kMipsSpRegno &&
            src_reg != kMipsFpRegno && src_reg != kMipsRaRegno &&
            (src_reg >= kMipsFirstFpr || has_any_call || uses_callee_saved_gpr) &&
            defined_in_block.contains(src_reg)) {
          bool call_or_stack_read = false;
          for (size_t k = i + 1; k < insns.size(); ++k) {
            if (IsCallExpr(insns[k].parsed.pattern)) {
              call_or_stack_read = true;
              break;
            }
          }
          for (size_t ob = 0; ob < block_insns.size() && !call_or_stack_read; ++ob) {
            for (const ParsedBlockInsn& other : block_insns[ob]) {
              if (ExprHasOverlappingStackRead(other.parsed.pattern, false, base, off, sz)) {
                call_or_stack_read = true;
                break;
              }
            }
          }
          if (!call_or_stack_read) {
            for (size_t j = i + 1; j < insns.size(); ++j) {
              const RtlExpr& jpat = insns[j].parsed.pattern;
              if (jpat.op == RtlOp::kUse || IsJumpOrReturnExpr(jpat)) {
                break;
              }
              if (insns[j].read_regs.contains(src_reg) && HasRegDeadNoteFor(insns[j], src_reg)) {
                bool already_has_mem_dead = false;
                for (const RtlRegNote& note : insns[j].parsed.reg_notes) {
                  if (note.kind == RtlMachineMode::kRegDead && note.datum.op == RtlOp::kMem) {
                    already_has_mem_dead = true;
                    break;
                  }
                }
                if (!already_has_mem_dead) {
                  insns[j].parsed.reg_notes.push_back(RtlRegNote{
                      .kind = RtlMachineMode::kRegDead,
                      .list_op = RtlOp::kExprList,
                      .datum = pat.children[0],
                  });
                  blk.instructions[j] = insns[j].parsed.ToGccPrintRtlString();
                  if (j < blk.rtl_insns.size()) {
                    blk.rtl_insns[j].canonical_pattern = blk.instructions[j];
                  }
                }
                break;
              }
              if (insns[j].write_regs.contains(src_reg) || insns[j].writes_mem) {
                break;
              }
            }
          }
        }
      }
      for (int w : insns[i].write_regs) {
        defined_in_block.insert(w);
      }
    }
  }
}

void PreserveBlockLabelsAndBarriers(RtlControlFlowGraph* cfg) {
  const size_t num_blocks = cfg->rpo_blocks.size();
  if (num_blocks == 0) {
    return;
  }

  std::vector<bool> ends_with_jump(num_blocks, false);
  std::vector<bool> ends_with_uncond_jump(num_blocks, false);
  std::vector<bool> starts_with_note(num_blocks, false);
  std::vector<bool> has_fallthrough_succ(num_blocks, false);

  for (size_t b = 0; b < num_blocks; ++b) {
    const RtlBasicBlock& blk = cfg->rpo_blocks[b];
    if (b + 1 < num_blocks &&
        std::find(blk.successors.begin(), blk.successors.end(), b + 1) != blk.successors.end()) {
      has_fallthrough_succ[b] = true;
    }
    if (!blk.instructions.empty()) {
      auto first_parsed = ParsePatternAndNotes(blk.instructions.front());
      if (first_parsed.ok() && first_parsed->pattern.op == RtlOp::kNote) {
        starts_with_note[b] = true;
      }
      auto last_parsed = ParsePatternAndNotes(blk.instructions.back());
      if (last_parsed.ok()) {
        ends_with_jump[b] = IsJumpOrReturnExpr(last_parsed->pattern);
        ends_with_uncond_jump[b] =
            IsUnconditionalJumpOrReturnExpr(last_parsed->pattern) || !blk.addr_vec_targets.empty();
      }
    }
  }

  auto compute_emits_label = [&]() {
    std::vector<bool> emits(num_blocks, false);
    for (size_t b = 0; b < num_blocks; ++b) {
      if (cfg->rpo_blocks[b].has_label || cfg->rpo_blocks[b].has_unreachable_entry_label) {
        emits[b] = true;
      }
    }
    for (size_t p = 0; p < num_blocks; ++p) {
      for (size_t t : cfg->rpo_blocks[p].addr_vec_targets) {
        if (t < num_blocks) {
          emits[t] = true;
        }
      }
      for (size_t s : cfg->rpo_blocks[p].successors) {
        if (s < num_blocks && (s != p + 1 || ends_with_uncond_jump[p])) {
          emits[s] = true;
        }
      }
    }
    return emits;
  };

  const std::vector<bool> old_emits = compute_emits_label();

  for (size_t b = 1; b < num_blocks; ++b) {
    const bool prev_followed_by_barrier =
        ends_with_uncond_jump[b - 1] || (!ends_with_jump[b - 1] && !has_fallthrough_succ[b - 1]);
    if (cfg->rpo_blocks[b].instructions.empty() ||
        (!ends_with_jump[b - 1] && has_fallthrough_succ[b - 1]) ||
        (prev_followed_by_barrier && !starts_with_note[b])) {
      cfg->rpo_blocks[b].has_label = true;
    }
  }

  for (size_t b = 0; b + 1 < num_blocks; ++b) {
    RtlBasicBlock& blk = cfg->rpo_blocks[b];
    const bool materialize_synthesizes_uncond_jump =
        !ends_with_jump[b] && blk.successors.size() == 1 && blk.successors[0] != b + 1;
    if (!ends_with_uncond_jump[b] && !has_fallthrough_succ[b] &&
        !materialize_synthesizes_uncond_jump) {
      if (!ends_with_jump[b]) {
        blk.has_trailing_barrier = true;
      } else if (!cfg->rpo_blocks[b + 1].has_label) {
        blk.has_trailing_barrier = true;
        RtlBasicBlock& next_blk = cfg->rpo_blocks[b + 1];
        if (next_blk.predecessors.empty() && !starts_with_note[b + 1]) {
          next_blk.has_unreachable_entry_label = true;
        }
      }
    }
  }

  const std::vector<bool> new_emits = compute_emits_label();
  if (new_emits != old_emits) {
    std::vector<std::string> label_remap;
    label_remap.reserve(num_blocks * 2);
    size_t new_idx = 0;
    for (size_t b = 0; b < num_blocks; ++b) {
      if (old_emits[b]) {
        label_remap.push_back("L" + std::to_string(new_idx++));
      } else if (new_emits[b]) {
        ++new_idx;
      }
      if (!cfg->rpo_blocks[b].addr_vec_targets.empty()) {
        label_remap.push_back("L" + std::to_string(new_idx++));
      }
    }
    const ptrdiff_t extra_shift =
        static_cast<ptrdiff_t>(new_idx) - static_cast<ptrdiff_t>(label_remap.size());
    for (size_t b = 0; b < num_blocks; ++b) {
      RtlBasicBlock& blk = cfg->rpo_blocks[b];
      const bool had_synced = (blk.rtl_insns.size() == blk.instructions.size());
      bool blk_mutated = false;
      for (size_t i = 0; i < blk.instructions.size(); ++i) {
        auto parsed = ParsePatternAndNotes(blk.instructions[i]);
        if (!parsed.ok()) {
          continue;
        }
        bool insn_mutated = false;
        auto remap_node = [&](RtlExpr& node) {
          if ((node.op == RtlOp::kLabelRef || node.op == RtlOp::kCodeLabel) &&
              node.str_val.size() >= 2 && node.str_val[0] == 'L') {
            size_t old_num = 0;
            bool digits_ok = true;
            for (size_t ch_i = 1; ch_i < node.str_val.size(); ++ch_i) {
              const char ch = node.str_val[ch_i];
              if (ch < '0' || ch > '9') {
                digits_ok = false;
                break;
              }
              old_num = old_num * 10 + static_cast<size_t>(ch - '0');
            }
            if (!digits_ok) {
              return;
            }
            if (old_num < label_remap.size()) {
              if (label_remap[old_num] != node.str_val) {
                node.str_val = label_remap[old_num];
                insn_mutated = true;
              }
            } else if (extra_shift > 0) {
              node.str_val = "L" + std::to_string(static_cast<size_t>(
                                       static_cast<ptrdiff_t>(old_num) + extra_shift));
              insn_mutated = true;
            }
          }
        };
        parsed->pattern.Mutate(remap_node);
        for (RtlRegNote& note : parsed->reg_notes) {
          note.datum.Mutate(remap_node);
        }
        if (insn_mutated) {
          blk.instructions[i] = FormatGccPrintRtlPatternAndNotes(parsed->pattern, parsed->reg_notes,
                                                                 parsed.value().bracketed_notes,
                                                                 /*call_fusage=*/{});
          if (had_synced) {
            blk.rtl_insns[i].canonical_pattern = blk.instructions[i];
          }
          blk_mutated = true;
        }
      }
      if (blk_mutated && !had_synced) {
        blk.SyncRtlInsnsFromInstructions();
      }
    }
  }
}

void RecoverOutgoingArgsAndFrameSize(RtlControlFlowGraph* cfg, const ParsedCompilerFlags& flags,
                                     bool gp64, bool found_sp_decrement, int tsize,
                                     bool saw_save_store, int min_save_offset,
                                     const std::vector<bool>& entry_strip) {
  const int min_reg_parm_bytes = gp64 ? 32 : 16;
  bool has_call = false;
  int max_call_arg_bytes = 0;
  bool has_dynamic_sp_alloc = (cfg->sideband.frame_pointer_needed && flags.flag_omit_frame_pointer);

  auto inspect_kept_pattern = [&](const RtlExpr& pat) {
    pat.Visit([&](const RtlExpr& node) {
      if (node.op == RtlOp::kCall) {
        has_call = true;
        if (node.children.size() >= 2 && node.children[1].op == RtlOp::kConstInt) {
          max_call_arg_bytes =
              std::max(max_call_arg_bytes, static_cast<int>(node.children[1].int_val));
        }
      }
      if (node.op == RtlOp::kSet && node.children.size() == 2 &&
          node.children[0].op == RtlOp::kReg && node.children[0].RegNo() == kMipsSpRegno &&
          (node.children[1].op == RtlOp::kPlus || node.children[1].op == RtlOp::kMinus) &&
          node.children[1].children.size() == 2 && node.children[1].children[0].op == RtlOp::kReg &&
          node.children[1].children[0].RegNo() == kMipsSpRegno) {
        const RtlExpr& op1 = node.children[1].children[1];
        if (op1.op != RtlOp::kConstInt &&
            (op1.op != RtlOp::kReg || op1.RegNo() != kMipsTemp1Regno)) {
          has_dynamic_sp_alloc = true;
        } else if (cfg->sideband.frame_pointer_needed && op1.op == RtlOp::kConstInt &&
                   ((node.children[1].op == RtlOp::kPlus && op1.int_val < 0) ||
                    (node.children[1].op == RtlOp::kMinus && op1.int_val > 0))) {
          has_dynamic_sp_alloc = true;
        }
      }
    });
  };

  for (size_t b = 0; b < cfg->rpo_blocks.size(); ++b) {
    const RtlBasicBlock& blk = cfg->rpo_blocks[b];
    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      if (b == 0 && i < entry_strip.size() && entry_strip[i]) {
        continue;
      }
      const auto parsed = ParsePatternAndNotes(blk.instructions[i]);
      if (parsed.ok()) {
        inspect_kept_pattern(parsed->pattern);
      }
    }
  }
  if (cfg->sideband.epilogue_delay_pattern.has_value() &&
      !cfg->sideband.epilogue_delay_pattern->empty()) {
    const auto ep_parsed = ParsePatternAndNotes(*cfg->sideband.epilogue_delay_pattern);
    if (ep_parsed.ok()) {
      inspect_kept_pattern(ep_parsed->pattern);
    }
  }

  const int frame_non_save_cap =
      (found_sp_decrement && tsize > 0) ? (saw_save_store ? min_save_offset : tsize) : 0;
  if (!has_call) {
    cfg->sideband.outgoing_args_size = 0;
  } else if (max_call_arg_bytes > 0) {
    cfg->sideband.outgoing_args_size = std::max(min_reg_parm_bytes, (max_call_arg_bytes + 7) & ~7);
  } else {
    cfg->sideband.outgoing_args_size =
        (!found_sp_decrement || frame_non_save_cap >= min_reg_parm_bytes) ? min_reg_parm_bytes
                                                                          : frame_non_save_cap;
  }

  const int args_size = (cfg->sideband.outgoing_args_size + 7) & ~7;
  const int effective_args_size =
      (args_size == 0 && has_dynamic_sp_alloc) ? min_reg_parm_bytes : args_size;
  cfg->sideband.frame_size_bytes =
      (found_sp_decrement && tsize > 0) ? std::max(0, frame_non_save_cap - effective_args_size) : 0;
}

bool IsMipsCallerSavableReg(int r) {
  return (r >= 2 && r <= 15) || r == 24 || r == 25 || (r >= 32 && r <= 51);
}

bool HasStrictInsnOrderHazard(const ParsedBlockInsn& a, const ParsedBlockInsn& b, bool gp64,
                              bool fp64) {
  auto is_non_movable_op = [](RtlOp op) {
    return op == RtlOp::kCodeLabel || op == RtlOp::kBarrier || op == RtlOp::kAddrVec ||
           op == RtlOp::kAddrDiffVec || op == RtlOp::kNote || op == RtlOp::kUse ||
           op == RtlOp::kClobber || op == RtlOp::kSequence || op == RtlOp::kAsmOperands ||
           op == RtlOp::kUnspecVolatile;
  };
  if (a.has_side_effects || b.has_side_effects || is_non_movable_op(a.parsed.pattern.op) ||
      is_non_movable_op(b.parsed.pattern.op) || IsCallExpr(a.parsed.pattern) ||
      IsCallExpr(b.parsed.pattern) || IsJumpOrReturnExpr(a.parsed.pattern) ||
      IsJumpOrReturnExpr(b.parsed.pattern)) {
    return true;
  }
  if (!SetsAreDisjoint(a.write_regs, b.read_regs) || !SetsAreDisjoint(a.read_regs, b.write_regs) ||
      !SetsAreDisjoint(a.write_regs, b.write_regs)) {
    return true;
  }

  auto note_conflicts_with = [&](const ParsedBlockInsn& src, const ParsedBlockInsn& other) {
    for (const RtlRegNote& note : src.parsed.reg_notes) {
      if (note.datum.op == RtlOp::kReg && !note.datum.IsArgPointerReg()) {
        absl::flat_hash_set<int> note_regs;
        InsertRegSpan(note.datum.RegNo(), note.datum.mode, gp64, fp64, &note_regs);
        if (!SetsAreDisjoint(note_regs, other.read_regs) ||
            !SetsAreDisjoint(note_regs, other.write_regs)) {
          return true;
        }
      } else if (note.kind == RtlMachineMode::kRegDead && note.datum.op == RtlOp::kMem &&
                 (other.writes_mem || other.reads_mem)) {
        int nb = -1, ob = -1;
        int64_t noff = 0, nsz = 0, ooff = 0, osz = 0;
        const RtlExpr* other_mem =
            (other.parsed.pattern.op == RtlOp::kSet && other.parsed.pattern.children.size() == 2)
                ? (other.parsed.pattern.children[0].op == RtlOp::kMem
                       ? &other.parsed.pattern.children[0]
                       : (other.parsed.pattern.children[1].op == RtlOp::kMem
                              ? &other.parsed.pattern.children[1]
                              : nullptr))
                : nullptr;
        const bool non_conflicting_unchanging_read =
            other_mem != nullptr && !other.writes_mem && other_mem->flags.unchanging &&
            !other_mem->flags.volatil && !note.datum.flags.unchanging && !note.datum.flags.volatil;
        if (!non_conflicting_unchanging_read &&
            (other_mem == nullptr || !ExtractStackBaseDispMem(note.datum, &nb, &noff, &nsz) ||
             !ExtractStackBaseDispMem(*other_mem, &ob, &ooff, &osz) || nb != ob ||
             !ByteRangesDisjoint(noff, nsz, ooff, osz))) {
          return true;
        }
      }
    }
    return false;
  };
  if (note_conflicts_with(a, b) || note_conflicts_with(b, a)) {
    return true;
  }

  if ((a.writes_mem && (b.reads_mem || b.writes_mem)) ||
      (b.writes_mem && (a.reads_mem || a.writes_mem))) {
    auto extract_single_mem = [](const RtlExpr& pat, bool* is_store) -> const RtlExpr* {
      if (pat.op != RtlOp::kSet || pat.children.size() != 2) {
        return nullptr;
      }
      const bool d_mem = (pat.children[0].op == RtlOp::kMem);
      const bool s_mem = (pat.children[1].op == RtlOp::kMem);
      if (d_mem == s_mem) {
        return nullptr;
      }
      *is_store = d_mem;
      return d_mem ? &pat.children[0] : &pat.children[1];
    };
    bool a_store = false;
    bool b_store = false;
    const RtlExpr* ma = extract_single_mem(a.parsed.pattern, &a_store);
    const RtlExpr* mb = extract_single_mem(b.parsed.pattern, &b_store);
    if (ma != nullptr && mb != nullptr && !ma->flags.volatil && !mb->flags.volatil &&
        a_store != b_store) {
      const RtlExpr& store_mem = a_store ? *ma : *mb;
      const RtlExpr& load_mem = a_store ? *mb : *ma;
      int s_base = -1;
      int64_t s_off = 0, s_sz = 0;
      // GCC `sched.c` (`true_dependence` / `anti_dependence`): an unchanging
      // `mem/u` read never conflicts with a non-unchanging stack-slot store.
      if (load_mem.flags.unchanging && !store_mem.flags.unchanging &&
          ExtractStackBaseDispMem(store_mem, &s_base, &s_off, &s_sz)) {
        return false;
      }
    }
    int ba = -1, bb = -1;
    int64_t oa = 0, sa = 0, ob = 0, sb = 0;
    if (ma == nullptr || mb == nullptr || !ExtractStackBaseDispMem(*ma, &ba, &oa, &sa) ||
        !ExtractStackBaseDispMem(*mb, &bb, &ob, &sb) || ba != bb ||
        !ByteRangesDisjoint(oa, sa, ob, sb)) {
      return true;
    }
  }
  return false;
}

void IdentifySched2CallerSaveAndSpillOffsets(
    const RtlControlFlowGraph& cfg, const std::vector<std::vector<ParsedBlockInsn>>& block_insns,
    bool gp64, bool fp64, int* out_min_prologue_save_off,
    absl::flat_hash_set<int64_t>* caller_save_offsets,
    absl::flat_hash_set<int64_t>* proven_reload_spill_offsets) {
  const size_t n_blocks = block_insns.size();
  int tsize = 0;
  bool found_sp_dec = false;
  size_t sp_dec_idx = 0;
  for (size_t i = 0; i < block_insns[0].size(); ++i) {
    if (IsPrologueSpDecrement(block_insns[0][i].parsed.pattern, &tsize) && tsize > 0) {
      found_sp_dec = true;
      sp_dec_idx = i;
      break;
    }
  }

  int min_prologue_save_off = tsize;
  absl::flat_hash_set<int> prologue_save_indices;
  absl::flat_hash_set<int> saved_gprs(cfg.sideband.saved_gpr_regnos.begin(),
                                      cfg.sideband.saved_gpr_regnos.end());
  absl::flat_hash_set<int> saved_fprs(cfg.sideband.saved_fpr_regnos.begin(),
                                      cfg.sideband.saved_fpr_regnos.end());
  if (found_sp_dec) {
    absl::flat_hash_set<int> written_or_saved_regs;
    for (size_t i = sp_dec_idx + 1; i < block_insns[0].size(); ++i) {
      const RtlExpr& pat = block_insns[0][i].parsed.pattern;
      if (IsCallExpr(pat) || IsJumpOrReturnExpr(pat)) {
        break;
      }
      int sbase = -1;
      int64_t save_off = 0;
      int64_t save_sz = 0;
      if (pat.IsSet() && pat.SetSrc().IsReg() &&
          ExtractStackBaseDispMem(pat.SetDest(), &sbase, &save_off, &save_sz) &&
          sbase == kMipsSpRegno && save_off >= 0 && save_off < tsize) {
        const int saved_reg = pat.SetSrc().RegNo();
        const int nw = std::max(1, MipsHardRegNregs(saved_reg, pat.SetSrc().mode, gp64, fp64));
        bool already_touched = false;
        for (int w = 0; w < nw; ++w) {
          if (written_or_saved_regs.contains(saved_reg + w)) {
            already_touched = true;
            break;
          }
        }
        if (!already_touched && ((saved_reg >= 16 && saved_reg <= 23) || saved_reg == 30 ||
                                 saved_reg == 31 || (saved_reg >= 52 && saved_reg <= 63))) {
          prologue_save_indices.insert(static_cast<int>(i));
          min_prologue_save_off = std::min(min_prologue_save_off, static_cast<int>(save_off));
          for (int w = 0; w < nw; ++w) {
            written_or_saved_regs.insert(saved_reg + w);
            if (saved_reg + w < 32) {
              saved_gprs.insert(saved_reg + w);
            } else if (saved_reg + w < 64) {
              saved_fprs.insert(saved_reg + w);
            }
          }
        }
      }
      for (int w : block_insns[0][i].write_regs) {
        written_or_saved_regs.insert(w);
      }
    }
  } else if (cfg.sideband.frame_size_bytes > 0 || cfg.sideband.outgoing_args_size > 0) {
    min_prologue_save_off = cfg.sideband.outgoing_args_size + cfg.sideband.frame_size_bytes;
  }
  *out_min_prologue_save_off = min_prologue_save_off;

  bool all_saved_gprs_16_23 = true;
  for (int r = 16; r <= 23; ++r) {
    if (!saved_gprs.contains(r)) {
      all_saved_gprs_16_23 = false;
      break;
    }
  }
  const bool all_saved_gprs_used =
      all_saved_gprs_16_23 && (cfg.sideband.frame_pointer_needed || saved_gprs.contains(30));
  bool all_saved_fprs_used = true;
  for (int r = 52; r <= 62; r += 2) {
    if (!saved_fprs.contains(r)) {
      all_saved_fprs_used = false;
      break;
    }
  }

  if (cfg.sideband.frame_pointer_needed || (!found_sp_dec && cfg.sideband.frame_size_bytes <= 0) ||
      min_prologue_save_off <= 0) {
    return;
  }

  struct SpOffSummary {
    int n_stores = 0;
    int n_loads = 0;
    int64_t sz = 0;
    RtlMachineMode mode = RtlMachineMode::kNone;
    bool all_reg_moves = true;
    int uniform_reg = -2;
    std::vector<std::pair<size_t, size_t>> store_locs;
    std::vector<std::pair<size_t, size_t>> load_locs;
  };
  absl::flat_hash_map<int64_t, SpOffSummary> off_map;
  absl::flat_hash_set<int64_t> invalidated_buckets;

  auto scan_non_mem_sp = [&](const auto& self, const RtlExpr& e, bool inside_mem) -> void {
    if (e.op == RtlOp::kMem) {
      for (const RtlExpr& ch : e.children) {
        self(self, ch, true);
      }
      return;
    }
    if (!inside_mem && e.op == RtlOp::kPlus && e.children.size() == 2 &&
        e.children[0].op == RtlOp::kReg && e.children[0].RegNo() == kMipsSpRegno &&
        e.children[1].op == RtlOp::kConstInt) {
      const int64_t off = e.children[1].int_val;
      if (off >= 0 && off < min_prologue_save_off) {
        invalidated_buckets.insert((off / 8) * 8);
      }
    }
    for (const RtlExpr& ch : e.children) {
      self(self, ch, inside_mem);
    }
  };

  for (size_t b = 0; b < n_blocks; ++b) {
    for (size_t i = 0; i < block_insns[b].size(); ++i) {
      if (b == 0 && ((found_sp_dec && i <= sp_dec_idx) ||
                     prologue_save_indices.contains(static_cast<int>(i)))) {
        continue;
      }
      const RtlExpr& pat = block_insns[b][i].parsed.pattern;
      scan_non_mem_sp(scan_non_mem_sp, pat, false);
      pat.Visit([&](const RtlExpr& n) {
        if (n.op != RtlOp::kMem) {
          return;
        }
        int base = -1;
        int64_t off = 0, sz = 0;
        if (!ExtractStackBaseDispMem(n, &base, &off, &sz)) {
          if (n.children.size() == 1 &&
              ((n.children[0].op == RtlOp::kReg && n.children[0].RegNo() == kMipsSpRegno) ||
               (n.children[0].op == RtlOp::kPlus && n.children[0].children.size() == 2 &&
                n.children[0].children[0].op == RtlOp::kReg &&
                n.children[0].children[0].RegNo() == kMipsSpRegno))) {
            for (int64_t bk = 0; bk < min_prologue_save_off; bk += 8) {
              invalidated_buckets.insert(bk);
            }
          }
          return;
        }
        if (base != kMipsSpRegno || off < 0 || off >= min_prologue_save_off) {
          return;
        }
        const int64_t bk = (off / 8) * 8;
        if (sz <= 0 || ((off + sz - 1) / 8) * 8 != bk) {
          invalidated_buckets.insert(bk);
          return;
        }
        const bool is_simple_store = pat.op == RtlOp::kSet && pat.children.size() == 2 &&
                                     &pat.children[0] == &n && pat.children[1].op == RtlOp::kReg &&
                                     !pat.children[1].flags.integrated;
        const bool is_simple_load = pat.op == RtlOp::kSet && pat.children.size() == 2 &&
                                    &pat.children[1] == &n && pat.children[0].op == RtlOp::kReg &&
                                    !pat.children[0].flags.integrated;
        SpOffSummary& sum = off_map[off];
        if (sum.mode == RtlMachineMode::kNone) {
          sum.mode = n.mode;
          sum.sz = sz;
        } else if (sum.mode != n.mode || sum.sz != sz) {
          invalidated_buckets.insert(bk);
        }
        if (is_simple_store) {
          ++sum.n_stores;
          sum.store_locs.emplace_back(b, i);
          const int r = pat.children[1].RegNo();
          sum.uniform_reg = (sum.uniform_reg == -2) ? r : (sum.uniform_reg == r ? r : -1);
        } else if (is_simple_load) {
          ++sum.n_loads;
          sum.load_locs.emplace_back(b, i);
          const int r = pat.children[0].RegNo();
          sum.uniform_reg = (sum.uniform_reg == -2) ? r : (sum.uniform_reg == r ? r : -1);
        } else {
          sum.all_reg_moves = false;
          invalidated_buckets.insert(bk);
        }
      });
    }
  }

  absl::flat_hash_map<int64_t, std::vector<int64_t>> bucket_offsets;
  for (const auto& [off, _] : off_map) {
    bucket_offsets[(off / 8) * 8].push_back(off);
  }

  absl::flat_hash_map<int64_t, bool> raw_valid_cs;
  for (const auto& [off, sum] : off_map) {
    const int64_t bk = (off / 8) * 8;
    if (invalidated_buckets.contains(bk) || !sum.all_reg_moves || sum.n_stores < 1 ||
        sum.n_loads < 1 || !IsMipsCallerSavableReg(sum.uniform_reg)) {
      continue;
    }
    const int cs_r = sum.uniform_reg;
    bool valid_cs = true;
    for (const auto& [b, s_idx] : sum.store_locs) {
      bool found_call = false;
      for (size_t k = s_idx + 1; k < block_insns[b].size(); ++k) {
        if (IsCallExpr(block_insns[b][k].parsed.pattern)) {
          if (CallInsnReadsArgReg(block_insns[b][k], cs_r, gp64, fp64)) {
            valid_cs = false;
          }
          found_call = true;
          break;
        }
        if (block_insns[b][k].read_regs.contains(cs_r) ||
            block_insns[b][k].write_regs.contains(cs_r)) {
          valid_cs = false;
          break;
        }
      }
      if (!found_call) {
        valid_cs = false;
        break;
      }
      const bool bank_exhausted = (cs_r < 32) ? all_saved_gprs_used : all_saved_fprs_used;
      if (bucket_offsets[bk].size() == 1 && bank_exhausted && (sum.sz != 4 || (off % 8) != 0)) {
        bool read_before_save = false;
        for (size_t prev = s_idx; prev > 0; --prev) {
          const auto& pin = block_insns[b][prev - 1];
          if (pin.read_regs.contains(cs_r) && !pin.write_regs.contains(cs_r)) {
            read_before_save = true;
            break;
          }
          if (pin.write_regs.contains(cs_r)) {
            break;
          }
        }
        if (!read_before_save) {
          const auto lit = cfg.sideband.basic_block_live_at_start.find(static_cast<int>(b));
          if (lit == cfg.sideband.basic_block_live_at_start.end() || !lit->second.contains(cs_r)) {
            valid_cs = false;
            break;
          }
        }
      }
    }
    for (const auto& [b, l_idx] : sum.load_locs) {
      if (!valid_cs) {
        break;
      }
      bool found_call = false;
      for (size_t prev = l_idx; prev > 0; --prev) {
        const auto& pin = block_insns[b][prev - 1];
        if (IsCallExpr(pin.parsed.pattern)) {
          if (CallInsnSetsReturnReg(pin, cs_r, gp64, fp64)) {
            valid_cs = false;
          }
          found_call = true;
          break;
        }
        if (pin.read_regs.contains(cs_r) || pin.write_regs.contains(cs_r)) {
          valid_cs = false;
          break;
        }
      }
      if (!found_call) {
        valid_cs = false;
      }
    }
    if (valid_cs) {
      raw_valid_cs[off] = true;
    }
  }

  const int64_t prologue_bucket = (min_prologue_save_off / 8) * 8;
  int64_t min_caller_save_bucket = prologue_bucket;
  int64_t max_caller_save_bucket = -1;
  for (auto& [bk, offs] : bucket_offsets) {
    if (invalidated_buckets.contains(bk) || offs.empty()) {
      continue;
    }
    std::sort(offs.begin(), offs.end());
    bool bucket_ok = true;
    for (size_t idx = 0; idx < offs.size(); ++idx) {
      if (!raw_valid_cs[offs[idx]]) {
        bucket_ok = false;
        break;
      }
      if (idx + 1 < offs.size() && offs[idx] + off_map.at(offs[idx]).sz > offs[idx + 1]) {
        bucket_ok = false;
        break;
      }
    }
    if (bucket_ok) {
      for (int64_t o : offs) {
        caller_save_offsets->insert(o);
      }
      min_caller_save_bucket = std::min(min_caller_save_bucket, bk);
      max_caller_save_bucket = std::max(max_caller_save_bucket, bk);
    }
  }

  auto try_add_spill_bucket = [&](int64_t bk) -> bool {
    if (invalidated_buckets.contains(bk)) {
      return false;
    }
    const auto bit = bucket_offsets.find(bk);
    if (bit == bucket_offsets.end()) {
      return all_saved_gprs_used || all_saved_fprs_used || all_saved_gprs_16_23;
    }
    if (bit->second.size() != 1) {
      return false;
    }
    const int64_t off = bit->second[0];
    const SpOffSummary& sum = off_map.at(off);
    if (!sum.all_reg_moves || sum.n_stores < 1 || sum.n_loads < 1 || off != bk + (8 - sum.sz)) {
      return false;
    }
    const bool is_fp = (sum.mode == RtlMachineMode::kSf || sum.mode == RtlMachineMode::kDf);
    const bool is_di_gpr = (!gp64 && sum.mode == RtlMachineMode::kDi);
    if (is_fp ? !all_saved_fprs_used : (is_di_gpr ? !all_saved_gprs_16_23 : !all_saved_gprs_used)) {
      return false;
    }
    proven_reload_spill_offsets->insert(off);
    return true;
  };

  // Zone 1 (`reload1.c:658, 879, 2352`): pre-`setup_save_areas` `alter_reg`
  // spill slots (`align = -1` -> 8-byte bucket, big-endian `bk + (8 - sz)`).
  const int64_t min_spill_bucket =
      ((std::max<int64_t>(0, cfg.sideband.outgoing_args_size) + 7) / 8) * 8;
  for (int64_t bk = min_caller_save_bucket - 8; bk >= min_spill_bucket; bk -= 8) {
    if (!try_add_spill_bucket(bk)) {
      break;
    }
  }

  // Zone 3 (`reload1.c:1446, 3499, 2352`): post-`setup_save_areas` reload-loop
  // `spill_hard_reg` -> `alter_reg` spill slots allocated above the caller-save
  // region and below the prologue register-save area.
  if (max_caller_save_bucket >= 0) {
    for (int64_t bk = max_caller_save_bucket + 8; bk < prologue_bucket; bk += 8) {
      if (!try_add_spill_bucket(bk)) {
        break;
      }
    }
  }
}

bool StripEarlyclobberDiOutputReloadDeadNotesInBlock(
    const RtlControlFlowGraph& cfg, size_t b, bool gp64, bool fp64,
    const absl::flat_hash_set<int>& use_pinned_regs, std::vector<ParsedBlockInsn>* block_ptr) {
  if (gp64) {
    return false;
  }
  auto& insns = *block_ptr;
  bool changed = false;
  auto is_dead_after = [&](size_t s, int scr_hr) {
    for (int w : {scr_hr, scr_hr + 1}) {
      if (HasRegDeadNoteFor(insns[s], w) && !insns[s].write_regs.contains(w)) {
        // Check actual post-`s` liveness below so we only strip when truly dead.
      }
      bool killed = false;
      for (size_t k = s + 1; k < insns.size(); ++k) {
        if (insns[k].read_regs.contains(w) ||
            (IsCallExpr(insns[k].parsed.pattern) && CallInsnReadsArgReg(insns[k], w, gp64, fp64))) {
          return false;
        }
        if (insns[k].write_regs.contains(w) ||
            (IsCallExpr(insns[k].parsed.pattern) && IsMipsCallerSavableReg(w))) {
          killed = true;
          break;
        }
      }
      if (!killed) {
        for (size_t succ : cfg.rpo_blocks[b].successors) {
          const auto it = cfg.sideband.basic_block_live_at_start.find(static_cast<int>(succ));
          if (it != cfg.sideband.basic_block_live_at_start.end() && it->second.contains(w)) {
            return false;
          }
        }
        if (cfg.rpo_blocks[b].successors.empty() && (w == 2 || w == 3)) {
          return false;
        }
      }
    }
    return true;
  };

  for (size_t s = 1; s < insns.size(); ++s) {
    const RtlExpr& mv = insns[s].parsed.pattern;
    if (!mv.IsSet() || !mv.SetDest().IsHardReg() || mv.SetDest().mode != RtlMachineMode::kDi ||
        !mv.SetSrc().IsHardReg() || mv.SetSrc().mode != RtlMachineMode::kDi) {
      continue;
    }
    const int dst_hr = mv.SetDest().RegNo();
    const int scr_hr = mv.SetSrc().RegNo();
    if (dst_hr < 2 || dst_hr > 24 || scr_hr < 2 || scr_hr > 24 || dst_hr == scr_hr ||
        use_pinned_regs.contains(scr_hr) || !is_dead_after(s, scr_hr)) {
      continue;
    }
    const absl::flat_hash_set<int> scr_words = {scr_hr, scr_hr + 1};
    const absl::flat_hash_set<int> dst_words = {dst_hr, dst_hr + 1};
    for (size_t rev = s; rev > 0; --rev) {
      const size_t p = rev - 1;
      if (!SetsAreDisjoint(insns[p].write_regs, scr_words)) {
        const RtlExpr& pp = insns[p].parsed.pattern;
        if (pp.op == RtlOp::kParallel && pp.children.size() >= 2 && pp.children[0].IsSet() &&
            pp.children[1].op == RtlOp::kClobber && pp.children[0].SetDest().IsHardReg() &&
            pp.children[0].SetDest().mode == RtlMachineMode::kDi &&
            pp.children[0].SetDest().RegNo() == scr_hr &&
            SetsAreDisjoint(insns[p].read_regs, scr_words)) {
          bool clobber_overlaps_dst = false;
          for (size_t c = 1; c < pp.children.size(); ++c) {
            if (pp.children[c].op == RtlOp::kClobber && !pp.children[c].children.empty() &&
                pp.children[c].children[0].IsHardReg()) {
              const int chr = pp.children[c].children[0].RegNo();
              const int cnw = MipsHardRegNregs(chr, pp.children[c].children[0].mode, gp64, fp64);
              if (std::max(dst_hr, chr) < std::min(dst_hr + 2, chr + cnw)) {
                clobber_overlaps_dst = true;
              }
            }
          }
          const RtlExpr& rhs = pp.children[0].SetSrc();
          const bool plus_imm_same_dst =
              rhs.op == RtlOp::kPlus && rhs.children.size() == 2 && rhs.children[0].IsHardReg() &&
              rhs.children[0].RegNo() == dst_hr && rhs.children[1].IsConstInt();
          if (!clobber_overlaps_dst &&
              ((rhs.op == RtlOp::kPlus && !plus_imm_same_dst) || rhs.op == RtlOp::kMinus ||
               rhs.op == RtlOp::kAshift || rhs.op == RtlOp::kAshiftrt ||
               rhs.op == RtlOp::kLshiftrt) &&
              !SetsAreDisjoint(insns[p].read_regs, dst_words)) {
            auto& notes = insns[s].parsed.reg_notes;
            const size_t before = notes.size();
            notes.erase(std::remove_if(notes.begin(), notes.end(),
                                       [scr_hr](const RtlRegNote& rn) {
                                         return rn.kind == RtlMachineMode::kRegDead &&
                                                rn.datum.IsReg() && rn.datum.RegNo() == scr_hr;
                                       }),
                        notes.end());
            if (notes.size() != before) {
              changed = true;
            }
          }
        }
        break;
      }
      if (!SetsAreDisjoint(insns[p].read_regs, scr_words) || IsCallExpr(insns[p].parsed.pattern) ||
          IsJumpOrReturnExpr(insns[p].parsed.pattern)) {
        break;
      }
    }
  }
  return changed;
}

bool NormalizeSpillOutputReloadRegNotesInBlock(
    const RtlControlFlowGraph& cfg, size_t b, bool gp64, bool fp64,
    const absl::flat_hash_set<int64_t>& proven_reload_spill_offsets,
    std::vector<ParsedBlockInsn>* block_ptr) {
  if (proven_reload_spill_offsets.empty()) {
    return false;
  }
  auto& insns = *block_ptr;
  bool changed = false;
  auto extract_sp_off = [](const RtlExpr& mem, int64_t* out_off) {
    int base = -1;
    int64_t sz = 0;
    return ExtractStackBaseDispMem(mem, &base, out_off, &sz) && base == kMipsSpRegno;
  };
  for (size_t s = 1; s < insns.size(); ++s) {
    const RtlExpr& spat = insns[s].parsed.pattern;
    int64_t off = 0;
    if (!spat.IsSet() || !spat.SetSrc().IsHardReg() || !extract_sp_off(spat.SetDest(), &off) ||
        !proven_reload_spill_offsets.contains(off)) {
      continue;
    }
    const int scr = spat.SetSrc().RegNo();
    if (scr < 2 || scr > 63 || scr == 28 || scr == 29 ||
        (cfg.sideband.frame_pointer_needed && scr == 30)) {
      continue;
    }
    absl::flat_hash_set<int> scr_words;
    InsertRegSpan(scr, spat.SetSrc().mode, gp64, fp64, &scr_words);

    bool saw_read_between = false;
    for (size_t prev = s; prev > 0; --prev) {
      const size_t pi = prev - 1;
      if (!SetsAreDisjoint(insns[pi].write_regs, scr_words)) {
        const RtlExpr& pdef = insns[pi].parsed.pattern;
        if (pdef.IsSet() && !pdef.SetSrc().IsReg() && !IsCallExpr(pdef) &&
            !IsJumpOrReturnExpr(pdef)) {
          {
            auto& snotes = insns[s].parsed.reg_notes;
            const size_t before = snotes.size();
            snotes.erase(std::remove_if(snotes.begin(), snotes.end(),
                                        [scr](const RtlRegNote& rn) {
                                          return rn.kind == RtlMachineMode::kRegDead &&
                                                 rn.datum.IsReg() && rn.datum.RegNo() == scr;
                                        }),
                         snotes.end());
            if (snotes.size() != before) {
              changed = true;
            }
          }
          if (!saw_read_between && !SetsAreDisjoint(insns[pi].read_regs, scr_words) &&
              !HasRegDeadNoteFor(insns[pi], scr)) {
            bool dying_pseudo = false;
            bool decided = false;
            for (size_t rev_e = pi; rev_e > 0; --rev_e) {
              const size_t ei = rev_e - 1;
              const RtlExpr& epat = insns[ei].parsed.pattern;
              int64_t moff = 0;
              if (epat.IsSet() && epat.SetSrc().IsHardReg() && epat.SetSrc().RegNo() == scr &&
                  extract_sp_off(epat.SetDest(), &moff) &&
                  proven_reload_spill_offsets.contains(moff)) {
                decided = true;
                break;
              }
              if (!SetsAreDisjoint(insns[ei].write_regs, scr_words)) {
                decided = true;
                if (IsCallExpr(epat)) {
                  bool sets_scr = false;
                  auto chk = [&](const RtlExpr& st) {
                    if (st.IsSet() && st.SetDest().IsHardReg() && st.SetDest().RegNo() == scr) {
                      sets_scr = true;
                    }
                  };
                  if (epat.op == RtlOp::kParallel) {
                    for (const RtlExpr& ch : epat.children) chk(ch);
                  } else {
                    chk(epat);
                  }
                  dying_pseudo = sets_scr;
                  break;
                }
                if (epat.IsSet()) {
                  const RtlExpr& esrc = epat.SetSrc();
                  const bool is_const_src = esrc.IsConstInt() || esrc.op == RtlOp::kConstDouble ||
                                            esrc.op == RtlOp::kSymbolRef ||
                                            esrc.op == RtlOp::kLabelRef ||
                                            esrc.op == RtlOp::kConst || esrc.op == RtlOp::kHigh;
                  const bool is_cross_bank_move =
                      esrc.IsHardReg() && ((scr >= 32) != (esrc.RegNo() >= 32));
                  if ((extract_sp_off(esrc, &moff) && proven_reload_spill_offsets.contains(moff)) ||
                      is_const_src || is_cross_bank_move) {
                    dying_pseudo = false;
                    break;
                  }
                }
                dying_pseudo = true;
                break;
              }
            }
            if (!decided) {
              const auto lit = cfg.sideband.basic_block_live_at_start.find(static_cast<int>(b));
              if (lit != cfg.sideband.basic_block_live_at_start.end() &&
                  lit->second.contains(scr)) {
                dying_pseudo = true;
              } else if (b == 0 && cfg.rpo_blocks[0].predecessors.empty() &&
                         ((scr >= 4 && scr <= 7) || (scr >= 44 && scr <= 47))) {
                dying_pseudo = true;
              }
            }
            if (dying_pseudo && !HasRegDeadNoteFor(insns[pi], scr)) {
              insns[pi].parsed.reg_notes.insert(
                  insns[pi].parsed.reg_notes.begin(),
                  RtlRegNote{
                      .kind = RtlMachineMode::kRegDead,
                      .list_op = RtlOp::kExprList,
                      .datum = RtlExpr::MakeReg(spat.SetSrc().mode, scr),
                  });
              changed = true;
            }
          }
        }
        break;
      }
      if (!SetsAreDisjoint(insns[pi].read_regs, scr_words)) {
        saw_read_between = true;
      }
      if (IsCallExpr(insns[pi].parsed.pattern) || IsJumpOrReturnExpr(insns[pi].parsed.pattern)) {
        break;
      }
    }
  }
  return changed;
}

int FirstReadRegPreorderIndex(const RtlExpr& root, const absl::flat_hash_set<int>& target_words) {
  int counter = 0;
  int found = -1;
  auto walk = [&](const auto& self, const RtlExpr& e, bool in_dst) -> void {
    if (found != -1 || e.op == RtlOp::kClobber) {
      return;
    }
    if (e.IsSet()) {
      if (!e.SetDest().IsReg()) {
        self(self, e.SetDest(), /*in_dst=*/true);
      }
      self(self, e.SetSrc(), /*in_dst=*/false);
      return;
    }
    if (in_dst && e.op == RtlOp::kMem) {
      for (const RtlExpr& ch : e.children) {
        self(self, ch, /*in_dst=*/false);
      }
      return;
    }
    if (!in_dst && e.op == RtlOp::kReg && !e.IsArgPointerReg()) {
      ++counter;
      if (target_words.contains(e.RegNo())) {
        found = counter;
      }
      return;
    }
    for (const RtlExpr& ch : e.children) {
      self(self, ch, in_dst);
    }
  };
  walk(walk, root, /*in_dst=*/false);
  return found;
}

bool CanonicalizeAdjacentSpillInputReloadPairs(
    const RtlControlFlowGraph& cfg, size_t b, bool gp64, bool fp64,
    const absl::flat_hash_set<int64_t>& proven_reload_spill_offsets,
    const absl::flat_hash_set<int>& use_pinned_regs, std::vector<ParsedBlockInsn>* block_ptr) {
  auto& insns = *block_ptr;
  bool changed = false;
  auto extract_spill_load = [&](size_t idx, int64_t* out_off, int* out_reg,
                                RtlMachineMode* out_mode) {
    const RtlExpr& pat = insns[idx].parsed.pattern;
    int base = -1;
    int64_t off = 0, sz = 0;
    if (!pat.IsSet() || !pat.SetDest().IsHardReg() || !insns[idx].parsed.reg_notes.empty() ||
        !ExtractStackBaseDispMem(pat.SetSrc(), &base, &off, &sz) || base != kMipsSpRegno ||
        !proven_reload_spill_offsets.contains(off)) {
      return false;
    }
    const int r = pat.SetDest().RegNo();
    if (r < 2 || r >= 64 || use_pinned_regs.contains(r)) {
      return false;
    }
    *out_off = off;
    *out_reg = r;
    *out_mode = pat.SetDest().mode;
    return true;
  };
  auto is_dead_after = [&](size_t cons_idx, const absl::flat_hash_set<int>& words) {
    for (int w : words) {
      if (insns[cons_idx].write_regs.contains(w) || HasRegDeadNoteFor(insns[cons_idx], w)) {
        continue;
      }
      bool killed = false;
      for (size_t k = cons_idx + 1; k < insns.size(); ++k) {
        if (insns[k].read_regs.contains(w) ||
            (IsCallExpr(insns[k].parsed.pattern) && CallInsnReadsArgReg(insns[k], w, gp64, fp64))) {
          return false;
        }
        if (insns[k].write_regs.contains(w) ||
            (IsCallExpr(insns[k].parsed.pattern) && IsMipsCallerSavableReg(w))) {
          killed = true;
          break;
        }
      }
      if (!killed) {
        for (size_t succ : cfg.rpo_blocks[b].successors) {
          const auto it = cfg.sideband.basic_block_live_at_start.find(static_cast<int>(succ));
          if (it != cfg.sideband.basic_block_live_at_start.end() && it->second.contains(w)) {
            return false;
          }
        }
        if (cfg.rpo_blocks[b].successors.empty() && (w == 2 || w == 3 || w == 32 || w == 33)) {
          return false;
        }
      }
    }
    return true;
  };
  auto extract_mtc1_load = [&](size_t idx, int* out_reg, RtlMachineMode* out_mode) {
    const RtlExpr& pat = insns[idx].parsed.pattern;
    if (!pat.IsSet() || !pat.SetDest().IsHardReg() || pat.SetDest().flags.integrated ||
        (pat.SetDest().mode != RtlMachineMode::kSf && pat.SetDest().mode != RtlMachineMode::kDf) ||
        pat.SetDest().RegNo() < 32 || pat.SetDest().RegNo() >= 64 || !pat.SetSrc().IsHardReg() ||
        pat.SetSrc().mode != pat.SetDest().mode || !insns[idx].parsed.reg_notes.empty() ||
        use_pinned_regs.contains(pat.SetDest().RegNo())) {
      return false;
    }
    const int src_r = pat.SetSrc().RegNo();
    const bool is_callee_saved_src = (src_r >= 16 && src_r <= 23) || src_r == 30;
    if (!is_callee_saved_src) {
      return false;
    }
    *out_reg = pat.SetDest().RegNo();
    *out_mode = pat.SetDest().mode;
    return true;
  };
  auto extract_cd_load = [&](size_t idx, int* out_reg, RtlMachineMode* out_mode) {
    const RtlExpr& pat = insns[idx].parsed.pattern;
    if (!pat.IsSet() || !pat.SetDest().IsHardReg() || pat.SetDest().flags.integrated ||
        (pat.SetDest().mode != RtlMachineMode::kSf && pat.SetDest().mode != RtlMachineMode::kDf) ||
        pat.SetDest().RegNo() < 32 || pat.SetDest().RegNo() >= 64 ||
        pat.SetSrc().op != RtlOp::kConstDouble || !insns[idx].parsed.reg_notes.empty() ||
        use_pinned_regs.contains(pat.SetDest().RegNo())) {
      return false;
    }
    *out_reg = pat.SetDest().RegNo();
    *out_mode = pat.SetDest().mode;
    return true;
  };
  for (size_t i = 0; i + 2 < insns.size(); ++i) {
    int64_t off0 = 0, off1 = 0;
    int r0 = -1, r1 = -1;
    RtlMachineMode m0 = RtlMachineMode::kNone, m1 = RtlMachineMode::kNone;
    const bool both_spill = extract_spill_load(i, &off0, &r0, &m0) &&
                            extract_spill_load(i + 1, &off1, &r1, &m1) && off0 != off1;
    const bool both_mtc1 =
        !both_spill && extract_mtc1_load(i, &r0, &m0) && extract_mtc1_load(i + 1, &r1, &m1);
    const bool spill_then_cd = !both_spill && !both_mtc1 &&
                               extract_spill_load(i, &off0, &r0, &m0) &&
                               extract_cd_load(i + 1, &r1, &m1);
    if (!both_spill && !both_mtc1 && !spill_then_cd) {
      continue;
    }
    absl::flat_hash_set<int> w0, w1;
    InsertRegSpan(r0, m0, gp64, fp64, &w0);
    InsertRegSpan(r1, m1, gp64, fp64, &w1);
    if (!SetsAreDisjoint(w0, w1)) {
      continue;
    }
    const RtlExpr& cpat = insns[i + 2].parsed.pattern;
    if (IsCallExpr(cpat) || IsJumpOrReturnExpr(cpat) || cpat.op == RtlOp::kUse ||
        cpat.op == RtlOp::kClobber || SetsAreDisjoint(insns[i + 2].read_regs, w0) ||
        SetsAreDisjoint(insns[i + 2].read_regs, w1) ||
        HasStrictInsnOrderHazard(insns[i], insns[i + 1], gp64, fp64)) {
      continue;
    }
    if (spill_then_cd) {
      // `reload1.c:6239-6300` (`emit_reload_insns`): when a `const_double`
      // input reload shares `cpat`'s output reload register (`op0`), it is
      // emitted ahead of a non-output-sharing spill-slot input reload (`op1`).
      if (!HasRegDeadNoteFor(insns[i + 2], r1) && !SetsAreDisjoint(insns[i + 2].write_regs, w1) &&
          SetsAreDisjoint(insns[i + 2].write_regs, w0)) {
        std::swap(insns[i], insns[i + 1]);
        changed = true;
      }
      continue;
    }
    if (both_spill) {
      if (!is_dead_after(i + 2, w0) || !is_dead_after(i + 2, w1)) {
        continue;
      }
    } else {
      // `reload1.c:6239-6300` (`emit_reload_insns`): adjacent cross-bank `mtc1`
      // input reloads feeding `cpat` (even when one is inherited into a later
      // insn) are emitted in left-to-right operand order (`ord0 < ord1`),
      // unless `w0` is also the output reload register of `cpat`.
      if (HasRegDeadNoteFor(insns[i + 2], r0) || HasRegDeadNoteFor(insns[i + 2], r1) ||
          !SetsAreDisjoint(insns[i + 2].write_regs, w0)) {
        continue;
      }
    }
    const int ord0 = FirstReadRegPreorderIndex(cpat, w0);
    const int ord1 = FirstReadRegPreorderIndex(cpat, w1);
    if (ord0 > 0 && ord1 > 0 && ord1 < ord0) {
      std::swap(insns[i], insns[i + 1]);
      changed = true;
    }
  }
  return changed;
}

int FindMinProvenGprReloadSpillReg(
    const std::vector<std::vector<ParsedBlockInsn>>& block_insns,
    const absl::flat_hash_set<int64_t>& proven_reload_spill_offsets) {
  if (proven_reload_spill_offsets.empty()) {
    return -1;
  }
  absl::flat_hash_set<int> gpr_spills;
  for (const auto& insns : block_insns) {
    for (const ParsedBlockInsn& insn : insns) {
      const RtlExpr& pat = insn.parsed.pattern;
      int base = -1;
      int64_t off = 0, sz = 0;
      if (pat.IsSet() && pat.SetDest().IsHardReg() && insn.parsed.reg_notes.empty() &&
          ExtractStackBaseDispMem(pat.SetSrc(), &base, &off, &sz) && base == kMipsSpRegno &&
          proven_reload_spill_offsets.contains(off)) {
        const int r = pat.SetDest().RegNo();
        if (r >= 2 && r < kMipsFirstFpr) {
          gpr_spills.insert(r);
        }
      }
    }
  }
  if (gpr_spills.size() < 2) {
    return -1;
  }
  return *std::min_element(gpr_spills.begin(), gpr_spills.end());
}

// Restores pre-`sched2` ordering when `reload1.c` cross-bank `spill_regs[]`
// round-robin witnesses that a `GR_REGS` spill-slot reload + consumer `[g1, g1+1]`
// using `min_gpr_spill_reg` (`spill_regs[0]`) could not have been separated from
// the next `GR_REGS` spill-slot load `g2` (`r2 > min_gpr_spill_reg`) by an
// `FP_REGS` spill-register reload `[f, f+1]` (`reload1.c:3775, 4758-4860`).
bool RestoreCrossBankReloadSpillRoundRobinOrder(
    const RtlControlFlowGraph& cfg, size_t b, bool gp64, bool fp64, int min_gpr_spill_reg,
    const absl::flat_hash_set<int64_t>& proven_reload_spill_offsets,
    const absl::flat_hash_set<int>& use_pinned_regs, std::vector<ParsedBlockInsn>* block_ptr) {
  if (min_gpr_spill_reg < 2 || proven_reload_spill_offsets.empty()) {
    return false;
  }
  auto& insns = *block_ptr;
  auto extract_gpr_spill_load = [&](size_t idx, int* out_reg, RtlMachineMode* out_mode) {
    const RtlExpr& pat = insns[idx].parsed.pattern;
    int base = -1;
    int64_t off = 0, sz = 0;
    if (!pat.IsSet() || !pat.SetDest().IsHardReg() || !insns[idx].parsed.reg_notes.empty() ||
        !ExtractStackBaseDispMem(pat.SetSrc(), &base, &off, &sz) || base != kMipsSpRegno ||
        !proven_reload_spill_offsets.contains(off)) {
      return false;
    }
    const int r = pat.SetDest().RegNo();
    if (r < 2 || r >= kMipsFirstFpr || use_pinned_regs.contains(r)) {
      return false;
    }
    *out_reg = r;
    *out_mode = pat.SetDest().mode;
    return true;
  };
  auto is_dead_after = [&](size_t cons_idx, const absl::flat_hash_set<int>& words) {
    for (int w : words) {
      if (insns[cons_idx].write_regs.contains(w) || HasRegDeadNoteFor(insns[cons_idx], w)) {
        continue;
      }
      bool killed = false;
      for (size_t k = cons_idx + 1; k < insns.size(); ++k) {
        if (insns[k].read_regs.contains(w) ||
            (IsCallExpr(insns[k].parsed.pattern) && CallInsnReadsArgReg(insns[k], w, gp64, fp64))) {
          return false;
        }
        if (insns[k].write_regs.contains(w) ||
            (IsCallExpr(insns[k].parsed.pattern) && IsMipsCallerSavableReg(w))) {
          killed = true;
          break;
        }
      }
      if (!killed) {
        for (size_t succ : cfg.rpo_blocks[b].successors) {
          const auto it = cfg.sideband.basic_block_live_at_start.find(static_cast<int>(succ));
          if (it != cfg.sideband.basic_block_live_at_start.end() && it->second.contains(w)) {
            return false;
          }
        }
        if (cfg.rpo_blocks[b].successors.empty() && (w == 2 || w == 3 || w == 32 || w == 33)) {
          return false;
        }
      }
    }
    return true;
  };
  auto match_fp_spill_reload_pair = [&](size_t f) {
    if (f + 1 >= insns.size() || !insns[f].parsed.reg_notes.empty()) {
      return false;
    }
    const RtlExpr& fpat = insns[f].parsed.pattern;
    if (!fpat.IsSet() || !fpat.SetDest().IsHardReg() ||
        (fpat.SetDest().mode != RtlMachineMode::kSf &&
         fpat.SetDest().mode != RtlMachineMode::kDf)) {
      return false;
    }
    const int fpr = fpat.SetDest().RegNo();
    if (fpr < kMipsFirstFpr || fpr >= 64 || use_pinned_regs.contains(fpr)) {
      return false;
    }
    bool valid_src = fpat.SetSrc().IsHardReg() && fpat.SetSrc().mode == fpat.SetDest().mode &&
                     fpat.SetSrc().RegNo() >= 2 && fpat.SetSrc().RegNo() < kMipsFirstFpr;
    if (!valid_src) {
      int base = -1;
      int64_t off = 0, sz = 0;
      valid_src = ExtractStackBaseDispMem(fpat.SetSrc(), &base, &off, &sz) &&
                  base == kMipsSpRegno && proven_reload_spill_offsets.contains(off);
    }
    if (!valid_src) {
      return false;
    }
    const RtlExpr& cpat = insns[f + 1].parsed.pattern;
    if (!cpat.IsSet() || cpat.SetSrc().IsReg() || cpat.SetSrc().IsMem() || cpat.SetDest().IsMem() ||
        IsCallExpr(cpat) || IsJumpOrReturnExpr(cpat)) {
      return false;
    }
    absl::flat_hash_set<int> fpr_words;
    InsertRegSpan(fpr, fpat.SetDest().mode, gp64, fp64, &fpr_words);
    return !SetsAreDisjoint(insns[f + 1].read_regs, fpr_words) && is_dead_after(f + 1, fpr_words);
  };

  bool any_changed = false;
  for (size_t g1 = 0; g1 + 4 < insns.size(); ++g1) {
    int r1 = -1;
    RtlMachineMode m1 = RtlMachineMode::kNone;
    int prev_r = -1;
    RtlMachineMode prev_m = RtlMachineMode::kNone;
    if (!extract_gpr_spill_load(g1, &r1, &m1) || r1 != min_gpr_spill_reg ||
        (g1 > 0 && extract_gpr_spill_load(g1 - 1, &prev_r, &prev_m))) {
      continue;
    }
    absl::flat_hash_set<int> r1_words;
    InsertRegSpan(r1, m1, gp64, fp64, &r1_words);
    const RtlExpr& g1_cpat = insns[g1 + 1].parsed.pattern;
    if (!g1_cpat.IsSet() || g1_cpat.SetSrc().IsReg() || IsCallExpr(g1_cpat) ||
        IsJumpOrReturnExpr(g1_cpat) || SetsAreDisjoint(insns[g1 + 1].read_regs, r1_words) ||
        !is_dead_after(g1 + 1, r1_words)) {
      continue;
    }
    size_t g2 = insns.size();
    int r2 = -1;
    size_t f_last = insns.size();
    bool min_gpr_touched = false;
    bool aborted = false;
    for (size_t k = g1 + 2; k < insns.size(); ++k) {
      if (IsCallExpr(insns[k].parsed.pattern) || IsJumpOrReturnExpr(insns[k].parsed.pattern)) {
        aborted = true;
        break;
      }
      int rk = -1;
      RtlMachineMode mk = RtlMachineMode::kNone;
      if (extract_gpr_spill_load(k, &rk, &mk)) {
        g2 = k;
        r2 = rk;
        break;
      }
      if (!SetsAreDisjoint(insns[k].read_regs, r1_words) ||
          !SetsAreDisjoint(insns[k].write_regs, r1_words)) {
        min_gpr_touched = true;
      }
      if (match_fp_spill_reload_pair(k)) {
        f_last = k;
      }
    }
    if (aborted || g2 >= insns.size() || r2 <= min_gpr_spill_reg || min_gpr_touched ||
        f_last >= insns.size() || f_last + 1 >= g2) {
      continue;
    }
    bool can_sink_g1 = true;
    for (size_t k = g1 + 2; k <= f_last + 1 && can_sink_g1; ++k) {
      if (HasStrictInsnOrderHazard(insns[g1], insns[k], gp64, fp64) ||
          HasStrictInsnOrderHazard(insns[g1 + 1], insns[k], gp64, fp64)) {
        can_sink_g1 = false;
      }
    }
    if (can_sink_g1) {
      std::rotate(insns.begin() + static_cast<ptrdiff_t>(g1),
                  insns.begin() + static_cast<ptrdiff_t>(g1 + 2),
                  insns.begin() + static_cast<ptrdiff_t>(f_last + 2));
      any_changed = true;
    }
  }
  return any_changed;
}

template <typename DeadPred>
bool RestoreCrossBankReloadOtherDeadNotesInBlock(const RtlControlFlowGraph& cfg, bool gp64,
                                                 bool fp64,
                                                 const absl::flat_hash_set<int>& use_pinned_regs,
                                                 const DeadPred& is_dead_after_consumer,
                                                 std::vector<ParsedBlockInsn>* block_ptr) {
  auto& insns = *block_ptr;
  bool changed = false;
  for (size_t i = 0; i + 2 < insns.size(); ++i) {
    const RtlExpr& pin = insns[i].parsed.pattern;
    if (!pin.IsSet() || !pin.SetDest().IsHardReg() || pin.SetDest().flags.integrated ||
        (pin.SetDest().mode != RtlMachineMode::kSf && pin.SetDest().mode != RtlMachineMode::kDf) ||
        pin.SetDest().RegNo() < 32 || pin.SetDest().RegNo() > 63 || !pin.SetSrc().IsHardReg() ||
        pin.SetSrc().flags.integrated || pin.SetSrc().mode != pin.SetDest().mode ||
        pin.SetSrc().RegNo() < 2 || pin.SetSrc().RegNo() >= 32 ||
        use_pinned_regs.contains(pin.SetDest().RegNo())) {
      continue;
    }
    const int fpr = pin.SetDest().RegNo();
    const int src_gpr = pin.SetSrc().RegNo();
    if (!HasRegDeadNoteFor(insns[i], src_gpr)) {
      continue;
    }
    absl::flat_hash_set<int> fpr_words, src_words;
    InsertRegSpan(fpr, pin.SetDest().mode, gp64, fp64, &fpr_words);
    InsertRegSpan(src_gpr, pin.SetSrc().mode, gp64, fp64, &src_words);

    size_t fp_idx = insns.size();
    bool src_touched = false;
    for (size_t k = i + 1; k < insns.size(); ++k) {
      if (!SetsAreDisjoint(insns[k].read_regs, fpr_words)) {
        fp_idx = k;
        break;
      }
      if (!SetsAreDisjoint(insns[k].write_regs, fpr_words) || IsCallExpr(insns[k].parsed.pattern) ||
          IsJumpOrReturnExpr(insns[k].parsed.pattern)) {
        break;
      }
      if (!SetsAreDisjoint(insns[k].read_regs, src_words) ||
          !SetsAreDisjoint(insns[k].write_regs, src_words)) {
        src_touched = true;
      }
    }
    if (src_touched || fp_idx + 1 >= insns.size() ||
        !SetsAreDisjoint(insns[fp_idx].read_regs, src_words) ||
        !SetsAreDisjoint(insns[fp_idx].write_regs, src_words)) {
      continue;
    }
    const RtlExpr& pfp = insns[fp_idx].parsed.pattern;
    if (!pfp.IsSet() || !pfp.SetDest().IsHardReg() || pfp.SetDest().flags.integrated ||
        pfp.SetDest().RegNo() != fpr || pfp.SetDest().mode != pin.SetDest().mode) {
      continue;
    }
    const RtlOp rop = pfp.SetSrc().op;
    if (rop == RtlOp::kReg || rop == RtlOp::kMem || rop == RtlOp::kConstDouble ||
        rop == RtlOp::kCall || rop == RtlOp::kSubreg) {
      continue;
    }
    const RtlExpr& pout = insns[fp_idx + 1].parsed.pattern;
    const int dst_gpr = (pout.IsSet() && pout.SetDest().IsHardReg()) ? pout.SetDest().RegNo() : -1;
    const bool valid_dst_gpr = (dst_gpr >= 2 && dst_gpr <= 25) ||
                               (dst_gpr == kMipsFpRegno && !cfg.sideband.frame_pointer_needed);
    if (!pout.IsSet() || !pout.SetDest().IsHardReg() || pout.SetDest().flags.integrated ||
        !valid_dst_gpr || pout.SetDest().mode != pin.SetDest().mode || !pout.SetSrc().IsHardReg() ||
        pout.SetSrc().RegNo() != fpr || HasRegDeadNoteFor(insns[fp_idx + 1], fpr) ||
        !is_dead_after_consumer(fp_idx + 1, fpr_words)) {
      continue;
    }

    auto& in_notes = insns[i].parsed.reg_notes;
    in_notes.erase(std::remove_if(in_notes.begin(), in_notes.end(),
                                  [src_gpr](const RtlRegNote& n) {
                                    return n.kind == RtlMachineMode::kRegDead &&
                                           n.datum.op == RtlOp::kReg && n.datum.RegNo() == src_gpr;
                                  }),
                   in_notes.end());
    if (!HasRegDeadNoteFor(insns[fp_idx], src_gpr)) {
      const int my_ord = FirstReadRegPreorderIndex(pfp, fpr_words);
      auto eff_note_ord = [&](const RtlRegNote& ex) -> int {
        if (ex.kind != RtlMachineMode::kRegDead || ex.datum.op != RtlOp::kReg ||
            ex.datum.IsArgPointerReg()) {
          return std::numeric_limits<int>::max();
        }
        absl::flat_hash_set<int> ex_words;
        InsertRegSpan(ex.datum.RegNo(), ex.datum.mode, gp64, fp64, &ex_words);
        const int direct = FirstReadRegPreorderIndex(pfp, ex_words);
        if (direct > 0) {
          return direct;
        }
        for (size_t m = 0; m < fp_idx; ++m) {
          const RtlExpr& mpat = insns[m].parsed.pattern;
          if (mpat.IsSet() && mpat.SetDest().IsHardReg() && mpat.SetSrc().IsHardReg() &&
              mpat.SetSrc().RegNo() == ex.datum.RegNo()) {
            absl::flat_hash_set<int> mf_words;
            InsertRegSpan(mpat.SetDest().RegNo(), mpat.SetDest().mode, gp64, fp64, &mf_words);
            const int mapped = FirstReadRegPreorderIndex(pfp, mf_words);
            if (mapped > 0) {
              return mapped;
            }
          }
        }
        return std::numeric_limits<int>::max();
      };
      auto& fp_notes = insns[fp_idx].parsed.reg_notes;
      auto ins_it = std::find_if(fp_notes.begin(), fp_notes.end(),
                                 [&](const RtlRegNote& ex) { return eff_note_ord(ex) > my_ord; });
      fp_notes.insert(ins_it, RtlRegNote{
                                  .kind = RtlMachineMode::kRegDead,
                                  .list_op = RtlOp::kExprList,
                                  .datum = pin.SetSrc(),
                              });
    }
    changed = true;
  }
  return changed;
}

bool HoistSched2SunkMdOutputReloadsInBlock(
    const RtlControlFlowGraph& cfg, bool gp64, bool fp64,
    const absl::flat_hash_set<int>& use_pinned_regs,
    const std::function<bool(size_t, const absl::flat_hash_set<int>&)>& is_dead_after_consumer,
    std::vector<ParsedBlockInsn>* insns_ptr) {
  auto& insns = *insns_ptr;
  if (insns.size() < 3) {
    return false;
  }
  auto expr_touches_md = [](const ParsedBlockInsn& insn) {
    if (IsCallExpr(insn.parsed.pattern)) {
      return false;
    }
    return insn.read_regs.contains(64) || insn.read_regs.contains(65) ||
           insn.write_regs.contains(64) || insn.write_regs.contains(65);
  };
  struct MdOutCandidate {
    size_t prod_idx = 0;
    size_t mv_idx = 0;
    bool single_use_input_elig = false;
  };
  std::vector<MdOutCandidate> cands;
  absl::flat_hash_set<size_t> covered_md_insns;
  for (size_t i = 0; i + 1 < insns.size(); ++i) {
    const RtlExpr& p0 = insns[i].parsed.pattern;
    if (p0.op != RtlOp::kParallel || p0.children.size() != 3 || !p0.children[0].IsSet() ||
        p0.children[1].op != RtlOp::kClobber || p0.children[2].op != RtlOp::kClobber ||
        !p0.children[0].SetDest().IsHardReg() ||
        p0.children[0].SetDest().mode != RtlMachineMode::kSi) {
      continue;
    }
    const int md_r = p0.children[0].SetDest().RegNo();
    const RtlOp op0 = p0.children[0].SetSrc().op;
    const bool is_expected_md_dest =
        (md_r == 64 && op0 == RtlOp::kTruncate) || (md_r == 65 && op0 == RtlOp::kMult);
    if (!is_expected_md_dest) {
      continue;
    }
    size_t mv_idx = insns.size();
    for (size_t k = i + 1; k < insns.size(); ++k) {
      const RtlExpr& pk = insns[k].parsed.pattern;
      if (pk.IsSet() && pk.SetDest().IsHardReg() && pk.SetDest().mode == RtlMachineMode::kSi &&
          !pk.SetDest().flags.integrated && pk.SetSrc().IsHardReg() &&
          pk.SetSrc().mode == RtlMachineMode::kSi && pk.SetSrc().RegNo() == md_r) {
        mv_idx = k;
        break;
      }
      if (IsCallExpr(pk) || IsJumpOrReturnExpr(pk) || expr_touches_md(insns[k])) {
        break;
      }
    }
    if (mv_idx >= insns.size() || !insns[mv_idx].parsed.reg_notes.empty()) {
      continue;
    }
    const int gpr = insns[mv_idx].parsed.pattern.SetDest().RegNo();
    const bool valid_gpr =
        (gpr >= 2 && gpr <= 25) || (gpr == kMipsFpRegno && !cfg.sideband.frame_pointer_needed);
    if (!valid_gpr || use_pinned_regs.contains(gpr) || !is_dead_after_consumer(mv_idx, {md_r})) {
      continue;
    }
    bool single_use_elig = false;
    size_t use_idx = insns.size();
    for (size_t k = mv_idx + 1; k < insns.size(); ++k) {
      if (insns[k].read_regs.contains(gpr)) {
        use_idx = k;
        break;
      }
      if (IsCallExpr(insns[k].parsed.pattern) || IsJumpOrReturnExpr(insns[k].parsed.pattern) ||
          insns[k].write_regs.contains(gpr) || expr_touches_md(insns[k])) {
        break;
      }
    }
    if (use_idx < insns.size() && insns[use_idx].parsed.pattern.IsSet() &&
        !IsCallExpr(insns[use_idx].parsed.pattern) &&
        !IsJumpOrReturnExpr(insns[use_idx].parsed.pattern) &&
        is_dead_after_consumer(use_idx, {md_r})) {
      const RtlExpr& up = insns[use_idx].parsed.pattern;
      const bool overwrites_gpr = up.SetDest().IsHardReg() && up.SetDest().RegNo() == gpr &&
                                  up.SetDest().mode == RtlMachineMode::kSi;
      if (overwrites_gpr ||
          (is_dead_after_consumer(use_idx, {gpr}) && HasRegDeadNoteFor(insns[use_idx], md_r))) {
        const RtlOp uop = up.SetSrc().op;
        const bool non_fp =
            up.SetDest().mode != RtlMachineMode::kSf && up.SetDest().mode != RtlMachineMode::kDf &&
            up.SetSrc().mode != RtlMachineMode::kSf && up.SetSrc().mode != RtlMachineMode::kDf;
        single_use_elig = (uop == RtlOp::kFloat && up.SetDest().IsHardReg() &&
                           up.SetDest().RegNo() >= 32 && up.SetDest().RegNo() <= 63) ||
                          (uop == RtlOp::kReg && non_fp && up.SetDest().op == RtlOp::kMem &&
                           up.SetSrc().IsHardReg() && up.SetSrc().RegNo() == gpr) ||
                          (non_fp && up.SetDest().IsReg() && uop != RtlOp::kReg &&
                           uop != RtlOp::kMem && uop != RtlOp::kDiv && uop != RtlOp::kUdiv &&
                           uop != RtlOp::kMod && uop != RtlOp::kUmod && uop != RtlOp::kMult &&
                           uop != RtlOp::kFloat && uop != RtlOp::kUnsignedFloat);
      }
    }
    if (single_use_elig) {
      covered_md_insns.insert(i);
      covered_md_insns.insert(mv_idx);
    }
    cands.push_back(MdOutCandidate{
        .prod_idx = i,
        .mv_idx = mv_idx,
        .single_use_input_elig = single_use_elig,
    });
  }
  if (cands.empty()) {
    return false;
  }
  bool other_md = false;
  for (size_t i = 0; i < insns.size(); ++i) {
    if (!covered_md_insns.contains(i) && expr_touches_md(insns[i])) {
      other_md = true;
      break;
    }
  }
  bool changed = false;
  for (const MdOutCandidate& c : cands) {
    if (c.prod_idx + 1 >= c.mv_idx || (c.single_use_input_elig && !other_md)) {
      continue;
    }
    bool can_hoist = true;
    for (size_t m = c.prod_idx + 1; m < c.mv_idx && can_hoist; ++m) {
      if (HasStrictInsnOrderHazard(insns[m], insns[c.mv_idx], gp64, fp64)) {
        can_hoist = false;
      }
    }
    if (can_hoist) {
      std::rotate(insns.begin() + static_cast<ptrdiff_t>(c.prod_idx + 1),
                  insns.begin() + static_cast<ptrdiff_t>(c.mv_idx),
                  insns.begin() + static_cast<ptrdiff_t>(c.mv_idx + 1));
      changed = true;
    }
  }
  return changed;
}

// Re-adjoins cross-bank `"=f"` FPR->GPR (`mfc1`) output reloads
// `(set (reg:SF|DF dst_gpr) (reg:SF|DF fpr))` (carrying no `REG_DEAD fpr`,
// emitted immediately after the producing FP arithmetic insn by
// `reload1.c:5186-5249, 5641, 6239-6285` `choose_reload_regs` +
// `emit_reload_insns`) when `sched2` interleaved independent instructions,
// reload-inheriting pure FP readers, or block-0 prologue callee-save stores
// (`toplev.c:3103`, `mips.c:expand_prologue`) into the FP latency shadow.
// Re-adjoins `sched2`-sunk output-reload spill stores `(set [sp/fp+off] r)`
// back immediately after their producing instruction (`reload1.c:6631`:
// `emit_reload_insns`). If every intervening blocker in `(prod_idx, i)` is an
// independent non-`/u` load with no hazard against `[prod_idx, k)`, hoist those
// loads ahead of `prod_idx` (`sched.c:3144`) so `insns[i]` can re-adjoin
// without requiring a speculative `/u` flag on the loads.
template <typename MatchMoveFn, typename DeadAfterFn>
bool ReadjoinSunkSpillStoresInBlock(const absl::flat_hash_set<int64_t>& proven_reload_spill_offsets,
                                    const absl::flat_hash_set<int>& use_pinned_regs,
                                    const MatchMoveFn& match_sp_off_reg_move,
                                    const DeadAfterFn& is_dead_after_consumer, bool gp64, bool fp64,
                                    std::vector<ParsedBlockInsn>* insns_ptr) {
  if (proven_reload_spill_offsets.empty()) {
    return false;
  }
  auto& insns = *insns_ptr;
  bool modified = false;
  for (size_t i = 2; i < insns.size(); ++i) {
    int64_t off = 0;
    int src_r = -1;
    if (!match_sp_off_reg_move(insns[i], /*want_store=*/true, &off, &src_r) ||
        !proven_reload_spill_offsets.contains(off) || src_r < 2 || src_r >= 64 ||
        use_pinned_regs.contains(src_r)) {
      continue;
    }
    bool has_dead_note = false;
    for (const RtlRegNote& n : insns[i].parsed.reg_notes) {
      if (n.kind == RtlMachineMode::kRegDead && n.datum.op == RtlOp::kReg &&
          n.datum.RegNo() == src_r) {
        has_dead_note = true;
        break;
      }
    }
    if (has_dead_note) {
      continue;
    }
    const RtlMachineMode rel_mode = insns[i].parsed.pattern.SetSrc().mode;
    absl::flat_hash_set<int> rel_words;
    InsertRegSpan(src_r, rel_mode, gp64, fp64, &rel_words);
    size_t prod_idx = insns.size();
    bool valid_prod = true;
    bool saw_read_between = false;
    for (size_t rev = i; rev > 0; --rev) {
      const size_t k = rev - 1;
      if (!SetsAreDisjoint(insns[k].write_regs, rel_words)) {
        prod_idx = k;
        break;
      }
      if (!SetsAreDisjoint(insns[k].read_regs, rel_words)) {
        saw_read_between = true;
      }
      if (IsCallExpr(insns[k].parsed.pattern) || IsJumpOrReturnExpr(insns[k].parsed.pattern)) {
        valid_prod = false;
        break;
      }
    }
    if (!valid_prod || prod_idx >= insns.size() || prod_idx + 1 >= i) {
      continue;
    }
    const RtlExpr& ppat = insns[prod_idx].parsed.pattern;
    if (!ppat.IsSet() || ppat.SetSrc().IsReg() || IsCallExpr(ppat) || IsJumpOrReturnExpr(ppat)) {
      continue;
    }

    bool can_hoist_store = true;
    std::vector<size_t> hoistable_loads;
    for (size_t k = prod_idx + 1; k < i && can_hoist_store; ++k) {
      if (!HasStrictInsnOrderHazard(insns[k], insns[i], gp64, fp64)) {
        continue;
      }
      const RtlExpr& kpat = insns[k].parsed.pattern;
      if (!insns[k].reads_mem || insns[k].writes_mem || insns[k].has_side_effects ||
          !kpat.IsSet() || !kpat.SetSrc().IsMem() || kpat.SetSrc().flags.unchanging) {
        can_hoist_store = false;
        break;
      }
      bool can_hoist_before_prod = true;
      for (size_t j = prod_idx; j < k; ++j) {
        if (HasStrictInsnOrderHazard(insns[j], insns[k], gp64, fp64)) {
          can_hoist_before_prod = false;
          break;
        }
      }
      if (!can_hoist_before_prod) {
        can_hoist_store = false;
        break;
      }
      hoistable_loads.push_back(k);
    }
    if (can_hoist_store) {
      size_t cur_prod = prod_idx;
      for (size_t k : hoistable_loads) {
        std::rotate(insns.begin() + static_cast<ptrdiff_t>(cur_prod),
                    insns.begin() + static_cast<ptrdiff_t>(k),
                    insns.begin() + static_cast<ptrdiff_t>(k + 1));
        ++cur_prod;
      }
      std::rotate(insns.begin() + static_cast<ptrdiff_t>(cur_prod + 1),
                  insns.begin() + static_cast<ptrdiff_t>(i),
                  insns.begin() + static_cast<ptrdiff_t>(i + 1));
      modified = true;
      continue;
    }

    bool can_sink_prod = !saw_read_between && is_dead_after_consumer(i, rel_words) &&
                         !insns[prod_idx].reads_mem && !insns[prod_idx].writes_mem;
    for (size_t k = prod_idx + 1; k < i && can_sink_prod; ++k) {
      if (HasStrictInsnOrderHazard(insns[prod_idx], insns[k], gp64, fp64)) {
        can_sink_prod = false;
      }
    }
    if (can_sink_prod) {
      std::rotate(insns.begin() + static_cast<ptrdiff_t>(prod_idx),
                  insns.begin() + static_cast<ptrdiff_t>(prod_idx + 1),
                  insns.begin() + static_cast<ptrdiff_t>(i));
      modified = true;
    }
  }
  return modified;
}

template <typename DeadAfterFn>
bool ReadjoinCrossBankFprOutputReloads(const absl::flat_hash_set<int>& use_pinned_regs,
                                       const DeadAfterFn& is_dead_after_consumer,
                                       int b0_min_prologue_save_off,
                                       const absl::flat_hash_set<int64_t>& caller_save_offsets,
                                       const absl::flat_hash_set<int64_t>& proven_reload_spill_offs,
                                       bool gp64, bool fp64,
                                       std::vector<ParsedBlockInsn>* insns_ptr) {
  auto& insns = *insns_ptr;
  bool modified = false;
  for (size_t i = 2; i < insns.size(); ++i) {
    const RtlExpr& mv = insns[i].parsed.pattern;
    if (!mv.IsSet() || mv.children.size() != 2 || !mv.SetDest().IsHardReg() ||
        mv.SetDest().flags.integrated || !mv.SetSrc().IsHardReg() ||
        mv.SetSrc().mode != mv.SetDest().mode ||
        (mv.SetDest().mode != RtlMachineMode::kSf && mv.SetDest().mode != RtlMachineMode::kDf)) {
      continue;
    }
    const int dst_gpr = mv.SetDest().RegNo();
    const int src_fpr = mv.SetSrc().RegNo();
    if (dst_gpr < 2 || dst_gpr >= 32 || src_fpr < 32 || src_fpr >= 64) {
      continue;
    }
    bool has_dead_or_equiv = false;
    for (const RtlRegNote& n : insns[i].parsed.reg_notes) {
      if (n.kind == RtlMachineMode::kRegEquiv || n.kind == RtlMachineMode::kRegEqual ||
          (n.kind == RtlMachineMode::kRegDead && n.datum.op == RtlOp::kReg &&
           n.datum.RegNo() == src_fpr)) {
        has_dead_or_equiv = true;
        break;
      }
    }
    if (has_dead_or_equiv) {
      continue;
    }
    absl::flat_hash_set<int> fpr_words;
    InsertRegSpan(src_fpr, mv.SetSrc().mode, gp64, fp64, &fpr_words);
    if (!is_dead_after_consumer(i, fpr_words)) {
      continue;
    }
    absl::flat_hash_set<int> dst_words;
    InsertRegSpan(dst_gpr, mv.SetDest().mode, gp64, fp64, &dst_words);
    size_t prod_idx = insns.size();
    bool valid_prod = true;
    for (size_t rev = i; rev > 0; --rev) {
      const size_t k = rev - 1;
      if (!SetsAreDisjoint(insns[k].write_regs, fpr_words)) {
        prod_idx = k;
        break;
      }
      if (IsCallExpr(insns[k].parsed.pattern) || IsJumpOrReturnExpr(insns[k].parsed.pattern)) {
        valid_prod = false;
        break;
      }
      if (!SetsAreDisjoint(insns[k].read_regs, fpr_words)) {
        const RtlExpr& kpat = insns[k].parsed.pattern;
        const bool is_inherited_fp_reader =
            kpat.IsSet() && kpat.children.size() == 2 && kpat.SetSrc().op != RtlOp::kReg &&
            kpat.SetSrc().op != RtlOp::kMem && kpat.SetSrc().op != RtlOp::kConstDouble &&
            !HasRegDeadNoteFor(insns[k], src_fpr);
        if (!is_inherited_fp_reader) {
          valid_prod = false;
          break;
        }
      }
    }
    if (!valid_prod || prod_idx >= insns.size() || prod_idx + 1 >= i) {
      continue;
    }
    const RtlExpr& ppat = insns[prod_idx].parsed.pattern;
    if (!ppat.IsSet() || !ppat.SetDest().IsHardReg() || ppat.SetDest().flags.integrated ||
        ppat.SetDest().RegNo() != src_fpr || ppat.SetDest().mode != mv.SetSrc().mode ||
        ppat.SetSrc().IsReg() || ppat.SetSrc().op == RtlOp::kMem ||
        ppat.SetSrc().op == RtlOp::kConstDouble || IsCallExpr(ppat) || IsJumpOrReturnExpr(ppat)) {
      continue;
    }
    bool can_hoist_mv = true;
    std::vector<std::pair<size_t, int64_t>> blocking_prologue_saves;
    auto is_b0_prologue_gpr_save = [&](size_t idx, int* saved_r_out, int64_t* off_out) {
      if (b0_min_prologue_save_off <= 0) {
        return false;
      }
      const RtlExpr& kpat = insns[idx].parsed.pattern;
      int kbase = -1;
      int64_t koff = 0, ksz = 0;
      if (!kpat.IsSet() || kpat.children.size() != 2 || !kpat.SetSrc().IsHardReg() ||
          kpat.SetSrc().flags.integrated) {
        return false;
      }
      const int saved_r = kpat.SetSrc().RegNo();
      const bool is_callee_saved_or_ra =
          (saved_r >= 16 && saved_r <= 23) || saved_r == 30 || saved_r == kMipsRaRegno;
      if (!is_callee_saved_or_ra) {
        return false;
      }
      if (!ExtractStackBaseDispMem(kpat.SetDest(), &kbase, &koff, &ksz) || kbase != kMipsSpRegno ||
          koff < b0_min_prologue_save_off || caller_save_offsets.contains(koff) ||
          proven_reload_spill_offs.contains(koff)) {
        return false;
      }
      for (size_t m = 0; m < idx; ++m) {
        if (insns[m].write_regs.contains(saved_r)) {
          return false;
        }
      }
      *saved_r_out = saved_r;
      *off_out = koff;
      return true;
    };
    for (size_t k = prod_idx + 1; k < i && can_hoist_mv; ++k) {
      if (!HasStrictInsnOrderHazard(insns[k], insns[i], gp64, fp64)) {
        continue;
      }
      int saved_r = -1;
      int64_t koff = 0;
      bool is_hoistable_prologue_save = false;
      if (is_b0_prologue_gpr_save(k, &saved_r, &koff) && dst_words.contains(saved_r)) {
        bool save_hazard = false;
        for (size_t m = prod_idx; m < k && !save_hazard; ++m) {
          if (HasStrictInsnOrderHazard(insns[m], insns[k], gp64, fp64)) {
            save_hazard = true;
          }
        }
        is_hoistable_prologue_save = !save_hazard;
      }
      if (is_hoistable_prologue_save) {
        blocking_prologue_saves.emplace_back(k, koff);
      } else {
        can_hoist_mv = false;
      }
    }
    if (can_hoist_mv) {
      // Preserve `mips.c:expand_prologue` descending-offset GPR save order
      // (e.g. `sw $ra` ahead of `sw $s2`, `sw $s1`, `sw $s0`) when hoisting a
      // blocking prologue save ahead of `prod_idx`.
      std::vector<size_t> prologue_saves_to_hoist;
      if (!blocking_prologue_saves.empty()) {
        int64_t min_hoisted_off = blocking_prologue_saves[0].second;
        size_t max_hoisted_idx = blocking_prologue_saves[0].first;
        for (const auto& [ps_idx, ps_off] : blocking_prologue_saves) {
          min_hoisted_off = std::min(min_hoisted_off, ps_off);
          max_hoisted_idx = std::max(max_hoisted_idx, ps_idx);
        }
        for (size_t m = prod_idx + 1; m <= max_hoisted_idx; ++m) {
          int sr = -1;
          int64_t moff = 0;
          if (!is_b0_prologue_gpr_save(m, &sr, &moff)) {
            continue;
          }
          const bool is_blocking =
              std::any_of(blocking_prologue_saves.begin(), blocking_prologue_saves.end(),
                          [&](const auto& p) { return p.first == m; });
          if (is_blocking) {
            prologue_saves_to_hoist.push_back(m);
          } else if (moff > min_hoisted_off) {
            bool m_hazard = false;
            for (size_t j = prod_idx; j < m && !m_hazard; ++j) {
              if (HasStrictInsnOrderHazard(insns[j], insns[m], gp64, fp64)) {
                m_hazard = true;
              }
            }
            if (!m_hazard) {
              prologue_saves_to_hoist.push_back(m);
            }
          }
        }
      }
      size_t cur_prod = prod_idx;
      for (size_t ps_idx : prologue_saves_to_hoist) {
        std::rotate(insns.begin() + static_cast<ptrdiff_t>(cur_prod),
                    insns.begin() + static_cast<ptrdiff_t>(ps_idx),
                    insns.begin() + static_cast<ptrdiff_t>(ps_idx + 1));
        ++cur_prod;
      }
      std::rotate(insns.begin() + static_cast<ptrdiff_t>(cur_prod + 1),
                  insns.begin() + static_cast<ptrdiff_t>(i),
                  insns.begin() + static_cast<ptrdiff_t>(i + 1));
      modified = true;
    }
  }
  return modified;
}

// Mirrors `reload1.c:702, 2229, 4758-4767` (`allocate_reload_reg`): `reload`
// initializes `last_spill_reg = -1` and advances round-robin through
// `spill_regs[]` (which orders tied call-used FP spill regs in ascending regno
// order on MIPS). When `sched2` schedules two independent cross-bank FP reload
// groups `[g1_start, g1_end)` (using `r1`) and `[g2_start, g2_end)` (using
// `r2`) out of round-robin order (`r2 < r1`) before either spill reg has been
// used earlier in the function, rotate `G2` ahead of `G1` when no strict order
// hazard prevents it.
bool RestoreRoundRobinFpReloadGroupOrder(bool gp64, bool fp64,
                                         absl::flat_hash_set<int>* ever_used_fprs,
                                         std::vector<ParsedBlockInsn>* insns_ptr) {
  auto& insns = *insns_ptr;
  auto match_cross_bank_fp_out_mv = [](const ParsedBlockInsn& insn, int* out_fpr) {
    const RtlExpr& mv = insn.parsed.pattern;
    if (!mv.IsSet() || mv.children.size() != 2 || !mv.SetDest().IsHardReg() ||
        mv.SetDest().flags.integrated || !mv.SetSrc().IsHardReg() ||
        mv.SetSrc().mode != mv.SetDest().mode ||
        (mv.SetDest().mode != RtlMachineMode::kSf && mv.SetDest().mode != RtlMachineMode::kDf)) {
      return false;
    }
    const int dst_gpr = mv.SetDest().RegNo();
    const int src_fpr = mv.SetSrc().RegNo();
    if (dst_gpr < 2 || dst_gpr >= 32 || src_fpr < 32 || src_fpr >= 52 ||
        HasRegDeadNoteFor(insn, src_fpr)) {
      return false;
    }
    *out_fpr = src_fpr;
    return true;
  };
  auto is_cross_bank_fp_in_mv = [](const ParsedBlockInsn& insn, int fpr) {
    const RtlExpr& mv = insn.parsed.pattern;
    if (!mv.IsSet() || mv.children.size() != 2 || !mv.SetDest().IsHardReg() ||
        mv.SetDest().flags.integrated || mv.SetDest().RegNo() != fpr || !mv.SetSrc().IsHardReg() ||
        mv.SetSrc().mode != mv.SetDest().mode) {
      return false;
    }
    const int src_gpr = mv.SetSrc().RegNo();
    return src_gpr >= 2 && src_gpr < 32;
  };
  struct FpReloadGroup {
    size_t start = 0;
    size_t end = 0;
    int fpr = -1;
  };
  auto is_any_prologue_gpr_save = [](const ParsedBlockInsn& insn, int* out_gpr = nullptr) {
    const RtlExpr& pat = insn.parsed.pattern;
    if (!pat.IsSet() || !pat.SetSrc().IsHardReg()) {
      return false;
    }
    const int gpr = pat.SetSrc().RegNo();
    const bool is_callee_saved_gpr = (gpr >= 16 && gpr <= 23) || gpr == 30;
    if (!is_callee_saved_gpr) {
      return false;
    }
    int sbase = -1;
    int64_t soff = 0;
    int64_t ssz = 0;
    if (!ExtractStackBaseDispMem(pat.SetDest(), &sbase, &soff, &ssz) || sbase != kMipsSpRegno) {
      return false;
    }
    if (out_gpr != nullptr) {
      *out_gpr = gpr;
    }
    return true;
  };
  auto find_group_at_out_mv = [&](size_t out_idx, FpReloadGroup* out_group) {
    int fpr = -1;
    if (out_idx == 0 || !match_cross_bank_fp_out_mv(insns[out_idx], &fpr)) {
      return false;
    }
    size_t prod_idx = out_idx - 1;
    while (prod_idx > 0 && is_any_prologue_gpr_save(insns[prod_idx])) {
      --prod_idx;
    }
    const RtlExpr& prod = insns[prod_idx].parsed.pattern;
    if (!prod.IsSet() || !prod.SetDest().IsHardReg() || prod.SetDest().flags.integrated ||
        prod.SetDest().RegNo() != fpr || prod.SetSrc().IsReg() || prod.SetSrc().op == RtlOp::kMem ||
        prod.SetSrc().op == RtlOp::kConstDouble || IsCallExpr(prod) || IsJumpOrReturnExpr(prod)) {
      return false;
    }
    size_t start = prod_idx;
    size_t scan = prod_idx;
    while (scan > 0) {
      if (is_cross_bank_fp_in_mv(insns[scan - 1], fpr)) {
        --scan;
        start = scan;
      } else if (is_any_prologue_gpr_save(insns[scan - 1])) {
        --scan;
      } else {
        break;
      }
    }
    out_group->start = start;
    out_group->end = out_idx + 1;
    out_group->fpr = fpr;
    return true;
  };

  bool modified = false;
  std::vector<FpReloadGroup> groups;
  for (size_t i = 1; i < insns.size(); ++i) {
    FpReloadGroup g;
    if (find_group_at_out_mv(i, &g)) {
      if (groups.empty() || g.start >= groups.back().end) {
        groups.push_back(g);
      }
    }
  }
  absl::flat_hash_set<int> used_before = *ever_used_fprs;
  size_t scan_pos = 0;
  for (size_t gi = 0; gi + 1 < groups.size(); ++gi) {
    while (scan_pos < groups[gi].start) {
      for (int r : insns[scan_pos].read_regs) {
        if (r >= 32 && r < 64) used_before.insert(r);
      }
      for (int r : insns[scan_pos].write_regs) {
        if (r >= 32 && r < 64) used_before.insert(r);
      }
      ++scan_pos;
    }
    const FpReloadGroup& g1 = groups[gi];
    const FpReloadGroup& g2 = groups[gi + 1];
    if (g2.fpr >= g1.fpr || used_before.contains(g1.fpr) || used_before.contains(g2.fpr)) {
      continue;
    }
    const int g2_out_gpr = insns[g2.end - 1].parsed.pattern.SetDest().RegNo();
    bool has_hazard = false;
    std::vector<size_t> hoistable_saves;
    for (size_t a = g1.start; a < g2.start && !has_hazard; ++a) {
      bool conflicts_g2 = false;
      for (size_t b = g2.start; b < g2.end; ++b) {
        if (HasStrictInsnOrderHazard(insns[a], insns[b], gp64, fp64)) {
          conflicts_g2 = true;
          break;
        }
      }
      if (!conflicts_g2) {
        continue;
      }
      // A `B0` prologue save `(set (mem:SI (+ sp off)) (reg:SI g2_out_gpr))`
      // interleaved inside `[g1.start, g2.start)` can be hoisted ahead of `G1`
      // before rotating `G2` ahead of `G1` when it has no hazard against
      // `[g1.start, a)`.
      int saved_gpr = -1;
      const bool is_g2_prologue_save =
          is_any_prologue_gpr_save(insns[a], &saved_gpr) && saved_gpr == g2_out_gpr;
      bool can_hoist_save = is_g2_prologue_save;
      for (size_t m = g1.start; m < a && can_hoist_save; ++m) {
        if (HasStrictInsnOrderHazard(insns[m], insns[a], gp64, fp64)) {
          can_hoist_save = false;
        }
      }
      if (can_hoist_save) {
        hoistable_saves.push_back(a);
      } else {
        has_hazard = true;
      }
    }
    if (!has_hazard) {
      size_t cur_g1_start = g1.start;
      for (size_t s_idx : hoistable_saves) {
        std::rotate(insns.begin() + static_cast<ptrdiff_t>(cur_g1_start),
                    insns.begin() + static_cast<ptrdiff_t>(s_idx),
                    insns.begin() + static_cast<ptrdiff_t>(s_idx + 1));
        ++cur_g1_start;
      }
      std::rotate(insns.begin() + static_cast<ptrdiff_t>(cur_g1_start),
                  insns.begin() + static_cast<ptrdiff_t>(g2.start),
                  insns.begin() + static_cast<ptrdiff_t>(g2.end));
      modified = true;
      break;
    }
  }
  for (const ParsedBlockInsn& insn : insns) {
    for (int r : insn.read_regs) {
      if (r >= 32 && r < 64) ever_used_fprs->insert(r);
    }
    for (int r : insn.write_regs) {
      if (r >= 32 && r < 64) ever_used_fprs->insert(r);
    }
  }
  return modified;
}

// Collects `FP_REGS` hard registers proven to act as `reload` spill registers
// in block `b` (`reload1.c:3445-3508` `spill_hard_reg`), via either a
// `proven_reload_spill_offsets` FP load/store or a cross-bank `mtc1`/`mfc1`
// reload in `insns`, excluding any FPR live-in or live-out of block `b`.
absl::flat_hash_set<int> CollectBlockProvenFpSpillRegs(
    const RtlControlFlowGraph& cfg, size_t b, const std::vector<ParsedBlockInsn>& insns,
    const absl::flat_hash_set<int64_t>& proven_reload_spill_offsets,
    const absl::flat_hash_set<int>& use_pinned_regs, bool gp64, bool fp64) {
  absl::flat_hash_set<int> live_boundary;
  if (auto it = cfg.sideband.basic_block_live_at_start.find(static_cast<int>(b));
      it != cfg.sideband.basic_block_live_at_start.end()) {
    live_boundary.insert(it->second.begin(), it->second.end());
  }
  for (size_t succ : cfg.rpo_blocks[b].successors) {
    if (auto it = cfg.sideband.basic_block_live_at_start.find(static_cast<int>(succ));
        it != cfg.sideband.basic_block_live_at_start.end()) {
      live_boundary.insert(it->second.begin(), it->second.end());
    }
  }
  if (cfg.rpo_blocks[b].successors.empty()) {
    live_boundary.insert(32);
    live_boundary.insert(33);
  }
  const int expected_base = cfg.sideband.frame_pointer_needed ? kMipsFpRegno : kMipsSpRegno;
  absl::flat_hash_set<int> proven;
  auto maybe_add = [&](int r, RtlMachineMode mode) {
    if (r < 32 || r >= 64 || use_pinned_regs.contains(r)) {
      return;
    }
    absl::flat_hash_set<int> words;
    InsertRegSpan(r, mode, gp64, fp64, &words);
    if (SetsAreDisjoint(words, live_boundary)) {
      proven.insert(r);
    }
  };
  for (const ParsedBlockInsn& insn : insns) {
    const RtlExpr& pat = insn.parsed.pattern;
    if (!pat.IsSet() || pat.children.size() != 2) {
      continue;
    }
    const RtlExpr& dst = pat.SetDest();
    const RtlExpr& src = pat.SetSrc();
    int base = -1;
    int64_t off = 0, sz = 0;
    if (dst.IsHardReg() && !dst.flags.integrated && dst.RegNo() >= 32 && dst.RegNo() < 64 &&
        (dst.mode == RtlMachineMode::kSf || dst.mode == RtlMachineMode::kDf)) {
      if (src.IsHardReg() && src.mode == dst.mode &&
          ((src.RegNo() >= 16 && src.RegNo() <= 23) || src.RegNo() == 30)) {
        maybe_add(dst.RegNo(), dst.mode);
      } else if (ExtractStackBaseDispMem(src, &base, &off, &sz) && base == expected_base &&
                 proven_reload_spill_offsets.contains(off)) {
        maybe_add(dst.RegNo(), dst.mode);
      }
    } else if (src.IsHardReg() && !src.flags.integrated && src.RegNo() >= 32 && src.RegNo() < 64 &&
               (src.mode == RtlMachineMode::kSf || src.mode == RtlMachineMode::kDf)) {
      // Do NOT add `src` from a stack spill store alone: `reload1.c:4712` may
      // reuse a dying allocated pseudo's hard register for an output reload
      // without spilling `src` via `spill_hard_reg`.
      if (dst.IsHardReg() && !dst.flags.integrated && dst.mode == src.mode &&
          ((dst.RegNo() >= 16 && dst.RegNo() <= 23) || dst.RegNo() == 30) &&
          !HasRegDeadNoteFor(insn, src.RegNo())) {
        maybe_add(src.RegNo(), src.mode);
      }
    }
  }
  return proven;
}

// Mirrors `reload1.c:4758-4767` (`choose_reload_regs`: `i = (last_spill_reg + 1) % n_spills`):
// when a block uses exactly two proven `FP_REGS` spill registers `{r_a, r_b}`
// and `sched2` reordered two independent single-spill-reg reload groups
// `G1, G2` so that `G1.fpr == prev_fpr` and `G2.fpr != prev_fpr` (breaking strict
// 2-register round-robin alternation), try rotating `G2` (with its minimal
// backward dependency closure inside `[G1.end, G2.end)`) ahead of `G1` when
// verified against exact `kSched2` forward replay.
template <typename VerifyFn>
bool RestoreRoundRobinTwoRegFpSpillOrder(const RtlControlFlowGraph& cfg, size_t b, bool gp64,
                                         bool fp64,
                                         const absl::flat_hash_set<int64_t>& proven_spill_offsets,
                                         const VerifyFn& verify_cand,
                                         std::vector<ParsedBlockInsn>* insns_ptr) {
  auto& insns = *insns_ptr;
  if (insns.size() < 4) {
    return false;
  }
  absl::flat_hash_set<int> use_pinned_regs;
  for (const ParsedBlockInsn& insn : insns) {
    if (insn.parsed.pattern.op == RtlOp::kUse && insn.parsed.pattern.children.size() == 1 &&
        insn.parsed.pattern.children[0].IsHardReg()) {
      use_pinned_regs.insert(insn.parsed.pattern.children[0].RegNo());
    }
  }
  const absl::flat_hash_set<int> fp_spills = CollectBlockProvenFpSpillRegs(
      cfg, b, insns, proven_spill_offsets, use_pinned_regs, gp64, fp64);
  if (fp_spills.size() != 2) {
    return false;
  }
  const int expected_base = cfg.sideband.frame_pointer_needed ? kMipsFpRegno : kMipsSpRegno;
  auto match_in_reload = [&](const ParsedBlockInsn& insn) -> int {
    const RtlExpr& pat = insn.parsed.pattern;
    if (!pat.IsSet() || !pat.SetDest().IsHardReg() || pat.SetDest().flags.integrated) {
      return -1;
    }
    const int r = pat.SetDest().RegNo();
    if (!fp_spills.contains(r)) {
      return -1;
    }
    const RtlExpr& src = pat.SetSrc();
    if (src.op == RtlOp::kConstDouble) {
      return r;
    }
    if (src.IsHardReg() && src.RegNo() >= 2 && src.RegNo() < 32 && src.mode == pat.SetDest().mode) {
      return r;
    }
    int base = -1;
    int64_t off = 0, sz = 0;
    if (ExtractStackBaseDispMem(src, &base, &off, &sz) && base == expected_base &&
        proven_spill_offsets.contains(off)) {
      return r;
    }
    return -1;
  };
  auto match_out_reload = [&](const ParsedBlockInsn& insn) -> int {
    const RtlExpr& pat = insn.parsed.pattern;
    if (!pat.IsSet() || !pat.SetSrc().IsHardReg() || pat.SetSrc().flags.integrated) {
      return -1;
    }
    const int r = pat.SetSrc().RegNo();
    if (!fp_spills.contains(r) || HasRegDeadNoteFor(insn, r)) {
      return -1;
    }
    const RtlExpr& dst = pat.SetDest();
    if (dst.IsHardReg() && dst.RegNo() >= 2 && dst.RegNo() < 32 && dst.mode == pat.SetSrc().mode) {
      return r;
    }
    int base = -1;
    int64_t off = 0, sz = 0;
    if (ExtractStackBaseDispMem(dst, &base, &off, &sz) && base == expected_base &&
        proven_spill_offsets.contains(off)) {
      return r;
    }
    return -1;
  };
  auto touched_spill_regs = [&](const ParsedBlockInsn& insn) {
    absl::flat_hash_set<int> s;
    for (int r : fp_spills) {
      if (insn.read_regs.contains(r) || insn.write_regs.contains(r)) {
        s.insert(r);
      }
    }
    return s;
  };

  struct ReloadGroup {
    size_t start = 0;
    size_t end = 0;
    int fpr = -1;  // >= 0 iff single-spill-reg group; -2 if multi-spill-reg barrier.
    int first_in_fpr = -1;
    int last_out_fpr = -1;
  };

  bool any_modified = false;
  for (int iter = 0; iter < 16; ++iter) {
    std::vector<ReloadGroup> groups;
    size_t idx = 0;
    while (idx < insns.size()) {
      if (IsCallExpr(insns[idx].parsed.pattern) || IsJumpOrReturnExpr(insns[idx].parsed.pattern)) {
        groups.push_back(ReloadGroup{.start = idx, .end = idx + 1, .fpr = -2});
        ++idx;
        continue;
      }
      const int in_r = match_in_reload(insns[idx]);
      if (in_r >= 0) {
        size_t g_start = idx;
        absl::btree_set<int> used_regs;
        used_regs.insert(in_r);
        int first_r = in_r;
        int last_r = in_r;
        size_t cur = idx + 1;
        while (cur < insns.size()) {
          const int next_in = match_in_reload(insns[cur]);
          if (next_in < 0 || used_regs.contains(next_in)) {
            break;
          }
          used_regs.insert(next_in);
          last_r = next_in;
          ++cur;
        }
        if (cur < insns.size() && !IsCallExpr(insns[cur].parsed.pattern) &&
            !IsJumpOrReturnExpr(insns[cur].parsed.pattern)) {
          const size_t cons_pos = cur;
          const auto c_regs = touched_spill_regs(insns[cons_pos]);
          if (!c_regs.empty()) {
            for (int r : c_regs) used_regs.insert(r);
            int last_alloc_r = last_r;
            if (used_regs.size() == 2) {
              // `reload1.c:4758-4860`: for a 2-spill-reg consumer insn, `op1`
              // is allocated before `op2`, so `last_spill_reg` ends on the
              // spill reg with the larger left-to-right operand read index
              // even if `emit_reload_insns` emitted an output-sharing `op2`
              // reload ahead of `op1`.
              int best_ord = -1;
              for (int r : used_regs) {
                absl::flat_hash_set<int> rw;
                InsertRegSpan(r, RtlMachineMode::kSf, gp64, fp64, &rw);
                const int ord = FirstReadRegPreorderIndex(insns[cons_pos].parsed.pattern, rw);
                if (ord >= 0 && (ord > best_ord || (ord == best_ord && r > last_alloc_r))) {
                  best_ord = ord;
                  last_alloc_r = r;
                }
              }
            }
            ++cur;
            if (cur < insns.size()) {
              const int out_r = match_out_reload(insns[cur]);
              if (out_r >= 0 && used_regs.contains(out_r)) {
                ++cur;
              }
            }
            // Include any immediately following pure consumer inheriting a
            // spill reg loaded by this 2-reg group (`reload1.c:5186-5258`).
            while (used_regs.size() == 2 && cur < insns.size() && match_in_reload(insns[cur]) < 0 &&
                   match_out_reload(insns[cur]) < 0 && !IsCallExpr(insns[cur].parsed.pattern) &&
                   !IsJumpOrReturnExpr(insns[cur].parsed.pattern)) {
              const auto next_t = touched_spill_regs(insns[cur]);
              if (next_t.size() == 1 && used_regs.contains(*next_t.begin()) &&
                  !insns[cur].write_regs.contains(*next_t.begin())) {
                ++cur;
              } else {
                break;
              }
            }
            groups.push_back(ReloadGroup{
                .start = g_start,
                .end = cur,
                .fpr = (used_regs.size() == 1) ? *used_regs.begin() : -2,
                .first_in_fpr = first_r,
                .last_out_fpr = (used_regs.size() == 1) ? *used_regs.begin() : last_alloc_r,
            });
            idx = cur;
            continue;
          }
        }
        idx = g_start + 1;
        continue;
      }
      const auto t_regs = touched_spill_regs(insns[idx]);
      if (!t_regs.empty()) {
        size_t g_start = idx;
        size_t cur = idx + 1;
        absl::flat_hash_set<int> used_regs = t_regs;
        if (cur < insns.size()) {
          const int out_r = match_out_reload(insns[cur]);
          if (out_r >= 0 && used_regs.contains(out_r)) {
            ++cur;
          }
        }
        groups.push_back(ReloadGroup{
            .start = g_start,
            .end = cur,
            .fpr = (used_regs.size() == 1) ? *used_regs.begin() : -2,
            .first_in_fpr = (used_regs.size() == 1) ? *used_regs.begin() : -1,
            .last_out_fpr = (used_regs.size() == 1) ? *used_regs.begin() : -1,
        });
        idx = cur;
        continue;
      }
      ++idx;
    }

    bool applied = false;
    int prev_fpr = -1;
    for (size_t gi = 0; gi < groups.size(); ++gi) {
      const ReloadGroup& g1 = groups[gi];
      // `reload1.c:4758-4767, 5186-5258`: if a 2-reg group
      // `[in0(r0), in1(r1), cons, inh_cons]` has `r0 == prev_fpr`, `r1 != prev_fpr`,
      // and `inh_cons` only reads `r1` (inheriting `r1`), un-swap to
      // `[in1(r1), inh_cons, in0(r0), cons]` when verified against exact `kSched2`.
      if (g1.fpr == -2 && prev_fpr >= 0 && g1.first_in_fpr == prev_fpr && g1.end - g1.start == 4) {
        const int r0 = match_in_reload(insns[g1.start]);
        const int r1 = match_in_reload(insns[g1.start + 1]);
        const auto inh_regs = touched_spill_regs(insns[g1.start + 3]);
        if (r0 == prev_fpr && r1 >= 0 && r1 != prev_fpr && inh_regs.size() == 1 &&
            inh_regs.contains(r1) && !insns[g1.start + 3].write_regs.contains(r1) &&
            !HasStrictInsnOrderHazard(insns[g1.start], insns[g1.start + 1], gp64, fp64) &&
            !HasStrictInsnOrderHazard(insns[g1.start], insns[g1.start + 3], gp64, fp64) &&
            !HasStrictInsnOrderHazard(insns[g1.start + 2], insns[g1.start + 3], gp64, fp64)) {
          std::vector<ParsedBlockInsn> cand = insns;
          cand[g1.start] = insns[g1.start + 1];
          cand[g1.start + 1] = insns[g1.start + 3];
          cand[g1.start + 2] = insns[g1.start];
          cand[g1.start + 3] = insns[g1.start + 2];
          if (verify_cand(cand)) {
            insns = std::move(cand);
            any_modified = true;
            applied = true;
            break;
          }
        }
      }
      if (g1.fpr < 0) {
        prev_fpr = g1.last_out_fpr;
        continue;
      }
      if (gi + 1 >= groups.size()) {
        break;
      }
      const ReloadGroup& g2 = groups[gi + 1];
      const bool single_pair_violation =
          (prev_fpr >= 0 && g1.fpr == prev_fpr && g2.fpr >= 0 && g2.fpr != prev_fpr);
      const bool single_before_two_reg_violation =
          (g2.fpr == -2 && g2.first_in_fpr == g1.fpr && g2.last_out_fpr >= 0 &&
           g2.last_out_fpr != g1.fpr && (prev_fpr < 0 || prev_fpr != g2.first_in_fpr));
      if (single_pair_violation || single_before_two_reg_violation) {
        // Compute minimal backward dependency closure of `g2` inside `[g1.end, g2.end)`.
        std::vector<bool> in_move(g2.end - g1.start, false);
        for (size_t k = g2.start; k < g2.end; ++k) {
          in_move[k - g1.start] = true;
        }
        for (size_t k = g2.start; k > g1.end; --k) {
          const size_t pos = k - 1;
          for (size_t m = pos + 1; m < g2.end; ++m) {
            if (in_move[m - g1.start] &&
                HasStrictInsnOrderHazard(insns[pos], insns[m], gp64, fp64)) {
              in_move[pos - g1.start] = true;
              break;
            }
          }
        }
        bool legal = true;
        for (size_t a = g1.start; a < g2.end && legal; ++a) {
          if (in_move[a - g1.start]) {
            continue;
          }
          for (size_t m = a + 1; m < g2.end; ++m) {
            if (in_move[m - g1.start] && HasStrictInsnOrderHazard(insns[a], insns[m], gp64, fp64)) {
              legal = false;
              break;
            }
          }
        }
        if (legal) {
          std::vector<ParsedBlockInsn> cand = insns;
          std::vector<ParsedBlockInsn> moved;
          std::vector<ParsedBlockInsn> stayed;
          for (size_t k = g1.start; k < g2.end; ++k) {
            if (in_move[k - g1.start]) {
              moved.push_back(cand[k]);
            } else {
              stayed.push_back(cand[k]);
            }
          }
          size_t dst = g1.start;
          for (auto& x : moved) cand[dst++] = std::move(x);
          for (auto& x : stayed) cand[dst++] = std::move(x);
          if (verify_cand(cand)) {
            insns = std::move(cand);
            any_modified = true;
            applied = true;
            break;
          }
        }
      }
      prev_fpr = g1.fpr;
    }
    if (!applied) {
      break;
    }
  }
  return any_modified;
}

// Mirrors `reload1.c:emit_reload_insns` (`reload1.c:6631`) and `sched.c`
// (`mips.md` `imul` latency): `reload` emits a `DI`-mode output reload
// `(set (reg:DI gpr) (reg:DI 64 hi))` immediately after its producing
// `mulsidi3` / `umulsidi3` `PARALLEL`, and `sched2` schedules independent
// instructions into the multiply latency shadow between them. Re-adjoin the
// output reload immediately after `mult:DI` when no intervening instruction
// conflicts with `{64, 65, gpr, gpr + 1}` and the block proves an output reload
// (either another `mult:DI` in the same block or a non-adjacent consumer).
bool ReadjoinMdDiOutputReloads(const RtlControlFlowGraph& cfg, int min_prologue_save_off,
                               const absl::flat_hash_set<int>& use_pinned_regs, bool gp64,
                               bool fp64, std::vector<ParsedBlockInsn>* insns) {
  auto is_di_mult_to_hi = [](const RtlExpr& pat) {
    return pat.op == RtlOp::kParallel && !pat.children.empty() && pat.children[0].IsSet() &&
           pat.children[0].SetDest().IsHardReg() &&
           pat.children[0].SetDest().mode == RtlMachineMode::kDi &&
           pat.children[0].SetDest().RegNo() == 64 && pat.children[0].SetSrc().op == RtlOp::kMult;
  };
  bool modified = false;
  int di_mult_count = 0;
  for (const ParsedBlockInsn& insn : *insns) {
    if (is_di_mult_to_hi(insn.parsed.pattern)) {
      ++di_mult_count;
    }
  }
  if (di_mult_count > 0) {
    const int expected_base = cfg.sideband.frame_pointer_needed ? kMipsFpRegno : kMipsSpRegno;
    for (size_t s1_idx = 1; s1_idx < insns->size(); ++s1_idx) {
      const RtlExpr& s1 = (*insns)[s1_idx].parsed.pattern;
      if (!s1.IsSet() || !s1.SetDest().IsHardReg() || s1.SetDest().flags.integrated ||
          s1.SetDest().mode != RtlMachineMode::kDi || !s1.SetSrc().IsHardReg() ||
          s1.SetSrc().mode != RtlMachineMode::kDi || s1.SetSrc().RegNo() != 64 ||
          !(*insns)[s1_idx].parsed.reg_notes.empty()) {
        continue;
      }
      const int dst_gpr = s1.SetDest().RegNo();
      if (dst_gpr < 2 || dst_gpr > 24 || (dst_gpr % 2) != 0 || use_pinned_regs.contains(dst_gpr) ||
          use_pinned_regs.contains(dst_gpr + 1)) {
        continue;
      }
      const absl::flat_hash_set<int> dst_words = {dst_gpr, dst_gpr + 1};
      const bool non_adjacent_consumer = (s1_idx + 1 < insns->size()) &&
                                         SetsAreDisjoint((*insns)[s1_idx + 1].read_regs, dst_words);
      if (di_mult_count < 2 && !non_adjacent_consumer) {
        continue;
      }
      const absl::flat_hash_set<int> pair_words = {64, 65, dst_gpr, dst_gpr + 1};
      size_t m_idx = insns->size();
      bool ok_scan = true;
      for (size_t rev = s1_idx; rev > 0; --rev) {
        const size_t k = rev - 1;
        const RtlExpr& kpat = (*insns)[k].parsed.pattern;
        if (is_di_mult_to_hi(kpat)) {
          m_idx = k;
          break;
        }
        if (!SetsAreDisjoint((*insns)[k].read_regs, pair_words) ||
            !SetsAreDisjoint((*insns)[k].write_regs, pair_words) || IsCallExpr(kpat) ||
            IsJumpOrReturnExpr(kpat) ||
            HasStrictInsnOrderHazard((*insns)[k], (*insns)[s1_idx], gp64, fp64)) {
          ok_scan = false;
          break;
        }
      }
      if (!ok_scan || m_idx >= insns->size()) {
        continue;
      }
      size_t cur_s1 = s1_idx;
      if (m_idx + 1 < s1_idx) {
        std::rotate(insns->begin() + static_cast<ptrdiff_t>(m_idx + 1),
                    insns->begin() + static_cast<ptrdiff_t>(s1_idx),
                    insns->begin() + static_cast<ptrdiff_t>(s1_idx + 1));
        cur_s1 = m_idx + 1;
        modified = true;
      }
      // Also re-adjoin a `sched2`-sunk secondary spill store
      // `(set (mem:DI [sp/fp+off]) (reg:DI dst_gpr))` back immediately after `s1`
      // (`reload1.c:6631`: `emit_reload_insns` emits `s1` and `s2` back-to-back).
      for (size_t s2_idx = cur_s1 + 2; s2_idx < insns->size(); ++s2_idx) {
        const RtlExpr& s2 = (*insns)[s2_idx].parsed.pattern;
        int sbase = -1;
        int64_t soff = 0;
        int64_t ssz = 0;
        if (s2.IsSet() && s2.SetDest().IsMem() && !s2.SetDest().flags.volatil &&
            !s2.SetDest().flags.in_struct && s2.SetDest().mode == RtlMachineMode::kDi &&
            s2.SetSrc().IsHardReg() && s2.SetSrc().mode == RtlMachineMode::kDi &&
            s2.SetSrc().RegNo() == dst_gpr && !HasRegDeadNoteFor((*insns)[s2_idx], dst_gpr) &&
            ExtractStackBaseDispMem(s2.SetDest(), &sbase, &soff, &ssz) && sbase == expected_base &&
            soff >= std::max<int64_t>(0, cfg.sideband.outgoing_args_size) &&
            (min_prologue_save_off <= 0 || soff < min_prologue_save_off) && (soff % 8) == 0) {
          // `schedule_block` (`sched.c:3144`) can place an independent non-`/u`
          // load into the `mult:DI` latency gap; after `s1` re-adjoins to
          // `m_idx + 1`, that load sits in `(cur_s1, s2_idx)` and blocks `s2`
          // via anti-dependence. If every blocker in `(cur_s1, s2_idx)` is an
          // independent non-`/u` load with no hazard against `[m_idx, k)`,
          // hoist those loads ahead of `m_idx` (preserving their anti-dependence
          // against `s2` without a speculative `/u` flag) and then re-adjoin `s2`.
          bool can_hoist_s2 = true;
          std::vector<size_t> hoistable_loads;
          for (size_t k = cur_s1 + 1; k < s2_idx; ++k) {
            const RtlExpr& kpat = (*insns)[k].parsed.pattern;
            if (!SetsAreDisjoint((*insns)[k].write_regs, dst_words) || IsCallExpr(kpat) ||
                IsJumpOrReturnExpr(kpat)) {
              can_hoist_s2 = false;
              break;
            }
            if (!HasStrictInsnOrderHazard((*insns)[k], (*insns)[s2_idx], gp64, fp64)) {
              continue;
            }
            if (!(*insns)[k].reads_mem || (*insns)[k].writes_mem || (*insns)[k].has_side_effects ||
                !kpat.IsSet() || !kpat.SetSrc().IsMem() || kpat.SetSrc().flags.unchanging) {
              can_hoist_s2 = false;
              break;
            }
            bool can_hoist_before_m = true;
            for (size_t j = m_idx; j < k; ++j) {
              if (HasStrictInsnOrderHazard((*insns)[j], (*insns)[k], gp64, fp64)) {
                can_hoist_before_m = false;
                break;
              }
            }
            if (!can_hoist_before_m) {
              can_hoist_s2 = false;
              break;
            }
            hoistable_loads.push_back(k);
          }
          if (can_hoist_s2) {
            for (size_t k : hoistable_loads) {
              std::rotate(insns->begin() + static_cast<ptrdiff_t>(m_idx),
                          insns->begin() + static_cast<ptrdiff_t>(k),
                          insns->begin() + static_cast<ptrdiff_t>(k + 1));
              ++m_idx;
              ++cur_s1;
            }
            std::rotate(insns->begin() + static_cast<ptrdiff_t>(cur_s1 + 1),
                        insns->begin() + static_cast<ptrdiff_t>(s2_idx),
                        insns->begin() + static_cast<ptrdiff_t>(s2_idx + 1));
            modified = true;
          }
          break;
        }
        if (!SetsAreDisjoint((*insns)[s2_idx].write_regs, dst_words) || IsCallExpr(s2) ||
            IsJumpOrReturnExpr(s2)) {
          break;
        }
      }
    }
  }
  // Also re-adjoin `sched2`-sunk `mflo` (`=l` output reload)
  // `(set (reg:SI dst_gpr) (reg:SI 65 lo))` back immediately after its
  // producing `(parallel [(set (reg:SI 65 lo) (mult:SI ...)) ...])`
  // (`reload1.c:6583`, `config/mips/mips.md:848-858` `mulsi3_internal`).
  auto is_si_mult_to_lo = [](const RtlExpr& pat) {
    return pat.op == RtlOp::kParallel && !pat.children.empty() && pat.children[0].IsSet() &&
           pat.children[0].SetDest().IsHardReg() &&
           pat.children[0].SetDest().mode == RtlMachineMode::kSi &&
           pat.children[0].SetDest().RegNo() == 65 && pat.children[0].SetSrc().op == RtlOp::kMult;
  };
  for (size_t s1_idx = 2; s1_idx < insns->size(); ++s1_idx) {
    const RtlExpr& s1 = (*insns)[s1_idx].parsed.pattern;
    if (!s1.IsSet() || !s1.SetDest().IsHardReg() || s1.SetDest().flags.integrated ||
        s1.SetDest().mode != RtlMachineMode::kSi || !s1.SetSrc().IsHardReg() ||
        s1.SetSrc().mode != RtlMachineMode::kSi || s1.SetSrc().RegNo() != 65 ||
        !(*insns)[s1_idx].parsed.reg_notes.empty()) {
      continue;
    }
    const int dst_gpr = s1.SetDest().RegNo();
    if (dst_gpr < 2 || dst_gpr > 25) {
      continue;
    }
    // If a subsequent instruction before the next write/clobber of `65 lo`
    // carries `(REG_DEAD (reg:SI 65 lo))`, the pseudo was allocated to `65 lo`
    // (`LO_REG`) and this `mflo` is an input reload (`reload1.c:emit_reload_insns`)
    // anchored before its consumer rather than a `sched2`-sunk output reload.
    bool is_lo_input_reload = false;
    for (size_t j = s1_idx + 1; j < insns->size(); ++j) {
      for (const RtlRegNote& note : (*insns)[j].parsed.reg_notes) {
        if (note.kind == RtlMachineMode::kRegDead && note.datum.IsHardReg() &&
            note.datum.RegNo() == 65) {
          is_lo_input_reload = true;
          break;
        }
      }
      if (is_lo_input_reload || (*insns)[j].write_regs.contains(65) ||
          IsCallExpr((*insns)[j].parsed.pattern) ||
          IsJumpOrReturnExpr((*insns)[j].parsed.pattern)) {
        break;
      }
    }
    if (is_lo_input_reload) {
      continue;
    }
    const absl::flat_hash_set<int> words = {65, dst_gpr};
    size_t m_idx = insns->size();
    bool ok_scan = true;
    for (size_t rev = s1_idx; rev > 0; --rev) {
      const size_t k = rev - 1;
      const RtlExpr& kpat = (*insns)[k].parsed.pattern;
      if (is_si_mult_to_lo(kpat)) {
        m_idx = k;
        break;
      }
      if (!SetsAreDisjoint((*insns)[k].read_regs, words) ||
          !SetsAreDisjoint((*insns)[k].write_regs, words) || IsCallExpr(kpat) ||
          IsJumpOrReturnExpr(kpat) ||
          HasStrictInsnOrderHazard((*insns)[k], (*insns)[s1_idx], gp64, fp64)) {
        ok_scan = false;
        break;
      }
    }
    if (ok_scan && m_idx < insns->size() && m_idx + 1 < s1_idx) {
      std::rotate(insns->begin() + static_cast<ptrdiff_t>(m_idx + 1),
                  insns->begin() + static_cast<ptrdiff_t>(s1_idx),
                  insns->begin() + static_cast<ptrdiff_t>(s1_idx + 1));
      modified = true;
    }
  }
  return modified;
}

void UnscheduleSched2ReloadsAndCallerSaves(RtlControlFlowGraph* cfg, bool gp64, bool fp64) {
  if (cfg->rpo_blocks.empty()) {
    return;
  }
  const size_t n_blocks = cfg->rpo_blocks.size();
  std::vector<std::vector<ParsedBlockInsn>> block_insns(n_blocks);
  for (size_t b = 0; b < n_blocks; ++b) {
    const RtlBasicBlock& blk = cfg->rpo_blocks[b];
    block_insns[b].reserve(blk.instructions.size());
    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      auto parsed = ParsePatternAndNotes(blk.instructions[i]);
      if (!parsed.ok()) {
        return;
      }
      std::optional<RtlInsn> orig_insn =
          (i < blk.rtl_insns.size()) ? std::optional<RtlInsn>(blk.rtl_insns[i]) : std::nullopt;
      block_insns[b].push_back(BuildParsedBlockInsn(std::move(*parsed), gp64, fp64, orig_insn));
    }
  }

  int min_prologue_save_off = 0;
  absl::flat_hash_set<int64_t> caller_save_offsets;
  absl::flat_hash_set<int64_t> proven_reload_spill_offsets;
  IdentifySched2CallerSaveAndSpillOffsets(*cfg, block_insns, gp64, fp64, &min_prologue_save_off,
                                          &caller_save_offsets, &proven_reload_spill_offsets);
  const int min_gpr_spill_reg =
      FindMinProvenGprReloadSpillReg(block_insns, proven_reload_spill_offsets);

  for (size_t b = 0; b < n_blocks; ++b) {
    auto& insns = block_insns[b];
    bool modified = false;

    auto match_sp_off_reg_move = [](const ParsedBlockInsn& insn, bool want_store, int64_t* out_off,
                                    int* out_reg) {
      const RtlExpr& pat = insn.parsed.pattern;
      if (pat.op != RtlOp::kSet || pat.children.size() != 2) {
        return false;
      }
      const RtlExpr& mem_side = want_store ? pat.children[0] : pat.children[1];
      const RtlExpr& reg_side = want_store ? pat.children[1] : pat.children[0];
      int base = -1;
      int64_t off = 0, sz = 0;
      if (reg_side.op != RtlOp::kReg || reg_side.flags.integrated ||
          !ExtractStackBaseDispMem(mem_side, &base, &off, &sz) || base != kMipsSpRegno) {
        return false;
      }
      *out_off = off;
      *out_reg = reg_side.RegNo();
      return true;
    };

    // (a) Sink `caller-save` stores down immediately before their `CALL_INSN`.
    if (!caller_save_offsets.empty()) {
      size_t seg_start = 0;
      for (size_t call_idx = 0; call_idx < insns.size(); ++call_idx) {
        if (!IsCallExpr(insns[call_idx].parsed.pattern)) {
          continue;
        }
        for (size_t rev = call_idx; rev > seg_start; --rev) {
          const size_t idx = rev - 1;
          int64_t off = 0;
          int cs_r = -1;
          if (!match_sp_off_reg_move(insns[idx], /*want_store=*/true, &off, &cs_r) ||
              !caller_save_offsets.contains(off)) {
            continue;
          }
          {
            auto& notes = insns[idx].parsed.reg_notes;
            const size_t before_n = notes.size();
            notes.erase(std::remove_if(notes.begin(), notes.end(),
                                       [&](const RtlRegNote& n) {
                                         return n.kind == RtlMachineMode::kRegDead &&
                                                n.datum.op == RtlOp::kReg &&
                                                n.datum.RegNo() == cs_r;
                                       }),
                        notes.end());
            if (notes.size() != before_n) {
              modified = true;
            }
          }
          size_t dst = idx;
          while (dst + 1 < call_idx &&
                 !HasStrictInsnOrderHazard(insns[dst], insns[dst + 1], gp64, fp64)) {
            int64_t next_off = 0;
            int next_r = -1;
            if (match_sp_off_reg_move(insns[dst + 1], /*want_store=*/true, &next_off, &next_r) &&
                caller_save_offsets.contains(next_off) && cs_r < next_r) {
              break;
            }
            std::swap(insns[dst], insns[dst + 1]);
            ++dst;
            modified = true;
          }
        }
        seg_start = call_idx + 1;
      }

      // (b1) Forward pass (`caller-save.c:559-646` `insert_restore` + `sched.c`):
      // hoist each caller-save restore's non-memory first post-call user right
      // after its restore in forward order so earlier restored chains (e.g.
      // `lw $6; xor $6`, `lw $7; addu $7`) move ahead of later restores before
      // dependent later users (`subu $8`, `xor $9`) are hoisted.
      for (size_t i = 0; i < insns.size(); ++i) {
        int64_t off = 0;
        int cs_r = -1;
        if (!match_sp_off_reg_move(insns[i], /*want_store=*/false, &off, &cs_r) ||
            !caller_save_offsets.contains(off)) {
          continue;
        }
        size_t first_stop = insns.size();
        for (size_t k = i + 1; k < insns.size(); ++k) {
          if (insns[k].read_regs.contains(cs_r) || insns[k].write_regs.contains(cs_r) ||
              IsCallExpr(insns[k].parsed.pattern) || IsJumpOrReturnExpr(insns[k].parsed.pattern)) {
            first_stop = k;
            break;
          }
        }
        if (first_stop <= i + 1 || first_stop >= insns.size() ||
            IsCallExpr(insns[first_stop].parsed.pattern) ||
            IsJumpOrReturnExpr(insns[first_stop].parsed.pattern) ||
            insns[first_stop].parsed.pattern.op == RtlOp::kUse ||
            insns[first_stop].parsed.pattern.op == RtlOp::kClobber || insns[first_stop].reads_mem ||
            insns[first_stop].writes_mem) {
          continue;
        }
        bool can_hoist_user = true;
        for (size_t k = i + 1; k < first_stop; ++k) {
          if (HasStrictInsnOrderHazard(insns[k], insns[first_stop], gp64, fp64)) {
            can_hoist_user = false;
            break;
          }
        }
        if (can_hoist_user) {
          std::rotate(insns.begin() + static_cast<ptrdiff_t>(i + 1),
                      insns.begin() + static_cast<ptrdiff_t>(first_stop),
                      insns.begin() + static_cast<ptrdiff_t>(first_stop + 1));
          modified = true;
        }
      }

      // (b2) Sink remaining `caller-save` restores down immediately before their
      // first post-call user, iterating backwards so rotating an earlier restore
      // down does not skip its immediate successor, and preserving ascending
      // `0..63` `caller-save.c` register order when multiple restores share the
      // same stopper.
      for (size_t rev = insns.size(); rev > 0; --rev) {
        const size_t i = rev - 1;
        int64_t off = 0;
        int cs_r = -1;
        if (!match_sp_off_reg_move(insns[i], /*want_store=*/false, &off, &cs_r) ||
            !caller_save_offsets.contains(off)) {
          continue;
        }
        size_t first_stop = insns.size();
        for (size_t k = i + 1; k < insns.size(); ++k) {
          if (insns[k].read_regs.contains(cs_r) || insns[k].write_regs.contains(cs_r) ||
              IsCallExpr(insns[k].parsed.pattern) || IsJumpOrReturnExpr(insns[k].parsed.pattern)) {
            first_stop = k;
            break;
          }
        }
        size_t target = first_stop;
        while (target > i + 1) {
          int64_t prev_off = 0;
          int prev_r = -1;
          if (!match_sp_off_reg_move(insns[target - 1], /*want_store=*/false, &prev_off, &prev_r) ||
              !caller_save_offsets.contains(prev_off) || prev_r <= cs_r) {
            break;
          }
          bool prev_used_before_stop = false;
          for (size_t m = target; m < first_stop; ++m) {
            if (insns[m].read_regs.contains(prev_r) || insns[m].write_regs.contains(prev_r)) {
              prev_used_before_stop = true;
              break;
            }
          }
          if (prev_used_before_stop) {
            break;
          }
          --target;
        }
        if (target > i + 1) {
          bool can_sink = true;
          for (size_t k = i + 1; k < target; ++k) {
            if (HasStrictInsnOrderHazard(insns[i], insns[k], gp64, fp64)) {
              can_sink = false;
              break;
            }
          }
          if (can_sink) {
            std::rotate(insns.begin() + static_cast<ptrdiff_t>(i),
                        insns.begin() + static_cast<ptrdiff_t>(i + 1),
                        insns.begin() + static_cast<ptrdiff_t>(target));
            modified = true;
          }
        }
      }
    }

    // (c) Re-adjoin separated single-use cross-bank `mtc1` reloads and proven
    // `reload` stack-spill loads with their single consumer.
    absl::flat_hash_set<int> use_pinned_regs;
    for (const ParsedBlockInsn& insn : insns) {
      if (insn.parsed.pattern.op == RtlOp::kUse && insn.parsed.pattern.children.size() == 1 &&
          insn.parsed.pattern.children[0].op == RtlOp::kReg) {
        InsertRegSpan(insn.parsed.pattern.children[0].RegNo(), insn.parsed.pattern.children[0].mode,
                      gp64, fp64, &use_pinned_regs);
      }
    }

    auto is_dead_after_consumer = [&](size_t cons_idx, const absl::flat_hash_set<int>& words) {
      for (int w : words) {
        if (insns[cons_idx].write_regs.contains(w) || HasRegDeadNoteFor(insns[cons_idx], w)) {
          continue;
        }
        bool killed_in_blk = false;
        for (size_t k = cons_idx + 1; k < insns.size(); ++k) {
          if (insns[k].read_regs.contains(w) || (IsCallExpr(insns[k].parsed.pattern) &&
                                                 CallInsnReadsArgReg(insns[k], w, gp64, fp64))) {
            return false;
          }
          if (insns[k].write_regs.contains(w) ||
              (IsCallExpr(insns[k].parsed.pattern) && IsMipsCallerSavableReg(w))) {
            killed_in_blk = true;
            break;
          }
        }
        if (!killed_in_blk) {
          for (size_t succ : cfg->rpo_blocks[b].successors) {
            const auto lit = cfg->sideband.basic_block_live_at_start.find(static_cast<int>(succ));
            if (lit != cfg->sideband.basic_block_live_at_start.end() && lit->second.contains(w)) {
              return false;
            }
          }
          if (cfg->rpo_blocks[b].successors.empty() && (w == 2 || w == 3 || w == 32 || w == 33)) {
            return false;
          }
        }
      }
      return true;
    };

    const absl::flat_hash_set<int> block_proven_fp_spill_regs = CollectBlockProvenFpSpillRegs(
        *cfg, b, insns, proven_reload_spill_offsets, use_pinned_regs, gp64, fp64);

    auto match_single_use_input_reload = [&](size_t idx, bool* is_mtc1_out, bool* is_cd_out,
                                             int* rel_reg_out, RtlMachineMode* rel_mode_out) {
      *is_mtc1_out = false;
      *is_cd_out = false;
      *rel_reg_out = -1;
      *rel_mode_out = RtlMachineMode::kNone;
      const RtlExpr& pat = insns[idx].parsed.pattern;
      if (pat.op == RtlOp::kSet && pat.children.size() == 2 && pat.children[0].op == RtlOp::kReg &&
          !pat.children[0].flags.integrated &&
          (pat.children[0].mode == RtlMachineMode::kSf ||
           pat.children[0].mode == RtlMachineMode::kDf) &&
          pat.children[0].RegNo() >= 32 && pat.children[0].RegNo() < 64 &&
          !use_pinned_regs.contains(pat.children[0].RegNo())) {
        if (pat.children[1].op == RtlOp::kReg && pat.children[1].mode == pat.children[0].mode &&
            pat.children[1].RegNo() >= 2 && pat.children[1].RegNo() < 32) {
          *is_mtc1_out = true;
          *rel_reg_out = pat.children[0].RegNo();
          *rel_mode_out = pat.children[0].mode;
          return true;
        }
        if (pat.children[1].op == RtlOp::kConstDouble &&
            block_proven_fp_spill_regs.contains(pat.children[0].RegNo())) {
          *is_cd_out = true;
          *rel_reg_out = pat.children[0].RegNo();
          *rel_mode_out = pat.children[0].mode;
          return true;
        }
      }
      int64_t off = 0;
      int dst_r = -1;
      if (match_sp_off_reg_move(insns[idx], /*want_store=*/false, &off, &dst_r) &&
          proven_reload_spill_offsets.contains(off) && dst_r >= 2 && dst_r < 64 &&
          !use_pinned_regs.contains(dst_r)) {
        *rel_reg_out = dst_r;
        *rel_mode_out = pat.children[0].mode;
        return true;
      }
      return false;
    };

    for (size_t rev = insns.size(); rev > 0; --rev) {
      const size_t i = rev - 1;
      if (i + 2 >= insns.size()) {
        continue;
      }
      bool is_mtc1_reload = false;
      bool is_cd_reload = false;
      int rel_reg = -1;
      RtlMachineMode rel_mode = RtlMachineMode::kNone;
      if (!match_single_use_input_reload(i, &is_mtc1_reload, &is_cd_reload, &rel_reg, &rel_mode)) {
        continue;
      }

      absl::flat_hash_set<int> rel_words;
      InsertRegSpan(rel_reg, rel_mode, gp64, fp64, &rel_words);

      size_t cons_idx = insns.size();
      bool valid_single_use = true;
      for (size_t k = i + 1; k < insns.size(); ++k) {
        const bool reads_rel = !SetsAreDisjoint(insns[k].read_regs, rel_words) ||
                               (IsCallExpr(insns[k].parsed.pattern) &&
                                CallInsnReadsArgReg(insns[k], rel_reg, gp64, fp64));
        const bool writes_rel = !SetsAreDisjoint(insns[k].write_regs, rel_words);
        if (reads_rel) {
          cons_idx = k;
          break;
        }
        if (writes_rel || IsCallExpr(insns[k].parsed.pattern) ||
            IsJumpOrReturnExpr(insns[k].parsed.pattern)) {
          valid_single_use = false;
          break;
        }
      }
      if (!valid_single_use || cons_idx <= i + 1 || cons_idx >= insns.size()) {
        continue;
      }
      const RtlExpr& cpat = insns[cons_idx].parsed.pattern;
      const bool cons_is_jump = IsJumpOrReturnExpr(cpat);
      const bool dead_after_cons = is_dead_after_consumer(cons_idx, rel_words);
      const int mtc1_src_r = is_mtc1_reload ? insns[i].parsed.pattern.children[1].RegNo() : -1;
      const bool allow_inherited_mtc1_sink =
          is_mtc1_reload && ((mtc1_src_r >= 16 && mtc1_src_r <= 23) || mtc1_src_r == 30) &&
          !dead_after_cons && !HasRegDeadNoteFor(insns[cons_idx], rel_reg);
      if (IsCallExpr(cpat) || (cons_is_jump && (is_mtc1_reload || is_cd_reload)) ||
          cpat.op == RtlOp::kUse || cpat.op == RtlOp::kClobber ||
          (!dead_after_cons && !allow_inherited_mtc1_sink)) {
        continue;
      }
      if (is_cd_reload && HasRegDeadNoteFor(insns[cons_idx], rel_reg)) {
        continue;
      }
      if ((is_mtc1_reload || is_cd_reload) && cpat.op == RtlOp::kSet && cpat.children.size() == 2 &&
          cpat.children[1].op == RtlOp::kReg) {
        continue;
      }

      bool can_hoist_cons = dead_after_cons && !cons_is_jump && !insns[cons_idx].reads_mem &&
                            !insns[cons_idx].writes_mem;
      for (size_t k = i + 1; k < cons_idx && can_hoist_cons; ++k) {
        if (HasStrictInsnOrderHazard(insns[k], insns[cons_idx], gp64, fp64)) {
          can_hoist_cons = false;
        }
      }
      if (can_hoist_cons) {
        std::rotate(insns.begin() + static_cast<ptrdiff_t>(i + 1),
                    insns.begin() + static_cast<ptrdiff_t>(cons_idx),
                    insns.begin() + static_cast<ptrdiff_t>(cons_idx + 1));
        modified = true;
        continue;
      }

      size_t sink_end = cons_idx;
      const int rel_ord = FirstReadRegPreorderIndex(cpat, rel_words);
      while (sink_end > i + 1) {
        bool sib_mtc1 = false;
        bool sib_cd = false;
        int sib_reg = -1;
        RtlMachineMode sib_mode = RtlMachineMode::kNone;
        if (!match_single_use_input_reload(sink_end - 1, &sib_mtc1, &sib_cd, &sib_reg, &sib_mode)) {
          break;
        }
        absl::flat_hash_set<int> sib_words;
        InsertRegSpan(sib_reg, sib_mode, gp64, fp64, &sib_words);
        bool touched_between = false;
        for (size_t m = sink_end; m < cons_idx; ++m) {
          if (!SetsAreDisjoint(insns[m].read_regs, sib_words) ||
              !SetsAreDisjoint(insns[m].write_regs, sib_words)) {
            touched_between = true;
            break;
          }
        }
        if (touched_between || SetsAreDisjoint(insns[cons_idx].read_regs, sib_words) ||
            !is_dead_after_consumer(cons_idx, sib_words)) {
          break;
        }
        const int sib_ord = FirstReadRegPreorderIndex(cpat, sib_words);
        if (rel_ord > 0 && sib_ord > 0 && rel_ord > sib_ord &&
            SetsAreDisjoint(insns[cons_idx].write_regs, rel_words)) {
          break;
        }
        --sink_end;
      }
      if (sink_end <= i + 1) {
        continue;
      }

      bool can_sink_rel = true;
      for (size_t k = i + 1; k < sink_end && can_sink_rel; ++k) {
        if (HasStrictInsnOrderHazard(insns[i], insns[k], gp64, fp64)) {
          can_sink_rel = false;
        }
      }
      if (can_sink_rel) {
        std::rotate(insns.begin() + static_cast<ptrdiff_t>(i),
                    insns.begin() + static_cast<ptrdiff_t>(i + 1),
                    insns.begin() + static_cast<ptrdiff_t>(sink_end));
        modified = true;
      }
    }

    if (CanonicalizeAdjacentSpillInputReloadPairs(*cfg, b, gp64, fp64, proven_reload_spill_offsets,
                                                  use_pinned_regs, &insns)) {
      modified = true;
    }
    if (RestoreCrossBankReloadSpillRoundRobinOrder(*cfg, b, gp64, fp64, min_gpr_spill_reg,
                                                   proven_reload_spill_offsets, use_pinned_regs,
                                                   &insns)) {
      modified = true;
    }

    if (NormalizeSpillOutputReloadRegNotesInBlock(*cfg, b, gp64, fp64, proven_reload_spill_offsets,
                                                  &insns)) {
      modified = true;
    }

    if (ReadjoinSunkSpillStoresInBlock(proven_reload_spill_offsets, use_pinned_regs,
                                       match_sp_off_reg_move, is_dead_after_consumer, gp64, fp64,
                                       &insns)) {
      modified = true;
    }

    if (ReadjoinCrossBankFprOutputReloads(use_pinned_regs, is_dead_after_consumer,
                                          (b == 0) ? min_prologue_save_off : 0, caller_save_offsets,
                                          proven_reload_spill_offsets, gp64, fp64, &insns)) {
      modified = true;
    }

    // Re-adjoin `mulsidi3` / `umulsidi3` secondary output reload + spill store
    // pairs `(set gpr (reg:DI 64 hi)) ; (set [sp/fp+off] gpr)` back immediately
    // after the producing `(parallel [(set (reg:DI 64 hi) (mult:DI ...)) ...])`
    // when `sched2` scheduled independent instructions into the multiply latency
    // shadow between `mult:DI` and `mflo`/`mfhi`.
    for (size_t s1_idx = 2; s1_idx + 1 < insns.size(); ++s1_idx) {
      const RtlExpr& s1 = insns[s1_idx].parsed.pattern;
      const RtlExpr& s2 = insns[s1_idx + 1].parsed.pattern;
      if (!s1.IsSet() || !s1.SetDest().IsHardReg() || s1.SetDest().mode != RtlMachineMode::kDi ||
          !s1.SetSrc().IsHardReg() || s1.SetSrc().mode != RtlMachineMode::kDi ||
          s1.SetSrc().RegNo() != 64 || !insns[s1_idx].parsed.reg_notes.empty()) {
        continue;
      }
      const int sec_gpr = s1.SetDest().RegNo();
      if (sec_gpr < 2 || sec_gpr > 24 || use_pinned_regs.contains(sec_gpr)) {
        continue;
      }
      int sbase = -1;
      int64_t soff = 0;
      int64_t ssz = 0;
      const int expected_base = cfg->sideband.frame_pointer_needed ? kMipsFpRegno : kMipsSpRegno;
      if (!s2.IsSet() || !s2.SetDest().IsMem() || s2.SetDest().flags.volatil ||
          s2.SetDest().flags.in_struct || s2.SetDest().mode != RtlMachineMode::kDi ||
          !s2.SetSrc().IsHardReg() || s2.SetSrc().mode != RtlMachineMode::kDi ||
          s2.SetSrc().RegNo() != sec_gpr || HasRegDeadNoteFor(insns[s1_idx + 1], sec_gpr) ||
          !ExtractStackBaseDispMem(s2.SetDest(), &sbase, &soff, &ssz) || sbase != expected_base ||
          soff < std::max<int64_t>(0, cfg->sideband.outgoing_args_size) ||
          (min_prologue_save_off > 0 && soff >= min_prologue_save_off) || (soff % 8) != 0) {
        continue;
      }
      absl::flat_hash_set<int> pair_words = {64, 65, sec_gpr, sec_gpr + 1};
      size_t m_idx = insns.size();
      bool ok_scan = true;
      for (size_t rev = s1_idx; rev > 0; --rev) {
        const size_t k = rev - 1;
        const RtlExpr& kpat = insns[k].parsed.pattern;
        if (kpat.op == RtlOp::kParallel && !kpat.children.empty() && kpat.children[0].IsSet() &&
            kpat.children[0].SetDest().IsHardReg() &&
            kpat.children[0].SetDest().mode == RtlMachineMode::kDi &&
            kpat.children[0].SetDest().RegNo() == 64 &&
            kpat.children[0].SetSrc().op == RtlOp::kMult) {
          m_idx = k;
          break;
        }
        if (!SetsAreDisjoint(insns[k].read_regs, pair_words) ||
            !SetsAreDisjoint(insns[k].write_regs, pair_words) || IsCallExpr(kpat) ||
            IsJumpOrReturnExpr(kpat)) {
          ok_scan = false;
          break;
        }
      }
      if (!ok_scan || m_idx >= insns.size() || m_idx + 1 >= s1_idx) {
        continue;
      }
      bool can_hoist_pair = true;
      for (size_t k = m_idx + 1; k < s1_idx && can_hoist_pair; ++k) {
        if (HasStrictInsnOrderHazard(insns[k], insns[s1_idx], gp64, fp64) ||
            HasStrictInsnOrderHazard(insns[k], insns[s1_idx + 1], gp64, fp64)) {
          can_hoist_pair = false;
        }
      }
      if (can_hoist_pair) {
        std::rotate(insns.begin() + static_cast<ptrdiff_t>(m_idx + 1),
                    insns.begin() + static_cast<ptrdiff_t>(s1_idx),
                    insns.begin() + static_cast<ptrdiff_t>(s1_idx + 2));
        modified = true;
      }
    }
    if (ReadjoinMdDiOutputReloads(*cfg, (b == 0) ? min_prologue_save_off : 0, use_pinned_regs, gp64,
                                  fp64, &insns)) {
      modified = true;
    }

    // (c3) Re-adjoin `sched2`-separated single-use `"=f"` FPR->GPR output
    // reload moves `(set (reg:SF/DF gpr) (reg:SF/DF fpr))` immediately after
    // their FP arithmetic producer `(set (reg:SF/DF fpr) <fp_op>)`.
    for (size_t i = 2; i < insns.size(); ++i) {
      const RtlExpr& mpat = insns[i].parsed.pattern;
      if (mpat.op != RtlOp::kSet || mpat.children.size() != 2 ||
          mpat.children[0].op != RtlOp::kReg || mpat.children[1].op != RtlOp::kReg ||
          mpat.children[0].flags.integrated || mpat.children[1].flags.integrated ||
          mpat.children[0].mode != mpat.children[1].mode ||
          (mpat.children[0].mode != RtlMachineMode::kSf &&
           mpat.children[0].mode != RtlMachineMode::kDf)) {
        continue;
      }
      const int gpr = mpat.children[0].RegNo();
      const int fpr = mpat.children[1].RegNo();
      const bool valid_gpr =
          (gpr >= 2 && gpr <= 25) || (gpr == kMipsFpRegno && !cfg->sideband.frame_pointer_needed);
      if (!valid_gpr || fpr < 32 || fpr > 63 || use_pinned_regs.contains(fpr) ||
          HasRegDeadNoteFor(insns[i], fpr)) {
        continue;
      }
      absl::flat_hash_set<int> fpr_words;
      InsertRegSpan(fpr, mpat.children[1].mode, gp64, fp64, &fpr_words);
      if (!is_dead_after_consumer(i, fpr_words)) {
        continue;
      }
      for (size_t rev = i; rev > 0; --rev) {
        const size_t k = rev - 1;
        if (!SetsAreDisjoint(insns[k].write_regs, fpr_words)) {
          const RtlExpr& ppat = insns[k].parsed.pattern;
          if (k + 1 < i && ppat.op == RtlOp::kSet && ppat.children.size() == 2 &&
              ppat.children[0].op == RtlOp::kReg && ppat.children[0].RegNo() == fpr &&
              ppat.children[0].mode == mpat.children[1].mode &&
              !ppat.children[0].flags.integrated && !insns[k].reads_mem && !insns[k].writes_mem) {
            const RtlOp rop = ppat.children[1].op;
            if (rop != RtlOp::kReg && rop != RtlOp::kMem && rop != RtlOp::kConstDouble &&
                rop != RtlOp::kCall && rop != RtlOp::kSubreg) {
              bool can_hoist = true;
              for (size_t m = k + 1; m < i && can_hoist; ++m) {
                if (HasStrictInsnOrderHazard(insns[m], insns[i], gp64, fp64)) {
                  can_hoist = false;
                }
              }
              if (can_hoist) {
                std::rotate(insns.begin() + static_cast<ptrdiff_t>(k + 1),
                            insns.begin() + static_cast<ptrdiff_t>(i),
                            insns.begin() + static_cast<ptrdiff_t>(i + 1));
                modified = true;
              } else if (SetsAreDisjoint(insns[k].read_regs, fpr_words)) {
                bool can_sink_prod = true;
                for (size_t m = k + 1; m < i && can_sink_prod; ++m) {
                  if (HasStrictInsnOrderHazard(insns[k], insns[m], gp64, fp64)) {
                    can_sink_prod = false;
                  }
                }
                if (can_sink_prod) {
                  std::rotate(insns.begin() + static_cast<ptrdiff_t>(k),
                              insns.begin() + static_cast<ptrdiff_t>(k + 1),
                              insns.begin() + static_cast<ptrdiff_t>(i));
                  modified = true;
                }
              }
            }
          }
          break;
        }
        if (!SetsAreDisjoint(insns[k].read_regs, fpr_words) ||
            IsCallExpr(insns[k].parsed.pattern) || IsJumpOrReturnExpr(insns[k].parsed.pattern)) {
          break;
        }
      }
    }

    if (HoistSched2SunkMdOutputReloadsInBlock(*cfg, gp64, fp64, use_pinned_regs,
                                              is_dead_after_consumer, &insns)) {
      modified = true;
    }

    if (RestoreCrossBankReloadOtherDeadNotesInBlock(*cfg, gp64, fp64, use_pinned_regs,
                                                    is_dead_after_consumer, &insns)) {
      modified = true;
    }

    // Normalize `sched2`-rewritten `REG_DEAD (reg rel_reg)` notes on adjacent
    // single-use `proven_reload_spill_offsets` consumers back to `.09.greg`'s
    // `REG_DEAD (mem [sp+off])` (when the spill slot dies at `i + 1`) or no
    // reload-scratch `REG_DEAD` note (when the spill slot remains live).
    if (!proven_reload_spill_offsets.empty()) {
      auto insn_accesses_sp_off = [](const RtlExpr& root, int64_t target_off, bool* reads_slot,
                                     bool* writes_slot) {
        auto walk = [&](const auto& self, const RtlExpr& e, bool in_dst) -> void {
          if (e.op == RtlOp::kMem) {
            int base = -1;
            int64_t off = 0, sz = 0;
            if (ExtractStackBaseDispMem(e, &base, &off, &sz) && base == kMipsSpRegno &&
                off == target_off) {
              if (in_dst) {
                *writes_slot = true;
              } else {
                *reads_slot = true;
              }
            }
            return;
          }
          for (size_t ci = 0; ci < e.children.size(); ++ci) {
            self(self, e.children[ci], e.op == RtlOp::kSet ? (ci == 0) : in_dst);
          }
        };
        walk(walk, root, false);
      };
      auto slot_dead_after = [&](size_t start_idx, int64_t target_off) {
        for (size_t k = start_idx + 1; k < insns.size(); ++k) {
          bool rd = false, wr = false;
          insn_accesses_sp_off(insns[k].parsed.pattern, target_off, &rd, &wr);
          if (rd) {
            return false;
          }
          if (wr) {
            return true;
          }
        }
        std::vector<size_t> q(cfg->rpo_blocks[b].successors.begin(),
                              cfg->rpo_blocks[b].successors.end());
        std::vector<bool> vis(n_blocks, false);
        for (size_t s : q) {
          if (s < n_blocks) vis[s] = true;
        }
        for (size_t qi = 0; qi < q.size(); ++qi) {
          const size_t sb = q[qi];
          if (sb >= n_blocks) continue;
          const auto& s_insns = (sb == b) ? insns : block_insns[sb];
          bool killed = false;
          for (const ParsedBlockInsn& si : s_insns) {
            bool rd = false, wr = false;
            insn_accesses_sp_off(si.parsed.pattern, target_off, &rd, &wr);
            if (rd) {
              return false;
            }
            if (wr) {
              killed = true;
              break;
            }
          }
          if (!killed) {
            for (size_t nx : cfg->rpo_blocks[sb].successors) {
              if (nx < n_blocks && !vis[nx]) {
                vis[nx] = true;
                q.push_back(nx);
              }
            }
          }
        }
        return true;
      };
      for (size_t i = 0; i + 1 < insns.size(); ++i) {
        int64_t off = 0;
        int rel_reg = -1;
        if (!match_sp_off_reg_move(insns[i], /*want_store=*/false, &off, &rel_reg) ||
            !proven_reload_spill_offsets.contains(off) || rel_reg < 2 || rel_reg >= 64 ||
            use_pinned_regs.contains(rel_reg) || !insns[i].parsed.reg_notes.empty()) {
          continue;
        }
        const RtlMachineMode rel_mode = insns[i].parsed.pattern.children[0].mode;
        absl::flat_hash_set<int> rel_words;
        InsertRegSpan(rel_reg, rel_mode, gp64, fp64, &rel_words);
        const RtlExpr& cpat = insns[i + 1].parsed.pattern;
        if (IsCallExpr(cpat) || IsJumpOrReturnExpr(cpat) || cpat.op == RtlOp::kUse ||
            cpat.op == RtlOp::kClobber || SetsAreDisjoint(insns[i + 1].read_regs, rel_words) ||
            !SetsAreDisjoint(insns[i + 1].write_regs, rel_words) ||
            (cpat.IsSet() && cpat.SetSrc().op == RtlOp::kReg) ||
            !is_dead_after_consumer(i + 1, rel_words)) {
          continue;
        }
        bool c_rd = false, c_wr = false;
        insn_accesses_sp_off(cpat, off, &c_rd, &c_wr);
        const bool slot_dies = !c_wr && slot_dead_after(i + 1, off);
        auto& notes = insns[i + 1].parsed.reg_notes;
        for (size_t ni = 0; ni < notes.size(); ++ni) {
          if (notes[ni].kind == RtlMachineMode::kRegDead && notes[ni].datum.op == RtlOp::kReg &&
              notes[ni].datum.RegNo() == rel_reg) {
            if (slot_dies) {
              notes[ni].datum = insns[i].parsed.pattern.children[1];
            } else {
              notes.erase(notes.begin() + static_cast<ptrdiff_t>(ni));
            }
            modified = true;
            break;
          }
        }
      }
    }

    if (StripEarlyclobberDiOutputReloadDeadNotesInBlock(*cfg, b, gp64, fp64, use_pinned_regs,
                                                        &insns)) {
      modified = true;
    }

    if (NormalizeSpillOutputReloadRegNotesInBlock(*cfg, b, gp64, fp64, proven_reload_spill_offsets,
                                                  &insns)) {
      modified = true;
    }

    if (modified) {
      RtlBasicBlock& blk = cfg->rpo_blocks[b];
      bool all_have_orig_rtl = true;
      for (size_t i = 0; i < insns.size(); ++i) {
        blk.instructions[i] = FormatGccPrintRtlPatternAndNotes(
            insns[i].parsed.pattern, insns[i].parsed.reg_notes, insns[i].parsed.bracketed_notes,
            /*call_fusage=*/{});
        if (!insns[i].orig_rtl_insn.has_value()) {
          all_have_orig_rtl = false;
        }
      }
      if (all_have_orig_rtl && blk.rtl_insns.size() == insns.size()) {
        for (size_t i = 0; i < insns.size(); ++i) {
          RtlInsn moved = std::move(*insns[i].orig_rtl_insn);
          moved.canonical_pattern = blk.instructions[i];
          blk.rtl_insns[i] = std::move(moved);
        }
      } else {
        blk.SyncRtlInsnsFromInstructions();
        for (size_t i = 0; i < blk.rtl_insns.size() && i < insns.size(); ++i) {
          if (insns[i].parsed.has_explicit_call_fusage &&
              !blk.rtl_insns[i].has_explicit_call_fusage) {
            blk.rtl_insns[i].has_explicit_call_fusage = true;
            blk.rtl_insns[i].call_fusage = insns[i].parsed.call_fusage;
          }
        }
      }
    }
  }
}

// =============================================================================
// Model-Guided Stage 6 (`kSched1`) Inversion & Stage 3 (`kSched2`) Selection
// =============================================================================

bool ExtractBasePseudoOrRegNo(const RtlExpr& x, int* out_regno) {
  if (x.op == RtlOp::kReg) {
    *out_regno = x.RegNo();
    return true;
  }
  if (x.op == RtlOp::kSubreg && !x.children.empty() && x.children[0].op == RtlOp::kReg) {
    *out_regno = x.children[0].RegNo();
    return true;
  }
  return false;
}

bool IsMipsLargeIntForSched1Split(int64_t val) {
  // Mirrors `config/mips/mips.c:416` (`large_int`): does not fit in signed
  // 16-bit, unsigned 16-bit, or a bare upper 16-bit `lui` constant.
  const uint32_t u32 = static_cast<uint32_t>(val);
  return (val < -32768 || val > 32767) && (val < 0 || val > 65535) && ((u32 & 0x0000ffffu) != 0u);
}

// Finds split `large_int` `lui`/`ori` pairs (`mips.md:3208-3221`) in `block` so
// `InvertBlockOrder(kSched1)` contracts them back into adjacent `lui; ori` pairs
// for `InvertUpdateFlowInfo` whenever compatible with exact forward `sched1` replay.
std::vector<sched_model::AdjacentPair> FindSplitLargeIntAdjacentPairs(
    absl::Span<const sched_model::Insn> block) {
  std::vector<sched_model::AdjacentPair> pairs;
  const int n = static_cast<int>(block.size());
  std::vector<bool> used(static_cast<size_t>(n), false);

  auto is_barrier = [](const sched_model::Insn& insn) {
    if (!insn.leading_loop_notes.empty()) {
      return true;
    }
    bool barrier = false;
    insn.pn.pattern.Visit([&](const RtlExpr& node) {
      if (node.op == RtlOp::kAsmInput || (node.op == RtlOp::kAsmOperands && node.flags.volatil) ||
          node.op == RtlOp::kUnspecVolatile ||
          (node.op == RtlOp::kUnknown && node.raw_op == "trap_if")) {
        barrier = true;
      }
    });
    return barrier;
  };

  for (int i = 0; i < n; ++i) {
    if (used[static_cast<size_t>(i)] ||
        block[static_cast<size_t>(i)].kind != sched_model::Insn::Kind::kInsn) {
      continue;
    }
    const RtlExpr& pat1 = block[static_cast<size_t>(i)].pn.pattern;
    if (pat1.op != RtlOp::kSet || pat1.SetSrc().op != RtlOp::kConstInt) {
      continue;
    }
    const RtlExpr& dst1 = pat1.SetDest();
    if (dst1.mode != RtlMachineMode::kSi) {
      continue;
    }
    int dest_regno = -1;
    if (!ExtractBasePseudoOrRegNo(dst1, &dest_regno)) {
      continue;
    }
    const uint32_t hi_u32 = static_cast<uint32_t>(pat1.SetSrc().int_val);
    if ((hi_u32 & 0x0000ffffu) != 0u) {
      continue;
    }

    for (int j = i + 1; j < n; ++j) {
      if (is_barrier(block[static_cast<size_t>(j)])) {
        break;
      }
      const RtlExpr& pat2 = block[static_cast<size_t>(j)].pn.pattern;
      if (!used[static_cast<size_t>(j)] &&
          block[static_cast<size_t>(j)].kind == sched_model::Insn::Kind::kInsn &&
          pat2.op == RtlOp::kSet && pat2.children.size() >= 2 && pat2.SetDest() == dst1 &&
          pat2.SetSrc().op == RtlOp::kIor && pat2.SetSrc().mode == RtlMachineMode::kSi &&
          pat2.SetSrc().children.size() == 2 && pat2.SetSrc().children[0] == dst1 &&
          pat2.SetSrc().children[1].op == RtlOp::kConstInt) {
        const uint32_t lo_u32 = static_cast<uint32_t>(pat2.SetSrc().children[1].int_val);
        if ((lo_u32 & 0xffff0000u) == 0u &&
            IsMipsLargeIntForSched1Split(static_cast<int32_t>(hi_u32 | lo_u32))) {
          pairs.push_back(sched_model::AdjacentPair{.first = i, .second = j});
          used[static_cast<size_t>(i)] = true;
          used[static_cast<size_t>(j)] = true;
          break;
        }
      }
      // Stop if an intervening instruction mentions `dest_regno`.
      bool touches_reg = false;
      pat2.Visit([&](const RtlExpr& node) {
        if (node.op == RtlOp::kReg && node.RegNo() == dest_regno) {
          touches_reg = true;
        }
      });
      if (touches_reg) {
        break;
      }
    }
  }
  return pairs;
}

void ApplyReorderedSchedRegion(RtlBasicBlock* blk, const sched_model::ParsedSchedRegion& region,
                               absl::Span<const int> order_in_sched) {
  const std::vector<std::string> orig_insns = blk->instructions;
  const std::vector<RtlInsn> orig_rtl = blk->rtl_insns;
  const bool has_synced_rtl = (orig_rtl.size() == orig_insns.size());

  std::vector<std::string> new_insns;
  std::vector<std::optional<std::vector<RtlExpr>>> new_fusage;
  new_insns.reserve(orig_insns.size());
  new_fusage.reserve(orig_insns.size());

  auto push_orig = [&](size_t idx) {
    new_insns.push_back(orig_insns[idx]);
    if (has_synced_rtl && orig_rtl[idx].has_explicit_call_fusage) {
      new_fusage.push_back(orig_rtl[idx].call_fusage);
    } else {
      new_fusage.push_back(std::nullopt);
    }
  };

  for (size_t idx : region.prefix_orig_indices) {
    push_orig(idx);
  }
  for (RtlLoopNoteKind nk : region.region_note_list) {
    new_insns.push_back(std::string(RtlLoopNoteKindToSexpr(nk)));
    new_fusage.push_back(std::nullopt);
  }
  for (int sched_idx : order_in_sched) {
    const auto& insn = region.sched_block[static_cast<size_t>(sched_idx)];
    for (RtlLoopNoteKind nk : insn.leading_loop_notes) {
      new_insns.push_back(std::string(RtlLoopNoteKindToSexpr(nk)));
      new_fusage.push_back(std::nullopt);
    }
    push_orig(region.sched_orig_indices[static_cast<size_t>(sched_idx)]);
  }
  for (size_t idx : region.suffix_orig_indices) {
    push_orig(idx);
  }

  blk->instructions = std::move(new_insns);
  blk->SyncRtlInsnsFromInstructions();
  for (size_t i = 0; i < blk->rtl_insns.size() && i < new_fusage.size(); ++i) {
    if (new_fusage[i].has_value() && !blk->rtl_insns[i].has_explicit_call_fusage) {
      blk->rtl_insns[i].has_explicit_call_fusage = true;
      blk->rtl_insns[i].call_fusage = std::move(*new_fusage[i]);
    }
  }
}

std::string BuildSchedModelPatternKey(const sched_model::Insn& insn) {
  std::string key = insn.pn.pattern.ToGccPrintRtlString();
  if (insn.pn.has_explicit_call_fusage) {
    key.push_back('|');
    for (const RtlExpr& fu : insn.pn.call_fusage) {
      key.append(fu.ToGccPrintRtlString());
      key.push_back(';');
    }
  }
  return key;
}

void UnscheduleSched1WithModel(RtlControlFlowGraph* cfg, const CompilationProfile& profile,
                               bool gp64, bool fp64) {
  const sched_model::MachineModel mm = sched_model::MachineModel::FromProfile(profile);
  const sched_model::RegFacts facts =
      sched_model::RegFacts::FromCfg(*cfg, profile, sched_model::Phase::kSched1);
  RtlControlFlowGraph live_cfg = *cfg;
  ReconstructRtlLiveness(
      &live_cfg, profile,
      {.reload_completed = false, .update_reg_notes = false, .preserve_phantom_pseudos = false});

  for (size_t b = 0; b < cfg->rpo_blocks.size(); ++b) {
    RtlBasicBlock& blk = cfg->rpo_blocks[b];
    const sched_model::ParsedSchedRegion region =
        sched_model::ParseBlockForSchedModel(blk, b, sched_model::Phase::kSched1);
    if (!region.valid || region.sched_block.size() <= 1) {
      continue;
    }

    absl::btree_set<int> live_at_end;
    for (int succ : blk.successors) {
      if (const auto it = live_cfg.sideband.basic_block_live_at_start.find(succ);
          it != live_cfg.sideband.basic_block_live_at_start.end()) {
        live_at_end.insert(it->second.begin(), it->second.end());
      }
    }

    const std::vector<sched_model::AdjacentPair> split_pairs =
        FindSplitLargeIntAdjacentPairs(region.sched_block);

    // Compute a canonical pre-`sched1` `.06.combine`-like preference using
    // `UnscheduleBlockInstructions` on a scratch copy of `blk`. `InvertBlockOrder`
    // strictly enforces exact `Schedule(Q, kSched1) == P` verification, so this acts
    // purely as a soft tie-breaking preference among valid `F_sched1` preimages.
    std::vector<int> preferred_order;
    {
      RtlBasicBlock scratch = blk;
      UnscheduleBlockInstructions(&scratch, gp64, fp64, /*is_post_reload=*/false,
                                  cfg->sideband.frame_pointer_needed, /*runs_sched1=*/true);
      const sched_model::ParsedSchedRegion scratch_region =
          sched_model::ParseBlockForSchedModel(scratch, b, sched_model::Phase::kSched1);
      if (scratch_region.valid && scratch_region.sched_block.size() == region.sched_block.size()) {
        absl::btree_map<std::string, std::vector<int>> target_buckets;
        for (size_t t = 0; t < region.sched_block.size(); ++t) {
          target_buckets[BuildSchedModelPatternKey(region.sched_block[t])].push_back(
              static_cast<int>(t));
        }
        absl::btree_map<std::string, size_t> cursor;
        std::vector<int> mapped;
        mapped.reserve(region.sched_block.size());
        bool ok = true;
        for (const sched_model::Insn& insn : scratch_region.sched_block) {
          const std::string key = BuildSchedModelPatternKey(insn);
          const auto it = target_buckets.find(key);
          if (it == target_buckets.end()) {
            ok = false;
            break;
          }
          size_t& cur = cursor[key];
          if (cur >= it->second.size()) {
            ok = false;
            break;
          }
          mapped.push_back(it->second[cur++]);
        }
        if (ok && mapped.size() == region.sched_block.size()) {
          // Ensure split `large_int` `ori` immediately follows its `lui` in `preferred_order`.
          for (const sched_model::AdjacentPair& p : split_pairs) {
            auto it_second = std::find(mapped.begin(), mapped.end(), p.second);
            if (it_second != mapped.end()) {
              mapped.erase(it_second);
              auto it_first = std::find(mapped.begin(), mapped.end(), p.first);
              if (it_first != mapped.end()) {
                mapped.insert(it_first + 1, p.second);
              }
            }
          }
          preferred_order = std::move(mapped);
        }
      }
    }

    const std::optional<std::vector<int>> inv =
        sched_model::InvertBlockOrder(region.sched_block, facts, mm, sched_model::Phase::kSched1,
                                      live_at_end, preferred_order, split_pairs);
    if (inv.has_value()) {
      ApplyReorderedSchedRegion(&blk, region, *inv);
    }
  }
}

// Shared per-block inputs for Stage 3's 3-gate verified `kSched1`-achievable
// `sched2` preimage search.
struct Stage3Sched1PreimageContext {
  const sched_model::ParsedSchedRegion& base_region;
  const sched_model::ParsedSchedRegion& target_region;
  size_t block_idx;
  int n;
  sched_model::PostReloadBodyBounds bounds;
  const sched_model::MachineModel& mm;
  const sched_model::RegFacts& facts_s2;
  const sched_model::CfgSched1Context& cfg_ctx;
  const sched_model::Sched1ProjectionContext& proj_ctx;
  const absl::btree_set<int>& live_at_end_s2;
  const std::vector<int>& base_to_target;
  const std::vector<int>& target_to_base;
  const std::vector<sched_model::AdjacentPair>& reload_adjacent;
  bool allow_moved_store_load_equiv_invalidation = false;
};

bool LiftSched1ViewToPreferredTarget(const Stage3Sched1PreimageContext& ctx,
                                     const std::vector<int>& cur_to_base,
                                     const sched_model::Sched1ProjectionView& view,
                                     const absl::btree_set<int>& excluded_prologue,
                                     absl::Span<const int> extra_pinned_body_indices,
                                     std::vector<int>* preferred_out,
                                     absl::Span<const int> explicit_s1_order = {}) {
  const int prologue_end = ctx.bounds.prologue_end;
  const int epilogue_begin = ctx.bounds.epilogue_begin;
  const int body_len = epilogue_begin - prologue_end;
  const int num_core = static_cast<int>(view.core_insns.size());
  std::vector<int> s1_order;
  if (!explicit_s1_order.empty()) {
    if (static_cast<int>(explicit_s1_order.size()) != num_core) {
      return false;
    }
    s1_order.assign(explicit_s1_order.begin(), explicit_s1_order.end());
  } else {
    s1_order = sched_model::ScheduleSched1ViewCoreOrder(view, ctx.mm);
    if (static_cast<int>(s1_order.size()) != num_core) {
      return false;
    }
  }
  std::vector<int> body_to_core(static_cast<size_t>(body_len), -1);
  for (int ci = 0; ci < num_core; ++ci) {
    body_to_core[static_cast<size_t>(view.core_to_orig_index[static_cast<size_t>(ci)])] = ci;
  }
  absl::btree_set<int> pinned_prefix_set(view.pinned_prefix_orig_indices.begin(),
                                         view.pinned_prefix_orig_indices.end());
  absl::btree_set<int> extra_pinned_core_set;
  std::vector<int> extra_pinned_cores;
  for (int rel : extra_pinned_body_indices) {
    if (rel >= 0 && rel < body_len) {
      const int ci = body_to_core[static_cast<size_t>(rel)];
      if (ci >= 0 && extra_pinned_core_set.insert(ci).second) {
        extra_pinned_cores.push_back(ci);
      }
    }
  }
  std::vector<int> interleaved_prologue_cur;
  std::vector<int> pinned_prefix_cur;
  std::vector<std::vector<int>> before_core(static_cast<size_t>(num_core));
  std::vector<std::vector<int>> after_core(static_cast<size_t>(num_core));
  std::vector<int> after_last_core;
  for (int rel = 0; rel < body_len; ++rel) {
    if (body_to_core[static_cast<size_t>(rel)] >= 0) {
      continue;
    }
    const int cur_i = prologue_end + rel;
    if (excluded_prologue.contains(rel)) {
      interleaved_prologue_cur.push_back(cur_i);
      continue;
    }
    if (pinned_prefix_set.contains(rel)) {
      pinned_prefix_cur.push_back(cur_i);
      continue;
    }
    if (const auto ait = view.output_reload_after_core.find(rel);
        ait != view.output_reload_after_core.end() && ait->second >= 0 && ait->second < num_core) {
      after_core[static_cast<size_t>(ait->second)].push_back(cur_i);
      continue;
    }
    if (const auto bit = view.input_reload_before_core.find(rel);
        bit != view.input_reload_before_core.end() && bit->second >= 0 && bit->second < num_core) {
      before_core[static_cast<size_t>(bit->second)].push_back(cur_i);
      continue;
    }
    int next_core = -1;
    for (int k = rel + 1; k < body_len; ++k) {
      if (body_to_core[static_cast<size_t>(k)] >= 0) {
        next_core = body_to_core[static_cast<size_t>(k)];
        break;
      }
    }
    if (next_core >= 0) {
      before_core[static_cast<size_t>(next_core)].push_back(cur_i);
    } else {
      after_last_core.push_back(cur_i);
    }
  }
  preferred_out->clear();
  preferred_out->reserve(static_cast<size_t>(ctx.n));
  auto push_cur = [&](int cur_i) {
    preferred_out->push_back(
        ctx.base_to_target[static_cast<size_t>(cur_to_base[static_cast<size_t>(cur_i)])]);
  };
  for (int i = 0; i < prologue_end; ++i) {
    push_cur(i);
  }
  for (int p_cur : interleaved_prologue_cur) {
    push_cur(p_cur);
  }
  for (int pp_cur : pinned_prefix_cur) {
    push_cur(pp_cur);
  }
  for (int ci : extra_pinned_cores) {
    for (int aux_cur : before_core[static_cast<size_t>(ci)]) {
      push_cur(aux_cur);
    }
    push_cur(prologue_end + view.core_to_orig_index[static_cast<size_t>(ci)]);
    for (int aux_cur : after_core[static_cast<size_t>(ci)]) {
      push_cur(aux_cur);
    }
  }
  for (int ci : s1_order) {
    if (extra_pinned_core_set.contains(ci)) {
      continue;
    }
    for (int aux_cur : before_core[static_cast<size_t>(ci)]) {
      push_cur(aux_cur);
    }
    push_cur(prologue_end + view.core_to_orig_index[static_cast<size_t>(ci)]);
    for (int aux_cur : after_core[static_cast<size_t>(ci)]) {
      push_cur(aux_cur);
    }
  }
  for (int aux_cur : after_last_core) {
    push_cur(aux_cur);
  }
  for (int i = epilogue_begin; i < ctx.n; ++i) {
    push_cur(i);
  }
  return static_cast<int>(preferred_out->size()) == ctx.n;
}

bool TryCommitAchievableSched1Candidate(const Stage3Sched1PreimageContext& ctx,
                                        const std::vector<int>& cand_in_base,
                                        const std::vector<sched_model::Insn>& cand_seq,
                                        bool cand_strict_safe, RtlBasicBlock* out_blk) {
  const int prologue_end = ctx.bounds.prologue_end;
  const int epilogue_begin = ctx.bounds.epilogue_begin;
  const int body_len = epilogue_begin - prologue_end;
  if (!sched_model::BuildInvertedRegDeadSeedOrderInBase(cand_seq, prologue_end, epilogue_begin,
                                                        ctx.facts_s2, ctx.mm)
           .has_value()) {
    if (!cand_strict_safe) {
      return false;
    }
    ApplyReorderedSchedRegion(out_blk, ctx.base_region, cand_in_base);
    return true;
  }
  // `cand_seq` already achieves `kSched1` and replays `kSched2`, but retains a
  // local `sched2`-inverted pure-reader `REG_DEAD` (`sched.c:2872-2906,
  // 3415-3611`). Apply one bounded `kDeferAfterReaders` / `kPullBeforeDead`
  // repair directly on `cand_seq` and re-verify all four gates.
  for (const sched_model::InvertedRegDeadSeedMode mode :
       {sched_model::InvertedRegDeadSeedMode::kDeferAfterReaders,
        sched_model::InvertedRegDeadSeedMode::kPullBeforeDead}) {
    const std::optional<std::vector<int>> fix_in_cand =
        sched_model::BuildInvertedRegDeadSeedOrderInBase(cand_seq, prologue_end, epilogue_begin,
                                                         ctx.facts_s2, ctx.mm, mode);
    if (!fix_in_cand.has_value()) {
      continue;
    }
    std::vector<int> fix_pref_target(static_cast<size_t>(ctx.n));
    for (int pos = 0; pos < ctx.n; ++pos) {
      fix_pref_target[static_cast<size_t>(pos)] = ctx.base_to_target[static_cast<size_t>(
          cand_in_base[static_cast<size_t>((*fix_in_cand)[static_cast<size_t>(pos)])])];
    }
    const std::optional<std::vector<int>> rep_in_target = sched_model::InvertBlockOrder(
        ctx.target_region.sched_block, ctx.facts_s2, ctx.mm, sched_model::Phase::kSched2,
        ctx.live_at_end_s2, fix_pref_target, ctx.reload_adjacent);
    if (!rep_in_target.has_value()) {
      continue;
    }
    std::vector<int> rep_in_base(static_cast<size_t>(ctx.n));
    for (int pos = 0; pos < ctx.n; ++pos) {
      rep_in_base[static_cast<size_t>(pos)] =
          ctx.target_to_base[static_cast<size_t>((*rep_in_target)[static_cast<size_t>(pos)])];
    }
    const bool rep_safe = sched_model::PreservesPostReloadOrderSafety(
        ctx.base_region.sched_block, rep_in_base, prologue_end, epilogue_begin, ctx.live_at_end_s2,
        ctx.cfg_ctx.cfg_copy_pairs, ctx.facts_s2, ctx.mm,
        /*allow_pure_reader_reg_dead_transfer=*/false, ctx.cfg_ctx.stack_layout,
        ctx.proj_ctx.excluded_prologue_body_indices, ctx.allow_moved_store_load_equiv_invalidation);
    if (!rep_safe) {
      continue;
    }
    std::vector<sched_model::Insn> rep_seq;
    rep_seq.reserve(static_cast<size_t>(ctx.n));
    for (int pos = 0; pos < ctx.n; ++pos) {
      sched_model::Insn insn =
          ctx.base_region.sched_block[static_cast<size_t>(rep_in_base[static_cast<size_t>(pos)])];
      insn.luid = pos;
      rep_seq.push_back(std::move(insn));
    }
    if (sched_model::BuildInvertedRegDeadSeedOrderInBase(rep_seq, prologue_end, epilogue_begin,
                                                         ctx.facts_s2, ctx.mm)
            .has_value()) {
      continue;
    }
    std::vector<sched_model::Insn> rep_body;
    rep_body.reserve(static_cast<size_t>(body_len));
    for (int pos = prologue_end; pos < epilogue_begin; ++pos) {
      sched_model::Insn insn = rep_seq[static_cast<size_t>(pos)];
      insn.luid = pos - prologue_end;
      rep_body.push_back(std::move(insn));
    }
    sched_model::Sched1ProjectionContext rep_ctx = ctx.proj_ctx;
    rep_ctx.excluded_prologue_body_indices = sched_model::FindInterleavedPrologueSaveBodyIndices(
        rep_seq, ctx.bounds, ctx.block_idx, ctx.mm);
    const sched_model::Sched1ProjectionView rep_view =
        sched_model::ProjectPostReloadBlockToSched1View(rep_body, ctx.mm, ctx.live_at_end_s2,
                                                        rep_ctx);
    const bool rep_ach =
        rep_view.core_insns.size() > 1 &&
        sched_model::IsAchievable(rep_view.core_insns, rep_view.facts_s1, ctx.mm,
                                  sched_model::Phase::kSched1, rep_view.live_at_end_s1);
    if (rep_ach) {
      ApplyReorderedSchedRegion(out_blk, ctx.base_region, rep_in_base);
      return true;
    }
    // `rep_seq` already replays `kSched2`, satisfies strict post-reload
    // safety, and eliminates the `REG_DEAD` inversion; run a short bounded
    // `kSched1` fixed-point + tie-flip/consumer-hoist refinement from it.
    std::vector<int> cur_rep_base = rep_in_base;
    std::vector<sched_model::Insn> cur_rep_seq = rep_seq;
    sched_model::Sched1ProjectionView cur_rep_view = rep_view;
    absl::btree_set<int> cur_rep_excl = rep_ctx.excluded_prologue_body_indices;
    absl::btree_set<std::vector<int>> rep_tried;
    auto eval_rep_pref = [&](const std::vector<int>& pref_target) -> bool {
      if (!rep_tried.insert(pref_target).second) {
        return false;
      }
      const std::optional<std::vector<int>> nxt_target = sched_model::InvertBlockOrder(
          ctx.target_region.sched_block, ctx.facts_s2, ctx.mm, sched_model::Phase::kSched2,
          ctx.live_at_end_s2, pref_target, ctx.reload_adjacent);
      if (!nxt_target.has_value()) {
        return false;
      }
      std::vector<int> nxt_base(static_cast<size_t>(ctx.n));
      for (int pos = 0; pos < ctx.n; ++pos) {
        nxt_base[static_cast<size_t>(pos)] =
            ctx.target_to_base[static_cast<size_t>((*nxt_target)[static_cast<size_t>(pos)])];
      }
      if (!sched_model::PreservesPostReloadOrderSafety(
              ctx.base_region.sched_block, nxt_base, prologue_end, epilogue_begin,
              ctx.live_at_end_s2, ctx.cfg_ctx.cfg_copy_pairs, ctx.facts_s2, ctx.mm,
              /*allow_pure_reader_reg_dead_transfer=*/false, ctx.cfg_ctx.stack_layout,
              ctx.proj_ctx.excluded_prologue_body_indices,
              ctx.allow_moved_store_load_equiv_invalidation)) {
        return false;
      }
      std::vector<sched_model::Insn> nxt_seq;
      nxt_seq.reserve(static_cast<size_t>(ctx.n));
      for (int pos = 0; pos < ctx.n; ++pos) {
        sched_model::Insn insn =
            ctx.base_region.sched_block[static_cast<size_t>(nxt_base[static_cast<size_t>(pos)])];
        insn.luid = pos;
        nxt_seq.push_back(std::move(insn));
      }
      if (sched_model::BuildInvertedRegDeadSeedOrderInBase(nxt_seq, prologue_end, epilogue_begin,
                                                           ctx.facts_s2, ctx.mm)
              .has_value()) {
        return false;
      }
      std::vector<sched_model::Insn> nxt_body;
      nxt_body.reserve(static_cast<size_t>(body_len));
      for (int pos = prologue_end; pos < epilogue_begin; ++pos) {
        sched_model::Insn insn = nxt_seq[static_cast<size_t>(pos)];
        insn.luid = pos - prologue_end;
        nxt_body.push_back(std::move(insn));
      }
      sched_model::Sched1ProjectionContext nxt_ctx = ctx.proj_ctx;
      nxt_ctx.excluded_prologue_body_indices = sched_model::FindInterleavedPrologueSaveBodyIndices(
          nxt_seq, ctx.bounds, ctx.block_idx, ctx.mm);
      sched_model::Sched1ProjectionView nxt_view = sched_model::ProjectPostReloadBlockToSched1View(
          nxt_body, ctx.mm, ctx.live_at_end_s2, nxt_ctx);
      if (nxt_view.core_insns.size() > 1 &&
          sched_model::IsAchievable(nxt_view.core_insns, nxt_view.facts_s1, ctx.mm,
                                    sched_model::Phase::kSched1, nxt_view.live_at_end_s1)) {
        ApplyReorderedSchedRegion(out_blk, ctx.base_region, nxt_base);
        return true;
      }
      cur_rep_base = std::move(nxt_base);
      cur_rep_seq = std::move(nxt_seq);
      cur_rep_view = std::move(nxt_view);
      cur_rep_excl = std::move(nxt_ctx.excluded_prologue_body_indices);
      return false;
    };
    for (int r = 0; r < 4; ++r) {
      std::vector<int> pref_target;
      if (!LiftSched1ViewToPreferredTarget(ctx, cur_rep_base, cur_rep_view, cur_rep_excl, {},
                                           &pref_target)) {
        break;
      }
      if (eval_rep_pref(pref_target)) {
        return true;
      }
    }
    const std::vector<int> snap_base = cur_rep_base;
    const std::vector<sched_model::Insn> snap_seq = cur_rep_seq;
    const sched_model::Sched1ProjectionView snap_view = cur_rep_view;
    const absl::btree_set<int> snap_excl = cur_rep_excl;
    const std::vector<std::pair<int, int>> rep_s2_edges = sched_model::ExtractCoreSched2Edges(
        snap_seq, prologue_end, epilogue_begin, snap_view, ctx.facts_s2, ctx.mm);
    std::vector<int> snap_to_target(static_cast<size_t>(ctx.n), -1);
    for (int pos = 0; pos < ctx.n; ++pos) {
      snap_to_target[static_cast<size_t>(pos)] =
          ctx.base_to_target[static_cast<size_t>(snap_base[static_cast<size_t>(pos)])];
    }
    const std::vector<std::pair<int, int>> rep_s2_replay_edges =
        sched_model::ExtractCoreSched2ReplayEdges(
            ctx.target_region.sched_block, snap_to_target, prologue_end, epilogue_begin, snap_view,
            ctx.facts_s2, ctx.mm, ctx.live_at_end_s2, /*preserve_reg_dead_reader_order=*/true);
    std::vector<std::vector<int>> alt_orders = sched_model::CollectSched1TieFlipCoreOrders(
        snap_view, ctx.mm, /*max_orders=*/12, rep_s2_edges, rep_s2_replay_edges);
    for (std::vector<int>& ord :
         sched_model::CollectSched1SunkConsumerHoistCoreOrders(snap_view, ctx.mm,
                                                               /*max_orders=*/8)) {
      alt_orders.push_back(std::move(ord));
    }
    for (const std::vector<int>& alt_ord : alt_orders) {
      std::vector<int> pref_target;
      if (!LiftSched1ViewToPreferredTarget(ctx, snap_base, snap_view, snap_excl, {}, &pref_target,
                                           alt_ord)) {
        continue;
      }
      if (eval_rep_pref(pref_target)) {
        return true;
      }
    }
  }
  return false;
}

// `REG_DEAD`) or was successfully reordered into `Im(F_sched1)`.
bool IsSched2NoopSelfMoveInsn(const sched_model::Insn& insn);

bool TrySelectSched1AchievablePreimageForBlock(
    RtlBasicBlock* out_blk_ptr, const RtlBasicBlock& target_blk, size_t b,
    const sched_model::MachineModel& mm, const sched_model::RegFacts& facts_s2,
    const sched_model::CfgSched1Context& cfg_ctx, const RtlControlFlowGraph& live_cfg,
    bool allow_moved_store_load_equiv_invalidation = false) {
  RtlBasicBlock& out_blk = *out_blk_ptr;
  const sched_model::ParsedSchedRegion base_region =
      sched_model::ParseBlockForSchedModel(out_blk, b, sched_model::Phase::kSched2);
  const sched_model::ParsedSchedRegion target_region =
      sched_model::ParseBlockForSchedModel(target_blk, b, sched_model::Phase::kSched2);
  if (!base_region.valid || !target_region.valid || base_region.has_mid_block_loop_barrier ||
      target_region.has_mid_block_loop_barrier) {
    return true;
  }
  const int n = static_cast<int>(base_region.sched_block.size());
  if (n <= 2 || static_cast<int>(target_region.sched_block.size()) != n) {
    return true;
  }

  absl::btree_set<int> live_at_end_s2;
  for (int succ : out_blk.successors) {
    if (const auto it = live_cfg.sideband.basic_block_live_at_start.find(succ);
        it != live_cfg.sideband.basic_block_live_at_start.end()) {
      live_at_end_s2.insert(it->second.begin(), it->second.end());
    }
  }

  // Exclude prologue prefix (block 0), interleaved block-0 prologue callee-save
  // stores (`toplev.c:3103`, `function.c:5516`, `mips.c:expand_prologue`), and
  // epilogue suffix from the `sched1` projection because `thread_prologue_and_epilogue_insns`
  // runs after `reload` and after `sched1`.
  const sched_model::PostReloadBodyBounds bounds =
      sched_model::FindPostReloadBodyBounds(base_region.sched_block, b);
  const int prologue_end = bounds.prologue_end;
  const int epilogue_begin = bounds.epilogue_begin;
  const int body_len = epilogue_begin - prologue_end;
  if (body_len <= 1) {
    return true;
  }

  std::vector<sched_model::Insn> base_body;
  base_body.reserve(static_cast<size_t>(body_len));
  for (int i = prologue_end; i < epilogue_begin; ++i) {
    sched_model::Insn insn = base_region.sched_block[static_cast<size_t>(i)];
    insn.luid = i - prologue_end;
    base_body.push_back(std::move(insn));
  }

  sched_model::Sched1ProjectionContext proj_ctx =
      sched_model::BuildSched1ProjectionContextForBlock(b, cfg_ctx);
  const bool fp_needed = !proj_ctx.stack_layout.valid || proj_ctx.stack_layout.frame_pointer_needed;
  // Stage 4 (`invert_alter_reg.cc`, `flow.c:2506-2515`) splits a hard-reg RMW
  // into distinct pseudos only when an explicit `(REG_DEAD r)` note is present
  // (either from real GCC RTL or restored at Stage 0). Never hallucinate
  // implicit post-call `combine_regs` splits here that Stage 4 will not emit.
  proj_ctx.allow_post_call_combine_regs_split = false;
  proj_ctx.excluded_prologue_body_indices =
      sched_model::FindInterleavedPrologueSaveBodyIndices(base_region.sched_block, bounds, b, mm);
  const sched_model::Sched1ProjectionView base_view =
      sched_model::ProjectPostReloadBlockToSched1View(base_body, mm, live_at_end_s2, proj_ctx);
  if (base_view.core_insns.size() <= 1) {
    return true;
  }
  // Fast path: if `Q_base`'s body is already in `Im(F_sched1)`, has no stale
  // `sched2`-inverted pure-reader `REG_DEAD` (`sched.c:2872-2906, 3415-3611`),
  // AND actually replays `target_region` under `kSched2` without a mid-block
  // deleted no-op self-move (`sched.c:4803-4818`), keep it untouched!
  const std::optional<std::vector<int>> inverted_reg_dead_seed =
      sched_model::BuildInvertedRegDeadSeedOrderInBase(base_region.sched_block, prologue_end,
                                                       epilogue_begin, facts_s2, mm);
  const bool base_ach =
      sched_model::IsAchievable(base_view.core_insns, base_view.facts_s1, mm,
                                sched_model::Phase::kSched1, base_view.live_at_end_s1);
  if (!inverted_reg_dead_seed.has_value() && base_ach) {
    bool base_replays_s2 = true;
    for (int pos = 0; pos + 1 < n; ++pos) {
      if (IsSched2NoopSelfMoveInsn(base_region.sched_block[static_cast<size_t>(pos)])) {
        base_replays_s2 = false;
        break;
      }
    }
    if (base_replays_s2) {
      const sched_model::DepGraph g_base_s2 =
          sched_model::Analyze(base_region.sched_block, facts_s2, mm, sched_model::Phase::kSched2);
      const sched_model::ScheduleResult sr_base_s2 =
          sched_model::Schedule(base_region.sched_block, g_base_s2, facts_s2, mm,
                                sched_model::Phase::kSched2, live_at_end_s2);
      if (static_cast<int>(sr_base_s2.order.size()) != n) {
        base_replays_s2 = false;
      } else {
        for (int pos = 0; pos < n; ++pos) {
          if (base_region
                  .sched_block[static_cast<size_t>(sr_base_s2.order[static_cast<size_t>(pos)])]
                  .pn.pattern.ToGccPrintRtlString() !=
              target_region.sched_block[static_cast<size_t>(pos)]
                  .pn.pattern.ToGccPrintRtlString()) {
            base_replays_s2 = false;
            break;
          }
        }
      }
    }
    if (base_replays_s2) {
      return true;
    }
  }

  // Build bijection between `base_region.sched_block` and `target_region.sched_block` by
  // pattern + call_fusage (ignoring `REG_NOTES` added by `RestoreInheritedSpillSlotDeadNotes`).
  absl::btree_map<std::string, std::vector<int>> target_buckets;
  for (int t = 0; t < n; ++t) {
    target_buckets[BuildSchedModelPatternKey(target_region.sched_block[static_cast<size_t>(t)])]
        .push_back(t);
  }
  absl::btree_map<std::string, size_t> bucket_cursor;
  std::vector<int> base_to_target(static_cast<size_t>(n), -1);
  std::vector<int> target_to_base(static_cast<size_t>(n), -1);
  for (int bi = 0; bi < n; ++bi) {
    const std::string key =
        BuildSchedModelPatternKey(base_region.sched_block[static_cast<size_t>(bi)]);
    const auto it = target_buckets.find(key);
    if (it == target_buckets.end()) {
      return false;
    }
    size_t& cur = bucket_cursor[key];
    if (cur >= it->second.size()) {
      return false;
    }
    const int t = it->second[cur++];
    base_to_target[static_cast<size_t>(bi)] = t;
    target_to_base[static_cast<size_t>(t)] = bi;
  }

  // Preserve adjacent `hi`/`lo` (`64`/`65`) output-reload and stack-slot input-reload
  // pairs (`reload1.c:emit_reload_insns`) via supernode adjacency in `InvertBlockOrder`.
  std::vector<sched_model::AdjacentPair> reload_adjacent;
  for (int i = 0; i + 1 < n; ++i) {
    if (sched_model::IsAdjacentMdOutputReloadPair(
            base_region.sched_block[static_cast<size_t>(i)],
            base_region.sched_block[static_cast<size_t>(i + 1)]) ||
        sched_model::IsAdjacentReloadContiguityPair(
            base_region.sched_block[static_cast<size_t>(i)],
            base_region.sched_block[static_cast<size_t>(i + 1)], mm, cfg_ctx.stack_layout)) {
      reload_adjacent.push_back(sched_model::AdjacentPair{
          .first = base_to_target[static_cast<size_t>(i)],
          .second = base_to_target[static_cast<size_t>(i + 1)],
      });
    }
  }

  const Stage3Sched1PreimageContext stage3_ctx{
      .base_region = base_region,
      .target_region = target_region,
      .block_idx = b,
      .n = n,
      .bounds = bounds,
      .mm = mm,
      .facts_s2 = facts_s2,
      .cfg_ctx = cfg_ctx,
      .proj_ctx = proj_ctx,
      .live_at_end_s2 = live_at_end_s2,
      .base_to_target = base_to_target,
      .target_to_base = target_to_base,
      .reload_adjacent = reload_adjacent,
      .allow_moved_store_load_equiv_invalidation = allow_moved_store_load_equiv_invalidation,
  };

  auto lift_view_to_preferred_target =
      [&](const std::vector<int>& cur_to_base, const sched_model::Sched1ProjectionView& view,
          const absl::btree_set<int>& excluded_prologue,
          absl::Span<const int> extra_pinned_body_indices, std::vector<int>* preferred_out,
          absl::Span<const int> explicit_s1_order = {}) -> bool {
    return LiftSched1ViewToPreferredTarget(stage3_ctx, cur_to_base, view, excluded_prologue,
                                           extra_pinned_body_indices, preferred_out,
                                           explicit_s1_order);
  };

  auto count_s1_diffs = [&](const sched_model::Sched1ProjectionView& v) -> int {
    const std::vector<int> ord = sched_model::ScheduleSched1ViewCoreOrder(v, mm);
    if (ord.size() != v.core_insns.size()) {
      return static_cast<int>(v.core_insns.size());
    }
    int diffs = 0;
    for (int i = 0; i < static_cast<int>(ord.size()); ++i) {
      if (ord[static_cast<size_t>(i)] != i) {
        ++diffs;
      }
    }
    return diffs;
  };

  auto try_commit_achievable_cand = [&](const std::vector<int>& cand_in_base,
                                        const std::vector<sched_model::Insn>& cand_seq,
                                        bool cand_strict_safe) -> bool {
    return TryCommitAchievableSched1Candidate(stage3_ctx, cand_in_base, cand_seq, cand_strict_safe,
                                              &out_blk);
  };

  constexpr int kMaxFixedPointRounds = 4;
  absl::btree_set<std::vector<int>> tried_prefs;
  auto run_fixed_point_search = [&](bool hoist_displaced_b0_args, bool run_tie_flips,
                                    const std::vector<int>* initial_seed = nullptr,
                                    bool use_closest_s1 = false) -> bool {
    const bool allow_relaxed = (initial_seed != nullptr);
    absl::btree_set<std::vector<int>> local_tried_prefs;
    absl::btree_set<std::vector<int>>& active_tried_prefs =
        allow_relaxed ? local_tried_prefs : tried_prefs;
    std::vector<int> cur_to_base(static_cast<size_t>(n));
    sched_model::Sched1ProjectionView cur_view = base_view;
    absl::btree_set<int> cur_excluded = proj_ctx.excluded_prologue_body_indices;
    if (initial_seed != nullptr && static_cast<int>(initial_seed->size()) == n) {
      cur_to_base = *initial_seed;
      std::vector<sched_model::Insn> seed_seq;
      seed_seq.reserve(static_cast<size_t>(n));
      for (int pos = 0; pos < n; ++pos) {
        sched_model::Insn insn =
            base_region.sched_block[static_cast<size_t>(cur_to_base[static_cast<size_t>(pos)])];
        insn.luid = pos;
        seed_seq.push_back(std::move(insn));
      }
      std::vector<sched_model::Insn> seed_body;
      seed_body.reserve(static_cast<size_t>(body_len));
      for (int pos = prologue_end; pos < epilogue_begin; ++pos) {
        sched_model::Insn insn = seed_seq[static_cast<size_t>(pos)];
        insn.luid = pos - prologue_end;
        seed_body.push_back(std::move(insn));
      }
      sched_model::Sched1ProjectionContext seed_proj_ctx = proj_ctx;
      seed_proj_ctx.excluded_prologue_body_indices =
          sched_model::FindInterleavedPrologueSaveBodyIndices(seed_seq, bounds, b, mm);
      cur_excluded = seed_proj_ctx.excluded_prologue_body_indices;
      cur_view = sched_model::ProjectPostReloadBlockToSched1View(seed_body, mm, live_at_end_s2,
                                                                 seed_proj_ctx);
      if (cur_view.core_insns.size() <= 1) {
        return false;
      }
    } else {
      for (int i = 0; i < n; ++i) {
        cur_to_base[static_cast<size_t>(i)] = i;
      }
    }

    for (int round = 0; round < kMaxFixedPointRounds; ++round) {
      std::vector<int> extra_pinned;
      if (hoist_displaced_b0_args && b == 0) {
        std::vector<sched_model::Insn> cur_body;
        cur_body.reserve(static_cast<size_t>(body_len));
        for (int pos = prologue_end; pos < epilogue_begin; ++pos) {
          sched_model::Insn insn =
              base_region.sched_block[static_cast<size_t>(cur_to_base[static_cast<size_t>(pos)])];
          insn.luid = pos - prologue_end;
          cur_body.push_back(std::move(insn));
        }
        extra_pinned = sched_model::CollectDisplacedBlock0EntryArgCopyBodyIndices(
            cur_body, cur_view, cur_excluded, mm, fp_needed);
        if (round == 0 && extra_pinned.empty()) {
          return false;
        }
      }
      std::vector<int> closest_s1;
      if (use_closest_s1) {
        closest_s1 = sched_model::ScheduleClosestSched1CoreOrder(cur_view, mm);
      }
      std::vector<int> preferred_in_target;
      if (!lift_view_to_preferred_target(cur_to_base, cur_view, cur_excluded, extra_pinned,
                                         &preferred_in_target, closest_s1)) {
        break;
      }
      if ((!run_tie_flips || allow_relaxed) &&
          !active_tried_prefs.insert(preferred_in_target).second) {
        break;
      }
      const std::optional<std::vector<int>> cand_in_target = sched_model::InvertBlockOrder(
          target_region.sched_block, facts_s2, mm, sched_model::Phase::kSched2, live_at_end_s2,
          preferred_in_target, reload_adjacent);
      if (!cand_in_target.has_value()) {
        break;
      }
      std::vector<int> cand_in_base(static_cast<size_t>(n));
      bool differs_from_base = false;
      for (int pos = 0; pos < n; ++pos) {
        const int bi =
            target_to_base[static_cast<size_t>((*cand_in_target)[static_cast<size_t>(pos)])];
        cand_in_base[static_cast<size_t>(pos)] = bi;
        if (bi != pos) {
          differs_from_base = true;
        }
      }
      if (!differs_from_base) {
        break;
      }
      const bool strict_safety_ok = sched_model::PreservesPostReloadOrderSafety(
          base_region.sched_block, cand_in_base, prologue_end, epilogue_begin, live_at_end_s2,
          cfg_ctx.cfg_copy_pairs, facts_s2, mm,
          /*allow_pure_reader_reg_dead_transfer=*/false, cfg_ctx.stack_layout,
          proj_ctx.excluded_prologue_body_indices, allow_moved_store_load_equiv_invalidation);
      if (!strict_safety_ok &&
          (!allow_relaxed ||
           !sched_model::PreservesPostReloadOrderSafety(
               base_region.sched_block, cand_in_base, prologue_end, epilogue_begin, live_at_end_s2,
               cfg_ctx.cfg_copy_pairs, facts_s2, mm,
               /*allow_pure_reader_reg_dead_transfer=*/true, cfg_ctx.stack_layout,
               proj_ctx.excluded_prologue_body_indices,
               allow_moved_store_load_equiv_invalidation))) {
        break;
      }
      std::vector<sched_model::Insn> cand_seq;
      cand_seq.reserve(static_cast<size_t>(n));
      for (int pos = 0; pos < n; ++pos) {
        sched_model::Insn insn =
            base_region.sched_block[static_cast<size_t>(cand_in_base[static_cast<size_t>(pos)])];
        insn.luid = pos;
        cand_seq.push_back(std::move(insn));
      }
      std::vector<sched_model::Insn> cand_body;
      cand_body.reserve(static_cast<size_t>(body_len));
      for (int pos = prologue_end; pos < epilogue_begin; ++pos) {
        sched_model::Insn insn = cand_seq[static_cast<size_t>(pos)];
        insn.luid = pos - prologue_end;
        cand_body.push_back(std::move(insn));
      }
      sched_model::Sched1ProjectionContext cand_proj_ctx = proj_ctx;
      cand_proj_ctx.excluded_prologue_body_indices =
          sched_model::FindInterleavedPrologueSaveBodyIndices(cand_seq, bounds, b, mm);
      sched_model::Sched1ProjectionView cand_view = sched_model::ProjectPostReloadBlockToSched1View(
          cand_body, mm, live_at_end_s2, cand_proj_ctx);
      if (cand_view.core_insns.size() <= 1) {
        break;
      }
      const bool cand_ach =
          sched_model::IsAchievable(cand_view.core_insns, cand_view.facts_s1, mm,
                                    sched_model::Phase::kSched1, cand_view.live_at_end_s1);
      if (cand_ach && try_commit_achievable_cand(cand_in_base, cand_seq, strict_safety_ok)) {
        return true;
      }
      cur_to_base = std::move(cand_in_base);
      cur_excluded = std::move(cand_proj_ctx.excluded_prologue_body_indices);
      cur_view = std::move(cand_view);
    }

    if (!run_tie_flips) {
      return false;
    }

    std::vector<int> extra_pinned;
    if (hoist_displaced_b0_args && b == 0) {
      std::vector<sched_model::Insn> cur_body;
      cur_body.reserve(static_cast<size_t>(body_len));
      for (int pos = prologue_end; pos < epilogue_begin; ++pos) {
        sched_model::Insn insn =
            base_region.sched_block[static_cast<size_t>(cur_to_base[static_cast<size_t>(pos)])];
        insn.luid = pos - prologue_end;
        cur_body.push_back(std::move(insn));
      }
      extra_pinned = sched_model::CollectDisplacedBlock0EntryArgCopyBodyIndices(
          cur_body, cur_view, cur_excluded, mm, fp_needed);
    }
    std::vector<sched_model::Insn> cur_seq_for_edges;
    cur_seq_for_edges.reserve(static_cast<size_t>(n));
    for (int pos = 0; pos < n; ++pos) {
      sched_model::Insn insn =
          base_region.sched_block[static_cast<size_t>(cur_to_base[static_cast<size_t>(pos)])];
      insn.luid = pos;
      cur_seq_for_edges.push_back(std::move(insn));
    }

    struct BestStage3Residual {
      int s1_diffs = std::numeric_limits<int>::max();
      std::vector<int> to_base;
      std::vector<sched_model::Insn> seq;
      sched_model::Sched1ProjectionView view;
      absl::btree_set<int> excluded;
      std::vector<int> extra_pinned;
    };
    const int cur_s1_diffs = count_s1_diffs(cur_view);
    BestStage3Residual best_residual;

    auto refine_from_target_pref = [&](const std::vector<int>& init_pref_in_target,
                                       bool record_best) -> bool {
      std::vector<int> seed_to_base;
      sched_model::Sched1ProjectionView seed_view;
      absl::btree_set<int> seed_excluded;
      std::vector<int> seed_extra_pinned;
      std::vector<int> preferred_in_target = init_pref_in_target;
      constexpr int kMaxSeedRefineRounds = 4;
      for (int sround = 0; sround < kMaxSeedRefineRounds; ++sround) {
        if (sround > 0 && !lift_view_to_preferred_target(seed_to_base, seed_view, seed_excluded,
                                                         seed_extra_pinned, &preferred_in_target)) {
          break;
        }
        if (!active_tried_prefs.insert(preferred_in_target).second) {
          break;
        }
        const std::optional<std::vector<int>> cand_in_target = sched_model::InvertBlockOrder(
            target_region.sched_block, facts_s2, mm, sched_model::Phase::kSched2, live_at_end_s2,
            preferred_in_target, reload_adjacent);
        if (!cand_in_target.has_value()) {
          break;
        }
        std::vector<int> cand_in_base(static_cast<size_t>(n));
        bool differs_from_base = false;
        for (int pos = 0; pos < n; ++pos) {
          const int bi =
              target_to_base[static_cast<size_t>((*cand_in_target)[static_cast<size_t>(pos)])];
          cand_in_base[static_cast<size_t>(pos)] = bi;
          if (bi != pos) {
            differs_from_base = true;
          }
        }
        if (!differs_from_base) {
          break;
        }
        const bool strict_safety_ok = sched_model::PreservesPostReloadOrderSafety(
            base_region.sched_block, cand_in_base, prologue_end, epilogue_begin, live_at_end_s2,
            cfg_ctx.cfg_copy_pairs, facts_s2, mm,
            /*allow_pure_reader_reg_dead_transfer=*/false, cfg_ctx.stack_layout,
            proj_ctx.excluded_prologue_body_indices, allow_moved_store_load_equiv_invalidation);
        if (!strict_safety_ok &&
            (!allow_relaxed ||
             !sched_model::PreservesPostReloadOrderSafety(
                 base_region.sched_block, cand_in_base, prologue_end, epilogue_begin,
                 live_at_end_s2, cfg_ctx.cfg_copy_pairs, facts_s2, mm,
                 /*allow_pure_reader_reg_dead_transfer=*/true, cfg_ctx.stack_layout,
                 proj_ctx.excluded_prologue_body_indices,
                 allow_moved_store_load_equiv_invalidation))) {
          break;
        }
        std::vector<sched_model::Insn> cand_seq;
        cand_seq.reserve(static_cast<size_t>(n));
        for (int pos = 0; pos < n; ++pos) {
          sched_model::Insn insn =
              base_region.sched_block[static_cast<size_t>(cand_in_base[static_cast<size_t>(pos)])];
          insn.luid = pos;
          cand_seq.push_back(std::move(insn));
        }
        std::vector<sched_model::Insn> cand_body;
        cand_body.reserve(static_cast<size_t>(body_len));
        for (int pos = prologue_end; pos < epilogue_begin; ++pos) {
          sched_model::Insn insn = cand_seq[static_cast<size_t>(pos)];
          insn.luid = pos - prologue_end;
          cand_body.push_back(std::move(insn));
        }
        sched_model::Sched1ProjectionContext cand_proj_ctx = proj_ctx;
        cand_proj_ctx.excluded_prologue_body_indices =
            sched_model::FindInterleavedPrologueSaveBodyIndices(cand_seq, bounds, b, mm);
        sched_model::Sched1ProjectionView cand_view =
            sched_model::ProjectPostReloadBlockToSched1View(cand_body, mm, live_at_end_s2,
                                                            cand_proj_ctx);
        const bool is_ach_cand =
            cand_view.core_insns.size() > 1 &&
            sched_model::IsAchievable(cand_view.core_insns, cand_view.facts_s1, mm,
                                      sched_model::Phase::kSched1, cand_view.live_at_end_s1);
        if (is_ach_cand && try_commit_achievable_cand(cand_in_base, cand_seq, strict_safety_ok)) {
          return true;
        }
        seed_to_base = std::move(cand_in_base);
        seed_excluded = std::move(cand_proj_ctx.excluded_prologue_body_indices);
        seed_view = std::move(cand_view);
        if (hoist_displaced_b0_args && b == 0) {
          seed_extra_pinned = sched_model::CollectDisplacedBlock0EntryArgCopyBodyIndices(
              cand_body, seed_view, seed_excluded, mm, fp_needed);
        }
        if (record_best && seed_view.core_insns.size() > 1) {
          const int nd = count_s1_diffs(seed_view);
          if (nd > 0 && nd < best_residual.s1_diffs) {
            best_residual.s1_diffs = nd;
            best_residual.to_base = seed_to_base;
            best_residual.seq = std::move(cand_seq);
            best_residual.view = seed_view;
            best_residual.excluded = seed_excluded;
            best_residual.extra_pinned = seed_extra_pinned;
          }
        }
      }
      return false;
    };

    // (a) Hoist callee-saved insns sunk across a call (`sched.c:1704, 2235`).
    for (const std::vector<int>& hoist_in_cur :
         sched_model::CollectSched1704CallHoistCandidatePrefs(
             cur_seq_for_edges, prologue_end, epilogue_begin, cur_view, facts_s2, mm)) {
      std::vector<int> hoist_in_target(static_cast<size_t>(n));
      for (int pos = 0; pos < n; ++pos) {
        hoist_in_target[static_cast<size_t>(pos)] = base_to_target[static_cast<size_t>(
            cur_to_base[static_cast<size_t>(hoist_in_cur[static_cast<size_t>(pos)])])];
      }
      if (refine_from_target_pref(hoist_in_target, /*record_best=*/true)) {
        return true;
      }
    }

    auto run_tie_flips_on_state =
        [&](const std::vector<int>& st_to_base, const std::vector<sched_model::Insn>& st_seq,
            const sched_model::Sched1ProjectionView& st_view,
            const absl::btree_set<int>& st_excluded, const std::vector<int>& st_extra_pinned,
            int max_orders, bool record_best) -> bool {
      const std::vector<std::pair<int, int>> core_s2_edges = sched_model::ExtractCoreSched2Edges(
          st_seq, prologue_end, epilogue_begin, st_view, facts_s2, mm);
      std::vector<int> st_to_target(static_cast<size_t>(n), -1);
      for (int pos = 0; pos < n; ++pos) {
        st_to_target[static_cast<size_t>(pos)] =
            base_to_target[static_cast<size_t>(st_to_base[static_cast<size_t>(pos)])];
      }
      const std::vector<std::pair<int, int>> core_s2_replay_edges =
          sched_model::ExtractCoreSched2ReplayEdges(
              target_region.sched_block, st_to_target, prologue_end, epilogue_begin, st_view,
              facts_s2, mm, live_at_end_s2,
              /*preserve_reg_dead_reader_order=*/(initial_seed == nullptr));
      const std::vector<std::vector<int>> tie_flip_orders =
          sched_model::CollectSched1TieFlipCoreOrders(st_view, mm, max_orders, core_s2_edges,
                                                      core_s2_replay_edges);
      for (const std::vector<int>& alt_s1_order : tie_flip_orders) {
        std::vector<int> preferred_in_target;
        if (!lift_view_to_preferred_target(st_to_base, st_view, st_excluded, st_extra_pinned,
                                           &preferred_in_target, alt_s1_order)) {
          continue;
        }
        if (refine_from_target_pref(preferred_in_target, record_best)) {
          return true;
        }
      }
      return false;
    };

    if (run_tie_flips_on_state(cur_to_base, cur_seq_for_edges, cur_view, cur_excluded, extra_pinned,
                               /*max_orders=*/24, /*record_best=*/true)) {
      return true;
    }

    bool cur_differs_from_base = false;
    for (int pos = 0; pos < n; ++pos) {
      if (cur_to_base[static_cast<size_t>(pos)] != pos) {
        cur_differs_from_base = true;
        break;
      }
    }
    if (cur_differs_from_base) {
      std::vector<int> id_to_base(static_cast<size_t>(n));
      std::vector<sched_model::Insn> id_seq;
      id_seq.reserve(static_cast<size_t>(n));
      for (int pos = 0; pos < n; ++pos) {
        id_to_base[static_cast<size_t>(pos)] = pos;
        sched_model::Insn insn = base_region.sched_block[static_cast<size_t>(pos)];
        insn.luid = pos;
        id_seq.push_back(std::move(insn));
      }
      std::vector<int> base_extra_pinned;
      if (hoist_displaced_b0_args && b == 0) {
        base_extra_pinned = sched_model::CollectDisplacedBlock0EntryArgCopyBodyIndices(
            base_body, base_view, proj_ctx.excluded_prologue_body_indices, mm, fp_needed);
      }
      if (run_tie_flips_on_state(id_to_base, id_seq, base_view,
                                 proj_ctx.excluded_prologue_body_indices, base_extra_pinned,
                                 /*max_orders=*/24, /*record_best=*/true)) {
        return true;
      }
    }

    auto run_sunk_consumer_hoists_on_state =
        [&](const std::vector<int>& st_to_base, const sched_model::Sched1ProjectionView& st_view,
            const absl::btree_set<int>& st_excluded, const std::vector<int>& st_extra_pinned,
            int max_orders, bool record_best) -> bool {
      const std::vector<std::vector<int>> hoist_orders =
          sched_model::CollectSched1SunkConsumerHoistCoreOrders(st_view, mm, max_orders);
      for (const std::vector<int>& alt_s1_order : hoist_orders) {
        std::vector<int> preferred_in_target;
        if (!lift_view_to_preferred_target(st_to_base, st_view, st_excluded, st_extra_pinned,
                                           &preferred_in_target, alt_s1_order)) {
          continue;
        }
        if (refine_from_target_pref(preferred_in_target, record_best)) {
          return true;
        }
      }
      return false;
    };

    if (best_residual.s1_diffs < cur_s1_diffs && !best_residual.to_base.empty()) {
      for (int descent = 0; descent < 2; ++descent) {
        const auto snap_h = best_residual;
        if (run_sunk_consumer_hoists_on_state(snap_h.to_base, snap_h.view, snap_h.excluded,
                                              snap_h.extra_pinned, /*max_orders=*/12,
                                              /*record_best=*/true)) {
          return true;
        }
        const auto snap_t = best_residual;
        if (run_tie_flips_on_state(snap_t.to_base, snap_t.seq, snap_t.view, snap_t.excluded,
                                   snap_t.extra_pinned, /*max_orders=*/12,
                                   /*record_best=*/true)) {
          return true;
        }
        if (best_residual.s1_diffs >= snap_h.s1_diffs) {
          break;
        }
      }
    } else if (cur_s1_diffs > 0) {
      if (run_sunk_consumer_hoists_on_state(cur_to_base, cur_view, cur_excluded, extra_pinned,
                                            /*max_orders=*/12, /*record_best=*/false)) {
        return true;
      }
    }
    return false;
  };

  std::vector<std::vector<int>> verified_reg_dead_seeds;
  auto record_verified_seed = [&](std::vector<int> cand_in_base) {
    if (std::find(verified_reg_dead_seeds.begin(), verified_reg_dead_seeds.end(), cand_in_base) ==
        verified_reg_dead_seeds.end()) {
      verified_reg_dead_seeds.push_back(std::move(cand_in_base));
    }
  };
  auto try_inverted_reg_dead_seed = [&](const std::vector<int>& seed_in_base) -> bool {
    std::vector<int> preferred_in_target(static_cast<size_t>(n));
    for (int pos = 0; pos < n; ++pos) {
      preferred_in_target[static_cast<size_t>(pos)] =
          base_to_target[static_cast<size_t>(seed_in_base[static_cast<size_t>(pos)])];
    }
    const std::optional<std::vector<int>> cand_in_target = sched_model::InvertBlockOrder(
        target_region.sched_block, facts_s2, mm, sched_model::Phase::kSched2, live_at_end_s2,
        preferred_in_target, reload_adjacent);
    if (!cand_in_target.has_value()) {
      return false;
    }
    std::vector<int> cand_in_base(static_cast<size_t>(n));
    for (int pos = 0; pos < n; ++pos) {
      cand_in_base[static_cast<size_t>(pos)] =
          target_to_base[static_cast<size_t>((*cand_in_target)[static_cast<size_t>(pos)])];
    }
    std::vector<sched_model::Insn> cand_seq;
    cand_seq.reserve(static_cast<size_t>(n));
    for (int pos = 0; pos < n; ++pos) {
      sched_model::Insn insn =
          base_region.sched_block[static_cast<size_t>(cand_in_base[static_cast<size_t>(pos)])];
      insn.luid = pos;
      cand_seq.push_back(std::move(insn));
    }
    if (sched_model::BuildInvertedRegDeadSeedOrderInBase(cand_seq, prologue_end, epilogue_begin,
                                                         facts_s2, mm)
            .has_value()) {
      return false;
    }
    if (!sched_model::PreservesPostReloadOrderSafety(
            base_region.sched_block, cand_in_base, prologue_end, epilogue_begin, live_at_end_s2,
            cfg_ctx.cfg_copy_pairs, facts_s2, mm,
            /*allow_pure_reader_reg_dead_transfer=*/false, cfg_ctx.stack_layout,
            proj_ctx.excluded_prologue_body_indices, allow_moved_store_load_equiv_invalidation)) {
      return false;
    }
    std::vector<sched_model::Insn> cand_body;
    cand_body.reserve(static_cast<size_t>(body_len));
    for (int pos = prologue_end; pos < epilogue_begin; ++pos) {
      sched_model::Insn insn = cand_seq[static_cast<size_t>(pos)];
      insn.luid = pos - prologue_end;
      cand_body.push_back(std::move(insn));
    }
    sched_model::Sched1ProjectionContext cand_proj_ctx = proj_ctx;
    cand_proj_ctx.excluded_prologue_body_indices =
        sched_model::FindInterleavedPrologueSaveBodyIndices(cand_seq, bounds, b, mm);
    sched_model::Sched1ProjectionView cand_view = sched_model::ProjectPostReloadBlockToSched1View(
        cand_body, mm, live_at_end_s2, cand_proj_ctx);
    if (cand_view.core_insns.size() <= 1 ||
        !sched_model::IsAchievable(cand_view.core_insns, cand_view.facts_s1, mm,
                                   sched_model::Phase::kSched1, cand_view.live_at_end_s1)) {
      record_verified_seed(std::move(cand_in_base));
      return false;
    }
    ApplyReorderedSchedRegion(&out_blk, base_region, cand_in_base);
    return true;
  };

  bool has_displaced_integrated_b0_arg = false;
  if (b == 0) {
    for (int rel : sched_model::CollectDisplacedBlock0EntryArgCopyBodyIndices(
             base_body, base_view, proj_ctx.excluded_prologue_body_indices, mm, fp_needed)) {
      const RtlExpr& pat = base_body[static_cast<size_t>(rel)].pn.pattern;
      if (pat.op == RtlOp::kSet && pat.children.size() == 2 &&
          ((pat.SetDest().op == RtlOp::kReg && pat.SetDest().flags.integrated) ||
           pat.SetDest().op == RtlOp::kMem)) {
        has_displaced_integrated_b0_arg = true;
        break;
      }
    }
  }
  auto refine_verified_seed = [&](const std::vector<int>& seed) -> bool {
    return run_fixed_point_search(/*hoist_displaced_b0_args=*/false, /*run_tie_flips=*/false,
                                  &seed) ||
           (b == 0 && run_fixed_point_search(/*hoist_displaced_b0_args=*/true,
                                             /*run_tie_flips=*/false, &seed)) ||
           run_fixed_point_search(/*hoist_displaced_b0_args=*/false, /*run_tie_flips=*/false, &seed,
                                  /*use_closest_s1=*/true) ||
           (b == 0 && run_fixed_point_search(/*hoist_displaced_b0_args=*/true,
                                             /*run_tie_flips=*/false, &seed,
                                             /*use_closest_s1=*/true)) ||
           run_fixed_point_search(/*hoist_displaced_b0_args=*/false, /*run_tie_flips=*/true,
                                  &seed) ||
           (b == 0 && run_fixed_point_search(/*hoist_displaced_b0_args=*/true,
                                             /*run_tie_flips=*/true, &seed));
  };
  bool solved = false;
  if (inverted_reg_dead_seed.has_value()) {
    solved = try_inverted_reg_dead_seed(*inverted_reg_dead_seed);
    for (const sched_model::InvertedRegDeadSeedMode alt_mode :
         {sched_model::InvertedRegDeadSeedMode::kPullBeforeDead,
          sched_model::InvertedRegDeadSeedMode::kDeferAfterReaders}) {
      if (solved) {
        break;
      }
      const std::optional<std::vector<int>> alt_seed =
          sched_model::BuildInvertedRegDeadSeedOrderInBase(base_region.sched_block, prologue_end,
                                                           epilogue_begin, facts_s2, mm, alt_mode);
      if (alt_seed.has_value() && *alt_seed != *inverted_reg_dead_seed) {
        solved = try_inverted_reg_dead_seed(*alt_seed);
      }
    }
  }
  if (!solved) {
    if (has_displaced_integrated_b0_arg) {
      solved = run_fixed_point_search(/*hoist_displaced_b0_args=*/true, /*run_tie_flips=*/false) ||
               run_fixed_point_search(/*hoist_displaced_b0_args=*/false, /*run_tie_flips=*/false);
    } else {
      solved = run_fixed_point_search(/*hoist_displaced_b0_args=*/false, /*run_tie_flips=*/false) ||
               (b == 0 &&
                run_fixed_point_search(/*hoist_displaced_b0_args=*/true, /*run_tie_flips=*/false));
    }
  }
  if (!solved) {
    if (has_displaced_integrated_b0_arg) {
      solved = run_fixed_point_search(/*hoist_displaced_b0_args=*/true, /*run_tie_flips=*/false,
                                      /*initial_seed=*/nullptr, /*use_closest_s1=*/true) ||
               run_fixed_point_search(/*hoist_displaced_b0_args=*/false, /*run_tie_flips=*/false,
                                      /*initial_seed=*/nullptr, /*use_closest_s1=*/true);
    } else {
      solved = run_fixed_point_search(/*hoist_displaced_b0_args=*/false, /*run_tie_flips=*/false,
                                      /*initial_seed=*/nullptr, /*use_closest_s1=*/true) ||
               (b == 0 && run_fixed_point_search(/*hoist_displaced_b0_args=*/true,
                                                 /*run_tie_flips=*/false, /*initial_seed=*/nullptr,
                                                 /*use_closest_s1=*/true));
    }
  }
  if (!solved) {
    if (has_displaced_integrated_b0_arg) {
      solved = run_fixed_point_search(/*hoist_displaced_b0_args=*/true, /*run_tie_flips=*/true) ||
               run_fixed_point_search(/*hoist_displaced_b0_args=*/false, /*run_tie_flips=*/true);
    } else {
      solved = run_fixed_point_search(/*hoist_displaced_b0_args=*/false, /*run_tie_flips=*/true) ||
               (b == 0 &&
                run_fixed_point_search(/*hoist_displaced_b0_args=*/true, /*run_tie_flips=*/true));
    }
  }
  // When strict search cannot solve a block whose `base_region` has
  // `sched2`-inverted pure-reader `REG_DEAD` notes (e.g. multiple registers or
  // `sched.c:1704` call-sunk insns), run relaxed intermediate refinement from
  // verified inverted-`REG_DEAD` seeds and (if needed) `base_region` itself;
  // final commit still strictly enforces all four gates (`kSched2` replay,
  // strict safety, zero remaining `REG_DEAD` inversions, and
  // `IsAchievable(kSched1)`).
  for (const std::vector<int>& vseed : verified_reg_dead_seeds) {
    if (solved) {
      break;
    }
    solved = refine_verified_seed(vseed);
  }
  if (!solved && inverted_reg_dead_seed.has_value()) {
    std::vector<int> identity_seed(static_cast<size_t>(n));
    for (int pos = 0; pos < n; ++pos) {
      identity_seed[static_cast<size_t>(pos)] = pos;
    }
    solved = refine_verified_seed(identity_seed);
  }
  return solved;
}

// Stage 3 (`kSched2`) -> Stage 6 (`kSched1`) forward-verified preimage selector
// (`step37_design.md` §2.3 Step 3). Strictly gated on:
//   (a) exact `InvertBlockOrder(kSched2)` verification `Schedule(Q_cand, kSched2) == P`,
//   (b) hard-reg/memory DAG and `REG_DEAD` safety for Stage 4 (`Pass04`/`Pass05`), AND
//   (c) `IsAchievable(ProjectPostReloadBlockToSched1View(Q_cand), kSched1) == true`.
// Mirrors `caller-save.c:410-411, 568-615` (`restore_referenced_regs`):
// post-call caller-save restores are emitted immediately in front of the first
// post-call instruction that references the restored hard register (in
// ascending hard-register order when multiple restores share a consumer). When
// Stage 3 commits a `kSched1`-achievable preimage (including from the
// pre-unscheduled fallback), re-sinks any `sched2`-hoisted caller-save restore
// (`cfg_ctx.stack_layout.caller_save_slot_offsets`) immediately before its
// first consumer iff the resulting block strictly passes
// `PreservesPostReloadOrderSafety`, exact forward `kSched2` replay against
// `target_blk`, and `IsAchievable(kSched1)`.
void SinkCallerSaveRestoresIfVerified(RtlBasicBlock* out_blk, const RtlBasicBlock& target_blk,
                                      size_t b, const sched_model::MachineModel& mm,
                                      const sched_model::RegFacts& facts_s2,
                                      const sched_model::CfgSched1Context& cfg_ctx,
                                      const RtlControlFlowGraph& live_cfg) {
  if (cfg_ctx.stack_layout.caller_save_slot_offsets.empty()) {
    return;
  }
  const sched_model::ParsedSchedRegion base_region =
      sched_model::ParseBlockForSchedModel(*out_blk, b, sched_model::Phase::kSched2);
  const sched_model::ParsedSchedRegion target_region =
      sched_model::ParseBlockForSchedModel(target_blk, b, sched_model::Phase::kSched2);
  if (!base_region.valid || !target_region.valid || base_region.has_mid_block_loop_barrier ||
      target_region.has_mid_block_loop_barrier) {
    return;
  }
  const int n = static_cast<int>(base_region.sched_block.size());
  if (n <= 2 || static_cast<int>(target_region.sched_block.size()) != n) {
    return;
  }
  const sched_model::PostReloadBodyBounds bounds =
      sched_model::FindPostReloadBodyBounds(base_region.sched_block, b);
  const int prologue_end = bounds.prologue_end;
  const int epilogue_begin = bounds.epilogue_begin;
  const int body_len = epilogue_begin - prologue_end;
  if (body_len <= 1) {
    return;
  }

  absl::btree_set<int> live_at_end_s2;
  for (int succ : out_blk->successors) {
    if (const auto it = live_cfg.sideband.basic_block_live_at_start.find(succ);
        it != live_cfg.sideband.basic_block_live_at_start.end()) {
      live_at_end_s2.insert(it->second.begin(), it->second.end());
    }
  }
  sched_model::Sched1ProjectionContext proj_ctx =
      sched_model::BuildSched1ProjectionContextForBlock(b, cfg_ctx);
  proj_ctx.allow_post_call_combine_regs_split = false;

  auto match_cs_restore = [&](const sched_model::Insn& insn, int* out_reg, int* out_nregs) -> bool {
    if (insn.kind != sched_model::Insn::Kind::kInsn) {
      return false;
    }
    const RtlExpr& pat = insn.pn.pattern;
    if (pat.op != RtlOp::kSet || pat.children.size() != 2 || pat.SetDest().op != RtlOp::kReg ||
        pat.SetDest().flags.integrated) {
      return false;
    }
    for (const RtlRegNote& note : insn.pn.reg_notes) {
      if (note.kind == RtlMachineMode::kRegEquiv || note.kind == RtlMachineMode::kRegEqual) {
        return false;
      }
    }
    const int r = pat.SetDest().RegNo();
    if (r <= 0 || r >= kMipsFirstPseudoRegister || !mm.CallUsed(r)) {
      return false;
    }
    int base = -1;
    int64_t off = 0;
    int64_t sz = 0;
    if (!ExtractStackBaseDispMem(pat.SetSrc(), &base, &off, &sz) || base != kMipsSpRegno ||
        !cfg_ctx.stack_layout.caller_save_slot_offsets.contains(off)) {
      return false;
    }
    *out_reg = r;
    *out_nregs = std::max(1, mm.HardRegNRegs(r, pat.SetDest().mode));
    return true;
  };

  auto insn_touches_reg_span = [&](const sched_model::Insn& insn, int r, int nr) -> bool {
    if (insn.kind == sched_model::Insn::Kind::kCall ||
        insn.kind == sched_model::Insn::Kind::kJump) {
      return true;
    }
    bool hit = false;
    auto check_expr = [&](const RtlExpr& root) {
      root.Visit([&](const RtlExpr& e) {
        if (hit || e.op != RtlOp::kReg || e.IsArgPointerReg()) {
          return;
        }
        const int er = e.RegNo();
        if (er < 0 || er >= kMipsFirstPseudoRegister) {
          return;
        }
        const int enr = std::max(1, mm.HardRegNRegs(er, e.mode));
        if (er < r + nr && r < er + enr) {
          hit = true;
        }
      });
    };
    check_expr(insn.pn.pattern);
    for (const RtlExpr& fu : insn.pn.call_fusage) {
      check_expr(fu);
    }
    return hit;
  };

  auto insn_reads_reg_span = [&](const sched_model::Insn& insn, int r, int nr) -> bool {
    bool hit = false;
    auto check_reads = [&](const auto& self, const RtlExpr& e, bool in_dst) -> void {
      if (hit || e.op == RtlOp::kClobber) {
        return;
      }
      if (e.op == RtlOp::kReg && !e.IsArgPointerReg()) {
        if (!in_dst) {
          const int er = e.RegNo();
          if (er >= 0 && er < kMipsFirstPseudoRegister) {
            const int enr = std::max(1, mm.HardRegNRegs(er, e.mode));
            if (er < r + nr && r < er + enr) {
              hit = true;
            }
          }
        }
        return;
      }
      if (e.op == RtlOp::kSet && e.children.size() == 2) {
        self(self, e.children[1], false);
        if (e.children[0].op == RtlOp::kMem) {
          self(self, e.children[0], false);
        } else {
          self(self, e.children[0], true);
        }
        return;
      }
      for (const RtlExpr& ch : e.children) {
        self(self, ch, in_dst);
      }
    };
    check_reads(check_reads, insn.pn.pattern, false);
    return hit;
  };

  auto has_reg_dead_covering = [&](const sched_model::Insn& insn, int r, int nr) -> bool {
    for (const RtlRegNote& note : insn.pn.reg_notes) {
      if (note.kind == RtlMachineMode::kRegDead && note.datum.op == RtlOp::kReg &&
          !note.datum.IsArgPointerReg()) {
        const int dr = note.datum.RegNo();
        if (dr >= 0 && dr < kMipsFirstPseudoRegister) {
          const int dnr = std::max(1, mm.HardRegNRegs(dr, note.datum.mode));
          if (dr < r + nr && r < dr + dnr) {
            return true;
          }
        }
      }
    }
    return false;
  };

  // Matches a `reload1.c:1938, 5641-6485` (`reload_as_needed` /
  // `emit_reload_insns`) input reload into a call-used scratch register feeding
  // `user_insn` without a `(REG_DEAD lr)` note on `user_insn`.
  auto match_input_reload_for_user = [&](const sched_model::Insn& r_insn,
                                         const sched_model::Insn& u_insn, int* out_reg,
                                         int* out_nregs) -> bool {
    if (r_insn.kind != sched_model::Insn::Kind::kInsn ||
        u_insn.kind != sched_model::Insn::Kind::kInsn || !r_insn.pn.reg_notes.empty()) {
      return false;
    }
    int dummy_r = -1;
    int dummy_nr = 0;
    if (match_cs_restore(r_insn, &dummy_r, &dummy_nr)) {
      return false;
    }
    const RtlExpr& pat = r_insn.pn.pattern;
    if (pat.op != RtlOp::kSet || pat.children.size() != 2 || pat.SetDest().op != RtlOp::kReg ||
        pat.SetDest().flags.integrated) {
      return false;
    }
    const int lr = pat.SetDest().RegNo();
    if (lr <= 0 || lr >= kMipsFirstPseudoRegister || !mm.CallUsed(lr)) {
      return false;
    }
    const RtlExpr& src = pat.SetSrc();
    int base = -1;
    int64_t off = 0;
    int64_t sz = 0;
    const bool is_stack_load =
        ExtractStackBaseDispMem(src, &base, &off, &sz) &&
        (base == kMipsSpRegno ||
         (cfg_ctx.stack_layout.frame_pointer_needed && base == kMipsFpRegno));
    const bool is_gpr_to_fpr = (lr >= 32 && lr < 64 && src.op == RtlOp::kReg &&
                                !src.IsArgPointerReg() && src.RegNo() >= 2 && src.RegNo() < 32);
    const bool is_const_double = (src.op == RtlOp::kConstDouble);
    if (!is_stack_load && !is_gpr_to_fpr && !is_const_double) {
      return false;
    }
    const int lnr = std::max(1, mm.HardRegNRegs(lr, pat.SetDest().mode));
    if (!insn_reads_reg_span(u_insn, lr, lnr) || has_reg_dead_covering(u_insn, lr, lnr)) {
      return false;
    }
    if (out_reg != nullptr) {
      *out_reg = lr;
    }
    if (out_nregs != nullptr) {
      *out_nregs = lnr;
    }
    return true;
  };

  auto insn_key = [](const sched_model::Insn& insn) {
    std::string k = insn.pn.pattern.ToSexprString();
    if (insn.pn.has_explicit_call_fusage) {
      k.push_back('|');
      for (const RtlExpr& e : insn.pn.call_fusage) {
        k.append(e.ToSexprString());
        k.push_back(';');
      }
    }
    return k;
  };
  std::vector<std::string> target_keys(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    target_keys[static_cast<size_t>(i)] =
        insn_key(target_region.sched_block[static_cast<size_t>(i)]);
  }

  std::vector<int> cur_in_base(static_cast<size_t>(n));
  std::iota(cur_in_base.begin(), cur_in_base.end(), 0);
  std::vector<sched_model::Insn> cur_seq = base_region.sched_block;
  bool changed = false;

  auto try_candidate_order = [&](const std::vector<int>& cand_in_cur) -> bool {
    std::vector<int> cand_in_base(static_cast<size_t>(n));
    std::vector<sched_model::Insn> cand_seq;
    cand_seq.reserve(static_cast<size_t>(n));
    for (int pos = 0; pos < n; ++pos) {
      cand_in_base[static_cast<size_t>(pos)] =
          cur_in_base[static_cast<size_t>(cand_in_cur[static_cast<size_t>(pos)])];
      sched_model::Insn insn = cur_seq[static_cast<size_t>(cand_in_cur[static_cast<size_t>(pos)])];
      insn.luid = pos;
      cand_seq.push_back(std::move(insn));
    }

    const absl::btree_set<int> cand_excl =
        sched_model::FindInterleavedPrologueSaveBodyIndices(cand_seq, bounds, b, mm);
    if (!sched_model::PreservesPostReloadOrderSafety(
            base_region.sched_block, cand_in_base, prologue_end, epilogue_begin, live_at_end_s2,
            cfg_ctx.cfg_copy_pairs, facts_s2, mm,
            /*allow_pure_reader_reg_dead_transfer=*/false, cfg_ctx.stack_layout, cand_excl)) {
      return false;
    }
    const sched_model::DepGraph g_s2 =
        sched_model::Analyze(cand_seq, facts_s2, mm, sched_model::Phase::kSched2);
    const sched_model::ScheduleResult sr_s2 = sched_model::Schedule(
        cand_seq, g_s2, facts_s2, mm, sched_model::Phase::kSched2, live_at_end_s2);
    if (static_cast<int>(sr_s2.order.size()) != n) {
      return false;
    }
    for (int pos = 0; pos < n; ++pos) {
      if (insn_key(cand_seq[static_cast<size_t>(sr_s2.order[static_cast<size_t>(pos)])]) !=
          target_keys[static_cast<size_t>(pos)]) {
        return false;
      }
    }

    std::vector<sched_model::Insn> cand_body;
    cand_body.reserve(static_cast<size_t>(body_len));
    for (int pos = prologue_end; pos < epilogue_begin; ++pos) {
      sched_model::Insn insn = cand_seq[static_cast<size_t>(pos)];
      insn.luid = pos - prologue_end;
      cand_body.push_back(std::move(insn));
    }
    sched_model::Sched1ProjectionContext cand_ctx = proj_ctx;
    cand_ctx.excluded_prologue_body_indices = cand_excl;
    const sched_model::Sched1ProjectionView cand_view =
        sched_model::ProjectPostReloadBlockToSched1View(cand_body, mm, live_at_end_s2, cand_ctx);
    if (cand_view.core_insns.size() > 1 &&
        !sched_model::IsAchievable(cand_view.core_insns, cand_view.facts_s1, mm,
                                   sched_model::Phase::kSched1, cand_view.live_at_end_s1)) {
      return false;
    }
    cur_in_base = std::move(cand_in_base);
    cur_seq = std::move(cand_seq);
    changed = true;
    return true;
  };

  for (int rev = epilogue_begin; rev > prologue_end; --rev) {
    const int i = rev - 1;
    int cs_r = -1;
    int cs_nr = 0;
    if (!match_cs_restore(cur_seq[static_cast<size_t>(i)], &cs_r, &cs_nr)) {
      continue;
    }
    int first_stop = epilogue_begin;
    for (int k = i + 1; k < epilogue_begin; ++k) {
      if (insn_touches_reg_span(cur_seq[static_cast<size_t>(k)], cs_r, cs_nr)) {
        first_stop = k;
        break;
      }
    }
    if (first_stop >= epilogue_begin) {
      continue;
    }
    // `reload1.c:1906` (`save_call_clobbered_regs`) runs strictly BEFORE
    // `reload1.c:1938, 5641` (`reload_as_needed` / `emit_reload_insns`), so
    // any caller-save restore for `first_stop` precedes `first_stop`'s
    // contiguous input reload(s).
    int target = first_stop;
    while (target > i + 1 && match_input_reload_for_user(cur_seq[static_cast<size_t>(target - 1)],
                                                         cur_seq[static_cast<size_t>(first_stop)],
                                                         nullptr, nullptr)) {
      --target;
    }
    while (target > i + 1) {
      int prev_r = -1;
      int prev_nr = 0;
      if (!match_cs_restore(cur_seq[static_cast<size_t>(target - 1)], &prev_r, &prev_nr) ||
          prev_r <= cs_r) {
        break;
      }
      bool prev_used = false;
      for (int m = target; m < first_stop; ++m) {
        if (insn_touches_reg_span(cur_seq[static_cast<size_t>(m)], prev_r, prev_nr)) {
          prev_used = true;
          break;
        }
      }
      if (prev_used) {
        break;
      }
      --target;
    }
    // Symmetric case: if `i` sits immediately after one or more input reloads
    // feeding `first_stop` (so `sched2` hoisted the input reload past the
    // caller-save restore), float `i` back ahead of those input reloads.
    if (target == i + 1) {
      int float_pos = i;
      while (float_pos > prologue_end &&
             match_input_reload_for_user(cur_seq[static_cast<size_t>(float_pos - 1)],
                                         cur_seq[static_cast<size_t>(first_stop)], nullptr,
                                         nullptr)) {
        --float_pos;
      }
      if (float_pos < i) {
        std::vector<int> float_restore_cand(static_cast<size_t>(n));
        std::iota(float_restore_cand.begin(), float_restore_cand.end(), 0);
        std::rotate(float_restore_cand.begin() + float_pos, float_restore_cand.begin() + i,
                    float_restore_cand.begin() + i + 1);
        try_candidate_order(float_restore_cand);
      }
      continue;
    }

    bool applied = false;
    if (target == first_stop &&
        cur_seq[static_cast<size_t>(first_stop)].kind == sched_model::Insn::Kind::kInsn) {
      std::vector<int> hoist_user_cand(static_cast<size_t>(n));
      std::iota(hoist_user_cand.begin(), hoist_user_cand.end(), 0);
      std::rotate(hoist_user_cand.begin() + i + 1, hoist_user_cand.begin() + first_stop,
                  hoist_user_cand.begin() + first_stop + 1);
      applied = try_candidate_order(hoist_user_cand);
    }
    if (!applied) {
      std::vector<int> sink_restore_cand(static_cast<size_t>(n));
      std::iota(sink_restore_cand.begin(), sink_restore_cand.end(), 0);
      std::rotate(sink_restore_cand.begin() + i, sink_restore_cand.begin() + i + 1,
                  sink_restore_cand.begin() + target);
      try_candidate_order(sink_restore_cand);
    }
  }

  // `reload1.c:6476-6482` (`other_input_reload_insns` emitted before
  // `input_reload_insns[j]`): canonicalize adjacent independent non-constant
  // input reloads feeding the same consumer `u` into ascending operand preorder
  // (`FirstReadRegPreorderIndex`) when verified against `kSched2` replay and
  // `IsAchievable(kSched1)`.
  for (int u = prologue_end + 2; u < epilogue_begin; ++u) {
    int grp_begin = u;
    while (grp_begin > prologue_end &&
           match_input_reload_for_user(cur_seq[static_cast<size_t>(grp_begin - 1)],
                                       cur_seq[static_cast<size_t>(u)], nullptr, nullptr)) {
      --grp_begin;
    }
    if (u - grp_begin < 2) {
      continue;
    }
    for (int pass = grp_begin; pass < u - 1; ++pass) {
      for (int k = grp_begin; k < u - 1; ++k) {
        int ra = -1, nra = 0, rb = -1, nrb = 0;
        if (!match_input_reload_for_user(cur_seq[static_cast<size_t>(k)],
                                         cur_seq[static_cast<size_t>(u)], &ra, &nra) ||
            !match_input_reload_for_user(cur_seq[static_cast<size_t>(k + 1)],
                                         cur_seq[static_cast<size_t>(u)], &rb, &nrb)) {
          continue;
        }
        if (cur_seq[static_cast<size_t>(k)].pn.pattern.SetSrc().op == RtlOp::kConstDouble ||
            cur_seq[static_cast<size_t>(k + 1)].pn.pattern.SetSrc().op == RtlOp::kConstDouble) {
          continue;
        }
        if (ra < rb + nrb && rb < ra + nra) {
          continue;
        }
        if (insn_touches_reg_span(cur_seq[static_cast<size_t>(k + 1)], ra, nra) ||
            insn_touches_reg_span(cur_seq[static_cast<size_t>(k)], rb, nrb)) {
          continue;
        }
        absl::flat_hash_set<int> words_a;
        for (int w = 0; w < nra; ++w) {
          words_a.insert(ra + w);
        }
        absl::flat_hash_set<int> words_b;
        for (int w = 0; w < nrb; ++w) {
          words_b.insert(rb + w);
        }
        const int idx_a =
            FirstReadRegPreorderIndex(cur_seq[static_cast<size_t>(u)].pn.pattern, words_a);
        const int idx_b =
            FirstReadRegPreorderIndex(cur_seq[static_cast<size_t>(u)].pn.pattern, words_b);
        if (idx_a >= 0 && idx_b >= 0 && idx_b < idx_a) {
          std::vector<int> swap_cand(static_cast<size_t>(n));
          std::iota(swap_cand.begin(), swap_cand.end(), 0);
          std::swap(swap_cand[static_cast<size_t>(k)], swap_cand[static_cast<size_t>(k + 1)]);
          // Swapping two independent input reloads in `u`'s contiguous prefix
          // preserves reload contiguity (`reload1.c:6481`), so verify exact
          // forward `kSched2` replay and `IsAchievable(kSched1)` directly.
          std::vector<int> cand_in_base(static_cast<size_t>(n));
          std::vector<sched_model::Insn> cand_seq;
          cand_seq.reserve(static_cast<size_t>(n));
          for (int pos = 0; pos < n; ++pos) {
            cand_in_base[static_cast<size_t>(pos)] =
                cur_in_base[static_cast<size_t>(swap_cand[static_cast<size_t>(pos)])];
            sched_model::Insn insn =
                cur_seq[static_cast<size_t>(swap_cand[static_cast<size_t>(pos)])];
            insn.luid = pos;
            cand_seq.push_back(std::move(insn));
          }
          const sched_model::DepGraph g_s2 =
              sched_model::Analyze(cand_seq, facts_s2, mm, sched_model::Phase::kSched2);
          const sched_model::ScheduleResult sr_s2 = sched_model::Schedule(
              cand_seq, g_s2, facts_s2, mm, sched_model::Phase::kSched2, live_at_end_s2);
          bool replay_ok = (static_cast<int>(sr_s2.order.size()) == n);
          for (int pos = 0; pos < n && replay_ok; ++pos) {
            if (insn_key(cand_seq[static_cast<size_t>(sr_s2.order[static_cast<size_t>(pos)])]) !=
                target_keys[static_cast<size_t>(pos)]) {
              replay_ok = false;
            }
          }
          if (replay_ok) {
            std::vector<sched_model::Insn> cand_body;
            cand_body.reserve(static_cast<size_t>(body_len));
            for (int pos = prologue_end; pos < epilogue_begin; ++pos) {
              sched_model::Insn insn = cand_seq[static_cast<size_t>(pos)];
              insn.luid = pos - prologue_end;
              cand_body.push_back(std::move(insn));
            }
            sched_model::Sched1ProjectionContext cand_ctx = proj_ctx;
            cand_ctx.excluded_prologue_body_indices =
                sched_model::FindInterleavedPrologueSaveBodyIndices(cand_seq, bounds, b, mm);
            const sched_model::Sched1ProjectionView cand_view =
                sched_model::ProjectPostReloadBlockToSched1View(cand_body, mm, live_at_end_s2,
                                                                cand_ctx);
            if (cand_view.core_insns.size() <= 1 ||
                sched_model::IsAchievable(cand_view.core_insns, cand_view.facts_s1, mm,
                                          sched_model::Phase::kSched1, cand_view.live_at_end_s1)) {
              cur_in_base = std::move(cand_in_base);
              cur_seq = std::move(cand_seq);
              changed = true;
            }
          }
        }
      }
    }
  }

  if (changed) {
    ApplyReorderedSchedRegion(out_blk, base_region, cur_in_base);
  }
}

// Mirrors `local-alloc.c:1396-1401` (`alloc_qty_for_scratch`),
// `local-alloc.c:1416-1417`, `local-alloc.c:2158-2182` (`find_free_reg`), and
// `config/mips/mips.h:1186-1193` (`FIXED_REGISTERS`, ascending numeric
// allocation order when `REG_ALLOC_ORDER` is undefined):
// for a `match_scratch` `PARALLEL` carrying `(clobber (reg:M scr_hr))`,
// `block_alloc` assigns `scr_hr` only when every earlier non-fixed hard
// register in `scr_hr`'s class is occupied across `[2*p - 1, 2*p + 1]`.
// When post-reload `sched2` hoisted the death chain of an earlier register `r`
// above `p`, sink that independent chain past `p` iff the resulting block
// strictly passes `PreservesPostReloadOrderSafety`, exact forward `kSched2`
// replay against `target_blk`, and eliminates the missing scratch barrier(s)
// (preferring a candidate that also satisfies `IsAchievable(kSched1)`).
void SinkPreScratchDeadChainsIfVerified(RtlBasicBlock* out_blk, const RtlBasicBlock& target_blk,
                                        size_t b, const sched_model::MachineModel& mm,
                                        const sched_model::RegFacts& facts_s2,
                                        const sched_model::CfgSched1Context& cfg_ctx,
                                        const RtlControlFlowGraph& live_cfg) {
  const sched_model::ParsedSchedRegion base_region =
      sched_model::ParseBlockForSchedModel(*out_blk, b, sched_model::Phase::kSched2);
  const sched_model::ParsedSchedRegion target_region =
      sched_model::ParseBlockForSchedModel(target_blk, b, sched_model::Phase::kSched2);
  if (!base_region.valid || !target_region.valid || base_region.has_mid_block_loop_barrier ||
      target_region.has_mid_block_loop_barrier) {
    return;
  }
  const int n = static_cast<int>(base_region.sched_block.size());
  if (n <= 2 || static_cast<int>(target_region.sched_block.size()) != n) {
    return;
  }
  const sched_model::PostReloadBodyBounds bounds =
      sched_model::FindPostReloadBodyBounds(base_region.sched_block, b);
  const int prologue_end = bounds.prologue_end;
  const int epilogue_begin = bounds.epilogue_begin;
  const int body_len = epilogue_begin - prologue_end;
  if (body_len <= 1) {
    return;
  }

  auto is_match_scratch_parallel = [](const RtlExpr& pat) -> bool {
    if (pat.op != RtlOp::kParallel) {
      return false;
    }
    for (const RtlExpr& elem : pat.children) {
      if (elem.op != RtlOp::kSet || elem.children.size() != 2) {
        continue;
      }
      const RtlOp sop = elem.children[1].op;
      if (sop == RtlOp::kFix || sop == RtlOp::kMult || sop == RtlOp::kDiv || sop == RtlOp::kMod ||
          sop == RtlOp::kUdiv || sop == RtlOp::kUmod || sop == RtlOp::kTruncate ||
          sop == RtlOp::kFfs) {
        return true;
      }
    }
    return false;
  };

  struct ScratchSite {
    int insn_idx = -1;
    int scr_hr = -1;
    RtlMachineMode mode = RtlMachineMode::kSi;
  };
  std::vector<ScratchSite> sites;
  for (int i = prologue_end; i < epilogue_begin; ++i) {
    const sched_model::Insn& insn = base_region.sched_block[static_cast<size_t>(i)];
    if (insn.kind != sched_model::Insn::Kind::kInsn ||
        !is_match_scratch_parallel(insn.pn.pattern)) {
      continue;
    }
    for (const RtlExpr& elem : insn.pn.pattern.children) {
      if (elem.op != RtlOp::kClobber || elem.children.size() != 1 || !elem.children[0].IsReg() ||
          elem.children[0].IsArgPointerReg()) {
        continue;
      }
      const int hr = elem.children[0].RegNo();
      const bool in_gr = (hr >= 2 && hr <= 25);
      const bool in_fp = (hr >= 32 && hr <= 63);
      if (!in_gr && !in_fp) {
        continue;
      }
      const bool has_unused = std::any_of(
          insn.pn.reg_notes.begin(), insn.pn.reg_notes.end(), [&](const RtlRegNote& rn) {
            return rn.kind == RtlMachineMode::kRegUnused && rn.datum.IsReg() &&
                   !rn.datum.IsArgPointerReg() && rn.datum.RegNo() == hr;
          });
      if (has_unused) {
        sites.push_back(ScratchSite{.insn_idx = i, .scr_hr = hr, .mode = elem.children[0].mode});
      }
    }
  }
  if (sites.empty()) {
    return;
  }

  absl::btree_set<int> live_at_end_s2;
  for (int succ : out_blk->successors) {
    if (const auto it = live_cfg.sideband.basic_block_live_at_start.find(succ);
        it != live_cfg.sideband.basic_block_live_at_start.end()) {
      live_at_end_s2.insert(it->second.begin(), it->second.end());
    }
  }

  auto collect_rw = [&](const sched_model::Insn& insn, absl::btree_set<int>* reads,
                        absl::btree_set<int>* writes) {
    auto add_span = [&](int r, RtlMachineMode mode, absl::btree_set<int>* dst) {
      if (r < 0 || r >= kMipsFirstPseudoRegister) {
        return;
      }
      const int nr = std::max(1, mm.HardRegNRegs(r, mode));
      for (int k = 0; k < nr && r + k < kMipsFirstPseudoRegister; ++k) {
        dst->insert(r + k);
      }
    };
    auto walk_reads = [&](const RtlExpr& root) {
      root.Visit([&](const RtlExpr& e) {
        if (e.op == RtlOp::kReg && !e.IsArgPointerReg()) {
          add_span(e.RegNo(), e.mode, reads);
        }
      });
    };
    auto handle_elem = [&](const RtlExpr& elem) {
      if (elem.IsSet() && elem.children.size() == 2) {
        const RtlExpr& d = elem.SetDest();
        if (d.op == RtlOp::kReg && !d.IsArgPointerReg()) {
          add_span(d.RegNo(), d.mode, writes);
        } else if (d.op == RtlOp::kSubreg && !d.children.empty() && d.children[0].IsReg() &&
                   !d.children[0].IsArgPointerReg()) {
          add_span(d.children[0].RegNo(), d.children[0].mode, writes);
        } else {
          walk_reads(d);
        }
        walk_reads(elem.SetSrc());
      } else if (elem.op == RtlOp::kClobber && elem.children.size() == 1) {
        const RtlExpr& d = elem.children[0];
        if (d.op == RtlOp::kReg && !d.IsArgPointerReg()) {
          add_span(d.RegNo(), d.mode, writes);
        }
      } else {
        walk_reads(elem);
      }
    };
    if (insn.pn.pattern.op == RtlOp::kParallel) {
      for (const RtlExpr& ch : insn.pn.pattern.children) {
        handle_elem(ch);
      }
    } else {
      handle_elem(insn.pn.pattern);
    }
    for (const RtlExpr& fu : insn.pn.call_fusage) {
      if (fu.op == RtlOp::kUse && !fu.children.empty()) {
        walk_reads(fu.children[0]);
      } else if (fu.op == RtlOp::kClobber && !fu.children.empty() && fu.children[0].IsReg()) {
        add_span(fu.children[0].RegNo(), fu.children[0].mode, writes);
      }
    }
    if (insn.kind == sched_model::Insn::Kind::kCall) {
      for (int r = 0; r < kMipsFirstPseudoRegister; ++r) {
        if (mm.CallUsed(r)) {
          writes->insert(r);
        }
      }
    }
  };

  auto compute_missing_barriers =
      [&](const std::vector<sched_model::Insn>& seq,
          absl::btree_map<int, absl::btree_set<int>>* missing_by_pos) -> int {
    missing_by_pos->clear();
    std::vector<absl::btree_set<int>> insn_reads(static_cast<size_t>(n));
    std::vector<absl::btree_set<int>> insn_writes(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      collect_rw(seq[static_cast<size_t>(i)], &insn_reads[static_cast<size_t>(i)],
                 &insn_writes[static_cast<size_t>(i)]);
    }
    std::vector<absl::btree_set<int>> live_after(static_cast<size_t>(n));
    std::vector<absl::btree_set<int>> live_before(static_cast<size_t>(n));
    absl::btree_set<int> cur_live = live_at_end_s2;
    for (int i = n - 1; i >= 0; --i) {
      live_after[static_cast<size_t>(i)] = cur_live;
      for (int w : insn_writes[static_cast<size_t>(i)]) {
        cur_live.erase(w);
      }
      for (int r : insn_reads[static_cast<size_t>(i)]) {
        cur_live.insert(r);
      }
      live_before[static_cast<size_t>(i)] = cur_live;
    }
    int total_missing = 0;
    for (int i = prologue_end; i < epilogue_begin; ++i) {
      const sched_model::Insn& insn = seq[static_cast<size_t>(i)];
      if (insn.kind != sched_model::Insn::Kind::kInsn ||
          !is_match_scratch_parallel(insn.pn.pattern)) {
        continue;
      }
      absl::btree_set<int> busy = live_before[static_cast<size_t>(i)];
      busy.insert(live_after[static_cast<size_t>(i)].begin(),
                  live_after[static_cast<size_t>(i)].end());
      busy.insert(insn_reads[static_cast<size_t>(i)].begin(),
                  insn_reads[static_cast<size_t>(i)].end());
      busy.insert(insn_writes[static_cast<size_t>(i)].begin(),
                  insn_writes[static_cast<size_t>(i)].end());
      for (const RtlExpr& elem : insn.pn.pattern.children) {
        if (elem.op != RtlOp::kClobber || elem.children.size() != 1 || !elem.children[0].IsReg() ||
            elem.children[0].IsArgPointerReg()) {
          continue;
        }
        const int hr = elem.children[0].RegNo();
        const bool in_gr = (hr >= 2 && hr <= 25);
        const bool in_fp = (hr >= 32 && hr <= 63);
        if (!in_gr && !in_fp) {
          continue;
        }
        const int start_r = in_gr ? 2 : 32;
        const int step = in_gr ? 1 : std::max(1, mm.HardRegNRegs(32, elem.children[0].mode));
        for (int r = start_r; r < hr; r += step) {
          const int nr = std::max(1, mm.HardRegNRegs(r, elem.children[0].mode));
          bool all_free = true;
          for (int k = 0; k < nr; ++k) {
            if (busy.contains(r + k)) {
              all_free = false;
              break;
            }
          }
          if (all_free) {
            (*missing_by_pos)[i].insert(r);
            ++total_missing;
          }
        }
      }
    }
    return total_missing;
  };

  absl::btree_map<int, absl::btree_set<int>> base_missing;
  const int base_missing_count = compute_missing_barriers(base_region.sched_block, &base_missing);
  if (base_missing_count == 0) {
    return;
  }

  const int last_p = base_missing.rbegin()->first;
  absl::btree_map<int, int> earliest_site_for_reg;
  for (const auto& [p_pos, regs] : base_missing) {
    for (int r : regs) {
      earliest_site_for_reg.try_emplace(r, p_pos);
    }
  }

  std::vector<absl::btree_set<int>> base_reads(static_cast<size_t>(n));
  std::vector<absl::btree_set<int>> base_writes(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    collect_rw(base_region.sched_block[static_cast<size_t>(i)], &base_reads[static_cast<size_t>(i)],
               &base_writes[static_cast<size_t>(i)]);
  }

  absl::btree_set<int> minimal_seeds;
  for (const auto& [r, first_p] : earliest_site_for_reg) {
    for (int k = first_p - 1; k >= prologue_end; --k) {
      if (base_reads[static_cast<size_t>(k)].contains(r)) {
        minimal_seeds.insert(k);
        break;
      }
      if (base_writes[static_cast<size_t>(k)].contains(r)) {
        break;
      }
    }
  }
  if (minimal_seeds.empty()) {
    return;
  }

  const sched_model::DepGraph g_base =
      sched_model::Analyze(base_region.sched_block, facts_s2, mm, sched_model::Phase::kSched2);
  std::vector<std::vector<int>> fwd_edges(static_cast<size_t>(n));
  for (int cons = 0; cons < n; ++cons) {
    for (const sched_model::Link& l : g_base.log_links[static_cast<size_t>(cons)]) {
      fwd_edges[static_cast<size_t>(l.target)].push_back(cons);
    }
  }

  auto build_forward_closure = [&](const absl::btree_set<int>& seeds) -> absl::btree_set<int> {
    absl::btree_set<int> closure = seeds;
    std::vector<int> work(seeds.begin(), seeds.end());
    while (!work.empty()) {
      const int u = work.back();
      work.pop_back();
      for (int v : fwd_edges[static_cast<size_t>(u)]) {
        if (v >= prologue_end && v < last_p && closure.insert(v).second) {
          work.push_back(v);
        }
      }
    }
    return closure;
  };

  auto is_pure_alu_producer = [](const sched_model::Insn& insn) -> bool {
    if (insn.kind != sched_model::Insn::Kind::kInsn || !insn.pn.pattern.IsSet() ||
        insn.pn.pattern.children.size() != 2) {
      return false;
    }
    const RtlExpr& dst = insn.pn.pattern.SetDest();
    if (!dst.IsReg() || dst.IsArgPointerReg() || dst.flags.integrated) {
      return false;
    }
    bool has_mem_or_fp = false;
    insn.pn.pattern.SetSrc().Visit([&](const RtlExpr& e) {
      if (e.op == RtlOp::kMem || e.op == RtlOp::kUnalignedLoad || e.op == RtlOp::kUnalignedStore ||
          e.op == RtlOp::kCall || e.op == RtlOp::kUnspecVolatile ||
          (e.op == RtlOp::kReg && e.RegNo() >= 32)) {
        has_mem_or_fp = true;
      }
    });
    return !has_mem_or_fp;
  };

  // Expand `minimal_seeds` backwards to include pure-ALU producers whose sole
  // consumers before `last_p` are already inside the sinking set (restoring
  // the pre-`sched2` integer tail block as a unit when `sched2` hoisted it).
  absl::btree_set<int> wide_seeds = build_forward_closure(minimal_seeds);
  bool grew = true;
  while (grew) {
    grew = false;
    for (int u = last_p - 1; u >= prologue_end; --u) {
      if (wide_seeds.contains(u) ||
          !is_pure_alu_producer(base_region.sched_block[static_cast<size_t>(u)])) {
        continue;
      }
      bool has_in_set_succ = false;
      bool has_outside_succ = false;
      for (int v : fwd_edges[static_cast<size_t>(u)]) {
        if (v < last_p) {
          if (wide_seeds.contains(v)) {
            has_in_set_succ = true;
          } else {
            has_outside_succ = true;
            break;
          }
        } else if (base_missing.contains(v)) {
          has_outside_succ = true;
          break;
        }
      }
      if (has_in_set_succ && !has_outside_succ) {
        wide_seeds.insert(u);
        grew = true;
      }
    }
  }

  auto insn_key = [](const sched_model::Insn& insn) {
    std::string k = insn.pn.pattern.ToSexprString();
    if (insn.pn.has_explicit_call_fusage) {
      k.push_back('|');
      for (const RtlExpr& e : insn.pn.call_fusage) {
        k.append(e.ToSexprString());
        k.push_back(';');
      }
    }
    return k;
  };
  std::vector<std::string> target_keys(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    target_keys[static_cast<size_t>(i)] =
        insn_key(target_region.sched_block[static_cast<size_t>(i)]);
  }
  sched_model::Sched1ProjectionContext proj_ctx =
      sched_model::BuildSched1ProjectionContextForBlock(b, cfg_ctx);
  proj_ctx.allow_post_call_combine_regs_split = false;

  std::optional<std::vector<int>> fallback_cand;
  for (const absl::btree_set<int>& seed_set : {wide_seeds, build_forward_closure(minimal_seeds)}) {
    bool blocked = false;
    for (int u : seed_set) {
      for (int v : fwd_edges[static_cast<size_t>(u)]) {
        if (v <= last_p && !seed_set.contains(v)) {
          blocked = true;
          break;
        }
      }
      if (blocked) {
        break;
      }
    }
    if (blocked) {
      continue;
    }

    std::vector<int> cand_in_base;
    cand_in_base.reserve(static_cast<size_t>(n));
    for (int i = 0; i < prologue_end; ++i) {
      cand_in_base.push_back(i);
    }
    for (int i = prologue_end; i <= last_p; ++i) {
      if (!seed_set.contains(i)) {
        cand_in_base.push_back(i);
      }
    }
    for (int i = prologue_end; i < last_p; ++i) {
      if (seed_set.contains(i)) {
        cand_in_base.push_back(i);
      }
    }
    for (int i = last_p + 1; i < n; ++i) {
      cand_in_base.push_back(i);
    }

    std::vector<sched_model::Insn> cand_seq;
    cand_seq.reserve(static_cast<size_t>(n));
    for (int pos = 0; pos < n; ++pos) {
      sched_model::Insn insn =
          base_region.sched_block[static_cast<size_t>(cand_in_base[static_cast<size_t>(pos)])];
      insn.luid = pos;
      cand_seq.push_back(std::move(insn));
    }

    absl::btree_map<int, absl::btree_set<int>> cand_missing;
    if (compute_missing_barriers(cand_seq, &cand_missing) >= base_missing_count) {
      continue;
    }

    const absl::btree_set<int> cand_excl =
        sched_model::FindInterleavedPrologueSaveBodyIndices(cand_seq, bounds, b, mm);
    if (!sched_model::PreservesPostReloadOrderSafety(
            base_region.sched_block, cand_in_base, prologue_end, epilogue_begin, live_at_end_s2,
            cfg_ctx.cfg_copy_pairs, facts_s2, mm,
            /*allow_pure_reader_reg_dead_transfer=*/false, cfg_ctx.stack_layout, cand_excl)) {
      continue;
    }

    const sched_model::DepGraph g_s2 =
        sched_model::Analyze(cand_seq, facts_s2, mm, sched_model::Phase::kSched2);
    const sched_model::ScheduleResult sr_s2 = sched_model::Schedule(
        cand_seq, g_s2, facts_s2, mm, sched_model::Phase::kSched2, live_at_end_s2);
    if (static_cast<int>(sr_s2.order.size()) != n) {
      continue;
    }
    bool replay_ok = true;
    for (int pos = 0; pos < n; ++pos) {
      if (insn_key(cand_seq[static_cast<size_t>(sr_s2.order[static_cast<size_t>(pos)])]) !=
          target_keys[static_cast<size_t>(pos)]) {
        replay_ok = false;
        break;
      }
    }
    if (!replay_ok) {
      continue;
    }

    std::vector<sched_model::Insn> cand_body;
    cand_body.reserve(static_cast<size_t>(body_len));
    for (int pos = prologue_end; pos < epilogue_begin; ++pos) {
      sched_model::Insn insn = cand_seq[static_cast<size_t>(pos)];
      insn.luid = pos - prologue_end;
      cand_body.push_back(std::move(insn));
    }
    sched_model::Sched1ProjectionContext cand_ctx = proj_ctx;
    cand_ctx.excluded_prologue_body_indices = cand_excl;
    const sched_model::Sched1ProjectionView cand_view =
        sched_model::ProjectPostReloadBlockToSched1View(cand_body, mm, live_at_end_s2, cand_ctx);
    const bool s1_ach =
        cand_view.core_insns.size() <= 1 ||
        sched_model::IsAchievable(cand_view.core_insns, cand_view.facts_s1, mm,
                                  sched_model::Phase::kSched1, cand_view.live_at_end_s1);
    if (s1_ach) {
      ApplyReorderedSchedRegion(out_blk, base_region, cand_in_base);
      return;
    }
    if (!fallback_cand.has_value()) {
      fallback_cand = std::move(cand_in_base);
    }
  }
  if (fallback_cand.has_value()) {
    ApplyReorderedSchedRegion(out_blk, base_region, *fallback_cand);
  }
}

// Mirrors `sched.c:4803-4821` (`single_set(insn)` + `rtx_equal_p(SET_SRC, SET_DEST)`
// with `rtx_equal_function_value_matters == 0`, `toplev.c:2779`).
bool RtxEqualForNoopMove(const RtlExpr& a, const RtlExpr& b) {
  if (a.op != b.op || a.mode != b.mode || a.children.size() != b.children.size()) {
    return false;
  }
  if (a.op == RtlOp::kReg) {
    return a.RegNo() == b.RegNo();
  }
  if (a.op == RtlOp::kConstInt || a.op == RtlOp::kCodeLabel) {
    return a.int_val == b.int_val;
  }
  if (a.op == RtlOp::kSymbolRef || a.op == RtlOp::kLabelRef) {
    return a.SymbolName() == b.SymbolName() && a.int_val == b.int_val;
  }
  for (size_t i = 0; i < a.children.size(); ++i) {
    if (!RtxEqualForNoopMove(a.children[i], b.children[i])) {
      return false;
    }
  }
  return a.ToSexprString() == b.ToSexprString();
}

bool IsSched2NoopSelfMoveInsn(const sched_model::Insn& insn) {
  if (insn.kind != sched_model::Insn::Kind::kInsn) {
    return false;
  }
  const RtlExpr* set = nullptr;
  if (insn.pn.pattern.op == RtlOp::kSet && insn.pn.pattern.children.size() >= 2) {
    set = &insn.pn.pattern;
  } else if (insn.pn.pattern.op == RtlOp::kParallel) {
    for (const RtlExpr& ch : insn.pn.pattern.children) {
      if (ch.op == RtlOp::kSet && ch.children.size() >= 2) {
        if (set != nullptr) {
          return false;
        }
        set = &ch;
      } else if (ch.op != RtlOp::kClobber && ch.op != RtlOp::kUse) {
        return false;
      }
    }
  }
  return set != nullptr && RtxEqualForNoopMove(set->SetSrc(), set->SetDest());
}

// Monotonic `kSched2` right-inverse safeguard (`sched.c:schedule_block`): if
// heuristic pre-unscheduling (`UnscheduleBlockInstructions` /
// `UnscheduleSched2ReloadsAndCallerSaves`) broke exact forward `kSched2` replay
// (e.g., by sinking `caller-save` restore loads past equal-priority ALU/const
// loads and shifting `INSN_LUID` tie-break order, or moving a surviving
// post-reload no-op self-move off `basic_block_end[b]`, `sched.c:4803-4818`)
// and 3-gate `kSched1` selection did not replace the block, repair the block
// via `InvertBlockOrder` using the heuristically unscheduled order as a soft
// preference.
void RepairSched2BlockReplayIfBroken(RtlBasicBlock* out_blk, const RtlBasicBlock& target_blk,
                                     const RtlBasicBlock* pre_unsched_blk, size_t b,
                                     const sched_model::MachineModel& mm,
                                     const sched_model::RegFacts& facts_s2,
                                     const RtlControlFlowGraph& live_cfg) {
  const sched_model::ParsedSchedRegion out_region =
      sched_model::ParseBlockForSchedModel(*out_blk, b, sched_model::Phase::kSched2);
  const sched_model::ParsedSchedRegion target_region =
      sched_model::ParseBlockForSchedModel(target_blk, b, sched_model::Phase::kSched2);
  const int n = static_cast<int>(target_region.sched_block.size());
  if (!out_region.valid || !target_region.valid || n <= 1 ||
      static_cast<int>(out_region.sched_block.size()) != n) {
    return;
  }
  absl::btree_set<int> live_at_end_s2;
  for (int succ : out_blk->successors) {
    if (const auto it = live_cfg.sideband.basic_block_live_at_start.find(succ);
        it != live_cfg.sideband.basic_block_live_at_start.end()) {
      live_at_end_s2.insert(it->second.begin(), it->second.end());
    }
  }
  auto pat_key = [](const sched_model::Insn& insn) {
    return insn.pn.pattern.ToGccPrintRtlString();
  };
  std::vector<std::string> target_pats;
  target_pats.reserve(static_cast<size_t>(n));
  for (const sched_model::Insn& insn : target_region.sched_block) {
    target_pats.push_back(pat_key(insn));
  }
  auto replays_target = [&](const sched_model::ParsedSchedRegion& region) -> bool {
    if (!region.valid || static_cast<int>(region.sched_block.size()) != n) {
      return false;
    }
    // `sched.c:4803-4818`: post-reload `schedule_insns` deletes every no-op
    // self-move before `schedule_block` except `basic_block_end[b]`.
    for (int pos = 0; pos + 1 < n; ++pos) {
      if (IsSched2NoopSelfMoveInsn(region.sched_block[static_cast<size_t>(pos)])) {
        return false;
      }
    }
    const sched_model::DepGraph g =
        sched_model::Analyze(region.sched_block, facts_s2, mm, sched_model::Phase::kSched2);
    const sched_model::ScheduleResult sr = sched_model::Schedule(
        region.sched_block, g, facts_s2, mm, sched_model::Phase::kSched2, live_at_end_s2);
    if (static_cast<int>(sr.order.size()) != n) {
      return false;
    }
    for (int pos = 0; pos < n; ++pos) {
      if (pat_key(region.sched_block[static_cast<size_t>(sr.order[static_cast<size_t>(pos)])]) !=
          target_pats[static_cast<size_t>(pos)]) {
        return false;
      }
    }
    return true;
  };
  if (replays_target(out_region)) {
    return;
  }
  // Map `out_region` indices to `target_region` indices by pattern so restored
  // `REG_DEAD` / `REG_UNUSED` notes on `*out_blk` are preserved verbatim when
  // reordering.
  std::vector<int> preferred_in_target;
  std::vector<int> target_to_out(static_cast<size_t>(n), -1);
  std::vector<bool> used_target(static_cast<size_t>(n), false);
  bool bijective = true;
  for (int oi = 0; oi < n; ++oi) {
    const std::string key = pat_key(out_region.sched_block[static_cast<size_t>(oi)]);
    int matched = -1;
    for (int j = 0; j < n; ++j) {
      if (!used_target[static_cast<size_t>(j)] && target_pats[static_cast<size_t>(j)] == key) {
        used_target[static_cast<size_t>(j)] = true;
        matched = j;
        break;
      }
    }
    if (matched < 0) {
      bijective = false;
      break;
    }
    preferred_in_target.push_back(matched);
    target_to_out[static_cast<size_t>(matched)] = oi;
  }
  if (bijective) {
    // Build a target-ordered block carrying `*out_blk`'s restored notes so
    // `InvertBlockOrder` operates on the exact post-note-restoration insns.
    RtlBasicBlock target_with_out_notes = *out_blk;
    ApplyReorderedSchedRegion(&target_with_out_notes, out_region, target_to_out);
    const sched_model::ParsedSchedRegion target_out_region =
        sched_model::ParseBlockForSchedModel(target_with_out_notes, b, sched_model::Phase::kSched2);
    if (target_out_region.valid && static_cast<int>(target_out_region.sched_block.size()) == n) {
      for (int attempt = 0; attempt < 2; ++attempt) {
        const absl::Span<const int> pref =
            (attempt == 0) ? absl::Span<const int>(preferred_in_target) : absl::Span<const int>{};
        const std::optional<std::vector<int>> inv =
            sched_model::InvertBlockOrder(target_out_region.sched_block, facts_s2, mm,
                                          sched_model::Phase::kSched2, live_at_end_s2, pref);
        if (inv.has_value()) {
          RtlBasicBlock repaired = target_with_out_notes;
          ApplyReorderedSchedRegion(&repaired, target_out_region, *inv);
          const sched_model::ParsedSchedRegion rep_region =
              sched_model::ParseBlockForSchedModel(repaired, b, sched_model::Phase::kSched2);
          if (replays_target(rep_region)) {
            *out_blk = std::move(repaired);
            return;
          }
        }
      }
    }
  }
  if (pre_unsched_blk != nullptr) {
    const sched_model::ParsedSchedRegion pre_region =
        sched_model::ParseBlockForSchedModel(*pre_unsched_blk, b, sched_model::Phase::kSched2);
    if (replays_target(pre_region)) {
      *out_blk = *pre_unsched_blk;
    }
  }
}

void SelectSched1AchievablePreimageForSched2(RtlControlFlowGraph* out,
                                             const RtlControlFlowGraph& scheduled_cfg,
                                             const CompilationProfile& profile,
                                             const RtlControlFlowGraph* pre_unsched_cfg = nullptr) {
  if (out->rpo_blocks.size() != scheduled_cfg.rpo_blocks.size()) {
    return;
  }
  const sched_model::MachineModel mm = sched_model::MachineModel::FromProfile(profile);
  const sched_model::RegFacts facts_s2 =
      sched_model::RegFacts::FromCfg(scheduled_cfg, profile, sched_model::Phase::kSched2);

  RtlControlFlowGraph live_cfg = *out;
  ReconstructRtlLiveness(
      &live_cfg, profile,
      {.reload_completed = true, .update_reg_notes = false, .preserve_phantom_pseudos = false});
  const std::vector<int> restored_b0_args = PredictRestoredEntryArgSelfMoveRegs(*out, profile);
  const sched_model::CfgSched1Context cfg_ctx =
      sched_model::ComputeCfgSched1Context(*out, live_cfg, mm, restored_b0_args);

  std::optional<RtlControlFlowGraph> pre_live_cfg;
  std::optional<sched_model::CfgSched1Context> pre_cfg_ctx;

  for (size_t b = 0; b < out->rpo_blocks.size(); ++b) {
    if (TrySelectSched1AchievablePreimageForBlock(&out->rpo_blocks[b], scheduled_cfg.rpo_blocks[b],
                                                  b, mm, facts_s2, cfg_ctx, live_cfg)) {
      SinkCallerSaveRestoresIfVerified(&out->rpo_blocks[b], scheduled_cfg.rpo_blocks[b], b, mm,
                                       facts_s2, cfg_ctx, live_cfg);
      SinkPreScratchDeadChainsIfVerified(&out->rpo_blocks[b], scheduled_cfg.rpo_blocks[b], b, mm,
                                         facts_s2, cfg_ctx, live_cfg);
      continue;
    }
    // If heuristic pre-unscheduling (`UnscheduleBlockInstructions` /
    // `UnscheduleSched2ReloadsAndCallerSaves`) locked an incompatible `reload_adjacent`
    // or base ordering, retry strictly from the pre-unscheduled post-reload block
    // (the exact Stage 0 domain) under all three gates.
    bool solved_from_pre = false;
    if (pre_unsched_cfg != nullptr &&
        pre_unsched_cfg->rpo_blocks.size() == out->rpo_blocks.size() &&
        pre_unsched_cfg->rpo_blocks[b].instructions != out->rpo_blocks[b].instructions) {
      if (!pre_live_cfg.has_value()) {
        pre_live_cfg = *pre_unsched_cfg;
        ReconstructRtlLiveness(&*pre_live_cfg, profile,
                               {.reload_completed = true,
                                .update_reg_notes = false,
                                .preserve_phantom_pseudos = false});
        const std::vector<int> pre_b0_args =
            PredictRestoredEntryArgSelfMoveRegs(*pre_unsched_cfg, profile);
        pre_cfg_ctx =
            sched_model::ComputeCfgSched1Context(*pre_unsched_cfg, *pre_live_cfg, mm, pre_b0_args);
      }
      RtlBasicBlock fallback_blk = pre_unsched_cfg->rpo_blocks[b];
      if (TrySelectSched1AchievablePreimageForBlock(&fallback_blk, scheduled_cfg.rpo_blocks[b], b,
                                                    mm, facts_s2, *pre_cfg_ctx, *pre_live_cfg)) {
        SinkCallerSaveRestoresIfVerified(&fallback_blk, scheduled_cfg.rpo_blocks[b], b, mm,
                                         facts_s2, *pre_cfg_ctx, *pre_live_cfg);
        SinkPreScratchDeadChainsIfVerified(&fallback_blk, scheduled_cfg.rpo_blocks[b], b, mm,
                                           facts_s2, *pre_cfg_ctx, *pre_live_cfg);
        out->rpo_blocks[b] = std::move(fallback_blk);
        solved_from_pre = true;
      }
    }
    if (!solved_from_pre) {
      // When strict `validate_equiv_mem` preservation finds no `kSched1`
      // preimage (e.g., `sched2` hoisted `REG_DEAD dst_r` ahead of an aliasing
      // store and Stage 0 stripped the spurious load `REG_EQUIV`, `local-alloc.c:
      // 540-603, 1049-1055`), retry allowing aliasing stores to move back into
      // `[load, dst_death)` for loads without a matching `REG_EQUIV`.
      if (TrySelectSched1AchievablePreimageForBlock(
              &out->rpo_blocks[b], scheduled_cfg.rpo_blocks[b], b, mm, facts_s2, cfg_ctx, live_cfg,
              /*allow_moved_store_load_equiv_invalidation=*/true)) {
        SinkCallerSaveRestoresIfVerified(&out->rpo_blocks[b], scheduled_cfg.rpo_blocks[b], b, mm,
                                         facts_s2, cfg_ctx, live_cfg);
        SinkPreScratchDeadChainsIfVerified(&out->rpo_blocks[b], scheduled_cfg.rpo_blocks[b], b, mm,
                                           facts_s2, cfg_ctx, live_cfg);
        solved_from_pre = true;
      } else if (pre_unsched_cfg != nullptr &&
                 pre_unsched_cfg->rpo_blocks.size() == out->rpo_blocks.size() &&
                 pre_unsched_cfg->rpo_blocks[b].instructions != out->rpo_blocks[b].instructions) {
        RtlBasicBlock fallback_blk = pre_unsched_cfg->rpo_blocks[b];
        if (TrySelectSched1AchievablePreimageForBlock(
                &fallback_blk, scheduled_cfg.rpo_blocks[b], b, mm, facts_s2, *pre_cfg_ctx,
                *pre_live_cfg, /*allow_moved_store_load_equiv_invalidation=*/true)) {
          SinkCallerSaveRestoresIfVerified(&fallback_blk, scheduled_cfg.rpo_blocks[b], b, mm,
                                           facts_s2, *pre_cfg_ctx, *pre_live_cfg);
          SinkPreScratchDeadChainsIfVerified(&fallback_blk, scheduled_cfg.rpo_blocks[b], b, mm,
                                             facts_s2, *pre_cfg_ctx, *pre_live_cfg);
          out->rpo_blocks[b] = std::move(fallback_blk);
          solved_from_pre = true;
        }
      }
    }
    if (!solved_from_pre) {
      const RtlBasicBlock* pre_blk = (pre_unsched_cfg != nullptr &&
                                      pre_unsched_cfg->rpo_blocks.size() == out->rpo_blocks.size())
                                         ? &pre_unsched_cfg->rpo_blocks[b]
                                         : nullptr;
      const std::vector<std::string> before_repair = out->rpo_blocks[b].instructions;
      RepairSched2BlockReplayIfBroken(&out->rpo_blocks[b], scheduled_cfg.rpo_blocks[b], pre_blk, b,
                                      mm, facts_s2, live_cfg);
      if (out->rpo_blocks[b].instructions != before_repair &&
          TrySelectSched1AchievablePreimageForBlock(&out->rpo_blocks[b],
                                                    scheduled_cfg.rpo_blocks[b], b, mm, facts_s2,
                                                    cfg_ctx, live_cfg)) {
        SinkCallerSaveRestoresIfVerified(&out->rpo_blocks[b], scheduled_cfg.rpo_blocks[b], b, mm,
                                         facts_s2, cfg_ctx, live_cfg);
      }
      SinkPreScratchDeadChainsIfVerified(&out->rpo_blocks[b], scheduled_cfg.rpo_blocks[b], b, mm,
                                         facts_s2, cfg_ctx, live_cfg);
    }
  }
}

// Re-aligns any block where `RestoreSched2DeletedNoopMoves` inserted a
// `sched.c:4803-4821`-deleted early-return noop copy `(set (reg/i:M r) (reg:M r))`
// immediately before its conditional jump. Because `sched2` deletes the noop
// copy before scheduling while `sched1` schedules the pre-allocation copy
// `(set (reg/i:M r) (reg:M P))` in place, verifies:
//   (a) `Schedule(Q_cand \ {copy}, kSched2) == P_target`,
//   (b) `PreservesPostReloadOrderSafety(Q_base \ {copy}, Q_cand \ {copy}, ...)`,
//   (c) `IsAchievable(ProjectPostReloadBlockToSched1View(Q_cand), kSched1)`.
void AlignEarlyReturnNoopMoveBlockForSched1(RtlControlFlowGraph* out,
                                            const RtlControlFlowGraph& scheduled_cfg,
                                            const CompilationProfile& profile) {
  if (out == nullptr || out->rpo_blocks.size() != scheduled_cfg.rpo_blocks.size()) {
    return;
  }
  const sched_model::MachineModel mm = sched_model::MachineModel::FromProfile(profile);
  const sched_model::RegFacts facts_s2 =
      sched_model::RegFacts::FromCfg(scheduled_cfg, profile, sched_model::Phase::kSched2);

  RtlControlFlowGraph live_cfg = *out;
  ReconstructRtlLiveness(
      &live_cfg, profile,
      {.reload_completed = true, .update_reg_notes = false, .preserve_phantom_pseudos = false});
  const std::vector<int> restored_b0_args = PredictRestoredEntryArgSelfMoveRegs(*out, profile);
  const sched_model::CfgSched1Context cfg_ctx =
      sched_model::ComputeCfgSched1Context(*out, live_cfg, mm, restored_b0_args);

  auto is_noop_self_move_insn = [](const sched_model::Insn& insn) {
    return insn.kind == sched_model::Insn::Kind::kInsn && insn.pn.pattern.IsSet() &&
           insn.pn.pattern.SetDest().IsHardReg() && insn.pn.pattern.SetSrc().IsHardReg() &&
           insn.pn.pattern.SetDest().RegNo() == insn.pn.pattern.SetSrc().RegNo() &&
           insn.pn.pattern.SetDest().mode == insn.pn.pattern.SetSrc().mode;
  };

  for (size_t b = 0; b < out->rpo_blocks.size(); ++b) {
    RtlBasicBlock& out_blk = out->rpo_blocks[b];
    const RtlBasicBlock& target_blk = scheduled_cfg.rpo_blocks[b];
    if (out_blk.instructions.size() <= target_blk.instructions.size()) {
      continue;
    }
    const sched_model::ParsedSchedRegion out_region =
        sched_model::ParseBlockForSchedModel(out_blk, b, sched_model::Phase::kSched2);
    const sched_model::ParsedSchedRegion target_region =
        sched_model::ParseBlockForSchedModel(target_blk, b, sched_model::Phase::kSched2);
    const int m = static_cast<int>(out_region.sched_block.size());
    const int n = static_cast<int>(target_region.sched_block.size());
    if (!out_region.valid || !target_region.valid || m <= n || n <= 1) {
      continue;
    }
    const int copy_idx = m - 2;
    const sched_model::Insn& copy_insn = out_region.sched_block[static_cast<size_t>(copy_idx)];
    const sched_model::Insn& tail_insn = out_region.sched_block[static_cast<size_t>(m - 1)];
    if (tail_insn.kind != sched_model::Insn::Kind::kJump || !is_noop_self_move_insn(copy_insn) ||
        !copy_insn.pn.pattern.SetDest().flags.integrated) {
      continue;
    }
    const sched_model::PostReloadBodyBounds bounds =
        sched_model::FindPostReloadBodyBounds(out_region.sched_block, b);
    const int prologue_end = bounds.prologue_end;
    const int epilogue_begin = bounds.epilogue_begin;
    const int body_len = epilogue_begin - prologue_end;
    if (body_len <= 2 || copy_idx < prologue_end || copy_idx >= epilogue_begin) {
      continue;
    }

    absl::btree_set<int> live_at_end_s2;
    for (int succ : target_blk.successors) {
      if (const auto it = live_cfg.sideband.basic_block_live_at_start.find(succ);
          it != live_cfg.sideband.basic_block_live_at_start.end()) {
        live_at_end_s2.insert(it->second.begin(), it->second.end());
      }
    }

    std::vector<sched_model::Insn> out_body;
    out_body.reserve(static_cast<size_t>(body_len));
    for (int i = prologue_end; i < epilogue_begin; ++i) {
      sched_model::Insn insn = out_region.sched_block[static_cast<size_t>(i)];
      insn.luid = i - prologue_end;
      out_body.push_back(std::move(insn));
    }
    sched_model::Sched1ProjectionContext proj_ctx =
        sched_model::BuildSched1ProjectionContextForBlock(b, cfg_ctx);
    proj_ctx.allow_post_call_combine_regs_split = false;
    proj_ctx.excluded_prologue_body_indices =
        sched_model::FindInterleavedPrologueSaveBodyIndices(out_region.sched_block, bounds, b, mm);
    const sched_model::Sched1ProjectionView out_view =
        sched_model::ProjectPostReloadBlockToSched1View(out_body, mm, live_at_end_s2, proj_ctx);
    const int num_core = static_cast<int>(out_view.core_insns.size());
    if (num_core <= 1 ||
        num_core + static_cast<int>(out_view.pinned_prefix_orig_indices.size()) != body_len ||
        sched_model::IsAchievable(out_view.core_insns, out_view.facts_s1, mm,
                                  sched_model::Phase::kSched1, out_view.live_at_end_s1)) {
      continue;
    }

    const std::vector<int> s1_order = sched_model::ScheduleSched1ViewCoreOrder(out_view, mm);
    if (static_cast<int>(s1_order.size()) != num_core) {
      continue;
    }
    std::vector<int> cand_in_out;
    cand_in_out.reserve(static_cast<size_t>(m));
    for (int i = 0; i < prologue_end; ++i) {
      cand_in_out.push_back(i);
    }
    for (int pp_rel : out_view.pinned_prefix_orig_indices) {
      cand_in_out.push_back(prologue_end + pp_rel);
    }
    for (int ci : s1_order) {
      cand_in_out.push_back(prologue_end + out_view.core_to_orig_index[static_cast<size_t>(ci)]);
    }
    for (int i = epilogue_begin; i < m; ++i) {
      cand_in_out.push_back(i);
    }
    if (static_cast<int>(cand_in_out.size()) != m ||
        cand_in_out[static_cast<size_t>(m - 1)] != m - 1) {
      continue;
    }

    // Build `base_wo` and `cand_wo` without non-block-end `noop_move_p` insns
    // (`sched.c:4805-4818` deletes them before `schedule_block`) and verify Gate (a) + Gate (b).
    absl::btree_set<int> noop_indices;
    for (int i = 0; i + 1 < m; ++i) {
      if (is_noop_self_move_insn(out_region.sched_block[static_cast<size_t>(i)])) {
        noop_indices.insert(i);
      }
    }
    if (m - static_cast<int>(noop_indices.size()) != n) {
      continue;
    }
    std::vector<sched_model::Insn> base_wo;
    std::vector<int> out_to_wo(static_cast<size_t>(m), -1);
    base_wo.reserve(static_cast<size_t>(n));
    for (int i = 0; i < m; ++i) {
      if (noop_indices.contains(i)) {
        continue;
      }
      out_to_wo[static_cast<size_t>(i)] = static_cast<int>(base_wo.size());
      sched_model::Insn insn = out_region.sched_block[static_cast<size_t>(i)];
      insn.luid = static_cast<int>(base_wo.size());
      base_wo.push_back(std::move(insn));
    }
    std::vector<int> cand_wo_in_base;
    cand_wo_in_base.reserve(static_cast<size_t>(n));
    std::vector<sched_model::Insn> cand_wo;
    cand_wo.reserve(static_cast<size_t>(n));
    for (int oi : cand_in_out) {
      if (noop_indices.contains(oi)) {
        continue;
      }
      const int bi = out_to_wo[static_cast<size_t>(oi)];
      cand_wo_in_base.push_back(bi);
      sched_model::Insn insn = base_wo[static_cast<size_t>(bi)];
      insn.luid = static_cast<int>(cand_wo.size());
      cand_wo.push_back(std::move(insn));
    }
    if (static_cast<int>(cand_wo.size()) != n) {
      continue;
    }
    const sched_model::DepGraph cand_wo_graph =
        sched_model::Analyze(cand_wo, facts_s2, mm, sched_model::Phase::kSched2);
    const std::vector<int> replayed_s2 =
        sched_model::Schedule(cand_wo, cand_wo_graph, facts_s2, mm, sched_model::Phase::kSched2,
                              live_at_end_s2)
            .order;
    if (static_cast<int>(replayed_s2.size()) != n) {
      continue;
    }
    bool gate_a_ok = true;
    for (int i = 0; i < n; ++i) {
      if (BuildSchedModelPatternKey(
              cand_wo[static_cast<size_t>(replayed_s2[static_cast<size_t>(i)])]) !=
          BuildSchedModelPatternKey(target_region.sched_block[static_cast<size_t>(i)])) {
        gate_a_ok = false;
        break;
      }
    }
    if (!gate_a_ok) {
      continue;
    }
    const sched_model::PostReloadBodyBounds wo_bounds =
        sched_model::FindPostReloadBodyBounds(base_wo, b);
    if (!sched_model::PreservesPostReloadOrderSafety(
            base_wo, cand_wo_in_base, wo_bounds.prologue_end, wo_bounds.epilogue_begin,
            live_at_end_s2, cfg_ctx.cfg_copy_pairs, facts_s2, mm,
            /*allow_pure_reader_reg_dead_transfer=*/false, cfg_ctx.stack_layout,
            proj_ctx.excluded_prologue_body_indices)) {
      continue;
    }

    // Verify Gate (c) on `cand_in_out` WITH `copy_idx`.
    std::vector<sched_model::Insn> cand_body;
    cand_body.reserve(static_cast<size_t>(body_len));
    for (int pos = prologue_end; pos < epilogue_begin; ++pos) {
      sched_model::Insn insn =
          out_region.sched_block[static_cast<size_t>(cand_in_out[static_cast<size_t>(pos)])];
      insn.luid = pos - prologue_end;
      cand_body.push_back(std::move(insn));
    }
    const sched_model::Sched1ProjectionView cand_view =
        sched_model::ProjectPostReloadBlockToSched1View(cand_body, mm, live_at_end_s2, proj_ctx);
    if (cand_view.core_insns.size() <= 1 ||
        !sched_model::IsAchievable(cand_view.core_insns, cand_view.facts_s1, mm,
                                   sched_model::Phase::kSched1, cand_view.live_at_end_s1)) {
      continue;
    }
    ApplyReorderedSchedRegion(&out_blk, out_region, cand_in_out);
  }
}

// Restores and aligns a `sched.c:4803-4821`-deleted `COND_EXPR` diamond
// pre-branch noop copy `(set (reg:M r) (reg:M r)) (REG_DEAD r)`
// (`expr.c:5867-5908`, `jump.c:699-834` `if (...) x = a; else x = b;` ->
// `x = b; if (...) x = a;`) ONLY when the block is otherwise NOT in
// `Im(F_sched1)` and the restored + reordered block strictly passes all three gates:
//   (a) `Schedule(Q_cand \ {noops}, kSched2) == P_target` (via `InvertBlockOrder`),
//   (b) `PreservesPostReloadOrderSafety(Q_base \ {noops}, Q_cand \ {noops}, ...)`,
//   (c) `IsAchievable(ProjectPostReloadBlockToSched1View(Q_cand), kSched1)`.
void RestoreAndAlignCondExprNoopMoveForSched1(RtlControlFlowGraph* out,
                                              const RtlControlFlowGraph& scheduled_cfg,
                                              const CompilationProfile& profile) {
  if (out == nullptr || out->rpo_blocks.size() != scheduled_cfg.rpo_blocks.size()) {
    return;
  }
  const sched_model::MachineModel mm = sched_model::MachineModel::FromProfile(profile);
  const sched_model::RegFacts facts_s2 =
      sched_model::RegFacts::FromCfg(scheduled_cfg, profile, sched_model::Phase::kSched2);

  RtlControlFlowGraph live_cfg = *out;
  ReconstructRtlLiveness(
      &live_cfg, profile,
      {.reload_completed = true, .update_reg_notes = false, .preserve_phantom_pseudos = false});
  const std::vector<int> restored_b0_args = PredictRestoredEntryArgSelfMoveRegs(*out, profile);
  const sched_model::CfgSched1Context cfg_ctx =
      sched_model::ComputeCfgSched1Context(*out, live_cfg, mm, restored_b0_args);

  auto is_noop_self_move_insn = [](const sched_model::Insn& insn) {
    return insn.kind == sched_model::Insn::Kind::kInsn && insn.pn.pattern.IsSet() &&
           insn.pn.pattern.SetDest().IsHardReg() && insn.pn.pattern.SetSrc().IsHardReg() &&
           insn.pn.pattern.SetDest().RegNo() == insn.pn.pattern.SetSrc().RegNo() &&
           insn.pn.pattern.SetDest().mode == insn.pn.pattern.SetSrc().mode;
  };
  auto extract_zero_cmp_hard_reg = [](const RtlExpr& jump_pat, int* out_reg,
                                      RtlMachineMode* out_mode) -> bool {
    if (!jump_pat.IsSet() || jump_pat.SetDest().op != RtlOp::kPc ||
        jump_pat.SetSrc().op != RtlOp::kIfThenElse || jump_pat.SetSrc().children.size() != 3) {
      return false;
    }
    const RtlExpr& cond = jump_pat.SetSrc().children[0];
    if (cond.children.size() != 2) {
      return false;
    }
    const RtlExpr* reg_side = nullptr;
    if (cond.children[0].IsHardReg() && !cond.children[0].IsArgPointerReg() &&
        cond.children[1].IsConstInt() && cond.children[1].int_val == 0) {
      reg_side = &cond.children[0];
    } else if (cond.children[1].IsHardReg() && !cond.children[1].IsArgPointerReg() &&
               cond.children[0].IsConstInt() && cond.children[0].int_val == 0) {
      reg_side = &cond.children[1];
    }
    if (reg_side == nullptr || reg_side->RegNo() < 2 || reg_side->RegNo() >= 64) {
      return false;
    }
    *out_reg = reg_side->RegNo();
    *out_mode = reg_side->mode;
    return true;
  };
  auto block_unconditionally_overwrites_reg = [](const RtlBasicBlock& blk, int regno,
                                                 RtlMachineMode mode) -> bool {
    bool saw_set = false;
    for (const std::string& raw : blk.instructions) {
      auto p = ParsePatternAndNotes(raw);
      if (!p.ok()) {
        return false;
      }
      if (p->pattern.op == RtlOp::kNote || p->pattern.op == RtlOp::kCodeLabel) {
        continue;
      }
      if (p->pattern.IsSet() && p->pattern.SetDest().IsHardReg() &&
          p->pattern.SetDest().RegNo() == regno && p->pattern.SetDest().mode == mode &&
          !p->pattern.SetSrc().ContainsReg(regno)) {
        saw_set = true;
        continue;
      }
      return false;
    }
    return saw_set;
  };

  for (size_t b = 0; b < out->rpo_blocks.size(); ++b) {
    RtlBasicBlock& out_blk = out->rpo_blocks[b];
    const RtlBasicBlock& target_blk = scheduled_cfg.rpo_blocks[b];
    if (out_blk.successors.size() != 2) {
      continue;
    }
    const sched_model::ParsedSchedRegion out_region =
        sched_model::ParseBlockForSchedModel(out_blk, b, sched_model::Phase::kSched2);
    const sched_model::ParsedSchedRegion target_region =
        sched_model::ParseBlockForSchedModel(target_blk, b, sched_model::Phase::kSched2);
    const int n = static_cast<int>(target_region.sched_block.size());
    const int m_orig = static_cast<int>(out_region.sched_block.size());
    if (!out_region.valid || !target_region.valid || n <= 2 || m_orig < n ||
        out_region.has_mid_block_loop_barrier || target_region.has_mid_block_loop_barrier) {
      continue;
    }
    const sched_model::Insn& tail_insn = out_region.sched_block.back();
    int cond_reg = -1;
    RtlMachineMode cond_mode = RtlMachineMode::kNone;
    if (tail_insn.kind != sched_model::Insn::Kind::kJump ||
        !extract_zero_cmp_hard_reg(tail_insn.pn.pattern, &cond_reg, &cond_mode)) {
      continue;
    }
    // Verify diamond `b -> {b_fall, b_merge}` where `b_fall` unconditionally
    // overwrites `cond_reg` and has sole successor `b_merge`, and `cond_reg` is
    // live-in at `b_merge`.
    const size_t s0 = out_blk.successors[0];
    const size_t s1 = out_blk.successors[1];
    auto is_diamond_pair = [&](size_t fall_id, size_t merge_id) -> bool {
      if (fall_id >= out->rpo_blocks.size() || merge_id >= out->rpo_blocks.size()) {
        return false;
      }
      const RtlBasicBlock& fall_blk = out->rpo_blocks[fall_id];
      if (fall_blk.successors.size() != 1 || fall_blk.successors[0] != merge_id) {
        return false;
      }
      const auto live_it =
          live_cfg.sideband.basic_block_live_at_start.find(static_cast<int>(merge_id));
      if (live_it == live_cfg.sideband.basic_block_live_at_start.end() ||
          !live_it->second.contains(cond_reg)) {
        return false;
      }
      return block_unconditionally_overwrites_reg(fall_blk, cond_reg, cond_mode);
    };
    if (!is_diamond_pair(s0, s1) && !is_diamond_pair(s1, s0)) {
      continue;
    }

    absl::btree_set<int> live_at_end_s2;
    for (int succ : target_blk.successors) {
      if (const auto it = live_cfg.sideband.basic_block_live_at_start.find(succ);
          it != live_cfg.sideband.basic_block_live_at_start.end()) {
        live_at_end_s2.insert(it->second.begin(), it->second.end());
      }
    }
    const sched_model::PostReloadBodyBounds orig_bounds =
        sched_model::FindPostReloadBodyBounds(out_region.sched_block, b);
    const int orig_body_len = orig_bounds.epilogue_begin - orig_bounds.prologue_end;
    if (orig_body_len <= 2) {
      continue;
    }
    std::vector<sched_model::Insn> orig_body;
    orig_body.reserve(static_cast<size_t>(orig_body_len));
    for (int i = orig_bounds.prologue_end; i < orig_bounds.epilogue_begin; ++i) {
      sched_model::Insn insn = out_region.sched_block[static_cast<size_t>(i)];
      insn.luid = i - orig_bounds.prologue_end;
      orig_body.push_back(std::move(insn));
    }
    sched_model::Sched1ProjectionContext proj_ctx =
        sched_model::BuildSched1ProjectionContextForBlock(b, cfg_ctx);
    proj_ctx.allow_post_call_combine_regs_split = false;
    proj_ctx.excluded_prologue_body_indices = sched_model::FindInterleavedPrologueSaveBodyIndices(
        out_region.sched_block, orig_bounds, b, mm);
    const sched_model::Sched1ProjectionView orig_view =
        sched_model::ProjectPostReloadBlockToSched1View(orig_body, mm, live_at_end_s2, proj_ctx);
    if (orig_view.core_insns.size() <= 1 ||
        sched_model::IsAchievable(orig_view.core_insns, orig_view.facts_s1, mm,
                                  sched_model::Phase::kSched1, orig_view.live_at_end_s1)) {
      continue;
    }

    // Build trial block inserting `(set (reg:M cond_reg) (reg:M cond_reg)) (REG_DEAD cond_reg)`
    // immediately before the trailing conditional jump.
    RtlPatternAndNotes noop_pn;
    RtlExpr reg_expr;
    reg_expr.op = RtlOp::kReg;
    reg_expr.mode = cond_mode;
    reg_expr.int_val = cond_reg;
    noop_pn.pattern = RtlExpr::MakeSet(reg_expr, reg_expr);
    noop_pn.reg_notes.push_back(RtlRegNote{
        .kind = RtlMachineMode::kRegDead,
        .list_op = RtlOp::kExprList,
        .datum = reg_expr,
    });
    const std::string noop_str = FormatGccPrintRtlPatternAndNotes(
        noop_pn.pattern, noop_pn.reg_notes, noop_pn.bracketed_notes, /*call_fusage=*/{});
    const size_t jump_orig_idx = out_region.sched_orig_indices.back();
    RtlBasicBlock trial_blk = out_blk;
    if (trial_blk.rtl_insns.size() != trial_blk.instructions.size()) {
      trial_blk.SyncRtlInsnsFromInstructions();
    }
    trial_blk.instructions.insert(trial_blk.instructions.begin() + jump_orig_idx, noop_str);
    RtlInsn noop_rinsn;
    noop_rinsn.canonical_pattern = noop_str;
    noop_rinsn.is_synthetic_rtl_expansion = true;
    trial_blk.rtl_insns.insert(trial_blk.rtl_insns.begin() + jump_orig_idx, std::move(noop_rinsn));

    const sched_model::ParsedSchedRegion trial_region =
        sched_model::ParseBlockForSchedModel(trial_blk, b, sched_model::Phase::kSched2);
    const int m = static_cast<int>(trial_region.sched_block.size());
    if (!trial_region.valid || m != m_orig + 1) {
      continue;
    }
    const sched_model::PostReloadBodyBounds bounds =
        sched_model::FindPostReloadBodyBounds(trial_region.sched_block, b);
    const int prologue_end = bounds.prologue_end;
    const int epilogue_begin = bounds.epilogue_begin;
    const int body_len = epilogue_begin - prologue_end;
    if (body_len <= 2) {
      continue;
    }
    absl::btree_set<int> noop_indices;
    for (int i = 0; i + 1 < m; ++i) {
      if (is_noop_self_move_insn(trial_region.sched_block[static_cast<size_t>(i)])) {
        noop_indices.insert(i);
      }
    }
    if (m - static_cast<int>(noop_indices.size()) != n) {
      continue;
    }
    std::vector<sched_model::Insn> base_wo;
    std::vector<int> trial_to_wo(static_cast<size_t>(m), -1);
    std::vector<int> wo_to_trial;
    base_wo.reserve(static_cast<size_t>(n));
    wo_to_trial.reserve(static_cast<size_t>(n));
    for (int i = 0; i < m; ++i) {
      if (noop_indices.contains(i)) {
        continue;
      }
      trial_to_wo[static_cast<size_t>(i)] = static_cast<int>(base_wo.size());
      wo_to_trial.push_back(i);
      sched_model::Insn insn = trial_region.sched_block[static_cast<size_t>(i)];
      insn.luid = static_cast<int>(base_wo.size());
      base_wo.push_back(std::move(insn));
    }
    absl::btree_map<std::string, std::vector<int>> target_buckets;
    for (int t = 0; t < n; ++t) {
      target_buckets[BuildSchedModelPatternKey(target_region.sched_block[static_cast<size_t>(t)])]
          .push_back(t);
    }
    absl::btree_map<std::string, size_t> bucket_cursor;
    std::vector<int> wo_to_target(static_cast<size_t>(n), -1);
    std::vector<int> target_to_wo(static_cast<size_t>(n), -1);
    bool bij_ok = true;
    for (int bi = 0; bi < n; ++bi) {
      const std::string key = BuildSchedModelPatternKey(base_wo[static_cast<size_t>(bi)]);
      const auto it = target_buckets.find(key);
      if (it == target_buckets.end() || bucket_cursor[key] >= it->second.size()) {
        bij_ok = false;
        break;
      }
      const int t = it->second[bucket_cursor[key]++];
      wo_to_target[static_cast<size_t>(bi)] = t;
      target_to_wo[static_cast<size_t>(t)] = bi;
    }
    if (!bij_ok) {
      continue;
    }
    const sched_model::PostReloadBodyBounds wo_bounds =
        sched_model::FindPostReloadBodyBounds(base_wo, b);

    auto eval_and_commit = [&](const std::vector<int>& trial_order) -> bool {
      std::vector<int> pref_wo_in_target;
      pref_wo_in_target.reserve(static_cast<size_t>(n));
      // Record each stripped noop's next non-noop anchor in `trial_order`.
      absl::btree_map<int, std::vector<int>> noops_before_wo;
      std::vector<int> pending_noops;
      for (int oi : trial_order) {
        if (noop_indices.contains(oi)) {
          pending_noops.push_back(oi);
          continue;
        }
        const int wi = trial_to_wo[static_cast<size_t>(oi)];
        if (!pending_noops.empty()) {
          auto& vec = noops_before_wo[wi];
          vec.insert(vec.end(), pending_noops.begin(), pending_noops.end());
          pending_noops.clear();
        }
        pref_wo_in_target.push_back(wo_to_target[static_cast<size_t>(wi)]);
      }
      if (static_cast<int>(pref_wo_in_target.size()) != n || !pending_noops.empty()) {
        return false;
      }
      const std::optional<std::vector<int>> cand_wo_in_target = sched_model::InvertBlockOrder(
          target_region.sched_block, facts_s2, mm, sched_model::Phase::kSched2, live_at_end_s2,
          pref_wo_in_target);
      if (!cand_wo_in_target.has_value()) {
        return false;
      }
      std::vector<int> cand_wo_in_base(static_cast<size_t>(n));
      for (int pos = 0; pos < n; ++pos) {
        cand_wo_in_base[static_cast<size_t>(pos)] =
            target_to_wo[static_cast<size_t>((*cand_wo_in_target)[static_cast<size_t>(pos)])];
      }
      if (!sched_model::PreservesPostReloadOrderSafety(
              base_wo, cand_wo_in_base, wo_bounds.prologue_end, wo_bounds.epilogue_begin,
              live_at_end_s2, cfg_ctx.cfg_copy_pairs, facts_s2, mm,
              /*allow_pure_reader_reg_dead_transfer=*/false, cfg_ctx.stack_layout,
              proj_ctx.excluded_prologue_body_indices)) {
        return false;
      }
      std::vector<int> cand_in_trial;
      cand_in_trial.reserve(static_cast<size_t>(m));
      for (int wi : cand_wo_in_base) {
        if (const auto nit = noops_before_wo.find(wi); nit != noops_before_wo.end()) {
          cand_in_trial.insert(cand_in_trial.end(), nit->second.begin(), nit->second.end());
        }
        cand_in_trial.push_back(wo_to_trial[static_cast<size_t>(wi)]);
      }
      if (static_cast<int>(cand_in_trial.size()) != m) {
        return false;
      }
      std::vector<sched_model::Insn> cand_body;
      cand_body.reserve(static_cast<size_t>(body_len));
      for (int pos = prologue_end; pos < epilogue_begin; ++pos) {
        sched_model::Insn insn =
            trial_region.sched_block[static_cast<size_t>(cand_in_trial[static_cast<size_t>(pos)])];
        insn.luid = pos - prologue_end;
        cand_body.push_back(std::move(insn));
      }
      const sched_model::Sched1ProjectionView cand_view =
          sched_model::ProjectPostReloadBlockToSched1View(cand_body, mm, live_at_end_s2, proj_ctx);
      if (cand_view.core_insns.size() <= 1 ||
          !sched_model::IsAchievable(cand_view.core_insns, cand_view.facts_s1, mm,
                                     sched_model::Phase::kSched1, cand_view.live_at_end_s1)) {
        return false;
      }
      ApplyReorderedSchedRegion(&trial_blk, trial_region, cand_in_trial);
      out_blk = std::move(trial_blk);
      return true;
    };

    std::vector<sched_model::Insn> trial_body;
    trial_body.reserve(static_cast<size_t>(body_len));
    for (int i = prologue_end; i < epilogue_begin; ++i) {
      sched_model::Insn insn = trial_region.sched_block[static_cast<size_t>(i)];
      insn.luid = i - prologue_end;
      trial_body.push_back(std::move(insn));
    }
    const sched_model::Sched1ProjectionView trial_view =
        sched_model::ProjectPostReloadBlockToSched1View(trial_body, mm, live_at_end_s2, proj_ctx);
    const int num_core = static_cast<int>(trial_view.core_insns.size());
    if (num_core <= 1 ||
        num_core + static_cast<int>(trial_view.pinned_prefix_orig_indices.size()) != body_len) {
      continue;
    }
    auto lift_core_order = [&](absl::Span<const int> core_ord) -> std::vector<int> {
      std::vector<int> full;
      full.reserve(static_cast<size_t>(m));
      for (int i = 0; i < prologue_end; ++i) {
        full.push_back(i);
      }
      for (int pp_rel : trial_view.pinned_prefix_orig_indices) {
        full.push_back(prologue_end + pp_rel);
      }
      for (int ci : core_ord) {
        full.push_back(prologue_end + trial_view.core_to_orig_index[static_cast<size_t>(ci)]);
      }
      for (int i = epilogue_begin; i < m; ++i) {
        full.push_back(i);
      }
      return full;
    };
    const std::vector<int> s1_order = sched_model::ScheduleSched1ViewCoreOrder(trial_view, mm);
    if (static_cast<int>(s1_order.size()) == num_core &&
        eval_and_commit(lift_core_order(s1_order))) {
      continue;
    }
    const std::vector<std::vector<int>> tie_orders =
        sched_model::CollectSched1TieFlipCoreOrders(trial_view, mm, /*max_orders=*/24);
    for (const std::vector<int>& alt : tie_orders) {
      if (static_cast<int>(alt.size()) == num_core && eval_and_commit(lift_core_order(alt))) {
        break;
      }
    }
  }
}

// Aligns synthetic `(set (reg:DI dh) (reg:DI dh)) (REG_RETVAL)` no-op moves
// restored by `RestoreSched2PreScheduleDeletedNoopMoves` (`reload1.c`/`sched.c:4803-4821`)
// when `sched1` scheduled an independent insn between the last `REG_NO_CONFLICT`
// subword set and the `REG_RETVAL` no-op (`sched.c:schedule_block`). Moving a
// synthetic no-op self-move across independent insns that do not touch `dh` is
// invisible to `sched2`/`reload`/`local-alloc` and restores exact `kSched1` placement.
void AlignRestoredNoConflictRetvalNoopsForSched1(RtlControlFlowGraph* out,
                                                 const CompilationProfile& profile) {
  if (out == nullptr) {
    return;
  }
  auto has_synth_retval_note = [](const RtlBasicBlock& blk, size_t i) -> bool {
    if (i >= blk.rtl_insns.size() || !blk.rtl_insns[i].is_synthetic_rtl_expansion) {
      return false;
    }
    const absl::StatusOr<RtlPatternAndNotes> parsed = ParsePatternAndNotes(blk.instructions[i]);
    if (!parsed.ok()) {
      return false;
    }
    return std::any_of(
        parsed->reg_notes.begin(), parsed->reg_notes.end(),
        [](const RtlRegNote& note) { return note.kind == RtlMachineMode::kRegRetval; });
  };
  bool any_candidate = false;
  for (const RtlBasicBlock& blk : out->rpo_blocks) {
    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      if (has_synth_retval_note(blk, i)) {
        any_candidate = true;
        break;
      }
    }
    if (any_candidate) {
      break;
    }
  }
  if (!any_candidate) {
    return;
  }

  const sched_model::MachineModel mm = sched_model::MachineModel::FromProfile(profile);
  RtlControlFlowGraph live_cfg = *out;
  ReconstructRtlLiveness(
      &live_cfg, profile,
      {.reload_completed = true, .update_reg_notes = false, .preserve_phantom_pseudos = false});
  const sched_model::CfgSched1Context cfg_ctx =
      sched_model::ComputeCfgSched1Context(*out, live_cfg, mm);

  auto is_synth_retval_di_self_move = [&](const RtlBasicBlock& blk,
                                          const sched_model::ParsedSchedRegion& region,
                                          int sched_idx, int* out_dh) -> bool {
    const int orig_idx = region.sched_orig_indices[static_cast<size_t>(sched_idx)];
    if (orig_idx < 0 || static_cast<size_t>(orig_idx) >= blk.rtl_insns.size() ||
        !blk.rtl_insns[static_cast<size_t>(orig_idx)].is_synthetic_rtl_expansion) {
      return false;
    }
    const sched_model::Insn& insn = region.sched_block[static_cast<size_t>(sched_idx)];
    const RtlExpr& pat = insn.pn.pattern;
    if (insn.kind != sched_model::Insn::Kind::kInsn || !pat.IsSet() || !pat.SetDest().IsHardReg() ||
        !pat.SetSrc().IsHardReg() || pat.SetDest().mode != RtlMachineMode::kDi ||
        pat.SetSrc().mode != RtlMachineMode::kDi || pat.SetDest().RegNo() != pat.SetSrc().RegNo()) {
      return false;
    }
    const bool has_retval =
        std::any_of(insn.pn.reg_notes.begin(), insn.pn.reg_notes.end(),
                    [](const RtlRegNote& note) { return note.kind == RtlMachineMode::kRegRetval; });
    if (!has_retval) {
      return false;
    }
    *out_dh = pat.SetDest().RegNo();
    return true;
  };

  for (size_t b = 0; b < out->rpo_blocks.size(); ++b) {
    RtlBasicBlock& blk = out->rpo_blocks[b];
    bool blk_has_synth_retval = false;
    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      if (has_synth_retval_note(blk, i)) {
        blk_has_synth_retval = true;
        break;
      }
    }
    if (!blk_has_synth_retval) {
      continue;
    }
    const sched_model::ParsedSchedRegion region =
        sched_model::ParseBlockForSchedModel(blk, b, sched_model::Phase::kSched2);
    if (!region.valid || region.sched_block.size() <= 1) {
      continue;
    }
    const int n = static_cast<int>(region.sched_block.size());
    absl::btree_set<int> live_at_end_s2;
    for (int succ_id : blk.successors) {
      const auto it = live_cfg.sideband.basic_block_live_at_start.find(succ_id);
      if (it != live_cfg.sideband.basic_block_live_at_start.end()) {
        live_at_end_s2.insert(it->second.begin(), it->second.end());
      }
    }
    const sched_model::PostReloadBodyBounds bounds =
        sched_model::FindPostReloadBodyBounds(region.sched_block, b);
    const int prologue_end = bounds.prologue_end;
    const int epilogue_begin = bounds.epilogue_begin;
    const int body_len = epilogue_begin - prologue_end;
    if (body_len <= 1) {
      continue;
    }
    std::vector<sched_model::Insn> body;
    body.reserve(static_cast<size_t>(body_len));
    for (int i = prologue_end; i < epilogue_begin; ++i) {
      sched_model::Insn copy = region.sched_block[static_cast<size_t>(i)];
      copy.luid = i - prologue_end;
      body.push_back(std::move(copy));
    }
    sched_model::Sched1ProjectionContext proj_ctx =
        sched_model::BuildSched1ProjectionContextForBlock(b, cfg_ctx);
    proj_ctx.allow_post_call_combine_regs_split = false;
    proj_ctx.excluded_prologue_body_indices =
        sched_model::FindInterleavedPrologueSaveBodyIndices(region.sched_block, bounds, b, mm);
    const sched_model::Sched1ProjectionView view =
        sched_model::ProjectPostReloadBlockToSched1View(body, mm, live_at_end_s2, proj_ctx);
    const int num_core = static_cast<int>(view.core_insns.size());
    if (num_core <= 1 ||
        sched_model::IsAchievable(view.core_insns, view.facts_s1, mm, sched_model::Phase::kSched1,
                                  view.live_at_end_s1)) {
      continue;
    }
    const std::vector<int> s1_order = sched_model::ScheduleSched1ViewCoreOrder(view, mm);
    if (static_cast<int>(s1_order.size()) != num_core) {
      continue;
    }

    absl::btree_map<int, int> synth_retval_core_dh;
    for (int ci = 0; ci < num_core; ++ci) {
      const int si = prologue_end + view.core_to_orig_index[static_cast<size_t>(ci)];
      int dh = -1;
      if (is_synth_retval_di_self_move(blk, region, si, &dh)) {
        synth_retval_core_dh[ci] = dh;
      }
    }
    if (synth_retval_core_dh.empty()) {
      continue;
    }

    // Verify `s1_order` preserves every non-synthetic-`REG_RETVAL` core insn in
    // its exact original relative order and only shifts synthetic `REG_RETVAL`
    // no-op moves across independent insns.
    int expected_real = 0;
    while (expected_real < num_core && synth_retval_core_dh.contains(expected_real)) {
      ++expected_real;
    }
    bool only_synth_moved = true;
    absl::btree_map<int, std::vector<int>> insert_before_base_si;
    std::vector<int> insert_at_body_end;
    absl::btree_set<int> moved_base_sis;
    for (size_t pos = 0; pos < s1_order.size(); ++pos) {
      const int ci = s1_order[pos];
      if (!synth_retval_core_dh.contains(ci)) {
        if (ci != expected_real) {
          only_synth_moved = false;
          break;
        }
        ++expected_real;
        while (expected_real < num_core && synth_retval_core_dh.contains(expected_real)) {
          ++expected_real;
        }
        continue;
      }
      const int rv_si = prologue_end + view.core_to_orig_index[static_cast<size_t>(ci)];
      int next_non_rv_si = epilogue_begin;
      for (size_t q = pos + 1; q < s1_order.size(); ++q) {
        if (!synth_retval_core_dh.contains(s1_order[q])) {
          next_non_rv_si = prologue_end + view.core_to_orig_index[static_cast<size_t>(s1_order[q])];
          break;
        }
      }
      if (next_non_rv_si <= rv_si) {
        only_synth_moved = false;
        break;
      }
      const int dh = synth_retval_core_dh[ci];
      for (int mid = rv_si + 1; mid < next_non_rv_si; ++mid) {
        const sched_model::Insn& minsn = region.sched_block[static_cast<size_t>(mid)];
        if (minsn.kind == sched_model::Insn::Kind::kCall ||
            minsn.kind == sched_model::Insn::Kind::kJump || minsn.pn.pattern.ContainsReg(dh) ||
            minsn.pn.pattern.ContainsReg(dh + 1)) {
          only_synth_moved = false;
          break;
        }
      }
      if (!only_synth_moved) {
        break;
      }
      moved_base_sis.insert(rv_si);
      if (next_non_rv_si < epilogue_begin) {
        insert_before_base_si[next_non_rv_si].push_back(rv_si);
      } else {
        insert_at_body_end.push_back(rv_si);
      }
    }
    if (!only_synth_moved || moved_base_sis.empty()) {
      continue;
    }

    std::vector<int> cand_in_base;
    cand_in_base.reserve(static_cast<size_t>(n));
    for (int si = 0; si < n; ++si) {
      if (si == epilogue_begin) {
        for (int rv_si : insert_at_body_end) {
          cand_in_base.push_back(rv_si);
        }
      }
      if (const auto it = insert_before_base_si.find(si); it != insert_before_base_si.end()) {
        for (int rv_si : it->second) {
          cand_in_base.push_back(rv_si);
        }
      }
      if (!moved_base_sis.contains(si)) {
        cand_in_base.push_back(si);
      }
    }
    if (epilogue_begin == n) {
      for (int rv_si : insert_at_body_end) {
        cand_in_base.push_back(rv_si);
      }
    }
    if (static_cast<int>(cand_in_base.size()) != n) {
      continue;
    }
    bool changed = false;
    for (int i = 0; i < n; ++i) {
      if (cand_in_base[static_cast<size_t>(i)] != i) {
        changed = true;
        break;
      }
    }
    if (!changed) {
      continue;
    }

    std::vector<sched_model::Insn> cand_body;
    cand_body.reserve(static_cast<size_t>(body_len));
    for (int pos = prologue_end; pos < epilogue_begin; ++pos) {
      sched_model::Insn copy =
          region.sched_block[static_cast<size_t>(cand_in_base[static_cast<size_t>(pos)])];
      copy.luid = pos - prologue_end;
      cand_body.push_back(std::move(copy));
    }
    const sched_model::Sched1ProjectionView cand_view =
        sched_model::ProjectPostReloadBlockToSched1View(cand_body, mm, live_at_end_s2, proj_ctx);
    if (cand_view.core_insns.size() > 1 &&
        sched_model::IsAchievable(cand_view.core_insns, cand_view.facts_s1, mm,
                                  sched_model::Phase::kSched1, cand_view.live_at_end_s1)) {
      ApplyReorderedSchedRegion(&blk, region, cand_in_base);
    }
  }
}

}  // namespace

RtlControlFlowGraph InvertScheduleBlock(const RtlControlFlowGraph& scheduled_cfg) {
  const CompilationProfile default_profile{.optimize_level = 2, .c_flags = {"-O2"}};
  return InvertScheduleBlock(scheduled_cfg, default_profile);
}

RtlControlFlowGraph InvertScheduleBlock(const RtlControlFlowGraph& scheduled_cfg,
                                        const CompilationProfile& profile) {
  if (!ValidateScheduledCfg(scheduled_cfg)) {
    return {};
  }

  const ParsedCompilerFlags flags = ParseCompilationProfileFlags(profile);
  bool gp64 = false;
  bool fp64 = false;
  DetectMipsRegisterWidthFlags(scheduled_cfg, &profile, &gp64, &fp64);

  RtlControlFlowGraph out = scheduled_cfg;
  const bool had_empty_live_at_start = scheduled_cfg.sideband.basic_block_live_at_start.empty();
  bool has_prologue = false;
  if (scheduled_cfg.pass == RtlPassKind::kSched2) {
    if (out.sideband.basic_block_live_at_start.empty()) {
      ReconstructRtlLiveness(&out, profile, {.reload_completed = true, .update_reg_notes = false});
    }
    out.pass = RtlPassKind::kGreg;
    for (const std::string& pat_str : out.rpo_blocks[0].instructions) {
      int tsize = 0;
      auto parsed = ParsePatternAndNotes(pat_str);
      if (parsed.ok() && IsPrologueSpDecrement(parsed->pattern, &tsize)) {
        has_prologue = true;
        break;
      }
    }
    if (has_prologue) {
      for (auto& [_, live_set] : out.sideband.basic_block_live_at_start) {
        live_set.insert(kMipsSpRegno);
      }
    }
  } else if (scheduled_cfg.pass == RtlPassKind::kSched) {
    out.pass = RtlPassKind::kCombine;
  }

  if (flags.opt_level <= 0) {
    return out;
  }

  const bool ran_sched = (scheduled_cfg.pass == RtlPassKind::kSched2)  ? flags.RunsSched2()
                         : (scheduled_cfg.pass == RtlPassKind::kSched) ? flags.RunsSched1()
                                                                       : true;
  if (ran_sched) {
    if (scheduled_cfg.pass == RtlPassKind::kSched) {
      UnscheduleSched1WithModel(&out, profile, gp64, fp64);
    } else {
      std::optional<RtlControlFlowGraph> pre_unsched_cfg;
      if (scheduled_cfg.pass == RtlPassKind::kSched2) {
        RestoreInheritedSpillSlotDeadNotes(&out, gp64, fp64);
        pre_unsched_cfg = out;
      }

      for (RtlBasicBlock& blk : out.rpo_blocks) {
        UnscheduleBlockInstructions(&blk, gp64, fp64, scheduled_cfg.pass == RtlPassKind::kSched2,
                                    out.sideband.frame_pointer_needed, flags.RunsSched1());
      }

      if (scheduled_cfg.pass == RtlPassKind::kSched2) {
        UnscheduleSched2ReloadsAndCallerSaves(&out, gp64, fp64);
        RestoreNoConflictExtendClobbers(&out, gp64, fp64);
        if (flags.RunsSched1()) {
          SelectSched1AchievablePreimageForSched2(
              &out, scheduled_cfg, profile,
              pre_unsched_cfg.has_value() ? &*pre_unsched_cfg : nullptr);
        }
        // After `SelectSched1AchievablePreimageForSched2` re-adjoins reload
        // groups, restore `reload1.c:702, 2229, 4758-4767` round-robin FP spill
        // register ordering (`r2 < r1`) when verified against exact `kSched2`
        // forward replay.
        {
          const sched_model::MachineModel mm = sched_model::MachineModel::FromProfile(profile);
          const sched_model::RegFacts facts_s2 =
              sched_model::RegFacts::FromCfg(scheduled_cfg, profile, sched_model::Phase::kSched2);
          RtlControlFlowGraph live_cfg = out;
          ReconstructRtlLiveness(&live_cfg, profile,
                                 {.reload_completed = true,
                                  .update_reg_notes = false,
                                  .preserve_phantom_pseudos = false});
          absl::flat_hash_set<int> ever_used_fprs;
          std::vector<std::vector<ParsedBlockInsn>> all_parsed_blocks(out.rpo_blocks.size());
          std::vector<bool> block_parse_ok(out.rpo_blocks.size(), false);
          for (size_t b = 0; b < out.rpo_blocks.size(); ++b) {
            const RtlBasicBlock& blk = out.rpo_blocks[b];
            all_parsed_blocks[b].reserve(blk.instructions.size());
            bool ok = true;
            for (size_t i = 0; i < blk.instructions.size(); ++i) {
              auto parsed = ParsePatternAndNotes(blk.instructions[i]);
              if (!parsed.ok()) {
                ok = false;
                break;
              }
              std::optional<RtlInsn> orig_insn = (i < blk.rtl_insns.size())
                                                     ? std::optional<RtlInsn>(blk.rtl_insns[i])
                                                     : std::nullopt;
              all_parsed_blocks[b].push_back(
                  BuildParsedBlockInsn(std::move(*parsed), gp64, fp64, orig_insn));
            }
            block_parse_ok[b] = ok;
          }
          int rr_min_prologue_save_off = 0;
          absl::flat_hash_set<int64_t> rr_caller_save_offsets;
          absl::flat_hash_set<int64_t> rr_proven_spill_offsets;
          IdentifySched2CallerSaveAndSpillOffsets(
              out, all_parsed_blocks, gp64, fp64, &rr_min_prologue_save_off,
              &rr_caller_save_offsets, &rr_proven_spill_offsets);
          for (size_t b = 0; b < out.rpo_blocks.size(); ++b) {
            if (!block_parse_ok[b]) {
              continue;
            }
            RtlBasicBlock& blk = out.rpo_blocks[b];
            auto& parsed_insns = all_parsed_blocks[b];
            const sched_model::ParsedSchedRegion tgt_reg = sched_model::ParseBlockForSchedModel(
                scheduled_cfg.rpo_blocks[b], b, sched_model::Phase::kSched2);
            if (!tgt_reg.valid) {
              continue;
            }
            absl::btree_set<int> live_end;
            for (size_t succ : live_cfg.rpo_blocks[b].successors) {
              auto it = live_cfg.sideband.basic_block_live_at_start.find(succ);
              if (it != live_cfg.sideband.basic_block_live_at_start.end()) {
                live_end.insert(it->second.begin(), it->second.end());
              }
            }
            auto build_and_verify_s2 = [&](const std::vector<ParsedBlockInsn>& cand_insns,
                                           RtlBasicBlock* out_cand_blk) -> bool {
              RtlBasicBlock cand_blk = blk;
              cand_blk.instructions.clear();
              cand_blk.rtl_insns.clear();
              cand_blk.instructions.reserve(cand_insns.size());
              cand_blk.rtl_insns.reserve(cand_insns.size());
              for (const ParsedBlockInsn& pi : cand_insns) {
                cand_blk.instructions.push_back(FormatGccPrintRtlPatternAndNotes(
                    pi.parsed.pattern, pi.parsed.reg_notes, pi.parsed.bracketed_notes,
                    /*call_fusage=*/{}));
                if (pi.orig_rtl_insn.has_value()) {
                  cand_blk.rtl_insns.push_back(*pi.orig_rtl_insn);
                }
              }
              if (cand_blk.rtl_insns.size() != cand_blk.instructions.size()) {
                cand_blk.SyncRtlInsnsFromInstructions();
              }
              const sched_model::ParsedSchedRegion cand_reg =
                  sched_model::ParseBlockForSchedModel(cand_blk, b, sched_model::Phase::kSched2);
              if (!cand_reg.valid || cand_reg.sched_block.size() != tgt_reg.sched_block.size()) {
                return false;
              }
              const sched_model::DepGraph g_s2 = sched_model::Analyze(
                  cand_reg.sched_block, facts_s2, mm, sched_model::Phase::kSched2);
              const sched_model::ScheduleResult sr = sched_model::Schedule(
                  cand_reg.sched_block, g_s2, facts_s2, mm, sched_model::Phase::kSched2, live_end);
              if (sr.order.size() != tgt_reg.sched_block.size()) {
                return false;
              }
              for (size_t pos = 0; pos < sr.order.size(); ++pos) {
                const auto& a = cand_reg.sched_block[static_cast<size_t>(sr.order[pos])].pn.pattern;
                const auto& t = tgt_reg.sched_block[pos].pn.pattern;
                if (a.ToSexprString() != t.ToSexprString()) {
                  return false;
                }
              }
              if (out_cand_blk != nullptr) {
                *out_cand_blk = std::move(cand_blk);
              }
              return true;
            };
            std::vector<ParsedBlockInsn> work_insns = parsed_insns;
            if (RestoreRoundRobinFpReloadGroupOrder(gp64, fp64, &ever_used_fprs, &work_insns)) {
              RtlBasicBlock cand_blk;
              if (build_and_verify_s2(work_insns, &cand_blk)) {
                blk = std::move(cand_blk);
                parsed_insns = std::move(work_insns);
              }
            }
            if (RestoreRoundRobinTwoRegFpSpillOrder(
                    live_cfg, b, gp64, fp64, rr_proven_spill_offsets,
                    [&](const std::vector<ParsedBlockInsn>& cand) {
                      return build_and_verify_s2(cand, nullptr);
                    },
                    &parsed_insns)) {
              RtlBasicBlock cand_blk;
              if (build_and_verify_s2(parsed_insns, &cand_blk)) {
                blk = std::move(cand_blk);
              }
            }
            if (!flags.RunsSched1()) {
              const RtlBasicBlock* pre_blk =
                  (pre_unsched_cfg.has_value() &&
                   pre_unsched_cfg->rpo_blocks.size() == out.rpo_blocks.size())
                      ? &pre_unsched_cfg->rpo_blocks[b]
                      : nullptr;
              RepairSched2BlockReplayIfBroken(&blk, scheduled_cfg.rpo_blocks[b], pre_blk, b, mm,
                                              facts_s2, live_cfg);
            }
          }
        }
      }
    }
  }
  if (scheduled_cfg.pass == RtlPassKind::kSched2) {
    PreserveBlockLabelsAndBarriers(&out);
    if (ran_sched) {
      RestoreSched2DeletedNoopMoves(&out, profile);
      if (flags.RunsSched1()) {
        AlignEarlyReturnNoopMoveBlockForSched1(&out, scheduled_cfg, profile);
        RestoreAndAlignCondExprNoopMoveForSched1(&out, scheduled_cfg, profile);
      }
    }
    if (had_empty_live_at_start) {
      out.sideband.basic_block_live_at_start.clear();
    }
  }

  return out;
}

RtlControlFlowGraph InvertThreadPrologueAndEpilogueInsns(const RtlControlFlowGraph& scheduled_cfg) {
  const CompilationProfile default_profile{.optimize_level = 2, .c_flags = {"-O2"}};
  return InvertThreadPrologueAndEpilogueInsns(scheduled_cfg, default_profile);
}

RtlControlFlowGraph InvertThreadPrologueAndEpilogueInsns(const RtlControlFlowGraph& scheduled_cfg,
                                                         const CompilationProfile& profile) {
  if (!ValidateScheduledCfg(scheduled_cfg)) {
    return {};
  }

  const ParsedCompilerFlags flags = ParseCompilationProfileFlags(profile);
  bool gp64 = false;
  bool fp64 = false;
  DetectMipsRegisterWidthFlags(scheduled_cfg, &profile, &gp64, &fp64);

  RtlControlFlowGraph out = scheduled_cfg;
  if (flags.opt_level > 0 && scheduled_cfg.pass == RtlPassKind::kSched2) {
    PreserveBlockLabelsAndBarriers(&out);
  }

  RtlBasicBlock& entry = out.rpo_blocks[0];
  if (entry.instructions.empty()) {
    RecoverOutgoingArgsAndFrameSize(&out, flags, gp64, /*found_sp_decrement=*/false, /*tsize=*/0,
                                    /*saw_save_store=*/false, /*min_save_offset=*/0, {});
    return out;
  }

  const size_t n = entry.instructions.size();
  const bool had_synced_rtl_insns = (entry.rtl_insns.size() == n);
  std::vector<ParsedBlockInsn> insns;
  insns.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    auto parsed = ParsePatternAndNotes(entry.instructions[i]);
    if (!parsed.ok()) {
      return out;
    }
    std::optional<RtlInsn> orig_insn;
    if (had_synced_rtl_insns) {
      orig_insn = entry.rtl_insns[i];
    }
    insns.push_back(BuildParsedBlockInsn(std::move(*parsed), gp64, fp64, std::move(orig_insn)));
  }

  std::vector<bool> strip(n, false);

  // Helper matching a constant load into `regno` ending at `end_idx` (`< limit`),
  // supporting both single `(set (reg regno) (const_int C))` and split `lui`+`ior`.
  auto find_const_load_before = [&](size_t limit, int regno, int* out_val, size_t* out_idx1,
                                    std::optional<size_t>* out_idx0) -> bool {
    for (size_t rev = limit; rev > 0; --rev) {
      const size_t j = rev - 1;
      if (strip[j]) {
        continue;
      }
      const ParsedBlockInsn& cand = insns[j];
      if (HasOnlyFlowLivenessNotes(cand.parsed)) {
        int64_t imm = 0;
        if (MatchConstIntSetToReg(cand.parsed.pattern, regno, &imm) && imm > 0) {
          *out_val = static_cast<int>(imm);
          *out_idx1 = j;
          *out_idx0 = std::nullopt;
          return true;
        }
        int64_t lo = 0;
        if (MatchRegIorConstInt(cand.parsed.pattern, regno, &lo)) {
          for (size_t rev2 = j; rev2 > 0; --rev2) {
            const size_t k = rev2 - 1;
            if (strip[k]) {
              continue;
            }
            int64_t hi = 0;
            if (HasOnlyFlowLivenessNotes(insns[k].parsed) &&
                MatchConstIntSetToReg(insns[k].parsed.pattern, regno, &hi)) {
              const uint32_t hi_u32 = static_cast<uint32_t>(hi);
              const uint32_t lo_u32 = static_cast<uint32_t>(lo);
              if ((hi_u32 & 0x0000ffffu) == 0u && (lo_u32 & 0xffff0000u) == 0u) {
                const int32_t combined = static_cast<int32_t>(hi_u32 | lo_u32);
                if (combined > 0) {
                  *out_val = combined;
                  *out_idx1 = j;
                  *out_idx0 = k;
                  return true;
                }
              }
            }
            if (insns[k].read_regs.contains(regno) || insns[k].write_regs.contains(regno) ||
                IsCallExpr(insns[k].parsed.pattern) ||
                IsJumpOrReturnExpr(insns[k].parsed.pattern)) {
              break;
            }
          }
          return false;
        }
      }
      if (cand.read_regs.contains(regno) || cand.write_regs.contains(regno) ||
          IsCallExpr(cand.parsed.pattern) || IsJumpOrReturnExpr(cand.parsed.pattern)) {
        return false;
      }
    }
    return false;
  };

  // 1. Locate `$sp` decrement (`tsize > 0`), supporting both small immediate
  //    frames and `> 32KB` `$12` (`kMipsTemp1Regno`) frame adjustments.
  bool found_sp_decrement = false;
  bool large_reg12_defined = false;
  size_t sp_dec_idx = n;
  int tsize = 0;
  for (size_t i = 0; i < n; ++i) {
    const RtlExpr& pat = insns[i].parsed.pattern;
    if (IsCallExpr(pat) || IsJumpOrReturnExpr(pat)) {
      break;
    }
    if (HasOnlyFlowLivenessNotes(insns[i].parsed)) {
      int sp_dec = 0;
      if (IsPrologueSpDecrement(pat, &sp_dec)) {
        if (sp_dec > 0) {
          found_sp_decrement = true;
          sp_dec_idx = i;
          tsize = sp_dec;
          strip[i] = true;
          break;
        }
        int reg12_val = 0;
        size_t idx1 = 0;
        std::optional<size_t> idx0;
        if (find_const_load_before(i, kMipsTemp1Regno, &reg12_val, &idx1, &idx0)) {
          found_sp_decrement = true;
          large_reg12_defined = true;
          sp_dec_idx = i;
          tsize = reg12_val;
          if (idx0.has_value()) {
            strip[*idx0] = true;
          }
          strip[idx1] = true;
          strip[i] = true;
          break;
        }
      }
    }
    if (insns[i].write_regs.contains(kMipsSpRegno)) {
      break;
    }
  }

  // 2. Locate `< 4`-byte by-value struct/union right-shifts (`ASHIFTRT` by
  //    `24`, `16`, or `8` on `$4..$7`) emitted before `$sp` adjustment (even
  //    when `tsize == 0`).
  const size_t pre_sp_limit = found_sp_decrement ? sp_dec_idx : n;
  std::array<std::optional<MipsPrologueArgSpec>, 4> arg_shift_specs;
  bool saw_any_shift = false;
  {
    absl::flat_hash_set<int> pre_reads;
    absl::flat_hash_set<int> pre_writes;
    int last_shifted_reg = kMipsFirstArgGpr - 1;
    for (size_t i = 0; i < pre_sp_limit; ++i) {
      if (strip[i]) {
        continue;
      }
      const RtlExpr& pat = insns[i].parsed.pattern;
      if (IsCallExpr(pat) || pat.op == RtlOp::kReturn ||
          (pat.op == RtlOp::kSet && !pat.children.empty() && pat.children[0].op == RtlOp::kPc)) {
        break;
      }
      int shift_reg = -1;
      MipsPrologueArgSpec spec;
      if (HasOnlyFlowLivenessNotes(insns[i].parsed) &&
          MatchPrologueStructArgShift(pat, &shift_reg, &spec) && shift_reg > last_shifted_reg &&
          !pre_reads.contains(shift_reg) && !pre_writes.contains(shift_reg)) {
        arg_shift_specs[static_cast<size_t>(shift_reg - kMipsFirstArgGpr)] = spec;
        last_shifted_reg = shift_reg;
        saw_any_shift = true;
        strip[i] = true;
        continue;
      }
      for (int r : insns[i].read_regs) {
        pre_reads.insert(r);
      }
      for (int r : insns[i].write_regs) {
        pre_writes.insert(r);
      }
    }
  }

  // 3. Locate variadic (`...`) GP argument home stores (`$start_reg .. $7` to
  //    `0..12($sp)`) emitted before `$sp` adjustment (even when `tsize == 0`).
  bool saw_variadic_stores = false;
  int variadic_start_reg = kMipsLastArgGpr + 1;
  {
    auto mode_byte_size = [](RtlMachineMode m) -> int {
      switch (m) {
        case RtlMachineMode::kQi:
          return 1;
        case RtlMachineMode::kHi:
          return 2;
        case RtlMachineMode::kDi:
        case RtlMachineMode::kDf:
          return 8;
        default:
          return 4;
      }
    };
    auto extract_sp_disp = [](const RtlExpr& addr, int* out_disp) -> bool {
      if (addr.op == RtlOp::kReg && addr.RegNo() == kMipsSpRegno) {
        *out_disp = 0;
        return true;
      }
      if (addr.op == RtlOp::kPlus && addr.children.size() == 2 &&
          addr.children[0].op == RtlOp::kReg && addr.children[0].RegNo() == kMipsSpRegno &&
          addr.children[1].op == RtlOp::kConstInt) {
        *out_disp = static_cast<int>(addr.children[1].int_val);
        return true;
      }
      return false;
    };

    std::array<std::optional<size_t>, 4> store_idx;
    absl::flat_hash_set<int> pre_writes;
    absl::flat_hash_set<int> accessed_sp_bytes;
    bool saw_aliasing_mem_barrier = false;
    for (size_t i = 0; i < pre_sp_limit; ++i) {
      if (strip[i]) {
        continue;
      }
      const RtlExpr& pat = insns[i].parsed.pattern;
      if (IsCallExpr(pat) || IsJumpOrReturnExpr(pat) ||
          insns[i].write_regs.contains(kMipsSpRegno)) {
        break;
      }
      int store_reg = -1;
      if (!saw_aliasing_mem_barrier && HasOnlyFlowLivenessNotes(insns[i].parsed) &&
          MatchPrologueVariadicArgStore(pat, &store_reg) && !pre_writes.contains(store_reg)) {
        const RtlMachineMode store_mode = pat.children[0].mode;
        const int word_bytes = (store_mode == RtlMachineMode::kDi) ? 8 : 4;
        const int slot_offset = (store_reg - kMipsFirstArgGpr) * word_bytes;
        bool slot_already_accessed = false;
        for (int b = 0; b < word_bytes; ++b) {
          if (accessed_sp_bytes.contains(slot_offset + b)) {
            slot_already_accessed = true;
            break;
          }
        }
        const size_t slot = static_cast<size_t>(store_reg - kMipsFirstArgGpr);
        if (!slot_already_accessed && !store_idx[slot].has_value()) {
          store_idx[slot] = i;
          for (int b = 0; b < word_bytes; ++b) {
            accessed_sp_bytes.insert(slot_offset + b);
          }
          for (int r : insns[i].write_regs) {
            pre_writes.insert(r);
          }
          continue;
        }
      }

      pat.Visit([&](const RtlExpr& node) {
        if (node.op != RtlOp::kMem || node.children.empty()) {
          return;
        }
        if (node.flags.volatil) {
          saw_aliasing_mem_barrier = true;
          return;
        }
        int disp = 0;
        if (extract_sp_disp(node.children[0], &disp)) {
          const int sz = mode_byte_size(node.mode);
          for (int b = 0; b < sz; ++b) {
            accessed_sp_bytes.insert(disp + b);
          }
        }
      });
      if (insns[i].writes_mem) {
        bool is_sp_const_store = false;
        if (pat.op == RtlOp::kSet && pat.children.size() == 2 &&
            pat.children[0].op == RtlOp::kMem && !pat.children[0].children.empty() &&
            !pat.children[0].flags.volatil) {
          int disp = 0;
          is_sp_const_store = extract_sp_disp(pat.children[0].children[0], &disp);
        }
        if (!is_sp_const_store) {
          saw_aliasing_mem_barrier = true;
        }
      }
      for (int r : insns[i].write_regs) {
        pre_writes.insert(r);
      }
    }

    // In `mips_expand_prologue` (`mips.c:4949, 5005`), `variadic_start_reg` is
    // the last named GP register parameter, so `__builtin_next_arg` / `va_arg`
    // accesses start strictly above caller-home slot `S = (cand_start - 4) * word_bytes`.
    // The function body can only read caller-home slot `S` directly if the named
    // parameter's address was taken (`assign_parms`), which emits its own store
    // to caller-home slot `S` in addition to `mips_expand_prologue`'s store
    // (`total_slot_stores[slot] >= 2`). Thus a single store to slot `S` paired
    // with a direct `MEM` read overlapping caller-home slot `S` comes from
    // `assign_parms` alone rather than `mips_expand_prologue`.
    auto extract_reg_disp = [](const RtlExpr& addr, int* out_reg, int* out_disp) -> bool {
      if (addr.op == RtlOp::kReg) {
        *out_reg = addr.RegNo();
        *out_disp = 0;
        return true;
      }
      if (addr.op == RtlOp::kPlus && addr.children.size() == 2 &&
          addr.children[0].op == RtlOp::kReg && addr.children[1].op == RtlOp::kConstInt) {
        *out_reg = addr.children[0].RegNo();
        *out_disp = static_cast<int>(addr.children[1].int_val);
        return true;
      }
      return false;
    };

    bool body_has_fp_setup = out.sideband.frame_pointer_needed;
    if (found_sp_decrement && !body_has_fp_setup) {
      for (size_t i = sp_dec_idx + 1; i < n; ++i) {
        if (IsPrologueFpSetup(insns[i].parsed.pattern)) {
          body_has_fp_setup = true;
          break;
        }
      }
    }

    auto count_and_check_caller_home_slot = [&](int cand_reg, int word_bytes, int* out_store_count,
                                                bool* out_directly_read) {
      const int slot_rel = (cand_reg - kMipsFirstArgGpr) * word_bytes;
      int store_count = 0;
      bool directly_read = false;

      auto overlaps_caller_home = [&](int base_reg, int disp, int access_bytes,
                                      bool after_sp_dec) -> bool {
        int caller_base = 0;
        if (!after_sp_dec) {
          if (base_reg != kMipsSpRegno) {
            return false;
          }
          caller_base = slot_rel;
        } else {
          if (base_reg != kMipsSpRegno && (!body_has_fp_setup || base_reg != kMipsFpRegno)) {
            return false;
          }
          caller_base = tsize + slot_rel;
        }
        return disp < caller_base + word_bytes && disp + access_bytes > caller_base;
      };

      auto scan_reads = [&](auto& self, const RtlExpr& expr, bool after_sp_dec) -> void {
        if (directly_read) {
          return;
        }
        if (expr.op == RtlOp::kSet && expr.children.size() == 2 &&
            expr.children[0].op == RtlOp::kMem) {
          if (!expr.children[0].children.empty()) {
            self(self, expr.children[0].children[0], after_sp_dec);
          }
          self(self, expr.children[1], after_sp_dec);
          return;
        }
        if (expr.op == RtlOp::kMem && !expr.children.empty()) {
          int breg = -1;
          int disp = 0;
          if (extract_reg_disp(expr.children[0], &breg, &disp) &&
              overlaps_caller_home(breg, disp, mode_byte_size(expr.mode), after_sp_dec)) {
            directly_read = true;
            return;
          }
        }
        for (const RtlExpr& child : expr.children) {
          self(self, child, after_sp_dec);
        }
      };

      auto scan_stores = [&](const RtlExpr& pat, bool after_sp_dec) {
        auto check_set = [&](const RtlExpr& s) {
          if (s.op == RtlOp::kSet && s.children.size() == 2 && s.children[0].op == RtlOp::kMem &&
              !s.children[0].children.empty()) {
            int breg = -1;
            int disp = 0;
            if (extract_reg_disp(s.children[0].children[0], &breg, &disp) &&
                overlaps_caller_home(breg, disp, mode_byte_size(s.children[0].mode),
                                     after_sp_dec)) {
              ++store_count;
            }
          }
        };
        if (pat.op == RtlOp::kParallel) {
          for (const RtlExpr& elem : pat.children) {
            check_set(elem);
          }
        } else {
          check_set(pat);
        }
      };

      for (size_t b = 0; b < out.rpo_blocks.size(); ++b) {
        if (b == 0) {
          for (size_t i = 0; i < n; ++i) {
            if (found_sp_decrement && i == sp_dec_idx) {
              continue;
            }
            const bool after_sp_dec = found_sp_decrement && i > sp_dec_idx;
            scan_stores(insns[i].parsed.pattern, after_sp_dec);
            scan_reads(scan_reads, insns[i].parsed.pattern, after_sp_dec);
          }
        } else {
          for (const std::string& text : out.rpo_blocks[b].instructions) {
            if (directly_read) {
              break;
            }
            const auto parsed = ParsePatternAndNotes(text);
            if (parsed.ok()) {
              scan_reads(scan_reads, parsed->pattern, found_sp_decrement);
            }
          }
        }
      }
      *out_store_count = store_count;
      *out_directly_read = directly_read;
    };

    for (int cand_start = kMipsFirstArgGpr; cand_start <= kMipsLastArgGpr; ++cand_start) {
      bool contiguous_to_end = true;
      for (int r = cand_start; r <= kMipsLastArgGpr; ++r) {
        if (!store_idx[static_cast<size_t>(r - kMipsFirstArgGpr)].has_value()) {
          contiguous_to_end = false;
          break;
        }
      }
      if (contiguous_to_end) {
        const size_t first_idx = *store_idx[static_cast<size_t>(cand_start - kMipsFirstArgGpr)];
        const RtlMachineMode first_mode = insns[first_idx].parsed.pattern.children[0].mode;
        const int first_word_bytes = (first_mode == RtlMachineMode::kDi) ? 8 : 4;
        int slot_stores = 0;
        bool slot_directly_read = false;
        count_and_check_caller_home_slot(cand_start, first_word_bytes, &slot_stores,
                                         &slot_directly_read);
        if (slot_stores < 2 && slot_directly_read) {
          continue;
        }
        saw_variadic_stores = true;
        variadic_start_reg = cand_start;
        for (int r = cand_start; r <= kMipsLastArgGpr; ++r) {
          const size_t idx = *store_idx[static_cast<size_t>(r - kMipsFirstArgGpr)];
          strip[idx] = true;
        }
        break;
      }
    }
  }

  // 4. Locate post-`$sp` `$13` (`kMipsTemp2Regno`) large-frame base setup,
  //    callee-saved GPR/FPR save stores, and `$30` (`$fp`) frame-pointer setup.
  std::vector<int> saved_gprs = out.sideband.saved_gpr_regnos;
  std::vector<int> saved_fprs = out.sideband.saved_fpr_regnos;
  int min_save_offset = tsize;
  bool saw_save_store = false;

  if (found_sp_decrement) {
    absl::flat_hash_set<int> body_writes;
    for (size_t i = 0; i < sp_dec_idx; ++i) {
      if (!strip[i]) {
        for (int r : insns[i].write_regs) {
          body_writes.insert(r);
        }
      }
    }

    absl::flat_hash_set<int> stripped_saves;
    bool has_active_base13_offset = false;
    int active_base13_offset = 0;
    bool reg12_clobbered = body_writes.contains(kMipsTemp1Regno);

    for (size_t i = sp_dec_idx + 1; i < n; ++i) {
      if (strip[i]) {
        continue;
      }
      const RtlExpr& pat = insns[i].parsed.pattern;
      if (IsCallExpr(pat) || pat.op == RtlOp::kReturn ||
          (pat.op == RtlOp::kSet && !pat.children.empty() && pat.children[0].op == RtlOp::kPc)) {
        break;
      }

      if (HasOnlyFlowLivenessNotes(insns[i].parsed)) {
        // `$13 = $12 + $sp` (`mips.c:4639, 4750`).
        if (large_reg12_defined && !reg12_clobbered &&
            MatchPrologueRegPlusSp(pat, kMipsTemp2Regno, kMipsTemp1Regno)) {
          has_active_base13_offset = true;
          active_base13_offset = tsize;
          strip[i] = true;
          continue;
        }
        // `$13 = C; $13 = $13 + $sp` (`mips.c:4658, 4769`).
        if (MatchPrologueRegPlusSp(pat, kMipsTemp2Regno, kMipsTemp2Regno)) {
          int reg13_val = 0;
          size_t idx1 = 0;
          std::optional<size_t> idx0;
          if (find_const_load_before(i, kMipsTemp2Regno, &reg13_val, &idx1, &idx0)) {
            has_active_base13_offset = true;
            active_base13_offset = reg13_val;
            if (idx0.has_value()) {
              strip[*idx0] = true;
            }
            strip[idx1] = true;
            strip[i] = true;
            continue;
          }
        }

        int saved_reg = -1;
        int save_offset = 0;
        if (MatchPrologueSaveStoreWithBase(pat, has_active_base13_offset, active_base13_offset,
                                           tsize, &saved_reg, &save_offset) &&
            !stripped_saves.contains(saved_reg) && !body_writes.contains(saved_reg)) {
          const RtlMachineMode src_mode = pat.children[1].mode;
          const bool second_half_clobbered =
              MipsHardRegNregs(saved_reg, src_mode, gp64, fp64) == 2 &&
              body_writes.contains(saved_reg + 1);
          if (!second_half_clobbered) {
            stripped_saves.insert(saved_reg);
            saw_save_store = true;
            min_save_offset = std::min(min_save_offset, save_offset);
            if (saved_reg == kMipsRaRegno) {
              out.sideband.is_leaf_function = false;
            }
            if (saved_reg >= kMipsFirstCalleeSavedFpr && saved_reg <= kMipsLastFpr) {
              if (std::find(saved_fprs.begin(), saved_fprs.end(), saved_reg) == saved_fprs.end()) {
                saved_fprs.push_back(saved_reg);
              }
            } else {
              if (std::find(saved_gprs.begin(), saved_gprs.end(), saved_reg) == saved_gprs.end()) {
                saved_gprs.push_back(saved_reg);
              }
            }
            strip[i] = true;
            continue;
          }
        }

        if (IsPrologueFpSetup(pat) && !body_writes.contains(kMipsFpRegno)) {
          out.sideband.frame_pointer_needed = true;
          strip[i] = true;
          continue;
        }
      }

      if (insns[i].write_regs.contains(kMipsSpRegno)) {
        break;
      }
      if (insns[i].write_regs.contains(kMipsTemp1Regno)) {
        reg12_clobbered = true;
      }
      if (insns[i].write_regs.contains(kMipsTemp2Regno)) {
        has_active_base13_offset = false;
      }
      for (int r : insns[i].write_regs) {
        body_writes.insert(r);
      }
    }
  }

  RecoverOutgoingArgsAndFrameSize(&out, flags, gp64, found_sp_decrement, tsize, saw_save_store,
                                  min_save_offset, strip);

  bool any_stripped = false;
  for (bool s : strip) {
    if (s) {
      any_stripped = true;
      break;
    }
  }
  if (!any_stripped) {
    if (flags.opt_level > 0 && flags.RunsSched2() &&
        (scheduled_cfg.pass == RtlPassKind::kSched2 || scheduled_cfg.pass == RtlPassKind::kGreg)) {
      RestoreSched2PreScheduleDeletedNoopMoves(&out, profile);
      if (flags.RunsSched1()) {
        AlignRestoredNoConflictRetvalNoopsForSched1(&out, profile);
      }
    }
    return out;
  }

  // Populate `sideband.is_variadic` and `sideband.prologue_args` from recovered
  // `< 4`-byte struct shifts and variadic GP argument home stores.
  if (saw_any_shift || saw_variadic_stores) {
    const int hidden_slots = out.sideband.returns_aggregate_in_memory ? 1 : 0;
    size_t target_len = out.sideband.prologue_args.size();
    for (int slot = 0; slot < 4; ++slot) {
      if (arg_shift_specs[static_cast<size_t>(slot)].has_value() && slot >= hidden_slots) {
        target_len = std::max(target_len, static_cast<size_t>(slot - hidden_slots + 1));
      }
    }
    if (saw_variadic_stores) {
      out.sideband.is_variadic = true;
      const int var_user_slots =
          std::max(0, (variadic_start_reg - kMipsFirstArgGpr + 1) - hidden_slots);
      target_len = static_cast<size_t>(var_user_slots);
    }
    const MipsPrologueArgSpec default_word_spec{
        .kind = MipsPrologueArgKind::kGeneralWord,
        .byte_size = 4,
        .align_bits = 32,
        .gcc_mode = 0,
    };
    if (out.sideband.prologue_args.size() < target_len) {
      out.sideband.prologue_args.resize(target_len, default_word_spec);
    } else if (saw_variadic_stores && out.sideband.prologue_args.size() > target_len) {
      out.sideband.prologue_args.resize(target_len);
    }
    for (int slot = hidden_slots; slot < 4; ++slot) {
      const size_t user_idx = static_cast<size_t>(slot - hidden_slots);
      if (arg_shift_specs[static_cast<size_t>(slot)].has_value() &&
          user_idx < out.sideband.prologue_args.size()) {
        out.sideband.prologue_args[user_idx] = *arg_shift_specs[static_cast<size_t>(slot)];
      }
    }
  }

  if (found_sp_decrement) {
    std::sort(saved_gprs.begin(), saved_gprs.end());
    std::sort(saved_fprs.begin(), saved_fprs.end());
    out.sideband.saved_gpr_regnos = std::move(saved_gprs);
    out.sideband.saved_fpr_regnos = std::move(saved_fprs);
  }

  // If a body instruction hoisted ahead of a stripped prologue instruction
  // carried its last register use on the stripped prologue instruction (e.g.
  // from Stage 0 hard-register liveness), transfer that `REG_DEAD` note back to
  // the last preceding kept reader.
  std::vector<bool> mutated_notes(n, false);
  for (size_t p = 0; p < n; ++p) {
    if (!strip[p]) {
      continue;
    }
    for (const RtlRegNote& note : insns[p].parsed.reg_notes) {
      if (note.kind != RtlMachineMode::kRegDead || note.datum.op != RtlOp::kReg) {
        continue;
      }
      const int dead_reg = note.datum.RegNo();
      if (dead_reg <= 0 || dead_reg == kMipsSpRegno || dead_reg == kMipsFpRegno ||
          dead_reg == kMipsRaRegno || dead_reg == kMipsTemp1Regno || dead_reg == kMipsTemp2Regno) {
        continue;
      }
      for (size_t rev = p; rev > 0; --rev) {
        const size_t k = rev - 1;
        if (strip[k]) {
          if (insns[k].write_regs.contains(dead_reg)) {
            break;
          }
          continue;
        }
        if (insns[k].read_regs.contains(dead_reg) && !insns[k].write_regs.contains(dead_reg)) {
          if (!HasRegDeadNoteFor(insns[k], dead_reg)) {
            insns[k].parsed.reg_notes.push_back(note);
            mutated_notes[k] = true;
          }
          break;
        }
        if (insns[k].write_regs.contains(dead_reg) || IsCallExpr(insns[k].parsed.pattern)) {
          break;
        }
      }
    }
  }

  std::vector<std::string> kept_insns;
  std::vector<RtlInsn> kept_rtl_insns;
  kept_insns.reserve(n);
  if (had_synced_rtl_insns) {
    kept_rtl_insns.reserve(n);
  }
  for (size_t i = 0; i < n; ++i) {
    if (strip[i]) {
      continue;
    }
    if (mutated_notes[i] && flags.RunsSched1()) {
      CanonicalizeInsnLivenessNoteOrder(&insns[i].parsed, /*sched_note_order=*/true, gp64, fp64,
                                        /*reload_completed=*/true);
    }
    std::string pat_str =
        mutated_notes[i]
            ? FormatGccPrintRtlPatternAndNotes(insns[i].parsed.pattern, insns[i].parsed.reg_notes,
                                               insns[i].parsed.bracketed_notes, /*call_fusage=*/{})
            : entry.instructions[i];
    if (had_synced_rtl_insns) {
      RtlInsn moved = std::move(*insns[i].orig_rtl_insn);
      moved.canonical_pattern = pat_str;
      kept_rtl_insns.push_back(std::move(moved));
    }
    kept_insns.push_back(std::move(pat_str));
  }

  entry.instructions = std::move(kept_insns);
  if (had_synced_rtl_insns) {
    entry.rtl_insns = std::move(kept_rtl_insns);
  } else {
    entry.SyncRtlInsnsFromInstructions();
  }

  if (flags.opt_level > 0 && flags.RunsSched2() &&
      (scheduled_cfg.pass == RtlPassKind::kSched2 || scheduled_cfg.pass == RtlPassKind::kGreg)) {
    RestoreSched2PreScheduleDeletedNoopMoves(&out, profile);
    if (flags.RunsSched1()) {
      AlignRestoredNoConflictRetvalNoopsForSched1(&out, profile);
    }
  }

  return out;
}

}  // namespace rom_nom_nom::lifter
