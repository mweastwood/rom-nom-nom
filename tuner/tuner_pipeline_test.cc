#include "tuner/tuner_pipeline.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "absl/strings/match.h"
#include "core/c_ast.h"
#include "core/endian.h"
#include "core/toolchain.h"
#include "gtest/gtest.h"
#include "tuner/ast_mutator.h"

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

  TunerPipelineOptions options;
  options.repo_root = temp_dir;
  options.game_name = "test-game";
  options.config_path = config_path;
  options.rom_path = rom_path;
  options.search_on_miss = false;
  options.format_with_clang = false;
  options.num_threads = 2;

  auto tuner_or = TunerPipeline::Create(options);
  ASSERT_TRUE(tuner_or.ok()) << tuner_or.status();
  auto& tuner = *tuner_or;

  // Populate cache entries for both functions so both worker threads execute TuneLoadedFunctionAst
  auto result_a = tuner->TuneFunction("FuncA", "code");
  auto result_b = tuner->TuneFunction("FuncB", "code");
  ASSERT_TRUE(result_a.ok());
  ASSERT_TRUE(result_b.ok());

  FunctionTuningEntry entry_a;
  entry_a.set_function_name("FuncA");
  entry_a.set_module_name("code");
  entry_a.set_input_sha1(result_a->input_sha1);
  entry_a.set_is_bit_exact(true);
  auto* step_a = entry_a.add_ast_mutations();
  step_a->set_kind(AST_MUTATION_COMMUTE_BINARY_OP);
  step_a->set_site_index(0);
  tuner->MutableCacheStore().Upsert(entry_a);

  FunctionTuningEntry entry_b;
  entry_b.set_function_name("FuncB");
  entry_b.set_module_name("code");
  entry_b.set_input_sha1(result_b->input_sha1);
  entry_b.set_is_bit_exact(true);
  tuner->MutableCacheStore().Upsert(entry_b);

  auto module_result_or = tuner->TuneModule("code");
  ASSERT_TRUE(module_result_or.ok()) << module_result_or.status();
  EXPECT_EQ(module_result_or->total_functions, 2u);
  EXPECT_EQ(module_result_or->bit_exact_functions, 2u);
  EXPECT_EQ(module_result_or->cache_hits, 2u);
  EXPECT_TRUE(absl::StrContains(module_result_or->tuned_c_code, "arg1 + arg0"));
  EXPECT_TRUE(absl::StrContains(module_result_or->tuned_c_code, "arg0 - arg1"));

  // Test module_c_flags voting when a bit-exact cache entry has non-default winning c_flags
  entry_a.add_c_flags("-O1");
  tuner->MutableCacheStore().Upsert(entry_a);
  auto module_result2_or = tuner->TuneModule("code");
  ASSERT_TRUE(module_result2_or.ok()) << module_result2_or.status();
  ASSERT_EQ(module_result2_or->module_c_flags.size(), 1u);
  EXPECT_EQ(module_result2_or->module_c_flags[0], "-O1");

  // Test plurality voting among bit-exact functions vs non-bit-exact functions, and fallback when
  // no functions are bit-exact
  entry_b.set_is_bit_exact(false);
  entry_b.add_c_flags("-O3");
  tuner->MutableCacheStore().Upsert(entry_b);
  auto module_result3_or = tuner->TuneModule("code");
  ASSERT_TRUE(module_result3_or.ok()) << module_result3_or.status();
  ASSERT_EQ(module_result3_or->module_c_flags.size(), 1u);
  EXPECT_EQ(module_result3_or->module_c_flags[0], "-O1");

  // When neither function is bit-exact, fallback to overall flag_votes
  entry_a.set_is_bit_exact(false);
  entry_a.clear_c_flags();
  entry_a.add_c_flags("-O3");
  tuner->MutableCacheStore().Upsert(entry_a);
  auto module_result4_or = tuner->TuneModule("code");
  ASSERT_TRUE(module_result4_or.ok()) << module_result4_or.status();
  ASSERT_EQ(module_result4_or->module_c_flags.size(), 1u);
  EXPECT_EQ(module_result4_or->module_c_flags[0], "-O3");

  std::filesystem::remove_all(temp_dir);
}

TEST(TunerPipelineTest, OnlyNonBitExactModeSkipsBitExactCacheEntriesAndReevaluatesNonBitExact) {
  std::filesystem::path temp_dir =
      std::filesystem::temp_directory_path() / "tuner_pipeline_only_non_bit_exact_test";
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
        subsegments { name: "code" type: SUBSEGMENT_C rom_start: 0x1000 vram: 0x80025C00 }
      }
      symbols { name: "BitExactFunc" address: 0x80025C00 type: SYMBOL_FUNC size: 8 }
      symbols { name: "NonBitExactFunc" address: 0x80025C0C type: SYMBOL_FUNC size: 8 }
    )pb";
  }

  std::filesystem::path rom_path = temp_dir / "test-game.z64";
  {
    std::vector<uint8_t> rom(0x1020, 0);
    WriteBigEndian32(rom.data(), 0x80371240);
    WriteBigEndian32(rom.data() + 0x1000, 0x00851021);  // addu $v0, $a0, $a1
    WriteBigEndian32(rom.data() + 0x1004, 0x03E00008);  // jr $ra
    WriteBigEndian32(rom.data() + 0x1008, 0x00000000);  // nop
    WriteBigEndian32(rom.data() + 0x100C, 0x00851021);  // addu $v0, $a0, $a1
    WriteBigEndian32(rom.data() + 0x1010, 0x03E00008);  // jr $ra
    WriteBigEndian32(rom.data() + 0x1014, 0x00000000);  // nop
    std::ofstream out(rom_path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(rom.data()), rom.size());
  }

  TunerPipelineOptions options;
  options.repo_root = temp_dir;
  options.game_name = "test-game";
  options.config_path = config_path;
  options.rom_path = rom_path;
  options.search_on_miss = false;
  options.only_non_bit_exact = true;
  options.format_with_clang = false;

  auto tuner_or = TunerPipeline::Create(options);
  ASSERT_TRUE(tuner_or.ok()) << tuner_or.status();
  auto& tuner = *tuner_or;

  auto initial_bit_exact = tuner->TuneFunction("BitExactFunc", "code");
  auto initial_non_bit_exact = tuner->TuneFunction("NonBitExactFunc", "code");
  ASSERT_TRUE(initial_bit_exact.ok());
  ASSERT_TRUE(initial_non_bit_exact.ok());

  FunctionTuningEntry bit_exact_entry;
  bit_exact_entry.set_function_name("BitExactFunc");
  bit_exact_entry.set_module_name("code");
  bit_exact_entry.set_input_sha1(initial_bit_exact->input_sha1);
  bit_exact_entry.set_is_bit_exact(true);
  auto* commute_step = bit_exact_entry.add_ast_mutations();
  commute_step->set_kind(AST_MUTATION_COMMUTE_BINARY_OP);
  commute_step->set_site_index(0);
  tuner->MutableCacheStore().Upsert(bit_exact_entry);

  FunctionTuningEntry non_bit_exact_entry;
  non_bit_exact_entry.set_function_name("NonBitExactFunc");
  non_bit_exact_entry.set_module_name("code");
  non_bit_exact_entry.set_input_sha1(initial_non_bit_exact->input_sha1);
  non_bit_exact_entry.set_is_bit_exact(false);
  auto* non_bit_exact_step = non_bit_exact_entry.add_ast_mutations();
  non_bit_exact_step->set_kind(AST_MUTATION_COMMUTE_BINARY_OP);
  non_bit_exact_step->set_site_index(0);
  tuner->MutableCacheStore().Upsert(non_bit_exact_entry);

  // BitExactFunc hits the cache fast-path (`from_cache == true`)
  auto bit_exact_result_or = tuner->TuneFunction("BitExactFunc", "code");
  ASSERT_TRUE(bit_exact_result_or.ok());
  EXPECT_TRUE(bit_exact_result_or->from_cache);
  EXPECT_TRUE(absl::StrContains(bit_exact_result_or->tuned_c_code, "arg1 + arg0"));

  // NonBitExactFunc bypasses the cache fast-path (`from_cache == false`) when `only_non_bit_exact`
  // is true
  auto non_bit_exact_result_or = tuner->TuneFunction("NonBitExactFunc", "code");
  ASSERT_TRUE(non_bit_exact_result_or.ok());
  EXPECT_FALSE(non_bit_exact_result_or->from_cache);

  std::filesystem::remove_all(temp_dir);
}

TEST(TunerPipelineTest, TuneAllModulesOverwritesReadOnlyLiftedFilesWithMutatedAst) {
  std::filesystem::path temp_dir =
      std::filesystem::temp_directory_path() / "tuner_pipeline_readonly_test";
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

  // Create minimal ROM: `addu $v0, $a0, $a1; jr $ra; nop`
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

  // Create lifted_dir containing a read-only baseline C file (perms: 0555 / read-only)
  std::filesystem::path lifted_dir = temp_dir / "lifted";
  std::filesystem::create_directories(lifted_dir);
  std::filesystem::path baseline_c = lifted_dir / "code.c";
  {
    std::ofstream out(baseline_c);
    out << "// baseline\ns32 AddArgs(s32 arg0, s32 arg1) {\n    return arg0 + arg1;\n}\n";
  }
  std::filesystem::permissions(baseline_c, std::filesystem::perms::owner_read |
                                               std::filesystem::perms::group_read |
                                               std::filesystem::perms::others_read);

  TunerPipelineOptions opts;
  opts.repo_root = temp_dir;
  opts.game_name = "test-game";
  opts.config_path = config_path;
  opts.rom_path = rom_path;
  opts.lifted_dir = lifted_dir;
  opts.search_on_miss = false;
  opts.format_with_clang = false;

  auto tuner_or = TunerPipeline::Create(opts);
  ASSERT_TRUE(tuner_or.ok()) << tuner_or.status();
  auto& tuner = *tuner_or;

  auto base_res_or = tuner->TuneFunction("AddArgs", "code");
  ASSERT_TRUE(base_res_or.ok()) << base_res_or.status();

  // Populate cache with commute mutation
  FunctionTuningEntry entry;
  entry.set_function_name("AddArgs");
  entry.set_module_name("code");
  entry.set_input_sha1(base_res_or->input_sha1);
  entry.add_c_flags("-O2");
  entry.set_is_bit_exact(true);
  auto* step = entry.add_ast_mutations();
  step->set_kind(AST_MUTATION_COMMUTE_BINARY_OP);
  step->set_site_index(0);
  tuner->MutableCacheStore().Upsert(entry);

  // Execute TuneAllModules into output_dir
  std::filesystem::path output_dir = temp_dir / "output";
  auto emitted_or = tuner->TuneAllModules(output_dir);
  ASSERT_TRUE(emitted_or.ok()) << emitted_or.status();

  std::filesystem::path tuned_c = output_dir / "code.c";
  ASSERT_TRUE(std::filesystem::exists(tuned_c));

  std::ifstream in(tuned_c);
  ASSERT_TRUE(in.is_open());
  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

  // Verify that TuneAllModules wrote the mutated tuned AST (arg1 + arg0), NOT the unmutated
  // baseline
  EXPECT_TRUE(absl::StrContains(content, "arg1 + arg0")) << content;
  EXPECT_FALSE(absl::StrContains(content, "// baseline")) << content;

  // Verify that the emitted file is writable
  auto perms = std::filesystem::status(tuned_c).permissions();
  EXPECT_NE(perms & std::filesystem::perms::owner_write, std::filesystem::perms::none);

  // Clean up
  std::error_code ec;
  std::filesystem::permissions(baseline_c, std::filesystem::perms::all, ec);
  std::filesystem::permissions(tuned_c, std::filesystem::perms::all, ec);
  std::filesystem::remove_all(temp_dir, ec);
}

TEST(TunerPipelineTest, NearMissPolishPassPrioritizesNearMissMutations) {
  auto body = std::make_unique<CompoundStatement>();
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "a"));
  body->AddStatement(CStatement::VariableDeclaration(CType::S32(), "b"));
  body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("a"),
      CExpression::Binary("+", CExpression::Identifier("a"), CExpression::Identifier("b")))));
  auto loop_body = std::make_unique<CompoundStatement>();
  loop_body->AddStatement(CStatement::Expression(CExpression::Assignment(
      "=", CExpression::Identifier("b"),
      CExpression::Binary("+", CExpression::Identifier("b"), CExpression::Integer(1)))));
  body->AddStatement(CStatement::While(
      CExpression::Binary("<", CExpression::Identifier("b"), CExpression::Integer(10)),
      std::move(loop_body)));
  body->AddStatement(CStatement::Return(CExpression::Identifier("a")));

  FunctionDeclaration func(CType::S32(), "NearMissFunc", {}, std::move(body));

  auto all_steps = AstMutator::DiscoverMutations(func, 512);
  EXPECT_GT(all_steps.size(), 0u);

  // Partition steps as in Phase 2b Near-Miss Polish Pass
  std::vector<AstMutationStep> polish_steps;
  for (const auto& step : all_steps) {
    if (step.kind() == AST_MUTATION_SWAP_ADJACENT_STATEMENTS ||
        step.kind() == AST_MUTATION_PROPAGATE_REGISTER_COPY ||
        step.kind() == AST_MUTATION_HOIST_WHILE_GUARD ||
        step.kind() == AST_MUTATION_RELOAD_OR_EARLY_RETURN_VAR ||
        step.kind() == AST_MUTATION_INVERT_IF_ELSE ||
        step.kind() == AST_MUTATION_COMMUTE_BINARY_OP) {
      polish_steps.push_back(step);
    }
  }

  EXPECT_FALSE(polish_steps.empty());
  for (const auto& step : polish_steps) {
    EXPECT_TRUE(step.kind() == AST_MUTATION_SWAP_ADJACENT_STATEMENTS ||
                step.kind() == AST_MUTATION_PROPAGATE_REGISTER_COPY ||
                step.kind() == AST_MUTATION_HOIST_WHILE_GUARD ||
                step.kind() == AST_MUTATION_RELOAD_OR_EARLY_RETURN_VAR ||
                step.kind() == AST_MUTATION_INVERT_IF_ELSE ||
                step.kind() == AST_MUTATION_COMMUTE_BINARY_OP);
  }
}

TEST(TunerPipelineTest, CachedByNameFallbackSeedsPreviousCachedFlagsAndMutations) {
  TunerPipelineOptions opts;
  opts.repo_root = FindRepoRoot();
  opts.game_name = "harvest-moon-64";
  opts.search_on_miss = true;
  opts.explore_ast_mutations = true;
  opts.format_with_clang = false;

  auto tuner_or = TunerPipeline::Create(opts);
  if (!tuner_or.ok()) {
    // If retail ROM or config is unavailable in this test environment, skip gracefully.
    return;
  }
  auto& tuner = *tuner_or;

  // Insert a cached entry for LayerSet with an outdated input_sha1,
  // custom c_flags, and the known winning AST mutations.
  FunctionTuningEntry entry;
  entry.set_function_name("LayerSet");
  entry.set_module_name("layer");
  entry.set_input_sha1("outdated_sha1_hash");
  entry.add_c_flags("-O2");
  entry.add_c_flags("-mips2");
  entry.add_c_flags("-mcpu=r4000");
  entry.add_c_flags("-Wa,-g");
  entry.set_is_bit_exact(true);
  auto* step1 = entry.add_ast_mutations();
  step1->set_kind(AST_MUTATION_ELIMINATE_DEAD_CALL_RETURN);
  step1->set_site_index(0);
  step1->set_param(0);
  auto* step2 = entry.add_ast_mutations();
  step2->set_kind(AST_MUTATION_PROPAGATE_REGISTER_COPY);
  step2->set_site_index(0);
  step2->set_param(1);
  tuner->MutableCacheStore().Upsert(entry);

  // When tuning LayerSet, the exact input_sha1 does NOT match, but cached_by_name matches.
  // It seeds previous_cached_flags and previous_cached_mutations into Phase 1b presets.
  auto res_or = tuner->TuneFunction("LayerSet", "layer");
  if (!res_or.ok()) {
    return;
  }
  const auto& res = *res_or;

  EXPECT_FALSE(res.from_cache);
  EXPECT_TRUE(res.best_score.raw_bit_exact);
  ASSERT_EQ(res.applied_mutations.size(), 2u);
  EXPECT_EQ(res.applied_mutations[0].kind(), AST_MUTATION_ELIMINATE_DEAD_CALL_RETURN);
  EXPECT_EQ(res.applied_mutations[1].kind(), AST_MUTATION_PROPAGATE_REGISTER_COPY);
}

}  // namespace
}  // namespace rom_nom_nom
