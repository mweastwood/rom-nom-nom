#include "splitter/disassembler.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "core/endian.h"
#include "core/mips.h"
#include "splitter/relocations.h"
#include "splitter/symbol_registry.h"

namespace rom_nom_nom {

Disassembler::Disassembler(const SymbolIndex* symbols, DisassemblerOptions options)
    : symbols_(symbols), options_(std::move(options)) {}

std::string Disassembler::DisassembleInstruction(uint32_t raw_word, uint32_t pc) const {
  auto inst_or = DecodeInstruction(raw_word, pc);
  if (!inst_or.ok()) {
    return absl::StrFormat(".word  0x%08X", raw_word);
  }
  const Instruction& inst = *inst_or;
  if (inst.opcode == Opcode::kJal) {
    uint32_t target = inst.JumpTarget();
    std::string name = symbols_ ? symbols_->LookupOrSynthesizeName(target, SYMBOL_FUNC)
                                : absl::StrFormat("func_%08X", target);
    return absl::StrFormat("jal    %s", name);
  }
  if (reloc_tracker_ != nullptr && reloc_tracker_->FindRelocation(pc) != nullptr) {
    return reloc_tracker_->FormatInstruction(inst);
  }
  return inst.Disassemble();
}

void Disassembler::PreScanCode(absl::Span<const uint8_t> code, uint32_t vram_start) {
  for (size_t offset = 0; offset + 4 <= code.size(); offset += 4) {
    uint32_t pc = vram_start + static_cast<uint32_t>(offset);
    uint32_t raw_word = ReadBigEndian32(code.data() + offset);
    auto inst_or = DecodeInstruction(raw_word, pc);
    if (!inst_or.ok()) {
      continue;
    }
    const Instruction& inst = *inst_or;
    if (inst.opcode == Opcode::kJal) {
      uint32_t target = inst.JumpTarget();
      if ((target >= 0x80000000 && target < 0x80800000) ||
          (target >= 0xA0000000 && target < 0xA0800000)) {
        known_entrypoints_.insert(target);
      }
    } else if (inst.IsBranch()) {
      uint32_t target = inst.BranchTarget();
      segment_labels_.insert(target);
    } else if (inst.opcode == Opcode::kJ) {
      uint32_t target = inst.JumpTarget();
      if (target >= vram_start && target < vram_start + code.size()) {
        segment_labels_.insert(target);
      }
    }
  }
}

absl::StatusOr<DisassembledFunction> Disassembler::DisassembleFunction(
    absl::Span<const uint8_t> code, uint32_t vram_start) const {
  if (code.size() < 4) {
    return absl::InvalidArgumentError("Code buffer too small (< 4 bytes) for function");
  }

  // Pass 1: Determine function boundary
  size_t func_end_offset = code.size();
  uint32_t max_forward_target = 0;

  for (size_t offset = 0; offset + 4 <= code.size(); offset += 4) {
    uint32_t pc = vram_start + static_cast<uint32_t>(offset);

    // If next function entry is registered, end current function here
    if (offset > 0) {
      bool is_entrypoint = known_entrypoints_.contains(pc);
      if (!is_entrypoint && symbols_ && symbols_->HasAddress(pc)) {
        const auto* entry = symbols_->FindByAddress(pc);
        if (entry != nullptr && entry->type() == SYMBOL_FUNC) {
          is_entrypoint = true;
        }
      }
      if (is_entrypoint) {
        func_end_offset = offset;
        break;
      }
    }

    uint32_t raw_word = ReadBigEndian32(code.data() + offset);
    auto inst_or = DecodeInstruction(raw_word, pc);
    if (!inst_or.ok()) {
      continue;
    }
    const Instruction& inst = *inst_or;

    // Track forward branch targets to avoid cutting functions on early returns
    if (inst.IsBranch()) {
      uint32_t target = inst.BranchTarget();
      if (target > pc) {
        max_forward_target = std::max(max_forward_target, target);
      }
    } else if (inst.opcode == Opcode::kJ) {
      uint32_t target = inst.JumpTarget();
      if (target > pc && target < vram_start + code.size()) {
        max_forward_target = std::max(max_forward_target, target);
      }
    }

    if (inst.IsReturn()) {
      // jr $ra return: include delay slot instruction if no forward branch jumps past it
      if (max_forward_target <= pc + 4) {
        func_end_offset = std::min<size_t>(offset + 8, code.size());

        // Include trailing alignment NOPs up to next symbol or non-zero instruction
        size_t next_offset = func_end_offset;
        while (next_offset + 4 <= code.size()) {
          uint32_t next_pc = vram_start + static_cast<uint32_t>(next_offset);
          if (known_entrypoints_.contains(next_pc) || (symbols_ && symbols_->HasAddress(next_pc))) {
            break;
          }
          if (ReadBigEndian32(code.data() + next_offset) != 0) {
            break;
          }
          func_end_offset = next_offset + 4;
          next_offset += 4;
        }
        break;
      }
    }
  }

  uint32_t vram_end = vram_start + static_cast<uint32_t>(func_end_offset);

  // Relocation analysis (if enabled and no external tracker was provided)
  const RelocationTracker* active_tracker = reloc_tracker_;
  std::unique_ptr<RelocationTracker> local_tracker;
  if (active_tracker == nullptr && options_.emit_relocations) {
    local_tracker = std::make_unique<RelocationTracker>(symbols_);
    (void)local_tracker->AnalyzeCode(code.subspan(0, func_end_offset), vram_start);
    active_tracker = local_tracker.get();
  }

  // Pass 2: Collect internal branch targets within function
  absl::flat_hash_set<uint32_t> local_labels;
  for (size_t offset = 0; offset + 4 <= func_end_offset; offset += 4) {
    uint32_t pc = vram_start + static_cast<uint32_t>(offset);
    uint32_t raw_word = ReadBigEndian32(code.data() + offset);
    auto inst_or = DecodeInstruction(raw_word, pc);
    if (!inst_or.ok()) {
      continue;
    }
    const Instruction& inst = *inst_or;

    if (inst.IsBranch()) {
      uint32_t target = inst.BranchTarget();
      if (target >= vram_start && target < vram_end) {
        local_labels.insert(target);
      }
    } else if (inst.opcode == Opcode::kJ) {
      uint32_t target = inst.JumpTarget();
      if (target >= vram_start && target < vram_end) {
        local_labels.insert(target);
      }
    }
  }

  // Pass 3: Disassemble and format instructions
  DisassembledFunction func;
  func.name = symbols_ ? symbols_->LookupOrSynthesizeName(vram_start, SYMBOL_FUNC)
                       : absl::StrFormat("func_%08X", vram_start);
  func.vram_start = vram_start;
  func.vram_end = vram_end;

  std::string asm_text;
  if (options_.emit_function_framing) {
    absl::StrAppend(&asm_text, ".set noat\n.set noreorder\n\n");
    absl::StrAppend(&asm_text,
                    absl::StrFormat(".globl %s\n.ent %s\n%s:\n", func.name, func.name, func.name));
  }

  bool prev_has_delay_slot = false;
  for (size_t offset = 0; offset + 4 <= func_end_offset; offset += 4) {
    uint32_t pc = vram_start + static_cast<uint32_t>(offset);
    uint32_t raw_word = ReadBigEndian32(code.data() + offset);
    auto inst_or = DecodeInstruction(raw_word, pc);

    DisassembledInstruction d_inst;
    d_inst.pc = pc;
    d_inst.raw_word = raw_word;
    d_inst.line_comment = absl::StrFormat("/* %08X %08X */", pc, raw_word);

    if (local_labels.contains(pc) || segment_labels_.contains(pc)) {
      absl::StrAppend(&asm_text, absl::StrFormat(".L%08X:\n", pc));
    }

    std::string formatted;
    if (!inst_or.ok()) {
      formatted = absl::StrFormat(".word  0x%08X", raw_word);
      d_inst.formatted_asm = formatted;
      func.instructions.push_back(d_inst);
      absl::StrAppend(&asm_text, d_inst.line_comment, "  ", formatted, "\n");
      prev_has_delay_slot = false;
      continue;
    }

    const Instruction& inst = *inst_or;
    d_inst.instruction = inst;

    if (inst.opcode == Opcode::kJal) {
      uint32_t target = inst.JumpTarget();
      std::string target_name = symbols_ ? symbols_->LookupOrSynthesizeName(target, SYMBOL_FUNC)
                                         : absl::StrFormat("func_%08X", target);
      formatted = absl::StrFormat("jal    %s", target_name);
    } else if (inst.opcode == Opcode::kJ) {
      uint32_t target = inst.JumpTarget();
      bool target_is_func = known_entrypoints_.contains(target);
      if (!target_is_func && symbols_ && symbols_->HasAddress(target)) {
        const auto* entry = symbols_->FindByAddress(target);
        if (entry != nullptr && entry->type() == SYMBOL_FUNC) {
          target_is_func = true;
        }
      }
      if (target_is_func) {
        std::string target_name = symbols_ ? symbols_->LookupOrSynthesizeName(target, SYMBOL_FUNC)
                                           : absl::StrFormat("func_%08X", target);
        formatted = absl::StrFormat("j      %s", target_name);
      } else if (local_labels.contains(target) || segment_labels_.contains(target)) {
        formatted = absl::StrFormat("j      .L%08X", target);
      } else {
        std::string target_name = symbols_ ? symbols_->LookupOrSynthesizeName(target, SYMBOL_FUNC)
                                           : absl::StrFormat("func_%08X", target);
        formatted = absl::StrFormat("j      %s", target_name);
      }
    } else if (inst.IsBranch()) {
      uint32_t target = inst.BranchTarget();
      bool target_is_func = known_entrypoints_.contains(target);
      if (!target_is_func && symbols_ && symbols_->HasAddress(target)) {
        const auto* entry = symbols_->FindByAddress(target);
        if (entry != nullptr && entry->type() == SYMBOL_FUNC) {
          target_is_func = true;
        }
      }
      std::string label;
      if (target_is_func) {
        label = symbols_ ? symbols_->LookupOrSynthesizeName(target, SYMBOL_FUNC)
                         : absl::StrFormat("func_%08X", target);
      } else {
        label = absl::StrFormat(".L%08X", target);
      }
      formatted = FormatBranch(inst, label);
    } else if (active_tracker != nullptr && active_tracker->FindRelocation(pc) != nullptr) {
      formatted = active_tracker->FormatInstruction(inst);
    } else {
      formatted = inst.Disassemble();
    }

    d_inst.formatted_asm = formatted;
    func.instructions.push_back(d_inst);

    // Delay slot instruction gets an extra leading space
    std::string indent = prev_has_delay_slot ? "   " : "  ";
    if (options_.emit_line_comments) {
      absl::StrAppend(&asm_text, d_inst.line_comment, indent, formatted, "\n");
    } else {
      absl::StrAppend(&asm_text, indent, formatted, "\n");
    }

    prev_has_delay_slot = inst.HasDelaySlot();
  }

  if (options_.emit_function_framing) {
    absl::StrAppend(&asm_text, absl::StrFormat(".end %s\n", func.name));
  }

  func.emitted_assembly = std::move(asm_text);
  return func;
}

absl::StatusOr<std::vector<DisassembledFunction>> Disassembler::DisassembleAllFunctions(
    absl::Span<const uint8_t> code, uint32_t vram_start) const {
  std::unique_ptr<RelocationTracker> buffer_tracker;
  const RelocationTracker* active_tracker = reloc_tracker_;
  if (active_tracker == nullptr && options_.emit_relocations) {
    buffer_tracker = std::make_unique<RelocationTracker>(symbols_);
    auto status = buffer_tracker->AnalyzeCode(code, vram_start);
    if (!status.ok()) {
      return status;
    }
    active_tracker = buffer_tracker.get();
  }

  Disassembler func_disasm = *this;
  func_disasm.SetRelocationTracker(active_tracker);
  func_disasm.PreScanCode(code, vram_start);

  std::vector<DisassembledFunction> functions;
  size_t offset = 0;

  while (offset + 4 <= code.size()) {
    uint32_t current_vram = vram_start + static_cast<uint32_t>(offset);
    auto func_or = func_disasm.DisassembleFunction(code.subspan(offset), current_vram);
    if (!func_or.ok()) {
      return func_or.status();
    }

    size_t func_bytes = func_or->Size();
    if (func_bytes == 0) {
      break;
    }

    functions.push_back(std::move(*func_or));
    offset += func_bytes;
  }

  return functions;
}

absl::StatusOr<std::string> Disassembler::DisassembleRange(absl::Span<const uint8_t> code,
                                                           uint32_t vram_start) const {
  auto funcs_or = DisassembleAllFunctions(code, vram_start);
  if (!funcs_or.ok()) {
    return funcs_or.status();
  }

  std::string full_asm;
  for (const auto& func : *funcs_or) {
    absl::StrAppend(&full_asm, func.emitted_assembly, "\n");
  }
  return full_asm;
}

}  // namespace rom_nom_nom
