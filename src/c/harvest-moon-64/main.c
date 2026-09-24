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
void func_80025FF0(void);
void func_8004CDA0(void);
void func_800268F0(void);
void func_80029000(void);
void func_80029170(void);
void func_80029B30(void);
void func_8002AFE0(void);
void func_800337D0(void);
void func_8003D970(void);
void func_80042F60(void);
void func_80045DE0(void);
void func_80046860(void);
void func_8004DEB0(void);
void func_8002DC70(void);
void func_8003B870(void);
void func_800FBED0(void*);
void func_800FBE90(void*);
s32 func_80026190(u16, u32);
s32 func_800261CC(u16);
void func_800FBE50(void*);
void func_800265CC(void);
void func_80026624(void);
void func_80026240(void);
void func_800262CC(s32);
void func_80026284(void);
void func_8004DEC8(void);
void func_800FC030(void);
void func_80029CC8(void);
void func_8003CF38(void);
void func_80029284(void);
void func_80046D78(void);
void func_80033058(void);
void func_8003C6E4(void);
void func_8003A1BC(void);
void func_800467F8(void);
void func_8002D3D4(void);
void func_800290B8(void);
void func_8002AE58(void);
void func_80042634(void);
void func_800293B8(void);
void func_80110410(void);
void func_8004CF68(void);
void func_8004DF00(void);
u8 func_800263B0(s32);
void func_800264CC(s32);

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
  func_80025E24();
  func_8004DF10();
  func_80025FF0();
}

void func_80025E24(void) {
  func_80025F04();
  func_8004CDA0();
  func_800268F0();
  func_80029000();
  func_80029170();
  func_80029B30();
  func_8002AFE0();
  func_800337D0();
  func_8003D970();
  func_80042F60();
  func_80045DE0();
  func_80046860();
  func_8004DEB0();
  func_8002DC70();
  func_8003B870();
  func_800FBED0(0);
  func_800FBED0(func_800265CC);
  func_800FBE90(func_80026624);
  func_80026190(0, (u32)func_80026240);
  func_800261CC(0);
  func_800FBE50(func_800262CC);
}

void func_80025F04(void) {
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
  func_80026284();
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
      func_8003CF38();
      func_80029284();
      func_80046D78();
      func_80033058();
      func_8003C6E4();
      func_8003A1BC();
      func_800467F8();
      func_8002D3D4();
      func_800290B8();
      func_8002AE58();
      func_80042634();
      TextBoxUpdate();
      func_800293B8();
      g_vsync_flag = 0;
      g_screen_fade_alpha = g_fade_counter;
      func_80110410();
    }
    func_800FC000();
  }
}
void func_80025FF0(void) __attribute__((alias("MainLoop")));

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
s32 func_80026190(u16, u32) __attribute__((alias("RegisterGameLoopCallback")));

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
s32 func_800261CC(u16) __attribute__((alias("SetCurrentGameLoopCallback")));

void EnableSystemFlag4B38(void) {
  g_system_flag_4b38 = 1;
}
void func_8002620C(void) __attribute__((alias("EnableSystemFlag4B38")));

void DisableSystemFlag4B38(void) {
  g_system_flag_4b38 = 0;
}
void func_80026220(void) __attribute__((alias("DisableSystemFlag4B38")));

void SetLoopCounter(u16 counter_val) {
  g_frame_rate_divisor = counter_val;
}
void func_80026230(u16) __attribute__((alias("SetLoopCounter")));

void NoOpCallback(void) {}
void func_80026240(void) __attribute__((alias("NoOpCallback")));

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
void func_80026248(u32) __attribute__((alias("WaitVsyncFrames")));

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
void func_80026284(void) __attribute__((alias("WaitForSystemReady")));

void UpdateFrameCounterAndTicks(s32 arg0) {
  g_audio_tick_delta = arg0;
  g_main_loop_flags &= ~2;
  func_8004CF68();
  func_800263B0(arg0);
  func_800264CC(arg0);
  if (g_tick_counter_60hz >= 60) {
    g_tick_counter_60hz = 0;
    g_main_loop_flags |= 2;
  }
  func_8004DF00();
  g_tick_counter_60hz++;
  g_fade_counter++;
  g_anim_frame_counter++;
}
void func_800262CC(s32) __attribute__((alias("UpdateFrameCounterAndTicks")));

INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/main", func_800263B0);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/main", func_800264CC);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/main", func_800265CC);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/main", func_80026604);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/main", func_80026624);
