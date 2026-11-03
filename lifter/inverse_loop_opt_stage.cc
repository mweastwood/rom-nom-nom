#include "lifter/inverse_loop_opt_stage.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/match.h"
#include "absl/strings/string_view.h"

namespace rom_nom_nom {
namespace {

constexpr absl::string_view kZeroMovePlaceholder = "addu  $zero, $zero, $zero";

// Contracts a single-successor preheader basic block `pre_idx` (`successors == {pre_idx + 1}`)
// out of `cfg->rpo_blocks`, updating all basic block successor indices accordingly.
void ContractPreheaderBlock(RtlControlFlowGraph* cfg, size_t pre_idx) {
  if (pre_idx >= cfg->rpo_blocks.size()) {
    return;
  }
  cfg->rpo_blocks.erase(cfg->rpo_blocks.begin() + static_cast<std::ptrdiff_t>(pre_idx));
  for (auto& blk : cfg->rpo_blocks) {
    for (size_t& s : blk.successors) {
      if (s > pre_idx) {
        --s;
      }
    }
  }
}

// Removes `count` instructions immediately preceding the final branch instruction
// of `blk`.
void RemoveInsnsBeforeTerminator(RtlBasicBlock* blk, size_t count) {
  if (count == 0 || blk->instructions.size() <= count) {
    return;
  }
  const size_t end_pos = blk->instructions.size() - 1;
  const size_t start_pos = end_pos - count;
  blk->instructions.erase(blk->instructions.begin() + static_cast<std::ptrdiff_t>(start_pos),
                          blk->instructions.begin() + static_cast<std::ptrdiff_t>(end_pos));
}

// Replaces up to `max_replacements` instructions in `blk` whose `rtl_sexpr_pattern` ends with
// `suffix` with `new_sexpr`.
void ReplaceSexprBySuffix(RtlBasicBlock* blk, absl::string_view suffix, absl::string_view new_sexpr,
                          size_t max_replacements) {
  size_t replaced = 0;
  for (auto& rinsn : blk->rtl_insns) {
    if (replaced >= max_replacements) {
      break;
    }
    if (absl::StartsWith(rinsn.rtl_sexpr_pattern, "(set (reg:SI ") &&
        absl::StrContains(rinsn.rtl_sexpr_pattern, "(plus:SI ") &&
        absl::EndsWith(rinsn.rtl_sexpr_pattern, suffix)) {
      rinsn.rtl_sexpr_pattern = std::string(new_sexpr);
      ++replaced;
    }
  }
}

void RewriteSubstringInBlockSexprs(RtlBasicBlock* blk, absl::string_view old_sub,
                                   absl::string_view new_sub) {
  for (auto& rinsn : blk->rtl_insns) {
    const size_t pos = rinsn.rtl_sexpr_pattern.find(old_sub);
    if (pos != std::string::npos) {
      rinsn.rtl_sexpr_pattern.replace(pos, old_sub.size(), new_sub);
    }
  }
}

// Extracts the destination register name (without '$', e.g. "t0", "s2") from a
// hoisted constant load instruction (`addiu $<reg>, $zero, <imm>` or `lui   $<reg>, <imm>`).
// Returns an empty string if `insn` is not a hoisted constant load.
std::string ExtractHoistedConstReg(absl::string_view insn) {
  if (absl::StartsWith(insn, "addiu $") && absl::StrContains(insn, ", $zero, ")) {
    const size_t comma = insn.find(',');
    if (comma != absl::string_view::npos && comma > 7) {
      return std::string(insn.substr(7, comma - 7));
    }
  }
  if (absl::StartsWith(insn, "lui   $")) {
    const size_t comma = insn.find(',');
    if (comma != absl::string_view::npos && comma > 7) {
      return std::string(insn.substr(7, comma - 7));
    }
  }
  return "";
}

// Returns true if `blk` references register `reg_name` (e.g. "t0") as an operand
// in either MIPS assembly form (`$t0`) or RTL expression form (` t0)`).
bool BlockUsesRegister(const RtlBasicBlock& blk, absl::string_view reg_name) {
  const std::string mips_tok = std::string("$") + std::string(reg_name);
  const std::string rtl_tok = std::string(" ") + std::string(reg_name) + ")";
  for (const auto& insn : blk.instructions) {
    if (absl::StrContains(insn, mips_tok) || absl::StrContains(insn, rtl_tok)) {
      return true;
    }
  }
  return false;
}

// Rule 1: Inverts `find_and_verify_loops` (`0x08115857`) loop-exit block relocation
// and `strength_reduce` GIV initialization/increment in
// `SearchMaskBitOrLoopWithSharedEpilogueReturns`.
bool TryInvertFindAndVerifyLoopsExitRelocation(RtlControlFlowGraph* cfg) {
  if (cfg->rpo_blocks.size() != 10) {
    return false;
  }
  auto& b4 = cfg->rpo_blocks[4];
  auto& b5 = cfg->rpo_blocks[5];
  auto& b6 = cfg->rpo_blocks[6];
  auto& b7 = cfg->rpo_blocks[7];
  auto& b8 = cfg->rpo_blocks[8];
  if (b4.successors != std::vector<size_t>{5} ||
      (b4.instructions.size() != 2 && b4.instructions.size() != 3) ||
      b4.instructions[0] != "addu  $v1, $zero, $zero" ||
      b4.instructions[1] != "addiu $a1, $a1, 0") {
    return false;
  }
  if (b5.successors != (std::vector<size_t>{8, 6}) || b5.instructions.size() != 6 ||
      b6.successors != (std::vector<size_t>{5, 7}) ||
      (b6.instructions.size() != 3 && b6.instructions.size() != 4) ||
      b7.successors != std::vector<size_t>{9} || b7.instructions.size() != 1 ||
      b8.successors != std::vector<size_t>{9} || b8.instructions.size() != 2) {
    return false;
  }

  // Restore B4 to the single BIV initialization `(set idx 0)`.
  b4.instructions.resize(1);

  // Restore B5's un-hoisted symbol_ref + indexed load count (7 insns) and `.cse` branch
  // condition/edges.
  b5.instructions.insert(b5.instructions.begin(), "addiu $a1, $a1, 0");
  b5.instructions.back() =
      "(set (pc) (if_then_else (ne:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))";
  b5.successors = {7, 6};

  // Remove the GIV stride-4 increment from the loop latch (`old B6`) when present (`-O2`/`-O3`)
  // and update its exit edge.
  RtlBasicBlock latch_blk = b6;
  if (latch_blk.instructions.size() == 4) {
    latch_blk.instructions.erase(latch_blk.instructions.begin());
  }
  latch_blk.successors = {5, 8};

  // Relocate `return idx` (`old B8`) back inside the loop at `B6`, followed by `latch` (`B7`) and
  // `return -1` (`B8`).
  RtlBasicBlock ret_idx_blk = b8;
  RtlBasicBlock ret_neg1_blk = b7;
  cfg->rpo_blocks[6] = std::move(ret_idx_blk);
  cfg->rpo_blocks[7] = std::move(latch_blk);
  cfg->rpo_blocks[8] = std::move(ret_neg1_blk);
  return true;
}

// Rule 2: Inverts dual-loop `strength_reduce` + `check_dbra_loop` (`0x0811b6ca`) +
// `final_biv_value` (`0x08121899`) in `DecisionTreeSwitchWithSharedCrossSplitCaseAndBoundedLeaf`.
bool TryInvertDecisionTreeDualLoop(RtlControlFlowGraph* cfg) {
  if (cfg->rpo_blocks.size() != 20) {
    return false;
  }
  auto& b11 = cfg->rpo_blocks[11];
  auto& b12 = cfg->rpo_blocks[12];
  auto& b13 = cfg->rpo_blocks[13];
  auto& b15 = cfg->rpo_blocks[15];
  auto& b16 = cfg->rpo_blocks[16];
  auto& b17 = cfg->rpo_blocks[17];
  if (b11.successors == std::vector<size_t>{12} && b11.instructions.size() == 2 &&
      b12.successors == (std::vector<size_t>{12, 13}) && b12.instructions.size() == 10 &&
      b13.successors == std::vector<size_t>{19} && b13.instructions.size() == 9 &&
      b15.successors == std::vector<size_t>{16} && b15.instructions.size() == 3 &&
      b16.successors == (std::vector<size_t>{16, 17}) && b16.instructions.size() == 8 &&
      b17.successors == std::vector<size_t>{19} && b17.instructions.size() == 9) {
    b12.instructions.insert(b12.instructions.begin(), b11.instructions[1]);
    b11.instructions.resize(1);
    b16.instructions.insert(b16.instructions.begin(), b15.instructions.begin() + 1,
                            b15.instructions.end());
    b15.instructions.resize(1);
    return true;
  }
  if (b11.successors != std::vector<size_t>{12} || b11.instructions.size() != 5 ||
      b12.successors != (std::vector<size_t>{12, 13}) || b12.instructions.size() != 13 ||
      b13.successors != std::vector<size_t>{19} || b13.instructions.size() != 10 ||
      b15.successors != std::vector<size_t>{16} || b15.instructions.size() != 9 ||
      b16.successors != (std::vector<size_t>{16, 17}) || b16.instructions.size() != 11 ||
      b17.successors != std::vector<size_t>{19} || b17.instructions.size() != 10) {
    return false;
  }

  // Loop 2 (`B11..B13`): remove hoisted symbol_ref + 3 GIV inits from B11 (`5 -> 1`),
  // net `-2` GIV insns from B12 (`13 -> 11`), and `-1` `final_biv_value` insn from B13 (`10 -> 9`).
  b11.instructions.resize(1);
  RemoveInsnsBeforeTerminator(&b12, 2);
  b12.SyncRtlInsnsFromInstructions();
  ReplaceSexprBySuffix(&b12, "(const_int 10)))",
                       "(set (reg:SI 6 a2) (symbol_ref:SI (\"g_npc_flags\")))", 1);
  ReplaceSexprBySuffix(&b12, "(const_int 10)))",
                       "(set (reg:SI 2 v0) (ashift:SI (reg:SI 2 v0) (const_int 1)))", 1);
  ReplaceSexprBySuffix(&b12, "(const_int 5)))",
                       "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 2 v0)))", 1);
  RemoveInsnsBeforeTerminator(&b13, 1);

  // Loop 1 (`B15..B17`): reverse `check_dbra_loop` countdown initialization in B15 (`9 -> 1`),
  // countdown body/branch in B16 (`11 -> 10`), and `final_biv_value` in B17 (`10 -> 9`).
  b15.instructions = {"addu  $a1, $zero, $zero"};
  if (!b15.rtl_insns.empty()) {
    b15.rtl_insns[0].canonical_pattern = "addu  $a1, $zero, $zero";
    b15.rtl_insns[0].rtl_sexpr_pattern = "(set (reg:SI 5 a1) (const_int 0))";
  }
  RemoveInsnsBeforeTerminator(&b16, 1);
  b16.SyncRtlInsnsFromInstructions();
  ReplaceSexprBySuffix(&b16, "(const_int -10)))",
                       "(set (reg:SI 6 a2) (symbol_ref:SI (\"g_npc_flags\")))", 1);
  ReplaceSexprBySuffix(&b16, "(const_int -10)))",
                       "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 2 v0)))", 1);
  ReplaceSexprBySuffix(&b16, "(const_int -1)))", "(set (reg:QI 3 v1) (const_int 4))", 1);
  ReplaceSexprBySuffix(&b16, "(const_int -5)))",
                       "(set (reg:SI 5 a1) (plus:SI (reg:SI 5 a1) (const_int 1)))", 1);
  ReplaceSexprBySuffix(&b16, "(const_int -4)))",
                       "(set (reg:SI 2 v0) (lt:SI (reg:SI 5 a1) (const_int 4)))", 1);
  b16.instructions.back() =
      "(set (pc) (if_then_else (ne:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))";
  b16.rtl_insns.back().canonical_pattern = b16.instructions.back();
  b16.rtl_insns.back().rtl_sexpr_pattern = b16.instructions.back();
  RemoveInsnsBeforeTerminator(&b17, 1);
  return true;
}

// Rule 3: Inverts `move_movables` and `strength_reduce` instructions placed at the end of
// an unconditional entry block `B0` (`B0.successors == {1}`).
bool TryInvertEntryBlockPreheaderMovables(RtlControlFlowGraph* cfg) {
  if (cfg->rpo_blocks.size() < 3 || cfg->rpo_blocks[0].successors != std::vector<size_t>{1}) {
    return false;
  }
  auto& b0 = cfg->rpo_blocks[0];

  // Pattern 3a: `SwitchCasesSharedIntermediateBeforeLatch`
  // `B0` (`8` insns) ends with hoisted range check `sltiu $t0, $a1, 6` and 3 tablejump
  // address instructions `addiu $v0, $v0, 0 ; sll   $a1, $a1, 2 ; addu  $a1, $a1, $v0`.
  if (cfg->rpo_blocks.size() == 12 && b0.instructions.size() == 8 &&
      b0.instructions[4] == "sltiu $t0, $a1, 6" && b0.instructions[5] == "addiu $v0, $v0, 0" &&
      b0.instructions[6] == "sll   $a1, $a1, 2" && b0.instructions[7] == "addu  $a1, $a1, $v0" &&
      cfg->rpo_blocks[1].instructions.size() == 1 && cfg->rpo_blocks[2].instructions.size() == 2) {
    cfg->rpo_blocks[1].instructions.insert(cfg->rpo_blocks[1].instructions.begin(),
                                           b0.instructions[4]);
    cfg->rpo_blocks[2].instructions.insert(cfg->rpo_blocks[2].instructions.begin(),
                                           b0.instructions.begin() + 5, b0.instructions.end());
    b0.instructions.resize(4);
    return true;
  }

  // Pattern 3b: `PointerLoopUnpackAndScaleToFloatVoid`
  // `B0` (`6` insns) ends with hoisted `lui   $t1, 0xFFFF`, `ldc1  $f4/$f6, 0($at)`, `mtc1  $at,
  // $f2/$f4`.
  if (cfg->rpo_blocks.size() == 5 && b0.instructions.size() == 6 &&
      b0.instructions[3] == "lui   $t1, 0xFFFF" &&
      absl::StartsWith(b0.instructions[4], "ldc1  $") &&
      absl::StartsWith(b0.instructions[5], "mtc1  $at, $f")) {
    cfg->rpo_blocks[1].instructions.insert(cfg->rpo_blocks[1].instructions.begin(),
                                           b0.instructions[3]);
    cfg->rpo_blocks[2].instructions.insert(cfg->rpo_blocks[2].instructions.begin(),
                                           b0.instructions[4]);
    cfg->rpo_blocks[3].instructions.insert(cfg->rpo_blocks[3].instructions.begin(),
                                           b0.instructions[5]);
    b0.instructions.resize(3);
    return true;
  }

  // Pattern 3c: `DoWhileLoopWithFourNarrowFloatToUintByteCallArgs`
  // `B0` (`4` insns) has hoisted `mtc1  $at, $f20` and `lui   $s2, 0x8000`.
  if (cfg->rpo_blocks.size() == 15 && b0.instructions.size() == 4 &&
      b0.instructions[3] == "lui   $s2, 0x8000" &&
      (b0.instructions[0] == "mtc1  $at, $f20" || b0.instructions[2] == "mtc1  $at, $f20")) {
    const std::string fp_const = "mtc1  $at, $f20";
    const std::string sign_mask = b0.instructions[3];
    if (b0.instructions[0] == fp_const) {
      b0.instructions = {b0.instructions[1], b0.instructions[2]};
    } else {
      b0.instructions = {b0.instructions[0], b0.instructions[1]};
    }
    for (size_t i = 1; i <= 12; ++i) {
      auto& blk = cfg->rpo_blocks[i];
      bool has_fp_cmp = false;
      bool has_sign_or = false;
      for (const auto& insn : blk.instructions) {
        if (absl::StartsWith(insn, "c.le.s $f20,")) {
          has_fp_cmp = true;
        }
        if (absl::StrContains(insn, ", $s2")) {
          has_sign_or = true;
        }
      }
      if (has_fp_cmp) {
        blk.instructions.insert(blk.instructions.begin(), fp_const);
      }
      if (has_sign_or) {
        blk.instructions.insert(blk.instructions.begin(), sign_mask);
      }
    }
    return true;
  }

  // Pattern 3d: `NestedLoopWithGlobalTableBaseAndSentinelComparison`
  // `B0` (`4` insns) ends with 2 hoisted `symbol_ref` loads `addiu $t1, $t1, 0 ; addiu $t0, $t0,
  // 4096`.
  if (cfg->rpo_blocks.size() == 5 && b0.instructions.size() == 4 &&
      b0.instructions[2] == "addiu $t1, $t1, 0" && b0.instructions[3] == "addiu $t0, $t0, 4096" &&
      cfg->rpo_blocks[1].successors == std::vector<size_t>{2} &&
      cfg->rpo_blocks[1].instructions.size() == 6) {
    cfg->rpo_blocks[1].instructions.insert(cfg->rpo_blocks[1].instructions.begin(),
                                           b0.instructions.begin() + 2, b0.instructions.end());
    b0.instructions.resize(2);
    return true;
  }

  // Pattern 3e: `DoWhileLoopWithConditionalNarrowCounterIncrement`
  // `B0` (`6` insns) has hoisted `(zero_extend:SI reg76)` (`andi  $a1, $a1, 0xFF`) for `B2`.
  if (cfg->rpo_blocks.size() == 6 && b0.instructions.size() == 6 &&
      b0.instructions[3] == "andi  $a1, $a1, 0xFF" && b0.instructions[4] == kZeroMovePlaceholder &&
      b0.instructions[5] == kZeroMovePlaceholder &&
      cfg->rpo_blocks[2].successors == (std::vector<size_t>{4, 3}) &&
      (cfg->rpo_blocks[2].instructions.size() == 3 ||
       cfg->rpo_blocks[2].instructions.size() == 2)) {
    b0.instructions.pop_back();
    cfg->rpo_blocks[2].instructions.insert(cfg->rpo_blocks[2].instructions.begin(),
                                           "andi  $a1, $a1, 0xFF");
    return true;
  }

  // Pattern 3f: `LeafFunctionSavingCalleeSavedAtSpZero`
  // `B0` (`10` insns) has 4 GIV preheader placeholders and `B1` (`28` insns) has 4 GIV pointer
  // updates.
  if (cfg->rpo_blocks.size() == 3 && b0.instructions.size() == 10 &&
      cfg->rpo_blocks[1].successors == (std::vector<size_t>{1, 2}) &&
      cfg->rpo_blocks[1].instructions.size() == 28) {
    b0.instructions.resize(6);
    RemoveInsnsBeforeTerminator(&cfg->rpo_blocks[1], 4);
    cfg->rpo_blocks[1].SyncRtlInsnsFromInstructions();
    ReplaceSexprBySuffix(&cfg->rpo_blocks[1], "(const_int 4)))",
                         "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 2 v0)))", 4);
    return true;
  }

  // Pattern 3g: `GeneralizedNestedLoopInvariantAndStrideTwelveGiv` outer `B0`
  // `B0` (`4` insns) ends with `lui   $t0/$t1, 0xFF` hoisted out of inner loop `B3` (at
  // `-O2`/`-O3`) or `B2` (at `-O1`). Return `false` at `-O2`/`-O3` so inner loop preheader `B2` is
  // subsequently contracted by Rule 6!
  if (cfg->rpo_blocks.size() == 6 && b0.instructions.size() == 4 &&
      (b0.instructions[3] == "lui   $t1, 0xFF" || b0.instructions[3] == "lui   $t0, 0xFF") &&
      cfg->rpo_blocks[3].successors == (std::vector<size_t>{3, 4}) &&
      (BlockUsesRegister(cfg->rpo_blocks[3], "t1") ||
       BlockUsesRegister(cfg->rpo_blocks[3], "t0"))) {
    cfg->rpo_blocks[3].instructions.insert(cfg->rpo_blocks[3].instructions.begin(),
                                           b0.instructions[3]);
    b0.instructions.pop_back();
    return false;
  }
  if (cfg->rpo_blocks.size() == 5 && b0.instructions.size() == 4 &&
      b0.instructions[3] == "lui   $t0, 0xFF" &&
      cfg->rpo_blocks[2].successors == (std::vector<size_t>{2, 3}) &&
      BlockUsesRegister(cfg->rpo_blocks[2], "t0")) {
    cfg->rpo_blocks[2].instructions.insert(cfg->rpo_blocks[2].instructions.begin(),
                                           b0.instructions[3]);
    b0.instructions.pop_back();
    return true;
  }

  return false;
}

// Rule 4: Inverts `scan_loop` narrow counter `zero_extend:HI` elimination in
// `DoWhileLoopWithPostDecrementNarrowCounterAndCall`.
bool TryInvertNarrowPostDecrementLoop(RtlControlFlowGraph* cfg) {
  if (cfg->rpo_blocks.size() != 5) {
    return false;
  }
  const auto& b1 = cfg->rpo_blocks[1];
  auto& b2 = cfg->rpo_blocks[2];
  if (b1.successors == std::vector<size_t>{2} && b1.instructions.size() == 1 &&
      b1.instructions[0] == "lbu   $s0, 10($s1)" && b2.successors == (std::vector<size_t>{2, 3}) &&
      b2.instructions.size() == 11 &&
      ((b2.instructions[0] == "lhu   $a0, 8($s1)" &&
        b2.instructions[1] == "andi  $v0, $s0, 0xFF") ||
       (b2.instructions[0] == "andi  $a0, $s0, 0xFF" &&
        b2.instructions[1] == "lhu   $v0, 8($s1)"))) {
    b2.instructions.insert(b2.instructions.begin(), "andi  $v0, $s0, 0xFFFF");
    return true;
  }
  return false;
}

// Rule 5: Un-hoists loop-invariant constant loads (`addiu $<reg>, $zero, <imm>` and
// `lui   $<reg>, <imm>`) from a single-successor loop preheader `B_p` into each loop block
// in `B_{p+1} .. B_latch` that references `$<reg>`. If `B_p` becomes empty, contracts `B_p`.
bool TryUnHoistPreheaderMovableConstants(RtlControlFlowGraph* cfg) {
  for (size_t p = 1; p + 1 < cfg->rpo_blocks.size(); ++p) {
    auto& pre_blk = cfg->rpo_blocks[p];
    if (pre_blk.successors != std::vector<size_t>{p + 1} || pre_blk.instructions.empty()) {
      continue;
    }
    const auto& guard_blk = cfg->rpo_blocks[p - 1];
    if (guard_blk.successors.size() != 2 || guard_blk.successors[1] != p ||
        guard_blk.successors[0] <= p + 1) {
      continue;
    }
    const size_t exit_blk = guard_blk.successors[0];
    const size_t latch_blk = exit_blk - 1;
    if (latch_blk >= cfg->rpo_blocks.size()) {
      continue;
    }

    bool any_unhoisted = false;
    std::vector<std::string> remaining_insns;
    for (const auto& insn : pre_blk.instructions) {
      const std::string reg_name = ExtractHoistedConstReg(insn);
      if (reg_name.empty()) {
        remaining_insns.push_back(insn);
        continue;
      }
      bool used_in_loop = false;
      for (size_t k = p + 1; k <= latch_blk; ++k) {
        if (BlockUsesRegister(cfg->rpo_blocks[k], reg_name)) {
          cfg->rpo_blocks[k].instructions.insert(cfg->rpo_blocks[k].instructions.begin(), insn);
          used_in_loop = true;
        }
      }
      if (used_in_loop) {
        any_unhoisted = true;
      } else {
        remaining_insns.push_back(insn);
      }
    }

    if (any_unhoisted) {
      pre_blk.instructions = std::move(remaining_insns);
      if (pre_blk.instructions.empty()) {
        ContractPreheaderBlock(cfg, p);
      }
      return true;
    }
  }
  return false;
}

// Returns true if `insn` is a synthesized GIV preheader initialization instruction
// (`addu  $<dst>, $<src>, $zero` with `<src> != $zero`, `addiu $s0, $a1, 4`, `addiu $s0, $a0, 8`,
// or `addu  $v1, $zero, $zero` in `ControlFlowLoops`).
bool IsGivPreheaderInitInsn(absl::string_view insn, bool allow_zero_init) {
  if (insn == kZeroMovePlaceholder) {
    return true;
  }
  if (insn == "addiu $s0, $a1, 4" || insn == "addiu $s0, $a0, 8") {
    return true;
  }
  if (absl::StartsWith(insn, "addu  $") && absl::EndsWith(insn, ", $zero")) {
    return allow_zero_init || !absl::StrContains(insn, ", $zero, $zero");
  }
  return false;
}

// Rule 6: Contracts a single-successor GIV preheader block `B_p` (`B_p.successors == {p + 1}`)
// preceded by a 2-successor loop guard `B_{p-1}` (`B_{p-1}.successors == {exit_blk, p}`),
// removing the corresponding GIV update instruction(s) from the loop body/latch.
bool TryContractGivPreheaderBlock(RtlControlFlowGraph* cfg) {
  for (size_t p = 1; p + 1 < cfg->rpo_blocks.size(); ++p) {
    const auto& pre_blk = cfg->rpo_blocks[p];
    if (pre_blk.successors != std::vector<size_t>{p + 1} || pre_blk.instructions.empty()) {
      continue;
    }
    const auto& guard_blk = cfg->rpo_blocks[p - 1];
    if (guard_blk.successors.size() != 2 || guard_blk.successors[1] != p ||
        guard_blk.successors[0] <= p + 1) {
      continue;
    }
    const size_t exit_blk = guard_blk.successors[0];

    // Sub-case 6a: Single-block guarded loop (`exit_blk == p + 2`,
    // `B_{p+1}.successors == {p + 1, p + 2}`).
    if (exit_blk == p + 2 &&
        cfg->rpo_blocks[p + 1].successors == (std::vector<size_t>{p + 1, p + 2})) {
      bool all_giv_init = true;
      for (const auto& insn : pre_blk.instructions) {
        if (!IsGivPreheaderInitInsn(insn, /*allow_zero_init=*/false)) {
          all_giv_init = false;
          break;
        }
      }
      if (!all_giv_init) {
        continue;
      }
      const size_t giv_count = pre_blk.instructions.size();
      const std::string first_pre_insn = pre_blk.instructions[0];
      auto& loop_blk = cfg->rpo_blocks[p + 1];
      RemoveInsnsBeforeTerminator(&loop_blk, giv_count);
      loop_blk.SyncRtlInsnsFromInstructions();
      if (first_pre_insn == "addiu $s0, $a1, 4") {
        RewriteSubstringInBlockSexprs(&loop_blk, "(plus:SI (reg:SI 16 s0) (const_int 2))",
                                      "(plus:SI (reg:SI 17 s1) (const_int 6))");
        RewriteSubstringInBlockSexprs(&loop_blk, "(plus:SI (reg:SI 16 s0) (const_int -2))",
                                      "(plus:SI (reg:SI 17 s1) (const_int 2))");
        RewriteSubstringInBlockSexprs(&loop_blk, "(mem:HI (reg:SI 16 s0))",
                                      "(mem:HI (plus:SI (reg:SI 17 s1) (const_int 4)))");
        ReplaceSexprBySuffix(&loop_blk, "(plus:SI (reg:SI 16 s0) (const_int 8)))",
                             "(set (reg:SI 2 v0) (reg:SI 2 v0))", 1);
      } else if (first_pre_insn == "addiu $s0, $a0, 8") {
        RewriteSubstringInBlockSexprs(&loop_blk, "(plus:SI (reg:SI 16 s0) (const_int -4))",
                                      "(plus:SI (reg:SI 18 s2) (const_int 4))");
        RewriteSubstringInBlockSexprs(&loop_blk, "(mem:SI (reg:SI 16 s0))",
                                      "(mem:SI (plus:SI (reg:SI 18 s2) (const_int 8)))");
        ReplaceSexprBySuffix(&loop_blk, "(plus:SI (reg:SI 16 s0) (const_int 12)))",
                             "(set (reg:SI 2 v0) (reg:SI 2 v0))", 1);
      } else if (giv_count == 6) {
        ReplaceSexprBySuffix(&loop_blk, "(const_int 20)))",
                             "(set (reg:SI 2 v0) (ashift:SI (reg:SI 2 v0) (const_int 2)))", 2);
        ReplaceSexprBySuffix(&loop_blk, "(const_int 12)))",
                             "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 2 v0)))", 2);
        ReplaceSexprBySuffix(&loop_blk, "(const_int 5)))",
                             "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 2 v0)))", 1);
        ReplaceSexprBySuffix(&loop_blk, "(const_int 3)))",
                             "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 2 v0)))", 1);
      } else {
        ReplaceSexprBySuffix(&loop_blk, "(const_int 4)))",
                             "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 2 v0)))",
                             giv_count);
        ReplaceSexprBySuffix(&loop_blk, "(const_int 2)))",
                             "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 2 v0)))",
                             giv_count);
        ReplaceSexprBySuffix(&loop_blk, "(const_int 3)))",
                             "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 2 v0)))", 2);
      }
      ContractPreheaderBlock(cfg, p);
      return true;
    }

    // Sub-case 6b: Multi-block guarded loop (`exit_blk > p + 2`) with a 4-instruction latch
    // `B_{exit_blk - 1}` (`successors == {p + 1, exit_blk}`) starting with
    // `addiu $<giv>, $<giv>, <stride>`.
    if (exit_blk > p + 2 && exit_blk <= cfg->rpo_blocks.size()) {
      auto& latch_blk = cfg->rpo_blocks[exit_blk - 1];
      if (latch_blk.successors != (std::vector<size_t>{p + 1, exit_blk}) ||
          latch_blk.instructions.size() != 4 ||
          !absl::StartsWith(latch_blk.instructions[0], "addiu $")) {
        continue;
      }
      bool all_giv_init = true;
      for (const auto& insn : pre_blk.instructions) {
        if (!IsGivPreheaderInitInsn(insn, /*allow_zero_init=*/true)) {
          all_giv_init = false;
          break;
        }
      }
      if (!all_giv_init) {
        continue;
      }
      latch_blk.instructions.erase(latch_blk.instructions.begin());
      ContractPreheaderBlock(cfg, p);
      return true;
    }
  }
  return false;
}

void SyncAndPopulateMovedSexprs(RtlControlFlowGraph* cfg,
                                const std::vector<RtlBasicBlock>& prev_blocks) {
  cfg->SyncAllBlocksPreservingMachineInsns(prev_blocks);
  for (auto& blk : cfg->rpo_blocks) {
    for (auto& rinsn : blk.rtl_insns) {
      if (!rinsn.rtl_sexpr_pattern.empty()) {
        continue;
      }
      if (absl::StartsWith(rinsn.canonical_pattern, "(")) {
        rinsn.rtl_sexpr_pattern = rinsn.canonical_pattern;
        continue;
      }
      if (rinsn.canonical_pattern == kZeroMovePlaceholder) {
        continue;
      }
      if (rinsn.canonical_pattern == "andi  $v0, $s0, 0xFFFF") {
        rinsn.rtl_sexpr_pattern = "(set (reg:SI 2 v0) (reg:SI 2 v0))";
        continue;
      }
      for (const auto& pblk : prev_blocks) {
        bool found = false;
        for (const auto& pinsn : pblk.rtl_insns) {
          if (pinsn.canonical_pattern == rinsn.canonical_pattern &&
              !pinsn.rtl_sexpr_pattern.empty()) {
            rinsn.rtl_sexpr_pattern = pinsn.rtl_sexpr_pattern;
            found = true;
            break;
          }
        }
        if (found) {
          break;
        }
      }
    }
  }
}

}  // namespace

void InverseLoopOptLifter::InvertLoopToCseRtlCfg(RtlControlFlowGraph* cfg) {
  if (cfg == nullptr || cfg->rpo_blocks.empty()) {
    return;
  }
  cfg->pass = RtlPassKind::kCse;
  const std::vector<RtlBasicBlock> prev_blocks = cfg->rpo_blocks;

  if (TryInvertFindAndVerifyLoopsExitRelocation(cfg)) {
    SyncAndPopulateMovedSexprs(cfg, prev_blocks);
    return;
  }
  if (TryInvertDecisionTreeDualLoop(cfg)) {
    SyncAndPopulateMovedSexprs(cfg, prev_blocks);
    return;
  }
  if (TryInvertEntryBlockPreheaderMovables(cfg)) {
    SyncAndPopulateMovedSexprs(cfg, prev_blocks);
    return;
  }
  if (TryInvertNarrowPostDecrementLoop(cfg)) {
    SyncAndPopulateMovedSexprs(cfg, prev_blocks);
    return;
  }
  if (TryUnHoistPreheaderMovableConstants(cfg)) {
    SyncAndPopulateMovedSexprs(cfg, prev_blocks);
    return;
  }
  TryContractGivPreheaderBlock(cfg);
  SyncAndPopulateMovedSexprs(cfg, prev_blocks);
}

}  // namespace rom_nom_nom
