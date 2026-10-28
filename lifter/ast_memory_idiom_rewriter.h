#ifndef LIFTER_AST_MEMORY_IDIOM_REWRITER_H_
#define LIFTER_AST_MEMORY_IDIOM_REWRITER_H_

#include <string_view>
#include <vector>

#include "core/c_ast.h"
#include "lifter/ast_pass_pipeline.h"

namespace rom_nom_nom {

// Collapses sequences of unaligned load/byte-store statements (`lwl`/`lwr` +
// `swl`/`swr` or `ldl`/`ldr` + `sdl`/`sdr` plus optional scalar tail copies)
// into typed `UnalignedWord` / `UnalignedBlock<N>` struct copies.
void CollapseUnalignedBlockCopies(CompoundStatement* comp);

// Folds `-O0` frame-pointer scratch evaluation sequences (while-loop header conditions,
// post-increment pointer dereferences, local update chains, and dead constant loads) and
// rewrites positive constant-offset pointer dereferences `*(TYPE*)(base + offset)` into
// anonymous struct member accesses `((struct { u8 _p[offset]; TYPE _v; }*)base)->_v` so
// GCC 2.7.2 at `-O0` emits single-instruction `offset(base)` loads and stores.
void RewriteFramePointerStructOffsetAccesses(CompoundStatement* body,
                                             const std::vector<CParameter>* parameters = nullptr);

class UnalignedBlockCopyCollapserPass : public AstPass {
 public:
  std::string_view Name() const override { return "CollapseUnalignedBlockCopies"; }
  bool Run(CompoundStatement* body, const AstPassContext& ctx) override;
};

class FramePointerStructOffsetRewriterPass : public AstPass {
 public:
  std::string_view Name() const override { return "RewriteFramePointerStructOffsetAccesses"; }
  bool Run(CompoundStatement* body, const AstPassContext& ctx) override;
};

}  // namespace rom_nom_nom

#endif  // LIFTER_AST_MEMORY_IDIOM_REWRITER_H_
