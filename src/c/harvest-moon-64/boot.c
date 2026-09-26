#include "boot.h"

void OsInitialize(void);
void OsCreateThread(OSThread* thread, OSId id, void (*entry)(void*), void* arg, void* sp,
                    OSPri pri);
void OsStartThread(OSThread* thread);
void OsSetThreadPriority(OSThread* thread, OSPri pri);

void func_800FD5B0(void);
void func_800FB140(s32 arg0, s32 arg1);
void func_80105B00(s32 arg0);

void main(void) {
  OsInitialize();
  OsCreateThread(&g_idle_thread, 1, idle, NULL, g_idle_thread_stack, 10);
  OsStartThread(&g_idle_thread);
}

void idle(void* arg) {
  g_idle_callback = NULL;
  func_800FD5B0();
  func_800FB140(2, 1);
  func_80105B00(90);
  OsCreateThread(&g_main_thread, 3, mainproc, NULL, g_main_thread_stack, 10);
  OsStartThread(&g_main_thread);
  OsSetThreadPriority(&g_idle_thread, 0);

  while (1) {
    if (g_idle_callback != NULL) {
      g_idle_callback();
    }
  }
}
