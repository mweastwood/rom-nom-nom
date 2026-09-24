#include "dma.h"

void func_800FD5E0(u32 rom_addr, u32 vram_addr, u32 size);

void DmaInit(void) {
  s32 i = 0;
  do {
    D_80158268[i & 0xFFFF].status = 0;
    i++;
  } while ((u32)(i & 0xFFFF) < 40);
  D_801FADB2 = 0;
}
void func_80029000(void) __attribute__((alias("DmaInit")));

s32 DmaQueueTransfer(u32 rom_addr, u32 vram_addr, u32 size) {
  s32 ret = 0;
  u32 index = D_801FADB2 & 0xFFFF;

  if (index < 40) {
    if (!(D_80158268[index].status & 1)) {
      ret = 1;
      D_80158268[index].rom_addr = rom_addr;
      D_80158268[index].vram_addr = vram_addr;
      D_80158268[index].size = size;
      D_80158268[index].status = 1;
      D_801FADB2++;
    }
  }

  return ret;
}
s32 func_8002903C(u32 rom_addr, u32 vram_addr, u32 size) __attribute__((alias("DmaQueueTransfer")));

void DmaProcessQueue(void) {
  s32 i = 0;
  do {
    if (D_80158268[i & 0xFFFF].status & 1) {
      func_800FD5E0(D_80158268[i & 0xFFFF].rom_addr, D_80158268[i & 0xFFFF].vram_addr,
                    D_80158268[i & 0xFFFF].size);
      D_80158268[i & 0xFFFF].status &= ~1;
    }
    i++;
  } while ((u32)(i & 0xFFFF) < 40);
  D_801FADB2 = 0;
}
void func_800290B8(void) __attribute__((alias("DmaProcessQueue")));
