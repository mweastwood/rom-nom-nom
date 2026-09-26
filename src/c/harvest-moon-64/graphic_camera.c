#include "graphic_camera.h"

Gfx* GraphicClearDisplay(Gfx* glist) {
  Gfx* g1 = glist++;
  Gfx* g2 = glist++;
  Gfx* g3 = glist++;
  Gfx* g4 = glist++;
  Gfx* g5 = glist++;
  Gfx* g6 = glist++;
  Gfx* g7 = glist++;
  Gfx* g8 = glist++;
  Gfx* g9 = glist++;
  Gfx* g10 = glist++;
  Gfx* g11 = glist++;

  g1->words.w0 = 0xFE000000;
  g1->words.w1 = g_graphic_mode_flags - 0x80000000;

  g2->words.w0 = 0xE7000000;
  g2->words.w1 = 0;

  g3->words.w0 = 0xE3000A01;
  g3->words.w1 = 0x00300000;

  g4->words.w0 = 0xFF10013F;
  g4->words.w1 = g_graphic_mode_flags - 0x80000000;

  g5->words.w0 = 0xF7000000;
  g5->words.w1 = 0xFFFCFFFC;

  g6->words.w0 = 0xF64FC3BC;
  g6->words.w1 = 0;

  g7->words.w0 = 0xE7000000;
  g7->words.w1 = 0;

  g8->words.w0 = 0xFF10013F;
  g8->words.w1 = OsVirtualToPhysical(g_depth_buffer);

  g9->words.w0 = 0xF7000000;
  g9->words.w1 = 0x00010001;

  g10->words.w0 = 0xF64FC3BC;
  g10->words.w1 = 0;

  g11->words.w0 = 0xE7000000;
  g11->words.w1 = 0;

  return glist;
}

Gfx* GraphicSetupCamera(Gfx* glist, CameraConfig* config, Mtx* projection_mtx) {
  u16 persp_norm;
  Mtx* view_mtx;
  Gfx* g;

  switch (config->projection_mode) {
    case 0:
      GuOrtho(projection_mtx, config->ortho_left, config->ortho_right, config->ortho_bottom,
              config->ortho_top, config->ortho_near, config->ortho_far, 0.9999f);
      break;
    case 1:
      g = glist++;
      GuPerspective(projection_mtx, &persp_norm, config->persp_fovy, config->persp_aspect,
                    config->persp_near, config->persp_far, 1.0f);
      g->words.w0 = 0xDB0E0000;
      g->words.w1 = persp_norm;
      break;
  }

  view_mtx = (Mtx*)((u8*)projection_mtx + 0x1C0);
  GuLookAt(view_mtx, config->eye_x, config->eye_y, config->eye_z, config->at_x, config->at_y,
           config->at_z, config->up_x, config->up_y, config->up_z);

  g = glist++;
  g->words.w0 = 0xDC08000A;
  g->words.w1 = (u32)&g_lookat;

  g = glist++;
  g->words.w0 = 0xDC08030A;
  g->words.w1 = (u32)&g_lookat + 0x10;

  g = glist++;
  g->words.w0 = 0xDA380007;
  g->words.w1 = (u32)projection_mtx;

  g = glist++;
  g->words.w0 = 0xDA380005;
  g->words.w1 = (u32)view_mtx;

  return glist;
}

void* CameraSetup(Gfx* glist, CameraConfig* config) {
  u16 persp_norm;
  Mtx* view_mtx;
  Gfx* g1;
  Gfx* g2;
  Gfx* g3;
  Gfx* g4;
  s32 cmd1;
  s32 cmd2;
  s32 cmd3;
  s32 cmd4;

  switch (config->projection_mode) {
    case 0:
      GuOrtho(&config->projection_mtx, config->ortho_left, config->ortho_right,
              config->ortho_bottom, config->ortho_top, config->ortho_near, config->ortho_far, 1.0f);
      break;
    case 1: {
      Gfx* g = glist++;
      GuPerspective(&config->projection_mtx, &persp_norm, config->persp_fovy, config->persp_aspect,
                    config->persp_near, config->persp_far, 1.0f);
      g->words.w0 = 0xDB0E0000;
      g->words.w1 = persp_norm;
      break;
    }
  }

  view_mtx = &config->view_mtx;
  GuLookAt(view_mtx, config->eye_x, config->eye_y, config->eye_z, config->at_x, config->at_y,
           config->at_z, config->up_x, config->up_y, config->up_z);

  {
    Gfx* g = glist++;
    g->words.w0 = 0xDC08000A;
    g->words.w1 = (u32)&g_lookat;
  }
  {
    Gfx* g = glist++;
    g->words.w0 = 0xDC08030A;
    g->words.w1 = (u32)&g_lookat + 0x10;
  }
  {
    Gfx* g = glist++;
    g->words.w0 = 0xDA380007;
    g->words.w1 = (u32)&config->projection_mtx;
  }
  {
    Gfx* g = glist++;
    g->words.w0 = 0xDA380005;
    g->words.w1 = (u32)view_mtx;
  }

  return glist;
}

void CameraSetEye(CameraConfig* config, f32 l, f32 r, f32 b, f32 t, f32 n, f32 f) {
  config->ortho_left = l;
  config->ortho_right = r;
  config->ortho_bottom = b;
  config->ortho_top = t;
  config->ortho_near = n;
  config->ortho_far = f;
}

void CameraSetAt(CameraConfig* config, f32 fovy, f32 aspect, f32 near, f32 far) {
  config->persp_fovy = fovy;
  config->persp_aspect = aspect;
  config->persp_near = near;
  config->persp_far = far;
}

void CameraSetUp(CameraConfig* config, f32 eye_x, f32 eye_y, f32 eye_z, f32 at_x, f32 at_y,
                 f32 at_z, f32 up_x, f32 up_y, f32 up_z) {
  config->eye_x = eye_x;
  config->eye_y = eye_y;
  config->eye_z = eye_z;
  config->at_x = at_x;
  config->at_y = at_y;
  config->at_z = at_z;
  config->up_x = up_x;
  config->up_y = up_y;
  config->up_z = up_z;
}

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

void CameraSetRoll(CameraTransform* transform, s8 roll_x, s8 roll_y, s8 roll_z) {
  transform->roll_x = roll_x;
  transform->roll_y = roll_y;
  transform->roll_z = roll_z;
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

void GraphicInitBuffers(void) {
  Gfx dl[3];
  u8 stack[2032];

  func_800FB910();
  func_800FBFD0(&g_framebuffers, 3);
  g_graphic_mode_flags = 0x80000400;
  func_800FBED0(func_800FBF10);
  g_graphic_context = &g_graphic_frame_count;
  func_800FBB6C();

  dl[0].words.w0 = 0xDE000000;
  dl[0].words.w1 = (u32)&g_init_display_list;
  dl[1].words.w0 = 0xE9000000;
  dl[1].words.w1 = 0;
  dl[2].words.w0 = 0xDF000000;
  dl[2].words.w1 = 0;

  func_800FBD24(dl, 0x18, 0, 0);
  func_800FBF30();
}
