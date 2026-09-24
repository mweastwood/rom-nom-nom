#ifndef HARVEST_MOON_64_MAIN_H
#define HARVEST_MOON_64_MAIN_H

#include "types.h"

/* --- Global Variables --- */

// N64 OS TV system type (1 = NTSC, 2 = MPAL)
extern u32 osTvType;
#define g_tv_type osTvType

// Video interface configuration modes
extern u8 D_8011D0F0;
#define g_vi_ntsc_mode D_8011D0F0

extern u8 D_8011D9B0;
#define g_vi_mpal_mode D_8011D9B0

// Main loop synchronization flags & counters
extern volatile u8 D_80205208;
#define g_vsync_flag D_80205208

extern volatile u16 D_801594E4;
#define g_main_loop_flags D_801594E4

extern volatile u16 D_80182BA0;
#define g_frame_rate_divisor D_80182BA0

extern volatile u16 D_8020564C;
#define g_frame_skip_counter D_8020564C

extern volatile u16 D_8020564A;
#define g_current_game_loop_index D_8020564A

extern volatile u8 D_80205634;
#define g_tick_counter_60hz D_80205634

extern volatile u8 D_8016FB04;
#define g_scene_transition_flag D_8016FB04

extern volatile u8 D_801C3F71;
#define g_anim_frame_counter D_801C3F71

extern volatile u8 D_801D6230;
#define g_fade_direction D_801D6230

extern volatile u8 D_801C3F34;
#define g_dialogue_state D_801C3F34

extern volatile u8 D_801C3BEC;
#define g_fade_counter D_801C3BEC

extern volatile u8 D_80237A04;
#define g_screen_fade_alpha D_80237A04

extern volatile u8 D_8020520A;
#define g_system_pause_flag D_8020520A

extern volatile u8 D_80237408;
#define g_event_trigger_flag D_80237408

extern u8 D_802373F1;
#define g_system_sub_state D_802373F1

extern volatile u8 D_802226E2;
#define g_audio_enable_flag D_802226E2

extern volatile s32 D_802226E8;
#define g_audio_tick_delta D_802226E8

extern u32 D_801C3B68;
#define g_debug_timer_0 D_801C3B68

extern u32 D_801C3B6C;
#define g_debug_timer_1 D_801C3B6C

extern u32 D_801C3B70;
#define g_debug_timer_2 D_801C3B70

extern u32 D_801C3B74;
#define g_debug_timer_3 D_801C3B74

extern u32 D_80205630;
#define g_debug_flags D_80205630

extern u8 D_80204B38;
#define g_system_flag_4b38 D_80204B38

extern u16 D_801FD610;
#define g_vblank_counter D_801FD610

extern volatile u32 D_80188F70[];
#define g_game_loop_callbacks D_80188F70

/* --- Function Prototypes --- */

void mainproc(void* arg);
void MainLoop(void);
s32 RegisterGameLoopCallback(u16 callback_index, u32 callback_ptr);
s32 SetCurrentGameLoopCallback(u16 callback_index);
void EnableSystemFlag4B38(void);
void DisableSystemFlag4B38(void);
void SetLoopCounter(u16 counter_val);
void NoOpCallback(void);
void WaitVsyncFrames(u32 frame_count);
void WaitForSystemReady(void);
void UpdateFrameCounterAndTicks(s32 arg0);

void func_80025E24(void);
void func_80025F04(void);

#endif
