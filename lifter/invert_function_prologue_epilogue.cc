#include "lifter/invert_function_prologue_epilogue.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_split.h"

namespace rom_nom_nom::lifter {
namespace {

// Saveable GPR bitmask per `mips.h:MUST_SAVE_REGISTER(regno)`:
// Callee-saved GPRs $16..$23 ($s0..$s7), $30 ($fp/$s8), and $31 ($ra).
constexpr uint32_t kSaveableGprMask = 0xc0ff0000u;

// Saveable FPR bitmask per `mips.h:CALL_USED_REGISTERS`:
// Callee-saved FPRs $f20..$f31 (even/odd pairs).
constexpr uint32_t kSaveableFprMask = 0xfff00000u;

constexpr int kRegZero = 0;
constexpr int kRegT4 = 12;  // $12: MIPS_TEMP1_REGNUM (`large_reg` for tsize >= 32768)
constexpr int kRegT5 = 13;  // $13: MIPS_TEMP2_REGNUM (`base_reg` for gp_sp_offset >= 32768)
constexpr int kRegSp = 29;  // $29 / $sp
constexpr int kRegFp = 30;  // $30 / $fp / $s8
constexpr int kRegRa = 31;  // $31 / $ra

int PopCount32(uint32_t mask) {
  return __builtin_popcount(mask);
}

int AlignStack8(int bytes) {
  return (bytes + 7) & ~7;
}

bool ParseSignedInt(std::string_view token, int64_t* out) {
  token = absl::StripAsciiWhitespace(token);
  if (token.empty()) {
    return false;
  }
  bool negative = false;
  if (token.front() == '-') {
    negative = true;
    token.remove_prefix(1);
  } else if (token.front() == '+') {
    token.remove_prefix(1);
  }
  if (token.empty()) {
    return false;
  }
  uint64_t mag = 0;
  if (absl::StartsWithIgnoreCase(token, "0x")) {
    std::string_view hex_part = token.substr(2);
    if (hex_part.empty() || !absl::SimpleHexAtoi(hex_part, &mag)) {
      return false;
    }
  } else {
    for (char c : token) {
      if (!absl::ascii_isdigit(c)) {
        return false;
      }
    }
    if (!absl::SimpleAtoi(token, &mag)) {
      return false;
    }
  }
  *out = negative ? -static_cast<int64_t>(mag) : static_cast<int64_t>(mag);
  return true;
}

bool ParseUint32(std::string_view token, uint32_t* out) {
  token = absl::StripAsciiWhitespace(token);
  if (token.empty()) {
    return false;
  }
  if (absl::StartsWithIgnoreCase(token, "0x")) {
    std::string_view hex_part = token.substr(2);
    return !hex_part.empty() && absl::SimpleHexAtoi(hex_part, out);
  }
  for (char c : token) {
    if (!absl::ascii_isdigit(c)) {
      return false;
    }
  }
  return absl::SimpleAtoi(token, out);
}

std::optional<int> ParseGprName(std::string_view token) {
  token = absl::StripAsciiWhitespace(token);
  if (token.size() < 2 || token.front() != '$') {
    return std::nullopt;
  }
  std::string_view name = token.substr(1);
  if (name == "zero") return 0;
  if (name == "at") return 1;
  if (name == "v0") return 2;
  if (name == "v1") return 3;
  if (name == "a0") return 4;
  if (name == "a1") return 5;
  if (name == "a2") return 6;
  if (name == "a3") return 7;
  if (name == "t0") return 8;
  if (name == "t1") return 9;
  if (name == "t2") return 10;
  if (name == "t3") return 11;
  if (name == "t4") return 12;
  if (name == "t5") return 13;
  if (name == "t6") return 14;
  if (name == "t7") return 15;
  if (name == "s0") return 16;
  if (name == "s1") return 17;
  if (name == "s2") return 18;
  if (name == "s3") return 19;
  if (name == "s4") return 20;
  if (name == "s5") return 21;
  if (name == "s6") return 22;
  if (name == "s7") return 23;
  if (name == "t8") return 24;
  if (name == "t9") return 25;
  if (name == "k0") return 26;
  if (name == "k1") return 27;
  if (name == "gp") return 28;
  if (name == "sp") return 29;
  if (name == "fp" || name == "s8") return 30;
  if (name == "ra") return 31;

  for (char c : name) {
    if (!absl::ascii_isdigit(c)) {
      return std::nullopt;
    }
  }
  int regno = -1;
  if (!absl::SimpleAtoi(name, &regno) || regno < 0 || regno > 31) {
    return std::nullopt;
  }
  return regno;
}

std::optional<int> ParseFprName(std::string_view token) {
  token = absl::StripAsciiWhitespace(token);
  if (token.size() < 3 || !absl::StartsWith(token, "$f")) {
    return std::nullopt;
  }
  std::string_view num_str = token.substr(2);
  for (char c : num_str) {
    if (!absl::ascii_isdigit(c)) {
      return std::nullopt;
    }
  }
  int fpreg = -1;
  if (!absl::SimpleAtoi(num_str, &fpreg) || fpreg < 0 || fpreg > 31) {
    return std::nullopt;
  }
  return fpreg;
}

bool ParseMemOperand(std::string_view token, int64_t* offset_out, int* base_reg_out) {
  token = absl::StripAsciiWhitespace(token);
  const size_t lparen = token.find('(');
  const size_t rparen = token.find(')');
  if (lparen == std::string_view::npos || rparen != token.size() - 1 || lparen >= rparen) {
    return false;
  }
  std::string_view off_str = absl::StripAsciiWhitespace(token.substr(0, lparen));
  std::string_view base_str =
      absl::StripAsciiWhitespace(token.substr(lparen + 1, rparen - lparen - 1));
  int64_t offset = 0;
  if (!off_str.empty() && !ParseSignedInt(off_str, &offset)) {
    return false;
  }
  std::optional<int> base_reg = ParseGprName(base_str);
  if (!base_reg.has_value()) {
    return false;
  }
  *offset_out = offset;
  *base_reg_out = *base_reg;
  return true;
}

bool IsValidSymbolName(std::string_view sym) {
  sym = absl::StripAsciiWhitespace(sym);
  if (sym.empty()) {
    return false;
  }
  const char first = sym.front();
  if (!absl::ascii_isalpha(first) && first != '_' && first != '.' && first != '$') {
    return false;
  }
  for (char c : sym) {
    if (!absl::ascii_isalnum(c) && c != '_' && c != '.' && c != '$') {
      return false;
    }
  }
  return true;
}

struct ParsedInsn {
  std::string mnemonic;
  std::vector<std::string> ops;
  bool in_noreorder = false;
};

bool ParseInsnLine(std::string_view line, bool in_noreorder, ParsedInsn* out) {
  line = absl::StripAsciiWhitespace(line);
  if (line.empty()) {
    return false;
  }
  size_t sp = 0;
  while (sp < line.size() && !absl::ascii_isspace(line[sp])) {
    ++sp;
  }
  out->mnemonic = std::string(line.substr(0, sp));
  out->ops.clear();
  out->in_noreorder = in_noreorder;
  std::string_view rest = absl::StripAsciiWhitespace(line.substr(sp));
  if (!rest.empty()) {
    for (std::string_view op : absl::StrSplit(rest, ',')) {
      std::string_view trimmed = absl::StripAsciiWhitespace(op);
      if (trimmed.empty()) {
        return false;
      }
      out->ops.emplace_back(trimmed);
    }
  }
  return true;
}

bool ParseFrameComment(std::string_view comment, int* vars_out, int* ngp_out, int* nfp_out,
                       int* args_out, int* extra_out) {
  // Format emitted by `mips.c:function_prologue`:
  // `# vars= %d, regs= %d/%d, args= %d, extra= %d`
  comment = absl::StripAsciiWhitespace(comment);
  if (!absl::StartsWith(comment, "vars=")) {
    return false;
  }
  int vars = -1;
  int ngp = -1;
  int nfp = -1;
  int args = -1;
  int extra = -1;
  for (std::string_view part : absl::StrSplit(comment, ',')) {
    part = absl::StripAsciiWhitespace(part);
    if (absl::StartsWith(part, "vars=")) {
      int64_t v = 0;
      if (!ParseSignedInt(part.substr(5), &v) || v < 0) return false;
      vars = static_cast<int>(v);
    } else if (absl::StartsWith(part, "regs=")) {
      std::string_view regs_str = absl::StripAsciiWhitespace(part.substr(5));
      const size_t slash = regs_str.find('/');
      if (slash == std::string_view::npos) return false;
      int64_t g = 0;
      int64_t f = 0;
      if (!ParseSignedInt(regs_str.substr(0, slash), &g) ||
          !ParseSignedInt(regs_str.substr(slash + 1), &f) || g < 0 || f < 0) {
        return false;
      }
      ngp = static_cast<int>(g);
      nfp = static_cast<int>(f);
    } else if (absl::StartsWith(part, "args=")) {
      int64_t a = 0;
      if (!ParseSignedInt(part.substr(5), &a) || a < 0) return false;
      args = static_cast<int>(a);
    } else if (absl::StartsWith(part, "extra=")) {
      int64_t e = 0;
      if (!ParseSignedInt(part.substr(6), &e) || e < 0) return false;
      extra = static_cast<int>(e);
    } else {
      return false;
    }
  }
  if (vars < 0 || ngp < 0 || nfp < 0 || args < 0 || extra < 0) {
    return false;
  }
  *vars_out = vars;
  *ngp_out = ngp;
  *nfp_out = nfp;
  *args_out = args;
  *extra_out = extra;
  return true;
}

bool IsMoveSpFp(const ParsedInsn& insn) {
  if (insn.mnemonic == "move" && insn.ops.size() == 2) {
    return ParseGprName(insn.ops[0]) == kRegSp && ParseGprName(insn.ops[1]) == kRegFp;
  }
  if ((insn.mnemonic == "addu" || insn.mnemonic == "or") && insn.ops.size() == 3) {
    return ParseGprName(insn.ops[0]) == kRegSp && ParseGprName(insn.ops[1]) == kRegFp &&
           ParseGprName(insn.ops[2]) == kRegZero;
  }
  return false;
}

bool IsMoveFpSp(const ParsedInsn& insn) {
  if (insn.mnemonic == "move" && insn.ops.size() == 2) {
    return ParseGprName(insn.ops[0]) == kRegFp && ParseGprName(insn.ops[1]) == kRegSp;
  }
  if ((insn.mnemonic == "addu" || insn.mnemonic == "or") && insn.ops.size() == 3) {
    return ParseGprName(insn.ops[0]) == kRegFp && ParseGprName(insn.ops[1]) == kRegSp &&
           ParseGprName(insn.ops[2]) == kRegZero;
  }
  return false;
}

bool IsReturnJump(const ParsedInsn& insn) {
  return (insn.mnemonic == "j" || insn.mnemonic == "jr") && insn.ops.size() == 1 &&
         ParseGprName(insn.ops[0]) == kRegRa;
}

bool IsLoadImmediate(const ParsedInsn& insn, int expected_reg, int64_t* imm_out) {
  if (insn.mnemonic == "li" && insn.ops.size() == 2 && ParseGprName(insn.ops[0]) == expected_reg) {
    return ParseSignedInt(insn.ops[1], imm_out);
  }
  if ((insn.mnemonic == "addiu" || insn.mnemonic == "ori") && insn.ops.size() == 3 &&
      ParseGprName(insn.ops[0]) == expected_reg && ParseGprName(insn.ops[1]) == kRegZero) {
    return ParseSignedInt(insn.ops[2], imm_out);
  }
  return false;
}

bool IsAdduRegRegSp(const ParsedInsn& insn, int dst_reg, int src_reg) {
  if (insn.mnemonic != "addu" || insn.ops.size() != 3) {
    return false;
  }
  const std::optional<int> d = ParseGprName(insn.ops[0]);
  const std::optional<int> s1 = ParseGprName(insn.ops[1]);
  const std::optional<int> s2 = ParseGprName(insn.ops[2]);
  if (d != dst_reg) {
    return false;
  }
  return (s1 == src_reg && s2 == kRegSp) || (s1 == kRegSp && s2 == src_reg);
}

bool IsEligibleEpilogueDelayInsn(const ParsedInsn& insn) {
  // Per `mips.h:ELIGIBLE_FOR_EPILOGUE_DELAY`, an epilogue delay slot
  // instruction cannot be a jump/branch and cannot reference `$sp` (`$29`) or
  // `$31` (`$ra`).
  if (insn.mnemonic.empty() || IsReturnJump(insn) || insn.mnemonic == "j" ||
      insn.mnemonic == "jal" || insn.mnemonic == "jr" || insn.mnemonic == "jalr" ||
      absl::StartsWith(insn.mnemonic, "b")) {
    return false;
  }
  for (const std::string& op : insn.ops) {
    std::optional<int> reg = ParseGprName(op);
    if (reg == kRegSp || reg == kRegRa) {
      return false;
    }
    int64_t off = 0;
    int base = -1;
    if (ParseMemOperand(op, &off, &base) && (base == kRegSp || base == kRegRa)) {
      return false;
    }
  }
  return true;
}

struct SaveRestoreGroup {
  uint32_t gpr_mask = 0;
  uint32_t fpr_mask = 0;
  std::vector<int> gpr_regnos;
  std::vector<int64_t> gpr_effective_offsets;
  bool used_large_base_t5 = false;
};

// Parses an optional `$13` large-offset setup (`addu $13,$12,$sp` or
// `li $13,<off>` + `addu $13,$13,$sp`) followed by zero or more GPR `sw`/`lw`
// and FPR `s.d`/`s.s`/`l.d`/`l.s` instructions at `*idx`.
bool ParseSaveRestoreInsns(const std::vector<ParsedInsn>& insns, bool is_restore,
                           std::optional<int64_t> t4_tsize, size_t* idx, SaveRestoreGroup* out) {
  *out = SaveRestoreGroup{};
  int expected_base_reg = kRegSp;
  int64_t base_offset = 0;

  if (*idx < insns.size()) {
    int64_t t5_imm = 0;
    if (t4_tsize.has_value() && IsAdduRegRegSp(insns[*idx], kRegT5, kRegT4)) {
      expected_base_reg = kRegT5;
      base_offset = *t4_tsize;
      out->used_large_base_t5 = true;
      ++(*idx);
    } else if (*idx + 1 < insns.size() && IsLoadImmediate(insns[*idx], kRegT5, &t5_imm) &&
               IsAdduRegRegSp(insns[*idx + 1], kRegT5, kRegT5)) {
      if (t5_imm < 32768) {
        return false;
      }
      expected_base_reg = kRegT5;
      base_offset = t5_imm;
      out->used_large_base_t5 = true;
      *idx += 2;
    }
  }

  const std::string_view gpr_mnemonic = is_restore ? "lw" : "sw";
  while (*idx < insns.size() && insns[*idx].mnemonic == gpr_mnemonic) {
    const ParsedInsn& insn = insns[*idx];
    if (insn.ops.size() != 2) {
      return false;
    }
    const std::optional<int> reg = ParseGprName(insn.ops[0]);
    int64_t raw_off = 0;
    int base_reg = -1;
    if (!reg.has_value() || !ParseMemOperand(insn.ops[1], &raw_off, &base_reg)) {
      return false;
    }
    if (base_reg != expected_base_reg || raw_off < -32768 || raw_off > 32767 ||
        (raw_off % 4) != 0) {
      return false;
    }
    const uint32_t bit = 1u << *reg;
    if ((bit & kSaveableGprMask) == 0 || (out->gpr_mask & bit) != 0) {
      return false;
    }
    if (!out->gpr_regnos.empty() && *reg >= out->gpr_regnos.back()) {
      return false;
    }
    const int64_t eff_off = base_offset + raw_off;
    if (!out->gpr_effective_offsets.empty() && eff_off != out->gpr_effective_offsets.back() - 4) {
      return false;
    }
    out->gpr_mask |= bit;
    out->gpr_regnos.push_back(*reg);
    out->gpr_effective_offsets.push_back(eff_off);
    ++(*idx);
  }

  if (out->used_large_base_t5 && out->gpr_regnos.empty()) {
    return false;
  }

  // Optional FPR save/restore instructions (`l.d`/`l.s` or `s.d`/`s.s`).
  const std::string_view fpr_double = is_restore ? "l.d" : "s.d";
  const std::string_view fpr_single = is_restore ? "l.s" : "s.s";
  int last_fpreg = 32;
  while (*idx < insns.size() &&
         (insns[*idx].mnemonic == fpr_double || insns[*idx].mnemonic == fpr_single)) {
    const ParsedInsn& insn = insns[*idx];
    if (insn.ops.size() != 2) {
      return false;
    }
    const std::optional<int> fpreg = ParseFprName(insn.ops[0]);
    int64_t raw_off = 0;
    int base_reg = -1;
    if (!fpreg.has_value() || !ParseMemOperand(insn.ops[1], &raw_off, &base_reg) ||
        *fpreg >= last_fpreg) {
      return false;
    }
    const bool is_double = (insn.mnemonic == fpr_double);
    if (is_double && (*fpreg % 2 != 0 || *fpreg + 1 >= 32)) {
      return false;
    }
    const uint32_t fbits = is_double ? (3u << *fpreg) : (1u << *fpreg);
    if ((fbits & kSaveableFprMask) != fbits || (out->fpr_mask & fbits) != 0) {
      return false;
    }
    out->fpr_mask |= fbits;
    last_fpreg = *fpreg;
    ++(*idx);
  }

  return true;
}

}  // namespace

PrologueEpilogueFrameSpec InvertFunctionPrologueEpilogue(std::string_view asm_text_x) {
  constexpr PrologueEpilogueFrameSpec kInvalid{.var_size = -1, .saved_gpr_mask = 0};

  bool has_ent = false;
  bool has_end = false;
  std::string ent_name;
  std::string end_name;

  bool has_frame_dir = false;
  int dir_frame_tsize = 0;
  bool has_frame_comment = false;
  int comment_vars = 0;
  int comment_ngp = 0;
  int comment_nfp = 0;
  int comment_args = 0;
  int comment_extra = 0;

  bool has_mask_dir = false;
  uint32_t dir_mask = 0;
  int64_t dir_gp_save_offset = 0;

  bool has_fmask_dir = false;
  uint32_t dir_fmask = 0;
  int64_t dir_fp_save_offset = 0;

  bool in_noreorder = false;
  std::vector<ParsedInsn> insns;

  for (std::string_view raw_line : absl::StrSplit(asm_text_x, '\n')) {
    std::string_view comment_text;
    const size_t hash_pos = raw_line.find('#');
    std::string_view code_part = raw_line;
    if (hash_pos != std::string_view::npos) {
      comment_text = absl::StripAsciiWhitespace(raw_line.substr(hash_pos + 1));
      code_part = raw_line.substr(0, hash_pos);
    }

    std::string_view line = absl::StripAsciiWhitespace(code_part);
    if (line.empty()) {
      continue;
    }

    // Strip any leading `<label>:` definition on this line.
    while (true) {
      const size_t colon_pos = line.find(':');
      if (colon_pos == std::string_view::npos) {
        break;
      }
      std::string_view candidate_lbl = line.substr(0, colon_pos);
      if (candidate_lbl.find_first_of(" \t,()") != std::string_view::npos) {
        break;
      }
      if (!IsValidSymbolName(candidate_lbl)) {
        return kInvalid;
      }
      line = absl::StripAsciiWhitespace(line.substr(colon_pos + 1));
    }
    if (line.empty()) {
      continue;
    }

    // Parse assembler directives.
    if (line.front() == '.') {
      size_t sp = 0;
      while (sp < line.size() && !absl::ascii_isspace(line[sp])) {
        ++sp;
      }
      const std::string_view dir = line.substr(0, sp);
      const std::string_view args = absl::StripAsciiWhitespace(line.substr(sp));

      if (dir == ".file") {
        continue;
      }
      if (dir == ".ent") {
        if (has_ent || !IsValidSymbolName(args)) {
          return kInvalid;
        }
        has_ent = true;
        ent_name = std::string(args);
        continue;
      }
      if (dir == ".end") {
        if (has_end || !IsValidSymbolName(args)) {
          return kInvalid;
        }
        has_end = true;
        end_name = std::string(args);
        continue;
      }
      if (dir == ".set") {
        if (args == "noreorder") {
          in_noreorder = true;
        } else if (args == "reorder") {
          in_noreorder = false;
        } else if (args != "macro" && args != "nomacro" && args != "at" && args != "noat") {
          return kInvalid;
        }
        continue;
      }
      if (dir == ".frame") {
        if (has_frame_dir) {
          return kInvalid;
        }
        const std::vector<std::string_view> parts = absl::StrSplit(args, ',');
        if (parts.size() != 3) {
          return kInvalid;
        }
        const std::optional<int> fp_reg = ParseGprName(parts[0]);
        int64_t tsize_val = -1;
        const std::optional<int> ret_reg = ParseGprName(parts[2]);
        if ((fp_reg != kRegSp && fp_reg != kRegFp) || !ParseSignedInt(parts[1], &tsize_val) ||
            tsize_val < 0 || (tsize_val % 8) != 0 || ret_reg != kRegRa) {
          return kInvalid;
        }
        has_frame_dir = true;
        dir_frame_tsize = static_cast<int>(tsize_val);
        if (!comment_text.empty()) {
          if (!ParseFrameComment(comment_text, &comment_vars, &comment_ngp, &comment_nfp,
                                 &comment_args, &comment_extra)) {
            return kInvalid;
          }
          if ((comment_vars % 8) != 0 || (comment_args % 8) != 0 || (comment_extra % 8) != 0) {
            return kInvalid;
          }
          has_frame_comment = true;
        }
        continue;
      }
      if (dir == ".mask") {
        if (has_mask_dir) {
          return kInvalid;
        }
        const std::vector<std::string_view> parts = absl::StrSplit(args, ',');
        if (parts.size() != 2 || !ParseUint32(parts[0], &dir_mask) ||
            !ParseSignedInt(parts[1], &dir_gp_save_offset)) {
          return kInvalid;
        }
        if ((dir_mask & ~kSaveableGprMask) != 0) {
          return kInvalid;
        }
        if (dir_mask == 0 && dir_gp_save_offset != 0) {
          return kInvalid;
        }
        has_mask_dir = true;
        continue;
      }
      if (dir == ".fmask") {
        if (has_fmask_dir) {
          return kInvalid;
        }
        const std::vector<std::string_view> parts = absl::StrSplit(args, ',');
        if (parts.size() != 2 || !ParseUint32(parts[0], &dir_fmask) ||
            !ParseSignedInt(parts[1], &dir_fp_save_offset)) {
          return kInvalid;
        }
        if ((dir_fmask & ~kSaveableFprMask) != 0) {
          return kInvalid;
        }
        if (dir_fmask == 0 && dir_fp_save_offset != 0) {
          return kInvalid;
        }
        has_fmask_dir = true;
        continue;
      }

      // Unknown or unsupported directive.
      return kInvalid;
    }

    ParsedInsn parsed;
    if (!ParseInsnLine(line, in_noreorder, &parsed)) {
      return kInvalid;
    }
    insns.push_back(std::move(parsed));
  }

  if (in_noreorder) {
    return kInvalid;
  }
  if (has_ent && has_end && ent_name != end_name) {
    return kInvalid;
  }

  // Case 1: `noepilogue == TRUE` in `mips.c:function_epilogue`.
  // Occurs when `total_size == 0 && epilogue_delay == 0` and the last RTL insn
  // is a `BARRIER`; only directives are present and `insns` is empty.
  if (insns.empty()) {
    if (!has_frame_dir || !has_mask_dir || dir_frame_tsize != 0 || dir_mask != 0 ||
        (has_fmask_dir && dir_fmask != 0)) {
      return kInvalid;
    }
    if (has_frame_comment && (comment_vars != 0 || comment_ngp != 0 || comment_nfp != 0 ||
                              comment_args != 0 || comment_extra != 0)) {
      return kInvalid;
    }
    return PrologueEpilogueFrameSpec{.var_size = 0, .saved_gpr_mask = 0};
  }

  // Case 2: Non-empty instruction sequence.
  // Check for an optional prologue instruction prefix (`li $12`, `subu $sp`,
  // `sw` saves, `move $fp,$sp`) before the epilogue sequence.
  size_t idx = 0;
  bool has_prologue_insns = false;
  int64_t prologue_tsize = 0;
  SaveRestoreGroup prologue_saves;

  {
    size_t p_idx = 0;
    std::optional<int64_t> p_t4_tsize;
    int64_t imm = 0;
    if (p_idx < insns.size() && IsLoadImmediate(insns[p_idx], kRegT4, &imm)) {
      if (imm < 32768 || (imm % 8) != 0) {
        return kInvalid;
      }
      p_t4_tsize = imm;
      ++p_idx;
    }
    if (p_idx < insns.size() &&
        (insns[p_idx].mnemonic == "subu" || insns[p_idx].mnemonic == "addiu") &&
        insns[p_idx].ops.size() == 3 && ParseGprName(insns[p_idx].ops[0]) == kRegSp &&
        ParseGprName(insns[p_idx].ops[1]) == kRegSp) {
      int64_t sp_delta = 0;
      if (p_t4_tsize.has_value()) {
        if (insns[p_idx].mnemonic != "subu" || ParseGprName(insns[p_idx].ops[2]) != kRegT4) {
          return kInvalid;
        }
        prologue_tsize = *p_t4_tsize;
      } else {
        if (!ParseSignedInt(insns[p_idx].ops[2], &sp_delta)) {
          return kInvalid;
        }
        if (insns[p_idx].mnemonic == "addiu") {
          sp_delta = -sp_delta;
        }
        if (sp_delta <= 0 || sp_delta >= 32768 || (sp_delta % 8) != 0) {
          return kInvalid;
        }
        prologue_tsize = sp_delta;
      }
      ++p_idx;
      if (!ParseSaveRestoreInsns(insns, /*is_restore=*/false, p_t4_tsize, &p_idx,
                                 &prologue_saves)) {
        return kInvalid;
      }
      if (p_idx < insns.size() && IsMoveFpSp(insns[p_idx])) {
        ++p_idx;
      }
      has_prologue_insns = true;
      idx = p_idx;
    }
  }

  // Parse epilogue instruction sequence starting at `idx`.
  if (idx >= insns.size()) {
    return kInvalid;
  }

  std::optional<int64_t> epilogue_t4_tsize;
  {
    int64_t imm = 0;
    if (idx < insns.size() && IsLoadImmediate(insns[idx], kRegT4, &imm)) {
      if (imm < 32768 || (imm % 8) != 0) {
        return kInvalid;
      }
      epilogue_t4_tsize = imm;
      ++idx;
    }
  }

  if (idx < insns.size() && IsMoveSpFp(insns[idx])) {
    ++idx;
  }

  SaveRestoreGroup epilogue_restores;
  if (!ParseSaveRestoreInsns(insns, /*is_restore=*/true, epilogue_t4_tsize, &idx,
                             &epilogue_restores)) {
    return kInvalid;
  }

  int64_t insn_tsize = -1;
  if (idx < insns.size() && insns[idx].in_noreorder) {
    // `.set noreorder` epilogue with `epilogue_delay != 0` in
    // `mips.c:function_epilogue` (lines 5180-5239):
    // Subcase A (`tsize > 0`, `load_only_r31`):
    //   lw $31,<off>($sp/$13)
    //   <1 delay slot insn>
    //   j $31
    //   addu $sp,$sp,<tsize/$12>
    // Subcase B (`tsize == 0`, `mask == 0`, `fmask == 0`):
    //   j $31
    //   <1 delay slot insn>
    if (epilogue_restores.gpr_mask == (1u << kRegRa) && epilogue_restores.fpr_mask == 0) {
      if (idx + 3 != insns.size()) {
        return kInvalid;
      }
      if (!IsEligibleEpilogueDelayInsn(insns[idx]) || !IsReturnJump(insns[idx + 1])) {
        return kInvalid;
      }
      const ParsedInsn& sp_adj = insns[idx + 2];
      if (sp_adj.mnemonic != "addu" || sp_adj.ops.size() != 3 ||
          ParseGprName(sp_adj.ops[0]) != kRegSp || ParseGprName(sp_adj.ops[1]) != kRegSp) {
        return kInvalid;
      }
      if (epilogue_t4_tsize.has_value()) {
        if (ParseGprName(sp_adj.ops[2]) != kRegT4) {
          return kInvalid;
        }
        insn_tsize = *epilogue_t4_tsize;
      } else {
        int64_t delta = 0;
        if (!ParseSignedInt(sp_adj.ops[2], &delta) || delta <= 0 || delta >= 32768 ||
            (delta % 8) != 0) {
          return kInvalid;
        }
        insn_tsize = delta;
      }
      idx += 3;
    } else if (epilogue_restores.gpr_mask == 0 && epilogue_restores.fpr_mask == 0 &&
               !epilogue_t4_tsize.has_value()) {
      if (idx + 2 != insns.size()) {
        return kInvalid;
      }
      if (!IsReturnJump(insns[idx]) || !IsEligibleEpilogueDelayInsn(insns[idx + 1])) {
        return kInvalid;
      }
      insn_tsize = 0;
      idx += 2;
    } else {
      return kInvalid;
    }
  } else {
    // Standard `.set reorder` epilogue (`noreorder == 0`):
    // Optional `addu $sp,$sp,<tsize/$12>` followed by `j $31`.
    if (idx < insns.size() && (insns[idx].mnemonic == "addu" || insns[idx].mnemonic == "addiu") &&
        insns[idx].ops.size() == 3 && ParseGprName(insns[idx].ops[0]) == kRegSp &&
        ParseGprName(insns[idx].ops[1]) == kRegSp) {
      if (epilogue_t4_tsize.has_value()) {
        if (insns[idx].mnemonic != "addu" || ParseGprName(insns[idx].ops[2]) != kRegT4) {
          return kInvalid;
        }
        insn_tsize = *epilogue_t4_tsize;
      } else {
        int64_t delta = 0;
        if (!ParseSignedInt(insns[idx].ops[2], &delta) || delta <= 0 || delta >= 32768 ||
            (delta % 8) != 0) {
          return kInvalid;
        }
        insn_tsize = delta;
      }
      ++idx;
    } else {
      if (epilogue_t4_tsize.has_value()) {
        return kInvalid;
      }
      insn_tsize = 0;
    }

    if (idx + 1 != insns.size() || !IsReturnJump(insns[idx])) {
      return kInvalid;
    }
    ++idx;
  }

  if (idx != insns.size() || insn_tsize < 0 || (insn_tsize % 8) != 0) {
    return kInvalid;
  }

  // If prologue instructions were also present, verify exact consistency with
  // the epilogue instructions.
  if (has_prologue_insns) {
    if (prologue_tsize != insn_tsize || prologue_saves.gpr_mask != epilogue_restores.gpr_mask ||
        prologue_saves.fpr_mask != epilogue_restores.fpr_mask ||
        prologue_saves.gpr_effective_offsets != epilogue_restores.gpr_effective_offsets) {
      return kInvalid;
    }
  }

  const uint32_t saved_gpr_mask = epilogue_restores.gpr_mask;
  const uint32_t saved_fpr_mask = has_fmask_dir ? dir_fmask : epilogue_restores.fpr_mask;

  // Cross-check directives against instructions when directives are present.
  if (has_frame_dir && dir_frame_tsize != insn_tsize) {
    return kInvalid;
  }
  if (has_mask_dir && dir_mask != saved_gpr_mask) {
    return kInvalid;
  }
  if (has_fmask_dir && epilogue_restores.fpr_mask != 0 && dir_fmask != epilogue_restores.fpr_mask) {
    return kInvalid;
  }

  const int num_gp = PopCount32(saved_gpr_mask);
  const int gp_reg_size = 4 * num_gp;
  const int gp_reg_rounded = AlignStack8(gp_reg_size);

  const int num_fp_regs = PopCount32(saved_fpr_mask);
  const int fp_reg_size = 4 * num_fp_regs;
  const int fp_reg_rounded = AlignStack8(fp_reg_size);

  if (has_frame_comment) {
    if (comment_ngp != num_gp || comment_nfp != (num_fp_regs / 2)) {
      return kInvalid;
    }
  }

  // Verify stack frame layout equations from `mips.c:compute_frame_size`:
  // `total_size = var_size + args_size + extra_size + gp_reg_rounded + fp_reg_rounded`
  // When `saved_gpr_mask != 0`:
  //   `end_offset = total_size - fp_reg_rounded - gp_reg_rounded`
  //   `gp_sp_offset = end_offset + gp_reg_size - 4`
  //   `gp_save_offset = gp_sp_offset - total_size`
  const int64_t non_reg_size = insn_tsize - fp_reg_rounded - gp_reg_rounded;
  if (non_reg_size < 0 || (non_reg_size % 8) != 0) {
    return kInvalid;
  }

  if (saved_gpr_mask != 0) {
    const int64_t expected_end_offset = non_reg_size;
    const int64_t expected_gp_sp_offset = expected_end_offset + gp_reg_size - 4;
    const int64_t expected_gp_save_offset = expected_gp_sp_offset - insn_tsize;

    if (epilogue_restores.gpr_effective_offsets.size() != static_cast<size_t>(num_gp) ||
        epilogue_restores.gpr_effective_offsets.front() != expected_gp_sp_offset ||
        epilogue_restores.gpr_effective_offsets.back() != expected_end_offset) {
      return kInvalid;
    }
    if ((expected_gp_sp_offset >= 32768) != epilogue_restores.used_large_base_t5) {
      return kInvalid;
    }
    if (has_mask_dir && dir_gp_save_offset != expected_gp_save_offset) {
      return kInvalid;
    }
  } else {
    if (has_mask_dir && dir_gp_save_offset != 0) {
      return kInvalid;
    }
  }

  if (saved_fpr_mask != 0 && has_fmask_dir) {
    const int64_t expected_fp_sp_offset = insn_tsize - fp_reg_rounded + fp_reg_size - 8;
    const int64_t expected_fp_save_offset = expected_fp_sp_offset - insn_tsize;
    if (dir_fp_save_offset != expected_fp_save_offset) {
      return kInvalid;
    }
  }

  int var_size = static_cast<int>(non_reg_size);
  if (has_frame_comment) {
    if (comment_vars + comment_args + comment_extra != non_reg_size) {
      return kInvalid;
    }
    var_size = comment_vars;
  }

  if (var_size < 0 || (var_size % 8) != 0) {
    return kInvalid;
  }

  return PrologueEpilogueFrameSpec{
      .var_size = var_size,
      .saved_gpr_mask = saved_gpr_mask,
  };
}

}  // namespace rom_nom_nom::lifter
