#include "gcc_harness/toy_subfunction_inverses.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
  std::vector<std::string> lines;
  size_t i = 0;
  while (i < words_x.size()) {
    const uint32_t w = words_x[i];
    if (IsBranchOrJumpWord(w) && i + 1 < words_x.size()) {
      const uint32_t delay_w = words_x[i + 1];
      if (delay_w == 0x00000000u) {
        // `append_insn` emitted a delay-slot `nop` because no prior instruction
        // could be swapped into the delay slot.
        lines.push_back(InvertGasMipsIp(w));
      } else {
        // `append_insn` swapped the preceding instruction into the delay slot;
        // restore the pre-`append_insn` order `[delay_insn, jump_insn]`.
        lines.push_back(InvertGasMipsIp(delay_w));
        lines.push_back(InvertGasMipsIp(w));
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

  if ((mnemonic == "addu" || mnemonic == "addiu") && args.size() == 3) {
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
  return "(const_int 0)";
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
      if (absl::StrContains(p0, "(mem:") && !absl::StrContains(p1, "(mem:")) {
        blk.instructions[i] = p1;
        blk.instructions[i + 1] = p0;
        break;
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

RtlControlFlowGraph ToySubfunctionInverses::InvertBlockAlloc(const RtlControlFlowGraph& x) const {
  RtlControlFlowGraph out = x;
  for (auto& blk : out.rpo_blocks) {
    for (std::string& pat : blk.instructions) {
      pat = StripTrailingRegNotesFromPattern(pat);
      constexpr std::string_view kClobberHi = "(clobber (reg:SI 64 hi))";
      constexpr std::string_view kClobberAccum = "(clobber (reg:SI 66 accum))";
      constexpr std::string_view kClobberHilo = "(clobber (reg:SI 66 hilo))";
      constexpr std::string_view kClobberScratch = "(clobber (scratch:SI))";
      size_t pos = 0;
      while ((pos = pat.find(kClobberHi, pos)) != std::string::npos) {
        pat.replace(pos, kClobberHi.size(), kClobberScratch);
        pos += kClobberScratch.size();
      }
      pos = 0;
      while ((pos = pat.find(kClobberAccum, pos)) != std::string::npos) {
        pat.replace(pos, kClobberAccum.size(), kClobberScratch);
        pos += kClobberScratch.size();
      }
      pos = 0;
      while ((pos = pat.find(kClobberHilo, pos)) != std::string::npos) {
        pat.replace(pos, kClobberHilo.size(), kClobberScratch);
        pos += kClobberScratch.size();
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
        break;
      }
    }
    for (std::string& pat : blk.instructions) {
      pat = StripTrailingRegNotesFromPattern(pat);
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
  if (s == "(set (reg:SI 2 v0) (gt:SI (reg:SI 4 a0) (const_int 0)))" ||
      s == "(set (reg:SI 2 v0) (ge:SI (reg:SI 4 a0) (const_int 1)))") {
    return SubstInverseSpec{
        .target_expr = "(set (reg:SI 2 v0) (xor:SI (reg:SI 72) (const_int 1)))",
        .from_reg = "(reg:SI 72)",
        .to_expr = "(lt:SI (reg:SI 4 a0) (const_int 1))",
    };
  }
  // General decomposition: replace the RHS of `(set <dst> <rhs>)` with `(reg:SI 72)`.
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
  // Prepend an immediately-overwritten dead store to `(reg:SI 72)` (or the
  // destination register of the first instruction) so `propagate_block`
  // exercises `insn_dead_p` deletion and `REG_DEAD` note attachment.
  auto& first_blk = out.rpo_blocks[0];
  std::string dead_dest = "(reg:SI 72)";
  if (!first_blk.instructions.empty() && absl::StartsWith(first_blk.instructions[0], "(set (reg")) {
    const size_t close_paren = first_blk.instructions[0].find(')', 5);
    if (close_paren != std::string::npos) {
      dead_dest = first_blk.instructions[0].substr(5, close_paren - 4);
    }
  }
  first_blk.instructions.insert(first_blk.instructions.begin(),
                                absl::StrFormat("(set %s (const_int 0))", dead_dest));
  first_blk.SyncRtlInsnsFromInstructions();
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
  if (out.rpo_blocks.size() < 2 || out.rpo_blocks[0].successors.empty()) {
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
  if (out.rpo_blocks.empty()) {
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

}  // namespace rom_nom_nom::gcc_harness
