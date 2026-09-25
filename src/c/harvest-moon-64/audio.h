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

extern AudioChannel g_audio_channels[4];

extern AudioVoice g_audio_voices[4];

extern const f64 kAudioMinPitch;

extern const f64 kAudioMaxPitch;

void AudioInit(s32 arg0);
void AudioUpdate(void);
void AudioSetMasterVolume(s32 arg0, s32 arg1, s32 arg2);
s32 AudioChannelInit(u16 channel, s32 arg1, s32 arg2);
s32 AudioChannelSetup(u16 channel, s32 arg1, s32 arg2, s32 arg3, s32 arg4, s32 arg5, s32 arg6,
                      s32 arg7);
s32 AudioChannelStop(u16 channel);
s32 AudioChannelSetSpeed(u16 channel, s32 arg1);
s32 AudioChannelSetPan(u16 channel, s32 arg1, s16 arg2);
s32 AudioChannelSetVolume(u16 channel, s32 volume);
s32 AudioChannelSetPitch(u16 channel, s32 pitch);
void AudioCommandSend(s32 arg0);
s32 AudioVoiceAllocate(s32 arg0);
s32 AudioVoiceStop(s32 arg0);
s32 AudioVoiceSetVolume(s32 arg0, s32 arg1);
s32 AudioVoiceSetPitch(s32 arg0, s32 arg1);
s32 AudioVoiceSetPan(s32 arg0, s32 arg1);
void AudioCommandReset(void);

#endif
