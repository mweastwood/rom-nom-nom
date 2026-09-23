#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

void os_initialize(void);
void os_create_thread(OSThread* thread, OSId id, void (*entry)(void*), void* arg, void* sp, OSPri pri);
void os_start_thread(OSThread* thread);
void os_set_thread_priority(OSThread* thread, OSPri pri);

void func_800FD5B0(void);
void func_800FB140(s32 arg0, s32 arg1);
void func_80105B00(s32 arg0);
void mainproc(void* arg);

extern OSThread D_801241C0;
extern u8 D_80126520[];
extern OSThread D_80124370;
extern u8 D_801C6220[];
extern void (*D_801FD628)(void);

void idle(void* arg);

void main(void) {
    os_initialize();
    os_create_thread(&D_801241C0, 1, idle, NULL, D_80126520, 10);
    os_start_thread(&D_801241C0);
}

void idle(void* arg) {
    D_801FD628 = NULL;
    func_800FD5B0();
    func_800FB140(2, 1);
    func_80105B00(90);
    os_create_thread(&D_80124370, 3, mainproc, NULL, D_801C6220, 10);
    os_start_thread(&D_80124370);
    os_set_thread_priority(&D_801241C0, 0);

    while (1) {
        if (D_801FD628 != NULL) {
            D_801FD628();
        }
    }
}

#ifdef __cplusplus
}
#endif
