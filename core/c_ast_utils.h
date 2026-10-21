#ifndef ROM_NOM_NOM_CORE_C_AST_UTILS_H_
#define ROM_NOM_NOM_CORE_C_AST_UTILS_H_

#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "core/c_ast.h"

namespace rom_nom_nom {

// Extracts the underlying callee identifier from a function-pointer cast callee string such as
// `((void (*)())func_ptr)`.
bool ExtractFunctionPointerCastCalleeIdentifier(std::string_view text,
                                                std::string_view* out_prefix = nullptr,
                                                std::string_view* out_identifier = nullptr);

// Returns true if `expression` has no side effects (no calls, assignments, or++/--).
bool IsPureExpression(const CExpression& expression);

// Returns the commuted binary operator string (e.g. `<` -> `>`, `+` -> `+`), or std::nullopt.
std::optional<std::string> CommutedBinaryOperator(std::string_view binary_operator);

// Returns true if `binary` can be safely commuted without changing evaluation semantics.
bool IsCommutableBinary(const BinaryExpression& binary);

// Returns a negated clone of `condition` (e.g. `a == b` -> `a != b`, `!x` -> `x`).
std::unique_ptr<CExpression> NegateCondition(const CExpression& condition);

// Returns the byte size of a fixed-width scalar type name (`u8`, `s16`, `s32`, `f32`, `f64`, etc.).
std::optional<int64_t> TypeByteSize(std::string_view type_name);

// Returns true if `call` is a built-in inline-assembly math intrinsic (`N64Sqrtf`, `fabsf`, etc.).
bool IsInlineAsmIntrinsicCall(const CallExpression& call);

// Returns true if `expr` or `stmt` contains a function call. When `ignore_math_intrinsics` is true,
// inline-assembly math intrinsics (`N64Sqrtf`, `fabsf`, etc.) are not counted as calls.
bool ExpressionContainsCall(const CExpression& expr, bool ignore_math_intrinsics = false);
bool StatementContainsCall(const CStatement& stmt, bool ignore_math_intrinsics = false);

// Counts occurrences of identifier `name` in `expression` or `statement`.
int CountIdentifierOccurrencesInExpression(const CExpression& expression, std::string_view name);
int CountIdentifierOccurrencesInStatement(const CStatement& statement, std::string_view name);

// Returns true if `expr` or `stmt` references `target_var`.
bool ExpressionUsesVariable(const CExpression& expr, std::string_view target_var);
bool StatementReferencesVariable(const CStatement& stmt, std::string_view target_var);

// Collects all variable identifiers referenced in `expr` or `stmt` in encounter order.
void CollectVariablesInExpression(const CExpression& expr, std::vector<std::string>* vars,
                                  absl::flat_hash_set<std::string>* seen);
void CollectUsedVariables(const CStatement& stmt, std::vector<std::string>* vars,
                          absl::flat_hash_set<std::string>* seen);
void CollectIdentifiersInExpression(const CExpression& expression, std::set<std::string>* out);

// Returns true if `stmt` contains a `return expr;` statement with a non-null return value.
bool HasReturnValue(const CStatement& stmt);

// Substitutes `target` with `replacement` in `expr` or `stmt` using lifter substitution semantics.
std::unique_ptr<CExpression> SubstituteIdentifier(const CExpression& expr, std::string_view target,
                                                  const CExpression& replacement);
std::unique_ptr<CStatement> SubstituteIdentifierInStatement(std::unique_ptr<CStatement> stmt,
                                                            std::string_view from,
                                                            std::string_view to);

// Invokes `visitor` on each immediate child `CompoundStatement` of `stmt`.
void ForEachSubCompoundStatement(CStatement* stmt,
                                 const std::function<void(CompoundStatement*)>& visitor);

}  // namespace rom_nom_nom

#endif  // ROM_NOM_NOM_CORE_C_AST_UTILS_H_
