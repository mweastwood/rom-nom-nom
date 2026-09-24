#ifndef HARVEST_MOON_64_DMA_H
#define HARVEST_MOON_64_DMA_H

#include "types.h"

typedef struct {
  u32 rom_addr;
  u32 vram_addr;
  u32 size;
  u16 status;
  u16 reserved;
} DmaRequest;

extern DmaRequest D_80158268[40];
#define g_dma_requests D_80158268

extern u16 D_801FADB2;
#define g_dma_pending_count D_801FADB2

void DmaInit(void);
s32 DmaQueueTransfer(u32 rom_addr, u32 vram_addr, u32 size);
void DmaProcessQueue(void);

#endif
