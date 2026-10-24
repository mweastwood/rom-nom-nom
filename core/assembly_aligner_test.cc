#include "core/assembly_aligner.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/strings/str_join.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

class AssemblyAlignerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    test_dir_ = std::filesystem::temp_directory_path() / "assembly_aligner_test";
    std::filesystem::remove_all(test_dir_);
    std::filesystem::create_directories(test_dir_);
  }

  void TearDown() override { std::filesystem::remove_all(test_dir_); }

  std::filesystem::path test_dir_;
};

TEST_F(AssemblyAlignerTest, ReadAssemblyLinesRetainsNewlinesAndReturnsNotFoundOnMissingFile) {
  std::filesystem::path asm_path = test_dir_ / "test.s";
  {
    std::ofstream out(asm_path, std::ios::binary);
    out << "\t.text\n"
        << "\t.globl\tFuncA\n"
        << "FuncA:\n"
        << "\tjr\t$31\n";
  }

  auto lines_or = ReadAssemblyLines(asm_path);
  ASSERT_TRUE(lines_or.ok()) << lines_or.status();
  EXPECT_THAT(*lines_or, ElementsAre("\t.text\n", "\t.globl\tFuncA\n", "FuncA:\n", "\tjr\t$31\n"));

  auto missing_or = ReadAssemblyLines(test_dir_ / "nonexistent.s");
  EXPECT_FALSE(missing_or.ok());
}

TEST_F(AssemblyAlignerTest, ParseAssemblyFunctionsExtractsEntToEndAndTrailingSizeSpans) {
  std::vector<std::string> lines = {
      "\t.file\t1 \"module.c\"\n",
      "\t.text\n",
      "\t.align\t2\n",
      "\t.globl\tFuncOne\n",
      "\t.ent\tFuncOne\n",
      "FuncOne:\n",
      "\taddiu\t$2,$4,1\n",
      "\tjr\t$31\n",
      "\t.end\tFuncOne\n",
      "\t.size\tFuncOne, .-FuncOne\n",
      "\t.align\t2\n",
      "\t.globl\tFuncTwo\n",
      "\t.ent\tFuncTwo\n",
      "FuncTwo:\n",
      ".L2:\n",
      "\taddu\t$2,$4,$5\n",
      "\tsll\t$2,$2,2\n",
      "\tjr\t$31\n",
      "\t.end\tFuncTwo\n",
  };

  auto spans = ParseAssemblyFunctions(lines);
  ASSERT_EQ(spans.size(), 2u);

  ASSERT_TRUE(spans.contains("FuncOne"));
  EXPECT_EQ(spans.at("FuncOne").name, "FuncOne");
  EXPECT_EQ(spans.at("FuncOne").start_line, 4u);
  EXPECT_EQ(spans.at("FuncOne").end_line, 9u);

  ASSERT_TRUE(spans.contains("FuncTwo"));
  EXPECT_EQ(spans.at("FuncTwo").name, "FuncTwo");
  EXPECT_EQ(spans.at("FuncTwo").start_line, 12u);
  EXPECT_EQ(spans.at("FuncTwo").end_line, 18u);
}

TEST_F(AssemblyAlignerTest, SplicePerFunctionAssemblyNamespacesLocalLabelsAndWrapsNomoveFunctions) {
  std::filesystem::path default_s = test_dir_ / "default.s272";
  std::filesystem::path alt_s = test_dir_ / "alt.s272";

  {
    std::ofstream out(default_s);
    out << "\t.text\n"
        << "\t.align\t2\n"
        << "\t.globl\tKeepDefaultFunc\n"
        << "\t.ent\tKeepDefaultFunc\n"
        << "KeepDefaultFunc:\n"
        << "\tbne\t$4,$0,.L1\n"
        << "\taddu\t$2,$0,$0\n"
        << ".L1:\n"
        << "\tjr\t$31\n"
        << "\t.end\tKeepDefaultFunc\n"
        << "\t.align\t2\n"
        << "\t.globl\tSplicedFunc\n"
        << "\t.ent\tSplicedFunc\n"
        << "SplicedFunc:\n"
        << "\tjr\t$31\n"
        << "\t.end\tSplicedFunc\n";
  }

  {
    std::ofstream out(alt_s);
    out << "\t.text\n"
        << "\t.align\t2\n"
        << "\t.globl\tSplicedFunc\n"
        << "\t.ent\tSplicedFunc\n"
        << "SplicedFunc:\n"
        << "\tbeq\t$4,$0,.L1\n"
        << ".L10:\n"
        << "\taddiu\t$2,$0,7\n"
        << ".L1:\n"
        << "\tjr\t$31\n"
        << "\t.end\tSplicedFunc\n";
  }

  std::vector<std::filesystem::path> s272_files = {default_s, alt_s};
  std::vector<std::vector<std::string>> unique_flag_sets = {
      {"-O2", "-Wa,-O2"},
      {"-O0", "-Wa,-g"},
  };
  absl::flat_hash_map<std::string, size_t> func_to_flag_set = {
      {"KeepDefaultFunc", 0},
      {"SplicedFunc", 1},
  };

  auto spliced_or = SplicePerFunctionAssembly(s272_files, unique_flag_sets, func_to_flag_set,
                                              /*overall_has_wa_opt=*/true);
  ASSERT_TRUE(spliced_or.ok()) << spliced_or.status();
  std::string content = absl::StrJoin(*spliced_or, "");

  // Default function retains its original local label .L1 without .set nomove
  EXPECT_THAT(content, HasSubstr("KeepDefaultFunc:\n\tbne\t$4,$0,.L1\n"));
  // Spliced function from flag set 1 has .L1 namespaced to .L_sp_SplicedFunc_1 (without corrupting
  // .L10 via prefix collision) and is wrapped in .set nomove / .set move
  EXPECT_THAT(content,
              HasSubstr("SplicedFunc:\n\t.set\tnomove\n\tbeq\t$4,$0,.L_sp_SplicedFunc_1\n"));
  EXPECT_THAT(content, HasSubstr(".L_sp_SplicedFunc_10:\n"));
  EXPECT_THAT(content,
              HasSubstr(".L_sp_SplicedFunc_1:\n\tjr\t$31\n\t.set\tmove\n\t.end\tSplicedFunc\n"));
}

}  // namespace
}  // namespace rom_nom_nom
