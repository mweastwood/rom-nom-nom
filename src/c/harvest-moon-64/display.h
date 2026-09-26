#ifndef HARVEST_MOON_64_DISPLAY_H
#define HARVEST_MOON_64_DISPLAY_H

#include "types.h"

typedef struct {
  Vec3f translation;
  Vec3f scale;
  Vec3f rotation;
  u32 pad;
  Mtx translation_matrix;
  Mtx scale_matrix;
  Mtx rotation_matrix_x;
  Mtx rotation_matrix_y;
  Mtx rotation_matrix_z;
  u32 dl;
  u16 flags;
  u16 reserved;
} RenderEntry;

typedef struct {
  Mtx matrices[2];
  Mtx translation_matrix;
  Mtx scale_matrix;
  Mtx rotation_matrix_x;
  Mtx rotation_matrix_y;
  Mtx rotation_matrix_z;
  u8 pad_1c0[0x200 - 0x1C0];
  f32 translation[3];
  f32 scale[3];
  f32 rotation[3];
} CameraContext;

extern u8 g_display_matrices[9];
extern RenderEntry g_render_entries[336];
extern u16 g_render_entry_count;
extern f32 g_camera_angles[3];
extern f32 g_camera_angles_prev[3];

void RenderInit(void);
void RenderResetCount(void);
void RenderNoOp(void);
u32 RenderAddEntry(u32 dl, u16 flags);
s32 RenderSetTranslation(s32 index, f32 x, f32 y, f32 z);
s32 RenderSetRotation(s32 index, f32 x, f32 y, f32 z);
s32 RenderSetScale(s32 index, f32 x, f32 y, f32 z);
void RenderNoOp2(void);
Gfx* RenderDrawEntries(Gfx* dl, CameraContext* camera);

void GuTranslate(Mtx* m, f32 x, f32 y, f32 z);
void GuScale(Mtx* m, f32 x, f32 y, f32 z);
void GuRotate(Mtx* m, f32 x, f32 y, f32 z);

f32 SinDegrees(f32 angle);
f32 CosDegrees(f32 angle);

#endif
