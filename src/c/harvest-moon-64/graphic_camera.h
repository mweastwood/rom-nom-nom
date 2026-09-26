#ifndef GRAPHIC_CAMERA_H
#define GRAPHIC_CAMERA_H

#include "types.h"

typedef struct {
  u8 pad0[0x10];
  s8 roll_x;
  s8 roll_y;
  s8 roll_z;
} CameraTransform;

typedef struct {
  Mtx projection_mtx;
  Mtx view_mtx;
  f32 ortho_left;
  f32 ortho_right;
  f32 ortho_bottom;
  f32 ortho_top;
  f32 ortho_near;
  f32 ortho_far;
  f32 persp_fovy;
  f32 persp_aspect;
  f32 persp_near;
  f32 persp_far;
  f32 eye_x;
  f32 eye_y;
  f32 eye_z;
  f32 at_x;
  f32 at_y;
  f32 at_z;
  f32 up_x;
  f32 up_y;
  f32 up_z;
  u8 projection_mode;
  u8 pad_cd[3];
} CameraConfig;

extern LookAt g_lookat;
extern f32 g_camera_angles[3];
extern f32 g_camera_angles_prev[3];
extern void* g_framebuffers;
extern void* g_graphic_frame_count;
extern Gfx g_init_display_list;
extern s32 g_graphic_mode_flags;
extern void* g_graphic_context;
extern void* g_depth_buffer;

void func_800FB910(void);
void func_800FBFD0(void*, s32);
void func_800FBED0(void*);
void func_800FBF10(void);
void func_800FBB6C(void);
void func_800FBD24(Gfx*, s32, s32, s32);
void func_800FBF30(void);

u32 OsVirtualToPhysical(void* addr);

void GuOrtho(Mtx* m, f32 l, f32 r, f32 b, f32 t, f32 n, f32 f, f32 scale);
void GuPerspective(Mtx* m, u16* persp_norm, f32 fovy, f32 aspect, f32 near, f32 far, f32 scale);
void GuLookAt(Mtx* m, f32 x_eye, f32 y_eye, f32 z_eye, f32 x_at, f32 y_at, f32 z_at, f32 x_up,
              f32 y_up, f32 z_up);

Gfx* GraphicClearDisplay(Gfx* glist);
Gfx* GraphicSetupCamera(Gfx* glist, CameraConfig* config, Mtx* projection_mtx);
void* CameraSetup(Gfx* glist, CameraConfig* config);
void CameraSetEye(CameraConfig* config, f32 l, f32 r, f32 b, f32 t, f32 n, f32 f);
void CameraSetAt(CameraConfig* config, f32 fovy, f32 aspect, f32 near, f32 far);
void CameraSetUp(CameraConfig* config, f32 eye_x, f32 eye_y, f32 eye_z, f32 at_x, f32 at_y,
                 f32 at_z, f32 up_x, f32 up_y, f32 up_z);
void* GraphicSetMode1(Gfx* glist, s32 mode, s32 shift, s32 len, u8 data);
void* GraphicSetMode2(Gfx* glist, s32 mode, s32 shift, s32 len, u8 data);
void CameraSetRoll(CameraTransform* transform, s8 roll_x, s8 roll_y, s8 roll_z);
void CameraSetAngles(f32 x, f32 y, f32 z);
void CameraAddAngles(f32 x, f32 y, f32 z);
void GraphicInitBuffers(void);

#endif
