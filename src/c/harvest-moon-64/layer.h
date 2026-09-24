#ifndef HARVEST_MOON_64_LAYER_H
#define HARVEST_MOON_64_LAYER_H

#include "types.h"

typedef struct {
  u32 param_0;
  u32 param_1;
  u32 param_2;
  u32 param_3;
  u32 param_4;
  u32 param_5;
  u32 param_6;
  u32 param_7;
  u32 param_8;
  u16 frame_index;
  u8 alpha;
  u8 blend_mode;
  f32 scale_x;
  f32 scale_y;
  f32 scale_z;
  u32 callback;
  u16 base_index;
  u32 offset_index : 8;
  u32 reserved_bits : 8;
  u16 flags;
  u16 reserved;
} LayerEntry;

extern LayerEntry D_80189A50[16];

void LayerInit(void);
s32 LayerSet(u16 index, s16 arg1, u32 arg2, u32 arg3, u32 arg4, u32 arg5, u32 arg6, u32 arg7,
             u32 arg8, u32 arg9, u32 arg10, u16 arg11, u8 arg12, f32 arg13, f32 arg14, f32 arg15,
             u8 arg16);
s32 LayerActivate(u16 index, u32 arg1, s8 arg2, u16 arg3);

#endif
