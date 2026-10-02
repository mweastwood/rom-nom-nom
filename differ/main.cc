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
#include "differ/differ_pipeline.h"

ABSL_FLAG(std::string, game, "harvest-moon-64", "Target game identifier.");
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
      "Assembly difference tool for N64 decompilation.\n"
      "Usage: differ <function_name> [--game=<game>] [--watch] [--no-color]");

  std::vector<char*> remaining_args = absl::ParseCommandLine(argc, argv);
  if (remaining_args.size() < 2) {
    std::cerr << "Error: Target function name required.\n"
              << "Example: bazel run //:diff -- MessageInit\n"
              << "         bazel run //:diff -- InterpolateInit --watch\n";
    return 1;
  }

  std::string func_name = remaining_args[1];
  std::string game_name = absl::GetFlag(FLAGS_game);
  bool watch = absl::GetFlag(FLAGS_watch);
  bool color = absl::GetFlag(FLAGS_color) && isatty(STDOUT_FILENO);
  int col_width = absl::GetFlag(FLAGS_column_width);

  rom_nom_nom::DifferPipelineOptions pipeline_opts;
  pipeline_opts.game_name = game_name;
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
