#ifndef HARVEST_MOON_64_TIME_H
#define HARVEST_MOON_64_TIME_H

#include "types.h"

extern s32 D_8013D5E4;
#define g_time_ticks D_8013D5E4

extern u8 D_80158260;
#define g_day_of_month D_80158260

extern u8 D_8017027F;
#define g_time_stop_flag D_8017027F

extern u8 D_80170445;
#define g_display_season_char_0 D_80170445

extern u8 D_80170446;
#define g_display_season_char_1 D_80170446

extern u8 D_80170447;
#define g_display_season_char_2 D_80170447

extern u8 D_80170448;
#define g_display_season_char_3 D_80170448

extern u8 D_80170449;
#define g_display_season_char_4 D_80170449

extern u8 D_8017044A;
#define g_display_season_char_5 D_8017044A

extern u8 D_80170458;
#define g_weather D_80170458

extern u8 D_80182DB1;
#define g_season D_80182DB1

extern s32 D_801891D4;
#define g_time_speed D_801891D4

extern u8 D_801C3F44;
#define g_time_dialogue_flag D_801C3F44

extern u8 D_801C3F7E;
#define g_time_pause_flag D_801C3F7E

extern u8 D_801F6F30;
#define g_time_counter D_801F6F30

extern u8 D_801FAD90;
#define g_total_days D_801FAD90

extern u8 D_801FB5CA;
#define g_hour_of_day D_801FB5CA

extern u8 D_801FD620;
#define g_time_tick_counter D_801FD620

extern u16 D_801FD622;
#define g_scene_id D_801FD622

extern u8 D_80204FB1;
#define g_season_name_char_0 D_80204FB1

extern u8 D_80204FB2;
#define g_season_name_char_1 D_80204FB2

extern u8 D_80204FB3;
#define g_season_name_char_2 D_80204FB3

extern u8 D_80204FB4;
#define g_season_name_char_3 D_80204FB4

extern u8 D_80204FB5;
#define g_season_name_char_4 D_80204FB5

extern u8 D_80204FB6;
#define g_season_name_char_5 D_80204FB6

extern u8 D_80204FF8;
#define g_time_transition_flag D_80204FF8

extern u8 D_80215DF2;
#define g_time_freeze_flag D_80215DF2

extern u8 D_802373A9;
#define g_indoor_flag D_802373A9

extern u8 D_80237409;
#define g_time_event_flag D_80237409

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
