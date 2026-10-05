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

TEST(DifferentialFuzzerTest, ExternalCallInterceptionBothMatch) {
  DifferentialFuzzer fuzzer;

  // Target:
  // 00: addiu $sp, $sp, -24
  // 04: sw    $ra, 16($sp)
  // 08: addiu $a0, $zero, 42
  // 0C: jal   0x80050000 (external call, returns v0=0)
  // 10: addiu $a1, $zero, 10
  // 14: addu  $v0, $v0, $a1
  // 18: lw    $ra, 16($sp)
  // 1C: addiu $sp, $sp, 24
  // 20: jr    $ra
  // 24: nop
  std::vector<uint32_t> target = {
      0x27BDFFE8,  // addiu $sp, $sp, -24
      0xAFBF0010,  // sw    $ra, 16($sp)
      0x2404002A,  // addiu $a0, $zero, 42
      0x0C014000,  // jal   0x80050000
      0x2405000A,  // addiu $a1, $zero, 10
      0x00451021,  // addu  $v0, $v0, $a1
      0x8FBF0010,  // lw    $ra, 16($sp)
      0x27BD0018,  // addiu $sp, $sp, 24
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  // Candidate: identical call to 0x80050000, slight instruction order variation
  std::vector<uint32_t> candidate = {
      0x27BDFFE8,  // addiu $sp, $sp, -24
      0xAFBF0010,  // sw    $ra, 16($sp)
      0x2405000A,  // addiu $a1, $zero, 10
      0x0C014000,  // jal   0x80050000
      0x2404002A,  // addiu $a0, $zero, 42 (delay slot)
      0x00451021,  // addu  $v0, $v0, $a1
      0x8FBF0010,  // lw    $ra, 16($sp)
      0x27BD0018,  // addiu $sp, $sp, 24
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  FuzzTestCase tc;
  DifferentialComparison comp = fuzzer.Compare(target, 0x80001000, candidate, 0x80001000, tc);

  EXPECT_EQ(comp.result, EquivalenceResult::kEquivalent) << comp.failure_reason;
  EXPECT_EQ(comp.target_result.v0, 10u);
  EXPECT_EQ(comp.candidate_result.v0, 10u);
  ASSERT_EQ(comp.target_result.call_log.size(), 1u);
  ASSERT_EQ(comp.candidate_result.call_log.size(), 1u);
}

TEST(DifferentialFuzzerTest, DetectsExternalCallMismatchWhenCheckEnabled) {
  DifferentialFuzzer::Options opts;
  opts.check_external_calls = true;
  DifferentialFuzzer fuzzer(opts);

  // Target calls 0x80050000
  std::vector<uint32_t> target = {
      0x27BDFFE8,  // addiu $sp, $sp, -24
      0xAFBF0010,  // sw    $ra, 16($sp)
      0x0C014000,  // jal   0x80050000
      0x00000000,  // nop
      0x8FBF0010,  // lw    $ra, 16($sp)
      0x27BD0018,  // addiu $sp, $sp, 24
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  // Candidate calls 0x80060000 (different target address!)
  std::vector<uint32_t> candidate = {
      0x27BDFFE8,  // addiu $sp, $sp, -24
      0xAFBF0010,  // sw    $ra, 16($sp)
      0x0C018000,  // jal   0x80060000 (0x00060000 >> 2 = 0x00018000)
      0x00000000,  // nop
      0x8FBF0010,  // lw    $ra, 16($sp)
      0x27BD0018,  // addiu $sp, $sp, 24
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  FuzzTestCase tc;
  DifferentialComparison comp = fuzzer.Compare(target, 0x80001000, candidate, 0x80001000, tc);

  EXPECT_EQ(comp.result, EquivalenceResult::kCallMismatch);
  EXPECT_NE(comp.failure_reason.find("Call log mismatch"), std::string::npos);
}

TEST(DifferentialFuzzerTest, LocalStackWritesFilteredUponReturn) {
  // Target: allocates 24 bytes stack, stores $ra at 16($sp), restores and returns
  std::vector<uint32_t> target = {
      0x27BDFFE8,  // addiu $sp, $sp, -24
      0xAFBF0010,  // sw    $ra, 16($sp)
      0x8FBF0010,  // lw    $ra, 16($sp)
      0x27BD0018,  // addiu $sp, $sp, 24
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  // Candidate: allocates 32 bytes stack, stores $ra at 24($sp) and $s0 at 20($sp)
  std::vector<uint32_t> candidate = {
      0x27BDFFE0,  // addiu $sp, $sp, -32
      0xAFBF0018,  // sw    $ra, 24($sp)
      0xAFB00014,  // sw    $s0, 20($sp)
      0x8FB00014,  // lw    $s0, 20($sp)
      0x8FBF0018,  // lw    $ra, 24($sp)
      0x27BD0020,  // addiu $sp, $sp, 32
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  FuzzTestCase tc;

  // With filter_local_stack_writes = true (default), stack frame differences are ignored
  DifferentialFuzzer fuzzer_filtered;
  DifferentialComparison comp_filtered =
      fuzzer_filtered.Compare(target, 0x80001000, candidate, 0x80001000, tc);
  EXPECT_EQ(comp_filtered.result, EquivalenceResult::kEquivalent) << comp_filtered.failure_reason;

  // With filter_local_stack_writes = false, the raw store log differences are caught
  DifferentialFuzzer::Options unfiltered_opts;
  unfiltered_opts.filter_local_stack_writes = false;
  DifferentialFuzzer fuzzer_unfiltered(unfiltered_opts);
  DifferentialComparison comp_unfiltered =
      fuzzer_unfiltered.Compare(target, 0x80001000, candidate, 0x80001000, tc);
  EXPECT_EQ(comp_unfiltered.result, EquivalenceResult::kMemoryWriteMismatch);
}

TEST(DifferentialFuzzerTest, ReorderedPersistentStoresMatchInvariantly) {
  DifferentialFuzzer fuzzer;

  // Target: writes $a0 to 0x80100000, then $a1 to 0x80100004
  std::vector<uint32_t> target = {
      0x3C088010,  // lui  $t0, 0x8010
      0xAC840000,  // sw   $a0, 0($t0)
      0xAC850004,  // sw   $a1, 4($t0)
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };

  // Candidate: writes $a1 to 0x80100004, then $a0 to 0x80100000 (instruction scheduler reordered)
  std::vector<uint32_t> candidate = {
      0x3C088010,  // lui  $t0, 0x8010
      0xAC850004,  // sw   $a1, 4($t0)
      0xAC840000,  // sw   $a0, 0($t0)
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };

  FuzzTestCase tc;
  tc.a0 = 0x11111111;
  tc.a1 = 0x22222222;

  DifferentialComparison comp = fuzzer.Compare(target, 0x80001000, candidate, 0x80001000, tc);
  EXPECT_EQ(comp.result, EquivalenceResult::kEquivalent) << comp.failure_reason;
}

TEST(DifferentialFuzzerTest, MmioAndSpinloopFunctionsMatchEquivalently) {
  DifferentialFuzzer fuzzer;

  // Function: reads SP_STATUS (0xA4040010), returns 1 in $v0
  // 00: lui  $t0, 0xA404
  // 04: lw   $v0, 16($t0)
  // 08: andi $v0, $v0, 1
  // 0C: jr   $ra
  // 10: nop
  std::vector<uint32_t> code = {
      0x3C08A404,  // lui  $t0, 0xA404
      0x8D020010,  // lw   $v0, 16($t0)
      0x30420001,  // andi $v0, $v0, 1
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };

  FuzzTestCase tc;
  DifferentialComparison comp = fuzzer.Compare(code, 0x80001000, code, 0x80001000, tc);
  EXPECT_EQ(comp.result, EquivalenceResult::kEquivalent) << comp.failure_reason;
  EXPECT_EQ(comp.target_result.v0, 1u);
  EXPECT_EQ(comp.candidate_result.v0, 1u);
}

}  // namespace
}  // namespace rom_nom_nom::fuzzer
