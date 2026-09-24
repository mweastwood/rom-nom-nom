#include "ultra64_mock.h"

#include <string.h>

MockUltra64State g_mock_state;

// Simulated BSS variables from ROM
OSThread g_idle_thread;
u8 g_idle_thread_stack[0x1000];
OSThread g_main_thread;
u8 g_main_thread_stack[0x1000];
void (*g_idle_callback)(void) = NULL;

void MockUltra64Reset(void) {
  memset(&g_mock_state, 0, sizeof(g_mock_state));
  memset(&g_idle_thread, 0, sizeof(g_idle_thread));
  memset(g_idle_thread_stack, 0, sizeof(g_idle_thread_stack));
  memset(&g_main_thread, 0, sizeof(g_main_thread));
  memset(g_main_thread_stack, 0, sizeof(g_main_thread_stack));
  g_idle_callback = NULL;
}

void os_initialize(void) {
  g_mock_state.os_initialize_calls++;
}

void os_create_thread(OSThread* thread, OSId id, void (*entry)(void*), void* arg, void* sp,
                      OSPri pri) {
  if (g_mock_state.os_create_thread_calls < 16) {
    MockThreadRecord* rec = &g_mock_state.created_threads[g_mock_state.os_create_thread_calls];
    rec->thread = thread;
    rec->id = id;
    rec->entry = entry;
    rec->arg = arg;
    rec->sp = sp;
    rec->pri = pri;
  }
  g_mock_state.os_create_thread_calls++;
}

void os_start_thread(OSThread* thread) {
  if (g_mock_state.os_start_thread_calls < 16) {
    g_mock_state.started_threads[g_mock_state.os_start_thread_calls] = thread;
  }
  g_mock_state.os_start_thread_calls++;
}

void os_set_thread_priority(OSThread* thread, OSPri pri) {
  if (g_mock_state.os_set_thread_priority_calls < 16) {
    g_mock_state.priority_changes[g_mock_state.os_set_thread_priority_calls].thread = thread;
    g_mock_state.priority_changes[g_mock_state.os_set_thread_priority_calls].pri = pri;
  }
  g_mock_state.os_set_thread_priority_calls++;
  if (g_mock_state.on_set_priority_hook != NULL) {
    g_mock_state.on_set_priority_hook(thread, pri);
  }
}

void func_800FD5B0(void) {
  g_mock_state.func_800FD5B0_calls++;
}

void func_800FB140(s32 arg0, s32 arg1) {
  g_mock_state.func_800FB140_calls++;
  g_mock_state.func_800FB140_last_arg0 = arg0;
  g_mock_state.func_800FB140_last_arg1 = arg1;
}

void func_80105B00(s32 arg0) {
  g_mock_state.func_80105B00_calls++;
  g_mock_state.func_80105B00_last_arg0 = arg0;
}

void mainproc(void* arg) {
  (void)arg;
  g_mock_state.mainproc_calls++;
}
