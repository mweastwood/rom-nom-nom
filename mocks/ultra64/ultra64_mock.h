#ifndef ULTRA64_MOCK_H
#define ULTRA64_MOCK_H

#include "src/c/harvest-moon-64/common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    OSThread* thread;
    OSId id;
    void (*entry)(void*);
    void* arg;
    void* sp;
    OSPri pri;
} MockThreadRecord;

typedef struct {
    int os_initialize_calls;
    int os_create_thread_calls;
    MockThreadRecord created_threads[16];
    int os_start_thread_calls;
    OSThread* started_threads[16];
    int os_set_thread_priority_calls;
    struct {
        OSThread* thread;
        OSPri pri;
    } priority_changes[16];

    // Engine function call trackers
    int func_800FD5B0_calls;
    int func_800FB140_calls;
    s32 func_800FB140_last_arg0;
    s32 func_800FB140_last_arg1;
    int func_80105B00_calls;
    s32 func_80105B00_last_arg0;
    int mainproc_calls;

    // Optional test hooks
    void (*on_set_priority_hook)(OSThread* thread, OSPri pri);
} MockUltra64State;

extern MockUltra64State g_mock_state;

// Reset mock state before tests
void mock_ultra64_reset(void);

// Simulated global BSS buffers from Harvest Moon 64 ROM
extern OSThread D_801241C0;
extern u8 D_80126520[0x1000];
extern OSThread D_80124370;
extern u8 D_801C6220[0x1000];
extern void (*D_801FD628)(void);

// N64 OS declarations
void os_initialize(void);
void os_create_thread(OSThread* thread, OSId id, void (*entry)(void*), void* arg, void* sp, OSPri pri);
void os_start_thread(OSThread* thread);
void os_set_thread_priority(OSThread* thread, OSPri pri);

// Engine stubs
void func_800FD5B0(void);
void func_800FB140(s32 arg0, s32 arg1);
void func_80105B00(s32 arg0);
void mainproc(void* arg);

// Host declaration of boot's main
void boot_main(void);
void idle(void* arg);

#ifdef __cplusplus
}
#endif

#endif // ULTRA64_MOCK_H
