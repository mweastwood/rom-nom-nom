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

__asm__(".include \"asm/harvest-moon-64/1224.s\"");
