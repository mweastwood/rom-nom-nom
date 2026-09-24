#ifndef HARVEST_MOON_64_LAYER_H
#define HARVEST_MOON_64_LAYER_H

#include "types.h"

typedef struct {
  u32 unk_0;
  u32 unk_4;
  u32 unk_8;
  u32 unk_C;
  u32 unk_10;
  u32 unk_14;
  u32 unk_18;
  u32 unk_1C;
  u32 unk_20;
  u16 unk_24;
  u8 unk_26;
  u8 unk_27;
  f32 unk_28;
  f32 unk_2C;
  f32 unk_30;
  u32 unk_34;
  u16 unk_38;
  u32 unk_3A : 8;
  u32 pad_3b : 8;
  u16 flags;
  u16 pad_3e;
} LayerEntry;

extern LayerEntry D_80189A50[16];

void LayerInit(void);
s32 LayerSet(u16 index, s16 arg1, u32 arg2, u32 arg3, u32 arg4, u32 arg5, u32 arg6, u32 arg7,
             u32 arg8, u32 arg9, u32 arg10, u16 arg11, u8 arg12, f32 arg13, f32 arg14, f32 arg15,
             u8 arg16);
s32 LayerActivate(u16 index, u32 arg1, s8 arg2, u16 arg3);

#endif
