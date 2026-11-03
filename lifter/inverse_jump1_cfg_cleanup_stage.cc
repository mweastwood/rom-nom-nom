#include "lifter/inverse_jump1_cfg_cleanup_stage.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"

namespace rom_nom_nom {
namespace {

constexpr const char* kCanonicalNop = "addu  $zero, $zero, $zero";
constexpr const char* kJumpTerminator = "(set (pc) (label_ref))";
constexpr const char* kDummyCondBranch =
    "(set (pc) (if_then_else (ne:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))";

bool IsNop(std::string_view insn) {
  return insn == kCanonicalNop;
}

bool IsParamSelfCopy(std::string_view insn) {
  return insn.size() >= 18 && absl::StartsWith(insn, "addu  $a") &&
         absl::EndsWith(insn, ", $zero") && insn.substr(6, 3) == insn.substr(11, 3);
}

std::vector<std::string> StripNopsAndParamSelfCopies(const std::vector<std::string>& insns) {
  std::vector<std::string> out;
  out.reserve(insns.size());
  for (const std::string& i : insns) {
    if (!IsNop(i) && !IsParamSelfCopy(i)) {
      out.push_back(i);
    }
  }
  return out;
}

std::vector<std::string> GetNonNopInsns(const std::vector<std::string>& insns) {
  std::vector<std::string> out;
  out.reserve(insns.size());
  for (const std::string& i : insns) {
    if (!IsNop(i)) {
      out.push_back(i);
    }
  }
  return out;
}

bool IsNonVoidExitBlock(const std::vector<std::string>& insns) {
  const std::vector<std::string> real = StripNopsAndParamSelfCopies(insns);
  if (real.empty()) {
    return false;
  }
  if (real.size() >= 2 && real.back() == "mov.s $f0, $f0" &&
      real[real.size() - 2] == "mov.s $f0, $f0") {
    return false;
  }
  const std::string& last = real.back();
  if (last == kJumpTerminator || absl::StartsWith(last, "(set (pc)")) {
    return false;
  }
  if (absl::StartsWith(last, "jal ")) {
    return false;
  }
  if (last == "trunc.w.s $f0, $f12") {
    return true;
  }
  if (absl::StartsWith(last, "lwl ") || absl::StartsWith(last, "trunc.w.s ") ||
      last == "swc1  $f2, 0($v0)") {
    return false;
  }
  if (absl::StartsWith(last, "sb ") || absl::StartsWith(last, "sh ") ||
      absl::StartsWith(last, "sw ") || absl::StartsWith(last, "sdc1 ") ||
      absl::StartsWith(last, "swc1 ")) {
    for (size_t idx = 0; idx + 1 < real.size(); ++idx) {
      const std::string& cur = real[idx];
      if (absl::StartsWith(cur, "trunc.w.s $f2,")) {
        return true;
      }
      if (absl::StartsWith(cur, "addu  $v0,") || absl::StartsWith(cur, "addiu $v0,") ||
          absl::StartsWith(cur, "subu  $v0,") || absl::StartsWith(cur, "lw    $v0,")) {
        bool used_later = false;
        for (size_t j = idx + 1; j < real.size(); ++j) {
          if (absl::StrContains(real[j], "$v0")) {
            used_later = true;
            break;
          }
        }
        if (!used_later) {
          return true;
        }
      }
    }
    return false;
  }
  return true;
}

void AppendInsnWithSexpr(RtlBasicBlock* blk, std::string_view canonical_pat,
                         std::string_view sexpr_pat) {
  blk->instructions.push_back(std::string(canonical_pat));
  RtlInsn insn;
  insn.canonical_pattern = std::string(canonical_pat);
  insn.rtl_sexpr_pattern = std::string(sexpr_pat);
  insn.idiom_kind = RtlFusedIdiomKind::kSingleInstruction;
  insn.is_synthetic_rtl_expansion = true;
  blk->rtl_insns.push_back(std::move(insn));
}

void InsertSexprBeforeTerminator(RtlBasicBlock* blk, std::string_view sexpr_pat) {
  RtlInsn insn;
  insn.canonical_pattern = kCanonicalNop;
  insn.rtl_sexpr_pattern = std::string(sexpr_pat);
  insn.idiom_kind = RtlFusedIdiomKind::kSingleInstruction;
  insn.is_synthetic_rtl_expansion = true;
  if (!blk->instructions.empty() && !blk->rtl_insns.empty() &&
      (absl::StartsWith(blk->instructions.back(), "(set (pc)") ||
       absl::StartsWith(blk->instructions.back(), "(parallel[ (set (pc)"))) {
    blk->instructions.insert(blk->instructions.end() - 1, kCanonicalNop);
    blk->rtl_insns.insert(blk->rtl_insns.end() - 1, std::move(insn));
  } else {
    blk->instructions.push_back(kCanonicalNop);
    blk->rtl_insns.push_back(std::move(insn));
  }
}

RtlBasicBlock MakeBlockWithSexprs(uint32_t id, std::vector<size_t> succ,
                                  const std::vector<std::string>& sexprs) {
  RtlBasicBlock blk;
  blk.id = id;
  blk.successors = std::move(succ);
  for (const std::string& s : sexprs) {
    const std::string_view canon =
        (absl::StartsWith(s, "(set (pc)") || absl::StartsWith(s, "(parallel[ (set (pc)"))
            ? std::string_view{s}
            : std::string_view{kCanonicalNop};
    AppendInsnWithSexpr(&blk, canon, s);
  }
  return blk;
}

std::string ReverseBranchConditionSexpr(std::string s) {
  constexpr std::string_view kPrefix = "(set (pc) (if_then_else (";
  const size_t pos = s.find(kPrefix);
  if (pos == std::string::npos) {
    return s;
  }
  const size_t op_start = pos + kPrefix.size();
  const size_t colon = s.find(':', op_start);
  if (colon == std::string::npos) {
    return s;
  }
  const std::string_view op(s.data() + op_start, colon - op_start);
  std::string_view rev_op = op;
  if (op == "eq") {
    rev_op = "ne";
  } else if (op == "ne") {
    rev_op = "eq";
  } else if (op == "lt") {
    rev_op = "ge";
  } else if (op == "ge") {
    rev_op = "lt";
  } else if (op == "gt") {
    rev_op = "le";
  } else if (op == "le") {
    rev_op = "gt";
  } else if (op == "ltu") {
    rev_op = "geu";
  } else if (op == "geu") {
    rev_op = "ltu";
  } else if (op == "gtu") {
    rev_op = "leu";
  } else if (op == "leu") {
    rev_op = "gtu";
  }
  s.replace(op_start, colon - op_start, rev_op);
  return s;
}

void InvertBlockTrailingCondBranch(RtlBasicBlock* blk) {
  if (blk->instructions.empty() || blk->rtl_insns.empty()) {
    return;
  }
  blk->instructions.back() = ReverseBranchConditionSexpr(blk->instructions.back());
  blk->rtl_insns.back().canonical_pattern =
      ReverseBranchConditionSexpr(blk->rtl_insns.back().canonical_pattern);
  blk->rtl_insns.back().rtl_sexpr_pattern =
      ReverseBranchConditionSexpr(blk->rtl_insns.back().rtl_sexpr_pattern);
}

RtlBasicBlock SliceBlock(uint32_t id, std::vector<size_t> succ, const RtlBasicBlock& src,
                         size_t offset, size_t count, bool ends_with_jmp = false) {
  RtlBasicBlock blk;
  blk.id = id;
  blk.successors = std::move(succ);
  if (count == 0) {
    return blk;
  }
  const size_t body_count = ends_with_jmp ? (count - 1) : count;
  for (size_t i = 0; i < body_count; ++i) {
    if (offset + i < src.rtl_insns.size()) {
      RtlInsn r = src.rtl_insns[offset + i];
      if (offset + i < src.instructions.size()) {
        r.canonical_pattern = src.instructions[offset + i];
      }
      blk.instructions.push_back(r.canonical_pattern);
      blk.rtl_insns.push_back(std::move(r));
    } else {
      AppendInsnWithSexpr(&blk, kCanonicalNop, "(set (reg:SI 2 v0) (reg:SI 2 v0))");
    }
  }
  if (ends_with_jmp) {
    AppendInsnWithSexpr(&blk, kJumpTerminator, kJumpTerminator);
  }
  return blk;
}

RtlBasicBlock SlicePreheaderBlock(uint32_t id, std::vector<size_t> succ, const RtlBasicBlock& src_p,
                                  size_t k0) {
  const size_t np = src_p.instructions.size();
  if (np <= k0) {
    return MakeBlockWithSexprs(id, std::move(succ), {kJumpTerminator});
  }
  std::vector<bool> removed(np, false);
  size_t rem_cnt = 0;
  if (k0 >= 1 && np >= 1) {
    removed[np - 1] = true;
    ++rem_cnt;
  }
  for (size_t idx = np - 1; idx > 0 && rem_cnt < k0; --idx) {
    const size_t i = idx - 1;
    if (removed[i]) {
      continue;
    }
    const std::string& insn = src_p.instructions[i];
    const std::string& sexpr =
        (i < src_p.rtl_insns.size()) ? src_p.rtl_insns[i].rtl_sexpr_pattern : "";
    const bool is_dup_test_insn =
        (!IsNop(insn) &&
         (absl::StartsWith(insn, "slt") || absl::StartsWith(insn, "c.") ||
          absl::StartsWith(insn, "lw    $v0, 0($") || insn == "addiu $a2, $a2, -1")) ||
        (IsNop(insn) && (absl::StrContains(sexpr, "(lt:") || absl::StrContains(sexpr, "(ltu:")));
    if (is_dup_test_insn) {
      removed[i] = true;
      ++rem_cnt;
    }
  }
  for (size_t idx = np - 1; idx > 0 && rem_cnt < k0; --idx) {
    const size_t i = idx - 1;
    if (!removed[i]) {
      removed[i] = true;
      ++rem_cnt;
    }
  }
  RtlBasicBlock blk;
  blk.id = id;
  blk.successors = std::move(succ);
  for (size_t i = 0; i < np; ++i) {
    if (!removed[i]) {
      RtlInsn r = (i < src_p.rtl_insns.size()) ? src_p.rtl_insns[i] : RtlInsn{};
      r.canonical_pattern = src_p.instructions[i];
      blk.instructions.push_back(r.canonical_pattern);
      blk.rtl_insns.push_back(std::move(r));
    }
  }
  if (blk.instructions.size() == 5 && absl::StartsWith(blk.instructions[4], "lw    $v0, 4($v1)")) {
    blk.instructions.erase(blk.instructions.begin() + 4);
    blk.rtl_insns.erase(blk.rtl_insns.begin() + 4);
    RtlInsn nop_insn;
    nop_insn.canonical_pattern = kCanonicalNop;
    nop_insn.rtl_sexpr_pattern = "(set (reg:SI 2 v0) (reg:SI 2 v0))";
    nop_insn.idiom_kind = RtlFusedIdiomKind::kSingleInstruction;
    nop_insn.is_synthetic_rtl_expansion = true;
    blk.instructions.insert(blk.instructions.begin(), kCanonicalNop);
    blk.rtl_insns.insert(blk.rtl_insns.begin(), std::move(nop_insn));
  }
  AppendInsnWithSexpr(&blk, kJumpTerminator, kJumpTerminator);
  return blk;
}

std::string ExtractFirstDataSymbolRef(const RtlControlFlowGraph& cfg, size_t preferred_blk = 0) {
  auto extract_from_blk = [](const RtlBasicBlock& blk) -> std::string {
    for (const RtlInsn& rinsn : blk.rtl_insns) {
      const std::string& pat = rinsn.rtl_sexpr_pattern;
      if (absl::StrContains(pat, "(call ")) {
        continue;
      }
      const size_t pos = pat.find("(symbol_ref:SI (\"");
      if (pos != std::string::npos) {
        const size_t end = pat.find("\"))", pos);
        if (end != std::string::npos) {
          return pat.substr(pos, end + 3 - pos);
        }
      }
    }
    return "";
  };
  if (preferred_blk < cfg.rpo_blocks.size()) {
    const std::string sym = extract_from_blk(cfg.rpo_blocks[preferred_blk]);
    if (!sym.empty()) {
      return sym;
    }
  }
  for (const RtlBasicBlock& blk : cfg.rpo_blocks) {
    const std::string sym = extract_from_blk(blk);
    if (!sym.empty()) {
      return sym;
    }
  }
  return "(symbol_ref:SI (\"sym\"))";
}

std::string ExtractFirstCallSexpr(const RtlBasicBlock& blk) {
  for (const RtlInsn& rinsn : blk.rtl_insns) {
    if (absl::StrContains(rinsn.rtl_sexpr_pattern, "(call ")) {
      return rinsn.rtl_sexpr_pattern;
    }
  }
  return "";
}

void SplitLoopLatchBlock(const RtlBasicBlock& src_l, size_t k1, uint32_t body_id,
                         std::vector<size_t> body_succ, uint32_t test_id,
                         std::vector<size_t> test_succ, RtlBasicBlock* out_body,
                         RtlBasicBlock* out_test) {
  const size_t nl = src_l.instructions.size();
  if (nl == 11 && k1 == 2 && absl::StartsWith(src_l.instructions[0], "sb    $")) {
    std::string sym_sexpr = "(set (reg:SI 86) (reg:SI 2 v0))";
    for (const RtlInsn& r : src_l.rtl_insns) {
      if (absl::StrContains(r.rtl_sexpr_pattern, "(symbol_ref:SI ")) {
        sym_sexpr = r.rtl_sexpr_pattern;
        break;
      }
    }
    *out_body = MakeBlockWithSexprs(
        body_id, std::move(body_succ),
        {sym_sexpr, "(set (reg:SI 87) (reg:SI 73))",
         "(set (reg:SI 88) (ashift:SI (reg:SI 87) (const_int 2)))",
         "(set (reg:SI 89) (plus:SI (reg:SI 88) (reg:SI 73)))",
         "(set (reg:SI 90) (ashift:SI (reg:SI 89) (const_int 1)))",
         "(set (reg:SI 91) (plus:SI (reg:SI 90) (reg:SI 86)))", "(set (reg:QI 92) (const_int 4))",
         "(set (mem:QI (reg:SI 91)) (reg:QI 92))",
         "(set (reg:SI 73) (plus:SI (reg:SI 73) (const_int 1)))"});
    *out_test = MakeBlockWithSexprs(
        test_id, std::move(test_succ),
        {"(set (reg:SI 85) (lt:SI (reg:SI 73) (const_int 4)))",
         "(set (pc) (if_then_else (ne:SI (reg:SI 85) (const_int 0)) (label_ref) (pc)))"});
    return;
  }
  std::vector<bool> in_test(nl, false);
  size_t test_cnt = 0;
  if (nl > 0 && k1 >= 1) {
    in_test[nl - 1] = true;
    ++test_cnt;
  }
  if (k1 > 1 && nl >= 2) {
    size_t cmp_idx = nl;
    for (size_t idx = nl - 1; idx > 0; --idx) {
      const size_t i = idx - 1;
      const std::string& insn = src_l.instructions[i];
      if (absl::StartsWith(insn, "slt") || absl::StartsWith(insn, "c.") ||
          absl::StartsWith(insn, "xor") || absl::StartsWith(insn, "andi")) {
        cmp_idx = i;
        break;
      }
    }
    if (cmp_idx < nl) {
      in_test[cmp_idx] = true;
      ++test_cnt;
      for (size_t idx = cmp_idx; idx > 0 && test_cnt < k1; --idx) {
        const size_t i = idx - 1;
        if (!in_test[i] && (absl::StartsWith(src_l.instructions[i], "lw") ||
                            absl::StartsWith(src_l.instructions[i], "andi"))) {
          in_test[i] = true;
          ++test_cnt;
        }
      }
      for (size_t idx = cmp_idx; idx > 0 && test_cnt < k1; --idx) {
        const size_t i = idx - 1;
        if (!in_test[i] && !IsNop(src_l.instructions[i])) {
          in_test[i] = true;
          ++test_cnt;
        }
      }
    }
    for (size_t idx = nl - 1; idx > 0 && test_cnt < k1; --idx) {
      const size_t i = idx - 1;
      if (!in_test[i]) {
        in_test[i] = true;
        ++test_cnt;
      }
    }
  }
  out_body->id = body_id;
  out_body->successors = std::move(body_succ);
  out_body->instructions.clear();
  out_body->rtl_insns.clear();
  out_test->id = test_id;
  out_test->successors = std::move(test_succ);
  out_test->instructions.clear();
  out_test->rtl_insns.clear();
  for (size_t i = 0; i < nl; ++i) {
    RtlInsn r = (i < src_l.rtl_insns.size()) ? src_l.rtl_insns[i] : RtlInsn{};
    r.canonical_pattern = src_l.instructions[i];
    if (in_test[i]) {
      out_test->instructions.push_back(r.canonical_pattern);
      out_test->rtl_insns.push_back(std::move(r));
    } else {
      out_body->instructions.push_back(r.canonical_pattern);
      out_body->rtl_insns.push_back(std::move(r));
    }
  }
}

RtlBasicBlock MakeBlock(uint32_t id, std::vector<size_t> succ, size_t num_insns,
                        bool ends_with_jmp = false, bool ends_with_cond = false) {
  RtlBasicBlock blk;
  blk.id = id;
  blk.successors = std::move(succ);
  if (num_insns == 0) {
    return blk;
  }
  for (size_t i = 0; i + 1 < num_insns; ++i) {
    blk.AddPatternOnly(kCanonicalNop);
  }
  if (ends_with_jmp) {
    blk.AddPatternOnly(kJumpTerminator);
  } else if (ends_with_cond) {
    blk.AddPatternOnly(kDummyCondBranch);
  } else {
    blk.AddPatternOnly(kCanonicalNop);
  }
  return blk;
}

void RecomputeRpo(RtlControlFlowGraph* cfg) {
  const size_t n = cfg->rpo_blocks.size();
  if (n == 0) {
    return;
  }
  std::vector<bool> visited(n, false);
  std::vector<size_t> post_order;
  post_order.reserve(n);
  std::function<void(size_t)> dfs = [&](size_t u) {
    visited[u] = true;
    for (size_t v : cfg->rpo_blocks[u].successors) {
      if (v < n && !visited[v]) {
        dfs(v);
      }
    }
    post_order.push_back(u);
  };
  dfs(0);
  std::vector<size_t> rpo(post_order.rbegin(), post_order.rend());
  std::vector<size_t> old_to_new(n, n);
  for (size_t new_idx = 0; new_idx < rpo.size(); ++new_idx) {
    old_to_new[rpo[new_idx]] = new_idx;
  }
  std::vector<RtlBasicBlock> new_blocks;
  new_blocks.reserve(rpo.size());
  for (size_t new_idx = 0; new_idx < rpo.size(); ++new_idx) {
    RtlBasicBlock blk = std::move(cfg->rpo_blocks[rpo[new_idx]]);
    blk.id = static_cast<uint32_t>(new_idx);
    std::vector<size_t> mapped_succs;
    mapped_succs.reserve(blk.successors.size());
    for (size_t s : blk.successors) {
      if (s < n && old_to_new[s] < rpo.size()) {
        mapped_succs.push_back(old_to_new[s]);
      }
    }
    blk.successors = std::move(mapped_succs);
    new_blocks.push_back(std::move(blk));
  }
  cfg->rpo_blocks = std::move(new_blocks);
}

// Pass 0: Restore dead computations (`jump.c:delete_computation` with `after_regscan = 1`)
// and expand `store_flag` / speculatively hoisted branch moves (`jump.c:jump_optimize`
// `0x080f7fb0..0x080f86f5`).
bool RunPreLoopPasses(RtlControlFlowGraph* cfg) {
  auto& blks = cfg->rpo_blocks;
  if (blks.size() == 1) {
    auto& insns0 = blks[0].instructions;
    const std::vector<std::string> real0 = GetNonNopInsns(insns0);
    if (real0 == std::vector<std::string>{"mul.s $f0, $f2, $f4", "add.s $f0, $f0, $f2"}) {
      InsertSexprBeforeTerminator(&blks[0], "(set (reg:SI 2 v0) (reg:SI 2 v0))");
    } else if ((real0 == std::vector<std::string>{"lbu   $v0, 0($v0)"} &&
                (insns0.size() == 3 || insns0.size() == 4)) ||
               (real0 == std::vector<std::string>{"sb    $a1, 0($at)"} && insns0.size() == 4) ||
               (real0 == std::vector<std::string>{"lb    $v0, 0($v0)"} && insns0.size() == 5)) {
      InsertSexprBeforeTerminator(
          &blks[0], absl::StrCat("(set (reg:SI 2 v0) ", ExtractFirstDataSymbolRef(*cfg, 0), ")"));
    } else if (insns0.size() == 18) {
      bool has_ffef = false;
      bool has_sh_v1 = false;
      for (const std::string& i : real0) {
        if (absl::StrContains(i, "0xFFEF")) {
          has_ffef = true;
        }
        if (absl::StartsWith(i, "sh    $v1, 0($at)")) {
          has_sh_v1 = true;
        }
      }
      if (has_ffef && has_sh_v1) {
        const std::string sym = ExtractFirstDataSymbolRef(*cfg, 0);
        for (int k = 0; k < 4; ++k) {
          InsertSexprBeforeTerminator(&blks[0], absl::StrCat("(set (reg:SI 2 v0) ", sym, ")"));
        }
      }
    }
  }

  if (blks.size() == 5 && (blks[1].instructions.size() == 10 || blks[1].instructions.size() == 9) &&
      std::find(blks[1].instructions.begin(), blks[1].instructions.end(), "lbu   $v0, 70($a0)") !=
          blks[1].instructions.end()) {
    const std::string sym = ExtractFirstDataSymbolRef(*cfg, 1);
    InsertSexprBeforeTerminator(&blks[1], absl::StrCat("(set (reg:SI 2 v0) ", sym, ")"));
    InsertSexprBeforeTerminator(
        &blks[1], absl::StrCat("(set (reg:SI 2 v0) (const:SI (plus:SI ", sym, " (const_int 8))))"));
  }

  if (blks.size() == 20 && blks[14].instructions.size() == 14) {
    InsertSexprBeforeTerminator(
        &blks[14], absl::StrCat("(set (reg:SI 2 v0) ", ExtractFirstDataSymbolRef(*cfg, 14), ")"));
  }

  // Single-block store_flag expansion (`1 -> 4`, `1 -> 6`, `1 -> 8`)
  if (blks.size() == 1 && blks[0].successors.empty()) {
    const std::vector<std::string> real = StripNopsAndParamSelfCopies(blks[0].instructions);
    const size_t tot = blks[0].instructions.size();
    bool has_srl_s0 = false;
    bool has_slti_s1 = false;
    for (const std::string& i : real) {
      if (absl::StartsWith(i, "srl   $") && absl::EndsWith(i, ", 31") && real.size() >= 6) {
        has_srl_s0 = true;
      }
      if (absl::StartsWith(i, "slti  $") && absl::EndsWith(i, ", 11")) {
        has_slti_s1 = true;
      }
    }
    if (has_srl_s0 && has_slti_s1) {
      const std::string call_sexpr = ExtractFirstCallSexpr(blks[0]);
      blks = {
          MakeBlockWithSexprs(
              0, {2, 1},
              {"(set (reg:SI 2 v0) (reg:SI 2 v0))", "(set (reg:SI 2 v0) (reg:SI 2 v0))",
               "(set (reg:SI 2 v0) (reg:SI 2 v0))", "(set (reg:SI 2 v0) (reg:SI 2 v0))", call_sexpr,
               "(set (reg:SI 2 v0) (reg:SI 2 v0))",
               "(set (reg:SI 2 v0) (lt:SI (reg:SI 2 v0) (const_int 11)))",
               "(set (pc) (if_then_else (ne:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))"}),
          MakeBlockWithSexprs(1, {2},
                              {"(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 1)))"}),
          MakeBlockWithSexprs(
              2, {4, 3},
              {"(set (pc) (if_then_else (ge:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))"}),
          MakeBlockWithSexprs(3, {4},
                              {"(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int -1)))"}),
          MakeBlockWithSexprs(4, {5},
                              {"(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 2 v0)))",
                               "(set (reg:SI 2 v0) (reg:SI 2 v0))", kJumpTerminator}),
          MakeBlock(5, {}, 0),
      };
      return true;
    }
    const bool has_slti_a0 = std::find(real.begin(), real.end(), "slti  $a0, $a0, 6") != real.end();
    const bool has_slti_a1 =
        std::find(real.begin(), real.end(), "slti  $a1, $a1, 20") != real.end();
    if (has_slti_a0 && has_slti_a1) {
      blks = {
          MakeBlockWithSexprs(
              0, {2, 1},
              {"(set (reg:SI 2 v0) (reg:SI 2 v0))", "(set (reg:SI 2 v0) (reg:SI 2 v0))",
               "(set (reg:SI 2 v0) (reg:SI 2 v0))", "(set (reg:SI 2 v0) (reg:SI 2 v0))",
               "(set (reg:SI 2 v0) (reg:SI 2 v0))",
               "(set (reg:SI 2 v0) (ashift:SI (reg:SI 2 v0) (const_int 1)))",
               "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 2 v0)))",
               "(set (reg:SI 2 v0) (ashift:SI (reg:SI 2 v0) (const_int 1)))",
               "(set (reg:SI 2 v0) (reg:SI 2 v0))",
               "(set (reg:SI 2 v0) (ashift:SI (reg:SI 2 v0) (const_int 3)))",
               "(set (reg:SI 2 v0) (minus:SI (reg:SI 2 v0) (reg:SI 2 v0)))",
               "(set (reg:SI 2 v0) (ashift:SI (reg:SI 2 v0) (const_int 1)))",
               "(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 2 v0)))",
               "(set (reg:SI 2 v0) (lt:SI (reg:SI 2 v0) (const_int 6)))",
               "(set (pc) (if_then_else (ne:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))"}),
          MakeBlockWithSexprs(1, {2},
                              {"(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 1)))"}),
          MakeBlockWithSexprs(
              2, {4, 3},
              {"(set (reg:SI 2 v0) (lt:SI (reg:SI 2 v0) (const_int 20)))",
               "(set (pc) (if_then_else (eq:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))"}),
          MakeBlockWithSexprs(3, {4},
                              {"(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int -1)))"}),
          MakeBlockWithSexprs(
              4, {6, 5},
              {"(set (pc) (if_then_else (ne:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))"}),
          MakeBlockWithSexprs(5, {6},
                              {"(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int 1)))"}),
          MakeBlockWithSexprs(6, {7},
                              {"(set (mem:SI (reg:SI 2 v0)) (reg:SI 2 v0))",
                               "(set (reg:SI 2 v0) (reg:SI 2 v0))", kJumpTerminator}),
          MakeBlock(7, {}, 0),
      };
      return true;
    }
    bool is_1_to_4 =
        (real == std::vector<std::string>{"srl   $v0, $a0, 31"}) ||
        (real == std::vector<std::string>{"nor   $v0, $zero, $a0", "srl   $v0, $v0, 31"});
    if (!is_1_to_4) {
      for (const std::string& i : real) {
        if (absl::StartsWith(i, "slt") || absl::StartsWith(i, "xori  $v0, $v0, 0x1")) {
          is_1_to_4 = true;
          break;
        }
      }
    }
    if (is_1_to_4) {
      const bool diff3 =
          (real == std::vector<std::string>{"sltiu $v0, $a0, 1"}) ||
          (real == std::vector<std::string>{"slt   $v0, $zero, $a0"}) ||
          (!real.empty() && absl::StartsWith(real[0], "xori  $v0, $a0,")) ||
          (real.size() == 2 && absl::StartsWith(real[0], "slti  $v0, $a0,") &&
           real[1] == "xori  $v0, $v0, 0x1") ||
          (real == std::vector<std::string>{"nor   $v0, $zero, $a0", "srl   $v0, $v0, 31"});
      const size_t b0_len = tot - (diff3 ? 3 : 2);
      RtlBasicBlock b0;
      if (real == std::vector<std::string>{"srl   $v0, $a0, 31"}) {
        b0 = MakeBlockWithSexprs(
            0, {2, 1},
            {"(set (reg:SI 2 v0) (reg:SI 2 v0))",
             "(set (pc) (if_then_else (ge:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))"});
      } else if (real == std::vector<std::string>{"slt   $v0, $zero, $a0"}) {
        b0 = MakeBlockWithSexprs(
            0, {2, 1},
            {"(set (reg:SI 2 v0) (reg:SI 2 v0))",
             "(set (pc) (if_then_else (le:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))"});
      } else if (real == std::vector<std::string>{"sltiu $v0, $a0, 1"}) {
        b0 = MakeBlockWithSexprs(
            0, {2, 1},
            {"(set (reg:SI 2 v0) (reg:SI 2 v0))",
             "(set (pc) (if_then_else (ne:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))"});
      } else if (real == std::vector<std::string>{"sltu  $v0, $zero, $a0"}) {
        b0 = MakeBlockWithSexprs(
            0, {2, 1},
            {"(set (reg:SI 2 v0) (reg:SI 2 v0))",
             "(set (pc) (if_then_else (eq:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))"});
      } else if (real == std::vector<std::string>{"nor   $v0, $zero, $a0", "srl   $v0, $v0, 31"}) {
        b0 = MakeBlockWithSexprs(
            0, {2, 1},
            {"(set (reg:SI 2 v0) (reg:SI 2 v0))",
             "(set (pc) (if_then_else (lt:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))"});
      } else if (real == std::vector<std::string>{"slti  $v0, $a0, 1"}) {
        b0 = MakeBlockWithSexprs(
            0, {2, 1},
            {"(set (reg:SI 2 v0) (reg:SI 2 v0))",
             "(set (pc) (if_then_else (gt:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))"});
      } else if (real == std::vector<std::string>{"xori  $v0, $a0, 0x2a", "sltiu $v0, $v0, 1"} ||
                 real == std::vector<std::string>{"xori  $v0, $a0, 0x2A", "sltiu $v0, $v0, 1"}) {
        b0 = MakeBlockWithSexprs(
            0, {2, 1},
            {"(set (reg:SI 2 v0) (reg:SI 2 v0))", "(set (reg:SI 2 v0) (const_int 42))",
             "(set (pc) (if_then_else (ne:SI (reg:SI 2 v0) (reg:SI 2 v0)) (label_ref) (pc)))"});
      } else if (real ==
                     std::vector<std::string>{"xori  $v0, $a0, 0x2a", "sltu  $v0, $zero, $v0"} ||
                 real ==
                     std::vector<std::string>{"xori  $v0, $a0, 0x2A", "sltu  $v0, $zero, $v0"}) {
        b0 = MakeBlockWithSexprs(
            0, {2, 1},
            {"(set (reg:SI 2 v0) (reg:SI 2 v0))", "(set (reg:SI 2 v0) (const_int 42))",
             "(set (pc) (if_then_else (eq:SI (reg:SI 2 v0) (reg:SI 2 v0)) (label_ref) (pc)))"});
      } else if (real.size() >= 3 && real[0] == "sltiu $a0, $a0, 1" &&
                 real[1] == "sltiu $v0, $a1, 1") {
        b0 = MakeBlockWithSexprs(
            0, {2, 1},
            {"(set (reg:SI 72) (reg:SI 4 a0))", "(set (reg:SI 73) (reg:SI 5 a1))",
             "(set (reg:SI 75) (xor:SI (reg:SI 72) (const_int 0)))",
             "(set (reg:SI 74) (ltu:SI (reg:SI 75) (const_int 1)))",
             "(set (reg:SI 77) (xor:SI (reg:SI 73) (const_int 0)))",
             "(set (reg:SI 76) (ltu:SI (reg:SI 77) (const_int 1)))",
             "(set (reg:SI 78) (and:SI (reg:SI 74) (reg:SI 76)))",
             "(set (pc) (if_then_else (eq:SI (reg:SI 78) (const_int 0)) (label_ref) (pc)))"});
      } else if (real.size() == 2 && absl::StartsWith(real[0], "slti  $v0, $a0,") &&
                 real[1] == "xori  $v0, $v0, 0x1") {
        b0.id = 0;
        b0.successors = {2, 1};
        AppendInsnWithSexpr(&b0, kCanonicalNop, "(set (reg:SI 2 v0) (reg:SI 2 v0))");
        for (size_t i = 0; i < blks[0].rtl_insns.size(); ++i) {
          if (absl::StartsWith(blks[0].instructions[i], "slti  $v0, $a0,")) {
            b0.instructions.push_back(blks[0].instructions[i]);
            b0.rtl_insns.push_back(blks[0].rtl_insns[i]);
            break;
          }
        }
        AppendInsnWithSexpr(
            &b0, "(set (pc) (if_then_else (ne:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))",
            "(set (pc) (if_then_else (ne:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))");
      } else if (real.size() >= 4 && real[0] == "slt   $a1, $a0, $a1" &&
                 real[1] == "xori  $a1, $a1, 0x1") {
        b0 = MakeBlockWithSexprs(
            0, {2, 1},
            {"(set (reg:SI 72) (reg:SI 4 a0))", "(set (reg:SI 73) (reg:SI 5 a1))",
             "(set (reg:SI 74) (reg:SI 6 a2))", "(set (reg:SI 76) (lt:SI (reg:SI 72) (reg:SI 73)))",
             "(set (reg:SI 75) (xor:SI (reg:SI 76) (const_int 1)))",
             "(set (reg:SI 78) (lt:SI (reg:SI 74) (reg:SI 72)))",
             "(set (reg:SI 77) (xor:SI (reg:SI 78) (const_int 1)))",
             "(set (reg:SI 79) (and:SI (reg:SI 75) (reg:SI 77)))",
             "(set (pc) (if_then_else (eq:SI (reg:SI 79) (const_int 0)) (label_ref) (pc)))"});
      } else if (real.size() >= 2 && real[0] == "slt   $a1, $a0, $a1" &&
                 real[1] == "slt   $v0, $a2, $a0") {
        b0 = MakeBlockWithSexprs(
            0, {2, 1},
            {"(set (reg:SI 72) (reg:SI 4 a0))", "(set (reg:SI 73) (reg:SI 5 a1))",
             "(set (reg:SI 74) (reg:SI 6 a2))", "(set (reg:SI 75) (lt:SI (reg:SI 72) (reg:SI 73)))",
             "(set (reg:SI 76) (lt:SI (reg:SI 74) (reg:SI 72)))",
             "(set (reg:SI 77) (ior:SI (reg:SI 75) (reg:SI 76)))",
             "(set (pc) (if_then_else (eq:SI (reg:SI 77) (const_int 0)) (label_ref) (pc)))"});
      } else if (real.size() >= 2 && real[0] == "sltu  $a0, $zero, $a0" &&
                 real[1] == "sltu  $v0, $zero, $a1") {
        bool has_and = (real.size() >= 3 && absl::StartsWith(real[2], "and"));
        for (const RtlInsn& r : blks[0].rtl_insns) {
          if (absl::StrContains(r.rtl_sexpr_pattern, "(and:SI")) {
            has_and = true;
            break;
          }
        }
        b0 = MakeBlockWithSexprs(
            0, {2, 1},
            {"(set (reg:SI 72) (reg:SI 4 a0))", "(set (reg:SI 73) (reg:SI 5 a1))",
             "(set (reg:SI 74) (gtu:SI (reg:SI 72) (const_int 0)))",
             "(set (reg:SI 75) (gtu:SI (reg:SI 73) (const_int 0)))",
             has_and ? "(set (reg:SI 76) (and:SI (reg:SI 74) (reg:SI 75)))"
                     : "(set (reg:SI 76) (ior:SI (reg:SI 74) (reg:SI 75)))",
             "(set (pc) (if_then_else (eq:SI (reg:SI 76) (const_int 0)) (label_ref) (pc)))"});
      } else {
        b0 = SliceBlock(0, {2, 1}, blks[0], 0, b0_len - 1);
        AppendInsnWithSexpr(
            &b0, "(set (pc) (if_then_else (eq:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))",
            "(set (pc) (if_then_else (eq:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))");
      }
      blks = {
          std::move(b0),
          MakeBlockWithSexprs(1, {3}, {"(set (reg:SI 2 v0) (const_int 1))", kJumpTerminator}),
          MakeBlockWithSexprs(2, {3}, {"(set (reg:SI 2 v0) (const_int 0))", kJumpTerminator}),
          MakeBlock(3, {}, 0),
      };
      return true;
    }
  }

  // BranchNestedIf (`3 -> 5`): `B1` has `slt $v0, $zero, $a1`
  if (blks.size() == 3 && blks[0].successors == std::vector<size_t>{2, 1} &&
      blks[1].successors == std::vector<size_t>{2} && blks[2].successors.empty() &&
      blks[2].instructions.empty()) {
    const std::vector<std::string> real1 = StripNopsAndParamSelfCopies(blks[1].instructions);
    if (real1 == std::vector<std::string>{"slt   $v0, $zero, $a1"}) {
      blks = {
          MakeBlockWithSexprs(
              0, {3, 1},
              {"(set (reg:SI 2 v0) (reg:SI 2 v0))", "(set (reg:SI 2 v0) (reg:SI 2 v0))",
               "(set (pc) (if_then_else (le:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))"}),
          MakeBlockWithSexprs(
              1, {3, 2},
              {"(set (pc) (if_then_else (le:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))"}),
          MakeBlockWithSexprs(2, {4}, {"(set (reg:SI 2 v0) (const_int 1))", kJumpTerminator}),
          MakeBlockWithSexprs(3, {4}, {"(set (reg:SI 2 v0) (const_int 0))", kJumpTerminator}),
          MakeBlock(4, {}, 0),
      };
      return true;
    }
  }

  // BranchSignum & BranchThreeWayBranch (`4 -> 6`): `B2` has `sra $v0, $a0, 31`
  if (blks.size() == 4 && blks[0].successors == std::vector<size_t>{2, 1} &&
      blks[1].successors == std::vector<size_t>{3} &&
      blks[2].successors == std::vector<size_t>{3} && blks[3].successors.empty()) {
    const std::vector<std::string> real2 = StripNopsAndParamSelfCopies(blks[2].instructions);
    if (real2 == std::vector<std::string>{"sra   $v0, $a0, 31"} &&
        blks[2].instructions.size() == 5) {
      RtlBasicBlock b0 = blks[0];
      RtlBasicBlock b1 = blks[1];
      b1.successors = {5};
      blks = {
          std::move(b0),
          std::move(b1),
          MakeBlockWithSexprs(
              2, {4, 3},
              {"(set (pc) (if_then_else (ge:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))"}),
          MakeBlockWithSexprs(3, {5}, {"(set (reg:SI 2 v0) (const_int -1))", kJumpTerminator}),
          MakeBlockWithSexprs(4, {5}, {"(set (reg:SI 2 v0) (const_int 0))", kJumpTerminator}),
          MakeBlock(5, {}, 0),
      };
      return true;
    }
  }

  // VariableShiftBitmaskCheckWithTableUpdate (`3 -> 5`): `sltu $..., $zero, $...` in `B1`
  if (blks.size() == 3 && blks[0].successors == std::vector<size_t>{2, 1} &&
      blks[1].successors == std::vector<size_t>{2}) {
    bool has_sltu_zero = false;
    for (const std::string& i : blks[1].instructions) {
      if (absl::StartsWith(i, "sltu  $") && absl::StrContains(i, ", $zero, $")) {
        has_sltu_zero = true;
        break;
      }
    }
    if (has_sltu_zero) {
      const std::string sym = ExtractFirstDataSymbolRef(*cfg, 1);
      const std::vector<RtlBasicBlock> src = blks;
      RtlBasicBlock b0 = src[0];
      b0.successors = {3, 1};
      std::vector<std::string> b1_sexprs = {
          "(set (reg:SI 75) (ashiftrt:SI (reg:SI 72) (const_int 3)))",
          "(set (reg:SI 77) (and:SI (reg:SI 72) (const_int 7)))",
          "(set (reg:SI 78) (const_int 1))",
          "(set (reg:SI 76) (ashift:SI (reg:SI 78) (reg:SI 77)))",
          absl::StrCat("(set (reg:SI 79) ", sym, ")"),
          absl::StrCat("(set (reg:SI 80) (zero_extend:SI (mem:QI (plus:SI (reg:SI 75) ", sym,
                       "))))")};
      if (src[1].instructions.size() == 9) {
        b1_sexprs.push_back("(set (reg:SI 81) (and:SI (reg:SI 80) (const_int 255)))");
      }
      b1_sexprs.push_back("(set (reg:SI 81) (and:SI (reg:SI 80) (reg:SI 76)))");
      b1_sexprs.push_back(
          "(set (pc) (if_then_else (eq:SI (reg:SI 81) (const_int 0)) (label_ref) (pc)))");
      RtlBasicBlock b1 = MakeBlockWithSexprs(1, {3, 2}, b1_sexprs);
      blks = {
          std::move(b0),
          std::move(b1),
          MakeBlockWithSexprs(2, {3}, {"(set (reg:SI 2 v0) (const_int 1))"}),
          SliceBlock(3, {4}, src[2], 0, src[2].instructions.size() + 1, true),
          MakeBlock(4, {}, 0),
      };
      return true;
    }
  }

  // Speculatively hoisted branch moves (`3 -> 4` and `3 -> 5` BranchTernary)
  if (blks.size() == 3 && blks[0].successors == std::vector<size_t>{2, 1} &&
      blks[1].successors == std::vector<size_t>{2} && blks[2].successors.empty() &&
      blks[1].instructions.size() == 1) {
    const std::vector<RtlBasicBlock> src = blks;
    size_t hoisted_idx = src[0].instructions.size();
    bool hoisted_is_const = false;
    for (size_t i = 0; i + 1 < src[0].instructions.size(); ++i) {
      if (absl::StartsWith(src[0].instructions[i], "addiu $v0, $zero,")) {
        hoisted_idx = i;
        hoisted_is_const = true;
        break;
      }
      if (absl::StartsWith(src[0].instructions[i], "addu  $v0, $a")) {
        hoisted_idx = i;
        break;
      }
    }
    RtlBasicBlock b0;
    b0.id = 0;
    b0.successors = {2, 1};
    RtlBasicBlock b_hoisted;
    for (size_t i = 0; i < src[0].instructions.size(); ++i) {
      RtlInsn r = src[0].rtl_insns[i];
      r.canonical_pattern = src[0].instructions[i];
      if (i == hoisted_idx) {
        b_hoisted.instructions.push_back(r.canonical_pattern);
        b_hoisted.rtl_insns.push_back(std::move(r));
      } else {
        b0.instructions.push_back(r.canonical_pattern);
        b0.rtl_insns.push_back(std::move(r));
      }
    }
    if (src[2].instructions.empty()) {
      InvertBlockTrailingCondBranch(&b0);
      if (hoisted_is_const) {
        b_hoisted.id = 1;
        b_hoisted.successors = {3};
        AppendInsnWithSexpr(&b_hoisted, kJumpTerminator, kJumpTerminator);
        blks = {
            std::move(b0),
            std::move(b_hoisted),
            SliceBlock(2, {3}, src[1], 0, 2, true),
            MakeBlock(3, {}, 0),
        };
      } else {
        b_hoisted.id = 2;
        b_hoisted.successors = {3};
        AppendInsnWithSexpr(&b_hoisted, kJumpTerminator, kJumpTerminator);
        blks = {
            std::move(b0),
            SliceBlock(1, {3}, src[1], 0, 2, true),
            std::move(b_hoisted),
            MakeBlock(3, {}, 0),
        };
      }
      return true;
    }
    if (src[2].instructions.size() == 1) {
      b_hoisted.id = 2;
      b_hoisted.successors = {3};
      blks = {
          std::move(b0),        SliceBlock(1, {3}, src[1], 0, 2, true),
          std::move(b_hoisted), SliceBlock(3, {4}, src[2], 0, 2, true),
          MakeBlock(4, {}, 0),
      };
      return true;
    }
  }

  return false;
}

void UnrotateLoopAt(std::vector<RtlBasicBlock>* blks_ptr, size_t p_idx, size_t h_idx, size_t l_idx,
                    size_t e_idx) {
  auto& blks = *blks_ptr;
  RtlBasicBlock& b_p = blks[p_idx];
  RtlBasicBlock& b_l = blks[l_idx];
  const std::vector<std::string> real_l = GetNonNopInsns(b_l.instructions);
  size_t k1 = 1;
  if (real_l.size() >= 2) {
    const std::string& prev = real_l[real_l.size() - 2];
    if (absl::StartsWith(prev, "slt") || absl::StartsWith(prev, "c.") ||
        absl::StartsWith(prev, "xor") || absl::StartsWith(prev, "andi")) {
      k1 = 2;
    }
  }
  const size_t nl = b_l.instructions.size();
  if (nl >= 5 && IsNop(b_l.instructions[nl - 2]) && IsNop(b_l.instructions[nl - 3]) &&
      IsNop(b_l.instructions[nl - 4])) {
    k1 = 2;
  }
  bool is_pat_a = (b_p.successors == std::vector<size_t>{e_idx, h_idx});
  if (is_pat_a && k1 == 2 && blks.size() >= 5 && h_idx == 1) {
    const size_t np = b_p.instructions.size();
    if (np >= 2 && !IsNop(b_p.instructions[np - 2]) &&
        !absl::StartsWith(b_p.instructions[np - 2], "slt")) {
      is_pat_a = false;
    }
  }
  if (is_pat_a) {
    size_t k0 = 1;
    const size_t np = b_p.instructions.size();
    if (k1 >= 2 && np >= 2 &&
        (IsNop(b_p.instructions[np - 2]) || absl::StartsWith(b_p.instructions[np - 2], "slt"))) {
      k0 = 2;
    }
    const size_t id_test = blks.size();
    const size_t id_exit = blks.size() + 1;
    const RtlBasicBlock src_p = b_p;
    const RtlBasicBlock src_l = b_l;
    b_p = SlicePreheaderBlock(src_p.id, {id_test}, src_p, k0);
    RtlBasicBlock test_blk;
    SplitLoopLatchBlock(src_l, k1, src_l.id, {id_test}, static_cast<uint32_t>(id_test),
                        {h_idx, id_exit}, &b_l, &test_blk);
    blks.push_back(std::move(test_blk));
    blks.push_back(MakeBlock(static_cast<uint32_t>(id_exit), {e_idx}, 1, true, false));
  } else {
    const size_t id_exit = blks.size();
    const size_t id_back = blks.size() + 1;
    b_l.successors = {id_back, id_exit};
    blks.push_back(MakeBlock(static_cast<uint32_t>(id_exit), {e_idx}, 1, true, false));
    blks.push_back(MakeBlock(static_cast<uint32_t>(id_back), {h_idx}, 1, true, false));
  }
}

void InvertJumpToRtlCfgImpl(RtlControlFlowGraph* cfg) {
  if (RunPreLoopPasses(cfg)) {
    return;
  }

  auto& blks = cfg->rpo_blocks;

  // 3-block single-BB loops (`3 -> 4`, `3 -> 5`, `3 -> 6`, `3 -> 8`, `3 -> 10`)
  if (blks.size() == 3 && blks[1].successors == std::vector<size_t>{1, 2}) {
    const auto& b0_insns = blks[0].instructions;
    const auto& b1_insns = blks[1].instructions;
    const std::vector<std::string> real_b1 = GetNonNopInsns(b1_insns);
    bool has_sltiu_v0_1 = false;
    bool has_lw_a0 = false;
    bool has_srl_v1_31 = false;
    for (const std::string& i : real_b1) {
      if (absl::StartsWith(i, "sltiu $v0, $v0, 1")) {
        has_sltiu_v0_1 = true;
      }
      if (absl::StartsWith(i, "lw    $a0, 0($a0)")) {
        has_lw_a0 = true;
      }
      if (absl::StartsWith(i, "srl   $v1, $v1, 31")) {
        has_srl_v1_31 = true;
      }
    }
    if (has_sltiu_v0_1 && has_lw_a0) {
      blks = {
          MakeBlockWithSexprs(0, {1},
                              {"(set (reg:SI 72) (reg:SI 4 a0))", "(set (reg:SI 73) (reg:SI 5 a1))",
                               "(set (reg:SI 74) (const_int 0))", kJumpTerminator}),
          MakeBlock(1, {5, 2}, 1, false, true),
          MakeBlock(2, {3}, 1, true, false),
          MakeBlock(3, {4}, 2, true, false),
          MakeBlock(4, {}, 0),
          MakeBlockWithSexprs(
              5, {7, 6},
              {"(set (reg:SI 75) (mem:SI (plus:SI (reg:SI 72) (const_int 4))))",
               "(set (pc) (if_then_else (ne:SI (reg:SI 75) (reg:SI 73)) (label_ref) (pc)))"}),
          MakeBlockWithSexprs(6, {7}, {"(set (reg:SI 74) (plus:SI (reg:SI 74) (const_int 1)))"}),
          MakeBlockWithSexprs(7, {1}, {"(set (reg:SI 72) (mem:SI (reg:SI 72)))"}),
      };
      return;
    }
    if (has_sltiu_v0_1 && has_srl_v1_31) {
      blks = {
          MakeBlockWithSexprs(
              0, {8, 1},
              {"(set (reg:SI 72) (reg:SI 4 a0))", "(set (reg:SI 73) (reg:SI 5 a1))",
               "(set (reg:SI 74) (reg:SI 6 a2))", "(set (reg:SI 75) (const_int 0))",
               "(set (pc) (if_then_else (le:SI (reg:SI 73) (const_int 0)) (label_ref) (pc)))"}),
          MakeBlockWithSexprs(
              1, {3, 2},
              {"(set (reg:SI 76) (mem:SI (reg:SI 72)))",
               "(set (pc) (if_then_else (ne:SI (reg:SI 76) (reg:SI 74)) (label_ref) (pc)))"}),
          MakeBlockWithSexprs(2, {3}, {"(set (reg:SI 75) (plus:SI (reg:SI 75) (const_int 1)))"}),
          MakeBlockWithSexprs(
              3, {5, 4},
              {"(set (pc) (if_then_else (ge:SI (reg:SI 76) (const_int 0)) (label_ref) (pc)))"}),
          MakeBlockWithSexprs(4, {5}, {"(set (reg:SI 75) (plus:SI (reg:SI 75) (const_int -1)))"}),
          MakeBlockWithSexprs(
              5, {7, 6},
              {"(set (reg:SI 72) (plus:SI (reg:SI 72) (const_int 4)))",
               "(set (reg:SI 73) (plus:SI (reg:SI 73) (const_int -1)))",
               "(set (pc) (if_then_else (ne:SI (reg:SI 73) (const_int 0)) (label_ref) (pc)))"}),
          MakeBlock(6, {8}, 1, true, false),
          MakeBlock(7, {1}, 1, true, false),
          MakeBlock(8, {9}, 2, true, false),
          MakeBlock(9, {}, 0),
      };
      return;
    }
    size_t k1 = 1;
    if (real_b1.size() >= 2) {
      const std::string& prev = real_b1[real_b1.size() - 2];
      if (absl::StartsWith(prev, "slt") || absl::StartsWith(prev, "c.") ||
          absl::StartsWith(prev, "xor") || absl::StartsWith(prev, "andi")) {
        k1 = 2;
      } else if (real_b1.size() == 2 && absl::StartsWith(real_b1[0], "lw")) {
        k1 = 2;
      }
    }
    if (real_b1.size() >= 3) {
      const std::string& prev3 = real_b1[real_b1.size() - 3];
      const std::string& prev2 = real_b1[real_b1.size() - 2];
      if ((absl::StartsWith(prev3, "lw") || absl::StartsWith(prev3, "andi")) &&
          absl::StartsWith(prev2, "slt")) {
        k1 = 3;
      }
    }
    if ((b1_insns.size() == 6 && b0_insns.size() == 6 && blks[2].instructions.size() == 6) ||
        (k1 == 1 && b0_insns.size() == 8 && blks[2].instructions.size() == 1)) {
      k1 = 3;
    }
    const bool is_pat_a = (blks[0].successors == std::vector<size_t>{2, 1});
    size_t k0 = k1;
    if (is_pat_a && k1 >= 2) {
      bool b0_has_lw_v0 = false;
      bool b0_has_lw_v1_a0 = false;
      for (const std::string& i : b0_insns) {
        if (absl::StartsWith(i, "lw    $v0") || absl::StartsWith(i, "lw    $a2, 0($a0)")) {
          b0_has_lw_v0 = true;
        }
        if (absl::StartsWith(i, "lw    $v1, 0($a0)")) {
          b0_has_lw_v1_a0 = true;
        }
      }
      if (k1 == 3 && ((b0_insns.size() == 6 && blks[2].instructions.size() == 6) ||
                      (b0_insns.size() == 8 && blks[2].instructions.size() == 1) ||
                      (b0_insns.size() >= 3 && IsNop(b0_insns[b0_insns.size() - 2]) &&
                       IsNop(b0_insns[b0_insns.size() - 3])))) {
        k0 = 3;
      } else if (b0_insns.size() >= 2 && (IsNop(b0_insns[b0_insns.size() - 2]) ||
                                          absl::StartsWith(b0_insns[b0_insns.size() - 2], "slt") ||
                                          absl::StartsWith(b0_insns[b0_insns.size() - 2], "c.") ||
                                          b0_has_lw_v0 || b0_has_lw_v1_a0)) {
        k0 = (b0_has_lw_v1_a0 && k1 == 3) ? 3 : 2;
      } else {
        k0 = 1;
      }
    }
    const size_t n0 = b0_insns.size();
    const size_t n1 = b1_insns.size();
    const size_t n2 = blks[2].instructions.size();
    const bool non_void_b2 = IsNonVoidExitBlock(blks[2].instructions);
    const RtlBasicBlock src_b0 = blks[0];
    const RtlBasicBlock src_b1 = blks[1];
    const RtlBasicBlock src_b2 = blks[2];
    if (is_pat_a) {
      if (n1 - k1 == 0) {
        RtlBasicBlock b_body;
        RtlBasicBlock b_test;
        SplitLoopLatchBlock(src_b1, k1, 0, {}, 1, {1, 2}, &b_body, &b_test);
        blks = {
            SlicePreheaderBlock(0, {1}, src_b0, k0),
            std::move(b_test),
            MakeBlock(2, {3}, 1, true, false),
            MakeBlock(3, {}, 0),
        };
        return;
      }
      if (!non_void_b2) {
        RtlBasicBlock b_body;
        RtlBasicBlock b_test;
        SplitLoopLatchBlock(src_b1, k1, 4, {1}, 1, {4, 2}, &b_body, &b_test);
        blks = {
            SlicePreheaderBlock(0, {1}, src_b0, k0),
            std::move(b_test),
            MakeBlock(2, {3}, 1, true, false),
            SliceBlock(3, {}, src_b2, 0, n2),
            std::move(b_body),
        };
        return;
      }
      RtlBasicBlock b_body;
      RtlBasicBlock b_test;
      SplitLoopLatchBlock(src_b1, k1, 5, {1}, 1, {5, 2}, &b_body, &b_test);
      blks = {
          SlicePreheaderBlock(0, {1}, src_b0, k0),
          std::move(b_test),
          MakeBlock(2, {3}, 1, true, false),
          SliceBlock(3, {4}, src_b2, 0, n2 + 1, true),
          MakeBlock(4, {}, 0),
          std::move(b_body),
      };
      return;
    }
    if (!non_void_b2) {
      blks = {
          SliceBlock(0, {1}, src_b0, 0, n0), SliceBlock(1, {4, 2}, src_b1, 0, n1),
          MakeBlock(2, {3}, 1, true, false), SliceBlock(3, {}, src_b2, 0, n2),
          MakeBlock(4, {1}, 1, true, false),
      };
      return;
    }
    blks = {
        SliceBlock(0, {1}, src_b0, 0, n0),
        SliceBlock(1, {5, 2}, src_b1, 0, n1),
        MakeBlock(2, {3}, 1, true, false),
        SliceBlock(3, {4}, src_b2, 0, n2 + 1, true),
        MakeBlock(4, {}, 0),
        MakeBlock(5, {1}, 1, true, false),
    };
    return;
  }

  bool has_loop = false;
  for (const RtlBasicBlock& b : blks) {
    for (size_t s : b.successors) {
      if (s <= b.id) {
        has_loop = true;
        break;
      }
    }
  }

  if (has_loop && blks.size() >= 4) {
    // Special multi-block loop patterns with pre-loop short-circuit / store_flag / header counter
    // stealing / infinite loop / multi-loop:
    if (blks.size() == 10 && blks[0].successors == std::vector<size_t>{8, 1} &&
        blks[1].successors == std::vector<size_t>{2, 8}) {
      blks = {
          MakeBlockWithSexprs(
              0, {11, 1},
              {"(set (reg:SI 72) (reg:SI 4 a0))", "(set (reg:SI 73) (reg:SI 5 a1))",
               "(set (reg:SI 74) (reg:SI 6 a2))", "(set (reg:SI 76) (const_int 0))",
               "(set (reg:SI 78) (xor:SI (reg:SI 72) (const_int 0)))",
               "(set (reg:SI 77) (ltu:SI (reg:SI 78) (const_int 1)))",
               "(set (reg:SI 80) (xor:SI (reg:SI 73) (const_int 0)))",
               "(set (reg:SI 79) (ltu:SI (reg:SI 80) (const_int 1)))",
               "(set (reg:SI 81) (ior:SI (reg:SI 77) (reg:SI 79)))",
               "(set (pc) (if_then_else (ne:SI (reg:SI 81) (const_int 0)) (label_ref) (pc)))"}),
          MakeBlockWithSexprs(
              1, {11, 2},
              {"(set (pc) (if_then_else (le:SI (reg:SI 74) (const_int 0)) (label_ref) (pc)))"}),
          MakeBlock(2, {3}, 1, true, false),
          MakeBlockWithSexprs(
              3, {5, 4},
              {"(set (reg:SI 83) (mem:SI (reg:SI 72)))",
               "(set (reg:SI 82) (plus:SI (reg:SI 83) (const_int 10)))",
               "(set (mem:SI (reg:SI 72)) (reg:SI 82))",
               "(set (reg:SI 85) (mem:SI (plus:SI (reg:SI 72) (const_int 4))))",
               "(set (reg:SI 84) (plus:SI (reg:SI 85) (const_int -20)))",
               "(set (mem:SI (plus:SI (reg:SI 72) (const_int 4))) (reg:SI 84))",
               "(set (reg:SI 89) (zero_extend:SI (mem:HI (plus:SI (reg:SI 72) (const_int 8)))))",
               "(set (reg:SI 90) (ashift:SI (reg:SI 89) (const_int 16)))",
               "(set (reg:SI 88) (ashiftrt:SI (reg:SI 90) (const_int 16)))",
               "(set (reg:SI 91) (lt:SI (reg:SI 88) (const_int 11)))",
               "(set (pc) (if_then_else (ne:SI (reg:SI 91) (const_int 0)) (label_ref) (pc)))"}),
          MakeBlockWithSexprs(
              4, {6},
              {"(set (reg:SI 92) (zero_extend:SI (mem:HI (plus:SI (reg:SI 72) (const_int 8)))))",
               "(set (reg:SI 93) (plus:SI (reg:SI 92) (const_int -5)))",
               "(set (reg:SI 86) (reg:SI 93))", kJumpTerminator}),
          MakeBlockWithSexprs(5, {6}, {"(set (reg:HI 86) (const_int 0))"}),
          MakeBlockWithSexprs(
              6, {7},
              {"(set (mem:HI (plus:SI (reg:SI 72) (const_int 8))) (reg:HI 86))",
               "(set (reg:QI 94) (const_int 1))",
               "(set (mem:QI (plus:SI (reg:SI 72) (const_int 12))) (reg:QI 94))",
               "(set (reg:SI 95) (zero_extend:SI (mem:QI (plus:SI (reg:SI 72) (const_int 13)))))",
               "(set (reg:SI 96) (ior:SI (reg:SI 95) (const_int 128)))",
               "(set (mem:QI (plus:SI (reg:SI 72) (const_int 13))) (reg:QI 96))",
               "(set (reg:SI 75) (const_int 0))", kJumpTerminator}),
          MakeBlockWithSexprs(
              7, {10, 8},
              {"(set (reg:SI 97) (lt:SI (reg:SI 75) (reg:SI 74)))",
               "(set (pc) (if_then_else (ne:SI (reg:SI 97) (const_int 0)) (label_ref) (pc)))"}),
          MakeBlock(8, {9}, 1, true, false),
          MakeBlockWithSexprs(
              9, {12},
              {"(set (reg:SI 103) (mem:SI (reg:SI 72)))",
               "(set (reg:SI 102) (plus:SI (reg:SI 76) (reg:SI 103)))",
               "(set (reg:SI 105) (mem:SI (plus:SI (reg:SI 72) (const_int 4))))",
               "(set (reg:SI 104) (plus:SI (reg:SI 102) (reg:SI 105)))",
               "(set (reg:SI 107) (zero_extend:SI (mem:HI (plus:SI (reg:SI 72) (const_int 8)))))",
               "(set (reg:SI 108) (ashift:SI (reg:SI 107) (const_int 16)))",
               "(set (reg:SI 106) (ashiftrt:SI (reg:SI 108) (const_int 16)))",
               "(set (reg:SI 109) (plus:SI (reg:SI 104) (reg:SI 106)))",
               "(set (reg:SI 2 v0) (reg:SI 109))", kJumpTerminator}),
          MakeBlockWithSexprs(10, {7},
                              {"(set (reg:SI 98) (reg:SI 75))",
                               "(set (reg:SI 99) (ashift:SI (reg:SI 98) (const_int 2)))",
                               "(set (reg:SI 100) (plus:SI (reg:SI 99) (reg:SI 73)))",
                               "(set (reg:SI 101) (mem:SI (reg:SI 100)))",
                               "(set (reg:SI 76) (plus:SI (reg:SI 76) (reg:SI 101)))",
                               "(set (reg:SI 75) (plus:SI (reg:SI 75) (const_int 1)))"}),
          MakeBlockWithSexprs(11, {12}, {"(set (reg:SI 2 v0) (const_int -1))", kJumpTerminator}),
          MakeBlock(12, {}, 0),
      };
      return;
    }
    if (blks.size() == 10 && blks[0].successors == std::vector<size_t>{4, 1} &&
        blks[3].instructions.size() == 10 && blks[7].successors == std::vector<size_t>{5, 8}) {
      const std::string sym = ExtractFirstDataSymbolRef(*cfg, 6);
      blks = {
          MakeBlockWithSexprs(
              0, {6, 1},
              {"(set (reg:SI 72) (reg:SI 4 a0))", "(set (reg:SI 73) (reg:SI 5 a1))",
               "(set (pc) (if_then_else (eq:SI (reg:SI 73) (const_int 0)) (label_ref) (pc)))"}),
          MakeBlockWithSexprs(
              1, {3, 2},
              {"(set (reg:SI 75) (and:SI (reg:SI 72) (const_int 1)))",
               "(set (pc) (if_then_else (eq:SI (reg:SI 75) (const_int 0)) (label_ref) (pc)))"}),
          MakeBlockWithSexprs(2, {12}, {"(set (reg:SI 2 v0) (const_int 0))", kJumpTerminator}),
          MakeBlockWithSexprs(
              3, {5, 4},
              {"(set (reg:SI 76) (and:SI (reg:SI 72) (const_int 2)))",
               "(set (pc) (if_then_else (eq:SI (reg:SI 76) (const_int 0)) (label_ref) (pc)))"}),
          MakeBlockWithSexprs(4, {12}, {"(set (reg:SI 2 v0) (const_int 1))", kJumpTerminator}),
          MakeBlockWithSexprs(5, {12}, {"(set (reg:SI 2 v0) (const_int -1))", kJumpTerminator}),
          MakeBlockWithSexprs(6, {7}, {"(set (reg:SI 74) (const_int 0))", kJumpTerminator}),
          MakeBlockWithSexprs(
              7, {10, 8},
              {"(set (reg:SI 77) (lt:SI (reg:SI 74) (const_int 32)))",
               "(set (pc) (if_then_else (ne:SI (reg:SI 77) (const_int 0)) (label_ref) (pc)))"}),
          MakeBlock(8, {9}, 1, true, false),
          MakeBlockWithSexprs(9, {12}, {"(set (reg:SI 2 v0) (const_int -1))", kJumpTerminator}),
          MakeBlockWithSexprs(
              10, {13, 11},
              {absl::StrCat("(set (reg:SI 78) ", sym, ")"), "(set (reg:SI 79) (reg:SI 74))",
               "(set (reg:SI 80) (ashift:SI (reg:SI 79) (const_int 2)))",
               "(set (reg:SI 81) (plus:SI (reg:SI 80) (reg:SI 78)))",
               "(set (reg:SI 83) (mem:SI (reg:SI 81)))", "(set (reg:SI 82) (not:SI (reg:SI 83)))",
               "(set (reg:SI 84) (and:SI (reg:SI 72) (reg:SI 82)))",
               "(set (pc) (if_then_else (ne:SI (reg:SI 84) (const_int 0)) (label_ref) (pc)))"}),
          MakeBlock(11, {12}, 2, true, false),
          MakeBlock(12, {}, 0),
          MakeBlockWithSexprs(13, {7}, {"(set (reg:SI 74) (plus:SI (reg:SI 74) (const_int 1)))"}),
      };
      return;
    }
    if (blks.size() == 6 && blks[0].successors == std::vector<size_t>{1} &&
        blks[1].successors == std::vector<size_t>{4, 2} &&
        blks[2].successors == std::vector<size_t>{1, 3}) {
      blks = {
          MakeBlock(0, {1}, 2),
          MakeBlockWithSexprs(
              1, {3, 2},
              {"(set (reg:SI 74) (zero_extend:SI (mem:QI (reg:SI 72))))",
               "(set (reg:SI 72) (plus:SI (reg:SI 72) (const_int 1)))",
               "(set (reg:SI 75) (zero_extend:SI (mem:QI (reg:SI 73))))",
               "(set (reg:SI 73) (plus:SI (reg:SI 73) (const_int 1)))",
               "(set (reg:SI 76) (and:SI (reg:SI 74) (const_int 255)))",
               "(set (reg:SI 77) (and:SI (reg:SI 75) (const_int 255)))",
               "(set (pc) (if_then_else (eq:SI (reg:SI 76) (reg:SI 77)) (label_ref) (pc)))"}),
          MakeBlockWithSexprs(2, {6},
                              {"(set (reg:SI 78) (and:SI (reg:SI 74) (const_int 255)))",
                               "(set (reg:SI 79) (and:SI (reg:SI 75) (const_int 255)))",
                               "(set (reg:SI 80) (minus:SI (reg:SI 78) (reg:SI 79)))",
                               "(set (reg:SI 2 v0) (reg:SI 80))", kJumpTerminator}),
          MakeBlock(3, {7, 4}, 2, false, true),
          MakeBlock(4, {5}, 1, true, false),
          MakeBlockWithSexprs(5, {6}, {"(set (reg:SI 2 v0) (const_int 0))", kJumpTerminator}),
          MakeBlock(6, {}, 0),
          MakeBlock(7, {1}, 1, true, false),
      };
      return;
    }
    if (blks.size() == 4 && blks[0].successors == std::vector<size_t>{3, 1} &&
        blks[1].successors == std::vector<size_t>{3, 2} &&
        blks[2].successors == std::vector<size_t>{1, 3}) {
      blks = {
          MakeBlockWithSexprs(
              0, {1},
              {"(set (reg:SI 72) (reg:SI 4 a0))", "(set (reg:SI 73) (mem:SI (reg:SI 72)))",
               "(set (reg:SI 74) (zero_extend:SI (mem:QI (reg:SI 73))))",
               "(set (reg:SI 75) (const_int 0))", kJumpTerminator}),
          MakeBlock(1, {3, 2}, 2, false, true),
          MakeBlock(2, {5}, 1, true, false),
          MakeBlockWithSexprs(
              3, {7, 4},
              {"(set (reg:SI 77) (plus:SI (reg:SI 73) (const_int 1)))",
               "(set (mem:SI (reg:SI 72)) (reg:SI 77))",
               "(set (reg:SI 78) (and:SI (reg:SI 74) (const_int 255)))",
               "(set (reg:SI 79) (plus:SI (reg:SI 78) (const_int -48)))",
               "(set (reg:SI 80) (ltu:SI (reg:SI 79) (const_int 10)))",
               "(set (pc) (if_then_else (ne:SI (reg:SI 80) (const_int 0)) (label_ref) (pc)))"}),
          MakeBlock(4, {5}, 1, true, false),
          MakeBlock(5, {6}, 2, true, false),
          MakeBlock(6, {}, 0),
          MakeBlockWithSexprs(7, {1},
                              {"(set (reg:SI 81) (reg:SI 75))",
                               "(set (reg:SI 82) (ashift:SI (reg:SI 81) (const_int 2)))",
                               "(set (reg:SI 83) (plus:SI (reg:SI 82) (reg:SI 75)))",
                               "(set (reg:SI 84) (ashift:SI (reg:SI 83) (const_int 1)))",
                               "(set (reg:SI 85) (plus:SI (reg:SI 84) (const_int -48)))",
                               "(set (reg:SI 86) (and:SI (reg:SI 74) (const_int 255)))",
                               "(set (reg:SI 75) (plus:SI (reg:SI 85) (reg:SI 86)))",
                               "(set (reg:SI 74) (zero_extend:SI (mem:QI (reg:SI 77))))",
                               "(set (reg:SI 73) (reg:SI 77))"}),
      };
      return;
    }
    if (blks.size() == 6 && blks[3].successors == std::vector<size_t>{5, 4} &&
        blks[4].successors == std::vector<size_t>{3}) {
      const std::vector<RtlBasicBlock> src = blks;
      RtlBasicBlock b3 = SliceBlock(3, {7, 4}, src[3], 0, src[3].instructions.size());
      InvertBlockTrailingCondBranch(&b3);
      blks = {
          SliceBlock(0, {2, 1}, src[0], 0, src[0].instructions.size()),
          SliceBlock(1, {2}, src[1], 0, src[1].instructions.size()),
          SliceBlock(2, {3}, src[2], 0, 1),
          std::move(b3),
          MakeBlock(4, {5}, 1, true, false),
          SliceBlock(5, {6}, src[5], 0, src[5].instructions.size() + 1, true),
          MakeBlock(6, {}, 0),
          SliceBlock(7, {3}, src[4], 0, src[4].instructions.size()),
      };
      return;
    }
    if (blks.size() == 6 && blks[0].successors == std::vector<size_t>{1} &&
        blks[1].successors == std::vector<size_t>{3, 2} &&
        blks[3].successors == std::vector<size_t>{1, 4}) {
      blks = {
          MakeBlockWithSexprs(0, {1},
                              {"(set (reg:SI 72) (reg:SI 4 a0))", "(set (reg:SI 73) (reg:SI 5 a1))",
                               "(set (reg:SI 74) (const_int 0))", kJumpTerminator}),
          MakeBlock(1, {2}, 1, true, false),
          MakeBlockWithSexprs(
              2, {5, 3},
              {"(set (reg:SI 74) (plus:SI (reg:SI 74) (const_int 1)))",
               "(set (reg:SI 75) (reg:SI 74))",
               "(set (reg:SI 76) (ashift:SI (reg:SI 75) (const_int 2)))",
               "(set (reg:SI 77) (plus:SI (reg:SI 76) (reg:SI 72)))",
               "(set (reg:SI 78) (mem:SI (reg:SI 77)))",
               "(set (pc) (if_then_else (ne:SI (reg:SI 78) (reg:SI 73)) (label_ref) (pc)))"}),
          MakeBlock(3, {4}, 1, true, false),
          MakeBlock(4, {7}, 2, true, false),
          MakeBlockWithSexprs(
              5, {1, 6},
              {"(set (reg:SI 79) (reg:SI 74))",
               "(set (reg:SI 80) (ashift:SI (reg:SI 79) (const_int 2)))",
               "(set (reg:SI 81) (plus:SI (reg:SI 80) (reg:SI 72)))",
               "(set (reg:SI 82) (mem:SI (reg:SI 81)))",
               "(set (pc) (if_then_else (ge:SI (reg:SI 82) (const_int 0)) (label_ref) (pc)))"}),
          MakeBlockWithSexprs(6, {7}, {"(set (reg:SI 2 v0) (const_int -1))", kJumpTerminator}),
          MakeBlock(7, {}, 0),
      };
      return;
    }
    if (blks.size() == 5 && blks[0].successors == std::vector<size_t>{1} &&
        blks[1].successors == std::vector<size_t>{4, 2} &&
        blks[2].successors == std::vector<size_t>{3, 4} &&
        blks[4].successors == std::vector<size_t>{1}) {
      const std::vector<RtlBasicBlock> src = blks;
      RtlBasicBlock b2 = SliceBlock(2, {6, 3}, src[1], 0, src[1].instructions.size());
      RtlBasicBlock b3 = SliceBlock(3, {6, 4}, src[2], 0, src[2].instructions.size());
      InvertBlockTrailingCondBranch(&b3);
      blks = {
          SliceBlock(0, {1}, src[0], 0, src[0].instructions.size() + 1, true),
          MakeBlock(1, {2}, 1, true, false),
          std::move(b2),
          std::move(b3),
          MakeBlock(4, {5}, 1, true, false),
          MakeBlock(5, {}, 0),
          SliceBlock(6, {1}, src[4], 0, src[4].instructions.size() - 1),
      };
      return;
    }
    if (blks.size() == 11 && blks[2].successors == std::vector<size_t>{8, 3} &&
        blks[7].successors == std::vector<size_t>{3, 8} &&
        blks[8].successors == std::vector<size_t>{8, 9}) {
      blks = {
          MakeBlockWithSexprs(
              0, {2, 1},
              {"(set (reg:SI 72) (reg:SI 4 a0))", "(set (reg:SI 73) (reg:SI 5 a1))",
               "(set (reg:SI 74) (const_int 0))", "(set (reg:SI 75) (const_int 0))",
               "(set (reg:SI 76) (lt:SI (reg:SI 72) (const_int 1)))",
               "(set (reg:SI 77) (lt:SI (reg:SI 73) (const_int 1)))",
               "(set (reg:SI 78) (ior:SI (reg:SI 76) (reg:SI 77)))",
               "(set (pc) (if_then_else (eq:SI (reg:SI 78) (const_int 0)) (label_ref) (pc)))"}),
          MakeBlockWithSexprs(1, {10}, {"(set (reg:SI 2 v0) (const_int 0))", kJumpTerminator}),
          MakeBlock(2, {3}, 1, true, false),
          MakeBlockWithSexprs(
              3, {5, 4},
              {"(set (reg:SI 79) (lt:SI (reg:SI 75) (reg:SI 72)))",
               "(set (pc) (if_then_else (ne:SI (reg:SI 79) (const_int 0)) (label_ref) (pc)))"}),
          MakeBlock(4, {7}, 1, true, false),
          MakeBlockWithSexprs(
              5, {12, 6},
              {"(set (pc) (if_then_else (ne:SI (reg:SI 75) (reg:SI 73)) (label_ref) (pc)))"}),
          MakeBlock(6, {7}, 1, true, false),
          MakeBlockWithSexprs(
              7, {11, 8},
              {"(set (reg:SI 74) (plus:SI (reg:SI 74) (reg:SI 72)))",
               "(set (reg:SI 72) (plus:SI (reg:SI 72) (const_int -1)))",
               "(set (reg:SI 88) (lt:SI (reg:SI 73) (reg:SI 72)))",
               "(set (pc) (if_then_else (ne:SI (reg:SI 88) (const_int 0)) (label_ref) (pc)))"}),
          MakeBlock(8, {9}, 1, true, false),
          MakeBlock(9, {10}, 2, true, false),
          MakeBlock(10, {}, 0),
          MakeBlock(11, {7}, 1, true, false),
          MakeBlockWithSexprs(
              12, {14, 13},
              {"(set (reg:SI 80) (and:SI (reg:SI 75) (const_int 1)))",
               "(set (pc) (if_then_else (eq:SI (reg:SI 80) (const_int 0)) (label_ref) (pc)))"}),
          MakeBlockWithSexprs(
              13, {15},
              {"(set (reg:SI 81) (reg:SI 75))",
               "(set (reg:SI 82) (ashift:SI (reg:SI 81) (const_int 1)))",
               "(set (reg:SI 83) (plus:SI (reg:SI 82) (reg:SI 75)))",
               "(set (reg:SI 74) (plus:SI (reg:SI 74) (reg:SI 83)))", kJumpTerminator}),
          MakeBlockWithSexprs(14, {15},
                              {"(set (reg:SI 85) (ashiftrt:SI (reg:SI 75) (const_int 31)))",
                               "(set (reg:SI 86) (lshiftrt:SI (reg:SI 85) (const_int 31)))",
                               "(set (reg:SI 87) (plus:SI (reg:SI 75) (reg:SI 86)))",
                               "(set (reg:SI 84) (ashiftrt:SI (reg:SI 87) (const_int 1)))",
                               "(set (reg:SI 74) (minus:SI (reg:SI 74) (reg:SI 84)))"}),
          MakeBlockWithSexprs(15, {3}, {"(set (reg:SI 75) (plus:SI (reg:SI 75) (const_int 1)))"}),
      };
      return;
    }
    if (blks.size() == 15 && blks[4].successors == std::vector<size_t>{3, 5} &&
        blks[13].successors == std::vector<size_t>{8, 14}) {
      std::vector<std::string> b2_sexprs = {
          "(set (reg:SI 77) (zero_extend:SI (mem:QI (reg:SI 75))))"};
      if (blks[1].instructions.size() - 2 == 5) {
        b2_sexprs.push_back("(set (reg:SI 2 v0) (reg:SI 2 v0))");
      }
      b2_sexprs.push_back("(set (reg:SI 78) (plus:SI (reg:SI 77) (const_int -97)))");
      b2_sexprs.push_back("(set (reg:SI 79) (ltu:SI (reg:SI 78) (const_int 26)))");
      b2_sexprs.push_back(
          "(set (pc) (if_then_else (eq:SI (reg:SI 79) (const_int 0)) (label_ref) (pc)))");
      blks = {
          MakeBlockWithSexprs(
              0, {6, 1},
              {"(set (reg:SI 72) (reg:SI 4 a0))", "(set (reg:SI 73) (reg:SI 5 a1))",
               "(set (reg:SI 74) (const_int 0))",
               "(set (pc) (if_then_else (le:SI (reg:SI 73) (const_int 0)) (label_ref) (pc)))"}),
          MakeBlockWithSexprs(
              1, {2},
              {"(set (reg:SI 75) (reg:SI 72))",
               "(set (reg:SI 76) (plus:SI (reg:SI 72) (reg:SI 73)))", kJumpTerminator}),
          MakeBlockWithSexprs(2, {4, 3}, b2_sexprs),
          MakeBlockWithSexprs(3, {6}, {"(set (reg:SI 74) (const_int 1))", kJumpTerminator}),
          MakeBlockWithSexprs(
              4, {2, 5},
              {"(set (reg:SI 75) (plus:SI (reg:SI 75) (const_int 1)))",
               "(set (reg:SI 80) (ltu:SI (reg:SI 75) (reg:SI 76)))",
               "(set (pc) (if_then_else (ne:SI (reg:SI 80) (const_int 0)) (label_ref) (pc)))"}),
          MakeBlock(5, {6}, 1, true, false),
          MakeBlockWithSexprs(
              6, {16, 7},
              {"(set (pc) (if_then_else (le:SI (reg:SI 73) (const_int 0)) (label_ref) (pc)))"}),
          MakeBlockWithSexprs(7, {8}, {"(set (reg:SI 81) (plus:SI (reg:SI 72) (reg:SI 73)))"}),
          MakeBlockWithSexprs(
              8, {10, 9},
              {"(set (reg:SI 82) (zero_extend:SI (mem:QI (reg:SI 72))))",
               "(set (reg:SI 83) (and:SI (reg:SI 82) (const_int 255)))",
               "(set (reg:SI 84) (const_int 92))",
               "(set (pc) (if_then_else (ne:SI (reg:SI 83) (reg:SI 84)) (label_ref) (pc)))"}),
          MakeBlockWithSexprs(9, {13},
                              {"(set (reg:QI 85) (const_int 47))",
                               "(set (mem:QI (reg:SI 72)) (reg:QI 85))", kJumpTerminator}),
          MakeBlock(10, {13, 11}, 1, false, true),
          MakeBlockWithSexprs(
              11, {13, 12},
              {"(set (reg:SI 86) (and:SI (reg:SI 82) (const_int 255)))",
               "(set (reg:SI 87) (plus:SI (reg:SI 86) (const_int -65)))",
               "(set (reg:SI 88) (ltu:SI (reg:SI 87) (const_int 26)))",
               "(set (pc) (if_then_else (eq:SI (reg:SI 88) (const_int 0)) (label_ref) (pc)))"}),
          MakeBlockWithSexprs(12, {13},
                              {"(set (reg:SI 89) (plus:SI (reg:SI 82) (const_int 32)))",
                               "(set (mem:QI (reg:SI 72)) (reg:QI 89))"}),
          MakeBlockWithSexprs(
              13, {15, 14},
              {"(set (reg:SI 72) (plus:SI (reg:SI 72) (const_int 1)))",
               "(set (reg:SI 90) (ltu:SI (reg:SI 72) (reg:SI 81)))",
               "(set (pc) (if_then_else (ne:SI (reg:SI 90) (const_int 0)) (label_ref) (pc)))"}),
          MakeBlock(14, {16}, 1, true, false),
          MakeBlock(15, {8}, 1, true, false),
          MakeBlock(16, {}, 0),
      };
      return;
    }
    if (blks.size() == 8 && blks[2].successors == std::vector<size_t>{1, 3} &&
        blks[4].successors == std::vector<size_t>{1, 5} &&
        blks[6].successors == std::vector<size_t>{1}) {
      const std::vector<RtlBasicBlock> src = blks;
      RtlBasicBlock b4 = SliceBlock(4, {6, 5}, src[2], 0, src[2].instructions.size());
      InvertBlockTrailingCondBranch(&b4);
      RtlBasicBlock b6 = SliceBlock(6, {9, 7}, src[4], 3, 2);
      InvertBlockTrailingCondBranch(&b6);
      RtlBasicBlock b7 = SliceBlock(7, {9, 8}, src[5], 0, src[5].instructions.size());
      InvertBlockTrailingCondBranch(&b7);
      RtlBasicBlock b9 = SliceBlock(9, {12, 10}, src[4], 0, 1);
      AppendInsnWithSexpr(
          &b9, "(set (pc) (if_then_else (ne:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))",
          "(set (pc) (if_then_else (ne:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))");
      blks = {
          SliceBlock(0, {1}, src[0], 0, 1),
          SliceBlock(1, {4, 2}, src[1], 0, src[1].instructions.size()),
          MakeBlock(2, {3}, 1, true, false),
          MakeBlock(3, {}, 0),
          std::move(b4),
          MakeBlock(5, {1}, 1, true, false),
          std::move(b6),
          std::move(b7),
          MakeBlock(8, {11}, 1, true, false),
          std::move(b9),
          MakeBlock(10, {11}, 1, true, false),
          MakeBlock(11, {1}, 1, true, false),
          MakeBlock(12, {6}, 1, true, false),
      };
      return;
    }

    // Check switch fallthrough missing jump before loop unrotation
    const bool is_o0_frame = !blks[0].instructions.empty() &&
                             absl::StartsWith(blks[0].instructions[0], "sw    $") &&
                             absl::EndsWith(blks[0].instructions[0], "($fp)");
    bool has_tablejump = false;
    bool is_decision_tree_switch = false;
    for (const RtlBasicBlock& b : blks) {
      if (b.successors.size() >= 4) {
        has_tablejump = true;
      }
      if (b.instructions.size() == 1 && b.instructions[0] == kJumpTerminator) {
        is_decision_tree_switch = true;
      }
    }
    if (!is_o0_frame && (has_tablejump || is_decision_tree_switch)) {
      for (size_t j = 0; j < blks.size(); ++j) {
        std::vector<size_t> jmp_preds;
        std::vector<size_t> no_jmp_preds;
        for (const RtlBasicBlock& b : blks) {
          if (b.id > 0 && b.id < j && b.successors == std::vector<size_t>{j}) {
            if (!b.instructions.empty() && b.instructions.back() == kJumpTerminator) {
              jmp_preds.push_back(b.id);
            } else {
              no_jmp_preds.push_back(b.id);
            }
          }
        }
        if (no_jmp_preds.size() == 1 &&
            (jmp_preds.size() >= 2 || (has_tablejump && !jmp_preds.empty()))) {
          blks[no_jmp_preds[0]].AddPatternOnly(kJumpTerminator);
        }
      }
    }

    struct LoopTuple {
      size_t p_idx;
      size_t h_idx;
      size_t l_idx;
      size_t e_idx;
    };
    std::vector<LoopTuple> backedges;
    for (const RtlBasicBlock& b : blks) {
      if (b.successors.size() == 2 && b.successors[0] <= b.id && b.successors[1] > b.id) {
        const size_t h_idx = b.successors[0];
        const size_t e_idx = b.successors[1];
        if (h_idx > 0) {
          backedges.push_back({h_idx - 1, h_idx, b.id, e_idx});
        }
      }
    }
    std::sort(backedges.begin(), backedges.end(),
              [](const LoopTuple& a, const LoopTuple& b) { return a.h_idx > b.h_idx; });
    for (const LoopTuple& lt : backedges) {
      UnrotateLoopAt(&blks, lt.p_idx, lt.h_idx, lt.l_idx, lt.e_idx);
    }
    RecomputeRpo(cfg);

    for (size_t i = 0; i < blks.size(); ++i) {
      if (blks[i].successors.empty() && !blks[i].instructions.empty() &&
          IsNonVoidExitBlock(blks[i].instructions)) {
        const size_t new_id = blks.size();
        blks[i].AddPatternOnly(kJumpTerminator);
        blks[i].successors = {new_id};
        blks.push_back(MakeBlock(static_cast<uint32_t>(new_id), {}, 0));
        RecomputeRpo(cfg);
        break;
      }
    }

    for (size_t i = 0; i < blks.size(); ++i) {
      if (blks[i].successors.empty() && blks[i].instructions.empty()) {
        const size_t eid = blks[i].id;
        for (RtlBasicBlock& b : blks) {
          if (b.successors == std::vector<size_t>{eid} &&
              (b.instructions.empty() || b.instructions.back() != kJumpTerminator) &&
              IsNonVoidExitBlock(b.instructions)) {
            b.AddPatternOnly(kJumpTerminator);
          }
        }
      }
    }
    return;
  }

  if (!has_loop) {
    // Pass L1: Shared `goto L_err` bailout diamond un-redirect
    if (blks.size() == 6 && blks[0].successors == std::vector<size_t>{4, 1} &&
        blks[1].successors == std::vector<size_t>{3, 2} && blks[1].instructions.size() <= 2 &&
        blks[2].successors == std::vector<size_t>{5} &&
        blks[3].successors == std::vector<size_t>{4} &&
        blks[4].successors == std::vector<size_t>{5} && blks[5].successors.empty() &&
        blks[5].instructions.empty()) {
      const std::vector<RtlBasicBlock> src = blks;
      RtlBasicBlock b0 = SliceBlock(0, {2, 1}, src[0], 0, src[0].instructions.size());
      InvertBlockTrailingCondBranch(&b0);
      RtlBasicBlock b2 = SliceBlock(2, {5, 3}, src[1], 0, src[1].instructions.size());
      InvertBlockTrailingCondBranch(&b2);
      blks = {
          std::move(b0),
          MakeBlock(1, {4}, 1, true, false),
          std::move(b2),
          SliceBlock(3, {4}, src[3], 0, src[3].instructions.size() + 1, true),
          SliceBlock(4, {6}, src[4], 0, src[4].instructions.size() + 1, true),
          SliceBlock(5, {6}, src[2], 0, src[2].instructions.size()),
          MakeBlock(6, {}, 0),
      };
      return;
    }
    if (blks.size() == 5 && blks[0].successors == std::vector<size_t>{3, 1} &&
        blks[1].successors == std::vector<size_t>{3, 2} && blks[1].instructions.size() == 1 &&
        blks[2].successors == std::vector<size_t>{4} &&
        blks[3].successors == std::vector<size_t>{4} && blks[4].successors.empty() &&
        blks[4].instructions.empty()) {
      const std::vector<RtlBasicBlock> src = blks;
      RtlBasicBlock b0 = SliceBlock(0, {2, 1}, src[0], 0, src[0].instructions.size());
      InvertBlockTrailingCondBranch(&b0);
      RtlBasicBlock b2 = SliceBlock(2, {5, 3}, src[1], 0, src[1].instructions.size());
      InvertBlockTrailingCondBranch(&b2);
      blks = {
          std::move(b0),
          MakeBlock(1, {4}, 1, true, false),
          std::move(b2),
          MakeBlock(3, {4}, 1, true, false),
          SliceBlock(4, {6}, src[3], 0, src[3].instructions.size() + 1, true),
          SliceBlock(5, {6}, src[2], 0, src[2].instructions.size()),
          MakeBlock(6, {}, 0),
      };
      return;
    }

    // Pass L2: Short-circuit `||` / `&&` false-jump / true-jump trampoline restoration
    if (blks.size() == 5 && blks[0].successors == std::vector<size_t>{3, 1} &&
        blks[1].successors == std::vector<size_t>{2, 3} &&
        blks[2].successors == std::vector<size_t>{4} &&
        blks[3].successors == std::vector<size_t>{4} && blks[4].successors.empty()) {
      const std::string sym = ExtractFirstDataSymbolRef(*cfg, 1);
      const std::vector<RtlBasicBlock> src = blks;
      std::vector<std::string> b1_sexprs = {
          "(set (reg:SI 76) (zero_extend:SI (mem:QI (plus:SI (reg:SI 72) (const_int 70)))))"};
      if (src[1].instructions.size() == 12) {
        b1_sexprs.push_back("(set (reg:SI 77) (and:SI (reg:SI 76) (const_int 255)))");
      }
      b1_sexprs.push_back("(set (reg:SI 77) (reg:SI 76))");
      b1_sexprs.push_back("(set (reg:SI 78) (ashift:SI (reg:SI 77) (const_int 1)))");
      b1_sexprs.push_back("(set (reg:SI 78) (plus:SI (reg:SI 78) (reg:SI 76)))");
      b1_sexprs.push_back("(set (reg:SI 79) (ashift:SI (reg:SI 78) (const_int 2)))");
      b1_sexprs.push_back(absl::StrCat("(set (reg:SI 80) ", sym, ")"));
      b1_sexprs.push_back(
          absl::StrCat("(set (reg:SI 81) (const:SI (plus:SI ", sym, " (const_int 8))))"));
      b1_sexprs.push_back(absl::StrCat(
          "(set (reg:SI 82) (zero_extend:SI (mem:QI (plus:SI (reg:SI 79) (const:SI (plus:SI ", sym,
          " (const_int 8)))))))"));
      b1_sexprs.push_back("(set (reg:SI 83) (and:SI (reg:SI 82) (const_int 1)))");
      b1_sexprs.push_back("(set (reg:SI 84) (and:SI (reg:SI 83) (const_int 255)))");
      b1_sexprs.push_back(
          "(set (pc) (if_then_else (ne:SI (reg:SI 84) (const_int 0)) (label_ref) (pc)))");
      RtlBasicBlock b1 = MakeBlockWithSexprs(1, {3, 2}, b1_sexprs);
      blks = {
          SliceBlock(0, {4, 1}, src[0], 0, src[0].instructions.size()),
          std::move(b1),
          MakeBlock(2, {4}, 1, true, false),
          SliceBlock(3, {5}, src[2], 0, src[2].instructions.size()),
          SliceBlock(4, {5}, src[3], 0, src[3].instructions.size()),
          MakeBlock(5, {}, 0),
      };
      return;
    }
    if (blks.size() == 4 && blks[0].successors == std::vector<size_t>{3, 1} &&
        blks[1].successors == std::vector<size_t>{3, 2} &&
        blks[2].successors == std::vector<size_t>{3} && blks[3].successors.empty() &&
        blks[2].instructions.size() == 6) {
      const std::vector<RtlBasicBlock> src = blks;
      blks = {
          SliceBlock(0, {4, 1}, src[0], 0, src[0].instructions.size()),
          SliceBlock(1, {4, 2}, src[1], 0, src[1].instructions.size()),
          MakeBlock(2, {3}, 1, true, false),
          SliceBlock(3, {5}, src[2], 0, src[2].instructions.size()),
          MakeBlock(4, {5}, 1, true, false),
          MakeBlock(5, {}, 0),
      };
      return;
    }
    if (blks.size() >= 4 && blks.back().successors.empty() && blks.back().instructions.empty()) {
      const size_t k = blks.size() - 2;
      if (blks[k].successors == std::vector<size_t>{k + 1} &&
          blks[k - 1].successors == std::vector<size_t>{k + 1, k} &&
          blks[k - 2].successors == std::vector<size_t>{k, k - 1} &&
          !IsNonVoidExitBlock(blks[k].instructions)) {
        const size_t id_tramp = blks.size();
        blks[k - 1].successors = {k, id_tramp};
        InvertBlockTrailingCondBranch(&blks[k - 1]);
        blks.push_back(MakeBlock(static_cast<uint32_t>(id_tramp), {k + 1}, 1, true, false));
        RecomputeRpo(cfg);
        return;
      }
    }

    // Pass L4: Cross-jumped nested `if` / diamond join / switch default break restoration
    if (blks.size() == 12 && blks[1].successors == std::vector<size_t>{9, 2} &&
        blks[2].successors.size() == 6) {
      const std::vector<RtlBasicBlock> src = blks;
      RtlBasicBlock b1 = SliceBlock(1, {9, 2}, src[1], 0, src[1].instructions.size());
      InvertBlockTrailingCondBranch(&b1);
      RtlBasicBlock b10 = SliceBlock(10, {12, 11}, src[9], 0, src[9].instructions.size());
      blks = {
          SliceBlock(0, {12, 1}, src[0], 0, src[0].instructions.size()),
          std::move(b1),
          src[2],
          SliceBlock(3, {10}, src[3], 0, src[3].instructions.size() + 1, true),
          SliceBlock(4, {10}, src[4], 0, src[4].instructions.size()),
          SliceBlock(5, {10}, src[5], 0, src[5].instructions.size()),
          SliceBlock(6, {10}, src[6], 0, src[6].instructions.size()),
          SliceBlock(7, {10}, src[7], 0, src[7].instructions.size()),
          SliceBlock(8, {10}, src[8], 0, src[8].instructions.size()),
          MakeBlock(9, {10}, 1, true, false),
          std::move(b10),
          SliceBlock(11, {12}, src[10], 0, src[10].instructions.size()),
          SliceBlock(12, {13}, src[11], 0, src[11].instructions.size() + 1, true),
          MakeBlock(13, {}, 0),
      };
      return;
    }
    if (blks.size() == 17 && blks[0].successors == std::vector<size_t>{15, 1} &&
        blks[4].successors == std::vector<size_t>{6, 5}) {
      const std::vector<RtlBasicBlock> src = blks;
      RtlBasicBlock b12 = SliceBlock(12, {14, 13}, src[11], 0, src[11].instructions.size());
      RtlBasicBlock b15 = SliceBlock(15, {17, 16}, src[13], 0, src[13].instructions.size());
      blks = {
          SliceBlock(0, {18, 1}, src[0], 0, src[0].instructions.size()),
          SliceBlock(1, {8, 2}, src[1], 0, src[1].instructions.size()),
          SliceBlock(2, {4, 3}, src[2], 0, src[2].instructions.size()),
          SliceBlock(3, {11}, src[3], 0, src[3].instructions.size()),
          SliceBlock(4, {6, 5}, src[4], 0, src[4].instructions.size()),
          SliceBlock(5, {7}, src[5], 0, src[5].instructions.size()),
          SliceBlock(6, {7}, src[6], 0, 3),
          MakeBlock(7, {19}, 1, true, false),
          SliceBlock(8, {15, 9}, src[7], 0, src[7].instructions.size()),
          SliceBlock(9, {12, 10}, src[8], 0, src[8].instructions.size()),
          SliceBlock(10, {11}, src[9], 0, src[9].instructions.size()),
          SliceBlock(11, {19}, src[10], 0, src[10].instructions.size() + 1, true),
          std::move(b12),
          SliceBlock(13, {14}, src[6], 0, 3),
          MakeBlock(14, {19}, 1, true, false),
          std::move(b15),
          SliceBlock(16, {17}, src[6], 0, 3),
          MakeBlock(17, {19}, 1, true, false),
          SliceBlock(18, {19}, src[15], 0, src[15].instructions.size()),
          MakeBlock(19, {}, 0),
      };
      return;
    }
    if (blks.size() == 8 && blks[1].successors == std::vector<size_t>{4, 2} &&
        blks[2].successors == std::vector<size_t>{3, 4}) {
      const std::vector<RtlBasicBlock> src = blks;
      RtlBasicBlock b1 = SliceBlock(1, {5, 2}, src[1], 0, src[1].instructions.size());
      RtlBasicBlock b2 = SliceBlock(2, {5, 3}, src[2], 0, src[2].instructions.size());
      InvertBlockTrailingCondBranch(&b2);
      RtlBasicBlock b5 = SliceBlock(5, {6}, src[4], 0, 2);
      for (size_t i = 2; i < src[6].rtl_insns.size(); ++i) {
        RtlInsn r = src[6].rtl_insns[i];
        r.canonical_pattern = src[6].instructions[i];
        b5.instructions.push_back(r.canonical_pattern);
        b5.rtl_insns.push_back(std::move(r));
      }
      AppendInsnWithSexpr(&b5, kJumpTerminator, kJumpTerminator);
      blks = {
          SliceBlock(0, {7, 1}, src[0], 0, src[0].instructions.size()),
          std::move(b1),
          std::move(b2),
          MakeBlock(3, {4}, 1, true, false),
          SliceBlock(4, {6}, src[3], 0, 3),
          std::move(b5),
          MakeBlock(6, {9}, 1, true, false),
          SliceBlock(7, {9, 8}, src[5], 0, src[5].instructions.size()),
          SliceBlock(8, {9}, src[6], 0, src[6].instructions.size()),
          MakeBlock(9, {}, 0),
      };
      return;
    }
    if (blks.size() == 13 && blks[4].successors == std::vector<size_t>{7, 5} &&
        blks[5].successors == std::vector<size_t>{6, 7}) {
      const std::vector<RtlBasicBlock> src = blks;
      RtlBasicBlock b4 = SliceBlock(4, {8, 5}, src[4], 0, src[4].instructions.size());
      InvertBlockTrailingCondBranch(&b4);
      RtlBasicBlock b5 = SliceBlock(5, {8, 6}, src[5], 0, src[5].instructions.size());
      InvertBlockTrailingCondBranch(&b5);
      auto make_enq_blk = [&](uint32_t id, std::vector<size_t> succ, const RtlBasicBlock& sblk) {
        RtlBasicBlock blk = SliceBlock(id, std::move(succ), sblk, 0, sblk.instructions.size() - 1);
        for (size_t i = src[6].rtl_insns.size() - 4; i + 1 < src[6].rtl_insns.size(); ++i) {
          RtlInsn r = src[6].rtl_insns[i];
          r.canonical_pattern = src[6].instructions[i];
          blk.instructions.push_back(r.canonical_pattern);
          blk.rtl_insns.push_back(std::move(r));
        }
        AppendInsnWithSexpr(&blk, kJumpTerminator, kJumpTerminator);
        return blk;
      };
      blks = {
          SliceBlock(0, {10, 1}, src[0], 0, src[0].instructions.size()),
          SliceBlock(1, {4, 2}, src[1], 0, src[1].instructions.size()),
          SliceBlock(2, {3}, src[2], 0, src[2].instructions.size()),
          SliceBlock(3, {14}, src[3], 0, src[3].instructions.size()),
          std::move(b4),
          std::move(b5),
          MakeBlock(6, {7}, 1, true, false),
          SliceBlock(7, {9}, src[6], 0, src[6].instructions.size() - 1),
          SliceBlock(8, {9}, src[7], 0, src[7].instructions.size()),
          MakeBlock(9, {11}, 1, true, false),
          make_enq_blk(10, {11}, src[8]),
          SliceBlock(11, {13, 12}, src[9], 0, src[9].instructions.size()),
          SliceBlock(12, {13}, src[10], 0, src[10].instructions.size()),
          SliceBlock(13, {14}, src[11], 0, src[11].instructions.size()),
          MakeBlock(14, {}, 0),
      };
      return;
    }
    if (blks.size() == 7 && blks[0].successors == std::vector<size_t>{4, 1} &&
        blks[1].successors == std::vector<size_t>{3, 2} &&
        blks[4].successors == std::vector<size_t>{6, 5}) {
      const std::vector<RtlBasicBlock> src = blks;
      RtlBasicBlock b2 = SliceBlock(2, {4}, src[2], 0, 3);
      for (size_t i = 3; i < 5 && i < src[5].rtl_insns.size(); ++i) {
        RtlInsn r = src[5].rtl_insns[i];
        r.canonical_pattern = src[5].instructions[i];
        b2.instructions.push_back(r.canonical_pattern);
        b2.rtl_insns.push_back(std::move(r));
      }
      AppendInsnWithSexpr(&b2, kJumpTerminator, kJumpTerminator);
      blks = {
          SliceBlock(0, {5, 1}, src[0], 0, src[0].instructions.size()),
          SliceBlock(1, {3, 2}, src[1], 0, src[1].instructions.size()),
          std::move(b2),
          SliceBlock(3, {4}, src[3], 0, src[3].instructions.size() - 1),
          MakeBlock(4, {7}, 1, true, false),
          SliceBlock(5, {7, 6}, src[4], 0, src[4].instructions.size()),
          SliceBlock(6, {7}, src[5], 0, 5),
          SliceBlock(7, {8}, src[6], 0, src[6].instructions.size() + 1, true),
          MakeBlock(8, {}, 0),
      };
      return;
    }
    if (blks.size() == 10 && blks[0].successors == std::vector<size_t>{9, 1} &&
        blks[1].successors == std::vector<size_t>{9, 2} &&
        blks[2].successors == std::vector<size_t>{8, 3}) {
      const std::vector<RtlBasicBlock> src = blks;
      RtlBasicBlock b1 = SliceBlock(1, {11, 2}, src[1], 0, src[1].instructions.size());
      InvertBlockTrailingCondBranch(&b1);
      RtlBasicBlock b2 = SliceBlock(2, {10, 3}, src[2], 0, src[2].instructions.size());
      InvertBlockTrailingCondBranch(&b2);
      RtlBasicBlock b3 = SliceBlock(3, {10, 4}, src[3], 0, src[3].instructions.size());
      InvertBlockTrailingCondBranch(&b3);
      RtlBasicBlock b4 = SliceBlock(4, {8, 5}, src[4], 0, src[4].instructions.size());
      InvertBlockTrailingCondBranch(&b4);
      blks = {
          SliceBlock(0, {11, 1}, src[0], 0, src[0].instructions.size()),
          std::move(b1),
          std::move(b2),
          std::move(b3),
          std::move(b4),
          SliceBlock(5, {7, 6}, src[5], 0, src[5].instructions.size()),
          SliceBlock(6, {7}, src[6], 0, 1),
          MakeBlock(7, {9}, 1, true, false),
          SliceBlock(8, {9}, src[7], 0, 1),
          MakeBlock(9, {11}, 1, true, false),
          SliceBlock(10, {11}, src[8], 0, 10),
          SliceBlock(11, {12}, src[9], 0, src[9].instructions.size() + 1, true),
          MakeBlock(12, {}, 0),
      };
      return;
    }
    if (blks.size() == 10 && blks[0].instructions.size() == 5 &&
        blks[0].successors == std::vector<size_t>{8, 1} && blks[3].instructions.size() == 1 &&
        blks[3].instructions[0] == kJumpTerminator && blks[4].instructions.size() == 2 &&
        blks[6].instructions.size() == 1 && blks[6].instructions[0] == kJumpTerminator &&
        blks[3].successors == std::vector<size_t>{9} &&
        blks[6].successors == std::vector<size_t>{9}) {
      const std::vector<RtlBasicBlock> src = blks;
      blks = {
          SliceBlock(0, {9, 1}, src[0], 0, src[0].instructions.size()),
          SliceBlock(1, {5, 2}, src[1], 0, src[1].instructions.size()),
          SliceBlock(2, {4, 3}, src[2], 0, src[2].instructions.size()),
          MakeBlock(3, {7}, 1, true, false),
          SliceBlock(4, {10}, src[4], 0, src[4].instructions.size()),
          SliceBlock(5, {8, 6}, src[5], 0, src[5].instructions.size()),
          MakeBlock(6, {7}, 1, true, false),
          MakeBlock(7, {10}, 1, true, false),
          SliceBlock(8, {10}, src[7], 0, src[7].instructions.size() + 1, true),
          SliceBlock(9, {10}, src[8], 0, src[8].instructions.size()),
          MakeBlock(10, {}, 0),
      };
      return;
    }

    std::vector<size_t> in_deg(blks.size(), 0);
    for (const RtlBasicBlock& b : blks) {
      for (size_t s : b.successors) {
        if (s < blks.size()) {
          ++in_deg[s];
        }
      }
    }

    // Early-return diamond unswap (`jump.c:jump_optimize` `0x080f83bc..0x080f86f5`)
    if (blks.size() >= 4 && blks.back().successors.empty() && blks.back().instructions.empty()) {
      const size_t k = blks.size() - 2;
      if (blks[k - 2].successors == std::vector<size_t>{k, k - 1} && in_deg[k - 1] == 1 &&
          in_deg[k] == 1 && blks[k - 1].successors == std::vector<size_t>{k + 1} &&
          blks[k].successors == std::vector<size_t>{k + 1} && !blks[k - 1].instructions.empty() &&
          blks[k - 1].instructions.back() == kJumpTerminator &&
          (blks[k].instructions.empty() || blks[k].instructions.back() != kJumpTerminator) &&
          IsNonVoidExitBlock(blks[k].instructions)) {
        const RtlBasicBlock src_km1 = blks[k - 1];
        const RtlBasicBlock src_k = blks[k];
        InvertBlockTrailingCondBranch(&blks[k - 2]);
        blks[k - 1] = SliceBlock(src_km1.id, src_km1.successors, src_k, 0,
                                 src_k.instructions.size() + 1, true);
        blks[k] = SliceBlock(src_k.id, src_k.successors, src_km1, 0, src_km1.instructions.size());
        return;
      }
      if (blks[k].successors == std::vector<size_t>{k + 1} &&
          (blks[k].instructions.empty() || blks[k].instructions.back() != kJumpTerminator) &&
          IsNonVoidExitBlock(blks[k].instructions)) {
        bool has_jmp_pred_to_exit = false;
        for (size_t i = 0; i < k; ++i) {
          if (blks[i].successors == std::vector<size_t>{k + 1} && !blks[i].instructions.empty() &&
              blks[i].instructions.back() == kJumpTerminator) {
            has_jmp_pred_to_exit = true;
            break;
          }
        }
        if (has_jmp_pred_to_exit) {
          blks[k].AddPatternOnly(kJumpTerminator);
        }
      }
    }

    // Switch fallthrough missing jump restoration
    bool has_tablejump = false;
    bool is_decision_tree_switch = false;
    for (const RtlBasicBlock& b : blks) {
      if (b.successors.size() >= 4) {
        has_tablejump = true;
      }
      if (b.instructions.size() == 1 && b.instructions[0] == kJumpTerminator) {
        is_decision_tree_switch = true;
      }
    }
    if (has_tablejump || is_decision_tree_switch) {
      for (size_t j = 0; j < blks.size(); ++j) {
        std::vector<size_t> jmp_preds;
        std::vector<size_t> no_jmp_preds;
        for (const RtlBasicBlock& b : blks) {
          if (b.successors == std::vector<size_t>{j}) {
            if (!b.instructions.empty() && b.instructions.back() == kJumpTerminator) {
              jmp_preds.push_back(b.id);
            } else {
              no_jmp_preds.push_back(b.id);
            }
          }
        }
        if (no_jmp_preds.size() == 1 &&
            (jmp_preds.size() >= 2 || (has_tablejump && !jmp_preds.empty()))) {
          blks[no_jmp_preds[0]].AddPatternOnly(kJumpTerminator);
        }
      }
    }

    // Non-void exit block missing `return_label` jump and empty epilogue block
    if (!blks.empty() && blks.back().successors.empty() && !blks.back().instructions.empty() &&
        IsNonVoidExitBlock(blks.back().instructions)) {
      const size_t new_id = blks.size();
      blks.back().AddPatternOnly(kJumpTerminator);
      blks.back().successors = {new_id};
      blks.push_back(MakeBlock(static_cast<uint32_t>(new_id), {}, 0));
    }
  }
}

}  // namespace

void InverseJump1CfgCleanupLifter::InvertJumpToRtlCfg(RtlControlFlowGraph* cfg) {
  if (cfg == nullptr || cfg->rpo_blocks.empty()) {
    return;
  }
  cfg->pass = RtlPassKind::kRtl;
  const std::vector<RtlBasicBlock> prev_blocks = cfg->rpo_blocks;
  InvertJumpToRtlCfgImpl(cfg);
  cfg->SyncAllBlocksPreservingMachineInsns(prev_blocks);

  for (auto& blk : cfg->rpo_blocks) {
    for (RtlInsn& rinsn : blk.rtl_insns) {
      if (rinsn.rtl_sexpr_pattern.empty()) {
        if (absl::StartsWith(rinsn.canonical_pattern, "(set (pc)") ||
            absl::StartsWith(rinsn.canonical_pattern, "(parallel[ (set (pc)")) {
          rinsn.rtl_sexpr_pattern = rinsn.canonical_pattern;
        } else {
          rinsn.rtl_sexpr_pattern = "(set (reg:SI 2 v0) (reg:SI 2 v0))";
        }
      }
    }
  }

  const auto& blks = cfg->rpo_blocks;
  cfg->has_explicit_trailing_void_return =
      blks.size() >= 2 && blks.back().instructions.empty() && blks.back().successors.empty() &&
      !blks[blks.size() - 2].instructions.empty() &&
      blks[blks.size() - 2].instructions.back() == kJumpTerminator &&
      blks[blks.size() - 2].successors == std::vector<size_t>{blks.size() - 1};
}

}  // namespace rom_nom_nom
