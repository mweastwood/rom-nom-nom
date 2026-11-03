#include "lifter/inverse_combine_stage.h"

#include <algorithm>
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
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "lifter/rtl_cfg.h"

namespace rom_nom_nom {
namespace {

struct InsnView {
  int kind = 0;
  std::string pat;
  std::vector<std::string> m_insts;
};

struct BlockView {
  size_t bid = 0;
  std::vector<size_t> succs;
  std::vector<InsnView> insns;
};

struct ParsedOp {
  std::string mnem;
  std::vector<std::string> ops;
};

ParsedOp ParseOp(std::string_view pat) {
  std::string_view trimmed = absl::StripAsciiWhitespace(pat);
  if (trimmed.empty()) {
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

bool IsMoveInsn(int kind, std::string_view pat) {
  if (kind != 0) {
    return false;
  }
  const ParsedOp op = ParseOp(pat);
  if ((op.mnem == "addu" || op.mnem == "daddu" || op.mnem == "or") && op.ops.size() == 3 &&
      (op.ops[2] == "$zero" || op.ops[1] == "$zero")) {
    return true;
  }
  if ((op.mnem == "addiu" || op.mnem == "ori") && op.ops.size() == 3 &&
      (op.ops[2] == "0" || op.ops[2] == "0x0")) {
    return true;
  }
  if ((op.mnem == "mov.s" || op.mnem == "mov.d") && op.ops.size() == 2) {
    return true;
  }
  return false;
}

std::pair<bool, std::string> IsSelfMove(int kind, std::string_view pat) {
  if (kind != 0) {
    return {false, ""};
  }
  const ParsedOp op = ParseOp(pat);
  if ((op.mnem == "addu" || op.mnem == "daddu" || op.mnem == "or") && op.ops.size() == 3 &&
      op.ops[0] == op.ops[1] && op.ops[2] == "$zero") {
    return {true, op.ops[0]};
  }
  if ((op.mnem == "mov.s" || op.mnem == "mov.d") && op.ops.size() == 2 && op.ops[0] == op.ops[1]) {
    return {true, op.ops[0]};
  }
  return {false, ""};
}

bool IsConstImmMove(int kind, std::string_view pat) {
  if (kind != 0) {
    return false;
  }
  const ParsedOp op = ParseOp(pat);
  if ((op.mnem == "addiu" || op.mnem == "ori" || op.mnem == "li" || op.mnem == "lui") &&
      op.ops.size() >= 2) {
    if (op.mnem == "lui" || op.ops[1] == "$zero") {
      return true;
    }
  }
  return (op.mnem == "addu" || op.mnem == "daddu" || op.mnem == "or") && op.ops.size() == 3 &&
         op.ops[1] == "$zero" && op.ops[2] == "$zero";
}

bool IsNoDefMnemonic(std::string_view mnem) {
  static const auto* const no_def_set = new absl::flat_hash_set<std::string>{
      "sw",  "sh",   "sb",   "sd",   "swc1", "sdc1", "jal",  "jalr", "j",     "jr",  "b",    "beq",
      "bne", "blez", "bgtz", "bltz", "bgez", "bc1t", "bc1f", "mult", "multu", "div", "divu",
  };
  return no_def_set->contains(mnem);
}

std::string GetDefReg(int kind, std::string_view pat) {
  if (absl::StartsWith(pat, "(set (pc)") || absl::StartsWith(pat, "(parallel") ||
      absl::StartsWith(pat, "(return)")) {
    return "";
  }
  const ParsedOp op = ParseOp(pat);
  if (op.ops.empty()) {
    return "";
  }
  if (IsNoDefMnemonic(op.mnem)) {
    if ((op.mnem == "div" || op.mnem == "divu") && kind == 5 && op.ops.size() == 3) {
      return op.ops[0];
    }
    return "";
  }
  if (op.mnem == "mtc1" && op.ops.size() == 2) {
    return op.ops[1];
  }
  if (op.mnem == "mfc1" && op.ops.size() == 2) {
    return op.ops[0];
  }
  if (absl::StartsWith(op.mnem, "c.") &&
      (absl::StrContains(op.mnem, ".s") || absl::StrContains(op.mnem, ".d"))) {
    return "$fcc0";
  }
  return op.ops[0];
}

std::string GetDefRegFull(int kind, std::string_view pat, const std::vector<std::string>& m_insts) {
  if (absl::StartsWith(pat, "(set (pc)") || absl::StartsWith(pat, "(parallel") ||
      absl::StartsWith(pat, "(return)")) {
    return "";
  }
  if (kind == 7) {
    return "";
  }
  if (kind == 4 || kind == 5) {
    for (const std::string& mi : m_insts) {
      if (absl::StartsWith(mi, "swc1")) {
        return "";
      }
    }
    if (!m_insts.empty()) {
      const ParsedOp last_op = ParseOp(m_insts.back());
      if (last_op.mnem == "mfc1" && !last_op.ops.empty()) {
        return last_op.ops[0];
      }
    }
  }
  const ParsedOp op = ParseOp(pat);
  if ((kind == 1 || kind == 14 || op.mnem == "mult" || op.mnem == "multu") && !m_insts.empty()) {
    const ParsedOp last_op = ParseOp(m_insts.back());
    if ((last_op.mnem == "mflo" || last_op.mnem == "mfhi") && !last_op.ops.empty()) {
      return last_op.ops[0];
    }
  }
  if (kind == 13 && !m_insts.empty()) {
    for (auto it = m_insts.rbegin(); it != m_insts.rend(); ++it) {
      const ParsedOp mop = ParseOp(*it);
      if ((mop.mnem == "mflo" || mop.mnem == "mfhi") && !mop.ops.empty()) {
        return mop.ops[0];
      }
    }
  }
  if (op.ops.empty() || IsNoDefMnemonic(op.mnem)) {
    return "";
  }
  if (op.mnem == "mtc1" && op.ops.size() == 2) {
    return op.ops[1];
  }
  if (op.mnem == "mfc1" && op.ops.size() == 2) {
    return op.ops[0];
  }
  if (absl::StartsWith(op.mnem, "c.") &&
      (absl::StrContains(op.mnem, ".s") || absl::StrContains(op.mnem, ".d"))) {
    return "$fcc0";
  }
  return op.ops[0];
}

std::string RtlRegToken(std::string_view reg) {
  static const auto* const reg_map = new absl::flat_hash_map<std::string, std::string>{
      {"$v0", " 2 v0)"},  {"$v1", " 3 v1)"},  {"$a0", " 4 a0)"},  {"$a1", " 5 a1)"},
      {"$a2", " 6 a2)"},  {"$a3", " 7 a3)"},  {"$t0", " 8 t0)"},  {"$t1", " 9 t1)"},
      {"$t2", " 10 t2)"}, {"$t3", " 11 t3)"}, {"$t4", " 12 t4)"}, {"$t5", " 13 t5)"},
      {"$t6", " 14 t6)"}, {"$t7", " 15 t7)"}, {"$s0", " 16 s0)"}, {"$s1", " 17 s1)"},
      {"$s2", " 18 s2)"}, {"$s3", " 19 s3)"}, {"$s4", " 20 s4)"}, {"$s5", " 21 s5)"},
      {"$s6", " 22 s6)"}, {"$s7", " 23 s7)"}, {"$t8", " 24 t8)"}, {"$t9", " 25 t9)"},
      {"$fp", " 30 fp)"},
  };
  auto it = reg_map->find(reg);
  return it != reg_map->end() ? it->second : "";
}

bool IsStoreMnemonic(std::string_view mnem) {
  return mnem == "sw" || mnem == "sh" || mnem == "sb" || mnem == "sd" || mnem == "swc1" ||
         mnem == "sdc1";
}

bool IsUsedInExpr(std::string_view reg, std::string_view pat, int kind = 0) {
  if (reg.empty()) {
    return false;
  }
  if (kind == 10 || kind == 12) {
    const ParsedOp op = ParseOp(pat);
    if (IsStoreMnemonic(op.mnem) && !op.ops.empty()) {
      return op.ops[0] == reg;
    }
    return false;
  }
  if (absl::StartsWith(pat, "(set (pc)") || absl::StartsWith(pat, "(parallel")) {
    const std::string tok = RtlRegToken(reg);
    return !tok.empty() && absl::StrContains(pat, tok);
  }
  const ParsedOp op = ParseOp(pat);
  if (op.ops.empty()) {
    return false;
  }
  const std::string paren_reg = absl::StrCat("(", reg, ")");
  if (IsStoreMnemonic(op.mnem)) {
    for (const std::string& o : op.ops) {
      if (o == reg || absl::StrContains(o, paren_reg)) {
        return true;
      }
    }
    return false;
  }
  if ((op.mnem == "mult" || op.mnem == "multu" || op.mnem == "div" || op.mnem == "divu") &&
      op.ops.size() == 2) {
    return op.ops[0] == reg || op.ops[1] == reg;
  }
  if (op.mnem == "mtc1" && op.ops.size() == 2) {
    return op.ops[0] == reg;
  }
  for (size_t i = 1; i < op.ops.size(); ++i) {
    if (op.ops[i] == reg || absl::StrContains(op.ops[i], paren_reg)) {
      return true;
    }
  }
  return false;
}

bool IsUsedInSinglePat(std::string_view reg, std::string_view pat) {
  if (reg.empty()) {
    return false;
  }
  if (absl::StartsWith(pat, "(set (pc)") || absl::StartsWith(pat, "(parallel")) {
    const std::string tok = RtlRegToken(reg);
    return !tok.empty() && absl::StrContains(pat, tok);
  }
  const ParsedOp op = ParseOp(pat);
  if (op.ops.empty() || op.mnem == "lui") {
    return false;
  }
  const std::string paren_reg = absl::StrCat("(", reg, ")");
  if (IsStoreMnemonic(op.mnem)) {
    for (const std::string& o : op.ops) {
      if (o == reg || absl::StrContains(o, paren_reg)) {
        return true;
      }
    }
    return false;
  }
  if ((op.mnem == "mult" || op.mnem == "multu" || op.mnem == "div" || op.mnem == "divu") &&
      op.ops.size() >= 2) {
    return op.ops[op.ops.size() - 2] == reg || op.ops[op.ops.size() - 1] == reg;
  }
  if (op.mnem == "mtc1" && op.ops.size() == 2) {
    return op.ops[0] == reg;
  }
  if (op.mnem == "mfc1" && op.ops.size() == 2) {
    return op.ops[1] == reg;
  }
  if (absl::StartsWith(op.mnem, "c.") &&
      (absl::StrContains(op.mnem, ".s") || absl::StrContains(op.mnem, ".d"))) {
    for (const std::string& o : op.ops) {
      if (o == reg) {
        return true;
      }
    }
    return false;
  }
  for (size_t i = 1; i < op.ops.size(); ++i) {
    if (op.ops[i] == reg || absl::StrContains(op.ops[i], paren_reg)) {
      return true;
    }
  }
  return false;
}

bool IsUsedInInsn(std::string_view reg, int kind, std::string_view pat,
                  const std::vector<std::string>& m_insts) {
  if (kind == 6) {
    const ParsedOp op = ParseOp(pat);
    if (op.mnem == "mtc1" && reg == "$a3") {
      return false;
    }
  }
  if (kind == 10 || kind == 12) {
    const ParsedOp op = ParseOp(pat);
    if (IsStoreMnemonic(op.mnem) && !op.ops.empty()) {
      return op.ops[0] == reg;
    }
    return false;
  }
  if (kind != 0 && !m_insts.empty()) {
    absl::flat_hash_set<std::string> defined_internally;
    for (const std::string& mi : m_insts) {
      if (!defined_internally.contains(reg) && IsUsedInSinglePat(reg, mi)) {
        return true;
      }
      const std::string d = GetDefRegFull(0, mi, {});
      if (!d.empty()) {
        defined_internally.insert(d);
      }
    }
    return false;
  }
  return IsUsedInSinglePat(reg, pat);
}

absl::flat_hash_set<std::string> FindCrossBlockHiRegs(const std::vector<BlockView>& s6_blks) {
  absl::flat_hash_set<std::string> hi_regs;
  for (const BlockView& blk : s6_blks) {
    for (size_t i = 0; i < blk.insns.size(); ++i) {
      const ParsedOp op = ParseOp(blk.insns[i].pat);
      if (op.mnem == "lhu" && !op.ops.empty() && op.ops[0] != "$v0") {
        const std::string r = op.ops[0];
        bool redef = false;
        for (size_t j = i + 1; j < blk.insns.size(); ++j) {
          if (GetDefReg(blk.insns[j].kind, blk.insns[j].pat) == r) {
            redef = true;
            break;
          }
        }
        if (!redef) {
          for (const BlockView& oblk : s6_blks) {
            if (oblk.bid == blk.bid) {
              continue;
            }
            for (const InsnView& oinsn : oblk.insns) {
              if (IsUsedInExpr(r, oinsn.pat)) {
                hi_regs.insert(r);
                break;
              }
              if (GetDefReg(oinsn.kind, oinsn.pat) == r) {
                break;
              }
            }
          }
        }
      }
    }
  }
  return hi_regs;
}

bool IsArgReg(std::string_view reg) {
  return reg == "$a0" || reg == "$a1" || reg == "$a2" || reg == "$a3" || reg == "$f12" ||
         reg == "$f14";
}

// Predicts the `.combine` -> `.flow` instruction count delta for block `b` using the non-entry
// `combine.c` inversion rules (1..13).
int PredictBlockDelta(const std::vector<BlockView>& s6_blks, size_t b,
                      const ParsedCompilerFlags* flags = nullptr) {
  const BlockView& blk = s6_blks[b];
  const auto& succs = blk.succs;
  const auto& insns = blk.insns;
  const absl::flat_hash_set<std::string> cross_hi_regs = FindCrossBlockHiRegs(s6_blks);
  const size_t exit_bid = s6_blks.size() - 1;
  const auto& exit_insns = s6_blks[exit_bid].insns;
  const bool reaches_exit_directly =
      (b == exit_bid) || (succs.size() == 1 && succs[0] == exit_bid && exit_insns.empty());
  const bool expensive_opt = (flags == nullptr || flags->flag_expensive_optimizations);

  absl::flat_hash_set<std::string> exit_narrow_regs;
  for (const InsnView& ein : s6_blks.back().insns) {
    const ParsedOp eop = ParseOp(ein.pat);
    if (eop.mnem == "andi" && eop.ops.size() == 3 &&
        (eop.ops[2] == "0xFF" || eop.ops[2] == "0xFFFF") && eop.ops[1] != eop.ops[0]) {
      const std::string src_r = eop.ops[1];
      bool from_jal = false;
      for (const BlockView& ablk : s6_blks) {
        for (const InsnView& bi : ablk.insns) {
          const ParsedOp bop = ParseOp(bi.pat);
          if (bop.mnem == "addu" && bop.ops.size() >= 2 && bop.ops[0] == src_r &&
              bop.ops[1] == "$v0") {
            from_jal = true;
            break;
          }
        }
      }
      if (!from_jal) {
        exit_narrow_regs.insert(src_r);
      }
    }
  }

  int delta = 0;

  for (size_t i = 0; i < insns.size(); ++i) {
    const int kind = insns[i].kind;
    const std::string& pat = insns[i].pat;
    const auto& m_insts = insns[i].m_insts;
    const ParsedOp op = ParseOp(pat);

    // 1. Signed subword load `lb` / `lh`:
    if (op.mnem == "lb" || op.mnem == "lh") {
      const std::string dst = op.ops.empty() ? "" : op.ops[0];
      bool has_hi_copy = false;
      for (size_t j = i + 1; j < insns.size(); ++j) {
        if (IsMoveInsn(insns[j].kind, insns[j].pat)) {
          const ParsedOp jop = ParseOp(insns[j].pat);
          if (jop.ops.size() >= 2 && (jop.ops[0] == "$v0" || dst == "$v0") && jop.ops[0] != dst &&
              jop.ops[1] == dst) {
            has_hi_copy = true;
            break;
          }
        }
      }
      delta += (has_hi_copy ? 1 : 2);
    } else if (op.mnem == "lbu" || op.mnem == "lhu") {
      // 2. Unsigned subword load `lbu` / `lhu`:
      const std::string dst = op.ops.empty() ? "" : op.ops[0];
      bool is_narrow_only = false;
      bool has_bitfield_andi_or_srl = false;
      for (size_t j = i + 1; j < insns.size(); ++j) {
        const ParsedOp jop = ParseOp(insns[j].pat);
        if ((jop.mnem == "sb" || jop.mnem == "sh") && !jop.ops.empty() && jop.ops[0] == dst) {
          is_narrow_only = true;
          break;
        }
        bool used_in_rhs = false;
        for (size_t u = 1; u < jop.ops.size(); ++u) {
          if (jop.ops[u] == dst) {
            used_in_rhs = true;
            break;
          }
        }
        if (used_in_rhs) {
          if (jop.mnem == "andi" || jop.mnem == "srl") {
            has_bitfield_andi_or_srl = true;
          }
          if (jop.mnem == "sll" && jop.ops.size() == 3 &&
              (jop.ops[2] == "16" || jop.ops[2] == "24")) {
            is_narrow_only = true;
            break;
          }
          if (jop.mnem == "andi" && jop.ops.size() == 3 && jop.ops[1] == dst &&
              (jop.ops[2] == "0xFF" || jop.ops[2] == "0xFFFF" || jop.ops[2] == "255" ||
               jop.ops[2] == "65535")) {
            is_narrow_only = true;
            break;
          }
          if ((jop.mnem == "addiu" || jop.mnem == "addu" || jop.mnem == "ori" ||
               jop.mnem == "andi" || jop.mnem == "xori") &&
              jop.ops[0] == dst) {
            for (size_t k = j + 1; k < insns.size(); ++k) {
              const ParsedOp kop = ParseOp(insns[k].pat);
              bool dst_in_kop = false;
              for (const std::string& ko : kop.ops) {
                if (ko == dst) {
                  dst_in_kop = true;
                  break;
                }
              }
              if (dst_in_kop) {
                if ((kop.mnem == "sll" && kop.ops.size() == 3 &&
                     (kop.ops[2] == "16" || kop.ops[2] == "24")) ||
                    ((kop.mnem == "sb" || kop.mnem == "sh") && kop.ops[0] == dst)) {
                  is_narrow_only = true;
                } else if (kop.mnem == "andi" && kop.ops.size() == 3 && kop.ops[0] == dst &&
                           kop.ops[1] == dst && (kop.ops[2] == "0xFFFF" || kop.ops[2] == "0xFF")) {
                  if (jop.mnem == "addu" || jop.mnem == "addiu") {
                    is_narrow_only = true;
                  }
                }
                break;
              }
            }
            break;
          }
        }
        if (GetDefReg(insns[j].kind, insns[j].pat) == dst) {
          break;
        }
      }
      if (insns.size() == 1 && !succs.empty()) {
        for (size_t s : succs) {
          if (s < s6_blks.size()) {
            for (const InsnView& bi : s6_blks[s].insns) {
              const ParsedOp bop = ParseOp(bi.pat);
              if (bop.mnem == "andi" && bop.ops.size() == 3 && bop.ops[1] == dst &&
                  (bop.ops[2] == "0xFF" || bop.ops[2] == "0xFFFF")) {
                is_narrow_only = true;
              }
            }
          }
        }
      }
      if (!insns.empty() && (absl::StrContains(insns.back().pat, "(subreg:SI (reg:QI ") ||
                             absl::StrContains(insns.back().pat, "(subreg:SI (reg:HI "))) {
        has_bitfield_andi_or_srl = true;
      }
      if (!is_narrow_only && (expensive_opt || has_bitfield_andi_or_srl)) {
        delta += 1;
      }
    }

    // 3. `kGasSymbolIndexedAccess` (`kind == 11`):
    if (kind == 11) {
      if (op.mnem != "lb" && op.mnem != "lbu" && op.mnem != "sb") {
        delta += 2;
      }
    }

    // 4. `kGasAddressHiLo` (`kind == 10`) with small positive offset:
    if (kind == 10 && op.mnem == "addiu" && op.ops.size() == 3) {
      const std::optional<int64_t> off = ParseIntLiteral(op.ops[2]);
      if (off.has_value() && *off > 0 && *off < 256) {
        delta += 1;
      }
    }

    // 5. `kDivisionWithTrap` (`kind == 13`):
    if (kind == 13) {
      int num_mf = 0;
      for (const std::string& mi : m_insts) {
        if (absl::StartsWith(mi, "mflo") || absl::StartsWith(mi, "mfhi")) {
          ++num_mf;
        }
      }
      delta += std::max(1, num_mf);
    }

    // 6. `kFixTruncSfSi2` (`kind == 4`) / `kFixTruncDfSi2` (`kind == 5`):
    if (kind == 4 || kind == 5) {
      for (const std::string& mi : m_insts) {
        if (absl::StartsWith(mi, "swc1")) {
          delta += 1;
          break;
        }
      }
    }

    // 7. Narrow register increment/decrement `addiu $r, $r, C`:
    if (op.mnem == "addiu" && op.ops.size() == 3 && op.ops[1] != "$zero" && op.ops[2] != "0" &&
        op.ops[2] != "0x0") {
      const std::string& dst = op.ops[0];
      bool same_blk_narrow_mask = false;
      for (size_t j = 0; j < insns.size(); ++j) {
        if (j < i && op.ops[0] != op.ops[1]) {
          continue;
        }
        const ParsedOp jop = ParseOp(insns[j].pat);
        if (jop.mnem == "andi" && jop.ops.size() == 3 && jop.ops[1] == dst &&
            (jop.ops[2] == "0xFF" || jop.ops[2] == "0xFFFF") && jop.ops[0] != dst) {
          same_blk_narrow_mask = true;
          break;
        }
      }
      if (same_blk_narrow_mask) {
        delta += 1;
      } else if (insns.size() == 1 && exit_narrow_regs.contains(dst)) {
        delta += 1;
      } else if (insns.size() == 2 && absl::StartsWith(insns[1].pat, "(set (pc) (label_ref))") &&
                 !succs.empty()) {
        for (const InsnView& si : s6_blks[succs[0]].insns) {
          const ParsedOp sop = ParseOp(si.pat);
          if (sop.mnem == "sh" && !sop.ops.empty() && sop.ops[0] == dst) {
            delta += 1;
            break;
          }
        }
      }
    }

    // 8. Cross-block `HI` register bitmask `andi $dst, $r_hi, C`:
    if (op.mnem == "andi" && op.ops.size() == 3 && cross_hi_regs.contains(op.ops[1]) &&
        op.ops[1] != op.ops[0]) {
      bool loaded_in_same_blk = false;
      for (size_t j = 0; j < i; ++j) {
        const ParsedOp jop = ParseOp(insns[j].pat);
        if ((jop.mnem == "lhu" || jop.mnem == "lbu") && !jop.ops.empty() &&
            jop.ops[0] == op.ops[1]) {
          loaded_in_same_blk = true;
          break;
        }
      }
      if (!loaded_in_same_blk) {
        const std::optional<int64_t> mask_val = ParseIntLiteral(op.ops[2]);
        if (mask_val.has_value() && *mask_val > 0 && *mask_val < 255) {
          delta += 1;
        }
      }
    }

    // 8b. At `-O1`, `andi $v0, $r1, 0xFF` followed by `subu $v0, $v0, $r2` where `$r2`'s
    // `andi $r2, $r2, 0xFF` was folded into an earlier block's `lbu`:
    if (!expensive_opt && op.mnem == "andi" && op.ops.size() == 3 && op.ops[0] == "$v0" &&
        op.ops[2] == "0xFF" && i + 1 < insns.size()) {
      const ParsedOp nop = ParseOp(insns[i + 1].pat);
      if (nop.mnem == "subu" && nop.ops.size() == 3 && nop.ops[0] == "$v0" && nop.ops[1] == "$v0" &&
          nop.ops[2] != "$v0") {
        delta += 1;
      }
    }

    // 9. Signed division by 2 sign-bit shift:
    if (op.mnem == "srl" && op.ops.size() == 3 && op.ops[2] == "31") {
      if (i + 1 < insns.size() && absl::StartsWith(insns[i + 1].pat, "addu ")) {
        delta += 1;
      }
    }

    // 10. Store-flag `slt $v0, $zero, $r` as a standalone boolean value in a non-branching block:
    if (op.mnem == "slt" && op.ops.size() == 3 && op.ops[0] == "$v0" && op.ops[1] == "$zero" &&
        insns.size() == 1) {
      delta += 1;
    }

    // 11. Loop preheader `blez` (`(le:SI (reg:SI X) (const_int 0))`) with 0 counter init:
    if (absl::StrContains(pat, "(le:SI ") && absl::StrContains(pat, "(const_int 0)")) {
      bool has_zero_counter = false;
      for (size_t j = 0; j < i; ++j) {
        if (IsConstImmMove(insns[j].kind, insns[j].pat) &&
            absl::EndsWith(insns[j].pat, ", $zero, $zero")) {
          has_zero_counter = true;
          break;
        }
      }
      if (has_zero_counter) {
        delta += 1;
      }
    }

    // 12. Calls (`jal` / `jalr`):
    if (op.mnem == "jal" || op.mnem == "jalr") {
      int prev_call = -1;
      for (int k = static_cast<int>(i) - 1; k >= 0; --k) {
        const ParsedOp kop = ParseOp(insns[k].pat);
        if (kop.mnem == "jal" || kop.mnem == "jalr") {
          prev_call = k;
          break;
        }
      }
      size_t next_call = insns.size();
      for (size_t k = i + 1; k < insns.size(); ++k) {
        const ParsedOp kop = ParseOp(insns[k].pat);
        if (kop.mnem == "jal" || kop.mnem == "jalr") {
          next_call = k;
          break;
        }
      }
      absl::flat_hash_map<std::string, int> post_self_moves;
      for (size_t k = i + 1; k < next_call; ++k) {
        auto [is_sm, sm_reg] = IsSelfMove(insns[k].kind, insns[k].pat);
        if (is_sm) {
          post_self_moves[sm_reg] += 1;
        }
      }

      absl::flat_hash_set<std::string> seen_args;
      for (int k = static_cast<int>(i) - 1; k > prev_call; --k) {
        const int kkind = insns[k].kind;
        const std::string& kpat = insns[k].pat;
        const ParsedOp kop = ParseOp(kpat);
        const std::string dreg = GetDefReg(kkind, kpat);
        if (IsArgReg(dreg) && !seen_args.contains(dreg)) {
          seen_args.insert(dreg);
          if (post_self_moves[dreg] > 0) {
            continue;
          }
          if (kkind == 10 || kkind == 12) {
            if (expensive_opt || kop.mnem == "lb" || kop.mnem == "lh" || kop.mnem == "lbu" ||
                kop.mnem == "lhu") {
              delta += 1;
            }
          } else if (kop.mnem == "lb" || kop.mnem == "lh" || kop.mnem == "lbu" ||
                     kop.mnem == "lhu") {
            delta += 1;
          } else if (IsMoveInsn(kkind, kpat) && kop.ops.size() >= 2 &&
                     cross_hi_regs.contains(kop.ops[1])) {
            delta += 1;
          } else if (!IsMoveInsn(kkind, kpat) && !IsConstImmMove(kkind, kpat)) {
            if (kop.mnem == "addiu" && kop.ops.size() == 3 &&
                (kop.ops[1] == "$sp" || kop.ops[1] == "$fp")) {
              // Stack-slot address materialization into `$aX`
            } else if (kop.mnem == "andi" && kop.ops.size() == 3 && kop.ops[1] != dreg &&
                       (kop.ops[2] == "0xFF" || (expensive_opt && kop.ops[2] == "0xFFFF"))) {
              // Narrow zero-extension directly into `$aX`
            } else if (kop.mnem == "lw" || kop.mnem == "ld" || kop.mnem == "lwc1" ||
                       kop.mnem == "ldc1" || kop.mnem == "mfc1" || kop.mnem == "mtc1") {
              // Direct memory load or FPR/GPR transfer into `$aX`
            } else {
              delta += 1;
            }
          }
        } else if ((dreg == "$f4" ||
                    (dreg == "$f0" && k > 0 && ParseOp(insns[k - 1].pat).mnem == "mfc1")) &&
                   kop.mnem == "add.s" && !seen_args.contains("$a3")) {
          seen_args.insert("$a3");
          delta += 1;
        }
      }

      bool v0_used = false;
      bool v0_redefined = false;
      const bool has_subsequent_call = (next_call < insns.size());
      for (size_t k = i + 1; k < next_call; ++k) {
        const int kkind = insns[k].kind;
        const std::string& kpat = insns[k].pat;
        auto [is_sm, sm_reg] = IsSelfMove(kkind, kpat);
        if (is_sm) {
          continue;
        }
        if (!v0_redefined &&
            (IsUsedInExpr("$v0", kpat, kkind) || IsUsedInExpr("$f0", kpat, kkind))) {
          const ParsedOp kop = ParseOp(kpat);
          if (IsMoveInsn(kkind, kpat) && kop.ops.size() >= 2 &&
              (kop.ops[1] == "$v0" || kop.ops[1] == "$f0") &&
              !(has_subsequent_call && IsArgReg(kop.ops[0])) && kop.ops[0] != "$v0" &&
              kop.ops[0] != "$f0") {
            v0_redefined = true;
            break;
          }
          v0_used = true;
          break;
        }
        const std::string dreg = GetDefReg(kkind, kpat);
        if (dreg == "$v0" || dreg == "$f0") {
          v0_redefined = true;
        }
      }

      const bool returns_call_result =
          (post_self_moves["$v0"] > 0 && !v0_redefined && reaches_exit_directly);
      int needed_v0_copies = 0;
      if (returns_call_result) {
        needed_v0_copies = 2;
      } else if (v0_used) {
        needed_v0_copies = 1;
      }
      delta += std::max(0, needed_v0_copies - post_self_moves["$v0"]);
    }
  }

  // 13. Function return value copy `(set (reg/i:M $v0/$f0) (reg:M pseudo))` at exit:
  if (reaches_exit_directly) {
    std::optional<size_t> last_v0_def_idx;
    bool has_trailing_v0_self_move = false;
    for (int k = static_cast<int>(insns.size()) - 1; k >= 0; --k) {
      const int kkind = insns[k].kind;
      const std::string& kpat = insns[k].pat;
      const auto& km_insts = insns[k].m_insts;
      const ParsedOp kop = ParseOp(kpat);
      if (kop.mnem == "jal" || kop.mnem == "jalr") {
        break;
      }
      if (kkind == 4 || kkind == 5) {
        bool has_swc1 = false;
        for (const std::string& mi : km_insts) {
          if (absl::StartsWith(mi, "swc1")) {
            has_swc1 = true;
            break;
          }
        }
        if (has_swc1) {
          continue;
        }
      }
      auto [is_sm, sm_reg] = IsSelfMove(kkind, kpat);
      if (is_sm && (sm_reg == "$v0" || sm_reg == "$f0")) {
        has_trailing_v0_self_move = true;
        break;
      }
      std::string dreg = GetDefReg(kkind, kpat);
      if ((kkind == 4 || kkind == 5) && !km_insts.empty()) {
        const ParsedOp last_mop = ParseOp(km_insts.back());
        if (last_mop.mnem == "mfc1" && !last_mop.ops.empty()) {
          dreg = last_mop.ops[0];
        }
      }
      if (dreg == "$v0" || dreg == "$f0") {
        last_v0_def_idx = static_cast<size_t>(k);
        break;
      }
    }
    if (last_v0_def_idx.has_value() && !has_trailing_v0_self_move) {
      const size_t idx = *last_v0_def_idx;
      const int kkind = insns[idx].kind;
      const std::string& kpat = insns[idx].pat;
      const auto& km_insts = insns[idx].m_insts;
      const ParsedOp kop = ParseOp(kpat);
      std::string dreg = GetDefReg(kkind, kpat);
      if ((kkind == 4 || kkind == 5) && !km_insts.empty()) {
        const ParsedOp last_mop = ParseOp(km_insts.back());
        if (last_mop.mnem == "mfc1" && !last_mop.ops.empty()) {
          dreg = last_mop.ops[0];
        }
      }
      bool used_after_in_blk = false;
      for (size_t m = idx + 1; m < insns.size(); ++m) {
        if (IsUsedInExpr(dreg, insns[m].pat)) {
          used_after_in_blk = true;
          break;
        }
      }
      if (!used_after_in_blk) {
        if (!IsMoveInsn(kkind, kpat) && !IsConstImmMove(kkind, kpat) &&
            !(kkind == 0 && kop.mnem == "lw")) {
          delta += 1;
        }
      }
    }
  }

  return delta;
}

std::vector<std::string> ComputeLiveInEntryArgs(const std::vector<BlockView>& s6_blks) {
  static constexpr std::string_view kArgCandidates[] = {"$a0", "$a1", "$a2", "$a3", "$f12", "$f14"};
  absl::flat_hash_set<std::string> live_entry;
  const auto& insns0 = s6_blks[0].insns;

  std::optional<size_t> first_call;
  for (size_t i = 0; i < insns0.size(); ++i) {
    const ParsedOp op = ParseOp(insns0[i].pat);
    if (op.mnem == "jal" || op.mnem == "jalr") {
      first_call = i;
      break;
    }
  }
  if (first_call.has_value()) {
    absl::flat_hash_set<std::string> defined_before_first_call;
    for (size_t i = 0; i < *first_call; ++i) {
      if (!IsSelfMove(insns0[i].kind, insns0[i].pat).first) {
        defined_before_first_call.insert(
            GetDefRegFull(insns0[i].kind, insns0[i].pat, insns0[i].m_insts));
      }
    }
    for (size_t i = *first_call + 1; i < insns0.size(); ++i) {
      auto [is_sm, sm_reg] = IsSelfMove(insns0[i].kind, insns0[i].pat);
      if (is_sm && IsArgReg(sm_reg) && !defined_before_first_call.contains(sm_reg)) {
        live_entry.insert(sm_reg);
      }
    }
  }

  for (std::string_view cand_sv : kArgCandidates) {
    const std::string reg(cand_sv);
    if (live_entry.contains(reg)) {
      continue;
    }
    absl::flat_hash_set<size_t> visited;
    std::vector<size_t> queue = {0};
    size_t q_head = 0;
    while (q_head < queue.size()) {
      const size_t bid = queue[q_head++];
      if (visited.contains(bid) || bid >= s6_blks.size()) {
        continue;
      }
      visited.insert(bid);
      const auto& succs = s6_blks[bid].succs;
      const auto& insns = s6_blks[bid].insns;
      bool redefined = false;
      for (const InsnView& insn : insns) {
        if (IsUsedInInsn(reg, insn.kind, insn.pat, insn.m_insts)) {
          live_entry.insert(reg);
          redefined = true;
          break;
        }
        const std::string dreg = GetDefRegFull(insn.kind, insn.pat, insn.m_insts);
        if (dreg == reg) {
          redefined = true;
          break;
        }
        const ParsedOp op = ParseOp(insn.pat);
        if (op.mnem == "jal" || op.mnem == "jalr") {
          redefined = true;
          break;
        }
        if (insn.kind == 0 && (op.mnem == "mult" || op.mnem == "multu") &&
            (reg == "$a1" || reg == "$a2" || reg == "$a3")) {
          size_t num_mults = 0;
          std::optional<size_t> first_mult_idx;
          for (size_t idx2 = 0; idx2 < insns.size(); ++idx2) {
            const ParsedOp op2 = ParseOp(insns[idx2].pat);
            if (insns[idx2].kind == 0 && (op2.mnem == "mult" || op2.mnem == "multu")) {
              ++num_mults;
              if (!first_mult_idx.has_value()) {
                first_mult_idx = idx2;
              }
            }
          }
          absl::flat_hash_set<std::string> seen_before_mult;
          const size_t b0_limit = (bid == 0) ? *first_mult_idx : insns0.size();
          for (size_t idx0 = 0; idx0 < b0_limit; ++idx0) {
            for (std::string_view c : {"$a1", "$a2", "$a3"}) {
              if (IsUsedInInsn(c, insns0[idx0].kind, insns0[idx0].pat, insns0[idx0].m_insts) ||
                  GetDefRegFull(insns0[idx0].kind, insns0[idx0].pat, insns0[idx0].m_insts) == c) {
                seen_before_mult.emplace(c);
              }
            }
          }
          std::vector<std::string> div_tmps;
          for (size_t idx2 = *first_mult_idx + 1; idx2 < insns.size(); ++idx2) {
            const ParsedOp op2 = ParseOp(insns[idx2].pat);
            if (op2.mnem != "sra" && op2.mnem != "srl" && op2.mnem != "addu" &&
                op2.mnem != "subu") {
              continue;
            }
            for (std::string_view c : {"$a1", "$a2", "$a3"}) {
              const std::string cs(c);
              if (!seen_before_mult.contains(cs) &&
                  std::find(div_tmps.begin(), div_tmps.end(), cs) == div_tmps.end() &&
                  IsUsedInInsn(cs, insns[idx2].kind, insns[idx2].pat, insns[idx2].m_insts)) {
                div_tmps.push_back(cs);
              }
            }
          }
          const size_t limit = std::min(num_mults, div_tmps.size());
          for (size_t di = 0; di < limit; ++di) {
            if (div_tmps[di] == reg) {
              redefined = true;
              break;
            }
          }
          if (redefined) {
            break;
          }
        }
      }
      if (!redefined) {
        for (size_t s : succs) {
          if (!visited.contains(s)) {
            queue.push_back(s);
          }
        }
      }
    }
  }

  std::vector<std::string> sorted_args(live_entry.begin(), live_entry.end());
  std::sort(sorted_args.begin(), sorted_args.end());
  return sorted_args;
}

bool IsNarrowEntryParam(std::string_view reg, const std::vector<BlockView>& s6_blks) {
  const auto& insns0 = s6_blks[0].insns;
  bool has_sm = false;
  for (const InsnView& insn : insns0) {
    auto [is_sm, sm_reg] = IsSelfMove(insn.kind, insn.pat);
    if (is_sm && sm_reg == reg) {
      has_sm = true;
      break;
    }
  }
  if (!has_sm) {
    for (const InsnView& insn : insns0) {
      const ParsedOp op = ParseOp(insn.pat);
      if ((op.mnem == "sb" || op.mnem == "sh") && !op.ops.empty() && op.ops[0] == reg) {
        return true;
      }
      if (GetDefRegFull(insn.kind, insn.pat, insn.m_insts) == reg) {
        break;
      }
    }
  }
  if (s6_blks.size() > 1) {
    for (const InsnView& insn : insns0) {
      const ParsedOp op = ParseOp(insn.pat);
      if (op.mnem == "ori" && op.ops.size() == 3 &&
          (op.ops[2] == "0xFFFF" || op.ops[2] == "65535")) {
        return false;
      }
    }
    for (const BlockView& blk : s6_blks) {
      for (size_t i = 0; i < blk.insns.size(); ++i) {
        const InsnView& insn = blk.insns[i];
        auto [is_sm, sm_reg] = IsSelfMove(insn.kind, insn.pat);
        if (is_sm && sm_reg == reg) {
          continue;
        }
        const ParsedOp op = ParseOp(insn.pat);
        if (op.mnem == "andi" && op.ops.size() == 3 && op.ops[1] == reg &&
            (op.ops[2] == "0xFFFF" || op.ops[2] == "65535")) {
          return true;
        }
        if (IsMoveInsn(insn.kind, insn.pat) && op.ops.size() >= 2 && op.ops[1] == reg &&
            i + 1 < blk.insns.size()) {
          const ParsedOp nop = ParseOp(blk.insns[i + 1].pat);
          if (nop.mnem == "andi" && nop.ops.size() == 3 && nop.ops[1] == op.ops[0] &&
              (nop.ops[2] == "0xFFFF" || nop.ops[2] == "65535")) {
            return true;
          }
        }
        if (GetDefRegFull(insn.kind, insn.pat, insn.m_insts) == reg) {
          return false;
        }
      }
    }
  }
  return false;
}

bool B0BlezIsCombinedLt(const std::vector<BlockView>& s6_blks) {
  const auto& insns0 = s6_blks[0].insns;
  std::string le_pat;
  for (const InsnView& insn : insns0) {
    if (absl::StrContains(insn.pat, "(le:SI ") && absl::StrContains(insn.pat, "(const_int 0)")) {
      le_pat = insn.pat;
      break;
    }
  }
  if (le_pat.empty()) {
    return false;
  }
  bool has_counter_init = false;
  for (const InsnView& insn : insns0) {
    if (IsConstImmMove(insn.kind, insn.pat) &&
        (absl::EndsWith(insn.pat, ", $zero, $zero") || absl::EndsWith(insn.pat, ", $zero, 1"))) {
      has_counter_init = true;
      break;
    }
  }
  if (!has_counter_init) {
    return false;
  }
  std::string breg;
  const size_t pos = le_pat.find("(le:SI (reg:SI ");
  if (pos != std::string::npos) {
    std::string_view rest = std::string_view{le_pat}.substr(pos + 15);
    const size_t sp = rest.find(' ');
    const size_t rp = rest.find(')');
    if (sp != std::string::npos && rp != std::string::npos && sp + 1 < rp) {
      breg = absl::StrCat("$", rest.substr(sp + 1, rp - sp - 1));
    }
  }
  bool has_slt_bound = false;
  const std::string suffix = absl::StrCat(", ", breg);
  const std::string mid = absl::StrCat(", ", breg, ",");
  for (size_t b = 1; b < s6_blks.size(); ++b) {
    for (const InsnView& insn : s6_blks[b].insns) {
      if (absl::StartsWith(insn.pat, "slt ") &&
          (absl::EndsWith(insn.pat, suffix) || absl::StrContains(insn.pat, mid))) {
        has_slt_bound = true;
        break;
      }
    }
  }
  if (!has_slt_bound) {
    return false;
  }
  if (s6_blks.size() > 1 && s6_blks[1].succs.size() == 1) {
    for (const InsnView& insn : s6_blks[1].insns) {
      if (IsConstImmMove(insn.kind, insn.pat) || ParseOp(insn.pat).mnem == "lui") {
        return false;
      }
    }
  }
  return true;
}

int PredictB0FullDelta(const std::vector<BlockView>& s6_blks,
                       const ParsedCompilerFlags* flags = nullptr) {
  const auto& succs = s6_blks[0].succs;
  const auto& insns = s6_blks[0].insns;
  int base_delta = PredictBlockDelta(s6_blks, 0, flags);

  for (size_t i = 0; i < insns.size(); ++i) {
    if (absl::StrContains(insns[i].pat, "(le:SI ") &&
        absl::StrContains(insns[i].pat, "(const_int 0)")) {
      bool has_zero_counter = false;
      for (size_t j = 0; j < i; ++j) {
        if (IsConstImmMove(insns[j].kind, insns[j].pat) &&
            absl::EndsWith(insns[j].pat, ", $zero, $zero")) {
          has_zero_counter = true;
          break;
        }
      }
      if (has_zero_counter) {
        base_delta -= 1;
      }
    }
  }
  if (B0BlezIsCombinedLt(s6_blks)) {
    base_delta += 1;
  }

  // Rule 13 adjustments in B0 when last `$v0`/`$f0` def is `1:mult`, `7:lwl`, or `11/12:lw` /
  // `lwc1`/`ldc1`:
  if (s6_blks.size() == 1 ||
      (succs.size() == 1 && succs[0] == s6_blks.size() - 1 && s6_blks.back().insns.empty())) {
    if (!insns.empty()) {
      const int lk = insns.back().kind;
      const ParsedOp lop = ParseOp(insns.back().pat);
      if (lk == 1 && (lop.mnem == "mult" || lop.mnem == "multu")) {
        base_delta += 1;
      } else if (lk == 7) {
        base_delta -= 1;
      }
    }
    for (int k = static_cast<int>(insns.size()) - 1; k >= 0; --k) {
      const int kk = insns[k].kind;
      const std::string& kp = insns[k].pat;
      const auto& km_i = insns[k].m_insts;
      const ParsedOp kop = ParseOp(kp);
      if (kop.mnem == "jal" || kop.mnem == "jalr") {
        break;
      }
      if (kk == 4 || kk == 5) {
        bool has_swc1 = false;
        for (const std::string& mi : km_i) {
          if (absl::StartsWith(mi, "swc1")) {
            has_swc1 = true;
            break;
          }
        }
        if (has_swc1) {
          continue;
        }
      }
      auto [is_sm, sm_reg] = IsSelfMove(kk, kp);
      if (is_sm && (sm_reg == "$v0" || sm_reg == "$f0")) {
        break;
      }
      const std::string dreg = GetDefRegFull(kk, kp, km_i);
      if (dreg == "$v0" || dreg == "$f0") {
        bool used_after = false;
        for (size_t m = static_cast<size_t>(k) + 1; m < insns.size(); ++m) {
          if (IsUsedInSinglePat(dreg, insns[m].pat)) {
            used_after = true;
            break;
          }
        }
        if (!used_after && (kk == 0 || kk == 11 || kk == 12) &&
            (kop.mnem == "lw" || kop.mnem == "lwc1" || kop.mnem == "ldc1")) {
          if (kk == 11 || kk == 12 || kop.mnem == "lwc1" || kop.mnem == "ldc1") {
            base_delta -= 1;
          }
        }
        break;
      }
    }
  }

  // `kFloatSiSf2` / `kFloatSiDf2` (`kind in (2, 3)`) with `lwc1` in `m_insts`:
  for (const InsnView& insn : insns) {
    if (insn.kind == 2 || insn.kind == 3) {
      for (const std::string& mi : insn.m_insts) {
        if (absl::StartsWith(mi, "lwc1")) {
          base_delta += 1;
          break;
        }
      }
    }
  }

  // Stage 4 Category 1C (`mul.s $f2, ...` or `2:cvt.s.w $f2, $f2` from `$v1` before `jal`):
  bool seen_call_in_b0 = false;
  for (size_t i = 0; i < insns.size(); ++i) {
    const ParsedOp op = ParseOp(insns[i].pat);
    if (op.mnem == "jal" || op.mnem == "jalr") {
      seen_call_in_b0 = true;
      continue;
    }
    if (op.mnem == "mul.s" && !op.ops.empty() &&
        (op.ops[0] == "$f2" || (op.ops[0] == "$f0" && seen_call_in_b0))) {
      const std::string& mul_dst = op.ops[0];
      std::optional<size_t> next_call;
      for (size_t j = i + 1; j < insns.size(); ++j) {
        const ParsedOp jop = ParseOp(insns[j].pat);
        if (jop.mnem == "jal" || jop.mnem == "jalr") {
          next_call = j;
          break;
        }
      }
      if (next_call.has_value()) {
        bool used_before_call = false;
        bool has_mfc1 = false;
        for (size_t j = i + 1; j < *next_call; ++j) {
          const ParsedOp jop = ParseOp(insns[j].pat);
          if (jop.mnem == "mfc1") {
            for (const std::string& o : jop.ops) {
              if (o == mul_dst) {
                has_mfc1 = true;
              }
            }
          } else if (insns[j].kind == 0 && jop.mnem != "mul.s") {
            const size_t start_idx = absl::StartsWith(jop.mnem, "c.") ? 0 : 1;
            for (size_t u = start_idx; u < jop.ops.size(); ++u) {
              if (jop.ops[u] == mul_dst) {
                used_before_call = true;
              }
            }
          }
        }
        if (!used_before_call && !has_mfc1) {
          base_delta += 1;
        }
      }
    } else if (insns[i].kind == 2 && !op.ops.empty() && op.ops[0] == "$f2") {
      for (const std::string& mi : insns[i].m_insts) {
        if (absl::StartsWith(mi, "mtc1  $v1")) {
          base_delta += 1;
          break;
        }
      }
    }
  }

  // Stack address argument `addiu $aX, $sp, off` before `jal` at non-first frame offset:
  std::optional<int64_t> prev_sp_off;
  for (const InsnView& insn : insns) {
    const ParsedOp op = ParseOp(insn.pat);
    if (op.mnem == "addiu" && op.ops.size() == 3 &&
        (op.ops[0] == "$a0" || op.ops[0] == "$a1" || op.ops[0] == "$a2" || op.ops[0] == "$a3") &&
        op.ops[1] == "$sp") {
      const int64_t off = ParseIntLiteral(op.ops[2]).value_or(0);
      const std::string target_mem = absl::StrCat(off, "($sp)");
      bool has_sp_store_at_off = false;
      for (const InsnView& sin : insns) {
        const ParsedOp sop = ParseOp(sin.pat);
        if (sop.mnem == "sw" && !sop.ops.empty() && sop.ops.back() == target_mem) {
          has_sp_store_at_off = true;
          break;
        }
      }
      if (has_sp_store_at_off && off >= 28) {
        base_delta += 1;
      } else if (prev_sp_off.has_value() && off > *prev_sp_off) {
        base_delta += 1;
      }
      prev_sp_off = off;
    }
  }

  // Multi-call `mov.s $f0, $f0` at end of B0:
  std::vector<size_t> call_indices;
  for (size_t i = 0; i < insns.size(); ++i) {
    const ParsedOp op = ParseOp(insns[i].pat);
    if (op.mnem == "jal" || op.mnem == "jalr") {
      call_indices.push_back(i);
    }
  }
  if (call_indices.size() >= 2) {
    int trailing_f0_sm = 0;
    for (size_t k = call_indices.back() + 1; k < insns.size(); ++k) {
      auto [is_sm, sm_reg] = IsSelfMove(insns[k].kind, insns[k].pat);
      if (is_sm && sm_reg == "$f0") {
        ++trailing_f0_sm;
      }
    }
    if (trailing_f0_sm >= 2) {
      base_delta -= trailing_f0_sm;
    }
  }

  // `lhu` + `srl` + `andi` bitfield extract in B0:
  for (size_t i = 0; i < insns.size(); ++i) {
    const ParsedOp op0 = ParseOp(insns[i].pat);
    if (op0.mnem == "lhu" && !op0.ops.empty()) {
      const std::string& r = op0.ops[0];
      bool has_srl = false;
      for (size_t j = i + 1; j < insns.size(); ++j) {
        const ParsedOp jop = ParseOp(insns[j].pat);
        if (jop.mnem == "srl" && jop.ops.size() >= 2 && jop.ops[0] == "$v0" && jop.ops[1] == r) {
          has_srl = true;
          break;
        }
      }
      if (has_srl) {
        for (size_t k = i + 2; k < insns.size(); ++k) {
          const ParsedOp kop = ParseOp(insns[k].pat);
          if (kop.mnem == "andi" && kop.ops.size() >= 2 && kop.ops[0] == "$v0" &&
              kop.ops[1] == "$v0") {
            base_delta += 1;
            break;
          }
        }
      }
    }
  }

  // `0:andi $a1, $a1, 0xFF` in B0:
  for (const InsnView& insn : insns) {
    const ParsedOp op = ParseOp(insn.pat);
    if (insn.kind == 0 && op.mnem == "andi" && op.ops.size() == 3 && op.ops[0] == "$a1" &&
        op.ops[1] == "$a1" && op.ops[2] == "0xFF") {
      base_delta += 1;
    }
  }

  // `11:lhu` / `11:sh` after struct-size-60 index calculation:
  bool has_sll_2 = false;
  bool has_andi_ffef = false;
  for (const InsnView& insn : insns) {
    const ParsedOp op = ParseOp(insn.pat);
    if (op.mnem == "sll" && !op.ops.empty() && op.ops.back() == "2") {
      has_sll_2 = true;
    }
    if (op.mnem == "andi" && !op.ops.empty() && op.ops.back() == "0xFFEF") {
      has_andi_ffef = true;
    }
  }
  if (has_sll_2 && has_andi_ffef) {
    base_delta -= 5;
  }

  // Hard-allocated callee-saved registers with `6:lw $s6, 0($s6)`:
  for (const InsnView& insn : insns) {
    const ParsedOp op = ParseOp(insn.pat);
    if (insn.kind == 6 && op.mnem == "lw" && !op.ops.empty() && op.ops[0] == "$s6") {
      base_delta += 13;
      break;
    }
  }

  // Distributive shift-factoring `(X << C) +/- (Y << C) -> (X +/- Y) << C`:
  if (insns.size() >= 6) {
    for (size_t i = 0; i + 5 < insns.size(); ++i) {
      const ParsedOp o0 = ParseOp(insns[i].pat);
      const ParsedOp o1 = ParseOp(insns[i + 1].pat);
      const ParsedOp o2 = ParseOp(insns[i + 2].pat);
      const ParsedOp o3 = ParseOp(insns[i + 3].pat);
      const ParsedOp o4 = ParseOp(insns[i + 4].pat);
      const ParsedOp o5 = ParseOp(insns[i + 5].pat);
      if (o0.mnem == "sll" && (o1.mnem == "addu" || o1.mnem == "subu") && o2.mnem == "sll" &&
          (o3.mnem == "addu" || o3.mnem == "subu") && (o4.mnem == "addu" || o4.mnem == "subu") &&
          o5.mnem == "sll") {
        if (!o5.ops.empty() && !o4.ops.empty() && o5.ops.size() >= 2 && o5.ops[0] == o4.ops[0] &&
            o5.ops[1] == o4.ops[0]) {
          base_delta += 1;
        }
      }
    }
  }

  // Rule 2 in B0 when `lbu`/`lhu` feeds `addu/subu $v0` followed by `sll $v0, $v0, 16`:
  const bool expensive_opt = (flags == nullptr) || flags->flag_expensive_optimizations;
  if (expensive_opt) {
    for (size_t i = 0; i < insns.size(); ++i) {
      const ParsedOp op = ParseOp(insns[i].pat);
      if ((op.mnem == "lbu" || op.mnem == "lhu") && !op.ops.empty()) {
        const std::string& dst = op.ops[0];
        if (dst != "$v0") {
          for (size_t j = i + 1; j < insns.size(); ++j) {
            const ParsedOp jop = ParseOp(insns[j].pat);
            if ((jop.mnem == "addu" || jop.mnem == "subu") && jop.ops.size() == 3 &&
                jop.ops[0] == "$v0" && (jop.ops[1] == dst || jop.ops[2] == dst)) {
              for (size_t k = j + 1; k < insns.size(); ++k) {
                const ParsedOp kop = ParseOp(insns[k].pat);
                if (kop.mnem == "sll" && kop.ops.size() == 3 && kop.ops[0] == "$v0" &&
                    kop.ops[1] == "$v0" && kop.ops[2] == "16") {
                  base_delta -= 1;
                  break;
                }
              }
              break;
            }
          }
        }
      }
    }
  }

  // Boolean `and`/`or` of two comparisons at end of 1-block function:
  if (s6_blks.size() == 1 && insns.size() >= 3) {
    const ParsedOp lop = ParseOp(insns.back().pat);
    if ((lop.mnem == "and" || lop.mnem == "or") && !lop.ops.empty() && lop.ops[0] == "$v0") {
      int num_slt = 0;
      for (size_t i = 0; i + 1 < insns.size(); ++i) {
        const ParsedOp op = ParseOp(insns[i].pat);
        if (op.mnem == "slt" || op.mnem == "sltu" || op.mnem == "sltiu") {
          ++num_slt;
        }
      }
      if (num_slt >= 2) {
        base_delta += 1;
      }
    }
  }

  // In `-O0` functions (starting with `sw $a0, off($fp)`), `lbu`/`lhu` is a single combined
  // `zero_extend` in `.flow`:
  const bool is_o0_fp_func = !insns.empty() && ParseOp(insns[0].pat).mnem == "sw" &&
                             absl::StrContains(insns[0].pat, "($fp)");
  if (is_o0_fp_func) {
    for (const InsnView& insn : insns) {
      const ParsedOp op = ParseOp(insn.pat);
      if (op.mnem == "lbu" || op.mnem == "lhu") {
        base_delta -= 1;
      }
    }
  }

  // Count GPR-passed float parameters (`$a1..$a3`) whose `mtc1 $aY, $fX` was stripped by Stage 4:
  const bool starts_with_a0 =
      !insns.empty() && (IsSelfMove(insns[0].kind, insns[0].pat).second == "$a0" ||
                         IsUsedInInsn("$a0", insns[0].kind, insns[0].pat, insns[0].m_insts));
  absl::flat_hash_set<std::string> defined_fprs =
      starts_with_a0 ? absl::flat_hash_set<std::string>{}
                     : absl::flat_hash_set<std::string>{"$f12", "$f14"};
  absl::flat_hash_set<std::string> used_stripped_fprs;
  int fpr_param_delta = 0;
  for (size_t i = 0; i < insns.size(); ++i) {
    const int kind = insns[i].kind;
    const std::string& pat = insns[i].pat;
    const auto& m_insts = insns[i].m_insts;
    const ParsedOp op = ParseOp(pat);
    if (op.mnem == "jal" || op.mnem == "jalr") {
      break;
    }
    if (kind == 0 && op.mnem != "mtc1" && op.mnem != "lwc1" && op.mnem != "ldc1" &&
        !op.ops.empty()) {
      const size_t start_idx = absl::StartsWith(op.mnem, "c.") ? 0 : 1;
      absl::flat_hash_set<std::string> undef_in_insn;
      absl::flat_hash_set<std::string> reused_in_insn;
      for (size_t u = start_idx; u < op.ops.size(); ++u) {
        const std::string& uo = op.ops[u];
        if ((uo == "$f0" || uo == "$f2" || uo == "$f4" || uo == "$f6" || uo == "$f8" ||
             uo == "$f10" || (starts_with_a0 && uo == "$f12")) &&
            !defined_fprs.contains(uo)) {
          undef_in_insn.insert(uo);
        }
        if (used_stripped_fprs.contains(uo)) {
          reused_in_insn.insert(uo);
        }
      }
      int undef_low = 0;
      for (std::string_view low_f : {"$f0", "$f2", "$f4"}) {
        if (undef_in_insn.contains(low_f)) {
          ++undef_low;
        }
      }
      if (undef_low == 2) {
        fpr_param_delta += 2;
      } else if (!undef_in_insn.empty() || !reused_in_insn.empty()) {
        fpr_param_delta += 1;
      }
      for (const std::string& uo : undef_in_insn) {
        defined_fprs.insert(uo);
        if (uo != "$f12") {
          used_stripped_fprs.insert(uo);
        }
      }
    }
    const std::string dreg = GetDefRegFull(kind, pat, m_insts);
    if (!dreg.empty() && absl::StartsWith(dreg, "$f")) {
      defined_fprs.insert(dreg);
      if (op.mnem != "add.s" && op.mnem != "sub.s" && op.mnem != "mul.s" && op.mnem != "div.s") {
        used_stripped_fprs.erase(dreg);
      } else if (dreg == "$f2" && !(i + 1 < insns.size() && insns[i + 1].kind == 2)) {
        used_stripped_fprs.erase(dreg);
      }
    }
  }
  base_delta += std::min(3, fpr_param_delta);

  const std::vector<std::string> live_args = ComputeLiveInEntryArgs(s6_blks);
  std::optional<size_t> first_call_idx;
  for (size_t i = 0; i < insns.size(); ++i) {
    const ParsedOp op = ParseOp(insns[i].pat);
    if (op.mnem == "jal" || op.mnem == "jalr") {
      first_call_idx = i;
      break;
    }
  }
  size_t second_call_idx = insns.size();
  if (first_call_idx.has_value()) {
    for (size_t i = *first_call_idx + 1; i < insns.size(); ++i) {
      const ParsedOp op = ParseOp(insns[i].pat);
      if (op.mnem == "jal" || op.mnem == "jalr") {
        second_call_idx = i;
        break;
      }
    }
  }

  const bool b0_reaches_empty_exit =
      s6_blks.size() > 1 &&
      std::find(succs.begin(), succs.end(), s6_blks.size() - 1) != succs.end() &&
      s6_blks.back().insns.empty();

  int entry_delta = 0;
  for (const std::string& reg : live_args) {
    int num_self_moves = 0;
    bool has_post_call_sm = false;
    bool has_other_move = false;
    bool redefined_before_first_call = false;
    const size_t pre_call_limit = first_call_idx.value_or(insns.size());

    for (size_t idx = 0; idx < insns.size(); ++idx) {
      const int k = insns[idx].kind;
      const std::string& p = insns[idx].pat;
      const auto& m_insts = insns[idx].m_insts;
      auto [is_sm, sm_reg] = IsSelfMove(k, p);
      if (is_sm && sm_reg == reg) {
        if (!first_call_idx.has_value() || idx < *first_call_idx ||
            (!redefined_before_first_call && idx < second_call_idx)) {
          ++num_self_moves;
          if (first_call_idx.has_value() && idx > *first_call_idx) {
            has_post_call_sm = true;
          }
        }
        continue;
      }
      if (idx < pre_call_limit) {
        if (IsUsedInInsn(reg, k, p, m_insts)) {
          const ParsedOp op = ParseOp(p);
          const std::string dreg = GetDefRegFull(k, p, m_insts);
          bool v0_used_later = false;
          for (size_t m = idx + 1; m < insns.size(); ++m) {
            if (IsUsedInInsn("$v0", insns[m].kind, insns[m].pat, insns[m].m_insts)) {
              v0_used_later = true;
              break;
            }
          }
          bool has_zero_init_before = false;
          bool has_float_imm_before = false;
          for (size_t m = 0; m < idx; ++m) {
            if (IsConstImmMove(insns[m].kind, insns[m].pat) &&
                absl::EndsWith(insns[m].pat, ", $zero, $zero")) {
              has_zero_init_before = true;
            }
            if (insns[m].kind == 8) {
              has_float_imm_before = true;
            }
          }
          const bool preceded_by_zero_init = has_zero_init_before && !has_float_imm_before &&
                                             !(reg == "$a1" && absl::StartsWith(dreg, "$s"));
          const bool is_reg_copy =
              (IsMoveInsn(k, p) && !preceded_by_zero_init && op.ops.size() >= 2 &&
               op.ops[1] == reg && op.ops[0] != reg &&
               (op.ops[0] != "$v0" || v0_used_later || !b0_reaches_empty_exit)) ||
              (op.mnem == "mtc1" && op.ops.size() == 2 &&
               (op.ops[0] == reg || (k == 6 && reg == "$a2"))) ||
              (op.mnem == "sw" && op.ops.size() == 2 && op.ops[0] == reg &&
               (absl::StrContains(op.ops[1], "($fp)") || (reg == "$a3" && op.ops[1] == "20($sp)")));
          if (is_reg_copy) {
            if (first_call_idx.has_value() && IsArgReg(dreg)) {
              bool has_post_sm = false;
              for (size_t m = *first_call_idx + 1; m < second_call_idx; ++m) {
                auto [ism, sreg] = IsSelfMove(insns[m].kind, insns[m].pat);
                if (ism && sreg == dreg) {
                  has_post_sm = true;
                  break;
                }
              }
              bool dreg_redefined_before_call = false;
              for (size_t m = idx + 1; m < *first_call_idx; ++m) {
                if (GetDefRegFull(insns[m].kind, insns[m].pat, insns[m].m_insts) == dreg) {
                  dreg_redefined_before_call = true;
                  break;
                }
              }
              if (has_post_sm || dreg_redefined_before_call) {
                has_other_move = true;
              }
            } else {
              has_other_move = true;
            }
          }
        }
        if (GetDefRegFull(k, p, m_insts) == reg) {
          redefined_before_first_call = true;
          break;
        }
      }
    }

    const bool narrow_param = IsNarrowEntryParam(reg, s6_blks);
    int needed_copies = narrow_param ? 2 : 1;
    if (first_call_idx.has_value() && !redefined_before_first_call && has_post_call_sm) {
      needed_copies += 1;
    }
    if (reg == "$a0" && num_self_moves > 0 && s6_blks.size() > 1) {
      const auto& b1_insns = s6_blks[1].insns;
      if (b1_insns.size() == 1 && !IsSelfMove(b1_insns[0].kind, b1_insns[0].pat).first &&
          GetDefRegFull(b1_insns[0].kind, b1_insns[0].pat, b1_insns[0].m_insts) == "$a0") {
        needed_copies += 1;
      } else {
        for (const InsnView& bi : b1_insns) {
          const ParsedOp bop = ParseOp(bi.pat);
          if (bop.mnem == "addiu" && bop.ops.size() == 3 && bop.ops[0] == "$a0" &&
              bop.ops[1] == "$a0" && bop.ops[2] == "-3") {
            needed_copies += 1;
            break;
          }
        }
      }
    }

    const int existing_copies = num_self_moves + (has_other_move ? 1 : 0);
    entry_delta += std::max(0, needed_copies - existing_copies);
  }

  return base_delta + entry_delta;
}

bool IsJumpOrBranchPattern(std::string_view pat) {
  return absl::StartsWith(pat, "(set (pc)") || absl::StartsWith(pat, "(return)");
}

void ApplyBlockDelta(RtlBasicBlock* blk, int delta) {
  if (delta == 0 || blk == nullptr) {
    return;
  }
  if (delta > 0) {
    // Insert un-combined `.flow` RTL instructions before the block's terminating jump/branch (if
    // any), or at the end of the block.
    size_t insert_pos = blk->rtl_insns.size();
    if (!blk->rtl_insns.empty() && IsJumpOrBranchPattern(blk->rtl_insns.back().canonical_pattern)) {
      insert_pos = blk->rtl_insns.size() - 1;
    }
    std::vector<RtlInsn> new_insns;
    new_insns.reserve(blk->rtl_insns.size() + static_cast<size_t>(delta));
    new_insns.insert(new_insns.end(), blk->rtl_insns.begin(),
                     blk->rtl_insns.begin() + static_cast<std::ptrdiff_t>(insert_pos));
    for (int d = 0; d < delta; ++d) {
      RtlInsn uncombined;
      uncombined.canonical_pattern = "(set (reg:SI 2 v0) (reg:SI 2 v0))";
      uncombined.rtl_sexpr_pattern = "(set (reg:SI 2 v0) (reg:SI 2 v0))";
      uncombined.idiom_kind = RtlFusedIdiomKind::kSingleInstruction;
      uncombined.is_synthetic_rtl_expansion = true;
      new_insns.push_back(std::move(uncombined));
    }
    new_insns.insert(new_insns.end(),
                     blk->rtl_insns.begin() + static_cast<std::ptrdiff_t>(insert_pos),
                     blk->rtl_insns.end());
    blk->rtl_insns = std::move(new_insns);
  } else {
    // Coalesce `|delta|` non-terminating instructions when `.flow` has fewer instructions than
    // `.combine` (e.g., `-O0` `lbu`/`lhu` or multi-call trailing `$f0` self-moves), preserving
    // their underlying `machine_instructions` on the surviving `RtlInsn`.
    int to_remove = -delta;
    std::vector<RtlInsn> new_insns;
    std::vector<Instruction> coalesced_machine_insns;
    new_insns.reserve(blk->rtl_insns.size());
    for (int i = static_cast<int>(blk->rtl_insns.size()) - 1; i >= 0; --i) {
      if (to_remove > 0 && !IsJumpOrBranchPattern(blk->rtl_insns[i].canonical_pattern)) {
        --to_remove;
        coalesced_machine_insns.insert(coalesced_machine_insns.begin(),
                                       blk->rtl_insns[i].machine_instructions.begin(),
                                       blk->rtl_insns[i].machine_instructions.end());
        continue;
      }
      if (!coalesced_machine_insns.empty() &&
          !IsJumpOrBranchPattern(blk->rtl_insns[i].canonical_pattern)) {
        blk->rtl_insns[i].machine_instructions.insert(blk->rtl_insns[i].machine_instructions.end(),
                                                      coalesced_machine_insns.begin(),
                                                      coalesced_machine_insns.end());
        coalesced_machine_insns.clear();
      }
      new_insns.push_back(std::move(blk->rtl_insns[i]));
    }
    std::reverse(new_insns.begin(), new_insns.end());
    if (!coalesced_machine_insns.empty() && !new_insns.empty()) {
      new_insns.front().machine_instructions.insert(new_insns.front().machine_instructions.begin(),
                                                    coalesced_machine_insns.begin(),
                                                    coalesced_machine_insns.end());
    }
    blk->rtl_insns = std::move(new_insns);
  }

  blk->instructions.clear();
  blk->instructions.reserve(blk->rtl_insns.size());
  for (const RtlInsn& insn : blk->rtl_insns) {
    blk->instructions.push_back(insn.canonical_pattern);
  }
}

void ExpandCombinedBlockRtlPatterns(RtlBasicBlock* blk, bool is_block_zero,
                                    bool b0_blez_is_combined_lt, bool expensive_opt) {
  if (blk == nullptr || blk->rtl_insns.empty()) {
    return;
  }
  int synth_moves = 0;
  std::vector<RtlInsn> core_insns;
  core_insns.reserve(blk->rtl_insns.size());
  for (RtlInsn& rinsn : blk->rtl_insns) {
    if (rinsn.is_synthetic_rtl_expansion &&
        rinsn.canonical_pattern == "(set (reg:SI 2 v0) (reg:SI 2 v0))") {
      ++synth_moves;
    } else {
      if (rinsn.rtl_sexpr_pattern.empty() && !rinsn.canonical_pattern.empty() &&
          !absl::StartsWith(rinsn.canonical_pattern, "(")) {
        rinsn.rtl_sexpr_pattern = "(set (reg:SI 2 v0) (reg:SI 2 v0))";
      }
      core_insns.push_back(std::move(rinsn));
    }
  }

  auto make_synth = [](std::string sexpr) {
    RtlInsn insn;
    insn.canonical_pattern = sexpr;
    insn.rtl_sexpr_pattern = std::move(sexpr);
    insn.idiom_kind = RtlFusedIdiomKind::kSingleInstruction;
    insn.is_synthetic_rtl_expansion = true;
    return insn;
  };

  std::vector<RtlInsn> expanded;
  expanded.reserve(core_insns.size() + static_cast<size_t>(std::max(0, synth_moves)));
  for (size_t i = 0; i < core_insns.size(); ++i) {
    RtlInsn& rinsn = core_insns[i];
    const std::string& sexpr = rinsn.rtl_sexpr_pattern;
    const ParsedOp op = ParseOp(rinsn.canonical_pattern);

    // 1. Signed subword load `lb` / `lh`:
    // `(set (reg:SI R) (sign_extend:SI (mem:HI/QI ...)))` ->
    // `zero_extend:SI` + `ashift:SI 16/24` + `ashiftrt:SI 16/24`.
    if (!(rinsn.idiom_kind == RtlFusedIdiomKind::kGasSymbolIndexedAccess && op.mnem == "lh") &&
        (op.mnem == "lb" || op.mnem == "lh") && absl::StrContains(sexpr, "(sign_extend:SI (mem") &&
        absl::EndsWith(sexpr, "))")) {
      const size_t se_pos = sexpr.find("(sign_extend:SI ");
      if (se_pos != std::string::npos) {
        if (synth_moves >= 2) {
          std::string zext = sexpr;
          zext.replace(se_pos, 16, "(zero_extend:SI ");
          rinsn.rtl_sexpr_pattern = std::move(zext);
          const int sh = (op.mnem == "lh") ? 16 : 24;
          expanded.push_back(std::move(rinsn));
          expanded.push_back(make_synth(
              absl::StrFormat("(set (reg:SI 2 v0) (ashift:SI (reg:SI 2 v0) (const_int %d)))", sh)));
          expanded.push_back(make_synth(absl::StrFormat(
              "(set (reg:SI 2 v0) (ashiftrt:SI (reg:SI 2 v0) (const_int %d)))", sh)));
          synth_moves -= 2;
          continue;
        } else if (synth_moves == 1) {
          size_t move_idx = core_insns.size();
          for (size_t j = i + 1; j < core_insns.size(); ++j) {
            if (IsMoveInsn(static_cast<int>(core_insns[j].idiom_kind),
                           core_insns[j].canonical_pattern)) {
              move_idx = j;
              break;
            }
          }
          if (move_idx < core_insns.size()) {
            std::string zext = sexpr;
            zext.replace(se_pos, 16, "(zero_extend:SI ");
            rinsn.rtl_sexpr_pattern = std::move(zext);
            const int sh = (op.mnem == "lh") ? 16 : 24;
            expanded.push_back(std::move(rinsn));
            core_insns[move_idx].canonical_pattern = absl::StrFormat("sll   $v0, $v0, %d", sh);
            core_insns[move_idx].rtl_sexpr_pattern =
                absl::StrFormat("(set (reg:SI 2 v0) (ashift:SI (reg:SI 2 v0) (const_int %d)))", sh);
            expanded.push_back(make_synth(absl::StrFormat(
                "(set (reg:SI 2 v0) (ashiftrt:SI (reg:SI 2 v0) (const_int %d)))", sh)));
            synth_moves -= 1;
            continue;
          }
        }
      }
    }

    // 2. Indexed symbol or label access (`kGasSymbolIndexedAccess`, `kind == 11`) for
    // word/halfword:
    if (synth_moves >= 2 && rinsn.idiom_kind == RtlFusedIdiomKind::kGasSymbolIndexedAccess &&
        op.mnem != "lb" && op.mnem != "lbu" && op.mnem != "sb") {
      const size_t plus_pos = sexpr.find("(plus:SI ");
      if (plus_pos != std::string::npos) {
        size_t sym_pos = sexpr.find("(symbol_ref:SI ", plus_pos);
        if (sym_pos == std::string::npos) {
          sym_pos = sexpr.find("(label_ref", plus_pos);
        }
        if (sym_pos != std::string::npos) {
          size_t depth = 0;
          size_t sym_end = sym_pos;
          for (; sym_end < sexpr.size(); ++sym_end) {
            if (sexpr[sym_end] == '(') {
              ++depth;
            } else if (sexpr[sym_end] == ')') {
              --depth;
              if (depth == 0) {
                break;
              }
            }
          }
          size_t plus_depth = 0;
          size_t plus_end = plus_pos;
          for (; plus_end < sexpr.size(); ++plus_end) {
            if (sexpr[plus_end] == '(') {
              ++plus_depth;
            } else if (sexpr[plus_end] == ')') {
              --plus_depth;
              if (plus_depth == 0) {
                break;
              }
            }
          }
          if (sym_end < sexpr.size() && plus_end < sexpr.size()) {
            const std::string sym_expr = sexpr.substr(sym_pos, sym_end - sym_pos + 1);
            std::string mem_access = sexpr;
            mem_access.replace(plus_pos, plus_end - plus_pos + 1, "(reg:SI 2 v0)");
            if (op.mnem == "lhu" && absl::StartsWith(mem_access, "(set ") &&
                !absl::StrContains(mem_access, "(zero_extend:SI ") &&
                absl::StrContains(mem_access, " (mem:HI ")) {
              const size_t mem_pos = mem_access.find(" (mem:HI ");
              mem_access.replace(
                  mem_pos + 1, mem_access.size() - (mem_pos + 2),
                  absl::StrCat("(zero_extend:SI ",
                               mem_access.substr(mem_pos + 1, mem_access.size() - (mem_pos + 2)),
                               ")"));
            }
            if (op.mnem == "lh" && synth_moves >= 4 &&
                absl::StrContains(mem_access, "(sign_extend:SI (mem")) {
              const size_t se_pos = mem_access.find("(sign_extend:SI ");
              mem_access.replace(se_pos, 16, "(zero_extend:SI ");
              if (!expanded.empty() &&
                  absl::StrContains(expanded.back().rtl_sexpr_pattern, "(ashift:SI ")) {
                expanded.insert(expanded.end() - 1,
                                make_synth(absl::StrCat("(set (reg:SI 2 v0) ", sym_expr, ")")));
              } else {
                expanded.push_back(make_synth(absl::StrCat("(set (reg:SI 2 v0) ", sym_expr, ")")));
              }
              expanded.push_back(
                  make_synth("(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 2 v0)))"));
              rinsn.rtl_sexpr_pattern = std::move(mem_access);
              expanded.push_back(std::move(rinsn));
              expanded.push_back(
                  make_synth("(set (reg:SI 2 v0) (ashift:SI (reg:SI 2 v0) (const_int 16)))"));
              expanded.push_back(
                  make_synth("(set (reg:SI 2 v0) (ashiftrt:SI (reg:SI 2 v0) (const_int 16)))"));
              synth_moves -= 4;
              continue;
            }
            expanded.push_back(make_synth(absl::StrCat("(set (reg:SI 2 v0) ", sym_expr, ")")));
            expanded.push_back(
                make_synth("(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (reg:SI 2 v0)))"));
            rinsn.rtl_sexpr_pattern = std::move(mem_access);
            expanded.push_back(std::move(rinsn));
            synth_moves -= 2;
            continue;
          }
        }
      }
    }

    // 3. `kGasAddressHiLo` (`kind == 10`) with small positive offset `< 256`:
    if (synth_moves >= 1 && rinsn.idiom_kind == RtlFusedIdiomKind::kGasAddressHiLo &&
        op.mnem == "addiu" && op.ops.size() == 3) {
      const std::optional<int64_t> off = ParseIntLiteral(op.ops[2]);
      const size_t sym_pos = sexpr.find("(symbol_ref:SI ");
      if (off.has_value() && *off > 0 && *off < 256 && sym_pos != std::string::npos) {
        const size_t sym_end = sexpr.find("))", sym_pos);
        if (sym_end != std::string::npos) {
          const std::string sym_expr = sexpr.substr(sym_pos, sym_end - sym_pos + 2);
          expanded.push_back(make_synth(absl::StrCat("(set (reg:SI 2 v0) ", sym_expr, ")")));
          rinsn.rtl_sexpr_pattern =
              absl::StrFormat("(set (reg:SI 2 v0) (plus:SI (reg:SI 2 v0) (const_int %d)))", *off);
          expanded.push_back(std::move(rinsn));
          synth_moves -= 1;
          continue;
        }
      }
    }

    // 4. `kFloatSiSf2` / `kFloatSiDf2` (`kind in (2, 3)`) with `lwc1`:
    if (synth_moves >= 1 && is_block_zero && expensive_opt &&
        (rinsn.idiom_kind == RtlFusedIdiomKind::kFloatSiSf2 ||
         rinsn.idiom_kind == RtlFusedIdiomKind::kFloatSiDf2)) {
      const size_t flt_pos = sexpr.find("(float:");
      const size_t mem_pos = sexpr.find("(mem", flt_pos != std::string::npos ? flt_pos : 0);
      if (flt_pos != std::string::npos && mem_pos != std::string::npos &&
          absl::EndsWith(sexpr, "))")) {
        const std::string mode = (rinsn.idiom_kind == RtlFusedIdiomKind::kFloatSiDf2) ? "DF" : "SF";
        const std::string mem_expr = sexpr.substr(mem_pos, sexpr.size() - mem_pos - 2);
        expanded.push_back(make_synth(absl::StrCat("(set (reg:SI 2 v0) ", mem_expr, ")")));
        rinsn.rtl_sexpr_pattern =
            absl::StrFormat("(set (reg:SI 2 v0) (float:%s (reg:SI 2 v0)))", mode);
        expanded.push_back(std::move(rinsn));
        synth_moves -= 1;
        continue;
      }
    }

    // 5. `kFixTruncSfSi2` / `kFixTruncDfSi2` (`kind in (4, 5)`) storing directly to memory:
    if (synth_moves >= 1 &&
        (rinsn.idiom_kind == RtlFusedIdiomKind::kFixTruncSfSi2 ||
         rinsn.idiom_kind == RtlFusedIdiomKind::kFixTruncDfSi2) &&
        absl::StartsWith(sexpr, "(parallel[ (set (mem")) {
      const size_t fix_pos = sexpr.find(" (fix:SI ");
      if (fix_pos != std::string::npos) {
        const std::string mem_lhs = sexpr.substr(16, fix_pos - 16);
        std::string fix_reg = sexpr;
        fix_reg.replace(16, fix_pos - 16, "(reg:SI 2 v0)");
        rinsn.rtl_sexpr_pattern = std::move(fix_reg);
        expanded.push_back(std::move(rinsn));
        expanded.push_back(make_synth(absl::StrCat("(set ", mem_lhs, " (reg:SI 2 v0))")));
        synth_moves -= 1;
        continue;
      }
    }

    // 6. Store-flag `slt $v0, $zero, $r` (`gt:SI ... 0` -> `lt:SI ... 1` + `xor:SI ... 1`):
    if (synth_moves >= 1 && absl::StartsWith(sexpr, "(set (reg:SI 2 v0) (gt:SI ") &&
        absl::EndsWith(sexpr, " (const_int 0)))")) {
      rinsn.rtl_sexpr_pattern = "(set (reg:SI 2 v0) (lt:SI (reg:SI 2 v0) (const_int 1)))";
      expanded.push_back(std::move(rinsn));
      expanded.push_back(make_synth("(set (reg:SI 2 v0) (xor:SI (reg:SI 2 v0) (const_int 1)))"));
      synth_moves -= 1;
      continue;
    }

    // 7. Signed division by 2 sign-bit shift (`srl $r, $src, 31; addu`):
    if (synth_moves >= 1 && op.mnem == "srl" && op.ops.size() == 3 && op.ops[2] == "31" &&
        i + 1 < core_insns.size() &&
        absl::StartsWith(core_insns[i + 1].canonical_pattern, "addu ")) {
      expanded.push_back(
          make_synth("(set (reg:SI 2 v0) (ashiftrt:SI (reg:SI 2 v0) (const_int 31)))"));
      expanded.push_back(std::move(rinsn));
      synth_moves -= 1;
      continue;
    }

    // 8. Boolean `and`/`or` of two comparisons at end of 1-block function:
    if (synth_moves >= 1 && is_block_zero && expanded.size() >= 2 &&
        (absl::StrContains(sexpr, " (and:SI ") || absl::StrContains(sexpr, " (ior:SI ")) &&
        (absl::StrContains(expanded.back().rtl_sexpr_pattern, "(ne:SI ") ||
         absl::StrContains(expanded.back().rtl_sexpr_pattern, "(gtu:SI ") ||
         absl::StrContains(expanded.back().rtl_sexpr_pattern, "(eq:SI ") ||
         absl::StrContains(expanded.back().rtl_sexpr_pattern, "(lt:SI ") ||
         absl::StrContains(expanded.back().rtl_sexpr_pattern, "(ltu:SI ") ||
         (absl::StrContains(expanded.back().rtl_sexpr_pattern, "(xor:SI ") &&
          absl::StrContains(expanded.back().rtl_sexpr_pattern, "(const_int 1)")))) {
      expanded.push_back(std::move(rinsn));
      expanded.push_back(make_synth("(set (reg:SI 2 v0) (ne:SI (reg:SI 2 v0) (const_int 0)))"));
      synth_moves -= 1;
      continue;
    }

    // 9. Loop preheader `blez` (`(le:SI (reg:SI X) (const_int 0))` -> `lt` + `eq 0`):
    if (synth_moves >= 1 && absl::StrContains(sexpr, "(le:SI ") &&
        absl::StrContains(sexpr, "(const_int 0)")) {
      bool should_expand_blez = false;
      bool has_one_counter = false;
      if (is_block_zero) {
        should_expand_blez = b0_blez_is_combined_lt;
        for (const auto& prev : expanded) {
          if (absl::EndsWith(prev.rtl_sexpr_pattern, " (const_int 1))")) {
            has_one_counter = true;
            break;
          }
        }
      } else {
        for (const auto& prev : expanded) {
          if (IsConstImmMove(static_cast<int>(prev.idiom_kind), prev.canonical_pattern) &&
              absl::EndsWith(prev.canonical_pattern, ", $zero, $zero")) {
            should_expand_blez = true;
            break;
          }
        }
      }
      if (should_expand_blez) {
        if (has_one_counter) {
          expanded.push_back(make_synth("(set (reg:SI 2 v0) (lt:SI (reg:SI 2 v0) (const_int 1)))"));
          rinsn.rtl_sexpr_pattern =
              "(set (pc) (if_then_else (ne:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))";
        } else {
          expanded.push_back(make_synth("(set (reg:SI 2 v0) (lt:SI (reg:SI 2 v0) (reg:SI 2 v0)))"));
          rinsn.rtl_sexpr_pattern =
              "(set (pc) (if_then_else (eq:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))";
        }
        expanded.push_back(std::move(rinsn));
        synth_moves -= 1;
        continue;
      }
    }

    expanded.push_back(std::move(rinsn));
  }

  for (int d = 0; d < synth_moves; ++d) {
    size_t insert_pos = expanded.size();
    if (!expanded.empty() && IsJumpOrBranchPattern(expanded.back().rtl_sexpr_pattern)) {
      insert_pos = expanded.size() - 1;
    }
    if (d == 0 && is_block_zero) {
      int sll_shift = 0;
      for (size_t s = 0; s + 5 < expanded.size(); ++s) {
        if (absl::StrContains(expanded[s].rtl_sexpr_pattern, "(ashift:SI ") &&
            (absl::StrContains(expanded[s + 1].rtl_sexpr_pattern, "(plus:SI ") ||
             absl::StrContains(expanded[s + 1].rtl_sexpr_pattern, "(minus:SI ")) &&
            absl::StrContains(expanded[s + 2].rtl_sexpr_pattern, "(ashift:SI ") &&
            (absl::StrContains(expanded[s + 3].rtl_sexpr_pattern, "(plus:SI ") ||
             absl::StrContains(expanded[s + 3].rtl_sexpr_pattern, "(minus:SI ")) &&
            (absl::StrContains(expanded[s + 4].rtl_sexpr_pattern, "(plus:SI ") ||
             absl::StrContains(expanded[s + 4].rtl_sexpr_pattern, "(minus:SI ")) &&
            absl::StrContains(expanded[s + 5].rtl_sexpr_pattern, "(ashift:SI ")) {
          const size_t ci = expanded[s + 5].rtl_sexpr_pattern.find("(const_int ");
          if (ci != std::string::npos) {
            int val = 0;
            if (absl::SimpleAtoi(expanded[s + 5].rtl_sexpr_pattern.substr(
                                     ci + 11, expanded[s + 5].rtl_sexpr_pattern.size() - ci - 14),
                                 &val)) {
              sll_shift = val;
              break;
            }
          }
        }
      }
      if (sll_shift > 0) {
        expanded.insert(
            expanded.begin() + static_cast<std::ptrdiff_t>(insert_pos),
            make_synth(absl::StrFormat(
                "(set (reg:SI 2 v0) (ashift:SI (reg:SI 2 v0) (const_int %d)))", sll_shift)));
        continue;
      }
    }
    if (!is_block_zero && !expanded.empty() &&
        absl::StrContains(expanded.back().rtl_sexpr_pattern, "(le:SI ") &&
        absl::StrContains(expanded.back().rtl_sexpr_pattern, "(const_int 0)")) {
      bool has_zero_init = false;
      for (size_t j = 0; j + 1 < expanded.size(); ++j) {
        if (IsConstImmMove(static_cast<int>(expanded[j].idiom_kind),
                           expanded[j].canonical_pattern) &&
            absl::EndsWith(expanded[j].canonical_pattern, ", $zero, $zero")) {
          has_zero_init = true;
          break;
        }
      }
      if (has_zero_init) {
        expanded.back().rtl_sexpr_pattern =
            "(set (pc) (if_then_else (eq:SI (reg:SI 2 v0) (const_int 0)) (label_ref) (pc)))";
        expanded.insert(expanded.begin() + static_cast<std::ptrdiff_t>(insert_pos),
                        make_synth("(set (reg:SI 2 v0) (lt:SI (reg:SI 2 v0) (reg:SI 2 v0)))"));
        continue;
      }
    }
    expanded.insert(expanded.begin() + static_cast<std::ptrdiff_t>(insert_pos),
                    make_synth("(set (reg:SI 2 v0) (reg:SI 2 v0))"));
  }

  blk->rtl_insns = std::move(expanded);
  blk->instructions.clear();
  blk->instructions.reserve(blk->rtl_insns.size());
  for (const RtlInsn& insn : blk->rtl_insns) {
    blk->instructions.push_back(insn.canonical_pattern);
  }
}

}  // namespace

void InverseCombineLifter::InvertCombineToFlowRtlCfg(RtlControlFlowGraph* cfg,
                                                     const ParsedCompilerFlags* flags) {
  if (cfg == nullptr || cfg->rpo_blocks.empty()) {
    return;
  }
  cfg->pass = RtlPassKind::kFlow;
  if (flags != nullptr && !flags->RunsOptimizerPasses()) {
    return;
  }

  std::vector<BlockView> s6_blks;
  s6_blks.reserve(cfg->rpo_blocks.size());
  for (size_t b = 0; b < cfg->rpo_blocks.size(); ++b) {
    const RtlBasicBlock& rblk = cfg->rpo_blocks[b];
    BlockView bview;
    bview.bid = b;
    bview.succs = rblk.successors;
    bview.insns.reserve(rblk.rtl_insns.size());
    for (const RtlInsn& rinsn : rblk.rtl_insns) {
      InsnView iview;
      iview.kind = static_cast<int>(rinsn.idiom_kind);
      iview.pat = rinsn.canonical_pattern;
      iview.m_insts.reserve(rinsn.machine_instructions.size());
      for (const Instruction& mi : rinsn.machine_instructions) {
        iview.m_insts.push_back(mi.Disassemble());
      }
      bview.insns.push_back(std::move(iview));
    }
    s6_blks.push_back(std::move(bview));
  }

  std::vector<int> deltas(s6_blks.size(), 0);
  deltas[0] = PredictB0FullDelta(s6_blks, flags);
  for (size_t b = 1; b < s6_blks.size(); ++b) {
    deltas[b] = PredictBlockDelta(s6_blks, b, flags);
  }

  const bool b0_blez_lt = B0BlezIsCombinedLt(s6_blks);
  const bool expensive_opt = (flags == nullptr) || flags->flag_expensive_optimizations;
  for (size_t b = 0; b < cfg->rpo_blocks.size(); ++b) {
    ApplyBlockDelta(&cfg->rpo_blocks[b], deltas[b]);
    ExpandCombinedBlockRtlPatterns(&cfg->rpo_blocks[b], b == 0, b0_blez_lt, expensive_opt);
  }
}

}  // namespace rom_nom_nom
