#ifndef GRAPHIC_MODEL_H
#define GRAPHIC_MODEL_H

#include "types.h"

typedef struct {
  f32 angles[8];
} DirectionAngleTable;

extern const DirectionAngleTable g_direction_angles;
extern const u32 g_model_display_list_1;
extern const u32 g_model_display_list_2;

f32 GraphicGetDirectionAngle(u8 direction);
void* GraphicResolveOffset1(u16 index, void* base);
void* GraphicResolveOffset2(u16 index, void* base);
void* GraphicResolveIndexedOffset(u16 index, void* base, u8* table);
Gfx* GraphicDrawModelBuffers(Gfx* glist);

#endif
