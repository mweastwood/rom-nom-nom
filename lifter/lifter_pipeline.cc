#include "lifter/lifter_pipeline.h"

#include <fstream>
#include <memory>
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
#include "core/c_ast.h"
#include "lifter/ast_converter.h"
#include "lifter/c_emitter.h"
#include "lifter/control_flow_graph.h"
#include "lifter/control_flow_structurer.h"
#include "lifter/dominator_tree.h"
#include "lifter/function_loader.h"
#include "lifter/jump_table.h"
#include "lifter/loop_analyzer.h"

namespace rom_nom_nom {

LifterPipeline::LifterPipeline(LifterPipelineOptions options,
                               std::unique_ptr<FunctionLoader> loader)
    : options_(std::move(options)), loader_(std::move(loader)) {}

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

  return DecompileFunction(*loaded_or);
}

absl::StatusOr<LifterResult> LifterPipeline::DecompileFunction(
    const LoadedFunction& loaded_func,
    const absl::flat_hash_map<std::string, int>* function_parameter_counts) const {
  if (loaded_func.instructions.empty()) {
    return absl::InvalidArgumentError("LifterPipeline: No instructions to decompile.");
  }

  // 1. Detect Jump Tables and Build Control Flow Graph
  std::vector<JumpTable> jump_tables;
  if (loaded_func.memory_reader) {
    jump_tables =
        JumpTableDetector::DetectJumpTables(loaded_func.instructions, loaded_func.memory_reader);
  }
  auto cfg_or = ControlFlowGraph::Build(loaded_func.instructions, jump_tables);
  if (!cfg_or.ok()) {
    return cfg_or.status();
  }
  const auto& cfg = *cfg_or;

  // 2. Compute Dominator Trees
  DominatorTree dom_tree = DominatorTree::Compute(cfg);
  DominatorTree post_dom_tree = DominatorTree::ComputePostDominators(cfg);

  // 3. Analyze Natural Loops
  LoopInfo loop_info = LoopInfo::Analyze(cfg, dom_tree);

  // 4. Structure Control Flow into Hierarchical Regions
  auto root_region = ControlFlowStructurer::Structure(cfg, dom_tree, post_dom_tree, loop_info);
  if (!root_region) {
    return absl::InternalError("LifterPipeline: Failed to structure control flow graph.");
  }

  // 5. Convert Structured Regions and Lifted Instructions to C AST
  AstConverterOptions converter_opts;
  converter_opts.function_name = loaded_func.name;
  converter_opts.split_config = loaded_func.split_config;
  converter_opts.function_parameter_counts = function_parameter_counts;

  FunctionDeclaration ast =
      AstConverter::Convert(cfg, *root_region, loaded_func.symbol_index, converter_opts);

  // 6. Format and Emit C Code
  CEmitterOptions emitter_opts;
  emitter_opts.includes = options_.includes;
  emitter_opts.format_with_clang = options_.format_with_clang;

  std::string c_code = CEmitter::EmitFunction(ast, emitter_opts);

  LifterResult result;
  result.function_name = loaded_func.name;
  result.vram = loaded_func.vram;
  result.instruction_count = loaded_func.instructions.size();
  result.ast = std::make_unique<FunctionDeclaration>(std::move(ast));
  result.c_code = std::move(c_code);

  return result;
}

absl::StatusOr<CTranslationUnit> LifterPipeline::BuildTranslationUnit(
    const std::vector<std::string>& func_names, const std::vector<std::string>& includes) const {
  if (func_names.empty()) {
    return absl::InvalidArgumentError("LifterPipeline: No function names provided.");
  }

  CTranslationUnit tu;
  tu.includes = !includes.empty() ? includes : options_.includes;
  if (loader_ != nullptr && loader_->Extractor() != nullptr) {
    tu.symbol_index = loader_->Extractor()->Symbols();
  }

  std::vector<LoadedFunction> loaded_functions;
  loaded_functions.reserve(func_names.size());
  absl::flat_hash_map<std::string, int> parameter_counts;

  for (const auto& func_name : func_names) {
    auto loaded_or = loader_->LoadFunction(func_name);
    if (!loaded_or.ok()) {
      return loaded_or.status();
    }
    parameter_counts[loaded_or->name] =
        ExpressionBuilder::DetermineParameterCount(loaded_or->instructions);
    loaded_functions.push_back(std::move(*loaded_or));
  }

  for (const auto& loaded_func : loaded_functions) {
    auto res_or = DecompileFunction(loaded_func, &parameter_counts);
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

  std::vector<std::string> func_names;
  absl::flat_hash_set<std::string> seen;

  std::vector<std::filesystem::path> search_paths;
  if (!options_.asm_dir.empty()) {
    search_paths.push_back(options_.asm_dir / absl::StrCat(module_name, ".s"));
  }
  search_paths.push_back(options_.repo_root / "bazel-bin" / "asm" / options_.game_name /
                         absl::StrCat(module_name, ".s"));
  search_paths.push_back(options_.repo_root / "asm" / options_.game_name /
                         absl::StrCat(module_name, ".s"));

  for (const auto& p : search_paths) {
    if (std::filesystem::exists(p)) {
      std::ifstream ifs(p);
      std::string line;
      while (std::getline(ifs, line)) {
        if (absl::StartsWith(line, ".ent ")) {
          std::string name = std::string(absl::StripAsciiWhitespace(line.substr(5)));
          if (!name.empty() && seen.insert(name).second) {
            func_names.push_back(name);
          }
        }
      }
      if (!func_names.empty()) {
        break;
      }
    }
  }

  std::vector<std::filesystem::path> nonmatching_dirs;
  if (!options_.asm_dir.empty()) {
    nonmatching_dirs.push_back(options_.asm_dir / "nonmatchings" / module_name);
  }
  nonmatching_dirs.push_back(options_.repo_root / "bazel-bin" / "asm" / options_.game_name /
                             "nonmatchings" / module_name);
  nonmatching_dirs.push_back(options_.repo_root / "asm" / options_.game_name / "nonmatchings" /
                             module_name);
  for (const auto& dir : nonmatching_dirs) {
    if (std::filesystem::exists(dir)) {
      for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".s") {
          std::string stem = entry.path().stem().string();
          if (!stem.empty() && seen.insert(stem).second) {
            func_names.push_back(stem);
          }
        }
      }
    }
  }

  if (!func_names.empty()) {
    return func_names;
  }

  // Fallback: Find subsegment matching module_name in config
  const Subsegment* matched_subseg = nullptr;
  const Segment* matched_seg = nullptr;
  uint32_t next_rom_start = 0;

  for (const auto& seg : config->segments()) {
    for (int i = 0; i < seg.subsegments_size(); ++i) {
      const auto& subseg = seg.subsegments(i);
      if (subseg.name() == module_name) {
        matched_subseg = &subseg;
        matched_seg = &seg;
        if (i + 1 < seg.subsegments_size()) {
          next_rom_start = seg.subsegments(i + 1).rom_start();
        } else {
          next_rom_start = seg.rom_end();
        }
        break;
      }
    }
    if (matched_subseg != nullptr) break;
  }

  if (matched_subseg == nullptr) {
    return absl::NotFoundError(
        absl::StrFormat("Module '%s' not found in split configuration.", module_name));
  }

  uint32_t seg_offset = matched_subseg->rom_start() - matched_seg->rom_start();
  uint32_t start_vram = matched_seg->vram() + seg_offset;
  uint32_t end_vram = start_vram + (next_rom_start - matched_subseg->rom_start());

  for (const auto* entry : symbols->SortedEntries()) {
    if (entry->type() == SYMBOL_FUNC && entry->address() >= start_vram &&
        entry->address() < end_vram) {
      func_names.push_back(std::string(entry->name()));
    }
  }

  if (func_names.empty()) {
    // If no individual functions are declared in symbols, fallback to synthesized name
    func_names.push_back(symbols->LookupOrSynthesizeName(start_vram, SYMBOL_FUNC));
  }

  return func_names;
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

  std::vector<std::string> modules;
  for (const auto& seg : config->segments()) {
    if (seg.type() != SEGMENT_CODE) {
      continue;
    }
    for (const auto& subseg : seg.subsegments()) {
      if (subseg.type() == SUBSEGMENT_C || subseg.type() == SUBSEGMENT_ASM) {
        modules.push_back(std::string(subseg.name()));
      }
    }
  }
  return modules;
}

absl::StatusOr<std::vector<std::filesystem::path>> LifterPipeline::DecompileAllModules(
    const std::filesystem::path& output_dir) const {
  auto modules_or = GetAllModules();
  if (!modules_or.ok()) {
    return modules_or.status();
  }

  std::error_code ec;
  std::filesystem::create_directories(output_dir, ec);

  // Generate self-contained standard types header
  std::filesystem::path types_file_path = output_dir / "types.h";
  std::ofstream types_output_stream(types_file_path);
  if (types_output_stream.is_open()) {
    types_output_stream << R"(#ifndef TYPES_H
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

#endif  // TYPES_H
)";
  }

  std::vector<std::string> default_headers = {"types.h"};

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
      std::string stub = absl::StrFormat("// Module '%s' lifting deferred: %s\n", module_name,
                                         tu_or.status().message());
      std::ofstream output_stream(output_file);
      if (output_stream.is_open()) {
        output_stream << stub;
        emitted_files.push_back(output_file);
      }
      continue;
    }

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

  // Generate self-contained linker script for undefined auto-symbols
  std::filesystem::path lifted_symbols_file_path = output_dir / "lifted_symbols.ld";
  std::ofstream symbols_output_stream(lifted_symbols_file_path);
  if (symbols_output_stream.is_open()) {
    symbols_output_stream << "/* Auto-generated symbol definitions for lifted C */\n";
    for (const auto& [symbol_name, address_hex] : undefined_auto_symbols) {
      symbols_output_stream << absl::StrFormat("%s = 0x%s;\n", symbol_name, address_hex);
    }
  }

  return emitted_files;
}

}  // namespace rom_nom_nom
