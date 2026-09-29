#ifndef SPLITTER_RELOCATIONS_H_
#define SPLITTER_RELOCATIONS_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "core/mips.h"
#include "splitter/symbol_registry.h"

namespace rom_nom_nom {

// MIPS relocation type for 16-bit address halves.
enum class RelocType {
  kHi16,  // %hi(symbol)
  kLo16,  // %lo(symbol)
};

// Represents a relocation reference attached to an instruction.
struct InstructionRelocation {
  uint32_t pc = 0;  // Address of the instruction
  RelocType type = RelocType::kHi16;
  uint32_t target_vram = 0;  // Reconstructed 32-bit address
  std::string symbol_name;   // Resolved symbol name or synthesized label
  int32_t addend = 0;        // Offset relative to symbol base address
};

// Options for relocation analysis.
struct RelocationOptions {
  // If true, any 32-bit address in standard N64 KSEG0/KSEG1 (0x80000000 - 0xBFFFFFFF)
  // will be paired, even if not explicitly present in the SymbolIndex.
  bool allow_unregistered_pointers = true;
};

// Tracks MIPS register dataflow to pair %hi(lui) and %lo(consumer) instructions.
class RelocationTracker {
 public:
  explicit RelocationTracker(const SymbolIndex* symbols = nullptr, RelocationOptions options = {});

  // Analyzes a sequence of decoded instructions and discovers %hi/%lo pairs.
  absl::Status Analyze(absl::Span<const Instruction> instructions);

  // Convenience overload: decodes raw code bytes starting at vram_start and analyzes.
  absl::Status AnalyzeCode(absl::Span<const uint8_t> code, uint32_t vram_start);

  // Looks up a relocation for a given instruction PC. Returns nullptr if none found.
  const InstructionRelocation* FindRelocation(uint32_t pc) const;

  // Formats an instruction using %hi(...) or %lo(...) if a relocation exists at pc.
  // If no relocation exists, returns standard inst.Disassemble().
  std::string FormatInstruction(const Instruction& inst) const;

  // Formats the symbol expression: "symbol", "symbol + 0x4", or "symbol - 0x4".
  static std::string FormatSymbolExpression(std::string_view symbol_name, int32_t addend);

  // Returns all discovered relocations.
  const std::vector<InstructionRelocation>& Relocations() const { return relocations_; }

  // Returns number of discovered relocations.
  size_t Size() const { return relocations_.size(); }

  // Clears all discovered relocations.
  void Clear();

 private:
  // Resolves target_vram to symbol_name and addend.
  void ResolveSymbol(uint32_t target_vram, std::string& out_name, int32_t& out_addend) const;

  // Checks if an address looks like a valid pointer target.
  bool IsValidTarget(uint32_t addr) const;

  const SymbolIndex* symbols_ = nullptr;
  RelocationOptions options_;
  std::vector<InstructionRelocation> relocations_;
  absl::flat_hash_map<uint32_t, size_t> pc_to_index_;
};

}  // namespace rom_nom_nom

#endif  // SPLITTER_RELOCATIONS_H_
