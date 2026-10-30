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

  // Verify structural readiness metrics on the 4 functions
  EXPECT_EQ(summary.frame_size_matches, 4u);
  EXPECT_EQ(summary.saved_registers_matches, 4u);
  EXPECT_EQ(summary.cfg_topology_matches, 4u);
  EXPECT_EQ(summary.structurally_isomorphic_functions, 4u);
  EXPECT_DOUBLE_EQ(summary.FrameSizeMatchPercentage(), 100.0);
  EXPECT_DOUBLE_EQ(summary.SavedRegistersMatchPercentage(), 100.0);
  EXPECT_DOUBLE_EQ(summary.CfgTopologyMatchPercentage(), 100.0);
  EXPECT_DOUBLE_EQ(summary.StructurallyIsomorphicPercentage(), 100.0);
  EXPECT_EQ(summary.total_target_instructions, 12u);
  EXPECT_EQ(summary.total_normalized_lcs_instructions, 11u);

  // Verify formatted report
  std::string report = EquivalenceRunner::FormatReport(summary, /*verbose=*/true);
  EXPECT_NE(report.find("EQUIVALENCE FUZZING REPORT: player_movement"), std::string::npos);
  EXPECT_NE(report.find("Exact Binary Matches (Fast-Pass): 2 (50.0%)"), std::string::npos);
  EXPECT_NE(report.find("Fuzz-Proven Equivalent:           1 (25.0%)"), std::string::npos);
  EXPECT_NE(report.find("Divergent (Behavioral Bugs):      1 (25.0%)"), std::string::npos);
  EXPECT_NE(report.find("Overall Functional Equivalence:    75.0%"), std::string::npos);
  EXPECT_NE(report.find("Structural Readiness Metrics:"), std::string::npos);
  EXPECT_NE(report.find("1. Stack Frame Size Match:        4 / 4 (100.0%)"), std::string::npos);
  EXPECT_NE(report.find("2. Callee-Saved Register Match:   4 / 4 (100.0%)"), std::string::npos);
  EXPECT_NE(report.find("3. CFG Block & Branch Match:      4 / 4 (100.0%)"), std::string::npos);
  EXPECT_NE(report.find("4. Normalized Instruction LCS:    11 / 12 (91.7%)"), std::string::npos);
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

TEST(EquivalenceRunnerTest, VoidFunctionDoesNotCheckV0) {
  EquivalenceRunner runner;

  FunctionEquivalenceTarget target;
  target.name = "VoidFunc";
  target.vram = 0x80001000;
  // Target: sw $a0, 0($a1); jr $ra; nop
  target.target_words = {0xACA40000, 0x03E00008, 0x00000000};
  // Candidate: sw $a0, 0($a1); addiu $v0, $zero, 42; jr $ra; nop
  target.candidate_words = {0xACA40000, 0x2402002A, 0x03E00008, 0x00000000};

  FunctionEquivalenceResult res = runner.EvaluateFunction(target);
  EXPECT_EQ(res.status, FunctionEquivalenceStatus::kFuzzProvenEquivalent);
  EXPECT_FALSE(res.counterexample.has_value());
}

TEST(EquivalenceRunnerTest, FloatFunctionChecksF0AndIgnoresV0) {
  EquivalenceRunner runner;

  FunctionEquivalenceTarget target;
  target.name = "FloatAdd";
  target.vram = 0x80001000;
  // Target: add.s $f0, $f12, $f14; jr $ra; nop
  target.target_words = {0x460E6000, 0x03E00008, 0x00000000};
  // Candidate: add.s $f0, $f14, $f12; addiu $v0, $zero, 99; jr $ra; nop
  target.candidate_words = {0x460C7000, 0x24020063, 0x03E00008, 0x00000000};

  FunctionEquivalenceResult res = runner.EvaluateFunction(target);
  EXPECT_EQ(res.status, FunctionEquivalenceStatus::kFuzzProvenEquivalent);
  EXPECT_FALSE(res.counterexample.has_value());
}

TEST(EquivalenceRunnerTest, FloatFunctionDetectsF0Divergence) {
  EquivalenceRunner runner;

  FunctionEquivalenceTarget target;
  target.name = "FloatDivergent";
  target.vram = 0x80001000;
  // Target: add.s $f0, $f12, $f14; jr $ra; nop
  target.target_words = {0x460E6000, 0x03E00008, 0x00000000};
  // Candidate: sub.s $f0, $f12, $f14; jr $ra; nop
  target.candidate_words = {0x460E6001, 0x03E00008, 0x00000000};

  FunctionEquivalenceResult res = runner.EvaluateFunction(target);
  EXPECT_EQ(res.status, FunctionEquivalenceStatus::kDivergent);
  ASSERT_TRUE(res.counterexample.has_value());
}

TEST(EquivalenceRunnerTest, DoubleFunctionChecksF0Double) {
  EquivalenceRunner runner;

  FunctionEquivalenceTarget target;
  target.name = "DoubleAdd";
  target.vram = 0x80001000;
  // Target: add.d $f0, $f12, $f14; jr $ra; nop
  target.target_words = {0x462E6000, 0x03E00008, 0x00000000};
  // Candidate: add.d $f0, $f14, $f12; addiu $v0, $zero, 123; jr $ra; nop
  target.candidate_words = {0x462C7000, 0x2402007B, 0x03E00008, 0x00000000};

  FunctionEquivalenceResult res = runner.EvaluateFunction(target);
  EXPECT_EQ(res.status, FunctionEquivalenceStatus::kFuzzProvenEquivalent);
  EXPECT_FALSE(res.counterexample.has_value());
}

TEST(EquivalenceRunnerTest, DoubleFunctionDetectsF0DoubleDivergence) {
  EquivalenceRunner runner;

  FunctionEquivalenceTarget target;
  target.name = "DoubleDivergent";
  target.vram = 0x80001000;
  // Target: add.d $f0, $f12, $f14; jr $ra; nop
  target.target_words = {0x462E6000, 0x03E00008, 0x00000000};
  // Candidate: sub.d $f0, $f12, $f14; jr $ra; nop
  target.candidate_words = {0x462E6001, 0x03E00008, 0x00000000};

  FunctionEquivalenceResult res = runner.EvaluateFunction(target);
  EXPECT_EQ(res.status, FunctionEquivalenceStatus::kDivergent);
  ASSERT_TRUE(res.counterexample.has_value());
}

TEST(EquivalenceRunnerTest, CounterexampleDetailsIncludeArguments) {
  EquivalenceRunner runner;

  FunctionEquivalenceTarget target;
  target.name = "DivergentWithArgs";
  target.vram = 0x80001000;
  // Target: addu $v0, $a0, $a1; jr $ra; nop
  target.target_words = {0x00851021, 0x03E00008, 0x00000000};
  // Candidate: subu $v0, $a0, $a1; jr $ra; nop
  target.candidate_words = {0x00851023, 0x03E00008, 0x00000000};

  FunctionEquivalenceResult res = runner.EvaluateFunction(target);
  EXPECT_EQ(res.status, FunctionEquivalenceStatus::kDivergent);
  ASSERT_TRUE(res.counterexample.has_value());
  EXPECT_NE(res.details.find("(args: a0=0x"), std::string::npos);
}

TEST(EquivalenceRunnerTest, InitialMemoryPayloadsForwardedAndProvenEquivalent) {
  EquivalenceRunner runner;

  FunctionEquivalenceTarget target;
  target.name = "LoadInitialMemory";
  target.vram = 0x80001000;

  // Target: loads word at 0x80200000 into v0
  // lui $at, 0x8020; lw $v0, 0($at); jr $ra; nop
  target.target_words = {0x3C018020, 0x8C220000, 0x03E00008, 0x00000000};
  // Candidate: loads word at 0x80200000 into v0 via different register t0
  // lui $t0, 0x8020; lw $v0, 0($t0); jr $ra; nop
  target.candidate_words = {0x3C088020, 0x8D020000, 0x03E00008, 0x00000000};

  MemoryPayload payload;
  payload.address = 0x80200000;
  payload.data = {0x11, 0x22, 0x33, 0x44};
  target.target_initial_memory.push_back(payload);
  target.candidate_initial_memory.push_back(payload);

  FunctionEquivalenceResult res = runner.EvaluateFunction(target);
  EXPECT_EQ(res.status, FunctionEquivalenceStatus::kFuzzProvenEquivalent);
  EXPECT_FALSE(res.counterexample.has_value());
}

TEST(EquivalenceRunnerTest, InitialMemoryPayloadsForwardedDetectsDivergence) {
  EquivalenceRunner runner;

  FunctionEquivalenceTarget target;
  target.name = "LoadInitialMemoryDivergent";
  target.vram = 0x80001000;

  // Both functions load word at 0x80200000 into v0
  target.target_words = {0x3C018020, 0x8C220000, 0x03E00008, 0x00000000};
  target.candidate_words = {0x3C088020, 0x8D020000, 0x03E00008, 0x00000000};

  MemoryPayload target_payload;
  target_payload.address = 0x80200000;
  target_payload.data = {0x11, 0x22, 0x33, 0x44};
  target.target_initial_memory.push_back(target_payload);

  MemoryPayload candidate_payload;
  candidate_payload.address = 0x80200000;
  candidate_payload.data = {0x99, 0x88, 0x77, 0x66};
  target.candidate_initial_memory.push_back(candidate_payload);

  FunctionEquivalenceResult res = runner.EvaluateFunction(target);
  EXPECT_EQ(res.status, FunctionEquivalenceStatus::kDivergent);
  ASSERT_TRUE(res.counterexample.has_value());
}

TEST(EquivalenceRunnerTest, EvaluateFunctionUsesExternalDifferentialFuzzer) {
  EquivalenceRunner runner;
  DifferentialFuzzer external_fuzzer;

  FunctionEquivalenceTarget target;
  target.name = "ExternalFuzzerFloatFunc";
  target.vram = 0x80001000;
  // Target: add.s $f0, $f12, $f14; jr $ra; nop
  target.target_words = {0x460E6000, 0x03E00008, 0x00000000};
  // Candidate: add.s $f0, $f14, $f12; jr $ra; nop
  target.candidate_words = {0x460C7000, 0x03E00008, 0x00000000};

  MemoryPayload payload;
  payload.address = 0x80200000;
  payload.data = {1, 2, 3, 4};
  target.target_initial_memory.push_back(payload);

  FunctionEquivalenceResult res = runner.EvaluateFunction(target, &external_fuzzer);
  EXPECT_EQ(res.status, FunctionEquivalenceStatus::kFuzzProvenEquivalent);
  // Verify EvaluateFunction configured and used external_fuzzer
  EXPECT_TRUE(external_fuzzer.GetOptions().check_f0);
  EXPECT_FALSE(external_fuzzer.GetOptions().check_v0);
  ASSERT_EQ(external_fuzzer.GetOptions().target_initial_memory.size(), 1u);
  EXPECT_EQ(external_fuzzer.GetOptions().target_initial_memory[0].address, 0x80200000u);
}

TEST(EquivalenceRunnerTest, StopOnFirstDivergenceMultiThreaded) {
  EquivalenceRunnerOptions opts;
  opts.num_threads = 2;
  opts.stop_on_first_divergence = true;
  opts.random_vectors_per_function = 200;
  EquivalenceRunner runner(opts);

  std::vector<FunctionEquivalenceTarget> targets;
  // Target 0: Divergent immediately
  FunctionEquivalenceTarget div_target;
  div_target.name = "FuncDivergent0";
  div_target.vram = 0x80001000;
  div_target.target_words = {0x00851021, 0x03E00008, 0x00000000};     // addu $v0, $a0, $a1
  div_target.candidate_words = {0x00851023, 0x03E00008, 0x00000000};  // subu $v0, $a0, $a1
  targets.push_back(div_target);

  // Targets 1..19: Non-matching instruction sequences that require full fuzzing batteries
  for (int i = 1; i < 20; ++i) {
    FunctionEquivalenceTarget eq_target;
    eq_target.name = "FuncEquivalent_" + std::to_string(i);
    eq_target.vram = 0x80001000 + static_cast<uint32_t>(i * 0x20);
    eq_target.target_words = {0x00851021, 0x03E00008, 0x00000000};
    eq_target.candidate_words = {0x00A41021, 0x03E00008, 0x00000000};
    targets.push_back(eq_target);
  }

  ModuleEquivalenceSummary summary = runner.EvaluateModule("mt_stop_early", targets);
  // Because target 0 diverges, the results must be truncated at index 0 (size 1)
  EXPECT_EQ(summary.total_functions, 1u);
  ASSERT_EQ(summary.function_results.size(), 1u);
  EXPECT_EQ(summary.function_results[0].name, "FuncDivergent0");
  EXPECT_EQ(summary.function_results[0].status, FunctionEquivalenceStatus::kDivergent);
  EXPECT_EQ(summary.divergent_functions, 1u);
}

TEST(EquivalenceRunnerTest, FastPassTrimsTrailingSectionAlignmentNops) {
  EquivalenceRunner runner;

  // Case 1: `jr $ra` + non-nop delay slot (`addiu $v0, $a0, 1`) + 2 trailing alignment NOPs
  FunctionEquivalenceTarget target1;
  target1.name = "SectionAlignedFuncWithNonNopDelay";
  target1.vram = 0x80001000;
  target1.target_words = {
      0x03E00008,  // jr $ra
      0x24820001,  // addiu $v0, $a0, 1
      0x00000000,  // section-alignment padding nop
      0x00000000,  // section-alignment padding nop
  };
  target1.candidate_words = {
      0x03E00008,  // jr $ra
      0x24820001,  // addiu $v0, $a0, 1
  };

  FunctionEquivalenceResult res1 = runner.EvaluateFunction(target1);
  EXPECT_EQ(res1.status, FunctionEquivalenceStatus::kExactBinaryMatch);
  EXPECT_EQ(res1.fuzz_vectors_evaluated, 0u);

  // Case 2: `addiu $v0, $a0, 1; jr $ra; nop` (delay slot nop) + 1 trailing alignment NOP
  FunctionEquivalenceTarget target2;
  target2.name = "SectionAlignedFuncWithNopDelay";
  target2.vram = 0x80001010;
  target2.target_words = {
      0x24820001,  // addiu $v0, $a0, 1
      0x03E00008,  // jr $ra
      0x00000000,  // delay-slot nop
      0x00000000,  // section-alignment padding nop
  };
  target2.candidate_words = {
      0x24820001,  // addiu $v0, $a0, 1
      0x03E00008,  // jr $ra
      0x00000000,  // delay-slot nop
  };

  FunctionEquivalenceResult res2 = runner.EvaluateFunction(target2);
  EXPECT_EQ(res2.status, FunctionEquivalenceStatus::kExactBinaryMatch);
  EXPECT_EQ(res2.fuzz_vectors_evaluated, 0u);
}

TEST(EquivalenceRunnerTest, FastPassMatchesRelocatedRodataFloatLiteralAndRejectsDivergentLiteral) {
  EquivalenceRunner runner;

  FunctionEquivalenceTarget target;
  target.name = "RelocatedFloatLiteralFunc";
  target.vram = 0x80001000;
  // Target loads float literal 1.0f (0x3F800000) from retail .rodata at 0x8011EF80:
  //   lui $at, 0x8012
  //   lwc1 $f0, -0x1080($at)  (0x80120000 - 0x1080 = 0x8011EF80)
  //   jr $ra
  //   nop
  target.target_words = {0x3C018012, 0xC420EF80, 0x03E00008, 0x00000000};

  // Candidate loads float literal 1.0f (0x3F800000) from relocated private .rodata at 0x80600020:
  //   lui $at, 0x8060
  //   lwc1 $f0, 0x0020($at)
  //   jr $ra
  //   nop
  target.candidate_words = {0x3C018060, 0xC4200020, 0x03E00008, 0x00000000};

  MemoryPayload t_rodata;
  t_rodata.address = 0x8011EF80;
  t_rodata.data = {0x3F, 0x80, 0x00, 0x00};
  target.target_initial_memory.push_back(t_rodata);

  MemoryPayload c_rodata;
  c_rodata.address = 0x80600020;
  c_rodata.data = {0x3F, 0x80, 0x00, 0x00};
  target.candidate_initial_memory.push_back(c_rodata);

  FunctionEquivalenceResult res = runner.EvaluateFunction(target);
  EXPECT_EQ(res.status, FunctionEquivalenceStatus::kExactBinaryMatch);
  EXPECT_EQ(res.fuzz_vectors_evaluated, 0u);

  // If the candidate's .rodata constant differs (e.g., 2.0f = 0x40000000), fast-pass must NOT
  // accept it and differential fuzzing must catch the divergence.
  target.candidate_initial_memory[0].data = {0x40, 0x00, 0x00, 0x00};
  FunctionEquivalenceResult div_res = runner.EvaluateFunction(target);
  EXPECT_EQ(div_res.status, FunctionEquivalenceStatus::kDivergent);
}

TEST(EquivalenceRunnerTest, FastPassMatchesRelocatedSwitchJumpTable) {
  EquivalenceRunner runner;

  FunctionEquivalenceTarget target;
  target.name = "RelocatedSwitchJumpTableFunc";
  target.vram = 0x80076150;
  target.candidate_vram = 0x80076200;

  // Both functions implement a 2-way jump table dispatch:
  //   0: sll $t0, $a0, 2          (0x00044080)
  //   1: lui $at, hi(jtbl)        (target: 0x800B, cand: 0x8060)
  //   2: addu $at, $at, $t0       (0x00280821)
  //   3: lw $t0, lo(jtbl)($at)    (target: -0x20D0 -> 0x800ADF30, cand: 0x0040 -> 0x80600040)
  //   4: jr $t0                   (0x01000008)
  //   5: nop                      (0x00000000)
  //   6: jr $ra                   (0x03E00008)  [case 0 @ +0x18]
  //   7: addiu $v0, $zero, 10     (0x2402000A)
  //   8: jr $ra                   (0x03E00008)  [case 1 @ +0x20]
  //   9: addiu $v0, $zero, 20     (0x24020014)
  target.target_words = {
      0x00044080, 0x3C01800B, 0x00280821, 0x8C28DF30, 0x01000008,
      0x00000000, 0x03E00008, 0x2402000A, 0x03E00008, 0x24020014,
  };
  target.candidate_words = {
      0x00044080, 0x3C018060, 0x00280821, 0x8C280040, 0x01000008,
      0x00000000, 0x03E00008, 0x2402000A, 0x03E00008, 0x24020014,
  };

  // Target jump table at 0x800ADF30: [0x80076168 (+0x18), 0x80076170 (+0x20)]
  MemoryPayload t_jtbl;
  t_jtbl.address = 0x800ADF30;
  t_jtbl.data = {0x80, 0x07, 0x61, 0x68, 0x80, 0x07, 0x61, 0x70};
  target.target_initial_memory.push_back(t_jtbl);

  // Candidate jump table at 0x80600040: [0x80076218 (+0x18), 0x80076220 (+0x20)]
  MemoryPayload c_jtbl;
  c_jtbl.address = 0x80600040;
  c_jtbl.data = {0x80, 0x07, 0x62, 0x18, 0x80, 0x07, 0x62, 0x20};
  target.candidate_initial_memory.push_back(c_jtbl);

  FunctionEquivalenceResult res = runner.EvaluateFunction(target);
  EXPECT_EQ(res.status, FunctionEquivalenceStatus::kExactBinaryMatch);
  EXPECT_EQ(res.fuzz_vectors_evaluated, 0u);
}

}  // namespace
}  // namespace rom_nom_nom::fuzzer
