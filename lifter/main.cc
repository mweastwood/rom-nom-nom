#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "absl/strings/str_split.h"
#include "lifter/lifter_pipeline.h"

ABSL_FLAG(std::string, game, "harvest-moon-64", "Target game identifier.");
ABSL_FLAG(bool, format, true, "Format generated C code using clang-format.");
ABSL_FLAG(std::string, output, "", "Optional output file path (defaults to stdout).");
ABSL_FLAG(std::string, output_dir, "", "Output directory for lifting all game modules.");
ABSL_FLAG(bool, all, false, "Decompile all modules in the game.");
ABSL_FLAG(std::string, config, "", "Optional path to split config textproto.");
ABSL_FLAG(std::string, symbols, "", "Optional path to symbols textproto.");
ABSL_FLAG(std::string, rom, "", "Optional path to ROM binary.");
ABSL_FLAG(std::vector<std::string>, function, {}, "Target function name(s) to lift.");
ABSL_FLAG(std::string, functions, "", "Comma-separated list of functions to lift.");
ABSL_FLAG(std::string, module, "", "Target module/subsegment to lift all functions from.");
ABSL_FLAG(std::vector<std::string>, include, {}, "Additional header #includes (e.g. types.h).");

int main(int argc, char* argv[]) {
  absl::SetProgramUsageMessage(
      "Native clean-room MIPS-to-C lifter for N64 decompilation.\n"
      "Usage: lifter [<function_name> ...] [--function=<func>] [--module=<module>] "
      "[--game=<game>] [--config=<file>] [--symbols=<file>] [--rom=<file>] "
      "[--noformat] [--output=<file>]");

  std::vector<char*> remaining_args = absl::ParseCommandLine(argc, argv);

  std::string game_name = absl::GetFlag(FLAGS_game);
  bool format = absl::GetFlag(FLAGS_format);
  std::string output_path = absl::GetFlag(FLAGS_output);
  std::string config_path = absl::GetFlag(FLAGS_config);
  std::string symbols_path = absl::GetFlag(FLAGS_symbols);
  std::string rom_path = absl::GetFlag(FLAGS_rom);
  std::string module_name = absl::GetFlag(FLAGS_module);
  std::vector<std::string> flag_funcs = absl::GetFlag(FLAGS_function);
  std::vector<std::string> extra_includes = absl::GetFlag(FLAGS_include);

  const char* workspace_dir = std::getenv("BUILD_WORKSPACE_DIRECTORY");
  std::filesystem::path repo_root = workspace_dir != nullptr ? workspace_dir : ".";

  rom_nom_nom::LifterPipelineOptions pipeline_opts;
  pipeline_opts.repo_root = repo_root;
  pipeline_opts.game_name = game_name;
  if (!config_path.empty()) {
    pipeline_opts.config_path = config_path;
  }
  if (!symbols_path.empty()) {
    pipeline_opts.symbols_path = symbols_path;
  }
  if (!rom_path.empty()) {
    pipeline_opts.rom_path = rom_path;
  }
  pipeline_opts.format_with_clang = format;

  if (!extra_includes.empty()) {
    pipeline_opts.includes = std::move(extra_includes);
  }

  auto pipeline_or = rom_nom_nom::LifterPipeline::Create(pipeline_opts);
  if (!pipeline_or.ok()) {
    std::cerr << "Lifter initialization failed: " << pipeline_or.status().message() << "\n";
    return 1;
  }

  std::vector<std::string> target_functions;
  for (size_t i = 1; i < remaining_args.size(); ++i) {
    target_functions.push_back(remaining_args[i]);
  }
  for (const auto& f : flag_funcs) {
    target_functions.push_back(f);
  }

  std::string functions_str = absl::GetFlag(FLAGS_functions);
  if (!functions_str.empty()) {
    std::vector<std::string_view> split = absl::StrSplit(functions_str, ',', absl::SkipEmpty());
    for (auto s : split) {
      target_functions.push_back(std::string(s));
    }
  }

  if (!module_name.empty()) {
    auto mod_funcs_or = (*pipeline_or)->GetFunctionsInModule(module_name);
    if (!mod_funcs_or.ok()) {
      std::cerr << "Error getting module functions: " << mod_funcs_or.status().message() << "\n";
      return 1;
    }
    target_functions.insert(target_functions.end(), mod_funcs_or->begin(), mod_funcs_or->end());
  }

  std::string output_dir = absl::GetFlag(FLAGS_output_dir);
  bool decompile_all = absl::GetFlag(FLAGS_all) ||
                       (!output_dir.empty() && target_functions.empty() && module_name.empty());

  if (decompile_all) {
    if (output_dir.empty()) {
      output_dir = "lifted/" + game_name;
    }
    auto files_or = (*pipeline_or)->DecompileAllModules(output_dir);
    if (!files_or.ok()) {
      std::cerr << "Failed to decompile game modules: " << files_or.status().message() << "\n";
      return 1;
    }
    std::cerr << "Successfully lifted " << files_or->size() << " module(s) to " << output_dir
              << "\n";
    return 0;
  }

  if (target_functions.empty()) {
    std::cerr << "Error: Target function name, --module, or --output_dir required.\n"
              << "Example: bazel run //lifter:lifter -- InterpolateInit\n"
              << "         bazel run //lifter:lifter -- --module=boot\n"
              << "         bazel run //lifter:lifter -- --output_dir=lifted/harvest-moon-64\n";
    return 1;
  }

  std::string c_code;
  if (target_functions.size() == 1) {
    auto result_or = (*pipeline_or)->Decompile(target_functions[0]);
    if (!result_or.ok()) {
      std::cerr << "Decompilation failed: " << result_or.status().message() << "\n";
      return 1;
    }
    c_code = std::move(result_or->c_code);
  } else {
    auto result_or = (*pipeline_or)->DecompileFunctions(target_functions);
    if (!result_or.ok()) {
      std::cerr << "Batch decompilation failed: " << result_or.status().message() << "\n";
      return 1;
    }
    c_code = std::move(*result_or);
  }

  if (!output_path.empty()) {
    std::ofstream out(output_path);
    if (!out.is_open()) {
      std::cerr << "Error opening output file: " << output_path << "\n";
      return 1;
    }
    out << c_code;
    std::cerr << "Wrote lifted C code (" << target_functions.size() << " function(s)) to "
              << output_path << "\n";
  } else {
    std::cout << c_code;
  }

  return 0;
}
