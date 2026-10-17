#include "core/symbol_registry.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"

namespace rom_nom_nom {
namespace {

constexpr std::string_view kValidSymbolsText = R"pb(
  entries {
    name: "Entrypoint"
    address: 0x80025C00
    type: SYMBOL_FUNC
    description: "Initial bootstrap code"
  }
  entries { name: "AudioUpdate" address: 0x8003CF38 type: SYMBOL_FUNC size: 0x318 }
  entries { name: "g_audio_voices" address: 0x801FB690 type: SYMBOL_DATA size: 0x80 }
  entries { name: "LeoBootReset" address: 0x84001008 type: SYMBOL_FUNC is_absolute: true }
  entries { name: ".L80025C20" address: 0x80025C20 type: SYMBOL_LABEL }
)pb";

TEST(SymbolRegistryTest, ParseValidTextproto) {
  auto index_or = SymbolIndex::ParseFromTextproto(kValidSymbolsText);
  ASSERT_TRUE(index_or.ok()) << index_or.status();

  const SymbolIndex& index = *index_or;
  EXPECT_EQ(index.Size(), 5u);
  EXPECT_FALSE(index.Empty());

  // By Address
  const SymbolRegistryEntry* entry = index.FindByAddress(0x80025C00);
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->name(), "Entrypoint");
  EXPECT_EQ(entry->type(), SYMBOL_FUNC);
  EXPECT_EQ(entry->description(), "Initial bootstrap code");

  // By Name
  const SymbolRegistryEntry* audio = index.FindByName("AudioUpdate");
  ASSERT_NE(audio, nullptr);
  EXPECT_EQ(audio->address(), 0x8003CF38u);
  EXPECT_EQ(audio->size(), 0x318u);

  // Absolute symbol
  const SymbolRegistryEntry* leo = index.FindByName("LeoBootReset");
  ASSERT_NE(leo, nullptr);
  EXPECT_TRUE(leo->is_absolute());

  // HasAddress / HasName
  EXPECT_TRUE(index.HasAddress(0x801FB690));
  EXPECT_TRUE(index.HasName("g_audio_voices"));
  EXPECT_FALSE(index.HasAddress(0x12345678));
  EXPECT_FALSE(index.HasName("UnknownSym"));
}

TEST(SymbolRegistryTest, LookupOrSynthesizeName) {
  auto index_or = SymbolIndex::ParseFromTextproto(kValidSymbolsText);
  ASSERT_TRUE(index_or.ok());
  const SymbolIndex& index = *index_or;

  // Registered symbols resolve directly
  EXPECT_EQ(index.LookupOrSynthesizeName(0x80025C00), "Entrypoint");
  EXPECT_EQ(index.LookupOrSynthesizeName(0x8003CF38), "AudioUpdate");
  EXPECT_EQ(index.LookupOrSynthesizeName(0x801FB690), "g_audio_voices");

  // Unregistered symbols use fallback naming
  EXPECT_EQ(index.LookupOrSynthesizeName(0x807FFF00, SYMBOL_FUNC), "func_807FFF00");
  EXPECT_EQ(index.LookupOrSynthesizeName(0x807FFF00, SYMBOL_LABEL), ".L807FFF00");
  EXPECT_EQ(index.LookupOrSynthesizeName(0x807FFF00, SYMBOL_DATA), "D_807FFF00");
}

TEST(SymbolRegistryTest, SortedEntries) {
  auto index_or = SymbolIndex::ParseFromTextproto(kValidSymbolsText);
  ASSERT_TRUE(index_or.ok());
  const SymbolIndex& index = *index_or;

  std::vector<const SymbolRegistryEntry*> sorted = index.SortedEntries();
  ASSERT_EQ(sorted.size(), 5u);
  EXPECT_EQ(sorted[0]->address(), 0x80025C00u);
  EXPECT_EQ(sorted[1]->address(), 0x80025C20u);
  EXPECT_EQ(sorted[2]->address(), 0x8003CF38u);
  EXPECT_EQ(sorted[3]->address(), 0x801FB690u);
  EXPECT_EQ(sorted[4]->address(), 0x84001008u);
}

TEST(SymbolRegistryTest, RejectDuplicateAddress) {
  constexpr std::string_view kDupAddr = R"pb(
    entries { name: "sym1" address: 0x80001000 type: SYMBOL_FUNC }
    entries { name: "sym2" address: 0x80001000 type: SYMBOL_DATA }
  )pb";
  auto status_or = SymbolIndex::ParseFromTextproto(kDupAddr);
  EXPECT_FALSE(status_or.ok());
  EXPECT_EQ(status_or.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(SymbolRegistryTest, RejectDuplicateName) {
  constexpr std::string_view kDupName = R"pb(
    entries { name: "SameName" address: 0x80001000 type: SYMBOL_FUNC }
    entries { name: "SameName" address: 0x80002000 type: SYMBOL_FUNC }
  )pb";
  auto status_or = SymbolIndex::ParseFromTextproto(kDupName);
  EXPECT_FALSE(status_or.ok());
  EXPECT_EQ(status_or.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(SymbolRegistryTest, RejectUnspecifiedType) {
  constexpr std::string_view kNoType = R"pb(
    entries { name: "sym" address: 0x80001000 type: SYMBOL_TYPE_UNSPECIFIED }
  )pb";
  EXPECT_FALSE(SymbolIndex::ParseFromTextproto(kNoType).ok());
}

TEST(SymbolRegistryTest, RejectEmptyName) {
  constexpr std::string_view kEmptyName = R"pb(
    entries { name: "" address: 0x80001000 type: SYMBOL_FUNC }
  )pb";
  EXPECT_FALSE(SymbolIndex::ParseFromTextproto(kEmptyName).ok());
}

TEST(SymbolRegistryTest, LoadFromFile) {
  const std::filesystem::path temp_path =
      std::filesystem::temp_directory_path() / "test_symbols.textproto";
  {
    std::ofstream out(temp_path);
    out << kValidSymbolsText;
  }

  auto index_or = SymbolIndex::LoadFromTextproto(temp_path);
  ASSERT_TRUE(index_or.ok()) << index_or.status();
  EXPECT_EQ(index_or->Size(), 5u);
  EXPECT_TRUE(index_or->HasName("Entrypoint"));

  std::filesystem::remove(temp_path);

  // Non-existent file
  auto bad_file_or = SymbolIndex::LoadFromTextproto("non_existent_file.textproto");
  EXPECT_FALSE(bad_file_or.ok());
  EXPECT_EQ(bad_file_or.status().code(), absl::StatusCode::kNotFound);
}

TEST(SymbolRegistryTest, MoveSemantics) {
  auto index_or = SymbolIndex::ParseFromTextproto(kValidSymbolsText);
  ASSERT_TRUE(index_or.ok());

  SymbolIndex index1 = std::move(*index_or);
  EXPECT_EQ(index1.Size(), 5u);

  SymbolIndex index2 = std::move(index1);
  EXPECT_EQ(index2.Size(), 5u);
  EXPECT_TRUE(index2.HasName("Entrypoint"));
  EXPECT_EQ(index2.FindByAddress(0x80025C00)->name(), "Entrypoint");
}

TEST(SymbolRegistryTest, FromGameConfigLoadsExternalSymbolFilesAndSortsEntries) {
  const std::filesystem::path ext_sym_path =
      std::filesystem::temp_directory_path() / "test_external_game_symbols.textproto";
  {
    std::ofstream out(ext_sym_path);
    out << R"pb(
      entries { name: "ExtLateFunc" address: 0x80050000 type: SYMBOL_FUNC }
      entries { name: "ExtEarlyFunc" address: 0x80010000 type: SYMBOL_FUNC }
      entries { name: "InlineOverrideDupAddr" address: 0x80030000 type: SYMBOL_FUNC }
    )pb";
  }

  GameConfig config;
  auto* inline_sym1 = config.add_symbols();
  inline_sym1->set_name("InlineMidFunc");
  inline_sym1->set_address(0x80030000);
  inline_sym1->set_type(SYMBOL_FUNC);

  auto* inline_sym2 = config.add_symbols();
  inline_sym2->set_name("InlineData");
  inline_sym2->set_address(0x80020000);
  inline_sym2->set_type(SYMBOL_DATA);

  config.add_symbol_files(ext_sym_path.string());

  auto index_or = SymbolIndex::FromGameConfig(config);
  ASSERT_TRUE(index_or.ok()) << index_or.status();

  const SymbolIndex& index = *index_or;
  EXPECT_EQ(index.Size(), 4u);
  EXPECT_TRUE(index.HasName("ExtEarlyFunc"));
  EXPECT_TRUE(index.HasName("InlineData"));
  EXPECT_TRUE(index.HasName("InlineMidFunc"));
  EXPECT_TRUE(index.HasName("ExtLateFunc"));
  // Duplicate address from external file should be ignored in favor of inline symbol.
  EXPECT_FALSE(index.HasName("InlineOverrideDupAddr"));

  const auto& sorted = index.SortedEntries();
  ASSERT_EQ(sorted.size(), 4u);
  EXPECT_EQ(sorted[0]->address(), 0x80010000u);
  EXPECT_EQ(sorted[0]->name(), "ExtEarlyFunc");
  EXPECT_EQ(sorted[1]->address(), 0x80020000u);
  EXPECT_EQ(sorted[1]->name(), "InlineData");
  EXPECT_EQ(sorted[2]->address(), 0x80030000u);
  EXPECT_EQ(sorted[2]->name(), "InlineMidFunc");
  EXPECT_EQ(sorted[3]->address(), 0x80050000u);
  EXPECT_EQ(sorted[3]->name(), "ExtLateFunc");

  std::filesystem::remove(ext_sym_path);
}

TEST(SymbolRegistryTest, ResolveSymbolOrSynthesizedAddressResolvesAllForms) {
  auto index_or = SymbolIndex::ParseFromTextproto(kValidSymbolsText);
  ASSERT_TRUE(index_or.ok());
  const SymbolIndex& index = *index_or;

  EXPECT_EQ(ResolveSymbolOrSynthesizedAddress("AudioUpdate", &index), 0x8003CF38u);
  EXPECT_EQ(ResolveSymbolOrSynthesizedAddress("0x80025C00", &index), 0x80025C00u);
  EXPECT_EQ(ResolveSymbolOrSynthesizedAddress("0X80025C00", nullptr), 0x80025C00u);
  EXPECT_EQ(ResolveSymbolOrSynthesizedAddress("func_80041234", nullptr), 0x80041234u);
  EXPECT_EQ(ResolveSymbolOrSynthesizedAddress("D_801FB600", nullptr), 0x801FB600u);
  EXPECT_EQ(ResolveSymbolOrSynthesizedAddress(".L80025C20", nullptr), 0x80025C20u);
  EXPECT_EQ(ResolveSymbolOrSynthesizedAddress("jtbl_8009ABC0", nullptr), 0x8009ABC0u);
  EXPECT_EQ(ResolveSymbolOrSynthesizedAddress("80025C00", nullptr), 0x80025C00u);

  EXPECT_EQ(ResolveSymbolOrSynthesizedAddress("UnknownSymbol", &index), std::nullopt);
  EXPECT_EQ(ResolveSymbolOrSynthesizedAddress("", &index), std::nullopt);
}

TEST(SymbolRegistryTest, LoadSymbolIndexPriorityAndFallbackDiscovery) {
  const std::filesystem::path temp_root =
      std::filesystem::temp_directory_path() / "test_symbol_load_root";
  std::filesystem::remove_all(temp_root);
  std::filesystem::create_directories(temp_root / "configs");

  const std::filesystem::path fallback_path = temp_root / "configs" / "my-game.symbols.textproto";
  {
    std::ofstream out(fallback_path);
    out << R"pb(
      entries { name: "FallbackSym" address: 0x80090000 type: SYMBOL_FUNC }
    )pb";
  }

  // 1. Text override takes highest priority
  auto override_or = LoadSymbolIndex({
      .symbols_text_override = std::string(kValidSymbolsText),
      .repo_root = temp_root,
      .game_name = "my-game",
  });
  ASSERT_TRUE(override_or.ok()) << override_or.status();
  EXPECT_TRUE(override_or->HasName("Entrypoint"));
  EXPECT_FALSE(override_or->HasName("FallbackSym"));

  // 2. Fallback discovery in configs/<game>.symbols.textproto
  auto fallback_or = LoadSymbolIndex({
      .repo_root = temp_root,
      .game_name = "my-game",
  });
  ASSERT_TRUE(fallback_or.ok()) << fallback_or.status();
  EXPECT_TRUE(fallback_or->HasName("FallbackSym"));

  std::filesystem::remove_all(temp_root);
}

}  // namespace
}  // namespace rom_nom_nom
