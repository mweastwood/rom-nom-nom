#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "core/c_ast.h"
#include "core/compiler.h"
#include "core/elf_reader.h"
#include "core/mips.h"
#include "core/register_tracker.h"
#include "core/round_trip_runner.h"
#include "core/round_trip_test_cases.h"
#include "core/symbol_registry.h"
#include "core/target_extractor.h"
#include "gtest/gtest.h"
#include "lifter/c_emitter.h"
#include "lifter/function_loader.h"
#include "lifter/lifter_pipeline.h"
#include "tuner/ast_mutator.h"
#include "tuner/candidate_scorer.h"
#include "tuner/flag_explorer.h"

namespace rom_nom_nom {
namespace {

void WriteTextFile(const std::filesystem::path& path, std::string_view content) {
  std::ofstream ofs(path, std::ios::out | std::ios::trunc);
  ofs.write(content.data(), content.size());
}

std::string EmitFunctionAsTranslationUnit(const FunctionDeclaration& func,
                                          const SymbolIndex* symbol_index) {
  CTranslationUnit tu;
  tu.includes = {"types.h"};
  tu.symbol_index = symbol_index;
  tu.functions.push_back(func.Clone());

  CEmitterOptions emitter_opts;
  emitter_opts.includes = {"types.h"};
  emitter_opts.format_with_clang = false;
  return CEmitter::EmitTranslationUnit(tu, emitter_opts);
}

class TunerRoundTripStage : public RoundTripStage {
 public:
  std::string_view Name() const override { return "tuner"; }

  absl::StatusOr<RoundTripCaseOutcome> Execute(const PreparedRoundTripCase& prepared) override {
    const auto& test_case = prepared.test_case;
    const uint32_t base_vram = test_case.options.base_vram;

    auto loaded_or = FunctionLoader::FromWords(
        test_case.function_name, prepared.patched_orig_words, base_vram,
        prepared.symbol_index.get(), /*game_config=*/nullptr, prepared.rodata_memory_reader);
    if (!loaded_or.ok()) {
      return loaded_or.status();
    }

    LifterPipelineOptions lifter_opts;
    lifter_opts.includes = {"types.h"};
    lifter_opts.format_with_clang = false;
    LifterPipeline lifter(lifter_opts);

    auto decompile_or = lifter.DecompileFunction(*loaded_or);
    if (!decompile_or.ok()) {
      return absl::InternalError(absl::StrFormat("Lifter failed to decompile function '%s': %s",
                                                 test_case.function_name,
                                                 decompile_or.status().message()));
    }

    FunctionDeclaration baseline_ast = std::move(*decompile_or->ast);

    TargetFunction target;
    target.name = test_case.function_name;
    target.vram = base_vram;
    target.size = static_cast<uint32_t>(prepared.patched_orig_words.size() * 4);
    target.raw_words = prepared.patched_orig_words;
    auto decoded_target_or = DecodeSequence(prepared.patched_orig_words, base_vram);
    if (decoded_target_or.ok()) {
      target.instructions = std::move(*decoded_target_or);
    }

    CandidateScorer scorer(nullptr, prepared.symbol_index.get());

    struct EvaluatedCandidate {
      bool compiled_ok = false;
      bool bit_exact = false;
      std::string c_code;
      ElfFunction elf_func;
      CandidateScore score;
    };

    size_t candidate_counter = 0;
    auto evaluate_candidate = [&](const FunctionDeclaration& ast,
                                  const std::vector<std::string>& flags) -> EvaluatedCandidate {
      EvaluatedCandidate eval;
      eval.c_code = EmitFunctionAsTranslationUnit(ast, prepared.symbol_index.get());
      const size_t id = candidate_counter++;
      std::filesystem::path cand_c_path = prepared.work_dir / absl::StrCat("tuner_cand_", id, ".c");
      std::filesystem::path cand_o_path = prepared.work_dir / absl::StrCat("tuner_cand_", id, ".o");
      WriteTextFile(cand_c_path, eval.c_code);

      auto compile_status = prepared.compiler->CompileC({
          .src_file = cand_c_path,
          .out_obj = cand_o_path,
          .opt_flags = flags,
          .include_dirs = {prepared.work_dir},
      });
      if (!compile_status.ok()) {
        return eval;
      }

      auto reader_or = ElfReader::LoadFromFile(cand_o_path);
      if (!reader_or.ok()) {
        return eval;
      }
      auto func_or = reader_or->ExtractFunction(test_case.function_name);
      if (!func_or.ok()) {
        return eval;
      }
      eval.compiled_ok = true;
      eval.elf_func = std::move(*func_or);
      eval.bit_exact = RoundTripRunner::CompareFunctionsBitExact(
          prepared.orig_func, eval.elf_func, base_vram, prepared.full_orig_c, eval.c_code,
          /*diagnostic_out=*/nullptr);

      std::vector<uint32_t> patched_cand_words =
          RoundTripRunner::ApplyCandidateRelocations(prepared, eval.elf_func);
      std::vector<Instruction> cand_insts;
      auto decoded_cand_or = DecodeSequence(patched_cand_words, base_vram);
      if (decoded_cand_or.ok()) {
        cand_insts = std::move(*decoded_cand_or);
      }
      eval.score = scorer.ScoreInstructions(target, cand_insts, patched_cand_words);
      if (eval.bit_exact) {
        eval.score.raw_bit_exact = true;
        eval.score.normalized_exact = true;
      }
      return eval;
    };

    std::vector<std::string> base_flags = test_case.options.c_flags;
    EvaluatedCandidate best_eval = evaluate_candidate(baseline_ast, base_flags);
    if (!best_eval.compiled_ok) {
      return absl::InternalError(
          absl::StrFormat("TunerRoundTripStage: Failed to compile baseline lifted C for '%s':\n%s",
                          test_case.function_name, best_eval.c_code));
    }

    FunctionDeclaration best_ast = baseline_ast.Clone();
    std::vector<std::string> winning_flags = base_flags;

    if (!best_eval.bit_exact) {
      absl::flat_hash_set<std::string> evaluated_keys;
      auto make_key = [](const std::vector<std::string>& flags, std::string_view code) {
        return absl::StrCat(absl::StrJoin(flags, " "), "\n", code);
      };
      evaluated_keys.insert(make_key(base_flags, best_eval.c_code));

      std::vector<std::vector<std::string>> flag_profiles =
          FlagExplorer::GenerateCandidateFlags(base_flags);

      // Phase 1a: Compiler Flag Sweep on Baseline AST
      for (size_t i = 1; i < flag_profiles.size() && !best_eval.bit_exact; ++i) {
        if (!evaluated_keys.insert(make_key(flag_profiles[i], best_eval.c_code)).second) {
          continue;
        }
        EvaluatedCandidate cand = evaluate_candidate(baseline_ast, flag_profiles[i]);
        if (cand.compiled_ok && (cand.bit_exact || cand.score.IsBetterThan(best_eval.score))) {
          best_eval = std::move(cand);
          winning_flags = flag_profiles[i];
        }
      }

      // Phase 1b: Canonical AST Preset Sweep
      if (!best_eval.bit_exact) {
        auto presets = AstMutator::GenerateCanonicalPresets(baseline_ast);
        for (const auto& preset_steps : presets) {
          FunctionDeclaration preset_ast = AstMutator::ApplyMutations(baseline_ast, preset_steps);
          std::string preset_c =
              EmitFunctionAsTranslationUnit(preset_ast, prepared.symbol_index.get());
          if (!evaluated_keys.insert(make_key(winning_flags, preset_c)).second) {
            continue;
          }
          EvaluatedCandidate cand = evaluate_candidate(preset_ast, winning_flags);
          if (cand.compiled_ok && (cand.bit_exact || cand.score.IsBetterThan(best_eval.score))) {
            best_eval = std::move(cand);
            best_ast = std::move(preset_ast);
            if (best_eval.bit_exact) break;
          }
        }
      }

      // Phase 2: Greedy AST Mutation Search
      constexpr size_t kMaxRounds = 4;
      for (size_t round = 0; round < kMaxRounds && !best_eval.bit_exact; ++round) {
        const FunctionDeclaration round_base_ast = best_ast.Clone();
        auto steps = AstMutator::DiscoverMutations(round_base_ast, /*max_mutations=*/64);
        bool improved_in_round = false;
        for (const auto& step : steps) {
          FunctionDeclaration mutated = AstMutator::ApplyMutation(round_base_ast, step);
          std::string mutated_c =
              EmitFunctionAsTranslationUnit(mutated, prepared.symbol_index.get());
          if (!evaluated_keys.insert(make_key(winning_flags, mutated_c)).second) {
            continue;
          }
          EvaluatedCandidate cand = evaluate_candidate(mutated, winning_flags);
          if (cand.compiled_ok && (cand.bit_exact || cand.score.IsBetterThan(best_eval.score))) {
            best_eval = std::move(cand);
            best_ast = std::move(mutated);
            improved_in_round = true;
            if (best_eval.bit_exact) break;
          }
        }
        if (!improved_in_round) break;
      }

      // Phase 2b: Near-Miss Polish Pass across flag profiles
      if (!best_eval.bit_exact) {
        constexpr size_t kMaxPolishRounds = 2;
        for (size_t polish = 0; polish < kMaxPolishRounds && !best_eval.bit_exact; ++polish) {
          const FunctionDeclaration polish_base_ast = best_ast.Clone();
          auto all_steps = AstMutator::DiscoverMutations(polish_base_ast, /*max_mutations=*/256);
          bool improved_in_polish = false;
          for (const auto& step : all_steps) {
            FunctionDeclaration mutated = AstMutator::ApplyMutation(polish_base_ast, step);
            std::string mutated_c =
                EmitFunctionAsTranslationUnit(mutated, prepared.symbol_index.get());
            for (const auto& flags : flag_profiles) {
              if (!evaluated_keys.insert(make_key(flags, mutated_c)).second) {
                continue;
              }
              EvaluatedCandidate cand = evaluate_candidate(mutated, flags);
              if (cand.compiled_ok &&
                  (cand.bit_exact || cand.score.IsBetterThan(best_eval.score))) {
                best_eval = std::move(cand);
                best_ast = mutated.Clone();
                winning_flags = flags;
                improved_in_polish = true;
                if (best_eval.bit_exact) break;
              }
            }
            if (best_eval.bit_exact) break;
          }
          if (!improved_in_polish) break;
        }
      }
    }

    RoundTripCaseOutcome outcome;
    outcome.test_name = test_case.test_name;
    outcome.function_name = test_case.function_name;
    outcome.candidate_c = best_eval.c_code;
    outcome.bit_exact = RoundTripRunner::CompareFunctionsBitExact(
        prepared.orig_func, best_eval.elf_func, base_vram, prepared.full_orig_c, best_eval.c_code,
        &outcome.diagnostic);
    if (outcome.bit_exact) {
      outcome.structural_comparison = CompareFunctionStructure(
          prepared.patched_orig_words, base_vram, prepared.patched_orig_words, base_vram);
      outcome.structurally_isomorphic = true;
    } else {
      outcome.structural_comparison = RoundTripRunner::EvaluateStructuralIsomorphism(
          prepared, best_eval.elf_func, best_eval.c_code, /*diagnostic_out=*/nullptr);
      outcome.structurally_isomorphic = outcome.structural_comparison.IsStructurallyIsomorphic();
    }
    outcome.passed = outcome.bit_exact;
    return outcome;
  }
};

class TunerRoundTripTest : public ::testing::TestWithParam<RoundTripTestCase> {
 protected:
  static void SetUpTestSuite() {
    auto runner_or = RoundTripRunner::Create();
    ASSERT_TRUE(runner_or.ok()) << runner_or.status();
    runner = runner_or->release();
  }

  static void TearDownTestSuite() {
    if (runner != nullptr) {
      std::cout << runner->FormatSuiteSummaryReport("TUNER ROUND-TRIP SUITE REPORT");
      delete runner;
      runner = nullptr;
    }
  }

  static RoundTripRunner* runner;
};

RoundTripRunner* TunerRoundTripTest::runner = nullptr;

TEST_P(TunerRoundTripTest, TunesLiftedFunctionToBitExact) {
  ASSERT_NE(runner, nullptr);
  TunerRoundTripStage stage;
  auto outcome_or = runner->RunCase(GetParam(), &stage);
  ASSERT_TRUE(outcome_or.ok()) << outcome_or.status();
  EXPECT_TRUE(outcome_or->bit_exact) << outcome_or->diagnostic;
}

INSTANTIATE_TEST_SUITE_P(SharedRoundTripSuite, TunerRoundTripTest,
                         ::testing::ValuesIn(GetSharedRoundTripTestCases()),
                         [](const ::testing::TestParamInfo<RoundTripTestCase>& info) {
                           return info.param.test_name;
                         });

}  // namespace
}  // namespace rom_nom_nom
