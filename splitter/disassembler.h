#ifndef SPLITTER_DISASSEMBLER_H_
#define SPLITTER_DISASSEMBLER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "splitter/relocations.h"
#include "splitter/symbol_registry.h"

namespace rom_nom_nom {

// A single disassembled instruction line.
struct DisassembledInstruction {
  uint32_t pc = 0;
  uint32_t raw_word = 0;
  Instruction instruction;
  std::string formatted_asm;  // e.g. "addiu $sp, $sp, -32"
  std::string line_comment;   // e.g. "/* 80025C00 27BDFFE0 */"
};

// A disassembled function with metadata and complete emitted assembly.
struct DisassembledFunction {
  std::string name;
  uint32_t vram_start = 0;
  uint32_t vram_end = 0;
  size_t Size() const { return vram_end - vram_start; }
  std::vector<DisassembledInstruction> instructions;
  std::string emitted_assembly;  // Complete formatted .s text with directives
};

// Formatting options for assembly output.
struct DisassemblerOptions {
  bool emit_line_comments = true;     // Emits /* VRAM RAW_HEX */ comments
  bool emit_function_framing = true;  // Emits .globl, .ent, .end, .set noat/noreorder
  bool emit_relocations = false;      // Emits %hi(...) and %lo(...) via RelocationTracker
};

class Disassembler {
 public:
  explicit Disassembler(const SymbolIndex* symbols = nullptr, DisassemblerOptions options = {});

  // Optionally attaches a pre-computed relocation tracker.
  void SetRelocationTracker(const RelocationTracker* tracker) { reloc_tracker_ = tracker; }

  // Registers an address as a known function entrypoint.
  void RegisterFunctionEntrypoint(uint32_t vram_address) {
    known_entrypoints_.insert(vram_address);
  }

  // Pre-scans a code buffer for jal instructions to discover function entrypoints,
  // and branches/jumps to discover segment-wide labels.
  void PreScanCode(absl::Span<const uint8_t> code, uint32_t vram_start);

  // Disassembles a single 32-bit instruction word at given PC.
  std::string DisassembleInstruction(uint32_t raw_word, uint32_t pc) const;

  // Disassembles a single function starting at vram_start up to jr $ra + delay slot.
  absl::StatusOr<DisassembledFunction> DisassembleFunction(absl::Span<const uint8_t> code,
                                                           uint32_t vram_start) const;

  // Disassembles an entire code buffer, slicing it into individual functions.
  absl::StatusOr<std::vector<DisassembledFunction>> DisassembleAllFunctions(
      absl::Span<const uint8_t> code, uint32_t vram_start) const;

  // Disassembles a contiguous code buffer into a single assembly string.
  absl::StatusOr<std::string> DisassembleRange(absl::Span<const uint8_t> code,
                                               uint32_t vram_start) const;

 private:
  const SymbolIndex* symbols_ = nullptr;
  DisassemblerOptions options_;
  const RelocationTracker* reloc_tracker_ = nullptr;
  absl::flat_hash_set<uint32_t> known_entrypoints_;
  absl::flat_hash_set<uint32_t> segment_labels_;
};

}  // namespace rom_nom_nom

#endif  // SPLITTER_DISASSEMBLER_H_
