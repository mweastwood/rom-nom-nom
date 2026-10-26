#include "lifter/lifter_pipeline.h"

#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_format.h"
#include "absl/strings/strip.h"
#include "absl/types/span.h"
#include "core/c_ast.h"
#include "core/control_flow_graph.h"
#include "core/jump_table.h"
#include "lifter/abi_analyzer.h"
#include "lifter/ast_converter.h"
#include "lifter/c_emitter.h"
#include "lifter/cfg_de_reorg.h"
#include "lifter/control_flow_structurer.h"
#include "lifter/decompilation_context.h"
#include "lifter/dominator_tree.h"
#include "lifter/function_loader.h"
#include "lifter/loop_analyzer.h"
#include "lifter/module_symbol_scanner.h"

namespace rom_nom_nom {
namespace {

void PopulateFunctionAbiMetadata(absl::Span<const Instruction> instructions,
                                 const std::string& func_name, const SymbolIndex* symbol_index,
                                 absl::flat_hash_map<std::string, int>* parameter_counts,
                                 absl::flat_hash_map<std::string, int>* fp_parameter_counts,
                                 absl::flat_hash_set<std::string>* callers_use_v0,
                                 absl::flat_hash_set<std::string>* callers_use_f0) {
  (*parameter_counts)[func_name] = AbiAnalyzer::DetermineParameterCount(instructions);
  (*fp_parameter_counts)[func_name] = AbiAnalyzer::DetermineFpParameterCount(instructions);
  auto cfg_or = ControlFlowGraph::Build(instructions);
  if (cfg_or.ok()) {
    if (AbiAnalyzer::DetermineReturnsF0(*cfg_or)) {
      callers_use_f0->insert(func_name);
    } else if (AbiAnalyzer::DetermineReturnsV0(*cfg_or, symbol_index, callers_use_v0)) {
      callers_use_v0->insert(func_name);
    }
  }
}

void ScanCallReturnRegisterUses(absl::Span<const Instruction> insts, size_t call_idx,
                                const std::string& callee_name,
                                absl::flat_hash_set<std::string>* callers_use_v0,
                                absl::flat_hash_set<std::string>* callers_use_f0) {
  CallReturnInfo info =
      AbiAnalyzer::ScanCallReturnUsage(insts, call_idx + 2, /*stop_at_branches=*/true);
  if (info.returns_v0) {
    callers_use_v0->insert(callee_name);
  }
  if (info.returns_f0 && !callers_use_v0->contains(callee_name)) {
    callers_use_f0->insert(callee_name);
  }
}

}  // namespace

LifterPipeline::LifterPipeline(LifterPipelineOptions options,
                               std::unique_ptr<FunctionLoader> loader)
    : options_(std::move(options)), loader_(std::move(loader)) {
  if (options_.game_name.empty() && loader_ != nullptr && loader_->Extractor() != nullptr &&
      loader_->Extractor()->Config() != nullptr) {
    const auto* cfg = loader_->Extractor()->Config();
    if (!cfg->basename().empty()) {
      options_.game_name = cfg->basename();
    } else if (!cfg->game_name().empty()) {
      options_.game_name = cfg->game_name();
    }
  }
}

absl::StatusOr<std::unique_ptr<LifterPipeline>> LifterPipeline::Create(
    LifterPipelineOptions options) {
  FunctionLoaderOptions loader_opts;
  loader_opts.repo_root = options.repo_root;
  loader_opts.game_name = options.game_name;
  loader_opts.config_path = options.config_path;
  loader_opts.symbols_path = options.symbols_path;
  loader_opts.rom_path = options.rom_path;
  loader_opts.asm_dir = options.asm_dir;

  auto loader_or = FunctionLoader::Create(loader_opts);
  if (!loader_or.ok()) {
    return loader_or.status();
  }

  return std::make_unique<LifterPipeline>(std::move(options), std::move(*loader_or));
}

absl::StatusOr<LifterResult> LifterPipeline::Decompile(std::string_view func_name_or_addr) const {
  if (loader_ == nullptr) {
    return absl::FailedPreconditionError("LifterPipeline: No FunctionLoader initialized.");
  }

  auto loaded_or = loader_->LoadFunction(func_name_or_addr);
  if (!loaded_or.ok()) {
    return loaded_or.status();
  }

  auto tu_or = BuildTranslationUnitForLoadedFunctions(absl::MakeConstSpan(&(*loaded_or), 1));
  if (tu_or.ok() && !tu_or->functions.empty()) {
    CEmitterOptions emitter_opts;
    emitter_opts.includes = tu_or->includes;
    emitter_opts.format_with_clang = options_.format_with_clang;
    std::string c_code = CEmitter::EmitFunction(tu_or->functions[0], emitter_opts);
    LifterResult result;
    result.function_name = loaded_or->name;
    result.vram = loaded_or->vram;
    result.instruction_count = loaded_or->instructions.size();
    result.ast = std::make_unique<FunctionDeclaration>(std::move(tu_or->functions[0]));
    result.c_code = std::move(c_code);
    return result;
  }

  return DecompileFunction(*loaded_or);
}

absl::StatusOr<LifterResult> LifterPipeline::DecompileFunction(
    const LoadedFunction& loaded_func,
    const absl::flat_hash_map<std::string, int>* function_parameter_counts,
    const absl::flat_hash_map<std::string, int>* function_fp_parameter_counts,
    const absl::flat_hash_set<std::string>* functions_returning_v0,
    const absl::flat_hash_set<std::string>* functions_returning_f0) const {
  InterProceduralAbiContext abi_context;
  abi_context.function_parameter_counts = function_parameter_counts;
  abi_context.function_fp_parameter_counts = function_fp_parameter_counts;
  abi_context.functions_returning_v0 = functions_returning_v0;
  abi_context.functions_returning_f0 = functions_returning_f0;

  DecompilationContext context(loaded_func, abi_context);

  // 1. Detect Jump Tables, Build Control Flow Graph, and Unschedule Delay Slots
  if (absl::Status status = context.BuildCfg(); !status.ok()) {
    return status;
  }
  CfgDeReorg::UnscheduleDelaySlots(context.MutableCfg(), &context.CfgAnalyses());
  context.InvalidateFrameLayout();

  // 2. Compute Dominator Trees and Analyze Natural Loops via CfgAnalysisManager
  auto& cfg_analyses = context.CfgAnalyses();
  const DominatorTree& dom_tree = cfg_analyses.GetDominatorTree();
  const DominatorTree& post_dom_tree = cfg_analyses.GetPostDominatorTree();
  const LoopInfo& loop_info = cfg_analyses.GetLoopInfo();

  // 3. Structure Control Flow into Hierarchical Regions
  context.SetRootRegion(
      ControlFlowStructurer::Structure(context.Cfg(), dom_tree, post_dom_tree, loop_info));
  if (!context.HasRootRegion()) {
    return absl::InternalError("LifterPipeline: Failed to structure control flow graph.");
  }

  // 5. Convert Structured Regions and Lifted Instructions to C AST
  AstConverterOptions converter_opts;
  converter_opts.function_name = context.FunctionName();
  converter_opts.game_config = context.Config();
  converter_opts.function_parameter_counts = context.AbiContext().function_parameter_counts;
  converter_opts.function_fp_parameter_counts = context.AbiContext().function_fp_parameter_counts;
  converter_opts.functions_returning_v0 = context.AbiContext().functions_returning_v0;
  converter_opts.functions_returning_f0 = context.AbiContext().functions_returning_f0;

  auto ast_or =
      AstConverter::Convert(context.Cfg(), context.RootRegion(), context.Symbols(), converter_opts);
  if (!ast_or.ok()) {
    return ast_or.status();
  }
  context.SetAst(std::move(*ast_or));

  // 6. Format and Emit C Code
  CEmitterOptions emitter_opts;
  emitter_opts.includes = options_.includes;
  emitter_opts.format_with_clang = options_.format_with_clang;

  std::string c_code = CEmitter::EmitFunction(context.Ast(), emitter_opts);

  LifterResult result;
  result.function_name = context.FunctionName();
  result.vram = context.Vram();
  result.instruction_count = context.Function().instructions.size();
  result.ast = context.TakeAst();
  result.c_code = std::move(c_code);

  return result;
}

absl::StatusOr<CTranslationUnit> LifterPipeline::BuildTranslationUnit(
    const std::vector<std::string>& func_names, const std::vector<std::string>& includes) const {
  if (func_names.empty()) {
    return absl::InvalidArgumentError("LifterPipeline: No function names provided.");
  }
  if (loader_ == nullptr) {
    return absl::FailedPreconditionError("LifterPipeline: No FunctionLoader initialized.");
  }

  std::vector<LoadedFunction> loaded_functions;
  loaded_functions.reserve(func_names.size());
  for (const auto& func_name : func_names) {
    auto loaded_or = loader_->LoadFunction(func_name);
    if (!loaded_or.ok()) {
      return loaded_or.status();
    }
    loaded_functions.push_back(std::move(*loaded_or));
  }

  return BuildTranslationUnitForLoadedFunctions(loaded_functions, includes);
}

absl::StatusOr<CTranslationUnit> LifterPipeline::BuildTranslationUnitForLoadedFunctions(
    absl::Span<const LoadedFunction> loaded_functions,
    const std::vector<std::string>& includes) const {
  if (loaded_functions.empty()) {
    return absl::InvalidArgumentError("LifterPipeline: No functions provided.");
  }

  CTranslationUnit tu;
  tu.includes = !includes.empty() ? includes : options_.includes;
  if (loader_ != nullptr && loader_->Extractor() != nullptr) {
    tu.symbol_index = loader_->Extractor()->Symbols();
  }

  // Populate header-provided standard math inline functions if types.h is included
  // and the functions are not defined as concrete symbols in the ROM symbol index.
  bool includes_types_h = false;
  for (const auto& inc : tu.includes) {
    if (absl::StrContains(inc, "types.h")) {
      includes_types_h = true;
      break;
    }
  }
  if (includes_types_h) {
    for (std::string_view intrin_name : CEmitter::StandardMathIntrinsicNames()) {
      if (tu.symbol_index == nullptr || tu.symbol_index->FindByName(intrin_name) == nullptr) {
        tu.header_provided_functions.insert(std::string(intrin_name));
      }
    }
  }

  absl::flat_hash_map<std::string, int> parameter_counts;
  absl::flat_hash_map<std::string, int> fp_parameter_counts;
  absl::flat_hash_set<std::string> callers_use_v0;
  absl::flat_hash_set<std::string> callers_use_f0;

  for (const auto& loaded_func : loaded_functions) {
    PopulateFunctionAbiMetadata(loaded_func.instructions, loaded_func.name, tu.symbol_index,
                                &parameter_counts, &fp_parameter_counts, &callers_use_v0,
                                &callers_use_f0);
  }

  for (const auto& loaded_func : loaded_functions) {
    const auto& insts = loaded_func.instructions;
    for (size_t i = 0; i < insts.size(); ++i) {
      uint32_t target_vram = 0;
      if (insts[i].opcode == Opcode::kJal) {
        target_vram = insts[i].JumpTarget();
      } else if (insts[i].opcode == Opcode::kBltzal || insts[i].opcode == Opcode::kBgezal ||
                 insts[i].opcode == Opcode::kBltzall || insts[i].opcode == Opcode::kBgezall) {
        target_vram = insts[i].BranchTarget();
      }

      if (target_vram != 0) {
        std::string callee_name =
            (tu.symbol_index != nullptr)
                ? tu.symbol_index->LookupOrSynthesizeName(target_vram, SYMBOL_FUNC)
                : absl::StrFormat("func_%08X", target_vram);

        if (!parameter_counts.contains(callee_name) && loader_ != nullptr) {
          auto callee_loaded = loader_->LoadFunction(callee_name);
          if (callee_loaded.ok()) {
            PopulateFunctionAbiMetadata(callee_loaded->instructions, callee_name, tu.symbol_index,
                                        &parameter_counts, &fp_parameter_counts, &callers_use_v0,
                                        &callers_use_f0);
          }
        }

        ScanCallReturnRegisterUses(insts, i, callee_name, &callers_use_v0, &callers_use_f0);
      }
    }
  }

  for (const auto& loaded_func : loaded_functions) {
    auto res_or = DecompileFunction(loaded_func, &parameter_counts, &fp_parameter_counts,
                                    &callers_use_v0, &callers_use_f0);
    if (!res_or.ok()) {
      return res_or.status();
    }
    tu.functions.push_back(std::move(*res_or->ast));
  }

  return tu;
}

absl::StatusOr<std::string> LifterPipeline::DecompileFunctions(
    const std::vector<std::string>& func_names, const std::vector<std::string>& includes) const {
  auto tu_or = BuildTranslationUnit(func_names, includes);
  if (!tu_or.ok()) {
    return tu_or.status();
  }

  CEmitterOptions emitter_opts;
  emitter_opts.includes = tu_or->includes;
  emitter_opts.format_with_clang = options_.format_with_clang;

  return CEmitter::EmitTranslationUnit(*tu_or, emitter_opts);
}

absl::StatusOr<std::vector<std::string>> LifterPipeline::GetFunctionsInModule(
    std::string_view module_name) const {
  if (loader_ == nullptr || loader_->Extractor() == nullptr) {
    return absl::FailedPreconditionError("LifterPipeline: Loader or extractor uninitialized.");
  }
  const auto* config = loader_->Extractor()->Config();
  const auto* symbols = loader_->Extractor()->Symbols();
  if (config == nullptr || symbols == nullptr) {
    return absl::FailedPreconditionError("LifterPipeline: Config or symbols uninitialized.");
  }
  return ModuleSymbolScanner::DiscoverFunctionsInModule(
      module_name, *config, *symbols, options_.repo_root, options_.game_name, options_.asm_dir);
}

absl::StatusOr<std::string> LifterPipeline::DecompileModule(
    std::string_view module_name, const std::vector<std::string>& includes) const {
  auto funcs_or = GetFunctionsInModule(module_name);
  if (!funcs_or.ok()) {
    return funcs_or.status();
  }
  return DecompileFunctions(*funcs_or, includes);
}

absl::StatusOr<std::vector<std::string>> LifterPipeline::GetAllModules() const {
  if (loader_ == nullptr || loader_->Extractor() == nullptr) {
    return absl::FailedPreconditionError("LifterPipeline: Loader or extractor uninitialized.");
  }
  const auto* config = loader_->Extractor()->Config();
  if (config == nullptr) {
    return absl::FailedPreconditionError("LifterPipeline: Config uninitialized.");
  }
  return ModuleSymbolScanner::DiscoverAllCodeModules(*config);
}

absl::StatusOr<std::vector<std::filesystem::path>> LifterPipeline::DecompileAllModules(
    const std::filesystem::path& output_dir) const {
  auto modules_or = GetAllModules();
  if (!modules_or.ok()) {
    return modules_or.status();
  }

  std::error_code ec;
  std::filesystem::create_directories(output_dir, ec);

  // Track all defined functions across modules so we do not generate linker script
  // fallback definitions for functions that have implementations.
  absl::flat_hash_set<std::string> defined_function_names;
  for (const auto& module_name : *modules_or) {
    auto functions_or = GetFunctionsInModule(module_name);
    if (functions_or.ok()) {
      for (const auto& function_name : *functions_or) {
        defined_function_names.insert(function_name);
      }
    }
  }

  const auto* symbols = (loader_ != nullptr && loader_->Extractor() != nullptr)
                            ? loader_->Extractor()->Symbols()
                            : nullptr;

  auto has_symbol_or_func = [&](std::string_view name) {
    if (defined_function_names.contains(name)) return true;
    if (symbols != nullptr && symbols->FindByName(name) != nullptr) return true;
    return false;
  };

  std::set<std::string> header_provided_functions;

  // Generate self-contained standard types header
  std::filesystem::path types_file_path = output_dir / "types.h";
  std::ofstream types_output_stream(types_file_path);
  if (types_output_stream.is_open()) {
    TypesHeaderOptions types_opts;
    types_opts.should_skip_math_intrinsic = has_symbol_or_func;
    types_opts.emitted_math_intrinsics = &header_provided_functions;
    types_output_stream << CEmitter::EmitTypesHeader(types_opts);
  }

  std::vector<std::string> default_headers = {"types.h"};

  std::set<std::string> seen_symbols;
  std::vector<std::pair<std::string, std::string>> undefined_auto_symbols;

  auto collect_auto_symbol = [&](const std::string& symbol_name) {
    if (defined_function_names.contains(symbol_name)) {
      return;
    }
    if (symbols != nullptr && symbols->FindByName(symbol_name) != nullptr) {
      return;
    }
    uint32_t address = 0;
    if (symbol_name.size() == 13 && absl::StartsWith(symbol_name, "func_") &&
        absl::SimpleHexAtoi(symbol_name.substr(5), &address)) {
      if (seen_symbols.insert(symbol_name).second) {
        undefined_auto_symbols.emplace_back(symbol_name, symbol_name.substr(5));
      }
    } else if (symbol_name.size() == 10 && absl::StartsWith(symbol_name, "D_") &&
               absl::SimpleHexAtoi(symbol_name.substr(2), &address)) {
      if (seen_symbols.insert(symbol_name).second) {
        undefined_auto_symbols.emplace_back(symbol_name, symbol_name.substr(2));
      }
    }
  };

  std::vector<std::filesystem::path> emitted_files;
  for (const auto& module_name : *modules_or) {
    auto functions_or = GetFunctionsInModule(module_name);
    if (!functions_or.ok() || functions_or->empty()) {
      continue;
    }

    std::filesystem::path output_file = output_dir / absl::StrCat(module_name, ".c");

    auto tu_or = BuildTranslationUnit(*functions_or, default_headers);
    if (!tu_or.ok()) {
      for (const auto& func_name : *functions_or) {
        defined_function_names.erase(func_name);
      }
      std::string stub = absl::StrFormat("// Module '%s' lifting deferred: %s\n", module_name,
                                         tu_or.status().message());
      std::ofstream output_stream(output_file);
      if (output_stream.is_open()) {
        output_stream << stub;
        emitted_files.push_back(output_file);
      }
      continue;
    }

    tu_or->header_provided_functions.insert(header_provided_functions.begin(),
                                            header_provided_functions.end());

    CEmitterOptions emitter_opts;
    emitter_opts.includes = tu_or->includes;
    emitter_opts.format_with_clang = options_.format_with_clang;

    std::string c_code = CEmitter::EmitTranslationUnit(*tu_or, emitter_opts);

    std::ofstream output_stream(output_file);
    if (!output_stream.is_open()) {
      return absl::InternalError(
          absl::StrFormat("Failed to open %s for writing.", output_file.string()));
    }
    output_stream << c_code;
    emitted_files.push_back(output_file);

    // Collect undefined auto-symbols (D_XXXXXXXX or undeclared func_XXXXXXXX) directly via AST
    for (const auto& func : tu_or->functions) {
      ReferencedSymbols refs = CollectReferencedSymbols(func);
      for (const auto& func_name : refs.external_functions) {
        collect_auto_symbol(func_name);
      }
      for (const auto& data_name : refs.external_data) {
        collect_auto_symbol(data_name);
      }
    }
  }

  ModuleSymbolScanner::WriteLiftedSymbolsLinkerScript(
      output_dir, loader_.get(), symbols, defined_function_names, undefined_auto_symbols);

  return emitted_files;
}

}  // namespace rom_nom_nom
