#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "differ/differ_pipeline.h"
#include "differ/rom_differ.h"

ABSL_FLAG(std::string, game, "harvest-moon-64", "Target game identifier.");
ABSL_FLAG(std::string, source, "", "Explicit path to C source file containing function.");
ABSL_FLAG(std::string, config, "", "Explicit path to split config textproto.");
ABSL_FLAG(std::string, symbols, "", "Explicit path to symbol table textproto or txt.");
ABSL_FLAG(std::string, rom, "", "Explicit path to target retail ROM binary.");
ABSL_FLAG(std::string, built_rom, "",
          "Explicit path to built comparison ROM binary (enables ROM diff mode).");
ABSL_FLAG(bool, watch, false, "Watch source file and automatically re-diff on save.");
ABSL_FLAG(bool, color, true, "Enable ANSI color syntax highlighting.");
ABSL_FLAG(int, column_width, 48, "Character width for each side-by-side assembly column.");

namespace {

volatile std::sig_atomic_t g_stop_requested = 0;

void SignalHandler(int signum) {
  g_stop_requested = 1;
}

}  // namespace

int main(int argc, char* argv[]) {
  absl::SetProgramUsageMessage(
      "Assembly & ROM difference tool for N64 decompilation.\n"
      "Usage:\n"
      "  Function Diff (unlinked code vs ROM):\n"
      "    differ <function_name> [--source=<path>] [--game=<game>] [--watch] [--no-color]\n"
      "  ROM Diff (target ROM vs built ROM):\n"
      "    differ --built_rom=<path> [--rom=<path>] [--config=<path>] [--game=<game>] "
      "[--no-color]\n"
      "    differ <target_rom.z64> <built_rom.z64> [--config=<path>] [--no-color]");

  std::vector<char*> remaining_args = absl::ParseCommandLine(argc, argv);

  std::string game_name = absl::GetFlag(FLAGS_game);
  std::string source_path = absl::GetFlag(FLAGS_source);
  std::string config_path = absl::GetFlag(FLAGS_config);
  std::string symbols_path = absl::GetFlag(FLAGS_symbols);
  std::string target_rom_path = absl::GetFlag(FLAGS_rom);
  std::string built_rom_path = absl::GetFlag(FLAGS_built_rom);
  bool watch = absl::GetFlag(FLAGS_watch);
  bool color = absl::GetFlag(FLAGS_color) && isatty(STDOUT_FILENO);
  int col_width = absl::GetFlag(FLAGS_column_width);

  // Check if ROM diff mode is triggered via positional arguments (e.g. differ a.z64 b.z64)
  bool is_rom_mode = !built_rom_path.empty();
  if (!is_rom_mode && remaining_args.size() >= 3) {
    std::string_view first_arg = remaining_args[1];
    std::string_view second_arg = remaining_args[2];
    if (absl::EndsWith(first_arg, ".z64") && absl::EndsWith(second_arg, ".z64")) {
      is_rom_mode = true;
      if (target_rom_path.empty()) {
        target_rom_path = std::string(first_arg);
      }
      built_rom_path = std::string(second_arg);
    }
  }

  // --- ROM DIFF MODE ---
  if (is_rom_mode) {
    std::error_code error_code;
    if (target_rom_path.empty()) {
      std::filesystem::path default_rom =
          std::filesystem::path("roms") / absl::StrCat(game_name, ".z64");
      if (std::filesystem::exists(default_rom, error_code)) {
        target_rom_path = default_rom.string();
      }
    }
    if (config_path.empty()) {
      std::filesystem::path default_config =
          std::filesystem::path("config") / absl::StrCat(game_name, ".textproto");
      if (std::filesystem::exists(default_config, error_code)) {
        config_path = default_config.string();
      }
    }

    if (target_rom_path.empty()) {
      std::cerr << "Error: Target ROM binary required for ROM diff mode.\n"
                << "Pass --rom=<path> or ensure roms/" << game_name << ".z64 exists.\n";
      return 1;
    }

    rom_nom_nom::RomDiffer rom_differ(rom_nom_nom::RomDiffOptions{
        .target_rom_path = target_rom_path,
        .built_rom_path = built_rom_path,
        .config_path = config_path,
        .use_color = color,
    });

    auto result_or = rom_differ.Diff();
    if (!result_or.ok()) {
      std::cerr << "Error: " << result_or.status().message() << "\n";
      return 1;
    }

    std::cout << result_or->formatted_report << "\n";
    return result_or->success ? 0 : 1;
  }

  // --- FUNCTION DIFF MODE ---
  if (remaining_args.size() < 2) {
    std::cerr
        << "Error: Target function name required.\n"
        << "Example: bazel run //:diff -- --source=src/c/harvest-moon-64/math.c Abs16\n"
        << "         bazel run //:diff -- --source=src/c/harvest-moon-64/math.c Abs16 --watch\n"
        << "Or diff complete ROMs:\n"
        << "         bazel run //:diff -- --built_rom=bazel-bin/harvest-moon-64_lifted_c.z64\n";
    return 1;
  }

  std::string func_name = remaining_args[1];
  if (watch && source_path.empty()) {
    std::cerr << "Error: --watch requires an explicit --source=<path> to monitor.\n";
    return 1;
  }

  rom_nom_nom::DifferPipelineOptions pipeline_opts;
  pipeline_opts.game_name = game_name;
  if (!source_path.empty()) {
    pipeline_opts.source_file = source_path;
  }
  if (!config_path.empty()) {
    pipeline_opts.config_path = config_path;
  }
  if (!symbols_path.empty()) {
    pipeline_opts.symbols_path = symbols_path;
  }
  if (!target_rom_path.empty()) {
    pipeline_opts.rom_path = target_rom_path;
  }
  pipeline_opts.format_options.use_color = color;
  pipeline_opts.format_options.column_width = static_cast<size_t>(col_width);

  rom_nom_nom::DifferPipeline pipeline(std::move(pipeline_opts));

  if (!watch) {
    auto res_or = pipeline.Diff(func_name);
    if (!res_or.ok()) {
      std::cerr << "Error: " << res_or.status().message() << "\n";
      return 1;
    }
    std::cout << res_or->formatted_output << "\n";
    return res_or->is_bit_exact ? 0 : 1;
  }

  // Watch mode: register SIGINT handler and loop on source file mtime.
  std::signal(SIGINT, SignalHandler);

  std::cout << "Watching for changes to function '" << func_name << "'... (Press Ctrl+C to stop)\n";

  std::filesystem::file_time_type last_mtime;
  std::optional<std::filesystem::path> watched_file;
  bool first_run = true;

  while (!g_stop_requested) {
    auto res_or = pipeline.Diff(func_name);
    if (res_or.ok() && res_or->source_file.has_value()) {
      watched_file = res_or->source_file;
    }

    bool should_render = first_run;
    std::error_code ec;

    if (watched_file.has_value() && std::filesystem::exists(*watched_file, ec)) {
      auto current_mtime = std::filesystem::last_write_time(*watched_file, ec);
      if (!ec && (first_run || current_mtime != last_mtime)) {
        should_render = true;
        last_mtime = current_mtime;
      }
    }

    if (should_render) {
      // Clear screen and move cursor to top-left
      std::cout << "\033[H\033[J" << std::flush;
      if (res_or.ok()) {
        std::cout << res_or->formatted_output << "\n";
      } else {
        std::cerr << "Error: " << res_or.status().message() << "\n";
      }
      first_run = false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  std::cout << "\nStopped watcher.\n";
  return 0;
}
