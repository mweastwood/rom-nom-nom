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

TEST(DifferentialFuzzerTest, SinglePrecisionFloatNanMatchesAsEquivalent) {
  DifferentialFuzzer::Options opts;
  opts.check_v0 = false;
  opts.check_f0 = true;
  DifferentialFuzzer fuzzer(opts);

  // Target: mtc1 $a0, $f0; jr $ra; nop
  std::vector<uint32_t> target = {
      0x44840000,  // mtc1 $a0, $f0
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };

  // Candidate: mtc1 $a1, $f0; jr $ra; nop
  std::vector<uint32_t> candidate = {
      0x44850000,  // mtc1 $a1, $f0
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };

  FuzzTestCase tc;
  tc.a0 = 0x7FC00000;  // Standard quiet NaN
  tc.a1 = 0x7F800001;  // Differing NaN bit pattern

  DifferentialComparison comp = fuzzer.Compare(target, 0x80001000, candidate, 0x80001000, tc);
  EXPECT_EQ(comp.result, EquivalenceResult::kEquivalent) << comp.failure_reason;

  // Verify non-NaN still reports mismatch
  tc.a1 = 0x3F800000;  // 1.0f
  DifferentialComparison mismatch_comp =
      fuzzer.Compare(target, 0x80001000, candidate, 0x80001000, tc);
  EXPECT_EQ(mismatch_comp.result, EquivalenceResult::kReturnValueMismatch);
}

TEST(DifferentialFuzzerTest, DoublePrecisionFloatNanMatchesAsEquivalent) {
  DifferentialFuzzer::Options opts;
  opts.check_v0 = false;
  opts.check_f0_double = true;
  DifferentialFuzzer fuzzer(opts);

  // Target: div.d $f0, $f12, $f14; jr $ra; nop
  std::vector<uint32_t> target = {
      0x462E6003,  // div.d $f0, $f12, $f14
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  // Candidate: div.d $f0, $f14, $f12; jr $ra; nop
  std::vector<uint32_t> candidate = {
      0x462C7003,  // div.d $f0, $f14, $f12
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  FuzzTestCase tc;
  tc.f12_double = 0.0;
  tc.f14_double = 0.0;

  DifferentialComparison comp = fuzzer.Compare(target, 0x80001000, candidate, 0x80001000, tc);
  EXPECT_EQ(comp.result, EquivalenceResult::kEquivalent) << comp.failure_reason;
}

TEST(DifferentialFuzzerTest, DoublePrecisionArgumentCasting) {
  DifferentialFuzzer::Options opts;
  opts.check_v0 = false;
  opts.check_f0_double = true;
  DifferentialFuzzer fuzzer(opts);

  // Function: mul.d $f0, $f12, $f12; jr $ra; nop
  std::vector<uint32_t> code = {
      0x462C6002,  // mul.d $f0, $f12, $f12
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  FuzzTestCase tc;
  tc.f12 = 3.0f;
  tc.f12_double = 0.0;  // Single-precision provided, should be cast to double

  DifferentialComparison comp = fuzzer.Compare(code, 0x80001000, code, 0x80001000, tc);
  EXPECT_EQ(comp.result, EquivalenceResult::kEquivalent) << comp.failure_reason;
  EXPECT_DOUBLE_EQ(comp.target_result.f0_double, 9.0);
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

TEST(DifferentialFuzzerTest, EquivalentWithSplitMemoryStores) {
  DifferentialFuzzer fuzzer;

  // Target: stores word $a1 (0x12345678) at 0($a0)
  // 00: sw   $a1, 0($a0)
  // 04: jr   $ra
  // 08: nop
  std::vector<uint32_t> target = {
      0xAC850000,  // sw $a1, 0($a0)
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };

  // Candidate: splits word store into 4 byte stores (sb)
  // 00: srl  $t0, $a1, 24
  // 04: sb   $t0, 0($a0)
  // 08: srl  $t0, $a1, 16
  // 0C: sb   $t0, 1($a0)
  // 10: srl  $t0, $a1, 8
  // 14: sb   $t0, 2($a0)
  // 18: sb   $a1, 3($a0)
  // 1C: jr   $ra
  // 20: nop
  std::vector<uint32_t> candidate = {
      0x00054602,  // srl $t0, $a1, 24
      0xA0880000,  // sb  $t0, 0($a0)
      0x00054402,  // srl $t0, $a1, 16
      0xA0880001,  // sb  $t0, 1($a0)
      0x00054202,  // srl $t0, $a1, 8
      0xA0880002,  // sb  $t0, 2($a0)
      0xA0850003,  // sb  $a1, 3($a0)
      0x03E00008,  // jr  $ra
      0x00000000,  // nop
  };

  FuzzTestCase tc;
  tc.a0 = 0x80100000;
  tc.a1 = 0x12345678;

  DifferentialComparison comp = fuzzer.Compare(target, 0x80001000, candidate, 0x80001000, tc);
  EXPECT_EQ(comp.result, EquivalenceResult::kEquivalent) << comp.failure_reason;
}

TEST(DifferentialFuzzerTest, EquivalentWithMergedHalfwordStores) {
  DifferentialFuzzer fuzzer;

  // Target: 2 halfword stores at 0($a0) and 2($a0)
  // 00: srl  $t0, $a1, 16
  // 04: sh   $t0, 0($a0)
  // 08: sh   $a1, 2($a0)
  // 0C: jr   $ra
  // 10: nop
  std::vector<uint32_t> target = {
      0x00054402,  // srl $t0, $a1, 16
      0xA4880000,  // sh  $t0, 0($a0)
      0xA4850002,  // sh  $a1, 2($a0)
      0x03E00008,  // jr  $ra
      0x00000000,  // nop
  };

  // Candidate: 1 single word store at 0($a0)
  // 00: sw   $a1, 0($a0)
  // 04: jr   $ra
  // 08: nop
  std::vector<uint32_t> candidate = {
      0xAC850000,  // sw  $a1, 0($a0)
      0x03E00008,  // jr  $ra
      0x00000000,  // nop
  };

  FuzzTestCase tc;
  tc.a0 = 0x80100000;
  tc.a1 = 0xDEADBEEF;

  DifferentialComparison comp = fuzzer.Compare(target, 0x80001000, candidate, 0x80001000, tc);
  EXPECT_EQ(comp.result, EquivalenceResult::kEquivalent) << comp.failure_reason;
}

TEST(DifferentialFuzzerTest, DetectsMemoryStoreByteMismatchWithDifferentWriteCounts) {
  DifferentialFuzzer fuzzer;

  // Target: stores word 0x12345678 at 0($a0)
  std::vector<uint32_t> target = {
      0xAC850000,  // sw $a1, 0($a0)
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };

  // Candidate: stores byte 0x99 instead of 0x78 at 3($a0)
  std::vector<uint32_t> candidate = {
      0x00054602,  // srl $t0, $a1, 24
      0xA0880000,  // sb  $t0, 0($a0)
      0x00054402,  // srl $t0, $a1, 16
      0xA0880001,  // sb  $t0, 1($a0)
      0x00054202,  // srl $t0, $a1, 8
      0xA0880002,  // sb  $t0, 2($a0)
      0x24080099,  // addiu $t0, $zero, 0x99 (mismatched byte!)
      0xA0880003,  // sb  $t0, 3($a0)
      0x03E00008,  // jr  $ra
      0x00000000,  // nop
  };

  FuzzTestCase tc;
  tc.a0 = 0x80100000;
  tc.a1 = 0x12345678;

  DifferentialComparison comp = fuzzer.Compare(target, 0x80001000, candidate, 0x80001000, tc);
  EXPECT_EQ(comp.result, EquivalenceResult::kMemoryWriteMismatch);
  EXPECT_NE(comp.failure_reason.find("Write log size mismatch"), std::string::npos);
}

TEST(DifferentialFuzzerTest, PassesStackArguments) {
  DifferentialFuzzer fuzzer;

  // Target: loads args 4 and 5 from stack (sp + 16, sp + 20) and adds all 6 args
  // 00: lw   $t0, 16($sp)
  // 04: lw   $t1, 20($sp)
  // 08: addu $v0, $a0, $a1
  // 0C: addu $v0, $v0, $a2
  // 10: addu $v0, $v0, $a3
  // 14: addu $v0, $v0, $t0
  // 18: addu $v0, $v0, $t1
  // 1C: jr   $ra
  // 20: nop
  std::vector<uint32_t> target = {
      0x8FA80010,  // lw   $t0, 16($sp)
      0x8FA90014,  // lw   $t1, 20($sp)
      0x00851021,  // addu $v0, $a0, $a1
      0x00461021,  // addu $v0, $v0, $a2
      0x00471021,  // addu $v0, $v0, $a3
      0x00481021,  // addu $v0, $v0, $t0
      0x00491021,  // addu $v0, $v0, $t1
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };

  // Candidate: computes the same sum with swapped temporary registers
  std::vector<uint32_t> candidate = {
      0x8FA90010,  // lw   $t1, 16($sp)
      0x8FA80014,  // lw   $t0, 20($sp)
      0x00851021,  // addu $v0, $a0, $a1
      0x00461021,  // addu $v0, $v0, $a2
      0x00471021,  // addu $v0, $v0, $a3
      0x00491021,  // addu $v0, $v0, $t1
      0x00481021,  // addu $v0, $v0, $t0
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };

  FuzzTestCase tc;
  tc.a0 = 10;
  tc.a1 = 20;
  tc.a2 = 30;
  tc.a3 = 40;
  tc.stack_args = {50, 60};

  DifferentialComparison comp = fuzzer.Compare(target, 0x80001000, candidate, 0x80001000, tc);
  EXPECT_EQ(comp.result, EquivalenceResult::kEquivalent) << comp.failure_reason;
  EXPECT_EQ(comp.target_result.v0, 210u);
  EXPECT_EQ(comp.candidate_result.v0, 210u);
}

TEST(DifferentialFuzzerTest, DetectsStackArgumentMismatch) {
  DifferentialFuzzer fuzzer;

  // Target: loads arg 4 from 16($sp)
  std::vector<uint32_t> target = {
      0x8FA80010,  // lw   $t0, 16($sp)
      0x00841021,  // addu $v0, $a0, $a0
      0x00481021,  // addu $v0, $v0, $t0
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };

  // Candidate: incorrectly loads arg 5 from 20($sp)
  std::vector<uint32_t> candidate = {
      0x8FA80014,  // lw   $t0, 20($sp)
      0x00841021,  // addu $v0, $a0, $a0
      0x00481021,  // addu $v0, $v0, $t0
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };

  FuzzTestCase tc;
  tc.a0 = 5;
  tc.stack_args = {100, 200};

  DifferentialComparison comp = fuzzer.Compare(target, 0x80001000, candidate, 0x80001000, tc);
  EXPECT_EQ(comp.result, EquivalenceResult::kReturnValueMismatch);
  EXPECT_NE(comp.failure_reason.find("Return value v0 mismatch"), std::string::npos);
}

TEST(DifferentialFuzzerTest, RandomCasesIncludeStackArgs) {
  auto cases = DifferentialFuzzer::GenerateRandomCases(10, /*seed=*/42);
  ASSERT_EQ(cases.size(), 10u);
  bool any_non_zero = false;
  for (const auto& tc : cases) {
    EXPECT_EQ(tc.stack_args.size(), 12u);
    for (uint32_t val : tc.stack_args) {
      if (val != 0) {
        any_non_zero = true;
      }
    }
  }
  EXPECT_TRUE(any_non_zero);
}

TEST(DifferentialFuzzerTest, HighMemoryIsolationDefaults) {
  DifferentialFuzzer::Options opts;
  EXPECT_EQ(opts.initial_sp, 0x807FE000u);

  auto pointer_cases = DifferentialFuzzer::GeneratePointerCases(5);
  ASSERT_EQ(pointer_cases.size(), 5u);
  for (const auto& tc : pointer_cases) {
    EXPECT_GE(tc.a0, 0x80500000u);
    EXPECT_LT(tc.a0, 0x80500000u + 256);
  }
}

TEST(DifferentialFuzzerTest, RandomCasesAlternateSingleAndDoubleFp) {
  auto cases = DifferentialFuzzer::GenerateRandomCases(10, /*seed=*/123);
  ASSERT_EQ(cases.size(), 10u);
  for (size_t i = 0; i < cases.size(); ++i) {
    if (i % 2 == 0) {
      EXPECT_DOUBLE_EQ(cases[i].f12_double, 0.0);
      EXPECT_DOUBLE_EQ(cases[i].f14_double, 0.0);
    } else {
      EXPECT_NE(cases[i].f12_double, 0.0);
      EXPECT_NE(cases[i].f14_double, 0.0);
    }
  }
}

TEST(DifferentialFuzzerTest, LocalStackWritesFilteredCorrectly) {
  DifferentialFuzzer::Options opts;
  opts.check_v0 = false;
  opts.check_memory_writes = true;
  opts.filter_local_stack_writes = true;
  DifferentialFuzzer fuzzer(opts);

  // Target writes to stack (filtered) and global memory at 0x80200000
  // sw $a0, -16($sp)
  // sw $a1, 0($a2)
  // jr $ra; nop
  std::vector<uint32_t> target = {
      0xAFA4FFF0,  // sw $a0, -16($sp)
      0xAC850000,  // sw $a1, 0($a0)
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };

  // Candidate writes different value to stack (should be filtered), but same to global memory
  // sw $zero, -16($sp)
  // sw $a1, 0($a2)
  // jr $ra; nop
  std::vector<uint32_t> candidate = {
      0xAFA0FFF0,  // sw $zero, -16($sp)
      0xAC850000,  // sw $a1, 0($a0)
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };

  FuzzTestCase tc;
  tc.a0 = 0x80200000;
  tc.a1 = 0x12345678;

  DifferentialComparison comp = fuzzer.Compare(target, 0x80001000, candidate, 0x80001000, tc);
  EXPECT_EQ(comp.result, EquivalenceResult::kEquivalent) << comp.failure_reason;
}

TEST(DifferentialFuzzerTest, WriteMismatchReportsSurroundingSampleContext) {
  DifferentialFuzzer::Options opts;
  opts.check_v0 = false;
  opts.check_memory_writes = true;
  DifferentialFuzzer fuzzer(opts);

  // Target writes 0x11111111 to 0x80200000
  std::vector<uint32_t> target = {
      0xAC850000,  // sw $a1, 0($a0)
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };

  // Candidate writes 0x22222222 to 0x80200000
  std::vector<uint32_t> candidate = {
      0xAC860000,  // sw $a2, 0($a0)
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };

  FuzzTestCase tc;
  tc.a0 = 0x80200000;
  tc.a1 = 0x11111111;
  tc.a2 = 0x22222222;

  DifferentialComparison comp = fuzzer.Compare(target, 0x80001000, candidate, 0x80001000, tc);
  EXPECT_EQ(comp.result, EquivalenceResult::kMemoryWriteMismatch);
  EXPECT_NE(comp.failure_reason.find("Write log mismatch at store #0:"), std::string::npos);
  EXPECT_NE(comp.failure_reason.find("#0: target(0x80200000"), std::string::npos);
}

TEST(DifferentialFuzzerTest, InitialMemoryPayloadsLoadedAndRestored) {
  DifferentialFuzzer::Options opts;
  opts.check_v0 = true;
  opts.check_memory_writes = false;

  MemoryPayload target_payload;
  target_payload.address = 0x80200000;
  target_payload.data = {0x12, 0x34, 0x56, 0x78};
  opts.target_initial_memory.push_back(target_payload);

  MemoryPayload candidate_payload;
  candidate_payload.address = 0x80200000;
  candidate_payload.data = {0x12, 0x34, 0x56, 0x78};
  opts.candidate_initial_memory.push_back(candidate_payload);

  DifferentialFuzzer fuzzer(opts);

  // lw $v0, 0($a0); jr $ra; nop
  std::vector<uint32_t> code = {
      0x8C820000,  // lw $v0, 0($a0)
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };

  FuzzTestCase tc;
  tc.a0 = 0x80200000;

  DifferentialComparison comp = fuzzer.Compare(code, 0x80001000, code, 0x80001000, tc);
  EXPECT_EQ(comp.result, EquivalenceResult::kEquivalent) << comp.failure_reason;
  EXPECT_EQ(comp.target_result.v0, 0x12345678u);
  EXPECT_EQ(comp.candidate_result.v0, 0x12345678u);
}

TEST(DifferentialFuzzerTest, TargetAndCandidateReadHooks) {
  DifferentialFuzzer::Options opts;
  opts.check_v0 = true;
  opts.check_memory_writes = false;
  opts.target_read_hook = [](uint32_t addr, size_t size,
                             bool /*is_fp*/) -> std::optional<uint64_t> {
    if (addr == 0x80300000 && size == 4) return 0xCAFEBABEu;
    return std::nullopt;
  };
  opts.candidate_read_hook = [](uint32_t addr, size_t size,
                                bool /*is_fp*/) -> std::optional<uint64_t> {
    if (addr == 0x80300000 && size == 4) return 0xCAFEBABEu;
    return std::nullopt;
  };

  DifferentialFuzzer fuzzer(opts);

  // lw $v0, 0($a0); jr $ra; nop
  std::vector<uint32_t> code = {
      0x8C820000,  // lw $v0, 0($a0)
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };

  FuzzTestCase tc;
  tc.a0 = 0x80300000;

  DifferentialComparison comp = fuzzer.Compare(code, 0x80001000, code, 0x80001000, tc);
  EXPECT_EQ(comp.result, EquivalenceResult::kEquivalent) << comp.failure_reason;
  EXPECT_EQ(comp.target_result.v0, 0xCAFEBABEu);
  EXPECT_EQ(comp.candidate_result.v0, 0xCAFEBABEu);
}

TEST(DifferentialFuzzerTest, BothFaultedReportsEquivalentInError) {
  // Target: loads from invalid address 0x0
  std::vector<uint32_t> target = {
      0x8C020000,  // lw $v0, 0($zero)
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };

  // Candidate: loads from invalid address 0x4
  std::vector<uint32_t> candidate = {
      0x8C020004,  // lw $v0, 4($zero)
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };

  DifferentialFuzzer fuzzer;
  FuzzTestCase tc;
  DifferentialComparison comp = fuzzer.Compare(target, 0x80001000, candidate, 0x80001000, tc);
  EXPECT_EQ(comp.result, EquivalenceResult::kEquivalent) << comp.failure_reason;
}

TEST(DifferentialFuzzerTest, TemporaryRegistersResetToZeroBetweenVectors) {
  // Target: addu $v0, $t0, $zero; jr $ra; nop (returns initial $t0)
  std::vector<uint32_t> target = {
      0x01001021,  // addu $v0, $t0, $zero
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };

  DifferentialFuzzer fuzzer;
  FuzzTestCase tc;
  auto comp = fuzzer.Compare(target, 0x80001000, target, 0x80001000, tc);
  EXPECT_EQ(comp.result, EquivalenceResult::kEquivalent) << comp.failure_reason;
  EXPECT_EQ(comp.target_result.v0, 0u);
  EXPECT_EQ(comp.candidate_result.v0, 0u);
}

TEST(DifferentialFuzzerTest, DetectsCalleeSavedZeroClearingCorruption) {
  // Target: jr $ra; nop (preserves callee-saved registers)
  std::vector<uint32_t> target = {
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };

  // Candidate: zeroes $s0 (move $s0, $zero) and returns
  std::vector<uint32_t> candidate = {
      0x00008021,  // move $s0, $zero (addu $s0, $zero, $zero)
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };

  DifferentialFuzzer fuzzer;
  FuzzTestCase tc;
  auto comp = fuzzer.Compare(target, 0x80001000, candidate, 0x80001000, tc);
  EXPECT_EQ(comp.result, EquivalenceResult::kCalleeSavedMismatch);
  EXPECT_NE(comp.failure_reason.find("corrupted callee-saved registers:"), std::string::npos);
  EXPECT_NE(comp.failure_reason.find("s0: init=0xcafe0010 curr=0x0"), std::string::npos)
      << comp.failure_reason;
}

TEST(DifferentialFuzzerTest, ReportsDetailedCalleeSavedRegistersOnCorruption) {
  // Target: jr $ra; nop (preserves callee-saved registers)
  std::vector<uint32_t> target = {
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };

  // Candidate: modifies $s1 and $sp
  // addiu $s1, $zero, 0x1234
  // addiu $sp, $sp, -32
  // jr $ra; nop
  std::vector<uint32_t> candidate = {
      0x24111234,  // addiu $s1, $zero, 0x1234
      0x27BDFFE0,  // addiu $sp, $sp, -32
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  DifferentialFuzzer fuzzer;
  FuzzTestCase tc;
  auto comp = fuzzer.Compare(target, 0x80001000, candidate, 0x80001000, tc);
  EXPECT_EQ(comp.result, EquivalenceResult::kCalleeSavedMismatch);
  EXPECT_NE(comp.failure_reason.find("s1: init=0xcafe0011 curr=0x1234"), std::string::npos)
      << comp.failure_reason;
  EXPECT_NE(comp.failure_reason.find("sp: init=0x807fe000 curr=0x807fdfe0"), std::string::npos)
      << comp.failure_reason;
}

TEST(DifferentialFuzzerTest, RetriesWithHigherStepLimitWhenCandidateExceedsLimit) {
  // Target: li $v0, 0; jr $ra; nop (3 steps)
  std::vector<uint32_t> target = {
      0x24020000,  // li   $v0, 0
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };

  // Candidate: li $t0, 15; loop: addiu $t0, $t0, -1; bnez $t0, loop; nop; li $v0, 0; jr $ra; nop
  // Loop takes 15 iterations * 2 instructions = 30 steps + entry/exit = ~35 steps.
  std::vector<uint32_t> candidate = {
      0x2408000F,  // li    $t0, 15
      0x2508FFFF,  // loop: addiu $t0, $t0, -1
      0x1500FFFE,  //       bne   $t0, $zero, loop
      0x00000000,  //       nop
      0x24020000,  //       li    $v0, 0
      0x03E00008,  //       jr    $ra
      0x00000000,  //       nop
  };

  DifferentialFuzzer::Options options;
  options.max_steps_per_run =
      20;  // candidate fails in 20 steps, but succeeds in 20 * 5 = 100 steps
  DifferentialFuzzer fuzzer(options);
  FuzzTestCase tc;
  auto comp = fuzzer.Compare(target, 0x80001000, candidate, 0x80001000, tc);
  EXPECT_EQ(comp.result, EquivalenceResult::kEquivalent) << comp.failure_reason;
  EXPECT_EQ(comp.candidate_result.status, ExecutionStatus::kHaltedReturn);
}

TEST(DifferentialFuzzerTest, RetriesWithHigherStepLimitWhenTargetExceedsLimit) {
  // Target: loop taking ~35 steps
  std::vector<uint32_t> target = {
      0x2408000F,  // li    $t0, 15
      0x2508FFFF,  // loop: addiu $t0, $t0, -1
      0x1500FFFE,  //       bne   $t0, $zero, loop
      0x00000000,  //       nop
      0x24020000,  //       li    $v0, 0
      0x03E00008,  //       jr    $ra
      0x00000000,  //       nop
  };

  // Candidate: li $v0, 0; jr $ra; nop (3 steps)
  std::vector<uint32_t> candidate = {
      0x24020000,  // li   $v0, 0
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };

  DifferentialFuzzer::Options options;
  options.max_steps_per_run = 20;
  DifferentialFuzzer fuzzer(options);
  FuzzTestCase tc;
  auto comp = fuzzer.Compare(target, 0x80001000, candidate, 0x80001000, tc);
  EXPECT_EQ(comp.result, EquivalenceResult::kEquivalent) << comp.failure_reason;
  EXPECT_EQ(comp.target_result.status, ExecutionStatus::kHaltedReturn);
}

TEST(DifferentialFuzzerTest, ExecutionStatusMismatchIncludesStepsAndV0) {
  // Target: li $v0, 42; jr $ra; nop
  std::vector<uint32_t> target = {
      0x2402002A,  // li   $v0, 42
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };

  // Candidate: teq $zero, $zero (traps / kIntegerOverflow or exception)
  std::vector<uint32_t> candidate = {
      0x00000034,  // teq  $zero, $zero
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };

  DifferentialFuzzer fuzzer;
  FuzzTestCase tc;
  auto comp = fuzzer.Compare(target, 0x80001000, candidate, 0x80001000, tc);
  EXPECT_EQ(comp.result, EquivalenceResult::kStatusMismatch);
  EXPECT_NE(comp.failure_reason.find("steps="), std::string::npos) << comp.failure_reason;
  EXPECT_NE(comp.failure_reason.find("v0=0x2a"), std::string::npos) << comp.failure_reason;
}

TEST(DifferentialFuzzerTest, MapsCandidateReturnValueToTargetAddress) {
  // Target returns 0x80100000
  std::vector<uint32_t> target = {
      0x3C028010,  // lui  $v0, 0x8010
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };

  // Candidate returns 0x80200000
  std::vector<uint32_t> candidate = {
      0x3C028020,  // lui  $v0, 0x8020
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };

  DifferentialFuzzer default_fuzzer;
  FuzzTestCase tc;
  auto unmapped_comp = default_fuzzer.Compare(target, 0x80001000, candidate, 0x80001000, tc);
  EXPECT_EQ(unmapped_comp.result, EquivalenceResult::kReturnValueMismatch);

  DifferentialFuzzer::Options options;
  options.candidate_to_target_address = [](uint32_t addr) -> uint32_t {
    if (addr == 0x80200000) return 0x80100000;
    return addr;
  };
  DifferentialFuzzer mapped_fuzzer(options);
  auto mapped_comp = mapped_fuzzer.Compare(target, 0x80001000, candidate, 0x80001000, tc);
  EXPECT_EQ(mapped_comp.result, EquivalenceResult::kEquivalent) << mapped_comp.failure_reason;
}

TEST(DifferentialFuzzerTest, MapsCandidateWriteAddressToTargetAddress) {
  // Target: writes 1 to 0x80100000
  std::vector<uint32_t> target = {
      0x3C088010,  // lui  $t0, 0x8010
      0x24090001,  // li   $t1, 1
      0xAD090000,  // sw   $t1, 0($t0)
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };

  // Candidate: writes 1 to 0x80200000
  std::vector<uint32_t> candidate = {
      0x3C088020,  // lui  $t0, 0x8020
      0x24090001,  // li   $t1, 1
      0xAD090000,  // sw   $t1, 0($t0)
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };

  DifferentialFuzzer default_fuzzer;
  FuzzTestCase tc;
  auto unmapped_comp = default_fuzzer.Compare(target, 0x80001000, candidate, 0x80001000, tc);
  EXPECT_EQ(unmapped_comp.result, EquivalenceResult::kMemoryWriteMismatch);

  DifferentialFuzzer::Options options;
  options.candidate_to_target_address = [](uint32_t addr) -> uint32_t {
    if (addr == 0x80200000) return 0x80100000;
    return addr;
  };
  DifferentialFuzzer mapped_fuzzer(options);
  auto mapped_comp = mapped_fuzzer.Compare(target, 0x80001000, candidate, 0x80001000, tc);
  EXPECT_EQ(mapped_comp.result, EquivalenceResult::kEquivalent) << mapped_comp.failure_reason;
}

TEST(DifferentialFuzzerTest, MapsCandidateStoredPointerValueToTargetAddress) {
  // Target: stores pointer 0x80150000 at address 0x80100000
  std::vector<uint32_t> target = {
      0x3C088010,  // lui  $t0, 0x8010
      0x3C098015,  // lui  $t1, 0x8015
      0xAD090000,  // sw   $t1, 0($t0)
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };

  // Candidate: stores pointer 0x80250000 at address 0x80200000
  std::vector<uint32_t> candidate = {
      0x3C088020,  // lui  $t0, 0x8020
      0x3C098025,  // lui  $t1, 0x8025
      0xAD090000,  // sw   $t1, 0($t0)
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };

  DifferentialFuzzer default_fuzzer;
  FuzzTestCase tc;
  auto unmapped_comp = default_fuzzer.Compare(target, 0x80001000, candidate, 0x80001000, tc);
  EXPECT_EQ(unmapped_comp.result, EquivalenceResult::kMemoryWriteMismatch);

  DifferentialFuzzer::Options options;
  options.candidate_to_target_address = [](uint32_t addr) -> uint32_t {
    if (addr == 0x80200000) return 0x80100000;
    if (addr == 0x80250000) return 0x80150000;
    return addr;
  };
  DifferentialFuzzer mapped_fuzzer(options);
  auto mapped_comp = mapped_fuzzer.Compare(target, 0x80001000, candidate, 0x80001000, tc);
  EXPECT_EQ(mapped_comp.result, EquivalenceResult::kEquivalent) << mapped_comp.failure_reason;
}

TEST(DifferentialFuzzerTest, PreservesPointerBufferWritesForPointerTestCases) {
  // Both target and candidate store to $a0
  std::vector<uint32_t> code = {
      0xAC800000,  // sw   $zero, 0($a0)
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };

  DifferentialFuzzer::Options options;
  options.pointer_buffer_vram = 0x80500000;
  options.pointer_buffer_size = 0x100000;
  options.candidate_to_target_address = [](uint32_t addr) -> uint32_t {
    // Erroneously remap pointer buffer addresses if called
    if (addr == 0x80500000) return 0x80600000;
    return addr;
  };
  DifferentialFuzzer fuzzer(options);

  FuzzTestCase tc;
  tc.a0 = 0x80500000;  // Inside pointer buffer
  auto comp = fuzzer.Compare(code, 0x80001000, code, 0x80001000, tc);
  EXPECT_EQ(comp.result, EquivalenceResult::kEquivalent) << comp.failure_reason;
}

TEST(DifferentialFuzzerTest, InvalidOpcodeAlwaysFailsWithStatusMismatch) {
  std::vector<uint32_t> invalid_code = {
      0x00000039,  // Invalid SPECIAL funct 0x39 -> kInvalidOpcode
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };

  DifferentialFuzzer fuzzer;
  FuzzTestCase tc;
  auto comp = fuzzer.Compare(invalid_code, 0x80001000, invalid_code, 0x80001000, tc);
  EXPECT_EQ(comp.result, EquivalenceResult::kStatusMismatch);
}

TEST(DifferentialFuzzerTest, RetryOnMaxStepsReachedRollsBackMemoryWritesBeforeRetry) {
  // Loop of 60,000 iterations (~240,005 steps): exceeds initial 50,000-step limit,
  // succeeds on the 250,000-step retry.
  // Reads initial counter from 0($a0) and increments *(a0) by 1 per iteration.
  // If memory writes from the initial 50,000-step run were not rolled back before retry,
  // 0($a0) would start the retry with ~12,499 instead of 0 and end up > 60,000.
  // 00: ori   $t0, $zero, 60000 (0x3408EA60)
  // 04: lw    $t1, 0($a0)       (0x8C890000)
  // loop (08):
  // 08: addiu $t0, $t0, -1      (0x2508FFFF)
  // 0C: addiu $t1, $t1, 1       (0x25290001)
  // 10: bne   $t0, $zero, -3    (0x1500FFFD -> 0x08)
  // 14: sw    $t1, 0($a0)       (0xAC890000, delay slot)
  // 18: addu  $v0, $t1, $zero   (0x01201021)
  // 1C: jr    $ra               (0x03E00008)
  // 20: nop                     (0x00000000)
  std::vector<uint32_t> loop_code = {
      0x3408EA60,  // ori   $t0, $zero, 60000
      0x8C890000,  // lw    $t1, 0($a0)
      0x2508FFFF,  // addiu $t0, $t0, -1
      0x25290001,  // addiu $t1, $t1, 1
      0x1500FFFD,  // bne   $t0, $zero, -3
      0xAC890000,  // sw    $t1, 0($a0)
      0x01201021,  // addu  $v0, $t1, $zero
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };

  // Fast equivalent code: adds 60,000 directly to *(a0) in 7 steps
  std::vector<uint32_t> fast_code = {
      0x3408EA60,  // ori  $t0, $zero, 60000
      0x8C890000,  // lw   $t1, 0($a0)
      0x01284821,  // addu $t1, $t1, $t0
      0xAC890000,  // sw   $t1, 0($a0)
      0x01201021,  // addu $v0, $t1, $zero
      0x03E00008,  // jr   $ra
      0x00000000,  // nop
  };

  DifferentialFuzzer fuzzer;
  FuzzTestCase tc;
  tc.a0 = 0x80100000;

  // Case 1: Target hits 50,000-step limit and retries with 250,000 steps
  DifferentialComparison comp_target_retry =
      fuzzer.Compare(loop_code, 0x80001000, fast_code, 0x80001000, tc);
  EXPECT_EQ(comp_target_retry.result, EquivalenceResult::kEquivalent)
      << comp_target_retry.failure_reason;
  EXPECT_EQ(comp_target_retry.target_result.v0, 60000u);
  EXPECT_EQ(comp_target_retry.candidate_result.v0, 60000u);
  ASSERT_EQ(comp_target_retry.target_result.write_log.size(), 60000u);
  EXPECT_EQ(comp_target_retry.target_result.write_log.back().value, 60000u);

  // Case 2: Candidate hits 50,000-step limit and retries with 250,000 steps
  DifferentialComparison comp_cand_retry =
      fuzzer.Compare(fast_code, 0x80001000, loop_code, 0x80001000, tc);
  EXPECT_EQ(comp_cand_retry.result, EquivalenceResult::kEquivalent)
      << comp_cand_retry.failure_reason;
  EXPECT_EQ(comp_cand_retry.target_result.v0, 60000u);
  EXPECT_EQ(comp_cand_retry.candidate_result.v0, 60000u);
  ASSERT_EQ(comp_cand_retry.candidate_result.write_log.size(), 60000u);
  EXPECT_EQ(comp_cand_retry.candidate_result.write_log.back().value, 60000u);
}

TEST(DifferentialFuzzerTest, PreservesPointerBufferWritesWhenPointerInA1A2OrA3) {
  DifferentialFuzzer::Options options;
  options.pointer_buffer_vram = 0x80500000;
  options.pointer_buffer_size = 0x100000;
  // Map 0x80500000 -> 0x80200000 (which is also in target_addresses) so that if is_pointer_case
  // fails to detect a pointer in $a1, $a2, or $a3, the candidate write to 0x80500000 would be
  // erroneously remapped to 0x80200000 and fail comparison.
  options.candidate_to_target_address = [](uint32_t addr) -> uint32_t {
    if (addr >= 0x80500000 && addr < 0x80600000) return 0x80200000;
    return addr;
  };
  DifferentialFuzzer fuzzer(options);

  // Pointer in $a1 ($a0 is a non-pointer integer 42)
  // Both write $a0 to 0x80200000 and to 0($a1)
  std::vector<uint32_t> code_a1 = {
      0x3C088020,  // lui $t0, 0x8020
      0xAD040000,  // sw  $a0, 0($t0)
      0xACA40000,  // sw  $a0, 0($a1)
      0x03E00008,  // jr  $ra
      0x00000000,  // nop
  };
  FuzzTestCase tc_a1;
  tc_a1.a0 = 42;
  tc_a1.a1 = 0x80500010;
  auto comp_a1 = fuzzer.Compare(code_a1, 0x80001000, code_a1, 0x80001000, tc_a1);
  EXPECT_EQ(comp_a1.result, EquivalenceResult::kEquivalent) << comp_a1.failure_reason;

  // Pointer in $a2 ($a0 = 42, $a1 = 0)
  std::vector<uint32_t> code_a2 = {
      0x3C088020,  // lui $t0, 0x8020
      0xAD040000,  // sw  $a0, 0($t0)
      0xACC40000,  // sw  $a0, 0($a2)
      0x03E00008,  // jr  $ra
      0x00000000,  // nop
  };
  FuzzTestCase tc_a2;
  tc_a2.a0 = 42;
  tc_a2.a2 = 0x80500020;
  auto comp_a2 = fuzzer.Compare(code_a2, 0x80001000, code_a2, 0x80001000, tc_a2);
  EXPECT_EQ(comp_a2.result, EquivalenceResult::kEquivalent) << comp_a2.failure_reason;

  // Pointer in $a3 ($a0 = 42, $a1 = 0, $a2 = 0)
  std::vector<uint32_t> code_a3 = {
      0x3C088020,  // lui $t0, 0x8020
      0xAD040000,  // sw  $a0, 0($t0)
      0xACE40000,  // sw  $a0, 0($a3)
      0x03E00008,  // jr  $ra
      0x00000000,  // nop
  };
  FuzzTestCase tc_a3;
  tc_a3.a0 = 42;
  tc_a3.a3 = 0x80500030;
  auto comp_a3 = fuzzer.Compare(code_a3, 0x80001000, code_a3, 0x80001000, tc_a3);
  EXPECT_EQ(comp_a3.result, EquivalenceResult::kEquivalent) << comp_a3.failure_reason;
}

TEST(DifferentialFuzzerTest, SetOptionsAndGetOptionsRoundTripAndTakeEffect) {
  DifferentialFuzzer fuzzer;
  EXPECT_EQ(fuzzer.GetOptions().max_steps_per_run, 50000u);
  EXPECT_TRUE(fuzzer.GetOptions().check_v0);
  EXPECT_FALSE(fuzzer.GetOptions().check_v1);

  DifferentialFuzzer::Options new_opts;
  new_opts.max_steps_per_run = 12345;
  new_opts.initial_sp = 0x80700000;
  new_opts.pointer_buffer_vram = 0x80400000;
  new_opts.pointer_buffer_size = 0x80000;
  new_opts.check_v0 = false;
  new_opts.check_v1 = true;
  fuzzer.SetOptions(new_opts);

  EXPECT_EQ(fuzzer.GetOptions().max_steps_per_run, 12345u);
  EXPECT_EQ(fuzzer.GetOptions().initial_sp, 0x80700000u);
  EXPECT_EQ(fuzzer.GetOptions().pointer_buffer_vram, 0x80400000u);
  EXPECT_EQ(fuzzer.GetOptions().pointer_buffer_size, 0x80000u);
  EXPECT_FALSE(fuzzer.GetOptions().check_v0);
  EXPECT_TRUE(fuzzer.GetOptions().check_v1);

  // Verify updated options take effect: different $v0 is ignored, different $v1 is caught
  std::vector<uint32_t> target = {
      0x24020001,  // addiu $v0, $zero, 1
      0x2403000A,  // addiu $v1, $zero, 10
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };
  std::vector<uint32_t> candidate = {
      0x24020002,  // addiu $v0, $zero, 2 (differs, but check_v0=false)
      0x24030014,  // addiu $v1, $zero, 20 (differs, check_v1=true)
      0x03E00008,  // jr    $ra
      0x00000000,  // nop
  };
  FuzzTestCase tc;
  auto comp = fuzzer.Compare(target, 0x80001000, candidate, 0x80001000, tc);
  EXPECT_EQ(comp.result, EquivalenceResult::kReturnValueMismatch);
  EXPECT_NE(comp.failure_reason.find("v1 mismatch"), std::string::npos);
}

}  // namespace
}  // namespace rom_nom_nom::fuzzer
