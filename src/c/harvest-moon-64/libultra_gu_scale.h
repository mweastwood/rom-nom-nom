#ifndef LIBULTRA_GU_SCALE_H
#define LIBULTRA_GU_SCALE_H

#include "src/c/harvest-moon-64/libultra_gu_mtx.h"

void guScaleF(MtxF mf, f32 x, f32 y, f32 z);
void guScale(Mtx* m, f32 x, f32 y, f32 z);

#endif  // LIBULTRA_GU_SCALE_H
