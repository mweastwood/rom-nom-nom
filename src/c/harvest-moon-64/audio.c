#include "audio.h"

#include "include_asm.h"

extern void func_800266C0(void*, s32, s32, s32, s32);
extern void func_800266F8(void*);
extern void func_800F2470(s32);
extern void func_800F268C(s32, s32, s32);
extern s32 func_800F2704(s32);
extern s32 func_800F2740(s32);
extern void func_800F4B68(s32, s32);
extern void func_800F5318(s32, s32);
extern s8 func_800F5404(s32);
extern void func_800F54C0(s32, s32);
extern void func_800F558C(s32, s32);
extern void func_800F5664(s32, f32);

void AudioInit(s32 arg0) {
  u16 i = 0;
  u16 j;

  do {
    g_audio_channels[i].flags = 0;
    g_audio_channels[i].handle = 0;
    g_audio_channels[i].is_active = 0;
    g_audio_channels[i].master_volume = 0;
    g_audio_channels[i].channel_volume = 0;
    g_audio_voices[i].volume = 0;
    g_audio_channels[i].fade_speed = 0;
    func_800266C0(&g_audio_channels[i].envelope, 0, 0, 0, 0);
    i++;
  } while (i < 4);

  j = 0;
  do {
    g_audio_voices[j].flags = 0;
    g_audio_voices[j].sfx_id = 0;
    g_audio_voices[j].handle = 0;
    g_audio_voices[j].is_active = 0;
    g_audio_voices[j].pitch = 0;
    g_audio_voices[j].pan = 0;
    g_audio_voices[j].volume = 0;
    j++;
  } while (j < 4);

  func_800F2470(arg0);
  func_800F4B68(2, 0x7FFF);
  func_800F4B68(1, 0x7FFF);
}

void AudioUpdate(void) {
  u16 j;
  u16 i = 0;

  do {
    if (g_audio_channels[i].flags & 1) {
      if (g_audio_channels[i].flags & 2) {
        u8 channel = (u8)i;
        func_800F268C(channel, g_audio_channels[i].sequence_start,
                      g_audio_channels[i].sequence_end - g_audio_channels[i].sequence_start);
        g_audio_channels[i].handle = func_800F2704(channel);
        g_audio_channels[i].flags &= ~2;
      }
      if (g_audio_channels[i].flags & 4) {
        func_800F5318(g_audio_channels[i].handle, g_audio_channels[i].fade_speed);
        g_audio_channels[i].flags &= ~4;
      }
      func_800266F8(&g_audio_channels[i].envelope);
      func_800F54C0(g_audio_channels[i].handle, g_audio_channels[i].volume);
      g_audio_channels[i].is_active = func_800F5404(g_audio_channels[i].handle);
      if (!g_audio_channels[i].is_active) {
        g_audio_channels[i].flags = 0;
        func_800F5318(g_audio_channels[i].handle, 1);
      }
    }
    i++;
  } while (i < 4);

  j = 0;
  do {
    if (g_audio_voices[j].flags & 1) {
      if (g_audio_voices[j].flags & 2) {
        g_audio_voices[j].handle = func_800F2740(g_audio_voices[j].sfx_id);
        g_audio_voices[j].flags &= ~2;
      }
      if (g_audio_voices[j].flags & 4) {
        func_800F5318(g_audio_voices[j].handle, 0);
        g_audio_voices[j].flags &= ~4;
      }
      {
        f32 freq = (f32)g_audio_voices[j].pitch;
        __asm__("nop" : : "r"(freq));
        func_800F5664(g_audio_voices[j].handle, freq);
      }
      func_800F558C(g_audio_voices[j].handle, g_audio_voices[j].pan);
      func_800F54C0(g_audio_voices[j].handle, g_audio_voices[j].volume);
      g_audio_voices[j].is_active = func_800F5404(g_audio_voices[j].handle);
      if (!g_audio_voices[j].is_active) {
        g_audio_voices[j].flags = 0;
      }
    }
    j++;
  } while (j < 4);
}

extern void func_800F2500(s32, s32);
extern void func_800267A4(void*, s16, s16);
extern void func_800F5130(s32, s32);

void AudioSetMasterVolume(s32 arg0, s32 arg1) {
  func_800F2500(arg0, arg1 - arg0);
}

s32 AudioChannelInit(u16 channel, s32 arg1, s32 arg2) {
  s32 result = 0;

  if (channel < 4) {
    if (!(g_audio_channels[channel].flags & 1)) {
      func_800266C0(&g_audio_channels[channel].envelope, 0, 0, 0, 0);
      result = 1;
      g_audio_channels[channel].master_volume = 0x80;
      g_audio_channels[channel].channel_volume = 0x80;
      g_audio_channels[channel].sequence_start = arg1;
      g_audio_channels[channel].sequence_end = arg2;
      g_audio_channels[channel].flags = 3;
    }
  }
  return result;
}

s32 AudioChannelSetSpeed(u16 channel, s32 arg1) {
  s32 result = 0;

  if (channel < 4 && (g_audio_channels[channel].flags & 1)) {
    result = 1;
    g_audio_channels[channel].fade_speed = arg1;
    g_audio_channels[channel].flags |= 4;
  }
  return result;
}

s32 AudioChannelStop(u16 channel) {
  s32 result = 0;

  if (channel < 4) {
    if (g_audio_channels[channel].flags & 1) {
      func_800F5318(g_audio_channels[channel].handle, 0);
      result = 1;
      g_audio_channels[channel].flags = 0;
    }
  }
  return result;
}

s32 AudioChannelSetPan(u16 channel, s32 arg1, s16 arg2) {
  s32 result = 0;

  if (channel < 4 && (g_audio_channels[channel].flags & 1)) {
    if (arg1 < 0) {
      arg1 = 0;
    }
    if (arg1 >= 0x101) {
      arg1 = 0x100;
    }
    g_audio_channels[channel].tempo = arg1;
    func_800267A4(&g_audio_channels[channel].envelope, arg2, (s16)arg1);
    result = 1;
  }
  return result;
}

s32 AudioChannelSetVolume(u16 channel, s32 arg1) {
  s32 result = 0;

  if (channel < 4 && (g_audio_channels[channel].flags & 1)) {
    g_audio_channels[channel].master_volume = arg1;
    if (arg1 < 0) {
      g_audio_channels[channel].master_volume = 0;
    }
    result = 1;
    if (g_audio_channels[channel].master_volume >= 0x101) {
      g_audio_channels[channel].master_volume = 0x100;
    }
  }
  return result;
}

s32 AudioChannelSetPitch(u16 channel, s32 arg1) {
  s32 result = 0;

  if (channel < 4 && (g_audio_channels[channel].flags & 1)) {
    g_audio_channels[channel].channel_volume = arg1;
    if (arg1 < 0) {
      g_audio_channels[channel].channel_volume = 0;
    }
    result = 1;
    if (g_audio_channels[channel].channel_volume >= 0x101) {
      g_audio_channels[channel].channel_volume = 0x100;
    }
  }
  return result;
}

void AudioCommandSend(s32 arg0) {
  func_800F5130(2, arg0);
}

s32 AudioVoiceAllocate(s32 arg0) {
  u16 i = 0;
  s32 result = 0;

  do {
    if (!(g_audio_voices[i].flags & 1)) {
      g_audio_voices[i].sfx_id = arg0;
      g_audio_voices[i].pitch = 0;
      g_audio_voices[i].pan = 0x80;
      g_audio_voices[i].flags = 3;
      i = 4;
      result = 1;
    } else {
      i++;
    }
  } while (i < 4);

  return result;
}

s32 AudioVoiceStop(s32 arg0) {
  u16 i = 0;
  s32 result = 0;

  do {
    if ((g_audio_voices[i].flags & 1) && g_audio_voices[i].sfx_id == arg0) {
      g_audio_voices[i].flags |= 4;
      result = 1;
    }
    i++;
  } while (i < 4);

  return result;
}

s32 AudioVoiceSetVolume(s32 arg0, s32 arg1) {
  u16 i = 0;
  s32 result = 0;

  do {
    if ((g_audio_voices[i].flags & 1) && g_audio_voices[i].sfx_id == arg0) {
      g_audio_voices[i].volume = arg1;
      if (arg1 < 0) {
        g_audio_voices[i].volume = 0;
      }
      if (g_audio_voices[i].volume >= 0x101) {
        g_audio_voices[i].volume = 0x100;
      }
      result = 1;
    }
    i++;
  } while (i < 4);

  return result;
}

s32 AudioVoiceSetPitch(s32 arg0, s32 arg1) {
  u16 i = 0;
  s32 result = 0;

  do {
    if ((g_audio_voices[i].flags & 1) && g_audio_voices[i].sfx_id == arg0) {
      g_audio_voices[i].pitch = arg1;
      if ((f64)arg1 < kAudioMinPitch) {
        g_audio_voices[i].pitch = -6;
      }
      result = 1;
      if (kAudioMaxPitch < (f64)g_audio_voices[i].pitch) {
        g_audio_voices[i].pitch = 6;
      }
    }
    i++;
  } while (i < 4);

  return result;
}

s32 AudioVoiceSetPan(s32 arg0, s32 arg1) {
  u16 i = 0;
  s32 result = 0;

  do {
    if ((g_audio_voices[i].flags & 1) && g_audio_voices[i].sfx_id == arg0) {
      g_audio_voices[i].pan = arg1;
      if (arg1 < 0) {
        g_audio_voices[i].pan = 0;
      }
      result = 1;
      if (g_audio_voices[i].pan >= 0x101) {
        g_audio_voices[i].pan = 0x100;
      }
    }
    i++;
  } while (i < 4);

  return result;
}

void AudioCommandReset(void) {
  func_800F5130(1, 0);
}
