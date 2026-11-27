#include "lifter/reorg_model.h"

#include <cstddef>
#include <cstdint>
#include <string>

#include "absl/status/statusor.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "lifter/rtl_cfg_utils.h"

namespace rom_nom_nom::lifter {
namespace {

bool IsSimpleMipsMemAddr(const RtlExpr& addr) {
  if (addr.IsReg() && addr.RegNo() >= 0 && addr.RegNo() < 32) {
    return true;
  }
  if (addr.IsConstInt() && addr.IntVal() >= -32768 && addr.IntVal() <= 32767) {
    return true;
  }
  if (addr.op == RtlOp::kPlus && addr.children.size() == 2 && addr.children[0].IsReg() &&
      addr.children[0].RegNo() >= 0 && addr.children[0].RegNo() < 32 &&
      addr.children[1].IsConstInt() && addr.children[1].IntVal() >= -32768 &&
      addr.children[1].IntVal() <= 32767) {
    return true;
  }
  return addr.op == RtlOp::kLoSum && addr.children.size() == 2 && addr.children[0].IsReg() &&
         addr.children[0].RegNo() >= 0 && addr.children[0].RegNo() < 32;
}

bool IsNoteWithKind(const RtlExpr& pat, std::string_view kind) {
  if (pat.op != RtlOp::kNote) {
    return false;
  }
  for (const auto& item : pat.generic_items) {
    if (item.text == kind) {
      return true;
    }
  }
  return pat.str_val == kind;
}

bool IsConstantExprForMayTrap(const RtlExpr& expr) {
  switch (expr.op) {
    case RtlOp::kConstInt:
    case RtlOp::kConstDouble:
    case RtlOp::kSymbolRef:
    case RtlOp::kLabelRef:
    case RtlOp::kConst:
      return true;
    default:
      return false;
  }
}

// Mirrors `reorg.c:1287-1325` (`rare_destination`): returns `0` immediately on
// any `CODE_LABEL` (`reorg.c:1302`), `1` for `RETURN` / end-of-function
// (`reorg.c:1311-1312, 1324`), `2` for a post-call `BARRIER` (`reorg.c:1304-1309`),
// and follows up to 10 `simplejump_p` hops (`reorg.c:1313-1315`).
int RareDestinationScore(const RtlControlFlowGraph& cfg, size_t start_b) {
  const size_t num_blocks = cfg.rpo_blocks.size();
  if (start_b >= num_blocks) {
    return 1;
  }
  size_t cur_b = start_b;
  int jump_count = 0;
  while (cur_b < num_blocks) {
    const RtlBasicBlock& blk = cfg.rpo_blocks[cur_b];
    if (blk.has_label) {
      // `reorg.c:1302`: encountering a `CODE_LABEL` returns 0.
      return 0;
    }

    bool jumped = false;
    for (const std::string& insn_str : blk.instructions) {
      auto parsed = ParsePatternAndNotes(insn_str);
      if (!parsed.ok()) {
        return 0;
      }
      const RtlExpr* pat = &parsed->pattern;
      if (pat->op == RtlOp::kSequence && !pat->children.empty() &&
          !pat->children[0].children.empty()) {
        pat = &pat->children[0].children[0];
      }
      if (pat->op == RtlOp::kBarrier) {
        return 2;
      }
      // `reorg.c:1310-1318`: only a bare `RETURN` pattern returns 1; MIPS
      // `return_internal` (`config/mips/mips.md:5934-5941`) is a `PARALLEL`
      // where `GET_CODE(PATTERN(insn)) != RETURN` and `simplejump_p == 0`, so
      // it takes the `else` branch below (`IsAnyJumpOrReturnTopExpr`) -> 0.
      if (pat->op == RtlOp::kReturn) {
        return 1;
      }
      if (IsSimpleUncondJumpExpr(*pat)) {
        if (jump_count++ < 10 && !blk.successors.empty() && blk.successors[0] < num_blocks) {
          cur_b = blk.successors[0];
          jumped = true;
          break;
        }
        return 0;
      }
      if (IsAnyJumpOrReturnTopExpr(*pat)) {
        return 0;
      }
    }
    if (jumped) {
      continue;
    }
    // Check for a post-call noreturn barrier (`has_trailing_barrier` without jump/return).
    if (blk.has_trailing_barrier && blk.successors.empty()) {
      return 2;
    }
    if (blk.successors.empty()) {
      return 1;
    }
    if (blk.successors.size() == 1 && blk.successors[0] == cur_b + 1) {
      ++cur_b;
      continue;
    }
    return 0;
  }
  return 1;
}

}  // namespace

bool IsEligibleForMipsDelaySlot(const RtlExpr& pat, const MipsDelaySlotOptions& options) {
  // `config/mips/mips.md:125-138`:
  //   - `ANNUL_IFTRUE` (slot 1) is `(nil)` for branch, jump, and call.
  //   - `ANNUL_IFFALSE` (slot 2) is `(branch_likely == yes && dslot == no && length == 1)`
  //     for branches only (`(nil)` for jump and call).
  //   - Call delay slots require `abicalls == "no"` (`mips.md:135`).
  if (options.annul_mode == MipsDelaySlotAnnulMode::kAnnulTrue) {
    return false;
  }
  if (options.annul_mode == MipsDelaySlotAnnulMode::kAnnulFalse &&
      (options.kind != MipsDelaySlotKind::kBranch || options.mips_isa < 2)) {
    return false;
  }
  if (options.kind == MipsDelaySlotKind::kCall && options.abicalls) {
    return false;
  }

  // `config/mips/mips.md:79-82, 125-138`: only single-instruction `SET` patterns
  // with `dslot == "no"` and `length == 1` are eligible. Reject `PARALLEL`,
  // `SEQUENCE`, `CALL`, jumps/returns, `USE`/`CLOBBER`/`NOTE`, unaligned/asm/unspec.
  if (!pat.IsSet() || IsAnyJumpOrReturnTopExpr(pat)) {
    return false;
  }
  const RtlExpr& dst = pat.SetDest();
  const RtlExpr& src = pat.SetSrc();
  if (src.op == RtlOp::kCall || ExprHasMem(src)) {
    return false;
  }

  // Reject HI/LO/HILO/FPSW (`>= 64`, `type = hilo/fcmp` -> `dslot = yes`),
  // `unspec`/`unspec_volatile`/unaligned/asm nodes anywhere in the pattern.
  bool has_ineligible_node = false;
  pat.Visit([&has_ineligible_node](const RtlExpr& node) {
    if (has_ineligible_node) {
      return;
    }
    if ((node.IsReg() && node.RegNo() >= 64) || node.op == RtlOp::kUnspec ||
        node.op == RtlOp::kUnspecVolatile || node.op == RtlOp::kUnalignedLoad ||
        node.op == RtlOp::kUnalignedStore || node.op == RtlOp::kAsmInput ||
        node.op == RtlOp::kAsmOperands) {
      has_ineligible_node = true;
    }
  });
  if (has_ineligible_node) {
    return false;
  }

  // `config/mips/mips.md:2485-2660` (`fix_trunc*` / `floatsi*` / `floatdi*`)
  // emit multi-instruction templates (`length >= 2`).
  if (src.op == RtlOp::kFloat || src.op == RtlOp::kUnsignedFloat || src.op == RtlOp::kFix ||
      src.op == RtlOp::kUnsignedFix || src.op == RtlOp::kConstDouble) {
    return false;
  }

  // `config/mips/mips.md:3037-3048` (`movdi_internal`, `!TARGET_64BIT`):
  // all 32-bit `DImode` operations/moves have `length >= 2`.
  if (!options.gp64 && (dst.mode == RtlMachineMode::kDi || src.mode == RtlMachineMode::kDi)) {
    return false;
  }

  if (dst.IsMem()) {
    const bool mode_ok = dst.mode == RtlMachineMode::kQi || dst.mode == RtlMachineMode::kHi ||
                         dst.mode == RtlMachineMode::kSi || dst.mode == RtlMachineMode::kSf ||
                         (options.gp64 && dst.mode == RtlMachineMode::kDi) ||
                         (options.fp64 && dst.mode == RtlMachineMode::kDf);
    if (!mode_ok || dst.children.size() != 1) {
      return false;
    }
    const int src_reg = TrueRegnum(src);
    const bool valid_store_src =
        (src_reg >= 0 && src_reg < 64) || (src.IsConstInt() && src.IntVal() == 0);
    if (!valid_store_src) {
      return false;
    }
    const RtlExpr& addr = dst.children[0];
    if (IsSimpleMipsMemAddr(addr)) {
      return true;
    }
    if (options.allow_sdata_symbolic_store &&
        (addr.op == RtlOp::kSymbolRef || addr.op == RtlOp::kConst)) {
      return true;
    }
    return false;
  }

  const int dst_reg = TrueRegnum(dst);
  if (dst_reg < 0 || dst_reg >= 64) {
    return false;
  }

  if (dst_reg >= 32) {
    // FPR destination: reject GPR-to-FPR `xfer` (`dslot == "yes"`, `mips.md:79-82`)
    // and `const_double`/`fix` nodes.
    bool touches_gpr_or_multi = false;
    src.Visit([&touches_gpr_or_multi](const RtlExpr& node) {
      if ((node.IsReg() && node.RegNo() >= 0 && node.RegNo() < 32) ||
          node.op == RtlOp::kConstDouble || node.op == RtlOp::kFix ||
          node.op == RtlOp::kUnsignedFix) {
        touches_gpr_or_multi = true;
      }
    });
    return !touches_gpr_or_multi;
  }

  // GPR destination (`0..31`).
  const bool gpr_mode_ok = dst.mode == RtlMachineMode::kQi || dst.mode == RtlMachineMode::kHi ||
                           dst.mode == RtlMachineMode::kSi || dst.mode == RtlMachineMode::kSf ||
                           (options.gp64 && dst.mode == RtlMachineMode::kDi) ||
                           (options.gp64 && options.fp64 && dst.mode == RtlMachineMode::kDf);
  if (!gpr_mode_ok) {
    return false;
  }

  bool touches_fpr = false;
  src.Visit([&touches_fpr](const RtlExpr& node) {
    if (node.IsReg() && node.RegNo() >= 32 && node.RegNo() < 64) {
      touches_fpr = true;
    }
  });
  if (touches_fpr || src.op == RtlOp::kMult || src.op == RtlOp::kDiv || src.op == RtlOp::kMod ||
      src.op == RtlOp::kUdiv || src.op == RtlOp::kUmod) {
    return false;
  }

  // `high` (`lui`) and `lo_sum` (`addiu`) are 1-instruction `arith` ops.
  if (src.op == RtlOp::kHigh || src.op == RtlOp::kLoSum) {
    return true;
  }

  // `config/mips/mips.md:3290-3300` (`movsi_internal2`): `symbol_ref`/`label_ref`/
  // `const` have `type = "load"` or `length = 2`.
  bool has_sym_or_label = false;
  src.Visit([&has_sym_or_label](const RtlExpr& node) {
    if (node.op == RtlOp::kSymbolRef || node.op == RtlOp::kLabelRef || node.op == RtlOp::kConst ||
        node.op == RtlOp::kConstDouble) {
      has_sym_or_label = true;
    }
  });
  if (has_sym_or_label) {
    return false;
  }

  if (src.IsConstInt()) {
    const int64_t v = src.IntVal();
    return (v >= -32768 && v <= 65535) ||
           ((v & 0xffff) == 0 && v >= -2147483648LL && v <= 2147483647LL);
  }

  if (src.children.size() == 2 && (src.children[0].IsConstInt() || src.children[1].IsConstInt())) {
    if (src.children[0].IsConstInt()) {
      return false;
    }
    const int64_t v = src.children[1].IntVal();
    switch (src.op) {
      case RtlOp::kAnd:
      case RtlOp::kIor:
      case RtlOp::kXor:
        return v >= 0 && v <= 65535;
      case RtlOp::kPlus:
      case RtlOp::kLt:
      case RtlOp::kLtu:
        return v >= -32768 && v <= 32767;
      case RtlOp::kLe:
        // `config/mips/mips.md:5234-5246` (`sle_si_const`): `slti %0,%1,c+1` (`length = 1`).
        return v >= -32768 && v < 32767;
      case RtlOp::kMinus:
        return v >= -32767 && v <= 32768;
      case RtlOp::kAshift:
      case RtlOp::kAshiftrt:
      case RtlOp::kLshiftrt:
        return v >= 0 && v <= (options.gp64 && dst.mode == RtlMachineMode::kDi ? 63 : 31);
      case RtlOp::kEq:
      case RtlOp::kNe:
      case RtlOp::kGt:
      case RtlOp::kGtu:
        return v == 0;
      default:
        return false;
    }
  }

  // `config/mips/mips.md:5119-5385` (`sge_si`/`sle_si_reg`/`sgeu_si`): `length = 2`.
  return src.op != RtlOp::kLe && src.op != RtlOp::kGe && src.op != RtlOp::kGeu;
}

namespace {

// Mirrors `reorg.c:1344-1351` (`LABEL_OUTSIDE_LOOP_P` at `rtl.h:545`, set by
// `loop.c:2563` and printed as `(label_ref/s ...)` at `print-rtl.c:82-83`):
// branches exiting a loop are marked highly unlikely (`-1`) before all other
// `mostly_true_jump` heuristics.
bool IsOutsideLoopBranch(const RtlExpr& jump_pat) {
  if (!jump_pat.IsSet() || jump_pat.SetSrc().op != RtlOp::kIfThenElse ||
      jump_pat.SetSrc().children.size() != 3) {
    return false;
  }
  const RtlExpr& arm1 = jump_pat.SetSrc().children[1];
  const RtlExpr& arm2 = jump_pat.SetSrc().children[2];
  return (arm1.op == RtlOp::kLabelRef && arm1.flags.in_struct) ||
         (arm2.op == RtlOp::kLabelRef && arm2.flags.in_struct);
}

}  // namespace

int MostlyTrueJumpScore(const RtlExpr& jump_pat, size_t block_b, size_t target_b) {
  if (IsOutsideLoopBranch(jump_pat)) {
    return -1;
  }
  if (IsSimpleUncondJumpExpr(jump_pat) || jump_pat.op == RtlOp::kReturn) {
    // `reorg.c:1403-1405, 1444-1448` + `jump.c:2983` (`condjump_in_parallel_p`
    // requires element 0 to be `SET`, so MIPS `return_internal` `PARALLEL` is
    // not unwrapped to `RETURN` and returns 0; only bare `RETURN` or
    // `simplejump_p` produces `const_true_rtx` -> 1).
    return 1;
  }
  const RtlExpr* ite = GetCondJumpIfThenElse(jump_pat);
  if (ite == nullptr || !ite->children[0].IsComparison()) {
    return 0;
  }
  const RtlExpr& cond = ite->children[0];
  RtlOp code = cond.op;
  // `reorg.c:1464-1471` (`get_branch_condition`): invert when arm 1 is `(pc)`.
  if (ite->children[1].op == RtlOp::kPc) {
    code = ReverseComparisonOp(code);
  }
  // `reorg.c:1401-1420`: `EQ` -> 0, `NE` -> 1, `LE`/`LT` vs 0 -> 0, `GE`/`GT` vs 0 -> 1.
  switch (code) {
    case RtlOp::kEq:
      return 0;
    case RtlOp::kNe:
      return 1;
    case RtlOp::kLe:
    case RtlOp::kLt:
      if (cond.children.size() == 2 && cond.children[1].IsConstInt() &&
          cond.children[1].IntVal() == 0) {
        return 0;
      }
      break;
    case RtlOp::kGe:
    case RtlOp::kGt:
      if (cond.children.size() == 2 && cond.children[1].IsConstInt() &&
          cond.children[1].IntVal() == 0) {
        return 1;
      }
      break;
    default:
      break;
  }
  // `reorg.c:1425-1428`: backward branches predict taken (1), forward not taken (0).
  return (target_b <= block_b) ? 1 : 0;
}

int MostlyTrueJumpScore(const RtlControlFlowGraph& cfg, size_t block_b, const RtlExpr& jump_pat,
                        size_t target_b) {
  if (IsOutsideLoopBranch(jump_pat)) {
    return -1;
  }
  const size_t num_blocks = cfg.rpo_blocks.size();
  if (target_b < num_blocks) {
    // `reorg.c:1359-1363`: backward note scan before `target_label` for
    // `NOTE_INSN_LOOP_BEG` -> returns 2.
    if (target_b > 0) {
      const auto& prev_insns = cfg.rpo_blocks[target_b - 1].instructions;
      for (size_t i = prev_insns.size(); i-- > 0;) {
        auto parsed = ParsePatternAndNotes(prev_insns[i]);
        if (!parsed.ok() || parsed->pattern.op != RtlOp::kNote) {
          break;
        }
        if (IsNoteWithKind(parsed->pattern, "NOTE_INSN_LOOP_BEG")) {
          return 2;
        }
      }
    }
    // `reorg.c:1369-1373`: immediate note after `target_label` for
    // `NOTE_INSN_LOOP_VTOP` -> returns 1.
    const auto& target_insns = cfg.rpo_blocks[target_b].instructions;
    if (!target_insns.empty()) {
      auto parsed = ParsePatternAndNotes(target_insns.front());
      if (parsed.ok() && IsNoteWithKind(parsed->pattern, "NOTE_INSN_LOOP_VTOP")) {
        return 1;
      }
    }
  }

  // `reorg.c:1341-1342, 1379-1391`: relative rarity of fallthrough vs target.
  const int rare_dest = RareDestinationScore(cfg, target_b);
  const int rare_fallthrough = RareDestinationScore(cfg, block_b + 1);
  switch (rare_fallthrough - rare_dest) {
    case -2:
      return -1;
    case -1:
      return 0;
    case 1:
      return 1;
    case 2:
      return 2;
    default:
      break;
  }

  return MostlyTrueJumpScore(jump_pat, block_b, target_b);
}

bool RtxAddrCanTrapForReorg(const RtlExpr& addr) {
  switch (addr.op) {
    case RtlOp::kSymbolRef:
    case RtlOp::kLabelRef:
      return false;
    case RtlOp::kReg: {
      // `rtlanal.c:136-139`: `$sp` (29), `$fp` (30), and `argp` do not trap.
      if (addr.IsArgPointerReg()) {
        return false;
      }
      const int r = addr.RegNo();
      return r != 29 && r != 30;
    }
    case RtlOp::kConst:
      return addr.children.empty() ? true : RtxAddrCanTrapForReorg(addr.children[0]);
    case RtlOp::kPlus:
      return addr.children.size() != 2 || RtxAddrCanTrapForReorg(addr.children[0]) ||
             !addr.children[1].IsConstInt();
    case RtlOp::kLoSum:
      return addr.children.size() != 2 || RtxAddrCanTrapForReorg(addr.children[1]);
    default:
      return true;
  }
}

bool PatternMayTrapForReorg(const RtlExpr& pat) {
  switch (pat.op) {
    case RtlOp::kConstInt:
    case RtlOp::kConstDouble:
    case RtlOp::kSymbolRef:
    case RtlOp::kLabelRef:
    case RtlOp::kConst:
    case RtlOp::kPc:
    case RtlOp::kCc0:
    case RtlOp::kReg:
    case RtlOp::kScratch:
      return false;
    case RtlOp::kUnspecVolatile:
    case RtlOp::kExprList:
    case RtlOp::kCall:
      return true;
    case RtlOp::kMem:
      return pat.children.empty() ? true : RtxAddrCanTrapForReorg(pat.children[0]);
    case RtlOp::kDiv:
    case RtlOp::kMod:
    case RtlOp::kUdiv:
    case RtlOp::kUmod:
      if (pat.children.size() < 2 || !IsConstantExprForMayTrap(pat.children[1])) {
        return true;
      }
      if (pat.children[1].IsConstInt() && pat.children[1].IntVal() == 0) {
        return true;
      }
      // `rtlanal.c:1597-1610`: note GCC 2.7.2 falls through to `EXPR_LIST` and
      // returns 1 for all `DIV`/`MOD`/`UDIV`/`UMOD` (missing `break` in
      // upstream `rtlanal.c:1606`). Mirror that exact behavior!
      return true;
    default:
      // `rtlanal.c:1613-1614`: any floating-point mode expression may trap.
      if (pat.mode == RtlMachineMode::kSf || pat.mode == RtlMachineMode::kDf ||
          pat.mode == RtlMachineMode::kTf) {
        return true;
      }
      break;
  }
  for (const RtlExpr& child : pat.children) {
    if (PatternMayTrapForReorg(child)) {
      return true;
    }
  }
  return false;
}

bool HasRegDeadConstZeroNote(const RtlPatternAndNotes& pat_notes) {
  for (const RtlRegNote& note : pat_notes.reg_notes) {
    if (note.kind == RtlMachineMode::kRegDead && note.datum.IsConstInt() &&
        note.datum.IntVal() == 0) {
      return true;
    }
  }
  return false;
}

bool BlockHasOnlyUseOrClobberInsns(const RtlBasicBlock& blk, bool skip_notes) {
  bool saw_use_or_clobber = false;
  for (const std::string& s : blk.instructions) {
    auto parsed = ParsePatternAndNotes(s);
    if (!parsed.ok()) {
      return false;
    }
    if (skip_notes && parsed->pattern.op == RtlOp::kNote) {
      continue;
    }
    if (parsed->pattern.op != RtlOp::kUse && parsed->pattern.op != RtlOp::kClobber) {
      return false;
    }
    saw_use_or_clobber = true;
  }
  return saw_use_or_clobber;
}

bool BlockStartsWithUseOrClobberInsn(const RtlBasicBlock& blk) {
  for (const std::string& s : blk.instructions) {
    auto parsed = ParsePatternAndNotes(s);
    if (!parsed.ok()) {
      return false;
    }
    if (parsed->pattern.op == RtlOp::kNote) {
      continue;
    }
    return parsed->pattern.op == RtlOp::kUse || parsed->pattern.op == RtlOp::kClobber;
  }
  return false;
}

}  // namespace rom_nom_nom::lifter
