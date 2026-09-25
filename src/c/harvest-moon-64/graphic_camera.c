#include "graphic_camera.h"

void CameraSetEye(CameraConfig* config, s32 mode_x, s32 mode_y, s32 mode_z, f32 eye_x, f32 eye_y,
                  f32 eye_z) {
  config->eye_mode_x = mode_x;
  config->eye_mode_y = mode_y;
  config->eye_mode_z = mode_z;
  config->eye_x = eye_x;
  config->eye_y = eye_y;
  config->eye_z = eye_z;
}

void CameraSetAt(CameraConfig* config, s32 mode_x, s32 mode_y, s32 mode_z, f32 at_offset) {
  config->at_mode_x = mode_x;
  config->at_mode_y = mode_y;
  config->at_mode_z = mode_z;
  config->at_offset = at_offset;
}

void CameraSetUp(CameraConfig* config, s32 mode_x, s32 mode_y, s32 mode_z, f32 p0, f32 p1, f32 p2,
                 f32 p3, f32 p4, f32 p5) {
  config->up_mode_x = mode_x;
  config->up_mode_y = mode_y;
  config->up_mode_z = mode_z;
  config->up_param[0] = p0;
  config->up_param[1] = p1;
  config->up_param[2] = p2;
  config->up_param[3] = p3;
  config->up_param[4] = p4;
  config->up_param[5] = p5;
}

typedef struct {
  u32 w0;
  u32 w1;
} DisplayWord;

void* GraphicSetMode1(Gfx* glist, s32 mode, s32 shift, s32 len, u8 data) {
  Gfx* g = glist++;
  u32 val;

  g->words.w0 = 0xDB0A0018;
  val = (shift << 24) + ((len & 0xFF) << 16) + (data << 8);
  g->words.w1 = val;
  glist->words.w0 = 0xDB0A001C;
  glist->words.w1 = val;
  return glist + 1;
}

void* GraphicSetMode2(Gfx* glist, s32 mode, s32 shift, s32 len, u8 data) {
  Gfx* g = glist++;
  u32 val;

  g->words.w0 = 0xDB0A0000;
  val = (shift << 24) + ((len & 0xFF) << 16) + (data << 8);
  g->words.w1 = val;
  glist->words.w0 = 0xDB0A0004;
  glist->words.w1 = val;
  return glist + 1;
}

void CameraSetRoll(CameraConfig* config, s8 roll_x, s8 roll_y, s8 roll_z) {
  config->roll_x = roll_x;
  config->roll_y = roll_y;
  config->roll_z = roll_z;
}

void CameraSetAngles(f32 x, f32 y, f32 z) {
  g_camera_angles[0] = x;
  g_camera_angles[1] = y;
  g_camera_angles[2] = z;
  g_camera_angles_prev[0] = x;
  g_camera_angles_prev[1] = y;
  g_camera_angles_prev[2] = z;
}

void CameraAddAngles(f32 x, f32 y, f32 z) {
  g_camera_angles[0] += x;
  g_camera_angles[1] += y;
  g_camera_angles[2] += z;
}
