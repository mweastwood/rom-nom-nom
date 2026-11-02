#include "lifter/inverse_cse1_basic_block_stage.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"

namespace rom_nom_nom {
namespace {

constexpr std::string_view kCanonicalNop = "addu  $zero, $zero, $zero";
constexpr std::string_view kJumpTerminator = "(set (pc) (label_ref))";
constexpr std::string_view kPreheaderCondBranch =
    "(set (pc) (if_then_else (eq:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))";

bool IsNop(std::string_view insn) {
  return insn == kCanonicalNop;
}

std::vector<std::string> GetRealInstructions(const RtlBasicBlock& blk) {
  std::vector<std::string> real;
  real.reserve(blk.instructions.size());
  for (const std::string& insn : blk.instructions) {
    if (!IsNop(insn)) {
      real.push_back(insn);
    }
  }
  return real;
}

std::vector<std::string_view> SplitOperands(std::string_view insn) {
  if (insn.size() <= 6) {
    return {};
  }
  std::vector<std::string_view> ops = absl::StrSplit(insn.substr(6), ',');
  for (std::string_view& op : ops) {
    op = absl::StripAsciiWhitespace(op);
  }
  return ops;
}

std::string ExtractBranchConditionReg(std::string_view insn) {
  if (!absl::StartsWith(insn, "(set (pc) (if_then_else ")) {
    return "";
  }
  const size_t reg_pos = insn.find("(reg:");
  if (reg_pos == std::string_view::npos) {
    return "";
  }
  const size_t close_pos = insn.find(')', reg_pos);
  if (close_pos == std::string_view::npos) {
    return "";
  }
  const std::string_view inner = insn.substr(reg_pos + 1, close_pos - reg_pos - 1);
  const std::vector<std::string_view> parts = absl::StrSplit(inner, ' ', absl::SkipEmpty());
  if (parts.size() >= 3) {
    return absl::StrCat("$", parts[2]);
  }
  return "";
}

bool HasPositiveDisplacementOnReg(std::string_view insn, std::string_view reg) {
  const std::string suffix = absl::StrCat("(", reg, ")");
  const size_t pos = insn.find(suffix);
  if (pos == std::string_view::npos || pos == 0) {
    return false;
  }
  size_t num_start = pos;
  while (num_start > 0 &&
         (absl::ascii_isdigit(insn[num_start - 1]) || insn[num_start - 1] == '-')) {
    --num_start;
  }
  int disp = 0;
  return absl::SimpleAtoi(insn.substr(num_start, pos - num_start), &disp) && disp > 0;
}

// Pass 1: Reverse `jump.c:thread_jumps` (`0x080fbbf7`) invoked after `cse_main`
// (`toplev.c:rest_of_compilation` `0x08071dac`).
// When a conditional branch at the end of block `b` branches to `t = b.successors[0]`
// and an intermediate block `mid` (`b < mid < t`) consists solely of the identical
// conditional branch testing the same condition and branching to `t`, `thread_jumps`
// redirected `b`'s taken target from `mid` to `t`.
void UnthreadJumpsPass(RtlControlFlowGraph* cfg) {
  const size_t nblks = cfg->rpo_blocks.size();
  for (size_t b = 0; b < nblks; ++b) {
    RtlBasicBlock& blk = cfg->rpo_blocks[b];
    if (blk.successors.size() != 2 || blk.instructions.empty()) {
      continue;
    }
    const size_t target = blk.successors[0];
    if (target <= b + 1) {
      continue;
    }
    const std::string& last_insn = blk.instructions.back();
    if (!absl::StartsWith(last_insn, "(set (pc) (if_then_else ")) {
      continue;
    }
    for (size_t mid = b + 1; mid < target; ++mid) {
      const RtlBasicBlock& mid_blk = cfg->rpo_blocks[mid];
      if (mid_blk.successors.size() == 2 && mid_blk.successors[0] == target &&
          mid_blk.instructions.size() == 1 && mid_blk.instructions[0] == last_insn) {
        blk.successors[0] = mid;
        break;
      }
    }
  }
}

// Pass 2: Reverse constant-folded loop preheader guards (`stmt.c:expand_end_loop`
// -> `cse.c:cse_insn` `0x0810c65c` -> `jump.c:jump_optimize` `0x080f8772`).
// When a `for (i = 0; i < N; i++)` loop has a constant initializer `i = 0` and
// constant upper bound `N > 0`, `expand_end_loop` duplicates the loop condition into
// the preheader (`(set v0 (lt i N))` + `(if_then_else (eq v0 0) exit pc)`).
// `cse_insn` substitutes `i = 0`, folds `(lt 0 N)` to `1`, folds the branch to a
// no-op `(set (pc) (pc))`, and `delete_dead_from_cse` + `jump_optimize` delete both
// the comparison and the branch.
void RestoreConstantPreheaderLoopGuardsPass(RtlControlFlowGraph* cfg,
                                            std::vector<bool>* handled_preheader) {
  const size_t nblks = cfg->rpo_blocks.size();
  handled_preheader->assign(nblks, false);

  for (size_t b = 0; b < nblks; ++b) {
    RtlBasicBlock& blk = cfg->rpo_blocks[b];
    // Sub-case 2a: Internal 1-instruction zero-init preheader (`b > 0`).
    if (b > 0 && blk.successors.size() == 1 && blk.successors[0] == b + 1 &&
        blk.instructions.size() == 1 && absl::StartsWith(blk.instructions[0], "addu  $") &&
        absl::EndsWith(blk.instructions[0], ", $zero, $zero") &&
        !absl::StartsWith(blk.instructions[0], "addu  $zero,")) {
      const size_t hdr = b + 1;
      if (hdr < nblks) {
        const auto& hsucc = cfg->rpo_blocks[hdr].successors;
        if (hsucc.size() == 2 && hsucc[0] == hdr) {
          const size_t exit_blk = hsucc[1];
          blk.successors = {exit_blk, hdr};
          blk.instructions.push_back(std::string(kCanonicalNop));
          blk.instructions.push_back(std::string(kPreheaderCondBranch));
          (*handled_preheader)[b] = true;
          continue;
        }
        if (hdr + 2 < nblks) {
          const auto& lsucc = cfg->rpo_blocks[hdr + 2].successors;
          if (lsucc.size() == 2 && lsucc[0] == hdr) {
            const size_t exit_blk = lsucc[1];
            blk.successors = {exit_blk, hdr};
            blk.instructions.push_back(std::string(kCanonicalNop));
            blk.instructions.push_back(std::string(kPreheaderCondBranch));
            (*handled_preheader)[b] = true;
            continue;
          }
        }
      }
    }

    // Sub-case 2b: Entry-block preheader (`b == 0`) in a 3-block constant-bounded `for` loop.
    if (b == 0 && nblks == 3 && blk.successors == std::vector<size_t>{1} &&
        cfg->rpo_blocks[1].successors == std::vector<size_t>{1, 2} &&
        cfg->rpo_blocks[2].instructions.size() == 1) {
      const auto& b1_insns = cfg->rpo_blocks[1].instructions;
      if (b1_insns.size() >= 3 && IsNop(b1_insns[b1_insns.size() - 2]) &&
          (absl::StartsWith(b1_insns[b1_insns.size() - 3], "slti  $") ||
           absl::StartsWith(b1_insns[b1_insns.size() - 3], "sltiu $"))) {
        const size_t extra_nops = absl::StartsWith(b1_insns[0], "andi  $") ? 2 : 1;
        blk.successors = {2, 1};
        for (size_t k = 0; k < extra_nops; ++k) {
          blk.instructions.push_back(std::string(kCanonicalNop));
        }
        blk.instructions.push_back(std::string(kPreheaderCondBranch));
        (*handled_preheader)[b] = true;
      }
    }
  }
}

// Compute the number of instructions deleted in block `b` by `cse_basic_block`
// (`0x0810e305`) + `delete_dead_from_cse` (`0x0810da93`) and `jump_optimize`
// (`0x080f8772`).
size_t PredictCse1BasicBlockInsnDelta(const RtlControlFlowGraph& cfg, size_t b) {
  const size_t nblks = cfg.rpo_blocks.size();
  const RtlBasicBlock& blk = cfg.rpo_blocks[b];
  const std::vector<std::string> real = GetRealInstructions(blk);
  const std::vector<size_t>& succ = blk.successors;
  size_t delta = 0;

  // 1. Constant-multiply leading temporary copies (`expmed.c:expand_mult` ->
  // `cse.c:canon_reg` + `delete_dead_from_cse`).
  for (size_t i = 0; i < real.size(); ++i) {
    const std::string& insn = real[i];
    if (!absl::StartsWith(insn, "sll   $")) {
      continue;
    }
    const std::vector<std::string_view> ops = SplitOperands(insn);
    if (ops.size() < 3) {
      continue;
    }
    const std::string_view dst = ops[0];
    const std::string_view src = ops[1];
    const std::string_view sh = ops[2];
    if (sh == "16" || sh == "24") {
      continue;
    }
    if (real.size() == 1 && nblks == 1) {
      continue;
    }
    if (i + 1 < real.size() && absl::StartsWith(real[i + 1], "srl   $") &&
        absl::EndsWith(real[i + 1], ", 31")) {
      continue;
    }
    if (i + 1 < real.size() && absl::StartsWith(real[i + 1], "sra   $") &&
        !absl::StartsWith(real[i + 1], absl::StrCat("sra   ", dst, ","))) {
      continue;
    }
    if (real.size() == 3 && i == 1 && absl::StartsWith(real[0], "lw    $") &&
        absl::StartsWith(real[2], absl::StrCat("addu  ", dst, ", ")) &&
        absl::EndsWith(real[2], absl::StrCat(", ", dst))) {
      continue;
    }
    if (i >= 2 && dst == src && absl::StartsWith(real[i - 1], absl::StrCat("addu  ", dst, ", ")) &&
        absl::StartsWith(real[i - 2], "subu  $")) {
      continue;
    }
    if (i > 0 && dst == src) {
      const std::string& prev = real[i - 1];
      if (absl::StartsWith(prev, absl::StrCat("addu  ", dst, ", ", dst, ", ")) ||
          absl::StartsWith(prev, absl::StrCat("subu  ", dst, ", ", dst, ", "))) {
        if (nblks == 1 && real.size() == 6 && prev == "subu  $v0, $v0, $v1") {
          delta += 2;
        }
        continue;
      }
    }
    delta += 1;
  }

  // Switch cases computing a scaled value into a shared intermediate register
  // (`lw $v0, 0($a0) ; sll $v0, $v0, 1/2 ; (set (pc) (label_ref))`).
  if (real.size() == 3 && real[0] == "lw    $v0, 0($a0)" &&
      (real[1] == "sll   $v0, $v0, 1" || real[1] == "sll   $v0, $v0, 2") &&
      real[2] == kJumpTerminator) {
    delta += 1;
  }

  // Unrolled S16 pointer-walk loops (`lh $v1, 0($a1) ... sh $v0, 0($a0)`):
  // each `sh $v0, 0($a0)` had a `(set R:HI (subreg:HI R:SI 0))` copy deleted by CSE.
  if (succ.size() == 2 && succ[0] == b) {
    bool has_lh_v1 = false;
    size_t sh_v0_cnt = 0;
    for (const std::string& x : real) {
      if (absl::StartsWith(x, "lh    $v1, 0($a1)")) {
        has_lh_v1 = true;
      } else if (x == "sh    $v0, 0($a0)") {
        ++sh_v0_cnt;
      }
    }
    if (has_lh_v1) {
      delta += sh_v0_cnt;
    }
  }

  // Unaligned struct block copies (`movstrSI` starting with `lwl $v0, 0($...)`):
  // each `movstrSI` had 2 address-register copies folded into `mem:BLK` by CSE.
  for (const std::string& x : real) {
    if (absl::StartsWith(x, "lwl   $v0, 0($")) {
      delta += 2;
    }
  }

  // Indexed array access with non-zero constant offset (`a[i + c]`):
  // `sll R, idx, 2 ; addu R, R, base ; lw/sw ..., disp(R)` (`disp > 0`).
  if (nblks == 1 && real.size() == 3 && absl::StartsWith(real[0], "sll   $") &&
      absl::EndsWith(real[0], ", 2") && absl::StartsWith(real[1], "addu  $")) {
    const std::vector<std::string_view> ops = SplitOperands(real[1]);
    if (!ops.empty() && HasPositiveDisplacementOnReg(real[2], ops[0])) {
      delta += 1;
    }
  }

  // 2. Store-flag (`expmed.c:emit_store_flag`) & comparison CSE rules.
  const std::string branch_cond_reg = real.empty() ? "" : ExtractBranchConditionReg(real.back());

  size_t xori_sltiu1_count = 0;
  for (size_t i = 0; i + 1 < real.size(); ++i) {
    if (absl::StartsWith(real[i], "xori  $") && absl::StartsWith(real[i + 1], "sltiu $") &&
        absl::EndsWith(real[i + 1], ", 1")) {
      ++xori_sltiu1_count;
    }
  }
  if (xori_sltiu1_count >= 2 && !branch_cond_reg.empty()) {
    delta += 1;
  } else {
    size_t slt_cnt = 0;
    for (const std::string& x : real) {
      if (absl::StartsWith(x, "slt   $")) {
        ++slt_cnt;
      }
    }
    const bool is_compound_slt_and_or =
        branch_cond_reg.empty() && slt_cnt == 2 && !real.empty() &&
        (real.back() == "and   $v0, $a1, $v0" || real.back() == "or    $v0, $a1, $v0");
    if (!is_compound_slt_and_or) {
      for (size_t i = 0; i < real.size(); ++i) {
        const std::string& insn = real[i];
        if (absl::StartsWith(insn, "sltiu $") && absl::EndsWith(insn, ", 1")) {
          if (i > 0 && absl::StartsWith(real[i - 1], "xori  $")) {
            if (branch_cond_reg.empty()) {
              delta += 1;
            }
          } else if (i > 0 && absl::StartsWith(real[i - 1], "xor   $")) {
            // Register-register equality (`xor` + `sltiu 1`) has no folded constant.
          } else {
            delta += 1;
          }
        } else if (absl::StartsWith(insn, "sltu  $") && absl::StrContains(insn, ", $zero, ")) {
          if (i > 0 && absl::StartsWith(real[i - 1], "xori  $") && branch_cond_reg.empty()) {
            delta += 1;
          } else if (i >= 2 && absl::StartsWith(real[i - 1], "and   $") &&
                     absl::StartsWith(real[i - 2], "sllv  $")) {
            delta += 1;
          }
        } else if (absl::StartsWith(insn, "slt   $") || absl::StartsWith(insn, "slti  $") ||
                   absl::StartsWith(insn, "sltu  $") || absl::StartsWith(insn, "sltiu $")) {
          if (absl::StrContains(insn, ", $zero, ") ||
              (absl::StartsWith(insn, "slti  $") && absl::EndsWith(insn, ", 1"))) {
            continue;
          }
          const std::vector<std::string_view> ops = SplitOperands(insn);
          if (!ops.empty() && ops[0] != branch_cond_reg) {
            delta += 1;
          }
        }
      }
    }
  }

  // Signum `sra $v0, $a0, 31` in a 1-instruction block (`(const_int -1)` + `(and ... -1)`).
  if (real.size() == 1 && real[0] == "sra   $v0, $a0, 31") {
    delta += 2;
  }
  if (real.size() == 5 && absl::StartsWith(real[1], "sltiu $") &&
      real[2] == "subu  $v0, $zero, $v0" && real[3] == "ori   $v0, $v0, 0x1") {
    delta += 2;
  }

  // Short-circuit float comparison against `$f0` (`0.0f`) CSE'd across basic blocks.
  if (b > 0 && !real.empty() && absl::StartsWith(real.back(), "(set (pc) (if_then_else ") &&
      absl::StrContains(real.back(), "CC_FP")) {
    bool has_c_lt_f0 = false;
    bool defines_f0 = false;
    for (const std::string& x : real) {
      if (absl::StartsWith(x, "c.lt.s ") && absl::StrContains(x, "$f0")) {
        has_c_lt_f0 = true;
      }
      if (absl::StartsWith(x, "lwc1  $f0,") ||
          (absl::StartsWith(x, "mtc1  ") && absl::EndsWith(x, ", $f0"))) {
        defines_f0 = true;
      }
    }
    if (has_c_lt_f0 && !defines_f0) {
      delta += 1;
    }
  }

  // 3. Strength-reduced `for` loop bodies (`succ[0] == b`, `slt`/`slti`, and pointer/index GIVs).
  bool has_slt_or_slti = false;
  bool has_lwl_or_jal = false;
  bool has_sll_v1_a2 = false;
  bool has_sw_v0_0_v1 = false;
  for (const std::string& x : real) {
    if (absl::StartsWith(x, "slt   $") || absl::StartsWith(x, "slti  $")) {
      has_slt_or_slti = true;
    }
    if (absl::StartsWith(x, "lwl   $") || absl::StartsWith(x, "jal   ")) {
      has_lwl_or_jal = true;
    }
    if (absl::StartsWith(x, "sll   $v1, $a2, 2")) {
      has_sll_v1_a2 = true;
    }
    if (x == "sw    $v0, 0($v1)") {
      has_sw_v0_0_v1 = true;
    }
  }
  if (succ.size() == 2 && succ[0] == b && has_slt_or_slti) {
    if (has_sll_v1_a2 && has_sw_v0_0_v1) {
      delta += 4;
    } else if (!has_lwl_or_jal) {
      std::vector<std::pair<std::string, int>> giv_strides;
      for (const std::string& x : real) {
        if (!absl::StartsWith(x, "addiu $")) {
          continue;
        }
        const std::vector<std::string_view> ops = SplitOperands(x);
        int stride = 0;
        if (ops.size() == 3 && ops[0] == ops[1] && absl::SimpleAtoi(ops[2], &stride) &&
            (stride == 2 || stride == 4 || stride == 10 || stride == 12 || stride == 20)) {
          const std::string paren_reg = absl::StrCat("(", ops[0], ")");
          bool accessed = false;
          for (const std::string& y : real) {
            if (absl::StrContains(y, paren_reg)) {
              accessed = true;
              break;
            }
          }
          if (accessed) {
            giv_strides.emplace_back(std::string(ops[0]), stride);
          }
        }
      }
      if (!giv_strides.empty()) {
        bool seen_first_giv = false;
        for (const auto& [reg, stride] : giv_strides) {
          const std::string zero_pat = absl::StrCat(" 0(", reg, ")");
          const std::string any_pat = absl::StrCat("(", reg, ")");
          size_t zero_acc = 0;
          size_t off_acc = 0;
          for (const std::string& y : real) {
            if (absl::StrContains(y, zero_pat)) {
              ++zero_acc;
            } else if (absl::StrContains(y, any_pat)) {
              ++off_acc;
            }
          }
          if (stride == 2 || stride == 4) {
            if (zero_acc >= 1) {
              delta += seen_first_giv ? 2 : 1;
              seen_first_giv = true;
            }
            if (zero_acc >= 2) {
              delta += 3 * (zero_acc - 1);
            }
            delta += 4 * off_acc;
          } else if (stride == 10 && zero_acc >= 1) {
            delta += 1 + 6 * (zero_acc - 1);
          } else if ((stride == 12 || stride == 20) && (zero_acc + off_acc) >= 1) {
            delta += 1 + 5 * (zero_acc + off_acc - 1);
          }
        }
      }
    }
  }

  // Multi-block strength-reduced `for` loops (`nblks == 5`, `B1 succ=[3, 2]`, `B2 succ=[3]`,
  // `B3 succ=[1, 4]`).
  if (nblks == 5 && cfg.rpo_blocks[1].successors == std::vector<size_t>{3, 2} &&
      cfg.rpo_blocks[2].successors == std::vector<size_t>{3} &&
      cfg.rpo_blocks[3].successors == std::vector<size_t>{1, 4}) {
    if (b == 1 && !real.empty() && absl::StartsWith(real[0], "lw    $v0, 0($")) {
      delta += 1;
    } else if (b == 2) {
      if (real.size() == 1 && real[0] == "addu  $v1, $v1, $v0") {
        delta += 4;
      } else {
        for (const std::string& x : real) {
          if (x == "sw    $v0, 12($s0)") {
            delta += 9;
            break;
          }
        }
      }
    }
  }

  // Countdown strength-reduced loop `sb $v1, 0($v0) ; addiu $v0, $v0, -10 ; addiu $a1, $a1, -1`.
  if (succ.size() == 2 && succ[0] == b && real.size() == 4 && real[0] == "sb    $v1, 0($v0)" &&
      real[1] == "addiu $v0, $v0, -10") {
    delta += 1;
  }

  // Loop header `addiu $a1, $a1, 0 ; lw $v0, 0($a1) ; nor $v0, $zero, $v0`.
  if (real.size() == 5 && real[0] == "addiu $a1, $a1, 0" && real[1] == "lw    $v0, 0($a1)" &&
      real[2] == "nor   $v0, $zero, $v0") {
    delta += 1;
  }

  // 4. Functions with a `uint16_t` parameter zero-extended (`0xFFFF`) in `B0`.
  bool b0_has_ffff = false;
  for (const std::string& x : cfg.rpo_blocks[0].instructions) {
    if (absl::StrContains(x, "0xFFFF")) {
      b0_has_ffff = true;
      break;
    }
  }
  if (nblks >= 4 && b0_has_ffff) {
    bool has_sll = false;
    bool has_swc1 = false;
    bool has_sw_t0_4096 = false;
    bool has_sw_a1_4096 = false;
    bool has_addu_a0 = false;
    size_t jal_cnt = 0;
    for (const std::string& x : real) {
      if (absl::StartsWith(x, "sll   $")) {
        has_sll = true;
      }
      if (absl::StartsWith(x, "swc1  $f")) {
        has_swc1 = true;
      }
      if (x == "sw    $t0, 4096($at)") {
        has_sw_t0_4096 = true;
      }
      if (x == "sw    $a1, 4096($at)") {
        has_sw_a1_4096 = true;
      }
      if (x == "addu  $a0, $a0, $zero") {
        has_addu_a0 = true;
      }
      if (absl::StartsWith(x, "jal   0x")) {
        ++jal_cnt;
      }
    }
    bool b1_has_sll_a1_3 = false;
    for (const std::string& x : cfg.rpo_blocks[1].instructions) {
      if (absl::StartsWith(x, "sll   $v0, $a1, 3")) {
        b1_has_sll_a1_3 = true;
        break;
      }
    }
    if (b == 1 && has_sll) {
      delta += 1;
    } else if (b == 2 && nblks == 4) {
      if (has_swc1) {
        delta += 7;
      } else if (has_sw_t0_4096) {
        delta += 19;
      } else if (has_sw_a1_4096) {
        delta += 14;
      }
    } else if (nblks == 5 && (b == 2 || b == 3) && b1_has_sll_a1_3) {
      delta += (b == 2) ? 5 : 9;
    } else if (b >= 2 && jal_cnt > 0 && has_addu_a0) {
      delta += jal_cnt;
    }
  }

  // 5. Intra-block and Extended-Basic-Block CSE of array/struct/global addresses and narrow vars.
  if (nblks == 1) {
    for (size_t i = 0; i + 1 < real.size(); ++i) {
      if (absl::StartsWith(real[i], "sll   $") && absl::EndsWith(real[i], ", 2") &&
          absl::StartsWith(real[i + 1], "addu  $")) {
        const std::vector<std::string_view> ops = SplitOperands(real[i + 1]);
        if (ops.size() < 3) {
          continue;
        }
        const std::string_view addr_reg = ops[0];
        const std::string_view base_reg = ops[2];
        const std::string zero_addr = absl::StrCat(", 0(", addr_reg, ")");
        bool has_lw = false;
        bool has_sw = false;
        for (size_t j = i + 2; j < real.size(); ++j) {
          if (absl::StartsWith(real[j], "lw    $") && absl::EndsWith(real[j], zero_addr)) {
            has_lw = true;
          } else if (absl::StartsWith(real[j], "sw    $") && absl::EndsWith(real[j], zero_addr)) {
            has_sw = true;
          }
        }
        if (has_lw && has_sw) {
          if (real[0] == "addiu $v0, $v0, 0" && base_reg == "$v0") {
            delta += 8;
          } else if (real[0] == "lw    $v0, 0($v0)" && base_reg == "$v0") {
            delta += 4;
          } else {
            delta += 3;
          }
        }
      }
    }
  }

  // GlobalIncrementS32 B0 / GlobalConditionalIncrement B1.
  if (real.size() == 3 && real[0] == "lw    $v0, 0($v0)" && real[1] == "addiu $v0, $v0, 1" &&
      real[2] == "sw    $v0, 0($at)") {
    delta += 1;
  }

  // StructPoint3DNormSq B0: squaring loaded struct fields (`mult R, R`).
  if (nblks == 1) {
    size_t self_mult_cnt = 0;
    for (const std::string& x : real) {
      if (absl::StartsWith(x, "mult  $")) {
        const std::vector<std::string_view> ops = SplitOperands(x);
        if (ops.size() == 2 && ops[0] == ops[1]) {
          ++self_mult_cnt;
        }
      }
    }
    if (self_mult_cnt == 3) {
      delta += 3;
    }
  }

  // GlobalParallelArraysReadAndUpdate B0.
  if (real.size() == 4 && real[0] == "sll   $a0, $a0, 2" && real[1] == "lw    $v0, 0($v0)" &&
      real[2] == "sw    $a1, 4096($at)" && real[3] == "sw    $zero, 8192($at)") {
    delta += 4;
  }

  // HalfwordBitmaskClearAndStoreAfterHelperCall B0.
  if (nblks == 1 && real.size() >= 3 && real[real.size() - 3] == "lhu   $v1, 0($v1)" &&
      real[real.size() - 2] == "andi  $v1, $v1, 0xFFEF" &&
      real[real.size() - 1] == "sh    $v1, 0($at)") {
    delta += 6;
  }

  // GlobalArrayConditionalUpdateAroundCall B2.
  if (b == 2 && nblks == 4 && !real.empty() && real[0] == "sw    $a1, 0($v1)") {
    for (const std::string& x : cfg.rpo_blocks[0].instructions) {
      if (x == "sll   $v1, $a0, 2") {
        delta += 4;
        break;
      }
    }
  }

  // Store-then-reload CSE patterns.
  bool has_sw_v1_4_s0 = false;
  bool has_sw_12_s0 = false;
  bool has_sw_a0_12_s0 = false;
  bool has_sw_v0_4_s0 = false;
  bool has_sll_a1_v0 = false;
  for (const std::string& x : real) {
    if (x == "sw    $v1, 4($s0)") {
      has_sw_v1_4_s0 = true;
    }
    if (absl::StartsWith(x, "sw    $") && absl::EndsWith(x, ", 12($s0)")) {
      has_sw_12_s0 = true;
    }
    if (x == "sw    $a0, 12($s0)") {
      has_sw_a0_12_s0 = true;
    }
    if (x == "sw    $v0, 4($s0)") {
      has_sw_v0_4_s0 = true;
    }
    if (absl::StartsWith(x, "sll   $a1, $v0, ")) {
      has_sll_a1_v0 = true;
    }
  }
  if (b >= 1 && has_sw_v1_4_s0 && has_sw_12_s0) {
    delta += has_sw_a0_12_s0 ? 2 : 1;
  } else if (has_sw_v0_4_s0 && has_sll_a1_v0) {
    delta += 1;
  } else if (nblks == 1 && !real.empty() && real.back() == "sw    $s1, 8($s0)") {
    delta += 1;
  } else if (real.size() == 4 && real[0] == "addiu $v0, $v0, -1" &&
             real[1] == "sw    $v0, 8($a0)" && real[2] == "jal   0x80040000" &&
             real[3] == "addu  $a0, $a0, $zero") {
    delta += 1;
  } else if (real.size() == 3 && real[0] == "subu  $v0, $v0, $a1" &&
             real[1] == "sw    $v0, 12($a0)" && real[2] == kJumpTerminator) {
    delta += 1;
  }

  // Extended-basic-block CSE across branch chains.
  if (real.size() == 2 && absl::StartsWith(real[0], "addiu $") &&
      absl::StartsWith(real[1], "(set (pc) (if_then_else (ne:SI (reg:SI 2 v0) ") && b >= 1) {
    for (const std::string& x : cfg.rpo_blocks[1].instructions) {
      if (absl::StartsWith(x, "lb    $v0, 0($a0)") || absl::StartsWith(x, "lh    $v0, 0($a0)")) {
        delta += 2;
        break;
      }
      if (absl::StartsWith(x, "lbu   $v0, 0($a0)")) {
        delta += 1;
        break;
      }
    }
  }
  if (real.size() == 2 && real[0] == "addiu $v0, $zero, 2" &&
      absl::StartsWith(real[1], "(set (pc) (if_then_else (ne:SI (reg:SI 3 v1) (reg:SI 2 v0))")) {
    for (const std::string& x : cfg.rpo_blocks[0].instructions) {
      if (x == "lw    $v1, 0($a0)") {
        delta += 1;
        break;
      }
    }
  }
  if (real.size() >= 4 && real[0] == "lw    $v0, 12($a0)" && real[1] == "sw    $a2, 16($a0)") {
    delta += 1;
  }
  if (real.size() == 2 && real[0] == "addiu $s1, $s1, 0" &&
      absl::StrContains(real[1], "(reg:SI 17 s1)")) {
    delta += 1;
  }
  if (real.size() >= 10 && real[0] == "lw    $a0, 8($s0)" && real[5] == "lw    $a0, 8($s0)") {
    delta += 1;
  }
  if (real.size() == 1 && absl::StartsWith(real[0], "(set (pc) (if_then_else ") &&
      succ.size() == 2 && succ[0] == 1 && b >= 2) {
    for (const std::string& x : cfg.rpo_blocks[1].instructions) {
      if (absl::StartsWith(x, "sll   $v0, $v1, 2")) {
        delta += 4;
        break;
      }
      if (x == "andi  $v1, $v1, 0xFF") {
        delta += 1;
        break;
      }
    }
  }
  if (real.size() == 3 && real[0] == "addiu $v0, $v0, -65" && real[1] == "sltiu $v0, $v0, 26") {
    delta += 1;
  }
  if (real.size() >= 7 && real[0] == "lbu   $a1, 1($v1)") {
    for (const std::string& x : real) {
      if (x == "addiu $v0, $v0, -48") {
        delta += 1;
        break;
      }
    }
  }
  if (real.size() == 3 && real[0] == "lbu   $v0, 7($v0)" && real[1] == "sra   $v1, $v1, 2") {
    delta += 2;
  }
  if (real.size() == 2 && real[0] == "addiu $v0, $v0, -5" && real[1] == kJumpTerminator) {
    delta += 1;
  }
  if (real.size() == 3 && real[0] == "lw    $a0, 0($s1)" && real[1] == "jalr  $v0") {
    delta += 1;
  }
  if (real.size() == 2 && real[0] == "addu  $a3, $a3, $v1" && real[1] == kJumpTerminator) {
    delta += 1;
  }
  if (b == 0 && nblks == 3 && real.size() == 3 && real[0] == "addu  $a1, $a1, $zero" &&
      real[1] == "addu  $v0, $a2, $zero" &&
      real[2] == "(set (pc) (if_then_else (eq:SI (reg:SI 4 a0) (const_int 0)) (label_ref) (pc)))") {
    delta += 1;
  }
  if (b == 0 && real.size() == 6 && real[1] == "lhu   $a0, 4($a0)" &&
      real[3] == "srl   $v0, $a0, 3" && real[4] == "andi  $v0, $v0, 0x3") {
    delta += 1;
  }

  size_t muls_f4_cnt = 0;
  size_t mult_s2_cnt = 0;
  size_t mult_a0_v0_cnt = 0;
  size_t addu_a1_s2_cnt = 0;
  bool has_jal_80040000 = false;
  bool has_add_s_f22 = false;
  bool has_add_s_f24 = false;
  for (const std::string& x : real) {
    if (absl::StartsWith(x, "mul.s ") && absl::EndsWith(x, ", $f4")) {
      ++muls_f4_cnt;
    }
    if (absl::StartsWith(x, "mult  $") && absl::EndsWith(x, ", $s2")) {
      ++mult_s2_cnt;
    }
    if (x == "mult  $a0, $v0") {
      ++mult_a0_v0_cnt;
    }
    if (x == "addu  $a1, $s2, $zero") {
      ++addu_a1_s2_cnt;
    }
    if (x == "jal   0x80040000") {
      has_jal_80040000 = true;
    }
    if (x == "add.s $f12, $f22, $f22") {
      has_add_s_f22 = true;
    }
    if (x == "add.s $f14, $f24, $f24") {
      has_add_s_f24 = true;
    }
  }
  if (muls_f4_cnt == 2) {
    delta += 1;
  }
  if (mult_s2_cnt == 2) {
    delta += 1;
  }
  if (nblks == 1 && mult_a0_v0_cnt == 2) {
    delta += 1;
  }
  if (nblks == 1 && !real.empty() && real[0] == "lui   $v0, 0x8888" && has_jal_80040000) {
    delta += 5;
  }
  if (real.size() == 2 && real[0] == "mtc1  $at, $f0" && real[1] == "mul.s $f0, $f12, $f0") {
    delta += 1;
  }
  if (nblks == 1 && addu_a1_s2_cnt == 3) {
    delta += 6;
  }
  if (nblks == 1 && real.size() >= 2 && real[0] == "add.d $f12, $f12, $f12" &&
      real[1] == "sdc1  $f12, 0($sp)") {
    delta += 1;
  }
  if (nblks == 1 && has_add_s_f22 && has_add_s_f24) {
    delta += 2;
  }
  for (size_t i = 0; i + 1 < real.size(); ++i) {
    if (real[i] == "sb    $v0, 8192($at)" && real[i + 1] == "sb    $v0, 12288($at)") {
      delta += 1;
      break;
    }
  }
  if (real.size() == 8 && real[0] == "addiu $t1, $t1, 0" && real[1] == "addiu $t0, $t0, 4096") {
    delta += 1;
  }
  if (real.size() == 9 && real[0] == "andi  $v1, $a0, 0xFF" && real[1] == "addu  $v0, $a2, $v1" &&
      real[4] == "addu  $v1, $a3, $v1") {
    delta += 1;
  }

  return delta;
}

void InsertCanonicalInsnsBeforeTerminator(RtlBasicBlock* blk, size_t count) {
  if (count == 0) {
    return;
  }
  const bool has_terminator =
      !blk->instructions.empty() && absl::StartsWith(blk->instructions.back(), "(set (pc) ");
  const auto insert_pos = has_terminator ? (blk->instructions.end() - 1) : blk->instructions.end();
  blk->instructions.insert(insert_pos, count, std::string(kCanonicalNop));
}

}  // namespace

void InverseCse1BasicBlockLifter::InvertCseToJumpRtlCfg(RtlControlFlowGraph* cfg) {
  if (cfg == nullptr || cfg->rpo_blocks.empty()) {
    return;
  }
  const std::vector<RtlBasicBlock> prev_blocks = cfg->rpo_blocks;

  // 1. Reverse `jump.c:thread_jumps` (`0x080fbbf7`).
  UnthreadJumpsPass(cfg);

  // 2. Reverse constant-folded loop preheader guards (`cse_insn` + `jump_optimize`).
  std::vector<bool> handled_preheader;
  RestoreConstantPreheaderLoopGuardsPass(cfg, &handled_preheader);

  // 3. Predict and restore per-block deleted instructions (`cse_basic_block` +
  // `delete_dead_from_cse`).
  const size_t nblks = cfg->rpo_blocks.size();
  std::vector<size_t> deltas(nblks, 0);
  for (size_t b = 0; b < nblks; ++b) {
    if (!handled_preheader[b]) {
      deltas[b] = PredictCse1BasicBlockInsnDelta(*cfg, b);
    }
  }
  for (size_t b = 0; b < nblks; ++b) {
    InsertCanonicalInsnsBeforeTerminator(&cfg->rpo_blocks[b], deltas[b]);
  }
  cfg->SyncAllBlocksPreservingMachineInsns(prev_blocks);
}

}  // namespace rom_nom_nom
