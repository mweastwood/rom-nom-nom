#ifndef HARVEST_MOON_64_TIME_H
#define HARVEST_MOON_64_TIME_H

#include "types.h"

extern s32 g_time_ticks;

extern u8 g_day_of_month;

extern u8 g_time_stop_flag;

extern u8 g_display_season_char_0;

extern u8 g_display_season_char_1;

extern u8 g_display_season_char_2;

extern u8 g_display_season_char_3;

extern u8 g_display_season_char_4;

extern u8 g_display_season_char_5;

extern u8 g_weather;

extern u8 g_season;

extern s32 g_time_speed;

extern u8 g_time_dialogue_flag;

extern u8 g_time_pause_flag;

extern u8 g_time_counter;

extern u8 g_total_days;

extern u8 g_hour_of_day;

extern u8 g_time_tick_counter;

extern u16 g_scene_id;

extern u8 g_season_name_char_0;

extern u8 g_season_name_char_1;

extern u8 g_season_name_char_2;

extern u8 g_season_name_char_3;

extern u8 g_season_name_char_4;

extern u8 g_season_name_char_5;

extern u8 g_time_transition_flag;

extern u8 g_time_freeze_flag;

extern u8 g_indoor_flag;

extern u8 g_time_event_flag;

void TimeInit(void);
void TimeResetFlags(void);
void TimeUpdate(void);
void TimeAdvance(s32 arg0);
void TimeYearAdvance(void);
void TimeSeasonAdvance(void);
void TimeDayAdvance(void);
void TimeResetMonthlyFlags(void);
s32 TimeGetState(s32 arg0);

#endif
