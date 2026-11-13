#include "lifter/invert_schedule_block.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/types/span.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"

namespace rom_nom_nom::lifter {
namespace {

// Architectural MIPS hard register numbers used in `mips.c` prologue/epilogue
// generation (`STACK_POINTER_REGNUM`, `FRAME_POINTER_REGNUM`, `GP_REG_FIRST + 31`,
// `MIPS_TEMP1_REGNUM`).
constexpr int kMipsSpRegno = 29;
constexpr int kMipsFpRegno = 30;
constexpr int kMipsRaRegno = 31;
constexpr int kMipsTemp1Regno = 24;
constexpr int kMipsFirstCalleeSavedGpr = 16;
constexpr int kMipsLastCalleeSavedGpr = 23;
constexpr int kMipsFirstFpr = 32;
constexpr int kMipsLastFpr = 63;
constexpr int kMipsFirstCalleeSavedFpr = 52;
constexpr int kMipsHiRegno = 64;
constexpr int kMipsAccumRegno = 66;

int HardRegSpanCount(int regno, RtlMachineMode mode) {
  if (regno >= 0 && regno <= kMipsAccumRegno) {
    if (mode == RtlMachineMode::kDi || mode == RtlMachineMode::kDf) {
      return 2;
    }
    if (mode == RtlMachineMode::kTi || mode == RtlMachineMode::kTf) {
      return 4;
    }
  }
  return 1;
}

void InsertRegSpan(int regno, RtlMachineMode mode, absl::flat_hash_set<int>* set) {
  if (regno < 0) {
    return;
  }
  const int count = HardRegSpanCount(regno, mode);
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

void CollectReadRegsFromExpr(const RtlExpr& expr, absl::flat_hash_set<int>* reads) {
  if (expr.op == RtlOp::kReg) {
    InsertRegSpan(expr.RegNo(), expr.mode, reads);
    return;
  }
  if (expr.op == RtlOp::kSubreg && !expr.children.empty() && expr.children[0].op == RtlOp::kReg) {
    InsertRegSpan(expr.children[0].RegNo(), expr.mode, reads);
    return;
  }
  for (const RtlExpr& child : expr.children) {
    CollectReadRegsFromExpr(child, reads);
  }
}

void CollectReadWriteAndMemFromDestination(const RtlExpr& dst, absl::flat_hash_set<int>* reads,
                                           absl::flat_hash_set<int>* writes, bool* writes_mem,
                                           bool* has_side_effects) {
  const RtlExpr* cur = &dst;
  while (cur->op == RtlOp::kSubreg || cur->op == RtlOp::kStrictLowPart ||
         cur->op == RtlOp::kZeroExtract || cur->op == RtlOp::kSignExtract) {
    if ((cur->op == RtlOp::kZeroExtract || cur->op == RtlOp::kSignExtract) &&
        cur->children.size() >= 3) {
      CollectReadRegsFromExpr(cur->children[1], reads);
      CollectReadRegsFromExpr(cur->children[2], reads);
    }
    if (cur->children.empty()) {
      return;
    }
    cur = &cur->children[0];
  }
  if (cur->op == RtlOp::kReg) {
    InsertRegSpan(cur->RegNo(), cur->mode, writes);
  } else if (cur->op == RtlOp::kMem) {
    *writes_mem = true;
    if (cur->flags.volatil) {
      *has_side_effects = true;
    }
    if (!cur->children.empty()) {
      CollectReadRegsFromExpr(cur->children[0], reads);
    }
  } else {
    CollectReadRegsFromExpr(*cur, reads);
  }
}

void AnalyzeSourceExpr(const RtlExpr& expr, absl::flat_hash_set<int>* reads, bool* reads_mem,
                       bool* has_side_effects) {
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
    InsertRegSpan(expr.RegNo(), expr.mode, reads);
    return;
  }
  for (const RtlExpr& child : expr.children) {
    AnalyzeSourceExpr(child, reads, reads_mem, has_side_effects);
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

void AnalyzeStatementNode(const RtlExpr& stmt, ParsedBlockInsn* info) {
  if (stmt.op == RtlOp::kSet && stmt.children.size() == 2) {
    if (stmt.children[0].op == RtlOp::kPc) {
      info->has_side_effects = true;
    } else {
      CollectReadWriteAndMemFromDestination(stmt.children[0], &info->read_regs, &info->write_regs,
                                            &info->writes_mem, &info->has_side_effects);
    }
    AnalyzeSourceExpr(stmt.children[1], &info->read_regs, &info->reads_mem,
                      &info->has_side_effects);
    return;
  }
  if (stmt.op == RtlOp::kClobber && !stmt.children.empty()) {
    CollectReadWriteAndMemFromDestination(stmt.children[0], &info->read_regs, &info->write_regs,
                                          &info->writes_mem, &info->has_side_effects);
    return;
  }
  if (stmt.op == RtlOp::kUse && !stmt.children.empty()) {
    AnalyzeSourceExpr(stmt.children[0], &info->read_regs, &info->reads_mem,
                      &info->has_side_effects);
    return;
  }
  if (stmt.op == RtlOp::kParallel) {
    for (const RtlExpr& elem : stmt.children) {
      AnalyzeStatementNode(elem, info);
    }
    return;
  }
  info->has_side_effects = true;
  AnalyzeSourceExpr(stmt, &info->read_regs, &info->reads_mem, &info->has_side_effects);
}

ParsedBlockInsn BuildParsedBlockInsn(RtlPatternAndNotes parsed,
                                     std::optional<RtlInsn> orig_insn = std::nullopt) {
  ParsedBlockInsn info;
  info.parsed = std::move(parsed);
  info.orig_rtl_insn = std::move(orig_insn);
  AnalyzeStatementNode(info.parsed.pattern, &info);
  return info;
}

bool FusageListReadsReg(absl::Span<const RtlExpr> call_fusage, int regno) {
  for (const RtlExpr& usage_elem : call_fusage) {
    if (usage_elem.op == RtlOp::kUse) {
      absl::flat_hash_set<int> fusage_reads;
      CollectReadRegsFromExpr(usage_elem, &fusage_reads);
      if (fusage_reads.contains(regno)) {
        return true;
      }
    }
  }
  return false;
}

bool CallInsnReadsArgReg(const ParsedBlockInsn& call_insn, int regno) {
  if (call_insn.read_regs.contains(regno)) {
    return true;
  }
  if (call_insn.orig_rtl_insn.has_value() && call_insn.orig_rtl_insn->has_explicit_call_fusage) {
    return FusageListReadsReg(call_insn.orig_rtl_insn->call_fusage, regno);
  }
  if (call_insn.parsed.has_explicit_call_fusage) {
    return FusageListReadsReg(call_insn.parsed.call_fusage, regno);
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
bool UnscheduleHoistedFpStoresAndCallArgs(std::vector<ParsedBlockInsn>* insns_ptr) {
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
          if (!CallInsnReadsArgReg(insns[call_idx], alu_dst)) {
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
bool UnscheduleCalleeSavedFpAluSunkAcrossCall(std::vector<ParsedBlockInsn>* insns_ptr) {
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
        !CallInsnReadsArgReg(insns[idx + 2], alu_dst)) {
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

void UnscheduleBlockInstructions(RtlBasicBlock* blk) {
  const size_t n = blk->instructions.size();
  if (n < 3) {
    return;
  }

  std::vector<ParsedBlockInsn> insns;
  insns.reserve(n);
  bool has_flow_liveness_notes = false;
  for (size_t i = 0; i < n; ++i) {
    auto parsed_or = ParsePatternAndNotes(blk->instructions[i]);
    if (!parsed_or.ok()) {
      return;
    }
    for (const RtlRegNote& note : parsed_or->reg_notes) {
      if (note.kind == RtlMachineMode::kRegDead || note.kind == RtlMachineMode::kRegUnused) {
        has_flow_liveness_notes = true;
      }
    }
    std::optional<RtlInsn> orig_insn;
    if (i < blk->rtl_insns.size()) {
      orig_insn = blk->rtl_insns[i];
    }
    insns.push_back(BuildParsedBlockInsn(std::move(*parsed_or), std::move(orig_insn)));
  }
  if (!has_flow_liveness_notes) {
    return;
  }

  bool modified = UnscheduleHoistedFpStoresAndCallArgs(&insns);
  if (UnscheduleCalleeSavedFpAluSunkAcrossCall(&insns)) {
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
  if (dst.op != RtlOp::kReg || dst.RegNo() != kMipsSpRegno || src.op != RtlOp::kMinus ||
      src.children.size() != 2 || src.children[0].op != RtlOp::kReg ||
      src.children[0].RegNo() != kMipsSpRegno) {
    return false;
  }
  if (src.children[1].op == RtlOp::kConstInt && src.children[1].int_val > 0) {
    *out_tsize = static_cast<int>(src.children[1].int_val);
    return true;
  }
  return src.children[1].op == RtlOp::kReg && src.children[1].RegNo() == kMipsTemp1Regno;
}

bool IsPrologueSaveStore(const RtlExpr& pat, int* out_saved_regno, int* out_offset) {
  if (pat.op != RtlOp::kSet || pat.children.size() != 2) {
    return false;
  }
  const RtlExpr& dst = pat.children[0];
  const RtlExpr& src = pat.children[1];
  if (dst.op != RtlOp::kMem || dst.children.size() != 1 || src.op != RtlOp::kReg) {
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
  if (addr.op == RtlOp::kReg && addr.RegNo() == kMipsSpRegno) {
    *out_saved_regno = regno;
    *out_offset = 0;
    return true;
  }
  if (addr.op == RtlOp::kPlus && addr.children.size() == 2 && addr.children[0].op == RtlOp::kReg &&
      (addr.children[0].RegNo() == kMipsSpRegno || addr.children[0].RegNo() == kMipsTemp1Regno) &&
      addr.children[1].op == RtlOp::kConstInt && addr.children[1].int_val >= 0) {
    *out_saved_regno = regno;
    *out_offset = static_cast<int>(addr.children[1].int_val);
    return true;
  }
  return false;
}

bool IsPrologueFpSetup(const RtlExpr& pat) {
  return pat.op == RtlOp::kSet && pat.children.size() == 2 && pat.children[0].op == RtlOp::kReg &&
         pat.children[0].RegNo() == kMipsFpRegno && pat.children[1].op == RtlOp::kReg &&
         pat.children[1].RegNo() == kMipsSpRegno;
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

}  // namespace

RtlControlFlowGraph InvertScheduleBlock(const RtlControlFlowGraph& scheduled_cfg) {
  if (!ValidateScheduledCfg(scheduled_cfg)) {
    return {};
  }

  // At `-O0` (`toplev.c:3025-3044`), `stupid_life_analysis` (`stupid.c:186-187`)
  // initializes `reg_n_deaths[0] = 1` while leaving `basic_block_live_at_start`
  // empty, and neither `schedule_insns` nor `find_basic_blocks` executes.
  const bool is_stupid_alloc = scheduled_cfg.sideband.reg_n_deaths.contains(0) &&
                               scheduled_cfg.sideband.basic_block_live_at_start.empty();

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

  if (is_stupid_alloc) {
    return out;
  }

  for (RtlBasicBlock& blk : out.rpo_blocks) {
    UnscheduleBlockInstructions(&blk);
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
  if (!ValidateScheduledCfg(scheduled_cfg)) {
    return {};
  }

  RtlControlFlowGraph out = scheduled_cfg;
  RtlBasicBlock& entry = out.rpo_blocks[0];
  if (entry.instructions.empty()) {
    return out;
  }

  int tsize = 0;
  bool found_sp_decrement = false;
  for (const std::string& pat_str : entry.instructions) {
    auto parsed = ParsePatternAndNotes(pat_str);
    if (parsed.ok() && IsPrologueSpDecrement(parsed->pattern, &tsize)) {
      found_sp_decrement = true;
      break;
    }
  }
  if (!found_sp_decrement) {
    return out;
  }

  std::vector<int> saved_gprs = out.sideband.saved_gpr_regnos;
  std::vector<int> saved_fprs = out.sideband.saved_fpr_regnos;
  int min_save_offset = tsize;
  bool saw_save_store = false;

  std::vector<std::string> kept_insns;
  kept_insns.reserve(entry.instructions.size());

  bool in_prologue_prefix = true;
  for (size_t i = 0; i < entry.instructions.size(); ++i) {
    auto parsed = ParsePatternAndNotes(entry.instructions[i]);
    if (in_prologue_prefix && parsed.ok() && parsed->reg_notes.empty()) {
      int sp_dec = 0;
      int saved_reg = -1;
      int save_offset = 0;
      if (IsPrologueSpDecrement(parsed->pattern, &sp_dec)) {
        if (sp_dec > 0) {
          tsize = sp_dec;
        }
        continue;
      }
      if (IsPrologueSaveStore(parsed->pattern, &saved_reg, &save_offset)) {
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
        continue;
      }
      if (IsPrologueFpSetup(parsed->pattern)) {
        out.sideband.frame_pointer_needed = true;
        continue;
      }
    }
    in_prologue_prefix = false;
    kept_insns.push_back(entry.instructions[i]);
  }

  std::sort(saved_gprs.begin(), saved_gprs.end());
  std::sort(saved_fprs.begin(), saved_fprs.end());
  out.sideband.saved_gpr_regnos = std::move(saved_gprs);
  out.sideband.saved_fpr_regnos = std::move(saved_fprs);

  if (out.sideband.frame_size_bytes == 0 && tsize > 0) {
    const int args_size = out.sideband.outgoing_args_size;
    if (saw_save_store && min_save_offset >= args_size) {
      out.sideband.frame_size_bytes = min_save_offset - args_size;
    } else if (!saw_save_store && tsize >= args_size) {
      out.sideband.frame_size_bytes = tsize - args_size;
    }
  }

  if (!kept_insns.empty()) {
    entry.instructions = std::move(kept_insns);
    entry.SyncRtlInsnsFromInstructions();
  }

  return out;
}

}  // namespace rom_nom_nom::lifter
