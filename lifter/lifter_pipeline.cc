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
#include "lifter/ast_converter.h"
#include "lifter/c_emitter.h"
#include "lifter/control_flow_structurer.h"
#include "lifter/dominator_tree.h"
#include "lifter/function_loader.h"
#include "lifter/loop_analyzer.h"

namespace rom_nom_nom {

namespace {

struct MathIntrinsic {
  std::string_view name;
  std::string_view type;  // "f32" or "f64"
  std::string_view asm_inst;
};

constexpr MathIntrinsic kMathIntrinsics[] = {
    {"N64Sqrtf", "f32", "sqrt.s %0, %1"},
    {"N64Sqrt", "f64", "sqrt.d %0, %1"},
    {"sqrtf", "f32", "sqrt.s %0, %1"},
    {"sqrt", "f64", "sqrt.d %0, %1"},
    {"fabsf", "f32", "abs.s %0, %1"},
    {"fabs", "f64", "abs.d %0, %1"},
    {"ceilf", "f32", "ceil.w.s %0, %1\n\tcvt.s.w %0, %0"},
    {"floorf", "f32", "floor.w.s %0, %1\n\tcvt.s.w %0, %0"},
    {"roundf", "f32", "round.w.s %0, %1\n\tcvt.s.w %0, %0"},
    {"truncf", "f32", "trunc.w.s %0, %1\n\tcvt.s.w %0, %0"},
    {"ceil", "f64", "ceil.w.d %0, %1\n\tcvt.d.w %0, %0"},
    {"floor", "f64", "floor.w.d %0, %1\n\tcvt.d.w %0, %0"},
    {"round", "f64", "round.w.d %0, %1\n\tcvt.d.w %0, %0"},
    {"trunc", "f64", "trunc.w.d %0, %1\n\tcvt.d.w %0, %0"},
};

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
  converter_opts.game_config = loaded_func.game_config;
  converter_opts.function_parameter_counts = function_parameter_counts;
  converter_opts.function_fp_parameter_counts = function_fp_parameter_counts;
  converter_opts.functions_returning_v0 = functions_returning_v0;
  converter_opts.functions_returning_f0 = functions_returning_f0;

  auto ast_or = AstConverter::Convert(cfg, *root_region, loaded_func.symbol_index, converter_opts);
  if (!ast_or.ok()) {
    return ast_or.status();
  }
  FunctionDeclaration ast = std::move(*ast_or);

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
    for (const auto& intrin : kMathIntrinsics) {
      if (tu.symbol_index == nullptr || tu.symbol_index->FindByName(intrin.name) == nullptr) {
        tu.header_provided_functions.insert(std::string(intrin.name));
      }
    }
  }

  absl::flat_hash_map<std::string, int> parameter_counts;
  absl::flat_hash_map<std::string, int> fp_parameter_counts;
  absl::flat_hash_set<std::string> callers_use_v0;
  absl::flat_hash_set<std::string> callers_use_f0;

  for (const auto& loaded_func : loaded_functions) {
    parameter_counts[loaded_func.name] =
        ExpressionBuilder::DetermineParameterCount(loaded_func.instructions);
    fp_parameter_counts[loaded_func.name] =
        ExpressionBuilder::DetermineFpParameterCount(loaded_func.instructions);
    auto cfg_or = ControlFlowGraph::Build(loaded_func.instructions);
    if (cfg_or.ok()) {
      if (ExpressionBuilder::DetermineReturnsF0(*cfg_or)) {
        callers_use_f0.insert(loaded_func.name);
      } else if (ExpressionBuilder::DetermineReturnsV0(*cfg_or, tu.symbol_index, &callers_use_v0)) {
        callers_use_v0.insert(loaded_func.name);
      }
    }
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
            parameter_counts[callee_name] =
                ExpressionBuilder::DetermineParameterCount(callee_loaded->instructions);
            fp_parameter_counts[callee_name] =
                ExpressionBuilder::DetermineFpParameterCount(callee_loaded->instructions);
            auto callee_cfg = ControlFlowGraph::Build(callee_loaded->instructions);
            if (callee_cfg.ok()) {
              if (ExpressionBuilder::DetermineReturnsF0(*callee_cfg)) {
                callers_use_f0.insert(callee_name);
              } else if (ExpressionBuilder::DetermineReturnsV0(*callee_cfg, tu.symbol_index,
                                                               &callers_use_v0)) {
                callers_use_v0.insert(callee_name);
              }
            }
          }
        }

        bool v0_active = true;
        bool f0_active = true;
        for (size_t next_i = i + 2; next_i < insts.size(); ++next_i) {
          const auto& next_inst = insts[next_i];
          RegisterUseDef next_ud = GetInstructionUseDef(next_inst);
          if (v0_active) {
            for (Register r : next_ud.gpr_uses) {
              if (r == Register::kV0) {
                callers_use_v0.insert(callee_name);
                v0_active = false;
                break;
              }
            }
            for (Register r : next_ud.gpr_defs) {
              if (r == Register::kV0) {
                v0_active = false;
                break;
              }
            }
          }
          if (f0_active) {
            for (FpRegister r : next_ud.fpr_uses) {
              if (r == FpRegister::kF0) {
                if (!callers_use_v0.contains(callee_name)) {
                  callers_use_f0.insert(callee_name);
                }
                f0_active = false;
                break;
              }
            }
            for (FpRegister r : next_ud.fpr_defs) {
              if (r == FpRegister::kF0) {
                f0_active = false;
                break;
              }
            }
          }
          if (next_inst.opcode == Opcode::kJal || next_inst.opcode == Opcode::kJalr ||
              (next_inst.opcode == Opcode::kJr && next_inst.rs == Register::kRa)) {
            // Also inspect the delay slot of this subsequent call/return before terminating scan
            if (next_i + 1 < insts.size()) {
              RegisterUseDef ds_ud = GetInstructionUseDef(insts[next_i + 1]);
              if (v0_active) {
                for (Register r : ds_ud.gpr_uses) {
                  if (r == Register::kV0) {
                    callers_use_v0.insert(callee_name);
                    v0_active = false;
                    break;
                  }
                }
              }
              if (f0_active) {
                for (FpRegister r : ds_ud.fpr_uses) {
                  if (r == FpRegister::kF0) {
                    if (!callers_use_v0.contains(callee_name)) {
                      callers_use_f0.insert(callee_name);
                    }
                    f0_active = false;
                    break;
                  }
                }
              }
            }
            break;
          }
          if (!v0_active && !f0_active) {
            break;
          }
        }
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

  std::vector<std::string> func_names;
  absl::flat_hash_set<std::string> seen;

  // Find subsegment matching module_name (or filename stem) in config
  const Subsegment* matched_subseg = nullptr;
  const Segment* matched_seg = nullptr;
  uint32_t next_rom_start = 0;

  for (const auto& seg : config->segments()) {
    for (int i = 0; i < seg.subsegments_size(); ++i) {
      const auto& subseg = seg.subsegments(i);
      if (subseg.name() == module_name ||
          std::filesystem::path(subseg.name()).filename().string() == module_name) {
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

  std::vector<std::string> candidate_rel_names = {std::string(module_name)};
  if (matched_subseg != nullptr && matched_subseg->name() != module_name) {
    candidate_rel_names.push_back(std::string(matched_subseg->name()));
  }

  // 1. Check nonmatchings directory (e.g. <asm_dir>/nonmatchings/<module_name>/*.s)
  std::vector<std::filesystem::path> nonmatching_dirs;
  for (const auto& rel_name : candidate_rel_names) {
    if (!options_.asm_dir.empty()) {
      nonmatching_dirs.push_back(options_.asm_dir / "nonmatchings" / rel_name);
    }
    nonmatching_dirs.push_back(options_.repo_root / "bazel-bin" / "asm" / options_.game_name /
                               "nonmatchings" / rel_name);
    nonmatching_dirs.push_back(options_.repo_root / "asm" / options_.game_name / "nonmatchings" /
                               rel_name);
  }
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

  // 2. Check standalone module assembly file (e.g. <asm_dir>/<module_name>.s)
  std::vector<std::filesystem::path> asm_files;
  for (const auto& rel_name : candidate_rel_names) {
    if (!options_.asm_dir.empty()) {
      asm_files.push_back(options_.asm_dir / absl::StrCat(rel_name, ".s"));
    }
    asm_files.push_back(options_.repo_root / "bazel-bin" / "asm" / options_.game_name /
                        absl::StrCat(rel_name, ".s"));
    asm_files.push_back(options_.repo_root / "asm" / options_.game_name /
                        absl::StrCat(rel_name, ".s"));
  }
  for (const auto& file : asm_files) {
    if (std::filesystem::exists(file)) {
      std::ifstream in(file);
      std::string line;
      while (std::getline(in, line)) {
        std::string_view sv = line;
        while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t')) {
          sv.remove_prefix(1);
        }
        std::string func_name;
        if (absl::ConsumePrefix(&sv, ".ent ")) {
          func_name = std::string(absl::StripTrailingAsciiWhitespace(sv));
        } else if (absl::ConsumePrefix(&sv, "glabel ")) {
          func_name = std::string(absl::StripTrailingAsciiWhitespace(sv));
        }
        if (!func_name.empty() && seen.insert(func_name).second) {
          func_names.push_back(func_name);
        }
      }
      if (!func_names.empty()) {
        return func_names;
      }
    }
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

typedef struct { u8 b[4]; } UnalignedWord;
typedef struct { u8 b[6]; } UnalignedBlock6;
typedef struct { u8 b[8]; } UnalignedBlock8;
typedef struct { u8 b[12]; } UnalignedBlock12;
typedef struct { u8 b[16]; } UnalignedBlock16;

)";
    types_output_stream << "#if defined(__GNUC__) && (defined(__mips__) || defined(__mips))\n";
    for (const auto& intrin : kMathIntrinsics) {
      if (!has_symbol_or_func(intrin.name)) {
        header_provided_functions.insert(std::string(intrin.name));
        types_output_stream << absl::StrFormat(
            "static inline %s %s(%s x) {\n"
            "  %s res;\n"
            "  __asm__(\"%s\" : \"=f\"(res) : \"f\"(x));\n"
            "  return res;\n"
            "}\n",
            intrin.type, intrin.name, intrin.type, intrin.type, intrin.asm_inst);
      }
    }
    types_output_stream << R"(#else
#ifdef __cplusplus
#include <cmath>
static inline f32 N64Sqrtf(f32 x) { return std::sqrt(x); }
static inline f64 N64Sqrt(f64 x) { return std::sqrt(x); }
#else
#include <math.h>
static inline f32 N64Sqrtf(f32 x) { return sqrtf(x); }
static inline f64 N64Sqrt(f64 x) { return sqrt(x); }
#endif
#endif
)";
    types_output_stream << "\n#endif  // TYPES_H\n";
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

  // Generate self-contained linker script for undefined auto-symbols
  std::filesystem::path lifted_symbols_file_path = output_dir / "lifted_symbols.ld";
  std::ofstream symbols_output_stream(lifted_symbols_file_path);
  if (symbols_output_stream.is_open()) {
    symbols_output_stream << "/* Auto-generated symbol definitions for lifted C */\n";

    std::map<uint32_t, std::string> all_known_functions;
    if (symbols != nullptr) {
      for (const auto* entry : symbols->SortedEntries()) {
        if (entry->type() == SYMBOL_FUNC) {
          all_known_functions[entry->address()] = entry->name();
        }
      }
    }
    for (const auto& func_name : defined_function_names) {
      uint32_t addr = 0;
      if (func_name.size() == 13 && absl::StartsWith(func_name, "func_") &&
          absl::SimpleHexAtoi(func_name.substr(5), &addr)) {
        all_known_functions[addr] = func_name;
      }
    }

    // Data and BSS subsegments are each emitted as one contiguous blob with a single base label.
    // When lifted C code is larger or smaller than the original, these blobs move, so every
    // symbol inside them must be defined relative to the blob's base label rather than at its
    // original absolute address.
    struct DataSubsegmentRange {
      uint32_t vram_start = 0;
      uint32_t vram_end = 0;
      std::string label_name;
    };
    std::vector<DataSubsegmentRange> data_subsegments;
    auto label_for_vram = [&](uint32_t vram) {
      if (symbols != nullptr) {
        const auto* sym = symbols->FindByAddress(vram);
        if (sym != nullptr) {
          return std::string(sym->name());
        }
      }
      return absl::StrFormat("D_%08X", vram);
    };
    if (loader_ != nullptr && loader_->Extractor() != nullptr) {
      const auto* cfg = loader_->Extractor()->Config();
      if (cfg != nullptr) {
        for (const auto& seg : cfg->segments()) {
          if (seg.type() != SEGMENT_CODE) continue;
          size_t seg_start = seg.rom_start();
          size_t seg_end = seg.rom_end();
          uint32_t seg_vram = seg.vram();
          for (int j = 0; j < seg.subsegments_size(); ++j) {
            const auto& sub = seg.subsegments(j);
            if (sub.type() == SUBSEGMENT_DATA) {
              size_t sub_start = sub.rom_start();
              size_t sub_end = seg_end;
              for (int k = j + 1; k < seg.subsegments_size(); ++k) {
                if (seg.subsegments(k).type() != SUBSEGMENT_BSS &&
                    seg.subsegments(k).rom_start() > 0) {
                  sub_end = seg.subsegments(k).rom_start();
                  break;
                }
              }
              uint32_t sub_vram = sub.vram();
              if (sub_vram == 0 && seg_vram != 0) {
                sub_vram = seg_vram + static_cast<uint32_t>(sub_start - seg_start);
              }
              uint32_t sub_size = static_cast<uint32_t>(sub_end - sub_start);
              data_subsegments.push_back({sub_vram, sub_vram + sub_size, label_for_vram(sub_vram)});
            } else if (sub.type() == SUBSEGMENT_BSS && sub.vram() != 0 && seg.bss_size() != 0) {
              data_subsegments.push_back(
                  {sub.vram(), sub.vram() + seg.bss_size(), label_for_vram(sub.vram())});
            }
          }
        }
      }
    }

    // Returns "<label> + 0xOFF" (or "<label>") for an address inside a relocatable data/BSS
    // range, or nullopt if the address is not inside any such range.
    auto relative_data_expr = [&](uint32_t addr) -> std::optional<std::pair<std::string, bool>> {
      for (const auto& ds : data_subsegments) {
        if (addr >= ds.vram_start && addr < ds.vram_end) {
          uint32_t offset = addr - ds.vram_start;
          if (offset == 0) return std::make_pair(ds.label_name, true);
          return std::make_pair(absl::StrFormat("%s + 0x%X", ds.label_name, offset), false);
        }
      }
      return std::nullopt;
    };

    for (const auto& [symbol_name, address_hex] : undefined_auto_symbols) {
      bool resolved = false;
      uint32_t addr = 0;
      if (absl::SimpleHexAtoi(address_hex, &addr)) {
        if (absl::StartsWith(symbol_name, "func_") && loader_ != nullptr) {
          // If this is an unexported internal code label or sub-entrypoint inside a known function,
          // resolve it relative to the preceding registered or defined function symbol.
          auto it = all_known_functions.upper_bound(addr);
          if (it != all_known_functions.begin()) {
            const auto& prev = *std::prev(it);
            auto prev_loaded = loader_->LoadFunction(prev.second);
            if (prev_loaded.ok()) {
              uint32_t prev_end =
                  prev.first + static_cast<uint32_t>(prev_loaded->instructions.size() * 4);
              if (addr > prev.first && addr < prev_end &&
                  defined_function_names.contains(prev.second)) {
                uint32_t offset = addr - prev.first;
                symbols_output_stream << absl::StrFormat("PROVIDE(%s = %s + 0x%X);\n", symbol_name,
                                                         prev.second, offset);
                resolved = true;
              }
            }
          }
        } else if (absl::StartsWith(symbol_name, "D_")) {
          if (auto rel = relative_data_expr(addr); rel.has_value()) {
            // A symbol that *is* the base label is already defined by the data/BSS object.
            if (!(rel->second && rel->first == symbol_name)) {
              symbols_output_stream << absl::StrFormat("%s = %s;\n", symbol_name, rel->first);
            }
            resolved = true;
          }
        }
      }
      if (!resolved) {
        symbols_output_stream << absl::StrFormat("PROVIDE(%s = 0x%s);\n", symbol_name, address_hex);
      }
    }

    // Registered (named) data symbols are PROVIDEd at their original absolute address by the
    // splitter's symbols.ld, which is only correct when nothing before them changes size. Re-anchor
    // every named data symbol that lives inside a data/BSS blob to that blob's base label. A plain
    // assignment is used because, in GNU ld, it overrides a PROVIDE regardless of script order.
    if (symbols != nullptr) {
      for (const auto* entry : symbols->SortedEntries()) {
        if (entry->type() != SYMBOL_DATA) continue;
        auto rel = relative_data_expr(entry->address());
        if (!rel.has_value()) continue;
        if (rel->second && rel->first == entry->name()) continue;  // This is the base label.
        symbols_output_stream << absl::StrFormat("%s = %s;\n", entry->name(), rel->first);
      }
    }
  }

  return emitted_files;
}

}  // namespace rom_nom_nom
