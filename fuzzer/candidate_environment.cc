#include "fuzzer/candidate_environment.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "core/endian.h"

namespace rom_nom_nom::fuzzer {

CandidateEnvironment::CandidateEnvironment()
    : target_payloads_(std::make_shared<std::vector<MemoryPayload>>()) {}

absl::StatusOr<CandidateEnvironment> CandidateEnvironment::Build(
    absl::Span<const uint8_t> base_rom, absl::Span<const uint8_t> candidate_rom,
    absl::Span<const uint8_t> candidate_elf, const GameConfig* config, const SymbolIndex* symbols,
    const FuzzerOptions& fuzzer_options, bool has_candidate_rom) {
  Options opts;
  opts.base_rom = base_rom;
  opts.candidate_rom = candidate_rom;
  opts.candidate_elf = candidate_elf;
  opts.config = config;
  opts.symbols = symbols;
  opts.pointer_buffer_vram = fuzzer_options.pointer_buffer_vram;
  opts.pointer_buffer_size = fuzzer_options.pointer_buffer_size;
  opts.initial_sp = fuzzer_options.initial_sp;
  opts.has_candidate_rom = has_candidate_rom || !candidate_rom.empty();
  return Build(opts);
}

absl::StatusOr<CandidateEnvironment> CandidateEnvironment::Build(const Options& options) {
  CandidateEnvironment env;
  env.reserved_ranges_ = {
      {options.pointer_buffer_vram, options.pointer_buffer_vram + options.pointer_buffer_size},
      {options.initial_sp - 0x10000, options.initial_sp + 0x1000},
  };
  env.has_candidate_payloads_ = options.has_candidate_rom || !options.candidate_rom.empty();

  auto extracted_target = ExtractTargetDataPayloads(options.base_rom, options.config);
  env.target_payloads_ =
      std::make_shared<const std::vector<MemoryPayload>>(std::move(extracted_target));

  if (options.config != nullptr) {
    for (const auto& seg : ResolveSegments(*options.config)) {
      for (const auto& sub : seg.subsegments) {
        if (sub.type == SUBSEGMENT_BSS && sub.vram_start != 0) {
          uint32_t bss_end = sub.vram_start + (sub.bss_size != 0 ? sub.bss_size : 0x100000);
          env.target_bss_ranges_.emplace_back(sub.vram_start, bss_end);
          if (env.target_bss_start_ == 0 || sub.vram_start < env.target_bss_start_) {
            env.target_bss_start_ = sub.vram_start;
          }
          if (bss_end > env.target_bss_end_) {
            env.target_bss_end_ = bss_end;
          }
        }
      }
    }
  }

  if (!options.candidate_elf.empty()) {
    if (options.enable_rebase) {
      RebaseOptions rebase_opts;
      rebase_opts.reserved_ranges = env.reserved_ranges_;
      // Keep relocated compiler rodata far away from game data: fuzz vectors routinely index
      // arrays out of bounds, and the target reads zeros wherever the candidate keeps extra data.
      rebase_opts.scratch_base = options.pointer_buffer_vram + 0x100000;
      // Functions whose candidate code overlaps the target's data addresses run from here.
      rebase_opts.code_scratch_base = options.pointer_buffer_vram + 0x200000;
      auto rebase_or = BuildCandidateRebase(options.candidate_elf,
                                            CollectDataBlobs(options.config, options.symbols),
                                            options.config, options.symbols, rebase_opts);
      if (!rebase_or.ok()) {
        return rebase_or.status();
      }
      if (rebase_or->layout_mismatches > 0) {
        return absl::FailedPreconditionError(
            absl::StrFormat("Candidate relocation rebase detected %zu layout mismatch(es).",
                            rebase_or->layout_mismatches));
      }
      env.rebase_ = std::move(*rebase_or);
    }

    env.symbol_map_ = BuildCandidateSymbolMap(options.candidate_elf, options.symbols);
    if (!env.symbol_map_.entries.empty()) {
      env.hooks_enabled_ = true;
    } else {
      // Without a symbol map the rebased hooks are not installed; keep words and memory unrebased.
      env.rebase_.reset();
    }
  }

  if (env.rebase_.has_value()) {
    env.candidate_payloads_ = *env.target_payloads_;
    env.candidate_payloads_.insert(env.candidate_payloads_.end(),
                                   env.rebase_->private_payloads.begin(),
                                   env.rebase_->private_payloads.end());
  } else if (!options.candidate_rom.empty()) {
    env.candidate_payloads_ =
        ExtractCandidateDataPayloads(options.candidate_rom, options.candidate_elf, options.config);
  }

  return env;
}

CandidateEnvironment CandidateEnvironment::FromComponents(
    std::vector<MemoryPayload> target_payloads, std::vector<MemoryPayload> candidate_payloads,
    std::vector<std::pair<uint32_t, uint32_t>> target_bss_ranges, CandidateSymbolMap symbol_map,
    std::optional<CandidateRebase> rebase, bool has_candidate_payloads) {
  CandidateEnvironment env;
  env.target_payloads_ =
      std::make_shared<const std::vector<MemoryPayload>>(std::move(target_payloads));
  env.candidate_payloads_ = std::move(candidate_payloads);
  env.target_bss_ranges_ = std::move(target_bss_ranges);
  for (const auto& [lo, hi] : env.target_bss_ranges_) {
    if (env.target_bss_start_ == 0 || lo < env.target_bss_start_) {
      env.target_bss_start_ = lo;
    }
    if (hi > env.target_bss_end_) {
      env.target_bss_end_ = hi;
    }
  }
  env.symbol_map_ = std::move(symbol_map);
  env.rebase_ = std::move(rebase);
  env.has_candidate_payloads_ = has_candidate_payloads || !env.candidate_payloads_.empty();
  env.hooks_enabled_ = true;
  return env;
}

void CandidateEnvironment::ConfigureFuzzerOptions(FuzzerOptions* fopts) const {
  if (fopts == nullptr || !hooks_enabled_) {
    return;
  }

  auto read_payload = [payloads = target_payloads_](uint32_t addr,
                                                    size_t size) -> std::optional<uint64_t> {
    if (payloads == nullptr) return std::nullopt;
    for (const auto& p : *payloads) {
      if (addr >= p.address && addr + size <= p.address + p.data.size()) {
        size_t offset = addr - p.address;
        if (size == 1) return p.data[offset];
        if (size == 2) return ReadBigEndian16(p.data.data() + offset);
        if (size == 4) return ReadBigEndian32(p.data.data() + offset);
        if (size == 8) return ReadBigEndian64(p.data.data() + offset);
      }
    }
    return std::nullopt;
  };

  uint32_t target_bss_start = target_bss_start_;
  uint32_t target_bss_end = target_bss_end_;
  auto shared_read_hook = [read_payload, target_bss_start, target_bss_end](
                              uint32_t vram, size_t size,
                              bool /*is_fp*/) -> std::optional<uint64_t> {
    if (target_bss_start != 0 && vram >= target_bss_start && vram < target_bss_end) {
      return 0;
    }
    return read_payload(vram, size);
  };
  fopts->target_read_hook = shared_read_hook;

  auto map_ptr = std::make_shared<const CandidateSymbolMap>(symbol_map_);
  if (rebase_.has_value()) {
    // Data addresses are already identical; only code addresses (e.g. returned or stored
    // function pointers) still differ between the two builds.
    auto function_map = std::make_shared<absl::flat_hash_map<uint32_t, uint32_t>>();
    for (const auto& e : map_ptr->entries) {
      if (!e.is_data && e.cand_addr != e.target_addr) {
        function_map->emplace(e.cand_addr, e.target_addr);
      }
    }
    fopts->candidate_to_target_address = [function_map](uint32_t addr) {
      auto it = function_map->find(addr);
      return it != function_map->end() ? it->second : addr;
    };
    fopts->candidate_read_hook = shared_read_hook;
  } else {
    fopts->candidate_to_target_address = [map = map_ptr](uint32_t addr) {
      return map->Translate(addr);
    };
    fopts->candidate_read_hook = [map = map_ptr, read_payload, target_bss_start, target_bss_end,
                                  has_candidate_payloads = has_candidate_payloads_](
                                     uint32_t cand_vram, size_t size,
                                     bool is_fp) -> std::optional<uint64_t> {
      // 1. If the address is within candidate's own rodata section, candidate emulator
      // already holds the compiled rodata (e.g. jump tables) directly.
      if (map != nullptr && map->IsCandidateRodata(cand_vram, is_fp)) {
        return std::nullopt;
      }

      // 2. Candidate address that translates to target rodata / data
      uint32_t target_vram = (map != nullptr) ? map->Translate(cand_vram) : cand_vram;
      if (target_bss_start != 0 && target_vram >= target_bss_start &&
          target_vram < target_bss_end) {
        return 0;
      }

      // If candidate data payloads were loaded from candidate ROM, candidate emulator
      // already holds candidate's own data directly.
      if (has_candidate_payloads && map != nullptr && map->IsCandidateData(cand_vram)) {
        return std::nullopt;
      }
      if (target_vram != cand_vram) {
        auto res = read_payload(target_vram, size);
        if (res.has_value()) return *res;
      }

      // 3. Direct target address dereference (via embedded retail pointer in data table)
      auto target_res = read_payload(cand_vram, size);
      if (target_res.has_value()) {
        return *target_res;
      }

      return std::nullopt;
    };
  }
}

CandidateRebase::PlacedFunction CandidateEnvironment::PlaceCandidateFunction(
    uint32_t cand_vram, absl::Span<const uint32_t> cand_words) const {
  if (rebase_.has_value() && !cand_words.empty()) {
    return rebase_->PlaceFunction(cand_vram, cand_words);
  }
  return CandidateRebase::PlacedFunction{
      .vram = cand_vram,
      .words = std::vector<uint32_t>(cand_words.begin(), cand_words.end()),
      .overlays = {},
  };
}

void CandidateEnvironment::PlaceCandidateFunction(FunctionEquivalenceTarget* target) const {
  if (target == nullptr) return;
  if (target->target_initial_memory.empty() && target_payloads_ != nullptr) {
    target->target_initial_memory = *target_payloads_;
  }
  if (target->candidate_initial_memory.empty()) {
    target->candidate_initial_memory = candidate_payloads_;
  }
  if (rebase_.has_value() && !target->candidate_words.empty()) {
    auto placed = rebase_->PlaceFunction(target->candidate_vram, target->candidate_words);
    target->candidate_vram = placed.vram;
    target->candidate_words = std::move(placed.words);
    target->candidate_initial_memory.insert(target->candidate_initial_memory.end(),
                                            placed.overlays.begin(), placed.overlays.end());
  }
}

}  // namespace rom_nom_nom::fuzzer
