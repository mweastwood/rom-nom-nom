#include "game_time.h"

extern void func_8003FBD8(s32, u8, s32);
extern void func_8005C07C(s32, s32);
extern void func_8005C940(s32, s32);
extern void func_80065074(s32);
extern void func_800650A8(s32);
extern s32 func_800650E0(s32);
extern void func_80065140(s32);
extern void func_80065270(s32);
extern void func_800661FC(s32, s32);
extern void func_8006EA44(u8, u8, u8);
extern s32 func_8006EC40(u8);
extern void func_800A7AE8(u8);
extern void func_800ACB5C(u16);
extern s32 func_800ACBB8(u16);
extern void func_800ACBEC(u16, s32);

void TimeSleep(void);
s32 TimeCheckFestival(void);
void TimeUpdateSeason(void);
void TimeSetSeasonName(s32 arg0);

void TimeUpdate(void) {
  s32 flag;

  if ((func_8006EC40(g_weather) & 1) && (func_800650E0(6) == 0)) {
    TimeAdvance(1);
  }
  if ((g_hour_of_day == 0) && (g_time_stop_flag == 0) && (g_time_freeze_flag == 0)) {
    TimeUpdateSeason();
  }
  if (g_hour_of_day == 6) {
    if (func_800650E0(0xE) == 0) {
      func_800661FC(0xFE, 0);
      func_8005C940(1, 0xE);
    }
  } else {
    func_800650A8(0xE);
    if (g_time_speed >= 0) {
      func_8005C07C(1, 0);
    }
  }
  if ((u32)(g_hour_of_day - 0x12) < 6U) {
    if (func_800650E0(0xF) == 0) {
      func_800ACB5C(g_scene_id);
      flag = 0xF;
      goto call_flag;
    }
    if ((func_800650E0(0x10) == 0) && (g_scene_id != 0xFF) && (func_800ACBB8(g_scene_id) & 0xFF)) {
      func_8006EA44(g_weather, g_season, g_hour_of_day);
      func_800ACBEC(g_scene_id, g_time_ticks);
      flag = 0x10;
    call_flag:
      func_80065074(flag);
    }
  }
  if ((g_hour_of_day < 6U) && (g_indoor_flag != 2) && (g_weather != 9) &&
      (func_800650E0(0x11) == 0)) {
    func_800ACB5C(g_scene_id);
    func_80065074(0x11);
  }
  func_800A7AE8(g_weather);
}
void func_800D7C20(void) __attribute__((alias("TimeUpdate")));

void TimeAdvance(s32 arg0) {
  s32 temp_a0;
  u8 temp_v0;

  temp_a0 = arg0 & 0xFF;
  if (temp_a0 == 1) {
    g_time_freeze_flag += 0xA;
  }
  if (g_time_freeze_flag >= 0x3CU) {
    g_time_freeze_flag = 0;
    g_time_stop_flag += 1;
  }
  if (g_time_stop_flag >= 0x3CU) {
    g_time_stop_flag = 0;
    temp_v0 = g_hour_of_day + 1;
    g_hour_of_day = temp_v0;
    if ((temp_v0 & 0xFF) == 6) {
      g_day_of_month += 1;
      g_total_days += 1;
    }
  }
  if (g_hour_of_day >= 0x18U) {
    g_hour_of_day = 0;
  }
  if (g_total_days >= 7U) {
    g_total_days = 0;
  }
  if (g_day_of_month >= 0x1FU) {
    g_day_of_month = 1;
    g_season += 1;
    TimeResetMonthlyFlags();
  }
  if (g_season >= 5U) {
    g_season = 1;
    g_time_counter += 1;
    TimeYearAdvance();
  }
  if (g_time_counter >= 0x64U) {
    g_time_counter = 0x63;
  }
  g_time_tick_counter = g_season;
  if ((g_day_of_month + 1) >= 0x1F) {
    g_time_tick_counter = g_season + 1;
  }
  if (g_time_tick_counter >= 5U) {
    g_time_tick_counter = 1;
  }
}
void func_800D7E28(s32 arg0) __attribute__((alias("TimeAdvance")));

void TimeYearAdvance(void) {
  u8 temp_v1;

  temp_v1 = g_time_event_flag;
  g_time_dialogue_flag = 0xFF;
  g_time_event_flag = 0xFF;
  g_time_pause_flag = 0xFF;
  g_time_transition_flag = temp_v1;
  func_80065140(0x44);
  func_80065270(0);
  func_80065270(1);
  func_80065270(2);
  func_80065270(3);
  func_80065270(4);
  func_80065270(5);
  func_80065270(6);
  func_80065270(7);
  func_80065270(8);
  func_80065270(9);
  func_80065270(0xA);
  func_80065270(0xB);
  func_80065270(0xC);
  func_80065270(0xD);
  func_80065270(0xE);
  func_80065270(0xF);
  func_80065270(0x10);
  func_80065270(0x11);
  func_80065270(0x12);
  func_80065270(0x13);
  func_80065270(0x14);
  func_80065270(0x15);
  func_80065270(0x17);
  func_80065270(0x19);
  func_80065270(0x1D);
  func_80065270(0x20);
  func_80065270(0x21);
  func_80065270(0x22);
  func_80065270(0x3B);
  func_80065270(0x3D);
  func_80065270(0x3E);
  func_80065270(0x3F);
  func_80065270(0x41);
  func_80065270(0x42);
  func_80065270(0x43);
  func_80065270(0x45);
  func_80065270(0x46);
  func_80065270(0x49);
}
void func_800D8004(void) __attribute__((alias("TimeYearAdvance")));

void TimeResetMonthlyFlags(void) {
  func_80065270(0x3C);
  func_80065270(0x40);
}
void func_800D8180(void) __attribute__((alias("TimeResetMonthlyFlags")));

void TimeSleep(void) {
  g_hour_of_day = 6;
  g_time_stop_flag = 0;
  g_time_freeze_flag = 0;
  g_day_of_month += 1;
  g_total_days += 1;
  TimeAdvance(0);
}
void func_800D81A8(void) __attribute__((alias("TimeSleep")));

s32 TimeCheckFestival(void) {
  s32 var_a1;

  var_a1 = 0;
  switch (g_season) {
    case 1:
      if (((g_day_of_month == 1) & (g_day_of_month == 8)) &&
          ((g_day_of_month == 0x11) & (g_day_of_month == 0x17))) {
        var_a1 = 1;
      }
      break;
    case 2:
      if (((g_day_of_month == 1) & (g_day_of_month == 9)) &&
          ((g_day_of_month == 0x11) & (g_day_of_month == 0x18))) {
        var_a1 = 1;
      }
      break;
    case 3:
      if (((g_day_of_month == 4) & (g_day_of_month == 0xC)) &&
          ((g_day_of_month == 0x14) & (g_day_of_month == 0x1C))) {
        var_a1 = 1;
      }
      break;
    case 4:
      if (((g_day_of_month == 6) & (g_day_of_month == 0x13)) && (g_day_of_month == 0x18)) {
        var_a1 = 1;
      }
      break;
  }
  return var_a1;
}
s32 func_800D820C(void) __attribute__((alias("TimeCheckFestival")));

void TimeUpdateSeason(void) {
  TimeSetSeasonName(g_season);
  g_display_season_char_0 = g_season_name_char_0;
  g_display_season_char_1 = g_season_name_char_1;
  g_display_season_char_2 = g_season_name_char_2;
  g_display_season_char_3 = g_season_name_char_3;
  g_display_season_char_4 = g_season_name_char_4;
  g_display_season_char_5 = g_season_name_char_5;
  func_8003FBD8(0x15, g_day_of_month, 1);
}
void func_800D835C(void) __attribute__((alias("TimeUpdateSeason")));

void TimeSetSeasonName(s32 arg0) {
  switch (arg0 & 0xFF) {
    case 1:
      g_season_name_char_0 = 0xB3;
      g_season_name_char_1 = 0xCA;
      g_season_name_char_2 = 0xCC;
      g_season_name_char_3 = 0xC3;
      g_season_name_char_4 = 0xC8;
      g_season_name_char_5 = 0xC1;
      break;
    case 2:
      g_season_name_char_0 = 0xB3;
      g_season_name_char_1 = 0xCF;
      g_season_name_char_2 = 0xC7;
      g_season_name_char_3 = 0xC7;
      g_season_name_char_4 = 0xBF;
      g_season_name_char_5 = 0xCC;
      break;
    case 3:
      g_season_name_char_0 = 0xA1;
      g_season_name_char_1 = 0xCF;
      g_season_name_char_2 = 0xCE;
      g_season_name_char_3 = 0xCF;
      g_season_name_char_4 = 0xC7;
      g_season_name_char_5 = 0xC8;
      break;
    case 4:
      g_season_name_char_0 = 0xB7;
      g_season_name_char_1 = 0xC3;
      g_season_name_char_2 = 0xC8;
      g_season_name_char_3 = 0xCE;
      g_season_name_char_4 = 0xBF;
      g_season_name_char_5 = 0xCC;
      break;
  }
}
void func_800D83F8(s32 arg0) __attribute__((alias("TimeSetSeasonName")));
