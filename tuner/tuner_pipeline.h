#ifndef TUNER_TUNER_PIPELINE_H_
#define TUNER_TUNER_PIPELINE_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "core/c_ast.h"
#include "core/game_config.pb.h"
#include "core/target_extractor.h"
#include "lifter/lifter_pipeline.h"
#include "tuner/candidate_scorer.h"
#include "tuner/flag_explorer.h"
#include "tuner/tuning_cache_store.h"

namespace rom_nom_nom {

struct TunerPipelineOptions {
  std::filesystem::path repo_root = ".";
  std::string game_name = "harvest-moon-64";
  std::filesystem::path config_path;
  std::filesystem::path symbols_path;
  std::filesystem::path rom_path;
  std::filesystem::path asm_dir;
  std::filesystem::path lifted_dir;
  std::filesystem::path cache_path;
  std::filesystem::path cache_out_path;

  // Whether to run live compiler/AST search when a function is not in the tuning cache.
  bool search_on_miss = true;
  // When true, use cached entries for already bit-exact functions and re-run live search on
  // non-bit-exact functions.
  bool only_non_bit_exact = false;
  bool explore_flags = true;
  bool explore_ast_mutations = true;
  bool verify_with_fuzzer = true;
  bool format_with_clang = true;

  FlagExplorerOptions flag_options;
  size_t max_ast_candidates_per_round = 64;
  size_t max_rounds = 4;
  size_t num_threads = 0;
};

struct FunctionSearchOptions {
  bool explore_flags = true;
  bool explore_ast_mutations = true;
  FlagExplorerOptions flag_options;
  size_t max_ast_candidates_per_round = 64;
  size_t max_rounds = 4;
  size_t max_polish_rounds = 2;
  size_t max_polish_candidates = 512;
  std::vector<std::string> previous_cached_flags;
  std::vector<AstMutationStep> previous_cached_mutations;
};

struct FunctionSearchResult {
  CandidateScore baseline_score;
  CandidateScore best_score;
  std::vector<std::string> winning_c_flags;
  std::vector<AstMutationStep> applied_mutations;
  std::unique_ptr<FunctionDeclaration> best_ast;
  std::string best_c_code;
  bool improved_over_baseline = false;
};

struct FunctionTuneResult {
  std::string function_name;
  std::string module_name;
  uint32_t vram = 0;
  std::string input_sha1;
  bool from_cache = false;
  bool improved_over_baseline = false;

  CandidateScore baseline_score;
  CandidateScore best_score;

  std::vector<std::string> winning_c_flags;
  std::vector<AstMutationStep> applied_mutations;
  std::unique_ptr<FunctionDeclaration> tuned_ast;
  std::string tuned_c_code;
};

struct ModuleTuneResult {
  std::string module_name;
  size_t total_functions = 0;
  size_t bit_exact_functions = 0;
  size_t cache_hits = 0;
  size_t improved_functions = 0;
  std::vector<std::string> module_c_flags;
  std::string tuned_c_code;
  std::vector<FunctionTuneResult> function_results;
};

// Orchestrates compiler flag sweeping, AST mutation search, and O(1) cached recipe application
// to maximize bit-exactness of lifted C functions and modules.
class TunerPipeline {
 public:
  static absl::StatusOr<std::unique_ptr<TunerPipeline>> Create(TunerPipelineOptions options = {});

  TunerPipeline(TunerPipelineOptions options, std::unique_ptr<LifterPipeline> lifter,
                TuningCacheStore cache_store);

  // Executes the multi-phase compiler flag and AST mutation search loop for a single function.
  static absl::StatusOr<FunctionSearchResult> SearchFunctionCandidates(
      const FunctionDeclaration& baseline_ast, const std::vector<std::string>& base_flags,
      const FunctionSearchOptions& options,
      const std::function<std::string(const FunctionDeclaration&)>& emit_c_fn,
      const std::function<absl::StatusOr<CandidateScore>(
          std::string_view c_code, const std::vector<std::string>& flags)>& score_fn);

  // Tunes a single function given by name or VRAM address.
  absl::StatusOr<FunctionTuneResult> TuneFunction(std::string_view func_name_or_addr,
                                                  std::string_view module_name = "");

  // Tunes all functions in a module and returns the tuned translation unit C code.
  absl::StatusOr<ModuleTuneResult> TuneModule(std::string_view module_name);

  // Tunes all modules for a game (ingesting `options_.lifted_dir` if provided) and emits the
  // tuned C directory (including `types.h`, `lifted_symbols.ld`, and `tuned_c_flags.textproto`).
  absl::StatusOr<std::vector<std::filesystem::path>> TuneAllModules(
      const std::filesystem::path& output_dir);

  const TunerPipelineOptions& Options() const { return options_; }
  const TuningCacheStore& CacheStore() const { return cache_store_; }
  TuningCacheStore& MutableCacheStore() { return cache_store_; }

 private:
  absl::StatusOr<FunctionTuneResult> TuneLoadedFunctionAst(const TargetFunction& target,
                                                           const FunctionDeclaration& baseline_ast,
                                                           const CTranslationUnit* parent_tu,
                                                           std::string_view module_name,
                                                           bool emit_formatted_c = true);

  TunerPipelineOptions options_;
  std::unique_ptr<LifterPipeline> lifter_;
  TuningCacheStore cache_store_;
  mutable std::mutex cache_mutex_;
};

}  // namespace rom_nom_nom

#endif  // TUNER_TUNER_PIPELINE_H_
