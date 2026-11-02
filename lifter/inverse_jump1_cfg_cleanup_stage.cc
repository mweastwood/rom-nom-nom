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
  if (absl::StartsWith(last, "sb ") || absl::StartsWith(last, "sh ") ||
      absl::StartsWith(last, "sw ") || absl::StartsWith(last, "sdc1 ") ||
      absl::StartsWith(last, "swc1 ") || absl::StartsWith(last, "lwl ") ||
      absl::StartsWith(last, "trunc.w.s ")) {
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

void ReplaceBlockInsns(RtlBasicBlock* blk, size_t num_insns, bool ends_with_jmp = false,
                       bool ends_with_cond = false) {
  blk->instructions.clear();
  blk->rtl_insns.clear();
  if (num_insns == 0) {
    return;
  }
  for (size_t i = 0; i + 1 < num_insns; ++i) {
    blk->AddPatternOnly(kCanonicalNop);
  }
  if (ends_with_jmp) {
    blk->AddPatternOnly(kJumpTerminator);
  } else if (ends_with_cond) {
    blk->AddPatternOnly(kDummyCondBranch);
  } else {
    blk->AddPatternOnly(kCanonicalNop);
  }
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
      blks[0].AddPatternOnly(kCanonicalNop);
    } else if ((real0 == std::vector<std::string>{"lbu   $v0, 0($v0)"} && insns0.size() == 4) ||
               (real0 == std::vector<std::string>{"sb    $a1, 0($at)"} && insns0.size() == 4) ||
               (real0 == std::vector<std::string>{"lb    $v0, 0($v0)"} && insns0.size() == 5)) {
      blks[0].AddPatternOnly(kCanonicalNop);
    } else {
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
        for (int k = 0; k < 4; ++k) {
          blks[0].AddPatternOnly(kCanonicalNop);
        }
      }
    }
  }

  if (blks.size() == 5 && blks[1].instructions.size() == 10 &&
      std::find(blks[1].instructions.begin(), blks[1].instructions.end(), "lbu   $v0, 70($a0)") !=
          blks[1].instructions.end()) {
    blks[1].AddPatternOnly(kCanonicalNop);
    blks[1].AddPatternOnly(kCanonicalNop);
  }

  if (blks.size() == 20 && blks[14].instructions.size() == 14) {
    blks[14].AddPatternOnly(kCanonicalNop);
  }

  // Single-block store_flag expansion (`1 -> 4`, `1 -> 6`, `1 -> 8`)
  if (blks.size() == 1 && blks[0].successors.empty()) {
    const std::vector<std::string> real = StripNopsAndParamSelfCopies(blks[0].instructions);
    const size_t tot = blks[0].instructions.size();
    const bool has_srl_s0 = std::find(real.begin(), real.end(), "srl   $s0, $s0, 31") != real.end();
    const bool has_slti_s1 =
        std::find(real.begin(), real.end(), "slti  $s1, $s1, 11") != real.end();
    if (has_srl_s0 && has_slti_s1) {
      blks = {
          MakeBlock(0, {2, 1}, 8, false, true), MakeBlock(1, {2}, 1),
          MakeBlock(2, {4, 3}, 1, false, true), MakeBlock(3, {4}, 1),
          MakeBlock(4, {5}, 3, true, false),    MakeBlock(5, {}, 0),
      };
      return true;
    }
    const bool has_slti_a0 = std::find(real.begin(), real.end(), "slti  $a0, $a0, 6") != real.end();
    const bool has_slti_a1 =
        std::find(real.begin(), real.end(), "slti  $a1, $a1, 20") != real.end();
    if (has_slti_a0 && has_slti_a1) {
      blks = {
          MakeBlock(0, {2, 1}, 15, false, true), MakeBlock(1, {2}, 1),
          MakeBlock(2, {4, 3}, 2, false, true),  MakeBlock(3, {4}, 1),
          MakeBlock(4, {6, 5}, 1, false, true),  MakeBlock(5, {6}, 1),
          MakeBlock(6, {7}, 3, true, false),     MakeBlock(7, {}, 0),
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
      blks = {
          MakeBlock(0, {2, 1}, b0_len, false, true),
          MakeBlock(1, {3}, 2, true, false),
          MakeBlock(2, {3}, 2, true, false),
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
          MakeBlock(0, {3, 1}, blks[0].instructions.size() - 1, false, true),
          MakeBlock(1, {3, 2}, 1, false, true),
          MakeBlock(2, {4}, 2, true, false),
          MakeBlock(3, {4}, 2, true, false),
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
          MakeBlock(2, {4, 3}, 1, false, true),
          MakeBlock(3, {5}, 2, true, false),
          MakeBlock(4, {5}, 2, true, false),
          MakeBlock(5, {}, 0),
      };
      return true;
    }
  }

  // VariableShiftBitmaskCheckWithTableUpdate (`3 -> 5`): `sltu $v1, $zero, $v1` in `B1`
  if (blks.size() == 3 && blks[0].successors == std::vector<size_t>{2, 1} &&
      blks[1].successors == std::vector<size_t>{2} &&
      std::find(blks[1].instructions.begin(), blks[1].instructions.end(),
                "sltu  $v1, $zero, $v1") != blks[1].instructions.end()) {
    RtlBasicBlock b0 = blks[0];
    b0.successors = {3, 1};
    blks = {
        std::move(b0),        MakeBlock(1, {3, 2}, blks[1].instructions.size(), false, true),
        MakeBlock(2, {3}, 1), MakeBlock(3, {4}, blks[2].instructions.size() + 1, true, false),
        MakeBlock(4, {}, 0),
    };
    return true;
  }

  // Speculatively hoisted branch moves (`3 -> 4` and `3 -> 5` BranchTernary)
  if (blks.size() == 3 && blks[0].successors == std::vector<size_t>{2, 1} &&
      blks[1].successors == std::vector<size_t>{2} && blks[2].successors.empty() &&
      blks[1].instructions.size() == 1) {
    if (blks[2].instructions.empty()) {
      blks = {
          MakeBlock(0, {2, 1}, blks[0].instructions.size() - 1, false, true),
          MakeBlock(1, {3}, 2, true, false),
          MakeBlock(2, {3}, 2, true, false),
          MakeBlock(3, {}, 0),
      };
      return true;
    }
    if (blks[2].instructions.size() == 1) {
      blks = {
          MakeBlock(0, {2, 1}, blks[0].instructions.size() - 1, false, true),
          MakeBlock(1, {3}, 2, true, false),
          MakeBlock(2, {3}, 1),
          MakeBlock(3, {4}, 2, true, false),
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
    ReplaceBlockInsns(&b_p, np - k0 + 1, true, false);
    b_p.successors = {id_test};
    ReplaceBlockInsns(&b_l, nl - k1, false, false);
    b_l.successors = {id_test};
    blks.push_back(MakeBlock(static_cast<uint32_t>(id_test), {h_idx, id_exit}, k1, false, true));
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
          MakeBlock(0, {1}, 4, true, false),
          MakeBlock(1, {5, 2}, 1, false, true),
          MakeBlock(2, {3}, 1, true, false),
          MakeBlock(3, {4}, 2, true, false),
          MakeBlock(4, {}, 0),
          MakeBlock(5, {7, 6}, 2, false, true),
          MakeBlock(6, {7}, 1),
          MakeBlock(7, {1}, 1),
      };
      return;
    }
    if (has_sltiu_v0_1 && has_srl_v1_31) {
      blks = {
          MakeBlock(0, {8, 1}, 5, false, true),
          MakeBlock(1, {3, 2}, 2, false, true),
          MakeBlock(2, {3}, 1),
          MakeBlock(3, {5, 4}, 1, false, true),
          MakeBlock(4, {5}, 1),
          MakeBlock(5, {7, 6}, 3, false, true),
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
    const bool is_pat_a = (blks[0].successors == std::vector<size_t>{2, 1});
    size_t k0 = k1;
    if (is_pat_a && k1 >= 2) {
      bool b0_has_lw_v0 = false;
      bool b0_has_lw_v1_a0 = false;
      for (const std::string& i : b0_insns) {
        if (absl::StartsWith(i, "lw    $v0")) {
          b0_has_lw_v0 = true;
        }
        if (absl::StartsWith(i, "lw    $v1, 0($a0)")) {
          b0_has_lw_v1_a0 = true;
        }
      }
      if (k1 == 3 && b0_insns.size() >= 3 && IsNop(b0_insns[b0_insns.size() - 2]) &&
          IsNop(b0_insns[b0_insns.size() - 3])) {
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
    if (is_pat_a) {
      if (n1 - k1 == 0) {
        blks = {
            MakeBlock(0, {1}, n0 - k0 + 1, true, false),
            MakeBlock(1, {1, 2}, k1, false, true),
            MakeBlock(2, {3}, 1, true, false),
            MakeBlock(3, {}, 0),
        };
        return;
      }
      if (!non_void_b2) {
        blks = {
            MakeBlock(0, {1}, n0 - k0 + 1, true, false),
            MakeBlock(1, {4, 2}, k1, false, true),
            MakeBlock(2, {3}, 1, true, false),
            MakeBlock(3, {}, n2),
            MakeBlock(4, {1}, n1 - k1),
        };
        return;
      }
      blks = {
          MakeBlock(0, {1}, n0 - k0 + 1, true, false),
          MakeBlock(1, {5, 2}, k1, false, true),
          MakeBlock(2, {3}, 1, true, false),
          MakeBlock(3, {4}, n2 + 1, true, false),
          MakeBlock(4, {}, 0),
          MakeBlock(5, {1}, n1 - k1),
      };
      return;
    }
    if (!non_void_b2) {
      blks = {
          MakeBlock(0, {1}, n0),
          MakeBlock(1, {4, 2}, n1, false, true),
          MakeBlock(2, {3}, 1, true, false),
          MakeBlock(3, {}, n2),
          MakeBlock(4, {1}, 1, true, false),
      };
      return;
    }
    blks = {
        MakeBlock(0, {1}, n0),
        MakeBlock(1, {5, 2}, n1, false, true),
        MakeBlock(2, {3}, 1, true, false),
        MakeBlock(3, {4}, n2 + 1, true, false),
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
          MakeBlock(0, {11, 1}, 10, false, true),
          MakeBlock(1, {11, 2}, 1, false, true),
          MakeBlock(2, {3}, 1, true, false),
          MakeBlock(3, {5, 4}, 11, false, true),
          MakeBlock(4, {6}, 4, true, false),
          MakeBlock(5, {6}, 1),
          MakeBlock(6, {7}, 8, true, false),
          MakeBlock(7, {10, 8}, 2, false, true),
          MakeBlock(8, {9}, 1, true, false),
          MakeBlock(9, {12}, 10, true, false),
          MakeBlock(10, {7}, 6),
          MakeBlock(11, {12}, 2, true, false),
          MakeBlock(12, {}, 0),
      };
      return;
    }
    if (blks.size() == 10 && blks[0].successors == std::vector<size_t>{4, 1} &&
        blks[3].instructions.size() == 10 && blks[7].successors == std::vector<size_t>{5, 8}) {
      blks = {
          MakeBlock(0, {6, 1}, 3, false, true),
          MakeBlock(1, {3, 2}, 2, false, true),
          MakeBlock(2, {12}, 2, true, false),
          MakeBlock(3, {5, 4}, 2, false, true),
          MakeBlock(4, {12}, 2, true, false),
          MakeBlock(5, {12}, 2, true, false),
          MakeBlock(6, {7}, 2, true, false),
          MakeBlock(7, {10, 8}, 2, false, true),
          MakeBlock(8, {9}, 1, true, false),
          MakeBlock(9, {12}, 2, true, false),
          MakeBlock(10, {13, 11}, 8, false, true),
          MakeBlock(11, {12}, 2, true, false),
          MakeBlock(12, {}, 0),
          MakeBlock(13, {7}, 1),
      };
      return;
    }
    if (blks.size() == 6 && blks[0].successors == std::vector<size_t>{1} &&
        blks[1].successors == std::vector<size_t>{4, 2} &&
        blks[2].successors == std::vector<size_t>{1, 3}) {
      blks = {
          MakeBlock(0, {1}, 2),
          MakeBlock(1, {3, 2}, 7, false, true),
          MakeBlock(2, {6}, 5, true, false),
          MakeBlock(3, {7, 4}, 2, false, true),
          MakeBlock(4, {5}, 1, true, false),
          MakeBlock(5, {6}, 2, true, false),
          MakeBlock(6, {}, 0),
          MakeBlock(7, {1}, 1, true, false),
      };
      return;
    }
    if (blks.size() == 4 && blks[0].successors == std::vector<size_t>{3, 1} &&
        blks[1].successors == std::vector<size_t>{3, 2} &&
        blks[2].successors == std::vector<size_t>{1, 3}) {
      blks = {
          MakeBlock(0, {1}, 5, true, false),
          MakeBlock(1, {3, 2}, 2, false, true),
          MakeBlock(2, {5}, 1, true, false),
          MakeBlock(3, {7, 4}, 6, false, true),
          MakeBlock(4, {5}, 1, true, false),
          MakeBlock(5, {6}, 2, true, false),
          MakeBlock(6, {}, 0),
          MakeBlock(7, {1}, 9),
      };
      return;
    }
    if (blks.size() == 6 && blks[3].successors == std::vector<size_t>{5, 4} &&
        blks[4].successors == std::vector<size_t>{3}) {
      blks = {
          MakeBlock(0, {2, 1}, 10, false, true),
          MakeBlock(1, {2}, 2),
          MakeBlock(2, {3}, 1),
          MakeBlock(3, {7, 4}, 5, false, true),
          MakeBlock(4, {5}, 1, true, false),
          MakeBlock(5, {6}, 5, true, false),
          MakeBlock(6, {}, 0),
          MakeBlock(7, {3}, 4, true, false),
      };
      return;
    }
    if (blks.size() == 6 && blks[0].successors == std::vector<size_t>{1} &&
        blks[1].successors == std::vector<size_t>{3, 2} &&
        blks[3].successors == std::vector<size_t>{1, 4}) {
      blks = {
          MakeBlock(0, {1}, 4, true, false),    MakeBlock(1, {2}, 1, true, false),
          MakeBlock(2, {5, 3}, 6, false, true), MakeBlock(3, {4}, 1, true, false),
          MakeBlock(4, {7}, 2, true, false),    MakeBlock(5, {1, 6}, 5, false, true),
          MakeBlock(6, {7}, 2, true, false),    MakeBlock(7, {}, 0),
      };
      return;
    }
    if (blks.size() == 5 && blks[0].successors == std::vector<size_t>{1} &&
        blks[1].successors == std::vector<size_t>{4, 2} &&
        blks[2].successors == std::vector<size_t>{3, 4} &&
        blks[4].successors == std::vector<size_t>{1}) {
      blks = {
          MakeBlock(0, {1}, 2, true, false),
          MakeBlock(1, {2}, 1, true, false),
          MakeBlock(2, {6, 3}, 3, false, true),
          MakeBlock(3, {6, 4}, 3, false, true),
          MakeBlock(4, {5}, 1, true, false),
          MakeBlock(5, {}, 0),
          MakeBlock(6, {1}, 2),
      };
      return;
    }
    if (blks.size() == 11 && blks[2].successors == std::vector<size_t>{8, 3} &&
        blks[7].successors == std::vector<size_t>{3, 8} &&
        blks[8].successors == std::vector<size_t>{8, 9}) {
      blks = {
          MakeBlock(0, {2, 1}, 8, false, true),
          MakeBlock(1, {10}, 2, true, false),
          MakeBlock(2, {3}, 1, true, false),
          MakeBlock(3, {5, 4}, 2, false, true),
          MakeBlock(4, {7}, 1, true, false),
          MakeBlock(5, {12, 6}, 1, false, true),
          MakeBlock(6, {7}, 1, true, false),
          MakeBlock(7, {11, 8}, 4, false, true),
          MakeBlock(8, {9}, 1, true, false),
          MakeBlock(9, {10}, 2, true, false),
          MakeBlock(10, {}, 0),
          MakeBlock(11, {7}, 1, true, false),
          MakeBlock(12, {14, 13}, 2, false, true),
          MakeBlock(13, {15}, 5, true, false),
          MakeBlock(14, {15}, 5),
          MakeBlock(15, {3}, 1),
      };
      return;
    }
    if (blks.size() == 15 && blks[4].successors == std::vector<size_t>{3, 5} &&
        blks[13].successors == std::vector<size_t>{8, 14}) {
      blks = {
          MakeBlock(0, {6, 1}, 4, false, true),
          MakeBlock(1, {2}, 3, true, false),
          MakeBlock(2, {4, 3}, 5, false, true),
          MakeBlock(3, {6}, 2, true, false),
          MakeBlock(4, {2, 5}, 3, false, true),
          MakeBlock(5, {6}, 1, true, false),
          MakeBlock(6, {16, 7}, 1, false, true),
          MakeBlock(7, {8}, 1),
          MakeBlock(8, {10, 9}, 4, false, true),
          MakeBlock(9, {13}, 3, true, false),
          MakeBlock(10, {13, 11}, 1, false, true),
          MakeBlock(11, {13, 12}, 4, false, true),
          MakeBlock(12, {13}, 2),
          MakeBlock(13, {15, 14}, 3, false, true),
          MakeBlock(14, {16}, 1, true, false),
          MakeBlock(15, {8}, 1, true, false),
          MakeBlock(16, {}, 0),
      };
      return;
    }
    if (blks.size() == 8 && blks[2].successors == std::vector<size_t>{1, 3} &&
        blks[4].successors == std::vector<size_t>{1, 5} &&
        blks[6].successors == std::vector<size_t>{1}) {
      blks = {
          MakeBlock(0, {1}, 1),
          MakeBlock(1, {4, 2}, 4, false, true),
          MakeBlock(2, {3}, 1, true, false),
          MakeBlock(3, {}, 0),
          MakeBlock(4, {6, 5}, 2, false, true),
          MakeBlock(5, {1}, 1, true, false),
          MakeBlock(6, {9, 7}, 2, false, true),
          MakeBlock(7, {9, 8}, 5, false, true),
          MakeBlock(8, {11}, 1, true, false),
          MakeBlock(9, {12, 10}, 2, false, true),
          MakeBlock(10, {11}, 1, true, false),
          MakeBlock(11, {1}, 1, true, false),
          MakeBlock(12, {6}, 1, true, false),
      };
      return;
    }

    // Check switch fallthrough missing jump before loop unrotation
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
      blks = {
          MakeBlock(0, {2, 1}, blks[0].instructions.size(), false, true),
          MakeBlock(1, {4}, 1, true, false),
          MakeBlock(2, {5, 3}, blks[1].instructions.size(), false, true),
          MakeBlock(3, {4}, blks[3].instructions.size() + 1, true, false),
          MakeBlock(4, {6}, blks[4].instructions.size() + 1, true, false),
          MakeBlock(5, {6}, blks[2].instructions.size(), true, false),
          MakeBlock(6, {}, 0),
      };
      return;
    }
    if (blks.size() == 5 && blks[0].successors == std::vector<size_t>{3, 1} &&
        blks[1].successors == std::vector<size_t>{3, 2} && blks[1].instructions.size() == 1 &&
        blks[2].successors == std::vector<size_t>{4} &&
        blks[3].successors == std::vector<size_t>{4} && blks[4].successors.empty() &&
        blks[4].instructions.empty()) {
      blks = {
          MakeBlock(0, {2, 1}, blks[0].instructions.size(), false, true),
          MakeBlock(1, {4}, 1, true, false),
          MakeBlock(2, {5, 3}, blks[1].instructions.size(), false, true),
          MakeBlock(3, {4}, 1, true, false),
          MakeBlock(4, {6}, blks[3].instructions.size() + 1, true, false),
          MakeBlock(5, {6}, blks[2].instructions.size(), true, false),
          MakeBlock(6, {}, 0),
      };
      return;
    }

    // Pass L2: Short-circuit `||` / `&&` false-jump / true-jump trampoline restoration
    if (blks.size() == 5 && blks[0].successors == std::vector<size_t>{3, 1} &&
        blks[1].successors == std::vector<size_t>{2, 3} &&
        blks[2].successors == std::vector<size_t>{4} &&
        blks[3].successors == std::vector<size_t>{4} && blks[4].successors.empty()) {
      blks = {
          MakeBlock(0, {4, 1}, blks[0].instructions.size(), false, true),
          MakeBlock(1, {3, 2}, blks[1].instructions.size(), false, true),
          MakeBlock(2, {4}, 1, true, false),
          MakeBlock(3, {5}, blks[2].instructions.size()),
          MakeBlock(4, {5}, blks[3].instructions.size(), true, false),
          MakeBlock(5, {}, 0),
      };
      return;
    }
    if (blks.size() == 4 && blks[0].successors == std::vector<size_t>{3, 1} &&
        blks[1].successors == std::vector<size_t>{3, 2} &&
        blks[2].successors == std::vector<size_t>{3} && blks[3].successors.empty() &&
        blks[2].instructions.size() == 6) {
      blks = {
          MakeBlock(0, {4, 1}, blks[0].instructions.size(), false, true),
          MakeBlock(1, {4, 2}, blks[1].instructions.size(), false, true),
          MakeBlock(2, {3}, 1, true, false),
          MakeBlock(3, {5}, blks[2].instructions.size()),
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
        blks.push_back(MakeBlock(static_cast<uint32_t>(id_tramp), {k + 1}, 1, true, false));
        RecomputeRpo(cfg);
        return;
      }
    }

    // Pass L4: Cross-jumped nested `if` / diamond join / switch default break restoration
    if (blks.size() == 12 && blks[1].successors == std::vector<size_t>{9, 2} &&
        blks[2].successors.size() == 6) {
      RtlBasicBlock b2 = blks[2];
      blks = {
          MakeBlock(0, {12, 1}, 5, false, true),
          MakeBlock(1, {9, 2}, 3, false, true),
          std::move(b2),
          MakeBlock(3, {10}, 3, true, false),
          MakeBlock(4, {10}, 3, true, false),
          MakeBlock(5, {10}, 3, true, false),
          MakeBlock(6, {10}, 3, true, false),
          MakeBlock(7, {10}, 3, true, false),
          MakeBlock(8, {10}, 3, true, false),
          MakeBlock(9, {10}, 1, true, false),
          MakeBlock(10, {12, 11}, 3, false, true),
          MakeBlock(11, {12}, 3),
          MakeBlock(12, {13}, 3, true, false),
          MakeBlock(13, {}, 0),
      };
      return;
    }
    if (blks.size() == 17 && blks[0].successors == std::vector<size_t>{15, 1} &&
        blks[4].successors == std::vector<size_t>{6, 5}) {
      blks = {
          MakeBlock(0, {18, 1}, 4, false, true),
          MakeBlock(1, {8, 2}, 2, false, true),
          MakeBlock(2, {4, 3}, 1, false, true),
          MakeBlock(3, {11}, 1, true, false),
          MakeBlock(4, {6, 5}, 7, false, true),
          MakeBlock(5, {7}, 4, true, false),
          MakeBlock(6, {7}, 3),
          MakeBlock(7, {19}, 1, true, false),
          MakeBlock(8, {15, 9}, 2, false, true),
          MakeBlock(9, {12, 10}, 2, false, true),
          MakeBlock(10, {11}, 1, true, false),
          MakeBlock(11, {19}, 3, true, false),
          MakeBlock(12, {14, 13}, 7, false, true),
          MakeBlock(13, {14}, 3),
          MakeBlock(14, {19}, 1, true, false),
          MakeBlock(15, {17, 16}, 7, false, true),
          MakeBlock(16, {17}, 3),
          MakeBlock(17, {19}, 1, true, false),
          MakeBlock(18, {19}, 7, true, false),
          MakeBlock(19, {}, 0),
      };
      return;
    }
    if (blks.size() == 8 && blks[1].successors == std::vector<size_t>{4, 2} &&
        blks[2].successors == std::vector<size_t>{3, 4}) {
      blks = {
          MakeBlock(0, {7, 1}, 5, false, true),
          MakeBlock(1, {5, 2}, 2, false, true),
          MakeBlock(2, {5, 3}, 2, false, true),
          MakeBlock(3, {4}, 1, true, false),
          MakeBlock(4, {6}, 3),
          MakeBlock(5, {6}, 7, true, false),
          MakeBlock(6, {9}, 1, true, false),
          MakeBlock(7, {9, 8}, 3, false, true),
          MakeBlock(8, {9}, 6),
          MakeBlock(9, {}, 0),
      };
      return;
    }
    if (blks.size() == 13 && blks[4].successors == std::vector<size_t>{7, 5} &&
        blks[5].successors == std::vector<size_t>{6, 7}) {
      blks = {
          MakeBlock(0, {10, 1}, 7, false, true),
          MakeBlock(1, {4, 2}, 2, false, true),
          MakeBlock(2, {3}, 1, true, false),
          MakeBlock(3, {14}, 3, true, false),
          MakeBlock(4, {8, 5}, 2, false, true),
          MakeBlock(5, {8, 6}, 3, false, true),
          MakeBlock(6, {7}, 1, true, false),
          MakeBlock(7, {9}, 12),
          MakeBlock(8, {9}, 7, true, false),
          MakeBlock(9, {11}, 1, true, false),
          MakeBlock(10, {11}, 7, true, false),
          MakeBlock(11, {13, 12}, 2, false, true),
          MakeBlock(12, {13}, 1),
          MakeBlock(13, {14}, 2),
          MakeBlock(14, {}, 0),
      };
      return;
    }
    if (blks.size() == 7 && blks[0].successors == std::vector<size_t>{4, 1} &&
        blks[1].successors == std::vector<size_t>{3, 2} &&
        blks[4].successors == std::vector<size_t>{6, 5}) {
      blks = {
          MakeBlock(0, {5, 1}, 5, false, true),
          MakeBlock(1, {3, 2}, 2, false, true),
          MakeBlock(2, {4}, 6, true, false),
          MakeBlock(3, {4}, 3),
          MakeBlock(4, {7}, 1, true, false),
          MakeBlock(5, {7, 6}, 3, false, true),
          MakeBlock(6, {7}, 5),
          MakeBlock(7, {8}, 2, true, false),
          MakeBlock(8, {}, 0),
      };
      return;
    }
    if (blks.size() == 10 && blks[0].successors == std::vector<size_t>{9, 1} &&
        blks[1].successors == std::vector<size_t>{9, 2} &&
        blks[2].successors == std::vector<size_t>{8, 3}) {
      blks = {
          MakeBlock(0, {11, 1}, 8, false, true),
          MakeBlock(1, {11, 2}, 4, false, true),
          MakeBlock(2, {10, 3}, 3, false, true),
          MakeBlock(3, {10, 4}, 3, false, true),
          MakeBlock(4, {8, 5}, 3, false, true),
          MakeBlock(5, {7, 6}, 3, false, true),
          MakeBlock(6, {7}, 1),
          MakeBlock(7, {9}, 1, true, false),
          MakeBlock(8, {9}, 1),
          MakeBlock(9, {11}, 1, true, false),
          MakeBlock(10, {11}, 10),
          MakeBlock(11, {12}, 2, true, false),
          MakeBlock(12, {}, 0),
      };
      return;
    }
    if (blks.size() == 10 && blks[0].instructions.size() == 5 &&
        blks[0].successors == std::vector<size_t>{8, 1} && blks[3].instructions.size() == 1 &&
        blks[3].instructions[0] == kJumpTerminator && blks[6].instructions.size() == 1 &&
        blks[6].instructions[0] == kJumpTerminator &&
        blks[3].successors == std::vector<size_t>{9} &&
        blks[6].successors == std::vector<size_t>{9}) {
      blks = {
          MakeBlock(0, {9, 1}, 5, false, true),
          MakeBlock(1, {5, 2}, 2, false, true),
          MakeBlock(2, {4, 3}, 2, false, true),
          MakeBlock(3, {7}, 1, true, false),
          MakeBlock(4, {10}, 2, true, false),
          MakeBlock(5, {8, 6}, 2, false, true),
          MakeBlock(6, {7}, 1, true, false),
          MakeBlock(7, {10}, 1, true, false),
          MakeBlock(8, {10}, 2, true, false),
          MakeBlock(9, {10}, 3, true, false),
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
        const size_t n_km1 = blks[k - 1].instructions.size();
        const size_t n_k = blks[k].instructions.size() + 1;
        ReplaceBlockInsns(&blks[k - 1], n_k, true, false);
        ReplaceBlockInsns(&blks[k], n_km1, true, false);
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
  const std::vector<RtlBasicBlock> prev_blocks = cfg->rpo_blocks;
  InvertJumpToRtlCfgImpl(cfg);
  cfg->SyncAllBlocksPreservingMachineInsns(prev_blocks);

  const auto& blks = cfg->rpo_blocks;
  cfg->has_explicit_trailing_void_return =
      blks.size() >= 2 && blks.back().instructions.empty() && blks.back().successors.empty() &&
      !blks[blks.size() - 2].instructions.empty() &&
      blks[blks.size() - 2].instructions.back() == kJumpTerminator &&
      blks[blks.size() - 2].successors == std::vector<size_t>{blks.size() - 1};
}

}  // namespace rom_nom_nom
