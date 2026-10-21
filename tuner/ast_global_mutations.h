#ifndef TUNER_AST_GLOBAL_MUTATIONS_H_
#define TUNER_AST_GLOBAL_MUTATIONS_H_

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/c_ast.h"

namespace rom_nom_nom {

// Matches `*(T *)&symbol` where T is a scalar type (`u8`, `s8`, `u16`, `s16`, `u32`, `s32`, `f32`,
// `f64`).
bool ExtractGlobalCastTarget(const CExpression& expression, std::string* out_symbol = nullptr,
                             CType* out_type = nullptr);

// Global symbol that is only ever accessed via `*(T *)&symbol` with a consistent width.
struct DirectTypedGlobalCandidate {
  std::string symbol_name;
  CType resolved_type;
};

// Collects global symbols that are only ever accessed via `*(T *)&symbol` with a consistent width.
std::vector<DirectTypedGlobalCandidate> CollectDirectTypedGlobalCandidates(
    const FunctionDeclaration& function, const ReferencedSymbols& referenced_symbols);

// Rewrites `*(T *)&symbol` accesses for `target_globals` into direct typed identifier references.
std::unique_ptr<CStatement> RewriteDirectTypedGlobalInStatement(
    const CStatement& statement, const std::map<std::string, CType>& target_globals);

// Collects 32-bit global symbols eligible for array promotion (`symbol` -> `symbol[0]`).
std::vector<std::string> Collect32BitGlobalCandidates(const FunctionDeclaration& function,
                                                      const ReferencedSymbols& referenced_symbols,
                                                      bool non_zero_or_pointer_only);

// Collects single-assignment local temporaries initialized as `ptr = &symbol;` for
// `target_globals`.
std::map<std::string, std::string> CollectSingleAssignmentGlobalAddressTemporaries(
    const FunctionDeclaration& function, const std::set<std::string>& target_globals);

// Rewrites uses of `pointer_variable` (where `pointer_variable = &symbol;`) and 32-bit global
// `symbol` into `symbol[index]` / `&symbol[index]`.
std::unique_ptr<CStatement> RewriteGlobalArrayInStatement(
    const CStatement& statement, const std::set<std::string>& array_globals,
    const std::map<std::string, std::string>& address_temporary_to_symbol);

// Collects global symbols assigned multiple times without intervening reads or calls.
std::vector<std::string> CollectVolatileGlobalCandidates(
    const FunctionDeclaration& function, const ReferencedSymbols& referenced_symbols);

// Rewrites assignments to `target_globals` as `volatile` global accesses.
std::unique_ptr<CStatement> RewriteVolatileGlobalAccessInStatement(
    const CStatement& statement, const std::set<std::string>& target_globals,
    const ReferencedSymbols& referenced_symbols, bool use_inline_cast = false);

}  // namespace rom_nom_nom

#endif  // TUNER_AST_GLOBAL_MUTATIONS_H_
