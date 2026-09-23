#include "common.h"

typedef struct {
  u32 unk_00[6];
  u32 unk_18;
  u32 unk_1C;
  u32 unk_20;
  u16 unk_24;
  u16 unk_26;
  s16 unk_28;
  s16 unk_2A;
  u8 pad[6];
  u16 unk_32;
  u8 pad2[0xF4 - 0x34];
} TextBox;

extern TextBox D_801C3F00[];

extern s32 D_801C3F18;
extern s32 D_801C3F1C;
extern s32 D_801C3F20;
extern s16 D_801C3F28;
extern s16 D_801C3F2A;
extern u16 D_801C3F32;

typedef struct {
  s32 unk_00;
  s32 unk_04;
  s32 unk_08;
  s32 unk_0C;
  s32 unk_10;
  s16 unk_14;
  s16 unk_16;
} MessageEntry_80205760;

extern MessageEntry_80205760 D_80205760[];

void func_80042F60(void) {
  u16 i;

  for (i = 0; i < 1; i++) {
    D_801C3F00[i].unk_24 = 0;
    D_801C3F00[i].unk_26 = 0;
    D_801C3F00[i].unk_28 = 0;
    D_801C3F00[i].unk_18 = 0xFF;
    D_801C3F00[i].unk_1C = 0xFF;
    D_801C3F00[i].unk_20 = 0xFF;
    D_801C3F00[i].unk_32 = 0;
  }
}

s32 func_80042FEC(s32 arg0, s16 arg1, s16 arg2) {
  s32 ret = 0;

  if (!(arg0 & 0xFFFF) && !(D_801C3F32 & 1)) {
    ret = 1;
    D_801C3F18 = 0xFF;
    D_801C3F1C = 0xFF;
    D_801C3F20 = 0xFF;
    D_801C3F28 = arg1;
    D_801C3F2A = arg2;
    D_801C3F32 = 1;
  }
  return ret;
}

s32 func_80043050(u16 index, s16 arg1, s16 arg2, s32 arg3, s32 arg4, s32 arg5, s32 arg6, s32 arg7) {
  s32 ret = 0;

  if (index < 0x46) {
    ret = 1;
    D_80205760[index].unk_14 = arg1;
    D_80205760[index].unk_16 = arg2;
    D_80205760[index].unk_00 = arg3;
    D_80205760[index].unk_04 = arg4;
    D_80205760[index].unk_08 = arg5;
    D_80205760[index].unk_0C = arg6;
    D_80205760[index].unk_10 = arg7;
  }
  return ret;
}

__asm__(".include \"asm/harvest-moon-64/1E4DC.s\"");
