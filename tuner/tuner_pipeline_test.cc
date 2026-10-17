#include "tuner/tuner_pipeline.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "absl/strings/match.h"
#include "core/endian.h"
#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

TEST(TunerPipelineTest, AppliesCachedAstMutationInO1WithoutSpawningCompiler) {
  std::filesystem::path temp_dir =
      std::filesystem::temp_directory_path() / "tuner_pipeline_test_dir";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir / "asm" / "test-game");

  std::filesystem::path config_path = temp_dir / "test-game.textproto";
  {
    std::ofstream out(config_path);
    out << R"pb(
      game_name: "test-game"
      sha1: "0123456789abcdef0123456789abcdef01234567"
      basename: "test-game"
      c_flags {
        key: "default"
        value { flags: "-O2" }
      }
      segments {
        name: "main"
        type: SEGMENT_CODE
        rom_start: 0x1000
        rom_end: 0x1010
        vram: 0x80025C00
        subsegments { name: "code" type: SUBSEGMENT_C rom_start: 0x1000 vram: 0x80025C00 }
      }
      symbols { name: "AddArgs" address: 0x80025C00 type: SYMBOL_FUNC size: 8 }
    )pb";
  }

  // Create a minimal ROM containing `addu $v0, $a0, $a1; jr $ra; nop` at 0x1000
  std::filesystem::path rom_path = temp_dir / "test-game.z64";
  {
    std::vector<uint8_t> rom(0x1010, 0);
    WriteBigEndian32(rom.data(), 0x80371240);
    WriteBigEndian32(rom.data() + 0x1000, 0x00851021);  // addu $v0, $a0, $a1
    WriteBigEndian32(rom.data() + 0x1004, 0x03E00008);  // jr $ra
    WriteBigEndian32(rom.data() + 0x1008, 0x00000000);  // nop
    std::ofstream out(rom_path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(rom.data()), rom.size());
  }

  TunerPipelineOptions opts;
  opts.repo_root = temp_dir;
  opts.game_name = "test-game";
  opts.config_path = config_path;
  opts.rom_path = rom_path;
  opts.search_on_miss = false;
  opts.format_with_clang = false;

  auto tuner_or = TunerPipeline::Create(opts);
  ASSERT_TRUE(tuner_or.ok()) << tuner_or.status();
  auto& tuner = *tuner_or;

  // First run: cache miss with search_on_miss=false returns baseline `arg0 + arg1`
  auto base_res_or = tuner->TuneFunction("AddArgs", "code");
  ASSERT_TRUE(base_res_or.ok()) << base_res_or.status();
  EXPECT_FALSE(base_res_or->from_cache);
  EXPECT_TRUE(absl::StrContains(base_res_or->tuned_c_code, "arg0 + arg1"))
      << base_res_or->tuned_c_code;

  // Populate cache with a commute mutation for AddArgs keyed by its exact `input_sha1`
  FunctionTuningEntry entry;
  entry.set_function_name("AddArgs");
  entry.set_module_name("code");
  entry.set_input_sha1(base_res_or->input_sha1);
  entry.add_c_flags("-O2");
  entry.add_c_flags("-Wa,-O2");
  entry.set_is_bit_exact(true);
  auto* step = entry.add_ast_mutations();
  step->set_kind(AST_MUTATION_COMMUTE_BINARY_OP);
  step->set_site_index(0);
  tuner->MutableCacheStore().Upsert(entry);

  // Second run: hits the cache in O(1) and emits `arg1 + arg0`!
  auto cached_res_or = tuner->TuneFunction("AddArgs", "code");
  ASSERT_TRUE(cached_res_or.ok()) << cached_res_or.status();
  EXPECT_TRUE(cached_res_or->from_cache);
  EXPECT_TRUE(cached_res_or->best_score.raw_bit_exact);
  EXPECT_EQ(cached_res_or->winning_c_flags, (std::vector<std::string>{"-O2", "-Wa,-O2"}));
  EXPECT_TRUE(absl::StrContains(cached_res_or->tuned_c_code, "arg1 + arg0"))
      << cached_res_or->tuned_c_code;

  std::filesystem::remove_all(temp_dir);
}

}  // namespace
}  // namespace rom_nom_nom
