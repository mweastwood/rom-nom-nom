#include "audio.h"

extern void func_800F2500(s32, s32);
extern void func_800266C0(void*, s32, s32, s32, s32);
extern void func_800267A4(void*, s16, s16);
extern void func_800F5318(s32, s32);
extern void func_800F5130(s32, s32);

void AudioSetMasterVolume(s32 arg0, s32 arg1) {
  func_800F2500(arg0, arg1 - arg0);
}
void func_8003D250(s32 arg0, s32 arg1) __attribute__((alias("AudioSetMasterVolume")));

s32 AudioChannelInit(u16 channel, s32 arg1, s32 arg2) {
  s32 result = 0;

  if (channel < 4) {
    if (!(D_801FB5D8[channel].flags & 1)) {
      func_800266C0(&D_801FB5D8[channel].unk_08, 0, 0, 0, 0);
      result = 1;
      D_801FB5D8[channel].unk_1C = 0x80;
      D_801FB5D8[channel].unk_20 = 0x80;
      D_801FB5D8[channel].unk_00 = arg1;
      D_801FB5D8[channel].unk_04 = arg2;
      D_801FB5D8[channel].flags = 3;
    }
  }
  return result;
}
s32 func_8003D270(u16 channel, s32 arg1, s32 arg2) __attribute__((alias("AudioChannelInit")));

s32 AudioChannelSetSpeed(u16 channel, s32 arg1) {
  s32 result = 0;

  if (channel < 4 && (D_801FB5D8[channel].flags & 1)) {
    result = 1;
    D_801FB5D8[channel].unk_24 = arg1;
    D_801FB5D8[channel].flags |= 4;
  }
  return result;
}
s32 func_8003D350(u16 channel, s32 arg1) __attribute__((alias("AudioChannelSetSpeed")));

s32 AudioChannelStop(u16 channel) {
  s32 result = 0;

  if (channel < 4) {
    if (D_801FB5D8[channel].flags & 1) {
      func_800F5318(D_801FB5D8[channel].unk_18, 0);
      result = 1;
      D_801FB5D8[channel].flags = 0;
    }
  }
  return result;
}
s32 func_8003D3C0(u16 channel) __attribute__((alias("AudioChannelStop")));

s32 AudioChannelSetPan(u16 channel, s32 arg1, s16 arg2) {
  s32 result = 0;

  if (channel < 4 && (D_801FB5D8[channel].flags & 1)) {
    if (arg1 < 0) {
      arg1 = 0;
    }
    if (arg1 >= 0x101) {
      arg1 = 0x100;
    }
    D_801FB5D8[channel].unk_14 = arg1;
    func_800267A4(&D_801FB5D8[channel].unk_08, arg2, (s16)arg1);
    result = 1;
  }
  return result;
}
s32 func_8003D444(u16 channel, s32 arg1, s16 arg2) __attribute__((alias("AudioChannelSetPan")));

s32 AudioChannelSetVolume(u16 channel, s32 arg1) {
  s32 result = 0;

  if (channel < 4 && (D_801FB5D8[channel].flags & 1)) {
    D_801FB5D8[channel].unk_1C = arg1;
    if (arg1 < 0) {
      D_801FB5D8[channel].unk_1C = 0;
    }
    result = 1;
    if (D_801FB5D8[channel].unk_1C >= 0x101) {
      D_801FB5D8[channel].unk_1C = 0x100;
    }
  }
  return result;
}
s32 func_8003D4E4(u16 channel, s32 arg1) __attribute__((alias("AudioChannelSetVolume")));

s32 AudioChannelSetPitch(u16 channel, s32 arg1) {
  s32 result = 0;

  if (channel < 4 && (D_801FB5D8[channel].flags & 1)) {
    D_801FB5D8[channel].unk_20 = arg1;
    if (arg1 < 0) {
      D_801FB5D8[channel].unk_20 = 0;
    }
    result = 1;
    if (D_801FB5D8[channel].unk_20 >= 0x101) {
      D_801FB5D8[channel].unk_20 = 0x100;
    }
  }
  return result;
}
s32 func_8003D570(u16 channel, s32 arg1) __attribute__((alias("AudioChannelSetPitch")));

void AudioCommandSend(s32 arg0) {
  func_800F5130(2, arg0);
}
void func_8003D5FC(s32 arg0) __attribute__((alias("AudioCommandSend")));

s32 AudioVoiceAllocate(s32 arg0) {
  u16 i = 0;
  s32 result = 0;

  do {
    if (!(D_801FB690[i].flags & 1)) {
      D_801FB690[i].unk_00 = arg0;
      D_801FB690[i].unk_08 = 0;
      D_801FB690[i].unk_0C = 0x80;
      D_801FB690[i].flags = 3;
      i = 4;
      result = 1;
    } else {
      i++;
    }
  } while (i < 4);

  return result;
}
s32 func_8003D620(s32 arg0) __attribute__((alias("AudioVoiceAllocate")));

s32 AudioVoiceStop(s32 arg0) {
  u16 i = 0;
  s32 result = 0;

  do {
    if ((D_801FB690[i].flags & 1) && D_801FB690[i].unk_00 == arg0) {
      D_801FB690[i].flags |= 4;
      result = 1;
    }
    i++;
  } while (i < 4);

  return result;
}
s32 func_8003D6A8(s32 arg0) __attribute__((alias("AudioVoiceStop")));

s32 AudioVoiceSetVolume(s32 arg0, s32 arg1) {
  u16 i = 0;
  s32 result = 0;

  do {
    if ((D_801FB690[i].flags & 1) && D_801FB690[i].unk_00 == arg0) {
      D_801FB690[i].unk_10 = arg1;
      if (arg1 < 0) {
        D_801FB690[i].unk_10 = 0;
      }
      if (D_801FB690[i].unk_10 >= 0x101) {
        D_801FB690[i].unk_10 = 0x100;
      }
      result = 1;
    }
    i++;
  } while (i < 4);

  return result;
}
s32 func_8003D718(s32 arg0, s32 arg1) __attribute__((alias("AudioVoiceSetVolume")));
