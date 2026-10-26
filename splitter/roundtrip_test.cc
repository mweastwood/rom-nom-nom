#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "core/disassembler.h"
#include "core/elf_reader.h"
#include "core/endian.h"
#include "core/process.h"
#include "core/register_tracker.h"
#include "core/round_trip_runner.h"
#include "core/round_trip_test_cases.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

void WriteTextFile(const std::filesystem::path& path, std::string_view text) {
  std::ofstream file(path, std::ios::out | std::ios::trunc);
  file.write(text.data(), text.size());
}

std::vector<uint32_t> BytesToBigEndianWords(const std::vector<uint8_t>& bytes) {
  std::vector<uint32_t> words;
  words.reserve(bytes.size() / 4);
  for (size_t i = 0; i + 4 <= bytes.size(); i += 4) {
    words.push_back(ReadBigEndian32(bytes.data() + i));
  }
  return words;
}

class SplitterRoundTripStage : public RoundTripStage {
 public:
  std::string_view Name() const override { return "splitter"; }

  absl::StatusOr<RoundTripCaseOutcome> Execute(const PreparedRoundTripCase& prepared) override {
    const ElfSection* orig_text_sec = prepared.orig_elf.FindSection(".text");
    if (orig_text_sec == nullptr || orig_text_sec->data.empty()) {
      return absl::InternalError("SplitterRoundTripStage: Missing or empty .text section.");
    }
    const std::vector<uint8_t>& orig_text = orig_text_sec->data;

    DisassemblerOptions disasm_options;
    disasm_options.emit_line_comments = false;
    disasm_options.emit_function_framing = true;
    Disassembler disassembler(nullptr, disasm_options);

    auto funcs_or = disassembler.DisassembleAllFunctions(orig_text, 0x00000000);
    if (!funcs_or.ok()) {
      return funcs_or.status();
    }
    if (funcs_or->empty()) {
      return absl::InternalError("SplitterRoundTripStage: DisassembleAllFunctions returned empty.");
    }

    std::string combined_asm;
    for (const auto& func : *funcs_or) {
      combined_asm += func.emitted_assembly;
      combined_asm += "\n";
    }

    std::filesystem::path reasm_s = prepared.work_dir / "reasm.s";
    std::filesystem::path reasm_o = prepared.work_dir / "reasm.o";
    WriteTextFile(reasm_s, combined_asm);

    auto as_args = prepared.toolchain->BuildKmcAsArgs(prepared.test_case.options.c_flags,
                                                      {prepared.work_dir}, {}, reasm_s, reasm_o);
    auto as_res_or = RunProcess({
        .program = prepared.toolchain->KmcAs().string(),
        .args = std::move(as_args),
        .working_directory = prepared.toolchain->RepoRoot(),
    });
    if (!as_res_or.ok()) {
      return as_res_or.status();
    }
    if (!as_res_or->Ok()) {
      return absl::InternalError(absl::StrFormat(
          "SplitterRoundTripStage: KMC AS reassembly failed for '%s':\n%s\n%s\nAssembly:\n%s",
          prepared.test_case.function_name, as_res_or->stdout_output, as_res_or->stderr_output,
          combined_asm));
    }

    auto reasm_elf_or = ElfReader::LoadFromFile(reasm_o);
    if (!reasm_elf_or.ok()) {
      return reasm_elf_or.status();
    }
    const ElfSection* reasm_text_sec = reasm_elf_or->FindSection(".text");
    if (reasm_text_sec == nullptr) {
      return absl::InternalError("SplitterRoundTripStage: Missing .text section in reasm.o.");
    }
    const std::vector<uint8_t>& reasm_text = reasm_text_sec->data;

    std::vector<uint32_t> orig_words = BytesToBigEndianWords(orig_text);
    std::vector<uint32_t> reasm_words = BytesToBigEndianWords(reasm_text);

    RoundTripCaseOutcome outcome;
    outcome.test_name = prepared.test_case.test_name;
    outcome.function_name = prepared.test_case.function_name;
    outcome.candidate_c = combined_asm;
    outcome.structural_comparison =
        CompareFunctionStructure(orig_words, 0x00000000, reasm_words, 0x00000000);
    outcome.structurally_isomorphic = outcome.structural_comparison.IsStructurallyIsomorphic();
    outcome.bit_exact = (orig_text == reasm_text);
    outcome.passed = outcome.bit_exact;
    if (!outcome.bit_exact) {
      outcome.diagnostic = absl::StrFormat(
          "Splitter disassembly/reassembly mismatch for '%s' (orig %zu bytes vs reasm %zu bytes)\n"
          "Emitted assembly:\n%s",
          prepared.test_case.function_name, orig_text.size(), reasm_text.size(), combined_asm);
    }
    return outcome;
  }
};

class SplitterRoundTripTest : public ::testing::TestWithParam<RoundTripTestCase> {
 protected:
  static void SetUpTestSuite() {
    auto runner_or = RoundTripRunner::Create();
    ASSERT_TRUE(runner_or.ok()) << runner_or.status();
    runner = runner_or->release();
  }

  static void TearDownTestSuite() {
    if (runner != nullptr) {
      std::cout << runner->FormatSuiteSummaryReport("SPLITTER ROUND-TRIP SUITE REPORT");
      delete runner;
      runner = nullptr;
    }
  }

  static RoundTripRunner* runner;
};

RoundTripRunner* SplitterRoundTripTest::runner = nullptr;

TEST_P(SplitterRoundTripTest, DisassemblesAndReassemblesBitExact) {
  ASSERT_NE(runner, nullptr);
  SplitterRoundTripStage stage;
  auto outcome_or = runner->RunCase(GetParam(), &stage);
  ASSERT_TRUE(outcome_or.ok()) << outcome_or.status();
  EXPECT_TRUE(outcome_or->bit_exact) << outcome_or->diagnostic;
}

INSTANTIATE_TEST_SUITE_P(SharedRoundTripSuite, SplitterRoundTripTest,
                         ::testing::ValuesIn(GetSharedRoundTripTestCases()),
                         [](const ::testing::TestParamInfo<RoundTripTestCase>& info) {
                           return info.param.test_name;
                         });

}  // namespace
}  // namespace rom_nom_nom
