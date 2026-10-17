#ifndef FUZZER_CANDIDATE_ENVIRONMENT_H_
#define FUZZER_CANDIDATE_ENVIRONMENT_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "core/game_config.h"
#include "core/symbol_registry.h"
#include "fuzzer/candidate_symbol_map.h"
#include "fuzzer/differential_fuzzer.h"
#include "fuzzer/equivalence_runner.h"
#include "fuzzer/relocation_rebaser.h"

namespace rom_nom_nom::fuzzer {

using CandidateRebaseResult = CandidateRebase;
using FuzzerOptions = DifferentialFuzzer::Options;

// Encapsulates the candidate/target memory environment, relocation rebasing state, symbol map,
// data payloads, BSS bounds, and emulator read/address-translation hooks for differential fuzzing.
class CandidateEnvironment {
 public:
  struct Options {
    absl::Span<const uint8_t> base_rom;
    absl::Span<const uint8_t> candidate_rom;
    absl::Span<const uint8_t> candidate_elf;
    const GameConfig* config = nullptr;
    const SymbolIndex* symbols = nullptr;
    uint32_t pointer_buffer_vram = 0x80500000;
    uint32_t pointer_buffer_size = 0x100000;
    uint32_t initial_sp = 0x807FE000;
    bool has_candidate_rom = false;
    bool enable_rebase = true;
  };

  CandidateEnvironment();

  // Builds a CandidateEnvironment from ROM/ELF binaries, game config, and symbol registry.
  static absl::StatusOr<CandidateEnvironment> Build(const Options& options);

  static absl::StatusOr<CandidateEnvironment> Build(
      absl::Span<const uint8_t> base_rom, absl::Span<const uint8_t> candidate_rom,
      absl::Span<const uint8_t> candidate_elf, const GameConfig* config, const SymbolIndex* symbols,
      const FuzzerOptions& fuzzer_options = {}, bool has_candidate_rom = false);

  static absl::StatusOr<CandidateEnvironment> Initialize(const Options& options) {
    return Build(options);
  }

  // Constructs a CandidateEnvironment directly from pre-built components (useful for unit tests).
  static CandidateEnvironment FromComponents(
      std::vector<MemoryPayload> target_payloads, std::vector<MemoryPayload> candidate_payloads,
      std::vector<std::pair<uint32_t, uint32_t>> target_bss_ranges,
      CandidateSymbolMap symbol_map = {}, std::optional<CandidateRebase> rebase = std::nullopt,
      bool has_candidate_payloads = false);

  // Attaches `target_read_hook`, `candidate_read_hook`, and `candidate_to_target_address` to
  // `fopts` when candidate symbol/rebase hooks are active.
  void ConfigureFuzzerOptions(FuzzerOptions* fopts) const;

  // Rebases and places a candidate function (and attaches its initial memory and overlays).
  void PlaceCandidateFunction(FunctionEquivalenceTarget* target) const;
  CandidateRebase::PlacedFunction PlaceCandidateFunction(
      uint32_t cand_vram, absl::Span<const uint32_t> cand_words) const;

  const std::optional<CandidateRebase>& Rebase() const { return rebase_; }
  const CandidateSymbolMap& SymbolMap() const { return symbol_map_; }
  const std::vector<MemoryPayload>& TargetPayloads() const { return *target_payloads_; }
  const std::vector<MemoryPayload>& CandidatePayloads() const { return candidate_payloads_; }
  const std::vector<std::pair<uint32_t, uint32_t>>& TargetBssRanges() const {
    return target_bss_ranges_;
  }
  uint32_t TargetBssStart() const { return target_bss_start_; }
  uint32_t TargetBssEnd() const { return target_bss_end_; }
  const std::vector<std::pair<uint32_t, uint32_t>>& ReservedRanges() const {
    return reserved_ranges_;
  }
  bool HooksEnabled() const { return hooks_enabled_; }

 private:
  std::optional<CandidateRebase> rebase_;
  CandidateSymbolMap symbol_map_;
  std::shared_ptr<const std::vector<MemoryPayload>> target_payloads_;
  std::vector<MemoryPayload> candidate_payloads_;
  std::vector<std::pair<uint32_t, uint32_t>> target_bss_ranges_;
  uint32_t target_bss_start_ = 0;
  uint32_t target_bss_end_ = 0;
  std::vector<std::pair<uint32_t, uint32_t>> reserved_ranges_;
  bool has_candidate_payloads_ = false;
  bool hooks_enabled_ = false;
};

}  // namespace rom_nom_nom::fuzzer

#endif  // FUZZER_CANDIDATE_ENVIRONMENT_H_
