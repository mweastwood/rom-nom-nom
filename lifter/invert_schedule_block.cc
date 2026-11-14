#include "lifter/invert_schedule_block.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/types/span.h"
#include "core/compiler_flags.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "lifter/reconstruct_hard_reg_liveness.h"

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
      if (op != RtlOp::kUse && op != RtlOp::kReturn &&
          !IsReturnInternalExpr(later.parsed.pattern) &&
          !(op == RtlOp::kSet && !later.parsed.pattern.children.empty() &&
            later.parsed.pattern.children[0].op == RtlOp::kPc)) {
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

void UnscheduleBlockInstructions(RtlBasicBlock* blk, bool gp64, bool fp64) {
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

  bool modified = UnscheduleHoistedFpStoresAndCallArgs(&insns, gp64, fp64);
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

    if (should_swap) {
      std::swap(insns[i], insns[i + 1]);
      modified = true;
      ++i;  // Advance past the swapped pair.
    }
  }

  if (modified) {
    bool all_have_orig_rtl = true;
    for (size_t i = 0; i < insns.size(); ++i) {
      blk->instructions[i] = insns[i].parsed.ToSexprString();
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
    }
  }
}

bool MatchSplitLargeIntPair(const RtlPatternAndNotes& first, const RtlPatternAndNotes& second,
                            RtlExpr* out_dest_reg, int64_t* out_combined_val) {
  // Match `first`: `(set (reg:SI R) (const_int HI))`
  if (first.pattern.op != RtlOp::kSet || first.pattern.children.size() != 2) {
    return false;
  }
  const RtlExpr& dst1 = first.pattern.children[0];
  const RtlExpr& src1 = first.pattern.children[1];
  if (dst1.op != RtlOp::kReg || dst1.mode != RtlMachineMode::kSi || src1.op != RtlOp::kConstInt) {
    return false;
  }

  // Match `second`: `(set (reg:SI R) (ior:SI (reg:SI R) (const_int LO)))`
  if (second.pattern.op != RtlOp::kSet || second.pattern.children.size() != 2) {
    return false;
  }
  const RtlExpr& dst2 = second.pattern.children[0];
  const RtlExpr& src2 = second.pattern.children[1];
  if (dst2.op != RtlOp::kReg || dst2.mode != RtlMachineMode::kSi || dst2.RegNo() != dst1.RegNo() ||
      src2.op != RtlOp::kIor || src2.mode != RtlMachineMode::kSi || src2.children.size() != 2) {
    return false;
  }
  if (src2.children[0].op != RtlOp::kReg || src2.children[0].RegNo() != dst1.RegNo() ||
      src2.children[1].op != RtlOp::kConstInt) {
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

  *out_dest_reg = dst2;
  *out_combined_val = combined;
  return true;
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

bool MatchPrologueSaveStoreWithBase(const RtlExpr& pat, std::optional<int> active_base13_offset,
                                    int tsize, int* out_saved_regno, int* out_save_offset) {
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
    if (addr.RegNo() == kMipsTemp2Regno && active_base13_offset.has_value() &&
        *active_base13_offset >= 0 && *active_base13_offset < tsize) {
      *out_saved_regno = regno;
      *out_save_offset = *active_base13_offset;
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
    if (base_reg == kMipsTemp2Regno && active_base13_offset.has_value()) {
      const int eff_offset = *active_base13_offset + disp;
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

bool ExtractNonStackBaseDispMem(const RtlExpr& mem, int* out_base, int64_t* out_off,
                                int64_t* out_size) {
  if (mem.op != RtlOp::kMem || mem.children.size() != 1) {
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
  if (base < 1 || base >= 32 || base == kMipsSpRegno || base == kMipsFpRegno) {
    return false;
  }
  *out_base = base;
  *out_off = off;
  *out_size = sz;
  return true;
}

bool ByteRangesDisjoint(int64_t off1, int64_t sz1, int64_t off2, int64_t sz2) {
  return (off1 + sz1 <= off2) || (off2 + sz2 <= off1);
}

bool MarkVolatileMemsOnBaseInExpr(RtlExpr* expr, const absl::flat_hash_set<int>& volatile_bases) {
  bool changed = false;
  int base = -1;
  int64_t off = 0;
  int64_t sz = 0;
  if (ExtractNonStackBaseDispMem(*expr, &base, &off, &sz) && volatile_bases.contains(base) &&
      !expr->flags.volatil) {
    expr->flags.volatil = true;
    changed = true;
  }
  for (RtlExpr& child : expr->children) {
    if (MarkVolatileMemsOnBaseInExpr(&child, volatile_bases)) {
      changed = true;
    }
  }
  return changed;
}

// Recovers `MEM_VOLATILE_P` (`mem/v`) on memory-mapped hardware struct pointer
// bases from post-`sched2` ordering invariants (`sched.c:true_dependence`,
// `anti_dependence`, `output_dependence`): for non-volatile `MEM` accesses on a
// fixed base register `b` at provably disjoint constant byte ranges, `sched2`
// sees zero memory dependence and hoists higher-priority downstream stores and
// multi-cycle loads ahead of a priority-1 store `insns[s]` (`[s-1]` 1-cycle ALU
// setting `r`, `[s]` store on `b`, `[s+1]` store of `r` to a disjoint field on
// `b`, `[s+2]` load into `r` from a disjoint field on `b` with a downstream
// consumer). When `insns[s]` remained ahead of `[s+1]` and `[s+2]`, `sched2`
// observed `MEM_VOLATILE_P` ordering barriers on base `b`.
void InferVolatileMemBaseRegs(RtlControlFlowGraph* cfg, bool gp64, bool fp64) {
  absl::flat_hash_set<int> written_gprs;
  std::vector<std::vector<ParsedBlockInsn>> block_insns(cfg->rpo_blocks.size());
  for (size_t b_idx = 0; b_idx < cfg->rpo_blocks.size(); ++b_idx) {
    const RtlBasicBlock& blk = cfg->rpo_blocks[b_idx];
    block_insns[b_idx].reserve(blk.instructions.size());
    for (const std::string& pat_str : blk.instructions) {
      auto parsed = ParsePatternAndNotes(pat_str);
      if (!parsed.ok()) {
        return;
      }
      ParsedBlockInsn info = BuildParsedBlockInsn(std::move(*parsed), gp64, fp64);
      for (int w : info.write_regs) {
        if (w >= 1 && w < 32) {
          written_gprs.insert(w);
        }
      }
      if (IsCallExpr(info.parsed.pattern)) {
        for (int r = 1; r <= 15; ++r) {
          written_gprs.insert(r);
        }
        written_gprs.insert(24);
        written_gprs.insert(25);
        written_gprs.insert(31);
      }
      block_insns[b_idx].push_back(std::move(info));
    }
  }

  absl::flat_hash_set<int> volatile_bases;
  for (const auto& insns : block_insns) {
    for (size_t s = 1; s + 2 < insns.size(); ++s) {
      int alu_dst = -1;
      int store_s_val = -1;
      int store_next_val = -1;
      int load_dst = -1;
      if (!IsSingleCycleIntAluInsn(insns[s - 1], &alu_dst) ||
          !IsSimpleRegMemStoreInsn(insns[s], &store_s_val) ||
          insns[s].read_regs.contains(alu_dst) ||
          !IsSimpleRegMemStoreInsn(insns[s + 1], &store_next_val) || store_next_val != alu_dst ||
          !IsSchedulableLoadInsn(insns[s + 2], &load_dst) || load_dst != alu_dst) {
        continue;
      }
      int base_s = -1, base_next = -1, base_load = -1;
      int64_t off_s = 0, sz_s = 0, off_next = 0, sz_next = 0, off_load = 0, sz_load = 0;
      if (!ExtractNonStackBaseDispMem(insns[s].parsed.pattern.children[0], &base_s, &off_s,
                                      &sz_s) ||
          !ExtractNonStackBaseDispMem(insns[s + 1].parsed.pattern.children[0], &base_next,
                                      &off_next, &sz_next) ||
          !ExtractNonStackBaseDispMem(insns[s + 2].parsed.pattern.children[1], &base_load,
                                      &off_load, &sz_load)) {
        continue;
      }
      if (base_s != base_next || base_s != base_load || written_gprs.contains(base_s)) {
        continue;
      }
      if (!ByteRangesDisjoint(off_s, sz_s, off_next, sz_next) ||
          !ByteRangesDisjoint(off_s, sz_s, off_load, sz_load) ||
          !ByteRangesDisjoint(off_next, sz_next, off_load, sz_load)) {
        continue;
      }
      bool later_writes_s_reads = false;
      bool later_consumes_load = false;
      for (size_t k = s + 1; k < insns.size(); ++k) {
        if (!SetsAreDisjoint(insns[k].write_regs, insns[s].read_regs)) {
          later_writes_s_reads = true;
          break;
        }
        if (k > s + 2 && insns[k].read_regs.contains(load_dst)) {
          later_consumes_load = true;
        }
      }
      if (!later_writes_s_reads && later_consumes_load) {
        volatile_bases.insert(base_s);
      }
    }
  }

  if (volatile_bases.empty()) {
    return;
  }

  for (RtlBasicBlock& blk : cfg->rpo_blocks) {
    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      auto parsed = ParsePatternAndNotes(blk.instructions[i]);
      if (!parsed.ok()) {
        continue;
      }
      if (MarkVolatileMemsOnBaseInExpr(&parsed->pattern, volatile_bases)) {
        blk.instructions[i] = parsed->ToGccPrintRtlString();
        if (i < blk.rtl_insns.size()) {
          blk.rtl_insns[i].canonical_pattern = blk.instructions[i];
        }
      }
    }
  }
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
      if (pat.op == RtlOp::kSet && pat.children.size() == 2 && pat.children[1].op == RtlOp::kReg &&
          ExtractStackBaseDispMem(pat.children[0], &base, &off, &sz)) {
        const int src_reg = pat.children[1].RegNo();
        if (src_reg >= 1 && src_reg < kMipsHiRegno && src_reg != kMipsSpRegno &&
            src_reg != kMipsFpRegno && src_reg != kMipsRaRegno &&
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

bool MatchGp32WordDest(const RtlExpr& dst, int expected_regno) {
  if (dst.op == RtlOp::kReg && dst.mode == RtlMachineMode::kSi && dst.RegNo() == expected_regno) {
    return true;
  }
  if (dst.op == RtlOp::kSubreg && dst.mode == RtlMachineMode::kSi && dst.children.size() == 1 &&
      dst.children[0].op == RtlOp::kReg && dst.children[0].mode == RtlMachineMode::kDi) {
    return dst.children[0].RegNo() + static_cast<int>(dst.int_val) == expected_regno;
  }
  return false;
}

bool ExprReadsExplicitDiReg(const RtlExpr& expr, int di_regno, bool in_dest = false) {
  if (expr.op == RtlOp::kSet && expr.children.size() == 2) {
    const bool dst_mem = (expr.children[0].op == RtlOp::kMem);
    if (dst_mem && ExprReadsExplicitDiReg(expr.children[0], di_regno, false)) {
      return true;
    }
    return ExprReadsExplicitDiReg(expr.children[1], di_regno, false);
  }
  if (expr.op == RtlOp::kParallel) {
    for (const RtlExpr& elem : expr.children) {
      if (ExprReadsExplicitDiReg(elem, di_regno, false)) {
        return true;
      }
    }
    return false;
  }
  if (expr.op == RtlOp::kClobber) {
    return false;
  }
  if (expr.op == RtlOp::kSubreg) {
    return false;
  }
  if (!in_dest && expr.op == RtlOp::kReg && expr.mode == RtlMachineMode::kDi &&
      expr.RegNo() == di_regno) {
    return true;
  }
  for (const RtlExpr& ch : expr.children) {
    if (ExprReadsExplicitDiReg(ch, di_regno, in_dest)) {
      return true;
    }
  }
  return false;
}

// In `!TARGET_64BIT`, `convert_move` (`expr.c:936-984`) expands 32-to-64-bit
// integer extension via `emit_no_conflict_block` (`optabs.c:2485-2525`), which
// emits a standalone `(clobber (reg:DI dH)) (insn_list:REG_LIBCALL (nil))`
// immediately ahead of the low-word copy and high-word sign/zero fill. Because
// `final_scan_insn` emits zero assembly for `CLOBBER`, this marker is absent
// when lifting from assembly, yet in `sched2` it occupies one virtual cycle and
// chains anti/output dependencies between prior readers of `dH..dH+1` and the
// subword writes.
void RestoreNoConflictExtendClobbers(RtlControlFlowGraph* cfg, bool gp64, bool fp64) {
  if (gp64) {
    return;
  }
  for (RtlBasicBlock& blk : cfg->rpo_blocks) {
    if (blk.instructions.size() < 3) {
      continue;
    }
    std::vector<ParsedBlockInsn> insns;
    insns.reserve(blk.instructions.size());
    bool parse_ok = true;
    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      auto parsed = ParsePatternAndNotes(blk.instructions[i]);
      if (!parsed.ok()) {
        parse_ok = false;
        break;
      }
      std::optional<RtlInsn> orig;
      if (blk.rtl_insns.size() == blk.instructions.size()) {
        orig = blk.rtl_insns[i];
      }
      insns.push_back(BuildParsedBlockInsn(std::move(*parsed), gp64, fp64, std::move(orig)));
    }
    if (!parse_ok) {
      continue;
    }

    std::vector<std::pair<size_t, int>> insertions;
    absl::flat_hash_set<size_t> used_indices;
    for (size_t i = 0; i < insns.size(); ++i) {
      if (used_indices.contains(i)) {
        continue;
      }
      const RtlExpr& pi = insns[i].parsed.pattern;
      if (pi.op != RtlOp::kSet || pi.children.size() != 2) {
        continue;
      }
      // Try `i` as the low-word copy `(set dst_lo (reg:SI src))` or narrow
      // signed `convert_move` `(set dst_lo (ashiftrt:SI (reg:SI src) 16|24))`
      // preceded by `(set (reg:SI src) (ashift:SI _ 16|24))`.
      int src_reg = -1;
      std::optional<size_t> narrow_sll_idx;
      if (pi.children[1].op == RtlOp::kReg && pi.children[1].mode == RtlMachineMode::kSi) {
        src_reg = pi.children[1].RegNo();
      } else if (pi.children[1].op == RtlOp::kAshiftrt &&
                 pi.children[1].mode == RtlMachineMode::kSi &&
                 pi.children[1].children.size() == 2 &&
                 pi.children[1].children[0].op == RtlOp::kReg &&
                 pi.children[1].children[0].mode == RtlMachineMode::kSi &&
                 pi.children[1].children[1].op == RtlOp::kConstInt &&
                 (pi.children[1].children[1].int_val == 16 ||
                  pi.children[1].children[1].int_val == 24)) {
        const int cand_src = pi.children[1].children[0].RegNo();
        const int64_t sh = pi.children[1].children[1].int_val;
        for (size_t rev = i; rev > 0; --rev) {
          const size_t k = rev - 1;
          if (used_indices.contains(k)) {
            break;
          }
          const RtlExpr& pk = insns[k].parsed.pattern;
          if (IsCallExpr(pk) || IsJumpOrReturnExpr(pk)) {
            break;
          }
          if (insns[k].write_regs.contains(cand_src)) {
            if (pk.op == RtlOp::kSet && pk.children.size() == 2 &&
                pk.children[0].op == RtlOp::kReg && pk.children[0].mode == RtlMachineMode::kSi &&
                pk.children[0].RegNo() == cand_src && pk.children[1].op == RtlOp::kAshift &&
                pk.children[1].mode == RtlMachineMode::kSi && pk.children[1].children.size() == 2 &&
                pk.children[1].children[0].op == RtlOp::kReg &&
                pk.children[1].children[0].mode == RtlMachineMode::kSi &&
                pk.children[1].children[1].op == RtlOp::kConstInt &&
                pk.children[1].children[1].int_val == sh) {
              src_reg = cand_src;
              narrow_sll_idx = k;
            }
            break;
          }
        }
      }
      if (src_reg < 1 || src_reg > 25) {
        continue;
      }
      int matched_dh = -1;
      size_t matched_hi_idx = 0;
      size_t matched_insert_pos = 0;
      static constexpr int kCandidateDiRegs[] = {2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 64};
      for (int dh : kCandidateDiRegs) {
        const int dl = dh + 1;
        if (src_reg == dl || !MatchGp32WordDest(pi.children[0], dl)) {
          continue;
        }
        // Search nearby for the paired high-word fill `(set dst_hi (ashiftrt src 31))` or `0`
        // (or for `dh == 64`, `(set (reg:SI 64) (reg:SI tmp))` fed by `(set tmp (ashiftrt src 31))`
        // because `mthi` requires a GPR operand).
        bool found_hi = false;
        for (size_t j = 0; j < insns.size(); ++j) {
          if (j == i || (narrow_sll_idx.has_value() && j <= *narrow_sll_idx) ||
              used_indices.contains(j)) {
            continue;
          }
          const RtlExpr& pj = insns[j].parsed.pattern;
          if (pj.op != RtlOp::kSet || pj.children.size() != 2 ||
              !MatchGp32WordDest(pj.children[0], dh)) {
            continue;
          }
          const RtlExpr& hi_src = pj.children[1];
          const bool is_zero_hi =
              !narrow_sll_idx.has_value() && (hi_src.op == RtlOp::kConstInt && hi_src.int_val == 0);
          bool is_sign_hi =
              (hi_src.op == RtlOp::kAshiftrt && hi_src.mode == RtlMachineMode::kSi &&
               hi_src.children.size() == 2 && hi_src.children[0].op == RtlOp::kReg &&
               hi_src.children[0].mode == RtlMachineMode::kSi &&
               (hi_src.children[0].RegNo() == src_reg ||
                (!narrow_sll_idx.has_value() && i < j && hi_src.children[0].RegNo() == dl)) &&
               hi_src.children[1].op == RtlOp::kConstInt && hi_src.children[1].int_val == 31);
          std::optional<size_t> hilo_sra_idx;
          if (!is_zero_hi && !is_sign_hi && dh == 64 && hi_src.op == RtlOp::kReg &&
              hi_src.mode == RtlMachineMode::kSi) {
            const int tmp_reg = hi_src.RegNo();
            for (size_t rev = j; rev > 0; --rev) {
              const size_t cand_sra = rev - 1;
              if (cand_sra == i || (narrow_sll_idx.has_value() && cand_sra <= *narrow_sll_idx)) {
                continue;
              }
              const RtlExpr& ps = insns[cand_sra].parsed.pattern;
              if (ps.op == RtlOp::kSet && ps.children.size() == 2 &&
                  ps.children[0].op == RtlOp::kReg && ps.children[0].mode == RtlMachineMode::kSi &&
                  ps.children[0].RegNo() == tmp_reg && ps.children[1].op == RtlOp::kAshiftrt &&
                  ps.children[1].mode == RtlMachineMode::kSi &&
                  ps.children[1].children.size() == 2 &&
                  ps.children[1].children[0].op == RtlOp::kReg &&
                  ps.children[1].children[0].mode == RtlMachineMode::kSi &&
                  ps.children[1].children[0].RegNo() == src_reg &&
                  ps.children[1].children[1].op == RtlOp::kConstInt &&
                  ps.children[1].children[1].int_val == 31 &&
                  !(tmp_reg == src_reg && cand_sra < i)) {
                hilo_sra_idx = cand_sra;
                is_sign_hi = true;
                break;
              }
              if (insns[cand_sra].write_regs.contains(tmp_reg) || IsCallExpr(ps) ||
                  IsJumpOrReturnExpr(ps)) {
                break;
              }
            }
          }
          if (!is_zero_hi && !is_sign_hi) {
            continue;
          }
          size_t lo_pos = std::min(i, j);
          if (hilo_sra_idx.has_value()) {
            lo_pos = std::min(lo_pos, *hilo_sra_idx);
          }
          if (narrow_sll_idx.has_value()) {
            lo_pos = std::min(lo_pos, *narrow_sll_idx);
          }
          const size_t hi_pos = std::max(i, j);
          bool span_clean = true;
          for (size_t k = lo_pos; k < hi_pos; ++k) {
            if (k == i || k == j || (hilo_sra_idx.has_value() && k == *hilo_sra_idx) ||
                (narrow_sll_idx.has_value() && k == *narrow_sll_idx)) {
              continue;
            }
            const size_t src_live_end =
                hilo_sra_idx.has_value() ? std::max(i, *hilo_sra_idx) : hi_pos;
            if (IsCallExpr(insns[k].parsed.pattern) ||
                IsJumpOrReturnExpr(insns[k].parsed.pattern) || insns[k].write_regs.contains(dh) ||
                insns[k].write_regs.contains(dl) ||
                (k < src_live_end && insns[k].write_regs.contains(src_reg)) ||
                insns[k].read_regs.contains(dh) || insns[k].read_regs.contains(dl)) {
              span_clean = false;
              break;
            }
          }
          if (!span_clean) {
            continue;
          }
          // Verify `(reg:DI dh)` is consumed in `DImode` after `hi_pos` before `dh`/`dl` is
          // overwritten.
          bool di_consumed = false;
          size_t di_consumer_idx = 0;
          for (size_t k = hi_pos + 1; k < insns.size(); ++k) {
            if (ExprReadsExplicitDiReg(insns[k].parsed.pattern, dh)) {
              di_consumed = true;
              di_consumer_idx = k;
              break;
            }
            if (insns[k].write_regs.contains(dh) || insns[k].write_regs.contains(dl) ||
                IsCallExpr(insns[k].parsed.pattern)) {
              break;
            }
          }
          if (!di_consumed) {
            continue;
          }
          // Skip if `(clobber (reg:DI dh))` is already present since the last write to `dh`/`dl`.
          bool already_has_clobber = false;
          for (size_t rev = lo_pos; rev > 0; --rev) {
            const RtlExpr& prev_pat = insns[rev - 1].parsed.pattern;
            if (prev_pat.op == RtlOp::kClobber && prev_pat.children.size() == 1 &&
                prev_pat.children[0].op == RtlOp::kReg &&
                prev_pat.children[0].mode == RtlMachineMode::kDi &&
                prev_pat.children[0].RegNo() == dh) {
              already_has_clobber = true;
              break;
            }
            if (insns[rev - 1].write_regs.contains(dh) || insns[rev - 1].write_regs.contains(dl) ||
                IsCallExpr(prev_pat)) {
              break;
            }
          }
          if (!already_has_clobber) {
            size_t insert_pos = lo_pos;
            // When `sched2`'s backward list scheduler hoisted a 3-cycle load of
            // the co-operand feeding `di_consumer_idx` ahead of the 1-cycle
            // `(lo_pos, hi_pos)` extension (leaving the zero-size `(clobber)`
            // before that load in `.10.sched2`), place `(clobber (reg:DI dh))`
            // before the hoisted co-operand load so `LUID(load) > LUID(clobber)`
            // is preserved.
            for (size_t rev = lo_pos; rev > 0; --rev) {
              const size_t cand = rev - 1;
              const RtlExpr& cpat = insns[cand].parsed.pattern;
              if (IsCallExpr(cpat) || IsJumpOrReturnExpr(cpat) ||
                  insns[cand].read_regs.contains(dh) || insns[cand].read_regs.contains(dl) ||
                  insns[cand].write_regs.contains(dh) || insns[cand].write_regs.contains(dl) ||
                  insns[cand].write_regs.contains(src_reg)) {
                break;
              }
              int saved_r = -1;
              int saved_off = 0;
              if (MatchPrologueSaveStoreWithBase(cpat, std::nullopt, 32768, &saved_r, &saved_off)) {
                continue;
              }
              if (cpat.op == RtlOp::kSet && cpat.children.size() == 2 &&
                  cpat.children[0].op == RtlOp::kReg && cpat.children[1].op == RtlOp::kMem &&
                  !cpat.children[1].flags.volatil) {
                const int load_reg = cpat.children[0].RegNo();
                const bool load_is_pair = (cpat.children[0].mode == RtlMachineMode::kDi ||
                                           cpat.children[0].mode == RtlMachineMode::kDf);
                bool fed_to_di_consumer = insns[di_consumer_idx].read_regs.contains(load_reg);
                if (fed_to_di_consumer) {
                  for (size_t m = cand + 1; m < di_consumer_idx; ++m) {
                    if (insns[m].read_regs.contains(load_reg) ||
                        insns[m].write_regs.contains(load_reg) ||
                        (load_is_pair && (insns[m].read_regs.contains(load_reg + 1) ||
                                          insns[m].write_regs.contains(load_reg + 1)))) {
                      fed_to_di_consumer = false;
                      break;
                    }
                  }
                }
                if (fed_to_di_consumer) {
                  insert_pos = cand;
                  continue;
                }
              }
              break;
            }
            matched_dh = dh;
            matched_hi_idx = j;
            matched_insert_pos = insert_pos;
            found_hi = true;
          }
          break;
        }
        if (found_hi) {
          break;
        }
      }
      if (matched_dh != -1) {
        if (matched_dh == 64 && insns[i].parsed.pattern.children[0].op == RtlOp::kReg) {
          // `reload` leaves `(subreg:SI (reg:DI 64) 1)` un-simplified on `mtlo`
          // (`n_reloads == 0`), which `sched_analyze_1` (`sched.c:1669-1704`)
          // unwraps to `(reg:DI 64)` (`HARD_REGNO_NREGS == 2`) to form the
          // 3-cycle `imuldiv` `REG_DEP_OUTPUT` edge into `mthi`.
          insns[i].parsed.pattern.children[0] = RtlExpr::MakeSubreg(
              RtlMachineMode::kSi, RtlExpr::MakeReg(RtlMachineMode::kDi, 64), 1);
          std::string updated = insns[i].parsed.ToGccPrintRtlString();
          if (blk.rtl_insns.size() == blk.instructions.size()) {
            blk.rtl_insns[i].canonical_pattern = updated;
          }
          blk.instructions[i] = std::move(updated);
        }
        used_indices.insert(i);
        used_indices.insert(matched_hi_idx);
        insertions.push_back({matched_insert_pos, matched_dh});
      }
    }

    if (insertions.empty()) {
      continue;
    }
    std::sort(insertions.begin(), insertions.end());
    const bool had_synced_rtl = (blk.rtl_insns.size() == blk.instructions.size());
    std::vector<std::string> new_instructions;
    std::vector<RtlInsn> new_rtl_insns;
    new_instructions.reserve(blk.instructions.size() + insertions.size());
    if (had_synced_rtl) {
      new_rtl_insns.reserve(blk.rtl_insns.size() + insertions.size());
    }
    size_t ins_cursor = 0;
    for (size_t idx = 0; idx < blk.instructions.size(); ++idx) {
      while (ins_cursor < insertions.size() && insertions[ins_cursor].first == idx) {
        RtlPatternAndNotes clob_pn;
        clob_pn.pattern = RtlExpr::MakeClobber(
            RtlExpr::MakeReg(RtlMachineMode::kDi, insertions[ins_cursor].second));
        clob_pn.reg_notes.push_back(RtlRegNote{
            .kind = RtlMachineMode::kRegLibcall,
            .list_op = RtlOp::kInsnList,
            .datum = RtlExpr::MakeNil(),
        });
        std::string clob_str = clob_pn.ToGccPrintRtlString();
        if (had_synced_rtl) {
          RtlInsn clob_rinsn;
          clob_rinsn.canonical_pattern = clob_str;
          new_rtl_insns.push_back(std::move(clob_rinsn));
        }
        new_instructions.push_back(std::move(clob_str));
        ++ins_cursor;
      }
      new_instructions.push_back(std::move(blk.instructions[idx]));
      if (had_synced_rtl) {
        new_rtl_insns.push_back(std::move(blk.rtl_insns[idx]));
      }
    }
    blk.instructions = std::move(new_instructions);
    if (had_synced_rtl) {
      blk.rtl_insns = std::move(new_rtl_insns);
    } else {
      blk.SyncRtlInsnsFromInstructions();
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
            !(op1.op == RtlOp::kReg && op1.RegNo() == kMipsTemp1Regno)) {
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
  if (scheduled_cfg.pass == RtlPassKind::kSched2) {
    out.pass = RtlPassKind::kGreg;
    bool has_prologue = false;
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
    if (scheduled_cfg.pass == RtlPassKind::kSched2) {
      InferVolatileMemBaseRegs(&out, gp64, fp64);
      RestoreInheritedSpillSlotDeadNotes(&out, gp64, fp64);
    }

    for (RtlBasicBlock& blk : out.rpo_blocks) {
      UnscheduleBlockInstructions(&blk, gp64, fp64);
    }

    if (scheduled_cfg.pass == RtlPassKind::kSched2) {
      RestoreNoConflictExtendClobbers(&out, gp64, fp64);
    }
  }
  if (scheduled_cfg.pass == RtlPassKind::kSched2) {
    PreserveBlockLabelsAndBarriers(&out);
  }

  return out;
}

RtlControlFlowGraph InvertUpdateFlowInfo(const RtlControlFlowGraph& scheduled_cfg) {
  if (!ValidateScheduledCfg(scheduled_cfg)) {
    return {};
  }

  RtlControlFlowGraph out = scheduled_cfg;
  for (RtlBasicBlock& blk : out.rpo_blocks) {
    if (blk.instructions.size() < 2) {
      continue;
    }
    std::vector<std::string> new_insns;
    new_insns.reserve(blk.instructions.size());

    bool modified_block = false;
    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      if (i + 1 < blk.instructions.size()) {
        auto p0 = ParsePatternAndNotes(blk.instructions[i]);
        auto p1 = ParsePatternAndNotes(blk.instructions[i + 1]);
        RtlExpr dest_reg;
        int64_t combined_val = 0;
        if (p0.ok() && p1.ok() && MatchSplitLargeIntPair(*p0, *p1, &dest_reg, &combined_val)) {
          RtlPatternAndNotes merged;
          merged.bracketed_notes = p1->bracketed_notes;
          merged.pattern = RtlExpr::MakeSet(dest_reg, RtlExpr::MakeConstInt(combined_val));

          for (const RtlRegNote& note : p0->reg_notes) {
            if (note.kind == RtlMachineMode::kRegDead && note.datum.op == RtlOp::kReg &&
                note.datum.RegNo() == dest_reg.RegNo()) {
              continue;
            }
            merged.reg_notes.push_back(note);
          }
          for (const RtlRegNote& note : p1->reg_notes) {
            merged.reg_notes.push_back(note);
          }

          new_insns.push_back(merged.ToSexprString());

          auto sets_it = out.sideband.reg_n_sets.find(dest_reg.RegNo());
          if (sets_it != out.sideband.reg_n_sets.end() && sets_it->second > 1) {
            sets_it->second -= 1;
          }

          modified_block = true;
          ++i;  // Skip `i + 1`.
          continue;
        }
      }

      new_insns.push_back(blk.instructions[i]);
    }

    if (modified_block) {
      blk.instructions = std::move(new_insns);
      blk.SyncRtlInsnsFromInstructions();
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
      if (IsCallExpr(pat) || IsJumpOrReturnExpr(pat)) {
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
  std::optional<size_t> last_stripped_prologue_store_idx;
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
          if (base_reg != kMipsSpRegno && !(body_has_fp_setup && base_reg == kMipsFpRegno)) {
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
          last_stripped_prologue_store_idx = last_stripped_prologue_store_idx.has_value()
                                                 ? std::max(*last_stripped_prologue_store_idx, idx)
                                                 : idx;
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
    std::optional<int> active_base13_offset;
    bool reg12_clobbered = body_writes.contains(kMipsTemp1Regno);

    for (size_t i = sp_dec_idx + 1; i < n; ++i) {
      if (strip[i]) {
        continue;
      }
      const RtlExpr& pat = insns[i].parsed.pattern;
      if (IsCallExpr(pat) || IsJumpOrReturnExpr(pat)) {
        break;
      }

      if (HasOnlyFlowLivenessNotes(insns[i].parsed)) {
        // `$13 = $12 + $sp` (`mips.c:4639, 4750`).
        if (large_reg12_defined && !reg12_clobbered &&
            MatchPrologueRegPlusSp(pat, kMipsTemp2Regno, kMipsTemp1Regno)) {
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
        if (MatchPrologueSaveStoreWithBase(pat, active_base13_offset, tsize, &saved_reg,
                                           &save_offset) &&
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
            last_stripped_prologue_store_idx = last_stripped_prologue_store_idx.has_value()
                                                   ? std::max(*last_stripped_prologue_store_idx, i)
                                                   : i;
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
        active_base13_offset.reset();
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
  // In `sched.c:true_dependence` (`sched.c:828-839`) + `rtlanal.c:rtx_varies_p`
  // (`rtlanal.c:101`, where `stack_pointer_rtx` has `rtx_addr_varies_p == 1`), a
  // load from a general-pointer `MEM` (`rtx_addr_varies_p == 1`, non-stack,
  // non-`LO_SUM`/constant) can only be scheduled ahead of a `$sp`-based prologue
  // store when `RTX_UNCHANGING_P(mem) == 1` (`mem/u`).
  if (last_stripped_prologue_store_idx.has_value()) {
    auto is_general_varying_ptr_addr = [&](const RtlExpr& addr) -> bool {
      if (addr.op == RtlOp::kLoSum || addr.op == RtlOp::kSymbolRef || addr.op == RtlOp::kLabelRef ||
          addr.op == RtlOp::kConst || addr.op == RtlOp::kConstInt) {
        return false;
      }
      const RtlExpr* base = &addr;
      if (addr.op == RtlOp::kPlus && addr.children.size() == 2 &&
          addr.children[1].op == RtlOp::kConstInt) {
        base = &addr.children[0];
      }
      if (base->op != RtlOp::kReg) {
        return false;
      }
      const int breg = base->RegNo();
      if (breg == kMipsSpRegno || breg == kMipsTemp2Regno ||
          (breg == kMipsFpRegno && out.sideband.frame_pointer_needed)) {
        return false;
      }
      return breg >= 1 && breg < kMipsFirstFpr;
    };
    auto mark_read_mems = [&](auto& self, RtlExpr* expr) -> bool {
      bool changed = false;
      if (expr->op == RtlOp::kMem && !expr->children.empty()) {
        if (!expr->flags.volatil && !expr->flags.unchanging &&
            is_general_varying_ptr_addr(expr->children[0])) {
          expr->flags.unchanging = true;
          changed = true;
        }
        if (self(self, &expr->children[0])) {
          changed = true;
        }
        return changed;
      }
      for (RtlExpr& ch : expr->children) {
        if (self(self, &ch)) {
          changed = true;
        }
      }
      return changed;
    };
    auto mark_stmt_reads = [&](auto& self, RtlExpr* stmt) -> bool {
      if (stmt->op == RtlOp::kSet && stmt->children.size() == 2) {
        bool changed = false;
        if (stmt->children[0].op == RtlOp::kMem && !stmt->children[0].children.empty()) {
          if (mark_read_mems(mark_read_mems, &stmt->children[0].children[0])) {
            changed = true;
          }
        }
        if (mark_read_mems(mark_read_mems, &stmt->children[1])) {
          changed = true;
        }
        return changed;
      }
      if (stmt->op == RtlOp::kParallel) {
        bool changed = false;
        for (RtlExpr& elem : stmt->children) {
          if (self(self, &elem)) {
            changed = true;
          }
        }
        return changed;
      }
      if (stmt->op == RtlOp::kClobber) {
        return false;
      }
      return mark_read_mems(mark_read_mems, stmt);
    };

    for (size_t i = 0; i < *last_stripped_prologue_store_idx; ++i) {
      if (strip[i]) {
        continue;
      }
      if (mark_stmt_reads(mark_stmt_reads, &insns[i].parsed.pattern)) {
        mutated_notes[i] = true;
      }
    }
  }
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
    std::string pat_str =
        mutated_notes[i] ? insns[i].parsed.ToSexprString() : entry.instructions[i];
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

  return out;
}

}  // namespace rom_nom_nom::lifter
