#ifndef HARVEST_MOON_64_MESSAGE_H
#define HARVEST_MOON_64_MESSAGE_H

#include "types.h"

typedef struct {
  u32 unk_00[6];
  u32 r;
  u32 g;
  u32 b;
  u16 unk_24;
  u16 unk_26;
  u16 bank_id;
  s16 message_id;
  u8 pad[6];
  u16 flags;
  u8 pad2[0xF4 - 0x34];
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
  u8 pad[0x14];
  u32 flags;
  u8 pad2[0xA8 - 0x18];
} MessageConfig;

extern TextBox D_801C3F00[];
extern MessageBankEntry D_80205760[];
extern MessageConfig D_80188C00[];

extern s32 D_801C3F18;
extern s32 D_801C3F1C;
extern s32 D_801C3F20;
extern s16 D_801C3F28;
extern s16 D_801C3F2A;
extern u16 D_801C3F32;

void MessageInit(void);
s32 MessageOpenTextBox(s32 box_index, s16 bank_id, s16 message_id);
void TextBoxUpdate(void);

#endif
