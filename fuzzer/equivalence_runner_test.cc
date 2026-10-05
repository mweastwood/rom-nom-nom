#include "fuzzer/equivalence_runner.h"

#include <cstdint>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace rom_nom_nom::fuzzer {
namespace {

TEST(EquivalenceRunnerTest, FastPassBitExactMatch) {
  EquivalenceRunner runner;

  FunctionEquivalenceTarget target;
  target.name = "MyBitExactFunc";
  target.vram = 0x80001000;
  // addiu $v0, $a0, 1; jr $ra; nop
  target.target_words = {0x24820001, 0x03E00008, 0x00000000};
  target.candidate_words = {0x24820001, 0x03E00008, 0x00000000};

  FunctionEquivalenceResult res = runner.EvaluateFunction(target);

  EXPECT_EQ(res.status, FunctionEquivalenceStatus::kExactBinaryMatch);
  EXPECT_EQ(res.fuzz_vectors_evaluated, 0u);
  EXPECT_FALSE(res.counterexample.has_value());
}

TEST(EquivalenceRunnerTest, FuzzProvenEquivalentOnInstructionMismatch) {
  EquivalenceRunner runner;

  FunctionEquivalenceTarget target;
  target.name = "CommutativeAdd";
  target.vram = 0x80001000;
  // Target: addu $v0, $a0, $a1; jr $ra; nop
  target.target_words = {0x00851021, 0x03E00008, 0x00000000};
  // Candidate: addu $v0, $a1, $a0; jr $ra; nop (different machine code!)
  target.candidate_words = {0x00A41021, 0x03E00008, 0x00000000};

  FunctionEquivalenceResult res = runner.EvaluateFunction(target);

  EXPECT_EQ(res.status, FunctionEquivalenceStatus::kFuzzProvenEquivalent);
  EXPECT_GT(res.fuzz_vectors_evaluated, 0u);
  EXPECT_FALSE(res.counterexample.has_value());
}

TEST(EquivalenceRunnerTest, DetectsDivergentFunctionWithCounterexample) {
  EquivalenceRunner runner;

  FunctionEquivalenceTarget target;
  target.name = "DivergentSub";
  target.vram = 0x80001000;
  // Target: addu $v0, $a0, $a1; jr $ra; nop
  target.target_words = {0x00851021, 0x03E00008, 0x00000000};
  // Candidate: subu $v0, $a0, $a1; jr $ra; nop (bug!)
  target.candidate_words = {0x00851023, 0x03E00008, 0x00000000};

  FunctionEquivalenceResult res = runner.EvaluateFunction(target);

  EXPECT_EQ(res.status, FunctionEquivalenceStatus::kDivergent);
  ASSERT_TRUE(res.counterexample.has_value());
  EXPECT_NE(res.details.find("Divergence on vector"), std::string::npos);
}

TEST(EquivalenceRunnerTest, EmptyInstructionsYieldsError) {
  EquivalenceRunner runner;

  FunctionEquivalenceTarget target;
  target.name = "EmptyFunc";
  target.vram = 0x80001000;

  FunctionEquivalenceResult res = runner.EvaluateFunction(target);
  EXPECT_EQ(res.status, FunctionEquivalenceStatus::kCompilationOrExtractError);
}

TEST(EquivalenceRunnerTest, WholeModuleBatchEvaluation) {
  EquivalenceRunner runner;

  // 1. Bit exact
  FunctionEquivalenceTarget f1;
  f1.name = "FuncBitExact";
  f1.vram = 0x80001000;
  f1.target_words = {0x24820001, 0x03E00008, 0x00000000};
  f1.candidate_words = {0x24820001, 0x03E00008, 0x00000000};

  // 2. Fuzz-proven equivalent (nonmatching bytes)
  FunctionEquivalenceTarget f2;
  f2.name = "FuncFuzzProven";
  f2.vram = 0x80001010;
  f2.target_words = {0x00851021, 0x03E00008, 0x00000000};
  f2.candidate_words = {0x00A41021, 0x03E00008, 0x00000000};

  // 3. Another bit exact
  FunctionEquivalenceTarget f3;
  f3.name = "FuncBitExact2";
  f3.vram = 0x80001020;
  f3.target_words = {0x24820002, 0x03E00008, 0x00000000};
  f3.candidate_words = {0x24820002, 0x03E00008, 0x00000000};

  // 4. Divergent
  FunctionEquivalenceTarget f4;
  f4.name = "FuncDivergent";
  f4.vram = 0x80001030;
  f4.target_words = {0x00851021, 0x03E00008, 0x00000000};
  f4.candidate_words = {0x00851023, 0x03E00008, 0x00000000};

  std::vector<FunctionEquivalenceTarget> module_targets = {f1, f2, f3, f4};
  ModuleEquivalenceSummary summary = runner.EvaluateModule("player_movement", module_targets);

  EXPECT_EQ(summary.module_name, "player_movement");
  EXPECT_EQ(summary.total_functions, 4u);
  EXPECT_EQ(summary.exact_binary_matches, 2u);
  EXPECT_EQ(summary.fuzz_proven_equivalent, 1u);
  EXPECT_EQ(summary.divergent_functions, 1u);
  EXPECT_EQ(summary.error_functions, 0u);
  EXPECT_DOUBLE_EQ(summary.EquivalencePercentage(), 75.0);

  // Verify formatted report
  std::string report = EquivalenceRunner::FormatReport(summary, /*verbose=*/true);
  EXPECT_NE(report.find("EQUIVALENCE FUZZING REPORT: player_movement"), std::string::npos);
  EXPECT_NE(report.find("Exact Binary Matches (Fast-Pass): 2 (50.0%)"), std::string::npos);
  EXPECT_NE(report.find("Fuzz-Proven Equivalent:           1 (25.0%)"), std::string::npos);
  EXPECT_NE(report.find("Divergent (Behavioral Bugs):      1 (25.0%)"), std::string::npos);
  EXPECT_NE(report.find("Overall Functional Equivalence:    75.0%"), std::string::npos);
  EXPECT_NE(report.find("FuncDivergent"), std::string::npos);
  EXPECT_NE(report.find("FuncFuzzProven"), std::string::npos);
}

TEST(EquivalenceRunnerTest, CandidateVramRelocationSupport) {
  EquivalenceRunner runner;

  FunctionEquivalenceTarget target;
  target.name = "RelocatedBranch";
  target.vram = 0x80001000;
  target.candidate_vram = 0x80005000;

  target.target_words = {
      0x10800002,  // beqz $a0, +2
      0x24020001,  // addiu $v0, $zero, 1
      0x24020002,  // addiu $v0, $zero, 2
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };
  target.candidate_words = {
      0x10800002,  // beqz $a0, +2
      0x34020001,  // ori $v0, $zero, 1
      0x34020002,  // ori $v0, $zero, 2
      0x03E00008,  // jr $ra
      0x00000000,  // nop
  };

  FunctionEquivalenceResult res = runner.EvaluateFunction(target);
  EXPECT_EQ(res.status, FunctionEquivalenceStatus::kFuzzProvenEquivalent);
  EXPECT_FALSE(res.counterexample.has_value());
}

}  // namespace
}  // namespace rom_nom_nom::fuzzer
