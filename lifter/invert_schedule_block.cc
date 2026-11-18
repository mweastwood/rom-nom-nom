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

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/types/span.h"
#include "core/compiler_flags.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "lifter/invert_do_cross_jump.h"
#include "lifter/mips_multi_insn_idioms.h"
#include "lifter/reconstruct_hard_reg_liveness.h"
#include "lifter/rtl_liveness.h"

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
                              !((k_off + k_sz <= st_off) || (st_off + st_sz <= k_off))) {
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
                                 bool frame_pointer_needed = false) {
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
      if (bucket_offsets[bk].size() == 1 && bank_exhausted && !(sum.sz == 4 && (off % 8) == 0)) {
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

  int64_t min_caller_save_bucket = (min_prologue_save_off / 8) * 8;
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
    }
  }

  const int64_t min_spill_bucket =
      ((std::max<int64_t>(0, cfg.sideband.outgoing_args_size) + 7) / 8) * 8;
  for (int64_t bk = min_caller_save_bucket - 8; bk >= min_spill_bucket; bk -= 8) {
    if (invalidated_buckets.contains(bk)) {
      break;
    }
    const auto bit = bucket_offsets.find(bk);
    if (bit == bucket_offsets.end()) {
      if (!all_saved_gprs_used && !all_saved_fprs_used && !all_saved_gprs_16_23) {
        break;
      }
      continue;
    }
    if (bit->second.size() != 1) {
      break;
    }
    const int64_t off = bit->second[0];
    const SpOffSummary& sum = off_map.at(off);
    if (!sum.all_reg_moves || sum.n_stores < 1 || sum.n_loads < 1 || off != bk + (8 - sum.sz)) {
      break;
    }
    const bool is_fp = (sum.mode == RtlMachineMode::kSf || sum.mode == RtlMachineMode::kDf);
    const bool is_di_gpr = (!gp64 && sum.mode == RtlMachineMode::kDi);
    if (is_fp ? !all_saved_fprs_used : (is_di_gpr ? !all_saved_gprs_16_23 : !all_saved_gprs_used)) {
      break;
    }
    proven_reload_spill_offsets->insert(off);
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
            if (dying_pseudo) {
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
  if (proven_reload_spill_offsets.empty()) {
    return false;
  }
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
  for (size_t i = 0; i + 2 < insns.size(); ++i) {
    int64_t off0 = 0, off1 = 0;
    int r0 = -1, r1 = -1;
    RtlMachineMode m0 = RtlMachineMode::kNone, m1 = RtlMachineMode::kNone;
    if (!extract_spill_load(i, &off0, &r0, &m0) || !extract_spill_load(i + 1, &off1, &r1, &m1) ||
        off0 == off1) {
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
        SetsAreDisjoint(insns[i + 2].read_regs, w1) || !is_dead_after(i + 2, w0) ||
        !is_dead_after(i + 2, w1) || HasStrictInsnOrderHazard(insns[i], insns[i + 1], gp64, fp64)) {
      continue;
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
    if (!((md_r == 64 && op0 == RtlOp::kTruncate) || (md_r == 65 && op0 == RtlOp::kMult))) {
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

      // (b) Sink `caller-save` restores down immediately before their first post-call user,
      // iterating backwards so rotating an earlier restore down does not skip its immediate
      // successor, and preserving ascending `0..63` `caller-save.c` register order when
      // multiple restores share the same stopper.
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

    auto match_single_use_input_reload = [&](size_t idx, bool* is_mtc1_out, int* rel_reg_out,
                                             RtlMachineMode* rel_mode_out) {
      *is_mtc1_out = false;
      *rel_reg_out = -1;
      *rel_mode_out = RtlMachineMode::kNone;
      const RtlExpr& pat = insns[idx].parsed.pattern;
      if (pat.op == RtlOp::kSet && pat.children.size() == 2 && pat.children[0].op == RtlOp::kReg &&
          !pat.children[0].flags.integrated &&
          (pat.children[0].mode == RtlMachineMode::kSf ||
           pat.children[0].mode == RtlMachineMode::kDf) &&
          pat.children[0].RegNo() >= 32 && pat.children[0].RegNo() < 64 &&
          pat.children[1].op == RtlOp::kReg && pat.children[1].mode == pat.children[0].mode &&
          pat.children[1].RegNo() >= 2 && pat.children[1].RegNo() < 32 &&
          !use_pinned_regs.contains(pat.children[0].RegNo())) {
        *is_mtc1_out = true;
        *rel_reg_out = pat.children[0].RegNo();
        *rel_mode_out = pat.children[0].mode;
        return true;
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
      int rel_reg = -1;
      RtlMachineMode rel_mode = RtlMachineMode::kNone;
      if (!match_single_use_input_reload(i, &is_mtc1_reload, &rel_reg, &rel_mode)) {
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
      if (IsCallExpr(cpat) || IsJumpOrReturnExpr(cpat) || cpat.op == RtlOp::kUse ||
          cpat.op == RtlOp::kClobber || !is_dead_after_consumer(cons_idx, rel_words)) {
        continue;
      }
      if (is_mtc1_reload && cpat.op == RtlOp::kSet && cpat.children.size() == 2 &&
          cpat.children[1].op == RtlOp::kReg) {
        continue;
      }

      bool can_hoist_cons = !insns[cons_idx].reads_mem && !insns[cons_idx].writes_mem;
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
      while (sink_end > i + 1) {
        bool sib_mtc1 = false;
        int sib_reg = -1;
        RtlMachineMode sib_mode = RtlMachineMode::kNone;
        if (!match_single_use_input_reload(sink_end - 1, &sib_mtc1, &sib_reg, &sib_mode)) {
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

    if (!proven_reload_spill_offsets.empty()) {
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
        if (!ppat.IsSet() || ppat.SetSrc().IsReg() || IsCallExpr(ppat) ||
            IsJumpOrReturnExpr(ppat)) {
          continue;
        }

        bool can_hoist_store = true;
        for (size_t k = prod_idx + 1; k < i && can_hoist_store; ++k) {
          if (HasStrictInsnOrderHazard(insns[k], insns[i], gp64, fp64)) {
            can_hoist_store = false;
          }
        }
        if (can_hoist_store) {
          std::rotate(insns.begin() + static_cast<ptrdiff_t>(prod_idx + 1),
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
    if (scheduled_cfg.pass == RtlPassKind::kSched2) {
      RestoreInheritedSpillSlotDeadNotes(&out, gp64, fp64);
    }

    for (RtlBasicBlock& blk : out.rpo_blocks) {
      UnscheduleBlockInstructions(&blk, gp64, fp64, scheduled_cfg.pass == RtlPassKind::kSched2,
                                  out.sideband.frame_pointer_needed);
    }

    if (scheduled_cfg.pass == RtlPassKind::kSched2) {
      UnscheduleSched2ReloadsAndCallerSaves(&out, gp64, fp64);
      RestoreNoConflictExtendClobbers(&out, gp64, fp64);
    }
  }
  if (scheduled_cfg.pass == RtlPassKind::kSched2) {
    PreserveBlockLabelsAndBarriers(&out);
    if (ran_sched) {
      RestoreSched2DeletedNoopMoves(&out, profile);
    }
    if (had_empty_live_at_start) {
      out.sideband.basic_block_live_at_start.clear();
    }
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

          new_insns.push_back(FormatGccPrintRtlPatternAndNotes(
              merged.pattern, merged.reg_notes, merged.bracketed_notes, /*call_fusage=*/{}));

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
      if (IsCallExpr(pat) || pat.op == RtlOp::kReturn ||
          (pat.op == RtlOp::kSet && !pat.children.empty() && pat.children[0].op == RtlOp::kPc)) {
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

  return out;
}

}  // namespace rom_nom_nom::lifter
