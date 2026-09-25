#ifndef LIBULTRA_GU_TRANSLATE_H
#define LIBULTRA_GU_TRANSLATE_H

#include "src/c/harvest-moon-64/libultra_gu_mtx.h"

void guTranslateF(MtxF mf, f32 x, f32 y, f32 z);
void guTranslate(Mtx* m, f32 x, f32 y, f32 z);

#endif  // LIBULTRA_GU_TRANSLATE_H
