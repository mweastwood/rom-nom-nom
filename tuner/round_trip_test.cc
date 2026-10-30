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

#include "absl/container/flat_hash_map.h"
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
#include "tuner/candidate_scorer.h"
#include "tuner/tuner_pipeline.h"

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

    absl::flat_hash_map<std::string, ElfFunction> compiled_elf_functions;
    auto make_key = [](const std::vector<std::string>& flags, std::string_view code) {
      return absl::StrCat(absl::StrJoin(flags, " "), "\n", code);
    };

    size_t candidate_counter = 0;
    auto score_candidate =
        [&](std::string_view c_code,
            const std::vector<std::string>& flags) -> absl::StatusOr<CandidateScore> {
      const size_t id = candidate_counter++;
      std::filesystem::path cand_c_path = prepared.work_dir / absl::StrCat("tuner_cand_", id, ".c");
      std::filesystem::path cand_o_path = prepared.work_dir / absl::StrCat("tuner_cand_", id, ".o");
      WriteTextFile(cand_c_path, c_code);

      auto compile_status = prepared.compiler->CompileC({
          .src_file = cand_c_path,
          .out_obj = cand_o_path,
          .opt_flags = flags,
          .include_dirs = {prepared.work_dir},
      });
      if (!compile_status.ok()) {
        return compile_status;
      }

      auto reader_or = ElfReader::LoadFromFile(cand_o_path);
      if (!reader_or.ok()) {
        return reader_or.status();
      }
      auto func_or = reader_or->ExtractFunction(test_case.function_name);
      if (!func_or.ok()) {
        return func_or.status();
      }
      ElfFunction elf_func = std::move(*func_or);
      const bool bit_exact = RoundTripRunner::CompareFunctionsBitExact(
          prepared.orig_func, elf_func, base_vram, prepared.full_orig_c, c_code,
          /*diagnostic_out=*/nullptr);

      std::vector<uint32_t> patched_cand_words =
          RoundTripRunner::ApplyCandidateRelocations(prepared, elf_func);
      std::vector<Instruction> cand_insts;
      auto decoded_cand_or = DecodeSequence(patched_cand_words, base_vram);
      if (decoded_cand_or.ok()) {
        cand_insts = std::move(*decoded_cand_or);
      }
      CandidateScore score = scorer.ScoreInstructions(target, cand_insts, patched_cand_words);
      score.raw_bit_exact = bit_exact;
      if (bit_exact) {
        score.normalized_exact = true;
      }
      compiled_elf_functions[make_key(flags, c_code)] = std::move(elf_func);
      return score;
    };

    FunctionSearchOptions search_opts;
    auto search_res_or = TunerPipeline::SearchFunctionCandidates(
        baseline_ast, test_case.options.c_flags, search_opts,
        [&](const FunctionDeclaration& ast) {
          return EmitFunctionAsTranslationUnit(ast, prepared.symbol_index.get());
        },
        score_candidate);
    if (!search_res_or.ok()) {
      return absl::InternalError(
          absl::StrFormat("TunerRoundTripStage: Failed to compile baseline lifted C for '%s': %s",
                          test_case.function_name, search_res_or.status().message()));
    }

    const ElfFunction& best_elf_func = compiled_elf_functions.at(
        make_key(search_res_or->winning_c_flags, search_res_or->best_c_code));

    RoundTripCaseOutcome outcome;
    outcome.test_name = test_case.test_name;
    outcome.function_name = test_case.function_name;
    outcome.candidate_c = search_res_or->best_c_code;
    outcome.bit_exact = RoundTripRunner::CompareFunctionsBitExact(
        prepared.orig_func, best_elf_func, base_vram, prepared.full_orig_c,
        search_res_or->best_c_code, &outcome.diagnostic);
    if (outcome.bit_exact) {
      outcome.structural_comparison = CompareFunctionStructure(
          prepared.patched_orig_words, base_vram, prepared.patched_orig_words, base_vram);
      outcome.structurally_isomorphic = true;
    } else {
      outcome.structural_comparison = RoundTripRunner::EvaluateStructuralIsomorphism(
          prepared, best_elf_func, search_res_or->best_c_code, /*diagnostic_out=*/nullptr);
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
