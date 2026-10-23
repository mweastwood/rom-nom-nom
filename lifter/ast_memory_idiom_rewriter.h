#ifndef LIFTER_AST_MEMORY_IDIOM_REWRITER_H_
#define LIFTER_AST_MEMORY_IDIOM_REWRITER_H_

#include "core/c_ast.h"

namespace rom_nom_nom {

// Collapses sequences of unaligned load/byte-store statements into typed
// `UnalignedWord` / `UnalignedBlock6` / `UnalignedBlock8` / `UnalignedBlock12`
// / `UnalignedBlock16` struct copies.
void CollapseUnalignedBlockCopies(CompoundStatement* comp);

// Folds `-O0` frame-pointer scratch evaluation sequences (while-loop header conditions,
// post-increment pointer dereferences, local update chains, and dead constant loads) and
// rewrites positive constant-offset pointer dereferences `*(TYPE*)(base + offset)` into
// anonymous struct member accesses `((struct { u8 _p[offset]; TYPE _v; }*)base)->_v` so
// GCC 2.7.2 at `-O0` emits single-instruction `offset(base)` loads and stores.
void RewriteFramePointerStructOffsetAccesses(CompoundStatement* body);

}  // namespace rom_nom_nom

#endif  // LIFTER_AST_MEMORY_IDIOM_REWRITER_H_
