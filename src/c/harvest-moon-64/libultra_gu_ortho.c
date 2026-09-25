#include "libultra_gu_ortho.h"

inline void guOrthoF(MtxF mf, f32 l, f32 r, f32 b, f32 t, f32 n, f32 f, f32 scale) {
  int i, j;
  f32 delx, dely, delz;

  guMtxIdentF(mf);

  delx = r - l;
  dely = t - b;
  delz = f - n;

  mf[0][0] = 2.0f / delx;
  mf[1][1] = 2.0f / dely;
  mf[2][2] = -2.0f / delz;
  mf[3][0] = -(r + l) / delx;
  mf[3][1] = -(t + b) / dely;
  mf[3][2] = -(f + n) / delz;
  mf[3][3] = 1.0f;

  for (i = 0; i < 4; i++) {
    for (j = 0; j < 4; j++) {
      mf[i][j] *= scale;
    }
  }
}

void guOrtho(Mtx* m, f32 l, f32 r, f32 b, f32 t, f32 n, f32 f, f32 scale) {
  MtxF mf;

  guOrthoF(mf, l, r, b, t, n, f, scale);
  guMtxF2L(mf, m);
}
