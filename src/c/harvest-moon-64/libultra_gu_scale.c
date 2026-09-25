#include "libultra_gu_scale.h"

inline void guScaleF(MtxF mf, f32 x, f32 y, f32 z) {
  guMtxIdentF(mf);
  mf[0][0] = x;
  mf[1][1] = y;
  mf[2][2] = z;
  mf[3][3] = 1.0f;
}

void guScale(Mtx* m, f32 x, f32 y, f32 z) {
  MtxF mf;

  guScaleF(mf, x, y, z);
  guMtxF2L(mf, m);
}

__asm__(".align 4");
