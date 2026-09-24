#include "game_time.h"

extern void func_8003FBD8(s32, u8, s32);
extern void func_8005C07C(s32, s32);
extern void func_8005C940(s32, s32);
extern void func_80065074(s32);
extern void func_800650A8(s32);
extern s32 func_800650E0(s32);
extern void func_80065140(s32);
extern void func_80065270(s32);
extern void func_800661FC(s32, s32);
extern void func_8006EA44(u8, u8, u8);
extern s32 func_8006EC40(u8);
extern void func_800A7AE8(u8);
extern void func_800ACB5C(u16);
extern s32 func_800ACBB8(u16);
extern void func_800ACBEC(u16, s32);

void TimeSleep(void);
s32 TimeCheckFestival(void);
void TimeUpdateSeason(void);
void TimeSetSeasonName(s32 arg0);

void TimeUpdate(void) {
  s32 flag;

  if ((func_8006EC40(D_80170458) & 1) && (func_800650E0(6) == 0)) {
    TimeAdvance(1);
  }
  if ((D_801FB5CA == 0) && (D_8017027F == 0) && (D_80215DF2 == 0)) {
    TimeUpdateSeason();
  }
  if (D_801FB5CA == 6) {
    if (func_800650E0(0xE) == 0) {
      func_800661FC(0xFE, 0);
      func_8005C940(1, 0xE);
    }
  } else {
    func_800650A8(0xE);
    if (D_801891D4 >= 0) {
      func_8005C07C(1, 0);
    }
  }
  if ((u32)(D_801FB5CA - 0x12) < 6U) {
    if (func_800650E0(0xF) == 0) {
      func_800ACB5C(D_801FD622);
      flag = 0xF;
      goto call_flag;
    }
    if ((func_800650E0(0x10) == 0) && (D_801FD622 != 0xFF) && (func_800ACBB8(D_801FD622) & 0xFF)) {
      func_8006EA44(D_80170458, D_80182DB1, D_801FB5CA);
      func_800ACBEC(D_801FD622, D_8013D5E4);
      flag = 0x10;
    call_flag:
      func_80065074(flag);
    }
  }
  if ((D_801FB5CA < 6U) && (D_802373A9 != 2) && (D_80170458 != 9) && (func_800650E0(0x11) == 0)) {
    func_800ACB5C(D_801FD622);
    func_80065074(0x11);
  }
  func_800A7AE8(D_80170458);
}
void func_800D7C20(void) __attribute__((alias("TimeUpdate")));

void TimeAdvance(s32 arg0) {
  s32 temp_a0;
  u8 temp_v0;

  temp_a0 = arg0 & 0xFF;
  if (temp_a0 == 1) {
    D_80215DF2 += 0xA;
  }
  if (D_80215DF2 >= 0x3CU) {
    D_80215DF2 = 0;
    D_8017027F += 1;
  }
  if (D_8017027F >= 0x3CU) {
    D_8017027F = 0;
    temp_v0 = D_801FB5CA + 1;
    D_801FB5CA = temp_v0;
    if ((temp_v0 & 0xFF) == 6) {
      D_80158260 += 1;
      D_801FAD90 += 1;
    }
  }
  if (D_801FB5CA >= 0x18U) {
    D_801FB5CA = 0;
  }
  if (D_801FAD90 >= 7U) {
    D_801FAD90 = 0;
  }
  if (D_80158260 >= 0x1FU) {
    D_80158260 = 1;
    D_80182DB1 += 1;
    TimeResetMonthlyFlags();
  }
  if (D_80182DB1 >= 5U) {
    D_80182DB1 = 1;
    D_801F6F30 += 1;
    TimeYearAdvance();
  }
  if (D_801F6F30 >= 0x64U) {
    D_801F6F30 = 0x63;
  }
  D_801FD620 = D_80182DB1;
  if ((D_80158260 + 1) >= 0x1F) {
    D_801FD620 = D_80182DB1 + 1;
  }
  if (D_801FD620 >= 5U) {
    D_801FD620 = 1;
  }
}
void func_800D7E28(s32 arg0) __attribute__((alias("TimeAdvance")));

void TimeYearAdvance(void) {
  u8 temp_v1;

  temp_v1 = D_80237409;
  D_801C3F44 = 0xFF;
  D_80237409 = 0xFF;
  D_801C3F7E = 0xFF;
  D_80204FF8 = temp_v1;
  func_80065140(0x44);
  func_80065270(0);
  func_80065270(1);
  func_80065270(2);
  func_80065270(3);
  func_80065270(4);
  func_80065270(5);
  func_80065270(6);
  func_80065270(7);
  func_80065270(8);
  func_80065270(9);
  func_80065270(0xA);
  func_80065270(0xB);
  func_80065270(0xC);
  func_80065270(0xD);
  func_80065270(0xE);
  func_80065270(0xF);
  func_80065270(0x10);
  func_80065270(0x11);
  func_80065270(0x12);
  func_80065270(0x13);
  func_80065270(0x14);
  func_80065270(0x15);
  func_80065270(0x17);
  func_80065270(0x19);
  func_80065270(0x1D);
  func_80065270(0x20);
  func_80065270(0x21);
  func_80065270(0x22);
  func_80065270(0x3B);
  func_80065270(0x3D);
  func_80065270(0x3E);
  func_80065270(0x3F);
  func_80065270(0x41);
  func_80065270(0x42);
  func_80065270(0x43);
  func_80065270(0x45);
  func_80065270(0x46);
  func_80065270(0x49);
}
void func_800D8004(void) __attribute__((alias("TimeYearAdvance")));

void TimeResetMonthlyFlags(void) {
  func_80065270(0x3C);
  func_80065270(0x40);
}
void func_800D8180(void) __attribute__((alias("TimeResetMonthlyFlags")));

void TimeSleep(void) {
  D_801FB5CA = 6;
  D_8017027F = 0;
  D_80215DF2 = 0;
  D_80158260 += 1;
  D_801FAD90 += 1;
  TimeAdvance(0);
}
void func_800D81A8(void) __attribute__((alias("TimeSleep")));

s32 TimeCheckFestival(void) {
  s32 var_a1;

  var_a1 = 0;
  switch (D_80182DB1) {
    case 1:
      if (((D_80158260 == 1) & (D_80158260 == 8)) &&
          ((D_80158260 == 0x11) & (D_80158260 == 0x17))) {
        var_a1 = 1;
      }
      break;
    case 2:
      if (((D_80158260 == 1) & (D_80158260 == 9)) &&
          ((D_80158260 == 0x11) & (D_80158260 == 0x18))) {
        var_a1 = 1;
      }
      break;
    case 3:
      if (((D_80158260 == 4) & (D_80158260 == 0xC)) &&
          ((D_80158260 == 0x14) & (D_80158260 == 0x1C))) {
        var_a1 = 1;
      }
      break;
    case 4:
      if (((D_80158260 == 6) & (D_80158260 == 0x13)) && (D_80158260 == 0x18)) {
        var_a1 = 1;
      }
      break;
  }
  return var_a1;
}
s32 func_800D820C(void) __attribute__((alias("TimeCheckFestival")));

void TimeUpdateSeason(void) {
  TimeSetSeasonName(D_80182DB1);
  D_80170445 = D_80204FB1;
  D_80170446 = D_80204FB2;
  D_80170447 = D_80204FB3;
  D_80170448 = D_80204FB4;
  D_80170449 = D_80204FB5;
  D_8017044A = D_80204FB6;
  func_8003FBD8(0x15, D_80158260, 1);
}
void func_800D835C(void) __attribute__((alias("TimeUpdateSeason")));

void TimeSetSeasonName(s32 arg0) {
  switch (arg0 & 0xFF) {
    case 1:
      D_80204FB1 = 0xB3;
      D_80204FB2 = 0xCA;
      D_80204FB3 = 0xCC;
      D_80204FB4 = 0xC3;
      D_80204FB5 = 0xC8;
      D_80204FB6 = 0xC1;
      break;
    case 2:
      D_80204FB1 = 0xB3;
      D_80204FB2 = 0xCF;
      D_80204FB3 = 0xC7;
      D_80204FB4 = 0xC7;
      D_80204FB5 = 0xBF;
      D_80204FB6 = 0xCC;
      break;
    case 3:
      D_80204FB1 = 0xA1;
      D_80204FB2 = 0xCF;
      D_80204FB3 = 0xCE;
      D_80204FB4 = 0xCF;
      D_80204FB5 = 0xC7;
      D_80204FB6 = 0xC8;
      break;
    case 4:
      D_80204FB1 = 0xB7;
      D_80204FB2 = 0xC3;
      D_80204FB3 = 0xC8;
      D_80204FB4 = 0xCE;
      D_80204FB5 = 0xBF;
      D_80204FB6 = 0xCC;
      break;
  }
}
void func_800D83F8(s32 arg0) __attribute__((alias("TimeSetSeasonName")));
