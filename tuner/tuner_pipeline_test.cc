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

TEST(TunerPipelineTest, ExplicitCachePathDoesNotFallBackToDefaultGameConfigCache) {
  std::filesystem::path temp_dir =
      std::filesystem::temp_directory_path() / "tuner_pipeline_explicit_cache_test";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir / "config");
  std::filesystem::create_directories(temp_dir / "asm" / "test-game");

  std::filesystem::path config_path = temp_dir / "config" / "test-game.textproto";
  {
    std::ofstream out(config_path);
    out << R"pb(
      game_name: "test-game"
      sha1: "0123456789abcdef0123456789abcdef01234567"
      basename: "test-game"
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

  // Write a default `config/test-game.tuning.textproto` containing an entry
  {
    std::ofstream out(temp_dir / "config" / "test-game.tuning.textproto");
    out << R"pb(
      game_name: "test-game"
      entries {
        function_name: "AddArgs"
        module_name: "code"
        input_sha1: "dummy"
        is_bit_exact: true
      }
    )pb";
  }

  std::filesystem::path rom_path = temp_dir / "test-game.z64";
  {
    std::vector<uint8_t> rom(0x1010, 0);
    WriteBigEndian32(rom.data(), 0x80371240);
    WriteBigEndian32(rom.data() + 0x1000, 0x00851021);
    WriteBigEndian32(rom.data() + 0x1004, 0x03E00008);
    std::ofstream out(rom_path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(rom.data()), rom.size());
  }

  TunerPipelineOptions opts;
  opts.repo_root = temp_dir;
  opts.game_name = "test-game";
  opts.config_path = config_path;
  opts.rom_path = rom_path;
  opts.cache_path = temp_dir / "fresh_nonexistent_cache.textproto";
  opts.search_on_miss = false;

  auto tuner_or = TunerPipeline::Create(opts);
  ASSERT_TRUE(tuner_or.ok()) << tuner_or.status();
  EXPECT_EQ((*tuner_or)->CacheStore().Size(), 0u);

  std::filesystem::remove_all(temp_dir);
}

TEST(TunerPipelineTest, TuneModuleParallelExecutesMultipleFunctionsAcrossWorkerThreads) {
  std::filesystem::path temp_dir =
      std::filesystem::temp_directory_path() / "tuner_pipeline_parallel_module_test";
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
        rom_end: 0x1020
        vram: 0x80025C00
        subsegments { name: "code" type: SUBSEGMENT_ASM rom_start: 0x1000 vram: 0x80025C00 }
      }
      symbols { name: "FuncA" address: 0x80025C00 type: SYMBOL_FUNC size: 8 }
      symbols { name: "FuncB" address: 0x80025C08 type: SYMBOL_FUNC size: 8 }
    )pb";
  }

  // Write assembly module containing FuncA and FuncB
  {
    std::ofstream out(temp_dir / "asm" / "test-game" / "code.s");
    out << ".section .text, \"ax\"\n"
           "glabel FuncA\n"
           "/* 001000 80025C00 00851021 */ addu $v0, $a0, $a1\n"
           "/* 001004 80025C04 03E00008 */ jr $ra\n"
           "/* 001008 80025C08 00000000 */ nop\n"
           "glabel FuncB\n"
           "/* 00100C 80025C0C 00851023 */ subu $v0, $a0, $a1\n"
           "/* 001010 80025C10 03E00008 */ jr $ra\n"
           "/* 001014 80025C14 00000000 */ nop\n";
  }

  std::filesystem::path rom_path = temp_dir / "test-game.z64";
  {
    std::vector<uint8_t> rom(0x1020, 0);
    WriteBigEndian32(rom.data(), 0x80371240);
    WriteBigEndian32(rom.data() + 0x1000, 0x00851021);
    WriteBigEndian32(rom.data() + 0x1004, 0x03E00008);
    WriteBigEndian32(rom.data() + 0x1008, 0x00000000);
    WriteBigEndian32(rom.data() + 0x100C, 0x00851023);
    WriteBigEndian32(rom.data() + 0x1010, 0x03E00008);
    WriteBigEndian32(rom.data() + 0x1014, 0x00000000);
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
  opts.num_threads = 2;

  auto tuner_or = TunerPipeline::Create(opts);
  ASSERT_TRUE(tuner_or.ok()) << tuner_or.status();
  auto& tuner = *tuner_or;

  // Populate cache entries for both functions so both worker threads execute TuneLoadedFunctionAst
  auto res_a = tuner->TuneFunction("FuncA", "code");
  auto res_b = tuner->TuneFunction("FuncB", "code");
  ASSERT_TRUE(res_a.ok());
  ASSERT_TRUE(res_b.ok());

  FunctionTuningEntry entry_a;
  entry_a.set_function_name("FuncA");
  entry_a.set_module_name("code");
  entry_a.set_input_sha1(res_a->input_sha1);
  entry_a.set_is_bit_exact(true);
  auto* step_a = entry_a.add_ast_mutations();
  step_a->set_kind(AST_MUTATION_COMMUTE_BINARY_OP);
  step_a->set_site_index(0);
  tuner->MutableCacheStore().Upsert(entry_a);

  FunctionTuningEntry entry_b;
  entry_b.set_function_name("FuncB");
  entry_b.set_module_name("code");
  entry_b.set_input_sha1(res_b->input_sha1);
  entry_b.set_is_bit_exact(true);
  tuner->MutableCacheStore().Upsert(entry_b);

  auto mod_res_or = tuner->TuneModule("code");
  ASSERT_TRUE(mod_res_or.ok()) << mod_res_or.status();
  EXPECT_EQ(mod_res_or->total_functions, 2u);
  EXPECT_EQ(mod_res_or->bit_exact_functions, 2u);
  EXPECT_EQ(mod_res_or->cache_hits, 2u);
  EXPECT_TRUE(absl::StrContains(mod_res_or->tuned_c_code, "arg1 + arg0"));
  EXPECT_TRUE(absl::StrContains(mod_res_or->tuned_c_code, "arg0 - arg1"));

  std::filesystem::remove_all(temp_dir);
}

}  // namespace
}  // namespace rom_nom_nom
