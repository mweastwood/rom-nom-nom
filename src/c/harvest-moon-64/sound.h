#ifndef HARVEST_MOON_64_SOUND_H
#define HARVEST_MOON_64_SOUND_H

#include "types.h"

typedef struct {
  s32 volume;
  s32 fade_speed;
  s32 unused;
} SoundVolumeSetting;

typedef struct {
  s32 sequence_start;
  s32 sequence_end;
} SoundSequence;

typedef struct {
  s16 volume;
  s16 pan;
} SoundSfxSetting;

extern SoundVolumeSetting g_sound_volume_settings[1];
extern SoundSequence g_sound_bgm_sequences[64];
extern SoundSequence g_sound_sfx_sequences[128];
extern u8 g_sound_sfx_channels[128];
extern SoundSfxSetting g_sound_sfx_params[128];

void SoundSetMasterVolume(s32 index);
void SoundPlayBgm(s32 index);
void SoundFadeBgm(s32 index);
void SoundStopBgm(s32 index);
s32 SoundIsBgmStopped(u16 index);
void SoundSetBgmPan(s32 index, s32 pan);
void SoundFadeOutBgm(s32 index);
void SoundPlayEffect(s32 index);
s32 SoundAreVoicesInactive(void);

#endif
