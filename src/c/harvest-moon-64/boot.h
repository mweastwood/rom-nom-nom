#ifndef HARVEST_MOON_64_BOOT_H
#define HARVEST_MOON_64_BOOT_H

#include "types.h"

extern OSThread D_801241C0;
#define g_idle_thread D_801241C0

extern u8 D_80126520[];
#define g_idle_thread_stack D_80126520

extern OSThread D_80124370;
#define g_main_thread D_80124370

extern u8 D_801C6220[];
#define g_main_thread_stack D_801C6220

extern void (*D_801FD628)(void);
#define g_idle_callback D_801FD628

void main(void);
void idle(void* arg);
void mainproc(void* arg);

#endif
