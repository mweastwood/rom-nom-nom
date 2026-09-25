#include "libultra_gu_translate.h"

inline void guTranslateF(MtxF mf, f32 x, f32 y, f32 z) {
  guMtxIdentF(mf);
  mf[3][0] = x;
  mf[3][1] = y;
  mf[3][2] = z;
}

void guTranslate(Mtx* m, f32 x, f32 y, f32 z) {
  MtxF mf;

  guTranslateF(mf, x, y, z);
  guMtxF2L(mf, m);
}

__asm__(".align 4");
