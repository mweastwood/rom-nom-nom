#include "message.h"

#include "include_asm.h"

INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_8003D970);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_8003DBE8);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_8003DD14);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_8003DDF8);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_8003E77C);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_8003E910);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_8003EA1C);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_8003EE7C);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_8003EFD8);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_8003F024);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_8003F0DC);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_8003F130);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_8003F28C);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_8003F30C);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_8003F360);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_8003F464);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_8003F4E0);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_8003F54C);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_8003F5D0);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_8003F630);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_8003F690);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_8003F80C);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_8003F910);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_8003FA1C);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_8003FAE8);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_8003FAF8);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_8003FB4C);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_8003FBA0);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_8003FBD8);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_8003FD74);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_8003FDB0);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_8003FE9C);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_8003FFF4);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_80040140);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_800401C8);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_8004022C);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_80040318);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_800403F0);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_80040628);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_80040858);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_80040C38);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_80041850);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_800419C4);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_80041B28);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_80041B80);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_80041CD8);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_80042014);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_800423F0);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_80042634);

void MessageInit(void) {
  u16 i;

  for (i = 0; i < 1; i++) {
    D_801C3F00[i].pos_x = 0;
    D_801C3F00[i].pos_y = 0;
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

s32 MessageSetBoxBackgroundLayer(s32 box_index, u16 font_or_tex_id, void* arg2, void* arg3,
                                 void* arg4, void* arg5, void* arg6, void* arg7, void* arg8,
                                 void* arg9, void* arg10, u16 arg11, u8 arg12, f32 scale_x,
                                 f32 scale_y, f32 scale_z) {
  s32 ret = 0;

  if (!(box_index & 0xFFFF) && (D_801C3F32 & 1)) {
    ret = 1;
    D_801C3E6C[0].buffer_0 = arg2;
    D_801C3E6C[0].buffer_1 = arg3;
    D_801C3E6C[0].buffer_2 = arg4;
    D_801C3E6C[0].buffer_3 = arg5;
    D_801C3E6C[0].buffer_4 = arg6;
    D_801C3E6C[0].buffer_5 = arg7;
    D_801C3E6C[0].buffer_6 = arg8;
    D_801C3E6C[0].buffer_7 = arg9;
    D_801C3E6C[0].buffer_8 = arg10;
    D_801C3E6C[0].font_or_tex_id = font_or_tex_id;
    D_801C3E6C[0].attribute = arg11;
    D_801C3E6C[0].priority = arg12;
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
    D_801C3E6C[1].buffer_0 = arg2;
    D_801C3E6C[1].buffer_1 = arg3;
    D_801C3E6C[1].buffer_2 = arg4;
    D_801C3E6C[1].buffer_3 = arg5;
    D_801C3E6C[1].buffer_4 = arg6;
    D_801C3E6C[1].buffer_5 = arg7;
    D_801C3E6C[1].buffer_6 = arg8;
    D_801C3E6C[1].buffer_7 = arg9;
    D_801C3E6C[1].buffer_8 = arg10;
    D_801C3E6C[1].font_or_tex_id = font_or_tex_id;
    D_801C3E6C[1].attribute = arg11;
    D_801C3E6C[1].priority = arg12;
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
    D_801C3E6C[2].buffer_0 = arg2;
    D_801C3E6C[2].buffer_1 = arg3;
    D_801C3E6C[2].buffer_2 = arg4;
    D_801C3E6C[2].buffer_3 = arg5;
    D_801C3E6C[2].buffer_4 = arg6;
    D_801C3E6C[2].buffer_5 = arg7;
    D_801C3E6C[2].buffer_6 = arg8;
    D_801C3E6C[2].buffer_7 = arg9;
    D_801C3E6C[2].buffer_8 = arg10;
    D_801C3E6C[2].font_or_tex_id = font_or_tex_id;
    D_801C3E6C[2].attribute = arg11;
    D_801C3E6C[2].priority = arg12;
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

INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_80043430);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_80043A88);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_80043AD8);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_80043B84);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_80043C6C);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_80043C98);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_80043CF8);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_80043D8C);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_80043E28);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_80043E5C);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_80043E94);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_80043EC8);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_80044684);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_800449C4);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_80044BF4);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_80044D78);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/message", func_80045260);

void TextBoxUpdate(void) {
  u16 i = 0;
  u32 mask = 0x20000;
  do {
    u16 flags = D_801C3F00[i].flags;
    if ((flags & 1) && (flags & 2)) {
      u8 handled = 0;
      if (flags & 0x10) {
        u32 val = D_80188C00[D_801C3F00[i].bank_id].flags;
        if ((val & 4) || (val & mask)) {
          D_801C3F00[i].flags = flags & ~0x10;
        }
        handled = 1;
      }
      if (D_801C3F00[i].flags & 0x20) {
        func_80044D78(i);
        handled = 1;
      }
      if (!handled) {
        func_80045260(i);
      }
    }
    i++;
  } while (i < 1);
}

void func_80045CB0(void) __attribute__((alias("TextBoxUpdate")));
