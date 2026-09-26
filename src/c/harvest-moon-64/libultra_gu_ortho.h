#ifndef LIBULTRA_GU_ORTHO_H
#define LIBULTRA_GU_ORTHO_H

#include "src/c/harvest-moon-64/libultra_gu_mtx.h"

void GuOrthoF(MtxF mf, f32 l, f32 r, f32 b, f32 t, f32 n, f32 f, f32 scale);
void GuOrtho(Mtx* m, f32 l, f32 r, f32 b, f32 t, f32 n, f32 f, f32 scale);

#endif  // LIBULTRA_GU_ORTHO_H
