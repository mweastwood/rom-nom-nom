#include "tuner/tuning_cache_store.h"

#include <cstdint>
#include <filesystem>
#include <vector>

#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

TEST(TuningCacheStoreTest, HashChangesOnTargetWordsOrBaselineCodeChange) {
  std::vector<uint32_t> words1 = {0x03E00008, 0x00000000};
  std::vector<uint32_t> words2 = {0x03E00008, 0x24020001};

  std::string h1 = ComputeFunctionTuningHash(words1, "void Foo(void) {}\n");
  std::string h2 = ComputeFunctionTuningHash(words2, "void Foo(void) {}\n");
  std::string h3 = ComputeFunctionTuningHash(words1, "s32 Foo(void) { return 0; }\n");

  EXPECT_NE(h1, h2);
  EXPECT_NE(h1, h3);
  EXPECT_EQ(h1, ComputeFunctionTuningHash(words1, "void Foo(void) {}\n"));
}

TEST(TuningCacheStoreTest, LookupVerifiesSha1AndRoundTripsToFile) {
  TuningCacheStore store;

  FunctionTuningEntry entry;
  entry.set_function_name("AudioUpdate");
  entry.set_module_name("nu_au_stl");
  entry.set_input_sha1("deadbeef");
  entry.add_c_flags("-O2");
  entry.set_is_bit_exact(true);
  auto* step = entry.add_ast_mutations();
  step->set_kind(AST_MUTATION_COMMUTE_BINARY_OP);
  step->set_site_index(2);

  store.Upsert(entry);
  store.SetModuleCFlags("nu_au_stl", {"-O2", "-Wa,-O2"});

  // Matching SHA-1 hits
  const auto* hit = store.Lookup("AudioUpdate", "deadbeef");
  ASSERT_NE(hit, nullptr);
  EXPECT_TRUE(hit->is_bit_exact());
  ASSERT_EQ(hit->ast_mutations_size(), 1);
  EXPECT_EQ(hit->ast_mutations(0).site_index(), 2u);

  // Mismatched SHA-1 misses (self-invalidation)
  EXPECT_EQ(store.Lookup("AudioUpdate", "different_sha1"), nullptr);

  // Round-trip save and load
  std::filesystem::path temp_path =
      std::filesystem::temp_directory_path() / "test_tuning_cache.textproto";
  ASSERT_TRUE(store.SaveToFile(temp_path, "test-game").ok());

  auto loaded_or = TuningCacheStore::LoadFromFile(temp_path);
  ASSERT_TRUE(loaded_or.ok()) << loaded_or.status();
  EXPECT_NE(loaded_or->Lookup("AudioUpdate", "deadbeef"), nullptr);
  EXPECT_EQ(loaded_or->GetModuleCFlags("nu_au_stl"), (std::vector<std::string>{"-O2", "-Wa,-O2"}));

  std::filesystem::remove(temp_path);
}

}  // namespace
}  // namespace rom_nom_nom
