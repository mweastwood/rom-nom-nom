#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "core/toolchain.h"
#include "tuner/ast_mutator.h"
#include "tuner/tuner_pipeline.h"

ABSL_FLAG(std::string, game, "harvest-moon-64", "Target game identifier.");
ABSL_FLAG(std::string, config, "", "Optional path to game config textproto.");
ABSL_FLAG(std::string, rom, "", "Optional path to retail ROM binary.");
ABSL_FLAG(std::string, asm_dir, "", "Optional path to assembly directory.");
ABSL_FLAG(std::string, lifted_dir, "", "Optional path to pre-lifted C directory.");
ABSL_FLAG(std::string, cache, "", "Optional path to input tuning cache .textproto.");
ABSL_FLAG(std::string, cache_out, "", "Optional path to write updated tuning cache .textproto.");
ABSL_FLAG(std::string, function, "", "Target function name or VRAM address to tune.");
ABSL_FLAG(std::string, module, "", "Target module/subsegment to tune.");
ABSL_FLAG(std::string, output, "", "Optional output C file path.");
ABSL_FLAG(std::string, output_dir, "", "Output directory for tuning all game modules.");
ABSL_FLAG(bool, search_on_miss, true,
          "Run live compiler flag and AST mutation search on cache misses.");
ABSL_FLAG(bool, only_non_bit_exact, false,
          "Skip live search for cached bit-exact functions and only search non-bit-exact ones.");
ABSL_FLAG(bool, explore_flags, true, "Explore compiler/assembler flag profiles.");
ABSL_FLAG(bool, explore_ast, true, "Explore semantics-preserving AST mutations.");
ABSL_FLAG(bool, verify_fuzzer, true,
          "Verify candidate behavioral equivalence via differential fuzzer.");
ABSL_FLAG(bool, format, true, "Format generated C code using clang-format.");
ABSL_FLAG(size_t, max_rounds, 4, "Maximum greedy AST mutation rounds per function.");
ABSL_FLAG(size_t, max_candidates, 64, "Maximum AST mutation candidates per round.");
ABSL_FLAG(size_t, max_flag_profiles, 24, "Maximum compiler flag profiles to explore per function.");
ABSL_FLAG(size_t, threads, 0, "Number of worker threads (0 = hardware_concurrency).");
ABSL_FLAG(bool, all_modules, false, "Tune all modules in the game.");

int main(int argc, char* argv[]) {
  absl::SetProgramUsageMessage(
      "Automated bit-exactness search and compiler/AST tuning engine for lifted N64 C code.\n"
      "Usage:\n"
      "  Single function tuning:\n"
      "    tuner <function_name> [--game=<game>] [--cache_out=<path>]\n"
      "  Module tuning:\n"
      "    tuner --module=<module> [--game=<game>] [--cache_out=<path>]\n"
      "  All modules tuning:\n"
      "    tuner --all_modules [--game=<game>] [--cache_out=<path>]\n"
      "  Whole-game directory tuning:\n"
      "    tuner --lifted_dir=<dir> --output_dir=<dir> [--cache=<file>] [--nosearch_on_miss]");

  std::vector<char*> remaining_args = absl::ParseCommandLine(argc, argv);

  std::filesystem::path repo_root = rom_nom_nom::FindRepoRoot();

  rom_nom_nom::TunerPipelineOptions opts;
  opts.repo_root = repo_root;
  opts.game_name = absl::GetFlag(FLAGS_game);
  opts.config_path = absl::GetFlag(FLAGS_config);
  opts.rom_path = absl::GetFlag(FLAGS_rom);
  opts.asm_dir = absl::GetFlag(FLAGS_asm_dir);
  opts.lifted_dir = absl::GetFlag(FLAGS_lifted_dir);
  opts.cache_path = absl::GetFlag(FLAGS_cache);
  opts.cache_out_path = absl::GetFlag(FLAGS_cache_out);
  opts.search_on_miss = absl::GetFlag(FLAGS_search_on_miss);
  opts.only_non_bit_exact = absl::GetFlag(FLAGS_only_non_bit_exact);
  opts.explore_flags = absl::GetFlag(FLAGS_explore_flags);
  opts.explore_ast_mutations = absl::GetFlag(FLAGS_explore_ast);
  opts.verify_with_fuzzer = absl::GetFlag(FLAGS_verify_fuzzer);
  opts.format_with_clang = absl::GetFlag(FLAGS_format);
  opts.max_rounds = absl::GetFlag(FLAGS_max_rounds);
  opts.max_ast_candidates_per_round = absl::GetFlag(FLAGS_max_candidates);
  opts.flag_options.max_profiles = absl::GetFlag(FLAGS_max_flag_profiles);
  opts.num_threads = absl::GetFlag(FLAGS_threads);

  std::string func_name = absl::GetFlag(FLAGS_function);
  if (func_name.empty() && remaining_args.size() >= 2) {
    func_name = remaining_args[1];
  }
  std::string module_name = absl::GetFlag(FLAGS_module);
  std::string output_path = absl::GetFlag(FLAGS_output);
  std::string output_dir = absl::GetFlag(FLAGS_output_dir);
  bool all_modules = absl::GetFlag(FLAGS_all_modules);

  auto tuner_or = rom_nom_nom::TunerPipeline::Create(opts);
  if (!tuner_or.ok()) {
    std::cerr << "Tuner initialization failed: " << tuner_or.status().message() << "\n";
    return 1;
  }
  auto& tuner = *tuner_or;

  if (!output_dir.empty() || all_modules) {
    std::filesystem::path target_out_dir =
        !output_dir.empty()
            ? std::filesystem::path(output_dir)
            : (std::filesystem::temp_directory_path() / absl::StrCat("tuned_all_", opts.game_name));
    auto files_or = tuner->TuneAllModules(target_out_dir);
    if (!files_or.ok()) {
      std::cerr << "Failed to tune game modules: " << files_or.status().message() << "\n";
      return 1;
    }
    std::cerr << "Successfully tuned " << files_or->size() << " artifact(s) to " << target_out_dir
              << "\n";
    return 0;
  }

  if (!func_name.empty()) {
    auto res_or = tuner->TuneFunction(func_name, module_name);
    if (!res_or.ok()) {
      std::cerr << "Failed to tune function '" << func_name << "': " << res_or.status().message()
                << "\n";
      return 1;
    }
    const auto& res = *res_or;
    std::cerr << absl::StrFormat(
        "=== Tuner Result: %s (0x%08X) ===\n"
        "  Source:          %s\n"
        "  Input SHA-1:     %s\n"
        "  Winning Flags:   %s\n"
        "  Bit-Exact Match: %s (%zu / %zu instructions)\n",
        res.function_name, res.vram, res.from_cache ? "cache hit" : "live search", res.input_sha1,
        absl::StrJoin(res.winning_c_flags, " "),
        (res.best_score.raw_bit_exact || res.best_score.normalized_exact) ? "YES" : "NO",
        res.best_score.matching_instructions, res.best_score.target_instruction_count);
    for (const auto& step : res.applied_mutations) {
      std::cerr << "  Applied AST Mutation: " << rom_nom_nom::AstMutator::DescribeStep(step)
                << "\n";
    }
    if (!output_path.empty()) {
      std::ofstream out(output_path);
      out << res.tuned_c_code;
    } else {
      std::cout << res.tuned_c_code;
    }
    return 0;
  }

  if (!module_name.empty()) {
    auto mod_or = tuner->TuneModule(module_name);
    if (!mod_or.ok()) {
      std::cerr << "Failed to tune module '" << module_name << "': " << mod_or.status().message()
                << "\n";
      return 1;
    }
    const auto& mod = *mod_or;
    std::cerr << absl::StrFormat(
        "=== Module Tuner Summary: %s ===\n"
        "  Functions:     %zu\n"
        "  Bit-Exact:     %zu / %zu\n"
        "  Cache Hits:    %zu\n"
        "  Improved:      %zu\n"
        "  Module Flags:  %s\n",
        mod.module_name, mod.total_functions, mod.bit_exact_functions, mod.total_functions,
        mod.cache_hits, mod.improved_functions, absl::StrJoin(mod.module_c_flags, " "));
    if (!output_path.empty()) {
      std::ofstream out(output_path);
      out << mod.tuned_c_code;
    } else {
      std::cout << mod.tuned_c_code;
    }
    return 0;
  }

  std::cerr << "Error: Specify <function_name>, --module=<module>, or --output_dir=<dir>.\n";
  return 1;
}
