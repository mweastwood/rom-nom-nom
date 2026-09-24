#include "message.h"

void MessageInit(void) {
  u16 i;

  for (i = 0; i < 1; i++) {
    D_801C3F00[i].unk_24 = 0;
    D_801C3F00[i].unk_26 = 0;
    D_801C3F00[i].bank_id = 0;
    D_801C3F00[i].r = 0xFF;
    D_801C3F00[i].g = 0xFF;
    D_801C3F00[i].b = 0xFF;
    D_801C3F00[i].flags = 0;
  }
}
void func_80042F60(void) __attribute__((alias("MessageInit")));

s32 MessageOpenTextBox(s32 box_index, s16 bank_id, s16 message_id) {
  s32 ret = 0;

  if (!(box_index & 0xFFFF) && !(D_801C3F32 & 1)) {
    ret = 1;
    D_801C3F18 = 0xFF;
    D_801C3F1C = 0xFF;
    D_801C3F20 = 0xFF;
    D_801C3F28 = bank_id;
    D_801C3F2A = message_id;
    D_801C3F32 = 1;
  }
  return ret;
}
s32 func_80042FEC(s32 box_index, s16 bank_id, s16 message_id)
    __attribute__((alias("MessageOpenTextBox")));

s32 MessageRegisterBank(u16 index, s16 bank_id, s16 message_count, s32 rom_start, s32 rom_end,
                        s32 ram_start, s32 ram_end, s32 flags) {
  s32 ret = 0;

  if (index < 0x46) {
    ret = 1;
    D_80205760[index].bank_id = bank_id;
    D_80205760[index].message_count = message_count;
    D_80205760[index].rom_start = rom_start;
    D_80205760[index].rom_end = rom_end;
    D_80205760[index].ram_start = ram_start;
    D_80205760[index].ram_end = ram_end;
    D_80205760[index].flags = flags;
  }
  return ret;
}
s32 func_80043050(u16 index, s16 bank_id, s16 message_count, s32 rom_start, s32 rom_end,
                  s32 ram_start, s32 ram_end, s32 flags)
    __attribute__((alias("MessageRegisterBank")));

typedef struct {
  void* ptr;
  s32 max_value;
  u8 type;
  u8 pad[3];
} MessageVariable;

extern MessageVariable D_8013CE08[];
extern u32* D_8013CE00;

s32 MessageRegisterVariable(u16 index, void* ptr, u8 type, s32 max_value) {
  s32 ret = 0;

  if (index < 0x50) {
    if (type < 5) {
      ret = 1;
      D_8013CE08[index].ptr = ptr;
      D_8013CE08[index].type = type;
      D_8013CE08[index].max_value = max_value;
    }
  }
  return ret;
}
s32 func_800430DC(u16 index, void* ptr, u8 type, s32 max_value)
    __attribute__((alias("MessageRegisterVariable")));

s32 MessageSetEventFlags(u32* flags) {
  D_8013CE00 = flags;
  return 0;
}
s32 func_80043138(u32* flags) __attribute__((alias("MessageSetEventFlags")));

s32 MessageSetTextColor(s32 box_index, s32 r, s32 g, s32 b) {
  s32 ret = 0;

  if (!(box_index & 0xFFFF) && (D_801C3F32 & 1)) {
    ret = 1;
    D_801C3F18 = r;
    D_801C3F1C = g;
    D_801C3F20 = b;
  }
  return ret;
}
s32 func_80043148(s32 box_index, s32 r, s32 g, s32 b) __attribute__((alias("MessageSetTextColor")));

typedef struct {
  void* unk_00;
  void* unk_04;
  void* unk_08;
  void* unk_0C;
  void* unk_10;
  void* unk_14;
  void* unk_18;
  void* unk_1C;
  void* unk_20;
  f32 scale_x;
  f32 scale_y;
  f32 scale_z;
  u16 font_or_tex_id;
  u16 unk_32;
  u8 unk_34;
  u8 pad[3];
} TextBoxLayer;

extern TextBoxLayer D_801C3E6C[];

s32 MessageSetBoxBackgroundLayer(s32 box_index, u16 font_or_tex_id, void* arg2, void* arg3,
                                 void* arg4, void* arg5, void* arg6, void* arg7, void* arg8,
                                 void* arg9, void* arg10, u16 arg11, u8 arg12, f32 scale_x,
                                 f32 scale_y, f32 scale_z) {
  s32 ret = 0;

  if (!(box_index & 0xFFFF) && (D_801C3F32 & 1)) {
    ret = 1;
    D_801C3E6C[0].unk_00 = arg2;
    D_801C3E6C[0].unk_04 = arg3;
    D_801C3E6C[0].unk_08 = arg4;
    D_801C3E6C[0].unk_0C = arg5;
    D_801C3E6C[0].unk_10 = arg6;
    D_801C3E6C[0].unk_14 = arg7;
    D_801C3E6C[0].unk_18 = arg8;
    D_801C3E6C[0].unk_1C = arg9;
    D_801C3E6C[0].unk_20 = arg10;
    D_801C3E6C[0].font_or_tex_id = font_or_tex_id;
    D_801C3E6C[0].unk_32 = arg11;
    D_801C3E6C[0].unk_34 = arg12;
    D_801C3E6C[0].scale_x = scale_x;
    D_801C3E6C[0].scale_y = scale_y;
    D_801C3E6C[0].scale_z = scale_z;
  }
  return ret;
}
s32 func_8004318C(s32 box_index, u16 font_or_tex_id, void* arg2, void* arg3, void* arg4, void* arg5,
                  void* arg6, void* arg7, void* arg8, void* arg9, void* arg10, u16 arg11, u8 arg12,
                  f32 scale_x, f32 scale_y, f32 scale_z)
    __attribute__((alias("MessageSetBoxBackgroundLayer")));

s32 MessageSetBoxTextLayer(s32 box_index, u16 font_or_tex_id, void* arg2, void* arg3, void* arg4,
                           void* arg5, void* arg6, void* arg7, void* arg8, void* arg9, void* arg10,
                           u16 arg11, u8 arg12, f32 scale_x, f32 scale_y, f32 scale_z) {
  s32 ret = 0;

  if (!(box_index & 0xFFFF) && (D_801C3F32 & 1)) {
    ret = 1;
    D_801C3E6C[1].unk_00 = arg2;
    D_801C3E6C[1].unk_04 = arg3;
    D_801C3E6C[1].unk_08 = arg4;
    D_801C3E6C[1].unk_0C = arg5;
    D_801C3E6C[1].unk_10 = arg6;
    D_801C3E6C[1].unk_14 = arg7;
    D_801C3E6C[1].unk_18 = arg8;
    D_801C3E6C[1].unk_1C = arg9;
    D_801C3E6C[1].unk_20 = arg10;
    D_801C3E6C[1].font_or_tex_id = font_or_tex_id;
    D_801C3E6C[1].unk_32 = arg11;
    D_801C3E6C[1].unk_34 = arg12;
    D_801C3E6C[1].scale_x = scale_x;
    D_801C3E6C[1].scale_y = scale_y;
    D_801C3E6C[1].scale_z = scale_z;
  }
  return ret;
}
s32 func_80043260(s32 box_index, u16 font_or_tex_id, void* arg2, void* arg3, void* arg4, void* arg5,
                  void* arg6, void* arg7, void* arg8, void* arg9, void* arg10, u16 arg11, u8 arg12,
                  f32 scale_x, f32 scale_y, f32 scale_z)
    __attribute__((alias("MessageSetBoxTextLayer")));

s32 MessageSetBoxPromptLayer(s32 box_index, u16 font_or_tex_id, void* arg2, void* arg3, void* arg4,
                             void* arg5, void* arg6, void* arg7, void* arg8, void* arg9,
                             void* arg10, u16 arg11, u8 arg12, f32 scale_x, f32 scale_y,
                             f32 scale_z) {
  s32 ret = 0;

  if (!(box_index & 0xFFFF) && (D_801C3F32 & 1)) {
    ret = 1;
    D_801C3E6C[2].unk_00 = arg2;
    D_801C3E6C[2].unk_04 = arg3;
    D_801C3E6C[2].unk_08 = arg4;
    D_801C3E6C[2].unk_0C = arg5;
    D_801C3E6C[2].unk_10 = arg6;
    D_801C3E6C[2].unk_14 = arg7;
    D_801C3E6C[2].unk_18 = arg8;
    D_801C3E6C[2].unk_1C = arg9;
    D_801C3E6C[2].unk_20 = arg10;
    D_801C3E6C[2].font_or_tex_id = font_or_tex_id;
    D_801C3E6C[2].unk_32 = arg11;
    D_801C3E6C[2].unk_34 = arg12;
    D_801C3E6C[2].scale_x = scale_x;
    D_801C3E6C[2].scale_y = scale_y;
    D_801C3E6C[2].scale_z = scale_z;
  }
  return ret;
}
s32 func_80043334(s32 box_index, u16 font_or_tex_id, void* arg2, void* arg3, void* arg4, void* arg5,
                  void* arg6, void* arg7, void* arg8, void* arg9, void* arg10, u16 arg11, u8 arg12,
                  f32 scale_x, f32 scale_y, f32 scale_z)
    __attribute__((alias("MessageSetBoxPromptLayer")));

s32 MessageClipSpan(s32 pos, s32 size, s32 limit) {
  pos += size;
  if (limit < pos) {
    size -= (pos - limit);
    pos = limit;
  }
  if (pos < 0) {
    size -= pos;
  }
  return size;
}
s32 func_80043408(s32 pos, s32 size, s32 limit) __attribute__((alias("MessageClipSpan")));
