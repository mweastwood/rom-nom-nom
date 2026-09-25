#ifndef GRAPHIC_CAMERA_H
#define GRAPHIC_CAMERA_H

#include "types.h"

typedef struct {
  u8 pad0[0x10];
  s8 roll_x;
  s8 roll_y;
  s8 roll_z;
  u8 pad13[0x6D];
  s32 eye_mode_x;
  s32 eye_mode_y;
  s32 eye_mode_z;
  f32 eye_x;
  f32 eye_y;
  f32 eye_z;
  s32 at_mode_x;
  s32 at_mode_y;
  s32 at_mode_z;
  f32 at_offset;
  s32 up_mode_x;
  s32 up_mode_y;
  s32 up_mode_z;
  f32 up_param[6];
} CameraConfig;

extern f32 g_camera_angles[3];
extern f32 g_camera_angles_prev[3];

void CameraSetEye(CameraConfig* config, s32 mode_x, s32 mode_y, s32 mode_z, f32 eye_x, f32 eye_y,
                  f32 eye_z);
void CameraSetAt(CameraConfig* config, s32 mode_x, s32 mode_y, s32 mode_z, f32 at_offset);
void CameraSetUp(CameraConfig* config, s32 mode_x, s32 mode_y, s32 mode_z, f32 p0, f32 p1, f32 p2,
                 f32 p3, f32 p4, f32 p5);
void* GraphicSetMode1(Gfx* glist, s32 mode, s32 shift, s32 len, u8 data);
void* GraphicSetMode2(Gfx* glist, s32 mode, s32 shift, s32 len, u8 data);
void CameraSetRoll(CameraConfig* config, s8 roll_x, s8 roll_y, s8 roll_z);
void CameraSetAngles(f32 x, f32 y, f32 z);
void CameraAddAngles(f32 x, f32 y, f32 z);

#endif
