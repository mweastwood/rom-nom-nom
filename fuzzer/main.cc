#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "differ/differ_pipeline.h"
#include "differ/target_extractor.h"
#include "fuzzer/differential_fuzzer.h"
#include "fuzzer/equivalence_runner.h"
#include "splitter/config.h"
#include "splitter/symbol_registry.h"

ABSL_FLAG(std::string, game, "harvest-moon-64", "Target game identifier.");
ABSL_FLAG(std::string, config, "", "Path to split config textproto.");
ABSL_FLAG(std::string, symbols, "", "Path to symbol table txt or textproto.");
ABSL_FLAG(std::string, base_rom, "", "Path to base retail ROM binary.");
ABSL_FLAG(std::string, candidate_rom, "", "Path to candidate comparison ROM binary.");
ABSL_FLAG(std::string, source, "", "Path to C source file containing target function.");
ABSL_FLAG(std::string, function, "", "Target function name or VRAM address to fuzz.");
ABSL_FLAG(std::string, module, "", "Target module subsegment to evaluate.");
ABSL_FLAG(size_t, vectors, 100,
          "Number of random fuzz vectors to evaluate per nonmatching function.");
ABSL_FLAG(size_t, pointer_vectors, 20, "Number of pointer/buffer payload vectors per function.");
ABSL_FLAG(uint32_t, seed, 42, "PRNG seed for deterministic test vector generation.");
ABSL_FLAG(std::string, report, "", "Optional file path to output detailed markdown/text report.");
ABSL_FLAG(bool, color, true, "Enable ANSI color output.");
ABSL_FLAG(bool, verbose, false, "Print detailed information for all functions.");

namespace {

std::filesystem::path FindRepoRoot() {
  const char* build_working_dir = std::getenv("BUILD_WORKING_DIRECTORY");
  if (build_working_dir != nullptr && build_working_dir[0] != '\0') {
    return std::filesystem::path(build_working_dir);
  }
  std::filesystem::path cur = std::filesystem::current_path();
  std::error_code ec;
  while (!cur.empty()) {
    if (std::filesystem::exists(cur / "MODULE.bazel", ec) ||
        std::filesystem::exists(cur / "WORKSPACE", ec)) {
      return cur;
    }
    auto parent = cur.parent_path();
    if (parent == cur) break;
    cur = parent;
  }
  return std::filesystem::current_path();
}

std::vector<uint8_t> ReadBinaryFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file.is_open()) return {};
  std::streamsize size = file.tellg();
  file.seekg(0, std::ios::beg);
  std::vector<uint8_t> buffer(size);
  if (file.read(reinterpret_cast<char*>(buffer.data()), size)) {
    return buffer;
  }
  return {};
}

}  // namespace

int main(int argc, char* argv[]) {
  absl::SetProgramUsageMessage(
      "Differential Fuzzer & Semantic Equivalence Testing for N64 Decompilation.\n"
      "Usage:\n"
      "  Single function fuzzing against compiled C:\n"
      "    fuzzer <function_name> [--source=<path>] [--game=<game>]\n"
      "  Module / Whole-ROM equivalence evaluation:\n"
      "    fuzzer --candidate_rom=<path> [--base_rom=<path>] [--module=<module>] [--game=<game>]\n"
      "    fuzzer <base_rom.z64> <candidate_rom.z64>");

  std::vector<char*> remaining_args = absl::ParseCommandLine(argc, argv);

  std::string game_name = absl::GetFlag(FLAGS_game);
  std::string config_path = absl::GetFlag(FLAGS_config);
  std::string symbols_path = absl::GetFlag(FLAGS_symbols);
  std::string base_rom_path = absl::GetFlag(FLAGS_base_rom);
  std::string candidate_rom_path = absl::GetFlag(FLAGS_candidate_rom);
  std::string source_path = absl::GetFlag(FLAGS_source);
  std::string func_flag = absl::GetFlag(FLAGS_function);
  std::string module_flag = absl::GetFlag(FLAGS_module);
  size_t num_vectors = absl::GetFlag(FLAGS_vectors);
  size_t num_pointer_vectors = absl::GetFlag(FLAGS_pointer_vectors);
  uint32_t seed = absl::GetFlag(FLAGS_seed);
  std::string report_path = absl::GetFlag(FLAGS_report);
  bool color = absl::GetFlag(FLAGS_color) && isatty(STDOUT_FILENO);
  bool verbose = absl::GetFlag(FLAGS_verbose);

  std::filesystem::path repo_root = FindRepoRoot();

  // Positional arguments handling
  if (remaining_args.size() >= 3) {
    std::string_view arg1 = remaining_args[1];
    std::string_view arg2 = remaining_args[2];
    if (absl::EndsWith(arg1, ".z64") && absl::EndsWith(arg2, ".z64")) {
      base_rom_path = std::string(arg1);
      candidate_rom_path = std::string(arg2);
    }
  } else if (remaining_args.size() >= 2 && func_flag.empty()) {
    std::string_view arg1 = remaining_args[1];
    if (!absl::EndsWith(arg1, ".z64")) {
      func_flag = std::string(arg1);
    }
  }

  // Resolve default paths
  std::error_code ec;
  if (base_rom_path.empty()) {
    std::filesystem::path default_rom = repo_root / "roms" / absl::StrCat(game_name, ".z64");
    if (std::filesystem::exists(default_rom, ec)) {
      base_rom_path = default_rom.string();
    }
  }
  if (config_path.empty()) {
    std::filesystem::path default_config =
        repo_root / "config" / absl::StrCat(game_name, ".textproto");
    if (std::filesystem::exists(default_config, ec)) {
      config_path = default_config.string();
    }
  }
  if (symbols_path.empty()) {
    std::filesystem::path default_syms =
        repo_root / "symbols" / absl::StrCat(game_name, ".textproto");
    if (std::filesystem::exists(default_syms, ec)) {
      symbols_path = default_syms.string();
    }
  }

  if (!base_rom_path.empty() && std::filesystem::path(base_rom_path).is_relative()) {
    base_rom_path = (repo_root / base_rom_path).string();
  }
  if (!candidate_rom_path.empty() && std::filesystem::path(candidate_rom_path).is_relative()) {
    candidate_rom_path = (repo_root / candidate_rom_path).string();
  }
  if (!config_path.empty() && std::filesystem::path(config_path).is_relative()) {
    config_path = (repo_root / config_path).string();
  }
  if (!symbols_path.empty() && std::filesystem::path(symbols_path).is_relative()) {
    symbols_path = (repo_root / symbols_path).string();
  }

  if (!symbols_path.empty()) {
    std::filesystem::path sym_path(symbols_path);
    if (sym_path.extension() == ".txt") {
      std::filesystem::path candidate = sym_path;
      candidate.replace_extension(".textproto");
      if (std::filesystem::exists(candidate, ec)) {
        symbols_path = candidate.string();
      }
    }
  }

  // Set up equivalence runner options
  rom_nom_nom::fuzzer::EquivalenceRunnerOptions runner_opts;
  runner_opts.random_vectors_per_function = num_vectors;
  runner_opts.pointer_vectors_per_function = num_pointer_vectors;
  runner_opts.seed = seed;
  rom_nom_nom::fuzzer::EquivalenceRunner runner(runner_opts);

  // Initialize TargetExtractor
  auto extractor_or = rom_nom_nom::TargetExtractor::Create({
      .repo_root = repo_root,
      .game_name = game_name,
      .config_path = config_path,
      .symbols_path = symbols_path,
      .rom_path = base_rom_path,
  });
  if (!extractor_or.ok()) {
    std::cerr << "Error initializing TargetExtractor: " << extractor_or.status().message() << "\n";
    return 1;
  }
  auto extractor = std::move(*extractor_or);

  // Mode 1: Single function fuzzing via C source file or ROM
  if (!func_flag.empty()) {
    rom_nom_nom::fuzzer::FunctionEquivalenceTarget target;
    target.name = func_flag;

    auto target_func_or = extractor->ExtractTarget(func_flag);
    if (!target_func_or.ok()) {
      std::cerr << "Error extracting target function '" << func_flag
                << "': " << target_func_or.status().message() << "\n";
      return 1;
    }
    target.vram = target_func_or->vram;
    target.target_words = target_func_or->raw_words;

    // If source file given, compile via DifferPipeline
    if (!source_path.empty()) {
      rom_nom_nom::DifferPipelineOptions pipe_opts;
      pipe_opts.game_name = game_name;
      pipe_opts.source_file = source_path;
      pipe_opts.config_path = config_path;
      pipe_opts.symbols_path = symbols_path;
      pipe_opts.rom_path = base_rom_path;
      rom_nom_nom::DifferPipeline pipeline(pipe_opts);

      auto diff_res_or = pipeline.Diff(func_flag);
      if (!diff_res_or.ok()) {
        std::cerr << "Error compiling/diffing candidate source: " << diff_res_or.status().message()
                  << "\n";
        return 1;
      }
      for (const auto& row : diff_res_or->alignment.rows) {
        if (row.compiled.has_value()) {
          target.candidate_words.push_back(row.compiled->original.raw_word);
        }
      }
    } else if (!candidate_rom_path.empty()) {
      std::vector<uint8_t> candidate_bytes = ReadBinaryFile(candidate_rom_path);
      if (candidate_bytes.empty()) {
        std::cerr << "Error reading candidate ROM file: " << candidate_rom_path << "\n";
        return 1;
      }
      auto cand_or = extractor->ExtractFromRom(candidate_bytes, func_flag);
      if (!cand_or.ok()) {
        std::cerr << "Error extracting candidate function from candidate ROM: "
                  << cand_or.status().message() << "\n";
        return 1;
      }
      target.candidate_words = cand_or->raw_words;
    } else {
      std::cerr << "Error: Fuzzing function '" << func_flag
                << "' requires either --source=<path.c> or --candidate_rom=<path.z64>\n";
      return 1;
    }

    auto eval_res = runner.EvaluateFunction(target);
    std::cout << "\n=== Differential Fuzzing Result: " << func_flag << " ===\n";
    std::cout << "Target instruction count:    " << eval_res.target_instruction_count << "\n";
    std::cout << "Candidate instruction count: " << eval_res.candidate_instruction_count << "\n";
    std::cout << "Vectors evaluated:           " << eval_res.fuzz_vectors_evaluated << "\n";
    std::cout << "Status:                      ";
    switch (eval_res.status) {
      case rom_nom_nom::fuzzer::FunctionEquivalenceStatus::kExactBinaryMatch:
        std::cout << (color ? "\033[1;32mEXACT BINARY MATCH (100%)\033[0m"
                            : "EXACT BINARY MATCH (100%)")
                  << "\n";
        break;
      case rom_nom_nom::fuzzer::FunctionEquivalenceStatus::kFuzzProvenEquivalent:
        std::cout << (color ? "\033[1;36mFUZZ-PROVEN EQUIVALENT (100% Semantic Match)\033[0m"
                            : "FUZZ-PROVEN EQUIVALENT")
                  << "\n";
        break;
      case rom_nom_nom::fuzzer::FunctionEquivalenceStatus::kDivergent:
        std::cout << (color ? "\033[1;31mDIVERGENT (Behavioral Discrepancy)\033[0m" : "DIVERGENT")
                  << "\n";
        std::cout << "Details: " << eval_res.details << "\n";
        return 1;
      case rom_nom_nom::fuzzer::FunctionEquivalenceStatus::kCompilationOrExtractError:
        std::cout << (color ? "\033[1;31mERROR\033[0m" : "ERROR") << "\n";
        std::cout << "Details: " << eval_res.details << "\n";
        return 1;
    }
    return 0;
  }

  // Mode 2: Whole-ROM or Module Batch Equivalence Evaluation
  if (candidate_rom_path.empty()) {
    std::filesystem::path default_built = repo_root / "bazel-bin" / absl::StrCat(game_name, ".z64");
    if (std::filesystem::exists(default_built, ec)) {
      candidate_rom_path = default_built.string();
    } else {
      std::cerr << "Error: Evaluation mode requires --candidate_rom=<path.z64>\n";
      return 1;
    }
  }

  std::vector<uint8_t> target_bytes = ReadBinaryFile(base_rom_path);
  if (target_bytes.empty()) {
    std::cerr << "Error reading base retail ROM: " << base_rom_path << "\n";
    return 1;
  }
  std::vector<uint8_t> built_bytes = ReadBinaryFile(candidate_rom_path);
  if (built_bytes.empty()) {
    std::cerr << "Error reading built candidate ROM: " << candidate_rom_path << "\n";
    return 1;
  }

  std::vector<rom_nom_nom::fuzzer::FunctionEquivalenceTarget> targets;
  if (extractor->Symbols() != nullptr) {
    for (const auto* entry : extractor->Symbols()->SortedEntries()) {
      if (entry->type() == rom_nom_nom::SymbolType::SYMBOL_FUNC) {
        auto t_or = extractor->ExtractFromRom(target_bytes, entry->name());
        auto c_or = extractor->ExtractFromRom(built_bytes, entry->name());
        if (t_or.ok() && c_or.ok() && !t_or->raw_words.empty() && !c_or->raw_words.empty()) {
          rom_nom_nom::fuzzer::FunctionEquivalenceTarget t;
          t.name = entry->name();
          t.vram = entry->address();
          t.target_words = t_or->raw_words;
          t.candidate_words = c_or->raw_words;
          targets.push_back(std::move(t));
        }
      }
    }
  }

  std::string eval_name = module_flag.empty() ? game_name : module_flag;
  auto summary = runner.EvaluateModule(eval_name, targets);
  std::string report = rom_nom_nom::fuzzer::EquivalenceRunner::FormatReport(summary, verbose);
  std::cout << report << "\n";

  if (!report_path.empty()) {
    std::ofstream rfile(report_path);
    if (rfile.is_open()) {
      rfile << report;
    }
  }

  return (summary.divergent_functions == 0 && summary.error_functions == 0) ? 0 : 1;
}
