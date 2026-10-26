#ifndef ROM_NOM_NOM_LIFTER_AST_EXPRESSION_FOLDER_H_
#define ROM_NOM_NOM_LIFTER_AST_EXPRESSION_FOLDER_H_

#include <string>
#include <string_view>

#include "absl/container/flat_hash_set.h"
#include "core/c_ast.h"
#include "lifter/ast_pass_pipeline.h"

namespace rom_nom_nom {

// Folds single-use scratch register, argument register, and stack argument assignments into
// subsequent expressions, branch conditions, return statements, and switch selectors, and
// eliminates dead pre-call/overwritten scratch assignments.
void FoldSingleUseExpressions(CompoundStatement* comp, const CompoundStatement* root_body = nullptr,
                              const absl::flat_hash_set<std::string>* dead_at_end_vars = nullptr,
                              bool is_switch_case_body = false, int root_has_label_cached = -1);

class SingleUseExpressionFolderPass : public AstPass {
 public:
  std::string_view Name() const override { return "FoldSingleUseExpressions"; }
  bool Run(CompoundStatement* body, const AstPassContext& ctx) override;

  // Returns true if `load_expr` and `store_lhs` may alias according to GCC 2.7.2 `sched.c`
  // (`true_dependence` / `anti_dependence`): stack variables (`var_sp_*`, `sp`) never alias global
  // symbols (`D_*`, `g_*`, `RO_*`, `B_*`), distinct global symbols never alias, and distinct
  // non-overlapping constant byte ranges `*(T*)((u8*)base + C1)` vs `*(U*)((u8*)base + C2)` from
  // the same base pointer never alias.
  static bool MayAlias(const CExpression& load_expr, const CExpression& store_lhs);
};

}  // namespace rom_nom_nom

#endif  // ROM_NOM_NOM_LIFTER_AST_EXPRESSION_FOLDER_H_
