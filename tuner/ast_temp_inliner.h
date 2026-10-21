#ifndef TUNER_AST_TEMP_INLINER_H_
#define TUNER_AST_TEMP_INLINER_H_

#include <cstddef>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "core/c_ast.h"

namespace rom_nom_nom {

// Returns true if `statement` reads `variable_name` before unconditionally overwriting it.
bool DoesStatementReadVariableBeforeOverwrite(const CStatement& statement,
                                              std::string_view variable_name,
                                              bool* out_always_overwrites = nullptr);

// Returns true if `variable_name` is dead after `statements[after_index]`.
bool IsVariableDeadAfter(const std::vector<std::unique_ptr<CStatement>>& statements,
                         size_t after_index, std::string_view variable_name,
                         bool is_top_level_block, const CompoundStatement* root_body = nullptr,
                         int max_dead_occurrences = 1);

// Replaces occurrences of `target_name` with `replacement` inside `expression`.
std::unique_ptr<CExpression> ReplaceIdentifierInExpression(const CExpression& expression,
                                                           std::string_view target_name,
                                                           const CExpression& replacement,
                                                           bool replace_assignment_lhs = false);

// Replaces occurrences of `target_name` with `replacement` in the top-level expression/condition
// of a single statement (`ExpressionStatement`, `ReturnStatement`, or `IfStatement` condition).
std::unique_ptr<CStatement> ReplaceIdentifierInSingleStatement(const CStatement& statement,
                                                               std::string_view target_name,
                                                               const CExpression& replacement);

// Recursively replaces occurrences of `target_name` with `replacement` across `statement`.
std::unique_ptr<CStatement> ReplaceIdentifierInStatement(const CStatement& statement,
                                                         std::string_view target_name,
                                                         const CExpression& replacement,
                                                         bool replace_assignment_lhs = false);

// Returns true if `statement` assigns or increments/decrements `name`.
bool IsIdentifierAssignedInStatement(const CStatement& statement, std::string_view name);

struct InlineCandidateInfo {
  size_t use_index = 0;
  std::string variable_name;
  const CExpression* rhs = nullptr;
  bool can_sink_to_use = false;
  bool can_hoist_to_def = false;
};

// Checks whether `statements[index]` is a single-use temporary assignment that can be inlined
// into a subsequent statement `statements[use_index]`.
bool AnalyzeInlineSingleUseTemporaryAt(const std::vector<std::unique_ptr<CStatement>>& statements,
                                       size_t index, bool is_top_level_block,
                                       InlineCandidateInfo* out_info,
                                       const CompoundStatement* root_body = nullptr);

// Returns true if `statements[index]` and `statements[index + 1]` are independent pure assignments
// that can be safely swapped.
bool CanSwapAdjacentStatements(const std::vector<std::unique_ptr<CStatement>>& statements,
                               size_t index);

// Returns true if `statements[index]` is a dead local variable assignment (or dead call return).
bool IsDeadAssignmentAt(const std::vector<std::unique_ptr<CStatement>>& statements, size_t index,
                        bool is_top_level_block, const std::set<std::string>& local_variables,
                        bool* out_is_call = nullptr, const CompoundStatement* root_body = nullptr);

// Prunes unused local declarations and dead variable initializers from `body`.
std::unique_ptr<CompoundStatement> PruneUnusedLocalDeclarations(
    const CompoundStatement& body, bool preserve_trailing_return = true);

struct ParameterCopyCandidate {
  size_t statement_index = 0;
  std::string temporary_variable;
  std::string parameter_name;
};

// Collects prologue parameter copy candidates (`temp_sX = argY;`).
std::vector<ParameterCopyCandidate> CollectParameterCopyCandidates(
    const FunctionDeclaration& function);

// Coalesces `candidate.temporary_variable` back into `candidate.parameter_name`.
std::unique_ptr<CompoundStatement> ApplyCoalesceParameterCopy(
    const FunctionDeclaration& function, const ParameterCopyCandidate& candidate);

// Iteratively inlines single-use temporaries, folds shift-add multiplications, and prunes unused
// local variable declarations across `body`.
std::unique_ptr<CompoundStatement> ApplyBulkInlineTemporaries(const CompoundStatement& body,
                                                              bool prefer_hoist);

}  // namespace rom_nom_nom

#endif  // TUNER_AST_TEMP_INLINER_H_
