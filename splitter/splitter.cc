#include "splitter/splitter.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "core/rom.h"
#include "splitter/config.h"
#include "splitter/config.pb.h"
#include "splitter/disassembler.h"
#include "splitter/linker_script.h"
#include "splitter/slicer.h"
#include "splitter/symbol_registry.h"

namespace rom_nom_nom {

absl::StatusOr<SplitterResult> RunSplitter(const SplitterOptions& options) {
  SplitterResult result;

  // 1. Load split configuration
  SplitConfig config;
  if (!options.config_text_override.empty()) {
    auto config_or = ParseSplitConfig(options.config_text_override);
    if (!config_or.ok()) {
      return config_or.status();
    }
    config = std::move(*config_or);
  } else {
    if (options.config_path.empty()) {
      return absl::InvalidArgumentError(
          "Splitter: config_path must not be empty (--config is required)");
    }
    auto config_or = LoadSplitConfig(options.config_path);
    if (!config_or.ok()) {
      return config_or.status();
    }
    config = std::move(*config_or);
  }

  // 2. Open and verify input ROM
  absl::StatusOr<Rom> rom_or;
  if (!options.rom_buffer_override.empty()) {
    rom_or = Rom::FromBuffer(options.rom_buffer_override);
  } else {
    std::filesystem::path rom_path = options.rom_path;
    if (rom_path.empty()) {
      rom_path = std::filesystem::path("roms") / absl::StrCat(config.basename(), ".z64");
    }
    rom_or = Rom::OpenFile(rom_path);
    if (!rom_or.ok()) {
      return absl::NotFoundError(absl::StrFormat("Splitter: Failed to open ROM at '%s': %s",
                                                 rom_path.string(), rom_or.status().message()));
    }
  }

  if (!rom_or.ok()) {
    return rom_or.status();
  }
  const Rom& rom = *rom_or;

  if (options.verify_sha1 && !config.sha1().empty()) {
    std::string actual_sha1 = rom.Sha1();
    if (actual_sha1 != config.sha1()) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "Splitter: ROM SHA-1 mismatch: expected %s, got %s", config.sha1(), actual_sha1));
    }
  }

  // 3. Load Symbol Index
  SymbolIndex symbol_index;
  if (!options.symbols_text_override.empty()) {
    auto index_or = SymbolIndex::ParseFromTextproto(options.symbols_text_override);
    if (!index_or.ok()) {
      return index_or.status();
    }
    symbol_index = std::move(*index_or);
  } else if (!options.symbols_path.empty()) {
    auto index_or = SymbolIndex::LoadFromTextproto(options.symbols_path);
    if (!index_or.ok()) {
      return index_or.status();
    }
    symbol_index = std::move(*index_or);
  } else {
    for (const auto& sym_file : config.symbol_files()) {
      if (std::filesystem::exists(sym_file)) {
        auto index_or = SymbolIndex::LoadFromTextproto(sym_file);
        if (index_or.ok()) {
          symbol_index = std::move(*index_or);
          break;
        }
      }
    }
  }

  // 4. Carve non-code binary assets (.bin, header, raw data)
  if (options.slice_data) {
    SlicerOptions slicer_options;
    slicer_options.output_dir = options.out_dir;
    slicer_options.verify_sha1 = options.verify_sha1;
    Slicer slicer(slicer_options);
    auto slice_report_or = slicer.SliceAll(rom, config);
    if (!slice_report_or.ok()) {
      return slice_report_or.status();
    }
    result.bytes_carved += slice_report_or->total_bytes_carved;
    result.files_written += slice_report_or->total_files_written;
  }

  // 5. Generate GNU ld Linker Scripts
  if (options.write_ld_script) {
    LinkerScriptOptions ls_options;
    ls_options.base_build_dir = options.out_dir.string();
    LinkerScriptGenerator ls_gen(ls_options);

    std::filesystem::path game_build_dir = options.out_dir / config.basename();
    std::filesystem::create_directories(game_build_dir);

    // Primary linker script
    std::filesystem::path ld_path = options.ld_script_path;
    if (ld_path.empty()) {
      ld_path = game_build_dir / absl::StrCat(config.basename(), ".ld");
    }
    std::filesystem::create_directories(ld_path.parent_path());

    auto main_ld_or = ls_gen.GenerateMainScript(config);
    if (!main_ld_or.ok()) {
      return main_ld_or.status();
    }
    std::ofstream ld_file(ld_path, std::ios::out | std::ios::trunc);
    if (!ld_file.is_open()) {
      return absl::InternalError(
          absl::StrFormat("Failed to open linker script file for writing: %s", ld_path.string()));
    }
    ld_file << *main_ld_or;
    result.ld_script_written = true;

    // Symbols linker script
    if (!symbol_index.Empty()) {
      std::filesystem::path sym_ld_path = options.symbols_ld_path;
      if (sym_ld_path.empty()) {
        sym_ld_path = game_build_dir / "symbols.ld";
      }
      std::ofstream sym_file(sym_ld_path, std::ios::out | std::ios::trunc);
      sym_file << ls_gen.GenerateSymbolsScript(symbol_index);
    }

    // Hardware registers linker script
    std::filesystem::path hw_ld_path = game_build_dir / "hardware_regs.ld";
    std::ofstream hw_file(hw_ld_path, std::ios::out | std::ios::trunc);
    hw_file << ls_gen.GenerateHardwareRegsScript();
  }

  // 6. Disassemble Code Subsegments into Assembly (.s)
  if (options.disassemble_code) {
    DisassemblerOptions disasm_options;
    disasm_options.emit_line_comments = options.emit_line_comments;
    disasm_options.emit_function_framing = true;
    Disassembler disassembler(symbol_index.Empty() ? nullptr : &symbol_index, disasm_options);

    std::filesystem::path asm_dir = options.asm_out_dir;
    if (asm_dir.empty()) {
      asm_dir = std::filesystem::path("asm") / config.basename();
    }

    for (int i = 0; i < config.segments_size(); ++i) {
      const Segment& seg = config.segments(i);
      if (seg.type() != SEGMENT_CODE) {
        continue;
      }

      size_t seg_start = seg.rom_start();
      size_t seg_end = seg.rom_end();
      if (seg_end == 0) {
        if (i + 1 < config.segments_size() && config.segments(i + 1).rom_start() > seg_start) {
          seg_end = std::min<size_t>(config.segments(i + 1).rom_start(), rom.Size());
        } else {
          seg_end = rom.Size();
        }
      }

      for (int j = 0; j < seg.subsegments_size(); ++j) {
        const Subsegment& sub = seg.subsegments(j);
        if (sub.type() != SUBSEGMENT_ASM && sub.type() != SUBSEGMENT_HASM) {
          // Skip C files or data files (data is handled by slicer)
          continue;
        }

        size_t sub_start = sub.rom_start();
        size_t sub_end = seg_end;
        if (j + 1 < seg.subsegments_size()) {
          sub_end = seg.subsegments(j + 1).rom_start();
        }

        if (sub_start >= sub_end || sub_end > rom.Size()) {
          continue;
        }

        uint32_t sub_vram = sub.vram();
        if (sub_vram == 0 && seg.vram() != 0) {
          sub_vram = seg.vram() + static_cast<uint32_t>(sub_start - seg_start);
        }

        auto code_span_or = rom.SliceRange(sub_start, sub_end);
        if (!code_span_or.ok()) {
          return code_span_or.status();
        }

        auto funcs_or = disassembler.DisassembleAllFunctions(*code_span_or, sub_vram);
        if (!funcs_or.ok()) {
          return funcs_or.status();
        }

        std::string emitted_s;
        emitted_s += absl::StrFormat(
            ".include \"macro.inc\"\n\n# Subsegment: %s [0x%X - 0x%X] VRAM: 0x%08X\n.section "
            ".text\n\n",
            sub.name(), sub_start, sub_end, sub_vram);
        for (const auto& func : *funcs_or) {
          emitted_s += func.emitted_assembly;
          emitted_s += "\n\n";
        }

        std::filesystem::path out_s_path = asm_dir / absl::StrCat(sub.name(), ".s");
        std::filesystem::create_directories(out_s_path.parent_path());
        std::ofstream s_file(out_s_path, std::ios::out | std::ios::trunc);
        s_file << emitted_s;

        result.functions_disassembled += funcs_or->size();
        result.asm_files_written++;
      }
    }
  }

  return result;
}

}  // namespace rom_nom_nom
