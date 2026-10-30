#include "tuner/tuner_pipeline.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
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
    single_tu.parent_module_context = parent_tu->parent_module_context;
    if (single_tu.parent_module_context == nullptr && parent_tu->functions.size() > 1) {
      single_tu.parent_module_context =
          std::make_shared<const ModuleSymbolContext>(AstCodegenLowerer::AnalyzeModuleSymbols(
              parent_tu->functions, parent_tu->symbol_index, parent_tu->header_provided_functions));
    }
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

bool IsHighYieldPolishMutation(AstMutationKind kind) {
  return kind == AST_MUTATION_SWAP_ADJACENT_STATEMENTS ||
         kind == AST_MUTATION_PROPAGATE_REGISTER_COPY || kind == AST_MUTATION_HOIST_WHILE_GUARD ||
         kind == AST_MUTATION_RELOAD_OR_EARLY_RETURN_VAR || kind == AST_MUTATION_INVERT_IF_ELSE ||
         kind == AST_MUTATION_COMMUTE_BINARY_OP ||
         kind == AST_MUTATION_TOGGLE_REGISTER_ANNOTATION ||
         kind == AST_MUTATION_MERGE_SPLIT_SCRATCH_WEBS ||
         kind == AST_MUTATION_TOGGLE_STORE_FLAG_CONDITIONAL ||
         kind == AST_MUTATION_HOIST_LOOP_INVARIANT;
}

bool IsNearMissScore(const CandidateScore& score) {
  const size_t target_insns = score.target_instruction_count;
  const size_t matching_insns = score.matching_instructions;
  const double match_ratio =
      target_insns > 0 ? static_cast<double>(matching_insns) / target_insns : 0.0;
  return (target_insns >= matching_insns && (target_insns - matching_insns <= 6)) ||
         (match_ratio >= 0.85);
}

}  // namespace

absl::StatusOr<FunctionSearchResult> TunerPipeline::SearchFunctionCandidates(
    const FunctionDeclaration& baseline_ast, const std::vector<std::string>& base_flags,
    const FunctionSearchOptions& options,
    const std::function<std::string(const FunctionDeclaration&)>& emit_c_fn,
    const std::function<absl::StatusOr<CandidateScore>(
        std::string_view c_code, const std::vector<std::string>& flags)>& score_fn) {
  FunctionSearchResult result;
  result.winning_c_flags = base_flags;
  result.best_ast = std::make_unique<FunctionDeclaration>(baseline_ast.Clone());
  result.best_c_code = emit_c_fn(baseline_ast);

  absl::flat_hash_set<std::string> evaluated_candidates;
  auto make_evaluation_key = [](const std::vector<std::string>& flags, std::string_view code) {
    return absl::StrCat(absl::StrJoin(flags, " "), "\n", code);
  };
  evaluated_candidates.insert(make_evaluation_key(base_flags, result.best_c_code));

  auto base_score_or = score_fn(result.best_c_code, base_flags);
  if (!base_score_or.ok()) {
    return base_score_or.status();
  }
  result.baseline_score = *base_score_or;
  result.best_score = *base_score_or;

  std::vector<std::vector<std::string>> flag_profiles = {base_flags};
  if (options.explore_flags) {
    flag_profiles = FlagExplorer::GenerateCandidateFlags(base_flags, options.flag_options);
  }
  if (!options.previous_cached_flags.empty() &&
      std::find(flag_profiles.begin(), flag_profiles.end(), options.previous_cached_flags) ==
          flag_profiles.end()) {
    flag_profiles.push_back(options.previous_cached_flags);
  }

  // Phase 1a: Compiler Flag Sweep on Baseline AST first (immediately resolves functions that only
  // need `-Wa,-O2` and ensures Phase 1b/Phase 2 run with the best flag profile).
  if (!result.best_score.raw_bit_exact && (options.explore_flags || flag_profiles.size() > 1)) {
    for (size_t i = 1; i < flag_profiles.size(); ++i) {
      if (evaluated_candidates.insert(make_evaluation_key(flag_profiles[i], result.best_c_code))
              .second) {
        auto candidate_score_or = score_fn(result.best_c_code, flag_profiles[i]);
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
  bool improved_in_ast_search = false;
  if (!result.best_score.raw_bit_exact && options.explore_ast_mutations) {
    auto presets = AstMutator::GenerateCanonicalPresets(baseline_ast);
    if (!options.previous_cached_mutations.empty()) {
      presets.insert(presets.begin(), options.previous_cached_mutations);
    }
    for (const auto& preset_steps : presets) {
      FunctionDeclaration preset_ast = AstMutator::ApplyMutations(baseline_ast, preset_steps);
      std::string preset_c = emit_c_fn(preset_ast);
      if (!evaluated_candidates.insert(make_evaluation_key(result.winning_c_flags, preset_c))
               .second) {
        continue;
      }
      auto candidate_score_or = score_fn(preset_c, result.winning_c_flags);
      if (!candidate_score_or.ok()) continue;
      if (candidate_score_or->IsBetterThan(result.best_score)) {
        result.best_score = *candidate_score_or;
        *result.best_ast = std::move(preset_ast);
        result.best_c_code = std::move(preset_c);
        result.applied_mutations = preset_steps;
        result.improved_over_baseline = true;
        improved_in_ast_search = true;
        if (result.best_score.raw_bit_exact) break;
      }
    }
  }

  // Phase 2: Greedy AST Mutation Search (steepest-ascent per round)
  if (!result.best_score.raw_bit_exact && options.explore_ast_mutations) {
    for (size_t round = 0; round < options.max_rounds && !result.best_score.raw_bit_exact;
         ++round) {
      const FunctionDeclaration round_base_ast = result.best_ast->Clone();
      auto candidate_steps =
          AstMutator::DiscoverMutations(round_base_ast, options.max_ast_candidates_per_round);
      bool improved_in_round = false;
      AstMutationStep round_best_step;
      for (const auto& step : candidate_steps) {
        FunctionDeclaration mutated = AstMutator::ApplyMutation(round_base_ast, step);
        std::string mutated_c = emit_c_fn(mutated);
        if (!evaluated_candidates.insert(make_evaluation_key(result.winning_c_flags, mutated_c))
                 .second) {
          continue;
        }
        auto candidate_score_or = score_fn(mutated_c, result.winning_c_flags);
        if (!candidate_score_or.ok()) continue;
        if (candidate_score_or->IsBetterThan(result.best_score)) {
          result.best_score = *candidate_score_or;
          *result.best_ast = std::move(mutated);
          result.best_c_code = std::move(mutated_c);
          round_best_step = step;
          result.improved_over_baseline = true;
          improved_in_round = true;
          improved_in_ast_search = true;
          if (result.best_score.raw_bit_exact) break;
        }
      }
      if (!improved_in_round) break;
      result.applied_mutations.push_back(round_best_step);
    }
  }

  // Phase 2b: Near-Miss Polish Pass
  // When a function is not yet bit-exact after the main mutation rounds, if the best candidate is
  // a near-miss (target_insns - matching_insns <= 6 or instruction_match_ratio >= 0.85), run an
  // extra Polish Pass (up to `max_polish_rounds` micro-rounds) that tests high-yield near-miss
  // mutations first on `winning_c_flags` without the O(mutations * flag_profiles) explosion.
  if (!result.best_score.raw_bit_exact && options.explore_ast_mutations &&
      IsNearMissScore(result.best_score)) {
    for (size_t polish_round = 0;
         polish_round < options.max_polish_rounds && !result.best_score.raw_bit_exact;
         ++polish_round) {
      const FunctionDeclaration polish_base_ast = result.best_ast->Clone();
      auto all_steps =
          AstMutator::DiscoverMutations(polish_base_ast, options.max_polish_candidates);
      std::vector<AstMutationStep> polish_steps;
      polish_steps.reserve(all_steps.size());
      for (const auto& step : all_steps) {
        if (IsHighYieldPolishMutation(step.kind())) {
          polish_steps.push_back(step);
        }
      }
      for (const auto& step : all_steps) {
        if (!IsHighYieldPolishMutation(step.kind())) {
          polish_steps.push_back(step);
        }
      }

      bool improved_in_polish = false;
      AstMutationStep polish_best_step;
      // Stage 2b-1: Evaluate all polish mutations on `winning_c_flags` first (O(mutations)).
      for (const auto& step : polish_steps) {
        FunctionDeclaration mutated = AstMutator::ApplyMutation(polish_base_ast, step);
        std::string mutated_c = emit_c_fn(mutated);
        if (!evaluated_candidates.insert(make_evaluation_key(result.winning_c_flags, mutated_c))
                 .second) {
          continue;
        }
        auto candidate_score_or = score_fn(mutated_c, result.winning_c_flags);
        if (!candidate_score_or.ok()) continue;
        if (candidate_score_or->IsBetterThan(result.best_score)) {
          result.best_score = *candidate_score_or;
          *result.best_ast = std::move(mutated);
          result.best_c_code = std::move(mutated_c);
          polish_best_step = step;
          result.improved_over_baseline = true;
          improved_in_polish = true;
          improved_in_ast_search = true;
          if (result.best_score.raw_bit_exact) break;
        }
      }
      // Stage 2b-2: If still not bit-exact, co-evaluate bounded high-yield polish steps across
      // remaining flag profiles.
      if (!result.best_score.raw_bit_exact && (options.explore_flags || flag_profiles.size() > 1)) {
        const size_t cross_flag_limit =
            std::min(polish_steps.size(), options.max_cross_flag_polish_steps);
        for (size_t idx = 0; idx < cross_flag_limit; ++idx) {
          const auto& step = polish_steps[idx];
          FunctionDeclaration mutated = AstMutator::ApplyMutation(polish_base_ast, step);
          std::string mutated_c = emit_c_fn(mutated);
          for (const auto& flags : flag_profiles) {
            if (!evaluated_candidates.insert(make_evaluation_key(flags, mutated_c)).second) {
              continue;
            }
            auto candidate_score_or = score_fn(mutated_c, flags);
            if (!candidate_score_or.ok()) continue;
            if (candidate_score_or->IsBetterThan(result.best_score)) {
              result.best_score = *candidate_score_or;
              result.winning_c_flags = flags;
              *result.best_ast = mutated.Clone();
              result.best_c_code = mutated_c;
              polish_best_step = step;
              result.improved_over_baseline = true;
              improved_in_polish = true;
              improved_in_ast_search = true;
              if (result.best_score.raw_bit_exact) break;
            }
          }
          if (result.best_score.raw_bit_exact) break;
        }
      }
      if (!improved_in_polish) break;
      result.applied_mutations.push_back(polish_best_step);
    }
  }

  // Phase 3: Post-AST Compiler Flag Re-Sweep if Phase 1b, Phase 2, or Phase 2b mutated the AST
  if (!result.best_score.raw_bit_exact && (options.explore_flags || flag_profiles.size() > 1) &&
      improved_in_ast_search) {
    for (const auto& flags : flag_profiles) {
      if (flags == result.winning_c_flags) continue;
      if (!evaluated_candidates.insert(make_evaluation_key(flags, result.best_c_code)).second) {
        continue;
      }
      auto candidate_score_or = score_fn(result.best_c_code, flags);
      if (!candidate_score_or.ok()) continue;
      if (candidate_score_or->IsBetterThan(result.best_score)) {
        result.best_score = *candidate_score_or;
        result.winning_c_flags = flags;
        result.improved_over_baseline = true;
        if (result.best_score.raw_bit_exact) break;
      }
    }
  }

  return result;
}

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

  if (!module_name.empty()) {
    auto module_funcs_or = lifter_->GetFunctionsInModule(module_name);
    if (module_funcs_or.ok() && module_funcs_or->size() > 1) {
      auto mod_tu_or = lifter_->BuildTranslationUnit(*module_funcs_or);
      if (mod_tu_or.ok()) {
        for (const auto& fn : mod_tu_or->functions) {
          if (fn.Name() == target_or->name) {
            auto res_or = TuneLoadedFunctionAst(*target_or, fn, &(*mod_tu_or), module_name);
            if (!res_or.ok()) {
              return res_or.status();
            }
            if (!options_.cache_out_path.empty()) {
              (void)cache_store_.SaveToFile(options_.cache_out_path, options_.game_name);
            }
            return res_or;
          }
        }
      }
    }
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
    const auto* cached_exact = cache_store_.Lookup(target.name, input_sha1);
    const auto* cached_by_name =
        cached_exact != nullptr ? cached_exact : cache_store_.Lookup(target.name, "");

    if (cached_exact != nullptr) {
      if (!options_.only_non_bit_exact || cached_exact->is_bit_exact()) {
        result.from_cache = true;
        if (!cached_exact->c_flags().empty()) {
          result.winning_c_flags.assign(cached_exact->c_flags().begin(),
                                        cached_exact->c_flags().end());
        }
        result.applied_mutations.assign(cached_exact->ast_mutations().begin(),
                                        cached_exact->ast_mutations().end());
        FunctionDeclaration mutated =
            AstMutator::ApplyMutations(baseline_ast, result.applied_mutations);
        result.best_score.raw_bit_exact = cached_exact->is_bit_exact();
        result.best_score.normalized_exact = cached_exact->is_bit_exact();
        result.best_score.bit_exact_instructions = cached_exact->bit_exact_instructions();
        result.best_score.matching_instructions = cached_exact->matching_instructions();
        result.best_score.target_instruction_count = cached_exact->target_instructions();
        result.baseline_score = result.best_score;
        result.improved_over_baseline = !result.applied_mutations.empty();
        if (emit_formatted_c) {
          result.tuned_c_code = EmitSingleFunctionWithExterns(mutated, parent_tu, symbols,
                                                              options_.format_with_clang);
        }
        result.tuned_ast = std::make_unique<FunctionDeclaration>(std::move(mutated));
        return result;
      }
    }

    if (cached_by_name != nullptr) {
      previous_cached_mutations.assign(cached_by_name->ast_mutations().begin(),
                                       cached_by_name->ast_mutations().end());
      previous_cached_flags.assign(cached_by_name->c_flags().begin(),
                                   cached_by_name->c_flags().end());
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
    types_out << CEmitter::EmitTypesHeader();
  }
  std::vector<std::filesystem::path> extra_include_dirs = {temporary_include_dir};
  if (!options_.lifted_dir.empty()) {
    extra_include_dirs.push_back(options_.lifted_dir);
  }

  CandidateScorer scorer(extractor, symbols);
  FunctionSearchOptions search_opts;
  search_opts.explore_flags = options_.explore_flags;
  search_opts.explore_ast_mutations = options_.explore_ast_mutations;
  search_opts.flag_options = options_.flag_options;
  search_opts.max_ast_candidates_per_round = options_.max_ast_candidates_per_round;
  search_opts.max_rounds = options_.max_rounds;
  search_opts.previous_cached_flags = std::move(previous_cached_flags);
  search_opts.previous_cached_mutations = std::move(previous_cached_mutations);

  auto search_res_or = SearchFunctionCandidates(
      baseline_ast, base_flags, search_opts,
      [&](const FunctionDeclaration& ast) {
        return EmitSingleFunctionWithExterns(ast, parent_tu, symbols, /*format_with_clang=*/false);
      },
      [&](std::string_view c_code, const std::vector<std::string>& flags) {
        return scorer.CompileAndScore(target, c_code, flags, extra_include_dirs);
      });
  std::filesystem::remove_all(temporary_include_dir, error_code);
  if (!search_res_or.ok()) {
    result.tuned_ast = std::make_unique<FunctionDeclaration>(baseline_ast.Clone());
    if (emit_formatted_c) {
      result.tuned_c_code = EmitSingleFunctionWithExterns(*result.tuned_ast, parent_tu, symbols,
                                                          options_.format_with_clang);
    }
    return result;
  }

  result.baseline_score = std::move(search_res_or->baseline_score);
  result.best_score = std::move(search_res_or->best_score);
  result.winning_c_flags = std::move(search_res_or->winning_c_flags);
  result.applied_mutations = std::move(search_res_or->applied_mutations);
  result.improved_over_baseline = search_res_or->improved_over_baseline;
  FunctionDeclaration best_ast = std::move(*search_res_or->best_ast);

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
  translation_unit_or->parent_module_context =
      std::make_shared<const ModuleSymbolContext>(AstCodegenLowerer::AnalyzeModuleSymbols(
          translation_unit_or->functions, translation_unit_or->symbol_index,
          translation_unit_or->header_provided_functions));

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

  translation_unit_or->parent_module_context.reset();
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
      std::filesystem::permissions(dst, std::filesystem::perms::owner_write,
                                   std::filesystem::perm_options::add, ec);
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
        std::error_code perm_ec;
        if (std::filesystem::exists(out_c, perm_ec)) {
          std::filesystem::permissions(out_c, std::filesystem::perms::owner_write,
                                       std::filesystem::perm_options::add, perm_ec);
        }
        std::ofstream ofs(out_c);
        if (!ofs.is_open()) {
          return absl::PermissionDeniedError(
              absl::StrFormat("Failed to open '%s' for writing tuned C code.", out_c.string()));
        }
        ofs << tuned_mod_or->tuned_c_code;
        if (!ofs.good()) {
          return absl::InternalError(
              absl::StrFormat("Failed to write tuned C code to '%s'.", out_c.string()));
        }
        if (std::find(emitted_files.begin(), emitted_files.end(), out_c) == emitted_files.end()) {
          emitted_files.push_back(out_c);
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
