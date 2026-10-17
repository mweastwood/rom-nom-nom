#include "tuner/tuner_pipeline.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "core/c_ast.h"
#include "core/game_config.h"
#include "lifter/c_emitter.h"
#include "tuner/ast_mutator.h"
#include "tuner/candidate_scorer.h"
#include "tuner/flag_explorer.h"

namespace rom_nom_nom {

namespace {

std::string EmitSingleFunctionWithExterns(const FunctionDeclaration& func,
                                          const CTranslationUnit* parent_tu,
                                          const SymbolIndex* symbols, bool format_with_clang) {
  CTranslationUnit single_tu;
  if (parent_tu != nullptr) {
    single_tu.includes = parent_tu->includes;
    single_tu.symbol_index = parent_tu->symbol_index;
    single_tu.header_provided_functions = parent_tu->header_provided_functions;
  } else {
    single_tu.includes = {"types.h"};
    single_tu.symbol_index = symbols;
  }
  single_tu.functions.push_back(func.Clone());

  CEmitterOptions emitter_opts;
  emitter_opts.includes = single_tu.includes;
  emitter_opts.format_with_clang = format_with_clang;
  return CEmitter::EmitTranslationUnit(single_tu, emitter_opts);
}

std::string WriteTypesHeaderContent() {
  return R"(#ifndef TYPES_H
#define TYPES_H

typedef signed char s8;
typedef unsigned char u8;
typedef signed short s16;
typedef unsigned short u16;
typedef signed int s32;
typedef unsigned int u32;
typedef signed long long s64;
typedef unsigned long long u64;

typedef float f32;
typedef double f64;

#if defined(__GNUC__) && (defined(__mips__) || defined(__mips))
static inline f32 N64Sqrtf(f32 x) {
  f32 res;
  __asm__("sqrt.s %0, %1" : "=f"(res) : "f"(x));
  return res;
}
static inline f64 N64Sqrt(f64 x) {
  f64 res;
  __asm__("sqrt.d %0, %1" : "=f"(res) : "f"(x));
  return res;
}
#else
#include <math.h>
static inline f32 N64Sqrtf(f32 x) { return sqrtf(x); }
static inline f64 N64Sqrt(f64 x) { return sqrt(x); }
#endif

#endif  // TYPES_H
)";
}

}  // namespace

TunerPipeline::TunerPipeline(TunerPipelineOptions options, std::unique_ptr<LifterPipeline> lifter,
                             TuningCacheStore cache_store)
    : options_(std::move(options)),
      lifter_(std::move(lifter)),
      cache_store_(std::move(cache_store)) {}

absl::StatusOr<std::unique_ptr<TunerPipeline>> TunerPipeline::Create(TunerPipelineOptions options) {
  LifterPipelineOptions lifter_opts;
  lifter_opts.repo_root = options.repo_root;
  lifter_opts.game_name = options.game_name;
  lifter_opts.config_path = options.config_path;
  lifter_opts.symbols_path = options.symbols_path;
  lifter_opts.rom_path = options.rom_path;
  lifter_opts.asm_dir = options.asm_dir;
  lifter_opts.format_with_clang = options.format_with_clang;

  auto lifter_or = LifterPipeline::Create(lifter_opts);
  if (!lifter_or.ok()) {
    return lifter_or.status();
  }
  auto lifter = std::move(*lifter_or);

  TuningCacheStore cache_store;
  std::error_code ec;
  if (!options.cache_path.empty()) {
    if (std::filesystem::exists(options.cache_path, ec)) {
      auto loaded_or = TuningCacheStore::LoadFromFile(options.cache_path);
      if (loaded_or.ok()) {
        cache_store = std::move(*loaded_or);
      }
    }
  } else if (lifter->Loader() != nullptr && lifter->Loader()->Extractor() != nullptr &&
             lifter->Loader()->Extractor()->Config() != nullptr) {
    cache_store = TuningCacheStore::FromGameConfig(*lifter->Loader()->Extractor()->Config(),
                                                   options.repo_root);
  }

  return std::make_unique<TunerPipeline>(std::move(options), std::move(lifter),
                                         std::move(cache_store));
}

absl::StatusOr<FunctionTuneResult> TunerPipeline::TuneFunction(std::string_view func_name_or_addr,
                                                               std::string_view module_name) {
  if (lifter_ == nullptr || lifter_->Loader() == nullptr ||
      lifter_->Loader()->Extractor() == nullptr) {
    return absl::FailedPreconditionError("TunerPipeline: LifterPipeline is not initialized.");
  }

  const auto* extractor = lifter_->Loader()->Extractor();
  auto target_or = extractor->ExtractTarget(func_name_or_addr);
  if (!target_or.ok()) {
    return target_or.status();
  }

  auto tu_or = lifter_->BuildTranslationUnit({target_or->name});
  if (!tu_or.ok() || tu_or->functions.empty()) {
    return !tu_or.ok() ? tu_or.status()
                       : absl::InternalError("TunerPipeline: Empty translation unit.");
  }

  auto res_or = TuneLoadedFunctionAst(*target_or, tu_or->functions[0], &(*tu_or), module_name);
  if (!res_or.ok()) {
    return res_or.status();
  }

  if (!options_.cache_out_path.empty()) {
    (void)cache_store_.SaveToFile(options_.cache_out_path, options_.game_name);
  }
  return res_or;
}

absl::StatusOr<FunctionTuneResult> TunerPipeline::TuneLoadedFunctionAst(
    const TargetFunction& target, const FunctionDeclaration& baseline_ast,
    const CTranslationUnit* parent_tu, std::string_view module_name, bool emit_formatted_c) {
  const auto* extractor = (lifter_ != nullptr && lifter_->Loader() != nullptr)
                              ? lifter_->Loader()->Extractor()
                              : nullptr;
  const auto* config = extractor != nullptr ? extractor->Config() : nullptr;
  const auto* symbols = extractor != nullptr ? extractor->Symbols() : nullptr;

  std::string raw_baseline_body = baseline_ast.ToString();
  std::string input_sha1 = ComputeFunctionTuningHash(target.raw_words, raw_baseline_body);

  std::vector<std::string> base_flags;
  if (config != nullptr) {
    base_flags = ResolveCFlags(*config, module_name.empty() ? "default" : module_name);
  }
  if (base_flags.empty()) {
    base_flags = {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"};
  }

  FunctionTuneResult result;
  result.function_name = target.name;
  result.module_name = std::string(module_name);
  result.vram = target.vram;
  result.input_sha1 = input_sha1;
  result.winning_c_flags = base_flags;

  // 1. Check O(1) TuningCacheStore
  std::vector<AstMutationStep> previous_cached_mutations;
  std::vector<std::string> previous_cached_flags;
  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    if (!module_name.empty()) {
      auto module_flags = cache_store_.GetModuleCFlags(module_name);
      if (!module_flags.empty()) {
        base_flags = std::move(module_flags);
        result.winning_c_flags = base_flags;
      }
    }
    if (const auto* cached = cache_store_.Lookup(target.name, input_sha1); cached != nullptr) {
      if (!options_.only_non_bit_exact || cached->is_bit_exact()) {
        result.from_cache = true;
        if (!cached->c_flags().empty()) {
          result.winning_c_flags.assign(cached->c_flags().begin(), cached->c_flags().end());
        }
        result.applied_mutations.assign(cached->ast_mutations().begin(),
                                        cached->ast_mutations().end());
        FunctionDeclaration mutated =
            AstMutator::ApplyMutations(baseline_ast, result.applied_mutations);
        result.best_score.raw_bit_exact = cached->is_bit_exact();
        result.best_score.normalized_exact = cached->is_bit_exact();
        result.best_score.bit_exact_instructions = cached->bit_exact_instructions();
        result.best_score.matching_instructions = cached->matching_instructions();
        result.best_score.target_instruction_count = cached->target_instructions();
        result.baseline_score = result.best_score;
        result.improved_over_baseline = !result.applied_mutations.empty();
        if (emit_formatted_c) {
          result.tuned_c_code = EmitSingleFunctionWithExterns(mutated, parent_tu, symbols,
                                                              options_.format_with_clang);
        }
        result.tuned_ast = std::make_unique<FunctionDeclaration>(std::move(mutated));
        return result;
      }
      previous_cached_mutations.assign(cached->ast_mutations().begin(),
                                       cached->ast_mutations().end());
      previous_cached_flags.assign(cached->c_flags().begin(), cached->c_flags().end());
    }
  }

  // 2. Cache miss: if live search is disabled, return baseline in O(1)
  if (!options_.search_on_miss || extractor == nullptr) {
    result.tuned_ast = std::make_unique<FunctionDeclaration>(baseline_ast.Clone());
    if (emit_formatted_c) {
      result.tuned_c_code = EmitSingleFunctionWithExterns(*result.tuned_ast, parent_tu, symbols,
                                                          options_.format_with_clang);
    }
    return result;
  }

  // Prepare temporary include directory containing types.h so standalone function compiles cleanly
  static std::atomic<uint64_t> include_counter{0};
  uint64_t include_id = include_counter.fetch_add(1, std::memory_order_relaxed);
  std::filesystem::path temporary_include_dir =
      std::filesystem::temp_directory_path() /
      absl::StrCat("tuner_inc_", target.name, "_", include_id);
  std::error_code error_code;
  std::filesystem::create_directories(temporary_include_dir, error_code);
  {
    std::ofstream types_out(temporary_include_dir / "types.h");
    types_out << WriteTypesHeaderContent();
  }
  std::vector<std::filesystem::path> extra_include_dirs = {temporary_include_dir};
  if (!options_.lifted_dir.empty()) {
    extra_include_dirs.push_back(options_.lifted_dir);
  }

  CandidateScorer scorer(extractor, symbols);
  std::string baseline_compilable_c =
      EmitSingleFunctionWithExterns(baseline_ast, parent_tu, symbols, /*format_with_clang=*/false);

  absl::flat_hash_set<std::string> evaluated_candidates;
  auto make_evaluation_key = [](const std::vector<std::string>& flags, std::string_view code) {
    return absl::StrCat(absl::StrJoin(flags, " "), "\n", code);
  };
  evaluated_candidates.insert(make_evaluation_key(base_flags, baseline_compilable_c));

  auto base_score_or =
      scorer.CompileAndScore(target, baseline_compilable_c, base_flags, extra_include_dirs);
  if (!base_score_or.ok()) {
    std::filesystem::remove_all(temporary_include_dir, error_code);
    result.tuned_ast = std::make_unique<FunctionDeclaration>(baseline_ast.Clone());
    if (emit_formatted_c) {
      result.tuned_c_code = EmitSingleFunctionWithExterns(*result.tuned_ast, parent_tu, symbols,
                                                          options_.format_with_clang);
    }
    return result;
  }

  result.baseline_score = *base_score_or;
  result.best_score = *base_score_or;
  FunctionDeclaration best_ast = baseline_ast.Clone();

  std::vector<std::vector<std::string>> flag_profiles = {base_flags};
  if (options_.explore_flags) {
    flag_profiles = FlagExplorer::GenerateCandidateFlags(base_flags, options_.flag_options);
  }
  if (!previous_cached_flags.empty() && std::find(flag_profiles.begin(), flag_profiles.end(),
                                                  previous_cached_flags) == flag_profiles.end()) {
    flag_profiles.push_back(previous_cached_flags);
  }

  // Phase 1a: Compiler Flag Sweep on Baseline AST first (immediately resolves functions that only
  // need `-Wa,-O2` and ensures Phase 1b/Phase 2 run with the best flag profile).
  if (!result.best_score.raw_bit_exact && options_.explore_flags) {
    for (size_t i = 1; i < flag_profiles.size(); ++i) {
      if (evaluated_candidates.insert(make_evaluation_key(flag_profiles[i], baseline_compilable_c))
              .second) {
        auto candidate_score_or = scorer.CompileAndScore(target, baseline_compilable_c,
                                                         flag_profiles[i], extra_include_dirs);
        if (candidate_score_or.ok() && candidate_score_or->IsBetterThan(result.best_score)) {
          result.best_score = *candidate_score_or;
          result.winning_c_flags = flag_profiles[i];
          result.improved_over_baseline = true;
          if (result.best_score.raw_bit_exact) break;
        }
      }
    }
  }

  // Phase 1b: Canonical AST Preset Sweep on winning_c_flags
  bool improved_in_phase1b = false;
  if (!result.best_score.raw_bit_exact && options_.explore_ast_mutations) {
    auto presets = AstMutator::GenerateCanonicalPresets(baseline_ast);
    if (!previous_cached_mutations.empty()) {
      presets.insert(presets.begin(), previous_cached_mutations);
    }
    for (const auto& preset_steps : presets) {
      FunctionDeclaration preset_ast = AstMutator::ApplyMutations(baseline_ast, preset_steps);
      std::string preset_c = EmitSingleFunctionWithExterns(preset_ast, parent_tu, symbols,
                                                           /*format_with_clang=*/false);
      if (!evaluated_candidates.insert(make_evaluation_key(result.winning_c_flags, preset_c))
               .second) {
        continue;
      }
      auto candidate_score_or =
          scorer.CompileAndScore(target, preset_c, result.winning_c_flags, extra_include_dirs);
      if (!candidate_score_or.ok()) continue;
      if (candidate_score_or->IsBetterThan(result.best_score)) {
        result.best_score = *candidate_score_or;
        best_ast = std::move(preset_ast);
        result.applied_mutations = preset_steps;
        result.improved_over_baseline = true;
        improved_in_phase1b = true;
        if (result.best_score.raw_bit_exact) break;
      }
    }
  }

  // Phase 2: Greedy AST Mutation Search
  bool improved_in_phase2 = false;
  if (!result.best_score.raw_bit_exact && options_.explore_ast_mutations) {
    for (size_t round = 0; round < options_.max_rounds && !result.best_score.raw_bit_exact;
         ++round) {
      auto candidate_steps =
          AstMutator::DiscoverMutations(best_ast, options_.max_ast_candidates_per_round);
      bool improved_in_round = false;
      for (const auto& step : candidate_steps) {
        FunctionDeclaration mutated = AstMutator::ApplyMutation(best_ast, step);
        std::string mutated_c =
            EmitSingleFunctionWithExterns(mutated, parent_tu, symbols, /*format_with_clang=*/false);
        if (!evaluated_candidates.insert(make_evaluation_key(result.winning_c_flags, mutated_c))
                 .second) {
          continue;
        }
        auto candidate_score_or =
            scorer.CompileAndScore(target, mutated_c, result.winning_c_flags, extra_include_dirs);
        if (!candidate_score_or.ok()) continue;
        if (candidate_score_or->IsBetterThan(result.best_score)) {
          result.best_score = *candidate_score_or;
          best_ast = std::move(mutated);
          result.applied_mutations.push_back(step);
          result.improved_over_baseline = true;
          improved_in_round = true;
          improved_in_phase2 = true;
          if (result.best_score.raw_bit_exact) break;
        }
      }
      if (!improved_in_round) break;
    }
  }

  // Phase 3: Post-AST Compiler Flag Re-Sweep if Phase 1b or Phase 2 mutated the AST
  if (!result.best_score.raw_bit_exact && options_.explore_flags &&
      (improved_in_phase1b || improved_in_phase2)) {
    std::string best_ast_c =
        EmitSingleFunctionWithExterns(best_ast, parent_tu, symbols, /*format_with_clang=*/false);
    for (const auto& flags : flag_profiles) {
      if (flags == result.winning_c_flags) continue;
      if (!evaluated_candidates.insert(make_evaluation_key(flags, best_ast_c)).second) {
        continue;
      }
      auto candidate_score_or =
          scorer.CompileAndScore(target, best_ast_c, flags, extra_include_dirs);
      if (!candidate_score_or.ok()) continue;
      if (candidate_score_or->IsBetterThan(result.best_score)) {
        result.best_score = *candidate_score_or;
        result.winning_c_flags = flags;
        result.improved_over_baseline = true;
        if (result.best_score.raw_bit_exact) break;
      }
    }
  }

  std::filesystem::remove_all(temporary_include_dir, error_code);

  // Verify behavioral equivalence once on the final winning candidate (if not raw bit-exact)
  if (result.improved_over_baseline && !result.best_score.raw_bit_exact &&
      options_.verify_with_fuzzer) {
    if (!scorer.VerifyEquivalence(target, result.best_score.patched_words)) {
      result.best_score = result.baseline_score;
      best_ast = baseline_ast.Clone();
      result.applied_mutations.clear();
      result.winning_c_flags = base_flags;
      result.improved_over_baseline = false;
    }
  }

  // Record in TuningCacheStore
  FunctionTuningEntry entry;
  entry.set_function_name(target.name);
  entry.set_module_name(std::string(module_name));
  entry.set_input_sha1(input_sha1);
  for (const auto& flag : result.winning_c_flags) {
    entry.add_c_flags(flag);
  }
  for (const auto& step : result.applied_mutations) {
    *entry.add_ast_mutations() = step;
  }
  entry.set_bit_exact_instructions(static_cast<uint32_t>(result.best_score.bit_exact_instructions));
  entry.set_matching_instructions(static_cast<uint32_t>(result.best_score.matching_instructions));
  entry.set_target_instructions(static_cast<uint32_t>(result.best_score.target_instruction_count));
  entry.set_is_bit_exact(result.best_score.raw_bit_exact || result.best_score.normalized_exact);
  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    cache_store_.Upsert(std::move(entry));
  }

  if (emit_formatted_c) {
    result.tuned_c_code =
        EmitSingleFunctionWithExterns(best_ast, parent_tu, symbols, options_.format_with_clang);
  }
  result.tuned_ast = std::make_unique<FunctionDeclaration>(std::move(best_ast));
  return result;
}

absl::StatusOr<ModuleTuneResult> TunerPipeline::TuneModule(std::string_view module_name) {
  if (lifter_ == nullptr || lifter_->Loader() == nullptr ||
      lifter_->Loader()->Extractor() == nullptr) {
    return absl::FailedPreconditionError("TunerPipeline: LifterPipeline is not initialized.");
  }

  auto function_names_or = lifter_->GetFunctionsInModule(module_name);
  if (!function_names_or.ok()) {
    return function_names_or.status();
  }

  auto translation_unit_or = lifter_->BuildTranslationUnit(*function_names_or);
  if (!translation_unit_or.ok()) {
    return translation_unit_or.status();
  }

  const auto* extractor = lifter_->Loader()->Extractor();
  const auto* config = extractor->Config();
  const auto* symbols = extractor->Symbols();

  for (std::string_view intrinsic_name : {"N64Sqrtf", "N64Sqrt"}) {
    if (symbols == nullptr || symbols->FindByName(intrinsic_name) == nullptr) {
      translation_unit_or->header_provided_functions.insert(std::string(intrinsic_name));
    }
  }

  std::vector<std::string> default_config_flags;
  if (config != nullptr) {
    default_config_flags = ResolveCFlags(*config, module_name);
  }
  if (default_config_flags.empty()) {
    default_config_flags = {"-O2", "-mips2", "-mcpu=r4000", "-Wa,-g"};
  }

  ModuleTuneResult module_result;
  module_result.module_name = std::string(module_name);
  module_result.total_functions = translation_unit_or->functions.size();
  module_result.module_c_flags = cache_store_.GetModuleCFlags(module_name);
  if (module_result.module_c_flags.empty()) {
    module_result.module_c_flags = default_config_flags;
  }

  const size_t total_functions = translation_unit_or->functions.size();
  std::vector<std::optional<TargetFunction>> targets(total_functions);
  size_t active_functions = 0;
  for (size_t i = 0; i < total_functions; ++i) {
    const auto& function_declaration = translation_unit_or->functions[i];
    if (!options_.search_on_miss && !cache_store_.ContainsFunction(function_declaration.Name())) {
      continue;
    }
    auto target_or = extractor->ExtractTarget(function_declaration.Name());
    if (target_or.ok()) {
      targets[i] = std::move(*target_or);
      active_functions++;
    }
  }

  std::vector<std::optional<FunctionTuneResult>> slot_results(total_functions);
  const size_t hardware_threads =
      options_.num_threads > 0
          ? options_.num_threads
          : std::max<size_t>(1, static_cast<size_t>(std::thread::hardware_concurrency()));
  const size_t worker_count = std::min(hardware_threads, std::max<size_t>(1, active_functions));

  std::atomic<size_t> next_index{0};
  std::atomic<size_t> completed_live{0};

  auto worker_function = [&]() {
    while (true) {
      size_t index = next_index.fetch_add(1, std::memory_order_relaxed);
      if (index >= total_functions) break;
      if (!targets[index].has_value()) continue;

      auto function_result_or =
          TuneLoadedFunctionAst(*targets[index], translation_unit_or->functions[index],
                                &(*translation_unit_or), module_name, /*emit_formatted_c=*/false);
      if (!function_result_or.ok()) continue;

      bool was_live = !function_result_or->from_cache;
      slot_results[index] = std::move(*function_result_or);
      if (was_live && options_.search_on_miss && !options_.cache_out_path.empty()) {
        size_t done = completed_live.fetch_add(1, std::memory_order_relaxed) + 1;
        if (done % 25 == 0) {
          std::lock_guard<std::mutex> lock(cache_mutex_);
          (void)cache_store_.SaveToFile(options_.cache_out_path, options_.game_name);
          std::cerr << absl::StreamFormat("  [%s] Checkpointed %zu/%zu functions...\n", module_name,
                                          done, active_functions);
        }
      }
    }
  };

  if (worker_count <= 1) {
    worker_function();
  } else {
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (size_t t = 0; t < worker_count; ++t) {
      workers.emplace_back(worker_function);
    }
    for (auto& worker : workers) {
      worker.join();
    }
  }

  absl::flat_hash_map<std::string, size_t> bit_exact_flag_votes;
  absl::flat_hash_map<std::string, size_t> flag_votes;
  for (size_t i = 0; i < total_functions; ++i) {
    if (!slot_results[i].has_value()) continue;
    auto& function_result = *slot_results[i];
    if (function_result.from_cache) {
      module_result.cache_hits++;
    }
    if (function_result.improved_over_baseline) {
      module_result.improved_functions++;
    }
    const bool is_bit_exact =
        function_result.best_score.raw_bit_exact || function_result.best_score.normalized_exact;
    if (is_bit_exact) {
      module_result.bit_exact_functions++;
    }
    if (!function_result.winning_c_flags.empty()) {
      std::string key = absl::StrJoin(function_result.winning_c_flags, "\t");
      flag_votes[key]++;
      if (is_bit_exact) {
        bit_exact_flag_votes[key]++;
      }
    }
    if (function_result.tuned_ast != nullptr) {
      translation_unit_or->functions[i] = std::move(*function_result.tuned_ast);
    }
    module_result.function_results.push_back(std::move(function_result));
  }

  // Pick winning c_flags for the module: prefer the plurality winner among bit-exact functions,
  // falling back to overall majority vote when no functions are bit-exact yet.
  if (!bit_exact_flag_votes.empty()) {
    size_t best_votes = 0;
    for (const auto& function_result : module_result.function_results) {
      if (function_result.winning_c_flags.empty()) continue;
      std::string key = absl::StrJoin(function_result.winning_c_flags, "\t");
      auto it = bit_exact_flag_votes.find(key);
      if (it != bit_exact_flag_votes.end() && it->second > best_votes) {
        best_votes = it->second;
        module_result.module_c_flags = function_result.winning_c_flags;
      }
    }
    if (!module_result.module_c_flags.empty()) {
      cache_store_.SetModuleCFlags(module_name, module_result.module_c_flags);
    }
  } else if (!flag_votes.empty()) {
    size_t best_votes = 0;
    for (const auto& function_result : module_result.function_results) {
      if (function_result.winning_c_flags.empty()) continue;
      std::string key = absl::StrJoin(function_result.winning_c_flags, "\t");
      size_t count = flag_votes[key];
      if (count > best_votes) {
        best_votes = count;
        module_result.module_c_flags = function_result.winning_c_flags;
      }
    }
    if (!module_result.module_c_flags.empty()) {
      cache_store_.SetModuleCFlags(module_name, module_result.module_c_flags);
    }
  }

  CEmitterOptions emitter_options;
  emitter_options.includes = translation_unit_or->includes;
  emitter_options.format_with_clang = options_.format_with_clang;
  module_result.tuned_c_code = CEmitter::EmitTranslationUnit(*translation_unit_or, emitter_options);

  if (!options_.cache_out_path.empty()) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    (void)cache_store_.SaveToFile(options_.cache_out_path, options_.game_name);
  }
  return module_result;
}

absl::StatusOr<std::vector<std::filesystem::path>> TunerPipeline::TuneAllModules(
    const std::filesystem::path& output_dir) {
  std::error_code ec;
  std::filesystem::create_directories(output_dir, ec);

  std::vector<std::filesystem::path> emitted_files;

  // Fast path: when `lifted_dir` is provided and no search_on_miss is requested (or only cached
  // recipes are applied), start by copying all files from `lifted_dir` into `output_dir`, then
  // re-emit only the modules that have cached AST mutations (or all modules if search_on_miss).
  bool copied_from_lifted_dir = false;
  if (!options_.lifted_dir.empty() && std::filesystem::exists(options_.lifted_dir, ec)) {
    for (const auto& entry : std::filesystem::directory_iterator(options_.lifted_dir, ec)) {
      if (!entry.is_regular_file()) continue;
      std::filesystem::path dst = output_dir / entry.path().filename();
      std::filesystem::copy_file(entry.path(), dst,
                                 std::filesystem::copy_options::overwrite_existing, ec);
      emitted_files.push_back(dst);
    }
    copied_from_lifted_dir = true;
  }

  if (!copied_from_lifted_dir) {
    if (lifter_ == nullptr) {
      return absl::FailedPreconditionError("TunerPipeline: LifterPipeline is not initialized.");
    }
    auto baseline_files_or = lifter_->DecompileAllModules(output_dir);
    if (!baseline_files_or.ok()) {
      return baseline_files_or.status();
    }
    emitted_files = std::move(*baseline_files_or);
  }

  // Determine which modules need AST tuning
  absl::flat_hash_set<std::string> modules_with_cached_ast_mutations;
  const TuningCache cache_proto = cache_store_.ToProto();
  for (const auto& entry : cache_proto.entries()) {
    if (entry.ast_mutations_size() > 0 && !entry.module_name().empty()) {
      modules_with_cached_ast_mutations.insert(std::string(entry.module_name()));
    }
  }

  if (lifter_ != nullptr &&
      (options_.search_on_miss || !modules_with_cached_ast_mutations.empty())) {
    auto modules_or = lifter_->GetAllModules();
    if (modules_or.ok()) {
      for (size_t m = 0; m < modules_or->size(); ++m) {
        const auto& module_name = (*modules_or)[m];
        if (!options_.search_on_miss && !modules_with_cached_ast_mutations.contains(module_name)) {
          continue;
        }
        auto tuned_mod_or = TuneModule(module_name);
        if (!tuned_mod_or.ok()) {
          continue;
        }
        if (options_.search_on_miss) {
          std::cerr << absl::StreamFormat(
              "[%zu/%zu] Tuned module %s: %zu/%zu bit-exact (%zu cache hits, %zu improved)\n",
              m + 1, modules_or->size(), module_name, tuned_mod_or->bit_exact_functions,
              tuned_mod_or->total_functions, tuned_mod_or->cache_hits,
              tuned_mod_or->improved_functions);
        }
        std::filesystem::path out_c = output_dir / absl::StrCat(module_name, ".c");
        std::ofstream ofs(out_c);
        if (ofs.is_open()) {
          ofs << tuned_mod_or->tuned_c_code;
        }
      }
    }
  }

  // Always emit `tuned_c_flags.textproto` into `output_dir` so `RomBuilder` picks up module-level
  // compiler flag overrides.
  std::filesystem::path tuned_flags_path = output_dir / "tuned_c_flags.textproto";
  auto save_status = cache_store_.SaveToFile(tuned_flags_path, options_.game_name);
  if (save_status.ok()) {
    emitted_files.push_back(tuned_flags_path);
  }

  if (!options_.cache_out_path.empty()) {
    (void)cache_store_.SaveToFile(options_.cache_out_path, options_.game_name);
  }

  return emitted_files;
}

}  // namespace rom_nom_nom
