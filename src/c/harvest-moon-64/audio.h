#ifndef HARVEST_MOON_64_AUDIO_H
#define HARVEST_MOON_64_AUDIO_H

#include "types.h"

typedef struct {
  s32 sequence_start;
  s32 sequence_end;
  u8 envelope[8];
  s32 volume;
  s32 tempo;
  s32 handle;
  s32 master_volume;
  s32 channel_volume;
  s32 fade_speed;
  u8 is_active;
  u8 priority;
  u16 flags;
} AudioChannel;

typedef struct {
  s32 sfx_id;
  s32 handle;
  s32 pitch;
  s32 pan;
  s32 volume;
  u8 is_active;
  u8 priority;
  u16 flags;
} AudioVoice;

extern AudioChannel D_801FB5D8[4];
#define g_audio_channels D_801FB5D8

extern AudioVoice D_801FB690[4];
#define g_audio_voices D_801FB690

void AudioInit(s32 arg0);
void AudioUpdate(void);
void AudioSetMasterVolume(s32 arg0, s32 arg1);
s32 AudioChannelInit(u16 channel, s32 arg1, s32 arg2);
s32 AudioChannelSetup(u16 channel, s32 arg1, s32 arg2, s32 arg3, s32 arg4, s32 arg5, s32 arg6,
                      s32 arg7);
s32 AudioChannelStop(u16 channel);
s32 AudioChannelUpdateVolume(u16 channel, s32 arg1);

#endif
