#include "graphic_1.h"

void GraphicInit(void) {
  u8 i;

  GraphicInitBuffers();
  g_camera_config.projection_mode = 0;
  CameraSetEye(&g_camera_config, -160.0f, 160.0f, -120.0f, 120.0f, 0.0f, 900.0f);
  CameraSetAt(&g_camera_config, 33.0f, 4.0f / 3.0f, 10.0f, 1000.0f);
  CameraSetUp(&g_camera_config, 0.0f, 0.0f, 400.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.1f, 0.0f);

  for (i = 0; i < 2; i++) {
    g_viewports[i].pos[0] = 0.0f;
    g_viewports[i].pos[1] = 0.0f;
    g_viewports[i].pos[2] = 0.0f;
    g_viewports[i].scale[0] = 1.0f;
    g_viewports[i].scale[1] = 1.0f;
    g_viewports[i].scale[2] = 1.0f;
    g_viewports[i].rot[0] = 0.0f;
    g_viewports[i].rot[1] = 0.0f;
    g_viewports[i].rot[2] = 0.0f;
  }

  CameraSetAngles(45.0f, 315.0f, 0.0f);
}

void GraphicSetSpriteDescriptor(TextureDescriptor* desc, SpriteHeader* header,
                                TextureHeader* tex_header) {
  u8 pad[40];
  u32 v0, v1;
  s32 fmt;

  GraphicSetTextureFormat(desc, tex_header);
  desc->unk_0 = header->data;

  v0 = header->width;
  v1 = v0 & 0xFF;
  v1 <<= 8;
  v0 >>= 8;
  v0 |= v1;
  v0 &= 0xFFFF;
  desc->unk_8 = (void*)v0;

  v0 = header->height;
  v1 = v0 & 0xFF;
  v1 <<= 8;
  v0 >>= 8;
  v0 |= v1;
  v0 &= 0xFFFF;
  desc->unk_c = (void*)v0;

  fmt = (header->format >> 4) & 0xF;
  switch (fmt) {
    case 0:
      desc->unk_10 = 2;
      desc->unk_14 = 1;
      break;
    case 1:
      desc->unk_10 = 2;
      desc->unk_14 = 0;
      break;
  }
}

void GraphicSetTextureFormat(TextureDescriptor* desc, TextureHeader* header) {
  u8 pad[20];
  s32 fmt;

  desc->data = header->data;
  fmt = (header->format >> 4) & 0xF;

  switch (fmt) {
    case 0:
      desc->unk_10 = 2;
      desc->unk_14 = 1;
      break;
    case 1:
      desc->unk_10 = 2;
      desc->unk_14 = 0;
      break;
  }
}

void GraphicSetSpriteVertices(Vtx* vtx, s32 width, s32 height, s32 depth, u16 arg4, u16 flip_s,
                              u16 unused, u16 arg7, u16 arg8, u16 flags, u8 r, u8 g, u8 b, u8 a) {
  u8 axis_x;
  u8 axis_y;
  u8 axis_z;
  s16* v;
  s16 base_x;
  s16 base_y;

  switch ((flags >> 7) & 3) {
    case 2:
      axis_x = 0;
      axis_y = 1;
      axis_z = 2;
      break;
    case 3:
      axis_x = 0;
      axis_y = 2;
      axis_z = 1;
      break;
  }

  if (flip_s != 0) {
    vtx[0].v.tc[0] = width << 6;
    vtx[3].v.tc[0] = width << 6;
    v = (s16*)((u8*)vtx + (axis_x * 2));
    base_x = -((u32)(width & 0xFFFF) >> 1) - arg7;
    vtx[1].v.tc[0] = 0;
    vtx[2].v.tc[0] = 0;
    v[0] = base_x;
    v[24] = base_x;
    v[8] = v[0] + width;
    v[16] = v[8];
  } else {
    vtx[1].v.tc[0] = width << 6;
    vtx[2].v.tc[0] = width << 6;
    v = (s16*)((u8*)vtx + (axis_x * 2));
    base_x = arg7 - ((u32)(width & 0xFFFF) >> 1);
    vtx[0].v.tc[0] = 0;
    vtx[3].v.tc[0] = 0;
    v[0] = base_x;
    v[24] = base_x;
    v[8] = v[0] + width;
    v[16] = v[8];
  }

  vtx[2].v.tc[1] = depth << 6;
  vtx[3].v.tc[1] = depth << 6;
  vtx[0].v.tc[1] = 0;
  vtx[1].v.tc[1] = 0;

  switch ((flags >> 5) & 3) {
    case 2:
      v = (s16*)((u8*)vtx + (axis_y * 2));
      base_y = (((u32)(height & 0xFFFF) >> 1) - arg4) - arg8;
      v[0] = base_y;
      v[8] = base_y;
      v[16] = v[0] - depth;
      v[24] = v[16];
      break;
    case 1:
      v = (s16*)((u8*)vtx + (axis_y * 2));
      base_y = arg4 - arg8;
      v[0] = base_y;
      v[8] = base_y;
      v[16] = v[0] - depth;
      v[24] = v[16];
      break;
    case 3:
      v = (s16*)((u8*)vtx + (axis_y * 2));
      base_y = (height - arg4) - arg8;
      v[0] = base_y;
      v[8] = base_y;
      v[16] = v[0] - depth;
      v[24] = v[16];
      break;
  }

  v = (s16*)((u8*)vtx + (axis_z * 2));
  v[0] = 0;
  v[8] = 0;
  v[16] = 0;
  v[24] = 0;

  vtx[0].v.cn[0] = r;
  vtx[0].v.cn[1] = g;
  vtx[0].v.cn[2] = b;
  vtx[0].v.cn[3] = a;

  vtx[1].v.cn[0] = r;
  vtx[1].v.cn[1] = g;
  vtx[1].v.cn[2] = b;
  vtx[1].v.cn[3] = a;

  vtx[2].v.cn[0] = r;
  vtx[2].v.cn[1] = g;
  vtx[2].v.cn[2] = b;
  vtx[2].v.cn[3] = a;

  vtx[3].v.cn[0] = r;
  vtx[3].v.cn[1] = g;
  vtx[3].v.cn[2] = b;
  vtx[3].v.cn[3] = a;
}
