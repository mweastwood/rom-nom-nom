#include "lifter/invert_gas_macro.h"

#include <algorithm>
#include <array>
#include <cmath>
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
#include "absl/container/flat_hash_set.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "core/compiler_flags.h"
#include "core/rtl_cfg.h"
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

  // 2b. `M_LI_SS` 2-word inline single-precision float load (`tc-mips.c:3697-3704`
  // when `g_switch_value < 4`): `load_register($1, bits)` + `mtc1 $1, $f<fs>`.
  if ((w1 & 0xffe007ffu) == 0x44800000u && rt1 == 1u) {
    uint32_t bits = 0u;
    bool valid_bits = false;
    if (op0 == 0x0fu && rs0 == 0u && rt0 == 1u && uimm0 != 0u) {
      bits = (uimm0 << 16);
      valid_bits = true;
    } else if (op0 == 0x0du && rs0 == 0u && rt0 == 1u && uimm0 >= 0x8000u) {
      bits = uimm0;
      valid_bits = true;
    } else if (op0 == 0x09u && rs0 == 0u && rt0 == 1u && simm0 > 0) {
      bits = static_cast<uint32_t>(simm0);
      valid_bits = true;
    }
    if (valid_bits && bits != 0x80000000u) {
      const float fv = absl::bit_cast<float>(bits);
      if (std::isfinite(fv)) {
        return absl::StrFormat("li.s $f%u,%.9e", rd1, static_cast<double>(fv));
      }
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
    // `M_LI_SS` 3-word inline single-precision float load (`tc-mips.c:3697-3704`):
    //   lui $1, hi16
    //   ori $1, $1, lo16
    //   mtc1 $1, $f<fs>
    if ((w2 & 0xffe007ffu) == 0x44800000u && rt2 == 1u) {
      const float fv = absl::bit_cast<float>(imm32);
      if (std::isfinite(fv)) {
        return absl::StrFormat("li.s $f%u,%.9e", rd2, static_cast<double>(fv));
      }
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

std::string InvertTenWords(absl::Span<const uint32_t> words) {
  const uint32_t w0 = words[0];
  // `M_TRUNCWS` / `M_TRUNCWD` (`tc-mips.c:4549-4578`, `mips_isa < 2`):
  //   0: cfc1 $treg, $31        (0x4440f800 | (treg << 16), treg in 2..31)
  //   1: cfc1 $treg, $31
  //   2: nop
  //   3: ori  $1, $treg, 3      (0x34010003 | (treg << 21))
  //   4: xori $1, $1, 2         (0x38210002)
  //   5: ctc1 $1, $31           (0x44c1f800)
  //   6: nop
  //   7: cvt.w.[sd] $fd, $fs    (0x46000024 / 0x46200024)
  //   8: ctc1 $treg, $31        (0x44c0f800 | (treg << 16))
  //   9: nop
  if ((w0 & 0xffe0ffffu) == 0x4440f800u) {
    const uint32_t treg = Rt(w0);
    const uint32_t w7 = words[7];
    if (treg >= 2u && treg <= 31u && words[1] == w0 && words[2] == 0x00000000u &&
        words[3] == (0x34010003u | (treg << 21)) && words[4] == 0x38210002u &&
        words[5] == 0x44c1f800u && words[6] == 0x00000000u && (w7 & 0xffdf003fu) == 0x46000024u &&
        words[8] == (0x44c0f800u | (treg << 16)) && words[9] == 0x00000000u) {
      const bool is_double = ((w7 & 0x00200000u) != 0u);
      const uint32_t sreg = (w7 >> 11) & 0x1fu;
      const uint32_t dreg = (w7 >> 6) & 0x1fu;
      return absl::StrFormat("trunc.w.%c $f%u,$f%u,$%u", is_double ? 'd' : 's', dreg, sreg, treg);
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

bool ParseAnyMemInsnWithBase(std::string_view line, int expected_base_reg, std::string* out_mnem,
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

bool MatchesGasMacroTempreg(std::string_view mnem, std::string_view reg, int breg, int tempreg) {
  if (mnem == "sb" || mnem == "sh" || mnem == "sw" || mnem == "sd" || mnem == "s.s" ||
      mnem == "s.d" || mnem == "l.s" || mnem == "l.d") {
    return tempreg == 1;
  }
  if (!absl::StartsWith(reg, "$")) {
    return false;
  }
  int treg = -1;
  if (!absl::SimpleAtoi(reg.substr(1), &treg) || treg < 0 || treg > 31) {
    return false;
  }
  const int expected = (breg == treg || treg == 0) ? 1 : treg;
  return tempreg == expected;
}

bool MatchPairedFpDoubleReloc(std::string_view next_line, int expected_base_reg,
                              std::string_view first_mnem, std::string_view first_reg,
                              const Stage0Relocation* rel_first, const Stage0Relocation* rel_next,
                              std::string* out_even_fpreg) {
  if ((first_mnem != "l.s" && first_mnem != "s.s") || !absl::StartsWith(first_reg, "$f") ||
      rel_first == nullptr || rel_next == nullptr || rel_next->kind != rel_first->kind ||
      rel_next->symbol_name != rel_first->symbol_name ||
      rel_next->addend != rel_first->addend + 4) {
    return false;
  }
  int first_fp = -1;
  if (!absl::SimpleAtoi(first_reg.substr(2), &first_fp) || first_fp < 0 || first_fp > 31) {
    return false;
  }
  std::string next_mnem;
  std::string next_reg;
  int32_t next_off = 0;
  if (!ParseAnyMemInsnWithBase(next_line, expected_base_reg, &next_mnem, &next_reg, &next_off) ||
      next_mnem != first_mnem || !absl::StartsWith(next_reg, "$f")) {
    return false;
  }
  int next_fp = -1;
  if (!absl::SimpleAtoi(next_reg.substr(2), &next_fp) || next_fp < 0 || next_fp > 31) {
    return false;
  }
  if ((first_fp % 2) == 0 && next_fp == first_fp + 1) {
    *out_even_fpreg = absl::StrFormat("$f%d", first_fp);
    return true;
  }
  if ((first_fp % 2) == 1 && next_fp == first_fp - 1) {
    *out_even_fpreg = absl::StrFormat("$f%d", next_fp);
    return true;
  }
  return false;
}

bool WordWritesGpr(uint32_t w, uint32_t reg) {
  if (reg == 0u) {
    return false;
  }
  const uint32_t pinfo = GetInsnPinfo(w);
  if ((pinfo & 0x00000001u) != 0u && Rd(w) == reg) {
    return true;
  }
  if ((pinfo & 0x00000002u) != 0u && Rt(w) == reg) {
    return true;
  }
  if ((pinfo & 0x01000000u) != 0u && reg == 31u) {
    return true;
  }
  return false;
}

bool IsTempregReusedByLaterLo16(
    absl::Span<const AsmItem> items, size_t start_idx, int tempreg,
    const absl::flat_hash_map<size_t, const Stage0Relocation*>& reloc_by_word) {
  // `$1` (`$at`) is strictly GAS's macro scratch register (`FIXED_REGISTERS[1] == 1`
  // in `mips.h`) and is never CSE'd or allocated across instructions by GCC.
  if (tempreg <= 1) {
    return false;
  }
  const uint32_t treg = static_cast<uint32_t>(tempreg);
  bool saw_control_flow = false;
  for (size_t j = start_idx; j < items.size(); ++j) {
    const AsmItem& it = items[j];
    if (it.kind == AsmItemKind::kLabel) {
      saw_control_flow = true;
      continue;
    }
    auto check_word_uses_lo16_base = [&](uint32_t w, size_t w_idx, size_t prev_item_idx) -> bool {
      auto rel_it = reloc_by_word.find(w_idx);
      if (rel_it == reloc_by_word.end() || rel_it->second->kind != Stage0RelocKind::kMipsLo16) {
        return false;
      }
      if (Rs(w) != treg) {
        return false;
      }
      // Check if this `%lo` was preceded immediately by its own `lui $tempreg`
      // (or `lui $tempreg` + `addu $tempreg,$tempreg,$breg` for indexed access).
      auto is_own_hi16_lui = [&](size_t idx) -> bool {
        if (idx >= items.size() || idx < start_idx ||
            items[idx].kind != AsmItemKind::kReorderInsn || Opcode(items[idx].word) != 0x0fu ||
            Rt(items[idx].word) != treg) {
          return false;
        }
        auto hi_it = reloc_by_word.find(items[idx].word_index);
        return hi_it != reloc_by_word.end() && hi_it->second->kind == Stage0RelocKind::kMipsHi16;
      };
      if (is_own_hi16_lui(prev_item_idx)) {
        return false;
      }
      int dummy_base = -1;
      return !(prev_item_idx < items.size() && prev_item_idx > start_idx &&
               items[prev_item_idx].kind == AsmItemKind::kReorderInsn &&
               ParseIndexedAddrAddu(items[prev_item_idx].line, tempreg, &dummy_base) &&
               is_own_hi16_lui(prev_item_idx - 1));
    };

    if (it.kind == AsmItemKind::kNoreorderBranchSeq) {
      if (check_word_uses_lo16_base(it.delay_word, it.delay_word_index,
                                    j > 0 ? j - 1 : items.size())) {
        return true;
      }
      saw_control_flow = true;
      continue;
    }

    if (check_word_uses_lo16_base(it.word, it.word_index, j > 0 ? j - 1 : items.size())) {
      return true;
    }
    if (IsBranchOrJumpWord(it.word)) {
      saw_control_flow = true;
      continue;
    }
    if (!saw_control_flow && WordWritesGpr(it.word, treg)) {
      return false;
    }
  }
  return false;
}

bool TryCollapseMips1TruncMacroItems(
    absl::Span<const AsmItem> raw_items, size_t start_idx,
    const absl::flat_hash_map<size_t, const Stage0Relocation*>* reloc_by_word,
    std::string* out_macro_asm, size_t* out_consumed_items) {
  auto valid_reorder_item = [&](size_t idx) -> bool {
    if (idx >= raw_items.size() || raw_items[idx].kind != AsmItemKind::kReorderInsn ||
        raw_items[idx].unswapped_from_branch) {
      return false;
    }
    return reloc_by_word == nullptr || !reloc_by_word->contains(raw_items[idx].word_index);
  };
  if (!valid_reorder_item(start_idx)) {
    return false;
  }
  const uint32_t w0 = raw_items[start_idx].word;
  if ((w0 & 0xffe0ffffu) != 0x4440f800u) {
    return false;
  }
  const uint32_t treg = Rt(w0);
  if (treg < 2u || treg > 31u) {
    return false;
  }
  size_t cur = start_idx + 1;
  if (!valid_reorder_item(cur) || raw_items[cur].word != w0) {
    return false;
  }
  ++cur;
  if (valid_reorder_item(cur) && raw_items[cur].word == 0x00000000u) {
    ++cur;
  }
  if (!valid_reorder_item(cur) || raw_items[cur].word != (0x34010003u | (treg << 21))) {
    return false;
  }
  const uint32_t w3 = raw_items[cur].word;
  ++cur;
  if (!valid_reorder_item(cur) || raw_items[cur].word != 0x38210002u) {
    return false;
  }
  const uint32_t w4 = raw_items[cur].word;
  ++cur;
  if (!valid_reorder_item(cur) || raw_items[cur].word != 0x44c1f800u) {
    return false;
  }
  const uint32_t w5 = raw_items[cur].word;
  ++cur;
  if (valid_reorder_item(cur) && raw_items[cur].word == 0x00000000u) {
    ++cur;
  }
  if (!valid_reorder_item(cur) || (raw_items[cur].word & 0xffdf003fu) != 0x46000024u) {
    return false;
  }
  const uint32_t w7 = raw_items[cur].word;
  ++cur;
  if (!valid_reorder_item(cur) || raw_items[cur].word != (0x44c0f800u | (treg << 16))) {
    return false;
  }
  const uint32_t w8 = raw_items[cur].word;
  ++cur;
  if (valid_reorder_item(cur) && raw_items[cur].word == 0x00000000u) {
    ++cur;
  }
  const uint32_t macro10[10] = {w0, w0, 0x00000000u, w3, w4, w5, 0x00000000u, w7, w8, 0x00000000u};
  const std::string macro_asm = InvertGasMacro(macro10);
  if (macro_asm.empty()) {
    return false;
  }
  *out_macro_asm = macro_asm;
  *out_consumed_items = cur - start_idx;
  return true;
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
    case 10:
      return InvertTenWords(macro_words_x);
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
      std::string trunc_macro_asm;
      size_t trunc_consumed = 0;
      if (TryCollapseMips1TruncMacroItems(raw_items, i, nullptr, &trunc_macro_asm,
                                          &trunc_consumed)) {
        item.line = std::move(trunc_macro_asm);
        item.unswapped_from_branch = false;
        item.preceded_by_active_hazard = false;
        items.push_back(std::move(item));
        i += trunc_consumed - 1;
        continue;
      }

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

      if (i + 2 < raw_items.size() && raw_items[i + 1].kind == AsmItemKind::kReorderInsn &&
          raw_items[i + 2].kind == AsmItemKind::kReorderInsn && !IsBranchOrJumpWord(item.word) &&
          !IsBranchOrJumpWord(raw_items[i + 1].word) &&
          !IsBranchOrJumpWord(raw_items[i + 2].word)) {
        const uint32_t triple[3] = {item.word, raw_items[i + 1].word, raw_items[i + 2].word};
        const std::string macro_asm = InvertGasMacro(triple);
        if (!macro_asm.empty() && absl::StartsWith(macro_asm, "li.s ")) {
          item.line = macro_asm;
          item.unswapped_from_branch = false;
          items.push_back(std::move(item));
          i += 2;
          continue;
        }
      }

      if (i + 1 < raw_items.size() && raw_items[i + 1].kind == AsmItemKind::kReorderInsn &&
          !IsBranchOrJumpWord(item.word) && !IsBranchOrJumpWord(raw_items[i + 1].word)) {
        const uint32_t pair[2] = {item.word, raw_items[i + 1].word};
        const std::string macro_asm = InvertGasMacro(pair);
        if (!macro_asm.empty() &&
            (absl::StartsWith(macro_asm, "li ") || absl::StartsWith(macro_asm, "li\t") ||
             absl::StartsWith(macro_asm, "li.s "))) {
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

std::vector<Stage0Relocation> SynthesizeLinkedRomRelocations(
    const Stage0FunctionInput& stage0_input, std::vector<Stage0JumpTable>* out_jump_tables,
    std::vector<Stage0ConstantPoolEntry>* out_constant_pool) {
  std::vector<Stage0JumpTable> norm_jts = stage0_input.jump_tables;
  std::vector<Stage0ConstantPoolEntry> norm_cps = stage0_input.constant_pool;

  if (stage0_input.base_vram == 0u || !stage0_input.relocations.empty()) {
    if (out_jump_tables != nullptr) {
      *out_jump_tables = std::move(norm_jts);
    }
    if (out_constant_pool != nullptr) {
      *out_constant_pool = std::move(norm_cps);
    }
    return stage0_input.relocations;
  }

  absl::flat_hash_set<std::string> used_sym_names;
  for (const auto& [vram, sym] : stage0_input.vram_to_symbol) {
    (void)vram;
    if (!sym.empty()) {
      used_sym_names.insert(sym);
    }
  }

  for (size_t i = 0; i < norm_jts.size(); ++i) {
    if (norm_jts[i].label_name.empty() || used_sym_names.contains(norm_jts[i].label_name)) {
      norm_jts[i].label_name = absl::StrFormat("__jt_%zu", i);
    }
    used_sym_names.insert(norm_jts[i].label_name);
  }

  auto parse_lc_num = [](std::string_view label, int* out_num) -> bool {
    if (absl::StartsWith(label, "*")) {
      label.remove_prefix(1);
    }
    if (!absl::StartsWith(label, "$LC") && !absl::StartsWith(label, ".LC")) {
      return false;
    }
    label.remove_prefix(3);
    return !label.empty() && absl::SimpleAtoi(label, out_num) && *out_num >= 0;
  };

  int next_lc_num = 0;
  int next_lit_num = 0;
  for (const Stage0ConstantPoolEntry& cp : norm_cps) {
    int lc = -1;
    if (parse_lc_num(cp.label_name, &lc)) {
      next_lc_num = std::max(next_lc_num, lc + 1);
    } else if (absl::StartsWith(cp.label_name, ".Llit")) {
      int lit = -1;
      if (absl::SimpleAtoi(cp.label_name.substr(5), &lit) && lit >= 0) {
        next_lit_num = std::max(next_lit_num, lit + 1);
      }
    }
  }
  for (Stage0ConstantPoolEntry& cp : norm_cps) {
    const bool is_lit_sec = (cp.section_name == ".lit4" || cp.section_name == ".lit8");
    int lc = -1;
    if (is_lit_sec) {
      if (!absl::StartsWith(cp.label_name, ".Llit") || used_sym_names.contains(cp.label_name)) {
        do {
          cp.label_name = absl::StrFormat(".Llit%d", next_lit_num++);
        } while (used_sym_names.contains(cp.label_name));
      }
    } else {
      if (!parse_lc_num(cp.label_name, &lc) || used_sym_names.contains(cp.label_name)) {
        do {
          cp.label_name = absl::StrFormat("$LC%d", next_lc_num++);
        } while (used_sym_names.contains(cp.label_name));
      }
    }
    used_sym_names.insert(cp.label_name);
  }

  const absl::Span<const uint32_t> full_words(stage0_input.machine_words);
  if (full_words.empty()) {
    if (out_jump_tables != nullptr) {
      *out_jump_tables = std::move(norm_jts);
    }
    if (out_constant_pool != nullptr) {
      *out_constant_pool = std::move(norm_cps);
    }
    return {};
  }

  absl::Span<const uint32_t> words = StripSectionAlignmentPadding(full_words);
  bool keep_full = false;
  for (const Stage0JumpTable& jt : norm_jts) {
    for (size_t target_idx : jt.target_word_indices) {
      if (target_idx >= words.size() && target_idx < full_words.size()) {
        keep_full = true;
        break;
      }
    }
  }
  if (!keep_full) {
    for (size_t i = 0; i < words.size();) {
      const uint32_t w = words[i];
      if (IsBranchOrJumpWord(w)) {
        if (Opcode(w) == 0x02u) {
          const uint32_t target_vram =
              (stage0_input.base_vram & 0xf0000000u) | ((w & 0x03ffffffu) << 2);
          if (target_vram >= stage0_input.base_vram + static_cast<uint32_t>(words.size()) * 4u &&
              target_vram <
                  stage0_input.base_vram + static_cast<uint32_t>(full_words.size()) * 4u) {
            keep_full = true;
            break;
          }
        }
        i += 2;
      } else {
        ++i;
      }
    }
  }
  if (keep_full) {
    words = full_words;
  }
  const size_t num_words = words.size();

  struct ResolvedSymTarget {
    std::string symbol_name;
    int64_t addend = 0;
    bool is_literal_sec = false;
  };
  absl::flat_hash_map<uint32_t, ResolvedSymTarget> exact_targets;
  absl::flat_hash_map<uint32_t, std::vector<size_t>> vram_to_jt_indices;
  for (size_t jt_idx = 0; jt_idx < norm_jts.size(); ++jt_idx) {
    const Stage0JumpTable& jt = norm_jts[jt_idx];
    if (jt.table_vram != 0u) {
      vram_to_jt_indices[jt.table_vram].push_back(jt_idx);
      exact_targets[jt.table_vram] = ResolvedSymTarget{
          .symbol_name = jt.label_name,
          .addend = 0,
          .is_literal_sec = false,
      };
    }
  }
  for (const Stage0ConstantPoolEntry& cp : norm_cps) {
    if (cp.entry_vram != 0u) {
      const bool is_lit = (cp.section_name == ".lit4" || cp.section_name == ".lit8");
      exact_targets[cp.entry_vram] = ResolvedSymTarget{
          .symbol_name = cp.label_name,
          .addend = 0,
          .is_literal_sec = is_lit,
      };
      if (cp.byte_size == 8u || cp.words.size() == 2u) {
        exact_targets.try_emplace(cp.entry_vram + 4u, ResolvedSymTarget{
                                                          .symbol_name = cp.label_name,
                                                          .addend = 4,
                                                          .is_literal_sec = is_lit,
                                                      });
      }
    }
  }
  for (const auto& [sym_vram, sym_name] : stage0_input.vram_to_symbol) {
    if (!sym_name.empty()) {
      exact_targets.try_emplace(sym_vram, ResolvedSymTarget{
                                              .symbol_name = sym_name,
                                              .addend = 0,
                                              .is_literal_sec = false,
                                          });
    }
  }

  struct SizedSymRange {
    uint32_t start_vram = 0;
    uint32_t end_vram = 0;
    std::string symbol_name;
  };
  std::vector<SizedSymRange> sized_ranges;
  for (const auto& [sym_vram, byte_size] : stage0_input.vram_to_symbol_size) {
    if (byte_size == 0u) {
      continue;
    }
    const auto sym_it = stage0_input.vram_to_symbol.find(sym_vram);
    if (sym_it == stage0_input.vram_to_symbol.end() || sym_it->second.empty()) {
      continue;
    }
    const uint64_t end64 = static_cast<uint64_t>(sym_vram) + static_cast<uint64_t>(byte_size);
    const uint32_t end_vram = end64 > 0xffffffffULL ? 0xffffffffu : static_cast<uint32_t>(end64);
    sized_ranges.push_back(SizedSymRange{
        .start_vram = sym_vram,
        .end_vram = end_vram,
        .symbol_name = sym_it->second,
    });
  }
  std::sort(
      sized_ranges.begin(), sized_ranges.end(),
      [](const SizedSymRange& a, const SizedSymRange& b) { return a.start_vram < b.start_vram; });

  auto resolve_address = [&](uint32_t addr, ResolvedSymTarget* out) -> bool {
    if (const auto it = exact_targets.find(addr); it != exact_targets.end()) {
      *out = it->second;
      return true;
    }
    for (const SizedSymRange& r : sized_ranges) {
      if (addr >= r.start_vram && addr < r.end_vram) {
        *out = ResolvedSymTarget{
            .symbol_name = r.symbol_name,
            .addend = static_cast<int64_t>(addr - r.start_vram),
            .is_literal_sec = false,
        };
        return true;
      }
    }
    return false;
  };

  struct HiDef {
    size_t lui_word_index = 0;
    uint16_t hi16 = 0;
    bool is_indexed = false;
    bool operator==(const HiDef& other) const {
      return lui_word_index == other.lui_word_index && hi16 == other.hi16 &&
             is_indexed == other.is_indexed;
    }
  };
  struct GprLattice {
    bool has_non_hi_def = false;
    std::vector<HiDef> hi_defs;
    std::vector<size_t> jt_indices;
  };
  struct WordInState {
    bool reachable = false;
    bool gp_intact = false;
    std::array<GprLattice, 32> regs;
  };

  auto append_unique_jts = [](const std::vector<size_t>& src, std::vector<size_t>* dst) {
    for (size_t jt_idx : src) {
      if (std::find(dst->begin(), dst->end(), jt_idx) == dst->end()) {
        dst->push_back(jt_idx);
      }
    }
  };

  auto kill_gpr = [](WordInState* st, uint32_t reg) {
    if (reg == 0u || reg >= 32u) {
      return;
    }
    st->regs[reg].has_non_hi_def = true;
    st->regs[reg].hi_defs.clear();
    st->regs[reg].jt_indices.clear();
    if (reg == 28u) {
      st->gp_intact = false;
    }
  };

  auto exec_word = [&](uint32_t w, size_t w_idx, WordInState* st) {
    const uint32_t op = Opcode(w);
    if (op == 0x0fu && Rs(w) == 0u) {
      const uint32_t rt = Rt(w);
      if (rt != 0u) {
        st->regs[rt].has_non_hi_def = false;
        st->regs[rt].hi_defs = {HiDef{
            .lui_word_index = w_idx,
            .hi16 = static_cast<uint16_t>(Uimm16(w)),
            .is_indexed = false,
        }};
        st->regs[rt].jt_indices.clear();
        if (rt == 28u) {
          st->gp_intact = false;
        }
      }
      return;
    }

    if (op == 0x00u && Sa(w) == 0u) {
      const uint32_t funct = Funct(w);
      const uint32_t rs = Rs(w);
      const uint32_t rt = Rt(w);
      const uint32_t rd = Rd(w);
      if ((funct == 0x21u || funct == 0x2du || funct == 0x25u) && (rs == 0u || rt == 0u)) {
        if (rd != 0u) {
          const uint32_t src = (rs != 0u) ? rs : rt;
          if (src == 0u) {
            kill_gpr(st, rd);
          } else {
            st->regs[rd] = st->regs[src];
            if (rd == 28u && src != 28u) {
              st->gp_intact = false;
            }
          }
        }
        return;
      }
      if ((funct == 0x21u || funct == 0x2du) && rs != 0u && rt != 0u) {
        if (rd != 0u) {
          const bool rs_has_jt = !st->regs[rs].jt_indices.empty();
          const bool rt_has_jt = !st->regs[rt].jt_indices.empty();
          const bool rs_is_hi = !st->regs[rs].has_non_hi_def && !st->regs[rs].hi_defs.empty();
          const bool rt_is_hi = !st->regs[rt].has_non_hi_def && !st->regs[rt].hi_defs.empty();
          if (rs != rt && (rs_has_jt != rt_has_jt)) {
            std::vector<size_t> next_jts =
                rs_has_jt ? st->regs[rs].jt_indices : st->regs[rt].jt_indices;
            kill_gpr(st, rd);
            st->regs[rd].jt_indices = std::move(next_jts);
          } else if (rs != rt && (rs_is_hi != rt_is_hi)) {
            const GprLattice& hi_src = rs_is_hi ? st->regs[rs] : st->regs[rt];
            std::vector<HiDef> indexed_defs = hi_src.hi_defs;
            for (HiDef& d : indexed_defs) {
              d.is_indexed = true;
            }
            st->regs[rd].has_non_hi_def = false;
            st->regs[rd].hi_defs = std::move(indexed_defs);
            st->regs[rd].jt_indices.clear();
            if (rd == 28u) {
              st->gp_intact = false;
            }
          } else {
            kill_gpr(st, rd);
          }
        }
        return;
      }
    }

    if (op == 0x09u || op == 0x19u) {
      const uint32_t rs = Rs(w);
      const uint32_t rt = Rt(w);
      const int16_t simm = Simm16(w);
      std::vector<size_t> next_jts;
      if (rs != 0u && rs != 28u && rs != 29u && !st->regs[rs].has_non_hi_def &&
          !st->regs[rs].hi_defs.empty()) {
        for (const HiDef& d : st->regs[rs].hi_defs) {
          if (!d.is_indexed) {
            const uint32_t addr = (static_cast<uint32_t>(d.hi16) << 16) +
                                  static_cast<uint32_t>(static_cast<int32_t>(simm));
            if (const auto jt_it = vram_to_jt_indices.find(addr);
                jt_it != vram_to_jt_indices.end()) {
              append_unique_jts(jt_it->second, &next_jts);
            }
          }
        }
      } else if (rs == 28u && st->gp_intact && stage0_input.gp_vram != 0u) {
        const uint32_t addr =
            stage0_input.gp_vram + static_cast<uint32_t>(static_cast<int32_t>(simm));
        if (const auto jt_it = vram_to_jt_indices.find(addr); jt_it != vram_to_jt_indices.end()) {
          append_unique_jts(jt_it->second, &next_jts);
        }
      } else if (rs != 0u && simm == 0 && !st->regs[rs].jt_indices.empty()) {
        next_jts = st->regs[rs].jt_indices;
      }
      kill_gpr(st, rt);
      if (rt != 0u && !next_jts.empty()) {
        st->regs[rt].jt_indices = std::move(next_jts);
      }
      return;
    }

    if (op == 0x23u || op == 0x27u || op == 0x37u) {
      const uint32_t rs = Rs(w);
      const uint32_t rt = Rt(w);
      const int16_t simm = Simm16(w);
      std::vector<size_t> next_jts;
      if (rs != 0u && !st->regs[rs].jt_indices.empty()) {
        next_jts = st->regs[rs].jt_indices;
      } else if (rs != 0u && rs != 28u && rs != 29u && !st->regs[rs].has_non_hi_def &&
                 !st->regs[rs].hi_defs.empty()) {
        for (const HiDef& d : st->regs[rs].hi_defs) {
          const uint32_t addr = (static_cast<uint32_t>(d.hi16) << 16) +
                                static_cast<uint32_t>(static_cast<int32_t>(simm));
          if (const auto jt_it = vram_to_jt_indices.find(addr); jt_it != vram_to_jt_indices.end()) {
            append_unique_jts(jt_it->second, &next_jts);
          }
        }
      } else if (rs == 28u && st->gp_intact && stage0_input.gp_vram != 0u) {
        const uint32_t addr =
            stage0_input.gp_vram + static_cast<uint32_t>(static_cast<int32_t>(simm));
        if (const auto jt_it = vram_to_jt_indices.find(addr); jt_it != vram_to_jt_indices.end()) {
          append_unique_jts(jt_it->second, &next_jts);
        }
      }
      kill_gpr(st, rt);
      if (rt != 0u && !next_jts.empty()) {
        st->regs[rt].jt_indices = std::move(next_jts);
      }
      return;
    }

    const uint32_t pinfo = GetInsnPinfo(w);
    if ((pinfo & 0x00000001u) != 0u) {
      kill_gpr(st, Rd(w));
    }
    if ((pinfo & 0x00000002u) != 0u) {
      kill_gpr(st, Rt(w));
    }
    if ((pinfo & 0x01000000u) != 0u) {
      kill_gpr(st, 31u);
    }
  };

  auto merge_state = [](const WordInState& src, WordInState* dst) -> bool {
    if (!src.reachable) {
      return false;
    }
    if (!dst->reachable) {
      *dst = src;
      return true;
    }
    bool changed = false;
    if (dst->gp_intact && !src.gp_intact) {
      dst->gp_intact = false;
      changed = true;
    }
    for (size_t r = 1; r < 32; ++r) {
      GprLattice& d_lat = dst->regs[r];
      const GprLattice& s_lat = src.regs[r];
      if (!d_lat.has_non_hi_def && s_lat.has_non_hi_def) {
        d_lat.has_non_hi_def = true;
        d_lat.hi_defs.clear();
        changed = true;
      } else if (!d_lat.has_non_hi_def) {
        for (const HiDef& def : s_lat.hi_defs) {
          if (std::find(d_lat.hi_defs.begin(), d_lat.hi_defs.end(), def) == d_lat.hi_defs.end()) {
            if (d_lat.hi_defs.size() < 16u) {
              d_lat.hi_defs.push_back(def);
              changed = true;
            } else {
              d_lat.has_non_hi_def = true;
              d_lat.hi_defs.clear();
              changed = true;
              break;
            }
          }
        }
      }
      for (size_t jt_idx : s_lat.jt_indices) {
        if (std::find(d_lat.jt_indices.begin(), d_lat.jt_indices.end(), jt_idx) ==
            d_lat.jt_indices.end()) {
          d_lat.jt_indices.push_back(jt_idx);
          changed = true;
        }
      }
    }
    return changed;
  };

  std::vector<std::optional<size_t>> delay_owner(num_words);
  for (size_t pc = 0; pc < num_words;) {
    if (IsBranchOrJumpWord(words[pc]) && pc + 1 < num_words) {
      delay_owner[pc + 1] = pc;
      pc += 2;
    } else {
      ++pc;
    }
  }

  std::vector<WordInState> in_state(num_words);
  in_state[0].reachable = true;
  in_state[0].gp_intact = (stage0_input.gp_vram != 0u);
  for (size_t r = 1; r < 32; ++r) {
    in_state[0].regs[r].has_non_hi_def = true;
  }

  std::vector<size_t> worklist = {0};
  std::vector<bool> in_queue(num_words, false);
  in_queue[0] = true;
  size_t q_head = 0;

  auto propagate = [&](size_t target_pc, const WordInState& st) {
    if (target_pc < num_words && merge_state(st, &in_state[target_pc]) && !in_queue[target_pc]) {
      in_queue[target_pc] = true;
      worklist.push_back(target_pc);
    }
  };

  while (q_head < worklist.size()) {
    const size_t pc = worklist[q_head++];
    in_queue[pc] = false;
    const uint32_t w = words[pc];
    if (IsBranchOrJumpWord(w)) {
      if (pc + 1 >= num_words) {
        continue;
      }
      const size_t d_pc = pc + 1;
      const uint32_t d_w = words[d_pc];
      const uint32_t pinfo = GetInsnPinfo(w);
      const uint32_t op = Opcode(w);

      if ((pinfo & 0x00080000u) != 0u) {
        // `INSN_COND_BRANCH_LIKELY`: delay slot executes only on the taken edge.
        WordInState taken_st = in_state[pc];
        if ((pinfo & 0x01000000u) != 0u) {
          kill_gpr(&taken_st, 31u);
        }
        exec_word(d_w, d_pc, &taken_st);
        const int64_t target_pc = static_cast<int64_t>(pc) + 1 + static_cast<int64_t>(Simm16(w));
        if (target_pc >= 0 && static_cast<size_t>(target_pc) < num_words) {
          propagate(static_cast<size_t>(target_pc), taken_st);
        }

        WordInState fall_st = in_state[pc];
        if ((pinfo & 0x01000000u) != 0u) {
          kill_gpr(&fall_st, 31u);
        }
        propagate(pc + 2, fall_st);
      } else if ((pinfo & 0x00040000u) != 0u) {
        // `INSN_COND_BRANCH_DELAY`: delay slot executes on both edges.
        WordInState after_d = in_state[pc];
        if ((pinfo & 0x01000000u) != 0u) {
          kill_gpr(&after_d, 31u);
        }
        exec_word(d_w, d_pc, &after_d);
        const int64_t target_pc = static_cast<int64_t>(pc) + 1 + static_cast<int64_t>(Simm16(w));
        if (target_pc >= 0 && static_cast<size_t>(target_pc) < num_words) {
          propagate(static_cast<size_t>(target_pc), after_d);
        }
        propagate(pc + 2, after_d);
      } else {
        // `INSN_UNCOND_BRANCH_DELAY`: `jal`, `jalr`, `j`, `jr`.
        if (op == 0x03u || (op == 0x00u && Funct(w) == 0x09u)) {
          WordInState after_call = in_state[pc];
          kill_gpr(&after_call, op == 0x03u ? 31u : Rd(w));
          exec_word(d_w, d_pc, &after_call);
          // Clobber MIPS O32/N32 caller-saved GPRs (`$1..$15`, `$24..$25`, `$31`),
          // preserving callee-saved `$s0..$s7` (`$16..$23`), `$gp`, `$sp`, `$fp`.
          for (uint32_t caller_reg = 1; caller_reg <= 15; ++caller_reg) {
            kill_gpr(&after_call, caller_reg);
          }
          kill_gpr(&after_call, 24u);
          kill_gpr(&after_call, 25u);
          kill_gpr(&after_call, 31u);
          propagate(pc + 2, after_call);
        } else if (op == 0x02u) {
          WordInState after_d = in_state[pc];
          exec_word(d_w, d_pc, &after_d);
          const uint32_t target_vram =
              (stage0_input.base_vram & 0xf0000000u) | ((w & 0x03ffffffu) << 2);
          if (target_vram >= stage0_input.base_vram &&
              ((target_vram - stage0_input.base_vram) & 3u) == 0u) {
            const size_t target_pc = (target_vram - stage0_input.base_vram) / 4u;
            propagate(target_pc, after_d);
          }
        } else if (op == 0x00u && Funct(w) == 0x08u && Rs(w) != 31u) {
          const std::vector<size_t> target_jts = in_state[pc].regs[Rs(w)].jt_indices;
          WordInState after_d = in_state[pc];
          exec_word(d_w, d_pc, &after_d);
          for (size_t jt_idx : target_jts) {
            if (jt_idx < norm_jts.size()) {
              for (size_t target_pc : norm_jts[jt_idx].target_word_indices) {
                propagate(target_pc, after_d);
              }
            }
          }
        }
      }
    } else {
      WordInState next_st = in_state[pc];
      exec_word(w, pc, &next_st);
      propagate(pc + 1, next_st);
    }
  }

  auto is_mem_lo_opcode = [](uint32_t op) -> bool {
    return (op >= 0x20u && op <= 0x2eu) || op == 0x31u || op == 0x35u || op == 0x37u ||
           op == 0x39u || op == 0x3du || op == 0x3fu;
  };
  auto is_addiu_opcode = [](uint32_t op) -> bool { return op == 0x09u || op == 0x19u; };

  struct CandidateAccess {
    size_t word_index = 0;
    uint32_t full_addr = 0;
    bool is_mem = false;
    bool is_gp_rel = false;
    std::vector<size_t> lui_word_indices;
    bool resolved = false;
    ResolvedSymTarget target;
  };
  std::vector<CandidateAccess> candidates;
  absl::flat_hash_set<uint32_t> resolved_mem_addrs;

  for (size_t w_idx = 0; w_idx < num_words; ++w_idx) {
    const uint32_t w = words[w_idx];
    if (IsBranchOrJumpWord(w)) {
      continue;
    }
    const uint32_t op = Opcode(w);
    const bool is_mem = is_mem_lo_opcode(op);
    const bool is_addiu = is_addiu_opcode(op);
    if (!is_mem && !is_addiu) {
      continue;
    }

    WordInState eff_state = in_state[w_idx];
    if (delay_owner[w_idx].has_value()) {
      const size_t br_pc = *delay_owner[w_idx];
      if (in_state[br_pc].reachable) {
        WordInState br_st = in_state[br_pc];
        const uint32_t br_w = words[br_pc];
        const uint32_t br_op = Opcode(br_w);
        if (br_op == 0x03u) {
          kill_gpr(&br_st, 31u);
        } else if (br_op == 0x00u && Funct(br_w) == 0x09u) {
          kill_gpr(&br_st, Rd(br_w));
        } else if ((GetInsnPinfo(br_w) & 0x01000000u) != 0u) {
          kill_gpr(&br_st, 31u);
        }
        merge_state(br_st, &eff_state);
      }
    }
    if (!eff_state.reachable) {
      continue;
    }

    const uint32_t rs = Rs(w);
    const int16_t simm = Simm16(w);
    if (rs == 28u && eff_state.gp_intact && stage0_input.gp_vram != 0u) {
      CandidateAccess cand;
      cand.word_index = w_idx;
      cand.full_addr = stage0_input.gp_vram + static_cast<uint32_t>(static_cast<int32_t>(simm));
      cand.is_mem = is_mem;
      cand.is_gp_rel = true;
      if (resolve_address(cand.full_addr, &cand.target)) {
        cand.resolved = true;
        if (is_mem) {
          resolved_mem_addrs.insert(cand.full_addr);
        }
      }
      candidates.push_back(std::move(cand));
    } else if (rs != 0u && rs != 28u && rs != 29u) {
      const GprLattice& lat = eff_state.regs[rs];
      if (lat.has_non_hi_def || lat.hi_defs.empty()) {
        continue;
      }
      const uint16_t common_hi16 = lat.hi_defs[0].hi16;
      bool all_agree = true;
      for (const HiDef& d : lat.hi_defs) {
        if (d.hi16 != common_hi16 || (is_addiu && d.is_indexed)) {
          all_agree = false;
          break;
        }
      }
      if (!all_agree) {
        continue;
      }
      CandidateAccess cand;
      cand.word_index = w_idx;
      cand.full_addr = (static_cast<uint32_t>(common_hi16) << 16) +
                       static_cast<uint32_t>(static_cast<int32_t>(simm));
      cand.is_mem = is_mem;
      cand.is_gp_rel = false;
      for (const HiDef& d : lat.hi_defs) {
        cand.lui_word_indices.push_back(d.lui_word_index);
      }
      if (resolve_address(cand.full_addr, &cand.target)) {
        cand.resolved = true;
        if (is_mem) {
          resolved_mem_addrs.insert(cand.full_addr);
        }
      }
      candidates.push_back(std::move(cand));
    }
  }

  // Second pass for paired 8-byte (`double` / `long long`) memory accesses at
  // `sym + 4` when `vram_to_symbol_size` was omitted on `sym` and `sym + 0` was
  // also accessed by a memory instruction in this function.
  for (CandidateAccess& cand : candidates) {
    if (cand.resolved || !cand.is_mem || cand.full_addr < 4u) {
      continue;
    }
    const uint32_t base_addr = cand.full_addr - 4u;
    if (!resolved_mem_addrs.contains(base_addr) ||
        stage0_input.vram_to_symbol_size.contains(base_addr)) {
      continue;
    }
    const auto sym_it = stage0_input.vram_to_symbol.find(base_addr);
    if (sym_it != stage0_input.vram_to_symbol.end() && !sym_it->second.empty()) {
      cand.resolved = true;
      cand.target = ResolvedSymTarget{
          .symbol_name = sym_it->second,
          .addend = 4,
          .is_literal_sec = false,
      };
    }
  }

  struct LuiUser {
    size_t word_index = 0;
    ResolvedSymTarget target;
  };
  std::vector<Stage0Relocation> relocs;
  absl::flat_hash_map<size_t, std::vector<LuiUser>> lui_users;
  for (const CandidateAccess& cand : candidates) {
    if (!cand.resolved) {
      continue;
    }
    if (cand.is_gp_rel) {
      relocs.push_back(Stage0Relocation{
          .word_index = cand.word_index,
          .kind = cand.target.is_literal_sec ? Stage0RelocKind::kMipsLiteral
                                             : Stage0RelocKind::kMipsGpRel16,
          .symbol_name = cand.target.symbol_name,
          .addend = cand.target.addend,
      });
    } else {
      relocs.push_back(Stage0Relocation{
          .word_index = cand.word_index,
          .kind = Stage0RelocKind::kMipsLo16,
          .symbol_name = cand.target.symbol_name,
          .addend = cand.target.addend,
      });
      for (size_t lui_idx : cand.lui_word_indices) {
        lui_users[lui_idx].push_back(LuiUser{
            .word_index = cand.word_index,
            .target = cand.target,
        });
      }
    }
  }

  for (const auto& [lui_idx, users] : lui_users) {
    if (users.empty()) {
      continue;
    }
    const LuiUser* best = nullptr;
    for (const LuiUser& u : users) {
      if (u.word_index > lui_idx && (best == nullptr || u.word_index < best->word_index)) {
        best = &u;
      }
    }
    if (best == nullptr) {
      for (const LuiUser& u : users) {
        if (best == nullptr || u.word_index < best->word_index) {
          best = &u;
        }
      }
    }
    relocs.push_back(Stage0Relocation{
        .word_index = lui_idx,
        .kind = Stage0RelocKind::kMipsHi16,
        .symbol_name = best->target.symbol_name,
        .addend = best->target.addend,
    });
  }

  std::stable_sort(relocs.begin(), relocs.end(),
                   [](const Stage0Relocation& a, const Stage0Relocation& b) {
                     return a.word_index < b.word_index;
                   });

  if (out_jump_tables != nullptr) {
    *out_jump_tables = std::move(norm_jts);
  }
  if (out_constant_pool != nullptr) {
    *out_constant_pool = std::move(norm_cps);
  }
  return relocs;
}

std::vector<std::string> CollapseGasMacrosAndFlattenLines(
    absl::Span<const AsmItem> raw_items, const Stage0FunctionInput& stage0_input,
    const absl::flat_hash_map<size_t, std::string>& word_to_label,
    std::vector<Stage0ResolvedJumpTable>* out_jump_tables,
    std::vector<Stage0ConstantPoolEntry>* out_constant_pool) {
  if (out_jump_tables != nullptr) {
    out_jump_tables->clear();
  }
  if (out_constant_pool != nullptr) {
    *out_constant_pool = stage0_input.constant_pool;
  }

  // If no relocations, jump tables, constant pool entries, or linked `base_vram`
  // are present, delegate to the pure-word macro collapse path.
  if (stage0_input.relocations.empty() && stage0_input.jump_tables.empty() &&
      stage0_input.constant_pool.empty() && stage0_input.base_vram == 0u &&
      stage0_input.profile.c_flags.empty()) {
    return CollapseGasMacrosAndFlattenLines(raw_items);
  }

  const bool is_linked_rom = stage0_input.base_vram != 0u && stage0_input.relocations.empty();
  std::vector<Stage0JumpTable> active_jump_tables;
  std::vector<Stage0ConstantPoolEntry> active_constant_pool;
  std::vector<Stage0Relocation> synthesized_relocs;
  if (is_linked_rom) {
    synthesized_relocs =
        SynthesizeLinkedRomRelocations(stage0_input, &active_jump_tables, &active_constant_pool);
  } else {
    active_jump_tables = stage0_input.jump_tables;
    active_constant_pool = stage0_input.constant_pool;
  }
  if (out_constant_pool != nullptr) {
    *out_constant_pool = active_constant_pool;
  }

  const absl::Span<const Stage0Relocation> active_relocs =
      is_linked_rom ? absl::MakeConstSpan(synthesized_relocs)
                    : absl::MakeConstSpan(stage0_input.relocations);
  absl::flat_hash_map<size_t, const Stage0Relocation*> reloc_by_word;
  absl::flat_hash_set<std::string> referenced_reloc_syms;
  for (const Stage0Relocation& rel : active_relocs) {
    reloc_by_word[rel.word_index] = &rel;
    if (!rel.symbol_name.empty()) {
      referenced_reloc_syms.insert(rel.symbol_name);
    }
  }

  // Build jump-table symbol -> synthetic local label (`$L<100000 + jt_idx>`) map
  // exclusively for jump tables referenced by at least one relocation.
  absl::flat_hash_map<std::string, std::string> jt_sym_to_label;
  for (size_t jt_idx = 0; jt_idx < active_jump_tables.size(); ++jt_idx) {
    const Stage0JumpTable& jt = active_jump_tables[jt_idx];
    if (!referenced_reloc_syms.contains(jt.label_name)) {
      continue;
    }
    const std::string tbl_tok = absl::StrFormat("L%zu", 100000 + jt_idx);
    jt_sym_to_label[jt.label_name] = absl::StrCat("$", tbl_tok);
    if (out_jump_tables != nullptr) {
      Stage0ResolvedJumpTable resolved;
      resolved.table_label_token = tbl_tok;
      resolved.target_labels.reserve(jt.target_word_indices.size());
      for (size_t target_w_idx : jt.target_word_indices) {
        auto it = word_to_label.find(target_w_idx);
        if (it == word_to_label.end()) {
          return {};
        }
        std::string_view lbl = it->second;
        if (absl::StartsWith(lbl, "$")) {
          lbl.remove_prefix(1);
        }
        resolved.target_labels.emplace_back(lbl);
      }
      out_jump_tables->push_back(std::move(resolved));
    }
  }

  absl::flat_hash_map<std::string, const Stage0ConstantPoolEntry*> cp_by_label;
  for (const Stage0ConstantPoolEntry& cp : active_constant_pool) {
    cp_by_label[cp.label_name] = &cp;
  }

  auto map_symbol = [&](std::string_view sym) -> std::string {
    if (auto it = jt_sym_to_label.find(sym); it != jt_sym_to_label.end()) {
      return it->second;
    }
    return std::string(sym);
  };

  auto format_sym_addend = [&](std::string_view sym, int64_t addend) -> std::string {
    const std::string base = map_symbol(sym);
    if (addend == 0) {
      return base;
    }
    if (addend > 0) {
      return absl::StrFormat("%s+%d", base, static_cast<int32_t>(addend));
    }
    return absl::StrFormat("%s%d", base, static_cast<int32_t>(addend));
  };

  auto rewrite_single_insn = [&](std::string line, uint32_t w, size_t w_idx) -> std::string {
    line = NormalizeCop1LoadStoreMnemonic(line);
    const uint32_t op = Opcode(w);
    auto rel_it = reloc_by_word.find(w_idx);
    const Stage0Relocation* rel = (rel_it != reloc_by_word.end()) ? rel_it->second : nullptr;

    if (rel != nullptr) {
      if (rel->kind == Stage0RelocKind::kMips26) {
        if (op == 0x03u) {
          return absl::StrFormat("jal %s", format_sym_addend(rel->symbol_name, rel->addend));
        }
        if (op == 0x02u && rel->symbol_name != ".text") {
          return absl::StrFormat("j %s", format_sym_addend(rel->symbol_name, rel->addend));
        }
      } else if (rel->kind == Stage0RelocKind::kMipsLiteral) {
        std::string mnem;
        std::string reg;
        int32_t off = 0;
        if (ParseAnyMemInsnWithBase(line, static_cast<int>(Rs(w)), &mnem, &reg, &off)) {
          auto cp_it = cp_by_label.find(rel->symbol_name);
          const Stage0ConstantPoolEntry* cp =
              (cp_it != cp_by_label.end()) ? cp_it->second : nullptr;
          if (cp != nullptr && rel->addend == 0) {
            if (mnem == "l.s" && cp->words.size() == 1 && absl::StartsWith(reg, "$f")) {
              const uint32_t bits = cp->words[0];
              const float fv = absl::bit_cast<float>(bits);
              if (std::isfinite(fv) && (cp->section_name == ".lit4" ||
                                        (std::abs(fv) > 1.0e-38f && std::abs(fv) < 1.0e38f))) {
                return absl::StrFormat("li.s %s,%.9e", reg, static_cast<double>(fv));
              }
            }
            if (mnem == "l.d" && cp->words.size() == 2 && absl::StartsWith(reg, "$f")) {
              const uint64_t bits =
                  (uint64_t{cp->words[0]} << 32) | static_cast<uint64_t>(cp->words[1]);
              const double dv = absl::bit_cast<double>(bits);
              if (std::isfinite(dv) && (cp->section_name == ".lit8" ||
                                        (std::abs(dv) > 1.0e-300 && std::abs(dv) < 1.0e300))) {
                return absl::StrFormat("li.d %s,%.17e", reg, dv);
              }
            }
          }
          return absl::StrFormat("%s %s,%s", mnem, reg,
                                 format_sym_addend(rel->symbol_name, rel->addend));
        }
      } else if (rel->kind == Stage0RelocKind::kMipsGpRel16) {
        if ((op == 0x09u || op == 0x19u) && Rs(w) == 28u) {
          return absl::StrFormat("%s $%u,%s", op == 0x19u ? "dla" : "la", Rt(w),
                                 format_sym_addend(rel->symbol_name, rel->addend));
        }
        std::string mnem;
        std::string reg;
        int32_t off = 0;
        if (ParseAnyMemInsnWithBase(line, static_cast<int>(Rs(w)), &mnem, &reg, &off)) {
          if (Rs(w) == 28u) {
            return absl::StrFormat("%s %s,%s", mnem, reg,
                                   format_sym_addend(rel->symbol_name, rel->addend));
          }
          return absl::StrFormat("%s %s,%s($%u)", mnem, reg,
                                 format_sym_addend(rel->symbol_name, rel->addend), Rs(w));
        }
      }
      return line;
    }

    if (is_linked_rom && (op == 0x03u || op == 0x02u)) {
      if (op == 0x02u && (absl::StartsWith(line, "j $L") || absl::StartsWith(line, "j\t$L"))) {
        return line;
      }
      const uint32_t target_vram =
          (stage0_input.base_vram & 0xf0000000u) | ((w & 0x03ffffffu) << 2);
      auto sym_it = stage0_input.vram_to_symbol.find(target_vram);
      const std::string sym = (sym_it != stage0_input.vram_to_symbol.end())
                                  ? sym_it->second
                                  : absl::StrFormat("func_%08x", target_vram);
      return absl::StrFormat("%s %s", op == 0x03u ? "jal" : "j", sym);
    }

    if (line == "jal 0x0" || line == "jal\t0x0") {
      return "jal __callee_0";
    }
    return line;
  };

  auto rewrite_uncollapsed_hilo = [&](std::string line, uint32_t w, size_t w_idx) -> std::string {
    auto rel_it = reloc_by_word.find(w_idx);
    if (rel_it == reloc_by_word.end()) {
      return line;
    }
    const Stage0Relocation* rel = rel_it->second;
    const std::string sym_expr = format_sym_addend(rel->symbol_name, rel->addend);
    if (rel->kind == Stage0RelocKind::kMipsHi16 && Opcode(w) == 0x0fu) {
      return absl::StrFormat("lui $%u,%%hi(%s)", Rt(w), sym_expr);
    }
    if (rel->kind == Stage0RelocKind::kMipsLo16) {
      const uint32_t op = Opcode(w);
      if (op == 0x09u || op == 0x19u) {
        return absl::StrFormat("%s $%u,$%u,%%lo(%s)", op == 0x19u ? "daddiu" : "addiu", Rt(w),
                               Rs(w), sym_expr);
      }
      std::string mnem;
      std::string reg;
      int32_t off = 0;
      if (ParseAnyMemInsnWithBase(line, static_cast<int>(Rs(w)), &mnem, &reg, &off)) {
        return absl::StrFormat("%s %s,%%lo(%s)($%u)", mnem, reg, sym_expr, Rs(w));
      }
    }
    return line;
  };

  const ParsedCompilerFlags parsed_flags = ParsedCompilerFlags::Parse(stage0_input.profile.c_flags);
  const int mips_isa = parsed_flags.mips_isa;

  // Pass 1: collapse relocation-free multi-word `InvertGasMacro` expansions
  // (`M_DIV_3`, `M_REM_3`, `M_DIVU_3`, `M_REMU_3`, `M_LI`) and resolve `jal` /
  // `$gp`-relative / literal relocations.
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
      std::string trunc_macro_asm;
      size_t trunc_consumed = 0;
      if (TryCollapseMips1TruncMacroItems(raw_items, i, &reloc_by_word, &trunc_macro_asm,
                                          &trunc_consumed)) {
        item.line = std::move(trunc_macro_asm);
        item.unswapped_from_branch = false;
        item.preceded_by_active_hazard = false;
        items.push_back(std::move(item));
        i += trunc_consumed - 1;
        continue;
      }

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

      if (i + 2 < raw_items.size() && raw_items[i + 1].kind == AsmItemKind::kReorderInsn &&
          raw_items[i + 2].kind == AsmItemKind::kReorderInsn && !IsBranchOrJumpWord(item.word) &&
          !IsBranchOrJumpWord(raw_items[i + 1].word) &&
          !IsBranchOrJumpWord(raw_items[i + 2].word) && !reloc_by_word.contains(item.word_index) &&
          !reloc_by_word.contains(raw_items[i + 1].word_index) &&
          !reloc_by_word.contains(raw_items[i + 2].word_index)) {
        const uint32_t triple[3] = {item.word, raw_items[i + 1].word, raw_items[i + 2].word};
        const std::string macro_asm = InvertGasMacro(triple);
        if (!macro_asm.empty() && absl::StartsWith(macro_asm, "li.s ")) {
          item.line = macro_asm;
          item.unswapped_from_branch = false;
          items.push_back(std::move(item));
          i += 2;
          continue;
        }
      }

      if (i + 1 < raw_items.size() && raw_items[i + 1].kind == AsmItemKind::kReorderInsn &&
          !IsBranchOrJumpWord(item.word) && !IsBranchOrJumpWord(raw_items[i + 1].word)) {
        if (!reloc_by_word.contains(item.word_index) &&
            !reloc_by_word.contains(raw_items[i + 1].word_index)) {
          if (mips_isa < 2) {
            const uint32_t w0 = item.word;
            const uint32_t w1 = raw_items[i + 1].word;
            const uint32_t op0 = Opcode(w0);
            const uint32_t op1 = Opcode(w1);
            if ((op0 == 0x31u || op0 == 0x39u) && op1 == op0 && Rs(w1) == Rs(w0) &&
                (Rt(w1) % 2u) == 0u && Rt(w0) == Rt(w1) + 1u && Simm16(w0) <= 0x7fff - 4 &&
                Simm16(w1) == Simm16(w0) + 4) {
              item.line = absl::StrFormat("%s $f%u,%d($%u)", op0 == 0x31u ? "l.d" : "s.d", Rt(w1),
                                          static_cast<int>(Simm16(w0)), Rs(w0));
              item.unswapped_from_branch = false;
              items.push_back(std::move(item));
              ++i;
              continue;
            }
          }
          const uint32_t pair[2] = {item.word, raw_items[i + 1].word};
          const std::string macro_asm = InvertGasMacro(pair);
          const bool is_li_int =
              absl::StartsWith(macro_asm, "li ") || absl::StartsWith(macro_asm, "li\t");
          if (!macro_asm.empty() &&
              ((is_li_int && (!parsed_flags.RunsSched1() || Rt(item.word) == 1u)) ||
               absl::StartsWith(macro_asm, "li.s "))) {
            item.line = macro_asm;
            item.unswapped_from_branch = false;
            items.push_back(std::move(item));
            ++i;
            continue;
          }
        } else if (Rs(item.word) == 28u && Rs(raw_items[i + 1].word) == 28u) {
          // Collapse MIPS1 2-word `$gp`-relative (`kMipsGpRel16` `.sdata`) or
          // `.lit8` (`kMipsLiteral`) `l.d`/`s.d`/`li.d` expansions (`lwc1`/`swc1` pair).
          auto r0_it = reloc_by_word.find(item.word_index);
          auto r1_it = reloc_by_word.find(raw_items[i + 1].word_index);
          const Stage0Relocation* r0 = (r0_it != reloc_by_word.end()) ? r0_it->second : nullptr;
          const Stage0Relocation* r1 = (r1_it != reloc_by_word.end()) ? r1_it->second : nullptr;
          if (r0 != nullptr && r1 != nullptr &&
              (r0->kind == Stage0RelocKind::kMipsGpRel16 ||
               r0->kind == Stage0RelocKind::kMipsLiteral) &&
              r1->kind == r0->kind) {
            const std::string line0 = NormalizeCop1LoadStoreMnemonic(item.line);
            const std::string line1 = NormalizeCop1LoadStoreMnemonic(raw_items[i + 1].line);
            std::string mnem0;
            std::string reg0;
            int32_t off0 = 0;
            std::string even_fpreg;
            if (ParseAnyMemInsnWithBase(line0, 28, &mnem0, &reg0, &off0) &&
                MatchPairedFpDoubleReloc(line1, 28, mnem0, reg0, r0, r1, &even_fpreg)) {
              if (r0->kind == Stage0RelocKind::kMipsLiteral && mnem0 == "l.s" && r0->addend == 0) {
                auto cp_it = cp_by_label.find(r0->symbol_name);
                const Stage0ConstantPoolEntry* cp =
                    (cp_it != cp_by_label.end()) ? cp_it->second : nullptr;
                if (cp != nullptr && cp->words.size() == 2) {
                  const uint64_t bits =
                      (uint64_t{cp->words[0]} << 32) | static_cast<uint64_t>(cp->words[1]);
                  const double dv = absl::bit_cast<double>(bits);
                  if (std::isfinite(dv) && (cp->section_name == ".lit8" ||
                                            (std::abs(dv) > 1.0e-300 && std::abs(dv) < 1.0e300))) {
                    item.line = absl::StrFormat("li.d %s,%.17e", even_fpreg, dv);
                    item.unswapped_from_branch = false;
                    items.push_back(std::move(item));
                    ++i;
                    continue;
                  }
                }
              }
              item.line = absl::StrFormat("%s %s,%s", mnem0 == "l.s" ? "l.d" : "s.d", even_fpreg,
                                          format_sym_addend(r0->symbol_name, r0->addend));
              item.unswapped_from_branch = false;
              items.push_back(std::move(item));
              ++i;
              continue;
            }
          }
        }
      }

      item.line = rewrite_single_insn(item.line, item.word, item.word_index);
    } else if (item.kind == AsmItemKind::kNoreorderBranchSeq) {
      item.line = rewrite_single_insn(item.line, item.word, item.word_index);
      item.delay_line =
          rewrite_single_insn(item.delay_line, item.delay_word, item.delay_word_index);
      item.delay_line =
          rewrite_uncollapsed_hilo(item.delay_line, item.delay_word, item.delay_word_index);
    }
    items.push_back(std::move(item));
  }

  // Pass 2: collapse adjacent GAS symbolic macros (`la`, `li.d`, direct/indexed
  // symbolic loads & stores, paired `l.d`/`s.d`) when matching GAS `tempreg`
  // rules, and rewrite remaining split `%hi`/`%lo` instructions explicitly.
  std::vector<AsmItem> collapsed;
  collapsed.reserve(items.size());
  for (size_t i = 0; i < items.size(); ++i) {
    AsmItem item = items[i];
    if (item.kind == AsmItemKind::kReorderInsn) {
      auto hi_it = reloc_by_word.find(item.word_index);
      const Stage0Relocation* rel_hi =
          (hi_it != reloc_by_word.end() && hi_it->second->kind == Stage0RelocKind::kMipsHi16 &&
           Opcode(item.word) == 0x0fu && Rs(item.word) == 0u)
              ? hi_it->second
              : nullptr;
      const int tempreg = (rel_hi != nullptr) ? static_cast<int>(Rt(item.word)) : -1;

      if (tempreg >= 1 && !item.unswapped_from_branch) {
        // 1) Indexed symbolic load/store:
        //    `lui $<tempreg>, %hi(sym+add)`
        //    `addu/daddu $<tempreg>, $<tempreg>, $<breg>`
        //    `<memop> <reg>, %lo(sym+add)($<tempreg>)`
        int base_reg = -1;
        if (i + 2 < items.size() && items[i + 1].kind == AsmItemKind::kReorderInsn &&
            items[i + 2].kind == AsmItemKind::kReorderInsn &&
            !reloc_by_word.contains(items[i + 1].word_index) &&
            ParseIndexedAddrAddu(items[i + 1].line, tempreg, &base_reg)) {
          auto lo_it = reloc_by_word.find(items[i + 2].word_index);
          const Stage0Relocation* rel_lo =
              (lo_it != reloc_by_word.end() && lo_it->second->kind == Stage0RelocKind::kMipsLo16 &&
               lo_it->second->symbol_name == rel_hi->symbol_name &&
               lo_it->second->addend == rel_hi->addend)
                  ? lo_it->second
                  : nullptr;
          std::string mnem;
          std::string reg;
          int32_t off = 0;
          if (rel_lo != nullptr &&
              ParseAnyMemInsnWithBase(items[i + 2].line, tempreg, &mnem, &reg, &off) &&
              MatchesGasMacroTempreg(mnem, reg, base_reg, tempreg)) {
            const std::string sym_expr = format_sym_addend(rel_lo->symbol_name, rel_lo->addend);
            if (i + 3 < items.size() && items[i + 3].kind == AsmItemKind::kReorderInsn) {
              auto lo2_it = reloc_by_word.find(items[i + 3].word_index);
              const Stage0Relocation* rel_lo2 =
                  (lo2_it != reloc_by_word.end()) ? lo2_it->second : nullptr;
              std::string even_fpreg;
              if (MatchPairedFpDoubleReloc(items[i + 3].line, tempreg, mnem, reg, rel_lo, rel_lo2,
                                           &even_fpreg) &&
                  !IsTempregReusedByLaterLo16(items, i + 4, tempreg, reloc_by_word)) {
                item.line = absl::StrFormat("%s %s,%s($%d)", mnem == "l.s" ? "l.d" : "s.d",
                                            even_fpreg, sym_expr, base_reg);
                item.unswapped_from_branch = false;
                collapsed.push_back(std::move(item));
                i += 3;
                continue;
              }
            }
            if (!IsTempregReusedByLaterLo16(items, i + 3, tempreg, reloc_by_word)) {
              item.line = absl::StrFormat("%s %s,%s($%d)", mnem, reg, sym_expr, base_reg);
              item.unswapped_from_branch = false;
              collapsed.push_back(std::move(item));
              i += 2;
              continue;
            }
          }
        }

        // 2) Direct symbolic load/store or `la` / `li.d`:
        if (i + 1 < items.size() && items[i + 1].kind == AsmItemKind::kReorderInsn) {
          auto lo_it = reloc_by_word.find(items[i + 1].word_index);
          const Stage0Relocation* rel_lo =
              (lo_it != reloc_by_word.end() && lo_it->second->kind == Stage0RelocKind::kMipsLo16 &&
               lo_it->second->symbol_name == rel_hi->symbol_name &&
               lo_it->second->addend == rel_hi->addend)
                  ? lo_it->second
                  : nullptr;
          if (rel_lo != nullptr) {
            const std::string sym_expr = format_sym_addend(rel_lo->symbol_name, rel_lo->addend);
            std::string mnem;
            std::string reg;
            int32_t off = 0;
            if (ParseAnyMemInsnWithBase(items[i + 1].line, tempreg, &mnem, &reg, &off) &&
                MatchesGasMacroTempreg(mnem, reg, /*breg=*/0, tempreg)) {
              auto try_recover_li_d = [&](std::string_view target_fpreg) -> std::string {
                if (tempreg != 1 || rel_lo->addend != 0 || !absl::StartsWith(target_fpreg, "$f")) {
                  return "";
                }
                auto cp_it = cp_by_label.find(rel_lo->symbol_name);
                const Stage0ConstantPoolEntry* cp =
                    (cp_it != cp_by_label.end()) ? cp_it->second : nullptr;
                if (cp != nullptr && cp->words.size() == 2 &&
                    (cp->section_name == ".rdata" || cp->section_name == ".rodata")) {
                  const uint64_t bits =
                      (uint64_t{cp->words[0]} << 32) | static_cast<uint64_t>(cp->words[1]);
                  const double dv = absl::bit_cast<double>(bits);
                  if (std::isfinite(dv) && std::abs(dv) > 1.0e-300 && std::abs(dv) < 1.0e300) {
                    return absl::StrFormat("li.d %s,%.17e", target_fpreg, dv);
                  }
                }
                return "";
              };

              // Paired 3-word `l.d` / `s.d` (`mips_isa == 1`).
              if (i + 2 < items.size() && items[i + 2].kind == AsmItemKind::kReorderInsn) {
                auto lo2_it = reloc_by_word.find(items[i + 2].word_index);
                const Stage0Relocation* rel_lo2 =
                    (lo2_it != reloc_by_word.end()) ? lo2_it->second : nullptr;
                std::string even_fpreg;
                if (MatchPairedFpDoubleReloc(items[i + 2].line, tempreg, mnem, reg, rel_lo, rel_lo2,
                                             &even_fpreg) &&
                    !IsTempregReusedByLaterLo16(items, i + 3, tempreg, reloc_by_word)) {
                  if (mnem == "l.s") {
                    if (std::string li_d_line = try_recover_li_d(even_fpreg); !li_d_line.empty()) {
                      item.line = std::move(li_d_line);
                      item.unswapped_from_branch = false;
                      collapsed.push_back(std::move(item));
                      i += 2;
                      continue;
                    }
                  }
                  item.line = absl::StrFormat("%s %s,%s", mnem == "l.s" ? "l.d" : "s.d", even_fpreg,
                                              sym_expr);
                  item.unswapped_from_branch = false;
                  collapsed.push_back(std::move(item));
                  i += 2;
                  continue;
                }
              }

              if (!IsTempregReusedByLaterLo16(items, i + 2, tempreg, reloc_by_word)) {
                // Reconstruct `li.d $f<even>, <double>` (`M_LI_DD`) for 8-byte
                // `.rdata`/`.rodata` `mips_const_double_ok` double literal loads.
                if (mnem == "l.d") {
                  if (std::string li_d_line = try_recover_li_d(reg); !li_d_line.empty()) {
                    item.line = std::move(li_d_line);
                    item.unswapped_from_branch = false;
                    collapsed.push_back(std::move(item));
                    ++i;
                    continue;
                  }
                }

                item.line = absl::StrFormat("%s %s,%s", mnem, reg, sym_expr);
                item.unswapped_from_branch = false;
                collapsed.push_back(std::move(item));
                ++i;
                continue;
              }
            }

            // `la` / `dla` (`M_LA_AB`): `lui $<tempreg>` + `addiu/daddiu $<tempreg>,$<tempreg>`.
            const uint32_t lo_op = Opcode(items[i + 1].word);
            if ((lo_op == 0x09u || lo_op == 0x19u) &&
                static_cast<int>(Rs(items[i + 1].word)) == tempreg &&
                static_cast<int>(Rt(items[i + 1].word)) == tempreg &&
                !IsTempregReusedByLaterLo16(items, i + 2, tempreg, reloc_by_word)) {
              item.line =
                  absl::StrFormat("%s $%d,%s", lo_op == 0x19u ? "dla" : "la", tempreg, sym_expr);
              item.unswapped_from_branch = false;
              collapsed.push_back(std::move(item));
              ++i;
              continue;
            }
          }
        }
      }

      // Split `%lo(sym)($breg)` paired `l.d` / `s.d` (`mips_isa == 1`).
      if (mips_isa < 2 && i + 1 < items.size() && items[i + 1].kind == AsmItemKind::kReorderInsn) {
        auto lo0_it = reloc_by_word.find(item.word_index);
        auto lo1_it = reloc_by_word.find(items[i + 1].word_index);
        const Stage0Relocation* r0 = (lo0_it != reloc_by_word.end()) ? lo0_it->second : nullptr;
        const Stage0Relocation* r1 = (lo1_it != reloc_by_word.end()) ? lo1_it->second : nullptr;
        if (r0 != nullptr && r1 != nullptr && r0->kind == Stage0RelocKind::kMipsLo16 &&
            r1->kind == Stage0RelocKind::kMipsLo16 && Rs(item.word) == Rs(items[i + 1].word)) {
          const int breg = static_cast<int>(Rs(item.word));
          std::string mnem0;
          std::string reg0;
          int32_t off0 = 0;
          std::string even_fpreg;
          if (ParseAnyMemInsnWithBase(item.line, breg, &mnem0, &reg0, &off0) &&
              MatchPairedFpDoubleReloc(items[i + 1].line, breg, mnem0, reg0, r0, r1, &even_fpreg)) {
            item.line =
                absl::StrFormat("%s %s,%%lo(%s)($%d)", mnem0 == "l.s" ? "l.d" : "s.d", even_fpreg,
                                format_sym_addend(r0->symbol_name, r0->addend), breg);
            item.unswapped_from_branch = false;
            collapsed.push_back(std::move(item));
            ++i;
            continue;
          }
        }
      }

      // Relocation-free paired `l.d` / `s.d` (`mips_isa == 1`) exposed after un-swapping `-Wa,-O2`.
      if (mips_isa < 2 && i + 1 < items.size() && items[i + 1].kind == AsmItemKind::kReorderInsn &&
          !IsBranchOrJumpWord(item.word) && !IsBranchOrJumpWord(items[i + 1].word) &&
          !reloc_by_word.contains(item.word_index) &&
          !reloc_by_word.contains(items[i + 1].word_index)) {
        const uint32_t w0 = item.word;
        const uint32_t w1 = items[i + 1].word;
        const uint32_t op0 = Opcode(w0);
        const uint32_t op1 = Opcode(w1);
        if ((op0 == 0x31u || op0 == 0x39u) && op1 == op0 && Rs(w1) == Rs(w0) &&
            (Rt(w1) % 2u) == 0u && Rt(w0) == Rt(w1) + 1u && Simm16(w0) <= 0x7fff - 4 &&
            Simm16(w1) == Simm16(w0) + 4) {
          item.line = absl::StrFormat("%s $f%u,%d($%u)", op0 == 0x31u ? "l.d" : "s.d", Rt(w1),
                                      static_cast<int>(Simm16(w0)), Rs(w0));
          item.unswapped_from_branch = false;
          collapsed.push_back(std::move(item));
          ++i;
          continue;
        }
      }

      // Not collapsed into a GAS macro: emit explicit `%hi(...)` / `%lo(...)` if relocated.
      item.line = rewrite_uncollapsed_hilo(item.line, item.word, item.word_index);
    }
    collapsed.push_back(std::move(item));
  }

  // Pass 3: flatten into assembly lines, preserving `.set noreorder` branch
  // sequences and any remaining un-swapped `[delay_item, branch_item]` pairs.
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
