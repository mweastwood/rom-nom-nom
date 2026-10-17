#include "fuzzer/candidate_environment.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/endian.h"
#include "core/game_config.h"
#include "core/symbol_registry.h"
#include "fuzzer/differential_fuzzer.h"
#include "fuzzer/equivalence_runner.h"
#include "gtest/gtest.h"

namespace rom_nom_nom::fuzzer {
namespace {

constexpr uint32_t kRMipsHi16 = 5;
constexpr uint32_t kRMipsLo16 = 6;

struct TestSymbol {
  std::string name;
  uint32_t value = 0;
  uint32_t size = 0;
  uint8_t type = 1;    // STT_OBJECT (1) or STT_FUNC (2)
  uint16_t shndx = 1;  // Defined in .main by default.
};

struct TestReloc {
  uint32_t offset = 0;
  uint32_t sym = 0;
  uint32_t type = 0;
};

void Put16(std::vector<uint8_t>* out, uint16_t v) {
  out->push_back(static_cast<uint8_t>(v >> 8));
  out->push_back(static_cast<uint8_t>(v));
}

void Put32(std::vector<uint8_t>* out, uint32_t v) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    out->push_back(static_cast<uint8_t>(v >> shift));
  }
}

std::vector<uint8_t> BuildTestElf(uint32_t main_addr, const std::vector<uint32_t>& main_words,
                                  uint32_t bss_addr, uint32_t bss_size,
                                  const std::vector<TestSymbol>& symbols,
                                  const std::vector<TestReloc>& relocs) {
  std::vector<uint8_t> main_data;
  for (uint32_t w : main_words) Put32(&main_data, w);

  std::vector<uint8_t> strtab = {0};
  std::vector<uint8_t> symtab(16, 0);  // Null symbol.
  for (const auto& sym : symbols) {
    uint32_t name_off = static_cast<uint32_t>(strtab.size());
    strtab.insert(strtab.end(), sym.name.begin(), sym.name.end());
    strtab.push_back(0);
    Put32(&symtab, name_off);
    Put32(&symtab, sym.value);
    Put32(&symtab, sym.size);
    symtab.push_back(static_cast<uint8_t>(0x10 | sym.type));  // STB_GLOBAL
    symtab.push_back(0);
    Put16(&symtab, sym.shndx);
  }

  std::vector<uint8_t> rel;
  for (const auto& r : relocs) {
    Put32(&rel, r.offset);
    Put32(&rel, (r.sym << 8) | r.type);
  }

  std::vector<std::string> names = {"",        ".main",   ".main_bss", ".rel.main",
                                    ".symtab", ".strtab", ".shstrtab"};
  std::vector<uint8_t> shstrtab;
  std::vector<uint32_t> name_offsets;
  for (const auto& n : names) {
    name_offsets.push_back(static_cast<uint32_t>(shstrtab.size()));
    shstrtab.insert(shstrtab.end(), n.begin(), n.end());
    shstrtab.push_back(0);
  }

  std::vector<uint8_t> elf(52, 0);
  auto append = [&](const std::vector<uint8_t>& data) {
    while (elf.size() % 4 != 0) elf.push_back(0);
    uint32_t off = static_cast<uint32_t>(elf.size());
    elf.insert(elf.end(), data.begin(), data.end());
    return off;
  };
  uint32_t main_off = append(main_data);
  uint32_t rel_off = append(rel);
  uint32_t symtab_off = append(symtab);
  uint32_t strtab_off = append(strtab);
  uint32_t shstrtab_off = append(shstrtab);
  while (elf.size() % 4 != 0) elf.push_back(0);
  uint32_t shoff = static_cast<uint32_t>(elf.size());

  struct Header {
    uint32_t type, flags, addr, offset, size, link, info, entsize;
  };
  std::vector<Header> headers = {
      {0, 0, 0, 0, 0, 0, 0, 0},
      {1, 0x7, main_addr, main_off, static_cast<uint32_t>(main_data.size()), 0, 0, 0},
      {8, 0x3, bss_addr, main_off, bss_size, 0, 0, 0},
      {9, 0x40, 0, rel_off, static_cast<uint32_t>(rel.size()), 4, 1, 8},
      {2, 0, 0, symtab_off, static_cast<uint32_t>(symtab.size()), 5, 1, 16},
      {3, 0, 0, strtab_off, static_cast<uint32_t>(strtab.size()), 0, 0, 0},
      {3, 0, 0, shstrtab_off, static_cast<uint32_t>(shstrtab.size()), 0, 0, 0},
  };
  for (size_t i = 0; i < headers.size(); ++i) {
    const Header& h = headers[i];
    Put32(&elf, name_offsets[i]);
    Put32(&elf, h.type);
    Put32(&elf, h.flags);
    Put32(&elf, h.addr);
    Put32(&elf, h.offset);
    Put32(&elf, h.size);
    Put32(&elf, h.link);
    Put32(&elf, h.info);
    Put32(&elf, 4);
    Put32(&elf, h.entsize);
  }

  elf[0] = 0x7F;
  elf[1] = 'E';
  elf[2] = 'L';
  elf[3] = 'F';
  elf[4] = 1;  // ELFCLASS32
  elf[5] = 2;  // ELFDATA2MSB
  elf[6] = 1;
  WriteBigEndian16(elf.data() + 16, 2);  // ET_EXEC
  WriteBigEndian16(elf.data() + 18, 8);  // EM_MIPS
  WriteBigEndian32(elf.data() + 32, shoff);
  WriteBigEndian16(elf.data() + 40, 52);
  WriteBigEndian16(elf.data() + 46, 40);
  WriteBigEndian16(elf.data() + 48, static_cast<uint16_t>(headers.size()));
  WriteBigEndian16(elf.data() + 50, 6);
  return elf;
}

TEST(CandidateEnvironmentTest, PayloadReadRoutingAllSizesAndBounds) {
  MemoryPayload payload{
      .address = 0x80010000,
      .data = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88},
  };
  CandidateEnvironment env = CandidateEnvironment::FromComponents(
      /*target_payloads=*/{payload},
      /*candidate_payloads=*/{},
      /*target_bss_ranges=*/{});

  FuzzerOptions fopts;
  env.ConfigureFuzzerOptions(&fopts);
  ASSERT_TRUE(fopts.target_read_hook != nullptr);

  EXPECT_EQ(fopts.target_read_hook(0x80010000, 1, false), 0x11u);
  EXPECT_EQ(fopts.target_read_hook(0x80010002, 2, false), 0x3344u);
  EXPECT_EQ(fopts.target_read_hook(0x80010000, 4, false), 0x11223344u);
  EXPECT_EQ(fopts.target_read_hook(0x80010000, 8, true), 0x1122334455667788ULL);

  // Out-of-bounds or straddling end of payload returns nullopt
  EXPECT_EQ(fopts.target_read_hook(0x80010006, 4, false), std::nullopt);
  EXPECT_EQ(fopts.target_read_hook(0x80020000, 4, false), std::nullopt);
}

TEST(CandidateEnvironmentTest, BssZeroFillReadsInRebasedAndFallbackModes) {
  MemoryPayload payload{
      .address = 0x80010000,
      .data = {0xDE, 0xAD, 0xBE, 0xEF},
  };
  CandidateSymbolMap sym_map;
  sym_map.entries.push_back({
      .cand_addr = 0x80035000,
      .size = 0x100,
      .target_addr = 0x80030000,
      .is_data = true,
  });

  // Fallback (non-rebased) mode: candidate BSS at 0x80035010 translates to target BSS 0x80030010
  CandidateEnvironment env = CandidateEnvironment::FromComponents(
      /*target_payloads=*/{payload},
      /*candidate_payloads=*/{},
      /*target_bss_ranges=*/{{0x80030000, 0x80031000}}, sym_map,
      /*rebase=*/std::nullopt);

  FuzzerOptions fopts;
  env.ConfigureFuzzerOptions(&fopts);
  ASSERT_TRUE(fopts.target_read_hook != nullptr);
  ASSERT_TRUE(fopts.candidate_read_hook != nullptr);

  EXPECT_EQ(fopts.target_read_hook(0x80030010, 4, false), 0u);
  EXPECT_EQ(fopts.candidate_read_hook(0x80035010, 4, false), 0u);
  EXPECT_EQ(fopts.candidate_read_hook(0x80010000, 4, false), 0xDEADBEEFu);
}

TEST(CandidateEnvironmentTest, RebasedModeSharesReadHookAndTranslatesOnlyFunctionAddresses) {
  MemoryPayload payload{
      .address = 0x80020000,
      .data = {0xCA, 0xFE, 0xBA, 0xBE},
  };
  CandidateSymbolMap sym_map;
  // Shifted function symbol
  sym_map.entries.push_back({
      .cand_addr = 0x80001800,
      .size = 0x40,
      .target_addr = 0x80001200,
      .is_data = false,
  });
  // Data symbol (already rebased in code, so candidate_to_target_address should NOT remap data)
  sym_map.entries.push_back({
      .cand_addr = 0x80020400,
      .size = 0x20,
      .target_addr = 0x80020000,
      .is_data = true,
  });

  CandidateRebase rebase;
  rebase.relocations_patched = 2;

  CandidateEnvironment env = CandidateEnvironment::FromComponents(
      /*target_payloads=*/{payload},
      /*candidate_payloads=*/{payload},
      /*target_bss_ranges=*/{{0x80030000, 0x80030100}}, sym_map, rebase);

  FuzzerOptions fopts;
  env.ConfigureFuzzerOptions(&fopts);
  ASSERT_TRUE(fopts.candidate_to_target_address != nullptr);
  ASSERT_TRUE(fopts.candidate_read_hook != nullptr);

  // Function address is translated to target function address
  EXPECT_EQ(fopts.candidate_to_target_address(0x80001800), 0x80001200u);
  // Data address is left untouched in rebased mode
  EXPECT_EQ(fopts.candidate_to_target_address(0x80020400), 0x80020400u);
  // Shared read hook serves target payloads and BSS zeros to candidate
  EXPECT_EQ(fopts.candidate_read_hook(0x80020000, 4, false), 0xCAFEBABEu);
  EXPECT_EQ(fopts.candidate_read_hook(0x80030020, 4, false), 0u);
}

TEST(CandidateEnvironmentTest, FallbackModeRespectsCandidateRodataAndDataPayloads) {
  MemoryPayload target_payload{
      .address = 0x80020000,
      .data = {0xAA, 0xBB, 0xCC, 0xDD},
  };
  CandidateSymbolMap sym_map;
  sym_map.rodata_ranges.push_back({0x80015000, 0x80016000});
  sym_map.data_ranges.push_back({0x80025000, 0x80026000});
  sym_map.entries.push_back({
      .cand_addr = 0x80025000,
      .size = 0x100,
      .target_addr = 0x80020000,
      .is_data = true,
  });

  // 1. When has_candidate_payloads is false, reading candidate data translates to target payload.
  CandidateEnvironment env_no_cand_payloads = CandidateEnvironment::FromComponents(
      /*target_payloads=*/{target_payload},
      /*candidate_payloads=*/{},
      /*target_bss_ranges=*/{}, sym_map, /*rebase=*/std::nullopt,
      /*has_candidate_payloads=*/false);
  FuzzerOptions fopts1;
  env_no_cand_payloads.ConfigureFuzzerOptions(&fopts1);
  EXPECT_EQ(fopts1.candidate_read_hook(0x80015010, 4, false), std::nullopt);
  EXPECT_EQ(fopts1.candidate_read_hook(0x80025000, 4, false), 0xAABBCCDDu);

  // 2. When has_candidate_payloads is true, reading candidate data returns nullopt so the
  // candidate emulator reads its own pre-loaded data section.
  CandidateEnvironment env_with_cand_payloads = CandidateEnvironment::FromComponents(
      /*target_payloads=*/{target_payload},
      /*candidate_payloads=*/{target_payload},
      /*target_bss_ranges=*/{}, sym_map, /*rebase=*/std::nullopt,
      /*has_candidate_payloads=*/true);
  FuzzerOptions fopts2;
  env_with_cand_payloads.ConfigureFuzzerOptions(&fopts2);
  EXPECT_EQ(fopts2.candidate_read_hook(0x80025000, 4, false), std::nullopt);
}

TEST(CandidateEnvironmentTest, PlaceCandidateFunctionAppliesRebasePatchesAndOverlays) {
  CandidateRebase rebase;
  rebase.patched_words[0x80001000] = 0x3C048002;
  rebase.patched_words[0x80001004] = 0x8C820010;

  MemoryPayload target_payload{.address = 0x80020000, .data = {1, 2, 3, 4}};
  MemoryPayload cand_payload{.address = 0x80020000, .data = {1, 2, 3, 4}};

  CandidateEnvironment env =
      CandidateEnvironment::FromComponents({target_payload}, {cand_payload}, {}, {}, rebase);

  FunctionEquivalenceTarget target;
  target.name = "TestFunc";
  target.vram = 0x80001000;
  target.candidate_vram = 0x80001000;
  target.target_words = {0x3C048002, 0x8C820010, 0x03E00008, 0x00000000};
  target.candidate_words = {0x3C048003, 0x8C820030, 0x03E00008, 0x00000000};

  env.PlaceCandidateFunction(&target);
  EXPECT_EQ(target.candidate_vram, 0x80001000u);
  EXPECT_EQ(target.candidate_words[0], 0x3C048002u);
  EXPECT_EQ(target.candidate_words[1], 0x8C820010u);
  EXPECT_EQ(target.target_initial_memory, std::vector<MemoryPayload>{target_payload});
  EXPECT_EQ(target.candidate_initial_memory, std::vector<MemoryPayload>{cand_payload});
}

TEST(CandidateEnvironmentTest, BuildEndToEndFromElfAndConfig) {
  constexpr char kConfigText[] = R"(
    game_name: "test-game"
    sha1: "0123456789abcdef0123456789abcdef01234567"
    basename: "test-game"
    segments {
      name: "main"
      type: SEGMENT_CODE
      rom_start: 0x1000
      rom_end: 0x2040
      vram: 0x80001000
      subsegments { name: "code" type: SUBSEGMENT_ASM rom_start: 0x1000 vram: 0x80001000 }
      subsegments { name: "data" type: SUBSEGMENT_DATA rom_start: 0x2000 vram: 0x80002000 }
      subsegments { name: "bss" type: SUBSEGMENT_BSS vram: 0x80003000 }
      bss_size: 0x100
    }
  )";
  constexpr char kSymbolsText[] = R"(
    entries { name: "Func" address: 0x80001000 type: SYMBOL_FUNC }
    entries { name: "g_table" address: 0x80002000 type: SYMBOL_DATA }
    entries { name: "g_bss" address: 0x80003000 type: SYMBOL_DATA }
  )";

  auto cfg_or = ParseGameConfig(kConfigText);
  ASSERT_TRUE(cfg_or.ok()) << cfg_or.status();
  auto syms_or = SymbolIndex::ParseFromTextproto(kSymbolsText);
  ASSERT_TRUE(syms_or.ok()) << syms_or.status();

  std::vector<uint8_t> base_rom(0x2040, 0);
  WriteBigEndian32(base_rom.data() + 0x2000, 0x12345678);

  // Candidate ELF has Func at 0x80001000 and g_table shifted to 0x80002020
  std::vector<uint32_t> main_words(0x1060 / 4, 0);
  main_words[0] = 0x3C048000;  // lui $a0, %hi(g_table)
  main_words[1] = 0x8C822020;  // lw  $v0, %lo(g_table)($a0)
  main_words[2] = 0x03E00008;  // jr  $ra
  main_words[3] = 0x00000000;  // nop

  std::vector<TestSymbol> elf_syms = {
      {"Func", 0x80001000, 16, 2, 1},
      {"g_table", 0x80002020, 0x40, 1, 1},
      {"g_bss", 0x80003100, 0x100, 1, 2},
  };
  std::vector<TestReloc> elf_relocs = {
      {0x80001000, 2, kRMipsHi16},
      {0x80001004, 2, kRMipsLo16},
  };
  std::vector<uint8_t> elf =
      BuildTestElf(0x80001000, main_words, 0x80003100, 0x100, elf_syms, elf_relocs);

  auto env_or =
      CandidateEnvironment::Build(base_rom, /*candidate_rom=*/{}, elf, &*cfg_or, &*syms_or);
  ASSERT_TRUE(env_or.ok()) << env_or.status();

  const CandidateEnvironment& env = *env_or;
  ASSERT_TRUE(env.Rebase().has_value());
  EXPECT_EQ(env.Rebase()->relocations_patched, 2u);
  EXPECT_TRUE(env.HooksEnabled());
  ASSERT_EQ(env.TargetBssRanges().size(), 1u);
  EXPECT_EQ(env.TargetBssRanges()[0].first, 0x80003000u);
  EXPECT_EQ(env.TargetBssRanges()[0].second, 0x80003100u);

  FuzzerOptions fopts;
  env.ConfigureFuzzerOptions(&fopts);
  ASSERT_TRUE(fopts.target_read_hook != nullptr);
  EXPECT_EQ(fopts.target_read_hook(0x80002000, 4, false), 0x12345678u);
  EXPECT_EQ(fopts.target_read_hook(0x80003004, 4, false), 0u);

  auto placed =
      env.PlaceCandidateFunction(0x80001000, {0x3C048000, 0x8C822020, 0x03E00008, 0x00000000});
  EXPECT_EQ(placed.words[1], 0x8C822000u);  // Rebased from 0x2020 to 0x2000!
}

}  // namespace
}  // namespace rom_nom_nom::fuzzer
