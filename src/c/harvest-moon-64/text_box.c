#include "message.h"

void func_80044D78(u16);
void func_80045260(u16);

void TextBoxUpdate(void) {
  u16 i = 0;
  u32 mask = 0x20000;
  do {
    u16 flags = D_801C3F00[i].flags;
    if ((flags & 1) && (flags & 2)) {
      u8 handled = 0;
      if (flags & 0x10) {
        u32 val = D_80188C00[D_801C3F00[i].bank_id].flags;
        if ((val & 4) || (val & mask)) {
          D_801C3F00[i].flags = flags & ~0x10;
        }
        handled = 1;
      }
      if (D_801C3F00[i].flags & 0x20) {
        func_80044D78(i);
        handled = 1;
      }
      if (!handled) {
        func_80045260(i);
      }
    }
    i++;
  } while (i < 1);
}

void func_80045CB0(void) __attribute__((alias("TextBoxUpdate")));
