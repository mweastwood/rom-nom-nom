#ifndef HARVEST_MOON_64_LIBULTRA_GU_MTX_H
#define HARVEST_MOON_64_LIBULTRA_GU_MTX_H

#include "types.h"

typedef f32 MtxF[4][4];

void guMtxF2L(MtxF mf, Mtx* m);
void guMtxL2F(MtxF mf, Mtx* m);
void guMtxIdentF(MtxF mf);
void guMtxIdent(Mtx* m);

#endif
