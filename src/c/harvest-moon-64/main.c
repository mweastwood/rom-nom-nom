#include "common.h"

extern u32 osTvType;
extern u8 D_8011D0F0;
extern u8 D_8011D9B0;

void func_800FC000(void);
void func_801002F0(void*);
void func_80105B00(s32);
void func_80025E24(void);
void func_8004DF10(void);
void func_80025FF0(void);

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
void func_80026190(s32, void*);
void func_800261CC(s32);
void func_800FBE50(void*);

void func_800265CC(void);
void func_80026624(void);
void func_80026240(void);
void func_800262CC(void);

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
  func_80026190(0, func_80026240);
  func_800261CC(0);
  func_800FBE50(func_800262CC);
}

extern volatile u8 D_80205208;
extern volatile u16 D_801594E4;
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
extern u32 D_801C3B68;
extern u32 D_801C3B6C;
extern u32 D_801C3B70;
extern u32 D_801C3B74;
extern u32 D_80205630;
extern u8 D_80204B38;
extern u16 D_801FD610;
extern u32 D_80188F70[];

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
