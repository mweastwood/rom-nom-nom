#ifndef HARVEST_MOON_64_DISPLAY_H
#define HARVEST_MOON_64_DISPLAY_H

#include "types.h"

typedef struct {
  u32 transform[9];
  u8 matrix_buffer[0x168 - 0x24];
  u32 dl;
  u16 flags;
  u16 reserved;
} RenderEntry;

extern u8 g_display_matrices[9];

extern RenderEntry g_render_entries[336];

extern u16 g_render_entry_count;

void RenderInit(void);
void RenderResetCount(void);
void RenderNoOp(void);
void RenderAddEntry(u32 dl, u16 flags);
s32 RenderSetTranslation(s32 index, u32 x, u32 y, u32 z);
s32 RenderSetRotation(s32 index, u32 x, u32 y, u32 z);
s32 RenderSetScale(s32 index, u32 x, u32 y, u32 z);
void RenderNoOp2(void);

#endif
