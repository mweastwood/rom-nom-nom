#include "common.h"

typedef struct {
  u32 unk_00[6];
  u32 r;
  u32 g;
  u32 b;
  u16 unk_24;
  u16 unk_26;
  s16 bankId;
  s16 messageId;
  u8 pad[6];
  u16 active;
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
  s32 romStart;
  s32 romEnd;
  s32 ramStart;
  s32 ramEnd;
  s32 flags;
  s16 bankId;
  s16 messageCount;
} MessageBankEntry;

extern MessageBankEntry D_80205760[];

void Message_Init(void) {
  u16 i;

  for (i = 0; i < 1; i++) {
    D_801C3F00[i].unk_24 = 0;
    D_801C3F00[i].unk_26 = 0;
    D_801C3F00[i].bankId = 0;
    D_801C3F00[i].r = 0xFF;
    D_801C3F00[i].g = 0xFF;
    D_801C3F00[i].b = 0xFF;
    D_801C3F00[i].active = 0;
  }
}
void func_80042F60(void) __attribute__((alias("Message_Init")));

s32 Message_OpenTextBox(s32 boxIndex, s16 bankId, s16 messageId) {
  s32 ret = 0;

  if (!(boxIndex & 0xFFFF) && !(D_801C3F32 & 1)) {
    ret = 1;
    D_801C3F18 = 0xFF;
    D_801C3F1C = 0xFF;
    D_801C3F20 = 0xFF;
    D_801C3F28 = bankId;
    D_801C3F2A = messageId;
    D_801C3F32 = 1;
  }
  return ret;
}
s32 func_80042FEC(s32 boxIndex, s16 bankId, s16 messageId)
    __attribute__((alias("Message_OpenTextBox")));

s32 Message_RegisterBank(u16 index, s16 bankId, s16 messageCount, s32 romStart, s32 romEnd,
                         s32 ramStart, s32 ramEnd, s32 flags) {
  s32 ret = 0;

  if (index < 0x46) {
    ret = 1;
    D_80205760[index].bankId = bankId;
    D_80205760[index].messageCount = messageCount;
    D_80205760[index].romStart = romStart;
    D_80205760[index].romEnd = romEnd;
    D_80205760[index].ramStart = ramStart;
    D_80205760[index].ramEnd = ramEnd;
    D_80205760[index].flags = flags;
  }
  return ret;
}
s32 func_80043050(u16 index, s16 bankId, s16 messageCount, s32 romStart, s32 romEnd, s32 ramStart,
                  s32 ramEnd, s32 flags) __attribute__((alias("Message_RegisterBank")));

typedef struct {
  void* ptr;
  s32 maxValue;
  u8 type;
  u8 pad[3];
} MessageVariable;

extern MessageVariable D_8013CE08[];
extern u32* D_8013CE00;

s32 Message_RegisterVariable(u16 index, void* ptr, u8 type, s32 maxValue) {
  s32 ret = 0;

  if (index < 0x50) {
    if (type < 5) {
      ret = 1;
      D_8013CE08[index].ptr = ptr;
      D_8013CE08[index].type = type;
      D_8013CE08[index].maxValue = maxValue;
    }
  }
  return ret;
}
s32 func_800430DC(u16 index, void* ptr, u8 type, s32 maxValue)
    __attribute__((alias("Message_RegisterVariable")));

s32 Message_SetEventFlags(u32* flags) {
  D_8013CE00 = flags;
  return 0;
}
s32 func_80043138(u32* flags) __attribute__((alias("Message_SetEventFlags")));

s32 Message_SetTextColor(s32 boxIndex, s32 r, s32 g, s32 b) {
  s32 ret = 0;

  if (!(boxIndex & 0xFFFF) && (D_801C3F32 & 1)) {
    ret = 1;
    D_801C3F18 = r;
    D_801C3F1C = g;
    D_801C3F20 = b;
  }
  return ret;
}
s32 func_80043148(s32 boxIndex, s32 r, s32 g, s32 b) __attribute__((alias("Message_SetTextColor")));

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
  f32 scaleX;
  f32 scaleY;
  f32 scaleZ;
  u16 fontOrTexId;
  u16 unk_32;
  u8 unk_34;
  u8 pad[3];
} TextBoxLayer;

extern TextBoxLayer D_801C3E6C[];

s32 Message_SetBoxBackgroundLayer(s32 boxIndex, u16 fontOrTexId, void* arg2, void* arg3, void* arg4,
                                  void* arg5, void* arg6, void* arg7, void* arg8, void* arg9,
                                  void* arg10, u16 arg11, u8 arg12, f32 scaleX, f32 scaleY,
                                  f32 scaleZ) {
  s32 ret = 0;

  if (!(boxIndex & 0xFFFF) && (D_801C3F32 & 1)) {
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
    D_801C3E6C[0].fontOrTexId = fontOrTexId;
    D_801C3E6C[0].unk_32 = arg11;
    D_801C3E6C[0].unk_34 = arg12;
    D_801C3E6C[0].scaleX = scaleX;
    D_801C3E6C[0].scaleY = scaleY;
    D_801C3E6C[0].scaleZ = scaleZ;
  }
  return ret;
}
s32 func_8004318C(s32 boxIndex, u16 fontOrTexId, void* arg2, void* arg3, void* arg4, void* arg5,
                  void* arg6, void* arg7, void* arg8, void* arg9, void* arg10, u16 arg11, u8 arg12,
                  f32 scaleX, f32 scaleY, f32 scaleZ)
    __attribute__((alias("Message_SetBoxBackgroundLayer")));

s32 Message_SetBoxTextLayer(s32 boxIndex, u16 fontOrTexId, void* arg2, void* arg3, void* arg4,
                            void* arg5, void* arg6, void* arg7, void* arg8, void* arg9, void* arg10,
                            u16 arg11, u8 arg12, f32 scaleX, f32 scaleY, f32 scaleZ) {
  s32 ret = 0;

  if (!(boxIndex & 0xFFFF) && (D_801C3F32 & 1)) {
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
    D_801C3E6C[1].fontOrTexId = fontOrTexId;
    D_801C3E6C[1].unk_32 = arg11;
    D_801C3E6C[1].unk_34 = arg12;
    D_801C3E6C[1].scaleX = scaleX;
    D_801C3E6C[1].scaleY = scaleY;
    D_801C3E6C[1].scaleZ = scaleZ;
  }
  return ret;
}
s32 func_80043260(s32 boxIndex, u16 fontOrTexId, void* arg2, void* arg3, void* arg4, void* arg5,
                  void* arg6, void* arg7, void* arg8, void* arg9, void* arg10, u16 arg11, u8 arg12,
                  f32 scaleX, f32 scaleY, f32 scaleZ)
    __attribute__((alias("Message_SetBoxTextLayer")));

s32 Message_SetBoxPromptLayer(s32 boxIndex, u16 fontOrTexId, void* arg2, void* arg3, void* arg4,
                              void* arg5, void* arg6, void* arg7, void* arg8, void* arg9,
                              void* arg10, u16 arg11, u8 arg12, f32 scaleX, f32 scaleY,
                              f32 scaleZ) {
  s32 ret = 0;

  if (!(boxIndex & 0xFFFF) && (D_801C3F32 & 1)) {
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
    D_801C3E6C[2].fontOrTexId = fontOrTexId;
    D_801C3E6C[2].unk_32 = arg11;
    D_801C3E6C[2].unk_34 = arg12;
    D_801C3E6C[2].scaleX = scaleX;
    D_801C3E6C[2].scaleY = scaleY;
    D_801C3E6C[2].scaleZ = scaleZ;
  }
  return ret;
}
s32 func_80043334(s32 boxIndex, u16 fontOrTexId, void* arg2, void* arg3, void* arg4, void* arg5,
                  void* arg6, void* arg7, void* arg8, void* arg9, void* arg10, u16 arg11, u8 arg12,
                  f32 scaleX, f32 scaleY, f32 scaleZ)
    __attribute__((alias("Message_SetBoxPromptLayer")));

s32 Message_ClipSpan(s32 pos, s32 size, s32 limit) {
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
s32 func_80043408(s32 pos, s32 size, s32 limit) __attribute__((alias("Message_ClipSpan")));

__asm__(".include \"asm/harvest-moon-64/1E4DC.s\"");
