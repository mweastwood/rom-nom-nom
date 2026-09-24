#include "layer.h"

void LayerInit(void) {
  u16 i = 0;
  do {
    D_80189A50[i].unk_38 = 0;
    D_80189A50[i].flags = 0;
    i++;
  } while (i < 16);
}

void func_80045DE0(void) __attribute__((alias("LayerInit")));

s32 LayerSet(u16 index, s16 arg1, u32 arg2, u32 arg3, u32 arg4, u32 arg5, u32 arg6, u32 arg7,
             u32 arg8, u32 arg9, u32 arg10, u16 arg11, u8 arg12, f32 arg13, f32 arg14, f32 arg15,
             u8 arg16) {
  s32 success = 0;

  if (index < 16) {
    if (!(D_80189A50[index].flags & 1)) {
      success = 1;
      D_80189A50[index].unk_0 = arg2;
      D_80189A50[index].unk_4 = arg3;
      D_80189A50[index].unk_8 = arg4;
      D_80189A50[index].unk_C = arg5;
      D_80189A50[index].unk_10 = arg6;
      D_80189A50[index].unk_14 = arg7;
      D_80189A50[index].unk_18 = arg8;
      D_80189A50[index].unk_1C = arg9;
      D_80189A50[index].unk_20 = arg10;
      D_80189A50[index].unk_24 = arg11;
      D_80189A50[index].unk_26 = arg12;
      D_80189A50[index].unk_28 = arg13;
      D_80189A50[index].unk_2C = arg14;
      D_80189A50[index].unk_30 = arg15;
      D_80189A50[index].unk_27 = arg16;
      D_80189A50[index].unk_38 = arg1;
      D_80189A50[index].flags = 1;
    }
  }

  return success;
}

s32 func_80045E20(u16 index, s16 arg1, u32 arg2, u32 arg3, u32 arg4, u32 arg5, u32 arg6, u32 arg7,
                  u32 arg8, u32 arg9, u32 arg10, u16 arg11, u8 arg12, f32 arg13, f32 arg14,
                  f32 arg15, u8 arg16) __attribute__((alias("LayerSet")));

void func_8002B138(u16, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32);
s32 func_800461D8(u16, u8, u8, u8, u8);

s32 LayerActivate(u16 index, u32 arg1, s8 arg2, u16 arg3) {
  s32 success = 0;
  u8 count;

  if (index < 16) {
    if (D_80189A50[index].flags & 1) {
      D_80189A50[index].unk_3A = arg2;
      count = D_80189A50[index].unk_3A;
      D_80189A50[index].unk_34 = arg1;
      D_80189A50[index].flags |= 2;
      do {
        func_8002B138(D_80189A50[index].unk_38 + count, D_80189A50[index].unk_0,
                      D_80189A50[index].unk_4, D_80189A50[index].unk_8, D_80189A50[index].unk_C, 0,
                      0, D_80189A50[index].unk_10, 0, D_80189A50[index].unk_14,
                      D_80189A50[index].unk_18, D_80189A50[index].unk_1C, D_80189A50[index].unk_20,
                      0, 0);
      } while (count--);
      func_800461D8(index, 0xFF, 0xFF, 0xFF, 0xFF);

      success = 1;
      if (arg3 == 3) {
        D_80189A50[index].flags |= 4;
      }
    }
  }

  return success;
}

s32 func_80045F5C(u16 index, u32 arg1, s8 arg2, u16 arg3) __attribute__((alias("LayerActivate")));
