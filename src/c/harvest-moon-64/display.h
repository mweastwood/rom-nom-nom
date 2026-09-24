#ifndef HARVEST_MOON_64_DISPLAY_H
#define HARVEST_MOON_64_DISPLAY_H

#include "types.h"

typedef struct {
  u32 unk_00[9];
  u8 pad_24[0x168 - 0x24];
  u32 dl;
  u16 flags;
  u16 pad_16e;
} RenderEntry;

extern u8 D_80189135[9];
extern RenderEntry D_8018A850[336];
extern u16 D_8018A05A;

void RenderInit(void);
void RenderResetCount(void);
void RenderNoOp(void);
void RenderAddEntry(u32 dl, u16 flags);
s32 RenderSetTranslation(s32 index, u32 x, u32 y, u32 z);
s32 RenderSetRotation(s32 index, u32 x, u32 y, u32 z);
s32 RenderSetScale(s32 index, u32 x, u32 y, u32 z);
void RenderNoOp2(void);

#endif
