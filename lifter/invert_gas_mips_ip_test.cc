#include "lifter/invert_gas_mips_ip.h"

#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "core/round_trip_test_cases.h"
#include "gcc_harness/function_hooks.h"
#include "gcc_harness/gcc_context.h"
#include "gcc_harness/right_inverse_verifier.h"
#include "gcc_harness/shared_corpus_trace_loader.h"
#include "gcc_harness/subfunction_inverse_provider.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

class LifterGasMipsIpInverseProvider : public gcc_harness::SubfunctionInverseProvider {
 public:
  std::string InvertGasMipsIp(uint32_t word_x) const override {
    return rom_nom_nom::InvertGasMipsIp(word_x);
  }
};

constexpr uint32_t EncodeRType(uint32_t rs, uint32_t rt, uint32_t rd, uint32_t sa, uint32_t funct) {
  return ((rs & 0x1fu) << 21) | ((rt & 0x1fu) << 16) | ((rd & 0x1fu) << 11) | ((sa & 0x1fu) << 6) |
         (funct & 0x3fu);
}

constexpr uint32_t EncodeIType(uint32_t op, uint32_t rs, uint32_t rt, uint16_t imm) {
  return ((op & 0x3fu) << 26) | ((rs & 0x1fu) << 21) | ((rt & 0x1fu) << 16) | imm;
}

constexpr uint32_t EncodeJType(uint32_t op, uint32_t target26) {
  return ((op & 0x3fu) << 26) | (target26 & 0x03ffffffu);
}

constexpr uint32_t EncodeCop1Fmt(uint32_t fmt, uint32_t ft, uint32_t fs, uint32_t fd,
                                 uint32_t funct) {
  return (0x11u << 26) | ((fmt & 0x1fu) << 21) | ((ft & 0x1fu) << 16) | ((fs & 0x1fu) << 11) |
         ((fd & 0x1fu) << 6) | (funct & 0x3fu);
}

TEST(InvertGasMipsIpTest, ExhaustiveInstructionCategoryRightInverse) {
  gcc_harness::GccContext ctx;
  gcc_harness::FunctionHooks hooks(&ctx);
  LifterGasMipsIpInverseProvider provider;
  gcc_harness::RightInverseVerifier verifier(&ctx, &hooks, &provider);

  std::vector<uint32_t> test_words;

  // 1. NOP.
  test_words.push_back(0x00000000u);

  // 2. R-type ALU (`add`, `addu`, `sub`, `subu`, `and`, `or`, `xor`, `nor`, `slt`, `sltu`,
  //    `dadd`, `daddu`, `dsub`, `dsubu`).
  for (uint32_t funct : {0x20u, 0x21u, 0x22u, 0x23u, 0x24u, 0x25u, 0x26u, 0x27u, 0x2au, 0x2bu,
                         0x2cu, 0x2du, 0x2eu, 0x2fu}) {
    test_words.push_back(EncodeRType(4, 5, 2, 0, funct));
    test_words.push_back(EncodeRType(0, 31, 16, 0, funct));
  }

  // 3. 32/64-bit shifts (`sll`, `srl`, `sra`, `sllv`, `srlv`, `srav`, `dsll`, `dsrl`, `dsra`,
  //    `dsll32`, `dsrl32`, `dsra32`, `dsllv`, `dsrlv`, `dsrav`).
  for (uint32_t funct : {0x00u, 0x02u, 0x03u, 0x38u, 0x3au, 0x3bu, 0x3cu, 0x3eu, 0x3fu}) {
    test_words.push_back(EncodeRType(0, 4, 2, 1, funct));
    test_words.push_back(EncodeRType(0, 17, 16, 31, funct));
  }
  for (uint32_t funct : {0x04u, 0x06u, 0x07u, 0x14u, 0x16u, 0x17u}) {
    test_words.push_back(EncodeRType(5, 4, 2, 0, funct));
  }

  // 4. Multiply/divide and HI/LO moves (`mult`, `multu`, `div`, `divu`, `dmult`, `dmultu`,
  //    `ddiv`, `ddivu`, `mfhi`, `mflo`, `mthi`, `mtlo`).
  for (uint32_t funct : {0x18u, 0x19u, 0x1au, 0x1bu, 0x1cu, 0x1du, 0x1eu, 0x1fu}) {
    test_words.push_back(EncodeRType(4, 5, 0, 0, funct));
  }
  test_words.push_back(EncodeRType(0, 0, 2, 0, 0x10u));  // mfhi $2
  test_words.push_back(EncodeRType(4, 0, 0, 0, 0x11u));  // mthi $4
  test_words.push_back(EncodeRType(0, 0, 3, 0, 0x12u));  // mflo $3
  test_words.push_back(EncodeRType(5, 0, 0, 0, 0x13u));  // mtlo $5

  // 5. Jumps (`jr`, `jalr`, `j`, `jal`).
  test_words.push_back(EncodeRType(31, 0, 0, 0, 0x08u));   // jr $31
  test_words.push_back(EncodeRType(25, 0, 0, 0, 0x08u));   // jr $25
  test_words.push_back(EncodeRType(25, 0, 31, 0, 0x09u));  // jalr $25
  test_words.push_back(EncodeRType(25, 0, 4, 0, 0x09u));   // jalr $4,$25
  test_words.push_back(EncodeJType(0x02u, 0x000000u));     // j 0x0
  test_words.push_back(EncodeJType(0x02u, 0x000040u));     // j 0x100
  test_words.push_back(EncodeJType(0x03u, 0x012345u));     // jal 0x48d14

  // 6. Branches and branch-likely (`beq`, `bne`, `blez`, `bgtz`, `beql`, `bnel`, `blezl`,
  //    `bgtzl`, `bltz`, `bgez`, `bltzl`, `bgezl`, `bltzal`, `bgezal`, `bltzall`, `bgezall`).
  for (uint16_t offset :
       {uint16_t{0}, uint16_t{4}, uint16_t{0x7fffu}, uint16_t{0xfffcu}, uint16_t{0x8000u}}) {
    for (uint32_t op : {0x04u, 0x05u, 0x14u, 0x15u}) {
      test_words.push_back(EncodeIType(op, 4, 5, offset));
    }
    for (uint32_t op : {0x06u, 0x07u, 0x16u, 0x17u}) {
      test_words.push_back(EncodeIType(op, 4, 0, offset));
    }
    for (uint32_t rt : {0x00u, 0x01u, 0x02u, 0x03u, 0x10u, 0x11u, 0x12u, 0x13u}) {
      test_words.push_back(EncodeIType(0x01u, 4, rt, offset));
    }
  }

  // 7. Immediate ALU (`addi`, `addiu`, `slti`, `sltiu`, `andi`, `ori`, `xori`, `lui`,
  //    `daddi`, `daddiu`).
  for (uint16_t imm : {uint16_t{0}, uint16_t{1}, uint16_t{0x1234u}, uint16_t{0x7fffu},
                       uint16_t{0x8000u}, uint16_t{0xffffu}}) {
    for (uint32_t op : {0x08u, 0x09u, 0x0au, 0x0bu, 0x0cu, 0x0du, 0x0eu, 0x18u, 0x19u}) {
      test_words.push_back(EncodeIType(op, 4, 2, imm));
    }
    test_words.push_back(EncodeIType(0x0fu, 0, 2, imm));  // lui $2,imm
  }

  // 8. Loads and stores (`lb`, `lh`, `lwl`, `lw`, `lbu`, `lhu`, `lwr`, `lwu`, `sb`, `sh`,
  //    `swl`, `sw`, `sdl`, `sdr`, `swr`, `cache`, `ll`, `lld`, `ld`, `sc`, `scd`, `sd`,
  //    `ldl`, `ldr`, `lwc1`, `ldc1`, `swc1`, `sdc1`).
  for (uint16_t offset : {uint16_t{0}, uint16_t{16}, uint16_t{0xfff0u}, uint16_t{0x8000u}}) {
    for (uint32_t op : {0x1au, 0x1bu, 0x20u, 0x21u, 0x22u, 0x23u, 0x24u, 0x25u, 0x26u, 0x27u,
                        0x28u, 0x29u, 0x2au, 0x2bu, 0x2cu, 0x2du, 0x2eu, 0x2fu, 0x30u, 0x31u,
                        0x34u, 0x35u, 0x37u, 0x38u, 0x39u, 0x3cu, 0x3du, 0x3fu}) {
      test_words.push_back(EncodeIType(op, 29, 4, offset));
    }
  }

  // 9. COP0 moves, branches, and TLB/exception instructions.
  for (uint32_t rs : {0x00u, 0x01u, 0x02u, 0x04u, 0x05u, 0x06u}) {
    test_words.push_back(EncodeIType(0x10u, rs, 4, 12 << 11));
  }
  for (uint32_t rt : {0x00u, 0x01u, 0x02u, 0x03u}) {
    test_words.push_back(EncodeIType(0x10u, 0x08u, rt, 8));
  }
  for (uint32_t funct : {0x01u, 0x02u, 0x06u, 0x08u, 0x10u, 0x18u}) {
    test_words.push_back((0x10u << 26) | (0x10u << 21) | funct);
  }

  // 10. COP1 FPU moves, branches, arithmetic, conversions, rounding, and comparisons.
  for (uint32_t rs : {0x00u, 0x01u, 0x02u, 0x04u, 0x05u, 0x06u}) {
    test_words.push_back(EncodeIType(0x11u, rs, 4, 6 << 11));
  }
  for (uint32_t rt : {0x00u, 0x01u, 0x02u, 0x03u}) {
    test_words.push_back(EncodeIType(0x11u, 0x08u, rt, 12));
    test_words.push_back(EncodeIType(0x11u, 0x08u, rt, 0xfff8u));
  }
  for (uint32_t fmt : {0x10u, 0x11u}) {
    for (uint32_t funct : {0x00u, 0x01u, 0x02u, 0x03u}) {
      test_words.push_back(EncodeCop1Fmt(fmt, 4, 2, 0, funct));
    }
    for (uint32_t funct : {0x04u, 0x05u, 0x06u, 0x07u, 0x08u, 0x09u, 0x0au, 0x0bu, 0x0cu, 0x0du,
                           0x0eu, 0x0fu, 0x24u, 0x25u}) {
      test_words.push_back(EncodeCop1Fmt(fmt, 0, 2, 0, funct));
    }
    for (uint32_t cond = 0x30u; cond <= 0x3fu; ++cond) {
      test_words.push_back(EncodeCop1Fmt(fmt, 4, 2, 0, cond));
    }
  }
  test_words.push_back(EncodeCop1Fmt(0x10u, 0, 2, 0, 0x21u));  // cvt.d.s
  test_words.push_back(EncodeCop1Fmt(0x11u, 0, 2, 0, 0x20u));  // cvt.s.d
  test_words.push_back(EncodeCop1Fmt(0x14u, 0, 2, 0, 0x20u));  // cvt.s.w
  test_words.push_back(EncodeCop1Fmt(0x14u, 0, 2, 0, 0x21u));  // cvt.d.w
  test_words.push_back(EncodeCop1Fmt(0x15u, 0, 2, 0, 0x20u));  // cvt.s.l
  test_words.push_back(EncodeCop1Fmt(0x15u, 0, 2, 0, 0x21u));  // cvt.d.l

  // 11. System calls, breakpoints, sync, and conditional traps.
  test_words.push_back(0x0000000cu);              // syscall
  test_words.push_back((0x12345u << 6) | 0x0cu);  // syscall 0x12345
  test_words.push_back(0x0000000du);              // break
  test_words.push_back((7u << 16) | 0x0du);       // break 7
  test_words.push_back(0x0000000fu);              // sync
  for (uint32_t funct : {0x30u, 0x31u, 0x32u, 0x33u, 0x34u, 0x36u}) {
    test_words.push_back(EncodeRType(4, 5, 0, 0, funct));
  }
  for (uint32_t rt : {0x08u, 0x09u, 0x0au, 0x0bu, 0x0cu, 0x0eu}) {
    test_words.push_back(EncodeIType(0x01u, 4, rt, 10));
  }

  for (uint32_t word : test_words) {
    auto res = verifier.VerifyGasMipsIpRightInverse(word);
    EXPECT_TRUE(res.matches) << "word=0x" << std::hex << word << " asm='" << InvertGasMipsIp(word)
                             << "' diff=" << res.diff_detail;
  }
}

TEST(InvertGasMipsIpTest, NegativeInvalidAndReservedOpcodesFailVerification) {
  gcc_harness::GccContext ctx;
  gcc_harness::FunctionHooks hooks(&ctx);
  LifterGasMipsIpInverseProvider provider;
  gcc_harness::RightInverseVerifier verifier(&ctx, &hooks, &provider);

  const std::vector<uint32_t> invalid_words = {
      0x00000001u,  // SPECIAL reserved funct=0x01
      0x00000005u,  // SPECIAL reserved funct=0x05
      0x00000028u,  // SPECIAL reserved funct=0x28
      0x00000039u,  // SPECIAL reserved funct=0x39
      0x04070000u,  // REGIMM reserved rt=0x07
      0x041f0000u,  // REGIMM reserved rt=0x1f
      0x41040000u,  // COP0 bc0 reserved rt=0x04
      0x4200003fu,  // COP0 cofun reserved funct=0x3f
      0x43e00000u,  // COP0 reserved rs=0x1f
      0x45040000u,  // COP1 bc1 reserved rt=0x04
      0x46000020u,  // COP1 fmt=s invalid cvt.s.s (funct=0x20)
      0x46200021u,  // COP1 fmt=d invalid cvt.d.d (funct=0x21)
      0x4600002bu,  // COP1 fmt=s reserved funct=0x2b
      0x4680003bu,  // COP1 fmt=w invalid compare funct=0x3b
      0x47e00000u,  // COP1 reserved rs=0x1f
      0x70000000u,  // Primary opcode 0x1c reserved
      0x7c000000u,  // Primary opcode 0x1f reserved
      0xd8000000u,  // Primary opcode 0x36 (ldc2) unimplemented
      0xec000000u,  // Primary opcode 0x3b (swc3) reserved
      0xf8000000u,  // Primary opcode 0x3e (sdc2) unimplemented
  };

  for (uint32_t w : invalid_words) {
    EXPECT_EQ(InvertGasMipsIp(w), "") << "word=0x" << std::hex << w;
    const auto res = verifier.VerifyGasMipsIpRightInverse(w);
    EXPECT_FALSE(res.matches) << "word=0x" << std::hex << w;
    EXPECT_EQ(res.diff_detail, "InvertGasMipsIp returned empty string");
  }

  EXPECT_EQ(hooks.RunGasMipsIp(""), 0xffffffffu);
  EXPECT_EQ(hooks.RunGasMipsIp("addiu $2"), 0xffffffffu);
  EXPECT_EQ(hooks.RunGasMipsIp("li $2,0x12345678"), 0xffffffffu);
}

class InvertGasMipsIpRoundTripCorpusTest : public ::testing::TestWithParam<RoundTripTestCase> {
 protected:
  static void SetUpTestSuite() {
    ctx = new gcc_harness::GccContext();
    hooks = new gcc_harness::FunctionHooks(ctx);
    provider = new LifterGasMipsIpInverseProvider();
    verifier = new gcc_harness::RightInverseVerifier(ctx, hooks, provider);
  }

  static void TearDownTestSuite() {
    delete verifier;
    delete provider;
    delete hooks;
    delete ctx;
  }

  static gcc_harness::GccContext* ctx;
  static gcc_harness::FunctionHooks* hooks;
  static LifterGasMipsIpInverseProvider* provider;
  static gcc_harness::RightInverseVerifier* verifier;
};

gcc_harness::GccContext* InvertGasMipsIpRoundTripCorpusTest::ctx = nullptr;
gcc_harness::FunctionHooks* InvertGasMipsIpRoundTripCorpusTest::hooks = nullptr;
LifterGasMipsIpInverseProvider* InvertGasMipsIpRoundTripCorpusTest::provider = nullptr;
gcc_harness::RightInverseVerifier* InvertGasMipsIpRoundTripCorpusTest::verifier = nullptr;

TEST_P(InvertGasMipsIpRoundTripCorpusTest, VerifiesKGasMipsIpBoundaryAndAllEmittedMachineWords) {
  const RoundTripTestCase& tc = GetParam();
  const absl::StatusOr<const gcc_harness::FunctionCompilationTrace*> cached_trace =
      gcc_harness::GetCachedSharedCorpusTrace(tc.test_name);
  ASSERT_TRUE(cached_trace.ok()) << tc.test_name << ": " << cached_trace.status();
  const gcc_harness::FunctionCompilationTrace& trace = **cached_trace;

  ctx->BeginFunction("harness_func");
  hooks->ApplyCompilationProfile(trace.stage0_input.profile);
  regclass_init();

  bool found_mips_ip_snap = false;
  for (const auto& snap : trace.subfunction_snapshots) {
    if (snap.subfunction_id != gcc_harness::SubfunctionId::kGasMipsIp) {
      continue;
    }
    found_mips_ip_snap = true;
    EXPECT_FALSE(snap.post_machine_words.empty()) << tc.test_name;
    const auto boundary_res = verifier->VerifySubfunctionBoundaryRightInverse(snap);
    EXPECT_TRUE(boundary_res.matches) << tc.test_name << ": " << boundary_res.diff_detail;
    for (uint32_t w : snap.post_machine_words) {
      const auto word_res = verifier->VerifyGasMipsIpRightInverse(w);
      EXPECT_TRUE(word_res.matches)
          << tc.test_name << " word=0x" << std::hex << w << ": " << word_res.diff_detail;
    }
  }
  EXPECT_TRUE(found_mips_ip_snap) << tc.test_name;

  EXPECT_FALSE(trace.final_machine_words.empty()) << tc.test_name;
  for (uint32_t w : trace.final_machine_words) {
    const auto word_res = verifier->VerifyGasMipsIpRightInverse(w);
    EXPECT_TRUE(word_res.matches) << tc.test_name << " final word=0x" << std::hex << w << ": "
                                  << word_res.diff_detail;
  }
}

INSTANTIATE_TEST_SUITE_P(SharedRoundTripSuite, InvertGasMipsIpRoundTripCorpusTest,
                         ::testing::ValuesIn(GetSharedRoundTripTestCases()),
                         [](const ::testing::TestParamInfo<RoundTripTestCase>& info) {
                           return info.param.test_name;
                         });

}  // namespace
}  // namespace rom_nom_nom
