#include "lifter/invert_final_scan_insn.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/base/casts.h"
#include "absl/container/flat_hash_map.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/types/span.h"
#include "core/rtl_cfg.h"

namespace rom_nom_nom::lifter {
namespace {

constexpr std::array<std::string_view, 32> kMipsGprRtlNames = {
    "$0", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6",  "t7",
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "$fp", "ra",
};

int ParseGprNumber(std::string_view tok) {
  tok = absl::StripAsciiWhitespace(tok);
  if (!absl::StartsWith(tok, "$") || tok.size() < 2) {
    return -1;
  }
  const std::string_view body = tok.substr(1);
  if (body == "zero") {
    return 0;
  }
  if (body == "fp" || body == "s8") {
    return 30;
  }
  int regno = -1;
  if (absl::SimpleAtoi(body, &regno)) {
    for (char c : body) {
      if (!absl::ascii_isdigit(static_cast<unsigned char>(c))) {
        return -1;
      }
    }
    return (regno >= 0 && regno < 32) ? regno : -1;
  }
  for (size_t i = 1; i < kMipsGprRtlNames.size(); ++i) {
    if (body == kMipsGprRtlNames[i]) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int ParseFprNumber(std::string_view tok) {
  tok = absl::StripAsciiWhitespace(tok);
  if (!absl::StartsWith(tok, "$f") || tok.size() < 3) {
    return -1;
  }
  const std::string_view digits = tok.substr(2);
  for (char c : digits) {
    if (!absl::ascii_isdigit(static_cast<unsigned char>(c))) {
      return -1;
    }
  }
  int fno = -1;
  if (!absl::SimpleAtoi(digits, &fno) || fno < 0 || fno >= 32) {
    return -1;
  }
  return fno;
}

std::string FormatGprRtl(int regno, std::string_view mode = "SI") {
  return absl::StrFormat("(reg:%s %d %s)", mode, regno, kMipsGprRtlNames[regno]);
}

std::string FormatFprRtl(int fno, std::string_view mode = "SF") {
  return absl::StrFormat("(reg:%s %d $f%d)", mode, 32 + fno, fno);
}

std::optional<int64_t> ParseIntImmediate(std::string_view tok) {
  tok = absl::StripAsciiWhitespace(tok);
  if (tok.empty()) {
    return std::nullopt;
  }
  bool neg = false;
  if (tok.front() == '-' || tok.front() == '+') {
    neg = (tok.front() == '-');
    tok.remove_prefix(1);
  }
  if (tok.empty()) {
    return std::nullopt;
  }
  if (absl::StartsWith(tok, "0x") || absl::StartsWith(tok, "0X")) {
    const std::string_view hex_digits = tok.substr(2);
    if (hex_digits.empty()) {
      return std::nullopt;
    }
    for (char c : hex_digits) {
      if (!absl::ascii_isxdigit(static_cast<unsigned char>(c))) {
        return std::nullopt;
      }
    }
    uint64_t uval = 0;
    if (!absl::SimpleHexAtoi(hex_digits, &uval)) {
      return std::nullopt;
    }
    if (neg) {
      return -static_cast<int64_t>(uval);
    }
    return static_cast<int64_t>(static_cast<int32_t>(uval));
  }
  for (char c : tok) {
    if (!absl::ascii_isdigit(static_cast<unsigned char>(c))) {
      return std::nullopt;
    }
  }
  int64_t val = 0;
  if (!absl::SimpleAtoi(tok, &val)) {
    return std::nullopt;
  }
  return neg ? -val : val;
}

bool IsValidSymbolOrLabelIdentifier(std::string_view s) {
  if (s.empty()) {
    return false;
  }
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (absl::ascii_isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '$') {
      continue;
    }
    return false;
  }
  return ParseGprNumber(s) < 0 && ParseFprNumber(s) < 0 && !ParseIntImmediate(s).has_value();
}

bool IsLocalLabelName(std::string_view s, std::string* out_label_token) {
  s = absl::StripAsciiWhitespace(s);
  if ((absl::StartsWith(s, "$L") || absl::StartsWith(s, ".L")) && s.size() >= 3) {
    const std::string_view rest = s.substr(2);
    for (char c : rest) {
      if (!absl::ascii_isdigit(static_cast<unsigned char>(c))) {
        return false;
      }
    }
    if (out_label_token != nullptr) {
      *out_label_token = std::string(s.substr(1));
    }
    return true;
  }
  if (absl::StartsWith(s, "L") && s.size() >= 2 &&
      absl::ascii_isdigit(static_cast<unsigned char>(s[1]))) {
    for (char c : s.substr(1)) {
      if (!absl::ascii_isdigit(static_cast<unsigned char>(c))) {
        return false;
      }
    }
    if (out_label_token != nullptr) {
      *out_label_token = std::string(s);
    }
    return true;
  }
  return false;
}

// Formats a symbolic or constant expression `<sym>`, `$L<n>`, `<sym>+<off>`, or `<sym>-<off>`.
std::string FormatSymbolicAddressExpr(std::string_view expr, std::string_view mode = "SI") {
  expr = absl::StripAsciiWhitespace(expr);
  if (expr.empty()) {
    return "";
  }
  if (absl::StartsWith(expr, "%hi(") && absl::EndsWith(expr, ")")) {
    const std::string inner = FormatSymbolicAddressExpr(expr.substr(4, expr.size() - 5), mode);
    if (inner.empty()) {
      return "";
    }
    return absl::StrFormat("(high:%s %s)", mode, inner);
  }
  std::string lbl_tok;
  if (IsLocalLabelName(expr, &lbl_tok)) {
    return absl::StrFormat("(label_ref %s)", lbl_tok);
  }
  size_t op_pos = std::string_view::npos;
  for (size_t i = 1; i < expr.size(); ++i) {
    if (expr[i] == '+' || expr[i] == '-') {
      op_pos = i;
      break;
    }
  }
  if (op_pos != std::string_view::npos) {
    const std::string_view sym_part = absl::StripAsciiWhitespace(expr.substr(0, op_pos));
    const std::string_view off_part = absl::StripAsciiWhitespace(expr.substr(op_pos));
    const auto off = ParseIntImmediate(off_part);
    if (!IsValidSymbolOrLabelIdentifier(sym_part) || !off.has_value()) {
      return "";
    }
    std::string base_rtx;
    if (IsLocalLabelName(sym_part, &lbl_tok)) {
      base_rtx = absl::StrFormat("(label_ref %s)", lbl_tok);
    } else {
      base_rtx = absl::StrFormat("(symbol_ref:%s (\"%s\"))", mode, sym_part);
    }
    if (*off == 0) {
      return base_rtx;
    }
    return absl::StrFormat("(const:%s (plus:%s %s (const_int %d)))", mode, mode, base_rtx,
                           static_cast<int32_t>(*off));
  }
  if (IsValidSymbolOrLabelIdentifier(expr)) {
    return absl::StrFormat("(symbol_ref:%s (\"%s\"))", mode, expr);
  }
  return "";
}

// Formats a memory address operand `<off>($base)`, `($base)`, `%lo(<sym>)($base)`,
// `<sym>($base)`, `<sym>+<off>($base)`, or direct `<sym>` / `<sym>+<off>`.
std::string FormatMemAddressRtl(std::string_view mem_arg, std::string_view addr_mode = "SI") {
  mem_arg = absl::StripAsciiWhitespace(mem_arg);
  if (mem_arg.empty()) {
    return "";
  }
  if (absl::StartsWith(mem_arg, "%lo(")) {
    const size_t close_lo = mem_arg.find(')');
    if (close_lo == std::string_view::npos || close_lo + 1 >= mem_arg.size()) {
      return "";
    }
    const std::string_view sym_expr = mem_arg.substr(4, close_lo - 4);
    const std::string_view rest = absl::StripAsciiWhitespace(mem_arg.substr(close_lo + 1));
    if (!absl::StartsWith(rest, "(") || !absl::EndsWith(rest, ")")) {
      return "";
    }
    const int base_reg = ParseGprNumber(rest.substr(1, rest.size() - 2));
    const std::string sym_rtx = FormatSymbolicAddressExpr(sym_expr, addr_mode);
    if (base_reg < 0 || sym_rtx.empty()) {
      return "";
    }
    return absl::StrFormat("(lo_sum:%s %s %s)", addr_mode, FormatGprRtl(base_reg, addr_mode),
                           sym_rtx);
  }

  const size_t lparen = mem_arg.rfind('(');
  if (lparen != std::string_view::npos) {
    if (!absl::EndsWith(mem_arg, ")")) {
      return "";
    }
    const std::string_view prefix = absl::StripAsciiWhitespace(mem_arg.substr(0, lparen));
    const std::string_view base_str =
        absl::StripAsciiWhitespace(mem_arg.substr(lparen + 1, mem_arg.size() - lparen - 2));
    const int base_reg = ParseGprNumber(base_str);
    if (base_reg < 0) {
      return "";
    }
    const std::string base_rtx = FormatGprRtl(base_reg, addr_mode);
    if (prefix.empty()) {
      return base_rtx;
    }
    if (const auto off = ParseIntImmediate(prefix); off.has_value()) {
      if (*off == 0) {
        return base_rtx;
      }
      return absl::StrFormat("(plus:%s %s (const_int %d))", addr_mode, base_rtx,
                             static_cast<int32_t>(*off));
    }
    const std::string sym_rtx = FormatSymbolicAddressExpr(prefix, addr_mode);
    if (!sym_rtx.empty()) {
      return absl::StrFormat("(plus:%s %s %s)", addr_mode, base_rtx, sym_rtx);
    }
    return "";
  }

  if (const auto abs_addr = ParseIntImmediate(mem_arg); abs_addr.has_value()) {
    return absl::StrFormat("(const_int %d)", static_cast<int32_t>(*abs_addr));
  }
  return FormatSymbolicAddressExpr(mem_arg, addr_mode);
}

std::vector<std::string_view> SplitOperands(std::string_view args_str) {
  args_str = absl::StripAsciiWhitespace(args_str);
  if (args_str.empty()) {
    return {};
  }
  std::vector<std::string_view> ops = absl::StrSplit(args_str, ',');
  for (std::string_view& op : ops) {
    op = absl::StripAsciiWhitespace(op);
  }
  return ops;
}

std::string InvertTwoLineFloatSi(std::string_view line0, std::string_view line1) {
  auto strip_comment = [](std::string_view s) {
    const size_t hash_pos = s.find('#');
    if (hash_pos != std::string_view::npos) {
      s = s.substr(0, hash_pos);
    }
    return absl::StripAsciiWhitespace(s);
  };
  line0 = strip_comment(line0);
  line1 = strip_comment(line1);
  if (line0.empty() || line1.empty()) {
    return "";
  }

  auto split_mnemonic = [](std::string_view line) -> std::pair<std::string_view, std::string_view> {
    size_t sp = 0;
    while (sp < line.size() && !absl::ascii_isspace(static_cast<unsigned char>(line[sp]))) {
      ++sp;
    }
    return {line.substr(0, sp), absl::StripAsciiWhitespace(line.substr(sp))};
  };

  const auto [m0, a0_str] = split_mnemonic(line0);
  const auto [m1, a1_str] = split_mnemonic(line1);
  if (m1 != "cvt.s.w" && m1 != "cvt.d.w") {
    return "";
  }
  const std::string_view target_mode = (m1 == "cvt.s.w") ? "SF" : "DF";
  const std::vector<std::string_view> ops0 = SplitOperands(a0_str);
  const std::vector<std::string_view> ops1 = SplitOperands(a1_str);
  if (ops0.size() != 2 || ops1.size() != 2) {
    return "";
  }
  const int dst_fpr = ParseFprNumber(ops1[0]);
  const int src_fpr = ParseFprNumber(ops1[1]);
  if (dst_fpr < 0 || src_fpr < 0 || dst_fpr != src_fpr) {
    return "";
  }
  if (m0 == "mtc1") {
    const int gpr = ParseGprNumber(ops0[0]);
    const int xfer_fpr = ParseFprNumber(ops0[1]);
    if (gpr < 0 || xfer_fpr != dst_fpr) {
      return "";
    }
    return absl::StrFormat("(set %s (float:%s %s))", FormatFprRtl(dst_fpr, target_mode),
                           target_mode, FormatGprRtl(gpr, "SI"));
  }
  if (m0 == "l.s" || m0 == "lwc1") {
    const int load_fpr = ParseFprNumber(ops0[0]);
    const std::string addr_rtx = FormatMemAddressRtl(ops0[1], "SI");
    if (load_fpr != dst_fpr || addr_rtx.empty()) {
      return "";
    }
    return absl::StrFormat("(set %s (float:%s (mem:SI %s)))", FormatFprRtl(dst_fpr, target_mode),
                           target_mode, addr_rtx);
  }
  return "";
}

bool ParseIntOffsetAndBaseReg(std::string_view mem_arg, int* out_base_reg, int32_t* out_offset) {
  mem_arg = absl::StripAsciiWhitespace(mem_arg);
  const size_t lparen = mem_arg.find('(');
  if (lparen == std::string_view::npos || !absl::EndsWith(mem_arg, ")")) {
    return false;
  }
  const std::string_view prefix = absl::StripAsciiWhitespace(mem_arg.substr(0, lparen));
  const std::string_view base_str =
      absl::StripAsciiWhitespace(mem_arg.substr(lparen + 1, mem_arg.size() - lparen - 2));
  const int base_reg = ParseGprNumber(base_str);
  if (base_reg < 0) {
    return false;
  }
  int32_t off = 0;
  if (!prefix.empty()) {
    const auto imm = ParseIntImmediate(prefix);
    if (!imm.has_value()) {
      return false;
    }
    off = static_cast<int32_t>(*imm);
  }
  *out_base_reg = base_reg;
  *out_offset = off;
  return true;
}

std::string InvertUnalignedBlockMove(std::string_view multi_line_asm) {
  std::vector<std::string_view> lines;
  for (std::string_view raw : absl::StrSplit(multi_line_asm, '\n')) {
    const size_t hash_pos = raw.find('#');
    if (hash_pos != std::string_view::npos) {
      raw = raw.substr(0, hash_pos);
    }
    raw = absl::StripAsciiWhitespace(raw);
    if (!raw.empty()) {
      lines.push_back(raw);
    }
  }
  if (lines.size() < 3) {
    return "";
  }

  auto parse_mem_insn = [](std::string_view expected_mnem, std::string_view line, int* out_reg,
                           int* out_base, int32_t* out_off) -> bool {
    size_t sp = 0;
    while (sp < line.size() && !absl::ascii_isspace(static_cast<unsigned char>(line[sp]))) {
      ++sp;
    }
    if (line.substr(0, sp) != expected_mnem) {
      return false;
    }
    const std::vector<std::string_view> ops = SplitOperands(line.substr(sp));
    if (ops.size() != 2) {
      return false;
    }
    const int reg = ParseGprNumber(ops[0]);
    if (reg < 0 || !ParseIntOffsetAndBaseReg(ops[1], out_base, out_off)) {
      return false;
    }
    *out_reg = reg;
    return true;
  };

  size_t idx = 0;
  int src_base = -1;
  int32_t src_off0 = 0;
  std::vector<int> word_regs;
  while (idx + 1 < lines.size() && word_regs.size() < 4) {
    int rl = -1;
    int bl = -1;
    int rr = -1;
    int br = -1;
    int32_t off_l = 0;
    int32_t off_r = 0;
    if (!parse_mem_insn("lwl", lines[idx], &rl, &bl, &off_l)) {
      break;
    }
    if (!parse_mem_insn("lwr", lines[idx + 1], &rr, &br, &off_r)) {
      return "";
    }
    if (word_regs.empty()) {
      src_base = bl;
      src_off0 = off_l;
    }
    const int32_t expected_off = src_off0 + 4 * static_cast<int32_t>(word_regs.size());
    if (rl != rr || bl != src_base || br != src_base || off_l != expected_off ||
        off_r != expected_off + 3) {
      return "";
    }
    word_regs.push_back(rl);
    idx += 2;
  }
  if (word_regs.empty()) {
    return "";
  }

  std::vector<int> byte_regs;
  while (idx < lines.size() && word_regs.size() + byte_regs.size() < 4) {
    int rb = -1;
    int bb = -1;
    int32_t off_b = 0;
    if (!parse_mem_insn("lb", lines[idx], &rb, &bb, &off_b)) {
      break;
    }
    const int32_t expected_off = src_off0 + 4 * static_cast<int32_t>(word_regs.size()) +
                                 static_cast<int32_t>(byte_regs.size());
    if (bb != src_base || off_b != expected_off) {
      return "";
    }
    byte_regs.push_back(rb);
    ++idx;
  }

  int dst_base = -1;
  int32_t dst_off0 = 0;
  int move_type = 0;  // 0 = BLOCK_MOVE_NORMAL, 1 = BLOCK_MOVE_NOT_LAST
  for (size_t i = 0; i < word_regs.size(); ++i) {
    if (idx >= lines.size()) {
      return "";
    }
    int r_swl = -1;
    int b_swl = -1;
    int32_t off_swl = 0;
    if (!parse_mem_insn("swl", lines[idx], &r_swl, &b_swl, &off_swl)) {
      return "";
    }
    if (i == 0) {
      dst_base = b_swl;
      dst_off0 = off_swl;
    }
    const int32_t expected_off = dst_off0 + 4 * static_cast<int32_t>(i);
    if (r_swl != word_regs[i] || b_swl != dst_base || off_swl != expected_off) {
      return "";
    }
    ++idx;
    if (i + 1 == word_regs.size() && byte_regs.empty() && idx == lines.size()) {
      move_type = 1;
      break;
    }
    if (idx >= lines.size()) {
      return "";
    }
    int r_swr = -1;
    int b_swr = -1;
    int32_t off_swr = 0;
    if (!parse_mem_insn("swr", lines[idx], &r_swr, &b_swr, &off_swr)) {
      return "";
    }
    if (r_swr != word_regs[i] || b_swr != dst_base || off_swr != expected_off + 3) {
      return "";
    }
    ++idx;
  }

  for (size_t j = 0; j < byte_regs.size(); ++j) {
    if (j + 1 == byte_regs.size() && idx == lines.size()) {
      move_type = 1;
      break;
    }
    if (idx >= lines.size()) {
      return "";
    }
    int r_sb = -1;
    int b_sb = -1;
    int32_t off_sb = 0;
    if (!parse_mem_insn("sb", lines[idx], &r_sb, &b_sb, &off_sb)) {
      return "";
    }
    const int32_t expected_off =
        dst_off0 + 4 * static_cast<int32_t>(word_regs.size()) + static_cast<int32_t>(j);
    if (r_sb != byte_regs[j] || b_sb != dst_base || off_sb != expected_off) {
      return "";
    }
    ++idx;
  }
  if (idx != lines.size()) {
    return "";
  }

  std::vector<int> clobber_regs = word_regs;
  clobber_regs.insert(clobber_regs.end(), byte_regs.begin(), byte_regs.end());
  for (int r : clobber_regs) {
    if (r == src_base || r == dst_base) {
      return "";
    }
  }
  while (clobber_regs.size() < 4) {
    clobber_regs.push_back(src_base);
  }

  const std::string dst_addr =
      (dst_off0 == 0)
          ? FormatGprRtl(dst_base, "SI")
          : absl::StrFormat("(plus:SI %s (const_int %d))", FormatGprRtl(dst_base, "SI"), dst_off0);
  const std::string src_addr =
      (src_off0 == 0)
          ? FormatGprRtl(src_base, "SI")
          : absl::StrFormat("(plus:SI %s (const_int %d))", FormatGprRtl(src_base, "SI"), src_off0);
  const int32_t total_bytes = static_cast<int32_t>(4 * word_regs.size() + byte_regs.size());
  return absl::StrFormat(
      "(parallel[ (set (mem:BLK %s) (mem:BLK %s)) "
      "(clobber %s) (clobber %s) (clobber %s) (clobber %s) "
      "(use (const_int %d)) (use (const_int 1)) (use (const_int %d)) ] )",
      dst_addr, src_addr, FormatGprRtl(clobber_regs[0], "SI"), FormatGprRtl(clobber_regs[1], "SI"),
      FormatGprRtl(clobber_regs[2], "SI"), FormatGprRtl(clobber_regs[3], "SI"), total_bytes,
      move_type);
}

}  // namespace

std::string InvertFinalScanInsn(std::string_view asm_line_x) {
  std::string_view trimmed_full = absl::StripAsciiWhitespace(asm_line_x);
  if (trimmed_full.empty()) {
    return "";
  }

  // Check for multi-line instruction outputs (`floatsisf2`/`floatsidf2` or `movstrsi`).
  if (const size_t nl_pos = trimmed_full.find('\n'); nl_pos != std::string_view::npos) {
    const std::string_view first_line = trimmed_full.substr(0, nl_pos);
    const std::string_view second_line = trimmed_full.substr(nl_pos + 1);
    if (second_line.find('\n') == std::string_view::npos) {
      if (std::string float_si = InvertTwoLineFloatSi(first_line, second_line); !float_si.empty()) {
        return float_si;
      }
    }
    return InvertUnalignedBlockMove(trimmed_full);
  }

  std::string_view line = trimmed_full;
  const size_t comment_pos = line.find('#');
  if (comment_pos != std::string_view::npos) {
    line = absl::StripAsciiWhitespace(line.substr(0, comment_pos));
  }
  if (line.empty() || line.front() == '.' || line.back() == ':') {
    return "";
  }

  bool is_volatile_mem = false;
  if (absl::StartsWith(line, "%{") && absl::EndsWith(line, "%}")) {
    line = absl::StripAsciiWhitespace(line.substr(2, line.size() - 4));
    is_volatile_mem = true;
  }

  if (line == "nop") {
    return "(const_int 0)";
  }

  size_t sp = 0;
  while (sp < line.size() && !absl::ascii_isspace(static_cast<unsigned char>(line[sp]))) {
    ++sp;
  }
  const std::string_view mnemonic = line.substr(0, sp);
  const std::string_view args_str = absl::StripAsciiWhitespace(line.substr(sp));
  const std::vector<std::string_view> ops = SplitOperands(args_str);
  if (ops.empty()) {
    return "";
  }
  for (std::string_view op : ops) {
    if (op.empty()) {
      return "";
    }
  }

  const std::string_view mem_tag = is_volatile_mem ? "mem/v" : "mem";

  // ---------------------------------------------------------------------------
  // 1. Unconditional jumps, indirect jumps, and returns (`j`, `jr`, `b`)
  // ---------------------------------------------------------------------------
  if (mnemonic == "j" || mnemonic == "jr" || mnemonic == "b") {
    if (ops.size() != 1) {
      return "";
    }
    const int reg = ParseGprNumber(ops[0]);
    if (reg >= 0) {
      if (mnemonic == "b") {
        return "";
      }
      if (reg == 31) {
        return "(return)";
      }
      return absl::StrFormat("(set (pc) %s)", FormatGprRtl(reg, "SI"));
    }
    if (mnemonic == "jr") {
      return "";
    }
    std::string lbl_tok;
    if (IsLocalLabelName(ops[0], &lbl_tok)) {
      return absl::StrFormat("(set (pc) (label_ref %s))", lbl_tok);
    }
    return "";
  }

  // ---------------------------------------------------------------------------
  // 2. Calls (`jal`, `jalr`)
  // ---------------------------------------------------------------------------
  if (mnemonic == "jal" || mnemonic == "jalr") {
    std::string_view target_op;
    if (ops.size() == 1) {
      target_op = ops[0];
    } else if (ops.size() == 2) {
      if (ParseGprNumber(ops[0]) != 31) {
        return "";
      }
      target_op = ops[1];
    } else {
      return "";
    }
    const int reg = ParseGprNumber(target_op);
    if (reg >= 0) {
      return absl::StrFormat(
          "(parallel[ (call (mem:SI %s) (const_int 0)) (clobber (reg:SI 31 ra)) ] )",
          FormatGprRtl(reg, "SI"));
    }
    if (mnemonic == "jal" && IsValidSymbolOrLabelIdentifier(target_op)) {
      return absl::StrFormat(
          "(parallel[ (call (mem:SI (symbol_ref:SI (\"%s\"))) (const_int 0)) "
          "(clobber (reg:SI 31 ra)) ] )",
          target_op);
    }
    return "";
  }

  // ---------------------------------------------------------------------------
  // 3. Conditional integer branches (`beq`, `bne`, `beql`, `bnel`, `b*z`, `b*zl`)
  // ---------------------------------------------------------------------------
  if (mnemonic == "beq" || mnemonic == "bne" || mnemonic == "beql" || mnemonic == "bnel" ||
      mnemonic == "beqz" || mnemonic == "bnez" || mnemonic == "beqzl" || mnemonic == "bnezl") {
    const bool likely = absl::EndsWith(mnemonic, "l");
    const bool zero_form = absl::StartsWith(mnemonic, "beqz") || absl::StartsWith(mnemonic, "bnez");
    const std::string_view cmp_code = absl::StartsWith(mnemonic, "beq") ? "eq" : "ne";
    int rs = -1;
    int rt = 0;
    std::string lbl_tok;
    if (zero_form) {
      if (ops.size() != 2) {
        return "";
      }
      rs = ParseGprNumber(ops[0]);
      if (!IsLocalLabelName(ops[1], &lbl_tok)) {
        return "";
      }
    } else {
      if (ops.size() != 3) {
        return "";
      }
      rs = ParseGprNumber(ops[0]);
      rt = ParseGprNumber(ops[1]);
      if (rt < 0) {
        if (const auto imm = ParseIntImmediate(ops[1]); imm.has_value() && *imm == 0) {
          rt = 0;
        }
      }
      if (!IsLocalLabelName(ops[2], &lbl_tok)) {
        return "";
      }
    }
    if (rs < 0 || rt < 0) {
      return "";
    }
    const std::string rhs = (rt == 0) ? "(const_int 0)" : FormatGprRtl(rt, "SI");
    const std::string core =
        absl::StrFormat("(set (pc) (if_then_else (%s:SI %s %s) (label_ref %s) (pc)))", cmp_code,
                        FormatGprRtl(rs, "SI"), rhs, lbl_tok);
    return likely ? absl::StrFormat("(jump_insn/u %s)", core) : core;
  }

  if (mnemonic == "bgtz" || mnemonic == "blez" || mnemonic == "bltz" || mnemonic == "bgez" ||
      mnemonic == "bgtzl" || mnemonic == "blezl" || mnemonic == "bltzl" || mnemonic == "bgezl") {
    if (ops.size() != 2) {
      return "";
    }
    const bool likely = absl::EndsWith(mnemonic, "l");
    const int rs = ParseGprNumber(ops[0]);
    std::string lbl_tok;
    if (rs < 0 || !IsLocalLabelName(ops[1], &lbl_tok)) {
      return "";
    }
    const std::string_view cmp_code =
        absl::StartsWith(mnemonic, "bgtz")
            ? "gt"
            : (absl::StartsWith(mnemonic, "blez")
                   ? "le"
                   : (absl::StartsWith(mnemonic, "bltz") ? "lt" : "ge"));
    const std::string core =
        absl::StrFormat("(set (pc) (if_then_else (%s:SI %s (const_int 0)) (label_ref %s) (pc)))",
                        cmp_code, FormatGprRtl(rs, "SI"), lbl_tok);
    return likely ? absl::StrFormat("(jump_insn/u %s)", core) : core;
  }

  // ---------------------------------------------------------------------------
  // 4. Floating-point branches (`bc1t`, `bc1f`, `bc1tl`, `bc1fl`)
  // ---------------------------------------------------------------------------
  if (mnemonic == "bc1t" || mnemonic == "bc1f" || mnemonic == "bc1tl" || mnemonic == "bc1fl") {
    if (ops.size() != 1) {
      return "";
    }
    std::string lbl_tok;
    if (!IsLocalLabelName(ops[0], &lbl_tok)) {
      return "";
    }
    const bool likely = absl::EndsWith(mnemonic, "l");
    const bool is_true = absl::StartsWith(mnemonic, "bc1t");
    const std::string_view cmp_code = is_true ? "ne" : "eq";
    const std::string core = absl::StrFormat(
        "(set (pc) (if_then_else (%s:CC_FP (reg:CC_FP 67 $fcr31) (const_int 0)) "
        "(label_ref %s) (pc)))",
        cmp_code, lbl_tok);
    return likely ? absl::StrFormat("(jump_insn/u %s)", core) : core;
  }

  // ---------------------------------------------------------------------------
  // 5. Moves, constant loads, and address loads (`move`, `li`, `dli`, `lui`, `la`, `dla`)
  // ---------------------------------------------------------------------------
  if (mnemonic == "move") {
    if (ops.size() != 2) {
      return "";
    }
    const int rd = ParseGprNumber(ops[0]);
    const int rs = ParseGprNumber(ops[1]);
    if (rd < 0 || rs < 0) {
      return "";
    }
    if (rs == 0) {
      return absl::StrFormat("(set %s (const_int 0))", FormatGprRtl(rd, "SI"));
    }
    return absl::StrFormat("(set %s %s)", FormatGprRtl(rd, "SI"), FormatGprRtl(rs, "SI"));
  }

  if (mnemonic == "li" || mnemonic == "dli") {
    if (ops.size() != 2) {
      return "";
    }
    const std::string_view mode = (mnemonic == "dli") ? "DI" : "SI";
    const int rd = ParseGprNumber(ops[0]);
    const auto imm = ParseIntImmediate(ops[1]);
    if (rd < 0 || !imm.has_value()) {
      return "";
    }
    return absl::StrFormat("(set %s (const_int %d))", FormatGprRtl(rd, mode),
                           static_cast<int32_t>(*imm));
  }

  if (mnemonic == "lui") {
    if (ops.size() != 2) {
      return "";
    }
    const int rd = ParseGprNumber(ops[0]);
    if (rd < 0) {
      return "";
    }
    if (absl::StartsWith(ops[1], "%hi(") && absl::EndsWith(ops[1], ")")) {
      const std::string hi_rtx = FormatSymbolicAddressExpr(ops[1], "SI");
      if (hi_rtx.empty()) {
        return "";
      }
      return absl::StrFormat("(set %s %s)", FormatGprRtl(rd, "SI"), hi_rtx);
    }
    const auto imm = ParseIntImmediate(ops[1]);
    if (!imm.has_value()) {
      return "";
    }
    const int32_t shifted = static_cast<int32_t>(static_cast<uint32_t>(*imm) << 16);
    return absl::StrFormat("(set %s (const_int %d))", FormatGprRtl(rd, "SI"), shifted);
  }

  if (mnemonic == "la" || mnemonic == "dla") {
    if (ops.size() != 2) {
      return "";
    }
    const std::string_view mode = (mnemonic == "dla") ? "DI" : "SI";
    const int rd = ParseGprNumber(ops[0]);
    const std::string addr_rtx = FormatMemAddressRtl(ops[1], mode);
    if (rd < 0 || addr_rtx.empty()) {
      return "";
    }
    return absl::StrFormat("(set %s %s)", FormatGprRtl(rd, mode), addr_rtx);
  }

  // ---------------------------------------------------------------------------
  // 6. Integer ALU & comparisons
  // ---------------------------------------------------------------------------
  if (mnemonic == "addu" || mnemonic == "addiu" || mnemonic == "add" || mnemonic == "addi" ||
      mnemonic == "daddu" || mnemonic == "daddiu" || mnemonic == "dadd" || mnemonic == "daddi") {
    if (ops.size() != 3) {
      return "";
    }
    const std::string_view mode = absl::StartsWith(mnemonic, "d") ? "DI" : "SI";
    const int rd = ParseGprNumber(ops[0]);
    const int rs = ParseGprNumber(ops[1]);
    if (rd < 0 || rs < 0) {
      return "";
    }
    if (rs == 0) {
      if (const auto imm = ParseIntImmediate(ops[2]); imm.has_value()) {
        return absl::StrFormat("(set %s (const_int %d))", FormatGprRtl(rd, mode),
                               static_cast<int32_t>(*imm));
      }
    }
    const int rt = ParseGprNumber(ops[2]);
    if (rt >= 0) {
      if (rt == 0 && (mnemonic == "addiu" || mnemonic == "addi" || mnemonic == "daddiu" ||
                      mnemonic == "daddi")) {
        return absl::StrFormat("(set %s (plus:%s %s (const_int 0)))", FormatGprRtl(rd, mode), mode,
                               FormatGprRtl(rs, mode));
      }
      return absl::StrFormat("(set %s (plus:%s %s %s))", FormatGprRtl(rd, mode), mode,
                             FormatGprRtl(rs, mode), FormatGprRtl(rt, mode));
    }
    if (const auto imm = ParseIntImmediate(ops[2]); imm.has_value()) {
      return absl::StrFormat("(set %s (plus:%s %s (const_int %d)))", FormatGprRtl(rd, mode), mode,
                             FormatGprRtl(rs, mode), static_cast<int32_t>(*imm));
    }
    if (absl::StartsWith(ops[2], "%lo(") && absl::EndsWith(ops[2], ")")) {
      const std::string sym_rtx =
          FormatSymbolicAddressExpr(ops[2].substr(4, ops[2].size() - 5), mode);
      if (sym_rtx.empty()) {
        return "";
      }
      return absl::StrFormat("(set %s (lo_sum:%s %s %s))", FormatGprRtl(rd, mode), mode,
                             FormatGprRtl(rs, mode), sym_rtx);
    }
    const std::string sym_rtx = FormatSymbolicAddressExpr(ops[2], mode);
    if (!sym_rtx.empty()) {
      return absl::StrFormat("(set %s (plus:%s %s %s))", FormatGprRtl(rd, mode), mode,
                             FormatGprRtl(rs, mode), sym_rtx);
    }
    return "";
  }

  if (mnemonic == "subu" || mnemonic == "sub" || mnemonic == "dsubu" || mnemonic == "dsub") {
    if (ops.size() != 3) {
      return "";
    }
    const std::string_view mode = absl::StartsWith(mnemonic, "d") ? "DI" : "SI";
    const int rd = ParseGprNumber(ops[0]);
    const int rs = ParseGprNumber(ops[1]);
    if (rd < 0 || rs < 0) {
      return "";
    }
    const int rt = ParseGprNumber(ops[2]);
    if (rt >= 0) {
      if (rs == 0) {
        return absl::StrFormat("(set %s (neg:%s %s))", FormatGprRtl(rd, mode), mode,
                               FormatGprRtl(rt, mode));
      }
      return absl::StrFormat("(set %s (minus:%s %s %s))", FormatGprRtl(rd, mode), mode,
                             FormatGprRtl(rs, mode), FormatGprRtl(rt, mode));
    }
    if (const auto imm = ParseIntImmediate(ops[2]); imm.has_value()) {
      return absl::StrFormat("(set %s (minus:%s %s (const_int %d)))", FormatGprRtl(rd, mode), mode,
                             FormatGprRtl(rs, mode), static_cast<int32_t>(*imm));
    }
    return "";
  }

  if (mnemonic == "negu" || mnemonic == "neg" || mnemonic == "dnegu" || mnemonic == "dneg") {
    if (ops.size() != 2) {
      return "";
    }
    const std::string_view mode = absl::StartsWith(mnemonic, "d") ? "DI" : "SI";
    const int rd = ParseGprNumber(ops[0]);
    const int rs = ParseGprNumber(ops[1]);
    if (rd < 0 || rs < 0) {
      return "";
    }
    return absl::StrFormat("(set %s (neg:%s %s))", FormatGprRtl(rd, mode), mode,
                           FormatGprRtl(rs, mode));
  }

  if (mnemonic == "and" || mnemonic == "andi" || mnemonic == "or" || mnemonic == "ori" ||
      mnemonic == "xor" || mnemonic == "xori") {
    if (ops.size() != 3) {
      return "";
    }
    const int rd = ParseGprNumber(ops[0]);
    const int rs = ParseGprNumber(ops[1]);
    if (rd < 0 || rs < 0) {
      return "";
    }
    const std::string_view rtl_op = absl::StartsWith(mnemonic, "and")
                                        ? "and"
                                        : (absl::StartsWith(mnemonic, "or") ? "ior" : "xor");
    const int rt = ParseGprNumber(ops[2]);
    if (rt >= 0) {
      return absl::StrFormat("(set %s (%s:SI %s %s))", FormatGprRtl(rd, "SI"), rtl_op,
                             FormatGprRtl(rs, "SI"), FormatGprRtl(rt, "SI"));
    }
    if (const auto imm = ParseIntImmediate(ops[2]); imm.has_value()) {
      return absl::StrFormat("(set %s (%s:SI %s (const_int %d)))", FormatGprRtl(rd, "SI"), rtl_op,
                             FormatGprRtl(rs, "SI"), static_cast<int32_t>(*imm));
    }
    return "";
  }

  if (mnemonic == "nor") {
    if (ops.size() != 3) {
      return "";
    }
    const int rd = ParseGprNumber(ops[0]);
    const int rs = ParseGprNumber(ops[1]);
    const int rt = ParseGprNumber(ops[2]);
    if (rd < 0 || rs < 0 || rt < 0) {
      return "";
    }
    if (rs == 0 && rt != 0) {
      return absl::StrFormat("(set %s (not:SI %s))", FormatGprRtl(rd, "SI"),
                             FormatGprRtl(rt, "SI"));
    }
    if (rt == 0 && rs != 0) {
      return absl::StrFormat("(set %s (not:SI %s))", FormatGprRtl(rd, "SI"),
                             FormatGprRtl(rs, "SI"));
    }
    return absl::StrFormat("(set %s (not:SI (ior:SI %s %s)))", FormatGprRtl(rd, "SI"),
                           FormatGprRtl(rs, "SI"), FormatGprRtl(rt, "SI"));
  }

  if (mnemonic == "not") {
    if (ops.size() != 2) {
      return "";
    }
    const int rd = ParseGprNumber(ops[0]);
    const int rs = ParseGprNumber(ops[1]);
    if (rd < 0 || rs < 0) {
      return "";
    }
    return absl::StrFormat("(set %s (not:SI %s))", FormatGprRtl(rd, "SI"), FormatGprRtl(rs, "SI"));
  }

  if (mnemonic == "slt" || mnemonic == "slti" || mnemonic == "sltu" || mnemonic == "sltiu") {
    if (ops.size() != 3) {
      return "";
    }
    const int rd = ParseGprNumber(ops[0]);
    const int rs = ParseGprNumber(ops[1]);
    if (rd < 0 || rs < 0) {
      return "";
    }
    const bool is_unsigned = absl::StartsWith(mnemonic, "sltu") || mnemonic == "sltiu";
    const int rt = ParseGprNumber(ops[2]);
    if (rt >= 0) {
      if (rs == 0) {
        const std::string_view zero_cmp = is_unsigned ? "ne" : "gt";
        return absl::StrFormat("(set %s (%s:SI %s (const_int 0)))", FormatGprRtl(rd, "SI"),
                               zero_cmp, FormatGprRtl(rt, "SI"));
      }
      const std::string_view rtl_op = is_unsigned ? "ltu" : "lt";
      return absl::StrFormat("(set %s (%s:SI %s %s))", FormatGprRtl(rd, "SI"), rtl_op,
                             FormatGprRtl(rs, "SI"), FormatGprRtl(rt, "SI"));
    }
    if (const auto imm = ParseIntImmediate(ops[2]); imm.has_value()) {
      if (is_unsigned && *imm == 1) {
        return absl::StrFormat("(set %s (eq:SI %s (const_int 0)))", FormatGprRtl(rd, "SI"),
                               FormatGprRtl(rs, "SI"));
      }
      const std::string_view rtl_op = is_unsigned ? "ltu" : "lt";
      return absl::StrFormat("(set %s (%s:SI %s (const_int %d)))", FormatGprRtl(rd, "SI"), rtl_op,
                             FormatGprRtl(rs, "SI"), static_cast<int32_t>(*imm));
    }
    return "";
  }

  // ---------------------------------------------------------------------------
  // 7. Shifts (`sll`, `sllv`, `sra`, `srav`, `srl`, `srlv`, `dsll`, `dsra`, `dsrl`)
  // ---------------------------------------------------------------------------
  if (mnemonic == "sll" || mnemonic == "sllv" || mnemonic == "sra" || mnemonic == "srav" ||
      mnemonic == "srl" || mnemonic == "srlv" || mnemonic == "dsll" || mnemonic == "dsllv" ||
      mnemonic == "dsra" || mnemonic == "dsrav" || mnemonic == "dsrl" || mnemonic == "dsrlv") {
    if (ops.size() != 3) {
      return "";
    }
    const bool is_64 = absl::StartsWith(mnemonic, "d");
    const std::string_view mode = is_64 ? "DI" : "SI";
    const std::string_view base_mnem = is_64 ? mnemonic.substr(1) : mnemonic;
    const std::string_view rtl_op =
        absl::StartsWith(base_mnem, "sll")
            ? "ashift"
            : (absl::StartsWith(base_mnem, "sra") ? "ashiftrt" : "lshiftrt");
    const int rd = ParseGprNumber(ops[0]);
    const int rt = ParseGprNumber(ops[1]);
    if (rd < 0 || rt < 0) {
      return "";
    }
    const int rs = ParseGprNumber(ops[2]);
    if (rs >= 0) {
      return absl::StrFormat("(set %s (%s:%s %s %s))", FormatGprRtl(rd, mode), rtl_op, mode,
                             FormatGprRtl(rt, mode), FormatGprRtl(rs, "SI"));
    }
    if (const auto sa = ParseIntImmediate(ops[2]); sa.has_value()) {
      return absl::StrFormat("(set %s (%s:%s %s (const_int %d)))", FormatGprRtl(rd, mode), rtl_op,
                             mode, FormatGprRtl(rt, mode), static_cast<int32_t>(*sa));
    }
    return "";
  }

  // ---------------------------------------------------------------------------
  // 8. Multiply, divide, and HI/LO transfers (`mult`, `multu`, `div`, `divu`,
  //    `rem`, `remu`, `mflo`, `mfhi`, `mtlo`, `mthi`)
  // ---------------------------------------------------------------------------
  if (mnemonic == "mult" || mnemonic == "multu" || mnemonic == "dmult" || mnemonic == "dmultu") {
    if (ops.size() != 2) {
      return "";
    }
    const int rs = ParseGprNumber(ops[0]);
    const int rt = ParseGprNumber(ops[1]);
    if (rs < 0 || rt < 0) {
      return "";
    }
    if (mnemonic == "mult" || mnemonic == "multu") {
      const std::string_view ext_op = (mnemonic == "multu") ? "zero_extend" : "sign_extend";
      return absl::StrFormat(
          "(parallel[ (set (reg:SI 64 hi) (truncate:SI (lshiftrt:DI "
          "(mult:DI (%s:DI %s) (%s:DI %s)) (const_int 32)))) "
          "(clobber (reg:SI 65 lo)) (clobber (reg:SI 66 accum)) ] )",
          ext_op, FormatGprRtl(rs, "SI"), ext_op, FormatGprRtl(rt, "SI"));
    }
    const std::string_view ext_op = (mnemonic == "dmultu") ? "zero_extend" : "sign_extend";
    return absl::StrFormat("(set (reg:TI 64 hi) (mult:TI (%s:TI %s) (%s:TI %s)))", ext_op,
                           FormatGprRtl(rs, "DI"), ext_op, FormatGprRtl(rt, "DI"));
  }

  if (mnemonic == "div" || mnemonic == "divu" || mnemonic == "rem" || mnemonic == "remu" ||
      mnemonic == "ddiv" || mnemonic == "ddivu" || mnemonic == "drem" || mnemonic == "dremu") {
    const bool is_64 = absl::StartsWith(mnemonic, "d") && mnemonic != "div" && mnemonic != "divu";
    const std::string_view mode = is_64 ? "DI" : "SI";
    const std::string_view base_mnem = is_64 ? mnemonic.substr(1) : mnemonic;
    if (ops.size() == 2 && (base_mnem == "div" || base_mnem == "divu")) {
      const int rs = ParseGprNumber(ops[0]);
      const int rt = ParseGprNumber(ops[1]);
      if (rs < 0 || rt < 0) {
        return "";
      }
      if (rs == 0) {
        return "";
      }
      const std::string_view div_op = (base_mnem == "divu") ? "udiv" : "div";
      const std::string_view mod_op = (base_mnem == "divu") ? "umod" : "mod";
      return absl::StrFormat(
          "(parallel[ (set (reg:%s 65 lo) (%s:%s %s %s)) "
          "(set (reg:%s 64 hi) (%s:%s %s %s)) (clobber (reg:%s 66 accum)) ] )",
          mode, div_op, mode, FormatGprRtl(rs, mode), FormatGprRtl(rt, mode), mode, mod_op, mode,
          FormatGprRtl(rs, mode), FormatGprRtl(rt, mode), mode);
    }
    if (ops.size() == 3) {
      const int rd = ParseGprNumber(ops[0]);
      const int rs = ParseGprNumber(ops[1]);
      const int rt = ParseGprNumber(ops[2]);
      if (rd < 0 || rs < 0 || rt < 0) {
        return "";
      }
      const std::string_view rtl_op =
          base_mnem == "div"
              ? "div"
              : (base_mnem == "divu" ? "udiv" : (base_mnem == "rem" ? "mod" : "umod"));
      return absl::StrFormat(
          "(parallel[ (set %s (%s:%s %s %s)) "
          "(clobber (reg:%s 65 lo)) (clobber (reg:%s 64 hi)) (clobber (reg:%s 66 accum)) ] )",
          FormatGprRtl(rd, mode), rtl_op, mode, FormatGprRtl(rs, mode), FormatGprRtl(rt, mode),
          mode, mode, mode);
    }
    return "";
  }

  if (mnemonic == "mflo" || mnemonic == "mfhi") {
    if (ops.size() != 1) {
      return "";
    }
    const int rd = ParseGprNumber(ops[0]);
    if (rd < 0) {
      return "";
    }
    const std::string_view hilo = (mnemonic == "mflo") ? "(reg:SI 65 lo)" : "(reg:SI 64 hi)";
    return absl::StrFormat("(set %s %s)", FormatGprRtl(rd, "SI"), hilo);
  }

  if (mnemonic == "mtlo" || mnemonic == "mthi") {
    if (ops.size() != 1) {
      return "";
    }
    const int rs = ParseGprNumber(ops[0]);
    if (rs < 0) {
      return "";
    }
    const std::string_view hilo = (mnemonic == "mtlo") ? "(reg:SI 65 lo)" : "(reg:SI 64 hi)";
    if (rs == 0) {
      return absl::StrFormat("(set %s (const_int 0))", hilo);
    }
    return absl::StrFormat("(set %s %s)", hilo, FormatGprRtl(rs, "SI"));
  }

  // ---------------------------------------------------------------------------
  // 9. Integer loads & stores (`lw`, `sw`, `lh`, `lhu`, `sh`, `lb`, `lbu`,
  //    `sb`, `lwu`, `ld`, `sd`, `ulw`, `usw`)
  // ---------------------------------------------------------------------------
  if (mnemonic == "lw" || mnemonic == "lh" || mnemonic == "lhu" || mnemonic == "lb" ||
      mnemonic == "lbu" || mnemonic == "lwu" || mnemonic == "ld") {
    if (ops.size() != 2) {
      return "";
    }
    const int rt = ParseGprNumber(ops[0]);
    const std::string addr_rtx = FormatMemAddressRtl(ops[1], "SI");
    if (rt < 0 || addr_rtx.empty()) {
      return "";
    }
    if (mnemonic == "lw") {
      return absl::StrFormat("(set %s (%s:SI %s))", FormatGprRtl(rt, "SI"), mem_tag, addr_rtx);
    }
    if (mnemonic == "ld") {
      return absl::StrFormat("(set %s (%s:DI %s))", FormatGprRtl(rt, "DI"), mem_tag, addr_rtx);
    }
    if (mnemonic == "lwu") {
      return absl::StrFormat("(set %s (zero_extend:DI (%s:SI %s)))", FormatGprRtl(rt, "DI"),
                             mem_tag, addr_rtx);
    }
    const std::string_view mem_mode = (mnemonic == "lh" || mnemonic == "lhu") ? "HI" : "QI";
    const std::string_view ext_op =
        (mnemonic == "lhu" || mnemonic == "lbu") ? "zero_extend" : "sign_extend";
    return absl::StrFormat("(set %s (%s:SI (%s:%s %s)))", FormatGprRtl(rt, "SI"), ext_op, mem_tag,
                           mem_mode, addr_rtx);
  }

  if (mnemonic == "sw" || mnemonic == "sh" || mnemonic == "sb" || mnemonic == "sd") {
    if (ops.size() != 2) {
      return "";
    }
    const int rt = ParseGprNumber(ops[0]);
    const std::string addr_rtx = FormatMemAddressRtl(ops[1], "SI");
    if (rt < 0 || addr_rtx.empty()) {
      return "";
    }
    const std::string_view mode =
        mnemonic == "sw" ? "SI" : (mnemonic == "sh" ? "HI" : (mnemonic == "sb" ? "QI" : "DI"));
    const std::string src_rtx = (rt == 0) ? "(const_int 0)" : FormatGprRtl(rt, mode);
    return absl::StrFormat("(set (%s:%s %s) %s)", mem_tag, mode, addr_rtx, src_rtx);
  }

  if (mnemonic == "ulw") {
    if (ops.size() != 2) {
      return "";
    }
    const int rt = ParseGprNumber(ops[0]);
    const std::string addr_rtx = FormatMemAddressRtl(ops[1], "SI");
    if (rt < 0 || addr_rtx.empty()) {
      return "";
    }
    return absl::StrFormat("(set %s (unspec:SI [(%s:QI %s)] 0))", FormatGprRtl(rt, "SI"), mem_tag,
                           addr_rtx);
  }

  if (mnemonic == "usw") {
    if (ops.size() != 2) {
      return "";
    }
    const int rt = ParseGprNumber(ops[0]);
    const std::string addr_rtx = FormatMemAddressRtl(ops[1], "SI");
    if (rt < 0 || addr_rtx.empty()) {
      return "";
    }
    const std::string src_rtx = (rt == 0) ? "(const_int 0)" : FormatGprRtl(rt, "SI");
    return absl::StrFormat("(set (%s:QI %s) (unspec:QI [%s] 1))", mem_tag, addr_rtx, src_rtx);
  }

  if (mnemonic == "swr") {
    if (ops.size() != 2) {
      return "";
    }
    const int rt = ParseGprNumber(ops[0]);
    int base_reg = -1;
    int32_t off = 0;
    if (rt < 0 || !ParseIntOffsetAndBaseReg(ops[1], &base_reg, &off) || rt == base_reg) {
      return "";
    }
    const int32_t dst_off = off - 3;
    const std::string dst_addr =
        (dst_off == 0)
            ? FormatGprRtl(base_reg, "SI")
            : absl::StrFormat("(plus:SI %s (const_int %d))", FormatGprRtl(base_reg, "SI"), dst_off);
    const std::string base_rtx = FormatGprRtl(base_reg, "SI");
    return absl::StrFormat(
        "(parallel[ (set (%s:BLK %s) (%s:BLK %s)) "
        "(clobber %s) (clobber %s) (clobber %s) (clobber %s) "
        "(use (const_int 4)) (use (const_int 1)) (use (const_int 2)) ] )",
        mem_tag, dst_addr, mem_tag, base_rtx, FormatGprRtl(rt, "SI"), base_rtx, base_rtx, base_rtx);
  }

  // ---------------------------------------------------------------------------
  // 10. Floating-point loads, stores, moves, and transfers (`l.s`, `lwc1`,
  //     `s.s`, `swc1`, `l.d`, `ldc1`, `s.d`, `sdc1`, `li.s`, `li.d`, `mov.s`,
  //     `mov.d`, `mfc1`, `mtc1`, `dmfc1`, `dmtc1`, `cfc1`, `ctc1`)
  // ---------------------------------------------------------------------------
  if (mnemonic == "l.s" || mnemonic == "lwc1" || mnemonic == "l.d" || mnemonic == "ldc1") {
    if (ops.size() != 2) {
      return "";
    }
    const std::string_view mode = (mnemonic == "l.d" || mnemonic == "ldc1") ? "DF" : "SF";
    const int fpr = ParseFprNumber(ops[0]);
    const std::string addr_rtx = FormatMemAddressRtl(ops[1], "SI");
    if (fpr < 0 || addr_rtx.empty()) {
      return "";
    }
    return absl::StrFormat("(set %s (%s:%s %s))", FormatFprRtl(fpr, mode), mem_tag, mode, addr_rtx);
  }

  if (mnemonic == "s.s" || mnemonic == "swc1" || mnemonic == "s.d" || mnemonic == "sdc1") {
    if (ops.size() != 2) {
      return "";
    }
    const std::string_view mode = (mnemonic == "s.d" || mnemonic == "sdc1") ? "DF" : "SF";
    const int fpr = ParseFprNumber(ops[0]);
    const std::string addr_rtx = FormatMemAddressRtl(ops[1], "SI");
    if (fpr < 0 || addr_rtx.empty()) {
      return "";
    }
    return absl::StrFormat("(set (%s:%s %s) %s)", mem_tag, mode, addr_rtx, FormatFprRtl(fpr, mode));
  }

  if (mnemonic == "li.s" || mnemonic == "li.d") {
    if (ops.size() != 2) {
      return "";
    }
    const std::string_view mode = (mnemonic == "li.d") ? "DF" : "SF";
    const int fpr = ParseFprNumber(ops[0]);
    if (fpr < 0) {
      return "";
    }
    const std::string val_str(ops[1]);
    char* endptr = nullptr;
    const double d = std::strtod(val_str.c_str(), &endptr);
    if (endptr == val_str.c_str() || *endptr != '\0') {
      return "";
    }
    const uint64_t bits = absl::bit_cast<uint64_t>(d);
    const int32_t lo = static_cast<int32_t>(bits & 0xffffffffu);
    const int32_t hi = static_cast<int32_t>(bits >> 32);
    return absl::StrFormat("(set %s (const_double:%s (cc0) %d %d))", FormatFprRtl(fpr, mode), mode,
                           lo, hi);
  }

  if (mnemonic == "mov.s" || mnemonic == "mov.d") {
    if (ops.size() != 2) {
      return "";
    }
    const std::string_view mode = (mnemonic == "mov.d") ? "DF" : "SF";
    const int fd = ParseFprNumber(ops[0]);
    const int fs = ParseFprNumber(ops[1]);
    if (fd < 0 || fs < 0) {
      return "";
    }
    return absl::StrFormat("(set %s %s)", FormatFprRtl(fd, mode), FormatFprRtl(fs, mode));
  }

  if (mnemonic == "mfc1" || mnemonic == "dmfc1") {
    if (ops.size() != 2) {
      return "";
    }
    const std::string_view mode = (mnemonic == "dmfc1") ? "DF" : "SF";
    const int rt = ParseGprNumber(ops[0]);
    const int fs = ParseFprNumber(ops[1]);
    if (rt < 0 || fs < 0) {
      return "";
    }
    return absl::StrFormat("(set %s %s)", FormatGprRtl(rt, mode), FormatFprRtl(fs, mode));
  }

  if (mnemonic == "mtc1" || mnemonic == "dmtc1") {
    if (ops.size() != 2) {
      return "";
    }
    const std::string_view mode = (mnemonic == "dmtc1") ? "DF" : "SF";
    const int rt = ParseGprNumber(ops[0]);
    const int fs = ParseFprNumber(ops[1]);
    if (rt < 0 || fs < 0) {
      return "";
    }
    if (rt == 0 && mnemonic == "mtc1") {
      return absl::StrFormat("(set %s (const_double:SF (cc0) 0 0))", FormatFprRtl(fs, "SF"));
    }
    return absl::StrFormat("(set %s %s)", FormatFprRtl(fs, mode), FormatGprRtl(rt, mode));
  }

  if (mnemonic == "cfc1") {
    if (ops.size() != 2) {
      return "";
    }
    const int rt = ParseGprNumber(ops[0]);
    if (rt < 0 || (ops[1] != "$31" && ops[1] != "$fcr31")) {
      return "";
    }
    return absl::StrFormat("(set %s (reg:SI 67 $fcr31))", FormatGprRtl(rt, "SI"));
  }

  if (mnemonic == "ctc1") {
    if (ops.size() != 2) {
      return "";
    }
    const int rt = ParseGprNumber(ops[0]);
    if (rt < 0 || (ops[1] != "$31" && ops[1] != "$fcr31")) {
      return "";
    }
    return absl::StrFormat("(set (reg:SI 67 $fcr31) %s)", FormatGprRtl(rt, "SI"));
  }

  // ---------------------------------------------------------------------------
  // 11. Floating-point arithmetic, unary ops, conversions, and comparisons
  // ---------------------------------------------------------------------------
  if (mnemonic == "add.s" || mnemonic == "add.d" || mnemonic == "sub.s" || mnemonic == "sub.d" ||
      mnemonic == "mul.s" || mnemonic == "mul.d" || mnemonic == "div.s" || mnemonic == "div.d") {
    if (ops.size() != 3) {
      return "";
    }
    const std::string_view mode = absl::EndsWith(mnemonic, ".d") ? "DF" : "SF";
    const int fd = ParseFprNumber(ops[0]);
    const int fs = ParseFprNumber(ops[1]);
    const int ft = ParseFprNumber(ops[2]);
    if (fd < 0 || fs < 0 || ft < 0) {
      return "";
    }
    const std::string_view rtl_op =
        absl::StartsWith(mnemonic, "add")
            ? "plus"
            : (absl::StartsWith(mnemonic, "sub")
                   ? "minus"
                   : (absl::StartsWith(mnemonic, "mul") ? "mult" : "div"));
    return absl::StrFormat("(set %s (%s:%s %s %s))", FormatFprRtl(fd, mode), rtl_op, mode,
                           FormatFprRtl(fs, mode), FormatFprRtl(ft, mode));
  }

  if (mnemonic == "abs.s" || mnemonic == "abs.d" || mnemonic == "neg.s" || mnemonic == "neg.d" ||
      mnemonic == "sqrt.s" || mnemonic == "sqrt.d") {
    if (ops.size() != 2) {
      return "";
    }
    const std::string_view mode = absl::EndsWith(mnemonic, ".d") ? "DF" : "SF";
    const int fd = ParseFprNumber(ops[0]);
    const int fs = ParseFprNumber(ops[1]);
    if (fd < 0 || fs < 0) {
      return "";
    }
    const std::string_view rtl_op = absl::StartsWith(mnemonic, "abs")
                                        ? "abs"
                                        : (absl::StartsWith(mnemonic, "neg") ? "neg" : "sqrt");
    return absl::StrFormat("(set %s (%s:%s %s))", FormatFprRtl(fd, mode), rtl_op, mode,
                           FormatFprRtl(fs, mode));
  }

  if (mnemonic == "cvt.d.s") {
    if (ops.size() != 2) {
      return "";
    }
    const int fd = ParseFprNumber(ops[0]);
    const int fs = ParseFprNumber(ops[1]);
    if (fd < 0 || fs < 0) {
      return "";
    }
    return absl::StrFormat("(set %s (float_extend:DF %s))", FormatFprRtl(fd, "DF"),
                           FormatFprRtl(fs, "SF"));
  }

  if (mnemonic == "cvt.s.d") {
    if (ops.size() != 2) {
      return "";
    }
    const int fd = ParseFprNumber(ops[0]);
    const int fs = ParseFprNumber(ops[1]);
    if (fd < 0 || fs < 0) {
      return "";
    }
    return absl::StrFormat("(set %s (float_truncate:SF %s))", FormatFprRtl(fd, "SF"),
                           FormatFprRtl(fs, "DF"));
  }

  if (mnemonic == "cvt.s.w" || mnemonic == "cvt.d.w") {
    if (ops.size() != 2) {
      return "";
    }
    const std::string_view mode = (mnemonic == "cvt.s.w") ? "SF" : "DF";
    const int fd = ParseFprNumber(ops[0]);
    const int fs = ParseFprNumber(ops[1]);
    if (fd < 0 || fs < 0) {
      return "";
    }
    return absl::StrFormat("(set %s (float:%s %s))", FormatFprRtl(fd, mode), mode,
                           FormatGprRtl(2, "SI"));
  }

  if (mnemonic == "trunc.w.s" || mnemonic == "trunc.w.d") {
    if (ops.size() == 3) {
      const std::string_view src_mode = (mnemonic == "trunc.w.d") ? "DF" : "SF";
      const int fd = ParseFprNumber(ops[0]);
      const int fs = ParseFprNumber(ops[1]);
      const int scratch_gpr = ParseGprNumber(ops[2]);
      if (fd < 0 || fs < 0 || scratch_gpr < 0) {
        return "";
      }
      return absl::StrFormat("(parallel[ (set %s (fix:SI %s)) (clobber %s) (clobber %s) ] )",
                             FormatFprRtl(fd, "SI"), FormatFprRtl(fs, src_mode),
                             FormatGprRtl(scratch_gpr, "SI"), FormatFprRtl(fd, src_mode));
    }
    if (ops.size() == 2) {
      const std::string_view src_mode = (mnemonic == "trunc.w.d") ? "DF" : "SF";
      const int fd = ParseFprNumber(ops[0]);
      const int fs = ParseFprNumber(ops[1]);
      if (fd < 0 || fs < 0) {
        return "";
      }
      return absl::StrFormat("(parallel[ (set %s (fix:SI %s)) (clobber %s) (clobber %s) ] )",
                             FormatFprRtl(fd, "SI"), FormatFprRtl(fs, src_mode),
                             FormatGprRtl(1, "SI"), FormatFprRtl(fd, src_mode));
    }
    return "";
  }

  if (absl::StartsWith(mnemonic, "c.") &&
      (absl::EndsWith(mnemonic, ".s") || absl::EndsWith(mnemonic, ".d"))) {
    if (ops.size() != 2) {
      return "";
    }
    const std::string_view mode = absl::EndsWith(mnemonic, ".d") ? "DF" : "SF";
    const std::string_view cond = mnemonic.substr(2, mnemonic.size() - 4);
    const int fs = ParseFprNumber(ops[0]);
    const int ft = ParseFprNumber(ops[1]);
    if (fs < 0 || ft < 0) {
      return "";
    }
    std::string_view rtl_cmp;
    if (cond == "eq" || cond == "seq") {
      rtl_cmp = "eq";
    } else if (cond == "lt" || cond == "olt" || cond == "ult") {
      rtl_cmp = "lt";
    } else if (cond == "le" || cond == "ole" || cond == "ule") {
      rtl_cmp = "le";
    } else if (cond == "gt") {
      rtl_cmp = "gt";
    } else if (cond == "ge") {
      rtl_cmp = "ge";
    } else if (cond == "ne" || cond == "neq") {
      rtl_cmp = "ne";
    } else {
      return "";
    }
    return absl::StrFormat("(set (reg:CC_FP 67 $fcr31) (%s:CC_FP %s %s))", rtl_cmp,
                           FormatFprRtl(fs, mode), FormatFprRtl(ft, mode));
  }

  return "";
}

bool ExprContainsCall(const RtlExpr& expr) {
  bool found = false;
  expr.Visit([&](const RtlExpr& node) {
    if (node.op == RtlOp::kCall || node.op == RtlOp::kCallInsn) {
      found = true;
    }
  });
  return found;
}

RtlExpr MakeInsnWrapperExpr(RtlOp wrapper_op, RtlFlags flags, RtlExpr inner) {
  RtlExpr w;
  w.op = wrapper_op;
  w.flags = flags;
  RtlExpr::GenericItem item;
  item.kind = RtlExpr::GenericItem::Kind::kChildExpr;
  item.child_index = 0;
  w.children.push_back(std::move(inner));
  w.generic_items.push_back(std::move(item));
  return w;
}

RtlExpr MakeSequenceExpr(RtlExpr first, RtlExpr second) {
  RtlExpr seq;
  seq.op = RtlOp::kSequence;
  seq.bracketed_children = true;
  seq.children.push_back(std::move(first));
  seq.children.push_back(std::move(second));
  return seq;
}

std::string FormatAstExpr(const RtlExpr& expr) {
  if (expr.op == RtlOp::kParallel) {
    std::string out = "(parallel[ ";
    for (size_t i = 0; i < expr.children.size(); ++i) {
      if (i > 0) {
        out.push_back(' ');
      }
      out.append(FormatAstExpr(expr.children[i]));
    }
    out.append(" ] )");
    return out;
  }
  if (expr.op == RtlOp::kSequence && expr.bracketed_children) {
    std::string out = "(sequence [";
    for (size_t i = 0; i < expr.children.size(); ++i) {
      if (i > 0) {
        out.push_back(' ');
      }
      out.append(FormatAstExpr(expr.children[i]));
    }
    out.append("])");
    return out;
  }
  if ((expr.op == RtlOp::kInsn || expr.op == RtlOp::kJumpInsn || expr.op == RtlOp::kCallInsn) &&
      expr.children.size() == 1) {
    std::string out = "(";
    out.append(RtlExpr::OpToString(expr.op));
    if (expr.flags.in_struct) out.append("/s");
    if (expr.flags.volatil) out.append("/v");
    if (expr.flags.unchanging) out.append("/u");
    if (expr.flags.integrated) out.append("/i");
    if (expr.flags.frame_related) out.append("/f");
    out.push_back(' ');
    out.append(FormatAstExpr(expr.children[0]));
    out.push_back(')');
    return out;
  }
  return expr.ToSexprString();
}

bool IsReturnOrJumpPattern(const RtlExpr& pat) {
  if (pat.op == RtlOp::kReturn || pat.op == RtlOp::kJumpInsn) {
    return true;
  }
  if (pat.IsSet() && pat.SetDest().op == RtlOp::kPc) {
    return true;
  }
  if (pat.op == RtlOp::kParallel && !pat.children.empty()) {
    return IsReturnOrJumpPattern(pat.children[0]);
  }
  return false;
}

const RtlExpr* UnwrapActiveControlPattern(const RtlExpr& pat) {
  const RtlExpr* cur = &pat;
  if (cur->op == RtlOp::kSequence && !cur->children.empty()) {
    cur = &cur->children[0];
  }
  if ((cur->op == RtlOp::kJumpInsn || cur->op == RtlOp::kCallInsn || cur->op == RtlOp::kInsn) &&
      !cur->children.empty()) {
    cur = &cur->children[0];
  }
  return cur;
}

bool IsTerminalReturnOrIndirectJump(const RtlExpr& pat) {
  const RtlExpr* core = UnwrapActiveControlPattern(pat);
  if (core->op == RtlOp::kParallel && !core->children.empty()) {
    core = &core->children[0];
  }
  if (core->op == RtlOp::kReturn) {
    return true;
  }
  if (core->IsSet() && core->SetDest().op == RtlOp::kPc && core->SetSrc().IsReg()) {
    return true;
  }
  return false;
}

std::optional<std::string> ExtractBranchTargetLabel(const RtlExpr& pat, bool* is_conditional) {
  *is_conditional = false;
  const RtlExpr* core = UnwrapActiveControlPattern(pat);
  if (core->op == RtlOp::kParallel && !core->children.empty()) {
    core = &core->children[0];
  }
  if (!core->IsSet() || core->SetDest().op != RtlOp::kPc) {
    return std::nullopt;
  }
  const RtlExpr& src = core->SetSrc();
  if (src.op == RtlOp::kLabelRef && !src.str_val.empty()) {
    return src.str_val;
  }
  if (src.op == RtlOp::kIfThenElse && src.children.size() == 3) {
    *is_conditional = true;
    if (src.children[1].op == RtlOp::kLabelRef && !src.children[1].str_val.empty()) {
      return src.children[1].str_val;
    }
    if (src.children[2].op == RtlOp::kLabelRef && !src.children[2].str_val.empty()) {
      return src.children[2].str_val;
    }
  }
  return std::nullopt;
}

void RewriteDataLabelRefsInAst(RtlExpr* expr,
                               const absl::flat_hash_map<std::string, size_t>& label_to_block,
                               bool allow_direct_label_ref, bool* modified) {
  if (expr->op == RtlOp::kLabelRef && expr->children.empty() && allow_direct_label_ref) {
    auto it = label_to_block.find(expr->str_val);
    if (it != label_to_block.end()) {
      *modified = true;
      *expr = RtlExpr::MakeSymbolicLabelRef(absl::StrFormat("__blk_%zu", it->second), expr->mode);
    }
    return;
  }

  const bool child_allow_direct = allow_direct_label_ref || expr->op == RtlOp::kHigh ||
                                  expr->op == RtlOp::kLoSum || expr->op == RtlOp::kInsn;

  for (size_t idx = 0; idx < expr->children.size(); ++idx) {
    const bool elem_allow = child_allow_direct || (expr->op == RtlOp::kSequence && idx > 0);
    RewriteDataLabelRefsInAst(&expr->children[idx], label_to_block, elem_allow, modified);
  }
}

RtlControlFlowGraph InvertFinalScanInsnLinesToCfg(absl::Span<const std::string> asm_lines,
                                                  const RtlSidebandState& sideband) {
  if (asm_lines.empty()) {
    return {};
  }

  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kDbr;
  cfg.sideband = sideband;

  std::vector<RtlBasicBlock> blocks;
  absl::flat_hash_map<std::string, size_t> label_to_block;
  RtlBasicBlock cur_blk;
  cur_blk.id = 0;

  for (size_t i = 0; i < asm_lines.size(); ++i) {
    const std::string& line = asm_lines[i];
    if (line == ".set\treorder" || line == ".set reorder") {
      continue;
    }
    if (line == ".set\tnoreorder" || line == ".set noreorder") {
      if (i + 2 < asm_lines.size() && asm_lines[i + 1] == "nop" &&
          (asm_lines[i + 2] == ".set\treorder" || asm_lines[i + 2] == ".set reorder")) {
        const std::string pat = InvertFinalScanInsn(asm_lines[i + 1]);
        if (pat.empty()) {
          return {};
        }
        cur_blk.AddPatternOnly(pat);
        i += 2;
        continue;
      }
      if (i + 3 < asm_lines.size() &&
          (asm_lines[i + 3] == ".set\treorder" || asm_lines[i + 3] == ".set reorder")) {
        const std::string pat0 = InvertFinalScanInsn(asm_lines[i + 1]);
        const std::string pat1 = InvertFinalScanInsn(asm_lines[i + 2]);
        if (pat0.empty() || pat1.empty()) {
          return {};
        }
        auto parsed0 = ParsePatternAndNotes(pat0);
        auto parsed1 = ParsePatternAndNotes(pat1);
        if (!parsed0.ok() || !parsed1.ok()) {
          return {};
        }
        const bool is_call_seq = ExprContainsCall(parsed0->pattern);
        RtlExpr seq_first = is_call_seq
                                ? MakeInsnWrapperExpr(RtlOp::kCallInsn, {}, parsed0->pattern)
                            : parsed0->pattern.op == RtlOp::kJumpInsn
                                ? parsed0->pattern
                                : MakeInsnWrapperExpr(RtlOp::kJumpInsn, {}, parsed0->pattern);
        RtlFlags slot_flags;
        slot_flags.in_struct =
            (parsed0->pattern.op == RtlOp::kJumpInsn && parsed0->pattern.flags.unchanging);
        RtlExpr seq_second = MakeInsnWrapperExpr(RtlOp::kInsn, slot_flags, parsed1->pattern);
        RtlExpr seq_expr = MakeSequenceExpr(std::move(seq_first), std::move(seq_second));
        cur_blk.AddPatternOnly(FormatAstExpr(seq_expr));
        i += 3;
        if (!is_call_seq && i + 1 < asm_lines.size() && !absl::EndsWith(asm_lines[i + 1], ":")) {
          blocks.push_back(std::move(cur_blk));
          cur_blk = RtlBasicBlock{};
          cur_blk.id = blocks.size();
        }
        continue;
      }
      return {};
    }

    if (absl::EndsWith(line, ":")) {
      std::string_view lbl(line.data(), line.size() - 1);
      if (absl::StartsWith(lbl, "$L")) {
        lbl.remove_prefix(1);
      }
      if (!cur_blk.instructions.empty() || cur_blk.has_label) {
        blocks.push_back(std::move(cur_blk));
        cur_blk = RtlBasicBlock{};
        cur_blk.id = blocks.size();
      }
      cur_blk.has_label = true;
      label_to_block[std::string(lbl)] = cur_blk.id;
      continue;
    }

    std::string combined_line = line;
    if (i + 1 < asm_lines.size() &&
        (absl::StartsWith(line, "mtc1\t") || absl::StartsWith(line, "mtc1 ") ||
         absl::StartsWith(line, "l.s\t") || absl::StartsWith(line, "l.s ")) &&
        (absl::StartsWith(asm_lines[i + 1], "cvt.s.w\t") ||
         absl::StartsWith(asm_lines[i + 1], "cvt.s.w ") ||
         absl::StartsWith(asm_lines[i + 1], "cvt.d.w\t") ||
         absl::StartsWith(asm_lines[i + 1], "cvt.d.w "))) {
      combined_line = absl::StrCat(line, "\n", asm_lines[i + 1]);
      ++i;
    } else if (absl::StartsWith(line, "lwl\t") || absl::StartsWith(line, "lwl ")) {
      const size_t max_len = std::min<size_t>(16, asm_lines.size() - i);
      for (size_t len = max_len; len >= 3; --len) {
        const std::string cand = absl::StrJoin(asm_lines.subspan(i, len), "\n");
        if (!InvertFinalScanInsn(cand).empty()) {
          combined_line = cand;
          i += len - 1;
          break;
        }
      }
    }

    const std::string pat = InvertFinalScanInsn(combined_line);
    if (pat.empty()) {
      return {};
    }
    auto parsed_pat = ParsePatternAndNotes(pat);
    if (!parsed_pat.ok()) {
      return {};
    }
    cur_blk.AddPatternOnly(pat);
    if (IsReturnOrJumpPattern(parsed_pat->pattern) && i + 1 < asm_lines.size() &&
        !absl::EndsWith(asm_lines[i + 1], ":")) {
      blocks.push_back(std::move(cur_blk));
      cur_blk = RtlBasicBlock{};
      cur_blk.id = blocks.size();
    }
  }

  if (!cur_blk.instructions.empty() || cur_blk.has_label || blocks.empty()) {
    blocks.push_back(std::move(cur_blk));
  }

  for (size_t b = 0; b < blocks.size(); ++b) {
    bool modified_block = false;
    for (size_t insn_idx = 0; insn_idx < blocks[b].instructions.size(); ++insn_idx) {
      auto parsed = ParsePatternAndNotes(blocks[b].instructions[insn_idx]);
      if (!parsed.ok()) {
        continue;
      }
      const RtlExpr* active_ctrl = UnwrapActiveControlPattern(parsed->pattern);
      const bool is_jump_or_branch_insn =
          (active_ctrl->IsSet() && active_ctrl->SetDest().op == RtlOp::kPc) ||
          active_ctrl->op == RtlOp::kReturn;
      const bool allow_direct =
          (insn_idx + 1 < blocks[b].instructions.size()) || !is_jump_or_branch_insn;
      bool modified_insn = false;
      RewriteDataLabelRefsInAst(&parsed->pattern, label_to_block, allow_direct, &modified_insn);
      if (modified_insn) {
        blocks[b].instructions[insn_idx] = FormatAstExpr(parsed->pattern);
        modified_block = true;
      }
    }
    if (modified_block) {
      blocks[b].SyncRtlInsnsFromInstructions();
    }

    if (blocks[b].instructions.empty()) {
      if (b + 1 < blocks.size()) {
        blocks[b].successors = {b + 1};
      }
      continue;
    }
    auto last_parsed = ParsePatternAndNotes(blocks[b].instructions.back());
    if (!last_parsed.ok()) {
      continue;
    }
    const RtlExpr& last_expr = last_parsed->pattern;
    if (IsTerminalReturnOrIndirectJump(last_expr)) {
      blocks[b].successors.clear();
      continue;
    }
    bool is_conditional = false;
    if (auto target_lbl = ExtractBranchTargetLabel(last_expr, &is_conditional);
        target_lbl.has_value()) {
      auto it = label_to_block.find(*target_lbl);
      if (it != label_to_block.end()) {
        if (is_conditional) {
          if (b + 1 < blocks.size()) {
            blocks[b].successors = {b + 1, it->second};
          } else {
            blocks[b].successors = {it->second};
          }
        } else {
          blocks[b].successors = {it->second};
        }
        continue;
      }
    }
    if (b + 1 < blocks.size()) {
      blocks[b].successors = {b + 1};
    }
  }

  // Reconstruct switch `tablejump` `(parallel[ (set (pc) (reg:SI R)) (use (label_ref L)) ] )`
  // only when `addr_vec_targets` are explicitly populated on the block.
  for (size_t b = 0; b < blocks.size(); ++b) {
    if (blocks[b].instructions.empty() || blocks[b].addr_vec_targets.empty()) {
      continue;
    }
    auto last_parsed = ParsePatternAndNotes(blocks[b].instructions.back());
    if (!last_parsed.ok()) {
      continue;
    }
    const RtlExpr* core = UnwrapActiveControlPattern(last_parsed->pattern);
    if (!core->IsSet() || core->SetDest().op != RtlOp::kPc || !core->SetSrc().IsReg()) {
      continue;
    }
    const std::string tbl_lbl = absl::StrFormat("__tbl_%zu", b);
    RtlExpr tablejump_par =
        RtlExpr::MakeParallel({*core, RtlExpr::MakeUse(RtlExpr::MakeSymbolicLabelRef(tbl_lbl))});
    if (last_parsed->pattern.op == RtlOp::kSequence && last_parsed->pattern.children.size() == 2) {
      RtlExpr seq = MakeSequenceExpr(
          MakeInsnWrapperExpr(RtlOp::kJumpInsn, last_parsed->pattern.children[0].flags,
                              std::move(tablejump_par)),
          last_parsed->pattern.children[1]);
      blocks[b].instructions.back() = FormatAstExpr(seq);
    } else {
      blocks[b].instructions.back() = FormatAstExpr(tablejump_par);
    }
    blocks[b].SyncRtlInsnsFromInstructions();
    for (size_t target_k : blocks[b].addr_vec_targets) {
      if (target_k < blocks.size()) {
        blocks[target_k].has_label = true;
        if (std::find(blocks[b].successors.begin(), blocks[b].successors.end(), target_k) ==
            blocks[b].successors.end()) {
          blocks[b].successors.push_back(target_k);
        }
      }
    }
  }

  // In frameless leaf functions (`simple_epilogue_p() == 1`), the final return
  // instruction comes from `gen_return()` (`(parallel[ (return) (use (reg:SI 31 ra)) ] )`).
  bool has_call_insn = false;
  for (const auto& blk : blocks) {
    for (const std::string& pat : blk.instructions) {
      auto parsed = ParsePatternAndNotes(pat);
      if (parsed.ok() && ExprContainsCall(parsed->pattern)) {
        has_call_insn = true;
        break;
      }
    }
  }
  const bool is_frameless_leaf = !has_call_insn && cfg.sideband.frame_size_bytes == 0 &&
                                 cfg.sideband.saved_gpr_regnos.empty() &&
                                 cfg.sideband.saved_fpr_regnos.empty() && !blocks.empty();
  if (is_frameless_leaf) {
    size_t last_nonempty_b = blocks.size() - 1;
    if (blocks[last_nonempty_b].instructions.empty() && last_nonempty_b > 0) {
      --last_nonempty_b;
    }
    if (!blocks[last_nonempty_b].instructions.empty()) {
      auto last_parsed = ParsePatternAndNotes(blocks[last_nonempty_b].instructions.back());
      if (last_parsed.ok()) {
        RtlExpr gen_return_par = RtlExpr::MakeParallel(
            {RtlExpr::MakeReturn(), RtlExpr::MakeUse(RtlExpr::MakeReg(RtlMachineMode::kSi, 31))});
        if (last_parsed->pattern.op == RtlOp::kReturn) {
          blocks[last_nonempty_b].instructions.back() = FormatAstExpr(gen_return_par);
          blocks[last_nonempty_b].SyncRtlInsnsFromInstructions();
        } else if (last_parsed->pattern.op == RtlOp::kSequence &&
                   last_parsed->pattern.children.size() == 2 &&
                   last_parsed->pattern.children[0].op == RtlOp::kJumpInsn &&
                   !last_parsed->pattern.children[0].children.empty() &&
                   last_parsed->pattern.children[0].children[0].op == RtlOp::kReturn) {
          RtlExpr seq = MakeSequenceExpr(
              MakeInsnWrapperExpr(RtlOp::kJumpInsn, last_parsed->pattern.children[0].flags,
                                  std::move(gen_return_par)),
              last_parsed->pattern.children[1]);
          blocks[last_nonempty_b].instructions.back() = FormatAstExpr(seq);
          blocks[last_nonempty_b].SyncRtlInsnsFromInstructions();
        }
      }
    }
  }

  cfg.rpo_blocks = std::move(blocks);
  return cfg;
}

}  // namespace rom_nom_nom::lifter
