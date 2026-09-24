#ifndef HARVEST_MOON_64_BOOT_H
#define HARVEST_MOON_64_BOOT_H

#include "types.h"

extern OSThread g_idle_thread;

extern u8 g_idle_thread_stack[];

extern OSThread g_main_thread;

extern u8 g_main_thread_stack[];

extern void (*g_idle_callback)(void);
void main(void);
void idle(void* arg);
void mainproc(void* arg);

#endif
