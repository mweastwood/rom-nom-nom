#include "lifter/round_trip_test_util.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "core/c_ast.h"
#include "core/compiler.h"
#include "core/elf_reader.h"
#include "core/round_trip_runner.h"
#include "core/round_trip_test_cases.h"
#include "lifter/c_emitter.h"
#include "lifter/function_loader.h"
#include "lifter/lifter_pipeline.h"

namespace rom_nom_nom {
namespace {

void WriteTextFile(const std::filesystem::path& path, std::string_view content) {
  std::ofstream ofs(path, std::ios::out | std::ios::trunc);
  ofs.write(content.data(), content.size());
}

}  // namespace

RoundTripRunner* RoundTripTest::runner = nullptr;

absl::StatusOr<RoundTripCaseOutcome> LifterRoundTripStage::Execute(
    const PreparedRoundTripCase& prepared) {
  const auto& test_case = prepared.test_case;
  auto loaded_or = FunctionLoader::FromWords(
      test_case.function_name, prepared.patched_orig_words, test_case.options.base_vram,
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

  CTranslationUnit tu;
  tu.includes = {"types.h"};
  tu.symbol_index = prepared.symbol_index.get();
  tu.functions.push_back(std::move(*decompile_or->ast));

  CEmitterOptions emitter_opts;
  emitter_opts.includes = {"types.h"};
  emitter_opts.format_with_clang = false;
  std::string lifted_c = CEmitter::EmitTranslationUnit(tu, emitter_opts);

  std::filesystem::path cand_c_path =
      prepared.work_dir / absl::StrCat("cand_", test_case.function_name, ".c");
  std::filesystem::path cand_o_path =
      prepared.work_dir / absl::StrCat("cand_", test_case.function_name, ".o");
  WriteTextFile(cand_c_path, lifted_c);

  auto cand_compile_status = prepared.compiler->CompileC({
      .src_file = cand_c_path,
      .out_obj = cand_o_path,
      .opt_flags = test_case.options.c_flags,
      .include_dirs = {prepared.work_dir},
  });
  if (!cand_compile_status.ok()) {
    return absl::InternalError(
        absl::StrFormat("Failed to compile lifted C code for '%s':\n%s\nLifted C:\n%s",
                        test_case.function_name, cand_compile_status.message(), lifted_c));
  }

  auto cand_reader_or = ElfReader::LoadFromFile(cand_o_path);
  if (!cand_reader_or.ok()) {
    return cand_reader_or.status();
  }
  auto cand_func_or = cand_reader_or->ExtractFunction(test_case.function_name);
  if (!cand_func_or.ok()) {
    return absl::NotFoundError(
        absl::StrFormat("Function '%s' not found in recompiled lifted object: %s",
                        test_case.function_name, cand_func_or.status().message()));
  }
  const ElfFunction& cand_func = *cand_func_or;

  RoundTripCaseOutcome outcome;
  outcome.test_name = test_case.test_name;
  outcome.function_name = test_case.function_name;
  outcome.candidate_c = lifted_c;
  std::string bit_exact_diag;
  outcome.bit_exact = RoundTripRunner::CompareFunctionsBitExact(
      prepared.orig_func, cand_func, test_case.options.base_vram, prepared.full_orig_c, lifted_c,
      &bit_exact_diag);
  if (outcome.bit_exact) {
    outcome.structural_comparison =
        CompareFunctionStructure(prepared.patched_orig_words, test_case.options.base_vram,
                                 prepared.patched_orig_words, test_case.options.base_vram);
    outcome.structurally_isomorphic = true;
  } else {
    outcome.structural_comparison = RoundTripRunner::EvaluateStructuralIsomorphism(
        prepared, cand_func, lifted_c, &outcome.diagnostic);
    outcome.structurally_isomorphic = outcome.structural_comparison.IsStructurallyIsomorphic();
    if (outcome.diagnostic.empty()) {
      outcome.diagnostic = std::move(bit_exact_diag);
    }
  }
  outcome.passed = outcome.structurally_isomorphic;
  return outcome;
}

void RoundTripTest::SetUpTestSuite() {
  auto runner_or = RoundTripRunner::Create();
  ASSERT_TRUE(runner_or.ok()) << runner_or.status();
  runner = runner_or->release();
}

void RoundTripTest::TearDownTestSuite() {
  if (runner != nullptr) {
    std::cout << runner->FormatSuiteSummaryReport("LIFTER ROUND-TRIP SUITE REPORT");
    delete runner;
    runner = nullptr;
  }
}

void RoundTripTest::AssertRoundTripIsomorphic(const RoundTripTestCase& test_case) {
  ASSERT_NE(runner, nullptr);
  LifterRoundTripStage stage;
  auto outcome_or = runner->RunCase(test_case, &stage);
  ASSERT_TRUE(outcome_or.ok()) << outcome_or.status();
  EXPECT_TRUE(outcome_or->structurally_isomorphic) << outcome_or->diagnostic;
}

void RoundTripTest::AssertRoundTripBitExact(std::string_view func_name, std::string_view original_c,
                                            const RoundTripOptions& options) {
  ASSERT_NE(runner, nullptr);
  RoundTripTestCase test_case{
      .test_name = std::string(func_name),
      .function_name = std::string(func_name),
      .original_c = std::string(original_c),
      .options = options,
  };
  LifterRoundTripStage stage;
  auto outcome_or = runner->RunCase(test_case, &stage);
  ASSERT_TRUE(outcome_or.ok()) << outcome_or.status();
  EXPECT_TRUE(outcome_or->bit_exact) << outcome_or->diagnostic;
}

bool RoundTripTest::CompareFunctions(const ElfFunction& orig, const ElfFunction& cand,
                                     uint32_t base_vram, std::string_view orig_c,
                                     std::string_view lifted_c, std::string* diagnostic_out) {
  return RoundTripRunner::CompareFunctionsBitExact(orig, cand, base_vram, orig_c, lifted_c,
                                                   diagnostic_out);
}

}  // namespace rom_nom_nom
