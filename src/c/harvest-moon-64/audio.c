#include "audio.h"

extern void func_800266C0(void*, s32, s32, s32, s32);
extern void func_800266F8(void*);
extern void func_800F2470(s32);
extern void func_800F268C(s32, s32, s32);
extern s32 func_800F2704(s32);
extern s32 func_800F2740(s32);
extern void func_800F4B68(s32, s32);
extern void func_800F5318(s32, s32);
extern s8 func_800F5404(s32);
extern void func_800F54C0(s32, s32);
extern void func_800F558C(s32, s32);
extern void func_800F5664(s32, f32);

void AudioInit(s32 arg0) {
  u16 i = 0;
  u16 j;

  do {
    D_801FB5D8[i].flags = 0;
    D_801FB5D8[i].unk_18 = 0;
    D_801FB5D8[i].pad_28 = 0;
    D_801FB5D8[i].unk_1C = 0;
    D_801FB5D8[i].unk_20 = 0;
    D_801FB690[i].unk_10 = 0;
    D_801FB5D8[i].unk_24 = 0;
    func_800266C0(&D_801FB5D8[i].unk_08, 0, 0, 0, 0);
    i++;
  } while (i < 4);

  j = 0;
  do {
    D_801FB690[j].flags = 0;
    D_801FB690[j].unk_00 = 0;
    D_801FB690[j].unk_04 = 0;
    D_801FB690[j].pad_14 = 0;
    D_801FB690[j].unk_08 = 0;
    D_801FB690[j].unk_0C = 0;
    D_801FB690[j].unk_10 = 0;
    j++;
  } while (j < 4);

  func_800F2470(arg0);
  func_800F4B68(2, 0x7FFF);
  func_800F4B68(1, 0x7FFF);
}
void func_8003CDC0(s32 arg0) __attribute__((alias("AudioInit")));

void AudioUpdate(void) {
  u16 j;
  u16 i = 0;

  do {
    if (D_801FB5D8[i].flags & 1) {
      if (D_801FB5D8[i].flags & 2) {
        u8 channel = (u8)i;
        func_800F268C(channel, D_801FB5D8[i].unk_00, D_801FB5D8[i].unk_04 - D_801FB5D8[i].unk_00);
        D_801FB5D8[i].unk_18 = func_800F2704(channel);
        D_801FB5D8[i].flags &= ~2;
      }
      if (D_801FB5D8[i].flags & 4) {
        func_800F5318(D_801FB5D8[i].unk_18, D_801FB5D8[i].unk_24);
        D_801FB5D8[i].flags &= ~4;
      }
      func_800266F8(&D_801FB5D8[i].unk_08);
      func_800F54C0(D_801FB5D8[i].unk_18, D_801FB5D8[i].unk_10);
      D_801FB5D8[i].pad_28 = func_800F5404(D_801FB5D8[i].unk_18);
      if (!D_801FB5D8[i].pad_28) {
        D_801FB5D8[i].flags = 0;
        func_800F5318(D_801FB5D8[i].unk_18, 1);
      }
    }
    i++;
  } while (i < 4);

  j = 0;
  do {
    if (D_801FB690[j].flags & 1) {
      if (D_801FB690[j].flags & 2) {
        D_801FB690[j].unk_04 = func_800F2740(D_801FB690[j].unk_00);
        D_801FB690[j].flags &= ~2;
      }
      if (D_801FB690[j].flags & 4) {
        func_800F5318(D_801FB690[j].unk_04, 0);
        D_801FB690[j].flags &= ~4;
      }
      {
        f32 freq = (f32)D_801FB690[j].unk_08;
        __asm__("nop" : : "r"(freq));
        func_800F5664(D_801FB690[j].unk_04, freq);
      }
      func_800F558C(D_801FB690[j].unk_04, D_801FB690[j].unk_0C);
      func_800F54C0(D_801FB690[j].unk_04, D_801FB690[j].unk_10);
      D_801FB690[j].pad_14 = func_800F5404(D_801FB690[j].unk_04);
      if (!D_801FB690[j].pad_14) {
        D_801FB690[j].flags = 0;
      }
    }
    j++;
  } while (j < 4);
}
void func_8003CF38(void) __attribute__((alias("AudioUpdate")));
