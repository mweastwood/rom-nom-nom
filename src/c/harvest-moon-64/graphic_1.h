#ifndef GRAPHIC_1_H
#define GRAPHIC_1_H

#include "graphic_camera.h"
#include "types.h"

typedef struct {
  void* unk_0;
  void* data;
  void* unk_8;
  void* unk_c;
  s32 unk_10;
  s32 unk_14;
} TextureDescriptor;

typedef struct {
  u16 unk_0;
  u16 format;
  u8 data[4];
} TextureHeader;

typedef struct {
  f32 pos[3];
  f32 scale[3];
  f32 rot[3];
  u8 pad[0x2D0 - 0x24];
} ViewportContext;

typedef struct {
  u16 unk_0;
  u16 format;
  u16 width;
  u16 height;
  u8 data[1];
} SpriteHeader;

extern CameraConfig g_camera_config;
extern ViewportContext g_viewports[2];

void GraphicInit(void);
void GraphicSetSpriteDescriptor(TextureDescriptor* desc, SpriteHeader* header,
                                TextureHeader* tex_header);
void GraphicSetTextureFormat(TextureDescriptor* desc, TextureHeader* header);
void GraphicSetSpriteVertices(Vtx* vtx, s32 width, s32 height, s32 depth, u16 arg4, u16 flip_s,
                              u16 unused, u16 arg7, u16 arg8, u16 flags, u8 r, u8 g, u8 b, u8 a);

#endif
