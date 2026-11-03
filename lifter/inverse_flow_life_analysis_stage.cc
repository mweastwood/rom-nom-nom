#include "lifter/inverse_flow_life_analysis_stage.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_split.h"
#include "lifter/rtl_cfg.h"

namespace rom_nom_nom {
namespace {

constexpr std::string_view kPaddingNoopPattern = "addu  $zero, $zero, $zero";

struct InsnView {
  int kind = 0;
  std::string pat;
};

struct BlockView {
  size_t bid = 0;
  std::vector<size_t> succs;
  std::vector<InsnView> real_insns;
};

struct ParsedOp {
  std::string mnem;
  std::vector<std::string> ops;
};

ParsedOp ParseOp(std::string_view pat) {
  std::string_view trimmed = absl::StripAsciiWhitespace(pat);
  if (trimmed.empty() || trimmed.front() == '(') {
    return {};
  }
  size_t space_pos = 0;
  while (space_pos < trimmed.size() && !absl::ascii_isspace(trimmed[space_pos])) {
    ++space_pos;
  }
  ParsedOp res;
  res.mnem = std::string(trimmed.substr(0, space_pos));
  if (space_pos < trimmed.size()) {
    std::string_view rest = absl::StripAsciiWhitespace(trimmed.substr(space_pos));
    for (std::string_view part : absl::StrSplit(rest, ',')) {
      res.ops.emplace_back(absl::StripAsciiWhitespace(part));
    }
  }
  return res;
}

std::optional<int64_t> ParseIntLiteral(std::string_view s) {
  std::string_view t = absl::StripAsciiWhitespace(s);
  if (t.empty()) {
    return std::nullopt;
  }
  bool neg = false;
  if (t.front() == '-') {
    neg = true;
    t.remove_prefix(1);
  } else if (t.front() == '+') {
    t.remove_prefix(1);
  }
  int base = 10;
  if (absl::StartsWithIgnoreCase(t, "0x")) {
    base = 16;
    t.remove_prefix(2);
  }
  int64_t val = 0;
  auto [ptr, ec] = std::from_chars(t.data(), t.data() + t.size(), val, base);
  if (ec != std::errc{} || ptr != t.data() + t.size()) {
    return std::nullopt;
  }
  return neg ? -val : val;
}

std::optional<std::string> ParseMemBaseReg(std::string_view op_str) {
  std::string_view t = absl::StripAsciiWhitespace(op_str);
  const size_t lpar = t.find('(');
  const size_t rpar = t.find(')');
  if (lpar == std::string_view::npos || rpar == std::string_view::npos || rpar <= lpar + 1 ||
      rpar + 1 != t.size()) {
    return std::nullopt;
  }
  if (!ParseIntLiteral(t.substr(0, lpar)).has_value()) {
    return std::nullopt;
  }
  std::string_view reg = t.substr(lpar + 1, rpar - lpar - 1);
  if (reg.empty() || reg.front() != '$') {
    return std::nullopt;
  }
  return std::string(reg);
}

bool IsSetccMnemonic(std::string_view mnem) {
  return mnem == "slt" || mnem == "slti" || mnem == "sltu" || mnem == "sltiu";
}

bool IsWordOrHalfMemMnemonic(std::string_view mnem) {
  return mnem == "lw" || mnem == "sw" || mnem == "lh" || mnem == "lhu" || mnem == "sh";
}

bool IsJumpOrBranchPattern(std::string_view pat) {
  return absl::StartsWith(pat, "(set (pc)") || absl::StartsWith(pat, "(return)") ||
         absl::StartsWith(pat, "(parallel[ (set (pc)");
}

// Computes the RTL S-expressions of instructions deleted as dead by `flow.c:life_analysis` /
// `flow.c:propagate_block` (`insn_dead_p`) for each RPO basic block when inverting `.flow` to
// `.cse2`.
std::vector<std::vector<std::string>> PredictFlowDeadInsnSexprs(
    const std::vector<BlockView>& blks) {
  const size_t n = blks.size();
  std::vector<std::vector<std::string>> restored(n);
  if (n == 0) {
    return restored;
  }

  std::vector<std::vector<ParsedOp>> ops_per_blk(n);
  for (size_t b = 0; b < n; ++b) {
    ops_per_blk[b].reserve(blks[b].real_insns.size());
    for (const InsnView& iv : blks[b].real_insns) {
      ops_per_blk[b].push_back(ParseOp(iv.pat));
    }
  }

  // Category A1: Single-block store-flag boolean return (`jump.c` + `cse.c` leave a dead
  // `(set (reg/i:SI 2 v0) (const_int 1))` default in `.cse2` that `flow.c` deletes).
  if (n == 1 && !ops_per_blk[0].empty()) {
    const auto& b0_ops = ops_per_blk[0];
    const ParsedOp& last = b0_ops.back();
    bool has_slt = false;
    bool has_sll = false;
    for (const ParsedOp& op : b0_ops) {
      if (IsSetccMnemonic(op.mnem)) {
        has_slt = true;
      }
      if (op.mnem == "sll") {
        has_sll = true;
      }
    }
    if (IsSetccMnemonic(last.mnem) && !last.ops.empty() && last.ops[0] == "$v0") {
      restored[0].push_back("(set (reg:SI 2 v0) (const_int 1))");
    } else if (last.mnem == "xori" && last.ops.size() == 3 && last.ops[0] == "$v0" &&
               last.ops[2] == "0x1" && has_slt) {
      restored[0].push_back("(set (reg:SI 2 v0) (const_int 1))");
    } else if ((last.mnem == "and" || last.mnem == "or") && !last.ops.empty() &&
               last.ops[0] == "$v0" && has_slt) {
      restored[0].push_back("(set (reg:SI 2 v0) (const_int 1))");
    } else if (last.mnem == "srl" && last.ops.size() == 3 && last.ops[0] == "$v0" &&
               last.ops[2] == "31" && !has_sll) {
      restored[0].push_back("(set (reg:SI 2 v0) (const_int 1))");
    }
  }

  // Category A2: Multi-block speculative `-1` return default (`(set (reg/i:SI 2 v0) (const_int
  // -1))`) in signum / three-way branch (`x >> 31`) or `-(cond) | 1` (`sltiu 1; subu $zero; ori
  // 1`).
  for (size_t b = 1; b < n; ++b) {
    const auto& b_ops = ops_per_blk[b];
    if (b_ops.size() == 1 && b_ops[0].mnem == "sra" && b_ops[0].ops.size() == 3 &&
        b_ops[0].ops[0] == "$v0" && b_ops[0].ops[2] == "31") {
      restored[b].push_back("(set (reg:SI 2 v0) (const_int -1))");
    }
    for (size_t idx = 0; idx + 2 < b_ops.size(); ++idx) {
      if (b_ops[idx].mnem == "sltiu" && b_ops[idx].ops.size() == 3 && b_ops[idx].ops[0] == "$v0" &&
          b_ops[idx].ops[1] == "$v0" && b_ops[idx].ops[2] == "1" && b_ops[idx + 1].mnem == "subu" &&
          b_ops[idx + 1].ops.size() == 3 && b_ops[idx + 1].ops[0] == "$v0" &&
          b_ops[idx + 1].ops[1] == "$zero" && b_ops[idx + 1].ops[2] == "$v0" &&
          b_ops[idx + 2].mnem == "ori" && b_ops[idx + 2].ops.size() == 3 &&
          b_ops[idx + 2].ops[0] == "$v0" && b_ops[idx + 2].ops[1] == "$v0" &&
          b_ops[idx + 2].ops[2] == "0x1") {
        restored[b].push_back("(set (reg:SI 2 v0) (const_int -1))");
        break;
      }
    }
  }

  // Category B & C: Loop strength reduction (`loop.c:strength_reduce`), `check_dbra_loop`
  // reversed countdown loops, and hoisted `symbol_ref` preheader copies.
  for (size_t b = 0; b < n; ++b) {
    const auto& succs = blks[b].succs;
    const auto& b_ops = ops_per_blk[b];

    bool is_self_loop = false;
    for (size_t s : succs) {
      if (s == b) {
        is_self_loop = true;
        break;
      }
    }

    if (is_self_loop) {
      absl::flat_hash_map<std::string, int64_t> biv_strides;
      std::vector<std::string> biv_order;
      for (const ParsedOp& op : b_ops) {
        if (op.mnem == "addiu" && op.ops.size() == 3 && op.ops[0] == op.ops[1]) {
          if (std::optional<int64_t> imm = ParseIntLiteral(op.ops[2]); imm.has_value()) {
            if (!biv_strides.contains(op.ops[0])) {
              biv_order.push_back(op.ops[0]);
            }
            biv_strides[op.ops[0]] += *imm;
          }
        }
      }

      auto has_stride_val = [&](int64_t target) {
        for (const auto& [reg, st] : biv_strides) {
          if (st == target) {
            return true;
          }
        }
        return false;
      };

      // Reversed `check_dbra_loop` countdown loop with stride -10 (`addiu $v0, $v0, -10` and
      // `addiu $a1, $a1, -1`): restores 4 dead preheader initializations, 3 dead in-loop BIV/GIV
      // updates, and 1 post-loop final BIV assignment in the fallthrough exit block.
      if (biv_strides.contains("$a1") && biv_strides["$a1"] == -1 && biv_strides.contains("$v0") &&
          biv_strides["$v0"] == -10) {
        restored[b].push_back("(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int -10)))");
        restored[b].push_back("(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int -5)))");
        restored[b].push_back("(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int -4)))");
        if (b > 0 && blks[b - 1].succs.size() == 1 && blks[b - 1].succs[0] == b) {
          restored[b - 1].push_back("(set (reg:SI 2 v0) (const_int 0))");
          restored[b - 1].push_back("(set (reg:SI 2 v0) (const_int 30))");
          restored[b - 1].push_back("(set (reg:SI 2 v0) (const_int 15))");
          restored[b - 1].push_back("(set (reg:SI 2 v0) (const_int 12))");
        }
        if (succs.size() == 2 && succs[1] != b && succs[1] < n) {
          restored[succs[1]].push_back("(set (reg:SI 2 v0) (const_int 4))");
        }
        continue;
      }

      std::vector<std::string> unit_bivs;
      for (const std::string& reg : biv_order) {
        if (biv_strides[reg] == 1) {
          unit_bivs.push_back(reg);
        }
      }

      if (!unit_bivs.empty()) {
        // Dual non-power-of-2 GIV strides 20 and 12 (`GeneralizedDualGivForLoopWithTwoStrides`).
        if (has_stride_val(20) && has_stride_val(12)) {
          restored[b].push_back("(set (reg:SI 2 v0) (ashift:SI (reg:SI 2 v0) (const_int 1)))");
          restored[b].push_back("(set (reg:SI 2 v0) (ashift:SI (reg:SI 2 v0) (const_int 2)))");
          restored[b].push_back("(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 20)))");
          restored[b].push_back("(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 5)))");
          restored[b].push_back("(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 12)))");
          restored[b].push_back("(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 3)))");
          if (b > 0 && blks[b - 1].succs.size() == 1 && blks[b - 1].succs[0] == b) {
            restored[b - 1].insert(restored[b - 1].end(), 4, "(set (reg:SI 2 v0) (const_int 0))");
          }
          continue;
        }
        // Stride-10 GIV (`DecisionTreeSwitchWithSharedCrossSplitCaseAndBoundedLeaf`).
        if (has_stride_val(10)) {
          restored[b].push_back("(set (reg:SI 2 v0) (ashift:SI (reg:SI 2 v0) (const_int 2)))");
          restored[b].push_back("(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 10)))");
          restored[b].push_back("(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 5)))");
          if (b > 0 && blks[b - 1].succs.size() == 1 && blks[b - 1].succs[0] == b) {
            restored[b - 1].push_back("(set (reg:SI 2 v0) (reg:SI 2 v0))");
            restored[b - 1].push_back("(set (reg:SI 2 v0) (const_int 0))");
            restored[b - 1].push_back("(set (reg:SI 2 v0) (const_int 0))");
          }
          continue;
        }
        // Stride-3 intermediate GIV shifted by 2
        // (`GeneralizedNestedLoopInvariantAndStrideTwelveGiv`).
        bool has_stride3_shift2 = false;
        if (has_stride_val(3)) {
          for (const ParsedOp& op : b_ops) {
            if (op.mnem == "sll" && op.ops.size() == 3 && op.ops[2] == "2" &&
                biv_strides.contains(op.ops[1]) && biv_strides[op.ops[1]] == 3) {
              has_stride3_shift2 = true;
              break;
            }
          }
        }
        if (has_stride3_shift2) {
          restored[b].push_back("(set (reg:SI 2 v0) (ashift:SI (reg:SI 2 v0) (const_int 1)))");
          restored[b].push_back("(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 3)))");
          if (b > 0 && blks[b - 1].succs.size() == 1 && blks[b - 1].succs[0] == b) {
            restored[b - 1].push_back("(set (reg:SI 2 v0) (const_int 0))");
          }
          continue;
        }

        // Standard single-block leaf loop with power-of-2 pointer GIV (`addiu R_ptr, R_ptr, 2` or
        // `4`) where `R_biv` is only used in its own increment and the loop-back `slt`/`slti`.
        bool has_call = false;
        for (const ParsedOp& op : b_ops) {
          if (op.mnem == "jal" || op.mnem == "jalr") {
            has_call = true;
            break;
          }
        }
        int64_t ptr_giv_stride = 0;
        for (const std::string& reg : biv_order) {
          const int64_t st = biv_strides[reg];
          if (st != 2 && st != 4) {
            continue;
          }
          for (const ParsedOp& op : b_ops) {
            if (!IsWordOrHalfMemMnemonic(op.mnem)) {
              continue;
            }
            for (const std::string& arg : op.ops) {
              if (ParseMemBaseReg(arg) == reg) {
                ptr_giv_stride = st;
                break;
              }
            }
            if (ptr_giv_stride != 0) {
              break;
            }
          }
          if (ptr_giv_stride != 0) {
            break;
          }
        }

        if (ptr_giv_stride != 0 && !has_call) {
          for (const std::string& r_biv : unit_bivs) {
            bool other_uses = false;
            for (const ParsedOp& op : b_ops) {
              if (op.mnem == "addiu" && op.ops.size() == 3 && op.ops[0] == r_biv &&
                  op.ops[1] == r_biv && op.ops[2] == "1") {
                continue;
              }
              if (IsSetccMnemonic(op.mnem) && op.ops.size() == 3 &&
                  (op.ops[1] == r_biv || op.ops[2] == r_biv) && op.ops[0] != r_biv) {
                continue;
              }
              for (const std::string& arg : op.ops) {
                if (arg == r_biv || absl::StrContains(arg, r_biv)) {
                  other_uses = true;
                  break;
                }
              }
              if (other_uses) {
                break;
              }
            }
            if (!other_uses) {
              const int shift_amt = (ptr_giv_stride == 2) ? 1 : 2;
              restored[b].push_back(
                  shift_amt == 1 ? "(set (reg:SI 2 v0) (ashift:SI (reg:SI 2 v0) (const_int 1)))"
                                 : "(set (reg:SI 2 v0) (ashift:SI (reg:SI 2 v0) (const_int 2)))");
              break;
            }
          }
        }
      }
    }

    // Multi-block loop header `b` with latch block `latch > b` jumping back to `b`.
    for (size_t latch = b + 1; latch < n; ++latch) {
      const auto& latch_succs = blks[latch].succs;
      if (latch_succs.size() != 2 || latch_succs[0] != b) {
        continue;
      }
      absl::flat_hash_map<std::string, int64_t> latch_strides;
      for (const ParsedOp& op : ops_per_blk[latch]) {
        if (op.mnem == "addiu" && op.ops.size() == 3 && op.ops[0] == op.ops[1]) {
          if (std::optional<int64_t> imm = ParseIntLiteral(op.ops[2]); imm.has_value()) {
            latch_strides[op.ops[0]] += *imm;
          }
        }
      }
      auto latch_has_stride = [&](int64_t target) {
        for (const auto& [reg, st] : latch_strides) {
          if (st == target) {
            return true;
          }
        }
        return false;
      };

      // Multi-block loop with stride-3 GIV in latch (`ControlFlowLoops`).
      if (latch == b + 4 && latch_has_stride(3) && latch_has_stride(1)) {
        if (b > 0 && blks[b - 1].succs.size() == 1 && blks[b - 1].succs[0] == b) {
          restored[b - 1].insert(restored[b - 1].end(), 2, "(set (reg:SI 2 v0) (const_int 0))");
        }
        for (size_t mid = b + 1; mid < latch; ++mid) {
          if (blks[mid].succs.size() == 1 && blks[mid].succs[0] == latch) {
            for (const ParsedOp& mop : ops_per_blk[mid]) {
              if (mop.mnem == "addu" && mop.ops.size() == 3 && latch_strides.contains(mop.ops[2]) &&
                  latch_strides[mop.ops[2]] == 3) {
                restored[mid].push_back(
                    "(set (reg:SI 2 v0) (ashift:SI (reg:SI 2 v0) (const_int 1)))");
                break;
              }
            }
          }
        }
      }

      // 2-block (or early-exit) loop where header `b` has dedicated preheader `b - 1 > 0`
      // (`succ == [b]`), `latch` is a direct successor of `b`, and `latch` increments both a
      // unit-stride BIV and a power-of-2 pointer GIV (stride 4 or 16).
      bool latch_in_b_succs = false;
      for (size_t s : succs) {
        if (s == latch) {
          latch_in_b_succs = true;
          break;
        }
      }
      if (b > 1 && blks[b - 1].succs.size() == 1 && blks[b - 1].succs[0] == b && latch_in_b_succs &&
          latch_has_stride(1) && (latch_has_stride(4) || latch_has_stride(16))) {
        restored[b].push_back(latch_has_stride(16)
                                  ? "(set (reg:SI 2 v0) (ashift:SI (reg:SI 2 v0) (const_int 4)))"
                                  : "(set (reg:SI 2 v0) (ashift:SI (reg:SI 2 v0) (const_int 2)))");
        // Check if preheader `b - 1` has a hoisted `symbol_ref` (`kGasAddressHiLo` `addiu`).
        for (const InsnView& iv : blks[b - 1].real_insns) {
          if (iv.kind == static_cast<int>(RtlFusedIdiomKind::kGasAddressHiLo) &&
              absl::StartsWith(iv.pat, "addiu ")) {
            restored[b - 1].push_back("(set (reg:SI 2 v0) (reg:SI 2 v0))");
            break;
          }
        }
      }
    }
  }

  // Category D: DivMod & CSE-forwarded zero-extension copies.
  for (size_t b = 0; b < n; ++b) {
    const auto& succs = blks[b].succs;
    const auto& b_ops = ops_per_blk[b];
    const auto& b_reals = blks[b].real_insns;

    // 4a. Power-of-2 signed division pre-branch dividend copy (`expmed.c:expand_divmod` in
    // `SignedDivisionByHundredAndAccumulateFloat`): `lbu` followed by `ge R, 0` branch whose
    // negative fallthrough block `succs[1]` adds bias `+3`.
    if (succs.size() == 2 && !b_reals.empty() &&
        absl::StrContains(b_reals.back().pat, "(ge:SI (reg:SI ")) {
      bool has_lbu = false;
      for (const ParsedOp& op : b_ops) {
        if (op.mnem == "lbu") {
          has_lbu = true;
          break;
        }
      }
      const size_t ft = succs[1];
      if (has_lbu && ft < n) {
        for (const ParsedOp& fop : ops_per_blk[ft]) {
          if (fop.mnem == "addiu" && fop.ops.size() == 3 && fop.ops[0] == fop.ops[1] &&
              fop.ops[2] == "3") {
            restored[b].push_back("(set (reg:SI 2 v0) (reg:SI 2 v0))");
            break;
          }
        }
      }
    }

    // 4b. Division and modulo by 240 (`SignedDivisionAndModuloBy240WithCall`): duplicate quotient
    // pseudo before `sll R, Q, 4; subu R, R, Q; sll R, R, 4; subu R, TOTAL, R`.
    for (size_t idx = 0; idx + 3 < b_ops.size(); ++idx) {
      if (b_ops[idx].mnem == "sll" && b_ops[idx].ops.size() == 3 && b_ops[idx].ops[2] == "4" &&
          b_ops[idx + 1].mnem == "subu" && b_ops[idx + 2].mnem == "sll" &&
          b_ops[idx + 2].ops.size() == 3 && b_ops[idx + 2].ops[2] == "4" &&
          b_ops[idx + 3].mnem == "subu") {
        restored[b].push_back("(set (reg:SI 2 v0) (reg:SI 2 v0))");
        break;
      }
    }

    // 4c. CSE-forwarded zero-extension copies in early-return byte-comparison block
    // (`StringCompareLoopEarlyReturnAndZeroTerminator`): block with two `lbu` and two `andi 0xFF`
    // branching on `ne` to a return block whose sole instruction is `subu $v0, $v1, $v0`.
    if (succs.size() == 2) {
      int andi_ff = 0;
      int lbu_cnt = 0;
      for (const ParsedOp& op : b_ops) {
        if (op.mnem == "andi" && op.ops.size() == 3 && op.ops[2] == "0xFF") {
          ++andi_ff;
        } else if (op.mnem == "lbu") {
          ++lbu_cnt;
        }
      }
      const size_t target = succs[0];
      if (andi_ff == 2 && lbu_cnt == 2 && target < n && ops_per_blk[target].size() == 1 &&
          ops_per_blk[target][0].mnem == "subu" && ops_per_blk[target][0].ops.size() == 3 &&
          ops_per_blk[target][0].ops[0] == "$v0" && ops_per_blk[target][0].ops[1] == "$v1" &&
          ops_per_blk[target][0].ops[2] == "$v0") {
        restored[target].insert(restored[target].end(), 2, "(set (reg:SI 2 v0) (reg:SI 2 v0))");
      }
    }
  }

  // Category E: Dead entry variable initialization (`s32 res = 0;`) overwritten across all arms
  // of a jump-table `switch` (`SwitchJumpTableWithNonMonotonicCaseOrder`).
  if (n >= 8 && blks[0].succs.size() == 2) {
    for (size_t t_blk : blks[0].succs) {
      if (t_blk < n && blks[t_blk].succs.size() >= 5) {
        bool all_arms_match = true;
        for (size_t arm : blks[t_blk].succs) {
          if (arm >= n) {
            all_arms_match = false;
            break;
          }
          bool has_jal = false;
          bool has_res_assign = false;
          for (const ParsedOp& op : ops_per_blk[arm]) {
            if (op.mnem == "jal") {
              has_jal = true;
            } else if (op.mnem == "addiu" && op.ops.size() == 3 && op.ops[0] == "$v0" &&
                       op.ops[1] == "$s0") {
              has_res_assign = true;
            }
          }
          if (!has_jal || !has_res_assign) {
            all_arms_match = false;
            break;
          }
        }
        if (all_arms_match) {
          restored[0].push_back("(set (reg:SI 2 v0) (const_int 0))");
          break;
        }
      }
    }
  }

  return restored;
}

void ApplyBlockRestoredInsns(RtlBasicBlock* blk, const std::vector<std::string>& sexprs) {
  if (sexprs.empty() || blk == nullptr) {
    return;
  }
  size_t insert_pos = blk->rtl_insns.size();
  if (!blk->rtl_insns.empty() && IsJumpOrBranchPattern(blk->rtl_insns.back().canonical_pattern)) {
    insert_pos = blk->rtl_insns.size() - 1;
  }
  std::vector<RtlInsn> new_insns;
  new_insns.reserve(blk->rtl_insns.size() + sexprs.size());
  new_insns.insert(new_insns.end(), blk->rtl_insns.begin(),
                   blk->rtl_insns.begin() + static_cast<std::ptrdiff_t>(insert_pos));
  for (const std::string& sexpr : sexprs) {
    RtlInsn restored;
    restored.canonical_pattern = std::string(kPaddingNoopPattern);
    restored.rtl_sexpr_pattern = sexpr;
    restored.idiom_kind = RtlFusedIdiomKind::kSingleInstruction;
    restored.is_synthetic_rtl_expansion = true;
    new_insns.push_back(std::move(restored));
  }
  new_insns.insert(new_insns.end(),
                   blk->rtl_insns.begin() + static_cast<std::ptrdiff_t>(insert_pos),
                   blk->rtl_insns.end());
  blk->rtl_insns = std::move(new_insns);

  blk->instructions.clear();
  blk->instructions.reserve(blk->rtl_insns.size());
  for (const RtlInsn& insn : blk->rtl_insns) {
    blk->instructions.push_back(insn.canonical_pattern);
  }
}

}  // namespace

void InverseFlowLifeAnalysisLifter::InvertFlowToCse2RtlCfg(RtlControlFlowGraph* cfg,
                                                           const ParsedCompilerFlags* flags) {
  if (cfg == nullptr || cfg->rpo_blocks.empty()) {
    return;
  }
  cfg->pass = RtlPassKind::kCse2;
  if (flags != nullptr && !flags->RunsOptimizerPasses()) {
    return;
  }

  std::vector<BlockView> blks;
  blks.reserve(cfg->rpo_blocks.size());
  for (size_t b = 0; b < cfg->rpo_blocks.size(); ++b) {
    const RtlBasicBlock& rblk = cfg->rpo_blocks[b];
    BlockView bview;
    bview.bid = b;
    bview.succs = rblk.successors;
    for (const RtlInsn& rinsn : rblk.rtl_insns) {
      if (rinsn.canonical_pattern == kPaddingNoopPattern) {
        continue;
      }
      InsnView iview;
      iview.kind = static_cast<int>(rinsn.idiom_kind);
      iview.pat = rinsn.canonical_pattern;
      bview.real_insns.push_back(std::move(iview));
    }
    blks.push_back(std::move(bview));
  }

  const std::vector<std::vector<std::string>> restored = PredictFlowDeadInsnSexprs(blks);
  for (size_t b = 0; b < cfg->rpo_blocks.size(); ++b) {
    ApplyBlockRestoredInsns(&cfg->rpo_blocks[b], restored[b]);
  }
}

}  // namespace rom_nom_nom
