#include "display.h"

#include "include_asm.h"

void RenderInit(void) {
  s32 i = 0;

  g_display_matrices[0] = 0xFF;
  g_display_matrices[1] = 0xFF;
  g_display_matrices[2] = 0xFF;
  g_display_matrices[3] = 0xFF;
  g_display_matrices[4] = 0xFF;
  g_display_matrices[5] = 0xFF;
  g_display_matrices[6] = 0xFF;
  g_display_matrices[7] = 0xFF;
  g_display_matrices[8] = 0xFF;

  do {
    g_render_entries[i & 0xFFFF].flags = 0;
    g_render_entries[i & 0xFFFF].transform[0] = 0;
    g_render_entries[i & 0xFFFF].transform[1] = 0;
    g_render_entries[i & 0xFFFF].transform[2] = 0;
    g_render_entries[i & 0xFFFF].transform[3] = 0;
    g_render_entries[i & 0xFFFF].transform[4] = 0;
    g_render_entries[i & 0xFFFF].transform[5] = 0;
    g_render_entries[i & 0xFFFF].transform[6] = 0;
    g_render_entries[i & 0xFFFF].transform[7] = 0;
    g_render_entries[i & 0xFFFF].transform[8] = 0;
    i++;
  } while ((u32)(i & 0xFFFF) < 336);

  RenderResetCount();
}
void func_80029170(void) __attribute__((alias("RenderInit")));

void RenderResetCount(void) {
  g_render_entry_count = 0;
}
void func_80029284(void) __attribute__((alias("RenderResetCount")));

void RenderNoOp(void) {}
void func_80029294(void) __attribute__((alias("RenderNoOp")));

void RenderAddEntry(u32 dl, u16 flags) {
#ifdef __clang__
  register u32 index = g_render_entry_count;
#else
  register u32 index asm("$2") = g_render_entry_count;
#endif
  u16 next = index + 1;
  index &= 0xFFFF;
  g_render_entries[index].dl = dl;
  g_render_entries[index].flags = flags | 1;
  g_render_entry_count = next;
}
void func_8002929C(u32 dl, u16 flags) __attribute__((alias("RenderAddEntry")));

s32 RenderSetTranslation(s32 index, u32 x, u32 y, u32 z) {
  u32 idx = index & 0xFFFF;
  g_render_entries[idx].transform[0] = x;
  g_render_entries[idx].transform[1] = y;
  g_render_entries[idx].transform[2] = z;
  return 1;
}
s32 func_800292EC(s32 index, u32 x, u32 y, u32 z) __attribute__((alias("RenderSetTranslation")));

s32 RenderSetRotation(s32 index, u32 x, u32 y, u32 z) {
  u32 idx = index & 0xFFFF;
  g_render_entries[idx].transform[3] = x;
  g_render_entries[idx].transform[4] = y;
  g_render_entries[idx].transform[5] = z;
  return 1;
}
s32 func_80029330(s32 index, u32 x, u32 y, u32 z) __attribute__((alias("RenderSetRotation")));

s32 RenderSetScale(s32 index, u32 x, u32 y, u32 z) {
  u32 idx = index & 0xFFFF;
  g_render_entries[idx].transform[6] = x;
  g_render_entries[idx].transform[7] = y;
  g_render_entries[idx].transform[8] = z;
  return 1;
}
s32 func_80029374(s32 index, u32 x, u32 y, u32 z) __attribute__((alias("RenderSetScale")));

void RenderNoOp2(void) {}
void func_800293B8(void) __attribute__((alias("RenderNoOp2")));

INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/display", func_800293C0);
