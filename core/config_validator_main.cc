#include <filesystem>
#include <iostream>
#include <string>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "absl/strings/str_format.h"
#include "core/game_config.h"
#include "core/game_config_rom_validator.h"
#include "core/rom.h"

ABSL_FLAG(std::string, config, "", "Path to the GameConfig .textproto file.");
ABSL_FLAG(std::string, rom, "", "Path to the retail .z64 ROM binary.");

int main(int argc, char* argv[]) {
  absl::SetProgramUsageMessage(
      "Validates a GameConfig .textproto against deterministic binary signatures in its retail "
      "N64 ROM.");
  absl::ParseCommandLine(argc, argv);

  const std::string config_path = absl::GetFlag(FLAGS_config);
  const std::string rom_path = absl::GetFlag(FLAGS_rom);
  if (config_path.empty()) {
    std::cerr << "Error: --config is required.\n";
    return 1;
  }
  if (rom_path.empty()) {
    std::cerr << "Error: --rom is required.\n";
    return 1;
  }

  auto config_or = rom_nom_nom::LoadGameConfig(config_path);
  if (!config_or.ok()) {
    std::cerr << "Failed to load GameConfig '" << config_path
              << "': " << config_or.status().message() << "\n";
    return 1;
  }

  auto rom_or = rom_nom_nom::Rom::OpenFile(rom_path);
  if (!rom_or.ok()) {
    std::cerr << "Failed to open ROM '" << rom_path << "': " << rom_or.status().message() << "\n";
    return 1;
  }

  rom_nom_nom::ConfigValidationReport report =
      rom_nom_nom::AnalyzeGameConfigAgainstRom(*config_or, *rom_or);
  if (!report.Ok()) {
    absl::Status status = rom_nom_nom::ValidateGameConfigAgainstRom(*config_or, *rom_or);
    std::cerr << status.message();
    return 1;
  }

  std::cout << absl::StrFormat(
      "OK: GameConfig '%s' validated against '%s' (%zu code segment(s), %zu code subsegment(s), "
      "%zu function(s) verified, 0 violations).\n",
      config_or->game_name(), std::filesystem::path(rom_path).filename().string(),
      report.code_segments_checked, report.code_subsegments_checked, report.functions_checked);
  return 0;
}
