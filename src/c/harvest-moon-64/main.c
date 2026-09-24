#include "main.h"

#include "audio.h"
#include "boot.h"
#include "display.h"
#include "dma.h"
#include "game_time.h"
#include "include_asm.h"
#include "layer.h"
#include "message.h"
#include "types.h"

void func_800FC000(void);
void func_801002F0(void*);
void func_80105B00(s32);
void func_8004DF10(void);
void func_8004CDA0(void);
void func_800268F0(void);
void func_80029B30(void);
void func_8002AFE0(void);
void func_800337D0(void);
void func_8003D970(void);
void func_80046860(void);
void func_8004DEB0(void);
void func_8002DC70(void);
void func_8003B870(void);
void func_800FBED0(void*);
void func_800FBE90(void*);
void func_800FBE50(void*);
void func_800FBF10(void);
void func_800FBF30(void);
void func_800FFB50(void*);
void func_80100340(f32);
void func_8004DEC8(void);
void func_800FC030(void);
void func_80029CC8(void);
void func_80046D78(void);
void func_80033058(void);
void func_8003C6E4(void);
void func_8003A1BC(void);
void func_800467F8(void);
void func_8002D3D4(void);
void func_8002AE58(void);
void func_80042634(void);
void func_80110410(void);
void func_8004CF68(void);
void func_80026A98(void);
s32 func_800FFF30(s32);

void mainproc(void* arg) {
  func_800FC000();

  if (g_tv_type == 1) {
    func_801002F0(&g_vi_ntsc_mode);
    func_80105B00(90);
  } else if (g_tv_type == 2) {
    func_801002F0(&g_vi_mpal_mode);
    func_80105B00(90);
  } else {
    while (1) {
    }
  }

  func_800FC000();
  MainInit();
  func_8004DF10();
  MainLoop();
}

void MainInit(void) {
  MainReset();
  func_8004CDA0();
  func_800268F0();
  DmaInit();
  RenderInit();
  func_80029B30();
  func_8002AFE0();
  func_800337D0();
  func_8003D970();
  MessageInit();
  LayerInit();
  func_80046860();
  func_8004DEB0();
  func_8002DC70();
  func_8003B870();
  func_800FBED0(0);
  func_800FBED0(MainCallback);
  func_800FBE90(MainShutdown);
  RegisterGameLoopCallback(0, (u32)NoOpCallback);
  SetCurrentGameLoopCallback(0);
  func_800FBE50(UpdateFrameCounterAndTicks);
}

void MainReset(void) {
  u8 i;

  g_vsync_flag = 0;
  g_main_loop_flags = 0;
  g_current_game_loop_index = 0;
  g_tick_counter_60hz = 0;
  g_scene_transition_flag = 0;
  g_anim_frame_counter = 0;
  g_fade_direction = 0;
  g_dialogue_state = 0;
  g_fade_counter = 0;
  g_screen_fade_alpha = 0;
  g_system_pause_flag = 0xFF;
  g_event_trigger_flag = 0;
  g_system_sub_state = 1;
  g_audio_enable_flag = 1;
  *(volatile u32*)&g_debug_timer_0 = 0;
  g_debug_timer_1 = 0;
  g_debug_timer_2 = 0;
  g_debug_timer_3 = 0;
  g_debug_flags = 0;
  g_system_flag_4b38 = 0;
  g_vblank_counter = 0;

  for (i = 0; i < 57; i++) {
    g_game_loop_callbacks[i] = 0;
  }
}

void MainLoop(void) {
  void (*func)(void);

  g_vsync_flag = 0;
  g_main_loop_flags = 1;
  WaitForSystemReady();
  func_8004DEC8();
  g_frame_rate_divisor = 1;
  g_frame_skip_counter = 0;

  while (1) {
    func_800FC030();
    while (g_main_loop_flags & 1) {
      while (g_vsync_flag == 0) {
      }
      if (g_frame_skip_counter == 0) {
        g_frame_rate_divisor = 1;
        func = (void (*)(void))g_game_loop_callbacks[g_current_game_loop_index];
        func();
        g_frame_skip_counter = g_frame_rate_divisor;
      }
      g_frame_skip_counter--;
      func_80029CC8();
      AudioUpdate();
      RenderResetCount();
      func_80046D78();
      func_80033058();
      func_8003C6E4();
      func_8003A1BC();
      func_800467F8();
      func_8002D3D4();
      DmaProcessQueue();
      func_8002AE58();
      func_80042634();
      TextBoxUpdate();
      RenderNoOp2();
      g_vsync_flag = 0;
      g_screen_fade_alpha = g_fade_counter;
      func_80110410();
    }
    func_800FC000();
  }
}

s32 RegisterGameLoopCallback(u16 callback_index, u32 callback_ptr) {
  s32 ret = 0;
  if (callback_index < 57) {
    if (g_game_loop_callbacks[callback_index] == 0) {
      g_game_loop_callbacks[callback_index] = callback_ptr;
      ret = 1;
    }
  }
  return ret;
}

s32 SetCurrentGameLoopCallback(u16 callback_index) {
  s32 ret = 0;
  if (callback_index < 57) {
    if (g_game_loop_callbacks[callback_index] != 0) {
      g_current_game_loop_index = callback_index;
      ret = 1;
    }
  }
  return ret;
}

void EnableSystemFlag4B38(void) {
  g_system_flag_4b38 = 1;
}

void DisableSystemFlag4B38(void) {
  g_system_flag_4b38 = 0;
}

void SetLoopCounter(u16 counter_val) {
  g_frame_rate_divisor = counter_val;
}

void NoOpCallback(void) {}

void WaitVsyncFrames(u32 frame_count) {
  u16 i = 1;
  frame_count &= 0xFFFF;
  if (frame_count != 0) {
    do {
      g_vsync_flag = 0;
      while (g_vsync_flag == 0) {
      }
    } while (i++ < frame_count);
  }
}

void WaitForSystemReady(void) {
  u16 i;
  while (!(g_main_loop_flags & 2)) {
    i = 1;
    do {
      g_vsync_flag = 0;
      while (g_vsync_flag == 0) {
      }
    } while (i++ == 0);
  }
}

void UpdateFrameCounterAndTicks(s32 arg0) {
  g_audio_tick_delta = arg0;
  g_main_loop_flags &= ~2;
  func_8004CF68();
  UpdateGraphicTiming(arg0);
  UpdateFrameTiming(arg0);
  if (g_tick_counter_60hz >= 60) {
    g_tick_counter_60hz = 0;
    g_main_loop_flags |= 2;
  }
  func_8004DF00();
  g_tick_counter_60hz++;
  g_fade_counter++;
  g_anim_frame_counter++;
}

void UpdateGraphicTiming(s32 arg0) {
  if (g_tick_counter_60hz % (u32)g_system_sub_state == 0) {
    if (g_tick_counter_60hz >= 60) {
      g_last_scene_frame_count = g_scene_transition_flag;
      g_scene_transition_flag = 0;
      if (g_last_scene_frame_count < 60 / g_system_sub_state) {
        g_frame_drop_flag = 2;
      }
    }
    if (arg0 < 3) {
      if (g_vsync_flag == 0) {
        func_80026A98();
        g_scene_transition_flag++;
        g_anim_frame_counter = 0;
      }
    }
  }
}

void UpdateFrameTiming(s32 arg0) {
  if (g_tick_counter_60hz % (u32)g_audio_enable_flag == 0) {
    if (g_tick_counter_60hz >= 60) {
      g_last_frame_count = g_dialogue_state;
      g_dialogue_state = 0;
      if (g_last_frame_count < 60 / g_audio_enable_flag) {
        g_frame_drop_flag = 1;
      }
    }
    if (g_vsync_flag == 0) {
      g_vsync_flag = 1;
      g_fade_counter = 0;
      g_dialogue_state++;
    }
  }
}

void MainCallback(s32 arg0) {
  g_main_callback_arg = arg0;
  func_800FBF10();
  g_fade_direction = g_anim_frame_counter;
}

void MainStopThread(void) {
  func_800FFF30(0);
}

void MainShutdown(void) {
  OSThread* thread;

  if (g_game_loop_callbacks[0] != 0) {
    g_current_game_loop_index = 0;
  }
  func_800FBE50(0);
  func_800FBF30();
  func_800FC000();
  thread = &g_system_thread_2;
  func_800FFB50(thread);
  func_800FFB50((void*)((u32)thread - 0x1B0));
  {
    f32 scale = 1.0f;
    __asm__("nop" : : "f"(scale));
    func_80100340(scale);
  }
  do {
  } while (func_800FFF30(0) != 0);
  while (1) {
  }
}
