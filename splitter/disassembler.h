#ifndef SPLITTER_DISASSEMBLER_H_
#define SPLITTER_DISASSEMBLER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "core/mips.h"
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
};

class Disassembler {
 public:
  explicit Disassembler(const SymbolIndex* symbols = nullptr, DisassemblerOptions options = {});

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
};

}  // namespace rom_nom_nom

#endif  // SPLITTER_DISASSEMBLER_H_
