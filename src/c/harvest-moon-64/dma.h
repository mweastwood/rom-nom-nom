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

extern DmaRequest g_dma_requests[40];

extern u16 g_dma_pending_count;

void DmaInit(void);
s32 DmaQueueTransfer(u32 rom_addr, u32 vram_addr, u32 size);
void DmaProcessQueue(void);

#endif
