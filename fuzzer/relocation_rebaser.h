#ifndef FUZZER_RELOCATION_REBASER_H_
#define FUZZER_RELOCATION_REBASER_H_

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "core/game_config.h"
#include "core/symbol_registry.h"
#include "fuzzer/differential_fuzzer.h"

namespace rom_nom_nom::fuzzer {

// A contiguous data, rodata, or BSS subsegment that the split build emits as a single opaque
// blob with one base label. Its bytes (and every offset inside it) are identical between the
// target ROM and any rebuilt candidate; only its base address may differ.
struct DataBlob {
  uint32_t target_start = 0;
  uint32_t size = 0;
  std::string label;
  bool is_bss = false;
};

// Collects every data/rodata/BSS blob of the code segments in `config`. Blob base labels are
// resolved exactly like the lifter's linker-script generator: the registered symbol at the
// blob's target address, or `D_XXXXXXXX` when none is registered.
std::vector<DataBlob> CollectDataBlobs(const GameConfig* config, const SymbolIndex* symbols);

struct RebaseOptions {
  // Address ranges [first, second) that relocated candidate-private data must never overlap
  // (e.g. the fuzzer's pointer buffers and stack window).
  std::vector<std::pair<uint32_t, uint32_t>> reserved_ranges;
  // Highest usable KSEG0 address (exclusive) for relocated candidate-private data.
  uint32_t ram_end = 0x80800000;
  // Where colliding candidate-private data is relocated. 0 places it directly above every
  // loaded range; callers can instead choose an address far from game data so that wild
  // out-of-bounds accesses in the target (which read zeros there) do not hit it.
  uint32_t scratch_base = 0;
  // Where functions whose code collides with rebased data are executed (0 = never move code).
  uint32_t code_scratch_base = 0;
};

// Result of rebasing a fully linked candidate ELF (linked with --emit-relocs) onto the target's
// data layout. After rebasing, every candidate reference into a data blob points at the blob's
// target address, so both emulators share one memory layout for data and BSS and memory
// effects can be compared byte-for-byte without heuristic address translation.
struct CandidateRebase {
  struct MovedRange {
    uint32_t cand_start = 0;
    uint32_t size = 0;
    int64_t shift = 0;
  };

  // Candidate instruction/data words rewritten by relocation, keyed by candidate vaddr.
  absl::flat_hash_map<uint32_t, uint32_t> patched_words;
  // Candidate-private data (compiler rodata, private data/BSS), placed at final addresses that
  // never collide with target blobs, with relocations inside it already applied.
  std::vector<MemoryPayload> private_payloads;
  // Every candidate range whose address changed (blobs and relocated private data).
  std::vector<MovedRange> moved_ranges;

  size_t relocations_patched = 0;
  // Relocations that reference a moved range but cannot be rewritten (unsupported types).
  size_t unsupported_relocations = 0;
  // Registered data symbols whose candidate offset inside their blob differs from the target.
  size_t layout_mismatches = 0;
  std::vector<std::string> warnings;

  // Candidate vaddrs of every R_MIPS_26 (j/jal) relocation site.
  absl::flat_hash_set<uint32_t> jump26_sites;
  // Absolute words (R_MIPS_32) inside private payloads: (final address, final value).
  std::vector<std::pair<uint32_t, uint32_t>> private_abs32;
  // Ranges [first, second) the candidate emulator fills with data after rebasing.
  std::vector<std::pair<uint32_t, uint32_t>> occupied_ranges;
  // Where functions whose code overlaps `occupied_ranges` are executed instead.
  uint32_t code_scratch_base = 0;

  // A function ready to execute in the candidate emulator.
  struct PlacedFunction {
    uint32_t vram = 0;
    std::vector<uint32_t> words;
    // Small memory overlays (e.g. relocated jump-table entries) to load after the payloads.
    std::vector<MemoryPayload> overlays;
  };

  // Returns the function's instruction words with relocation patches applied.
  std::vector<uint32_t> RebaseFunctionWords(uint32_t vram, absl::Span<const uint32_t> words) const;

  // Rebases the function and, when its code would overlap rebased data (the candidate's code
  // grew past the target's data addresses), moves it to `code_scratch_base`, retargeting its
  // internal jumps and the jump-table entries that point into it.
  PlacedFunction PlaceFunction(uint32_t vram, absl::Span<const uint32_t> words) const;

  // Maps a pre-rebase candidate address to its post-rebase address (identity if not moved).
  uint32_t MapAddress(uint32_t cand_addr) const;
};

// Builds a CandidateRebase from a fully linked big-endian MIPS ELF32 that retains relocations
// (`ld --emit-relocs`). Fails if the ELF carries no relocations or no blob label is found.
absl::StatusOr<CandidateRebase> BuildCandidateRebase(absl::Span<const uint8_t> candidate_elf,
                                                     absl::Span<const DataBlob> blobs,
                                                     const GameConfig* config,
                                                     const SymbolIndex* symbols,
                                                     const RebaseOptions& options);

}  // namespace rom_nom_nom::fuzzer

#endif  // FUZZER_RELOCATION_REBASER_H_
