#include "fuzzer/differential_fuzzer.h"

#include <cstdint>
#include <vector>

#include "gtest/gtest.h"

namespace rom_nom_nom::fuzzer {
namespace {

TEST(DifferentialFuzzerTest, IdenticalCodeReportsEquivalent) {
  DifferentialFuzzer fuzzer;

  // addiu $v0, $a0, 42; jr $ra; nop
  std::vector<uint32_t> code = {
      0x2482002A,  // addiu $v0, $a0, 42
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  auto boundary_cases = DifferentialFuzzer::GenerateBoundaryCases();
  auto comparisons = fuzzer.RunBattery(code, 0x80001000, code, 0x80001000, boundary_cases);

  ASSERT_EQ(comparisons.size(), boundary_cases.size());
  for (const auto& comp : comparisons) {
    EXPECT_EQ(comp.result, EquivalenceResult::kEquivalent) << comp.failure_reason;
  }
}

TEST(DifferentialFuzzerTest, SemanticallyEquivalentWithDifferentInstructions) {
  // Target: addu $v0, $a0, $a1; jr $ra; nop (0x00851021)
  std::vector<uint32_t> target = {
      0x00851021,  // addu $v0, $a0, $a1
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };

  // Candidate: addu $v0, $a1, $a0; jr $ra; nop (0x00A41021 - swapped registers!)
  std::vector<uint32_t> candidate = {
      0x00A41021,  // addu $v0, $a1, $a0
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };

  DifferentialFuzzer fuzzer;
  auto random_cases = DifferentialFuzzer::GenerateRandomCases(50, /*seed=*/123);
  auto comparisons = fuzzer.RunBattery(target, 0x80001000, candidate, 0x80001000, random_cases);

  ASSERT_EQ(comparisons.size(), random_cases.size());
  for (const auto& comp : comparisons) {
    EXPECT_EQ(comp.result, EquivalenceResult::kEquivalent) << comp.failure_reason;
  }
}

TEST(DifferentialFuzzerTest, DetectsReturnValueMismatch) {
  // Target: addu $v0, $a0, $a1
  std::vector<uint32_t> target = {
      0x00851021,  // addu $v0, $a0, $a1
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };

  // Candidate: subu $v0, $a0, $a1 (buggy subtraction!)
  std::vector<uint32_t> candidate = {
      0x00851023,  // subu $v0, $a0, $a1
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };

  DifferentialFuzzer fuzzer;
  FuzzTestCase tc;
  tc.a0 = 10;
  tc.a1 = 5;

  DifferentialComparison comp = fuzzer.Compare(target, 0x80001000, candidate, 0x80001000, tc);

  EXPECT_EQ(comp.result, EquivalenceResult::kReturnValueMismatch);
  EXPECT_NE(comp.failure_reason.find("Return value v0 mismatch"), std::string::npos);
}

TEST(DifferentialFuzzerTest, DetectsMemoryStoreValueMismatch) {
  // Target: sw $a1, 0($a0); jr $ra; nop
  std::vector<uint32_t> target = {
      0xAC850000,  // sw $a1, 0($a0)
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };

  // Candidate: sw $a1, 4($a0); jr $ra; nop (wrong offset!)
  std::vector<uint32_t> candidate = {
      0xAC850004,  // sw $a1, 4($a0)
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };

  DifferentialFuzzer fuzzer;
  FuzzTestCase tc;
  tc.a0 = 0x80100000;
  tc.a1 = 0x12345678;

  DifferentialComparison comp = fuzzer.Compare(target, 0x80001000, candidate, 0x80001000, tc);

  EXPECT_EQ(comp.result, EquivalenceResult::kMemoryWriteMismatch);
  EXPECT_NE(comp.failure_reason.find("Write log mismatch"), std::string::npos);
}

TEST(DifferentialFuzzerTest, DetectsMissingMemoryStore) {
  // Target: sw $a1, 0($a0); sw $a2, 4($a0); jr $ra; nop
  std::vector<uint32_t> target = {
      0xAC850000,  // sw $a1, 0($a0)
      0xAC860004,  // sw $a2, 4($a0)
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };

  // Candidate: sw $a1, 0($a0); jr $ra; nop (missed second store!)
  std::vector<uint32_t> candidate = {
      0xAC850000,  // sw $a1, 0($a0)
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };

  DifferentialFuzzer fuzzer;
  FuzzTestCase tc;
  tc.a0 = 0x80100000;
  tc.a1 = 0x11111111;
  tc.a2 = 0x22222222;

  DifferentialComparison comp = fuzzer.Compare(target, 0x80001000, candidate, 0x80001000, tc);

  EXPECT_EQ(comp.result, EquivalenceResult::kMemoryWriteMismatch);
  EXPECT_NE(comp.failure_reason.find("Write log size mismatch"), std::string::npos);
}

TEST(DifferentialFuzzerTest, DetectsCalleeSavedRegisterCorruption) {
  // Target: addu $v0, $a0, $zero; jr $ra; nop
  std::vector<uint32_t> target = {
      0x00801021,  // addu $v0, $a0, $zero
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };

  // Candidate: clobbers $s0! addiu $s0, $zero, 99; addu $v0, $a0, $zero; jr $ra; nop
  std::vector<uint32_t> candidate = {
      0x24100063,  // addiu $s0, $zero, 99
      0x00801021,  // addu  $v0, $a0, $zero
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  DifferentialFuzzer fuzzer;
  FuzzTestCase tc;
  tc.a0 = 5;

  DifferentialComparison comp = fuzzer.Compare(target, 0x80001000, candidate, 0x80001000, tc);

  EXPECT_EQ(comp.result, EquivalenceResult::kCalleeSavedMismatch);
  EXPECT_NE(comp.failure_reason.find("corrupted callee-saved registers"), std::string::npos);
}

TEST(DifferentialFuzzerTest, SinglePrecisionFloatComparison) {
  DifferentialFuzzer::Options opts;
  opts.check_v0 = false;
  opts.check_f0 = true;
  DifferentialFuzzer fuzzer(opts);

  // Target: add.s $f0, $f12, $f14; jr $ra; nop
  std::vector<uint32_t> target = {
      0x460E6000,  // add.s $f0, $f12, $f14
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  // Candidate: sub.s $f0, $f12, $f14; jr $ra; nop
  std::vector<uint32_t> candidate = {
      0x460E6001,  // sub.s $f0, $f12, $f14
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  FuzzTestCase tc;
  tc.f12 = 10.0f;
  tc.f14 = 4.0f;

  DifferentialComparison comp = fuzzer.Compare(target, 0x80001000, candidate, 0x80001000, tc);

  EXPECT_EQ(comp.result, EquivalenceResult::kReturnValueMismatch);
  EXPECT_NE(comp.failure_reason.find("Return value f0 mismatch"), std::string::npos);
}

TEST(DifferentialFuzzerTest, DoublePrecisionFloatComparison) {
  DifferentialFuzzer::Options opts;
  opts.check_v0 = false;
  opts.check_f0_double = true;
  DifferentialFuzzer fuzzer(opts);

  // Target: mul.d $f0, $f12, $f14; jr $ra; nop
  std::vector<uint32_t> target = {
      0x462E6002,  // mul.d $f0, $f12, $f14
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  // Candidate identical: mul.d $f0, $f14, $f12; jr $ra; nop
  std::vector<uint32_t> candidate = {
      0x462C7002,  // mul.d $f0, $f14, $f12
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  FuzzTestCase tc;
  tc.f12_double = 12.5;
  tc.f14_double = 2.0;

  DifferentialComparison comp = fuzzer.Compare(target, 0x80001000, candidate, 0x80001000, tc);

  EXPECT_EQ(comp.result, EquivalenceResult::kEquivalent);
  EXPECT_DOUBLE_EQ(comp.target_result.f0_double, 25.0);
}

TEST(DifferentialFuzzerTest, PointerPayloadBufferFuzzing) {
  DifferentialFuzzer fuzzer;

  // Function: reads 2 words from buffer *a0, adds them, returns in $v0
  // 00: lw $t0, 0($a0)
  // 04: lw $t1, 4($a0)
  // 08: addu $v0, $t0, $t1
  // 0C: jr $ra
  // 10: nop
  std::vector<uint32_t> code = {
      0x8C880000,  // lw   $t0, 0($a0)
      0x8C890004,  // lw   $t1, 4($a0)
      0x01091021,  // addu $v0, $t0, $t1
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };

  auto pointer_cases =
      DifferentialFuzzer::GeneratePointerCases(20, /*buffer_vram=*/0x80100000, /*buffer_size=*/64);
  auto comparisons = fuzzer.RunBattery(code, 0x80001000, code, 0x80001000, pointer_cases);

  ASSERT_EQ(comparisons.size(), pointer_cases.size());
  for (const auto& comp : comparisons) {
    EXPECT_EQ(comp.result, EquivalenceResult::kEquivalent) << comp.failure_reason;
  }
}

}  // namespace
}  // namespace rom_nom_nom::fuzzer
