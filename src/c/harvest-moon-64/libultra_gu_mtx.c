#include "libultra_gu_mtx.h"

#include "include_asm.h"

inline void guMtxF2L(MtxF mf, Mtx* m) {
  int i, j;
  s32 e1, e2;
  s32* ai = (s32*)&m->m[0][0];
  s32* af = (s32*)&m->m[2][0];

  for (i = 0; i < 4; i++) {
    for (j = 0; j < 2; j++) {
      e1 = (s32)(mf[i][j * 2] * 65536.0f);
      e2 = (s32)(mf[i][j * 2 + 1] * 65536.0f);
      *ai++ = (e1 & 0xFFFF0000) | ((u32)e2 >> 16);
      *af++ = ((e1 << 16) & 0xFFFF0000) | (e2 & 0xFFFF);
    }
  }
}

void guMtxL2F(MtxF mf, Mtx* m) {
  int i, j;
  int q[2];
  f32 f1, f2;
  u32* ai = (u32*)&m->m[0][0];
  u16* af = (u16*)&m->m[2][0];

  for (i = 0; i < 4; i++) {
    for (j = 0; j < 2; j++) {
      q[0] = (*ai & 0xFFFF0000) | af[0];
      q[1] = ((*ai << 16) & 0xFFFF0000) | af[1];
      af += 2;
      ai++;
      f1 = q[0] * (1.0f / 65536.0f);
      f2 = q[1] * (1.0f / 65536.0f);
      mf[i][j * 2] = f1;
      mf[i][j * 2 + 1] = f2;
    }
  }
}

inline void guMtxIdentF(MtxF mf) {
  int i, j;

  for (i = 0; i < 4; i++) {
    for (j = 0; j < 4; j++) {
      if (i == j) {
        mf[i][j] = 1.0f;
      } else {
        mf[i][j] = 0.0f;
      }
    }
  }
}

void guMtxIdent(Mtx* m) {
  MtxF mf;
  guMtxIdentF(mf);
  guMtxF2L(mf, m);
}
