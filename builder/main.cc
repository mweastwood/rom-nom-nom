#include <cstdlib>
#include <iostream>
#include <string>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "absl/strings/str_format.h"
#include "builder/rom_builder.h"
#include "core/toolchain.h"

ABSL_FLAG(std::string, game, "", "Game name, e.g. harvest-moon-64");
ABSL_FLAG(std::string, config, "", "Path to split config (.textproto)");
ABSL_FLAG(std::string, symbols, "", "Path to symbols file");
ABSL_FLAG(std::string, asm_dir, "", "Generated assembly directory");
ABSL_FLAG(std::string, build_dir, "", "Generated build directory (with LD script)");
ABSL_FLAG(std::string, assets_dir, "", "Generated assets directory");
ABSL_FLAG(std::string, out_elf, "", "Output ELF file path");
ABSL_FLAG(std::string, out_rom, "", "Output ROM (.z64) file path");
ABSL_FLAG(std::string, toolchain, "original", "Toolchain to use (original or modern)");
ABSL_FLAG(std::string, verify_rom, "", "Original ROM to verify match against");
ABSL_FLAG(bool, test, false, "Fail with non-zero exit code if not byte-exact match");

int main(int argc, char* argv[]) {
  absl::SetProgramUsageMessage(
      "Usage: builder --game=<name> --config=<config> --symbols=<symbols> "
      "--asm-dir=<dir> --build-dir=<dir> --assets-dir=<dir> --out-elf=<elf> --out-rom=<rom> "
      "[options]");

  std::vector<std::string> normalized_args;
  std::vector<char*> normalized_argv;
  normalized_args.reserve(argc);
  normalized_argv.reserve(argc);

  for (int i = 0; i < argc; ++i) {
    std::string arg = argv[i];
    if (i > 0 && arg.rfind("--", 0) == 0) {
      size_t eq_pos = arg.find('=');
      if (eq_pos == std::string::npos) {
        std::replace(arg.begin() + 2, arg.end(), '-', '_');
      } else {
        std::replace(arg.begin() + 2, arg.begin() + eq_pos, '-', '_');
      }
    }
    normalized_args.push_back(std::move(arg));
  }
  for (auto& arg : normalized_args) {
    normalized_argv.push_back(arg.data());
  }

  int norm_argc = static_cast<int>(normalized_argv.size());
  char** norm_argv = normalized_argv.data();
  absl::ParseCommandLine(norm_argc, norm_argv);

  std::string game = absl::GetFlag(FLAGS_game);
  if (game.empty()) {
    std::cerr << "Error: --game is required.\n" << absl::ProgramUsageMessage() << std::endl;
    return EXIT_FAILURE;
  }

  std::string config_path = absl::GetFlag(FLAGS_config);
  if (config_path.empty()) {
    std::cerr << "Error: --config is required.\n" << absl::ProgramUsageMessage() << std::endl;
    return EXIT_FAILURE;
  }

  std::string out_rom = absl::GetFlag(FLAGS_out_rom);
  if (out_rom.empty()) {
    std::cerr << "Error: --out-rom is required.\n" << absl::ProgramUsageMessage() << std::endl;
    return EXIT_FAILURE;
  }

  std::string out_elf = absl::GetFlag(FLAGS_out_elf);
  if (out_elf.empty()) {
    std::cerr << "Error: --out-elf is required.\n" << absl::ProgramUsageMessage() << std::endl;
    return EXIT_FAILURE;
  }

  std::string toolchain_str = absl::GetFlag(FLAGS_toolchain);
  rom_nom_nom::ToolchainMode mode = (toolchain_str == "modern")
                                        ? rom_nom_nom::ToolchainMode::kModern
                                        : rom_nom_nom::ToolchainMode::kOriginal;

  rom_nom_nom::ToolchainOptions tc_opts;
  tc_opts.mode = mode;
  auto tc_or = rom_nom_nom::Toolchain::Discover(tc_opts);
  if (!tc_or.ok()) {
    std::cerr << "Error: Toolchain discovery failed: " << tc_or.status().message() << std::endl;
    return EXIT_FAILURE;
  }

  rom_nom_nom::RomBuilderOptions options;
  options.game_name = game;
  options.config_path = config_path;
  options.symbols_path = absl::GetFlag(FLAGS_symbols);
  options.asm_dir = absl::GetFlag(FLAGS_asm_dir);
  options.build_dir = absl::GetFlag(FLAGS_build_dir);
  options.assets_dir = absl::GetFlag(FLAGS_assets_dir);
  options.out_elf = out_elf;
  options.out_rom = out_rom;
  options.toolchain_mode = mode;
  options.verify_rom = absl::GetFlag(FLAGS_verify_rom);
  options.is_test = absl::GetFlag(FLAGS_test);

  std::cout << absl::StrFormat("=== Building %s [%s toolchain] ===\n", game, toolchain_str);

  rom_nom_nom::RomBuilder builder(*tc_or, options);
  auto result_or = builder.Build();
  if (!result_or.ok()) {
    std::cerr << "Error: Build failed: " << result_or.status().message() << std::endl;
    return EXIT_FAILURE;
  }

  const auto& result = *result_or;
  std::cout << absl::StrFormat("\nBuilt ROM: %s\n", out_rom);
  std::cout << absl::StrFormat("Built ROM SHA-1:    %s\n", result.built_sha1);
  if (!result.expected_sha1.empty()) {
    std::cout << absl::StrFormat("Expected ROM SHA-1: %s\n", result.expected_sha1);
  }

  if (mode == rom_nom_nom::ToolchainMode::kModern) {
    std::cout << "[SUCCESS] Modern ROM built successfully!\n";
    return EXIT_SUCCESS;
  }

  if (result.success) {
    std::cout << absl::StrFormat("\n*** [MATCH OK] %s matches 100%% byte-for-byte! ***\n\n", game);
    return EXIT_SUCCESS;
  } else {
    std::cout << absl::StrFormat(
        "\n*** [MATCH FAILED] %s does not match (%d differing bytes)! ***\n\n", game,
        result.differing_bytes);
    return options.is_test ? EXIT_FAILURE : EXIT_SUCCESS;
  }
}
