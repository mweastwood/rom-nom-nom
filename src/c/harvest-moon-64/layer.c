#include "layer.h"

#include "include_asm.h"

void LayerInit(void) {
  u16 i = 0;
  do {
    g_layer_entries[i].base_index = 0;
    g_layer_entries[i].flags = 0;
    i++;
  } while (i < 16);
}

s32 LayerSet(u16 index, s16 arg1, u32 arg2, u32 arg3, u32 arg4, u32 arg5, u32 arg6, u32 arg7,
             u32 arg8, u32 arg9, u32 arg10, u16 arg11, u8 arg12, f32 arg13, f32 arg14, f32 arg15,
             u8 arg16) {
  s32 success = 0;

  if (index < 16) {
    if (!(g_layer_entries[index].flags & 1)) {
      success = 1;
      g_layer_entries[index].param_0 = arg2;
      g_layer_entries[index].param_1 = arg3;
      g_layer_entries[index].param_2 = arg4;
      g_layer_entries[index].param_3 = arg5;
      g_layer_entries[index].param_4 = arg6;
      g_layer_entries[index].param_5 = arg7;
      g_layer_entries[index].param_6 = arg8;
      g_layer_entries[index].param_7 = arg9;
      g_layer_entries[index].param_8 = arg10;
      g_layer_entries[index].frame_index = arg11;
      g_layer_entries[index].alpha = arg12;
      g_layer_entries[index].scale_x = arg13;
      g_layer_entries[index].scale_y = arg14;
      g_layer_entries[index].scale_z = arg15;
      g_layer_entries[index].blend_mode = arg16;
      g_layer_entries[index].base_index = arg1;
      g_layer_entries[index].flags = 1;
    }
  }

  return success;
}

void func_8002B138(u16, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32);
s32 func_800461D8(u16, u8, u8, u8, u8);

s32 LayerActivate(u16 index, u32 arg1, s8 arg2, u16 arg3) {
  s32 success = 0;
  u8 count;

  if (index < 16) {
    if (g_layer_entries[index].flags & 1) {
      g_layer_entries[index].offset_index = arg2;
      count = g_layer_entries[index].offset_index;
      g_layer_entries[index].callback = arg1;
      g_layer_entries[index].flags |= 2;
      do {
        func_8002B138(g_layer_entries[index].base_index + count, g_layer_entries[index].param_0,
                      g_layer_entries[index].param_1, g_layer_entries[index].param_2,
                      g_layer_entries[index].param_3, 0, 0, g_layer_entries[index].param_4, 0,
                      g_layer_entries[index].param_5, g_layer_entries[index].param_6,
                      g_layer_entries[index].param_7, g_layer_entries[index].param_8, 0, 0);
      } while (count--);
      func_800461D8(index, 0xFF, 0xFF, 0xFF, 0xFF);

      success = 1;
      if (arg3 == 3) {
        g_layer_entries[index].flags |= 4;
      }
    }
  }

  return success;
}

INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80046120);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_800461D8);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_800462B4);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004635C);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004644C);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80046504);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80046650);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_800467F8);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80046860);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_800469A8);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80046A58);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80046AB0);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80046BB8);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80046BF8);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80046C48);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80046C98);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80046CF4);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80046D50);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80046D78);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_800471B0);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_800475B4);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_800475F8);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80047640);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80047E34);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80047E94);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80047F20);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80047F90);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80048124);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80048258);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_800482B8);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_800482F8);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004835C);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004838C);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80048430);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_800484D4);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80048578);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_800485EC);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80048694);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_800486F4);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_800488CC);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80048B08);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80048B90);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80048BEC);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80048C48);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80048CA4);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80048DA8);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80048E98);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80048F88);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80048FF4);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80049064);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004910C);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80049228);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80049350);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_800495F0);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004969C);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80049708);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80049828);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_800498B0);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80049950);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80049A28);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80049AC4);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80049D64);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80049DF4);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80049E84);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80049F40);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_80049FA0);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004A000);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004A0A8);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004A0F4);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004A140);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004A1DC);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004A234);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004A320);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004A38C);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004A400);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004A47C);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004A7A4);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004AB04);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004ABA8);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004AC4C);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004ACE4);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004AD44);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004B050);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004B09C);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004B0E8);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004B2FC);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004B410);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004B498);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004B538);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004B5F0);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004B700);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004B7B8);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004B920);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004B9A0);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004BA34);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004BB84);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004BD00);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004BD6C);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004BE14);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004BE88);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004BFC4);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004C048);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004C0D0);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004C148);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004C258);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004C34C);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004C3D0);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004C454);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004C4E0);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004C56C);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004C5D8);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004C6CC);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004C770);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004C7E4);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004C870);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004C904);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004C994);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004CA80);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004CB1C);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004CB88);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004CC3C);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004CCF0);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004CDA0);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004CF68);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004D35C);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004D380);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004D3A4);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004D3C8);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004D3EC);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004D410);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004D434);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004D458);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004D47C);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004D788);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004D87C);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004D8B4);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004D904);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004D954);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004D9AC);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004DA48);
INCLUDE_ASM("asm/harvest-moon-64/nonmatchings/layer", func_8004DAF4);
