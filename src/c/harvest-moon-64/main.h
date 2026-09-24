#ifndef HARVEST_MOON_64_MAIN_H
#define HARVEST_MOON_64_MAIN_H

#include "types.h"

/* --- Global Variables --- */

// N64 OS TV system type (1 = NTSC, 2 = MPAL)
extern u32 g_tv_type;

// Video interface configuration modes
extern u8 g_vi_ntsc_mode;

extern u8 g_vi_mpal_mode;

// Main loop synchronization flags & counters
extern volatile u8 g_vsync_flag;

extern volatile u16 g_main_loop_flags;

extern volatile u16 g_frame_rate_divisor;

extern volatile u16 g_frame_skip_counter;

extern volatile u16 g_current_game_loop_index;

extern volatile u8 g_tick_counter_60hz;

extern volatile u8 g_scene_transition_flag;

extern volatile u8 g_anim_frame_counter;

extern volatile u8 g_fade_direction;

extern volatile u8 g_dialogue_state;

extern volatile u8 g_fade_counter;

extern volatile u8 g_screen_fade_alpha;

extern volatile u8 g_system_pause_flag;

extern volatile u8 g_event_trigger_flag;

extern volatile u8 g_system_sub_state;

extern volatile u8 g_audio_enable_flag;

extern volatile s32 g_audio_tick_delta;

extern u32 g_debug_timer_0;

extern u32 g_debug_timer_1;

extern u32 g_debug_timer_2;

extern u32 g_debug_timer_3;

extern u32 g_debug_flags;

extern u8 g_system_flag_4b38;

extern u16 g_vblank_counter;

extern volatile s32 g_main_callback_arg;

extern OSThread g_system_thread_1;

extern OSThread g_system_thread_2;

extern volatile u8 g_last_frame_count;

extern volatile u8 g_last_scene_frame_count;

extern volatile s8 g_frame_drop_flag;

extern volatile u32 g_game_loop_callbacks[];

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
void UpdateGraphicTiming(s32 arg0);
void UpdateFrameTiming(s32 arg0);

void MainInit(void);
void MainReset(void);
void MainCallback(s32 arg0);
void MainStopThread(void);
void MainShutdown(void);

#endif
