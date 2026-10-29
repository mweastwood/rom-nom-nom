#include "core/game_config_rom_validator.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "core/disassembler.h"
#include "core/game_config.h"
#include "core/mips.h"
#include "core/symbol_registry.h"

namespace rom_nom_nom {
namespace {

struct FunctionSignatureSummary {
  bool has_sp_allocation = false;
  bool has_o0_frame_pointer = false;
  std::vector<std::string> hasm_reasons;

  bool IsHandWrittenAssembly() const { return !hasm_reasons.empty(); }
};

bool IsMips364BitOpcode(Opcode op) {
  switch (op) {
    case Opcode::kLd:
    case Opcode::kSd:
    case Opcode::kLld:
    case Opcode::kScd:
    case Opcode::kLdl:
    case Opcode::kLdr:
    case Opcode::kLwu:
    case Opcode::kSdl:
    case Opcode::kSdr:
    case Opcode::kDadd:
    case Opcode::kDaddu:
    case Opcode::kDaddi:
    case Opcode::kDaddiu:
    case Opcode::kDsub:
    case Opcode::kDsubu:
    case Opcode::kDsll:
    case Opcode::kDsrl:
    case Opcode::kDsra:
    case Opcode::kDsll32:
    case Opcode::kDsrl32:
    case Opcode::kDsra32:
    case Opcode::kDsllv:
    case Opcode::kDsrlv:
    case Opcode::kDsrav:
    case Opcode::kDmult:
    case Opcode::kDmultu:
    case Opcode::kDdiv:
    case Opcode::kDdivu:
    case Opcode::kDmfc1:
    case Opcode::kDmtc1:
      return true;
    default:
      return false;
  }
}

bool UsesKernelRegister(const Instruction& inst) {
  auto is_kernel = [](const std::optional<Register>& reg) {
    return reg == Register::kK0 || reg == Register::kK1;
  };
  return is_kernel(inst.rd) || is_kernel(inst.rs) || is_kernel(inst.rt);
}

void AddUniqueReason(std::vector<std::string>* reasons, std::string_view reason) {
  if (std::find(reasons->begin(), reasons->end(), reason) == reasons->end()) {
    reasons->emplace_back(reason);
  }
}

FunctionSignatureSummary AnalyzeFunctionSignatures(const DisassembledFunction& func) {
  FunctionSignatureSummary summary;
  bool has_fp_save = false;
  bool has_fp_move = false;
  bool has_jal = false;
  size_t lwl_count = 0;
  size_t lwr_count = 0;
  size_t swl_count = 0;
  size_t swr_count = 0;

  for (size_t i = 0; i < func.instructions.size(); ++i) {
    const auto& d_inst = func.instructions[i];
    if (d_inst.raw_word == 0) {
      continue;
    }

    const Instruction& inst = d_inst.instruction;
    if (inst.opcode == Opcode::kUnknown) {
      uint32_t major_op = d_inst.raw_word >> 26;
      if (major_op == 0x2F) {
        AddUniqueReason(&summary.hasm_reasons, "CACHE instruction");
      } else if (major_op == 0x10) {
        AddUniqueReason(&summary.hasm_reasons, "COP0 system instruction (tlb/eret)");
      } else {
        AddUniqueReason(&summary.hasm_reasons,
                        absl::StrFormat("non-compiler raw word 0x%08X", d_inst.raw_word));
      }
      continue;
    }

    if (inst.opcode == Opcode::kAddiu && inst.rs == Register::kSp && inst.rt == Register::kSp &&
        inst.immediate < 0) {
      summary.has_sp_allocation = true;
    }

    if (i < 16) {
      if (inst.opcode == Opcode::kSw && inst.rs == Register::kSp && inst.rt == Register::kFp) {
        has_fp_save = true;
      }
      if ((inst.opcode == Opcode::kAddu || inst.opcode == Opcode::kOr ||
           inst.opcode == Opcode::kDaddu) &&
          inst.rd == Register::kFp &&
          ((inst.rs == Register::kSp && inst.rt == Register::kZero) ||
           (inst.rs == Register::kZero && inst.rt == Register::kSp))) {
        has_fp_move = true;
      }
    }

    if (inst.opcode == Opcode::kJal) {
      has_jal = true;
    }

    if (inst.opcode == Opcode::kMfc0 || inst.opcode == Opcode::kMtc0) {
      AddUniqueReason(&summary.hasm_reasons, "COP0 register transfer (mfc0/mtc0)");
    } else if (inst.opcode == Opcode::kCfc1 || inst.opcode == Opcode::kCtc1) {
      AddUniqueReason(&summary.hasm_reasons, "COP1 control register transfer (cfc1/ctc1)");
    } else if (IsMips364BitOpcode(inst.opcode)) {
      AddUniqueReason(&summary.hasm_reasons, "64-bit MIPS III instruction");
    } else if (inst.opcode == Opcode::kXor && inst.rd.has_value() && inst.rd != Register::kZero &&
               inst.rd == inst.rs && inst.rs == inst.rt) {
      AddUniqueReason(&summary.hasm_reasons, "hand-written self-XOR register clear");
    } else if (inst.opcode == Opcode::kLui && inst.rt == Register::kSp) {
      AddUniqueReason(&summary.hasm_reasons, "direct stack pointer initialization (lui $sp)");
    }

    if (UsesKernelRegister(inst)) {
      AddUniqueReason(&summary.hasm_reasons, "kernel register ($k0/$k1)");
    }

    if (inst.opcode == Opcode::kLwl) {
      ++lwl_count;
    } else if (inst.opcode == Opcode::kLwr) {
      ++lwr_count;
    } else if (inst.opcode == Opcode::kSwl) {
      ++swl_count;
    } else if (inst.opcode == Opcode::kSwr) {
      ++swr_count;
    }
  }

  if (has_jal && !summary.has_sp_allocation) {
    AddUniqueReason(&summary.hasm_reasons, "frameless non-leaf call (jal without stack frame)");
  }
  if (lwl_count != lwr_count || swl_count != swr_count) {
    AddUniqueReason(&summary.hasm_reasons, "unpaired unaligned load/store (lwl/lwr or swl/swr)");
  }

  summary.has_o0_frame_pointer = summary.has_sp_allocation && has_fp_save && has_fp_move;
  return summary;
}

bool HasO0FramePointerFlag(const std::vector<std::string>& c_flags) {
  bool is_o0 = false;
  bool force_frame_pointer = false;
  for (std::string_view flag : c_flags) {
    if (flag == "-O0") {
      is_o0 = true;
    } else if (flag == "-O" || flag == "-O1" || flag == "-O2" || flag == "-O3" || flag == "-Os") {
      is_o0 = false;
    } else if (flag == "-fno-omit-frame-pointer") {
      force_frame_pointer = true;
    } else if (flag == "-fomit-frame-pointer") {
      force_frame_pointer = false;
    }
  }
  return is_o0 || force_frame_pointer;
}

}  // namespace

ConfigValidationReport AnalyzeGameConfigAgainstRom(const GameConfig& config, const Rom& rom) {
  ConfigValidationReport report;

  if (absl::Status status = ValidateGameConfig(config); !status.ok()) {
    report.violations.push_back({
        .message = std::string(status.message()),
    });
    return report;
  }

  if (!config.sha1().empty()) {
    std::string actual_sha1 = rom.Sha1();
    if (absl::AsciiStrToLower(actual_sha1) != absl::AsciiStrToLower(config.sha1())) {
      report.violations.push_back({
          .message = absl::StrFormat("ROM SHA-1 mismatch: config expects %s, actual ROM is %s",
                                     config.sha1(), actual_sha1),
      });
    }
  }

  auto symbols_or = SymbolIndex::FromGameConfig(config);
  SymbolIndex symbols = symbols_or.ok() ? std::move(*symbols_or) : SymbolIndex();
  DisassemblerOptions disasm_opts;
  disasm_opts.emit_line_comments = false;
  disasm_opts.emit_function_framing = false;
  disasm_opts.emit_relocations = false;
  Disassembler disassembler(symbols.Empty() ? nullptr : &symbols, disasm_opts);

  const std::vector<ResolvedSegment> resolved_segments = ResolveSegments(config, rom.Size());

  // Pre-scan all code segments so DisassembleAllFunctions knows all jal entrypoints.
  for (const auto& seg : resolved_segments) {
    if (seg.type != SEGMENT_CODE) {
      continue;
    }
    if (seg.rom_start >= seg.rom_end || seg.rom_end > rom.Size()) {
      report.violations.push_back({
          .segment_name = seg.name,
          .rom_offset = seg.rom_start,
          .vram = seg.vram_start,
          .message = absl::StrFormat(
              "Code segment '%s' bounds [0x%X, 0x%X) are invalid for ROM of size 0x%X", seg.name,
              seg.rom_start, seg.rom_end, rom.Size()),
      });
      continue;
    }
    if (seg.vram_start == 0) {
      report.violations.push_back({
          .segment_name = seg.name,
          .rom_offset = seg.rom_start,
          .message =
              absl::StrFormat("Code segment '%s' is missing a valid base VRAM address", seg.name),
      });
      continue;
    }
    auto span_or = rom.SliceRange(seg.rom_start, seg.rom_end);
    if (span_or.ok()) {
      disassembler.PreScanCode(*span_or, seg.vram_start);
    }
  }

  for (const auto& seg : resolved_segments) {
    if (seg.type != SEGMENT_CODE || seg.rom_start >= seg.rom_end || seg.rom_end > rom.Size() ||
        seg.vram_start == 0) {
      continue;
    }
    ++report.code_segments_checked;

    for (const auto& sub : seg.subsegments) {
      if (sub.type == SUBSEGMENT_BSS) {
        continue;
      }

      if (sub.rom_start < seg.rom_start || sub.rom_end > seg.rom_end ||
          sub.rom_start >= sub.rom_end) {
        report.violations.push_back({
            .segment_name = seg.name,
            .subsegment_name = sub.name,
            .rom_offset = sub.rom_start,
            .vram = sub.vram_start,
            .message = absl::StrFormat(
                "Subsegment '%s' bounds [0x%X, 0x%X) are invalid within segment '%s' [0x%X, 0x%X)",
                sub.name, sub.rom_start, sub.rom_end, seg.name, seg.rom_start, seg.rom_end),
        });
        continue;
      }

      if ((sub.rom_start & 0xFu) != 0 || (sub.rom_end & 0xFu) != 0) {
        report.violations.push_back({
            .segment_name = seg.name,
            .subsegment_name = sub.name,
            .rom_offset = sub.rom_start,
            .vram = sub.vram_start,
            .message = absl::StrFormat(
                "Subsegment '%s' [0x%X, 0x%X) in code segment '%s' is not 16-byte (0x10) aligned",
                sub.name, sub.rom_start, sub.rom_end, seg.name),
        });
      }

      if (sub.type != SUBSEGMENT_C && sub.type != SUBSEGMENT_ASM && sub.type != SUBSEGMENT_HASM) {
        continue;
      }
      ++report.code_subsegments_checked;

      auto code_span_or = rom.SliceRange(sub.rom_start, sub.rom_end);
      if (!code_span_or.ok()) {
        report.violations.push_back({
            .segment_name = seg.name,
            .subsegment_name = sub.name,
            .rom_offset = sub.rom_start,
            .vram = sub.vram_start,
            .message = std::string(code_span_or.status().message()),
        });
        continue;
      }

      auto funcs_or = disassembler.DisassembleAllFunctions(*code_span_or, sub.vram_start);
      if (!funcs_or.ok()) {
        report.violations.push_back({
            .segment_name = seg.name,
            .subsegment_name = sub.name,
            .rom_offset = sub.rom_start,
            .vram = sub.vram_start,
            .message = std::string(funcs_or.status().message()),
        });
        continue;
      }

      std::vector<std::string> resolved_flags = ResolveCFlags(config, sub.name);
      bool configured_o0 = HasO0FramePointerFlag(resolved_flags);

      size_t hasm_func_count = 0;
      size_t sp_alloc_func_count = 0;
      size_t o0_fp_func_count = 0;

      for (const auto& func : *funcs_or) {
        if (!func.is_function || func.instructions.empty()) {
          continue;
        }
        ++report.functions_checked;
        uint32_t func_rom_offset = sub.rom_start + (func.vram_start - sub.vram_start);
        FunctionSignatureSummary sig = AnalyzeFunctionSignatures(func);

        if (sig.IsHandWrittenAssembly()) {
          ++hasm_func_count;
          if (sub.type == SUBSEGMENT_C || sub.type == SUBSEGMENT_ASM) {
            report.violations.push_back({
                .segment_name = seg.name,
                .subsegment_name = sub.name,
                .rom_offset = func_rom_offset,
                .vram = func.vram_start,
                .message = absl::StrFormat(
                    "Function '%s' (ROM 0x%X, VRAM 0x%08X) in C-lifted subsegment '%s' contains "
                    "hand-written assembly signature(s): %s (should be SUBSEGMENT_HASM)",
                    func.name, func_rom_offset, func.vram_start, sub.name,
                    absl::StrJoin(sig.hasm_reasons, ", ")),
            });
          }
          continue;
        }

        if (sub.type == SUBSEGMENT_C || sub.type == SUBSEGMENT_ASM) {
          if (sig.has_sp_allocation) {
            ++sp_alloc_func_count;
          }
          if (sig.has_o0_frame_pointer) {
            ++o0_fp_func_count;
          }

          if (!configured_o0 && sig.has_o0_frame_pointer) {
            report.violations.push_back({
                .segment_name = seg.name,
                .subsegment_name = sub.name,
                .rom_offset = func_rom_offset,
                .vram = func.vram_start,
                .message = absl::StrFormat(
                    "Function '%s' (ROM 0x%X, VRAM 0x%08X) in subsegment '%s' has an -O0 frame-"
                    "pointer prologue (sw $fp + move $fp, $sp), but resolved c_flags [%s] do not "
                    "enable -O0",
                    func.name, func_rom_offset, func.vram_start, sub.name,
                    absl::StrJoin(resolved_flags, " ")),
            });
          } else if (configured_o0 && sig.has_sp_allocation && !sig.has_o0_frame_pointer) {
            report.violations.push_back({
                .segment_name = seg.name,
                .subsegment_name = sub.name,
                .rom_offset = func_rom_offset,
                .vram = func.vram_start,
                .message = absl::StrFormat(
                    "Function '%s' (ROM 0x%X, VRAM 0x%08X) in -O0 subsegment '%s' allocates a "
                    "stack frame without establishing an -O0 frame pointer ($fp)",
                    func.name, func_rom_offset, func.vram_start, sub.name),
            });
          }
        }
      }

      if (sub.type == SUBSEGMENT_HASM && hasm_func_count == 0) {
        report.violations.push_back({
            .segment_name = seg.name,
            .subsegment_name = sub.name,
            .rom_offset = sub.rom_start,
            .vram = sub.vram_start,
            .message = absl::StrFormat(
                "Subsegment '%s' [0x%X, 0x%X) is marked SUBSEGMENT_HASM, but none of its "
                "functions contain hand-written assembly signatures",
                sub.name, sub.rom_start, sub.rom_end),
        });
      }

      if ((sub.type == SUBSEGMENT_C || sub.type == SUBSEGMENT_ASM) && configured_o0 &&
          sp_alloc_func_count == 0 && o0_fp_func_count == 0) {
        report.violations.push_back({
            .segment_name = seg.name,
            .subsegment_name = sub.name,
            .rom_offset = sub.rom_start,
            .vram = sub.vram_start,
            .message = absl::StrFormat(
                "Subsegment '%s' [0x%X, 0x%X) is configured with -O0, but contains no functions "
                "with an -O0 frame pointer ($fp)",
                sub.name, sub.rom_start, sub.rom_end),
        });
      }
    }
  }

  return report;
}

absl::Status ValidateGameConfigAgainstRom(const GameConfig& config, const Rom& rom) {
  ConfigValidationReport report = AnalyzeGameConfigAgainstRom(config, rom);
  if (report.Ok()) {
    return absl::OkStatus();
  }

  std::string message =
      absl::StrFormat("GameConfig '%s' failed ROM binary validation with %zu violation(s):\n",
                      config.game_name().empty() ? config.basename() : config.game_name(),
                      report.violations.size());
  const size_t max_listed = 25;
  for (size_t i = 0; i < std::min(report.violations.size(), max_listed); ++i) {
    absl::StrAppendFormat(&message, "  [%zu] %s\n", i + 1, report.violations[i].message);
  }
  if (report.violations.size() > max_listed) {
    absl::StrAppendFormat(&message, "  ... and %zu more violation(s)\n",
                          report.violations.size() - max_listed);
  }
  return absl::InvalidArgumentError(message);
}

}  // namespace rom_nom_nom
