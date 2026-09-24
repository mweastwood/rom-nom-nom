#include "common.h"

extern volatile u8 D_80205208;
extern volatile u16 D_801594E4;
extern volatile u16 D_80182BA0;
extern volatile u16 D_8020564C;
extern volatile u16 D_8020564A;
extern volatile u8 D_80205634;
extern volatile u8 D_801C3F71;
extern volatile u8 D_801C3BEC;
extern volatile u8 D_80237A04;
extern volatile u8 D_80204B38;
extern volatile s32 D_802226E8;
extern volatile u32 D_80188F70[];

void func_80026284(void);
void func_8004DEC8(void);
void func_800FC030(void);
void func_80029CC8(void);
void func_8003CF38(void);
void func_80029284(void);
void func_80046D78(void);
void func_80033058(void);
void func_8003C6E4(void);
void func_8003A1BC(void);
void func_800467F8(void);
void func_8002D3D4(void);
void func_800290B8(void);
void func_8002AE58(void);
void func_80042634(void);
void TextBoxUpdate(void);
void func_800293B8(void);
void func_80110410(void);
void func_800FC000(void);
void func_8004CF68(void);
void func_8004DF00(void);
u8 func_800263B0(s32);
void func_800264CC(s32);

void MainLoop(void) {
  void (*func)(void);

  D_80205208 = 0;
  D_801594E4 = 1;
  func_80026284();
  func_8004DEC8();
  D_80182BA0 = 1;
  D_8020564C = 0;

  while (1) {
    func_800FC030();
    while (D_801594E4 & 1) {
      while (D_80205208 == 0) {
      }
      if (D_8020564C == 0) {
        D_80182BA0 = 1;
        func = (void (*)(void))D_80188F70[D_8020564A];
        func();
        D_8020564C = D_80182BA0;
      }
      D_8020564C--;
      func_80029CC8();
      func_8003CF38();
      func_80029284();
      func_80046D78();
      func_80033058();
      func_8003C6E4();
      func_8003A1BC();
      func_800467F8();
      func_8002D3D4();
      func_800290B8();
      func_8002AE58();
      func_80042634();
      TextBoxUpdate();
      func_800293B8();
      D_80205208 = 0;
      D_80237A04 = D_801C3BEC;
      func_80110410();
    }
    func_800FC000();
  }
}
void func_80025FF0(void) __attribute__((alias("MainLoop")));

s32 RegisterGameLoopCallback(u16 callback_index, u32 callback_ptr) {
  s32 ret = 0;
  if (callback_index < 57) {
    if (D_80188F70[callback_index] == 0) {
      D_80188F70[callback_index] = callback_ptr;
      ret = 1;
    }
  }
  return ret;
}
s32 func_80026190(u16, u32) __attribute__((alias("RegisterGameLoopCallback")));

s32 SetCurrentGameLoopCallback(u16 callback_index) {
  s32 ret = 0;
  if (callback_index < 57) {
    if (D_80188F70[callback_index] != 0) {
      D_8020564A = callback_index;
      ret = 1;
    }
  }
  return ret;
}
s32 func_800261CC(u16) __attribute__((alias("SetCurrentGameLoopCallback")));

void EnableSystemFlag4B38(void) {
  D_80204B38 = 1;
}
void func_8002620C(void) __attribute__((alias("EnableSystemFlag4B38")));

void DisableSystemFlag4B38(void) {
  D_80204B38 = 0;
}
void func_80026220(void) __attribute__((alias("DisableSystemFlag4B38")));

void SetLoopCounter(u16 counter_val) {
  D_80182BA0 = counter_val;
}
void func_80026230(u16) __attribute__((alias("SetLoopCounter")));

void NoOpCallback(void) {}
void func_80026240(void) __attribute__((alias("NoOpCallback")));

void WaitVsyncFrames(u32 frame_count) {
  u16 i = 1;
  frame_count &= 0xFFFF;
  if (frame_count != 0) {
    do {
      D_80205208 = 0;
      while (D_80205208 == 0) {
      }
    } while (i++ < frame_count);
  }
}
void func_80026248(u32) __attribute__((alias("WaitVsyncFrames")));

void WaitForSystemReady(void) {
  u16 i;
  while (!(D_801594E4 & 2)) {
    i = 1;
    do {
      D_80205208 = 0;
      while (D_80205208 == 0) {
      }
    } while (i++ == 0);
  }
}
void func_80026284(void) __attribute__((alias("WaitForSystemReady")));

void UpdateFrameCounterAndTicks(s32 arg0) {
  D_802226E8 = arg0;
  D_801594E4 &= ~2;
  func_8004CF68();
  func_800263B0(arg0);
  func_800264CC(arg0);
  if (D_80205634 >= 60) {
    D_80205634 = 0;
    D_801594E4 |= 2;
  }
  func_8004DF00();
  D_80205634++;
  D_801C3BEC++;
  D_801C3F71++;
}
void func_800262CC(s32) __attribute__((alias("UpdateFrameCounterAndTicks")));
