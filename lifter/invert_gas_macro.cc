#include "lifter/invert_gas_macro.h"

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
#include "absl/types/span.h"
#include "lifter/invert_gas_append_insn.h"
#include "lifter/invert_gas_mips_ip.h"

namespace rom_nom_nom::lifter {
namespace {

uint32_t Opcode(uint32_t w) {
  return (w >> 26) & 0x3fu;
}
uint32_t Rs(uint32_t w) {
  return (w >> 21) & 0x1fu;
}
uint32_t Rt(uint32_t w) {
  return (w >> 16) & 0x1fu;
}
uint32_t Rd(uint32_t w) {
  return (w >> 11) & 0x1fu;
}
uint32_t Sa(uint32_t w) {
  return (w >> 6) & 0x1fu;
}
uint32_t Funct(uint32_t w) {
  return w & 0x3fu;
}
uint32_t Uimm16(uint32_t w) {
  return w & 0xffffu;
}
int16_t Simm16(uint32_t w) {
  return static_cast<int16_t>(w & 0xffffu);
}

// Attempts to match an R-type instruction `<op> $rd, $rs, $1` (or `<op> $rd,
// $1, $rs` for `M_SGT_I` / `M_SGTU_I`) that consumes a 32-bit immediate
// `imm32` loaded into `$at` (`$1`) by `tc-mips.c:load_register`.
std::string InvertImmediateAluTail(uint32_t rtype_word, uint32_t imm32) {
  if (Opcode(rtype_word) != 0x00u || Sa(rtype_word) != 0u) {
    return "";
  }
  const uint32_t rs = Rs(rtype_word);
  const uint32_t rt = Rt(rtype_word);
  const uint32_t rd = Rd(rtype_word);
  const uint32_t funct = Funct(rtype_word);

  if (rt == 1u) {
    std::string_view mnem;
    switch (funct) {
      case 0x20u:
        mnem = "add";
        break;
      case 0x21u:
        mnem = "addu";
        break;
      case 0x22u:
        mnem = "sub";
        break;
      case 0x23u:
        mnem = "subu";
        break;
      case 0x24u:
        mnem = "and";
        break;
      case 0x25u:
        mnem = "or";
        break;
      case 0x26u:
        mnem = "xor";
        break;
      case 0x27u:
        mnem = "nor";
        break;
      case 0x2au:
        mnem = "slt";
        break;
      case 0x2bu:
        mnem = "sltu";
        break;
      case 0x2cu:
        mnem = "dadd";
        break;
      case 0x2du:
        mnem = "daddu";
        break;
      case 0x2eu:
        mnem = "dsub";
        break;
      case 0x2fu:
        mnem = "dsubu";
        break;
      default:
        break;
    }
    if (!mnem.empty()) {
      return absl::StrFormat("%s $%u,$%u,0x%08x", mnem, rd, rs, imm32);
    }
  }

  if (rs == 1u && rt != 1u) {
    if (funct == 0x2au) {
      return absl::StrFormat("sgt $%u,$%u,0x%08x", rd, rt, imm32);
    }
    if (funct == 0x2bu) {
      return absl::StrFormat("sgtu $%u,$%u,0x%08x", rd, rt, imm32);
    }
  }
  return "";
}

// Attempts to match the 2-word tail of `M_SEQ_I`, `M_SNE_I`, `M_SGE_I`,
// `M_SGEU_I`, `M_SLE_I`, or `M_SLEU_I` after `load_register` has loaded a
// 32-bit immediate `imm32` (`>= 0x10000`) into `$at` (`$1`).
std::string InvertSetImmediateTail(uint32_t w_mid, uint32_t w_last, uint32_t imm32) {
  if (Opcode(w_mid) != 0x00u || Sa(w_mid) != 0u) {
    return "";
  }
  const uint32_t rs_mid = Rs(w_mid);
  const uint32_t rt_mid = Rt(w_mid);
  const uint32_t rd_mid = Rd(w_mid);
  const uint32_t funct_mid = Funct(w_mid);

  // `M_SEQ_I` / `M_SNE_I` with 32-bit immediate:
  //   xor $d, $s, $1
  //   sltiu $d, $d, 1  (or `sltu $d, $0, $d`)
  if (funct_mid == 0x26u && rt_mid == 1u && rs_mid != 0u) {
    if (Opcode(w_last) == 0x0bu && Rs(w_last) == rd_mid && Rt(w_last) == rd_mid &&
        Uimm16(w_last) == 1u) {
      return absl::StrFormat("seq $%u,$%u,0x%08x", rd_mid, rs_mid, imm32);
    }
    if (Opcode(w_last) == 0x00u && Rs(w_last) == 0u && Rt(w_last) == rd_mid &&
        Rd(w_last) == rd_mid && Sa(w_last) == 0u && Funct(w_last) == 0x2bu) {
      return absl::StrFormat("sne $%u,$%u,0x%08x", rd_mid, rs_mid, imm32);
    }
  }

  // `M_SGE_I` / `M_SGEU_I` / `M_SLE_I` / `M_SLEU_I` with 32-bit immediate:
  //   slt/sltu $d, $s, $1 (or $d, $1, $s)
  //   xori $d, $d, 1
  if ((funct_mid == 0x2au || funct_mid == 0x2bu) && Opcode(w_last) == 0x0eu &&
      Rs(w_last) == rd_mid && Rt(w_last) == rd_mid && Uimm16(w_last) == 1u) {
    if (rt_mid == 1u) {
      return absl::StrFormat("%s $%u,$%u,0x%08x", funct_mid == 0x2bu ? "sgeu" : "sge", rd_mid,
                             rs_mid, imm32);
    }
    if (rs_mid == 1u && rt_mid != 1u) {
      return absl::StrFormat("%s $%u,$%u,0x%08x", funct_mid == 0x2bu ? "sleu" : "sle", rd_mid,
                             rt_mid, imm32);
    }
  }
  return "";
}

std::string InvertOneWord(uint32_t w0) {
  const uint32_t op0 = Opcode(w0);
  const uint32_t rs0 = Rs(w0);
  const uint32_t rt0 = Rt(w0);
  const uint32_t uimm0 = Uimm16(w0);
  const int16_t simm0 = Simm16(w0);

  // `M_LI` 1-word expansions (`load_register` in `tc-mips.c:2141-2177`):
  // 1) `addiu $rt, $0, simm16` -> `li $rt, simm16`
  if (op0 == 0x09u && rs0 == 0u) {
    return absl::StrFormat("li $%u,%d", rt0, simm0);
  }
  // 2) `ori $rt, $0, uimm16` (for `0x8000 <= uimm16 <= 0xffff`) -> `li $rt, 0x<uimm16>`
  if (op0 == 0x0du && rs0 == 0u && uimm0 >= 0x8000u) {
    return absl::StrFormat("li $%u,0x%x", rt0, uimm0);
  }
  // 3) `lui $rt, uimm16` (for `uimm16 != 0`) -> `li $rt, 0x<uimm16>0000`
  if (op0 == 0x0fu && rs0 == 0u && uimm0 != 0u) {
    return absl::StrFormat("li $%u,0x%08x", rt0, uimm0 << 16);
  }

  return InvertGasMipsIp(w0);
}

std::string InvertTwoWords(uint32_t w0, uint32_t w1) {
  const uint32_t op0 = Opcode(w0);
  const uint32_t rs0 = Rs(w0);
  const uint32_t rt0 = Rt(w0);
  const uint32_t rd0 = Rd(w0);
  const uint32_t sa0 = Sa(w0);
  const uint32_t funct0 = Funct(w0);
  const uint32_t uimm0 = Uimm16(w0);
  const int16_t simm0 = Simm16(w0);

  const uint32_t op1 = Opcode(w1);
  const uint32_t rs1 = Rs(w1);
  const uint32_t rt1 = Rt(w1);
  const uint32_t rd1 = Rd(w1);
  const uint32_t sa1 = Sa(w1);
  const uint32_t funct1 = Funct(w1);
  const uint32_t uimm1 = Uimm16(w1);
  const int16_t simm1 = Simm16(w1);

  // 1. `M_LI` 32-bit immediate (`lui $rt, hi16` + `ori $rt, $rt, lo16` with
  // `hi16 != 0 && lo16 != 0`).
  if (op0 == 0x0fu && rs0 == 0u && uimm0 != 0u && op1 == 0x0du && rs1 == rt0 && rt1 == rt0 &&
      uimm1 != 0u) {
    return absl::StrFormat("li $%u,0x%08x", rt0, (uimm0 << 16) | uimm1);
  }

  // 2. 32-bit immediate ALU with `lo16 == 0` (`lui $1, hi16` + `<op> $rd, $rs, $1`).
  if (op0 == 0x0fu && rs0 == 0u && rt0 == 1u && uimm0 != 0u) {
    if (std::string alu = InvertImmediateAluTail(w1, uimm0 << 16); !alu.empty()) {
      return alu;
    }
  }

  // 3. `M_NOR_I` with 16-bit unsigned immediate (`ori $rt, $rs, uimm16` + `nor $rt, $rt, $0`).
  if (op0 == 0x0du && op1 == 0x00u && sa1 == 0u && funct1 == 0x27u && rd1 == rt0 && rs1 == rt0 &&
      rt1 == 0u) {
    return absl::StrFormat("nor $%u,$%u,0x%x", rt0, rs0, uimm0);
  }

  // 4. `M_MUL` / `M_DMUL` (`multu`/`dmultu` `$rs, $rt` + `mflo $rd`).
  if (op0 == 0x00u && rd0 == 0u && sa0 == 0u && (funct0 == 0x19u || funct0 == 0x1du) &&
      op1 == 0x00u && rs1 == 0u && rt1 == 0u && sa1 == 0u && funct1 == 0x12u) {
    return absl::StrFormat("%s $%u,$%u,$%u", funct0 == 0x1du ? "dmul" : "mul", rd1, rs0, rt0);
  }

  // 5. `M_SEQ` / `M_SNE` / `M_SEQ_I` / `M_SNE_I` (2-word).
  bool is_seq_tail = (op1 == 0x0bu && rs1 == rt1 && uimm1 == 1u);
  bool is_sne_tail = (op1 == 0x00u && rs1 == 0u && rt1 == rd1 && sa1 == 0u && funct1 == 0x2bu);
  if (is_seq_tail || is_sne_tail) {
    const uint32_t dreg = is_seq_tail ? rt1 : rd1;
    const std::string_view mnem = is_seq_tail ? "seq" : "sne";
    // Register-register `seq`/`sne $d, $s, $t`: `xor $d, $s, $t` (`$s != 0 && $t != 0`).
    if (op0 == 0x00u && sa0 == 0u && funct0 == 0x26u && rd0 == dreg && rs0 != 0u && rt0 != 0u) {
      return absl::StrFormat("%s $%u,$%u,$%u", mnem, dreg, rs0, rt0);
    }
    // 16-bit unsigned immediate `seq`/`sne $d, $s, uimm16` (`0 < uimm16 < 0x10000`):
    // `xori $d, $s, uimm16` (`$s != 0`).
    if (op0 == 0x0eu && rt0 == dreg && rs0 != 0u && uimm0 != 0u) {
      return absl::StrFormat("%s $%u,$%u,0x%x", mnem, dreg, rs0, uimm0);
    }
    // 16-bit negative immediate `seq`/`sne $d, $s, -simm0` (`0 < simm0 < 0x8000`):
    // `addiu $d, $s, simm0` (`$s != 0`).
    if (op0 == 0x09u && rt0 == dreg && rs0 != 0u && simm0 > 0) {
      return absl::StrFormat("%s $%u,$%u,%d", mnem, dreg, rs0, -static_cast<int32_t>(simm0));
    }
  }

  // 6. `M_SGE` / `M_SGEU` / `M_SGE_I` / `M_SGEU_I` (2-word).
  if (op1 == 0x0eu && rs1 == rt1 && uimm1 == 1u) {
    const uint32_t dreg = rt1;
    // Register-register `sge`/`sgeu $d, $s, $t`: `slt`/`sltu $d, $s, $t` + `xori $d, $d, 1`.
    if (op0 == 0x00u && sa0 == 0u && (funct0 == 0x2au || funct0 == 0x2bu) && rd0 == dreg) {
      return absl::StrFormat("%s $%u,$%u,$%u", funct0 == 0x2bu ? "sgeu" : "sge", dreg, rs0, rt0);
    }
    // 16-bit immediate `sge`/`sgeu $d, $s, 1`: `slti`/`sltiu $d, $s, 1` + `xori $d, $d, 1`.
    if ((op0 == 0x0au || op0 == 0x0bu) && rt0 == dreg && uimm0 == 1u) {
      return absl::StrFormat("%s $%u,$%u,1", op0 == 0x0bu ? "sgeu" : "sge", dreg, rs0);
    }
  }

  // 7. Big-endian unaligned load/store (`M_ULW`, `M_ULD`, `M_USW`, `M_USD`).
  if (rt1 == rt0 && rs1 == rs0) {
    if (op0 == 0x22u && op1 == 0x26u && simm0 <= 0x7fff - 3 && simm1 == simm0 + 3) {
      return absl::StrFormat("ulw $%u,%d($%u)", rt0, simm0, rs0);
    }
    if (op0 == 0x1au && op1 == 0x1bu && simm0 <= 0x7fff - 7 && simm1 == simm0 + 7) {
      return absl::StrFormat("uld $%u,%d($%u)", rt0, simm0, rs0);
    }
    if (op0 == 0x2au && op1 == 0x2eu && simm0 <= 0x7fff - 3 && simm1 == simm0 + 3) {
      return absl::StrFormat("usw $%u,%d($%u)", rt0, simm0, rs0);
    }
    if (op0 == 0x2cu && op1 == 0x2du && simm0 <= 0x7fff - 7 && simm1 == simm0 + 7) {
      return absl::StrFormat("usd $%u,%d($%u)", rt0, simm0, rs0);
    }
  }

  return "";
}

std::string InvertThreeWords(uint32_t w0, uint32_t w1, uint32_t w2) {
  const uint32_t op0 = Opcode(w0);
  const uint32_t rs0 = Rs(w0);
  const uint32_t rt0 = Rt(w0);
  const uint32_t rd0 = Rd(w0);
  const uint32_t sa0 = Sa(w0);
  const uint32_t funct0 = Funct(w0);
  const uint32_t uimm0 = Uimm16(w0);
  const int16_t simm0 = Simm16(w0);

  const uint32_t op1 = Opcode(w1);
  const uint32_t rs1 = Rs(w1);
  const uint32_t rt1 = Rt(w1);
  const uint32_t rd1 = Rd(w1);
  const uint32_t sa1 = Sa(w1);
  const uint32_t funct1 = Funct(w1);
  const uint32_t uimm1 = Uimm16(w1);

  const uint32_t op2 = Opcode(w2);
  const uint32_t rs2 = Rs(w2);
  const uint32_t rt2 = Rt(w2);
  const uint32_t rd2 = Rd(w2);
  const uint32_t sa2 = Sa(w2);
  const uint32_t funct2 = Funct(w2);
  const int16_t simm2 = Simm16(w2);

  // 1. `M_ABS` / `M_DABS` (`tc-mips.c:2421-2444`):
  //   bgez $sreg, .+8  (offset field = 2)
  //   nop              (if dreg == sreg)  OR  addu $dreg, $sreg, $0 (if dreg != sreg)
  //   sub/dsub $dreg, $0, $sreg
  if ((w0 & 0xfc1fffffu) == 0x04010002u && op2 == 0x00u && rs2 == 0u && rt2 == rs0 && sa2 == 0u &&
      (funct2 == 0x22u || funct2 == 0x2eu)) {
    const uint32_t dreg = rd2;
    const uint32_t sreg = rs0;
    const uint32_t expected_w1 =
        (dreg == sreg) ? 0x00000000u : (0x00000021u | (sreg << 21) | (dreg << 11));
    if (w1 == expected_w1) {
      return absl::StrFormat("%s $%u,$%u", funct2 == 0x2eu ? "dabs" : "abs", dreg, sreg);
    }
  }

  // 2. 32-bit immediate ALU with `lo16 != 0`:
  //   lui $1, hi16
  //   ori $1, $1, lo16
  //   <op> $rd, $rs, $1
  if (op0 == 0x0fu && rs0 == 0u && rt0 == 1u && uimm0 != 0u && op1 == 0x0du && rs1 == 1u &&
      rt1 == 1u && uimm1 != 0u) {
    const uint32_t imm32 = (uimm0 << 16) | uimm1;
    if (std::string alu = InvertImmediateAluTail(w2, imm32); !alu.empty()) {
      return alu;
    }
  }

  // 3. 3-word `M_SEQ_I` / `M_SNE_I` / `M_SGE_I` / `M_SGEU_I` / `M_SLE_I` / `M_SLEU_I`
  // with `lo16 == 0` (`lui $1, hi16` + tail).
  if (op0 == 0x0fu && rs0 == 0u && rt0 == 1u && uimm0 != 0u) {
    if (std::string set_imm = InvertSetImmediateTail(w1, w2, uimm0 << 16); !set_imm.empty()) {
      return set_imm;
    }
  }
  // Also `M_SGE_I` / `M_SGEU_I` / `M_SLE_I` / `M_SLEU_I` with `0x8000 <= uimm16 < 0x10000`
  // (`ori $1, $0, uimm16` + `slt`/`sltu` + `xori $d, $d, 1`).
  if (op0 == 0x0du && rs0 == 0u && rt0 == 1u && uimm0 >= 0x8000u && op1 == 0x00u && sa1 == 0u &&
      (funct1 == 0x2au || funct1 == 0x2bu) && op2 == 0x0eu && rs2 == rd1 && rt2 == rd1 &&
      Uimm16(w2) == 1u) {
    if (rt1 == 1u) {
      return absl::StrFormat("%s $%u,$%u,0x%x", funct1 == 0x2bu ? "sgeu" : "sge", rd1, rs1, uimm0);
    }
    if (rs1 == 1u && rt1 != 1u) {
      return absl::StrFormat("%s $%u,$%u,0x%x", funct1 == 0x2bu ? "sleu" : "sle", rd1, rt1, uimm0);
    }
  }
  // Also `M_SLE_I` / `M_SLEU_I` with 16-bit signed immediate (`addiu $1, $0, simm16` +
  // `slt`/`sltu $d, $1, $s` + `xori $d, $d, 1`).
  if (op0 == 0x09u && rs0 == 0u && rt0 == 1u && op1 == 0x00u && sa1 == 0u &&
      (funct1 == 0x2au || funct1 == 0x2bu) && rs1 == 1u && rt1 != 1u && op2 == 0x0eu &&
      rs2 == rd1 && rt2 == rd1 && Uimm16(w2) == 1u) {
    return absl::StrFormat("%s $%u,$%u,%d", funct1 == 0x2bu ? "sleu" : "sle", rd1, rt1, simm0);
  }

  // 4. `M_ROL_I` / `M_ROR_I` (`tc-mips.c:4241-4263`):
  //   sll/srl $1, $sreg, shamt
  //   srl/sll $dreg, $sreg, ((32 - shamt) & 0x1f)
  //   or $dreg, $dreg, $1
  if (op0 == 0x00u && rs0 == 0u && rd0 == 1u && sa0 != 0u && op1 == 0x00u && rs1 == 0u &&
      rt1 == rt0 && sa1 == ((32u - sa0) & 0x1fu) && op2 == 0x00u && rs2 == rd1 && rt2 == 1u &&
      rd2 == rd1 && sa2 == 0u && funct2 == 0x25u) {
    if (funct0 == 0x00u && funct1 == 0x02u) {
      return absl::StrFormat("rol $%u,$%u,%u", rd1, rt0, sa0);
    }
    if (funct0 == 0x02u && funct1 == 0x00u) {
      return absl::StrFormat("ror $%u,$%u,%u", rd1, rt0, sa0);
    }
  }

  // 5. `M_USH` (`tc-mips.c:4680-4694`, big-endian):
  //   sb $treg, (off+1)($breg)
  //   srl $1, $treg, 8
  //   sb $1, off($breg)
  if (op0 == 0x28u && op1 == 0x00u && rs1 == 0u && rt1 == rt0 && rd1 == 1u && sa1 == 8u &&
      funct1 == 0x02u && op2 == 0x28u && rt2 == 1u && rs2 == rs0 && simm2 < 0x7fff &&
      simm0 == simm2 + 1) {
    return absl::StrFormat("ush $%u,%d($%u)", rt0, simm2, rs0);
  }

  // 6. `M_MUL_I` / `M_DMUL_I` with 1-word `$1` load (`addiu`/`ori`/`lui` + `mult`/`dmult` +
  // `mflo`).
  if (op1 == 0x00u && rt1 == 1u && rd1 == 0u && sa1 == 0u && (funct1 == 0x18u || funct1 == 0x1cu) &&
      op2 == 0x00u && rs2 == 0u && rt2 == 0u && sa2 == 0u && funct2 == 0x12u) {
    const std::string_view mnem = (funct1 == 0x1cu) ? "dmul" : "mul";
    if (op0 == 0x09u && rs0 == 0u && rt0 == 1u) {
      return absl::StrFormat("%s $%u,$%u,%d", mnem, rd2, rs1, simm0);
    }
    if (op0 == 0x0du && rs0 == 0u && rt0 == 1u && uimm0 >= 0x8000u) {
      return absl::StrFormat("%s $%u,$%u,0x%x", mnem, rd2, rs1, uimm0);
    }
    if (op0 == 0x0fu && rs0 == 0u && rt0 == 1u && uimm0 != 0u) {
      return absl::StrFormat("%s $%u,$%u,0x%08x", mnem, rd2, rs1, uimm0 << 16);
    }
  }

  return "";
}

std::string InvertFourWords(uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3) {
  const uint32_t op0 = Opcode(w0);
  const uint32_t rs0 = Rs(w0);
  const uint32_t rt0 = Rt(w0);
  const uint32_t rd0 = Rd(w0);
  const uint32_t sa0 = Sa(w0);
  const uint32_t funct0 = Funct(w0);
  const uint32_t uimm0 = Uimm16(w0);
  const int16_t simm0 = Simm16(w0);

  const uint32_t op1 = Opcode(w1);
  const uint32_t rs1 = Rs(w1);
  const uint32_t rt1 = Rt(w1);
  const uint32_t rd1 = Rd(w1);
  const uint32_t sa1 = Sa(w1);
  const uint32_t funct1 = Funct(w1);
  const uint32_t uimm1 = Uimm16(w1);
  const int16_t simm1 = Simm16(w1);

  const uint32_t op2 = Opcode(w2);
  const uint32_t rs2 = Rs(w2);
  const uint32_t rt2 = Rt(w2);
  const uint32_t rd2 = Rd(w2);
  const uint32_t sa2 = Sa(w2);
  const uint32_t funct2 = Funct(w2);

  const uint32_t op3 = Opcode(w3);
  const uint32_t rs3 = Rs(w3);
  const uint32_t rt3 = Rt(w3);
  const uint32_t rd3 = Rd(w3);
  const uint32_t sa3 = Sa(w3);
  const uint32_t funct3 = Funct(w3);

  // 1. `M_ROL` / `M_ROR` (`tc-mips.c:4233-4255`):
  //   subu $1, $0, $treg
  //   srlv/sllv $1, $sreg, $1
  //   sllv/srlv $dreg, $sreg, $treg
  //   or $dreg, $dreg, $1
  if (op0 == 0x00u && rs0 == 0u && rd0 == 1u && sa0 == 0u && funct0 == 0x23u && op1 == 0x00u &&
      rs1 == 1u && rd1 == 1u && sa1 == 0u && op2 == 0x00u && rs2 == rt0 && rt2 == rt1 &&
      sa2 == 0u && op3 == 0x00u && rs3 == rd2 && rt3 == 1u && rd3 == rd2 && sa3 == 0u &&
      funct3 == 0x25u) {
    if (funct1 == 0x06u && funct2 == 0x04u) {
      return absl::StrFormat("rol $%u,$%u,$%u", rd2, rt1, rt0);
    }
    if (funct1 == 0x04u && funct2 == 0x06u) {
      return absl::StrFormat("ror $%u,$%u,$%u", rd2, rt1, rt0);
    }
  }

  // 2. `M_ULH` / `M_ULHU` (`tc-mips.c:4580-4601`, big-endian):
  //   lb/lbu $treg, off($breg)
  //   lbu $1, (off+1)($breg)
  //   sll $treg, $treg, 8
  //   or $treg, $treg, $1
  if ((op0 == 0x20u || op0 == 0x24u) && simm0 < 0x7fff && op1 == 0x24u && rt1 == 1u && rs1 == rs0 &&
      simm1 == simm0 + 1 && op2 == 0x00u && rs2 == 0u && rt2 == rt0 && rd2 == rt0 && sa2 == 8u &&
      funct2 == 0x00u && op3 == 0x00u && rs3 == rt0 && rt3 == 1u && rd3 == rt0 && sa3 == 0u &&
      funct3 == 0x25u) {
    return absl::StrFormat("%s $%u,%d($%u)", op0 == 0x24u ? "ulhu" : "ulh", rt0, simm0, rs0);
  }

  // 3. 4-word `M_SEQ_I` / `M_SNE_I` / `M_SGE_I` / `M_SGEU_I` / `M_SLE_I` / `M_SLEU_I`
  // with 2-word `$1` load (`lui $1, hi16` + `ori $1, $1, lo16` + tail).
  if (op0 == 0x0fu && rs0 == 0u && rt0 == 1u && uimm0 != 0u && op1 == 0x0du && rs1 == 1u &&
      rt1 == 1u && uimm1 != 0u) {
    const uint32_t imm32 = (uimm0 << 16) | uimm1;
    if (std::string set_imm = InvertSetImmediateTail(w2, w3, imm32); !set_imm.empty()) {
      return set_imm;
    }
    // 4. `M_MUL_I` / `M_DMUL_I` with 2-word `$1` load (`lui $1, hi16` + `ori $1, $1, lo16` +
    // `mult`/`dmult $rs, $1` + `mflo $rd`).
    if (op2 == 0x00u && rt2 == 1u && rd2 == 0u && sa2 == 0u &&
        (funct2 == 0x18u || funct2 == 0x1cu) && op3 == 0x00u && rs3 == 0u && rt3 == 0u &&
        sa3 == 0u && funct3 == 0x12u) {
      return absl::StrFormat("%s $%u,$%u,0x%08x", funct2 == 0x1cu ? "dmul" : "mul", rd3, rs2,
                             imm32);
    }
  }

  return "";
}

std::string InvertFiveWords(absl::Span<const uint32_t> words) {
  const uint32_t w0 = words[0];
  const uint32_t w1 = words[1];
  const uint32_t w2 = words[2];
  const uint32_t w3 = words[3];
  const uint32_t w4 = words[4];

  // `M_DIVU_3` / `M_REMU_3` / `M_DDIVU_3` / `M_DREMU_3` (`tc-mips.c:3011-3042`):
  //   divu/ddivu $0, $sreg, $treg
  //   bne $treg, $0, .+8   (offset field = 2)
  //   nop
  //   break 7
  //   mflo/mfhi $dreg
  if (Opcode(w0) == 0x00u && Rd(w0) == 0u && Sa(w0) == 0u &&
      (Funct(w0) == 0x1bu || Funct(w0) == 0x1fu)) {
    const uint32_t sreg = Rs(w0);
    const uint32_t treg = Rt(w0);
    const bool is_dbl = (Funct(w0) == 0x1fu);
    if (w1 == (0x14000002u | (treg << 21)) && w2 == 0x00000000u && w3 == 0x0007000du &&
        Opcode(w4) == 0x00u && Rs(w4) == 0u && Rt(w4) == 0u && Sa(w4) == 0u &&
        (Funct(w4) == 0x12u || Funct(w4) == 0x10u)) {
      const uint32_t dreg = Rd(w4);
      const bool is_rem = (Funct(w4) == 0x10u);
      const std::string_view mnem =
          is_dbl ? (is_rem ? "dremu" : "ddivu") : (is_rem ? "remu" : "divu");
      return absl::StrFormat("%s $%u,$%u,$%u", mnem, dreg, sreg, treg);
    }
  }
  return "";
}

std::string InvertSixWords(absl::Span<const uint32_t> words) {
  const uint32_t w0 = words[0];
  // `M_MULOU` / `M_DMULOU` (`tc-mips.c:4210-4231`):
  //   multu/dmultu $sreg, $treg
  //   mfhi $1
  //   mflo $dreg
  //   beq $1, $0, .+8
  //   nop
  //   break 6
  if (Opcode(w0) == 0x00u && Rd(w0) == 0u && Sa(w0) == 0u &&
      (Funct(w0) == 0x19u || Funct(w0) == 0x1du) && words[1] == 0x00000810u &&
      (words[2] & 0xffff07ffu) == 0x00000012u && words[3] == 0x10200002u &&
      words[4] == 0x00000000u && words[5] == 0x0006000du) {
    const uint32_t sreg = Rs(w0);
    const uint32_t treg = Rt(w0);
    const uint32_t dreg = Rd(words[2]);
    return absl::StrFormat("%s $%u,$%u,$%u", Funct(w0) == 0x1du ? "dmulou" : "mulou", dreg, sreg,
                           treg);
  }
  return "";
}

std::string InvertEightWords(absl::Span<const uint32_t> words) {
  const uint32_t w0 = words[0];
  // `M_MULO` / `M_DMULO` (`tc-mips.c:4183-4208`):
  //   mult/dmult $sreg, $treg
  //   mflo $dreg
  //   sra/dsra32 $dreg, $dreg, 31
  //   mfhi $1
  //   beq $dreg, $1, .+8
  //   nop
  //   break 6
  //   mflo $dreg
  if (Opcode(w0) == 0x00u && Rd(w0) == 0u && Sa(w0) == 0u &&
      (Funct(w0) == 0x18u || Funct(w0) == 0x1cu) && (words[1] & 0xffff07ffu) == 0x00000012u) {
    const uint32_t sreg = Rs(w0);
    const uint32_t treg = Rt(w0);
    const uint32_t dreg = Rd(words[1]);
    const bool is_dbl = (Funct(w0) == 0x1cu);
    const uint32_t expected_w2 = (is_dbl ? 0x000007ffu : 0x000007c3u) | (dreg << 16) | (dreg << 11);
    if (words[2] == expected_w2 && words[3] == 0x00000810u &&
        words[4] == (0x10010002u | (dreg << 21)) && words[5] == 0x00000000u &&
        words[6] == 0x0006000du && words[7] == words[1]) {
      return absl::StrFormat("%s $%u,$%u,$%u", is_dbl ? "dmulo" : "mulo", dreg, sreg, treg);
    }
  }
  return "";
}

std::string InvertElevenWords(absl::Span<const uint32_t> words) {
  const uint32_t w0 = words[0];
  // `M_DIV_3` / `M_REM_3` (`tc-mips.c:2869-2933`):
  //   0: div $0, $sreg, $treg   (treg != 0)
  //   1: bne $treg, $0, .+8     (offset field = 2)
  //   2: nop
  //   3: break 7
  //   4: addiu $1, $0, -1
  //   5: bne $treg, $1, .+16    (offset field = 4)
  //   6: lui $1, 0x8000
  //   7: bne $sreg, $1, .+8     (offset field = 2)
  //   8: nop
  //   9: break 6
  //  10: mflo/mfhi $dreg
  if (Opcode(w0) == 0x00u && Rd(w0) == 0u && Sa(w0) == 0u && Funct(w0) == 0x1au && Rt(w0) != 0u) {
    const uint32_t sreg = Rs(w0);
    const uint32_t treg = Rt(w0);
    const uint32_t w10 = words[10];
    if (words[1] == (0x14000002u | (treg << 21)) && words[2] == 0x00000000u &&
        words[3] == 0x0007000du && words[4] == 0x2401ffffu &&
        words[5] == (0x14010004u | (treg << 21)) && words[6] == 0x3c018000u &&
        words[7] == (0x14010002u | (sreg << 21)) && words[8] == 0x00000000u &&
        words[9] == 0x0006000du && Opcode(w10) == 0x00u && Rs(w10) == 0u && Rt(w10) == 0u &&
        Sa(w10) == 0u && (Funct(w10) == 0x12u || Funct(w10) == 0x10u)) {
      const uint32_t dreg = Rd(w10);
      return absl::StrFormat("%s $%u,$%u,$%u", Funct(w10) == 0x10u ? "rem" : "div", dreg, sreg,
                             treg);
    }
  }
  return "";
}

std::string InvertTwelveWords(absl::Span<const uint32_t> words) {
  const uint32_t w0 = words[0];
  // `M_DDIV_3` / `M_DREM_3` (`tc-mips.c:2867-2933`):
  //   0: ddiv $0, $sreg, $treg  (treg != 0)
  //   1: bne $treg, $0, .+8     (offset field = 2)
  //   2: nop
  //   3: break 7
  //   4: daddiu $1, $0, -1
  //   5: bne $treg, $1, .+20    (offset field = 5)
  //   6: daddiu $1, $0, 1
  //   7: dsll32 $1, $1, 31
  //   8: bne $sreg, $1, .+8     (offset field = 2)
  //   9: nop
  //  10: break 6
  //  11: mflo/mfhi $dreg
  if (Opcode(w0) == 0x00u && Rd(w0) == 0u && Sa(w0) == 0u && Funct(w0) == 0x1eu && Rt(w0) != 0u) {
    const uint32_t sreg = Rs(w0);
    const uint32_t treg = Rt(w0);
    const uint32_t w11 = words[11];
    if (words[1] == (0x14000002u | (treg << 21)) && words[2] == 0x00000000u &&
        words[3] == 0x0007000du && words[4] == 0x6401ffffu &&
        words[5] == (0x14010005u | (treg << 21)) && words[6] == 0x64010001u &&
        words[7] == 0x00010ffcu && words[8] == (0x14010002u | (sreg << 21)) &&
        words[9] == 0x00000000u && words[10] == 0x0006000du && Opcode(w11) == 0x00u &&
        Rs(w11) == 0u && Rt(w11) == 0u && Sa(w11) == 0u &&
        (Funct(w11) == 0x12u || Funct(w11) == 0x10u)) {
      const uint32_t dreg = Rd(w11);
      return absl::StrFormat("%s $%u,$%u,$%u", Funct(w11) == 0x10u ? "drem" : "ddiv", dreg, sreg,
                             treg);
    }
  }
  return "";
}

int ParseLuiZeroReg(std::string_view line) {
  if ((!absl::StartsWith(line, "lui $") && !absl::StartsWith(line, "lui\t$")) ||
      !absl::EndsWith(line, ",0x0")) {
    return -1;
  }
  int reg = -1;
  if (!absl::SimpleAtoi(line.substr(5, line.size() - 9), &reg) || reg < 1 || reg > 31) {
    return -1;
  }
  return reg;
}

bool ParseIndexedAddrAddu(std::string_view line, int expected_tempreg, int* out_base_reg) {
  const size_t sep = line.find_first_of(" \t");
  if (sep == std::string_view::npos) {
    return false;
  }
  const std::string_view mnem = line.substr(0, sep);
  if (mnem != "addu" && mnem != "daddu") {
    return false;
  }
  const std::string_view rest = absl::StripAsciiWhitespace(line.substr(sep + 1));
  const std::string prefix = absl::StrFormat("$%d,$%d,$", expected_tempreg, expected_tempreg);
  if (!absl::StartsWith(rest, prefix)) {
    return false;
  }
  int base_reg = -1;
  if (!absl::SimpleAtoi(rest.substr(prefix.size()), &base_reg) || base_reg < 1 || base_reg > 31) {
    return false;
  }
  *out_base_reg = base_reg;
  return true;
}

bool ParseSameRegAddiuOffset(std::string_view line, int expected_reg, int32_t* out_off) {
  const size_t sep = line.find_first_of(" \t");
  if (sep == std::string_view::npos) {
    return false;
  }
  const std::string_view mnem = line.substr(0, sep);
  if (mnem != "addiu" && mnem != "daddiu") {
    return false;
  }
  const std::string_view rest = absl::StripAsciiWhitespace(line.substr(sep + 1));
  const std::string prefix = absl::StrFormat("$%d,$%d,", expected_reg, expected_reg);
  if (!absl::StartsWith(rest, prefix)) {
    return false;
  }
  const std::string_view off_str = rest.substr(prefix.size());
  int32_t off = 0;
  if (!absl::SimpleAtoi(off_str, &off)) {
    return false;
  }
  *out_off = off;
  return true;
}

bool ParseBaseMemInsn(std::string_view line, int expected_base_reg, std::string* out_mnem,
                      std::string* out_reg, int32_t* out_off) {
  const size_t sep = line.find_first_of(" \t");
  if (sep == std::string_view::npos) {
    return false;
  }
  std::string_view mnem = line.substr(0, sep);
  if (mnem == "lwc1") mnem = "l.s";
  if (mnem == "swc1") mnem = "s.s";
  if (mnem == "ldc1") mnem = "l.d";
  if (mnem == "sdc1") mnem = "s.d";
  if (mnem != "lb" && mnem != "lbu" && mnem != "lh" && mnem != "lhu" && mnem != "lw" &&
      mnem != "lwu" && mnem != "ld" && mnem != "sb" && mnem != "sh" && mnem != "sw" &&
      mnem != "sd" && mnem != "l.s" && mnem != "s.s" && mnem != "l.d" && mnem != "s.d") {
    return false;
  }
  const std::string_view rest = absl::StripAsciiWhitespace(line.substr(sep + 1));
  const size_t comma = rest.find(',');
  const std::string suffix = absl::StrFormat("($%d)", expected_base_reg);
  if (comma == std::string_view::npos || !absl::EndsWith(rest, suffix)) {
    return false;
  }
  const std::string_view reg = rest.substr(0, comma);
  if (expected_base_reg != 1 && reg != absl::StrFormat("$%d", expected_base_reg)) {
    return false;
  }
  const std::string_view off_str = rest.substr(comma + 1, rest.size() - comma - 1 - suffix.size());
  int32_t off = 0;
  if (!off_str.empty() && !absl::SimpleAtoi(off_str, &off)) {
    return false;
  }
  *out_mnem = std::string(mnem);
  *out_reg = std::string(reg);
  *out_off = off;
  return true;
}

std::string FormatSymOffset(int32_t off) {
  if (off == 0) {
    return "__sym_0";
  }
  if (off > 0) {
    return absl::StrFormat("__sym_0+%d", off);
  }
  return absl::StrFormat("__sym_0%d", off);
}

bool IsPairedFpDoubleSecondHalf(std::string_view next_line, int expected_base_reg,
                                std::string_view first_mnem, std::string_view first_reg,
                                int32_t first_off) {
  if ((first_mnem != "l.s" && first_mnem != "s.s") || !absl::StartsWith(first_reg, "$f")) {
    return false;
  }
  int fpreg = -1;
  if (!absl::SimpleAtoi(first_reg.substr(2), &fpreg) || (fpreg % 2) != 0) {
    return false;
  }
  std::string next_mnem;
  std::string next_reg;
  int32_t next_off = 0;
  if (!ParseBaseMemInsn(next_line, expected_base_reg, &next_mnem, &next_reg, &next_off)) {
    return false;
  }
  return next_mnem == first_mnem && next_reg == absl::StrFormat("$f%d", fpreg + 1) &&
         next_off == first_off + 4;
}

std::string NormalizeCop1LoadStoreMnemonic(std::string_view line) {
  if (absl::StartsWith(line, "lwc1 ") || absl::StartsWith(line, "lwc1\t")) {
    return absl::StrCat("l.s ", absl::StripAsciiWhitespace(line.substr(5)));
  }
  if (absl::StartsWith(line, "swc1 ") || absl::StartsWith(line, "swc1\t")) {
    return absl::StrCat("s.s ", absl::StripAsciiWhitespace(line.substr(5)));
  }
  if (absl::StartsWith(line, "ldc1 ") || absl::StartsWith(line, "ldc1\t")) {
    return absl::StrCat("l.d ", absl::StripAsciiWhitespace(line.substr(5)));
  }
  if (absl::StartsWith(line, "sdc1 ") || absl::StartsWith(line, "sdc1\t")) {
    return absl::StrCat("s.d ", absl::StripAsciiWhitespace(line.substr(5)));
  }
  return std::string(line);
}

}  // namespace

std::string InvertGasMacro(absl::Span<const uint32_t> macro_words_x) {
  if (macro_words_x.empty()) {
    return "";
  }
  for (uint32_t w : macro_words_x) {
    if (InvertGasMipsIp(w).empty()) {
      return "";
    }
  }

  switch (macro_words_x.size()) {
    case 1:
      return InvertOneWord(macro_words_x[0]);
    case 2:
      return InvertTwoWords(macro_words_x[0], macro_words_x[1]);
    case 3:
      return InvertThreeWords(macro_words_x[0], macro_words_x[1], macro_words_x[2]);
    case 4:
      return InvertFourWords(macro_words_x[0], macro_words_x[1], macro_words_x[2],
                             macro_words_x[3]);
    case 5:
      return InvertFiveWords(macro_words_x);
    case 6:
      return InvertSixWords(macro_words_x);
    case 8:
      return InvertEightWords(macro_words_x);
    case 11:
      return InvertElevenWords(macro_words_x);
    case 12:
      return InvertTwelveWords(macro_words_x);
    default:
      return "";
  }
}

std::vector<std::string> CollapseGasMacrosAndFlattenLines(absl::Span<const AsmItem> raw_items) {
  // Pass 1: collapse multi-word `InvertGasMacro` expansions (`M_DIV_3`,
  // `M_REM_3`, `M_DIVU_3`, `M_REMU_3`, `M_LI`) and normalize single-instruction
  // mnemonics (`lwc1`/`swc1`/`ldc1`/`sdc1`, `jal`).
  std::vector<AsmItem> items;
  items.reserve(raw_items.size());
  const auto is_nop_delay_branch_or_reorder = [](const AsmItem& it) {
    return it.kind == AsmItemKind::kReorderInsn ||
           (it.kind == AsmItemKind::kNoreorderBranchSeq && it.delay_word == 0x00000000u &&
            it.delay_label.empty());
  };
  for (size_t i = 0; i < raw_items.size(); ++i) {
    AsmItem item = raw_items[i];
    if (item.kind == AsmItemKind::kReorderInsn) {
      // 11-word signed `div`/`rem` (`M_DIV_3` / `M_REM_3`) spanning 10 `AsmItem`s:
      if (i + 9 < raw_items.size() && is_nop_delay_branch_or_reorder(raw_items[i + 1]) &&
          raw_items[i + 2].kind == AsmItemKind::kReorderInsn &&
          raw_items[i + 2].word == 0x0007000du && raw_items[i + 3].kind == AsmItemKind::kLabel &&
          raw_items[i + 4].kind == AsmItemKind::kReorderInsn &&
          raw_items[i + 5].kind == AsmItemKind::kNoreorderBranchSeq &&
          is_nop_delay_branch_or_reorder(raw_items[i + 6]) &&
          raw_items[i + 7].kind == AsmItemKind::kReorderInsn &&
          raw_items[i + 7].word == 0x0006000du && raw_items[i + 8].kind == AsmItemKind::kLabel &&
          raw_items[i + 9].kind == AsmItemKind::kReorderInsn) {
        const uint32_t macro11[11] = {
            item.word,
            raw_items[i + 1].word,
            0x00000000u,
            raw_items[i + 2].word,
            raw_items[i + 4].word,
            raw_items[i + 5].word,
            raw_items[i + 5].delay_word,
            raw_items[i + 6].word,
            0x00000000u,
            raw_items[i + 7].word,
            raw_items[i + 9].word,
        };
        const std::string macro_asm = InvertGasMacro(macro11);
        if (!macro_asm.empty()) {
          item.line = macro_asm;
          item.unswapped_from_branch = false;
          item.preceded_by_active_hazard = false;
          items.push_back(std::move(item));
          i += 9;
          continue;
        }
      }

      // 5-word unsigned `divu`/`remu` (`M_DIVU_3` / `M_REMU_3`) spanning 5 `AsmItem`s:
      if (i + 4 < raw_items.size() && is_nop_delay_branch_or_reorder(raw_items[i + 1]) &&
          raw_items[i + 2].kind == AsmItemKind::kReorderInsn &&
          raw_items[i + 2].word == 0x0007000du && raw_items[i + 3].kind == AsmItemKind::kLabel &&
          raw_items[i + 4].kind == AsmItemKind::kReorderInsn) {
        const uint32_t macro5[5] = {
            item.word,
            raw_items[i + 1].word,
            0x00000000u,
            raw_items[i + 2].word,
            raw_items[i + 4].word,
        };
        const std::string macro_asm = InvertGasMacro(macro5);
        if (!macro_asm.empty()) {
          item.line = macro_asm;
          item.unswapped_from_branch = false;
          item.preceded_by_active_hazard = false;
          items.push_back(std::move(item));
          i += 4;
          continue;
        }
      }

      if (i + 1 < raw_items.size() && raw_items[i + 1].kind == AsmItemKind::kReorderInsn &&
          !IsBranchOrJumpWord(item.word) && !IsBranchOrJumpWord(raw_items[i + 1].word)) {
        const uint32_t pair[2] = {item.word, raw_items[i + 1].word};
        const std::string macro_asm = InvertGasMacro(pair);
        if (!macro_asm.empty() &&
            (absl::StartsWith(macro_asm, "li ") || absl::StartsWith(macro_asm, "li\t"))) {
          item.line = macro_asm;
          item.unswapped_from_branch = false;
          items.push_back(std::move(item));
          ++i;
          continue;
        }
      }
      item.line = NormalizeCop1LoadStoreMnemonic(item.line);
      if (item.line == "jal 0x0" || item.line == "jal\t0x0") {
        item.line = "jal __callee_0";
      }
    } else if (item.kind == AsmItemKind::kNoreorderBranchSeq) {
      item.line = NormalizeCop1LoadStoreMnemonic(item.line);
      if (item.line == "jal 0x0" || item.line == "jal\t0x0") {
        item.line = "jal __callee_0";
      }
      item.delay_line = NormalizeCop1LoadStoreMnemonic(item.delay_line);
    }
    items.push_back(std::move(item));
  }

  // Pass 2: collapse 2/3/4-instruction GAS symbolic macros (`lui $<tempreg>,0x0`
  // followed by symbolic load/store or `daddiu`/`addiu` for `la`).
  std::vector<AsmItem> collapsed;
  collapsed.reserve(items.size());
  for (size_t i = 0; i < items.size(); ++i) {
    AsmItem item = items[i];
    if (item.kind == AsmItemKind::kReorderInsn) {
      const int tempreg = ParseLuiZeroReg(item.line);
      if (tempreg >= 1) {
        // 1) 3-instruction (or 4-instruction paired `l.d`/`s.d`) indexed symbolic load/store:
        //    `lui $<tempreg>,0x0`
        //    `addu/daddu $<tempreg>,$<tempreg>,$<breg>`
        //    `<memop> <reg>,<off>($<tempreg>)`
        int base_reg = -1;
        if (i + 2 < items.size() && items[i + 1].kind == AsmItemKind::kReorderInsn &&
            items[i + 2].kind == AsmItemKind::kReorderInsn &&
            ParseIndexedAddrAddu(items[i + 1].line, tempreg, &base_reg)) {
          std::string mnem;
          std::string reg;
          int32_t off = 0;
          if (ParseBaseMemInsn(items[i + 2].line, tempreg, &mnem, &reg, &off)) {
            if (i + 3 < items.size() && items[i + 3].kind == AsmItemKind::kReorderInsn &&
                IsPairedFpDoubleSecondHalf(items[i + 3].line, tempreg, mnem, reg, off)) {
              item.line = absl::StrFormat("%s %s,%s($%d)", mnem == "l.s" ? "l.d" : "s.d", reg,
                                          FormatSymOffset(off), base_reg);
              item.unswapped_from_branch = false;
              collapsed.push_back(std::move(item));
              i += 3;
              continue;
            }
            item.line = absl::StrFormat("%s %s,%s($%d)", mnem, reg, FormatSymOffset(off), base_reg);
            item.unswapped_from_branch = false;
            collapsed.push_back(std::move(item));
            i += 2;
            continue;
          }
        }

        // 2) 2-instruction (or 3-instruction paired `l.d`/`s.d`) absolute symbolic load/store:
        //    `lui $<tempreg>,0x0`
        //    `<memop> <reg>,<off>($<tempreg>)`
        if (i + 1 < items.size() && items[i + 1].kind == AsmItemKind::kReorderInsn) {
          std::string mnem;
          std::string reg;
          int32_t off = 0;
          if (ParseBaseMemInsn(items[i + 1].line, tempreg, &mnem, &reg, &off)) {
            if (i + 2 < items.size() && items[i + 2].kind == AsmItemKind::kReorderInsn &&
                IsPairedFpDoubleSecondHalf(items[i + 2].line, tempreg, mnem, reg, off)) {
              item.line = absl::StrFormat("%s %s,%s", mnem == "l.s" ? "l.d" : "s.d", reg,
                                          FormatSymOffset(off));
              item.unswapped_from_branch = false;
              collapsed.push_back(std::move(item));
              i += 2;
              continue;
            }
            item.line = absl::StrFormat("%s %s,%s", mnem, reg, FormatSymOffset(off));
            item.unswapped_from_branch = false;
            collapsed.push_back(std::move(item));
            ++i;
            continue;
          }

          // 3) 2-instruction `la $<tempreg>,__sym_0+<off>` (`M_LA_AB`):
          //    `lui $<tempreg>,0x0`
          //    `daddiu/addiu $<tempreg>,$<tempreg>,<off>`
          int32_t la_off = 0;
          if (ParseSameRegAddiuOffset(items[i + 1].line, tempreg, &la_off)) {
            item.line = absl::StrFormat("la $%d,%s", tempreg, FormatSymOffset(la_off));
            item.unswapped_from_branch = false;
            collapsed.push_back(std::move(item));
            ++i;
            continue;
          }
        }
      }
    }
    collapsed.push_back(std::move(item));
  }

  // Pass 3: flatten into assembly lines, emitting any remaining non-hazard,
  // non-multi-line un-swapped `[delay_item, branch_item]` pairs as
  // `.set noreorder` `SEQUENCE`s.
  std::vector<std::string> lines;
  lines.reserve(collapsed.size() * 2);
  for (size_t i = 0; i < collapsed.size(); ++i) {
    const AsmItem& item = collapsed[i];
    if (item.kind == AsmItemKind::kLabel) {
      lines.push_back(item.line);
      continue;
    }
    if (item.kind == AsmItemKind::kNoreorderBranchSeq) {
      lines.push_back(".set\tnoreorder");
      lines.push_back(item.line);
      if (!item.delay_label.empty()) {
        lines.push_back(item.delay_label);
      }
      lines.push_back(item.delay_line);
      lines.push_back(".set\treorder");
      continue;
    }

    if (item.unswapped_from_branch && i + 1 < collapsed.size() &&
        collapsed[i + 1].kind == AsmItemKind::kReorderInsn &&
        collapsed[i + 1].unswapped_from_branch && !IsBranchOrJumpWord(item.word) &&
        IsBranchOrJumpWord(collapsed[i + 1].word)) {
      const bool part_of_prev_float_si =
          i > 0 && collapsed[i - 1].kind == AsmItemKind::kReorderInsn &&
          (absl::StartsWith(collapsed[i - 1].line, "mtc1 ") ||
           absl::StartsWith(collapsed[i - 1].line, "l.s ")) &&
          (absl::StartsWith(item.line, "cvt.s.w ") || absl::StartsWith(item.line, "cvt.d.w "));
      if (!item.preceded_by_active_hazard && !part_of_prev_float_si) {
        lines.push_back(".set\tnoreorder");
        lines.push_back(collapsed[i + 1].line);
        lines.push_back(item.line);
        lines.push_back(".set\treorder");
        ++i;
        continue;
      }
    }

    lines.push_back(item.line);
  }

  return lines;
}

}  // namespace rom_nom_nom::lifter
