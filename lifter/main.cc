#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "lifter/lifter_pipeline.h"

ABSL_FLAG(std::string, game, "harvest-moon-64", "Target game identifier.");
ABSL_FLAG(bool, format, true, "Format generated C code using clang-format.");
ABSL_FLAG(std::string, output, "", "Optional output file path (defaults to stdout).");

int main(int argc, char* argv[]) {
  absl::SetProgramUsageMessage(
      "Native clean-room MIPS-to-C lifter for N64 decompilation.\n"
      "Usage: lifter <function_name> [--game=<game>] [--noformat] "
      "[--output=<file>]");

  std::vector<char*> remaining_args = absl::ParseCommandLine(argc, argv);
  if (remaining_args.size() < 2) {
    std::cerr << "Error: Target function name or address required.\n"
              << "Example: bazel run //lifter:lifter -- InterpolateInit\n"
              << "         bazel run //:m2c -- InterpolateInit\n";
    return 1;
  }

  std::string func_name = remaining_args[1];
  std::string game_name = absl::GetFlag(FLAGS_game);
  bool format = absl::GetFlag(FLAGS_format);
  std::string output_path = absl::GetFlag(FLAGS_output);

  const char* workspace_dir = std::getenv("BUILD_WORKSPACE_DIRECTORY");
  std::filesystem::path repo_root = workspace_dir != nullptr ? workspace_dir : ".";

  rom_nom_nom::LifterPipelineOptions pipeline_opts;
  pipeline_opts.repo_root = repo_root;
  pipeline_opts.game_name = game_name;
  pipeline_opts.format_with_clang = format;

  auto pipeline_or = rom_nom_nom::LifterPipeline::Create(pipeline_opts);
  if (!pipeline_or.ok()) {
    std::cerr << "Lifter initialization failed: " << pipeline_or.status().message() << "\n";
    return 1;
  }

  auto result_or = (*pipeline_or)->Decompile(func_name);
  if (!result_or.ok()) {
    std::cerr << "Decompilation failed: " << result_or.status().message() << "\n";
    return 1;
  }

  if (!output_path.empty()) {
    std::ofstream out(output_path);
    if (!out.is_open()) {
      std::cerr << "Error opening output file: " << output_path << "\n";
      return 1;
    }
    out << result_or->c_code;
    std::cerr << "Wrote lifted C code to " << output_path << "\n";
  } else {
    std::cout << result_or->c_code;
  }

  return 0;
}
