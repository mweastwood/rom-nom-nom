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
    D_801FB5D8[i].flags = 0;
    D_801FB5D8[i].unk_18 = 0;
    D_801FB5D8[i].pad_28 = 0;
    D_801FB5D8[i].unk_1C = 0;
    D_801FB5D8[i].unk_20 = 0;
    D_801FB690[i].unk_10 = 0;
    D_801FB5D8[i].unk_24 = 0;
    func_800266C0(&D_801FB5D8[i].unk_08, 0, 0, 0, 0);
    i++;
  } while (i < 4);

  j = 0;
  do {
    D_801FB690[j].flags = 0;
    D_801FB690[j].unk_00 = 0;
    D_801FB690[j].unk_04 = 0;
    D_801FB690[j].pad_14 = 0;
    D_801FB690[j].unk_08 = 0;
    D_801FB690[j].unk_0C = 0;
    D_801FB690[j].unk_10 = 0;
    j++;
  } while (j < 4);

  func_800F2470(arg0);
  func_800F4B68(2, 0x7FFF);
  func_800F4B68(1, 0x7FFF);
}
void func_8003CDC0(s32 arg0) __attribute__((alias("AudioInit")));

void AudioUpdate(void) {
  u16 j;
  u16 i = 0;

  do {
    if (D_801FB5D8[i].flags & 1) {
      if (D_801FB5D8[i].flags & 2) {
        u8 channel = (u8)i;
        func_800F268C(channel, D_801FB5D8[i].unk_00, D_801FB5D8[i].unk_04 - D_801FB5D8[i].unk_00);
        D_801FB5D8[i].unk_18 = func_800F2704(channel);
        D_801FB5D8[i].flags &= ~2;
      }
      if (D_801FB5D8[i].flags & 4) {
        func_800F5318(D_801FB5D8[i].unk_18, D_801FB5D8[i].unk_24);
        D_801FB5D8[i].flags &= ~4;
      }
      func_800266F8(&D_801FB5D8[i].unk_08);
      func_800F54C0(D_801FB5D8[i].unk_18, D_801FB5D8[i].unk_10);
      D_801FB5D8[i].pad_28 = func_800F5404(D_801FB5D8[i].unk_18);
      if (!D_801FB5D8[i].pad_28) {
        D_801FB5D8[i].flags = 0;
        func_800F5318(D_801FB5D8[i].unk_18, 1);
      }
    }
    i++;
  } while (i < 4);

  j = 0;
  do {
    if (D_801FB690[j].flags & 1) {
      if (D_801FB690[j].flags & 2) {
        D_801FB690[j].unk_04 = func_800F2740(D_801FB690[j].unk_00);
        D_801FB690[j].flags &= ~2;
      }
      if (D_801FB690[j].flags & 4) {
        func_800F5318(D_801FB690[j].unk_04, 0);
        D_801FB690[j].flags &= ~4;
      }
      {
        f32 freq = (f32)D_801FB690[j].unk_08;
        __asm__("nop" : : "r"(freq));
        func_800F5664(D_801FB690[j].unk_04, freq);
      }
      func_800F558C(D_801FB690[j].unk_04, D_801FB690[j].unk_0C);
      func_800F54C0(D_801FB690[j].unk_04, D_801FB690[j].unk_10);
      D_801FB690[j].pad_14 = func_800F5404(D_801FB690[j].unk_04);
      if (!D_801FB690[j].pad_14) {
        D_801FB690[j].flags = 0;
      }
    }
    j++;
  } while (j < 4);
}
void func_8003CF38(void) __attribute__((alias("AudioUpdate")));

extern void func_800F2500(s32, s32);
extern void func_800267A4(void*, s16, s16);
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

INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/audio", func_8003D7C0);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/audio", func_8003D8A0);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/audio", func_8003D948);
