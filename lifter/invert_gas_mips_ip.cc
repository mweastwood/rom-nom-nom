#include "lifter/invert_gas_mips_ip.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "absl/strings/str_format.h"

namespace rom_nom_nom {
namespace {

constexpr std::array<std::string_view, 16> kCop1CompareConditions = {
    "f",  "un",   "eq",  "ueq", "olt", "ult", "ole", "ule",
    "sf", "ngle", "seq", "ngl", "lt",  "nge", "le",  "ngt",
};

std::string InvertRType(uint32_t word_x, uint32_t rs, uint32_t rt, uint32_t rd, uint32_t sa,
                        uint32_t funct) {
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
      return rd == 31u ? absl::StrFormat("jalr $%u", rs) : absl::StrFormat("jalr $%u,$%u", rd, rs);
    case 0x0cu: {
      const uint32_t code = (word_x >> 6) & 0xfffffu;
      return code == 0u ? "syscall" : absl::StrFormat("syscall 0x%x", code);
    }
    case 0x0du: {
      const uint32_t code = (word_x >> 16) & 0x3ffu;
      return code == 0u ? "break" : absl::StrFormat("break %u", code);
    }
    case 0x0fu:
      return "sync";
    case 0x10u:
      return absl::StrFormat("mfhi $%u", rd);
    case 0x11u:
      return absl::StrFormat("mthi $%u", rs);
    case 0x12u:
      return absl::StrFormat("mflo $%u", rd);
    case 0x13u:
      return absl::StrFormat("mtlo $%u", rs);
    case 0x14u:
      return absl::StrFormat("dsllv $%u,$%u,$%u", rd, rt, rs);
    case 0x16u:
      return absl::StrFormat("dsrlv $%u,$%u,$%u", rd, rt, rs);
    case 0x17u:
      return absl::StrFormat("dsrav $%u,$%u,$%u", rd, rt, rs);
    case 0x18u:
      return absl::StrFormat("mult $%u,$%u", rs, rt);
    case 0x19u:
      return absl::StrFormat("multu $%u,$%u", rs, rt);
    case 0x1au:
      return absl::StrFormat("div $0,$%u,$%u", rs, rt);
    case 0x1bu:
      return absl::StrFormat("divu $0,$%u,$%u", rs, rt);
    case 0x1cu:
      return absl::StrFormat("dmult $%u,$%u", rs, rt);
    case 0x1du:
      return absl::StrFormat("dmultu $%u,$%u", rs, rt);
    case 0x1eu:
      return absl::StrFormat("ddiv $0,$%u,$%u", rs, rt);
    case 0x1fu:
      return absl::StrFormat("ddivu $0,$%u,$%u", rs, rt);
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
    case 0x2cu:
      return absl::StrFormat("dadd $%u,$%u,$%u", rd, rs, rt);
    case 0x2du:
      return absl::StrFormat("daddu $%u,$%u,$%u", rd, rs, rt);
    case 0x2eu:
      return absl::StrFormat("dsub $%u,$%u,$%u", rd, rs, rt);
    case 0x2fu:
      return absl::StrFormat("dsubu $%u,$%u,$%u", rd, rs, rt);
    case 0x30u:
      return absl::StrFormat("tge $%u,$%u", rs, rt);
    case 0x31u:
      return absl::StrFormat("tgeu $%u,$%u", rs, rt);
    case 0x32u:
      return absl::StrFormat("tlt $%u,$%u", rs, rt);
    case 0x33u:
      return absl::StrFormat("tltu $%u,$%u", rs, rt);
    case 0x34u:
      return absl::StrFormat("teq $%u,$%u", rs, rt);
    case 0x36u:
      return absl::StrFormat("tne $%u,$%u", rs, rt);
    case 0x38u:
      return absl::StrFormat("dsll $%u,$%u,%u", rd, rt, sa);
    case 0x3au:
      return absl::StrFormat("dsrl $%u,$%u,%u", rd, rt, sa);
    case 0x3bu:
      return absl::StrFormat("dsra $%u,$%u,%u", rd, rt, sa);
    case 0x3cu:
      return absl::StrFormat("dsll32 $%u,$%u,%u", rd, rt, sa);
    case 0x3eu:
      return absl::StrFormat("dsrl32 $%u,$%u,%u", rd, rt, sa);
    case 0x3fu:
      return absl::StrFormat("dsra32 $%u,$%u,%u", rd, rt, sa);
    default:
      return "";
  }
}

std::string InvertRegImm(uint32_t rs, uint32_t rt, int16_t simm) {
  switch (rt) {
    case 0x00u:
      return absl::StrFormat("bltz $%u,%d", rs, simm);
    case 0x01u:
      return absl::StrFormat("bgez $%u,%d", rs, simm);
    case 0x02u:
      return absl::StrFormat("bltzl $%u,%d", rs, simm);
    case 0x03u:
      return absl::StrFormat("bgezl $%u,%d", rs, simm);
    case 0x08u:
      return absl::StrFormat("tgei $%u,%d", rs, simm);
    case 0x09u:
      return absl::StrFormat("tgeiu $%u,%d", rs, simm);
    case 0x0au:
      return absl::StrFormat("tlti $%u,%d", rs, simm);
    case 0x0bu:
      return absl::StrFormat("tltiu $%u,%d", rs, simm);
    case 0x0cu:
      return absl::StrFormat("teqi $%u,%d", rs, simm);
    case 0x0eu:
      return absl::StrFormat("tnei $%u,%d", rs, simm);
    case 0x10u:
      return absl::StrFormat("bltzal $%u,%d", rs, simm);
    case 0x11u:
      return absl::StrFormat("bgezal $%u,%d", rs, simm);
    case 0x12u:
      return absl::StrFormat("bltzall $%u,%d", rs, simm);
    case 0x13u:
      return absl::StrFormat("bgezall $%u,%d", rs, simm);
    default:
      return "";
  }
}

std::string InvertCop0(uint32_t rs, uint32_t rt, uint32_t rd, uint32_t funct, int16_t simm) {
  switch (rs) {
    case 0x00u:
      return absl::StrFormat("mfc0 $%u,$%u", rt, rd);
    case 0x01u:
      return absl::StrFormat("dmfc0 $%u,$%u", rt, rd);
    case 0x02u:
      return absl::StrFormat("cfc0 $%u,$%u", rt, rd);
    case 0x04u:
      return absl::StrFormat("mtc0 $%u,$%u", rt, rd);
    case 0x05u:
      return absl::StrFormat("dmtc0 $%u,$%u", rt, rd);
    case 0x06u:
      return absl::StrFormat("ctc0 $%u,$%u", rt, rd);
    case 0x08u:
      switch (rt) {
        case 0x00u:
          return absl::StrFormat("bc0f %d", simm);
        case 0x01u:
          return absl::StrFormat("bc0t %d", simm);
        case 0x02u:
          return absl::StrFormat("bc0fl %d", simm);
        case 0x03u:
          return absl::StrFormat("bc0tl %d", simm);
        default:
          return "";
      }
    case 0x10u:
      switch (funct) {
        case 0x01u:
          return "tlbr";
        case 0x02u:
          return "tlbwi";
        case 0x06u:
          return "tlbwr";
        case 0x08u:
          return "tlbp";
        case 0x10u:
          return "rfe";
        case 0x18u:
          return "eret";
        default:
          return "";
      }
    default:
      return "";
  }
}

std::string InvertCop1(uint32_t rs, uint32_t rt, uint32_t rd, uint32_t sa, uint32_t funct,
                       int16_t simm) {
  switch (rs) {
    case 0x00u:
      return absl::StrFormat("mfc1 $%u,$f%u", rt, rd);
    case 0x01u:
      return absl::StrFormat("dmfc1 $%u,$f%u", rt, rd);
    case 0x02u:
      return absl::StrFormat("cfc1 $%u,$f%u", rt, rd);
    case 0x04u:
      return absl::StrFormat("mtc1 $%u,$f%u", rt, rd);
    case 0x05u:
      return absl::StrFormat("dmtc1 $%u,$f%u", rt, rd);
    case 0x06u:
      return absl::StrFormat("ctc1 $%u,$f%u", rt, rd);
    case 0x08u:
      switch (rt) {
        case 0x00u:
          return absl::StrFormat("bc1f %d", simm);
        case 0x01u:
          return absl::StrFormat("bc1t %d", simm);
        case 0x02u:
          return absl::StrFormat("bc1fl %d", simm);
        case 0x03u:
          return absl::StrFormat("bc1tl %d", simm);
        default:
          return "";
      }
    case 0x10u:
    case 0x11u:
    case 0x14u:
    case 0x15u: {
      std::string_view fmt = "s";
      if (rs == 0x11u) {
        fmt = "d";
      } else if (rs == 0x14u) {
        fmt = "w";
      } else if (rs == 0x15u) {
        fmt = "l";
      }
      const uint32_t fd = sa;
      const uint32_t fs = rd;
      const uint32_t ft = rt;
      if (rs == 0x14u || rs == 0x15u) {
        if (funct == 0x20u) {
          return absl::StrFormat("cvt.s.%s $f%u,$f%u", fmt, fd, fs);
        }
        if (funct == 0x21u) {
          return absl::StrFormat("cvt.d.%s $f%u,$f%u", fmt, fd, fs);
        }
        return "";
      }
      if (funct >= 0x30u && funct <= 0x3fu) {
        return absl::StrFormat("c.%s.%s $f%u,$f%u", kCop1CompareConditions[funct - 0x30u], fmt, fs,
                               ft);
      }
      switch (funct) {
        case 0x00u:
          return absl::StrFormat("add.%s $f%u,$f%u,$f%u", fmt, fd, fs, ft);
        case 0x01u:
          return absl::StrFormat("sub.%s $f%u,$f%u,$f%u", fmt, fd, fs, ft);
        case 0x02u:
          return absl::StrFormat("mul.%s $f%u,$f%u,$f%u", fmt, fd, fs, ft);
        case 0x03u:
          return absl::StrFormat("div.%s $f%u,$f%u,$f%u", fmt, fd, fs, ft);
        case 0x04u:
          return absl::StrFormat("sqrt.%s $f%u,$f%u", fmt, fd, fs);
        case 0x05u:
          return absl::StrFormat("abs.%s $f%u,$f%u", fmt, fd, fs);
        case 0x06u:
          return absl::StrFormat("mov.%s $f%u,$f%u", fmt, fd, fs);
        case 0x07u:
          return absl::StrFormat("neg.%s $f%u,$f%u", fmt, fd, fs);
        case 0x08u:
          return absl::StrFormat("round.l.%s $f%u,$f%u", fmt, fd, fs);
        case 0x09u:
          return absl::StrFormat("trunc.l.%s $f%u,$f%u", fmt, fd, fs);
        case 0x0au:
          return absl::StrFormat("ceil.l.%s $f%u,$f%u", fmt, fd, fs);
        case 0x0bu:
          return absl::StrFormat("floor.l.%s $f%u,$f%u", fmt, fd, fs);
        case 0x0cu:
          return absl::StrFormat("round.w.%s $f%u,$f%u", fmt, fd, fs);
        case 0x0du:
          return absl::StrFormat("trunc.w.%s $f%u,$f%u", fmt, fd, fs);
        case 0x0eu:
          return absl::StrFormat("ceil.w.%s $f%u,$f%u", fmt, fd, fs);
        case 0x0fu:
          return absl::StrFormat("floor.w.%s $f%u,$f%u", fmt, fd, fs);
        case 0x20u:
          return rs == 0x10u ? "" : absl::StrFormat("cvt.s.%s $f%u,$f%u", fmt, fd, fs);
        case 0x21u:
          return rs == 0x11u ? "" : absl::StrFormat("cvt.d.%s $f%u,$f%u", fmt, fd, fs);
        case 0x24u:
          return absl::StrFormat("cvt.w.%s $f%u,$f%u", fmt, fd, fs);
        case 0x25u:
          return absl::StrFormat("cvt.l.%s $f%u,$f%u", fmt, fd, fs);
        default:
          return "";
      }
    }
    default:
      return "";
  }
}

}  // namespace

std::string InvertGasMipsIp(uint32_t word_x) {
  if (word_x == 0x00000000u) {
    return "nop";
  }

  const uint32_t op = (word_x >> 26) & 0x3fu;
  const uint32_t rs = (word_x >> 21) & 0x1fu;
  const uint32_t rt = (word_x >> 16) & 0x1fu;
  const uint32_t rd = (word_x >> 11) & 0x1fu;
  const uint32_t sa = (word_x >> 6) & 0x1fu;
  const uint32_t funct = word_x & 0x3fu;
  const uint16_t uimm = static_cast<uint16_t>(word_x & 0xffffu);
  const int16_t simm = static_cast<int16_t>(uimm);

  switch (op) {
    case 0x00u:
      return InvertRType(word_x, rs, rt, rd, sa, funct);
    case 0x01u:
      return InvertRegImm(rs, rt, simm);
    case 0x02u:
      return absl::StrFormat("j 0x%x", (word_x & 0x03ffffffu) << 2);
    case 0x03u:
      return absl::StrFormat("jal 0x%x", (word_x & 0x03ffffffu) << 2);
    case 0x04u:
      return absl::StrFormat("beq $%u,$%u,%d", rs, rt, simm);
    case 0x05u:
      return absl::StrFormat("bne $%u,$%u,%d", rs, rt, simm);
    case 0x06u:
      return absl::StrFormat("blez $%u,%d", rs, simm);
    case 0x07u:
      return absl::StrFormat("bgtz $%u,%d", rs, simm);
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
    case 0x10u:
      return InvertCop0(rs, rt, rd, funct, simm);
    case 0x11u:
      return InvertCop1(rs, rt, rd, sa, funct, simm);
    case 0x14u:
      return absl::StrFormat("beql $%u,$%u,%d", rs, rt, simm);
    case 0x15u:
      return absl::StrFormat("bnel $%u,$%u,%d", rs, rt, simm);
    case 0x16u:
      return absl::StrFormat("blezl $%u,%d", rs, simm);
    case 0x17u:
      return absl::StrFormat("bgtzl $%u,%d", rs, simm);
    case 0x18u:
      return absl::StrFormat("daddi $%u,$%u,%d", rt, rs, simm);
    case 0x19u:
      return absl::StrFormat("daddiu $%u,$%u,%d", rt, rs, simm);
    case 0x1au:
      return absl::StrFormat("ldl $%u,%d($%u)", rt, simm, rs);
    case 0x1bu:
      return absl::StrFormat("ldr $%u,%d($%u)", rt, simm, rs);
    case 0x20u:
      return absl::StrFormat("lb $%u,%d($%u)", rt, simm, rs);
    case 0x21u:
      return absl::StrFormat("lh $%u,%d($%u)", rt, simm, rs);
    case 0x22u:
      return absl::StrFormat("lwl $%u,%d($%u)", rt, simm, rs);
    case 0x23u:
      return absl::StrFormat("lw $%u,%d($%u)", rt, simm, rs);
    case 0x24u:
      return absl::StrFormat("lbu $%u,%d($%u)", rt, simm, rs);
    case 0x25u:
      return absl::StrFormat("lhu $%u,%d($%u)", rt, simm, rs);
    case 0x26u:
      return absl::StrFormat("lwr $%u,%d($%u)", rt, simm, rs);
    case 0x27u:
      return absl::StrFormat("lwu $%u,%d($%u)", rt, simm, rs);
    case 0x28u:
      return absl::StrFormat("sb $%u,%d($%u)", rt, simm, rs);
    case 0x29u:
      return absl::StrFormat("sh $%u,%d($%u)", rt, simm, rs);
    case 0x2au:
      return absl::StrFormat("swl $%u,%d($%u)", rt, simm, rs);
    case 0x2bu:
      return absl::StrFormat("sw $%u,%d($%u)", rt, simm, rs);
    case 0x2cu:
      return absl::StrFormat("sdl $%u,%d($%u)", rt, simm, rs);
    case 0x2du:
      return absl::StrFormat("sdr $%u,%d($%u)", rt, simm, rs);
    case 0x2eu:
      return absl::StrFormat("swr $%u,%d($%u)", rt, simm, rs);
    case 0x2fu:
      return absl::StrFormat("cache %u,%d($%u)", rt, simm, rs);
    case 0x30u:
      return absl::StrFormat("ll $%u,%d($%u)", rt, simm, rs);
    case 0x31u:
      return absl::StrFormat("lwc1 $f%u,%d($%u)", rt, simm, rs);
    case 0x34u:
      return absl::StrFormat("lld $%u,%d($%u)", rt, simm, rs);
    case 0x35u:
      return absl::StrFormat("ldc1 $f%u,%d($%u)", rt, simm, rs);
    case 0x37u:
      return absl::StrFormat("ld $%u,%d($%u)", rt, simm, rs);
    case 0x38u:
      return absl::StrFormat("sc $%u,%d($%u)", rt, simm, rs);
    case 0x39u:
      return absl::StrFormat("swc1 $f%u,%d($%u)", rt, simm, rs);
    case 0x3cu:
      return absl::StrFormat("scd $%u,%d($%u)", rt, simm, rs);
    case 0x3du:
      return absl::StrFormat("sdc1 $f%u,%d($%u)", rt, simm, rs);
    case 0x3fu:
      return absl::StrFormat("sd $%u,%d($%u)", rt, simm, rs);
    default:
      return "";
  }
}

}  // namespace rom_nom_nom
