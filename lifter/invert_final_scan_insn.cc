#include "lifter/invert_final_scan_insn.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/base/casts.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/types/span.h"
#include "core/compiler_flags.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "lifter/mips_multi_insn_idioms.h"
#include "lifter/reconstruct_hard_reg_liveness.h"
#include "lifter/reconstruct_o0_final_scan.h"

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

std::string InvertTwoLineTruncW(std::string_view line0, std::string_view line1) {
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
  if (m0 != "trunc.w.s" && m0 != "trunc.w.d") {
    return "";
  }
  const std::string_view src_mode = (m0 == "trunc.w.d") ? "DF" : "SF";
  const std::vector<std::string_view> ops0 = SplitOperands(a0_str);
  if (ops0.size() != 2 && ops0.size() != 3) {
    return "";
  }
  const int fscratch = ParseFprNumber(ops0[0]);
  const int fsrc = ParseFprNumber(ops0[1]);
  const int gscratch = (ops0.size() == 3) ? ParseGprNumber(ops0[2]) : 1;
  if (fscratch < 0 || fsrc < 0 || gscratch < 1) {
    return "";
  }
  if (src_mode == "DF" && ((fscratch % 2) != 0 || (fsrc % 2) != 0)) {
    return "";
  }

  bool is_volatile_mem = false;
  if (absl::StartsWith(line1, "%{") && absl::EndsWith(line1, "%}")) {
    line1 = absl::StripAsciiWhitespace(line1.substr(2, line1.size() - 4));
    is_volatile_mem = true;
  }
  const auto [m1, a1_str] = split_mnemonic(line1);
  const std::vector<std::string_view> ops1 = SplitOperands(a1_str);
  if (ops1.size() != 2) {
    return "";
  }
  if (!is_volatile_mem && m1 == "mfc1") {
    const int gdst = ParseGprNumber(ops1[0]);
    const int xfer_fpr = ParseFprNumber(ops1[1]);
    if (gdst < 1 || xfer_fpr != fscratch) {
      return "";
    }
    return absl::StrFormat("(parallel[ (set %s (fix:SI %s)) (clobber %s) (clobber %s) ] )",
                           FormatGprRtl(gdst, "SI"), FormatFprRtl(fsrc, src_mode),
                           FormatGprRtl(gscratch, "SI"), FormatFprRtl(fscratch, src_mode));
  }
  if (m1 == "s.s" || m1 == "swc1") {
    const int store_fpr = ParseFprNumber(ops1[0]);
    const std::string addr_rtx = FormatMemAddressRtl(ops1[1], "SI");
    if (store_fpr != fscratch || addr_rtx.empty()) {
      return "";
    }
    auto parsed_addr = RtlExpr::Parse(addr_rtx);
    if (!parsed_addr.ok()) {
      return "";
    }
    bool overlaps_earlyclobber = false;
    parsed_addr->Visit([&](const RtlExpr& node) {
      if (node.IsReg() && node.RegNo() == gscratch) {
        overlaps_earlyclobber = true;
      }
    });
    if (overlaps_earlyclobber) {
      return "";
    }
    const std::string_view mem_tag = is_volatile_mem ? "mem/v" : "mem";
    return absl::StrFormat("(parallel[ (set (%s:SI %s) (fix:SI %s)) (clobber %s) (clobber %s) ] )",
                           mem_tag, addr_rtx, FormatFprRtl(fsrc, src_mode),
                           FormatGprRtl(gscratch, "SI"), FormatFprRtl(fscratch, src_mode));
  }
  return "";
}

std::string InvertTwoLineDiConstLoad(std::string_view line0, std::string_view line1,
                                     bool require_nonneg_decimal) {
  auto strip_comment = [](std::string_view s) {
    const size_t hash_pos = s.find('#');
    if (hash_pos != std::string_view::npos) {
      s = s.substr(0, hash_pos);
    }
    return absl::StripAsciiWhitespace(s);
  };
  line0 = strip_comment(line0);
  line1 = strip_comment(line1);
  auto parse_decimal_li = [](std::string_view line, int* out_rd, int64_t* out_val) -> bool {
    size_t sp = 0;
    while (sp < line.size() && !absl::ascii_isspace(static_cast<unsigned char>(line[sp]))) {
      ++sp;
    }
    if (line.substr(0, sp) != "li") {
      return false;
    }
    const std::vector<std::string_view> ops = SplitOperands(line.substr(sp));
    if (ops.size() != 2 || absl::StrContains(ops[1], 'x') || absl::StrContains(ops[1], 'X')) {
      return false;
    }
    const int rd = ParseGprNumber(ops[0]);
    const auto val = ParseIntImmediate(ops[1]);
    if (rd < 2 || rd > 31 || !val.has_value()) {
      return false;
    }
    *out_rd = rd;
    *out_val = *val;
    return true;
  };

  int rd0 = -1;
  int rd1 = -1;
  int64_t hi = 0;
  int64_t lo = 0;
  if (!parse_decimal_li(line0, &rd0, &hi) || !parse_decimal_li(line1, &rd1, &lo)) {
    return "";
  }
  if ((rd0 % 2) != 0 || rd1 != rd0 + 1 || (hi == 0 && lo == 0)) {
    return "";
  }
  if (require_nonneg_decimal && hi < 0 && lo < 0) {
    return "";
  }
  const int32_t hi32 = static_cast<int32_t>(hi);
  const int32_t lo32 = static_cast<int32_t>(lo);
  if ((hi32 == 0 && lo32 > 0) || (hi32 == -1 && lo32 < 0)) {
    return absl::StrFormat("(set %s (const_int %d))", FormatGprRtl(rd0, "DI"), lo32);
  }
  return absl::StrFormat("(set %s (const_double:VOID (cc0) %d %d))", FormatGprRtl(rd0, "DI"), lo32,
                         hi32);
}

std::string InvertTwoLineDfTransfer(std::string_view first_line, std::string_view second_line) {
  auto clean_line = [](std::string_view raw) {
    const size_t hash_pos = raw.find('#');
    if (hash_pos != std::string_view::npos) {
      raw = raw.substr(0, hash_pos);
    }
    return absl::StripAsciiWhitespace(raw);
  };
  first_line = clean_line(first_line);
  second_line = clean_line(second_line);
  if (first_line.empty() || second_line.empty()) {
    return "";
  }
  auto parse_cop1_xfer = [](std::string_view ln, std::string_view* out_mnem, int* out_gpr,
                            int* out_fpr) -> bool {
    size_t sp = 0;
    while (sp < ln.size() && !absl::ascii_isspace(static_cast<unsigned char>(ln[sp]))) {
      ++sp;
    }
    const std::string_view mnem = ln.substr(0, sp);
    if (mnem != "mtc1" && mnem != "mfc1") {
      return false;
    }
    const std::vector<std::string_view> ops = SplitOperands(ln.substr(sp));
    if (ops.size() != 2) {
      return false;
    }
    const int gpr = ParseGprNumber(ops[0]);
    const int fpr = ParseFprNumber(ops[1]);
    if (gpr < 0 || fpr < 0) {
      return false;
    }
    *out_mnem = mnem;
    *out_gpr = gpr;
    *out_fpr = fpr;
    return true;
  };
  std::string_view m0;
  std::string_view m1;
  int g0 = -1;
  int f0 = -1;
  int g1 = -1;
  int f1 = -1;
  if (!parse_cop1_xfer(first_line, &m0, &g0, &f0) || !parse_cop1_xfer(second_line, &m1, &g1, &f1) ||
      m0 != m1 || (f0 % 2) != 0 || f1 != f0 + 1) {
    return "";
  }
  if (m0 == "mtc1" && g0 == 0 && g1 == 0) {
    return absl::StrFormat("(set %s (const_double:DF (cc0) 0 0))", FormatFprRtl(f0, "DF"));
  }
  if (g1 >= 2 && (g1 % 2) == 0 && g0 == g1 + 1) {
    if (m0 == "mtc1") {
      return absl::StrFormat("(set %s %s)", FormatFprRtl(f0, "DF"), FormatGprRtl(g1, "DF"));
    }
    return absl::StrFormat("(set %s %s)", FormatGprRtl(g1, "DF"), FormatFprRtl(f0, "DF"));
  }
  return "";
}

std::string InvertMips1DiShift(std::string_view multi_line_asm) {
  std::vector<std::string_view> insns;
  for (std::string_view raw : absl::StrSplit(multi_line_asm, '\n')) {
    const size_t hash_pos = raw.find('#');
    if (hash_pos != std::string_view::npos) {
      raw = raw.substr(0, hash_pos);
    }
    raw = absl::StripAsciiWhitespace(raw);
    if (raw.empty() || absl::StartsWith(raw, ".set")) {
      continue;
    }
    insns.push_back(raw);
  }

  auto split_op = [](std::string_view line, std::string_view* out_mnem,
                     std::vector<std::string_view>* out_ops) {
    size_t sp = 0;
    while (sp < line.size() && !absl::ascii_isspace(static_cast<unsigned char>(line[sp]))) {
      ++sp;
    }
    *out_mnem = line.substr(0, sp);
    *out_ops = SplitOperands(line.substr(sp));
  };

  // 1. `*_internal2` (2-line constant shift by 32..63).
  if (insns.size() == 2) {
    std::string_view m0, m1;
    std::vector<std::string_view> o0, o1;
    split_op(insns[0], &m0, &o0);
    split_op(insns[1], &m1, &o1);
    if (o0.size() != 3 || (m1 == "move" ? o1.size() != 2 : o1.size() != 3)) {
      return "";
    }
    const int r0_dst = ParseGprNumber(o0[0]);
    const int r0_src = ParseGprNumber(o0[1]);
    const auto amt_mod32 = ParseIntImmediate(o0[2]);
    const int r1_dst = ParseGprNumber(o1[0]);
    if (r0_dst < 2 || r0_src < 2 || r1_dst < 2 || !amt_mod32.has_value() || *amt_mod32 < 0 ||
        *amt_mod32 > 31) {
      return "";
    }
    const int32_t full_amt = static_cast<int32_t>(32 + *amt_mod32);
    if (m0 == "sll" && m1 == "move" && (r0_dst % 2) == 0 && (r0_src % 2) == 1 &&
        r1_dst == r0_dst + 1 && ParseGprNumber(o1[1]) == 0) {
      return absl::StrFormat(
          "(parallel[ (set %s (ashift:DI %s (const_int %d))) (clobber (reg:SI 1 at)) ] )",
          FormatGprRtl(r0_dst, "DI"), FormatGprRtl(r0_src - 1, "DI"), full_amt);
    }
    if (m0 == "srl" && m1 == "move" && (r0_dst % 2) == 1 && (r0_src % 2) == 0 &&
        r1_dst == r0_dst - 1 && ParseGprNumber(o1[1]) == 0) {
      return absl::StrFormat(
          "(parallel[ (set %s (lshiftrt:DI %s (const_int %d))) (clobber (reg:SI 1 at)) ] )",
          FormatGprRtl(r1_dst, "DI"), FormatGprRtl(r0_src, "DI"), full_amt);
    }
    const auto sra_imm = (o1.size() == 3) ? ParseIntImmediate(o1[2]) : std::nullopt;
    if (m0 == "sra" && m1 == "sra" && (r0_dst % 2) == 1 && (r0_src % 2) == 0 &&
        r1_dst == r0_dst - 1 && ParseGprNumber(o1[1]) == r0_src && sra_imm.has_value() &&
        *sra_imm == 31) {
      return absl::StrFormat(
          "(parallel[ (set %s (ashiftrt:DI %s (const_int %d))) (clobber (reg:SI 1 at)) ] )",
          FormatGprRtl(r1_dst, "DI"), FormatGprRtl(r0_src, "DI"), full_amt);
    }
    return "";
  }

  // 2. `*_internal3` (4-line constant shift by 1..31).
  if (insns.size() == 4) {
    std::string_view m[4];
    std::vector<std::string_view> o[4];
    for (int k = 0; k < 4; ++k) {
      split_op(insns[k], &m[k], &o[k]);
      if (o[k].size() != 3) {
        return "";
      }
    }
    const auto amt0 = ParseIntImmediate(o[0][2]);
    const auto amt1 = ParseIntImmediate(o[1][2]);
    const auto amt3 = ParseIntImmediate(o[3][2]);
    if (!amt0.has_value() || !amt1.has_value() || !amt3.has_value() || *amt0 < 1 || *amt0 > 31 ||
        *amt0 != *amt3 || *amt1 != (32 - *amt0) || m[2] != "or") {
      return "";
    }
    const int r_scr = ParseGprNumber(o[1][0]);
    if (r_scr < 1 || ParseGprNumber(o[2][2]) != r_scr) {
      return "";
    }
    if (m[0] == "sll" && m[1] == "srl" && m[3] == "sll") {
      const int d0 = ParseGprNumber(o[0][0]);
      const int s1 = ParseGprNumber(o[0][1]);
      if (d0 >= 2 && (d0 % 2) == 0 && s1 >= 2 && (s1 % 2) == 0 &&
          ParseGprNumber(o[1][1]) == s1 + 1 && ParseGprNumber(o[2][0]) == d0 &&
          ParseGprNumber(o[2][1]) == d0 && ParseGprNumber(o[3][0]) == d0 + 1 &&
          ParseGprNumber(o[3][1]) == s1 + 1) {
        return absl::StrFormat("(parallel[ (set %s (ashift:DI %s (const_int %d))) (clobber %s) ] )",
                               FormatGprRtl(d0, "DI"), FormatGprRtl(s1, "DI"),
                               static_cast<int32_t>(*amt0), FormatGprRtl(r_scr, "SI"));
      }
    }
    if (m[0] == "srl" && m[1] == "sll" && (m[3] == "sra" || m[3] == "srl")) {
      const int d0_lo = ParseGprNumber(o[0][0]);
      const int s1_lo = ParseGprNumber(o[0][1]);
      const int d0 = d0_lo - 1;
      const int s1 = s1_lo - 1;
      if (d0 >= 2 && (d0_lo % 2) == 1 && s1 >= 2 && (s1_lo % 2) == 1 &&
          ParseGprNumber(o[1][1]) == s1 && ParseGprNumber(o[2][0]) == d0_lo &&
          ParseGprNumber(o[2][1]) == d0_lo && ParseGprNumber(o[3][0]) == d0 &&
          ParseGprNumber(o[3][1]) == s1) {
        const std::string_view rtl_op = (m[3] == "sra") ? "ashiftrt" : "lshiftrt";
        return absl::StrFormat("(parallel[ (set %s (%s:DI %s (const_int %d))) (clobber %s) ] )",
                               FormatGprRtl(d0, "DI"), rtl_op, FormatGprRtl(s1, "DI"),
                               static_cast<int32_t>(*amt0), FormatGprRtl(r_scr, "SI"));
      }
    }
    return "";
  }

  // 3. `*_internal` (14-item variable 64-bit shift with `1:`, `2:`, `3:` or
  // `.o`-disassembled `$L<a>:`, `$L<b>:`, `$L<c>:` labels).
  if (insns.size() == 15 && insns[2] == "nop") {
    insns.erase(insns.begin() + 2);
  }
  if (insns.size() == 14 && absl::EndsWith(insns[5], ":") && absl::EndsWith(insns[11], ":") &&
      absl::EndsWith(insns[13], ":")) {
    const bool is_numeric_local = (insns[5] == "1:" && insns[11] == "2:" && insns[13] == "3:");
    const std::string_view lbl_a = insns[5].substr(0, insns[5].size() - 1);
    const std::string_view lbl_b = insns[11].substr(0, insns[11].size() - 1);
    const std::string_view lbl_c = insns[13].substr(0, insns[13].size() - 1);
    auto is_local_sym_lbl = [](std::string_view s) {
      return (absl::StartsWith(s, "$L") || absl::StartsWith(s, ".L")) && s.size() > 2;
    };
    if (!is_numeric_local &&
        (!is_local_sym_lbl(lbl_a) || !is_local_sym_lbl(lbl_b) || !is_local_sym_lbl(lbl_c) ||
         lbl_a == lbl_b || lbl_b == lbl_c || lbl_a == lbl_c)) {
      return "";
    }
    const std::string_view exp_t_a = is_numeric_local ? std::string_view("1f") : lbl_a;
    const std::string_view exp_t_b = is_numeric_local ? std::string_view("2f") : lbl_b;
    const std::string_view exp_t_c = is_numeric_local ? std::string_view("3f") : lbl_c;

    std::string_view m0, m1, m2, m3, m4, m6, m7, m8, m9, m10, m12;
    std::vector<std::string_view> o0, o1, o2, o3, o4, o6, o7, o8, o9, o10, o12;
    split_op(insns[0], &m0, &o0);
    split_op(insns[1], &m1, &o1);
    split_op(insns[2], &m2, &o2);
    split_op(insns[3], &m3, &o3);
    split_op(insns[4], &m4, &o4);
    split_op(insns[6], &m6, &o6);
    split_op(insns[7], &m7, &o7);
    split_op(insns[8], &m8, &o8);
    split_op(insns[9], &m9, &o9);
    split_op(insns[10], &m10, &o10);
    split_op(insns[12], &m12, &o12);

    auto norm_var_shift = [](std::string_view m) -> std::string_view {
      if (m == "sllv") return "sll";
      if (m == "srlv") return "srl";
      if (m == "srav") return "sra";
      return m;
    };
    m2 = norm_var_shift(m2);
    m7 = norm_var_shift(m7);
    m9 = norm_var_shift(m9);
    m12 = norm_var_shift(m12);

    auto is_uncond_branch_to = [&](std::string_view m, absl::Span<const std::string_view> ops,
                                   std::string_view target) {
      if (m == "b" && ops.size() == 1 && ops[0] == target) {
        return true;
      }
      if (!is_numeric_local && m == "beq" && ops.size() == 3 && ParseGprNumber(ops[0]) == 0 &&
          ParseGprNumber(ops[1]) == 0 && ops[2] == target) {
        return true;
      }
      return false;
    };
    auto is_zero_move_to = [](std::string_view m, absl::Span<const std::string_view> ops,
                              int dst_reg) {
      if (m == "move" && ops.size() == 2) {
        return ParseGprNumber(ops[0]) == dst_reg && ParseGprNumber(ops[1]) == 0;
      }
      if ((m == "addu" || m == "or") && ops.size() == 3) {
        return ParseGprNumber(ops[0]) == dst_reg && ParseGprNumber(ops[1]) == 0 &&
               ParseGprNumber(ops[2]) == 0;
      }
      return false;
    };

    const auto c26 = (o0.size() == 3) ? ParseIntImmediate(o0[2]) : std::nullopt;
    if (m0 != "sll" || o0.size() != 3 || !c26.has_value() || *c26 != 26 || m1 != "bgez" ||
        o1.size() != 2 || o1[1] != exp_t_a || !is_uncond_branch_to(m3, o3, exp_t_c) ||
        m6 != "beq" || o6.size() != 3 || ParseGprNumber(o6[1]) != 0 || o6[2] != exp_t_b ||
        m8 != "subu" || o8.size() != 3 || ParseGprNumber(o8[1]) != 0 || m10 != "or" ||
        o10.size() != 3 || o2.size() != 3 || o7.size() != 3 || o9.size() != 3 || o12.size() != 3) {
      return "";
    }
    const int r3 = ParseGprNumber(o0[0]);
    const int r2 = ParseGprNumber(o0[1]);
    if (r3 < 1 || r2 < 1 || ParseGprNumber(o1[0]) != r3 || ParseGprNumber(o6[0]) != r3 ||
        ParseGprNumber(o8[0]) != r3 || ParseGprNumber(o8[2]) != r2 || ParseGprNumber(o9[0]) != r3 ||
        ParseGprNumber(o9[2]) != r3 || ParseGprNumber(o10[2]) != r3 ||
        ParseGprNumber(o2[2]) != r2 || ParseGprNumber(o7[2]) != r2 ||
        ParseGprNumber(o12[2]) != r2) {
      return "";
    }

    if (m2 == "sll" && m7 == "sll" && m9 == "srl" && m12 == "sll") {
      const int d0 = ParseGprNumber(o2[0]);
      const int s1_lo = ParseGprNumber(o2[1]);
      const int s1 = s1_lo - 1;
      if (d0 >= 2 && (d0 % 2) == 0 && s1 >= 2 && (s1_lo % 2) == 1 && d0 != s1 && r2 != d0 &&
          r2 != d0 + 1 && r3 != d0 && r3 != d0 + 1 && is_zero_move_to(m4, o4, d0 + 1) &&
          ParseGprNumber(o7[0]) == d0 && ParseGprNumber(o7[1]) == s1 &&
          ParseGprNumber(o9[1]) == s1_lo && ParseGprNumber(o10[0]) == d0 &&
          ParseGprNumber(o10[1]) == d0 && ParseGprNumber(o12[0]) == d0 + 1 &&
          ParseGprNumber(o12[1]) == s1_lo) {
        return absl::StrFormat("(parallel[ (set %s (ashift:DI %s %s)) (clobber %s) ] )",
                               FormatGprRtl(d0, "DI"), FormatGprRtl(s1, "DI"),
                               FormatGprRtl(r2, "SI"), FormatGprRtl(r3, "SI"));
      }
    }

    if ((m2 == "sra" || m2 == "srl") && m7 == "srl" && m9 == "sll" && m12 == m2) {
      const bool is_arith = (m2 == "sra");
      const int d0_lo = ParseGprNumber(o2[0]);
      const int s1 = ParseGprNumber(o2[1]);
      const int d0 = d0_lo - 1;
      bool fill_ok = false;
      if (is_arith) {
        const auto c31 = (o4.size() == 3) ? ParseIntImmediate(o4[2]) : std::nullopt;
        fill_ok = (m4 == "sra" && o4.size() == 3 && ParseGprNumber(o4[0]) == d0 &&
                   ParseGprNumber(o4[1]) == s1 && c31.has_value() && *c31 == 31);
      } else {
        fill_ok = is_zero_move_to(m4, o4, d0);
      }
      if (fill_ok && d0 >= 2 && (d0_lo % 2) == 1 && s1 >= 2 && (s1 % 2) == 0 && d0 != s1 &&
          r2 != d0 && r2 != d0 + 1 && r3 != d0 && r3 != d0 + 1 && ParseGprNumber(o7[0]) == d0_lo &&
          ParseGprNumber(o7[1]) == s1 + 1 && ParseGprNumber(o9[1]) == s1 &&
          ParseGprNumber(o10[0]) == d0_lo && ParseGprNumber(o10[1]) == d0_lo &&
          ParseGprNumber(o12[0]) == d0 && ParseGprNumber(o12[1]) == s1) {
        const std::string_view rtl_op = is_arith ? "ashiftrt" : "lshiftrt";
        return absl::StrFormat("(parallel[ (set %s (%s:DI %s %s)) (clobber %s) ] )",
                               FormatGprRtl(d0, "DI"), rtl_op, FormatGprRtl(s1, "DI"),
                               FormatGprRtl(r2, "SI"), FormatGprRtl(r3, "SI"));
      }
    }
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
  constexpr std::array<int, 16> kCallerSavedAllocOrder = {2,  3,  4,  5,  6,  7,  8,  9,
                                                          10, 11, 12, 13, 14, 15, 24, 25};
  const int max_used_clobber = *std::max_element(clobber_regs.begin(), clobber_regs.end());
  for (int pass = 0; pass < 2 && clobber_regs.size() < 4; ++pass) {
    for (int cand : kCallerSavedAllocOrder) {
      if ((pass == 0 && cand <= max_used_clobber) || cand == src_base || cand == dst_base ||
          std::find(clobber_regs.begin(), clobber_regs.end(), cand) != clobber_regs.end()) {
        continue;
      }
      clobber_regs.push_back(cand);
      if (clobber_regs.size() == 4) {
        break;
      }
    }
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

std::string InvertFloatingPointFinalScanInsn(std::string_view mnemonic,
                                             absl::Span<const std::string_view> ops) {
  if (mnemonic == "li.s" || mnemonic == "li.d") {
    if (ops.size() != 2) {
      return "";
    }
    const std::string_view mode = (mnemonic == "li.d") ? "DF" : "SF";
    const int fpr = ParseFprNumber(ops[0]);
    const int gpr = (fpr < 0) ? ParseGprNumber(ops[0]) : -1;
    if (fpr < 0 && gpr < 1) {
      return "";
    }
    const std::string dst_rtx = (fpr >= 0) ? FormatFprRtl(fpr, mode) : FormatGprRtl(gpr, mode);
    const std::string val_str(ops[1]);
    char* endptr = nullptr;
    const double d = std::strtod(val_str.c_str(), &endptr);
    if (endptr == val_str.c_str() || *endptr != '\0') {
      return "";
    }
    const uint64_t bits = absl::bit_cast<uint64_t>(d);
    const int32_t lo = static_cast<int32_t>(bits & 0xffffffffu);
    const int32_t hi = static_cast<int32_t>(bits >> 32);
    return absl::StrFormat("(set %s (const_double:%s (cc0) %d %d))", dst_rtx, mode, lo, hi);
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

}  // namespace

std::string InvertFinalScanInsn(std::string_view asm_line_x) {
  std::string_view trimmed_full = absl::StripAsciiWhitespace(asm_line_x);
  if (trimmed_full.empty()) {
    return "";
  }

  // Check for multi-line instruction outputs (`floatsisf2`/`floatsidf2`, `fix_trunc*`,
  // `!TARGET_64BIT` `DImode` loads/shifts, or `movstrsi`).
  if (const size_t nl_pos = trimmed_full.find('\n'); nl_pos != std::string_view::npos) {
    const std::string_view first_line = trimmed_full.substr(0, nl_pos);
    const std::string_view second_line = trimmed_full.substr(nl_pos + 1);
    if (second_line.find('\n') == std::string_view::npos) {
      if (std::string float_si = InvertTwoLineFloatSi(first_line, second_line); !float_si.empty()) {
        return float_si;
      }
      if (std::string trunc_w = InvertTwoLineTruncW(first_line, second_line); !trunc_w.empty()) {
        return trunc_w;
      }
      if (std::string di_li =
              InvertTwoLineDiConstLoad(first_line, second_line, /*require_nonneg_decimal=*/false);
          !di_li.empty()) {
        return di_li;
      }
      if (std::string df_xfer = InvertTwoLineDfTransfer(first_line, second_line);
          !df_xfer.empty()) {
        return df_xfer;
      }
    }
    if (std::string di_shift = InvertMips1DiShift(trimmed_full); !di_shift.empty()) {
      return di_shift;
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
        return "(parallel[ (return) (use (reg:SI 31 ra)) ] )";
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
      mnemonic == "srl" || mnemonic == "srlv" || mnemonic == "dsll" || mnemonic == "dsll32" ||
      mnemonic == "dsllv" || mnemonic == "dsra" || mnemonic == "dsra32" || mnemonic == "dsrav" ||
      mnemonic == "dsrl" || mnemonic == "dsrl32" || mnemonic == "dsrlv") {
    if (ops.size() != 3) {
      return "";
    }
    const bool is_64 = absl::StartsWith(mnemonic, "d");
    const bool plus_32 = absl::EndsWith(mnemonic, "32");
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
    if (rs >= 0 && !plus_32) {
      return absl::StrFormat("(set %s (%s:%s %s %s))", FormatGprRtl(rd, mode), rtl_op, mode,
                             FormatGprRtl(rt, mode), FormatGprRtl(rs, "SI"));
    }
    if (const auto sa = ParseIntImmediate(ops[2]); sa.has_value()) {
      const int32_t shift_amt = static_cast<int32_t>(*sa) + (plus_32 ? 32 : 0);
      return absl::StrFormat("(set %s (%s:%s %s (const_int %d)))", FormatGprRtl(rd, mode), rtl_op,
                             mode, FormatGprRtl(rt, mode), shift_amt);
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

  return InvertFloatingPointFinalScanInsn(mnemonic, ops);
}

namespace {

bool ExprContainsCall(const RtlExpr& expr) {
  bool found = false;
  expr.Visit([&](const RtlExpr& node) {
    if (node.op == RtlOp::kCall || node.op == RtlOp::kCallInsn) {
      found = true;
    }
  });
  return found;
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
                               const absl::flat_hash_set<std::string>& jt_tokens,
                               bool allow_direct_label_ref, bool* modified) {
  if (expr->op == RtlOp::kLabelRef && expr->children.empty() && allow_direct_label_ref) {
    auto it = label_to_block.find(expr->str_val);
    if (it != label_to_block.end()) {
      *modified = true;
      *expr = RtlExpr::MakeSymbolicLabelRef(absl::StrFormat("__blk_%zu", it->second), expr->mode);
      return;
    }
    if (!jt_tokens.contains(expr->str_val)) {
      std::string_view num_part = expr->str_val;
      if (absl::StartsWith(num_part, "L")) {
        num_part.remove_prefix(1);
        int32_t lbl_num = 0;
        if (absl::SimpleAtoi(num_part, &lbl_num) && lbl_num >= 0 && lbl_num < 100000) {
          *modified = true;
          *expr = RtlExpr::MakeSymbolicLabelRef(absl::StrCat("L", 100000 + lbl_num), expr->mode);
        }
      }
    }
    return;
  }

  const bool child_allow_direct = allow_direct_label_ref || expr->op == RtlOp::kHigh ||
                                  expr->op == RtlOp::kLoSum || expr->op == RtlOp::kInsn;

  for (size_t idx = 0; idx < expr->children.size(); ++idx) {
    const bool elem_allow = child_allow_direct || (expr->op == RtlOp::kSequence && idx > 0);
    RewriteDataLabelRefsInAst(&expr->children[idx], label_to_block, jt_tokens, elem_allow,
                              modified);
  }
}

void RewriteSpecificLabelRefInAst(RtlExpr* expr, std::string_view from_token,
                                  std::string_view to_token, bool* modified) {
  if (expr->op == RtlOp::kLabelRef && expr->children.empty() && expr->str_val == from_token) {
    *modified = true;
    *expr = RtlExpr::MakeSymbolicLabelRef(std::string(to_token), expr->mode);
    return;
  }
  for (RtlExpr& child : expr->children) {
    RewriteSpecificLabelRefInAst(&child, from_token, to_token, modified);
  }
}

bool IsTableJumpCoreExpr(const RtlExpr& core) {
  return core.IsParallel() && core.children.size() == 2 && core.children[0].IsSet() &&
         core.children[0].SetDest().op == RtlOp::kPc && core.children[0].SetSrc().IsReg() &&
         core.children[1].op == RtlOp::kUse && core.children[1].children.size() == 1 &&
         core.children[1].children[0].op == RtlOp::kLabelRef;
}

void RewriteBranchTargetLabelInAst(RtlExpr* pat, std::string_view new_label_tok, bool* modified) {
  RtlExpr* cur = pat;
  if (cur->op == RtlOp::kSequence && !cur->children.empty()) {
    cur = &cur->children[0];
  }
  if ((cur->op == RtlOp::kJumpInsn || cur->op == RtlOp::kInsn) && !cur->children.empty()) {
    cur = &cur->children[0];
  }
  if (cur->op == RtlOp::kParallel && !cur->children.empty()) {
    cur = &cur->children[0];
  }
  if (!cur->IsSet() || cur->SetDest().op != RtlOp::kPc) {
    return;
  }
  RtlExpr& src = cur->children[1];
  if (src.op == RtlOp::kLabelRef && !src.str_val.empty()) {
    if (src.str_val != new_label_tok) {
      src = RtlExpr::MakeSymbolicLabelRef(std::string(new_label_tok), src.mode);
      *modified = true;
    }
    return;
  }
  if (src.op == RtlOp::kIfThenElse && src.children.size() == 3) {
    for (size_t arm : {size_t{1}, size_t{2}}) {
      if (src.children[arm].op == RtlOp::kLabelRef && !src.children[arm].str_val.empty() &&
          src.children[arm].str_val != new_label_tok) {
        src.children[arm] =
            RtlExpr::MakeSymbolicLabelRef(std::string(new_label_tok), src.children[arm].mode);
        *modified = true;
      }
    }
  }
}

void CanonicalizeCfgLabels(RtlControlFlowGraph* cfg) {
  const size_t num_blocks = cfg->rpo_blocks.size();
  if (num_blocks == 0) {
    return;
  }
  std::vector<bool> block_needs_label(num_blocks, false);
  std::vector<bool> block_has_tablejump(num_blocks, false);
  std::vector<std::string> old_tablejump_token(num_blocks);

  for (size_t b = 0; b < num_blocks; ++b) {
    const auto& blk = cfg->rpo_blocks[b];
    if (blk.has_label) {
      block_needs_label[b] = true;
    }
    bool ends_with_uncond = false;
    if (!blk.instructions.empty()) {
      auto parsed = ParsePatternAndNotes(blk.instructions.back());
      if (parsed.ok()) {
        const RtlExpr* core = UnwrapActiveControlPattern(parsed->pattern);
        if (IsTableJumpCoreExpr(*core)) {
          ends_with_uncond = true;
          block_has_tablejump[b] = true;
          old_tablejump_token[b] = core->children[1].children[0].str_val;
          for (size_t t : blk.addr_vec_targets) {
            if (t < num_blocks) {
              block_needs_label[t] = true;
            }
          }
        } else {
          bool is_cond = false;
          if (ExtractBranchTargetLabel(parsed->pattern, &is_cond).has_value() && !is_cond) {
            ends_with_uncond = true;
          } else if (IsTerminalReturnOrIndirectJump(parsed->pattern)) {
            ends_with_uncond = true;
          }
        }
      }
    }
    if (blk.successors.size() >= 2 && blk.addr_vec_targets.empty()) {
      size_t target_b = blk.successors[0];
      if (target_b == b + 1) {
        target_b = blk.successors[1];
      }
      if (target_b < num_blocks) {
        block_needs_label[target_b] = true;
      }
    }
    for (size_t succ : blk.successors) {
      if (succ < num_blocks && (succ != b + 1 || ends_with_uncond)) {
        block_needs_label[succ] = true;
      }
    }
  }

  std::vector<std::string> block_label_token(num_blocks);
  absl::flat_hash_map<std::string, std::string> tablejump_token_remap;
  size_t next_lbl_idx = 0;
  for (size_t b = 0; b < num_blocks; ++b) {
    cfg->rpo_blocks[b].has_label = block_needs_label[b];
    if (block_needs_label[b]) {
      block_label_token[b] = absl::StrCat("L", next_lbl_idx++);
    }
    if (block_has_tablejump[b]) {
      std::string new_tbl_tok = absl::StrCat("L", next_lbl_idx++);
      if (!old_tablejump_token[b].empty()) {
        tablejump_token_remap[old_tablejump_token[b]] = std::move(new_tbl_tok);
      }
    }
  }

  for (size_t b = 0; b < num_blocks; ++b) {
    RtlBasicBlock& blk = cfg->rpo_blocks[b];
    bool modified_blk = false;
    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      auto parsed = ParsePatternAndNotes(blk.instructions[i]);
      if (!parsed.ok()) {
        continue;
      }
      bool modified_insn = false;
      if (!tablejump_token_remap.empty()) {
        auto remap_fn = [&tablejump_token_remap, &modified_insn](RtlExpr& node) {
          if (node.op == RtlOp::kLabelRef && node.children.empty() && !node.str_val.empty()) {
            auto it = tablejump_token_remap.find(node.str_val);
            if (it != tablejump_token_remap.end() && node.str_val != it->second) {
              node = RtlExpr::MakeSymbolicLabelRef(it->second, node.mode);
              modified_insn = true;
            }
          }
        };
        parsed->pattern.Mutate(remap_fn);
        for (RtlRegNote& note : parsed->reg_notes) {
          note.datum.Mutate(remap_fn);
        }
      }
      if (i + 1 == blk.instructions.size() && !block_has_tablejump[b] && !blk.successors.empty()) {
        const size_t target_b = blk.successors[0];
        if (target_b < num_blocks && !block_label_token[target_b].empty()) {
          RewriteBranchTargetLabelInAst(&parsed->pattern, block_label_token[target_b],
                                        &modified_insn);
        }
      }
      if (modified_insn) {
        blk.instructions[i] = parsed->ToGccPrintRtlString();
        modified_blk = true;
      }
    }
    if (modified_blk) {
      blk.SyncRtlInsnsFromInstructions();
    }
  }
}

// Marks `SYMBOL_REF`s referencing constant-pool labels (`CONSTANT_POOL_ADDRESS_P`,
// `/u`) and any enclosing `MEM` (`RTX_UNCHANGING_P`, `/u`) on the structured
// `RtlExpr` AST.
bool MarkConstantPoolRefsInAst(RtlExpr* expr, const absl::flat_hash_set<std::string>& pool_labels,
                               bool* modified) {
  if (expr->op == RtlOp::kSymbolRef && pool_labels.contains(expr->str_val)) {
    if (!expr->flags.unchanging) {
      expr->flags.unchanging = true;
      *modified = true;
    }
    return true;
  }
  bool child_has_pool_sym = false;
  for (RtlExpr& child : expr->children) {
    if (MarkConstantPoolRefsInAst(&child, pool_labels, modified)) {
      child_has_pool_sym = true;
    }
  }
  if (expr->op == RtlOp::kMem && child_has_pool_sym && !expr->flags.unchanging) {
    expr->flags.unchanging = true;
    *modified = true;
  }
  return child_has_pool_sym;
}

bool IsUnassignedIndirectJumpBlock(const RtlBasicBlock& blk) {
  if (blk.instructions.empty() || !blk.addr_vec_targets.empty()) {
    return false;
  }
  auto last_parsed = ParsePatternAndNotes(blk.instructions.back());
  if (!last_parsed.ok()) {
    return false;
  }
  const RtlExpr* core = UnwrapActiveControlPattern(last_parsed->pattern);
  return core->IsSet() && core->SetDest().op == RtlOp::kPc && core->SetSrc().IsReg();
}

using RegJtTaint = std::array<absl::flat_hash_set<size_t>, 32>;

absl::flat_hash_set<size_t> EvalExprJtTaint(
    const RtlExpr& expr, const RegJtTaint& state,
    const absl::flat_hash_map<std::string, size_t>& token_to_jt_idx) {
  if (expr.op == RtlOp::kLabelRef && !expr.str_val.empty()) {
    if (const auto it = token_to_jt_idx.find(expr.str_val); it != token_to_jt_idx.end()) {
      return {it->second};
    }
    return {};
  }
  if (expr.IsReg()) {
    const int r = expr.RegNo();
    if (r > 0 && r < 32) {
      return state[r];
    }
    return {};
  }
  if (expr.op == RtlOp::kLoSum && expr.children.size() == 2) {
    return EvalExprJtTaint(expr.children[1], state, token_to_jt_idx);
  }
  absl::flat_hash_set<size_t> out;
  for (const RtlExpr& child : expr.children) {
    const absl::flat_hash_set<size_t> child_taint = EvalExprJtTaint(child, state, token_to_jt_idx);
    out.insert(child_taint.begin(), child_taint.end());
  }
  return out;
}

void ClobberCallerSavedJtTaint(RegJtTaint* state) {
  for (int r = 1; r <= 15; ++r) {
    (*state)[r].clear();
  }
  (*state)[24].clear();
  (*state)[25].clear();
  (*state)[31].clear();
}

void StepSinglePatternJtTaint(const RtlExpr& pat, RegJtTaint* state,
                              const absl::flat_hash_map<std::string, size_t>& token_to_jt_idx) {
  if (ExprContainsCall(pat)) {
    ClobberCallerSavedJtTaint(state);
    if (pat.IsSet() && pat.SetDest().IsReg()) {
      const int dst = pat.SetDest().RegNo();
      if (dst > 0 && dst < 32) {
        (*state)[dst].clear();
      }
    }
    return;
  }
  if (pat.IsSet()) {
    if (pat.SetDest().IsReg()) {
      const int dst = pat.SetDest().RegNo();
      if (dst > 0 && dst < 32) {
        (*state)[dst] = EvalExprJtTaint(pat.SetSrc(), *state, token_to_jt_idx);
      }
    }
    return;
  }
  if (pat.IsParallel()) {
    const RegJtTaint pre = *state;
    for (const RtlExpr& elem : pat.children) {
      if (elem.op == RtlOp::kClobber && !elem.children.empty() && elem.children[0].IsReg()) {
        const int dst = elem.children[0].RegNo();
        if (dst > 0 && dst < 32) {
          (*state)[dst].clear();
        }
      }
    }
    for (const RtlExpr& elem : pat.children) {
      if (elem.IsSet() && elem.SetDest().IsReg()) {
        const int dst = elem.SetDest().RegNo();
        if (dst > 0 && dst < 32) {
          (*state)[dst] = EvalExprJtTaint(elem.SetSrc(), pre, token_to_jt_idx);
        }
      }
    }
    return;
  }
  if (pat.op == RtlOp::kClobber && !pat.children.empty() && pat.children[0].IsReg()) {
    const int dst = pat.children[0].RegNo();
    if (dst > 0 && dst < 32) {
      (*state)[dst].clear();
    }
  }
}

void DisambiguateAndAttachJumpTables(absl::Span<const Stage0ResolvedJumpTable> jump_tables,
                                     const absl::flat_hash_map<std::string, size_t>& label_to_block,
                                     std::vector<RtlBasicBlock>* blocks_ptr) {
  std::vector<RtlBasicBlock>& blocks = *blocks_ptr;
  if (jump_tables.empty() || blocks.empty()) {
    return;
  }

  struct ValidJumpTable {
    std::string table_label_token;
    std::vector<size_t> resolved_targets;
  };
  std::vector<ValidJumpTable> valid_jts;
  absl::flat_hash_map<std::string, size_t> token_to_jt_idx;
  for (const Stage0ResolvedJumpTable& jt : jump_tables) {
    if (jt.table_label_token.empty() || jt.target_labels.empty()) {
      continue;
    }
    std::vector<size_t> resolved_targets;
    resolved_targets.reserve(jt.target_labels.size());
    bool all_resolved = true;
    for (const std::string& raw_lbl : jt.target_labels) {
      std::string_view key = raw_lbl;
      if (absl::StartsWith(key, "$L")) {
        key.remove_prefix(1);
      }
      const auto it = label_to_block.find(key);
      if (it == label_to_block.end()) {
        all_resolved = false;
        break;
      }
      resolved_targets.push_back(it->second);
    }
    if (!all_resolved || resolved_targets.empty()) {
      continue;
    }
    const size_t idx = valid_jts.size();
    token_to_jt_idx[jt.table_label_token] = idx;
    valid_jts.push_back(ValidJumpTable{
        .table_label_token = jt.table_label_token,
        .resolved_targets = std::move(resolved_targets),
    });
  }
  if (valid_jts.empty()) {
    return;
  }

  const size_t num_blocks = blocks.size();
  std::vector<std::vector<std::optional<RtlExpr>>> parsed_blocks(num_blocks);
  std::vector<int> indirect_jump_reg(num_blocks, -1);
  for (size_t b = 0; b < num_blocks; ++b) {
    parsed_blocks[b].resize(blocks[b].instructions.size());
    for (size_t i = 0; i < blocks[b].instructions.size(); ++i) {
      auto parsed = ParsePatternAndNotes(blocks[b].instructions[i]);
      if (parsed.ok()) {
        parsed_blocks[b][i] = std::move(parsed->pattern);
      }
    }
    if (IsUnassignedIndirectJumpBlock(blocks[b]) && !parsed_blocks[b].empty() &&
        parsed_blocks[b].back().has_value()) {
      const RtlExpr* core = UnwrapActiveControlPattern(*parsed_blocks[b].back());
      if (core->IsSet() && core->SetDest().op == RtlOp::kPc && core->SetSrc().IsReg()) {
        const int r = core->SetSrc().RegNo();
        if (r > 0 && r < 32) {
          indirect_jump_reg[b] = r;
        }
      }
    }
  }

  std::vector<std::optional<size_t>> jt_bound_block(valid_jts.size());
  std::vector<bool> block_bound(num_blocks, false);

  auto bind_jt_to_block = [&](size_t jt_idx, size_t disp_b) {
    jt_bound_block[jt_idx] = disp_b;
    block_bound[disp_b] = true;
    blocks[disp_b].addr_vec_targets = valid_jts[jt_idx].resolved_targets;
    for (size_t target_k : blocks[disp_b].addr_vec_targets) {
      if (target_k < num_blocks &&
          std::find(blocks[disp_b].successors.begin(), blocks[disp_b].successors.end(), target_k) ==
              blocks[disp_b].successors.end()) {
        blocks[disp_b].successors.push_back(target_k);
      }
    }
  };

  auto run_dataflow_and_bind = [&](bool seed_all_blocks) -> bool {
    std::vector<bool> reachable(num_blocks, false);
    std::vector<RegJtTaint> in_taint(num_blocks);
    std::vector<RegJtTaint> pre_jump_taint(num_blocks);
    std::queue<size_t> worklist;
    std::vector<bool> in_queue(num_blocks, false);

    if (seed_all_blocks) {
      for (size_t b = 0; b < num_blocks; ++b) {
        reachable[b] = true;
        in_queue[b] = true;
        worklist.push(b);
      }
    } else {
      reachable[0] = true;
      in_queue[0] = true;
      worklist.push(0);
    }

    auto propagate = [&](size_t succ, const RegJtTaint& src) {
      if (succ >= num_blocks) {
        return;
      }
      bool changed = !reachable[succ];
      reachable[succ] = true;
      for (size_t r = 1; r < 32; ++r) {
        for (size_t jt_i : src[r]) {
          if (in_taint[succ][r].insert(jt_i).second) {
            changed = true;
          }
        }
      }
      if (changed && !in_queue[succ]) {
        in_queue[succ] = true;
        worklist.push(succ);
      }
    };

    while (!worklist.empty()) {
      const size_t b = worklist.front();
      worklist.pop();
      in_queue[b] = false;

      RegJtTaint cur_state = in_taint[b];
      const auto& p_insns = parsed_blocks[b];
      if (p_insns.empty()) {
        for (size_t succ : blocks[b].successors) {
          propagate(succ, cur_state);
        }
        continue;
      }

      for (size_t i = 0; i + 1 < p_insns.size(); ++i) {
        if (p_insns[i].has_value()) {
          StepSinglePatternJtTaint(*p_insns[i], &cur_state, token_to_jt_idx);
        }
      }

      if (!p_insns.back().has_value()) {
        pre_jump_taint[b] = cur_state;
        for (size_t succ : blocks[b].successors) {
          propagate(succ, cur_state);
        }
        continue;
      }

      const RtlExpr& last_pat = *p_insns.back();
      if (last_pat.op == RtlOp::kSequence && last_pat.children.size() == 2 &&
          !last_pat.children[0].children.empty() && !last_pat.children[1].children.empty()) {
        const RtlExpr& wrap0 = last_pat.children[0];
        const RtlExpr& wrap1 = last_pat.children[1];
        const RtlExpr& ctrl = wrap0.children[0];
        const RtlExpr& slot = wrap1.children[0];
        if (wrap0.op == RtlOp::kCallInsn) {
          StepSinglePatternJtTaint(slot, &cur_state, token_to_jt_idx);
          StepSinglePatternJtTaint(ctrl, &cur_state, token_to_jt_idx);
          pre_jump_taint[b] = cur_state;
          for (size_t succ : blocks[b].successors) {
            propagate(succ, cur_state);
          }
        } else {
          pre_jump_taint[b] = cur_state;
          StepSinglePatternJtTaint(ctrl, &cur_state, token_to_jt_idx);
          if (wrap1.flags.in_struct && blocks[b].successors.size() == 2) {
            RegJtTaint taken_state = cur_state;
            StepSinglePatternJtTaint(slot, &taken_state, token_to_jt_idx);
            propagate(blocks[b].successors[0], taken_state);
            propagate(blocks[b].successors[1], cur_state);
          } else {
            StepSinglePatternJtTaint(slot, &cur_state, token_to_jt_idx);
            for (size_t succ : blocks[b].successors) {
              propagate(succ, cur_state);
            }
          }
        }
      } else {
        pre_jump_taint[b] = cur_state;
        StepSinglePatternJtTaint(last_pat, &cur_state, token_to_jt_idx);
        for (size_t succ : blocks[b].successors) {
          propagate(succ, cur_state);
        }
      }
    }

    bool any_bound = false;
    for (size_t b = 0; b < num_blocks; ++b) {
      if (block_bound[b] || !reachable[b] || indirect_jump_reg[b] <= 0) {
        continue;
      }
      const int jreg = indirect_jump_reg[b];
      std::vector<size_t> cands;
      for (size_t jt_i : pre_jump_taint[b][jreg]) {
        if (!jt_bound_block[jt_i].has_value()) {
          cands.push_back(jt_i);
        }
      }
      if (cands.size() == 1) {
        bind_jt_to_block(cands[0], b);
        any_bound = true;
      }
    }
    return any_bound;
  };

  while (run_dataflow_and_bind(/*seed_all_blocks=*/false)) {
  }
  while (run_dataflow_and_bind(/*seed_all_blocks=*/true)) {
  }

  for (size_t jt_idx = 0; jt_idx < valid_jts.size(); ++jt_idx) {
    if (!jt_bound_block[jt_idx].has_value()) {
      continue;
    }
    const size_t disp_b = *jt_bound_block[jt_idx];
    const std::string tbl_lbl = absl::StrFormat("__tbl_%zu", disp_b);
    for (size_t b = 0; b < num_blocks; ++b) {
      bool modified_blk = false;
      for (std::string& pat : blocks[b].instructions) {
        auto parsed = ParsePatternAndNotes(pat);
        if (!parsed.ok()) {
          continue;
        }
        bool modified_insn = false;
        RewriteSpecificLabelRefInAst(&parsed->pattern, valid_jts[jt_idx].table_label_token, tbl_lbl,
                                     &modified_insn);
        if (modified_insn) {
          pat = parsed->ToGccPrintRtlString();
          modified_blk = true;
        }
      }
      if (modified_blk) {
        blocks[b].SyncRtlInsnsFromInstructions();
      }
    }
  }
}

// In KMC GCC 2.7.2 `mips.md`, 3-operand `divsi3`/`modsi3`/`udivsi3`/`umodsi3`
// (and `DI` counterparts) have condition `"!optimize"`, whereas `optimize > 0`
// exclusively uses `divmodsi4`/`udivmodsi4` (`divmoddi4`/`udivmoddi4`) with a
// `REG_UNUSED` note on the unused output (or fused with an adjacent `mfhi`).
void UpgradeDivModInsnsForOptimize(RtlControlFlowGraph* cfg) {
  for (RtlBasicBlock& blk : cfg->rpo_blocks) {
    std::vector<std::string> upgraded;
    upgraded.reserve(blk.instructions.size());
    bool changed = false;
    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      auto parsed = ParsePatternAndNotes(blk.instructions[i]);
      if (!parsed.ok() || !parsed->pattern.IsParallel() || parsed->pattern.children.size() != 4 ||
          !parsed->pattern.children[0].IsSet()) {
        upgraded.push_back(blk.instructions[i]);
        continue;
      }
      const RtlExpr& set0 = parsed->pattern.children[0];
      const RtlExpr& dst = set0.SetDest();
      const RtlExpr& src = set0.SetSrc();
      const RtlOp op = src.op;
      const bool is_signed_divmod = (op == RtlOp::kDiv || op == RtlOp::kMod);
      const bool is_unsigned_divmod = (op == RtlOp::kUdiv || op == RtlOp::kUmod);
      if ((!is_signed_divmod && !is_unsigned_divmod) ||
          (src.mode != RtlMachineMode::kSi && src.mode != RtlMachineMode::kDi) ||
          src.children.size() != 2 || !dst.IsReg() || !src.children[0].IsReg() ||
          !src.children[1].IsReg()) {
        upgraded.push_back(blk.instructions[i]);
        continue;
      }

      const RtlMachineMode mode = src.mode;
      const RtlOp div_op = is_signed_divmod ? RtlOp::kDiv : RtlOp::kUdiv;
      const RtlOp mod_op = is_signed_divmod ? RtlOp::kMod : RtlOp::kUmod;
      const bool is_quotient = (op == RtlOp::kDiv || op == RtlOp::kUdiv);
      const RtlExpr& lhs = src.children[0];
      const RtlExpr& rhs = src.children[1];

      if (is_quotient && i + 1 < blk.instructions.size()) {
        auto next_parsed = ParsePatternAndNotes(blk.instructions[i + 1]);
        if (next_parsed.ok() && next_parsed->pattern.IsSet() &&
            next_parsed->pattern.SetDest().IsReg() && next_parsed->pattern.SetDest().mode == mode &&
            next_parsed->pattern.SetDest().RegNo() < 32 &&
            next_parsed->pattern.SetDest().RegNo() != dst.RegNo() &&
            next_parsed->pattern.SetSrc().IsReg() && next_parsed->pattern.SetSrc().RegNo() == 64) {
          const RtlExpr& rem_dst = next_parsed->pattern.SetDest();
          RtlExpr fused = RtlExpr::MakeParallel({
              RtlExpr::MakeSet(dst, RtlExpr::MakeBinary(div_op, mode, lhs, rhs)),
              RtlExpr::MakeSet(rem_dst, RtlExpr::MakeBinary(mod_op, mode, lhs, rhs)),
              RtlExpr::MakeClobber(RtlExpr::MakeReg(mode, 65)),
              RtlExpr::MakeClobber(RtlExpr::MakeReg(mode, 64)),
              RtlExpr::MakeClobber(RtlExpr::MakeReg(mode, 66)),
          });
          std::vector<RtlRegNote> notes = parsed->reg_notes;
          notes.insert(notes.end(), next_parsed->reg_notes.begin(), next_parsed->reg_notes.end());
          upgraded.push_back(FormatGccPrintRtlPatternAndNotes(fused, notes, parsed->bracketed_notes,
                                                              /*call_fusage=*/{}));
          changed = true;
          ++i;
          continue;
        }
      }

      const RtlExpr scratch_at = RtlExpr::MakeReg(mode, 1);
      const RtlExpr q_dst = is_quotient ? dst : scratch_at;
      const RtlExpr r_dst = is_quotient ? scratch_at : dst;
      RtlExpr divmod = RtlExpr::MakeParallel({
          RtlExpr::MakeSet(q_dst, RtlExpr::MakeBinary(div_op, mode, lhs, rhs)),
          RtlExpr::MakeSet(r_dst, RtlExpr::MakeBinary(mod_op, mode, lhs, rhs)),
          RtlExpr::MakeClobber(RtlExpr::MakeReg(mode, 65)),
          RtlExpr::MakeClobber(RtlExpr::MakeReg(mode, 64)),
          RtlExpr::MakeClobber(RtlExpr::MakeReg(mode, 66)),
      });
      std::vector<RtlRegNote> notes = parsed->reg_notes;
      notes.push_back(RtlRegNote{
          .kind = RtlMachineMode::kRegUnused,
          .list_op = RtlOp::kExprList,
          .datum = scratch_at,
      });
      upgraded.push_back(FormatGccPrintRtlPatternAndNotes(divmod, notes, parsed->bracketed_notes,
                                                          /*call_fusage=*/{}));
      changed = true;
    }
    if (changed) {
      blk.instructions = std::move(upgraded);
      blk.SyncRtlInsnsFromInstructions();
    }
  }
}

bool IsMipsCpuR4000(const CompilationProfile& profile) {
  int mips_isa = 3;
  std::string mcpu;
  for (size_t i = 0; i < profile.c_flags.size(); ++i) {
    const std::string& f = profile.c_flags[i];
    if (f == "-mips1") {
      mips_isa = 1;
    } else if (f == "-mips2") {
      mips_isa = 2;
    } else if (f == "-mips3") {
      mips_isa = 3;
    } else if (f == "-mips4") {
      mips_isa = 4;
    } else if (absl::StartsWith(f, "-mcpu=")) {
      mcpu = f.substr(6);
    } else if (f == "-mcpu" && i + 1 < profile.c_flags.size()) {
      mcpu = profile.c_flags[++i];
    }
  }
  if (mcpu.empty() || mcpu == "default" || mcpu == "DEFAULT") {
    return mips_isa == 3;
  }
  std::string_view p = mcpu;
  if (!p.empty() && (p[0] == 'r' || p[0] == 'R')) {
    p.remove_prefix(1);
  }
  return p == "4000" || p == "4k" || p == "4K" || p == "4400";
}

// On `mips_cpu == PROCESSOR_R4000` (`mips.md:860-881`), 32-bit signed/unsigned
// integer multiplication (`mulsi3`) is emitted as the single RTL instruction
// `mulsi3_r4000`, whose output template emits consecutive `mult $rs,$rt` +
// `mflo $rd`. Fuse consecutive `mult` + `mflo` pairs back into `mulsi3_r4000`
// whenever neither `hi` (`reg 64`) nor `lo` (`reg 65`) is read afterwards.
void UpgradeR4000Mulsi3Insns(RtlControlFlowGraph* cfg) {
  auto analyze_hilo_expr = [](const RtlExpr& expr, uint32_t* reads, uint32_t* kills) {
    *reads = 0u;
    *kills = 0u;
    auto hilo_bit = [](int r) -> uint32_t { return (r == 64) ? 1u : ((r == 65) ? 2u : 0u); };
    auto scan_reads = [&](const RtlExpr& sub) {
      sub.Visit([&](const RtlExpr& n) {
        if (n.IsReg()) {
          *reads |= hilo_bit(n.RegNo());
        }
      });
    };
    auto process_elem = [&](const RtlExpr& elem) {
      if (elem.IsSet()) {
        if (elem.SetDest().IsMem()) {
          scan_reads(elem.SetDest());
        }
        scan_reads(elem.SetSrc());
        if (elem.SetSrc().op == RtlOp::kCall) {
          *kills |= 3u;
        } else if (elem.SetDest().IsReg()) {
          *kills |= hilo_bit(elem.SetDest().RegNo());
          if (elem.SetDest().RegNo() == 64 && (elem.SetDest().mode == RtlMachineMode::kDi ||
                                               elem.SetDest().mode == RtlMachineMode::kTi)) {
            *kills |= 3u;
          }
        }
      } else if (elem.op == RtlOp::kCall) {
        scan_reads(elem);
        *kills |= 3u;
      } else if (elem.op == RtlOp::kClobber) {
        if (!elem.children.empty() && elem.children[0].IsReg()) {
          *kills |= hilo_bit(elem.children[0].RegNo());
        }
      } else {
        scan_reads(elem);
      }
    };
    if (expr.IsParallel()) {
      for (const RtlExpr& c : expr.children) {
        process_elem(c);
      }
    } else {
      process_elem(expr);
    }
  };

  auto hilo_read_after = [&](size_t start_b, size_t start_i) -> bool {
    using State = std::tuple<size_t, size_t, uint32_t>;
    std::queue<State> q;
    absl::flat_hash_set<State> visited;
    q.emplace(start_b, start_i, 3u);
    visited.emplace(start_b, start_i, 3u);
    while (!q.empty()) {
      auto [cb, ci, alive] = q.front();
      q.pop();
      if (alive == 0u || cb >= cfg->rpo_blocks.size()) {
        continue;
      }
      const auto& cblk = cfg->rpo_blocks[cb];
      for (size_t idx = ci; idx < cblk.instructions.size() && alive != 0u; ++idx) {
        auto cp = ParsePatternAndNotes(cblk.instructions[idx]);
        if (!cp.ok()) {
          continue;
        }
        if (cp->pattern.op == RtlOp::kSequence && cp->pattern.children.size() == 2 &&
            !cp->pattern.children[0].children.empty() &&
            !cp->pattern.children[1].children.empty()) {
          uint32_t r0 = 0, k0 = 0, r1 = 0, k1 = 0;
          analyze_hilo_expr(cp->pattern.children[0].children[0], &r0, &k0);
          analyze_hilo_expr(cp->pattern.children[1].children[0], &r1, &k1);
          if (cp->pattern.children[0].op == RtlOp::kCallInsn) {
            if ((r1 & alive) != 0u) return true;
            alive &= ~k1;
            if ((r0 & alive) != 0u) return true;
            alive = 0u;
          } else {
            if ((r0 & alive) != 0u) return true;
            alive &= ~k0;
            if ((r1 & alive) != 0u) return true;
            alive &= ~k1;
          }
        } else {
          uint32_t rm = 0, km = 0;
          analyze_hilo_expr(cp->pattern, &rm, &km);
          if ((rm & alive) != 0u) return true;
          alive &= ~km;
        }
      }
      if (alive == 0u) {
        continue;
      }
      for (size_t succ : cblk.successors) {
        if (visited.emplace(succ, 0, alive).second) {
          q.emplace(succ, 0, alive);
        }
      }
    }
    return false;
  };

  for (size_t b = 0; b < cfg->rpo_blocks.size(); ++b) {
    RtlBasicBlock& blk = cfg->rpo_blocks[b];
    std::vector<std::string> upgraded;
    upgraded.reserve(blk.instructions.size());
    bool changed = false;
    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      if (i + 1 >= blk.instructions.size()) {
        upgraded.push_back(blk.instructions[i]);
        continue;
      }
      auto p0 = ParsePatternAndNotes(blk.instructions[i]);
      auto p1 = ParsePatternAndNotes(blk.instructions[i + 1]);
      int rs = -1;
      int rt = -1;
      int rd = -1;
      if (p0.ok() && p1.ok() && p0->pattern.IsParallel() && p0->pattern.children.size() == 3 &&
          p0->pattern.children[0].IsSet() && p0->pattern.children[0].SetDest().IsReg() &&
          p0->pattern.children[0].SetDest().RegNo() == 64 &&
          p0->pattern.children[0].SetDest().mode == RtlMachineMode::kSi &&
          p0->pattern.children[0].SetSrc().op == RtlOp::kTruncate &&
          p0->pattern.children[0].SetSrc().children.size() == 1 &&
          p0->pattern.children[0].SetSrc().children[0].op == RtlOp::kLshiftrt &&
          p0->pattern.children[0].SetSrc().children[0].children.size() == 2 &&
          p0->pattern.children[0].SetSrc().children[0].children[0].op == RtlOp::kMult &&
          p0->pattern.children[0].SetSrc().children[0].children[0].children.size() == 2) {
        const RtlExpr& ext0 = p0->pattern.children[0].SetSrc().children[0].children[0].children[0];
        const RtlExpr& ext1 = p0->pattern.children[0].SetSrc().children[0].children[0].children[1];
        if (ext0.op == RtlOp::kSignExtend && ext1.op == RtlOp::kSignExtend &&
            ext0.children.size() == 1 && ext1.children.size() == 1 && ext0.children[0].IsReg() &&
            ext1.children[0].IsReg() && ext0.children[0].mode == RtlMachineMode::kSi &&
            ext1.children[0].mode == RtlMachineMode::kSi) {
          rs = ext0.children[0].RegNo();
          rt = ext1.children[0].RegNo();
        }
      }
      if (p1.ok() && p1->pattern.IsSet() && p1->pattern.SetDest().IsReg() &&
          p1->pattern.SetDest().mode == RtlMachineMode::kSi && p1->pattern.SetDest().RegNo() >= 1 &&
          p1->pattern.SetDest().RegNo() < 32 && p1->pattern.SetSrc().IsReg() &&
          p1->pattern.SetSrc().RegNo() == 65 && p1->pattern.SetSrc().mode == RtlMachineMode::kSi) {
        rd = p1->pattern.SetDest().RegNo();
      }
      if (rs >= 0 && rs < 32 && rt >= 0 && rt < 32 && rd >= 1 && !hilo_read_after(b, i + 2)) {
        auto make_si_gpr = [](int r) {
          RtlExpr reg = RtlExpr::MakeReg(RtlMachineMode::kSi, r);
          if (r == 30) {
            reg.str_val = "$fp";
          }
          return reg;
        };
        RtlExpr fused = RtlExpr::MakeParallel({
            RtlExpr::MakeSet(make_si_gpr(rd),
                             RtlExpr::MakeBinary(RtlOp::kMult, RtlMachineMode::kSi, make_si_gpr(rs),
                                                 make_si_gpr(rt))),
            RtlExpr::MakeClobber(RtlExpr::MakeReg(RtlMachineMode::kSi, 64)),
            RtlExpr::MakeClobber(RtlExpr::MakeReg(RtlMachineMode::kSi, 65)),
            RtlExpr::MakeClobber(RtlExpr::MakeReg(RtlMachineMode::kSi, 66)),
        });
        std::vector<RtlRegNote> notes = p0->reg_notes;
        notes.insert(notes.end(), p1->reg_notes.begin(), p1->reg_notes.end());
        upgraded.push_back(FormatGccPrintRtlPatternAndNotes(fused, notes, p0->bracketed_notes,
                                                            /*call_fusage=*/{}));
        changed = true;
        ++i;
        continue;
      }
      upgraded.push_back(blk.instructions[i]);
    }
    if (changed) {
      blk.instructions = std::move(upgraded);
      blk.SyncRtlInsnsFromInstructions();
    }
  }
}

// When a single basic block contains two `(set (reg:SI R) (const_int C))`
// instructions with the same register `R` and constant `C` without an
// intervening overwrite of `R`, `jump2` (`jump.c:delete_noop_moves`) only
// preserved the second load because their destination modes differed (`QI` vs
// `HI` vs `SI`). Recover the narrower `QI`/`HI` destination mode on each such
// load when all reads before `R`'s next overwrite in the block use that mode.
void NarrowDuplicateConstLoadsInBlocks(RtlControlFlowGraph* cfg) {
  auto is_caller_saved_gpr = [](int r) {
    return (r >= 1 && r <= 15) || r == 24 || r == 25 || r == 31;
  };
  auto analyze_reg_in_expr = [&](const RtlExpr& expr, int target_r,
                                 std::vector<RtlMachineMode>* read_modes, bool* killed) {
    auto scan_reads = [&](const RtlExpr& sub) {
      sub.Visit([&](const RtlExpr& n) {
        if (n.IsReg() && n.RegNo() == target_r) {
          read_modes->push_back(n.mode);
        }
      });
    };
    auto process_elem = [&](const RtlExpr& elem) {
      if (elem.IsSet()) {
        if (elem.SetDest().IsMem()) {
          scan_reads(elem.SetDest());
        }
        scan_reads(elem.SetSrc());
        if (elem.SetSrc().op == RtlOp::kCall && is_caller_saved_gpr(target_r)) {
          *killed = true;
        }
        if (elem.SetDest().IsReg()) {
          const int dr = elem.SetDest().RegNo();
          if (dr == target_r ||
              (elem.SetDest().mode == RtlMachineMode::kDi && dr + 1 == target_r)) {
            *killed = true;
          }
        }
      } else if (elem.op == RtlOp::kCall) {
        scan_reads(elem);
        if (is_caller_saved_gpr(target_r)) {
          *killed = true;
        }
      } else if (elem.op == RtlOp::kClobber) {
        if (!elem.children.empty() && elem.children[0].IsReg() &&
            elem.children[0].RegNo() == target_r) {
          *killed = true;
        }
      } else {
        scan_reads(elem);
      }
    };
    if (expr.op == RtlOp::kSequence && expr.children.size() == 2 &&
        !expr.children[0].children.empty() && !expr.children[1].children.empty()) {
      if (expr.children[0].op == RtlOp::kCallInsn) {
        process_elem(expr.children[1].children[0]);
        if (!*killed) {
          process_elem(expr.children[0].children[0]);
        }
      } else {
        process_elem(expr.children[0].children[0]);
        if (!*killed) {
          process_elem(expr.children[1].children[0]);
        }
      }
    } else if (expr.IsParallel()) {
      for (const RtlExpr& c : expr.children) {
        process_elem(c);
      }
    } else {
      process_elem(expr);
    }
  };

  for (RtlBasicBlock& blk : cfg->rpo_blocks) {
    const size_t n = blk.instructions.size();
    std::vector<RtlPatternAndNotes> parsed(n);
    std::vector<bool> ok(n, false);
    for (size_t i = 0; i < n; ++i) {
      auto p = ParsePatternAndNotes(blk.instructions[i]);
      if (p.ok()) {
        parsed[i] = std::move(*p);
        ok[i] = true;
      }
    }
    auto is_si_const_load = [&](size_t idx, int* out_r, int64_t* out_c) -> bool {
      if (!ok[idx] || !parsed[idx].pattern.IsSet() || !parsed[idx].pattern.SetDest().IsReg() ||
          parsed[idx].pattern.SetDest().mode != RtlMachineMode::kSi ||
          !parsed[idx].pattern.SetSrc().IsConstInt()) {
        return false;
      }
      const int r = parsed[idx].pattern.SetDest().RegNo();
      if (r < 1 || r >= 32) {
        return false;
      }
      *out_r = r;
      *out_c = parsed[idx].pattern.SetSrc().int_val;
      return true;
    };

    std::vector<bool> is_dup_candidate(n, false);
    for (size_t i = 0; i < n; ++i) {
      int r_i = -1;
      int64_t c_i = 0;
      if (!is_si_const_load(i, &r_i, &c_i)) {
        continue;
      }
      for (size_t j = i + 1; j < n; ++j) {
        if (!ok[j]) {
          continue;
        }
        int r_j = -1;
        int64_t c_j = 0;
        if (is_si_const_load(j, &r_j, &c_j) && r_j == r_i && c_j == c_i) {
          is_dup_candidate[i] = true;
          is_dup_candidate[j] = true;
          break;
        }
        std::vector<RtlMachineMode> dummy_reads;
        bool killed = false;
        analyze_reg_in_expr(parsed[j].pattern, r_i, &dummy_reads, &killed);
        if (killed) {
          break;
        }
      }
    }

    bool changed = false;
    for (size_t i = 0; i < n; ++i) {
      if (!is_dup_candidate[i]) {
        continue;
      }
      int r_i = -1;
      int64_t c_i = 0;
      if (!is_si_const_load(i, &r_i, &c_i)) {
        continue;
      }
      std::vector<RtlMachineMode> read_modes;
      for (size_t j = i + 1; j < n; ++j) {
        if (!ok[j]) {
          continue;
        }
        bool killed = false;
        analyze_reg_in_expr(parsed[j].pattern, r_i, &read_modes, &killed);
        if (killed) {
          break;
        }
      }
      if (read_modes.empty()) {
        continue;
      }
      const RtlMachineMode m0 = read_modes[0];
      const bool all_same = std::all_of(read_modes.begin(), read_modes.end(),
                                        [&](RtlMachineMode m) { return m == m0; });
      if (!all_same) {
        continue;
      }
      if ((m0 == RtlMachineMode::kQi && c_i >= -128 && c_i <= 255) ||
          (m0 == RtlMachineMode::kHi && c_i >= -32768 && c_i <= 65535)) {
        parsed[i].pattern.MutableSetDest().mode = m0;
        blk.instructions[i] = parsed[i].ToGccPrintRtlString();
        changed = true;
      }
    }
    if (changed) {
      blk.SyncRtlInsnsFromInstructions();
    }
  }
}

// Reverses `final.c:cleanup_subreg_operands` on multi-use sub-word register
// webs loaded by `movqi_internal2`/`movhi_internal2` (`(set (reg:QI/HI r)
// (mem:QI/HI addr))`), zero-extended into another register via
// `zero_extendqisi2`/`zero_extendhisi2` (`(set (reg:SI d) (zero_extend:SI
// (reg:QI/HI r)))`, assembled as `andi $d, $r, 0xff|0xffff`), and consumed via
// `(subreg:SI (reg:QI/HI r) 0)` in downstream ALU instructions.
void NarrowMultiUseSubwordZeroExtendWebs(RtlControlFlowGraph* cfg) {
  const size_t nb = cfg->rpo_blocks.size();
  if (nb == 0) {
    return;
  }
  std::vector<std::vector<size_t>> preds(nb);
  std::vector<std::vector<RtlPatternAndNotes>> parsed(nb);
  std::vector<std::vector<bool>> ok(nb);
  for (size_t b = 0; b < nb; ++b) {
    for (size_t s : cfg->rpo_blocks[b].successors) {
      if (s < nb) {
        preds[s].push_back(b);
      }
    }
    const size_t n = cfg->rpo_blocks[b].instructions.size();
    parsed[b].resize(n);
    ok[b].assign(n, false);
    for (size_t i = 0; i < n; ++i) {
      auto p = ParsePatternAndNotes(cfg->rpo_blocks[b].instructions[i]);
      if (p.ok()) {
        parsed[b][i] = std::move(*p);
        ok[b][i] = true;
      }
    }
  }

  auto is_caller_saved_gpr = [](int r) {
    return (r >= 1 && r <= 15) || r == 24 || r == 25 || r == 31;
  };
  auto step_elem = [&](RtlExpr* elem, int r, RtlMachineMode mem_mode, int64_t full_mask,
                       bool mutate, bool* reads_r, bool* kills_r, bool* invalid) {
    auto check_bad_sub = [&](const RtlExpr& sub, bool in_mem) {
      sub.Visit([&](const RtlExpr& n) {
        if (n.op == RtlOp::kUnspecVolatile || n.op == RtlOp::kAsmInput) {
          *invalid = true;
        }
        if (n.IsReg()) {
          const int nr = n.RegNo();
          if ((n.mode == RtlMachineMode::kDi || n.mode == RtlMachineMode::kDf) &&
              (nr == r || nr + 1 == r)) {
            *invalid = true;
          } else if (nr == r) {
            if (in_mem || n.flags.volatil || n.flags.integrated ||
                (n.mode != RtlMachineMode::kSi && n.mode != mem_mode)) {
              *invalid = true;
            }
          }
        }
      });
    };
    auto check_all_mems = [&](const RtlExpr& sub) {
      sub.Visit([&](const RtlExpr& n) {
        if (n.IsMem()) {
          check_bad_sub(n, /*in_mem=*/true);
        }
      });
    };
    auto rewrite_reads = [&](const auto& self, RtlExpr* node) -> void {
      if (node->IsMem()) {
        return;
      }
      if (node->IsReg()) {
        if (node->RegNo() == r) {
          *reads_r = true;
          if (mutate && node->mode == RtlMachineMode::kSi) {
            RtlExpr narrow = *node;
            narrow.mode = mem_mode;
            *node = RtlExpr::MakeSubreg(RtlMachineMode::kSi, std::move(narrow), 0);
          }
        }
        return;
      }
      if (node->IsSubreg() && !node->children.empty() && node->children[0].IsReg() &&
          node->children[0].RegNo() == r) {
        *reads_r = true;
        if (node->children[0].mode != mem_mode) {
          *invalid = true;
        }
        return;
      }
      for (RtlExpr& ch : node->children) {
        self(self, &ch);
      }
    };

    check_bad_sub(*elem, /*in_mem=*/false);
    check_all_mems(*elem);
    if (*invalid) {
      return;
    }
    if (elem->op == RtlOp::kCall || (elem->IsSet() && elem->SetSrc().op == RtlOp::kCall)) {
      elem->Visit([&](const RtlExpr& n) {
        if (n.IsReg() && n.RegNo() == r) {
          *invalid = true;
        }
      });
      if (is_caller_saved_gpr(r)) {
        *kills_r = true;
      }
      return;
    }
    if (elem->IsSet()) {
      if (elem->SetDest().op == RtlOp::kPc && elem->SetSrc().IsReg() &&
          elem->SetSrc().RegNo() == r) {
        *invalid = true;
        return;
      }
      RtlExpr& src = elem->MutableSetSrc();
      if (src.op == RtlOp::kAnd && src.mode == RtlMachineMode::kSi && src.children.size() == 2 &&
          src.children[0].IsReg() && src.children[0].RegNo() == r &&
          src.children[0].mode == RtlMachineMode::kSi && src.children[1].IsConstInt() &&
          src.children[1].int_val == full_mask) {
        *reads_r = true;
        if (mutate) {
          RtlExpr narrow = src.children[0];
          narrow.mode = mem_mode;
          src = RtlExpr::MakeUnary(RtlOp::kZeroExtend, RtlMachineMode::kSi, std::move(narrow));
        }
      } else {
        rewrite_reads(rewrite_reads, &src);
      }
      if (elem->SetDest().IsReg() && elem->SetDest().RegNo() == r) {
        *kills_r = true;
      }
      return;
    }
    if (elem->op == RtlOp::kClobber) {
      if (!elem->children.empty() && elem->children[0].IsReg() && elem->children[0].RegNo() == r) {
        *kills_r = true;
      }
      return;
    }
    rewrite_reads(rewrite_reads, elem);
  };

  auto step_insn = [&](RtlExpr* pat, int r, RtlMachineMode mem_mode, int64_t full_mask, bool mutate,
                       bool* reads_before_kill, bool* kills_all_paths, bool* invalid) {
    *reads_before_kill = false;
    *kills_all_paths = false;
    *invalid = false;
    auto run_one = [&](RtlExpr* e, bool* rd, bool* kl) {
      *rd = false;
      *kl = false;
      if (e->IsParallel()) {
        for (RtlExpr& ch : e->children) {
          bool cr = false;
          bool ck = false;
          step_elem(&ch, r, mem_mode, full_mask, mutate, &cr, &ck, invalid);
          if (cr && !*kl) *rd = true;
          if (ck) *kl = true;
        }
      } else {
        step_elem(e, r, mem_mode, full_mask, mutate, rd, kl, invalid);
      }
    };
    if (pat->op == RtlOp::kSequence && pat->children.size() == 2 &&
        !pat->children[0].children.empty() && !pat->children[1].children.empty()) {
      const bool call_head = (pat->children[0].op == RtlOp::kCallInsn);
      const bool annulled_untaken =
          !call_head && pat->children[0].flags.unchanging && pat->children[1].flags.in_struct;
      RtlExpr* first_e = call_head ? &pat->children[1].children[0] : &pat->children[0].children[0];
      RtlExpr* second_e = call_head ? &pat->children[0].children[0] : &pat->children[1].children[0];
      bool r1 = false, k1 = false;
      run_one(first_e, &r1, &k1);
      if (r1) *reads_before_kill = true;
      if (k1) {
        *kills_all_paths = true;
        return;
      }
      bool r2 = false, k2 = false;
      run_one(second_e, &r2, &k2);
      if (r2) *reads_before_kill = true;
      if (k2) {
        if (annulled_untaken) {
          *invalid = true;
        } else {
          *kills_all_paths = true;
        }
      }
      return;
    }
    run_one(pat, reads_before_kill, kills_all_paths);
  };

  std::vector<bool> blk_dirty(nb, false);
  for (size_t b0 = 0; b0 < nb; ++b0) {
    const size_t n0 = parsed[b0].size();
    for (size_t i0 = 0; i0 + 1 < n0; ++i0) {
      if (!ok[b0][i0] || !parsed[b0][i0].pattern.IsSet()) {
        continue;
      }
      const RtlExpr& dst_i = parsed[b0][i0].pattern.SetDest();
      const RtlExpr& src_i = parsed[b0][i0].pattern.SetSrc();
      if (!dst_i.IsReg() || dst_i.mode != RtlMachineMode::kSi || dst_i.flags.volatil ||
          dst_i.flags.integrated || src_i.op != RtlOp::kZeroExtend ||
          src_i.mode != RtlMachineMode::kSi || src_i.children.size() != 1 ||
          !src_i.children[0].IsMem() || src_i.children[0].flags.volatil) {
        continue;
      }
      const int r = dst_i.RegNo();
      const RtlMachineMode mem_mode = src_i.children[0].mode;
      if (r < 2 || r > 25 || (mem_mode != RtlMachineMode::kQi && mem_mode != RtlMachineMode::kHi)) {
        continue;
      }
      const int64_t full_mask = (mem_mode == RtlMachineMode::kQi) ? 255 : 65535;

      bool found_zext_copy = false;
      for (size_t j = i0 + 1; j < n0; ++j) {
        if (!ok[b0][j]) {
          break;
        }
        const RtlExpr& pj = parsed[b0][j].pattern;
        if (pj.IsSet() && pj.SetDest().IsReg() && !pj.SetDest().flags.volatil &&
            pj.SetDest().mode == RtlMachineMode::kSi && pj.SetDest().RegNo() != r &&
            pj.SetSrc().op == RtlOp::kAnd && pj.SetSrc().mode == RtlMachineMode::kSi &&
            pj.SetSrc().children.size() == 2 && pj.SetSrc().children[0].IsReg() &&
            pj.SetSrc().children[0].RegNo() == r &&
            pj.SetSrc().children[0].mode == RtlMachineMode::kSi &&
            pj.SetSrc().children[1].IsConstInt() && pj.SetSrc().children[1].int_val == full_mask) {
          const int d = pj.SetDest().RegNo();
          if ((d >= 2 && d <= 25) || (d == 30 && !cfg->sideband.frame_pointer_needed)) {
            found_zext_copy = true;
            break;
          }
        }
        bool rd = false, kl = false, inv = false;
        RtlExpr probe = pj;
        step_insn(&probe, r, mem_mode, full_mask, /*mutate=*/false, &rd, &kl, &inv);
        if (kl || inv) {
          break;
        }
      }
      if (!found_zext_copy) {
        continue;
      }

      std::vector<bool> blk_gen(nb, false);
      std::vector<bool> blk_kill(nb, false);
      bool any_parse_fail = false;
      for (size_t b = 0; b < nb; ++b) {
        bool killed_so_far = false;
        for (size_t i = 0; i < parsed[b].size(); ++i) {
          if (!ok[b][i]) {
            any_parse_fail = true;
            break;
          }
          bool rd = false, kl = false, inv = false;
          RtlExpr probe = parsed[b][i].pattern;
          step_insn(&probe, r, mem_mode, full_mask, /*mutate=*/false, &rd, &kl, &inv);
          if (!killed_so_far && rd) {
            blk_gen[b] = true;
          }
          if (kl) {
            killed_so_far = true;
          }
        }
        blk_kill[b] = killed_so_far;
      }
      if (any_parse_fail) {
        continue;
      }

      std::vector<bool> live_in(nb, false);
      bool changed_df = true;
      while (changed_df) {
        changed_df = false;
        for (size_t rev_b = nb; rev_b > 0; --rev_b) {
          const size_t b = rev_b - 1;
          bool live_out = false;
          for (size_t s : cfg->rpo_blocks[b].successors) {
            if (s < nb && live_in[s]) {
              live_out = true;
            }
          }
          const bool new_in = blk_gen[b] || (!blk_kill[b] && live_out);
          if (new_in != live_in[b]) {
            live_in[b] = new_in;
            changed_df = true;
          }
        }
      }

      bool valid_web = true;
      bool killed_in_b0_tail = false;
      int other_subreg_reads = 0;
      auto check_other_reads = [&](const RtlExpr& pat) {
        pat.Visit([&](const RtlExpr& n) {
          if (n.IsSubreg() && !n.children.empty() && n.children[0].IsReg() &&
              n.children[0].RegNo() == r && n.children[0].mode == mem_mode) {
            ++other_subreg_reads;
          }
        });
      };
      for (size_t j = i0 + 1; j < n0; ++j) {
        bool rd = false, kl = false, inv = false;
        RtlExpr probe = parsed[b0][j].pattern;
        step_insn(&probe, r, mem_mode, full_mask, /*mutate=*/true, &rd, &kl, &inv);
        if (inv) {
          valid_web = false;
          break;
        }
        check_other_reads(probe);
        if (kl) {
          killed_in_b0_tail = true;
          break;
        }
      }
      if (!valid_web) {
        continue;
      }

      std::vector<bool> in_web(nb, false);
      std::vector<bool> live_out_of_web_blk(nb, false);
      std::vector<size_t> q;
      if (!killed_in_b0_tail) {
        live_out_of_web_blk[b0] = true;
        for (size_t s : cfg->rpo_blocks[b0].successors) {
          if (s < nb && live_in[s]) {
            if (s == b0) {
              valid_web = false;
              break;
            }
            if (!in_web[s]) {
              in_web[s] = true;
              q.push_back(s);
            }
          }
        }
      }
      for (size_t qi = 0; valid_web && qi < q.size(); ++qi) {
        const size_t wb = q[qi];
        bool killed_wb = false;
        for (size_t j = 0; j < parsed[wb].size(); ++j) {
          bool rd = false, kl = false, inv = false;
          RtlExpr probe = parsed[wb][j].pattern;
          step_insn(&probe, r, mem_mode, full_mask, /*mutate=*/true, &rd, &kl, &inv);
          if (inv) {
            valid_web = false;
            break;
          }
          check_other_reads(probe);
          if (kl) {
            killed_wb = true;
            break;
          }
        }
        if (!valid_web) {
          break;
        }
        if (!killed_wb) {
          live_out_of_web_blk[wb] = true;
          for (size_t s : cfg->rpo_blocks[wb].successors) {
            if (s < nb && live_in[s]) {
              if (s == b0) {
                valid_web = false;
                break;
              }
              if (!in_web[s]) {
                in_web[s] = true;
                q.push_back(s);
              }
            }
          }
        }
      }
      for (size_t wb : q) {
        for (size_t p : preds[wb]) {
          if (p != b0 && !in_web[p]) {
            valid_web = false;
          }
          if (!live_out_of_web_blk[p]) {
            valid_web = false;
          }
        }
      }
      if (!valid_web || other_subreg_reads == 0) {
        continue;
      }

      RtlExpr mem_expr = parsed[b0][i0].pattern.SetSrc().children[0];
      parsed[b0][i0].pattern.MutableSetDest().mode = mem_mode;
      parsed[b0][i0].pattern.MutableSetSrc() = std::move(mem_expr);
      cfg->rpo_blocks[b0].instructions[i0] = parsed[b0][i0].ToGccPrintRtlString();
      blk_dirty[b0] = true;

      for (size_t j = i0 + 1; j < n0; ++j) {
        bool rd = false, kl = false, inv = false;
        step_insn(&parsed[b0][j].pattern, r, mem_mode, full_mask, /*mutate=*/true, &rd, &kl, &inv);
        if (rd) {
          cfg->rpo_blocks[b0].instructions[j] = parsed[b0][j].ToGccPrintRtlString();
          blk_dirty[b0] = true;
        }
        if (kl) {
          break;
        }
      }
      for (size_t wb : q) {
        for (size_t j = 0; j < parsed[wb].size(); ++j) {
          bool rd = false, kl = false, inv = false;
          step_insn(&parsed[wb][j].pattern, r, mem_mode, full_mask, /*mutate=*/true, &rd, &kl,
                    &inv);
          if (rd) {
            cfg->rpo_blocks[wb].instructions[j] = parsed[wb][j].ToGccPrintRtlString();
            blk_dirty[wb] = true;
          }
          if (kl) {
            break;
          }
        }
      }
    }
  }
  for (size_t b = 0; b < nb; ++b) {
    if (blk_dirty[b]) {
      cfg->rpo_blocks[b].SyncRtlInsnsFromInstructions();
    }
  }
}

// Reverses `final.c:cleanup_subreg_operands` (`alter_subreg`) on single-use
// non-volatile sub-word loads `(set (reg:QI/HI r) (mem:QI/HI addr))` feeding
// `(set (reg:SI d) (and:SI (subreg:SI (reg:QI/HI r) 0) (const_int C)))` with
// `0 < C < (QI ? 255 : 65535)` produced by `combine.c` (`force_to_mode`).
void NarrowSubwordMaskedLoadsInBlocks(RtlControlFlowGraph* cfg) {
  auto touches_gpr = [](const RtlExpr& expr, int target_r, int* read_cnt, bool* pure_kill,
                        bool* bad_ctrl_or_call) {
    *read_cnt = 0;
    *pure_kill = false;
    *bad_ctrl_or_call = false;
    if (expr.op == RtlOp::kSequence || IsReturnOrJumpPattern(expr)) {
      *bad_ctrl_or_call = true;
      return;
    }
    expr.Visit([&](const RtlExpr& n) {
      if (n.op == RtlOp::kCall || n.op == RtlOp::kUnspecVolatile || n.op == RtlOp::kAsmInput) {
        *bad_ctrl_or_call = true;
      }
    });
    if (*bad_ctrl_or_call) {
      return;
    }
    auto count_reads = [&](const RtlExpr& sub) {
      sub.Visit([&](const RtlExpr& n) {
        if (n.IsReg()) {
          const int nr = n.RegNo();
          if (nr == target_r || ((n.mode == RtlMachineMode::kDi || n.mode == RtlMachineMode::kDf) &&
                                 nr + 1 == target_r)) {
            ++(*read_cnt);
          }
        }
      });
    };
    auto check_elem = [&](const RtlExpr& elem) {
      if (elem.IsSet()) {
        if (!elem.SetDest().IsReg()) {
          count_reads(elem.SetDest());
        }
        count_reads(elem.SetSrc());
        if (elem.SetDest().IsReg()) {
          const int dr = elem.SetDest().RegNo();
          if (dr == target_r || ((elem.SetDest().mode == RtlMachineMode::kDi ||
                                  elem.SetDest().mode == RtlMachineMode::kDf) &&
                                 dr + 1 == target_r)) {
            *pure_kill = (*read_cnt == 0);
          }
        }
      } else if (elem.op == RtlOp::kClobber && !elem.children.empty() && elem.children[0].IsReg()) {
        const int dr = elem.children[0].RegNo();
        if (dr == target_r) {
          *pure_kill = (*read_cnt == 0);
        }
      } else {
        count_reads(elem);
      }
    };
    if (expr.IsParallel()) {
      for (const RtlExpr& c : expr.children) {
        check_elem(c);
      }
    } else {
      check_elem(expr);
    }
  };

  for (RtlBasicBlock& blk : cfg->rpo_blocks) {
    const size_t n = blk.instructions.size();
    if (n < 2) {
      continue;
    }
    std::vector<RtlPatternAndNotes> parsed(n);
    std::vector<bool> ok(n, false);
    for (size_t i = 0; i < n; ++i) {
      auto p = ParsePatternAndNotes(blk.instructions[i]);
      if (p.ok()) {
        parsed[i] = std::move(*p);
        ok[i] = true;
      }
    }
    bool changed = false;
    for (size_t i = 0; i + 1 < n; ++i) {
      if (!ok[i] || !parsed[i].pattern.IsSet()) {
        continue;
      }
      const RtlExpr& dst_i = parsed[i].pattern.SetDest();
      const RtlExpr& src_i = parsed[i].pattern.SetSrc();
      if (!dst_i.IsReg() || dst_i.mode != RtlMachineMode::kSi || dst_i.flags.volatil ||
          dst_i.flags.integrated || src_i.op != RtlOp::kZeroExtend ||
          src_i.mode != RtlMachineMode::kSi || src_i.children.size() != 1 ||
          !src_i.children[0].IsMem() || src_i.children[0].flags.volatil) {
        continue;
      }
      const int r = dst_i.RegNo();
      const RtlMachineMode mem_mode = src_i.children[0].mode;
      if (r < 2 || r > 25 || (mem_mode != RtlMachineMode::kQi && mem_mode != RtlMachineMode::kHi)) {
        continue;
      }
      const int64_t max_mask = (mem_mode == RtlMachineMode::kQi) ? 255 : 65535;

      for (size_t j = i + 1; j < n; ++j) {
        if (!ok[j]) {
          break;
        }
        int read_cnt = 0;
        bool pure_kill = false;
        bool bad = false;
        touches_gpr(parsed[j].pattern, r, &read_cnt, &pure_kill, &bad);
        if (bad || pure_kill) {
          break;
        }
        bool writes_r = false;
        if (parsed[j].pattern.IsSet() && parsed[j].pattern.SetDest().IsReg()) {
          const int dr = parsed[j].pattern.SetDest().RegNo();
          if (dr == r || ((parsed[j].pattern.SetDest().mode == RtlMachineMode::kDi ||
                           parsed[j].pattern.SetDest().mode == RtlMachineMode::kDf) &&
                          dr + 1 == r)) {
            writes_r = true;
          }
        }
        if (read_cnt == 0 && !writes_r) {
          continue;
        }
        if (!parsed[j].pattern.IsSet() || read_cnt != 1) {
          break;
        }
        const RtlExpr& dst_j = parsed[j].pattern.SetDest();
        const RtlExpr& src_j = parsed[j].pattern.SetSrc();
        const int d = dst_j.IsReg() ? dst_j.RegNo() : -1;
        const bool valid_d =
            (d >= 2 && d <= 25) || (d == 30 && !cfg->sideband.frame_pointer_needed);
        if (!dst_j.IsReg() || !valid_d || dst_j.mode != RtlMachineMode::kSi ||
            dst_j.flags.volatil || src_j.op != RtlOp::kAnd || src_j.mode != RtlMachineMode::kSi ||
            src_j.children.size() != 2 || !src_j.children[0].IsReg() ||
            src_j.children[0].RegNo() != r || src_j.children[0].mode != RtlMachineMode::kSi ||
            src_j.children[0].flags.volatil || src_j.children[0].flags.integrated ||
            !src_j.children[1].IsConstInt() || src_j.children[1].int_val <= 0 ||
            src_j.children[1].int_val >= max_mask) {
          break;
        }
        bool proven_dead_after_j = (d == r);
        for (size_t m = j + 1; !proven_dead_after_j && m < n; ++m) {
          if (!ok[m]) {
            break;
          }
          int m_reads = 0;
          bool m_kill = false;
          bool m_bad = false;
          touches_gpr(parsed[m].pattern, r, &m_reads, &m_kill, &m_bad);
          if (m_bad || m_reads > 0) {
            break;
          }
          if (m_kill) {
            proven_dead_after_j = true;
          }
        }
        if (proven_dead_after_j) {
          RtlExpr mem_expr = src_i.children[0];
          parsed[i].pattern.MutableSetDest().mode = mem_mode;
          parsed[i].pattern.MutableSetSrc() = std::move(mem_expr);
          RtlExpr narrow_reg = src_j.children[0];
          narrow_reg.mode = mem_mode;
          parsed[j].pattern.MutableSetSrc().children[0] =
              RtlExpr::MakeSubreg(RtlMachineMode::kSi, std::move(narrow_reg), 0);
          blk.instructions[i] = parsed[i].ToGccPrintRtlString();
          blk.instructions[j] = parsed[j].ToGccPrintRtlString();
          changed = true;
        }
        break;
      }
    }
    if (changed) {
      blk.SyncRtlInsnsFromInstructions();
    }
  }
}

// In KMC GCC 2.7.2 `reorg.c:3348-3368` (`fill_slots_from_thread`), a self-loop
// polling block `[<reg> = <load>(<mem>), <cond_branch_to_self>]` with no memory
// stores, calls, or modifications to the address registers would always have
// its backedge redirected past `<load>` by `redundant_insn` unless the `MEM`
// carried `MEM_VOLATILE_P` (`mem/v`).
void MarkSelfLoopSpinWaitLoadsVolatile(RtlControlFlowGraph* cfg) {
  for (size_t b = 0; b < cfg->rpo_blocks.size(); ++b) {
    RtlBasicBlock& blk = cfg->rpo_blocks[b];
    bool has_self_edge = false;
    for (size_t succ : blk.successors) {
      if (succ == b) {
        has_self_edge = true;
        break;
      }
    }
    if (!has_self_edge || blk.instructions.size() < 2) {
      continue;
    }

    auto last_parsed = ParsePatternAndNotes(blk.instructions.back());
    if (!last_parsed.ok() || last_parsed->pattern.op == RtlOp::kSequence ||
        !last_parsed->pattern.IsSet() || last_parsed->pattern.SetDest().op != RtlOp::kPc ||
        last_parsed->pattern.SetSrc().op != RtlOp::kIfThenElse) {
      continue;
    }

    auto first_parsed = ParsePatternAndNotes(blk.instructions.front());
    if (!first_parsed.ok() || !first_parsed->pattern.IsSet() ||
        !first_parsed->pattern.SetDest().IsReg()) {
      continue;
    }
    bool first_has_mem = false;
    std::vector<int> addr_regs;
    first_parsed->pattern.SetSrc().Visit([&](const RtlExpr& node) {
      if (node.IsMem()) {
        first_has_mem = true;
        node.MemAddr().Visit([&](const RtlExpr& addr_node) {
          if (addr_node.IsReg()) {
            addr_regs.push_back(addr_node.RegNo());
          }
        });
      }
    });
    if (!first_has_mem) {
      continue;
    }

    bool invalidated_in_loop = false;
    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      auto p = ParsePatternAndNotes(blk.instructions[i]);
      if (!p.ok()) {
        invalidated_in_loop = true;
        break;
      }
      p->pattern.Visit([&](const RtlExpr& node) {
        if (node.op == RtlOp::kCall || node.op == RtlOp::kUnspecVolatile ||
            node.op == RtlOp::kAsmInput) {
          invalidated_in_loop = true;
        }
        if (node.IsSet() || node.op == RtlOp::kClobber) {
          const RtlExpr& d = node.children[0];
          if (d.IsMem()) {
            invalidated_in_loop = true;
          } else if (d.IsReg()) {
            for (int ar : addr_regs) {
              if (d.RegNo() == ar) {
                invalidated_in_loop = true;
              }
            }
          }
        }
      });
      if (invalidated_in_loop) {
        break;
      }
    }
    if (invalidated_in_loop) {
      continue;
    }

    first_parsed->pattern.MutableSetSrc().Mutate([](RtlExpr& node) {
      if (node.IsMem()) {
        node.flags.volatil = true;
      }
    });
    blk.instructions.front() = FormatGccPrintRtlPatternAndNotes(
        first_parsed->pattern, first_parsed->reg_notes, first_parsed->bracketed_notes,
        /*call_fusage=*/{});
    blk.SyncRtlInsnsFromInstructions();
  }
}

struct Stage0PreReorgBlockInsn {
  bool is_sequence = false;
  RtlExpr seq_head_wrap;
  RtlExpr seq_slot_wrap;
  RtlPatternAndNotes top_pn;
  RtlPatternAndNotes head_pn;
  RtlPatternAndNotes slot_pn;
  bool mutated = false;
};

struct Stage0PreReorgExecStep {
  size_t insn_idx = 0;
  RtlPatternAndNotes* pn = nullptr;
  bool* mutated = nullptr;
  absl::flat_hash_set<int> read_regs;
  absl::flat_hash_set<int> write_regs;
  bool reads_mem = false;
  bool writes_mem = false;
  bool has_side_effects = false;
};

int64_t Stage0ScalarMemModeByteSize(RtlMachineMode mode) {
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

void InsertStage0RegSpan(int regno, RtlMachineMode mode, bool gp64, bool fp64,
                         absl::flat_hash_set<int>* out) {
  if (regno <= 0) {
    return;
  }
  const int nregs = std::max(1, MipsHardRegNregs(regno, mode, gp64, fp64));
  for (int k = 0; k < nregs; ++k) {
    out->insert(regno + k);
  }
}

void AnalyzeStage0SourceExpr(const RtlExpr& expr, bool gp64, bool fp64,
                             absl::flat_hash_set<int>* reads, bool* reads_mem,
                             bool* has_side_effects) {
  if (expr.op == RtlOp::kMem) {
    *reads_mem = true;
    if (expr.flags.volatil) {
      *has_side_effects = true;
    }
  }
  if (expr.op == RtlOp::kCall || expr.op == RtlOp::kUnspecVolatile || expr.op == RtlOp::kAsmInput ||
      expr.op == RtlOp::kAsmOperands || expr.op == RtlOp::kReturn) {
    *has_side_effects = true;
  }
  if (expr.op == RtlOp::kReg && !expr.IsArgPointerReg()) {
    InsertStage0RegSpan(expr.RegNo(), expr.mode, gp64, fp64, reads);
    return;
  }
  for (const RtlExpr& ch : expr.children) {
    AnalyzeStage0SourceExpr(ch, gp64, fp64, reads, reads_mem, has_side_effects);
  }
}

void AnalyzeStage0DestExpr(const RtlExpr& dst, bool gp64, bool fp64,
                           absl::flat_hash_set<int>* reads, absl::flat_hash_set<int>* writes,
                           bool* writes_mem, bool* has_side_effects) {
  if (dst.op == RtlOp::kMem) {
    *writes_mem = true;
    if (dst.flags.volatil) {
      *has_side_effects = true;
    }
    for (const RtlExpr& ch : dst.children) {
      bool dummy_reads_mem = false;
      AnalyzeStage0SourceExpr(ch, gp64, fp64, reads, &dummy_reads_mem, has_side_effects);
    }
    return;
  }
  const RtlExpr* inner = &dst;
  while (inner->op == RtlOp::kSubreg && !inner->children.empty()) {
    inner = &inner->children[0];
  }
  if (inner->op == RtlOp::kReg && !inner->IsArgPointerReg()) {
    InsertStage0RegSpan(inner->RegNo(), dst.mode, gp64, fp64, writes);
    return;
  }
  for (const RtlExpr& ch : dst.children) {
    AnalyzeStage0DestExpr(ch, gp64, fp64, reads, writes, writes_mem, has_side_effects);
  }
}

void AnalyzeStage0Stmt(const RtlExpr& stmt, bool gp64, bool fp64, Stage0PreReorgExecStep* step) {
  if (stmt.op == RtlOp::kSet && stmt.children.size() == 2) {
    if (stmt.children[0].op == RtlOp::kPc) {
      step->has_side_effects = true;
    } else {
      AnalyzeStage0DestExpr(stmt.children[0], gp64, fp64, &step->read_regs, &step->write_regs,
                            &step->writes_mem, &step->has_side_effects);
    }
    AnalyzeStage0SourceExpr(stmt.children[1], gp64, fp64, &step->read_regs, &step->reads_mem,
                            &step->has_side_effects);
    return;
  }
  if (stmt.op == RtlOp::kClobber && !stmt.children.empty()) {
    AnalyzeStage0DestExpr(stmt.children[0], gp64, fp64, &step->read_regs, &step->write_regs,
                          &step->writes_mem, &step->has_side_effects);
    return;
  }
  if (stmt.op == RtlOp::kUse && !stmt.children.empty()) {
    AnalyzeStage0SourceExpr(stmt.children[0], gp64, fp64, &step->read_regs, &step->reads_mem,
                            &step->has_side_effects);
    return;
  }
  if (stmt.op == RtlOp::kParallel) {
    for (const RtlExpr& elem : stmt.children) {
      AnalyzeStage0Stmt(elem, gp64, fp64, step);
    }
    return;
  }
  step->has_side_effects = true;
  AnalyzeStage0SourceExpr(stmt, gp64, fp64, &step->read_regs, &step->reads_mem,
                          &step->has_side_effects);
}

bool BuildStage0PreReorgSteps(const RtlBasicBlock& blk, bool gp64, bool fp64,
                              std::vector<Stage0PreReorgBlockInsn>* parsed_insns,
                              std::vector<Stage0PreReorgExecStep>* steps) {
  parsed_insns->clear();
  steps->clear();
  parsed_insns->resize(blk.instructions.size());
  for (size_t i = 0; i < blk.instructions.size(); ++i) {
    auto parsed = ParsePatternAndNotes(blk.instructions[i]);
    if (!parsed.ok()) {
      return false;
    }
    Stage0PreReorgBlockInsn& item = (*parsed_insns)[i];
    if (parsed->pattern.op == RtlOp::kSequence && parsed->pattern.children.size() == 2) {
      auto head_pn = RtlPatternAndNotes::FromSequenceSubInsn(parsed->pattern.children[0]);
      auto slot_pn = RtlPatternAndNotes::FromSequenceSubInsn(parsed->pattern.children[1]);
      if (!head_pn.ok() || !slot_pn.ok()) {
        return false;
      }
      item.is_sequence = true;
      item.seq_head_wrap = parsed->pattern.children[0];
      item.seq_slot_wrap = parsed->pattern.children[1];
      item.head_pn = std::move(*head_pn);
      item.slot_pn = std::move(*slot_pn);
      if (!item.seq_slot_wrap.flags.in_struct) {
        Stage0PreReorgExecStep slot_step{
            .insn_idx = i, .pn = &item.slot_pn, .mutated = &item.mutated};
        AnalyzeStage0Stmt(item.slot_pn.pattern, gp64, fp64, &slot_step);
        steps->push_back(std::move(slot_step));
      }
      Stage0PreReorgExecStep head_step{
          .insn_idx = i, .pn = &item.head_pn, .mutated = &item.mutated};
      AnalyzeStage0Stmt(item.head_pn.pattern, gp64, fp64, &head_step);
      steps->push_back(std::move(head_step));
    } else {
      item.top_pn = std::move(*parsed);
      Stage0PreReorgExecStep top_step{.insn_idx = i, .pn = &item.top_pn, .mutated = &item.mutated};
      AnalyzeStage0Stmt(item.top_pn.pattern, gp64, fp64, &top_step);
      steps->push_back(std::move(top_step));
    }
  }
  return true;
}

void FlushStage0MutatedBlockInsns(const std::vector<Stage0PreReorgBlockInsn>& parsed_insns,
                                  RtlBasicBlock* blk) {
  const bool had_synced_rtl = (blk->rtl_insns.size() == blk->instructions.size());
  bool any_mutated = false;
  for (size_t i = 0; i < parsed_insns.size(); ++i) {
    const Stage0PreReorgBlockInsn& item = parsed_insns[i];
    if (!item.mutated) {
      continue;
    }
    any_mutated = true;
    if (item.is_sequence) {
      RtlPatternAndNotes seq_head_pn = item.head_pn;
      if (item.seq_head_wrap.op == RtlOp::kCallInsn) {
        seq_head_pn.reg_notes.clear();
        seq_head_pn.has_explicit_call_fusage = false;
        seq_head_pn.call_fusage.clear();
      }
      blk->instructions[i] =
          RtlExpr::MakeSequence(
              {
                  RtlExpr::MakeInsnWrapper(item.seq_head_wrap.op, item.seq_head_wrap.flags,
                                           seq_head_pn),
                  RtlExpr::MakeInsnWrapper(item.seq_slot_wrap.op, item.seq_slot_wrap.flags,
                                           item.slot_pn),
              })
              .ToGccPrintRtlString();
    } else {
      blk->instructions[i] = FormatGccPrintRtlPatternAndNotes(
          item.top_pn.pattern, item.top_pn.reg_notes, item.top_pn.bracketed_notes,
          /*call_fusage=*/{});
    }
    if (had_synced_rtl) {
      blk->rtl_insns[i].canonical_pattern = blk->instructions[i];
    }
  }
  if (any_mutated && !had_synced_rtl) {
    blk->SyncRtlInsnsFromInstructions();
  }
}

bool Stage0ExprHasCall(const RtlExpr& pat) {
  if (pat.op == RtlOp::kCall) {
    return true;
  }
  for (const RtlExpr& ch : pat.children) {
    if (Stage0ExprHasCall(ch)) {
      return true;
    }
  }
  return false;
}

bool Stage0ExprIsJumpOrReturn(const RtlExpr& pat) {
  if (pat.op == RtlOp::kReturn) {
    return true;
  }
  if (pat.op == RtlOp::kSet && !pat.children.empty() && pat.children[0].op == RtlOp::kPc) {
    return true;
  }
  if (pat.op == RtlOp::kParallel) {
    for (const RtlExpr& elem : pat.children) {
      if (Stage0ExprIsJumpOrReturn(elem)) {
        return true;
      }
    }
  }
  return false;
}

bool Stage0HasOnlyFlowLivenessNotes(const RtlPatternAndNotes& pn) {
  for (const RtlRegNote& note : pn.reg_notes) {
    if (note.kind != RtlMachineMode::kRegDead && note.kind != RtlMachineMode::kRegUnused) {
      return false;
    }
  }
  return true;
}

bool MatchStage0PrologueSpDecrement(const RtlExpr& pat, int* out_tsize) {
  if (pat.op != RtlOp::kSet || pat.children.size() != 2) {
    return false;
  }
  const RtlExpr& dst = pat.children[0];
  const RtlExpr& src = pat.children[1];
  if (dst.op != RtlOp::kReg || dst.RegNo() != 29 || src.children.size() != 2 ||
      src.children[0].op != RtlOp::kReg || src.children[0].RegNo() != 29) {
    return false;
  }
  if (src.op == RtlOp::kMinus && src.children[1].op == RtlOp::kConstInt &&
      src.children[1].int_val > 0) {
    *out_tsize = static_cast<int>(src.children[1].int_val);
    return true;
  }
  if (src.op == RtlOp::kPlus && src.children[1].op == RtlOp::kConstInt &&
      src.children[1].int_val < 0) {
    *out_tsize = static_cast<int>(-src.children[1].int_val);
    return true;
  }
  return false;
}

bool MatchStage0PrologueSaveStore(const RtlExpr& pat, int tsize, int* out_saved_regno) {
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
  const bool is_callee_saved_gpr = (regno >= 16 && regno <= 23) || regno == 30 || regno == 31;
  const bool is_callee_saved_fpr = (regno >= 52 && regno <= 63);
  if (!is_callee_saved_gpr && !is_callee_saved_fpr) {
    return false;
  }
  const RtlExpr& addr = dst.children[0];
  if (addr.op == RtlOp::kReg && addr.RegNo() == 29) {
    *out_saved_regno = regno;
    return tsize > 0;
  }
  if (addr.op == RtlOp::kPlus && addr.children.size() == 2 && addr.children[0].op == RtlOp::kReg &&
      addr.children[0].RegNo() == 29 && addr.children[1].op == RtlOp::kConstInt) {
    const int64_t off = addr.children[1].int_val;
    if (off >= 0 && off < tsize) {
      *out_saved_regno = regno;
      return true;
    }
  }
  return false;
}

bool IsStage0CalleeSavedGpr(int r) {
  return (r >= 16 && r <= 23) || r == 28 || r == 29 || r == 30 || r == 31;
}

// Recovers `RTX_UNCHANGING_P` (`mem/u`) in Stage 0 (`.12.dbr`) from post-`sched2`
// prologue-store hoisting (`sched.c:true_dependence` + `rtlanal.c:rtx_varies_p`,
// where `stack_pointer_rtx` has `rtx_addr_varies_p == 1` so a general varying
// pointer `MEM` read can only be scheduled ahead of a `$sp`-based callee-saved
// prologue store when `RTX_UNCHANGING_P(mem) == 1`).
void MarkPrologueHoistedUnchangingPtrMems(RtlControlFlowGraph* cfg,
                                          const CompilationProfile& profile) {
  if (cfg == nullptr || cfg->rpo_blocks.empty() || cfg->rpo_blocks[0].instructions.empty()) {
    return;
  }
  bool gp64 = false;
  bool fp64 = false;
  DetectMipsRegisterWidthFlags(*cfg, &profile, &gp64, &fp64);

  RtlBasicBlock& entry = cfg->rpo_blocks[0];
  std::vector<Stage0PreReorgBlockInsn> parsed_insns;
  std::vector<Stage0PreReorgExecStep> steps;
  if (!BuildStage0PreReorgSteps(entry, gp64, fp64, &parsed_insns, &steps)) {
    return;
  }

  std::optional<size_t> sp_dec_step;
  int tsize = 0;
  for (size_t s = 0; s < steps.size(); ++s) {
    int sp_dec = 0;
    if (Stage0HasOnlyFlowLivenessNotes(*steps[s].pn) &&
        MatchStage0PrologueSpDecrement(steps[s].pn->pattern, &sp_dec) && sp_dec > 0) {
      sp_dec_step = s;
      tsize = sp_dec;
      break;
    }
    if (Stage0ExprHasCall(steps[s].pn->pattern) || Stage0ExprIsJumpOrReturn(steps[s].pn->pattern) ||
        steps[s].write_regs.contains(29)) {
      break;
    }
  }
  if (!sp_dec_step.has_value()) {
    return;
  }

  std::vector<bool> is_prologue(steps.size(), false);
  is_prologue[*sp_dec_step] = true;
  bool frame_pointer_needed = cfg->sideband.frame_pointer_needed;
  absl::flat_hash_set<int> stripped_saves;
  absl::flat_hash_set<int> body_writes;
  std::optional<size_t> last_prologue_store_step;
  for (size_t s = *sp_dec_step + 1; s < steps.size(); ++s) {
    const RtlExpr& pat = steps[s].pn->pattern;
    if (Stage0ExprHasCall(pat) || Stage0ExprIsJumpOrReturn(pat)) {
      break;
    }
    if (Stage0HasOnlyFlowLivenessNotes(*steps[s].pn)) {
      int saved_reg = -1;
      if (MatchStage0PrologueSaveStore(pat, tsize, &saved_reg) &&
          !stripped_saves.contains(saved_reg) && !body_writes.contains(saved_reg)) {
        const RtlMachineMode src_mode = pat.children[1].mode;
        const bool second_half_clobbered = MipsHardRegNregs(saved_reg, src_mode, gp64, fp64) == 2 &&
                                           body_writes.contains(saved_reg + 1);
        if (!second_half_clobbered) {
          stripped_saves.insert(saved_reg);
          is_prologue[s] = true;
          last_prologue_store_step = s;
          continue;
        }
      }
      if (pat.op == RtlOp::kSet && pat.children.size() == 2 && pat.children[0].op == RtlOp::kReg &&
          pat.children[0].RegNo() == 30 && !pat.children[0].flags.volatil &&
          pat.children[1].op == RtlOp::kReg && pat.children[1].RegNo() == 29 &&
          !body_writes.contains(30)) {
        frame_pointer_needed = true;
        is_prologue[s] = true;
        continue;
      }
    }
    if (steps[s].write_regs.contains(29)) {
      break;
    }
    for (int r : steps[s].write_regs) {
      body_writes.insert(r);
    }
  }
  if (!last_prologue_store_step.has_value()) {
    return;
  }

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
    if (breg == 29 || breg == 13 || (breg == 30 && frame_pointer_needed)) {
      return -1;
    }
    return (breg >= 1 && breg < 32) ? breg : -1;
  };

  absl::flat_hash_set<int> unchanging_ptr_regs;
  auto mark_read_mems = [&](auto& self, RtlExpr* expr, bool before_prologue_store) -> bool {
    bool changed = false;
    if (expr->op == RtlOp::kMem && !expr->children.empty()) {
      const int breg = extract_general_varying_ptr_base(expr->children[0]);
      if (!expr->flags.volatil && breg != -1) {
        if (before_prologue_store || expr->flags.unchanging) {
          unchanging_ptr_regs.insert(breg);
          if (!expr->flags.unchanging) {
            expr->flags.unchanging = true;
            changed = true;
          }
        } else if (unchanging_ptr_regs.contains(breg)) {
          expr->flags.unchanging = true;
          changed = true;
        }
      }
      if (self(self, &expr->children[0], before_prologue_store)) {
        changed = true;
      }
      return changed;
    }
    for (RtlExpr& ch : expr->children) {
      if (self(self, &ch, before_prologue_store)) {
        changed = true;
      }
    }
    return changed;
  };
  auto mark_stmt_reads = [&](auto& self, RtlExpr* stmt, bool before_prologue_store) -> bool {
    if (stmt->op == RtlOp::kSet && stmt->children.size() == 2) {
      bool changed = false;
      if (stmt->children[0].op == RtlOp::kMem && !stmt->children[0].children.empty()) {
        const int w_breg = extract_general_varying_ptr_base(stmt->children[0].children[0]);
        if (w_breg != -1) {
          unchanging_ptr_regs.erase(w_breg);
        }
        if (mark_read_mems(mark_read_mems, &stmt->children[0].children[0], before_prologue_store)) {
          changed = true;
        }
      }
      if (mark_read_mems(mark_read_mems, &stmt->children[1], before_prologue_store)) {
        changed = true;
      }
      return changed;
    }
    if (stmt->op == RtlOp::kParallel) {
      bool changed = false;
      for (RtlExpr& elem : stmt->children) {
        if (self(self, &elem, before_prologue_store)) {
          changed = true;
        }
      }
      return changed;
    }
    if (stmt->op == RtlOp::kClobber) {
      return false;
    }
    return mark_read_mems(mark_read_mems, stmt, before_prologue_store);
  };

  for (size_t s = 0; s < steps.size(); ++s) {
    if (is_prologue[s]) {
      continue;
    }
    const bool before_prologue_store = (s < *last_prologue_store_step);
    if (mark_stmt_reads(mark_stmt_reads, &steps[s].pn->pattern, before_prologue_store)) {
      *steps[s].mutated = true;
    }
    const RtlExpr& pat = steps[s].pn->pattern;
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
      if (dst_reg >= 1 && dst_reg < 32 && unchanging_ptr_regs.contains(src_ptr_reg)) {
        unchanging_ptr_regs.insert(dst_reg);
        preserved_dst = dst_reg;
      }
    }
    for (int w : steps[s].write_regs) {
      if (w != preserved_dst) {
        unchanging_ptr_regs.erase(w);
      }
    }
    if (Stage0ExprHasCall(pat)) {
      for (int r = 1; r < 32; ++r) {
        if (!IsStage0CalleeSavedGpr(r)) {
          unchanging_ptr_regs.erase(r);
        }
      }
    }
  }

  // When `sched2` hoisted a `mem/u` load `l_s` ahead of prologue saves after
  // `pending_lists_length > 32` (`sched.c:1740`) flushed pending memory stores,
  // `l_s` originally followed the intervening readers of `breg` in `.09.greg`
  // and therefore retained `(REG_DEAD (reg breg))` (`sched.c` ignores
  // reader-reader `REG_DEAD` placement).
  if (ParseCompilationProfileFlags(profile).RunsSched2()) {
    std::optional<size_t> first_call_step;
    for (size_t s = 0; s < steps.size(); ++s) {
      if (Stage0ExprHasCall(steps[s].pn->pattern)) {
        first_call_step = s;
        break;
      }
      if (Stage0ExprIsJumpOrReturn(steps[s].pn->pattern)) {
        break;
      }
    }
    if (first_call_step.has_value()) {
      auto count_expr_mems = [](auto& self, const RtlExpr& e) -> int {
        int cnt = (e.op == RtlOp::kMem) ? 1 : 0;
        for (const RtlExpr& ch : e.children) {
          cnt += self(self, ch);
        }
        return cnt;
      };
      auto is_sp_reg_store = [](const Stage0PreReorgExecStep& st) -> bool {
        if (st.has_side_effects || st.reads_mem || !st.writes_mem) {
          return false;
        }
        const RtlExpr& p = st.pn->pattern;
        if (p.op != RtlOp::kSet || p.children.size() != 2 || p.children[0].op != RtlOp::kMem ||
            p.children[0].flags.volatil || p.children[0].children.size() != 1 ||
            p.children[1].op != RtlOp::kReg) {
          return false;
        }
        const RtlExpr& addr = p.children[0].children[0];
        if (addr.op == RtlOp::kReg) {
          return addr.RegNo() == 29;
        }
        return addr.op == RtlOp::kPlus && addr.children.size() == 2 &&
               addr.children[0].op == RtlOp::kReg && addr.children[0].RegNo() == 29 &&
               addr.children[1].op == RtlOp::kConstInt;
      };
      int pending_lists_len = 0;
      bool overflowed_pending_lists = false;
      std::optional<size_t> max_body_sp_store_step;
      for (size_t i = 0; i < *first_call_step; ++i) {
        if (steps[i].writes_mem && pending_lists_len > 32) {
          overflowed_pending_lists = true;
        }
        if (!is_prologue[i] && is_sp_reg_store(steps[i])) {
          max_body_sp_store_step = i;
        }
        pending_lists_len += count_expr_mems(count_expr_mems, steps[i].pn->pattern);
      }
      if (overflowed_pending_lists && max_body_sp_store_step.has_value() &&
          *last_prologue_store_step < *max_body_sp_store_step) {
        struct HoistedPtrLoadCand {
          size_t l_s = 0;
          int breg = -1;
          size_t consumer_s = 0;
        };
        std::vector<HoistedPtrLoadCand> cands;
        for (size_t l_s = 0; l_s < *last_prologue_store_step; ++l_s) {
          if (is_prologue[l_s] || steps[l_s].has_side_effects || steps[l_s].writes_mem ||
              !steps[l_s].reads_mem) {
            continue;
          }
          const RtlExpr& lpat = steps[l_s].pn->pattern;
          if (lpat.op != RtlOp::kSet || lpat.children.size() != 2 ||
              lpat.children[0].op != RtlOp::kReg) {
            continue;
          }
          const int load_dst = lpat.children[0].RegNo();
          if (load_dst < 1 || load_dst >= 32 || load_dst == 29 ||
              (frame_pointer_needed && load_dst == 30)) {
            continue;
          }
          const RtlExpr* load_rhs = &lpat.children[1];
          if ((load_rhs->op == RtlOp::kSignExtend || load_rhs->op == RtlOp::kZeroExtend) &&
              load_rhs->children.size() == 1) {
            load_rhs = &load_rhs->children[0];
          }
          if (load_rhs->op != RtlOp::kMem || !load_rhs->flags.unchanging ||
              load_rhs->flags.volatil || load_rhs->children.size() != 1) {
            continue;
          }
          const int breg = extract_general_varying_ptr_base(load_rhs->children[0]);
          if (breg == -1 || breg == load_dst) {
            continue;
          }
          std::optional<size_t> consumer_s;
          bool blocked = false;
          for (size_t k = l_s + 1; k < *first_call_step; ++k) {
            if (steps[k].read_regs.contains(load_dst)) {
              consumer_s = k;
              break;
            }
            if (steps[k].has_side_effects || steps[k].write_regs.contains(load_dst) ||
                steps[k].write_regs.contains(breg) ||
                (steps[k].writes_mem && !is_sp_reg_store(steps[k]))) {
              blocked = true;
              break;
            }
          }
          if (blocked || !consumer_s.has_value()) {
            continue;
          }
          bool breg_used_after_consumer = false;
          for (size_t k = *consumer_s; k < *first_call_step; ++k) {
            if (steps[k].read_regs.contains(breg) || steps[k].write_regs.contains(breg)) {
              breg_used_after_consumer = true;
              break;
            }
          }
          if (breg_used_after_consumer) {
            continue;
          }
          cands.push_back(HoistedPtrLoadCand{.l_s = l_s, .breg = breg, .consumer_s = *consumer_s});
        }
        for (size_t m = *last_prologue_store_step + 1; m < *first_call_step; ++m) {
          auto& m_notes = steps[m].pn->reg_notes;
          for (size_t ni = 0; ni < m_notes.size();) {
            if (m_notes[ni].kind != RtlMachineMode::kRegDead ||
                m_notes[ni].datum.op != RtlOp::kReg || m_notes[ni].datum.IsArgPointerReg()) {
              ++ni;
              continue;
            }
            const int breg = m_notes[ni].datum.RegNo();
            if (!steps[m].read_regs.contains(breg)) {
              ++ni;
              continue;
            }
            std::optional<size_t> best_l_s;
            size_t best_consumer_s = 0;
            for (const HoistedPtrLoadCand& c : cands) {
              if (c.breg != breg || c.l_s >= m || c.consumer_s <= m) {
                continue;
              }
              if (!(c.consumer_s > *max_body_sp_store_step || m <= *max_body_sp_store_step)) {
                continue;
              }
              if (!best_l_s.has_value() || c.consumer_s > best_consumer_s ||
                  (c.consumer_s == best_consumer_s && c.l_s > *best_l_s)) {
                best_l_s = c.l_s;
                best_consumer_s = c.consumer_s;
              }
            }
            if (!best_l_s.has_value()) {
              ++ni;
              continue;
            }
            RtlRegNote moved_note = m_notes[ni];
            m_notes.erase(m_notes.begin() + static_cast<ptrdiff_t>(ni));
            *steps[m].mutated = true;
            bool l_has_dead = false;
            for (const RtlRegNote& ln : steps[*best_l_s].pn->reg_notes) {
              if (ln.kind == RtlMachineMode::kRegDead && ln.datum.op == RtlOp::kReg &&
                  !ln.datum.IsArgPointerReg() && ln.datum.RegNo() == breg) {
                l_has_dead = true;
                break;
              }
            }
            if (!l_has_dead) {
              steps[*best_l_s].pn->reg_notes.push_back(std::move(moved_note));
              *steps[*best_l_s].mutated = true;
            }
          }
        }
      }
    }
  }

  FlushStage0MutatedBlockInsns(parsed_insns, &entry);
}

bool IsStage0MipsLargeInt(int64_t val) {
  const uint32_t u32 = static_cast<uint32_t>(val);
  const uint32_t high = u32 & 0xffff0000u;
  const uint32_t low = u32 & 0x0000ffffu;
  return high != 0u && high != 0xffff0000u && low != 0u;
}

bool IsStage0SingleCycleIntAluExpr(const RtlExpr& expr) {
  switch (expr.op) {
    case RtlOp::kReg: {
      const int r = expr.RegNo();
      return r >= 0 && (r < 32 || r > 66);
    }
    case RtlOp::kSubreg:
      return !expr.children.empty() && IsStage0SingleCycleIntAluExpr(expr.children[0]);
    case RtlOp::kConstInt:
      return !IsStage0MipsLargeInt(expr.int_val);
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
      for (const RtlExpr& ch : expr.children) {
        if (!IsStage0SingleCycleIntAluExpr(ch)) {
          return false;
        }
      }
      return true;
    }
    default:
      return false;
  }
}

bool IsStage0SingleCycleIntAluStep(const Stage0PreReorgExecStep& step, int* out_dest_regno) {
  if (step.has_side_effects || step.reads_mem || step.writes_mem) {
    return false;
  }
  const RtlExpr& pat = step.pn->pattern;
  if (pat.op != RtlOp::kSet || pat.children.size() != 2) {
    return false;
  }
  const RtlExpr& dst = pat.children[0];
  if (dst.op != RtlOp::kReg || dst.mode == RtlMachineMode::kDi || dst.mode == RtlMachineMode::kSf ||
      dst.mode == RtlMachineMode::kDf) {
    return false;
  }
  const int dst_regno = dst.RegNo();
  if (dst_regno < 0 || dst_regno == 29 || (dst_regno >= 32 && dst_regno <= 66) ||
      !IsStage0SingleCycleIntAluExpr(pat.children[1])) {
    return false;
  }
  *out_dest_regno = dst_regno;
  return true;
}

bool IsStage0SimpleRegMemStoreStep(const Stage0PreReorgExecStep& step, int* out_src_regno) {
  if (step.has_side_effects || step.reads_mem || !step.writes_mem) {
    return false;
  }
  const RtlExpr& pat = step.pn->pattern;
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

bool IsStage0SchedulableLoadStep(const Stage0PreReorgExecStep& step, int* out_dest_regno,
                                 const RtlExpr** out_mem) {
  if (step.has_side_effects || step.writes_mem || !step.reads_mem) {
    return false;
  }
  const RtlExpr& pat = step.pn->pattern;
  if (pat.op != RtlOp::kSet || pat.children.size() != 2 || pat.children[0].op != RtlOp::kReg) {
    return false;
  }
  const int dst_regno = pat.children[0].RegNo();
  if (dst_regno < 0 || dst_regno == 29 || dst_regno == 30 || (dst_regno >= 64 && dst_regno <= 66)) {
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
  *out_mem = src;
  return true;
}

bool ExtractStage0NonStackBaseDispMem(const RtlExpr& mem, int* out_base, int64_t* out_off,
                                      int64_t* out_size) {
  if (mem.op != RtlOp::kMem || mem.children.size() != 1) {
    return false;
  }
  const int64_t sz = Stage0ScalarMemModeByteSize(mem.mode);
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
  if (base < 1 || base >= 32 || base == 29 || base == 30) {
    return false;
  }
  *out_base = base;
  *out_off = off;
  *out_size = sz;
  return true;
}

bool Stage0MarkVolatileMemsOnBaseInExpr(RtlExpr* expr,
                                        const absl::flat_hash_set<int>& volatile_bases) {
  bool changed = false;
  int base = -1;
  int64_t off = 0;
  int64_t sz = 0;
  if (ExtractStage0NonStackBaseDispMem(*expr, &base, &off, &sz) && volatile_bases.contains(base) &&
      !expr->flags.volatil) {
    expr->flags.volatil = true;
    changed = true;
  }
  for (RtlExpr& child : expr->children) {
    if (Stage0MarkVolatileMemsOnBaseInExpr(&child, volatile_bases)) {
      changed = true;
    }
  }
  return changed;
}

// Recovers `MEM_VOLATILE_P` (`mem/v`) in Stage 0 (`.12.dbr`) on memory-mapped
// hardware struct pointer bases from post-`sched2` ordering invariants.
void MarkMmioVolatileMemBaseRegs(RtlControlFlowGraph* cfg, const CompilationProfile& profile) {
  if (cfg == nullptr || cfg->rpo_blocks.empty()) {
    return;
  }
  bool gp64 = false;
  bool fp64 = false;
  DetectMipsRegisterWidthFlags(*cfg, &profile, &gp64, &fp64);

  absl::flat_hash_set<int> written_gprs;
  std::vector<std::vector<Stage0PreReorgBlockInsn>> all_parsed(cfg->rpo_blocks.size());
  std::vector<std::vector<Stage0PreReorgExecStep>> all_steps(cfg->rpo_blocks.size());
  std::optional<RtlPatternAndNotes> epilogue_delay_pn;
  bool epilogue_delay_mutated = false;
  std::optional<Stage0PreReorgExecStep> epilogue_delay_step;
  if (cfg->sideband.epilogue_delay_pattern.has_value() &&
      !cfg->sideband.epilogue_delay_pattern->empty()) {
    auto ep_pn = ParsePatternAndNotes(*cfg->sideband.epilogue_delay_pattern);
    if (ep_pn.ok()) {
      epilogue_delay_pn = std::move(*ep_pn);
      Stage0PreReorgExecStep ep_st{
          .insn_idx = 0, .pn = &*epilogue_delay_pn, .mutated = &epilogue_delay_mutated};
      AnalyzeStage0Stmt(epilogue_delay_pn->pattern, gp64, fp64, &ep_st);
      for (int w : ep_st.write_regs) {
        if (w >= 1 && w < 32) {
          written_gprs.insert(w);
        }
      }
      epilogue_delay_step = std::move(ep_st);
    }
  }
  for (size_t b = 0; b < cfg->rpo_blocks.size(); ++b) {
    if (!BuildStage0PreReorgSteps(cfg->rpo_blocks[b], gp64, fp64, &all_parsed[b], &all_steps[b])) {
      return;
    }
    for (const Stage0PreReorgExecStep& st : all_steps[b]) {
      for (int w : st.write_regs) {
        if (w >= 1 && w < 32) {
          written_gprs.insert(w);
        }
      }
      if (Stage0ExprHasCall(st.pn->pattern)) {
        for (int r = 1; r <= 15; ++r) {
          written_gprs.insert(r);
        }
        written_gprs.insert(24);
        written_gprs.insert(25);
        written_gprs.insert(31);
      }
    }
    for (const Stage0PreReorgBlockInsn& item : all_parsed[b]) {
      if (item.is_sequence && item.seq_slot_wrap.flags.in_struct) {
        Stage0PreReorgExecStep slot_step;
        AnalyzeStage0Stmt(item.slot_pn.pattern, gp64, fp64, &slot_step);
        for (int w : slot_step.write_regs) {
          if (w >= 1 && w < 32) {
            written_gprs.insert(w);
          }
        }
      }
    }
  }

  auto byte_ranges_disjoint = [](int64_t off1, int64_t sz1, int64_t off2, int64_t sz2) {
    return (off1 + sz1 <= off2) || (off2 + sz2 <= off1);
  };
  auto sets_disjoint = [](const absl::flat_hash_set<int>& a, const absl::flat_hash_set<int>& b) {
    for (int x : a) {
      if (b.contains(x)) {
        return false;
      }
    }
    return true;
  };

  absl::flat_hash_set<int> volatile_bases;
  for (size_t b = 0; b < all_steps.size(); ++b) {
    absl::flat_hash_set<int> block_exit_used_regs;
    std::vector<Stage0PreReorgExecStep> steps;
    steps.reserve(all_steps[b].size() + 1);
    bool has_return_step = false;
    for (const Stage0PreReorgExecStep& st : all_steps[b]) {
      const RtlOp op = st.pn->pattern.op;
      if (op == RtlOp::kUse) {
        for (int r : st.read_regs) {
          block_exit_used_regs.insert(r);
        }
        continue;
      }
      if (op == RtlOp::kClobber || op == RtlOp::kNote) {
        continue;
      }
      if (op == RtlOp::kReturn) {
        has_return_step = true;
      }
      steps.push_back(st);
    }
    if (cfg->rpo_blocks[b].successors.empty() && !cfg->rpo_blocks[b].has_trailing_barrier &&
        !has_return_step && epilogue_delay_step.has_value()) {
      steps.push_back(*epilogue_delay_step);
    }
    const bool is_exit_block = cfg->rpo_blocks[b].successors.empty() || has_return_step;
    for (size_t s = 1; s + 2 < steps.size(); ++s) {
      int alu_dst = -1;
      int store_s_val = -1;
      int store_next_val = -1;
      int load_dst = -1;
      const RtlExpr* load_mem = nullptr;
      if (!IsStage0SingleCycleIntAluStep(steps[s - 1], &alu_dst) ||
          !IsStage0SimpleRegMemStoreStep(steps[s], &store_s_val) ||
          steps[s].read_regs.contains(alu_dst) ||
          !IsStage0SimpleRegMemStoreStep(steps[s + 1], &store_next_val) ||
          store_next_val != alu_dst ||
          !IsStage0SchedulableLoadStep(steps[s + 2], &load_dst, &load_mem) || load_dst != alu_dst) {
        continue;
      }
      int base_s = -1, base_next = -1, base_load = -1;
      int64_t off_s = 0, sz_s = 0, off_next = 0, sz_next = 0, off_load = 0, sz_load = 0;
      if (!ExtractStage0NonStackBaseDispMem(steps[s].pn->pattern.children[0], &base_s, &off_s,
                                            &sz_s) ||
          !ExtractStage0NonStackBaseDispMem(steps[s + 1].pn->pattern.children[0], &base_next,
                                            &off_next, &sz_next) ||
          !ExtractStage0NonStackBaseDispMem(*load_mem, &base_load, &off_load, &sz_load)) {
        continue;
      }
      if (base_s != base_next || base_s != base_load || written_gprs.contains(base_s)) {
        continue;
      }
      if (!byte_ranges_disjoint(off_s, sz_s, off_next, sz_next) ||
          !byte_ranges_disjoint(off_s, sz_s, off_load, sz_load) ||
          !byte_ranges_disjoint(off_next, sz_next, off_load, sz_load)) {
        continue;
      }
      bool later_writes_s_reads = false;
      bool later_consumes_load = false;
      bool load_dst_overwritten = false;
      for (size_t k = s + 1; k < steps.size(); ++k) {
        if (!sets_disjoint(steps[k].write_regs, steps[s].read_regs)) {
          later_writes_s_reads = true;
          break;
        }
        if (k > s + 2) {
          if (steps[k].read_regs.contains(load_dst)) {
            later_consumes_load = true;
          }
          if (steps[k].write_regs.contains(load_dst)) {
            load_dst_overwritten = true;
          }
        }
      }
      if (!later_consumes_load && !load_dst_overwritten && is_exit_block &&
          block_exit_used_regs.contains(load_dst)) {
        later_consumes_load = true;
      }
      if (!later_writes_s_reads && later_consumes_load) {
        volatile_bases.insert(base_s);
      }
    }
  }

  if (volatile_bases.empty()) {
    return;
  }

  for (size_t b = 0; b < cfg->rpo_blocks.size(); ++b) {
    for (Stage0PreReorgBlockInsn& item : all_parsed[b]) {
      if (item.is_sequence) {
        if (Stage0MarkVolatileMemsOnBaseInExpr(&item.head_pn.pattern, volatile_bases)) {
          item.mutated = true;
        }
        if (Stage0MarkVolatileMemsOnBaseInExpr(&item.slot_pn.pattern, volatile_bases)) {
          item.mutated = true;
        }
      } else if (Stage0MarkVolatileMemsOnBaseInExpr(&item.top_pn.pattern, volatile_bases)) {
        item.mutated = true;
      }
    }
    FlushStage0MutatedBlockInsns(all_parsed[b], &cfg->rpo_blocks[b]);
  }
  if (cfg->sideband.epilogue_delay_pattern.has_value() &&
      !cfg->sideband.epilogue_delay_pattern->empty()) {
    auto ep_pn = ParsePatternAndNotes(*cfg->sideband.epilogue_delay_pattern);
    if (ep_pn.ok() && Stage0MarkVolatileMemsOnBaseInExpr(&ep_pn->pattern, volatile_bases)) {
      cfg->sideband.epilogue_delay_pattern = FormatGccPrintRtlPatternAndNotes(
          ep_pn->pattern, ep_pn->reg_notes, ep_pn->bracketed_notes, /*call_fusage=*/{});
    }
  }
}

// Infers MIPS O32 `call_value` (`(set <ret_reg> (call ...))`) vs void `call`
// (`(call ...)`) from post-call register uses across `cfg`.
void UpgradeCallReturnValues(RtlControlFlowGraph* cfg) {
  auto ret_reg_bit = [](int regno) -> uint32_t {
    if (regno == 2) return 1u;
    if (regno == 3) return 2u;
    if (regno == 32) return 4u;
    if (regno == 33) return 8u;
    return 0u;
  };

  auto analyze_single_expr = [&](const RtlExpr& expr, uint32_t* read_mask, bool* read_r2_di,
                                 bool* read_f0_df, uint32_t* kill_mask, uint32_t* pure_set_mask) {
    *read_mask = 0u;
    *kill_mask = 0u;
    *pure_set_mask = 0u;
    auto scan_reads = [&](const RtlExpr& sub) {
      sub.Visit([&](const RtlExpr& node) {
        if (node.IsReg()) {
          const int r = node.RegNo();
          *read_mask |= ret_reg_bit(r);
          if (r == 2 && node.mode == RtlMachineMode::kDi) {
            *read_r2_di = true;
          } else if (r == 32 && node.mode == RtlMachineMode::kDf) {
            *read_f0_df = true;
          }
        }
      });
    };
    auto mark_def = [&](const RtlExpr& dst) {
      if (!dst.IsReg()) {
        return;
      }
      const int r = dst.RegNo();
      *kill_mask |= ret_reg_bit(r);
      if (dst.mode == RtlMachineMode::kDi || dst.mode == RtlMachineMode::kDf) {
        *kill_mask |= ret_reg_bit(r + 1);
      }
    };
    auto process_elem = [&](const RtlExpr& elem) {
      if (elem.IsSet()) {
        if (elem.SetDest().IsMem()) {
          scan_reads(elem.SetDest());
        }
        scan_reads(elem.SetSrc());
        if (elem.SetSrc().op == RtlOp::kCall) {
          *kill_mask |= 0xfu;
        } else {
          mark_def(elem.SetDest());
          if (elem.SetDest().IsReg()) {
            const int r = elem.SetDest().RegNo();
            *pure_set_mask |= ret_reg_bit(r);
            if (elem.SetDest().mode == RtlMachineMode::kDi ||
                elem.SetDest().mode == RtlMachineMode::kDf) {
              *pure_set_mask |= ret_reg_bit(r + 1);
            }
          }
        }
      } else if (elem.op == RtlOp::kCall) {
        scan_reads(elem);
        *kill_mask |= 0xfu;
      } else if (elem.op == RtlOp::kClobber) {
        if (!elem.children.empty()) {
          mark_def(elem.children[0]);
        }
      } else {
        scan_reads(elem);
      }
    };
    if (expr.IsParallel()) {
      for (const RtlExpr& child : expr.children) {
        process_elem(child);
      }
    } else {
      process_elem(expr);
    }
  };

  uint32_t fn_exit_ret_mask = 0u;
  bool fn_exit_r2_di = false;
  bool fn_exit_f0_df = false;
  bool saw_scalar_r2_exit = false;
  bool saw_scalar_f0_exit = false;
  if (cfg->sideband.epilogue_delay_pattern.has_value()) {
    auto ep = ParsePatternAndNotes(*cfg->sideband.epilogue_delay_pattern);
    if (ep.ok()) {
      uint32_t rm = 0, km = 0, pm = 0;
      bool r2_di = false, f0_df = false;
      analyze_single_expr(ep->pattern, &rm, &r2_di, &f0_df, &km, &pm);
      fn_exit_ret_mask |= pm;
      if ((pm & 2u) != 0u) fn_exit_r2_di = true;
      if ((pm & 8u) != 0u) fn_exit_f0_df = true;
      if ((pm & 1u) != 0u && (pm & 2u) == 0u && !r2_di) saw_scalar_r2_exit = true;
      if ((pm & 4u) != 0u && (pm & 8u) == 0u && !f0_df) saw_scalar_f0_exit = true;
    }
  }
  auto is_empty_exit_block = [&](size_t sb) {
    return sb < cfg->rpo_blocks.size() && cfg->rpo_blocks[sb].instructions.empty() &&
           cfg->rpo_blocks[sb].successors.empty();
  };
  for (size_t b = 0; b < cfg->rpo_blocks.size(); ++b) {
    const auto& blk = cfg->rpo_blocks[b];
    bool reaches_exit_directly = blk.successors.empty();
    bool reaches_exit_on_ft = false;
    bool reaches_exit_on_taken = false;
    for (size_t sb : blk.successors) {
      if (is_empty_exit_block(sb)) {
        if (blk.successors.size() == 1) {
          reaches_exit_directly = true;
        } else if (sb == b + 1) {
          reaches_exit_on_ft = true;
        } else {
          reaches_exit_on_taken = true;
        }
      }
    }
    if ((!reaches_exit_directly && !reaches_exit_on_ft && !reaches_exit_on_taken) ||
        blk.instructions.empty()) {
      continue;
    }
    uint32_t unread_set = 0u;
    for (size_t idx = 0; idx < blk.instructions.size(); ++idx) {
      const std::string& s = blk.instructions[idx];
      auto p = ParsePatternAndNotes(s);
      if (!p.ok()) continue;
      if (p->pattern.op == RtlOp::kSequence && p->pattern.children.size() == 2 &&
          !p->pattern.children[0].children.empty() && !p->pattern.children[1].children.empty()) {
        uint32_t r0 = 0, k0 = 0, p0 = 0, r1 = 0, k1 = 0, p1 = 0;
        bool d0 = false, f0 = false, d1 = false, f1 = false;
        analyze_single_expr(p->pattern.children[0].children[0], &r0, &d0, &f0, &k0, &p0);
        analyze_single_expr(p->pattern.children[1].children[0], &r1, &d1, &f1, &k1, &p1);
        if (p->pattern.children[0].op == RtlOp::kCallInsn) {
          unread_set = 0u;
        } else {
          const uint32_t after_head = (unread_set & ~(r0 | k0)) | p0;
          const uint32_t after_slot = (after_head & ~(r1 | k1)) | p1;
          if (!reaches_exit_directly && idx + 1 == blk.instructions.size()) {
            uint32_t exit_unread = 0u;
            if (reaches_exit_on_taken) {
              exit_unread |= after_slot;
            }
            if (reaches_exit_on_ft) {
              const bool annulled_target =
                  p->pattern.children[0].flags.unchanging && p->pattern.children[1].flags.in_struct;
              exit_unread |= annulled_target ? after_head : after_slot;
            }
            unread_set = exit_unread;
          } else {
            unread_set = after_slot;
          }
        }
      } else {
        uint32_t rm = 0, km = 0, pm = 0;
        bool rd = false, fd = false;
        analyze_single_expr(p->pattern, &rm, &rd, &fd, &km, &pm);
        unread_set = (unread_set & ~(rm | km)) | pm;
      }
    }
    fn_exit_ret_mask |= unread_set;
    if ((unread_set & 2u) != 0u) fn_exit_r2_di = true;
    if ((unread_set & 8u) != 0u) fn_exit_f0_df = true;
    if ((unread_set & 1u) != 0u && (unread_set & 2u) == 0u) saw_scalar_r2_exit = true;
    if ((unread_set & 4u) != 0u && (unread_set & 8u) == 0u) saw_scalar_f0_exit = true;
  }
  if (saw_scalar_r2_exit) {
    fn_exit_ret_mask &= ~2u;
    fn_exit_r2_di = false;
  }
  if (saw_scalar_f0_exit) {
    fn_exit_ret_mask &= ~8u;
    fn_exit_f0_df = false;
  }

  for (size_t b = 0; b < cfg->rpo_blocks.size(); ++b) {
    RtlBasicBlock& blk = cfg->rpo_blocks[b];
    bool blk_changed = false;
    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      auto parsed = ParsePatternAndNotes(blk.instructions[i]);
      if (!parsed.ok()) {
        continue;
      }
      RtlExpr* call_par = nullptr;
      if (parsed->pattern.IsParallel() && parsed->pattern.children.size() == 2 &&
          parsed->pattern.children[0].op == RtlOp::kCall) {
        call_par = &parsed->pattern;
      } else if (parsed->pattern.op == RtlOp::kSequence && parsed->pattern.children.size() == 2 &&
                 parsed->pattern.children[0].op == RtlOp::kCallInsn &&
                 parsed->pattern.children[0].children.size() == 1 &&
                 parsed->pattern.children[0].children[0].IsParallel() &&
                 parsed->pattern.children[0].children[0].children.size() == 2 &&
                 parsed->pattern.children[0].children[0].children[0].op == RtlOp::kCall) {
        call_par = &parsed->pattern.children[0].children[0];
      }
      if (call_par == nullptr) {
        continue;
      }

      uint32_t used_ret_mask = 0u;
      bool used_r2_di = false;
      bool used_f0_df = false;

      using State = std::tuple<size_t, size_t, uint32_t>;
      std::queue<State> q;
      absl::flat_hash_set<State> visited;
      q.emplace(b, i + 1, 0xfu);
      visited.emplace(b, i + 1, 0xfu);

      while (!q.empty()) {
        auto [cb, ci, alive] = q.front();
        q.pop();
        if (alive == 0u || cb >= cfg->rpo_blocks.size()) {
          continue;
        }
        const auto& cblk = cfg->rpo_blocks[cb];
        bool advanced_via_seq = false;
        for (size_t idx = ci; idx < cblk.instructions.size() && alive != 0u; ++idx) {
          auto cp = ParsePatternAndNotes(cblk.instructions[idx]);
          if (!cp.ok()) {
            continue;
          }
          if (cp->pattern.op == RtlOp::kSequence && cp->pattern.children.size() == 2 &&
              !cp->pattern.children[0].children.empty() &&
              !cp->pattern.children[1].children.empty()) {
            const RtlExpr& h_wrap = cp->pattern.children[0];
            const RtlExpr& s_wrap = cp->pattern.children[1];
            uint32_t r0 = 0, k0 = 0, p0 = 0, r1 = 0, k1 = 0, p1 = 0;
            bool d0 = false, f0 = false, d1 = false, f1 = false;
            analyze_single_expr(h_wrap.children[0], &r0, &d0, &f0, &k0, &p0);
            analyze_single_expr(s_wrap.children[0], &r1, &d1, &f1, &k1, &p1);
            if (h_wrap.op == RtlOp::kCallInsn) {
              used_ret_mask |= (r1 & alive);
              if (d1 && (alive & 1u) != 0u) used_r2_di = true;
              if (f1 && (alive & 4u) != 0u) used_f0_df = true;
              alive &= ~k1;
              used_ret_mask |= (r0 & alive);
              alive = 0u;
              break;
            }
            used_ret_mask |= (r0 & alive);
            if (d0 && (alive & 1u) != 0u) used_r2_di = true;
            if (f0 && (alive & 4u) != 0u) used_f0_df = true;
            alive &= ~k0;
            if (idx + 1 == cblk.instructions.size() && h_wrap.flags.unchanging &&
                s_wrap.flags.in_struct) {
              for (size_t succ : cblk.successors) {
                uint32_t next_alive = alive;
                if (succ != cb + 1) {
                  used_ret_mask |= (r1 & next_alive);
                  if (d1 && (next_alive & 1u) != 0u) used_r2_di = true;
                  if (f1 && (next_alive & 4u) != 0u) used_f0_df = true;
                  next_alive &= ~k1;
                }
                if (next_alive != 0u && visited.emplace(succ, 0, next_alive).second) {
                  q.emplace(succ, 0, next_alive);
                }
              }
              advanced_via_seq = true;
              break;
            }
            used_ret_mask |= (r1 & alive);
            if (d1 && (alive & 1u) != 0u) used_r2_di = true;
            if (f1 && (alive & 4u) != 0u) used_f0_df = true;
            alive &= ~k1;
            continue;
          }

          uint32_t rm = 0, km = 0, pm = 0;
          bool rd = false, fd = false;
          analyze_single_expr(cp->pattern, &rm, &rd, &fd, &km, &pm);
          used_ret_mask |= (rm & alive);
          if (rd && (alive & 1u) != 0u) used_r2_di = true;
          if (fd && (alive & 4u) != 0u) used_f0_df = true;
          alive &= ~km;
        }
        if (advanced_via_seq || alive == 0u) {
          continue;
        }
        if (cblk.successors.empty()) {
          if (cfg->sideband.epilogue_delay_pattern.has_value()) {
            auto ep = ParsePatternAndNotes(*cfg->sideband.epilogue_delay_pattern);
            if (ep.ok()) {
              uint32_t rm = 0, km = 0, pm = 0;
              bool rd = false, fd = false;
              analyze_single_expr(ep->pattern, &rm, &rd, &fd, &km, &pm);
              used_ret_mask |= (rm & alive);
              if (rd && (alive & 1u) != 0u) used_r2_di = true;
              if (fd && (alive & 4u) != 0u) used_f0_df = true;
              alive &= ~km;
            }
          }
          used_ret_mask |= (alive & fn_exit_ret_mask);
          if (fn_exit_r2_di && (alive & 2u) != 0u) used_r2_di = true;
          if (fn_exit_f0_df && (alive & 8u) != 0u) used_f0_df = true;
          continue;
        }
        for (size_t succ : cblk.successors) {
          if (visited.emplace(succ, 0, alive).second) {
            q.emplace(succ, 0, alive);
          }
        }
      }

      int ret_regno = -1;
      RtlMachineMode ret_mode = RtlMachineMode::kSi;
      if ((used_ret_mask & 2u) != 0u || ((used_ret_mask & 1u) != 0u && used_r2_di)) {
        ret_regno = 2;
        ret_mode = RtlMachineMode::kDi;
      } else if ((used_ret_mask & 1u) != 0u) {
        ret_regno = 2;
        ret_mode = RtlMachineMode::kSi;
      } else if ((used_ret_mask & 8u) != 0u || ((used_ret_mask & 4u) != 0u && used_f0_df)) {
        ret_regno = 32;
        ret_mode = RtlMachineMode::kDf;
      } else if ((used_ret_mask & 4u) != 0u) {
        ret_regno = 32;
        ret_mode = RtlMachineMode::kSf;
      }

      if (ret_regno >= 0) {
        call_par->children[0] =
            RtlExpr::MakeSet(RtlExpr::MakeReg(ret_mode, ret_regno), call_par->children[0]);
        blk.instructions[i] = parsed->ToGccPrintRtlString();
        blk_changed = true;
      }
    }
    if (blk_changed) {
      blk.SyncRtlInsnsFromInstructions();
    }
  }
}

void AttachReferencedConstantPoolEntries(absl::Span<const RtlBasicBlock> blocks,
                                         absl::Span<const Stage0ConstantPoolEntry> constant_pool,
                                         RtlSidebandState* sideband) {
  if (constant_pool.empty()) {
    return;
  }
  absl::flat_hash_set<std::string> referenced_symbols;
  auto collect_syms = [&](std::string_view pat) {
    auto parsed = ParsePatternAndNotes(pat);
    if (!parsed.ok()) {
      return;
    }
    parsed->pattern.Visit([&](const RtlExpr& node) {
      if (node.op == RtlOp::kSymbolRef && !node.str_val.empty()) {
        referenced_symbols.insert(node.str_val);
      }
    });
  };
  if (sideband->epilogue_delay_pattern.has_value()) {
    collect_syms(*sideband->epilogue_delay_pattern);
  }
  for (const RtlBasicBlock& blk : blocks) {
    for (const std::string& pat : blk.instructions) {
      collect_syms(pat);
    }
  }
  absl::flat_hash_set<std::string> existing_labels;
  for (const Stage0ConstantPoolEntry& cp : sideband->constant_pool) {
    existing_labels.insert(cp.label_name);
  }
  for (const Stage0ConstantPoolEntry& cp : constant_pool) {
    if (cp.section_name == ".lit4" || cp.section_name == ".lit8") {
      continue;
    }
    if (referenced_symbols.contains(cp.label_name) &&
        existing_labels.insert(cp.label_name).second) {
      sideband->constant_pool.push_back(cp);
    }
  }
}

void NormalizeSingleAdduZeroSet(RtlExpr* expr, bool* changed) {
  if (!expr->IsSet() || !expr->SetDest().IsReg() || expr->SetDest().mode != RtlMachineMode::kSi) {
    return;
  }
  const int rd = expr->SetDest().RegNo();
  if (rd <= 0 || rd >= 32) {
    return;
  }
  const RtlExpr& src = expr->SetSrc();
  if (src.op != RtlOp::kPlus || src.mode != RtlMachineMode::kSi || src.children.size() != 2 ||
      !src.children[0].IsReg() || src.children[0].mode != RtlMachineMode::kSi ||
      !src.children[1].IsReg() || src.children[1].mode != RtlMachineMode::kSi) {
    return;
  }
  const int rs = src.children[0].RegNo();
  const int rt = src.children[1].RegNo();
  if (rs == 0 && rt == 0) {
    expr->MutableSetSrc() = RtlExpr::MakeConstInt(0);
    *changed = true;
  } else if (rt == 0 && rs > 0 && rs < 32) {
    RtlExpr replacement = src.children[0];
    expr->MutableSetSrc() = std::move(replacement);
    *changed = true;
  } else if (rs == 0 && rt > 0 && rt < 32) {
    RtlExpr replacement = src.children[1];
    expr->MutableSetSrc() = std::move(replacement);
    *changed = true;
  }
}

void NormalizeMips12AdduZeroMoves(RtlControlFlowGraph* cfg) {
  auto normalize_pat_str = [](std::string* pat_str) -> bool {
    auto parsed = ParsePatternAndNotes(*pat_str);
    if (!parsed.ok()) {
      return false;
    }
    bool changed = false;
    if (parsed->pattern.op == RtlOp::kSequence && parsed->pattern.children.size() == 2) {
      if (!parsed->pattern.children[0].children.empty()) {
        NormalizeSingleAdduZeroSet(&parsed->pattern.children[0].children[0], &changed);
      }
      if (!parsed->pattern.children[1].children.empty()) {
        NormalizeSingleAdduZeroSet(&parsed->pattern.children[1].children[0], &changed);
      }
    } else {
      NormalizeSingleAdduZeroSet(&parsed->pattern, &changed);
    }
    if (changed) {
      *pat_str = parsed->ToGccPrintRtlString();
    }
    return changed;
  };
  if (cfg->sideband.epilogue_delay_pattern.has_value()) {
    normalize_pat_str(&*cfg->sideband.epilogue_delay_pattern);
  }
  for (auto& blk : cfg->rpo_blocks) {
    bool blk_changed = false;
    for (std::string& pat : blk.instructions) {
      if (normalize_pat_str(&pat)) {
        blk_changed = true;
      }
    }
    if (blk_changed) {
      blk.SyncRtlInsnsFromInstructions();
    }
  }
}

bool FoldFinalScanMultiInsnTemplates(RtlControlFlowGraph* cfg, const CompilationProfile& profile) {
  if (cfg == nullptr || cfg->rpo_blocks.empty()) {
    return false;
  }
  const ParsedCompilerFlags flags = ParseCompilationProfileFlags(profile);
  bool gp64 = false;
  bool fp64 = false;
  DetectMipsRegisterWidthFlags(*cfg, &profile, &gp64, &fp64);

  std::vector<std::vector<RtlPatternAndNotes>> parsed_blocks(cfg->rpo_blocks.size());
  for (size_t b = 0; b < cfg->rpo_blocks.size(); ++b) {
    parsed_blocks[b].reserve(cfg->rpo_blocks[b].instructions.size());
    for (size_t i = 0; i < cfg->rpo_blocks[b].instructions.size(); ++i) {
      auto parsed = ParsePatternAndNotes(cfg->rpo_blocks[b].instructions[i]);
      if (!parsed.ok()) {
        return false;
      }
      if (i < cfg->rpo_blocks[b].rtl_insns.size() &&
          cfg->rpo_blocks[b].rtl_insns[i].has_explicit_call_fusage &&
          !parsed->has_explicit_call_fusage) {
        parsed->has_explicit_call_fusage = true;
        parsed->call_fusage = cfg->rpo_blocks[b].rtl_insns[i].call_fusage;
      }
      parsed_blocks[b].push_back(std::move(*parsed));
    }
  }
  std::optional<RtlPatternAndNotes> epilogue_delay_pn;
  if (cfg->sideband.epilogue_delay_pattern.has_value() &&
      !cfg->sideband.epilogue_delay_pattern->empty()) {
    auto parsed = ParsePatternAndNotes(*cfg->sideband.epilogue_delay_pattern);
    if (!parsed.ok()) {
      return false;
    }
    epilogue_delay_pn = std::move(*parsed);
  }

  bool folded_any = false;
  absl::flat_hash_set<int> compare_di_pairs;
  absl::flat_hash_set<int> existing_di_regs;
  bool has_di_witness = false;
  if (!gp64) {
    for (size_t b = 0; b < parsed_blocks.size(); ++b) {
      if (TryFoldGp32DiMultiInsnIdiomsInBlock(*cfg, b, /*mark_newly_folded_sentinel=*/false,
                                              &parsed_blocks[b],
                                              /*compute_live_in_when_sideband_empty=*/
                                              flags.opt_level <= 0)) {
        folded_any = true;
      }
    }
    CollectCompareDiamondDiPairs(parsed_blocks, &compare_di_pairs);
    bool has_any_di = false;
    CollectExistingDiEvenRegs(parsed_blocks, &existing_di_regs, &has_any_di);
    has_di_witness = has_any_di || !compare_di_pairs.empty();
  }

  const MipsMovstrsiFoldOptions movstrsi_opts{
      .gp64 = gp64,
      .fp64 = fp64,
      .has_di_witness = has_di_witness,
      .skip_unfilled_dbr_jump_tail = flags.flag_delayed_branch,
      .allow_delay_slot_split_movstrsi = (flags.opt_level > 0),
      .mark_newly_folded_sentinel = false,
      .compute_live_in_when_sideband_empty = (flags.opt_level <= 0),
  };
  for (size_t b = 0; b < parsed_blocks.size(); ++b) {
    std::optional<RtlPatternAndNotes>* ep_ptr =
        (b + 1 == parsed_blocks.size()) ? &epilogue_delay_pn : nullptr;
    if (TryFoldMovstrsiBlockMovesInBlock(*cfg, b, movstrsi_opts, &parsed_blocks[b], ep_ptr)) {
      folded_any = true;
    }
  }

  if (!gp64) {
    const int frame_size = DetectStackFrameSize(*cfg);
    if (has_di_witness) {
      const MipsWitnessedDiFoldOptions witnessed_opts{
          .allow_noncontiguous_reg_move = false,
          .mark_newly_folded_sentinel = false,
      };
      if (TryFoldGp32DiWitnessedIdioms(*cfg, frame_size, compare_di_pairs, existing_di_regs,
                                       witnessed_opts, &parsed_blocks)) {
        folded_any = true;
      }
    }
    if (!fp64 &&
        TryFoldGp32DfInGprPairMoves(*cfg, frame_size, compare_di_pairs, existing_di_regs,
                                    /*mark_newly_folded_sentinel=*/false, &parsed_blocks)) {
      folded_any = true;
    }
  }

  if (!folded_any) {
    return false;
  }

  for (size_t b = 0; b < cfg->rpo_blocks.size(); ++b) {
    cfg->rpo_blocks[b].instructions.clear();
    cfg->rpo_blocks[b].instructions.reserve(parsed_blocks[b].size());
    for (const auto& pn : parsed_blocks[b]) {
      cfg->rpo_blocks[b].instructions.push_back(FormatGccPrintRtlPatternAndNotes(
          pn.pattern, pn.reg_notes, pn.bracketed_notes, /*call_fusage=*/{}));
    }
    cfg->rpo_blocks[b].SyncRtlInsnsFromInstructions();
    for (size_t i = 0; i < cfg->rpo_blocks[b].rtl_insns.size() && i < parsed_blocks[b].size();
         ++i) {
      if (parsed_blocks[b][i].has_explicit_call_fusage &&
          !cfg->rpo_blocks[b].rtl_insns[i].has_explicit_call_fusage) {
        cfg->rpo_blocks[b].rtl_insns[i].has_explicit_call_fusage = true;
        cfg->rpo_blocks[b].rtl_insns[i].call_fusage = parsed_blocks[b][i].call_fusage;
      }
    }
  }
  if (epilogue_delay_pn.has_value()) {
    cfg->sideband.epilogue_delay_pattern =
        FormatGccPrintRtlPatternAndNotes(epilogue_delay_pn->pattern, epilogue_delay_pn->reg_notes,
                                         epilogue_delay_pn->bracketed_notes, /*call_fusage=*/{});
  }
  return true;
}

void StripRegDeadAndUnusedNotesOnCfg(RtlControlFlowGraph* cfg) {
  auto strip_notes = [](std::vector<RtlRegNote>* notes) -> bool {
    const size_t before = notes->size();
    notes->erase(std::remove_if(notes->begin(), notes->end(),
                                [](const RtlRegNote& n) {
                                  return n.kind == RtlMachineMode::kRegDead ||
                                         n.kind == RtlMachineMode::kRegUnused;
                                }),
                 notes->end());
    return notes->size() != before;
  };
  auto strip_str = [&](std::string* s) -> bool {
    auto p = ParsePatternAndNotes(*s);
    if (!p.ok()) {
      return false;
    }
    bool changed = strip_notes(&p->reg_notes);
    if (p->pattern.op == RtlOp::kSequence && p->pattern.children.size() == 2) {
      auto sub0 = RtlPatternAndNotes::FromSequenceSubInsn(p->pattern.children[0]);
      auto sub1 = RtlPatternAndNotes::FromSequenceSubInsn(p->pattern.children[1]);
      if (sub0.ok() && sub1.ok()) {
        const bool c0 = strip_notes(&sub0->reg_notes);
        const bool c1 = strip_notes(&sub1->reg_notes);
        if (c0 || c1) {
          p->pattern = RtlExpr::MakeSequence({
              RtlExpr::MakeInsnWrapper(p->pattern.children[0].op, p->pattern.children[0].flags,
                                       *sub0),
              RtlExpr::MakeInsnWrapper(p->pattern.children[1].op, p->pattern.children[1].flags,
                                       *sub1),
          });
          changed = true;
        }
      }
    }
    if (changed) {
      *s = p->ToGccPrintRtlString();
    }
    return changed;
  };
  if (cfg->sideband.epilogue_delay_pattern.has_value() &&
      !cfg->sideband.epilogue_delay_pattern->empty()) {
    strip_str(&*cfg->sideband.epilogue_delay_pattern);
  }
  for (auto& blk : cfg->rpo_blocks) {
    bool blk_changed = false;
    for (std::string& s : blk.instructions) {
      if (strip_str(&s)) {
        blk_changed = true;
      }
    }
    if (blk_changed) {
      blk.SyncRtlInsnsFromInstructions();
    }
  }
}

bool IsGccSynthesizedLibcallSymbol(std::string_view sym, bool is_void_call) {
  if (sym == "bcopy" || sym == "__gcc_bcmp" || sym == "cacheflush") {
    return true;
  }
  if (is_void_call && sym == "memcpy") {
    return true;
  }
  static constexpr std::string_view kLibcallPrefixes[] = {
      "__add",    "__sub",  "__mul",    "__div",   "__udiv", "__mod",      "__umod",
      "__ftrunc", "__and",  "__ior",    "__xor",   "__ashl", "__ashr",     "__lshr",
      "__min",    "__max",  "__umin",   "__umax",  "__neg",  "__one_cmpl", "__ffs",
      "__cmp",    "__ucmp", "__extend", "__trunc", "__eq",   "__ne",       "__gt",
      "__ge",     "__lt",   "__le",     "__float", "__fix",
  };
  for (std::string_view pfx : kLibcallPrefixes) {
    if (absl::StartsWith(sym, pfx) && sym.size() > pfx.size()) {
      const char tail = sym.back();
      if (tail == '2' || tail == '3' || tail == '4' || tail == 'i' || tail == 'f') {
        return true;
      }
    }
  }
  return false;
}

void UpgradeCallOutgoingArgBytes(RtlControlFlowGraph* cfg, const CompilationProfile& profile) {
  if (cfg == nullptr || cfg->rpo_blocks.empty()) {
    return;
  }
  bool gp64 = false;
  bool fp64 = false;
  DetectMipsRegisterWidthFlags(*cfg, &profile, &gp64, &fp64);
  const int min_reg_parm_bytes = gp64 ? 32 : 16;

  std::vector<std::vector<RtlPatternAndNotes>> blocks(cfg->rpo_blocks.size());
  bool any_call = false;
  for (size_t b = 0; b < cfg->rpo_blocks.size(); ++b) {
    blocks[b].reserve(cfg->rpo_blocks[b].instructions.size());
    for (const std::string& s : cfg->rpo_blocks[b].instructions) {
      auto p = ParsePatternAndNotes(s);
      if (!p.ok()) {
        return;
      }
      if (ExprContainsCall(p->pattern)) {
        any_call = true;
      }
      blocks[b].push_back(std::move(*p));
    }
  }
  if (!any_call) {
    return;
  }

  // Determine `non_reg_size = args_size + var_size`: either from `sideband` (populated by
  // `ExtractFunctionPrologueEpilogue`) or from block 0's prologue `$sp` decrement / lowest save.
  int non_reg_size = cfg->sideband.outgoing_args_size + cfg->sideband.frame_size_bytes;
  if (non_reg_size <= 0) {
    const int tsize = DetectStackFrameSize(*cfg);
    if (tsize > 0) {
      int min_save_off = tsize;
      for (const auto& pn : blocks[0]) {
        const RtlExpr& pat = pn.pattern;
        int breg = -1;
        int64_t off = 0;
        if (pat.IsSet() && pat.SetDest().IsMem() && pat.SetDest().children.size() == 1 &&
            pat.SetSrc().IsReg() &&
            ExtractBasePlusConstAddr(pat.SetDest().children[0], &breg, &off) && breg == 29 &&
            off >= 0 && off < tsize) {
          const int sr = pat.SetSrc().RegNo();
          if (IsSavedGprOrRaRegno(sr) || (sr >= 52 && sr <= 63)) {
            min_save_off = std::min(min_save_off, static_cast<int>(off));
          }
        }
      }
      non_reg_size = min_save_off;
    }
  }

  std::vector<bool> is_local_byte(static_cast<size_t>(std::max(0, non_reg_size)), false);
  auto mark_local = [&](int64_t off, int bytes) {
    for (int k = 0; k < bytes; ++k) {
      const int64_t idx = off + k;
      if (idx >= 0 && idx < non_reg_size) {
        is_local_byte[static_cast<size_t>(idx)] = true;
      }
    }
  };

  if (non_reg_size > min_reg_parm_bytes) {
    // Identify which blocks can reach at least one `CALL` along CFG edges.
    const size_t nb = blocks.size();
    std::vector<std::vector<size_t>> preds(nb);
    std::vector<bool> block_has_call(nb, false);
    std::vector<bool> block_reaches_call(nb, false);
    std::queue<size_t> q;
    for (size_t b = 0; b < nb; ++b) {
      for (size_t s : cfg->rpo_blocks[b].successors) {
        if (s < nb) {
          preds[s].push_back(b);
        }
      }
      for (const auto& pn : blocks[b]) {
        if (ExprContainsCall(pn.pattern)) {
          block_has_call[b] = true;
          break;
        }
      }
      if (block_has_call[b]) {
        block_reaches_call[b] = true;
        q.push(b);
      }
    }
    while (!q.empty()) {
      const size_t cur = q.front();
      q.pop();
      for (size_t p : preds[cur]) {
        if (!block_reaches_call[p]) {
          block_reaches_call[p] = true;
          q.push(p);
        }
      }
    }

    auto check_single_pat_locals = [&](const RtlExpr& pat, bool after_last_call_without_succ_call,
                                       std::array<int, 32>* sp_ptr_off) {
      int movstr_dst = -1;
      int64_t movstr_dst_off = 0;
      int movstr_src = -1;
      int64_t movstr_src_off = 0;
      int movstr_bytes = 0;
      if (ExtractMovstrsiAddrsAndBytes(pat, &movstr_dst, &movstr_dst_off, &movstr_src,
                                       &movstr_src_off, &movstr_bytes)) {
        const bool src_is_fp = cfg->sideband.frame_pointer_needed && movstr_src == 30;
        if ((movstr_src == 29 || src_is_fp) && movstr_src_off >= min_reg_parm_bytes &&
            movstr_src_off < non_reg_size) {
          mark_local(movstr_src_off, movstr_bytes);
        } else if (movstr_src >= 1 && movstr_src < 32 &&
                   (*sp_ptr_off)[movstr_src] >= min_reg_parm_bytes) {
          const int64_t eff_src = (*sp_ptr_off)[movstr_src] + movstr_src_off;
          if (eff_src >= min_reg_parm_bytes && eff_src < non_reg_size) {
            mark_local(eff_src, non_reg_size - static_cast<int>(eff_src));
          }
        }
        const bool dst_is_fp = cfg->sideband.frame_pointer_needed && movstr_dst == 30;
        if ((movstr_dst == 29 || dst_is_fp) && (dst_is_fp || after_last_call_without_succ_call) &&
            movstr_dst_off >= min_reg_parm_bytes && movstr_dst_off < non_reg_size) {
          mark_local(movstr_dst_off, movstr_bytes);
        } else if (movstr_dst >= 1 && movstr_dst < 32 &&
                   (*sp_ptr_off)[movstr_dst] >= min_reg_parm_bytes &&
                   after_last_call_without_succ_call) {
          const int64_t eff_dst = (*sp_ptr_off)[movstr_dst] + movstr_dst_off;
          if (eff_dst >= min_reg_parm_bytes && eff_dst < non_reg_size) {
            mark_local(eff_dst, movstr_bytes);
          }
        }
        for (size_t c = 1; c <= 4; ++c) {
          if (pat.children[c].op == RtlOp::kClobber && !pat.children[c].children.empty() &&
              pat.children[c].children[0].IsReg()) {
            const int cr = pat.children[c].children[0].RegNo();
            if (cr >= 1 && cr < 32) {
              (*sp_ptr_off)[cr] = -1;
            }
          }
        }
        return;
      }

      // Check if `pat` sets `dreg = sp/fp + off`.
      int dreg = -1;
      int breg = -1;
      int64_t off = 0;
      if (pat.IsSet() && IsPlainSiGpr(pat.SetDest(), &dreg) &&
          ExtractBasePlusConstAddr(pat.SetSrc(), &breg, &off) && off != 0) {
        if (cfg->sideband.frame_pointer_needed && breg == 30 && off >= min_reg_parm_bytes &&
            off < non_reg_size) {
          mark_local(off, non_reg_size - static_cast<int>(off));
          (*sp_ptr_off)[dreg] = -1;
          return;
        }
        if (breg == 29 && dreg != 29 && !(cfg->sideband.frame_pointer_needed && dreg == 30) &&
            off >= min_reg_parm_bytes && off < non_reg_size) {
          (*sp_ptr_off)[dreg] = static_cast<int>(off);
          return;
        }
      }

      // Any other read of a register holding `$sp + off` (or passing it in `$4..$7` into a `CALL`)
      // means its address escaped or was used as a local variable pointer!
      MipsHardRegSet defs, uses;
      CollectHardRegDefsAndUses(pat, gp64, fp64, &defs, &uses);
      if (ExprContainsCall(pat)) {
        for (int ar = 4; ar <= 7; ++ar) {
          if ((*sp_ptr_off)[ar] >= min_reg_parm_bytes) {
            mark_local((*sp_ptr_off)[ar], non_reg_size - (*sp_ptr_off)[ar]);
          }
        }
        static constexpr int kCallClobberedGprs[] = {
            1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 31,
        };
        for (int cr : kCallClobberedGprs) {
          (*sp_ptr_off)[cr] = -1;
        }
      }
      for (int r = 1; r < 32; ++r) {
        if (uses.Test(r) && (*sp_ptr_off)[r] >= min_reg_parm_bytes) {
          mark_local((*sp_ptr_off)[r], non_reg_size - (*sp_ptr_off)[r]);
        }
      }

      auto inspect_stmt_mems = [&](const RtlExpr& stmt) {
        if (stmt.IsSet() && stmt.SetDest().IsMem() && stmt.SetDest().children.size() == 1) {
          int mb = -1;
          int64_t moff = 0;
          const int sz = ModeAccessByteSize(stmt.SetDest().mode);
          if (sz > 0 && ExtractBasePlusConstAddr(stmt.SetDest().children[0], &mb, &moff)) {
            const bool is_fp = cfg->sideband.frame_pointer_needed && mb == 30;
            if ((is_fp || stmt.SetDest().flags.volatil || after_last_call_without_succ_call) &&
                (mb == 29 || is_fp) && moff >= min_reg_parm_bytes && moff < non_reg_size) {
              mark_local(moff, sz);
            }
          }
          stmt.SetSrc().Visit([&](const RtlExpr& n) {
            if (n.IsMem() && n.children.size() == 1) {
              int lb = -1;
              int64_t loff = 0;
              const int lsz = std::max(1, ModeAccessByteSize(n.mode));
              if (ExtractBasePlusConstAddr(n.children[0], &lb, &loff) &&
                  (lb == 29 || (cfg->sideband.frame_pointer_needed && lb == 30)) &&
                  loff >= min_reg_parm_bytes && loff < non_reg_size) {
                mark_local(loff, lsz);
              }
            }
          });
          return;
        }
        stmt.Visit([&](const RtlExpr& n) {
          if (n.IsMem() && n.children.size() == 1) {
            int lb = -1;
            int64_t loff = 0;
            const int lsz = std::max(1, ModeAccessByteSize(n.mode));
            if (ExtractBasePlusConstAddr(n.children[0], &lb, &loff) &&
                (lb == 29 || (cfg->sideband.frame_pointer_needed && lb == 30)) &&
                loff >= min_reg_parm_bytes && loff < non_reg_size) {
              mark_local(loff, lsz);
            }
          }
        });
      };
      if (pat.IsParallel()) {
        for (const RtlExpr& el : pat.children) {
          inspect_stmt_mems(el);
        }
      } else {
        inspect_stmt_mems(pat);
      }

      for (int r = 1; r < 32; ++r) {
        if (defs.Test(r)) {
          (*sp_ptr_off)[r] = -1;
        }
      }
    };

    for (size_t b = 0; b < nb; ++b) {
      bool succ_reaches_call = false;
      for (size_t s : cfg->rpo_blocks[b].successors) {
        if (s < nb && block_reaches_call[s]) {
          succ_reaches_call = true;
          break;
        }
      }
      size_t last_call_idx = blocks[b].size();
      for (size_t i = 0; i < blocks[b].size(); ++i) {
        if (ExprContainsCall(blocks[b][i].pattern)) {
          last_call_idx = i;
        }
      }
      std::array<int, 32> sp_ptr_off;
      sp_ptr_off.fill(-1);
      for (size_t i = 0; i < blocks[b].size(); ++i) {
        const bool cannot_reach_call =
            !block_reaches_call[b] ||
            (!succ_reaches_call && (last_call_idx == blocks[b].size() || i > last_call_idx));
        const RtlExpr& pat = blocks[b][i].pattern;
        if (pat.op == RtlOp::kSequence && pat.children.size() >= 2 &&
            !pat.children[0].children.empty() && !pat.children[1].children.empty()) {
          const bool is_call_seq = pat.children[0].op == RtlOp::kCallInsn ||
                                   ExprContainsCall(pat.children[0].children[0]);
          if (is_call_seq) {
            check_single_pat_locals(pat.children[1].children[0], false, &sp_ptr_off);
            check_single_pat_locals(pat.children[0].children[0], false, &sp_ptr_off);
          } else {
            check_single_pat_locals(pat.children[0].children[0], cannot_reach_call, &sp_ptr_off);
            check_single_pat_locals(pat.children[1].children[0], cannot_reach_call, &sp_ptr_off);
          }
        } else {
          check_single_pat_locals(pat, cannot_reach_call, &sp_ptr_off);
        }
      }
      // If any `$sp + off` pointer is still live in a callee-saved or live-out register at block
      // end without being consumed solely by `movstrsi` in this block, conservatively mark it
      // local unless no successor reads it—checking `basic_block_live_at_start` of successors!
      for (int r = 1; r < 32; ++r) {
        if (sp_ptr_off[r] >= min_reg_parm_bytes) {
          bool live_in_succ = false;
          for (size_t s : cfg->rpo_blocks[b].successors) {
            auto it = cfg->sideband.basic_block_live_at_start.find(static_cast<int>(s));
            if (it != cfg->sideband.basic_block_live_at_start.end() &&
                std::find(it->second.begin(), it->second.end(), r) != it->second.end()) {
              live_in_succ = true;
              break;
            }
          }
          if (live_in_succ) {
            mark_local(sp_ptr_off[r], non_reg_size - sp_ptr_off[r]);
          }
        }
      }
    }
  }

  auto record_arg_store_end = [&](const RtlExpr& pat, std::array<int, 32>* sp_ptr_off,
                                  int* cur_end) {
    if (non_reg_size <= min_reg_parm_bytes) {
      return;
    }
    int movstr_dst = -1;
    int64_t movstr_dst_off = 0;
    int movstr_src = -1;
    int64_t movstr_src_off = 0;
    int movstr_bytes = 0;
    if (ExtractMovstrsiAddrsAndBytes(pat, &movstr_dst, &movstr_dst_off, &movstr_src,
                                     &movstr_src_off, &movstr_bytes)) {
      int64_t start = -1;
      if (movstr_dst == 29) {
        start = movstr_dst_off;
      } else if (movstr_dst >= 1 && movstr_dst < 32 &&
                 (*sp_ptr_off)[movstr_dst] >= min_reg_parm_bytes) {
        start = (*sp_ptr_off)[movstr_dst] + movstr_dst_off;
      }
      if (start >= min_reg_parm_bytes && start + movstr_bytes <= non_reg_size) {
        bool overlaps_local = false;
        for (int k = 0; k < movstr_bytes; ++k) {
          if (is_local_byte[static_cast<size_t>(start + k)]) {
            overlaps_local = true;
            break;
          }
        }
        if (!overlaps_local) {
          *cur_end = std::max(*cur_end, static_cast<int>(start) + movstr_bytes);
        }
      }
      for (size_t c = 1; c <= 4; ++c) {
        if (pat.children[c].op == RtlOp::kClobber && !pat.children[c].children.empty() &&
            pat.children[c].children[0].IsReg()) {
          const int cr = pat.children[c].children[0].RegNo();
          if (cr >= 1 && cr < 32) {
            (*sp_ptr_off)[cr] = -1;
          }
        }
      }
      return;
    }

    int dreg = -1;
    int breg = -1;
    int64_t off = 0;
    if (pat.IsSet() && IsPlainSiGpr(pat.SetDest(), &dreg) &&
        ExtractBasePlusConstAddr(pat.SetSrc(), &breg, &off) && breg == 29 && dreg != 29 &&
        !(cfg->sideband.frame_pointer_needed && dreg == 30) && off >= min_reg_parm_bytes &&
        off < non_reg_size) {
      (*sp_ptr_off)[dreg] = static_cast<int>(off);
      return;
    }

    auto check_store = [&](const RtlExpr& stmt) {
      if (!stmt.IsSet() || !stmt.SetDest().IsMem() || stmt.SetDest().flags.volatil ||
          stmt.SetDest().children.size() != 1) {
        return;
      }
      int mb = -1;
      int64_t moff = 0;
      const int sz = ModeAccessByteSize(stmt.SetDest().mode);
      if (sz > 0 && ExtractBasePlusConstAddr(stmt.SetDest().children[0], &mb, &moff) && mb == 29 &&
          moff >= min_reg_parm_bytes && moff + sz <= non_reg_size) {
        bool overlaps_local = false;
        for (int k = 0; k < sz; ++k) {
          if (is_local_byte[static_cast<size_t>(moff + k)]) {
            overlaps_local = true;
            break;
          }
        }
        if (!overlaps_local) {
          *cur_end = std::max(*cur_end, static_cast<int>(moff) + sz);
        }
      }
    };
    if (pat.IsParallel()) {
      for (const RtlExpr& el : pat.children) {
        check_store(el);
      }
    } else {
      check_store(pat);
    }

    MipsHardRegSet defs, uses;
    CollectHardRegDefsAndUses(pat, gp64, fp64, &defs, &uses);
    for (int r = 1; r < 32; ++r) {
      if (defs.Test(r)) {
        (*sp_ptr_off)[r] = -1;
      }
    }
  };

  auto find_mutable_call = [](RtlExpr* pat, bool* out_is_void_call) -> RtlExpr* {
    *out_is_void_call = false;
    auto check_core = [&](RtlExpr* core) -> RtlExpr* {
      if (core->op == RtlOp::kCall) {
        *out_is_void_call = true;
        return core;
      }
      if (core->IsSet() && core->MutableSetSrc().op == RtlOp::kCall) {
        *out_is_void_call = false;
        return &core->MutableSetSrc();
      }
      if (core->IsParallel() && !core->children.empty()) {
        if (core->children[0].op == RtlOp::kCall) {
          *out_is_void_call = true;
          return &core->children[0];
        }
        if (core->children[0].IsSet() && core->children[0].MutableSetSrc().op == RtlOp::kCall) {
          *out_is_void_call = false;
          return &core->children[0].MutableSetSrc();
        }
      }
      return nullptr;
    };
    return check_core(pat);
  };

  // Fixed-point forward propagation of `in_arg_end[b]` across basic blocks.
  const size_t nb = blocks.size();
  std::vector<int> in_arg_end(nb, 0);
  auto simulate_block = [&](size_t b, int start_end, bool apply_rewrite, int* max_call_bytes_seen,
                            bool* block_mutated) -> int {
    int cur_end = start_end;
    std::array<int, 32> sp_ptr_off;
    sp_ptr_off.fill(-1);
    for (size_t i = 0; i < blocks[b].size(); ++i) {
      RtlExpr& pat = blocks[b][i].pattern;
      if (pat.op == RtlOp::kSequence && pat.children.size() >= 2 &&
          !pat.children[0].children.empty() && !pat.children[1].children.empty()) {
        const bool is_call_seq =
            pat.children[0].op == RtlOp::kCallInsn || ExprContainsCall(pat.children[0].children[0]);
        if (is_call_seq) {
          record_arg_store_end(pat.children[1].children[0], &sp_ptr_off, &cur_end);
          bool is_void = false;
          RtlExpr* call_node = find_mutable_call(&pat.children[0].children[0], &is_void);
          if (call_node != nullptr && call_node->children.size() >= 2 &&
              call_node->children[1].IsConstInt()) {
            std::string_view callee_sym;
            if (call_node->children[0].IsMem() && !call_node->children[0].children.empty() &&
                call_node->children[0].children[0].op == RtlOp::kSymbolRef) {
              callee_sym = call_node->children[0].children[0].str_val;
            }
            const bool is_libcall =
                !callee_sym.empty() && IsGccSynthesizedLibcallSymbol(callee_sym, is_void);
            int call_bytes = std::max(min_reg_parm_bytes, (cur_end + 7) & ~7);
            if (non_reg_size >= min_reg_parm_bytes) {
              call_bytes = std::min(call_bytes, non_reg_size);
            }
            if (max_call_bytes_seen != nullptr) {
              *max_call_bytes_seen = std::max(*max_call_bytes_seen, call_bytes);
            }
            if (apply_rewrite && !is_libcall && call_node->children[1].int_val == 0) {
              call_node->children[1].int_val = call_bytes;
              if (block_mutated != nullptr) {
                *block_mutated = true;
              }
            }
          }
          cur_end = 0;
          sp_ptr_off.fill(-1);
          continue;
        }
        record_arg_store_end(pat.children[0].children[0], &sp_ptr_off, &cur_end);
        record_arg_store_end(pat.children[1].children[0], &sp_ptr_off, &cur_end);
        continue;
      }
      if (ExprContainsCall(pat)) {
        bool is_void = false;
        RtlExpr* call_node = find_mutable_call(&pat, &is_void);
        if (call_node != nullptr && call_node->children.size() >= 2 &&
            call_node->children[1].IsConstInt()) {
          std::string_view callee_sym;
          if (call_node->children[0].IsMem() && !call_node->children[0].children.empty() &&
              call_node->children[0].children[0].op == RtlOp::kSymbolRef) {
            callee_sym = call_node->children[0].children[0].str_val;
          }
          const bool is_libcall =
              !callee_sym.empty() && IsGccSynthesizedLibcallSymbol(callee_sym, is_void);
          int call_bytes = std::max(min_reg_parm_bytes, (cur_end + 7) & ~7);
          if (non_reg_size >= min_reg_parm_bytes) {
            call_bytes = std::min(call_bytes, non_reg_size);
          }
          if (max_call_bytes_seen != nullptr) {
            *max_call_bytes_seen = std::max(*max_call_bytes_seen, call_bytes);
          }
          if (apply_rewrite && !is_libcall && call_node->children[1].int_val == 0) {
            call_node->children[1].int_val = call_bytes;
            if (block_mutated != nullptr) {
              *block_mutated = true;
            }
          }
        }
        cur_end = 0;
        sp_ptr_off.fill(-1);
        continue;
      }
      record_arg_store_end(pat, &sp_ptr_off, &cur_end);
    }
    return cur_end;
  };

  bool prop_changed = true;
  while (prop_changed) {
    prop_changed = false;
    for (size_t b = 0; b < nb; ++b) {
      const int out_end =
          simulate_block(b, in_arg_end[b], /*apply_rewrite=*/false, nullptr, nullptr);
      for (size_t s : cfg->rpo_blocks[b].successors) {
        if (s < nb && out_end > in_arg_end[s]) {
          in_arg_end[s] = out_end;
          prop_changed = true;
        }
      }
    }
  }

  int max_call_bytes_seen = 0;
  for (size_t b = 0; b < nb; ++b) {
    bool block_mutated = false;
    simulate_block(b, in_arg_end[b], /*apply_rewrite=*/true, &max_call_bytes_seen, &block_mutated);
    if (block_mutated) {
      for (size_t i = 0; i < blocks[b].size(); ++i) {
        cfg->rpo_blocks[b].instructions[i] = blocks[b][i].ToGccPrintRtlString();
      }
      cfg->rpo_blocks[b].SyncRtlInsnsFromInstructions();
    }
  }

  if (non_reg_size >= min_reg_parm_bytes &&
      max_call_bytes_seen > cfg->sideband.outgoing_args_size &&
      cfg->sideband.outgoing_args_size + cfg->sideband.frame_size_bytes == non_reg_size) {
    cfg->sideband.outgoing_args_size = std::min(non_reg_size, max_call_bytes_seen);
    cfg->sideband.frame_size_bytes = non_reg_size - cfg->sideband.outgoing_args_size;
  }
}

}  // namespace

RtlControlFlowGraph InvertFinalScanInsnLinesToCfg(
    absl::Span<const std::string> asm_lines, const RtlSidebandState& sideband,
    absl::Span<const Stage0ResolvedJumpTable> jump_tables,
    absl::Span<const Stage0ConstantPoolEntry> constant_pool, const CompilationProfile* profile) {
  if (asm_lines.empty()) {
    return {};
  }

  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kDbr;
  cfg.sideband = sideband;

  bool gp64 = false;
  bool fp64 = false;
  if (profile != nullptr) {
    DetectMipsRegisterWidthFlags(cfg, profile, &gp64, &fp64);
  } else {
    for (const std::string& ln : asm_lines) {
      if (absl::StartsWith(ln, "dadd") || absl::StartsWith(ln, "dsll") ||
          absl::StartsWith(ln, "dsrl") || absl::StartsWith(ln, "dsra") ||
          absl::StartsWith(ln, "dmtc1") || absl::StartsWith(ln, "dmfc1") ||
          absl::StartsWith(ln, "ld\t") || absl::StartsWith(ln, "ld ") ||
          absl::StartsWith(ln, "sd\t") || absl::StartsWith(ln, "sd ")) {
        fp64 = true;
        break;
      }
    }
  }

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
                                ? RtlExpr::MakeInsnWrapper(RtlOp::kCallInsn, {}, parsed0->pattern)
                            : parsed0->pattern.op == RtlOp::kJumpInsn
                                ? parsed0->pattern
                                : RtlExpr::MakeInsnWrapper(RtlOp::kJumpInsn, {}, parsed0->pattern);
        RtlFlags slot_flags;
        slot_flags.in_struct =
            (parsed0->pattern.op == RtlOp::kJumpInsn && parsed0->pattern.flags.unchanging);
        RtlExpr seq_second = RtlExpr::MakeInsnWrapper(RtlOp::kInsn, slot_flags, parsed1->pattern);
        RtlExpr seq_expr = RtlExpr::MakeSequence({std::move(seq_first), std::move(seq_second)});
        cur_blk.AddPatternOnly(seq_expr.ToGccPrintRtlString());
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
    std::string precomputed_pat;
    if (i + 1 < asm_lines.size() &&
        (absl::StartsWith(line, "mtc1\t") || absl::StartsWith(line, "mtc1 ") ||
         absl::StartsWith(line, "l.s\t") || absl::StartsWith(line, "l.s ")) &&
        (absl::StartsWith(asm_lines[i + 1], "cvt.s.w\t") ||
         absl::StartsWith(asm_lines[i + 1], "cvt.s.w ") ||
         absl::StartsWith(asm_lines[i + 1], "cvt.d.w\t") ||
         absl::StartsWith(asm_lines[i + 1], "cvt.d.w "))) {
      combined_line = absl::StrCat(line, "\n", asm_lines[i + 1]);
      ++i;
    } else if (i + 1 < asm_lines.size() &&
               (absl::StartsWith(line, "trunc.w.s\t") || absl::StartsWith(line, "trunc.w.s ") ||
                absl::StartsWith(line, "trunc.w.d\t") || absl::StartsWith(line, "trunc.w.d "))) {
      if (std::string trunc_w = InvertTwoLineTruncW(line, asm_lines[i + 1]); !trunc_w.empty()) {
        precomputed_pat = std::move(trunc_w);
        ++i;
      }
    } else if (!fp64 && i + 1 < asm_lines.size() &&
               (absl::StartsWith(line, "mtc1\t") || absl::StartsWith(line, "mtc1 ") ||
                absl::StartsWith(line, "mfc1\t") || absl::StartsWith(line, "mfc1 ")) &&
               (absl::StartsWith(asm_lines[i + 1], "mtc1\t") ||
                absl::StartsWith(asm_lines[i + 1], "mtc1 ") ||
                absl::StartsWith(asm_lines[i + 1], "mfc1\t") ||
                absl::StartsWith(asm_lines[i + 1], "mfc1 "))) {
      if (std::string df_xfer = InvertTwoLineDfTransfer(line, asm_lines[i + 1]); !df_xfer.empty()) {
        precomputed_pat = std::move(df_xfer);
        ++i;
      }
    } else if (i + 1 < asm_lines.size() &&
               (absl::StartsWith(line, "li\t") || absl::StartsWith(line, "li ")) &&
               (absl::StartsWith(asm_lines[i + 1], "li\t") ||
                absl::StartsWith(asm_lines[i + 1], "li "))) {
      if (std::string di_li =
              InvertTwoLineDiConstLoad(line, asm_lines[i + 1], /*require_nonneg_decimal=*/true);
          !di_li.empty()) {
        precomputed_pat = std::move(di_li);
        ++i;
      }
    } else if ([&]() -> bool {
                 if (i + 2 >= asm_lines.size()) {
                   return false;
                 }
                 auto parse_zero_clear_dest = [](std::string_view ln) -> int {
                   if (absl::StartsWith(ln, "move\t") || absl::StartsWith(ln, "move ")) {
                     const auto ops = SplitOperands(ln.substr(4));
                     if (ops.size() == 2 && ParseGprNumber(ops[1]) == 0) {
                       return ParseGprNumber(ops[0]);
                     }
                   } else if (absl::StartsWith(ln, "addu\t") || absl::StartsWith(ln, "addu ") ||
                              absl::StartsWith(ln, "or\t") || absl::StartsWith(ln, "or ")) {
                     const size_t sp = ln.find_first_of(" \t");
                     const auto ops = SplitOperands(ln.substr(sp));
                     if (ops.size() == 3 && ParseGprNumber(ops[1]) == 0 &&
                         ParseGprNumber(ops[2]) == 0) {
                       return ParseGprNumber(ops[0]);
                     }
                   }
                   return -1;
                 };
                 const int rd0 = parse_zero_clear_dest(line);
                 const int rd1 = parse_zero_clear_dest(asm_lines[i + 1]);
                 if (rd0 < 2 || (rd0 % 2) != 0 || rd1 != rd0 + 1) {
                   return false;
                 }
                 const std::string_view jln = asm_lines[i + 2];
                 bool unfilled_jump = absl::StartsWith(jln, "j\t") || absl::StartsWith(jln, "j ") ||
                                      absl::StartsWith(jln, "b\t") || absl::StartsWith(jln, "b ");
                 if (!unfilled_jump && (jln == ".set\tnoreorder" || jln == ".set noreorder")) {
                   size_t k = i + 3;
                   if (k < asm_lines.size() &&
                       (asm_lines[k] == ".set\tnomacro" || asm_lines[k] == ".set nomacro")) {
                     ++k;
                   }
                   if (k + 1 < asm_lines.size() &&
                       (absl::StartsWith(asm_lines[k], "j\t") ||
                        absl::StartsWith(asm_lines[k], "j ") ||
                        absl::StartsWith(asm_lines[k], "b\t") ||
                        absl::StartsWith(asm_lines[k], "b ")) &&
                       asm_lines[k + 1] == "nop") {
                     unfilled_jump = true;
                   }
                 }
                 if (!unfilled_jump) {
                   return false;
                 }
                 precomputed_pat =
                     absl::StrFormat("(set %s (const_int 0))", FormatGprRtl(rd0, "DI"));
                 return true;
               }()) {
      ++i;
    } else if ((absl::StartsWith(line, "sll\t") || absl::StartsWith(line, "sll ")) &&
               absl::EndsWith(line, ",26") && i + 13 < asm_lines.size()) {
      const size_t bgez_idx =
          (asm_lines[i + 1] == ".set\tnoreorder" || asm_lines[i + 1] == ".set noreorder") ? (i + 2)
                                                                                          : (i + 1);
      if (bgez_idx < asm_lines.size() && (absl::StartsWith(asm_lines[bgez_idx], "bgez\t") ||
                                          absl::StartsWith(asm_lines[bgez_idx], "bgez "))) {
        auto strip_lbl_prefix = [](std::string_view s) {
          if (absl::StartsWith(s, "$L") || absl::StartsWith(s, ".L")) {
            s.remove_prefix(1);
          }
          return s;
        };
        auto label_ref_outside_span = [&](size_t span_begin, size_t span_end,
                                          std::string_view raw_lbl) {
          const std::string_view core_lbl = strip_lbl_prefix(raw_lbl);
          if (core_lbl.empty()) {
            return false;
          }
          for (const Stage0ResolvedJumpTable& jt : jump_tables) {
            for (const std::string& t : jt.target_labels) {
              if (strip_lbl_prefix(t) == core_lbl) {
                return true;
              }
            }
          }
          for (size_t k = 0; k < asm_lines.size(); ++k) {
            if (k >= span_begin && k <= span_end) {
              continue;
            }
            const std::string_view ln = asm_lines[k];
            if (absl::EndsWith(ln, ":")) {
              continue;
            }
            for (size_t pos = 0; (pos = ln.find(core_lbl, pos)) != std::string_view::npos;
                 pos += core_lbl.size()) {
              if (pos == 0 || (ln[pos - 1] != '$' && ln[pos - 1] != '.')) {
                continue;
              }
              const size_t after = pos + core_lbl.size();
              if (after == ln.size() ||
                  (!absl::ascii_isalnum(static_cast<unsigned char>(ln[after])) &&
                   ln[after] != '_')) {
                return true;
              }
            }
          }
          return false;
        };

        const size_t max_end = std::min<size_t>(asm_lines.size() - 1, i + 22);
        for (size_t end_idx = i + 13; end_idx <= max_end; ++end_idx) {
          if (!absl::EndsWith(asm_lines[end_idx], ":")) {
            continue;
          }
          const std::string cand = absl::StrJoin(asm_lines.subspan(i, end_idx - i + 1), "\n");
          std::string di_shift = InvertMips1DiShift(cand);
          if (di_shift.empty()) {
            continue;
          }
          if (asm_lines[end_idx] == "3:") {
            precomputed_pat = std::move(di_shift);
            i = end_idx;
            break;
          }
          std::vector<std::string_view> span_labels;
          for (size_t k = i; k <= end_idx; ++k) {
            if (absl::EndsWith(asm_lines[k], ":")) {
              span_labels.emplace_back(asm_lines[k].data(), asm_lines[k].size() - 1);
            }
          }
          if (span_labels.size() == 3 && !label_ref_outside_span(i, end_idx, span_labels[0]) &&
              !label_ref_outside_span(i, end_idx, span_labels[1])) {
            precomputed_pat = std::move(di_shift);
            const bool c_ref_outside = label_ref_outside_span(i, end_idx, span_labels[2]);
            i = c_ref_outside ? (end_idx - 1) : end_idx;
            break;
          }
        }
      }
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

    const std::string pat =
        !precomputed_pat.empty() ? std::move(precomputed_pat) : InvertFinalScanInsn(combined_line);
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

  AttachReferencedConstantPoolEntries(blocks, constant_pool, &cfg.sideband);

  absl::flat_hash_set<std::string> jt_tokens;
  for (const Stage0ResolvedJumpTable& jt : jump_tables) {
    if (!jt.table_label_token.empty()) {
      jt_tokens.insert(jt.table_label_token);
    }
  }

  absl::flat_hash_set<std::string> pool_labels;
  for (const Stage0ConstantPoolEntry& cp : cfg.sideband.constant_pool) {
    pool_labels.insert(cp.label_name);
  }
  if (cfg.sideband.epilogue_delay_pattern.has_value()) {
    auto delay_parsed = ParsePatternAndNotes(*cfg.sideband.epilogue_delay_pattern);
    if (delay_parsed.ok()) {
      bool modified_delay = false;
      RewriteDataLabelRefsInAst(&delay_parsed->pattern, label_to_block, jt_tokens,
                                /*allow_direct_label_ref=*/true, &modified_delay);
      if (!pool_labels.empty()) {
        MarkConstantPoolRefsInAst(&delay_parsed->pattern, pool_labels, &modified_delay);
      }
      if (modified_delay) {
        cfg.sideband.epilogue_delay_pattern = delay_parsed->ToGccPrintRtlString();
      }
    }
  }

  for (size_t b = 0; b < blocks.size(); ++b) {
    bool modified_block = false;
    for (size_t insn_idx = 0; insn_idx < blocks[b].instructions.size(); ++insn_idx) {
      auto parsed = ParsePatternAndNotes(blocks[b].instructions[insn_idx]);
      if (!parsed.ok()) {
        continue;
      }
      const RtlExpr* active_ctrl = UnwrapActiveControlPattern(parsed->pattern);
      const bool is_jump_or_branch_insn = IsReturnOrJumpPattern(*active_ctrl);
      const bool allow_direct =
          (insn_idx + 1 < blocks[b].instructions.size()) || !is_jump_or_branch_insn;
      bool modified_insn = false;
      RewriteDataLabelRefsInAst(&parsed->pattern, label_to_block, jt_tokens, allow_direct,
                                &modified_insn);
      if (!pool_labels.empty()) {
        MarkConstantPoolRefsInAst(&parsed->pattern, pool_labels, &modified_insn);
      }
      if (modified_insn) {
        blocks[b].instructions[insn_idx] = parsed->ToGccPrintRtlString();
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
            blocks[b].successors = {it->second, b + 1};
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

  DisambiguateAndAttachJumpTables(jump_tables, label_to_block, &blocks);

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
      RtlExpr seq = RtlExpr::MakeSequence({
          RtlExpr::MakeInsnWrapper(RtlOp::kJumpInsn, last_parsed->pattern.children[0].flags,
                                   std::move(tablejump_par)),
          last_parsed->pattern.children[1],
      });
      blocks[b].instructions.back() = seq.ToGccPrintRtlString();
    } else {
      blocks[b].instructions.back() = tablejump_par.ToGccPrintRtlString();
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

  cfg.rpo_blocks = std::move(blocks);

  if (profile != nullptr) {
    NormalizeMips12AdduZeroMoves(&cfg);
  }
  UpgradeCallReturnValues(&cfg);
  if (profile != nullptr) {
    const bool has_non_empty_frame =
        cfg.sideband.frame_size_bytes > 0 || !cfg.sideband.saved_gpr_regnos.empty() ||
        !cfg.sideband.saved_fpr_regnos.empty() || cfg.sideband.outgoing_args_size > 0 ||
        !cfg.sideband.is_leaf_function;
    if (has_non_empty_frame) {
      for (const auto& blk : cfg.rpo_blocks) {
        for (const std::string& pat : blk.instructions) {
          auto parsed = ParsePatternAndNotes(pat);
          if (!parsed.ok()) {
            continue;
          }
          const RtlExpr* core = UnwrapActiveControlPattern(parsed->pattern);
          if (core->op == RtlOp::kReturn ||
              (core->op == RtlOp::kParallel && !core->children.empty() &&
               core->children[0].op == RtlOp::kReturn)) {
            return {};
          }
        }
      }
    }
    if (IsMipsCpuR4000(*profile)) {
      UpgradeR4000Mulsi3Insns(&cfg);
    }
    if (profile->optimize_level > 0) {
      UpgradeDivModInsnsForOptimize(&cfg);
      NarrowDuplicateConstLoadsInBlocks(&cfg);
      NarrowSubwordMaskedLoadsInBlocks(&cfg);
      NarrowMultiUseSubwordZeroExtendWebs(&cfg);
      MarkSelfLoopSpinWaitLoadsVolatile(&cfg);
    }
    UpgradeCallOutgoingArgBytes(&cfg, *profile);
    ReconstructPostDbrHardRegLivenessAndReturnUse(&cfg, *profile);
    if (FoldFinalScanMultiInsnTemplates(&cfg, *profile)) {
      StripRegDeadAndUnusedNotesOnCfg(&cfg);
      ReconstructPostDbrHardRegLivenessAndReturnUse(&cfg, *profile);
    }
    if (profile->optimize_level > 0) {
      MarkPrologueHoistedUnchangingPtrMems(&cfg, *profile);
      MarkMmioVolatileMemBaseRegs(&cfg, *profile);
      bool gp64 = false;
      bool fp64 = false;
      DetectMipsRegisterWidthFlags(cfg, profile, &gp64, &fp64);
      RestoreNoConflictExtendClobbers(&cfg, gp64, fp64,
                                      ParseCompilationProfileFlags(*profile).RunsSched2());
      SynthesizePostReloadMemRegEquivNotes(&cfg, *profile);
    }
    ReconstructMissingCallFusage(&cfg, *profile);
    if (profile->optimize_level == 0) {
      ReconstructO0FinalScanDeletions(&cfg, *profile);
    }
  }
  CanonicalizeCfgLabels(&cfg);
  return cfg;
}

}  // namespace rom_nom_nom::lifter
