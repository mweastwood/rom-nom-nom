#include "lifter/lifter_pipeline.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
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
    const std::vector<std::string>& func_names) const {
  if (func_names.empty()) {
    return absl::InvalidArgumentError("LifterPipeline: No function names provided.");
  }

  CTranslationUnit tu;
  tu.includes = options_.includes;

  for (const auto& func_name : func_names) {
    auto res_or = Decompile(func_name);
    if (!res_or.ok()) {
      return res_or.status();
    }
    tu.functions.push_back(std::move(*res_or->ast));
  }

  CEmitterOptions emitter_opts;
  emitter_opts.includes = options_.includes;
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

  // Find subsegment matching module_name
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

  std::vector<std::string> func_names;
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

absl::StatusOr<std::string> LifterPipeline::DecompileModule(std::string_view module_name) const {
  auto funcs_or = GetFunctionsInModule(module_name);
  if (!funcs_or.ok()) {
    return funcs_or.status();
  }
  return DecompileFunctions(*funcs_or);
}

}  // namespace rom_nom_nom
