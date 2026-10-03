#include "lifter/lifter_pipeline.h"

#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_format.h"
#include "absl/strings/strip.h"
#include "core/c_ast.h"
#include "lifter/ast_converter.h"
#include "lifter/c_emitter.h"
#include "lifter/control_flow_graph.h"
#include "lifter/control_flow_structurer.h"
#include "lifter/dominator_tree.h"
#include "lifter/function_loader.h"
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
    const LoadedFunction& loaded_func) const {
  if (loaded_func.instructions.empty()) {
    return absl::InvalidArgumentError("LifterPipeline: No instructions to decompile.");
  }

  // 1. Build Control Flow Graph
  auto cfg_or = ControlFlowGraph::Build(loaded_func.instructions);
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

absl::StatusOr<std::string> LifterPipeline::DecompileFunctions(
    const std::vector<std::string>& func_names, const std::vector<std::string>& includes) const {
  if (func_names.empty()) {
    return absl::InvalidArgumentError("LifterPipeline: No function names provided.");
  }

  CTranslationUnit tu;
  tu.includes = !includes.empty() ? includes : options_.includes;

  for (const auto& func_name : func_names) {
    auto res_or = Decompile(func_name);
    if (!res_or.ok()) {
      return res_or.status();
    }
    tu.functions.push_back(std::move(*res_or->ast));
  }

  CEmitterOptions emitter_opts;
  emitter_opts.includes = tu.includes;
  emitter_opts.format_with_clang = options_.format_with_clang;

  return CEmitter::EmitTranslationUnit(tu, emitter_opts);
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

  std::filesystem::path game_src_dir = options_.repo_root / "src" / "c" / options_.game_name;
  if (std::filesystem::exists(game_src_dir, ec)) {
    for (const auto& entry : std::filesystem::directory_iterator(game_src_dir, ec)) {
      if (entry.is_regular_file() && entry.path().extension() == ".h") {
        std::filesystem::copy_file(entry.path(), output_dir / entry.path().filename(),
                                   std::filesystem::copy_options::overwrite_existing, ec);
      }
    }
  }

  std::vector<std::string> default_headers = options_.includes;
  default_headers.push_back("types.h");

  std::vector<std::filesystem::path> emitted_files;
  for (const auto& mod : *modules_or) {
    auto funcs_or = GetFunctionsInModule(mod);
    if (!funcs_or.ok() || funcs_or->empty()) {
      continue;
    }

    std::vector<std::string> includes = default_headers;
    if (std::filesystem::exists(game_src_dir / absl::StrCat(mod, ".h"), ec)) {
      includes.push_back(absl::StrCat(mod, ".h"));
    }

    std::filesystem::path out_file = output_dir / absl::StrCat(mod, ".c");

    auto c_code_or = DecompileFunctions(*funcs_or, includes);
    if (!c_code_or.ok()) {
      std::string stub = absl::StrFormat("// Module '%s' lifting deferred: %s\n", mod,
                                         c_code_or.status().message());
      std::ofstream ofs(out_file);
      if (ofs.is_open()) {
        ofs << stub;
        emitted_files.push_back(out_file);
      }
      continue;
    }

    std::ofstream ofs(out_file);
    if (!ofs.is_open()) {
      return absl::InternalError(
          absl::StrFormat("Failed to open %s for writing.", out_file.string()));
    }
    ofs << *c_code_or;
    emitted_files.push_back(out_file);
  }

  return emitted_files;
}

}  // namespace rom_nom_nom
