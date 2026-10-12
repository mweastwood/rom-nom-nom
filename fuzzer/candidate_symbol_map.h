#ifndef FUZZER_CANDIDATE_SYMBOL_MAP_H_
#define FUZZER_CANDIDATE_SYMBOL_MAP_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/types/span.h"
#include "fuzzer/differential_fuzzer.h"
#include "splitter/config.h"
#include "splitter/symbol_registry.h"

namespace rom_nom_nom::fuzzer {

// Tracks candidate ELF memory ranges and translates candidate addresses back to target addresses.
struct CandidateSymbolMap {
  struct Entry {
    uint32_t cand_addr = 0;
    uint32_t size = 0;
    uint32_t target_addr = 0;
    bool is_data = false;
  };

  struct SectionRange {
    uint32_t cand_start = 0;
    uint32_t cand_end = 0;
    int64_t shift = 0;
  };

  std::vector<Entry> entries;
  std::vector<SectionRange> section_ranges;
  std::vector<std::pair<uint32_t, uint32_t>> rodata_ranges;
  std::vector<std::pair<uint32_t, uint32_t>> data_ranges;

  bool IsCandidateData(uint32_t addr) const;
  bool IsDataSymbol(uint32_t cand_addr) const;
  bool IsCandidateRodata(uint32_t addr, bool is_fp) const;
  uint32_t Translate(uint32_t cand_addr) const;
};

// Extracts initial memory payloads for DATA and RODATA subsegments from the base retail ROM.
std::vector<MemoryPayload> ExtractTargetDataPayloads(absl::Span<const uint8_t> base_rom,
                                                     const SplitConfig* config);

// Extracts initial memory payloads for candidate DATA and RODATA from candidate ELF and ROM.
std::vector<MemoryPayload> ExtractCandidateDataPayloads(absl::Span<const uint8_t> candidate_rom,
                                                        absl::Span<const uint8_t> candidate_elf,
                                                        const SplitConfig* config);

// Constructs a CandidateSymbolMap by analyzing symbol table and section headers of candidate ELF.
CandidateSymbolMap BuildCandidateSymbolMap(absl::Span<const uint8_t> candidate_elf,
                                           const SymbolIndex* symbol_index);

}  // namespace rom_nom_nom::fuzzer

#endif  // FUZZER_CANDIDATE_SYMBOL_MAP_H_
