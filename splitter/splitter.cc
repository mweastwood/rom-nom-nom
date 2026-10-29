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
#include "core/disassembler.h"
#include "core/game_config.h"
#include "core/game_config.pb.h"
#include "core/rom.h"
#include "core/symbol_registry.h"
#include "splitter/assembly_generator.h"
#include "splitter/auto_symbols.h"
#include "splitter/linker_script.h"
#include "splitter/slicer.h"

namespace rom_nom_nom {

absl::StatusOr<SplitterResult> RunSplitter(const SplitterOptions& options) {
  SplitterResult result;

  // 1. Load split configuration
  GameConfig config;
  if (!options.config_text_override.empty()) {
    auto config_or = ParseGameConfig(options.config_text_override);
    if (!config_or.ok()) {
      return config_or.status();
    }
    config = std::move(*config_or);
  } else {
    if (options.config_path.empty()) {
      return absl::InvalidArgumentError(
          "Splitter: config_path must not be empty (--config is required)");
    }
    auto config_or = LoadGameConfig(options.config_path);
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
  auto index_or = LoadSymbolIndex({
      .symbols_text_override = options.symbols_text_override,
      .symbols_path = options.symbols_path,
      .config = &config,
      .game_name = std::string(config.basename()),
  });
  if (!index_or.ok()) {
    return index_or.status();
  }
  SymbolIndex symbol_index = std::move(*index_or);

  // 4. Carve non-code binary assets (.bin, header, raw data)
  if (options.slice_data) {
    SlicerOptions slicer_options;
    if (!options.assets_out_dir.empty()) {
      slicer_options.output_dir = (options.assets_out_dir.filename() == config.basename() &&
                                   options.assets_out_dir.has_parent_path())
                                      ? options.assets_out_dir.parent_path()
                                      : options.assets_out_dir;
    } else {
      slicer_options.output_dir = options.out_dir;
    }
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

    std::filesystem::path game_build_dir = (options.out_dir.filename() == config.basename())
                                               ? options.out_dir
                                               : (options.out_dir / config.basename());
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

  // 6. Disassemble Code Subsegments into Assembly (.s) & Discover Auto Symbols
  if (options.disassemble_code) {
    DisassemblerOptions disasm_options;
    disasm_options.emit_line_comments = options.emit_line_comments;
    disasm_options.emit_function_framing = true;
    disasm_options.emit_relocations = options.emit_relocations;
    Disassembler disassembler(symbol_index.Empty() ? nullptr : &symbol_index, disasm_options);

    AutoSymbolFinder auto_symbols(symbol_index.Empty() ? nullptr : &symbol_index);

    const std::vector<ResolvedSegment> resolved_segments = ResolveSegments(config, rom.Size());

    // Register defined segments and subsegments
    for (const auto& seg : resolved_segments) {
      if (seg.vram_start != 0 && seg.RomSize() > 0) {
        auto_symbols.RegisterDefinedAddressRange(seg.vram_start, seg.vram_start + seg.RomSize());
      }
      for (const auto& sub : seg.subsegments) {
        if (sub.proto != nullptr && sub.proto->vram() != 0) {
          auto_symbols.RegisterDefinedSymbol(sub.name, sub.vram_start);
        }
      }
    }

    std::filesystem::path asm_dir = options.asm_out_dir;
    if (asm_dir.empty()) {
      asm_dir = std::filesystem::path("asm") / config.basename();
    }
    std::filesystem::create_directories(asm_dir);

    AssemblyGeneratorOptions asm_opts;
    asm_opts.asm_dir = asm_dir;
    asm_opts.symbols = symbol_index.Empty() ? nullptr : &symbol_index;
    AssemblyGenerator asm_gen(asm_opts);

    // Emit standard macro.inc and copy c_macro.inc if present
    std::filesystem::path game_macro =
        !config.macro_inc().empty()
            ? std::filesystem::path(config.macro_inc())
            : (std::filesystem::path("macros") / absl::StrCat(config.basename(), "_macro.inc"));
    auto macro_status = asm_gen.EmitMacroIncludes(game_macro);
    if (!macro_status.ok()) {
      return macro_status;
    }

    // Emit header.s for SEGMENT_HEADER
    for (const auto& seg : resolved_segments) {
      if (seg.type == SEGMENT_HEADER) {
        size_t seg_start = seg.rom_start;
        size_t seg_end = seg.rom_end;
        if (seg_end > seg_start && seg_end <= rom.Size()) {
          auto span_or = rom.SliceRange(seg_start, seg_end);
          if (span_or.ok()) {
            auto h_status = asm_gen.WriteHeaderAssembly(*span_or, asm_dir / "header.s");
            if (!h_status.ok()) {
              return h_status;
            }
            result.asm_files_written++;
          }
        }
      }
    }

    // Pre-scan all code segments across the ROM for function entrypoints and labels
    for (const auto& seg : resolved_segments) {
      if (seg.type != SEGMENT_CODE) {
        continue;
      }
      size_t seg_start = seg.rom_start;
      size_t seg_end = seg.rom_end;
      if (seg_start >= seg_end || seg_end > rom.Size() || seg.vram_start == 0) {
        continue;
      }
      auto span_or = rom.SliceRange(seg_start, seg_end);
      if (span_or.ok()) {
        disassembler.PreScanCode(*span_or, seg.vram_start);
      }
    }

    for (const auto& seg : resolved_segments) {
      if (seg.type != SEGMENT_CODE) {
        continue;
      }

      for (const auto& sub : seg.subsegments) {
        size_t sub_start = sub.rom_start;
        size_t sub_end = sub.rom_end;
        uint32_t sub_vram = sub.vram_start;

        if (sub.type == SUBSEGMENT_DATA) {
          if (sub_start >= sub_end || sub_end > rom.Size()) {
            continue;
          }
          auto span_or = rom.SliceRange(sub_start, sub_end);
          if (!span_or.ok()) {
            return span_or.status();
          }
          std::filesystem::path out_data_path = asm_gen.ResolveDataPath(sub.name);
          auto d_status = asm_gen.WriteDataAssembly(*span_or, sub_vram, out_data_path);
          if (!d_status.ok()) {
            return d_status;
          }
          result.asm_files_written++;
          continue;
        }

        if (sub.type == SUBSEGMENT_BSS) {
          size_t bss_size = sub.bss_size > 0 ? sub.bss_size : 0x10;
          std::filesystem::path out_bss_path = asm_gen.ResolveBssPath(sub.name);
          auto b_status = asm_gen.WriteBssAssembly(bss_size, sub_vram, out_bss_path);
          if (!b_status.ok()) {
            return b_status;
          }
          result.asm_files_written++;
          continue;
        }

        if (sub.type == SUBSEGMENT_C) {
          if (sub_start >= sub_end || sub_end > rom.Size()) {
            continue;
          }
          auto code_span_or = rom.SliceRange(sub_start, sub_end);
          if (!code_span_or.ok()) {
            return code_span_or.status();
          }

          if (options.generate_undefined_symbols) {
            auto scan_status = auto_symbols.ScanCode(*code_span_or, sub_vram);
            if (!scan_status.ok()) {
              return scan_status;
            }
          }

          auto funcs_or = disassembler.DisassembleAllFunctions(*code_span_or, sub_vram);
          if (!funcs_or.ok()) {
            return funcs_or.status();
          }

          auto written_count_or = asm_gen.WriteNonmatchingFunctions(sub.name, *funcs_or);
          if (!written_count_or.ok()) {
            return written_count_or.status();
          }
          result.asm_files_written += *written_count_or;
          result.functions_disassembled += funcs_or->size();

          std::filesystem::path out_s_path = asm_dir / absl::StrCat(sub.name, ".s");
          auto s_status = asm_gen.WriteStandaloneAssembly(sub.name, sub_start, sub_end, sub_vram,
                                                          *funcs_or, out_s_path);
          if (!s_status.ok()) {
            return s_status;
          }
          result.asm_files_written++;
          continue;
        }

        if (sub.type != SUBSEGMENT_ASM) {
          // Skip other non-asm files
          continue;
        }

        if (sub_start >= sub_end || sub_end > rom.Size()) {
          continue;
        }

        auto code_span_or = rom.SliceRange(sub_start, sub_end);
        if (!code_span_or.ok()) {
          return code_span_or.status();
        }

        // Scan code for undefined function calls and relocations
        if (options.generate_undefined_symbols) {
          auto scan_status = auto_symbols.ScanCode(*code_span_or, sub_vram);
          if (!scan_status.ok()) {
            return scan_status;
          }
        }

        auto funcs_or = disassembler.DisassembleAllFunctions(*code_span_or, sub_vram);
        if (!funcs_or.ok()) {
          return funcs_or.status();
        }

        std::filesystem::path out_s_path = asm_dir / absl::StrCat(sub.name, ".s");
        auto s_status = asm_gen.WriteStandaloneAssembly(sub.name, sub_start, sub_end, sub_vram,
                                                        *funcs_or, out_s_path);
        if (!s_status.ok()) {
          return s_status;
        }
        result.functions_disassembled += funcs_or->size();
        result.asm_files_written++;
      }
    }

    // Write undefined symbol scripts
    if (options.generate_undefined_symbols) {
      std::filesystem::path game_build_dir = (options.out_dir.filename() == config.basename())
                                                 ? options.out_dir
                                                 : (options.out_dir / config.basename());
      std::filesystem::create_directories(game_build_dir);

      std::filesystem::path undef_syms_path = options.undefined_syms_path;
      if (undef_syms_path.empty()) {
        undef_syms_path = game_build_dir / "undefined_syms_auto.txt";
      }
      std::filesystem::create_directories(undef_syms_path.parent_path());
      std::ofstream syms_out(undef_syms_path, std::ios::out | std::ios::trunc);
      syms_out << auto_symbols.GenerateUndefinedSymsScript();

      std::filesystem::path undef_funcs_path = options.undefined_funcs_path;
      if (undef_funcs_path.empty()) {
        undef_funcs_path = game_build_dir / "undefined_funcs_auto.txt";
      }
      std::filesystem::create_directories(undef_funcs_path.parent_path());
      std::ofstream funcs_out(undef_funcs_path, std::ios::out | std::ios::trunc);
      funcs_out << auto_symbols.GenerateUndefinedFuncsScript();

      result.undefined_data_symbols_found = auto_symbols.DiscoveredDataSymbols().size();
      result.undefined_func_symbols_found = auto_symbols.DiscoveredFuncSymbols().size();
    }
  }

  return result;
}

}  // namespace rom_nom_nom
