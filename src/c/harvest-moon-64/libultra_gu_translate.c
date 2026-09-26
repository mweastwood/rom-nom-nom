#include "libultra_gu_translate.h"

inline void GuTranslateF(MtxF mf, f32 x, f32 y, f32 z) {
  GuMtxIdentF(mf);
  mf[3][0] = x;
  mf[3][1] = y;
  mf[3][2] = z;
}

void GuTranslate(Mtx* m, f32 x, f32 y, f32 z) {
  MtxF mf;

  GuTranslateF(mf, x, y, z);
  GuMtxF2L(mf, m);
}
