#include "common.h"

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

void RenderResetCount(void);

void RenderInit(void) {
  s32 i = 0;

  D_80189135[0] = 0xFF;
  D_80189135[1] = 0xFF;
  D_80189135[2] = 0xFF;
  D_80189135[3] = 0xFF;
  D_80189135[4] = 0xFF;
  D_80189135[5] = 0xFF;
  D_80189135[6] = 0xFF;
  D_80189135[7] = 0xFF;
  D_80189135[8] = 0xFF;

  do {
    D_8018A850[i & 0xFFFF].flags = 0;
    D_8018A850[i & 0xFFFF].unk_00[0] = 0;
    D_8018A850[i & 0xFFFF].unk_00[1] = 0;
    D_8018A850[i & 0xFFFF].unk_00[2] = 0;
    D_8018A850[i & 0xFFFF].unk_00[3] = 0;
    D_8018A850[i & 0xFFFF].unk_00[4] = 0;
    D_8018A850[i & 0xFFFF].unk_00[5] = 0;
    D_8018A850[i & 0xFFFF].unk_00[6] = 0;
    D_8018A850[i & 0xFFFF].unk_00[7] = 0;
    D_8018A850[i & 0xFFFF].unk_00[8] = 0;
    i++;
  } while ((u32)(i & 0xFFFF) < 336);

  RenderResetCount();
}
void func_80029170(void) __attribute__((alias("RenderInit")));

void RenderResetCount(void) {
  D_8018A05A = 0;
}
void func_80029284(void) __attribute__((alias("RenderResetCount")));

void RenderNoOp(void) {}
void func_80029294(void) __attribute__((alias("RenderNoOp")));

void RenderAddEntry(u32 dl, u16 flags) {
#ifdef __clang__
  register u32 index = D_8018A05A;
#else
  register u32 index asm("$2") = D_8018A05A;
#endif
  u16 next = index + 1;
  index &= 0xFFFF;
  D_8018A850[index].dl = dl;
  D_8018A850[index].flags = flags | 1;
  D_8018A05A = next;
}
void func_8002929C(u32 dl, u16 flags) __attribute__((alias("RenderAddEntry")));

s32 RenderSetTranslation(s32 index, u32 x, u32 y, u32 z) {
  u32 idx = index & 0xFFFF;
  D_8018A850[idx].unk_00[0] = x;
  D_8018A850[idx].unk_00[1] = y;
  D_8018A850[idx].unk_00[2] = z;
  return 1;
}
s32 func_800292EC(s32 index, u32 x, u32 y, u32 z) __attribute__((alias("RenderSetTranslation")));

s32 RenderSetRotation(s32 index, u32 x, u32 y, u32 z) {
  u32 idx = index & 0xFFFF;
  D_8018A850[idx].unk_00[3] = x;
  D_8018A850[idx].unk_00[4] = y;
  D_8018A850[idx].unk_00[5] = z;
  return 1;
}
s32 func_80029330(s32 index, u32 x, u32 y, u32 z) __attribute__((alias("RenderSetRotation")));

s32 RenderSetScale(s32 index, u32 x, u32 y, u32 z) {
  u32 idx = index & 0xFFFF;
  D_8018A850[idx].unk_00[6] = x;
  D_8018A850[idx].unk_00[7] = y;
  D_8018A850[idx].unk_00[8] = z;
  return 1;
}
s32 func_80029374(s32 index, u32 x, u32 y, u32 z) __attribute__((alias("RenderSetScale")));

void RenderNoOp2(void) {}
void func_800293B8(void) __attribute__((alias("RenderNoOp2")));
