#include "graphic_model.h"

f32 GraphicGetDirectionAngle(u8 direction) {
  DirectionAngleTable table = g_direction_angles;
  return table.angles[direction];
}

void* GraphicResolveOffset1(u16 index, void* base) {
  return (void*)((u32)base + *(u32*)((u32)base + (index * 4)));
}

void* GraphicResolveOffset2(u16 index, void* base) {
  return (void*)((u32)base + *(u32*)((u32)base + (index * 4)));
}

void* GraphicResolveIndexedOffset(u16 index, void* base, u8* table) {
  table += index;
  return (void*)((u32)base + *(u32*)((u32)base + (table[4] * 4)));
}

Gfx* GraphicDrawModelBuffers(Gfx* glist) {
  Gfx* g1 = glist++;
  Gfx* g2 = glist++;
  Gfx* g3 = glist++;

  g1->words.w0 = 0xDB060000;
  g1->words.w1 = 0;

  g2->words.w0 = 0xDE000000;
  g2->words.w1 = (u32)&g_model_display_list_1;

  g3->words.w0 = 0xDE000000;
  g3->words.w1 = (u32)&g_model_display_list_2;

  return glist;
}
