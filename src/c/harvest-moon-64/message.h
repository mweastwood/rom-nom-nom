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

extern TextBox g_text_boxes[];

extern MessageBankEntry g_message_banks[];

extern MessageConfig g_message_configs[];

extern MessageVariable g_message_variables[];

extern u32* g_message_event_flags;

extern TextBoxLayer g_text_box_layers[];

extern s32 g_message_active_box_index;

extern s32 g_message_text_offset;

extern s32 g_message_char_delay;

extern s16 g_message_window_x;

extern s16 g_message_window_y;

extern u16 g_message_state_flags;

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
void MessageSetEventFlag(u16 flag_index);
void MessageClearEventFlag(u16 flag_index);
u32 MessageGetEventFlag(u16 flag_index);
void TextBoxUpdate(void);

#endif
