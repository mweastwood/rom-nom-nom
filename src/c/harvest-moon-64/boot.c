#include "boot.h"

void os_initialize(void);
void os_create_thread(OSThread* thread, OSId id, void (*entry)(void*), void* arg, void* sp,
                      OSPri pri);
void os_start_thread(OSThread* thread);
void os_set_thread_priority(OSThread* thread, OSPri pri);

void func_800FD5B0(void);
void func_800FB140(s32 arg0, s32 arg1);
void func_80105B00(s32 arg0);

void main(void) {
  os_initialize();
  os_create_thread(&g_idle_thread, 1, idle, NULL, g_idle_thread_stack, 10);
  os_start_thread(&g_idle_thread);
}

void idle(void* arg) {
  g_idle_callback = NULL;
  func_800FD5B0();
  func_800FB140(2, 1);
  func_80105B00(90);
  os_create_thread(&g_main_thread, 3, mainproc, NULL, g_main_thread_stack, 10);
  os_start_thread(&g_main_thread);
  os_set_thread_priority(&g_idle_thread, 0);

  while (1) {
    if (g_idle_callback != NULL) {
      g_idle_callback();
    }
  }
}
