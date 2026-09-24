#ifndef HARVEST_MOON_64_AUDIO_H
#define HARVEST_MOON_64_AUDIO_H

#include "types.h"

typedef struct {
  s32 unk_00;
  s32 unk_04;
  u8 unk_08[8];
  s32 unk_10;
  s32 unk_14;
  s32 unk_18;
  s32 unk_1C;
  s32 unk_20;
  s32 unk_24;
  u8 pad_28;
  u8 unk_29;
  u16 flags;
} AudioChannel;

typedef struct {
  s32 unk_00;
  s32 unk_04;
  s32 unk_08;
  s32 unk_0C;
  s32 unk_10;
  u8 pad_14;
  u8 unk_15;
  u16 flags;
} AudioVoice;

extern AudioChannel D_801FB5D8[4];
extern AudioVoice D_801FB690[4];

void AudioInit(s32 arg0);
void AudioUpdate(void);
void AudioSetMasterVolume(s32 arg0, s32 arg1);
s32 AudioChannelInit(u16 channel, s32 arg1, s32 arg2);
s32 AudioChannelSetup(u16 channel, s32 arg1, s32 arg2, s32 arg3, s32 arg4, s32 arg5, s32 arg6,
                      s32 arg7);
s32 AudioChannelStop(u16 channel);
s32 AudioChannelUpdateVolume(u16 channel, s32 arg1);

#endif
