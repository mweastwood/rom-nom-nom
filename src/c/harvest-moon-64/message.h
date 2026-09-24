#ifndef HARVEST_MOON_64_MESSAGE_H
#define HARVEST_MOON_64_MESSAGE_H

#include "types.h"

typedef struct {
  u32 text_buffers[6];
  u32 r;
  u32 g;
  u32 b;
  u16 pos_x;
  u16 pos_y;
  u16 bank_id;
  s16 message_id;
  u8 reserved[6];
  u16 flags;
  u8 render_state[0xF4 - 0x34];
} TextBox;

typedef struct {
  s32 rom_start;
  s32 rom_end;
  s32 ram_start;
  s32 ram_end;
  s32 flags;
  s16 bank_id;
  s16 message_count;
} MessageBankEntry;

typedef struct {
  u8 header_data[0x14];
  u32 flags;
  u8 config_data[0xA8 - 0x18];
} MessageConfig;

typedef struct {
  void* ptr;
  s32 max_value;
  u8 type;
  u8 reserved[3];
} MessageVariable;

typedef struct {
  void* buffer_0;
  void* buffer_1;
  void* buffer_2;
  void* buffer_3;
  void* buffer_4;
  void* buffer_5;
  void* buffer_6;
  void* buffer_7;
  void* buffer_8;
  f32 scale_x;
  f32 scale_y;
  f32 scale_z;
  u16 font_or_tex_id;
  u16 attribute;
  u8 priority;
  u8 reserved[3];
} TextBoxLayer;

extern TextBox D_801C3F00[];
extern MessageBankEntry D_80205760[];
extern MessageConfig D_80188C00[];
extern MessageVariable D_8013CE08[];
extern u32* D_8013CE00;
extern TextBoxLayer D_801C3E6C[];

extern s32 D_801C3F18;
extern s32 D_801C3F1C;
extern s32 D_801C3F20;
extern s16 D_801C3F28;
extern s16 D_801C3F2A;
extern u16 D_801C3F32;

void MessageInit(void);
s32 MessageOpenTextBox(s32 box_index, s16 bank_id, s16 message_id);
s32 MessageRegisterBank(u16 index, s16 bank_id, s16 message_count, s32 rom_start, s32 rom_end,
                        s32 ram_start, s32 ram_end, s32 flags);
s32 MessageRegisterVariable(u16 index, void* ptr, u8 type, s32 max_value);
s32 MessageSetEventFlags(u32* flags);
s32 MessageSetTextColor(s32 box_index, s32 r, s32 g, s32 b);
s32 MessageSetBoxBackgroundLayer(s32 box_index, u16 font_or_tex_id, void* arg2, void* arg3,
                                 void* arg4, void* arg5, void* arg6, void* arg7, void* arg8,
                                 void* arg9, void* arg10, u16 arg11, u8 arg12, f32 scale_x,
                                 f32 scale_y, f32 scale_z);
s32 MessageSetBoxTextLayer(s32 box_index, u16 font_or_tex_id, void* arg2, void* arg3, void* arg4,
                           void* arg5, void* arg6, void* arg7, void* arg8, void* arg9, void* arg10,
                           u16 arg11, u8 arg12, f32 scale_x, f32 scale_y, f32 scale_z);
s32 MessageSetBoxPromptLayer(s32 box_index, u16 font_or_tex_id, void* arg2, void* arg3, void* arg4,
                             void* arg5, void* arg6, void* arg7, void* arg8, void* arg9,
                             void* arg10, u16 arg11, u8 arg12, f32 scale_x, f32 scale_y,
                             f32 scale_z);
s32 MessageClipSpan(s32 pos, s32 size, s32 limit);
void TextBoxUpdate(void);

#endif
