#include "libultra_gu_scale.h"

inline void GuScaleF(MtxF mf, f32 x, f32 y, f32 z) {
  GuMtxIdentF(mf);
  mf[0][0] = x;
  mf[1][1] = y;
  mf[2][2] = z;
  mf[3][3] = 1.0f;
}

void GuScale(Mtx* m, f32 x, f32 y, f32 z) {
  MtxF mf;

  GuScaleF(mf, x, y, z);
  GuMtxF2L(mf, m);
}
