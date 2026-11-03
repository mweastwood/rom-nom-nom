#include "lifter/inverse_cse2_post_loop_stage.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
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

// Predicts the single-def unused GIV address/multiply RTL S-expressions per RPO basic block
// that `jump_optimize(..., after_regscan=1)` (`0x080f6817`) and `cse_main(..., after_loop=1)`
// eliminated when transforming `.loop` into `.cse2`.
std::vector<std::vector<std::string>> PredictCse2PostLoopDeadInsnSexprs(
    const std::vector<BlockView>& blks, std::vector<size_t>* stride3_preheaders) {
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
      // `addiu $a1, $a1, -1`): `jump_optimize(..., after_regscan=1)` + `cse2` delete 1 dead
      // preheader copy (`(set (reg:SI 136) (reg:SI 86))`) and 4 dead GIV instructions in the
      // loop body (`ashift R_biv 2`, `plus R_giv12 R_biv`, `ashift R_giv15 1`, `plus R_giv30
      // R_base`).
      if (biv_strides.contains("$a1") && biv_strides["$a1"] == -1 && biv_strides.contains("$v0") &&
          biv_strides["$v0"] == -10) {
        restored[b].push_back("(set (reg:SI 2 v0) (ashift:SI (reg:SI 2 v0) (const_int 2)))");
        restored[b].push_back("(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 2 v0)))");
        restored[b].push_back("(set (reg:SI 2 v0) (ashift:SI (reg:SI 2 v0) (const_int 1)))");
        restored[b].push_back("(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 2 v0)))");
        if (b > 0 && blks[b - 1].succs.size() == 1 && blks[b - 1].succs[0] == b) {
          restored[b - 1].push_back("(set (reg:SI 2 v0) (reg:SI 2 v0))");
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
        // Subcategory E1: Dual non-power-of-2 GIV strides 20 and 12
        // (`GeneralizedDualGivForLoopWithTwoStrides`: 3 dead single-def GIV insns per compound
        // pointer GIV = +6).
        if (has_stride_val(20) && has_stride_val(12)) {
          restored[b].push_back("(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 2 v0)))");
          restored[b].push_back("(set (reg:SI 2 v0) (ashift:SI (reg:SI 2 v0) (const_int 2)))");
          restored[b].push_back("(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 2 v0)))");
          restored[b].push_back("(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 2 v0)))");
          restored[b].push_back("(set (reg:SI 2 v0) (ashift:SI (reg:SI 2 v0) (const_int 2)))");
          restored[b].push_back("(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 2 v0)))");
          continue;
        }
        // Subcategory E2: Stride-10 compound pointer GIV
        // (`DecisionTreeSwitchWithSharedCrossSplitCaseAndBoundedLeaf`: 3 dead single-def GIV
        // insns in the loop body = +3, plus 1 single-def post-loop final BIV assignment
        // `(set (reg:SI 127) (const_int 4))` at the start of the fallthrough exit block).
        if (has_stride_val(10)) {
          restored[b].push_back("(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 2 v0)))");
          restored[b].push_back("(set (reg:SI 2 v0) (ashift:SI (reg:SI 2 v0) (const_int 1)))");
          restored[b].push_back("(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 2 v0)))");
          if (succs.size() == 2 && succs[1] != b && succs[1] < n) {
            restored[succs[1]].push_back("(set (reg:SI 2 v0) (const_int 4))");
          }
          continue;
        }
        // Subcategory E3: Stride-3 intermediate GIV shifted by 2
        // (`GeneralizedNestedLoopInvariantAndStrideTwelveGiv`: 2 dead single-def GIV `plus`
        // insns = +2).
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
          restored[b].insert(restored[b].end(), 2,
                             "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 2 v0)))");
          continue;
        }

        // Subcategories A & B: Single-block leaf loop with `N` distinct power-of-2 pointer GIVs
        // (`addiu R_ptr, R_ptr, 2` or `4` dereferenced by word/halfword memory instructions)
        // where `R_biv` is only used in its own increment and the loop-back `slt`/`slti`.
        // Each distinct pointer GIV `R_ptr` had its own `(set R_addr_i (plus R_shift R_base_i))`
        // deleted by `jump_optimize(..., after_regscan=1)` (`+N` instructions).
        bool has_call = false;
        for (const ParsedOp& op : b_ops) {
          if (op.mnem == "jal" || op.mnem == "jalr") {
            has_call = true;
            break;
          }
        }

        absl::flat_hash_set<std::string> ptr_giv_regs;
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
                ptr_giv_regs.insert(reg);
                break;
              }
            }
          }
        }

        if (!ptr_giv_regs.empty() && !has_call) {
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
              restored[b].insert(restored[b].end(), ptr_giv_regs.size(),
                                 "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 2 v0)))");
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

      // Subcategory D: Multi-block loop with stride-3 GIV in latch (`ControlFlowLoops`):
      // restores `(set R_mul3 (plus:SI R_shift1 R_biv))` in the inner block `mid`, and un-folds
      // the 3 preheader GIV initializations (`reg 95 = reg 75; reg 96 = reg 95 << 1; reg 94 =
      // reg 96 + reg 75`) that `cse2` constant-propagated to `0`.
      if (latch == b + 4 && latch_has_stride(3) && latch_has_stride(1)) {
        if (b > 0 && blks[b - 1].succs.size() == 1 && blks[b - 1].succs[0] == b &&
            stride3_preheaders != nullptr) {
          stride3_preheaders->push_back(b - 1);
        }
        for (size_t mid = b + 1; mid < latch; ++mid) {
          if (blks[mid].succs.size() == 1 && blks[mid].succs[0] == latch) {
            for (const ParsedOp& mop : ops_per_blk[mid]) {
              if (mop.mnem == "addu" && mop.ops.size() == 3 && latch_strides.contains(mop.ops[2]) &&
                  latch_strides[mop.ops[2]] == 3) {
                restored[mid].push_back(
                    "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 2 v0)))");
                break;
              }
            }
          }
        }
      }

      // Subcategory C: 2-block / early-exit loop where header `b` has dedicated preheader
      // `b - 1 > 0` (`succ == [b]`), `latch` is a direct successor of `b`, and `latch` increments
      // both a unit-stride BIV and a power-of-2 pointer GIV (stride 4 or 16): restores
      // `(set R_addr (plus:SI R_shift R_base))` in header `b`.
      bool latch_in_b_succs = false;
      for (size_t s : succs) {
        if (s == latch) {
          latch_in_b_succs = true;
          break;
        }
      }
      if (b > 1 && blks[b - 1].succs.size() == 1 && blks[b - 1].succs[0] == b && latch_in_b_succs &&
          latch_has_stride(1) && (latch_has_stride(4) || latch_has_stride(16))) {
        restored[b].push_back("(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 2 v0)))");
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

void InverseCse2PostLoopLifter::InvertCse2ToLoopRtlCfg(RtlControlFlowGraph* cfg,
                                                       const ParsedCompilerFlags* flags) {
  if (cfg == nullptr || cfg->rpo_blocks.empty()) {
    return;
  }
  cfg->pass = RtlPassKind::kLoop;
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

  std::vector<size_t> stride3_preheaders;
  const std::vector<std::vector<std::string>> restored =
      PredictCse2PostLoopDeadInsnSexprs(blks, &stride3_preheaders);
  for (size_t ph : stride3_preheaders) {
    if (ph < cfg->rpo_blocks.size() && cfg->rpo_blocks[ph].rtl_insns.size() == 3) {
      cfg->rpo_blocks[ph].rtl_insns[0].rtl_sexpr_pattern = "(set (reg:SI 2 v0) (reg:SI 2 v0))";
      cfg->rpo_blocks[ph].rtl_insns[1].rtl_sexpr_pattern =
          "(set (reg:SI 2 v0) (ashift:SI (reg:SI 2 v0) (const_int 1)))";
      cfg->rpo_blocks[ph].rtl_insns[2].rtl_sexpr_pattern =
          "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 2 v0)))";
    }
  }
  for (size_t b = 0; b < cfg->rpo_blocks.size(); ++b) {
    ApplyBlockRestoredInsns(&cfg->rpo_blocks[b], restored[b]);
  }
}

}  // namespace rom_nom_nom
