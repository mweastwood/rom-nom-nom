#ifndef LIFTER_AST_CODEGEN_LOWERER_H_
#define LIFTER_AST_CODEGEN_LOWERER_H_

#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "core/c_ast.h"
#include "core/symbol_registry.h"

namespace rom_nom_nom {

// Pre-analyzed module-level symbol classification and scalar-global promotion tables so
// single-function candidate emissions during tuning use the exact same global types and
// array/scalar normalization rules as the parent multi-function module.
struct ModuleSymbolContext {
  std::set<std::string> defined_function_names;
  std::set<std::string> external_function_names;
  std::set<std::string> external_data_symbols;
  std::set<std::string> shared_external_data_symbols;
  std::map<std::string, CType> inferred_return_types;
  std::map<std::string, CType> inferred_data_types;
  std::set<std::string> array_data_symbols;
  std::set<std::string> volatile_data_symbols;
  std::map<std::string, CType> promoted_scalar_globals;
};

// Result of analyzing and lowering all functions in a translation unit for GCC 2.7.2 codegen.
struct LoweredTranslationUnit {
  std::vector<FunctionDeclaration> functions;
  std::set<std::string> external_function_names;
  std::set<std::string> external_data_symbols;
  std::map<std::string, CType> inferred_return_types;
  std::map<std::string, CType> inferred_data_types;
  std::set<std::string> array_data_symbols;
  std::set<std::string> volatile_data_symbols;
};

// Lowers high-level lifted C ASTs into GCC 2.7.2-compatible codegen ASTs by resolving
// translation-unit-wide symbol types, promoting scalar cast-deref globals, inserting
// targeted RTL CSE/reorg inline-asm barriers, and lowering float/GPR bitcasts.
class AstCodegenLowerer {
 public:
  // Returns true if `name` is an internal compiler intrinsic (`__n64_*` or `__builtin_*`).
  static bool IsCompilerIntrinsic(std::string_view name);

  // Returns true if `body` uses O2 register-pinned locals or a raw `sp` stack array.
  static bool IsO2FunctionBody(const CompoundStatement& body);

  // Returns true if `function` should be lowered using O2 rules (honoring explicit
  // `FunctionOptimizationMode` when present, or falling back to `IsO2FunctionBody`).
  static bool IsO2Function(const FunctionDeclaration& function);

  // Disambiguates duplicate callee-saved symbol loads and CSE-prone callee-saved copies across
  // calls by inserting no-op inline-asm register barriers where GCC 2.7.2's `cse.c` would
  // otherwise conflate registers with caller-saved temporaries while preserving `reorg.c` delay
  // slot filling.
  static std::unique_ptr<CompoundStatement> DisambiguateCalleeSavedAndGlobalLoads(
      const CompoundStatement& body, const std::set<std::string>& external_data_symbols,
      bool has_calls, bool suppress_all_cse_barriers = false,
      const std::set<std::string>& suppressed_barrier_vars = {});

  // Normalizes global array symbol references, resolves cross-function global type conflicts, and
  // rewrites promoted scalar cast-dereference globals into direct typed symbol accesses.
  static std::unique_ptr<CompoundStatement> NormalizeGlobalAndArraySymbols(
      const CompoundStatement& body, const std::set<std::string>& array_symbols,
      const std::set<std::string>& local_vars,
      const std::map<std::string, CType>& module_data_types,
      const std::set<std::string>& all_function_names,
      const std::map<std::string, CType>& promoted_scalar_globals = {});

  // Prevents GCC 2.7.2 CSE from hoisting duplicate address-taken global symbols passed as call
  // arguments into shared temporaries when the original code loaded them directly into `$a0..$a3`.
  static std::unique_ptr<CompoundStatement> PreventDuplicateCallArgSymbolCse(
      const CompoundStatement& body, const std::set<std::string>& dup_addr_symbols);

  // Lowers `*(s32*)&fN` and `*(f32*)&gpr` bitcasts in O2 functions to direct `mfc1`/`mtc1` inline
  // assembly so GCC does not spill through stack slots.
  static std::unique_ptr<CompoundStatement> LowerFloatGprBitcastsToInlineAsm(
      const CompoundStatement& original_body, const CompoundStatement& current_body);

  // Pre-analyzes all functions in a module to compute module-wide symbol classifications, inferred
  // data/return types, array symbols, and promoted scalar globals.
  static ModuleSymbolContext AnalyzeModuleSymbols(
      const std::vector<FunctionDeclaration>& functions, const SymbolIndex* symbol_index,
      const std::set<std::string>& header_provided_functions);

  // Analyzes all functions in a translation unit, infers and promotes global symbol types, and
  // lowers each function body for C emission. When `parent_module_context` is provided, uses the
  // parent module's symbol classifications and promoted scalar globals so single-function TUs
  // lower identically to the full module.
  static LoweredTranslationUnit LowerTranslationUnit(
      const std::vector<FunctionDeclaration>& functions, const SymbolIndex* symbol_index,
      const std::set<std::string>& header_provided_functions,
      const ModuleSymbolContext* parent_module_context = nullptr);
};

}  // namespace rom_nom_nom

#endif  // LIFTER_AST_CODEGEN_LOWERER_H_
