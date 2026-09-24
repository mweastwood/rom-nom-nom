#include "audio.h"
#include "boot.h"
#include "display.h"
#include "dma.h"
#include "game_time.h"
#include "layer.h"
#include "message.h"
#include "types.h"

extern u32 osTvType;
extern u8 D_8011D0F0;
extern u8 D_8011D9B0;

extern volatile u8 D_80205208;
extern volatile u16 D_801594E4;
extern volatile u16 D_80182BA0;
extern volatile u16 D_8020564C;
extern volatile u16 D_8020564A;
extern volatile u8 D_80205634;
extern volatile u8 D_8016FB04;
extern volatile u8 D_801C3F71;
extern volatile u8 D_801D6230;
extern volatile u8 D_801C3F34;
extern volatile u8 D_801C3BEC;
extern volatile u8 D_80237A04;
extern volatile u8 D_8020520A;
extern volatile u8 D_80237408;
extern u8 D_802373F1;
extern volatile u8 D_802226E2;
extern volatile s32 D_802226E8;
extern u32 D_801C3B68;
extern u32 D_801C3B6C;
extern u32 D_801C3B70;
extern u32 D_801C3B74;
extern u32 D_80205630;
extern u8 D_80204B38;
extern u16 D_801FD610;
extern volatile u32 D_80188F70[];

void func_800FC000(void);
void func_801002F0(void*);
void func_80105B00(s32);
void func_80025E24(void);
void func_8004DF10(void);
void func_80025FF0(void);
void func_80025F04(void);
void func_8004CDA0(void);
void func_800268F0(void);
void func_80029000(void);
void func_80029170(void);
void func_80029B30(void);
void func_8002AFE0(void);
void func_800337D0(void);
void func_8003D970(void);
void func_80042F60(void);
void func_80045DE0(void);
void func_80046860(void);
void func_8004DEB0(void);
void func_8002DC70(void);
void func_8003B870(void);
void func_800FBED0(void*);
void func_800FBE90(void*);
s32 func_80026190(u16, u32);
s32 func_800261CC(u16);
void func_800FBE50(void*);
void func_800265CC(void);
void func_80026624(void);
void func_80026240(void);
void func_800262CC(s32);
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
void func_800293B8(void);
void func_80110410(void);
void func_8004CF68(void);
void func_8004DF00(void);
u8 func_800263B0(s32);
void func_800264CC(s32);

void mainproc(void* arg) {
  func_800FC000();

  if (osTvType == 1) {
    func_801002F0(&D_8011D0F0);
    func_80105B00(90);
  } else if (osTvType == 2) {
    func_801002F0(&D_8011D9B0);
    func_80105B00(90);
  } else {
    while (1) {
    }
  }

  func_800FC000();
  func_80025E24();
  func_8004DF10();
  func_80025FF0();
}

void func_80025E24(void) {
  func_80025F04();
  func_8004CDA0();
  func_800268F0();
  func_80029000();
  func_80029170();
  func_80029B30();
  func_8002AFE0();
  func_800337D0();
  func_8003D970();
  func_80042F60();
  func_80045DE0();
  func_80046860();
  func_8004DEB0();
  func_8002DC70();
  func_8003B870();
  func_800FBED0(0);
  func_800FBED0(func_800265CC);
  func_800FBE90(func_80026624);
  func_80026190(0, (u32)func_80026240);
  func_800261CC(0);
  func_800FBE50(func_800262CC);
}

void func_80025F04(void) {
  u8 i;

  D_80205208 = 0;
  D_801594E4 = 0;
  D_8020564A = 0;
  D_80205634 = 0;
  D_8016FB04 = 0;
  D_801C3F71 = 0;
  D_801D6230 = 0;
  D_801C3F34 = 0;
  D_801C3BEC = 0;
  D_80237A04 = 0;
  D_8020520A = 0xFF;
  D_80237408 = 0;
  D_802373F1 = 1;
  D_802226E2 = 1;
  *(volatile u32*)&D_801C3B68 = 0;
  D_801C3B6C = 0;
  D_801C3B70 = 0;
  D_801C3B74 = 0;
  D_80205630 = 0;
  D_80204B38 = 0;
  D_801FD610 = 0;

  for (i = 0; i < 57; i++) {
    D_80188F70[i] = 0;
  }
}

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
