#include "dma.h"

void func_800FD5E0(u32 rom_addr, u32 vram_addr, u32 size);

void DmaInit(void) {
  s32 i = 0;
  do {
    g_dma_requests[i & 0xFFFF].status = 0;
    i++;
  } while ((u32)(i & 0xFFFF) < 40);
  g_dma_pending_count = 0;
}
void func_80029000(void) __attribute__((alias("DmaInit")));

s32 DmaQueueTransfer(u32 rom_addr, u32 vram_addr, u32 size) {
  s32 ret = 0;
  u32 index = g_dma_pending_count & 0xFFFF;

  if (index < 40) {
    if (!(g_dma_requests[index].status & 1)) {
      ret = 1;
      g_dma_requests[index].rom_addr = rom_addr;
      g_dma_requests[index].vram_addr = vram_addr;
      g_dma_requests[index].size = size;
      g_dma_requests[index].status = 1;
      g_dma_pending_count++;
    }
  }

  return ret;
}
s32 func_8002903C(u32 rom_addr, u32 vram_addr, u32 size) __attribute__((alias("DmaQueueTransfer")));

void DmaProcessQueue(void) {
  s32 i = 0;
  do {
    if (g_dma_requests[i & 0xFFFF].status & 1) {
      func_800FD5E0(g_dma_requests[i & 0xFFFF].rom_addr, g_dma_requests[i & 0xFFFF].vram_addr,
                    g_dma_requests[i & 0xFFFF].size);
      g_dma_requests[i & 0xFFFF].status &= ~1;
    }
    i++;
  } while ((u32)(i & 0xFFFF) < 40);
  g_dma_pending_count = 0;
}
void func_800290B8(void) __attribute__((alias("DmaProcessQueue")));
