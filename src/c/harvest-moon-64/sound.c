#include "sound.h"

#include "audio.h"

void SoundSetMasterVolume(s32 index) {
  u32 idx = index & 0xFFFF;
  AudioSetMasterVolume(g_sound_volume_settings[idx].volume, g_sound_volume_settings[idx].fade_speed,
                       g_sound_volume_settings[idx].unused);
}

void SoundPlayBgm(s32 index) {
  u32 idx = index & 0xFFFF;
  if (idx < 0x40) {
    AudioChannelInit(0, g_sound_bgm_sequences[idx].sequence_start,
                     g_sound_bgm_sequences[idx].sequence_end);
  }
  AudioChannelSetPan(0, 0, 0);
}

void SoundFadeBgm(s32 index) {
  if ((u32)(index & 0xFFFF) < 0x40) {
    AudioChannelSetSpeed(0, 0x20);
  }
}

void SoundStopBgm(s32 index) {
  if ((u32)(index & 0xFFFF) < 0x40) {
    AudioChannelStop(0);
  }
}

s32 SoundIsBgmStopped(u16 index) {
  u16 result = 0;

  if (index < 0x40) {
    result = (g_audio_channels[0].flags < 1);
  }
  if (index == 0xFF) {
    result = 1;
  }
  return result;
}

void SoundSetBgmPan(s32 index, s32 pan) {
  if ((u32)(index & 0xFFFF) < 0x40) {
    AudioChannelSetPan(0, pan, 0x20);
  }
}

void SoundFadeOutBgm(s32 index) {
  if ((u32)(index & 0xFFFF) < 0x40) {
    AudioChannelSetPan(0, 0, 0x20);
  }
}

void SoundPlayEffect(s32 index) {
  s32 pad[2];
  u32 idx = index & 0xFFFF;

  if (idx < 0x80) {
    u8 channel = g_sound_sfx_channels[idx];

    if (channel == 0xFF) {
      s32 voice = idx + 1;
      AudioVoiceAllocate(voice);
      AudioVoiceSetVolume(voice, *(s32*)&g_sound_sfx_params[idx]);
      return;
    }
    AudioChannelInit(channel, g_sound_sfx_sequences[idx].sequence_start,
                     g_sound_sfx_sequences[idx].sequence_end);
    AudioChannelSetPan(g_sound_sfx_channels[idx], *(s32*)&g_sound_sfx_params[idx],
                       g_sound_sfx_params[idx].pan);
  }
}

s32 SoundAreVoicesInactive(void) {
  u8 count = 0;
  u8 i = 0;

  do {
    if (g_audio_voices[i].flags == 0) {
      count++;
    }
    i++;
  } while (i < 4);
  return count == 4;
}
