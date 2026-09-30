#include <cstdlib>
#include <iostream>
#include <string>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "absl/strings/str_format.h"
#include "splitter/splitter.h"

ABSL_FLAG(std::string, config, "", "Path to split config (.textproto)");
ABSL_FLAG(std::string, rom, "", "Path to input ROM binary (.z64)");
ABSL_FLAG(std::string, symbols, "", "Path to symbols file");
ABSL_FLAG(std::string, out_dir, "build", "Output directory for carved assets and linker scripts");
ABSL_FLAG(std::string, asm_out_dir, "", "Output directory for disassembled assembly");
ABSL_FLAG(std::string, ld_script_path, "", "Override output path for primary linker script");
ABSL_FLAG(std::string, symbols_ld_path, "", "Override output path for symbols linker script");
ABSL_FLAG(bool, write_ld_script, true, "Generate and write GNU ld linker scripts");
ABSL_FLAG(bool, slice_data, true, "Carve non-code binary assets (.bin, header, raw data)");
ABSL_FLAG(bool, disassemble_code, true, "Disassemble code sections into .s assembly files");
ABSL_FLAG(bool, verify_sha1, true, "Verify ROM SHA-1 matches split configuration");
ABSL_FLAG(bool, emit_line_comments, true, "Emit instruction VRAM and hex comments in assembly");
ABSL_FLAG(bool, verbose, false, "Print verbose progress messages");

int main(int argc, char* argv[]) {
  absl::SetProgramUsageMessage("Usage: splitter --config=<path_to_config> [options]");
  absl::ParseCommandLine(argc, argv);

  std::string config_path = absl::GetFlag(FLAGS_config);
  if (config_path.empty()) {
    std::cerr << "Error: --config is required.\n" << absl::ProgramUsageMessage() << std::endl;
    return EXIT_FAILURE;
  }

  rom_nom_nom::SplitterOptions options;
  options.config_path = config_path;
  options.rom_path = absl::GetFlag(FLAGS_rom);
  options.symbols_path = absl::GetFlag(FLAGS_symbols);
  options.out_dir = absl::GetFlag(FLAGS_out_dir);
  options.asm_out_dir = absl::GetFlag(FLAGS_asm_out_dir);
  options.ld_script_path = absl::GetFlag(FLAGS_ld_script_path);
  options.symbols_ld_path = absl::GetFlag(FLAGS_symbols_ld_path);
  options.write_ld_script = absl::GetFlag(FLAGS_write_ld_script);
  options.slice_data = absl::GetFlag(FLAGS_slice_data);
  options.disassemble_code = absl::GetFlag(FLAGS_disassemble_code);
  options.verify_sha1 = absl::GetFlag(FLAGS_verify_sha1);
  options.emit_line_comments = absl::GetFlag(FLAGS_emit_line_comments);
  options.verbose = absl::GetFlag(FLAGS_verbose);

  auto result_or = rom_nom_nom::RunSplitter(options);
  if (!result_or.ok()) {
    std::cerr << "Error: " << result_or.status().message() << std::endl;
    return EXIT_FAILURE;
  }

  const auto& result = *result_or;
  std::cout << absl::StrFormat(
      "Splitter completed successfully:\n"
      "  Bytes carved: %d\n"
      "  Files written: %d\n"
      "  Functions disassembled: %d\n"
      "  Assembly files written: %d\n"
      "  Linker script generated: %s\n",
      result.bytes_carved, result.files_written, result.functions_disassembled,
      result.asm_files_written, result.ld_script_written ? "yes" : "no");

  return EXIT_SUCCESS;
}
