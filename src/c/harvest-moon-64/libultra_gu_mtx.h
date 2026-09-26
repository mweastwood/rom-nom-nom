#ifndef HARVEST_MOON_64_LIBULTRA_GU_MTX_H
#define HARVEST_MOON_64_LIBULTRA_GU_MTX_H

#include "types.h"

typedef f32 MtxF[4][4];

void GuMtxF2L(MtxF mf, Mtx* m);
void GuMtxL2F(MtxF mf, Mtx* m);
void GuMtxIdentF(MtxF mf);
void GuMtxIdent(Mtx* m);

#endif
