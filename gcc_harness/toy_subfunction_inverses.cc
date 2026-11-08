#include "gcc_harness/toy_subfunction_inverses.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_split.h"
#include "absl/types/span.h"
#include "core/mips.h"
#include "core/rtl_cfg.h"
#include "core/rtl_sexpr_utils.h"

namespace rom_nom_nom::gcc_harness {
namespace {

bool IsBranchOrJumpWord(uint32_t w) {
  const uint32_t op = (w >> 26) & 0x3fu;
  if (op == 0x00u) {
    const uint32_t funct = w & 0x3fu;
    return funct == 0x08u || funct == 0x09u;  // jr, jalr
  }
  if (op == 0x01u) {
    const uint32_t rt = (w >> 16) & 0x1fu;
    return rt == 0x00u || rt == 0x01u || rt == 0x02u || rt == 0x03u || rt == 0x10u || rt == 0x11u ||
           rt == 0x12u || rt == 0x13u;
  }
  return op >= 0x02u && op <= 0x07u;
}

std::string InvertWordToRtlSexpr(uint32_t w, bool is_return_value = false) {
  const uint32_t op = (w >> 26) & 0x3fu;
  const uint32_t rs = (w >> 21) & 0x1fu;
  const uint32_t rt = (w >> 16) & 0x1fu;
  const uint32_t rd = (w >> 11) & 0x1fu;
  const uint32_t sa = (w >> 6) & 0x1fu;
  const uint32_t funct = w & 0x3fu;
  const int16_t simm = static_cast<int16_t>(w & 0xffffu);
  const uint16_t uimm = static_cast<uint16_t>(w & 0xffffu);

  if (w == 0x03e00008u) {
    return "(return)";
  }
  if (op == 0x00u) {
    const std::string dst = FormatHardRegRtl(rd, is_return_value && rd == 2);
    switch (funct) {
      case 0x00u:
        return absl::StrFormat("(set %s (ashift:SI %s (const_int %u)))", dst, FormatHardRegRtl(rt),
                               sa);
      case 0x02u:
        return absl::StrFormat("(set %s (lshiftrt:SI %s (const_int %u)))", dst,
                               FormatHardRegRtl(rt), sa);
      case 0x03u:
        return absl::StrFormat("(set %s (ashiftrt:SI %s (const_int %u)))", dst,
                               FormatHardRegRtl(rt), sa);
      case 0x21u:
        if (rs == 0) {
          return absl::StrFormat("(set %s %s)", dst, FormatHardRegRtl(rt));
        }
        if (rt == 0) {
          return absl::StrFormat("(set %s %s)", dst, FormatHardRegRtl(rs));
        }
        return absl::StrFormat("(set %s (plus:SI %s %s))", dst, FormatHardRegRtl(rs),
                               FormatHardRegRtl(rt));
      case 0x23u:
        return absl::StrFormat("(set %s (minus:SI %s %s))", dst, FormatHardRegRtl(rs),
                               FormatHardRegRtl(rt));
      case 0x24u:
        return absl::StrFormat("(set %s (and:SI %s %s))", dst, FormatHardRegRtl(rs),
                               FormatHardRegRtl(rt));
      case 0x25u:
        return absl::StrFormat("(set %s (ior:SI %s %s))", dst, FormatHardRegRtl(rs),
                               FormatHardRegRtl(rt));
      case 0x26u:
        return absl::StrFormat("(set %s (xor:SI %s %s))", dst, FormatHardRegRtl(rs),
                               FormatHardRegRtl(rt));
      case 0x2au:
        return absl::StrFormat("(set %s (lt:SI %s %s))", dst, FormatHardRegRtl(rs),
                               FormatHardRegRtl(rt));
      case 0x2bu:
        return absl::StrFormat("(set %s (ltu:SI %s %s))", dst, FormatHardRegRtl(rs),
                               FormatHardRegRtl(rt));
      default:
        break;
    }
  } else if (op == 0x09u) {  // addiu
    const std::string dst = FormatHardRegRtl(rt, is_return_value && rt == 2);
    if (rs == 0) {
      return absl::StrFormat("(set %s (const_int %d))", dst, simm);
    }
    return absl::StrFormat("(set %s (plus:SI %s (const_int %d)))", dst, FormatHardRegRtl(rs), simm);
  } else if (op == 0x0cu) {  // andi
    const std::string dst = FormatHardRegRtl(rt, is_return_value && rt == 2);
    return absl::StrFormat("(set %s (and:SI %s (const_int %u)))", dst, FormatHardRegRtl(rs), uimm);
  } else if (op == 0x0du) {  // ori
    const std::string dst = FormatHardRegRtl(rt, is_return_value && rt == 2);
    if (rs == 0) {
      return absl::StrFormat("(set %s (const_int %u))", dst, uimm);
    }
    return absl::StrFormat("(set %s (ior:SI %s (const_int %u)))", dst, FormatHardRegRtl(rs), uimm);
  } else if (op == 0x0eu) {  // xori
    const std::string dst = FormatHardRegRtl(rt, is_return_value && rt == 2);
    return absl::StrFormat("(set %s (xor:SI %s (const_int %u)))", dst, FormatHardRegRtl(rs), uimm);
  } else if (op == 0x0fu) {  // lui
    const std::string dst = FormatHardRegRtl(rt, is_return_value && rt == 2);
    const int32_t hi_val = static_cast<int32_t>(static_cast<uint32_t>(uimm) << 16);
    return absl::StrFormat("(set %s (const_int %d))", dst, hi_val);
  } else if (op == 0x23u) {  // lw
    const std::string dst = FormatHardRegRtl(rt, is_return_value && rt == 2);
    if (simm == 0) {
      return absl::StrFormat("(set %s (mem:SI %s))", dst, FormatHardRegRtl(rs));
    }
    return absl::StrFormat("(set %s (mem:SI (plus:SI %s (const_int %d))))", dst,
                           FormatHardRegRtl(rs), simm);
  } else if (op == 0x2bu) {  // sw
    if (simm == 0) {
      return absl::StrFormat("(set (mem:SI %s) %s)", FormatHardRegRtl(rs), FormatHardRegRtl(rt));
    }
    return absl::StrFormat("(set (mem:SI (plus:SI %s (const_int %d))) %s)", FormatHardRegRtl(rs),
                           simm, FormatHardRegRtl(rt));
  }
  return "(const_int 0)";
}

}  // namespace

// -----------------------------------------------------------------------------
// Stage 0: GAS (`tc-mips.c`) & Final (`final.c`, `mips.c`)
// -----------------------------------------------------------------------------

std::string ToySubfunctionInverses::InvertGasMipsIp(uint32_t word_x) const {
  if (word_x == 0x00000000u) {
    return "nop";
  }
  if (word_x == 0x03a0e808u) {
    return "subu $29,$29,8";
  }
  if (word_x == 0x03bd0008u) {
    return "addu $29,$29,8";
  }
  const uint32_t op = (word_x >> 26) & 0x3fu;
  const uint32_t rs = (word_x >> 21) & 0x1fu;
  const uint32_t rt = (word_x >> 16) & 0x1fu;
  const uint32_t rd = (word_x >> 11) & 0x1fu;
  const uint32_t sa = (word_x >> 6) & 0x1fu;
  const uint32_t funct = word_x & 0x3fu;
  const int16_t simm = static_cast<int16_t>(word_x & 0xffffu);
  const uint16_t uimm = static_cast<uint16_t>(word_x & 0xffffu);

  if (op == 0x00u) {
    switch (funct) {
      case 0x00u:
        return absl::StrFormat("sll $%u,$%u,%u", rd, rt, sa);
      case 0x02u:
        return absl::StrFormat("srl $%u,$%u,%u", rd, rt, sa);
      case 0x03u:
        return absl::StrFormat("sra $%u,$%u,%u", rd, rt, sa);
      case 0x04u:
        return absl::StrFormat("sllv $%u,$%u,$%u", rd, rt, rs);
      case 0x06u:
        return absl::StrFormat("srlv $%u,$%u,$%u", rd, rt, rs);
      case 0x07u:
        return absl::StrFormat("srav $%u,$%u,$%u", rd, rt, rs);
      case 0x08u:
        return absl::StrFormat("jr $%u", rs);
      case 0x09u:
        return rd == 31 ? absl::StrFormat("jalr $%u", rs) : absl::StrFormat("jalr $%u,$%u", rd, rs);
      case 0x10u:
        return absl::StrFormat("mfhi $%u", rd);
      case 0x11u:
        return absl::StrFormat("mthi $%u", rs);
      case 0x12u:
        return absl::StrFormat("mflo $%u", rd);
      case 0x13u:
        return absl::StrFormat("mtlo $%u", rs);
      case 0x18u:
        return absl::StrFormat("mult $%u,$%u", rs, rt);
      case 0x19u:
        return absl::StrFormat("multu $%u,$%u", rs, rt);
      case 0x20u:
        return absl::StrFormat("add $%u,$%u,$%u", rd, rs, rt);
      case 0x21u:
        return absl::StrFormat("addu $%u,$%u,$%u", rd, rs, rt);
      case 0x22u:
        return absl::StrFormat("sub $%u,$%u,$%u", rd, rs, rt);
      case 0x23u:
        return absl::StrFormat("subu $%u,$%u,$%u", rd, rs, rt);
      case 0x24u:
        return absl::StrFormat("and $%u,$%u,$%u", rd, rs, rt);
      case 0x25u:
        return absl::StrFormat("or $%u,$%u,$%u", rd, rs, rt);
      case 0x26u:
        return absl::StrFormat("xor $%u,$%u,$%u", rd, rs, rt);
      case 0x27u:
        return absl::StrFormat("nor $%u,$%u,$%u", rd, rs, rt);
      case 0x2au:
        return absl::StrFormat("slt $%u,$%u,$%u", rd, rs, rt);
      case 0x2bu:
        return absl::StrFormat("sltu $%u,$%u,$%u", rd, rs, rt);
      default:
        break;
    }
  }

  switch (op) {
    case 0x02u:
      return absl::StrFormat("j 0x%x", (word_x & 0x03ffffffu) << 2);
    case 0x03u:
      return absl::StrFormat("jal 0x%x", (word_x & 0x03ffffffu) << 2);
    case 0x08u:
      return absl::StrFormat("addi $%u,$%u,%d", rt, rs, simm);
    case 0x09u:
      return absl::StrFormat("addiu $%u,$%u,%d", rt, rs, simm);
    case 0x0au:
      return absl::StrFormat("slti $%u,$%u,%d", rt, rs, simm);
    case 0x0bu:
      return absl::StrFormat("sltiu $%u,$%u,%d", rt, rs, simm);
    case 0x0cu:
      return absl::StrFormat("andi $%u,$%u,0x%x", rt, rs, uimm);
    case 0x0du:
      return absl::StrFormat("ori $%u,$%u,0x%x", rt, rs, uimm);
    case 0x0eu:
      return absl::StrFormat("xori $%u,$%u,0x%x", rt, rs, uimm);
    case 0x0fu:
      return absl::StrFormat("lui $%u,0x%x", rt, uimm);
    case 0x20u:
      return absl::StrFormat("lb $%u,%d($%u)", rt, simm, rs);
    case 0x21u:
      return absl::StrFormat("lh $%u,%d($%u)", rt, simm, rs);
    case 0x23u:
      return absl::StrFormat("lw $%u,%d($%u)", rt, simm, rs);
    case 0x24u:
      return absl::StrFormat("lbu $%u,%d($%u)", rt, simm, rs);
    case 0x25u:
      return absl::StrFormat("lhu $%u,%d($%u)", rt, simm, rs);
    case 0x28u:
      return absl::StrFormat("sb $%u,%d($%u)", rt, simm, rs);
    case 0x29u:
      return absl::StrFormat("sh $%u,%d($%u)", rt, simm, rs);
    case 0x2bu:
      return absl::StrFormat("sw $%u,%d($%u)", rt, simm, rs);
    default:
      break;
  }
  return "nop";
}

std::string ToySubfunctionInverses::InvertGasMacro(absl::Span<const uint32_t> macro_words_x) const {
  if (macro_words_x.size() == 2) {
    const uint32_t w0 = macro_words_x[0];
    const uint32_t w1 = macro_words_x[1];
    const uint32_t op0 = (w0 >> 26) & 0x3fu;
    const uint32_t rt0 = (w0 >> 16) & 0x1fu;
    const uint32_t hi16 = w0 & 0xffffu;
    const uint32_t op1 = (w1 >> 26) & 0x3fu;
    const uint32_t rs1 = (w1 >> 21) & 0x1fu;
    const uint32_t rt1 = (w1 >> 16) & 0x1fu;
    const uint32_t lo16 = w1 & 0xffffu;
    if (op0 == 0x0fu && op1 == 0x0du && rs1 == rt0 && rt1 == rt0) {
      const uint32_t imm32 = (hi16 << 16) | lo16;
      return absl::StrFormat("li $%u,0x%08x", rt0, imm32);
    }
    if (op0 == 0x0fu && op1 == 0x09u && rs1 == rt0 && rt1 == rt0) {
      const int32_t imm32 =
          static_cast<int32_t>(hi16 << 16) + static_cast<int32_t>(static_cast<int16_t>(lo16));
      return absl::StrFormat("li $%u,%d", rt0, imm32);
    }
  } else if (macro_words_x.size() == 3) {
    const uint32_t w0 = macro_words_x[0];
    const uint32_t w1 = macro_words_x[1];
    const uint32_t w2 = macro_words_x[2];
    const uint32_t op0 = (w0 >> 26) & 0x3fu;
    const uint32_t rt0 = (w0 >> 16) & 0x1fu;
    const uint32_t hi16 = w0 & 0xffffu;
    const uint32_t op1 = (w1 >> 26) & 0x3fu;
    const uint32_t lo16 = w1 & 0xffffu;
    const uint32_t op2 = (w2 >> 26) & 0x3fu;
    const uint32_t rs2 = (w2 >> 21) & 0x1fu;
    const uint32_t rt2 = (w2 >> 16) & 0x1fu;
    const uint32_t rd2 = (w2 >> 11) & 0x1fu;
    const uint32_t funct2 = w2 & 0x3fu;
    if (op0 == 0x0fu && rt0 == 1 && op1 == 0x0du && op2 == 0x00u && rt2 == 1) {
      const uint32_t imm32 = (hi16 << 16) | lo16;
      if (funct2 == 0x21u) {
        return absl::StrFormat("addu $%u,$%u,0x%08x", rd2, rs2, imm32);
      }
      if (funct2 == 0x23u) {
        return absl::StrFormat("subu $%u,$%u,0x%08x", rd2, rs2, imm32);
      }
    }
  } else if (macro_words_x.size() == 1) {
    return InvertGasMipsIp(macro_words_x[0]);
  }
  return "nop";
}

std::vector<std::string> ToySubfunctionInverses::InvertGasAppendInsn(
    absl::Span<const uint32_t> words_x) const {
  absl::flat_hash_map<size_t, std::string> target_labels;
  int next_label_id = 2;
  for (size_t i = 0; i < words_x.size(); ++i) {
    const uint32_t w = words_x[i];
    if (((w >> 26) & 0x3fu) == 0x02u) {
      const size_t target_idx = static_cast<size_t>(w & 0x03ffffffu);
      if (target_idx > 0 && target_idx < words_x.size() && !target_labels.contains(target_idx)) {
        target_labels[target_idx] = absl::StrFormat("$L%d", next_label_id++);
      }
    }
  }

  std::vector<std::string> lines;
  size_t i = 0;
  while (i < words_x.size()) {
    if (auto it = target_labels.find(i); it != target_labels.end()) {
      lines.push_back(absl::StrCat(it->second, ":"));
    }
    const uint32_t w = words_x[i];
    if (IsBranchOrJumpWord(w) && i + 1 < words_x.size()) {
      const uint32_t delay_w = words_x[i + 1];
      std::string jump_line = InvertGasMipsIp(w);
      if (((w >> 26) & 0x3fu) == 0x02u) {
        const size_t target_idx = static_cast<size_t>(w & 0x03ffffffu);
        if (auto it = target_labels.find(target_idx); it != target_labels.end()) {
          jump_line = absl::StrCat("j ", it->second);
        }
      }
      if (delay_w == 0x00000000u) {
        if (i > 0) {
          lines.push_back(".set\tnoreorder");
          lines.push_back(std::move(jump_line));
          lines.push_back("nop");
          lines.push_back(".set\treorder");
        } else {
          // `append_insn` emitted a delay-slot `nop` because no prior instruction
          // could be swapped into the delay slot.
          lines.push_back(std::move(jump_line));
        }
      } else {
        // `append_insn` swapped the preceding instruction into the delay slot;
        // restore the pre-`append_insn` order `[delay_insn, jump_insn]`.
        lines.push_back(InvertGasMipsIp(delay_w));
        lines.push_back(std::move(jump_line));
      }
      i += 2;
      continue;
    }
    lines.push_back(InvertGasMipsIp(w));
    ++i;
  }
  return lines;
}

std::string ToySubfunctionInverses::InvertFinalScanInsn(std::string_view asm_line_x) const {
  std::string_view line = absl::StripAsciiWhitespace(asm_line_x);
  const size_t comment_pos = line.find('#');
  if (comment_pos != std::string_view::npos) {
    line = absl::StripAsciiWhitespace(line.substr(0, comment_pos));
  }
  if (line == "nop") {
    return "(const_int 0)";
  }
  if (line == "j\t$31" || line == "j $31" || line == "jr\t$31" || line == "jr $31") {
    return "(return)";
  }

  size_t sp = 0;
  while (sp < line.size() && !absl::ascii_isspace(line[sp])) {
    ++sp;
  }
  const std::string_view mnemonic = line.substr(0, sp);
  const std::string_view args_str = absl::StripAsciiWhitespace(line.substr(sp));
  std::vector<std::string_view> args = absl::StrSplit(args_str, ',');

  auto strip_label_prefix = [](std::string_view lbl) -> std::string {
    lbl = absl::StripAsciiWhitespace(lbl);
    if (absl::StartsWith(lbl, "$L")) {
      return std::string(lbl.substr(1));
    }
    return std::string(lbl);
  };

  if ((mnemonic == "j" || mnemonic == "b") && args.size() == 1) {
    const std::string lbl = strip_label_prefix(args[0]);
    if (absl::StartsWith(lbl, "L")) {
      return absl::StrFormat("(set (pc) (label_ref %s))", lbl);
    }
  } else if ((mnemonic == "beq" || mnemonic == "bne") && args.size() == 3) {
    const int rs = ParseMipsAsmRegNumber(args[0]);
    const int rt = ParseMipsAsmRegNumber(args[1]);
    const std::string lbl = strip_label_prefix(args[2]);
    if (rs >= 0 && rt >= 0 && absl::StartsWith(lbl, "L")) {
      const std::string_view cmp_op = mnemonic == "beq" ? "eq" : "ne";
      const std::string rhs_op = rt == 0 ? "(const_int 0)" : FormatHardRegRtl(rt);
      return absl::StrFormat("(set (pc) (if_then_else (%s:SI %s %s) (label_ref %s) (pc)))", cmp_op,
                             FormatHardRegRtl(rs), rhs_op, lbl);
    }
  } else if ((mnemonic == "bgtz" || mnemonic == "blez" || mnemonic == "bltz" ||
              mnemonic == "bgez") &&
             args.size() == 2) {
    const int rs = ParseMipsAsmRegNumber(args[0]);
    const std::string lbl = strip_label_prefix(args[1]);
    if (rs >= 0 && absl::StartsWith(lbl, "L")) {
      const std::string_view cmp_op =
          mnemonic == "bgtz" ? "gt"
                             : (mnemonic == "blez" ? "le" : (mnemonic == "bltz" ? "lt" : "ge"));
      return absl::StrFormat(
          "(set (pc) (if_then_else (%s:SI %s (const_int 0)) (label_ref %s) (pc)))", cmp_op,
          FormatHardRegRtl(rs), lbl);
    }
  } else if ((mnemonic == "addu" || mnemonic == "addiu") && args.size() == 3) {
    const int rd = ParseMipsAsmRegNumber(args[0]);
    const int rs = ParseMipsAsmRegNumber(args[1]);
    const int rt = ParseMipsAsmRegNumber(args[2]);
    if (rd >= 0 && rs >= 0 && rt >= 0) {
      return absl::StrFormat("(set %s (plus:SI %s %s))", FormatHardRegRtl(rd), FormatHardRegRtl(rs),
                             FormatHardRegRtl(rt));
    }
    int imm = 0;
    if (rd >= 0 && rs >= 0 && absl::SimpleAtoi(absl::StripAsciiWhitespace(args[2]), &imm)) {
      return absl::StrFormat("(set %s (plus:SI %s (const_int %d)))", FormatHardRegRtl(rd),
                             FormatHardRegRtl(rs), imm);
    }
  } else if (mnemonic == "subu" && args.size() == 3) {
    const int rd = ParseMipsAsmRegNumber(args[0]);
    const int rs = ParseMipsAsmRegNumber(args[1]);
    const int rt = ParseMipsAsmRegNumber(args[2]);
    if (rd >= 0 && rs >= 0 && rt >= 0) {
      return absl::StrFormat("(set %s (minus:SI %s %s))", FormatHardRegRtl(rd),
                             FormatHardRegRtl(rs), FormatHardRegRtl(rt));
    }
    int imm = 0;
    if (rd >= 0 && rs >= 0 && absl::SimpleAtoi(absl::StripAsciiWhitespace(args[2]), &imm)) {
      return absl::StrFormat("(set %s (minus:SI %s (const_int %d)))", FormatHardRegRtl(rd),
                             FormatHardRegRtl(rs), imm);
    }
  } else if ((mnemonic == "and" || mnemonic == "andi" || mnemonic == "or" || mnemonic == "ori" ||
              mnemonic == "xor" || mnemonic == "xori" || mnemonic == "slt" || mnemonic == "slti" ||
              mnemonic == "sltu" || mnemonic == "sltiu") &&
             args.size() == 3) {
    const int rd = ParseMipsAsmRegNumber(args[0]);
    const int rs = ParseMipsAsmRegNumber(args[1]);
    const int rt = ParseMipsAsmRegNumber(args[2]);
    const std::string_view rtl_op =
        absl::StartsWith(mnemonic, "and")
            ? "and"
            : (absl::StartsWith(mnemonic, "or")
                   ? "ior"
                   : (absl::StartsWith(mnemonic, "xor")
                          ? "xor"
                          : (absl::StartsWith(mnemonic, "sltu") ? "ltu" : "lt")));
    if (rd >= 0 && rs >= 0 && rt >= 0) {
      return absl::StrFormat("(set %s (%s:SI %s %s))", FormatHardRegRtl(rd), rtl_op,
                             FormatHardRegRtl(rs), FormatHardRegRtl(rt));
    }
    std::string_view imm_tok = absl::StripAsciiWhitespace(args[2]);
    int64_t imm = 0;
    bool parsed_imm = false;
    if (absl::StartsWith(imm_tok, "0x") || absl::StartsWith(imm_tok, "0X")) {
      uint32_t uimm = 0;
      if (absl::SimpleHexAtoi(imm_tok, &uimm)) {
        imm = static_cast<int32_t>(uimm);
        parsed_imm = true;
      }
    } else if (absl::SimpleAtoi(imm_tok, &imm)) {
      parsed_imm = true;
    }
    if (rd >= 0 && rs >= 0 && parsed_imm) {
      return absl::StrFormat("(set %s (%s:SI %s (const_int %d)))", FormatHardRegRtl(rd), rtl_op,
                             FormatHardRegRtl(rs), static_cast<int32_t>(imm));
    }
  } else if ((mnemonic == "sll" || mnemonic == "sra" || mnemonic == "srl") && args.size() == 3) {
    const int rd = ParseMipsAsmRegNumber(args[0]);
    const int rt = ParseMipsAsmRegNumber(args[1]);
    int sa = 0;
    if (rd >= 0 && rt >= 0 && absl::SimpleAtoi(absl::StripAsciiWhitespace(args[2]), &sa)) {
      const std::string_view rtl_op =
          mnemonic == "sll" ? "ashift" : (mnemonic == "sra" ? "ashiftrt" : "lshiftrt");
      return absl::StrFormat("(set %s (%s:SI %s (const_int %d)))", FormatHardRegRtl(rd), rtl_op,
                             FormatHardRegRtl(rt), sa);
    }
  } else if (mnemonic == "li" && args.size() == 2) {
    const int rd = ParseMipsAsmRegNumber(args[0]);
    std::string_view imm_tok = absl::StripAsciiWhitespace(args[1]);
    int64_t val = 0;
    if (absl::StartsWith(imm_tok, "0x") || absl::StartsWith(imm_tok, "0X")) {
      uint32_t uval = 0;
      if (absl::SimpleHexAtoi(imm_tok, &uval)) {
        val = static_cast<int32_t>(uval);
      }
    } else {
      (void)absl::SimpleAtoi(imm_tok, &val);
    }
    if (rd >= 0) {
      return absl::StrFormat("(set %s (const_int %d))", FormatHardRegRtl(rd),
                             static_cast<int32_t>(val));
    }
  } else if (mnemonic == "move" && args.size() == 2) {
    const int rd = ParseMipsAsmRegNumber(args[0]);
    const int rs = ParseMipsAsmRegNumber(args[1]);
    if (rd >= 0 && rs >= 0) {
      return absl::StrFormat("(set %s %s)", FormatHardRegRtl(rd), FormatHardRegRtl(rs));
    }
  } else if ((mnemonic == "lw" || mnemonic == "sw") && args.size() == 2) {
    const int rt = ParseMipsAsmRegNumber(args[0]);
    std::string_view mem_arg = absl::StripAsciiWhitespace(args[1]);
    const size_t lparen = mem_arg.find('(');
    const size_t rparen = mem_arg.find(')');
    if (rt >= 0 && lparen != std::string_view::npos && rparen != std::string_view::npos &&
        rparen > lparen) {
      int offset = 0;
      if (lparen > 0) {
        (void)absl::SimpleAtoi(mem_arg.substr(0, lparen), &offset);
      }
      const int base = ParseMipsAsmRegNumber(mem_arg.substr(lparen + 1, rparen - lparen - 1));
      if (base >= 0) {
        const std::string addr = offset == 0 ? FormatHardRegRtl(base)
                                             : absl::StrFormat("(plus:SI %s (const_int %d))",
                                                               FormatHardRegRtl(base), offset);
        if (mnemonic == "lw") {
          return absl::StrFormat("(set %s (mem:SI %s))", FormatHardRegRtl(rt), addr);
        }
        return absl::StrFormat("(set (mem:SI %s) %s)", addr, FormatHardRegRtl(rt));
      }
    }
  }
  return {};
}

PrologueEpilogueFrameSpec ToySubfunctionInverses::InvertFunctionPrologueEpilogue(
    std::string_view asm_text_x) const {
  PrologueEpilogueFrameSpec spec;
  for (std::string_view raw_line : absl::StrSplit(asm_text_x, '\n')) {
    std::string_view line = absl::StripAsciiWhitespace(raw_line);
    const size_t vars_pos = line.find("vars=");
    if (vars_pos != std::string_view::npos) {
      std::string_view rest = absl::StripAsciiWhitespace(line.substr(vars_pos + 5));
      const size_t comma = rest.find(',');
      if (comma != std::string_view::npos) {
        rest = rest.substr(0, comma);
      }
      (void)absl::SimpleAtoi(absl::StripAsciiWhitespace(rest), &spec.var_size);
    } else if (absl::StartsWith(line, ".mask")) {
      std::string_view rest = absl::StripAsciiWhitespace(line.substr(5));
      const size_t comma = rest.find(',');
      if (comma != std::string_view::npos) {
        rest = rest.substr(0, comma);
      }
      (void)absl::SimpleHexAtoi(absl::StripAsciiWhitespace(rest), &spec.saved_gpr_mask);
    }
  }
  return spec;
}

RtlControlFlowGraph ToySubfunctionInverses::InvertFinalAndGasFunction(
    absl::Span<const uint32_t> machine_words_x) const {
  RtlControlFlowGraph cfg;
  cfg.pass = RtlPassKind::kDbr;
  cfg.rpo_blocks.resize(1);
  cfg.rpo_blocks[0].id = 0;

  size_t i = 0;
  while (i < machine_words_x.size()) {
    const uint32_t w = machine_words_x[i];
    if (w == 0x03e00008u && i + 1 < machine_words_x.size()) {
      const uint32_t delay_w = machine_words_x[i + 1];
      if (delay_w == 0x00000000u) {
        cfg.rpo_blocks[0].AddPatternOnly("(sequence [(jump_insn (return)) (insn (const_int 0))])");
      } else {
        cfg.rpo_blocks[0].AddPatternOnly(
            absl::StrFormat("(sequence [(jump_insn (return)) (insn %s)])",
                            InvertWordToRtlSexpr(delay_w, /*is_return_value=*/true)));
      }
      i += 2;
      continue;
    }
    // Check 2-word `lui` + `ori`/`addiu` constant synthesis.
    if (i + 1 < machine_words_x.size()) {
      const uint32_t w1 = machine_words_x[i + 1];
      const uint32_t op0 = (w >> 26) & 0x3fu;
      const uint32_t rt0 = (w >> 16) & 0x1fu;
      const uint32_t op1 = (w1 >> 26) & 0x3fu;
      const uint32_t rs1 = (w1 >> 21) & 0x1fu;
      const uint32_t rt1 = (w1 >> 16) & 0x1fu;
      if (op0 == 0x0fu && (op1 == 0x0du || op1 == 0x09u) && rs1 == rt0 && rt1 == rt0) {
        const int32_t imm32 = op1 == 0x0du
                                  ? static_cast<int32_t>(((w & 0xffffu) << 16) | (w1 & 0xffffu))
                                  : static_cast<int32_t>((w & 0xffffu) << 16) +
                                        static_cast<int32_t>(static_cast<int16_t>(w1 & 0xffffu));
        cfg.rpo_blocks[0].AddPatternOnly(
            absl::StrFormat("(set %s (const_int %d))", FormatHardRegRtl(rt0), imm32));
        i += 2;
        continue;
      }
    }
    const bool next_is_unscheduled_ret =
        (i + 2 == machine_words_x.size() - 1 && machine_words_x[i + 1] == 0x03e00008u &&
         machine_words_x[i + 2] == 0x00000000u);
    cfg.rpo_blocks[0].AddPatternOnly(InvertWordToRtlSexpr(w, next_is_unscheduled_ret));
    ++i;
  }
  return cfg;
}

// -----------------------------------------------------------------------------
// Stage 1: Delayed-Branch Scheduling (`reorg.c`)
// -----------------------------------------------------------------------------

RtlControlFlowGraph ToySubfunctionInverses::InvertMakeReturnInsns(
    const RtlControlFlowGraph& x) const {
  RtlControlFlowGraph out = x;
  const size_t end_block_id = out.rpo_blocks.size();
  const std::string end_label = absl::StrFormat("L%zu", end_block_id);
  const std::string jump_to_end = absl::StrFormat("(set (pc) (label_ref %s))", end_label);

  bool converted_any = false;
  for (auto& blk : out.rpo_blocks) {
    for (size_t i = 0; i < blk.instructions.size(); ++i) {
      std::string& pat = blk.instructions[i];
      const size_t seq_ret_pos = pat.find("(jump_insn (return))");
      if (seq_ret_pos != std::string::npos) {
        pat.replace(seq_ret_pos, std::string_view("(jump_insn (return))").size(),
                    absl::StrCat("(jump_insn ", jump_to_end, ")"));
        if (blk.successors.empty()) {
          blk.successors.push_back(end_block_id);
        }
        converted_any = true;
      } else if (pat == "(return)") {
        pat = jump_to_end;
        if (blk.successors.empty()) {
          blk.successors.push_back(end_block_id);
        }
        converted_any = true;
      }
    }
    blk.SyncRtlInsnsFromInstructions();
  }

  if (converted_any) {
    RtlBasicBlock end_blk;
    end_blk.id = end_block_id;
    out.rpo_blocks.push_back(std::move(end_blk));
  }
  return out;
}

RtlControlFlowGraph ToySubfunctionInverses::InvertRelaxDelaySlots(
    const RtlControlFlowGraph& x) const {
  for (size_t b = 0; b < x.rpo_blocks.size(); ++b) {
    const auto& blk = x.rpo_blocks[b];
    if (blk.instructions.empty() || blk.successors.size() < 2) {
      continue;
    }
    const std::string& last_pat = blk.instructions.back();
    const std::string inverted_branch = InvertBranchConditionInRtlPattern(last_pat);
    if (inverted_branch == last_pat) {
      continue;
    }

    const size_t fallthrough_b = (blk.successors.size() >= 2 && blk.successors[1] == b + 1)
                                     ? blk.successors[1]
                                     : blk.successors[0];
    const size_t target_b =
        (blk.successors[0] == fallthrough_b) ? blk.successors[1] : blk.successors[0];
    if (fallthrough_b != b + 1 || target_b == b + 1) {
      continue;
    }

    // Shift all block indices >= b + 1 by +1 to make room for the trampoline
    // block at index `b + 1`.
    auto shift_idx = [b](size_t idx) -> size_t { return idx >= b + 1 ? idx + 1 : idx; };

    RtlControlFlowGraph out;
    out.pass = x.pass;
    out.rpo_blocks.reserve(x.rpo_blocks.size() + 1);

    for (size_t i = 0; i <= b; ++i) {
      RtlBasicBlock copy = x.rpo_blocks[i];
      copy.id = i;
      for (size_t& succ : copy.successors) {
        succ = shift_idx(succ);
      }
      out.rpo_blocks.push_back(std::move(copy));
    }

    // Invert the branch condition in block `b` so it jumps over the trampoline
    // (`b + 1`) to `b + 2` (`shift_idx(fallthrough_b)`).
    out.rpo_blocks[b].instructions.back() = inverted_branch;
    out.rpo_blocks[b].SyncRtlInsnsFromInstructions();
    out.rpo_blocks[b].successors = {b + 2, b + 1};

    // Insert trampoline block at `b + 1` jumping unconditionally to
    // `shift_idx(target_b)`.
    const size_t shifted_target = shift_idx(target_b);
    RtlBasicBlock tramp;
    tramp.id = b + 1;
    tramp.AddPatternOnly(absl::StrFormat("(set (pc) (label_ref L%zu))", shifted_target));
    tramp.successors = {shifted_target};
    out.rpo_blocks.push_back(std::move(tramp));

    for (size_t i = b + 1; i < x.rpo_blocks.size(); ++i) {
      RtlBasicBlock copy = x.rpo_blocks[i];
      copy.id = i + 1;
      for (size_t& succ : copy.successors) {
        succ = shift_idx(succ);
      }
      out.rpo_blocks.push_back(std::move(copy));
    }
    return out;
  }
  return x;
}

RtlControlFlowGraph ToySubfunctionInverses::InvertFillEagerDelaySlots(
    const RtlControlFlowGraph& x) const {
  RtlControlFlowGraph out = x;
  for (size_t b = 0; b < out.rpo_blocks.size(); ++b) {
    auto& blk = out.rpo_blocks[b];
    if (blk.instructions.empty() || blk.successors.size() < 2) {
      continue;
    }
    std::string jump_pat;
    std::string delay_tag;
    std::string delay_pat;
    if (!ExtractOuterSequenceParts(blk.instructions.back(), &jump_pat, &delay_tag, &delay_pat)) {
      continue;
    }
    if (!absl::StrContains(jump_pat, "(if_then_else ")) {
      continue;
    }

    const size_t fallthrough_b =
        (blk.successors[1] == b + 1) ? blk.successors[1] : blk.successors[0];
    const size_t target_b =
        (blk.successors[0] == fallthrough_b) ? blk.successors[1] : blk.successors[0];

    blk.instructions.back() = jump_pat;
    blk.SyncRtlInsnsFromInstructions();

    const size_t dest_b = absl::StartsWith(delay_tag, "insn/s") ? target_b : fallthrough_b;
    if (dest_b < out.rpo_blocks.size()) {
      auto& dest_blk = out.rpo_blocks[dest_b];
      dest_blk.instructions.insert(dest_blk.instructions.begin(), delay_pat);
      dest_blk.SyncRtlInsnsFromInstructions();
    }
  }
  return out;
}

RtlControlFlowGraph ToySubfunctionInverses::InvertFillSimpleDelaySlots(
    const RtlControlFlowGraph& x) const {
  RtlControlFlowGraph out = x;
  for (auto& blk : out.rpo_blocks) {
    std::vector<std::string> expanded;
    expanded.reserve(blk.instructions.size() + 1);
    for (const std::string& pat : blk.instructions) {
      std::string jump_pat;
      std::string delay_tag;
      std::string delay_pat;
      if (ExtractOuterSequenceParts(pat, &jump_pat, &delay_tag, &delay_pat)) {
        expanded.push_back(delay_pat);
        expanded.push_back(jump_pat);
      } else {
        expanded.push_back(pat);
      }
    }
    blk.instructions = std::move(expanded);
    blk.SyncRtlInsnsFromInstructions();
  }
  return out;
}

RtlControlFlowGraph ToySubfunctionInverses::InvertDbrSchedule(const RtlControlFlowGraph& x) const {
  return InvertFillSimpleDelaySlots(
      InvertFillEagerDelaySlots(InvertRelaxDelaySlots(InvertMakeReturnInsns(x))));
}

// -----------------------------------------------------------------------------
// Stage 2: Post-Reload Cross-Jumping (`jump.c`)
// -----------------------------------------------------------------------------

RtlControlFlowGraph ToySubfunctionInverses::InvertDoCrossJump(const RtlControlFlowGraph& x) const {
  // Locate a join block `join_b` with >= 2 incoming edges where at least one
  // predecessor `pred_jump_b` ends with an unconditional jump `(set (pc) (label_ref ...))`.
  for (size_t join_b = 0; join_b < x.rpo_blocks.size(); ++join_b) {
    const auto& join_blk = x.rpo_blocks[join_b];
    if (join_blk.instructions.empty()) {
      continue;
    }
    std::vector<size_t> preds;
    for (size_t p = 0; p < x.rpo_blocks.size(); ++p) {
      for (size_t s : x.rpo_blocks[p].successors) {
        if (s == join_b) {
          preds.push_back(p);
        }
      }
    }
    if (preds.size() < 2) {
      continue;
    }

    // Separate the shared non-jump tail instructions in `join_blk` from the
    // trailing `(use ...)` / `(return)` epilogue.
    std::vector<std::string> shared_insns;
    std::vector<std::string> epilogue_insns;
    for (const std::string& pat : join_blk.instructions) {
      if (absl::StartsWith(pat, "(use ") || pat == "(return)" ||
          absl::StartsWith(pat, "(set (pc)")) {
        epilogue_insns.push_back(pat);
      } else if (epilogue_insns.empty()) {
        shared_insns.push_back(pat);
      } else {
        epilogue_insns.push_back(pat);
      }
    }
    if (shared_insns.empty()) {
      continue;
    }
    if (shared_insns.size() >= 2) {
      epilogue_insns.insert(epilogue_insns.begin(), shared_insns.back());
      shared_insns.pop_back();
    }

    RtlControlFlowGraph out = x;
    for (size_t pred_b : preds) {
      auto& pred_blk = out.rpo_blocks[pred_b];
      if (!pred_blk.instructions.empty() &&
          absl::StartsWith(pred_blk.instructions.back(), "(set (pc) (label_ref")) {
        std::string jmp = pred_blk.instructions.back();
        pred_blk.instructions.pop_back();
        for (const std::string& s_pat : shared_insns) {
          pred_blk.instructions.push_back(s_pat);
        }
        pred_blk.instructions.push_back(std::move(jmp));
      } else {
        for (const std::string& s_pat : shared_insns) {
          pred_blk.instructions.push_back(s_pat);
        }
      }
      pred_blk.SyncRtlInsnsFromInstructions();
    }
    out.rpo_blocks[join_b].instructions =
        epilogue_insns.empty() ? std::vector<std::string>{"(return)"} : std::move(epilogue_insns);
    out.rpo_blocks[join_b].SyncRtlInsnsFromInstructions();
    return out;
  }
  return x;
}

// -----------------------------------------------------------------------------
// Stage 3 & Stage 6: Instruction Scheduling (`sched.c`)
// -----------------------------------------------------------------------------

RtlControlFlowGraph ToySubfunctionInverses::InvertScheduleBlock(
    const RtlControlFlowGraph& x) const {
  RtlControlFlowGraph out = x;
  for (auto& blk : out.rpo_blocks) {
    for (size_t i = 0; i + 2 < blk.instructions.size(); ++i) {
      const std::string p0 = StripTrailingRegNotesFromPattern(blk.instructions[i]);
      const std::string p1 = StripTrailingRegNotesFromPattern(blk.instructions[i + 1]);
      if (absl::StartsWith(p0, "(set (reg:SI ") && absl::StrContains(p0, "(mem:") &&
          !absl::StrContains(p1, "(mem:")) {
        const size_t dest_end = p0.find(')', 5);
        const std::string dest_reg =
            dest_end != std::string::npos ? p0.substr(5, dest_end - 4) : "";
        if (!dest_reg.empty() && !absl::StrContains(p1, dest_reg)) {
          blk.instructions[i] = p1;
          blk.instructions[i + 1] = p0;
          break;
        }
      }
    }
    blk.SyncRtlInsnsFromInstructions();
  }
  return out;
}

// -----------------------------------------------------------------------------
// Stage 4 & Stage 5: Register Allocation & Reload (`reload1.c`, `local-alloc.c`)
// -----------------------------------------------------------------------------

RtlControlFlowGraph ToySubfunctionInverses::InvertAlterReg(const RtlControlFlowGraph& x,
                                                           int pseudo_regno) const {
  RtlControlFlowGraph out = x;
  if (out.sideband.reg_renumber.empty()) {
    out.sideband.max_regno = std::max(out.sideband.max_regno, pseudo_regno + 1);
    out.sideband.reg_renumber[pseudo_regno] = 2;
    for (auto& blk : out.rpo_blocks) {
      for (std::string& pat : blk.instructions) {
        auto parsed = ParsePatternAndNotes(pat);
        if (parsed.ok()) {
          parsed->RewriteRegs([pseudo_regno](RtlExpr& reg) {
            if (reg.RegNo() == 2 && !reg.flags.integrated) {
              reg.SetRegNo(pseudo_regno);
            }
          });
          pat = parsed->ToSexprString();
        }
      }
      blk.SyncRtlInsnsFromInstructions();
    }
    return out;
  }

  for (const auto& [cand_pseudo, target_hard] : out.sideband.reg_renumber) {
    if (cand_pseudo < 67 || target_hard < 0) {
      continue;
    }
    const int p_reg = cand_pseudo;
    const int h_reg = target_hard;
    bool has_user_var_match = false;
    RtlMachineMode target_mode = RtlMachineMode::kNone;
    for (const auto& blk : out.rpo_blocks) {
      for (const std::string& pat : blk.instructions) {
        auto parsed = ParsePatternAndNotes(pat);
        if (!parsed.ok()) {
          continue;
        }
        parsed->RewriteRegs([&](RtlExpr& reg) {
          if (reg.RegNo() == h_reg && reg.flags.volatil && !reg.flags.integrated &&
              !reg.flags.in_struct && !reg.flags.unchanging && !reg.flags.frame_related) {
            if (!has_user_var_match) {
              target_mode = reg.mode;
              has_user_var_match = true;
            }
          }
        });
      }
    }
    if (!has_user_var_match) {
      continue;
    }
    out.sideband.max_regno = std::max(out.sideband.max_regno, p_reg + 1);
    for (auto& blk : out.rpo_blocks) {
      for (std::string& pat : blk.instructions) {
        auto parsed = ParsePatternAndNotes(pat);
        if (parsed.ok()) {
          parsed->RewriteRegs([p_reg, h_reg, target_mode](RtlExpr& reg) {
            if (reg.RegNo() == h_reg && reg.mode == target_mode && reg.flags.volatil &&
                !reg.flags.integrated && !reg.flags.in_struct && !reg.flags.unchanging &&
                !reg.flags.frame_related) {
              reg.SetRegNo(p_reg);
            }
          });
          pat = parsed->ToSexprString();
        }
      }
      blk.SyncRtlInsnsFromInstructions();
    }
    break;
  }
  return out;
}

RtlControlFlowGraph ToySubfunctionInverses::InvertReload(const RtlControlFlowGraph& x) const {
  RtlControlFlowGraph out = x;
  for (const auto& blk : out.rpo_blocks) {
    for (const std::string& pat : blk.instructions) {
      if (absl::StartsWith(pat, "(set (reg:SI 29 sp)")) {
        return out;
      }
    }
  }
  constexpr std::string_view kMemSp = "(mem:SI (reg:SI 29 sp))";
  constexpr std::string_view kMemFp = "(mem:SI (reg:SI 30 fp))";
  for (auto& blk : out.rpo_blocks) {
    for (std::string& pat : blk.instructions) {
      size_t pos = 0;
      while ((pos = pat.find(kMemSp, pos)) != std::string::npos) {
        pat.replace(pos, kMemSp.size(), kMemFp);
        pos += kMemFp.size();
      }
    }
    blk.SyncRtlInsnsFromInstructions();
  }
  return out;
}

RtlControlFlowGraph ToySubfunctionInverses::InvertGlobalAlloc(const RtlControlFlowGraph& x) const {
  RtlControlFlowGraph out = x;
  out.pass = RtlPassKind::kLreg;
  if (out.sideband.reg_renumber.empty()) {
    return out;
  }

  absl::flat_hash_map<int, absl::flat_hash_set<size_t>> pseudo_blocks;
  for (size_t b = 0; b < out.rpo_blocks.size(); ++b) {
    for (const std::string& pat : out.rpo_blocks[b].instructions) {
      auto parsed = ParsePatternAndNotes(pat);
      if (!parsed.ok()) {
        continue;
      }
      parsed->RewriteRegs([&pseudo_blocks, b](RtlExpr& reg) {
        if (reg.RegNo() >= 67) {
          pseudo_blocks[reg.RegNo()].insert(b);
        }
      });
    }
  }

  bool erased_multi_block = false;
  for (const auto& [regno, blocks] : pseudo_blocks) {
    if (blocks.size() >= 2 && out.sideband.reg_renumber.contains(regno)) {
      out.sideband.reg_renumber.erase(regno);
      erased_multi_block = true;
    }
  }
  if (!erased_multi_block) {
    out.sideband.reg_renumber.clear();
  }
  return out;
}

RtlControlFlowGraph ToySubfunctionInverses::InvertBlockAlloc(const RtlControlFlowGraph& x) const {
  RtlControlFlowGraph out = x;
  out.sideband.reg_renumber.clear();
  for (auto& blk : out.rpo_blocks) {
    for (std::string& pat : blk.instructions) {
      constexpr std::string_view kClobberHi = "(clobber (reg:SI 64 hi))";
      constexpr std::string_view kClobberAccum = "(clobber (reg:SI 66 accum))";
      constexpr std::string_view kClobberHilo = "(clobber (reg:SI 66 hilo))";
      constexpr std::string_view kClobberScratch = "(clobber (scratch:SI))";
      bool replaced_clobber = false;
      size_t pos = 0;
      while ((pos = pat.find(kClobberHi, pos)) != std::string::npos) {
        pat.replace(pos, kClobberHi.size(), kClobberScratch);
        pos += kClobberScratch.size();
        replaced_clobber = true;
      }
      pos = 0;
      while ((pos = pat.find(kClobberAccum, pos)) != std::string::npos) {
        pat.replace(pos, kClobberAccum.size(), kClobberScratch);
        pos += kClobberScratch.size();
        replaced_clobber = true;
      }
      pos = 0;
      while ((pos = pat.find(kClobberHilo, pos)) != std::string::npos) {
        pat.replace(pos, kClobberHilo.size(), kClobberScratch);
        pos += kClobberScratch.size();
        replaced_clobber = true;
      }
      if (replaced_clobber || out.sideband.reg_n_deaths.empty()) {
        pat = StripTrailingRegNotesFromPattern(pat);
      }
    }
    blk.SyncRtlInsnsFromInstructions();
  }
  return out;
}

RtlControlFlowGraph ToySubfunctionInverses::InvertUpdateEquivRegs(
    const RtlControlFlowGraph& x) const {
  RtlControlFlowGraph out = x;
  for (auto& blk : out.rpo_blocks) {
    bool rewrote_copy = false;
    for (size_t i = 0; i + 1 < blk.instructions.size(); ++i) {
      std::string& cur = blk.instructions[i];
      std::string& next = blk.instructions[i + 1];
      // Detect `(set <dest> <src_pseudo>) (expr_list:REG_DEAD <src_pseudo> (nil))`
      // produced by `optimize_reg_copy_1` inside `update_equiv_regs`.
      const size_t dead_pos = cur.find(" (expr_list:REG_DEAD (reg:SI ");
      if (dead_pos == std::string::npos) {
        continue;
      }
      const size_t reg_start = dead_pos + std::string_view(" (expr_list:REG_DEAD ").size();
      const size_t reg_end = cur.find(')', reg_start);
      if (reg_end == std::string::npos) {
        continue;
      }
      const std::string src_pseudo = cur.substr(reg_start, reg_end - reg_start + 1);
      const std::string base_cur = cur.substr(0, dead_pos);
      if (!absl::StartsWith(base_cur, "(set ") ||
          !absl::EndsWith(base_cur, absl::StrCat(" ", src_pseudo, ")"))) {
        continue;
      }
      const size_t dest_start = std::string_view("(set ").size();
      const size_t dest_end = base_cur.size() - src_pseudo.size() - 2;  // before ` <src_pseudo>)`
      const std::string dest_reg = base_cur.substr(dest_start, dest_end - dest_start);

      // Replace the second occurrence of `dest_reg` (the source operand in `next`)
      // back with `src_pseudo`.
      const size_t use_pos = next.rfind(dest_reg);
      if (use_pos != std::string::npos && use_pos > 5) {
        cur = base_cur;
        next.replace(use_pos, dest_reg.size(), src_pseudo);
        rewrote_copy = true;
        break;
      }
    }
    if (rewrote_copy) {
      for (std::string& pat : blk.instructions) {
        pat = StripTrailingRegNotesFromPattern(pat);
      }
    }
    blk.SyncRtlInsnsFromInstructions();
  }
  return out;
}

// -----------------------------------------------------------------------------
// Stage 7: Instruction Combination (`combine.c`)
// -----------------------------------------------------------------------------

std::string ToySubfunctionInverses::InvertSimplifyRtx(std::string_view simplified_rtx_x) const {
  std::string_view s = absl::StripAsciiWhitespace(simplified_rtx_x);
  if (absl::StartsWith(s, "(ge:SI ") && absl::EndsWith(s, " (const_int 1))")) {
    std::string_view inner = s.substr(
        std::string_view("(ge:SI ").size(),
        s.size() - std::string_view("(ge:SI ").size() - std::string_view(" (const_int 1))").size());
    return absl::StrFormat("(xor:SI (lt:SI %s (const_int 1)) (const_int 1))", inner);
  }
  if (absl::StartsWith(s, "(eq:SI ") && absl::EndsWith(s, " (const_int 0))")) {
    std::string_view inner = s.substr(
        std::string_view("(eq:SI ").size(),
        s.size() - std::string_view("(eq:SI ").size() - std::string_view(" (const_int 0))").size());
    return absl::StrFormat("(xor:SI (ne:SI %s (const_int 0)) (const_int 1))", inner);
  }
  return absl::StrFormat("(plus:SI %s (const_int 0))", s);
}

SubstInverseSpec ToySubfunctionInverses::InvertSubst(std::string_view subst_rtx_x) const {
  std::string_view s = absl::StripAsciiWhitespace(subst_rtx_x);
  if (absl::StartsWith(s, "(set ") && absl::StrContains(s, " (gt:SI ") &&
      absl::EndsWith(s, " (const_int 0)))")) {
    const size_t gt_pos = s.find(" (gt:SI ");
    const std::string_view dst = s.substr(5, gt_pos - 5);
    const size_t src_start = gt_pos + std::string_view(" (gt:SI ").size();
    const size_t src_end = s.size() - std::string_view(" (const_int 0)))").size();
    const std::string_view src = s.substr(src_start, src_end - src_start);
    return SubstInverseSpec{
        .target_expr = absl::StrFormat("(set %s (xor:SI (reg:SI 72) (const_int 1)))", dst),
        .from_reg = "(reg:SI 72)",
        .to_expr = absl::StrFormat("(lt:SI %s (const_int 1))", src),
    };
  }
  if (absl::StartsWith(s, "(set ") && absl::StrContains(s, " (ge:SI ") &&
      absl::EndsWith(s, " (const_int 1)))")) {
    const size_t ge_pos = s.find(" (ge:SI ");
    const std::string_view dst = s.substr(5, ge_pos - 5);
    const size_t src_start = ge_pos + std::string_view(" (ge:SI ").size();
    const size_t src_end = s.size() - std::string_view(" (const_int 1)))").size();
    const std::string_view src = s.substr(src_start, src_end - src_start);
    return SubstInverseSpec{
        .target_expr = absl::StrFormat("(set %s (xor:SI (reg:SI 72) (const_int 1)))", dst),
        .from_reg = "(reg:SI 72)",
        .to_expr = absl::StrFormat("(lt:SI %s (const_int 1))", src),
    };
  }
  // General decomposition: replace the RHS of `(set <dst> <rhs>)` with `(reg:SI 72)`
  // (or `(plus:SI (reg:SI 72) (const_int 0))` for compound arithmetic/shift/logic RHS).
  if (absl::StartsWith(s, "(set ") && s.back() == ')') {
    size_t pos = std::string_view("(set ").size();
    int depth = 0;
    for (; pos < s.size(); ++pos) {
      if (s[pos] == '(') ++depth;
      if (s[pos] == ')' && --depth == 0) {
        ++pos;
        break;
      }
    }
    std::string_view dst = absl::StripAsciiWhitespace(
        s.substr(std::string_view("(set ").size(), pos - std::string_view("(set ").size()));
    std::string_view rhs = absl::StripAsciiWhitespace(s.substr(pos, s.size() - pos - 1));
    const bool is_compound_rhs =
        dst != "(pc)" && absl::StartsWith(rhs, "(") && !absl::StartsWith(rhs, "(reg") &&
        !absl::StartsWith(rhs, "(subreg") && !absl::StartsWith(rhs, "(const_int") &&
        !absl::StartsWith(rhs, "(mem") && !absl::StartsWith(rhs, "(clobber") &&
        !absl::StartsWith(rhs, "(call") && !absl::StartsWith(rhs, "(if_then_else") &&
        !absl::StartsWith(rhs, "(label_ref") && rhs != "(pc)";
    if (is_compound_rhs) {
      return SubstInverseSpec{
          .target_expr = absl::StrFormat("(set %s (plus:SI (reg:SI 72) (const_int 0)))", dst),
          .from_reg = "(reg:SI 72)",
          .to_expr = std::string(rhs),
      };
    }
    return SubstInverseSpec{
        .target_expr = absl::StrFormat("(set %s (reg:SI 72))", dst),
        .from_reg = "(reg:SI 72)",
        .to_expr = std::string(rhs),
    };
  }
  return SubstInverseSpec{
      .target_expr = "(reg:SI 72)",
      .from_reg = "(reg:SI 72)",
      .to_expr = std::string(s),
  };
}

RtlControlFlowGraph ToySubfunctionInverses::InvertTryCombine(const RtlControlFlowGraph& x) const {
  RtlControlFlowGraph out = x;
  for (auto& blk : out.rpo_blocks) {
    std::vector<std::string> expanded;
    for (const std::string& pat : blk.instructions) {
      const std::string clean = StripTrailingRegNotesFromPattern(pat);
      const std::string notes = pat.size() > clean.size() ? pat.substr(clean.size()) : "";
      // Case 1: `(set <dst> (gt:SI <src> (const_int 0)))`
      if (absl::StartsWith(clean, "(set ") && absl::StrContains(clean, " (gt:SI ") &&
          absl::EndsWith(clean, " (const_int 0)))")) {
        const size_t gt_pos = clean.find(" (gt:SI ");
        const std::string dst = clean.substr(5, gt_pos - 5);
        const size_t src_start = gt_pos + std::string_view(" (gt:SI ").size();
        const size_t src_end = clean.size() - std::string_view(" (const_int 0)))").size();
        const std::string src = clean.substr(src_start, src_end - src_start);
        expanded.push_back(
            absl::StrFormat("(set (reg:SI 72) (lt:SI %s (const_int 1)))%s", src, notes));
        expanded.push_back(
            absl::StrFormat("(set %s (xor:SI (reg:SI 72) (const_int 1))) "
                            "(expr_list:REG_DEAD (reg:SI 72) (nil))",
                            dst));
        continue;
      }
      // Case 2: `(set <dst> (const:SI (plus:SI (symbol_ref:SI (<sym>)) (const_int <off>))))`
      if (absl::StartsWith(clean, "(set ") &&
          absl::StrContains(clean, " (const:SI (plus:SI (symbol_ref:SI ") &&
          absl::EndsWith(clean, "))))")) {
        const size_t const_pos = clean.find(" (const:SI ");
        const std::string dst = clean.substr(5, const_pos - 5);
        const std::string const_expr = clean.substr(const_pos + 1, clean.size() - const_pos - 2);
        expanded.push_back(absl::StrFormat("(set (reg:SI 72) (high:SI %s))", const_expr));
        expanded.push_back(
            absl::StrFormat("(set %s (lo_sum:SI (reg:SI 72) %s)) "
                            "(expr_list:REG_DEAD (reg:SI 72) (nil))%s",
                            dst, const_expr, notes));
        continue;
      }
      // Case 3: `(set (reg:SI <dst>) (plus:SI (reg:SI 4 a0) (reg:SI 5 a1)))`
      // combined from argument pseudos `(reg/v:SI 72)` and `(reg/v:SI 73)`.
      if (absl::StartsWith(clean, "(set (reg:SI ") &&
          absl::EndsWith(clean, " (plus:SI (reg:SI 4 a0) (reg:SI 5 a1)))") &&
          notes == " (expr_list:REG_DEAD (reg:SI 5 a1) (expr_list:REG_DEAD (reg:SI 4 a0) (nil)))") {
        const size_t dst_end = clean.find(')', 5);
        if (dst_end != std::string::npos) {
          const std::string dst = clean.substr(5, dst_end - 4);
          expanded.push_back(
              "(set (reg/v:SI 72) (reg:SI 4 a0)) (expr_list:REG_DEAD (reg:SI 4 a0) (nil))");
          expanded.push_back(
              "(set (reg/v:SI 73) (reg:SI 5 a1)) (expr_list:REG_DEAD (reg:SI 5 a1) (nil))");
          expanded.push_back(absl::StrFormat(
              "(set %s (plus:SI (reg/v:SI 72) (reg/v:SI 73))) "
              "(expr_list:REG_DEAD (reg/v:SI 72) (expr_list:REG_DEAD (reg/v:SI 73) (nil)))",
              dst));
          continue;
        }
      }
      expanded.push_back(pat);
    }
    blk.instructions = std::move(expanded);
    blk.SyncRtlInsnsFromInstructions();
  }
  return out;
}

// -----------------------------------------------------------------------------
// Stage 8: Flow & Life Analysis (`flow.c`)
// -----------------------------------------------------------------------------

RtlControlFlowGraph ToySubfunctionInverses::InvertFindBasicBlocks(
    const RtlControlFlowGraph& x) const {
  RtlControlFlowGraph out = x;
  if (out.rpo_blocks.empty()) {
    return out;
  }
  // Ensure the last block ends with an unconditional `(return)` so a `BARRIER`
  // is emitted before the unreachable block; `flow.c:find_basic_blocks` deletes
  // unreachable instructions and unused labels following a `BARRIER`.
  const auto& last_blk = out.rpo_blocks.back();
  if (last_blk.instructions.empty() ||
      StripTrailingRegNotesFromPattern(last_blk.instructions.back()) != "(return)") {
    return out;
  }
  RtlBasicBlock dead_blk;
  dead_blk.id = out.rpo_blocks.size();
  dead_blk.AddPatternOnly("(set (reg:SI 3 v1) (const_int 999))");
  out.rpo_blocks.push_back(std::move(dead_blk));
  return out;
}

RtlControlFlowGraph ToySubfunctionInverses::InvertPropagateBlock(
    const RtlControlFlowGraph& x) const {
  RtlControlFlowGraph out = x;
  if (out.rpo_blocks.empty()) {
    return out;
  }
  for (auto& blk : out.rpo_blocks) {
    for (std::string& pat : blk.instructions) {
      pat = StripTrailingRegNotesFromPattern(pat);
    }
  }
  const bool is_stupid_life_only =
      !out.sideband.reg_n_sets.empty() && out.sideband.basic_block_live_at_start.empty();
  // Prepend an immediately-overwritten dead store to the destination register
  // of the first `(set (reg ...) ...)` instruction when its RHS does not read
  // that register, so `propagate_block` deletes the dead store via `insn_dead_p`
  // and recomputes `REG_DEAD` notes.
  auto& first_blk = out.rpo_blocks[0];
  if (!is_stupid_life_only && !first_blk.instructions.empty() &&
      absl::StartsWith(first_blk.instructions[0], "(set (reg")) {
    const size_t close_paren = first_blk.instructions[0].find(')', 5);
    if (close_paren != std::string::npos) {
      const std::string dead_dest = first_blk.instructions[0].substr(5, close_paren - 4);
      const std::string rhs = first_blk.instructions[0].substr(close_paren + 1);
      if (!absl::StrContains(rhs, dead_dest)) {
        first_blk.instructions.insert(first_blk.instructions.begin(),
                                      absl::StrFormat("(set %s (const_int 0))", dead_dest));
      }
    }
  }
  for (auto& blk : out.rpo_blocks) {
    blk.SyncRtlInsnsFromInstructions();
  }
  return out;
}

// -----------------------------------------------------------------------------
// Stage 9 & Stage 11: Common Subexpression Elimination (`cse.c`)
// -----------------------------------------------------------------------------

std::string ToySubfunctionInverses::InvertFoldRtx(std::string_view folded_rtx_x) const {
  std::string_view s = absl::StripAsciiWhitespace(folded_rtx_x);
  if (absl::StartsWith(s, "(const_int ") && s.back() == ')') {
    std::string_view num_str =
        absl::StripAsciiWhitespace(s.substr(std::string_view("(const_int ").size(),
                                            s.size() - std::string_view("(const_int ").size() - 1));
    int64_t val = 0;
    if (absl::SimpleAtoi(num_str, &val)) {
      return absl::StrFormat("(plus:SI (const_int 10) (const_int %d))",
                             static_cast<int32_t>(val - 10));
    }
  }
  if (absl::StartsWith(s, "(set ") && s.back() == ')') {
    size_t pos = std::string_view("(set ").size();
    int depth = 0;
    for (; pos < s.size(); ++pos) {
      if (s[pos] == '(') ++depth;
      if (s[pos] == ')' && --depth == 0) {
        ++pos;
        break;
      }
    }
    std::string_view dst = absl::StripAsciiWhitespace(
        s.substr(std::string_view("(set ").size(), pos - std::string_view("(set ").size()));
    std::string_view rhs = absl::StripAsciiWhitespace(s.substr(pos, s.size() - pos - 1));
    if (absl::StartsWith(rhs, "(const_int ") && rhs.back() == ')') {
      std::string_view num_str = absl::StripAsciiWhitespace(
          rhs.substr(std::string_view("(const_int ").size(),
                     rhs.size() - std::string_view("(const_int ").size() - 1));
      int64_t val = 0;
      if (absl::SimpleAtoi(num_str, &val)) {
        return absl::StrFormat("(set %s (plus:SI (const_int 10) (const_int %d)))", dst,
                               static_cast<int32_t>(val - 10));
      }
    }
    return std::string(s);
  }
  return absl::StrFormat("(plus:SI %s (const_int 0))", s);
}

RtlControlFlowGraph ToySubfunctionInverses::InvertCseBasicBlock(
    const RtlControlFlowGraph& x) const {
  RtlControlFlowGraph out = x;
  for (auto& blk : out.rpo_blocks) {
    for (size_t i = 0; i + 1 < blk.instructions.size(); ++i) {
      const std::string cur = StripTrailingRegNotesFromPattern(blk.instructions[i]);
      const std::string next = StripTrailingRegNotesFromPattern(blk.instructions[i + 1]);
      if (!absl::StartsWith(cur, "(set (reg") || !absl::StartsWith(next, "(set ")) {
        continue;
      }
      const size_t cur_reg_end = cur.find(')', 5);
      if (cur_reg_end == std::string::npos || cur_reg_end + 2 >= cur.size()) {
        continue;
      }
      const std::string cse_reg = cur.substr(5, cur_reg_end - 4);
      const std::string cse_expr = cur.substr(cur_reg_end + 2, cur.size() - cur_reg_end - 3);
      if (absl::EndsWith(next, absl::StrCat(" ", cse_reg, ")"))) {
        const size_t dup_reg_end = next.find(')', 5);
        const std::string dup_reg =
            dup_reg_end != std::string::npos ? next.substr(5, dup_reg_end - 4) : "";
        if (absl::StartsWith(dup_reg, "(reg/i:")) {
          continue;
        }
        const std::string next_prefix = next.substr(0, next.size() - cse_reg.size() - 1);
        blk.instructions[i + 1] = absl::StrCat(next_prefix, cse_expr, ")");
        if (!dup_reg.empty() && i + 2 < blk.instructions.size()) {
          std::string& user_pat = blk.instructions[i + 2];
          const size_t second_pos = user_pat.rfind(cse_reg);
          const size_t first_pos = user_pat.find(cse_reg);
          if (second_pos != std::string::npos && second_pos != first_pos) {
            user_pat.replace(second_pos, cse_reg.size(), dup_reg);
          }
        }
        break;
      }
    }
    blk.SyncRtlInsnsFromInstructions();
  }
  return out;
}

// -----------------------------------------------------------------------------
// Stage 10: Loop Optimization (`loop.c`)
// -----------------------------------------------------------------------------

RtlControlFlowGraph ToySubfunctionInverses::InvertScanLoop(const RtlControlFlowGraph& x) const {
  RtlControlFlowGraph out = x;
  if (out.rpo_blocks.size() < 2) {
    return out;
  }
  auto& preheader = out.rpo_blocks[0];
  auto& loop_body = out.rpo_blocks[1];
  for (size_t i = 0; i < preheader.instructions.size(); ++i) {
    if (preheader.instructions[i] == "(note NOTE_INSN_LOOP_BEG)" && i > 0) {
      const std::string hoisted = preheader.instructions[i - 1];
      preheader.instructions.erase(preheader.instructions.begin() + static_cast<ptrdiff_t>(i - 1));
      loop_body.instructions.insert(loop_body.instructions.begin(), hoisted);
      preheader.SyncRtlInsnsFromInstructions();
      loop_body.SyncRtlInsnsFromInstructions();
      break;
    }
  }
  return out;
}

RtlControlFlowGraph ToySubfunctionInverses::InvertStrengthReduce(
    const RtlControlFlowGraph& x) const {
  RtlControlFlowGraph out = x;
  if (out.rpo_blocks.size() < 2) {
    return out;
  }
  auto& preheader = out.rpo_blocks[0];
  auto& loop_body = out.rpo_blocks[1];

  // Case A: `check_dbra_loop` countdown reversal inside `strength_reduce`:
  // Preheader has `(set <biv> (const_int <N-1>))` immediately before
  // `(note NOTE_INSN_LOOP_BEG)`, and loop body has
  // `(set <biv> (plus:SI <biv> (const_int -1)))` after `(note NOTE_INSN_LOOP_CONT)`
  // followed by `(if_then_else (ge:SI <biv> (const_int 0)) ...)`.
  for (size_t i = 0; i < preheader.instructions.size(); ++i) {
    if (preheader.instructions[i] == "(note NOTE_INSN_LOOP_BEG)" && i > 0) {
      const std::string& init_pat = preheader.instructions[i - 1];
      if (absl::StartsWith(init_pat, "(set (reg:SI ") &&
          absl::StrContains(init_pat, " (const_int ")) {
        const size_t biv_end = init_pat.find(')', 5);
        const size_t cint_pos = init_pat.find("(const_int ");
        const size_t cint_end = init_pat.find(')', cint_pos);
        int64_t start_minus_one = 0;
        if (biv_end != std::string::npos && cint_pos != std::string::npos &&
            cint_end != std::string::npos &&
            absl::SimpleAtoi(init_pat.substr(cint_pos + 11, cint_end - (cint_pos + 11)),
                             &start_minus_one) &&
            start_minus_one > 0) {
          const std::string biv_reg = init_pat.substr(5, biv_end - 4);
          preheader.instructions.erase(preheader.instructions.begin() +
                                       static_cast<ptrdiff_t>(i - 1));
          preheader.SyncRtlInsnsFromInstructions();

          const std::string dec_pat =
              absl::StrFormat("(set %s (plus:SI %s (const_int -1)))", biv_reg, biv_reg);
          const std::string inc_pat =
              absl::StrFormat("(set %s (plus:SI %s (const_int 1)))", biv_reg, biv_reg);
          std::vector<std::string> restored_body;
          bool saw_cont_note = false;
          for (const std::string& pat : loop_body.instructions) {
            if (pat == dec_pat) {
              restored_body.push_back(inc_pat);
              continue;
            }
            if (pat == "(note NOTE_INSN_LOOP_CONT)") {
              saw_cont_note = true;
              restored_body.push_back(pat);
              continue;
            }
            const std::string clean = StripTrailingRegNotesFromPattern(pat);
            const std::string ge_zero = absl::StrFormat("(ge:SI %s (const_int 0))", biv_reg);
            const std::string lt_bound = absl::StrFormat("(lt:SI %s (const_int %d))", biv_reg,
                                                         static_cast<int32_t>(start_minus_one + 1));
            if (absl::StrContains(clean, ge_zero)) {
              if (!saw_cont_note) {
                restored_body.push_back("(note NOTE_INSN_LOOP_CONT)");
              }
              std::string replaced = clean;
              const size_t pos = replaced.find(ge_zero);
              replaced.replace(pos, ge_zero.size(), lt_bound);
              restored_body.push_back(std::move(replaced));
              continue;
            }
            restored_body.push_back(pat);
          }
          loop_body.instructions = std::move(restored_body);
          loop_body.SyncRtlInsnsFromInstructions();
          if (out.rpo_blocks.size() >= 3) {
            RtlBasicBlock& exit_bb = out.rpo_blocks[2];
            const std::string final_val_pat = absl::StrFormat(
                "(set %s (const_int %d))", biv_reg, static_cast<int32_t>(start_minus_one + 1));
            for (auto it = exit_bb.instructions.begin(); it != exit_bb.instructions.end(); ++it) {
              if (*it == final_val_pat) {
                exit_bb.instructions.erase(it);
                exit_bb.SyncRtlInsnsFromInstructions();
                break;
              }
            }
          }
          return out;
        }
      }
      break;
    }
  }

  // Case B: GIV initialization `(set (reg:SI <giv>) (reg/v:SI <base>))`
  // immediately before `(note NOTE_INSN_LOOP_BEG)` in the preheader.
  std::string giv_reg;
  std::string base_reg;
  for (size_t i = 0; i < preheader.instructions.size(); ++i) {
    if (preheader.instructions[i] == "(note NOTE_INSN_LOOP_BEG)" && i > 0) {
      const std::string& init_pat = preheader.instructions[i - 1];
      if (absl::StartsWith(init_pat, "(set (reg:SI ")) {
        const size_t giv_end = init_pat.find(')', 5);
        if (giv_end != std::string::npos && giv_end + 2 < init_pat.size()) {
          giv_reg = init_pat.substr(5, giv_end - 4);
          base_reg = init_pat.substr(giv_end + 2, init_pat.size() - giv_end - 3);
          preheader.instructions.erase(preheader.instructions.begin() +
                                       static_cast<ptrdiff_t>(i - 1));
          preheader.SyncRtlInsnsFromInstructions();
        }
      }
      break;
    }
  }
  if (giv_reg.empty()) {
    return out;
  }

  // Locate the unreduced address register `(set <addr_reg> (plus:SI <scaled> <base_reg>))`
  // in `loop_body`, replace `<giv_reg>` in `(mem/s:SI <giv_reg>)` with `<addr_reg>`,
  // and remove the GIV step `(set <giv_reg> (plus:SI <giv_reg> (const_int ...)))`.
  std::string addr_reg;
  for (const std::string& pat : loop_body.instructions) {
    if (absl::StartsWith(pat, "(set (reg:SI ") &&
        absl::EndsWith(pat, absl::StrCat(" ", base_reg, "))"))) {
      const size_t end_pos = pat.find(')', 5);
      if (end_pos != std::string::npos) {
        addr_reg = pat.substr(5, end_pos - 4);
        break;
      }
    }
  }

  const std::string giv_step_prefix = absl::StrFormat("(set %s (plus:SI %s ", giv_reg, giv_reg);
  std::vector<std::string> restored_body;
  for (std::string pat : loop_body.instructions) {
    if (absl::StartsWith(pat, giv_step_prefix)) {
      continue;
    }
    if (!addr_reg.empty()) {
      const size_t pos = pat.find(giv_reg);
      if (pos != std::string::npos) {
        pat.replace(pos, giv_reg.size(), addr_reg);
      }
    }
    restored_body.push_back(std::move(pat));
  }
  loop_body.instructions = std::move(restored_body);
  loop_body.SyncRtlInsnsFromInstructions();
  return out;
}

// -----------------------------------------------------------------------------
// Stage 12: Initial Jump Optimization (`jump.c`)
// -----------------------------------------------------------------------------

RtlControlFlowGraph ToySubfunctionInverses::InvertJumpRedirect(const RtlControlFlowGraph& x) const {
  RtlControlFlowGraph out = x;
  if (out.rpo_blocks.size() < 3 || out.rpo_blocks[0].successors.size() < 2) {
    return out;
  }
  const size_t target_b = out.rpo_blocks[0].successors[0];
  const size_t tramp_b = out.rpo_blocks.size();
  out.rpo_blocks[0].successors[0] = tramp_b;

  RtlBasicBlock tramp;
  tramp.id = tramp_b;
  tramp.AddPatternOnly(absl::StrFormat("(set (pc) (label_ref L%zu))", target_b));
  tramp.successors = {target_b};
  out.rpo_blocks.push_back(std::move(tramp));
  return out;
}

RtlControlFlowGraph ToySubfunctionInverses::InvertJump1Optimize(
    const RtlControlFlowGraph& x) const {
  RtlControlFlowGraph out = x;
  if (out.rpo_blocks.empty() || out.rpo_blocks.back().instructions.empty()) {
    return out;
  }
  const size_t ret_lbl_b = out.rpo_blocks.size();
  auto& last_blk = out.rpo_blocks.back();
  last_blk.AddPatternOnly(absl::StrFormat("(set (pc) (label_ref L%zu))", ret_lbl_b));
  last_blk.successors = {ret_lbl_b};

  RtlBasicBlock ret_blk;
  ret_blk.id = ret_lbl_b;
  out.rpo_blocks.push_back(std::move(ret_blk));
  return out;
}

// -----------------------------------------------------------------------------
// Stage 13: Tree -> RTL Expansion (`function.c`, `stmt.c`, `expr.c`, `calls.c`)
// -----------------------------------------------------------------------------

namespace {

using CExprPtr = std::unique_ptr<CExpression>;

constexpr int kFirstUserPseudo = 72;

std::string ParamNameForRegno(int regno) {
  return absl::StrCat("p", regno - kFirstUserPseudo);
}

void ScanExprForStage13Regs(const RtlExpr& expr, int* max_v_pseudo, int* min_temp_pseudo,
                            absl::flat_hash_set<int>* deref_v_pseudos,
                            absl::flat_hash_map<int, RtlMachineMode>* v_pseudo_modes,
                            bool inside_mem = false) {
  if (expr.IsReg()) {
    int regno = expr.RegNo();
    if (regno >= kFirstUserPseudo) {
      if (expr.flags.volatil) {
        *max_v_pseudo = std::max(*max_v_pseudo, regno);
        if (expr.mode != RtlMachineMode::kNone) {
          (*v_pseudo_modes)[regno] = expr.mode;
        }
        if (inside_mem) {
          deref_v_pseudos->insert(regno);
        }
      } else {
        *min_temp_pseudo = std::min(*min_temp_pseudo, regno);
      }
    }
    return;
  }
  const bool next_inside_mem = inside_mem || expr.op == RtlOp::kMem;
  for (const RtlExpr& child : expr.children) {
    ScanExprForStage13Regs(child, max_v_pseudo, min_temp_pseudo, deref_v_pseudos, v_pseudo_modes,
                           next_inside_mem);
  }
}

FunctionDeclaration InferIsolatedStage13FuncDecl(const RtlControlFlowGraph& x,
                                                 CType return_type = CType::S32(),
                                                 int explicit_num_params = -1) {
  int max_v_pseudo = 0;
  int min_temp_pseudo = 1000000;
  absl::flat_hash_set<int> deref_v_pseudos;
  absl::flat_hash_map<int, RtlMachineMode> v_pseudo_modes;
  for (const auto& blk : x.rpo_blocks) {
    for (const std::string& pat_str : blk.instructions) {
      auto parsed = ParsePatternAndNotes(pat_str);
      if (parsed.ok()) {
        ScanExprForStage13Regs(parsed->pattern, &max_v_pseudo, &min_temp_pseudo, &deref_v_pseudos,
                               &v_pseudo_modes);
      }
    }
  }
  int num_params = 0;
  if (explicit_num_params >= 0) {
    num_params = explicit_num_params;
  } else {
    if (max_v_pseudo >= kFirstUserPseudo) {
      num_params = max_v_pseudo - kFirstUserPseudo + 1;
    }
    if (min_temp_pseudo < 1000000 && min_temp_pseudo >= kFirstUserPseudo) {
      num_params = std::max(num_params, min_temp_pseudo - kFirstUserPseudo);
    }
  }

  std::vector<CParameter> params;
  params.reserve(num_params);
  for (int i = 0; i < num_params; ++i) {
    const int regno = kFirstUserPseudo + i;
    CType param_type = CType::S32();
    if (deref_v_pseudos.contains(regno)) {
      param_type = CType::S32().MakePointer();
    } else if (auto it = v_pseudo_modes.find(regno); it != v_pseudo_modes.end()) {
      if (it->second == RtlMachineMode::kSF) {
        param_type = CType::F32();
      } else if (it->second == RtlMachineMode::kDF) {
        param_type = CType::F64();
      } else if (it->second == RtlMachineMode::kDI) {
        param_type = CType::Named("s64");
      }
    }
    params.push_back(CParameter{
        .type = std::move(param_type),
        .name = ParamNameForRegno(regno),
    });
  }
  return FunctionDeclaration(std::move(return_type), "harness_func", std::move(params),
                             std::make_unique<CompoundStatement>());
}

std::string NegateComparisonOp(std::string_view op) {
  if (op == "==") return "!=";
  if (op == "!=") return "==";
  if (op == "<") return ">=";
  if (op == ">=") return "<";
  if (op == "<=") return ">";
  if (op == ">") return "<=";
  return std::string(op);
}

CExprPtr RtlExprToCExpr(const RtlExpr& expr, const absl::flat_hash_map<int, CExprPtr>& reg_env) {
  if (expr.IsConstInt()) {
    return CExpression::Integer(expr.IntVal());
  }
  if (expr.IsReg()) {
    const int regno = expr.RegNo();
    if (auto it = reg_env.find(regno); it != reg_env.end() && it->second != nullptr) {
      return it->second->Clone();
    }
    if (regno >= kFirstUserPseudo) {
      return CExpression::Identifier(ParamNameForRegno(regno));
    }
    return CExpression::Integer(0);
  }
  if (expr.op == RtlOp::kSubreg && !expr.children.empty()) {
    return RtlExprToCExpr(expr.children[0], reg_env);
  }
  if (expr.op == RtlOp::kMem && !expr.children.empty()) {
    return CExpression::Unary("*", RtlExprToCExpr(expr.children[0], reg_env));
  }
  if (expr.op == RtlOp::kNeg && !expr.children.empty()) {
    return CExpression::Unary("-", RtlExprToCExpr(expr.children[0], reg_env));
  }
  if (expr.op == RtlOp::kNot && !expr.children.empty()) {
    return CExpression::Unary("~", RtlExprToCExpr(expr.children[0], reg_env));
  }
  if (expr.children.size() == 2) {
    CExprPtr lhs = RtlExprToCExpr(expr.children[0], reg_env);
    CExprPtr rhs = RtlExprToCExpr(expr.children[1], reg_env);
    if (expr.op == RtlOp::kPlus) {
      if (expr.children[1].IsConstInt() && expr.children[1].IntVal() < 0) {
        return CExpression::Binary("-", std::move(lhs),
                                   CExpression::Integer(-expr.children[1].IntVal()));
      }
      return CExpression::Binary("+", std::move(lhs), std::move(rhs));
    }
    if (expr.op == RtlOp::kMinus) {
      return CExpression::Binary("-", std::move(lhs), std::move(rhs));
    }
    if (expr.op == RtlOp::kMult) {
      return CExpression::Binary("*", std::move(lhs), std::move(rhs));
    }
    if (expr.op == RtlOp::kDiv || expr.op == RtlOp::kUdiv) {
      return CExpression::Binary("/", std::move(lhs), std::move(rhs));
    }
    if (expr.op == RtlOp::kMod || expr.op == RtlOp::kUmod) {
      return CExpression::Binary("%", std::move(lhs), std::move(rhs));
    }
    if (expr.op == RtlOp::kAnd) {
      return CExpression::Binary("&", std::move(lhs), std::move(rhs));
    }
    if (expr.op == RtlOp::kIor) {
      return CExpression::Binary("|", std::move(lhs), std::move(rhs));
    }
    if (expr.op == RtlOp::kXor) {
      if (expr.children[1].IsConstInt() && expr.children[1].IntVal() == 1 &&
          expr.children[0].IsReg()) {
        // Logical negation of a comparison temp `(xor (lt a b) 1)` -> `a >= b`.
        if (auto it = reg_env.find(expr.children[0].RegNo());
            it != reg_env.end() && it->second != nullptr &&
            it->second->Kind() == CExpressionKind::kBinaryExpression) {
          const auto& bin = static_cast<const BinaryExpression&>(*it->second);
          std::string neg_op = NegateComparisonOp(bin.Op());
          if (neg_op != bin.Op()) {
            return CExpression::Binary(neg_op, bin.Lhs().Clone(), bin.Rhs().Clone());
          }
        }
      }
      return CExpression::Binary("^", std::move(lhs), std::move(rhs));
    }
    if (expr.op == RtlOp::kAshift) {
      return CExpression::Binary("<<", std::move(lhs), std::move(rhs));
    }
    if (expr.op == RtlOp::kAshiftrt || expr.op == RtlOp::kLshiftrt) {
      return CExpression::Binary(">>", std::move(lhs), std::move(rhs));
    }
    if (expr.op == RtlOp::kLt) {
      return CExpression::Binary("<", std::move(lhs), std::move(rhs));
    }
    if (expr.op == RtlOp::kLtu) {
      if (expr.children[1].IsConstInt() && expr.children[1].IntVal() == 1 &&
          expr.children[0].IsReg()) {
        if (auto it = reg_env.find(expr.children[0].RegNo());
            it != reg_env.end() && it->second != nullptr &&
            it->second->Kind() == CExpressionKind::kBinaryExpression) {
          const auto& bin = static_cast<const BinaryExpression&>(*it->second);
          if (bin.Op() == "^") {
            return CExpression::Binary("==", bin.Lhs().Clone(), bin.Rhs().Clone());
          }
        }
        return CExpression::Binary("==", std::move(lhs), CExpression::Integer(0));
      }
      return CExpression::Binary("<", std::move(lhs), std::move(rhs));
    }
    if (expr.op == RtlOp::kLe) {
      return CExpression::Binary("<=", std::move(lhs), std::move(rhs));
    }
    if (expr.op == RtlOp::kLeu) {
      if (expr.children[1].IsConstInt() && expr.children[1].IntVal() == 0 &&
          expr.children[0].IsReg()) {
        if (auto it = reg_env.find(expr.children[0].RegNo());
            it != reg_env.end() && it->second != nullptr &&
            it->second->Kind() == CExpressionKind::kBinaryExpression) {
          const auto& bin = static_cast<const BinaryExpression&>(*it->second);
          if (bin.Op() == "^") {
            return CExpression::Binary("==", bin.Lhs().Clone(), bin.Rhs().Clone());
          }
        }
        return CExpression::Binary("==", std::move(lhs), CExpression::Integer(0));
      }
      return CExpression::Binary("<=", std::move(lhs), std::move(rhs));
    }
    if (expr.op == RtlOp::kGt) {
      return CExpression::Binary(">", std::move(lhs), std::move(rhs));
    }
    if (expr.op == RtlOp::kGtu) {
      if (expr.children[1].IsConstInt() && expr.children[1].IntVal() == 0) {
        return CExpression::Binary("!=", std::move(lhs), CExpression::Integer(0));
      }
      return CExpression::Binary(">", std::move(lhs), std::move(rhs));
    }
    if (expr.op == RtlOp::kGe || expr.op == RtlOp::kGeu) {
      return CExpression::Binary(">=", std::move(lhs), std::move(rhs));
    }
    if (expr.op == RtlOp::kEq) {
      return CExpression::Binary("==", std::move(lhs), std::move(rhs));
    }
    if (expr.op == RtlOp::kNe) {
      return CExpression::Binary("!=", std::move(lhs), std::move(rhs));
    }
  }
  return CExpression::Integer(0);
}

const RtlExpr* UnwrapSingleSet(const RtlExpr& pat) {
  if (pat.IsSet()) {
    return &pat;
  }
  if (pat.IsParallel() && !pat.children.empty() && pat.children[0].IsSet()) {
    return &pat.children[0];
  }
  return nullptr;
}

// Decodes a basic block's `do_jump` instructions into the C condition expression
// that branches to `if_false_label` when false (or when `invert_for_false_jump`
// is false, branches when true).
CExprPtr DecodeBlockJumpCondition(const RtlBasicBlock& blk, bool invert_for_false_jump = true) {
  absl::flat_hash_map<int, CExprPtr> reg_env;
  for (const std::string& pat_str : blk.instructions) {
    auto parsed = ParsePatternAndNotes(pat_str);
    if (!parsed.ok()) {
      continue;
    }
    const RtlExpr* set_expr = UnwrapSingleSet(parsed->pattern);
    if (set_expr == nullptr) {
      continue;
    }
    const RtlExpr& dst = set_expr->SetDest();
    const RtlExpr& src = set_expr->SetSrc();
    if (dst.IsReg()) {
      reg_env[dst.RegNo()] = RtlExprToCExpr(src, reg_env);
      continue;
    }
    if (dst.op == RtlOp::kPc && src.op == RtlOp::kIfThenElse && src.children.size() >= 2) {
      const RtlExpr& cmp = src.children[0];
      if (cmp.children.size() == 2) {
        // Check if `cmp` is `(eq (reg tmp) (const_int 0))` or `(ne (reg tmp) (const_int 0))`
        // where `tmp` holds a relational comparison like `(lt a b)`.
        if ((cmp.op == RtlOp::kEq || cmp.op == RtlOp::kNe) && cmp.children[0].IsReg() &&
            cmp.children[1].IsConstInt() && cmp.children[1].IntVal() == 0) {
          if (auto it = reg_env.find(cmp.children[0].RegNo());
              it != reg_env.end() && it->second != nullptr &&
              it->second->Kind() == CExpressionKind::kBinaryExpression) {
            const auto& bin = static_cast<const BinaryExpression&>(*it->second);
            bool negate = (cmp.op == RtlOp::kEq) != invert_for_false_jump;
            std::string op = negate ? NegateComparisonOp(bin.Op()) : bin.Op();
            return CExpression::Binary(op, bin.Lhs().Clone(), bin.Rhs().Clone());
          }
        }
        CExprPtr c_cmp = RtlExprToCExpr(cmp, reg_env);
        if (c_cmp != nullptr && c_cmp->Kind() == CExpressionKind::kBinaryExpression &&
            invert_for_false_jump) {
          const auto& bin = static_cast<const BinaryExpression&>(*c_cmp);
          return CExpression::Binary(NegateComparisonOp(bin.Op()), bin.Lhs().Clone(),
                                     bin.Rhs().Clone());
        }
        return c_cmp;
      }
    }
  }
  return CExpression::Integer(1);
}

std::vector<std::unique_ptr<CStatement>> DecodeBlockAssignmentStatements(const RtlBasicBlock& blk) {
  absl::flat_hash_map<int, CExprPtr> reg_env;
  std::vector<std::unique_ptr<CStatement>> stmts;
  for (const std::string& pat_str : blk.instructions) {
    auto parsed = ParsePatternAndNotes(pat_str);
    if (!parsed.ok()) {
      continue;
    }
    const RtlExpr* set_expr = UnwrapSingleSet(parsed->pattern);
    if (set_expr == nullptr) {
      continue;
    }
    const RtlExpr& dst = set_expr->SetDest();
    const RtlExpr& src = set_expr->SetSrc();
    if (dst.op == RtlOp::kPc) {
      continue;
    }
    CExprPtr rhs = RtlExprToCExpr(src, reg_env);
    if (dst.IsReg()) {
      const int regno = dst.RegNo();
      reg_env[regno] = rhs->Clone();
      if (dst.flags.volatil && regno >= kFirstUserPseudo) {
        CExprPtr assign = CExpression::Assignment(
            "=", CExpression::Identifier(ParamNameForRegno(regno)), std::move(rhs));
        stmts.push_back(CStatement::Expression(std::move(assign)));
      }
    } else if (dst.IsMem()) {
      CExprPtr lhs = CExpression::Unary("*", RtlExprToCExpr(dst.MemAddr(), reg_env));
      CExprPtr assign = CExpression::Assignment("=", std::move(lhs), std::move(rhs));
      stmts.push_back(CStatement::Expression(std::move(assign)));
    }
  }
  return stmts;
}

}  // namespace

RtlControlFlowGraph ToySubfunctionInverses::InvertInstantiateVirtualRegs(
    const RtlControlFlowGraph& x) const {
  RtlControlFlowGraph out = x;
  out.pass = RtlPassKind::kRtl;
  for (auto& blk : out.rpo_blocks) {
    for (std::string& pat : blk.instructions) {
      auto parsed = ParsePatternAndNotes(pat);
      if (parsed.ok()) {
        parsed->RewriteRegs([](RtlExpr& reg) {
          if (reg.RegNo() == 0) {
            // `arg_pointer_rtx` (`$0`) <- `virtual_incoming_args_rtx` (`68`).
            reg.SetRegNo(68);
          } else if (reg.RegNo() == 30) {
            // `frame_pointer_rtx` (`$fp`) <- `virtual_stack_vars_rtx` (`69`).
            reg.SetRegNo(69);
          } else if (reg.RegNo() == 29) {
            // `stack_pointer_rtx` (`$sp`) <- `virtual_outgoing_args_rtx` (`71`).
            reg.SetRegNo(71);
          }
        });
        pat = parsed->ToSexprString();
      }
    }
    blk.SyncRtlInsnsFromInstructions();
  }
  return out;
}

absl::StatusOr<ExpandFunctionEndInput> ToySubfunctionInverses::InvertExpandFunctionEnd(
    const RtlControlFlowGraph& x) const {
  if (x.rpo_blocks.empty()) {
    return absl::InvalidArgumentError("InvertExpandFunctionEnd: empty CFG");
  }
  ExpandFunctionEndInput input;
  input.body_cfg = x;
  input.body_cfg.pass = RtlPassKind::kRtl;
  CType ret_type = CType::Void();
  for (const auto& blk : x.rpo_blocks) {
    for (const std::string& pat : blk.instructions) {
      if (absl::StrContains(pat, "(use (reg/i:SI 2")) {
        ret_type = CType::S32();
      } else if (absl::StrContains(pat, "(use (reg/i:SF 32")) {
        ret_type = CType::F32();
      } else if (absl::StrContains(pat, "(use (reg/i:DI 2")) {
        ret_type = CType::Named("s64");
      }
    }
  }
  input.func_decl = InferIsolatedStage13FuncDecl(x, ret_type);

  // Strip the trailing `return_label` + `(use (reg/i:...))` block emitted by
  // `expand_function_end`, or strip trailing `(use (reg/i:...))` in a single-block
  // function.
  if (input.body_cfg.rpo_blocks.size() >= 2) {
    const auto& last_blk = input.body_cfg.rpo_blocks.back();
    if (last_blk.instructions.empty() ||
        (last_blk.instructions.size() == 1 &&
         absl::StartsWith(last_blk.instructions[0], "(use (reg/i:"))) {
      const size_t removed_id = input.body_cfg.rpo_blocks.size() - 1;
      input.body_cfg.rpo_blocks.pop_back();
      for (auto& blk : input.body_cfg.rpo_blocks) {
        blk.successors.erase(std::remove(blk.successors.begin(), blk.successors.end(), removed_id),
                             blk.successors.end());
      }
      return input;
    }
  }
  auto& last_blk = input.body_cfg.rpo_blocks.back();
  if (!last_blk.instructions.empty() &&
      absl::StartsWith(last_blk.instructions.back(), "(use (reg/i:")) {
    last_blk.instructions.pop_back();
    last_blk.SyncRtlInsnsFromInstructions();
  }
  return input;
}

absl::StatusOr<ExpandReturnInput> ToySubfunctionInverses::InvertExpandReturn(
    const RtlControlFlowGraph& x) const {
  if (x.rpo_blocks.empty() || x.rpo_blocks[0].instructions.empty()) {
    return absl::InvalidArgumentError("InvertExpandReturn: empty CFG");
  }
  ExpandReturnInput input;
  const auto& blk = x.rpo_blocks[0];
  bool has_ret_reg = false;
  absl::flat_hash_map<int, CExprPtr> reg_env;
  CExprPtr ret_expr = nullptr;

  for (const std::string& pat_str : blk.instructions) {
    auto parsed = ParsePatternAndNotes(pat_str);
    if (!parsed.ok()) {
      continue;
    }
    const RtlExpr* set_expr = UnwrapSingleSet(parsed->pattern);
    if (set_expr == nullptr) {
      continue;
    }
    const RtlExpr& dst = set_expr->SetDest();
    const RtlExpr& src = set_expr->SetSrc();
    if (dst.IsReg()) {
      CExprPtr rhs = RtlExprToCExpr(src, reg_env);
      if (dst.RegNo() == 2 && dst.flags.integrated) {
        has_ret_reg = true;
        ret_expr = std::move(rhs);
      } else {
        reg_env[dst.RegNo()] = std::move(rhs);
      }
    }
  }

  input.func_decl = InferIsolatedStage13FuncDecl(x, has_ret_reg ? CType::S32() : CType::Void());
  input.ret_stmt = ReturnStatement(has_ret_reg ? std::move(ret_expr) : nullptr);
  return input;
}

absl::StatusOr<ExpandExprInput> ToySubfunctionInverses::InvertExpandExpr(
    const RtlControlFlowGraph& x) const {
  if (x.rpo_blocks.empty() || x.rpo_blocks[0].instructions.empty()) {
    return absl::InvalidArgumentError("InvertExpandExpr: empty CFG");
  }
  ExpandExprInput input;
  input.func_decl = InferIsolatedStage13FuncDecl(x, CType::S32());
  absl::flat_hash_map<int, CExprPtr> reg_env;
  CExprPtr last_expr = nullptr;
  int first_dst_regno = 0;
  int last_dst_regno = 0;

  for (const std::string& pat_str : x.rpo_blocks[0].instructions) {
    auto parsed = ParsePatternAndNotes(pat_str);
    if (!parsed.ok()) {
      continue;
    }
    const RtlExpr* set_expr = UnwrapSingleSet(parsed->pattern);
    if (set_expr == nullptr) {
      continue;
    }
    const RtlExpr& dst = set_expr->SetDest();
    const RtlExpr& src = set_expr->SetSrc();
    CExprPtr rhs = RtlExprToCExpr(src, reg_env);
    last_expr = rhs->Clone();
    if (dst.IsReg()) {
      if (first_dst_regno == 0) {
        first_dst_regno = dst.RegNo();
      }
      last_dst_regno = dst.RegNo();
      reg_env[dst.RegNo()] = std::move(rhs);
    }
  }
  if (last_expr == nullptr) {
    return absl::InvalidArgumentError("InvertExpandExpr: could not decode expression");
  }
  input.expr = std::move(last_expr);
  const int expected_first_temp =
      kFirstUserPseudo + static_cast<int>(input.func_decl.Parameters().size());
  if (first_dst_regno == last_dst_regno && last_dst_regno > expected_first_temp) {
    input.target_pseudo_regno = last_dst_regno;
  } else {
    input.target_pseudo_regno = 0;
  }
  return input;
}

absl::StatusOr<ExpandAssignmentInput> ToySubfunctionInverses::InvertExpandAssignment(
    const RtlControlFlowGraph& x) const {
  if (x.rpo_blocks.empty() || x.rpo_blocks[0].instructions.empty()) {
    return absl::InvalidArgumentError("InvertExpandAssignment: empty CFG");
  }
  ExpandAssignmentInput input;
  input.func_decl = InferIsolatedStage13FuncDecl(x, CType::Void());
  absl::flat_hash_map<int, CExprPtr> reg_env;
  CExprPtr lhs_expr = nullptr;
  CExprPtr rhs_expr = nullptr;

  for (const std::string& pat_str : x.rpo_blocks[0].instructions) {
    auto parsed = ParsePatternAndNotes(pat_str);
    if (!parsed.ok()) {
      continue;
    }
    const RtlExpr* set_expr = UnwrapSingleSet(parsed->pattern);
    if (set_expr == nullptr) {
      continue;
    }
    const RtlExpr& dst = set_expr->SetDest();
    const RtlExpr& src = set_expr->SetSrc();
    CExprPtr rhs = RtlExprToCExpr(src, reg_env);
    if (dst.IsReg()) {
      reg_env[dst.RegNo()] = rhs->Clone();
      if (dst.flags.volatil && dst.RegNo() >= kFirstUserPseudo) {
        lhs_expr = CExpression::Identifier(ParamNameForRegno(dst.RegNo()));
        rhs_expr = std::move(rhs);
      }
    } else if (dst.IsMem()) {
      lhs_expr = CExpression::Unary("*", RtlExprToCExpr(dst.MemAddr(), reg_env));
      rhs_expr = std::move(rhs);
    }
  }
  if (lhs_expr == nullptr || rhs_expr == nullptr) {
    return absl::InvalidArgumentError("InvertExpandAssignment: could not decode assignment");
  }
  input.lhs = std::move(lhs_expr);
  input.rhs = std::move(rhs_expr);
  return input;
}

absl::StatusOr<DoStoreFlagInput> ToySubfunctionInverses::InvertDoStoreFlag(
    const RtlControlFlowGraph& x) const {
  if (x.rpo_blocks.empty() || x.rpo_blocks[0].instructions.empty()) {
    return absl::InvalidArgumentError("InvertDoStoreFlag: empty CFG");
  }
  DoStoreFlagInput input;
  input.func_decl = InferIsolatedStage13FuncDecl(x, CType::S32());
  absl::flat_hash_map<int, CExprPtr> reg_env;
  CExprPtr last_expr = nullptr;

  for (const std::string& pat_str : x.rpo_blocks[0].instructions) {
    auto parsed = ParsePatternAndNotes(pat_str);
    if (!parsed.ok()) {
      continue;
    }
    const RtlExpr* set_expr = UnwrapSingleSet(parsed->pattern);
    if (set_expr == nullptr) {
      continue;
    }
    const RtlExpr& dst = set_expr->SetDest();
    const RtlExpr& src = set_expr->SetSrc();
    CExprPtr rhs = RtlExprToCExpr(src, reg_env);
    last_expr = rhs->Clone();
    if (dst.IsReg()) {
      reg_env[dst.RegNo()] = std::move(rhs);
    }
  }
  if (last_expr == nullptr) {
    return absl::InvalidArgumentError("InvertDoStoreFlag: could not decode comparison");
  }
  input.comparison_expr = std::move(last_expr);
  input.target_pseudo_regno = 0;
  input.only_cheap = 0;
  return input;
}

absl::StatusOr<DoJumpInput> ToySubfunctionInverses::InvertDoJump(
    const RtlControlFlowGraph& x) const {
  if (x.rpo_blocks.empty() || x.rpo_blocks[0].instructions.empty()) {
    return absl::InvalidArgumentError("InvertDoJump: empty CFG");
  }
  DoJumpInput input;
  input.func_decl = InferIsolatedStage13FuncDecl(x, CType::Void());
  input.jump_if_true = false;
  input.jump_if_false = true;

  if (x.rpo_blocks.size() == 1) {
    input.cond_expr = DecodeBlockJumpCondition(x.rpo_blocks[0], /*invert_for_false_jump=*/true);
    return input;
  }

  // Short-circuit `&&` or `||` across blocks 0 and 1:
  // For `||` (`TRUTH_ORIF_EXPR`), GCC 2.7.2 sets `if_true_label = drop_through_label`,
  // emitting two positive branches (`invert = false`) in blocks 0 and 1 followed by
  // an unconditional jump to `if_false_label` in block 2 (`rpo_blocks.size() >= 3`).
  // For `&&` (`TRUTH_ANDIF_EXPR`), blocks 0 and 1 both jump to `if_false_label` when
  // false (`rpo_blocks.size() == 2`).
  const bool is_logical_or = (x.rpo_blocks.size() >= 3);
  if (is_logical_or) {
    CExprPtr lhs = DecodeBlockJumpCondition(x.rpo_blocks[0], /*invert_for_false_jump=*/false);
    CExprPtr rhs = DecodeBlockJumpCondition(x.rpo_blocks[1], /*invert_for_false_jump=*/false);
    input.cond_expr = CExpression::Binary("||", std::move(lhs), std::move(rhs));
  } else {
    CExprPtr lhs = DecodeBlockJumpCondition(x.rpo_blocks[0], /*invert_for_false_jump=*/true);
    CExprPtr rhs = DecodeBlockJumpCondition(x.rpo_blocks[1], /*invert_for_false_jump=*/true);
    input.cond_expr = CExpression::Binary("&&", std::move(lhs), std::move(rhs));
  }
  return input;
}

absl::StatusOr<ExpandCallInput> ToySubfunctionInverses::InvertExpandCall(
    const RtlControlFlowGraph& x) const {
  if (x.rpo_blocks.empty() || x.rpo_blocks[0].instructions.empty()) {
    return absl::InvalidArgumentError("InvertExpandCall: empty CFG");
  }
  ExpandCallInput input;
  input.caller_decl = InferIsolatedStage13FuncDecl(x, CType::S32());
  absl::flat_hash_map<int, CExprPtr> reg_env;
  std::vector<std::pair<int, CExprPtr>> arg_regs;
  std::string callee_name;
  bool call_sets_v0 = false;
  int copy_from_v0_regno = 0;

  for (const std::string& pat_str : x.rpo_blocks[0].instructions) {
    auto parsed = ParsePatternAndNotes(pat_str);
    if (!parsed.ok()) {
      continue;
    }
    const RtlExpr& pat = parsed->pattern;
    if (pat.IsParallel() && !pat.children.empty()) {
      const RtlExpr& first_elem = pat.children[0];
      const RtlExpr* call_node = nullptr;
      if (first_elem.IsSet() && first_elem.SetSrc().op == RtlOp::kCall) {
        call_sets_v0 = true;
        call_node = &first_elem.SetSrc();
      } else if (first_elem.op == RtlOp::kCall) {
        call_sets_v0 = false;
        call_node = &first_elem;
      }
      if (call_node != nullptr && !call_node->children.empty() && call_node->children[0].IsMem() &&
          call_node->children[0].MemAddr().IsSymbolRef()) {
        callee_name = std::string(call_node->children[0].MemAddr().SymbolName());
        if (callee_name.size() >= 2 && callee_name.front() == '"' && callee_name.back() == '"') {
          callee_name = callee_name.substr(1, callee_name.size() - 2);
        }
        if (!callee_name.empty() && callee_name.front() == '@') {
          callee_name.erase(0, 1);
        }
      }
      continue;
    }
    const RtlExpr* set_expr = UnwrapSingleSet(pat);
    if (set_expr == nullptr) {
      continue;
    }
    const RtlExpr& dst = set_expr->SetDest();
    const RtlExpr& src = set_expr->SetSrc();
    if (dst.IsReg()) {
      const int regno = dst.RegNo();
      if (src.IsReg() && src.RegNo() == 2 && regno >= kFirstUserPseudo) {
        copy_from_v0_regno = regno;
      } else if (regno >= 4 && regno <= 7) {
        arg_regs.emplace_back(regno, RtlExprToCExpr(src, reg_env));
      } else {
        reg_env[regno] = RtlExprToCExpr(src, reg_env);
      }
    }
  }
  if (callee_name.empty()) {
    return absl::InvalidArgumentError("InvertExpandCall: no symbol_ref call found");
  }
  std::sort(arg_regs.begin(), arg_regs.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  std::vector<CExprPtr> call_args;
  call_args.reserve(arg_regs.size());
  for (auto& [_, arg_expr] : arg_regs) {
    call_args.push_back(std::move(arg_expr));
  }
  input.call_expr = CallExpression(CExpression::Identifier(callee_name), std::move(call_args));
  input.return_type = call_sets_v0 ? CType::S32() : CType::Void();
  input.target_pseudo_regno = copy_from_v0_regno;
  input.ignore_result = !call_sets_v0;
  return input;
}

absl::StatusOr<ExpandCondInput> ToySubfunctionInverses::InvertExpandCond(
    const RtlControlFlowGraph& x) const {
  if (x.rpo_blocks.size() < 2) {
    return absl::InvalidArgumentError("InvertExpandCond: expected at least 2 basic blocks");
  }
  ExpandCondInput input;
  input.func_decl = InferIsolatedStage13FuncDecl(x, CType::Void());
  CExprPtr cond = DecodeBlockJumpCondition(x.rpo_blocks[0], /*invert_for_false_jump=*/true);
  auto then_block =
      std::make_unique<CompoundStatement>(DecodeBlockAssignmentStatements(x.rpo_blocks[1]));
  std::unique_ptr<CStatement> else_branch = nullptr;
  if (x.rpo_blocks.size() >= 3) {
    auto else_stmts = DecodeBlockAssignmentStatements(x.rpo_blocks[2]);
    if (!else_stmts.empty()) {
      else_branch = std::make_unique<CompoundStatement>(std::move(else_stmts));
    }
  }
  input.if_stmt = IfStatement(std::move(cond), std::move(then_block), std::move(else_branch));
  return input;
}

absl::StatusOr<ExpandLoopInput> ToySubfunctionInverses::InvertExpandLoop(
    const RtlControlFlowGraph& x) const {
  if (x.rpo_blocks.empty()) {
    return absl::InvalidArgumentError("InvertExpandLoop: empty CFG");
  }
  ExpandLoopInput input;
  input.func_decl = InferIsolatedStage13FuncDecl(x, CType::Void());

  CExprPtr cond = nullptr;
  std::vector<std::unique_ptr<CStatement>> body_stmts;
  for (const auto& blk : x.rpo_blocks) {
    bool has_cond_jump = false;
    for (const std::string& pat : blk.instructions) {
      if (absl::StrContains(pat, "(if_then_else")) {
        has_cond_jump = true;
        break;
      }
    }
    if (has_cond_jump && cond == nullptr) {
      bool jumps_to_exit = true;
      if (!blk.successors.empty()) {
        for (size_t succ : blk.successors) {
          if (succ < blk.id) {
            jumps_to_exit = false;
            break;
          }
        }
      }
      cond = DecodeBlockJumpCondition(blk, /*invert_for_false_jump=*/jumps_to_exit);
    }
    auto blk_stmts = DecodeBlockAssignmentStatements(blk);
    for (auto& s : blk_stmts) {
      body_stmts.push_back(std::move(s));
    }
  }
  if (cond == nullptr) {
    cond = CExpression::Integer(1);
  }
  input.loop_stmt = CStatement::While(std::move(cond),
                                      std::make_unique<CompoundStatement>(std::move(body_stmts)));
  return input;
}

absl::StatusOr<ExpandCaseInput> ToySubfunctionInverses::InvertExpandCase(
    const RtlControlFlowGraph& x) const {
  if (x.rpo_blocks.empty()) {
    return absl::InvalidArgumentError("InvertExpandCase: empty CFG");
  }
  ExpandCaseInput input;
  input.func_decl = InferIsolatedStage13FuncDecl(x, CType::Void());

  CExprPtr ctrl_expr = nullptr;
  std::vector<std::pair<int64_t, size_t>> case_targets;
  absl::flat_hash_set<size_t> dispatch_blocks;
  absl::flat_hash_set<size_t> case_target_set;

  for (size_t b = 0; b < x.rpo_blocks.size(); ++b) {
    const auto& blk = x.rpo_blocks[b];
    if (blk.instructions.empty()) {
      continue;
    }
    absl::flat_hash_map<int, CExprPtr> reg_env;
    for (size_t i = 0; i + 1 < blk.instructions.size(); ++i) {
      auto pre_parsed = ParsePatternAndNotes(blk.instructions[i]);
      if (!pre_parsed.ok()) {
        continue;
      }
      const RtlExpr* pre_set = UnwrapSingleSet(pre_parsed->pattern);
      if (pre_set != nullptr && pre_set->SetDest().IsReg()) {
        reg_env[pre_set->SetDest().RegNo()] = RtlExprToCExpr(pre_set->SetSrc(), reg_env);
      }
    }
    auto parsed = ParsePatternAndNotes(blk.instructions.back());
    if (!parsed.ok()) {
      continue;
    }
    const RtlExpr* set_expr = UnwrapSingleSet(parsed->pattern);
    if (set_expr != nullptr && set_expr->SetDest().op == RtlOp::kPc &&
        set_expr->SetSrc().op == RtlOp::kIfThenElse && !set_expr->SetSrc().children.empty()) {
      const RtlExpr& cmp = set_expr->SetSrc().children[0];
      if (cmp.op == RtlOp::kEq && cmp.children.size() == 2 && !blk.successors.empty()) {
        CExprPtr rhs_val = RtlExprToCExpr(cmp.children[1], reg_env);
        if (rhs_val != nullptr && rhs_val->Kind() == CExpressionKind::kIntegerLiteral) {
          const int64_t case_val = static_cast<const IntegerLiteralExpression&>(*rhs_val).Value();
          if (ctrl_expr == nullptr) {
            ctrl_expr = RtlExprToCExpr(cmp.children[0], reg_env);
          }
          size_t target_b = blk.successors[0];
          if (blk.successors.size() >= 2 && blk.successors[0] == b + 1) {
            target_b = blk.successors[1];
          }
          dispatch_blocks.insert(b);
          case_target_set.insert(target_b);
          case_targets.emplace_back(case_val, target_b);
        }
      }
    }
  }
  if (ctrl_expr == nullptr) {
    ctrl_expr = CExpression::Identifier(ParamNameForRegno(kFirstUserPseudo));
  }

  std::vector<SwitchCase> cases;
  for (const auto& [val, target_b] : case_targets) {
    if (target_b >= x.rpo_blocks.size()) {
      continue;
    }
    SwitchCase sc;
    sc.case_values = {val};
    sc.is_default = false;
    auto stmts = DecodeBlockAssignmentStatements(x.rpo_blocks[target_b]);
    stmts.push_back(CStatement::Break());
    sc.body = std::make_unique<CompoundStatement>(std::move(stmts));
    cases.push_back(std::move(sc));
  }

  if (!dispatch_blocks.empty()) {
    size_t max_dispatch_b = *std::max_element(dispatch_blocks.begin(), dispatch_blocks.end());
    size_t default_b = max_dispatch_b + 1;
    if (default_b < x.rpo_blocks.size() && x.rpo_blocks[default_b].instructions.size() == 1 &&
        !x.rpo_blocks[default_b].successors.empty() &&
        absl::StartsWith(x.rpo_blocks[default_b].instructions[0], "(set (pc) (label_ref")) {
      default_b = x.rpo_blocks[default_b].successors[0];
    }
    if (default_b < x.rpo_blocks.size() && !case_target_set.contains(default_b)) {
      auto def_stmts = DecodeBlockAssignmentStatements(x.rpo_blocks[default_b]);
      if (!def_stmts.empty()) {
        SwitchCase def_case;
        def_case.is_default = true;
        def_stmts.push_back(CStatement::Break());
        def_case.body = std::make_unique<CompoundStatement>(std::move(def_stmts));
        cases.push_back(std::move(def_case));
      }
    }
  }
  input.switch_stmt = SwitchStatement(std::move(ctrl_expr), std::move(cases));
  return input;
}

absl::StatusOr<ExpandDeclInput> ToySubfunctionInverses::InvertExpandDecl(
    const RtlControlFlowGraph& x) const {
  if (x.rpo_blocks.empty() || x.rpo_blocks[0].instructions.empty()) {
    return absl::InvalidArgumentError("InvertExpandDecl: empty CFG");
  }
  absl::flat_hash_map<int, CExprPtr> reg_env;
  int decl_regno = 0;
  CExprPtr init_expr = nullptr;

  for (const std::string& pat_str : x.rpo_blocks[0].instructions) {
    auto parsed = ParsePatternAndNotes(pat_str);
    if (!parsed.ok()) {
      continue;
    }
    const RtlExpr* set_expr = UnwrapSingleSet(parsed->pattern);
    if (set_expr == nullptr) {
      continue;
    }
    const RtlExpr& dst = set_expr->SetDest();
    const RtlExpr& src = set_expr->SetSrc();
    CExprPtr rhs = RtlExprToCExpr(src, reg_env);
    if (dst.IsReg()) {
      reg_env[dst.RegNo()] = rhs->Clone();
      if (dst.flags.volatil && dst.RegNo() >= kFirstUserPseudo) {
        decl_regno = dst.RegNo();
        init_expr = std::move(rhs);
      }
    }
  }
  if (decl_regno < kFirstUserPseudo || init_expr == nullptr) {
    return absl::InvalidArgumentError("InvertExpandDecl: could not decode local decl");
  }
  const int num_params = decl_regno - kFirstUserPseudo;
  ExpandDeclInput input;
  input.func_decl = InferIsolatedStage13FuncDecl(x, CType::Void(), num_params);
  input.var_decl = VariableDeclarationStatement(CType::S32(), "local_var", std::move(init_expr));
  return input;
}

absl::StatusOr<FunctionDeclaration> ToySubfunctionInverses::InvertExpandFunctionStart(
    const RtlControlFlowGraph& x) const {
  std::vector<CParameter> params;
  if (x.rpo_blocks.empty()) {
    return FunctionDeclaration(CType::S32(), "harness_func", std::move(params),
                               std::make_unique<CompoundStatement>());
  }

  const auto& blk = x.rpo_blocks[0];
  std::vector<std::pair<int, CType>> reg_params;
  for (size_t i = 0; i < blk.instructions.size(); ++i) {
    auto parsed = ParsePatternAndNotes(blk.instructions[i]);
    if (!parsed.ok()) {
      continue;
    }
    const RtlExpr* set_expr = UnwrapSingleSet(parsed->pattern);
    if (set_expr == nullptr) {
      continue;
    }
    const RtlExpr& dst = set_expr->SetDest();
    if (!dst.IsReg() || !dst.flags.volatil || dst.RegNo() < kFirstUserPseudo) {
      continue;
    }
    CType param_type = CType::S32();
    if (dst.mode == RtlMachineMode::kHI) {
      param_type = CType::S16();
    } else if (dst.mode == RtlMachineMode::kQI) {
      param_type = CType::U8();
    } else if (dst.mode == RtlMachineMode::kSF) {
      param_type = CType::F32();
    } else if (dst.mode == RtlMachineMode::kDF) {
      param_type = CType::F64();
    } else if (dst.mode == RtlMachineMode::kDI) {
      param_type = CType::Named("s64");
    }
    reg_params.emplace_back(dst.RegNo(), std::move(param_type));
  }
  std::sort(reg_params.begin(), reg_params.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  for (auto& [_, param_type] : reg_params) {
    params.push_back(CParameter{
        .type = std::move(param_type),
        .name = absl::StrCat("p", params.size()),
    });
  }
  return FunctionDeclaration(CType::S32(), "harness_func", std::move(params),
                             std::make_unique<CompoundStatement>());
}

}  // namespace rom_nom_nom::gcc_harness
